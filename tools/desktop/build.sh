#!/bin/sh
# Builds L7 as a desktop app (SDL2): the sketch and all its code against
# stand-ins for the board (stubs/M8Board.h: the screen into a window, the
# keys from the keyboard), the Teensy core and Audio library (stubs/Audio.h:
# SDL's audio thread runs the engine) and the SD card (a folder,
# ~/Documents/L7 Desktop). Sources are copied next to the stubs so the
# stand-in headers are the ones they include.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
out="${TMPDIR:-/tmp}/l7_desktop"
rm -rf "$out" && mkdir -p "$out"
for f in "$root"/src/*.cpp "$root"/src/*.h; do
  case "$(basename "$f")" in M8Board.*|usb_drive.cpp|midi_usb.cpp) ;; *) cp "$f" "$out/" ;; esac
done
sed 's|#include "src/|#include "|' "$root/L7.ino" > "$out/l7_sketch.cpp"
cp "$here"/stubs/*.h "$here/desktop.cpp" "$out/"
fontsrc="$HOME/Documents/Arduino/libraries/ILI9341_t3n/src/glcdfont.c"
sed 's/^const unsigned char glcdfont/extern "C" const unsigned char glcdfont/' "$fontsrc" > "$out/glcdfont.cpp"
# SDL linked in (the app runs without Homebrew's).
sdl_static=$(sdl2-config --static-libs | sed "s|-lSDL2 |$(sdl2-config --prefix)/lib/libSDL2.a |")
c++ -std=c++17 -O2 -Wno-macro-redefined -I"$out" $(sdl2-config --cflags) \
  "$out"/*.cpp $sdl_static -o "$here/L7"
echo "built $here/L7"

# On a Mac: L7.app (opens straight into the app, no terminal), its icon the
# logo (./L7 --icon), its card ~/Documents/L7 Desktop.
[ "$(uname)" = Darwin ] || exit 0
app="$here/L7.app"
rm -rf "$app" && mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$here/L7" "$app/Contents/MacOS/L7"
set_="$out/L7.iconset" && mkdir -p "$set_"
for n in 16 32 128 256 512; do
  for s in 1 2; do
    px=$((n * s)); name=icon_${n}x${n}; [ $s = 2 ] && name=${name}@2x
    "$here/L7" --icon "$out/i.ppm" $px && sips -s format png "$out/i.ppm" --out "$set_/$name.png" >/dev/null
  done
done
iconutil -c icns "$set_" -o "$app/Contents/Resources/L7.icns"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>L7</string>
  <key>CFBundleDisplayName</key><string>L7</string>
  <key>CFBundleIdentifier</key><string>com.minusro.l7</string>
  <key>CFBundleExecutable</key><string>L7</string>
  <key>CFBundleIconFile</key><string>L7</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSMicrophoneUsageDescription</key><string>L7 records from your audio input, as the M8's line input.</string>
</dict></plist>
PLIST
codesign --force -s - "$app" 2>/dev/null || true
echo "built $app"
