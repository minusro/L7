// ui_splash.cpp — the logo shown while the looper starts (see ui.h): "L8",
// in the accent colour, in 2x2-pixel blocks, two track rows tall, in the middle of the screen,
// with the copyright line under it in light grey, in the pixel font (ui_font.cpp).
#include "ui.h"
#include "ui_font.h"

PROGMEM static const char LOGO[][44] = {          // (in flash: const data would sit in RAM1)
    "..............................############.",
    ".........##.................##############.",
    "........###................###############.",
    ".......###................################.",
    "......####...............####........####..",
    ".....#####..............#####........####..",
    ".....####...............#####......######..",
    "....#####...............####......#######..",
    "....#####...............####....########...",
    "....#####...............###############....",
    "....####.................#############.....",
    "...#####..................###########......",
    "...#####.................###########.......",
    "...#####................#############......",
    "...####................###############.....",
    "..#####...............#####...########.....",
    "..#####..............#####......######.....",
    "...........................................",
    "..####...............####........#####.....",
    "..####...............####........#####.....",
    "...........................................",
    ".##############......####........####......",
    ".###############.....#################.....",
    "...........................................",
    "#################......################....",
    "##################.....#################...",
};
static constexpr int LOGO_ROWS = sizeof LOGO / sizeof LOGO[0], LOGO_COLS = sizeof LOGO[0] - 1, LOGO_PIXEL = 2;
// The logo and the copyright line under it centred together (the logo
// above the middle).
static constexpr int TEXT_GAP = 24;               // logo to text, px
static constexpr int GROUP_H = LOGO_ROWS * LOGO_PIXEL + TEXT_GAP + 6 * LOGO_PIXEL;   // logo, gap, text (to the copyright sign's bottom)
static constexpr int GROUP_DOWN = 8;                 // the group sits this far below the middle, px
static constexpr int LOGO_X = (320 - LOGO_COLS * LOGO_PIXEL) / 2,
                     LOGO_Y = (240 - GROUP_H) / 2 / LOGO_PIXEL * LOGO_PIXEL + GROUP_DOWN;
static constexpr uint32_t SPLASH_MIN_MS = 2000;   // shown at least this long
static uint32_t shownAt = 0;

static const char COPYRIGHT[] = "\xA9 2026 Daniel Stanciu";

// Width of a line of text, in blocks.
static int textCols(const char *text) {
  int cols = 0;
  for (const char *p = text; *p; p++)
    if (const Glyph *g = glyphFor(*p)) cols += glyphWidth(*g) + (p[1] ? 1 : 0);
  return cols;
}

static void drawLine(int x, int y, const char *text, uint16_t color) {   // (the copyright sign reaches a row above y)
  auto &t = board.tft();
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) continue;
    int w = glyphWidth(*g);
    for (int r = 0; r < 7 && g->rows[r]; r++)
      for (int c = 0; c < w; c++)
        if (g->rows[r][c] == '#')
          t.fillRect(x + c * LOGO_PIXEL, y + (g->top + r) * LOGO_PIXEL, LOGO_PIXEL, LOGO_PIXEL, color);
    x += (w + 1) * LOGO_PIXEL;
  }
}

static constexpr int TEXT_Y_SPLASH = LOGO_Y + LOGO_ROWS * LOGO_PIXEL + TEXT_GAP;

int splashBottom() { return TEXT_Y_SPLASH + 6 * LOGO_PIXEL; }   // (the copyright sign's last row)

void showSplash() {
  auto &t = board.tft();
  t.fillScreen(BG);
  for (int r = 0; r < LOGO_ROWS; r++)
    for (int c = 0; c < LOGO_COLS; c++)
      if (LOGO[r][c] == '#') t.fillRect(LOGO_X + c * LOGO_PIXEL, LOGO_Y + r * LOGO_PIXEL, LOGO_PIXEL, LOGO_PIXEL, LOGO_COLOR);
  drawLine((320 - textCols(COPYRIGHT) * LOGO_PIXEL) / 2, TEXT_Y_SPLASH, COPYRIGHT, SPLASH_TEXT);
  shownAt = millis();
}

void hideSplash() {
  while (millis() - shownAt < SPLASH_MIN_MS) delay(10);
  board.tft().fillScreen(BG);
  uiGeneration++;                                // everything redraws in full
  for (int i = 0; i < ROWS; i++) drawTrack(i);
}
