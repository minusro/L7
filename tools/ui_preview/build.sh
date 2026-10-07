#!/bin/sh
# Builds the UI preview: the sketch's UI, app, engine, storage and scenes code
# against stand-ins for the board (tools/ui_preview/stubs) and the Teensy
# libraries (tools/looper_sim/stubs). Sources are copied next to the stubs so
# the stand-in M8Board.h is the one they include.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
out="${TMPDIR:-/tmp}/m8_ui_preview"
rm -rf "$out" && mkdir -p "$out"
for f in "$root"/*.cpp "$root"/*.h; do
  case "$(basename "$f")" in M8Board.*|input.cpp|midi_usb.cpp) ;; *) cp "$f" "$out/" ;; esac
done
cp "$here"/stubs/*.h "$here/preview.cpp" "$out/"
fontsrc="$HOME/Documents/Arduino/libraries/ILI9341_t3n/src/glcdfont.c"
sed 's/^const unsigned char glcdfont/extern "C" const unsigned char glcdfont/' "$fontsrc" > "$out/glcdfont.cpp"
c++ -std=c++17 -O1 -Wno-macro-redefined -I"$out" -I"$here/../looper_sim/stubs" \
  "$out"/*.cpp -o "$here/ui_preview"
echo "built $here/ui_preview"
