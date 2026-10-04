// ui_menu.cpp — the menu (see ui.h): in place of the track rows, a second
// view of the looper (the status row stays). Opened with B+A (or A on the
// scene cell), closed with A. Eight boxes (two rows of four), the project
// slots, and two buttons, "EXPORT LIVE SET" and "USB MODE" (nothing yet),
// selectable with the arrows. On a slot, A loads its project (an empty one:
// a new project there) and closes the menu; with SHIFT, cuts the project, or
// pastes one into an empty slot (only while stopped). Elsewhere A closes it.
//
// Items, in selection order: the boxes 0-3 (top row), 4-7, then the buttons
// 8 and 9; below them the status row, which the menu's selection reaches too
// (its cells then work as in the looper's view; the looper's own selection is
// kept meanwhile and comes back when the menu closes). The menu remembers
// where its selection was for the next time it opens. Each sits in a one-block
// border, the selected one's grey (corner blocks cut, as the cells' frames).
// A slot is its colour (colors.h), filled when it holds a project, an outline
// when empty; the loaded project's shows the looper's rows (as the HOME icon)
// in black. The buttons: dark grey, their labels light grey, white when
// selected.
#include "ui.h"

uint32_t uiGeneration = 1;

static bool shown = false;
static int menuSel = 0;                          // the selected item
static bool onStatus = false;                    // the selection is on the status row (selected = STATUS_ROW meanwhile)
static int statusCell = SCENE_CELL;              // where on it, while the menu's closed
static int mainRow = 0, mainCell = 0;            // the looper's selection, back when the menu closes

bool menuOpen() { return shown; }
bool menuOnStatus() { return shown && onStatus; }

int menuSlot() { return shown && !onStatus && menuSel < PROJECTS ? menuSel : -1; }

void mainSelection(int &row, int &cell) {
  row = shown ? mainRow : selected;
  cell = shown ? mainCell : selectedCell;
}

// The items' rects (their borders included), on the block grid: the boxes
// 36 x 18 blocks, the buttons 72 x 14, across the cells' width (x 2-289).
static constexpr int ITEMS = 10, BOXES = 8, BOX = 36 * PX, BOX_H = 18 * PX, BUTTON_H = 14 * PX;
static constexpr int MENU_X = 2, BOXES_Y = 2 * PX, BUTTONS_Y = BOXES_Y + 2 * BOX_H + 3 * PX;
struct Rect { int x, y, w, h; };
static Rect itemRect(int k) {
  if (k < BOXES) return {MENU_X + (k % 4) * BOX, BOXES_Y + (k / 4) * BOX_H, BOX, BOX_H};
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
  int x = r.x + PX, y = r.y + PX, w = r.w - 2 * PX, h = r.h - 2 * PX;   // inside
  if (k < BOXES) {                               // a project slot
    uint16_t color = PROJECT_COLORS[k];
    if (projectFilled(k)) {
      t.fillRect(x, y, w, h, color);
    } else {                                     // empty: an outline, its corners cut
      t.fillRect(x, y, w, h, BG);
      t.fillRect(x + PX, y, w - 2 * PX, PX, color);
      t.fillRect(x + PX, y + h - PX, w - 2 * PX, PX, color);
      t.fillRect(x, y + PX, PX, h - 2 * PX, color);
      t.fillRect(x + w - PX, y + PX, PX, h - 2 * PX, color);
    }
    if (k == projectCurrent()) {                 // loaded: the looper's rows
      static const char *const ROWS_ICON[5] = {"###.#.#", ".......", "###.#.#", ".......", "###.#.#"};
      int x0 = x + (w / PX - 7) / 2 * PX, y0 = y + (h / PX - 5) / 2 * PX;
      for (int i = 0; i < 5; i++)
        for (int c = 0; c < 7; c++)
          if (ROWS_ICON[i][c] == '#') t.fillRect(x0 + c * PX, y0 + i * PX, PX, PX, BG);
    }
  } else {
    t.fillRect(x, y, w, h, DARK);
    const char *label = LABELS[k - BOXES];
    int tw = textWidth(label);
    int tx = r.x + (r.w - tw) / 2 / PX * PX, ty = r.y + (r.h - FONT_H) / 2 / PX * PX;
    drawText(tx, ty, label, sel ? INK : LIGHT, DARK);
  }
}

// What an item shows: redrawn when it changes.
static uint8_t itemKey(int k) {
  return (k == menuSel && !onStatus) | (k < BOXES && projectFilled(k)) << 1 | (k == projectCurrent()) << 2;
}

void drawMenu() {
  static uint8_t shownKey[ITEMS];
  static uint32_t drawn = 0;                     // uiGeneration when last drawn whole
  if (!shown) return;
  bool all = drawn != uiGeneration;
  if (all) {
    drawn = uiGeneration;
    board.tft().fillRect(0, 0, 320, STATUS_TOP, BG);   // (where the track rows were)
  }
  for (int k = 0; k < ITEMS; k++) {
    uint8_t key = itemKey(k);
    if (!all && key == shownKey[k]) continue;
    shownKey[k] = key;
    drawItem(k);
  }
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
