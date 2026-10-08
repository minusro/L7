// ui_mix.cpp — the volume / balance cell and its meters (see ui.h)
#include "ui.h"

// ---- Volume / balance cell: straight vertical lines of blocks either side
// of the cell's centre, longer further out, like waves: MIX_ARCS for the left
// channel, MIX_ARCS for the right, each two 1-pixel halves (HALVES a channel).
// A channel's level lights its halves from the inside out: one per two volume
// steps (config.h; a step past: one more), less one per balance step away
// from that side. The meter (the channel's actual output, with peak hold
// falling at METER_FALL_DB_PER_S) lights them white on the same scale: half h
// at volume step 2 (h + 1)'s level. Up to the level / above it (the outer ones):
//   unselected: grey / dark grey;  the selected track: light grey / grey;
//   muted (no meter): dark grey / black.
// While B is held on the cell (with other keys or not), muted or not (B + arrows change volume
// and balance): the level's halves in the accent colour, the outer ones grey,
// and the meter white over them as ever (unmuted). A half is only redrawn when its colour changes.
static constexpr int CELL_COLS = CELL[MIX_CELL].w / PX, BAND_ROWS = CELL_H / PX;   // 24 x 16, the border's included
// The lines: one in every other column, from the centre out (a column short
// of the cell's edges), 2 taller each one out, the outermost the waveform's
// height (rows 2-13).
static constexpr int MIX_ARCS = (CELL_COLS - 4) / 4;                     // 5
static constexpr int HALVES = 2 * MIX_ARCS;                              // 10 a channel
static_assert(2 * HALVES == VOLUME_MAX && BALANCE_MAX == HALVES, "a half per two volume steps, and per balance step");
static constexpr float METER_FALL_DB_PER_S = 24;

static int8_t arcAt[BAND_ROWS][CELL_COLS];          // [row][col]: side * MIX_ARCS + arc, -1: none
// A line's pixel column dx (0, 1) of side `side`: its half, from the inside out.
static int halfOf(int side, int arc, int dx) { return 2 * arc + (side ? dx : 1 - dx); }

static void buildArcs() {
  for (auto &row : arcAt) for (auto &a : row) a = -1;
  const int inner = CELL_COLS / 2 - 2;             // the left channel's innermost column (10; the right's: 13)
  for (int j = 0; j < MIX_ARCS; j++) {
    int h = BAND_ROWS - 4 - 2 * (MIX_ARCS - 1 - j), top = (BAND_ROWS - h) / 2;   // centred (4 to 12 tall)
    for (int side = 0; side < 2; side++) {
      int c = side ? CELL_COLS - 1 - inner + 2 * j : inner - 2 * j;
      for (int r = top; r < top + h; r++) arcAt[r][c] = side * MIX_ARCS + j;
    }
  }
}

// How many halves (0..HALVES) the meter lights at a level in dB: those whose
// level (two volume steps each) it reaches (within half a half).
static int halvesForDb(float db) {
  return constrain((int)floorf((db / VOLUME_STEP_DB + VOLUME_UNITY) / 2 + 0.5f), 0, HALVES);
}
static float gainDb(float g) { return g > 0 ? 20 * log10f(g) : -200.0f; }

static void drawHalf(int i, int r, int c, int dx, uint16_t color) {   // (block r, c's pixel column dx)
  board.tft().fillRect(CELL[MIX_CELL].x + c * PX + dx, rowTop(i) + r * PX, 1, PX, color);
}

void drawMix(int i) {
  if (spanRow(i)) return;                          // (an empty track: one cell, drawSpan())
  static bool built = false;
  if (!built) { buildArcs(); built = true; }
  static uint64_t shownFrame[NUM_TRACKS];
  static uint16_t shown[NUM_TRACKS][2 * HALVES];      // each half's colour
  static uint32_t drawn[NUM_TRACKS];               // uiGeneration when last drawn whole
  static float meterDb[NUM_TRACKS][2];
  static uint32_t lastMs[NUM_TRACKS];

  const ShownTrack &tr = shownTrack(i);           // (as the row shows it: a switch all at once)
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
  uint16_t looks[2 * HALVES];                      // each half's colour
  const int volumeHalves = (tr.volume + 1) / 2;    // (a step past a half: that one too)
  int levels[2] = {volumeHalves - max(0, (int)tr.balance), volumeHalves - max(0, -(int)tr.balance)};   // halves lit
  bool muted = tr.state == TrackState::Muted, here = i == selected;   // (the selected track: all its cells look selected)
  bool editing = editingCell(i, MIX_CELL);         // (B held, with other keys or not: as the filter cell)
  uint16_t ground = rowGround(i);
  uint16_t levelInk = onGround(editing ? ACCENT : muted ? DARK : here ? LIGHT : DIM, ground);
  uint16_t outerInk = editing ? DIM : muted ? ground : here ? DIM : DARK;   // (muted: none)
  for (int ch = 0; ch < 2; ch++) {
    int level = levels[ch], meter = halvesForDb(meterDb[i][ch]);   // (muted: none)
    for (int h = 0; h < HALVES; h++)
      looks[ch * HALVES + h] = !shownClip(i) ? ground : h < meter ? INK : h < level ? levelInk : outerInk;
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
        const int a = arcAt[r][c];
        for (int dx = 0; dx < PX; dx++) {
          uint16_t color = a >= 0 ? looks[(a / MIX_ARCS) * HALVES + halfOf(a / MIX_ARCS, a % MIX_ARCS, dx)]
                         : b >= 0 ? (uint16_t)b : ground;
          for (int dy = 0; dy < PX; dy++) px[(r * PX + dy) * cell.w + c * PX + dx] = color;
        }
      }
    t.writeRect(cell.x, top, cell.w, CELL_H, px);
    drawn[i] = uiGeneration;
    shownFrame[i] = frame;
    memcpy(shown[i], looks, sizeof looks);
    return;
  }
  for (int k = 0; k < 2 * HALVES; k++) {           // (k: side * HALVES + half)
    if (looks[k] == shown[i][k]) continue;
    shown[i][k] = looks[k];
    const int side = k / HALVES, arc = (k % HALVES) / 2;
    const int dx = halfOf(side, arc, 0) == k % HALVES ? 0 : 1;   // (its pixel column in the line's blocks)
    for (int r = 0; r < BAND_ROWS; r++)
      for (int c = 0; c < CELL_COLS; c++)
        if (arcAt[r][c] == side * MIX_ARCS + arc) drawHalf(i, r, c, dx, looks[k]);
  }
}
