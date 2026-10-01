// looper_sim.cpp — runs the real engine.cpp + storage.cpp on a desktop with a
// fake audio interrupt and a file-backed SD card, and checks recording length
// snapping, latency compensation, sync to the beat grid, tempo changes and
// MIDI clock.
//
//   cd tools/looper_sim
//   c++ -std=c++17 -O1 -Istubs -I../.. ../../engine.cpp ../../storage.cpp looper_sim.cpp -o looper_sim
//   ./looper_sim
#include <cmath>
#include <vector>
#include "Audio.h"
#include "SD.h"
#include "engine.h"
#include "storage.h"

using namespace looper;

uint32_t simMicros = 0;
std::string simRoot;
SDClass SD;
SerialStub Serial;
int simFailIo = 0, simReinits = 0;
void (*IntervalTimer::fn)() = nullptr;
HardwareSerial midi;

static LooperEngine engine;
static audio_block_t inL, inR, outL, outR;
static uint32_t inputIndex = 0;        // index of the next input sample (== transport.now)
static bool feedRamp = true;
static int failures = 0;

// Input that identifies its own sample index: left = index mod 20000 - 10000.
static int16_t rampAt(uint32_t idx) { return (int16_t)((idx % 20000) - 10000); }

static void check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) failures++;
}

// One audio interrupt + whatever loop() would do in between.
static void block() {
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    int16_t v = feedRamp ? rampAt(inputIndex + i) : 0;
    inL.data[i] = v;
    inR.data[i] = -v;
  }
  engine.simIn[0] = &inL; engine.simIn[1] = &inR;
  engine.simSetInput(0, &inL); engine.simSetInput(1, &inR);
  engine.simOut[0] = &outL; engine.simOut[1] = &outR;
  engine.update();
  inputIndex += AUDIO_BLOCK_SAMPLES;
  simMicros += (uint32_t)(AUDIO_BLOCK_SAMPLES * 1e6 / AUDIO_SAMPLE_RATE_EXACT);
  for (int k = 0; k < 15; k++) { simMicros += 1; if (IntervalTimer::fn) IntervalTimer::fn(); }
  serviceStorage();
}

static void runBeats(double beats) {
  uint32_t n = (uint32_t)(beats * transport.spb / AUDIO_BLOCK_SAMPLES);
  for (uint32_t i = 0; i < n; i++) block();
}

static void runUntilIdle() {
  for (int i = 0; i < 100000 && recording.mode != Recording::Idle; i++) block();
}

// Reads frame `frame` of track t's WAV (left channel).
static int16_t wavLeft(int t, uint32_t frame) {
  char path[64];
  snprintf(path, sizeof path, "%s/looper/track%d.wav", simRoot.c_str(), t + 1);
  FILE *f = fopen(path, "rb");
  if (!f) return INT16_MIN;
  int16_t s[2] = {INT16_MIN, 0};
  fseek(f, WAV_DATA_OFFSET + frame * 4, SEEK_SET);
  fread(s, 2, 2, f);
  fclose(f);
  return s[0];
}

// Records track t for `beats` beats (from the moment of the call) and checks
// the snapped length and that frame 0 holds the input from the bar line + latency.
static void recordTrack(int t, double beats, uint32_t expectBars, const char *label) {
  printf("%s\n", label);
  feedRamp = true;
  recordPress(t);
  for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
  uint32_t start = recording.startSample;
  runBeats(beats);
  recordPress(t);
  runUntilIdle();
  const Track &tr = tracks[t];
  char msg[128];
  snprintf(msg, sizeof msg, "loop is %u bars (expected %u), %u frames recorded",
           tr.loopBars, expectBars, tr.recordedFrames);
  check(tr.loopBars == expectBars && tr.state == TrackState::Playing, msg);
  int16_t first = wavLeft(t, 0), expect = rampAt(start);
  snprintf(msg, sizeof msg, "frame 0 = input sample %u (bar line + %u latency): %d vs %d",
           start, LATENCY_FRAMES, first, expect);
  check(first == expect, msg);
  int16_t later = wavLeft(t, 5000);
  check(later == rampAt(start + 5000), "frame 5000 continues the input with no gaps");
}

// Checks the output at the next few beats against the track's recording.
static void checkSync(int t, const char *label) {
  feedRamp = false;
  engine.monitor = false;
  for (auto &tr : tracks) tr.gain = 0;
  tracks[t].gain = 1;
  const Track &tr = tracks[t];
  int bad = 0, checked = 0;
  for (int b = 0; b < 12; b++) {
    // Run until the block that starts a beat, then compare that beat's first sample.
    int32_t target = transport.beat + 1;
    for (int guard = 0; guard < 2000; guard++) {
      uint32_t sib = transport.sampleInBeat, spb = transport.spb;
      if (transport.beat == target - 1 && sib + AUDIO_BLOCK_SAMPLES > spb) {
        uint32_t offset = spb - sib;               // beat starts at this index in the block
        block();
        if (!tr.live || offset >= AUDIO_BLOCK_SAMPLES) break;
        int32_t phase = ((target - tr.anchorBeat) % (int32_t)(tr.rangeBars * BEATS_PER_BAR) +
                         tr.rangeBars * BEATS_PER_BAR) % (tr.rangeBars * BEATS_PER_BAR);
        uint32_t frame = tr.rangeStartFrame() + phase * tr.spbNative;
        float expect = frame < tr.recordedFrames ? wavLeft(t, frame) * engine.masterGain : 0;
        checked++;
        if (fabsf(outL.data[offset] - expect) > 2) {
          if (bad++ < 3) printf("        beat %d: out %d, expected %.0f (frame %u)\n", target, outL.data[offset], expect, frame);
        }
        break;
      }
      block();
    }
  }
  char msg[128];
  snprintf(msg, sizeof msg, "%s: %d/%d beats start on the right frame, %u dropouts", label,
           checked - bad, checked, tr.underruns);
  check(bad == 0 && checked > 8 && tr.underruns == 0, msg);
  for (auto &x : tracks) x.gain = 1;
  engine.monitor = true;
}

// Random session: every few beats press something, like a user would. Looks
// for hangs (a watchdog alarm aborts the run) and for slow loop() passes.
#include <csignal>
#include <chrono>
static const char *lastAction = "start";
static void onAlarm(int) { printf("  FAIL hang: no progress for 10 s after '%s'\n", lastAction); fflush(stdout); _exit(2); }

static int fuzz(int seconds, unsigned seed) {
  srand(seed);
  signal(SIGALRM, onAlarm);
  uint64_t blocks = (uint64_t)(seconds * AUDIO_SAMPLE_RATE_EXACT / AUDIO_BLOCK_SAMPLES);
  double worstMs = 0;
  const char *worstAfter = "";
  int sel = 0;
  float bpm = 120;
  for (uint64_t i = 0; i < blocks; i++) {
    if (i % 2000 == 0) alarm(10);
    if (rand() % 300 == 0) {
      sel = rand() % NUM_TRACKS;
      Track &tr = tracks[sel];
      switch (rand() % 9) {
        case 0: case 1: lastAction = "record press"; recordPress(sel); break;
        case 2: lastAction = "clear"; clearTrack(sel); break;
        case 3: lastAction = "mute"; if (tr.state != TrackState::Empty) tr.state = tr.state == TrackState::Playing ? TrackState::Muted : TrackState::Playing; break;
        case 4: { lastAction = "tempo"; float b = bpm + (rand() % 21 - 10);
                  uint32_t spb = spbForBpm(b); bool ok = recording.mode == Recording::Idle && b >= MIN_BPM && b <= MAX_BPM;
                  for (auto &x : tracks) if (x.state != TrackState::Empty) { double sp = (double)x.spbNative / spb; if (sp < MIN_SPEED || sp > MAX_SPEED) ok = false; }
                  if (ok) { bpm = b; transport.pendingSpb = spb; } break; }
        case 5: lastAction = "range"; if (tr.state != TrackState::Empty) { uint32_t s0 = rand() % tr.loopBars; setRange(sel, s0, 1 + rand() % (tr.loopBars - s0)); } break;
        case 6: lastAction = "transport"; if (transport.running) transportStop(); else transportStart(); break;
        default: lastAction = "nothing"; break;
      }
    }
    feedRamp = true;
    auto t0 = std::chrono::steady_clock::now();
    block();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > worstMs) { worstMs = ms; worstAfter = lastAction; }
  }
  alarm(0);
  uint32_t drops = 0;
  for (auto &tr : tracks) drops += tr.underruns;
  printf("  ok    fuzz seed %u: %d s simulated, slowest block+loop %.1f ms (after '%s'), dropouts %u\n",
         seed, seconds, worstMs, worstAfter, drops);
  return 0;
}

int main(int argc, char **argv) {
  simRoot = "/tmp/looper_sim_sd";
  system(("rm -rf " + simRoot + " && mkdir -p " + simRoot).c_str());
  storageBegin();
  transport.spb = spbForBpm(120);
  midiBegin(&midi);

  recordTrack(0, 4 * 4 + 0.5, 4, "track 1: record 4 bars + half a beat from stopped (expect 4 bars)");
  recordTrack(1, 2 * 4 + 2, 4, "track 2: record 2 bars + 2 beats (expect 4 bars)");
  recordTrack(2, 3, 1, "track 3: record 3 beats (expect 1 bar)");

  runBeats(4);
  checkSync(0, "track 1 at 120 BPM");
  checkSync(1, "track 2 at 120 BPM");

  printf("tempo change to 90 BPM\n");
  transport.pendingSpb = spbForBpm(90);
  runBeats(3);
  checkSync(0, "track 1 at 90 BPM");
  checkSync(2, "track 3 at 90 BPM");

  printf("loop range of track 1: bars 2-3\n");
  setRange(0, 1, 2);
  runBeats(4);
  checkSync(0, "track 1 range 2-3");

  printf("SD card stops responding while recording track 4 (others playing)\n");
  {
    feedRamp = true;
    recordPress(3);
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    uint32_t start = recording.startSample;
    runBeats(2);
    simFailIo = 6;                                 // next 6 transfers fail
    int reinitsBefore = simReinits;
    runBeats(6);
    recordPress(3);
    runUntilIdle();
    const Track &tr = tracks[3];
    char m[128];
    snprintf(m, sizeof m, "card re-initialised %d time(s), recording finished as %u bars, overruns %u",
             simReinits - reinitsBefore, tr.loopBars, recording.overruns);
    check(simReinits > reinitsBefore && tr.loopBars == 2 && recording.overruns == 0, m);
    bool continuous = true;
    for (uint32_t f = 0; f < tr.recordedFrames; f += 997)
      if (wavLeft(3, f) != rampAt(start + f)) { continuous = false; printf("        frame %u wrong\n", f); break; }
    check(continuous, "recording has no gap across the failure");
    runBeats(4);
    for (auto &x : tracks) x.underruns = 0;
    checkSync(3, "track 4 after recovery");
    checkSync(0, "track 1 after recovery");
  }

  printf("MIDI clock\n");
  if (!MIDI_OUT_ENABLED) {
    printf("  (MIDI out disabled in config.h, skipped)\n");
  } else {
  uint32_t t0 = midi.ticks;
  int32_t b0 = transport.beat;
  runBeats(8);
  // Let queued ticks go out.
  for (int k = 0; k < 400; k++) { simMicros += 50; IntervalTimer::fn(); }
  int32_t beats = transport.beat - b0;
  char msg[96];
  snprintf(msg, sizeof msg, "%u ticks over %d beats (24 per beat)", midi.ticks - t0, beats);
  check(abs((int)(midi.ticks - t0) - beats * 24) <= 24, msg);
  }

  printf("random sessions\n");
  for (unsigned seed = 1; seed <= (argc > 1 ? atoi(argv[1]) : 5); seed++) fuzz(600, seed);

  printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
