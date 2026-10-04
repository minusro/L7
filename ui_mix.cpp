// ui_mix.cpp — the volume / balance cell and its meters (see ui.h)
#include "ui.h"

// ---- Volume / balance cell: straight vertical lines of blocks either side
// of the cell's centre, longer further out, like waves: MIX_ARCS for the left
// channel, MIX_ARCS for the right: one per volume step (config.h). A
// channel's level lights its lines from the inside out: one per volume step,
// less one per balance step away from that side (so every press of B + an
// arrow shows). The meter (the channel's actual output, with peak hold falling
// at METER_FALL_DB_PER_S) lights them white on the same scale: line j at
// volume step j + 1's level. Up to the level / above it (the outer lines):
//   unselected: grey / dark grey;  selected: light grey / grey;
//   muted (no meter): dark grey / black.
// While B alone is held on the cell, muted or not (B + arrows change volume
// and balance), no meter: the level's lines in the accent colour, the outer
// ones grey. A line is only redrawn when its colour changes.
static constexpr int CELL_COLS = CELL[MIX_CELL].w / PX, BAND_ROWS = CELL_H / PX;   // 24 x 14, the border's included
// The lines: one in every other column, from the centre out (a column short
// of the cell's edges), 2 blocks tall at the centre and 2 taller each one
// out: as many per channel as there are volume steps.
static constexpr int MIX_ARCS = (CELL_COLS - 4) / 4;                     // 5
static_assert(MIX_ARCS == VOLUME_MAX && BALANCE_MAX == MIX_ARCS, "a line per volume step (and balance step a side)");
static constexpr float METER_FALL_DB_PER_S = 24;

static int8_t arcAt[BAND_ROWS][CELL_COLS];          // [row][col]: side * MIX_ARCS + arc, -1: none

static void buildArcs() {
  for (auto &row : arcAt) for (auto &a : row) a = -1;
  const int inner = CELL_COLS / 2 - 2;             // the left channel's innermost column (10; the right's: 13)
  for (int j = 0; j < MIX_ARCS; j++) {
    int h = 2 + 2 * j, top = (BAND_ROWS - h) / 2;  // centred in rows 1-12
    for (int side = 0; side < 2; side++) {
      int c = side ? CELL_COLS - 1 - inner + 2 * j : inner - 2 * j;
      for (int r = top; r < top + h; r++) arcAt[r][c] = side * MIX_ARCS + j;
    }
  }
}

// How many lines (0..MIX_ARCS) the meter lights at a level in dB: those
// whose volume step's level it reaches (within half a step).
static int arcsForDb(float db) {
  return constrain((int)floorf(db / VOLUME_STEP_DB + VOLUME_UNITY + 0.5f), 0, MIX_ARCS);
}
static float gainDb(float g) { return g > 0 ? 20 * log10f(g) : -200.0f; }

static void drawBlock(int i, int r, int c, uint16_t color) {
  board.tft().fillRect(CELL[MIX_CELL].x + c * PX, rowTop(i) + r * PX, PX, PX, color);
}

void drawMix(int i) {
  static bool built = false;
  if (!built) { buildArcs(); built = true; }
  static uint64_t shownFrame[NUM_TRACKS];
  static uint16_t shown[NUM_TRACKS][2 * MIX_ARCS];   // each line's colour
  static uint32_t drawn[NUM_TRACKS];               // uiGeneration when last drawn whole
  static float meterDb[NUM_TRACKS][2];
  static uint32_t lastMs[NUM_TRACKS];

  const Track &tr = tracks[i];
  // Meter: the newest peak, or the previous reading falling.
  float peak[2];
  takeTrackPeaks(i, peak[0], peak[1]);
  uint32_t now = millis();
  float fall = (now - lastMs[i]) * (METER_FALL_DB_PER_S / 1000);
  lastMs[i] = now;
  for (int ch = 0; ch < 2; ch++) meterDb[i][ch] = max(gainDb(peak[ch]), meterDb[i][ch] - fall);
  if (tr.state == TrackState::Muted || !transport.running)   // muted or stopped: no meter at once
    meterDb[i][0] = meterDb[i][1] = -200;                       // (not the peak hold falling)

  uint64_t frame = bordersOf(i, MIX_CELL);
  uint16_t looks[2 * MIX_ARCS];                    // each line's colour
  int levels[2] = {tr.volume - max(0, (int)tr.balance), tr.volume - max(0, -(int)tr.balance)};   // lines lit
  bool muted = tr.state == TrackState::Muted, here = i == selected && selectedCell == MIX_CELL;
  bool editing = here && bAlone();
  uint16_t levelInk = editing ? ACCENT : muted ? DARK : here ? LIGHT : DIM;
  uint16_t outerInk = editing ? DIM : muted ? BG : here ? DIM : DARK;
  for (int ch = 0; ch < 2; ch++) {
    int level = levels[ch], meter = editing ? 0 : arcsForDb(meterDb[i][ch]);
    for (int j = 0; j < MIX_ARCS; j++)
      looks[ch * MIX_ARCS + j] = !hasClip(i) ? BG : j < meter ? INK : j < level ? levelInk : outerInk;
  }

  auto &t = board.tft();
  const Cell &cell = CELL[MIX_CELL];
  int top = rowTop(i);
  if (drawn[i] != uiGeneration || shownFrame[i] != frame) {
    // Whole cell: built in a buffer and sent at once (no flash).
    static uint16_t px[CELL_H * CELL[MIX_CELL].w];
    for (int r = 0; r < BAND_ROWS; r++)
      for (int c = 0; c < CELL_COLS; c++) {
        int b = borderAt(i, MIX_CELL, r, c, BAND_ROWS, CELL_COLS);   // the border (the frame)
        uint16_t color = b >= 0 ? (uint16_t)b : BG;
        if (arcAt[r][c] >= 0) color = looks[arcAt[r][c]];
        for (int dy = 0; dy < PX; dy++)
          for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * cell.w + c * PX + dx] = color;
      }
    t.writeRect(cell.x, top, cell.w, CELL_H, px);
    drawn[i] = uiGeneration;
    shownFrame[i] = frame;
    memcpy(shown[i], looks, sizeof looks);
    return;
  }
  for (int a = 0; a < 2 * MIX_ARCS; a++) {
    if (looks[a] == shown[i][a]) continue;
    shown[i][a] = looks[a];
    for (int r = 0; r < BAND_ROWS; r++)
      for (int c = 0; c < CELL_COLS; c++)
        if (arcAt[r][c] == a) drawBlock(i, r, c, looks[a]);
  }
}
