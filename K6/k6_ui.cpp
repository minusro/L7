// k6_ui.cpp — K6's drawing: the shared primitives/font (lifted from the
// N9-era app, k6_shared_ui.inc) and the six screens of the 3x2 map, mock
// content for now (the navigation prototype; design in
// docs/design/k6_mockup.html).
#include "k6_ui.h"

#include <stdio.h>

#include "k6_state.h"

#include "k6_shared_ui.h"

// ---- the top bar: map dots, clock, tempo, play, key — on every screen -----
static void topBar(const char *title) {
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      rct(4 + c * 6, 4 + r * 6, 4, 4, (r == nav.mapR && c == nav.mapC) ? ACCENT : DARK);
  txt(26, 6, title, INK);
  txt(140, 6, "1.1.1", LIGHT);
  txt(186, 6, "132", DIM);
  rct(212, 5, 4, 10, DIM);                       // play state (mock: stopped)
  txt(250, 6, "C MIN", DIM);
  rct(0, 20, 320, 2, DARK);
}

// ---- GRID -------------------------------------------------------------------
static void drawGrid() {
  topBar("GRID");
  constexpr int GX = 4, GY = 28, GW = 44, GH = 30, SX = 48, SY = 34;
  for (int t = 0; t < NT; t++)
    for (int s = 0; s < NS; s++) {
      int x = GX + t * SX, y = GY + s * SY;
      bool has = ((t * 7 + s * 3) & 3) != 1, playing = s == 1 && t < 4;
      frm(x, y, GW, GH, playing ? ACCENT : has ? LIGHT : DARK);
      if (has)
        for (int i = 0; i < 6; i++) rct(x + 4 + i * 6, y + 6 + ((t + i * s) % 4) * 5, 4, 2, LIGHT);
      if (playing) rct(x + 16, y + 4, 2, GH - 8, ACCENT);
      if (nav.gridT == t && nav.gridS == s) frm(x - 2, y - 2, GW + 4, GH + 4, INK);
    }
  for (int s = 0; s < NS; s++) {                 // the scene column
    int y = GY + s * SY;
    frm(296, y, 20, 30, s == 1 ? ACCENT : DARK);
    char n[2] = {(char)('1' + s), 0};
    txt(302, y + 10, n, s == 1 ? ACCENT : DIM);
    if (nav.gridT == NT && nav.gridS == s) frm(294, y - 2, 24, 34, INK);
  }
  rct(GX + nav.selTrack * SX, 234, GW, 4, ACCENT);
}

// ---- SONG -------------------------------------------------------------------
static void drawSong() {
  topBar("SONG");
  txt(8, 28, "ROW  SCENE   REP", DIM);
  static const uint8_t ROWS[5][2] = {{1, 2}, {1, 2}, {2, 4}, {3, 1}, {2, 4}};
  for (int i = 0; i < 5; i++) {
    int y = 44 + i * 24;
    bool playing = i == 2;
    if (playing) rct(4, y - 4, 312, 22, DARK);
    char b[12];
    snprintf(b, sizeof b, "%02d", i + 1);
    txt(8, y, b, playing ? ACCENT : DIM);
    snprintf(b, sizeof b, "SCENE %d", ROWS[i][0]);
    txt(60, y, b, playing ? INK : LIGHT);
    snprintf(b, sizeof b, "X%d", ROWS[i][1]);
    txt(170, y, b, LIGHT);
    if (playing) { rct(230, y + 2, 60, 6, BG); rct(230, y + 2, 36, 6, ACCENT); }
    if (nav.songRow == i) frm(4, y - 4, 312, 22, INK);
  }
  txt(8, 220, "A ADD  A+ARROWS SCENE/REP  B JUMP", DIM);
}

// ---- MIX --------------------------------------------------------------------
static void drawMix() {
  topBar("MIX");
  static const uint8_t V[7] = {62, 48, 70, 30, 55, 66, 74};
  for (int t = 0; t <= NT; t++) {
    int x = 8 + t * 44;
    bool master = t == NT;
    char n[2] = {master ? 'M' : (char)('1' + t), 0};
    txt(x + 14, 28, n, master ? INK : DIM);
    frm(x, 42, 36, 150, DARK);
    int h = V[t] * 142 / 99;
    rct(x + 6, 42 + 146 - h, 10, h, master ? INK : LIGHT);
    rct(x + 20, 42 + 146 - (h - 12), 8, h - 12, ACCENT_DARK);
    if (t == 3) txt(x + 4, 198, "MUTE", ACCENT);
    rct(x, 214, 36, 2, DIM);
    rct(x + 4 + (V[t] - 30) / 3, 212, 4, 6, LIGHT);
    if (nav.mixT == t) frm(x - 3, 39, 42, 180, INK);
  }
}

// ---- NOTE -------------------------------------------------------------------
static void drawNote() {
  topBar("NOTE");
  txt(8, 28, "T3 S2  BAR 2/4", DIM);
  const int X0 = 48, SW = 16, Y0 = 44, RH = 15;
  for (int r = 0; r < 12; r++)
    rct(X0, Y0 + r * RH, 16 * SW, RH - 1, (r % 3 == 1) ? BG : DARK);
  for (int g = 0; g <= 16; g += 4) rct(X0 + g * SW, Y0, 1, 12 * RH, DIM);
  static const uint8_t NOTES[4][4] = {{0, 2, 2, 0}, {4, 4, 4, 1}, {10, 2, 7, 0}, {13, 3, 1, 2}};
  for (int i = 0; i < 4; i++) {
    int x = X0 + NOTES[i][0] * SW + 1, y = Y0 + NOTES[i][2] * RH + 3, w = NOTES[i][1] * SW - 2;
    uint16_t ink = NOTES[i][3] == 2 ? ACCENT : LIGHT;
    frm(x, y, w, RH - 7, ink);
    rct(x + 2, y + 2, w - 4, RH - 11, NOTES[i][3] == 1 ? LIGHT : NOTES[i][3] == 2 ? ACCENT_DARK : DARK);
  }
  txt(8, Y0 + 4 * RH + 2, "D#3", INK);
  frm(X0 + nav.noteStep * SW, Y0 + 4 * RH, SW, RH - 1, INK);
  rct(X0 + 9 * SW, Y0, 2, 12 * RH, ACCENT);
}

// ---- INST -------------------------------------------------------------------
static void drawInst() {
  topBar("INST");
  if (nav.instRow == 0) frm(4, 26, 126, 22, INK);
  txt(10, 32, "< PROPHET >", INK);
  txt(142, 32, "12 ACOU GRAND", LIGHT);
  static const char *const TABS[3] = {"SYN", "MOD", "FX"};
  int tx = 6;
  for (int p = 0; p < 3; p++) {
    txt(tx, 54, TABS[p], p == nav.instPage ? INK : DIM);
    if (p == nav.instPage) rct(tx, 64, txtW(TABS[p]), 2, ACCENT);
    tx += txtW(TABS[p]) + 14;
  }
  static const char *const N[8] = {"DETUNE", "CUTOFF", "RES", "ENV", "ATK", "DEC", "SUS", "REL"};
  for (int i = 0; i < 8; i++) {
    int row = i / 4, col = i % 4;
    int x = 4 + col * 79, y = 74 + row * 56;
    bool sel = nav.instRow == row + 1 && nav.instCol == col;
    if (sel) { rct(x, y, 76, 50, DARK); frm(x, y, 76, 50, LIGHT); }
    txt(x + 6, y + 6, N[i], sel ? LIGHT : DIM);
    txt(x + 6, y + 20, "50", INK);
    rct(x + 6, y + 40, 64, 4, sel ? BG : DARK);
    rct(x + 6, y + 40, 32, 4, sel ? ACCENT : LIGHT);
  }
}

// ---- PROJ -------------------------------------------------------------------
static void drawProj() {
  topBar("PROJ");
  for (int k = 0; k < 12; k++) {
    int x = 16 + (k % 4) * 74, y = 32 + (k / 4) * 44;
    frm(x, y, 64, 36, k == 0 ? ACCENT : DIM);
    char n[2] = {(char)('A' + k), 0};
    txt(x + 28, y + 12, n, k == 0 ? ACCENT : DIM);
    if (nav.projSel == k) frm(x - 2, y - 2, 68, 40, INK);
  }
  static const char *const ITEMS[3] = {"STORAGE MODE", "KEY / SCALE", "SWITCH TO L7"};
  for (int i = 0; i < 3; i++) {
    int y = 172 + i * 20;
    txt(90, y, ITEMS[i], nav.projSel == 12 + i ? INK : LIGHT);
    if (nav.projSel == 12 + i) frm(84, y - 4, 152, 18, INK);
  }
}

// ---- the K6 mark + static splash --------------------------------------------
static int lu(float u, float s) { return (int)(u * s + 0.5f); }
static void drawMark(int x, int y, float s, uint16_t c) {
  int h = lu(26, s);
  for (int p = 0; p < h; p++) {
    if ((p >= lu(18, s) && p < lu(20, s)) || (p >= lu(22, s) && p < lu(24, s))) continue;
    int off = (h - 1 - p) / 2;
    int mid = lu(13, s);
    rct(x + off, y + p, lu(6, s), 1, c);
    if (p < mid) rct(x + off + lu(6, s) + (mid - 1 - p) / 2, y + p, lu(7, s), 1, c);
    else rct(x + off + lu(6, s) + (p - mid) / 2, y + p, lu(7, s), 1, c);
    rct(x + off + lu(24, s), y + p, lu(7, s), 1, c);
    if ((p >= lu(13, s) && p < lu(16, s)) || p >= lu(23, s))
      rct(x + off + lu(24, s), y + p, lu(21, s), 1, c);
    else if (p >= lu(16, s))
      rct(x + off + lu(38, s), y + p, lu(7, s), 1, c);
  }
}

void splashShow() {
  rct(0, 0, 320, 240, BG);
  drawMark((320 - 145) / 2 + 12, (240 - 65) / 2, 2.5f, LOGO_COLOR);
  board.tft().updateScreen();
}

// ---- entry -------------------------------------------------------------------
void drawScreen() {
  board.tft().fillScreen(BG);
  switch (screenAt(nav.mapR, nav.mapC)) {
    case SCR_SONG: drawSong(); break;
    case SCR_GRID: drawGrid(); break;
    case SCR_MIX:  drawMix();  break;
    case SCR_PROJ: drawProj(); break;
    case SCR_NOTE: drawNote(); break;
    default:       drawInst(); break;
  }
  board.tft().updateScreen();
}
