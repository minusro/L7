// ui_mix.cpp — the volume / balance cell and its meters (see ui.h)
#include "ui.h"

// ---- Volume / balance cell: concentric arcs around the cell's centre, (((
// for the left channel and ))) for the right, MIX_ARCS each. A channel's
// level lights its arcs from the inside out, over MIX_FLOOR_DB..MIX_TOP_DB:
// the volume, plus what the balance moves onto that side (up to the mono sum,
// +6 dB) or takes away from it. The meter (the channel's actual output, with
// peak hold falling at METER_FALL_DB_PER_S) lights them white on the same
// scale, so a full-scale signal on both channels reaches the level's arc. Each arc's pixels
// (with anti-aliasing coverage) are worked out once; an arc is only redrawn
// when its colour changes.
static constexpr int MIX_ARCS = 6, MIX_ARC_PX = 128;
static constexpr float MIX_R0 = 8, MIX_DR = 5, MIX_STROKE = 1.25f, MIX_HALF_H = 10, MIX_SLOPE = 1.2f;
static constexpr float MIX_FLOOR_DB = -40, MIX_TOP_DB = 12, METER_FALL_DB_PER_S = 24;

struct ArcPx { uint8_t x, y, cov; };                 // in the cell; coverage 1..16
static ArcPx arcPx[2 * MIX_ARCS][MIX_ARC_PX];         // [side * MIX_ARCS + arc]
static int arcCount[2 * MIX_ARCS];

static void buildArcs() {
  const Cell &cell = CELL[MIX_CELL];
  const float cx = cell.w / 2.0f, cy = ROW_BAND / 2.0f;
  for (int y = 0; y < ROW_BAND; y++) {
    for (int x = 0; x < cell.w; x++) {
      int cov[2 * MIX_ARCS] = {};
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          float dx = x + (sx + 0.5f) / 4 - cx, dy = y + (sy + 0.5f) / 4 - cy;
          if (fabsf(dy) > MIX_HALF_H || fabsf(dy) > fabsf(dx) * MIX_SLOPE) continue;
          float d = sqrtf(dx * dx + dy * dy);
          int j = (int)lroundf((d - MIX_R0) / MIX_DR);
          if (j < 0 || j >= MIX_ARCS || fabsf(d - (MIX_R0 + j * MIX_DR)) > MIX_STROKE / 2) continue;
          cov[(dx < 0 ? 0 : MIX_ARCS) + j]++;
        }
      }
      for (int a = 0; a < 2 * MIX_ARCS; a++)
        if (cov[a] && arcCount[a] < MIX_ARC_PX) arcPx[a][arcCount[a]++] = {(uint8_t)x, (uint8_t)y, (uint8_t)cov[a]};
    }
  }
}

// How many arcs (0..MIX_ARCS, fractional) a level in dB lights.
static float arcsForDb(float db) {
  return constrain((db - MIX_FLOOR_DB) / (MIX_TOP_DB - MIX_FLOOR_DB), 0.0f, 1.0f) * MIX_ARCS;
}
static float gainDb(float g) { return g > 0 ? 20 * log10f(g) : -200.0f; }

// Arc j's colour for a channel lit to `level` arcs, with the meter at `meter` arcs.
// Partly lit arcs blend in quarter steps.
static uint16_t arcColor(int j, float level, float meter, uint16_t levelColor) {
  auto part = [](float f) { return (uint8_t)(lroundf(constrain(f, 0.0f, 1.0f) * 4) * 255 / 4); };
  uint16_t c = blend565(levelColor, MIX_ARC_OFF, part(level - j));
  return blend565(MIX_ARC_METER, c, part(meter - j));
}

void drawMix(int i) {
  static bool built = false;
  if (!built) { buildArcs(); built = true; }
  static uint16_t shownBg[NUM_TRACKS], shown[NUM_TRACKS][2 * MIX_ARCS];
  static bool drawn[NUM_TRACKS];
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

  uint16_t bg = cellBackground(i, MIX_CELL);
  uint16_t colors[2 * MIX_ARCS];
  bool empty = tr.state == TrackState::Empty;
  uint16_t levelColor = tr.state == TrackState::Muted ? blend565(MIX_ARC_LEVEL, BG, MUTED_WEIGHT) : MIX_ARC_LEVEL;
  float gains[2] = {tr.levelL, tr.levelR};
  for (int ch = 0; ch < 2; ch++) {
    float level = arcsForDb(gainDb(gains[ch])), meter = arcsForDb(meterDb[i][ch]);
    for (int j = 0; j < MIX_ARCS; j++)
      colors[ch * MIX_ARCS + j] = empty ? bg : arcColor(j, level, meter, levelColor);
  }

  auto &t = board.tft();
  const Cell &cell = CELL[MIX_CELL];
  int top = rowTop(i);
  if (!drawn[i] || shownBg[i] != bg) {
    // Whole cell: built in a buffer and sent at once (no flash).
    static uint16_t buf[ROW_BAND * CELL[MIX_CELL].w];
    for (auto &p : buf) p = bg;
    if (i == DIVIDER_ROW && bg == BG)
      for (int x = 0; x < cell.w; x++) buf[(dividerY() - top) * cell.w + x] = DIVIDER;
    for (int a = 0; a < 2 * MIX_ARCS; a++)
      for (int k = 0; k < arcCount[a]; k++) {
        const ArcPx &p = arcPx[a][k];
        buf[p.y * cell.w + p.x] = blend565(colors[a], bg, p.cov * 255 / 16);
      }
    if (bgTop(i) < top) t.fillRect(cell.x, bgTop(i), cell.w, top - bgTop(i), bg);   // the first row's margin
    t.writeRect(cell.x, top, cell.w, ROW_BAND, buf);
    drawn[i] = true;
    shownBg[i] = bg;
    memcpy(shown[i], colors, sizeof colors);
    return;
  }
  for (int a = 0; a < 2 * MIX_ARCS; a++) {
    if (colors[a] == shown[i][a]) continue;
    shown[i][a] = colors[a];
    for (int k = 0; k < arcCount[a]; k++) {
      const ArcPx &p = arcPx[a][k];
      t.drawPixel(cell.x + p.x, top + p.y, blend565(colors[a], bg, p.cov * 255 / 16));
    }
  }
}
