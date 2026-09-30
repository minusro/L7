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
AudioSynthWaveform  osc;
AudioEffectEnvelope env;
AudioOutputI2S      i2sOut;
AudioConnection     c1(osc, env);
AudioConnection     c2(env, 0, i2sOut, 0);
AudioConnection     c3(env, 0, i2sOut, 1);

struct KeyUi { uint8_t bit; const char *name; int16_t x, y; float hz; };
// Laid out like the M8's physical key cluster.
static const KeyUi KEYS[8] = {
  {m8::BTN_UP,    "UP",     70,  70, 392.00f},
  {m8::BTN_LEFT,  "LEFT",   10, 130, 261.63f},
  {m8::BTN_DOWN,  "DOWN",   70, 130, 329.63f},
  {m8::BTN_RIGHT, "RIGHT", 130, 130, 440.00f},
  {m8::BTN_SHIFT, "SHIFT", 200, 70,  493.88f},
  {m8::BTN_PLAY,  "PLAY",  260, 70,  523.25f},
  {m8::BTN_OPT,   "OPT",   200, 130, 587.33f},
  {m8::BTN_EDIT,  "EDIT",  260, 130, 659.25f},
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
  AudioMemory(12);
  osc.begin(0.25f, 440.0f, WAVEFORM_SAWTOOTH);
  env.attack(5); env.release(150);

  bool keysOk = board.begin();

  auto &t = board.tft();
  t.setTextColor(ILI9341_WHITE);
  t.setTextSize(2);
  t.setCursor(10, 10);
  t.print("M8 looper test");
  t.setTextSize(1);
  t.setCursor(10, 36);
  t.printf("board variant %c   keys %s", board.variant() == m8::VARIANT_A ? 'A' : 'B',
           keysOk ? "OK" : "NOT FOUND");
  for (auto &k : KEYS) drawKey(k, false);
  drawRaw(0);

  Serial.printf("M8 starter: variant %c, key expander %s\n",
                board.variant() == m8::VARIANT_A ? 'A' : 'B', keysOk ? "ok" : "missing");
}

void loop() {
  static uint8_t prev = 0;
  uint8_t now;
  if (!board.keysChanged(now)) return;

  for (auto &k : KEYS) {
    bool was = prev & k.bit, is = now & k.bit;
    if (was != is) drawKey(k, is);
  }
  drawRaw(now);
  Serial.printf("keys 0x%02X\n", now);

  // Play the note of the lowest pressed key; release when nothing is held.
  if (now) {
    for (auto &k : KEYS) if (now & k.bit) { osc.frequency(k.hz); break; }
    if (!prev) env.noteOn();
  } else {
    env.noteOff();
  }
  prev = now;
}
