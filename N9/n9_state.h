// n9_state.h — N9's shared state: the screen stack position and each screen's
// cursor. Mock content stays in n9_ui.cpp; only what the keys move lives here.
#pragma once
#include <stdint.h>

#include "n9_ui.h"   // Screen

struct NavState {
  uint8_t screen = SCR_GRID;     // the clip grid is home

  // FX: rows 0 delay params, 1 reverb params, 2 send A, 3 send B;
  // col 0..3 on the param rows, 0..8 on the send rows (clamped on a row change).
  uint8_t fxRow = 0, fxCol = 0;

  uint8_t mixSel = 2;            // mixer strip

  // MOD: rows 0..3 the sources, 4 the parameter cells (col 0..3). The cells
  // edit the source row last stood on (modSrc).
  uint8_t modRow = 0, modCell = 3, modSrc = 0;

  // INST: row 0 holds two cells — the engine selector (instTop 0, left,
  // a quarter of the row) and the preset cell (1, engines with banks) —
  // rows 1..2 the parameter cells (col 0..3).
  uint8_t instRow = 0, instCol = 0, instTop = 0;

  // TFX (the track's two FX slots): rows 0/2 the slots' selectors, 1/3 their
  // parameter cells (col 0..3).
  uint8_t tfxRow = 0, tfxCol = 0;

  // MFX (the master's two FX slots, above the sends): the same shape.
  uint8_t mfxRow = 0, mfxCol = 0;

  uint8_t gridT = 2, gridS = 1;  // clip grid cursor: its track is the selected track

  // Piano roll cursor: the bar page shown, the step within it, and the
  // cursor's absolute pitch; the 13-row window slides by octaves (editOct).
  uint8_t editPage = 0, editStep = 8;
  uint8_t editPitch = 60;
  int8_t editOct = 0;                  // window top = 60 + 12 * editOct
  bool scaleShow = false;              // B held on the grid: the key/scale overlay

};

extern NavState nav;
inline uint8_t &curEngine();   // defined below NavState

// The engine lineup (user-trimmed 2026-10-13): Prophet-5, 303, Juno, then
// every Mutable Instruments model as its own engine (n9_mi.h). The M1 and
// Microwave emulations are parked (code kept, selector entries gone) until
// their real sample/wavetable data is in.
constexpr uint8_t E_PRO5 = 0, E_303 = 1, E_JUNO = 2, E_MI_FIRST = 3;
constexpr uint8_t ENGINE_COUNT = 3 + 24;
const char *engineName(uint8_t e);   // n9_synth.cpp

// The INST screen's eight parameter cells, live per engine (0-99); names and
// defaults per engine kind (n9_synth.cpp). A "-" name is an empty cell.
struct ParamInfo { const char *name; uint8_t def; };
const ParamInfo *engineParams(uint8_t e);

// Per track: its engine and, per engine, its eight cells (edits are kept
// when stepping engines or tracks). The selected track (the grid cursor)
// is what MIDI, the sequencer and the INST/MOD/TFX screens act on.
extern uint8_t trackEngine[9];
extern uint8_t engParams[9][ENGINE_COUNT][8];
// Parked engines: sentinel ids no selector position reaches.
constexpr uint8_t E_MWAVE = 254, E_M1 = 253;

inline uint8_t &curEngine() { return trackEngine[nav.gridT]; }
inline uint8_t *curParams(uint8_t engine) { return engParams[nav.gridT][engine]; }
