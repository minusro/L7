// ili9488_spi.cpp — see ili9488_spi.h. Only in a build for the Model:02's
// board; the Model:01 and the host builds compile it to nothing.
#if defined(ARDUINO_TEENSY_MICROMOD)
#include "ili9488_spi.h"
#include "glcdfont5x7.h"
#include <stdarg.h>

// The init sequence the stock Model:02 firmware sets the panel up with
// (docs/HARDWARE.md: the live table, read out of its .data), verbatim:
// a length (the command and its data), the command, the data.
static const uint8_t INIT[] = {
  3, 0xC0, 0x01, 0x1F,                 // power control 1
  2, 0xC1, 0x40,                       // power control 2
  2, 0xC2, 0x00,                       // power control 3
  4, 0xC5, 0x00, 0x48, 0x80,           // VCOM control
  2, 0x36, 0x48,                       // MADCTL (setRotation() sets it again)
  2, 0x3A, 0x66,                       // COLMOD: 18 bits a pixel
  2, 0xB0, 0x00,                       // interface mode control
  2, 0xB1, 0xC0,                       // frame rate control
  2, 0xB4, 0x02,                       // display inversion control
  3, 0xB6, 0x02, 0x02,                 // display function control
  16, 0xE0, 0x00, 0x08, 0x0C, 0x02, 0x0E, 0x04, 0x30, 0x45, 0x47, 0x04, 0x0C, 0x0A, 0x2E, 0x34, 0x0F,   // positive gamma
  16, 0xE1, 0x00, 0x11, 0x0D, 0x01, 0x0F, 0x05, 0x39, 0x36, 0x51, 0x06, 0x0F, 0x0D, 0x33, 0x37, 0x0F,   // negative gamma
  2, 0xE9, 0x00,                       // set image function
  5, 0xF7, 0xA9, 0x51, 0x2C, 0x82,     // adjust control 3
  3, 0xF8, 0x21, 0x07,                 // adjust control 4
  0
};

// MADCTL for each rotation, as ILI9488_t3 has them (MX 0x40, MY 0x80, MV 0x20, BGR 0x08).
static const uint8_t MADCTL[4] = {0x48, 0x28, 0x88, 0xE8};

// A row's worth of 18-bit pixels: the most L7 sends at once is a screen's width.
static constexpr int ROW_PX = 480;
static uint8_t rowBuf[ROW_PX * 3];

static inline void px666(uint16_t c, uint8_t *o) {   // RGB565 -> the three bytes (the top 6 bits of each count)
  o[0] = (c & 0xF800) >> 8;
  o[1] = (c & 0x07E0) >> 3;
  o[2] = (c & 0x001F) << 3;
}

void ILI9488Spi::start() {
  _spi.beginTransaction(SPISettings(_hz, MSBFIRST, SPI_MODE0));
  if (_cs != _dc) digitalWriteFast(_cs, LOW);
}
void ILI9488Spi::stop() {
  if (_cs != _dc) digitalWriteFast(_cs, HIGH);
  _spi.endTransaction();
}
void ILI9488Spi::cmd(uint8_t c) {
  digitalWriteFast(_dc, LOW);
  _spi.transfer(c);
  digitalWriteFast(_dc, HIGH);
}

void ILI9488Spi::begin(uint32_t hz) {
  _hz = hz;
  pinMode(_dc, OUTPUT);
  digitalWriteFast(_dc, HIGH);
  if (_cs != _dc) {
    pinMode(_cs, OUTPUT);
    digitalWriteFast(_cs, HIGH);
  }
  if (_rst != 255) {                        // the stock driver's pulse: 5 ms high, 20 low, 150 high
    pinMode(_rst, OUTPUT);
    digitalWriteFast(_rst, HIGH);
    delay(5);
    digitalWriteFast(_rst, LOW);
    delay(20);
    digitalWriteFast(_rst, HIGH);
    delay(150);
  }
  _spi.begin();
  start();
  for (const uint8_t *p = INIT; *p; p += 1 + *p) {
    cmd(p[1]);
    data(p + 2, *p - 1);
  }
  cmd(0x11);                                // sleep out
  stop();
  delay(120);
  start();
  cmd(0x29);                                // display on
  stop();
}

void ILI9488Spi::sendCmd(uint8_t c, const uint8_t *d, uint8_t n) {
  start();
  cmd(c);
  if (n) data(d, n);
  stop();
}

void ILI9488Spi::setRotation(uint8_t r) {
  r &= 3;
  bool landscape = r & 1;
  _w = landscape ? 480 : 320;
  _h = landscape ? 320 : 480;
  setClipRect();
  sendCmd(0x36, &MADCTL[r], 1);
}

void ILI9488Spi::invertDisplay(bool on) { sendCmd(on ? 0x21 : 0x20, nullptr, 0); }

void ILI9488Spi::setClipRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > _w) w = _w - x;
  if (y + h > _h) h = _h - y;
  _cx = x; _cy = y; _cw = max(w, (int16_t)0); _ch = max(h, (int16_t)0);
}

bool ILI9488Spi::clip(int16_t &x, int16_t &y, int16_t &w, int16_t &h, int16_t &dx, int16_t &dy) const {
  int16_t x0 = max(x, _cx), y0 = max(y, _cy);
  int16_t x1 = min((int)x + w, (int)_cx + _cw), y1 = min((int)y + h, (int)_cy + _ch);
  if (x1 <= x0 || y1 <= y0) return false;
  dx = x0 - x; dy = y0 - y;                 // (how far into the caller's rect the visible part starts)
  x = x0; y = y0; w = x1 - x0; h = y1 - y0;
  return true;
}

void ILI9488Spi::window(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  uint8_t c[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t p[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  cmd(0x2A); data(c, 4);
  cmd(0x2B); data(p, 4);
  cmd(0x2C);
}

void ILI9488Spi::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  int16_t dx, dy;
  if (!clip(x, y, w, h, dx, dy)) return;
  uint8_t px[3];
  px666(color, px);
  int chunk = min((int)w * h, ROW_PX);     // the buffer filled once, sent as often as needed
  for (int i = 0; i < chunk; i++) memcpy(rowBuf + 3 * i, px, 3);
  start();
  window(x, y, x + w - 1, y + h - 1);
  for (int left = w * h; left > 0; left -= chunk) data(rowBuf, 3 * min(left, chunk));
  stop();
}

void ILI9488Spi::writeRect(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px) {
  const int16_t stride = w;
  int16_t dx, dy;
  if (!clip(x, y, w, h, dx, dy)) return;
  start();
  window(x, y, x + w - 1, y + h - 1);
  for (int16_t r = 0; r < h; r++) {
    const uint16_t *row = px + (dy + r) * stride + dx;
    for (int16_t i = 0; i < w; i++) px666(row[i], rowBuf + 3 * i);
    data(rowBuf, 3 * w);
  }
  stop();
}

// A character: its 5 columns and a blank one, 8 rows, each dot a size x size
// block, built as one writeRect (the background included, so a line printed
// over an old one leaves nothing of it).
void ILI9488Spi::drawChar(char ch) {
  static uint16_t cell[6 * 8 * 4 * 4];             // (up to size 4)
  const uint8_t s = min(_ts, (uint8_t)4);
  const int w = 6 * s, h = 8 * s;
  const uint8_t *g = GLCDFONT5X7 + (uint8_t)ch * 5;
  for (int col = 0; col < 6; col++) {
    uint8_t bits = col < 5 ? g[col] : 0;
    for (int row = 0; row < 8; row++) {
      uint16_t c = (bits >> row) & 1 ? _fg : _bg;
      for (int dy = 0; dy < s; dy++)
        for (int dx = 0; dx < s; dx++) cell[(row * s + dy) * w + col * s + dx] = c;
    }
  }
  writeRect(_cx0, _cy0, w, h, cell);
  _cx0 += w;
}

void ILI9488Spi::printf(const char *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  print(buf);
}

#endif  // ARDUINO_TEENSY_MICROMOD
