// n9_ui.cpp — see n9_ui.h. All sizes are in pixels on L7's 2x2 block grid.
#include "n9_ui.h"

#include <stdio.h>

#include "n9_state.h"

static constexpr int PX = 2;

// ---- Primitives -----------------------------------------------------------

static void rct(int x, int y, int w, int h, uint16_t c) {
  if (w > 0 && h > 0) board.tft().fillRect(x, y, w, h, c);
}

static void frm(int x, int y, int w, int h, uint16_t c) {
  rct(x, y, w, PX, c); rct(x, y + h - PX, w, PX, c);
  rct(x, y, PX, h, c); rct(x + w - PX, y, PX, h, c);
}

static int rnd(float f) { return (int)(f + 0.5f); }

// ---- The 3x5 font (ui_font.cpp's glyphs, upper case + digits + signs) -----

struct Glyph { char c; const char *rows[5]; };
static const Glyph GLYPHS[] = {
    {' ', {"..", "..", "..", "..", ".."}},
    {'.', {".", ".", ".", ".", "#"}},
    {':', {".", "#", ".", "#", "."}},
    {'/', {"..#", "..#", ".#.", "#..", "#.."}},
    {'*', {"...", "#.#", ".#.", "#.#", "..."}},
    {'-', {"...", "...", "###", "...", "..."}},
    {'+', {"...", ".#.", "###", ".#.", "..."}},
    {'<', {"...#", "..##", ".###", "..##", "...#"}},
    {'>', {"#...", "##..", "###.", "##..", "#..."}},
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
};

static const Glyph *glyph(char c) {
  for (const auto &g : GLYPHS) if (g.c == c) return &g;
  return nullptr;
}

static int txtW(const char *s) {
  int b = 0;
  for (const char *p = s; *p; p++) if (const Glyph *g = glyph(*p)) b += strlen(g->rows[0]) + 1;
  return b ? b * PX - PX : 0;
}

static void txt(int x, int y, const char *s, uint16_t ink) {
  int bx = 0;
  for (const char *p = s; *p; p++) {
    const Glyph *g = glyph(*p); if (!g) continue;
    int gw = strlen(g->rows[0]);
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < gw; c++)
        if (g->rows[r][c] == '#') rct(x + (bx + c) * PX, y + r * PX, PX, PX, ink);
    bx += gw + 1;
  }
}

// Inverted tag, L7 style: ink box, cut corners, black text; 18 px tall.
static int tagI(int x, int y, const char *s, uint16_t bg = INK, uint16_t ink = BG) {
  int w = txtW(s) + 8, h = 18;
  rct(x, y, w, h, bg);
  rct(x, y, PX, PX, BG); rct(x + w - PX, y, PX, PX, BG);
  rct(x, y + h - PX, PX, PX, BG); rct(x + w - PX, y + h - PX, PX, PX, BG);
  txt(x + (w - txtW(s)) / 2, y + 4, s, ink);
  return w;
}

// ---- Shared chrome --------------------------------------------------------

// The screen title row (y 0-21): inverted name tag, dim context text after.
static void title(const char *name, const char *context) {
  int w = tagI(4, 2, name);
  if (context) txt(4 + w + 8, 6, context, DIM);
}

// Status row (y 214-239): screen-stack dots, scene, clock, tempo, action button.
static void statusRow(uint8_t screenIx) {
  const char *scene = "2", *clock = "2.3.1", *bpm = "132.0";
  for (int k = 0; k < SCR_COUNT; k++) rct(4, 215 + k * 3, 2, 2, k == screenIx ? ACCENT : DARK);
  int w = tagI(12, 218, "SCN");
  txt(12 + w + 4, 222, scene, LIGHT);
  // Play icon + clock, roughly centred as L7's clock cell.
  const int px0 = 128, py0 = 218, hh[5] = {10, 8, 6, 4, 2};
  for (int i = 0; i < 5; i++) rct(px0 + i * 2, py0 + (10 - hh[i]) / 2, 2, hh[i], ACCENT);
  txt(px0 + 14, 218, clock, LIGHT);
  txt(292 - txtW(bpm) - txtW("BPM") - 12, 222, bpm, DIM);
  tagI(292 - txtW("BPM") - 8, 218, "BPM");
  // Action button: accent, left corners cut, a plus for now.
  rct(294, 214, 26, 26, ACCENT);
  rct(294, 214, PX, PX, BG); rct(294, 238, PX, PX, BG);
  rct(304, 222, 6, 2, BG); rct(306, 220, 2, 6, BG);
}

// A parameter cell: name dim, value white, amount bar under (amt < 0: none);
// selected = frame + dark ground.
static void paramCell(int x, int y, int w, int h, const char *name, const char *val, float amt, bool sel) {
  if (sel) { rct(x, y, w, h, DARK); frm(x, y, w, h, LIGHT); }
  txt(x + 6, y + 6, name, sel ? LIGHT : DIM);
  txt(x + 6, y + 20, val, INK);
  if (amt >= 0) {
    rct(x + 6, y + h - 10, w - 12, 4, sel ? BG : DARK);
    rct(x + 6, y + h - 10, rnd((w - 12) * amt), 4, sel ? ACCENT : LIGHT);
  }
}

// ---- 2 · Send effects -------------------------------------------

static void sendRow(int y, const char *label, const float *levels, int selTrack) {
  txt(4, y + 8, label, DIM);
  for (int t = 0; t < 9; t++) {
    int x = 64 + t * 28, bh = rnd(18 * levels[t]);
    bool sel = t == selTrack;
    frm(x, y, 20, 22, sel ? LIGHT : DARK);
    rct(x + 4, y + 18 - bh, 12, bh + 2 - PX, sel ? ACCENT : (levels[t] > 0 ? LIGHT : DARK));
  }
}

static void drawFx() {
  title("FX", "SENDS");
  struct { const char *n, *v; float a; } d[] = {{"TIME", "3/16", 0.45f}, {"FDBK", "52", 0.52f}, {"HP", "160", 0.25f}, {"MIX", "100", 1.0f}},
                                         r[] = {{"SIZE", "72", 0.72f}, {"DAMP", "45", 0.45f}, {"PRE", "20", 0.2f}, {"MIX", "100", 1.0f}};
  txt(6, 26, "DELAY", LIGHT);
  for (int i = 0; i < 4; i++) paramCell(4 + i * 79, 38, 76, 42, d[i].n, d[i].v, d[i].a, nav.fxRow == 0 && nav.fxCol == i);
  txt(6, 88, "REVERB", LIGHT);
  for (int i = 0; i < 4; i++) paramCell(4 + i * 79, 100, 76, 42, r[i].n, r[i].v, r[i].a, nav.fxRow == 1 && nav.fxCol == i);
  txt(6, 150, "SENDS", LIGHT);
  const float a[9] = {0.3f, 0, 0.7f, 0.2f, 0, 0.5f, 0.1f, 0, 0},
              b[9] = {0.5f, 0.2f, 0.4f, 0, 0.6f, 0.3f, 0, 0.2f, 0};
  sendRow(162, "A DEL", a, nav.fxRow == 2 ? nav.fxCol : -1);
  sendRow(188, "B REV", b, nav.fxRow == 3 ? nav.fxCol : -1);
}

// ---- 3 · Mixer -------------------------------------------------------------

static void drawMix() {
  title("MIX", "N9 A02");
  const float vols[9] = {0.8f, 0.6f, 0.7f, 0.5f, 0.65f, 0.55f, 0.4f, 0.75f, 0},
              mets[9] = {0.6f, 0.4f, 0.68f, 0.3f, 0.5f, 0, 0.2f, 0.6f, 0},
              pans[9] = {0, -0.4f, 0, 0.3f, 0, 0, -0.2f, 0.5f, 0};
  const bool muted[9] = {0, 0, 0, 0, 0, 1, 0, 0, 0}, empty[9] = {0, 0, 0, 0, 0, 0, 0, 0, 1};
  const int SEL = nav.mixSel;
  for (int t = 0; t < 9; t++) {
    int x = 2 + t * 35;
    if (t == SEL) { rct(x, 24, 33, 186, DARK); frm(x, 24, 33, 186, LIGHT); }
    uint16_t ground = t == SEL ? BG : DARK;
    rct(x + 4, 32, 25, 2, ground);                                        // pan line
    if (!empty[t]) rct(x + 13 + rnd(pans[t] * 9), 30, 4, 6, muted[t] ? DIM : LIGHT);
    const int fy = 44, fh = 136;
    rct(x + 8, fy, 8, fh, ground);                                        // volume level
    if (!empty[t]) { int lh = rnd(fh * vols[t]); rct(x + 8, fy + fh - lh, 8, lh, muted[t] ? DIM : LIGHT); }
    rct(x + 20, fy, 2, fh, ground);                                       // meter beside it
    if (!empty[t] && !muted[t] && mets[t] > 0) { int mh = rnd(fh * mets[t]); rct(x + 20, fy + fh - mh, 2, mh, INK); }
    // Track badge, L7 rules: accent square playing, outlined muted, dark empty.
    int bx = x + 11, by = 188;
    char num[2] = {(char)('1' + t), 0};
    if (empty[t]) txt(bx + 4, by + 4, num, DARK);
    else if (muted[t]) { frm(bx, by, 13, 17, DIM); txt(bx + 4, by + 4, num, DIM); }
    else { rct(bx, by, 13, 17, ACCENT); txt(bx + 4, by + 4, num, BG); }
  }
}

// ---- 4 · Track modulation ---------------------------------------------------
// Four selectable sources per track, two destination slots each.

static void drawMod() {
  char ctx[24];
  snprintf(ctx, sizeof ctx, "TRACK %d  %s", nav.gridT + 1, ENGINE_NAMES[nav.engine]);
  title("MOD", ctx);
  static const char *const SRC_NAMES[4] = {"LFO1", "LFO2", "ENV", "VEL"};
  struct { const char *src, *d1, *a1, *d2, *a2; } rows[] = {
      {"1 LFO1", "CUTOFF", "+42", "PITCH", "-12"},
      {"2 LFO2", "SHAPE", "+20", "-", ""},
      {"3 ENV", "LEVEL", "+64", "RES", "+8"},
      {"4 VEL", "LEVEL", "+50", "-", ""},
  };
  for (int i = 0; i < 4; i++) {
    int y = 26 + i * 17;
    bool sel = nav.modRow == i;
    if (sel) { rct(4, y, 312, 16, DARK); frm(4, y, 312, 16, LIGHT); }
    txt(10, y + 3, rows[i].src, sel ? INK : DIM);
    txt(80, y + 3, rows[i].d1, sel ? LIGHT : DIM);
    txt(146, y + 3, rows[i].a1, sel ? ACCENT : DIM);
    txt(200, y + 3, rows[i].d2, sel ? LIGHT : DIM);
    txt(266, y + 3, rows[i].a2, sel ? ACCENT : DIM);
  }
  txt(6, 98, SRC_NAMES[nav.modSrc], LIGHT);
  struct { const char *n, *v; float a; } p[] = {{"SHAPE", "TRI", -1}, {"RATE", "1/8", 0.4f}, {"SYNC", "ON", -1}, {"DEPTH", "+42", 0.66f}};
  for (int i = 0; i < 4; i++) paramCell(4 + i * 79, 110, 76, 42, p[i].n, p[i].v, p[i].a, nav.modRow == 4 && nav.modCell == i);
  // LFO shape preview: a triangle wave in the accent colour.
  frm(4, 158, 180, 48, DARK);
  rct(8, 181, 172, 2, DARK);
  for (int i = 0; i < 86; i++) {
    float ph = (i % 43) / 43.0f, v = ph < 0.5f ? ph * 2 : 2 - ph * 2;
    rct(8 + i * 2, 198 - rnd(v * 16) * 2, 2, 2, ACCENT);
  }
  txt(196, 164, "RETRIG ON", DIM);
  txt(196, 178, "FADE 0", DIM);
  txt(196, 192, "MODE FREE", DIM);
}

// ---- 5 · Track FX -----------------------------------------------------------
// Two selectable FX slots per track; the types and values are mock.

static void tfxSlot(int y, const char *name, const char *cnt, bool selSel, int selCell,
                    const char *n1, const char *v1, float a1, const char *n2, const char *v2, float a2,
                    const char *n3, const char *v3, float a3, const char *n4, const char *v4, float a4) {
  if (selSel) frm(4, y, 312, 18, LIGHT);
  txt(8, y + 6, "<", DIM);
  txt(24, y + 6, name, INK);
  txt(24 + txtW(name) + 10, y + 6, ">", DIM);
  txt(282, y + 6, cnt, DIM);
  const char *n[4] = {n1, n2, n3, n4}, *v[4] = {v1, v2, v3, v4};
  float a[4] = {a1, a2, a3, a4};
  for (int i = 0; i < 4; i++) paramCell(4 + i * 79, y + 22, 76, 42, n[i], v[i], a[i], selCell == i);
}

static void drawTfx() {
  char ctx[12];
  snprintf(ctx, sizeof ctx, "TRACK %d", nav.gridT + 1);
  title("TFX", ctx);
  txt(6, 26, "SLOT 1", LIGHT);
  tfxSlot(36, "CHORUS", "2/8", nav.tfxRow == 0, nav.tfxRow == 1 ? nav.tfxCol : -1,
          "RATE", "35", 0.35f, "DEPTH", "60", 0.6f, "SPREAD", "80", 0.8f, "MIX", "50", 0.5f);
  txt(6, 110, "SLOT 2", LIGHT);
  tfxSlot(120, "DRIVE", "4/8", nav.tfxRow == 2, nav.tfxRow == 3 ? nav.tfxCol : -1,
          "GAIN", "28", 0.28f, "TONE", "55", 0.55f, "LEVEL", "70", 0.7f, "MIX", "100", 1.0f);
}

// ---- 1 · Master FX slots --------------------------------------------------
// Two selectable FX slots on the mix bus, above the sends; mock types/values.

static void drawMfx() {
  title("MFX", "MASTER");
  txt(6, 26, "SLOT 1", LIGHT);
  tfxSlot(36, "COMP", "3/8", nav.mfxRow == 0, nav.mfxRow == 1 ? nav.mfxCol : -1,
          "THRESH", "-18", 0.6f, "RATIO", "4", 0.5f, "GAIN", "+6", 0.55f, "MIX", "100", 1.0f);
  txt(6, 110, "SLOT 2", LIGHT);
  tfxSlot(120, "TAPE", "6/8", nav.mfxRow == 2, nav.mfxRow == 3 ? nav.mfxCol : -1,
          "DRIVE", "35", 0.35f, "TONE", "60", 0.6f, "WOW", "15", 0.15f, "MIX", "100", 1.0f);
}

// ---- 6 · Track instrument ---------------------------------------------------

static void drawInst() {
  char ctx[12], cnt[8];
  snprintf(ctx, sizeof ctx, "TRACK %d", nav.gridT + 1);
  title("INST", ctx);
  const char *name = ENGINE_NAMES[nav.engine];
  snprintf(cnt, sizeof cnt, "%d/%d", nav.engine + 1, ENGINE_COUNT);
  if (nav.instRow == 0) frm(4, 26, 312, 18, LIGHT);
  txt(8, 32, "<", DIM);
  txt(24, 32, name, INK);
  txt(24 + txtW(name) + 10, 32, ">", DIM);
  txt(282, 32, cnt, DIM);
  rct(4, 46, 312, 2, DARK);
  struct { const char *n, *v; float a; } r1[] = {{"TUNE", "+0", 0.5f}, {"SHAPE", "38", 0.38f}, {"CUTOFF", "86", 0.86f}, {"RES", "22", 0.22f}},
                                         r2[] = {{"ATK", "2", 0.05f}, {"DEC", "340", 0.45f}, {"SUS", "60", 0.6f}, {"REL", "180", 0.3f}};
  for (int i = 0; i < 4; i++) paramCell(4 + i * 79, 54, 76, 44, r1[i].n, r1[i].v, r1[i].a, nav.instRow == 1 && nav.instCol == i);
  for (int i = 0; i < 4; i++) paramCell(4 + i * 79, 102, 76, 44, r2[i].n, r2[i].v, r2[i].a, nav.instRow == 2 && nav.instCol == i);
  // ADSR preview in the accent colour.
  frm(4, 152, 180, 54, DARK);
  const int base = 198, top = 160;
  for (int i = 0; i < 8; i++)  rct(8 + i * 2, base - rnd(i / 8.0f * (base - top)), 2, 2, ACCENT);          // attack
  for (int i = 0; i < 24; i++) rct(24 + i * 2, top + rnd(i / 24.0f * 16), 2, 2, ACCENT);                   // decay
  rct(72, top + 16, 60, 2, ACCENT);                                                                        // sustain
  for (int i = 0; i < 22; i++) rct(132 + i * 2, top + 16 + rnd(i / 22.0f * (base - top - 16)), 2, 2, ACCENT); // release
  txt(196, 160, "VOICES 32", DIM);
  txt(196, 174, "MIDI CH 1", DIM);
  txt(196, 188, "OUT A+B", DIM);
}

// ---- 7 · Clip grid ----------------------------------------------------------

static void drawGrid() {
  // No title tag: the header row is the tracks; the dot column places the screen.
  const int SEL_T = nav.gridT, PLAY_S = 1, CUR_T = nav.gridT, CUR_S = nav.gridS, QUEUED_T = 4, QUEUED_S = 3;
  // 0 none, 1 clip, 2 playing
  static const uint8_t cells[8][9] = {
      {1, 0, 1, 0, 0, 1, 0, 0, 0},
      {2, 0, 2, 1, 0, 2, 0, 2, 0},
      {1, 1, 0, 1, 0, 0, 0, 1, 0},
      {0, 1, 0, 1, 1, 0, 0, 0, 0},
      {1, 0, 1, 0, 1, 0, 1, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, 0},
      {1, 1, 1, 0, 0, 0, 0, 0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0, 0},
  };
  for (int t = 0; t < 9; t++) {
    int x = 16 + t * 33 + 9;
    char num[2] = {(char)('1' + t), 0};
    bool any = false;
    for (int s = 0; s < 8; s++) any |= cells[s][t] != 0;
    if (cells[PLAY_S][t] == 2) { rct(x, 2, 13, 17, ACCENT); txt(x + 4, 6, num, BG); }
    else txt(x + 4, 6, num, any ? DIM : DARK);
    if (t == SEL_T) frm(x - 2, 0, 17, 21, LIGHT);
  }
  rct(0, 22, 320, 2, DARK);
  for (int s = 0; s < 8; s++) {
    int y = 28 + s * 23;
    char num[2] = {(char)('1' + s), 0};
    txt(4, y + 6, num, s == PLAY_S ? ACCENT : DIM);
    for (int t = 0; t < 9; t++) {
      int x = 16 + t * 33;
      uint8_t st = cells[s][t];
      if (st == 0) frm(x + 2, y, 29, 19, DARK);
      else if (st == 2) rct(x + 2, y, 29, 19, ACCENT);
      else rct(x + 2, y, 29, 19, LIGHT);
      if (t == QUEUED_T && s == QUEUED_S) { rct(x + 2, y, 29, 19, BG); frm(x + 2, y, 29, 19, ACCENT); }
      if (t == CUR_T && s == CUR_S) frm(x, y - 2, 33, 23, INK);
    }
  }
}

// ---- 8 · Clip editor --------------------------------------------------------

static bool inLoopRange(int b, int clipBars, int loopStart, int loopLen) {
  for (int k = 0; k < loopLen; k++) if ((loopStart + k) % clipBars == b) return true;
  return false;
}

static void drawEdit() {
  char ctx[8];
  snprintf(ctx, sizeof ctx, "T%d", nav.gridT + 1);
  title("EDIT", ctx);
  // Bar strip: 16 slots (clips are at most 16 bars); this mock clip has 4.
  // The loop brace (accent) selects whole bars and may wrap: here bars 3-4-1.
  // The white frame is the bar the roll below shows.
  const int CLIP_BARS = 4, LOOP_START = 2, LOOP_LEN = 3, PAGE_BAR = 0;
  for (int b = 0; b < 16; b++) {
    int x = 76 + b * 14;
    bool in = inLoopRange(b, CLIP_BARS, LOOP_START, LOOP_LEN);
    if (b < CLIP_BARS) rct(x, 8, 12, 10, in ? LIGHT : DIM);
    else frm(x, 8, 12, 10, DARK);
    if (in) rct(x, 4, inLoopRange((b + 1) % CLIP_BARS, CLIP_BARS, LOOP_START, LOOP_LEN) && b + 1 < CLIP_BARS ? 14 : 12, 2, ACCENT);
  }
  frm(76 + PAGE_BAR * 14 - 2, 6, 16, 14, INK);
  const int ROWS = 13, rowH = 12, top = 24, left = 14, stepW = 19;
  // Pitches top to bottom, chromatic: C4 B3 A#3 A3 G#3 G3 F#3 F3 E3 D#3 D3 C#3 C3.
  static const bool blackRow[ROWS] = {0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0};
  for (int r = 0; r < ROWS; r++) {
    int y = top + r * rowH;
    if (!blackRow[r]) rct(0, y + 1, 10, rowH - 2, DIM);    // white keys in the strip
    else rct(0, y + 1, 6, rowH - 2, DARK);                 // black keys shorter, darker
  }
  rct(0, top - 2, 320, 2, DARK);                            // octave line above C4
  rct(0, top + ROWS * rowH, 320, 2, DARK);
  for (int s = 0; s <= 16; s += 4) rct(left + s * stepW, top, 2, ROWS * rowH, DARK);   // beat lines
  struct { int r, s, l; float v; } notes[] = {
      {12, 0, 2, 0.8f}, {8, 2, 2, 0.6f}, {5, 4, 4, 0.9f}, {0, 8, 2, 0.7f}, {3, 10, 2, 0.5f}, {5, 12, 3, 0.75f}};
  // The note under the cursor is the selected one; on an empty cell the
  // cursor itself shows as a frame.
  int SEL = -1;
  for (int i = 0; i < 6; i++)
    if (notes[i].r == nav.editRow && nav.editStep >= notes[i].s && nav.editStep < notes[i].s + notes[i].l) SEL = i;
  for (int i = 0; i < 6; i++) {
    int x = left + notes[i].s * stepW + 2, y = top + notes[i].r * rowH + 2;
    rct(x, y, notes[i].l * stepW - 4, rowH - 4, i == SEL ? INK : LIGHT);
    if (i == SEL) frm(x - 2, y - 2, notes[i].l * stepW, rowH, ACCENT);
  }
  if (SEL < 0) frm(left + nav.editStep * stepW, top + nav.editRow * rowH, stepW, rowH, INK);
  rct(left + 9 * stepW, top - 2, 2, ROWS * rowH + 4, ACCENT);   // playhead
  int vy = top + ROWS * rowH + 6;                               // velocity lane
  txt(2, vy + 6, "V", DIM);
  for (int i = 0; i < 6; i++) {
    int bh = rnd(20 * notes[i].v);
    rct(left + notes[i].s * stepW + 2, vy + 22 - bh, 8, bh, i == SEL ? INK : DIM);
  }
}

// ---- Entry -----------------------------------------------------------------

void drawScreen(uint8_t ix) {
  board.tft().fillScreen(BG);
  switch (ix) {
    case SCR_MFX:  drawMfx();  break;
    case SCR_FX:   drawFx();   break;
    case SCR_MIX:  drawMix();  break;
    case SCR_MOD:  drawMod();  break;
    case SCR_TFX:  drawTfx();  break;
    case SCR_INST: drawInst(); break;
    case SCR_GRID: drawGrid(); break;
    case SCR_EDIT: drawEdit(); break;
  }
  statusRow(ix);
  // Synchronous: ~40 ms per frame, only on key-driven redraws. (The DMA
  // updateScreenAsync() stalled on this panel: the first frame went out
  // partly and asyncUpdateActive() never cleared, the screen frozen for
  // good. Not investigated further.)
  board.tft().updateScreen();
}
