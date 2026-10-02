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
//   OPT + arrows     change the current cell:
//                      waveform: LEFT/RIGHT move the loop range by a beat,
//                                UP/DOWN double/halve it (1/4 bar to the whole loop)
//                      filter: LEFT/RIGHT move the band, UP/DOWN widen/narrow it
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
// first the filter (its response curve); the second is a placeholder. On the selected row the
// current cell and the button get the selection highlight.
//   cell 1: x 0-78   cell 2: x 80-158   waveform cell: x 160-294   button: x 296-319
// The waveform fills its cell's whole width.
struct Cell { int x, w; };
static constexpr int CELLS = 3, FILTER_CELL = 0, WAVE_CELL = 2;
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
// boundaries, and the playhead: 2 px grey on unmuted clips, a thin black line
// on muted ones, only on the selected track. Full redraws only happen when
// something changes; the playhead moves by redrawing just its columns.
// While a track records it shows the recording growing (live view) with a
// red record head, over the length it would become.
// Outside the loop range: the same dark grey for playing and muted clips.
// (Colours: colors.h.)
static constexpr int HEAD_W = 2;                     // playheads: from the playing column, 2 px to the right

struct WaveView {
  bool visible = false;                            // something is shown (not an empty track)
  bool live = false;                               // recording in progress: live view
  uint16_t bg = BG;
  int rangeFirst = 0, rangeLast = -1;              // columns inside the loop range
  int playCol = -1;                                // playhead / record head; -1: none
  int headW = HEAD_W;                              // playhead covers playCol .. playCol + headW - 1
  uint16_t headColor = 0;
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

static void drawColumn(int i, int c) {
  if (c < 0 || c >= WAVE_W) return;
  auto &t = board.tft();
  const Track &tr = tracks[i];
  const WaveView &v = views[i];
  int x = WAVE_X + c, top = waveTop(i), mid = top + WAVE_H / 2;
  t.drawFastVLine(x, bgTop(i), bgHeight(i), v.bg);   // background: the row's full height
  if (i == DIVIDER_ROW && v.bg == BG) t.drawPixel(x, dividerY(), DIVIDER);
  if (!v.visible) return;
  if (v.playCol >= 0 && c >= v.playCol && c < v.playCol + v.headW) {
    t.drawFastVLine(x, rowTop(i), ROW_BAND, v.headColor);   // the row's full height
    return;
  }
  if (v.live && c >= v.liveFilled) return;
  bool bar = isBarColumn(v.live ? v.liveBars : tr.loopBars, c);
  bool in = v.live || (c >= v.rangeFirst && c <= v.rangeLast);
  uint16_t color = in ? (bar ? WAVE_IN_BAR : WAVE_IN) : (bar ? WAVE_OUT_BAR : WAVE_OUT);
  if (!v.live && in && tr.state == TrackState::Muted)
    color = blend565(color, BG, MUTED_WEIGHT);   // muted: the loop range dimmed
  int h = columnLevel(v.live ? v.liveData : tr.overview, c) * (WAVE_H / 2) / 255;
  t.drawFastVLine(x, mid - h, 2 * h + 1, color);
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
  v.rangeLast = -1;
  if (v.live) {
    v.liveBars = 0;                                // updatePlayheads() fills it in
    v.liveFilled = 0;
  } else if (v.visible) {
    uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
    v.rangeFirst = rs * WAVE_W / loopBeats;
    v.rangeLast = max(v.rangeFirst, (int)(((rs + rb) * WAVE_W + loopBeats - 1) / loopBeats) - 1);
  }
  for (int c = 0; c < WAVE_W; c++) drawColumn(i, c);
}

// Moves a view's playhead (or record head) to col with the given look,
// redrawing only the columns it leaves and covers.
static void movePlayhead(int i, int col, int width, uint16_t color) {
  WaveView &v = views[i];
  if (col == v.playCol && width == v.headW && color == v.headColor) return;
  int old = v.playCol, oldW = v.headW;
  v.playCol = col;
  v.headW = width;
  v.headColor = color;
  if (old >= 0) for (int c = old; c < old + oldW; c++) drawColumn(i, c);
  if (col >= 0) for (int c = col; c < col + width; c++) drawColumn(i, c);
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
    v.headW = HEAD_W;
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
  movePlayhead(i, head, HEAD_W, REC_HEAD);
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
    // Unmuted: 2 px grey. Muted: none, but a thin black line on the
    // selected track.
    bool muted = tr.state == TrackState::Muted;
    int col = -1;
    if (v.visible && running && tr.live && (!muted || i == selected)) {
      double inRange = fmod(pos - tr.rangeAnchorQ / 4.0, (double)tr.rangeBeats);
      if (inRange < 0) inRange += tr.rangeBeats;
      double loopBeat = tr.rangeStartBeat + inRange;
      col = min(WAVE_W - 1, (int)(loopBeat * WAVE_W / (tr.loopBars * BEATS_PER_BAR)));
    }
    movePlayhead(i, col, muted ? 1 : HEAD_W, muted ? MUTED_HEAD : PLAYHEAD);
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
// placeholder cells, the gaps and the button. Redrawn only when they change.
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
  if (drawn[i] && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = true;
  shown[i] = k;
  auto &t = board.tft();
  int y = rowTop(i), y0 = bgTop(i), h = bgHeight(i);
  if (i == STATUS_ROW) t.fillRect(CELL[0].x, y0, CELL[0].w, h, k.bg0);   // tracks: drawFilter()'s job
  t.fillRect(CELL[1].x, y0, CELL[1].w, h, k.bg1);
  if (i == STATUS_ROW) t.fillRect(CELL[2].x, y0, CELL[2].w, h, k.bg2);
  for (int c = 0; c < CELLS; c++)                    // 1 px gaps after each cell
    t.drawFastVLine(CELL[c].x + CELL[c].w, y0, h, GAP);
  t.fillRect(BUTTON_X, y0, BUTTON_W, h, k.bgButton);
  if (i == DIVIDER_ROW) {                            // broken at the gaps (the waveform: drawColumn)
    if (k.bg1 == BG) t.drawFastHLine(CELL[1].x, dividerY(), CELL[1].w, DIVIDER);
    if (k.bgButton == BG) t.drawFastHLine(BUTTON_X, dividerY(), BUTTON_W, DIVIDER);
  }
  if (k.icon != ICON_NONE)
    drawIcon(k.icon, BUTTON_X + BUTTON_W / 2, y + ROW_BAND / 2, k.iconColor, k.bgButton);
}

// ---- Filter cell: the response of the track's highpass + lowpass pair over
// 20 Hz - 20 kHz (log), 0 dB at the top to -CURVE_DB at the bottom; a line
// with a faint fill under it. Bright while the clip plays, dimmed when
// muted, nothing on an empty track. Each column is drawn whole (background,
// fill, line), so a change doesn't flash.
static constexpr int CURVE_H = 18;
static constexpr float CURVE_DB = 30;

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
  int top = rowTop(i) + (ROW_BAND - CURVE_H) / 2, bottom = top + CURVE_H - 1;
  uint16_t fill = blend565(k.line, k.bg, CURVE_FILL_WEIGHT);
  int prevY = -1;
  for (int c = 0; c < cell.w; c++) {
    int x = cell.x + c;
    float hz = FILTER_MIN_HZ * powf(2.0f, FILTER_OCTAVES * (c + 0.5f) / cell.w);
    float db = filterResponseDb(k.lo, k.hi, hz);
    int y = top + (int)lroundf(min(-db, CURVE_DB) / CURVE_DB * (CURVE_H - 1));
    if (prevY < 0) prevY = y;
    t.drawFastVLine(x, bgTop(i), bgHeight(i), k.bg);
    if (i == DIVIDER_ROW && k.bg == BG) t.drawPixel(x, dividerY(), DIVIDER);
    if (tr.state == TrackState::Empty) continue;
    if (y < bottom) t.drawFastVLine(x, y + 1, bottom - y, fill);
    int a = min(prevY, y), b = max(prevY, y);      // joins steep slopes
    if (y < bottom || prevY < bottom) t.drawFastVLine(x, a, min(b, bottom) - a + 1, k.line);
    prevY = y;
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
  drawWave(i, recordingOn(i));
}

// ---- Keys
static void onKeys(uint8_t keys, uint8_t pressed) {
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  bool onStatus = selected == STATUS_ROW;
  int prevSel = selected;

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
      // by one beat; UP/DOWN double/halve the length (doubling keeps the
      // range inside the loop).
      Track &tr = tracks[selected];
      uint32_t rs, rb, loopBeats = tr.loopBars * BEATS_PER_BAR;
      trackRange(selected, rs, rb);                  // includes a queued change
      if (pressed & m8::BTN_LEFT && rs >= 1) setRange(selected, rs - 1, rb);
      if (pressed & m8::BTN_RIGHT && rs + 1 + rb <= loopBeats) setRange(selected, rs + 1, rb);
      if (pressed & m8::BTN_UP && rb * 2 <= loopBeats) setRange(selected, min(rs, loopBeats - rb * 2), rb * 2);
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
    }
    // the second cell: no parameters yet
  } else {
    if (pressed & m8::BTN_UP) selected = (selected + ROWS - 1) % ROWS;
    if (pressed & m8::BTN_DOWN) selected = (selected + 1) % ROWS;
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

  debugStage = "keys (I2C)";
  uint8_t keys;
  static uint8_t prev = 0;
  if (board.keysChanged(keys)) {
    debugStage = "key action";
    onKeys(keys, keys & ~prev);
    prev = keys;
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
    drawStatus();                                    // every pass: the clock shows steps
    if (selected != STATUS_ROW) drawCells(selected);   // every pass: its icon may flash
    drawCells(STATUS_ROW);                           // the transport may have started/stopped
    updatePlayheads();
    drawTrack(nextRow);
    if (++nextRow >= ROWS) nextRow = 0;
  }
}
