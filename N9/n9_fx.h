// n9_fx.h — the selectable effects behind the TFX and MFX screens: four
// slots (two on the track bus, two on the master), each one of twelve
// effects with four parameters (0-99). Audio runs the slots in order
// (track 1, track 2, master 1, master 2) over the stereo bus.
#pragma once
#include <stdint.h>

#include "n9_state.h"   // ParamInfo

enum FxType : uint8_t {
  FXT_NONE, FXT_CHORUS, FXT_FLANGER, FXT_PHASER, FXT_TREM, FXT_DELAY,
  FXT_PINGPONG, FXT_REVERB, FXT_DRIVE, FXT_CRUSH, FXT_FILTER, FXT_COMP,
  FXT_WOW, FX_TYPES
};
extern const char *const FX_TYPE_NAMES[FX_TYPES];

struct FxSlot { uint8_t type; uint8_t p[4]; };
extern FxSlot fxSlots[4];                 // the processors: 0,1 track; 2,3 master

// Each track keeps its own two slot settings; only the selected track's
// chain processes (there is one voice chain for now), so switching tracks
// swaps its settings into processors 0/1 (tails clear).
extern FxSlot trackFx[9][2];
void fxApplyTrack(uint8_t t);

const ParamInfo *fxParams(uint8_t type);  // the type's four cells
void fxSlotsInit();
void fxSetType(uint8_t slot, uint8_t type);   // loads the defaults, clears state

// Processes all four slots over one stereo block, in place.
void fxProcessAll(float *l, float *r, int n);
