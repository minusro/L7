// ui_status.cpp — the status row: tempo and clock (see ui.h)
#include "ui.h"

extern "C" const unsigned char glcdfont[];     // ILI9341_t3n's built-in 5x7 font

// Status row text, in its first cell: the tempo and the clock (Bar.Beat.Step)
// STATUS_INSET px inside the waveforms' left and right edges. The tempo is
// white while its cell is selected, grey otherwise; the clock white while
// running, a grey 1.1.1 while stopped. The font's glyphs carry empty columns
// left and right, so text is shifted so its first/last lit pixel lands exactly
// on the edge.
static int glyphFirstColumn(char c) {
  int k = 0;
  while (k < 5 && !glcdfont[(uint8_t)c * 5 + k]) k++;
  return k;
}
static int glyphLastColumn(char c) {
  int k = 4;
  while (k > 0 && !glcdfont[(uint8_t)c * 5 + k]) k--;
  return k;
}

// Text whose first lit pixel is at x, padded to `cols` characters so shorter
// text clears longer text; the pixels it shifts over (back to clipLeft) are cleared.
static void printFromLeft(int x, int clipLeft, const char *text, int cols, uint16_t color, uint16_t bg) {
  auto &t = board.tft();
  int left = x - glyphFirstColumn(text[0]) * HEADER_SCALE;
  if (left > clipLeft) t.fillRect(clipLeft, TEXT_Y, left - clipLeft, TEXT_H, bg);
  t.setTextColor(color, bg);
  t.setCursor(left, TEXT_Y);
  t.printf("%-*s", cols, text);
}

// Text whose last lit pixel is at x, padded on the left to `cols` characters.
static void printToRight(int x, const char *text, int cols, uint16_t color, uint16_t bg) {
  auto &t = board.tft();
  int n = strlen(text);
  int left = x - ((cols - 1) * 6 + glyphLastColumn(text[n - 1])) * HEADER_SCALE;
  t.setTextColor(color, bg);
  t.setCursor(left, TEXT_Y);
  t.printf("%*s", cols, text);
}

static constexpr int32_t CLOCK_BARS = 256;        // the clock wraps from 256.4.4 to 1.1.1
static constexpr int STATUS_INSET = 5;             // tempo/clock this far inside the waveforms' edges

void drawStatus() {
  auto &t = board.tft();
  t.setTextSize(HEADER_SCALE);
  uint16_t bg = cellBackground(STATUS_ROW, TEMPO_CELL);
  char text[24];
  // A scene's tempo waiting for its switch point: the one playing until then.
  noInterrupts();
  bool tempoWaiting = transport.pendingSpb && transport.spbAtBeat >= 0;
  uint32_t playingSpb = transport.spb;
  interrupts();
  float shown = tempoWaiting ? roundf(bpmForSpb(playingSpb) * 10) / 10 : bpm;
  snprintf(text, sizeof text, "%.1f BPM", shown);
  bool editing = selected == STATUS_ROW && selectedCell == TEMPO_CELL;
  printFromLeft(WAVE_X + STATUS_INSET, CELL[TEMPO_CELL].x, text, 10, editing ? TEXT_ACTIVE : TEXT_IDLE, bg);
  noInterrupts();
  int32_t beat = transport.beat;
  uint32_t step = (uint64_t)transport.sampleInBeat * 4 / transport.spb;   // quarter beats
  bool running = transport.running;
  interrupts();
  if (running)
    snprintf(text, sizeof text, "%ld.%ld.%lu", beat / BEATS_PER_BAR % CLOCK_BARS + 1, beat % BEATS_PER_BAR + 1, step + 1);
  else
    snprintf(text, sizeof text, "1.1.1");          // where it starts from
  printToRight(WAVE_X + WAVE_W - 1 - STATUS_INSET, text, 10, running ? TEXT_ACTIVE : TEXT_IDLE, bg);

  // The scene, in the next cell: "SCN 3/7"; the empty scene and the copy
  // after the last (not stored yet) with a star, in grey. Flashing with the
  // beat (white, then dim, each beat) while another scene is selected but not
  // there yet: SHIFT still held on it, or the change waiting for its switch point.
  int scene = sceneCurrent(), count = sceneCount();
  bool temporary = scene == 0 || scene > count;
  snprintf(text, sizeof text, "SCN %d%s/%d", scene, temporary ? "*" : "", count);
  const Cell &cell = CELL[1];
  uint16_t color = temporary ? TEXT_IDLE : TEXT_ACTIVE;
  if (sceneHeld() || sceneChangeWaiting()) {
    noInterrupts();
    bool firstHalf = transport.sampleInBeat < transport.spb / 2;
    interrupts();
    color = firstHalf ? TEXT_ACTIVE : TEXT_WAITING;
  }
  printFromLeft(cell.x + STATUS_INSET, cell.x, text, 12, color, cellBackground(STATUS_ROW, 1));
}
