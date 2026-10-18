// k6_state.h — K6's shared state: the 3x2 screen map position and each
// screen's cursor. Design: docs/design/k6_mockup.html. Navigation prototype:
// mock content lives in k6_ui.cpp; only what the keys move lives here.
#pragma once
#include <stdint.h>

constexpr uint8_t NT = 6, NS = 6;      // tracks, scenes

// The screen map, 3 columns x 2 rows (SHIFT+arrows, wrapping):
//   SONG  GRID  MIX
//   PROJ  NOTE  INST
enum : uint8_t { MAP_COLS = 3, MAP_ROWS = 2 };
enum Screen : uint8_t { SCR_SONG, SCR_GRID, SCR_MIX, SCR_PROJ, SCR_NOTE, SCR_INST };
inline Screen screenAt(uint8_t r, uint8_t c) { return (Screen)(r * 3 + c); }

// ---- the engine's shared state (lifted from the N9-era app, kept in step:
// the synth pool, FX and projects read these; the UI edits them) ------------

// Transport extras.
extern bool overdubOn;           // MIDI records into the playing clip
extern bool metroOn;             // the metronome's blip

// The engine lineup (the selector on INST's top row).
constexpr uint8_t E_PRO5 = 0, E_303 = 1, E_JUNO = 2, E_MI_FIRST = 3;
constexpr uint8_t E_GM = 27;
constexpr uint8_t E_GMDRUM = 28;
constexpr uint8_t E_DM = 29;
constexpr uint8_t E_M1 = 253, E_MWAVE = 254;   // parked (code kept, no selector entry)
constexpr uint8_t ENGINE_COUNT = 3 + 24 + 3;
const char *engineName(uint8_t e);   // k6_synth.cpp

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

// ---- navigation ------------------------------------------------------------

struct NavState {
  uint8_t mapR = 0, mapC = 1;          // GRID is home

  uint8_t selTrack = 0;

  uint8_t gridT = 0, gridS = 0;        // the grid cursor (col NT = the scene column)
  uint8_t songRow = 0;                 // the arranger cursor
  uint8_t mixT = 0;                    // the mixer strip (NT = master)
  uint8_t noteStep = 0, notePitch = 60, notePage = 0;
  uint8_t noteTop = 71;                // the top visible roll row (in-scale pitch)
  uint32_t sel[4] = {0, 0, 0, 0};      // selected notes of the edited clip
  bool boxOpen = false;                // B dragging a selection box
  uint8_t boxStep0 = 0, boxPitch0 = 0; // its anchor (absolute step, pitch)
  uint8_t boxStep1 = 0, boxPitch1 = 0; // its moving corner
  uint8_t instPage = 0, instRow = 0, instCol = 0, instTop = 0;   // instTop: 0 engine, 1 preset
  uint8_t projSel = 0;
};

extern NavState nav;

inline uint8_t &curEngine() { return trackEngine[nav.selTrack]; }
