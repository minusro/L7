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
// The tempo is saved a while after it last changed, and only while stopped
// (saving rewrites a file, which can stall the card).
void saveTempoWhenIdle();
// Saves it now if it changed (power off).
void saveTempoNow();
