# Building and testing

## Firmware

Teensyduino 1.62, board `teensy:avr:teensy40`, **USB type "Serial + MIDI + Audio + Mass Storage
(L7 patch)"** (`usb=serialmsc`, for storage mode, see usb_drive.h, USB MIDI,
midi_usb.h, and USB audio, playback.md; with plain "Serial" the menu item does nothing and there's no USB
MIDI; a core patched before MIDI was added gets it by running the patch again), 600 MHz. That type isn't in Teensyduino:
run `python3 tools/teensy_msc/patch.py` once (and again after a Teensyduino
update; `--undo` restores the originals) before building. Without the patch,
`usb=serialmtp` ("Serial + MTP Disk") also builds: storage mode then shows the
card in an MTP app (Android File Transfer) instead of mounting it.
Libraries: ILI9341_t3n (KurtE), Audio, SD/SdFat (bundled).

```sh
"/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli" \
  compile -b teensy:avr:teensy40:usb=serialmsc --build-path <somewhere-outside-the-IDE-cache> .
```

Use a separate `--build-path`: sharing the IDE's cache breaks builds. Check
the `RAM1`/`RAM2` lines — both banks are nearly full.

Uploading to the M8 over USB (it shows up as a Teensy 4.0; the port is the
"Teensy Ports" row of `arduino-cli board list`, usually `usb:100000`):

```sh
"/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli" \
  upload -b teensy:avr:teensy40:usb=serialmsc -p usb:100000 --input-dir <the same build path> .
```

Working with Claude Code: after each firmware change, build and upload, so
the latest sketch is always on the M8 to try. After each successful upload,
commit the change, the user's notes files (such as `M8 Looper.txt`)
included; no commit when the upload fails.

## Simulator (`tools/looper_sim`)

Runs engine.cpp, storage.cpp and scenes.cpp on the PC against stubs
(`stubs/`: Audio, SdFat on a temp directory with directories, rename,
truncate; counts file-system changes and file creations).

```sh
cd tools/looper_sim
c++ -std=c++17 -O1 -Wno-macro-redefined -Istubs -I../.. \
  ../../engine.cpp ../../storage.cpp ../../scenes.cpp ../../ableton.cpp ../../midi_in.cpp ../../audio_follow.cpp looper_sim.cpp -o looper_sim
./looper_sim          # all checks + 5 random 600 s sessions; ./looper_sim 0 skips the fuzzing
```

It checks recording alignment and lengths, gapless playback, sync after every
kind of change (`checkSync`: each beat starts on the right frame), ranges,
clipboard, sample stepping, scenes (including bar-line and quick switching,
the SHIFT hold, playhead continuity), queued recordings, overrun silence,
declicking (sine input: the largest sample-to-sample step at loop and range
wraps, mute, a clip switch and transport stop stays within twice the sine's
own), and that nothing changes the file system while playing. Loop code that
busy-waits for the interrupt uses `delayMicroseconds()`, which the simulator
turns into audio blocks.

Measurements (printed as `info`, not pass/fail) use `realisticSd`: reads
~2 ms + size, 16 KB writes 97 % ~10 ms, 2 % ~90 ms, 1 % ~250 ms (calibrated
on the M8's card). `LOOPER_TRACE=1 ./looper_sim 0` prints the card
operations before the first dropouts. input.cpp and ui*.cpp are not in the
simulator — key handling and drawing are only tested on the device.

The Live Set export's test writes real sets to `/tmp/looper_sim_sd/L7/EXPORTS/`
(open them in Live) and checks them with Python (gzip, XML, the tracks,
devices, scenes, clips and copied samples).

## Live Set templates (`tools/ableton/make_template.py`)

`ableton_template.h` is made from a set Live saved
(`docs/design/ableton/L7 Test 2 Project/L7 Test 2 Project.als`, Live 11.3): its first
track (the two Auto Filters kept, other devices left out), a clip slot, a
clip, a scene and the set around them, with the fields the export fills in
marked. Remake it after changing that set:

```sh
python3 tools/ableton/make_template.py "docs/design/ableton/L7 Test 2 Project/L7 Test 2 Project.als" ableton_template.h
```

## UI preview (`tools/ui_preview`)

Renders the screen on the PC with the firmware's own drawing code (ui*.cpp,
app, engine, storage, scenes) against a stand-in display that draws into a
frame buffer (`stubs/M8Board.h`). `preview.cpp` sets the tracks up in a few
states (a selected cell of each kind, the status row, an empty, a muted and a
recording track) and writes each as a PNG at 2× (needs Python with PIL).

```sh
cd tools/ui_preview
./build.sh && ./ui_preview <out-dir>
```

`./ui_preview <out-dir> showcase` renders instead eight varied screens (each
its own project, colour, tempo, scenes and tracks: a groove, range and filter
and volume edits, a recording, tap tempo, the menu, a new project waiting to
record) as `show_*.png`; the last set is in `docs/design/showcase/`.

## Help pages (`tools/help/make_help.py`)

The menu's HELP pages are drawn from `help_slides.h`, which this script
writes from the pages' content (in the script) and the UI preview's
screenshots. After changing either (or anything the screenshots show), render
the screenshots and run it again:

```sh
tools/ui_preview/build.sh && mkdir -p /tmp/l7prev && (cd /tmp/l7prev && "$OLDPWD/tools/ui_preview/ui_preview")
python3 tools/help/make_help.py /tmp/l7prev
```

The preview renders the pages themselves too (`ui_help_1.png` ...), as the
firmware draws them.

## On the device

Serial (115200) logs worth asking for when something glitches:
* `dropout: track N +K samples (loop in: <stage>, …)` — a ring ran dry, and
  what `loop()` was doing.
* `slow SD read/write: N ms …` — card operations over 150 ms.
* `no recording space left: creating … while playing` — pool exhausted.
* `recording fell behind (…)` — input lost, saved as silence.
* Freeze detector: reports the stage if `loop()` stops for too long.

Measured on the hardware: round-trip latency 3.47 ms (153 samples); headphone
sense ≈ 10 plugged / ≈ 580 unplugged on pin 22; speaker enable pin 9 HIGH = on.

## User guide and cheatsheet

`docs/user-guide/guide.html` (A4) and `docs/user-guide/cheatsheet.html` (one
A5 page) are printed to `docs/L7 User Guide.pdf` and `docs/L7 Cheatsheet.pdf`;
print both again after a change to either (the cheatsheet must stay one page):

```sh
for f in "guide:L7 User Guide" "cheatsheet:L7 Cheatsheet"; do
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless --disable-gpu \
    --no-pdf-header-footer --print-to-pdf="docs/${f#*:}.pdf" "file://$PWD/docs/user-guide/${f%%:*}.html"
done
```
