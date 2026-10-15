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
// Build/upload like L7 (docs/build-and-test.md), from this folder (the
// vendored Mutable Instruments library must be passed explicitly):
//   arduino-cli compile -b teensy:avr:teensy40:usb=serialmsc --library libraries/mi --build-path <out> .
//   arduino-cli upload  -b teensy:avr:teensy40:usb=serialmsc -p usb:100000 --input-dir <out> .

#include <Audio.h>

#include "M8Board.h"
#include "n9_input.h"
#include "n9_state.h"
#include "n9_apps.h"
#include "n9_gm_cache.h"
#include "n9_project.h"
#include "n9_song.h"
#include "n9_synth.h"
#include "n9_ui.h"
#include "n9_usb_drive.h"

m8::Board board;

// The synth is defined here, not in n9_synth.cpp: the connections below must
// construct after it, and across files that order isn't guaranteed.
N9Synth synth;
AudioOutputI2S i2sOut;
AudioConnection c1(synth, 0, i2sOut, 0), c2(synth, 1, i2sOut, 1);
AudioControlSGTL5000 codec;

static bool dirty = false;

void setup() {
  Serial.begin(115200);
  if (CrashReport) Serial.print(CrashReport);
  songInit();                                      // the clips live in DMAMEM: no startup init there
  AudioMemory(8);
  board.begin(60000000);                          // the panel takes 70 MHz from stock firmware
  board.powerButtonPressed();                      // the press that switched us on
  // The frame buffer is STATIC: left to malloc it silently fails once
  // RAM2's heap runs low, and the library falls back to direct drawing -
  // which is exactly a full-screen flicker on every sequencer step.
  static DMAMEM uint16_t framebuffer[320 * 240];
  board.tft().setFrameBuffer(framebuffer);
  if (!board.tft().useFrameBuffer(true)) Serial.println("n9: NO FRAMEBUFFER");
  splashShow();                                    // the logo in, the hue cycle (L7's splash)
  splashIntro();                                   // (blocking; any key skips)
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.volume(0.6f);
  synthMidiBegin();
  splashTick();
  appsInit();                                      // the card's /N9 and /APPS folders
  usbDriveBegin();                                 // the computer's disk probing answered from now on
  projectsBegin();                                 // the last project back in
  gmCacheInit();                                   // the GM bank's directory + held-over slots
  for (int i = 0; i < 10 && gmCacheService(true); i++) splashTick();   // the project's programs in
  splashHide();                                    // waits out its minimum, fades
  drawScreen(nav.screen);
}

void loop() {
  if (inputService()) dirty = true;
  if (projectService()) dirty = true;              // a beat-load came due
  if (gmCacheService()) dirty = true;              // a settled GM program flashed in
  synthServiceMidi();
  static uint8_t lastStep = 255;                 // the playhead and clock follow the transport
  if (synth.seqOn() && synth.seqStepN() != lastStep) { lastStep = synth.seqStepN(); dirty = true; }
  static uint32_t meterAt = 0;                   // the mixer's meters tick on their own
  if (nav.screen == SCR_MIX && millis() - meterAt >= 80) { meterAt = millis(); dirty = true; }
  if (dirty) { drawScreen(nav.screen); dirty = false; }

  bool hp;
  if (board.headphonesChanged(hp)) board.setSpeaker(!hp);
  board.chargeGuard();
  if (board.powerButtonPressed()) { codec.volume(0.0f); projectSaveCurrent(); board.powerOff(); }

  // One line once USB serial is likely up, so a host sees the app is alive.
  static bool announced = false;
  if (!announced && millis() > 2000) { announced = true; Serial.printf("n9: up, screen %u\n", nav.screen); }

  // The synth's cost on the serial log, once a second while it plays:
  // the dearest audio block since the last line, against the 2.9 ms budget.
  static uint32_t logAt = 0;
  if (millis() - logAt >= 1000) {
    logAt = millis();
    uint32_t cyc = synth.takeMaxCycles();
    if (synth.active())
      Serial.printf("synth: %u voices, max block %lu us (%lu%% of 2902 us)\n",
                    synth.active(), cyc / 600, cyc / 600 * 100 / 2902);
  }
}
