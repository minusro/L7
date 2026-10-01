// storage.h — SD side of the looper: streaming tracks into their rings,
// writing recordings, and keeping tracks/tempo across power cycles.
// Everything here runs from loop(), never from an interrupt.
#pragma once
#include "engine.h"

namespace looper {

bool storageBegin();                 // mount SD, load saved tracks; false = no card
void serviceStorage();               // call as often as possible from loop()

void transportStart();               // from bar 1, all tracks in sync
void transportStop();

// Record button for track t: start (or arm until the next bar), then stop.
// Returns false if the track already holds a loop.
bool recordPress(int t);
void clearTrack(int t);
bool setRange(int t, uint32_t startBar, uint32_t bars);

float loadTempo();                   // saved tempo, or 120
void saveTempo(float bpm);
void saveTrackInfo(int t);

// Debugging: the step loop() is in, for the freeze detector in the sketch.
extern volatile const char *debugStage;

}  // namespace looper
