// ui_menu.cpp — the menu (see ui.h): in place of the track rows, a second
// view of the looper (the status row stays). Opened with B+A (or A on the
// scene cell), closed with A. For now: eight boxes (two rows of four) and two
// buttons, "EXPORT LIVE SET" and "USB MODE", selectable with the arrows, none
// of them doing anything yet.
//
// Items, in selection order: the boxes 0-3 (top row), 4-7, then the buttons
// 8 and 9; below them the status row, which the menu's selection reaches too
// (its cells then work as in the looper's view; the looper's own selection is
// kept meanwhile and comes back when the menu closes). The menu remembers
// where its selection was for the next time it opens. Each is a dark grey block in a one-block border, the selected
// one's grey (corner blocks cut, as the cells' frames); the buttons' labels
// light grey, white when selected.
#include "ui.h"

uint32_t uiGeneration = 1;

static bool shown = false;
static int menuSel = 0;                          // the selected item
static bool onStatus = false;                    // the selection is on the status row (selected = STATUS_ROW meanwhile)
static int statusCell = SCENE_CELL;              // where on it, while the menu's closed
static int mainRow = 0, mainCell = 0;            // the looper's selection, back when the menu closes

bool menuOpen() { return shown; }
bool menuOnStatus() { return shown && onStatus; }

// The items' rects (their borders included), on the block grid: the boxes
// 36 x 36 blocks, the buttons 72 x 14, across the cells' width (x 2-289).
static constexpr int ITEMS = 10, BOXES = 8, BOX = 36 * PX, BUTTON_H = 14 * PX;
static constexpr int MENU_X = 2, BOXES_Y = 2 * PX, BUTTONS_Y = BOXES_Y + 2 * BOX + 3 * PX;
struct Rect { int x, y, w, h; };
static Rect itemRect(int k) {
  if (k < BOXES) return {MENU_X + (k % 4) * BOX, BOXES_Y + (k / 4) * BOX, BOX, BOX};
  return {MENU_X + (k - BOXES) * 2 * BOX, BUTTONS_Y, 2 * BOX, BUTTON_H};
}
static const char *const LABELS[2] = {"EXPORT LIVE SET", "USB MODE"};

static void drawItem(int k) {
  auto &t = board.tft();
  Rect r = itemRect(k);
  bool sel = k == menuSel && !onStatus;
  uint16_t frame = sel ? DIM : BG;
  t.fillRect(r.x + PX, r.y, r.w - 2 * PX, PX, frame);                 // the border, its corners cut
  t.fillRect(r.x + PX, r.y + r.h - PX, r.w - 2 * PX, PX, frame);
  t.fillRect(r.x, r.y + PX, PX, r.h - 2 * PX, frame);
  t.fillRect(r.x + r.w - PX, r.y + PX, PX, r.h - 2 * PX, frame);
  t.fillRect(r.x, r.y, PX, PX, BG);
  t.fillRect(r.x + r.w - PX, r.y, PX, PX, BG);
  t.fillRect(r.x, r.y + r.h - PX, PX, PX, BG);
  t.fillRect(r.x + r.w - PX, r.y + r.h - PX, PX, PX, BG);
  t.fillRect(r.x + PX, r.y + PX, r.w - 2 * PX, r.h - 2 * PX, DARK);   // inside
  if (k >= BOXES) {
    const char *label = LABELS[k - BOXES];
    int w = textWidth(label);
    int x = r.x + (r.w - w) / 2 / PX * PX, y = r.y + (r.h - FONT_H) / 2 / PX * PX;
    drawText(x, y, label, sel ? INK : LIGHT, DARK);
  }
}

void drawMenu() {
  static int shownSel = -1;                      // (-1: on the status row)
  static uint32_t drawn = 0;                     // uiGeneration when last drawn whole
  if (!shown) return;
  if (drawn != uiGeneration) {
    drawn = uiGeneration;
    board.tft().fillRect(0, 0, 320, STATUS_TOP, BG);   // (where the track rows were)
    for (int k = 0; k < ITEMS; k++) drawItem(k);
    shownSel = onStatus ? -1 : menuSel;
    return;
  }
  int sel = onStatus ? -1 : menuSel;
  if (shownSel == sel) return;
  int was = shownSel;
  shownSel = sel;
  if (was >= 0) drawItem(was);
  if (sel >= 0) drawItem(sel);
}

// Onto the status row (from a button: the cell under it), and back.
static void toStatus(int cell) {
  onStatus = true;
  selected = STATUS_ROW;
  selectedCell = cell;
  drawMenu();
  drawTrack(STATUS_ROW);
}
void menuLeaveStatus() {
  if (!onStatus) return;
  onStatus = false;
  statusCell = selectedCellIn(STATUS_ROW);
  menuSel = statusCell == TEMPO_CELL ? ITEMS - 1 : BOXES;   // the button above it
  selected = mainRow;
  selectedCell = mainCell;
  drawMenu();
  drawTrack(STATUS_ROW);
}

// The arrows: along a row (no wrapping), and between the rows (the buttons
// under the boxes: each below two of them).
void menuKeys(uint8_t pressed) {
  int k = menuSel;
  bool box = k < BOXES;
  int row = box ? k / 4 : 2, col = box ? k % 4 : (k - BOXES) * 2;
  if (pressed & m8::BTN_LEFT) {
    if (box && col > 0) k--;
    else if (!box && k > BOXES) k--;
  }
  if (pressed & m8::BTN_RIGHT) {
    if (box && col < 3) k++;
    else if (!box && k < ITEMS - 1) k++;
  }
  if (pressed & m8::BTN_UP && row > 0) k = row == 2 ? 4 + col : col;
  if (pressed & m8::BTN_DOWN && row < 2) k = row == 0 ? 4 + col : BOXES + col / 2;
  menuSel = k;
  if (pressed & m8::BTN_DOWN && row == 2) {      // below the buttons: the status row
    toStatus(k == BOXES ? SCENE_CELL : TEMPO_CELL);
    return;
  }
  drawMenu();
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
