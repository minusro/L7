// app.cpp — see app.h
#include "app.h"
#include "ui.h"
#include "midi_in.h"
#include "audio_follow.h"

m8::Board board;

float bpm = 120.0f;
int selected = NUM_TRACKS - 1;              // row: a track, or STATUS_ROW (from track 1, above the status row)
int selectedCell = 0;                       // cell in the row (0: the waveform, ui.h WAVE_CELL)
static uint32_t changedMs = 0;                // when the tempo or the selection last changed
static bool tempoDirty = false;
static int savedRow = -1, savedCell = -1;      // the selection session.txt holds
static int savedMaster = MASTER_VOLUME_UNITY, savedBalance = 0;
static int16_t savedOffsets[SYNC_SOURCES] = {};       // the external clocks' offsets session.txt holds   // the master volume and balance session.txt holds
static int savedProject = 0;                    // and the loaded project
static int savedMenu = -1, bootMenu = -1;       // and the menu (its selection, + 1000 open)
static int bootProject = 0;
static bool savedPlaying = false, bootPlaying = false;   // the clock running, as saved / at boot
static bool savedFollow = false;                // audio follow, as saved

// ---- Tempo: MIN_BPM-MAX_BPM, whatever the loops were recorded at. It's the
// project's (all its scenes); new projects take the last one set by hand.
static float newBpm = 120.0f;

static bool changeTempo(float b) {
  if (midiClockFollowing() || audioFollowOn()) return false;   // (an external clock's tempo, or the one heard: not by hand, nor a project's)
  b = constrain(roundf(b * 10) / 10, MIN_BPM, MAX_BPM);   // tenths of a BPM
  // (Recording too: the recorder follows the clock's tempo changes, engine.h.)
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

void followTempo(uint32_t spb, float shownBpm) {
  uint32_t now = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  if (spb != now) transport.pendingSpb = spb;      // (exactly: not rounded to tenths of a BPM)
  bpm = constrain(shownBpm, MIN_BPM, MAX_BPM);     // (shown; not the project's, nor saved as set by hand)
}
float newProjectTempo() { return newBpm; }

// ---- Master volume (steps, config.h): the app's, one for all projects (kept in the session).
void stepMasterVolume(int by) {
  setMasterVolume(masterVolume() + by);
  changedMs = millis();
}
void stepMasterBalance(int by) {
  setMasterBalance(masterBalance() + by);
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
int nextMetronomeMode() { return metroMode == METRO_ON ? METRO_OFF : METRO_ON; }
bool metronomeWanted() {
  if (projectCurrent() < 0) return false;          // (the dummy project: never)
  return metroMode == METRO_ON || (setting(SET_EMPTY_CLICK) && !projectHasClips());
}

// ---- Settings (app.h)
static bool settings[SETTING_COUNT] = {true, true, true, true}, savedSettings[SETTING_COUNT] = {true, true, true, true};
bool setting(int which) { return which >= 0 && which < SETTING_COUNT && settings[which]; }
// Where they act: the engine's monitoring, the storage's new clips.
static void applySettings() {
  inputMonitored = settings[SET_MONITOR];
  newClipsPlay = settings[SET_PLAY_NEW];
}
static uint16_t seenPages = 0, savedSeenPages = 0;
static int shownPage = 0, savedShownPage = 0;
uint16_t helpSeen() { return seenPages; }
int helpPage() { return shownPage; }
void setHelpPage(int page) {
  if (page < 0 || page > 15) return;
  shownPage = page;
  seenPages |= 1 << page;
  changedMs = millis();
}
void setSetting(int which, bool on) {
  if (which < 0 || which >= SETTING_COUNT) return;
  settings[which] = on;
  applySettings();
  changedMs = millis();
}

// ---- Session (tempo, master volume, the loaded project, the selection: the
// app's, not a project's or a scene's)
static uint32_t savedTouched[12];                // (touchedSample as last saved)
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
  setMasterBalance(s.balance);
  savedBalance = s.balance;
  savedProject = bootProject = s.project;
  savedPlaying = bootPlaying = s.playing;
  savedMenu = bootMenu = s.menu;
  if (s.metro >= METRO_OFF && s.metro <= METRO_ON) metroMode = savedMetro = s.metro == METRO_ON ? METRO_ON : METRO_OFF;   // (auto: off, the setting below)
  for (int i = 0; i < SETTING_COUNT; i++) settings[i] = savedSettings[i] = s.settings[i];
  applySettings();
  seenPages = savedSeenPages = (uint16_t)s.helpSeen;
  shownPage = savedShownPage = s.helpPage;
  memcpy(touchedSample, s.touched, sizeof touchedSample);
  memcpy(savedTouched, s.touched, sizeof savedTouched);
  audioFollowBegin(s.follow);
  savedFollow = s.follow;
  for (int i = 0; i < SYNC_SOURCES; i++) syncOffsetMs[i] = savedOffsets[i] = (int16_t)s.offsets[i];
}

int sessionProject() { return bootProject; }
int sessionMenu() { return bootMenu; }
bool sessionPlaying() { return bootPlaying; }

static bool sessionDirty() {
  int row, cell;
  mainSelection(row, cell);                        // (the looper's, not the menu's)
  return tempoDirty || masterVolume() != savedMaster || masterBalance() != savedBalance || projectCurrent() != savedProject ||
         menuState() != savedMenu || transport.running != savedPlaying || row != savedRow || cell != savedCell ||
         metroMode != savedMetro || audioFollowOn() != savedFollow || memcmp(settings, savedSettings, sizeof settings) || seenPages != savedSeenPages || shownPage != savedShownPage ||
         memcmp(syncOffsetMs, savedOffsets, sizeof savedOffsets) || memcmp(touchedSample, savedTouched, sizeof savedTouched);
}

static void writeSession() {
  Session s;
  s.bpm = bpm;
  s.newBpm = newBpm;
  s.master = masterVolume();
  s.balance = masterBalance();
  s.project = projectCurrent();
  s.menu = menuState();
  s.playing = transport.running;
  mainSelection(s.row, s.cell);
  s.metro = metroMode;
  s.follow = audioFollowOn();
  for (int i = 0; i < SETTING_COUNT; i++) s.settings[i] = savedSettings[i] = settings[i];
  s.helpSeen = savedSeenPages = seenPages;
  s.helpPage = savedShownPage = shownPage;
  memcpy(s.touched, touchedSample, sizeof s.touched);
  memcpy(savedTouched, touchedSample, sizeof savedTouched);
  for (int i = 0; i < SYNC_SOURCES; i++) s.offsets[i] = savedOffsets[i] = syncOffsetMs[i];
  saveSession(s);
  savedMetro = s.metro;
  savedFollow = s.follow;
  savedRow = s.row;
  savedCell = s.cell;
  savedMenu = s.menu;
  savedPlaying = s.playing;
  savedMaster = s.master;
  savedBalance = s.balance;
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
