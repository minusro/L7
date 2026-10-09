// engine.cpp — see engine.h
#include "engine.h"

namespace looper {

// 7 x 64 KB, all in RAM2 next to the other DMAMEM buffers.
DMAMEM static uint32_t rings[NUM_TRACKS][RING_FRAMES];
uint32_t inputRing[INPUT_FRAMES];

Track tracks[NUM_TRACKS];
Transport transport;
Recording recording;
volatile uint8_t silencedTracks = 0;
volatile uint32_t silenceFrom = 0;

static struct RingInit {
  RingInit() {
    for (int t = 0; t < NUM_TRACKS; t++) tracks[t].ring = rings[t];
  }
} ringInit;

uint32_t spbForBpm(float bpm) { return (uint32_t)(AUDIO_SAMPLE_RATE_EXACT * 60.0f / bpm + 0.5f); }
float bpmForSpb(uint32_t spb) { return AUDIO_SAMPLE_RATE_EXACT * 60.0f / spb; }

// ---- Filters
float filterStepHz(int step) { return FILTER_MIN_HZ * powf(2.0f, step * FILTER_OCTAVES / FILTER_MAX); }

// Butterworth (Q = 1/sqrt 2) sections from the RBJ cookbook.
static Biquad butterworth(float hz, bool highpass) {
  float w0 = 2 * (float)M_PI * hz / AUDIO_SAMPLE_RATE_EXACT;
  float c = cosf(w0), alpha = sinf(w0) / (2 * 0.70710678f), a0 = 1 + alpha;
  Biquad q;
  float k = highpass ? (1 + c) / 2 : (1 - c) / 2;
  q.b0 = k / a0;
  q.b1 = (highpass ? -2 * k : 2 * k) / a0;
  q.b2 = k / a0;
  q.a1 = -2 * c / a0;
  q.a2 = (1 - alpha) / a0;
  return q;
}

FLASHMEM void setTrackFilter(int t, int lo, int hi) {   // (flash: on edits only; RAM1 is full)
  Track &tr = tracks[t];
  if (hi < lo) hi = lo;
  if (hi - lo > FILTER_MAX) hi = lo + FILTER_MAX;
  if (hi < 0) { lo -= hi; hi = 0; }
  if (lo > FILTER_MAX) { hi -= lo - FILTER_MAX; lo = FILTER_MAX; }
  float hpHz = filterStepHz(constrain(lo, 1, FILTER_MAX)), lpHz = filterStepHz(constrain(hi, 0, FILTER_MAX - 1));
  noInterrupts();
  tr.filterLo = lo;
  tr.filterHi = hi;
  tr.hpTarget = hpHz;                            // (the interrupt glides there: glideFilters())
  tr.lpTarget = lpHz;
  tr.hpOn = lo > 0;
  tr.lpOn = hi < FILTER_MAX;
  // One faded out (or never on): at its cutoff at once, clean, fading in from there.
  if (tr.hpWet == 0) { tr.hpHz = hpHz; tr.hp = butterworth(hpHz, true); memset(tr.filterZ[0], 0, sizeof tr.filterZ[0]); }
  if (tr.lpWet == 0) { tr.lpHz = lpHz; tr.lp = butterworth(lpHz, false); memset(tr.filterZ[1], 0, sizeof tr.filterZ[1]); }
  interrupts();
}

float filterResponseDb(int lo, int hi, float hz) {
  float db = 0;
  if (lo > 0) db -= 10 * log10f(1 + powf(filterStepHz(lo) / hz, 4));
  if (hi < FILTER_MAX) db -= 10 * log10f(1 + powf(hz / filterStepHz(hi), 4));
  return db;
}

// ---- Volume and balance
uint16_t peakOf(const uint8_t *o) {
  uint16_t p = o[OVERVIEW_COLS] | o[OVERVIEW_COLS + 1] << 8;
  if (p) return p;
  uint8_t top = 0;                                   // (not kept: from the overview, on its square-root scale)
  for (int c = 0; c < OVERVIEW_COLS; c++) top = max(top, o[c]);
  float f = top / 255.0f;
  return (uint16_t)min(32767.0f, f * f * 32768.0f + 0.5f);
}
float headroomDbOf(const uint8_t *o) {
  uint16_t p = peakOf(o);
  return p < QUIET_PEAK ? 0.0f : max(0.0f, -20 * log10f(p / 32768.0f));
}
float overviewNormOf(const uint8_t *o) {
  uint16_t p = peakOf(o);
  return p < QUIET_PEAK ? 1.0f : 1 / sqrtf(p / 32768.0f);
}
int unityStepOf(float d) {
  int ups = (int)ceilf(d / VOLUME_STEP_DB - 0.01f);  // (steps above unity to 0 dBFS, the last a part one)
  return max(1, VOLUME_MAX - max(0, ups));
}
int volumeStepOf(int stored, float d) {
  return stored == VOLUME_OFF ? 0 : constrain(unityStepOf(d) + stored - VOLUME_UNITY, 1, VOLUME_MAX);
}
int volumeStoredOf(int step, float d) {
  return step <= 0 ? VOLUME_OFF : VOLUME_UNITY + min(step, VOLUME_MAX) - unityStepOf(d);
}
float volumeGainOf(int stored, float d) {
  if (stored == VOLUME_OFF) return 0.0f;
  int v0 = unityStepOf(d), k = volumeStepOf(stored, d) - v0, ups = VOLUME_MAX - v0;
  float db = k <= 0 ? k * VOLUME_STEP_DB : k >= ups ? d : k * VOLUME_STEP_DB;   // (the top: 0 dBFS exactly)
  return powf(10.0f, db / 20);
}

FLASHMEM void setTrackMix(int t, int volume, int balance) {
  Track &tr = tracks[t];
  if (volume != VOLUME_OFF) volume = constrain(volume, -100, 100);   // (steps from unity: the clip's scale limits them, volumeStepOf())
  balance = constrain(balance, -BALANCE_MAX, BALANCE_MAX);
  float g = volumeGainOf(volume, headroomDbOf(tr.overview));
  float toL = balance < 0 ? -(float)balance / BALANCE_MAX : 0;   // share of the right channel moved left
  float toR = balance > 0 ? (float)balance / BALANCE_MAX : 0;    // share of the left channel moved right
  noInterrupts();
  tr.volume = volume;
  tr.balance = balance;
  tr.mixLL = g * (1 - toR);
  tr.mixLR = g * toL;
  tr.mixRL = g * toR;
  tr.mixRR = g * (1 - toL);
  tr.levelL = tr.mixLL + tr.mixLR;
  tr.levelR = tr.mixRL + tr.mixRR;
  interrupts();
}

static int masterStep = MASTER_VOLUME_UNITY, masterBal = 0;
static volatile float outputGain = 1.0f;          // the master volume's
static volatile float outLL = 1, outLR = 0, outRL = 0, outRR = 1;   // and with its balance (as a track's: setTrackMix())
volatile bool metronomeOn = false;
volatile bool inputMonitored = true;
volatile bool metronomeSteady = false;
volatile float lineInPeak = 0, lineInFloor = 1;
volatile bool usbAudioIn = false;
bool (*usbAudioRead)(uint32_t *frames) = nullptr;
int16_t *(*usbAudioWrite)() = nullptr;
void (*usbAudioWritten)() = nullptr;
// ---- USB's input latency (engine.h): sharp sounds near our beats, timed.
static constexpr uint32_t USB_LATENCY_GUESS = LATENCY_FRAMES + AUDIO_BLOCK_SAMPLES;   // (L7's own USB buffering; a computer's not known yet)
volatile uint32_t usbInputLatency = USB_LATENCY_GUESS;
uint32_t inputLatency() { return usbAudioIn ? usbInputLatency : LATENCY_FRAMES; }
struct UsbOnset { int32_t beat; uint32_t inBeat, spb; };
static constexpr uint32_t USB_ONSETS = 16;
static volatile UsbOnset usbOnsets[USB_ONSETS];
static volatile uint32_t usbOnsetHead = 0;
static float usbEnv = 0;                             // the USB input's slow average level (interrupt)
static bool usbOnsetArmed = false;                   // (one a beat: its first)
static constexpr float LINE_GATE_LEVEL = 0.0015f;   // ~ -56 dBFS (as measured on the M8: see the log)
static constexpr uint32_t LINE_GATE_HOLD_MS = 2000;
static constexpr bool USB_OUT_CLICK = false;          // the metronome on the USB output too (measuring: tools/latency/clocksync.c)
volatile int32_t clickFromBeat = INT32_MIN;

// The metronome's wood block: two damped sines (its body and a harder upper
// partial), started on a beat.
static constexpr float CLICK_BAR_HZ = 1600, CLICK_BEAT_HZ = 1150;
// Its level (the metronome's auto volume): what's heard (after the master
// volume, the click left out) followed by its peak, rising at once and falling
// REF_FALL_DB a second; at least -24 dBFS at the master volume (all the way
// down: none); the bar's first beat at it, the others 3/4 of it.
static constexpr float REF_FALL_DB = 6;
static const float REF_FALL = powf(10, -REF_FALL_DB / 20 * AUDIO_BLOCK_SAMPLES / AUDIO_SAMPLE_RATE_EXACT);   // (a block's)
static float refPeak = 0;
static float clickLevel() {
  float m = max(0.0631f * 32767 * outputGain, refPeak);   // (-24 dBFS, at the master volume)
  return min(m, 32767.0f) / 1.4f;                  // (the two partials' peak together: ~1.4 of the body's)
}
static const float CLICK_DECAY = expf(-1.0f / (0.012f * AUDIO_SAMPLE_RATE_EXACT));      // body, ~12 ms
static const float CLICK_DECAY_HI = expf(-1.0f / (0.004f * AUDIO_SAMPLE_RATE_EXACT));   // partial, ~4 ms
static float clickAmp = 0, clickAmpHi = 0, clickPhase = 0, clickStep = 0;

static void startClick(bool bar) {
  float hz = bar ? CLICK_BAR_HZ : CLICK_BEAT_HZ;
  clickStep = 2 * (float)M_PI * hz / AUDIO_SAMPLE_RATE_EXACT;
  clickPhase = 0;
  clickAmp = clickLevel() * (bar ? 1.0f : 0.75f);
  clickAmpHi = clickAmp * 0.5f;
}

static volatile uint8_t clickAsked = 0;          // metronomeClick(): 1 a beat's, 2 a bar's (the next block)
void metronomeClick(bool bar) { clickAsked = bar ? 2 : 1; }

static float clickSample() {
  if (clickAmp < 1) return 0;
  float s = clickAmp * sinf(clickPhase) + clickAmpHi * sinf(clickPhase * 2.76f);
  clickPhase += clickStep;                       // (from 0 on each click: it's over long before precision matters)
  clickAmp *= CLICK_DECAY;
  clickAmpHi *= CLICK_DECAY_HI;
  return s;
}
static volatile float outPeakL = 0, outPeakR = 0;
FLASHMEM static void setOutputMix() {
  float g = masterStep ? powf(10.0f, (masterStep - MASTER_VOLUME_UNITY) * MASTER_STEP_DB / 20) : 0.0f;
  float toL = masterBal < 0 ? -(float)masterBal / MASTER_BALANCE_MAX : 0, toR = masterBal > 0 ? (float)masterBal / MASTER_BALANCE_MAX : 0;
  noInterrupts();
  outputGain = g;
  outLL = g * (1 - toR);
  outLR = g * toL;
  outRL = g * toR;
  outRR = g * (1 - toL);
  interrupts();
}
void setMasterVolume(int step) {
  masterStep = constrain(step, 0, MASTER_VOLUME_MAX);
  setOutputMix();
}
int masterVolume() { return masterStep; }
void setMasterBalance(int balance) {
  masterBal = constrain(balance, -MASTER_BALANCE_MAX, MASTER_BALANCE_MAX);
  setOutputMix();
}
int masterBalance() { return masterBal; }
void takeOutputPeaks(float &left, float &right) {
  noInterrupts();
  left = outPeakL;
  right = outPeakR;
  outPeakL = outPeakR = 0;
  interrupts();
}

void takeTrackPeaks(int t, float &left, float &right) {
  Track &tr = tracks[t];
  noInterrupts();
  left = tr.peakL;
  right = tr.peakR;
  tr.peakL = 0;
  tr.peakR = 0;
  interrupts();
}

// ---- Time stretching (engine.h Track::stretch): WSOLA. Grains of
// STRETCH_GRAIN frames, a new one every STRETCH_HOP output samples (half a
// grain: two overlap, their Hann windows summing to 1). Each new grain is read
// from around the stream frame the playhead puts there (centred: its middle
// where the playhead will be by then), within STRETCH_SEARCH either way, where
// it best continues the grain before it (the normalised cross-correlation of
// its first half with where the grain before would have gone on: at an eighth
// of the rate, then to the frame, nearest first). Played at
// the tempo recorded (speed 1) that's where the grain before goes on: the
// stream as it is.
static constexpr int STRETCH_GRAIN = 1024, STRETCH_HOP = STRETCH_GRAIN / 2;
static constexpr int STRETCH_SEARCH = 256, STRETCH_DECIM = 8, STRETCH_FINE = 5;
static constexpr float STRETCH_NEAR = 0.95f;
static float stretchWindow[STRETCH_HOP + 1];      // its first half (and the middle): w(n) = w(GRAIN - n)
static inline float stretchW(int n) { return stretchWindow[n <= STRETCH_HOP ? n : STRETCH_GRAIN - n]; }
static inline void frameAt(const uint32_t *ring, uint32_t f, float &l, float &r) {
  uint32_t v = ring[f % RING_FRAMES];
  l = (int16_t)(v & 0xFFFF);
  r = (int16_t)(v >> 16);
}

// A new grain (when the one before is half way): its stream frame. Compared
// on one signal: left + right, or left - right where that's the louder (a mono
// sum would lose what's opposite in the channels).
static uint32_t placeGrain(const Track &tr, uint32_t natural, uint32_t target) {
  // The signal: from where the grain before would go on.
  constexpr int REF = STRETCH_HOP / STRETCH_DECIM, LAGS = 2 * STRETCH_SEARCH / STRETCH_DECIM;
  float refL[REF], refR[REF];
  float mid = 0, side = 0;
  for (int j = 0; j < REF; j++) {
    frameAt(tr.ring, natural + j * STRETCH_DECIM, refL[j], refR[j]);
    float m = refL[j] + refR[j], d = refL[j] - refR[j];
    mid += m * m;
    side += d * d;
  }
  const float sgn = side > mid ? -1.0f : 1.0f;
  float ref[REF];
  for (int j = 0; j < REF; j++) ref[j] = refL[j] + sgn * refR[j];
  // Coarse: every STRETCH_DECIM-th frame; each lag's energy from running sums.
  const uint32_t from = target - STRETCH_SEARCH;
  float cand[REF + LAGS], energy[REF + LAGS + 1];
  energy[0] = 0;
  for (int j = 0; j < REF + LAGS; j++) {
    float l, r;
    frameAt(tr.ring, from + j * STRETCH_DECIM, l, r);
    cand[j] = l + sgn * r;
    energy[j + 1] = energy[j] + cand[j] * cand[j];
  }
  // The nearest lag scoring within STRETCH_NEAR of the best (a periodic sound
  // matches as well a period further: the nearest keeps it where it belongs).
  float scores[LAGS + 1], top = -1e30f;
  for (int lag = 0; lag <= LAGS; lag++) {
    float c = 0;
    const float *x = cand + lag;
    for (int j = 0; j < REF; j++) c += ref[j] * x[j];
    scores[lag] = c / sqrtf(energy[lag + REF] - energy[lag] + 1);
    top = max(top, scores[lag]);
  }
  const int centre = LAGS / 2;
  const float good = top > 0 ? top * STRETCH_NEAR : top;
  int best = centre;
  for (int d = 0; d <= centre; d++) {
    if (scores[centre - d] >= good) { best = centre - d; break; }
    if (scores[centre + d] >= good) { best = centre + d; break; }
  }
  // Fine: each frame round it, on the first FINE_LEN frames.
  constexpr int FINE_LEN = STRETCH_HOP / 2, SPAN = 2 * STRETCH_FINE;
  const int32_t coarse = (best - centre) * STRETCH_DECIM;
  float nat[FINE_LEN], at[FINE_LEN + SPAN];
  for (int j = 0; j < FINE_LEN; j++) { float l, r; frameAt(tr.ring, natural + j, l, r); nat[j] = l + sgn * r; }
  for (int j = 0; j < FINE_LEN + SPAN; j++) { float l, r; frameAt(tr.ring, target + coarse - STRETCH_FINE + j, l, r); at[j] = l + sgn * r; }
  int bestK = STRETCH_FINE;
  float bestF = -1e30f;
  for (int d = 0; d <= SPAN; d++) {
    int k = STRETCH_FINE + (d % 2 ? (d + 1) / 2 : -(d / 2));   // (the middle, +1, -1, +2, ...: nearest first)
    float c = 0, e = 1;
    for (int j = 0; j < FINE_LEN; j++) { c += nat[j] * at[k + j]; e += at[k + j] * at[k + j]; }
    float sc = c / sqrtf(e);
    if (sc > bestF) { bestF = sc; bestK = k; }
  }
  return target + coarse - STRETCH_FINE + bestK;
}

// ---- Transients (engine.h findTransients()): the energy of each window of
// TR_WINDOW frames, against the windows' average before it (an exponential one,
// TR_SLOW a window): TR_RISE times that and over TR_FLOOR, at least TR_GAP
// frames after the last, is one, placed TR_EARLY frames before its window
// (an attack starts in it, maybe just before: better a little early than cut).
static constexpr uint32_t TR_WINDOW = 64, TR_GAP = 2205, TR_EARLY = 32;   // (TR_GAP: 50 ms)
static constexpr float TR_SLOW = 0.1f, TR_RISE = 8.0f;   // (~9 dB above the last ~15 ms)
static constexpr float TR_FLOOR = 2 * 180.0f * 180.0f;   // (~-45 dBFS, both channels)

void findTransients(Track &tr, uint32_t at, const uint32_t *frames, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (tr.onsetFrames == 0) tr.onsetWindow = at + i;
    uint32_t v = frames[i];
    float l = (int16_t)(v & 0xFFFF), r = (int16_t)(v >> 16);
    tr.onsetSum += l * l + r * r;
    if (++tr.onsetFrames < TR_WINDOW) continue;
    float e = tr.onsetSum / TR_WINDOW;
    if (e > TR_FLOOR && e > tr.onsetSlow * TR_RISE && (int32_t)(tr.onsetWindow - tr.onsetLast) >= (int32_t)TR_GAP) {
      tr.onsetLast = tr.onsetWindow;
      if (tr.onsetIn - tr.onsetOut < Track::ONSET_QUEUE) {   // (full: this one's left out)
        tr.onsets[tr.onsetIn % Track::ONSET_QUEUE] = tr.onsetWindow - TR_EARLY;
        __asm__ volatile("" ::: "memory");
        tr.onsetIn = tr.onsetIn + 1;
      }
    }
    tr.onsetSlow += (e - tr.onsetSlow) * TR_SLOW;
    tr.onsetSum = 0;
    tr.onsetFrames = 0;
  }
}

void resetTransients(Track &tr, uint32_t at) {
  noInterrupts();
  tr.onsetOut = tr.onsetIn;
  interrupts();
  tr.onsetSlow = tr.onsetSum = 0;
  tr.onsetFrames = 0;
  tr.onsetLast = at - TR_GAP;
}

void dropTransientsFrom(Track &tr, uint32_t at) {
  noInterrupts();
  while (tr.onsetIn != tr.onsetOut && (int32_t)(tr.onsets[(tr.onsetIn - 1) % Track::ONSET_QUEUE] - at) >= 0)
    tr.onsetIn = tr.onsetIn - 1;
  interrupts();
  tr.onsetSum = 0;
  tr.onsetFrames = 0;                              // (the window starts again where the stream goes on)
}

// Transient locking: a grain reading towards the next transient fades out over
// LOCK_FADE_IN frames before it (it plays nothing past it: no attack early, or
// twice); when the playhead reaches it, the grains start afresh on it (one at
// full level from the transient on, its partner rising: the attack as it was,
// on time) and the ones before fade out over LOCK_FADE_OUT samples. A transient
// the playhead is already LOCK_STALE frames past (a start part way in): passed
// over.
static constexpr int LOCK_FADE_IN = 32, LOCK_FADE_OUT = 64;
static constexpr int32_t LOCK_STALE = 512;
bool transientLock = true;
static inline float beforeLimit(uint32_t f, uint32_t limit) {   // (a grain's frame f, the transient at limit)
  int32_t d = (int32_t)(limit - f);
  return d >= LOCK_FADE_IN ? 1.0f : d <= 0 ? 0.0f : d * (1.0f / LOCK_FADE_IN);
}

// The grains afresh from the playhead: two, both from it (their windows
// summing to 1), the next one due `toNext` samples on.
static inline void grainsFrom(Track &tr, uint32_t tail, int o) {
  tr.grainPos[0] = STRETCH_HOP + o;
  tr.grainSrc[0] = tail - tr.grainPos[0];
  tr.grainPos[1] = o;
  tr.grainSrc[1] = tail - o;
  tr.grainGain[0] = tr.grainGain[1] = 1;
  tr.toNextGrain = STRETCH_HOP - o;
}

// The track's sample now, stretched (its playhead at tail, moving at speed).
static inline void stretchSample(Track &tr, uint32_t tail, double speed, float &sl, float &sr) {
  if (!tr.grainsOn) {                              // afresh: each track at another point of the hop (their
    grainsFrom(tr, tail, (int)((&tr - tracks) * STRETCH_HOP / NUM_TRACKS));   // searches spread over the blocks)
    tr.grainPos[2] = tr.grainPos[3] = -1;
    tr.grainsOn = true;
  }
  // The next transient: reached, the grains from it (the ones before fading out).
  bool ahead = tr.onsetOut != tr.onsetIn;
  uint32_t onset = ahead ? tr.onsets[tr.onsetOut % Track::ONSET_QUEUE] : 0;
  while (ahead && (int32_t)(tail - onset) >= 0) {
    if ((int32_t)(tail - onset) < LOCK_STALE && transientLock) {
      for (int g = 0; g < 2; g++) {
        tr.grainSrc[2 + g] = tr.grainSrc[g];
        tr.grainPos[2 + g] = tr.grainPos[g];
        tr.grainGain[2 + g] = 1;
      }
      tr.fadeLimit = onset;
      grainsFrom(tr, tail, 0);
    }
    tr.onsetOut = tr.onsetOut + 1;
    ahead = tr.onsetOut != tr.onsetIn;
    onset = ahead ? tr.onsets[tr.onsetOut % Track::ONSET_QUEUE] : 0;
  }
  if (tr.toNextGrain <= 0) {                       // (the older one has just ended)
    int old = tr.grainPos[0] >= 0 ? 0 : 1, fresh = 1 - old;
    uint32_t natural = tr.grainSrc[old] + tr.grainPos[old];   // (where the one playing would go on)
    uint32_t target = tail + (int32_t)lround((speed - 1) * STRETCH_HOP);   // (centred: its middle on the playhead then)
    tr.grainSrc[fresh] = placeGrain(tr, natural, target);
    tr.grainPos[fresh] = 0;
    tr.toNextGrain = STRETCH_HOP;
  }
  sl = sr = 0;
  for (int g = 0; g < 4; g++) {
    int pos = tr.grainPos[g];
    if (pos < 0) continue;
    uint32_t f = tr.grainSrc[g] + pos;
    float w = stretchW(pos);
    if (g < 2) {
      if (ahead && transientLock) w *= beforeLimit(f, onset);   // (not past the next transient)
    } else {                                       // (the ones before the last transient: fading out)
      w *= tr.grainGain[g] * beforeLimit(f, tr.fadeLimit);
      tr.grainGain[g] -= 1.0f / LOCK_FADE_OUT;
      if (tr.grainGain[g] <= 0) pos = STRETCH_GRAIN;
    }
    uint32_t v = tr.ring[f % RING_FRAMES];
    sl += w * (int16_t)(v & 0xFFFF);
    sr += w * (int16_t)(v >> 16);
    tr.grainPos[g] = pos + 1 < STRETCH_GRAIN ? pos + 1 : -1;
  }
  tr.toNextGrain--;
}

// Not stretching: the transients the playhead has passed, let go.
static inline void passTransients(Track &tr, uint32_t tail) {
  while (tr.onsetOut != tr.onsetIn && (int32_t)(tail - tr.onsets[tr.onsetOut % Track::ONSET_QUEUE]) >= 0)
    tr.onsetOut = tr.onsetOut + 1;
}

// The engine's CPU use (takeEngineLoad()).
static volatile uint32_t loadCycles = 0, loadMost = 0, loadBlocks = 0;
void takeEngineLoad(float &avg, float &most) {
#if defined(ARM_DWT_CYCCNT)
  noInterrupts();
  uint32_t c = loadCycles, m = loadMost, n = loadBlocks;
  loadCycles = loadMost = loadBlocks = 0;
  interrupts();
  const float blockCycles = (float)F_CPU_ACTUAL * AUDIO_BLOCK_SAMPLES / AUDIO_SAMPLE_RATE_EXACT;
  avg = n ? c / (float)n / blockCycles : 0;
  most = m / blockCycles;
#else
  avg = most = 0;
#endif
}

static inline float runBiquad(const Biquad &q, float *z, float x) {
  float y = q.b0 * x + z[0];
  z[0] = q.b1 * x - q.a1 * y + z[1];
  z[1] = q.b2 * x - q.a2 * y;
  return y;
}

// ---- MIDI clock
// The audio interrupt knows exactly which sample each clock tick falls on, but
// runs once per 128-sample block. It queues each tick with the time it should
// go out; a 200 us timer sends it then, so ticks have <0.2 ms of jitter
// instead of a whole block (2.9 ms).
static HardwareSerial *midiPort = nullptr;
static IntervalTimer midiTimer;
static constexpr uint32_t TICK_QUEUE = 64;
static volatile uint32_t tickDue[TICK_QUEUE];
static volatile uint32_t tickHead = 0, tickTail = 0;     // audio interrupt / timer
static constexpr uint32_t MSG_QUEUE = 16;
static volatile uint8_t msgQueue[MSG_QUEUE];
static volatile uint32_t msgHead = 0, msgTail = 0;       // loop() / timer
// A second reader, USB MIDI (midi_usb.cpp, its own timer): its own places in both.
static volatile bool usbOut = false;
static volatile uint32_t usbTickTail = 0, usbMsgTail = 0;

volatile BlockStamp blockStamp;
volatile bool onsetListen = false;
DMAMEM uint16_t onsetRing[ONSET_FRAMES];
volatile uint32_t onsetCount = 0, onsetNow = 0;
volatile float onsetLevel = 0;
volatile BeatStart beatLog[BEAT_LOG];
// The onset measure's state (interrupt): the bands' filters (one-pole: the
// lows two below ~150 Hz, the highs above ~2.5 kHz), their energies smoothed
// over ~10 ms, and each band's log energy at the last block's end.
static float onLo1 = 0, onLo2 = 0, onHiLp = 0, onE[3] = {}, onPrev[3] = {};
static constexpr float ON_K_LO = 2 * 3.14159265f * 150 / AUDIO_SAMPLE_RATE_EXACT;
static constexpr float ON_K_HI = 2 * 3.14159265f * 2500 / AUDIO_SAMPLE_RATE_EXACT;
static constexpr float ON_K_E = 1.0f / (0.010f * AUDIO_SAMPLE_RATE_EXACT);
void (*midiInPollHook)() = nullptr;

static void midiTimerIsr() {
  if (midiInPollHook) midiInPollHook();
  while (msgTail != msgHead) midiPort->write(msgQueue[msgTail++ % MSG_QUEUE]);
  uint32_t now = micros();
  while (tickTail != tickHead && (int32_t)(now - tickDue[tickTail % TICK_QUEUE]) >= 0) {
    midiPort->write(0xF8);
    tickTail++;
  }
}

void midiBegin(HardwareSerial *port) {
  if (!MIDI_OUT_ENABLED) return;
  midiPort = port;
  midiTimer.begin(midiTimerIsr, 200);
}

void midiSendNow(uint8_t status) {
  if (!midiPort) return;
  noInterrupts();                                  // (loop() and the audio interrupt both send)
  if (msgHead - msgTail < MSG_QUEUE && (!usbOut || msgHead - usbMsgTail < MSG_QUEUE)) { msgQueue[msgHead % MSG_QUEUE] = status; msgHead++; }
  interrupts();
}

void midiSendPosition(int32_t beats) {
  uint32_t pos = (uint32_t)max(beats, 0) * 4 & 0x3FFF;   // (in 16ths)
  midiSendNow(0xF2);
  midiSendNow(pos & 0x7F);
  midiSendNow(pos >> 7);
}

static void queueTick(uint32_t due) {
  if (!midiPort) return;
  if (tickHead - tickTail < TICK_QUEUE && (!usbOut || tickHead - usbTickTail < TICK_QUEUE)) {
    tickDue[tickHead % TICK_QUEUE] = due;
    tickHead++;
  }
}

void serviceUsbLatency() {
  // The last 8 beats' first sharp sounds, each within the first third of its
  // beat: agreeing within USB_AGREE samples, their middle one is USB's latency.
  static constexpr uint32_t USB_AGREE = 88;        // ~2 ms
  static uint32_t seen = 0, last[8];
  static int count = 0;
  static int32_t lastBeat = INT32_MIN;
  noInterrupts();
  uint32_t head = usbOnsetHead;
  interrupts();
  if (head - seen > USB_ONSETS) seen = head - USB_ONSETS;
  for (; seen < head; seen++) {
    UsbOnset o;
    noInterrupts();
    o.beat = usbOnsets[seen % USB_ONSETS].beat;
    o.inBeat = usbOnsets[seen % USB_ONSETS].inBeat;
    o.spb = usbOnsets[seen % USB_ONSETS].spb;
    interrupts();
    if (o.inBeat > o.spb / 3) { count = 0; continue; }   // (not near a beat: offbeat content)
    if (o.beat != lastBeat + 1) count = 0;            // (beats in a row)
    lastBeat = o.beat;
    for (int k = 7; k > 0; k--) last[k] = last[k - 1];
    last[0] = o.inBeat;
    if (count < 8) count++;
    if (count < 8) continue;
    uint32_t s[8];
    memcpy(s, last, sizeof s);
    for (int a = 0; a < 8; a++) for (int b = a + 1; b < 8; b++) if (s[b] < s[a]) { uint32_t x = s[a]; s[a] = s[b]; s[b] = x; }
    if (s[7] - s[0] > USB_AGREE) continue;
    uint32_t mid = (s[3] + s[4]) / 2;
    if (abs((int32_t)mid - (int32_t)usbInputLatency) > 22) {   // (half a ms or more: news)
      usbInputLatency = mid;
      Serial.printf("USB input latency: %lu samples (%.1f ms)\n", (unsigned long)mid, mid * 1000.0f / AUDIO_SAMPLE_RATE_EXACT);
    }
  }
}

void midiOutAttachUsb() {
  usbTickTail = tickHead;
  usbMsgTail = msgHead;
  usbOut = true;
}

int midiOutTakeUsb(uint32_t now, uint8_t *out, int max) {
  int n = 0;
  while (n < max && usbMsgTail != msgHead) out[n++] = msgQueue[usbMsgTail++ % MSG_QUEUE];
  while (n < max && usbTickTail != tickHead && (int32_t)(now - tickDue[usbTickTail % TICK_QUEUE]) >= 0) {
    out[n++] = 0xF8;
    usbTickTail++;
  }
  return n;
}

// ---- Tempo marks while recording (engine.h)
void beginTempoMarks(uint32_t sample, int32_t beat) {
  recording.marks[0] = {sample, (double)beat, transport.spb};
  recording.markCount = 1;
}

// The tempo just changed at transport sample `at`: a mark, while recording.
static void markTempo(uint32_t at) {
  if ((recording.mode != Recording::Running && recording.mode != Recording::Finishing) ||
      recording.markCount < 1 || recording.markCount >= Recording::TEMPO_MARKS)
    return;
  int n = recording.markCount;
  recording.marks[n] = {at, transport.beat + (double)transport.sampleInBeat / transport.spb, transport.spb};
  recording.markCount = n + 1;
}

// ---- Engine
// A track's stream has reached its sample switch: its clock mapping follows.
static void takeSwitch(Track &tr) {
  silencedTracks &= ~(1 << (&tr - tracks));      // a new clip here: heard again
  tr.s0 = tr.nextS0;
  tr.startBeat = tr.nextStartBeat;
  tr.playSpb = tr.nextSpb;
  tr.sampleSwitch = false;
  if (!tr.playSpb) {                             // a switch to nothing: the track stops there
    tr.live = false;
    memset(tr.curMix, 0, sizeof tr.curMix);
  }
}

// Per-sample step of the level glide (one-pole, GAIN_GLIDE_MS).
static const float GLIDE = 1.0f - expf(-1000.0f / (GAIN_GLIDE_MS * AUDIO_SAMPLE_RATE_EXACT));
static const float MASTER_GLIDE = 1.0f - expf(-1000.0f / (MASTER_GLIDE_MS * AUDIO_SAMPLE_RATE_EXACT));
static const float FADE = 1.0f - expf(-1000.0f / (FILTER_FADE_MS * AUDIO_SAMPLE_RATE_EXACT));
static const float CUTOFF_GLIDE = 1.0f - expf(-1000.0f * AUDIO_BLOCK_SAMPLES / (FILTER_GLIDE_MS * AUDIO_SAMPLE_RATE_EXACT));   // (a block's)

// Each block: the filters' cutoffs a step nearer their settings (on a log
// scale), their coefficients recalculated while they move.
static void glideFilters(Track &tr) {
  auto glide = [](float &hz, float target, Biquad &q, bool highpass) {
    if (hz == target || hz <= 0) return;
    float r = target / hz;
    hz = fabsf(r - 1) < 0.001f ? target : hz * powf(r, CUTOFF_GLIDE);
    q = butterworth(hz, highpass);
  };
  glide(tr.hpHz, tr.hpTarget, tr.hp, true);
  glide(tr.lpHz, tr.lpTarget, tr.lp, false);
}

void LooperEngine::onBeat(uint32_t blockMicros, int sampleIndex) {
  (void)blockMicros; (void)sampleIndex;
  int32_t beat = transport.beat;
  tickInBeat_ = 0;
  nextTickAt_ = 0;
  // The click, but on auto (only in a project with no clips) not from where a
  // finishing recording ends: its clip plays from there (loop() turns the
  // click off a little later, once the recording is completely done). On
  // (metronomeSteady): it goes on.
  bool recorded = !metronomeSteady && recording.mode == Recording::Finishing &&
                  beat >= recording.startBeat + (int32_t)(recording.loopBars * BEATS_PER_BAR);
  if (metronomeOn && !recorded && beat >= clickFromBeat) startClick(beat % BEATS_PER_BAR == 0);   // (a switch waiting: not until it)
  if (beat == transport.midiRestartBeat) {           // the clock back to 1.1.1 here (a project's switch): MIDI followers too,
    transport.midiRestartBeat = INT32_MIN;           // before this beat's first tick
    midiSendNow(0xFC);
    midiSendPosition(0);
    midiSendNow(0xFB);
  }

  if (beat % BEATS_PER_BAR == 0) {
    transport.lastBarSample = transport.now;
    transport.lastBarBeat = beat;
    if (recording.mode == Recording::WaitBar) {
      recording.startSample = transport.now + recordingLatency();
      recording.startBeat = beat;
      beginTempoMarks(transport.now, beat);
      recording.mode = Recording::Running;
    }
  }

  for (auto &tr : tracks) {
    if (!tr.live && !(tr.armed && beat >= tr.startBeat)) continue;
    if (tr.sampleSwitch && beat >= tr.nextStartBeat) takeSwitch(tr);
    if (!tr.playSpb) continue;
    // Where the stream should be at this beat; snapping to it every beat keeps
    // varispeed playback locked to the clock (no drift, tempo changes included).
    uint32_t expected = tr.s0 + (uint32_t)(beat - tr.startBeat) * tr.playSpb;
    int32_t ahead = (int32_t)(tr.head - expected);
    if (ahead >= 2 && ahead <= (int32_t)RING_FRAMES) {
      tr.tail = expected;
      tr.frac = 0;
      if (!tr.live) {                              // joining: glides in from silence; stretching, its grains from here
        memset(tr.curMix, 0, sizeof tr.curMix);    // (playing already: they go on through the playhead's snap)
        tr.grainsOn = false;
      }
      tr.live = true;
      tr.armed = false;
    } else if (!tr.live) {
      tr.armed = false;                          // missed its start: loop() refills
      tr.needsRestart = true;
    }
  }
}

void LooperEngine::update() {
#if defined(ARM_DWT_CYCCNT)
  const uint32_t cyclesAt = ARM_DWT_CYCCNT;
#endif
  if (stretchWindow[STRETCH_HOP] == 0)             // (once: the window's first half)
    for (int n = 0; n <= STRETCH_HOP; n++) stretchWindow[n] = 0.5f - 0.5f * cosf(2 * 3.14159265f * n / STRETCH_GRAIN);
  audio_block_t *inL = receiveReadOnly(0), *inR = receiveReadOnly(1);
  audio_block_t *outL = allocate(), *outR = allocate();
  if (!outL || !outR) {
    if (outL) release(outL);
    if (outR) release(outR);
    if (inL) release(inL);
    if (inR) release(inR);
    return;
  }
  static uint32_t usbFrames[AUDIO_BLOCK_SAMPLES];    // from the computer (left | right << 16)
  const bool usbIn = usbAudioRead && usbAudioRead(usbFrames);
  int16_t *usbOut = usbAudioWrite ? usbAudioWrite() : nullptr;   // to it: the mix and the tracks (USB_OUT_CHANNELS a frame)
  // The line input's gate (only while USB audio comes in): its peak this block.
  int16_t linePeak = 0;
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    int16_t a = inL ? abs(inL->data[i]) : 0, b = inR ? abs(inR->data[i]) : 0;
    linePeak = max(linePeak, max(a, b));
  }
  float lp = linePeak / 32768.0f;
  if (lp > lineInPeak) lineInPeak = lp;
  if (lp < lineInFloor) lineInFloor = lp;
  usbAudioIn = usbIn;
  const uint32_t holdBlocks = (uint32_t)(LINE_GATE_HOLD_MS * AUDIO_SAMPLE_RATE_EXACT / 1000 / AUDIO_BLOCK_SAMPLES);
  lineQuietBlocks_ = lp < LINE_GATE_LEVEL ? lineQuietBlocks_ + 1 : 0;
  const float lineFrom = lineGain_, lineTo = usbIn && lineQuietBlocks_ >= holdBlocks ? 0.0f : 1.0f;
  lineGain_ = lineTo;

  uint32_t blockMicros = micros();
  const float usPerSample = 1e6f / AUDIO_SAMPLE_RATE_EXACT;

  // Tempo change: keep the position within the beat proportionally.
  if (transport.pendingSpb && (transport.spbAtBeat < 0 || !transport.running)) {
    transport.spbAtBeat = -1;
    uint32_t spb = transport.pendingSpb;
    transport.sampleInBeat = (uint64_t)transport.sampleInBeat * spb / transport.spb;
    transport.spb = spb;
    transport.pendingSpb = 0;
    nextTickAt_ = (uint64_t)tickInBeat_ * spb / 24;
    markTempo(transport.now);
  }
  if (transport.joinMidBeat && transport.running) {  // started part way into this beat: the tracks join there
    transport.joinMidBeat = false;
    uint32_t in = transport.sampleInBeat, spbNow = transport.spb;
    for (auto &tr : tracks) {
      if (tr.live || !tr.armed || tr.startBeat != transport.beat || !tr.playSpb) continue;
      uint32_t at = tr.s0 + (uint32_t)((uint64_t)in * tr.playSpb / spbNow);
      int32_t ahead = (int32_t)(tr.head - at);
      if (ahead >= 2 && ahead <= (int32_t)RING_FRAMES) {
        tr.tail = at;
        tr.frac = 0;
        memset(tr.curMix, 0, sizeof tr.curMix);    // (glides in from silence)
        tr.live = true;
        tr.grainsOn = false;
        tr.armed = false;
      }
    }
    volatile BeatStart &b = beatLog[(uint32_t)transport.beat % BEAT_LOG];   // (the beat as if started a whole one: audio follow)
    b.beat = transport.beat;
    b.now = transport.now - in;
    b.spb = spbNow;
    tickInBeat_ = (int)min((uint64_t)24, ((uint64_t)in * 24 + spbNow - 1) / spbNow);   // the MIDI clock: from its next tick
    nextTickAt_ = (uint64_t)tickInBeat_ * spbNow / 24;
  }
  uint32_t spb = transport.spb;
  const bool running = transport.running;
  blockStamp.us = blockMicros;                       // (MIDI in: the clock at this block's start)
  blockStamp.beat = transport.beat;
  blockStamp.sampleInBeat = transport.sampleInBeat;
  blockStamp.spb = spb;
  blockStamp.running = running;

  double speed[NUM_TRACKS];
  float mix[NUM_TRACKS][4];                          // the tracks' own volume/balance mixes (metered)
  float peakL[NUM_TRACKS] = {}, peakR[NUM_TRACKS] = {};
  bool silencing = silencedTracks && (int32_t)(transport.now - silenceFrom) >= 0;   // (from this block on)
  for (int t = 0; t < NUM_TRACKS; t++) {
    const Track &tr = tracks[t];
    speed[t] = tr.playSpb ? (double)tr.playSpb / spb : 0;
    bool silenced = silencing && (silencedTracks & (1 << t));
    float g = tr.state == TrackState::Playing && !tr.stopping && !silenced ? tr.gain : 0;
    mix[t][0] = g * tr.mixLL;
    mix[t][1] = g * tr.mixLR;
    mix[t][2] = g * tr.mixRL;
    mix[t][3] = g * tr.mixRR;
  }

  if (clickAsked) {                                // (a click asked for by hand: a tap)
    startClick(clickAsked == 2);
    clickAsked = 0;
  }
  const bool listen = onsetListen;
  const uint32_t blockNow = transport.now;
  // Resampling: the output goes in the input history (what's recorded) instead.
  const bool captureOutput = recording.resample && recording.mode != Recording::Idle;
  const float outTo[4] = {outLL, outLR, outRL, outRR};
  static float outNow[4] = {1, 0, 0, 1};           // (the master's mix as it glides, MASTER_GLIDE_MS)
  for (auto &tr : tracks) glideFilters(tr);
  float outPeak[2] = {0, 0};
  float blockPeak = 0;                             // (the click's reference: clickLevel())
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    // USB's latency: the first sharp sound after each beat line (well above the
    // input's average level), timed in the beat (loop() decides).
    if (usbIn && running) {
      int32_t a = max(abs((int16_t)(usbFrames[i] & 0xFFFF)), abs((int16_t)(usbFrames[i] >> 16)));
      if (transport.sampleInBeat == 0) usbOnsetArmed = true;
      if (usbOnsetArmed && a > 1500 && a > 8 * usbEnv) {
        usbOnsetArmed = false;
        volatile UsbOnset &o = usbOnsets[usbOnsetHead % USB_ONSETS];
        o.beat = transport.beat;
        o.inBeat = transport.sampleInBeat;
        o.spb = spb;
        usbOnsetHead++;
      }
      usbEnv += (a - usbEnv) * (1.0f / 2048);
    }
    // The input: the line input (gated, faded over the block) and USB audio, mixed.
    float g = lineFrom + (lineTo - lineFrom) * i / AUDIO_BLOCK_SAMPLES;
    int32_t li = (int32_t)((inL ? inL->data[i] : 0) * g) + (usbIn ? (int16_t)(usbFrames[i] & 0xFFFF) : 0);
    int32_t ri = (int32_t)((inR ? inR->data[i] : 0) * g) + (usbIn ? (int16_t)(usbFrames[i] >> 16) : 0);
    int16_t l = (int16_t)constrain(li, -32768, 32767), r = (int16_t)constrain(ri, -32768, 32767);
    const uint32_t at = transport.now;
    inputRing[at % INPUT_FRAMES] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
    if (listen) {                                    // audio follow: the bands' energies
      float m = 0.5f * (l + r);
      onLo1 += (m - onLo1) * ON_K_LO;
      onLo2 += (onLo1 - onLo2) * ON_K_LO;
      onHiLp += (m - onHiLp) * ON_K_HI;
      float hi = m - onHiLp;
      onE[0] += (onLo2 * onLo2 - onE[0]) * ON_K_E;
      onE[1] += (hi * hi - onE[1]) * ON_K_E;
      onE[2] += (m * m - onE[2]) * ON_K_E;
    }

    const float monL = inputMonitored ? l : 0, monR = inputMonitored ? r : 0;   // the input being monitored (app.h's setting)
    float mixL = 0, mixR = 0;                        // the tracks
    int16_t *usbFrame = usbOut ? usbOut + i * USB_OUT_CHANNELS : nullptr;
    if (usbFrame) memset(usbFrame + 2, 0, 2 * NUM_TRACKS * sizeof *usbFrame);   // (each track's: silent unless playing)
    if (running) {
      if (transport.sampleInBeat == 0) {
        if (transport.pendingSpb && transport.spbAtBeat >= 0 && transport.beat >= transport.spbAtBeat) {
          spb = transport.spb = transport.pendingSpb;   // a scene's tempo, on its bar line
          transport.pendingSpb = 0;
          transport.spbAtBeat = -1;
          markTempo(at);
          for (int t = 0; t < NUM_TRACKS; t++) speed[t] = tracks[t].playSpb ? (double)tracks[t].playSpb / spb : 0;
        }
        onBeat(blockMicros, i);
        volatile BeatStart &b = beatLog[(uint32_t)transport.beat % BEAT_LOG];
        b.beat = transport.beat;
        b.now = at;
        b.spb = spb;
      }
      if (tickInBeat_ < 24 && transport.sampleInBeat >= nextTickAt_) {
        queueTick(blockMicros + (uint32_t)((i + AUDIO_BLOCK_SAMPLES) * usPerSample));
        tickInBeat_++;
        nextTickAt_ = (uint64_t)tickInBeat_ * spb / 24;
      }

      for (int t = 0; t < NUM_TRACKS; t++) {
        Track &tr = tracks[t];
        if (!tr.live) continue;
        uint32_t tail = tr.tail;
        if (tr.sampleSwitch && (int32_t)(tail - tr.switchAt) >= 0) {
          takeSwitch(tr);
          if (!tr.live) continue;
          speed[t] = (double)tr.playSpb / spb;
        }
        if (tr.head - tail < 2) { tr.underruns++; continue; }
        float sl, sr;
        if (tr.stretch) {
          stretchSample(tr, tail, speed[t], sl, sr);
        } else {
          passTransients(tr, tail);
          uint32_t a = tr.ring[tail % RING_FRAMES], b = tr.ring[(tail + 1) % RING_FRAMES];
          float f = (float)tr.frac;
          float al = (int16_t)(a & 0xFFFF), ar = (int16_t)(a >> 16);
          float bl = (int16_t)(b & 0xFFFF), br = (int16_t)(b >> 16);
          sl = al + (bl - al) * f;
          sr = ar + (br - ar) * f;
        }
        // The filters, each faded in or out (mixed with the signal unfiltered).
        tr.hpWet += ((tr.hpOn ? 1.0f : 0.0f) - tr.hpWet) * FADE;
        if (!tr.hpOn && tr.hpWet < 1e-4f) tr.hpWet = 0;
        if (tr.hpWet > 0) {
          float fl = runBiquad(tr.hp, tr.filterZ[0][0], sl), fr = runBiquad(tr.hp, tr.filterZ[0][1], sr);
          sl += (fl - sl) * tr.hpWet;
          sr += (fr - sr) * tr.hpWet;
        }
        tr.lpWet += ((tr.lpOn ? 1.0f : 0.0f) - tr.lpWet) * FADE;
        if (!tr.lpOn && tr.lpWet < 1e-4f) tr.lpWet = 0;
        if (tr.lpWet > 0) {
          float fl = runBiquad(tr.lp, tr.filterZ[1][0], sl), fr = runBiquad(tr.lp, tr.filterZ[1][1], sr);
          sl += (fl - sl) * tr.lpWet;
          sr += (fr - sr) * tr.lpWet;
        }
        const float *m = mix[t];
        float *c = tr.curMix;                        // levels glide (no clicks on mute, volume, start, stop)
        for (int k = 0; k < 4; k++) c[k] += (m[k] - c[k]) * GLIDE;
        float ol = c[0] * sl + c[1] * sr, orr = c[2] * sl + c[3] * sr;
        sl = ol;
        sr = orr;
        peakL[t] = max(peakL[t], fabsf(sl));
        peakR[t] = max(peakR[t], fabsf(sr));
        sl *= masterGain;
        sr *= masterGain;
        mixL += sl;
        mixR += sr;
        if (usbFrame) {                              // to USB: the track as it is in the mix (the top row's first)
          int16_t *st = usbFrame + usbTrackChannel(t);
          st[0] = (int16_t)constrain(sl, -32767.0f, 32767.0f);
          st[1] = (int16_t)constrain(sr, -32767.0f, 32767.0f);
        }
        double fr = tr.frac + speed[t];
        while (fr >= 1.0) { fr -= 1.0; tail++; }
        tr.frac = fr;
        tr.tail = tail;
      }

      if (++transport.sampleInBeat >= spb) {
        transport.sampleInBeat = 0;
        transport.beat = transport.beat + 1;
      }
    }
    transport.now = transport.now + 1;

    if (captureOutput) {                             // (without the metronome) the tracks and the input as they are:
      int16_t cl = (int16_t)constrain(mixL + monL, -32767.0f, 32767.0f);   // the master volume is not in it
      int16_t cr = (int16_t)constrain(mixR + monR, -32767.0f, 32767.0f);
      inputRing[at % INPUT_FRAMES] = (uint16_t)cl | ((uint32_t)(uint16_t)cr << 16);
    }
    float click = clickSample();                     // (its level the mix's peak: the master volume in it already)
    {                                                // the master volume and balance
      for (int k = 0; k < 4; k++) outNow[k] += (outTo[k] - outNow[k]) * MASTER_GLIDE;
      float l = mixL + monL, r = mixR + monR;
      mixL = l * outNow[0] + r * outNow[1];
      mixR = l * outNow[2] + r * outNow[3];
    }
    if (usbFrame) {                                  // USB: the mix as the line out has it, without the metronome
      float uc = USB_OUT_CLICK ? click : 0.0f;
      usbFrame[0] = (int16_t)constrain(mixL + uc, -32767.0f, 32767.0f);
      usbFrame[1] = (int16_t)constrain(mixR + uc, -32767.0f, 32767.0f);
    }
    blockPeak = max(blockPeak, max(fabsf(mixL), fabsf(mixR)));   // (what's heard, the click left out: its level)
    mixL += click;
    mixR += click;
    outPeak[0] = max(outPeak[0], fabsf(mixL));
    outPeak[1] = max(outPeak[1], fabsf(mixR));
    outL->data[i] = (int16_t)constrain(mixL, -32767.0f, 32767.0f);
    outR->data[i] = (int16_t)constrain(mixR, -32767.0f, 32767.0f);
  }
  refPeak = max(blockPeak, refPeak * REF_FALL);     // (up at once, down 6 dB a second)

  if (listen) {                                      // audio follow: this block's rise in loudness
    float flux = 0;
    for (int b = 0; b < 3; b++) {
      float c = logf(1 + onE[b] * (1.0f / 1000));    // (log energy, from ~-60 dBFS)
      flux += max(0.0f, c - onPrev[b]);
      onPrev[b] = c;
    }
    onsetRing[onsetCount % ONSET_FRAMES] = (uint16_t)min(flux * ONSET_SCALE, 65535.0f);
    onsetNow = blockNow;
    onsetLevel = onE[2];
    onsetCount = onsetCount + 1;
  }
  for (int t = 0; t < NUM_TRACKS; t++) {
    Track &tr = tracks[t];
    if (tr.stopping && tr.live) {                    // faded out: off
      const float *c = tr.curMix;
      if (fabsf(c[0]) + fabsf(c[1]) + fabsf(c[2]) + fabsf(c[3]) < 1e-3f) {
        tr.live = false;
        memset(tr.curMix, 0, sizeof tr.curMix);
      }
    }
    if (peakL[t] > tr.peakL * 32768) tr.peakL = peakL[t] / 32768;
    if (peakR[t] > tr.peakR * 32768) tr.peakR = peakR[t] / 32768;
  }

  if (outPeak[0] > outPeakL * 32768) outPeakL = outPeak[0] / 32768;
  if (outPeak[1] > outPeakR * 32768) outPeakR = outPeak[1] / 32768;

  transmit(outL, 0);
  transmit(outR, 1);
  release(outL);
  release(outR);
  if (usbOut) usbAudioWritten();
  if (inL) release(inL);
  if (inR) release(inR);
#if defined(ARM_DWT_CYCCNT)
  uint32_t used = ARM_DWT_CYCCNT - cyclesAt;
  loadCycles = loadCycles + used;
  loadBlocks = loadBlocks + 1;
  if (used > loadMost) loadMost = used;
#endif
}

}  // namespace looper
