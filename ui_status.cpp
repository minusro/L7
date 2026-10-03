// ui_status.cpp — the status row: tempo, clock and scene (see ui.h)
#include "ui.h"

// Status row, in the pixel font, each cell framed like the tracks' when the
// row is selected:
//   first cell (under the waveforms): the scene, a "SCN" tag (inverted: white
//     while playing, grey while stopped; in the accent colour while B is held
//     on the cell: B + UP/DOWN step through the scenes) and its position "3/7"
//     (white while playing, grey while stopped; 0* and N+1* with a star, grey).
//     Both in the accent colour while SHIFT is held, or B alone on the cell. The
//     position flashes with the beat (the accent colour, then grey) while
//     another scene is selected but not there yet: SHIFT (or B) still held on
//     it, or the change waiting for its switch point.
//   second cell (the filter and volume columns): the clock (bar.beat.step)
//     on the left (white while running, a grey 1.1.1 while stopped), the
//     tempo on the right (white while playing, grey while stopped; inverted
//     while its cell is selected, on the accent colour while B is held there:
//     B + arrows change it; in the accent colour while A alone is held on the
//     status row: tapping it).
// Text and tags are redrawn whole each pass (their boxes clear what was
// there); the frames only when they change.
static constexpr int32_t CLOCK_BARS = 256;        // the clock wraps from 256.4.4 to 1.1.1
static constexpr int INSET = 2 * PX;               // text from the cell's edges (inside the frame)
static constexpr int CLOCK_W = 24 * PX;           // its box (clearing longer text)

// The status row's cells: the scene's, and the tempo's spanning two columns.
static constexpr int STATUS_CELLS = TEMPO_CELL + 1;
static const Cell &statusCell(int cell) { return cell == TEMPO_CELL ? TEMPO_BOX : CELL[cell]; }

static void drawFrame(int cell, uint16_t color) {
  auto &t = board.tft();
  const Cell &c = statusCell(cell);
  int top = rowTop(STATUS_ROW);
  t.fillRect(c.x + PX, top, c.w - 2 * PX, PX, color);
  t.fillRect(c.x + PX, top + ROW_BAND - PX, c.w - 2 * PX, PX, color);
  t.fillRect(c.x, top + PX, PX, ROW_BAND - 2 * PX, color);
  t.fillRect(c.x + c.w - PX, top + PX, PX, ROW_BAND - 2 * PX, color);
}

void drawStatus() {
  static uint16_t shownFrame[STATUS_CELLS];
  static uint32_t drawn = 0;                       // uiGeneration when the cells were last cleared
  auto &t = board.tft();
  int top = rowTop(STATUS_ROW);
  if (drawn != uiGeneration) {
    drawn = uiGeneration;
    for (int c = 0; c < STATUS_CELLS; c++) {
      t.fillRect(statusCell(c).x, top, statusCell(c).w, 240 - top, BG);
      shownFrame[c] = BG;
    }
  }
  for (int c = 0; c < STATUS_CELLS; c++) {
    uint16_t f = cellFrame(STATUS_ROW, c);
    if (f != shownFrame[c]) drawFrame(c, shownFrame[c] = f);
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
  int tagY = top + (ROW_BAND - TAG_H) / 2, textY = top + (ROW_BAND - FONT_H) / 2;
  {
    const Cell &box = TEMPO_BOX;
    int right = box.x + box.w - INSET;
    static const int tempoW = tagWidth("888.8 BPM");   // the widest tempo: what any other clears
    bool editing = cell == TEMPO_CELL;
    // In the accent colour: B alone held on it (B + arrows change it), or A
    // alone held on the status row (tapping it).
    bool accent = (editing && bAlone()) || (selected == STATUS_ROW && board.keys() == m8::BTN_EDIT);
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
  drawText(TEMPO_BOX.x + INSET, textY, text, running ? INK : DIM, BG, CLOCK_W);   // (its box clears longer text)

  // The scene. Its box reaches the cell's inner edge: the position's text
  // (up to "250*/250") always fits, and clears any longer text before it.
  const Cell &box = CELL[SCENE_CELL];
  // In the accent colour: SHIFT held (SHIFT + UP/DOWN step through the
  // scenes), or B alone on the cell (B + UP/DOWN do).
  bool sceneAccent = (board.keys() & m8::BTN_SHIFT) || (cell == SCENE_CELL && bAlone());
  drawTag(box.x + INSET, tagY, "SCN", TAG_INVERTED, sceneAccent ? ACCENT : live, BG);
  int scene = sceneCurrent(), count = sceneCount();
  bool temporary = scene == 0 || scene > count;
  snprintf(text, sizeof text, "%d%s/%d", scene, temporary ? "*" : "", count);
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
