// n9_input.cpp — see n9_input.h.
#include "n9_input.h"

#include "M8Board.h"
#include "n9_state.h"
#include "n9_synth.h"
#include "n9_mi.h"
#include "n9_fx.h"
#include "n9_song.h"

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
      if (dx) {
        if (nav.instRow > 0) c |= to(nav.instCol, clamp(nav.instCol + dx, 0, 3));
        else c |= to(nav.instTop, clamp(nav.instTop + dx, 0, 1));
      }
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
  bool dirtyNow = false;
  if (pressed & BTN_PLAY) { synth.seqToggle(); dirtyNow = true; }   // start from 1.1.1 / stop
  if ((pressed & BTN_EDIT) && !shift && !opt && nav.screen == SCR_EDIT) {
    // A on the roll: add a step note at the cursor, or remove the one there.
    clipToggle(clipAt(nav.gridT, nav.gridS), EDIT_TOP_PITCH - nav.editRow, nav.editStep, 100);
    dirtyNow = true;
  }
  uint32_t ms = millis();
  static uint32_t pressedAt[4], lastRep[4];
  bool dirty = false;

  for (int i = 0; i < 4; i++) {
    uint8_t bit = ARROW_BIT[i];
    bool fire = false;
    if (pressed & bit) { pressedAt[i] = ms; lastRep[i] = 0; fire = true; }
    else if ((now & bit) && !shift && ms - pressedAt[i] >= REPEAT_DELAY_MS &&
             (lastRep[i] == 0 || ms - lastRep[i] >= REPEAT_PERIOD_MS)) { lastRep[i] = ms; fire = true; }
    if (!fire) continue;

    if (shift) {                                   // the screen stack (fresh presses only)
      if (i == A_UP && nav.screen > 0) { nav.screen--; dirty = true; }
      if (i == A_DOWN && nav.screen < SCR_COUNT - 1) { nav.screen++; dirty = true; }
    } else if (opt) {                              // B + arrows on INST: step the engine,
      if (nav.screen == SCR_INST) {                // the preset, or a parameter cell
        if (nav.instRow == 0) {
          if (nav.instTop == 0 && (i == A_LEFT || i == A_RIGHT)) {
            curEngine() = (curEngine() + (i == A_RIGHT ? 1 : ENGINE_COUNT - 1)) % ENGINE_COUNT;
            dirty = true;
          }                                        // (the preset cell: no banked engine right now)
        } else {
          int idx = (nav.instRow - 1) * 4 + nav.instCol;
          if (engineParams(curEngine())[idx].name[0] != '-') {
            int d = i == A_LEFT ? -1 : i == A_RIGHT ? 1 : i == A_UP ? 10 : -10;
            uint8_t &p = engParams[nav.gridT][curEngine()][idx];
            p = (uint8_t)clamp(p + d, 0, 99);
            dirty = true;
          }
        }
      } else if (nav.screen == SCR_EDIT) {       // B + arrows on a note: length / velocity
        Clip &c = clipAt(nav.gridT, nav.gridS);
        int ni = clipFindAt(c, EDIT_TOP_PITCH - nav.editRow, nav.editStep);
        if (ni >= 0) {
          ClipNote &cn = c.notes[ni];
          if (i == A_LEFT || i == A_RIGHT) {
            int len = clamp(cn.len + (i == A_RIGHT ? 1 : -1), 1, CLIP_STEPS - cn.start);
            cn.len = (uint8_t)len;
          } else {
            cn.vel = (uint8_t)clamp(cn.vel + (i == A_UP ? 10 : -10), 1, 127);
          }
          dirty = true;
        }
      } else if (nav.screen == SCR_TFX || nav.screen == SCR_MFX) {
        uint8_t row = nav.screen == SCR_TFX ? nav.tfxRow : nav.mfxRow;
        uint8_t col = nav.screen == SCR_TFX ? nav.tfxCol : nav.mfxCol;
        uint8_t slot = (nav.screen == SCR_MFX ? 2 : 0) + (row >= 2 ? 1 : 0);
        if (!(row & 1)) {                        // the type selector
          if (i == A_LEFT || i == A_RIGHT) {
            fxSetType(slot, (fxSlots[slot].type + (i == A_RIGHT ? 1 : FX_TYPES - 1)) % FX_TYPES);
            if (slot < 2) trackFx[nav.gridT][slot] = fxSlots[slot];   // the track keeps it
            dirty = true;
          }
        } else if (fxParams(fxSlots[slot].type)[col].name[0] != '-') {
          int d = i == A_LEFT ? -1 : i == A_RIGHT ? 1 : i == A_UP ? 10 : -10;
          uint8_t &p = fxSlots[slot].p[col];
          p = (uint8_t)clamp(p + d, 0, 99);
          if (slot < 2) trackFx[nav.gridT][slot].p[col] = p;
          dirty = true;
        }
      }
    } else {
      dirty |= move(i == A_LEFT ? -1 : i == A_RIGHT ? 1 : 0, i == A_UP ? -1 : i == A_DOWN ? 1 : 0);
    }
  }
  static uint8_t lastTrack = 0;                  // a track switch brings its engine's
  if (nav.gridT != lastTrack) {                  // screens and its FX chain with it
    lastTrack = nav.gridT;
    fxApplyTrack(nav.gridT);
    dirtyNow = true;
  }
  return dirty || dirtyNow;
}
