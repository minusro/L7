// ui_status.cpp — the status row: tempo, clock and scene (see ui.h)
#include "ui.h"

// Status row, in the pixel font, each cell framed like the tracks' when the
// row is selected:
// The labels (the "SCN" and "BPM" tags, the scene's position, the tempo) are grey,
// light grey on the selected cell.
//   first cell: the scene, an inverted "SCN" tag and its position "3/7" (the
//     empty scene "0/7" and the copy after the last "*/7" in grey). Both in the
//     accent colour while SHIFT is held, or B alone on the cell: SHIFT or B +
//     UP/DOWN step through the scenes. The position flashes with the beat (the accent colour, then grey) while
//     another scene is selected but not there yet: SHIFT (or B) still held on
//     it, or the change waiting for its switch point.
//   second cell: the clock (bar.beat.step), centred: stopped, 1.1.1 in dark
//     grey (selected: grey); running, grey with light grey on each beat's
//     first step (selected: light grey, white on the first step), in the
//     accent colour on 1.1.1. Selected, A starts / stops the
//     clock (as START; SHIFT+A as SHIFT+START).
//   third cell: the tempo, followed by an inverted "BPM" tag (both in the
//     accent colour while B alone is held there: B + arrows change it).
// Text and tags are redrawn whole each pass (their boxes clear what was
// there); the frames only when they change.
static constexpr int32_t CLOCK_BARS = 256;        // the clock wraps from 256.4.4 to 1.1.1
static constexpr int INSET = 2 * PX;               // text from the cell's edges (inside the frame)

// The status row's cells: the scene's, the clock's and the tempo's (ui.h).
static constexpr int STATUS_CELLS = TEMPO_CELL + 1;
static const Cell &statusCell(int cell) { return cell == TEMPO_CELL ? TEMPO_BOX : cell == CLOCK_CELL ? CLOCK_BOX : SCENE_BOX; }

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
  uint64_t borders = 0;
  for (int c = 0; c < STATUS_CELLS; c++) borders = borders * 31 + bordersOf(STATUS_ROW, c);
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
  snprintf(text, sizeof text, "%.1f", shown);
  int cell = selected == STATUS_ROW ? selectedCellIn(STATUS_ROW) : -1;
  auto labelInk = [&](int c) -> uint16_t { return cell == c ? LIGHT : DIM; };
  int tagY = top + (STATUS_H - TAG_H) / 2 / PX * PX, textY = top + (STATUS_H - FONT_H) / 2 / PX * PX;   // (on the block grid)
  {
    // The tempo, then an inverted "BPM" tag at the cell's right edge (like the
    // scene's "SCN"): both in the accent colour while B alone is held on the
    // cell (B + arrows change the tempo).
    const Cell &box = TEMPO_BOX;
    int right = box.x + box.w - INSET;
    uint16_t ink = cell == TEMPO_CELL && bAlone() ? ACCENT : labelInk(TEMPO_CELL);
    int tagX = right - tagWidth("BPM");
    drawTag(tagX, tagY, "BPM", TAG_INVERTED, ink, BG);
    static const int tempoW = textWidth("888.8");  // the widest tempo: its box clears any other
    int numRight = tagX - PX, numLeft = numRight - tempoW, w = textWidth(text);
    t.fillRect(numLeft, tagY, numRight - numLeft + PX, textY - tagY, BG);   // round the text: what was there
    t.fillRect(numLeft, textY + FONT_H, numRight - numLeft + PX, tagY + TAG_H - textY - FONT_H, BG);
    t.fillRect(numLeft, textY, tempoW - w, FONT_H, BG);
    t.fillRect(numRight, textY, PX, FONT_H, BG);
    drawText(numRight - w, textY, text, ink, BG);
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
  // Stopped: dark grey (selected: grey). Running: grey, light grey on each
  // beat's first step (x.y.1) (selected: light grey, white on x.y.1); the
  // accent colour on 1.1.1.
  bool first = beat % (BEATS_PER_BAR * CLOCK_BARS) == 0 && step == 0, here = cell == CLOCK_CELL;
  uint16_t clockInk = !running ? (here ? DIM : DARK) : first ? ACCENT
                    : step == 0 ? (here ? INK : LIGHT) : (here ? LIGHT : DIM);
  {
    // Centred (on the block grid); what longer text left either side cleared.
    int w = textWidth(text), inL = CLOCK_BOX.x + INSET, inR = CLOCK_BOX.x + CLOCK_BOX.w - INSET;
    int x = CLOCK_BOX.x + (CLOCK_BOX.w - w) / 2 / PX * PX;
    t.fillRect(inL, textY, x - inL, FONT_H, BG);
    t.fillRect(x + w, textY, inR - x - w, FONT_H, BG);
    drawText(x, textY, text, clockInk, BG);
  }

  // The scene. Its box reaches the cell's inner edge: the position's text
  // (up to "250*/250") always fits, and clears any longer text before it.
  const Cell &box = SCENE_BOX;
  // The tag and position in the accent colour: SHIFT held (SHIFT + UP/DOWN
  // step through the scenes), or B alone on the cell (B + UP/DOWN do).
  bool sceneAccent = (board.keys() & m8::BTN_SHIFT) || (cell == SCENE_CELL && bAlone());
  drawTag(box.x + INSET, tagY, "SCN", TAG_INVERTED, sceneAccent ? ACCENT : labelInk(SCENE_CELL), BG);
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
  drawText(x, textY, text, ink, BG, box.x + box.w - INSET - x);
}
