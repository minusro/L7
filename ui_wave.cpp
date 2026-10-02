// ui_wave.cpp — waveforms, loop range markers and playheads (see ui.h)
#include "ui.h"

// ---- Waveforms
// Each track's whole loop, drawn one column at a time: the row background
// (dim grey across the whole screen for the selected track, dark red while
// recording), the waveform (80% white inside the loop range, grey outside; muted
// clips dimmed by MUTED_WEIGHT) with faint darker bar lines at the clip's bar
// boundaries, and the playhead: a white line with a black one on its left
// on unmuted clips; on muted ones a single line like the range lines, only on the selected track. Full redraws only happen when
// something changes; the playhead moves by redrawing just its columns.
// While a track records it shows the recording growing (live view) with a
// red record head, over the length it would become.
// Outside the loop range: the same dark grey for playing and muted clips.
// (Colours: colors.h.)

struct WaveView {
  bool visible = false;                            // something is shown (not an empty track)
  bool live = false;                               // recording in progress: live view
  uint16_t bg = BG;
  int rangeFirst = 0, rangeCols = 0;               // columns inside the loop range: rangeFirst on, wrapping
  int markCol = -1, beginCol = -1, endCol = -1;    // start position flag, range's first and last columns; -1: none
  int playCol = -1;                                // playhead / record head; -1: none
  uint16_t headColor = 0;                          // playhead: playCol in this colour,
  bool headEdge = true;                            // and PLAYHEAD_EDGE on its left
  // live view
  uint32_t liveBars = 0;
  int liveFilled = 0;                              // columns with recorded audio
  uint8_t liveData[OVERVIEW_COLS] = {};
  // what the last full draw showed
  uint8_t version = 0, state = 0;
  bool rec = false, drawn = false;
  uint32_t rs = 0, rb = 0;
};
static WaveView views[NUM_TRACKS];

static int waveTop(int i) { return rowTop(i) + (ROW_BAND - WAVE_H) / 2; }

// A screen column holds a bar line if a bar boundary (other than the start)
// falls in it.
static bool isBarColumn(uint32_t bars, int c) {
  uint32_t b0 = ((uint64_t)c * bars + WAVE_W - 1) / WAVE_W;                 // first boundary >= column start
  return b0 > 0 && b0 < bars && (uint64_t)b0 * WAVE_W < (uint64_t)(c + 1) * bars;
}

// Level shown in screen column c: the peak of the stored overview columns
// (OVERVIEW_COLS of them) that it covers.
static uint8_t columnLevel(const uint8_t *src, int c) {
  int a = c * OVERVIEW_COLS / WAVE_W, b = max(a + 1, (c + 1) * OVERVIEW_COLS / WAVE_W);
  uint8_t m = 0;
  for (int k = a; k < b; k++) m = max(m, src[k]);
  return m;
}

static void drawColumnUnder(int i, int c) {
  auto &t = board.tft();
  const Track &tr = tracks[i];
  const WaveView &v = views[i];
  int x = WAVE_X + c, top = waveTop(i), mid = top + WAVE_H / 2;
  t.drawFastVLine(x, bgTop(i), bgHeight(i), v.bg);   // background: the row's full height
  if (i == DIVIDER_ROW && v.bg == BG) t.drawPixel(x, dividerY(), DIVIDER);
  if (!v.visible) return;
  if (v.live && c >= v.liveFilled) return;
  bool bar = isBarColumn(v.live ? v.liveBars : tr.loopBars, c);
  bool in = v.live || (c - v.rangeFirst + WAVE_W) % WAVE_W < v.rangeCols;
  uint16_t color = in ? (bar ? WAVE_IN_BAR : WAVE_IN) : (bar ? WAVE_OUT_BAR : WAVE_OUT);
  if (!v.live && in && tr.state == TrackState::Muted)
    color = blend565(color, BG, MUTED_WEIGHT);   // muted: the loop range dimmed
  int h = columnLevel(v.live ? v.liveData : tr.overview, c) * (WAVE_H / 2) / 255;
  t.drawFastVLine(x, mid - h, 2 * h + 1, color);
}

// Markers (behind the playhead; only on the selected track), 1 px lines down
// the row: the range's first and last columns,
// and the start position, which also has a small triangle at its top
// pointing right (MARK_FLAG rows tall, widest in the middle).
static constexpr int MARK_TOP = 1, MARK_FLAG = 5;

static void drawMark(int i, int c) {
  const WaveView &v = views[i];
  if (v.markCol < 0 || i != selected) return;
  auto &t = board.tft();
  uint16_t color = tracks[i].state == TrackState::Muted ? blend565(START_MARK, BG, MUTED_WEIGHT) : START_MARK;
  int x = WAVE_X + c, top = rowTop(i) + MARK_TOP, half = MARK_FLAG / 2;
  if (c == v.markCol || c == v.beginCol || c == v.endCol) t.drawFastVLine(x, top, ROW_BAND - 2 * MARK_TOP, color);
  int k = c - v.markCol;                           // the triangle, right of its line
  if (k > 0 && k <= half) t.drawFastVLine(x, top + k, MARK_FLAG - 2 * k, color);
}

static void drawColumn(int i, int c) {
  if (c < 0 || c >= WAVE_W) return;
  drawColumnUnder(i, c);
  drawMark(i, c);
  const WaveView &v = views[i];
  if (v.visible && v.playCol >= 0 && (c == v.playCol || (v.headEdge && c == v.playCol - 1)))   // on top, the row's full height
    board.tft().drawFastVLine(WAVE_X + c, rowTop(i), ROW_BAND, c == v.playCol ? v.headColor : PLAYHEAD_EDGE);
}

void drawWave(int i, bool recordingHere) {
  const Track &tr = tracks[i];
  WaveView &v = views[i];
  uint32_t rs = 0, rb = 0;
  if (tr.state != TrackState::Empty) trackRange(i, rs, rb);
  uint16_t bg = cellBackground(i, WAVE_CELL);
  if (v.drawn && v.version == tr.overviewVersion && v.state == (uint8_t)tr.state &&
      v.rec == recordingHere && v.rs == rs && v.rb == rb && v.bg == bg)
    return;
  v.drawn = true;
  v.version = tr.overviewVersion;
  v.state = (uint8_t)tr.state;
  v.rec = recordingHere;
  v.rs = rs;
  v.rb = rb;
  v.bg = bg;
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
    v.rangeFirst = rs * WAVE_W / loopBeats;
    int end = (int)(((rs + rb) * WAVE_W + loopBeats - 1) / loopBeats);   // unwrapped
    v.rangeCols = constrain(end - v.rangeFirst, 1, WAVE_W);
    v.markCol = startPosition(loopBeats, rs, rb) * WAVE_W / loopBeats;
    v.beginCol = v.rangeFirst;
    // The range's last column; a whole-loop range's edges meet: one line.
    v.endCol = v.rangeCols >= WAVE_W ? v.beginCol : (v.rangeFirst + v.rangeCols - 1) % WAVE_W;
  }
  for (int c = 0; c < WAVE_W; c++) drawColumn(i, c);
}

// Moves a view's playhead (or record head) to col with the given look,
// redrawing only the columns it leaves and covers.
static void movePlayhead(int i, int col, uint16_t color, bool edge = true) {
  WaveView &v = views[i];
  if (col == v.playCol && color == v.headColor && edge == v.headEdge) return;
  int old = v.playCol;
  v.playCol = col;
  v.headColor = color;
  v.headEdge = edge;
  // The columns it leaves (those it still covers draw as the new playhead), then the new ones.
  if (old >= 0) for (int c = old - 1; c <= old; c++) if (col < 0 || c < col - 1 || c > col) drawColumn(i, c);
  if (col >= 0) for (int c = col - 1; c <= col; c++) drawColumn(i, c);
}

// Live view of the recording: new columns as audio is written, a full
// redraw when the shown length grows, and the record head.
static void updateLive(int i) {
  WaveView &v = views[i];
  uint8_t cols[OVERVIEW_COLS];
  uint32_t bars, framesPerBar, recorded;
  if (!recordingWaveform(cols, bars, framesPerBar, recorded)) return;
  uint32_t total = bars * framesPerBar;
  int filled = min(WAVE_W, (int)(((uint64_t)recorded * WAVE_W + total - 1) / total));
  noInterrupts();
  int32_t sofar = (int32_t)(transport.now - recording.startSample);
  interrupts();
  int head = sofar < 0 ? -1 : min(WAVE_W - 1, (int)((uint64_t)sofar * WAVE_W / total));
  uint16_t headColor = REC_HEAD;
  if (tracks[i].live) {
    // The loop already plays (the rest of the recording is still being
    // saved): its playhead instead of the record head.
    noInterrupts();
    double pos = transport.beat + (double)transport.sampleInBeat / transport.spb;
    interrupts();
    double loopBeats = bars * BEATS_PER_BAR, beat = fmod(pos - recording.startBeat, loopBeats);
    if (beat < 0) beat += loopBeats;
    head = min(WAVE_W - 1, (int)(beat * WAVE_W / loopBeats));
    headColor = PLAYHEAD;
  }
  if (bars != v.liveBars) {                        // new scale: redraw everything
    v.liveBars = bars;
    v.liveFilled = filled;
    memcpy(v.liveData, cols, OVERVIEW_COLS);
    v.playCol = head;
    v.headColor = headColor;
    v.headEdge = true;
    for (int c = 0; c < WAVE_W; c++) drawColumn(i, c);
    return;
  }
  uint8_t before[WAVE_W];
  for (int c = 0; c < WAVE_W; c++) before[c] = columnLevel(v.liveData, c);
  memcpy(v.liveData, cols, OVERVIEW_COLS);
  int oldFilled = v.liveFilled;
  v.liveFilled = filled;
  for (int c = 0; c < WAVE_W; c++)
    if (columnLevel(v.liveData, c) != before[c] || (c < filled) != (c < oldFilled)) drawColumn(i, c);
  movePlayhead(i, head, headColor);
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
    // Unmuted: white (black edge). Muted: none, but a line on the selected track.
    bool muted = tr.state == TrackState::Muted;
    int col = -1;
    if (v.visible && running && tr.live && (!muted || i == selected)) {
      double inRange = fmod(pos - tr.rangeAnchorQ / 4.0, (double)tr.rangeBeats);
      if (inRange < 0) inRange += tr.rangeBeats;
      double loopBeat = fmod(tr.rangeStartBeat + inRange, (double)(tr.loopBars * BEATS_PER_BAR));
      col = min(WAVE_W - 1, (int)(loopBeat * WAVE_W / (tr.loopBars * BEATS_PER_BAR)));
    }
    movePlayhead(i, col, muted ? MUTED_HEAD : PLAYHEAD, !muted);   // muted: a single line
  }
}
