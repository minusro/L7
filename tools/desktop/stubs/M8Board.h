// Stand-in for M8Board.h in the desktop app: the display draws into a screen-sized (screen.h)
// RGB565 frame buffer (desktop.cpp shows it in a window), the keys come from
// the keyboard (desktop.cpp sets heldKeys).
#pragma once
#include "screen.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#define PROGMEM
#include "Audio.h"

extern "C" const unsigned char glcdfont[];

namespace m8 {

enum : uint8_t {
  BTN_EDIT  = 1 << 0,
  BTN_OPT   = 1 << 1,
  BTN_RIGHT = 1 << 2,
  BTN_PLAY  = 1 << 3,
  BTN_SHIFT = 1 << 4,
  BTN_DOWN  = 1 << 5,
  BTN_UP    = 1 << 6,
  BTN_LEFT  = 1 << 7,
};

class Display {
public:
  static constexpr int W = SCREEN_W, H = SCREEN_H;
  uint16_t px[W * H] = {};
  bool dirty = true;                               // (drawn on since the window last showed it)
  void drawPixel(int x, int y, uint16_t c) {
    x += ox_; y += oy_;
    if (x >= 0 && x < W && y >= 0 && y < H) { px[y * W + x] = c; dirty = true; }
  }
  void setOrigin(int x = 0, int y = 0) { ox_ = x; oy_ = y; }
  void setClipRect(int, int, int, int) {}           // (the device's clipping: not modelled)
  void setClipRect() {}
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) drawPixel(x + i, y + j, c);
  }
  void fillScreen(uint16_t c) { fillRect(0, 0, W, H, c); }
  void drawFastVLine(int x, int y, int h, uint16_t c) { fillRect(x, y, 1, h, c); }
  void drawFastHLine(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }
  void drawRect(int x, int y, int w, int h, uint16_t c) {
    drawFastHLine(x, y, w, c); drawFastHLine(x, y + h - 1, w, c);
    drawFastVLine(x, y, h, c); drawFastVLine(x + w - 1, y, h, c);
  }
  void writeRect(int x, int y, int w, int h, const uint16_t *p) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) drawPixel(x + i, y + j, p[j * w + i]);
  }
  // Text: the 5x7 font, size 1, opaque background.
  void setTextColor(uint16_t fg, uint16_t bg) { fg_ = fg; bg_ = bg; }
  void setTextSize(int) {}
  void setTextWrap(bool) {}
  void setCursor(int x, int y) { cx_ = x; cy_ = y; }
  void print(const char *s) { for (; *s; s++) putc(*s); }
  void printf(const char *f, ...) {
    char b[128]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); print(b);
  }
private:
  uint16_t fg_ = 0xFFFF, bg_ = 0;
  int ox_ = 0, oy_ = 0;                            // (setOrigin())
  int cx_ = 0, cy_ = 0;
  void putc(char ch) {
    for (int c = 0; c < 6; c++) {
      uint8_t bits = c < 5 ? glcdfont[(uint8_t)ch * 5 + c] : 0;
      for (int r = 0; r < 8; r++) drawPixel(cx_ + c, cy_ + r, bits >> r & 1 ? fg_ : bg_);
    }
    cx_ += 6;
  }
};

class Board {
public:
  bool begin(uint32_t = 0) { return true; }
  Display &tft() { return display; }
  uint8_t keys() { return heldKeys; }
  bool keysChanged(uint8_t &k) {
    if (heldKeys == lastKeys) return false;
    k = lastKeys = heldKeys;
    return true;
  }
  void setBacklight(uint8_t) {}
  bool headphonesChanged(bool &in) { if (hpReported) return false; hpReported = true; in = true; return true; }
  void setSpeaker(bool) {}
  int batteryPercent() { return battery; }
  bool batteryCharging() const { return charging; }
  void batteryLog() {}
  void chargeGuard() {}
  HardwareSerial *midiOutBegin() { return &midi; }
  HardwareSerial *midiInBegin() { return &midi; }
  bool powerButtonPressed() { return false; }
  bool powerButtonHeld() { return false; }
  [[noreturn]] void powerOff() { exit(0); }
  Display display;
  HardwareSerial midi;
  uint8_t heldKeys = 0, lastKeys = 0;
  int battery = 100;
  bool charging = true;
  bool hpReported = false;
};

}  // namespace m8
