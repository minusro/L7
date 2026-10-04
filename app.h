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
// while recording.
bool setTempo(float b);
// The master volume a step up (by 1) or down (-1); saved with the session.
void stepMasterVolume(int by);
// The session: the tempo, the master volume and the loaded project, kept in
// session.txt. beginSession() loads it (none saved: 120 BPM, project 1).
// It's saved a while after it last changed, and only while stopped (saving
// rewrites a file, which can stall the card); saveSessionNow() saves it at
// once if it changed (power off). The selected row and cell are the loaded
// project's: kept in it a while after they change (and at power off).
void beginSession();
int sessionProject();                              // the project to load at boot
int sessionMenu();                                 // the menu as it was (ui_menu.cpp menuState(); -1: none saved)
// The loaded project's selection taken (after loading it). None saved (a new
// project): track 1's waveform cell if orTrack1, else as it is (at boot: the
// one the session had before projects kept it).
void takeProjectSelection(bool orTrack1 = true);
void saveSessionWhenIdle();
void saveSessionNow();
