// n9_input.cpp — see n9_input.h.
#include "n9_input.h"

#include "M8Board.h"
#include "n9_state.h"

using namespace m8;

NavState nav;

static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 50;   // L7's (input.cpp)

enum { A_UP, A_DOWN, A_LEFT, A_RIGHT };
static const uint8_t ARROW_BIT[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};

template <typename T> static bool to(T &field, int v) {
  if ((int)field == v) return false;
  field = (T)v;
  return true;
}
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// One cursor step on the current screen; dx/dy in cells (dy positive = down).
static bool move(int dx, int dy) {
  switch (nav.screen) {
    case SCR_FX: {
      bool c = false;
      if (dy) {
        c = to(nav.fxRow, clamp(nav.fxRow + dy, 0, 3));
        if (nav.fxRow < 2) c |= to(nav.fxCol, clamp(nav.fxCol, 0, 3));   // back on a param row
      }
      if (dx) c |= to(nav.fxCol, clamp(nav.fxCol + dx, 0, nav.fxRow < 2 ? 3 : 8));
      return c;
    }
    case SCR_MIX:
      return dx ? to(nav.mixSel, clamp(nav.mixSel + dx, 0, 8)) : false;
    case SCR_MOD: {
      bool c = false;
      if (dy) {
        c = to(nav.modRow, clamp(nav.modRow + dy, 0, 4));
        if (nav.modRow < 4) c |= to(nav.modSrc, nav.modRow);   // the cells edit the row stood on
      }
      if (dx && nav.modRow == 4) c |= to(nav.modCell, clamp(nav.modCell + dx, 0, 3));
      return c;
    }
    case SCR_INST: {
      bool c = false;
      if (dy) c = to(nav.instRow, clamp(nav.instRow + dy, 0, 2));
      if (dx && nav.instRow > 0) c |= to(nav.instCol, clamp(nav.instCol + dx, 0, 3));
      return c;
    }
    case SCR_TFX: {
      bool c = false;
      if (dy) c = to(nav.tfxRow, clamp(nav.tfxRow + dy, 0, 3));
      if (dx && (nav.tfxRow & 1)) c |= to(nav.tfxCol, clamp(nav.tfxCol + dx, 0, 3));
      return c;
    }
    case SCR_MFX: {
      bool c = false;
      if (dy) c = to(nav.mfxRow, clamp(nav.mfxRow + dy, 0, 3));
      if (dx && (nav.mfxRow & 1)) c |= to(nav.mfxCol, clamp(nav.mfxCol + dx, 0, 3));
      return c;
    }
    case SCR_GRID: {
      bool c = false;
      if (dx) c = to(nav.gridT, clamp(nav.gridT + dx, 0, 8));
      if (dy) c |= to(nav.gridS, clamp(nav.gridS + dy, 0, 7));
      return c;
    }
    case SCR_EDIT: {
      bool c = false;
      if (dx) c = to(nav.editStep, clamp(nav.editStep + dx, 0, 15));
      if (dy) c |= to(nav.editRow, clamp(nav.editRow + dy, 0, 12));
      return c;
    }
  }
  return false;
}

bool inputService() {
  uint8_t now = board.keys();
  static uint8_t prev = 0;
  uint8_t pressed = now & ~prev;
  prev = now;

  bool shift = now & BTN_SHIFT, opt = now & BTN_OPT;
  uint32_t ms = millis();
  static uint32_t pressedAt[4], lastRep[4];
  bool dirty = false;

  for (int i = 0; i < 4; i++) {
    uint8_t bit = ARROW_BIT[i];
    bool fire = false;
    if (pressed & bit) { pressedAt[i] = ms; lastRep[i] = 0; fire = true; }
    else if ((now & bit) && !shift && !opt && ms - pressedAt[i] >= REPEAT_DELAY_MS &&
             (lastRep[i] == 0 || ms - lastRep[i] >= REPEAT_PERIOD_MS)) { lastRep[i] = ms; fire = true; }
    if (!fire) continue;

    if (shift) {                                   // the screen stack (fresh presses only)
      if (i == A_UP && nav.screen > 0) { nav.screen--; dirty = true; }
      if (i == A_DOWN && nav.screen < SCR_COUNT - 1) { nav.screen++; dirty = true; }
    } else if (opt) {                              // B + LEFT/RIGHT on INST: step the engine
      if (nav.screen == SCR_INST && (i == A_LEFT || i == A_RIGHT)) {
        nav.engine = (nav.engine + (i == A_RIGHT ? 1 : ENGINE_COUNT - 1)) % ENGINE_COUNT;
        dirty = true;
      }
    } else {
      dirty |= move(i == A_LEFT ? -1 : i == A_RIGHT ? 1 : 0, i == A_UP ? -1 : i == A_DOWN ? 1 : 0);
    }
  }
  return dirty;
}
