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
  // The sample switch (SHIFT on the track, ui.h sampleSwitchShown()): its own
  // frame round the waveform cell, in the accent colour, whichever cell is selected.
  if (!corner && cell == WAVE_CELL && sampleSwitchShown(i)) return ACCENT;
  if (corner && ((l && cellRect(i, cell).x == 0) ||   // (the selected row's ground: all its corners cut, at the screen's
                 (rt && cellRect(i, cell).x + cellRect(i, cell).w == CELLS_RIGHT))) return BG;   //  edge and its last cell's)
  if (t || b) {
    // Shared with the row above / below: its selected cell's frame where it
    // runs over this column (not at that cell's own corners: cut there too),
    // by place on the screen (an empty track's one cell spans the row).
    int ni = t ? rowAbove(i) : rowBelow(i);
    if (ni >= 0 && ni == selected && sampleSwitchShown(ni)) {   // (its sample switch's frame, over its waveform cell)
      int x = cellRect(i, cell).x / PX + c;
      const Cell &w = cellRect(ni, WAVE_CELL);
      if (x > w.x / PX && x < (w.x + w.w) / PX - 1) return ACCENT;
    }
    if (ni >= 0 && ni == selected) {
      int x = cellRect(i, cell).x / PX + c, nc = frameCell(ni);
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
    k = k * 31 + (near ? (uint64_t)(frameCell(selected) + 1) << 20 | cellFrame(selected, frameCell(selected)) |
                             (uint64_t)(selected == rowAbove(i)) << 25 | (uint64_t)sampleSwitchShown(selected) << 26 : 0);
  }
  return k;
}

// ---- The tracks as the rows show them (ui.h).
static ShownTrack shownTracks[NUM_TRACKS];
const ShownTrack &shownTrack(int i) { return shownTracks[i]; }

static void takeLooks(int t, ShownTrack &s) {
  const Track &tr = tracks[t];
  memset(&s, 0, sizeof s);                         // (padding too: compared with memcmp)
  s.state = tr.state;
  s.volume = volumeStepOf(tr.volume, headroomDbOf(tr.overview));   // (as a step on its clip's scale)
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
  float norm = overviewNormOf(tr.overview);        // drawn normalised: its peak the full height
  // Its fine rotate (a change on its way, and one held but not heard yet,
  // counting): the waveform turned as far right.
  uint32_t loop = tr.loopFrames();
  TrackUnit u = unitOf(t);
  // (The step offset, whole quarter beats, not: shown as the grid moved instead.)
  const int32_t quarters = tr.loopBars * BEATS_PER_BAR * 4, qf = (int32_t)(tr.spbNative / 4);
  const int32_t steps = quarters ? (int32_t)((((int64_t)(u.sample == tr.sample ? u.steps : tr.steps) + clipRotatePendingSteps(t)) % quarters + quarters) % quarters) : 0;
  s.steps = (uint16_t)steps;
  int64_t turn = (u.sample == tr.sample ? u.rotate : tr.rotate) + (int64_t)clipRotatePending(t) - (int64_t)steps * qf;
  int shift = loop ? (int)(((turn % loop + loop) % loop) * OVERVIEW_COLS / loop) : 0;
  s.turn = shift;
  for (int c = 0; c < OVERVIEW_COLS; c++) s.overview[(c + shift) % OVERVIEW_COLS] = (uint8_t)min(255.0f, tr.overview[c] * norm + 0.5f);
}

// A scene shown before it plays (scenes.h scenePreviewing()): track t's looks
// from its unit there (a held selection's, or the change on its way) and its
// clip's (false: its file not opened yet, the row as it was meanwhile).
FLASHMEM static bool previewLooks(int t, ShownTrack &s) {
  TrackUnit u;
  if (!sceneHeldUnit(t, u)) u = unitOnWay(t);
  ClipLooks c = {nullptr, 0, 0};
  if (u.sample && !clipLooks(t, u.sample, c)) return false;
  memset(&s, 0, sizeof s);
  s.state = !u.sample ? TrackState::Empty : u.muted ? TrackState::Muted : TrackState::Playing;
  s.volume = volumeStepOf(u.volume, u.sample ? headroomDbOf(c.overview) : 0.0f);
  s.balance = u.balance;
  s.filterLo = u.filterLo;
  s.filterHi = u.filterHi;
  if (!u.sample) return true;
  s.overviewVersion = shownTracks[t].overviewVersion;   // (another clip's: told apart in followTracks())
  s.loopBars = c.loopBars;
  const uint32_t loopBeats = c.loopBars * BEATS_PER_BAR;
  const uint32_t len = u.rangeLen && u.rangeLen <= loopBeats ? u.rangeLen : loopBeats;   // (as storage.cpp applyRange())
  s.rs = s.rangeStartBeat = u.rangeStart < loopBeats ? u.rangeStart : 0;
  s.rb = s.rangeBeats = len;
  s.rangeAnchorQ = len ? -(int32_t)(u.rangePhase % (4 * len)) : 0;
  const int32_t quarters = loopBeats * 4, qf = (int32_t)(c.spb / 4);
  const int32_t steps = quarters ? (int32_t)(u.steps % quarters) : 0;
  s.steps = (uint16_t)steps;
  const int64_t loop = (int64_t)loopBeats * c.spb, turn = (int64_t)u.rotate - (int64_t)steps * qf;
  const int shift = loop ? (int)(((turn % loop + loop) % loop) * OVERVIEW_COLS / loop) : 0;
  s.turn = shift;
  const float norm = overviewNormOf(c.overview);
  for (int k = 0; k < OVERVIEW_COLS; k++) s.overview[(k + shift) % OVERVIEW_COLS] = (uint8_t)min(255.0f, c.overview[k] * norm + 0.5f);
  return true;
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
  // A scene shown before it plays: its tracks' looks instead (held ones too).
  const bool preview = scenePreviewing();
  uint8_t changed = 0;
  for (int t = 0; t < NUM_TRACKS; t++) {
    ShownTrack s;
    if (preview) {
      if (!previewLooks(t, s)) continue;
    } else {
      if (held & (1 << t)) continue;
      takeLooks(t, s);
    }
    // (Another waveform under the same version number: the drawn one not
    // taken for it, ui_wave.cpp.)
    if (s.overviewVersion == shownTracks[t].overviewVersion && memcmp(s.overview, shownTracks[t].overview, sizeof s.overview))
      s.overviewVersion ^= 0x80;
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

// The space between a track's cells (none touching: CELL): the row's ground,
// its top and bottom block rows the ones it shares with the rows above and
// below (dark grey if either is the selected track), as the cells' borders.
static void drawGaps(int i) {
  static uint32_t drawn[NUM_TRACKS], shown[NUM_TRACKS];
  uint16_t mid = rowGround(i), topG = mid == DARK || rowGround(rowAbove(i)) == DARK ? DARK : BG,
           botG = mid == DARK || rowGround(rowBelow(i)) == DARK ? DARK : BG;
  // The selected row above / below: its frame runs along the shared block row
  // by place (an empty track's one cell spans the gaps too), as in borderAt().
  auto frameOver = [&](int ni, int x) -> int {
    if (ni < 0 || ni != selected) return -1;
    int nc = frameCell(ni);
    const Cell &n = cellRect(ni, nc);
    int first = n.x / PX, last = (n.x + n.w) / PX - 1;
    uint16_t f = cellFrame(ni, nc);
    return x > first && x < last && f != BG ? f : -1;
  };
  uint32_t key = (uint32_t)mid << 2 | (topG == DARK) << 1 | (botG == DARK);
  for (int ni : {rowAbove(i), rowBelow(i)})
    if (ni == selected) key = key * 31 + ((uint32_t)cellFrame(ni, frameCell(ni)) << 4 | (ni == rowAbove(i)));
  if (drawn[i] == uiGeneration && shown[i] == key) return;
  drawn[i] = uiGeneration;
  shown[i] = key;
  auto &t = board.tft();
  const int top = rowTop(i);
  for (int k = 0; k + 1 < CELLS; k++) {
    int x = CELL[k].x + CELL[k].w, w = CELL[k + 1].x - x;
    if (w <= 0) continue;
    t.fillRect(x, top + PX, w, CELL_H - 2 * PX, mid);
    for (int c = x / PX; c < (x + w) / PX; c++) {  // (the shared block rows, a block at a time)
      int a = frameOver(rowAbove(i), c), b = frameOver(rowBelow(i), c);
      t.fillRect(c * PX, top, PX, PX, a >= 0 ? (uint16_t)a : topG);
      t.fillRect(c * PX, top + CELL_H - PX, PX, PX, b >= 0 ? (uint16_t)b : botG);
    }
  }
}

void drawTrack(int i) {
  keepStreaming();                                 // (a long redraw, row by row: the streams kept topped up)
  followTracks();
  if (i < 0 || i >= ROWS || (menuOpen() && i != STATUS_ROW)) return;   // (the menu: in the track rows' place)
  drawCells(i);
  if (i == STATUS_ROW) {
    drawStatus();
    return;
  }
  drawFilter(i);
  drawMix(i);
  drawWave(i, recordingOn(i));
  drawGaps(i);
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

// The edge line shown: its edge (its arrow; 0: none), uiGeneration then (since
// redrawn whole, it's gone already), its band's depth (px).
static uint8_t shownKey = 0;
static uint32_t shownGen = 0;
static int shownDepth = 0;

// The line along the edge of arrow `key` drawn, everything else clipped to
// the rest of the screen meanwhile.
FLASHMEM static void paintEdgeLine(uint8_t key, uint16_t color) {
  // A line the edge's whole length, a block wide (both screens alike): grey,
  // light grey for its first 50 ms.
  auto &t = board.tft();
  t.setClipRect();
  constexpr int W = PX;
  auto band = [&](int in, int w, uint16_t c) {     // (w px, in px from the edge)
    if (key == m8::BTN_LEFT) t.fillRect(in, 0, w, SCREEN_H, c);
    else if (key == m8::BTN_RIGHT) t.fillRect(SCREEN_W - in - w, 0, w, SCREEN_H, c);
    else if (key == m8::BTN_UP) t.fillRect(0, in, SCREEN_W, w, c);
    else t.fillRect(0, SCREEN_H - in - w, SCREEN_W, w, c);
  };
  band(0, W, color);
  shownDepth = W;
  if (key == m8::BTN_LEFT) t.setClipRect(W, 0, SCREEN_W - W, SCREEN_H);
  else if (key == m8::BTN_RIGHT) t.setClipRect(0, 0, SCREEN_W - W, SCREEN_H);
  else if (key == m8::BTN_UP) t.setClipRect(0, W, SCREEN_W, SCREEN_H - W);
  else t.setClipRect(0, 0, SCREEN_W, SCREEN_H - W);
}

FLASHMEM void drawEdgeLine() {   // (flash: RAM1 is full)
  uint8_t key = screenSwitchPending() ? screenSwitchKey() : 0;
  if (key != m8::BTN_LEFT && key != m8::BTN_RIGHT && key != m8::BTN_UP && key != m8::BTN_DOWN) key = 0;   // (LEFT+RIGHT held: no one edge)
  auto &t = board.tft();
  if (!key) {
    t.setClipRect();                               // (the whole screen drawn to again)
    if (shownKey && shownGen == uiGeneration) {    // let go without switching: what was under it
      uiGeneration++;
      if (menuOpen()) {
        drawMenu();
        drawTrack(STATUS_ROW);
      } else {                                     // (only the line's band cleared first: no flash of the whole screen)
        if (shownKey == m8::BTN_LEFT) t.fillRect(0, 0, shownDepth, SCREEN_H, BG);
        else if (shownKey == m8::BTN_RIGHT) t.fillRect(SCREEN_W - shownDepth, 0, shownDepth, SCREEN_H, BG);
        else if (shownKey == m8::BTN_UP) t.fillRect(0, 0, SCREEN_W, shownDepth, BG);
        else t.fillRect(0, SCREEN_H - shownDepth, SCREEN_W, shownDepth, BG);
        for (int i = 0; i < ROWS; i++) drawTrack(i);
      }
    }
    shownKey = 0;
    return;
  }
  // Drawn once (and again after a full redraw), then everything else is
  // clipped to the rest of the screen meanwhile: nothing drawn over it (no
  // flicker). (A switch clears the clip first: input.cpp, edgeDue().)
  // Light grey for its first EDGE_FLASH_MS (the arrow just pressed), then grey.
  constexpr uint32_t EDGE_FLASH_MS = 50;
  static uint32_t lineAt = 0;
  static bool shownLight = false;
  if (key != shownKey) lineAt = millis();          // (a new line: from now)
  bool light = millis() - lineAt < EDGE_FLASH_MS;
  if (key == shownKey && shownGen == uiGeneration && light == shownLight) return;
  paintEdgeLine(key, light ? LIGHT : DIM);
  shownKey = key;
  shownGen = uiGeneration;
  shownLight = light;
}


void uiRefresh() {
  followProjectColor();
  static bool wasPreviewing = false;               // (a scene shown before it plays, or not any more: every row at once)
  if (scenePreviewing() != wasPreviewing && !menuOpen()) {
    wasPreviewing = !wasPreviewing;
    for (int i = 0; i < ROWS; i++) drawTrack(i);
  }
  followTracks();                                  // (a switch through: its rows at once)
  // Redraw a little at a time so SD streaming keeps going.
  static uint32_t lastDraw = 0;
  static int nextRow = 0;
  if (millis() - lastDraw >= 30) {
    lastDraw = millis();
    if (menuHelpOpen()) {                            // (the help pages: drawn as they're turned; UP or DOWN held: its edge line)
      helpService(helpPage());
      drawEdgeLine();
      return;
    }
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
