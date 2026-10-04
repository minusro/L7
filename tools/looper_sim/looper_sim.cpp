// looper_sim.cpp — runs the real engine.cpp + storage.cpp on a desktop with a
// fake audio interrupt and a file-backed SD card, and checks recording length
// snapping, latency compensation, sync to the beat grid, tempo changes and
// MIDI clock.
//
//   cd tools/looper_sim
//   c++ -std=c++17 -O1 -Istubs -I../.. ../../engine.cpp ../../storage.cpp ../../scenes.cpp looper_sim.cpp -o looper_sim
//   ./looper_sim
#include <cmath>
#include <vector>
#include <algorithm>
#include "Audio.h"
#include "SD.h"
#include "engine.h"
#include "storage.h"
#include "ableton.h"

using namespace looper;

uint32_t simMicros = 0;
void (*simDelayHook)(uint32_t us) = nullptr;
std::string simRoot;
SDClass SD;
SerialStub Serial;
int simFailIo = 0, simReinits = 0, simFsChanges = 0, simFileCreates = 0;
void (*simSdLatency)(size_t, bool) = nullptr;
void (*IntervalTimer::fn)() = nullptr;
HardwareSerial midi;

static LooperEngine engine;
static audio_block_t inL, inR, outL, outR;
static uint32_t inputIndex = 0;        // index of the next input sample (== transport.now)
static bool feedRamp = true;
static double feedSineHz = 0;                       // instead: a sine (declicking tests)
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

#include <deque>
static bool traceOn = false;
static std::deque<std::string> traceLines;
static int simTopped() {
  for (const auto &tr : tracks)
    if ((tr.live || tr.armed) && RING_FRAMES - (tr.head - tr.tail) >= CHUNK_FRAMES / 2) return 0;
  return 1;
}

// Card timing calibrated on the M8's card logs: reads ~4 ms per 16 KB; of the
// 16 KB writes ~97% are quick (~10 ms), ~2% take ~90 ms and ~1% ~250 ms.
static void realisticSd(size_t bytes, bool write) {
  double ms;
  if (!write) ms = 2.0 + bytes / 10000.0;
  else { int r = rand() % 100; ms = r < 97 ? 10 : r < 99 ? 90 : 250; }
  if (traceOn) {
    uint32_t minFill = RING_FRAMES;
    for (auto &tr : tracks) if (tr.live || tr.armed) minFill = std::min(minFill, tr.head - tr.tail);
    char line[160];
    snprintf(line, sizeof line, "%8.1f ms  %s %5zu B %5.1f ms  min ring %5u  rec backlog %6d  topped %d  [%s]",
             inputIndex / 44.1, write ? "W" : "R", bytes, ms, minFill,
             recording.mode ? (int)(inputIndex - recording.startSample - recording.written) : 0, simTopped(), (const char *)debugStage);
    traceLines.push_back(line);
    if (traceLines.size() > 30) traceLines.pop_front();
  }
  pendingUs += ms * 1000;
  const double blockUs = AUDIO_BLOCK_SAMPLES * 1e6 / AUDIO_SAMPLE_RATE_EXACT;
  while (pendingUs >= blockUs) { pendingUs -= blockUs; audioBlock(); }
}

// One audio interrupt + whatever loop() would do in between.
static void block() {
  audioBlock();
  serviceStorage();
  serviceScenes();
}

// The app's tempo, for scenes: simSetTempo as a project loads, tempoByHand as
// the app's setTempo (the project keeps it, new projects start with it).
static float simTempo() { return bpmForSpb(transport.pendingSpb ? transport.pendingSpb : transport.spb); }
static bool simSetTempo(float b) {
  uint32_t spb = spbForBpm(b);
  if (spb != (transport.pendingSpb ? transport.pendingSpb : transport.spb)) transport.pendingSpb = spb;   // (the same: nothing, as the app)
  return true;
}
static float simNewBpm = 120.0f;
static float simNewTempo() { return simNewBpm; }
static void tempoByHand(float b) {
  simSetTempo(b);
  simNewBpm = b;
  projectTempoChanged();
}

#include <map>
static std::map<std::string, uint32_t> dropStages;   // what loop() was doing when tracks ran dry
static std::map<std::string, uint32_t> dropPhases;   // where the recording was (measurement below)
static uint32_t snags = 0;                            // separate dropouts (blocks with drops after one without)
static uint32_t finishedAt = 0, lastMode = 0;
#include <functional>
static std::function<void()> afterAudio;              // measurements taken in the audio interrupt
static bool lastDropped = false;

static void audioBlock() {
  uint32_t before = 0;
  for (auto &tr : tracks) before += tr.underruns;
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    int16_t v = feedSineHz ? (int16_t)lround(8000 * sin(2 * M_PI * feedSineHz * (inputIndex + i) / AUDIO_SAMPLE_RATE_EXACT))
                : feedRamp ? rampAt(inputIndex + i) : 0;
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
  if (afterAudio) afterAudio();
  if (lastMode != Recording::Idle && recording.mode == Recording::Idle) finishedAt = inputIndex;
  lastMode = recording.mode;
  if (after > before) {
    static const char *names[] = {"idle", "waiting for the bar", "recording", "finishing"};
    const char *ph = names[recording.mode];
    if (recording.mode == Recording::Idle && finishedAt && inputIndex - finishedAt < 2 * 44100) ph = "idle, < 2 s after finishing";
    if (recording.mode == Recording::Running && (int32_t)(inputIndex - recording.startSample) < 44100) ph = "recording, first second";
    dropPhases[ph] += after - before;
    if (!lastDropped) {
      snags++;
      if (traceOn && snags <= 3) {
        printf("---- snag %u at %.1f ms (%s)\n", snags, inputIndex / 44.1, ph);
        for (auto &l : traceLines) printf("%s\n", l.c_str());
      }
    }
  }
  lastDropped = after > before;
}

static void runBeats(double beats) {
  uint32_t n = (uint32_t)(beats * transport.spb / AUDIO_BLOCK_SAMPLES);
  for (uint32_t i = 0; i < n; i++) block();
}

static void runUntilIdle() {
  for (int i = 0; i < 100000 && recording.mode != Recording::Idle; i++) block();
}

// Sample n's file on the host: "NNNN.wav", or once named "XYNNNN.wav"
// (project letter, track number).
static std::string sampleFile(uint32_t n) {
  char num[16];
  snprintf(num, sizeof num, "%04u.wav", (unsigned)n);
  std::string dir = simRoot + "/L7/samples/";
  if (DIR *d = opendir(dir.c_str())) {
    while (struct dirent *e = readdir(d)) {
      size_t len = strlen(e->d_name);
      if ((len == 8 || len == 10) && !strcmp(e->d_name + len - 8, num)) {
        std::string found = dir + e->d_name;
        closedir(d);
        return found;
      }
    }
    closedir(d);
  }
  return dir + num;
}

// Reads frame `frame` of track t's sample (left channel).
static int16_t wavLeft(int t, uint32_t frame) {
  std::string path = sampleFile(tracks[t].sample);
  FILE *f = fopen(path.c_str(), "rb");
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
  // Carrying on to the loop length: playback must take over exactly where the
  // loop starts again (the beat after the recording's last), not later.
  const Track &tr = tracks[t];
  int32_t loopEnd = recording.startBeat + (int32_t)(recording.loopBars * BEATS_PER_BAR);
  bool endAhead = transport.beat < loopEnd;
  int32_t liveAt = -1;
  for (int i = 0; i < 20000 && liveAt < 0 && (endAhead || recording.mode != Recording::Idle); i++) {
    block();
    if (tr.live) liveAt = transport.beat;
  }
  runUntilIdle();
  char msg[128];
  if (endAhead) {
    snprintf(msg, sizeof msg, "plays from the loop's start as the recording ends (beat %d, live at %d)", loopEnd, liveAt);
    check(liveAt == loopEnd, msg);
  }
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
  // Expected overview: per column, the input's peak over the 4096-frame bins
  // that column touches, on the square-root scale.
  int wrong = 0;
  for (int c = 0; c < OVERVIEW_COLS; c++) {
    uint32_t f0 = (uint64_t)loopFrames * c / OVERVIEW_COLS, f1 = (uint64_t)loopFrames * (c + 1) / OVERVIEW_COLS;
    uint32_t b0 = f0 / PEAK_BIN_FRAMES, b1 = (f1 - 1) / PEAK_BIN_FRAMES;
    int peak = 0;
    for (uint32_t f = b0 * PEAK_BIN_FRAMES; f < (b1 + 1) * PEAK_BIN_FRAMES && f < tr.recordedFrames; f++)
      peak = std::max(peak, abs((int)rampAt(start + f)));
    int expect = (int)std::min(255.0f, sqrtf(peak / 32768.0f) * 255.0f + 0.5f);
    if (abs(expect - tr.overview[c]) > 1) wrong++;
  }
  snprintf(msg, sizeof msg, "waveform overview matches the input (%d of %d columns off)", wrong, OVERVIEW_COLS);
  check(wrong == 0, msg);
}

// Runs until a scene change has reached its bar line.
static bool waitScene() {
  for (int i = 0; i < 4000 && sceneChangeWaiting(); i++) block();
  return true;
}

static int32_t pmod(int32_t a, int32_t m) { int32_t r = a % m; return r < 0 ? r + m : r; }
static uint32_t quarterFrames(uint32_t q, uint32_t spb) { return (q / 4) * spb + (q % 4) * spb / 4; }

// Checks the output at the next few beats against the track's recording.
static void checkSync(int t, const char *label) {
  feedRamp = false;
  engine.monitor = false;
  for (auto &tr : tracks) tr.gain = 0;
  tracks[t].gain = 1;
  for (int i = 0; i < 6; i++) block();             // the other tracks' levels glide down (~1 ms time constant)
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
        int32_t phaseQ = pmod(4 * target - tr.rangeAnchorQ, 4 * tr.rangeBeats);
        uint32_t frame = tr.rangeFrame(quarterFrames(phaseQ, tr.spbNative));
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
    if (int r = takeResampled(); r >= 0 && !sceneFromResample(r)) endSilencing();   // (as followRecording())
    if (rand() % 300 == 0) {
      sel = rand() % NUM_TRACKS;
      Track &tr = tracks[sel];
      switch (rand() % 13) {
        case 0: case 1: lastAction = "record press"; recordPress(sel, rand() % 4 == 0); break;   // (a quarter of them resample)
        case 2: if (rand() % 3) { lastAction = "clear"; clearTrack(sel); }
                else { lastAction = "cancel recording"; cancelRecording(sel); } break;
        case 3: lastAction = "mute"; if (tr.state != TrackState::Empty) tr.state = tr.state == TrackState::Playing ? TrackState::Muted : TrackState::Playing; break;
        case 4: { lastAction = "tempo"; float b = bpm + (rand() % 21 - 10);
                  uint32_t spb = spbForBpm(b); bool ok = recording.mode == Recording::Idle && b >= MIN_BPM && b <= MAX_BPM;
                  // (kept within 0.5x-2x of each loop: the fuzzing is about the logic, not card bandwidth)
                  for (auto &x : tracks) if (x.state != TrackState::Empty) { double sp = (double)x.spbNative / spb; if (sp < 0.5 || sp > 2.0) ok = false; }
                  if (ok) { bpm = b; transport.pendingSpb = spb; } break; }
        case 5: lastAction = "range";
                if (tr.state != TrackState::Empty) {
                  uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR, len = 1 + rand() % loopBeats;   // any length
                  setRange(sel, rand() % loopBeats, len);   // may run past the loop's end
                }
                break;
        case 6: lastAction = "transport"; if (transport.running) transportStop(); else transportStart(); break;
        case 9: lastAction = "scene"; sceneStep(rand() % 2 ? 1 : -1); break;
        case 10: lastAction = "scene cut/paste"; if (rand() % 2) sceneCut(); else scenePaste(); break;
        case 11: lastAction = "scene copy"; if (rand() % 2) sceneDuplicate(); else sceneGoEmpty(); break;
        case 12: lastAction = "track cut/paste"; if (!cutTrack(sel)) pasteTrack(sel); break;
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
  simDelayHook = [](uint32_t us) {                 // loop() waiting: the audio interrupt runs on
    pendingUs += us;
    const double blockUs = AUDIO_BLOCK_SAMPLES * 1e6 / AUDIO_SAMPLE_RATE_EXACT;
    while (pendingUs >= blockUs) { pendingUs -= blockUs; audioBlock(); }
  };
  simRoot = "/tmp/looper_sim_sd";
  system(("rm -rf " + simRoot + " && mkdir -p " + simRoot + "/looper").c_str());
  // A card from an older version: track 5 holds a 1-bar loop in its own
  // track5.wav, settings in the header (version 9).
  const uint32_t legacyFrames = 4 * 22059;
  {
    uint8_t h[WAV_DATA_OFFSET] = {};
    memcpy(h, "RIFF", 4);
    struct { char magic[4]; uint32_t version, used, spb, loopBars, frames, rangeStart, rangeLen, muted, phase; } info =
        {{'M', '8', 'L', 'P'}, 9, 1, 22059, 1, legacyFrames, 0, 2, 1, 0};
    memcpy(h + 44, &info, sizeof info);
    for (int c = 0; c < OVERVIEW_COLS; c++) h[44 + sizeof info + c] = 100;
    int8_t settings[4] = {3, 20, 25, -4};          // filter lo/hi, volume, balance
    memcpy(h + 44 + sizeof info + OVERVIEW_COLS, settings, 4);
    // Track 4: an empty full-size file (it becomes recording space, numbered
    // before track 5's sample: renumbered at boot so new recordings come after).
    FILE *e = fopen((simRoot + "/looper/track4.wav").c_str(), "wb");
    struct { char magic[4]; uint32_t version, used; } none = {{'M', '8', 'L', 'P'}, 9, 0};
    uint8_t eh[WAV_DATA_OFFSET] = {};
    memcpy(eh, "RIFF", 4);
    memcpy(eh + 44, &none, sizeof none);
    fwrite(eh, 1, sizeof eh, e);
    ftruncate(fileno(e), WAV_DATA_OFFSET + (uint64_t)MAX_REC_FRAMES * 4);
    fclose(e);
    FILE *f = fopen((simRoot + "/looper/track5.wav").c_str(), "wb");
    fwrite(h, 1, sizeof h, f);
    for (uint32_t i = 0; i < legacyFrames; i++) { int16_t fr[2] = {(int16_t)(i % 1000 - 500), 0}; fwrite(fr, 4, 1, f); }
    fclose(f);
  }
  printf("first boot (with track 5 in the older one-file-per-track format)\n");
  storageBegin([](const char *m) { if (*m) printf("  (%s)\n", m); });
  scenesBegin(simTempo, simSetTempo, 0, simNewTempo);
  check(sceneCount() == 1 && sceneCurrent() == 1, "scenes start with what's on the tracks as scene 1");
  {
    const Track &tr = tracks[4];
    char m[160];
    snprintf(m, sizeof m, "track 5 now plays sample %04u (after track 4's empty file): %u frames, %u bar, range %u+%u, muted %d",
             tr.sample, tr.recordedFrames, tr.loopBars, tr.rangeStartBeat, tr.rangeBeats, tr.state == TrackState::Muted);
    check(tr.sample == 2 && tr.recordedFrames == legacyFrames && tr.loopBars == 1 && tr.rangeBeats == 2 &&
          tr.state == TrackState::Muted && tr.overview[10] == 100, m);
    check(tr.filterLo == 3 && tr.filterHi == 20 && tr.volume == 3 && tr.balance == -1,
          "its filter kept, volume and balance in today's steps (25 -> 3, -4 -> -1)");
    check(wavLeft(4, 0) == -500 && wavLeft(4, 1234) == 1234 % 1000 - 500 && wavLeft(4, legacyFrames - 1) == (int16_t)((legacyFrames - 1) % 1000 - 500),
          "its audio is intact in samples/0002.wav");
    struct stat st;
    check(::stat((simRoot + "/looper").c_str(), &st) != 0 && ::stat((simRoot + "/L7/samples").c_str(), &st) == 0,
          "the app's folder moved from /looper to /L7");
    FILE *old = fopen((simRoot + "/L7/track5.wav").c_str(), "rb");
    check(!old && sampleCount() == 1, "the old track file is gone; one sample on the card");
    if (old) fclose(old);
    FILE *low = fopen((simRoot + "/L7/samples/0001.wav").c_str(), "rb");
    check(!low, "track 4's empty file renumbered after it (0001 gone)");
    if (low) fclose(low);
    clearTrack(4);
    flushHeaders();
  }
  transport.spb = spbForBpm(120);
  int fsChangesAtStart = simFsChanges;
  midiBegin(&midi);

  recordTrack(0, 4 * 4 + 0.5, 4, "track 1: stop 4 bars + 0.5 beats in, from stopped (trims to 4 bars)");
  recordTrack(1, 2 * 4 + 3, 3, "track 2: stop 2 bars + 3 beats in (carries on to the end of bar 3)");
  recordTrack(2, 3, 1, "track 3: stop 3 beats in (carries on to 1 bar)");

  runBeats(4);
  checkSync(0, "track 1 at 120 BPM");
  checkSync(1, "track 2 at 120 BPM");

  printf("tempo change to 90 BPM\n");
  transport.pendingSpb = spbForBpm(90);
  runBeats(3);
  checkSync(0, "track 1 at 90 BPM");
  checkSync(2, "track 3 at 90 BPM");

  printf("loop range of track 1 changed to bars 2-3 (beats 4-11) while playing\n");
  {
    feedRamp = false;
    engine.monitor = false;
    for (auto &x : tracks) x.gain = 0;
    Track &tr = tracks[0];
    tr.gain = 1;
    tr.underruns = 0;
    uint32_t oldStart = tr.rangeStartBeat, oldBeats = tr.rangeBeats;
    int32_t oldAnchor = tr.rangeAnchorQ;
    int32_t asked = transport.beat;
    setRange(0, 4, 8);
    int32_t sw = tr.switchQ;
    char m[128];
    snprintf(m, sizeof m, "queued for beat %.2f (asked during beat %d), keeps playing meanwhile", sw / 4.0, asked);
    check(tr.rangePending && tr.live && sw > 4 * asked && sw <= 4 * asked + 8, m);
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
          bool newRange = 4 * target >= sw;
          uint32_t rs = newRange ? 4 : oldStart, rb = newRange ? 8 : oldBeats;
          int32_t anchor = newRange ? tr.rangeAnchorQ : oldAnchor;   // re-anchored at the switch
          uint32_t frame = (rs * tr.spbNative + quarterFrames(pmod(4 * target - anchor, 4 * rb), tr.spbNative)) % tr.loopFrames();
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
    check(bad == 0 && checked - before > 4 && tr.underruns == 0, m);
    check(!tr.rangePending && tr.rangeStartBeat == 4 && tr.rangeBeats == 8, "range applied and kept");
    check(!setRange(0, 0, 0) && !setRange(0, 0, tr.loopBars * 4 + 1), "lengths of 0 or longer than the loop are refused");
    for (auto &x : tracks) x.gain = 1;
    engine.monitor = true;
  }

  printf("quarter-bar range: beat 6 of track 1 repeated\n");
  {
    setRange(0, 5, 1);
    check(tracks[0].rangePending, "queued");
    runBeats(3);
    check(!tracks[0].rangePending && tracks[0].rangeBeats == 1, "switched within a few beats");
    checkSync(0, "1/4-bar range");
    setRange(0, 3, 4);                             // a bar starting on beat 4: off the length grid
    runBeats(6);
    checkSync(0, "1-bar range starting at beat 4");

    // Slide it one beat right: if the spot due at the switch is inside the new
    // range, playback must carry on from it instead of jumping.
    Track &tr = tracks[0];
    for (int slide = 0; slide < 3; slide++) {
      uint32_t oldStart = tr.rangeStartBeat;
      int32_t oldAnchor = tr.rangeAnchorQ;
      setRange(0, oldStart + 1, 4);
      int32_t sw = tr.switchQ;
      uint32_t due = 4 * oldStart + pmod(sw - oldAnchor, 16);          // file quarter the old range would play
      runBeats(8);
      uint32_t got = 4 * tr.rangeStartBeat + pmod(sw - tr.rangeAnchorQ, 16);
      bool inside = due >= 4 * (oldStart + 1) && due < 4 * (oldStart + 5);
      char m[128];
      snprintf(m, sizeof m, "slide (switch at beat %.2f): plays file beat %.2f (%s, expected %.2f)",
               sw / 4.0, got / 4.0, inside ? "kept" : "outside the new range", (inside ? due : got) / 4.0);
      check(!inside || got == due, m);
      checkSync(0, "after the slide");
    }
    // Ranges running past the loop's end carry on from its start.
    uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
    setRange(0, loopBeats - 2, 4);
    runBeats(6);
    check(!tr.rangePending && tr.rangeStartBeat == loopBeats - 2, "range across the loop's end applied");
    checkSync(0, "range across the loop's end");
    setRange(0, loopBeats / 4, loopBeats);         // the whole loop, from a quarter in
    runBeats(6);
    checkSync(0, "whole-loop range offset by a quarter");
    check(startPosition(64, 0, 16) == 0 && startPosition(64, 4, 8) == 4 && startPosition(64, 3, 16) == 4 &&
          startPosition(64, 1, 8) == 4, "longer loops, ranges of a bar or more: the first bar start");
    check(startPosition(64, 63, 16) == 0 && startPosition(64, 61, 4) == 0, "... also across the loop's end");
    check(startPosition(16, 0, 16) == 0 && startPosition(32, 3, 16) == 4 && startPosition(16, 1, 8) == 2 &&
          startPosition(32, 5, 4) == 6, "loops up to 8 bars: the first half bar or bar line");
    check(startPosition(16, 15, 16) == 0 && startPosition(16, 13, 4) == 14, "... also across the loop's end");
    check(startPosition(16, 5, 2) == 6 && startPosition(16, 6, 2) == 6 && startPosition(16, 15, 2) == 0,
          "2-beat ranges: the first bar start or middle");
    check(startPosition(16, 5, 1) == 5 && startPosition(16, 15, 1) == 15, "1-beat ranges: their start");
    setRange(0, 4, 8);
    runBeats(6);
    for (auto &x : tracks) x.gain = 1;
    engine.monitor = true;
  }

  printf("clipboard: track 3 cut, pasted onto tracks 6 and 7 while playing\n");
  {
    uint32_t sample = tracks[2].sample;
    int32_t anchor = tracks[2].rangeAnchorQ;
    check(cutTrack(2) && tracks[2].state == TrackState::Empty && clipboardFull(), "cut: track 3 empty, clipboard full");
    check(!cutTrack(2), "nothing to cut on an empty track");
    check(pasteTrack(5) && pasteTrack(6) && clipboardFull(), "pasted twice, still on the clipboard");
    check(!pasteTrack(0), "no pasting over a clip");
    check(tracks[5].sample == sample && tracks[6].sample == sample &&
          (tracks[6].rangeAnchorQ - anchor) % (int32_t)(4 * tracks[6].rangeBeats) == 0,
          "both play the same sample, in the same alignment");
    runBeats(4);
    checkSync(5, "track 6 (pasted)");
    checkSync(6, "track 7 (pasted)");
    check(sampleExists(sample), "the sample is still on the card's list");
    clearTrack(5);
    clearTrack(6);
    for (auto &x : tracks) x.gain = 1;
    engine.monitor = true;
  }

  printf("choosing samples on track 7 (empty) while playing: in the order of their names\n");
  {
    Track &tr = tracks[6];
    int changes = simFsChanges;
    transportStop();
    runUntilIdle();
    for (int i = 0; i < 40; i++) serviceStorage();   // the recordings named (stopped)
    fsChangesAtStart += simFsChanges - changes;    // (stopped: the file system may change)
    transportStart();
    // The recordings' file names, sorted, with their numbers.
    std::vector<std::pair<std::string, uint32_t>> names;
    for (uint32_t n = 1; n <= 9999; n++)
      if (sampleExists(n)) { std::string f = sampleFile(n); names.push_back({f.substr(f.rfind('/') + 1), n}); }
    std::sort(names.begin(), names.end());
    size_t count = names.size();
    auto at = [&](long i) { return names[((i % (long)count) + count) % count].second; };
    // Track 7 is track 1 on the screen: its own recordings start "A1" (project A).
    std::string own = std::string(1, (char)('A' + projectCurrent())) + "1", after = own.substr(0, 1) + "2";
    long first = 0, last = -1;                     // its own first (or the next name), its own last (or the one before)
    while (first < (long)count && names[first].first < own) first++;
    while (last + 1 < (long)count && names[last + 1].first < after) last++;
    setTrackMix(6, 3, 2);
    check(stepSample(6, 1) && tr.sample == at(first), "SHIFT+RIGHT on an empty track: the first of its own, or the next name");
    check(stepSample(6, 1) && tr.sample == at(first + 1), "SHIFT+RIGHT: the next name (its own first, then the project's other tracks, then other projects)");
    check(stepSample(6, -1) && tr.sample == at(first), "SHIFT+LEFT: back to the previous");
    for (size_t i = 0; i < count; i++) stepSample(6, 1);
    check(tr.sample == at(first), "SHIFT+RIGHT through all of them: round to the same one (wrapping)");
    check(stepSample(6, 1) && tr.sample == at(first + 1) && tr.rangeStartBeat == 0 && tr.rangeBeats == tr.loopBars * 4 &&
          tr.volume == 3 && tr.balance == 2, "range reset to the whole loop, volume and balance kept");
    setTrackMix(6, VOLUME_UNITY, 0);               // the sync check compares at unity gain
    trackChanged(6);
    runBeats(4);
    for (auto &x : tracks) x.gain = 0;
    tr.gain = 1;
    checkSync(6, "track 7 playing its new sample");
    clearTrack(6);
    check(stepSample(6, -1) && tr.sample == at(last), "SHIFT+LEFT on an empty track: the last of its own, or the name before");
    clearTrack(6);
    char m[64];
    snprintf(m, sizeof m, "%zu recordings on the card", count);
    check(count == 4, m);
    for (auto &x : tracks) x.gain = 1;
    engine.monitor = true;
  }

  printf("record on track 7 while track 6 records: 6 stops, 7 records next\n");
  {
    int changes = simFsChanges;
    transportStop();                               // recording space ready
    for (int i = 0; i < 20; i++) serviceStorage();
    transportStart();
    feedRamp = true;
    recordPress(5);
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    runBeats(5.5);
    check(recordPress(6) && recording.track == 5 && recording.mode == Recording::Finishing && recordQueued() == 6,
          "track 6 finishing, track 7 queued");
    check(tracks[6].sample != 0, "track 7's sample file taken when queued");
    int32_t end = recording.startBeat + (int32_t)(recording.loopBars * BEATS_PER_BAR);
    uint32_t endSample = recording.startSample + recording.loopBars * BEATS_PER_BAR * transport.spb;
    for (int i = 0; i < 20000 && recording.track != 6; i++) block();
    char m[128];
    snprintf(m, sizeof m, "track 7 records from where track 6 ends (beat %d, expected %d), recording at once",
             recording.startBeat, end);
    check(recording.track == 6 && recording.mode == Recording::Running && recording.startBeat == end &&
          recording.startSample == endSample && recordQueued() < 0, m);
    check(tracks[5].state == TrackState::Playing && tracks[5].loopBars == 2, "track 6 holds its 2 bars");
    runBeats(3.5);
    uint32_t start = recording.startSample;
    recordPress(6);
    runUntilIdle();
    check(tracks[6].state == TrackState::Playing && tracks[6].loopBars == 1, "track 7 holds its 1 bar");
    check(wavLeft(6, 0) == rampAt(start) && wavLeft(6, 1000) == rampAt(start + 1000),
          "track 7's first frames are the input right after track 6's last");
    // Stopped just past a bar line (trimmed back to it): the next one still
    // starts there, catching up from the input history.
    clearTrack(5);
    clearTrack(6);
    recordPress(5);
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    runBeats(4.5);
    recordPress(6);
    int32_t bar = recording.startBeat + 4;
    for (int i = 0; i < 20000 && recording.track != 6; i++) block();
    snprintf(m, sizeof m, "trimmed: track 7 records from track 6's end (beat %d, expected %d)", recording.startBeat, bar);
    check(recording.track == 6 && recording.mode == Recording::Running && recording.startBeat == bar &&
          tracks[5].loopBars == 1, m);
    runBeats(3.5);
    recordPress(6);
    runUntilIdle();
    recordPress(0);                                // track 1 holds a clip: refused, nothing queued
    check(recording.mode == Recording::Idle && recordQueued() < 0, "nothing recorded or queued on tracks with clips");
    clearTrack(5);
    clearTrack(6);
    recordPress(5);
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    runBeats(2);
    recordPress(6);
    recordPress(6);
    check(recordQueued() < 0 && tracks[6].sample == 0, "pressed again: no longer queued, its file given back");
    runUntilIdle();
    check(recording.mode == Recording::Idle && tracks[6].state == TrackState::Empty, "track 7 not recorded");
    clearTrack(6);
    // SHIFT+A: a recording thrown away at once, while running and while
    // carrying on to its length; its file back to the pool.
    recordPress(6);
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    runBeats(2);
    uint32_t sample = tracks[6].sample;
    check(sample && cancelRecording(6) && recording.mode == Recording::Idle && tracks[6].state == TrackState::Empty &&
          tracks[6].sample == 0, "cancelled while recording: nothing recorded, its file given back");
    runBeats(1);
    recordPress(6);
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    check(tracks[6].sample == sample, "the next recording reuses that file");
    runBeats(5.5);
    recordPress(6);                                // carries on to 2 bars
    check(recording.mode == Recording::Finishing && cancelRecording(6) && recording.mode == Recording::Idle &&
          tracks[6].state == TrackState::Empty && !tracks[6].live && !tracks[6].armed,
          "cancelled while finishing: the track empty again, not playing");
    runBeats(2);
    check(tracks[6].state == TrackState::Empty && tracks[6].underruns == 0, "and it stays empty");
    transportStop();                               // recording space refilled for what follows
    for (int i = 0; i < 20; i++) serviceStorage();
    transportStart();
    for (auto &x : tracks) x.underruns = 0;
    fsChangesAtStart += simFsChanges - changes;   // more recordings than the pool holds: not counted
  }

  printf("snapping to the nearest bar (tap tempo while playing)\n");
  {
    while (transport.beat % 4 != 1) block();       // early in a bar (beat 2)
    int32_t bar = transport.beat / 4;
    transportSnapToBar();
    check(transport.beat == bar * 4 && transport.sampleInBeat < 2 * AUDIO_BLOCK_SAMPLES, "early in the bar: back to its start");
    while (transport.beat % 4 != 3) block();       // late in a bar (beat 4)
    bar = transport.beat / 4;
    transportSnapToBar();
    check(transport.beat == (bar + 1) * 4, "late in the bar: on to the next one");
    for (auto &x : tracks) x.underruns = 0;
    checkSync(0, "track 1 after snapping");
    checkSync(1, "track 2 after snapping");
    for (auto &x : tracks) x.gain = 1;
    engine.monitor = true;
  }

  printf("scenes while playing\n");
  {
    for (int i = 0; i < 50; i++) block();        // changes so far saved in scene 1
    auto empty = [] { for (auto &x : tracks) if (x.state != TrackState::Empty) return false; return true; };
    uint32_t s1 = tracks[0].sample, s2 = tracks[1].sample;
    check(sceneCurrent() == 1 && sceneCount() == 1, "on scene 1 of 1");
    check(sceneStep(-1) && waitScene() && sceneCurrent() == 0 && empty(), "SHIFT+DOWN: the empty scene 0*, tracks empty");
    check(!sceneStep(-1) && waitScene(), "no wrapping past 0*");
    check(sceneStep(1) && waitScene() && tracks[0].sample == s1 && tracks[1].sample == s2, "SHIFT+UP: scene 1 back");
    check(sceneStep(1) && waitScene() && sceneCurrent() == 2 && sceneCount() == 1 && tracks[0].sample == s1,
          "SHIFT+UP past the last: 2*, a copy of scene 1, not stored");
    check(!sceneStep(1) && waitScene(), "nothing after 2*");
    tracks[1].state = TrackState::Muted;           // a change on 2*
    trackChanged(1);
    block();
    check(sceneCount() == 2 && sceneCurrent() == 2, "changing 2* makes it scene 2");
    check(sceneStep(-1) && waitScene() && tracks[1].state == TrackState::Playing, "scene 1 keeps track 2 playing");
    check(sceneStep(1) && waitScene() && tracks[1].state == TrackState::Muted, "scene 2 has it muted");
    check(sceneCut() && waitScene() && sceneClipboardFull() && sceneCount() == 1 && sceneCurrent() == 1 &&
          tracks[1].state == TrackState::Playing, "SHIFT+B+A on scene 2: cut, back on scene 1");
    check(scenePaste() && waitScene() && sceneCount() == 2 && sceneCurrent() == 2 && tracks[1].state == TrackState::Muted,
          "B+A: pasted after it as scene 2");
    check(sceneDuplicate() && waitScene() && sceneCount() == 3 && sceneCurrent() == 3 && tracks[1].state == TrackState::Muted,
          "SHIFT+B twice: a copy as scene 3");
    check(sceneGoEnd() && waitScene() && sceneCurrent() == sceneCount() + 1, "SHIFT+UP twice: straight to N+1*");
    check(sceneGoEmpty() && waitScene() && sceneCurrent() == 0 && empty(), "SHIFT+DOWN twice: straight to 0*");
    setTrackMix(6, 3, 0);                          // a change on 0*
    trackChanged(6);
    block();
    check(sceneCurrent() == 1 && sceneCount() == 4 && tracks[6].volume == 3, "changing 0* inserts a new scene 1");
    check(sceneStep(1) && waitScene() && sceneCurrent() == 2 && tracks[0].sample == s1 && tracks[1].state == TrackState::Playing &&
          tracks[6].volume == VOLUME_UNITY, "the others moved up: old scene 1 is now 2");
    runBeats(4);
    for (auto &x : tracks) { x.gain = 0; x.underruns = 0; }
    tracks[0].gain = 1;
    checkSync(0, "track 1 after the scene changes");
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
    // Stopped: finished samples are cut down to size and the recording space
    // topped up, a file at a time.
    int changes = simFsChanges;
    for (int i = 0; i < 20; i++) serviceStorage();
    check(simFsChanges > changes, "stopped: samples trimmed and recording space refilled");
    flushHeaders();
    Track saved[NUM_TRACKS];
    for (int t = 0; t < NUM_TRACKS; t++) {
      saved[t].state = tracks[t].state; saved[t].loopBars = tracks[t].loopBars;
      saved[t].recordedFrames = tracks[t].recordedFrames; saved[t].rangeStartBeat = tracks[t].rangeStartBeat;
      saved[t].rangeBeats = tracks[t].rangeBeats; saved[t].spbNative = tracks[t].spbNative;
      saved[t].rangeAnchorQ = tracks[t].rangeAnchorQ;
      memcpy(saved[t].overview, tracks[t].overview, OVERVIEW_COLS);
      tracks[t].state = TrackState::Empty; tracks[t].file = FsFile();
    }
    int before = simFileCreates;
    int scenes = sceneCount(), scene = sceneCurrent();
    for (int i = 0; i < 20; i++) serviceScenes();  // anything left saved
    storageBegin([](const char *) {});
    scenesBegin(simTempo, simSetTempo, 0, simNewTempo);
    check(sceneCount() == scenes && sceneCurrent() == scene, "scenes and the current one restored");
    bool same = true;
    for (int t = 0; t < NUM_TRACKS; t++) {
      const Track &a = saved[t], &b = tracks[t];
      if (a.state != b.state) same = false;
      if (a.state != TrackState::Empty &&
          (a.loopBars != b.loopBars || a.recordedFrames != b.recordedFrames || a.rangeStartBeat != b.rangeStartBeat ||
           a.rangeBeats != b.rangeBeats || a.spbNative != b.spbNative ||
           // the range's alignment to the bar counter (anchor modulo its length)
           ((a.rangeAnchorQ - b.rangeAnchorQ) % (int32_t)(4 * a.rangeBeats)) != 0 ||
           memcmp(a.overview, b.overview, OVERVIEW_COLS))) same = false;
    }
    check(same, "every track's state, length, range, its alignment, tempo and waveform restored");
    int cleared = -1;
    for (int t = 0; t < NUM_TRACKS && cleared < 0; t++) if (tracks[t].state != TrackState::Empty) cleared = t;
    clearTrack(cleared);
    bool flat = true;
    for (int c = 0; c < OVERVIEW_COLS; c++) if (tracks[cleared].overview[c]) flat = false;
    check(flat, "clearing a track flattens its waveform");
    check(simFileCreates == before, "second boot creates no files");
  }

  printf("slow card (measurement): 7 tracks playing while track 7 is recorded and cleared again and again\n");
  {
    simSdLatency = realisticSd;
    srand(7);
    if (!transport.running) transportStart();
    for (int t = 0; t < 6; t++)
      if (tracks[t].state == TrackState::Empty) { recordPress(t); runBeats(16.2); recordPress(t); runUntilIdle(); }
    for (float bpm : {120.0f, 150.0f, 180.0f, 220.0f}) {
      // Tempo relative to what the tracks were recorded at (mostly 120, one at 90).
      transport.pendingSpb = spbForBpm(bpm);
      runBeats(4);
      for (auto &tr : tracks) tr.underruns = 0;
      dropStages.clear();
      uint32_t overruns = 0;
      for (int cycle = 0; cycle < 5; cycle++) {
        clearTrack(6);
        runBeats(2);
        recordPress(6);
        runBeats(9 + cycle);
        overruns += recording.overruns;
        recordPress(6);
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

  printf("card stalls for 1.5 s while track 7 records: the lost part saved as silence\n");
  {
    transportStop();                               // recording space ready
    for (int i = 0; i < 20; i++) serviceStorage();
    transportStart();
    feedRamp = true;
    clearTrack(5);
    recordPress(5);
    // The file's old contents: junk that must not be left in the recording.
    if (FILE *f = fopen(sampleFile(tracks[5].sample).c_str(), "r+b")) {
      std::vector<int16_t> junk(2 * 44100 * 8, 0x5555);
      fseek(f, WAV_DATA_OFFSET, SEEK_SET);
      fwrite(junk.data(), 2, junk.size(), f);
      fclose(f);
    }
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    uint32_t start = recording.startSample;
    runBeats(2);
    static int stalls;
    stalls = 1;
    simSdLatency = [](size_t, bool write) {
      if (!write || !stalls) return;
      stalls--;
      pendingUs += 1500000;
      const double blockUs = AUDIO_BLOCK_SAMPLES * 1e6 / AUDIO_SAMPLE_RATE_EXACT;
      while (pendingUs >= blockUs) { pendingUs -= blockUs; audioBlock(); }
    };
    runBeats(5);
    simSdLatency = nullptr;
    uint32_t overruns = recording.overruns;
    recordPress(5);
    runUntilIdle();
    const Track &tr = tracks[5];
    uint32_t silent = 0, junk = 0, audio = 0;
    for (uint32_t f = 0; f < tr.recordedFrames; f++) {
      int16_t v = wavLeft(5, f);
      if (v == rampAt(start + f)) audio++;
      else if (v == 0) silent++;
      else junk++;
    }
    char m[128];
    snprintf(m, sizeof m, "%u overrun(s): %u frames silent, %u junk, %u recorded", overruns, silent, junk, audio);
    check(overruns > 0 && silent > 10000 && junk == 0, m);
    runBeats(4);
    clearTrack(5);
    for (auto &x : tracks) x.underruns = 0;
  }

  printf("slow card (measurement): 5 tracks playing, track 6 recorded, then track 7 queued after it\n");
  {
    simSdLatency = realisticSd;
    srand(11);
    for (int queued = 0; queued < 2; queued++) {
      runBeats(4);
      for (auto &tr : tracks) tr.underruns = 0;
      dropStages.clear();
      for (int cycle = 0; cycle < 5; cycle++) {
        clearTrack(5);
        clearTrack(6);
        transportStop();                           // recording space refilled
        for (int i = 0; i < 20; i++) serviceStorage();
        transportStart();
        runBeats(2);
        recordPress(5);
        runBeats(8 + cycle);
        if (queued) {
          recordPress(6);
        } else {
          recordPress(5);
          runUntilIdle();
          recordPress(6);
        }
        runBeats(9);
        recordPress(6);
        runUntilIdle();
        runBeats(8);
      }
      uint32_t drops = 0;
      for (auto &tr : tracks) drops += tr.underruns;
      printf("  info  %s: %u dropped samples\n", queued ? "queued" : "one after the other", drops);
      for (auto &kv : dropStages) printf("        during '%s': %u\n", kv.first.c_str(), kv.second);
    }
    simSdLatency = nullptr;
    clearTrack(5);
    clearTrack(6);
  }

  printf("slow card (measurement): where dropouts fall around a recording, by tracks playing (all at their own tempo)\n");
  {
    simSdLatency = realisticSd;
    traceOn = getenv("LOOPER_TRACE") != nullptr;   // the card's operations before the first snags
    for (int t = 0; t < NUM_TRACKS; t++) clearTrack(t);      // all recorded at the tempo they play at
    for (int playing : {4, 5, 6}) {
      srand(21);
      for (int t = 0; t < NUM_TRACKS; t++) tracks[t].gain = 1;
      for (int t = 0; t < 6; t++) {
        if (t < playing && tracks[t].state == TrackState::Empty) { recordPress(t); runBeats(16.2); recordPress(t); runUntilIdle(); }
        if (t >= playing && tracks[t].state != TrackState::Empty) clearTrack(t);
      }
      clearTrack(6);
      transportStop();
      for (int i = 0; i < 40; i++) serviceStorage();
      transportStart();
      runBeats(8);
      for (auto &tr : tracks) tr.underruns = 0;
      dropPhases.clear();
      dropStages.clear();
      snags = 0;
      for (int cycle = 0; cycle < 4; cycle++) {
        runBeats(3);
        recordPress(6);
        runBeats(10.3);
        recordPress(6);
        runUntilIdle();
        runBeats(8);
        clearTrack(6);
      }
      uint32_t drops = 0;
      for (auto &tr : tracks) drops += tr.underruns;
      printf("  info  %d tracks playing, 4 recordings: %u snags, %u dropped samples\n", playing, snags, drops);
      for (auto &kv : dropPhases) printf("        %s: %u\n", kv.first.c_str(), kv.second);
      for (auto &kv : dropStages) printf("        during '%s': %u\n", kv.first.c_str(), kv.second);
    }
    simSdLatency = nullptr;
  }

  printf("switching all the tracks to other samples while playing (as a scene change does), slow card\n");
  {
    simSdLatency = realisticSd;
    srand(5);
    std::vector<uint32_t> samples;
    for (uint32_t n = 1; n <= 9999; n++) if (sampleExists(n)) samples.push_back(n);
    if (!transport.running) transportStart();
    runBeats(4);
    auto switchAll = [&](int set) {
      for (int t = 0; t < NUM_TRACKS; t++) {
        TrackUnit u = unitOf(t);
        u.sample = samples[(t + NUM_TRACKS * set + 3) % samples.size()];
        u.rangeStart = 0; u.rangeLen = 0; u.rangePhase = 0; u.muted = 0;
        setTrackUnit(t, u);
      }
    };
    auto switched = [&](int set) {
      for (int t = 0; t < NUM_TRACKS; t++)
        if (tracks[t].sample != samples[(t + NUM_TRACKS * set + 3) % samples.size()] || tracks[t].sampleSwitch || !tracks[t].live)
          return false;
      return true;
    };
    switchAll(1);                                  // every track playing first (empty ones start on a beat)
    runBeats(4);
    double sumLatency = 0, maxLatency = 0;
    uint32_t silent = 0, drops = 0;
    const int SWITCHES = 12;
    for (int sw = 0; sw < SWITCHES; sw++) {
      runBeats(1.37 + sw * 0.29);                  // anywhere in the beat
      for (auto &tr : tracks) tr.underruns = 0;
      uint32_t t0 = inputIndex;
      switchAll(sw);
      // When the interrupt plays every new sample (loop() may be held up by
      // the card meanwhile, so measured from the interrupt).
      double latency = -1;
      afterAudio = [&] {
        for (auto &tr : tracks) if (!tr.live) silent += AUDIO_BLOCK_SAMPLES;
        if (latency < 0 && switched(sw)) latency = (inputIndex - t0) / 44.1;
      };
      for (int i = 0; i < 4000 && latency < 0; i++) block();
      afterAudio = nullptr;
      runBeats(2);
      for (auto &tr : tracks) drops += tr.underruns;
      sumLatency += latency; maxLatency = std::max(maxLatency, latency);
    }
    char m[160];
    snprintf(m, sizeof m, "%d switches at %.0f BPM: new samples heard after %.0f ms on average (max %.0f), %u samples of silence, %u dropped",
             SWITCHES, bpmForSpb(transport.spb), sumLatency / SWITCHES, maxLatency, silent, drops);
    check(maxLatency > 0 && maxLatency < 300 && silent == 0 && drops == 0, m);
    simSdLatency = nullptr;
    for (int t = 0; t < NUM_TRACKS; t++) {
      snprintf(m, sizeof m, "track %d after switching (recorded at %.0f BPM)", t + 1, bpmForSpb(tracks[t].spbNative));
      checkSync(t, m);
    }
    // Stepping on quickly: each change replaces the planned one.
    runBeats(1.5);
    switchAll(0);
    for (int i = 0; i < 3; i++) block();
    switchAll(1);
    for (int i = 0; i < 3; i++) block();
    switchAll(2);
    for (int i = 0; i < 400 && !switched(2); i++) block();
    check(switched(2), "three quick changes: the last one plays");
    runBeats(1);
    for (int t = 0; t < 2; t++) {
      snprintf(m, sizeof m, "track %d after the quick changes", t + 1);
      checkSync(t, m);
    }
    // Back to the same sample with another range: no file needed.
    TrackUnit u = unitOf(0);
    u.rangeStart = 4; u.rangeLen = 4; u.rangePhase = 0;
    check(setTrackUnit(0, u) && tracks[0].live, "same sample, another range: keeps playing until the switch");
    runBeats(2);
    check(tracks[0].rangeStartBeat == 4 && tracks[0].rangeBeats == 4, "then on the new range");
    checkSync(0, "track 1 on its new range");
  }

  printf("scene changes while playing wait for the next bar line\n");
  {
    if (!transport.running) transportStart();
    runBeats(4);
    sceneStateChanged();                           // scene A: what's playing
    for (int i = 0; i < 50; i++) block();
    TrackUnit a[NUM_TRACKS];
    for (int t = 0; t < NUM_TRACKS; t++) a[t] = unitOf(t);
    uint32_t spbA = transport.spb;
    // Scene B: a copy with track 3 on another sample, track 5 muted, track 6
    // empty, and the tempo changed there (the project's: A's too).
    check(sceneDuplicate(), "scene B: a copy of A");
    stepSample(2, 1);
    setTrackMuted(4, true);
    clearTrack(5);
    tempoByHand(125);
    sceneStateChanged();
    runBeats(3);
    TrackUnit b2 = unitOf(2);
    uint32_t spbB = transport.spb;
    check(b2.sample != a[2].sample && trackMuted(4) && tracks[5].state == TrackState::Empty && spbB != spbA,
          "scene B set up");
    runBeats(1.3);
    while (transport.beat % BEATS_PER_BAR != 1) block();   // early in a bar: the streams don't take A's clips yet
    // To A, and when each part of the change is heard.
    struct At { int32_t beat = -1; uint32_t sib = 0; void mark() { if (beat < 0) { beat = transport.beat; sib = transport.sampleInBeat; } } };
    At sw, tempo, unmute, start;
    afterAudio = [&] {
      if (tracks[2].sample == a[2].sample && !tracks[2].sampleSwitch && tracks[2].live) sw.mark();
      if (transport.spb == spbA) tempo.mark();
      if (tracks[4].state == TrackState::Playing) unmute.mark();
      if (tracks[5].live) start.mark();
    };
    check(sceneStep(-1), "back to scene A");
    check(sceneChangeWaiting() && tracks[2].sample == b2.sample && tracks[4].state == TrackState::Muted &&
          tracks[5].state != TrackState::Empty && transport.spb == spbB, "waiting: nothing changed yet (label dim)");
    waitScene();
    for (int i = 0; i < 20; i++) block();
    afterAudio = nullptr;
    auto onBar = [](const At &x, uint32_t within) { return x.beat >= 0 && x.beat % 4 == 0 && x.sib <= within; };
    char m[160];
    snprintf(m, sizeof m, "on a bar line together: sample (beat %d +%u), unmute (%d +%u), empty track starts (%d +%u)",
             sw.beat, sw.sib, unmute.beat, unmute.sib, start.beat, start.sib);
    check(onBar(sw, AUDIO_BLOCK_SAMPLES) && onBar(unmute, 4 * AUDIO_BLOCK_SAMPLES) &&
          onBar(start, AUDIO_BLOCK_SAMPLES) && sw.beat == unmute.beat && sw.beat == start.beat, m);
    check(tempo.beat < 0 && transport.spb == spbB, "the tempo stays: the project's (changed in B, A has it too)");
    bool same = true;
    for (int t = 0; t < NUM_TRACKS; t++) if (unitOf(t).sample != a[t].sample) same = false;
    check(same && !sceneChangeWaiting(), "scene A's tracks, label bright again");
    checkSync(2, "track 3 back on A's sample");
    checkSync(5, "track 6 (started on the bar)");
    // To B: track 6 stops on the bar; a volume set while waiting stays.
    runBeats(1.2);
    At stop;
    afterAudio = [&] { if (!tracks[5].live) stop.mark(); };
    uint32_t beatsBefore[NUM_TRACKS];
    int32_t anchorBefore[NUM_TRACKS];
    for (int t = 0; t < NUM_TRACKS; t++) { beatsBefore[t] = tracks[t].rangeBeats; anchorBefore[t] = tracks[t].rangeAnchorQ; }
    check(sceneStep(1), "to scene B");
    setTrackMix(0, 3, 0);
    trackChanged(0);
    waitScene();
    for (int i = 0; i < 20; i++) block();
    afterAudio = nullptr;
    snprintf(m, sizeof m, "track 6 stops on the bar line (beat %d +%u), then empty", stop.beat, stop.sib);
    check(onBar(stop, AUDIO_BLOCK_SAMPLES) && tracks[5].state == TrackState::Empty, m);
    check(tracks[0].volume == 3, "the volume set while waiting stayed");
    int kept = 0, jumped = 0;
    for (int t = 0; t < NUM_TRACKS; t++) {
      if (tracks[t].state == TrackState::Empty || tracks[t].rangeBeats != beatsBefore[t]) continue;
      if (pmod(tracks[t].rangeAnchorQ - anchorBefore[t], 4 * beatsBefore[t]) == 0) kept++; else jumped++;
    }
    snprintf(m, sizeof m, "loops as long as before carry on from the same spot (%d kept, %d jumped)", kept, jumped);
    check(kept > 0 && jumped == 0, m);
    checkSync(2, "track 3 on B's sample");
    setTrackMix(0, VOLUME_UNITY, 0);
    trackChanged(0);
    // Back and forth before the bar: A's clips just carry on.
    runBeats(1.2);
    uint32_t drops = 0;
    for (auto &x : tracks) x.underruns = 0;
    sceneStep(-1);
    for (int i = 0; i < 20; i++) block();
    sceneStep(1);
    for (int i = 0; i < 20; i++) block();
    sceneStep(-1);
    waitScene();
    runBeats(2);
    for (auto &x : tracks) drops += x.underruns;
    check(tracks[2].sample == a[2].sample && drops == 0, "A, B, A before the bar: A, no gaps");
    checkSync(2, "track 3 after going back and forth");
  }

  printf("SHIFT held while stepping scenes: the change starts once it's let go\n");
  {
    int from = sceneCurrent();
    uint32_t sample = tracks[2].sample;
    sceneHold(true);
    check(sceneStep(1) && sceneCurrent() == from + 1 && sceneHeld() && !sceneChangeWaiting(),
          "stepped while held: the label moves, nothing waits yet");
    runBeats(9);
    check(tracks[2].sample == sample && !sceneChangeWaiting(), "two bars later: still scene A playing");
    int32_t released = transport.beat;
    check(sceneHold(false) && sceneChangeWaiting() && !sceneHeld(), "let go: the change waits for its bar line");
    int32_t at = -1;
    afterAudio = [&] { if (at < 0 && tracks[2].sample != sample && !tracks[2].sampleSwitch) at = transport.beat; };
    waitScene();
    afterAudio = nullptr;
    char m[96];
    snprintf(m, sizeof m, "switched on the first bar line after letting go (beat %d, let go in %d)", at, released);
    check(at % 4 == 0 && at > released && at <= released + 4 && sceneCurrent() == from + 1, m);
  }

  printf("a scene selected with SHIFT held is prepared; let go early, it launches once ready\n");
  {
    int from = sceneCurrent();
    sceneHold(true);
    check(sceneStep(-1) && sceneHeld() && !unitsPrepared(), "stepped while held: preparing (the label flashes)");
    sceneHold(false);
    check(sceneHeld() && !sceneChangeWaiting(), "let go at once: waits for its files");
    for (int i = 0; i < 200 && sceneHeld(); i++) block();
    check(!sceneHeld() && sceneChangeWaiting() && sceneCurrent() == from - 1, "files open: the change launches");
    waitScene();
    runBeats(2);
    for (int t = 0; t < 2; t++)
      if (tracks[t].state == TrackState::Playing) {
        char m[64];
        snprintf(m, sizeof m, "track %d after the prepared change", t + 1);
        checkSync(t, m);
      }
    sceneStep(1);                                  // back (not held: the stopped/held logic aside)
    waitScene();
  }

  printf("quick scene changes: on the next beat, SHIFT held or not\n");
  {
    auto empty = [] { for (auto &x : tracks) if (x.state != TrackState::Empty) return false; return true; };
    while (transport.beat % 4 != 0 || transport.sampleInBeat > transport.spb / 4) block();   // early in a bar
    int32_t pressed = transport.beat;
    sceneHold(true);
    check(sceneGoEmpty() && !sceneHeld() && sceneCurrent() == 0 && sceneChangeWaiting(),
          "SHIFT held, jump to 0*: goes at once");
    int32_t at = -1;
    afterAudio = [&] { if (at < 0 && !sceneChangeWaiting()) at = transport.beat; };
    waitScene();
    for (int i = 0; i < 5; i++) block();
    afterAudio = nullptr;
    sceneHold(false);
    char m[128];
    snprintf(m, sizeof m, "switched on the next beat (beat %d, pressed in %d), tracks empty", at, pressed);
    check(at == pressed + 1 && empty(), m);
    // A step waiting for its bar line, then a quick jump: all on the next beat.
    while (transport.beat % 4 != 0 || transport.sampleInBeat > transport.spb / 4) block();
    pressed = transport.beat;
    check(sceneStep(1) && sceneChangeWaiting(), "0* to scene 1: waits for the bar line");
    for (int i = 0; i < 10; i++) block();
    check(sceneGoEnd() && sceneCurrent() == sceneCount() + 1, "then straight to N+1*");
    at = -1;
    afterAudio = [&] { if (at < 0 && !sceneChangeWaiting()) at = transport.beat; };
    waitScene();
    for (int i = 0; i < 5; i++) block();
    afterAudio = nullptr;
    snprintf(m, sizeof m, "all of it on the next beat, not the bar line (beat %d, pressed in %d)", at, pressed);
    check(at == pressed + 1 && !empty(), m);
    runBeats(2);
    for (int t = 0; t < 2; t++) {
      snprintf(m, sizeof m, "track %d after the quick change", t + 1);
      if (tracks[t].state == TrackState::Playing) checkSync(t, m);   // (muted ones are silent)
    }
  }

  printf("declicking: loop and range wraps, mute, clip switch, stop\n");
  {
    if (!transport.running) transportStart();
    for (int t = 5; t < 7; t++) clearTrack(t);
    // Sines that don't fit their loops: every wrap would be a jump.
    auto recordSine = [&](int t, double hz, double beats) {
      feedRamp = false;
      feedSineHz = hz;
      runBeats(1.5);                               // (a recording can start up to ~250 ms back)
      recordPress(t);
      for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
      runBeats(beats);
      recordPress(t);
      runUntilIdle();
      feedSineHz = 0;
    };
    recordSine(6, 101, 3.6);                       // 1 bar
    recordSine(5, 331, 3.6);
    engine.monitor = false;
    for (auto &x : tracks) x.gain = 0;
    tracks[6].gain = 1;
    for (int i = 0; i < 10; i++) block();
    std::vector<int16_t> out;
    auto capture = [&] { afterAudio = [&] { out.insert(out.end(), outL.data, outL.data + AUDIO_BLOCK_SAMPLES); }; };
    // Largest step between neighbouring samples, against the sine's own
    // steepest (amplitude x 2 pi f / rate).
    int level = 0;                                 // the sines' peak level at the output
    auto steps = [&](double hz, const char *what) {
      afterAudio = nullptr;
      int peak = 0, step = 0;
      for (size_t i = 1; i < out.size(); i++) {
        peak = std::max(peak, abs((int)out[i]));
        step = std::max(step, abs(out[i] - out[i - 1]));
      }
      if (!level) level = peak;
      peak = level;
      double natural = peak * 2 * M_PI * hz / AUDIO_SAMPLE_RATE_EXACT;
      char m[128];
      snprintf(m, sizeof m, "%s: largest step %d, the sine's own %.0f", what, step, natural);
      check(peak > 1000 && step <= 2 * natural + 2, m);
      out.clear();
    };
    capture();
    runBeats(9);                                   // two loop wraps
    steps(101, "loop wraps (into the pre-roll)");
    setRange(6, 1, 2);                             // 2 beats from beat 2: wraps into beat 1's end
    runBeats(3);
    capture();
    runBeats(6);
    steps(101, "range wraps");
    capture();
    runBeats(0.25);                                // (from a little before)
    setTrackMuted(6, true);
    runBeats(0.5);
    steps(101, "muting");
    setTrackMuted(6, false);
    runBeats(1);
    TrackUnit u = unitOf(6);
    u.sample = tracks[5].sample;
    u.rangeStart = u.rangeLen = u.rangePhase = 0;
    capture();
    setTrackUnit(6, u);
    runBeats(2);
    steps(331, "switching to another sample");
    capture();
    runBeats(0.25);
    transportStop();
    for (int i = 0; i < 10; i++) block();
    steps(331, "stopping the transport");
    transportStart();
    for (auto &x : tracks) x.gain = 1;
    engine.monitor = true;
    feedRamp = true;
  }

  printf("resampling: the output recorded, then a scene of its own\n");
  {
    if (!transport.running) transportStart();
    feedRamp = false;                              // no input: the output is the tracks
    for (int t = 1; t < NUM_TRACKS; t++) if (recording.mode == Recording::Idle) clearTrack(t);
    if (tracks[0].state != TrackState::Playing) recordTrack(0, 7.5, 2, "  (track 1 recorded to play under it)");
    feedRamp = true;
    recordTrack(1, 3.5, 1, "  (track 2 recorded, then muted: it comes along to the new scene)");
    setTrackMuted(1, true);
    uint32_t mutedSample = tracks[1].sample;
    feedRamp = false;
    runBeats(4);
    // The output, by transport sample, while it records.
    std::vector<int16_t> outLog(44100 * 30);
    uint32_t logFrom = transport.now;
    afterAudio = [&] {
      uint32_t s0 = transport.now - AUDIO_BLOCK_SAMPLES;
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        if (s0 + i - logFrom < outLog.size()) outLog[s0 + i - logFrom] = outL.data[i];
    };
    check(recordPress(6, true) && recording.mode == Recording::WaitBar && recording.resample,
          "resample on track 7: waits for the next bar line");
    for (int i = 0; i < 4000 && recording.mode == Recording::WaitBar; i++) block();
    check(recording.mode == Recording::Running && recording.startSample == transport.lastBarSample,
          "starts on the bar line (no latency: it's the output)");
    uint32_t start = recording.startSample;
    runBeats(5.5);
    recordPress(6);                                // carries on to 2 bars
    check(recording.mode == Recording::Finishing && (silencedTracks & 1) && !(silencedTracks & (1 << 6)),
          "stopped: track 1 goes quiet where the loop ends");
    runUntilIdle();
    afterAudio = nullptr;
    bool same = true;
    for (uint32_t f : {0u, 1000u, 30000u, 100000u, tracks[6].recordedFrames - 1})
      if (wavLeft(6, f) != outLog[start + f - logFrom]) same = false;
    check(tracks[6].state == TrackState::Playing && tracks[6].loopBars == 2 && same && outLog[start + 1000 - logFrom] != 0,
          "track 7 holds 2 bars of what was played");
    check(takeResampled() == 6 && takeResampled() == -1, "reported once as resampled");
    uint32_t sample = tracks[6].sample;
    int count = sceneCount(), pos = sceneCurrent();
    check(sceneFromResample(6) && sceneCount() == count + 1 && sceneCurrent() == pos + 1,
          "a new scene inserted after the current one, and selected");
    waitScene();
    runBeats(2);
    bool othersEmpty = true;
    for (int t = 0; t < NUM_TRACKS; t++) if (t != 6 && t != 1 && tracks[t].state != TrackState::Empty) othersEmpty = false;
    check(tracks[6].sample == sample && tracks[6].state == TrackState::Playing && tracks[6].live && othersEmpty &&
          silencedTracks == 0, "in it: the resampling, on the same track, and nothing unmuted");
    check(tracks[1].sample == mutedSample && tracks[1].state == TrackState::Muted, "the muted clip came along, still muted");
    setTrackMuted(1, false);
    runBeats(2);
    checkSync(1, "the muted clip unmuted: heard (not left silenced)");
    setTrackMuted(1, true);
    checkSync(6, "the resampling, playing on");
    check(sceneStep(-1), "back to the scene before");
    waitScene();
    runBeats(2);
    check(tracks[0].state == TrackState::Playing && tracks[6].state == TrackState::Empty &&
          tracks[1].sample == mutedSample && tracks[1].state == TrackState::Muted,
          "the scene before: as it was (the muted clip too), without the resampling");
    feedRamp = true;
  }

  printf("recordings named after the project and the track: XYNNNN.wav\n");
  {
    lastAction = "naming";
    clearTrack(3);
    recordTrack(3, 4.5, 1, "  (track 4 recorded)");
    uint32_t n = tracks[3].sample;
    char want[16];
    snprintf(want, sizeof want, "%c%d%04u.wav", 'A' + projectCurrent(), NUM_TRACKS - 3, (unsigned)n);
    std::string before = sampleFile(n);
    transportStop();
    runUntilIdle();
    for (int i = 0; i < 40; i++) serviceStorage();   // cut down to size and named, stopped
    std::string after = sampleFile(n);
    char m[160];
    snprintf(m, sizeof m, "while playing still %s; once stopped %s (project letter, track number on the screen)",
             before.substr(before.rfind('/') + 1).c_str(), after.substr(after.rfind('/') + 1).c_str());
    check(before.size() >= 8 && !strcmp(before.c_str() + before.size() - 8, want + 2) && after.size() >= 10 &&
          !strcmp(after.c_str() + after.size() - 10, want), m);
    transportStart();
    runBeats(4);
    checkSync(3, "track 4 playing on from the renamed file");
    transportStop();
    runUntilIdle();
  }

  printf("projects: a new one, back, cut and paste\n");
  {
    lastAction = "projects";
    if (transport.running) transportStop();
    runUntilIdle();
    int scenes = sceneCount(), pos = sceneCurrent();
    uint32_t sample0 = tracks[0].sample;
    check(projectCurrent() == 0 && projectFilled(0) && !projectFilled(1), "project 1 loaded (the first version's scenes), slot 2 empty");
    projectSetSelection(3, 1);
    check(projectLoad(1) && projectCurrent() == 1 && projectFilled(1) && sceneCount() == 0 && sceneCurrent() == 0,
          "an empty slot: a new project, on its empty scene");
    bool empty = true;
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) empty = false;
    int row, cell;
    projectSelection(row, cell);
    check(empty && row == -1 && !projectHasClips(), "its tracks empty, no clips, no selection saved");
    check(projectLoad(0) && sceneCount() == scenes && sceneCurrent() == pos && tracks[0].sample == sample0,
          "back to project 1: its scenes and tracks");
    check(projectHasClips(), "project 1 has clips");
    projectSelection(row, cell);
    check(row == 3 && cell == 1, "with its selection");
    check(projectCut(1) && !projectFilled(1) && projectClipboardFull(), "project 2 cut: its slot empty");
    check(projectPaste(5) && projectFilled(5) && !projectPaste(5), "pasted into slot 6 (once: it's full then)");
    check(projectCut(0) && projectCurrent() == -1 && !projectFilled(0) && sceneCount() == 0 && !sceneStep(1),
          "the loaded one cut: its slot empty, the empty dummy project loaded (no N+1* with no scenes)");
    check(projectPaste(2) && projectLoad(2) && sceneCount() == scenes && sceneCurrent() == pos && tracks[0].sample == sample0,
          "pasted into slot 3 and loaded: project 1 as it was");
    check(projectPaste(7), "and into slot 8 too (the same clips)");
    // A track's position at the clock's 1.1.1 is its start position.
    auto fromStart = [&](const char *what) {
      bool ok = true;
      int32_t origin = transportOrigin();
      for (int t = 0; t < NUM_TRACKS; t++) {
        const Track &tr = tracks[t];
        if (tr.state == TrackState::Empty) continue;
        uint32_t sp = startPosition(tr.loopBars * BEATS_PER_BAR, tr.rangeStartBeat, tr.rangeBeats);
        int32_t phase = pmod(4 * origin - tr.rangeAnchorQ, 4 * tr.rangeBeats);
        if (phase != pmod(4 * ((int32_t)sp - (int32_t)tr.rangeStartBeat), 4 * tr.rangeBeats)) ok = false;
      }
      check(ok && origin % BEATS_PER_BAR == 0, what);
    };
    transportStart();
    check(projectsMayCopy(), "cut and paste work while playing");
    check(projectLoad(5), "loading while playing");
    waitScene();
    runBeats(2);
    empty = true;
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) empty = false;
    check(empty, "on the next bar line: its (empty) scene");
    check(projectLoad(2), "and back");
    waitScene();
    runBeats(2);
    check(tracks[0].sample == sample0 && tracks[0].state != TrackState::Empty, "project 3's scene playing again");
    check(transportOrigin() > 0, "the clock counts from the switch's bar line");
    fromStart("its tracks from their 1.1.1 positions there");
    runBeats(5.5);
    check(projectLoad(7), "loading a project with the same clips, mid-bar");
    waitScene();
    runBeats(2);
    fromStart("the same clips start over from 1.1.1 too");
    for (int t = 0; t < NUM_TRACKS; t++)
      if (tracks[t].state == TrackState::Playing) checkSync(t, "after the switch");   // (muted ones are silent)
    check(projectCurrent() == 7 && projectCut(2) && !projectFilled(2) && projectCopying(2) && !projectsMayCopy() && !projectLoad(2),
          "cutting another project while playing: its slot empty at once, being copied");
    for (int i = 0; i < 2000 && projectCopying(2); i++) block();
    check(!projectCopying(2) && projectClipboardFull(), "copied in the background, a block at a time");
    check(projectPaste(2) && projectFilled(2), "pasted back while playing");
    for (int i = 0; i < 2000 && projectCopying(2); i++) block();
    check(!projectCopying(2), "copied");
    for (int t = 0; t < NUM_TRACKS; t++)
      if (tracks[t].state == TrackState::Playing) checkSync(t, "while copying");   // (muted ones are silent)
    transportStop();
    runUntilIdle();
    for (int i = 0; i < 20; i++) serviceScenes();
    storageBegin([](const char *) {});
    scenesBegin(simTempo, simSetTempo, 2, simNewTempo);
    check(projectCurrent() == 2 && sceneCount() == scenes && projectFilled(5) && projectFilled(2) && !projectFilled(1) && projectClipboardFull(),
          "after a power cycle: the slots as they were");
    {
      uint32_t before = sampleCount(), playing = tracks[0].sample;
      int deleted = deleteUnusedSamples();
      bool kept = sampleExists(playing) && tracks[0].state != TrackState::Empty;
      char m[120];
      snprintf(m, sizeof m, "unused recordings deleted (%d of %u), the ones in use kept", deleted, before);
      check(deleted > 0 && sampleCount() == before - deleted && kept, m);
      check(deleteUnusedSamples() == 0, "a second time: nothing more");
      transportStart();
      runBeats(4);
      checkSync(0, "playing on after the clean-up");
      transportStop();
      runUntilIdle();
    }
  }

  printf("projects: one tempo each; new ones take the last one set by hand\n");
  {
    lastAction = "project tempo";
    auto at = [](float b) { return fabsf(simTempo() - b) < 0.05f; };
    check(projectLoad(2) && sceneCount() >= 2, "project 3");
    tempoByHand(133);
    for (int i = 0; i < 5; i++) block();
    int pos = sceneCurrent();
    check((sceneStep(-1) || sceneStep(1)) && sceneCurrent() != pos && at(133), "another scene: the same tempo");
    check(sceneGoEmpty() && at(133), "the empty scene too");
    check(projectLoad(3) && sceneCount() == 0 && at(133), "a new project (slot 4): the tempo last set by hand");
    tempoByHand(97);
    check(projectLoad(2) && at(133), "project 3 again: its own tempo");
    check(projectLoad(4) && at(97), "another new project (slot 5): 97, the last set by hand (loading project 3 didn't count)");
    check(projectLoad(2) && at(133), "and project 3");
    for (int i = 0; i < 20; i++) serviceScenes();
    simSetTempo(120);                              // (power off: the tempo forgotten)
    for (int i = 0; i < 5; i++) block();
    storageBegin([](const char *) {});
    scenesBegin(simTempo, simSetTempo, 2, simNewTempo);
    for (int i = 0; i < 5; i++) block();
    check(projectCurrent() == 2 && at(133), "after a power cycle: project 3's tempo");
    check(projectDelete(3) && projectDelete(4), "(the new ones deleted)");
  }

  printf("a project exported as an Ableton Live Set\n");
  {
    lastAction = "export";
    check(projectLoad(2), "project 3");
    for (int i = 0; i < 10 && sceneCurrent() != 2; i++) sceneStep(sceneCurrent() < 2 ? 1 : -1);   // (stopped: at once)
    check(sceneCurrent() == 2, "on its scene 2");
    int scenes = sceneCount();
    int playing = 0;                               // its tracks with a clip there: saved as playing
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) playing++;
    int first = exportProject(2), second = exportProject(2);
    std::string dir = simRoot + "/Ableton/L7 C00" + std::to_string(first);
    char m[200];
    snprintf(m, sizeof m, "exported as L7 C%03d, then L7 C%03d (the next number for its letter)", first, second);
    check(first >= 1 && second == first + 1, m);
    // The set: valid gzip and XML, 7 tracks with two Auto Filters each, a clip
    // slot per scene on each, the scenes; every clip's sample copied.
    std::string py = "python3 -c \"import gzip,sys,os,xml.etree.ElementTree as E; d=sys.argv[1]; r=E.fromstring(gzip.open(d+'/'+os.path.basename(d)+'.als').read()); "
                     "t=r.find('LiveSet/Tracks'); s=len(r.find('LiveSet/Scenes')); "
                     "ok=len(t)==7 and all(len(x.findall('.//Devices/AutoFilter'))==2 for x in t) and all(len(x.find('DeviceChain/MainSequencer/ClipSlotList'))==s for x in t); "
                     "c=[f.get('Value') for f in r.iter('RelativePath') if f.get('Value').startswith('Samples/')]; "
                     "ok=ok and all(os.path.exists(d+'/'+p) for p in c); pl=[x.find('SavedPlayingSlot').get('Value') for x in t]; "
                     "print(s, len(c), sum(v=='1' for v in pl), sum(v not in ('1','-2') for v in pl)); sys.exit(0 if ok else 1)\" '" + dir + "'";
    FILE *out = popen(py.c_str(), "r");
    int s = -1, clips = -1, saved = -1, other = -1;
    if (out) { if (fscanf(out, "%d %d %d %d", &s, &clips, &saved, &other) != 4) s = -1; }
    int rc = out ? pclose(out) : -1;
    snprintf(m, sizeof m, "the set opens as gzip and XML: 7 tracks, two Auto Filters each, %d scene(s) (%d in the project), %d clip(s), their samples copied",
             s, scenes, clips);
    check(rc == 0 && s == scenes && clips > 0, m);
    snprintf(m, sizeof m, "scene 2's clips saved as playing: %d track(s) (%d have a clip there), %d other", saved, playing, other);
    check(saved == playing && other == 0 && playing > 0, m);
  }

  printf("random sessions\n");
  for (unsigned seed = 1; seed <= (argc > 1 ? atoi(argv[1]) : 5); seed++) fuzz(600, seed);

  printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
