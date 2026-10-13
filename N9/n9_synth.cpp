// n9_synth.cpp — see n9_synth.h.
#include "n9_synth.h"

#include "M8Board.h"
#include "n9_state.h"

// (synth itself is defined in N9.ino, before the AudioConnections that take
// it: across files the construction order would be undefined, and a
// connection built on a not-yet-constructed stream corrupts the update list
// — a hard fault in the first audio interrupt.)

// The lineup (decided 2026-10-13): 8 generic engines plus the five
// emulations — TB-303, Juno-60 (with chorus I + II), Prophet-5, Waldorf
// Microwave, Korg M1. The emulations (and FM4/WT) are placeholder voices
// for now: the selector and voice pool are real, the sound isn't yet.
const char *const ENGINE_NAMES[ENGINE_COUNT] = {
    "SAW", "PULSE", "SUPER", "FM2", "FM4", "PLUCK", "WT", "NOISE",
    "303", "JUNO", "PRO5", "MWAVE", "M1"};

enum OscType : uint8_t {
  O_SAW,     // naive saw through the one-pole lowpass
  O_PULSE,   // naive square, same lowpass
  O_SUPER,   // three saws, detuned +-0.45%
  O_FM,      // two-operator FM
  O_NOISE,   // white noise through the lowpass
  O_303,     // mono acid voice: polyBLEP saw into a resonant 4-pole ladder,
             // the filter swept by its own decaying envelope; slide on
             // overlapping notes, accent on velocity >= 100
  O_PRO5,    // poly: two detuned polyBLEP saws into the ladder, mild
             // resonance, the filter easing to a sustain level (velocity
             // opens it: fenv starts from the velocity)
};

constexpr uint8_t E_303 = 8;   // its place in ENGINE_NAMES

// Per-engine fixed settings (ms; the real app gets these from the INST cells).
struct EngineDef {
  OscType osc;
  float atkMs, relMs;
  bool pluck;      // no sustain: the note decays on its own
  float decMs;     // the pluck's decay
  float ratio, index0, idxDecMs;   // FM: modulator ratio, index and its decay
};
static const EngineDef ENGINES[ENGINE_COUNT] = {
    {O_SAW, 2, 150, false, 0, 0, 0, 0},                // SAW
    {O_PULSE, 2, 150, false, 0, 0, 0, 0},              // PULSE
    {O_SUPER, 4, 200, false, 0, 0, 0, 0},              // SUPER
    {O_FM, 1, 200, false, 0, 2.0f, 2.5f, 300},         // FM2
    {O_FM, 1, 250, false, 0, 3.0f, 3.5f, 400},         // FM4    (placeholder: brighter 2-op)
    {O_FM, 0.5f, 60, true, 500, 1.0f, 4.0f, 80},       // PLUCK  (Karplus-Strong later)
    {O_FM, 2, 200, false, 0, 0.5f, 1.5f, 1000},        // WT     (placeholder)
    {O_NOISE, 1, 120, true, 250, 0, 0, 0},             // NOISE
    {O_303, 3, 12, true, 3500, 0, 0, 0},               // 303 (the VCA decays while gated, off fast)
    {O_SUPER, 3, 300, false, 0, 0, 0, 0},              // JUNO   (placeholder)
    {O_PRO5, 5, 350, false, 0, 1.004f, 0, 0},          // PRO5 (ratio = osc B's detune)
    {O_FM, 2, 220, false, 0, 1.5f, 2.0f, 600},         // MWAVE  (placeholder)
    {O_FM, 1, 300, false, 0, 3.5f, 2.0f, 500},         // M1     (placeholder)
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

// polyBLEP residual: subtracts the saw's step discontinuity alias
static inline float polyblep(float t, float dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}

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
      const EngineDef &d = ENGINES[nav.engine];
      float hz = 440.0f * exp2f((e.note - 69) / 12.0f);
      float inc = hz / 44100.0f;
      Voice *v = nullptr;
      if (nav.engine == E_303) {                   // mono: one 303 voice, reused
        for (auto &c : _v) if (c.state != IDLE && c.engine == E_303) { v = &c; break; }
        if (v && v->state != RELEASE) {            // legato: slide there, envelopes kept running
          v->tinc = inc;
          v->note = e.note;
          v->accent = e.vel >= 100;
          continue;
        }
      }
      if (!v) {
        for (auto &c : _v) if (c.state == IDLE) { v = &c; break; }
        if (!v) {                                  // steal: the quietest releasing, else the oldest
          float quiet = 2.0f;
          for (auto &c : _v) if (c.state == RELEASE && c.env < quiet) { quiet = c.env; v = &c; }
          if (!v) { uint32_t old = UINT32_MAX; for (auto &c : _v) if (c.age < old) { old = c.age; v = &c; } }
        }
      }
      v->note = e.note; v->engine = nav.engine;
      v->accent = e.vel >= 100;
      v->inc = v->tinc = inc;
      v->minc = d.osc == O_SUPER ? inc * 1.0045f : inc * d.ratio;
      v->inc3 = inc * 0.9955f;
      v->phase = v->mphase = v->p3 = 0;
      v->mindex = d.index0;
      v->f1 = v->f2 = v->f3 = v->f4 = 0;
      v->fenv = v->accent ? 1.25f : 0.85f;
      v->env = 0; v->lp = 0;
      v->vel = e.vel / 127.0f;
      if (nav.engine == E_303 && v->accent) v->vel = 1.2f;   // the accent's kick
      v->rng = _ageCounter * 2654435761u + e.note * 7919u + 1;
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
    float idxC = d.osc == O_FM ? decay(d.idxDecMs) : 1.0f;
    float fdecC = d.osc == O_303 ? decay(v.accent ? 130 : 300)           // the filter env's decay; accent: the "wow"
                : d.osc == O_PRO5 ? decay(600) : 1.0f;
    float amp = v.vel * 0.12f;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      switch (v.state) {
        case ATTACK:  v.env += atkC * (1.0f - v.env); if (v.env > 0.999f) v.state = HOLD; break;
        case HOLD:    v.env *= decC; break;
        default:      v.env *= relC; break;    // RELEASE
      }
      float s;
      switch (d.osc) {
        case O_FM: {
          float mod = sinT(v.mphase) * v.mindex;
          v.mindex *= idxC;
          s = sinT(v.phase + mod * 0.15915f);  // index in radians -> cycles
          v.mphase += v.minc; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
          break;
        }
        case O_SUPER:
          s = ((2.0f * v.phase - 1.0f) + (2.0f * v.mphase - 1.0f) + (2.0f * v.p3 - 1.0f)) * 0.333f;
          v.lp += 0.5f * (s - v.lp); s = v.lp;
          v.mphase += v.minc; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
          v.p3 += v.inc3; if (v.p3 >= 1.0f) v.p3 -= 1.0f;
          break;
        case O_NOISE:
          v.rng = v.rng * 1664525u + 1013904223u;
          s = (int32_t)v.rng * 4.6566e-10f;
          v.lp += 0.5f * (s - v.lp); s = v.lp;
          break;
        case O_303: {
          v.inc += (v.tinc - v.inc) * 0.0008f;             // slide, ~35 ms
          v.fenv *= fdecC;
          float saw = 2.0f * v.phase - 1.0f - polyblep(v.phase, v.inc);
          float fc = 0.006f + 0.11f * v.fenv;              // cutoff, cycles/sample: ~265 Hz up to ~5 kHz
          float w = 6.2832f * fc, g = w / (1.0f + w);
          float in = saw - 3.3f * v.f4;                    // resonance feedback
          if (in > 1.4f) in = 1.4f; else if (in < -1.4f) in = -1.4f;
          in -= in * in * in * (1.0f / 6.0f);              // the ladder's soft squash
          v.f1 += g * (in - v.f1); v.f2 += g * (v.f1 - v.f2);
          v.f3 += g * (v.f2 - v.f3); v.f4 += g * (v.f3 - v.f4);
          s = v.f4 * 2.4f;                                 // makeup for what the ladder takes
          break;
        }
        case O_PRO5: {
          v.fenv *= fdecC;
          float sawA = 2.0f * v.phase - 1.0f - polyblep(v.phase, v.inc);
          float sawB = 2.0f * v.mphase - 1.0f - polyblep(v.mphase, v.minc);
          float s0 = (sawA + sawB) * 0.5f;
          float sweep = 0.35f + 0.65f * v.fenv;            // eases to a sustain level, never closed
          float fc = 0.012f + 0.085f * sweep;
          float w = 6.2832f * fc, g = w / (1.0f + w);
          float in = s0 - 1.3f * v.f4;                     // mild resonance
          if (in > 1.4f) in = 1.4f; else if (in < -1.4f) in = -1.4f;
          in -= in * in * in * (1.0f / 6.0f);
          v.f1 += g * (in - v.f1); v.f2 += g * (v.f1 - v.f2);
          v.f3 += g * (v.f2 - v.f3); v.f4 += g * (v.f3 - v.f4);
          s = v.f4 * 1.6f;
          v.mphase += v.minc; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
          break;
        }
        default:                               // O_SAW, O_PULSE
          s = d.osc == O_SAW ? 2.0f * v.phase - 1.0f : (v.phase < 0.5f ? 1.0f : -1.0f);
          v.lp += 0.5f * (s - v.lp); s = v.lp;
          break;
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
