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

  // INST: row 0 the engine selector, rows 1..2 the parameter cells (col 0..3).
  uint8_t instRow = 0, instCol = 0;

  uint8_t gridT = 2, gridS = 1;  // clip grid cursor: its track is the selected track

  uint8_t editStep = 8, editRow = 0;   // piano roll cursor (16 steps x 13 rows)

  uint8_t engine = 0;            // the selected track's engine (one global for now)
};

extern NavState nav;

// The engines the prototype knows (n9_synth.cpp): names for the INST screen.
constexpr uint8_t ENGINE_COUNT = 4;
extern const char *const ENGINE_NAMES[ENGINE_COUNT];
