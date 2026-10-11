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

// A regular octagon centred on (cx, cy), `a` px from its centre to each side
// (its apothem), turned `turn` degrees (0: flat sides at the top, bottom, left
// and right): filled a row at a time, each row between where it crosses the
// edges (it's convex).
static void fillOctagon(int cx, int cy, float a, float turn, uint16_t color) {
  auto &t = board.tft();
  const float r = a / cosf(M_PI / 8);              // (centre to a corner)
  float vx[8], vy[8];
  for (int k = 0; k < 8; k++) {
    float ang = (22.5f + 45 * k + turn) * (float)M_PI / 180;
    vx[k] = cx + r * cosf(ang);
    vy[k] = cy + r * sinf(ang);
  }
  for (int y = (int)floorf(cy - r); y <= (int)ceilf(cy + r); y++) {
    float yc = y + 0.5f, lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < 8; k++) {
      float x0 = vx[k], y0 = vy[k], x1 = vx[(k + 1) % 8], y1 = vy[(k + 1) % 8];
      if ((yc < y0) == (yc < y1)) continue;        // (this edge doesn't cross the row)
      float x = x0 + (yc - y0) * (x1 - x0) / (y1 - y0);
      lo = fminf(lo, x);
      hi = fmaxf(hi, x);
    }
    if (hi < lo) continue;
    int xl = (int)lroundf(lo), xr = (int)lroundf(hi);
    t.fillRect(xl, y, xr - xl, 1, color);
  }
}

static constexpr float TURN = 22.5f;               // (degrees: a corner at the top)

void setup() {
  board.begin();
  auto &t = board.tft();
  t.fillScreen(LIGHT);
  const int cx = 160, cy = 120;
  const float outer = 110 * cosf(M_PI / 8);        // (turned: its corners, top and bottom, 110 px from the centre)
  fillOctagon(cx, cy, outer, TURN, GREY);
  fillOctagon(cx, cy, outer / 2, TURN, LIGHT);     // (half its size, inside it)
}

void loop() {}
