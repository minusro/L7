// midi_in.cpp — see midi_in.h
#include "midi_in.h"
#include "engine.h"
#include "storage.h"
#include "audio_follow.h"
using namespace looper;

static constexpr uint32_t FOLLOW_TIMEOUT_MS = 500;
static constexpr float MAX_NUDGE = 0.005f;            // the speed off their tempo, at most (to catch their beat up: ~9 cents)
static constexpr float NUDGE_GAIN = 0.5f;             // of the beat's difference, taken out over the next beat
// For CHANGE_TICKS after their tempo changed (or a start), while the loops
// change pitch anyway: up to CHANGE_NUDGE, CHANGE_GAIN: the beat caught up
// within about a beat, not over several (each of them off pitch).
static constexpr float CHANGE_NUDGE = 0.05f, CHANGE_GAIN = 2.0f;
static constexpr uint32_t CHANGE_TICKS = 96;
static constexpr float DEADBAND_BEATS = 0.003f;       // a difference this small: left (their jitter)
static constexpr float MIN_CHANGE = 0.0015f;          // a tempo change this small: not made (each one is a recording's tempo mark; ~2.6 cents)
// Their tempo: two alpha-beta trackers on their ticks' times (each tick: the
// next one predicted; its miss moves the prediction by alpha of it and the
// tick period by beta of it), a slow one (the tempo used: steady through
// jitter) and a fast one. The fast one HANDOVER off the slow one for
// HANDOVER_TICKS ticks running: their tempo changed, the slow one takes the
// fast one's state, and the fast gains, easing back to its own over
// BOOST_TICKS. Modelled with ±0.8 ms of jitter: steady within 0.054 %; a
// change (1 to 10 BPM either way) followed to within 0.2 % in ~11 ticks.
static constexpr double SLOW_ALPHA = 0.06, SLOW_BETA = 0.0019, FAST_ALPHA = 0.3, FAST_BETA = 0.053;
static constexpr double HANDOVER = 0.006;
static constexpr uint32_t HANDOVER_TICKS = 4, BOOST_TICKS = 24;
static constexpr uint32_t APPLY_TICKS = 6;            // their tempo and beat applied every quarter beat
// Our beat kept this far ahead of theirs (us; serial, USB): their clock's way in
// and our audio's way out, so our audio reaches them on their beat (as their
// recording of it places it). USB: measured with tools/latency/clocksync.c,
// L7's audio recorded over USB audio (aheadBeats(): at their tempo).
static constexpr int32_t AHEAD_US[2] = {0, 2000};    // (USB: 1.7-2.1 ms late without it; with it within ±1 ms, median ~0; 2026-10-07)
static constexpr bool LOG_TICKS = false;              // (diagnosing: each tick's time and each tempo applied, on the serial log; data/daw_clock_usb.txt came from it)

static HardwareSerial *port = nullptr;
int16_t syncOffsetMs[SYNC_SOURCES] = {0, 0, 0};
// Our tempo their times 2^octave (B+UP/DOWN on the tempo cell; each MIDI
// source its own): our position their position times the ratio, plus the offset.
static int octaveOf[2] = {0, 0};

// Interrupt -> loop(): the ticks' times, and their transport's messages in
// order (Start, Continue, Stop, Song Position), each with the tick it comes
// before.
static constexpr uint32_t TICKS = 128;
static volatile uint32_t tickUs[TICKS];
static volatile uint32_t tickCount = 0;               // ticks received
struct Event { uint8_t type; uint16_t pos; uint32_t tick; };   // pos: a Song Position's, in 16ths
static constexpr uint32_t EVENTS = 16;
static volatile Event events[EVENTS];
static volatile uint32_t eventHead = 0;
static uint8_t sppHave = 0, sppLsb = 0;              // a Song Position on its way in (serial): its data bytes so far (interrupt only)

static void pushEvent(uint8_t type, uint16_t pos = 0) {
  volatile Event &e = events[eventHead % EVENTS];
  e.type = type;
  e.pos = pos;
  e.tick = tickCount;
  eventHead++;
}

static volatile bool suspended = false;
static volatile uint8_t source = MIDI_SERIAL;         // the clock followed (its ticks the latest)
static volatile uint32_t sourceUs = 0;                // its latest tick
static volatile bool sourceKnown = false;

void midiInSuspend(bool off) { suspended = off; }

void midiInByte(uint8_t b, uint32_t us, MidiSource from) {
  if (suspended) return;
  if (b < 0xF8) {                                     // (not realtime: a Song Position, 0xF2 and two data bytes, from the serial in)
    if (b == 0xF2) sppHave = 1;
    else if (b >= 0x80) sppHave = 0;                  // (another message)
    else if (sppHave == 1) { sppLsb = b; sppHave = 2; }
    else if (sppHave == 2) { sppHave = 0; midiInSongPosition(sppLsb | b << 7, us, from); }
    return;
  }
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
    case 0xFB:
    case 0xFC:
      pushEvent(b);
      break;
    default:                                          // (everything else: not for us)
      break;
  }
}

void midiInSongPosition(uint16_t pos16, uint32_t us, MidiSource from) {
  if (suspended) return;
  bool active = sourceKnown && (int32_t)(us - sourceUs) < (int32_t)(FOLLOW_TIMEOUT_MS * 1000);
  if (active && from != source) return;               // (another clock is followed)
  pushEvent(0xF2, pos16);
}

static void poll() {
  while (port && port->available() > 0) midiInByte((uint8_t)port->read(), micros());
}

void midiInBegin(HardwareSerial *p) {
  port = p;
  midiInPollHook = poll;
}

static bool theirsRunning = false;                    // between their Start (or Continue) and Stop (loop()'s)

static uint32_t startedUs = 0;                        // their latest Start / Continue / jump (loop()'s)

// Their clock's alive: a tick within FOLLOW_TIMEOUT_MS, or their Start (or
// Continue, a jump) as recent: a source that sends no clock while stopped (a
// DAW over USB) may send its first tick a moment after it, and the Start may
// be taken in before that tick.
static bool alive() {
  noInterrupts();
  uint32_t n = tickCount, latest = n ? tickUs[(n - 1) % TICKS] : startedUs;
  interrupts();
  if (theirsRunning && (int32_t)(startedUs - latest) > 0) latest = startedUs;
  return (n || theirsRunning) && (int32_t)(micros() - latest) < (int32_t)(FOLLOW_TIMEOUT_MS * 1000);   // (signed: a stamp a moment ahead counts too)
}

bool midiClockFollowing() { return theirsRunning && alive(); }
int midiFollowSource() { return source; }

// The loop()'s side.
static uint32_t seenTicks = 0, seenEvents = 0;
static int32_t refTick = -1;                          // a tick of theirs whose position we know (-1: started here, beats only)
static double refBeats = 0;                           // its position (beats from their 1.1.1)
static uint16_t nextPos = 0;                          // where a Continue goes on from (their latest Song Position, 16ths)
static int32_t startAt = INT32_MIN;                   // our transport starts on this beat of theirs, once they're there (INT32_MIN: no)
static float usPerSample = 1e6f / AUDIO_SAMPLE_RATE_EXACT;   // micros() per audio sample, as measured
static uint32_t lastStampUs = 0;
static double errSum = 0;                             // this beat's ticks' differences (beats, ours less theirs)
static int errCount = 0;
static uint32_t theirSpb = 0;                         // their tempo (samples a beat), over their last beats
static uint32_t trackFrom = 0;                        // the trackers' first tick (after a pause: the one after the first, often late after a Start)
static uint32_t trackBase = 0;                        // the latest tick's time (micros())
struct Tracker { double next, period; int wayOff; };  // the next tick's predicted time (us after trackBase), the tick period (us), ticks way off running
static Tracker slowT = {0, 0, 0}, fastT = {0, 0, 0};
static uint32_t apart = 0, boost = 0;                 // ticks running the fast one's been HANDOVER off; fast gains left (BOOST_TICKS down)
static uint32_t changing = 0;                         // ticks left since their tempo changed or they started (CHANGE_TICKS down)
// Their tempo remembered: their ticks timed over the stretch we've followed
// them steadily (from the end of the catch-up after their start or tempo
// change: its first tick to the latest, AVG_MIN ticks at least; their jitter
// only at its two ends; their tempo changing: afresh). On
// their Stop (or their clock gone) our tempo becomes it (no nudge in it), and
// their next start begins from it (the trackers too), unless their clock
// meanwhile is clearly at another (HANDOVER off: theirs then).
static constexpr uint32_t AVG_MIN = 48;
static double avgSpb = 0;
static uint32_t steadyTick = UINT32_MAX, steadyUs = 0;   // the stretch's first tick (UINT32_MAX: none)

// Our beat position (beats from the transport's start) at micros() `us`.
static double ourBeatAt(uint32_t us, const BlockStamp &s) {
  double samples = (int32_t)(us - s.us) / usPerSample;
  return s.beat + (s.sampleInBeat + samples) / s.spb;
}

uint32_t midiTheirSpb() { return theirSpb; }

static double ratio() { return ldexp(1.0, octaveOf[source & 1]); }
static uint32_t oursOf(double spb) { return (uint32_t)(spb / ratio() + 0.5); }   // (our spb for theirs)
static float shownOf(double spb) { return roundf(bpmForSpb((uint32_t)(spb + 0.5)) * (float)ratio() * 10) / 10; }
// Their beat to start ours on: whole, and halved (or less) one of every 2
// (4, 8) of theirs, so ours is whole too.
static int32_t startBeatFrom(double theirs) {
  int32_t sb = (int32_t)ceil(theirs - 1e-9), k = ratio() < 1 ? (int32_t)lround(1 / ratio()) : 1;
  return sb + (k - ((sb % k) + k) % k) % k;          // (up to a multiple of k)
}
static int32_t ourBeatOf(int32_t theirs) { return (int32_t)lround(theirs * ratio()); }

static double aheadBeats() {                          // (at their tempo; the offset set by hand: later)
  return theirSpb ? (AHEAD_US[source & 1] - 1000.0 * syncOffsetMs[source & 1]) / (theirSpb * usPerSample) : 0;
}

// Their position (beats) at tick i.
static double theirBeatAt(uint32_t i) { return refBeats + (double)((int32_t)i - refTick) / 24; }

bool midiScale(int by) {
  if (!midiClockFollowing() || !theirSpb || !by) return false;
  int &o = octaveOf[source & 1], to = o + (by > 0 ? 1 : -1);
  float b = bpmForSpb(theirSpb) * ldexpf(1, to);
  if (to < -3 || to > 3 || b < MIN_BPM || b > MAX_BPM) return false;   // (the clock's range)
  o = to;
  Serial.printf("MIDI in: x%g\n", ratio());
  followTempo(oursOf(theirSpb), shownOf(theirSpb));
  if (refTick >= 0 && seenTicks) {                    // running: ours again from their next beat that fits (as a start)
    if (transport.running) transportStop();
    startAt = startBeatFrom(theirBeatAt(seenTicks - 1));
    changing = CHANGE_TICKS;
  }
  return true;
}

uint32_t syncOffsetChangedMs = 0;
void syncOffsetChanged(int src) {
  syncOffsetChangedMs = max(1u, millis());
  if (src == SYNC_SERIAL || src == SYNC_USB) changing = CHANGE_TICKS;   // (caught up as after a start: within a few beats)
  else audioFollowOffsetChanged();
}


// Starting: how far they are past the beat we start on, now (samples at our
// tempo): from the tick that got there, at their tempo since.
static uint32_t startTickAt = 0;
static int32_t startingBeat = 0;
static uint32_t pastStartingBeat() {
  uint32_t spb = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  double theirs = theirSpb ? theirSpb : spb * ratio(), usPerBeat = theirs * usPerSample;   // (their beats: samples since, at their tempo)
  double beats = theirBeatAt(startTickAt) + aheadBeats() - startingBeat + (int32_t)(micros() - tickUs[startTickAt % TICKS]) / usPerBeat;
  return beats <= 0 ? 0 : (uint32_t)(beats * theirs + 0.5);
}

// Their tempo remembered becomes ours (stopped following: no nudge in it).
static void rememberTempo() {
  if (!avgSpb) return;
  theirSpb = (uint32_t)(avgSpb + 0.5);
  slowT.period = fastT.period = theirSpb * usPerSample / 24;
  followTempo(oursOf(theirSpb), shownOf(theirSpb));
}

// Theirs (from tick `tick`, at position pos16): our transport stopped, and
// started again on their next whole beat (as Start, Continue or a jump).
static void startWith(uint32_t tick, uint16_t pos16) {
  theirsRunning = true;
  steadyTick = UINT32_MAX;
  changing = CHANGE_TICKS;
  startedUs = micros();
  refTick = (int32_t)tick;
  refBeats = pos16 / 4.0;
  startAt = startBeatFrom(refBeats);
  errSum = 0;
  errCount = 0;
  if (transport.running) transportStop();
  // Their tempo from the start: the one remembered, unless their clock's
  // clearly at another now (timed while they were stopped).
  if (avgSpb && (!theirSpb || fabs((double)theirSpb - avgSpb) < HANDOVER * avgSpb)) {
    theirSpb = (uint32_t)(avgSpb + 0.5);
    slowT.period = fastT.period = theirSpb * usPerSample / 24;
  }
  if (theirSpb) followTempo(oursOf(theirSpb), shownOf(theirSpb));
}

void midiInService() {
  noInterrupts();
  BlockStamp s;
  s.us = blockStamp.us;
  s.beat = blockStamp.beat;
  s.sampleInBeat = blockStamp.sampleInBeat;
  s.spb = blockStamp.spb;
  s.running = blockStamp.running;
  uint32_t n = tickCount, ne = eventHead;
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

  // Their transport's messages, in order.
  if (ne - seenEvents > EVENTS) seenEvents = ne - EVENTS;
  for (; seenEvents < ne; seenEvents++) {
    Event e;
    noInterrupts();
    e.type = events[seenEvents % EVENTS].type;
    e.pos = events[seenEvents % EVENTS].pos;
    e.tick = events[seenEvents % EVENTS].tick;
    interrupts();
    switch (e.type) {
      case 0xFA:                                      // Start: from 1.1.1
        nextPos = 0;
        startWith(e.tick, 0);
        Serial.println("MIDI in: start");
        break;
      case 0xFB:                                      // Continue: from their Song Position
        startWith(e.tick, nextPos);
        Serial.printf("MIDI in: continue from beat %.2f\n", nextPos / 4.0);
        break;
      case 0xFC:                                      // Stop
        theirsRunning = false;                        // (the tempo free again)
        startAt = INT32_MIN;
        if (transport.running) transportStop();
        rememberTempo();
        Serial.println("MIDI in: stop");
        break;
      case 0xF2:                                      // Song Position: where a Continue goes on from; while they run, a jump there
        nextPos = e.pos;
        if (theirsRunning) {
          startWith(e.tick, e.pos);
          Serial.printf("MIDI in: jump to beat %.2f\n", e.pos / 4.0);
        }
        break;
    }
  }
  if (theirsRunning && !alive()) {                    // (their clock gone)
    theirsRunning = false;
    startAt = INT32_MIN;
    rememberTempo();
  }
  if (n - seenTicks > TICKS) seenTicks = n - TICKS;   // (fallen far behind: the last ones)
  bool running = transport.running && s.running;
  if (!transport.running && startAt == INT32_MIN && refTick >= 0 && !theirsRunning) refTick = -1;   // (stopped: a later start here is ours)
  for (; seenTicks < n; seenTicks++) {
    uint32_t i = seenTicks, us = tickUs[i % TICKS];
    if (changing) changing--;
    if (theirsRunning && !changing && running) {      // their tempo remembered: the steady stretch timed
      if (steadyTick == UINT32_MAX) {
        steadyTick = i;
        steadyUs = us;
      } else if (i - steadyTick >= AVG_MIN) {
        avgSpb = (double)(uint32_t)(us - steadyUs) / (i - steadyTick) * 24 / usPerSample;
      }
    } else if (!theirsRunning) {
      steadyTick = UINT32_MAX;                        // (stretches don't run across their stops)
    }
    if (LOG_TICKS) Serial.printf("T %lu %lu\n", (unsigned long)i, (unsigned long)us);
    // After a pause (or another clock) tracked afresh, from the tick after the
    // first (often late after a Start), from the tempo known if any.
    if (i == 0 || (uint32_t)(us - tickUs[(i - 1) % TICKS]) > FOLLOW_TIMEOUT_MS * 1000) {
      trackFrom = i + 1;
      slowT.period = fastT.period = theirSpb ? theirSpb * usPerSample / 24 : 0;
      apart = 0;
      boost = theirSpb ? 0 : BOOST_TICKS;             // (no tempo known: learnt quickly)
    } else if (i == trackFrom) {
      trackBase = us;
      slowT.next = slowT.period;
      fastT.next = fastT.period;
    } else if (i > trackFrom) {
      double meas = (int32_t)(us - trackBase);
      if (!slowT.period) slowT = fastT = {meas, meas, 0};   // (no tempo known: its first interval)
      auto step = [&](Tracker &t, double alpha, double beta) {
        double miss = meas - t.next;
        if (fabs(miss) > t.period / 2) {             // way off: one (as their first after a Start often is) taken as it is, the
          if (++t.wayOff >= 3) {                     // tempo left; three running: their tempo's another, measured afresh from this interval
            t.period = meas;
            t.wayOff = 0;
          }
          miss = meas - t.next;                      // (the prediction from this tick on: at it)
          t.next = meas;
          miss = 0;
        } else {
          t.wayOff = 0;
        }
        double at = t.next + alpha * miss;
        t.period += beta * miss;
        t.next = at - meas + t.period;
      };
      double f = boost ? (double)--boost / BOOST_TICKS : 0;
      step(slowT, SLOW_ALPHA + (FAST_ALPHA - SLOW_ALPHA) * f, SLOW_BETA + (FAST_BETA - SLOW_BETA) * f);
      step(fastT, FAST_ALPHA, FAST_BETA);
      trackBase = us;
      apart = fabs(fastT.period - slowT.period) > HANDOVER * slowT.period ? apart + 1 : 0;
      if (apart >= HANDOVER_TICKS) {                  // their tempo changed: the fast one's
        slowT = fastT;
        apart = 0;
        boost = BOOST_TICKS;
        changing = CHANGE_TICKS;
        avgSpb = 0;                                   // (another tempo: remembered afresh)
        steadyTick = UINT32_MAX;
      }
      uint32_t spb = (uint32_t)(slowT.period * 24 / usPerSample + 0.5);
      if (spb >= spbForBpm(MAX_BPM) && spb <= spbForBpm(MIN_BPM) && i - trackFrom >= 3) theirSpb = spb;
    }
    // Their next whole beat reached: our transport from that beat (the clock
    // reads it; the tracks where they'd be).
    if (startAt != INT32_MIN && refTick >= 0 && (int32_t)i >= refTick && theirBeatAt(i) >= startAt - 1e-9) {
      startTickAt = i;                                // (started exactly where they are: no catching up)
      startingBeat = startAt;
      transportStartAt(ourBeatOf(startAt), pastStartingBeat);
      startAt = INT32_MIN;
      seenTicks++;
      return;                                         // (running from the next block)
    }
    if (!running || !theirSpb) continue;
    // How far we're off their position at this tick: theirs known (bars too),
    // else beats only.
    double ours = ourBeatAt(us, s),                   // (where we should be: ahead of them; their beats times the ratio)
           theirs = ((refTick >= 0 ? theirBeatAt(i) : 0) + aheadBeats()) * ratio();
    double period = refTick >= 0 ? BEATS_PER_BAR : 1;
    double err = fmod(ours - theirs, period);
    if (err >= period / 2) err -= period;
    if (err < -period / 2) err += period;
    errSum += err;
    errCount++;
  }
  if (!theirSpb || !theirsRunning) return;          // (their transport stopped: timed, not followed; the tempo's free)
  uint32_t want = oursOf(theirSpb);
  if (running && errCount >= (int)APPLY_TICKS) {      // every quarter beat: their tempo, nudged to their beat
    float err = errSum / errCount;
    errSum = 0;
    errCount = 0;
    float most = changing ? CHANGE_NUDGE : MAX_NUDGE, gain = changing ? CHANGE_GAIN : NUDGE_GAIN;
    float nudge = fabsf(err) < DEADBAND_BEATS ? 0 : constrain(err * gain, -most, most);   // (ahead: slower)
    want = (uint32_t)(theirSpb * (1 + nudge) / ratio() + 0.5);
  } else if (running) {
    return;                                           // (between: as it is)
  }
  uint32_t now = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  float raw = bpmForSpb(theirSpb) * (float)ratio(), shownBpm = fabsf(raw - bpm) < 0.08f ? bpm : roundf(raw * 10) / 10;   // (shown: a tenth's change only when clear, no flicker)
  if (LOG_TICKS) Serial.printf("E %lu %lu %lu\n", (unsigned long)theirSpb, (unsigned long)want, (unsigned long)now);
  if (fabsf((float)want - now) > MIN_CHANGE * now) followTempo(want, shownBpm);
  else if (shownBpm != bpm) followTempo(now, shownBpm);   // (the tempo shown only)
}
