// touch_test.ino — touch panel test for the Dirtywave M8 Model:01
//
// Draws a circle wherever the screen is touched and shows the raw panel
// coordinates next to the mapped screen position, to check the mapping.
// Press any key to clear the screen. Most M8 units have no touch panel; then
// the screen says so.
//
// Build: Teensyduino, board "Teensy 4.0". Library: ILI9341_t3n (KurtE).

#include "M8Board.h"

m8::Board board;
static bool hasTouch = false;

static void drawFrame() {
  auto &t = board.tft();
  t.fillScreen(ILI9341_BLACK);
  t.setTextSize(1);
  t.setTextColor(ILI9341_DARKGREY);
  t.setCursor(4, 228);
  if (hasTouch) {
    t.printf("touch chip id 0x%02X - touch to draw, any key clears", board.touchChipId());
  } else {
    t.setTextColor(ILI9341_RED);
    t.print("No touch controller answered at I2C 0x38.");
  }
}

static void showCoords(const m8::Board::Touch &p) {
  auto &t = board.tft();
  t.fillRect(0, 0, 320, 12, ILI9341_BLACK);
  t.setTextColor(ILI9341_WHITE);
  t.setCursor(4, 2);
  t.printf("screen %3d,%3d   raw %4u,%4u", p.x, p.y, p.rawX, p.rawY);
}

// Short press shows a prompt; keep holding ~1 s to power off.
static void handlePowerButton() {
  if (!board.powerButtonPressed()) return;
  uint32_t start = millis();
  while (board.powerButtonHeld())
    if (millis() - start >= 1000) board.powerOff();
  board.powerButtonPressed();
}

void setup() {
  Serial.begin(115200);
  board.begin();
  board.powerButtonPressed();                        // the press that switched us on
  hasTouch = board.touchBegin();
  Serial.printf("touch: %s (chip id 0x%02X)\n", hasTouch ? "found" : "none", board.touchChipId());
  drawFrame();
}

void loop() {
  handlePowerButton();

  uint8_t keys;
  if (board.keysChanged(keys) && keys) drawFrame();

  if (hasTouch) {
    static bool wasDown = false;
    m8::Board::Touch p = board.touchRead();
    if (p.down) {
      board.tft().fillCircle(p.x, p.y, 10, wasDown ? ILI9341_CYAN : ILI9341_ORANGE);
      showCoords(p);
      Serial.printf("screen %d,%d raw %u,%u\n", p.x, p.y, p.rawX, p.rawY);
    }
    wasDown = p.down;
  }
  delay(10);
}
