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
// cancel a recording still waiting for its bar (X); mute (a speaker crossed
// out) or unmute (a speaker with sound waves); with SHIFT held, cut the clip
// (an arrow up from a line), cancel a recording (X: thrown away at once) or,
// on an empty track, paste (an arrow down to a line; dark grey while the
// clipboard is empty). With B
// held alone on a cell it adjusts: four small arrows round a + (B + arrows),
// black on the accent colour; the pressed look (dark grey, the icon in the
// accent colour) while an arrow is down; A does nothing then; on an empty track: resample
// (the record dot with a plus: B+A); with SHIFT too, paste a scene (B pressed
// first) or cut it (SHIFT first): the track cut / paste icons in a square.
// While the menu is open (on its items A runs the action when let go; B does
// nothing): on its
// list's items, export Ableton project (an arrow into a tray) and enter USB data mode (the USB
// trident), nothing yet; on a project slot, load (a small play; the loaded one too: back to it) or, on an
// empty slot, new (a small asterisk), closing the menu; with SHIFT, cut the project
// (a question mark after the first A: a second A confirms) or
// paste one into an empty slot (dark grey while it can't: playing, recording,
// the project clipboard empty). On the status row itself:
// the menu (a small stop) on the scene cell, start from 1.1.1 (|>, as
// SHIFT+START) on the clock cell, tap tempo ("TAP") on the tempo cell; with SHIFT,
// start from 1.1.1 (play).
// While A is held it keeps showing the command it ran, on the accent colour.
// (int, not an enum class: the Arduino IDE declares functions above any types.)
enum { ICON_NONE, ICON_RECORD, ICON_STOP, ICON_PLAY, ICON_PAUSE, ICON_DELETE, ICON_TAP, ICON_CUT, ICON_PASTE, ICON_MENU, ICON_HOME, ICON_SCENE_CUT, ICON_SCENE_PASTE, ICON_MUTE, ICON_UNMUTE, ICON_RESAMPLE, ICON_ADJUST, ICON_LOAD, ICON_NEW, ICON_SAVE, ICON_USB, ICON_CHECK, ICON_RESTART };

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
  if (menuOpen() && !menuOnStatus()) {             // a project slot, or the list's items
    int slot = menuSlot();
    if (slot < 0) return menuListItem() == 0 ? ICON_SAVE : ICON_USB;
    if (shift) return projectFilled(slot) ? (cutConfirmSlot() == slot ? ICON_CHECK : ICON_CUT) : ICON_PASTE;
    return projectFilled(slot) ? ICON_LOAD : ICON_NEW;
  }
  if (board.keys() & m8::BTN_OPT) {                // B: adjusting the cell (B + arrows); with SHIFT, the scene
    if (shift) return optBeforeShift() ? ICON_SCENE_PASTE : ICON_SCENE_CUT;
    if (selected != STATUS_ROW && tracks[selected].state == TrackState::Empty && !recordingOn(selected) &&
        recordQueued() != selected)
      return ICON_RESAMPLE;                          // an empty track: B+A resamples
    if (board.keys() & m8::BTN_PLAY) return ICON_NONE;
    return selected == STATUS_ROW || hasClip(selected) ? ICON_ADJUST : ICON_NONE;
  }
  if (selected == STATUS_ROW) {                    // restart / the menu / tap tempo
    if (shift) return ICON_RESTART;
    int cell = selectedCellIn(STATUS_ROW);
    return cell == TEMPO_CELL ? ICON_TAP : cell == CLOCK_CELL ? ICON_RESTART : ICON_MENU;   // the clock cell: from 1.1.1
  }
  const Track &tr = tracks[selected];
  if (shift) return hasClip(selected) ? ICON_CUT : recordingOn(selected) || recordQueued() == selected ? ICON_DELETE : ICON_PASTE;
  if (recordQueued() == selected) return ICON_DELETE;   // takes it out of the queue
  if (recordingOn(selected)) {
    switch (recording.mode) {
      case Recording::WaitBar: return ICON_DELETE;   // cancels it
      case Recording::Running: return ICON_PLAY;     // finishes it
      default:                 return ICON_NONE;     // already finishing
    }
  }
  if (tr.state == TrackState::Empty) return ICON_RECORD;
  return tr.state == TrackState::Playing ? ICON_MUTE : ICON_UNMUTE;                               // mute / unmute
}

// The command A ran, shown dimmed while A is still held (-1: none).
static int heldAction = -1;

void noteActionPress() {
  heldAction = actionIcon();
  if (heldAction == ICON_ADJUST) heldAction = -1;  // (B held on a cell: A does nothing, the button stays)
}

// B + arrows adjusting a cell: the button looks pressed while an arrow is down.
static bool adjusting() {
  return board.keys() & (m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN);
}

// The track's own state.
static int trackIcon(int i) {
  // (Resampling: the record dot with a plus instead.)
  int rec = (recordQueued() == i ? recordQueuedResample() : recordingOn(i) && recording.resample) ? ICON_RESAMPLE : ICON_RECORD;
  if (recordQueued() == i) return flashOn() ? rec : ICON_NONE;   // waiting, like WaitBar
  if (recordingOn(i)) {
    switch (recording.mode) {
      case Recording::WaitBar: return flashOn() ? rec : ICON_NONE;
      case Recording::Running: return rec;
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
  static bool wasOpen = false;
  if (menuOpen() != wasOpen) {                     // opened or closed: the icon it shows now, at once
    wasOpen = menuOpen();
    heldAction = -1;
  }
  uint8_t act = menuOpen() ? m8::BTN_EDIT | m8::BTN_OPT | m8::BTN_PLAY : m8::BTN_EDIT;   // (the menu: B and START act too)
  if (heldAction >= 0 && !(board.keys() & act)) heldAction = -1;   // released
  return heldAction >= 0 ? heldAction : actionIcon();
}

// Icons: bitmaps of blocks ('#'), centred in the button. On the action
// button they're black (in the accent colour while A is held);
// elsewhere the accent colour for a
// recording, white for a track playing, grey otherwise. Track rows' are
// smaller, left of centre (on the selected track's panel, its left
// corners cut). An action with nothing to act
// on (paste, clipboard empty) is dark grey.
struct Icon { int w, h; const char *rows[11]; };
PROGMEM static const Icon ICONS[] = {
  {0, 0, {}},                                                                  // ICON_NONE
  {7, 7, {".......", "..###..", ".#####.", ".#####.", ".#####.", "..###..", "......."}},   // RECORD
  {7, 7, {".......", ".#####.", ".#####.", ".#####.", ".#####.", ".#####.", "......."}},   // STOP
  {7, 7, {"..#....", "..##...", "..###..", "..####.", "..###..", "..##...", "..#...."}},   // PLAY
  {7, 7, {".......", ".##.##.", ".##.##.", ".##.##.", ".##.##.", ".##.##.", "......."}},   // PAUSE
  {7, 7, {".......", ".#...#.", "..#.#..", "...#...", "..#.#..", ".#...#.", "......."}},   // DELETE
  {0, 0, {}},                                                                  // TAP (text)
  {7, 7, {"...#...", "..###..", ".#####.", "...#...", "...#...", ".......", "#######"}},   // CUT (an arrow up from a line)
  {7, 7, {"...#...", "...#...", ".#####.", "..###..", "...#...", ".......", "#######"}},   // PASTE (down to a line)
  {5, 5, {"#####", "#####", "#####", "#####", "#####"}},   // MENU (a small stop: to the menu, the clock stopped)
  {5, 5, {"..#..", "..##.", "#####", "..##.", "..#.."}},   // HOME (out of the menu: the small arrow right, as LOAD)
  {9, 9, {"#.#.#.#.#", "....#....", "#..###..#", "..#####..", "#...#...#",
          "....#....", "#.......#", ".#######.", "#.#.#.#.#"}},   // SCENE_CUT (CUT in a checkered square)
  {9, 9, {"#.#.#.#.#", "....#....", "#...#...#", "..#####..", "#..###..#",
          "....#....", "#.......#", ".#######.", "#.#.#.#.#"}},   // SCENE_PASTE
  {10, 7, {"....#.....", "...##.....", ".####.#.#.", ".####..#..", ".####.#.#.", "...##.....", "....#....."}},   // MUTE (a block right of UNMUTE)
  {9, 7, {"...#...#.", "..##.#..#", "####..#.#", "####..#.#", "####..#.#", "..##.#..#", "...#...#."}},   // UNMUTE
  {9, 7, {".........", ".###.....", "#####..#.", "#####.###", "#####..#.", ".###.....", "........."}},   // RESAMPLE (record +)
  {9, 9, {"....#....", "...###...", ".........", ".#..#..#.", "##.###.##", ".#..#..#.", ".........", "...###...", "....#...."}},   // ADJUST (B + arrows: four small arrows round a small +)
  {5, 7, {".#...", ".##..", ".###.", ".####", ".###.", ".##..", ".#..."}},   // LOAD (a small play, a block right of centre: back to the looper, playing it)
  {5, 5, {"..#..", "#.#.#", ".###.", "#.#.#", "..#.."}},   // NEW (a small asterisk)
  {8, 9, {"...##...", "...##...", "...##...", ".######.", "..####..", "...##...", "#......#", "#......#", "########"}},   // SAVE (an arrow down into a tray; a block right: centred on the backdrop)
  {7, 9, {"...#...", "..###..", "...#.##", "#..#.##", "#..#..#", ".#.#.#.", "..###..", "...#...", "..###.."}},   // USB (the trident)
  {5, 7, {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},   // CHECK (a question mark: cut the project with a second A?)
  {9, 7, {"..#.#....", "..#.##...", "..#.###..", "..#.####.", "..#.###..", "..#.##...", "..#.#...."}},   // RESTART (|>: start from 1.1.1; a block right of centre)
};
// Track rows' state icons, smaller (the status button keeps the big ones).
PROGMEM static const Icon SMALL_RECORD = {5, 5, {".###.", "#####", "#####", "#####", ".###."}};
PROGMEM static const Icon SMALL_PLAY = {3, 5, {"#..", "##.", "###", "##.", "#.."}};
PROGMEM static const Icon SMALL_PAUSE = {3, 5, {"#.#", "#.#", "#.#", "#.#", "#.#"}};
static constexpr int TRACK_ICON_CENTRE = 6;       // the column track rows' icons and number centre on
static constexpr int PANEL_LEFT = 1;              // the selected track's panel's first column (a block in)

static bool unavailable(int i, int icon) {
  if (i != STATUS_ROW) return false;
  if (menuSlot() >= 0 && !menuOnStatus()) {        // a project slot: cut and paste not while recording or copying
    if (icon == ICON_CUT || icon == ICON_CHECK) return !projectsMayCopy();
    if (icon == ICON_PASTE) return !projectsMayCopy() || !projectClipboardFull();
    if (icon == ICON_LOAD || icon == ICON_NEW)        // (not while recording, nor while it's being copied)
      return (recording.mode != Recording::Idle && menuSlot() != projectCurrent()) || projectCopying(menuSlot());
    return false;
  }
  return (icon == ICON_PASTE && !clipboardFull()) || (icon == ICON_SCENE_PASTE && !sceneClipboardFull());
}

// The action button (the status row's): the accent colour with black icons; dark grey
// with icons in the accent colour while A is held. In the menu its colour
// follows the item (menuActionColor()): a card's, or light grey on the
// system-level ones (USB data mode). The tracks' buttons: black.
// The action's colour: the menu item's, or the accent (going to the menu too).
static uint16_t actionColor(int icon) { return icon == ICON_MENU ? ACCENT : menuActionColor(); }

static uint16_t buttonBackground(int i, int icon) {
  if (i == STATUS_ROW) return heldAction >= 0 || (icon == ICON_ADJUST && adjusting()) ? DARK : actionColor(icon);
  return cellBackground(i, -1);
}

static uint16_t iconColor(int i, int icon) {
  uint16_t bg = buttonBackground(i, icon);
  if (i == STATUS_ROW) {
    if (icon == ICON_ADJUST) return adjusting() ? ACCENT : BG;
    uint16_t c = actionColor(icon);                  // (grey: an item that can't act; its icon black, held too)
    return heldAction >= 0 ? (unavailable(i, icon) || c == DIM ? BG : c) : unavailable(i, icon) ? DARK : BG;
  }
  if (unavailable(i, icon)) return DARK;
  if (bg != BG) return BG;
  if (recordingOn(i) || recordQueued() == i) return ACCENT;
  return i == STATUS_ROW || icon == ICON_PLAY ? INK : DIM;   // a track playing: white
}

// The action button's backdrop (the menu's logo follows it).
uint16_t actionBackground() { return buttonBackground(STATUS_ROW, buttonIcon(STATUS_ROW)); }

// The selected track's panel: grey; in the
// accent colour while A is held, with the action button (acting on the track).
static uint16_t markColor() { return heldAction >= 0 && heldAction != ICON_MENU ? ACCENT : DIM; }   // (going to the menu: not the track)

// The button, built whole in a buffer and sent at once.
static void drawButton(int i, int icon, uint16_t color, uint16_t bg, uint8_t badge) {
  // The action button: as tall as the status cells.
  const int height = i == STATUS_ROW ? STATUS_H : ROW_BAND;
  static uint16_t px[BUTTON_W * max(STATUS_H, ROW_BAND)];
  for (int p = 0; p < BUTTON_W * height; p++) px[p] = bg;
  auto blockIn = [&](int c, int r, uint16_t ink) {
    for (int dy = 0; dy < PX; dy++)
      for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * BUTTON_W + c * PX + dx] = ink;
  };
  const int cols = BUTTON_W / PX, rows = height / PX;
  // The selected track's panel, under its number or icons: from a block in to
  // the right edge, the button's height, its left corners cut (a two-block
  // border round the badge on the left, top and bottom). Grey; the accent
  // colour while A is held, showing just the track's number then, in black.
  uint16_t panel = i == selected && i != STATUS_ROW ? markColor() : BG;
  if (panel != BG) {
    for (int r = 0; r < rows; r++)
      for (int c = PANEL_LEFT; c < cols; c++)
        if (!(c == PANEL_LEFT && (r == 0 || r == rows - 1))) blockIn(c, r, panel);
    if (color == panel) color = BG;                // (a recording icon in the accent colour, on it)
    if (panel == ACCENT && badge != BADGE_NONE) badge = BADGE_LABEL;   // highlighted: the number alone
  }
  auto block = [&](int c, int r) { blockIn(c, r, color); };
  // A badge: a square w x h blocks with its corners cut (filled, or
  // outlined) from row r0, centred on the icons' column.
  auto drawSquare = [&](int w, int h, int r0, bool filled, uint16_t frame) {
    const int c0 = TRACK_ICON_CENTRE - w / 2;
    for (int r = 0; r < h; r++)
      for (int c = 0; c < w; c++) {
        bool side = c == 0 || c == w - 1, edge = r == 0 || r == h - 1;
        if (side && edge) continue;                  // (corners cut)
        if (filled || side || edge) blockIn(c0 + c, r0 + r, frame);
      }
  };
  // A track's badge: its number, in a square (filled, or outlined) or alone.
  // The square: 7 x 9 blocks, centred (a block of edge, a block of space
  // round the digit).
  auto drawBadge = [&](int number, bool filled, bool square, uint16_t frame, uint16_t ink) {
    const int r0 = (rows - 9) / 2;
    if (square) drawSquare(7, 9, r0, filled, frame);
    const Glyph *g = glyphFor((char)('0' + number));
    int gc = TRACK_ICON_CENTRE - glyphWidth(*g) / 2, gr = r0 + 2;
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < glyphWidth(*g); c++)
        if (g->rows[r][c] == '#') blockIn(gc + c, gr + r, ink);
  };
  if (icon == ICON_TAP) {
    // Condensed: T and A touch (T's stem and A's point keep them apart), 10
    // blocks, centred in the backdrop. Inverted: the letters in the
    // backdrop's colour, each traced by a one-block outline in the icon's
    // (the blocks next to a letter's, diagonals too), using the full width.
    const char *text = "TAP";
    const int w = textWidth(text) / PX - 1;
    int c0 = PANEL_LEFT + (cols - PANEL_LEFT - w) / 2, r0 = (rows - FONT_H / PX) / 2;   // (level with the status row's text)
    bool on[5][16] = {};                           // the word's blocks
    int x = 0;
    for (const char *p = text; *p; p++) {
      const Glyph *g = glyphFor(*p);
      for (int r = 0; r < 5; r++)
        for (int c = 0; c < glyphWidth(*g); c++)
          if (g->rows[r][c] == '#') on[r][x + c] = true;
      x += glyphWidth(*g) + (*p == 'T' ? 0 : 1);
    }
    auto letter = [&](int r, int c) { return r >= 0 && r < 5 && c >= 0 && c < w && on[r][c]; };
    for (int r = -1; r <= 5; r++)
      for (int c = -1; c <= w; c++) {
        if (letter(r, c)) {
          blockIn(c0 + c, r0 + r, bg);
          continue;
        }
        bool near = false;
        for (int dr = -1; dr <= 1; dr++)
          for (int dc = -1; dc <= 1; dc++) near |= letter(r + dr, c + dc);
        if (near) block(c0 + c, r0 + r);
      }
  } else if (icon != ICON_NONE) {
    bool track = i != STATUS_ROW;
    const Icon &ic = !track ? ICONS[icon] : icon == ICON_RECORD ? SMALL_RECORD : icon == ICON_PLAY ? SMALL_PLAY
                   : icon == ICON_PAUSE ? SMALL_PAUSE : ICONS[icon];
    int c0 = track ? TRACK_ICON_CENTRE - (ic.w - 1) / 2 : max((cols - ic.w) / 2, PANEL_LEFT + 1) + (icon == ICON_SAVE), r0 = (rows - ic.h) / 2;   // (a block clear of the backdrop's edge)
    for (int r = 0; r < ic.h; r++)
      for (int c = 0; c < ic.w; c++)
        if (ic.rows[r][c] == '#') block(c0 + c, r0 + r);
  }
  if (badge != BADGE_NONE) {                       // the track's number, maybe in a square
    // (On the panel: a muted one's outline black; a playing one's square
    // black, its number in the accent colour, on the panel in the accent colour.)
    uint16_t frame = badge == BADGE_PLAYING ? (panel == ACCENT ? BG : ACCENT) : (panel != BG ? BG : DIM);
    uint16_t ink = badge == BADGE_PLAYING ? (panel == ACCENT ? ACCENT : BG)
                 : badge == BADGE_MUTED ? (panel != BG ? BG : DIM) : badge == BADGE_LABEL || panel != BG ? BG : DARK;   // (empty: black on the panel)
    drawBadge(trackNumber(i), badge == BADGE_PLAYING, badge == BADGE_PLAYING || badge == BADGE_MUTED, frame, ink);
  }
  if (i == STATUS_ROW && bg != BG)                 // the action button: from a block in (as the
    for (int y = 0; y < height; y++)               // tracks' panels), its top left corner cut
      for (int x = 0; x < (PANEL_LEFT + (y < PX)) * PX; x++) px[y * BUTTON_W + x] = BG;
  auto &t = board.tft();
  int top = rowTop(i);
  if (bgTop(i) < top) t.fillRect(BUTTON_X, bgTop(i), BUTTON_W, top - bgTop(i), bg);   // the first row's margin
  t.writeRect(BUTTON_X, top, BUTTON_W, height, px);
  int bottom = top + height, end = bgTop(i) + bgHeight(i);
  if (bottom < end) t.fillRect(BUTTON_X, bottom, BUTTON_W, end - bottom, BG);   // the margin below
}

// The row's button (the cells: drawWave(), drawFilter(), drawMix(),
// drawStatus()). Redrawn only when it changes.
void drawCells(int i) {
  if (i < 0 || i >= ROWS) return;
  struct Key { int icon; uint16_t bg, iconColor, mark; uint8_t badge; };
  static Key shown[ROWS];
  static uint32_t drawn[ROWS];                     // uiGeneration when last drawn whole
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.icon = buttonIcon(i);
  k.bg = buttonBackground(i, k.icon);
  k.iconColor = iconColor(i, k.icon);
  k.mark = i == selected && i != STATUS_ROW ? markColor() : BG;
  k.badge = trackBadge(i);
  bool first = drawn[i] != uiGeneration;
  if (!first && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = uiGeneration;
  shown[i] = k;
  drawButton(i, k.icon, k.iconColor, k.bg, k.badge);
}
