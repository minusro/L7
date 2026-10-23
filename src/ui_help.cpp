// ui_help.cpp — the help pages (the menu's HELP): drawn from help_slides.h,
// or help_slides_m02.h on the Model:02, whose screen the pages are laid out
// for (both from tools/help/make_help.py, which takes M8_MODEL)
// — each page a list of steps (rectangles, text in
// L7's font at 1-6 px a dot, run-length coded images), then the page row:
// a square each, the pages seen (app.h helpSeen()) filled in their project's
// colour, the others a grey outline; the page shown in its colour, wider,
// down to the screen's bottom. Rainbow text animated (helpService()).
#include "ui.h"
#include "ui_font.h"
#if M8_MODEL == 1
#include "help_slides.h"
#else
#include "help_slides_m02.h"
#endif

int helpPages() { return HELP_PAGES; }

// Text at k px a dot (the font's blocks drawn k px square), from (x, y);
// squeezed: its middle row half as tall (the titles: a block shorter).
FLASHMEM static void helpText(int x, int y, int k, uint16_t ink, const char *s, bool squeezed = false) {
  auto &t = board.tft();
  for (const char *p = s; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) g = glyphFor(toupper(*p));
    if (!g) g = glyphFor(' ');
    int w = glyphWidth(*g);
    for (int r = 0; r < 7 && g->rows[r]; r++) {
      int row = r + g->top, top = row * k, h = k;   // (squeezed: row 2 half as tall, the rows under it that much higher)
      if (squeezed && row == 2) h = k / 2;
      if (squeezed && row > 2) top -= k / 2;
      for (int c = 0; c < w;) {
        if (g->rows[r][c] != '#') { c++; continue; }
        int n = 1;
        while (c + n < w && g->rows[r][c + n] == '#') n++;
        t.fillRect(x + c * k, y + top, n * k, h, ink);
        c += n;
      }
    }
    x += (w + 1) * k;
  }
}

// An image at (x0, y0): a row at a time (a transparent run: that row drawn run by run).
FLASHMEM static void helpImage(int x0, int y0, const HelpImage &im) {
  auto &t = board.tft();
  DMAMEM static uint16_t line[SCREEN_W];               // (RAM2: RAM1 is full)
  const uint8_t *run = im.runs, *end = im.runs + 2 * im.runCount;
  for (int y = 0; y < im.h && run < end; y++) {
    const uint8_t *rowStart = run;
    bool clear = false;
    for (int x = 0; x < im.w && run < end; run += 2) {
      int n = run[0] + 1;
      if (run[1] == 255) clear = true;
      else for (int i = 0; i < n && x + i < SCREEN_W; i++) line[x + i] = im.pal[run[1]];
      x += n;
    }
    if (!clear) {
      t.writeRect(x0, y0 + y, im.w, 1, line);
      continue;
    }
    for (int x = 0; rowStart < run; rowStart += 2) {   // (the opaque runs only)
      int n = rowStart[0] + 1;
      if (rowStart[1] != 255) t.fillRect(x0 + x, y0 + y, n, 1, im.pal[rowStart[1]]);
      x += n;
    }
  }
}

// The page row: a square each, 16 px apart, 8 px square, from (x, 226).
static int dotsX = 10;
FLASHMEM static void helpDots(int x, int shown) {
  auto &t = board.tft();
  constexpr int Y = SCREEN_H - 14, SIZE = 8, STEP = 16;   // 226 on the Model:01
  uint16_t seen = helpSeen();
  t.fillRect(0, Y - 2 * PX, SCREEN_W, SCREEN_H - (Y - 2 * PX), MENU_BG);   // (the row: places shift as the page turns)
  for (int i = 0; i < HELP_PAGES; i++) {
    int x0 = x + i * STEP;                         // (the shown page: two blocks more space either side, not past the ends)
    if (shown > 0 && i >= shown) x0 += 2 * PX;
    if (shown < HELP_PAGES - 1 && i > shown) x0 += 2 * PX;
    if (i == shown) {                              // (the shown page: two blocks wider each side, down to the bottom)
      t.fillRect(x0 - 2 * PX, Y, SIZE + 4 * PX, SCREEN_H - Y, PROJECT_COLORS[i]);   // (its top level with the others', down to the screen's bottom)
    } else if (seen & (1 << i)) {
      t.fillRect(x0, Y, SIZE, SIZE, PROJECT_COLORS[i]);
    } else {
      t.fillRect(x0, Y, SIZE, SIZE, DIM);
      t.fillRect(x0 + 2, Y + 2, SIZE - 4, SIZE - 4, MENU_BG);
    }
  }
}

// A turn (LEFT/RIGHT): nothing more now (the square drawn with the page).
void helpTurned() {}

// The rainbow text ([...] in make_help.py: "L7" on the sync page): in the
// projects' colours in turn, a colour every RAINBOW_MS.
static constexpr uint32_t RAINBOW_MS = 120;
static int rainbowShown = -1;                    // (the colour it was drawn in)
static uint16_t rainbowColor() { return PROJECT_COLORS[millis() / RAINBOW_MS % 12]; }
FLASHMEM static void drawRainbow(int page) {
  const HelpPage &p = HELP[page];
  rainbowShown = millis() / RAINBOW_MS % 12;
  for (int i = 0; i < p.count; i++)
    if (p.ops[i].op == HELP_RAINBOW) helpText(p.ops[i].x, p.ops[i].y, p.ops[i].k, rainbowColor(), p.ops[i].text);
}
// Every pass while the help shows: the rainbow text on to its next colour.
FLASHMEM void helpService(int page) {
  if (page < 0 || page >= HELP_PAGES || (int)(millis() / RAINBOW_MS % 12) == rainbowShown) return;
  drawRainbow(page);
}

FLASHMEM void drawHelpPage(int page) {
  if (page < 0 || page >= HELP_PAGES) return;
  auto &t = board.tft();
  t.setClipRect();
  const HelpPage &p = HELP[page];
  for (int i = 0; i < p.count; i++) {
    const HelpOp &op = p.ops[i];
    if (op.op == HELP_RECT) t.fillRect(op.x, op.y, op.w, op.h, op.color);
    else if (op.op == HELP_TEXT) helpText(op.x, op.y, op.k, op.color, op.text, op.w);   // (w: squeezed)
    else if (op.op == HELP_LOGO) drawLogoScaled(op.x, op.y, op.k, op.color);
    else if (op.op == HELP_RAINBOW) helpText(op.x, op.y, op.k, rainbowColor(), op.text);
    else helpImage(op.x, op.y, *op.img);
  }
  dotsX = p.dotsX;
  helpDots(p.dotsX, page);
}
