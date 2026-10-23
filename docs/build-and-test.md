# Building and testing

## Firmware

Teensyduino 1.62, board `teensy:avr:teensy40`, **USB type "Serial + MIDI + Audio + Mass Storage
(L7 patch)"** (`usb=serialmsc`, for storage mode, see usb_drive.h, USB MIDI,
midi_usb.h, and USB audio, playback.md; with plain "Serial" the menu item does nothing and there's no USB
MIDI; a core patched before MIDI was added gets it by running the patch again), 600 MHz. That type isn't in Teensyduino:
run `python3 tools/teensy_msc/patch.py` once (and again after a Teensyduino
update; `--undo` restores the originals) before building. It also patches
SdFat's SDIO driver (SdioTeensy.cpp): in DMA mode it switched its interrupt on
before sending a read's command, and a long interrupt in between (the audio
engine's, at a high tempo with many tracks stretched) let the transfer finish
first, the handler clearing the flag the command was waiting for: 1 s timeouts
("card error 0x0B/0x0C data 0xB" on the serial log), the card re-initialised,
every track running dry. Built without it, that comes back. Without the patch,
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
c++ -std=c++17 -O1 -Wno-macro-redefined -Istubs -I../../src \
  ../../src/engine.cpp ../../src/storage.cpp ../../src/scenes.cpp ../../src/ableton.cpp ../../src/midi_in.cpp ../../src/audio_follow.cpp looper_sim.cpp -o looper_sim
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
python3 tools/ableton/make_template.py "docs/design/ableton/L7 Test 2 Project/L7 Test 2 Project.als" src/ableton_template.h
```

## Demo video (`tools/demo`)

A demo of the main features (about 100 s), rendered from the real app: the sketch
(setup(), loop(), the engine, the UI) built against the desktop app's stand-ins
with `demo.cpp` as the host, in virtual time (no window, no SDL; the audio
"interrupt" runs whenever a block is due). A script presses the keys (a new
project, four parts recorded and layered as 1-, 2- and 4-bar loops, the
filter, a loop range, a mute swipe, scenes, a resampling and a layer over it,
the export to Ableton Live, pressed while playing: the export stops the clock,
its progress shown (the card stand-in taking a slow card's time during it, 4 MB
a second), and the app's start of the clock after it silenced and stopped, so
it's quiet to the end), and a "performer" plays a synthesized beat into
the line input in time with the clock, so what's recorded is what's heard. The
video is the house style (`L7 demo - house.mp4`: 122 BPM, four on the floor,
chord stabs, a chorused supersaw pad, marimba); the script's other styles
still render when named, a video each (`L7 demo - <style>.mp4`, the ones there
from an earlier layout): lofi (85, swung, vinyl crackle, electric piano,
kalimba), synthwave (100: gated snare, octave saw bass, pads, a saw lead, an
arpeggio), boombap (90: organ, flute, vibraphone) and jazz (112, swung 8ths:
ride and brushes, walking bass, piano, guitar, vibraphone). Each style's parts
and sounds are in `writeParts()` and `voice()`; the card starts with a session
whose tempo last set by hand is the style's (the new project takes it). It
writes the screen 30 times a second and the output's audio; `compose.py` lays
it out for Instagram Stories (1080x1920), from the bottom up: the M8's keys,
laid out as on the M8, unlabelled, lit while held, their bottom 90 px above the
frame's (Stories put the reply bar at the bottom; the top safe zone matters
less), the caption above them, and the screen above that, across the whole
width (scaled by area, 3.375x, no bezel). It ends on the app's own splash
played backwards without its credits line, its first frame fading in over the
last second (needs ffmpeg, numpy, Pillow).

```sh
tools/demo/build.sh            # builds, runs, composes the house video (~1.5 min)
tools/demo/build.sh lofi jazz  # those styles instead
python3 tools/demo/compose.py <out-dir> --stills 12,30 <dir>   # frames to check
```

The script steps on transport beats (`untilBeat()`), so it keeps in time with
the music; navigation goes an arrow at a time to a row and cell (`go()`),
never past an edge (that would open the menu).

## README pictures (`tools/readme/make_images.py`)

The README's screenshots, annotated screen, keys and GIF (`docs/readme/`) are
taken from a run of the demo renderer (its `frames.raw`, the real app's
screen): `python3 tools/readme/make_images.py <demo-out-dir>` (Pillow, numpy,
ffmpeg). The moments it takes are the house demo's; retime them in `main()`
if the script changes.

The logo over the README (`docs/readme/l7-logo.svg`, and `l7-logo-light.svg` for
a light page) is drawn from the splash screen's own shape (`l7Row()` in
`src/ui_splash.cpp`) as flat polygons: `python3 tools/readme/make_logo.py`
(nothing else needed). Change the shape there and run it again.

## Which M8 a build is for (`src/screen.h`)

`M8_MODEL` is 1 for the Model:01 (320 x 240) and 2 for the Model:02
(480 x 320); it gives `SCREEN_W` and `SCREEN_H`, which the layout (`ui.h`)
and the host builds' stand-in displays are measured from. The host builds
take it from the environment, with `DEFS` for any other flags:

```sh
M8_MODEL=2 ./build.sh          # tools/ui_preview, tools/desktop
```

The Model:02 keeps the same block size (`PX` 2) and gives its extra room to
the rows (30 -> 40 px) and to the waveform and filter cells; the status row is 34 px, a block
shorter than the space left over, that block going to the gap above it; the volume cell
keeps its width, because its arcs are the volume's 20 steps rather than
anything about the cell's size. In the status row the clock cell is the
Model:01's own 94 wide, so its contents sit in it exactly as they do there:
the master meter (86 px, 22 lines at a 4 px pitch) with a block of space
each side, and the clock's text 20 px in. The meter always starts 4 px into
the cell, so a wider cell would leave more space on its right than on its
left. The scene and the tempo take the extra width between them -- no cell
shows a wide empty frame when it is selected -- and since the scene's
contents sit at its left and the tempo's at its right, the clock still
lands in the middle of what the two leave: 126 and 132 px from them, where
the Model:01 leaves 78 and 28. The scene's and the tempo's
contents come two blocks further in from the row's two ends than on the
Model:01 (`EDGE_INSET` in ui_status.cpp, 8 px against 4; the cells' inner
edges keep 4). Nothing of the Model:02 has run on hardware
and it has no board layer yet (see docs/testing-checklist.md).

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
