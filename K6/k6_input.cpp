// k6_input.cpp — the navigation prototype's keys: SHIFT+arrows move across
// the 3x2 screen map (wrapping); plain arrows move each screen's cursor.
// The grammar (A act / A+arrows primary / B+arrows secondary / B-tap zoom /
// SHIFT+A copy / SHIFT+B paste) lands with the real content.
#include "k6_input.h"

#include "M8Board.h"
#include "k6_state.h"

extern m8::Board board;

using namespace m8;

NavState nav;

static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 50;
enum { A_UP, A_DOWN, A_LEFT, A_RIGHT };
static const uint8_t ARROW_BIT[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool inputService() {
  uint8_t now = board.keys();
  static uint8_t prev = 0;
  uint8_t pressed = now & ~prev;
  prev = now;
  uint32_t ms = millis();
  bool shift = now & BTN_SHIFT;
  bool dirty = false;

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

    if (shift) {                                 // the screen map, wrapping
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
        nav.notePitch = (uint8_t)clamp(nav.notePitch - dy, 24, 96);
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
  return dirty;
}
