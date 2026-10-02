// ui_status.cpp — the status row: tempo, clock and scene (see ui.h)
#include "ui.h"

// Status row, in the pixel font, each cell framed like the tracks' when the
// row is selected:
//   first cell (under the waveforms): the tempo on the left (dim; inverted
//     while it's being edited), the clock (bar.beat.step) on the right
//     (white while running, a dim 1.1.1 while stopped);
//   second cell (the filter and volume columns): the scene, an inverted "SCN"
//     tag and its position "3/7"
//     (0* and N+1* with a star, grey). The position flashes with the beat
//     (the accent colour, then grey) while another scene is selected but not
//     there yet: SHIFT still held on it, or the change waiting for its switch point.
// Text and tags are redrawn whole each pass (their boxes clear what was
// there); the frames only when they change.
static constexpr int32_t CLOCK_BARS = 256;        // the clock wraps from 256.4.4 to 1.1.1
static constexpr int INSET = 2 * PX;               // text from the cell's edges (inside the frame)
static constexpr int CLOCK_W = 24 * PX;           // its box (clearing longer text)

// The status row's cells: the tempo's, and the scene's spanning two columns.
static constexpr int STATUS_CELLS = SCENE_CELL + 1;
static const Cell &statusCell(int cell) { return cell == SCENE_CELL ? SCENE_BOX : CELL[cell]; }

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
  bool editing = selected == STATUS_ROW && selectedCell == TEMPO_CELL;
  int tagY = top + (ROW_BAND - TAG_H) / 2, textY = top + (ROW_BAND - FONT_H) / 2;
  static const int tempoW = tagWidth("888.8 BPM");   // the widest tempo: its box clears any other
  drawTag(WAVE_X + INSET, tagY, text, editing ? TAG_INVERTED : TAG_PLAIN, editing ? INK : DIM, BG, tempoW);

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

  // The scene. Its box reaches the cell's inner edge: the position's text
  // (up to "250*/250") always fits, and clears any longer text before it.
  const Cell &cell = SCENE_BOX;
  drawTag(cell.x + INSET, tagY, "SCN", TAG_INVERTED, INK, BG);
  int scene = sceneCurrent(), count = sceneCount();
  bool temporary = scene == 0 || scene > count;
  snprintf(text, sizeof text, "%d%s/%d", scene, temporary ? "*" : "", count);
  uint16_t ink = temporary ? DIM : INK;
  if (sceneHeld() || sceneChangeWaiting()) {
    noInterrupts();
    bool firstHalf = transport.sampleInBeat < transport.spb / 2;
    interrupts();
    ink = firstHalf ? ACCENT : DIM;
  }
  int x = cell.x + INSET + tagWidth("SCN") + PX;
  drawText(x, textY, text, ink, BG, cell.x + cell.w - INSET - x);
}
