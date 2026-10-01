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
//   UP/DOWN          select track          PLAY         start/stop
//   LEFT/RIGHT       tempo -/+ 1 BPM       SHIFT+L/R    tempo -/+ 0.1 BPM
//   EDIT             empty track: record / stop recording; full track: mute
//   SHIFT+EDIT       clear the track
//   OPT+LEFT/RIGHT   move the loop range by one beat
//   OPT+UP/DOWN      double/halve the loop range (1/4 bar up to the whole loop)
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
static int selected = 0;
static uint32_t tempoChangedMs = 0;
static bool tempoDirty = false;

// ---- Tempo: every loop must stay within 50%-200% of its recorded speed
static bool setTempo(float b) {
  b = constrain(b, MIN_BPM, MAX_BPM);
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

// ---- Screen (text is drawn over its own background: no full clears, which
// would hold up SD streaming)
// Vertical layout: tempo line at TOP_Y, then 8 track rows. TOP_Y centres the
// whole block: 20 px above the tempo text and below track 8's selection bar.
static constexpr int ROW_H = 22;
static constexpr int TOP_Y = 20;
static constexpr int ROW_Y = TOP_Y + 26;
// Row text runs from x = 50 to 200 (25 columns); the waveform sits right of it.
static constexpr int TEXT_COLS = 25;
static constexpr int WAVE_X = 204, WAVE_H = 16;

// ---- Waveforms
// Each track's whole loop, drawn one column at a time, layers bottom to top:
// the waveform (white inside the loop range, grey outside), darker bar lines
// at the clip's bar boundaries, yellow [ ] brackets around the loop range, and
// a green playhead. Full redraws only happen when something changes; the
// playhead moves by redrawing just the two columns involved.
static constexpr uint16_t WAVE_IN = ILI9341_WHITE, WAVE_OUT = 0x7BEF;        // white, grey
static constexpr uint16_t WAVE_IN_BAR = 0x8410, WAVE_OUT_BAR = 0x39E7;       // the same, half as bright
static constexpr uint16_t BRACKET = ILI9341_YELLOW, PLAYHEAD = ILI9341_GREEN;

struct WaveView {
  bool visible = false;                            // a loop is shown (not empty / recording)
  int rangeFirst = 0, rangeLast = -1;              // columns inside the loop range
  int playCol = -1;                                // -1: no playhead
  // what the last full draw showed
  uint8_t version = 0, state = 0;
  bool rec = false, drawn = false;
  uint32_t rs = 0, rb = 0;
};
static WaveView views[NUM_TRACKS];

static int waveTop(int i) { return ROW_Y + i * ROW_H + (ROW_H - 2 - WAVE_H) / 2; }

static bool isBarColumn(const Track &tr, int c) {
  // A column holds a bar line if a bar boundary (other than the loop's start)
  // falls inside it.
  uint32_t bars = tr.loopBars;
  uint32_t b0 = ((uint64_t)c * bars + OVERVIEW_COLS - 1) / OVERVIEW_COLS;     // first boundary >= column start
  return b0 > 0 && b0 < bars && (uint64_t)b0 * OVERVIEW_COLS < (uint64_t)(c + 1) * bars;
}

static void drawColumn(int i, int c) {
  auto &t = board.tft();
  const Track &tr = tracks[i];
  const WaveView &v = views[i];
  int x = WAVE_X + c, top = waveTop(i), mid = top + WAVE_H / 2, bottom = top + WAVE_H;
  t.drawFastVLine(x, top, WAVE_H + 1, ILI9341_BLACK);
  if (!v.visible) return;
  bool in = c >= v.rangeFirst && c <= v.rangeLast;
  bool bar = isBarColumn(tr, c);
  uint16_t color = in ? (bar ? WAVE_IN_BAR : WAVE_IN) : (bar ? WAVE_OUT_BAR : WAVE_OUT);
  int h = tr.overview[c] * (WAVE_H / 2) / 255;
  t.drawFastVLine(x, mid - h, 2 * h + 1, color);   // (a see-through black bar line only darkens the waveform)
  // Brackets: [ at the first column of the range, ] at the last.
  if (c == v.rangeFirst || c == v.rangeLast) t.drawFastVLine(x, top, WAVE_H + 1, BRACKET);
  if (c == v.rangeFirst + 1 || c == v.rangeLast - 1 || c == v.rangeFirst || c == v.rangeLast) {
    t.drawPixel(x, top, BRACKET);
    t.drawPixel(x, bottom, BRACKET);
  }
  if (c == v.playCol) t.drawFastVLine(x, top, WAVE_H + 1, PLAYHEAD);
}

static void drawWave(int i, bool recordingHere) {
  const Track &tr = tracks[i];
  WaveView &v = views[i];
  uint32_t rs = 0, rb = 0;
  if (tr.state != TrackState::Empty) trackRange(i, rs, rb);
  if (v.drawn && v.version == tr.overviewVersion && v.state == (uint8_t)tr.state &&
      v.rec == recordingHere && v.rs == rs && v.rb == rb)
    return;
  v.drawn = true;
  v.version = tr.overviewVersion;
  v.state = (uint8_t)tr.state;
  v.rec = recordingHere;
  v.rs = rs;
  v.rb = rb;
  v.visible = tr.state != TrackState::Empty && !recordingHere;
  if (v.visible) {
    uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
    v.rangeFirst = rs * OVERVIEW_COLS / loopBeats;
    v.rangeLast = max(v.rangeFirst, (int)(((rs + rb) * OVERVIEW_COLS + loopBeats - 1) / loopBeats) - 1);
  } else {
    v.playCol = -1;
  }
  for (int c = 0; c < OVERVIEW_COLS; c++) drawColumn(i, c);
}

// Playhead column for each track from the transport position and the
// track's range alignment; only the columns it leaves and enters are redrawn.
static void updatePlayheads() {
  noInterrupts();
  bool running = transport.running;
  double pos = transport.beat + (double)transport.sampleInBeat / transport.spb;   // in beats
  interrupts();
  for (int i = 0; i < NUM_TRACKS; i++) {
    const Track &tr = tracks[i];
    WaveView &v = views[i];
    int col = -1;
    if (v.visible && running && tr.live) {
      double inRange = fmod(pos - tr.rangeAnchorQ / 4.0, (double)tr.rangeBeats);
      if (inRange < 0) inRange += tr.rangeBeats;
      double loopBeat = tr.rangeStartBeat + inRange;
      col = min(OVERVIEW_COLS - 1, (int)(loopBeat * OVERVIEW_COLS / (tr.loopBars * BEATS_PER_BAR)));
    }
    if (col == v.playCol) continue;
    int old = v.playCol;
    v.playCol = col;
    if (old >= 0) drawColumn(i, old);
    if (col >= 0) drawColumn(i, col);
  }
}


static void drawHeader() {
  auto &t = board.tft();
  t.setTextSize(2);
  t.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  t.setCursor(4, TOP_Y);
  t.printf("%6.1f BPM ", bpm);
  noInterrupts();
  int32_t beat = transport.beat;
  uint32_t step = (uint64_t)transport.sampleInBeat * 4 / transport.spb;   // quarter beats
  interrupts();
  if (transport.running) {
    t.setTextColor(ILI9341_GREEN, ILI9341_BLACK);
    t.printf(" %3ld.%ld.%lu ", beat / BEATS_PER_BAR + 1, beat % BEATS_PER_BAR + 1, step + 1);
  } else {
    t.setTextColor(ILI9341_DARKGREY, ILI9341_BLACK);
    t.print(" stopped ");
  }
}

static void drawTrack(int i) {
  auto &t = board.tft();
  const Track &tr = tracks[i];
  int y = ROW_Y + i * ROW_H;
  bool isRec = recording.track == i && recording.mode != Recording::Idle;
  const char *state;
  uint16_t color;
  if (isRec) {
    switch (recording.mode) {
      case Recording::WaitBar: state = "WAIT"; color = ILI9341_YELLOW; break;
      case Recording::Running: state = "REC "; color = ILI9341_RED; break;
      default:                                       // stopped: still recording up to the length?
        if ((int32_t)(transport.now - recording.startSample) < (int32_t)recording.targetFrames) {
          state = "REC>"; color = ILI9341_RED;
        } else {
          state = "SAVE"; color = ILI9341_ORANGE;
        }
        break;
    }
  } else {
    switch (tr.state) {
      case TrackState::Empty:   state = "----"; color = ILI9341_DARKGREY; break;
      case TrackState::Playing: state = "PLAY"; color = ILI9341_GREEN; break;
      default:                  state = "MUTE"; color = ILI9341_BLUE; break;
    }
  }
  t.fillRect(0, y, 4, ROW_H - 2, i == selected ? ILI9341_WHITE : ILI9341_BLACK);
  t.setTextSize(1);
  t.setCursor(8, y + 6);
  t.setTextColor(color, ILI9341_BLACK);
  t.printf("%d %s ", i + 1, state);
  t.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  char line[64];
  if (isRec && recording.mode == Recording::Running) {
    uint32_t frames = recording.written;
    snprintf(line, sizeof line, "%5.1f beats  ovr %lu", (double)frames / transport.spb,
             recording.overruns);
  } else if (isRec && recording.mode == Recording::Finishing) {
    int32_t left = (int32_t)(recording.targetFrames - (transport.now - recording.startSample));
    snprintf(line, sizeof line, "-> %lu bars, %4.1f to go", recording.loopBars,
             left > 0 ? (double)left / transport.spb : 0.0);
  } else if (tr.state == TrackState::Empty) {
    snprintf(line, sizeof line, "%s", "");
  } else {
    uint32_t rs, rb;
    trackRange(i, rs, rb);
    char len[12];                                    // range length in bars
    if (rb < BEATS_PER_BAR) snprintf(len, sizeof len, "1/%lu", BEATS_PER_BAR / rb);
    else snprintf(len, sizeof len, "%lu", rb / BEATS_PER_BAR);
    snprintf(line, sizeof line, "%2lu bars  at %lu.%lu len %s",
             tr.loopBars, rs / BEATS_PER_BAR + 1, rs % BEATS_PER_BAR + 1, len);
  }
  // Pad to the waveform (text starts at x = 50) to clear older, longer text;
  // with wrapping off, anything longer is cut instead of landing elsewhere.
  line[TEXT_COLS] = 0;
  t.printf("%-*s", TEXT_COLS, line);
  drawWave(i, isRec);
}

// ---- Keys
static void onKeys(uint8_t keys, uint8_t pressed) {
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  Track &tr = tracks[selected];
  int prevSel = selected;

  if (opt) {
    // Range in beats, length a power of two. LEFT/RIGHT move the start by
    // one beat; UP/DOWN double/halve the length (doubling keeps the range
    // inside the loop).
    uint32_t rs, rb, loopBeats = tr.loopBars * BEATS_PER_BAR;
    trackRange(selected, rs, rb);                    // includes a queued change
    if (pressed & m8::BTN_LEFT && rs >= 1) setRange(selected, rs - 1, rb);
    if (pressed & m8::BTN_RIGHT && rs + 1 + rb <= loopBeats) setRange(selected, rs + 1, rb);
    if (pressed & m8::BTN_UP && rb * 2 <= loopBeats) setRange(selected, min(rs, loopBeats - rb * 2), rb * 2);
    if (pressed & m8::BTN_DOWN && rb > 1) setRange(selected, rs, rb / 2);
  } else {
    if (pressed & m8::BTN_UP) selected = (selected + NUM_TRACKS - 1) % NUM_TRACKS;
    if (pressed & m8::BTN_DOWN) selected = (selected + 1) % NUM_TRACKS;
    float step = shift ? 0.1f : 1.0f;
    if (pressed & m8::BTN_LEFT) setTempo(bpm - step);
    if (pressed & m8::BTN_RIGHT) setTempo(bpm + step);
  }

  if (pressed & m8::BTN_EDIT) {
    bool recordingHere = recording.track == selected && recording.mode != Recording::Idle;
    if (shift) {
      clearTrack(selected);
    } else if (tr.state == TrackState::Empty || recordingHere) {
      recordPress(selected);
    } else {
      tr.state = tr.state == TrackState::Playing ? TrackState::Muted : TrackState::Playing;
      trackChanged(selected);
    }
  }
  if (pressed & m8::BTN_PLAY) {
    if (transport.running) transportStop();
    else transportStart();
  }
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
  bpm = loadTempo();
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
    drawHeader();                                    // every pass: the clock shows steps
    updatePlayheads();
    drawTrack(nextRow);
    if (++nextRow >= NUM_TRACKS) nextRow = 0;
  }
}
