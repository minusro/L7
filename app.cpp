// app.cpp — see app.h
#include "app.h"
#include "ui.h"

m8::Board board;

float bpm = 120.0f;
int selected = NUM_TRACKS - 1;              // row: a track, or STATUS_ROW (from track 1, above the status row)
int selectedCell = 0;                       // cell in the row (0: the waveform, ui.h WAVE_CELL)
static uint32_t changedMs = 0;                // when the tempo or the selection last changed
static bool tempoDirty = false;
static int savedRow = -1, savedCell = -1;      // the selection session.txt holds

// ---- Tempo: MIN_BPM-MAX_BPM, whatever the loops were recorded at
bool setTempo(float b) {
  b = constrain(roundf(b * 10) / 10, MIN_BPM, MAX_BPM);   // tenths of a BPM
  if (recording.mode != Recording::Idle) return false;   // a recording keeps its tempo
  uint32_t spb = spbForBpm(b);
  if (b == bpm) return true;
  bpm = b;
  transport.pendingSpb = spb;
  changedMs = millis();
  tempoDirty = true;
  sceneStateChanged();                             // the current scene follows
  return true;
}

// ---- Session (tempo, selection)
void beginSession() {
  Session s = loadSession();
  bpm = roundf(s.bpm * 10) / 10;
  if (s.row >= 0 && s.row < ROWS && s.cell >= 0 && s.cell < CELLS) {   // (none saved: track 1's waveform)
    selected = s.row;
    selectedCell = s.cell;
  }
  savedRow = selected;
  savedCell = selectedCell;
}

static bool sessionDirty() { return tempoDirty || selected != savedRow || selectedCell != savedCell; }

static void writeSession() {
  Session s;
  s.bpm = bpm;
  s.row = selected;
  s.cell = selectedCell;
  saveSession(s);
  tempoDirty = false;
  savedRow = selected;
  savedCell = selectedCell;
}

void saveSessionWhenIdle() {
  static int row = -1, cell = -1;                 // the selection last seen here
  if (selected != row || selectedCell != cell) {
    row = selected;
    cell = selectedCell;
    changedMs = millis();
  }
  if (sessionDirty() && !transport.running && millis() - changedMs > 2000) writeSession();
}

void saveSessionNow() {
  if (sessionDirty()) writeSession();
}
