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
static int savedMaster = MASTER_VOLUME_UNITY;   // the master volume session.txt holds
static int savedProject = 0;                    // and the loaded project
static int savedMenu = -1, bootMenu = -1;       // and the menu (its selection, + 1000 open)
static int bootProject = 0;
static bool savedPlaying = false, bootPlaying = false;   // the clock running, as saved / at boot

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

// ---- Master volume (steps, config.h): the app's, one for all projects (kept in the session).
void stepMasterVolume(int by) {
  setMasterVolume(masterVolume() + by);
  changedMs = millis();
}


// ---- Metronome (app.h)
static int metroMode = METRO_AUTO, savedMetro = METRO_AUTO;
int metronomeMode() { return metroMode; }
void setMetronomeMode(int mode) {
  if (mode < METRO_OFF || mode > METRO_ON) return;
  metroMode = mode;
  changedMs = millis();
}
bool silentScene() {
  for (int t = 0; t < NUM_TRACKS; t++)
    if (tracks[t].state == TrackState::Playing) return false;
  return true;
}
int nextMetronomeMode() {
  bool silent = silentScene();
  switch (metroMode) {
    case METRO_ON: return silent ? METRO_OFF : METRO_AUTO;
    case METRO_AUTO: return silent ? METRO_OFF : METRO_ON;
    default: return silent ? METRO_AUTO : METRO_ON;
  }
}
bool metronomeWanted() {
  if (projectCurrent() < 0) return false;          // (the dummy project: never)
  return metroMode == METRO_ON || (metroMode == METRO_AUTO && silentScene());
}

// ---- Session (tempo, master volume, the loaded project, the selection: the
// app's, not a project's or a scene's)
void beginSession() {
  Session s = loadSession();
  bpm = roundf(s.bpm * 10) / 10;
  newBpm = roundf(s.newBpm * 10) / 10;
  if (s.row >= 0 && s.row < ROWS && s.cell >= 0 && s.cell < maxCells(s.row)) {
    selected = s.row;
    selectedCell = s.cell;
  }
  savedRow = selected;
  savedCell = selectedCell;
  setMasterVolume(s.master);
  savedMaster = s.master;
  savedProject = bootProject = s.project;
  savedPlaying = bootPlaying = s.playing;
  savedMenu = bootMenu = s.menu;
  if (s.metro >= METRO_OFF && s.metro <= METRO_ON) metroMode = savedMetro = s.metro;
}

int sessionProject() { return bootProject; }
int sessionMenu() { return bootMenu; }
bool sessionPlaying() { return bootPlaying; }

static bool sessionDirty() {
  int row, cell;
  mainSelection(row, cell);                        // (the looper's, not the menu's)
  return tempoDirty || masterVolume() != savedMaster || projectCurrent() != savedProject ||
         menuState() != savedMenu || transport.running != savedPlaying || row != savedRow || cell != savedCell ||
         metroMode != savedMetro;
}

static void writeSession() {
  Session s;
  s.bpm = bpm;
  s.newBpm = newBpm;
  s.master = masterVolume();
  s.project = projectCurrent();
  s.menu = menuState();
  s.playing = transport.running;
  mainSelection(s.row, s.cell);
  s.metro = metroMode;
  saveSession(s);
  savedMetro = s.metro;
  savedRow = s.row;
  savedCell = s.cell;
  savedMenu = s.menu;
  savedPlaying = s.playing;
  savedMaster = s.master;
  savedProject = s.project;
  tempoDirty = false;
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
  if (sessionDirty() && !transport.running && millis() - changedMs > 2000) writeSession();
}

void saveSessionNow() {
  if (sessionDirty()) writeSession();
}
