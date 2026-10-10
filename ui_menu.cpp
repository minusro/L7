// ui_menu.cpp — the menu (see ui.h): the looper's other screen, to its left,
// the whole screen (no action button). Opened with any arrow off the looper's
// edges; left by loading a project (A on the loaded one:
// just back), or any arrow off its edges (menuLeavesOn()). Twelve cards (three rows of
// four), the project slots, and under them a list, "EXPORT ABLETON SET",
// "STORAGE MODE", "SETTINGS" (the settings popup), "HELP" (not yet,
// grey); it scrolls once it has more than four. A runs
// the item's action when let go. On a slot, A loads its project (an
// empty one: a new project there), the menu staying open; on the loaded
// project's card, A closes the menu; with SHIFT, cuts the project, or pastes
// one into an empty slot. B acts as A; START starts / stops the clock. Opening it stops
// the clock; loading a project (or making a new one) starts it.
//
// Items: the cards 0-11 (A-L, rows of four, top first: A-D on top, I-L
// above the list), then the list's 12-15. UP/DOWN go card row to
// card row and on into the list (back up: to the column it came from, on the
// I-L row); LEFT/RIGHT along a card row (not on to another). Switching
// screens, each screen's selection is where it was last left (row and
// column); off an edge, it lands on the other screen's far edge (UP/DOWN: its
// bottom / top row, at the column it was left on; LEFT/RIGHT: its right / left
// side, in the row it was left on: menuLand(), and input.cpp's leaveMenu()).
//
// The ground: dark grey (MENU_BG), the whole screen's.
// A card: a block shape, the bottom right corner cut diagonally (3 blocks),
// in its slot's colour (colors.h): the loaded project's
// filled, its letter A-L (the project's name) black;
// another project's a thick (two-block) outline, the letter in the
// slot's colour; empty, a grey outline with the letter grey (its colour while an action button is held). The selected item's frame looks and behaves as the looper's
// cells' (frameColor()), corners cut, a block out from a card (following its
// shape too), three from the list's box (the longest label's size, centred;
// labels left-aligned, light grey, white when selected). Bottom left, the
// logo small, in the item's colour (actionRestColor()). No mark for the way
// out (any edge, menuLeavesOn()).
#include "ui.h"
#include "input.h"
#include "ui_font.h"

uint32_t uiGeneration = 1;

// The list: its items, LIST_ROOM of them in view at a time (scrolling; with
// no more than that, from the top of its room).
static constexpr int CARDS = PROJECTS, LIST = 4, LIST_ROOM = 4, ITEMS = CARDS + LIST, COLS = 4, CARD_ROWS = CARDS / COLS;
static const char *const LABELS[LIST] = {"EXPORT ABLETON SET", "STORAGE MODE", "SETTINGS", "HELP"};   // (ui.h MENU_EXPORT...)
// The export item's label while exporting and after it ("EXPORTING 3/14",
// "SAVED AS L7 A005"; none: its own), kept (the menu closed and opened again,
// storage mode too) until the project changes (projectEdits()) or another one
// is loaded (the project's name in the project's colour: from exportNameAt on).
static char exportText[24];
static int exportNameAt = -1, exportFor = -1;
static uint32_t exportEdits = 0;                 // projectEdits() when it was set
static void dropStaleExport() {
  if (exportText[0] && (projectCurrent() != exportFor || projectEdits() != exportEdits)) exportText[0] = 0;
}
static int listTop = 0;                          // the list's first item in view

static bool shown = false;
static int menuSel = 0;                          // the selected item
static bool setOpen = false, setDrawn = false;   // the settings popup open (below); setDrawn: its box on screen (else drawn whole next)
static int setSel = 0;                           // its selected item (0: the bottom one)
static uint32_t setPressAt = 0;                  // millis() of the latest A or B press in it (0: none)
static bool helpOpen = false;                    // the help pages open (over the whole screen)
static int cardCol = 0;                          // the card column last selected (UP from the list goes back to it)
static bool onStatus = false;                    // the selection is on the status row (selected = STATUS_ROW meanwhile)
static int statusCell = SCENE_CELL;              // where on it, while the menu's closed (or the selection's up in the menu)
static int mainRow = 0, mainCell = 0;            // the looper's selection, back when the menu closes

bool menuOpen() { return shown; }
bool menuOnStatus() { return shown && onStatus; }
int menuSlot() { return shown && !onStatus && menuSel < CARDS ? menuSel : -1; }
bool menuOnList() { return shown && !onStatus && menuSel >= CARDS; }
int menuListItem() { return menuOnList() ? menuSel - CARDS : -1; }

int menuState() {
  int sel = onStatus ? 100 + (shown ? selectedCellIn(STATUS_ROW) : statusCell) : menuSel;
  return sel + (shown ? 1000 : 0);
}

void mainSelection(int &row, int &cell) {
  row = shown ? mainRow : selected;
  cell = shown ? mainCell : selectedCell;
}

void setMainSelection(int row, int cell) {
  (shown ? mainRow : selected) = row;
  (shown ? mainCell : selectedCell) = cell;
}

// ---- Shapes, in blocks: an item's content is w x h blocks from (x, y); a card
// (cut > 0) has a block missing from each top corner and a cut x cut block
// from each bottom corner (0: a plain rectangle). shape(d) is the content
// grown by d blocks all round (the notch keeps its size); a frame is a ring
// between two of them.
struct Shape { int x, y, w, h, cut; };

// Row r (screen block row) of shape s grown by d: its first and last column
// (false: the row isn't in it). round: the corner blocks left out.
static bool rowSpan(const Shape &s, int d, int r, int &c0, int &c1, bool round = false) {
  int y = r - s.y;
  if (y < -d || y > s.h - 1 + d) return false;
  c0 = -d;
  c1 = s.w - 1 + d;
  bool top = y == -d, bottom = y == s.h - 1 + d;
  if (s.cut) {                                    // a card: the bottom right cut diagonally (cut blocks), the
    c1 = min(c1, s.w + s.h - s.cut - 2 + d - y);  // others square: the diagonal: x + y at most this
    if (round && top) { c0++; c1--; }              // (round: its three square corners cut a block)
    if (round && bottom) c0++;
  } else if (round && (top || bottom)) {          // the top and bottom rows: their corners cut
    c0++;
    c1--;
  }
  if (c1 < c0) return false;
  c0 += s.x;
  c1 += s.x;
  return true;
}

// Fills shape(outer) less shape(inner) (inner < outer) in `color`; round:
// the outer shape's corners cut.
static void fillRing(const Shape &s, int outer, int inner, uint16_t color, bool round = false) {
  auto &t = board.tft();
  for (int r = s.y - outer; r <= s.y + s.h - 1 + outer; r++) {
    int a0, a1, b0, b1;
    if (!rowSpan(s, outer, r, a0, a1, round)) continue;
    if (!rowSpan(s, inner, r, b0, b1)) {
      t.fillRect(a0 * PX, r * PX, (a1 - a0 + 1) * PX, PX, color);
      continue;
    }
    if (b0 > a0) t.fillRect(a0 * PX, r * PX, (b0 - a0) * PX, PX, color);
    if (a1 > b1) t.fillRect((b1 + 1) * PX, r * PX, (a1 - b1) * PX, PX, color);
  }
}

// ---- Layout, in blocks (the screen: 160 x 107 above the status row).
// Cards: 21 x 15, the cut 3, 3 apart across and 3 down (neighbouring
// rows' frames share a row: only the selected one's shows), the last row ending at row 64 (the first from 13).
// The list: centred under them, an item every 12 rows from row 75, on down into the status row's place (its
// cells are hidden in the menu; the action button is clear to the right), its frame
// round its label with 3 blocks of space.
static constexpr int CARD_W = 21, CARD_H = 15, CARD_CUT = 3, CARD_GAP = 3, CARD_VGAP = 3;
static constexpr int CARDS_X = (320 / PX - COLS * CARD_W - (COLS - 1) * CARD_GAP) / 2, CARDS_Y = 13 + 2;   // (2 blocks lower)
static constexpr int LIST_PITCH = 12, LIST_SPACE = 3;   // (neighbouring frames share a row: only one shows)
// The first place where it was with four places, the last of them level with
// the action button (from the status row's top); the fourth place gone (the
// logo's row clear).
static constexpr int LIST_Y = STATUS_TOP / PX + LIST_SPACE + 1 - 3 * LIST_PITCH;   // 75

// The logo in the bottom left corner (CORNER_LOGO_X / Y: a block in, on the status row's line).
static constexpr int LOGO_X = CORNER_LOGO_X, LOGO_Y = CORNER_LOGO_Y;

static Shape itemShape(int k) {
  if (k < CARDS)
    return {CARDS_X + (k % COLS) * (CARD_W + CARD_GAP), CARDS_Y + (k / COLS) * (CARD_H + CARD_VGAP), CARD_W, CARD_H, CARD_CUT};
  // A list item: the box fitting the longest label, centred (its label
  // left-aligned in it); at least as wide as "EXPORT ABLETON PROJECT" was (the
  // list where it's always been, its labels shorter now).
  int w = textWidth("EXPORT ABLETON PROJECT") / PX;
  for (const char *label : LABELS) w = max(w, textWidth(label) / PX);
  return {(320 / PX - w) / 2, LIST_Y + (k - CARDS - listTop) * LIST_PITCH, w, FONT_H / PX, 0};   // (in view: listInView())
}

int menuListX() { return itemShape(CARDS).x * PX; }
int menuHelpY() { return (LIST_Y + MENU_HELP * LIST_PITCH) * PX; }   // (its place with the list unscrolled: all four in view)

// The selected item's frame, as the looper's cells': light grey; white while
// only arrows are held and the last press moved the selection; grey while A
// is held (B: no change, it only leads to leaving); on a card, in the
// project's colour while an action button (A or B) is held.
static bool movedHere = false;                   // the last arrow press moved the menu's selection
// An action button held (A or B).
static bool actionHeld() { return board.keys() & (m8::BTN_EDIT | m8::BTN_OPT); }

static uint16_t frameColor() {
  uint8_t k = board.keys();
  const uint8_t arrows = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  if (screenSwitchPending()) return MENU_BG;       // (leaving: no frame meanwhile, the edge arrow bright)
  constexpr uint32_t PRESS_FLASH_MS = 100;         // A or B just pressed on a card: its frame white a moment
  if (menuSel < CARDS && !onStatus && menuCardPressMs() && millis() - menuCardPressMs() < PRESS_FLASH_MS) return INK;
  if (menuSel < CARDS && !onStatus && actionHeld())
    return PROJECT_COLORS[menuSel];                // an action button held on a card: its frame in the project's colour
  return k & m8::BTN_EDIT ? DIM : k && !(k & ~arrows) && movedHere ? INK : LIGHT;
}

static bool listInView(int k) { return k < CARDS || (k - CARDS >= listTop && k - CARDS < listTop + LIST_ROOM); }

// Scrolls the list after its selection moved to item i (dir: 1 down, -1 up,
// 0: just into view): the item past it that way stays in view where there is
// one; the view never runs past the list's ends (no empty places).
static void scrollList(int i, int dir) {
  if (dir > 0) listTop = max(listTop, min(i + 1, LIST - 1) - (LIST_ROOM - 1));
  if (dir < 0) listTop = min(listTop, max(i - 1, 0));
  listTop = constrain(listTop, i - (LIST_ROOM - 1), i);   // (in view)
  listTop = constrain(listTop, 0, max(0, LIST - LIST_ROOM));
}

static bool playPulse();

// The card shown as the loaded project's (filled, its letter black, the play
// icon): the loaded one's; but from letting go of the A or B that loaded it
// (loaded on the press: while it's held, steady) until the new project has
// really come in (playing: its switch point, the next bar line), it flashes
// every eighth: on for the first half of each beat (so on at the switch, N.1.1),
// a project's outline for the second. The one before shows as not loaded
// from the press on.
static int loadFrom = -1;                        // the project loaded before, while the new one's on its way (-1: none on its way)

void menuNoteLoad(int from) {
  loadFrom = from >= 0 && from != projectCurrent() && sceneChangeWaiting() ? from : -1;
}

bool menuLoadPending() {
  if (loadFrom >= 0 && !sceneChangeWaiting()) loadFrom = -1;   // (come in: as it is)
  return loadFrom >= 0;
}

static int shownLoaded() {
  int now = projectCurrent();
  menuLoadPending();
  if (loadFrom >= 0 && !actionHeld()) {
    noInterrupts();
    bool first = transport.sampleInBeat < transport.spb / 2;
    interrupts();
    return first ? now : -1;                     // (none: the one before stays a project's outline)
  }
  return now;
}

// The card with the play icon (the clock running; its letter 2 blocks left to
// make room): the project heard, the one before until the new one comes in.
static int playingCard() {
  return menuLoadPending() ? loadFrom : projectCurrent();
}

// SHIFT+A held on a project's card (to cut it): from its left, as far as
// the hold, the card as an empty slot's (its outline and letter), drawn over
// it clipped to those columns.
static bool emptyLook = false;
static void drawItem(int k);
static void drawCutWipe(int k) {
  int slot;
  float cut = menuCutProgress(slot);
  if (slot != k || cut <= 0) return;
  Shape s = itemShape(k);
  int cols = (int)(cut * s.w);
  if (cols <= 0) return;
  auto &t = board.tft();
  t.setClipRect(s.x * PX, s.y * PX, cols * PX, s.h * PX);   // (the card's own area: its frame round it as it is)
  emptyLook = true;
  drawItem(k);
  emptyLook = false;
  t.setClipRect();
}

// The hold through, before the cut itself (which takes a while): the card as
// an empty slot's, its outline and letter white for 100 ms (the storage kept
// going meanwhile), then grey.
static bool cutFlashing = false;
FLASHMEM void menuCutFlash(int k) {
  emptyLook = cutFlashing = true;
  drawItem(k);
  for (uint32_t at = millis(); millis() - at < 100;) serviceStorage();
  cutFlashing = false;
  drawItem(k);
  emptyLook = false;
}

static void drawItem(int k) {
  if (!listInView(k)) return;
  auto &t = board.tft();
  Shape s = itemShape(k);
  bool sel = k == menuSel && !onStatus;
  uint16_t frame = sel && !setOpen ? frameColor() : MENU_BG;   // (the settings open: theirs instead)
  if (k < CARDS) {                                // a project slot: its frame a block out
    fillRing(s, 2, 1, frame, true);
    fillRing(s, 1, 0, MENU_BG);
    // The frame's diagonal a row thicker, outwards: the block past its end on
    // each row of the diagonal (part of the frame: cleared with it, in the ground).
    for (int r = s.y + s.h - s.cut; r < s.y + s.h + 2; r++) {
      int c0, c1;
      if (rowSpan(s, 2, r, c0, c1, true)) t.fillRect((c1 + 1) * PX, r * PX, PX, PX, frame);
    }
    uint16_t color = PROJECT_COLORS[k];
    bool filled = projectFilled(k) && !emptyLook, loaded = k == shownLoaded() && !emptyLook,
         playing = k == playingCard() && transport.running && !emptyLook;   // (emptyLook: as an empty slot's, the cut's wipe)
    // Its letter (the project's name): black on the loaded one, else in the slot's colour (an empty one's too).
    uint16_t letter = loaded ? BG : color;
    const bool cutFlash = emptyLook && cutFlashing;   // (the hold through: its outline and letter white, menuCutFlash())
    if (cutFlash) letter = INK;
    if (loaded) {                                 // the loaded one: filled
      fillRing(s, 0, -s.w, color);
    } else if (filled) {                          // a project: a thick outline, two blocks
      fillRing(s, 0, -2, color);
      fillRing(s, -2, -s.w, MENU_BG);
      // (the diagonal a block thicker where it meets the right edge: two blocks inside its corner)
      t.fillRect((s.x + s.w - 3) * PX, (s.y + s.h - s.cut - 1) * PX, PX, PX, color);
      t.fillRect((s.x + s.w - 4) * PX, (s.y + s.h - s.cut) * PX, PX, PX, color);
    } else {                                      // empty: an outline (the shape's edge)
      fillRing(s, 0, -1, cutFlash ? INK : DIM);
      fillRing(s, -1, -s.w, MENU_BG);
    }
    const Glyph *g = glyphFor((char)('A' + k));
    if (g) {
      int w = glyphWidth(*g), x0 = s.x + (s.w - w) / 2 - (playing ? 2 : 0), y0 = s.y + (s.h - 5) / 2;   // (with the play icon: both 2 blocks left)
      for (int r = 0; r < 5 && g->rows[r]; r++)
        for (int c = 0; c < w; c++)
          if (g->rows[r][c] == '#') t.fillRect((x0 + c) * PX, (y0 + r) * PX, PX, PX, letter);
      if (playing && playPulse()) {               // the clock running: a small play icon right of the letter, in the letter's colour, on the first half of each beat
        static const char *const PLAY[5] = {"#..", "##.", "###", "##.", "#.."};
        for (int r = 0; r < 5; r++)
          for (int c = 0; c < 3; c++)
            if (PLAY[r][c] == '#') t.fillRect((x0 + w + 1 + c) * PX, (y0 + r) * PX, PX, PX, letter);
      }
    }
    if (!emptyLook) drawCutWipe(k);                 // (SHIFT+A held on it: as far as the hold, an empty slot's look)
  } else {                                        // a list item: its frame round its label, rounded
    fillRing(s, LIST_SPACE + 1, LIST_SPACE, frame, true);
    fillRing(s, LIST_SPACE, 0, MENU_BG);
    bool off = k - CARDS == MENU_EXPORT && !projectHasClips();   // export: nothing to export (no clips, or the dummy project)
    const char *label = k == CARDS && exportText[0] ? exportText : LABELS[k - CARDS];
    // (Export, when it can: in the loaded project's colour, selected or not.)
    uint16_t ink = off ? DIM : k == CARDS + MENU_EXPORT && projectCurrent() >= 0 ? PROJECT_COLORS[projectCurrent()] : sel ? INK : LIGHT;
    if (k == CARDS && exportText[0] && exportNameAt > 0 && exportFor >= 0) {   // "SAVED AS " grey, the name in the project's colour
      char head[24];
      snprintf(head, sizeof head, "%.*s", exportNameAt, exportText);
      int w1 = textWidth(head) + PX;
      drawText(s.x * PX, s.y * PX, head, DIM, MENU_BG, w1);   // (the gap before the name cleared too)
      drawText(s.x * PX + w1, s.y * PX, exportText + exportNameAt, PROJECT_COLORS[exportFor], MENU_BG, s.w * PX - w1);
    } else {
      drawText(s.x * PX, s.y * PX, label, ink, MENU_BG, s.w * PX);   // (the whole box: a longer label scrolled away is covered)
    }
  }
}

// The action button's colour: a card's, its slot's; the system-level list
// items' (storage mode), light grey; the project's own (export, the status
// row), the accent colour.
uint16_t menuActionColor() {
  dropStaleExport();
  if (!shown || onStatus) return ACCENT;
  if (menuSel < CARDS) return PROJECT_COLORS[menuSel];
  if (menuSel - CARDS >= 1) return LIGHT;          // (storage mode, settings, help: system-level)
  return projectHasClips() && !exportText[0] ? ACCENT : DIM;   // export: grey with nothing to export, or while its label shows
}

// The loaded card's play icon (the clock running): shown on the first half of each beat.
static bool playPulse() {
  noInterrupts();
  bool on = transport.running && transport.sampleInBeat < transport.spb / 2;
  interrupts();
  return on;
}

// What an item shows: redrawn when it changes.
static uint32_t itemKey(int k) {
  bool sel = k == menuSel && !onStatus;
  int loaded = shownLoaded(), playing = playingCard();
  return sel | (k < CARDS && projectFilled(k)) << 1 | (k == loaded) << 2 | (k == CARDS && !projectHasClips()) << 3 |
         (uint32_t)(k >= CARDS && listInView(k) ? k - CARDS - listTop + 1 : 0) << 4 |   // (its place in view)
         (uint32_t)(sel && !setOpen ? frameColor() : 0) << 8 | (uint32_t)(sel && k < CARDS && actionHeld()) << 28 | (uint32_t)(sel && menuJustCut()) << 29 | (uint32_t)(k == CARDS && exportText[0]) << 30 |
         (uint32_t)(k == playing && transport.running) << 24 | (uint32_t)(k == playing && playPulse()) << 25 ^
         (k == CARDS + MENU_EXPORT ? (uint32_t)(projectCurrent() + 2) * 2654435761u : 0);   // (export: the loaded project's colour)
}

// ---- The settings (app.h): a popup over the menu, rising from SETTINGS (an
// arrow down to it), its items from the bottom up, each a label and, right of
// it, ON or OFF. A or B on SETTINGS opens it (once let
// go); UP/DOWN go through it, A or B turns the item on or off (on the press),
// DOWN off its bottom one or LEFT/RIGHT close it (the menu's own selection
// frame hidden meanwhile). Grey all over, from four blocks under the screen's top, each corner
// cut (its block and the two next to it); its items' frames as the list's, two
// blocks of space round them; OFF dark grey.
static const char *const SET_LABELS[SETTING_COUNT] = {"MONITOR AUDIO IN", "PLAY NEWLY RECORDED", "SELECT NEXT FREE TRACK",
                                                      "EMPTY PROJECT CLICK"};
static constexpr int SET_ITEMS = SETTING_COUNT, SET_PITCH = 10;

bool menuSettingsOpen() { return shown && setOpen; }
int menuSettingsItem() { return menuSettingsOpen() ? setSel : -1; }

// Its content's place (blocks): x, the bottom item's label row, the width;
// its box (the border's outer edge). An item's label row: up from the bottom.
static constexpr int SET_TOP = 4;                // (the box's top row: four blocks under the screen's)
static int setRow(int i, int yb) { return yb - i * SET_PITCH; }
static void setLayout(int &x, int &yb, int &w) {
  int lw = 0;
  for (const char *l : SET_LABELS) lw = max(lw, textWidth(l) / PX);
  w = lw + 4 + textWidth("OFF") / PX;
  x = (320 / PX - w) / 2;
  yb = itemShape(CARDS + MENU_SETTINGS).y - 17;   // (its border 8 rows above SETTINGS' label: the arrow between)
}
static void setBox(int &x0, int &y0, int &x1, int &y1) {
  int x, yb, w;
  setLayout(x, yb, w);
  x0 = x - 5;
  x1 = x + w + 4;
  y0 = SET_TOP;                                    // (up to four blocks short of the screen's top)
  y1 = yb + 9;
}
// An item's place overlapping the popup's (its frame's too): not drawn while
// it's open; once it closes, drawn again (the popup's place cleared first).
static bool redrawUnder = false;
static bool underBox(int k) {
  int x0, y0, x1, y1;
  setBox(x0, y0, x1, y1);
  y1 += 3;                                         // (the arrow)
  Shape s = itemShape(k);
  int d = k < CARDS ? 2 : LIST_SPACE + 1;
  return s.x - d <= x1 && s.x + s.w - 1 + d >= x0 && s.y - d <= y1 && s.y + s.h - 1 + d >= y0;
}
static bool underSettings(int k) { return setOpen && underBox(k); }

FLASHMEM static void drawSettings(bool whole) {
  auto &t = board.tft();
  int x, yb, w, x0, y0, x1, y1;
  setLayout(x, yb, w);
  setBox(x0, y0, x1, y1);
  static uint32_t shownKey = 0;
  if (whole) {
    t.fillRect(x0 * PX, y0 * PX, (x1 - x0 + 1) * PX, (y1 - y0 + 1) * PX, DIM);   // (grey; each corner cut: its block and the two next to it)
    for (int c : {x0, x1})
      for (int r : {y0, y1}) {
        int dc = c == x0 ? 1 : -1, dr = r == y0 ? 1 : -1;
        t.fillRect(c * PX, r * PX, PX, PX, MENU_BG);
        t.fillRect((c + dc) * PX, r * PX, PX, PX, MENU_BG);
        t.fillRect(c * PX, (r + dr) * PX, PX, PX, MENU_BG);
      }
    int ax = itemShape(CARDS + MENU_SETTINGS).x + 1;   // the arrow: down to SETTINGS, over its first S (3 blocks wide: its middle)
    for (int r = 0; r < 3; r++) t.fillRect((ax - 2 + r) * PX, (y1 + 1 + r) * PX, (5 - 2 * r) * PX, PX, DIM);
    shownKey = ~0u;
  }
  // The selected item's frame: as the list's; while A or B is held, dark grey
  // (on the grey), light grey for its first 100 ms (the press: it switched).
  constexpr uint32_t PRESS_FLASH_MS = 100;
  uint16_t frame = setPressAt && millis() - setPressAt < PRESS_FLASH_MS ? LIGHT : actionHeld() ? DARK : frameColor();
  uint32_t key = setSel | (uint32_t)frame << 8;
  for (int i = 0; i < SETTING_COUNT; i++) key |= (uint32_t)setting(i) << (3 + i);
  if (key == shownKey) return;
  shownKey = key;
  for (int pass = 0; pass < 2; pass++)             // (the selected one last: its frame on top of the rows it shares)
    for (int i = 0; i < SET_ITEMS; i++) {
      bool sel = i == setSel;
      if (sel != (pass == 1)) continue;
      bool on = setting(i);
      Shape s = {x, setRow(i, yb), w, FONT_H / PX, 0};   // (its frame round the whole line, 2 blocks out)
      fillRing(s, 3, 2, sel ? frame : DIM, true);
      fillRing(s, 2, 0, DIM);
      t.fillRect(s.x * PX, s.y * PX, s.w * PX, FONT_H, DIM);   // (the line cleared: OFF to ON leaves nothing behind)
      drawText(s.x * PX, s.y * PX, SET_LABELS[i], sel ? INK : LIGHT, DIM);
      const char *v = on ? "ON" : "OFF";
      drawText((x + w) * PX - textWidth(v), s.y * PX, v, on ? (sel ? INK : LIGHT) : DARK, DIM);
    }
}

// Opened: drawn over the menu as it is; closed: its place cleared, the items
// under it drawn again (nothing else redrawn: no flicker).
FLASHMEM void menuSettings(bool open) {
  if (!shown || setOpen == open) return;
  if (!open) {
    int x0, y0, x1, y1;
    setBox(x0, y0, x1, y1);
    board.tft().fillRect(x0 * PX, y0 * PX, (x1 - x0 + 1) * PX, (y1 + 3 - y0 + 1) * PX, MENU_BG);   // (the arrow's rows too)
    redrawUnder = true;
  }
  setOpen = open;
  setSel = 0;
  setDrawn = false;
  drawMenu();
}

FLASHMEM bool menuSettingsKeys(uint8_t pressed) {
  if ((pressed & m8::BTN_LEFT) || (pressed & m8::BTN_RIGHT) || ((pressed & m8::BTN_DOWN) && setSel == 0)) {
    menuSettings(false);
    return true;
  }
  int was = setSel;
  if ((pressed & m8::BTN_UP) && setSel < SET_ITEMS - 1) setSel++;
  if ((pressed & m8::BTN_DOWN) && setSel > 0) setSel--;
  movedHere = setSel != was;
  drawSettings(false);
  return movedHere;
}

FLASHMEM void menuSettingsAct() {
  setPressAt = millis();                           // (its frame light grey a moment: drawSettings())
  setSetting(setSel, !setting(setSel));
  drawSettings(false);
}

// ---- The help pages: over the whole screen, from the menu's HELP (once let
// go), on the page last shown; LEFT/RIGHT the page before / after (not
// wrapping: LEFT on the first, RIGHT on the last back to the menu; held, they repeat half as fast as elsewhere),
// A or B back to the menu, UP or DOWN too (once let go, its edge line meanwhile). Each page drawn whole as it's turned to (ui_help.cpp).
bool menuHelpOpen() { return shown && helpOpen; }

FLASHMEM void menuHelp(bool open) {
  if (!shown || helpOpen == open) return;
  helpOpen = open;
  if (open) {
    setHelpPage(constrain(helpPage(), 0, helpPages() - 1));   // (seen)
    drawHelpPage(helpPage());
    return;
  }
  uiGeneration++;                                  // (the menu whole again)
  drawMenu();
}

FLASHMEM bool menuHelpKeys(uint8_t pressed, bool repeat) {
  const int n = helpPages(), p = helpPage();
  static bool skip = false;                        // (held: every other repeat, half as fast)
  if (repeat && (skip = !skip)) return false;
  if (!repeat) skip = false;
  int to = p;
  if (pressed & m8::BTN_RIGHT && p < n - 1) to = p + 1;   // (no wrapping: off the ends, input.cpp leaves for the menu)
  if (pressed & m8::BTN_LEFT && p > 0) to = p - 1;
  if (to == p) return false;
  setHelpPage(to);
  helpTurned();
  drawHelpPage(to);
  return true;
}

// The battery, top right (two blocks in): a tiny grey one, 11 x 7 blocks, its
// outline, a block of space, its fill in sevenths (90 % and up: full); its tip
// a block right, 3 blocks tall. Charging: its inside filled whole (up to the
// outline), a lightning bolt cut out of it. Always shown: with no reading
// (variant A, no answer), full.
// (ink on ground: the menu's grey on its ground; storage mode's dark grey on its grey.)
FLASHMEM void drawBatteryIcon(bool all, uint16_t ink, uint16_t ground) {
  static int shownFill = -1;
  static bool shownCharging = false;
  constexpr int STEPS = 7;
  int pct = board.batteryPercent(), fill = pct < 0 || pct >= 90 ? STEPS : (pct * STEPS + 50) / 100;
  bool charging = pct >= 0 && board.batteryCharging();
  if (!all && fill == shownFill && charging == shownCharging) return;
  shownFill = fill;
  shownCharging = charging;
  static const char *const BOLT[5] = {"....#....", "...##....", "..#####..", "....##...", "....#...."};   // (cut out: rows 1-5, columns 1-9)
  auto &t = board.tft();
  constexpr int W = 11, H = 7, X = 320 / PX - 2 - (W + 1), Y = 2;   // (its body, then its tip)
  for (int r = 0; r < H; r++)
    for (int c = 0; c <= W; c++) {
      bool edge = r == 0 || r == H - 1 || c == 0 || c == W - 1, tip = c == W && r >= 2 && r <= H - 3;
      bool inside = r >= 2 && r <= H - 3 && c >= 2 && c - 2 < fill;
      bool on = c < W ? edge || inside : tip;
      if (charging && !edge && c < W) on = BOLT[r - 1][c - 1] != '#';
      t.fillRect((X + c) * PX, (Y + r) * PX, PX, PX, on ? ink : ground);
    }
}
static void drawBattery(bool all) { drawBatteryIcon(all, DIM, MENU_BG); }

FLASHMEM void drawMenu() {
  static uint32_t shownKey[ITEMS];
  static uint32_t drawn = 0;                     // uiGeneration when last drawn whole
  if (!shown || helpOpen) return;                // (the help pages: drawn when they change, drawHelpPage())
  dropStaleExport();                             // (the project changed, or another one loaded: the export's message goes)
  bool all = drawn != uiGeneration;
  auto &t = board.tft();
  if (all) {
    // The ground: everywhere but the items' places (each drawn whole over
    // its own just after: nothing on screen wiped first, no flicker; the
    // track rows' place and the status row's: no action button in the menu).
    drawn = uiGeneration;
    for (int r = 0; r < 240 / PX; r++) {
      int spans[ITEMS][2], n = 0;
      for (int k = 0; k < ITEMS; k++) {
        if (!listInView(k) || underSettings(k)) continue;   // (under the settings: the ground, the popup over it)
        int c0, c1;
        if (rowSpan(itemShape(k), k < CARDS ? 2 : LIST_SPACE + 1, r, c0, c1, true)) {
          int i = n++;
          while (i > 0 && spans[i - 1][0] > c0) { spans[i][0] = spans[i - 1][0]; spans[i][1] = spans[i - 1][1]; i--; }
          spans[i][0] = c0;
          spans[i][1] = c1;
        }
      }
      int at = 0;                                  // (the first column not filled yet)
      for (int i = 0; i <= n; i++) {
        int end = i < n ? spans[i][0] : 320 / PX;
        if (end > at) t.fillRect(at * PX, r * PX, (end - at) * PX, PX, MENU_BG);
        if (i < n) at = max(at, spans[i][1] + 1);
      }
    }
  }
  // Scrolled: the items in view redraw over each other's places (their keys
  // hold the place), each label drawn the box's whole width; no clearing
  // first, which flickered.
  // Unselected first: frames that touch a neighbour's space don't get wiped.
  // (Anything redrawn: the selected one again after it, its frame on top.)
  drawBattery(all);
  static uint16_t logo;                          // the corner logo's colour shown
  if (all || logo != actionRestColor()) {        // the logo, bottom left: the action button's colour (not its pressed look)
    logo = actionRestColor();
    drawSmallLogo(LOGO_X, LOGO_Y, logo);
  }
  bool redrawn = false;
  int cutSlot;
  const float cut = menuCutProgress(cutSlot);
  static int shownCut = -1;                        // (the card being wiped)
  if (cutSlot != shownCut) {                       // (another, or none: the one before whole again)
    if (shownCut >= 0) shownKey[shownCut] = ~0u;
    shownCut = cutSlot;
  }
  static int shownCols = -1;                       // (wiping: redrawn as it gets further, over the card as drawn: no flicker)
  int cols = cutSlot >= 0 ? (int)(cut * CARD_W) : -1;
  if (cutSlot >= 0 && cols != shownCols) drawCutWipe(cutSlot);
  shownCols = cols;
  if (redrawUnder)                                 // (the settings just closed: what was under them)
    for (int k = 0; k < ITEMS; k++)
      if (underBox(k)) shownKey[k] = ~0u;
  redrawUnder = false;
  for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < ITEMS; k++) {
      uint32_t key = itemKey(k);
      if ((key & 1) != pass || (!all && key == shownKey[k] && !(pass && redrawn))) continue;
      if (underSettings(k)) {                      // (under the settings: once they close)
        shownKey[k] = ~0u;
        continue;
      }
      shownKey[k] = key;
      drawItem(k);
      redrawn = true;
    }
  if (setOpen) {
    drawSettings(all || !setDrawn);
    setDrawn = true;
  }
}

// Back from the status row (it can't be reached any more: kept for input.cpp).
void menuLeaveStatus() {
  if (!onStatus) return;
  onStatus = false;
  statusCell = selectedCellIn(STATUS_ROW);
  menuSel = ITEMS - 1;                           // the list's last item
  scrollList(LIST - 1, 0);
  movedHere = true;
  selected = mainRow;
  selectedCell = mainCell;
  drawMenu();
  drawTrack(STATUS_ROW);
}

// The arrows: LEFT/RIGHT through every item in order; UP/DOWN between the
// card rows and the list's items (no wrapping).
bool menuKeys(uint8_t pressed) {
  const uint8_t arrows = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  int k = menuSel;
  // LEFT/RIGHT: along a card row (not on to another; off its ends, or on a
  // list item, they leave the menu: menuLeavesOn()).
  if (k < CARDS) {
    if (pressed & m8::BTN_LEFT && k % COLS > 0) k--;
    if (pressed & m8::BTN_RIGHT && k % COLS < COLS - 1) k++;
    int row = k / COLS;
    if (pressed & m8::BTN_UP && row > 0) k -= COLS;
    if (pressed & m8::BTN_DOWN) k = row < CARD_ROWS - 1 ? k + COLS : CARDS;
    if (k < CARDS) cardCol = k % COLS;
  } else if (!(pressed & (m8::BTN_LEFT | m8::BTN_RIGHT))) {
    if (pressed & m8::BTN_UP) k = k > CARDS ? k - 1 : CARDS - COLS + cardCol;   // (to the bottom row: I-L)
    if (pressed & m8::BTN_DOWN && k < ITEMS - 1) k++;
  }
  if (pressed & arrows) movedHere = k != menuSel;
  if (k >= CARDS && k != menuSel) scrollList(k - CARDS, k > menuSel ? 1 : -1);
  menuSel = k;
  drawMenu();
  return movedHere;
}

// ---- Switching screens off an edge: into the menu, onto its far edge (UP
// off the looper's top: the menu's bottom item, the list's last; DOWN off its
// bottom: the menu's top card row, at its own column, the card column last
// selected; LEFT off the looper's left edge: the menu's right side, RIGHT: its
// left side, in its own row, where its selection was: a card row, a list item,
// or its status row).
FLASHMEM static void landInMenu(uint8_t key) {
  const bool vertical = key == m8::BTN_UP || key == m8::BTN_DOWN;
  if (onStatus && vertical) {                      // (off its status row, onto its items)
    onStatus = false;
    statusCell = selectedCellIn(STATUS_ROW);
    selected = mainRow;
    selectedCell = mainCell;
  }
  if (onStatus) {                                  // its status row: its far cell
    selectedCell = key == m8::BTN_LEFT ? TEMPO_CELL : SCENE_CELL;
    movedHere = true;
    return;
  }
  int k = menuSel;
  if (key == m8::BTN_UP) k = ITEMS - 1;
  else if (key == m8::BTN_DOWN) k = cardCol;
  else if (k < CARDS) k = k / COLS * COLS + (key == m8::BTN_LEFT ? COLS - 1 : 0);   // (a list item spans the side: as it is)
  menuSel = k;
  if (k < CARDS) cardCol = k % COLS;
  else scrollList(k - CARDS, 0);
  movedHere = true;
}

// A pointer (the desktop app's mouse): the menu item at (x, y) on the screen
// (-1: none; the settings or help open: none either), and selecting one (as
// the arrows would, off the status row too).
FLASHMEM int menuItemAt(int x, int y) {
  if (!shown || setOpen || helpOpen) return -1;
  for (int k = 0; k < ITEMS; k++) {
    if (!listInView(k)) continue;
    Shape s = itemShape(k);
    if (x >= s.x * PX && x < (s.x + s.w) * PX && y >= s.y * PX && y < (s.y + s.h) * PX) return k;
  }
  return -1;
}
FLASHMEM void menuPoint(int k) {
  if (!shown || setOpen || helpOpen || k < 0 || k >= ITEMS || (k == menuSel && !onStatus)) return;
  if (onStatus) {
    onStatus = false;
    statusCell = selectedCellIn(STATUS_ROW);
    selected = mainRow;
    selectedCell = mainCell;
  }
  menuSel = k;
  if (k < CARDS) cardCol = k % COLS;
  movedHere = true;
  drawMenu();
}

void menuLand(uint8_t key) {
  if (!shown || !key) return;
  landInMenu(key);
  drawMenu();
}

bool menuExportShown() {
  dropStaleExport();
  return shown && exportText[0];
}

void menuExportStatus(const char *text, int nameAt) {
  snprintf(exportText, sizeof exportText, "%s", text ? text : "");
  exportNameAt = nameAt;
  exportFor = projectCurrent();
  exportEdits = projectEdits();
  if (shown) drawItem(CARDS);                   // (now: the export holds up the loop)
}

int menuFirstEmptySlot() {
  for (int k = 0; k < CARDS; k++)
    if (!projectFilled(k)) return k;
  return -1;
}

bool menuLeavesOn(uint8_t key) {
  // (No project loaded: leaving makes a new one in the first empty slot; with none, nowhere to go.)
  if (!shown || onStatus || (projectCurrent() < 0 && menuFirstEmptySlot() < 0)) return false;
  if (menuLoadPending() || setOpen || helpOpen) return false;   // (the settings open: DOWN, LEFT and RIGHT close them; help: the arrows turn its pages)           // (a project on its way: not until it has come in)
  // RIGHT off the screen's right edge (the looper's that way): off a card
  // row's right end, or a list item's side; LEFT off its left edge; UP off its
  // top: the top card row (A-D); DOWN off its bottom: the list's last item.
  if (key == m8::BTN_UP) return menuSel < COLS;
  if (key == m8::BTN_DOWN) return menuSel == ITEMS - 1;
  if (key == m8::BTN_LEFT) return menuSel >= CARDS || menuSel % COLS == 0;   // (LEFT off its left edge too)
  return key == m8::BTN_RIGHT && (menuSel >= CARDS || menuSel % COLS == COLS - 1);
}

void menuSelectSlot(int k) {
  if (k < 0 || k >= CARDS) return;
  menuSel = k;
  cardCol = k % COLS;
}

// Left by A or B on a card (to the looper): back in, on the loaded project's
// card, wherever it's opened from; off an edge, as ever.
static bool backToCard = false;
void menuNoteLeave(bool byAction) { backToCard = byAction; }

FLASHMEM void openMenu(uint8_t key) {
  if (shown) return;
  // A project with no clips at all (none recorded since it was made): gone,
  // the dummy project instead (one has to be chosen before leaving the menu).
  if (projectCurrent() >= 0 && recording.mode == Recording::Idle && recordQueued() < 0 && !projectHasClips())
    projectDelete(projectCurrent());
  if (DELETE_UNUSED_SAMPLES) deleteUnusedSamples();   // recordings no project uses: gone
  shown = true;                                  // (where it was last left: an item, or its status row)
  notesLayout = false;                           // (the menu: main's layout, whichever screen it came from)
  mainRow = selected;
  mainCell = selectedCell;
  if (onStatus) {                                // (where it was last time)
    selected = STATUS_ROW;
    selectedCell = statusCell;
  }
  setOpen = false;
  helpOpen = false;
  if (backToCard && projectCurrent() >= 0) {      // left with A or B on a card: back on the loaded project's card
    menuSel = projectCurrent();
    cardCol = menuSel % COLS;
    movedHere = false;
  } else if (key) {
    landInMenu(key);                             // (before it's drawn: once, where it lands)
  }
  backToCard = false;
  uiGeneration++;                                // the menu in full; the status row redraws too
  drawMenu();
  drawTrack(STATUS_ROW);
}

void closeMenu(bool draw) {
  if (!shown) return;
  if (exportNameAt <= 0) exportText[0] = 0;      // ("EXPORT FAILED" goes with the menu: exporting can be tried again)
  if (onStatus) statusCell = selectedCellIn(STATUS_ROW);
  selected = mainRow;
  selectedCell = mainCell;
  shown = false;
  setOpen = false;
  helpOpen = false;
  notesLayout = notesScreen;                     // (back to the screen it came from: notes, or main)
  // (Not the whole screen wiped first, which flickered: the looper's rows,
  // drawn whole below, cover all of it but the strip under the last track's
  // cells, above the status row.)
  board.tft().fillRect(0, rowTop(LAST_TRACK_ROW) + CELL_H, 320, STATUS_TOP - rowTop(LAST_TRACK_ROW) - CELL_H, BG);
  uiGeneration++;                                // everything redraws in full
  if (draw)
    for (int i = 0; i < ROWS; i++) drawTrack(i);
}

void restoreMenu(int state) {
  if (state < 0) return;
  int sel = state % 1000;
  menuSel = constrain(sel, 0, ITEMS - 1);         // (saved on the status row, before it was hidden: the list's end)
  if (menuSel < CARDS) cardCol = menuSel % COLS;
  if (menuSel >= CARDS) scrollList(menuSel - CARDS, 0);
  if (state >= 1000) openMenu();
}
