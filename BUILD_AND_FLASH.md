# Building and flashing

Every command here was run against a real device: an OLED unit, flashed over
SWD with a Flipper Zero in CMSIS-DAP mode and with OTA packages through
nRF Connect. See [HANDOFF.md](HANDOFF.md) for what the firmware does and what
is still unverified.

## 1. Toolchain

You need `make`, `arm-none-eabi-gcc` and `python3`. The nRF5 SDK 17.1.0 is
vendored in `firmware/sdk/`, so nothing else has to be downloaded.

| System | Install |
|---|---|
| Debian / Ubuntu | `sudo apt install gcc-arm-none-eabi make python3` |
| Arch | `sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib make python3` |
| Fedora | `sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-newlib make python3` |
| macOS | `brew install arm-none-eabi-gcc` |

The Makefile picks up whatever `arm-none-eabi-gcc` is in PATH. For a toolchain
that is not, pass it in: `make GNU_INSTALL_ROOT=/path/to/bin/ GNU_VERSION=13.3.1`.

Signing OTA packages needs the `cryptography` module: `pip install cryptography`.
Flashing over SWD needs either OpenOCD (any probe) or JLinkExe (J-Link only).

## 2. Build

```bash
cd firmware
make                       # both variants, plus OTA .zip packages
make PixlAnalyzerOLED      # one variant only (no packaging, no cryptography needed)
make PixlAnalyzerLCD
make test                  # host side tests, must be green before flashing
make clean
```

**Pick the variant that matches the panel.** OLED (SH1106) and LCD (ST7565/ST7567)
differ in the controller init and a two pixel column offset; the wrong one gives
a shifted or blank screen. If you do not know which one the device has, flash
either and look — or read section 6, which identifies it from a flash dump.

Output lands in `firmware/build/`:

| File | What |
|---|---|
| `<target>.hex` | application, 0x19000 upwards |
| `<target>.bin` | the same as raw bytes, used to compute the image CRC |
| `<target>.zip` | signed OTA package for nRF Connect |
| `<target>-settings.hex` | bootloader settings pages, written with the app over SWD |
| `<target>.out`, `.map` | ELF and map, needed by the SWD tools in section 5 |

`make packages` signs with `firmware/priv.pem` through `tools/mkdfu.py`. That is
the same key the pixl.js bootloader trusts — it is published upstream, so the
signature proves nothing about who built the image, it only satisfies the
bootloader.

## 3. Flash over the air

The normal route. No wires, and the device cannot be bricked by it.

1. Copy `firmware/build/<target>.zip` (or one from `prebuilt/`) to a phone with
   **nRF Connect**, and open it in the DFU dialog *before* step 2 — the
   advertisement does not wait forever.
2. Put the device into DFU mode, any of:
   - **Hold the joystick to one side while it starts.** This firmware shows
     "BOOTLOADER / KEEP HOLDING TO ENTER DFU" and enters DFU after a second.
     Release earlier and it boots normally.
   - **Menu → DFU** (last item).
   - The bootloader's own gesture, independent of the firmware: hold the
     joystick **right** while power is applied, for more than three seconds.
3. Connect to **Pixl DFU** and flash the zip.

## 4. Flash over SWD

### Wiring

**SWDIO, SWCLK, GND.** Leave the device on its own battery and do not connect
the probe's 3.3 V output. Swapping SWDIO and SWCLK is the usual mistake and
looks exactly like a dead target (section 7).

### With OpenOCD — any probe, including DAPLink and CMSIS-DAP

```bash
sudo apt install openocd          # or: brew install open-ocd
cd firmware
OPENOCD_IF=interface/cmsis-dap.cfg tools/flash_openocd.sh PixlAnalyzerOLED
```

`OPENOCD_IF` selects the probe: `interface/cmsis-dap.cfg` for DAPLink and for a
Flipper Zero running its DAP Link app, `interface/jlink.cfg` for a J-Link,
`interface/stlink.cfg` for an ST-Link (add `-c "transport select hla_swd"` —
ST's own tools will not program an nRF52, OpenOCD and pyOCD will).

### With a J-Link

```bash
cd firmware
make flash                          # PixlAnalyzerOLED
make flash VARIANT=PixlAnalyzerLCD
```

### What the scripts do, and why it is not just "program the hex"

1. Read the device's current settings page back.
2. Regenerate it for the new image (`tools/mkblsettings.py`).
3. Program the application **and** that page.

Both scripts print `verified ... bytes` for each, and end with the device
running.

**The bootloader will not start an application flashed by wire unless the
settings page describes it.** `nrf_bootloader.c: app_is_valid()` checks that
bank 0 is marked valid and that the boot validation — a CRC32 of the image —
matches. An OTA update writes that page itself; `loadfile` and
`flash write_image` do not. Flash only the application and the device sits in
DFU mode, looking bricked while being perfectly healthy.

The page is written **twice**, at `0x7F000` and at the backup address
`0x7E000`. When the backup has a valid CRC the bootloader copies the bank and
validation fields from it over the main page, so a stale backup silently
replaces a fresh record. nrfutil writes both for the same reason.

### Never mass erase

A chip erase takes the SoftDevice at `0x00000`, the bootloader at `0x77000` and
the UICR word that points at it. After that there is no OTA and no way back
without flashing all three by wire. Only these regions are ever written:

| Region | Size | What |
|---|---|---|
| `0x19000` | the image | application |
| `0x7E000`, `0x7F000` | 904 bytes each | bootloader settings and its backup |

`flash write_image erase` and `loadfile` erase only the sectors they write.

### Back up first

Two minutes, and it is the difference between a mistake and a dead device:

```bash
cd firmware && mkdir -p build/backup
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/nrf52.cfg \
  -c "init" -c "reset halt" \
  -c "dump_image build/backup/flash_full.bin 0x00000000 0x80000" \
  -c "dump_image build/backup/uicr.bin 0x10001000 0x1000" \
  -c "shutdown"
```

Restoring everything, including after a mass erase — the UICR can only be
rewritten after one, so this is also the only way to put the bootloader address
back:

```bash
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/nrf52.cfg \
  -c "init" -c "reset halt" -c "nrf5 mass_erase" \
  -c "flash write_image build/backup/flash_full.bin 0x0" \
  -c "flash write_image build/backup/uicr.bin 0x10001000" \
  -c "reset run" -c "shutdown"
```

## 5. Look at the running device

These work over SWD while the firmware runs, without halting it. They need the
`.out` of the build that is actually flashed, because they resolve symbols from
it.

```bash
cd firmware
export OPENOCD_IF=interface/cmsis-dap.cfg

python3 tools/screenshot.py shot.png --scale 4   # the 128x64 frame buffer as a PNG
python3 tools/rssidump.py                        # the current sweep as numbers
python3 tools/liveview.py                        # a live window of the screen
```

## 6. Check what is on the device

Which variant is flashed, and whether it matches a local build:

```bash
cd firmware
python3 - <<'EOF'
import sys, zlib; sys.path.insert(0, 'tools')
from mkblsettings import parse_ihex, get32, OFF_BANK0
flash = open('build/backup/flash_full.bin', 'rb').read()
page  = open('build/backup/settings_0x7F000.bin', 'rb').read()   # or dump 0x7F000 0x1000
size  = get32(page, OFF_BANK0)
dev   = flash[0x19000:0x19000 + size]
print(f"device: {size} bytes, crc32 0x{zlib.crc32(dev) & 0xffffffff:08X}")
for v in ("OLED", "LCD"):
    blob = open(f"build/PixlAnalyzer{v}.bin", "rb").read()
    n = min(len(blob), len(dev))
    same = sum(1 for i in range(n) if blob[i] == dev[i]) * 100 // n
    print(f"  {v}: {len(blob)} bytes, matches {same}%")
EOF
```

The right variant matches ~99% (the build stamps a version string and git hash
into the image, so it is never 100% unless it is the identical build); the wrong
one lands far below.

## 7. When it does not work

| Symptom | Cause |
|---|---|
| `Error connecting DP: cannot read IDR` | SWDIO and SWCLK swapped, no common ground, bad contact, or the target is unpowered. Drop the speed: `-c "adapter speed 200"`. A working connection prints `SWD DPIDR 0x2ba01477` and `Cortex-M4 r0p1 processor detected`. |
| Connects, but every access fails | APPROTECT. Check `0x10001208` in the UICR dump: `0xFFFFFFFF` is unprotected. Recovering a protected part costs a mass erase, so restore from the backup afterwards. |
| Boots into DFU after an SWD flash | The settings page does not describe the image. Use the scripts in section 4 rather than programming the hex alone. |
| Screen blank or shifted by two columns | Wrong variant: flash the other one. |
| `make` cannot find the compiler | `arm-none-eabi-gcc` is not in PATH; pass `GNU_INSTALL_ROOT` and `GNU_VERSION`. |
| nRF Connect does not see "Pixl DFU" | The advertisement timed out. Open the zip in the DFU dialog first, then put the device into DFU mode. |
| The device seems dead after a failed flash | It almost certainly is not: the bootloader lives in its own region and answers the joystick-right-at-power-up gesture. Flash a `.zip` over the air. |

## 8. A known good session, end to end

What was actually run on 2026-09-26, from a clean checkout:

```
$ cd firmware && make PixlAnalyzerOLED
   text  88376   data 264   bss 40264

$ make test
ALL PASS  x6

$ OPENOCD_IF=interface/cmsis-dap.cfg tools/flash_openocd.sh PixlAnalyzerOLED
Info : SWD DPIDR 0x2ba01477
Info : nRF52832-QFAA(build code: E1) 512kB Flash, 64kB RAM
wrote 88640 bytes from file build/PixlAnalyzerOLED.hex
wrote 5000 bytes from file build/PixlAnalyzerOLED-settings.hex
verified 88640 bytes
verified 1808 bytes
done: PixlAnalyzerOLED is running, OTA still available

$ OPENOCD_IF=interface/cmsis-dap.cfg python3 tools/screenshot.py shot.png
```

The screenshot showed the main menu, which is the proof that matters: the
device booted the application instead of falling back into DFU, so the
regenerated settings page was accepted.
