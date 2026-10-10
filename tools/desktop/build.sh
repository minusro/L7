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
for f in "$root"/*.cpp "$root"/*.h; do
  case "$(basename "$f")" in M8Board.*|usb_drive.cpp|midi_usb.cpp) ;; *) cp "$f" "$out/" ;; esac
done
cp "$root/m8_looper.ino" "$out/l7_sketch.cpp"
cp "$here"/stubs/*.h "$here/desktop.cpp" "$out/"
fontsrc="$HOME/Documents/Arduino/libraries/ILI9341_t3n/src/glcdfont.c"
sed 's/^const unsigned char glcdfont/extern "C" const unsigned char glcdfont/' "$fontsrc" > "$out/glcdfont.cpp"
c++ -std=c++17 -O2 -Wno-macro-redefined -I"$out" $(sdl2-config --cflags) \
  "$out"/*.cpp $(sdl2-config --libs) -o "$here/L7"
echo "built $here/L7"
