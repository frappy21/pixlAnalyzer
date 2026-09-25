#!/usr/bin/env python3
"""
Read the scanner's per-channel results off the running device over SWD and
print peak / weakest / busy / noise floor statistics. For comparing radio
changes on real hardware.

    tools/rssidump.py [--elf build/PixlAnalyzerOLED.out] [-n 5]
"""
import argparse, os, statistics, subprocess, tempfile, time

ap = argparse.ArgumentParser()
ap.add_argument("--elf", default=os.path.join(os.path.dirname(__file__), "..", "build", "PixlAnalyzerOLED.out"))
ap.add_argument("-n", type=int, default=5, help="snapshots, 300 ms apart")
ap.add_argument("--channels", type=int, default=84)
args = ap.parse_args()

syms = {}
for l in subprocess.run(["arm-none-eabi-nm", "-S", args.elf], capture_output=True, text=True, check=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4:
        syms[p[3]] = (int(p[0], 16), int(p[1], 16))
scan_addr, scan_size = syms["g_scan"]
floor_addr, floor_size = syms["g_floor"]
stride = scan_size // floor_size  # sizeof(chan_result_t)

peaks, weaks, busys, floors = [], [], [], []
with tempfile.TemporaryDirectory() as tmp:
    cmds = ["init"]
    for i in range(args.n):
        cmds += [f"dump_image {tmp}/s{i}.bin {scan_addr:#x} {scan_size}",
                 f"dump_image {tmp}/f{i}.bin {floor_addr:#x} {floor_size}", "sleep 300"]
    cmds.append("shutdown")
    cl = ["openocd", "-f", os.environ.get("OPENOCD_IF", "interface/jlink.cfg"), "-c", "transport select swd",
          "-f", "target/nrf52.cfg"]
    for c in cmds:
        cl += ["-c", c]
    subprocess.run(cl, capture_output=True, check=True)
    for i in range(args.n):
        s = open(f"{tmp}/s{i}.bin", "rb").read()
        f = open(f"{tmp}/f{i}.bin", "rb").read()
        for ch in range(args.channels):
            peak, weak, busy = s[ch * stride], s[ch * stride + 1], s[ch * stride + 2]
            if peak == 127:
                continue
            peaks.append(-peak); weaks.append(-weak); busys.append(busy); floors.append(-f[ch])

def st(name, v):
    v = sorted(v)
    print(f"{name:8} min {v[0]:5} p10 {v[len(v)//10]:5} median {statistics.median(v):6} p90 {v[len(v)*9//10]:5} max {v[-1]:5}")

print(f"{len(peaks)} channel samples (sizeof chan_result_t = {stride})")
st("peak", peaks); st("weakest", weaks); st("floor", floors); st("busy", busys)
st("spread", [p - w for p, w in zip(peaks, weaks)])
st("pk-floor", [p - f for p, f in zip(peaks, floors)])
