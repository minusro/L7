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

  uint8_t editStep = 8, editRow = 0;   // piano roll cursor (16 steps x 13 rows)

  uint8_t engine = 0;            // the selected track's engine (one global for now)
};

extern NavState nav;

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
extern uint8_t engParams[ENGINE_COUNT][8];
// Parked engines: sentinel ids no selector position reaches.
constexpr uint8_t E_MWAVE = 254, E_M1 = 253;
