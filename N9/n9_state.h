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

// The agreed engine lineup (2026-10-13; docs/design/n9_mockup.html). The
// synth implements SAW, PULSE, SUPER, FM2, PLUCK and NOISE; the rest play
// placeholder voices until their emulations exist (n9_synth.cpp).
constexpr uint8_t ENGINE_COUNT = 15;
extern const char *const ENGINE_NAMES[ENGINE_COUNT];
// Engines the code treats specially. E_M1 is hidden from the selector for
// now (the stand-ins disappoint until the real PCM is in).
constexpr uint8_t E_303 = 8, E_MWAVE = 11, E_M1 = 12, E_PLAITS = 13, E_SPELL = 14;
