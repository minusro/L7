// ui_wave.cpp — waveforms, loop range markers and playheads (see ui.h)
#include "ui.h"

// ---- Waveforms
// Each track's whole loop in WAVE_COLS columns of blocks, inside the cell's
// frame column on either side. Rows of blocks (13 in the band):
//   0, 12   the frame (selected row); on the last track row 12 is the
//           divider when there's no frame
//   1       the loop range: a bracket over it, its ends down to row 2
//   2 - 10  the waveform: a line along row 6, the peaks reaching up to 4
//           blocks above and below it
//   11      bar ticks
// The waveform is white inside the loop range and dark grey outside; on a
// muted clip the range is 50% grey and the rest dotted dark grey. The bracket,
// the start position (a dotted line) and the bar ticks (dark grey) only show
// on the selected track. The playhead inverts its column (white on black,
// black on white); on a muted clip, only when selected, it's a grey line. A
// recording track's cell is in the accent colour, its waveform growing in
// black (live view), the record head inverting its column too.
// Full redraws only happen when something changes; the playhead moves by
// redrawing just its columns.

static constexpr int BLOCK_COLS = WAVE_W / PX;                  // 66, the frame's included
static constexpr int WAVE_COLS = BLOCK_COLS - 2;               // 64 waveform columns
static constexpr int BAND_ROWS = ROW_BAND / PX;                // 13
static constexpr int MID_ROW = 6, WAVE_REACH = 4;              // rows 2-10
static constexpr int TICK_ROW = MID_ROW + WAVE_REACH + 1;      // 11

struct WaveView {
  bool visible = false;                            // something is shown (not an empty track)
  bool live = false;                               // recording in progress: live view
  uint16_t bg = BG, frame = BG;
  int rangeFirst = 0, rangeCols = 0;               // columns inside the loop range: rangeFirst on, wrapping
  int markCol = -1, beginCol = -1, endCol = -1;    // start position, range's first and last columns; -1: none
  int playCol = -1;                                // playhead / record head; -1: none
  uint16_t headColor = 0;                          // PLAYHEAD: inverts the column; else a line this colour
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

// A column holds a bar tick if a bar boundary (other than the start) falls in it.
static bool isBarColumn(uint32_t bars, int c) {
  uint32_t b0 = ((uint64_t)c * bars + WAVE_COLS - 1) / WAVE_COLS;   // first boundary >= column start
  return b0 > 0 && b0 < bars && (uint64_t)b0 * WAVE_COLS < (uint64_t)(c + 1) * bars;
}

// Level shown in column c: the peak of the stored overview columns
// (OVERVIEW_COLS of them) that it covers.
static uint8_t columnLevel(const uint8_t *src, int c) {
  int a = c * OVERVIEW_COLS / WAVE_COLS, b = max(a + 1, (c + 1) * OVERVIEW_COLS / WAVE_COLS);
  uint8_t m = 0;
  for (int k = a; k < b; k++) m = max(m, src[k]);
  return m;
}

// Block column bc (0 and BLOCK_COLS - 1: the frame's), drawn whole.
static void drawColumn(int i, int bc) {
  if (bc < 0 || bc >= BLOCK_COLS) return;
  const Track &tr = tracks[i];
  const WaveView &v = views[i];
  uint16_t col[BAND_ROWS];
  for (auto &c : col) c = v.bg;
  if (v.frame != BG) {                             // the frame, its corners cut (black on a coloured cell)
    uint16_t frame = v.bg != BG ? BG : v.frame;
    if (bc == 0 || bc == BLOCK_COLS - 1) for (int r = 1; r < BAND_ROWS - 1; r++) col[r] = frame;
    else col[0] = col[BAND_ROWS - 1] = frame;
  } else if (i == DIVIDER_ROW) {
    col[BAND_ROWS - 1] = DIVIDER;
  }
  int c = bc - 1;                                  // waveform column
  bool selectedTrack = i == selected;
  if (c >= 0 && c < WAVE_COLS && v.visible) {
    bool muted = !v.live && tr.state == TrackState::Muted;
    bool in = v.live || (c - v.rangeFirst + WAVE_COLS) % WAVE_COLS < v.rangeCols;
    if (!v.live || c < v.liveFilled) {
      uint16_t ink = v.live ? BG : !in ? DARK : muted ? DIM : INK;   // (live: on the coloured cell)
      int h = (columnLevel(v.live ? v.liveData : tr.overview, c) * WAVE_REACH + 127) / 255;
      for (int r = MID_ROW - h; r <= MID_ROW + h; r++)
        if (in || !muted || (c + r) % 2 == 0) col[r] = ink;   // muted, outside: every other block
    }
    if (selectedTrack && !v.live) {
      uint16_t mark = muted ? DIM : INK;
      if (in) col[1] = mark;                                      // the bracket
      if (c == v.beginCol || c == v.endCol) col[2] = mark;        // its ends
      if (isBarColumn(tr.loopBars, c)) col[TICK_ROW] = DARK;
      if (c == v.markCol)                                         // the start position: dotted, inverting
        for (int r = MID_ROW - WAVE_REACH; r < TICK_ROW; r += 2) col[r] = col[r] == v.bg ? mark : v.bg;
    }
    if (c == v.playCol) {                                         // on top: the playhead
      for (int r = 1; r < BAND_ROWS - 1; r++)
        col[r] = v.headColor != PLAYHEAD ? v.headColor           // (on a coloured cell: black on it, it on black)
               : col[r] == v.bg ? (v.bg == BG ? INK : BG) : v.bg;
    }
  }
  static uint16_t px[ROW_BAND * PX];
  for (int r = 0; r < BAND_ROWS; r++)
    for (int y = 0; y < PX; y++) px[(r * PX + y) * PX] = px[(r * PX + y) * PX + 1] = col[r];
  auto &t = board.tft();
  int x = WAVE_X + bc * PX, top = rowTop(i);
  if (bgTop(i) < top) t.fillRect(x, bgTop(i), PX, top - bgTop(i), v.bg);   // the first row's margin
  t.writeRect(x, top, PX, ROW_BAND, px);
}

void drawWave(int i, bool recordingHere) {
  const Track &tr = tracks[i];
  WaveView &v = views[i];
  uint32_t rs = 0, rb = 0;
  if (tr.state != TrackState::Empty) trackRange(i, rs, rb);
  uint16_t bg = cellBackground(i, WAVE_CELL), frame = cellFrame(i, WAVE_CELL);
  if (v.drawn == uiGeneration && v.version == tr.overviewVersion && v.state == (uint8_t)tr.state &&
      v.rec == recordingHere && v.rs == rs && v.rb == rb && v.bg == bg && v.frame == frame)
    return;
  v.drawn = uiGeneration;
  v.version = tr.overviewVersion;
  v.state = (uint8_t)tr.state;
  v.rec = recordingHere;
  v.rs = rs;
  v.rb = rb;
  v.bg = bg;
  v.frame = frame;
  v.live = recordingHere;
  v.visible = recordingHere || tr.state != TrackState::Empty;
  if (!v.visible || v.live) v.playCol = -1;
  v.rangeFirst = 0;
  v.rangeCols = 0;
  v.markCol = -1;
  v.beginCol = -1;
  v.endCol = -1;
  if (v.live) {
    v.liveBars = 0;                                // updatePlayheads() fills it in
    v.liveFilled = 0;
  } else if (v.visible) {
    uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
    v.rangeFirst = rs * WAVE_COLS / loopBeats;
    int end = (int)(((rs + rb) * WAVE_COLS + loopBeats - 1) / loopBeats);   // unwrapped
    v.rangeCols = constrain(end - v.rangeFirst, 1, WAVE_COLS);
    v.markCol = startPosition(loopBeats, rs, rb) * WAVE_COLS / loopBeats;
    v.beginCol = v.rangeFirst;
    // The range's last column; a whole-loop range's ends meet: one tick.
    v.endCol = v.rangeCols >= WAVE_COLS ? v.beginCol : (v.rangeFirst + v.rangeCols - 1) % WAVE_COLS;
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
  if (old >= 0 && old != col) drawColumn(i, old + 1);
  if (col >= 0) drawColumn(i, col + 1);
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
    v.headColor = PLAYHEAD;
    for (int bc = 0; bc < BLOCK_COLS; bc++) drawColumn(i, bc);
    return;
  }
  uint8_t before[WAVE_COLS];
  for (int c = 0; c < WAVE_COLS; c++) before[c] = columnLevel(v.liveData, c);
  memcpy(v.liveData, cols, OVERVIEW_COLS);
  int oldFilled = v.liveFilled;
  v.liveFilled = filled;
  for (int c = 0; c < WAVE_COLS; c++)
    if (columnLevel(v.liveData, c) != before[c] || (c < filled) != (c < oldFilled)) drawColumn(i, c + 1);
  movePlayhead(i, head, PLAYHEAD);
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
    // Unmuted: inverting. Muted: none, but a dim line on the selected track.
    bool muted = tr.state == TrackState::Muted;
    int col = -1;
    if (v.visible && running && tr.live && (!muted || i == selected)) {
      double inRange = fmod(pos - tr.rangeAnchorQ / 4.0, (double)tr.rangeBeats);
      if (inRange < 0) inRange += tr.rangeBeats;
      double loopBeat = fmod(tr.rangeStartBeat + inRange, (double)(tr.loopBars * BEATS_PER_BAR));
      col = min(WAVE_COLS - 1, (int)(loopBeat * WAVE_COLS / (tr.loopBars * BEATS_PER_BAR)));
    }
    movePlayhead(i, col, muted ? MUTED_HEAD : PLAYHEAD);
  }
}
