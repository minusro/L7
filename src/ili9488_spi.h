// ili9488_spi.h — the Model:02's panel: an ILI9488, 480 x 320, over SPI, 18
// bits a pixel (it has no 16-bit mode over SPI: every RGB565 pixel L7 draws
// goes out as three bytes). Only what L7 asks of a display (ui.h: fillRect,
// writeRect, setClipRect, fillScreen) and what M8Board needs to bring it up,
// with the init table and rotation read out of the stock Model:02 firmware
// (docs/HARDWARE.md). Plain SPI library transfers: no DMA, no frame buffer.
// Untested on hardware - there is no Model:02 here.
#pragma once
#include <Arduino.h>
#include <SPI.h>

class ILI9488Spi {
public:
  // cs == dc: no chip-select line is driven (the panel's CS tied, the stock
  // firmware's own setting - HARDWARE.md, "DC and CS both 10"); rst 255: none.
  ILI9488Spi(SPIClass &spi, uint8_t cs, uint8_t dc, uint8_t rst)
    : _spi(spi), _cs(cs), _dc(dc), _rst(rst) {}
  void begin(uint32_t hz);                  // reset, the init table, sleep out, display on; portrait until setRotation()
  void setRotation(uint8_t r);              // 0-3 as ILI9341_t3n's; 1 and 3 landscape, 480 x 320 (the stock firmware: 3)
  void invertDisplay(bool on);
  void setClipRect(int16_t x, int16_t y, int16_t w, int16_t h);
  void setClipRect() { _cx = 0; _cy = 0; _cw = _w; _ch = _h; }   // the whole screen
  void fillScreen(uint16_t c) { fillRect(0, 0, _w, _h, c); }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c);
  void writeRect(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px);   // RGB565, w x h, row by row
  void sendCmd(uint8_t cmd, const uint8_t *data, uint8_t n);   // one command with its data (its own transaction)
  int16_t width() const { return _w; }
  int16_t height() const { return _h; }

  // Text, as the sketch's start-up and fatal messages use it (ILI9341_t3n's
  // calls): the 5 x 7 font at a size, each character its 6 x 8 cell in the
  // text colour on its background, from the cursor, which moves on; no
  // wrapping (setTextWrap(true) is accepted and ignored).
  void setTextColor(uint16_t fg, uint16_t bg) { _fg = fg; _bg = bg; }
  void setTextSize(uint8_t s) { _ts = s ? s : 1; }
  void setCursor(int16_t x, int16_t y) { _cx0 = x; _cy0 = y; }
  void setTextWrap(bool) {}
  void print(const char *s) { while (*s) drawChar(*s++); }
  void printf(const char *fmt, ...);

private:
  void start();                             // a transaction: the clock, CS low (if there is one)
  void stop();
  void cmd(uint8_t c);                      // D/C low for the byte, high after
  void data(const uint8_t *p, size_t n) { _spi.transfer(p, nullptr, n); }
  void window(int16_t x0, int16_t y0, int16_t x1, int16_t y1);   // CASET, PASET, RAMWR: pixels follow
  // The rect clipped to the clip rect, in place; false: nothing left of it.
  bool clip(int16_t &x, int16_t &y, int16_t &w, int16_t &h, int16_t &dx, int16_t &dy) const;

  void drawChar(char c);

  SPIClass &_spi;
  uint8_t _cs, _dc, _rst;
  uint16_t _fg = 0xFFFF, _bg = 0;
  uint8_t _ts = 1;
  int16_t _cx0 = 0, _cy0 = 0;
  uint32_t _hz = 30000000;
  int16_t _w = 320, _h = 480;               // (portrait until setRotation())
  int16_t _cx = 0, _cy = 0, _cw = 320, _ch = 480;
};
