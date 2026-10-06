// ui.cpp — drawing rows, and the screen refresh pass (see ui.h)
#include "ui.h"
#include <utility>

int borderAt(int i, int cell, int r, int c, int rows, int cols) {
  bool l = c == 0, rt = c == cols - 1, t = r == 0, b = r == rows - 1;
  if (!(l || rt || t || b)) return -1;
  if ((l || rt) && (t || b)) return BG;            // corners: cut
  uint16_t own = cellFrame(i, cell);
  if (own != BG) return own;
  int ni = t ? rowAbove(i) : b ? rowBelow(i) : i, nc = l ? cell - 1 : rt ? cell + 1 : cell;
  if (i == STATUS_ROW) {                           // (its cells only share sides with each other)
    if (t || b || nc < 0 || nc >= cellCount(i)) return BG;
    return cellFrame(i, nc);
  }
  if (ni < 0 || ni >= NUM_TRACKS || nc < 0 || nc >= CELLS) return BG;
  if (nc != cell) {                                // only a cell right next to it (no space between)
    const Cell &a = CELL[min(cell, nc)], &n = CELL[max(cell, nc)];
    if (a.x + a.w - PX != n.x) return BG;
  }
  return cellFrame(ni, nc);
}

uint64_t bordersOf(int i, int cell) {
  // A middle block of each side (rows / columns: any cell's are at least 3).
  const int rows = 3, cols = 3;
  uint64_t k = 0;
  for (auto rc : {std::pair<int, int>{1, 0}, {1, 2}, {0, 1}, {2, 1}})
    k = k << 16 | (uint16_t)borderAt(i, cell, rc.first, rc.second, rows, cols);
  return k;
}

void drawTrack(int i) {
  if (i < 0 || i >= ROWS || (menuOpen() && i != STATUS_ROW)) return;   // (the menu: in the track rows' place)
  drawCells(i);
  if (i == STATUS_ROW) {
    drawStatus();
    return;
  }
  drawFilter(i);
  drawMix(i);
  drawWave(i, recordingOn(i));
}

// The accent colour: the loaded project's (ACCENT_DARK at half its
// brightness). A change redraws everything.
static void followProjectColor() {
  static int shownProject = -1;
  int p = projectCurrent();
  if (p == shownProject) return;
  shownProject = p;
  ACCENT = p >= 0 ? PROJECT_COLORS[p] : LOGO_COLOR;   // (the dummy project: the splash's turquoise)
  ACCENT_DARK = blend565(ACCENT, BG, 128);
  uiGeneration++;
  for (int i = 0; i < ROWS; i++) drawTrack(i);
}

void uiRefresh() {
  followProjectColor();
  // Redraw a little at a time so SD streaming keeps going.
  static uint32_t lastDraw = 0;
  static int nextRow = 0;
  if (millis() - lastDraw >= 30) {
    lastDraw = millis();
    if (menuOpen()) {                                // the menu and the status row
      drawCells(STATUS_ROW);
      drawStatus();
      drawMenu();
      return;
    }
    // Every pass, all rows' buttons (only what changed is redrawn): the
    // recording track's icon flashes; the status row's follows SHIFT, the
    // selection and the transport.
    for (int i = 0; i < ROWS; i++) drawCells(i);
    drawStatus();                                    // every pass: the clock shows steps
    updatePlayheads();
    for (int i = 0; i < NUM_TRACKS; i++) drawMix(i);   // the meters
    drawTrack(nextRow);
    if (++nextRow >= ROWS) nextRow = 0;
  }
}
