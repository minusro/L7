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

// ---- Volume and balance
float volumeGain(int volume) {
  return volume <= 0 ? 0.0f : powf(10.0f, (volume - VOLUME_UNITY) * VOLUME_STEP_DB / 20);
}

void setTrackMix(int t, int volume, int balance) {
  Track &tr = tracks[t];
  volume = constrain(volume, 0, VOLUME_MAX);
  balance = constrain(balance, -BALANCE_MAX, BALANCE_MAX);
  float g = volumeGain(volume);
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

static int masterStep = MASTER_VOLUME_UNITY;
static volatile float outputGain = 1.0f;          // the master volume's
volatile bool metronomeOn = false;

// The metronome's wood block: two damped sines (its body and a harder upper
// partial), started on a beat.
static constexpr float CLICK_LEVEL = 5000, CLICK_BAR_HZ = 1600, CLICK_BEAT_HZ = 1150;
static const float CLICK_DECAY = expf(-1.0f / (0.012f * AUDIO_SAMPLE_RATE_EXACT));      // body, ~12 ms
static const float CLICK_DECAY_HI = expf(-1.0f / (0.004f * AUDIO_SAMPLE_RATE_EXACT));   // partial, ~4 ms
static float clickAmp = 0, clickAmpHi = 0, clickPhase = 0, clickStep = 0;

static void startClick(bool bar) {
  float hz = bar ? CLICK_BAR_HZ : CLICK_BEAT_HZ;
  clickStep = 2 * (float)M_PI * hz / AUDIO_SAMPLE_RATE_EXACT;
  clickPhase = 0;
  clickAmp = CLICK_LEVEL * (bar ? 1.0f : 0.75f);
  clickAmpHi = clickAmp * 0.5f;
}

static float clickSample() {
  if (clickAmp < 1) return 0;
  float s = clickAmp * sinf(clickPhase) + clickAmpHi * sinf(clickPhase * 2.76f);
  clickPhase += clickStep;                       // (from 0 on each click: it's over long before precision matters)
  clickAmp *= CLICK_DECAY;
  clickAmpHi *= CLICK_DECAY_HI;
  return s;
}
static volatile float outPeakL = 0, outPeakR = 0;
void setMasterVolume(int step) {
  masterStep = constrain(step, 0, MASTER_VOLUME_MAX);
  outputGain = masterStep ? powf(10.0f, (masterStep - MASTER_VOLUME_UNITY) * MASTER_STEP_DB / 20) : 0.0f;
}
int masterVolume() { return masterStep; }
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

void LooperEngine::onBeat(uint32_t blockMicros, int sampleIndex) {
  (void)blockMicros; (void)sampleIndex;
  int32_t beat = transport.beat;
  tickInBeat_ = 0;
  nextTickAt_ = 0;
  if (metronomeOn) startClick(beat % BEATS_PER_BAR == 0);

  if (beat % BEATS_PER_BAR == 0) {
    transport.lastBarSample = transport.now;
    transport.lastBarBeat = beat;
    if (recording.mode == Recording::WaitBar) {
      recording.startSample = transport.now + recordingLatency();
      recording.startBeat = beat;
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
      if (!tr.live) memset(tr.curMix, 0, sizeof tr.curMix);   // joining: glides in from silence
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
  if (transport.pendingSpb && (transport.spbAtBeat < 0 || !transport.running)) {
    transport.spbAtBeat = -1;
    uint32_t spb = transport.pendingSpb;
    transport.sampleInBeat = (uint64_t)transport.sampleInBeat * spb / transport.spb;
    transport.spb = spb;
    transport.pendingSpb = 0;
    nextTickAt_ = (uint64_t)tickInBeat_ * spb / 24;
  }
  uint32_t spb = transport.spb;
  const bool running = transport.running;

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

  // Resampling: the output goes in the input history (what's recorded) instead.
  const bool captureOutput = recording.resample && recording.mode != Recording::Idle;
  const float outGain = outputGain;
  float outPeak[2] = {0, 0};
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    int16_t l = inL ? inL->data[i] : 0, r = inR ? inR->data[i] : 0;
    const uint32_t at = transport.now;
    inputRing[at % INPUT_FRAMES] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);

    float mixL = monitor ? l : 0, mixR = monitor ? r : 0;
    if (running) {
      if (transport.sampleInBeat == 0) {
        if (transport.pendingSpb && transport.spbAtBeat >= 0 && transport.beat >= transport.spbAtBeat) {
          spb = transport.spb = transport.pendingSpb;   // a scene's tempo, on its bar line
          transport.pendingSpb = 0;
          transport.spbAtBeat = -1;
          for (int t = 0; t < NUM_TRACKS; t++) speed[t] = tracks[t].playSpb ? (double)tracks[t].playSpb / spb : 0;
        }
        onBeat(blockMicros, i);
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
        uint32_t a = tr.ring[tail % RING_FRAMES], b = tr.ring[(tail + 1) % RING_FRAMES];
        float f = (float)tr.frac;
        float al = (int16_t)(a & 0xFFFF), ar = (int16_t)(a >> 16);
        float bl = (int16_t)(b & 0xFFFF), br = (int16_t)(b >> 16);
        float sl = al + (bl - al) * f, sr = ar + (br - ar) * f;
        if (tr.hpOn) { sl = runBiquad(tr.hp, tr.filterZ[0][0], sl); sr = runBiquad(tr.hp, tr.filterZ[0][1], sr); }
        if (tr.lpOn) { sl = runBiquad(tr.lp, tr.filterZ[1][0], sl); sr = runBiquad(tr.lp, tr.filterZ[1][1], sr); }
        const float *m = mix[t];
        float *c = tr.curMix;                        // levels glide (no clicks on mute, volume, start, stop)
        for (int k = 0; k < 4; k++) c[k] += (m[k] - c[k]) * GLIDE;
        float ol = c[0] * sl + c[1] * sr, orr = c[2] * sl + c[3] * sr;
        sl = ol;
        sr = orr;
        peakL[t] = max(peakL[t], fabsf(sl));
        peakR[t] = max(peakR[t], fabsf(sr));
        mixL += sl * masterGain;
        mixR += sr * masterGain;
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

    mixL *= outGain;                                 // the master volume
    mixR *= outGain;
    if (captureOutput) {                             // (without the metronome)
      int16_t cl = (int16_t)constrain(mixL, -32767.0f, 32767.0f), cr = (int16_t)constrain(mixR, -32767.0f, 32767.0f);
      inputRing[at % INPUT_FRAMES] = (uint16_t)cl | ((uint32_t)(uint16_t)cr << 16);
    }
    float click = clickSample() * outGain;
    mixL += click;
    mixR += click;
    outPeak[0] = max(outPeak[0], fabsf(mixL));
    outPeak[1] = max(outPeak[1], fabsf(mixR));
    outL->data[i] = (int16_t)constrain(mixL, -32767.0f, 32767.0f);
    outR->data[i] = (int16_t)constrain(mixR, -32767.0f, 32767.0f);
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
  if (inL) release(inL);
  if (inR) release(inR);
}

}  // namespace looper
