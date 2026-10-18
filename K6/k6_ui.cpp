// k6_ui.cpp — K6's drawing: the shared primitives/font (lifted from the
// N9-era app, k6_shared_ui.h) and the six screens of the 3x2 map. GRID,
// NOTE, MIX and PROJ draw the live engine state now; SONG is still mock
// (no arranger engine yet) and INST shows the real engine/params read-only.
// Design: docs/design/k6_mockup.html.
#include "k6_ui.h"

#include <stdio.h>

#include "k6_fx.h"
#include "k6_gm_bank.h"
#include "k6_gm_cache.h"
#include "k6_project.h"
#include "k6_song.h"
#include "k6_state.h"
#include "k6_synth.h"

#include "k6_shared_ui.h"

bool rollRowHas(int p);   // k6_input.cpp: the roll's row space per track

static const char *const PITCH_NAMES[12] = {"C", "C#", "D", "D#", "E", "F",
                                            "F#", "G", "G#", "A", "A#", "B"};
static bool blackKey(uint8_t p) {
  uint8_t k = p % 12;
  return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

static void dotFrame(int x, int y, int w, int h, uint16_t c) {
  for (int i = 0; i < w; i += 4) { rct(x + i, y, 2, 2, c); rct(x + i, y + h - 2, 2, 2, c); }
  for (int i = 0; i < h; i += 4) { rct(x, y + i, 2, 2, c); rct(x + w - 2, y + i, 2, 2, c); }
}

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
  if (overdubOn) txt(222, 6, "OD", ACCENT);
  snprintf(b, sizeof b, "%s %s", KEY_NAMES[projKey], SCALE_NAMES[projScale]);
  txt(244, 6, b, DIM);
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
    if (!master && trackSolo[t]) txt(x + 4, 198, "SOLO", ACCENT);
    else if (!master && trackMute[t]) txt(x + 4, 198, "MUTE", ACCENT);
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
  const Clip &c = editClip();
  uint8_t t = editTrack();
  uint8_t eng = trackEngine[t];
  bool drumTrack = eng == E_GMDRUM || eng == E_DM;
  char b[24];
  snprintf(b, sizeof b, "T%d S%d  BAR %d/%d", t + 1, nav.gridS + 1,
           nav.notePage + 1, c.bars ? c.bars : 1);
  txt(8, 28, b, DIM);
  // Only in-scale rows show (drum tracks: one row per sound): 12 pitches
  // walking down from the window's top.
  const int X0 = 48, SW = 16, Y0 = 44, RH = 15;
  int rows[12];
  int p = nav.noteTop;
  while (p > 24 && !rollRowHas(p)) p--;
  rows[0] = rollRowHas(p) ? p : -1;
  for (int r = 1; r < 12; r++) {
    int q = p - 1;
    while (q >= 24 && !rollRowHas(q)) q--;
    if (q >= 24 && rollRowHas(q)) { rows[r] = q; p = q; }
    else rows[r] = -1;                           // fewer sounds than rows: blank
  }
  int curRow = -1;
  for (int r = 0; r < 12; r++) {
    if (rows[r] < 0) continue;
    int y = Y0 + r * RH;
    bool darkRow = drumTrack ? (r & 1) : blackKey((uint8_t)rows[r]);
    rct(X0, y, 16 * SW, RH - 1, darkRow ? BG : DARK);
    if (rows[r] == nav.notePitch) curRow = r;
  }
  for (int g = 0; g <= 16; g += 4) rct(X0 + g * SW, Y0, 1, 12 * RH, DIM);
  // the notes of this page, on their rows
  int span = (c.bars ? c.bars : 1) * CLIP_STEPS;
  for (uint8_t i = 0; i < c.count; i++) {
    const ClipNote &n = c.notes[i];
    int r = -1;
    for (int k = 0; k < 12; k++) if (rows[k] == n.pitch) { r = k; break; }
    if (r < 0) continue;
    bool selN = nav.sel[i >> 5] & (1u << (i & 31));
    uint16_t ink = selN ? ACCENT : LIGHT;
    // velocity as the fill's brightness, inside a full-colour block outline
    uint16_t fill = blend565(ink, BG, (uint8_t)(40 + (int)n.vel * 215 / 127));
    int e = n.start + clipEffLen(c, i);
    for (int seg = 0; seg < 2; seg++) {          // a note over the loop point wraps
      if (seg && e <= span) break;
      int a0 = seg ? 0 : n.start, b0 = seg ? e - span : (e > span ? span : e);
      int s0 = a0 - nav.notePage * 16, s1 = b0 - nav.notePage * 16;
      if (s1 <= 0 || s0 >= 16) continue;
      int xa = X0 + (s0 < 0 ? 0 : s0) * SW, xb = X0 + (s1 > 16 ? 16 : s1) * SW;
      int nx = xa + 1, ny = Y0 + r * RH + 3, nw = xb - xa - 2, nh = RH - 7;
      frm(nx, ny, nw, nh, ink);
      rct(nx + 2, ny + 2, nw - 4, nh - 4, fill);
    }
  }
  // playhead
  if (clipPlaying[t] == (int8_t)nav.gridS && synth.seqOn()) {
    int lp = (int)synth.clipLocalStep(t);
    if (lp >= 0 && lp / 16 == nav.notePage)
      rct(X0 + (lp % 16) * SW, Y0, 2, 12 * RH, ACCENT);
  }
  // the cursor, its row's name to the left, and the selection box
  if (curRow >= 0) {
    char nm[8];
    if (eng == E_DM) snprintf(nm, sizeof nm, "%s", DM_ROW[nav.notePitch % 12]);
    else if (eng == E_GMDRUM) snprintf(nm, sizeof nm, "%s", gmdRowName(nav.notePitch));
    else snprintf(nm, sizeof nm, "%s%d", PITCH_NAMES[nav.notePitch % 12], nav.notePitch / 12 - 1);
    txt(6, Y0 + curRow * RH + 4, nm, INK);
    frm(X0 + nav.noteStep * SW, Y0 + curRow * RH, SW, RH - 1, INK);
  }
  if (nav.boxOpen) {
    int a0 = (int)(nav.boxStep0 < nav.boxStep1 ? nav.boxStep0 : nav.boxStep1) - nav.notePage * 16;
    int a1 = (int)(nav.boxStep0 < nav.boxStep1 ? nav.boxStep1 : nav.boxStep0) - nav.notePage * 16;
    int pTop = nav.boxPitch0 > nav.boxPitch1 ? nav.boxPitch0 : nav.boxPitch1;
    int pBot = nav.boxPitch0 > nav.boxPitch1 ? nav.boxPitch1 : nav.boxPitch0;
    int r0 = 11, r1 = 0;
    for (int k = 0; k < 12; k++) {               // rows the box's pitch range covers
      if (rows[k] >= 0 && rows[k] <= pTop && rows[k] >= pBot) { if (k < r0) r0 = k; if (k > r1) r1 = k; }
    }
    if (a0 < 0) a0 = 0;
    if (a1 > 15) a1 = 15;
    if (a0 <= a1 && r0 <= r1)
      dotFrame(X0 + a0 * SW, Y0 + r0 * RH, (a1 - a0 + 1) * SW, (r1 - r0 + 1) * RH - 1, INK);
  }
}

// ---- INST --------------------------------------------------------------------
static const char *const FTYPE_NAMES[3] = {"LP", "HP", "BP"};
static const char *const SHAPE_NAMES[LFO_SHAPES] = {"SIN", "NSE", "SQR", "UP", "DWN", "ENV"};
static const char *const DEST_NAMES[LD_COUNT] = {"S1", "S2", "S3", "S4", "CUT", "RES", "ENV",
                                                 "VOL", "PAN", "AX", "AY", "AW", "BX", "BY", "BW", "PIT"};

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
  topBar("INST");
  uint8_t e = curEngine();
  bool presets = e == E_GM || e == E_DM;
  char b[24];
  // The engine selector (40% wide) and, for engines with banks, the preset box.
  if (nav.instRow == 0) {
    if (nav.instTop == 0 || !presets) frm(4, 26, 126, 22, INK);
    else frm(136, 26, 180, 22, INK);
  }
  snprintf(b, sizeof b, "< %s >", engineName(e));
  txt(10, 32, b, INK);
  if (e == E_GM) {
    uint8_t prog = gmProgram[nav.selTrack] & 127;
    int ix = gmAvailIndexOf(prog);               // banked programs count 1..N
    if (ix >= 0) snprintf(b, sizeof b, "%02d %s", ix + 1, GM_NAMES[prog]);
    else snprintf(b, sizeof b, "-- %s", GM_NAMES[prog]);
    txt(142, 32, b, nav.instTop == 1 ? INK : LIGHT);
  } else if (e == E_DM) {
    static const char *const MACH[4] = {"606", "707", "808", "909"};
    txt(142, 32, MACH[drumMachine[nav.selTrack] & 3], nav.instTop == 1 ? INK : LIGHT);
  }
  static const char *const TABS[3] = {"SYN", "MOD", "FX"};
  int tx = 6;
  for (int p = 0; p < 3; p++) {
    txt(tx, 54, TABS[p], p == nav.instPage ? INK : DIM);
    if (p == nav.instPage) rct(tx, 64, txtW(TABS[p]), 2, ACCENT);
    tx += txtW(TABS[p]) + 14;
  }
  snprintf(b, sizeof b, "T%d", nav.selTrack + 1);
  txt(314 - txtW(b), 54, b, ACCENT);
  for (int row = 0; row < 2; row++)
    for (int col = 0; col < 4; col++) {
      const char *nm = "";
      char val[16];
      float amt;
      instCellView(nav.instPage, row + 1, col, nm, val, sizeof val, amt);
      int x = 4 + col * 79, y = 74 + row * 56;
      bool sel = nav.instRow == row + 1 && nav.instCol == col;
      if (sel) { rct(x, y, 76, 50, DARK); frm(x, y, 76, 50, LIGHT); }
      txt(x + 6, y + 6, nm, sel ? LIGHT : DIM);
      txt(x + 6, y + 20, val, INK);
      if (amt >= 0) {
        rct(x + 6, y + 40, 64, 4, sel ? BG : DARK);
        rct(x + 6, y + 40, (int)(amt * 64), 4, sel ? ACCENT : LIGHT);
      }
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
  char kb[20];
  snprintf(kb, sizeof kb, "KEY  %s %s", KEY_NAMES[projKey], SCALE_NAMES[projScale]);
  const char *items[4] = {"STORAGE MODE", kb, metroOn ? "METRONOME ON" : "METRONOME OFF",
                          "SWITCH TO L7"};
  for (int i = 0; i < 4; i++) {
    int y = 166 + i * 18;
    txt(90, y, items[i], nav.projSel == 12 + i ? INK : LIGHT);
    if (nav.projSel == 12 + i) frm(84, y - 4, 152, 16, INK);
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
