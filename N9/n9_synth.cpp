// n9_synth.cpp — see n9_synth.h.
#include "n9_synth.h"

#include "M8Board.h"
#include "n9_state.h"

// (synth itself is defined in N9.ino, before the AudioConnections that take
// it: across files the construction order would be undefined, and a
// connection built on a not-yet-constructed stream corrupts the update list
// — a hard fault in the first audio interrupt.)

const char *const ENGINE_NAMES[ENGINE_COUNT] = {"SAW", "PULSE", "FM2", "PLUCK"};

// Per-engine fixed settings (ms; the real app gets these from the INST cells).
struct EngineDef {
  float atkMs, relMs;
  bool fm;         // two-operator FM (else saw/pulse through the lowpass)
  bool pluck;      // no sustain: the note decays on its own
  float decMs;     // the pluck's decay
  float ratio, index0, idxDecMs;   // FM: modulator ratio, index and its decay
};
static const EngineDef ENGINES[ENGINE_COUNT] = {
    {2, 150, false, false, 0, 0, 0, 0},            // SAW
    {2, 150, false, false, 0, 0, 0, 0},            // PULSE
    {1, 200, true, false, 0, 2.0f, 2.5f, 300},     // FM2
    {0.5f, 60, true, true, 500, 1.0f, 4.0f, 80},   // PLUCK
};

// Note/velocity events cross from loop() to the audio interrupt through this
// little ring; all voice state is touched by update() only.
struct Ev { uint8_t on, note, vel; };
static Ev evq[32];
static volatile uint8_t evw = 0, evr = 0;

void N9Synth::noteOn(uint8_t note, uint8_t vel) {
  uint8_t next = (evw + 1) & 31;
  if (next == evr) return;                 // full: drop (prototype)
  evq[evw] = {1, note, vel};
  evw = next;
}

void N9Synth::noteOff(uint8_t note) {
  uint8_t next = (evw + 1) & 31;
  if (next == evr) return;
  evq[evw] = {0, note, 0};
  evw = next;
}

static float SINE[257];
static bool sineReady = false;

static inline float sinT(float ph) {
  ph -= floorf(ph);
  float f = ph * 256.0f;
  int i = (int)f;
  float fr = f - i;
  return SINE[i] + (SINE[i + 1] - SINE[i]) * fr;
}

// decay multiplier per sample for a time constant in ms
static inline float decay(float ms) { return expf(-1.0f / (ms * 44.1f)); }

void N9Synth::update() {
  uint32_t t0 = ARM_DWT_CYCCNT;
  if (!sineReady) {
    for (int i = 0; i <= 256; i++) SINE[i] = sinf(i * (2.0f * (float)M_PI / 256.0f));
    sineReady = true;
  }

  // Take the notes that arrived.
  while (evr != evw) {
    Ev e = evq[evr];
    evr = (evr + 1) & 31;
    if (e.on) {
      Voice *v = nullptr;
      for (auto &c : _v) if (c.state == IDLE) { v = &c; break; }
      if (!v) {                                    // steal: the quietest releasing, else the oldest
        float quiet = 2.0f;
        for (auto &c : _v) if (c.state == RELEASE && c.env < quiet) { quiet = c.env; v = &c; }
        if (!v) { uint32_t old = UINT32_MAX; for (auto &c : _v) if (c.age < old) { old = c.age; v = &c; } }
      }
      const EngineDef &d = ENGINES[nav.engine];
      float hz = 440.0f * exp2f((e.note - 69) / 12.0f);
      v->note = e.note; v->engine = nav.engine;
      v->inc = hz / 44100.0f;
      v->minc = v->inc * d.ratio;
      v->phase = v->mphase = 0;
      v->mindex = d.index0;
      v->env = 0; v->lp = 0;
      v->vel = e.vel / 127.0f;
      v->age = ++_ageCounter;
      v->state = ATTACK;
    } else {
      for (auto &c : _v)
        if (c.state != IDLE && c.state != RELEASE && c.note == e.note) c.state = RELEASE;
    }
  }

  audio_block_t *l = allocate();
  audio_block_t *r = allocate();
  if (!l || !r) { if (l) release(l); if (r) release(r); return; }

  static float mix[AUDIO_BLOCK_SAMPLES];
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) mix[i] = 0;

  uint8_t active = 0;
  for (auto &v : _v) {
    if (v.state == IDLE) continue;
    active++;
    const EngineDef &d = ENGINES[v.engine];
    float atkC = 1.0f - decay(d.atkMs < 0.1f ? 0.1f : d.atkMs);
    float decC = d.pluck ? decay(d.decMs) : 1.0f;
    float relC = decay(d.relMs);
    float idxC = d.fm ? decay(d.idxDecMs) : 1.0f;
    float amp = v.vel * 0.12f;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      switch (v.state) {
        case ATTACK:  v.env += atkC * (1.0f - v.env); if (v.env > 0.999f) v.state = HOLD; break;
        case HOLD:    v.env *= decC; break;
        default:      v.env *= relC; break;    // RELEASE
      }
      float s;
      if (d.fm) {
        float mod = sinT(v.mphase) * v.mindex;
        v.mindex *= idxC;
        s = sinT(v.phase + mod * 0.15915f);    // index in radians -> cycles
        v.mphase += v.minc; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
      } else {
        s = v.engine == 0 ? 2.0f * v.phase - 1.0f : (v.phase < 0.5f ? 1.0f : -1.0f);
        v.lp += 0.5f * (s - v.lp);             // take the top edge off the naive shapes
        s = v.lp;
      }
      v.phase += v.inc; if (v.phase >= 1.0f) v.phase -= 1.0f;
      mix[i] += s * v.env * amp;
    }
    if (v.env < 0.0005f && v.state != ATTACK) { v.state = IDLE; active--; }
  }
  _active = active;

  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    float s = mix[i];
    if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
    l->data[i] = r->data[i] = (int16_t)(s * 32767.0f);
  }
  transmit(l, 0); transmit(r, 1);
  release(l); release(r);

  uint32_t dt = ARM_DWT_CYCCNT - t0;
  if (dt > _maxCycles) _maxCycles = dt;
}

// ---- MIDI in (channel 1): USB and the serial in port ------------------------

static HardwareSerial *midiSerial = nullptr;

void synthMidiBegin() { midiSerial = board.midiInBegin(); }

void synthServiceMidi() {
  while (usbMIDI.read()) {
    if (usbMIDI.getChannel() != 1) continue;
    uint8_t type = usbMIDI.getType();
    if (type == usbMIDI.NoteOn && usbMIDI.getData2() > 0) synth.noteOn(usbMIDI.getData1(), usbMIDI.getData2());
    else if (type == usbMIDI.NoteOff || type == usbMIDI.NoteOn) synth.noteOff(usbMIDI.getData1());
  }
  if (!midiSerial) return;
  static uint8_t status = 0, d1 = 255;
  while (midiSerial->available()) {
    uint8_t b = midiSerial->read();
    if (b >= 0xF8) continue;                      // realtime
    if (b & 0x80) { status = b; d1 = 255; continue; }
    if (status == 0x90 || status == 0x80) {       // channel 1 only
      if (d1 == 255) { d1 = b; continue; }
      if (status == 0x90 && b > 0) synth.noteOn(d1, b);
      else synth.noteOff(d1);
      d1 = 255;                                   // (running status kept)
    }
  }
}
