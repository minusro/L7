// ui_font.cpp — the pixel font and tags (see ui.h)
#include "ui.h"
#include "ui_font.h"

// Glyphs: 5 rows ('#': lit), as wide as their strings, drawn a block apart.
// The copyright sign (splash only) is 7 rows, from a row above.
PROGMEM static const Glyph GLYPHS[] = {
    {' ', {"..", "..", "..", "..", ".."}},
    {'.', {".", ".", ".", ".", "#"}},
    {':', {".", "#", ".", "#", "."}},
    {'/', {"..#", "..#", ".#.", "#..", "#.."}},
    {'*', {"...", ".#.", "###", ".#.", "..."}},   // (the scene's "*": a small plus)
    {'-', {"...", "...", "###", "...", "..."}},
    {'+', {"...", ".#.", "###", ".#.", "..."}},
    {'0', {"###", "#.#", "#.#", "#.#", "###"}},
    {'1', {".#.", "##.", ".#.", ".#.", "###"}},
    {'2', {"###", "..#", "###", "#..", "###"}},
    {'3', {"###", "..#", "###", "..#", "###"}},
    {'4', {"#.#", "#.#", "###", "..#", "..#"}},
    {'5', {"###", "#..", "###", "..#", "###"}},
    {'6', {"###", "#..", "###", "#.#", "###"}},
    {'7', {"###", "..#", "..#", "..#", "..#"}},
    {'8', {"###", "#.#", "###", "#.#", "###"}},
    {'9', {"###", "#.#", "###", "..#", "###"}},
    {'A', {".#.", "#.#", "###", "#.#", "#.#"}},
    {'B', {"##.", "#.#", "##.", "#.#", "##."}},
    {'C', {".##", "#..", "#..", "#..", ".##"}},
    {'D', {"##.", "#.#", "#.#", "#.#", "##."}},
    {'E', {"###", "#..", "##.", "#..", "###"}},
    {'F', {"###", "#..", "##.", "#..", "#.."}},
    {'G', {".##", "#..", "#.#", "#.#", ".##"}},
    {'H', {"#.#", "#.#", "###", "#.#", "#.#"}},
    {'I', {"###", ".#.", ".#.", ".#.", "###"}},
    {'J', {"..#", "..#", "..#", "#.#", ".#."}},
    {'K', {"#.#", "#.#", "##.", "#.#", "#.#"}},
    {'L', {"#..", "#..", "#..", "#..", "###"}},
    {'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#"}},
    {'N', {"#..#", "##.#", "#.##", "#..#", "#..#"}},
    {'O', {".#.", "#.#", "#.#", "#.#", ".#."}},
    {'P', {"##.", "#.#", "##.", "#..", "#.."}},
    {'Q', {".#.", "#.#", "#.#", "##.", ".##"}},
    {'R', {"##.", "#.#", "##.", "#.#", "#.#"}},
    {'S', {"###", "#..", "###", "..#", "###"}},
    {'T', {"###", ".#.", ".#.", ".#.", ".#."}},
    {'U', {"#.#", "#.#", "#.#", "#.#", "###"}},
    {'V', {"#.#", "#.#", "#.#", "#.#", ".#."}},
    {'W', {"#...#", "#...#", "#.#.#", "##.##", "#...#"}},
    {'X', {"#.#", "#.#", ".#.", "#.#", "#.#"}},
    {'Y', {"#.#", "#.#", ".#.", ".#.", ".#."}},
    {'Z', {"###", "..#", ".#.", "#..", "###"}},
    // Lower case: only what the splash screen uses.
    {'a', {"...", ".##", "#.#", "#.#", ".##"}},
    {'c', {"...", ".##", "#..", "#..", ".##"}},
    {'e', {"...", ".##", "#.#", "##.", ".##"}},
    {'i', {"#", ".", "#", "#", "#"}},
    {'l', {"#.", "#.", "#.", "#.", ".#"}},
    {'n', {"...", "##.", "#.#", "#.#", "#.#"}},
    {'t', {".#.", "###", ".#.", ".#.", ".##"}},
    {'u', {"...", "#.#", "#.#", "#.#", ".##"}},
    {'\xA9', {".#####.", "#.....#", "#.###.#", "#.#...#", "#.###.#", "#.....#", ".#####."}, -1},
};

const Glyph *glyphFor(char c) {
  for (const auto &g : GLYPHS) if (g.c == c) return &g;
  return nullptr;
}

int glyphWidth(const Glyph &g) { return strlen(g.rows[0]); }

int textWidth(const char *text) {
  int blocks = 0;
  for (const char *p = text; *p; p++)
    if (const Glyph *g = glyphFor(*p)) blocks += glyphWidth(*g) + (p[1] ? 1 : 0);
  return blocks * PX;
}

// A text box is built in this buffer and sent at once.
static uint16_t box[160 * (FONT_H + 4 * PX)];

// Draws text into the box buffer (w px wide) at block offset (bx, by).
static void putText(int w, int bx, int by, const char *text, uint16_t ink) {
  for (const char *p = text; *p; p++) {
    const Glyph *g = glyphFor(*p);
    if (!g) continue;
    int gw = glyphWidth(*g);
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < gw; c++)
        if (g->rows[r][c] == '#')
          for (int y = 0; y < PX; y++)
            for (int x = 0; x < PX; x++) box[((by + r) * PX + y) * w + (bx + c) * PX + x] = ink;
    bx += gw + 1;
  }
}

void drawText(int x, int y, const char *text, uint16_t ink, uint16_t bg, int width) {
  int w = max(textWidth(text), width);
  if (w <= 0 || w * FONT_H > (int)(sizeof box / sizeof box[0])) return;   // (plain text: up to 288 px wide)
  for (int i = 0; i < w * FONT_H; i++) box[i] = bg;
  putText(w, 0, 0, text, ink);
  board.tft().writeRect(x, y, w, FONT_H, box);
}

static constexpr int TAG_PAD = PX;                 // around the text, inside the box

int tagWidth(const char *text) { return textWidth(text) + 2 * TAG_PAD + 2 * PX; }

void drawTag(int x, int y, const char *text, TagStyle style, uint16_t ink, uint16_t bg, int width) {
  int w = max(tagWidth(text), width), h = TAG_H;
  if (w > 160) return;
  for (int py = 0; py < h; py++)
    for (int px = 0; px < w; px++) {
      bool edge = onFrame(px, py, w, h);
      bool corner = (px < PX || px >= w - PX) && (py < PX || py >= h - PX);
      box[py * w + px] = corner || style == TAG_PLAIN ? bg : style == TAG_INVERTED || edge ? ink : bg;
    }
  putText(w, (w / PX - textWidth(text) / PX) / 2, (TAG_PAD + PX) / PX, text, style == TAG_INVERTED ? bg : ink);
  board.tft().writeRect(x, y, w, h, box);
}
