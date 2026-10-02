// engine.cpp — see engine.h
#include "engine.h"

namespace looper {

// 8 x 64 KB: seven fit in RAM2 next to the other DMAMEM buffers, the eighth
// goes in RAM1.
DMAMEM static uint32_t rings[NUM_TRACKS - 1][RING_FRAMES];
static uint32_t lastRing[RING_FRAMES];
uint32_t inputRing[INPUT_FRAMES];

Track tracks[NUM_TRACKS];
Transport transport;
Recording recording;

static struct RingInit {
  RingInit() {
    for (int t = 0; t < NUM_TRACKS - 1; t++) tracks[t].ring = rings[t];
    tracks[NUM_TRACKS - 1].ring = lastRing;
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

void setTrackFilter(int t, int lo, int hi) {
  Track &tr = tracks[t];
  if (hi < lo) hi = lo;
  if (hi - lo > FILTER_MAX) hi = lo + FILTER_MAX;
  if (hi < 0) { lo -= hi; hi = 0; }
  if (lo > FILTER_MAX) { hi -= lo - FILTER_MAX; lo = FILTER_MAX; }
  Biquad hp = butterworth(filterStepHz(constrain(lo, 1, FILTER_MAX)), true);
  Biquad lp = butterworth(filterStepHz(constrain(hi, 0, FILTER_MAX - 1)), false);
  noInterrupts();
  tr.filterLo = lo;
  tr.filterHi = hi;
  bool wasHp = tr.hpOn, wasLp = tr.lpOn;
  tr.hp = hp;
  tr.lp = lp;
  tr.hpOn = lo > 0;
  tr.lpOn = hi < FILTER_MAX;
  if (tr.hpOn && !wasHp) memset(tr.filterZ[0], 0, sizeof tr.filterZ[0]);   // start clean
  if (tr.lpOn && !wasLp) memset(tr.filterZ[1], 0, sizeof tr.filterZ[1]);
  interrupts();
}

float filterResponseDb(int lo, int hi, float hz) {
  float db = 0;
  if (lo > 0) db -= 10 * log10f(1 + powf(filterStepHz(lo) / hz, 4));
  if (hi < FILTER_MAX) db -= 10 * log10f(1 + powf(hz / filterStepHz(hi), 4));
  return db;
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
static volatile uint8_t msgQueue[8];
static volatile uint32_t msgHead = 0, msgTail = 0;       // loop() / timer

static void midiTimerIsr() {
  while (msgTail != msgHead) midiPort->write(msgQueue[msgTail++ % 8]);
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
  if (msgHead - msgTail < 8) { msgQueue[msgHead % 8] = status; msgHead++; }
}

static void queueTick(uint32_t due) {
  if (!midiPort) return;
  if (tickHead - tickTail < TICK_QUEUE) { tickDue[tickHead % TICK_QUEUE] = due; tickHead++; }
}

// ---- Engine
void LooperEngine::onBeat(uint32_t blockMicros, int sampleIndex) {
  (void)blockMicros; (void)sampleIndex;
  int32_t beat = transport.beat;
  tickInBeat_ = 0;
  nextTickAt_ = 0;

  if (beat % BEATS_PER_BAR == 0) {
    transport.lastBarSample = transport.now;
    transport.lastBarBeat = beat;
    if (recording.mode == Recording::WaitBar) {
      recording.startSample = transport.now + LATENCY_FRAMES;
      recording.startBeat = beat;
      recording.mode = Recording::Running;
    }
  }

  for (auto &tr : tracks) {
    if (!tr.live && !(tr.armed && beat >= tr.startBeat)) continue;
    // Where the stream should be at this beat; snapping to it every beat keeps
    // varispeed playback locked to the clock (no drift, tempo changes included).
    uint32_t expected = tr.s0 + (uint32_t)(beat - tr.startBeat) * tr.spbNative;
    int32_t ahead = (int32_t)(tr.head - expected);
    if (ahead >= 2 && ahead <= (int32_t)RING_FRAMES) {
      tr.tail = expected;
      tr.frac = 0;
      tr.live = true;
      tr.armed = false;
    } else if (!tr.live) {
      tr.armed = false;                          // missed its start: loop() refills
      tr.needsRestart = true;
    }
  }
}

void LooperEngine::update() {
  audio_block_t *inL = receiveReadOnly(0), *inR = receiveReadOnly(1);
  audio_block_t *outL = allocate(), *outR = allocate();
  if (!outL || !outR) {
    if (outL) release(outL);
    if (outR) release(outR);
    if (inL) release(inL);
    if (inR) release(inR);
    return;
  }

  uint32_t blockMicros = micros();
  const float usPerSample = 1e6f / AUDIO_SAMPLE_RATE_EXACT;

  // Tempo change: keep the position within the beat proportionally.
  if (transport.pendingSpb) {
    uint32_t spb = transport.pendingSpb;
    transport.sampleInBeat = (uint64_t)transport.sampleInBeat * spb / transport.spb;
    transport.spb = spb;
    transport.pendingSpb = 0;
    nextTickAt_ = (uint64_t)tickInBeat_ * spb / 24;
  }
  const uint32_t spb = transport.spb;
  const bool running = transport.running;

  double speed[NUM_TRACKS];
  float gain[NUM_TRACKS];
  for (int t = 0; t < NUM_TRACKS; t++) {
    speed[t] = tracks[t].spbNative ? (double)tracks[t].spbNative / spb : 0;
    gain[t] = tracks[t].state == TrackState::Playing ? tracks[t].gain * masterGain : 0;
  }

  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    int16_t l = inL ? inL->data[i] : 0, r = inR ? inR->data[i] : 0;
    inputRing[transport.now % INPUT_FRAMES] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);

    float mixL = monitor ? l : 0, mixR = monitor ? r : 0;
    if (running) {
      if (transport.sampleInBeat == 0) onBeat(blockMicros, i);
      if (tickInBeat_ < 24 && transport.sampleInBeat >= nextTickAt_) {
        queueTick(blockMicros + (uint32_t)((i + AUDIO_BLOCK_SAMPLES) * usPerSample));
        tickInBeat_++;
        nextTickAt_ = (uint64_t)tickInBeat_ * spb / 24;
      }

      for (int t = 0; t < NUM_TRACKS; t++) {
        Track &tr = tracks[t];
        if (!tr.live) continue;
        uint32_t tail = tr.tail;
        if (tr.head - tail < 2) { tr.underruns++; continue; }
        uint32_t a = tr.ring[tail % RING_FRAMES], b = tr.ring[(tail + 1) % RING_FRAMES];
        float f = (float)tr.frac;
        float al = (int16_t)(a & 0xFFFF), ar = (int16_t)(a >> 16);
        float bl = (int16_t)(b & 0xFFFF), br = (int16_t)(b >> 16);
        float sl = al + (bl - al) * f, sr = ar + (br - ar) * f;
        if (tr.hpOn) { sl = runBiquad(tr.hp, tr.filterZ[0][0], sl); sr = runBiquad(tr.hp, tr.filterZ[0][1], sr); }
        if (tr.lpOn) { sl = runBiquad(tr.lp, tr.filterZ[1][0], sl); sr = runBiquad(tr.lp, tr.filterZ[1][1], sr); }
        mixL += sl * gain[t];
        mixR += sr * gain[t];
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

    outL->data[i] = (int16_t)constrain(mixL, -32767.0f, 32767.0f);
    outR->data[i] = (int16_t)constrain(mixR, -32767.0f, 32767.0f);
  }

  transmit(outL, 0);
  transmit(outR, 1);
  release(outL);
  release(outR);
  if (inL) release(inL);
  if (inR) release(inR);
}

}  // namespace looper
