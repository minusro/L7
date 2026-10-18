// K6 — a fresh six-track clip machine for the M8 hardware: the 3x2 screen
// map (SONG/GRID/MIX over PROJ/NOTE/INST) over the proven engine layer
// (32-voice pool synth, FX chains, the boot-loaded GM flash region, projects,
// the app switcher), kept in step with the N9-era app's copies.
// Design: ../docs/design/k6_mockup.html. M8Board.cpp/.h and colors.h are
// copies of L7's; keep them in step.
//
// Build/upload (from this folder; the vendored Mutable Instruments library
// is shared with the N9-era app):
//   arduino-cli compile -b teensy:avr:teensy40:usb=serialmsc --library ../N9/libraries/mi --build-path <out> .
//   arduino-cli upload  -b teensy:avr:teensy40:usb=serialmsc -p usb:100000 --input-dir <out> .
#include <Audio.h>

#include "M8Board.h"
#include "k6_apps.h"
#include "k6_gm_cache.h"
#include "k6_input.h"
#include "k6_project.h"
#include "k6_song.h"
#include "k6_state.h"
#include "k6_synth.h"
#include "k6_ui.h"
#include "k6_usb_drive.h"

m8::Board board;

// The synth is defined here, not in k6_synth.cpp: the connections below must
// construct after it, and across files that order isn't guaranteed.
K6Synth synth;
AudioOutputI2S i2sOut;
AudioConnection c1(synth, 0, i2sOut, 0), c2(synth, 1, i2sOut, 1);
AudioControlSGTL5000 codec;

static bool dirty = false;

void setup() {
  Serial.begin(115200);
  if (CrashReport) Serial.print(CrashReport);
  songInit();                                      // the clips live in DMAMEM: no startup init there
  AudioMemory(8);
  board.begin(60000000);                           // the panel takes 70 MHz from stock firmware
  board.powerButtonPressed();                      // the press that switched us on
  static DMAMEM uint16_t framebuffer[320 * 240];   // static: malloc fails silently when low
  board.tft().setFrameBuffer(framebuffer);
  if (!board.tft().useFrameBuffer(true)) Serial.println("k6: NO FRAMEBUFFER");
  splashShow();
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.volume(0.6f);
  trackLayerInit();
  synthMidiBegin();
  appsInit();                                      // the card's /K6 and /APPS folders
  usbDriveBegin();                                 // the computer's disk probing answered from now on
  projectsBegin();                                 // the last project back in
  gmCacheInit();                                   // the whole GM bank into its flash region (once)
  drawScreen();
}

void loop() {
  if (inputService()) dirty = true;
  if (projectService()) dirty = true;              // a beat-load came due
  if (songService()) dirty = true;                 // the arranger's bar bookkeeping
  synthServiceMidi();
  static uint8_t lastStep = 255;                   // the playhead and clock follow the transport
  if (synth.seqOn() && synth.seqStepN() != lastStep) { lastStep = synth.seqStepN(); dirty = true; }
  if (dirty) { drawScreen(); dirty = false; }

  bool hp;
  if (board.headphonesChanged(hp)) board.setSpeaker(!hp);
  board.chargeGuard();
  if (board.powerButtonPressed()) { codec.volume(0.0f); projectSaveCurrent(); board.powerOff(); }

  static bool announced = false;
  if (!announced && millis() > 2000) { announced = true; Serial.println("k6: up"); }

  // The synth's cost on the serial log, once a second while it plays.
  static uint32_t logAt = 0;
  if (millis() - logAt >= 1000) {
    logAt = millis();
    uint32_t cyc = synth.takeMaxCycles();
    if (synth.active())
      Serial.printf("synth: %u voices, max block %lu us (%lu%% of 2902 us)\n",
                    synth.active(), cyc / 600, cyc / 600 * 100 / 2902);
  }
}
