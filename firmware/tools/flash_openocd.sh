#!/bin/sh
#
# Flash the analyzer over SWD with OpenOCD, the same way flash_jlink.sh does
# with JLinkExe: read the device's settings page, regenerate it for the new
# image, program the application and the page. Never erases the chip.
#
#   tools/flash_openocd.sh [PixlAnalyzerOLED|PixlAnalyzerLCD]
#
# Probe defaults to a J-Link; override with OPENOCD_IF=interface/cmsis-dap.cfg

set -e

TARGET="${1:-PixlAnalyzerOLED}"
BUILD=build
IF="${OPENOCD_IF:-interface/jlink.cfg}"
OCD="openocd -f $IF -c 'transport select swd' -c 'adapter speed 1000' -f target/nrf52.cfg"

cd "$(dirname "$0")/.."

if [ ! -f "$BUILD/$TARGET.hex" ] || [ ! -f "$BUILD/$TARGET.bin" ]; then
    echo "Building $TARGET ..."
    make "$TARGET"
fi

echo "--- reading the current settings page from the device ---"
rm -f "$BUILD/settings_old.bin"
eval "$OCD" -c init -c "'dump_image $BUILD/settings_old.bin 0x7F000 0x1000'" -c shutdown || true

BASE=""
if [ -s "$BUILD/settings_old.bin" ]; then
    BASE="--base $BUILD/settings_old.bin"
else
    echo "could not read the page, building the settings from scratch"
fi

echo "--- generating the settings page ---"
python3 tools/mkblsettings.py "$BUILD/$TARGET.bin" "$BUILD/$TARGET-settings.hex" $BASE

echo "--- programming ---"
eval "$OCD" -c init -c "'reset halt'" \
    -c "'flash write_image erase $BUILD/$TARGET.hex'" \
    -c "'flash write_image erase $BUILD/$TARGET-settings.hex'" \
    -c "'verify_image $BUILD/$TARGET.hex'" \
    -c "'verify_image $BUILD/$TARGET-settings.hex'" \
    -c "'reset run'" -c shutdown

echo "done: $TARGET is running, OTA still available"
