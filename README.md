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
* **Zigbee**: 802.15.4 frames received through the WazaBee trick (MSK chip correlation on the
  BLE 2M receiver), MAC headers decoded, PANs listed with their addresses, beacons and
  permit-join status, all FCS checked.
* **ESB snif**: ShockBurst packets actually decoded, not just counted. The promiscuous front
  end catches the preamble on both polarities, the software decoder checks the CRC over the
  PCF and payload for every address length, and what validates goes into an address book and
  a packet browser with a payload hex view. The bit order ambiguity of the nRF24
  documentation is handled by trying the candidate CRC engines and byte orders until one
  validates, and the payload byte order is a setting to flip against a live device.
* **ESB TX**: the transmit side of the sniffer, a lab tool for self owned links. Replays a
  captured packet bit for bit (the raw capture is sent verbatim after the recovered address),
  or injects a crafted one through a three button hex editor. Lowest power default, a one
  second hold to start, any button stops it, and it stops itself after 30 s.
* **Beacon TX**: the transmit side of the BLE decoder, for self tests. Six presets (plain
  name, iBeacon, Eddystone UID, Eddystone URL, AltBeacon, Swift Pair, Apple type 0x12
  offline finding) that each decode back through this firmware's own classifier — the host
  tests prove it. Advertising address derived from the device id, one channel per event,
  37/38/39 rotating, 60 s limit.
* **RC DASH**: live remote control link dashboard for 2.4GHz toys. The
  protocol table covers Bayang, SymaX/X5C, H8 mini 3D and MJX (layouts from
  the nRF24 Multiprotocol project): sticks decoded and checksum verified,
  bind phases caught, flags shown. Unknown links land in a generic view
  where the payload bytes that actually move become bars. Hoppers are
  followed with a resweep (the RQ counter). Short MID captures the newest
  packet for the RC emulator.
* **RC emulator**: the transmit side of the dash. Takes a captured control
  packet, edits its sticks through the protocol encoder (checksum
  recomputed) and replays a short burst. For your own toys in your own lab.
* **RADAR**: persistent signal tracking with verdicts - ANALOG VIDEO (an AV
  sender, a 2.4GHz camera, a baby monitor), WIFI VIDEO (a streaming camera
  or drone), HOPPING (a DJI-style 2.4 link), CONTROL LINK, CARRIER. The
  hunt mode keeps a baseline and flashes on anything new (the bug sweep),
  the microwave mode parks on 2450 MHz and watches the level.
* **JAMMER**: the lab interference transmitter - random-data GFSK noise on
  one channel, sweeping, or parked on a WiFi channel centre - for immunity
  testing of self owned receivers. Lowest power default, hold to arm, any
  button stops, 30 s hard limit.
* **NFC**: the board's unused antenna as a field detector (the hidden
  reader hunter: field counter, LED) and as a readable NDEF tag - a URI or
  a text from the settings, the digital business card. UID stable from the
  device id or randomised per start.
* **Unifying**: Logitech keyboard and mouse frames decoded right in the
  packet browser (keys named, mouse deltas), and keystroke injection
  presets in the ESB TX screen for testing one's own receiver.
* **Event log**: radar, hunt and sentry events in the upper half of the
  external NOR flash, written with the user's consent (Settings: Tools: NOR
  log) and browsable on the device.

* **Mouse/Kbd**: ShockBurst / nRF24 presence detection across the band.
* **Busiest**: the five busiest channels and a "use WiFi channel N" verdict.
* **Meter**: single frequency hunting mode with a trend graph and the LED as a signal indicator.
* **TX test**: unmodulated carrier for antenna work, off by default, lowest power first,
  stops itself after 30 seconds.
* Settings persisted in flash, auto dim and auto sleep, brownout protection, a watchdog, and a
  battery gauge that does not lie.

The transmit tools are for research on your own devices in your own lab: replaying or
injecting packets into someone else's equipment, or advertising an address that is not yours,
is not what they are for. The ESB TX and Beacon TX screens say so on their confirm pages.

Buttons: left and right act on the selected tool, a short press of the middle button opens the
menu, a long press cycles the tool (marker, span, waterfall speed, history scroll). A long
left/right on a main screen cycles the carousel - keep holding and it spins through it,
with the category (RF TOOLS / RADIOS / RC + DRONES / NFC) riding along in the banner. The
first boot shows a three page tutorial, and Settings: Behaviour: Start screen picks the
screen the device boots into.



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
      scr_sniff.c       ESB sniffer main screen and the packet browser
      scr_esb_tx.c      ESB transmitter screen and its payload editor
      scr_beacon.c      beacon transmitter screen
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
      ble_beacon.c      beacon payload builders and the advertising transmitter
      esb_frame.c       Enhanced ShockBurst frame model: decode, build, CRC
      esb_scan.c        ShockBurst / nRF24 presence detection
      esb_sniff.c       ESB sniffer: promiscuous capture, address book, packet ring
      esb_tx.c          ESB packet transmitter, bit exact replay
      rc_proto.c        RC toy protocol table: sticks, checksums, bind packets
      radar.c           persistent signal tracking, verdicts, hunt, microwave
      jam.c             the lab jammer: noise, sweep, WiFi channel park
      nfc_ndef.c        NDEF URI and text records
      nfc_tag.c         NFC T2T emulation through the SDK, field events
      log_store.c       event log in the external NOR flash
      unifying.c        Logitech Unifying payload decoder and builders
      zb_rx.c           802.15.4 receiver through BLE 2M chip correlation
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

See [BUILD_AND_FLASH.md](BUILD_AND_FLASH.md) for the full build and flashing instructions (SWD with
any probe, OTA, backups, troubleshooting), and [HANDOFF.md](HANDOFF.md) for what the firmware does
and the current state of testing.

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
