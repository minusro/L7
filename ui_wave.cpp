// ui_wave.cpp — waveforms, loop range markers and playheads (see ui.h)
#include "ui.h"

// ---- Waveforms
// Each track's whole loop in WAVE_COLS columns of blocks, inside the cell's
// border (shared with its neighbours: the selection frame) and a block of
// space. Rows of blocks (14 in the cell):
//   0, 13   the border
//   1, 12   space (the playheads reach into it)
// A black line the waveform's height just before each bar's start (but the
// first's), on every track.
//   2 - 11  the waveform: a line along rows 6-7, the peaks reaching up to 4
//           blocks above and below it. Heights go with the square root of
//           the peak level (as the overview stores it).
// The waveform is light grey inside the loop range (white on the selected
// cell) and 50% grey outside; on a muted clip the range is 50% grey and the
// rest dark grey. While B alone is held on the selected waveform cell (B +
// arrows change the range), the range is in the accent colour, muted or not.
// The start position: a dotted line the waveform's height (a block of the
// waveform's colour there every third row, black between), on every track. The playhead is an accent-coloured line (white
// while the range is in the accent colour); on a muted clip a black one
// (cutting through its waveform).
// A recording track's cell is in the accent colour, its waveform growing in
// black (live view), the record head inverting its column.
// Full redraws only happen when something changes; the playhead moves by
// redrawing just its columns.

static constexpr int BLOCK_COLS = WAVE_W / PX;                  // 94, the border's included
static constexpr int WAVE_LEFT = 2;                            // the first waveform column (after the border and a space)
static constexpr int WAVE_COLS = BLOCK_COLS - 2 * WAVE_LEFT;   // 90 waveform columns
static constexpr int BAND_ROWS = CELL_H / PX;                  // 14
static constexpr int MID_ROW = 6, WAVE_REACH = 4;              // the line: rows 6-7; peaks: rows 2-11

// Waveform height (blocks each way) for a stored overview level (the square
// root of the peak, 0-255): in proportion.
static uint8_t reachFor(uint8_t level) { return (uint8_t)((level * WAVE_REACH + 127) / 255); }

struct WaveView {
  bool visible = false;                            // something is shown (not an empty track)
  bool live = false;                               // recording in progress: live view
  uint16_t bg = BG;
  uint64_t borders = 0;                            // the border's sides (bordersOf())
  int rangeFirst = 0, rangeCols = 0;               // columns inside the loop range: rangeFirst on, wrapping
  int markCol = -1;                                // the start position's column; -1: none
  int playCol = -1;                                // playhead / record head; -1: none
  uint16_t headColor = 0;                          // a line this colour; INVERT_HEAD: the column inverted
  uint16_t rangeInk = INK;                         // the waveform inside the loop range
  uint16_t outInk = DARK;                          // the waveform outside the loop range
  // live view
  uint32_t liveBars = 0;
  int liveFilled = 0;                              // columns with recorded audio
  uint8_t liveData[OVERVIEW_COLS] = {};
  // what the last full draw showed
  uint8_t version = 0, state = 0;
  bool rec = false;
  uint32_t drawn = 0;                              // uiGeneration when last drawn whole
  uint32_t rs = 0, rb = 0;
};
static WaveView views[NUM_TRACKS];
// The record head's look (not a colour the playheads use): its column inverted.
static constexpr uint16_t INVERT_HEAD = 0x0001;

// Level shown in column c: the peak of the stored overview columns
// (OVERVIEW_COLS of them) that it covers.
static uint8_t columnLevel(const uint8_t *src, int c) {
  int a = c * OVERVIEW_COLS / WAVE_COLS, b = max(a + 1, (c + 1) * OVERVIEW_COLS / WAVE_COLS);
  uint8_t m = 0;
  for (int k = a; k < b; k++) m = max(m, src[k]);
  return m;
}

// The column edge nearest beat `beat` of a loop of loopBeats (0..WAVE_COLS;
// past the loop's end for beats past it): column edge k is between columns
// k - 1 and k. Range edges, bar starts and the start position all snap to
// these, so they agree.
static int edgeCol(uint32_t beat, uint32_t loopBeats) {
  return (int)(((uint64_t)beat * WAVE_COLS + loopBeats / 2) / loopBeats);
}

// The start position's line in waveform column c of view v: the waveform's
// height (rows 2-11), a block of the waveform's colour there every third
// row from the top (2, 5, 8, 11), black between; BG: no line there.
static constexpr int START_TOP = MID_ROW - WAVE_REACH, START_BOTTOM = MID_ROW + 1 + WAVE_REACH;   // rows 2-11
static uint16_t startInk(const WaveView &v, int c) {
  if (!v.visible || v.live || c != v.markCol) return BG;
  bool in = (c - v.rangeFirst + WAVE_COLS) % WAVE_COLS < v.rangeCols;
  return in ? v.rangeInk : v.outInk;
}

// Block column bc (0 and BLOCK_COLS - 1: the frame's), drawn whole.
static void drawColumn(int i, int bc) {
  if (bc < 0 || bc >= BLOCK_COLS) return;
  const Track &tr = tracks[i];
  const WaveView &v = views[i];
  uint16_t col[BAND_ROWS];
  for (int r = 0; r < BAND_ROWS; r++) {           // the border (the frame), round the background
    int b = borderAt(i, WAVE_CELL, r, bc, BAND_ROWS, BLOCK_COLS);
    col[r] = b >= 0 ? (uint16_t)b : v.bg;
  }
  int c = bc - WAVE_LEFT;                          // waveform column
  if (c >= 0 && c < WAVE_COLS && v.visible) {
    bool in = v.live || (c - v.rangeFirst + WAVE_COLS) % WAVE_COLS < v.rangeCols;
    if (!v.live || c < v.liveFilled) {
      uint16_t ink = v.live ? BG : !in ? v.outInk : v.rangeInk;   // (live: on the coloured cell)
      int h = reachFor(columnLevel(v.live ? v.liveData : tr.overview, c));
      for (int r = MID_ROW - h; r <= MID_ROW + 1 + h; r++)
        col[r] = ink;
    }
    if (!v.live)                                                  // bars 2 on: a black line just before each start
      for (int b = 1; b < (int)tr.loopBars; b++)
        if (edgeCol(b * BEATS_PER_BAR, tr.loopBars * BEATS_PER_BAR) == c + 1) {
          for (int r = MID_ROW - WAVE_REACH; r <= MID_ROW + 1 + WAVE_REACH; r++) col[r] = v.bg;
          break;
        }
    if (uint16_t ink = startInk(v, c); ink != BG)                 // the start position: a checkered line
      for (int r = START_TOP; r <= START_BOTTOM; r++) col[r] = (r - START_TOP) % 3 ? v.bg : ink;
    if (c == v.playCol) {                                         // on top: the playhead
      for (int r = MID_ROW - WAVE_REACH - 1; r <= MID_ROW + 2 + WAVE_REACH; r++)   // (a block past the waveform each way)
        col[r] = v.headColor != INVERT_HEAD ? v.headColor           // (on a coloured cell: black on it, it on black)
               : col[r] == v.bg ? (v.bg == BG ? INK : BG) : v.bg;
    }
  }
  static uint16_t px[CELL_H * PX];
  for (int r = 0; r < BAND_ROWS; r++)
    for (int y = 0; y < PX; y++) px[(r * PX + y) * PX] = px[(r * PX + y) * PX + 1] = col[r];
  auto &t = board.tft();
  int x = WAVE_X + bc * PX, top = rowTop(i);
  t.writeRect(x, top, PX, CELL_H, px);
}

void drawWave(int i, bool recordingHere) {
  const Track &tr = tracks[i];
  WaveView &v = views[i];
  uint32_t rs = 0, rb = 0;
  if (tr.state != TrackState::Empty) trackRange(i, rs, rb);
  uint16_t bg = cellBackground(i, WAVE_CELL);
  uint64_t borders = bordersOf(i, WAVE_CELL);
  // The range: light grey, white on the selected cell, the accent colour while
  // B alone is held on it (editing, muted or not); muted: grey.
  bool muted = tr.state == TrackState::Muted, here = i == selected && selectedCell == WAVE_CELL;
  bool editing = here && bAlone();
  uint16_t rangeInk = editing ? ACCENT : muted ? DIM : here ? INK : LIGHT;
  uint16_t outInk = muted ? DARK : DIM;            // outside it: grey (muted: dark grey)
  if (v.drawn == uiGeneration && v.version == tr.overviewVersion && v.state == (uint8_t)tr.state &&
      v.rec == recordingHere && v.rs == rs && v.rb == rb && v.bg == bg && v.borders == borders &&
      v.rangeInk == rangeInk && v.outInk == outInk)
    return;
  v.drawn = uiGeneration;
  v.version = tr.overviewVersion;
  v.state = (uint8_t)tr.state;
  v.rec = recordingHere;
  v.rs = rs;
  v.rb = rb;
  v.bg = bg;
  v.borders = borders;
  v.rangeInk = rangeInk;
  v.outInk = outInk;
  v.live = recordingHere;
  v.visible = recordingHere || tr.state != TrackState::Empty;
  if (!v.visible || v.live) v.playCol = -1;
  v.rangeFirst = 0;
  v.rangeCols = 0;
  v.markCol = -1;
  if (v.live) {
    v.liveBars = 0;                                // updatePlayheads() fills it in
    v.liveFilled = 0;
  } else if (v.visible) {
    uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
    // (Range edges, bar starts and the start position all on the same column
    // edges: the range's colour stops exactly at the bar lines.)
    int first = edgeCol(rs, loopBeats), end = edgeCol(rs + rb, loopBeats);   // (end unwrapped)
    v.rangeFirst = first % WAVE_COLS;
    v.rangeCols = constrain(end - first, 1, WAVE_COLS);
    v.markCol = edgeCol(startPosition(loopBeats, rs, rb), loopBeats) % WAVE_COLS;
  }
  for (int bc = 0; bc < BLOCK_COLS; bc++) drawColumn(i, bc);
}

// Moves a view's playhead (or record head) to col with the given look,
// redrawing only the columns it leaves and covers.
static void movePlayhead(int i, int col, uint16_t color) {
  WaveView &v = views[i];
  if (col == v.playCol && color == v.headColor) return;
  int old = v.playCol;
  v.playCol = col;
  v.headColor = color;
  if (old >= 0 && old != col) drawColumn(i, old + WAVE_LEFT);
  if (col >= 0) drawColumn(i, col + WAVE_LEFT);
}

// Live view of the recording: new columns as audio is written, a full
// redraw when the shown length grows, and the record head.
static void updateLive(int i) {
  WaveView &v = views[i];
  uint8_t cols[OVERVIEW_COLS];
  uint32_t bars, framesPerBar, recorded;
  if (!recordingWaveform(cols, bars, framesPerBar, recorded)) return;
  uint32_t total = bars * framesPerBar;
  int filled = min(WAVE_COLS, (int)(((uint64_t)recorded * WAVE_COLS + total - 1) / total));
  noInterrupts();
  int32_t sofar = (int32_t)(transport.now - recording.startSample);
  interrupts();
  int head = sofar < 0 ? -1 : min(WAVE_COLS - 1, (int)((uint64_t)sofar * WAVE_COLS / total));
  if (tracks[i].live) {
    // The loop already plays (the rest of the recording is still being
    // saved): its playhead instead of the record head.
    noInterrupts();
    double pos = transport.beat + (double)transport.sampleInBeat / transport.spb;
    interrupts();
    double loopBeats = bars * BEATS_PER_BAR, beat = fmod(pos - recording.startBeat, loopBeats);
    if (beat < 0) beat += loopBeats;
    head = min(WAVE_COLS - 1, (int)(beat * WAVE_COLS / loopBeats));
  }
  if (bars != v.liveBars) {                        // new scale: redraw everything
    v.liveBars = bars;
    v.liveFilled = filled;
    memcpy(v.liveData, cols, OVERVIEW_COLS);
    v.playCol = head;
    v.headColor = INVERT_HEAD;
    for (int bc = 0; bc < BLOCK_COLS; bc++) drawColumn(i, bc);
    return;
  }
  uint8_t before[WAVE_COLS];
  for (int c = 0; c < WAVE_COLS; c++) before[c] = columnLevel(v.liveData, c);
  memcpy(v.liveData, cols, OVERVIEW_COLS);
  int oldFilled = v.liveFilled;
  v.liveFilled = filled;
  for (int c = 0; c < WAVE_COLS; c++)
    if (columnLevel(v.liveData, c) != before[c] || (c < filled) != (c < oldFilled)) drawColumn(i, c + WAVE_LEFT);
  movePlayhead(i, head, INVERT_HEAD);              // (on the coloured cell)
}

// Playhead column for each track from the transport position and the
// track's range alignment.
void updatePlayheads() {
  noInterrupts();
  bool running = transport.running;
  double pos = transport.beat + (double)transport.sampleInBeat / transport.spb;   // in beats
  interrupts();
  for (int i = 0; i < NUM_TRACKS; i++) {
    const Track &tr = tracks[i];
    WaveView &v = views[i];
    // Recording started or ended: redraw the row now (its cells change too),
    // not on its next turn.
    if (v.live != recordingOn(i)) drawTrack(i);
    if (v.live) { updateLive(i); continue; }
    // The accent colour (white on a range in the accent colour); muted: black.
    bool muted = tr.state == TrackState::Muted;
    int col = -1;
    if (v.visible && running && tr.live) {
      double inRange = fmod(pos - tr.rangeAnchorQ / 4.0, (double)tr.rangeBeats);
      if (inRange < 0) inRange += tr.rangeBeats;
      // Spread over the range's columns (as the range is drawn): never past its edges.
      uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
      int first = edgeCol(tr.rangeStartBeat, loopBeats);
      int cols = max(1, edgeCol(tr.rangeStartBeat + tr.rangeBeats, loopBeats) - first);
      col = (first + min(cols - 1, (int)(inRange * cols / tr.rangeBeats))) % WAVE_COLS;
    }
    movePlayhead(i, col, muted ? MUTED_HEAD : v.rangeInk == ACCENT ? INK : PLAYHEAD);
  }
}
