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
// Vertical layout: rows every 30 px (15 blocks): the 7 tracks from the top
// of the screen, track 1 first (row NUM_TRACKS - 1) down to track 7 (row 0, just
// above the status row), then the status row 4 px (two blocks) lower.
constexpr int ROW_BAND = 30;                             // a row's height (its button's)
constexpr int STATUS_TOP = NUM_TRACKS * ROW_BAND + 2 * PX;   // 214
// Each track row: three cells, then the button (a square): the waveform, the
// filter (its response curve), then volume and balance. A cell is its
// content in a one-block border, shared with the cells above and below (a
// cell's rect is CELL_H tall: its row's band and the block below, the next
// row's border) and with the cells right next to it: the selection frame is
// drawn there, grey round the selected cell (ui.cpp, borderAt()). The
// selected track gets a grey panel in its button, behind its number
// (ui_buttons.cpp). A block's space before the buttons.
//   waveform: x 0-187   filter: x 188-247   volume: x 250-297   button: x 298-319
//   (a block of space between filter and volume; volume up to the button)
//   (the track cells side by side, their own borders each; the tracks'
//   buttons TRACK_BUTTON_SHIFT right of the status row's)
struct Cell { int x, w; };
constexpr int CELL_H = ROW_BAND + PX;
// The status row's cells: to the screen's bottom edge, framed all round (no
// row below shares their bottom border).
constexpr int STATUS_H = 240 - STATUS_TOP;               // y 214-239: the tags a block from the frame each way
constexpr int CELLS = 3, WAVE_CELL = 0, FILTER_CELL = 1, MIX_CELL = 2;   // left to right
constexpr Cell CELL[CELLS] = {{0, 188}, {188, 60}, {250, 48}};
constexpr int TRACK_BUTTON_SHIFT = 2 * PX;           // (their content cut by the screen's edge)
constexpr int BUTTON_SIZE = ROW_BAND, BUTTON_X = 294, BUTTON_W = 320 - BUTTON_X;
constexpr int WAVE_X = CELL[WAVE_CELL].x, WAVE_W = CELL[WAVE_CELL].w;
// The status row below the tracks: three cells (to the volume cells' edge), left
// to right: the scene (the widest, from the screen's left edge), the clock
// (its width is the master volume's 22 lines) and the tempo. Neighbouring
// cells share the side between them.
constexpr int STATUS_ROW = NUM_TRACKS, ROWS = NUM_TRACKS + 1, MENU_CELL = 0, SCENE_CELL = 1, CLOCK_CELL = 2, TEMPO_CELL = 3;
// (MENU_CELL: no cell any more, nothing in its place: kept, empty, so the others keep their numbers.)
constexpr Cell MENU_BOX = {0, 0}, SCENE_BOX = {0, 120}, CLOCK_BOX = {118, 94}, TEMPO_BOX = {210, 80};
// ---- What the rows show of each track (ui.cpp): a copy of its looks, taken
// as they're drawn. While a change is on its way to any track (a scene's, a
// project's, a move: from its request until its switch point has passed, the
// stream having taken it on; some of a track's data changes ahead of that),
// the tracks it's on its way to keep their copy; once none is on its way any
// more, they all take theirs together and their rows are redrawn in the same
// pass: the old scene's looks go and the new one's come at one moment (what's
// the same in both stays). The other tracks follow at once.
struct ShownTrack {
  TrackState state;
  int8_t volume, balance, filterLo, filterHi;
  uint8_t overviewVersion;
  uint32_t loopBars;
  uint32_t rs, rb;                                 // the range shown (trackRange(): a queued one)
  uint32_t rangeStartBeat, rangeBeats;             // the range playing (the playhead's)
  int32_t rangeAnchorQ;
  uint8_t overview[OVERVIEW_COLS];
};
const ShownTrack &shownTrack(int i);
// An empty track (no clip, not recording) has one cell spanning the whole
// row (SPAN: the cells' width; drawn by drawSpan()): its outline round all of
// it. The column selected is kept for the rows with three.
constexpr Cell SPAN = {0, CELL[CELLS - 1].x + CELL[CELLS - 1].w};
inline bool spanRow(int i) {
  return i >= 0 && i < NUM_TRACKS && shownTrack(i).state == TrackState::Empty &&
         !(recording.track == i && recording.mode != Recording::Idle);
}
// The cells a row has (the status row's: as many as a track row's; an empty track's: one).
inline int cellCount(int i) { return i == STATUS_ROW ? TEMPO_CELL + 1 : spanRow(i) ? 1 : CELLS; }
// The most a row ever has (for a saved selection).
inline int maxCells(int i) { return i == STATUS_ROW ? TEMPO_CELL + 1 : CELLS; }
// A track row's cell's place (its x and width).
inline const Cell &cellRect(int i, int cell) { return spanRow(i) ? SPAN : CELL[cell]; }
// The selected cell in row i's terms (on the status row, never MENU_CELL: none).
inline int selectedCellIn(int i) {
  int c = min(selectedCell, cellCount(i) - 1);
  return i == STATUS_ROW ? max(c, SCENE_CELL) : c;
}
// The leftmost cell selectable in row i.
inline int firstCellIn(int i) { return i == STATUS_ROW ? SCENE_CELL : 0; }

// ---- The looper's two screens, the same rows: main (the tracks from the top
// of the screen, the status row below them) and notes (the status row at the
// top, the tracks below it, a block lower so track 7's bottom border is the
// screen's bottom edge; the same colours).
// UP / DOWN off one's top or bottom edge goes to the other, as if they were
// stacked in a loop: main's status row and notes' meet, and main's track 1 and
// notes' track 7 (input.cpp). LEFT / RIGHT off its sides: the menu, which goes
// back to whichever it came from. notesScreen: notes is the looper's screen
// (the menu open or not); notesLayout: and it's what's shown (the menu
// closed): the rows' places follow it.
inline bool notesScreen = false, notesLayout = false;
inline int rowTop(int i) {
  if (i == STATUS_ROW) return notesLayout ? 0 : STATUS_TOP;
  return (notesLayout ? STATUS_H + PX : 0) + (NUM_TRACKS - 1 - i) * ROW_BAND;
}
// A row's place on the screen from the top (0: the top row), and back.
inline int rowPos(int i) {
  if (notesLayout) return i == STATUS_ROW ? 0 : NUM_TRACKS - i;
  return i == STATUS_ROW ? NUM_TRACKS : NUM_TRACKS - 1 - i;
}
inline int rowAtPos(int p) {
  if (notesLayout) return p == 0 ? STATUS_ROW : NUM_TRACKS - p;
  return p == NUM_TRACKS ? STATUS_ROW : NUM_TRACKS - 1 - p;
}
// The row above / below row i on the screen (-1: none; the status row has a gap next to it).
inline int rowAbove(int i) { return i < NUM_TRACKS - 1 ? i + 1 : -1; }
inline int rowBelow(int i) { return i > 0 && i != STATUS_ROW ? i - 1 : -1; }
// A row's backgrounds: its band; main: the track row above the status row's
// out to it, the status row's out to the screen's bottom edge; notes: the
// first track row's from the status row's bottom, the last's to the screen's
// bottom edge.
constexpr int LAST_TRACK_ROW = 0;
inline int bgTop(int i) { return notesLayout && i == NUM_TRACKS - 1 ? STATUS_H : rowTop(i); }
inline int bgHeight(int i) {
  if (notesLayout) return (i == STATUS_ROW ? STATUS_H : i == LAST_TRACK_ROW ? 240 : rowTop(i) + ROW_BAND) - bgTop(i);
  return (i == STATUS_ROW ? 240 : i == LAST_TRACK_ROW ? STATUS_TOP : rowTop(i) + ROW_BAND) - bgTop(i);
}

inline bool recordingOn(int i) { return recording.track == i && recording.mode != Recording::Idle; }

// The track has a finished clip: until its recording is completely done, its
// cells and button show it as empty (and its controls don't act).
inline bool hasClip(int i) { return tracks[i].state != TrackState::Empty && !recordingOn(i); }
// The same for what the row shows (shownTrack()).
inline bool shownClip(int i) { return shownTrack(i).state != TrackState::Empty && !recordingOn(i); }


// Background of a cell in row i (cell -1: the button): black, but the accent colour
// for the status row's button (the action button; ui_buttons.cpp has its
// colours) and the accent colour for a recording track's waveform cell.
// The selected row's ground: a dark grey rectangle under it, as tall as the
// selection frame (a track's band and the block below, the row below's top
// border; the row above's bottom border is its top; the status row's cells'
// height), the screen's whole width (under the button too, round its panel;
// the status row's from the scene cell's left edge to the tempo cell's right
// one, all its corners cut a block: not under the button).
// Black elsewhere.
bool menuOpen();
bool screenSwitchPending();               // (input.cpp) an arrow held that switches screens once let go
// (An arrow held off an edge, the screen about to switch: none.)
uint8_t arrivalEdgeKey();                // (input.cpp: just back from the menu, its arrow held)
// (An edge line shown, either way: none.)
// Row i shows as selected: the selected one, but while an edge line's shown
// (it then looks as the others).
inline bool shownSelected(int i) { return i == selected && !menuOpen() && !screenSwitchPending() && !arrivalEdgeKey(); }
inline uint16_t rowGround(int i) { return i >= 0 && i < ROWS && shownSelected(i) ? DARK : BG; }
// Something drawn dark grey that would vanish on it: black there instead.
inline uint16_t onGround(uint16_t color, uint16_t ground) { return ground == DARK && color == DARK ? BG : color; }
inline uint16_t cellBackground(int i, int cell) {
  if (cell == WAVE_CELL && recordingOn(i)) return ACCENT;
  if (cell == -1 && i == STATUS_ROW) return ACCENT;
  return cell >= 0 ? rowGround(i) : BG;
}
// Its frame: light grey round the selected cell (grey while B is held, with
// any other keys or none: adjusting; white while only arrows are held and the
// last press moved the selection: not at an edge), none elsewhere (BG).
bool menuOpen();
bool menuOnStatus();
bool arrowsMoved();                      // input.cpp: the last arrow press moved the selection
bool screenSwitchPending();               // (input.cpp) an arrow held that switches screens once let go
bool tempoNudging();                      // (input.cpp) SHIFT+LEFT/RIGHT held on the status row: the tempo nudged
uint32_t lastTapMs();                     // (input.cpp) millis() of the latest tap tempo tap
// While an arrow is held off an edge that switches screens once let go: a
// light grey line a block thick (both screens alike) along that edge
// of the screen (on top of everything; let go without switching, what was under it again).
void drawEdgeLine();
// The arrows' modifier (input.cpp): SHIFT or B, whichever of the two held was
// pressed last (0: neither). They're exclusive for the arrows and the looks
// (not the action button's): pressing the other while one is held switches
// to its arrows; letting it go, back to the first, still held.
uint8_t arrowModifier();
// The keys held as the looper's looks follow them (input.cpp): A, B and SHIFT
// pressed in the menu left out until let go (the menu: all of them).
uint8_t mainKeys();
inline bool shiftArrows() { return arrowModifier() == m8::BTN_SHIFT; }
inline bool bArrows() { return arrowModifier() == m8::BTN_OPT; }
// The cell framed in row i (the selected row): its selected cell; a track's
// with SHIFT the arrows' modifier, its waveform cell (SHIFT+LEFT/RIGHT step
// its sample, from any of its cells), in the accent colour, the other hidden.
inline int frameCell(int i) {
  return i != STATUS_ROW && shiftArrows() && !recordingOn(i) ? WAVE_CELL : selectedCellIn(i);
}
inline uint16_t cellFrame(int i, int cell) {
  if (i != selected || cell != frameCell(i) || (menuOpen() && !menuOnStatus())) return BG;   // (the menu's items have their own)
  if (screenSwitchPending() || arrivalEdgeKey()) return BG;   // (an edge line shown: no frame meanwhile)
  uint8_t k = board.keys();
  const uint8_t arrows = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  if (i != STATUS_ROW && shiftArrows() && !recordingOn(i)) return ACCENT;   // (SHIFT: its waveform cell)
  return bArrows() ? DIM : k && !(k & ~arrows) && arrowsMoved() ? INK : LIGHT;
}
// B the arrows' modifier (A and START not held; arrows may be down).
inline bool bAlone() {
  return bArrows() && !(mainKeys() & (m8::BTN_EDIT | m8::BTN_PLAY));
}
// The cell is being edited: it's the selected one and B is the arrows'
// modifier (B + arrows change it). Its markings then show in the accent colour.
inline bool editingCell(int i, int cell) {
  return shownSelected(i) && cell == selectedCellIn(i) && bArrows();
}
// The colour of block (r, c) of row i's cell (rows x cols blocks, its border
// included) if it's on the border (corners: always BG), from its own frame or
// the neighbour's across that side; -1: not on the border (content).
int borderAt(int i, int cell, int r, int c, int rows, int cols);
// The four sides' colours together (a cell redraws when they change).
uint64_t bordersOf(int i, int cell);
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

// Bumped when the whole screen must be redrawn (the menu opening or closing):
// every part redraws in full when its last draw was of another generation.
extern uint32_t uiGeneration;

// ---- The menu (ui_menu.cpp): the looper's other screen, to its left, the
// whole screen (no action button), opened with any arrow off the looper's
// edges, left by loading a project (or RIGHT off its
// own right edge, or UP off its top); the arrows move its
// selection. While it's open the track rows aren't drawn.
// Its ground: dark grey (the whole screen).
constexpr uint16_t MENU_BG = GREY_17;                // (the menu: always the normal greys)
bool menuOpen();
// (Off an edge with arrow `key`: landing on its far edge before it's first
// drawn, menuLand(); closeMenu(false): the looper's rows not drawn yet, the
// caller moves the selection first. Drawn once, at the landing place: no flicker.)
void openMenu(uint8_t key = 0);
void closeMenu(bool draw = true);
int menuFirstEmptySlot();                // the first slot with no project (-1: none)
float menuCutProgress(int &slot);        // (input.cpp) SHIFT+A held on a project's card: it (slot; -1: none) and how far to its cut (0-1)
uint32_t menuCardPressMs();              // (input.cpp) millis() of the latest A or B press on a card (0: none)
bool menuJustCut();                      // (input.cpp) a project just cut by holding, its keys still held
void menuSelectSlot(int k);              // the menu's selection on card k
void menuNoteLoad(int from);             // a project just loaded from the menu, `from` the one before: its card flashing until it comes in
bool menuLoadPending();                  // a project loaded from the menu hasn't come in yet (playing: its switch point): the menu can't be left meanwhile
// Switching screens, each one's selection is where it was last left; opened
// off the looper's edge (key: the arrow), menuLand() puts the menu's on its far
// edge (UP: its bottom item; DOWN: its top card row, at its own column;
// LEFT/RIGHT: its right / left side, in its own row).
void menuLand(uint8_t key);
bool menuLeavesOn(uint8_t key);          // that arrow leaves the menu: RIGHT / LEFT off its sides (a card row's end, a list item), UP off its top (the top card row) or DOWN off its bottom (the list's last item); a project loaded
void drawMenu();                         // what changed (all of it after uiGeneration moves on)
bool menuKeys(uint8_t pressed);          // the arrows pressed, its selection on the menu's items (true: it moved)
bool menuOnStatus();                     // open, its selection on the status row (selected = STATUS_ROW)
void menuLeaveStatus();                  // from the status row back up to the menu's items
int menuSlot();                          // the project slot selected in the menu (0-11), or -1
bool menuOnList();                       // the selection on the menu's list (EXPORT ABLETON PROJECT ...)
uint16_t menuActionColor();
uint16_t actionRestColor();              // the action button's colour when not pressed (ui_buttons.cpp; grey: it can't act)              // the action button's colour (the accent, a card's, light grey)
int menuListItem();                      // which of its items (0: export, 1: storage mode, 2: settings), or -1
int menuListX();                         // the list's left edge, px (storage mode's texts start there too)
bool menuExportShown();                  // the export item shows its label (exporting is off until it goes)
void menuExportStatus(const char *text, int nameAt = -1);   // the export item's label ("EXPORTING 3/14"; nullptr: its own), kept until the
                                                            // project changes or another is loaded (a failure's: until the menu closes);
                                                            // from character nameAt on, in the project's colour (-1: none)
// The menu's state for the session: its selection (an item, or 100 + the
// status row's cell), + 1000 while it's open. restoreMenu() puts it back
// (opening it if it was open; at boot).
int menuState();
void restoreMenu(int state);
void mainSelection(int &row, int &cell); // the looper's selection (kept while the menu is open)
void setMainSelection(int row, int cell);   // and set (the menu open: for when it closes)

// Splash screen (ui_splash.cpp): the logo while the looper starts. hideSplash()
// waits until it has been up a moment, then clears the screen (the menu follows).
void showSplash();
void hideSplash();
void splashIntro();                      // the intro (the slide, the hue cycle, the copyright line's rise) played through, after showSplash()
void splashTick();                       // the logo's colour cycle: call while starting up (hideSplash() finishes it)
void splashLoading();
void splashHideLine();                   // the line under the logo gone (the start-up beginning)                    // the start-up beginning: "Loading..." in the copyright line's place
uint16_t hueCycle(float phase);          // the logo's hue at a phase (0-1) of one turn round the hues
int splashBottom();                    // y just below the logo (for startup messages)
void drawSmallLogo(int x, int y, uint16_t color);   // the logo small, 55 x 26 px (the menu's corner), top left at (x, y)
// The corner logo's place (the menu's, storage mode's): bottom left, a block
// in from the left edge, on the status row's line (its height).
constexpr int CORNER_LOGO_X = PX, CORNER_LOGO_Y = STATUS_TOP;

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
