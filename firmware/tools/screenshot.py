#!/usr/bin/env python3
"""
Grab the 128x64 frame buffer off the running device over SWD and save it as a
PNG, without halting the firmware.

    tools/screenshot.py [out.png] [--scale 4] [--elf build/PixlAnalyzerOLED.out]
"""
import argparse, os, struct, subprocess, sys, tempfile, zlib

ap = argparse.ArgumentParser()
ap.add_argument("out", nargs="?", default="screen.png")
ap.add_argument("--scale", type=int, default=4)
ap.add_argument("--elf", default=os.path.join(os.path.dirname(__file__), "..", "build", "PixlAnalyzerOLED.out"))
ap.add_argument("--interface", default=os.environ.get("OPENOCD_IF", "interface/jlink.cfg"))
args = ap.parse_args()

nm = subprocess.run(["arm-none-eabi-nm", args.elf], capture_output=True, text=True, check=True).stdout
addr = next(int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(" g_frame_buffer"))

with tempfile.TemporaryDirectory() as tmp:
    dump = os.path.join(tmp, "fb.bin")
    subprocess.run(["openocd", "-f", args.interface, "-c", "transport select swd", "-f", "target/nrf52.cfg",
                    "-c", "init", "-c", f"dump_image {dump} {addr:#x} 1024", "-c", "shutdown"],
                   capture_output=True, check=True)
    fb = open(dump, "rb").read()

W, H, S = 128, 64, args.scale
rows = []
for y in range(H * S):
    py = y // S
    row = bytearray([0])
    for x in range(W * S):
        on = (fb[(py // 8) * W + x // S] >> (py % 8)) & 1
        row.append(0xFF if on else 0x10)
    rows.append(bytes(row))

def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W * S, H * S, 8, 0, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
open(args.out, "wb").write(png)
print(args.out)
