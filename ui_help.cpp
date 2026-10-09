// ui_help.cpp — the help pages (the menu's HELP): drawn from help_slides.h
// (tools/help/make_help.py), each page a list of steps (rectangles, text in
// L7's font at 1-6 px a dot, run-length coded images), then the page row:
// a square each, the pages seen (app.h helpSeen()) filled in their project's
// colour, the others a grey outline; the page shown in a light grey line 2 px
// out from its square (a gap), its corner blocks cut.
#include "ui.h"
#include "ui_font.h"
#include "help_slides.h"

int helpPages() { return HELP_PAGES; }

// Text at k px a dot (the font's blocks drawn k px square), from (x, y).
FLASHMEM static void helpText(int x, int y, int k, uint16_t ink, const char *s) {
  auto &t = board.tft();
  for (const char *p = s; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) g = glyphFor(toupper(*p));
    if (!g) g = glyphFor(' ');
    int w = glyphWidth(*g);
    for (int r = 0; r < 7 && g->rows[r]; r++)
      for (int c = 0; c < w;) {
        if (g->rows[r][c] != '#') { c++; continue; }
        int n = 1;
        while (c + n < w && g->rows[r][c + n] == '#') n++;
        t.fillRect(x + c * k, y + (r + g->top) * k, n * k, k, ink);
        c += n;
      }
    x += (w + 1) * k;
  }
}

// An image at (x0, y0): a row at a time (a transparent run: that row drawn run by run).
FLASHMEM static void helpImage(int x0, int y0, const HelpImage &im) {
  auto &t = board.tft();
  DMAMEM static uint16_t line[320];               // (RAM2: RAM1 is full)
  const uint8_t *run = im.runs, *end = im.runs + 2 * im.runCount;
  for (int y = 0; y < im.h && run < end; y++) {
    const uint8_t *rowStart = run;
    bool clear = false;
    for (int x = 0; x < im.w && run < end; run += 2) {
      int n = run[0] + 1;
      if (run[1] == 255) clear = true;
      else for (int i = 0; i < n && x + i < 320; i++) line[x + i] = im.pal[run[1]];
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

// The page row: a square each, 14 px apart, 8 px square, from (x, 224).
FLASHMEM static void helpDots(int x, int shown) {
  auto &t = board.tft();
  constexpr int Y = 224, SIZE = 8, STEP = 14;
  uint16_t seen = helpSeen();
  for (int i = 0; i < HELP_PAGES; i++) {
    int x0 = x + i * STEP;
    if (seen & (1 << i)) {
      t.fillRect(x0, Y, SIZE, SIZE, PROJECT_COLORS[i]);
    } else {
      t.fillRect(x0, Y, SIZE, SIZE, DIM);
      t.fillRect(x0 + 2, Y + 2, SIZE - 4, SIZE - 4, MENU_BG);
    }
    if (i != shown) continue;
    t.fillRect(x0 - 2, Y - 4, SIZE + 4, 2, LIGHT);             // (2 px out from the square, its corners cut)
    t.fillRect(x0 - 2, Y + SIZE + 2, SIZE + 4, 2, LIGHT);
    t.fillRect(x0 - 4, Y - 2, 2, SIZE + 4, LIGHT);
    t.fillRect(x0 + SIZE + 2, Y - 2, 2, SIZE + 4, LIGHT);
  }
}

FLASHMEM void drawHelpPage(int page) {
  if (page < 0 || page >= HELP_PAGES) return;
  auto &t = board.tft();
  t.setClipRect();
  const HelpPage &p = HELP[page];
  for (int i = 0; i < p.count; i++) {
    const HelpOp &op = p.ops[i];
    if (op.op == HELP_RECT) t.fillRect(op.x, op.y, op.w, op.h, op.color);
    else if (op.op == HELP_TEXT) helpText(op.x, op.y, op.k, op.color, op.text);
    else if (op.op == HELP_LOGO) drawLogoScaled(op.x, op.y, op.k, op.color);
    else helpImage(op.x, op.y, *op.img);
  }
  helpDots(p.dotsX, page);
}
