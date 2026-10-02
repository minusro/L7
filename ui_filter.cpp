// ui_filter.cpp — the filter cell: the response curve (see ui.h)
#include "ui.h"

// ---- Filter cell: the response of the track's highpass + lowpass pair over
// 20 Hz - 20 kHz (log), 0 dB at the top to -CURVE_DB at the bottom; a line
// with a faint fill under it. Bright while the clip plays, dimmed when
// muted, nothing on an empty track. Each column is drawn whole (background,
// fill, line), so a change doesn't flash.
static constexpr int CURVE_H = 18;
static constexpr float CURVE_DB = 30, CURVE_LINE = 1.2f;   // line width in px (anti-aliased)

void drawFilter(int i) {
  struct Key { int8_t lo, hi; uint16_t bg, line; };
  static Key shown[NUM_TRACKS];
  static bool drawn[NUM_TRACKS];
  const Track &tr = tracks[i];
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.lo = tr.filterLo;
  k.hi = tr.filterHi;
  k.bg = cellBackground(i, FILTER_CELL);
  k.line = tr.state == TrackState::Playing ? CURVE
         : tr.state == TrackState::Muted ? blend565(CURVE, BG, MUTED_WEIGHT) : BG;   // empty: no curve
  if (drawn[i] && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = true;
  shown[i] = k;
  auto &t = board.tft();
  const Cell &cell = CELL[FILTER_CELL];
  const int band = rowTop(i), top = (ROW_BAND - CURVE_H) / 2, bottom = top + CURVE_H - 1;   // rows in the band
  uint16_t fill = blend565(k.line, k.bg, CURVE_FILL_WEIGHT);
  // The curve through each column's centre, in band pixel coordinates (row
  // r spans r..r+1); below the graph it keeps going down, out of sight.
  static float curveY[CELL[FILTER_CELL].w + 2];      // columns -1 .. w
  for (int c = -1; c <= cell.w; c++) {
    float hz = FILTER_MIN_HZ * powf(2.0f, FILTER_OCTAVES * (c + 0.5f) / cell.w);
    curveY[c + 1] = top + 0.5f + -filterResponseDb(k.lo, k.hi, hz) / CURVE_DB * (CURVE_H - 1);
  }
  auto yAt = [&](float x) {                        // straight between column centres
    float f = constrain(x - 0.5f, -1.0f, (float)cell.w);
    int c = min((int)floorf(f), cell.w - 1);
    return curveY[c + 1] + (curveY[c + 2] - curveY[c + 1]) * (f - c);
  };
  // Distance from (x, y) to the curve's segment between columns c and c + 1.
  auto segDist = [&](int c, float x, float y) {
    float ax = c + 0.5f, ay = curveY[c + 1], dx = 1, dy = curveY[c + 2] - ay;
    float u = constrain(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    float ex = x - (ax + u * dx), ey = y - (ay + u * dy);
    return sqrtf(ex * ex + ey * ey);
  };
  static uint16_t col[ROW_BAND];
  for (int c = 0; c < cell.w; c++) {
    for (int r = 0; r < ROW_BAND; r++) col[r] = k.bg;
    if (i == DIVIDER_ROW && k.bg == BG) col[dividerY() - band] = DIVIDER;
    if (tr.state != TrackState::Empty) {
      for (int r = top - 1; r <= bottom; r++) {
        // 4x4 samples: on the line (within CURVE_LINE / 2), or under it (fill).
        int onLine = 0, under = 0;
        for (int sx = 0; sx < 4; sx++) {
          float x = c + (sx + 0.5f) / 4, cy = yAt(x);
          for (int sy = 0; sy < 4; sy++) {
            float y = r + (sy + 0.5f) / 4;
            if (min(segDist(c - 1, x, y), segDist(c, x, y)) <= CURVE_LINE / 2) onLine++;
            else if (y > cy && y < bottom + 1) under++;
          }
        }
        if (onLine || under) {
          uint16_t base = blend565(fill, k.bg, under * 255 / (16 - onLine ? 16 - onLine : 1));
          col[r] = blend565(k.line, base, onLine * 255 / 16);
        }
      }
    }
    if (bgTop(i) < band) t.drawFastVLine(cell.x + c, bgTop(i), band - bgTop(i), k.bg);   // the first row's margin
    t.writeRect(cell.x + c, band, 1, ROW_BAND, col);
  }
}
