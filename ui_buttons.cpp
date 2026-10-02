// ui_buttons.cpp — the row buttons and their icons (see ui.h)
#include "ui.h"
#include "input.h"
#include "ui_font.h"

// ---- Row buttons
// Track rows show the track's state: an unmuted track play while the clock
// runs, pause while it's stopped (black on the selected row's inverted
// button, dim on the others); the recording track record, flashing while it
// waits for the bar, steady while recording, then a flashing play while it
// carries on to its length (white). Nothing on muted or empty tracks.
// The status row's button (always inverted) shows what A does now on the
// selected row: record (red) on an empty track; finish recording (play);
// cancel a recording still waiting for its bar (X); mute (stop) or unmute
// (play); with SHIFT held, cut the clip (scissors) or, on an empty track,
// paste (a clipboard; a checker pattern while the clipboard is empty). With B
// held: open the panel (a menu icon); with SHIFT too, paste a scene (B pressed
// first) or cut it (SHIFT first): the track cut / paste icons in a square.
// While the panel is open: a house (A closes it). On the status row itself:
// tap tempo ("TAP"); with SHIFT, start from 1.1.1 (play).
// While A is held it keeps showing the command it ran, white on black.
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

// Icons: bitmaps of blocks ('#'), centred in the button. On an inverted
// button (the selected track's, the status row's) they're black; elsewhere
// white for a recording, dim for the others. The status row's record icon is
// red. An action with nothing to act on (paste, clipboard empty) shows as a
// checker pattern; while A is held the status button turns back to white on
// black.
struct Icon { int w, h; const char *rows[9]; };
PROGMEM static const Icon ICONS[] = {
  {0, 0, {}},                                                                  // ICON_NONE
  {7, 7, {".......", "..###..", ".#####.", ".#####.", ".#####.", "..###..", "......."}},   // RECORD
  {7, 7, {".......", ".#####.", ".#####.", ".#####.", ".#####.", ".#####.", "......."}},   // STOP
  {7, 7, {"..#....", "..##...", "..###..", "..####.", "..###..", "..##...", "..#...."}},   // PLAY
  {7, 7, {".......", ".##.##.", ".##.##.", ".##.##.", ".##.##.", ".##.##.", "......."}},   // PAUSE
  {7, 7, {".......", ".#...#.", "..#.#..", "...#...", "..#.#..", ".#...#.", "......."}},   // DELETE
  {0, 0, {}},                                                                  // TAP (text)
  {7, 7, {".##...#", "#..#.#.", ".##.#..", "...#...", ".##.#..", "#..#.#.", ".##...#"}},   // CUT
  {7, 7, {"..###..", ".#####.", ".#...#.", ".#.#.#.", ".#...#.", ".#.#.#.", ".#####."}},   // PASTE
  {7, 7, {".......", "#######", ".......", "#######", ".......", "#######", "......."}},   // MENU
  {7, 7, {"...#...", "..###..", ".#####.", "#######", ".##.##.", ".##.##.", ".##.##."}},   // HOME
  {9, 9, {"#########", "#.......#", "#.#...#.#", "#..#.#..#", "#...#...#", "#.##.##.#", "#.##.##.#",
          "#.......#", "#########"}},                                                   // SCENE_CUT
  {9, 9, {"#########", "#.......#", "#..###..#", "#.#...#.#", "#.#.#.#.#", "#.#...#.#", "#.#####.#",
          "#.......#", "#########"}},                                                   // SCENE_PASTE
};

static bool unavailable(int i, int icon) {
  return i == STATUS_ROW && ((icon == ICON_PASTE && !clipboardFull()) || (icon == ICON_SCENE_PASTE && !sceneClipboardFull()));
}

static uint16_t buttonBackground(int i) {
  if (i == STATUS_ROW && heldAction >= 0) return BG;   // A held: back to white on black
  return cellBackground(i, -1);
}

static uint16_t iconColor(int i, int icon) {
  uint16_t bg = buttonBackground(i);
  if (i == STATUS_ROW && icon == ICON_RECORD && bg == INK) return REC;   // A records
  if (bg == INK) return BG;
  return i == STATUS_ROW || recordingOn(i) || recordQueued() == i ? INK : DIM;
}

// The button, built whole in a buffer and sent at once.
static void drawButton(int i, int icon, uint16_t color, uint16_t bg) {
  static uint16_t px[BUTTON_W * ROW_BAND];
  for (auto &p : px) p = bg;
  auto block = [&](int c, int r) {
    for (int dy = 0; dy < PX; dy++)
      for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * BUTTON_W + c * PX + dx] = color;
  };
  const int cols = BUTTON_W / PX, rows = ROW_BAND / PX;
  bool checker = unavailable(i, icon);
  if (icon == ICON_TAP) {
    const char *text = "TAP";
    int c0 = (cols - textWidth(text) / PX) / 2, r0 = (rows - FONT_H / PX) / 2;
    for (const char *p = text; *p; p++) {
      const Glyph *g = glyphFor(*p);
      for (int r = 0; r < 5; r++)
        for (int c = 0; c < glyphWidth(*g); c++)
          if (g->rows[r][c] == '#') block(c0 + c, r0 + r);
      c0 += glyphWidth(*g) + 1;
    }
  } else if (icon != ICON_NONE) {
    const Icon &ic = ICONS[icon];
    int c0 = (cols - ic.w) / 2, r0 = (rows - ic.h) / 2;
    for (int r = 0; r < ic.h; r++)
      for (int c = 0; c < ic.w; c++)
        if (ic.rows[r][c] == '#' && (!checker || (r + c) % 2 == 0)) block(c0 + c, r0 + r);
  }
  auto &t = board.tft();
  int top = rowTop(i);
  if (bgTop(i) < top) t.fillRect(BUTTON_X, bgTop(i), BUTTON_W, top - bgTop(i), bg);   // the first row's margin
  t.writeRect(BUTTON_X, top, BUTTON_W, ROW_BAND, px);
  int bottom = top + ROW_BAND, end = bgTop(i) + bgHeight(i);
  if (bottom < end) t.fillRect(BUTTON_X, bottom, BUTTON_W, end - bottom, BG);   // the last row's margin
}

// The row's gaps and button (the cells: drawWave(), drawFilter(), drawMix(),
// drawStatus()). Redrawn only when they change.
void drawCells(int i) {
  struct Key { int icon; uint16_t bg, iconColor; bool checker; };
  static Key shown[ROWS];
  static uint32_t drawn[ROWS];                     // uiGeneration when last drawn whole
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.icon = buttonIcon(i);
  k.bg = buttonBackground(i);
  k.iconColor = iconColor(i, k.icon);
  k.checker = unavailable(i, k.icon);
  bool first = drawn[i] != uiGeneration;
  if (!first && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = uiGeneration;
  shown[i] = k;
  auto &t = board.tft();
  if (first && !panelOpen())                         // (the panel's dimming covers them meanwhile)
    for (int c = 0; c < CELLS; c++) {                // the gaps after each cell
      int from = CELL[c].x + CELL[c].w, to = c + 1 < CELLS ? CELL[c + 1].x : BUTTON_X;
      t.fillRect(from, bgTop(i), to - from, bgHeight(i), BG);
    }
  drawButton(i, k.icon, k.iconColor, k.bg);
}
