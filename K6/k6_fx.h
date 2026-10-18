// k6_fx.h — N9's effects.
//
// Every effect has eight parameters; the last is always DRY/WET.
// Each track has TWO effects in series. A slot is either
//   * a mono insert (FLANGER .. WOW): its own state and settings per track,
//     in the track's mono path before the mixer strip, or
//   * one of the three global stereo SEND effects (DELAY, REVERB, CHORUS),
//     post the mixer strip. Cells 1-7 then edit the shared send effect
//     itself; cell 8 is this track's send control: up to midway it sends
//     more and more (100% at half), past midway the track's own path to
//     the mixer fades out while the send stays at 100%.
// Series rules: FX1 feeds FX2. FX1 send + FX2 insert: FX2 gets the
// remaining (fading) dry. FX1 send + FX2 send: FX2 taps the full synth
// output, both dry reductions apply to what reaches the mixer. FX1 insert
// + FX2 send: the send taps FX1's output. The master keeps one stereo
// effect, then the master volume and the soft clipper.
#pragma once
#include <stdint.h>

#include "k6_state.h"   // ParamInfo

enum FxType : uint8_t {
  FXT_NONE,
  FXT_FLANGER, FXT_PHASER, FXT_TREM, FXT_DRIVE, FXT_CRUSH,   // mono inserts
  FXT_FILTER, FXT_COMP, FXT_WOW,
  FXT_DELAY, FXT_REVERB, FXT_CHORUS,    // sends on tracks; plain stereo on the master
  FXT_PINGPONG,                         // master only
  FX_TYPES
};
extern const char *const FX_TYPE_NAMES[FX_TYPES];

bool fxIsSend(uint8_t type);            // DELAY/REVERB/CHORUS on a track slot
int fxSendIndex(uint8_t type);          // 0..2, or -1

struct FxSlot { uint8_t type; uint8_t p[8]; };
extern FxSlot trackFx[9][2];
extern FxSlot sendFx[3];                // the shared send effects' own settings
extern FxSlot masterFx;

// The eight cells' names/defaults; a send slot's last cell reads SEND.
const ParamInfo *fxParams(uint8_t type, bool asSend);

void fxSlotsInit();
void fxSetTrackType(uint8_t t, uint8_t slot, uint8_t type);
void fxSetMasterType(uint8_t type);
uint8_t fxStepTrackType(uint8_t type, int dir);   // skips master-only types
uint8_t fxStepMasterType(uint8_t type, int dir);

void fxBeginBlock(int n);                         // clears the send taps
// Runs FX1 -> FX2 over the track's mono bus in place (inserts), tapping the
// send buses per the rules; postVol is the track's post-fader gain, applied
// to the taps (the sends sit post the strip).
void fxTrackChain(uint8_t t, float *mono, int n, float postVol);
void fxRenderSends(float *addL, float *addR, int n);
void fxProcessMaster(float *l, float *r, int n);
float fxSoftClip(float x);
