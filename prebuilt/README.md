# Prebuilt images

Built from the sources in this repository at the commit that added this file,
with `arm-none-eabi-gcc 13.3.1`. Rebuild with `cd firmware && make packages` if
you want to be sure they match.

Pick **OLED** or **LCD** to match the panel in the device.

| File | Use |
|---|---|
| `PixlAnalyzer*.zip` | OTA update through nRF Connect, the normal way |
| `PixlAnalyzer*-jlink.hex` | one file for SWD: application **and** bootloader settings page |
| `PixlAnalyzer*.hex` | application only, 0x19000–0x1F0xx |
| `PixlAnalyzer*-settings.hex` | bootloader settings page only, 0x7F000, 904 bytes |

## SWD

Program `*-jlink.hex`, or the two separate files, and **never mass erase** — a
chip erase takes the SoftDevice, the bootloader and the UICR with it, and there
is no OTA after that. `loadfile` erases only the sectors it writes.

The application on its own is not enough: the bootloader checks the settings
page at 0x7F000 and refuses to start an application it has no valid record for,
dropping into DFU mode instead. That is what the settings page is for. See
[../HANDOFF.md](../HANDOFF.md).

```
JLinkExe -device nrf52832_xxaa -if SWD -speed 4000
J-Link> connect
J-Link> loadfile PixlAnalyzerOLED-jlink.hex
J-Link> r
J-Link> g
```

Or from a checkout, which regenerates the settings page from the device's own
current page: `cd firmware && make flash`.
