# Building a sibling app on the M8 (what L7 teaches)

Everything learned making L7 that a new app on the same hardware (N9, and
whatever comes after) can reuse without re-deriving it. Keep this page updated
when a sibling project learns something new. The L7 docs stay the authority on
L7 itself; this is the extract you need to start fresh.

## Starting a sibling project

* Make it **its own sketch in a subfolder** of this repo (`N9/N9.ino`):
  arduino-cli treats the subfolder as a separate sketch, the L7 build ignores
  it, and everything shares one git history.
* **Copy** `M8Board.h/.cpp` and `colors.h` into the sketch folder (Arduino
  can't include files outside it). They're self-contained (Arduino + Wire +
  ILI9341_t3n only). Note in the sketch header that they're copies, and keep
  them in step with L7's.
* Reuse the 3×5 font glyphs from `ui_font.cpp` (N9: `n9_ui.cpp`) — the table
  copies over cleanly without L7's ui.h around it.

## Hardware bring-up (M8Board)

* `board.begin()` does everything: variant detect (pin 17), I2C, key
  expander, display (rotation per variant), backlight. It returns false if
  the key expander doesn't answer.
* Keys are an 8-bit mask (`BTN_*`); `keys()` is cheap to poll (only touches
  I2C when the expander's INT line changed; reads capped at 1 per ms, 50 ms
  after a bus failure — don't poll it harder yourself).
* Power button: `powerButtonPressed()` consumes the press that switched the
  unit on — call it once in setup. Before `powerOff()`, mute the codec
  (`codec.volume(0)`) and flush anything unsaved; L7 requires a 1 s hold
  before powering off.
* Headphones/speaker: `headphonesChanged(in)` debounces the sense pin;
  `setSpeaker(!in)` mirrors it. The speaker amp is ON at boot (stock
  behaviour).
* Battery/charging: `initChargerB()` configures the BQ2589x as stock (512 mA,
  termination, 12 h safety timer; the chip's own protections are autonomous).
  Call `chargeGuard()` from `loop()` for the belt-and-braces layer: it pauses
  charging on abnormal readings and latches off after three strikes.
* MIDI serial: use `midiOutBegin()` / `midiInBegin()`, never the ports'
  `begin()` directly — variant B shares pins between the ports, the key INT
  and the SD card (details in M8Board.h and HARDWARE.md).

## Display

* ILI9341 320×240 over SPI at 30 MHz (stock firmware runs 70; 30 is known
  good). No frame buffer in L7 (RAM is full) — it redraws only what changed.
* A sibling with RAM to spare: `tft().useFrameBuffer(true)` + draw whole
  screens + the synchronous `updateScreen()` is simple and flicker-free; a
  full frame is ~40 ms of blocking, fine for key-driven redraws. **Don't use
  `updateScreenAsync()`**: on the M8 (N9, 30 MHz SPI, Audio library running)
  the DMA transfer stalled — a partial first frame, `asyncUpdateActive()`
  never clearing, the screen frozen for good while `loop()` ran on. Not
  investigated further.

## Audio

* The codec is an **SGTL5000** (Teensy audio shield wiring): `AudioInputI2S`,
  `AudioOutputI2S`, `AudioControlSGTL5000`. Setup: `AudioMemory(n)`, retry
  `codec.enable()` a few times, `inputSelect(AUDIO_INPUT_LINEIN)`,
  `lineInLevel(5)`, `volume(0.6f)`. Measured round trip: 153 samples
  (3.47 ms).
* Put the whole engine in **one AudioStream subclass** (`update()` every
  128 samples at 44.1 kHz) rather than a graph of Audio-library objects:
  one place to reason about, one place to measure.
* **Define every AudioStream and its AudioConnections in the same file (the
  .ino), streams before connections.** Across files the static construction
  order is undefined; a connection built on a not-yet-constructed stream
  corrupts the audio update list and hard-faults in the first audio interrupt
  — on N9 that looked like a crash while the first frame drew, with nothing
  on serial (it died before USB came up, and the printed CrashReport was
  dropped the same way on each reboot).
* **Threading rule** (L7's hard-won one): the audio interrupt owns playback
  state; `loop()` changes it only through explicit handshakes (volatile
  fields set whole, or a small lock-free ring of events — N9's note queue).
  Measure the interrupt with `ARM_DWT_CYCCNT`; the budget is 2.902 ms
  (1.74 M cycles at 600 MHz) per block.

## Build and upload

* Teensyduino 1.62, FQBN `teensy:avr:teensy40:usb=serialmsc`, 600 MHz. The
  USB type comes from `tools/teensy_msc/patch.py` (run once per Teensyduino
  install); it also fixes SdFat's SDIO DMA interrupt bug — build without it
  and long audio interrupts cause 1 s card timeouts.
* Always a **separate `--build-path`** (sharing the IDE cache breaks builds);
  upload with `-p usb:100000 --input-dir <same path>`. Watch the RAM1/RAM2
  lines of every build.
* Workflow: after each change build + upload, **commit only after a
  successful upload** (user notes files included), then push. Log what wasn't
  watched on the device in `docs/testing-checklist.md`.

## RAM (Teensy 4.0: 512 KB RAM1 + 512 KB RAM2)

* Big `const` data goes to RAM1 unless marked `PROGMEM`. Never use
  `sscanf`/`scanf` (pulls ~26 KB of Unicode tables); `strtof`/`strtol` fine.
* Big buffers: `DMAMEM` (RAM2). But any buffer SdFat's SDIO driver DMAs into
  **must be RAM1** (it skips cache maintenance; RAM2 is cached).
* The heap is RAM2; `useFrameBuffer(true)` mallocs its 150 KB there.

## SD card (when a sibling needs storage)

* SdFat SDIO, 16 KB transfer chunks. Writes stall sometimes (~10 ms typical,
  rare 90–250 ms): never do file-system **changes** while timing matters
  (L7's "no FS changes while playing" rule), pre-create files, keep writes
  small and spread out.
* L7's storage.md has the full pattern (pools, units, sessions) if the
  sibling grows persistence.

## UI language (shared look across siblings)

* Everything on a 2×2-pixel block grid (`PX`); the palette is `colors.h`:
  black BG, DARK 17% (background detail), DIM 44% (inactive), LIGHT 81%,
  INK white (active/selected), ACCENT (+ ACCENT_DARK) for what plays,
  records or edits.
* 3×5 pixel font, inverted tags with cut corners; selection = LIGHT frame on
  the selected cell, dark ground under the selected row; badges: accent
  square = playing, grey outline = muted, dark number = empty.
* Status row along the bottom (y 214–239): scene · clock · tempo · accent
  action button with cut left corners. N9 adds a 6-dot screen-stack column
  at its left.
* Cell grammar: arrows select; B (OPT) + arrows adjust; B twice resets; A
  (EDIT) is the context action; SHIFT + A cut/paste. SHIFT and B are
  exclusive arrow modifiers (last pressed wins).
* Timing that feels right: key repeat 200 ms then every 50 ms; double tap
  200 ms; hold-to-switch 300–600 ms.

## Switching apps on the device

`app_loader.h/.cpp` + `FlashTxx.c/.h` (FlasherX, public domain) are kept as
identical copies in each app, like M8Board. Each app carries a key chord
that flashes the OTHER app's Intel-hex from the card (`/APPS/<APP>.HEX`,
staged by `tools/make_apps.sh`) into upper flash, verifies it, copies it
over the running program and reboots — ~15 s, computer-free. Every app must
contain the `FLASH_ID` string (app_loader.cpp does this) or images of it
will be refused. The final copy is the only brick window (a few seconds);
recovery is a USB reflash.

## Debugging

* Serial 115200. L7's freeze detector pattern is worth copying early: a
  low-priority `IntervalTimer` checks every 100 ms that `loop()` and the
  audio interrupt both advanced, and names the stage `loop()` was in
  (a volatile stage string updated as it goes).
* Print `CrashReport` at boot if set.
* A PC simulator (L7: `tools/looper_sim` against stubs) and a UI preview that
  renders the real drawing code to PNGs (`tools/ui_preview`) both paid for
  themselves many times over; build them once the sibling's logic grows.
