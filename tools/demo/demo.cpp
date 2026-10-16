// demo.cpp — renders a demo video of L7: the real app (the sketch's setup()
// and loop(), the engine, the UI) run headless in virtual time, its keys
// pressed by a script, a "performer" playing synthesized parts (drums, bass,
// chords, a lead) into its line input in time with its clock. Writes the
// screen 30 times a second (raw RGB565, with the keys held and the caption)
// and the output audio (WAV); compose.py turns them into the video.
//
//   ./build.sh            (builds, runs, composes: tools/demo/L7 demo.mp4)
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

// ---- The performer: a lo-fi hip hop beat (85 BPM, swung), its parts as
// notes over one, two or four bars (start and length in 16ths; the odd 16ths
// played late: the swing), played into the line input over the beats the
// script gives it, in time with the clock. The loops: drums 1 bar, bass 2,
// keys 4 (Dm9 | G13 | Dm9 | Cmaj9 over the bass's D | G), horn 2, pluck 1.
enum Part { DRUMS, BASS, KEYS, LEAD, PLUCK, PARTS };
enum Voice { KICK, SNARE, GHOST, HAT, VINYL, BASSV, EPIANO, HORN, PLUCKV };
struct Note { float at, len; int voice, midi; float gain; };
static std::vector<Note> parts[PARTS];
static const int PART_BEATS[PARTS] = {4, 8, 16, 8, 4};
struct Playing { int part; double from, to; };   // (transport beats)
static std::vector<Playing> playing;
static constexpr float SWING = 0.28f;            // (of a 16th: the odd ones that late)

static float hum(int i) { return ((i * 2654435761u) >> 24) / 255.0f - 0.5f; }   // (a nudge: -0.5..0.5)
static float swung(float s16, int i) { return s16 + (((int)s16) % 2 ? SWING : 0) + 0.06f * hum(i); }

static void writeParts() {
  auto &d = parts[DRUMS];                          // (one bar)
  int n = 0;
  for (int st : {0, 3, 10}) d.push_back({swung(st, n++), 4, KICK, 0, st ? 0.85f : 1.0f});
  for (int st : {4, 12}) d.push_back({swung(st, n++), 4, SNARE, 0, 1.0f});
  d.push_back({swung(15, n++), 1, GHOST, 0, 0.5f});
  for (int st = 0; st < 16; st += 2) d.push_back({swung(st, n++), 1, HAT, 0, st % 4 ? 0.55f : 0.9f});
  for (int st : {7, 13}) d.push_back({swung(st, n++), 1, HAT, 0, 0.3f});
  d.push_back({0, 16, VINYL, 0, 1.0f});           // (the record's crackle, under it all)
  auto &b = parts[BASS];                           // (two bars: D | G)
  const int roots[2] = {38, 31};
  for (int bar = 0; bar < 2; bar++) {
    const int o = 16 * bar, r = roots[bar];
    b.push_back({swung(o, 100 + bar), 5, BASSV, r, 1.0f});
    b.push_back({swung(o + 6, 110 + bar), 2, BASSV, r, 0.75f});
    b.push_back({swung(o + 10, 120 + bar), 2, BASSV, r + 7, 0.7f});
    b.push_back({swung(o + 14, 130 + bar), 1.6f, BASSV, r + 12, 0.6f});
  }
  auto &k = parts[KEYS];                           // (four bars)
  const int chords[4][4] = {{53, 57, 60, 64}, {53, 59, 64, 69}, {53, 57, 60, 64}, {52, 55, 59, 62}};
  for (int bar = 0; bar < 4; bar++)
    for (int hit = 0; hit < 2; hit++)
      for (int v = 0; v < 4; v++)
        k.push_back({swung(16 * bar + (hit ? 10 : 0), 200 + 2 * bar + hit) + v * 0.12f, hit ? 5.6f : 7.4f, EPIANO, chords[bar][v],
                     (v == 3 ? 0.7f : 1.0f) * (hit ? 0.75f : 1.0f)});
  auto &l = parts[LEAD];                            // (two bars: a horn line)
  const float mel[][3] = {{2, 2, 69}, {4, 3, 72}, {7, 1, 74}, {8, 4, 76}, {12, 2, 74}, {14, 2, 72},
                          {16, 6, 71}, {22, 2, 67}, {24, 4, 69}, {28, 3.5f, 64}};
  for (int i = 0; i < 10; i++) l.push_back({swung(mel[i][0], 300 + i), mel[i][1], HORN, (int)mel[i][2], 1.0f});
  auto &p = parts[PLUCK];                           // (one bar: A C D F, over all of it)
  const int arp[8] = {69, 72, 74, 77, 74, 72, 76, 74};
  for (int i = 0; i < 8; i++) p.push_back({swung(2 * i, 400 + i), 2, PLUCKV, arp[i] + 12, i % 2 ? 0.75f : 1.0f});
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
        if (n.voice == HORN || n.voice == PLUCKV) {   // (an echo, three 16ths on)
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
static void say(const char *text, const char *sub = "") {
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
  untilBeat(9);
  say("Layer it", "a 2-bar bassline, recorded from the next bar line");
  play(BASS, 12, 8);
  untilBeat(11);
  press(A);
  untilBeat(18);
  press(A);
  untilBeat(21);
  say("Layer it", "4 bars of electric piano...");
  play(KEYS, 24, 16);
  untilBeat(23);
  press(A);
  untilBeat(38);
  press(A);
  untilBeat(41);
  say("Layer it", "...and a 2-bar horn line");
  play(LEAD, 44, 8);
  untilBeat(43);
  press(A);
  untilBeat(50);
  press(A);
  untilBeat(53);
  say("All four parts", "loops of 1, 2 and 4 bars, each on its own track, in time");
  untilBeat(57);
  // The filter on the keys.
  say("Filter", "B + arrows: a lowpass swept down on the keys...");
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
  say("Keep going", "a 1-bar pluck over the resampling");
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
  // Export.
  say("Export to Live", "the project as an Ableton Live Set, every scene and clip");
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
  wait(2.4);
  press(ST);                                        // (stopped)
  wait(2.0);
  // (the end: the last step done)
}

int main(int argc, char **argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  simRoot = out + "/card";
  std::string cmd = "rm -rf '" + simRoot + "' && mkdir -p '" + simRoot + "'";
  if (system(cmd.c_str()) != 0) return 1;
  {                                                 // (the tempo last set by hand: 85, the new project's)
    std::string data = simRoot + "/L7/DATA";
    std::string mk = "mkdir -p '" + data + "'";
    if (system(mk.c_str()) != 0) return 1;
    FILE *f = fopen((data + "/SESSION.DAT").c_str(), "w");
    fputs("bpm=85.00\nnewbpm=85.00\n", f);
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
