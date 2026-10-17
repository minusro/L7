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

struct NavState {
  uint8_t mapR = 0, mapC = 1;          // GRID is home

  uint8_t selTrack = 0;

  uint8_t gridT = 0, gridS = 0;        // the grid cursor (col NT = the scene column)
  uint8_t songRow = 0;                 // the arranger cursor
  uint8_t mixT = 0;                    // the mixer strip (NT = master)
  uint8_t noteStep = 0, notePitch = 60, notePage = 0;
  uint8_t instPage = 0, instRow = 0, instCol = 0;
  uint8_t projSel = 0;
};

extern NavState nav;
