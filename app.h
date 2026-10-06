// app.h — what the sketch's parts share: the board, the tempo and the
// selection (row and cell).
#pragma once
#include "M8Board.h"
#include "engine.h"
#include "storage.h"
#include "colors.h"

using namespace looper;

extern m8::Board board;

extern float bpm;
extern int selected;                               // row: a track, or STATUS_ROW (ui.h)
extern int selectedCell;                           // cell in the row (ui.h: FILTER_CELL ...)

// In tenths of a BPM, limited to MIN_BPM-MAX_BPM. false (and no change)
// while recording. setTempo: by hand (the loaded project keeps it, and new
// projects start with it); loadTempo: a project's, as it loads (neither).
bool setTempo(float b);
bool loadTempo(float b);
float newProjectTempo();                 // the last tempo set by hand (saved with the session; first 120)
// The master volume a step up (by 1) or down (-1); saved with the session.
void stepMasterVolume(int by);
// The metronome (saved with the session): off (0), auto (A: only in a
// silent scene, no unmuted clip playing) or on (1), while the clock runs;
// never in the dummy project. SHIFT+A on the tempo cell goes to the next
// setting: from on or auto, off in a silent scene, else the other one of
// the two; from off, auto in a silent scene, else on (so it always changes
// what's heard).
enum { METRO_OFF, METRO_AUTO, METRO_ON };
int metronomeMode();
int nextMetronomeMode();
void setMetronomeMode(int mode);
bool silentScene();                      // no unmuted clip playing in the current scene
bool metronomeWanted();                  // the click, by the setting now (a recording keeps its own: m8_looper.ino)
// The session: the tempo, the master volume, the loaded project and the
// selected row and cell (the app's: loading a project or changing scene
// leaves it as it is), kept in SESSION.DAT. beginSession() loads it (none
// saved: 120 BPM, project 1, track 1's waveform). It's saved a while after it
// last changed, and only while stopped (saving rewrites a file, which can
// stall the card); saveSessionNow() saves it at once if it changed (power off).
void beginSession();
int sessionProject();                              // the project to load at boot
int sessionMenu();                                 // the menu as it was (ui_menu.cpp menuState(); -1: none saved)
bool sessionPlaying();                             // the clock was running when the session was last saved
void saveSessionWhenIdle();
void saveSessionNow();
