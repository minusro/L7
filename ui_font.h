// ui_font.h — the pixel font's glyphs (ui_font.cpp), for drawing text
// outside the usual text boxes (the splash screen).
#pragma once

struct Glyph { char c; const char *rows[7]; int8_t top = 0; };   // rows from `top` (blocks)
const Glyph *glyphFor(char c);                     // nullptr: not in the font
int glyphWidth(const Glyph &g);                    // in blocks
