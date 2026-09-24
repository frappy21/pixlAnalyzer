#!/bin/sh
#
# Flash the analyzer over SWD with a J-Link, keeping the device able to take
# OTA updates afterwards.
#
#   tools/flash_jlink.sh [PixlAnalyzerOLED|PixlAnalyzerLCD]
#
# What it does, and why:
#  1. reads the current bootloader settings page back from the device, so the
#     regenerated one keeps the fields the bootloader itself wrote
#  2. programs the application at 0x19000 only
#  3. programs a matching settings page at 0x7F000, otherwise the bootloader
#     decides the application is invalid and sits in DFU mode forever
#
# It never erases the chip: the SoftDevice (0x00000), the bootloader (0x77000)
# and the UICR that points at it are left untouched. A mass erase would make
# OTA impossible and the device would need all three flashed back by wire.

set -e

TARGET="${1:-PixlAnalyzerOLED}"
DEVICE=nrf52832_xxaa
BUILD=build
JLINK="${JLINK:-JLinkExe}"

cd "$(dirname "$0")/.."

if [ ! -f "$BUILD/$TARGET.hex" ] || [ ! -f "$BUILD/$TARGET.bin" ]; then
    echo "Building $TARGET ..."
    make "$TARGET"
fi

echo "--- reading the current settings page from the device ---"
cat > "$BUILD/read_settings.jlink" <<EOF
device $DEVICE
si 1
speed 4000
connect
savebin $BUILD/settings_old.bin 0x7F000 0x1000
exit
EOF
rm -f "$BUILD/settings_old.bin"
"$JLINK" -nogui 1 -commandfile "$BUILD/read_settings.jlink" || true

BASE=""
if [ -s "$BUILD/settings_old.bin" ]; then
    BASE="--base $BUILD/settings_old.bin"
    echo "read back $(wc -c < "$BUILD/settings_old.bin" | tr -d ' ') bytes"
else
    echo "could not read the page, building the settings from scratch"
fi

echo "--- generating the settings page ---"
python3 tools/mkblsettings.py "$BUILD/$TARGET.bin" "$BUILD/$TARGET-settings.hex" $BASE

echo "--- programming ---"
cat > "$BUILD/flash.jlink" <<EOF
device $DEVICE
si 1
speed 4000
connect
loadfile $BUILD/$TARGET.hex
loadfile $BUILD/$TARGET-settings.hex
r
g
exit
EOF
"$JLINK" -nogui 1 -commandfile "$BUILD/flash.jlink"

echo "done: $TARGET is running, OTA still available"
