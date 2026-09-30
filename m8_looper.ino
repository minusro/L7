// m8_starter.ino — "hello world" custom firmware for the Dirtywave M8 Model:01
//
// Shows which board variant was detected, draws the 8 keys live, and plays a
// note through the I2S audio path when a key is held. Use it to confirm the
// pin map and key names on your unit before building anything bigger.
//
// Build: Teensyduino, board "Teensy 4.0", USB type "Serial" (or "Serial + MIDI + Audio").
// Libraries: ILI9341_t3n (KurtE), Audio (bundled with Teensyduino).

#include <Audio.h>
#include "M8Board.h"

m8::Board board;

// ---- Audio: Teensy Audio Library on SAI1 (MCLK 23, BCLK 21, LRCLK 20, TX 7, RX 8)
// One sine voice (osc -> env) per key, summed by two 4-input mixers into a master mixer.
AudioSynthWaveform  osc[8];
AudioEffectEnvelope env[8];
AudioMixer4         mixA, mixB, master;
AudioOutputI2S      i2sOut;
AudioConnection     v0(osc[0], env[0]), m0(env[0], 0, mixA, 0);
AudioConnection     v1(osc[1], env[1]), m1(env[1], 0, mixA, 1);
AudioConnection     v2(osc[2], env[2]), m2(env[2], 0, mixA, 2);
AudioConnection     v3(osc[3], env[3]), m3(env[3], 0, mixA, 3);
AudioConnection     v4(osc[4], env[4]), m4(env[4], 0, mixB, 0);
AudioConnection     v5(osc[5], env[5]), m5(env[5], 0, mixB, 1);
AudioConnection     v6(osc[6], env[6]), m6(env[6], 0, mixB, 2);
AudioConnection     v7(osc[7], env[7]), m7(env[7], 0, mixB, 3);
AudioConnection     ma(mixA, 0, master, 0), mb(mixB, 0, master, 1);
AudioConnection     outL(master, 0, i2sOut, 0), outR(master, 0, i2sOut, 1);
AudioControlSGTL5000 codec;      // SGTL5000 @0x0A on Wire, same as stock firmware

struct KeyUi { uint8_t bit; const char *name; int16_t x, y; float hz; };
// Laid out like the M8's physical key cluster. Index = voice number.
// C major C4..C5, ascending left to right across the columns (bottom key before top).
static const KeyUi KEYS[8] = {
  {m8::BTN_LEFT,  "LEFT",   10, 130, 261.63f},  // C4
  {m8::BTN_DOWN,  "DOWN",   70, 130, 293.66f},  // D4
  {m8::BTN_UP,    "UP",     70,  70, 329.63f},  // E4
  {m8::BTN_RIGHT, "RIGHT", 130, 130, 349.23f},  // F4
  {m8::BTN_OPT,   "OPT",   200, 130, 392.00f},  // G4
  {m8::BTN_SHIFT, "SHIFT", 200, 70,  440.00f},  // A4
  {m8::BTN_EDIT,  "EDIT",  260, 130, 493.88f},  // B4
  {m8::BTN_PLAY,  "PLAY",  260, 70,  523.25f},  // C5
};

static void drawKey(const KeyUi &k, bool down) {
  auto &t = board.tft();
  uint16_t bg = down ? ILI9341_ORANGE : ILI9341_DARKGREY;
  t.fillRoundRect(k.x, k.y, 54, 50, 6, bg);
  t.setTextColor(down ? ILI9341_BLACK : ILI9341_WHITE);
  t.setTextSize(1);
  t.setCursor(k.x + 6, k.y + 21);
  t.print(k.name);
}

static void drawRaw(uint8_t keys) {
  auto &t = board.tft();
  t.fillRect(10, 200, 300, 30, ILI9341_BLACK);
  t.setTextColor(ILI9341_GREEN);
  t.setTextSize(2);
  t.setCursor(10, 205);
  t.printf("raw 0x%02X  ", keys);
  for (int b = 7; b >= 0; b--) t.print((keys >> b) & 1);
}

void setup() {
  Serial.begin(115200);
  AudioMemory(32);
  for (int i = 0; i < 8; i++) {
    osc[i].begin(1.0f, KEYS[i].hz, WAVEFORM_SINE);
    env[i].attack(5); env[i].release(150);
  }
  // Each voice ends up at 1/8 full scale, so all 8 keys together can't clip.
  for (int ch = 0; ch < 4; ch++) { mixA.gain(ch, 0.25f); mixB.gain(ch, 0.25f); }
  master.gain(0, 0.5f); master.gain(1, 0.5f);

  bool keysOk = board.begin();

  // Stock firmware retries enable() up to 6 times, 10 ms apart.
  bool codecOk = false;
  for (int i = 0; i < 6 && !(codecOk = codec.enable()); i++) delay(10);
  codec.volume(0.6f);

  auto &t = board.tft();
  t.setTextColor(ILI9341_WHITE);
  t.setTextSize(2);
  t.setCursor(10, 10);
  t.print("M8 looper test");
  t.setTextSize(1);
  t.setCursor(10, 36);
  t.printf("board variant %c   keys %s", board.variant() == m8::VARIANT_A ? 'A' : 'B',
           keysOk ? "OK" : "NOT FOUND");
  t.setCursor(10, 48);
  t.printf("codec SGTL5000 %s", codecOk ? "OK" : "NOT FOUND");
  for (auto &k : KEYS) drawKey(k, false);
  drawRaw(0);

  Serial.printf("M8 starter: variant %c, key expander %s, codec %s\n",
                board.variant() == m8::VARIANT_A ? 'A' : 'B', keysOk ? "ok" : "missing",
                codecOk ? "ok" : "missing");
}

// Short press shows a prompt; keep holding ~1 s to power off, like the stock firmware.
static void handlePowerButton() {
  if (!board.powerButtonPressed()) return;
  auto &t = board.tft();
  t.fillRect(10, 185, 300, 10, ILI9341_BLACK);
  t.setTextColor(ILI9341_RED);
  t.setTextSize(1);
  t.setCursor(10, 185);
  t.print("POWER OFF? (KEEP HOLDING)");
  uint32_t start = millis();
  while (board.powerButtonHeld()) {
    if (millis() - start >= 1000) {
      for (auto &e : env) e.noteOff();
      codec.volume(0.0f);
      delay(200);
      board.powerOff();
    }
  }
  t.fillRect(10, 185, 300, 10, ILI9341_BLACK);
  board.powerButtonPressed();  // drop a request latched while we were waiting
}

void loop() {
  handlePowerButton();

  static uint8_t prev = 0;
  uint8_t now;
  if (!board.keysChanged(now)) return;

  for (auto &k : KEYS) {
    bool was = prev & k.bit, is = now & k.bit;
    if (was != is) drawKey(k, is);
  }
  drawRaw(now);
  Serial.printf("keys 0x%02X\n", now);

  // Polyphonic: every key gates its own voice.
  for (int i = 0; i < 8; i++) {
    bool was = prev & KEYS[i].bit, is = now & KEYS[i].bit;
    if (is && !was) env[i].noteOn();
    if (was && !is) env[i].noteOff();
  }
  prev = now;
}
