// ui.cpp — drawing rows, and the screen refresh pass (see ui.h)
#include "ui.h"

void drawTrack(int i) {
  if (i < 0 || i >= ROWS) return;
  drawCells(i);
  if (i == STATUS_ROW) {
    drawStatus();
    return;
  }
  drawFilter(i);
  drawMix(i);
  drawWave(i, recordingOn(i));
}

void uiRefresh() {
  // Redraw a little at a time so SD streaming keeps going.
  static uint32_t lastDraw = 0;
  static int nextRow = 0;
  if (millis() - lastDraw >= 30) {
    lastDraw = millis();
    // Every pass, all rows' buttons (only what changed is redrawn): the
    // recording track's icon flashes; the status row's follows SHIFT, the
    // selection and the transport.
    for (int i = 0; i < ROWS; i++) drawCells(i);
    drawStatus();                                    // every pass: the clock shows steps
    updatePlayheads();
    for (int i = 0; i < NUM_TRACKS; i++) drawMix(i);   // the meters
    drawTrack(nextRow);
    if (++nextRow >= ROWS) nextRow = 0;
  }
}
