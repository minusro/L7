// k6_ui.cpp — K6's drawing: the shared primitives/font (lifted from the
// N9-era app, k6_shared_ui.h) and the six screens of the 3x2 map. GRID,
// NOTE, MIX and PROJ draw the live engine state now; SONG is still mock
// (no arranger engine yet) and INST shows the real engine/params read-only.
// Design: docs/design/k6_mockup.html.
#include "k6_ui.h"

#include <stdio.h>

#include "k6_project.h"
#include "k6_song.h"
#include "k6_state.h"
#include "k6_synth.h"

#include "k6_shared_ui.h"

static const char *const PITCH_NAMES[12] = {"C", "C#", "D", "D#", "E", "F",
                                            "F#", "G", "G#", "A", "A#", "B"};
static bool blackKey(uint8_t p) {
  uint8_t k = p % 12;
  return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

// The clip the NOTE screen edits: the grid cursor's (scene column: the
// selected track's cell in that scene).
static Clip &editClip() {
  uint8_t t = nav.gridT < NT ? nav.gridT : nav.selTrack;
  return clipAt(t, nav.gridS);
}
static uint8_t editTrack() { return nav.gridT < NT ? nav.gridT : nav.selTrack; }

// ---- the top bar: map dots, clock, tempo, play, key — on every screen -----
static void topBar(const char *title) {
  for (int r = 0; r < MAP_ROWS; r++)
    for (int c = 0; c < MAP_COLS; c++)
      rct(4 + c * 6, 4 + r * 6, 4, 4, (r == nav.mapR && c == nav.mapC) ? ACCENT : DARK);
  txt(26, 6, title, INK);
  char b[16];
  snprintf(b, sizeof b, "%u.%u.%u", synth.seqBarN() + 1, synth.seqStepN() / 4 + 1,
           synth.seqStepN() % 4 + 1);
  txt(140, 6, b, synth.seqOn() ? LIGHT : DIM);
  snprintf(b, sizeof b, "%d", (int)(synth.seqBpm() + 0.5f));
  txt(186, 6, b, DIM);
  rct(212, 5, 4, 10, synth.seqOn() ? ACCENT : DIM);
  snprintf(b, sizeof b, "%s %s", KEY_NAMES[projKey], SCALE_NAMES[projScale]);
  txt(236, 6, b, DIM);
  rct(0, 20, 320, 2, DARK);
}

// ---- GRID -------------------------------------------------------------------
static void drawGrid() {
  topBar("GRID");
  constexpr int GX = 4, GY = 28, GW = 44, GH = 30, SX = 48, SY = 34;
  for (int t = 0; t < NT; t++)
    for (int s = 0; s < NS; s++) {
      int x = GX + t * SX, y = GY + s * SY;
      Clip &c = clipAt(t, s);
      bool playing = clipPlaying[t] == s, pending = clipPending[t] == s;
      frm(x, y, GW, GH, playing ? ACCENT : pending ? ACCENT_DARK : c.used ? LIGHT : DARK);
      if (c.used && c.count) {                     // the notes, scaled into the cell
        int span = (c.bars ? c.bars : 1) * CLIP_STEPS;
        for (int i = 0; i < c.count; i++) {
          int nx = x + 3 + c.notes[i].start % span * (GW - 8) / span;
          int ny = y + 3 + (11 - c.notes[i].pitch % 12) * (GH - 8) / 12;
          rct(nx, ny, 3, 2, playing ? ACCENT : LIGHT);
        }
      }
      if (playing && synth.seqOn()) {
        int span = (c.bars ? c.bars : 1) * CLIP_STEPS;
        int32_t pos = synth.clipLocalStep(t);
        if (pos >= 0) rct(x + 3 + (int)(pos % span) * (GW - 8) / span, y + 2, 2, GH - 4, ACCENT);
      }
      if (nav.gridT == t && nav.gridS == s) frm(x - 2, y - 2, GW + 4, GH + 4, INK);
    }
  for (int s = 0; s < NS; s++) {                 // the scene column
    int y = GY + s * SY;
    bool live = false;
    for (int t = 0; t < NT; t++) live |= clipPlaying[t] == s;
    frm(296, y, 20, 30, live ? ACCENT : DARK);
    char n[2] = {(char)('1' + s), 0};
    txt(302, y + 10, n, live ? ACCENT : DIM);
    if (nav.gridT == NT && nav.gridS == s) frm(294, y - 2, 24, 34, INK);
  }
  rct(GX + nav.selTrack * SX, 234, GW, 4, ACCENT);
}

// ---- SONG (mock: the arranger engine is still to come) ----------------------
static void drawSong() {
  topBar("SONG");
  txt(8, 28, "ROW  SCENE   REP", DIM);
  static const uint8_t ROWS[5][2] = {{1, 2}, {1, 2}, {2, 4}, {3, 1}, {2, 4}};
  for (int i = 0; i < 5; i++) {
    int y = 44 + i * 24;
    char b[12];
    snprintf(b, sizeof b, "%02d", i + 1);
    txt(8, y, b, DIM);
    snprintf(b, sizeof b, "SCENE %d", ROWS[i][0]);
    txt(60, y, b, LIGHT);
    snprintf(b, sizeof b, "X%d", ROWS[i][1]);
    txt(170, y, b, LIGHT);
    if (nav.songRow == i) frm(4, y - 4, 312, 22, INK);
  }
  txt(8, 220, "MOCK - THE ARRANGER LANDS NEXT", DIM);
}

// ---- MIX --------------------------------------------------------------------
static void drawMix() {
  topBar("MIX");
  for (int t = 0; t <= NT; t++) {
    int x = 8 + t * 44;
    bool master = t == NT;
    char n[2] = {master ? 'M' : (char)('1' + t), 0};
    txt(x + 14, 28, n, master ? INK : DIM);
    frm(x, 42, 36, 150, DARK);
    int v = master ? masterVol : trackVol[t];      // 0..99, 80 = unity
    int h = v * 146 / 99;
    rct(x + 6, 42 + 148 - h, 10, h, master ? INK : LIGHT);
    int pk = master ? synth.masterPeakL() : synth.trackPeak(t);   // 0..255
    int mh = pk * 146 / 255;
    if (mh) rct(x + 20, 42 + 148 - mh, 8, mh, ACCENT_DARK);
    if (!master && trackMute[t]) txt(x + 4, 198, "MUTE", ACCENT);
    if (!master) {                                 // pan, 50 = centre
      rct(x, 214, 36, 2, DIM);
      rct(x + 2 + trackPan[t] * 30 / 99, 212, 4, 6, LIGHT);
    }
    if (nav.mixT == t) frm(x - 3, 39, 42, 180, INK);
  }
}

// ---- NOTE -------------------------------------------------------------------
static void drawNote() {
  topBar("NOTE");
  Clip &c = editClip();
  uint8_t t = editTrack();
  int span = (c.bars ? c.bars : 1) * CLIP_STEPS;
  char b[24];
  snprintf(b, sizeof b, "T%d S%d  BAR %d/%d", t + 1, nav.gridS + 1,
           nav.notePage + 1, c.bars ? c.bars : 1);
  txt(8, 28, b, DIM);
  const int X0 = 48, SW = 16, Y0 = 44, RH = 15;
  for (int r = 0; r < 12; r++) {
    uint8_t p = nav.noteTop - r;
    rct(X0, Y0 + r * RH, 16 * SW, RH - 1, blackKey(p) ? BG : DARK);
  }
  for (int g = 0; g <= 16; g += 4) rct(X0 + g * SW, Y0, 1, 12 * RH, DIM);
  int page0 = nav.notePage * CLIP_STEPS;           // the shown bar's first step
  for (int i = 0; i < c.count; i++) {
    const ClipNote &nt = c.notes[i];
    int row = nav.noteTop - nt.pitch;
    if (row < 0 || row > 11) continue;
    int len = clipEffLen(c, i);
    for (int seg = 0; seg < len; seg++) {          // wrapped tails draw too
      int st = (nt.start + seg) % span - page0;
      if (st < 0 || st >= CLIP_STEPS) continue;
      int x = X0 + st * SW, y = Y0 + row * RH + 3;
      bool cur = nav.notePitch == nt.pitch && (nt.start + seg) % span == page0 + nav.noteStep;
      rct(x + 1, y + 2, SW - 2, RH - 11, cur ? ACCENT_DARK : DARK);
      if (seg == 0) rct(x + 1, y, 2, RH - 7, LIGHT);
      rct(x + 1, y, SW - 1, 1, LIGHT);
      rct(x + 1, y + RH - 8, SW - 1, 1, LIGHT);
    }
  }
  int curRow = nav.noteTop - nav.notePitch;
  snprintf(b, sizeof b, "%s%d", PITCH_NAMES[nav.notePitch % 12], nav.notePitch / 12 - 1);
  txt(8, Y0 + curRow * RH + 4, b, INK);
  frm(X0 + nav.noteStep * SW, Y0 + curRow * RH, SW, RH - 1, INK);
  if (clipPlaying[t] == nav.gridS && synth.seqOn()) {
    int32_t pos = synth.clipLocalStep(t);
    int ph = (int)(pos % span) - page0;
    if (ph >= 0 && ph < CLIP_STEPS) rct(X0 + ph * SW, Y0, 2, 12 * RH, ACCENT);
  }
}

// ---- INST (read-only so far: the editor lands with the full grammar) --------
static void drawInst() {
  topBar("INST");
  uint8_t e = curEngine();
  char b[20];
  if (nav.instRow == 0) frm(4, 26, 126, 22, INK);
  snprintf(b, sizeof b, "< %s >", engineName(e));
  txt(10, 32, b, INK);
  if (e == E_GM) {
    snprintf(b, sizeof b, "PROG %d", gmProgram[nav.selTrack] + 1);
    txt(142, 32, b, LIGHT);
  } else if (e == E_DM) {
    snprintf(b, sizeof b, "KIT %d", drumMachine[nav.selTrack] + 1);
    txt(142, 32, b, LIGHT);
  }
  static const char *const TABS[3] = {"SYN", "MOD", "FX"};
  int tx = 6;
  for (int p = 0; p < 3; p++) {
    txt(tx, 54, TABS[p], p == nav.instPage ? INK : DIM);
    if (p == nav.instPage) rct(tx, 64, txtW(TABS[p]), 2, ACCENT);
    tx += txtW(TABS[p]) + 14;
  }
  const ParamInfo *par = engineParams(e);
  for (int i = 0; i < 8; i++) {
    int row = i / 4, col = i % 4;
    int x = 4 + col * 79, y = 74 + row * 56;
    bool sel = nav.instRow == row + 1 && nav.instCol == col;
    if (sel) { rct(x, y, 76, 50, DARK); frm(x, y, 76, 50, LIGHT); }
    uint8_t v = engParams[nav.selTrack][e][i];
    txt(x + 6, y + 6, par ? par[i].name : "-", sel ? LIGHT : DIM);
    snprintf(b, sizeof b, "%d", v);
    txt(x + 6, y + 20, b, INK);
    rct(x + 6, y + 40, 64, 4, sel ? BG : DARK);
    rct(x + 6, y + 40, v * 64 / 99, 4, sel ? ACCENT : LIGHT);
  }
}

// ---- PROJ -------------------------------------------------------------------
static void drawProj() {
  topBar("PROJ");
  for (int k = 0; k < 12; k++) {
    int x = 16 + (k % 4) * 74, y = 32 + (k / 4) * 44;
    bool cur = k == projCurrent;
    frm(x, y, 64, 36, cur ? ACCENT : projExists[k] ? LIGHT : DIM);
    char n[2] = {(char)('A' + k), 0};
    txt(x + 28, y + 12, n, cur ? ACCENT : projExists[k] ? LIGHT : DIM);
    if (nav.projSel == k) frm(x - 2, y - 2, 68, 40, INK);
  }
  static const char *const ITEMS[3] = {"STORAGE MODE", "KEY / SCALE", "SWITCH TO L7"};
  for (int i = 0; i < 3; i++) {
    int y = 172 + i * 20;
    txt(90, y, ITEMS[i], nav.projSel == 12 + i ? INK : LIGHT);
    if (nav.projSel == 12 + i) frm(84, y - 4, 152, 18, INK);
  }
}

// ---- messages / storage mode -------------------------------------------------
void uiMessage(const char *l1, const char *l2) {
  rct(0, 0, 320, 240, BG);
  txt((320 - txtW(l1)) / 2, 100, l1, INK);
  if (l2) txt((320 - txtW(l2)) / 2, 124, l2, DIM);
  board.tft().updateScreen();
}

void uiStorageDraw(uint8_t state, const char *msg) {
  rct(0, 0, 320, 240, DIM);
  uint16_t c = state == 2 ? ACCENT : LIGHT;
  txt((320 - txtW("STORAGE MODE")) / 2, 88, "STORAGE MODE", c);
  static const char *const TEXT[4] = {"CONNECT A USB CABLE", "WAITING FOR THE COMPUTER",
                                      "CONNECTED", "INSERT AN SD CARD"};
  txt((320 - txtW(TEXT[state & 3])) / 2, 116, TEXT[state & 3], INK);
  const char *foot = msg ? msg : "PRESS ANY KEY TO EXIT";
  txt((320 - txtW(foot)) / 2, 152, foot, BG);
  board.tft().updateScreen();
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
