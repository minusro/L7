# Architecture

## Modules

| File | Role |
|---|---|
| `m8_looper.ino` | `setup()` / `loop()`, the key list (header comment), power button, freeze detector, splash |
| `M8Board.*` | Board access: display (ILI9341_t3n), keys (SX1508 on variant B), headphone sense, speaker enable, MIDI ports |
| `engine.*` | Audio interrupt (`LooperEngine::update()`): master clock, varispeed playback from the track rings, filters, volume/balance, master volume and output meter, input capture and monitoring, MIDI clock scheduling |
| `storage.*` | Everything on the SD card, all from `loop()`: streaming tracks into their rings, recording, sample files and the pool, track units, the clipboard, transport start/stop, clip changes while playing |
| `scenes.*` | The scene list (`/looper/scenes.dat`), stepping, cut/paste/duplicate, the SHIFT hold |
| `app.*` | Shared state: `board`, `bpm`, the selection (`selected` row, `selectedCell`), `setTempo()` |
| `input.*` | Keys: navigation, cell editing, transport, recording, scenes, double taps and key repeat |
| `ui*.cpp`, `ui.h` | Drawing: `ui.cpp` (rows, refresh pass), `ui_wave` (waveform, playheads, range), `ui_filter` (curve), `ui_mix` (volume/balance lines + meters), `ui_buttons` (icons), `ui_status` (tempo, clock, scene label), `ui_menu` (the menu, B+A), `ui_splash` (logo) |
| `colors.h` | All colours, as named constants |
| `config.h` | Constants: buffer sizes, limits, latency, tempo range |
| `tools/looper_sim/` | PC simulator and test suite for engine + storage + scenes (see build-and-test.md) |

## Threading

* **Audio interrupt** — `LooperEngine::update()` every 128 samples (44.1 kHz):
  advances the clock, reads each live track's ring at its speed
  (`playSpb / spb`), filters and mixes, writes the input into `inputRing`,
  queues MIDI clock ticks. On each beat (`onBeat`) it starts armed tracks and
  re-snaps every live track's ring position to where the clock says it should
  be (no drift, tempo changes included).
* **`loop()`** — everything else: SD reads/writes (`serviceStorage()`), scene
  saving (`serviceScenes()`), keys, drawing (`uiRefresh()`, a little every
  30 ms so streaming keeps going).
* They talk through volatile fields on `Track`, `Transport` and `Recording`
  (engine.h). Rule: a track's playback fields are changed by `loop()` only
  while the track is neither live nor armed — *except* through the explicit
  handshakes: the start handshake (`armed`, `startBeat`, `s0`) and the sample
  switch (`sampleSwitch`, `switchAt`, `nextS0`, `nextStartBeat`, `nextSpb`),
  see playback.md. Tempo changes go through `transport.pendingSpb`
  (optionally held until `transport.spbAtBeat`).

## RAM (Teensy 4.0: 512 KB RAM1 + 512 KB RAM2)

Free: ≈70 KB in RAM1 (stack lives there), ≈13 KB in RAM2 (the heap; little
uses it: printf internals). The big items:

* 8 track rings × 64 KB (`RING_FRAMES` = 16384 stereo frames, ~370 ms at 1x):
  seven in RAM2 (DMAMEM), the eighth in RAM1.
* `inputRing` 128 KB (`INPUT_FRAMES` = 32768, ~740 ms): pre-roll for late
  record presses and slack for slow SD writes.
* `chunk` 16 KB (RAM1): the SD transfer buffer. It must stay in RAM1: SdFat's
  SDIO driver DMAs straight into it without cache maintenance, and RAM2 is
  cached.
* `sampleOrder[]` 20 KB, per-track plans ~8 KB and crossfade caches ~8 KB:
  in RAM2 (DMAMEM; only `loop()` touches them, never DMA).

Keeping RAM1 free:
* Teensy 4 copies `const` data into RAM1 unless it's marked `PROGMEM` (the
  splash logo is). Fonts and tables add up.
* Don't use `sscanf`/`scanf`: newlib's version pulls in ~26 KB of Unicode
  tables (parse by hand; `strtof`/`strtol` are fine).

Consequences: the rings can't grow much (8 tracks × any increase); caching
the next scene's audio is not possible. A track played faster than recorded empties its ring faster
(at 1.33x a full ring lasts ~280 ms), which is where the remaining dropouts
on slow card writes come from.
