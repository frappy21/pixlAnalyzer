#!/usr/bin/env python3
"""
Live view of the running analyzer on a PC, over SWD, without halting it.

Starts OpenOCD, reads the scanner results (g_scan, g_floor, the channel list)
and the frame buffer through OpenOCD's Tcl port, and serves a page with a live
spectrum, a waterfall and a mirror of the device screen:

    tools/liveview.py [--port 8024] [--elf build/PixlAnalyzerOLED.out]
    then open http://localhost:8024

Needs only python3, openocd and arm-none-eabi-nm. The probe defaults to a
J-Link; OPENOCD_IF=interface/cmsis-dap.cfg for another one.
"""
import argparse
import http.server
import json
import os
import socket
import subprocess
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8024)
ap.add_argument("--elf", default=os.path.join(os.path.dirname(__file__), "..", "build", "PixlAnalyzerOLED.out"))
ap.add_argument("--tcl-port", type=int, default=6666)
args = ap.parse_args()


def symbols(elf):
    """name -> (addr, size); file-local statics are keyed as 'file.c:name'."""
    out = subprocess.run(["arm-none-eabi-nm", "-l", "-S", elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        addr, size, name = int(parts[0], 16), int(parts[1], 16), parts[3]
        syms[name] = (addr, size)
        if len(parts) > 4:
            syms[os.path.basename(parts[4].rsplit(":", 1)[0]) + ":" + name] = (addr, size)
    return syms


SYMS = symbols(args.elf)
G_SCAN = SYMS["g_scan"]
G_FLOOR = SYMS["g_floor"]
FB = SYMS["g_frame_buffer"]
COUNT = SYMS["scanner.c:m_count"]
MHZ = SYMS["scanner.c:m_mhz"]
STRIDE = G_SCAN[1] // G_FLOOR[1]  # sizeof(chan_result_t)


class Ocd:
    """Minimal client for OpenOCD's Tcl RPC (commands end with 0x1a)."""

    def __init__(self, port):
        self.proc = subprocess.Popen(
            ["openocd", "-f", os.environ.get("OPENOCD_IF", "interface/jlink.cfg"), "-c", "transport select swd",
             "-c", "adapter speed 4000", "-f", "target/nrf52.cfg", "-c", f"tcl_port {port}", "-c", "gdb_port disabled",
             "-c", "telnet_port disabled", "-c", "init"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port))
                return
            except OSError:
                time.sleep(0.1)
        raise SystemExit("openocd did not come up - is the probe connected and the target powered?")

    def cmd(self, text):
        self.sock.sendall(text.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("openocd closed the connection")
            buf += chunk
        return buf[:-1].decode()

    def read(self, addr, size):
        words = self.cmd(f"read_memory {addr:#x} 8 {size}").split()
        return bytes(int(w, 16) for w in words)


ocd = Ocd(args.tcl_port)
lock = threading.Lock()


def snapshot():
    with lock:
        count = ocd.read(*COUNT)[0]
        mhz_raw = ocd.read(MHZ[0], count * 2)
        scan = ocd.read(G_SCAN[0], count * STRIDE)
        floor = ocd.read(G_FLOOR[0], count)
        fb = ocd.read(*FB)
    mhz = [mhz_raw[2 * i] | mhz_raw[2 * i + 1] << 8 for i in range(count)]
    return {
        "mhz": mhz,
        "peak": [-scan[i * STRIDE] if scan[i * STRIDE] != 127 else None for i in range(count)],
        "weak": [-scan[i * STRIDE + 1] if scan[i * STRIDE + 1] != 127 else None for i in range(count)],
        "busy": [scan[i * STRIDE + 2] for i in range(count)],
        "floor": [-f for f in floor],
        "fb": fb.hex(),
    }


PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>Pixl Live View</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#101214;--fg:#e8e8e8;--dim:#7a8088;--acc:#4fc3f7;--peak:#ffb74d;--grid:#2a2e33}
body{margin:0;background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif}
main{max-width:1100px;margin:0 auto;padding:16px}
h1{font-size:18px;margin:0 0 8px}
.row{display:flex;gap:16px;flex-wrap:wrap;align-items:flex-start}
canvas{background:#000;border:1px solid var(--grid);width:100%;image-rendering:pixelated}
#spec,#wf{max-width:820px}
#screen{width:256px;max-width:100%}
.info{color:var(--dim);font-variant-numeric:tabular-nums;min-height:1.4em}
button{background:var(--grid);color:var(--fg);border:0;padding:6px 10px;border-radius:4px;cursor:pointer}
</style></head><body><main>
<h1>Pixl Live View</h1>
<div class="info" id="info">connecting...</div>
<div class="row">
 <div style="flex:1;min-width:280px">
  <canvas id="spec" width="820" height="260"></canvas>
  <canvas id="wf" width="820" height="200"></canvas>
  <div><button id="clr">Clear max hold</button> <span class="info" id="hover"></span></div>
 </div>
 <div><div class="info">Device screen</div><canvas id="screen" width="128" height="64"></canvas></div>
</div>
</main><script>
const spec=document.getElementById('spec'),sc=spec.getContext('2d');
const wf=document.getElementById('wf'),wc=wf.getContext('2d');
const scr=document.getElementById('screen'),xc=scr.getContext('2d');
const TOP=-30,BOT=-110;let maxHold=null,last=null,frames=0,t0=performance.now();
document.getElementById('clr').onclick=()=>maxHold=null;
const y=v=>spec.height-(v-BOT)/(TOP-BOT)*spec.height;
function heat(v){const t=Math.max(0,Math.min(1,(v-BOT+5)/(TOP-BOT)));
 return `hsl(${240-240*t},90%,${10+50*t}%)`}
function draw(d){const n=d.mhz.length;if(!n)return;last=d;
 if(!maxHold||maxHold.length!=n)maxHold=d.peak.map(v=>v??BOT);
 d.peak.forEach((v,i)=>{if(v!=null&&v>maxHold[i])maxHold[i]=v});
 const w=spec.width/n;sc.fillStyle='#000';sc.fillRect(0,0,spec.width,spec.height);
 sc.strokeStyle='#2a2e33';sc.fillStyle='#7a8088';sc.font='11px system-ui';
 for(let db=-40;db>=-110;db-=10){sc.beginPath();sc.moveTo(0,y(db));sc.lineTo(spec.width,y(db));sc.stroke();sc.fillText(db,2,y(db)-2)}
 for(let i=0;i<n;i++)if(d.mhz[i]%10==0){sc.fillText(d.mhz[i],i*w+2,spec.height-4)}
 sc.fillStyle='#4fc3f7';d.peak.forEach((v,i)=>{if(v!=null)sc.fillRect(i*w,y(v),Math.max(1,w-1),spec.height-y(v))});
 sc.strokeStyle='#ffb74d';sc.beginPath();maxHold.forEach((v,i)=>{i?sc.lineTo(i*w+w/2,y(v)):sc.moveTo(w/2,y(v))});sc.stroke();
 sc.strokeStyle='#8bc34a';sc.setLineDash([4,4]);sc.beginPath();d.floor.forEach((v,i)=>{i?sc.lineTo(i*w+w/2,y(v)):sc.moveTo(w/2,y(v))});sc.stroke();sc.setLineDash([]);
 wc.drawImage(wf,0,0,wf.width,wf.height-2,0,2,wf.width,wf.height-2);
 d.peak.forEach((v,i)=>{wc.fillStyle=heat(v??BOT);wc.fillRect(i*w,0,Math.ceil(w),2)});
 const fb=d.fb,img=xc.createImageData(128,64);
 for(let yy=0;yy<64;yy++)for(let x=0;x<128;x++){const b=parseInt(fb.substr(((yy>>3)*128+x)*2,2),16);
  const on=(b>>(yy&7))&1,o=(yy*128+x)*4;img.data[o]=img.data[o+1]=img.data[o+2]=on?235:20;img.data[o+3]=255}
 xc.putImageData(img,0,0);
 frames++;const dt=(performance.now()-t0)/1000;
 document.getElementById('info').textContent=`${n} channels, ${d.mhz[0]}-${d.mhz[n-1]} MHz, ${(frames/dt).toFixed(1)} updates/s`}
spec.onmousemove=e=>{if(!last)return;const r=spec.getBoundingClientRect();
 const i=Math.floor((e.clientX-r.left)/r.width*last.mhz.length);if(i<0||i>=last.mhz.length)return;
 document.getElementById('hover').textContent=`${last.mhz[i]} MHz  peak ${last.peak[i]} dBm  max ${maxHold[i]} dBm  floor ${last.floor[i]} dBm  busy ${Math.round(last.busy[i]/2.55)}%`};
async function loop(){try{const r=await fetch('/data');draw(await r.json())}catch(e){document.getElementById('info').textContent='no data: '+e}
 setTimeout(loop,50)}
loop();
</script></body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/data":
            try:
                body = json.dumps(snapshot()).encode()
            except Exception as e:  # probe unplugged, target asleep
                self.send_error(503, str(e))
                return
            ctype = "application/json"
        else:
            body, ctype = PAGE.encode(), "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


print(f"live view on http://localhost:{args.port}  (Ctrl+C to stop)")
try:
    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
except KeyboardInterrupt:
    pass
finally:
    ocd.proc.terminate()
