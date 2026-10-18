// k6_input.cpp — K6's keys, the full grammar (the editor logic is lifted
// from the N9-era app and mapped onto K6's verbs):
//   SHIFT+arrows   move across the 3x2 screen map (wrapping)
//   arrows         move the screen's cursor
//   A (EDIT) tap   act: launch/stop/create a clip, make/select a note, mute
//   A held+arrows  primary value: note length/velocity, clip bar ladder,
//                  engine/preset, INST cells (U/D coarse, L/R fine), volume/pan
//   B (OPTION)     held+arrows: move/transpose notes (selection box on empty);
//                  tap: zoom GRID->NOTE, solo on MIX; double tap: reset a cell
//   SHIFT+A        cut on full / paste on empty (clips, notes, project cards)
//   SHIFT+B        copy without removing (clips, notes)
//   A+B+arrows     global: select track (L/R), step the playing clip (U/D)
//   PLAY           toggle (on release); SHIFT+PLAY resume/reset; B+PLAY
//                  overdub; PLAY held+arrows tempo (U/D 1, L/R 0.1 BPM)
#include "k6_input.h"

#include "M8Board.h"
#include "k6_apps.h"
#include "k6_fx.h"
#include "k6_gm_cache.h"
#include "k6_gm_pcm.h"
#include "k6_project.h"
#include "k6_song.h"
#include "k6_state.h"
#include "k6_synth.h"
#include "k6_ui.h"
#include "k6_usb_drive.h"

extern m8::Board board;

using namespace m8;

NavState nav;

static constexpr uint8_t BTN_A = BTN_EDIT, BTN_B = BTN_OPT;

static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 50;
enum { A_UP, A_DOWN, A_LEFT, A_RIGHT };
static const uint8_t ARROW_BIT[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static Screen curScreen() { return screenAt(nav.mapR, nav.mapC); }

// ---- selection helpers (NOTE) ----------------------------------------------

uint8_t editTrack() { return nav.gridT < NT ? nav.gridT : nav.selTrack; }
static Clip &editClip() { return clipAt(editTrack(), nav.gridS); }
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

// The visible rows are in-scale pitches; the window slides to keep the
// cursor inside.
static int scaleDistance(int hi, int lo) {       // rows from lo up to hi
  int n = 0;
  for (int p = lo + 1; p <= hi; p++) if (rollRowHas(p)) n++;
  return n;
}
static void pitchWindowFollow() {
  if (!rollRowHas(nav.noteTop)) { int p = scaleStep(nav.noteTop, -1); if (!p) p = scaleStep(nav.noteTop, 1); nav.noteTop = (uint8_t)(p ? p : nav.notePitch); }
  if (nav.notePitch > nav.noteTop) nav.noteTop = nav.notePitch;
  while (scaleDistance(nav.noteTop, nav.notePitch) > 11)
    nav.noteTop = (uint8_t)scaleStep(nav.noteTop, -1);
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

// Instant, and every clip rides the global clock (its position is the clock
// modulo its length), so a switch keeps the phase by construction.
static void clipPlayNow(uint8_t t, uint8_t s) {
  songStop();                                    // a hand on the grid leaves song mode
  clipPlaying[t] = (int8_t)s;
  if (!synth.seqOn()) synth.seqToggle();
}

static void clipStepPlay(uint8_t t, int dir) {   // A+B+UP/DOWN
  int8_t s = clipPlaying[t];
  int to = s < 0 ? (dir < 0 ? NS - 1 : 0) : clamp(s + dir, 0, NS - 1);
  clipPlayNow(t, (uint8_t)to);
}

// ---- the grid's bar-length ladder (A held + U/D) -----------------------------

static const uint8_t BAR_LADDER[6] = {0, 1, 2, 4, 8, 16};   // 0 = Auto
static void clipBarsStep(Clip &c, int dir) {
  int ix = 0;
  for (int i = 0; i < 6; i++) if (BAR_LADDER[i] == c.barsFixed) ix = i;
  ix = clamp(ix + dir, 0, 5);
  c.barsFixed = BAR_LADDER[ix];
  clipRefreshBars(c);
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

static void instCellStep(int d) {                // A+arrows on a cell
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

// A+arrows on the selector row: the engine, or the preset (GM walks the
// banked list continuously, U/D jumps eight at a time; DRUMS steps the kit).
static void instTopStep(int dx, int dy) {
  bool onPreset = nav.instTop == 1 && (curEngine() == E_GM || curEngine() == E_DM);
  if (!onPreset) {
    int d = dx ? dx : -dy * 6;
    curEngine() = (uint8_t)((curEngine() + d + 8 * ENGINE_COUNT) % ENGINE_COUNT);
    return;
  }
  if (curEngine() == E_GM) {
    int n = gmAvailCount();
    int d = dx ? dx : -dy * 8;
    if (n) {                                     // only the banked programs show
      int ix = gmAvailIndexOf(gmProgram[nav.selTrack]);
      if (ix < 0) ix = 0; else ix = (ix + d + 8 * n) % n;
      gmProgram[nav.selTrack] = gmAvailAt(ix);
    } else {
      gmProgram[nav.selTrack] = (uint8_t)((gmProgram[nav.selTrack] + d + 256) & 127);
    }
  } else if (dx) {
    drumMachine[nav.selTrack] = (uint8_t)((drumMachine[nav.selTrack] + dx + 4) & 3);
  }
}

// ---- NOTE editing ------------------------------------------------------------

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

static void noteCopy() {                         // SHIFT+B: the clipboard, nothing removed
  Clip &c = editClip();
  if (!selAny()) selFollowCursor();
  if (!selAny()) return;
  uint8_t base = 255;
  noteClipCount = 0;
  for (uint8_t i = 0; i < c.count; i++)
    if (selHas(i) && c.notes[i].start < base) base = c.notes[i].start;
  for (uint8_t i = 0; i < c.count; i++) {
    if (!selHas(i) || noteClipCount >= CLIP_MAX_NOTES) continue;
    noteClipboard[noteClipCount] = c.notes[i];
    noteClipboard[noteClipCount].start -= base;
    noteClipCount++;
  }
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

// ---- GRID actions ------------------------------------------------------------

static void gridAct() {                          // A tap: launch/stop/create, instant
  if (nav.gridT < NT) {
    Clip &c = clipAt(nav.gridT, nav.gridS);
    if (!c.used) { c.used = 1; if (!c.bars) c.bars = 1; return; }
    songStop();
    if (clipPlaying[nav.gridT] == (int8_t)nav.gridS) clipPlaying[nav.gridT] = -1;
    else clipPlayNow(nav.gridT, nav.gridS);
  } else {
    songStop();
    for (int t = 0; t < NT; t++)
      clipPlaying[t] = clipAt(t, nav.gridS).used ? (int8_t)nav.gridS : -1;
    if (!synth.seqOn()) synth.seqToggle();
  }
}

// ---- SONG rows ----------------------------------------------------------------

static SongRow rowClipboard = {0, 1};
static bool rowClipFull = false;

static void songInsertAfter(uint8_t at, SongRow r) {
  if (songLen >= SONG_MAX) return;
  uint8_t to = songLen ? (uint8_t)(at + 1) : 0;
  for (int i = songLen; i > to; i--) songRows[i] = songRows[i - 1];
  songRows[to] = r;
  songLen++;
  if (songOn && songPos >= to) songPos++;        // the sounding row keeps its identity
  nav.songRow = to;
}

static void songDeleteAt(uint8_t at) {
  if (at >= songLen) return;
  for (int i = at; i < songLen - 1; i++) songRows[i] = songRows[i + 1];
  songLen--;
  if (songOn) {
    if (!songLen) songStop();
    else if (songPos > at) songPos--;
    else if (songPos == at && songPos >= songLen) songPos = 0;
  }
  if (nav.songRow >= songLen && nav.songRow) nav.songRow = (uint8_t)(songLen ? songLen - 1 : 0);
}

static void songMoveRow(uint8_t at, int dir) {   // B+U/D: reorder
  int to = at + dir;
  if (at >= songLen || to < 0 || to >= (int)songLen) return;
  SongRow tmp = songRows[at];
  songRows[at] = songRows[to];
  songRows[to] = tmp;
  if (songOn) {
    if (songPos == at) songPos = (uint8_t)to;
    else if (songPos == (uint8_t)to) songPos = at;
  }
  nav.songRow = (uint8_t)to;
}

static void gridCutPaste() {                     // SHIFT+A: L7's card grammar
  if (nav.gridT >= NT) return;
  Clip &c = clipAt(nav.gridT, nav.gridS);
  if (c.count || c.used) {
    clipClipboard = c;
    clipClipFull = true;
    clipClipPlaying = clipPlaying[nav.gridT] == (int8_t)nav.gridS;
    if (clipClipPlaying) clipPlaying[nav.gridT] = -1;
    c = Clip{};
  } else if (clipClipFull) {
    c = clipClipboard;
    c.used = 1;
    if (clipClipPlaying) clipPlayNow(nav.gridT, nav.gridS);   // it was playing: on it goes
  }
}

static void gridCopy() {                         // SHIFT+B
  if (nav.gridT >= NT) return;
  Clip &c = clipAt(nav.gridT, nav.gridS);
  if (!c.used) return;
  clipClipboard = c;
  clipClipFull = true;
  clipClipPlaying = false;                       // a copy pastes silent
}

// ---- PROJ actions ------------------------------------------------------------

static void projAct() {                          // A tap
  if (nav.projSel < 12) {
    if (nav.projSel == projCurrent) projectSaveCurrent();
    else if (projExists[nav.projSel]) projectRequestLoad(nav.projSel);
    else projectNew(nav.projSel);
  } else if (nav.projSel == 12) {
    projectSaveCurrent();
    usbDriveMode();
    projectReloadCurrent();                      // the computer may have changed the card
  } else if (nav.projSel == 13) {
    projScale = (uint8_t)((projScale + 1) % SCALE_COUNT);
    nav.notePitch = scaleSnap(nav.notePitch);
    nav.noteTop = scaleSnap(nav.noteTop);
  } else if (nav.projSel == 14) {
    metroOn = !metroOn;
  } else {
    appSwitchToL7();                             // returns only on failure
  }
}

static void projKeyStep(int dx, int dy) {        // A held + arrows on KEY/SCALE
  if (dx) projKey = (uint8_t)((projKey + (dx > 0 ? 1 : 11)) % 12);
  else projScale = (uint8_t)((projScale + (dy < 0 ? 1 : SCALE_COUNT - 1)) % SCALE_COUNT);
  nav.notePitch = scaleSnap(nav.notePitch);
  nav.noteTop = scaleSnap(nav.noteTop);
  pitchWindowFollow();
}

// ---- the main service --------------------------------------------------------

bool inputService() {
  uint8_t now = board.keys();
  static uint8_t prev = 0;
  uint8_t pressed = now & ~prev;
  uint8_t released = prev & ~now;
  prev = now;
  uint32_t ms = millis();

  bool shift = now & BTN_SHIFT, a = now & BTN_A, b = now & BTN_B, play = now & BTN_PLAY;
  Screen scr = curScreen();
  bool dirtyNow = false;
  if (scr == SCR_NOTE) { rowNormalize(); pitchWindowFollow(); }

  // Chord bookkeeping: an A or B used in any combo doesn't fire its tap action.
  static bool aUsed = false, bUsed = false;
  static uint32_t bTapAt = 0;                    // the B double tap (INST reset)
  if (pressed & BTN_A) aUsed = (now & ~BTN_A) != 0;
  if (pressed & BTN_B) bUsed = (now & ~BTN_B) != 0;
  if ((now & BTN_A) && (pressed & ~BTN_A)) aUsed = true;
  if ((now & BTN_B) && (pressed & ~BTN_B)) bUsed = true;
  if (pressed & ~(BTN_A | BTN_SHIFT | BTN_PLAY)) restoreArmed = false;

  // PLAY: toggle on a clean release (the held+arrows tempo gesture keeps the
  // press); SHIFT+PLAY resume/reset and B+PLAY overdub act at once.
  static bool playUsed = false;
  if (pressed & BTN_PLAY) {
    playUsed = false;
    if (b) { overdubOn = !overdubOn; playUsed = true; bUsed = true; dirtyNow = true; }
    else if (shift) { synth.seqShiftPlay(); playUsed = true; dirtyNow = true; }
  }
  if ((released & BTN_PLAY) && !playUsed) {
    // PLAY on SONG plays the arrangement from the top; elsewhere the grid.
    if (scr == SCR_SONG && !synth.seqOn() && songLen) songLaunch(0);
    else synth.seqToggle();
    dirtyNow = true;
  }

  // A tap (press side): act on the cursor's object.
  if ((pressed & BTN_A) && !shift && !b && !play) {
    switch (scr) {
      case SCR_GRID: gridAct(); dirtyNow = true; break;
      case SCR_NOTE: {
        // A on an empty cell makes a note; on a note it selects (A+arrows edit).
        Clip &c = editClip();
        int i = noteAtCursor();
        aTapOnNote = i >= 0;
        if (selCount() > 1) {                    // a marquee selection survives the press
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
        break;
      }
      case SCR_MIX:
        if (nav.mixT < NT) { trackMute[nav.mixT] = !trackMute[nav.mixT]; dirtyNow = true; }
        break;
      case SCR_SONG:                               // A: a new row after the cursor
        songInsertAfter(nav.songRow, songLen ? songRows[nav.songRow] : SongRow{0, 1});
        dirtyNow = true;
        break;
      case SCR_PROJ: projAct(); dirtyNow = true; break;
      default: break;
    }
  }

  // SHIFT+A: cut on full / paste on empty.
  if ((pressed & BTN_A) && shift && !b) {
    if (scr == SCR_GRID) { gridCutPaste(); dirtyNow = true; }
    else if (scr == SCR_NOTE) {
      if (selAny() || noteAtCursor() >= 0) noteCut();
      else if (noteClipCount) notePaste();
      dirtyNow = true;
    } else if (scr == SCR_SONG && songLen) {       // cut the cursor's row
      rowClipboard = songRows[nav.songRow];
      rowClipFull = true;
      songDeleteAt(nav.songRow);
      dirtyNow = true;
    } else if (scr == SCR_PROJ && nav.projSel < 12) {
      if (projExists[nav.projSel]) projectCut(nav.projSel);
      else projectPaste(nav.projSel);
      dirtyNow = true;
    }
    aUsed = true;
  }

  // SHIFT+B: copy without removing (SONG: insert the cut/copied row back).
  if ((pressed & BTN_B) && shift && !a) {
    if (scr == SCR_GRID) { gridCopy(); dirtyNow = true; }
    else if (scr == SCR_NOTE) { noteCopy(); dirtyNow = true; }
    else if (scr == SCR_SONG && rowClipFull) { songInsertAfter(nav.songRow, rowClipboard); dirtyNow = true; }
    bUsed = true;
  }

  // B pressed in the editor: audition a note, or open a selection box.
  if ((pressed & BTN_B) && !shift && !a && scr == SCR_NOTE) {
    int ni = noteAtCursor();
    if (ni >= 0) {
      const Clip &c = editClip();
      audPlay(c.notes[ni].pitch, c.notes[ni].vel);
    } else {
      nav.boxOpen = true;
      nav.boxStep0 = nav.boxStep1 = (uint8_t)(nav.notePage * 16 + nav.noteStep);
      nav.boxPitch0 = nav.boxPitch1 = nav.notePitch;
      dirtyNow = true;
    }
  }
  if ((released & BTN_B) && nav.boxOpen) {       // released: the box selects
    boxFinish();
    dirtyNow = true;
  }

  // A clean B tap: GRID zooms into the clip's roll; MIX solos the strip;
  // INST double-taps reset the selected cell.
  if ((released & BTN_B) && !bUsed) {
    if (scr == SCR_GRID) {
      nav.mapR = 1; nav.mapC = 1;                // NOTE sits below GRID
      nav.notePage = 0;
      selClear();
      rowNormalize();
      pitchWindowFollow();
      dirtyNow = true;
    } else if (scr == SCR_MIX) {
      if (nav.mixT < NT) { trackSolo[nav.mixT] = !trackSolo[nav.mixT]; dirtyNow = true; }
    } else if (scr == SCR_SONG) {
      if (songLen) { songLaunch(nav.songRow); dirtyNow = true; }   // jump the transport here
    } else if (scr == SCR_INST && nav.instRow > 0) {
      if (ms - bTapAt < 350) { instCellReset(); dirtyNow = true; }
      bTapAt = ms;
    }
  }

  if (audNote >= 0 && (!(now & (BTN_A | BTN_B)) || scr != SCR_NOTE)) audStop();

  // An A let go cleanly in the editor: delete the selection, or bring the
  // last deletion back (only SHIFT and PLAY may have come between the taps).
  if ((released & BTN_A) && !aUsed && scr == SCR_NOTE) {
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
      undoT = editTrack(); undoS = nav.gridS;
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

  // Arrows, with repeat (SHIFT and the A+B chord: fresh presses only).
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

    if (play) {                                  // PLAY held: tempo anywhere
      playUsed = true;
      float bpm = synth.seqBpm() - dy * 1.0f + dx * 0.1f;
      synth.setSeqBpm(bpm < 40 ? 40 : bpm > 300 ? 300 : bpm);
      dirty = true;
      continue;
    }

    if (a && b) {                                // A+B+arrows: track / clip stepping
      if (!fresh) continue;
      aUsed = bUsed = true;
      if (dx) {
        nav.selTrack = (uint8_t)((nav.selTrack + dx + NT) % NT);
        if (nav.gridT < NT) nav.gridT = nav.selTrack;
        if (nav.mixT < NT) nav.mixT = nav.selTrack;
      } else clipStepPlay(nav.selTrack, -dy);    // UP = previous
      dirty = true;
      continue;
    }

    if (shift) {                                 // the screen map, wrapping
      if (!fresh) continue;
      nav.mapC = (uint8_t)((nav.mapC + dx + MAP_COLS) % MAP_COLS);
      nav.mapR = (uint8_t)((nav.mapR + dy + MAP_ROWS) % MAP_ROWS);
      if (screenAt(nav.mapR, nav.mapC) == SCR_NOTE) { selClear(); rowNormalize(); pitchWindowFollow(); }
      dirty = true;
      continue;
    }

    switch (scr) {
      case SCR_GRID:
        if (a) {                                 // A held + U/D: the bar ladder
          if (dy && fresh && nav.gridT < NT) { clipBarsStep(clipAt(nav.gridT, nav.gridS), -dy); dirty = true; }
          aUsed = true;
          break;
        }
        if (b) break;
        nav.gridT = (uint8_t)clamp(nav.gridT + dx, 0, NT);      // NT = the scene column
        nav.gridS = (uint8_t)clamp(nav.gridS + dy, 0, NS - 1);
        if (nav.gridT < NT) nav.selTrack = nav.gridT;
        dirty = true;
        break;

      case SCR_NOTE: {
        Clip &c = editClip();
        if (b && !nav.boxOpen) {                 // B + arrows: move / transpose
          if (selAny() || noteAtCursor() >= 0) {
            if (!selAny()) selFollowCursor();
            if (dx) selMove(dx); else selTranspose(-dy);
            if (!dx) {
              int ni = noteAtCursor();
              if (ni >= 0) audRetrig(editClip().notes[ni].pitch, editClip().notes[ni].vel);
            }
            bUsed = true;
            dirty = true;
            break;
          }
        }
        if (a && (selAny() || noteAtCursor() >= 0)) {   // A + arrows: length / velocity
          if (!selAny()) selFollowCursor();
          if (dx) selLength(dx); else selVelocity(-dy);
          if (!dx) {
            int ni = noteAtCursor();
            if (ni >= 0) audRetrig(editClip().notes[ni].pitch, editClip().notes[ni].vel);
          }
          aUsed = true;
          dirty = true;
          break;
        }
        // plain (or box-dragging) movement
        if (dx) {
          int pos = nav.notePage * 16 + nav.noteStep;
          int ni = noteAtCursor();
          int espan = editBars() * 16, span = c.bars * 16;
          if (!nav.boxOpen && ni >= 0) {         // skip over the note's extent (wrapping)
            pos = dx > 0 ? (c.notes[ni].start + clipEffLen(c, (uint8_t)ni)) % span
                         : (c.notes[ni].start - 1 + span) % span;
          } else pos = (pos + dx + espan) % espan;
          nav.notePage = (uint8_t)(pos / 16);
          nav.noteStep = (uint8_t)(pos % 16);
        } else {
          int p = scaleStep(nav.notePitch, -dy);
          if (p) { nav.notePitch = (uint8_t)p; pitchWindowFollow(); }
        }
        if (nav.boxOpen) { nav.boxStep1 = (uint8_t)(nav.notePage * 16 + nav.noteStep); nav.boxPitch1 = nav.notePitch; }
        else selFollowCursor();
        dirty = true;
        break;
      }

      case SCR_MIX:
        if (a) {                                 // A held: volume (U/D), pan (L/R)
          if (dy) {
            uint8_t &v = nav.mixT < NT ? trackVol[nav.mixT] : masterVol;
            v = (uint8_t)clamp(v - dy * 2, 0, 99);
          } else if (nav.mixT < NT) {
            trackPan[nav.mixT] = (uint8_t)clamp(trackPan[nav.mixT] + dx * 2, 0, 99);
          }
          aUsed = true;
          dirty = true;
          break;
        }
        if (b) break;
        nav.mixT = (uint8_t)clamp(nav.mixT + dx, 0, NT);
        if (nav.mixT < NT) nav.selTrack = nav.mixT;
        dirty = true;
        break;

      case SCR_SONG:
        if (!songLen) break;
        if (a) {                                 // A held: scene (U/D) / repeats (L/R)
          SongRow &r = songRows[nav.songRow];
          if (dy) r.scene = (uint8_t)clamp(r.scene - dy, 0, NS - 1);
          else r.rep = (uint8_t)clamp(r.rep + dx, 1, 16);
          if (songOn && songPos == nav.songRow) {   // the sounding row: take it up live
            if (songBarsLeft > songRowBars(songPos)) songBarsLeft = songRowBars(songPos);
            if (songBarsLeft == 1) songQueueNext();
          }
          aUsed = true;
          dirty = true;
          break;
        }
        if (b) {                                 // B held + U/D: reorder
          if (dy && fresh) { songMoveRow(nav.songRow, dy); bUsed = true; dirty = true; }
          break;
        }
        nav.songRow = (uint8_t)clamp(nav.songRow + dy, 0, songLen - 1);
        dirty = true;
        break;

      case SCR_PROJ:
        if (a) {
          if (nav.projSel == 13) { projKeyStep(dx, dy); aUsed = true; dirty = true; }
          break;
        }
        if (b) break;
        nav.projSel = (uint8_t)clamp(nav.projSel + dx + dy * 4, 0, 15);
        dirty = true;
        break;

      default:                                   // INST
        if (a) {                                 // A held: the cell's value
          if (nav.instRow == 0) { if (fresh || nav.instTop == 1) { instTopStep(dx, dy); dirty = true; } }
          else { instCellStep(dy ? -dy * 10 : dx); dirty = true; }
          aUsed = true;
          break;
        }
        if (b) {                                 // B held + L/R: the SYN/MOD/FX page
          if (dx && fresh) { nav.instPage = (uint8_t)((nav.instPage + dx + 3) % 3); bUsed = true; dirty = true; }
          break;
        }
        if (dy) nav.instRow = (uint8_t)clamp(nav.instRow + dy, 0, 2);
        else if (nav.instRow == 0) {
          bool presets = curEngine() == E_GM || curEngine() == E_DM;
          if (presets) nav.instTop = (uint8_t)clamp(nav.instTop + dx, 0, 1);
          else nav.instTop = 0;
        } else nav.instCol = (uint8_t)clamp(nav.instCol + dx, 0, 3);
        dirty = true;
        break;
    }
  }
  return dirty || dirtyNow;
}
