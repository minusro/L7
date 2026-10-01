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
//   OPT+LEFT/RIGHT   move the loop range   OPT+UP/DOWN  range longer/shorter
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
static constexpr int ROW_Y = 30, ROW_H = 22;

static void drawHelp() {
  auto &t = board.tft();
  t.fillScreen(ILI9341_BLACK);
  t.setTextSize(1);
  t.setTextColor(ILI9341_DARKGREY);
  t.setCursor(4, 212); t.print("UP/DN track  L/R tempo  PLAY start/stop  EDIT rec/mute");
  t.setCursor(4, 224); t.print("SHIFT+EDIT clear  OPT+L/R range pos  OPT+UP/DN range len");
}

static void drawHeader() {
  auto &t = board.tft();
  t.setTextSize(2);
  t.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  t.setCursor(4, 4);
  t.printf("%6.1f BPM ", bpm);
  int32_t beat = transport.beat;
  if (transport.running) {
    t.setTextColor(ILI9341_GREEN, ILI9341_BLACK);
    t.printf(" %3ld.%ld  ", beat / BEATS_PER_BAR + 1, beat % BEATS_PER_BAR + 1);
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
      default:                 state = "SAVE"; color = ILI9341_ORANGE; break;
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
    snprintf(line, sizeof line, "%5.1f beats   overruns %lu", (double)frames / transport.spb,
             recording.overruns);
  } else if (tr.state == TrackState::Empty) {
    snprintf(line, sizeof line, "%s", "");
  } else {
    snprintf(line, sizeof line, "%2lu bars  range %lu-%lu  @%.1f  x%.2f  drops %lu",
             tr.loopBars, tr.rangeStartBar + 1, tr.rangeStartBar + tr.rangeBars,
             bpmForSpb(tr.spbNative), (double)tr.spbNative / transport.spb, tr.underruns);
  }
  t.printf("%-46s", line);
}

// ---- Keys
static void onKeys(uint8_t keys, uint8_t pressed) {
  bool shift = keys & m8::BTN_SHIFT, opt = keys & m8::BTN_OPT;
  Track &tr = tracks[selected];
  int prevSel = selected;

  if (opt) {
    if (pressed & m8::BTN_LEFT && tr.rangeStartBar > 0)
      setRange(selected, tr.rangeStartBar - 1, tr.rangeBars);
    if (pressed & m8::BTN_RIGHT && tr.rangeStartBar + tr.rangeBars < tr.loopBars)
      setRange(selected, tr.rangeStartBar + 1, tr.rangeBars);
    if (pressed & m8::BTN_UP) setRange(selected, tr.rangeStartBar, tr.rangeBars + 1);
    if (pressed & m8::BTN_DOWN && tr.rangeBars > 1) setRange(selected, tr.rangeStartBar, tr.rangeBars - 1);
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
      saveTrackInfo(selected);
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
  AudioMemory(12);
  board.begin();
  board.powerButtonPressed();                        // the press that switched us on
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(5);
  codec.volume(0.6f);

  drawHelp();
  if (!storageBegin()) fatal("No SD card: the looper keeps its tracks in /looper on the card.");
  bpm = loadTempo();
  transport.spb = spbForBpm(bpm);
  midiBegin(board.pins().midiOut);
}

void loop() {
  handlePowerButton();
  serviceStorage();

  uint8_t keys;
  static uint8_t prev = 0;
  if (board.keysChanged(keys)) {
    onKeys(keys, keys & ~prev);
    prev = keys;
  }

  if (tempoDirty && millis() - tempoChangedMs > 2000) {   // save once it settles
    saveTempo(bpm);
    tempoDirty = false;
  }

  // Redraw a little at a time so SD streaming keeps going.
  static uint32_t lastDraw = 0;
  static int nextRow = -1;
  if (millis() - lastDraw >= 30) {
    lastDraw = millis();
    if (nextRow < 0) drawHeader();
    else drawTrack(nextRow);
    if (++nextRow >= NUM_TRACKS) nextRow = -1;
  }
}
