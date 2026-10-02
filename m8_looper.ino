// m8_looper.ino — 8-track stereo looper for the Dirtywave M8 Model:01
//
// Engine milestone with a debug screen. engine.cpp runs the master clock,
// varispeed playback, input monitoring and MIDI clock; storage.cpp streams
// the tracks from SD and records new ones. Tracks are WAV files in /looper on
// the SD card and survive power-off, along with the tempo.
//
// Behaviour:
//   * the input (LINE IN) is always heard
//   * recording is free length; the loop snaps to a power of two bars (or
//     shrinks to the one below if you stopped less than a beat past it), and
//     starts on the bar line you were nearest to
//   * changing the tempo repitches the loops (50%-200% of each one's tempo)
//   * each track can loop any range of whole bars of its recording
//   * MIDI clock + start/stop go out on serial MIDI port 1
//
// Keys:
//   UP/DOWN          select a row: tracks 1-8, then the status row
//   LEFT/RIGHT       select a cell (third cell: waveform; on the status row, tempo)
//   OPT twice        reset the current cell (waveform: whole loop; filter: open;
//                      volume: unity, balance centred)
//   OPT + arrows     change the current cell:
//                      waveform: LEFT/RIGHT move the loop range by a beat,
//                                UP/DOWN double/halve it (1/4 bar to the whole loop)
//                      filter: LEFT/RIGHT move the band, UP/DOWN widen/narrow it
//                      volume: LEFT/RIGHT balance, UP/DOWN volume
//                      tempo: LEFT/RIGHT -/+ 0.1 BPM, UP/DOWN +/- 1 BPM
//   EDIT             empty track: record / stop recording; full track: mute/unmute
//   SHIFT+EDIT       clear the track
//   PLAY             start from 1.1.1 / stop (also EDIT on the status row)
//   SHIFT+PLAY       start from 1.1.1, also while running (also SHIFT+EDIT on the status row)
//   power (hold)     switch off
//
// Build: Teensyduino, board "Teensy 4.0", USB type "Serial", CPU speed 600 MHz.
// Libraries: ILI9341_t3n (KurtE), Audio, SD (bundled with Teensyduino).

#include <Audio.h>
#include <SD.h>
#include "M8Board.h"
#include "engine.h"
#include "storage.h"
#include "colors.h"

using namespace looper;

m8::Board board;

AudioInputI2S        i2sIn;
LooperEngine         engine;
AudioOutputI2S       i2sOut;
AudioConnection      c1(i2sIn, 0, engine, 0), c2(i2sIn, 1, engine, 1);
AudioConnection      c3(engine, 0, i2sOut, 0), c4(engine, 1, i2sOut, 1);
AudioControlSGTL5000 codec;

// ---- Freeze detector (debugging): loop() and the audio interrupt both count
// their progress; a low-priority timer reports on Serial if either stalls for
// 1.5 s, naming the step loop() was in. It can only print while interrupts
// still run, so silence after a freeze means interrupts were off or it crashed
// (then CrashReport prints at the next boot).
static volatile uint32_t loopCount = 0;
static IntervalTimer freezeTimer;

static void freezeCheck() {
  static uint32_t lastLoop = 0, lastNow = 0, loopStill = 0, audioStill = 0;
  uint32_t lc = loopCount, now = transport.now;
  if (lc == lastLoop) {
    if (++loopStill == 15) Serial.printf("FREEZE: loop() stuck for 1.5 s in: %s\n", (const char *)debugStage);
  } else {
    if (loopStill >= 15) Serial.printf("loop() resumed after %lu ms\n", loopStill * 100);
    loopStill = 0;
    lastLoop = lc;
  }
  if (now == lastNow) {
    if (++audioStill == 15) Serial.printf("FREEZE: audio interrupt stopped (loop in: %s)\n", (const char *)debugStage);
  } else {
    if (audioStill >= 15) Serial.printf("audio resumed after %lu ms\n", audioStill * 100);
    audioStill = 0;
    lastNow = now;
  }
}

static float bpm = 120.0f;
static int selected = 0;                           // row: a track, or STATUS_ROW
static int selectedCell = 2;                       // cell in a track row (2 = waveform)
static uint32_t tempoChangedMs = 0;
static bool tempoDirty = false;

// ---- Tempo: every loop must stay within 50%-200% of its recorded speed
static bool setTempo(float b) {
  b = constrain(roundf(b * 10) / 10, MIN_BPM, MAX_BPM);   // tenths of a BPM
  if (recording.mode != Recording::Idle) return false;   // a recording keeps its tempo
  uint32_t spb = spbForBpm(b);
  for (const auto &tr : tracks) {
    if (tr.state == TrackState::Empty) continue;
    double speed = (double)tr.spbNative / spb;
    if (speed < MIN_SPEED || speed > MAX_SPEED) return false;
  }
  bpm = b;
  transport.pendingSpb = spb;
  tempoChangedMs = millis();
  tempoDirty = true;
  return true;
}

// ---- Screen (drawn over its own background: no full clears, which would
// hold up SD streaming)
// Vertical layout: 9 equal 26 px rows (234 px, 3 px padding above and below):
// the 8 tracks and the status row. Each row's band fills its slice.
// (26 px is the most that fits: 9 rows of 32 px would need 288 px.)
static constexpr int SLICES = NUM_TRACKS + 1, SLICE_H = 26, ROW_BAND = 26;
static constexpr int LAYOUT_TOP = (240 - SLICES * SLICE_H) / 2;
static constexpr int HEADER_SCALE = 1;                       // text size of the status row
static constexpr int TEXT_H = 8 * HEADER_SCALE;
static constexpr int sliceTop(int k) { return LAYOUT_TOP + k * SLICE_H; }
static constexpr int sliceCentredTop(int k, int h) { return sliceTop(k) + (SLICE_H - h) / 2; }
static constexpr int TEXT_Y = sliceCentredTop(NUM_TRACKS, TEXT_H);
// Each track row: three cells, then the button (icon only on the selected
// track), separated by 1 px gaps. The third cell holds the waveform; the
// first the filter (its response curve), the second volume and balance. On the selected row the
// current cell and the button get the selection highlight.
//   cell 1: x 0-78   cell 2: x 80-158   waveform cell: x 160-294   button: x 296-319
// The waveform fills its cell's whole width.
struct Cell { int x, w; };
static constexpr int CELLS = 3, FILTER_CELL = 0, MIX_CELL = 1, WAVE_CELL = 2;
static constexpr Cell CELL[CELLS] = {{0, 79}, {80, 79}, {160, 135}};
static constexpr int BUTTON_SIZE = ROW_BAND, BUTTON_X = 296, BUTTON_W = 320 - BUTTON_X;
static constexpr int WAVE_X = CELL[WAVE_CELL].x, WAVE_W = CELL[WAVE_CELL].w, WAVE_H = 16;   // waveform: x 160-294
// The status row below the tracks has the same cells; its third cell holds
// the tempo (left-aligned with the waveforms, adjustable) and the clock
// (right-aligned with them).
static constexpr int STATUS_ROW = NUM_TRACKS, ROWS = NUM_TRACKS + 1, TEMPO_CELL = 2;


// ---- Waveforms
// Each track's whole loop, drawn one column at a time: the row background
// (dim grey across the whole screen for the selected track, dark red while
// recording), the waveform (80% white inside the loop range, grey outside; muted
// clips dimmed by MUTED_WEIGHT) with faint darker bar lines at the clip's bar
// boundaries, and the playhead: a white line with a black one either side
// on unmuted clips; on muted ones the same, dimmed, only on the selected track. Full redraws only happen when
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
  uint16_t headColor = 0;                          // playhead: playCol in this colour, PLAYHEAD_EDGE either side
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

static int rowTop(int i) { return sliceCentredTop(i, ROW_BAND); }
// A row's backgrounds: its band, the first and last rows also out to the
// screen's top and bottom edges.
static int bgTop(int i) { return i == 0 ? 0 : rowTop(i); }
static int bgHeight(int i) { return (i == ROWS - 1 ? 240 : rowTop(i) + ROW_BAND) - bgTop(i); }
// The divider above the status row: the last track row's bottom line, drawn
// over its unhighlighted cells (a highlight covers it).
static constexpr int DIVIDER_ROW = NUM_TRACKS - 1;
static int dividerY() { return rowTop(DIVIDER_ROW) + ROW_BAND - 1; }

static bool recordingOn(int i) { return recording.track == i && recording.mode != Recording::Idle; }

// Background of a cell in track row i (cell -1: the button). The selected row
// highlights its current cell; its other cells and the button get a darker
// grey; a recording track's waveform cell is dark red.
static uint16_t cellBackground(int i, int cell) {
  if (cell == WAVE_CELL && recordingOn(i)) return BG_RECORDING;
  if (i != selected) return BG;
  return (cell != -1 && cell == selectedCell) ? BG_SELECTED : BG_ROW;
}
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
  if (v.visible && v.playCol >= 0 && abs(c - v.playCol) <= 1)   // on top, the row's full height
    board.tft().drawFastVLine(WAVE_X + c, rowTop(i), ROW_BAND, c == v.playCol ? v.headColor : PLAYHEAD_EDGE);
}

static void drawWave(int i, bool recordingHere) {
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
static void movePlayhead(int i, int col, uint16_t color) {
  WaveView &v = views[i];
  if (col == v.playCol && color == v.headColor) return;
  int old = v.playCol;
  v.playCol = col;
  v.headColor = color;
  // The columns it leaves (those it still covers draw as the new playhead), then the new ones.
  if (old >= 0) for (int c = old - 1; c <= old + 1; c++) if (col < 0 || abs(c - col) > 1) drawColumn(i, c);
  if (col >= 0) for (int c = col - 1; c <= col + 1; c++) drawColumn(i, c);
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
  if (bars != v.liveBars) {                        // new scale: redraw everything
    v.liveBars = bars;
    v.liveFilled = filled;
    memcpy(v.liveData, cols, OVERVIEW_COLS);
    v.playCol = head;
    v.headColor = REC_HEAD;
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
  movePlayhead(i, head, REC_HEAD);
}

// Playhead column for each track from the transport position and the
// track's range alignment.
static void updatePlayheads() {
  noInterrupts();
  bool running = transport.running;
  double pos = transport.beat + (double)transport.sampleInBeat / transport.spb;   // in beats
  interrupts();
  for (int i = 0; i < NUM_TRACKS; i++) {
    const Track &tr = tracks[i];
    WaveView &v = views[i];
    if (v.live) { updateLive(i); continue; }
    // Unmuted: white. Muted: none, but dimmed on the selected track.
    bool muted = tr.state == TrackState::Muted;
    int col = -1;
    if (v.visible && running && tr.live && (!muted || i == selected)) {
      double inRange = fmod(pos - tr.rangeAnchorQ / 4.0, (double)tr.rangeBeats);
      if (inRange < 0) inRange += tr.rangeBeats;
      double loopBeat = fmod(tr.rangeStartBeat + inRange, (double)(tr.loopBars * BEATS_PER_BAR));
      col = min(WAVE_W - 1, (int)(loopBeat * WAVE_W / (tr.loopBars * BEATS_PER_BAR)));
    }
    movePlayhead(i, col, muted ? MUTED_HEAD : PLAYHEAD);
  }
}

// ---- Row buttons
// Tracks with a clip show a toggle on every row: a filled play triangle while
// the clip plays, an outline when muted; white on the selected row, dark grey
// on the others. Unselected empty tracks show nothing; the selected one shows record (red), then play while recording
// (the press ends the recording and plays the clip). Both flash while waiting
// for the recording to start (next bar) or to finish (its full length).
// The status row shows the transport: play while running, stop while
// stopped; grey, white when the row is selected (A then starts from 1.1.1).
// (int, not an enum class: the Arduino IDE declares functions above any types.)
enum { ICON_NONE, ICON_RECORD, ICON_STOP, ICON_PLAY, ICON_TOGGLE_OFF, ICON_TOGGLE_ON };

// Flashing icons blink four times per beat, twice above 100 BPM.
static bool flashOn() {
  noInterrupts();
  bool running = transport.running;
  uint32_t sampleInBeat = transport.sampleInBeat, spb = transport.spb;
  interrupts();
  if (!running) return true;
  uint32_t phases = bpmForSpb(spb) > 100 ? 4 : 8;   // on/off halves per beat
  return ((uint64_t)sampleInBeat * phases / spb) % 2 == 0;
}

static int buttonIcon(int i) {
  if (i == STATUS_ROW) return transport.running ? ICON_PLAY : ICON_STOP;   // the transport's state
  if (i == selected) {
    if (recordingOn(i)) {
      Recording::Mode mode = recording.mode;
      if (mode == Recording::Running) return ICON_PLAY;
      if (!flashOn()) return ICON_NONE;
      return mode == Recording::WaitBar ? ICON_RECORD : ICON_PLAY;
    }
    if (tracks[i].state == TrackState::Empty) return ICON_RECORD;
  }
  if (tracks[i].state == TrackState::Empty) return ICON_NONE;
  return tracks[i].state == TrackState::Playing ? ICON_TOGGLE_ON : ICON_TOGGLE_OFF;
}

static uint16_t iconColor(int i, int icon) {
  switch (icon) {
    case ICON_RECORD:     return ICON_RECORD_COLOR;
    case ICON_TOGGLE_ON:
    case ICON_TOGGLE_OFF: return i == selected ? ICON_SELECTED : ICON_TOGGLE_IDLE;
    default:              return i == selected ? ICON_SELECTED : ICON_TRANSPORT_IDLE;   // play/stop
  }
}


// Button icons: filled shapes in an ICON_SIZE px box, anti-aliased (4x4
// samples per pixel, blended over the row background).
static constexpr float ICON_SIZE = 10.5f;
static constexpr int ICON_REACH = 7;               // drawn pixels: centre +- this

// Toggle: an 8 px tall play triangle, filled (on) or a 1 px outline (off).
static constexpr float TOGGLE_HALF = 4, TOGGLE_STROKE = 1;

// Distance from (dx, dy) inside a right-pointing play triangle of half-height
// h (negative outside).
static float playTriangle(float dx, float dy, float h) {
  const float ax = -0.75f * h, ay = -h, bx = -0.75f * h, by = h, cx = h, cy = 0;
  auto edge = [&](float px, float py, float qx, float qy) {
    float ex = qx - px, ey = qy - py;
    return ((dx - px) * ey - (dy - py) * ex) / sqrtf(ex * ex + ey * ey);
  };
  return min(min(edge(ax, ay, bx, by), edge(bx, by, cx, cy)), edge(cx, cy, ax, ay));
}

// Is the point (dx, dy) from the icon's centre inside the icon?
static bool onIcon(int icon, float dx, float dy) {
  float h = ICON_SIZE / 2;
  switch (icon) {
    case ICON_RECORD: return dx * dx + dy * dy <= 0.5625f * h * h;   // 75% of the box
    case ICON_STOP:   return max(fabsf(dx), fabsf(dy)) <= h;
    case ICON_PLAY:   return playTriangle(dx, dy, h) >= 0;
    case ICON_TOGGLE_OFF:
    case ICON_TOGGLE_ON: {
      float d = playTriangle(dx, dy, TOGGLE_HALF);
      return d >= 0 && (icon == ICON_TOGGLE_ON || d < TOGGLE_STROKE);
    }
    default: return false;
  }
}

static void drawIcon(int icon, float cx, float cy, uint16_t color, uint16_t bg) {
  auto &t = board.tft();
  const int r = ICON_REACH;
  for (int y = (int)cy - r; y <= (int)cy + r; y++) {
    for (int x = (int)cx - r; x <= (int)cx + r; x++) {
      int on = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
          if (onIcon(icon, x + (sx + 0.5f) / 4 - cx, y + (sy + 0.5f) / 4 - cy)) on++;
      if (on) t.drawPixel(x, y, blend565(color, bg, on * 255 / 16));
    }
  }
}

// The row's cells other than the waveform (which drawWave() handles): the two
// status row's cells, the gaps and the button. Each part is redrawn only when
// it changes: refilling a cell that didn't change would blank what's drawn on
// it (the status text) until its next redraw.
static void drawCells(int i) {
  struct Key { int icon; uint16_t bg0, bg1, bg2, bgButton, iconColor; };
  static Key shown[ROWS];
  static bool drawn[ROWS];
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.icon = buttonIcon(i);
  k.bg0 = cellBackground(i, 0);
  k.bg1 = cellBackground(i, 1);
  k.bg2 = i == STATUS_ROW ? cellBackground(i, TEMPO_CELL) : 0;   // tracks: drawWave()'s job
  k.bgButton = cellBackground(i, -1);
  k.iconColor = iconColor(i, k.icon);
  bool first = !drawn[i];
  if (!first && !memcmp(&k, &shown[i], sizeof k)) return;
  const Key old = shown[i];
  drawn[i] = true;
  shown[i] = k;
  auto &t = board.tft();
  int y = rowTop(i), y0 = bgTop(i), h = bgHeight(i);
  if (i == STATUS_ROW) {                             // tracks: drawFilter(), drawMix(), drawWave()
    if (first || k.bg0 != old.bg0) t.fillRect(CELL[0].x, y0, CELL[0].w, h, k.bg0);
    if (first || k.bg1 != old.bg1) t.fillRect(CELL[1].x, y0, CELL[1].w, h, k.bg1);
    if (first || k.bg2 != old.bg2) t.fillRect(CELL[2].x, y0, CELL[2].w, h, k.bg2);
  }
  if (first)
    for (int c = 0; c < CELLS; c++)                  // 1 px gaps after each cell
      t.drawFastVLine(CELL[c].x + CELL[c].w, y0, h, GAP);
  if (first || k.bgButton != old.bgButton || k.icon != old.icon || k.iconColor != old.iconColor) {
    // The button: its background, then the icon (a small area: no visible flash).
    t.fillRect(BUTTON_X, y0, BUTTON_W, h, k.bgButton);
    if (i == DIVIDER_ROW && k.bgButton == BG) t.drawFastHLine(BUTTON_X, dividerY(), BUTTON_W, DIVIDER);
    if (k.icon != ICON_NONE)
      drawIcon(k.icon, BUTTON_X + BUTTON_W / 2, y + ROW_BAND / 2, k.iconColor, k.bgButton);
  }
}

// ---- Filter cell: the response of the track's highpass + lowpass pair over
// 20 Hz - 20 kHz (log), 0 dB at the top to -CURVE_DB at the bottom; a line
// with a faint fill under it. Bright while the clip plays, dimmed when
// muted, nothing on an empty track. Each column is drawn whole (background,
// fill, line), so a change doesn't flash.
static constexpr int CURVE_H = 18;
static constexpr float CURVE_DB = 30, CURVE_LINE = 1.2f;   // line width in px (anti-aliased)

static void drawFilter(int i) {
  struct Key { int8_t lo, hi; uint16_t bg, line; };
  static Key shown[NUM_TRACKS];
  static bool drawn[NUM_TRACKS];
  const Track &tr = tracks[i];
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.lo = tr.filterLo;
  k.hi = tr.filterHi;
  k.bg = cellBackground(i, FILTER_CELL);
  k.line = tr.state == TrackState::Playing ? CURVE
         : tr.state == TrackState::Muted ? blend565(CURVE, BG, MUTED_WEIGHT) : BG;   // empty: no curve
  if (drawn[i] && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = true;
  shown[i] = k;
  auto &t = board.tft();
  const Cell &cell = CELL[FILTER_CELL];
  const int band = rowTop(i), top = (ROW_BAND - CURVE_H) / 2, bottom = top + CURVE_H - 1;   // rows in the band
  uint16_t fill = blend565(k.line, k.bg, CURVE_FILL_WEIGHT);
  // The curve through each column's centre, in band pixel coordinates (row
  // r spans r..r+1); below the graph it keeps going down, out of sight.
  static float curveY[CELL[FILTER_CELL].w + 2];      // columns -1 .. w
  for (int c = -1; c <= cell.w; c++) {
    float hz = FILTER_MIN_HZ * powf(2.0f, FILTER_OCTAVES * (c + 0.5f) / cell.w);
    curveY[c + 1] = top + 0.5f + -filterResponseDb(k.lo, k.hi, hz) / CURVE_DB * (CURVE_H - 1);
  }
  auto yAt = [&](float x) {                        // straight between column centres
    float f = constrain(x - 0.5f, -1.0f, (float)cell.w);
    int c = min((int)floorf(f), cell.w - 1);
    return curveY[c + 1] + (curveY[c + 2] - curveY[c + 1]) * (f - c);
  };
  // Distance from (x, y) to the curve's segment between columns c and c + 1.
  auto segDist = [&](int c, float x, float y) {
    float ax = c + 0.5f, ay = curveY[c + 1], dx = 1, dy = curveY[c + 2] - ay;
    float u = constrain(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    float ex = x - (ax + u * dx), ey = y - (ay + u * dy);
    return sqrtf(ex * ex + ey * ey);
  };
  static uint16_t col[ROW_BAND];
  for (int c = 0; c < cell.w; c++) {
    for (int r = 0; r < ROW_BAND; r++) col[r] = k.bg;
    if (i == DIVIDER_ROW && k.bg == BG) col[dividerY() - band] = DIVIDER;
    if (tr.state != TrackState::Empty) {
      for (int r = top - 1; r <= bottom; r++) {
        // 4x4 samples: on the line (within CURVE_LINE / 2), or under it (fill).
        int onLine = 0, under = 0;
        for (int sx = 0; sx < 4; sx++) {
          float x = c + (sx + 0.5f) / 4, cy = yAt(x);
          for (int sy = 0; sy < 4; sy++) {
            float y = r + (sy + 0.5f) / 4;
            if (min(segDist(c - 1, x, y), segDist(c, x, y)) <= CURVE_LINE / 2) onLine++;
            else if (y > cy && y < bottom + 1) under++;
          }
        }
        if (onLine || under) {
          uint16_t base = blend565(fill, k.bg, under * 255 / (16 - onLine ? 16 - onLine : 1));
          col[r] = blend565(k.line, base, onLine * 255 / 16);
        }
      }
    }
    if (bgTop(i) < band) t.drawFastVLine(cell.x + c, bgTop(i), band - bgTop(i), k.bg);   // the first row's margin
    t.writeRect(cell.x + c, band, 1, ROW_BAND, col);
  }
}

// ---- Volume / balance cell: concentric arcs around the cell's centre, (((
// for the left channel and ))) for the right, MIX_ARCS each. A channel's
// level (volume and balance) lights its arcs from the inside out, over
// MIX_FLOOR_DB..MIX_TOP_DB; the meter (the channel's actual output, with
// peak hold falling at METER_FALL_DB_PER_S) lights them white on the same
// scale, so a full-scale signal reaches the level's arc. Each arc's pixels
// (with anti-aliasing coverage) are worked out once; an arc is only redrawn
// when its colour changes.
static constexpr int MIX_ARCS = 6, MIX_ARC_PX = 128;
static constexpr float MIX_R0 = 8, MIX_DR = 5, MIX_STROKE = 1.25f, MIX_HALF_H = 10, MIX_SLOPE = 1.2f;
static constexpr float MIX_FLOOR_DB = -40, MIX_TOP_DB = 6, METER_FALL_DB_PER_S = 24;

struct ArcPx { uint8_t x, y, cov; };                 // in the cell; coverage 1..16
static ArcPx arcPx[2 * MIX_ARCS][MIX_ARC_PX];         // [side * MIX_ARCS + arc]
static int arcCount[2 * MIX_ARCS];

static void buildArcs() {
  const Cell &cell = CELL[MIX_CELL];
  const float cx = cell.w / 2.0f, cy = ROW_BAND / 2.0f;
  for (int y = 0; y < ROW_BAND; y++) {
    for (int x = 0; x < cell.w; x++) {
      int cov[2 * MIX_ARCS] = {};
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          float dx = x + (sx + 0.5f) / 4 - cx, dy = y + (sy + 0.5f) / 4 - cy;
          if (fabsf(dy) > MIX_HALF_H || fabsf(dy) > fabsf(dx) * MIX_SLOPE) continue;
          float d = sqrtf(dx * dx + dy * dy);
          int j = (int)lroundf((d - MIX_R0) / MIX_DR);
          if (j < 0 || j >= MIX_ARCS || fabsf(d - (MIX_R0 + j * MIX_DR)) > MIX_STROKE / 2) continue;
          cov[(dx < 0 ? 0 : MIX_ARCS) + j]++;
        }
      }
      for (int a = 0; a < 2 * MIX_ARCS; a++)
        if (cov[a] && arcCount[a] < MIX_ARC_PX) arcPx[a][arcCount[a]++] = {(uint8_t)x, (uint8_t)y, (uint8_t)cov[a]};
    }
  }
}

// How many arcs (0..MIX_ARCS, fractional) a level in dB lights.
static float arcsForDb(float db) {
  return constrain((db - MIX_FLOOR_DB) / (MIX_TOP_DB - MIX_FLOOR_DB), 0.0f, 1.0f) * MIX_ARCS;
}
static float gainDb(float g) { return g > 0 ? 20 * log10f(g) : -200.0f; }

// Arc j's colour for a channel lit to `level` arcs, with the meter at `meter` arcs.
// Partly lit arcs blend in quarter steps.
static uint16_t arcColor(int j, float level, float meter, uint16_t levelColor) {
  auto part = [](float f) { return (uint8_t)(lroundf(constrain(f, 0.0f, 1.0f) * 4) * 255 / 4); };
  uint16_t c = blend565(levelColor, MIX_ARC_OFF, part(level - j));
  return blend565(MIX_ARC_METER, c, part(meter - j));
}

static void drawMix(int i) {
  static bool built = false;
  if (!built) { buildArcs(); built = true; }
  static uint16_t shownBg[NUM_TRACKS], shown[NUM_TRACKS][2 * MIX_ARCS];
  static bool drawn[NUM_TRACKS];
  static float meterDb[NUM_TRACKS][2];
  static uint32_t lastMs[NUM_TRACKS];

  const Track &tr = tracks[i];
  // Meter: the newest peak, or the previous reading falling.
  float peak[2];
  takeTrackPeaks(i, peak[0], peak[1]);
  uint32_t now = millis();
  float fall = (now - lastMs[i]) * (METER_FALL_DB_PER_S / 1000);
  lastMs[i] = now;
  for (int ch = 0; ch < 2; ch++) meterDb[i][ch] = max(gainDb(peak[ch]), meterDb[i][ch] - fall);

  uint16_t bg = cellBackground(i, MIX_CELL);
  uint16_t colors[2 * MIX_ARCS];
  bool empty = tr.state == TrackState::Empty;
  uint16_t levelColor = tr.state == TrackState::Muted ? blend565(MIX_ARC_LEVEL, BG, MUTED_WEIGHT) : MIX_ARC_LEVEL;
  float gains[2] = {tr.gainL, tr.gainR};
  for (int ch = 0; ch < 2; ch++) {
    float level = arcsForDb(gainDb(gains[ch])), meter = arcsForDb(meterDb[i][ch]);
    for (int j = 0; j < MIX_ARCS; j++)
      colors[ch * MIX_ARCS + j] = empty ? bg : arcColor(j, level, meter, levelColor);
  }

  auto &t = board.tft();
  const Cell &cell = CELL[MIX_CELL];
  int top = rowTop(i);
  if (!drawn[i] || shownBg[i] != bg) {
    // Whole cell: built in a buffer and sent at once (no flash).
    static uint16_t buf[ROW_BAND * CELL[MIX_CELL].w];
    for (auto &p : buf) p = bg;
    if (i == DIVIDER_ROW && bg == BG)
      for (int x = 0; x < cell.w; x++) buf[(dividerY() - top) * cell.w + x] = DIVIDER;
    for (int a = 0; a < 2 * MIX_ARCS; a++)
      for (int k = 0; k < arcCount[a]; k++) {
        const ArcPx &p = arcPx[a][k];
        buf[p.y * cell.w + p.x] = blend565(colors[a], bg, p.cov * 255 / 16);
      }
    if (bgTop(i) < top) t.fillRect(cell.x, bgTop(i), cell.w, top - bgTop(i), bg);   // the first row's margin
    t.writeRect(cell.x, top, cell.w, ROW_BAND, buf);
    drawn[i] = true;
    shownBg[i] = bg;
    memcpy(shown[i], colors, sizeof colors);
    return;
  }
  for (int a = 0; a < 2 * MIX_ARCS; a++) {
    if (colors[a] == shown[i][a]) continue;
    shown[i][a] = colors[a];
    for (int k = 0; k < arcCount[a]; k++) {
      const ArcPx &p = arcPx[a][k];
      t.drawPixel(cell.x + p.x, top + p.y, blend565(colors[a], bg, p.cov * 255 / 16));
    }
  }
}

extern "C" const unsigned char glcdfont[];     // ILI9341_t3n's built-in 5x7 font

// Status row text, in its tempo cell: the tempo and the clock (Bar.Beat.Step)
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

static void drawStatus() {
  auto &t = board.tft();
  t.setTextSize(HEADER_SCALE);
  uint16_t bg = cellBackground(STATUS_ROW, TEMPO_CELL);
  char text[24];
  snprintf(text, sizeof text, "%.1f BPM", bpm);
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
}

static void drawTrack(int i) {
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

// ---- Keys
static constexpr uint32_t REPEAT_DELAY_MS = 200, REPEAT_PERIOD_MS = 100;   // held arrows
static constexpr uint32_t DOUBLE_TAP_MS = 200;      // B twice: reset the current cell

// The selected track's current cell back to its defaults: the whole loop as
// the range, the filter open, volume at unity and balance centred.
static void resetCell() {
  if (selected == STATUS_ROW || tracks[selected].state == TrackState::Empty) return;
  const Track &tr = tracks[selected];
  if (selectedCell == WAVE_CELL) {
    setRange(selected, 0, tr.loopBars * BEATS_PER_BAR);
  } else if (selectedCell == FILTER_CELL) {
    setTrackFilter(selected, 0, FILTER_MAX);
    trackChanged(selected);
  } else if (selectedCell == MIX_CELL) {
    setTrackMix(selected, VOLUME_UNITY, 0);
    trackChanged(selected);
  }
}

static void onKeys(uint8_t keys, uint8_t pressed) {
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  bool onStatus = selected == STATUS_ROW;
  int prevSel = selected;

  // OPT (B) tapped twice within DOUBLE_TAP_MS (with no arrows in between):
  // the current cell's parameters back to their defaults.
  static uint32_t lastOptTap = 0;
  static bool optTapArmed = false;
  const uint8_t arrows = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  if (pressed & arrows) optTapArmed = false;         // B was used as a modifier
  if (pressed & m8::BTN_OPT) {
    if (optTapArmed && millis() - lastOptTap < DOUBLE_TAP_MS) {
      resetCell();
      optTapArmed = false;
    } else {
      optTapArmed = true;
      lastOptTap = millis();
    }
  }

  if (opt) {
    // OPT (B) + arrows: the current cell's parameters (none on empty tracks).
    if (onStatus) {
      if (selectedCell == TEMPO_CELL) {              // tempo: 0.1 BPM steps, or 1 BPM
        if (pressed & m8::BTN_LEFT) setTempo(bpm - 0.1f);
        if (pressed & m8::BTN_RIGHT) setTempo(bpm + 0.1f);
        if (pressed & m8::BTN_UP) setTempo(bpm + 1);
        if (pressed & m8::BTN_DOWN) setTempo(bpm - 1);
      }
    } else if (selectedCell == WAVE_CELL) {
      // Loop range in beats, length a power of two. LEFT/RIGHT move the start
      // by one beat, wrapping around the loop (a range may run past the
      // loop's end); UP/DOWN double/halve the length.
      Track &tr = tracks[selected];
      uint32_t rs, rb, loopBeats = tr.loopBars * BEATS_PER_BAR;
      trackRange(selected, rs, rb);                  // includes a queued change
      if (pressed & m8::BTN_LEFT) setRange(selected, (rs + loopBeats - 1) % loopBeats, rb);
      if (pressed & m8::BTN_RIGHT) setRange(selected, (rs + 1) % loopBeats, rb);
      if (pressed & m8::BTN_UP && rb * 2 <= loopBeats) setRange(selected, rs, rb * 2);
      if (pressed & m8::BTN_DOWN && rb > 1) setRange(selected, rs, rb / 2);
    } else if (selectedCell == FILTER_CELL && tracks[selected].state != TrackState::Empty) {
      // Filter: LEFT/RIGHT move the passband a step; moving stops where both
      // points would leave the range. UP/DOWN widen/narrow it: a step on each
      // side around its centre, or, while one point sits at its edge (the
      // highpass at the bottom, the lowpass at the top; off), a step of the
      // other point only, from that edge. With both at their edges (flat),
      // around the centre again. Narrowing near an edge slides the band back
      // in (setTrackFilter).
      const Track &tr = tracks[selected];
      int lo = tr.filterLo, hi = tr.filterHi;
      if (pressed & m8::BTN_LEFT && hi > 0) { lo--; hi--; }
      if (pressed & m8::BTN_RIGHT && lo < FILTER_MAX) { lo++; hi++; }
      int grow = (pressed & m8::BTN_UP ? 1 : 0) - (pressed & m8::BTN_DOWN ? 1 : 0);
      bool loEdge = lo <= 0, hiEdge = hi >= FILTER_MAX;
      if (grow && loEdge != hiEdge) {
        if (loEdge) {                                  // from the bottom: the lowpass moves
          lo = 0;
          hi = constrain(hi + grow, lo, lo + FILTER_MAX);
        } else {                                       // from the top: the highpass moves
          hi = FILTER_MAX;
          lo = constrain(lo - grow, hi - FILTER_MAX, hi);
        }
      } else if (grow > 0 && hi - lo + 2 <= FILTER_MAX) {
        lo--; hi++;
      } else if (grow < 0 && hi - lo >= 2) {
        lo++; hi--;
      }
      if (lo != tr.filterLo || hi != tr.filterHi) {
        setTrackFilter(selected, lo, hi);
        trackChanged(selected);
      }
    } else if (selectedCell == MIX_CELL && tracks[selected].state != TrackState::Empty) {
      // Volume and balance: UP/DOWN louder/quieter, RIGHT/LEFT towards the
      // right/left channel.
      const Track &tr = tracks[selected];
      int volume = tr.volume, balance = tr.balance;
      if (pressed & m8::BTN_UP) volume++;
      if (pressed & m8::BTN_DOWN) volume--;
      if (pressed & m8::BTN_RIGHT) balance++;
      if (pressed & m8::BTN_LEFT) balance--;
      volume = constrain(volume, 0, VOLUME_MAX);
      balance = constrain(balance, -BALANCE_MAX, BALANCE_MAX);
      if (volume != tr.volume || balance != tr.balance) {
        setTrackMix(selected, volume, balance);
        trackChanged(selected);
      }
    }
  } else {
    if (pressed & m8::BTN_UP && selected > 0) selected--;              // no wrapping
    if (pressed & m8::BTN_DOWN && selected < ROWS - 1) selected++;
    if (pressed & m8::BTN_LEFT && selectedCell > 0) selectedCell--;
    if (pressed & m8::BTN_RIGHT && selectedCell < CELLS - 1) selectedCell++;
  }

  // START (or A on the status row): start from 1.1.1 / stop. With SHIFT:
  // start from 1.1.1 on every press, running or not.
  bool transportKey = (pressed & m8::BTN_PLAY) || (pressed & m8::BTN_EDIT && onStatus);
  bool toggleTransport = transportKey && !shift;
  if (transportKey && shift) transportRestart();
  if (pressed & m8::BTN_EDIT && !onStatus) {
    Track &tr = tracks[selected];
    if (shift) {
      clearTrack(selected);
    } else if (tr.state == TrackState::Empty || recordingOn(selected)) {
      recordPress(selected);
    } else {
      tr.state = tr.state == TrackState::Playing ? TrackState::Muted : TrackState::Playing;
      trackChanged(selected);
    }
  }
  if (toggleTransport) {
    if (transport.running) transportStop();
    else transportStart();
  }
  // Show the result right away rather than on the rows' next redraw: the new
  // selection first, then the old one, so highlight and icon move together.
  drawTrack(selected);
  if (prevSel != selected) drawTrack(prevSel);
}

// After a recording finishes on the selected track, move on: to the next
// empty track (in order, wrapping), else the next muted one, else stay.
static void selectAfterRecording(int t) {
  if (selected != t) return;                       // the user has moved on already
  for (TrackState want : {TrackState::Empty, TrackState::Muted}) {
    for (int k = 1; k < NUM_TRACKS; k++) {
      int i = (t + k) % NUM_TRACKS;
      if (tracks[i].state == want) {
        selected = i;
        drawTrack(i);
        drawTrack(t);
        return;
      }
    }
  }
}

// ---- Power
static void handlePowerButton() {
  if (!board.powerButtonPressed()) return;
  uint32_t start = millis();
  while (board.powerButtonHeld()) {
    if (millis() - start >= 1000) {
      codec.volume(0.0f);
      flushHeaders();
      if (tempoDirty) saveTempo(bpm);
      board.powerOff();
    }
  }
  board.powerButtonPressed();
}

static void fatal(const char *msg) {
  auto &t = board.tft();
  t.setTextColor(TEXT_ERROR, BG);
  t.setTextSize(1);
  t.setCursor(4, 100);
  t.print(msg);
  for (;;) handlePowerButton();
}

void setup() {
  Serial.begin(115200);
  if (CrashReport) Serial.print(CrashReport);
  AudioMemory(12);
  board.begin();
  board.powerButtonPressed();                        // the press that switched us on
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(5);
  codec.volume(0.6f);

  board.tft().fillScreen(BG);
  board.tft().setTextWrap(false);
  auto progress = [](const char *msg) {
    auto &t = board.tft();
    t.setTextColor(TEXT_PROGRESS, BG);
    t.setTextSize(1);
    t.setCursor(4, 100);
    t.printf("%-50s", msg);
    Serial.println(msg);
  };
  if (!storageBegin(progress))
    fatal("SD card missing or full: the looper needs ~180 MB in /looper.");
  progress("");
  bpm = roundf(loadTempo() * 10) / 10;
  transport.spb = spbForBpm(bpm);
  if (MIDI_OUT_ENABLED) midiBegin(board.midiOutBegin());
  freezeTimer.priority(255);
  freezeTimer.begin(freezeCheck, 100000);
}

void loop() {
  loopCount++;
  debugStage = "power button";
  handlePowerButton();
  debugStage = "storage";
  serviceStorage();

  // Speaker off while headphones are plugged in.
  bool phones;
  if (board.headphonesChanged(phones)) {
    board.setSpeaker(!phones);
    Serial.printf("headphones %s: speaker %s\n", phones ? "in" : "out", phones ? "off" : "on");
  }

  debugStage = "keys (I2C)";
  // Arrows repeat while held: the press acts at once, then again after
  // REPEAT_DELAY_MS and every REPEAT_PERIOD_MS (the most recently pressed one).
  uint8_t keys;
  static uint8_t prev = 0, repeatKey = 0;
  static uint32_t repeatAt = 0;
  const uint8_t ARROWS = m8::BTN_LEFT | m8::BTN_RIGHT | m8::BTN_UP | m8::BTN_DOWN;
  if (board.keysChanged(keys)) {
    debugStage = "key action";
    uint8_t pressed = keys & ~prev;
    if (pressed & ARROWS) {
      repeatKey = pressed & ARROWS & -(pressed & ARROWS);   // one of them, if several
      repeatAt = millis() + REPEAT_DELAY_MS;
    } else if (!(keys & repeatKey)) {
      repeatKey = 0;                                 // released
    }
    onKeys(keys, pressed);
    prev = keys;
  } else if (repeatKey && (int32_t)(millis() - repeatAt) >= 0) {
    debugStage = "key repeat";
    repeatAt = millis() + REPEAT_PERIOD_MS;          // after a slow pass: no burst of catch-up repeats
    onKeys(prev, repeatKey);
  }

  // Saving creates/rewrites a file, which can stall the card: only while stopped.
  if (tempoDirty && !transport.running && millis() - tempoChangedMs > 2000) {
    saveTempo(bpm);
    tempoDirty = false;
  }

  // Report dropouts with what the storage code was doing (debugging).
  static uint32_t lastDropCheck = 0, lastDrops[NUM_TRACKS] = {};
  if (millis() - lastDropCheck >= 250) {
    lastDropCheck = millis();
    for (int t = 0; t < NUM_TRACKS; t++) {
      uint32_t d = tracks[t].underruns;
      if (d != lastDrops[t]) {
        Serial.printf("dropout: track %d +%lu samples (loop in: %s, recording: %d)\n", t + 1,
                      d - lastDrops[t], (const char *)debugStage, (int)recording.mode);
        lastDrops[t] = d;
      }
    }
  }

  // A recording that ended with a clip (not cancelled): select the next track.
  static int recTrack = -1;
  if (recording.mode != Recording::Idle) {
    recTrack = recording.track;
  } else if (recTrack >= 0) {
    if (tracks[recTrack].state != TrackState::Empty) selectAfterRecording(recTrack);
    recTrack = -1;
  }

  // Redraw a little at a time so SD streaming keeps going.
  static uint32_t lastDraw = 0;
  static int nextRow = 0;
  if (millis() - lastDraw >= 30) {
    lastDraw = millis();
    debugStage = "screen";
    if (selected != STATUS_ROW) drawCells(selected);   // every pass: its icon may flash
    drawCells(STATUS_ROW);                           // the transport may have started/stopped
    drawStatus();                                    // every pass: the clock shows steps
    updatePlayheads();
    for (int i = 0; i < NUM_TRACKS; i++) drawMix(i);   // the meters
    drawTrack(nextRow);
    if (++nextRow >= ROWS) nextRow = 0;
  }
}
