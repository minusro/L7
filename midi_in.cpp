// midi_in.cpp — see midi_in.h
#include "midi_in.h"
#include "engine.h"
#include "storage.h"
using namespace looper;

static constexpr uint32_t FOLLOW_TIMEOUT_MS = 500;
static constexpr float MAX_NUDGE = 0.005f;            // the speed off their tempo, at most (to catch their beat up: ~9 cents)
static constexpr float BIG_NUDGE = 0.02f;             // and when we're far off their beat (FAR_BEATS: after their tempo jumped)
static constexpr float FAR_BEATS = 0.03f;
static constexpr float NUDGE_GAIN = 0.5f;             // of the beat's difference, taken out over the next beat
static constexpr float DEADBAND_BEATS = 0.003f;       // a difference this small: left (their jitter)
static constexpr float MIN_CHANGE = 0.001f;           // a tempo change this small: not made (each one is a recording's tempo mark)
static constexpr uint32_t SPAN = 96;                  // their tempo: over their last 4 beats (24 ticks at least)
static constexpr float NEW_TEMPO = 0.02f;             // measured over fewer than SPAN ticks, a tempo this far off the one known: theirs changed

static HardwareSerial *port = nullptr;

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
static uint32_t firstTick = 0;                        // the first tick of their clock as it is now (after a pause: a new one)

// Our beat position (beats from the transport's start) at micros() `us`.
static double ourBeatAt(uint32_t us, const BlockStamp &s) {
  double samples = (int32_t)(us - s.us) / usPerSample;
  return s.beat + (s.sampleInBeat + samples) / s.spb;
}

// Their position (beats) at tick i.
static double theirBeatAt(uint32_t i) { return refBeats + (double)((int32_t)i - refTick) / 24; }

// Starting: how far they are past the beat we start on, now (samples at our
// tempo): from the tick that got there, at their tempo since.
static uint32_t startTickAt = 0;
static int32_t startingBeat = 0;
static uint32_t pastStartingBeat() {
  uint32_t spb = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  double usPerBeat = (theirSpb ? theirSpb : spb) * usPerSample;
  double beats = theirBeatAt(startTickAt) - startingBeat + (int32_t)(micros() - tickUs[startTickAt % TICKS]) / usPerBeat;
  return beats <= 0 ? 0 : (uint32_t)(beats * spb + 0.5);
}

// Theirs (from tick `tick`, at position pos16): our transport stopped, and
// started again on their next whole beat (as Start, Continue or a jump).
static void startWith(uint32_t tick, uint16_t pos16) {
  theirsRunning = true;
  startedUs = micros();
  refTick = (int32_t)tick;
  refBeats = pos16 / 4.0;
  startAt = (int32_t)ceil(refBeats - 1e-9);
  errSum = 0;
  errCount = 0;
  if (transport.running) transportStop();
  if (theirSpb) followTempo(theirSpb, roundf(bpmForSpb(theirSpb) * 10) / 10);   // (their tempo, timed while they were stopped: from the start)
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
  }
  if (n - seenTicks > TICKS) seenTicks = n - TICKS;   // (fallen far behind: the last ones)
  bool running = transport.running && s.running;
  if (!transport.running && startAt == INT32_MIN && refTick >= 0 && !theirsRunning) refTick = -1;   // (stopped: a later start here is ours)
  for (; seenTicks < n; seenTicks++) {
    uint32_t i = seenTicks, us = tickUs[i % TICKS];
    // After a pause (or another clock) counted afresh, from the tick after the
    // first (often a little late after a Start).
    if (i == 0 || (uint32_t)(us - tickUs[(i - 1) % TICKS]) > FOLLOW_TIMEOUT_MS * 1000) firstTick = i + 1;
    // Their tempo: a straight line through their last ticks' times (least
    // squares: each tick's jitter weighs little), up to SPAN of them, 24 at
    // least. Over fewer than SPAN, the tempo known is kept unless this one's
    // NEW_TEMPO off it (theirs changed; not their start's jitter).
    // Their last beat alone NEW_TEMPO off the tempo known: theirs changed, the
    // line from that beat on.
    auto fit = [&](uint32_t span) {                   // samples a beat, the line through ticks i - span .. i
      double sx = 0, sy = 0, sxx = 0, sxy = 0, m = span + 1;
      for (uint32_t k = 0; k <= span; k++) {
        double y = (int32_t)(tickUs[(i - span + k) % TICKS] - tickUs[(i - span) % TICKS]);
        sx += k; sy += y; sxx += (double)k * k; sxy += k * y;
      }
      return (uint32_t)((m * sxy - sx * sy) / (m * sxx - sx * sx) * 24 / usPerSample + 0.5);
    };
    if (theirSpb && i >= firstTick + 24 && n - (i - 24) <= TICKS) {
      uint32_t last = fit(24);
      if (fabsf((float)last - theirSpb) > NEW_TEMPO * theirSpb) firstTick = i - 24;
    }
    uint32_t span = i >= firstTick ? min(i - firstTick, SPAN) : 0;
    if (span >= 24 && n - (i - span) <= TICKS) {
      uint32_t spb = fit(span);
      bool sure = span >= SPAN || !theirSpb || fabsf((float)spb - theirSpb) > NEW_TEMPO * theirSpb;
      if (sure && spb >= spbForBpm(MAX_BPM) && spb <= spbForBpm(MIN_BPM)) theirSpb = spb;
    }
    // Their next whole beat reached: our transport from that beat (the clock
    // reads it; the tracks where they'd be).
    if (startAt != INT32_MIN && refTick >= 0 && (int32_t)i >= refTick && theirBeatAt(i) >= startAt - 1e-9) {
      startTickAt = i;                                // (started exactly where they are: no catching up)
      startingBeat = startAt;
      transportStartAt(startAt, pastStartingBeat);
      startAt = INT32_MIN;
      seenTicks++;
      return;                                         // (running from the next block)
    }
    if (!running || !theirSpb) continue;
    // How far we're off their position at this tick: theirs known (bars too),
    // else beats only.
    double ours = ourBeatAt(us, s), theirs = refTick >= 0 ? theirBeatAt(i) : 0;
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
    float most = fabsf(err) > FAR_BEATS ? BIG_NUDGE : MAX_NUDGE;
    float nudge = fabsf(err) < DEADBAND_BEATS ? 0 : constrain(err * NUDGE_GAIN, -most, most);   // (ahead: slower)
    want = (uint32_t)(theirSpb * (1 + nudge) + 0.5f);
  } else if (running) {
    return;                                           // (between: as it is)
  }
  uint32_t now = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  float shownBpm = roundf(bpmForSpb(shown) * 10) / 10;
  if (fabsf((float)want - now) > MIN_CHANGE * now) followTempo(want, shownBpm);
  else if (shownBpm != bpm) followTempo(now, shownBpm);   // (the tempo shown only)
}
