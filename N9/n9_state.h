// n9_state.h — N4's shared state: the three-screen stack (GRID, NOTE, INST,
// wrapping), each screen's cursor, the selected track, and the per-track
// instrument layer the INST pages edit. The 2026-10-16 reorganisation: four
// tracks, four scenes, minimalist UI.
#pragma once
#include <stdint.h>

#include "n9_ui.h"   // Screen

// Four tracks and four scenes for now (the engine underneath still carries
// nine and eight; only these are shown and played).
constexpr uint8_t NT = 4, NS = 4;

struct NavState {
  uint8_t screen = SCR_GRID;

  uint8_t selTrack = 0;          // the selected track (B+A+LEFT/RIGHT, wraps);
                                 // MIDI, overdub and INST act on it

  // GRID: row 0 is the status bar (its cell in statusSel), rows 1..4 the
  // clip grid (gridT = column = track, gridS = scene).
  uint8_t gridRow = 1, gridT = 0, gridS = 0, statusSel = 4;

  // NOTE: the bar page shown, the cursor cell (step within the bar, pitch),
  // the 13-row pitch window (top = EDIT_TOP_PITCH + 12 * noteOct), and the
  // selection: a bit per clip note, plus the B-drag box while it's open.
  uint8_t notePage = 0, noteStep = 0;
  uint8_t notePitch = 60;
  uint8_t noteTop = 72;                // the top visible row (in-scale pitch)
  uint32_t sel[4] = {0, 0, 0, 0};      // selected notes of the shown clip
  bool boxOpen = false;                // B dragging a selection box
  uint8_t boxStep0 = 0, boxPitch0 = 0; // its anchor (absolute step, pitch)
  uint8_t boxStep1 = 0, boxPitch1 = 0; // its moving corner

  // INST: page 0 SYN, 1 MOD, 2 FX; cursor row 0 = the synth selector, rows
  // 1..2 the page's two knob rows (col 0..3); the status bar is row 3.
  uint8_t instPage = 0, instRow = 0, instCol = 0;

  bool menu = false;             // past the grid's left/right edge: the menu
  uint8_t menuSel = 0;
  uint8_t menuCardCol = 0;

  // An arrow held past an edge: its line shows on that edge and the switch
  // fires on release or after 600 ms held (L7's grammar). 0: none pending.
  uint8_t edgePending = 0;
  uint32_t edgeAt = 0;
};

extern NavState nav;

// The status bar's cells (GRID's top row, INST's bottom).
enum { ST_TAP, ST_TEMPO, ST_METRO, ST_OD, ST_PLAY, ST_KEY, ST_COUNT };

// Transport extras.
extern bool overdubOn;           // B+PLAY (or the status bar): MIDI records
extern bool metroOn;             // the status bar's metronome

// The engine lineup (the selector on INST's top row).
constexpr uint8_t E_PRO5 = 0, E_303 = 1, E_JUNO = 2, E_MI_FIRST = 3;
constexpr uint8_t E_GM = 27;
constexpr uint8_t E_GMDRUM = 28;
constexpr uint8_t E_DM = 29;
constexpr uint8_t E_M1 = 253, E_MWAVE = 254;   // parked (code kept, no selector entry)
constexpr uint8_t ENGINE_COUNT = 3 + 24 + 3;
const char *engineName(uint8_t e);   // n9_synth.cpp

extern uint8_t gmProgram[9];
extern uint8_t drumMachine[9];

struct ParamInfo { const char *name; uint8_t def; };
const ParamInfo *engineParams(uint8_t e);
extern uint8_t trackEngine[9];
void paramsInit();

// The per-engine source cells (SYN row 1 edits cells 0..3 of the current
// engine; the other four keep their defaults).
extern uint8_t engParams[9][ENGINE_COUNT][8];

// The INST layer on top of every pool voice: a state-variable filter
// (type 0 LP, 1 HP, 2 BP), the MOD ADSR (volume, and the cutoff by fenv),
// and one LFO per track.
enum : uint8_t { LFO_SINE, LFO_NOISE, LFO_SQUARE, LFO_UP, LFO_DOWN, LFO_ENV, LFO_SHAPES };
// LFO destinations, in the spec's order.
enum : uint8_t { LD_SYN1, LD_SYN2, LD_SYN3, LD_SYN4, LD_CUT, LD_RES, LD_FENV,
                 LD_VOL, LD_PAN, LD_AX, LD_AY, LD_AWET, LD_BX, LD_BY, LD_BWET,
                 LD_PIT, LD_COUNT };   // PIT appended: saved projects keep their values
struct TrackLayer {
  uint8_t cut = 99, res = 10, ftype = 0, fenv = 50;   // filter (fenv 50 = none)
  uint8_t a = 0, d = 50, s = 99, r = 40;              // the MOD ADSR
  uint8_t lfoRate = 40, lfoAmt = 0, lfoShape = 0, lfoDest = 0;
};
extern TrackLayer trackL[9];
void trackLayerInit();

// The LFO's block-rate value per track (-1..1 scaled by amount), applied
// where its destination lives; written by the audio interrupt.
extern volatile float lfoVal[9];

inline uint8_t &curEngine() { return trackEngine[nav.selTrack]; }
