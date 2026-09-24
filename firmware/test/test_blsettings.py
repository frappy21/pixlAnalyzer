#!/usr/bin/env python3
"""
Checks the generated bootloader settings page the way the bootloader checks it:
parse the Intel HEX back, recompute both CRCs, and confirm the page really
describes the application binary next to it.

A wrong page here is not a cosmetic bug: the device would boot into DFU mode
instead of the firmware, with no obvious reason why.
"""

import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import mkblsettings as m  # noqa: E402

failures = 0


def check(what, got, want):
    global failures
    if got != want:
        print(f"  FAIL {what:<44} got {got!r} want {want!r}")
        failures += 1
    else:
        print(f"  ok   {what:<44}")


def parse_ihex(text):
    """Returns {address: byte} for the data records."""
    out = {}
    upper = 0
    for line in text.splitlines():
        if not line.startswith(":"):
            continue
        raw = bytes.fromhex(line[1:])
        count, hi, lo, rtype = raw[0], raw[1], raw[2], raw[3]
        data = raw[4:4 + count]
        if (sum(raw) & 0xFF) != 0:
            raise ValueError("bad checksum in " + line)
        if rtype == 0x04:
            upper = (data[0] << 8 | data[1]) << 16
        elif rtype == 0x00:
            base = upper + (hi << 8 | lo)
            for i, b in enumerate(data):
                out[base + i] = b
    return out


def main():
    app = bytes(range(256)) * 97  # 24832 bytes, stands in for a real image
    with tempfile.TemporaryDirectory() as tmp:
        app_path = Path(tmp) / "app.bin"
        hex_path = Path(tmp) / "settings.hex"
        app_path.write_bytes(app)

        subprocess.run([sys.executable, str(Path(m.__file__)), str(app_path), str(hex_path)],
                       check=True, capture_output=True)

        cells = parse_ihex(hex_path.read_text())

        addrs = sorted(cells)
        check("page starts at the settings address", addrs[0], m.SETTINGS_ADDR)
        check("page covers the whole struct", len(cells), m.SETTINGS_SIZE)
        check("page is contiguous", addrs[-1], m.SETTINGS_ADDR + m.SETTINGS_SIZE - 1)

        page = bytes(cells[a] for a in addrs)

        # The two checks the bootloader performs
        check("settings crc matches its own payload",
              m.get32(page, m.OFF_CRC),
              zlib.crc32(page[4:4 + m.SETTINGS_CRC_LEN]))
        check("boot validation crc matches",
              m.get32(page, m.OFF_BOOT_VALIDATION_CRC),
              zlib.crc32(page[m.OFF_BV_SOFTDEVICE:m.OFF_BV_SOFTDEVICE + m.BOOT_VALIDATION_CRC_LEN]))

        # What makes the bootloader accept and start the app
        check("bank 0 is marked as a valid app", m.get32(page, m.OFF_BANK0 + 8), m.BANK_VALID_APP)
        check("bank 0 size is the image size", m.get32(page, m.OFF_BANK0), len(app))
        check("bank 0 crc is the image crc", m.get32(page, m.OFF_BANK0 + 4), zlib.crc32(app))
        check("app boot validation is a crc check", m.get32(page, m.OFF_BV_APP), m.VALIDATE_CRC)
        check("app boot validation carries the image crc",
              m.get32(page, m.OFF_BV_APP + 4), zlib.crc32(app))
        check("softdevice validation is skipped", m.get32(page, m.OFF_BV_SOFTDEVICE),
              m.NO_VALIDATION)
        check("current bank is bank 0", m.get32(page, m.OFF_BANK_CURRENT), m.CURRENT_BANK_0)
        check("settings version is 2", m.get32(page, m.OFF_VERSION), 2)

        # A different image must not validate against this page
        other = zlib.crc32(app + b"\x00")
        check("a modified image would fail the crc check",
              m.get32(page, m.OFF_BV_APP + 4) != other, True)

    print(f"\n{'FAILED' if failures else 'ALL PASS'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
