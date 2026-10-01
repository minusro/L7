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
//                      tempo: LEFT/RIGHT -/+ 0.1 BPM, UP/DOWN +/- 1 BPM
//   EDIT             empty track: record / stop recording; full track: mute/unmute
//   SHIFT+EDIT       clear the track
//   PLAY             start/stop (also EDIT on the status row)
//   power (hold)     switch off
//
// Build: Teensyduino, board "Teensy 4.0", USB type "Serial", CPU speed 600 MHz.
// Libraries: ILI9341_t3n (KurtE), Audio, SD (bundled with Teensyduino).

#include <Audio.h>
#include <SD.h>
#include "M8Board.h"
#include "engine.h"
#include "storage.h"

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
// first two are placeholders for future parameters. On the selected row the
// current cell and the button get the selection highlight.
//   cell 1: x 0-78   cell 2: x 80-158   waveform cell: x 160-294   button: x 296-319
struct Cell { int x, w; };
static constexpr int CELLS = 3, WAVE_CELL = 2;
static constexpr Cell CELL[CELLS] = {{0, 79}, {80, 79}, {160, 135}};
static constexpr int BUTTON_SIZE = ROW_BAND, BUTTON_X = 296, BUTTON_W = 320 - BUTTON_X;
static constexpr int WAVE_X = CELL[WAVE_CELL].x + 2, WAVE_W = 130, WAVE_H = 16;   // waveform: x 162-291
// The status row below the tracks has the same cells; its third cell holds
// the tempo (left-aligned with the waveforms, adjustable) and the clock
// (right-aligned with them).
static constexpr int STATUS_ROW = NUM_TRACKS, ROWS = NUM_TRACKS + 1, TEMPO_CELL = 2;

// Blends two RGB565 colours: a at weight w (0-255), b at 255 - w.
static uint16_t blend565(uint16_t a, uint16_t b, uint8_t w) {
  int r = (((a >> 11) & 31) * w + ((b >> 11) & 31) * (255 - w)) / 255;
  int g = (((a >> 5) & 63) * w + ((b >> 5) & 63) * (255 - w)) / 255;
  int bl = ((a & 31) * w + (b & 31) * (255 - w)) / 255;
  return (r << 11) | (g << 5) | bl;
}


// ---- Waveforms
// Each track's whole loop, drawn one column at a time: the row background
// (dim grey across the whole screen for the selected track, dark red while
// recording), the waveform (white inside the loop range, grey outside; muted
// clips at half brightness) with faint darker bar lines at the clip's bar
// boundaries, and the playhead: 3 px in the accent colour on unmuted clips,
// none on muted ones except a thin black line on the selected track. Full redraws only happen when
// something changes; the playhead moves by redrawing just its columns.
// While a track records it shows the recording growing (live view) with a
// red record head, over the length it would become.
// Outside the loop range: the same dark grey for playing and muted clips.
static constexpr uint16_t WAVE_IN = ILI9341_WHITE, WAVE_OUT = 0x2104;        // white, ~33/33/33
static constexpr uint16_t WAVE_IN_BAR = 0xBDF7, WAVE_OUT_BAR = 0x18C3;       // the same at ~75%
static constexpr uint16_t BG_SELECTED = 0x2945, BG_RECORDING = 0x3800;       // dim grey, dark red
static constexpr uint16_t PLAYHEAD = 0x6B4D, REC_HEAD = ILI9341_RED;   // grey ~107, red
static constexpr int HEAD_W = 2;                     // playheads: from the playing column, 2 px to the right
static constexpr uint16_t MUTED_HEAD = ILI9341_BLACK;
static constexpr uint16_t BUTTON_ICON = ILI9341_WHITE;

struct WaveView {
  bool visible = false;                            // something is shown (not an empty track)
  bool live = false;                               // recording in progress: live view
  uint16_t bg = ILI9341_BLACK;
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

static bool recordingOn(int i) { return recording.track == i && recording.mode != Recording::Idle; }

// Background of a cell in track row i (cell -1: the button). The selected row
// highlights its current cell and the button; a recording track's waveform
// cell is dark red.
static uint16_t cellBackground(int i, int cell) {
  if (cell == WAVE_CELL && recordingOn(i)) return BG_RECORDING;
  if (i != selected) return ILI9341_BLACK;
  return (cell == -1 || cell == selectedCell) ? BG_SELECTED : ILI9341_BLACK;
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
  t.drawFastVLine(x, rowTop(i), BUTTON_SIZE, v.bg);   // background: the row's full height
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
    color = blend565(color, ILI9341_BLACK, 128);   // muted: the loop range at half brightness
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
  // The waveform cell's margins around the waveform, then the waveform.
  auto &t = board.tft();
  const Cell &cell = CELL[WAVE_CELL];
  t.fillRect(cell.x, rowTop(i), WAVE_X - cell.x, ROW_BAND, bg);
  t.fillRect(WAVE_X + WAVE_W, rowTop(i), cell.x + cell.w - WAVE_X - WAVE_W, ROW_BAND, bg);
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

// ---- Track button: only on the selected track, grey, on the row background.
// No clip: record. Unmuted clip: stop. Muted clip: play.
// (int, not an enum class: the Arduino IDE declares functions above any types.)
enum { ICON_NONE, ICON_RECORD, ICON_STOP, ICON_PLAY };

static int buttonIcon(int i) {
  if (i != selected) return ICON_NONE;
  if (i == STATUS_ROW) return transport.running ? ICON_STOP : ICON_PLAY;   // the transport
  switch (tracks[i].state) {
    case TrackState::Empty:   return ICON_RECORD;
    case TrackState::Playing: return ICON_STOP;
    default:                  return ICON_PLAY;
  }
}


// Button icons: filled shapes in an ICON_SIZE px box, anti-aliased (4x4
// samples per pixel, blended over the row background).
static constexpr float ICON_SIZE = 10.5f;

// Is the point (dx, dy) from the icon's centre inside the icon?
static bool onIcon(int icon, float dx, float dy) {
  float h = ICON_SIZE / 2;
  switch (icon) {
    case ICON_RECORD: return dx * dx + dy * dy <= h * h;
    case ICON_STOP:   return max(fabsf(dx), fabsf(dy)) <= h;
    case ICON_PLAY: {
      // Triangle pointing right: inside where it's on the inner side of all three edges.
      const float ax = -0.8f * h, ay = -h, bx = -0.8f * h, by = h, cx = h, cy = 0;
      auto edge = [&](float px, float py, float qx, float qy) {
        float ex = qx - px, ey = qy - py;
        return ((dx - px) * ey - (dy - py) * ex) / sqrtf(ex * ex + ey * ey);
      };
      float d = min(min(edge(ax, ay, bx, by), edge(bx, by, cx, cy)), edge(cx, cy, ax, ay));
      return d >= 0;
    }
    default: return false;
  }
}

static void drawIcon(int icon, float cx, float cy, uint16_t color, uint16_t bg) {
  auto &t = board.tft();
  int r = (int)(ICON_SIZE / 2) + 1;
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
  struct Key { int icon; uint16_t bg0, bg1, bg2, bgButton; };
  static Key shown[ROWS];
  static bool drawn[ROWS];
  Key k;
  memset(&k, 0, sizeof k);                         // padding too: compared with memcmp
  k.icon = buttonIcon(i);
  k.bg0 = cellBackground(i, 0);
  k.bg1 = cellBackground(i, 1);
  k.bg2 = i == STATUS_ROW ? cellBackground(i, TEMPO_CELL) : 0;   // tracks: drawWave()'s job
  k.bgButton = cellBackground(i, -1);
  if (drawn[i] && !memcmp(&k, &shown[i], sizeof k)) return;
  drawn[i] = true;
  shown[i] = k;
  auto &t = board.tft();
  int y = rowTop(i);
  t.fillRect(CELL[0].x, y, CELL[0].w, ROW_BAND, k.bg0);
  t.fillRect(CELL[1].x, y, CELL[1].w, ROW_BAND, k.bg1);
  if (i == STATUS_ROW) t.fillRect(CELL[2].x, y, CELL[2].w, ROW_BAND, k.bg2);
  for (int c = 0; c < CELLS; c++)                    // 1 px gaps after each cell
    t.drawFastVLine(CELL[c].x + CELL[c].w, y, ROW_BAND, ILI9341_BLACK);
  t.fillRect(BUTTON_X, y, BUTTON_W, ROW_BAND, k.bgButton);
  if (k.icon != ICON_NONE)
    drawIcon(k.icon, BUTTON_X + BUTTON_W / 2, y + ROW_BAND / 2, BUTTON_ICON, k.bgButton);
}

extern "C" const unsigned char glcdfont[];     // ILI9341_t3n's built-in 5x7 font

// Status row text, in its tempo cell: the tempo and the clock (Bar.Beat.Step)
// STATUS_INSET px inside the waveforms' left and right edges; white,
// the clock grey 1.1.1 while stopped. The font's glyphs carry empty columns
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

static constexpr int STATUS_INSET = 5;             // tempo/clock this far inside the waveforms' edges

static void drawStatus() {
  auto &t = board.tft();
  t.setTextSize(HEADER_SCALE);
  uint16_t bg = cellBackground(STATUS_ROW, TEMPO_CELL);
  char text[24];
  snprintf(text, sizeof text, "%.1f BPM", bpm);
  printFromLeft(WAVE_X + STATUS_INSET, CELL[TEMPO_CELL].x, text, 10, ILI9341_WHITE, bg);
  noInterrupts();
  int32_t beat = transport.beat;
  uint32_t step = (uint64_t)transport.sampleInBeat * 4 / transport.spb;   // quarter beats
  bool running = transport.running;
  interrupts();
  if (running)
    snprintf(text, sizeof text, "%ld.%ld.%lu", beat / BEATS_PER_BAR + 1, beat % BEATS_PER_BAR + 1, step + 1);
  else
    snprintf(text, sizeof text, "1.1.1");          // where it starts from
  printToRight(WAVE_X + WAVE_W - 1 - STATUS_INSET, text, 10, running ? ILI9341_WHITE : ILI9341_DARKGREY, bg);
}

static void drawTrack(int i) {
  if (i < 0 || i >= ROWS) return;
  drawCells(i);
  if (i == STATUS_ROW) drawStatus();
  else drawWave(i, recordingOn(i));
}

// ---- Keys
static void onKeys(uint8_t keys, uint8_t pressed) {
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  bool onStatus = selected == STATUS_ROW;
  int prevSel = selected;

  if (opt) {
    // OPT (B) + arrows: the current cell's parameters.
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
    }
    // cells 1 and 2: no parameters yet
  } else {
    if (pressed & m8::BTN_UP) selected = (selected + ROWS - 1) % ROWS;
    if (pressed & m8::BTN_DOWN) selected = (selected + 1) % ROWS;
    if (pressed & m8::BTN_LEFT && selectedCell > 0) selectedCell--;
    if (pressed & m8::BTN_RIGHT && selectedCell < CELLS - 1) selectedCell++;
  }

  bool toggleTransport = pressed & m8::BTN_PLAY;
  if (pressed & m8::BTN_EDIT && onStatus) toggleTransport = !toggleTransport;   // A on the status row
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
  t.setTextColor(ILI9341_RED, ILI9341_BLACK);
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

  board.tft().fillScreen(ILI9341_BLACK);
  board.tft().setTextWrap(false);
  auto progress = [](const char *msg) {
    auto &t = board.tft();
    t.setTextColor(ILI9341_YELLOW, ILI9341_BLACK);
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

  // Redraw a little at a time so SD streaming keeps going.
  static uint32_t lastDraw = 0;
  static int nextRow = 0;
  if (millis() - lastDraw >= 30) {
    lastDraw = millis();
    debugStage = "screen";
    drawStatus();                                    // every pass: the clock shows steps
    updatePlayheads();
    drawTrack(nextRow);
    if (++nextRow >= ROWS) nextRow = 0;
  }
}
