# ATC1441 pixlAnalyzer

Simple 2.4GHz Spectrum Analyzer based on the nRF52832 in the PixlJs allmiibo emulator

This Firmware is currently compatible with the LCD and OLED variant you can get on Aliexpress for around 10-15€ it will make it a very simple battery powered 2400 - 2487MHz Spectrum Analyzer with a Waterfal like history show with little options.

![Image](AnalyzerDemo.jpg)

## What it does

* **Sweep** 2400-2483 MHz, or 2400-2500, or 2360-2500 using the radio's low channel map.
  Fast receiver ramp-up plus 32 RSSI samples per channel, so the air is actually observed
  instead of sampled once per visit.
* **Spectrum** with peak hold, max hold and a reference trace for before/after comparisons,
  auto tracked noise floor and a dB grid.
* **Waterfall** with four levels through an ordered dither, an adjustable sweep-per-row rate,
  and a session history in RAM you can scroll back through.
* **Marker** with a live frequency and dBm readout, and a zoom that magnifies the span.
* **Channel ruler** overlaying the WiFi, BLE or 802.15.4 channel plan.
* **Identify**: park on the marker frequency, record every burst, and report emission width,
  duty cycle, burst length and repetition period together with the technology those numbers
  are consistent with - and how confident that is.
* **BLE scan**: actual advertising packets with a CRC check, listing devices, names, RSSI and
  tracker families (FindMy, Tile, SmartTag, Google), plus how long each has been in range.
* **Mouse/Kbd**: ShockBurst / nRF24 presence detection across the band.
* **Busiest**: the five busiest channels and a "use WiFi channel N" verdict.
* **Meter**: single frequency hunting mode with a trend graph and the LED as a signal indicator.
* **TX test**: unmodulated carrier for antenna work, off by default, lowest power first,
  stops itself after 30 seconds.
* Settings persisted in flash, auto dim and auto sleep, brownout protection, a watchdog, and a
  battery gauge that does not lie.

Buttons: left and right act on the selected tool, a short press of the middle button opens the
menu, a long press cycles the tool (marker, span, waterfall speed, history scroll).



This repo is made together with this explanation video:(click on it)

[![YoutubeVideo](https://img.youtube.com/vi/kgrsfGIeL9w/0.jpg)](https://www.youtube.com/watch?v=kgrsfGIeL9w)

Find them on Aliexpress as example here:

https://aliexpress.com/item/1005008726926205.html


## Flashing

You can flash this firmware fully OTA and go back to the stock Pixl.js firmware as well.
Navigate to the Pixl.js firmware settings and enter the "Firmware Update" menu
The device will reboot and show "DFU Update" now use the nRFConnect App to connect to the "Pixl DFU" device showing.
Select the correct firmware update file "PixlAnalyzerLCD.zip" or "PixlAnalyzerOLED.zip" depending on your device and it will flash and reboot to the new Firmware.

To Go back to the Pixl.js firmware you can open the menu by pressing the middle button, then navigate to "DFU" and flash the Stock firmware in the same way again.

## Project structure

```
firmware/
  Makefile              build both display variants, OTA packaging, host tests
  ld/loader.ld          memory layout, application slot at 0x19000 (OTA)
  config/
    app_config.h        band plan, screen layout, levels, timing
    board_config.h      pin map and display geometry
    custom_board.h      required by the SDK boards.c (empty)
    sdk_config.h        nRF5 SDK configuration
  src/
    main.c              state machine and main loop
    app/
      spectrum.c        traces, peak/max hold, reference, waterfall history
      classify.c        traffic classification from RSSI evidence
      channels.c        WiFi / BLE / 802.15.4 channel plans and scoring
      settings.c        settings persisted in the DFU app data page
      ui.c              every screen
    drivers/
      display.c         SPIM + EasyDMA, dirty pages, contrast, backlight PWM
      spi_bus.c         SPIM0 shared by the display and the NOR flash
      buttons.c         debounced, event based input
      battery.c         oversampled gauge with an interpolated LiPo curve
      systime.c         RTC millisecond clock, TIMER0 microseconds, idle sleep
      power.c           vector table, brownout, watchdog, sleep, DFU handover
      led.c             status LED, used as the signal indicator
      flash_int.c       internal flash page used for settings
      flash_ext.c       2MB SPI NOR driver (identified, never written to)
    gfx/
      gfx.c             primitives, halo text, ordered dither, int formatting
      font5x7.c         main font
      font3x5.c         micro font for the channel ruler
    radio/
      scanner.c         RSSI sweep with fast ramp-up and dwell, park capture
      ble_scan.c        real BLE advertising receiver with CRC check
      esb_scan.c        ShockBurst / nRF24 presence detection
      tx_test.c         unmodulated test carrier
  test/                 host side tests (make test)
  tools/mkdfu.py        signed OTA packaging without nrfutil
  sdk/                  nRF5 SDK 17.1.0
```

## Compiling

You need to have make and gcc-arm-none-eabi installed and working.

The Makefile picks up whatever `arm-none-eabi-gcc` is in PATH, so on Linux and macOS a plain

```
make                     # both variants + OTA zips
make PixlAnalyzerOLED    # single variant, no nrfutil needed
make test                # host side tests of the button debouncing
```

is enough. Override with `make GNU_INSTALL_ROOT=/path/to/bin/ GNU_VERSION=13.3.1` for a toolchain
that is not in PATH. On Windows the path still comes from Makefile.windows in the SDK folder.

See [HANDOFF.md](HANDOFF.md) for flashing with a J-Link, the bootloader settings page that SWD
flashing needs, and the current state of testing.

Packaging the OTA .zip files needs nrfutil. If nrfutil is not available on your platform, use

```
make packages
```

instead, which builds both variants and signs the .zip files with `tools/mkdfu.py`, a small script
that only needs python3 and the `cryptography` module. The packages land in `build/` and are
byte compatible with the ones `nrfutil pkg generate` produces.

Flashing over SWD needs a J-Link:

```
make flash                          # PixlAnalyzerOLED
make flash VARIANT=PixlAnalyzerLCD
```

This programs the application **and** a regenerated bootloader settings page, because the
bootloader refuses to start an application it has no valid settings record for. It never erases
the chip: the SoftDevice, the bootloader and the UICR are left alone, so OTA keeps working.
Wire up SWDIO, SWCLK and GND, and leave the device on its own battery.

## Credits

Credit goes to this repo for the codebase and pinout etc.:

https://github.com/solosky/pixl.js/
