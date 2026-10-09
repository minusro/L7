// ui_buttons.cpp — the row buttons and their icons (see ui.h)
#include "ui.h"
#include "input.h"
#include "ui_font.h"
#include "audio_follow.h"

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
// accent colour) while an arrow is down; A does nothing then; on the tempo cell (no arrow
// down): the metronome (B+A: its next setting; with SHIFT instead, audio follow: a jack plug with an arrow, crossed while it's on); on an empty track: resample
// (the record dot with a plus: B+A); with SHIFT too, paste a scene (B pressed
// first) or cut it (SHIFT first): the track cut / paste icons in a checkered square (9 x 9, as the duplicate one).
// While the menu is open (on its items A runs the action when let go; B does
// nothing): on its
// list's items, export Ableton project (an arrow into a tray) and storage mode (an
// arrow up out of a tray, export's reversed), nothing yet; on a project slot, load (a small arrow right; the loaded one too: back to it) or, on an
// empty slot, new (a small asterisk), closing the menu; with SHIFT, cut the project
// or paste one into an empty slot (dark grey while it can't: playing, recording,
// the project clipboard empty). On the status row itself:
// the next scene (two small plays) on the scene cell (SHIFT: duplicate it, a + in a checkered square), play / stop (a play
// or a stop icon) on the clock cell, tap tempo ("TAP") on the tempo cell; with SHIFT,
// start from 1.1.1 (play).
// While A is held it keeps showing the command it ran, on the accent colour.
// (int, not an enum class: the Arduino IDE declares functions above any types.)
enum { ICON_NONE, ICON_RECORD, ICON_STOP, ICON_PLAY, ICON_PAUSE, ICON_DELETE, ICON_TAP, ICON_CUT, ICON_PASTE, ICON_HOME, ICON_SCENE_CUT, ICON_SCENE_PASTE, ICON_MUTE, ICON_UNMUTE, ICON_RESAMPLE, ICON_ADJUST, ICON_LOAD, ICON_NEW, ICON_SAVE, ICON_USB, ICON_CHECK, ICON_RESTART, ICON_SCENE_DUP, ICON_NEXT, ICON_METRO, ICON_MOVE, ICON_EAR };

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
    if (slot < 0) return menuListItem() == 0 ? ICON_SAVE : menuListItem() == 1 ? ICON_USB : ICON_NONE;   // (settings: off for now)
    if (shift) return projectFilled(slot) ? ICON_CUT : ICON_PASTE;
    if (slot == projectCurrent()) return ICON_HOME;   // (the loaded one: back to the looper)
    return projectFilled(slot) ? ICON_LOAD : ICON_NEW;
  }
  if (board.keys() & m8::BTN_OPT) {                // B: adjusting the cell (B + arrows); with SHIFT, the scene
    if (shift) return optBeforeShift() ? ICON_SCENE_PASTE : ICON_SCENE_CUT;
    if (selected != STATUS_ROW && tracks[selected].state == TrackState::Empty && !recordingOn(selected) &&
        recordQueued() != selected)
      return ICON_RESAMPLE;                          // an empty track: B+A resamples (unavailable with no clip playing unmuted)
    if (board.keys() & m8::BTN_PLAY) return ICON_NONE;
    const uint8_t arrows = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
    if (selected == STATUS_ROW && selectedCellIn(STATUS_ROW) == TEMPO_CELL && !(board.keys() & arrows))
      return ICON_METRO;                             // the tempo cell: B+A, the metronome's next setting
    if (selected == STATUS_ROW) return ICON_ADJUST;
    return hasClip(selected) ? ICON_ADJUST : ICON_NONE;
  }
  if (selected == STATUS_ROW) {                    // restart / duplicate / play-stop / tap tempo
    int cell = selectedCellIn(STATUS_ROW);
    if (shift) return cell == SCENE_CELL ? ICON_SCENE_DUP : cell == TEMPO_CELL ? ICON_EAR : ICON_RESTART;   // (SHIFT: the scene cell duplicates it; the tempo cell: audio follow)
    return cell == TEMPO_CELL ? ICON_TAP : cell == CLOCK_CELL ? (transport.running ? ICON_STOP : ICON_PLAY) : ICON_NEXT;   // the scene cell: the next scene
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

static bool unavailable(int i, int icon);

void noteActionPress() {
  heldAction = actionIcon();
  if (heldAction == ICON_ADJUST) heldAction = -1;  // (B held on a cell: A does nothing, the button stays)
  if (heldAction >= 0 && unavailable(STATUS_ROW, heldAction)) heldAction = -1;   // (nothing happens: no pressed look)
}

// B + arrows adjusting a cell: the button looks pressed while an arrow is down,
// and while A is held with B (A does nothing yet: for finer adjusting later).
static bool adjusting() {
  return board.keys() & (m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN | m8::BTN_EDIT);
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
  // B held on the selected empty track (B + UP/DOWN moves it): up and down
  // arrows instead of its number.
  if (i == selected && bAlone() && unitOf(i).sample == 0) return ICON_MOVE;
  return ICON_NONE;                                // (the track's number shows its state)
}

// A track row's number badge: the track's number (counted from the bottom),
// dark grey on an empty track; in a square outlined grey (number grey) on a
// muted one; in a square filled with the accent colour (number black) on one
// playing. Hidden on the recording (or queued) track, whose icons show instead.
enum : uint8_t { BADGE_NONE, BADGE_EMPTY, BADGE_MUTED, BADGE_PLAYING, BADGE_LABEL };   // LABEL: the number alone, black

static uint8_t trackBadge(int i) {
  if (i == STATUS_ROW || recordingOn(i) || recordQueued() == i || trackIcon(i) == ICON_MOVE) return BADGE_NONE;   // (its icons instead)
  if (!shownClip(i)) return BADGE_EMPTY;          // (as the row shows it: a switch all at once)
  return shownTrack(i).state == TrackState::Muted ? BADGE_MUTED : BADGE_PLAYING;
}

int trackNumber(int i) { return NUM_TRACKS - i; }

static int buttonIcon(int i) {
  if (i != STATUS_ROW) return trackIcon(i);
  static bool wasOpen = false;
  if (menuOpen() != wasOpen) {                     // opened or closed: the icon it shows now, at once
    wasOpen = menuOpen();
    heldAction = -1;
  }
  uint8_t act = menuOpen() ? m8::BTN_EDIT | m8::BTN_OPT : m8::BTN_EDIT;   // (the menu: B acts too)
  if (heldAction >= 0 && !(board.keys() & act)) heldAction = -1;   // released
  return heldAction >= 0 ? heldAction : actionIcon();
}

// Icons: bitmaps of blocks ('#'), centred in the button. On the action
// button they're black (in the accent colour while A is held);
// elsewhere the accent colour for a
// recording, white for a track playing, grey otherwise. Track rows' are
// smaller, left of centre (on the selected track's panel, its left
// corners cut). An action with nothing to act
// on (paste, clipboard empty; resample, no clip playing unmuted; export,
// no clips) greys the action button, its icon black, pressed or not.
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
  {5, 5, {"..#..", "..##.", "#####", "..##.", "..#.."}},   // HOME (out of the menu: the small arrow right, as LOAD)
  {9, 9, {"#.#.#.#.#", "....#....", "#..###..#", "..#####..", "#...#...#",
          "....#....", "#.......#", ".#######.", "#.#.#.#.#"}},   // SCENE_CUT (CUT in a checkered square)
  {9, 9, {"#.#.#.#.#", "....#....", "#...#...#", "..#####..", "#..###..#",
          "....#....", "#.......#", ".#######.", "#.#.#.#.#"}},   // SCENE_PASTE
  {10, 7, {"....#.....", "...##.....", ".####.#.#.", ".####..#..", ".####.#.#.", "...##.....", "....#....."}},   // MUTE (a block right of UNMUTE)
  {9, 7, {"...#...#.", "..##.#..#", "####..#.#", "####..#.#", "####..#.#", "..##.#..#", "...#...#."}},   // UNMUTE
  {9, 7, {".........", ".###.....", "#####..#.", "#####.###", "#####..#.", ".###.....", "........."}},   // RESAMPLE (record +)
  {9, 9, {"....#....", "...###...", ".........", ".#..#..#.", "##.###.##", ".#..#..#.", ".........", "...###...", "....#...."}},   // ADJUST (B + arrows: four small arrows round a small +; a block right of centre)
  {5, 5, {"..#..", "..##.", "#####", "..##.", "..#.."}},   // LOAD (a small arrow right: into the project)
  {5, 5, {"..#..", "#.#.#", ".###.", "#.#.#", "..#.."}},   // NEW (a small asterisk)
  {8, 9, {"...##...", "...##...", "...##...", ".######.", "..####..", "...##...", "#......#", "#......#", "########"}},   // SAVE (an arrow down into a tray; a block right: centred on the backdrop)
  {8, 9, {"...##...", "..####..", ".######.", "...##...", "...##...", "...##...", "#......#", "#......#", "########"}},   // USB (an arrow up out of a tray: SAVE reversed; a block right as it)
  {5, 7, {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},   // CHECK (a question mark: cut the project with a second A?)
  {9, 7, {"..#.#....", "..#.##...", "..#.###..", "..#.####.", "..#.###..", "..#.##...", "..#.#...."}},   // RESTART (|>: start from 1.1.1; a block right of centre)
  {9, 9, {"#.#.#.#.#", ".........", "#...#...#", "....#....", "#.#####.#",
          "....#....", "#...#...#", ".........", "#.#.#.#.#"}},   // SCENE_DUP (a + in a checkered square, as SCENE_CUT / PASTE)
  {7, 5, {"#...#..", "##..##.", "###.###", "##..##.", "#...#.."}},   // NEXT (two small plays, a block apart: the next scene)
  {0, 0, {}},                                                                  // METRO (drawn on its own: a metronome and the next setting)
  {3, 5, {".#.", "###", "...", "###", ".#."}},   // MOVE (a track's: up and down, a block apart: B + UP/DOWN moves the empty track)
  {0, 0, {}},                                                                  // EAR (drawn on its own: the input, a jack plug, crossed while following)
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
    if (icon == ICON_CUT) return !projectsMayCopy();
    if (icon == ICON_PASTE) return !projectsMayCopy() || !projectClipboardFull();
    if (icon == ICON_LOAD || icon == ICON_NEW)        // (not while recording, nor while it's being copied)
      return (recording.mode != Recording::Idle && menuSlot() != projectCurrent()) || projectCopying(menuSlot());
    return false;
  }
  return (icon == ICON_PASTE && !clipboardFull()) || (icon == ICON_SCENE_PASTE && !sceneClipboardFull()) ||
         (icon == ICON_RESAMPLE && !anyClipHeard());
}

// The action button (the status row's): the accent colour with black icons; dark grey
// (in the menu, on its dark grey ground: black) with icons in the accent colour while A is held. In the menu its colour
// follows the item (menuActionColor()): a card's, or light grey on the
// system-level ones (storage mode). The tracks' buttons: black.
// The action's colour: the menu item's, or the accent.
static uint16_t actionColor(int) { return menuActionColor(); }
// An action that can't act now: grey (a menu item's own grey too: export with nothing to export).
static bool cannotAct(int icon) { return unavailable(STATUS_ROW, icon) || actionColor(icon) == DIM; }

// B held on a track (A too or not; SHIFT not the arrows' modifier): its
// panel in the accent colour, the action button grey with its icon in the accent colour.
static bool bOnTrack() {
  return selected != STATUS_ROW && !menuOpen() && (board.keys() & m8::BTN_OPT) && arrowModifier() != m8::BTN_SHIFT;
}
// The same for the action button with B on the clock cell (the master volume; the panel: a track's only).
static bool bAdjusting() {
  return bOnTrack() || (selected == STATUS_ROW && selectedCellIn(STATUS_ROW) == CLOCK_CELL && !menuOpen() && bArrows());   // (A held too or not)
}

static uint16_t buttonBackground(int i, int icon) {
  if (i == STATUS_ROW) {
    if (cannotAct(icon)) return DIM;               // (no pressed look either)
    if (bAdjusting()) return DIM;
    if (heldAction >= 0 || (icon == ICON_ADJUST && adjusting())) return menuOpen() ? BG : DARK;   // pressed (the menu's ground is dark grey: black there)
    return actionColor(icon);
  }
  return cellBackground(i, -1);
}

// The action button's colour when not pressed (the menu's logo follows it).
uint16_t actionRestColor() {
  int icon = buttonIcon(STATUS_ROW);
  return cannotAct(icon) ? DIM : actionColor(icon);
}

static uint16_t iconColor(int i, int icon) {
  uint16_t bg = buttonBackground(i, icon);
  if (i == STATUS_ROW) {
    if (bAdjusting() && !cannotAct(icon)) return ACCENT;
    if (icon == ICON_ADJUST) return adjusting() ? ACCENT : BG;
    if (cannotAct(icon)) return BG;                // (grey, its icon black, held too)
    return heldAction >= 0 ? actionColor(icon) : BG;
  }
  if (unavailable(i, icon)) return DARK;
  if (icon == ICON_MOVE) return ACCENT;            // (on the selected track's grey panel)
  if (bg != BG) return BG;
  if (recordingOn(i) || recordQueued() == i) return ACCENT;
  return i == STATUS_ROW || icon == ICON_PLAY ? INK : DIM;   // a track playing: white
}

// The selected track's panel: grey; in the
// accent colour while A is held, with the action button (acting on the track).
static uint16_t markColor() {                     // (B held on a track, or SHIFT the arrows' modifier there: the accent colour too)
  return heldAction >= 0 || bOnTrack() || (selected != STATUS_ROW && !menuOpen() && shiftArrows()) ? ACCENT : DIM;
}

// The button, built whole in a buffer and sent at once.
static void drawButton(int i, int icon, uint16_t color, uint16_t bg, uint8_t badge) {
  // The action button pressed (A held, or B + an arrow adjusting): a block
  // narrower on the left (its icon where it was); else it reaches the
  // button's place's left edge.
  const bool pressed = i == STATUS_ROW && !cannotAct(icon) && (heldAction >= 0 || (icon == ICON_ADJUST && adjusting()));
  // The action button: as tall as the status cells.
  const int height = i == STATUS_ROW ? STATUS_H : ROW_BAND;
  static uint16_t px[BUTTON_W * max(STATUS_H, ROW_BAND)];
  // A track's: on the selected track's dark grey ground (rowGround(); its
  // top block row shared with the row above's ground, as the cells' borders).
  const uint16_t ground = i != STATUS_ROW && bg == BG ? rowGround(i) : bg;
  const uint16_t topGround = i != STATUS_ROW && bg == BG && rowGround(rowAbove(i)) == DARK ? DARK : ground;
  for (int p = 0; p < BUTTON_W * height; p++) px[p] = p < BUTTON_W * PX ? topGround : ground;
  auto blockIn = [&](int c, int r, uint16_t ink) {
    for (int dy = 0; dy < PX; dy++)
      for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * BUTTON_W + c * PX + dx] = ink;
  };
  const int cols = BUTTON_W / PX, rows = height / PX;
  // The selected track's panel, under its number or icons: from a block in to
  // the right edge, the button's height but its top and bottom block rows, its left
  // corners cut (a border round the badge: a block on top, two on the left and
  // bottom). Grey; the accent
  // colour while A is held, showing just the track's number then, in black.
  uint16_t panel = i == selected && i != STATUS_ROW ? markColor() : BG;
  if (panel != BG) {
    for (int r = 1; r < rows - 1; r++)            // (not the top block row, nor the bottom one)
      for (int c = PANEL_LEFT; c < cols; c++)         // (to the right edge)
        if (!(c == PANEL_LEFT && (r == 1 || r == rows - 2))) blockIn(c, r, panel);   // (the left corners cut)
    if (color == panel) color = BG;                // (a recording icon in the accent colour, on it)
    if (panel == ACCENT && badge != BADGE_NONE) badge = BADGE_LABEL;   // highlighted: the number alone
  }
  auto block = [&](int c, int r) { blockIn(c, r, color); };
  // A badge: a square w x h blocks with its bottom corners cut (filled, or
  // outlined) from row r0, centred on the icons' column.
  auto drawSquare = [&](int w, int h, int r0, bool filled, uint16_t frame) {
    const int c0 = TRACK_ICON_CENTRE - w / 2;
    for (int r = 0; r < h; r++)
      for (int c = 0; c < w; c++) {
        bool side = c == 0 || c == w - 1, edge = r == 0 || r == h - 1;
        if (side && edge && r == h - 1) continue;    // (the bottom corners cut; the top ones kept)
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
    // "TAP" in the font's letters, a block apart (11 x 5 blocks, the P's bowl
    // closed), in the icon's colour (black), from the button's left block
    // column in; a block lower while A is held (pressed).
    static const char *const TAP[5] = {"###.###.###", ".#..#.#.#.#", ".#..###.###", ".#..#.#.#..", ".#..#.#.#.."};
    const int w = 11, h = 5;
    int c0 = PANEL_LEFT, r0 = (rows - h) / 2 + (heldAction >= 0);
    for (int r = 0; r < h; r++)
      for (int c = 0; c < w; c++)
        if (TAP[r][c] == '#') block(c0 + c, r0 + r);
  } else if (icon == ICON_METRO || icon == ICON_EAR) {
    // The metronome: four beats as dots (2 x 2), the bar's first raised above
    // the other three, 10 x 10, a block of margin round it at least;
    // crossed out while it would click with the clock running (what B+A will
    // change). Or the input (a jack plug from the right, an arrow pointing in
    // above it), the same size, crossed out while audio follow is on (what SHIFT+A will change). The
    // cross: a slash from the top right to the bottom left, cleared a block
    // either side.
    static const char *const METRONOME[10] = {   // (the beats: the bar's first raised, three after it)
        "..........", "..........", "..........", "##........", "##........",
        "..........", "..##.##.##", "..##.##.##", "..........", ".........."};
    static const char *const EAR[10] = {   // (audio follow: a jack plug coming in from the right, an arrow above it pointing in)
        "...#......", "..##......", ".#######..", "..##......", "...#......",
        "..........", "......####", ".##.######", ".##.######", "......####"};
    const char *const *art = icon == ICON_EAR ? EAR : METRONOME;
    constexpr int W = 10, H = 10;
    int c0 = PANEL_LEFT, r0 = (rows - H) / 2;
    bool crossed = icon == ICON_EAR ? audioFollowOn() : metronomeOn;   // (the metronome: it would click were the clock running, metronomeOn, kept up to date running or not)
    for (int r = 0; r < H; r++)
      for (int c = 0; c < W; c++) {
        bool on = art[r][c] == '#';
        if (crossed) {                             // the cross: a slash from the top right to the bottom left, a gap either side of it
          int s = (W - 1) - r * (W - 1) / (H - 1);
          if (c == s) on = true;
          else if (c == s - 1 || c == s + 1) on = false;
        }
        if (on) block(c0 + c, r0 + r);
      }
  } else if (icon != ICON_NONE) {
    bool track = i != STATUS_ROW;
    const Icon &ic = !track ? ICONS[icon] : icon == ICON_RECORD ? SMALL_RECORD : icon == ICON_PLAY ? SMALL_PLAY
                   : icon == ICON_PAUSE ? SMALL_PAUSE : ICONS[icon];
    int c0 = (track ? TRACK_ICON_CENTRE - (ic.w - 1) / 2 : max((cols - ic.w) / 2, PANEL_LEFT + 1) + (icon == ICON_SAVE || icon == ICON_USB || icon == ICON_RESAMPLE)) +
             0,
      r0 = (rows - ic.h) / 2;   // (a block clear of the backdrop's edge)
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
  const uint16_t round = i == STATUS_ROW ? (menuOpen() ? MENU_BG : BG) : rowGround(i);   // (round the button: the menu's ground while it's open; a selected track's; the status row's highlight ends at the tempo cell)
  if (i == STATUS_ROW && bg != round)              // the action button: from its place's left edge (pressed: a block in, as the
    for (int y = 0; y < height; y++)               // tracks' panels), its top left and bottom left corners cut
      for (int x = 0; x < ((pressed ? PANEL_LEFT : 0) + (y < PX || y >= height - PX)) * PX; x++) px[y * BUTTON_W + x] = round;
  auto &t = board.tft();
  int top = rowTop(i);
  if (bgTop(i) < top) t.fillRect(BUTTON_X, bgTop(i), BUTTON_W, top - bgTop(i), bg);   // the first row's margin
  t.writeRect(BUTTON_X, top, BUTTON_W, height, px);
  int bottom = top + height, end = bgTop(i) + bgHeight(i);
  if (i != STATUS_ROW) {                           // the block's space before it: the ground too
    t.fillRect(BUTTON_X - PX, top, PX, PX, topGround);
    t.fillRect(BUTTON_X - PX, top + PX, PX, height - PX, ground);
  } else if (!menuOpen()) {
    t.fillRect(BUTTON_X - PX, top, PX, height, round);
  }
  if (bottom < end) {                              // the margin below: the last track's ground's last block row (as under its cells), then black
    int groundEnd = i != STATUS_ROW ? min(end, bottom + PX) : bottom;
    if (groundEnd > bottom) t.fillRect(BUTTON_X - PX, bottom, BUTTON_W + PX, groundEnd - bottom, ground);
    if (end > groundEnd) t.fillRect(BUTTON_X - PX, groundEnd, BUTTON_W + PX, end - groundEnd, i == STATUS_ROW ? round : BG);
  }
}

// The row's button (the cells: drawWave(), drawFilter(), drawMix(),
// drawStatus()). Redrawn only when it changes.
void drawCells(int i) {
  if (i < 0 || i >= ROWS || (i == STATUS_ROW && menuOpen())) return;   // (the menu: no action button)
  struct Key { int icon; uint16_t bg, iconColor, mark, ground, topGround; uint8_t badge, metro; };
  static Key shown[ROWS];
  static uint32_t drawn[ROWS];                     // uiGeneration when last drawn whole
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.icon = buttonIcon(i);
  k.bg = buttonBackground(i, k.icon);
  k.iconColor = iconColor(i, k.icon);
  k.mark = i == selected && i != STATUS_ROW ? markColor() : BG;
  k.badge = trackBadge(i);
  k.metro = k.icon == ICON_METRO ? (uint8_t)(metronomeMode() + 4 * metronomeOn)   // (its look: the setting now, crossed while clicking;
          : k.icon == ICON_EAR ? (uint8_t)(8 + audioFollowOn()) : 0;                   // the input: crossed while following)
  k.ground = rowGround(i);                         // (the selected row's ground, under the button too)
  if (i != STATUS_ROW) k.topGround = rowGround(rowAbove(i));
  bool first = drawn[i] != uiGeneration;
  if (!first && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = uiGeneration;
  shown[i] = k;
  drawButton(i, k.icon, k.iconColor, k.bg, k.badge);
}
