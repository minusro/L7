# Building and testing

## Firmware

Teensyduino 1.62, board `teensy:avr:teensy40`, USB type Serial, 600 MHz.
Libraries: ILI9341_t3n (KurtE), Audio, SD/SdFat (bundled).

```sh
"/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli" \
  compile -b teensy:avr:teensy40 --build-path <somewhere-outside-the-IDE-cache> .
```

Use a separate `--build-path`: sharing the IDE's cache breaks builds. Check
the `RAM1`/`RAM2` lines — both banks are nearly full.

## Simulator (`tools/looper_sim`)

Runs engine.cpp, storage.cpp and scenes.cpp on the PC against stubs
(`stubs/`: Audio, SdFat on a temp directory with directories, rename,
truncate; counts file-system changes and file creations).

```sh
cd tools/looper_sim
c++ -std=c++17 -O1 -Wno-macro-redefined -Istubs -I../.. \
  ../../engine.cpp ../../storage.cpp ../../scenes.cpp looper_sim.cpp -o looper_sim
./looper_sim          # all checks + 5 random 600 s sessions; ./looper_sim 0 skips the fuzzing
```

It checks recording alignment and lengths, gapless playback, sync after every
kind of change (`checkSync`: each beat starts on the right frame), ranges,
clipboard, sample stepping, scenes (including bar-line and quick switching,
the SHIFT hold, playhead continuity), queued recordings, overrun silence, and
that nothing changes the file system while playing.

Measurements (printed as `info`, not pass/fail) use `realisticSd`: reads
~2 ms + size, 16 KB writes 97 % ~10 ms, 2 % ~90 ms, 1 % ~250 ms (calibrated
on the M8's card). `LOOPER_TRACE=1 ./looper_sim 0` prints the card
operations before the first dropouts. input.cpp and ui*.cpp are not in the
simulator — key handling and drawing are only tested on the device.

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
