// ui.cpp — drawing rows, and the screen refresh pass (see ui.h)
#include "ui.h"
#include "input.h"
#include <utility>

int borderAt(int i, int cell, int r, int c, int rows, int cols) {
  bool l = c == 0, rt = c == cols - 1, t = r == 0, b = r == rows - 1;
  if (!(l || rt || t || b)) return -1;
  // No frame there: the ground (a top or bottom border row is shared with
  // the row above / below: dark grey if either is the selected track).
  uint16_t ground = rowGround(i);
  if (i != STATUS_ROW && (t || b) && rowGround(t ? rowAbove(i) : rowBelow(i)) == DARK) ground = DARK;
  bool corner = (l || rt) && (t || b);             // (corners: cut)
  uint16_t own = cellFrame(i, cell);
  if (i == STATUS_ROW) {                           // (its cells only share sides with each other)
    int nc = l ? cell - 1 : rt ? cell + 1 : cell;
    if (corner) {                                  // all corners cut (the highlight's own outer ones too: black)
      bool outer = (l && cell == SCENE_CELL) || (rt && cell == TEMPO_CELL);
      return outer ? BG : ground;
    }
    if (own != BG) return own;
    if (t || b || nc < 0 || nc >= cellCount(i)) return ground;
    uint16_t theirs = cellFrame(i, nc);
    return theirs != BG ? theirs : ground;
  }
  if (own != BG && !corner) return own;
  if (t || b) {
    // Shared with the row above / below: its selected cell's frame where it
    // runs over this column (not at that cell's own corners: cut there too),
    // by place on the screen (an empty track's one cell spans the row).
    int ni = t ? rowAbove(i) : rowBelow(i);
    if (ni >= 0 && ni == selected) {
      int x = cellRect(i, cell).x / PX + c, nc = selectedCellIn(ni);
      const Cell &n = cellRect(ni, nc);
      int first = n.x / PX, last = (n.x + n.w) / PX - 1;
      if (x > first && x < last) {
        uint16_t theirs = cellFrame(ni, nc);
        if (theirs != BG) return theirs;
      }
    }
    return ground;
  }
  int nc = l ? cell - 1 : cell + 1;                // a side: the cell right next to it, if any
  if (nc < 0 || nc >= cellCount(i)) return ground;
  const Cell &a = cellRect(i, min(cell, nc)), &n = cellRect(i, max(cell, nc));
  if (a.x + a.w - PX != n.x) return ground;        // (only a cell right next to it: no space between)
  uint16_t theirs = cellFrame(i, nc);
  return theirs != BG ? theirs : ground;
}

uint64_t bordersOf(int i, int cell) {
  // A middle block of each side (rows / columns: any cell's are at least 3).
  const int rows = 3, cols = 3;
  uint64_t k = 0;
  for (auto rc : {std::pair<int, int>{1, 0}, {1, 2}, {0, 1}, {2, 1}})
    k = k << 16 | (uint16_t)borderAt(i, cell, rc.first, rc.second, rows, cols);
  // A track's: the selection next to it too (its frame along a shared border
  // row is drawn by place, not by those middle blocks), and its own shape.
  if (i != STATUS_ROW) {
    bool near = selected == rowAbove(i) || selected == rowBelow(i);
    k = k * 31 + (near ? (uint64_t)(selectedCellIn(selected) + 1) << 20 | cellFrame(selected, selectedCellIn(selected)) |
                             (uint64_t)spanRow(selected) << 24 | (uint64_t)(selected == rowAbove(i)) << 25 : 0);
    k = k * 31 + spanRow(i);
  }
  return k;
}

// An empty track's one cell (spanRow()): its border round the whole row and
// the ground inside, a block row at a time.
static void drawSpan(int i) {
  static uint64_t shown[NUM_TRACKS];
  static uint32_t drawn[NUM_TRACKS];
  uint64_t key = bordersOf(i, 0) * 31 + rowGround(i);
  if (drawn[i] == uiGeneration && shown[i] == key) return;
  drawn[i] = uiGeneration;
  shown[i] = key;
  constexpr int COLS = SPAN.w / PX, ROWS_B = CELL_H / PX;
  static uint16_t line[SPAN.w * PX];
  uint16_t ground = rowGround(i);
  auto &t = board.tft();
  for (int r = 0; r < ROWS_B; r++) {
    for (int c = 0; c < COLS; c++) {
      int b = borderAt(i, 0, r, c, ROWS_B, COLS);
      uint16_t color = b >= 0 ? (uint16_t)b : ground;
      for (int dy = 0; dy < PX; dy++)
        for (int dx = 0; dx < PX; dx++) line[dy * SPAN.w + c * PX + dx] = color;
    }
    t.writeRect(SPAN.x, rowTop(i) + r * PX, SPAN.w, PX, line);
  }
}

// ---- The tracks as the rows show them (ui.h).
static ShownTrack shownTracks[NUM_TRACKS];
const ShownTrack &shownTrack(int i) { return shownTracks[i]; }

static void takeLooks(int t, ShownTrack &s) {
  const Track &tr = tracks[t];
  memset(&s, 0, sizeof s);                         // (padding too: compared with memcmp)
  s.state = tr.state;
  s.volume = tr.volume;
  s.balance = tr.balance;
  s.filterLo = tr.filterLo;
  s.filterHi = tr.filterHi;
  if (tr.state == TrackState::Empty) return;       // (an empty track: no clip to show)
  s.overviewVersion = tr.overviewVersion;
  s.loopBars = tr.loopBars;
  trackRange(t, s.rs, s.rb);
  s.rangeStartBeat = tr.rangeStartBeat;
  s.rangeBeats = tr.rangeBeats;
  s.rangeAnchorQ = tr.rangeAnchorQ;
  memcpy(s.overview, tr.overview, OVERVIEW_COLS);
}

// Takes the tracks' looks (held ones as above); the rows that changed are
// redrawn at once, together (their buttons too). drawTrack() calls it first.
static void followTracks() {
  static bool busy = false;                        // (drawing the changed rows: drawTrack() again)
  static uint8_t held = 0;                         // tracks keeping their looks until every change is through
  if (busy) return;
  uint8_t onWay = 0;
  for (int t = 0; t < NUM_TRACKS; t++)
    if (trackChangeOnWay(t)) onWay |= 1 << t;
  held = onWay ? held | onWay : 0;
  uint8_t changed = 0;
  for (int t = 0; t < NUM_TRACKS; t++) {
    if (held & (1 << t)) continue;
    ShownTrack s;
    takeLooks(t, s);
    if (memcmp(&s, &shownTracks[t], sizeof s)) {
      shownTracks[t] = s;
      changed |= 1 << t;
    }
  }
  if (!changed || menuOpen()) return;              // (the menu: redrawn in full when it closes)
  busy = true;
  for (int t = 0; t < NUM_TRACKS; t++)
    if (changed & (1 << t)) {
      drawCells(t);
      drawTrack(t);
    }
  busy = false;
}

void drawTrack(int i) {
  followTracks();
  if (i < 0 || i >= ROWS || (menuOpen() && i != STATUS_ROW)) return;   // (the menu: in the track rows' place)
  drawCells(i);
  if (i == STATUS_ROW) {
    drawStatus();
    return;
  }
  // One cell or three: a change redraws everything (the rows next to it share borders).
  static bool wasSpan[NUM_TRACKS];
  if (spanRow(i) != wasSpan[i]) {
    wasSpan[i] = spanRow(i);
    uiGeneration++;
  }
  if (spanRow(i)) {
    drawSpan(i);
    return;
  }
  drawFilter(i);
  drawMix(i);
  drawWave(i, recordingOn(i));
}

// The accent colour: the loaded project's (ACCENT_DARK at half its
// brightness). A change redraws everything; in the menu, nothing: its cards,
// list and logo follow by themselves (wiping it all and drawing it again
// flickered), and closing it redraws the looper's view in full.
static void followProjectColor() {
  static int shownProject = -1;
  int p = projectCurrent();
  if (p == shownProject) return;
  shownProject = p;
  ACCENT = p >= 0 ? PROJECT_COLORS[p] : LOGO_COLOR;   // (the dummy project: the splash's turquoise)
  ACCENT_DARK = blend565(ACCENT, BG, 128);
  if (menuOpen()) return;
  uiGeneration++;
  for (int i = 0; i < ROWS; i++) drawTrack(i);
}

void drawEdgeLine() {
  static uint8_t shownKey = 0;                     // the edge shown (its arrow), 0: none
  static uint32_t shownGen = 0;                    // (uiGeneration then: since redrawn whole, it's gone already)
  uint8_t key = screenSwitchPending() ? screenSwitchKey() : 0;
  if (key != m8::BTN_LEFT && key != m8::BTN_RIGHT && key != m8::BTN_UP && key != m8::BTN_DOWN) key = 0;   // (LEFT+RIGHT held: no one edge)
  auto &t = board.tft();
  if (!key) {
    if (shownKey && shownGen == uiGeneration) {    // let go without switching: what was under it
      uiGeneration++;
      if (menuOpen()) {
        drawMenu();
        drawTrack(STATUS_ROW);
      } else {
        t.fillScreen(BG);
        for (int i = 0; i < ROWS; i++) drawTrack(i);
      }
    }
    shownKey = 0;
    return;
  }
  uint16_t ink = menuOpen() ? BG : DARK;           // (dark grey on the looper's black; black on the menu's dark grey)
  if (key == m8::BTN_LEFT) t.fillRect(0, 0, PX, 240, ink);
  else if (key == m8::BTN_RIGHT) t.fillRect(320 - PX, 0, PX, 240, ink);
  else if (key == m8::BTN_UP) t.fillRect(0, 0, 320, PX, ink);
  else t.fillRect(0, 240 - PX, 320, PX, ink);
  if (menuOpen()) menuDrawLogo();                  // (the menu's corner logo on top of it)
  shownKey = key;
  shownGen = uiGeneration;
}

void uiRefresh() {
  followProjectColor();
  followTracks();                                  // (a switch through: its rows at once)
  // Redraw a little at a time so SD streaming keeps going.
  static uint32_t lastDraw = 0;
  static int nextRow = 0;
  if (millis() - lastDraw >= 30) {
    lastDraw = millis();
    if (menuOpen()) {                                // the menu and the status row
      drawCells(STATUS_ROW);
      drawStatus();
      drawMenu();
      drawEdgeLine();
      return;
    }
    // Every pass, all rows' buttons (only what changed is redrawn): the
    // recording track's icon flashes; the status row's follows SHIFT, the
    // selection and the transport.
    for (int i = 0; i < ROWS; i++) drawCells(i);
    drawStatus();                                    // every pass: the clock shows steps
    updatePlayheads();
    for (int i = 0; i < NUM_TRACKS; i++) drawMix(i);   // the meters
    drawTrack(nextRow);
    if (++nextRow >= ROWS) nextRow = 0;
    drawEdgeLine();                                  // (on top)
  }
}
