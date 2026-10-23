#!/bin/sh
# Builds the demo renderer (the sketch and all its code against the desktop
# app's stand-ins, tools/desktop/stubs, with demo.cpp as the host: virtual time,
# no window, no SDL), runs it, and composes the video with compose.py.
#
#   tools/demo/build.sh [styles...]     (default: house; the videos: tools/demo/L7 demo - <style>.mp4)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
src="${TMPDIR:-/tmp}/l7_demo_src"
out="${TMPDIR:-/tmp}/l7_demo"
styles="${*:-house}"
rm -rf "$src" && mkdir -p "$src" "$out"
for f in "$root"/src/*.cpp "$root"/src/*.h; do
  case "$(basename "$f")" in M8Board.*|usb_drive.cpp|midi_usb.cpp) ;; *) cp "$f" "$src/" ;; esac
done
sed 's|#include "src/|#include "|' "$root/L7.ino" > "$src/l7_sketch.cpp"
cp "$root"/tools/desktop/stubs/*.h "$here/demo.cpp" "$src/"
fontsrc="$HOME/Documents/Arduino/libraries/ILI9341_t3n/src/glcdfont.c"
sed 's/^const unsigned char glcdfont/extern "C" const unsigned char glcdfont/' "$fontsrc" > "$src/glcdfont.cpp"
c++ -std=c++17 -O2 -Wno-macro-redefined -I"$src" "$src"/*.cpp -o "$out/l7demo"
for st in $styles; do
  "$out/l7demo" "$out" "$st"
  python3 "$here/compose.py" "$out" "$here/L7 demo - $st.mp4"
done
