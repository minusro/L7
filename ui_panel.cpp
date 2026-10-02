// ui_panel.cpp — the panel (see ui.h): a box over the middle of the screen,
// PANEL_W x PANEL_H (60% x 80%), on the rest of the screen dimmed. Empty for
// now.
//
// The display has no frame buffer to read back and darken, so the dimming is
// a checkerboard of PANEL_SHADE pixels over what's there (half of them: a 50%
// screen at this pixel size). The status row's button stays as it is (it
// shows a house: A closes the panel).
#include "ui.h"

uint32_t uiGeneration = 1;

static constexpr int PANEL_W = 320 * 6 / 10, PANEL_H = 240 * 8 / 10;
static constexpr int PANEL_X = (320 - PANEL_W) / 2, PANEL_Y = (240 - PANEL_H) / 2;
static bool shown = false;

bool panelOpen() { return shown; }

static bool inPanel(int x, int y) {
  return x >= PANEL_X && x < PANEL_X + PANEL_W && y >= PANEL_Y && y < PANEL_Y + PANEL_H;
}
static bool inButton(int x, int y) {
  return x >= BUTTON_X && y >= bgTop(STATUS_ROW);
}

void openPanel() {
  if (shown) return;
  shown = true;
  auto &t = board.tft();
  // The dimming, a row at a time: every other pixel, offset on alternate rows.
  for (int y = 0; y < 240; y++)
    for (int x = y & 1; x < 320; x += 2)
      if (!inPanel(x, y) && !inButton(x, y)) t.drawPixel(x, y, PANEL_SHADE);
  t.fillRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL_BG);
  t.drawRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL_BORDER);
}

void closePanel() {
  if (!shown) return;
  shown = false;
  board.tft().fillScreen(BG);
  uiGeneration++;                                // everything redraws in full
  for (int i = 0; i < ROWS; i++) drawTrack(i);
}
