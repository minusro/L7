// ui_buttons.cpp — the row buttons and their icons (see ui.h)
#include "ui.h"

// ---- Row buttons
// Tracks with a clip show a toggle on every row: a filled play triangle while
// the clip plays, an outline when muted; white on the selected row, dark grey
// on the others. Unselected empty tracks show nothing; the selected one shows record (red), then play while recording
// (the press ends the recording and plays the clip). Both flash while waiting
// for the recording to start (next bar) or to finish (its full length).
// The status row shows the transport: play while running, stop while
// stopped; grey, white when the row is selected (A then starts from 1.1.1).
// (int, not an enum class: the Arduino IDE declares functions above any types.)
enum { ICON_NONE, ICON_RECORD, ICON_STOP, ICON_PLAY, ICON_TOGGLE_OFF, ICON_TOGGLE_ON };

// Flashing icons blink four times per beat, twice above 100 BPM.
static bool flashOn() {
  noInterrupts();
  bool running = transport.running;
  uint32_t sampleInBeat = transport.sampleInBeat, spb = transport.spb;
  interrupts();
  if (!running) return true;
  uint32_t phases = bpmForSpb(spb) > 100 ? 4 : 8;   // on/off halves per beat
  return ((uint64_t)sampleInBeat * phases / spb) % 2 == 0;
}

static int buttonIcon(int i) {
  if (i == STATUS_ROW) return transport.running ? ICON_PLAY : ICON_STOP;   // the transport's state
  if (i == selected) {
    if (recordingOn(i)) {
      Recording::Mode mode = recording.mode;
      if (mode == Recording::Running) return ICON_PLAY;
      if (!flashOn()) return ICON_NONE;
      return mode == Recording::WaitBar ? ICON_RECORD : ICON_PLAY;
    }
    if (tracks[i].state == TrackState::Empty) return ICON_RECORD;
  }
  if (tracks[i].state == TrackState::Empty) return ICON_NONE;
  return tracks[i].state == TrackState::Playing ? ICON_TOGGLE_ON : ICON_TOGGLE_OFF;
}

static uint16_t iconColor(int i, int icon) {
  switch (icon) {
    case ICON_RECORD:     return ICON_RECORD_COLOR;
    case ICON_TOGGLE_ON:
    case ICON_TOGGLE_OFF: return i == selected ? ICON_SELECTED : ICON_TOGGLE_IDLE;
    default:              return i == selected ? ICON_SELECTED : ICON_TRANSPORT_IDLE;   // play/stop
  }
}


// Button icons: filled shapes in an ICON_SIZE px box, anti-aliased (4x4
// samples per pixel, blended over the row background).
static constexpr float ICON_SIZE = 10.5f;
static constexpr int ICON_REACH = 7;               // drawn pixels: centre +- this

// Toggle: an 8 px tall play triangle, filled (on) or a 1 px outline (off).
static constexpr float TOGGLE_HALF = 4, TOGGLE_STROKE = 1;

// Distance from (dx, dy) inside a right-pointing play triangle of half-height
// h (negative outside).
static float playTriangle(float dx, float dy, float h) {
  const float ax = -0.75f * h, ay = -h, bx = -0.75f * h, by = h, cx = h, cy = 0;
  auto edge = [&](float px, float py, float qx, float qy) {
    float ex = qx - px, ey = qy - py;
    return ((dx - px) * ey - (dy - py) * ex) / sqrtf(ex * ex + ey * ey);
  };
  return min(min(edge(ax, ay, bx, by), edge(bx, by, cx, cy)), edge(cx, cy, ax, ay));
}

// Is the point (dx, dy) from the icon's centre inside the icon?
static bool onIcon(int icon, float dx, float dy) {
  float h = ICON_SIZE / 2;
  switch (icon) {
    case ICON_RECORD: return dx * dx + dy * dy <= 0.5625f * h * h;   // 75% of the box
    case ICON_STOP:   return max(fabsf(dx), fabsf(dy)) <= h;
    case ICON_PLAY:   return playTriangle(dx, dy, h) >= 0;
    case ICON_TOGGLE_OFF:
    case ICON_TOGGLE_ON: {
      float d = playTriangle(dx, dy, TOGGLE_HALF);
      return d >= 0 && (icon == ICON_TOGGLE_ON || d < TOGGLE_STROKE);
    }
    default: return false;
  }
}

static void drawIcon(int icon, float cx, float cy, uint16_t color, uint16_t bg) {
  auto &t = board.tft();
  const int r = ICON_REACH;
  for (int y = (int)cy - r; y <= (int)cy + r; y++) {
    for (int x = (int)cx - r; x <= (int)cx + r; x++) {
      int on = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
          if (onIcon(icon, x + (sx + 0.5f) / 4 - cx, y + (sy + 0.5f) / 4 - cy)) on++;
      if (on) t.drawPixel(x, y, blend565(color, bg, on * 255 / 16));
    }
  }
}

// The row's cells other than the waveform (which drawWave() handles): the two
// status row's cells, the gaps and the button. Each part is redrawn only when
// it changes: refilling a cell that didn't change would blank what's drawn on
// it (the status text) until its next redraw.
void drawCells(int i) {
  struct Key { int icon; uint16_t bg0, bg1, bg2, bgButton, iconColor; };
  static Key shown[ROWS];
  static bool drawn[ROWS];
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.icon = buttonIcon(i);
  k.bg0 = cellBackground(i, 0);
  k.bg1 = cellBackground(i, 1);
  k.bg2 = i == STATUS_ROW ? cellBackground(i, TEMPO_CELL) : 0;   // tracks: drawWave()'s job
  k.bgButton = cellBackground(i, -1);
  k.iconColor = iconColor(i, k.icon);
  bool first = !drawn[i];
  if (!first && !memcmp(&k, &shown[i], sizeof k)) return;
  const Key old = shown[i];
  drawn[i] = true;
  shown[i] = k;
  auto &t = board.tft();
  int y = rowTop(i), y0 = bgTop(i), h = bgHeight(i);
  if (i == STATUS_ROW) {                             // tracks: drawFilter(), drawMix(), drawWave()
    if (first || k.bg0 != old.bg0) t.fillRect(CELL[0].x, y0, CELL[0].w, h, k.bg0);
    if (first || k.bg1 != old.bg1) t.fillRect(CELL[1].x, y0, CELL[1].w, h, k.bg1);
    if (first || k.bg2 != old.bg2) t.fillRect(CELL[2].x, y0, CELL[2].w, h, k.bg2);
  }
  if (first)
    for (int c = 0; c < CELLS; c++)                  // 1 px gaps after each cell
      t.drawFastVLine(CELL[c].x + CELL[c].w, y0, h, GAP);
  if (first || k.bgButton != old.bgButton || k.icon != old.icon || k.iconColor != old.iconColor) {
    // The button: its background, then the icon (a small area: no visible flash).
    t.fillRect(BUTTON_X, y0, BUTTON_W, h, k.bgButton);
    if (i == DIVIDER_ROW && k.bgButton == BG) t.drawFastHLine(BUTTON_X, dividerY(), BUTTON_W, DIVIDER);
    if (k.icon != ICON_NONE)
      drawIcon(k.icon, BUTTON_X + BUTTON_W / 2, y + ROW_BAND / 2, k.iconColor, k.bgButton);
  }
}
