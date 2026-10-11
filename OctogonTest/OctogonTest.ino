// OctogonTest — a sketch of its own (in the L7 project's folder), for trying
// out an idea on the M8: a light grey regular octagon inside a grey one twice
// its size, both centred, on a light grey ground.
//
// Uses the L7 project's board code: M8Board.* here are links to the ones one folder up.
#include "M8Board.h"
#include <math.h>

m8::Board board;

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
static constexpr uint16_t LIGHT = rgb565(208, 208, 208);   // light grey (L7's GREY_81)
static constexpr uint16_t GREY = rgb565(112, 112, 112);    // grey (L7's GREY_44)

// A regular octagon centred on (cx, cy), flat sides at the top, bottom, left
// and right, `a` px from its centre to each side (its apothem): row by row,
// |x| <= a and |x| + |y| <= a * sqrt(2).
static void fillOctagon(int cx, int cy, float a, uint16_t color) {
  auto &t = board.tft();
  const float diag = a * 1.41421356f;
  for (int y = (int)ceilf(-a); y <= (int)floorf(a); y++) {
    float half = fminf(a, diag - fabsf((float)y));
    int w = (int)lroundf(half);
    t.fillRect(cx - w, cy + y, 2 * w + 1, 1, color);
  }
}

void setup() {
  board.begin();
  auto &t = board.tft();
  t.fillScreen(LIGHT);
  const int cx = 160, cy = 120;
  const float outer = 110;                         // (the screen's height, nearly: 220 px across its flats)
  fillOctagon(cx, cy, outer, GREY);
  fillOctagon(cx, cy, outer / 2, LIGHT);           // (half its size, inside it)
}

void loop() {}
