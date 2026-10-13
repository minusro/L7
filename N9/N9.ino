// N9 — L7's sister app: nine MIDI clip tracks, each with its own synthesizer
// engine (32 voices shared). Prototype so far:
//   * the six screens (mock content), stepped with SHIFT + UP/DOWN, top to
//     bottom: FX (master effects & sends), mixer, track modulation, track
//     instrument, clip grid, clip editor
//   * a cursor on every screen (arrows, held = repeating): FX cells and sends,
//     mixer strips, mod sources + cells, instrument cells, the grid (its track
//     is the selected track), the piano roll
//   * first sound (n9_synth): 32 voices, MIDI channel 1 in over USB and the
//     serial MIDI port; B + LEFT/RIGHT on the INST screen steps the engine
//     (SAW, PULSE, FM2, PLUCK — placeholders). No storage, no editing yet.
//
// Design document: ../docs/design/n9_mockup.html. What N9 reuses from L7:
// ../docs/sibling-app-notes.md. M8Board.cpp/.h and colors.h are copies of
// L7's (../); keep them in step.
//
// Build/upload like L7 (docs/build-and-test.md), from this folder:
//   arduino-cli compile -b teensy:avr:teensy40:usb=serialmsc --build-path <out> .
//   arduino-cli upload  -b teensy:avr:teensy40:usb=serialmsc -p usb:100000 --input-dir <out> .

#include <Audio.h>

#include "M8Board.h"
#include "n9_input.h"
#include "n9_state.h"
#include "n9_synth.h"
#include "n9_ui.h"

m8::Board board;

AudioOutputI2S i2sOut;
AudioConnection c1(synth, 0, i2sOut, 0), c2(synth, 1, i2sOut, 1);
AudioControlSGTL5000 codec;

static bool dirty = false;

void setup() {
  Serial.begin(115200);
  if (CrashReport) Serial.print(CrashReport);
  AudioMemory(8);
  board.begin();
  board.powerButtonPressed();                      // the press that switched us on
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.volume(0.6f);
  synthMidiBegin();
  board.tft().useFrameBuffer(true);
  drawScreen(nav.screen);
}

void loop() {
  if (inputService()) dirty = true;
  synthServiceMidi();
  if (dirty && !board.tft().asyncUpdateActive()) { drawScreen(nav.screen); dirty = false; }

  bool hp;
  if (board.headphonesChanged(hp)) board.setSpeaker(!hp);
  if (board.powerButtonPressed()) { codec.volume(0.0f); board.powerOff(); }

  // The synth's cost on the serial log, once a second while it plays:
  // the dearest audio block since the last line, against the 2.9 ms budget.
  static uint32_t logAt = 0;
  if (millis() - logAt >= 1000) {
    logAt = millis();
    uint32_t cyc = synth.takeMaxCycles();
    if (synth.active() || cyc)
      Serial.printf("synth: %u voices, max block %lu us (%lu%% of 2902 us)\n",
                    synth.active(), cyc / 600, cyc / 600 * 100 / 2902);
  }
}
