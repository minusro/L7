#!/bin/sh
# Builds the demo renderer (the sketch and all its code against the desktop
# app's stand-ins, tools/desktop/stubs, with demo.cpp as the host: virtual time,
# no window, no SDL), runs it, and composes the video with compose.py.
#
#   tools/demo/build.sh [out-dir]       (default: $TMPDIR/l7_demo; the video: tools/demo/L7 demo.mp4)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
src="${TMPDIR:-/tmp}/l7_demo_src"
out="${1:-${TMPDIR:-/tmp}/l7_demo}"
rm -rf "$src" && mkdir -p "$src" "$out"
for f in "$root"/*.cpp "$root"/*.h; do
  case "$(basename "$f")" in M8Board.*|usb_drive.cpp|midi_usb.cpp|app_loader.cpp) ;; *) cp "$f" "$src/" ;; esac
done
cp "$root/m8_looper.ino" "$src/l7_sketch.cpp"
cp "$root"/tools/desktop/stubs/*.h "$here"/stubs/*.h "$here/demo.cpp" "$src/"
fontsrc="$HOME/Documents/Arduino/libraries/ILI9341_t3n/src/glcdfont.c"
sed 's/^const unsigned char glcdfont/extern "C" const unsigned char glcdfont/' "$fontsrc" > "$src/glcdfont.cpp"
c++ -std=c++17 -O2 -Wno-macro-redefined -I"$src" "$src"/*.cpp -o "$out/l7demo"
"$out/l7demo" "$out"
python3 "$here/compose.py" "$out" "$here/L7 demo.mp4"
