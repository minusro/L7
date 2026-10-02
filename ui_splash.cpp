// ui_splash.cpp — the logo shown while the looper starts (see ui.h): "L8",
// white, in 2x2-pixel blocks, two track rows tall, in the middle of the screen,
// with the copyright line under it in a 3x5 font of the same blocks.
#include "ui.h"

static const char *const LOGO[] = {
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
static constexpr int LOGO_ROWS = sizeof LOGO / sizeof LOGO[0], LOGO_COLS = 43, LOGO_PIXEL = 2;
static constexpr int LOGO_X = (320 - LOGO_COLS * LOGO_PIXEL) / 2, LOGO_Y = (240 - LOGO_ROWS * LOGO_PIXEL) / 2;
static constexpr uint32_t SPLASH_MIN_MS = 1500;   // shown at least this long
static uint32_t shownAt = 0;

// The font: 5 rows per glyph (the copyright sign 7, from a row above), as
// wide as its strings ('#': lit), 1 block apart. Only the characters the
// splash uses ("\xA9": the copyright sign).
struct Glyph { char c; const char *rows[7]; int8_t top = 0; };
static const Glyph GLYPHS[] = {
    {'\xA9', {".#####.", "#.....#", "#.###.#", "#.#...#", "#.###.#", "#.....#", ".#####."}, -1},
    {'0', {"###", "#.#", "#.#", "#.#", "###"}},
    {'2', {"###", "..#", "###", "#..", "###"}},
    {'6', {"###", "#..", "###", "#.#", "###"}},
    {'D', {"##.", "#.#", "#.#", "#.#", "##."}},
    {'S', {"###", "#..", "###", "..#", "###"}},
    {'a', {"...", ".##", "#.#", "#.#", ".##"}},
    {'c', {"...", ".##", "#..", "#..", ".##"}},
    {'e', {"...", ".##", "#.#", "##.", ".##"}},
    {'i', {"#", ".", "#", "#", "#"}},
    {'l', {"#.", "#.", "#.", "#.", ".#"}},
    {'n', {"...", "##.", "#.#", "#.#", "#.#"}},
    {'t', {".#.", "###", ".#.", ".#.", ".##"}},
    {'u', {"...", "#.#", "#.#", "#.#", ".##"}},
    {' ', {"..", "..", "..", "..", ".."}},
};
static const char COPYRIGHT[] = "\xA9 2026 Daniel Stanciu";
static constexpr int TEXT_GAP = 12;               // logo to text, px

static const Glyph *glyphFor(char c) {
  for (const auto &g : GLYPHS) if (g.c == c) return &g;
  return nullptr;
}

// Width of a line of text, in blocks.
static int textCols(const char *text) {
  int cols = 0;
  for (const char *p = text; *p; p++)
    if (const Glyph *g = glyphFor(*p)) cols += strlen(g->rows[0]) + (p[1] ? 1 : 0);
  return cols;
}

static void drawText(int x, int y, const char *text, uint16_t color) {
  auto &t = board.tft();
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) continue;
    int w = strlen(g->rows[0]);
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
  drawText((320 - textCols(COPYRIGHT) * LOGO_PIXEL) / 2, TEXT_Y_SPLASH, COPYRIGHT, LOGO_COLOR);
  shownAt = millis();
}

void hideSplash() {
  while (millis() - shownAt < SPLASH_MIN_MS) delay(10);
  board.tft().fillScreen(BG);
  uiGeneration++;                                // everything redraws in full
  for (int i = 0; i < ROWS; i++) drawTrack(i);
}
