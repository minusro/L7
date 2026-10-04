// ui_filter.cpp — the filter cell: the response curve (see ui.h)
#include "ui.h"

// ---- Filter cell: the response of the track's highpass + lowpass pair over
// 20 Hz - 20 kHz (log), 0 dB at the top to -CURVE_DB at the bottom, in
// blocks: the line, one block thick, and filled under it. Fill / line:
//   unselected: grey / light grey;  selected: light grey / white;
//   muted: dark grey, no line;
//   B held on the cell, muted or not (B + arrows change the filter): the
//   dark accent / the accent colour.
// Nothing on an empty track.
// The cell is built in a buffer and sent at once (no flash).
static constexpr int CELL_COLS = CELL[FILTER_CELL].w / PX;      // 30, the border's included
static constexpr int GRAPH_LEFT = 2;                             // the first graph column (after the border and a space)
static constexpr int GRAPH_COLS = CELL_COLS - 2 * GRAPH_LEFT;
static constexpr int BAND_ROWS = CELL_H / PX;                   // 16
static constexpr int TOP_ROW = 2, BOTTOM_ROW = BAND_ROWS - 3;   // 0 dB, -CURVE_DB (a block of space round them; rows 2-13, as the waveform)
static constexpr float CURVE_DB = 30;

void drawFilter(int i) {
  struct Key { int8_t lo, hi; bool clip; uint16_t fill, line; uint64_t borders; };
  static Key shown[NUM_TRACKS];
  static uint32_t drawn[NUM_TRACKS];               // uiGeneration when last drawn
  const Track &tr = tracks[i];
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.lo = tr.filterLo;
  k.hi = tr.filterHi;
  k.borders = bordersOf(i, FILTER_CELL);
  k.clip = hasClip(i);                             // empty (or still recording): nothing
  bool muted = tr.state == TrackState::Muted, here = i == selected && selectedCell == FILTER_CELL;
  bool editing = here && editingCell(i, FILTER_CELL);
  k.fill = editing ? ACCENT_DARK : muted ? DARK : here ? LIGHT : DIM;
  k.line = editing ? ACCENT : muted ? BG : here ? INK : LIGHT;   // (BG: none)
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
  for (int r = 0; r < BAND_ROWS; r++)             // the border (the frame), round the background
    for (int c = 0; c < CELL_COLS; c++) {
      int b = borderAt(i, FILTER_CELL, r, c, BAND_ROWS, CELL_COLS);
      blocks[r][c] = b >= 0 ? (uint16_t)b : BG;
    }
  if (k.clip) {
    for (int c = 0; c < GRAPH_COLS; c++) {
      // The line: this column's row, stretched halfway to its neighbours'
      // so it stays joined where it's steep.
      float lo = y[c], hi = y[c];
      if (c > 0) { lo = min(lo, (y[c] + y[c - 1]) / 2); hi = max(hi, (y[c] + y[c - 1]) / 2); }
      if (c < GRAPH_COLS - 1) { lo = min(lo, (y[c] + y[c + 1]) / 2); hi = max(hi, (y[c] + y[c + 1]) / 2); }
      int r0 = (int)lroundf(lo), r1 = (int)lroundf(hi);
      if (k.line != BG)
        for (int r = max(r0, TOP_ROW); r <= min(r1, BOTTOM_ROW); r++) blocks[r][c + GRAPH_LEFT] = k.line;
      int under = k.line != BG ? r1 + 1 : (int)lroundf(y[c]);   // (no line: the fill from the curve down)
      for (int r = max(under, TOP_ROW); r <= BOTTOM_ROW; r++) blocks[r][c + GRAPH_LEFT] = k.fill;
    }
  }
  static uint16_t px[CELL_H * CELL[FILTER_CELL].w];
  const int w = CELL[FILTER_CELL].w;
  for (int r = 0; r < BAND_ROWS; r++)
    for (int c = 0; c < CELL_COLS; c++)
      for (int dy = 0; dy < PX; dy++)
        for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * w + c * PX + dx] = blocks[r][c];
  auto &t = board.tft();
  int top = rowTop(i);
  t.writeRect(CELL[FILTER_CELL].x, top, w, CELL_H, px);
}
