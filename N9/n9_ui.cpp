// n9_ui.cpp — see n9_ui.h. All sizes are in pixels on L7's 2x2 block grid.
#include "n9_ui.h"

#include <stdio.h>

#include "n9_m1_bank.h"
#include "n9_mw_bank.h"
#include "n9_mi.h"
#include "n9_fx.h"
#include "n9_project.h"
#include "n9_song.h"
#include "n9_state.h"
#include "n9_synth.h"

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
    {'#', {".#.#", "####", ".#.#", "####", "#.#."}},
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
// With the status row focused (a SHIFT tap, nav.statusFocus) the stood-on
// cell gets a light frame; B+arrows there edit it (the scene, the tempo).
static void statusRow(uint8_t screenIx) {
  char scene[4], clock[16], bpm[8];
  snprintf(scene, sizeof scene, "%d", gridScene() + 1);
  uint8_t stp = synth.seqStepN();
  snprintf(clock, sizeof clock, "%u.%u.%u", synth.seqBarN() + 1, stp / 4 + 1, stp % 4 + 1);
  snprintf(bpm, sizeof bpm, "%d.0", (int)(synth.seqBpm() + 0.5f));
  for (int k = 0; k < SCR_COUNT; k++) rct(4, 215 + k * 3, 2, 2, k == screenIx ? ACCENT : DARK);
  int w = tagI(12, 218, "SCN");
  txt(12 + w + 4, 222, scene, LIGHT);
  // Play icon + clock, roughly centred as L7's clock cell.
  const int px0 = 128, py0 = 218, hh[5] = {10, 8, 6, 4, 2};
  for (int i = 0; i < 5; i++) rct(px0 + i * 2, py0 + (10 - hh[i]) / 2, 2, hh[i], synth.seqOn() ? ACCENT : DIM);
  txt(px0 + 14, 218, clock, LIGHT);
  int bx = 292 - txtW(bpm) - txtW("BPM") - 12;
  txt(bx, 222, bpm, DIM);
  tagI(292 - txtW("BPM") - 8, 218, "BPM");
  // Action button: accent, left corners cut, a plus for now.
  rct(294, 214, 26, 26, ACCENT);
  rct(294, 214, PX, PX, BG); rct(294, 238, PX, PX, BG);
  rct(304, 222, 6, 2, BG); rct(306, 220, 2, 6, BG);
  if (nav.statusFocus) {
    int cx, cw;
    switch (nav.statusSel) {
      case 0:  cx = 8;   cw = 12 + w + 4 + txtW(scene) + 6 - 8; break;
      case 1:  cx = 122; cw = px0 + 14 + txtW(clock) + 6 - 122; break;
      case 2:  cx = bx - 6; cw = 292 - bx + 8; break;
      default: cx = 294; cw = 26; break;
    }
    frm(cx, 214, cw, 26, LIGHT);
  }
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
  title("MIX", "A = MUTE");
  for (int t = 0; t < 10; t++) {
    bool master = t == 9;
    int x = t * 32;
    bool sel = nav.mixSel == t;
    if (sel) { rct(x, 24, 30, 186, DARK); frm(x, 24, 30, 186, LIGHT); }
    uint16_t ground = sel ? BG : DARK;
    rct(x + 3, 32, 24, 2, ground);                 // pan line (the master has none)
    if (!master) rct(x + 13 + ((trackPan[t] - 50) * 9) / 50, 30, 4, 6, trackMute[t] ? DIM : LIGHT);
    const int fy = 44, fh = 136;
    rct(x + 5, fy, 8, fh, ground);                 // volume
    uint8_t vol = master ? masterVol : trackVol[t];
    int lh = fh * vol / 99;
    rct(x + 5, fy + fh - lh, 8, lh, (!master && trackMute[t]) ? DIM : LIGHT);
    if (master) {                                  // stereo meter pair
      rct(x + 17, fy, 4, fh, ground); rct(x + 23, fy, 4, fh, ground);
      int mhL = fh * synth.masterPeakL() / 255, mhR = fh * synth.masterPeakR() / 255;
      if (mhL) rct(x + 17, fy + fh - mhL, 4, mhL, INK);
      if (mhR) rct(x + 23, fy + fh - mhR, 4, mhR, INK);
    } else {
      rct(x + 19, fy, 4, fh, ground);
      int mh = fh * synth.trackPeak(t) / 255;
      if (mh) rct(x + 19, fy + fh - mh, 4, mh, INK);
    }
    int bx = x + 9, by = 188;                      // badge
    char num[2] = {master ? 'M' : (char)('1' + t), 0};
    if (!master && trackMute[t]) { frm(bx, by, 13, 17, DIM); txt(bx + 4, by + 4, num, DIM); }
    else { rct(bx, by, 13, 17, ACCENT); txt(bx + 4, by + 4, num, BG); }
  }
}

// ---- 4 · Track modulation ---------------------------------------------------
// Four selectable sources per track, two destination slots each.

static void drawMod() {
  char ctx[24];
  snprintf(ctx, sizeof ctx, "TRACK %d  %s", nav.gridT + 1, engineName(curEngine()));
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

static void fxSelectorRow(int y, char label, const FxSlot &sl, bool selFrame, bool marker) {
  if (marker) rct(0, y + 8, 2, 6, ACCENT);         // whose cells are shown below
  if (selFrame) frm(4, y, 312, 22, LIGHT);
  char lb[2] = {label, 0};
  txt(8, y + 6, lb, DIM);
  txt(20, y + 6, "<", DIM);
  txt(32, y + 6, FX_TYPE_NAMES[sl.type], sl.type ? INK : DIM);
  txt(32 + txtW(FX_TYPE_NAMES[sl.type]) + 10, y + 6, ">", DIM);
  if (fxIsSend(sl.type)) txt(272, y + 6, "SEND", DIM);
}

static void fxCells(int y0, const FxSlot &sl, bool asSend, int rowBase, uint8_t row, uint8_t col) {
  const ParamInfo *fp = fxParams(sl.type, asSend);
  for (int i = 0; i < 8; i++) {
    int rr = i / 4, cc = i % 4;
    char val[8];
    bool empty = fp[i].name[0] == '-';
    uint8_t v = (asSend && i < 7) ? sendFx[fxSendIndex(sl.type)].p[i] : sl.p[i];
    if (!empty) snprintf(val, sizeof val, "%d", v);
    paramCell(4 + cc * 79, y0 + rr * 46, 76, 42, fp[i].name, empty ? "" : val,
              empty ? -1.0f : v / 99.0f, (int)row == rowBase + rr && col == cc);
  }
}

static void drawTfx() {
  char ctx[12];
  snprintf(ctx, sizeof ctx, "TRACK %d", nav.gridT + 1);
  title("TFX", ctx);
  fxSelectorRow(26, '1', trackFx[nav.gridT][0], nav.tfxRow == 0, nav.tfxSlot == 0);
  fxSelectorRow(52, '2', trackFx[nav.gridT][1], nav.tfxRow == 1, nav.tfxSlot == 1);
  const FxSlot &sl = trackFx[nav.gridT][nav.tfxSlot];
  fxCells(80, sl, fxIsSend(sl.type), 2, nav.tfxRow, nav.tfxCol);
}

static void drawMfx() {
  title("MFX", "MASTER, THEN VOLUME + CLIP");
  fxSelectorRow(26, 'M', masterFx, nav.mfxRow == 0, false);
  fxCells(54, masterFx, false, 1, nav.mfxRow, nav.mfxCol);
}

// ---- 6 · Track instrument ---------------------------------------------------

static void drawInst() {
  char ctx[12], cnt[8];
  snprintf(ctx, sizeof ctx, "TRACK %d", nav.gridT + 1);
  title("INST", ctx);
  const char *name = engineName(curEngine());
  (void)cnt;
  // The engine selector (left, a quarter of the row) and the preset cell
  // (engines with banks; the rest of the row). B + LEFT/RIGHT steps
  // whichever is selected; the frames reach two blocks below the row.
  if (nav.instRow == 0) frm(nav.instTop == 0 ? 4 : 112, 26, nav.instTop == 0 ? 104 : 204, 22, LIGHT);
  txt(8, 32, "<", DIM);
  txt(20, 32, name, INK);
  txt(20 + txtW(name) + 6, 32, ">", DIM);
  txt(118, 32, "-", DIM);                        // the preset cell: no banked engine right now
  rct(4, 50, 312, 2, DARK);
  const ParamInfo *pi = engineParams(curEngine());
  for (int i = 0; i < 8; i++) {
    int row = i / 4, col = i % 4;
    char val[8];
    bool empty = pi[i].name[0] == '-';
    if (!empty) snprintf(val, sizeof val, "%d", engParams[nav.gridT][curEngine()][i]);
    paramCell(4 + col * 79, 54 + row * 48, 76, 44, pi[i].name, empty ? "" : val,
              empty ? -1.0f : engParams[nav.gridT][curEngine()][i] / 99.0f,
              nav.instRow == row + 1 && nav.instCol == col);
  }
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
  // Cells: playing = accent fill, pending (launch or stop, taken on the next
  // bar line) = dark accent fill, a clip at rest = light fill, empty = a dark
  // frame. Under the eight scene rows, the stop row: a small square a track,
  // lit while the track plays, accented while its stop waits for the bar.
  const int SEL_T = nav.gridT;
  bool playing = synth.seqOn();
  for (int t = 0; t < 9; t++) {
    int x = 16 + t * 33 + 9;
    char num[2] = {(char)('1' + t), 0};
    bool any = false;
    for (int s2 = 0; s2 < 8; s2++) any |= clips[t][s2].used != 0;
    if (playing && clipPlaying[t] >= 0) { rct(x, 2, 13, 17, ACCENT); txt(x + 4, 6, num, BG); }
    else txt(x + 4, 6, num, any ? DIM : DARK);
    if (t == SEL_T) frm(x - 2, 0, 17, 21, LIGHT);
  }
  rct(0, 22, 320, 2, DARK);
  if (nav.scaleShow) {                           // B held: the project's key and scale
    char ks[20];
    snprintf(ks, sizeof ks, "%s %s", KEY_NAMES[projKey], SCALE_NAMES[projScale]);
    tagI((320 - (txtW(ks) + 8)) / 2, 2, ks);
  }
  for (int sc = 0; sc < 8; sc++) {
    int y = 27 + sc * 21;
    char num[2] = {(char)('1' + sc), 0};
    txt(4, y + 4, num, DIM);
    for (int t = 0; t < 9; t++) {
      int x = 16 + t * 33;
      const Clip &c = clips[t][sc];
      bool isPlaying = playing && clipPlaying[t] == sc;
      bool isPending = clipPending[t] == sc;
      if (isPending) rct(x + 2, y, 29, 17, ACCENT_DARK);
      else if (isPlaying) rct(x + 2, y, 29, 17, ACCENT);
      else if (c.used) rct(x + 2, y, 29, 17, LIGHT);
      else frm(x + 2, y, 29, 17, DARK);
      if (c.used && !c.count)                    // an empty clip: hollow middle
        rct(x + 2 + PX, y + PX, 29 - 2 * PX, 17 - 2 * PX, isPending ? ACCENT_DARK : isPlaying ? ACCENT : BG);
      if (t == nav.gridT && sc == nav.gridS) frm(x, y - 2, 33, 21, INK);
    }
  }
  const int sy = 197;                            // the stop row
  for (int t = 0; t < 9; t++) {
    int x = 16 + t * 33;
    bool active = playing && clipPlaying[t] >= 0;
    bool stopping = clipPending[t] == CLIP_STOP;
    frm(x + 2, sy, 29, 12, DARK);
    rct(x + 2 + 12, sy + 3, 6, 6, stopping ? ACCENT : active ? LIGHT : DIM);
    if (t == nav.gridT && nav.gridS == 8) frm(x, sy - 2, 33, 16, INK);
  }
}

// ---- 8 · Clip editor --------------------------------------------------------

static bool inLoopRange(int b, int clipBars, int loopStart, int loopLen) {
  for (int k = 0; k < loopLen; k++) if ((loopStart + k) % clipBars == b) return true;
  return false;
}

static void drawEdit() {
  char ctx[8];
  snprintf(ctx, sizeof ctx, "T%d S%d", nav.gridT + 1, gridScene() + 1);
  title("EDIT", ctx);
  const Clip &c = clipAt(nav.gridT, gridScene());
  // Bar strip: the clip's bars filled, the shown page framed white, the
  // playing bar's slot underlined in accent; the brace spans the loop
  // (the whole clip for now).
  uint16_t localPlay = 0;
  bool playingHere = synth.seqOn();
  if (playingHere) localPlay = (uint16_t)(((uint32_t)synth.seqBarN() * 16 + synth.seqStepN()) % (c.bars * 16));
  for (int b = 0; b < 16; b++) {
    int x = 76 + b * 14;
    if (b < c.bars) {
      rct(x, 8, 12, 10, LIGHT);
      rct(x, 4, b + 1 < c.bars ? 14 : 12, 2, ACCENT);        // the loop brace
      if (playingHere && localPlay / 16 == b) rct(x, 20, 12, 2, ACCENT);
    } else frm(x, 8, 12, 10, DARK);
  }
  frm(76 + nav.editPage * 14 - 2, 6, 16, 14, INK);
  const int ROWS = 13, rowH = 12, top = 24, left = 14, stepW = 19;
  const int topPitch = EDIT_TOP_PITCH + 12 * nav.editOct;
  // Pitches top to bottom, chromatic, the window top an octave multiple of
  // C4. The strip marks the project's scale: in-scale rows wide and dim,
  // the rest short and dark; a dark line above each C row.
  for (int r = 0; r < ROWS; r++) {
    int y = top + r * rowH;
    if (scaleHas((uint8_t)(topPitch - r))) rct(0, y + 1, 10, rowH - 2, DIM);
    else rct(0, y + 1, 6, rowH - 2, DARK);
    if ((topPitch - r) % 12 == 0) rct(0, y - 1, 320, 2, DARK);
  }
  rct(0, top + ROWS * rowH, 320, 2, DARK);
  for (int s = 0; s <= 16; s += 4) rct(left + s * stepW, top, 2, ROWS * rowH, DARK);   // beat lines
  // The note under the cursor is the selected one; on an empty cell the
  // cursor itself shows as a frame. Notes are windowed to the page and the
  // octave view (a note running past the page edge is drawn to the edge).
  int curAbs = nav.editPage * 16 + nav.editStep;
  int SEL = clipFindAt(c, nav.editPitch, (uint8_t)curAbs);
  int pageStart = nav.editPage * 16;
  for (int i = 0; i < c.count; i++) {
    const ClipNote &n = c.notes[i];
    int row = topPitch - n.pitch;
    if (row < 0 || row >= ROWS) continue;
    int s0 = n.start - pageStart, s1 = s0 + n.len;
    if (s1 <= 0 || s0 >= 16) continue;
    int d0 = s0 < 0 ? 0 : s0, d1 = s1 > 16 ? 16 : s1;
    int x = left + d0 * stepW + 2, y = top + row * rowH + 2;
    rct(x, y, (d1 - d0) * stepW - 4, rowH - 4, i == SEL ? INK : LIGHT);
    if (i == SEL) frm(x - 2, y - 2, (d1 - d0) * stepW, rowH, ACCENT);
  }
  int curRow = topPitch - nav.editPitch;
  if (SEL < 0 && curRow >= 0 && curRow < ROWS)
    frm(left + nav.editStep * stepW, top + curRow * rowH, stepW, rowH, INK);
  if (playingHere && localPlay / 16 == nav.editPage)
    rct(left + (localPlay % 16) * stepW, top - 2, 2, ROWS * rowH + 4, ACCENT);   // playhead
  int vy = top + ROWS * rowH + 6;                               // velocity lane
  txt(2, vy + 6, "V", DIM);
  for (int i = 0; i < c.count; i++) {
    int s0 = c.notes[i].start - pageStart;
    if (s0 < 0 || s0 >= 16) continue;
    int bh = rnd(20.0f * c.notes[i].vel / 127.0f);
    rct(left + s0 * stepW + 2, vy + 22 - bh, 8, bh, i == SEL ? INK : DIM);
  }
}

void uiMessage(const char *l1, const char *l2) {
  rct(0, 0, 320, 240, BG);
  txt((320 - txtW(l1)) / 2, 100, l1, INK);
  if (l2) txt((320 - txtW(l2)) / 2, 124, l2, DIM);
  board.tft().updateScreen();
}

// ---- Menu (past the left edge; RIGHT off its edge leaves) -------------------

// The N9 logo, L7's corner-logo place and size (55 x 26 px at (PX, 214)) and
// style (ui_splash.cpp: italic, a pixel right every three rows up, the 9's
// right edge slanting with it, two cuts near the bottom).
static void drawN9Logo(int x, int y, uint16_t c) {
  for (int p = 0; p < 26; p++) {
    if ((p >= 18 && p < 20) || (p >= 22 && p < 24)) continue;   // the cuts
    int off = (25 - p) / 2;                                     // italic: every diagonal 1:2
    rct(x + off, y + p, 6, 1, c);                               // N: left stem,
    rct(x + 15, y + p, 6, 1, c);                                //   middle dead vertical (meets
    rct(x + off + 16, y + p, 6, 1, c);                          //   the stems top and bottom), right stem
    rct(x + off + 36, y + p, 7, 1, c);                          // 9: its stem,
    if (p < 3 || (p >= 10 && p < 13)) rct(x + off + 24, y + p, 12, 1, c);   // bowl top/bottom,
    else if (p < 10) rct(x + off + 24, y + p, 7, 1, c);                     // bowl wall
  }
}

// The battery, top right, L7's (ui_menu.cpp drawBatteryIcon): 11 x 7 blocks,
// outline + fill in sevenths (90 % and up: full); charging: filled whole with
// a lightning bolt cut out. (The frame buffer makes the caching unnecessary.)
static void batteryIcon(uint16_t ink, uint16_t ground) {
  constexpr int STEPS = 7, W = 11, H = 7, X = 320 / PX - 2 - (W + 1), Y = 2;
  int pct = board.batteryPercent(), fill = pct < 0 || pct >= 90 ? STEPS : (pct * STEPS + 50) / 100;
  bool charging = pct >= 0 && board.batteryCharging();
  static const char *const BOLT[5] = {"....#....", "...##....", "..#####..", "....##...", "....#...."};
  for (int r = 0; r < H; r++)
    for (int c = 0; c <= W; c++) {
      bool edge = r == 0 || r == H - 1 || c == 0 || c == W - 1, tip = c == W && r >= 2 && r <= H - 3;
      bool inside = r >= 2 && r <= H - 3 && c >= 2 && c - 2 < fill;
      bool on = c < W ? edge || inside : tip;
      if (charging && !edge && c < W) on = BOLT[r - 1][c - 1] != '#';
      rct((X + c) * PX, (Y + r) * PX, PX, PX, on ? ink : ground);
    }
}

// L7's menu layout (ui_menu.cpp): twelve project cards, three rows of four
// (21 x 15 blocks, the bottom right corner cut diagonally, a block missing
// from the others), and under them the list. Only STORAGE MODE is live for
// now: the cards draw as empty slots and the other items grey; the cursor
// walks everything as L7's does (n9_input.cpp).
struct Shape { int x, y, w, h, cut; };   // in blocks; cut: the corner's size (0: plain)

// Row r (screen block row) of shape s grown by d: its first and last column
// (false: the row isn't in it). round: the corner blocks left out. (L7's.)
static bool rowSpan(const Shape &s, int d, int r, int &c0, int &c1, bool round = false) {
  int y = r - s.y;
  if (y < -d || y > s.h - 1 + d) return false;
  c0 = -d;
  c1 = s.w - 1 + d;
  bool top = y == -d, bottom = y == s.h - 1 + d;
  if (s.cut) {
    c1 = min(c1, s.w + s.h - s.cut - 2 + d - y);
    if (round && top) { c0++; c1--; }
    if (round && bottom) c0++;
  } else if (round && (top || bottom)) {
    c0++;
    c1--;
  }
  if (c1 < c0) return false;
  c0 += s.x;
  c1 += s.x;
  return true;
}

// Fills shape(outer) less shape(inner) in `color`; round: corners cut. (L7's.)
static void fillRing(const Shape &s, int outer, int inner, uint16_t color, bool round = false) {
  for (int r = s.y - outer; r <= s.y + s.h - 1 + outer; r++) {
    int a0, a1, b0, b1;
    if (!rowSpan(s, outer, r, a0, a1, round)) continue;
    if (!rowSpan(s, inner, r, b0, b1)) {
      rct(a0 * PX, r * PX, (a1 - a0 + 1) * PX, PX, color);
      continue;
    }
    if (b0 > a0) rct(a0 * PX, r * PX, (b0 - a0) * PX, PX, color);
    if (a1 > b1) rct((b1 + 1) * PX, r * PX, (a1 - b1) * PX, PX, color);
  }
}

static constexpr int M_CARD_W = 21, M_CARD_H = 15, M_CARD_CUT = 3, M_CARD_GAP = 3, M_CARD_VGAP = 3;
static constexpr int M_CARDS_X = (160 - MENU_COLS * M_CARD_W - (MENU_COLS - 1) * M_CARD_GAP) / 2, M_CARDS_Y = 15;
static constexpr int M_LIST_PITCH = 12, M_LIST_SPACE = 3, M_LIST_Y = 75;   // (L7's: the last item level with the status row's text)
static const char *const M_LABELS[MENU_LIST] = {"EXPORT ABLETON SET", "STORAGE MODE", "SETTINGS", "HELP"};

static Shape menuShape(int k) {
  if (k < MENU_CARDS)
    return {M_CARDS_X + (k % MENU_COLS) * (M_CARD_W + M_CARD_GAP),
            M_CARDS_Y + (k / MENU_COLS) * (M_CARD_H + M_CARD_VGAP), M_CARD_W, M_CARD_H, M_CARD_CUT};
  // A list item: the box fitting the longest label, centred, L7's width.
  int w = txtW("EXPORT ABLETON PROJECT") / PX;
  for (const char *l : M_LABELS) w = max(w, txtW(l) / PX);
  return {(160 - w) / 2, M_LIST_Y + (k - MENU_CARDS) * M_LIST_PITCH, w, 5, 0};
}

static void drawMenuScr() {
  rct(0, 0, 320, 240, DARK);                       // the ground: L7's MENU_BG grey
  for (int k = 0; k < MENU_ITEMS; k++) {
    Shape s = menuShape(k);
    bool sel = k == nav.menuSel;
    if (k < MENU_CARDS) {
      if (sel) {                                   // the frame, a block out, corners cut,
        fillRing(s, 2, 1, LIGHT, true);            // the diagonal a row thicker outwards
        for (int r = s.y + s.h - s.cut; r < s.y + s.h + 2; r++) {
          int c0, c1;
          if (rowSpan(s, 2, r, c0, c1, true)) rct((c1 + 1) * PX, r * PX, PX, PX, LIGHT);
        }
        fillRing(s, 1, 0, DARK);
      }
      // The card, L7's looks: the loaded project filled (its letter black,
      // a play icon while the clock runs), another project a thick outline
      // in its colour, an empty slot a grey outline.
      uint16_t color = PROJECT_COLORS[k];
      bool loaded = k == projCurrent, filled = projExists[k];
      uint16_t letterInk = loaded ? BG : filled ? color : DIM;
      if (loaded) {
        fillRing(s, 0, -s.w, color);
      } else if (filled) {
        fillRing(s, 0, -2, color);
        rct((s.x + s.w - 3) * PX, (s.y + s.h - s.cut - 1) * PX, PX, PX, color);
        rct((s.x + s.w - 4) * PX, (s.y + s.h - s.cut) * PX, PX, PX, color);
      } else {
        fillRing(s, 0, -1, DIM);
      }
      const char letter[2] = {(char)('A' + k), 0};
      bool playIcon = loaded && synth.seqOn();
      int lw = txtW(letter) / PX;
      int x0 = s.x + (s.w - lw) / 2 - (playIcon ? 2 : 0), y0 = s.y + (s.h - 5) / 2;
      txt(x0 * PX, y0 * PX, letter, letterInk);
      if (playIcon) {                              // a small play triangle after the letter
        static const char *const PLAY[5] = {"#..", "##.", "###", "##.", "#.."};
        for (int r = 0; r < 5; r++)
          for (int cc = 0; cc < 3; cc++)
            if (PLAY[r][cc] == '#') rct((x0 + lw + 2 + cc) * PX, (y0 + r) * PX, PX, PX, BG);
      }
    } else {
      if (sel) {
        fillRing(s, M_LIST_SPACE + 1, M_LIST_SPACE, LIGHT, true);
        fillRing(s, M_LIST_SPACE, 0, DARK);
      }
      bool live = k - MENU_CARDS == MENU_STORAGE;  // only STORAGE MODE does anything yet
      txt(s.x * PX, s.y * PX, M_LABELS[k - MENU_CARDS], !live ? DIM : sel ? INK : LIGHT);
    }
  }
  batteryIcon(DIM, DARK);
  drawN9Logo(PX, 214, LIGHT);
}

// Storage mode's screen (n9_usb_drive.cpp): L7's (usb_drive.cpp) — a grey
// ground, STORAGE MODE and the connection's state left-aligned with the
// menu's list, the exit hint (or a message in its place) on the menu's HELP
// line, the logo and battery as the menu's. Sent at once (the drive loop
// calls this only when something changes).
void uiStorageDraw(uint8_t state, const char *msg) {
  const uint16_t SCRN = DIM;                       // L7's storage grey (GREY_44)
  rct(0, 0, 320, 240, SCRN);
  int lx = menuShape(MENU_CARDS).x * PX;
  constexpr int LINE = 10 + 3 * PX, STATE_DY = 32;
  constexpr int TITLE_Y = (240 - (STATE_DY + 56 + LINE + 10)) / 2 / PX * PX + LINE;
  uint16_t c = state == 2 ? ACCENT : LIGHT;        // connected: the accent (for L7's hue cycle)
  txt(lx, TITLE_Y, "STORAGE MODE", c);
  static const char *const TEXT[4] = {"CONNECT A USB CABLE", "WAITING FOR THE COMPUTER", "CONNECTED", "INSERT AN SD CARD"};
  txt(lx, TITLE_Y + STATE_DY, TEXT[state & 3], INK);
  txt(lx, (M_LIST_Y + 3 * M_LIST_PITCH) * PX, msg ? msg : "PRESS ANY KEY TO EXIT", BG);
  batteryIcon(DARK, SCRN);
  drawN9Logo(PX, 214, c);
  board.tft().updateScreen();
}

// ---- Entry -----------------------------------------------------------------

void drawScreen(uint8_t ix) {
  board.tft().fillScreen(BG);
  if (nav.menu) {
    drawMenuScr();
  } else {
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
  }
  if (nav.edgePending) {
    // An arrow held off an edge: its line, the edge's whole length, a block
    // wide, light grey its first 50 ms then grey (L7's, ui.cpp).
    uint16_t c = millis() - nav.edgeAt < 50 ? LIGHT : DIM;
    if (nav.edgePending == m8::BTN_LEFT) rct(0, 0, PX, 240, c);
    else if (nav.edgePending == m8::BTN_RIGHT) rct(320 - PX, 0, PX, 240, c);
    else if (nav.edgePending == m8::BTN_UP) rct(0, 0, 320, PX, c);
    else rct(0, 240 - PX, 320, PX, c);
  }
  // Synchronous: ~40 ms per frame, only on key-driven redraws. (The DMA
  // updateScreenAsync() stalled on this panel: the first frame went out
  // partly and asyncUpdateActive() never cleared, the screen frozen for
  // good. Not investigated further.)
  board.tft().updateScreen();
}
