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
void transportRestart();             // from bar 1 again, running or not

// Record button for track t: start (or arm until the next bar), then stop.
// Returns false if the track already holds a loop.
bool recordPress(int t);
void clearTrack(int t);
// Loop range in beats: starts on any beat of the loop; the length must be a
// power of two (1 beat = 1/4 bar up to the whole loop). It may run past the
// loop's end and carry on from its start. While playing, the change is queued and takes over at
// the next quarter beat the track hasn't buffered yet (usually within a beat);
// trackRange() reports the queued range if any.
bool setRange(int t, uint32_t startBeat, uint32_t beats);
void trackRange(int t, uint32_t &startBeat, uint32_t &beats);
// The range's start position (a beat of the loop): the first of the
// lowest-order power-of-two divisions of the loop in the range, where the
// loop's start and middle rank the same (then its quarters, eighths, ...).
// The transport starting at 1.1.1 plays each track from here.
uint32_t startPosition(uint32_t loopBeats, uint32_t rangeStart, uint32_t rangeBeats);

float loadTempo();                   // saved tempo, or 120
void saveTempo(float bpm);           // creates/rewrites a file: only while stopped
void trackChanged(int t);            // settings changed: header saved when buffers allow
void flushHeaders();                 // save all pending headers now (power off)

// Live waveform of the recording in progress (OVERVIEW_COLS columns over
// `bars` bars: the final loop length once stopped, before that the smallest
// power of two that holds what's recorded). false = not recording.
bool recordingWaveform(uint8_t *cols, uint32_t &bars, uint32_t &framesPerBar, uint32_t &recordedFrames);

// Debugging: the step loop() is in, for the freeze detector in the sketch.
extern volatile const char *debugStage;

}  // namespace looper
