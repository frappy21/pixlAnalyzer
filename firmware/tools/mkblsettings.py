#!/usr/bin/env python3
"""
Build the bootloader settings page for an application flashed over SWD.

The pixl.js bootloader refuses to start an application unless the DFU settings
page at 0x7F000 describes it: bank 0 must be marked valid and the boot
validation (a CRC32 of the image) must match. An OTA update writes that page
itself, a J-Link `loadfile` does not - so flashing only the application by wire
leaves the device booting into DFU mode instead of the analyzer.

This is the same page `nrfutil settings generate` produces, built here because
nrfutil cannot be installed on this machine.

    python3 tools/mkblsettings.py build/PixlAnalyzerOLED.bin build/settings.hex

Pass --base <dump.bin> with a page read back from the device to keep the
fields the bootloader already wrote (bootloader version, SoftDevice size).

The page is written twice: at 0x7F000 and at the backup address 0x7E000 (the
MBR parameters page). When the backup has a valid CRC the bootloader copies the
bank and boot validation fields from it over the main page
(nrf_dfu_settings.c: nrf_dfu_settings_reinit), so a stale backup - e.g. the one
the stock pixl.js image ships - silently replaces our record and the device
boots into DFU mode. nrfutil writes the backup for the same reason.
"""

import argparse
import struct
import zlib

SETTINGS_ADDR = 0x0007F000
BACKUP_ADDR = 0x0007E000  # NRF_MBR_PARAMS_PAGE_ADDRESS, the settings backup
SETTINGS_SIZE = 904  # sizeof(nrf_dfu_settings_t)

# Offsets inside nrf_dfu_settings_t, from components/libraries/bootloader/dfu/nrf_dfu_types.h
OFF_CRC = 0
OFF_VERSION = 4
OFF_APP_VERSION = 8
OFF_BL_VERSION = 12
OFF_BANK_LAYOUT = 16
OFF_BANK_CURRENT = 20
OFF_BANK0 = 24  # image_size, image_crc, bank_code
OFF_BANK1 = 36
OFF_WRITE_OFFSET = 48
OFF_SD_SIZE = 52
OFF_PROGRESS = 56  # dfu_progress_t, 32 bytes
OFF_ENTER_BUTTONLESS = 88
OFF_INIT_COMMAND = 92  # 512 bytes
OFF_BOOT_VALIDATION_CRC = 604
OFF_BV_SOFTDEVICE = 608  # boot_validation_t: type (4) + bytes[64]
OFF_BV_APP = 676
OFF_BV_BOOTLOADER = 744
OFF_PEER_DATA = 812  # 64 bytes, not covered by any CRC
OFF_ADV_NAME = 876   # 28 bytes, not covered by any CRC

# Lengths the bootloader uses when it checks the two CRCs
SETTINGS_CRC_LEN = OFF_INIT_COMMAND - 4  # 88
BOOT_VALIDATION_CRC_LEN = 3 * 68         # the three boot_validation_t structs

NO_VALIDATION = 0
VALIDATE_CRC = 1

BANK_VALID_APP = 0x01
BANK_LAYOUT_DUAL = 0x00
CURRENT_BANK_0 = 0x00

SETTINGS_VERSION_DEFAULT = 2


def put32(buf, offset, value):
    struct.pack_into("<I", buf, offset, value & 0xFFFFFFFF)


def get32(buf, offset):
    return struct.unpack_from("<I", buf, offset)[0]


def build(app_bytes, app_version, base=None):
    page = bytearray(b"\xFF" * SETTINGS_SIZE)

    version = SETTINGS_VERSION_DEFAULT
    bl_version = 1
    sd_size = 0
    if base:
        # Keep what the device's own bootloader wrote, so we cannot disagree
        # with it about the layout it expects
        # The settings version is not kept: a version 1 page makes the
        # bootloader skip the app CRC check and migrate the page itself,
        # and every bootloader that reads the backup page understands 2
        bl_version = get32(base, OFF_BL_VERSION)
        if bl_version == 0xFFFFFFFF:
            bl_version = 1
        sd_size = get32(base, OFF_SD_SIZE)
        if sd_size == 0xFFFFFFFF:
            sd_size = 0

    app_crc = zlib.crc32(app_bytes) & 0xFFFFFFFF

    # Everything the two CRCs cover starts as zero, not as erased flash
    for off, length in ((OFF_VERSION, OFF_INIT_COMMAND - OFF_VERSION),
                        (OFF_BOOT_VALIDATION_CRC, 4 + BOOT_VALIDATION_CRC_LEN)):
        page[off:off + length] = b"\x00" * length

    put32(page, OFF_VERSION, version)
    put32(page, OFF_APP_VERSION, app_version)
    put32(page, OFF_BL_VERSION, bl_version)
    put32(page, OFF_BANK_LAYOUT, BANK_LAYOUT_DUAL)
    put32(page, OFF_BANK_CURRENT, CURRENT_BANK_0)

    put32(page, OFF_BANK0 + 0, len(app_bytes))
    put32(page, OFF_BANK0 + 4, app_crc)
    put32(page, OFF_BANK0 + 8, BANK_VALID_APP)

    put32(page, OFF_SD_SIZE, sd_size)

    # The SoftDevice is present in flash but we are not touching it, so do not
    # ask the bootloader to validate it. The app gets a real CRC check.
    put32(page, OFF_BV_SOFTDEVICE, NO_VALIDATION)
    put32(page, OFF_BV_APP, VALIDATE_CRC)
    put32(page, OFF_BV_APP + 4, app_crc)
    put32(page, OFF_BV_BOOTLOADER, NO_VALIDATION)

    put32(page, OFF_BOOT_VALIDATION_CRC,
          zlib.crc32(bytes(page[OFF_BV_SOFTDEVICE:OFF_BV_SOFTDEVICE + BOOT_VALIDATION_CRC_LEN])))

    put32(page, OFF_CRC, zlib.crc32(bytes(page[4:4 + SETTINGS_CRC_LEN])))

    return page, app_crc


def verify(page):
    """Re-check the page the way the bootloader does."""
    assert get32(page, OFF_CRC) == zlib.crc32(bytes(page[4:4 + SETTINGS_CRC_LEN]))
    assert get32(page, OFF_BOOT_VALIDATION_CRC) == zlib.crc32(
        bytes(page[OFF_BV_SOFTDEVICE:OFF_BV_SOFTDEVICE + BOOT_VALIDATION_CRC_LEN]))
    assert get32(page, OFF_BANK0 + 8) == BANK_VALID_APP
    assert get32(page, OFF_BV_APP) == VALIDATE_CRC


def parse_ihex(text):
    """Intel HEX to {address: byte}, handling both segment and linear records."""
    cells = {}
    segment = 0
    linear = 0
    for line in text.splitlines():
        if not line.startswith(":"):
            continue
        raw = bytes.fromhex(line[1:].strip())
        if (sum(raw) & 0xFF) != 0:
            raise ValueError("bad checksum: " + line)
        count, offset, rtype = raw[0], (raw[1] << 8) | raw[2], raw[3]
        data = raw[4:4 + count]
        if rtype == 0x00:
            base = linear + segment + offset
            for i, b in enumerate(data):
                cells[base + i] = b
        elif rtype == 0x02:  # extended segment address, what objcopy emits
            segment = ((data[0] << 8) | data[1]) * 16
            linear = 0
        elif rtype == 0x04:  # extended linear address
            linear = ((data[0] << 8) | data[1]) << 16
            segment = 0
    return cells


def ihex(data, base_addr):
    """Intel HEX with the extended linear address records J-Link expects."""
    out = []
    upper = None
    for offset in range(0, len(data), 16):
        addr = base_addr + offset
        hi = (addr >> 16) & 0xFFFF
        if hi != upper:
            rec = [0x02, 0x00, 0x00, 0x04, (hi >> 8) & 0xFF, hi & 0xFF]
            rec.append((-sum(rec)) & 0xFF)
            out.append(":" + "".join(f"{b:02X}" for b in rec))
            upper = hi
        chunk = data[offset:offset + 16]
        rec = [len(chunk), (addr >> 8) & 0xFF, addr & 0xFF, 0x00] + list(chunk)
        rec.append((-sum(rec)) & 0xFF)
        out.append(":" + "".join(f"{b:02X}" for b in rec))
    out.append(":00000001FF")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("application", help="application binary (build/<target>.bin)")
    ap.add_argument("output", help="settings page to write, Intel HEX")
    ap.add_argument("--app-version", type=int, default=99)
    ap.add_argument("--base", help="page read back from the device, to preserve its fields")
    ap.add_argument("--merge", help="application hex to prepend, producing one flashable file")
    args = ap.parse_args()

    app = open(args.application, "rb").read()
    base = open(args.base, "rb").read() if args.base else None
    if base is not None and len(base) < SETTINGS_SIZE:
        base = None  # a short or empty dump tells us nothing, build from scratch

    page, app_crc = build(app, args.app_version, base)
    verify(page)

    text = (ihex(page, BACKUP_ADDR).replace(":00000001FF\n", "")
            + ihex(page, SETTINGS_ADDR))

    if args.merge:
        # Do not staple the two files together: objcopy addresses its records
        # with extended *segment* records and this page uses extended *linear*
        # ones, and mixing the two in one file is read differently by different
        # tools. Re-emit everything in one addressing scheme instead.
        cells = parse_ihex(open(args.merge).read())
        merged = []
        run_start = None
        run = bytearray()
        for addr in sorted(cells):
            if run_start is not None and addr != run_start + len(run):
                merged.append((run_start, bytes(run)))
                run = bytearray()
                run_start = None
            if run_start is None:
                run_start = addr
            run.append(cells[addr])
        if run_start is not None:
            merged.append((run_start, bytes(run)))

        text = "".join(ihex(blob, start).replace(":00000001FF\n", "")
                       for start, blob in merged) + text

    open(args.output, "w").write(text)
    print(f"{args.output}: app {len(app)} bytes, crc32 0x{app_crc:08X}, "
          f"settings version {get32(page, OFF_VERSION)}"
          f"{', based on the device dump' if base else ''}")


if __name__ == "__main__":
    main()
