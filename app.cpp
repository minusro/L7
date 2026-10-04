// app.cpp — see app.h
#include "app.h"
#include "ui.h"

m8::Board board;

float bpm = 120.0f;
int selected = NUM_TRACKS - 1;              // row: a track, or STATUS_ROW (from track 1, above the status row)
int selectedCell = 0;                       // cell in the row (0: the waveform, ui.h WAVE_CELL)
static uint32_t changedMs = 0;                // when the tempo or the selection last changed
static bool tempoDirty = false;
static int savedRow = -1, savedCell = -1;      // the selection the project holds
static int savedMaster = MASTER_VOLUME_UNITY;   // the master volume session.txt holds
static int savedProject = 0;                    // and the loaded project
static int savedMenu = -1, bootMenu = -1;       // and the menu (its selection, + 1000 open)
static int bootProject = 0;

// ---- Tempo: MIN_BPM-MAX_BPM, whatever the loops were recorded at. It's the
// project's (all its scenes); new projects take the last one set by hand.
static float newBpm = 120.0f;

static bool changeTempo(float b) {
  b = constrain(roundf(b * 10) / 10, MIN_BPM, MAX_BPM);   // tenths of a BPM
  if (recording.mode != Recording::Idle) return false;   // a recording keeps its tempo
  uint32_t spb = spbForBpm(b);
  if (b == bpm) return true;
  bpm = b;
  transport.pendingSpb = spb;
  changedMs = millis();
  tempoDirty = true;
  return true;
}

bool setTempo(float b) {
  float was = bpm;
  if (!changeTempo(b)) return false;
  if (bpm != was) {
    newBpm = bpm;                                  // new projects start with it
    projectTempoChanged();                         // the loaded project keeps it
  }
  return true;
}

bool loadTempo(float b) { return changeTempo(b); }
float newProjectTempo() { return newBpm; }

// ---- Master volume (steps, config.h): kept in the session
void stepMasterVolume(int by) {
  setMasterVolume(masterVolume() + by);
  changedMs = millis();
}

// ---- Session (tempo, master volume, the loaded project); the selection is the project's
void beginSession() {
  Session s = loadSession();
  bpm = roundf(s.bpm * 10) / 10;
  newBpm = roundf(s.newBpm * 10) / 10;
  if (s.row >= 0 && s.row < ROWS && s.cell >= 0 && s.cell < CELLS) {   // (saved before projects kept it)
    selected = s.row;
    selectedCell = s.cell;
  }
  setMasterVolume(s.master);
  savedMaster = s.master;
  savedProject = bootProject = s.project;
  savedMenu = bootMenu = s.menu;
}

int sessionProject() { return bootProject; }
int sessionMenu() { return bootMenu; }

void takeProjectSelection(bool orTrack1) {
  int row, cell;
  projectSelection(row, cell);
  if (!(row >= 0 && row < ROWS && cell >= 0 && cell < CELLS)) {   // none saved: track 1's waveform, or as it is
    mainSelection(row, cell);
    if (orTrack1) {
      row = NUM_TRACKS - 1;
      cell = WAVE_CELL;
    }
  }
  setMainSelection(row, cell);
  savedRow = row;
  savedCell = cell;
  projectSetSelection(row, cell);
}

static bool sessionDirty() {
  return tempoDirty || masterVolume() != savedMaster || projectCurrent() != savedProject || menuState() != savedMenu;
}

static void writeSession() {
  Session s;
  s.bpm = bpm;
  s.newBpm = newBpm;
  s.master = masterVolume();
  s.project = projectCurrent();
  s.menu = menuState();
  saveSession(s);
  savedMenu = s.menu;
  savedMaster = s.master;
  savedProject = s.project;
  tempoDirty = false;
}

// The selection (the looper's, not the menu's) into the project.
static void keepSelection() {
  int row, cell;
  mainSelection(row, cell);
  if (row == savedRow && cell == savedCell) return;
  projectSetSelection(row, cell);
  savedRow = row;
  savedCell = cell;
}

void saveSessionWhenIdle() {
  static int row = -1, cell = -1, menu = -1;      // the selection and the menu last seen here
  int r, c;
  mainSelection(r, c);
  if (r != row || c != cell || menuState() != menu) {
    row = r;
    cell = c;
    menu = menuState();
    changedMs = millis();
  }
  if (millis() - changedMs > 2000) keepSelection();   // (written to the project when the buffers allow)
  if (sessionDirty() && !transport.running && millis() - changedMs > 2000) writeSession();
}

void saveSessionNow() {
  keepSelection();
  if (sessionDirty()) writeSession();
}
