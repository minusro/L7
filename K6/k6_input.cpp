// k6_input.cpp — K6's keys. SHIFT+arrows move across the 3x2 screen map
// (wrapping); plain arrows move each screen's cursor; A (the EDIT key) tap
// acts: GRID launches/stops/creates clips (the scene column whole rows),
// NOTE toggles a step note, MIX mutes, PROJ loads/creates projects and runs
// the items. PLAY toggles the transport (SHIFT+PLAY resume/reset), and PLAY
// held + arrows set the tempo anywhere (U/D ±1, L/R ±0.1 BPM). The rest of
// the grammar (A/B held value edits, B-tap zoom, clipboards) lands next.
#include "k6_input.h"

#include "M8Board.h"
#include "k6_apps.h"
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

// GRID's A: the cursor cell launches/stops (or is created empty); the scene
// column launches the whole row on the next bar line (tracks without a clip
// there stop).
static void gridAct() {
  if (nav.gridT < NT) {
    Clip &c = clipAt(nav.gridT, nav.gridS);
    if (!c.used) { c.used = 1; if (!c.bars) c.bars = 1; return; }
    clipPending[nav.gridT] =
        clipPlaying[nav.gridT] == (int8_t)nav.gridS ? CLIP_STOP : (int8_t)nav.gridS;
    if (!synth.seqOn()) synth.seqToggle();         // a launch starts the clock
  } else {
    for (int t = 0; t < NT; t++)
      clipPending[t] = clipAt(t, nav.gridS).used ? (int8_t)nav.gridS : CLIP_STOP;
    if (!synth.seqOn()) synth.seqToggle();
  }
}

static void noteAct() {
  uint8_t t = nav.gridT < NT ? nav.gridT : nav.selTrack;
  Clip &c = clipAt(t, nav.gridS);
  clipToggle(c, nav.notePitch, nav.notePage * CLIP_STEPS + nav.noteStep, 100);
}

static void projAct() {
  if (nav.projSel < 12) {
    if (projExists[nav.projSel]) projectRequestLoad(nav.projSel);
    else projectNew(nav.projSel);
  } else if (nav.projSel == 12) {
    projectSaveCurrent();
    usbDriveMode();
    projectReloadCurrent();                        // the computer may have changed the card
  } else if (nav.projSel == 13) {
    projScale = (uint8_t)((projScale + 1) % SCALE_COUNT);
  } else {
    appSwitchToL7();                               // returns only on failure
  }
}

bool inputService() {
  uint8_t now = board.keys();
  static uint8_t prev = 0;
  uint8_t pressed = now & ~prev;
  uint8_t released = prev & ~now;
  prev = now;
  uint32_t ms = millis();
  bool shift = now & BTN_SHIFT, play = now & BTN_PLAY;
  bool dirty = false;

  // PLAY: SHIFT+PLAY acts at once; a plain PLAY waits for its release so the
  // held+arrows tempo gesture doesn't also toggle the transport.
  static bool playUsed = false;
  if (pressed & BTN_PLAY) {
    playUsed = false;
    if (shift) { synth.seqShiftPlay(); playUsed = true; dirty = true; }
  }
  if ((released & BTN_PLAY) && !playUsed) { synth.seqToggle(); dirty = true; }

  static uint32_t pressedAt[4], lastRep[4];
  for (int i = 0; i < 4; i++) {
    uint8_t bit = ARROW_BIT[i];
    bool fire = false;
    if (pressed & bit) { pressedAt[i] = ms; lastRep[i] = 0; fire = true; }
    else if ((now & bit) && !shift && ms - pressedAt[i] >= REPEAT_DELAY_MS &&
             (lastRep[i] == 0 || ms - lastRep[i] >= REPEAT_PERIOD_MS)) { lastRep[i] = ms; fire = true; }
    if (!fire) continue;
    int dx = i == A_LEFT ? -1 : i == A_RIGHT ? 1 : 0;
    int dy = i == A_UP ? -1 : i == A_DOWN ? 1 : 0;

    if (play) {                                    // PLAY held: tempo anywhere
      playUsed = true;
      float bpm = synth.seqBpm() - dy * 1.0f + dx * 0.1f;
      synth.setSeqBpm(bpm < 40 ? 40 : bpm > 300 ? 300 : bpm);
      dirty = true;
      continue;
    }
    if (shift) {                                   // the screen map, wrapping
      if (!(pressed & bit)) continue;
      nav.mapC = (uint8_t)((nav.mapC + dx + MAP_COLS) % MAP_COLS);
      nav.mapR = (uint8_t)((nav.mapR + dy + MAP_ROWS) % MAP_ROWS);
      dirty = true;
      continue;
    }
    switch (screenAt(nav.mapR, nav.mapC)) {
      case SCR_GRID:
        nav.gridT = (uint8_t)clamp(nav.gridT + dx, 0, NT);      // NT = the scene column
        nav.gridS = (uint8_t)clamp(nav.gridS + dy, 0, NS - 1);
        if (nav.gridT < NT) nav.selTrack = nav.gridT;
        break;
      case SCR_SONG: nav.songRow = (uint8_t)clamp(nav.songRow + dy, 0, 4); break;
      case SCR_MIX:
        nav.mixT = (uint8_t)clamp(nav.mixT + dx, 0, NT);
        if (nav.mixT < NT) nav.selTrack = nav.mixT;
        break;
      case SCR_NOTE:
        nav.noteStep = (uint8_t)clamp(nav.noteStep + dx, 0, 15);
        nav.notePitch = (uint8_t)clamp(nav.notePitch - dy, 12, 108);
        if (nav.notePitch > nav.noteTop) nav.noteTop = nav.notePitch;
        if (nav.notePitch < nav.noteTop - 11) nav.noteTop = (uint8_t)(nav.notePitch + 11);
        break;
      case SCR_PROJ: nav.projSel = (uint8_t)clamp(nav.projSel + dx + dy * 4, 0, 14); break;
      default:
        if (dy) nav.instRow = (uint8_t)clamp(nav.instRow + dy, 0, 2);
        else if (nav.instRow == 0) nav.instPage = (uint8_t)((nav.instPage + dx + 3) % 3);
        else nav.instCol = (uint8_t)clamp(nav.instCol + dx, 0, 3);
        break;
    }
    dirty = true;
  }

  // A tap: act on the cursor's object (plain presses only for now; the held
  // combos and B's zoom come with the full grammar).
  if ((pressed & BTN_A) && !shift && !play && !(now & BTN_B)) {
    switch (screenAt(nav.mapR, nav.mapC)) {
      case SCR_GRID: gridAct(); dirty = true; break;
      case SCR_NOTE: noteAct(); dirty = true; break;
      case SCR_MIX:
        if (nav.mixT < NT) { trackMute[nav.mixT] = !trackMute[nav.mixT]; dirty = true; }
        break;
      case SCR_PROJ: projAct(); dirty = true; break;
      default: break;
    }
  }
  return dirty;
}
