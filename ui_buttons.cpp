// ui_buttons.cpp — the row buttons and their icons (see ui.h)
#include "ui.h"
#include "input.h"
#include "ui_font.h"

// ---- Row buttons
// Track rows show the track's state: an unmuted track play while the clock
// runs (white), pause while it's stopped (grey); the recording track record, flashing while it waits for the bar,
// steady while recording, then a flashing play while it carries on to its
// length (in the accent colour). Nothing on muted or empty tracks.
// The status row's button (always inverted) shows what A does now on the
// selected row: record (red) on an empty track; finish recording (play);
// cancel a recording still waiting for its bar (X); mute (stop) or unmute
// (play); with SHIFT held, cut the clip (scissors) or, on an empty track,
// paste (a clipboard; dark grey while the clipboard is empty). With B
// held: open the panel (a menu icon); with SHIFT too, paste a scene (B pressed
// first) or cut it (SHIFT first): the track cut / paste icons in a square.
// While the panel is open: a house (A closes it). On the status row itself:
// tap tempo ("TAP") on the tempo cell, the panel (menu, as B+A) on the
// scene cell; with SHIFT, start from 1.1.1 (play).
// While A is held it keeps showing the command it ran, on the accent colour.
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
  if (selected == STATUS_ROW)                      // restart / the panel (scene cell, as B+A) / tap tempo
    return shift ? ICON_PLAY : selectedCellIn(STATUS_ROW) == SCENE_CELL ? ICON_MENU : ICON_TAP;
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
  return ICON_NONE;                                // (the track's number shows its state)
}

// A track row's number badge: the track's number (counted from the bottom),
// dark grey on an empty track; in a square outlined grey (number grey) on a
// muted one; in a square filled with the accent colour (number black) on one
// playing. Hidden on the recording (or queued) track, whose icons show instead.
enum : uint8_t { BADGE_NONE, BADGE_EMPTY, BADGE_MUTED, BADGE_PLAYING, BADGE_LABEL };   // LABEL: the number alone, black

static uint8_t trackBadge(int i) {
  if (i == STATUS_ROW || recordingOn(i) || recordQueued() == i) return BADGE_NONE;
  if (!hasClip(i)) return BADGE_EMPTY;
  return tracks[i].state == TrackState::Muted ? BADGE_MUTED : BADGE_PLAYING;
}

int trackNumber(int i) { return NUM_TRACKS - i; }

static int buttonIcon(int i) {
  if (i != STATUS_ROW) return trackIcon(i);
  if (panelOpen() && heldAction != ICON_HOME) heldAction = -1;   // just opened: the house, at once
  if (heldAction >= 0 && !(board.keys() & m8::BTN_EDIT)) heldAction = -1;   // A released
  return heldAction >= 0 ? heldAction : actionIcon();
}

// Icons: bitmaps of blocks ('#'), centred in the button. On the action
// button (50% grey; the accent colour while A is held) they're black;
// elsewhere the accent colour for a
// recording, white for a track playing, grey otherwise. Track rows' are
// smaller, left of centre (clear of the grey line along the selected track's
// button's right edge, its left corners cut). An action with nothing to act
// on (paste, clipboard empty) is dark grey.
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
// Track rows' state icons, smaller (the status button keeps the big ones).
PROGMEM static const Icon SMALL_RECORD = {4, 4, {".##.", "####", "####", ".##."}};
PROGMEM static const Icon SMALL_PLAY = {3, 5, {"#..", "##.", "###", "##.", "#.."}};
PROGMEM static const Icon SMALL_PAUSE = {3, 5, {"#.#", "#.#", "#.#", "#.#", "#.#"}};
static constexpr int TRACK_ICON_CENTRE = 5;       // the column track rows' icons and number centre on

static bool unavailable(int i, int icon) {
  return i == STATUS_ROW && ((icon == ICON_PASTE && !clipboardFull()) || (icon == ICON_SCENE_PASTE && !sceneClipboardFull()));
}

// The action button (the status row's): 50% grey, the accent colour while A
// is held. The tracks' buttons: black.
static uint16_t buttonBackground(int i, int icon) {
  (void)icon;
  if (i == STATUS_ROW) return heldAction >= 0 ? ACCENT : DIM;
  return cellBackground(i, -1);
}

static uint16_t iconColor(int i, int icon) {
  uint16_t bg = buttonBackground(i, icon);
  if (unavailable(i, icon)) return DARK;
  if (i == STATUS_ROW) return BG;
  if (bg != BG) return BG;
  if (recordingOn(i) || recordQueued() == i) return ACCENT;
  return i == STATUS_ROW || icon == ICON_PLAY ? INK : DIM;   // a track playing: white
}

// The selected track's line: grey, as the selected cell's frame; in the
// accent colour while A is held, with the action button (acting on the track).
static uint16_t markColor() { return heldAction >= 0 ? ACCENT : DIM; }

// The button, built whole in a buffer and sent at once.
static void drawButton(int i, int icon, uint16_t color, uint16_t bg, uint8_t badge) {
  static uint16_t px[BUTTON_W * ROW_BAND];
  for (auto &p : px) p = bg;
  auto blockIn = [&](int c, int r, uint16_t ink) {
    for (int dy = 0; dy < PX; dy++)
      for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * BUTTON_W + c * PX + dx] = ink;
  };
  const int cols = BUTTON_W / PX, rows = ROW_BAND / PX;
  // The selected track's panel, under its number or icons: the button's whole
  // height, its left corners cut (a two-block border round the number on the
  // left, top and bottom). Grey; the accent colour while A is held, showing
  // just the track's number then, in black.
  uint16_t panel = i == selected && i != STATUS_ROW ? markColor() : BG;
  if (panel != BG) {
    for (int r = 0; r < rows; r++)
      for (int c = 0; c < cols; c++)
        if (!(c == 0 && (r == 0 || r == rows - 1))) blockIn(c, r, panel);
    if (color == panel) color = BG;                // (a recording icon in the accent colour, on it)
    if (panel == ACCENT && badge != BADGE_NONE) badge = BADGE_LABEL;   // highlighted: the number alone
  }
  auto block = [&](int c, int r) { blockIn(c, r, color); };
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
    bool track = i != STATUS_ROW;
    const Icon &ic = !track ? ICONS[icon] : icon == ICON_RECORD ? SMALL_RECORD : icon == ICON_PLAY ? SMALL_PLAY
                   : icon == ICON_PAUSE ? SMALL_PAUSE : ICONS[icon];
    int c0 = track ? TRACK_ICON_CENTRE - ic.w / 2 : (cols - ic.w) / 2, r0 = (rows - ic.h) / 2;
    for (int r = 0; r < ic.h; r++)
      for (int c = 0; c < ic.w; c++)
        if (ic.rows[r][c] == '#') block(c0 + c, r0 + r);
  }
  if (badge != BADGE_NONE) {                       // the track's number, maybe in a square
    // The square: 7 x 9 blocks (a block of edge, a block of space round the
    // digit), corners cut, centred on the icons' column.
    const int w = 7, h = 9, c0 = TRACK_ICON_CENTRE - w / 2, r0 = (rows - h) / 2;
    if (badge != BADGE_EMPTY && badge != BADGE_LABEL) {
      // (On the panel: a muted one's outline black; a playing one's square
      // black, its number in the accent colour, on the panel in the accent colour.)
      uint16_t fill = badge == BADGE_PLAYING ? (panel == ACCENT ? BG : ACCENT) : (panel == DIM ? BG : DIM);
      for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
          bool side = c == 0 || c == w - 1, edge = r == 0 || r == h - 1;
          if (side && edge) continue;                // (corners cut)
          if (badge == BADGE_PLAYING || side || edge) blockIn(c0 + c, r0 + r, fill);
        }
    }
    const Glyph *g = glyphFor((char)('0' + trackNumber(i)));
    uint16_t ink = badge == BADGE_PLAYING ? (panel == ACCENT ? ACCENT : BG)
                 : badge == BADGE_MUTED ? (panel == DIM ? BG : DIM) : badge == BADGE_LABEL ? BG : DARK;
    int gc = TRACK_ICON_CENTRE - glyphWidth(*g) / 2, gr = (rows - 5) / 2;
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < glyphWidth(*g); c++)
        if (g->rows[r][c] == '#') blockIn(gc + c, gr + r, ink);
  }
  if (i == STATUS_ROW && bg != BG)                 // the action button: its corners cut
    for (int y = 0; y < PX; y++)
      for (int x = 0; x < PX; x++)
        for (int cy : {y, ROW_BAND - 1 - y})
          for (int cx : {x, BUTTON_W - 1 - x}) px[cy * BUTTON_W + cx] = BG;
  auto &t = board.tft();
  int top = rowTop(i);
  if (bgTop(i) < top) t.fillRect(BUTTON_X, bgTop(i), BUTTON_W, top - bgTop(i), bg);   // the first row's margin
  t.writeRect(BUTTON_X, top, BUTTON_W, ROW_BAND, px);
  int bottom = top + ROW_BAND, end = bgTop(i) + bgHeight(i);
  if (bottom < end) t.fillRect(BUTTON_X, bottom, BUTTON_W, end - bottom, BG);   // the margin below
  drawDividerUnder(i, BUTTON_X, BUTTON_W);
}

// The row's gaps and button (the cells: drawWave(), drawFilter(), drawMix(),
// drawStatus()). Redrawn only when they change.
void drawCells(int i) {
  struct Key { int icon; uint16_t bg, iconColor, mark; uint8_t badge; };
  static Key shown[ROWS];
  static uint32_t drawn[ROWS];                     // uiGeneration when last drawn whole
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.icon = buttonIcon(i);
  k.bg = buttonBackground(i, k.icon);
  k.iconColor = iconColor(i, k.icon);
  k.mark = i == selected ? markColor() : BG;
  k.badge = trackBadge(i);
  bool first = drawn[i] != uiGeneration;
  if (!first && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = uiGeneration;
  shown[i] = k;
  auto &t = board.tft();
  if (first && !panelOpen())                         // (the panel's dimming covers them meanwhile)
    for (int c = 0; c < CELLS; c++) {                // the gaps after each cell
      if (i == STATUS_ROW && c == TEMPO_CELL) continue;   // (inside the tempo's cell there)
      int from = CELL[c].x + CELL[c].w, to = c + 1 < CELLS ? CELL[c + 1].x : BUTTON_X;
      t.fillRect(from, bgTop(i), to - from, bgHeight(i), BG);
      if (i == DIVIDER_ROW) t.fillRect(from, dividerY(), to - from, PX, DIVIDER);   // the divider runs on
    }
  drawButton(i, k.icon, k.iconColor, k.bg, k.badge);
}
