// audio_test.ino — stereo input test for the Dirtywave M8 Model:01
//
// Step 1 of the looper plan: check that the input jack reaches the SGTL5000,
// that both channels arrive, and how long the round trip takes.
//   * the input is monitored straight to the output (on/off with PLAY)
//   * peak meters for left and right, with a clip marker
//   * OPT switches the codec input between LINE IN and MIC, to find out which
//     one the M8's jack is wired to
//   * UP/DOWN change the input gain
//   * EDIT measures round-trip latency: connect the output to the input with a
//     cable first. Monitoring is muted during the measurement (no feedback).
// Hold the power button ~1 s to switch off.
//
// Build: Teensyduino, board "Teensy 4.0", USB type "Serial".
// Libraries: ILI9341_t3n (KurtE), Audio (bundled with Teensyduino).

#include <Audio.h>
#include "M8Board.h"

m8::Board board;

// ---- Audio object: monitor + peak meters + latency probe
class MonitorProbe : public AudioStream {
public:
  MonitorProbe() : AudioStream(2, inputQueue_) {}

  volatile bool monitor = true;
  volatile int16_t peakL = 0, peakR = 0;              // max |sample| since last read
  volatile bool clipL = false, clipR = false;

  // Latency probe: 0 idle, 1 armed, 2 waiting for the click to come back
  volatile int probeState = 0;
  volatile int32_t probeResult = -1;                  // samples, or -1 = timed out

  void startProbe() { probeResult = -1; probeState = 1; }

  void update() override {
    audio_block_t *in[2] = {receiveReadOnly(0), receiveReadOnly(1)};
    for (int c = 0; c < 2; c++) {
      if (!in[c]) continue;
      int16_t pk = 0;
      bool clip = false;
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        int16_t s = in[c]->data[i];
        int16_t a = s < 0 ? (s == -32768 ? 32767 : -s) : s;
        if (a > pk) pk = a;
        if (a >= 32000) clip = true;
      }
      if (c == 0) { if (pk > peakL) peakL = pk; if (clip) clipL = true; }
      else        { if (pk > peakR) peakR = pk; if (clip) clipR = true; }
    }

    if (probeState) {
      runProbe(in);
    } else if (monitor) {
      for (int c = 0; c < 2; c++) if (in[c]) transmit(in[c], c);
    }
    for (int c = 0; c < 2; c++) if (in[c]) release(in[c]);
  }

private:
  audio_block_t *inputQueue_[2];
  int32_t elapsed_ = 0;

  void runProbe(audio_block_t *in[2]) {
    audio_block_t *out = allocate();
    if (out) memset(out->data, 0, sizeof out->data);
    if (probeState == 1) {                            // send one full-scale click
      if (out) for (int i = 0; i < 8; i++) out->data[i] = 30000;
      elapsed_ = 0;
      probeState = 2;
    } else {                                          // look for it in the left input
      if (in[0]) {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
          int16_t s = in[0]->data[i];
          if (s > 8000 || s < -8000) { probeResult = elapsed_ + i; probeState = 0; break; }
        }
      }
      elapsed_ += AUDIO_BLOCK_SAMPLES;
      if (probeState && elapsed_ > (int32_t)AUDIO_SAMPLE_RATE_EXACT) probeState = 0;  // 1 s: give up
    }
    if (out) {
      transmit(out, 0);
      transmit(out, 1);
      release(out);
    }
  }
};

AudioInputI2S        i2sIn;
MonitorProbe         probe;
AudioOutputI2S       i2sOut;
AudioConnection      c1(i2sIn, 0, probe, 0), c2(i2sIn, 1, probe, 1);
AudioConnection      c3(probe, 0, i2sOut, 0), c4(probe, 1, i2sOut, 1);
AudioControlSGTL5000 codec;

// ---- State
static bool useMic = false;
static int lineLevel = 5;          // SGTL5000 line-in gain 0..15 (5 = 1.33 Vpp full scale)
static int micGainDb = 30;         // 0..63 dB
static int32_t lastLatency = -2;   // -2 = not measured yet

static void applyInput() {
  codec.inputSelect(useMic ? AUDIO_INPUT_MIC : AUDIO_INPUT_LINEIN);
  if (useMic) codec.micGain(micGainDb);
  else codec.lineInLevel(lineLevel);
}

// ---- Screen
static constexpr int METER_X = 40, METER_W = 260, METER_H = 24;

static void drawStatic() {
  auto &t = board.tft();
  t.fillScreen(ILI9341_BLACK);
  t.setTextColor(ILI9341_WHITE);
  t.setTextSize(2);
  t.setCursor(10, 8);
  t.print("Audio input test");
  t.setTextSize(1);
  t.setCursor(10, 60);  t.print("L");
  t.setCursor(10, 100); t.print("R");
  t.setTextColor(ILI9341_DARKGREY);
  t.setCursor(10, 196); t.print("OPT: line in / mic   UP/DOWN: gain");
  t.setCursor(10, 208); t.print("PLAY: monitor on/off");
  t.setCursor(10, 220); t.print("EDIT: latency test (cable from output to input)");
}

static void drawStatus() {
  auto &t = board.tft();
  t.fillRect(0, 30, 320, 20, ILI9341_BLACK);
  t.setTextSize(1);
  t.setTextColor(ILI9341_YELLOW);
  t.setCursor(10, 34);
  if (useMic) t.printf("input MIC   gain %d dB", micGainDb);
  else t.printf("input LINE IN   level %d/15", lineLevel);
  t.setCursor(200, 34);
  t.print(probe.monitor ? "monitor ON" : "monitor off");

  t.fillRect(0, 140, 320, 40, ILI9341_BLACK);
  t.setTextColor(ILI9341_WHITE);
  t.setCursor(10, 146);
  if (lastLatency == -2) t.print("latency: not measured");
  else if (lastLatency == -1) t.print("latency: no click came back (is the cable connected?)");
  else t.printf("round trip: %ld samples = %.2f ms", lastLatency,
                lastLatency * 1000.0f / AUDIO_SAMPLE_RATE_EXACT);
}

// Peak meter on a dB scale from -60 to 0 dBFS.
static void drawMeter(int y, int16_t peak, bool clip) {
  float db = peak > 0 ? 20.0f * log10f(peak / 32768.0f) : -60.0f;
  int w = (int)((db + 60.0f) / 60.0f * METER_W);
  w = constrain(w, 0, METER_W);
  uint16_t color = db > -6 ? ILI9341_RED : db > -18 ? ILI9341_YELLOW : ILI9341_GREEN;
  auto &t = board.tft();
  t.fillRect(METER_X, y, w, METER_H, color);
  t.fillRect(METER_X + w, y, METER_W - w, METER_H, ILI9341_DARKGREY);
  t.fillRect(METER_X + METER_W + 4, y, 10, METER_H, clip ? ILI9341_RED : ILI9341_BLACK);
}

// ---- Power
static void handlePowerButton() {
  if (!board.powerButtonPressed()) return;
  uint32_t start = millis();
  while (board.powerButtonHeld())
    if (millis() - start >= 1000) { codec.volume(0.0f); board.powerOff(); }
  board.powerButtonPressed();
}

void setup() {
  Serial.begin(115200);
  AudioMemory(16);
  board.begin();
  board.powerButtonPressed();                        // the press that switched us on
  for (int i = 0; i < 6 && !codec.enable(); i++) delay(10);
  codec.volume(0.6f);
  applyInput();
  drawStatic();
  drawStatus();
}

void loop() {
  handlePowerButton();

  uint8_t keys;
  static uint8_t prev = 0;
  if (board.keysChanged(keys)) {
    uint8_t pressed = keys & ~prev;
    prev = keys;
    if (pressed & m8::BTN_OPT) useMic = !useMic;
    if (pressed & m8::BTN_UP) { if (useMic) micGainDb = min(micGainDb + 3, 63); else lineLevel = min(lineLevel + 1, 15); }
    if (pressed & m8::BTN_DOWN) { if (useMic) micGainDb = max(micGainDb - 3, 0); else lineLevel = max(lineLevel - 1, 0); }
    if (pressed & m8::BTN_PLAY) probe.monitor = !probe.monitor;
    if (pressed & (m8::BTN_OPT | m8::BTN_UP | m8::BTN_DOWN)) applyInput();
    if (pressed & m8::BTN_EDIT) {
      probe.startProbe();
      while (probe.probeState) {}                    // at most ~1 s
      lastLatency = probe.probeResult;
      Serial.printf("latency: %ld samples\n", lastLatency);
    }
    if (pressed) drawStatus();
  }

  // Meters at ~30 Hz, holding the peak seen since the last redraw.
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 33) {
    lastDraw = millis();
    drawMeter(56, probe.peakL, probe.clipL);
    drawMeter(96, probe.peakR, probe.clipR);
    probe.peakL = probe.peakR = 0;
    static uint32_t clipClear = 0;
    if (millis() - clipClear > 1500) { probe.clipL = probe.clipR = false; clipClear = millis(); }
  }
}
