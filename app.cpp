// app.cpp — see app.h
#include "app.h"

m8::Board board;

float bpm = 120.0f;
int selected = 0;                           // row: a track, or STATUS_ROW
int selectedCell = 0;                       // cell in the row (0: the waveform, ui.h WAVE_CELL)
static uint32_t tempoChangedMs = 0;
static bool tempoDirty = false;

// ---- Tempo: MIN_BPM-MAX_BPM, whatever the loops were recorded at
bool setTempo(float b) {
  b = constrain(roundf(b * 10) / 10, MIN_BPM, MAX_BPM);   // tenths of a BPM
  if (recording.mode != Recording::Idle) return false;   // a recording keeps its tempo
  uint32_t spb = spbForBpm(b);
  if (b == bpm) return true;
  bpm = b;
  transport.pendingSpb = spb;
  tempoChangedMs = millis();
  tempoDirty = true;
  sceneStateChanged();                             // the current scene follows
  return true;
}

void saveTempoWhenIdle() {
  if (tempoDirty && !transport.running && millis() - tempoChangedMs > 2000) {
    saveTempo(bpm);
    tempoDirty = false;
  }
}

void saveTempoNow() {
  if (tempoDirty) saveTempo(bpm);
  tempoDirty = false;
}
