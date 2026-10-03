// ui_status.cpp — the status row: tempo, clock and scene (see ui.h)
#include "ui.h"

// Status row, in the pixel font, each cell framed like the tracks' when the
// row is selected:
//   first cell: the scene, a "SCN" tag (inverted: white
//     while playing, grey while stopped; in the accent colour while B is held
//     on the cell: B + UP/DOWN step through the scenes) and its position "3/7"
//     (white while playing, grey while stopped; the empty scene "0/7" and the
//     copy after the last "*/7" in grey).
//     Both in the accent colour while SHIFT is held, or B alone on the cell. The
//     position flashes with the beat (the accent colour, then grey) while
//     another scene is selected but not there yet: SHIFT (or B) still held on
//     it, or the change waiting for its switch point.
//   second cell: the clock (bar.beat.step)
//     on the left (a grey 1.1.1 while stopped; running: in the accent colour
//     on 1.1.1, white on each beat's first step, light grey on the others), the
//     tempo on the right (white while playing, grey while stopped; inverted
//     while its cell is selected, on the accent colour while B is held there:
//     B + arrows change it).
// Text and tags are redrawn whole each pass (their boxes clear what was
// there); the frames only when they change.
static constexpr int32_t CLOCK_BARS = 256;        // the clock wraps from 256.4.4 to 1.1.1
static constexpr int INSET = 2 * PX;               // text from the cell's edges (inside the frame)
static constexpr int CLOCK_W = 24 * PX;           // its box (clearing longer text)

// The status row's cells: the scene's and the tempo's, half the width each.
static constexpr int STATUS_CELLS = TEMPO_CELL + 1;
static const Cell &statusCell(int cell) { return cell == TEMPO_CELL ? TEMPO_BOX : SCENE_BOX; }

// A cell's border (the frame), each side in its colour (borderAt(): the
// two cells share the side between them).
static void drawFrame(int cell) {
  auto &t = board.tft();
  const Cell &c = statusCell(cell);
  int top = rowTop(STATUS_ROW), rows = CELL_H / PX, cols = c.w / PX;
  t.fillRect(c.x + PX, top, c.w - 2 * PX, PX, borderAt(STATUS_ROW, cell, 0, 1, rows, cols));
  t.fillRect(c.x + PX, top + CELL_H - PX, c.w - 2 * PX, PX, borderAt(STATUS_ROW, cell, rows - 1, 1, rows, cols));
  t.fillRect(c.x, top + PX, PX, CELL_H - 2 * PX, borderAt(STATUS_ROW, cell, 1, 0, rows, cols));
  t.fillRect(c.x + c.w - PX, top + PX, PX, CELL_H - 2 * PX, borderAt(STATUS_ROW, cell, 1, cols - 1, rows, cols));
}

void drawStatus() {
  static uint64_t shownBorders = 0;                // the cells' borders (bordersOf())
  static uint32_t drawn = 0;                       // uiGeneration when the cells were last cleared
  auto &t = board.tft();
  int top = rowTop(STATUS_ROW);
  if (drawn != uiGeneration) {
    drawn = uiGeneration;
    for (int c = 0; c < STATUS_CELLS; c++) t.fillRect(statusCell(c).x, top, statusCell(c).w, 240 - top, BG);
    shownBorders = ~0ull;
  }
  uint64_t borders = bordersOf(STATUS_ROW, SCENE_CELL) ^ bordersOf(STATUS_ROW, TEMPO_CELL) << 1;
  if (borders != shownBorders) {
    shownBorders = borders;
    for (int c = 0; c < STATUS_CELLS; c++) drawFrame(c);
  }

  char text[24];
  // The tempo. A scene's tempo waiting for its switch point: the one playing until then.
  noInterrupts();
  bool tempoWaiting = transport.pendingSpb && transport.spbAtBeat >= 0;
  uint32_t playingSpb = transport.spb;
  interrupts();
  float shown = tempoWaiting ? roundf(bpmForSpb(playingSpb) * 10) / 10 : bpm;
  snprintf(text, sizeof text, "%.1f BPM", shown);
  int cell = selected == STATUS_ROW ? selectedCellIn(STATUS_ROW) : -1;
  bool playing = transport.running;                // labels: white while playing, grey while stopped
  uint16_t live = playing ? INK : DIM;
  int tagY = top + (CELL_H - TAG_H) / 2, textY = top + (CELL_H - FONT_H) / 2;
  {
    const Cell &box = TEMPO_BOX;
    int right = box.x + box.w - INSET;
    static const int tempoW = tagWidth("888.8 BPM");   // the widest tempo: what any other clears
    bool editing = cell == TEMPO_CELL;
    // In the accent colour: B alone held on it (B + arrows change it).
    bool accent = editing && bAlone();
    uint16_t back = accent ? ACCENT : INK;
    int w = tagWidth(text);
    t.fillRect(right - tempoW, tagY, tempoW - w, TAG_H, BG);   // what longer text left
    drawTag(right - w, tagY, text, editing ? TAG_INVERTED : TAG_PLAIN, editing || accent ? back : live, BG);
  }

  // The clock.
  noInterrupts();
  int32_t beat = transport.beat;
  uint32_t step = (uint64_t)transport.sampleInBeat * 4 / transport.spb;   // quarter beats
  bool running = transport.running;
  interrupts();
  if (running)
    snprintf(text, sizeof text, "%ld.%ld.%lu", beat / BEATS_PER_BAR % CLOCK_BARS + 1, beat % BEATS_PER_BAR + 1, step + 1);
  else
    snprintf(text, sizeof text, "1.1.1");          // where it starts from
  // Grey while stopped; running: the accent colour on 1.1.1, white on each
  // beat's first step (x.y.1), light grey on the others.
  bool first = beat % (BEATS_PER_BAR * CLOCK_BARS) == 0 && step == 0;
  uint16_t clockInk = !running ? DIM : first ? ACCENT : step == 0 ? INK : LIGHT;
  drawText(TEMPO_BOX.x + INSET, textY, text, clockInk, BG, CLOCK_W);   // (its box clears longer text)

  // The scene. Its box reaches the cell's inner edge: the position's text
  // (up to "250*/250") always fits, and clears any longer text before it.
  const Cell &box = SCENE_BOX;
  // In the accent colour: SHIFT held (SHIFT + UP/DOWN step through the
  // scenes), or B alone on the cell (B + UP/DOWN do).
  bool sceneAccent = (board.keys() & m8::BTN_SHIFT) || (cell == SCENE_CELL && bAlone());
  drawTag(box.x + INSET, tagY, "SCN", TAG_INVERTED, sceneAccent ? ACCENT : live, BG);
  int scene = sceneCurrent(), count = sceneCount();
  bool temporary = scene == 0 || scene > count;
  if (scene > count) snprintf(text, sizeof text, "*/%d", count);   // the copy after the last
  else snprintf(text, sizeof text, "%d/%d", scene, count);       // (0: the empty scene)
  uint16_t ink = sceneAccent ? ACCENT : temporary ? DIM : live;
  if (sceneHeld() || sceneChangeWaiting()) {
    noInterrupts();
    bool firstHalf = transport.sampleInBeat < transport.spb / 2;
    interrupts();
    ink = firstHalf ? ACCENT : DIM;
  }
  int x = box.x + INSET + tagWidth("SCN") + PX;
  drawText(x, textY, text, ink, BG, box.x + box.w - INSET - x);
}
