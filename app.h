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
// The session: the tempo and the selected row and cell, kept in session.txt.
// beginSession() loads it (none saved: 120 BPM, track 1's waveform cell).
// It's saved a while after it last changed, and only while stopped (saving
// rewrites a file, which can stall the card); saveSessionNow() saves it at
// once if it changed (power off).
void beginSession();
void saveSessionWhenIdle();
void saveSessionNow();
