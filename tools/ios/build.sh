#!/bin/sh
# Builds L7 for the iPhone Simulator (a first draft): the sketch and all its
# code against the desktop app's stand-ins (tools/desktop/stubs: the screen
# into a buffer, the Teensy core, the SD card as a folder) with ios.mm (UIKit,
# the audio unit, the touch keys), packs L7.app, installs it in a booted (or
# booted here) iPhone simulator and launches it.
#
#   tools/ios/build.sh [simulator name]      (default: iPhone 15 Pro)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
out="${TMPDIR:-/tmp}/l7_ios"
rm -rf "$out" && mkdir -p "$out"
for f in "$root"/*.cpp "$root"/*.h; do
  case "$(basename "$f")" in M8Board.*|usb_drive.cpp|midi_usb.cpp) ;; *) cp "$f" "$out/" ;; esac
done
cp "$root/m8_looper.ino" "$out/l7_sketch.cpp"
cp "$root"/tools/desktop/stubs/*.h "$here/ios.mm" "$out/"
fontsrc="$HOME/Documents/Arduino/libraries/ILI9341_t3n/src/glcdfont.c"
sed 's/^const unsigned char glcdfont/extern "C" const unsigned char glcdfont/' "$fontsrc" > "$out/glcdfont.cpp"

sdk=$(xcrun --sdk iphonesimulator --show-sdk-path)
cxx="xcrun -sdk iphonesimulator clang++ -arch arm64 -mios-simulator-version-min=16.0 -isysroot $sdk"
for f in "$out"/*.cpp; do
  $cxx -std=c++17 -O2 -Wno-macro-redefined -I"$out" -c "$f" -o "${f%.cpp}.o" &
done
wait
$cxx -std=c++17 -O2 -fobjc-arc -Wno-macro-redefined -I"$out" -c "$out/ios.mm" -o "$out/ios.o"
app="$here/L7.app"
rm -rf "$app" && mkdir -p "$app"
$cxx "$out"/*.o -framework UIKit -framework Foundation -framework AVFoundation -framework AudioToolbox \
  -framework QuartzCore -framework CoreGraphics -o "$app/L7"
cat > "$app/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>L7</string>
  <key>CFBundleDisplayName</key><string>L7</string>
  <key>CFBundleIdentifier</key><string>com.minusro.l7</string>
  <key>CFBundleExecutable</key><string>L7</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>MinimumOSVersion</key><string>16.0</string>
  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
  <key>UILaunchScreen</key><dict/>
  <key>UIRequiresFullScreen</key><true/>
  <key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationPortrait</string></array>
  <key>UIBackgroundModes</key><array><string>audio</string></array>
  <key>NSMicrophoneUsageDescription</key><string>L7 records from the microphone, as the M8's line input.</string>
</dict></plist>
PLIST
echo "built $app"

device="${1:-iPhone 15 Pro}"
xcrun simctl boot "$device" 2>/dev/null || true
open -a Simulator
xcrun simctl install "$device" "$app"
xcrun simctl launch "$device" com.minusro.l7
