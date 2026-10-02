// ui_filter.cpp — the filter cell: the response curve (see ui.h)
#include "ui.h"

// ---- Filter cell: the response of the track's highpass + lowpass pair over
// 20 Hz - 20 kHz (log), 0 dB at the top to -CURVE_DB at the bottom, in
// blocks: a line one block thick (white while the clip plays, grey when
// muted) filled dark grey under it. Nothing on an empty track.
// The cell is built in a buffer and sent at once (no flash).
static constexpr int CELL_COLS = CELL[FILTER_CELL].w / PX;      // 39, the frame's included
static constexpr int GRAPH_COLS = CELL_COLS - 2;
static constexpr int BAND_ROWS = ROW_BAND / PX;                 // 13
static constexpr int TOP_ROW = 2, BOTTOM_ROW = 10;              // 0 dB, -CURVE_DB
static constexpr float CURVE_DB = 30;

void drawFilter(int i) {
  struct Key { int8_t lo, hi; uint16_t frame, line; };
  static Key shown[NUM_TRACKS];
  static uint32_t drawn[NUM_TRACKS];               // uiGeneration when last drawn
  const Track &tr = tracks[i];
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.lo = tr.filterLo;
  k.hi = tr.filterHi;
  k.frame = cellFrame(i, FILTER_CELL);
  k.line = !hasClip(i) ? BG                        // empty (or still recording): no curve
         : tr.state == TrackState::Playing ? INK : DIM;
  if (drawn[i] == uiGeneration && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = uiGeneration;
  shown[i] = k;

  // The curve's row (fractional) at each column's centre.
  float y[GRAPH_COLS];
  for (int c = 0; c < GRAPH_COLS; c++) {
    float hz = FILTER_MIN_HZ * powf(2.0f, FILTER_OCTAVES * (c + 0.5f) / GRAPH_COLS);
    y[c] = TOP_ROW + -filterResponseDb(k.lo, k.hi, hz) / CURVE_DB * (BOTTOM_ROW - TOP_ROW);
  }
  uint16_t blocks[BAND_ROWS][CELL_COLS];
  for (auto &row : blocks) for (auto &b : row) b = BG;
  for (int c = 0; c < CELL_COLS; c++) {
    if (k.frame != BG) {
      if (c == 0 || c == CELL_COLS - 1) for (int r = 1; r < BAND_ROWS - 1; r++) blocks[r][c] = k.frame;
      else blocks[0][c] = blocks[BAND_ROWS - 1][c] = k.frame;
    } else if (i == DIVIDER_ROW) {
      blocks[BAND_ROWS - 1][c] = DIVIDER;
    }
  }
  if (k.line != BG) {
    for (int c = 0; c < GRAPH_COLS; c++) {
      // The line: this column's row, stretched halfway to its neighbours'
      // so it stays joined where it's steep.
      float lo = y[c], hi = y[c];
      if (c > 0) { lo = min(lo, (y[c] + y[c - 1]) / 2); hi = max(hi, (y[c] + y[c - 1]) / 2); }
      if (c < GRAPH_COLS - 1) { lo = min(lo, (y[c] + y[c + 1]) / 2); hi = max(hi, (y[c] + y[c + 1]) / 2); }
      int r0 = (int)lroundf(lo), r1 = (int)lroundf(hi);
      for (int r = max(r0, TOP_ROW); r <= min(r1, BOTTOM_ROW); r++) blocks[r][c + 1] = k.line;
      for (int r = max(r1 + 1, TOP_ROW); r <= BOTTOM_ROW; r++) blocks[r][c + 1] = DARK;   // under it
    }
  }
  static uint16_t px[ROW_BAND * CELL[FILTER_CELL].w];
  const int w = CELL[FILTER_CELL].w;
  for (int r = 0; r < BAND_ROWS; r++)
    for (int c = 0; c < CELL_COLS; c++)
      for (int dy = 0; dy < PX; dy++)
        for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * w + c * PX + dx] = blocks[r][c];
  auto &t = board.tft();
  int top = rowTop(i);
  if (bgTop(i) < top) t.fillRect(CELL[FILTER_CELL].x, bgTop(i), w, top - bgTop(i), BG);   // the first row's margin
  t.writeRect(CELL[FILTER_CELL].x, top, w, ROW_BAND, px);
}
