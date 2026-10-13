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
// Every track owns a live mono insert (before the mixer) - all nine process
// at once - and the master a stereo effect (before the master volume). The
// time-based types (DELAY, PINGPONG, REVERB) need more delay memory than a
// track slot carries, so they are selectable on the master only.
extern FxSlot trackFx[9];
extern FxSlot masterFx;

const ParamInfo *fxParams(uint8_t type);  // the type's four cells
bool fxTrackAllowed(uint8_t type);
void fxSlotsInit();
void fxSetTrackType(uint8_t t, uint8_t type);   // loads defaults, clears state
void fxSetMasterType(uint8_t type);

void fxProcessTrack(uint8_t t, float *mono, int n);   // in place
void fxProcessMaster(float *l, float *r, int n);

// The output soft clipper: unity through +-1.0, never past it.
float fxSoftClip(float x);
