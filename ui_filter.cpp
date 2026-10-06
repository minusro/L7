// ui_filter.cpp — the filter cell: the response curve (see ui.h)
#include "ui.h"

// ---- Filter cell: the response of the track's highpass + lowpass pair over
// 20 Hz - 20 kHz (log), 0 dB at the top to -CURVE_DB at the bottom, drawn in
// pixels (the frame in blocks): the line, 2 pixels thick, and filled under it.
// Fill / line:
//   unselected: grey / light grey;  the selected track: light grey / white;
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
  bool muted = tr.state == TrackState::Muted, here = i == selected;   // (the selected track: all its cells look selected)
  bool editing = here && editingCell(i, FILTER_CELL);
  k.fill = editing ? ACCENT_DARK : muted ? DARK : here ? LIGHT : DIM;
  k.line = editing ? ACCENT : muted ? BG : here ? INK : LIGHT;   // (BG: none)
  if (drawn[i] == uiGeneration && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = uiGeneration;
  shown[i] = k;

  uint16_t blocks[BAND_ROWS][CELL_COLS];
  for (int r = 0; r < BAND_ROWS; r++)             // the border (the frame), round the background
    for (int c = 0; c < CELL_COLS; c++) {
      int b = borderAt(i, FILTER_CELL, r, c, BAND_ROWS, CELL_COLS);
      blocks[r][c] = b >= 0 ? (uint16_t)b : BG;
    }
  static uint16_t px[CELL_H * CELL[FILTER_CELL].w];
  const int w = CELL[FILTER_CELL].w;
  for (int r = 0; r < BAND_ROWS; r++)
    for (int c = 0; c < CELL_COLS; c++)
      for (int dy = 0; dy < PX; dy++)
        for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * w + c * PX + dx] = blocks[r][c];
  if (k.clip) {
    // The graph in pixels: columns from GRAPH_LEFT's, rows 2-13's pixels; the
    // curve's top pixel row (fractional) at each column's centre.
    constexpr int X0 = GRAPH_LEFT * PX, COLS = GRAPH_COLS * PX, Y0 = TOP_ROW * PX, Y1 = (BOTTOM_ROW + 1) * PX - 1, LINE = 2;
    // The curve is worked out a column past each side too (and below the graph
    // as far as it goes), so where it leaves the graph the line runs on to the
    // graph's edge at its full thickness; everything is then masked to the
    // graph (a block of space left before the frame on every side).
    float ys[COLS + 2];
    for (int c = -1; c <= COLS; c++) {
      float hz = FILTER_MIN_HZ * powf(2.0f, FILTER_OCTAVES * (c + 0.5f) / COLS);
      ys[c + 1] = Y0 + -filterResponseDb(k.lo, k.hi, hz) / CURVE_DB * (Y1 - (LINE - 1) - Y0);
    }
    auto y = [&](int c) { return ys[c + 1]; };
    for (int c = 0; c < COLS; c++) {
      // The line: this column's row, stretched halfway to its neighbours'
      // so it stays joined where it's steep.
      float lo = min(y(c), min((y(c) + y(c - 1)) / 2, (y(c) + y(c + 1)) / 2));
      float hi = max(y(c), max((y(c) + y(c - 1)) / 2, (y(c) + y(c + 1)) / 2));
      int r0 = (int)lroundf(lo), r1 = (int)lroundf(hi) + LINE - 1;
      if (k.line != BG)
        for (int r = max(r0, Y0); r <= min(r1, Y1); r++) px[r * w + X0 + c] = k.line;
      int under = k.line != BG ? r1 + 1 : (int)lroundf(y(c));   // (no line: the fill from the curve down)
      for (int r = max(under, Y0); r <= Y1; r++) px[r * w + X0 + c] = k.fill;
    }
  }
  auto &t = board.tft();
  int top = rowTop(i);
  t.writeRect(CELL[FILTER_CELL].x, top, w, CELL_H, px);
}
