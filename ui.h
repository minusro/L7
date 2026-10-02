// ui.h — the screen: layout, and drawing each row's cells. Everything is drawn
// over its own background (no full clears, which would hold up SD streaming);
// each part redraws only what changed.
#pragma once
#include "app.h"

// ---- Screen (drawn over its own background: no full clears, which would
// hold up SD streaming)
// The look: white on black with two greys and an accent colour (colors.h), everything on a grid of
// PX x PX pixel blocks (a 160 x 120 screen of chunky pixels), a blocky
// pixel font, and the selection shown by frames and inverted boxes.
constexpr int PX = 2;
// Vertical layout: 9 equal 26 px rows (13 blocks; 234 px, 2 px above, 4 below):
// the 8 tracks and the status row. Each row's band fills its slice.
constexpr int SLICES = NUM_TRACKS + 1, SLICE_H = 26, ROW_BAND = 26;
constexpr int LAYOUT_TOP = 2;
constexpr int sliceTop(int k) { return LAYOUT_TOP + k * SLICE_H; }
constexpr int sliceCentredTop(int k, int h) { return sliceTop(k) + (SLICE_H - h) / 2; }
// Each track row: three cells, then the button (a square), separated by
// one-block gaps: the waveform, the filter (its response curve), then volume
// and balance. The selected cell gets a grey frame; the selected track a
// grey line down the screen's right edge (TRACK_MARK_W px, in its button).
//   waveform: x 0-131   filter: x 134-211   volume: x 214-291   button: x 294-319
struct Cell { int x, w; };
constexpr int CELLS = 3, WAVE_CELL = 0, FILTER_CELL = 1, MIX_CELL = 2;   // left to right
constexpr Cell CELL[CELLS] = {{0, 132}, {134, 78}, {214, 78}};
constexpr int BUTTON_SIZE = ROW_BAND, BUTTON_X = 294, BUTTON_W = 320 - BUTTON_X;
constexpr int TRACK_MARK_W = 2 * PX;
constexpr int WAVE_X = CELL[WAVE_CELL].x, WAVE_W = CELL[WAVE_CELL].w;
// The status row below the tracks: its first cell, under the waveforms,
// holds the tempo (left) and the clock (right); the second, spanning the
// filter and volume columns, the scene.
constexpr int STATUS_ROW = NUM_TRACKS, ROWS = NUM_TRACKS + 1, TEMPO_CELL = WAVE_CELL, SCENE_CELL = 1;
constexpr Cell SCENE_BOX = {CELL[1].x, CELL[2].x + CELL[2].w - CELL[1].x};
// The cells a row has (the status row two: the volume column is the scene's).
inline int cellCount(int i) { return i == STATUS_ROW ? SCENE_CELL + 1 : CELLS; }
// The selected cell in row i's terms (on the status row, the volume column
// is the scene's).
inline int selectedCellIn(int i) { return min(selectedCell, cellCount(i) - 1); }

inline int rowTop(int i) { return sliceCentredTop(i, ROW_BAND); }
// A row's backgrounds: its band, the first and last rows also out to the
// screen's top and bottom edges.
inline int bgTop(int i) { return i == 0 ? 0 : rowTop(i); }
inline int bgHeight(int i) { return (i == ROWS - 1 ? 240 : rowTop(i) + ROW_BAND) - bgTop(i); }
// The divider above the status row: a dim line along the last track row's
// bottom block (under a frame: the frame).
constexpr int DIVIDER_ROW = NUM_TRACKS - 1;
inline int dividerY() { return rowTop(DIVIDER_ROW) + ROW_BAND - PX; }

inline bool recordingOn(int i) { return recording.track == i && recording.mode != Recording::Idle; }

// The track has a finished clip: until its recording is completely done, its
// cells and button show it as empty (and its controls don't act).
inline bool hasClip(int i) { return tracks[i].state != TrackState::Empty && !recordingOn(i); }

// Background of a cell in row i (cell -1: the button): black, but inverted
// for the status row's button (white) and the waveform cell of a recording
// track (the accent).
inline uint16_t cellBackground(int i, int cell) {
  if (cell == WAVE_CELL && recordingOn(i)) return ACCENT;
  if (cell == -1 && i == STATUS_ROW) return INK;
  return BG;
}
// Its frame: grey round the selected cell (light grey while B alone is held:
// B + arrows edit it), none elsewhere (BG).
inline uint16_t cellFrame(int i, int cell) {
  if (i != selected || cell != selectedCellIn(i)) return BG;
  return board.keys() == m8::BTN_OPT ? LIGHT : DIM;
}
// The cell is being edited: it's the selected one and B is held (B + arrows
// change it). Its markings then show in the accent colour.
inline bool editingCell(int i, int cell) {
  return i == selected && cell == selectedCell && (board.keys() & m8::BTN_OPT);
}
// Does the frame (one block wide, corners cut) cover pixel (x, y) of a cell
// w x h px? Corner blocks are left out, for rounded corners.
inline bool onFrame(int x, int y, int w, int h) {
  bool l = x < PX, r = x >= w - PX, t = y < PX, b = y >= h - PX;
  return (l || r || t || b) && !((l || r) && (t || b));
}

// ---- Pixel font (ui_font.cpp): 5 blocks tall, mostly 3 wide, a block apart;
// upper case, digits and a few signs. Widths and positions in px.
constexpr int FONT_H = 5 * PX;
int textWidth(const char *text);
// Text at (x, y) (top left), on bg: its box is drawn whole (no flash), as
// wide as the text, or `width` px if wider (the rest bg).
void drawText(int x, int y, const char *text, uint16_t ink, uint16_t bg, int width = 0);
// A tag: text in a box with rounded (cut) corners, a block of space around
// the text and a block of edge: plain (no box), outlined, or inverted (the
// box in ink, the text in bg). TAG_H px tall; at least `width` px wide.
enum TagStyle { TAG_PLAIN, TAG_OUTLINE, TAG_INVERTED };
constexpr int TAG_H = FONT_H + 4 * PX;
void drawTag(int x, int y, const char *text, TagStyle style, uint16_t ink, uint16_t bg, int width = 0);
int tagWidth(const char *text);

// Bumped when the whole screen must be redrawn (after the panel closes):
// every part redraws in full when its last draw was of another generation.
extern uint32_t uiGeneration;

// ---- Panel (ui_panel.cpp): a centred box over the dimmed screen, opened
// with B+A and closed with A. While it's open the rows aren't drawn.
bool panelOpen();
void openPanel();
void closePanel();

// Splash screen (ui_splash.cpp): the logo while the looper starts. hideSplash()
// waits until it has been up a moment, then draws the looper.
void showSplash();
void hideSplash();
int splashBottom();                    // y just below the logo (for startup messages)

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
