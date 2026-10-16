// demo.cpp — renders a demo video of L7: the real app (the sketch's setup()
// and loop(), the engine, the UI) run headless in virtual time, its keys
// pressed by a script, a "performer" playing synthesized parts (drums, bass,
// chords, a lead) into its line input in time with its clock. Writes the
// screen 30 times a second (raw RGB565, with the keys held and the caption)
// and the output audio (WAV); compose.py turns them into the video.
//
//   ./build.sh            (builds, runs, composes a video per style: tools/demo/L7 demo - <style>.mp4)
//   l7demo <out-dir> [style]   (lofi, house, synthwave, boombap, jazz)
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "app.h"
#include "engine.h"
#include "ui.h"
#include "input.h"

void setup();
void loop();
extern LooperEngine engine;

// ---- What the stand-ins expect from their host
std::string simRoot;
SDClass SD;
SerialStub Serial;
CrashReportStub CrashReport;
int simFailIo = 0, simReinits = 0, simFsChanges = 0, simFileCreates = 0, simSampleTruncates = 0;
void (*simSdLatency)(size_t, bool) = nullptr;

namespace looper {
void (*usbDriveTick)() = nullptr;
void (*usbDriveReload)() = nullptr;
void usbDriveBegin() {}
bool usbDriveMode() { return false; }
}  // namespace looper
void midiUsbBegin() {}
bool appLoaderFlash(FsFile &, const char **err, void (*)(int), void (*)()) {   // (no app switching here)
  *err = "NOT IN THE DEMO";
  return false;
}

// ---- Virtual time. Every look at the clock moves it on a little (so waits
// on it end); the audio "interrupt" runs whenever a block is due and
// interrupts aren't held off; the screen is taken 30 times a second.
static constexpr double SR = 44100.0, BLOCK_US = AUDIO_BLOCK_SAMPLES * 1e6 / SR, FRAME_US = 1e6 / 30;
static double nowUs = 0, nextBlockUs = 0, nextFrameUs = 0;
static bool inAudio = false;
static int irqDepth = 0;
static void audioBlock();
static void takeFrame();
static void skipSplash();
static void due() {
  if (!inAudio && irqDepth == 0)
    while (nowUs >= nextBlockUs) {
      inAudio = true;
      audioBlock();
      inAudio = false;
      nextBlockUs += BLOCK_US;
    }
  skipSplash();
  while (nowUs >= nextFrameUs) {
    takeFrame();
    nextFrameUs += FRAME_US;
  }
}
// The splash cut short (a key tapped while it shows: straight to its end, the
// press taken by it, not acted on).
static bool splashing = true;
static void skipSplash() {
  if (!splashing) return;
  if (nowUs >= 2.3e6 && nowUs < 2.36e6) board.heldKeys = m8::BTN_PLAY;
  else if (nowUs >= 2.36e6 && board.heldKeys == m8::BTN_PLAY) board.heldKeys = 0;
}
static void advance(double us) {
  const double end = nowUs + us;
  while (nowUs < end) {
    nowUs = std::min(end, nowUs + 100);
    due();
  }
}
uint32_t micros() {
  if (!inAudio) {
    nowUs += 1;
    due();
  }
  return (uint32_t)nowUs;
}
uint32_t millis() { return micros() / 1000; }
void delay(uint32_t ms) { advance(ms * 1000.0); }
void delayMicroseconds(uint32_t us) { advance(us); }
void yield() {}
void noInterrupts() { irqDepth++; }
void interrupts() {
  if (irqDepth > 0 && --irqDepth == 0) due();
}

// ---- The performer: a beat in one of several styles (style()), its parts as
// notes over one, two or four bars (start and length in 16ths; swing and a
// little looseness per style), played into the line input over the beats the
// script gives it, in time with the clock. The loops: drums 1 bar, bass 2,
// keys 4 (over the bass's two roots, twice), lead 2, pluck 1.
enum Part { DRUMS, BASS, KEYS, LEAD, PLUCK, PARTS };
enum Voice {
  KICK, SNARE, GHOST, HAT, VINYL, BASSV, EPIANO, HORN, PLUCKV,         // (lo-fi)
  KICK909, CLAP, OHAT, SHAKER, SUBPLUCK, STAB, SUPERSAW, MARIMBA,      // (house)
  GATEDSNARE, SAWBASS, PADSAW, SAWLEAD, ARPSQ,                         // (synthwave)
  BBKICK, BBSNARE, ORGAN, FLUTE, VIBES,                                // (boom bap)
  RIDE, BRUSH, FEATHER, UPRIGHT, PIANO, SAX, JAZZGTR,                  // (jazz)
  KALIMBA                                                              // (lofi's lead)
};
struct Note { float at, len; int voice, midi; float gain; };
static std::vector<Note> parts[PARTS];
static const int PART_BEATS[PARTS] = {4, 8, 16, 8, 4};
struct Playing { int part; double from, to; };   // (transport beats)
static std::vector<Playing> playing;

struct Style {
  const char *name;                               // (its video: "L7 demo - <name>.mp4")
  float bpm, swing16, swing8, loose;              // (the odd 16ths late, the off 8ths late (in 16ths), a random nudge)
  const char *bass, *keys, *lead, *pluck;         // (the parts, as the captions name them)
};
static const Style STYLES[] = {
  {"lofi", 85, 0.28f, 0, 0.06f, "a 2-bar bassline", "4 bars of electric piano", "a 2-bar kalimba line", "a 1-bar pluck"},
  {"house", 122, 0.1f, 0, 0.02f, "a 2-bar bassline", "4 bars of chord stabs", "a 2-bar chorused saw pad", "a 1-bar marimba riff"},
  {"synthwave", 100, 0, 0, 0.0f, "a 2-bar octave bass", "4 bars of pads", "a 2-bar saw lead", "a 1-bar arpeggio"},
  {"boombap", 90, 0.22f, 0, 0.05f, "a 2-bar sub bass", "4 bars of organ", "a 2-bar flute line", "a 1-bar vibraphone riff"},
  {"jazz", 112, 0, 0.67f, 0.05f, "a 2-bar walking bass", "4 bars of piano", "a 2-bar guitar line", "a 1-bar vibraphone riff"},
};
static const Style *style = &STYLES[0];

static float hum(int i) { return ((i * 2654435761u) >> 24) / 255.0f - 0.5f; }   // (a nudge: -0.5..0.5)
static float swung(float s16, int i) {
  const int whole = (int)s16;
  return s16 + (whole % 2 ? style->swing16 : 0) + (whole % 4 == 2 ? style->swing8 : 0) + style->loose * hum(i);
}

static void writeParts() {
  for (auto &p : parts) p.clear();
  auto &d = parts[DRUMS];
  auto &b = parts[BASS];
  auto &k = parts[KEYS];
  auto &l = parts[LEAD];
  auto &p = parts[PLUCK];
  int n = 0;
  auto drum = [&](std::initializer_list<int> at, float len, int v, float gain) { for (int s16 : at) d.push_back({swung(s16, n++), len, v, 0, gain}); };
  // Chords: each of the four bars' voicing (four notes) struck at the 16ths given.
  auto chords = [&](const int (&c)[4][4], std::initializer_list<std::pair<int, float>> hits, int v, float strum) {
    for (int bar = 0; bar < 4; bar++)
      for (auto h : hits)
        for (int i = 0; i < 4; i++) k.push_back({swung(16 * bar + h.first, 200 + 8 * bar + h.first) + i * strum, h.second, v, c[bar][i], i == 3 ? 0.75f : 1.0f});
  };
  auto line = [&](std::vector<Note> &to, std::initializer_list<std::array<float, 3>> notes, int v, int seed) {
    for (auto &m : notes) to.push_back({swung(m[0], seed++), m[1], v, (int)m[2], 1.0f});
  };
  const std::string name = style->name;
  if (name == "lofi") {
    drum({0, 3, 10}, 4, KICK, 0.9f);
    drum({4, 12}, 4, SNARE, 1.0f);
    drum({15}, 1, GHOST, 0.5f);
    drum({0, 4, 8, 12}, 1, HAT, 0.9f);
    drum({2, 6, 10, 14}, 1, HAT, 0.55f);
    drum({7, 13}, 1, HAT, 0.3f);
    d.push_back({0, 16, VINYL, 0, 1.0f});         // (the record's crackle, under it all)
    const int roots[2] = {38, 31};                 // (D | G)
    for (int bar = 0; bar < 2; bar++) {
      const int o = 16 * bar, r = roots[bar];
      line(b, {{(float)o, 5, (float)r}, {(float)o + 6, 2, (float)r}, {(float)o + 10, 2, (float)r + 7}, {(float)o + 14, 1.6f, (float)r + 12}}, BASSV, 100 + 10 * bar);
    }
    static const int c[4][4] = {{53, 57, 60, 64}, {53, 59, 64, 69}, {53, 57, 60, 64}, {52, 55, 59, 62}};   // Dm9 | G13 | Dm9 | Cmaj9
    chords(c, {{0, 7.4f}, {10, 5.6f}}, EPIANO, 0.12f);
    line(l, {{2, 2, 81}, {4, 3, 84}, {7, 1, 86}, {8, 4, 88}, {12, 2, 86}, {14, 2, 84}, {16, 6, 83}, {22, 2, 79}, {24, 4, 81}, {28, 3.5f, 76}}, KALIMBA, 300);
    line(p, {{0, 2, 81}, {2, 2, 84}, {4, 2, 86}, {6, 2, 89}, {8, 2, 86}, {10, 2, 84}, {12, 2, 88}, {14, 2, 86}}, PLUCKV, 400);
  } else if (name == "house") {
    drum({0, 4, 8, 12}, 4, KICK909, 1.0f);
    drum({4, 12}, 4, CLAP, 1.0f);
    drum({2, 6, 10, 14}, 2, OHAT, 0.9f);
    drum({1, 3, 5, 7, 9, 11, 13, 15}, 1, SHAKER, 0.9f);
    drum({0, 4, 8, 12}, 1, SHAKER, 0.5f);
    const int roots[2] = {45, 41};                 // (A | F)
    for (int bar = 0; bar < 2; bar++) {
      const float o = 16.0f * bar, r = roots[bar];
      line(b, {{o + 2, 1.5f, r}, {o + 5, 1, r}, {o + 8, 1.5f, r + 12}, {o + 10, 1.5f, r + 7}, {o + 14, 1.5f, r}}, SUBPLUCK, 100 + 10 * bar);
    }
    static const int c[4][4] = {{60, 64, 67, 71}, {57, 60, 64, 67}, {59, 62, 64, 67}, {55, 57, 62, 67}};   // Am9 | Fmaj9 | Am11 | F6/9
    chords(c, {{3, 1.5f}, {6, 1.5f}, {10, 2.5f}}, STAB, 0.0f);
    static const int pad[2][4] = {{64, 69, 72, 76}, {65, 69, 72, 76}};   // Am | Fmaj7, held
    for (int bar = 0; bar < 2; bar++)
      for (int i = 0; i < 4; i++) l.push_back({16.0f * bar, 15.0f, SUPERSAW, pad[bar][i], 1.0f});
    line(p, {{0, 2, 81}, {3, 2, 79}, {6, 2, 76}, {8, 2, 74}, {11, 2, 76}, {14, 2, 79}}, MARIMBA, 400);
  } else if (name == "synthwave") {
    drum({0, 8, 10}, 4, KICK909, 1.0f);
    drum({4, 12}, 4, GATEDSNARE, 1.0f);
    drum({0, 2, 4, 6, 8, 10, 12, 14}, 1, HAT, 0.7f);
    const int roots[2] = {33, 29};                 // (A | F)
    for (int bar = 0; bar < 2; bar++)
      for (int e = 0; e < 8; e++) b.push_back({swung(16.0f * bar + 2 * e, 100 + 8 * bar + e), 1.8f, SAWBASS, roots[bar] + (e % 2 ? 12 : 0), 1.0f});
    static const int c[4][4] = {{57, 60, 64, 69}, {57, 60, 65, 69}, {59, 60, 64, 71}, {57, 60, 64, 65}};   // Am | F | Am(add9) | Fmaj7
    chords(c, {{0, 15.5f}}, PADSAW, 0.0f);
    line(l, {{0, 4, 76}, {4, 4, 79}, {8, 6, 81}, {14, 2, 79}, {16, 6, 77}, {22, 2, 76}, {24, 8, 72}}, SAWLEAD, 300);
    const int arp[4] = {69, 72, 76, 81};
    for (int i = 0; i < 16; i++) p.push_back({swung((float)i, 400 + i), 0.9f, ARPSQ, arp[i % 4] + 12, i % 4 ? 0.7f : 1.0f});
  } else if (name == "boombap") {
    drum({0, 7, 10}, 4, BBKICK, 1.0f);
    drum({4, 12}, 4, BBSNARE, 1.0f);
    drum({0, 4, 8, 12}, 1, HAT, 0.85f);
    drum({2, 6, 10, 14}, 1, HAT, 0.5f);
    drum({15}, 1, GHOST, 0.4f);
    const int roots[2] = {43, 39};                 // (G | Eb)
    for (int bar = 0; bar < 2; bar++) {
      const float o = 16.0f * bar, r = roots[bar];
      line(b, {{o, 6, r}, {o + 7, 2, r}, {o + 10, 5, r + (bar ? 2.0f : -2.0f)}}, BASSV, 100 + 10 * bar);
    }
    static const int c[4][4] = {{58, 62, 65, 69}, {58, 62, 63, 67}, {58, 62, 65, 67}, {55, 58, 60, 65}};   // Gm9 | Ebmaj7 | Gm7 | Eb6/9
    chords(c, {{0, 7.5f}, {8, 7.5f}}, ORGAN, 0.0f);
    line(l, {{0, 3, 74}, {3, 3, 77}, {6, 2, 79}, {8, 6, 77}, {14, 2, 74}, {16, 4, 70}, {20, 4, 72}, {24, 8, 67}}, FLUTE, 300);
    line(p, {{0, 3, 82}, {4, 2, 79}, {6, 3, 77}, {10, 2, 74}, {12, 3, 77}}, VIBES, 400);
  } else {                                         // jazz
    drum({0, 4, 6, 8, 12, 14}, 4, RIDE, 1.0f);
    drum({4, 12}, 4, BRUSH, 1.0f);
    drum({0, 4, 8, 12}, 2, FEATHER, 1.0f);
    drum({4, 12}, 1, HAT, 0.35f);
    line(b, {{0, 3.6f, 38}, {4, 3.6f, 41}, {8, 3.6f, 45}, {12, 3.6f, 48}, {16, 3.6f, 43}, {20, 3.6f, 47}, {24, 3.6f, 50}, {28, 3.6f, 49}}, UPRIGHT, 100);   // (Dm7 | G7, walked)
    static const int c[4][4] = {{53, 57, 60, 64}, {53, 59, 64, 69}, {53, 55, 60, 64}, {53, 56, 59, 62}};   // Dm9 | G13 | Dm11 | G7b9
    chords(c, {{0, 3}, {6, 2}}, PIANO, 0.03f);
    line(l, {{0, 2, 74}, {2, 2, 77}, {4, 4, 81}, {8, 2, 79}, {10, 2, 77}, {12, 4, 76}, {16, 4, 71}, {20, 2, 74}, {22, 2, 77}, {24, 6, 76}, {30, 2, 74}}, JAZZGTR, 300);
    line(p, {{0, 3, 77}, {3, 3, 81}, {6, 2, 84}, {8, 4, 79}, {12, 4, 76}}, VIBES, 400);
  }
}

static double hz(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }
static float noise(uint32_t n) {                  // (white, from an index: the same every run)
  n = (n << 13) ^ n;
  return 1.0f - (float)((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffff) / 1073741824.0f;
}
// One note's sound t seconds after it started (it lasts dur seconds; after:
// its release), stereo.
static void voice(const Note &n, int id, double t, double dur, float &l, float &r) {
  const double TAU = 2 * M_PI;
  float s = 0, pan = 0;
  const uint32_t k = id * 104729u + (uint32_t)(t * SR);
  switch (n.voice) {
    case KICK: {
      double ph = 50 * t + 70 / 22.0 * (1 - std::exp(-22 * t));
      s = (float)std::tanh(1.6 * std::sin(TAU * ph) * std::exp(-5.5 * t)) * 0.9f;
      s += 0.12f * (float)std::exp(-250 * t) * noise(k);
      break;
    }
    case SNARE: case GHOST: {
      float nz = 0.5f * (noise(k) + noise(k + 1));   // (a little dark)
      s = (float)(0.5 * nz * std::exp(-11 * t) + 0.35 * std::sin(TAU * 190 * t) * std::exp(-24 * t) + 0.08 * nz * std::exp(-3 * t));
      break;
    }
    case HAT: {
      float nz = noise(k) - noise(k - 1);
      s = (float)(0.12 * nz * std::exp(-60 * t));
      pan = 0.35f;
      break;
    }
    case VINYL: {                                   // (hiss, and now and then a pop)
      const uint32_t i = (uint32_t)(t * SR);
      s = 0.012f * noise(i * 7 + 3);
      for (int back = 0; back < 40; back++) {
        const uint32_t j = i - back;
        if (((j * 2654435761u) >> 20) % 2600 == 0) s += 0.22f * std::exp(-back / 6.0f) * noise(j);
      }
      break;
    }
    case BASSV: {
      const double f = hz(n.midi), env = std::min(1.0, t / 0.008) * (t < dur ? 0.75 + 0.25 * std::exp(-5 * t) : 0.75 * std::exp(-(t - dur) / 0.05));
      s = (float)((std::sin(TAU * f * t) + 0.35 * std::sin(TAU * 2 * f * t) + 0.12 * std::sin(TAU * 3 * f * t)) * 0.62 * env);
      break;
    }
    case EPIANO: {                                  // (FM, a tine; a tremolo across the stereo; a tape wobble)
      const double f = hz(n.midi) * (1 + 0.0022 * std::sin(TAU * 0.6 * t));
      const double env = std::min(1.0, t / 0.004) * std::exp(-0.9 * t) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.18));
      const double idx = 1.4 * std::exp(-5 * t) + 0.2, tine = 0.5 * std::exp(-28 * t);
      const double v = std::sin(TAU * f * t + idx * std::sin(TAU * f * t) + tine * std::sin(TAU * 14 * f * t));
      const double trem = 0.18 * std::sin(TAU * 4.2 * t);
      l += (float)(0.17 * v * env * (1 + trem) * n.gain);
      r += (float)(0.17 * v * env * (1 - trem) * n.gain);
      return;
    }
    case HORN: {                                    // (soft, breathy, a slow vibrato)
      const double vib = t > 0.18 ? 0.007 * std::sin(TAU * 5 * t) * std::min(1.0, (t - 0.18) / 0.3) : 0;
      const double f = hz(n.midi) * (1 + vib), env = std::min(1.0, t / 0.06) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.08));
      const double v = std::sin(TAU * f * t) + 0.32 * std::sin(TAU * 2 * f * t) + 0.12 * std::sin(TAU * 3 * f * t);
      s = (float)(0.3 * v * env + 0.03 * noise(k) * std::exp(-25 * t));
      pan = -0.3f;
      break;
    }
    case PLUCKV: {                                  // (a nylon-ish pluck)
      const double f = hz(n.midi);
      double v = 0;
      for (int h = 1; h <= 8; h++) v += std::sin(TAU * h * f * t) / h * std::exp(-(1.5 + 1.1 * h) * t);
      s = (float)(0.3 * v * std::min(1.0, t / 0.002) * (t < dur + 0.4 ? 1.0 : std::exp(-(t - dur - 0.4) / 0.1)));
      pan = 0.4f;
      break;
    }
    // (house)
    case KICK909: {
      double ph = 48 * t + 110 / 35.0 * (1 - std::exp(-35 * t));
      s = (float)std::tanh(1.3 * std::sin(TAU * ph) * std::exp(-4.5 * t)) * 0.9f + 0.15f * (float)std::exp(-300 * t) * noise(k);
      break;
    }
    case CLAP: {
      float nz = noise(k) - 0.6f * noise(k + 2);
      double e = 0;
      for (double at : {0.0, 0.011, 0.022}) if (t >= at) e += std::exp(-140 * (t - at));
      s = (float)(0.45 * nz * (0.6 * e + 0.5 * std::exp(-13 * t)));
      break;
    }
    case OHAT: {
      s = (float)(0.11 * (noise(k) - noise(k - 1)) * std::exp(-13 * t));
      pan = 0.25f;
      break;
    }
    case SHAKER: {
      s = (float)(0.05 * (noise(k) - noise(k - 1)) * std::min(1.0, t / 0.008) * std::exp(-40 * t));
      pan = -0.3f;
      break;
    }
    case SUBPLUCK: {
      const double f = hz(n.midi), env = std::min(1.0, t / 0.003) * std::exp(-7 * t) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.03));
      s = (float)((std::sin(TAU * f * t) + 0.55 * std::sin(TAU * 2 * f * t) * std::exp(-18 * t) + 0.2 * std::sin(TAU * 3 * f * t) * std::exp(-25 * t)) * 0.75 * env);
      break;
    }
    case STAB: {
      const double f = hz(n.midi), env = std::min(1.0, t / 0.003) * std::exp(-4 * t) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.05));
      double a = 0, c = 0;
      for (int h = 1; h <= 10; h++) {
        const double w = std::exp(-(1.5 + 0.9 * h) * t * 4) / h;
        a += w * std::sin(TAU * h * f * 1.003 * t);
        c += w * std::sin(TAU * h * f * 0.997 * t);
      }
      l += (float)(0.16 * a * env * n.gain);
      r += (float)(0.16 * c * env * n.gain);
      return;
    }
    case SUPERSAW: {                                // (seven saws a note, detuned and each drifting (a chorus), spread wide; slow in, slow out)
      static const double DETUNE[7] = {0, -0.011, 0.011, -0.006, 0.006, -0.0025, 0.0025};
      const double f = hz(n.midi), env = std::min(1.0, t / 0.18) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.35));
      double a = 0, c = 0;
      for (int o = 0; o < 7; o++) {
        const double rate = 0.35 + 0.17 * o, drift = 0.0025;
        const double ph = f * (1 + DETUNE[o]) * t + f * drift / (TAU * rate) * std::sin(TAU * rate * t + 1.3 * o) + 0.37 * o;
        double v = 0;
        for (int h = 1; h <= 14; h++) v += std::sin(TAU * h * ph) / h * std::exp(-0.14 * (h - 1));
        const double left = o == 0 ? 0.5 : o % 2 ? 0.85 : 0.15;
        a += v * left;
        c += v * (1 - left);
      }
      l += (float)(0.06 * a * env * n.gain);
      r += (float)(0.06 * c * env * n.gain);
      return;
    }
    case MARIMBA: {
      const double f = hz(n.midi);
      s = (float)(0.3 * (std::sin(TAU * f * t) * std::exp(-7 * t) + 0.3 * std::sin(TAU * 4 * f * t) * std::exp(-28 * t)) * std::min(1.0, t / 0.002));
      pan = 0.35f;
      break;
    }
    // (synthwave)
    case GATEDSNARE: {
      const double gate = t < 0.22 ? 1.0 : std::exp(-(t - 0.22) / 0.012);
      float nz = 0.5f * (noise(k) + noise(k + 1));
      s = (float)((0.45 * nz * (0.7 * std::exp(-9 * t) + 0.3) + 0.35 * std::sin(TAU * 180 * t) * std::exp(-20 * t)) * gate);
      break;
    }
    case SAWBASS: {
      const double f = hz(n.midi), env = std::min(1.0, t / 0.003) * std::exp(-3 * t) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.02));
      double v = 0;
      for (int h = 1; h <= 12; h++) v += std::sin(TAU * h * f * t) / h * std::exp(-0.28 * (h - 1) * (1 + 6 * t));
      s = (float)(0.5 * v * env);
      break;
    }
    case PADSAW: {                                  // (detuned saws, wide; slow in, slow out)
      const double f = hz(n.midi), env = std::min(1.0, t / 0.35) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.45));
      double a = 0, c = 0;
      for (int h = 1; h <= 7; h++) {
        const double w = std::exp(-0.35 * (h - 1)) / h;
        a += w * (std::sin(TAU * h * f * 1.006 * t) + std::sin(TAU * h * f * 0.999 * t + 0.7));
        c += w * (std::sin(TAU * h * f * 0.994 * t + 1.9) + std::sin(TAU * h * f * 1.001 * t + 2.4));
      }
      l += (float)(0.06 * a * env * n.gain);
      r += (float)(0.06 * c * env * n.gain);
      return;
    }
    case SAWLEAD: {
      const double vib = t > 0.25 ? 0.008 * std::sin(TAU * 5.5 * t) * std::min(1.0, (t - 0.25) / 0.3) : 0;
      const double f = hz(n.midi) * (1 + vib), env = std::min(1.0, t / 0.008) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.08));
      double v = 0;
      for (int h = 1; h <= 10; h++) v += std::sin(TAU * h * f * t) / h * std::exp(-0.18 * (h - 1));
      s = (float)(0.22 * v * env);
      pan = -0.15f;
      break;
    }
    case ARPSQ: {
      const double f = hz(n.midi);
      double v = 0;
      for (int h = 1; h <= 9; h += 2) v += std::sin(TAU * h * f * t) / h;
      s = (float)(0.13 * v * std::exp(-9 * t) * std::min(1.0, t / 0.002));
      pan = 0.4f;
      break;
    }
    // (boom bap)
    case BBKICK: {
      double ph = 44 * t + 85 / 20.0 * (1 - std::exp(-20 * t));
      s = (float)std::tanh(2.0 * std::sin(TAU * ph) * std::exp(-4 * t)) * 0.85f + 0.2f * (float)std::exp(-200 * t) * noise(k);
      break;
    }
    case BBSNARE: {
      float nz = noise(k) - 0.3f * noise(k + 1);
      s = (float)std::tanh(1.5 * (0.6 * nz * std::exp(-15 * t) + 0.45 * std::sin(TAU * 215 * t) * std::exp(-22 * t))) * 0.75f;
      break;
    }
    case ORGAN: {                                   // (drawbars, a slow rotary)
      const double f = hz(n.midi), env = std::min(1.0, t / 0.012) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.04));
      const double v = std::sin(TAU * f * t) + 0.8 * std::sin(TAU * 2 * f * t) + 0.45 * std::sin(TAU * 3 * f * t) + 0.25 * std::sin(TAU * 4 * f * t) + 0.15 * std::sin(TAU * 6 * f * t);
      const double rot = 0.22 * std::sin(TAU * 5.8 * t);
      l += (float)(0.075 * v * env * (1 + rot) * n.gain);
      r += (float)(0.075 * v * env * (1 - rot) * n.gain);
      return;
    }
    case FLUTE: {
      const double vib = t > 0.2 ? 0.006 * std::sin(TAU * 5 * t) : 0;
      const double f = hz(n.midi) * (1 + vib), env = std::min(1.0, t / 0.05) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.07));
      s = (float)((0.32 * std::sin(TAU * f * t) + 0.04 * std::sin(TAU * 2 * f * t)) * env + 0.025 * noise(k) * env * (0.4 + std::exp(-12 * t)));
      pan = -0.25f;
      break;
    }
    case VIBES: {
      const double f = hz(n.midi), trem = 1 + 0.25 * std::sin(TAU * 5.5 * t);
      s = (float)(0.26 * (std::sin(TAU * f * t) * std::exp(-1.6 * t) + 0.22 * std::sin(TAU * 4 * f * t) * std::exp(-7 * t)) * trem * std::min(1.0, t / 0.002));
      pan = 0.35f;
      break;
    }
    // (jazz)
    case RIDE: {
      double v = 0;
      for (double fr : {3180.0, 4870.0, 6110.0, 7330.0, 8890.0}) v += std::sin(TAU * fr * t);
      s = (float)((0.025 * v * std::exp(-2.5 * t) + 0.06 * (noise(k) - noise(k - 1)) * std::exp(-14 * t)));
      pan = 0.3f;
      break;
    }
    case BRUSH: {
      float nz = 0.5f * (noise(k) + noise(k + 3));
      s = (float)(0.14 * nz * std::min(1.0, t / 0.03) * std::exp(-7 * t) + 0.12 * nz * std::exp(-45 * t));
      pan = -0.1f;
      break;
    }
    case FEATHER: {
      s = (float)(0.35 * std::sin(TAU * 58 * t) * std::exp(-9 * t));
      break;
    }
    case UPRIGHT: {
      const double f = hz(n.midi), env = std::min(1.0, t / 0.004) * std::exp(-2.6 * t) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.04));
      s = (float)((std::sin(TAU * f * t) + 0.45 * std::sin(TAU * 2 * f * t) * std::exp(-4 * t) + 0.15 * std::sin(TAU * 3 * f * t) * std::exp(-7 * t)) * 0.7 * env
                  + 0.08 * noise(k) * std::exp(-70 * t));
      break;
    }
    case PIANO: {
      const double f = hz(n.midi), env = std::min(1.0, t / 0.002) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.1));
      double v = 0;
      for (int h = 1; h <= 8; h++) v += std::sin(TAU * h * f * std::sqrt(1 + 0.0004 * h * h) * t) / (h * 0.8 + 0.2) * std::exp(-(0.8 + 0.55 * h) * t);
      s = (float)(0.12 * v * env + 0.02 * noise(k) * std::exp(-90 * t));
      pan = 0.1f * (n.midi - 58) / 6.0f;
      break;
    }
    case KALIMBA: {                                 // (a tine: its overtone inharmonic, soon gone)
      const double f = hz(n.midi), env = std::min(1.0, t / 0.002) * std::exp(-2.2 * t) * (t < dur + 0.6 ? 1.0 : std::exp(-(t - dur - 0.6) / 0.15));
      s = (float)(0.3 * (std::sin(TAU * f * t) + 0.3 * std::sin(TAU * 5.4 * f * t) * std::exp(-16 * t) + 0.12 * std::sin(TAU * 2 * f * t) * std::exp(-5 * t)) * env);
      pan = -0.25f;
      break;
    }
    case JAZZGTR: {                                 // (a clean archtop: warm, no vibrato)
      const double f = hz(n.midi), env = std::min(1.0, t / 0.003) * std::exp(-1.4 * t) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.07));
      static const double w[6] = {1, 0.5, 0.3, 0.15, 0.08, 0.04};
      double v = 0;
      for (int h = 1; h <= 6; h++) v += w[h - 1] * std::sin(TAU * h * f * t) * std::exp(-0.9 * h * t);
      s = (float)(0.24 * v * env + 0.02 * noise(k) * std::exp(-80 * t));
      pan = -0.25f;
      break;
    }
    case SAX: {
      const double vib = t > 0.2 ? 0.008 * std::sin(TAU * 5.4 * t) * std::min(1.0, (t - 0.2) / 0.25) : 0;
      const double f = hz(n.midi) * (1 + vib), env = std::min(1.0, t / 0.04) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.06));
      static const double w[7] = {1, 0.75, 0.55, 0.4, 0.28, 0.18, 0.1};
      double v = 0;
      for (int h = 1; h <= 7; h++) v += w[h - 1] * std::sin(TAU * h * f * t);
      s = (float)(0.16 * v * env + 0.02 * noise(k) * env);
      pan = -0.25f;
      break;
    }
  }
  s *= n.gain;
  l += s * (1 - std::max(0.0f, pan));
  r += s * (1 + std::min(0.0f, pan));
}

// The performer's sample at transport beat p (with the line input's latency:
// heard that much later, as a real one would be).
static void perform(double p, double secPerBeat, float &l, float &r) {
  l = r = 0;
  for (const auto &pl : playing) {
    if (p < pl.from || p >= pl.to + 2) continue;   // (and its tails a little past)
    const double inPart = p - pl.from;
    const auto &notes = parts[pl.part];
    const double len = PART_BEATS[pl.part];
    for (int rep = 0; rep * len <= inPart; rep++) {   // (its bars over and over)
      for (size_t i = 0; i < notes.size(); i++) {
        const Note &n = notes[i];
        const double start = rep * len + n.at / 4.0;
        if (pl.from + start >= pl.to) continue;
        const double t = (inPart - start) * secPerBeat, dur = n.len / 4.0 * secPerBeat;
        if (t < 0 || t > dur + (n.voice == VINYL ? 0 : 1.6)) continue;
        voice(n, (int)(rep * 1000 + i), t, dur, l, r);
        if (n.voice == HORN || n.voice == PLUCKV || n.voice == SAWLEAD || n.voice == ARPSQ || n.voice == FLUTE ||
            n.voice == MARIMBA || n.voice == VIBES || n.voice == SAX || n.voice == KALIMBA || n.voice == JAZZGTR) {   // (an echo, three 16ths on)
          const double te = t - 0.75 * secPerBeat;
          float el = 0, er = 0;
          if (te >= 0) voice(n, (int)(rep * 1000 + i), te, dur, el, er);
          l += 0.18f * er;
          r += 0.3f * el;
        }
      }
    }
  }
}

// ---- Audio: the engine a block at a time, the performer as its input.
static std::vector<int16_t> outAudio;
static void audioBlock() {
  static audio_block_t inL, inR, outL, outR;
  const uint32_t spb = transport.spb;
  const double secPerBeat = spb / SR, lat = inputLatency() / (double)spb;
  double p = transport.running ? transport.beat + (double)transport.sampleInBeat / spb : -1;
  for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
    float l = 0, r = 0;
    if (p >= 0) perform(p + k / (double)spb - lat, secPerBeat, l, r);
    inL.data[k] = (int16_t)std::max(-32767.0f, std::min(32767.0f, l * 9000));
    inR.data[k] = (int16_t)std::max(-32767.0f, std::min(32767.0f, r * 9000));
  }
  memset(&outL, 0, sizeof outL);
  memset(&outR, 0, sizeof outR);
  engine.simIn[0] = &inL;
  engine.simIn[1] = &inR;
  engine.simSetInput(0, &inL);
  engine.simSetInput(1, &inR);
  engine.simOut[0] = &outL;
  engine.simOut[1] = &outR;
  engine.update();
  for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
    outAudio.push_back(outL.data[k]);
    outAudio.push_back(outR.data[k]);
  }
}

// ---- The screen, 30 times a second, with the keys held and the caption.
static FILE *framesOut, *metaOut;
static std::string caption, subCaption;
static int frames = 0;
static void takeFrame() {
  fwrite(board.tft().px, 2, m8::Display::W * m8::Display::H, framesOut);
  fprintf(metaOut, "%d\t%s\t%s\n", board.heldKeys, caption.c_str(), subCaption.c_str());
  frames++;
}

// ---- The script: steps in order, each polled until it's done.
using Step = std::function<bool()>;
static std::vector<Step> script;
static double beatNow() {
  return transport.running ? transport.beat + (double)transport.sampleInBeat / transport.spb : -1;
}
static void wait(double sec) {
  auto from = std::make_shared<double>(-1);
  script.push_back([=] {
    if (*from < 0) *from = nowUs;
    return nowUs - *from >= sec * 1e6;
  });
}
static void untilBeat(double b) { script.push_back([=] { return beatNow() >= b; }); }
static void keys(uint8_t k) { script.push_back([=] { board.heldKeys = k; return true; }); }
static void press(uint8_t k, double hold = 0.09, uint8_t after = 0) {
  keys(k);
  wait(hold);
  keys(after);
  wait(0.06);
}
static void say(const std::string &text, const std::string &sub = "") {
  script.push_back([=] { caption = text; subCaption = sub; return true; });
}
static void play(int part, double fromBeat, double beats) {
  script.push_back([=] { playing.push_back({part, fromBeat, fromBeat + beats}); return true; });
}

// To row and cell (ui.h: rows by index, the status row's cells by number), an
// arrow tap at a time (never past an edge: that would open the menu).
static void go(int row, int cell) {
  auto st = std::make_shared<std::pair<int, double>>(0, 0.0);   // (phase: 0 idle, 1 pressed; when)
  script.push_back([=] {
    if (st->first == 1) {
      if (nowUs - st->second < 0.08e6) return false;
      board.heldKeys = 0;
      st->first = 0;
      st->second = nowUs;
      return false;
    }
    if (nowUs - st->second < 0.07e6) return false;
    uint8_t k = 0;
    if (selected != row) k = rowPos(selected) > rowPos(row) ? m8::BTN_UP : m8::BTN_DOWN;
    else if (selectedCellIn(row) != cell) k = selectedCellIn(row) > cell ? m8::BTN_LEFT : m8::BTN_RIGHT;
    if (!k) return true;
    board.heldKeys = k;
    st->first = 1;
    st->second = nowUs;
    return false;
  });
}

using namespace m8;
// The menu's selection to its list item `item` (an arrow at a time).
static void goMenuItem(int item) {
  auto st = std::make_shared<std::pair<int, double>>(0, 0.0);
  script.push_back([=] {
    if (st->first == 1) {
      if (nowUs - st->second < 0.08e6) return false;
      board.heldKeys = 0;
      st->first = 0;
      st->second = nowUs;
      return false;
    }
    if (nowUs - st->second < 0.1e6) return false;
    if (menuSlot() < 0 && menuListItem() == item) return true;
    board.heldKeys = menuSlot() >= 0 || menuListItem() < item ? m8::BTN_DOWN : m8::BTN_UP;
    st->first = 1;
    st->second = nowUs;
    return false;
  });
}
// B held on a track's volume cell, UP tapped until its volume is `step` (of
// VOLUME_MAX: 18 is 90 %).
static void volumeTo(int t, int step) {
  keys(BTN_OPT);
  wait(0.2);
  auto st = std::make_shared<std::pair<int, double>>(0, 0.0);
  script.push_back([=] {
    if (st->first == 1) {
      if (nowUs - st->second < 0.09e6) return false;
      board.heldKeys = BTN_OPT;
      st->first = 0;
      st->second = nowUs;
      return false;
    }
    if (nowUs - st->second < 0.22e6) return false;
    if (volumeStepOf(tracks[t].volume, headroomDbOf(tracks[t].overview)) >= step) return true;
    board.heldKeys = BTN_OPT | BTN_UP;
    st->first = 1;
    st->second = nowUs;
    return false;
  });
  wait(0.6);
  keys(0);
}

static void writeScript() {
  const uint8_t A = BTN_EDIT, B = BTN_OPT, SH = BTN_SHIFT, UP = BTN_UP, DN = BTN_DOWN, LT = BTN_LEFT, RT = BTN_RIGHT, ST = BTN_PLAY;
  wait(0.4);
  say("Pick a project", "12 slots, each with its own scenes and tempo");
  wait(1.1);
  press(A);                                         // a new project, into the looper
  wait(0.5);
  // Drums: the clock starts with the recording, on bar 1.
  say("Record a loop", "A on an empty track: the clock starts, recording on bar 1");
  play(DRUMS, 0, 4);
  press(A);
  untilBeat(2);
  say("Record a loop", "A again: it ends on the bar line and plays on, no gap");
  press(A);                                         // (one bar; on to the next free track)
  const double g = style->bpm > 105 ? 4 : 0;        // (a faster beat: a bar more between the parts, the same time to take each in)
  untilBeat(9 + g);
  say("Layer it", (std::string(style->bass) + ", recorded from the next bar line"));
  play(BASS, 12 + g, 8);
  untilBeat(11 + g);
  press(A);
  untilBeat(18 + g);
  press(A);
  untilBeat(21 + 2 * g);
  say("Layer it", (std::string(style->keys) + "..."));
  play(KEYS, 24 + 2 * g, 16);
  untilBeat(23 + 2 * g);
  press(A);
  untilBeat(38 + 2 * g);
  press(A);
  untilBeat(40.5 + 2 * g);
  // The keys up a little.
  say("Volume", "B + UP on track 3's volume cell: the chords up to 90%");
  go(NUM_TRACKS - 3, MIX_CELL);                     // (track 3's volume)
  volumeTo(NUM_TRACKS - 3, 18);
  wait(0.4);
  go(NUM_TRACKS - 4, WAVE_CELL);                    // (track 4, the next free one)
  const double v = 4;                               // (a bar for it)
  untilBeat(41 + 3 * g + v);
  say("Layer it", ("...and " + std::string(style->lead)));
  play(LEAD, 44 + 3 * g + v, 8);
  untilBeat(43 + 3 * g + v);
  press(A);
  untilBeat(50 + 3 * g + v);
  press(A);
  untilBeat(53 + 3 * g + v);
  say("All four parts", "loops of 1, 2 and 4 bars, each on its own track, in time");
  untilBeat(57 + 4 * g + v);
  // The filter on the keys.
  say("Filter", "B + arrows: a lowpass swept down on the chords...");
  go(NUM_TRACKS - 3, FILTER_CELL);                  // (track 3's filter)
  keys(B); wait(0.2);
  keys(B | LT); wait(1.4);
  keys(B); wait(2.2);
  say("Filter", "...and opened up again");
  keys(B | RT); wait(1.4);
  keys(B); wait(0.4);
  keys(0); wait(1.6);
  // The drums' range: a beat of them, then the whole loop again.
  say("Loop range", "B + DOWN on the waveform: the drums cut to one beat");
  go(NUM_TRACKS - 1, WAVE_CELL);                    // (track 1's waveform)
  keys(B); wait(0.15);
  for (int i = 0; i < 4; i++) { keys(B | DN); wait(0.09); keys(B); wait(0.12); }
  wait(2.6);
  keys(0); wait(0.25);
  say("Loop range", "B twice: the whole loop back");
  press(B, 0.06); press(B, 0.06);
  wait(2.2);
  // Mutes: a swipe.
  say("Mute", "A mutes; held, UP / DOWN swipe it across the tracks");
  go(NUM_TRACKS - 4, WAVE_CELL);                    // (track 4)
  keys(A); wait(0.3);
  keys(A | DN); wait(0.1); keys(A); wait(0.3);
  keys(A | DN); wait(0.1); keys(A); wait(0.3);
  keys(0); wait(2.4);
  // Scenes.
  say("Scenes", "SHIFT + B twice: a copy of the scene to change");
  keys(SH); wait(0.1);
  keys(SH | B); wait(0.06); keys(SH); wait(0.08);
  keys(SH | B); wait(0.06); keys(SH); wait(0.15);
  keys(0); wait(0.8);
  say("Scenes", "the copy with everything back in");
  keys(A); wait(0.3);
  keys(A | UP); wait(0.1); keys(A); wait(0.3);
  keys(A | UP); wait(0.1); keys(A); wait(0.2);
  keys(0); wait(3.0);
  say("Scenes", "SHIFT + UP / DOWN: back and forth, on the next beat");
  keys(SH); wait(0.1); keys(SH | DN); wait(0.09); keys(SH); wait(0.6); keys(0); wait(3.2);
  keys(SH); wait(0.1); keys(SH | UP); wait(0.09); keys(SH); wait(0.6); keys(0); wait(2.4);
  // Resampling into a new scene.
  say("Resample", "B + A on an empty track: what you hear, recorded");
  go(NUM_TRACKS - 5, WAVE_CELL);                    // (track 5: empty)
  keys(B); wait(0.12); keys(B | A); wait(0.1); keys(B); wait(0.06); keys(0);
  script.push_back([] { return recording.mode == Recording::Running; });
  wait(3.6);
  press(A);                                         // (stops at the end of that bar)
  script.push_back([] { return recording.mode == Recording::Idle; });
  say("Resample", "...into a scene of its own, on track 1");
  wait(2.6);
  // A new layer over it.
  say("Keep going", (std::string(style->pluck) + " over the resampling"));
  script.push_back([] { return beatNow() - std::floor(beatNow() / 4) * 4 >= 2.8; });   // (late in a bar: from the next one)
  auto from = std::make_shared<double>(0);
  script.push_back([=] {
    *from = std::floor(beatNow() / 4) * 4 + 4;
    playing.push_back({PLUCK, *from, *from + 4});
    return true;
  });
  press(A);
  script.push_back([=] { return beatNow() >= *from + 2; });
  press(A);                                         // (one bar)
  script.push_back([] { return recording.mode == Recording::Idle; });
  wait(3.0);
  // Export (stopped first: it stays stopped).
  say("Export to Live", "the project as an Ableton Live Set, every scene and clip");
  press(ST);
  wait(0.8);
  go(STATUS_ROW, SCENE_CELL);
  keys(LT); wait(0.1); keys(0);                     // (off the row's first cell: the menu)
  script.push_back([] { return menuOpen(); });
  wait(0.4);
  goMenuItem(MENU_EXPORT);
  wait(0.4);
  press(A);
  wait(2.1);
  keys(RT); wait(0.1); keys(0);                     // (back to the looper)
  wait(1.4);
  say("L7", "record, layer, resample. On the M8.");
  wait(1.6);
  // (the end: the last step done; compose.py ends it with the splash, reversed)
}

int main(int argc, char **argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  if (argc > 2)                                     // (the style: its name)
    for (const auto &st : STYLES)
      if (!strcmp(st.name, argv[2])) style = &st;
  simRoot = out + "/card";
  std::string cmd = "rm -rf '" + simRoot + "' && mkdir -p '" + simRoot + "'";
  if (system(cmd.c_str()) != 0) return 1;
  {                                                 // (the tempo last set by hand: 85, the new project's)
    std::string data = simRoot + "/L7/DATA";
    std::string mk = "mkdir -p '" + data + "'";
    if (system(mk.c_str()) != 0) return 1;
    FILE *f = fopen((data + "/SESSION.DAT").c_str(), "w");
    fprintf(f, "bpm=%.2f\nnewbpm=%.2f\n", style->bpm, style->bpm);
    fclose(f);
  }
  framesOut = fopen((out + "/frames.raw").c_str(), "wb");
  metaOut = fopen((out + "/frames.txt").c_str(), "w");
  FILE *log = freopen((out + "/serial.log").c_str(), "w", stdout);
  (void)log;
  writeParts();
  writeScript();
  caption = "L7";
  subCaption = "a live looper for the Dirtywave M8";
  setup();
  splashing = false;
  {                                                 // (the splash's frames: compose.py plays them backwards at the end)
    FILE *f = fopen((out + "/splash.txt").c_str(), "w");
    fprintf(f, "%d\n", frames);
    fclose(f);
  }
  board.heldKeys = 0;
  size_t step = 0;
  const double END_US = 130e6;                      // (a ceiling)
  while (nowUs < END_US && step < script.size()) {
    while (step < script.size() && script[step]()) step++;
    loop();
    advance(250);
  }
  fclose(framesOut);
  fclose(metaOut);
  // The output: 16-bit stereo WAV.
  FILE *w = fopen((out + "/audio.wav").c_str(), "wb");
  const uint32_t bytes = outAudio.size() * 2;
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, w); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, w); };
  fwrite("RIFF", 1, 4, w); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, w);
  u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
  fwrite("data", 1, 4, w); u32(bytes);
  fwrite(outAudio.data(), 2, outAudio.size(), w);
  fclose(w);
  fprintf(stderr, "demo: %d frames (%.1f s), %.1f s of audio\n", frames, frames / 30.0, outAudio.size() / 2 / SR);
  return 0;
}
