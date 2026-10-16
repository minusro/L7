// n9_ui.cpp — see n9_ui.h. All sizes are in pixels on L7's 2x2 block grid.
#include "n9_ui.h"

#include <stdio.h>

#include "n9_gm_bank.h"
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
    {'\xA9', {".#####.", "#.###.#", "#.#...#", "#.###.#", ".#####."}},
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

// ---- Shared chrome ----------------------------------------------------------

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

static const char *const FTYPE_NAMES[3] = {"LP", "HP", "BP"};
static const char *const SHAPE_NAMES[LFO_SHAPES] = {"SIN", "NSE", "SQR", "UP", "DWN", "ENV"};
static const char *const DEST_NAMES[LD_COUNT] = {"S1", "S2", "S3", "S4", "CUT", "RES", "ENV",
                                                 "VOL", "PAN", "AX", "AY", "AW", "BX", "BY", "BW", "PIT"};

// The status bar: six cells, GRID's top row and INST's bottom. sel: the
// stood-on cell (-1: the cursor is elsewhere).
static const int ST_X[7] = {0, 44, 108, 152, 192, 236, 320};
static void statusBar(int y, int sel) {
  char buf[16];
  for (int c = 0; c < ST_COUNT; c++) {
    int x = ST_X[c], w = ST_X[c + 1] - x;
    const char *txt1 = "";
    uint16_t ink = DIM;
    switch (c) {
      case ST_TAP:   txt1 = "TAP"; ink = LIGHT; break;
      case ST_TEMPO: snprintf(buf, sizeof buf, "%d", (int)(synth.seqBpm() + 0.5f)); txt1 = buf; ink = INK; break;
      case ST_METRO: txt1 = "MET"; ink = metroOn ? ACCENT : DIM; break;
      case ST_OD:    txt1 = "OD"; ink = overdubOn ? ACCENT : DIM; break;
      case ST_PLAY:  txt1 = ""; break;
      case ST_KEY:   snprintf(buf, sizeof buf, "%s %s", KEY_NAMES[projKey], SCALE_NAMES[projScale]); txt1 = buf; ink = LIGHT; break;
    }
    if (c == ST_PLAY) {                          // the play triangle
      uint16_t pc = synth.seqOn() ? ACCENT : DIM;
      int cx = x + w / 2 - 4, cy = y + 7;
      for (int k = 0; k < 6; k++) rct(cx + k, cy + k / 2, 1, 12 - k, pc);
    } else {
      txt(x + (w - txtW(txt1)) / 2, y + 8, txt1, ink);
    }
    if (sel == c) frm(x + 2, y + 2, w - 4, 22, INK);
  }
}

static int clampP(int p) { p -= 24; return p < 0 ? 0 : p > 72 ? 72 : p; }

// ---- GRID --------------------------------------------------------------------

static void drawGrid() {
  statusBar(0, nav.gridRow == 0 ? nav.statusSel : -1);
  rct(2 + nav.selTrack * 80, 236, 76, 4, ACCENT);  // the selected track, under its column
  for (int t = 0; t < NT; t++)
    for (int sc = 0; sc < NS; sc++) {
      int x = 2 + t * 80, y = 31 + sc * 52;
      const Clip &c = clips[t][sc];
      bool armed = clipPlaying[t] == sc;
      bool playing = armed && synth.seqOn();
      uint16_t f = playing ? ACCENT : armed ? ACCENT_DARK : c.count ? LIGHT : DARK;
      frm(x, y, 76, 48, f);
      // the notes, tiny
      int span = c.bars * 16;
      for (uint8_t i = 0; i < c.count; i++) {
        const ClipNote &n = c.notes[i];
        int ny = y + 42 - clampP(n.pitch) * 38 / 72;
        int e = n.start + clipEffLen(c, i);
        for (int seg = 0; seg < 2; seg++) {      // a note over the loop point wraps
          int a = seg ? 0 : n.start, b = seg ? e - span : (e > span ? span : e);
          if (seg && e <= span) break;
          int nx = x + 3 + a * 70 / span;
          int nw = (b - a) * 70 / span; if (nw < 2) nw = 2;
          rct(nx, ny, nw, 2, n.vel ? LIGHT : DIM);
        }
      }
      for (int b = 1; b < c.bars; b++) {         // bar crossings, on the outline
        int bx = x + 3 + b * 16 * 70 / span;
        rct(bx, y, 2, 2, DIM);
        rct(bx, y + 46, 2, 2, DIM);
      }
      int lp = playing ? (int)synth.clipLocalStep(t) : -1;
      if (lp >= 0) rct(x + 3 + lp * 70 / span, y + 3, 2, 42, ACCENT);   // the playhead
      if (!c.barsFixed) {                        // the auto mark rides under it
        int ax = lp >= 0 ? x + 3 + lp * 70 / span : x + 4;
        if (ax > x + 66) ax = x + 66;
        txt(ax, y + 36, "A", DIM);
      }
      if (nav.gridRow > 0 && nav.gridT == t && nav.gridS == sc) frm(x - 2, y - 2, 80, 52, INK);
    }
}

// ---- NOTE (the piano roll) ---------------------------------------------------

static void dotFrame(int x, int y, int w, int h, uint16_t c) {
  for (int i = 0; i < w; i += 4) { rct(x + i, y, 2, 2, c); rct(x + i, y + h - 2, 2, 2, c); }
  for (int i = 0; i < h; i += 4) { rct(x, y + i, 2, 2, c); rct(x + w - 2, y + i, 2, 2, c); }
}

bool rollRowHas(int p);   // n9_input.cpp: the roll's row space per track

// Row labels: drum tracks name the sound, melodic tracks the note + octave.
static const char *const DM_ROW[12] = {"KICK", "CHAT", "SNR", "OHAT", "CLAP", "LTOM",
                                       "CRSH", "MTOM", "RIDE", "HTOM", "CYM", "RIM"};
static const char *gmdRowName(int p) {
  switch (p) {
    case 27: return "HI Q";  case 29: return "SCR+";  case 30: return "SCR-";
    case 31: return "STIX";  case 32: return "CLK";   case 34: return "MBEL";
    case 35: return "BD 2";  case 36: return "BD 1";  case 37: return "SSTK";
    case 38: return "SNR1";  case 39: return "CLAP";  case 40: return "SNR2";
    case 41: return "TOM1";  case 42: return "CHAT";  case 43: return "TOM2";
    case 44: return "PHAT";  case 45: return "TOM3";  case 46: return "OHAT";
    case 47: return "TOM4";  case 48: return "TOM5";  case 49: return "CRSH";
    case 50: return "TOM6";  case 51: return "RIDE";  case 52: return "CHIN";
    case 53: return "BELL";  case 54: return "TAMB";  case 55: return "SPL";
    case 56: return "COWB";  case 57: return "CRS2";  case 58: return "JNG-";
    case 59: return "RID2";  case 60: return "BNGH";  case 61: return "BNGL";
    case 62: return "CONM";  case 63: return "CONH";  case 64: return "CONL";
    case 65: return "TIMH";  case 66: return "TIML";  case 67: return "AGOH";
    case 68: return "AGOL";  case 69: return "CABA";  case 70: return "MARA";
    case 71: return "WHIS";  case 72: return "WHIL";  case 73: return "GUIS";
    case 74: return "GUIL";  case 75: return "CLAV";  case 76: return "WBLH";
    case 77: return "WBLL";  case 78: return "CUIM";  case 79: return "CUIO";
    case 80: return "TRIM";  case 81: return "TRIO";  case 82: return "SHKR";
    case 83: return "JBEL";  case 85: return "CAST";  case 86: return "SURM";
    case 87: return "SURO";  default: return "?";
  }
}

static void drawNote() {
  const Clip &c = clipAt(nav.gridT, nav.gridS);
  uint8_t eng = trackEngine[nav.selTrack];
  bool drumTrack = eng == E_GMDRUM || eng == E_DM;
  char hdr[36];
  uint8_t stp = synth.seqStepN();
  snprintf(hdr, sizeof hdr, "T%d S%d  BAR %d/%d  %u.%u", nav.selTrack + 1, nav.gridS + 1,
           nav.notePage + 1, c.bars, synth.seqBarN() + 1, stp / 4 + 1);
  txt(4, 2, hdr, DIM);
  // Only in-scale rows show: 13 pitches walking down from the window's top.
  // The roll sits against the right edge; the cursor row's name goes left.
  const int SW = 17, X0 = 318 - 16 * SW, Y0 = 16, RH = 17;
  int rows[13];
  int p = nav.noteTop;
  while (p > 24 && !rollRowHas(p)) p--;
  rows[0] = rollRowHas(p) ? p : -1;
  for (int r = 1; r < 13; r++) {
    int q = p - 1;
    while (q >= 24 && !rollRowHas(q)) q--;
    if (q >= 24 && rollRowHas(q)) { rows[r] = q; p = q; }
    else rows[r] = -1;                           // fewer sounds than rows: blank
  }
  static const bool BLACK[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
  int curRow = -1;
  for (int r = 0; r < 13; r++) {
    if (rows[r] < 0) continue;
    int y = Y0 + r * RH;
    bool darkRow = drumTrack ? (r & 1) : BLACK[rows[r] % 12];
    rct(X0, y, 16 * SW, RH - 1, darkRow ? BG : DARK);
    if (rows[r] == nav.notePitch) curRow = r;
  }
  for (int sgrid = 0; sgrid <= 16; sgrid += 4)   // beat lines
    rct(X0 + sgrid * SW, Y0, 1, 13 * RH, BLACK[0] ? DIM : DIM);
  // the notes of this page, on their rows
  int span = c.bars * 16;
  for (uint8_t i = 0; i < c.count; i++) {
    const ClipNote &n = c.notes[i];
    int r = -1;
    for (int k = 0; k < 13; k++) if (rows[k] == n.pitch) { r = k; break; }
    if (r < 0) continue;
    bool selN = nav.sel[i >> 5] & (1u << (i & 31));
    uint16_t ink = selN ? ACCENT : LIGHT;
    // velocity as the fill's brightness, inside a full-colour block outline
    uint16_t fill = blend565(ink, BG, (uint8_t)(40 + (int)n.vel * 215 / 127));
    int e = n.start + clipEffLen(c, i);
    for (int seg = 0; seg < 2; seg++) {          // a note over the loop point wraps
      if (seg && e <= span) break;
      int a = seg ? 0 : n.start, b = seg ? e - span : (e > span ? span : e);
      int s0 = a - nav.notePage * 16, s1 = b - nav.notePage * 16;
      if (s1 <= 0 || s0 >= 16) continue;
      int xa = X0 + (s0 < 0 ? 0 : s0) * SW, xb = X0 + (s1 > 16 ? 16 : s1) * SW;
      int nx = xa + 1, ny = Y0 + r * RH + 3, nw = xb - xa - 2, nh = RH - 7;
      frm(nx, ny, nw, nh, ink);
      rct(nx + 2, ny + 2, nw - 4, nh - 4, fill);
    }
  }
  // playhead
  if (clipPlaying[nav.gridT] == (int8_t)nav.gridS && synth.seqOn()) {
    int lp = (int)synth.clipLocalStep(nav.gridT);
    if (lp >= 0 && lp / 16 == nav.notePage)
      rct(X0 + (lp % 16) * SW, Y0, 2, 13 * RH, ACCENT);
  }
  // the cursor, its row's name to the left, and the selection box
  if (curRow >= 0) {
    char nm[8];
    if (eng == E_DM) snprintf(nm, sizeof nm, "%s", DM_ROW[nav.notePitch % 12]);
    else if (eng == E_GMDRUM) snprintf(nm, sizeof nm, "%s", gmdRowName(nav.notePitch));
    else snprintf(nm, sizeof nm, "%s%d", KEY_NAMES[nav.notePitch % 12], nav.notePitch / 12 - 1);
    txt(4, Y0 + curRow * RH + 4, nm, INK);
    frm(X0 + nav.noteStep * SW, Y0 + curRow * RH, SW, RH - 1, INK);
  }
  if (nav.boxOpen) {
    int a0 = (int)(nav.boxStep0 < nav.boxStep1 ? nav.boxStep0 : nav.boxStep1) - nav.notePage * 16;
    int a1 = (int)(nav.boxStep0 < nav.boxStep1 ? nav.boxStep1 : nav.boxStep0) - nav.notePage * 16;
    int pTop = nav.boxPitch0 > nav.boxPitch1 ? nav.boxPitch0 : nav.boxPitch1;
    int pBot = nav.boxPitch0 > nav.boxPitch1 ? nav.boxPitch1 : nav.boxPitch0;
    int r0 = 12, r1 = 0;
    for (int k = 0; k < 13; k++) {               // rows the box's pitch range covers
      if (rows[k] <= pTop && rows[k] >= pBot) { if (k < r0) r0 = k; if (k > r1) r1 = k; }
    }
    if (a0 < 0) a0 = 0; if (a1 > 15) a1 = 15;
    if (a0 <= a1 && r0 <= r1)
      dotFrame(X0 + a0 * SW, Y0 + r0 * RH, (a1 - a0 + 1) * SW, (r1 - r0 + 1) * RH - 1, INK);
  }
}

// ---- INST --------------------------------------------------------------------

static void instCellView(uint8_t pg, uint8_t row, uint8_t col, const char *&name, char *val, size_t vn, float &amt) {
  uint8_t t = nav.selTrack;
  const TrackLayer &tl = trackL[t];
  amt = -1;
  if (pg == 0) {
    if (row == 1) {
      const ParamInfo *pi = engineParams(curEngine());
      name = pi[col].name;
      if (name[0] == '-') { val[0] = 0; return; }
      uint8_t v = engParams[t][curEngine()][col];
      snprintf(val, vn, "%d", v); amt = v / 99.0f;
      return;
    }
    static const char *const N[4] = {"CUT", "RES", "TYPE", "ENV"};
    name = N[col];
    uint8_t v[4] = {tl.cut, tl.res, tl.ftype, tl.fenv};
    if (col == 2) { snprintf(val, vn, "%s", FTYPE_NAMES[tl.ftype]); return; }
    snprintf(val, vn, "%d", v[col]); amt = v[col] / 99.0f;
    return;
  }
  if (pg == 1) {
    if (row == 1) {
      static const char *const N[4] = {"ATK", "DEC", "SUS", "REL"};
      name = N[col];
      uint8_t v[4] = {tl.a, tl.d, tl.s, tl.r};
      snprintf(val, vn, "%d", v[col]); amt = v[col] / 99.0f;
      return;
    }
    static const char *const N[4] = {"RATE", "AMT", "SHAPE", "DEST"};
    name = N[col];
    if (col == 2) { snprintf(val, vn, "%s", SHAPE_NAMES[tl.lfoShape]); return; }
    if (col == 3) { snprintf(val, vn, "%s", DEST_NAMES[tl.lfoDest]); return; }
    uint8_t v = col ? tl.lfoAmt : tl.lfoRate;
    snprintf(val, vn, "%d", v); amt = v / 99.0f;
    return;
  }
  uint8_t slot = row - 1;
  const FxSlot &sl = trackFx[t][slot];
  static const char *const N[4] = {"TYPE", "X", "Y", "WET"};
  name = N[col];
  if (col == 0) { snprintf(val, vn, "%s", FX_TYPE_NAMES[sl.type]); return; }
  bool send = fxIsSend(sl.type);
  uint8_t v = (send && col < 3) ? sendFx[fxSendIndex(sl.type)].p[col - 1] : sl.p[col == 3 ? 7 : col - 1];
  snprintf(val, vn, "%d", v); amt = v / 99.0f;
}

static void drawInst() {
  // the synth selector, above the tabs
  const char *name = engineName(curEngine());
  if (nav.instRow == 0) frm(2, 0, 316, 24, INK);
  txt(8, 8, "<", DIM);
  txt(20, 8, name, INK);
  txt(20 + txtW(name) + 6, 8, ">", DIM);
  if (curEngine() == E_GM) {
    char pn[20];
    snprintf(pn, sizeof pn, "%03d %s", gmProgram[nav.selTrack] + 1, GM_NAMES[gmProgram[nav.selTrack] & 127]);
    txt(314 - txtW(pn), 8, pn, DIM);
  } else if (curEngine() == E_DM) {
    static const char *const MACH[4] = {"606", "707", "808", "909"};
    txt(314 - txtW(MACH[drumMachine[nav.selTrack] & 3]), 8, MACH[drumMachine[nav.selTrack] & 3], DIM);
  }
  static const char *const TABS[3] = {"SYN", "MOD", "FX"};
  int tx = 4;
  for (int p = 0; p < 3; p++) {
    if (p == nav.instPage) tx += tagI(tx, 28, TABS[p]) + 8;
    else { txt(tx + 4, 32, TABS[p], DIM); tx += txtW(TABS[p]) + 16; }
  }
  char tnum[8];
  snprintf(tnum, sizeof tnum, "T%d", nav.selTrack + 1);
  txt(314 - txtW(tnum), 32, tnum, ACCENT);
  for (int row = 0; row < 2; row++)
    for (int col = 0; col < 4; col++) {
      const char *nm = "";
      char val[16];
      float amt;
      instCellView(nav.instPage, row + 1, col, nm, val, sizeof val, amt);
      paramCell(4 + col * 79, 52 + row * 56, 76, 50, nm, val, amt,
                nav.instRow == row + 1 && nav.instCol == col);
    }
  statusBar(214, nav.instRow == 3 ? nav.statusSel : -1);
}

void uiMessage(const char *l1, const char *l2) {
  rct(0, 0, 320, 240, BG);
  txt((320 - txtW(l1)) / 2, 100, l1, INK);
  if (l2) txt((320 - txtW(l2)) / 2, 124, l2, DIM);
  board.tft().updateScreen();
}

// ---- Menu (past the left edge; RIGHT off its edge leaves) -------------------

// The N9 mark at any scale (the menu corner at 1, the splash at 2.5): italic
// at a 1:2 step per pixel row whatever the size, the N's middle dead vertical,
// a block of extra air before the 9, the two 80s cuts. 57 x 26 units; shiftN
// and shift9 slide the glyphs apart (the splash's entrance).
static int lu(float units, float s) { return (int)lroundf(units * s); }
static void drawN9Mark(int x, int y, float s, uint16_t c, int shiftN = 0, int shift9 = 0) {
  int h = lu(26, s);
  for (int p = 0; p < h; p++) {
    if ((p >= lu(18, s) && p < lu(20, s)) || (p >= lu(22, s) && p < lu(24, s))) continue;
    int off = (h - 1 - p) / 2;                                  // italic: every diagonal 1:2
    int xn = x + shiftN, x9 = x + shift9;
    rct(xn + off, y + p, lu(6, s), 1, c);                       // N: left stem,
    rct(xn + lu(15, s), y + p, lu(6, s), 1, c);                 //   middle dead vertical,
    rct(xn + off + lu(16, s), y + p, lu(6, s), 1, c);           //   right stem
    rct(x9 + off + lu(38, s), y + p, lu(7, s), 1, c);           // 4: its stem,
    if (p >= lu(13, s) && p < lu(16, s))
      rct(x9 + off + lu(24, s), y + p, lu(21, s), 1, c);        // the bar,
    else if (p < lu(13, s))
      rct(x9 + off + lu(24, s), y + p, lu(7, s), 1, c);         // the left arm
  }
}

// The corner logo, L7's place and size (at (0, 214), a block left of L7's).
static void drawN9Logo(int x, int y, uint16_t c) { drawN9Mark(x, y, 1, c); }

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
  drawN9Logo(0, 214, LIGHT);
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
  drawN9Logo(0, 214, c);
  board.tft().updateScreen();
}

// ---- Splash: static. Just the N4 mark, no animation (2026-10-16). ----------

static constexpr float SPL_SCALE = 2.5f;
static constexpr int SPL_W = 145, SPL_H = 65;
static constexpr int SPL_X = (320 - SPL_W) / 2 + 12, SPL_Y = (240 - SPL_H) / 2;

void splashShow() {
  rct(0, 0, 320, 240, BG);
  drawN9Mark(SPL_X, SPL_Y, SPL_SCALE, LOGO_COLOR);
  board.tft().updateScreen();
}
void splashTick() {}
void splashIntro() {}
void splashHide() {}

// The frame goes out SYNCHRONOUSLY (the DMA path is dead on this panel —
// see the git history; the SPI clock carries the frame rate instead).
static void uiSend() { board.tft().updateScreen(); }

// ---- Entry -----------------------------------------------------------------

void drawScreen(uint8_t ix) {
  board.tft().fillScreen(BG);
  if (nav.menu) {
    drawMenuScr();
  } else {
    switch (ix) {
      case SCR_GRID: drawGrid(); break;
      case SCR_NOTE: drawNote(); break;
      default:       drawInst(); break;
    }
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
  uiSend();   // DMA when the panel cooperates, synchronous after a stall
}
