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
// held: open the menu (a menu icon); with SHIFT too, paste a scene (B pressed
// first) or cut it (SHIFT first): the track cut / paste icons in a square.
// While the menu is open: the looper's rows (A closes it, back to them). On the status row itself:
// the menu (as B+A) on the scene cell, start / stop (play / stop, as
// START) on the clock cell, tap tempo ("TAP") on the tempo cell; with SHIFT,
// start from 1.1.1 (play).
// SHIFT tapped twice and held: resample (a cassette) on an
// empty track, overdub (the record dot with a plus; not there yet: A does
// nothing) on one with a clip.
// While A is held it keeps showing the command it ran, on the accent colour.
// (int, not an enum class: the Arduino IDE declares functions above any types.)
enum { ICON_NONE, ICON_RECORD, ICON_STOP, ICON_PLAY, ICON_PAUSE, ICON_DELETE, ICON_TAP, ICON_CUT, ICON_PASTE, ICON_MENU, ICON_HOME, ICON_SCENE_CUT, ICON_SCENE_PASTE, ICON_MUTE, ICON_UNMUTE, ICON_RESAMPLE, ICON_OVERDUB };

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
  if (menuOpen() && !menuOnStatus()) return ICON_HOME;   // A closes the menu
  if (board.keys() & m8::BTN_OPT)                  // B+A: the menu; with SHIFT, the scene
    return !shift ? (menuOpen() ? ICON_HOME : ICON_MENU) : optBeforeShift() ? ICON_SCENE_PASTE : ICON_SCENE_CUT;
  if (selected == STATUS_ROW) {                    // restart / the menu (as B+A) / START / tap tempo
    if (shift) return ICON_PLAY;
    int cell = selectedCellIn(STATUS_ROW);
    return cell == SCENE_CELL ? (menuOpen() ? ICON_HOME : ICON_MENU) : cell == CLOCK_CELL ? (transport.running ? ICON_STOP : ICON_PLAY) : ICON_TAP;
  }
  const Track &tr = tracks[selected];
  if (shiftTwiceHeld() && !recordingOn(selected) && recordQueued() != selected)   // SHIFT twice, held
    return tr.state == TrackState::Empty ? ICON_RESAMPLE : ICON_OVERDUB;
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

void noteActionPress() { heldAction = actionIcon(); }

// The track's own state.
static int trackIcon(int i) {
  // (Resampling: a cassette instead of the record dot.)
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
  if (heldAction >= 0 && !(board.keys() & m8::BTN_EDIT)) heldAction = -1;   // A released
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
  {7, 7, {".......", "#######", ".......", "#######", ".......", "#######", "......."}},   // MENU
  {7, 7, {".......", "###.#.#", ".......", "###.#.#", ".......", "###.#.#", "......."}},   // HOME (the looper's rows: waveform, filter, volume; as MENU's bars)
  {9, 9, {"#.#.#.#.#", "....#....", "#..###..#", "..#####..", "#...#...#",
          "....#....", "#.......#", ".#######.", "#.#.#.#.#"}},   // SCENE_CUT (CUT in a checkered square)
  {9, 9, {"#.#.#.#.#", "....#....", "#...#...#", "..#####..", "#..###..#",
          "....#....", "#.......#", ".#######.", "#.#.#.#.#"}},   // SCENE_PASTE
  {10, 7, {"....#.....", "...##.....", ".####.#.#.", ".####..#..", ".####.#.#.", "...##.....", "....#....."}},   // MUTE (a block right of UNMUTE)
  {9, 7, {"...#...#.", "..##.#..#", "####..#.#", "####..#.#", "####..#.#", "..##.#..#", "...#...#."}},   // UNMUTE
  {10, 7, {".########.", "#........#", "#.##..##.#", "#.##..##.#", "#........#", "#.######.#", ".########."}},   // RESAMPLE (a cassette)
  {9, 7, {".........", ".###.....", "#####..#.", "#####.###", "#####..#.", ".###.....", "........."}},   // OVERDUB (record +)
};
// Track rows' state icons, smaller (the status button keeps the big ones).
PROGMEM static const Icon SMALL_RECORD = {5, 5, {".###.", "#####", "#####", "#####", ".###."}};
PROGMEM static const Icon SMALL_PLAY = {3, 5, {"#..", "##.", "###", "##.", "#.."}};
PROGMEM static const Icon SMALL_PAUSE = {3, 5, {"#.#", "#.#", "#.#", "#.#", "#.#"}};
static constexpr int TRACK_ICON_CENTRE = 6;       // the column track rows' icons and number centre on
static constexpr int PANEL_LEFT = 1;              // the selected track's panel's first column (a block in)

static bool unavailable(int i, int icon) {
  return i == STATUS_ROW && ((icon == ICON_PASTE && !clipboardFull()) || (icon == ICON_SCENE_PASTE && !sceneClipboardFull()));
}

// The action button (the status row's): the accent colour with black icons; dark grey
// with icons in the accent colour while A is held. The tracks' buttons: black.
static uint16_t buttonBackground(int i, int icon) {
  (void)icon;
  if (i == STATUS_ROW) return heldAction >= 0 ? DARK : ACCENT;
  return cellBackground(i, -1);
}

static uint16_t iconColor(int i, int icon) {
  uint16_t bg = buttonBackground(i, icon);
  if (i == STATUS_ROW) return heldAction >= 0 ? (unavailable(i, icon) ? BG : ACCENT) : unavailable(i, icon) ? DARK : BG;
  if (unavailable(i, icon)) return DARK;
  if (bg != BG) return BG;
  if (recordingOn(i) || recordQueued() == i) return ACCENT;
  return i == STATUS_ROW || icon == ICON_PLAY ? INK : DIM;   // a track playing: white
}

// The selected track's panel: grey; in the
// accent colour while A is held, with the action button (acting on the track).
static uint16_t markColor() { return heldAction >= 0 ? ACCENT : DIM; }

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
  const int cols = BUTTON_W / PX, rows = ROW_BAND / PX;
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
  // The square: 7 x 9 blocks from row 2 (a block of edge, a block of space
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
    // blocks, centred in the backdrop.
    const char *text = "TAP";
    const int w = textWidth(text) / PX - 1;
    int c0 = PANEL_LEFT + (cols - PANEL_LEFT - w) / 2, r0 = (rows - FONT_H / PX) / 2;   // (level with the status row's text)
    for (const char *p = text; *p; p++) {
      const Glyph *g = glyphFor(*p);
      for (int r = 0; r < 5; r++)
        for (int c = 0; c < glyphWidth(*g); c++)
          if (g->rows[r][c] == '#') block(c0 + c, r0 + r);
      c0 += glyphWidth(*g) + (*p == 'T' ? 0 : 1);
    }
  } else if (icon != ICON_NONE) {
    bool track = i != STATUS_ROW;
    const Icon &ic = !track ? ICONS[icon] : icon == ICON_RECORD ? SMALL_RECORD : icon == ICON_PLAY ? SMALL_PLAY
                   : icon == ICON_PAUSE ? SMALL_PAUSE : ICONS[icon];
    int c0 = track ? TRACK_ICON_CENTRE - (ic.w - 1) / 2 : max((cols - ic.w) / 2, PANEL_LEFT + 1), r0 = (rows - ic.h) / 2;   // (a block clear of the backdrop's edge)
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
