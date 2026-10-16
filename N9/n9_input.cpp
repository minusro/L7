// n9_input.cpp — N4's keys (the 2026-10-16 reorganisation). Three wrapping
// screens: GRID (status bar + 4x4 clips), NOTE (the piano roll with a
// selection), INST (SYN/MOD/FX pages under the synth selector, status bar
// below). Edge crossings use L7's pending lines (n9_ui draws them); SHIFT +
// arrows switch at once. PLAY stops/starts from 1.1.1, SHIFT+PLAY resets or
// resumes, B+PLAY toggles overdub, B+A+LEFT/RIGHT picks the track,
// B+A+UP/DOWN steps the track's playing clip. SHIFT+B+A still switches apps.
#include "n9_input.h"

#include "M8Board.h"
#include "n9_apps.h"
#include "n9_fx.h"
#include "n9_gm_pcm.h"
#include "n9_project.h"
#include "n9_song.h"
#include "n9_state.h"
#include "n9_synth.h"
#include "n9_usb_drive.h"

using namespace m8;

NavState nav;

static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 50;

enum { A_UP, A_DOWN, A_LEFT, A_RIGHT };
static const uint8_t ARROW_BIT[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- selection helpers (NOTE) ----------------------------------------------

static Clip &editClip() { return clipAt(nav.gridT, nav.gridS); }
// Auto clips open all 16 bars to the editor (playback stays at the filled
// span); fixed clips edit what they play.
static int editBars() { const Clip &c = editClip(); return c.barsFixed ? c.bars : CLIP_MAX_BARS; }
static bool selHas(uint8_t i) { return nav.sel[i >> 5] & (1u << (i & 31)); }
static void selAdd(uint8_t i) { nav.sel[i >> 5] |= 1u << (i & 31); }
static void selClear() { nav.sel[0] = nav.sel[1] = nav.sel[2] = nav.sel[3] = 0; }
static bool selAny() { return nav.sel[0] | nav.sel[1] | nav.sel[2] | nav.sel[3]; }
static int selCount() {
  int n = 0;
  for (int w = 0; w < 4; w++) n += __builtin_popcount(nav.sel[w]);
  return n;
}

// While a note is created or manipulated (A or B held), it sounds on the
// track; it retriggers as the edit changes pitch or velocity and stops when
// the modifier lifts.
static int audNote = -1;
static void audStop() {
  if (audNote >= 0) synth.noteOff((uint8_t)audNote);
  audNote = -1;
}
static void audPlay(uint8_t pitch, uint8_t vel) {
  if (audNote == pitch) return;
  audStop();
  synth.noteOn(pitch, vel ? vel : 100);
  audNote = pitch;
}
static void audRetrig(uint8_t pitch, uint8_t vel) {
  audStop();
  synth.noteOn(pitch, vel ? vel : 100);
  audNote = pitch;
}

// An A tap on a note deletes it (with the rest of the selection); the next
// A tap restores them, as long as only SHIFT or PLAY came in between.
static ClipNote undoNotes[CLIP_MAX_NOTES];
static uint8_t undoCount = 0, undoT = 0, undoS = 0;
static bool restoreArmed = false;
static bool aTapOnNote = false;

static int noteAtCursor() {
  return clipFindAt(editClip(), nav.notePitch, nav.notePage * 16 + nav.noteStep);
}

// The cursor moved by hand: the selection follows the cell (the note under
// it, or nothing).
static void selFollowCursor() {
  selClear();
  int i = noteAtCursor();
  if (i >= 0) selAdd((uint8_t)i);
}

static int scaleStep(int p, int dir);
bool rollRowHas(int p);

// The 13 visible rows are in-scale pitches; the window slides to keep the
// cursor inside.
static int scaleDistance(int hi, int lo) {       // rows from lo up to hi
  int n = 0;
  for (int p = lo + 1; p <= hi; p++) if (rollRowHas(p)) n++;
  return n;
}
static void pitchWindowFollow() {
  if (!rollRowHas(nav.noteTop)) { int p = scaleStep(nav.noteTop, -1); if (!p) p = scaleStep(nav.noteTop, 1); nav.noteTop = (uint8_t)(p ? p : nav.notePitch); }
  if (nav.notePitch > nav.noteTop) nav.noteTop = nav.notePitch;
  while (scaleDistance(nav.noteTop, nav.notePitch) > 12)
    nav.noteTop = (uint8_t)scaleStep(nav.noteTop, -1);
}

// The roll's row space: melodic tracks fold to the scale; drum tracks show
// one row per playable sound (DRUMS: the 12 slots of one octave, GM DRUM:
// the mapped hits), unfolded.
bool rollRowHas(int p) {
  uint8_t e = trackEngine[nav.selTrack];
  if (e == E_DM) return p >= 60 && p <= 71;
  if (e == E_GMDRUM) return p >= 27 && p <= 87 && GM_DRUMMAP[p].smp >= 0;
  return p >= 24 && p <= 96 && scaleHas((uint8_t)p);
}

// The next row from p in direction dir (0 if none).
static int scaleStep(int p, int dir) {
  do p += dir; while (p >= 24 && p <= 96 && !rollRowHas(p));
  return (p < 24 || p > 96 || !rollRowHas(p)) ? 0 : p;
}

// The cursor and window land on valid rows (the track or its engine changed).
static void rowNormalize() {
  if (!rollRowHas(nav.notePitch)) {
    int p = scaleStep(nav.notePitch, -1);
    if (!p) p = scaleStep(nav.notePitch, 1);
    if (!p) p = 60;
    nav.notePitch = (uint8_t)p;
    nav.noteTop = (uint8_t)p;
  }
}

// ---- clip launching (GRID) --------------------------------------------------

// Instant, and every clip rides the global clock (its position is the
// clock modulo its length), so a switch keeps the phase by construction.
static void clipPlayNow(uint8_t t, uint8_t s) {
  clipPlaying[t] = (int8_t)s;
  if (!synth.seqOn()) synth.seqToggle();
}

static void clipStepPlay(uint8_t t, int dir) {   // B+A+UP/DOWN
  int8_t s = clipPlaying[t];
  int to = s < 0 ? (dir < 0 ? NS - 1 : 0) : clamp(s + dir, 0, NS - 1);
  clipPlayNow(t, (uint8_t)to);
}

// ---- the grid's bar-length ladder -------------------------------------------

static const uint8_t BAR_LADDER[6] = {0, 1, 2, 4, 8, 16};   // 0 = Auto
static void clipBarsStep(Clip &c, int dir) {
  int ix = 0;
  for (int i = 0; i < 6; i++) if (BAR_LADDER[i] == c.barsFixed) ix = i;
  ix = clamp(ix + dir, 0, 5);
  c.barsFixed = BAR_LADDER[ix];
  clipRefreshBars(c);
}

// ---- tap tempo ---------------------------------------------------------------

static void tapTempo() {
  static uint32_t taps[4];
  static uint8_t n = 0;
  uint32_t now = millis();
  if (n && now - taps[n - 1] > 2500) n = 0;      // a stale run starts over
  if (n == 4) { taps[0] = taps[1]; taps[1] = taps[2]; taps[2] = taps[3]; n = 3; }
  taps[n++] = now;
  if (n < 2) return;
  float avg = (taps[n - 1] - taps[0]) / (float)(n - 1);
  int bpm = (int)(60000.0f / avg + 0.5f);
  synth.setSeqBpm((float)clamp(bpm, 40, 240));
}

// ---- the status bar (GRID top, INST bottom) ---------------------------------

static bool statusA(uint8_t cell) {
  switch (cell) {
    case ST_TAP:   tapTempo(); return true;
    case ST_METRO: metroOn = !metroOn; return true;
    case ST_OD:    overdubOn = !overdubOn; return true;
    case ST_PLAY:  synth.seqToggle(); return true;
  }
  return false;
}

static bool statusB(uint8_t cell, int arrow) {
  if (cell == ST_TEMPO) {
    int d = arrow == A_LEFT ? -1 : arrow == A_RIGHT ? 1 : arrow == A_UP ? 10 : -10;
    synth.setSeqBpm((float)clamp((int)synth.seqBpm() + d, 40, 240));
    return true;
  }
  if (cell == ST_KEY) {
    if (arrow == A_LEFT || arrow == A_RIGHT) projKey = (projKey + (arrow == A_RIGHT ? 1 : 11)) % 12;
    else projScale = (projScale + (arrow == A_UP ? 1 : SCALE_COUNT - 1)) % SCALE_COUNT;
    nav.notePitch = scaleSnap(nav.notePitch);
    nav.noteTop = scaleSnap(nav.noteTop);
    pitchWindowFollow();
    return true;
  }
  return false;
}

// ---- INST page cells ---------------------------------------------------------

// The selected cell's storage, range and default. page/row/col from nav.
struct CellRef { uint8_t *v; uint8_t max; uint8_t def; bool fxType; uint8_t fxSlot; };
static CellRef instCell() {
  uint8_t t = nav.selTrack, pg = nav.instPage, row = nav.instRow, col = nav.instCol;
  TrackLayer &tl = trackL[t];
  if (pg == 0) {
    if (row == 1) {
      static uint8_t dummy = 0;
      const ParamInfo *pi = engineParams(curEngine());
      if (pi[col].name[0] == '-') { dummy = 0; return {&dummy, 0, 0, false, 0}; }
      return {&engParams[t][curEngine()][col], 99, pi[col].def, false, 0};
    }
    uint8_t *v[4] = {&tl.cut, &tl.res, &tl.ftype, &tl.fenv};
    uint8_t mx[4] = {99, 99, 2, 99}, df[4] = {99, 10, 0, 50};
    return {v[col], mx[col], df[col], false, 0};
  }
  if (pg == 1) {
    if (row == 1) {
      uint8_t *v[4] = {&tl.a, &tl.d, &tl.s, &tl.r};
      uint8_t df[4] = {0, 50, 99, 40};
      return {v[col], 99, df[col], false, 0};
    }
    uint8_t *v[4] = {&tl.lfoRate, &tl.lfoAmt, &tl.lfoShape, &tl.lfoDest};
    uint8_t mx[4] = {99, 99, LFO_SHAPES - 1, LD_COUNT - 1}, df[4] = {40, 0, 0, 0};
    return {v[col], mx[col], df[col], false, 0};
  }
  uint8_t slot = row - 1;                        // FX: row 1 = A, row 2 = B
  FxSlot &sl = trackFx[t][slot];
  if (col == 0) return {&sl.type, 0, FXT_NONE, true, slot};
  bool send = fxIsSend(sl.type);
  uint8_t *x = (send && col < 3) ? &sendFx[fxSendIndex(sl.type)].p[col - 1] : &sl.p[col == 3 ? 7 : col - 1];
  uint8_t df = fxParams(sl.type, false)[col == 3 ? 7 : col - 1].def;
  return {x, 99, df, false, 0};
}

static void instCellStep(int d) {                // B+arrows on a cell
  CellRef c = instCell();
  if (c.fxType) {
    if (d == 1 || d == -1)
      fxSetTrackType(nav.selTrack, c.fxSlot, fxStepTrackType(trackFx[nav.selTrack][c.fxSlot].type, d));
    return;
  }
  if (c.max == 0) return;
  *c.v = (uint8_t)clamp(*c.v + d, 0, c.max);
}

static void instCellReset() {                    // a B double tap
  CellRef c = instCell();
  if (c.fxType) { fxSetTrackType(nav.selTrack, c.fxSlot, FXT_NONE); return; }
  if (c.max) *c.v = c.def;
}

// ---- NOTE editing ------------------------------------------------------------

static void notesForEach(void (*fn)(Clip &, uint8_t));

static void noteCut() {                          // SHIFT+A: cut (or the cursor's note)
  Clip &c = editClip();
  if (!selAny()) selFollowCursor();
  if (!selAny()) return;
  uint8_t base = 255;
  noteClipCount = 0;
  for (uint8_t i = 0; i < c.count; i++)
    if (selHas(i) && c.notes[i].start < base) base = c.notes[i].start;
  for (int i = c.count - 1; i >= 0; i--) {
    if (!selHas((uint8_t)i)) continue;
    if (noteClipCount < CLIP_MAX_NOTES) {
      noteClipboard[noteClipCount] = c.notes[i];
      noteClipboard[noteClipCount].start -= base;
      noteClipCount++;
    }
    c.notes[i] = c.notes[c.count - 1];           // (the swap reorders: selection goes)
    c.count--;
  }
  selClear();
  clipRefreshBars(c);
}

static void notePaste() {
  Clip &c = editClip();
  uint8_t at = nav.notePage * 16 + nav.noteStep;
  selClear();
  for (uint8_t i = 0; i < noteClipCount && c.count < CLIP_MAX_NOTES; i++) {
    int st = at + noteClipboard[i].start;
    if (st >= CLIP_MAX_BARS * 16) continue;
    ClipNote &n = c.notes[c.count];
    n = noteClipboard[i];
    n.start = (uint8_t)st;
    selAdd(c.count);
    c.count++;
  }
  c.used = 1;
  clipRefreshBars(c);
}

static void selMove(int d) {                     // B+LEFT/RIGHT: wraps round the clip
  Clip &c = editClip();
  int span = c.bars * 16;
  for (uint8_t i = 0; i < c.count; i++)
    if (selHas(i)) c.notes[i].start = (uint8_t)((c.notes[i].start + d + span) % span);
  clipRefreshBars(c);
  int pos = (nav.notePage * 16 + nav.noteStep + d + span) % span;
  nav.notePage = (uint8_t)(pos / 16); nav.noteStep = (uint8_t)(pos % 16);
}

static void selTranspose(int dir) {              // B+UP/DOWN: in scale
  Clip &c = editClip();
  for (uint8_t i = 0; i < c.count; i++) {
    if (!selHas(i)) continue;
    if (!scaleStep(c.notes[i].pitch, dir)) return;
  }
  for (uint8_t i = 0; i < c.count; i++)
    if (selHas(i)) c.notes[i].pitch = (uint8_t)scaleStep(c.notes[i].pitch, dir);
  int p = scaleStep(nav.notePitch, dir);
  if (p) { nav.notePitch = (uint8_t)p; pitchWindowFollow(); }
}

static void selLength(int d) {                   // A+LEFT/RIGHT
  Clip &c = editClip();
  for (uint8_t i = 0; i < c.count; i++)
    if (selHas(i))
      c.notes[i].len = (uint8_t)clamp(c.notes[i].len + d, 1, CLIP_MAX_BARS * 16 - c.notes[i].start);
  clipRefreshBars(c);
}

static void selVelocity(int d) {                 // A+UP/DOWN: 17 steps, 0 = muted
  Clip &c = editClip();
  for (uint8_t i = 0; i < c.count; i++) {
    if (!selHas(i)) continue;
    int step = (c.notes[i].vel + 4) / 8;
    step = clamp(step + d, 0, 16);
    c.notes[i].vel = step >= 16 ? 127 : (uint8_t)(step * 8);
  }
}

static void boxFinish() {                        // B released: take the box
  Clip &c = editClip();
  uint8_t s0 = nav.boxStep0 < nav.boxStep1 ? nav.boxStep0 : nav.boxStep1;
  uint8_t s1 = nav.boxStep0 < nav.boxStep1 ? nav.boxStep1 : nav.boxStep0;
  uint8_t p0 = nav.boxPitch0 < nav.boxPitch1 ? nav.boxPitch0 : nav.boxPitch1;
  uint8_t p1 = nav.boxPitch0 < nav.boxPitch1 ? nav.boxPitch1 : nav.boxPitch0;
  selClear();
  int best = -1;
  uint32_t bestD = ~0u;
  int span = c.bars * 16;
  for (uint8_t i = 0; i < c.count; i++) {
    const ClipNote &n = c.notes[i];
    if (n.pitch < p0 || n.pitch > p1) continue;
    int len = clipEffLen(c, i);
    bool body = !(n.start > s1 || n.start + len - 1 < s0);
    bool tail = n.start + len > span && (int)(n.start + len - span - 1) >= (int)s0 && 0 <= (int)s1;
    if (!body && !tail) continue;
    selAdd(i);
    // the endpoint nearest the finishing corner, if inside the box
    for (int e = 0; e < 2; e++) {
      int es = e ? (n.start + len - 1) % span : n.start;
      if (es < s0 || es > s1) continue;
      uint32_t dd = (uint32_t)abs(es - (int)nav.boxStep1) * 2 + (uint32_t)abs((int)n.pitch - (int)nav.boxPitch1);
      if (dd < bestD) { bestD = dd; best = i; nav.noteStep = es % 16; nav.notePage = es / 16; nav.notePitch = n.pitch; }
    }
  }
  if (best >= 0) pitchWindowFollow();
  nav.boxOpen = false;
}

// ---- edges -------------------------------------------------------------------

static void edgeSwitch(uint8_t bit) {
  nav.edgePending = 0;
  if (nav.menu) { nav.menu = false; return; }    // any edge leaves for the GRID
  if (bit == BTN_UP) nav.screen = nav.screen == 0 ? SCR_COUNT - 1 : nav.screen - 1;
  else if (bit == BTN_DOWN) nav.screen = (nav.screen + 1) % SCR_COUNT;
  else if (nav.screen == SCR_GRID) nav.menu = true;
  else if (nav.screen == SCR_INST)
    nav.instPage = (nav.instPage + (bit == BTN_RIGHT ? 1 : 2)) % 3;
}

// ---- the main service --------------------------------------------------------

bool inputService() {
  // A and B trade places (2026-10-16): the physical A acts as the modifier
  // "B" grammar and vice versa. Swapped once here; everything below keeps
  // its logical names.
  uint8_t raw = board.keys();
  uint8_t now = raw & ~(BTN_EDIT | BTN_OPT);
  if (raw & BTN_EDIT) now |= BTN_OPT;
  if (raw & BTN_OPT) now |= BTN_EDIT;
  static uint8_t prev = 0;
  uint8_t pressed = now & ~prev;
  uint8_t released = prev & ~now;
  prev = now;
  uint32_t ms = millis();

  bool shift = now & BTN_SHIFT, opt = now & BTN_OPT, edit = now & BTN_EDIT;
  bool dirtyNow = false;
  if (nav.screen == SCR_NOTE && !nav.menu) { rowNormalize(); pitchWindowFollow(); }

  // Chord bookkeeping: a B or A used in any combo doesn't fire its tap action.
  static bool optUsed = false, editUsed = false;
  static uint32_t optTapAt = 0;                  // the B double tap (INST reset)
  if (pressed & BTN_OPT) optUsed = (now & ~BTN_OPT) != 0;
  if (pressed & BTN_EDIT) editUsed = (now & ~BTN_EDIT) != 0;
  if ((now & BTN_OPT) && (pressed & ~BTN_OPT)) optUsed = true;
  if ((now & BTN_EDIT) && (pressed & ~BTN_EDIT)) editUsed = true;
  if (pressed & ~(BTN_EDIT | BTN_SHIFT | BTN_PLAY)) restoreArmed = false;

  // A pending edge switch: another key cancels, its arrow's release (or a
  // 600 ms hold) fires it.
  static uint8_t edgeDead = 0;
  edgeDead &= now;
  if (nav.edgePending && (pressed & ~nav.edgePending)) { nav.edgePending = 0; dirtyNow = true; }
  if (nav.edgePending && (released & nav.edgePending)) { edgeSwitch(nav.edgePending); dirtyNow = true; }
  if (nav.edgePending && (now & nav.edgePending) && ms - nav.edgeAt >= 600) {
    edgeDead |= nav.edgePending;
    edgeSwitch(nav.edgePending);
    dirtyNow = true;
  }
  static bool edgeLight = false;
  bool lightNow = nav.edgePending && ms - nav.edgeAt < 50;
  if (nav.edgePending && lightNow != edgeLight) dirtyNow = true;
  edgeLight = lightNow;

  if ((pressed & BTN_EDIT) && shift && opt) {    // SHIFT+B+A: switch to L7
    appSwitchToL7();
    dirtyNow = true;
  }

  // PLAY: stop / start from 1.1.1; SHIFT+PLAY: reset / resume; B+PLAY: overdub.
  if (pressed & BTN_PLAY) {
    if (opt) { overdubOn = !overdubOn; optUsed = true; }
    else if (shift) synth.seqShiftPlay();
    else synth.seqToggle();
    dirtyNow = true;
  }

  // The menu's own buttons (cards, storage mode).
  if ((pressed & BTN_EDIT) && !opt && nav.menu) {
    if (shift) {
      if (nav.menuSel < MENU_CARDS) {
        if (projExists[nav.menuSel]) projectCut(nav.menuSel);
        else projectPaste(nav.menuSel);
      }
    } else if (nav.menuSel < MENU_CARDS) {
      if (nav.menuSel == projCurrent) { projectSaveCurrent(); nav.menu = false; }
      else if (projExists[nav.menuSel]) projectRequestLoad(nav.menuSel);
      else projectNew(nav.menuSel);
    } else if (nav.menuSel == MENU_CARDS + MENU_STORAGE) {
      projectSaveCurrent();
      usbDriveMode();
      projectReloadCurrent();
    }
    dirtyNow = true;
  }

  // GRID: plain A on the status bar or a clip cell.
  if ((pressed & BTN_EDIT) && !shift && !opt && !nav.menu) {
    if (nav.screen == SCR_GRID) {
      if (nav.gridRow == 0) { if (statusA(nav.statusSel)) dirtyNow = true; }
      else {
        uint8_t t = nav.gridT, s = nav.gridS;
        if (clipPlaying[t] == (int8_t)s) clipPlaying[t] = -1;   // playing: stop
        else clipPlayNow(t, s);
        dirtyNow = true;
      }
    } else if (nav.screen == SCR_NOTE) {
      // A on an empty cell makes a note; on a note it selects (A+arrows edit).
      Clip &c = editClip();
      int i = noteAtCursor();
      aTapOnNote = i >= 0;
      if (selCount() > 1) {                      // a marquee selection survives the press
        if (i >= 0) audPlay(c.notes[i].pitch, c.notes[i].vel);
      } else {
      selClear();
      if (i >= 0) { selAdd((uint8_t)i); audPlay(c.notes[i].pitch, c.notes[i].vel); }
      else if (c.count < CLIP_MAX_NOTES) {
        ClipNote &n = c.notes[c.count];
        n.pitch = nav.notePitch; n.vel = 100;
        n.start = nav.notePage * 16 + nav.noteStep; n.len = 1;
        c.count++;
        c.used = 1;
        clipRefreshBars(c);
        selAdd(c.count - 1);
        audPlay(n.pitch, n.vel);
      }
      }
      dirtyNow = true;
    } else if (nav.screen == SCR_INST && nav.instRow == 3) {
      if (statusA(nav.statusSel)) dirtyNow = true;
    }
  }

  // SHIFT+A: the clipboards.
  if ((pressed & BTN_EDIT) && shift && !opt && !nav.menu) {
    if (nav.screen == SCR_GRID && nav.gridRow > 0) {
      Clip &c = clipAt(nav.gridT, nav.gridS);
      if (c.count) {
        clipClipboard = c;
        clipClipFull = true;
        if (clipPlaying[nav.gridT] == (int8_t)nav.gridS) clipPlaying[nav.gridT] = -1;
        c = Clip{};
      } else if (clipClipFull) {
        c = clipClipboard;
        c.used = 1;
      }
      dirtyNow = true;
    } else if (nav.screen == SCR_NOTE) {
      if (selAny() || noteAtCursor() >= 0) noteCut();
      else if (noteClipCount) notePaste();
      dirtyNow = true;
    }
  }

  // B pressed in the editor over an empty cell: a selection box opens.
  if ((pressed & BTN_OPT) && !shift && !edit && !nav.menu && nav.screen == SCR_NOTE) {
    int ni = noteAtCursor();
    if (ni >= 0) {
      const Clip &c = editClip();
      audPlay(c.notes[ni].pitch, c.notes[ni].vel);
    }
    if (ni < 0) {
      nav.boxOpen = true;
      nav.boxStep0 = nav.boxStep1 = nav.notePage * 16 + nav.noteStep;
      nav.boxPitch0 = nav.boxPitch1 = nav.notePitch;
      dirtyNow = true;
    }
  }
  if ((released & BTN_OPT) && nav.boxOpen) {     // released: the box selects
    boxFinish();
    dirtyNow = true;
  }

  // A B tap: on the GRID it opens the clip in the editor; on INST a double
  // tap resets the selected cell.
  if ((released & BTN_OPT) && !optUsed && !nav.menu) {
    if (nav.screen == SCR_GRID && nav.gridRow > 0) {
      nav.screen = SCR_NOTE;
      nav.notePage = 0;
      selClear();
      dirtyNow = true;
    } else if (nav.screen == SCR_INST && nav.instRow > 0 && nav.instRow < 3) {
      if (ms - optTapAt < 350) { instCellReset(); dirtyNow = true; }
      optTapAt = ms;
    }
  }

  if (audNote >= 0 && (!(now & (BTN_EDIT | BTN_OPT)) || nav.screen != SCR_NOTE || nav.menu))
    audStop();

  // An A let go cleanly in the editor: delete the selection, or bring the
  // last deletion back (only SHIFT and PLAY may have come between the taps).
  if ((released & BTN_EDIT) && !editUsed && !nav.menu && nav.screen == SCR_NOTE) {
    if (restoreArmed && undoCount) {
      Clip &c = clipAt(undoT, undoS);
      selClear();
      for (uint8_t i = 0; i < undoCount && c.count < CLIP_MAX_NOTES; i++) {
        c.notes[c.count] = undoNotes[i];
        selAdd(c.count);
        c.count++;
      }
      c.used = 1;
      clipRefreshBars(c);
      undoCount = 0;
      restoreArmed = false;
      dirtyNow = true;
    } else if (aTapOnNote) {
      Clip &c = editClip();
      undoCount = 0;
      undoT = nav.gridT; undoS = nav.gridS;
      for (int i = c.count - 1; i >= 0; i--) {
        if (!selHas((uint8_t)i)) continue;
        if (undoCount < CLIP_MAX_NOTES) undoNotes[undoCount++] = c.notes[i];
        c.notes[i] = c.notes[c.count - 1];
        c.count--;
      }
      selClear();
      clipRefreshBars(c);
      restoreArmed = undoCount > 0;
      dirtyNow = true;
    }
    aTapOnNote = false;
  }

  // Arrows, with repeat (SHIFT: fresh presses only).
  static uint32_t pressedAt[4], lastRep[4];
  bool dirty = false;
  for (int i = 0; i < 4; i++) {
    uint8_t bit = ARROW_BIT[i];
    bool fire = false, fresh = false;
    if (pressed & bit) { pressedAt[i] = ms; lastRep[i] = 0; fire = fresh = true; }
    else if ((now & bit) && !shift && ms - pressedAt[i] >= REPEAT_DELAY_MS &&
             (lastRep[i] == 0 || ms - lastRep[i] >= REPEAT_PERIOD_MS)) { lastRep[i] = ms; fire = true; }
    if (!fire) continue;
    int dx = i == A_LEFT ? -1 : i == A_RIGHT ? 1 : 0;
    int dy = i == A_UP ? -1 : i == A_DOWN ? 1 : 0;

    if (opt && edit) {                           // B+A+arrows: track / clip stepping
      if (!fresh) continue;
      if (dx) {
        nav.selTrack = (uint8_t)((nav.selTrack + dx + NT) % NT);
        nav.gridT = nav.selTrack;
        dirty = true;
      }
      else { clipStepPlay(nav.selTrack, -dy); dirty = true; }   // UP = previous
      continue;
    }

    if (shift) {                                 // SHIFT+arrows: screens, at once
      if (!fresh) continue;
      if (nav.menu) { nav.menu = false; dirty = true; continue; }
      if (dy) { edgeSwitch(dy < 0 ? BTN_UP : BTN_DOWN); dirty = true; }
      else if (nav.screen == SCR_NOTE) {
        int n = editBars();
        nav.notePage = (uint8_t)((nav.notePage + dx + n) % n);
        dirty = true;
      } else { edgeSwitch(dx < 0 ? BTN_LEFT : BTN_RIGHT); dirty = true; }
      continue;
    }

    if (nav.menu) {                              // the menu's walk (unchanged)
      if (opt) continue;
      if (dx) {
        int col = nav.menuSel % MENU_COLS;
        if (nav.menuSel < MENU_CARDS && col + dx >= 0 && col + dx < MENU_COLS) {
          nav.menuSel = (uint8_t)(nav.menuSel + dx);
          nav.menuCardCol = nav.menuSel % MENU_COLS;
          dirty = true;
        } else if (!(edgeDead & bit) && !nav.edgePending) {
          nav.edgePending = bit; nav.edgeAt = ms; dirty = true;
        }
      } else {
        if (nav.menuSel < MENU_CARDS) {
          int row = nav.menuSel / MENU_COLS;
          if (row + dy >= 0 && row + dy < MENU_CARDS / MENU_COLS) {
            nav.menuSel = (uint8_t)(nav.menuSel + dy * MENU_COLS);
            nav.menuCardCol = nav.menuSel % MENU_COLS;
            dirty = true;
          } else if (dy > 0) { nav.menuSel = MENU_CARDS; dirty = true; }
          else if (!(edgeDead & bit) && !nav.edgePending) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
        } else if (dy > 0) {
          if (nav.menuSel < MENU_ITEMS - 1) { nav.menuSel++; dirty = true; }
          else if (!(edgeDead & bit) && !nav.edgePending) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
        } else {
          if (nav.menuSel > MENU_CARDS) nav.menuSel--;
          else nav.menuSel = (uint8_t)(MENU_CARDS - MENU_COLS + nav.menuCardCol);
          dirty = true;
        }
      }
      continue;
    }

    if (nav.screen == SCR_GRID) {
      if (opt) {                                 // B+UP/DOWN on a clip: the bar ladder
        if (nav.gridRow > 0 && dy && fresh) { clipBarsStep(clipAt(nav.gridT, nav.gridS), -dy); dirty = true; }
        else if (nav.gridRow == 0 && statusB(nav.statusSel, i)) dirty = true;
        continue;
      }
      if (edit) continue;                        // (A held: nothing on the grid)
      if (dy) {
        int row = nav.gridRow + dy;
        if (row < 0 || row > (int)NS) {          // off the top/bottom: the stack wraps
          if (fresh && !(edgeDead & bit) && nav.edgePending != bit) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
        } else { nav.gridRow = (uint8_t)row; if (row > 0) nav.gridS = (uint8_t)(row - 1); dirty = true; }
      } else {
        if (nav.gridRow == 0) {
          int c = nav.statusSel + dx;
          if (c < 0 || c >= ST_COUNT) {
            if (fresh && !(edgeDead & bit) && nav.edgePending != bit) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
          } else { nav.statusSel = (uint8_t)c; dirty = true; }
        } else {
          int t = nav.gridT + dx;
          if (t < 0 || t >= (int)NT) {           // off the side: the menu
            if (fresh && !(edgeDead & bit) && nav.edgePending != bit) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
          } else { nav.gridT = (uint8_t)t; nav.selTrack = (uint8_t)t; dirty = true; }
        }
      }
      continue;
    }

    if (nav.screen == SCR_NOTE) {
      Clip &c = editClip();
      if (opt && !nav.boxOpen) {                 // the box button + arrows: length / velocity
        if (selAny() || noteAtCursor() >= 0) {
          if (!selAny()) selFollowCursor();
          if (dx) selLength(dx); else selVelocity(-dy);
          if (!dx) {
            int ni = noteAtCursor();
            if (ni >= 0) audRetrig(editClip().notes[ni].pitch, editClip().notes[ni].vel);
          }
          optUsed = true;
          dirty = true;
          continue;
        }
      }
      if (edit && (selAny() || noteAtCursor() >= 0)) {   // the note button + arrows: move / transpose
        if (!selAny()) selFollowCursor();
        if (dx) selMove(dx); else selTranspose(-dy);
        if (!dx) {
          int ni = noteAtCursor();
          if (ni >= 0) audRetrig(editClip().notes[ni].pitch, editClip().notes[ni].vel);
        }
        editUsed = true;
        dirty = true;
        continue;
      }
      // plain (or box-dragging) movement
      if (dx) {
        int pos = nav.notePage * 16 + nav.noteStep;
        int ni = noteAtCursor();
        int espan = editBars() * 16, span = c.bars * 16;
        if (!nav.boxOpen && ni >= 0) {           // skip over the note's extent (wrapping)
          pos = dx > 0 ? (c.notes[ni].start + clipEffLen(c, (uint8_t)ni)) % span
                       : (c.notes[ni].start - 1 + span) % span;
        } else pos = (pos + dx + espan) % espan;
        nav.notePage = (uint8_t)(pos / 16);
        nav.noteStep = (uint8_t)(pos % 16);
      } else {
        int p = scaleStep(nav.notePitch, -dy);
        if (p) { nav.notePitch = (uint8_t)p; pitchWindowFollow(); }
      }
      if (nav.boxOpen) { nav.boxStep1 = nav.notePage * 16 + nav.noteStep; nav.boxPitch1 = nav.notePitch; }
      else selFollowCursor();
      dirty = true;
      continue;
    }

    // INST
    if (opt) {
      if (nav.instRow == 0) {                    // the synth selector
        if (dx && fresh) {
          curEngine() = (uint8_t)((curEngine() + dx + ENGINE_COUNT) % ENGINE_COUNT);
          dirty = true;
        } else if (dy && curEngine() == E_GM) {
          gmProgram[nav.selTrack] = (uint8_t)((gmProgram[nav.selTrack] - dy + 128) & 127);
          dirty = true;
        } else if (dy && curEngine() == E_DM && fresh) {
          drumMachine[nav.selTrack] = (uint8_t)((drumMachine[nav.selTrack] - dy + 4) & 3);
          dirty = true;
        }
      } else if (nav.instRow == 3) {
        if (statusB(nav.statusSel, i)) dirty = true;
      } else {
        instCellStep(dy ? -dy * 10 : dx);        // B+U/D +-10, B+L/R +-1
        dirty = true;
      }
      optUsed = true;
      continue;
    }
    if (edit) continue;
    if (dy) {
      int row = nav.instRow + dy;
      if (row < 0 || row > 3) {
        if (fresh && !(edgeDead & bit) && nav.edgePending != bit) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
      } else { nav.instRow = (uint8_t)row; dirty = true; }
    } else {
      if (nav.instRow == 0) {                    // the selector: sideways = the pages
        if (fresh && !(edgeDead & bit) && nav.edgePending != bit) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
      } else if (nav.instRow == 3) {
        int cc = nav.statusSel + dx;
        if (cc < 0 || cc >= ST_COUNT) {
          if (fresh && !(edgeDead & bit) && nav.edgePending != bit) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
        } else { nav.statusSel = (uint8_t)cc; dirty = true; }
      } else {
        int cc = nav.instCol + dx;
        if (cc < 0 || cc > 3) {
          if (fresh && !(edgeDead & bit) && nav.edgePending != bit) { nav.edgePending = bit; nav.edgeAt = ms; dirty = true; }
        } else { nav.instCol = (uint8_t)cc; dirty = true; }
      }
    }
  }
  return dirty || dirtyNow;
}
