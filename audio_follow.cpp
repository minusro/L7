// audio_follow.cpp — see audio_follow.h
#include "audio_follow.h"
#include "engine.h"
#include "storage.h"
#include "midi_in.h"
using namespace looper;

// Frames: the interrupt's onset measure, one a block (engine.h onsetRing).
static constexpr float FPS = AUDIO_SAMPLE_RATE_EXACT / AUDIO_BLOCK_SAMPLES;   // (~345 a second)
// The tempo: its autocorrelation on frames two blocks long, over the last
// ONSET_FRAMES blocks (~5.9 s), every ESTIMATE_MS once MIN_FRAMES are in (enough
// for every candidate's multiples: each scored on the same number of them).
static constexpr float LO_BPM = 60, HI_BPM = 200;   // (found within; the clock's range is wider)
static constexpr int PAIR = 2;
static constexpr int N2 = ONSET_FRAMES / PAIR;
static constexpr int LAG_MIN = (int)(FPS / PAIR * 60 / HI_BPM);
static constexpr int LAG_MAX = (int)(FPS / PAIR * 60 / LO_BPM) + 1;
static constexpr int COMB = 4;                      // a candidate's lag and its multiples up to this
static constexpr int ACF_MAX = COMB * LAG_MAX + 2;
static constexpr float PRIOR_OCTAVES = 0.6f;        // the weighting towards the tempo set: its width
static constexpr uint32_t ESTIMATE_MS = 500;
static constexpr uint32_t MIN_FRAMES = 1800;        // (~5.2 s heard)
static constexpr float MIN_STRENGTH = 0.08f;        // a tempo this clear at least (its comb's mean autocorrelation, 1: perfect)
// The beat: a rise looked for within WINDOW of a period of where it's due,
// weighted by closeness (SIGMA of a period); found if it stands out
// (HIT_RATIO the input's average rise over the last couple of seconds, and
// HIT_MIN at least). Each found beat moves the beat by ALPHA of its miss and
// the period by BETA (finding them, or a miss of FAST_MISS frames or more:
// their tempo changing, the larger ones).
static constexpr float WINDOW = 0.25f, SIGMA = 0.1f;
static constexpr float HIT_RATIO = 2.5f, HIT_MIN = 0.3f;
static constexpr float ALPHA_FIND = 0.35f, BETA_FIND = 0.06f, ALPHA = 0.2f, BETA = 0.03f;
static constexpr int FIND_BEATS = 8;
static constexpr float FAST_MISS = 3;               // (~9 ms)                // the larger gains for this many beats after a (re)start
static constexpr float LOCK_RATE = 0.6f, LOSE_RATE = 0.3f;   // the found beats' rate (smoothed) that locks / unlocks
// Where in its frame an onset is, as the rise peaks (frames after the block's
// start; the bands' smoothing and the block's own length in it: calibrated in
// the simulator, drums' kicks landing on our beats).
static constexpr float ONSET_DELAY = -0.1f;
// The clock: as midi_in.cpp's (the nudge a beat: at most MAX_NUDGE, GAIN of the
// difference; CATCH_NUDGE / CATCH_GAIN for CATCH_BEATS after a lock or a start,
// and while it's CATCH_BEATS_OFF of a beat off or more).
static constexpr float MAX_NUDGE = 0.005f, GAIN = 0.5f, CATCH_NUDGE = 0.05f, CATCH_GAIN = 1.0f;
static constexpr int CATCH_BEATS = 8;
static constexpr float CATCH_BEATS_OFF = 0.025f;
static constexpr float DEADBAND_BEATS = 0.006f;     // (their beats' jitter: ~3 ms at 120 BPM)
static constexpr float MIN_CHANGE = 0.0015f;
// Silence: the input under SILENT_DBFS for SILENT_MS (sparse music's gaps are
// shorter). Meanwhile nothing changes: no tempo estimated, the clock left as it
// is; their beat counts as lost (it's looked for afresh once they're back).
static constexpr float SILENT_DBFS = -50;
static constexpr uint32_t SILENT_MS = 1500;
static constexpr float SILENT_LEVEL = 32768.0f * 32768.0f * 1e-5f;   // (-50 dBFS, as a mean square)

static bool on = false;
static uint32_t listenFrom = 0;                     // the first frame heard since turned on
static double period = 0;                           // frames a beat (0: none yet)
static double nextBeat = 0;                         // the frame their next beat is due at (as its rise peaks)
static double lastBeat = -1;                        // and their latest (-1: none)
static double heardBeat = -1;                       // their latest as heard (its rise found; else as expected): the clock's
static float hitRate = 0;
static int beatsTracked = 0, catchBeats = 0;
static bool locked = false;
static uint32_t estimateAt = 0;
static int disagree = 0;                            // estimates in a row far from the tracker's period
static int offPhase = 0;                            // and in a row with the beats strongest elsewhere in the period
static bool startPending = false;
static uint32_t loudAt = 0;                         // millis() the input was last above SILENT_DBFS
static bool silent = false;
static bool resuming = false;                       // sound again after silence: its beat found afresh once
static uint32_t resumeFrom = 0;                     // RESUME_BEATS are heard from this frame on (the tempo kept meanwhile)
static constexpr int RESUME_BEATS = 3;
static double pendingFrom = 0;                      // their phase when START was pressed
// The clock at their tempo times 2^octave (B+arrows on the tempo cell: halved
// or doubled; 0 again when turned on). Halved, our beat on every other one of theirs.
static int octave = 0;
static double ratio() { return ldexp(1.0, octave); }

static inline float osf(uint32_t n) { return onsetRing[n % ONSET_FRAMES] * (1.0f / ONSET_SCALE); }
static float osfAt(double f) {                      // (between frames: linear)
  uint32_t i = (uint32_t)f;
  float a = osf(i), b = osf(i + 1), x = (float)(f - i);
  return a + (b - a) * x;
}
static float bpmOfFrames(double frames) { return 60.0f * FPS / (float)frames; }

static void snapshot(uint32_t &count, uint32_t &at) {
  noInterrupts();
  count = onsetCount;
  at = onsetNow;
  interrupts();
}

// The tempo heard, in frames a beat (0: none clear enough); strength: how clear.
FLASHMEM static double estimate(uint32_t count, float centreBpm, float &strength) {   // (flash: twice a second)
  strength = 0;
  uint32_t have = min(count - listenFrom, ONSET_FRAMES - 2);
  if (have < MIN_FRAMES) return 0;
  const int n2 = (int)(have / PAIR);
  float d[N2], r[ACF_MAX + 1];                       // (~6.8 KB of stack, only meanwhile)
  const uint32_t first = count - (uint32_t)n2 * PAIR;
  float mean = 0;
  for (int j = 0; j < n2; j++) mean += d[j] = osf(first + PAIR * j) + osf(first + PAIR * j + 1);
  mean /= n2;
  float r0 = 0;
  for (int j = 0; j < n2; j++) {
    d[j] -= mean;
    r0 += d[j] * d[j];
  }
  if (r0 < 1e-6f * n2) return 0;                     // (silence)
  const int maxL = min(ACF_MAX, n2 - 1);
  for (int L = 1; L <= maxL; L++) {
    float s = 0;
    for (int j = L; j < n2; j++) s += d[j] * d[j - L];
    r[L] = s / (n2 - L) * n2 / r0;
  }
  // Its peaks widened a little (each a frame either side: an onset's peak is
  // about a frame wide, a lag between frames would miss it).
  float prev = r[1];
  for (int L = 2; L < maxL; L++) {
    float here = r[L];
    r[L] = 0.25f * prev + 0.5f * here + 0.25f * r[L + 1];
    prev = here;
  }
  auto comb = [&](double L) {
    float s = 0;
    int n = 0;
    for (int k = 1; k <= COMB; k++) {
      double x = k * L;
      int i = (int)x;
      if (i + 1 > maxL) break;
      float f = (float)(x - i);
      s += r[i] + (r[i + 1] - r[i]) * f;
      n++;
    }
    return n >= 2 ? s / n : -1.0f;
  };
  auto score = [&](double L) {
    float c = comb(L);
    float o = log2f(bpmOfFrames(L * PAIR) / centreBpm) / PRIOR_OCTAVES;
    return max(c, 0.0f) * expf(-0.5f * o * o);
  };
  double best = 0;                                  // (in quarter frames: a whole-frame lag's multiples miss theirs)
  float bestScore = 0;
  for (double L = LAG_MIN; L <= LAG_MAX; L += 0.25) {
    float s = score(L);
    if (s > bestScore) { bestScore = s; best = L; }
  }
  if (!best) return 0;
  double fine = best;                                // between lags (each multiple interpolated)
  for (double L = best - 0.25; L <= best + 0.2501; L += 0.025) {
    float s = score(L);
    if (s > bestScore) { bestScore = s; fine = L; }
  }
  strength = comb(fine);
  if (strength < MIN_STRENGTH) return 0;
  return fine * PAIR;
}

// The frame of their latest beat, for a period (the phase whose beats over the
// last few seconds rise most; -1: not enough heard).
FLASHMEM static double findPhase(uint32_t count, double p) {
  uint32_t have = min(count - listenFrom, ONSET_FRAMES - 2);
  int beats = min(8, (int)((have - 2) / p));
  if (beats < 2) return -1;
  double newest = count - 2.0;                       // (a frame of margin: the next may be in the making)
  double best = -1;
  float bestSum = -1;
  for (double off = 0; off < p; off += 0.5) {
    float s = 0;
    for (int k = 0; k < beats; k++) s += osfAt(newest - off - k * p);
    if (s > bestSum) { bestSum = s; best = newest - off; }
  }
  return best;
}

FLASHMEM static void restart(uint32_t count, double p) {
  double last = findPhase(count, p);
  if (last < 0) return;
  period = p;
  lastBeat = last;
  nextBeat = last + p;
  hitRate = 0.5f;
  beatsTracked = 0;
  disagree = 0;
  Serial.printf("audio follow: %.1f BPM, looking for its beat\n", bpmOfFrames(p));
}

// Our beat position at transport.now `t` (the beats we've played: beatLog); false: not known.
static bool ourBeatAt(uint32_t t, double &pos) {
  int32_t nearest = INT32_MAX;                       // (the latest beat started by then)
  for (int i = 0; i < BEAT_LOG; i++) {
    BeatStart b;
    noInterrupts();
    b.beat = beatLog[i].beat;
    b.now = beatLog[i].now;
    b.spb = beatLog[i].spb;
    interrupts();
    int32_t since = (int32_t)(t - b.now);
    if (!b.spb || since < 0 || since >= nearest || since > (int32_t)(2 * b.spb)) continue;
    nearest = since;
    pos = b.beat + (double)since / b.spb;
  }
  return nearest != INT32_MAX;
}

// Their latest beat on our timeline (transport.now: as heard, less the input's latency).
static uint32_t theirBeatNow(uint32_t count, uint32_t at, double beat = lastBeat) {
  double rel = (beat - ONSET_DELAY - (count - 1)) * AUDIO_BLOCK_SAMPLES - inputLatency() +
               syncOffsetMs[SYNC_AUDIO] * (AUDIO_SAMPLE_RATE_EXACT / 1000.0);   // (the offset set by hand: our beat that much later)
  return at + (uint32_t)(int32_t)floor(rel);
}
static double theirPhase() {                         // how far into their beat we are now (0-1)
  uint32_t count, at;
  snapshot(count, at);
  double since = (int32_t)(transport.now - theirBeatNow(count, at));
  double ph = fmod(since / (period * AUDIO_BLOCK_SAMPLES), 1.0);
  return ph < 0 ? ph + 1 : ph;
}
static double ourPhase() { return fmod(theirPhase() * ratio(), 1.0); }   // (stopped: how far into our beat, theirs scaled)
static uint32_t inBeatNow() {                        // (transportStartAt(): samples into our first beat)
  return (uint32_t)min(ourPhase() * transport.spb, transport.spb - 1.0);
}

static void follow(bool nudged, uint32_t count, uint32_t at) {
  double theirSpb = period * AUDIO_BLOCK_SAMPLES / ratio();   // (our beat's: theirs halved or doubled)
  double want = theirSpb;
  if (nudged) {
    double ours;
    if (!ourBeatAt(theirBeatNow(count, at, heardBeat), ours)) return;
    // Their beat on one of ours (ours halved: on a half of ours, every other one of theirs on a whole one).
    double g = min(ratio(), 1.0), x = ours / g;
    float err = (float)((x - floor(x + 0.5)) * g);   // ahead of them: slower
    bool catching = catchBeats > 0 || fabsf(err) >= CATCH_BEATS_OFF;
    if (catchBeats > 0) catchBeats--;
    float most = catching ? CATCH_NUDGE : MAX_NUDGE, gain = catching ? CATCH_GAIN : GAIN;
    float nudge = fabsf(err) < DEADBAND_BEATS ? 0 : constrain(err * gain, -most, most);
    want = theirSpb * (1 + nudge);
  }
  uint32_t now = transport.pendingSpb ? transport.pendingSpb : transport.spb;
  float raw = bpmForSpb((uint32_t)(theirSpb + 0.5)), shown = fabsf(raw - bpm) < 0.08f ? bpm : roundf(raw * 10) / 10;
  if (fabs(want - now) > MIN_CHANGE * now) followTempo((uint32_t)(want + 0.5), shown);
  else if (shown != bpm) followTempo(now, shown);
}

// Their next beat looked for (its window all heard): the tracker moved, the clock with it.
static void trackBeat(uint32_t count, uint32_t at) {
  const double p = period;
  int lo = (int)ceil(nextBeat - WINDOW * p), hi = (int)floor(nextBeat + WINDOW * p);
  int bestN = -1;
  float bestV = -1;
  for (int n = lo; n <= hi; n++) {
    float x = (float)((n - nextBeat) / (SIGMA * p));
    float v = osf((uint32_t)n) * expf(-0.5f * x * x);
    if (v > bestV) { bestV = v; bestN = n; }
  }
  float peak = osf((uint32_t)bestN);
  float avg = 0;                                     // their average rise, the last ~2 s
  const int span = (int)(2 * FPS);
  for (int k = 1; k <= span; k++) avg += osf(count - k);
  avg /= span;
  bool hit = peak > HIT_MIN && peak > HIT_RATIO * avg;
  bool finding = beatsTracked < FIND_BEATS || !locked;
  if (hit) {
    float a = osf((uint32_t)bestN - 1), c = osf((uint32_t)bestN + 1);   // (its peak between frames)
    float den = a - 2 * peak + c;
    double at2 = bestN + (den < 0 ? constrain(0.5f * (a - c) / den, -0.5f, 0.5f) : 0);
    double miss = at2 - nextBeat;
    bool fast = finding || fabs(miss) >= FAST_MISS;
    nextBeat += (fast ? ALPHA_FIND : ALPHA) * miss;
    period += (fast ? BETA_FIND : BETA) * miss;
    heardBeat = at2;
  } else {
    heardBeat = nextBeat;
  }
  hitRate += ((hit ? 1.0f : 0.0f) - hitRate) * 0.25f;
  beatsTracked++;
  lastBeat = nextBeat;
  nextBeat = lastBeat + period;
  bool was = locked;
  if (!locked && hitRate >= LOCK_RATE && beatsTracked >= 4) locked = true;
  if (locked && hitRate < LOSE_RATE) locked = false;
  if (locked != was) {
    Serial.printf("audio follow: %s (%.2f BPM)\n", locked ? "locked" : "lost the beat", bpmOfFrames(period));
    if (locked) catchBeats = CATCH_BEATS;
  }
  float b = bpmOfFrames(period);
  if (b < LO_BPM * 0.9f || b > HI_BPM * 1.1f) {       // (wandered off: afresh)
    period = 0;
    locked = false;
    return;
  }
  if (audioFollowActive() && !silent && (locked || !transport.running)) follow(locked && transport.running, count, at);   // (stopped: the tempo, its beat found or not; silence: as it is)
}

FLASHMEM void audioFollowBegin(bool o) {
  on = !o;
  audioFollowToggle();
}

bool audioFollowOn() { return on; }

FLASHMEM void audioFollowToggle() {
  on = !on;
  if (on) {
    noInterrupts();
    listenFrom = onsetCount;
    interrupts();
    onsetListen = true;
  } else {
    onsetListen = false;
    if (locked && period > 0 && !midiClockFollowing()) {   // (the tempo theirs, without the nudge)
      uint32_t count, at;
      snapshot(count, at);
      follow(false, count, at);
    }
  }
  period = 0;
  lastBeat = -1;
  locked = false;
  startPending = false;
  octave = 0;
  estimateAt = millis();
  loudAt = millis();
  silent = false;
  resuming = false;
}

bool audioFollowActive() { return on && !midiClockFollowing(); }
void audioFollowOffsetChanged() { catchBeats = CATCH_BEATS; }
int syncSource() { return midiClockFollowing() ? midiFollowSource() : audioFollowActive() ? SYNC_AUDIO : SYNC_NONE; }
bool audioFollowLocked() { return audioFollowActive() && locked && !silent; }
float audioFollowBpm() { return period > 0 ? bpmOfFrames(period) * (float)ratio() : 0; }

FLASHMEM bool audioFollowScale(int by) {
  if (!on || !by) return false;
  if (period <= 0) {                                 // nothing found yet: the tempo shown halved / doubled, which the estimate leans towards
    float b = roundf(bpm * (by > 0 ? 20 : 5)) / 10;
    if (b < MIN_BPM || b > MAX_BPM || midiClockFollowing()) return false;
    followTempo(spbForBpm(b), b);
    Serial.printf("audio follow: looking near %.1f BPM\n", b);
    return true;
  }
  int o = octave + (by > 0 ? 1 : -1);
  if (o < -3 || o > 3) return false;
  float heard = period > 0 ? bpmOfFrames(period) : 0;
  if (heard > 0 && (heard * ldexpf(1, o) < MIN_BPM || heard * ldexpf(1, o) > MAX_BPM)) return false;   // (the clock's range)
  octave = o;
  Serial.printf("audio follow: x%g\n", ratio());
  if (heard > 0 && audioFollowActive() && !silent && (locked || !transport.running)) {   // (the clock: at once)
    uint32_t count, at;
    snapshot(count, at);
    catchBeats = CATCH_BEATS;
    follow(false, count, at);
  }
  return true;
}

void audioFollowService() {
  if (!on) return;
  uint32_t count, at;
  snapshot(count, at);
  if (onsetLevel > SILENT_LEVEL) loudAt = millis();
  bool wasSilent = silent;
  silent = millis() - loudAt >= SILENT_MS;
  if (silent != wasSilent) {
    Serial.printf("audio follow: %s\n", silent ? "silence (the tempo kept)" : "sound again");
    if (silent) {
      locked = false;                                // (their beat: to be found again)
      hitRate = 0;
      startPending = false;
    } else if (period > 0) {
      resuming = true;
      resumeFrom = count;
    }
  }
  if (resuming && !silent && (double)(count - resumeFrom) >= RESUME_BEATS * period + 2) {
    double last = findPhase(count, period);          // (the silence before adds nothing: their beat in what's come since)
    if (last >= 0) {
      lastBeat = heardBeat = last;
      nextBeat = last + period;
      beatsTracked = 0;
      hitRate = 0.5f;
    }
    resuming = false;
  }
  if (millis() - estimateAt >= ESTIMATE_MS && !silent && !resuming) {   // the tempo, afresh (not in silence)
    estimateAt = millis();
    float strength;
    double est = estimate(count, bpm / (float)ratio(), strength);   // (towards the tempo shown: the one set, or theirs once followed; halved or doubled: theirs)
    if (est > 0) {
      if (period <= 0) {
        restart(count, est);
        if (period > 0 && !transport.running && audioFollowActive()) follow(false, count, at);   // (stopped: the tempo at once)
      } else if (fabs(est / period - 1) > 0.04) {    // another tempo (theirs changed, or the tracker's off)
        if (++disagree >= (locked ? 6 : 2)) restart(count, est);
      } else {
        disagree = 0;
        if (!locked && hitRate < LOSE_RATE && beatsTracked >= 8) restart(count, est);   // (the tempo right, the beat not found: its phase afresh)
      }
    }
    // The beat itself, checked: the phase whose beats rose most over the last
    // few seconds, at the tracker's period; far from the tracker's (an off-beat
    // taken: a hat on the eighths) a few times in a row, moved there.
    if (period > 0 && lastBeat >= 0 && locked) {
      double best = findPhase(count, period);
      double d = best < 0 ? 0 : fmod((best - lastBeat) / period, 1.0);
      if (d < -0.5) d += 1;
      if (d >= 0.5) d -= 1;
      if (fabs(d) > 0.2) {
        if (++offPhase >= 3) {
          lastBeat += d * period;
          nextBeat += d * period;
          offPhase = 0;
          catchBeats = CATCH_BEATS;
          Serial.printf("audio follow: the beat moved %+.2f\n", d);
        }
      } else {
        offPhase = 0;
      }
    }
  }
  while (period > 0 && !resuming && (double)(count - 1) >= nextBeat + WINDOW * period + 1) trackBeat(count, at);
  if (startPending) {                                // START pressed late in their beat: on their next one
    if (!locked || transport.running || !audioFollowActive()) {
      startPending = false;
    } else if (theirPhase() < pendingFrom - 0.25) {
      startPending = false;
      catchBeats = CATCH_BEATS;
      transportStartAt(0, inBeatNow);
    }
  }
}

bool audioFollowStart() {
  if (!audioFollowLocked() || transport.running) return false;
  double ph = theirPhase();
  catchBeats = CATCH_BEATS;
  if (ph < 0.5) {
    transportStartAt(0, inBeatNow);                  // just past their beat: from there
  } else {
    startPending = true;                             // before it: on it
    pendingFrom = ph;
  }
  return true;
}

bool audioFollowFlash() {
  if (!audioFollowActive()) return false;
  if (transport.running) {
    noInterrupts();
    bool first = transport.sampleInBeat < transport.spb / 2;
    interrupts();
    return first;
  }
  if (period > 0 && lastBeat >= 0) return ourPhase() < 0.5;   // stopped: their beat (halved or doubled), as the tracker has it (locked or not yet)
  uint32_t msPerBeat = (uint32_t)(60000 / bpm);    // (nothing found yet: the tempo shown, still listening)
  return millis() % msPerBeat < msPerBeat / 2;
}
