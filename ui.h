// ui.h — the screen: layout, and drawing each row's cells. Everything is drawn
// over its own background (no full clears, which would hold up SD streaming);
// each part redraws only what changed.
#pragma once
#include "app.h"

// ---- Screen (drawn over its own background: no full clears, which would
// hold up SD streaming)
// Vertical layout: 9 equal 26 px rows (234 px, 3 px padding above and below):
// the 8 tracks and the status row. Each row's band fills its slice.
// (26 px is the most that fits: 9 rows of 32 px would need 288 px.)
constexpr int SLICES = NUM_TRACKS + 1, SLICE_H = 26, ROW_BAND = 26;
constexpr int LAYOUT_TOP = (240 - SLICES * SLICE_H) / 2;
constexpr int HEADER_SCALE = 1;                       // text size of the status row
constexpr int TEXT_H = 8 * HEADER_SCALE;
constexpr int sliceTop(int k) { return LAYOUT_TOP + k * SLICE_H; }
constexpr int sliceCentredTop(int k, int h) { return sliceTop(k) + (SLICE_H - h) / 2; }
constexpr int TEXT_Y = sliceCentredTop(NUM_TRACKS, TEXT_H);
// Each track row: three cells, then the button (icon only on the selected
// track), separated by 1 px gaps: the waveform, the filter (its response
// curve), then volume and balance. On the selected row the
// current cell and the button get the selection highlight.
//   waveform cell: x 0-134   filter: x 136-214   volume: x 216-294   button: x 296-319
// The waveform fills its cell's whole width.
struct Cell { int x, w; };
constexpr int CELLS = 3, WAVE_CELL = 0, FILTER_CELL = 1, MIX_CELL = 2;   // left to right
constexpr Cell CELL[CELLS] = {{0, 135}, {136, 79}, {216, 79}};
constexpr int BUTTON_SIZE = ROW_BAND, BUTTON_X = 296, BUTTON_W = 320 - BUTTON_X;
constexpr int WAVE_X = CELL[WAVE_CELL].x, WAVE_W = CELL[WAVE_CELL].w, WAVE_H = 16;   // waveform: x 0-134
// The status row below the tracks has the same cells; its first, under the
// waveforms, holds the tempo (left-aligned with the waveforms, adjustable)
// and the clock (right-aligned with them).
constexpr int STATUS_ROW = NUM_TRACKS, ROWS = NUM_TRACKS + 1, TEMPO_CELL = WAVE_CELL;

inline int rowTop(int i) { return sliceCentredTop(i, ROW_BAND); }
// A row's backgrounds: its band, the first and last rows also out to the
// screen's top and bottom edges.
inline int bgTop(int i) { return i == 0 ? 0 : rowTop(i); }
inline int bgHeight(int i) { return (i == ROWS - 1 ? 240 : rowTop(i) + ROW_BAND) - bgTop(i); }
// The divider above the status row: the last track row's bottom line, drawn
// over its unhighlighted cells (a highlight covers it).
constexpr int DIVIDER_ROW = NUM_TRACKS - 1;
inline int dividerY() { return rowTop(DIVIDER_ROW) + ROW_BAND - 1; }

inline bool recordingOn(int i) { return recording.track == i && recording.mode != Recording::Idle; }

// Background of a cell in track row i (cell -1: the button). The selected row
// highlights its current cell; its other cells and the button get a darker
// grey; a recording track's waveform cell is dark red.
// The track has a finished clip: until its recording is completely done, its
// cells and button show it as empty (and its controls don't act).
inline bool hasClip(int i) { return tracks[i].state != TrackState::Empty && !recordingOn(i); }

inline uint16_t cellBackground(int i, int cell) {
  if (cell == WAVE_CELL && recordingOn(i)) return BG_RECORDING;
  if (i == STATUS_ROW && cell == -1) return BG_SELECTED;   // its button: always highlighted
  if (i != selected) return BG;
  return (cell != -1 && cell == selectedCell) ? BG_SELECTED : BG_ROW;
}

// Bumped when the whole screen must be redrawn (after the panel closes):
// every part redraws in full when its last draw was of another generation.
extern uint32_t uiGeneration;

// ---- Panel (ui_panel.cpp): a centred box over the dimmed screen, opened
// with B+A and closed with A. While it's open the rows aren't drawn.
bool panelOpen();
void openPanel();
void closePanel();

// ---- Drawing (ui*.cpp)
void drawTrack(int i);                   // ui.cpp: row i (a track or the status row), whatever changed
void uiRefresh();                        // ui.cpp: call from loop(): a little more of the screen every 30 ms
void drawCells(int i);                   // ui_buttons.cpp: the row's button and status cells
void noteActionPress();                  // ui_buttons.cpp: A pressed (before it acts): its icon dims while held
void drawWave(int i, bool recordingHere);   // ui_wave.cpp
void updatePlayheads();                  // ui_wave.cpp: playheads, record head, live recording
void drawFilter(int i);                  // ui_filter.cpp
void drawMix(int i);                     // ui_mix.cpp (also the meters)
void drawStatus();                       // ui_status.cpp: tempo and clock
