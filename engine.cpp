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
static constexpr uint32_t MSG_QUEUE = 16;
static volatile uint8_t msgQueue[MSG_QUEUE];
static volatile uint32_t msgHead = 0, msgTail = 0;       // loop() / timer
// A second reader, USB MIDI (midi_usb.cpp, its own timer): its own places in both.
static volatile bool usbOut = false;
static volatile uint32_t usbTickTail = 0, usbMsgTail = 0;

volatile BlockStamp blockStamp;
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

void LooperEngine::onBeat(uint32_t blockMicros, int sampleIndex) {
  (void)blockMicros; (void)sampleIndex;
  int32_t beat = transport.beat;
  tickInBeat_ = 0;
  nextTickAt_ = 0;
  // The click (an empty project's), but not from where a finishing recording
  // ends: its clip plays from there (loop() turns the click off a little later,
  // once the recording is completely done).
  bool recorded = recording.mode == Recording::Finishing &&
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
        tr.armed = false;
      }
    }
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

  // Resampling: the output goes in the input history (what's recorded) instead.
  const bool captureOutput = recording.resample && recording.mode != Recording::Idle;
  const float outGain = outputGain;
  float outPeak[2] = {0, 0};
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

    const float monL = monitor ? l : 0, monR = monitor ? r : 0;   // the input being monitored
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
        sl *= masterGain;
        sr *= masterGain;
        mixL += sl;
        mixR += sr;
        if (usbFrame) {                              // to USB: the track as it is in the mix
          usbFrame[2 + 2 * t] = (int16_t)constrain(sl, -32767.0f, 32767.0f);
          usbFrame[3 + 2 * t] = (int16_t)constrain(sr, -32767.0f, 32767.0f);
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
    float click = clickSample();
    if (usbFrame) {                                  // USB: the mix before the master volume (and without the metronome)
      float uc = USB_OUT_CLICK ? click : 0.0f;
      usbFrame[0] = (int16_t)constrain(mixL + monL + uc, -32767.0f, 32767.0f);
      usbFrame[1] = (int16_t)constrain(mixR + monR + uc, -32767.0f, 32767.0f);
    }
    click *= outGain;
    mixL = (mixL + monL) * outGain + click;          // the master volume
    mixR = (mixR + monR) * outGain + click;
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
  if (usbOut) usbAudioWritten();
  if (inL) release(inL);
  if (inR) release(inR);
}

}  // namespace looper
