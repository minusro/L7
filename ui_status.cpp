// ui_status.cpp — the status row: menu icon, scene, clock and tempo (see ui.h)
#include "ui.h"
#include "input.h"

// Status row, in the pixel font, each cell framed like the tracks' when the
// row is selected:
// The labels (the "SCN" and "BPM" tags, the scene's position, the tempo) are grey,
// light grey on the selected cell.
//   first cell: the edge mark (ui.h), a dark grey triangle pointing down
//     (DOWN on the status row opens the menu); not selectable, no frame.
//   second cell: the scene, an inverted "SCN" tag and its position "3/7" (the
//     empty scene "0/7" and the copy after the last "*/7" in grey). Both in the
//     accent colour while SHIFT is held, or B alone on the cell: SHIFT or B +
//     UP/DOWN step through the scenes. The position flashes with the beat (the accent colour, then grey) while
//     another scene is selected but not there yet: SHIFT (or B) still held on
//     it, or the change waiting for its switch point. Selected, A duplicates the
//     scene (as SHIFT+B twice).
//   third cell: the clock (bar.beat.step), after a small play icon (the
//     accent colour while it runs), centred together: stopped, 1.1.1 in dark
//     grey (the row selected: black, on its dark grey ground); running, grey with light grey on each beat's
//     first step (selected: light grey, white on the first step), in the
//     accent colour on 1.1.1. Selected, A starts / stops the
//     clock (as START; SHIFT+A as SHIFT+START).
//   fourth cell: the tempo, followed by an inverted "BPM" tag (both in the
//     accent colour while B alone is held there: B + arrows change it).
// Text and tags are redrawn whole each pass (their boxes clear what was
// there); the frames only when they change.
static constexpr int32_t CLOCK_BARS = 256;        // the clock wraps from 256.4.4 to 1.1.1
static constexpr int INSET = 2 * PX;               // text from the cell's edges (inside the frame)

// The edge mark (ui.h), built in a buffer and sent at once over the space
// given (x, w): the looper's tip 2 blocks in from its left; the menu's
// arrow centred in it.
void drawEdgeMark(bool right, int x, int w, int y, uint16_t ink, uint16_t ground) {
  // An arrow: an open head (a chevron, 3 x 5) on a shaft along its middle row.
  static const char *const ARROW_LEFT[5] = {"..#..", ".#...", "#####", ".#...", "..#.."};
  constexpr int ROWS = STATUS_H / PX, CW = 5, CH = 5, R0 = (ROWS - CH) / 2;
  constexpr int MAX_W = 160;
  w = min(w, MAX_W);
  const int cols = w / PX;
  const int C0 = right ? (cols - CW) / 2 : 2;      // the looper's: its tip 2 blocks in; the menu's: centred
  static uint16_t px[STATUS_H * MAX_W];
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < cols; c++) {
      int tr = r - R0, tc = c - C0;
      bool on = tr >= 0 && tr < CH && tc >= 0 && tc < CW && ARROW_LEFT[tr][right ? CW - 1 - tc : tc] == '#';   // (right: mirrored)
      for (int dy = 0; dy < PX; dy++)
        for (int dx = 0; dx < PX; dx++) px[(r * PX + dy) * w + c * PX + dx] = on ? ink : ground;
    }
  board.tft().writeRect(x, y, w, STATUS_H, px);
}

// The status row's cells: the scene's, the clock's and the tempo's (ui.h).
static constexpr int STATUS_CELLS = TEMPO_CELL + 1;
static const Cell &statusCell(int cell) {
  return cell == TEMPO_CELL ? TEMPO_BOX : cell == CLOCK_CELL ? CLOCK_BOX : cell == SCENE_CELL ? SCENE_BOX : MENU_BOX;
}

// ---- The master volume: in the clock cell while B alone is held on it (B +
// arrows change it). A vertical line per step in every other column, filling
// the cell inside the border's block of space: the volume's line white (the
// first one at 0), the ones before it the dark accent, those after it dark
// grey. Each line's
// upper half is the left channel's meter, its lower half the right's: the
// accent colour up to the output's level, on the same scale (the top line:
// 0 dBFS), falling at MV_FALL_DB_PER_S. Shown again, it starts from silence.
static constexpr int MV_COLS = CLOCK_BOX.w / PX, MV_ROWS = STATUS_H / PX;   // 47 x 13, the border's included
static constexpr int MV_LINES = (MV_COLS - 4 + 1) / 2;                     // columns 2, 4 ... 44
static_assert(MV_LINES == MASTER_VOLUME_MAX + 1, "a line per master volume step (0 included)");
static constexpr int MV_HALF = (MV_ROWS - 4 - 1) / 2;                       // rows 2-5 left, 7-10 right
static constexpr float MV_FALL_DB_PER_S = 24;

static void drawMasterVolume(bool full) {
  static uint16_t shown[MV_LINES][2];
  static float meterDb[2] = {-200, -200};
  static uint32_t lastMs = 0;
  auto &t = board.tft();
  const int top = rowTop(STATUS_ROW);
  float peak[2];
  takeOutputPeaks(peak[0], peak[1]);
  if (full) peak[0] = peak[1] = 0;                 // (what played while hidden: not shown)
  uint32_t now = millis();
  float fall = (now - lastMs) * (MV_FALL_DB_PER_S / 1000);
  lastMs = now;
  if (full) {
    t.fillRect(CLOCK_BOX.x + PX, top + PX, CLOCK_BOX.w - 2 * PX, STATUS_H - 2 * PX, rowGround(STATUS_ROW));   // (inside the border)
    meterDb[0] = meterDb[1] = -200;
  }
  int level = masterVolume();
  for (int ch = 0; ch < 2; ch++) {
    float db = peak[ch] > 0 ? 20 * log10f(peak[ch]) : -200.0f;
    meterDb[ch] = max(db, meterDb[ch] - fall);
    // (Line j: step j's level, (j - MASTER_VOLUME_UNITY) * MASTER_STEP_DB dB.)
    int lit = meterDb[ch] < -150 ? 0 : constrain((int)floorf(meterDb[ch] / MASTER_STEP_DB + MASTER_VOLUME_UNITY + 0.5f) + 1, 0, MV_LINES);
    for (int j = 0; j < MV_LINES; j++) {
      uint16_t color = j == level ? INK : j < lit ? ACCENT : j < level ? ACCENT_DARK : onGround(DARK, rowGround(STATUS_ROW));
      if (!full && color == shown[j][ch]) continue;
      shown[j][ch] = color;
      t.fillRect(CLOCK_BOX.x + (2 + 2 * j) * PX, top + (2 + ch * (MV_HALF + 1)) * PX, PX, MV_HALF * PX, color);
    }
  }
}

// A cell's border (the frame), each side in its colour (borderAt(): the
// two cells share the side between them).
static void drawFrame(int cell) {
  auto &t = board.tft();
  const Cell &c = statusCell(cell);
  int top = rowTop(STATUS_ROW), rows = STATUS_H / PX, cols = c.w / PX;
  t.fillRect(c.x + PX, top, c.w - 2 * PX, PX, borderAt(STATUS_ROW, cell, 0, 1, rows, cols));
  t.fillRect(c.x + PX, top + STATUS_H - PX, c.w - 2 * PX, PX, borderAt(STATUS_ROW, cell, rows - 1, 1, rows, cols));
  t.fillRect(c.x, top + PX, PX, STATUS_H - 2 * PX, borderAt(STATUS_ROW, cell, 1, 0, rows, cols));
  t.fillRect(c.x + c.w - PX, top + PX, PX, STATUS_H - 2 * PX, borderAt(STATUS_ROW, cell, 1, cols - 1, rows, cols));
  for (int r : {0, rows - 1})                      // and its corners (top: cut; bottom: the frame's, not cut)
    for (int k : {0, cols - 1})
      t.fillRect(c.x + k * PX, top + r * PX, PX, PX, borderAt(STATUS_ROW, cell, r, k, rows, cols));
}

void drawStatus() {
  if (menuOpen()) return;                          // (the menu: only the action button shows on the status row)
  static uint64_t shownBorders = 0;                // the cells' borders (bordersOf())
  static uint32_t drawn = 0;                       // uiGeneration when the cells were last cleared
  auto &t = board.tft();
  int top = rowTop(STATUS_ROW);
  // Selected: on a dark grey ground (rowGround()); only the selected cell in
  // its selected look (not the whole row's, as on a track).
  const uint16_t G = rowGround(STATUS_ROW);
  static uint16_t shownGround = BG;
  bool cleared = drawn != uiGeneration || G != shownGround;
  if (cleared) {
    drawn = uiGeneration;
    shownGround = G;
    for (int c = 0; c < STATUS_CELLS; c++)        // (the ground from the scene cell's left edge: not under the menu cell)
      t.fillRect(statusCell(c).x, top, statusCell(c).w, 240 - top, c == MENU_CELL ? BG : G);
    shownBorders = ~0ull;
  }
  uint64_t borders = 0;
  for (int c = 0; c < STATUS_CELLS; c++) borders = borders * 31 + bordersOf(STATUS_ROW, c);
  if (borders != shownBorders) {
    shownBorders = borders;
    for (int c = SCENE_CELL; c < STATUS_CELLS; c++) drawFrame(c);   // (the menu cell: never selected, its edge mark to the edges)
  }

  char text[24];
  // The tempo. A scene's tempo waiting for its switch point: the one playing until then.
  noInterrupts();
  bool tempoWaiting = transport.pendingSpb && transport.spbAtBeat >= 0;
  uint32_t playingSpb = transport.spb;
  interrupts();
  float shown = tempoWaiting ? roundf(bpmForSpb(playingSpb) * 10) / 10 : bpm;
  snprintf(text, sizeof text, "%.1f", shown);
  int cell = selected == STATUS_ROW && !(menuOpen() && !menuOnStatus()) ? selectedCellIn(STATUS_ROW) : -1;
  bool rowSelected = cell >= 0;
  auto labelInk = [&](int c) -> uint16_t { return cell == c ? LIGHT : DIM; };   // (only the selected cell: not the whole row, as on a track)
  int tagY = top + (STATUS_H - TAG_H) / 2 / PX * PX, textY = top + (STATUS_H - FONT_H) / 2 / PX * PX;   // (on the block grid)
  {
    // The menu cell: the edge mark, pointing left, on black (the selected
    // row's ground starts after it): dark grey; grey where LEFT would go to
    // the menu (a row's first cell); white while an arrow is held that goes
    // there once let go.
    static uint16_t markInk = 0;
    bool leftLeaves = selectedCellIn(selected) == firstCellIn(selected);
    uint16_t ink = screenSwitchPending() ? INK : leftLeaves ? DIM : DARK;
    if (cleared || ink != markInk) drawEdgeMark(false, 0, EDGE_W, top, ink, BG);
    markInk = ink;
  }
  {
    // The tempo, then an inverted "BPM" tag at the cell's right edge (like the
    // scene's "SCN"): both in the accent colour while B alone is held on the
    // cell (B + arrows change the tempo).
    const Cell &box = TEMPO_BOX;
    int right = box.x + box.w - INSET;
    uint16_t ink = cell == TEMPO_CELL && bAlone() ? ACCENT : labelInk(TEMPO_CELL);
    int tagX = right - tagWidth("BPM");
    drawTag(tagX, tagY, "BPM", TAG_INVERTED, ink, G);
    static const int tempoW = textWidth("888.8");  // the widest tempo: its box clears any other
    int numRight = tagX - PX, numLeft = numRight - tempoW, w = textWidth(text);
    t.fillRect(numLeft, tagY, numRight - numLeft + PX, textY - tagY, G);   // round the text: what was there
    t.fillRect(numLeft, textY + FONT_H, numRight - numLeft + PX, tagY + TAG_H - textY - FONT_H, G);
    t.fillRect(numLeft, textY, tempoW - w, FONT_H, G);
    t.fillRect(numRight, textY, PX, FONT_H, G);
    drawText(numRight - w, textY, text, ink, G);
  }

  // The clock, or the master volume while B alone is held on its cell.
  static bool shownVolume = false;
  bool volume = cell == CLOCK_CELL && bAlone();
  if (volume) {
    drawMasterVolume(!shownVolume || cleared);
    shownVolume = true;
  } else if (shownVolume) {
    t.fillRect(CLOCK_BOX.x + PX, top + PX, CLOCK_BOX.w - 2 * PX, STATUS_H - 2 * PX, G);   // (the clock again)
    shownVolume = false;
  }
  noInterrupts();
  int32_t beat = transport.beat - transportOrigin();   // (from a project switch's bar line)
  uint32_t step = (uint64_t)transport.sampleInBeat * 4 / transport.spb;   // quarter beats
  bool running = transport.running;
  interrupts();
  if (running)
    snprintf(text, sizeof text, "%ld.%ld.%lu", beat / BEATS_PER_BAR % CLOCK_BARS + 1, beat % BEATS_PER_BAR + 1, step + 1);
  else
    snprintf(text, sizeof text, "1.1.1");          // where it starts from
  // Stopped: dark grey (the row selected: black, on its dark grey ground). Running: grey, light grey on each
  // beat's first step (x.y.1) (selected: light grey, white on x.y.1); the
  // accent colour on 1.1.1.
  bool first = beat % (BEATS_PER_BAR * CLOCK_BARS) == 0 && step == 0, here = cell == CLOCK_CELL;
  uint16_t clockInk = !running ? (rowSelected ? BG : DARK) : first ? ACCENT
                    : step == 0 ? (here ? INK : LIGHT) : (here ? LIGHT : DIM);
  if (!volume) {
    // A small play icon (in the accent colour while the clock runs), then the
    // text, centred together (on the block grid); what longer text left
    // either side cleared.
    static const char *const PLAY[5] = {"#..", "##.", "###", "##.", "#.."};
    const int iconW = 3 * PX, gap = 2 * PX;
    int w = textWidth(text), inL = CLOCK_BOX.x + INSET, inR = CLOCK_BOX.x + CLOCK_BOX.w - INSET;
    int x = CLOCK_BOX.x + (CLOCK_BOX.w - iconW - gap - w) / 2 / PX * PX, tx = x + iconW + gap;
    x -= PX;                                       // (the icon a block further left)
    t.fillRect(inL, textY, x - inL, FONT_H, G);
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < 3; c++)
        t.fillRect(x + c * PX, textY + r * PX, PX, PX, PLAY[r][c] == '#' && running ? ACCENT : G);
    t.fillRect(x + iconW, textY, tx - x - iconW, FONT_H, G);
    t.fillRect(tx + w, textY, inR - tx - w, FONT_H, G);
    drawText(tx, textY, text, clockInk, G);
  }

  // The scene. Its box reaches the cell's inner edge: the position's text
  // (up to "250*/250") always fits, and clears any longer text before it.
  const Cell &box = SCENE_BOX;
  // The tag and position in the accent colour: SHIFT held (SHIFT + UP/DOWN
  // step through the scenes), or B alone on the cell (B + UP/DOWN do).
  bool sceneAccent = (board.keys() & m8::BTN_SHIFT) || (cell == SCENE_CELL && bAlone());
  drawTag(box.x + INSET, tagY, "SCN", TAG_INVERTED, sceneAccent ? ACCENT : labelInk(SCENE_CELL), G);
  int scene = sceneCurrent(), count = sceneCount();
  bool temporary = scene == 0 || scene > count;
  if (scene > count) snprintf(text, sizeof text, "*/%d", count);   // the copy after the last
  else snprintf(text, sizeof text, "%d/%d", scene, count);       // (0: the empty scene)
  uint16_t ink = sceneAccent ? ACCENT : temporary ? DIM : labelInk(SCENE_CELL);
  if (sceneHeld() || sceneChangeWaiting()) {
    noInterrupts();
    bool firstHalf = transport.sampleInBeat < transport.spb / 2;
    interrupts();
    ink = firstHalf ? ACCENT : DIM;
  }
  int x = box.x + INSET + tagWidth("SCN") + PX;
  drawText(x, textY, text, ink, G, box.x + box.w - INSET - x);

}

