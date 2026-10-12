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
#include "colors.h"
#include "midi_in.h"
#include "audio_follow.h"

using namespace looper;

uint32_t simMicros = 0;
void (*simDelayHook)(uint32_t us) = nullptr;
std::string simRoot;
SDClass SD;
SerialStub Serial;
bool simNoRun = false;
int simFailIo = 0, simReinits = 0, simFsChanges = 0, simFileCreates = 0, simSampleTruncates = 0;
void (*simSdLatency)(size_t, bool) = nullptr;
void (*IntervalTimer::fn)() = nullptr;
HardwareSerial midi;

static LooperEngine engine;
static audio_block_t inL, inR, outL, outR;
static int16_t usbOut[AUDIO_BLOCK_SAMPLES * USB_OUT_CHANNELS];   // to the computer (the engine's usbAudioWrite)
static bool usbStreaming = true;                    // (the computer taking it)
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
static int16_t feedUsb = 0;                         // USB audio in: this level (0: none coming in)
static int32_t feedUsbClick = -1;                   // USB audio in: a click this many samples after each beat line (-1: none)
static bool feedNoise = false;                      // the line input: noise only (overrides the feed below)
// The input: a drum pattern (audio follow's tests): a kick on every beat, a
// snare on 2 and 4, a closed hat on the eighths, over a quiet pad; at drumBpm
// (0: none), drumBeat its position in beats; each beat's kick (its input sample) in drumKicks.
static double drumBpm = 0, drumBeat = 0;
static bool drumClicks = false;                      // (instead: a click on each beat but the bar's first, a 2 kHz burst dying in ~3 ms)
static bool drumsOverUsb = false;                    // (the drums from the computer, USB audio, the line input silent)
static float drumLevel = 1;
static std::vector<uint32_t> drumKicks;
static uint32_t drumNoise = 12345;
static float drumSample(uint32_t at) {
  double prev = drumBeat;
  drumBeat += drumBpm / 60 / AUDIO_SAMPLE_RATE_EXACT;
  if (floor(drumBeat) != floor(prev)) drumKicks.push_back(at);
  const double secPerBeat = 60 / drumBpm;
  double inBeat = (drumBeat - floor(drumBeat)) * secPerBeat, inEighth = fmod(drumBeat * 2, 1.0) * secPerBeat / 2;
  int beat = (int)floor(drumBeat);
  if (drumClicks)                                    // (none on a bar's first beat: by a loop's wrap, crossfaded there)
    return beat % BEATS_PER_BAR == 0 ? 0 : 0.8f * sinf(2 * (float)M_PI * 2000 * (float)inBeat) * expf(-(float)inBeat / 0.003f) * drumLevel;
  drumNoise = drumNoise * 1664525u + 1013904223u;
  float noise = (int32_t)drumNoise / 2147483648.0f;
  float v = 0.6f * sinf(2 * (float)M_PI * 55 * (float)inBeat) * expf(-(float)inBeat / 0.08f);   // kick
  if (beat % 2 == 1) v += expf(-(float)inBeat / 0.05f) * (0.35f * noise + 0.2f * sinf(2 * (float)M_PI * 190 * (float)inBeat));   // snare
  static float lastNoise = 0;
  v += 0.12f * (noise - lastNoise) * expf(-(float)inEighth / 0.012f);   // hat (the noise's highs)
  lastNoise = noise;
  v += 0.04f * sinf(2 * (float)M_PI * 220 * at / AUDIO_SAMPLE_RATE_EXACT);   // a pad
  return v * drumLevel;
}
static uint32_t usbIn[AUDIO_BLOCK_SAMPLES];        // from the computer this block (left | right << 16)
static bool usbInOn = false;
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
  serviceUsbLatency();                             // (as loop() does)
  audioFollowService();
}

// The app's tempo, for scenes: simSetTempo as a project loads, tempoByHand as
// the app's setTempo (the project keeps it, new projects start with it).
static float simTempo() { return bpmForSpb(transport.pendingSpb ? transport.pendingSpb : transport.spb); }
static bool simSetTempo(float b) {
  uint32_t spb = spbForBpm(b);
  if (spb != (transport.pendingSpb ? transport.pendingSpb : transport.spb)) transport.pendingSpb = spb;   // (the same: nothing, as the app)
  return true;
}
// The app's followTempo() (an external MIDI clock, midi_in.cpp): the spb as is, the tempo shown kept.
static float simShownBpm = 0;
static int simTempoChanges = 0;
void followTempo(uint32_t spb, float shownBpm) {
  if (spb != (transport.pendingSpb ? transport.pendingSpb : transport.spb)) transport.pendingSpb = spb, simTempoChanges++;
  simShownBpm = shownBpm;
  bpm = shownBpm;                                  // (as the app's: the tempo shown, its no-flicker margin measured from it)
}
float bpm = 120.0f;
static float simNewBpm = 120.0f;
static float simNewTempo() { return simNewBpm; }
static void tempoByHand(float b) {
  simSetTempo(b);
  simNewBpm = b;
  projectTempoChanged();
}
// And the master volume (the app's stepMasterVolume(): one for all projects).
static void volumeByHand(int v) { setMasterVolume(v); }

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
  if (drumBpm > 0)
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      int16_t v = (int16_t)constrain(lroundf(drumSample(inputIndex + i) * 32767), -32767L, 32767L);
      if (drumsOverUsb) usbIn[i] = (uint32_t)(uint16_t)v | (uint32_t)(uint16_t)v << 16, inL.data[i] = inR.data[i] = 0;
      else inL.data[i] = inR.data[i] = v;
    }
  if (feedNoise)                                   // (the line input: a little noise, as with nothing plugged in)
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) inL.data[i] = inR.data[i] = (int16_t)(rand() % 21 - 10);
  engine.simIn[0] = &inL; engine.simIn[1] = &inR;
  engine.simSetInput(0, &inL); engine.simSetInput(1, &inR);
  if (feedUsb || feedUsbClick >= 0) {             // USB audio from a computer: a constant level, both channels, or a click after each beat
    auto both = [](int16_t v) { return (uint32_t)(uint16_t)v | (uint32_t)(uint16_t)v << 16; };
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) usbIn[i] = both(feedUsb);
    if (feedUsbClick >= 0 && transport.running)
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        int32_t in = (int32_t)((transport.sampleInBeat + i) % transport.spb) - feedUsbClick;   // (samples into the click)
        if (in >= 0 && in < 300) usbIn[i] = both((int16_t)(20000 * exp(-in / 30.0) * (in % 2 ? -1 : 1)));
      }
  }
  usbInOn = feedUsb || feedUsbClick >= 0 || (drumsOverUsb && drumBpm > 0);
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

// Sample id's file on the host, where it is now ("XYNNNN.wav" once named).
static std::string hostFile(uint32_t id) {
  char path[64];
  sampleFile(id, path, sizeof path);
  return simRoot + path;
}

// Reads frame `frame` of track t's sample (left channel).
static int16_t wavLeft(int t, uint32_t frame) {
  std::string path = hostFile(tracks[t].sample);
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
static uint32_t silentId = 0;                    // (the silent recording: DELETE UNUSED SAMPLES at the end)
static void checkSync(int t, const char *label) {
  feedRamp = false;
  inputMonitored = false;
  for (auto &tr : tracks) tr.gain = 0;
  tracks[t].gain = 1;
  for (int i = 0; i < 6; i++) block();             // the other tracks' levels glide down (~1 ms time constant)
  for (int i = 0; i < 2000 && !limiterResting(); i++) block();   // (the master limiter let go, after a loud mix)
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
  inputMonitored = true;
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
    if (int r = takeResampled(); r >= 0 && sceneFromResample(r) < 0) endSilencing();   // (as followRecording())
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
  for (auto &tr : tracks) tr.stretch = false;      // (the checks below count on exact frames: resampled; stretching has its own)
  usbAudioRead = [](uint32_t *frames) {
    if (usbInOn) memcpy(frames, usbIn, sizeof usbIn);
    return usbInOn;
  };
  usbAudioWrite = []() -> int16_t * { return usbStreaming ? usbOut : nullptr; };
  usbAudioWritten = [] {};
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
    snprintf(m, sizeof m, "track 5 now plays sample %04u (track 4's empty file went to recording space): %u frames, %u bar, range %u+%u, muted %d",
             tr.sample, tr.recordedFrames, tr.loopBars, tr.rangeStartBeat, tr.rangeBeats, tr.state == TrackState::Muted);
    check(tr.sample == 1 && tr.recordedFrames == legacyFrames && tr.loopBars == 1 && tr.rangeBeats == 2 &&
          tr.state == TrackState::Muted && tr.overview[10] == 100, m);
    check(tr.filterLo == 3 && tr.filterHi == 20 && tr.volume == 14 && tr.balance == -5,
          "its filter kept, volume and balance in today's steps (25 -> 14, -4 -> -5)");
    check(wavLeft(4, 0) == -500 && wavLeft(4, 1234) == 1234 % 1000 - 500 && wavLeft(4, legacyFrames - 1) == (int16_t)((legacyFrames - 1) % 1000 - 500),
          "its audio is intact in SAMPLES/0001.wav");
    struct stat st;
    check(::stat((simRoot + "/looper").c_str(), &st) != 0 && ::stat((simRoot + "/L7/SAMPLES").c_str(), &st) == 0,
          "the app's folder moved from /looper to /L7");
    FILE *old = fopen((simRoot + "/L7/track5.wav").c_str(), "rb");
    check(!old && sampleCount() == 1, "the old track file is gone; one sample on the card");
    if (old) fclose(old);
    FILE *space = fopen((simRoot + "/L7/DATA/0001.DAT").c_str(), "rb");
    check(space != nullptr, "track 4's empty file is recording space, named from the pool's numbers (DATA/0001.DAT)");
    if (space) fclose(space);
    {                                              // converted (no spectrum profile yet): measured while stopped, into its header and the track
      const bool before = profileOf(tr.overview)[0] == 'S';
      for (int i = 0; i < 200 && profileOf(tr.overview)[0] != 'S'; i++) serviceStorage();
      SampleDetails sd;
      check(!before && profileOf(tr.overview)[0] == 'S' && sampleDetails(tr.sample, sd) && !memcmp(sd.profile, profileOf(tr.overview), PROFILE_BYTES),
            "the converted recording's spectrum profile: measured while stopped, written into its header, the track's too");
    }
    clearTrack(4);
    flushHeaders();
  }
  transport.spb = spbForBpm(120);
  int fsChangesAtStart = simFsChanges;
  midiBegin(&midi);

  recordTrack(0, 4 * 4 + 0.1, 4, "track 1: stop 4 bars + 0.1 beats in (50 ms: within the 100 ms grace), from stopped (trims to 4 bars)");
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
    inputMonitored = false;
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
    inputMonitored = true;
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

    // Slide it one beat right: the playhead moves with the start (as far into
    // the new range as it was into the old one: a beat further into the file).
    Track &tr = tracks[0];
    for (int slide = 0; slide < 3; slide++) {
      uint32_t oldStart = tr.rangeStartBeat;
      int32_t oldAnchor = tr.rangeAnchorQ;
      setRange(0, oldStart + 1, 4);
      int32_t sw = tr.switchQ;
      uint32_t due = 4 * oldStart + pmod(sw - oldAnchor, 16) + 4;      // file quarter the old range would play, a beat on
      runBeats(8);
      uint32_t got = 4 * tr.rangeStartBeat + pmod(sw - tr.rangeAnchorQ, 16);
      char m[128];
      snprintf(m, sizeof m, "slide (switch at beat %.2f): plays file beat %.2f (expected %.2f: moved with the start)",
               sw / 4.0, got / 4.0, due / 4.0);
      check(got == due, m);
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
    inputMonitored = true;
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
    inputMonitored = true;
  }

  printf("moving an empty track (B + UP/DOWN): track 3 (empty) and track 2 swap while playing\n");
  {
    uint32_t sample = tracks[1].sample;
    setTrackFilter(2, 10, 30);                     // (the empty track's own settings go with it)
    trackChanged(2);
    uint32_t edits = projectEdits();
    check(swapTracks(2, 1), "swapped");
    check(unitOf(2).sample == sample && unitOf(1).sample == 0, "on its way: track 3 gets the clip, track 2 the empty unit");
    check(projectEdits() != edits, "the scene follows");
    runBeats(2);
    check(tracks[2].sample == sample && tracks[2].state != TrackState::Empty && tracks[1].state == TrackState::Empty &&
          tracks[1].filterLo == 10 && tracks[1].filterHi == 30, "done: the clip on track 3, track 2 empty with track 3's filter");
    runBeats(4);
    checkSync(2, "the clip moved to track 3, in step with the clock");
    check(!swapTracks(2, 2), "not with itself");
    check(swapTracks(1, 2), "and back");
    runBeats(2);
    check(tracks[1].sample == sample && tracks[2].state == TrackState::Empty, "back: the clip on track 2 again");
    setTrackFilter(2, 0, FILTER_MAX);
    trackChanged(2);
    for (auto &x : tracks) x.gain = 1;
    inputMonitored = true;
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
    for (int i = 0; i < (int)sampleCount(); i++) {
      std::string f = hostFile(sampleAt(i));
      names.push_back({f.substr(f.rfind('/') + 1), sampleAt(i)});
    }
    // (In L7's order, as sampleAt() has them: today's "A001" names before the older "A10012" ones of a project.)
    size_t count = names.size();
    auto at = [&](long i) { return names[((i % (long)count) + count) % count].second; };
    // The project's own recordings: today's names, "A001.wav" (project A).
    const char letter = (char)('A' + projectCurrent());
    auto own = [&](long i) { const std::string &f = names[i].first; return f.size() == 8 && f[0] == letter && f[1] >= '0' && f[1] <= '9'; };
    long first = 0, last = -1;                     // its own first, its own last
    while (first < (long)count && !own(first)) first++;
    for (long i = 0; i < (long)count; i++) if (own(i)) last = i;
    setTrackMix(6, 6, 4);
    check(stepSample(6, 1) && tr.sample == at(first), "SHIFT+RIGHT on an empty track: the first of its own, or the next name");
    check(stepSample(6, 1) && tr.sample == at(first + 1), "SHIFT+RIGHT: the next name (its own first, then the project's other tracks, then other projects)");
    check(stepSample(6, -1) && tr.sample == at(first), "SHIFT+LEFT: back to the previous");
    for (size_t i = 0; i < count; i++) stepSample(6, 1);
    check(tr.sample == at(first), "SHIFT+RIGHT through all of them: round to the same one (wrapping)");
    check(stepSample(6, 1) && tr.sample == at(first + 1) && tr.rangeStartBeat == 0 && tr.rangeBeats == tr.loopBars * 4 &&
          tr.volume == 6 && tr.balance == 4, "range reset to the whole loop, volume and balance kept");
    setTrackMix(6, VOLUME_UNITY, 0);               // the sync check compares at unity gain
    trackChanged(6);
    runBeats(4);
    for (auto &x : tracks) x.gain = 0;
    tr.gain = 1;
    checkSync(6, "track 7 playing its new sample");
    clearTrack(6);
    check(stepSample(6, -1) && tr.sample == at(first + 1), "SHIFT+LEFT on an empty track: the project's sample last touched");
    clearTrack(6);
    check(stepSample(6, 1) && tr.sample == at(first + 2), "SHIFT+RIGHT on an empty track: the one after the last touched");
    clearTrack(6);
    uint32_t keep = touchedSample[projectCurrent()];
    touchedSample[projectCurrent()] = 0;           // (none touched yet: its own)
    check(stepSample(6, -1) && tr.sample == at(last), "SHIFT+LEFT on an empty track, nothing touched: the last of its own, or the name before");
    touchedSample[projectCurrent()] = keep;
    clearTrack(6);
    char m[64];
    snprintf(m, sizeof m, "%zu recordings on the card", count);
    check(count == 4, m);
    for (auto &x : tracks) x.gain = 1;
    inputMonitored = true;
  }

  printf("silent recordings: stepped onto by SHIFT+LEFT/RIGHT, deleted (and taken off the tracks) by DELETE UNUSED SAMPLES\n");
  {
    if (!transport.running) transportStart();
    feedRamp = false; feedSineHz = 0;
    runBeats(1.5);
    recordPress(4);
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    runBeats(3.6);
    recordPress(4);
    runUntilIdle();
    uint32_t empty = tracks[4].sample;
    check(empty && tracks[4].state != TrackState::Empty, "a bar of silence recorded");
    clearTrack(4);
    bool landed = false;
    for (int i = 0; i < (int)sampleCount() + 2 && !landed; i++) { stepSample(4, 1); runBeats(0.5); landed |= unitOf(4).sample == empty; }
    check(landed && sampleExists(empty), "playing: stepped onto (silent ones not skipped)");
    int changes = simFsChanges;
    transportStop();
    for (int i = 0; i < 20; i++) serviceStorage();
    uint32_t ids[16];
    int ns = silentSamples(ids, 16);
    bool found = false;
    for (int i = 0; i < ns; i++) found |= ids[i] == empty;
    check(found, "found silent");
    silentId = empty;                              // (deleted at the end: DELETE UNUSED SAMPLES would empty other tests' scenes)
    clearTrack(4);
    clearTrack(4);
    for (int i = 0; i < 20; i++) serviceStorage();
    fsChangesAtStart += simFsChanges - changes;    // (stopped: the file system may change)
    feedRamp = true;
    transportStart();
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
    runBeats(4.08);                                // (just past the bar line: within the 100 ms grace)
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
    inputMonitored = true;
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
    check(sceneCut() && waitScene() && sceneCount() == 1 && sceneCurrent() == 2 && tracks[1].state == TrackState::Playing,
          "cutting it there and then: back on 2*, the copy of scene 1");
    tracks[1].state = TrackState::Muted;           // (made again)
    trackChanged(1);
    block();
    check(sceneCount() == 2 && sceneCurrent() == 2, "(changed again: scene 2)");
    check(sceneStep(-1) && waitScene() && tracks[1].state == TrackState::Playing, "scene 1 keeps track 2 playing");
    check(sceneStep(1) && waitScene() && tracks[1].state == TrackState::Muted, "scene 2 has it muted");
    check(sceneCut() && waitScene() && sceneClipboardFull() && sceneCount() == 1 && sceneCurrent() == 1 &&
          tracks[1].state == TrackState::Playing, "SHIFT+B+A on scene 2: cut, back on scene 1");
    check(scenePaste() && waitScene() && sceneCount() == 2 && sceneCurrent() == 2 && tracks[1].state == TrackState::Muted,
          "B+A: pasted after it as scene 2");
    check(sceneDuplicate() && waitScene() && sceneCount() == 3 && sceneCurrent() == 3 && tracks[1].state == TrackState::Muted,
          "SHIFT+B twice: a copy as scene 3");
    check(sceneGoEnd() && waitScene() && sceneCurrent() == sceneCount() + 1, "SHIFT+UP twice: straight to N+1*");
    check(sceneNext() && sceneSwitchBeat() > transport.beat && waitScene() &&
          sceneCurrent() == 1 && sceneSwitchBeat() == INT32_MIN,
          "the next scene action on N+1*: to 1 (meanwhile its switch beat known, the click silent until it; then none)");
    check(sceneNext() && waitScene() && sceneCurrent() == 2, "then: 2");
    while (sceneCurrent() < sceneCount()) sceneNext(), waitScene();
    check(sceneCurrent() == sceneCount() && sceneNext() && waitScene() && sceneCurrent() == 1,
          "on the last: round to 1 (N+1* and 0* left out)");
    check(sceneGoEmpty() && waitScene() && sceneNext() && waitScene() && sceneCurrent() == 1, "on 0*: to 1");
    check(sceneGoEnd() && waitScene() && sceneCurrent() == sceneCount() + 1, "(back to N+1*)");
    check(sceneGoEmpty() && waitScene() && sceneCurrent() == 0 && empty(), "SHIFT+DOWN twice: straight to 0*");
    setTrackMix(6, 6, 0);                          // a change on 0*
    trackChanged(6);
    block();
    check(sceneCurrent() == 1 && sceneCount() == 4 && tracks[6].volume == 6, "changing 0* inserts a new scene 1");
    // That scene has no clips: cutting it removes it, but the clipboard keeps the scene cut before (pasted back, it has track 2 muted).
    check(sceneCut() && waitScene() && sceneCount() == 3 && sceneCurrent() == 0 && sceneClipboardFull(), "cutting a scene with no clips: it's gone, the clipboard still full");
    check(scenePaste() && waitScene() && sceneCount() == 4 && sceneCurrent() == 1 && tracks[0].sample == s1 && tracks[1].state == TrackState::Muted,
          "the clipboard still holds the scene cut before it");
    check(sceneGoEmpty() && waitScene() && sceneCurrent() == 0 && sceneCut() && sceneCurrent() == 0 && sceneCount() == 4 && !sceneClipboardFull(),
          "cutting 0*: it stays, the clipboard emptied");
    check(scenePaste() && waitScene() && sceneCurrent() == 1 && sceneCount() == 5 && empty(),
          "pasting with the clipboard empty: an empty scene after it (scene 1)");
    check(sceneCut() && waitScene() && sceneCount() == 4 && sceneCurrent() == 0, "(that empty scene cut again)");
    check(sceneStep(1) && waitScene() && sceneCurrent() == 1, "(on to scene 1 again)");
    check(sceneCut() && waitScene() && sceneCount() == 3, "(the pasted scene cut again, to put the old state back)");
    check(sceneStep(1) && waitScene() && sceneCurrent() == 1, "(scene 1: the old one)");
    check(sceneStep(-1) && waitScene() && sceneCurrent() == 0 && scenePaste() && waitScene() && sceneCount() == 4, "(and back: a new scene 1 as before)");
    check(sceneStep(1) && waitScene() && sceneCurrent() == 2 && tracks[0].sample == s1 && tracks[1].state == TrackState::Playing &&
          tracks[6].volume == VOLUME_UNITY, "the others moved up: old scene 1 is now 2");
    runBeats(4);
    for (auto &x : tracks) { x.gain = 0; x.underruns = 0; }
    tracks[0].gain = 1;
    checkSync(0, "track 1 after the scene changes");
    for (auto &x : tracks) x.gain = 1;
    inputMonitored = true;
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
    if (FILE *f = fopen(hostFile(tracks[5].sample).c_str(), "r+b")) {
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
    for (int i = 0; i < (int)sampleCount(); i++) samples.push_back(sampleAt(i));
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
    check(maxLatency > 0 && maxLatency < 320 && silent == 0 && drops == 0, m);   // (~300 ms: which samples are switched to moves it a little)
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
    check(fabsf(sceneSavedTempo() - 125) < 0.05f, "scene B's tempo: the one set by hand in it (tempo per scene)");
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
    check(transport.spb == spbA, "back to A: A's own tempo (tempo per scene)");
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
    int32_t startBefore[NUM_TRACKS];                // (each one's start position, quarters into its range)
    for (int t = 0; t < NUM_TRACKS; t++) {
      beatsBefore[t] = tracks[t].rangeBeats;
      anchorBefore[t] = tracks[t].rangeAnchorQ;
      const uint32_t lb = tracks[t].loopBars * BEATS_PER_BAR;
      startBefore[t] = lb ? 4 * (int32_t)pmod((int32_t)startPosition(lb, tracks[t].rangeStartBeat, tracks[t].rangeBeats) - (int32_t)tracks[t].rangeStartBeat, lb) : 0;
    }
    check(sceneStep(1), "to scene B");
    setTrackMix(0, 6, 0);
    trackChanged(0);
    waitScene();
    for (int i = 0; i < 20; i++) block();
    afterAudio = nullptr;
    snprintf(m, sizeof m, "track 6 stops on the bar line (beat %d +%u), then empty", stop.beat, stop.sib);
    check(onBar(stop, AUDIO_BLOCK_SAMPLES) && tracks[5].state == TrackState::Empty, m);
    check(tracks[0].volume == 6, "the volume set while waiting stayed");
    // At the switch (the bar line): a range after one playing as far from its
    // start position as that one was from its own, wrapped to its length; another, from its start position.
    int sameLen = 0, fromStart = 0, wrong = 0;
    for (int t = 0; t < NUM_TRACKS; t++) {
      const Track &tr = tracks[t];
      if (tr.state == TrackState::Empty) continue;
      const uint32_t loopBeats = tr.loopBars * BEATS_PER_BAR;
      const int32_t q = 4 * stop.beat, len = 4 * tr.rangeBeats;
      const int32_t sNew = 4 * (int32_t)pmod((int32_t)startPosition(loopBeats, tr.rangeStartBeat, tr.rangeBeats) - (int32_t)tr.rangeStartBeat, loopBeats);
      const int32_t at = pmod(q - tr.rangeAnchorQ, len);   // (quarters into its range at the switch)
      if (beatsBefore[t] && anchorBefore[t] != INT32_MIN) {   // (any length: the distance wrapped to its own)
        const int32_t lenOut = 4 * (int32_t)beatsBefore[t];
        const int32_t d = pmod(pmod(q - anchorBefore[t], lenOut) - startBefore[t], lenOut);
        if (at == pmod(sNew + d, len)) sameLen++; else wrong++;
      } else {
        if (at == sNew) fromStart++; else wrong++;
      }
    }
    snprintf(m, sizeof m, "the switch: playing ranges as far from their start positions, wrapped (%d), others from them (%d), %d wrong", sameLen, fromStart, wrong);
    check(sameLen + fromStart > 0 && wrong == 0, m);
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

  printf("quick scene changes: on the next beat once SHIFT is let go\n");
  {
    auto empty = [] { for (auto &x : tracks) if (x.state != TrackState::Empty) return false; return true; };
    while (transport.beat % 4 != 0 || transport.sampleInBeat > transport.spb / 4) block();   // early in a bar
    sceneHold(true);
    check(sceneGoEmpty() && sceneHeld() && sceneCurrent() == 0 && !sceneChangeWaiting(),
          "SHIFT held, jump to 0*: held, as any step");
    for (int i = 0; i < 10; i++) block();
    check(sceneHeld() && !sceneChangeWaiting(), "still held while SHIFT is");
    int32_t pressed = transport.beat;
    sceneHold(false);
    check(!sceneHeld() && sceneCurrent() == 0 && sceneChangeWaiting(), "SHIFT let go: on its way");
    int32_t at = -1;
    afterAudio = [&] { if (at < 0 && !sceneChangeWaiting()) at = transport.beat; };
    waitScene();
    for (int i = 0; i < 5; i++) block();
    afterAudio = nullptr;
    char m[128];
    snprintf(m, sizeof m, "switched on the next beat (beat %d, let go in %d), tracks empty", at, pressed);
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
    {                                              // its peak kept, its volume scale from it (engine.h)
      const Track &tr = tracks[6];
      uint16_t pk = peakOf(tr.overview);
      float d = headroomDbOf(tr.overview);
      SampleDetails sd;
      bool read = sampleDetails(tr.sample, sd);
      char mp[200];
      snprintf(mp, sizeof mp, "a sine at 8000: its peak kept (%u; in its header: %.2f dB down, here %.2f), unity step %d, the top step's gain x%.3f (0 dBFS: x%.3f)",
               pk, read ? sd.headroomDb : -1.0f, d, unityStepOf(d), volumeGainOf(volumeStoredOf(VOLUME_MAX, d), d), 32768.0f / pk);
      check(abs((int)pk - 8000) <= 2 && read && fabsf(sd.headroomDb - d) < 0.01f && unityStepOf(d) == 13 &&
            volumeStepOf(VOLUME_UNITY, d) == 13 && fabsf(volumeGainOf(VOLUME_UNITY, d) - 1) < 1e-4f &&
            fabsf(volumeGainOf(volumeStoredOf(VOLUME_MAX, d), d) * pk / 32768.0f - 1) < 1e-3f &&
            fabsf(volumeGainOf(volumeStoredOf(15, d), d) - powf(10, 3.75f / 20)) < 1e-3f, mp);
      uint8_t quiet[OVERVIEW_BYTES] = {};
      quiet[OVERVIEW_COLS] = 60;                   // (~-55 dBFS: quiet)
      check(headroomDbOf(quiet) == 0 && unityStepOf(0) == VOLUME_MAX && volumeGainOf(VOLUME_UNITY, 0) == 1 &&
            volumeStepOf(VOLUME_OFF, 0) == 0 && volumeGainOf(VOLUME_OFF, 0) == 0, "a quiet clip: taken as peaking at 0 dBFS, its unity the top step; off is off");
    }
    {                                              // its spectrum profile (config.h), and the filters' compensation from it
      Track &tr = tracks[6];
      const uint8_t *pr = profileOf(tr.overview);
      auto level = [&](const uint8_t *q, int k) { return q[1 + k / 2] >> (k & 1 ? 4 : 0) & 15; };
      SampleDetails sd;
      bool read = sampleDetails(tr.sample, sd);
      char mp[240];
      snprintf(mp, sizeof mp, "a 101 Hz sine: profiled as it recorded, its band (80-160 Hz) the loudest, the top ones far down (%d %d %d ... %d), in its header too",
               level(pr, 1), level(pr, 2), level(pr, 3), level(pr, 9));
      check(pr[0] == 'S' && level(pr, 2) == 0 && level(pr, 9) >= 10 && read && !memcmp(sd.profile, pr, PROFILE_BYTES), mp);
      const float open = filterCompOf(0, FILTER_MAX, pr), lp = filterCompOf(0, 16, pr), hp = filterCompOf(20, FILTER_MAX, pr);
      uint8_t none[PROFILE_BYTES] = {};
      snprintf(mp, sizeof mp, "compensation (half strength): filters open x%.3f, a lowpass at 640 Hz (the sine through) x%.3f, a highpass at 1.6 kHz (gone) x%.3f (the cap, halved), not measured x%.3f",
               open, lp, hp, filterCompOf(0, 16, none));
      check(open == 1 && lp < 1.05f && fabsf(hp - powf(FILTER_COMP_MAX, FILTER_COMP_STRENGTH)) < 1e-4f && filterCompOf(0, 16, none) == 1, mp);
      const float before = tr.levelL;
      setTrackFilter(6, 20, FILTER_MAX);
      const float after = tr.comp, mixAfter = tr.levelL;
      setTrackFilter(6, 0, FILTER_MAX);
      snprintf(mp, sizeof mp, "the track's make-up follows its filters (x%.3f), and back (x%.3f), its mix as it was", after, (float)tr.comp);
      check(fabsf(after - hp) < 1e-4f && tr.comp == 1 && mixAfter == before && tr.levelL == before, mp);
      // The master limiter: the sine turned up 16 times (+24 dB) at the output,
      // its peaks held at the ceiling, never clipped.
      float gains[NUM_TRACKS];
      for (int t = 0; t < NUM_TRACKS; t++) { gains[t] = tracks[t].gain; tracks[t].gain = 0; }
      tr.gain = 16;
      runBeats(1);
      int top = 0, clipped = 0;
      afterAudio = [&] {
        for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
          top = max(top, abs((int)outL.data[k]));
          if (abs((int)outL.data[k]) >= 32767) clipped++;
        }
      };
      runBeats(2);
      afterAudio = nullptr;
      snprintf(mp, sizeof mp, "the master limiter: a track 24 dB over, its peak at the output %d (ceiling %.0f), %d samples clipped", top, LIMIT_CEILING, clipped);
      check(top <= (int)LIMIT_CEILING + 1 && top >= (int)(LIMIT_CEILING * 0.95f) && clipped == 0, mp);
      for (int t = 0; t < NUM_TRACKS; t++) tracks[t].gain = gains[t];
      setTrackFilter(6, 0, FILTER_MAX);
      runBeats(1);
    }
    recordSine(5, 331, 3.6);
    inputMonitored = false;
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
    inputMonitored = true;
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
    runBeats(4.5);                                   // (half a beat past the bar line: past the 100 ms grace, so it waits)
    setMasterVolume(MASTER_VOLUME_UNITY - 3);        // (the master volume is not in what's recorded)
    const float masterNow = powf(10.0f, -3 * MASTER_STEP_DB / 20);
    // The output, by transport sample, while it records.
    std::vector<int16_t> outLog(44100 * 30);
    uint32_t logFrom = transport.now;
    afterAudio = [&] {
      uint32_t s0 = transport.now - AUDIO_BLOCK_SAMPLES;
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        if (s0 + i - logFrom < outLog.size()) outLog[s0 + i - logFrom] = outL.data[i];
    };
    feedRamp = true;                                 // (the input too: not scaled by the master volume either)
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
    feedRamp = false;
    setMasterVolume(MASTER_VOLUME_UNITY);
    bool same = true;
    for (uint32_t f : {0u, 1000u, 30000u, 100000u, tracks[6].recordedFrames - 1})
      if (fabsf(wavLeft(6, f) - outLog[start + f - logFrom] / masterNow) > 2 + 1 / masterNow) {   // (a step of the output, scaled back up)
        same = false;
        if (getenv("RESAMPLE_TRACE")) printf("    frame %u: recorded %.1f, heard %.1f\n", f, (double)wavLeft(6, f), outLog[start + f - logFrom] / masterNow);
      }
    check(tracks[6].state == TrackState::Playing && tracks[6].loopBars == 2 && same && outLog[start + 1000 - logFrom] != 0,
          "track 7 holds 2 bars of what was played, tracks and input, without the master volume (turned down 3 steps)");
    check(takeResampled() == 6 && takeResampled() == -1, "reported once as resampled");
    uint32_t sample = tracks[6].sample;
    int count = sceneCount(), pos = sceneCurrent();
    check(sceneFromResample(6) >= 0 && sceneCount() == count + 1 && sceneCurrent() == pos + 1,
          "a new scene inserted after the current one, and selected");
    waitScene();
    runBeats(2);
    bool othersEmpty = true;
    for (int t = 0; t < NUM_TRACKS; t++) if (t != 6 && t != 5 && tracks[t].state != TrackState::Empty) othersEmpty = false;
    check(tracks[6].sample == sample && tracks[6].state == TrackState::Playing && tracks[6].live && othersEmpty &&
          silencedTracks == 0, "in it: the resampling, on the same track, and nothing unmuted");
    check(tracks[5].sample == mutedSample && tracks[5].state == TrackState::Muted, "the muted clip came along, still muted, on track 2 (right after the resampling's)");
    setTrackMuted(5, false);
    runBeats(2);
    checkSync(5, "the muted clip unmuted: heard (not left silenced)");
    setTrackMuted(5, true);
    check(tracks[6].volume == VOLUME_UNITY + 2, "the resampled clip two volume steps up (as loud as what it resampled: the mix's headroom, -3.75 dB)");
    setTrackMix(6, VOLUME_UNITY, tracks[6].balance);   // (the sync check compares at unity gain)
    runBeats(1);
    checkSync(6, "the resampling, playing on");
    check(sceneStep(-1), "back to the scene before");
    waitScene();
    runBeats(2);
    check(tracks[0].state == TrackState::Playing && tracks[6].state == TrackState::Empty &&
          tracks[1].sample == mutedSample && tracks[1].state == TrackState::Muted,
          "the scene before: as it was (the muted clip too), without the resampling");
    feedRamp = true;
  }

  printf("recordings named after the project: XNNN.wav, numbered per project (001-999)\n");
  {
    lastAction = "naming";
    const std::string letter(1, (char)('A' + projectCurrent()));
    std::string names[2], before[2];
    const int tracksTo[2] = {3, 2};                // (two tracks: one numbering for the project)
    if (!transport.running) transportStart();
    for (int k = 0; k < 2; k++) {
      clearTrack(tracksTo[k]);
      recordTrack(tracksTo[k], 3.5, 1, k ? "  (track 3 recorded)" : "  (track 4 recorded)");
      before[k] = hostFile(tracks[tracksTo[k]].sample);
      before[k] = before[k].substr(before[k].rfind('/') + 1);
    }
    transportStop();
    runUntilIdle();
    for (int i = 0; i < 40; i++) serviceStorage();   // cut down to size and named, stopped
    for (int k = 0; k < 2; k++) {
      std::string after = hostFile(tracks[tracksTo[k]].sample);
      after = after.substr(after.rfind('/') + 1);
      names[k] = after;
      char m[200];
      snprintf(m, sizeof m, "while playing still %s (recording space); once stopped %s (the project's letter and a number of three digits)",
               before[k].c_str(), after.c_str());
      check(before[k].size() == 8 && after.size() == 8 && after.compare(0, 1, letter) == 0 && isdigit(after[1]) && isdigit(after[3]), m);
    }
    char m[160];
    snprintf(m, sizeof m, "another track, the same project: the next number (%s, then %s)", names[0].c_str(), names[1].c_str());
    check(atoi(names[1].substr(1, 3).c_str()) == atoi(names[0].substr(1, 3).c_str()) + 1, m);
    transportStart();
    runBeats(4);
    checkSync(3, "track 4 playing on from the renamed file");
    transportStop();
    runUntilIdle();
  }

  printf("projects: a new one, back, cut and paste\n");
  {
    lastAction = "projects";
    if (!transport.running) transportStart();
    runBeats(9.5);
    const int32_t at = transport.beat - transportOrigin();
    transportStop();
    runUntilIdle();
    check(at > 0 && transportResumeBeat() == at, "stopped: its beat kept for SHIFT+START");
    transportStartAt(transportResumeBeat());
    check(transport.beat - transportOrigin() == at, "resumed from it");
    transportStop();
    runUntilIdle();
    int scenes = sceneCount(), pos = sceneCurrent();
    uint32_t sample0 = tracks[0].sample;
    check(projectCurrent() == 0 && projectFilled(0) && !projectFilled(1), "project 1 loaded (the first version's scenes), slot 2 empty");
    check(projectLoad(1) && projectCurrent() == 1 && projectFilled(1) && sceneCount() == 0 && sceneCurrent() == 0,
          "an empty slot: a new project, on its empty scene");
    check(transportResumeBeat() == 0, "a project loaded: SHIFT+START from 1.1.1");
    bool empty = true;
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) empty = false;
    check(empty && !projectHasClips(), "its tracks empty, no clips");
    check(projectLoad(0) && sceneCount() == scenes && sceneCurrent() == pos && tracks[0].sample == sample0,
          "back to project 1: its scenes and tracks");
    check(projectHasClips(), "project 1 has clips");
    {
      uint32_t e = projectEdits();
      bool stepped = sceneStep(-1) && sceneStep(1);
      runUntilIdle();
      check(stepped && sceneCurrent() == pos && projectEdits() == e, "stepping through its scenes, or loading it: no edit");
      float was = simTempo();
      tempoByHand(was + 1);
      check(projectEdits() != e, "the tempo set by hand: an edit");
      tempoByHand(was);
      e = projectEdits();
      bool muted = trackMuted(0);
      setTrackMuted(0, !muted);
      setTrackMuted(0, muted);
      check(projectEdits() != e, "a track muted by hand: an edit");
      runUntilIdle();
    }
    check(projectCut(1) && !projectFilled(1) && !projectClipboardFull() && !projectPaste(5) && !projectFilled(5),
          "project 2 cut (no clips): its slot empty, not to the clipboard (nothing to paste)");
    check(projectCut(0) && projectCurrent() == -1 && !projectFilled(0) && sceneCount() == 0 && !sceneStep(1),
          "the loaded one cut: its slot empty, the empty dummy project loaded (no N+1* with no scenes)");
    check(projectPaste(2) && projectLoad(2) && sceneCount() == scenes && sceneCurrent() == pos && tracks[0].sample == sample0,
          "pasted into slot 3 and loaded: project 1 as it was");
    check(projectPaste(7), "and into slot 8 too (the same clips)");
    check(projectLoad(9) && projectLoad(2) && projectCut(9) && !projectFilled(9) && projectClipboardFull() &&
          projectPaste(10) && !projectPaste(10) && projectLoad(10) && tracks[0].sample == sample0 && projectLoad(2),
          "a project with no clips cut: the clipboard keeps project 1 (pasted into slot 11, once: it's full then)");
    check(projectDelete(10), "(that paste gone)");
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
    auto sentLast = [&](std::initializer_list<int> want) {   // (the MIDI out's last non-clock bytes)
      int k = (int)want.size(), i = 0;
      if (midi.nmsg < k) return false;
      for (int b : want) if (midi.msgs[(midi.nmsg - k + i++) % 16] != b) return false;
      return true;
    };
    transportStart();
    check(sentLast({0xF2, 0, 0, 0xFA}), "started: MIDI Song Position 0, then Start");
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
    check(sentLast({0xFC, 0xF2, 0, 0, 0xFB}), "MIDI followers back to 1.1.1 with it: Stop, Song Position 0, Continue");
    {
      runBeats(5.3);                                 // (tap tempo's snap: to the nearest bar start)
      transportSnapToBar();
      int32_t at = transport.beat - transportOrigin();
      block(); block();                              // (sent by the MIDI timer)
      uint32_t pos = (uint32_t)at * 4;
      char m9[160];
      snprintf(m9, sizeof m9, "snapped to a bar (%d beats in): MIDI Stop, Song Position %u, Continue", (int)at, pos);
      char got[64] = "";
      for (int k = std::max(0, midi.nmsg - 6); k < midi.nmsg; k++) snprintf(got + strlen(got), sizeof got - strlen(got), " %02X", midi.msgs[k % 16]);
      snprintf(m9 + strlen(m9), sizeof m9 - strlen(m9), " [last:%s]", got);
      check(sentLast({0xFC, 0xF2, (int)(pos & 0x7F), (int)(pos >> 7), 0xFB}), m9);
      runBeats(1);
    }
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
      int deleted = deleteUnusedSamples(false);   // (not the silent: the later tests' recordings of silence kept)
      bool kept = sampleExists(playing) && tracks[0].state != TrackState::Empty;
      char m[120];
      snprintf(m, sizeof m, "unused recordings deleted (%d of %u), the ones in use kept", deleted, before);
      check(deleted > 0 && sampleCount() == before - deleted && kept, m);
      check(deleteUnusedSamples(false) == 0, "a second time: nothing more");
      transportStart();
      runBeats(4);
      checkSync(0, "playing on after the clean-up");
      transportStop();
      runUntilIdle();
    }
  }

  printf("projects: one tempo each, new ones take the last set by hand; the master volume is the app's, not a project's\n");
  {
    lastAction = "project tempo";
    auto at = [](float b, int v) { return fabsf(simTempo() - b) < 0.05f && masterVolume() == v; };
    const int U = MASTER_VOLUME_UNITY;
    check(projectLoad(2) && sceneCount() >= 2, "project 3");
    tempoByHand(133);
    volumeByHand(U - 2);
    for (int i = 0; i < 5; i++) block();
    int pos = sceneCurrent();
    check((sceneStep(-1) || sceneStep(1)) && sceneCurrent() != pos && masterVolume() == U - 2, "another scene: the same master volume (its own tempo)");
    check(sceneGoEmpty() && masterVolume() == U - 2, "the empty scene too");
    tempoByHand(133);
    check(projectLoad(3) && sceneCount() == 0 && at(133, U - 2), "a new project (slot 4): the tempo last set by hand, the volume unchanged");
    tempoByHand(97);
    volumeByHand(U + 2);
    check(projectLoad(2) && at(133, U + 2), "project 3 again: its own tempo, the volume as it is (100% + 2 steps)");
    check(projectLoad(4) && at(97, U + 2), "another new project (slot 5): 97, the last set by hand (loading project 3 didn't count)");
    check(projectLoad(2) && at(133, U + 2), "and project 3");
    for (int i = 0; i < 20; i++) serviceScenes();
    simSetTempo(120);                              // (power off: the tempo forgotten; the volume comes back from the session, not tested here)
    for (int i = 0; i < 5; i++) block();
    storageBegin([](const char *) {});
    scenesBegin(simTempo, simSetTempo, 2, simNewTempo);
    for (int i = 0; i < 5; i++) block();
    check(projectCurrent() == 2 && at(133, U + 2), "after a power cycle: project 3's tempo, the volume not changed by loading it");
    volumeByHand(U);                               // (the rest at 100%)
    check(projectDelete(3) && projectDelete(4), "(the new ones deleted)");
  }

  printf("a project exported as an Ableton Live Set\n");
  {
    lastAction = "export";
    check(projectLoad(2), "project 3");
    int want = 2;                                  // (a scene with a clip on track 1: silent ones were deleted from scenes before)
    for (int pos = 1; pos <= sceneCount(); pos++) { TrackUnit us[NUM_TRACKS]; if (projectScene(2, pos, us) && us[NUM_TRACKS - 1].sample) { want = pos; break; } }
    const float keepTempo = simTempo();             // (tempo per scene: the tests after run at the tempo they did)
    for (int i = 0; i < 20 && sceneCurrent() != want; i++) sceneStep(sceneCurrent() < want ? 1 : -1);   // (stopped: at once)
    simSetTempo(keepTempo);
    for (int i = 0; i < 3; i++) block();
    check(sceneCurrent() == want, "on a scene with a clip on track 1");
    // Its scenes with clips (empty ones left out), and scene 2's place among them.
    int scenes = 0, index = -1;
    for (int pos = 1; pos <= sceneCount(); pos++) {
      TrackUnit units[NUM_TRACKS];
      bool clips = false;
      if (projectScene(2, pos, units)) for (const auto &u : units) clips |= u.sample != 0;
      if (clips && pos == want) index = scenes;
      if (clips) scenes++;
    }
    int playing = 0;                               // its tracks with a clip there: saved as playing
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) playing++;
    setTrackMix(NUM_TRACKS - 1, 6, 4);              // (on scene 2: track 1, Live's first: a lower volume, a balance and filter points)
    setTrackFilter(NUM_TRACKS - 1, 8, 24);
    trackChanged(NUM_TRACKS - 1);
    char colorHex[8];
    snprintf(colorHex, sizeof colorHex, "%02X%02X%02X", (PROJECT_COLORS[2] >> 11) * 255 / 31, ((PROJECT_COLORS[2] >> 5) & 63) * 255 / 63, (PROJECT_COLORS[2] & 31) * 255 / 31);
    int first = exportProject(2), second = exportProject(2);
    std::string dir = simRoot + "/L7/EXPORTS/L7 C0" + std::to_string(first) + " Project";
    char m[200];
    snprintf(m, sizeof m, "exported as L7 C%02d, then L7 C%02d (the next number for its letter)", first, second);
    check(first >= 1 && second == first + 1, m);
    // The set: valid gzip and XML, 7 tracks with two Auto Filters each, a clip
    // slot per scene on each, the scenes; every clip's sample copied.
    std::string py = "python3 -c \"import gzip,sys,os,xml.etree.ElementTree as E; d=sys.argv[1]; r=E.fromstring(gzip.open(d+'/'+os.path.basename(d)[:-8]+'.als').read()); "
                     "t=r.find('LiveSet/Tracks'); s=len(r.find('LiveSet/Scenes')); "
                     "ok=len(t)==7 and all(len(x.findall('.//Devices/AutoFilter'))==2 for x in t) and all(len(x.find('DeviceChain/MainSequencer/ClipSlotList'))==s for x in t); "
                     "c=[f.get('Value') for f in r.iter('RelativePath') if f.get('Value').startswith('Samples/')]; "
                     "ok=ok and all(os.path.exists(d+'/'+p) for p in c) and os.path.isdir(d+'/Ableton Project Info'); i=sys.argv[2]; pl=[x.find('SavedPlayingSlot').get('Value') for x in t]; "
                     "print(s, len(c), sum(v==i for v in pl), sum(v not in (i,'-2') for v in pl)); sys.exit(0 if ok else 1)\" '" + dir + "' " + std::to_string(index);
    FILE *out = popen(py.c_str(), "r");
    int s = -1, clips = -1, saved = -1, other = -1;
    if (out) { if (fscanf(out, "%d %d %d %d", &s, &clips, &saved, &other) != 4) s = -1; }
    int rc = out ? pclose(out) : -1;
    snprintf(m, sizeof m, "the set opens as gzip and XML: 7 tracks, two Auto Filters each, %d scene(s) (%d of the project's have clips, and an empty last one), %d clip(s), "
             "their samples copied, a Live project (Ableton Project Info)", s, scenes, clips);
    check(rc == 0 && s == scenes + 1 && clips > 0, m);
    {                                              // each scene's tempo: L7's scene's (the empty last one: none)
      std::string want;
      for (int pos = 1; pos <= sceneCount(); pos++) {
        TrackUnit us[NUM_TRACKS];
        bool c = false;
        if (projectScene(2, pos, us)) for (const auto &u : us) c |= u.sample != 0;
        if (!c) continue;
        char b[24];
        float t = projectSceneTempo(2, pos);
        snprintf(b, sizeof b, "%s%g", want.empty() ? "" : ",", t > 0 ? roundf(t * 10) / 10 : -1.0f);
        want += b;
      }
      std::string tp = "python3 -c \"import gzip,sys,os,xml.etree.ElementTree as E; d=sys.argv[1]; r=E.fromstring(gzip.open(d+'/'+os.path.basename(d)[:-8]+'.als').read()); "
                       "sc=r.find('LiveSet/Scenes'); print(','.join(('%g'%float(x.find('Tempo').get('Value'))) if x.find('IsTempoEnabled').get('Value')=='true' else '-1' for x in sc))\" '" + dir + "'";
      FILE *to = popen(tp.c_str(), "r");
      char got[400] = {};
      if (to) { if (!fgets(got, sizeof got, to)) got[0] = 0; pclose(to); }
      std::string g = got;
      while (!g.empty() && (g.back() == '\n' || g.back() == '\r')) g.pop_back();
      char mt[600];
      snprintf(mt, sizeof mt, "each scene's tempo L7's scene's, the last one's off: %s (want %s,-1)", g.c_str(), want.c_str());
      check(g == want + ",-1", mt);
    }
    snprintf(m, sizeof m, "scene 2's clips (set scene %d) saved as playing: %d track(s) (%d have a clip there), %d other", index + 1, saved,
             playing, other);
    check(saved == playing && other == 0 && playing > 0, m);
    // No scene named; every track and clip in the palette colour nearest the project's (Lab distance, worked out here on its own).
    std::string cp = "python3 -c \"import gzip,sys,os,xml.etree.ElementTree as E\n"
                     "P='FF94A6 FFA529 CC9927 F7F47C BFFB00 1AFF2F 25FFA8 5CFFE8 8BC5FF 5480E4 92A7FF D86CE4 E553A0 FFFFFF FF3636 F66C03 99724B FFF034 87FF67 3DC300 00BFAF 19E9FF 10A4EE 007DC0 886CE4 B677C6 FF39D4 D0D0D0 "
                     "E2675A FFA374 D3AD71 EDFFAE D2E498 BAD074 9BC48D D4FDE1 CDF1F8 B9C1E3 CDBBE4 AE98E5 E5DCE1 A9A9A9 C6928B B78256 99836A BFBA69 A6BE00 7DB04D 88C2BA 9BB3C4 85A5C2 8393CC A595B5 BF9FBE BC7196 7B7B7B "
                     "AF3333 A95131 724F41 DBC300 85961F 539F31 0A9C8E 236384 1A2F96 2F52A2 624BAD A34BAD CC2E6E 3C3C3C'.split()\n"
                     "def lab(h):\n"
                     " c=[((int(h[i:i+2],16)/255+0.055)/1.055)**2.4 if int(h[i:i+2],16)/255>0.04045 else int(h[i:i+2],16)/255/12.92 for i in (0,2,4)]\n"
                     " x=(0.4124*c[0]+0.3576*c[1]+0.1805*c[2])/0.95047; y=0.2126*c[0]+0.7152*c[1]+0.0722*c[2]; z=(0.0193*c[0]+0.1192*c[1]+0.9505*c[2])/1.08883\n"
                     " f=lambda t: t**(1/3) if t>0.008856 else 7.787*t+16/116\n"
                     " return (116*f(y)-16,500*(f(x)-f(y)),200*(f(y)-f(z)))\n"
                     "d=sys.argv[1]; r=E.fromstring(gzip.open(d+'/'+os.path.basename(d)[:-8]+'.als').read()); w=lab(sys.argv[2])\n"
                     "n=min(range(70),key=lambda i:sum((a-b)**2 for a,b in zip(lab(P[i]),w)))\n"
                     "t=r.find('LiveSet/Tracks'); cl=[c.find('Color').get('Value') for c in r.iter('AudioClip')]\n"
                     "ok=all(x.find('Color').get('Value')==str(n) for x in t) and cl and all(v==str(n) for v in cl) and all(sc.find('Name').get('Value')=='' for sc in r.find('LiveSet/Scenes'))\n"
                     "print(n); sys.exit(0 if ok else 1)\" '" + dir + "' " + colorHex;
    FILE *cout = popen(cp.c_str(), "r");
    int colorIdx = -1;
    if (cout) { if (fscanf(cout, "%d", &colorIdx) != 1) colorIdx = -1; }
    int crc = cout ? pclose(cout) : -1;
    snprintf(m, sizeof m, "no scene named; the 7 tracks and every clip in Live colour %d, the nearest to the project's", colorIdx);
    check(crc == 0, m);
    // Every clip's warp markers: one at every bar line of its sample (beat 0, 4, 8 ..., past its loop's end), later and later in it.
    std::string wp = "python3 -c \"import gzip,sys,os,xml.etree.ElementTree as E\n"
                     "d=sys.argv[1]; r=E.fromstring(gzip.open(d+'/'+os.path.basename(d)[:-8]+'.als').read()); n=0; bad=0\n"
                     "for c in r.iter('AudioClip'):\n"
                     " w=c.find('WarpMarkers'); b=[float(x.get('BeatTime')) for x in w]; t=[float(x.get('SecTime')) for x in w]; n+=1\n"
                     " if len(b)<2 or b!=[4.0*i for i in range(len(b))] or any(t[i+1]<=t[i] for i in range(len(t)-1)) or b[-1]<float(c.find('Loop/LoopEnd').get('Value')): bad+=1\n"
                     "print(n,bad); sys.exit(0 if n and not bad else 1)\" '" + dir + "'";
    FILE *wout = popen(wp.c_str(), "r");
    int wclips = -1, wbad = -1;
    if (wout) { if (fscanf(wout, "%d %d", &wclips, &wbad) != 2) wclips = -1; }
    int wrc = wout ? pclose(wout) : -1;
    snprintf(m, sizeof m, "each of the %d clip(s) has a warp marker at every bar line of its sample (%d wrong)", wclips, wbad);
    check(wrc == 0, m);
    // Every clip's four envelopes (volume, pan, lowpass and highpass cutoff), pointing at its own track's parameters, values in range.
    std::string ep = "python3 -c \"import gzip,sys,os,xml.etree.ElementTree as E\n"
                     "d=sys.argv[1]; r=E.fromstring(gzip.open(d+'/'+os.path.basename(d)[:-8]+'.als').read()); n=0; bad=0; dis=0; vals=set()\n"
                     "for t in r.find('LiveSet/Tracks'):\n"
                     " m=t.find('DeviceChain/Mixer'); af=[a for a in t.iter('AutoFilter')]\n"
                     " lp=[a for a in af if a.find('FilterType/Manual').get('Value')=='0'][0]; hp=[a for a in af if a.find('FilterType/Manual').get('Value')=='1'][0]\n"
                     " u=[d for d in t.iter('StereoGain')][0]; want=[u.find('Balance/AutomationTarget').get('Id'),hp.find('Cutoff/AutomationTarget').get('Id'),lp.find('Cutoff/AutomationTarget').get('Id')]\n"
                     " lim=[(-1,1),(20,135),(20,135)]\n"
                     " if [a.find('FilterType/Manual').get('Value') for a in af]!=['1','0'] or [d.tag for d in t.find('DeviceChain/DeviceChain/Devices')]!=['AutoFilter','AutoFilter','StereoGain']: bad+=1\n"
                     " for c in t.iter('AudioClip'):\n"
                     "  es=c.findall('Envelopes/Envelopes/ClipEnvelope'); n+=1; dis+=c.find('Disabled').get('Value')=='true'; g=float(c.find('SampleVolume').get('Value'))\n"
                     "  if not 0<=g<=2: bad+=1\n"
                     "  if [e.find('EnvelopeTarget/PointeeId').get('Value') for e in es]!=want: bad+=1; continue\n"
                     "  for e,(lo,hi) in zip(es,lim):\n"
                     "   v=[float(x.get('Value')) for x in e.findall('Automation/Events/FloatEvent')]\n"
                     "   if len(v)!=2 or v[0]!=v[1] or not lo<=v[0]<=hi: bad+=1\n"
                     "   vals.add(round(v[0],3))\n"
                     "ids=[x.get('Id') for x in r.iter('FloatEvent') if x.get('Time') in ('0','-63072000')]\n"
                     "print(n,bad,dis); sys.exit(0 if n and not bad else 1)\" '" + dir + "'";
    FILE *eout = popen(ep.c_str(), "r");
    int envClips = -1, envBad = -1, envOff = -1;
    if (eout) { if (fscanf(eout, "%d %d %d", &envClips, &envBad, &envOff) != 3) envClips = -1; }
    int erc = eout ? pclose(eout) : -1;
    snprintf(m, sizeof m, "each of the %d clip(s) (%d of them off) has a gain and its 3 envelopes (the Utility's balance, highpass, lowpass cutoff) on its own track's parameters, the filters in the order hp > lp, values in range (%d wrong)", envClips, envOff, envBad);
    check(erc == 0, m);
    // Those values in the first track's clip of that scene: the gain from the volume step, the balance, the cutoffs.
    {
      char args[200];
      const float comp = filterCompOf(8, 24, profileOf(tracks[NUM_TRACKS - 1].overview));   // (the filters' compensation, in the clip's gain too)
      snprintf(args, sizeof args, "%d %d %d %.4f %.4f %.6f", index, BALANCE_MAX, VOLUME_UNITY, (double)filterStepHz(8), (double)filterStepHz(24), (double)comp);
      std::string vp = "python3 -c \"import gzip,sys,os,math,xml.etree.ElementTree as E\n"
                       "d=sys.argv[1]; i,bm,vu,hz1,hz2,comp=[float(a) for a in sys.argv[2:8]]; i=int(i)\n"
                       "r=E.fromstring(gzip.open(d+'/'+os.path.basename(d)[:-8]+'.als').read()); t=r.find('LiveSet/Tracks')[0]\n"
                       "c=t.findall('DeviceChain/MainSequencer/ClipSlotList/ClipSlot')[i].find('ClipSlot/Value/AudioClip')\n"
                       "v=[float(e.find('Automation/Events/FloatEvent').get('Value')) for e in c.findall('Envelopes/Envelopes/ClipEnvelope')]\n"
                       "note=lambda hz: 69+12*math.log2(hz/440)\n"
                       "want=[float(c.find('SampleVolume').get('Value'))/comp-10**((6-vu)*1.875/20), v[0]-4/bm, v[1]-note(hz1), v[2]-note(hz2)]\n"
                       "print(*['%.4f'%w for w in want]); sys.exit(0 if all(abs(w)<0.01 for w in want) else 1)\" '" + dir + "' " + args;
      FILE *vout = popen(vp.c_str(), "r");
      char res[120] = {};
      if (vout) { if (!fgets(res, sizeof res, vout)) res[0] = 0; }
      int vrc = vout ? pclose(vout) : -1;
      snprintf(m, sizeof m, "volume 6, balance 4, filter 8-24 on track 1 of scene 2: the clip's gain, the Utility's balance, the cutoffs as expected (differences %s)", res);
      check(vrc == 0, m);
    }
    setTrackMix(NUM_TRACKS - 1, VOLUME_UNITY, 0);
    setTrackFilter(NUM_TRACKS - 1, 0, FILTER_MAX);
    trackChanged(NUM_TRACKS - 1);
    // The master track's Limiter, and every automation target / pointee id in the set unique, below NextPointeeId.
    {
      std::string ip = "python3 -c \"import gzip,sys,os,re,xml.etree.ElementTree as E\n"
                       "d=sys.argv[1]; x=gzip.open(d+'/'+os.path.basename(d)[:-8]+'.als').read().decode()\n"
                       "ids=[int(v) for v in re.findall(r'<\\w*(?:Target|Pointee) Id=\\\"(\\d+)\\\"', x)]\n"
                       "nxt=int(re.search(r'<NextPointeeId Value=\\\"(\\d+)\\\"', x).group(1)); r=E.fromstring(x)\n"
                       "lim=[c.tag for c in r.find('LiveSet/MasterTrack/DeviceChain/DeviceChain/Devices')]\n"
                       "print(len(ids), nxt, lim); sys.exit(0 if len(ids)==len(set(ids)) and max(ids)<nxt and lim==['Limiter'] else 1)\" '" + dir + "'";
      int irc = system(ip.c_str());
      check(irc == 0, "the master track has a Limiter, and every target/pointee id in the set is unique and below NextPointeeId");
    }
    // The folder's icon for a Mac: Icon<CR>, its AppleDouble file (Finder info + the icon's fork), and the folder's own.
    auto sizeOf = [](const std::string &path) { FILE *f = fopen(path.c_str(), "rb"); long n = -1; if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); } return n; };
    long iconSize = sizeOf(dir + "/Icon\r"), forkSize = sizeOf(dir + "/._Icon\r"), folderSize;
    folderSize = sizeOf(simRoot + "/L7/EXPORTS/._L7 C0" + std::to_string(first) + " Project");
    snprintf(m, sizeof m, "the folder's Mac icon: Icon<CR> empty (%ld), ._Icon<CR> %ld bytes, the folder's ._ file %ld bytes", iconSize, forkSize, folderSize);
    check(iconSize == 0 && forkSize == 0xEE2 + 62577 && folderSize == 0xEE2, m);
  }

  printf("a card from before the folders: moved into them at boot\n");
  {
    transportStop();
    runUntilIdle();
    for (int i = 0; i < 20; i++) serviceScenes();
    flushHeaders();
    flushScenes();
    uint32_t samplesBefore = sampleCount();
    bool filledBefore[PROJECTS];
    for (int p = 0; p < PROJECTS; p++) filledBefore[p] = projectFilled(p);
    int projectBefore = projectCurrent(), scenesBefore = sceneCount();
    // The old layout: lower case folders, the projects as 1.dat..12.dat and clip.dat/none.dat in /projects, recording space beside the
    // samples, the unit and session files in /L7, the exports in /Ableton.
    std::string cmd = "python3 -c '" + std::string(R"PY(
import os, sys, shutil
root = sys.argv[1] + "/L7"
os.chdir(root)
for up, low in (("SAMPLES", "samples"), ("PROJECTS", "projects")):
    os.rename(up, "_tmp"); os.rename("_tmp", low)
for f in os.listdir("DATA"):
    src = "DATA/" + f
    if f.endswith(".DAT") and f[:4].isdigit(): dst = "samples/" + f[:4] + ".wav"
    elif f.startswith("TRACK"): dst = f[:6].lower() + ".unit"
    elif f == "SESSION.DAT": dst = "session.txt"
    elif f in ("CLIP.DAT", "NONE.DAT"): dst = "projects/" + f.lower()
    else: continue
    os.rename(src, dst)
os.rmdir("DATA")
for i, l in enumerate("ABCDEFGHIJKL"):
    if os.path.exists("projects/%s.L7P" % l): os.rename("projects/%s.L7P" % l, "projects/%d.dat" % (i + 1))
os.rename("EXPORTS", "../Ableton")
)PY") + "' " + simRoot;
    check(system(cmd.c_str()) == 0, "(the card put back to the older layout)");
    storageBegin([](const char *) {});
    scenesBegin(simTempo, simSetTempo, 2, simNewTempo);
    bool same = projectCurrent() == projectBefore && sceneCount() == scenesBefore && sampleCount() == samplesBefore && projectClipboardFull();
    for (int p = 0; p < PROJECTS; p++) same &= projectFilled(p) == filledBefore[p];
    check(same, "after the boot: the same projects, scenes and samples");
    FILE *ls = popen(("cd " + simRoot + "/L7 && python3 -c \"import os; print(' '.join(sorted(os.listdir('.'))), '|', ' '.join(sorted(f for f in os.listdir('PROJECTS'))), '|', len([f for f in os.listdir('DATA') if f.endswith('.DAT') and f[0].isdigit()]), '|', len([f for f in os.listdir('SAMPLES') if f.endswith('.wav')]), '|', ' '.join(os.listdir('EXPORTS')[:1]))\"").c_str(), "r");
    char line[400] = {};
    if (ls) { if (!fgets(line, sizeof line, ls)) line[0] = 0; pclose(ls); }
    std::string got = line;
    check(got.find("DATA EXPORTS PROJECTS SAMPLES |") == 0 && got.find(".dat") == std::string::npos && got.find("L7P") != std::string::npos &&
          got.find("L7 C") != std::string::npos,
          "folders DATA EXPORTS PROJECTS SAMPLES (upper case), projects as .L7P, the exports moved");
  }

  printf("a card with the folders but DATA's files named .wav, .UNIT and .TXT: renamed to .DAT at boot\n");
  {
    transportStop();
    runUntilIdle();
    flushHeaders();
    flushScenes();
    uint32_t samplesBefore = sampleCount();
    Session session;
    saveSession(session);                          // (the sim never writes one otherwise)
    std::string cmd = "python3 -c '" + std::string(R"PY(
import os, sys
os.chdir(sys.argv[1] + "/L7/DATA")
for f in os.listdir("."):
    if f.endswith(".DAT") and f[:4].isdigit(): os.rename(f, f[:4] + ".wav")
    elif f.startswith("TRACK"): os.rename(f, f[:6] + ".UNIT")
    elif f == "SESSION.DAT": os.rename(f, "SESSION.TXT")
)PY") + "' " + simRoot;
    check(system(cmd.c_str()) == 0, "(DATA's files put back to the earlier names)");
    storageBegin([](const char *) {});
    scenesBegin(simTempo, simSetTempo, 2, simNewTempo);
    FILE *ls = popen(("cd " + simRoot + "/L7/DATA && ls | tr '\\n' ' '").c_str(), "r");
    char line[600] = {};
    if (ls) { if (!fgets(line, sizeof line, ls)) line[0] = 0; pclose(ls); }
    std::string got = line;
    check(sampleCount() == samplesBefore && got.find(".wav") == std::string::npos && got.find("UNIT") == std::string::npos &&
          got.find("TXT") == std::string::npos && got.find("TRACK7.DAT") != std::string::npos && got.find("SESSION.DAT") != std::string::npos,
          "after the boot: NNNN.DAT, TRACK1-7.DAT, SESSION.DAT, nothing lost");
  }

  printf("back from storage mode: everything dropped and read from the card again\n");
  {
    transportStop();
    runUntilIdle();
    for (int i = 0; i < 20; i++) serviceScenes();
    flushHeaders();
    flushScenes();
    int loaded = projectCurrent();
    uint32_t sampleBefore[NUM_TRACKS];
    TrackState stateBefore[NUM_TRACKS];
    for (int t = 0; t < NUM_TRACKS; t++) { sampleBefore[t] = tracks[t].sample; stateBefore[t] = tracks[t].state; }
    uint32_t samplesBefore = sampleCount();
    int scenesBefore = sceneCount(), other = -1;
    for (int p = 0; p < PROJECTS && other < 0; p++) if (p != loaded && projectFilled(p)) other = p;
    // The computer: deletes another project's file and a sample no track plays, and leaves a stray file.
    uint32_t gone = 0;
    for (int i = 0; i < (int)sampleCount() && !gone; i++) {
      bool used = false;
      for (int t = 0; t < NUM_TRACKS; t++) used |= sampleBefore[t] == sampleAt(i);
      if (!used) gone = sampleAt(i);
    }
    char gonePath[64] = {};
    if (gone) sampleFile(gone, gonePath, sizeof gonePath);
    char m0[120];
    std::string cmd = "rm -f " + simRoot + gonePath + " " + simRoot + "/L7/PROJECTS/" + std::string(1, (char)('A' + other)) + ".L7P; echo stray > " + simRoot + "/L7/STRAY.TXT";
    check(other >= 0 && gone && system(cmd.c_str()) == 0, "(the computer deleted another project's file and a sample, and added a file)");
    storageForget();
    scenesForget();
    bool forgot = true;
    for (int t = 0; t < NUM_TRACKS; t++) forgot &= tracks[t].state == TrackState::Empty && tracks[t].sample == 0;
    check(forgot && sampleCount() == 0, "dropped: every track empty, no samples known");
    int truncatesBefore = simSampleTruncates;
    check(storageBegin([](const char *) {}), "the card read again");
    snprintf(m0, sizeof m0, "the finished samples left as they are (%d cut down again: each one's modified date)", simSampleTruncates - truncatesBefore);
    check(simSampleTruncates == truncatesBefore, m0);
    scenesBegin(simTempo, simSetTempo, loaded, simNewTempo);
    bool same = true;
    for (int t = 0; t < NUM_TRACKS; t++) same &= tracks[t].sample == sampleBefore[t] && tracks[t].state == stateBefore[t];
    check(same && projectCurrent() == loaded && sceneCount() == scenesBefore, "the loaded project and its tracks as they were");
    char m[160];
    snprintf(m, sizeof m, "what the computer changed is seen: %u samples (one fewer than %u), project %c empty, the rest still there", sampleCount(), samplesBefore,
             'A' + other);
    check(sampleCount() == samplesBefore - 1 && !sampleExists(gone) && !projectFilled(other) && projectFilled(loaded), m);
    transportStart();
    runBeats(6);
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state == TrackState::Playing) checkSync(t, "after the reload");
    transportStop();
    runUntilIdle();
  }

  printf("recording space: 8 files, the lowest numbers; a card with more or higher ones is brought down to them\n");
  {
    transportStop();
    runUntilIdle();
    flushHeaders();
    flushScenes();
    std::string cmd = "cd " + simRoot + "/L7/DATA && for i in 1 2 3 4 5 6 7 8; do mv 000$i.DAT 007$i.DAT; ln 007$i.DAT 009$i.DAT; done";
    check(system(cmd.c_str()) == 0, "(the pool numbered 71-78, and 8 more files, 91-98)");
    storageForget();
    scenesForget();
    storageBegin([](const char *) {});
    scenesBegin(simTempo, simSetTempo, projectCurrent() < 0 ? 0 : 2, simNewTempo);
    FILE *ls = popen(("ls " + simRoot + "/L7/DATA | grep '^[0-9]' | tr '\\n' ' '").c_str(), "r");
    char line[400] = {};
    if (ls) { if (!fgets(line, sizeof line, ls)) line[0] = 0; pclose(ls); }
    check(std::string(line) == "0001.DAT 0002.DAT 0003.DAT 0004.DAT 0005.DAT 0006.DAT 0007.DAT 0008.DAT 0009.DAT 0010.DAT 0011.DAT 0012.DAT 0013.DAT 0014.DAT 0015.DAT 0016.DAT 0017.DAT 0018.DAT 0019.DAT 0020.DAT 0021.DAT 0022.DAT 0023.DAT 0024.DAT ", "after the boot: exactly 0001-0024.DAT (the pool: 24)");
  }

  printf("a recording in the dummy project (no project letter): a plain-numbered sample, still in step with the files\n");
  {
    if (projectCurrent() >= 0) projectDelete(projectCurrent());
    check(projectCurrent() < 0, "(the dummy project loaded)");
    if (!transport.running) transportStart();
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) clearTrack(t);
    recordTrack(2, 3, 1, "  (track 3, in the dummy project)");
    uint32_t id = tracks[2].sample;
    transportStop();
    runUntilIdle();
    for (int i = 0; i < 40; i++) serviceStorage();
    char path[64];
    sampleFile(id, path, sizeof path);
    struct stat st;
    FILE *ls = popen(("ls " + simRoot + "/L7/DATA | grep -c '^[0-9]'").c_str(), "r");
    int spaceFiles = -1;
    if (ls) { if (fscanf(ls, "%d", &spaceFiles) != 1) spaceFiles = -1; pclose(ls); }
    char m[160];
    snprintf(m, sizeof m, "sample %u is a plain number in SAMPLES (%s), its recording-space number free again (%d files in recording space)", id, path, spaceFiles);
    check(id > 0 && id < 10000 && ::stat((simRoot + path).c_str(), &st) == 0 && st.st_size < 10 * 1000 * 1000 && spaceFiles == 24, m);
  }

  printf("a card stall while playing: the tracks run short, then back in step with the clock\n");
  {
    if (!transport.running) transportStart();
    if (tracks[0].state == TrackState::Empty) recordTrack(0, 7.5, 2, "  (track 1 recorded)");
    runBeats(4);
    for (auto &tr : tracks) tr.underruns = 0;
    static int stalls;
    stalls = 1;
    simSdLatency = [](size_t, bool write) {          // one read: 1.5 s (far past what the buffers hold)
      if (write || !stalls) return;
      stalls--;
      pendingUs += 1500000;
      const double blockUs = AUDIO_BLOCK_SAMPLES * 1e6 / AUDIO_SAMPLE_RATE_EXACT;
      while (pendingUs >= blockUs) { pendingUs -= blockUs; audioBlock(); }
    };
    runBeats(4);
    simSdLatency = nullptr;
    uint32_t short_ = 0;
    for (auto &tr : tracks) short_ += tr.underruns;
    check(short_ > 0, "(the stall ran the tracks short)");
    runBeats(4);
    for (auto &tr : tracks) tr.underruns = 0;
    for (int t = 0; t < NUM_TRACKS; t++)
      if (tracks[t].live) {
        char m[96];
        snprintf(m, sizeof m, "track %d after the stall", t + 1);
        checkSync(t, m);
      }
  }

  printf("the metronome while stopped: its level follows the mix and the master volume\n");
  {
    if (transport.running) transportStop();
    for (int i = 0; i < 50; i++) block();
    const int wasMaster = masterVolume();
    const bool wasMonitored = inputMonitored, wasRamp = feedRamp;
    inputMonitored = true;
    auto clickPeak = [&](int master, bool loud) {
      setMasterVolume(master);
      feedRamp = loud;
      for (int i = 0; i < 400; i++) block();         // (the level's reference settles: up at once, down 6 dB a second)
      metronomeClick(true);
      int pk = 0;
      for (int i = 0; i < 20; i++) {
        block();
        for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++) pk = std::max(pk, abs((int)outL.data[k]));
      }
      return pk;
    };
    int quiet = clickPeak(MASTER_VOLUME_UNITY, false), down = clickPeak(MASTER_VOLUME_UNITY - 6, false);
    int loud = clickPeak(MASTER_VOLUME_UNITY, true);
    char m[160];
    snprintf(m, sizeof m, "stopped: a click %d, the master down 6 steps %d, over a loud input %d", quiet, down, loud);
    check(quiet > 0 && down < quiet * 0.7 && loud > quiet, m);
    setMasterVolume(wasMaster);
    inputMonitored = wasMonitored;
    feedRamp = wasRamp;
  }

  printf("tempo changes while recording: written as it comes, at its average tempo, its bar lines kept\n");
  {
    if (!transport.running) transportStart();
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) clearTrack(t);
    simSetTempo(120);
    runBeats(1);
    feedRamp = true;
    inputMonitored = true;
    recordPress(4);
    for (int i = 0; i < 4000 && recording.mode != Recording::Running; i++) block();
    int32_t startBeat = recording.startBeat;
    std::vector<uint32_t> bars = {recording.startSample};   // each bar line's input sample
    int32_t seenBar = transport.lastBarBeat;
    auto step = [&]() {
      block();
      if (transport.lastBarBeat != seenBar) {
        seenBar = transport.lastBarBeat;
        bars.push_back(transport.lastBarSample + LATENCY_FRAMES);
      }
    };
    auto run = [&](double toBeat) {
      for (int i = 0; i < 200000 && transport.beat + (double)transport.sampleInBeat / transport.spb < toBeat; i++) step();
    };
    run(startBeat + 5.5);
    simSetTempo(100);                               // mid bar 2
    run(startBeat + 10.25);
    simSetTempo(140);                               // mid bar 3
    run(startBeat + 15.5);
    recordPress(4);                                 // half a beat before 4 bars: 4 bars
    for (int i = 0; i < 200000 && recording.mode != Recording::Idle; i++) step();
    const Track &tr = tracks[4];
    char m[200];
    uint32_t total = bars.size() > 4 ? bars[4] - bars[0] : 0, avg = total / 16;
    snprintf(m, sizeof m, "4 bars (%u), at the average tempo (%u vs %u samples a beat), %u frames", tr.loopBars, tr.spbNative, avg, tr.recordedFrames);
    check(tr.loopBars == 4 && total && tr.spbNative == avg && tr.recordedFrames == 16 * avg, m);
    int off = 0, checked = 0;
    for (int b = 0; b < 4 && b < (int)bars.size(); b++) {
      uint32_t in = bars[b];
      if (in % 20000 > 19990 || in % 20000 < 10) continue;   // (the ramp wraps there)
      checked++;
      if (wavLeft(4, in - bars[0]) != rampAt(in)) off++;
    }
    snprintf(m, sizeof m, "written as it came: each bar line's input where it fell (%d of %d off)", off, checked);
    check(checked >= 3 && off == 0, m);
    SampleDetails d;
    bool got = sampleDetails(tr.sample, d);
    int wrong = 0;
    for (int b = 1; b < 4; b++) if (!got || abs((int)d.bars[b - 1] - (int)(bars[b] - bars[0])) > 1) wrong++;
    snprintf(m, sizeof m, "its bar lines kept in its header (%d of 3 off; the last: the clip's end, %u)", wrong, got ? d.bars[3] : 0);
    check(got && wrong == 0 && d.bars[3] == tr.recordedFrames, m);
    simSetTempo(120);
    runBeats(8);
    checkSync(4, "the clip at its average tempo in step with the clock");
  }

  printf("following an external MIDI clock (jittering up to 0.6 ms): its tempo, Start, its beat, a tempo change, Stop\n");
  {
    if (transport.running) transportStop();
    runUntilIdle();
    // Their clock, in the simulator's micros() (a little faster than its samples: the
    // follower measures the ratio); ticks fed as the blocks go by.
    double tickAt = simMicros + 1000, tickUs = 60e6 / 128 / 24;
    int32_t sent = 0, startedAt = -1;
    const double usPerSample = (AUDIO_BLOCK_SAMPLES * 1e6 / AUDIO_SAMPLE_RATE_EXACT + 15 - 0.49) / AUDIO_BLOCK_SAMPLES;   // (audioBlock(): 2902 + 15 us a block)
    double worst = 0;                                // their beat vs ours at their ticks, the last stretch
    bool measure = false;
    auto run = [&](int beats, bool clockOn = true) {
      for (int i = 0; i < 100000 && sent < beats * 24; i++) {
        block();
        while (clockOn && simMicros >= tickAt) {
          midiInByte(0xF8, (uint32_t)(tickAt + (rand() % 1201 - 600)));   // (their jitter: up to 0.6 ms)
          if (measure && startedAt >= 0 && transport.running) {
            double ours = transport.beat + (transport.sampleInBeat + ((uint32_t)tickAt - (int32_t)simMicros) / usPerSample) / transport.spb;
            double theirs = (sent - startedAt) / 24.0, err = fmod(ours - theirs + 1000.5 * BEATS_PER_BAR, BEATS_PER_BAR) - BEATS_PER_BAR / 2.0;
            worst = std::max(worst, fabs(err));
          }
          sent++;
          tickAt += tickUs;
        }
        if (!clockOn) sent++;                        // (counting blocks instead)
        midiInService();
      }
      sent = 0;
    };
    auto expectSpb = [&](double bpmTheirs) { return 60e6 / bpmTheirs / usPerSample; };
    char m[200];
    uint32_t spbBefore = transport.spb;
    run(3);                                          // their clock, their transport stopped: timed, not followed
    snprintf(m, sizeof m, "their clock while they're stopped: not followed (%d), the tempo free (%u samples a beat, as before)",
             midiClockFollowing(), transport.spb);
    check(!midiClockFollowing() && !transport.running && transport.spb == spbBefore, m);
    midiInByte(0xFA, simMicros);                     // Start: the next tick is 1.1.1
    startedAt = 0;
    for (int i = 0; i < 3; i++) { block(); midiInService(); }
    snprintf(m, sizeof m, "Start: their tempo at once (%u samples a beat, theirs %.0f), following", transport.pendingSpb ? transport.pendingSpb : transport.spb, expectSpb(128));
    check(midiClockFollowing() && fabs(transport.spb - expectSpb(128)) < expectSpb(128) * 0.002, m);
    run(1);
    check(transport.running, "Start: the transport runs");
    // (from here: ticks counted from the start)
    int32_t total = 24;
    double mult = 1, plus = 0;                       // ours: theirs times this, plus this (B+UP/DOWN, a jump)
    auto runCounted = [&](int beats) {
      for (int i = 0; i < 400000 && beats > 0; i++) {
        block();
        while (simMicros >= tickAt) {
          midiInByte(0xF8, (uint32_t)(tickAt + (rand() % 1201 - 600)));   // (their jitter: up to 0.6 ms)
          if (measure && transport.running) {
            double ours = transport.beat + (transport.sampleInBeat + ((int32_t)((uint32_t)tickAt - simMicros)) / usPerSample) / transport.spb;
            double theirs = total / 24.0 * mult + plus, err = ours - theirs;
            err = fmod(err + 1000.5 * BEATS_PER_BAR, BEATS_PER_BAR) - BEATS_PER_BAR / 2.0;
            worst = std::max(worst, fabs(err));
          }
          total++;
          tickAt += tickUs;
          if (total % 24 == 0) beats--;
        }
        midiInService();
      }
    };
    runCounted(12);                                  // settling
    measure = true; worst = 0;
    int changesBefore = simTempoChanges;
    runCounted(16);
    snprintf(m, sizeof m, "locked to their beat (and bar): at most %.4f beats off over 16 beats, the tempo %u (theirs %.0f), changed %d time(s) meanwhile",
             worst, transport.spb, expectSpb(128), simTempoChanges - changesBefore);
    check(worst < 0.01 && simTempoChanges - changesBefore <= 4, m);
    tickUs = 60e6 / 133.5 / 24;                      // their tempo changes
    measure = false;
    runCounted(12);
    measure = true; worst = 0;
    runCounted(8);
    snprintf(m, sizeof m, "their tempo up to 133.5: followed, at most %.4f beats off, shown %.1f", worst, simShownBpm);
    check(worst < 0.01 && fabs(transport.spb - expectSpb(133.5)) < expectSpb(133.5) * 0.003, m);
    {                                                // B+DOWN: ours theirs halved, from their next beat; B+UP: back
      measure = false;
      check(!midiScale(1), "B+UP following MIDI at 133.5: refused (267, over the clock's range)");
      check(midiScale(-1), "B+DOWN following MIDI: halved");
      mult = 0.5;
      runCounted(4);
      measure = true; worst = 0;
      runCounted(8);
      snprintf(m, sizeof m, "halved: half their tempo (%u, theirs %.0f), one of our beats to two of theirs, at most %.4f beats off", transport.spb, expectSpb(133.5), worst);
      check(transport.running && fabs(transport.spb - expectSpb(133.5) * 2) < expectSpb(133.5) * 0.006 && worst < 0.02, m);
      measure = false;
      check(midiScale(1), "B+UP: back");
      mult = 1;
      runCounted(4);
      measure = true; worst = 0;
      runCounted(8);
      snprintf(m, sizeof m, "back: on their beat (and bar) again, at most %.4f beats off", worst);
      check(transport.running && worst < 0.02 && fabs(transport.spb - expectSpb(133.5)) < expectSpb(133.5) * 0.003, m);
      measure = false;
    }
    midiInByte(0xFC, simMicros);
    runCounted(1);
    check(!transport.running, "Stop: the transport stops");
    for (int i = 0; i < 300; i++) { block(); midiInService(); }   // their clock gone (~0.9 s)
    check(!midiClockFollowing(), "their clock gone: not following");
    // Two clocks: the serial one ticking, USB's ignored until the serial one stops.
    auto feed = [&](MidiSource src, double us, int n, double bpmTheirs) {
      double step = 60e6 / bpmTheirs / 24;
      for (int k = 0; k < n; k++) {
        while (simMicros < us) { block(); midiInService(); }
        midiInByte(0xF8, (uint32_t)us, src);
        us += step;
      }
      return us;
    };
    double at = feed(MIDI_SERIAL, simMicros + 1000, 72, 100);
    midiInByte(0xFA, (uint32_t)at - 600, MIDI_SERIAL);   // (the serial one started)
    at = feed(MIDI_SERIAL, at, 24, 100);
    midiInByte(0xF8, (uint32_t)at - 500, MIDI_USB);  // (USB's tick between: ignored)
    midiInByte(0xFC, (uint32_t)at - 400, MIDI_USB);   // (and its stop)
    for (int i = 0; i < 4; i++) { block(); midiInService(); }
    snprintf(m, sizeof m, "two clocks: the serial one followed (%.1f shown), USB's ticks and stop ignored (running: %d)", simShownBpm, transport.running);
    check(midiClockFollowing() && transport.running && fabs(simShownBpm - 100.5) < 1.5, m);
    for (int i = 0; i < 300; i++) { block(); midiInService(); }   // the serial one stops ticking
    check(!midiClockFollowing(), "the serial clock gone: not following");
    transportStop();
    at = feed(MIDI_USB, simMicros + 1000, 48, 150);
    midiInByte(0xFA, (uint32_t)at - 600, MIDI_USB);
    feed(MIDI_USB, at, 24, 150);
    snprintf(m, sizeof m, "then USB's clock, started: followed (%.1f shown)", simShownBpm);
    check(midiClockFollowing() && transport.running && fabs(simShownBpm - 150.7) < 2, m);
    transportStop();
    midiInSuspend(true);                             // storage mode: nothing taken in
    midiInByte(0xFA, simMicros, MIDI_USB);
    for (int i = 0; i < 4; i++) { block(); midiInService(); }
    midiInSuspend(false);
    check(!transport.running, "storage mode: a start meanwhile ignored");
    for (int i = 0; i < 300; i++) { block(); midiInService(); }
    // Song Position: Continue from it (on their next whole beat), a jump to another while they run.
    {
      double usTick = 60e6 / 120 / 24, tAt = simMicros + 1000, theirPos = 0;   // their position (beats) at the next tick
      double worstPos = 0;
      bool measurePos = false;
      auto ticks = [&](int count) {
        for (int k = 0; k < count; k++) {
          while (simMicros < tAt) { block(); midiInService(); }
          midiInByte(0xF8, (uint32_t)tAt, MIDI_SERIAL);
          if (measurePos && transport.running) {
            double ours = transport.beat + (transport.sampleInBeat + ((int32_t)((uint32_t)tAt - simMicros)) / usPerSample) / transport.spb;
            worstPos = std::max(worstPos, fabs(ours - theirPos));
          }
          theirPos += 1.0 / 24;
          tAt += usTick;
        }
      };
      ticks(48);                                     // their clock, stopped
      auto spp = [&](uint16_t pos16) {               // (as the serial in has it: 0xF2, then two data bytes)
        midiInByte(0xF2, simMicros, MIDI_SERIAL);
        midiInByte(pos16 & 0x7F, simMicros, MIDI_SERIAL);
        midiInByte(pos16 >> 7, simMicros, MIDI_SERIAL);
      };
      spp(34);                                       // 8.5 beats in (bar 3, beat 1 and a half)
      midiInByte(0xFB, simMicros, MIDI_SERIAL);     // Continue
      theirPos = 8.5;
      int32_t startedOn = INT32_MIN;
      for (int k = 0; k < 24 && startedOn == INT32_MIN; k++) {
        ticks(1);
        for (int b = 0; b < 3; b++) { block(); midiInService(); }
        if (transport.running) startedOn = transport.beat;
      }
      char m2[200];
      snprintf(m2, sizeof m2, "Continue from Song Position 8.5 beats: started on their next whole beat, %d (their position %.2f)", (int)startedOn, theirPos);
      check(startedOn == 9 && theirPos >= 9 && theirPos < 9.3, m2);
      ticks(24 * 8);                                 // settling
      measurePos = true; worstPos = 0;
      ticks(24 * 8);
      snprintf(m2, sizeof m2, "on their position, bars and all: at most %.4f beats off over 8 beats", worstPos);
      check(worstPos < 0.01, m2);
      measurePos = false;
      spp(64);                                       // a jump while they run: to beat 16
      theirPos = 16;
      startedOn = INT32_MIN;
      for (int k = 0; k < 30 && startedOn == INT32_MIN; k++) {
        ticks(1);
        for (int b = 0; b < 3; b++) { block(); midiInService(); }
        if (transport.running && transport.beat >= 16) startedOn = transport.beat;
      }
      ticks(24 * 8);
      measurePos = true; worstPos = 0;
      ticks(24 * 6);
      snprintf(m2, sizeof m2, "a Song Position while they run: a jump there (from beat %d), then at most %.4f beats off", (int)startedOn, worstPos);
      check(startedOn == 16 && worstPos < 0.01, m2);
      int before = midi.nmsg;
      midiInByte(0xFC, simMicros, MIDI_SERIAL);
      for (int b = 0; b < 4; b++) { block(); midiInService(); }
      check(!transport.running, "Stop: stopped");
      transportStartAt(12);                          // our own start mid-song: Song Position and Continue out
      for (int b = 0; b < 4; b++) block();
      bool sent = false;                             // (the stand-in keeps the last 16 bytes, in a ring)
      for (int k = std::max(0, midi.nmsg - 16); k + 3 < midi.nmsg; k++)
        if (midi.msgs[k % 16] == 0xF2 && midi.msgs[(k + 1) % 16] == ((12 * 4) & 0x7F) && midi.msgs[(k + 2) % 16] == 0 && midi.msgs[(k + 3) % 16] == 0xFB) sent = true;
      snprintf(m2, sizeof m2, "started at beat 12 here: Song Position 48 and Continue sent (%d message bytes)", midi.nmsg - before);
      check(sent, m2);
      transportStop();
      for (int i = 0; i < 300; i++) { block(); midiInService(); }
    }
    // A source that sends no clock while stopped: its Start handled before its first tick (which comes a moment later).
    {
      for (int i = 0; i < 400; i++) { block(); midiInService(); }   // (nothing for over a second)
      midiInByte(0xFA, simMicros, MIDI_USB);
      for (int i = 0; i < 8; i++) { block(); midiInService(); }     // (handled; no tick yet: ~23 ms)
      double tAt = simMicros + 2000, usTick = 60e6 / 120 / 24;
      for (int k = 0; k < 48; k++) {
        while (simMicros < tAt) { block(); midiInService(); }
        midiInByte(0xF8, (uint32_t)tAt, MIDI_USB);
        tAt += usTick;
      }
      for (int i = 0; i < 4; i++) { block(); midiInService(); }
      check(transport.running && midiClockFollowing(), "Start before their first tick (no clock while they were stopped): started all the same, following");
      midiInByte(0xFC, simMicros, MIDI_USB);
      for (int i = 0; i < 4; i++) { block(); midiInService(); }
      for (int i = 0; i < 300; i++) { block(); midiInService(); }
    }
    // Started again at the same tempo (no clock while stopped, the first tick a little late, the rest jittering):
    // the tempo stays as it was through the first bars.
    {
      srand(7);                                      // (the jitter the same whatever ran before; ~1 in 4 others take the
                                                     // speed just past 0.6 % at the start: a follower borderline to look at)
      double speedOff = 0, posOff = 0;               // the speed applied off theirs; our position off theirs (from their 2nd beat)
      auto play = [&](int beats, double bpmTheirs, bool lateFirst, double &worstOff, float &shownMin, float &shownMax) {
        midiInByte(0xFA, simMicros, MIDI_USB);
        double usTick = 60e6 / bpmTheirs / 24, tAt = simMicros + 3000 + (lateFirst ? 2500 : 0);
        for (int k = 0; k < beats * 24; k++) {
          while (simMicros < tAt) { block(); midiInService(); }
          midiInByte(0xF8, (uint32_t)(tAt + (rand() % 1601 - 800)), MIDI_USB);   // (USB: up to 0.8 ms either way)
          if (transport.running && k >= 24) {
            double applied = transport.pendingSpb ? transport.pendingSpb : transport.spb;
            speedOff = std::max(speedOff, fabs(applied - expectSpb(bpmTheirs)) / expectSpb(bpmTheirs));
            double ours = transport.beat + (transport.sampleInBeat + ((int32_t)((uint32_t)tAt - simMicros)) / usPerSample) / transport.spb;
            posOff = std::max(posOff, fabs(ours - k / 24.0));
          }
          if (k == 0 && lateFirst) tAt -= 2500;
          tAt += usTick;
          if (transport.running && k > 0) {
            double theirsShown = 44100.0 * 60 / expectSpb(bpmTheirs);   // (their tempo in the simulator's samples)
            double off = fabs(simShownBpm - theirsShown) / theirsShown;
            worstOff = std::max(worstOff, off);
            shownMin = std::min(shownMin, simShownBpm);
            if (getenv("TRACE_PLAY")) printf("    play k %d shown %.2f theirs %u applied %u\n", k, simShownBpm, midiTheirSpb(), transport.pendingSpb ? transport.pendingSpb : transport.spb);
            shownMax = std::max(shownMax, simShownBpm);
          }
        }
        midiInByte(0xFC, simMicros, MIDI_USB);
        for (int i = 0; i < 400; i++) { block(); midiInService(); }   // (stopped, no clock: over a second)
      };
      double off = 0;
      float lo = 1e9f, hi = 0;
      play(12, 120, false, off, lo, hi);             // a first play: its tempo learnt
      off = 0; lo = 1e9f; hi = 0;
      speedOff = 0; posOff = 0;
      play(8, 120, true, off, lo, hi);               // played again, the same tempo
      char m3[200];
      snprintf(m3, sizeof m3, "played again at the same tempo: the tempo shown %.1f-%.1f, at most %.2f%% off theirs", lo, hi, off * 100);
      check(hi - lo < 0.15f && off < 0.001, m3);
      snprintf(m3, sizeof m3, "and started where they are: the speed at most %.2f%% off theirs, our position at most %.4f beats off theirs from the start",
               speedOff * 100, posOff);
      check(speedOff < 0.006 && posOff < 0.01, m3);
    }
    // Their tempo changing while playing: locked onto quickly.
    {
      midiInByte(0xFA, simMicros, MIDI_USB);
      double tAt = simMicros + 2000, usTick = 60e6 / 120 / 24;
      auto tick = [&]() {
        while (simMicros < tAt) { block(); midiInService(); }
        midiInByte(0xF8, (uint32_t)(tAt + (rand() % 1601 - 800)), MIDI_USB);
        tAt += usTick;
      };
      for (int k = 0; k < 24 * 8; k++) tick();
      usTick = 60e6 / 123 / 24;                      // 120 to 123
      int lockedAfter = -1;
      for (int k = 0; k < 24 * 4; k++) {
        tick();
        for (int b = 0; b < 2; b++) { block(); midiInService(); }
        double got = midiTheirSpb(), wantSpb = expectSpb(123);   // (their tempo as measured: within 0.2 %)
        if (lockedAfter < 0 && fabs(got - wantSpb) / wantSpb <= 0.002) lockedAfter = k + 1;
        if (lockedAfter >= 0 && fabs(got - wantSpb) / wantSpb > 0.002) lockedAfter = -1;   // (it has to stay)
      }
      char m4[160];
      snprintf(m4, sizeof m4, "their tempo 120 to 123 while playing: locked onto (measured within 0.2%%; shown %.1f) after %d ticks", simShownBpm, lockedAfter);
      check(lockedAfter > 0 && lockedAfter <= 24, m4);
      midiInByte(0xFC, simMicros, MIDI_USB);
      for (int i = 0; i < 400; i++) { block(); midiInService(); }
    }
    // A DAW's clock over USB as the M8 really got it (data/daw_clock_usb.txt: 72 BPM with ticks off by up
    // to ±2 ms, its first after a Start early; a change to 83 while playing): the speed played near theirs.
    {
      FILE *f = fopen("data/daw_clock_usb.txt", "r");
      check(f != nullptr, "(the captured DAW clock: data/daw_clock_usb.txt)");
      std::vector<std::pair<char, uint32_t>> ev;
      char line[64];
      while (f && fgets(line, sizeof line, f)) {
        if (line[0] == 'T') ev.push_back({'T', (uint32_t)strtoul(line + 2, nullptr, 10)});
        else if (line[0] == 'A' || line[0] == 'C') ev.push_back({line[0], 0});
      }
      if (f) fclose(f);
      for (int i = 0; i < 400; i++) { block(); midiInService(); }
      uint32_t base = simMicros + 5000, lastT = 0;
      int run = 0, sinceStart = -1;
      double steadyOff = 0, changeOff = 0, posOffEnd[4] = {}, theirBpm[4] = {};
      int stillOff[4] = {};
      for (size_t e = 0; e < ev.size(); e++) {
        if (ev[e].first == 'A' || ev[e].first == 'C') {
          uint32_t next = lastT;                     // (just before the next tick)
          for (size_t k = e; k < ev.size(); k++) if (ev[k].first == 'T') { next = ev[k].second; break; }
          uint32_t at = base + next - 1000;
          while ((int32_t)(simMicros - at) < 0) { block(); midiInService(); }
          midiInByte(ev[e].first == 'A' ? 0xFA : 0xFC, simMicros, MIDI_USB);
          if (ev[e].first == 'A') { run++; sinceStart = 0; }
          else sinceStart = -1;
          continue;
        }
        lastT = ev[e].second;
        uint32_t at = base + ev[e].second;
        while ((int32_t)(simMicros - at) < 0) { block(); midiInService(); }
        midiInByte(0xF8, at, MIDI_USB);
        if (sinceStart < 0) continue;
        sinceStart++;
        uint32_t theirs = midiTheirSpb(), applied = transport.pendingSpb ? transport.pendingSpb : transport.spb;
        if (!transport.running || !theirs || sinceStart < 24 || run > 3) continue;
        double off = fabs((double)applied - theirs) / theirs;
        bool around = sinceStart < 48 || (run == 2 && sinceStart > 440 && sinceStart < 600);   // (a start, their change to 83: catching up allowed)
        if (around) changeOff = std::max(changeOff, off);
        else steadyOff = std::max(steadyOff, off);
        if (!around && off > 0.0066) stillOff[run]++;
        if (getenv("TRACE_DAW") && off > 0.0051) printf("    off %.2f%% run %d tick %d\n", off * 100, run, sinceStart);
        double ours = transport.beat + (transport.sampleInBeat + (int32_t)(at - simMicros) / usPerSample) / transport.spb;
        posOffEnd[run] = fabs(ours - (sinceStart - 1) / 24.0);
        if (getenv("TRACE_DAW") && (sinceStart % 12 == 0 || (run == 1 && sinceStart > 166 && sinceStart < 182)))
          printf("    run %d tick %d: theirs %u applied %u, ours %.3f theirs %.3f (beat %d in %u spb %u t.now %u)\n", run, sinceStart, theirs, applied, ours, (sinceStart - 1) / 24.0, (int)transport.beat, (unsigned)transport.sampleInBeat, (unsigned)transport.spb, (unsigned)transport.now);
        theirBpm[run] = 44100.0 * 60 / theirs / 1.005;   // (the simulator's micros() run ~0.5 % fast)
      }
      for (int i = 0; i < 4; i++) { block(); midiInService(); }
      char m5[240];
      snprintf(m5, sizeof m5, "a real DAW clock: their tempo read %.1f, %.1f, %.1f; the speed at most %.2f%% off theirs while steady (%d, %d, %d ticks over 0.66%%), %.2f%% starting and around their change",
               theirBpm[1], theirBpm[2], theirBpm[3], steadyOff * 100, stillOff[1], stillOff[2], stillOff[3], changeOff * 100);
      check(steadyOff < 0.0066 && changeOff < 0.051 && fabs(theirBpm[1] - 72) < 0.5 && fabs(theirBpm[2] - 83) < 0.5, m5);
      snprintf(m5, sizeof m5, "and on their beat at each run's end: %.4f, %.4f, %.4f beats off", posOffEnd[1], posOffEnd[2], posOffEnd[3]);
      check(posOffEnd[1] < 0.02 && posOffEnd[2] < 0.02 && posOffEnd[3] < 0.02, m5);
      if (transport.running) transportStop();
      for (int i = 0; i < 400; i++) { block(); midiInService(); }
    }
    // Their tempo remembered: our tempo after their Stop is its average (no nudge), and their next start
    // begins from it; a clock clearly at another tempo taken at once.
    {
      double tAt = simMicros + 2000, usTick = 60e6 / 120 / 24;
      auto tick = [&]() {
        while (simMicros < tAt) { block(); midiInService(); }
        midiInByte(0xF8, (uint32_t)(tAt + (rand() % 1601 - 800)), MIDI_USB);
        tAt += usTick;
      };
      auto applied = [&]() { return (double)(transport.pendingSpb ? transport.pendingSpb : transport.spb); };
      midiInByte(0xFA, simMicros, MIDI_USB);
      for (int k = 0; k < 24 * 16; k++) tick();
      midiInByte(0xFC, simMicros, MIDI_USB);
      for (int i = 0; i < 4; i++) { block(); midiInService(); }
      double want = expectSpb(120), after = applied();
      char m8[200];
      snprintf(m8, sizeof m8, "their Stop: our tempo their average (%.0f, theirs %.0f: %.3f%% off), no nudge left", after, want, fabs(after - want) / want * 100);
      snprintf(m8 + strlen(m8), sizeof m8 - strlen(m8), " [running %d, theirs measured %u]", transport.running, midiTheirSpb());
      check(!transport.running && fabs(after - want) / want < 0.0008 && abs((int)after - (int)midiTheirSpb()) <= 2, m8);
      for (int i = 0; i < 400; i++) { block(); midiInService(); }   // (stopped, no clock: over a second)
      tAt = simMicros + 2000;
      midiInByte(0xFA, simMicros, MIDI_USB);
      double worst = 0;
      for (int k = 0; k < 24; k++) {
        tick();
        if (transport.running) worst = std::max(worst, fabs(applied() - want) / want);
      }
      snprintf(m8, sizeof m8, "started again: from the remembered tempo through the first beat (at most %.2f%% off theirs)", worst * 100);
      check(worst < 0.006, m8);
      for (int k = 0; k < 24 * 4; k++) tick();
      midiInByte(0xFC, simMicros, MIDI_USB);
      for (int i = 0; i < 4; i++) { block(); midiInService(); }
      usTick = 60e6 / 130 / 24;                      // their clock now clearly at another tempo (sent while stopped)
      for (int k = 0; k < 24 * 2; k++) tick();
      midiInByte(0xFA, simMicros, MIDI_USB);
      for (int k = 0; k < 24 * 2; k++) tick();
      double now130 = applied(), want130 = expectSpb(130);
      snprintf(m8, sizeof m8, "their clock clearly at another tempo: that one, not the remembered (%.0f, theirs %.0f)", now130, want130);
      check(fabs(now130 - want130) / want130 < 0.006, m8);
      midiInByte(0xFC, simMicros, MIDI_USB);
      for (int i = 0; i < 400; i++) { block(); midiInService(); }
    }
    // The clock out for USB too: Start, the ticks, Stop, as the serial one's.
    midiOutAttachUsb();
    uint8_t got[64];
    int ticks = 0, startSeen = 0, stopSeen = 0;
    auto take = [&]() {
      int n = midiOutTakeUsb(simMicros + 10000, got, sizeof got);
      for (int k = 0; k < n; k++) ticks += got[k] == 0xF8, startSeen += got[k] == 0xFA, stopSeen += got[k] == 0xFC;
    };
    transportStart();
    for (int i = 0; i < 2000 && ticks < 48; i++) { block(); take(); }
    transportStop();
    take();
    snprintf(m, sizeof m, "USB's clock out: start %d, %d ticks (2 beats), stop %d", startSeen, ticks, stopSeen);
    check(startSeen == 1 && ticks >= 48 && stopSeen == 1, m);
    simSetTempo(120);
    runBeats(1);
  }

  printf("USB audio in: mixed with the line input; the line input left out while it's only noise\n");
  {
    auto ringL = [&]() { return (int16_t)(inputRing[(transport.now - 1) % INPUT_FRAMES] & 0xFFFF); };
    feedRamp = false; feedSineHz = 0;
    feedUsb = 3000;
    feedNoise = true;                                // nothing plugged into the line input
    for (int i = 0; i < 40; i++) block();
    int16_t early = ringL();
    for (int i = 0; i < 1000; i++) block();          // (~3 s: quiet past the hold)
    int16_t later = ringL();
    char m6[200];
    snprintf(m6, sizeof m6, "the line input's noise mixed in at first (%d), left out once quiet a while (%d: USB's alone, 3000)", early, later);
    check(abs(early - 3000) <= 10 && later == 3000, m6);
    feedNoise = false; feedSineHz = 440;             // a signal on the line input: in again at once
    block(); block();
    int16_t withLine = ringL();
    int32_t sineNow = (int16_t)lround(8000 * sin(2 * M_PI * 440 * (inputIndex - 1) / AUDIO_SAMPLE_RATE_EXACT));
    snprintf(m6, sizeof m6, "a signal on the line input: mixed in again at once (%d = USB's 3000 + the line's %d)", withLine, (int)sineNow);
    check(abs(withLine - (3000 + sineNow)) <= 2, m6);
    feedSineHz = 0; feedNoise = true;
    for (int i = 0; i < 1000; i++) block();
    feedUsb = 0;                                     // USB audio stops: the line input as ever
    block(); block();
    snprintf(m6, sizeof m6, "USB audio gone: the line input in again, noise and all (%d)", ringL());
    check(abs(ringL()) <= 10, m6);
    feedNoise = false;
    for (int i = 0; i < 4; i++) block();
  }

  printf("USB audio in: its latency measured from the computer's beats, recordings taken with it\n");
  {
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) clearTrack(t);
    if (!transport.running) transportStart();
    feedRamp = false; feedSineHz = 0;
    feedUsbClick = 900;                              // their click 900 samples (20.4 ms) after each of our beats
    runBeats(12);
    char m10[200];
    snprintf(m10, sizeof m10, "measured: %u samples (their click 900 after our beats)", (unsigned)usbInputLatency);
    check(abs((int)usbInputLatency - 900) <= 3, m10);
    check(inputLatency() == usbInputLatency, "the input's latency USB's while USB audio comes in");
    recordPress(3);                                  // a loop of what comes in
    for (int i = 0; i < 4000 && recording.mode != Recording::Running; i++) block();
    check(recording.latency == usbInputLatency, "a recording's latency: USB's, kept to its end");
    runBeats(4.5);
    recordPress(3);
    for (int i = 0; i < 200000 && recording.mode != Recording::Idle; i++) block();
    int at = -1, peak = 0;                           // where the click is in the loop's first beat
    for (int k = 0; k < (int)transport.spb / 2; k++) if (abs(wavLeft(3, k)) > peak) { peak = abs(wavLeft(3, k)); at = k; }
    snprintf(m10, sizeof m10, "the loop's click at its start, on the beat (frame %d, not 900)", at);
    check(at >= 0 && at < 8 && peak > 10000, m10);
    feedUsbClick = -1;
    for (int i = 0; i < 4; i++) block();
    check(inputLatency() == LATENCY_FRAMES, "no USB audio: the line input's latency again");
    clearTrack(3);
  }

  printf("USB audio out: the mix as the line out has it, without the metronome\n");
  {
    bool wasMonitor = inputMonitored;
    inputMonitored = true;
    feedSineHz = 440;
    if (!transport.running) transportStart();
    int vol = masterVolume();
    setMasterVolume(MASTER_VOLUME_UNITY - 2);        // turned down
    auto lineMinusUsb = [](int blocks, int &usbPeak) {   // the line out's left less USB's main left, at most
      int worst = 0;
      usbPeak = 0;
      for (int b = 0; b < blocks; b++) {
        block();
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
          int16_t u = usbOut[i * USB_OUT_CHANNELS];
          usbPeak = std::max(usbPeak, abs((int)u));
          worst = std::max(worst, abs(outL.data[i] - u));
        }
      }
      return worst;
    };
    int peakUsb = 0;
    for (int i = 0; i < 6; i++) block();
    int off = lineMinusUsb(4 * (int)(transport.spb / AUDIO_BLOCK_SAMPLES), peakUsb);   // (4 beats)
    bool wasClick = metronomeOn;
    metronomeOn = true;
    int unusedPeak = 0, on = lineMinusUsb(4 * (int)(transport.spb / AUDIO_BLOCK_SAMPLES), unusedPeak);
    metronomeOn = wasClick;
    char m7[240];
    snprintf(m7, sizeof m7, "master volume turned down: USB's mix follows it (peak %d of the sine's 8000), the line out's same (within %d); "
             "the metronome on: only on the line out (%d off USB)", peakUsb, off, on);
    check(peakUsb > 2000 && peakUsb < 7000 && off <= 1 && on > 1000, m7);
    {                                                // the master balance (the input: a sine left, its inverse right)
      auto peaks = [](int bal, int &pl, int &pr) {
        setMasterBalance(bal);
        for (int i = 0; i < 60; i++) block();          // (the master's glide: settled)
        pl = pr = 0;
        for (int b = 0; b < 16; b++) {
          block();
          for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { pl = std::max(pl, abs((int)outL.data[i])); pr = std::max(pr, abs((int)outR.data[i])); }
        }
      };
      int l0, r0, lh, rh, lf, rf;
      peaks(0, l0, r0);
      peaks(MASTER_BALANCE_MAX / 2, lh, rh);
      peaks(MASTER_BALANCE_MAX, lf, rf);
      setMasterBalance(0);
      snprintf(m7, sizeof m7, "master balance: centred %d / %d; half right %d / %d (half the left moved right: both halved); right: the left silent (%d)",
               l0, r0, lh, rh, lf);
      check(l0 > 2000 && abs(l0 - r0) < 50 && abs(2 * lh - l0) < 60 && abs(2 * rh - r0) < 60 && lf < 50, m7);
    }
    setMasterVolume(vol);
    feedSineHz = 0;
    inputMonitored = wasMonitor;
    for (int i = 0; i < 4; i++) block();
  }

  printf("USB audio out: each track as heard, beside the mix\n");
  {
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) clearTrack(t);
    if (!transport.running) transportStart();
    bool wasMonitor = inputMonitored;
    inputMonitored = false;                          // (the mix: the tracks alone)
    feedRamp = false; feedSineHz = 300;
    for (int t : {1, 4}) {                           // two loops of the sine
      recordPress(t);
      for (int i = 0; i < 4000 && recording.mode != Recording::Running; i++) block();
      runBeats(4.5);
      recordPress(t);
      for (int i = 0; i < 200000 && recording.mode != Recording::Idle; i++) block();
    }
    feedSineHz = 0;
    auto stemPeaks = [](int *pk) {                   // each track's loudest on USB over 40 blocks (left, right)
      for (int c = 0; c < 2 * NUM_TRACKS; c++) pk[c] = 0;
      for (int b = 0; b < 40; b++) {
        block();
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
          for (int c = 0; c < 2 * NUM_TRACKS; c++) pk[c] = std::max(pk[c], abs((int)usbOut[i * USB_OUT_CHANNELS + usbTrackChannel(c / 2) + c % 2]));
      }
    };
    int before[2 * NUM_TRACKS], after[2 * NUM_TRACKS];
    stemPeaks(before);
    setTrackMix(1, VOLUME_UNITY, BALANCE_MAX);       // track 2: hard right (its left moved into its right; the test
                                                     // input's right is its left inverted, so they cancel there too)
    setTrackMix(4, VOLUME_UNITY - 4, 0);             // track 5: 7.5 dB down
    for (int b = 0; b < 20; b++) block();            // (the levels glide there)
    stemPeaks(after);
    int worst = 0, others = 0;
    for (int b = 0; b < 40; b++) {
      block();
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        const int16_t *f = usbOut + i * USB_OUT_CHANNELS;
        int sumL = 0, sumR = 0;
        for (int t = 0; t < NUM_TRACKS; t++) { sumL += f[usbTrackChannel(t)]; sumR += f[usbTrackChannel(t) + 1]; }
        worst = std::max(worst, std::max(abs(sumL - f[0]), abs(sumR - f[1])));
        for (int t = 0; t < NUM_TRACKS; t++) if (t != 1 && t != 4) others = std::max(others, abs((int)f[usbTrackChannel(t)]) + abs((int)f[usbTrackChannel(t) + 1]));
      }
    }
    char m8[260];
    snprintf(m8, sizeof m8, "the tracks add up to the mix (within %d); track 2 there (%d), then hard right: its left silent (%d); "
             "track 5 7.5 dB down: %d to %d; the empty ones silent (%d)", worst, before[2], after[2], before[8], after[8], others);
    check(worst <= NUM_TRACKS && before[2] > 3000 && after[2] < 50 && before[8] > 3000 &&
          after[8] > before[8] * 0.3 && after[8] < before[8] * 0.6 && others == 0, m8);
    setTrackMuted(4, true);
    for (int b = 0; b < 40; b++) block();
    int muted = 0;
    for (int b = 0; b < 10; b++) {
      block();
      for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) muted = std::max(muted, abs((int)usbOut[i * USB_OUT_CHANNELS + usbTrackChannel(4)]));
    }
    snprintf(m8, sizeof m8, "track 5 muted: silent on USB too (%d)", muted);
    check(muted == 0, m8);
    snprintf(m8, sizeof m8, "the top row's track on channels 3-4, the bottom row's on 15-16 (track 5 on %d-%d, track 2 on %d-%d)",
             usbTrackChannel(4) + 1, usbTrackChannel(4) + 2, usbTrackChannel(1) + 1, usbTrackChannel(1) + 2);
    check(usbTrackChannel(NUM_TRACKS - 1) == 2 && usbTrackChannel(0) == 14, m8);
    usbStreaming = false;                            // the computer not taking it: nothing written, all the same
    std::fill(usbOut, usbOut + AUDIO_BLOCK_SAMPLES * USB_OUT_CHANNELS, 12345);
    block();
    check(usbOut[0] == 12345, "not streaming: nothing written");
    usbStreaming = true;
    setTrackMuted(4, false);
    setTrackMix(1, VOLUME_UNITY, 0);
    setTrackMix(4, VOLUME_UNITY, 0);
    inputMonitored = wasMonitor;
    for (int t : {1, 4}) clearTrack(t);
    for (int i = 0; i < 4; i++) block();
  }

  printf("recording once A is let go: the late press's window counted from the press\n");
  {
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) clearTrack(t);
    if (!transport.running) transportStart();
    feedRamp = true;
    const double fps = AUDIO_SAMPLE_RATE_EXACT / AUDIO_BLOCK_SAMPLES;
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < 4000 && !(transport.beat % BEATS_PER_BAR == 1); i++) block();   // (into a bar)
      for (int i = 0; i < 4000 && !(transport.beat % BEATS_PER_BAR == 0 && transport.sampleInBeat < AUDIO_BLOCK_SAMPLES); i++) block();
      uint32_t bar = transport.lastBarSample;
      for (int i = 0; i < (int)(0.05 * fps); i++) block();   // pressed 50 ms after the bar line
      uint32_t pressAt = transport.now;
      for (int i = 0; i < (int)(0.15 * fps); i++) block();  // let go 150 ms later
      bool counted = pass == 0;
      recordPress(3, false, counted ? transport.now - pressAt : 0);
      char m11[200];
      if (counted) {
        snprintf(m11, sizeof m11, "pressed 50 ms after a bar line, let go 150 ms later: from that bar line (%s)",
                 recording.mode == Recording::Running ? "running" : "waiting");
        check(recording.mode == Recording::Running && recording.startSample == bar + recording.latency, m11);
      } else {
        check(recording.mode == Recording::WaitBar, "the same moment, the hold not counted: the next bar line (as a press 200 ms late: past the 100 ms grace)");
      }
      cancelRecording(3);
      for (int i = 0; i < 20; i++) block();
    }
    feedRamp = false;
  }

  printf("audio follow: the tempo and the beat heard in the input\n");
  {
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) clearTrack(t);
    if (transport.running) transportStop();
    simSetTempo(120);
    bpm = 120;
    feedRamp = false; feedSineHz = 0;
    const double fps = AUDIO_SAMPLE_RATE_EXACT / AUDIO_BLOCK_SAMPLES;
    std::vector<uint32_t> ours;                      // our beats' first samples (beatLog)
    int32_t lastLogged = INT32_MIN;
    auto run = [&](double sec) {
      for (int n = (int)(sec * fps); n > 0; n--) {
        block();
        for (int k = 0; k < BEAT_LOG; k++) {
          const volatile BeatStart &b = beatLog[k];
          if (transport.running && b.spb && b.beat > lastLogged && b.beat <= transport.beat) { lastLogged = b.beat; ours.push_back((uint32_t)b.now); }
        }
      }
    };
    // How far our beats are off theirs (ms: the mean, the worst) over the kicks heard from `from` on: their
    // kick heard at s is on our beat at s - the input's latency.
    auto offBy = [&](uint32_t from, double &mean, double &worst) {
      mean = 0, worst = 0;
      int n = 0;
      for (uint32_t k : drumKicks) {
        if ((int32_t)(k - from) < 0) continue;
        uint32_t want = k - inputLatency();
        double nearest = 1e9;
        for (uint32_t o : ours) if (fabs((double)(int32_t)(o - want)) < fabs(nearest)) nearest = (double)(int32_t)(o - want);
        if (fabs(nearest) > AUDIO_SAMPLE_RATE_EXACT) continue;   // (none near)
        double ms = nearest * 1000 / AUDIO_SAMPLE_RATE_EXACT;
        mean += ms;
        worst = std::max(worst, fabs(ms));
        n++;
      }
      if (n) mean /= n;
      return n;
    };
    drumBpm = 126; drumBeat = 0.37; drumKicks.clear();
    char m9[260];
    audioFollowToggle();
    check(audioFollowOn() && onsetListen, "B+A: on, the input listened to");
    float firstShown = 0;
    double firstAt = 0;
    int earlyFlips = 0;
    bool earlyWas = audioFollowFlash();
    for (int k = 0; k < (int)(10 * fps); k++) {     // stopped: the tempo theirs as soon as it's found (before the beat is)
      run(1 / fps);
      if (!firstShown && bpm != 120) { firstShown = bpm; firstAt = k / fps; }
      if (k < 4 * fps && audioFollowFlash() != earlyWas) { earlyWas = !earlyWas; earlyFlips++; }
    }
    snprintf(m9, sizeof m9, "still listening (nothing found yet): flashing at the tempo shown (%d changes in 4 s: 8 beats at 120)", earlyFlips);
    check(earlyFlips >= 15 && earlyFlips <= 17, m9);
    snprintf(m9, sizeof m9, "stopped: the tempo theirs as soon as it's found (%.1f shown after %.1f s)", firstShown, firstAt);
    check(fabs(firstShown - 126) < 0.5f && firstAt < 6.5, m9);
    snprintf(m9, sizeof m9, "drums at 126 BPM, stopped: their beat found (%d), the tempo theirs (%.1f shown, %.3f found)",
             audioFollowLocked(), bpm, audioFollowBpm());
    check(audioFollowLocked() && fabs(bpm - 126) <= 0.15f && fabs(audioFollowBpm() - 126) < 0.2f, m9);
    int flips = 0;
    bool was = audioFollowFlash();
    for (int n = 0; n < (int)(4 * fps); n++) { block(); if (audioFollowFlash() != was) { was = !was; flips++; } }
    snprintf(m9, sizeof m9, "stopped: the tempo flashes with their beat (%d changes in 4 s: 8.4 beats)", flips);
    check(flips >= 15 && flips <= 19, m9);
    {                                                // the input silent a while (the line input's noise floor): nothing changes
      float before = bpm;
      uint32_t spbBefore = transport.pendingSpb ? transport.pendingSpb : transport.spb;
      feedNoise = true;
      double lostAfter = -1;
      bool changed = false;
      for (int k = 0; k < (int)(12 * fps); k++) {
        run(1 / fps);
        if (lostAfter < 0 && !audioFollowLocked()) lostAfter = k / fps;
        if (bpm != before || (transport.pendingSpb ? transport.pendingSpb : transport.spb) != spbBefore) changed = true;
      }
      snprintf(m9, sizeof m9, "stopped, 12 s of silence: the tempo kept (%.2f, before %.2f; changed %d), the beat counted as lost after %.1f s (black flashing)",
               bpm, before, changed, lostAfter);
      check(!changed && lostAfter > 1.0 && lostAfter < 2.5 && !audioFollowLocked() && audioFollowOn(), m9);
      feedNoise = false;
      drumBeat += 0.4;                               // back, the same tempo, its beat elsewhere
      double relockAfter = -1;
      float lo = bpm, hi = bpm;
      for (int k = 0; k < (int)(15 * fps) && relockAfter < 0; k++) {
        run(1 / fps);
        lo = std::min(lo, bpm);
        hi = std::max(hi, bpm);
        if (audioFollowLocked()) relockAfter = k / fps;
      }
      snprintf(m9, sizeof m9, "sound again (its beat 0.4 later): their beat found again after %.1f s, the tempo steady meanwhile (%.2f-%.2f)", relockAfter, lo, hi);
      check(relockAfter > 0 && relockAfter < 6 && hi - lo <= 0.1f && fabs(bpm - 126) <= 0.15f, m9);
      run(4);
    }
    check(audioFollowStart(), "START: in step with them");
    for (int n = 0; n < 2000 && !transport.running; n++) block();
    uint32_t startedAt = inputIndex;
    run(4);
    double mean, worst;
    int n = offBy(startedAt, mean, worst);
    snprintf(m9, sizeof m9, "started on their beat: our beats on theirs from the first (%d kicks, mean %+.1f ms, worst %.1f ms)", n, mean, worst);
    check(n >= 7 && worst < 12, m9);
    run(20);
    n = offBy(inputIndex - (uint32_t)(15 * AUDIO_SAMPLE_RATE_EXACT), mean, worst);
    snprintf(m9, sizeof m9, "running: kept on their beat (the last 15 s: %d kicks, mean %+.1f ms, worst %.1f ms; %.2f BPM shown)", n, mean, worst, bpm);
    check(n >= 25 && fabs(mean) < 3 && worst < 10 && fabs(bpm - 126) <= 0.15f, m9);
    {                                                // SHIFT+RIGHT x5 on the status row: our beat 50 ms after theirs
      syncOffsetMs[SYNC_AUDIO] = 50;
      syncOffsetChanged(SYNC_AUDIO);
      run(12);
      n = offBy(inputIndex - (uint32_t)(5 * AUDIO_SAMPLE_RATE_EXACT), mean, worst);
      snprintf(m9, sizeof m9, "offset +50 ms: our beats that much after theirs (the last 5 s: %d kicks, mean %+.1f ms)", n, mean);
      check(syncSource() == SYNC_AUDIO && n >= 8 && fabs(mean - 50) < 4, m9);
      syncOffsetMs[SYNC_AUDIO] = 0;
      syncOffsetChanged(SYNC_AUDIO);
      run(12);
      n = offBy(inputIndex - (uint32_t)(5 * AUDIO_SAMPLE_RATE_EXACT), mean, worst);
      snprintf(m9, sizeof m9, "back to 0: on their beat again (mean %+.1f ms)", mean);
      check(n >= 8 && fabs(mean) < 3, m9);
    }
    {                                                // B+arrows: halved, then doubled back; 252 out of range
      check(audioFollowScale(-1), "B+DOWN: halved");
      run(16);
      int on = 0, all = 0;
      uint32_t from = inputIndex - (uint32_t)(6 * AUDIO_SAMPLE_RATE_EXACT);
      for (uint32_t k : drumKicks) {
        if ((int32_t)(k - from) < 0) continue;
        uint32_t want = k - inputLatency();
        double nearest = 1e9;
        for (uint32_t o : ours) if (fabs((double)(int32_t)(o - want)) < fabs(nearest)) nearest = (double)(int32_t)(o - want);
        all++;
        if (getenv("FOLLOW_TRACE")) printf("    kick off by %.1f ms\n", nearest * 1000 / AUDIO_SAMPLE_RATE_EXACT);
        if (fabs(nearest) * 1000 / AUDIO_SAMPLE_RATE_EXACT < 10) on++;
      }
      snprintf(m9, sizeof m9, "halved: %.2f BPM shown, every other kick of theirs on our beat (%d of %d)", bpm, on, all);
      check(fabs(bpm - 63) <= 0.15f && all >= 10 && on >= all / 2 - 1 && on <= (all + 1) / 2, m9);
      check(audioFollowScale(1), "B+UP: doubled back");
      run(16);
      n = offBy(inputIndex - (uint32_t)(6 * AUDIO_SAMPLE_RATE_EXACT), mean, worst);
      snprintf(m9, sizeof m9, "doubled back: on their beat again (the last 6 s: %d kicks, worst %.1f ms; %.2f BPM)", n, worst, bpm);
      check(n >= 10 && worst < 10 && fabs(bpm - 126) <= 0.15f, m9);
      check(!audioFollowScale(1) && fabs(bpm - 126) <= 0.15f, "doubled again (252): out of the clock's range, refused");
    }
    for (int k = 0; k < 8 * (int)fps; k++) {        // their tempo rising to 132 over 8 s
      drumBpm = 126 + 6.0 * k / (8 * fps);
      run(1 / fps);
    }
    run(6);
    n = offBy(inputIndex - (uint32_t)(5 * AUDIO_SAMPLE_RATE_EXACT), mean, worst);
    snprintf(m9, sizeof m9, "theirs rising to 132 BPM: followed (%.2f shown; the last 5 s: mean %+.1f ms, worst %.1f ms)", bpm, mean, worst);
    check(fabs(bpm - 132) <= 0.3f && worst < 10, m9);
    drumBpm = 0;                                     // silence: the beat lost, the clock as it was
    run(8);
    snprintf(m9, sizeof m9, "silence: their beat lost (%d), the tempo kept (%.2f)", audioFollowLocked(), bpm);
    check(!audioFollowLocked() && fabs(bpm - 132) <= 0.35f, m9);
    transportStop();
    audioFollowToggle();
    check(!audioFollowOn() && !onsetListen && !audioFollowFlash(), "B+A again: off, nothing listened to, no flashing");
    // Their tempo far from ours: not taken for its half or double (the tempo set nearer it than them).
    {                                                // halved before anything's found: the estimate finds that octave
      simSetTempo(140);
      bpm = 140;
      run(0.1);
      drumBpm = 140; drumBeat = 0; drumKicks.clear();
      audioFollowToggle();
      check(audioFollowScale(-1) && bpm == 70, "nothing found yet: B+DOWN halves the tempo shown (70)");
      run(12);
      snprintf(m9, sizeof m9, "drums at 140, halved first: their tempo found as 70 (%.2f found, %.1f shown)", audioFollowBpm(), bpm);
      check(fabs(audioFollowBpm() - 70) < 0.5f && fabs(bpm - 70) <= 0.3f, m9);
      audioFollowToggle();
      drumBpm = 0;
      run(1);
    }
    for (auto [their, set] : std::vector<std::pair<double, float>>{{90, 120}, {174, 150}, {70, 85}, {174, 100}}) {
      simSetTempo(set);
      bpm = set;
      run(0.1);
      drumBpm = their; drumBeat = 0; drumKicks.clear();
      audioFollowToggle();
      run(10);
      double expect = their / set > 1.5 ? their / 2 : their;   // (twice the tempo set: its half, a beat every two of theirs)
      snprintf(m9, sizeof m9, "drums at %.0f BPM, the tempo set %.0f: %.1f found (%d)", their, set, audioFollowBpm(), audioFollowLocked());
      check(audioFollowLocked() && fabs(audioFollowBpm() - expect) < 0.3, m9);
      audioFollowToggle();
    }
    // Over USB audio (a computer playing into L7), the line input silent: the same.
    simSetTempo(120);
    bpm = 120;
    drumsOverUsb = true;
    drumBpm = 104; drumBeat = 0.6; drumKicks.clear(); ours.clear(); lastLogged = INT32_MIN;
    audioFollowToggle();
    run(10);
    snprintf(m9, sizeof m9, "drums at 104 BPM over USB audio: found (%.1f shown, %d)", bpm, audioFollowLocked());
    check(audioFollowLocked() && fabs(bpm - 104) <= 0.15f, m9);
    audioFollowStart();
    for (int k = 0; k < 2000 && !transport.running; k++) block();
    run(10);
    n = offBy(inputIndex - (uint32_t)(6 * AUDIO_SAMPLE_RATE_EXACT), mean, worst);
    snprintf(m9, sizeof m9, "running: on their beat less USB's input latency (%u samples; %d kicks, mean %+.1f ms, worst %.1f ms)",
             (unsigned)inputLatency(), n, mean, worst);
    check(n >= 8 && fabs(mean) < 3 && worst < 10, m9);
    transportStop();
    audioFollowToggle();
    drumsOverUsb = false;
    drumBpm = 0;
    simSetTempo(120);
    run(0.2);
  }

  printf("time stretching: the clip at its own pitch whatever the tempo\n");
  {
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) clearTrack(t);
    if (transport.running) transportStop();
    simSetTempo(120);
    runBeats(0.1);
    bool wasMonitor = inputMonitored;
    feedRamp = false; feedSineHz = 440;
    transportStart();
    recordPress(2);                                  // a bar of a 440 Hz sine at 120 BPM
    for (int i = 0; i < 4000 && recording.mode != Recording::Running; i++) block();
    runBeats(3.5);
    recordPress(2);
    for (int i = 0; i < 200000 && recording.mode != Recording::Idle; i++) block();
    feedSineHz = 0;
    inputMonitored = false;                          // (the track alone)
    // Its frequency, its level's spread and its biggest step over 40 blocks of the output.
    auto measure = [&](double &hz, double &spread, double &step) {
      std::vector<float> v;
      for (int b = 0; b < 40; b++) { block(); for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) v.push_back(outL.data[i]); }
      double first = -1, last = -1;
      int crossings = 0;
      for (size_t i = 1; i < v.size(); i++)
        if (v[i - 1] < 0 && v[i] >= 0) {
          double at = i - 1 + v[i - 1] / (v[i - 1] - v[i]);
          if (first < 0) first = at; else crossings++;
          last = at;
        }
      hz = crossings > 0 ? crossings * AUDIO_SAMPLE_RATE_EXACT / (last - first) : 0;
      double lo = 1e9, hi = 0;
      for (size_t k = 0; k + 441 <= v.size(); k += 441) {          // (10 ms windows' RMS)
        double e = 0;
        for (size_t i = k; i < k + 441; i++) e += v[i] * v[i];
        e = sqrt(e / 441);
        lo = std::min(lo, e);
        hi = std::max(hi, e);
      }
      spread = hi > 0 ? (hi - lo) / hi : 1;
      step = 0;
      for (size_t i = 1; i < v.size(); i++) step = std::max(step, (double)fabsf(v[i] - v[i - 1]));
    };
    const double sineStep = 8000 * 0.7 * 2 * M_PI * 440 / AUDIO_SAMPLE_RATE_EXACT;   // (the loop's own biggest step, at the mix's headroom)
    for (float tempo : {120.0f, 96.0f, 150.0f}) {
      simSetTempo(tempo);
      for (bool st : {false, true}) {
        tracks[2].stretch = st;
        runBeats(1);
        double hz, spread, step;
        measure(hz, spread, step);
        double want = st ? 440 : 440 * tempo / 120;
        char m12[220];
        snprintf(m12, sizeof m12, "%.0f BPM (recorded at 120), %s: %.1f Hz (%.0f), level within %.1f%%, biggest step %.0f (the sine's %.0f)",
                 tempo, st ? "stretched" : "resampled", hz, want, spread * 100, step, sineStep);
        check(fabs(hz - want) < 1.5 && spread < 0.06 && step < 1.3 * sineStep * std::max(1.0f, tempo / 120), m12);
      }
    }
    // Drums recorded at 120, played at 100: their kicks on our beats, stretched as resampled.
    simSetTempo(120);
    clearTrack(2);
    runBeats(1);
    drumBpm = 120;
    drumBeat = transport.beat + (double)transport.sampleInBeat / transport.spb;
    recordPress(4);
    for (int i = 0; i < 4000 && recording.mode != Recording::Running; i++) block();
    runBeats(3.5);
    recordPress(4);
    for (int i = 0; i < 200000 && recording.mode != Recording::Idle; i++) block();
    drumBpm = 0;
    feedNoise = false;
    simSetTempo(100);
    auto kicksOff = [&](double &mean, double &worst) {   // each of our beats: the kick's onset after it (ms)
      runBeats(1.2);
      std::vector<double> errs;
      for (int k = 0; k < 8; k++) {
        for (int i = 0; i < 4000 && transport.sampleInBeat >= AUDIO_BLOCK_SAMPLES; i++) block();   // (a beat's first block)
        uint32_t beatAt = transport.now - transport.sampleInBeat;
        std::vector<int16_t> v;
        for (int b = 0; b < 40; b++) { block(); for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) v.push_back(outL.data[i]); }
        int peak = 0;
        for (int16_t x : v) peak = std::max(peak, abs((int)x));
        uint32_t s0 = transport.now - (uint32_t)v.size();
        for (size_t i = 0; i < v.size(); i++)
          if (abs(v[i]) > peak / 3) { errs.push_back((double)(int32_t)(s0 + i - beatAt) * 1000 / AUDIO_SAMPLE_RATE_EXACT); break; }
      }
      mean = 0, worst = 0;
      for (double e : errs) mean += e;
      mean /= std::max<size_t>(1, errs.size());
      for (double e : errs) worst = std::max(worst, fabs(e - mean));
    };
    double rMean, rWorst, sMean, sWorst;
    tracks[4].stretch = false;
    kicksOff(rMean, rWorst);
    tracks[4].stretch = true;
    kicksOff(sMean, sWorst);
    char m13[220];
    snprintf(m13, sizeof m13, "drums recorded at 120, played at 100: kicks %+.1f ms from our beats stretched (%+.1f resampled), spread %.1f ms",
             sMean, rMean, sWorst);
    check(fabs(sMean - rMean) < 8 && sWorst < 8, m13);
    tracks[4].stretch = false;
    inputMonitored = wasMonitor;
    clearTrack(4);
    simSetTempo(120);
    runBeats(1);
  }

  printf("transient locking: each attack once, on time, as sharp as recorded\n");
  {
    for (int t = 0; t < NUM_TRACKS; t++) if (tracks[t].state != TrackState::Empty) clearTrack(t);
    if (!transport.running) transportStart();
    simSetTempo(120);
    runBeats(1);
    bool wasMonitor = inputMonitored;
    feedRamp = false; feedSineHz = 0;
    drumClicks = true;
    drumBpm = 120;
    drumBeat = transport.beat + (double)transport.sampleInBeat / transport.spb;
    recordPress(5);
    for (int i = 0; i < 4000 && recording.mode != Recording::Running; i++) block();
    runBeats(3.5);
    recordPress(5);
    for (int i = 0; i < 200000 && recording.mode != Recording::Idle; i++) block();
    drumBpm = 0;
    drumClicks = false;
    inputMonitored = false;
    // Six of our beats' clicks (each beat's start from beatLog): their onsets (ms from the beat) and how
    // closely each matches `ref`, the click as recorded (the best normalised correlation within 2 ms:
    // a click played twice, a flam or a smear match less).
    struct Hits { int beats = 0; double onsetMean = 0, matchMin = 1; std::vector<float> first; };
    auto listen = [&](const std::vector<float> *ref) {
      runBeats(1.5);
      std::vector<float> v;
      uint32_t from = transport.now;
      std::vector<uint32_t> starts;
      int32_t seen = transport.beat;
      while (starts.size() < 10) {
        block();
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) v.push_back(outL.data[i]);
        if (transport.beat != seen) { seen = transport.beat; starts.push_back((uint32_t)beatLog[(uint32_t)seen % BEAT_LOG].now); }
      }
      Hits h;
      const int pre = 44, len = 441;                 // (1 ms before the onset, 10 ms of it)
      std::vector<double> on;
      for (size_t k = 1; k + 1 < starts.size(); k++) {   // (each beat's window: from a quarter of a beat before it to three quarters after)
        int32_t b = (int32_t)(starts[k] - from);
        int32_t q = (int32_t)(starts[k + 1] - starts[k]) / 4;
        int32_t lo = b - q, hi = b + 3 * q;
        if (lo < 0 || hi > (int32_t)v.size()) continue;
        int32_t am = lo;
        for (int32_t i = lo; i < hi; i++) if (fabsf(v[i]) > fabsf(v[am])) am = i;
        float peak = fabsf(v[am]);
        if (peak < 1000) continue;
        int32_t at = am;                             // (its onset: back from the peak to where it's quiet)
        int quiet = 0;
        for (int32_t i = am; i > lo && quiet < 22; i--) { if (fabsf(v[i]) < 0.1f * peak) quiet++; else { quiet = 0; at = i; } }
        on.push_back((at - b) * 1000.0 / AUDIO_SAMPLE_RATE_EXACT);
        std::vector<float> w(v.begin() + std::max<int32_t>(0, at - pre), v.begin() + std::min<int32_t>((int32_t)v.size(), at - pre + len));
        if (h.first.empty()) h.first = w;
        if (ref && ref->size() == (size_t)len && w.size() == (size_t)len) {
          double best = 0;
          for (int lag = -88; lag <= 88; lag++) {
            double c = 0, ea = 0, eb = 0;
            for (int i = 0; i < len; i++) {
              int j = i + lag;
              float x = j >= 0 && j < len ? w[j] : 0, y = (*ref)[i];
              c += x * y; ea += x * x; eb += y * y;
            }
            best = std::max(best, c / sqrt(ea * eb + 1));
          }
          h.matchMin = std::min(h.matchMin, best);
        }
        h.beats++;
      }
      for (double x : on) h.onsetMean += x / std::max<size_t>(1, on.size());
      return h;
    };
    tracks[5].stretch = false;
    simSetTempo(120);
    Hits base = listen(nullptr);                     // as recorded
    char m15[260];
    snprintf(m15, sizeof m15, "the clicks as recorded: %d beats, %+.1f ms from our beats", base.beats, base.onsetMean);
    check(base.beats >= 5 && base.first.size() == 441, m15);
    for (float tempo : {120.0f, 96.0f, 80.0f, 150.0f}) {
      simSetTempo(tempo);
      tracks[5].stretch = false;                     // (resampled: where each click belongs at this tempo)
      Hits want = listen(&base.first);
      tracks[5].stretch = true;
      transientLock = false;
      Hits plain = listen(&base.first);
      transientLock = true;
      Hits locked = listen(&base.first);
      printf("  info  %.0f BPM: resampled %+.1f ms (matching %.3f); stretched, not locked: %+.1f ms, matching at least %.3f\n",
             tempo, want.onsetMean, want.matchMin, plain.onsetMean, plain.matchMin);
      snprintf(m15, sizeof m15, "%.0f BPM (recorded at 120), locked: the clicks %+.1f ms from our beats (resampled %+.1f), matching the recorded one at least %.3f (%d beats)",
               tempo, locked.onsetMean, want.onsetMean, locked.matchMin, locked.beats);
      check(locked.beats >= 4 && fabs(locked.onsetMean - want.onsetMean) < 1.0 && locked.matchMin > 0.98, m15);   // (4: which beats the window catches moves with the state earlier tests leave)
    }
    tracks[5].stretch = true;
    tracks[5].stretch = false;
    inputMonitored = wasMonitor;
    clearTrack(5);
    simSetTempo(120);
    runBeats(1);
  }

  printf("scenes: each keeps its own tempo (the one it was made at, then its latest recording's)\n");
  {
    if (!transport.running) transportStart();
    feedRamp = true;
    auto recordOn = [&](int t) {                     // a bar's recording on track t
      recordPress(t);
      for (int i = 0; i < 4000 && recording.mode != Recording::Running; i++) block();
      runBeats(3.5);
      recordPress(t);
      for (int i = 0; i < 200000 && recording.mode != Recording::Idle; i++) block();
      runBeats(1);
    };
    auto near = [](float a, float b) { return fabsf(a - b) < 0.11f; };
    tempoByHand(120);
    runBeats(1);
    sceneGoEnd();                                    // the copy after the last (N+1*): no saved tempo of its own yet
    runBeats(2);
    int empty = -1;
    for (int t = 0; t < NUM_TRACKS && empty < 0; t++) if (tracks[t].state == TrackState::Empty) empty = t;
    check(empty >= 0, "(an empty track to record on)");
    recordOn(empty);
    int made = sceneCurrent();
    char m14[200];
    snprintf(m14, sizeof m14, "a recording made the scene (%d): its tempo the recording's (%.1f)", made, sceneSavedTempo());
    check(made == sceneCount() && near(sceneSavedTempo(), 120), m14);
    tempoByHand(110);
    runBeats(1);
    snprintf(m14, sizeof m14, "the tempo set to 110 by hand: the scene's now (%.1f)", sceneSavedTempo());
    check(near(sceneSavedTempo(), 110), m14);
    check(sceneDuplicate() && near(sceneSavedTempo(), 110), "a duplicate: made at the tempo now (110)");
    tempoByHand(100);
    runBeats(1);
    int empty2 = -1;
    for (int t = 0; t < NUM_TRACKS && empty2 < 0; t++) if (tracks[t].state == TrackState::Empty) empty2 = t;
    recordOn(empty2);
    snprintf(m14, sizeof m14, "a recording on it at 100: the scene's tempo 100 (%.1f)", sceneSavedTempo());
    check(near(sceneSavedTempo(), 100), m14);
    sceneStep(-1);
    runBeats(BEATS_PER_BAR * 2);
    snprintf(m14, sizeof m14, "back to the scene before: its own tempo (%.1f, set to 110 there)", sceneSavedTempo());
    check(near(sceneSavedTempo(), 110), m14);
    sceneGoEnd();
    runBeats(BEATS_PER_BAR * 2);
    snprintf(m14, sizeof m14, "the copy after the last: the last's (%.1f)", sceneSavedTempo());
    check(near(sceneSavedTempo(), 100), m14);
    sceneGoEmpty();
    runBeats(2);
    check(sceneSavedTempo() == 0, "the empty scene: none");
    feedRamp = false;
    tempoByHand(120);
    runBeats(1);
  }

  printf("track settings saved in older volume steps: brought to today's\n");
  {
    auto up = [](uint32_t version, int vol, int bal) {
      TrackUnit u = {};
      memcpy(u.magic, "M8TU", 4);
      u.version = version;
      u.volume = (int8_t)vol;
      u.balance = (int8_t)bal;
      upgradeUnit(u);
      return std::make_pair((int)u.volume, (int)u.balance);
    };
    auto v4 = up(4, 16, -6), v4quiet = up(4, 1, 10), v3 = up(3, 4, 2), v3max = up(3, 5, -5);
    char m16[220];
    snprintf(m16, sizeof m16, "20 steps (unity 16): 16, -6 -> %d, %d; the quietest, 1 -> %d (still heard); 5 steps: 4, 2 -> %d, %d; 5, -5 -> %d, %d (balance +-20 today)",
             v4.first, v4.second, v4quiet.first, v3.first, v3.second, v3max.first, v3max.second);
    check(v4 == std::make_pair(VOLUME_UNITY, -12) && v4quiet.first == 1 && v4quiet.second == 20 &&
          v3 == std::make_pair(VOLUME_UNITY, 8) && v3max == std::make_pair(VOLUME_MAX, -BALANCE_MAX), m16);
  }

  printf("fine rotate: the clip turned, its audio as it was, played in step\n");
  {
    int t = -1;
    if (!transport.running) transportStart();
    for (int i = 0; i < NUM_TRACKS && t < 0; i++)   // (a clip of its own: recorded here)
      if (tracks[i].state == TrackState::Empty) t = i;
    if (t >= 0) {
      feedRamp = true;
      recordPress(t);
      for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
      runBeats(7);
      recordPress(t);
      runUntilIdle();
      runBeats(4);
      feedRamp = false;
    }
    check(t >= 0 && tracks[t].state == TrackState::Playing && tracks[t].live, "a track playing (recorded here)");
    if (t >= 0) {
      Track &tr = tracks[t];
      const uint32_t loop = tr.loopFrames(), quarter = tr.spbNative / 4, start = tr.rangeStartBeat * tr.spbNative;
      check(rotateTrack(t, (int32_t)quarter, 1), "turned a quarter beat later (a step: the step offset)");
      check(tr.rangePending && (int32_t)(tr.switchS - tr.head) >= 0 && tr.switchQ % 1 == 0,
            "queued: switched to where the stream hadn't buffered yet (the old place played up to it, nothing dropped)");
      runBeats(3);
      char msg[160];
      snprintf(msg, sizeof msg, "its unit and track: %u frames (a quarter beat: %u); the range starts that much earlier in the file",
               (unsigned)unitOf(t).rotate, (unsigned)quarter);
      check(unitOf(t).rotate == quarter && tr.rotate == quarter && tr.rangeStartFrame() == (start + loop - quarter) % loop, msg);
      check(unitOf(t).steps == 1 && tr.steps == 1, "the step offset kept apart: 1 quarter (its unit, its track)");
      checkSync(t, "a quarter beat later");
      rotateTrack(t, 44);                          // (a millisecond more)
      runBeats(3);
      checkSync(t, "a millisecond more");
      rotateTrack(t, -(int32_t)quarter - 44, -1);   // back
      check(unitOf(t).steps == 0, "back: no step offset");
      runBeats(3);
      check(tr.rotate == 0 && unitOf(t).rotate == 0 && tr.rangeStartFrame() == start, "turned back: none");
      checkSync(t, "back as it was");
    }
  }

  printf("\"play newly recorded\" off: a new clip comes in muted\n");
  {
    int t = -1;
    for (int i = 0; i < NUM_TRACKS && t < 0; i++)
      if (tracks[i].state == TrackState::Empty) t = i;
    if (!transport.running) transportStart();
    check(t >= 0, "an empty track to record on");
    if (t >= 0) {
      newClipsPlay = false;
      feedRamp = true;
      recordPress(t);
      for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
      runBeats(3);
      recordPress(t);
      runUntilIdle();
      runBeats(2);
      check(tracks[t].state == TrackState::Muted && unitOf(t).muted && tracks[t].loopBars == 1, "recorded, a bar long, muted");
      newClipsPlay = true;
      feedRamp = false;
    }
  }

  printf("DELETE UNUSED SAMPLES: silent recordings taken off the tracks and deleted\n");
  {
    if (!transport.running) transportStart();
    clearTrack(4);
    feedRamp = false; feedSineHz = 0;
    runBeats(1.5);
    recordPress(4);
    for (int i = 0; i < 2000 && recording.mode == Recording::WaitBar; i++) block();
    runBeats(3.6);
    recordPress(4);
    runUntilIdle();
    silentId = tracks[4].sample;
    transportStop();
    runUntilIdle();
    check(silentId && tracks[4].state != TrackState::Empty, "(a bar of silence on track 5, in the scene)");
    flushScenes();
    int deleted = deleteUnusedSamples();
    char m[120];
    snprintf(m, sizeof m, "the silent one deleted, its track emptied (%d deleted in all)", deleted);
    check(deleted >= 1 && !sampleExists(silentId) && tracks[4].state == TrackState::Empty, m);
  }

  printf("a card whose free space is in pieces: recording space grown a chunk at a time while stopped\n");
  {
    if (transport.running) transportStop();
    runUntilIdle();
    std::system(("rm -f " + simRoot + "/L7/DATA/0024.DAT " + simRoot + "/L7/DATA/0023.DAT").c_str());
    simNoRun = true;
    storageForget();
    scenesForget();
    storageBegin([](const char *) {});
    scenesBegin(simTempo, simSetTempo, projectCurrent() < 0 ? 0 : projectCurrent(), simNewTempo);
    for (int i = 0; i < 40000; i++) serviceStorage();   // (a chunk a pass: ~1350 a file)
    simNoRun = false;
    FILE *ls = popen(("ls " + simRoot + "/L7/DATA | grep -c '^[0-9]'").c_str(), "r");
    int files = -1;
    if (ls) { if (fscanf(ls, "%d", &files) != 1) files = -1; pclose(ls); }
    struct stat st;
    bool full = ::stat((simRoot + "/L7/DATA/0024.DAT").c_str(), &st) == 0 && (uint64_t)st.st_size >= 128ull * 44118 * 4;
    char m[160];
    snprintf(m, sizeof m, "no run of 21 MB free: grown in pieces, the 24 there again (%d files, 0024.DAT full size %d)", files, (int)full);
    check(files == 24 && full, m);
  }

  printf("random sessions\n");
  for (unsigned seed = 1; seed <= (argc > 1 ? atoi(argv[1]) : 5); seed++) fuzz(600, seed);
  for (auto &tr : tracks) tr.stretch = true;       // and one with every track stretched
  if (argc <= 1 || atoi(argv[1]) > 0) fuzz(600, 101);

  printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
