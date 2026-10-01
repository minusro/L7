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
int simFailIo = 0, simReinits = 0, simFsChanges = 0;
void (*simSdLatency)(size_t, bool) = nullptr;
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

// One audio interrupt only (runs while loop() is blocked in an SD transfer).
static double pendingUs = 0;
static void audioBlock();

// Card timing calibrated on the M8's card logs: reads ~4 ms per 16 KB; of the
// 16 KB writes ~97% are quick (~10 ms), ~2% take ~90 ms and ~1% ~250 ms.
static void realisticSd(size_t bytes, bool write) {
  double ms;
  if (!write) ms = 2.0 + bytes / 10000.0;
  else { int r = rand() % 100; ms = r < 97 ? 10 : r < 99 ? 90 : 250; }
  pendingUs += ms * 1000;
  const double blockUs = AUDIO_BLOCK_SAMPLES * 1e6 / AUDIO_SAMPLE_RATE_EXACT;
  while (pendingUs >= blockUs) { pendingUs -= blockUs; audioBlock(); }
}

// One audio interrupt + whatever loop() would do in between.
static void block() {
  audioBlock();
  serviceStorage();
}

#include <map>
static std::map<std::string, uint32_t> dropStages;   // what loop() was doing when tracks ran dry

static void audioBlock() {
  uint32_t before = 0;
  for (auto &tr : tracks) before += tr.underruns;
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
  uint32_t after = 0;
  for (auto &tr : tracks) after += tr.underruns;
  if (after > before) dropStages[(const char *)debugStage] += after - before;
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

// Records track t, pressing stop after `beats` beats, and checks the loop
// length, that frame 0 holds the input from the bar line + latency, and that
// the audio is continuous up to its last frame (also when recording carried
// on past the stop press to reach the loop length).
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
  uint32_t loopFrames = tr.loopBars * BEATS_PER_BAR * tr.spbNative;
  uint32_t last = tr.recordedFrames - 1;
  snprintf(msg, sizeof msg, "whole loop is recorded audio (%u of %u frames), last frame continuous",
           tr.recordedFrames, loopFrames);
  check(tr.recordedFrames == loopFrames && wavLeft(t, last) == rampAt(start + last), msg);
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
  for (auto &tr : tracks) tr.underruns = 0;
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
  storageBegin([](const char *m) { if (*m) printf("  (%s)\n", m); });
  transport.spb = spbForBpm(120);
  int fsChangesAtStart = simFsChanges;
  midiBegin(&midi);

  recordTrack(0, 4 * 4 + 1.5, 4, "track 1: stop 4 bars + 1.5 beats in, from stopped (trims to 4 bars)");
  recordTrack(1, 2 * 4 + 3, 4, "track 2: stop 2 bars + 3 beats in (carries on to 4 bars)");
  recordTrack(2, 3, 1, "track 3: stop 3 beats in (carries on to 1 bar)");

  runBeats(4);
  checkSync(0, "track 1 at 120 BPM");
  checkSync(1, "track 2 at 120 BPM");

  printf("tempo change to 90 BPM\n");
  transport.pendingSpb = spbForBpm(90);
  runBeats(3);
  checkSync(0, "track 1 at 90 BPM");
  checkSync(2, "track 3 at 90 BPM");

  printf("loop range of track 1 changed to bars 2-3 while playing\n");
  {
    feedRamp = false;
    engine.monitor = false;
    for (auto &x : tracks) x.gain = 0;
    Track &tr = tracks[0];
    tr.gain = 1;
    tr.underruns = 0;
    uint32_t oldStart = tr.rangeStartBar, oldBars = tr.rangeBars;
    int32_t asked = transport.beat;
    setRange(0, 1, 2);
    int32_t sw = tr.switchBeat;
    char m[128];
    snprintf(m, sizeof m, "queued for bar line beat %d (asked at beat %d), keeps playing meanwhile", sw, asked);
    check(tr.rangePending && tr.live && sw % BEATS_PER_BAR == 0 && sw > asked && sw <= asked + 2 * BEATS_PER_BAR, m);
    // Compare the first sample of each beat with old range before the switch, new range after.
    int bad = 0, checked = 0, before = 0;
    for (int b = 0; b < 16; b++) {
      int32_t target = transport.beat + 1;
      for (int guard = 0; guard < 2000; guard++) {
        uint32_t sib = transport.sampleInBeat, spb = transport.spb;
        if (transport.beat == target - 1 && sib + AUDIO_BLOCK_SAMPLES > spb) {
          uint32_t offset = spb - sib;
          block();
          if (offset >= AUDIO_BLOCK_SAMPLES) break;
          bool newRange = target >= sw;
          uint32_t rs = newRange ? 1 : oldStart, rb = newRange ? 2 : oldBars;
          int32_t ph = ((target - tr.anchorBeat) % (int32_t)(rb * BEATS_PER_BAR) + rb * BEATS_PER_BAR) % (rb * BEATS_PER_BAR);
          uint32_t frame = rs * BEATS_PER_BAR * tr.spbNative + ph * tr.spbNative;
          float expect = frame < tr.recordedFrames ? wavLeft(0, frame) * engine.masterGain : 0;
          checked++;
          if (!newRange) before++;
          if (fabsf(outL.data[offset] - expect) > 2 && bad++ < 3)
            printf("        beat %d (%s range): out %d, expected %.0f\n", target, newRange ? "new" : "old",
                   outL.data[offset], expect);
          break;
        }
        block();
      }
    }
    snprintf(m, sizeof m, "%d beats on the old range then %d on the new, all on the right frame; %u dropouts",
             before, checked - before, tr.underruns);
    check(bad == 0 && before > 0 && checked - before > 4 && tr.underruns == 0, m);
    check(!tr.rangePending && tr.rangeStartBar == 1 && tr.rangeBars == 2, "range applied and kept");
    for (auto &x : tracks) x.gain = 1;
    engine.monitor = true;
  }

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
      if (wavLeft(3, f) != rampAt(start + f)) {
        continuous = false;
        printf("        frame %u wrong (recorded %u, written %u)\n", f, tr.recordedFrames, recording.written);
        for (uint32_t g = f - 6; g < f + 4 && g < tr.recordedFrames; g++)
          printf("          frame %u: file %d, input history %d, expected %d\n", g, wavLeft(3, g),
                 (int16_t)(inputRing[(start + g) % INPUT_FRAMES] & 0xFFFF), rampAt(start + g));
        printf("          start %u, now %u\n", start, transport.now);
        break;
      }
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

  {
    char m[96];
    snprintf(m, sizeof m, "no files created, resized or deleted while playing (%d)", simFsChanges - fsChangesAtStart);
    check(simFsChanges == fsChangesAtStart, m);
  }

  printf("power cycle: tracks come back from their WAV headers\n");
  {
    transportStop();
    flushHeaders();
    Track saved[NUM_TRACKS];
    for (int t = 0; t < NUM_TRACKS; t++) {
      saved[t].state = tracks[t].state; saved[t].loopBars = tracks[t].loopBars;
      saved[t].recordedFrames = tracks[t].recordedFrames; saved[t].rangeStartBar = tracks[t].rangeStartBar;
      saved[t].rangeBars = tracks[t].rangeBars; saved[t].spbNative = tracks[t].spbNative;
      tracks[t].state = TrackState::Empty; tracks[t].file = FsFile();
    }
    int before = simFsChanges;
    storageBegin([](const char *) {});
    bool same = true;
    for (int t = 0; t < NUM_TRACKS; t++) {
      const Track &a = saved[t], &b = tracks[t];
      if (a.state != b.state) same = false;
      if (a.state != TrackState::Empty &&
          (a.loopBars != b.loopBars || a.recordedFrames != b.recordedFrames || a.rangeStartBar != b.rangeStartBar ||
           a.rangeBars != b.rangeBars || a.spbNative != b.spbNative)) same = false;
    }
    check(same, "every track's state, length, range and tempo restored");
    check(simFsChanges - before == 8, "second boot only removes the 8 old-style settings files, creates nothing");
  }

  printf("slow card (measurement): 8 tracks playing while track 8 is recorded and cleared again and again\n");
  {
    simSdLatency = realisticSd;
    srand(7);
    if (!transport.running) transportStart();
    for (int t = 0; t < 7; t++)
      if (tracks[t].state == TrackState::Empty) { recordPress(t); runBeats(16.2); recordPress(t); runUntilIdle(); }
    for (float bpm : {120.0f, 150.0f, 180.0f, 220.0f}) {
      // Tempo relative to what the tracks were recorded at (mostly 120, one at 90).
      transport.pendingSpb = spbForBpm(bpm);
      runBeats(4);
      for (auto &tr : tracks) tr.underruns = 0;
      dropStages.clear();
      uint32_t overruns = 0;
      for (int cycle = 0; cycle < 6; cycle++) {
        clearTrack(7);
        runBeats(2);
        recordPress(7);
        runBeats(9 + cycle);
        overruns += recording.overruns;
        recordPress(7);
        runUntilIdle();
        runBeats(4);
      }
      uint32_t drops = 0;
      for (auto &tr : tracks) drops += tr.underruns;
      char m[128];
      snprintf(m, sizeof m, "%.0f BPM (%.2fx): %u dropped samples, %u recording overruns", bpm, bpm / 120.0f,
               drops, overruns);
      // Measured, not pass/fail: with 8 stereo tracks the buffers can't cover
      // the card's rarest stalls at higher speeds (RAM limit, see storage.cpp).
      printf("  info  %s\n", m);
      for (auto &kv : dropStages) printf("        during '%s': %u\n", kv.first.c_str(), kv.second);
      dropStages.clear();
    }
    simSdLatency = nullptr;
    transport.pendingSpb = spbForBpm(120);
    runBeats(2);
  }

  printf("random sessions\n");
  for (unsigned seed = 1; seed <= (argc > 1 ? atoi(argv[1]) : 5); seed++) fuzz(600, seed);

  printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
