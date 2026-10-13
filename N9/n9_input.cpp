// n9_input.cpp — see n9_input.h.
#include "n9_input.h"

#include "M8Board.h"
#include "n9_state.h"
#include "n9_synth.h"
#include "n9_mi.h"
#include "n9_fx.h"
#include "n9_song.h"
#include "n9_apps.h"

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
      return dx ? to(nav.mixSel, clamp(nav.mixSel + dx, 0, 9)) : false;   // 9 = master
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
      if (dy) {
        c = to(nav.tfxRow, clamp(nav.tfxRow + dy, 0, 3));
        if (nav.tfxRow < 2) c |= to(nav.tfxSlot, nav.tfxRow);   // the cells follow this slot
      }
      if (dx && nav.tfxRow >= 2) c |= to(nav.tfxCol, clamp(nav.tfxCol + dx, 0, 3));
      return c;
    }
    case SCR_MFX: {
      bool c = false;
      if (dy) c = to(nav.mfxRow, clamp(nav.mfxRow + dy, 0, 2));
      if (dx && nav.mfxRow >= 1) c |= to(nav.mfxCol, clamp(nav.mfxCol + dx, 0, 3));
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
      if (dx) {                                  // steps run across bar pages
        int pos = clamp(nav.editPage * 16 + nav.editStep + dx, 0, CLIP_MAX_BARS * 16 - 1);
        c = to(nav.editPage, pos / 16);
        c |= to(nav.editStep, pos % 16);
      }
      if (dy) {                                  // pitches skip out-of-scale notes,
        int p = nav.editPitch;                   //   the window slides by octaves
        do p -= dy; while (p >= 24 && p <= 96 && !scaleHas((uint8_t)p));
        if (p >= 24 && p <= 96) {
          c |= to(nav.editPitch, p);
          int top = EDIT_TOP_PITCH + 12 * nav.editOct;
          while (p > top) { nav.editOct++; top += 12; c = true; }
          while (p < top - 12) { nav.editOct--; top -= 12; c = true; }
        }
      }
      return c;
    }
  }
  return false;
}

// The edge switch itself: LEFT into the menu, RIGHT (on the menu) back out,
// UP/DOWN along the screen stack.
static void edgeSwitch(uint8_t bit) {
  if (nav.menu) { if (bit == BTN_RIGHT) nav.menu = false; }
  else if (bit == BTN_LEFT) nav.menu = true;
  else if (bit == BTN_UP && nav.screen > 0) nav.screen--;
  else if (bit == BTN_DOWN && nav.screen < SCR_COUNT - 1) nav.screen++;
  nav.edgePending = 0;
}

bool inputService() {
  uint8_t now = board.keys();
  static uint8_t prev = 0;
  uint8_t pressed = now & ~prev;
  uint8_t released = prev & ~now;
  prev = now;
  uint32_t ms = millis();

  bool shift = now & BTN_SHIFT, opt = now & BTN_OPT;
  bool dirtyNow = false;

  // A SHIFT tap (nothing else touched while it's down, nothing held when it
  // came down) toggles the cursor between the content and the status row.
  static bool shiftUsed = false;
  if (pressed & BTN_SHIFT) shiftUsed = (now & ~BTN_SHIFT) != 0;
  if ((now & BTN_SHIFT) && (pressed & ~BTN_SHIFT)) shiftUsed = true;
  if ((released & BTN_SHIFT) && !shiftUsed && !nav.menu) {
    nav.statusFocus = !nav.statusFocus;
    dirtyNow = true;
  }

  // A pending edge switch: any other key cancels it; its own arrow let go
  // (or held 600 ms) fires it. A hold-fired arrow stays dead until let go.
  static uint8_t edgeDead = 0;
  edgeDead &= now;
  if (nav.edgePending && (pressed & ~nav.edgePending)) { nav.edgePending = 0; dirtyNow = true; }
  if (nav.edgePending && (released & nav.edgePending)) { edgeSwitch(nav.edgePending); dirtyNow = true; }
  if (nav.edgePending && (now & nav.edgePending) && ms - nav.edgeAt >= 600) {
    edgeDead |= nav.edgePending;
    edgeSwitch(nav.edgePending);
    dirtyNow = true;
  }
  static bool edgeLight = false;       // the line's first 50 ms are light grey:
  bool lightNow = nav.edgePending && ms - nav.edgeAt < 50;   // one redraw at the turn
  if (nav.edgePending && lightNow != edgeLight) dirtyNow = true;
  edgeLight = lightNow;

  if ((pressed & BTN_EDIT) && shift && opt) {    // SHIFT+B+A: switch to L7
    appSwitchToL7();                             // (returns only on failure)
    dirtyNow = true;
  }
  bool overlay = opt && !shift && !nav.menu && nav.screen == SCR_GRID;
  if (overlay != nav.scaleShow) { nav.scaleShow = overlay; dirtyNow = true; }
  if (pressed & BTN_PLAY) { synth.seqToggle(); dirtyNow = true; }   // start from 1.1.1 / stop
  bool plainA = (pressed & BTN_EDIT) && !shift && !opt && !nav.menu && !nav.statusFocus;
  if (plainA && nav.screen == SCR_MIX && nav.mixSel < 9) {
    trackMute[nav.mixSel] = !trackMute[nav.mixSel];
    dirtyNow = true;
  }
  if (plainA && nav.screen == SCR_EDIT) {
    // A on the roll: add a step note at the cursor, or remove the one there.
    clipToggle(clipAt(nav.gridT, nav.gridS), nav.editPitch, nav.editPage * 16 + nav.editStep, 100);
    dirtyNow = true;
  }
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
      if (nav.menu) continue;
      if (i == A_UP && nav.screen > 0) { nav.screen--; dirty = true; }
      if (i == A_DOWN && nav.screen < SCR_COUNT - 1) { nav.screen++; dirty = true; }
    } else if (nav.menu) {                         // the menu: RIGHT off its edge leaves
      if (i == A_RIGHT && !opt && !(edgeDead & bit) && !nav.edgePending) {
        nav.edgePending = BTN_RIGHT;
        nav.edgeAt = ms;
        dirty = true;
      }
    } else if (nav.statusFocus) {                  // the status row has the cursor
      if (opt) {                                   // B + arrows: the stood-on cell
        if (nav.statusSel == 0 && (i == A_LEFT || i == A_RIGHT)) {       // the scene
          dirty |= to(nav.gridS, clamp(nav.gridS + (i == A_RIGHT ? 1 : -1), 0, 7));
        } else if (nav.statusSel == 2) {                                 // the tempo
          int d = i == A_LEFT ? -1 : i == A_RIGHT ? 1 : i == A_UP ? 10 : -10;
          synth.setSeqBpm((float)clamp((int)synth.seqBpm() + d, 40, 240));
          dirty = true;
        }
      } else if (i == A_LEFT || i == A_RIGHT) {
        dirty |= to(nav.statusSel, clamp(nav.statusSel + (i == A_RIGHT ? 1 : -1), 0, 3));
      }
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
      } else if (nav.screen == SCR_GRID) {       // B + arrows: the project's key and scale
        if (i == A_LEFT || i == A_RIGHT) projKey = (projKey + (i == A_RIGHT ? 1 : 11)) % 12;
        else projScale = (projScale + (i == A_UP ? 1 : SCALE_COUNT - 1)) % SCALE_COUNT;
        nav.editPitch = scaleSnap(nav.editPitch);   // keep the roll cursor legal
        dirty = true;
      } else if (nav.screen == SCR_EDIT) {       // B + arrows on a note: length / velocity
        Clip &c = clipAt(nav.gridT, nav.gridS);
        int ni = clipFindAt(c, nav.editPitch, nav.editPage * 16 + nav.editStep);
        if (ni >= 0) {
          ClipNote &cn = c.notes[ni];
          if (i == A_LEFT || i == A_RIGHT) {
            cn.len = (uint8_t)clamp(cn.len + (i == A_RIGHT ? 1 : -1), 1, CLIP_MAX_BARS * 16 - cn.start);
            clipRefreshBars(c);
          } else {
            cn.vel = (uint8_t)clamp(cn.vel + (i == A_UP ? 10 : -10), 1, 127);
          }
          dirty = true;
        }
      } else if (nav.screen == SCR_MIX) {        // the strip: volume / pan
        int t = nav.mixSel;
        if (i == A_UP || i == A_DOWN) {
          uint8_t &v = t == 9 ? masterVol : trackVol[t];
          v = (uint8_t)clamp(v + (i == A_UP ? 2 : -2), 0, 99);
          dirty = true;
        } else if (t < 9) {
          trackPan[t] = (uint8_t)clamp(trackPan[t] + (i == A_RIGHT ? 2 : -2), 0, 99);
          dirty = true;
        }
      } else if (nav.screen == SCR_TFX) {
        uint8_t slot = nav.tfxRow < 2 ? nav.tfxRow : nav.tfxSlot;
        FxSlot &sl = trackFx[nav.gridT][slot];
        if (nav.tfxRow < 2) {                    // a slot's type selector
          if (i == A_LEFT || i == A_RIGHT)
            { fxSetTrackType(nav.gridT, slot, fxStepTrackType(sl.type, i == A_RIGHT ? 1 : -1)); dirty = true; }
        } else {
          int idx = (nav.tfxRow - 2) * 4 + nav.tfxCol;
          bool send = fxIsSend(sl.type);
          if (fxParams(sl.type, send)[idx].name[0] != '-') {
            int d = i == A_LEFT ? -1 : i == A_RIGHT ? 1 : i == A_UP ? 10 : -10;
            uint8_t &p = (send && idx < 7) ? sendFx[fxSendIndex(sl.type)].p[idx] : sl.p[idx];
            p = (uint8_t)clamp(p + d, 0, 99);
            dirty = true;
          }
        }
      } else if (nav.screen == SCR_MFX) {
        if (nav.mfxRow == 0) {
          if (i == A_LEFT || i == A_RIGHT)
            { fxSetMasterType(fxStepMasterType(masterFx.type, i == A_RIGHT ? 1 : -1)); dirty = true; }
        } else {
          int idx = (nav.mfxRow - 1) * 4 + nav.mfxCol;
          if (fxParams(masterFx.type, false)[idx].name[0] != '-') {
            int d = i == A_LEFT ? -1 : i == A_RIGHT ? 1 : i == A_UP ? 10 : -10;
            masterFx.p[idx] = (uint8_t)clamp(masterFx.p[idx] + d, 0, 99);
            dirty = true;
          }
        }
      }
    } else {
      bool moved = move(i == A_LEFT ? -1 : i == A_RIGHT ? 1 : 0, i == A_UP ? -1 : i == A_DOWN ? 1 : 0);
      if (moved) {
        dirty = true;
        if (nav.edgePending) { nav.edgePending = 0; }
      } else if (!(edgeDead & bit)) {
        // Off the content's edge: arm the switch there (UP/DOWN along the
        // screen stack, LEFT into the menu; RIGHT leads nowhere here).
        uint8_t want = 0;
        if (i == A_UP && nav.screen > 0) want = BTN_UP;
        else if (i == A_DOWN && nav.screen < SCR_COUNT - 1) want = BTN_DOWN;
        else if (i == A_LEFT) want = BTN_LEFT;
        if (want && nav.edgePending != want) {
          nav.edgePending = want;
          nav.edgeAt = ms;
          dirty = true;
        }
      }
    }
  }
  return dirty || dirtyNow;
}
