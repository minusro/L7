// K6 — a fresh six-track clip machine for the M8 hardware. Navigation
// prototype: the 3x2 screen map (SONG/GRID/MIX over PROJ/NOTE/INST) with
// mock content; the engine layer (synth pool, FX, GM region) lands next.
// Design: ../docs/design/k6_mockup.html. M8Board.cpp/.h and colors.h are
// copies of L7's; keep them in step.
//
// Build/upload (from this folder):
//   arduino-cli compile -b teensy:avr:teensy40:usb=serialmsc --build-path <out> .
//   arduino-cli upload  -b teensy:avr:teensy40:usb=serialmsc -p usb:100000 --input-dir <out> .
#include "M8Board.h"
#include "k6_input.h"
#include "k6_state.h"
#include "k6_ui.h"

m8::Board board;

void setup() {
  Serial.begin(115200);
  if (CrashReport) Serial.print(CrashReport);
  board.begin(60000000);
  board.powerButtonPressed();                    // the press that switched us on
  static DMAMEM uint16_t framebuffer[320 * 240]; // static: malloc fails silently when low
  board.tft().setFrameBuffer(framebuffer);
  if (!board.tft().useFrameBuffer(true)) Serial.println("k6: NO FRAMEBUFFER");
  splashShow();
  delay(600);
  drawScreen();
}

void loop() {
  if (inputService()) drawScreen();
  bool hp;
  if (board.headphonesChanged(hp)) board.setSpeaker(!hp);
  board.chargeGuard();
  if (board.powerButtonPressed()) board.powerOff();
  static bool announced = false;
  if (!announced && millis() > 2000) { announced = true; Serial.println("k6: up"); }
}
