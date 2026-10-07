// midi_in.cpp — see midi_in.h
#include "midi_in.h"
#include "engine.h"
#include "storage.h"
using namespace looper;

static constexpr uint32_t FOLLOW_TIMEOUT_MS = 500;
static constexpr float MAX_NUDGE = 0.02f;             // the speed off their tempo, at most (to catch their beat up)
static constexpr float NUDGE_GAIN = 0.5f;             // of the beat's difference, taken out over the next beat
static constexpr float DEADBAND_BEATS = 0.003f;       // a difference this small: left (their jitter)
static constexpr float MIN_CHANGE = 0.001f;           // a tempo change this small: not made (each one is a recording's tempo mark)
static constexpr uint32_t SPAN = 96;                  // their tempo: over their last 4 beats (24 ticks at least)

static HardwareSerial *port = nullptr;

// Interrupt -> loop(): the ticks' times, and the starts and stops (counted).
static constexpr uint32_t TICKS = 128;
static volatile uint32_t tickUs[TICKS];
static volatile uint32_t tickCount = 0;               // ticks received
static volatile uint32_t starts = 0, stops = 0;
static volatile uint32_t startTick = 0;               // the tick that's 1.1.1 of the latest start

static volatile bool suspended = false;
static volatile uint8_t source = MIDI_SERIAL;         // the clock followed (its ticks the latest)
static volatile uint32_t sourceUs = 0;                // its latest tick
static volatile bool sourceKnown = false;

void midiInSuspend(bool off) { suspended = off; }

void midiInByte(uint8_t b, uint32_t us, MidiSource from) {
  if (suspended || b < 0xF8) return;                  // (realtime messages only)
  bool active = sourceKnown && (int32_t)(us - sourceUs) < (int32_t)(FOLLOW_TIMEOUT_MS * 1000);
  if (active && from != source) return;               // (another clock is followed)
  if (b == 0xF8) {
    source = from;
    sourceUs = us;
    sourceKnown = true;
  }
  switch (b) {
    case 0xF8:
      tickUs[tickCount % TICKS] = us;
      tickCount++;
      break;
    case 0xFA:
    case 0xFB:                                        // (Continue: as Start, from 1.1.1)
      startTick = tickCount;                          // the next tick
      starts++;
      break;
    case 0xFC:
      stops++;
      break;
    default:                                          // (everything else: not for us)
      break;
  }
}

static void poll() {
  while (port && port->available() > 0) midiInByte((uint8_t)port->read(), micros());
}

void midiInBegin(HardwareSerial *p) {
  port = p;
  midiInPollHook = poll;
}

static bool theirsRunning = false;                    // between their Start (or Continue) and Stop (loop()'s)

static bool ticking() {
  noInterrupts();
  uint32_t n = tickCount, last = n ? tickUs[(n - 1) % TICKS] : 0;
  interrupts();
  return n >= 2 && (int32_t)(micros() - last) < (int32_t)(FOLLOW_TIMEOUT_MS * 1000);   // (signed: a stamp a moment ahead counts too)
}

bool midiClockFollowing() { return theirsRunning && ticking(); }

// The loop()'s side.
static uint32_t seenTicks = 0, seenStarts = 0, seenStops = 0;
static int32_t refTick = -1;                          // the tick at the transport's 1.1.1 (-1: started here, beats only)
static float usPerSample = 1e6f / AUDIO_SAMPLE_RATE_EXACT;   // micros() per audio sample, as measured
static uint32_t lastStampUs = 0;
static double errSum = 0;                             // this beat's ticks' differences (beats, ours less theirs)
static int errCount = 0;
static uint32_t theirSpb = 0;                         // their tempo (samples a beat), over their last beats
static uint32_t firstTick = 0;                        // the first tick of their clock as it is now (after a pause: a new one)

// Our beat position (beats from the transport's start) at micros() `us`.
static double ourBeatAt(uint32_t us, const BlockStamp &s) {
  double samples = (int32_t)(us - s.us) / usPerSample;
  return s.beat + (s.sampleInBeat + samples) / s.spb;
}

void midiInService() {
  noInterrupts();
  BlockStamp s;
  s.us = blockStamp.us;
  s.beat = blockStamp.beat;
  s.sampleInBeat = blockStamp.sampleInBeat;
  s.spb = blockStamp.spb;
  s.running = blockStamp.running;
  uint32_t n = tickCount, st = starts, sp = stops, sTick = startTick;
  interrupts();

  // micros() per sample from the blocks (the audio clock and the CPU's may differ a little).
  if (lastStampUs && s.us != lastStampUs) {
    uint32_t d = s.us - lastStampUs;
    if (d > 0 && d < 200000) {
      float blocks = roundf(d / (AUDIO_BLOCK_SAMPLES * usPerSample));
      if (blocks >= 1) usPerSample += 0.01f * (d / (blocks * AUDIO_BLOCK_SAMPLES) - usPerSample);
    }
  }
  lastStampUs = s.us;

  if (sp != seenStops) {
    seenStops = sp;
    theirsRunning = false;                            // (the tempo free again)
    if (transport.running) transportStop();
    Serial.println("MIDI in: stop");
  }
  if (theirsRunning && !ticking()) theirsRunning = false;   // (their clock gone)
  if (st != seenStarts) {
    seenStarts = st;
    theirsRunning = true;
    refTick = (int32_t)sTick;
    errSum = 0;
    errCount = 0;
    if (theirSpb) followTempo(theirSpb, roundf(bpmForSpb(theirSpb) * 10) / 10);   // (their tempo, timed while they were stopped: from the start)
    if (transport.running) transportRestart();
    else transportStart();
    Serial.println("MIDI in: start");
    return;                                           // (the clock's new start: from the next block)
  }
  if (n - seenTicks > TICKS) seenTicks = n - TICKS;   // (fallen far behind: the last ones)
  bool running = transport.running && s.running;
  if (!running && refTick >= 0 && !transport.running) refTick = -1;   // (stopped here: a later start is ours)
  for (; seenTicks < n; seenTicks++) {
    uint32_t i = seenTicks, us = tickUs[i % TICKS];
    if (i == 0 || (uint32_t)(us - tickUs[(i - 1) % TICKS]) > FOLLOW_TIMEOUT_MS * 1000) firstTick = i;   // (after a pause, or another clock: counted afresh)
    uint32_t span = min(i - firstTick, SPAN) / 24 * 24;   // their tempo: over their last beats (whole ones)
    if (span >= 24 && n - (i - span) <= TICKS) {
      uint32_t beatUs = (us - tickUs[(i - span) % TICKS]) / (span / 24);
      uint32_t spb = (uint32_t)(beatUs / usPerSample + 0.5f);
      if (spb >= spbForBpm(MAX_BPM) && spb <= spbForBpm(MIN_BPM)) theirSpb = spb;
    }
    if (!running || !theirSpb) continue;
    // How far we're off their beat at this tick: theirs counted from their
    // start (bars too), else beats only.
    double ours = ourBeatAt(us, s), theirs = refTick >= 0 ? (double)((int32_t)i - refTick) / 24 : 0;
    double period = refTick >= 0 ? BEATS_PER_BAR : 1;
    double err = fmod(ours - theirs, period);
    if (err >= period / 2) err -= period;
    if (err < -period / 2) err += period;
    errSum += err;
    errCount++;
  }
  if (!theirSpb || !theirsRunning) return;          // (their transport stopped: timed, not followed; the tempo's free)
  uint32_t shown = theirSpb;
  uint32_t want = theirSpb;
  if (running && errCount >= 24) {                    // once a beat: their tempo, nudged to their beat
    float err = errSum / errCount;
    errSum = 0;
    errCount = 0;
    float nudge = fabsf(err) < DEADBAND_BEATS ? 0 : constrain(err * NUDGE_GAIN, -MAX_NUDGE, MAX_NUDGE);   // (ahead: slower)
    want = (uint32_t)(theirSpb * (1 + nudge) + 0.5f);
  } else if (running) {
    return;                                           // (between: as it is)
  }
  uint32_t now = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  float shownBpm = roundf(bpmForSpb(shown) * 10) / 10;
  if (fabsf((float)want - now) > MIN_CHANGE * now) followTempo(want, shownBpm);
  else if (shownBpm != bpm) followTempo(now, shownBpm);   // (the tempo shown only)
}
