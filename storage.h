// storage.h — SD side of the looper: streaming tracks into their rings,
// writing recordings, and keeping tracks/tempo across power cycles.
// Everything here runs from loop(), never from an interrupt.
#pragma once
#include "engine.h"

namespace looper {

// Mount SD, load saved tracks; creates the full-size track files the first
// time (progress messages go to `progress`). false = no card / no space.
bool storageBegin(void (*progress)(const char *));
void serviceStorage();               // call as often as possible from loop()

void transportStart();               // from bar 1, all tracks in sync
void transportStop();

// Record button for track t: start (or arm until the next bar), then stop.
// Returns false if the track already holds a loop.
bool recordPress(int t);
void clearTrack(int t);
// Loop range in bars. While playing, the change is queued and takes over at
// the next bar line the track hasn't buffered yet; trackRange() reports the
// queued range if there is one.
bool setRange(int t, uint32_t startBar, uint32_t bars);
void trackRange(int t, uint32_t &startBar, uint32_t &bars);

float loadTempo();                   // saved tempo, or 120
void saveTempo(float bpm);           // creates/rewrites a file: only while stopped
void trackChanged(int t);            // settings changed: header saved when buffers allow
void flushHeaders();                 // save all pending headers now (power off)

// Debugging: the step loop() is in, for the freeze detector in the sketch.
extern volatile const char *debugStage;

}  // namespace looper
