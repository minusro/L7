// ui_status.cpp — the status row: tempo, clock and scene (see ui.h)
#include "ui.h"

// Status row, in the pixel font, each cell framed like the tracks' when the
// row is selected:
//   first cell (under the waveforms): the tempo on the left (dim; inverted
//     while it's being edited), the clock (bar.beat.step) on the right
//     (white while running, a dim 1.1.1 while stopped);
//   second cell: the scene, an inverted "SCN" tag and its position "3/7"
//     (0* and N+1* with a star, dim). The position flashes with the beat
//     (white, then dim) while another scene is selected but not there yet:
//     SHIFT still held on it, or the change waiting for its switch point.
// Text and tags are redrawn whole each pass (their boxes clear what was
// there); the frames only when they change.
static constexpr int32_t CLOCK_BARS = 256;        // the clock wraps from 256.4.4 to 1.1.1
static constexpr int INSET = 2 * PX;               // text from the cell's edges (inside the frame)
static constexpr int TEMPO_W = 34 * PX, CLOCK_W = 24 * PX;   // their boxes (clearing longer text)

static void drawFrame(int cell, uint16_t color) {
  auto &t = board.tft();
  const Cell &c = CELL[cell];
  int top = rowTop(STATUS_ROW);
  t.fillRect(c.x + PX, top, c.w - 2 * PX, PX, color);
  t.fillRect(c.x + PX, top + ROW_BAND - PX, c.w - 2 * PX, PX, color);
  t.fillRect(c.x, top + PX, PX, ROW_BAND - 2 * PX, color);
  t.fillRect(c.x + c.w - PX, top + PX, PX, ROW_BAND - 2 * PX, color);
}

void drawStatus() {
  static uint16_t shownFrame[CELLS];
  static uint32_t drawn = 0;                       // uiGeneration when the cells were last cleared
  auto &t = board.tft();
  int top = rowTop(STATUS_ROW);
  if (drawn != uiGeneration) {
    drawn = uiGeneration;
    for (int c = 0; c < CELLS; c++) {
      t.fillRect(CELL[c].x, top, CELL[c].w, 240 - top, BG);
      shownFrame[c] = BG;
    }
  }
  for (int c = 0; c < CELLS; c++) {
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
  bool editing = selected == STATUS_ROW && selectedCell == TEMPO_CELL;
  int tagY = top + (ROW_BAND - TAG_H) / 2, textY = top + (ROW_BAND - FONT_H) / 2;
  drawTag(WAVE_X + INSET, tagY, text, editing ? TAG_INVERTED : TAG_PLAIN, editing ? INK : DIM, BG, TEMPO_W);

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
  int right = WAVE_X + WAVE_W - INSET, w = textWidth(text);
  t.fillRect(right - CLOCK_W, textY, CLOCK_W - w, FONT_H, BG);   // what longer text left
  drawText(right - w, textY, text, running ? INK : DIM, BG);

  // The scene.
  const Cell &cell = CELL[1];
  drawTag(cell.x + INSET, tagY, "SCN", TAG_INVERTED, INK, BG);
  int scene = sceneCurrent(), count = sceneCount();
  bool temporary = scene == 0 || scene > count;
  snprintf(text, sizeof text, "%d%s/%d", scene, temporary ? "*" : "", count);
  uint16_t ink = temporary ? DIM : INK;
  if (sceneHeld() || sceneChangeWaiting()) {
    noInterrupts();
    bool firstHalf = transport.sampleInBeat < transport.spb / 2;
    interrupts();
    ink = firstHalf ? INK : DIM;
  }
  int x = cell.x + INSET + tagWidth("SCN") + PX;
  drawText(x, textY, text, ink, BG, cell.x + cell.w - INSET - x);
}
