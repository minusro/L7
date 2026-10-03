// ui_mix.cpp — the volume / balance cell and its meters (see ui.h)
#include "ui.h"

// ---- Volume / balance cell: concentric arcs of blocks around the cell's
// centre, ((( for the left channel and ))) for the right, MIX_ARCS each. A
// channel's level lights its arcs from the inside out, over
// MIX_FLOOR_DB..MIX_TOP_DB: the volume, plus what the balance moves onto that
// side (up to the mono sum, +6 dB) or takes away from it. The meter (the
// channel's actual output, with peak hold falling at METER_FALL_DB_PER_S)
// lights them white on the same scale. Arcs above both: dark grey; up to the
// level: 50% grey. While B alone is held on the cell (B + arrows change volume
// and balance) the meter is in the accent colour, the level's arcs in it at
// half brightness. An arc is only redrawn when its look changes.
static constexpr int CELL_COLS = CELL[MIX_CELL].w / PX, BAND_ROWS = ROW_BAND / PX;   // 39 x 13
static constexpr int MIX_ARCS = 4;
static constexpr float MIX_R0 = 4, MIX_DR = 3.5f, MIX_HALF_H = 5, MIX_SLOPE = 1.2f;   // in blocks
static constexpr float MIX_FLOOR_DB = -40, MIX_TOP_DB = 12, METER_FALL_DB_PER_S = 24;

static int8_t arcAt[BAND_ROWS][CELL_COLS];          // [row][col]: side * MIX_ARCS + arc, -1: none

static void buildArcs() {
  const float cx = CELL_COLS / 2.0f, cy = BAND_ROWS / 2.0f;
  for (int r = 0; r < BAND_ROWS; r++)
    for (int c = 0; c < CELL_COLS; c++) {
      arcAt[r][c] = -1;
      if (r == 0 || r == BAND_ROWS - 1 || c == 0 || c == CELL_COLS - 1) continue;   // the frame's
      float dx = c + 0.5f - cx, dy = r + 0.5f - cy;
      if (fabsf(dy) > MIX_HALF_H || fabsf(dy) > fabsf(dx) * MIX_SLOPE) continue;
      float d = sqrtf(dx * dx + dy * dy);
      int j = (int)lroundf((d - MIX_R0) / MIX_DR);
      if (j < 0 || j >= MIX_ARCS || fabsf(d - (MIX_R0 + j * MIX_DR)) > 0.55f) continue;
      arcAt[r][c] = (dx < 0 ? 0 : MIX_ARCS) + j;
    }
}

// How many arcs (0..MIX_ARCS) a level in dB lights.
static int arcsForDb(float db) {
  return (int)lroundf(constrain((db - MIX_FLOOR_DB) / (MIX_TOP_DB - MIX_FLOOR_DB), 0.0f, 1.0f) * MIX_ARCS);
}
static float gainDb(float g) { return g > 0 ? 20 * log10f(g) : -200.0f; }

// An arc's look: off (dark grey), up to the level (50% grey; the accent at half brightness while edited),
// the meter (white; the accent while edited).
enum : uint8_t { ARC_OFF, ARC_LEVEL, ARC_METER, ARC_NONE, ARC_LEVEL_EDIT, ARC_METER_EDIT };

static uint16_t blockColor(uint8_t look, int r, int c) {
  switch (look) {
    case ARC_METER: return INK;
    case ARC_METER_EDIT: return ACCENT;
    case ARC_LEVEL: return DIM;
    case ARC_LEVEL_EDIT: return blend565(ACCENT, BG, 128);   // the accent at half brightness
    case ARC_OFF:   return DARK;
    default:        return BG;
  }
}

static void drawBlock(int i, int r, int c, uint16_t color) {
  board.tft().fillRect(CELL[MIX_CELL].x + c * PX, rowTop(i) + r * PX, PX, PX, color);
}

void drawMix(int i) {
  static bool built = false;
  if (!built) { buildArcs(); built = true; }
  static uint16_t shownFrame[NUM_TRACKS];
  static uint8_t shown[NUM_TRACKS][2 * MIX_ARCS];
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

  uint16_t frame = cellFrame(i, MIX_CELL);
  uint8_t looks[2 * MIX_ARCS];
  float gains[2] = {tr.levelL, tr.levelR};
  bool editing = i == selected && selectedCell == MIX_CELL && bAlone();
  uint8_t levelLook = editing ? ARC_LEVEL_EDIT : ARC_LEVEL, meterLook = editing ? ARC_METER_EDIT : ARC_METER;
  for (int ch = 0; ch < 2; ch++) {
    int level = arcsForDb(gainDb(gains[ch])), meter = arcsForDb(meterDb[i][ch]);
    for (int j = 0; j < MIX_ARCS; j++)
      looks[ch * MIX_ARCS + j] = !hasClip(i) ? ARC_NONE : j < meter ? meterLook : j < level ? levelLook : ARC_OFF;
  }

  auto &t = board.tft();
  const Cell &cell = CELL[MIX_CELL];
  int top = rowTop(i);
  if (drawn[i] != uiGeneration || shownFrame[i] != frame) {
    // Whole cell: built in a buffer and sent at once (no flash).
    static uint16_t px[ROW_BAND * CELL[MIX_CELL].w];
    for (int r = 0; r < BAND_ROWS; r++)
      for (int c = 0; c < CELL_COLS; c++) {
        uint16_t color = BG;
        if (frame != BG) {
          bool side = c == 0 || c == CELL_COLS - 1, edge = r == 0 || r == BAND_ROWS - 1;
          if (side != edge) color = frame;                       // (corners cut)
        }
        if (arcAt[r][c] >= 0) color = blockColor(looks[arcAt[r][c]], r, c);
        for (int dy = 0; dy < PX; dy++)
          for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * cell.w + c * PX + dx] = color;
      }
    if (bgTop(i) < top) t.fillRect(cell.x, bgTop(i), cell.w, top - bgTop(i), BG);   // the first row's margin
    t.writeRect(cell.x, top, cell.w, ROW_BAND, px);
    drawDividerUnder(i, cell.x, cell.w);
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
        if (arcAt[r][c] == a) drawBlock(i, r, c, blockColor(looks[a], r, c));
  }
}
