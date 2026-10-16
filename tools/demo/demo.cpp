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

// ---- The performer: parts in A minor at the clock's tempo, each a list of
// notes over two bars (start and length in 16ths), played into the line
// input over the bars the script gives it.
enum Part { DRUMS, BASS, CHORDS, LEAD, PARTS };
enum Voice { KICK, SNARE, HAT, OPENHAT, BASSV, PAD, LEADV };
struct Note { int at, len, voice, midi; float gain; };
static std::vector<Note> parts[PARTS];
struct Playing { int part; double from, to; };   // (transport beats)
static std::vector<Playing> playing;

static void writeParts() {
  auto &d = parts[DRUMS];
  for (int bar = 0; bar < 2; bar++) {
    const int o = bar * 16;
    for (int s : {0, 7, 10}) d.push_back({o + s, 4, KICK, 0, 1.0f});
    if (bar == 1) d.push_back({o + 13, 2, KICK, 0, 0.7f});
    for (int s : {4, 12}) d.push_back({o + s, 4, SNARE, 0, 1.0f});
    for (int s = 0; s < 16; s += 2) d.push_back({o + s, 1, s == 14 && bar == 1 ? OPENHAT : HAT, 0, s % 4 ? 0.7f : 1.0f});
    for (int s : {3, 11}) d.push_back({o + s, 1, HAT, 0, 0.35f});
  }
  auto &b = parts[BASS];                         // A1 / F1 G1
  for (int s : {0, 3, 6, 8, 11, 14}) b.push_back({s, s == 6 || s == 14 ? 2 : 3, BASSV, s == 8 ? 45 : 33, 1.0f});
  for (int s : {0, 3, 6, 8, 11}) b.push_back({16 + s, s == 6 ? 2 : 3, BASSV, 29, 1.0f});
  b.push_back({16 + 14, 2, BASSV, 31, 1.0f});
  auto &c = parts[CHORDS];                       // Am | F (add9)
  for (int n : {57, 60, 64, 71}) c.push_back({0, 15, PAD, n, n == 71 ? 0.5f : 1.0f});
  for (int n : {53, 57, 60, 67}) c.push_back({16, 15, PAD, n, n == 67 ? 0.5f : 1.0f});
  auto &l = parts[LEAD];                         // a hook
  const int mel[][3] = {{0, 3, 76}, {3, 3, 74}, {6, 2, 72}, {8, 4, 69}, {12, 2, 72}, {14, 2, 74},
                        {16, 3, 72}, {19, 3, 69}, {22, 2, 67}, {24, 6, 69}};
  for (auto &m : mel) l.push_back({m[0], m[1], LEADV, m[2], 1.0f});
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
  switch (n.voice) {
    case KICK: {
      double ph = 45 * t + 90 / 28.0 * (1 - std::exp(-28 * t));
      s = (float)(std::sin(TAU * ph) * std::exp(-7 * t) + 0.3 * std::exp(-400 * t) * noise(id * 7919 + (int)(t * SR))) * 0.95f;
      break;
    }
    case SNARE: {
      float nz = noise(id * 104729 + (int)(t * SR)) - 0.5f * noise(id * 104729 + (int)(t * SR) - 1);
      s = (float)(0.55 * nz * std::exp(-16 * t) + 0.45 * std::sin(TAU * 185 * t) * std::exp(-28 * t));
      break;
    }
    case HAT: case OPENHAT: {
      const uint32_t k = id * 15485863 + (int)(t * SR);
      float nz = noise(k) - noise(k - 1);          // (differenced: bright)
      s = (float)(0.22 * nz * std::exp(-(n.voice == HAT ? 55 : 7) * t));
      pan = 0.3f;
      break;
    }
    case BASSV: {
      const double f = hz(n.midi), env = std::min(1.0, t / 0.004) * (t < dur ? 0.8 + 0.2 * std::exp(-6 * t) : 0.8 * std::exp(-(t - dur) / 0.03));
      double v = 0;
      for (int k = 1; k <= 7; k++) v += std::sin(TAU * k * f * t) / k * std::exp(-0.35 * (k - 1) * (1 + 3 * t));
      s = (float)(0.55 * v * env);
      break;
    }
    case PAD: {
      const double f = hz(n.midi), env = std::min(1.0, t / 0.12) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.25));
      double a = 0, b = 0;
      for (int k = 1; k <= 5; k += 2) {          // (triangles, detuned left and right)
        const double w = 1.0 / (k * k) * (k % 4 == 1 ? 1 : -1);
        a += w * std::sin(TAU * k * f * 1.004 * t);
        b += w * std::sin(TAU * k * f * 0.996 * t + 1.3);
      }
      l += (float)(0.16 * a * env * n.gain);
      r += (float)(0.16 * b * env * n.gain);
      return;
    }
    case LEADV: {
      const double vib = t > 0.15 ? 0.006 * std::sin(TAU * 5.5 * t) : 0;
      const double f = hz(n.midi) * (1 + vib), env = std::min(1.0, t / 0.01) * (t < dur ? 1.0 : std::exp(-(t - dur) / 0.06));
      double v = 0;
      for (int k = 1; k <= 9; k += 2) v += std::sin(TAU * k * f * t) / k * std::exp(-0.25 * (k - 1));
      s = (float)(0.28 * v * env);
      pan = -0.25f;
      break;
    }
  }
  s *= n.gain;
  l += s * (1 - std::max(0.0f, pan));
  r += s * (1 + std::min(0.0f, pan));
}

// The performer's sample at transport beat p (with the line input's latency: heard
// that much later, as a real one would be).
static void perform(double p, double secPerBeat, float &l, float &r) {
  l = r = 0;
  for (const auto &pl : playing) {
    if (p < pl.from || p >= pl.to + 2) continue;   // (and its tails a little past)
    const double inPart = p - pl.from;
    const auto &notes = parts[pl.part];
    for (int rep = 0; rep * 8.0 <= inPart; rep++) {   // (the two bars over and over)
      for (size_t i = 0; i < notes.size(); i++) {
        const Note &n = notes[i];
        const double start = rep * 8.0 + n.at / 4.0;
        if (pl.from + start >= pl.to) continue;
        const double t = (inPart - start) * secPerBeat;
        if (t < 0 || t > n.len / 4.0 * secPerBeat + 1.2) continue;
        voice(n, (int)(rep * 100 + i), t, n.len / 4.0 * secPerBeat, l, r);
        if (n.voice == LEADV) {                   // (an echo, three 16ths on)
          const double te = t - 0.75 * secPerBeat;
          float el = 0, er = 0;
          if (te >= 0) voice(n, (int)(rep * 100 + i), te, n.len / 4.0 * secPerBeat, el, er);
          l += 0.15f * er;
          r += 0.35f * el;
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
static void writeScript() {
  const uint8_t A = BTN_EDIT, B = BTN_OPT, SH = BTN_SHIFT, UP = BTN_UP, DN = BTN_DOWN, LT = BTN_LEFT, RT = BTN_RIGHT, ST = BTN_PLAY;
  wait(0.4);
  say("Pick a project", "12 slots, each with its own scenes and tempo");
  wait(1.1);
  press(A);                                         // a new project, into the looper
  wait(0.8);
  // Drums: the clock starts with the recording, on bar 1.
  say("Record a loop", "A on an empty track: the clock starts, recording on bar 1");
  play(DRUMS, 0, 8);
  press(A);
  untilBeat(6);
  say("Record a loop", "A again: it ends on the bar line and plays on, no gap");
  press(A);                                         // (on to the next free track)
  untilBeat(9);
  say("Layer it", "bass on the next track, recorded on the next bar line");
  play(BASS, 12, 8);
  untilBeat(11);
  press(A);
  untilBeat(18);
  press(A);
  untilBeat(19.2);
  say("Layer it", "chords...");
  play(CHORDS, 20, 8);
  press(A);
  untilBeat(26);
  press(A);
  untilBeat(27.2);
  say("Layer it", "...and a lead");
  play(LEAD, 28, 8);
  press(A);
  untilBeat(34);
  press(A);
  untilBeat(36.5);
  // The filter on the drums.
  say("Filter", "B + arrows: a lowpass swept down and back");
  go(NUM_TRACKS - 1, FILTER_CELL);                  // (track 1's filter)
  keys(B); wait(0.15);
  keys(B | LT); wait(1.25);
  keys(B); wait(1.2);
  keys(B | RT); wait(1.25);
  keys(B); wait(0.3);
  keys(0); wait(0.5);
  // The lead's range: a beat of it, then the whole loop again.
  say("Loop range", "B + DOWN: the lead cut to a single beat");
  go(NUM_TRACKS - 4, WAVE_CELL);                    // (track 4's waveform)
  keys(B); wait(0.15);
  for (int i = 0; i < 4; i++) { keys(B | DN); wait(0.09); keys(B); wait(0.12); }
  wait(1.6);
  say("Loop range", "B twice: the whole loop back");
  keys(0); wait(0.25);
  press(B, 0.06); press(B, 0.06);
  wait(1.0);
  // Mutes: a swipe.
  say("Mute", "A mutes; held, UP / DOWN swipe it across the tracks");
  keys(A); wait(0.3);
  keys(A | DN); wait(0.1); keys(A); wait(0.25);
  keys(A | DN); wait(0.1); keys(A); wait(0.25);
  keys(0); wait(1.4);
  // Scenes.
  say("Scenes", "SHIFT + B twice: a copy of the scene to change");
  keys(SH); wait(0.1);
  keys(SH | B); wait(0.06); keys(SH); wait(0.08);
  keys(SH | B); wait(0.06); keys(SH); wait(0.15);
  keys(0); wait(0.4);
  say("Scenes", "the copy with everything back in");
  keys(A); wait(0.25);
  keys(A | UP); wait(0.1); keys(A); wait(0.25);
  keys(A | UP); wait(0.1); keys(A); wait(0.2);
  keys(0); wait(1.6);
  say("Scenes", "SHIFT + UP / DOWN: back and forth, on the next beat");
  keys(SH); wait(0.1); keys(SH | DN); wait(0.09); keys(SH); wait(0.5); keys(0); wait(2.0);
  keys(SH); wait(0.1); keys(SH | UP); wait(0.09); keys(SH); wait(0.5); keys(0); wait(1.6);
  // Resampling into a new scene.
  say("Resample", "B + A on an empty track: what you hear, recorded");
  go(NUM_TRACKS - 5, WAVE_CELL);                    // (track 5: empty)
  keys(B); wait(0.12); keys(B | A); wait(0.1); keys(B); wait(0.06); keys(0);
  script.push_back([] { return recording.mode == Recording::Running; });
  wait(2.6);
  press(A);                                         // (stops at the end of that bar)
  script.push_back([] { return recording.mode == Recording::Idle; });
  say("Resample", "...into a scene of its own, on track 1");
  wait(2.0);
  // The tempo.
  say("Tempo", "faster: every loop stretched to it, pitch kept");
  go(STATUS_ROW, TEMPO_CELL);
  keys(B); wait(0.15);
  keys(B | UP); wait(1.1);
  keys(B); wait(0.3);
  keys(0); wait(3.6);
  say("L7", "record, layer, resample. On the M8.");
  press(ST);                                        // (stopped)
  wait(2.4);
  // (the end: the last step done)
}

int main(int argc, char **argv) {
  const std::string out = argc > 1 ? argv[1] : ".";
  simRoot = out + "/card";
  std::string cmd = "rm -rf '" + simRoot + "' && mkdir -p '" + simRoot + "'";
  if (system(cmd.c_str()) != 0) return 1;
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
  const double END_US = 75e6;                       // (a ceiling)
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
