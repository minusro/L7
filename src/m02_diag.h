// m02_diag.h — a throwaway diagnostic for the Model:02: is anything on its
// board wired to sense a plug in the line-in jack? (The stock firmware reads
// no such pin - docs/HARDWARE.md - and the codec hears the line input or the
// mic, never both, so L7 can't listen for a cable while it records the mic.)
//
// Built only when M02_DIAG is 1, in a test build's own copy (0 here: the
// normal builds compile none of it). Header-only and included from L7.ino
// alone, so the normal builds don't even gain an empty object file (one
// reorders the linker's branch stubs, and the image is no longer
// byte-identical).
//
// It takes over once the board and the codec are up and never returns: a
// screen of every free pin's level, read with a pull-up and with a
// pull-down; the analog pins' readings; and the line input's and the mic's
// levels, the codec switched between the two. Whatever changes is lit and
// stays lit until a key is pressed.
#pragma once

#define M02_DIAG 0

// (The implementation: Model:02 only, and only with M02_DIAG 1.)
#if M02_DIAG
#include "screen.h"
#if M8_MODEL == 2
#include "app.h"
#include "engine.h"
#include <Audio.h>
#include <stdarg.h>
#include <math.h>

extern AudioControlSGTL5000 codec;

namespace {

// Pins never reconfigured here: 0/1 MIDI in and the boot line, 4 backlight,
// 5 display reset, 6/28/40 the power lines, 7/8/20/21/23 audio (SAI1), 9 the
// speaker enable, 10-13 the display's SPI, 14 MIDI out, 18/19 I2C (keys,
// codec, charger), 34-39 the SD card.
// Read as they are (already inputs): 2 charger INT, 15 key INT, 16 touch
// INT, 17 variant B's SD detect.
constexpr uint8_t READ_ONLY[] = {2, 15, 16, 17};
// Free (the stock firmware never sets them up): read with a pull-up, then
// with a pull-down. Unconnected, a pin follows the pulls ("10"); tied or
// driven, it doesn't ("00", "11"); a jack switch would flip one.
constexpr uint8_t FREE[] = {3, 24, 25, 26, 29, 30, 31, 32, 33, 41, 42, 43, 44, 45};
// Analog (the MicroMod's A0-A13 are pins 14-27): 22 the headphone sense, as
// the check that this works (headphones in or out must light it); 27 the
// random seed's; 24-26 free. Read with the pull-down on, as the headphone
// sense is.
constexpr uint8_t ANALOG[] = {22, 24, 25, 26, 27};
constexpr int ND = sizeof READ_ONLY + sizeof FREE, NA = sizeof ANALOG;
constexpr uint32_t BASELINE_MS = 500;       // (readings taken as they are first, then changes counted)
constexpr int ANALOG_LIT = 80;              // (a min-max spread this wide lights an analog pin)
constexpr uint32_t SETTLE_MS = 100, WINDOW_MS = 400;   // the codec on one input: settling, then measured until here
constexpr int HISTORY = 6;                  // windows kept per input (~5 s)

struct Dig { uint8_t pin; bool free; uint8_t now, prev; uint16_t changes; };
struct Ana { uint8_t pin; int now, lo, hi; };
struct Meter { float peak = -99, floor = -99; float pk[HISTORY], fl[HISTORY]; int n = 0; };
Dig dig[ND];
Ana ana[NA];
Meter meters[2];                            // [0] line, [1] mic
uint32_t clearedAt, scans;

constexpr uint16_t c565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
constexpr uint16_t BLACK = 0, WHITE = 0xFFFF, GREY = c565(150, 150, 150), LIT = c565(48, 230, 170);

void text(int x, int y, int size, uint16_t ink, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  auto &t = board.tft();
  t.setTextSize(size);
  t.setTextColor(ink, BLACK);
  t.setCursor(x, y);
  t.print(buf);
}

uint8_t readDigital(const Dig &d) {
  if (!d.free) return digitalRead(d.pin) ? 1 : 0;
  pinMode(d.pin, INPUT_PULLUP);
  delayMicroseconds(300);
  uint8_t up = digitalRead(d.pin);
  pinMode(d.pin, INPUT_PULLDOWN);           // (left with the pull-down: the analog reads below want it)
  delayMicroseconds(300);
  return (up << 1) | digitalRead(d.pin);
}

void setInput(int mic) {
  if (mic) {                                // the stock Model:02's: +30 dB preamp, bias on, +12 dB ADC gain
    codec.inputSelect(AUDIO_INPUT_MIC);
    codec.micGain(30);
    codec.lineInLevel(8, 8);
  } else {                                  // L7's line input as it runs (0x55)
    codec.inputSelect(AUDIO_INPUT_LINEIN);
    codec.lineInLevel(5);
  }
}

float db(float x) { return 20 * log10f(max(x, 1e-5f)); }

void clearAll() {
  clearedAt = millis();
  for (auto &d : dig) d.changes = 0;
  for (auto &a : ana) { a.lo = 1023; a.hi = 0; }
  for (auto &m : meters) m.n = 0;
}

void drawStatic() {
  auto &t = board.tft();
  t.fillScreen(BLACK);
  text(4, 2, 2, LIT, "L7 MODEL:02 DIAGNOSTIC: LINE IN SENSE");
  text(4, 22, 1, GREY, "1 PLUG A CABLE INTO LINE IN AND PULL IT OUT, 3 TIMES. PHOTO.");
  text(4, 32, 1, GREY, "2 PHOTO WITH A CABLE IN (SOURCE OFF), THEN WITH NOTHING IN.");
  text(4, 42, 1, GREY, "3 CLAP NEAR THE M8: MIC PEAK. HEADPHONES IN/OUT LIGHT A22. ANY KEY CLEARS.");
  text(4, 56, 1, WHITE, "PINS, READ WITH A PULL-UP / A PULL-DOWN (ONE DIGIT: AS IS). xN = CHANGES");
  text(4, 162, 1, WHITE, "ANALOG 0-1023: NOW  MIN-MAX      (A22 = THE HEADPHONE SENSE)");
  text(4, 232, 1, WHITE, "AUDIO DBFS");           // (the labels over drawValues()'s columns: 12 px a character there)
  text(4 + 7 * 12, 232, 1, WHITE, "PEAK");
  text(4 + 13 * 12, 232, 1, WHITE, "FLOOR");
  text(4 + 23 * 12, 232, 1, WHITE, "5 S: LOUDEST");
  text(4 + 30 * 12 + 18, 232, 1, WHITE, "QUIETEST");
  text(4, 286, 1, GREY, "MIC +30 DB PREAMP +12 DB ADC, BIAS ON (DIRTYWAVE'S). LINE AS IN L7. 0.4 S EACH");
}

void drawValues() {
  for (int i = 0; i < ND; i++) {
    const Dig &d = dig[i];
    int x = 4 + (i % 4) * 120, y = 66 + (i / 4) * 18;
    if (d.free) text(x, y, 2, d.changes ? LIT : WHITE, "%2d %d%d x%-2u", d.pin, d.now >> 1, d.now & 1, min(d.changes, (uint16_t)99));
    else        text(x, y, 2, d.changes ? LIT : GREY, "%2d %d  x%-2u", d.pin, d.now, min(d.changes, (uint16_t)99));
  }
  for (int i = 0; i < NA; i++) {
    const Ana &a = ana[i];
    int x = 4 + (i % 2) * 240, y = 172 + (i / 2) * 18;
    bool lit = a.hi >= a.lo && a.hi - a.lo > ANALOG_LIT;
    if (a.hi >= a.lo) text(x, y, 2, lit ? LIT : WHITE, "A%d %4d %4d-%-4d", a.pin, a.now, a.lo, a.hi);
    else              text(x, y, 2, WHITE, "A%d %4d           ", a.pin, a.now);
  }
  for (int s = 0; s < 2; s++) {
    const Meter &m = meters[s];
    float loud = -99, quiet = 0;
    for (int k = 0; k < min(m.n, HISTORY); k++) { loud = max(loud, m.pk[k]); quiet = min(quiet, m.fl[k]); }
    text(4, 244 + s * 20, 2, WHITE, "%-4s %6.1f %6.1f     %6.1f %6.1f", s ? "MIC" : "LINE", m.peak, m.floor, loud, m.n ? quiet : -99.0f);
  }
  text(4, 302, 1, GREY, "SCAN %-8lu  THE POWER BUTTON SWITCHES OFF", (unsigned long)scans);
}

}  // namespace

inline void m02Diag() {
  inputMonitored = false;                   // (the mic must not reach the speaker: at +42 dB it would howl)
  for (int i = 0; i < ND; i++) {
    bool f = i >= (int)sizeof READ_ONLY;
    dig[i] = {f ? FREE[i - sizeof READ_ONLY] : READ_ONLY[i], f, 0, 0, 0};
  }
  for (int i = 0; i < NA; i++) ana[i] = {ANALOG[i], 0, 1023, 0};
  drawStatic();
  clearAll();
  int input = 0;
  setInput(input);
  uint32_t switchedAt = millis(), drawnAt = 0;
  bool measuring = false;
  for (;;) {
    if (board.powerButtonPressed()) board.powerOff();
    uint8_t k;
    if (board.keysChanged(k) && k) clearAll();
    const bool counting = millis() - clearedAt >= BASELINE_MS;
    for (auto &d : dig) {
      d.now = readDigital(d);
      if (counting && d.now != d.prev) d.changes++;
      d.prev = d.now;
    }
    for (auto &a : ana) {
      a.now = analogRead(a.pin);
      if (counting) { a.lo = min(a.lo, a.now); a.hi = max(a.hi, a.now); }
    }
    scans++;
    uint32_t now = millis();
    if (!measuring && now - switchedAt >= SETTLE_MS) {
      lineInPeak = 0;                       // (the engine's: the loudest and quietest block peaks since)
      lineInFloor = 1;
      measuring = true;
    }
    if (measuring && now - switchedAt >= WINDOW_MS) {
      Meter &m = meters[input];
      m.peak = db(lineInPeak);
      m.floor = db(lineInFloor);
      m.pk[m.n % HISTORY] = m.peak;
      m.fl[m.n % HISTORY] = m.floor;
      m.n++;
      input ^= 1;
      setInput(input);
      switchedAt = now;
      measuring = false;
    }
    if (now - drawnAt >= 200) {
      drawValues();
      drawnAt = now;
    }
    delay(5);
  }
}

#endif  // M8_MODEL == 2
#endif  // M02_DIAG
