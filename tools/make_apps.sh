#!/bin/sh
# make_apps.sh — builds L7, and N9 beside it when its folder is there, and
# stages their Intel-hex images for the SD card's /APPS folder (the on-device
# app switcher flashes these: app_loader.h). Copy apps_sd/L7.HEX and
# apps_sd/N9.HEX to the card as /APPS/L7.HEX and /APPS/N9.HEX (card reader,
# or L7's storage mode). N9 lives on the `experiments` branch, not on main,
# so on main this builds L7 alone and leaves any apps_sd/N9.HEX as it is.
set -e
cd "$(dirname "$0")/.."
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
OUT="${TMPDIR:-/tmp}/m8_apps_build"
mkdir -p apps_sd "$OUT/l7" "$OUT/n9"
echo "building L7..."
"$CLI" compile -b teensy:avr:teensy40:usb=serialmsc --build-path "$OUT/l7" . | tail -3
cp "$OUT/l7/L7.ino.hex" apps_sd/L7.HEX
if [ -f N9/N9.ino ]; then                        # (on main the folder can still be there, holding only its ignored sound sources)
  echo "building N9..."
  ( cd N9 && "$CLI" compile -b teensy:avr:teensy40:usb=serialmsc --library libraries/mi --build-path "$OUT/n9" . | tail -3 )
  cp "$OUT/n9/N9.ino.hex" apps_sd/N9.HEX
else
  echo "no N9/ here (it is on the experiments branch): building L7 alone"
fi
ls -la apps_sd
echo "copy both files to the SD card as /APPS/L7.HEX and /APPS/N9.HEX"
