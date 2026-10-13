// N9 — L7's sister app: nine MIDI clip tracks, each with its own synthesizer
// engine (32 voices shared). This sketch is the UI navigation prototype only:
// the six screens as static mockups, stepped with SHIFT + UP/DOWN, top to
// bottom: FX (master effects & sends), mixer, track modulation, track
// instrument, clip grid, clip editor. No audio, no storage, no editing.
//
// Design document: ../docs/design/n9_mockup.html. M8Board.cpp/.h and colors.h
// are copies of L7's (../); keep them in step.
//
// Build/upload like L7 (docs/build-and-test.md), from this folder:
//   arduino-cli compile -b teensy:avr:teensy40:usb=serialmsc --build-path <out> .
//   arduino-cli upload  -b teensy:avr:teensy40:usb=serialmsc -p usb:100000 --input-dir <out> .

#include "M8Board.h"
#include "n9_ui.h"

m8::Board board;

static uint8_t screenIx = SCR_GRID;   // the clip grid is home
static uint8_t prevKeys = 0;

void setup() {
  Serial.begin(115200);
  if (!board.begin()) {
    while (true) { Serial.println("key expander not answering"); delay(1000); }
  }
  // The whole screen is redrawn on every switch: draw into the frame buffer
  // and send it whole, so switching doesn't flicker. (L7 draws piecemeal
  // instead and needs its RAM; N9's prototype has plenty.)
  board.tft().useFrameBuffer(true);
  drawScreen(screenIx);
}

void loop() {
  uint8_t now;
  if (board.keysChanged(now)) {
    uint8_t pressed = now & ~prevKeys;
    prevKeys = now;
    if (now & m8::BTN_SHIFT) {
      if ((pressed & m8::BTN_UP) && screenIx > 0) drawScreen(--screenIx);
      if ((pressed & m8::BTN_DOWN) && screenIx < SCR_COUNT - 1) drawScreen(++screenIx);
    }
  }
  if (board.powerButtonPressed()) board.powerOff();
}
