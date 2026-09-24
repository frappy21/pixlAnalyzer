# Handoff

Everything needed to build and flash this firmware on a Linux machine with a
J-Link. Read this first — the two things that will bite you are in **Flashing**.

## What this is

A 2.4 GHz spectrum analyzer firmware for the **Pixl.js** amiibo emulator
(nRF52832, 128x64 mono display, single axis joystick: click plus left/right).
Forked from [atc1441/PixlAnalyzer](https://github.com/atc1441/PixlAnalyzer),
restructured into modules and extended.

Bare metal: the S112 SoftDevice sits in flash at 0x00000 but is never started.
The application owns 0x19000–0x74000 and all 64 KB of RAM. The pixl.js
bootloader at 0x77000 does signed OTA DFU.

## Hardware

| | |
|---|---|
| MCU | nRF52832-QFxx, 512 KB flash / 64 KB RAM |
| Display | SH1106 OLED **or** ST7565/ST7567 LCD, SPIM on P0.25 MOSI / P0.26 SCK / P0.27 CS / P0.28 DC / P0.29 RST / P0.30 backlight |
| Input | joystick: P0.05 left, P0.06 click, P0.07 right (active low) |
| Power | LiPo, ADC on AIN0 (P0.02), charger status P0.03 |
| Extras | 2 MB SPI NOR on P0.18 (identified only, never written), LED P0.31, NFC antenna (unused) |

**There are no SWD pads broken out to a connector on this unit** — the pins were
found on the board itself. Handle the probe wiring carefully.

## Build

Needs `make` and `arm-none-eabi-gcc` in PATH (any recent version; built and
tested with 13.3.1). The nRF5 SDK 17.1.0 is vendored in `firmware/sdk/`.

```bash
cd firmware
make                  # both variants + OTA .zip packages
make PixlAnalyzerOLED # one variant
make test             # host side tests, all four suites must pass
```

`make packages` produces signed OTA packages with `tools/mkdfu.py` — nrfutil is
not needed and cannot be installed on the machine this was developed on
(Homebrew cask disabled, Nordic's download host returns 403, and the pip package
needs Python < 3.11 plus a wheel that does not exist for arm64 macOS). The
script is byte compatible with `nrfutil pkg generate`, verified against the
package shipped in the upstream repo.

## Flashing with a J-Link

```bash
cd firmware
make flash                          # PixlAnalyzerOLED
make flash VARIANT=PixlAnalyzerLCD
```

That runs `tools/flash_jlink.sh`, which reads the current bootloader settings
page off the device, regenerates it for the new image, and programs the
application plus that page.

Wiring: **SWDIO, SWCLK, GND**. Leave the device on its own battery and do not
connect the probe's 3.3 V output to it.

### Two things that will bite you

**1. Never mass erase.** A chip erase wipes the SoftDevice, the bootloader at
0x77000 and the UICR word that points at it. After that there is no OTA and no
way back without flashing all three by wire. Program only:

| Region | What |
|---|---|
| `0x19000`–`0x1F073` | the application |
| `0x7F000` (904 bytes) | bootloader settings page |

`loadfile` only erases the sectors it writes, which is what the script uses.

**2. The bootloader will not start an app flashed by wire unless the settings
page describes it.** `nrf_bootloader.c: app_is_valid()` checks that bank 0 is
marked `NRF_DFU_BANK_VALID_APP` and that the boot validation — a CRC32 of the
image — matches. An OTA update writes that page itself; a `loadfile` does not.
Flash only the application and the device sits in DFU mode instead of booting.

`tools/mkblsettings.py` builds that page (this is what `nrfutil settings
generate` would do). `make settings` writes it on its own, and
`build/<target>-jlink.hex` is a single normalised file containing both the
application and the page, if you would rather flash one file with another tool.

Note that the application hex from objcopy addresses its records with extended
*segment* records while the settings page uses extended *linear* ones. Do not
concatenate the two files by hand — use the merged file, which re-emits
everything in one scheme.

`test/test_blsettings.py` (part of `make test`) verifies the generated page the
way the bootloader verifies it.

## Flashing over the air instead

Works without opening anything:

1. **Hold the joystick to one side while the device starts** — this firmware
   shows "BOOTLOADER / KEEP HOLDING TO ENTER DFU" and enters DFU by itself.
2. Or from the menu: click → the menu opens → **DFU** is the last item.
3. Or the bootloader's own gesture: hold the joystick **right** while power is
   applied, for more than 3 seconds (`NRF_BL_DFU_ENTER_METHOD_BUTTON_PIN = 7`).

Then flash `firmware/build/PixlAnalyzer{OLED,LCD}.zip` with nRF Connect.

## State of things

Built and tested on the host; **the current build has not yet run on hardware**.
The previous build did, and produced two bug reports that drove the fixes below.

### Fixed after hardware feedback

- **Device switched itself off on almost any input.** Root cause was a battery
  reading that could read several hundred millivolts low, feeding an automatic
  "critical battery" shutdown. That shutdown is gone entirely — low battery is
  now only displayed, and POFCON at 2.7 V is the real protection.
- **The "HOLD TO START" gate** after wake-up required holding the click, and
  releasing it slept the device immediately. Removed: sleep is a menu item and
  nothing else. Inactivity sleep now defaults to off.
- **Battery reading jumped between full and half between runs.** The firmware
  keeps the receiver on far more than the original did, so it was measuring the
  sag across the cell's internal resistance, and the first SAADC conversion
  after enabling was landing in the average. Now: 3 ms settling, first
  conversion discarded, second highest of eight kept, exponential filter,
  plausibility range, and a "?" in the icon when the reading fails.
- **Backlight PWM polarity was inverted** — dimming made the LCD brighter.
- **Watchdog** now pauses while the CPU sleeps, so it cannot fire in SYSTEM OFF.
- Sweep order stride could be non-coprime with the channel count, leaving most
  of the band unvisited (`test/test_sweep_order.c` covers this now).
- Peak hold decayed at two different rates across the screen (128 columns were
  driving 88 channel slots).

### What to check on hardware

The boot screen shows **RESET: \<reason\>** and the raw battery reading for
2.5 s, and the Info screen shows both at any time.

1. `RESET:` after an unexpected shutdown — `WAKE` means it slept normally,
   `POWER ON` means the supply collapsed, `WATCHDOG` or `LOCKUP` means the
   firmware hung and that is a real bug.
2. Battery: compare `BATT x.xxx V` and the raw ADC code against a multimeter.
   The divider factor is 1.451 inherited from upstream; trim with
   Settings → Batt cal (per mille, 1000 = nominal) and report the numbers so the
   default can be corrected.
3. Display: backlight dimming and contrast on both panels, and whether the
   4-level dithered waterfall is legible on the LCD.
4. BLE scan: whether advertising packets are actually received (CRC checked) and
   devices appear.
5. Mouse/Kbd screen: whether a real wireless mouse shows up, and whether it
   false-triggers in a quiet room.
6. Identify: point it at a known source (router, microwave oven) and see whether
   the verdict and the evidence numbers are sane.

## Layout

```
firmware/
  Makefile            build, OTA packaging, J-Link flashing, host tests
  ld/loader.ld        app slot at 0x19000
  config/             pin map, band plan, screen layout, SDK config
  src/
    main.c            state machine
    app/              spectrum, classify, channels, settings, ui
    drivers/          display, spi_bus, buttons, battery, systime, power, led, flash
    gfx/              primitives and the two fonts
    radio/            scanner, ble_scan, esb_scan, tx_test, sweep_order
  test/               host side tests
  tools/
    mkdfu.py          signed OTA packages without nrfutil
    mkblsettings.py   bootloader settings page for SWD flashing
    flash_jlink.sh    read page, regenerate, program app + page
  sdk/                nRF5 SDK 17.1.0
```

## Licence

GPL-2.0, inherited from upstream (`LICENSE`, and pixl.js itself is GPL-2.0).
Code taken from solosky/pixl.js may be used under the same terms.
