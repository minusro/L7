// ui_buttons.cpp — the row buttons and their icons (see ui.h)
#include "ui.h"
#include "input.h"

// ---- Row buttons
// Track rows show the track's state: an unmuted track play while the clock
// runs, pause while it's stopped (white on the selected row, grey on the
// others); the recording track record, flashing while it waits for the bar,
// steady while recording, then a flashing play while it carries on to its
// length (white). Nothing on muted or empty tracks.
// The status row's button (always highlighted) shows what A does now on the
// selected row: record (red) on an empty track; finish recording (play);
// cancel a recording still waiting for its bar (X); mute (stop) or unmute
// (play); with SHIFT held, cut the clip (scissors) or, on an empty track,
// paste (a clipboard; very dim while the clipboard is empty). With B held:
// open the panel (a menu icon); with SHIFT too, paste a scene (B pressed
// first) or cut it (SHIFT first): the track cut / paste icons in a square.
// While the panel is open: a house (A closes it). On the status row itself:
// tap tempo ("TAP"); with SHIFT, start from 1.1.1 (play).
// While A is held it keeps showing the command it ran, dimmed.
// (int, not an enum class: the Arduino IDE declares functions above any types.)
enum { ICON_NONE, ICON_RECORD, ICON_STOP, ICON_PLAY, ICON_PAUSE, ICON_DELETE, ICON_TAP, ICON_CUT, ICON_PASTE, ICON_MENU, ICON_HOME, ICON_SCENE_CUT, ICON_SCENE_PASTE };

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

// What A does on the selected row (mirrors onKeys()).
static int actionIcon() {
  bool shift = board.keys() & m8::BTN_SHIFT;
  if (panelOpen()) return ICON_HOME;               // A closes the panel
  if (board.keys() & m8::BTN_OPT)                  // B+A: the panel; with SHIFT, the scene
    return !shift ? ICON_MENU : optBeforeShift() ? ICON_SCENE_PASTE : ICON_SCENE_CUT;
  if (selected == STATUS_ROW) return shift ? ICON_PLAY : ICON_TAP;   // restart / tap tempo
  const Track &tr = tracks[selected];
  if (shift) return hasClip(selected) ? ICON_CUT : recordingOn(selected) ? ICON_NONE : ICON_PASTE;
  if (recordQueued() == selected) return ICON_DELETE;   // takes it out of the queue
  if (recordingOn(selected)) {
    switch (recording.mode) {
      case Recording::WaitBar: return ICON_DELETE;   // cancels it
      case Recording::Running: return ICON_PLAY;     // finishes it
      default:                 return ICON_NONE;     // already finishing
    }
  }
  if (tr.state == TrackState::Empty) return ICON_RECORD;
  return tr.state == TrackState::Playing ? ICON_STOP : ICON_PLAY;   // mute / unmute
}

// The command A ran, shown dimmed while A is still held (-1: none).
static int heldAction = -1;

void noteActionPress() { heldAction = actionIcon(); }

// The track's own state.
static int trackIcon(int i) {
  if (recordQueued() == i) return flashOn() ? ICON_RECORD : ICON_NONE;   // waiting, like WaitBar
  if (recordingOn(i)) {
    switch (recording.mode) {
      case Recording::WaitBar: return flashOn() ? ICON_RECORD : ICON_NONE;
      case Recording::Running: return ICON_RECORD;
      default:                 return flashOn() ? ICON_PLAY : ICON_NONE;
    }
  }
  if (!hasClip(i)) return ICON_NONE;
  if (tracks[i].state == TrackState::Muted) return ICON_NONE;
  return transport.running ? ICON_PLAY : ICON_PAUSE;
}

static int buttonIcon(int i) {
  if (i != STATUS_ROW) return trackIcon(i);
  if (panelOpen() && heldAction != ICON_HOME) heldAction = -1;   // just opened: the house, at once
  if (heldAction >= 0 && !(board.keys() & m8::BTN_EDIT)) heldAction = -1;   // A released
  return heldAction >= 0 ? heldAction : actionIcon();
}

static uint16_t iconColor(int i, int icon) {
  uint16_t c = i == STATUS_ROW ? (icon == ICON_RECORD ? ICON_RECORD_COLOR : ICON_SELECTED)   // record: red
             : i == selected || recordingOn(i) ? ICON_SELECTED : ICON_IDLE;
  if (i == STATUS_ROW && ((icon == ICON_PASTE && !clipboardFull()) || (icon == ICON_SCENE_PASTE && !sceneClipboardFull())))
    c = blend565(c, cellBackground(i, -1), ICON_UNAVAILABLE_WEIGHT);   // nothing to paste
  if (i == STATUS_ROW && heldAction >= 0) c = blend565(c, cellBackground(i, -1), ICON_PRESSED_WEIGHT);   // A held
  return c;
}


// Button icons: filled shapes in an ICON_SIZE px box, anti-aliased (4x4
// samples per pixel, blended over the row background).
static constexpr float ICON_SIZE = 10.5f;
static constexpr int ICON_REACH = 7;               // drawn pixels: centre +- this

// Delete: an X, 8 px across, 1.5 px strokes.
static constexpr float DELETE_HALF = 3.5f, DELETE_STROKE = 1.5f;

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
    case ICON_PAUSE: {                             // two bars
      float ax = fabsf(dx);
      return fabsf(dy) <= h && ax >= 0.25f * h && ax <= 0.8f * h;
    }
    case ICON_SCENE_CUT:                           // the track icons, smaller, in a 1 px square
    case ICON_SCENE_PASTE: {
      float m = max(fabsf(dx), fabsf(dy));
      if (m >= 6 && m <= 7) return true;
      const float k = 0.82f;
      return onIcon(icon == ICON_SCENE_CUT ? ICON_CUT : ICON_PASTE, dx / k, dy / k);
    }
    case ICON_MENU:                                // three 2 px bars, on whole pixels
      return fabsf(dx) <= 5 && (fabsf(dy) <= 1 || fabsf(fabsf(dy) - 4) <= 1);
    case ICON_HOME: {                              // a roof with eaves over a box with a door
      const float eaves = -0.15f * h;
      if (dy < -h) return false;
      if (dy < eaves) return fabsf(dx) <= (dy + h) / (eaves + h) * 1.05f * h;   // the roof
      if (fabsf(dx) > 0.65f * h || dy > h) return false;
      return !(fabsf(dx) <= 0.2f * h && dy >= 0.3f * h);                        // the door
    }
    case ICON_CUT: {                               // scissors: two finger rings left, blades crossing to the right
      for (float sy : {-1.0f, 1.0f}) {
        float rx = dx + 0.55f * h, ry = dy - sy * 0.55f * h;
        if (fabsf(sqrtf(rx * rx + ry * ry) - 0.3f * h) <= 0.5f) return true;
        // Blade from the ring across the middle to the far side's tip.
        float ax = -0.3f * h, ay = sy * 0.4f * h, bx = h, by = -sy * 0.6f * h;
        float ex = bx - ax, ey = by - ay;
        float u = constrain(((dx - ax) * ex + (dy - ay) * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
        float qx = dx - (ax + u * ex), qy = dy - (ay + u * ey);
        if (qx * qx + qy * qy <= 0.36f) return true;
      }
      return false;
    }
    case ICON_PASTE: {                             // clipboard: an outlined board, a clip on top, two lines
      float ax = fabsf(dx);
      if (ax <= 0.35f * h && dy >= -h && dy <= -0.55f * h) return true;   // the clip
      bool inBoard = ax <= 0.7f * h && dy >= -0.75f * h && dy <= h;
      bool inside = ax <= 0.7f * h - 1 && dy >= -0.75f * h + 1 && dy <= h - 1;
      if (inBoard && !inside) return true;
      return ax <= 0.35f * h && (fabsf(dy - 0.05f * h) <= 0.45f || fabsf(dy - 0.5f * h) <= 0.45f);
    }
    case ICON_DELETE: {                            // X: two diagonal strokes
      const float arm = DELETE_HALF, stroke = DELETE_STROKE / 2;
      if (fabsf(dx) > arm + stroke || fabsf(dy) > arm + stroke) return false;
      return fabsf(dx - dy) <= stroke * 1.41421f || fabsf(dx + dy) <= stroke * 1.41421f;
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

// "TAP": a 3x4 pixel font drawn at 2x (2 px strokes), letters 1 px apart.
static constexpr uint8_t TAP_GLYPHS[3][4] = {      // rows, bit 2 = left column
  {0b111, 0b010, 0b010, 0b010},                    // T
  {0b010, 0b101, 0b111, 0b101},                    // A
  {0b110, 0b101, 0b110, 0b100},                    // P
};
static constexpr int TAP_SCALE = 2, TAP_W = 3 * 3 * TAP_SCALE + 2, TAP_H = 4 * TAP_SCALE;   // 20 x 8 px

static void drawTap(int x, int y, uint16_t color) {
  auto &t = board.tft();
  for (int g = 0; g < 3; g++)
    for (int row = 0; row < 4; row++)
      for (int col = 0; col < 3; col++)
        if (TAP_GLYPHS[g][row] >> (2 - col) & 1)
          t.fillRect(x + g * (3 * TAP_SCALE + 1) + col * TAP_SCALE, y + row * TAP_SCALE, TAP_SCALE, TAP_SCALE, color);
}

// The row's cells other than the waveform (which drawWave() handles): the two
// status row's cells, the gaps and the button. Each part is redrawn only when
// it changes: refilling a cell that didn't change would blank what's drawn on
// it (the status text) until its next redraw.
void drawCells(int i) {
  struct Key { int icon; uint16_t bg0, bg1, bg2, bgButton, iconColor; };
  static Key shown[ROWS];
  static uint32_t drawn[ROWS];                     // uiGeneration when last drawn whole
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.icon = buttonIcon(i);
  k.bg0 = cellBackground(i, 0);
  k.bg1 = cellBackground(i, 1);
  k.bg2 = cellBackground(i, 2);                    // (a track's cells: drawWave(), drawFilter(), drawMix())
  k.bgButton = cellBackground(i, -1);
  k.iconColor = iconColor(i, k.icon);
  bool first = drawn[i] != uiGeneration;
  if (!first && !memcmp(&k, &shown[i], sizeof k)) return;
  const Key old = shown[i];
  drawn[i] = uiGeneration;
  shown[i] = k;
  auto &t = board.tft();
  int y = rowTop(i), y0 = bgTop(i), h = bgHeight(i);
  if (i == STATUS_ROW && !panelOpen()) {             // tracks: drawFilter(), drawMix(), drawWave()
    if (first || k.bg0 != old.bg0) t.fillRect(CELL[0].x, y0, CELL[0].w, h, k.bg0);
    if (first || k.bg1 != old.bg1) t.fillRect(CELL[1].x, y0, CELL[1].w, h, k.bg1);
    if (first || k.bg2 != old.bg2) t.fillRect(CELL[2].x, y0, CELL[2].w, h, k.bg2);
  }
  if (first && !panelOpen())                         // (the panel's dimming covers them meanwhile)
    for (int c = 0; c < CELLS; c++)                  // 1 px gaps after each cell
      t.drawFastVLine(CELL[c].x + CELL[c].w, y0, h, GAP);
  if (first || k.bgButton != old.bgButton || k.icon != old.icon || k.iconColor != old.iconColor) {
    // The button: its background, then the icon (a small area: no visible flash).
    t.fillRect(BUTTON_X, y0, BUTTON_W, h, k.bgButton);
    if (i == DIVIDER_ROW && k.bgButton == BG) t.drawFastHLine(BUTTON_X, dividerY(), BUTTON_W, DIVIDER);
    if (k.icon == ICON_TAP) {
      drawTap(BUTTON_X + (BUTTON_W - TAP_W) / 2, y + (ROW_BAND - TAP_H) / 2, k.iconColor);
    } else if (k.icon != ICON_NONE) {
      drawIcon(k.icon, BUTTON_X + BUTTON_W / 2, y + ROW_BAND / 2, k.iconColor, k.bgButton);
    }
  }
}
