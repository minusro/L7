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

// In tenths of a BPM, limited to MIN_BPM-MAX_BPM; recording too (the
// recording follows: engine.h's tempo marks). setTempo: by hand (the loaded project keeps it, and new
// projects start with it); loadTempo: a project's, as it loads (neither).
bool setTempo(float b);
bool loadTempo(float b);
// An external clock's tempo (midi_in.cpp): spb exactly (nudged to its beat),
// shownBpm the tempo shown.
void followTempo(uint32_t spb, float shownBpm);
float newProjectTempo();                 // the last tempo set by hand (saved with the session; first 120)
// The master volume a step up (by 1) or down (-1); saved with the session.
void stepMasterVolume(int by);
void stepMasterBalance(int by);                  // the master balance a step right (1) or left (-1); saved too
// The metronome (saved with the session): off or on, while the clock runs;
// off, it still clicks in a project with no clips (none on its tracks or in
// any of its scenes, projectHasClips()) while the "empty project click"
// setting is on; never in the dummy project. B+A on the tempo cell: the other
// one. (METRO_AUTO: an older session's auto, taken as off, the setting
// doing what it did.)
enum { METRO_OFF, METRO_AUTO, METRO_ON };
int metronomeMode();
int nextMetronomeMode();
void setMetronomeMode(int mode);
bool metronomeWanted();                  // the click, by the setting now (a recording keeps its own: m8_looper.ino)
// The settings (the menu's SETTINGS popup; saved with the session), each on
// or off, all on at first: the input monitored (heard in the mix: the line
// out and USB 1-2; recorded either way), a new recording playing once done
// (off: it comes in muted), the selection moving on to the next free track
// after a recording, and the click in a project with no clips (above).
enum { SET_MONITOR, SET_PLAY_NEW, SET_NEXT_FREE, SET_EMPTY_CLICK, SETTING_COUNT };
bool setting(int which);
void setSetting(int which, bool on);
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
