// ui_menu.cpp — the menu (see ui.h): in place of the track rows, a second
// view of the looper (the status row stays), the screen's whole width.
// Opened with B+A (or A on the scene cell), closed the same way from the status row, or by loading a project. Twelve
// cards (three rows of four), the project slots, and under them a list,
// "EXPORT TO ALS" and "USB DATA MODE" (nothing yet). A and B both run
// the action button's action. On a slot, A loads its project (an empty one: a new project there) and
// closes the menu; with SHIFT, cuts the project, or pastes one into an empty
// slot (only while stopped).
//
// Items: the cards 0-11 (rows of four, top first) and the list's 12 and 13
// (export, USB data mode), the list above the cards (filled from the bottom
// up), the cards just above the status row, which the
// menu's selection reaches too (its cells then work as in the looper's view;
// the looper's own selection is kept meanwhile and comes back when the menu
// closes). UP/DOWN go between the list's rows, the card rows and the status
// row; LEFT/RIGHT through everything in order: the list, the cards row by
// row, the status row's cells (and back). The menu remembers where its
// selection was for the next time it opens.
//
// A card: a block shape, its bottom right corner cut diagonally (about a
// fifth of its height), in its slot's colour (colors.h): filled when it
// holds a project (the loaded project's outlined white), its letter A-L (the
// project's name) black; empty, a grey outline with the letter in the slot's
// colour. The selected item's frame looks and behaves as the looper's
// cells' (frameColor()), corners cut, a block out from a card (the diagonal
// too), three from the list's box (the longest label's size, centred;
// labels left-aligned, light grey, white when selected).
#include "ui.h"
#include "ui_font.h"

uint32_t uiGeneration = 1;

static constexpr int CARDS = PROJECTS, LIST = 2, ITEMS = CARDS + LIST, COLS = 4;
static const char *const LABELS[LIST] = {"EXPORT TO ALS", "USB DATA MODE"};

static bool shown = false;
static int menuSel = 0;                          // the selected item
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
  if (!shown) return -1;
  return onStatus ? 100 + selectedCellIn(STATUS_ROW) : menuSel;
}

void mainSelection(int &row, int &cell) {
  row = shown ? mainRow : selected;
  cell = shown ? mainCell : selectedCell;
}

// ---- Shapes, in blocks: an item's content is w x h blocks from (x, y), its
// bottom right corner cut `cut` blocks (0: a rectangle). shape(d) is the
// content grown by d blocks all round (the diagonal moved out a block per
// step too); a frame is a ring between two of them.
struct Shape { int x, y, w, h, cut; };

// Row r (screen block row) of shape s grown by d: its first and last column
// (false: the row isn't in it). round: the corner blocks left out.
static bool rowSpan(const Shape &s, int d, int r, int &c0, int &c1, bool round = false) {
  int y = r - s.y;
  if (y < -d || y > s.h - 1 + d) return false;
  c0 = -d;
  c1 = s.w - 1 + d;
  if (round && (y == -d || y == s.h - 1 + d)) {   // the top and bottom rows: their corners cut
    c0++;
    if (y == -d || !s.cut) c1--;                  // (the bottom right: the diagonal's already)
  }
  if (s.cut) c1 = min(c1, s.w + s.h - s.cut - 2 + d - y);   // the diagonal: x + y at most this
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
// The cards at the bottom, just above the status row: 26 x 14, the cut 3,
// 6 apart across, 8 down, centred. Above them, 8 rows clear of their frames,
// the list: a box the size of its longest label, centred (labels
// left-aligned), an item every 16 rows filled from the bottom up (export,
// then USB data mode above it), its frame 3 blocks out.
static constexpr int CARD_W = 26, CARD_H = 14, CARD_CUT = 3, CARD_GAP = 6, CARD_VGAP = 8, CARD_ROWS = CARDS / COLS;
static constexpr int CARDS_X = (320 / PX - COLS * CARD_W - (COLS - 1) * CARD_GAP) / 2;
static constexpr int CARDS_Y = 102 - (CARD_ROWS * CARD_H + (CARD_ROWS - 1) * CARD_VGAP) + 1;   // (the last row ends at row 102)
static constexpr int LIST_SPACE = 3, LIST_PITCH = 16, LIST_COLS = 1, LIST_GAP = 8;
static constexpr int LIST_Y = CARDS_Y - 2 - LIST_GAP - LIST_SPACE - 1 - FONT_H / PX;   // the bottom row's text (its frame LIST_GAP above the cards')

static int listRow(int item) { return item / LIST_COLS; }   // 0: the bottom row (filled first)
static int listCol(int item) { return item % LIST_COLS; }

static Shape itemShape(int k) {
  if (k < CARDS)
    return {CARDS_X + (k % COLS) * (CARD_W + CARD_GAP), CARDS_Y + (k / COLS) * (CARD_H + CARD_VGAP), CARD_W, CARD_H, CARD_CUT};
  // A list item: its column's box (the longest label in it).
  int item = k - CARDS, col = listCol(item), w = 0;
  for (int i = col; i < LIST; i += LIST_COLS) w = max(w, textWidth(LABELS[i]) / PX);
  return {(320 / PX - w) / 2, LIST_Y - listRow(item) * LIST_PITCH, w, FONT_H / PX, 0};   // (one column, centred)
}

// The selected item's frame, as the looper's cells': light grey; white while
// only arrows are held and the last press moved the selection; grey while A
// or B is held.
static bool movedHere = false;                   // the last arrow press moved the menu's selection
static uint16_t frameColor() {
  uint8_t k = board.keys();
  const uint8_t arrows = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  return k & (m8::BTN_EDIT | m8::BTN_OPT) ? DIM : k && !(k & ~arrows) && movedHere ? INK : LIGHT;
}

static void drawItem(int k) {
  auto &t = board.tft();
  Shape s = itemShape(k);
  bool sel = k == menuSel && !onStatus;
  uint16_t frame = sel ? frameColor() : BG;
  if (k < CARDS) {                                // a project slot: its frame a block out
    fillRing(s, 2, 1, frame, true);
    fillRing(s, 1, 0, BG);
    uint16_t color = PROJECT_COLORS[k];
    bool filled = projectFilled(k);
    // Its letter (the project's name): black on a project, grey on an empty slot.
    uint16_t letter = filled ? BG : color;
    if (filled) {
      fillRing(s, 0, -s.w, color);
      if (k == projectCurrent()) fillRing(s, 0, -1, INK);   // the loaded one: outlined white
    } else {                                      // empty: an outline (the shape's edge)
      fillRing(s, 0, -1, DIM);
      fillRing(s, -1, -s.w, BG);
    }
    const Glyph *g = glyphFor((char)('A' + k));
    if (g) {
      int w = glyphWidth(*g), x0 = s.x + (s.w - w) / 2, y0 = s.y + (s.h - 5) / 2;
      for (int r = 0; r < 5 && g->rows[r]; r++)
        for (int c = 0; c < w; c++)
          if (g->rows[r][c] == '#') t.fillRect((x0 + c) * PX, (y0 + r) * PX, PX, PX, letter);
    }
  } else {                                        // a list item: its frame round its label, rounded
    fillRing(s, LIST_SPACE + 1, LIST_SPACE, frame, true);
    fillRing(s, LIST_SPACE, 0, BG);
    drawText(s.x * PX, s.y * PX, LABELS[k - CARDS], sel ? INK : LIGHT, BG);
  }
}

// The action button's colour: a card's, its slot's; the system-level list
// items' (USB data mode), light grey; the project's own (export, the status
// row), the accent colour.
uint16_t menuActionColor() {
  if (!shown || onStatus) return ACCENT;
  if (menuSel < CARDS) return PROJECT_COLORS[menuSel];
  return menuSel - CARDS == 1 ? LIGHT : ACCENT;
}

// What an item shows: redrawn when it changes.
static uint32_t itemKey(int k) {
  bool sel = k == menuSel && !onStatus;
  return sel | (k < CARDS && projectFilled(k)) << 1 | (k == projectCurrent()) << 2 | (uint32_t)(sel ? frameColor() : 0) << 8;
}

void drawMenu() {
  static uint32_t shownKey[ITEMS];
  static uint32_t drawn = 0;                     // uiGeneration when last drawn whole
  if (!shown) return;
  bool all = drawn != uiGeneration;
  if (all) {
    drawn = uiGeneration;
    board.tft().fillRect(0, 0, 320, STATUS_TOP, BG);   // (where the track rows were)
  }
  // Unselected first: frames that touch a neighbour's space don't get wiped.
  for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < ITEMS; k++) {
      uint32_t key = itemKey(k);
      if ((key & 1) != pass || (!all && key == shownKey[k])) continue;
      shownKey[k] = key;
      drawItem(k);
    }
}

// Onto the status row, and back.
static void toStatus() {
  onStatus = true;
  selected = STATUS_ROW;
  selectedCell = statusCell;
  drawMenu();
  drawTrack(STATUS_ROW);
}
// The order LEFT/RIGHT go through, top to bottom: the list (its top item
// first), the cards row by row; then the status row.
static int orderOf(int k) { return k >= CARDS ? LIST - 1 - (k - CARDS) : LIST + k; }
static int itemAt(int pos) { return pos < LIST ? CARDS + LIST - 1 - pos : pos - LIST; }

void menuLeaveStatus(bool left) {
  if (!onStatus) return;
  onStatus = false;
  statusCell = selectedCellIn(STATUS_ROW);
  menuSel = left ? CARDS - 1 : CARDS - COLS + cardCol;   // LEFT: the last card; UP: the cards' last row
  movedHere = true;
  selected = mainRow;
  selectedCell = mainCell;
  drawMenu();
  drawTrack(STATUS_ROW);
}

// The arrows: LEFT/RIGHT through everything in order (orderOf()), on past
// the last card onto the status row's first cell; UP/DOWN between the list's
// rows, the card rows (a list column over each half of them: back down to
// the column last used there) and the status row.
bool menuKeys(uint8_t pressed) {
  const uint8_t arrows = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  int k = menuSel;
  if (pressed & m8::BTN_LEFT && orderOf(k) > 0) k = itemAt(orderOf(k) - 1);
  if (pressed & m8::BTN_RIGHT) {
    if (orderOf(k) == ITEMS - 1) {
      movedHere = true;
      statusCell = SCENE_CELL;
      toStatus();
      return true;
    }
    k = itemAt(orderOf(k) + 1);
  }
  if (!(pressed & (m8::BTN_LEFT | m8::BTN_RIGHT))) {
    if (k < CARDS) {
      int row = k / COLS, col = k % COLS;
      if (pressed & m8::BTN_UP)                      // the top row: up to the list column over it
        k = row > 0 ? k - COLS : CARDS + min(col / (COLS / LIST_COLS), LIST - 1);
      if (pressed & m8::BTN_DOWN) {
        if (row == CARD_ROWS - 1) {                  // the last row: the status row
          movedHere = true;
          toStatus();
          return true;
        }
        k += COLS;
      }
    } else {
      int item = k - CARDS, col = listCol(item), half = COLS / LIST_COLS;
      if (pressed & m8::BTN_UP && item + LIST_COLS < LIST) k += LIST_COLS;   // a row up
      if (pressed & m8::BTN_DOWN) {
        if (listRow(item) > 0) k -= LIST_COLS;
        else k = cardCol / half == col ? cardCol : col * half;   // onto the cards' top row, under it
      }
    }
  }
  if (k < CARDS) cardCol = k % COLS;
  if (pressed & arrows) movedHere = k != menuSel;
  menuSel = k;
  drawMenu();
  return movedHere;
}

void openMenu() {
  if (shown) return;
  shown = true;
  mainRow = selected;
  mainCell = selectedCell;
  if (onStatus) {                                // (where it was last time)
    selected = STATUS_ROW;
    selectedCell = statusCell;
  }
  uiGeneration++;                                // the menu in full; the status row redraws too
  drawMenu();
  drawTrack(STATUS_ROW);
}

void closeMenu() {
  if (!shown) return;
  if (onStatus) statusCell = selectedCellIn(STATUS_ROW);
  selected = mainRow;
  selectedCell = mainCell;
  shown = false;
  board.tft().fillRect(0, 0, 320, STATUS_TOP, BG);
  uiGeneration++;                                // everything redraws in full
  for (int i = 0; i < ROWS; i++) drawTrack(i);
}

void restoreMenu(int state) {
  if (state < 0) return;
  if (state >= 100) {
    onStatus = true;
    statusCell = constrain(state - 100, 0, TEMPO_CELL);
  } else {
    menuSel = constrain(state, 0, ITEMS - 1);
    if (menuSel < CARDS) cardCol = menuSel % COLS;
  }
  openMenu();
}
