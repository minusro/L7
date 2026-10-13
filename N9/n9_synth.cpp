// n9_synth.cpp — see n9_synth.h.
#include "n9_synth.h"

#include "M8Board.h"
#include "n9_m1_bank.h"
#include "n9_mw_bank.h"
#include "n9_fx.h"
#include "n9_state.h"

// The M1 engine's current program (0-99), set by MIDI program change.
static volatile uint8_t m1Prog = 0;
// And the Microwave engine's current sound (0-255).
static volatile uint8_t mwProg = 0;

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
  O_M1,      // the factory bank's parameters (n9_m1_bank.h: osc pair, VDA
             // EG segments, VDF sweep) over stand-in oscillators by
             // multisound category; MIDI program change picks the program
  O_MWAVE,   // the Microwave factory bank's parameters (n9_mw_bank.h,
             // offsets inferred from the data): VCA ADSR, resonant VCF with
             // its env sweep, and a morphing wave (sine-saw-square) swept
             // from the sound's start wave — the wavetable stand-in
};

// (E_303 / E_M1: n9_state.h)

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
    {O_MWAVE, 2, 220, false, 0, 0, 0, 0},              // MWAVE (envelopes come from the sound)
    {O_M1, 1, 300, false, 0, 0, 0, 0},                 // M1 (envelopes come from the program)
};

// M1 helpers: a time value 0-99 as milliseconds (roughly the M1's log
// scale), and as a per-sample easing coefficient.
static inline float m1ms(uint8_t v) { return 2.0f * expf(0.0816f * v); }
static inline float m1coef(uint8_t v) { return 1.0f - expf(-1.0f / (m1ms(v) * 44.1f)); }

// Microwave times (0-127), the scale a guess until a spec surfaces.
static inline float mwcoef(uint8_t v) {
  float ms = 4.0f * expf(0.0677f * v);
  if (ms > 8000.0f) ms = 8000.0f;
  return 1.0f - expf(-1.0f / (ms * 44.1f));
}

// Signed velocity sensitivity (-99..99) as a gain factor.
static inline float velF(int8_t sens, float velN) {
  float f = sens / 99.0f;
  return f >= 0 ? (1.0f - f) + f * velN : (1.0f + f) - f * (1.0f - velN);
}
// An MG's frequency value as Hz, and a block-rate easing coefficient.
static inline float mgHz(uint8_t v) { return 0.1f * expf(0.0537f * v); }
static inline float m1coefB(uint8_t t) { return 1.0f - expf(-2.902f / m1ms(t)); }
// MG waveforms: 0 triangle, 1 saw up, 2 saw down, 3 square.
static inline float mgShape(uint8_t wave, float ph) {
  switch (wave) {
    case 1: return 2.0f * ph - 1.0f;
    case 2: return 1.0f - 2.0f * ph;
    case 3: return ph < 0.5f ? 1.0f : -1.0f;
    default: return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
  }
}

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

static inline float sinT(float ph);

// An M1 multisound's stand-in, by category. FM categories take the voice's
// modulator (index 0 degrades them to a sine: the second oscillator).
static inline float m1oscS(uint8_t cat, float ph, float dt, float fmPhase, float fmIndex, uint32_t &rng) {
  switch (cat) {
    case M1C_SAW: case M1C_SUPER: return 2.0f * ph - 1.0f - polyblep(ph, dt);
    case M1C_PULSE: return ph < 0.5f ? 1.0f : -1.0f;
    case M1C_SINE: return sinT(ph);
    case M1C_ORGAN: return 0.55f * sinT(ph) + 0.33f * sinT(2.0f * ph) + 0.18f * sinT(4.0f * ph);
    case M1C_BELL: case M1C_EP: case M1C_PLUCK: return sinT(ph + 0.15915f * fmIndex * sinT(fmPhase));
    default: rng = rng * 1664525u + 1013904223u; return (int32_t)rng * 4.6566e-10f;
  }
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
      if (nav.engine == E_M1) {                  // cache the program into the voice
        const M1Program &pr = M1_BANK[m1Prog];
        float velN = e.vel / 127.0f;
        v->dual = pr.oscMode == 1;               // (drum programs play as single)
        v->cat1 = pr.ms1 < 100 ? M1_MS_CAT[pr.ms1] : M1C_SINE;
        v->cat2 = pr.ms2 < 100 ? M1_MS_CAT[pr.ms2] : M1C_SINE;
        v->inc = inc * exp2f(pr.oct1);
        v->minc = inc * exp2f(pr.oct2 + pr.interval / 12.0f + pr.detune / 1200.0f);
        v->eAc = m1coef(pr.aA); v->eDc = m1coef(pr.aD);              // VDA1 EG
        v->eSc = m1coef(pr.aSl); v->eRc = m1coef(pr.aR);
        v->lAtk = pr.aAl / 99.0f; v->lBreak = pr.aB / 99.0f; v->lSus = pr.aS / 99.0f;
        v->mseg = 0;
        v->e2Ac = m1coef(pr.bA); v->e2Dc = m1coef(pr.bD);            // VDA2 EG (osc2's own)
        v->e2Sc = m1coef(pr.bSl); v->e2Rc = m1coef(pr.bR);
        v->l2Atk = pr.bAl / 99.0f; v->l2Break = pr.bB / 99.0f; v->l2Sus = pr.bS / 99.0f;
        v->mseg2 = 0; v->env2 = 0;
        v->lvl2 = (pr.vda2Level / 99.0f) * velF(pr.amp2Vel, velN);
        float c = pr.vdfCutoff / 99.0f;                              // VDF + its EG
        v->cutB = 0.003f + 0.22f * c * c;
        v->cutE = 0.17f * (pr.vdfEgInt / 99.0f) * velF(pr.egIntVel, velN);
        v->fAc = m1coef(pr.fA); v->fDc = m1coef(pr.fD);
        v->fSc = m1coef(pr.fSl); v->fRc = m1coef(pr.fR);
        v->lfAl = pr.fAl / 99.0f; v->lfB = pr.fB / 99.0f;
        v->lfS = pr.fS / 99.0f; v->lfRl = pr.fRl / 99.0f;
        v->fenv = 0; v->fseg = 0;
        v->kbdCut = ((int)e.note - pr.kbdCenter) / 12.0f * (pr.cutKbd / 99.0f) * 0.05f;
        v->penv = pr.pStartL / 99.0f;                                // pitch EG (block rate)
        v->pAtkL = pr.pAtkL / 99.0f; v->pRelL = pr.pRelL / 99.0f;
        v->pAc = m1coefB(pr.pAtkT); v->pDc = m1coefB(pr.pDecT); v->pRc = m1coefB(pr.pRelT);
        v->pseg = 0;
        v->mgWave = pr.mgWave & 3; v->fmgWave = pr.fmgWave & 3;      // the MGs
        v->lfoPh = 0; v->lfoInc = mgHz(pr.mgFreq) * (128.0f / 44100.0f);
        v->vibAmt = (pr.mgWave & 0x60) ? (pr.mgInt / 99.0f) * (pr.mgInt / 99.0f) * 0.8f : 0;
        v->fmgPh = 0.25f; v->fmgInc = mgHz(pr.fmgFreq) * (128.0f / 44100.0f);
        v->fmgAmt = (pr.fmgWave & 0x60) ? (pr.fmgInt / 99.0f) * (pr.fmgInt / 99.0f) * 0.05f : 0;
        v->vel = (pr.vdaLevel / 99.0f) * velF(pr.ampVel, velN);
        v->mindex = v->cat1 == M1C_BELL ? 2.5f : v->cat1 == M1C_PLUCK ? 3.0f : 1.2f;
        v->midxC = v->cat1 == M1C_BELL ? decay(400) : v->cat1 == M1C_PLUCK ? decay(100) : decay(600);
        v->inc3 = v->inc * (v->cat1 == M1C_BELL ? 3.5f : 1.0f);      // the FM stand-ins' modulator
      } else if (nav.engine == E_MWAVE) {        // cache the sound into the voice
        const MWSound &sn = MW_BANK[mwProg];
        v->eAc = mwcoef(sn.a); v->eDc = mwcoef(sn.dec);
        v->eSc = v->eDc; v->eRc = mwcoef(sn.r);
        v->lAtk = 1.0f; v->lBreak = v->lSus = sn.s / 127.0f;
        v->mseg = 0;
        float c = sn.cutoff / 127.0f;
        v->cutB = 0.004f + 0.26f * c * c;
        v->cutE = 0.2f * sn.fenvAmt / 127.0f;
        v->fSus = 0; v->fdC = mwcoef(sn.fdec);
        v->fenv = 1.0f;
        v->resK = 3.2f * sn.reso / 127.0f;
        v->wStart = sn.startWave / 63.0f;
        v->mindex = 0;                           // the wave env, rising
        v->midxC = 1.0f - expf(-1.0f / (900.0f * 44.1f));
      }
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

  static float mix[AUDIO_BLOCK_SAMPLES], m1buf[AUDIO_BLOCK_SAMPLES];
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) mix[i] = m1buf[i] = 0;

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
    float i1 = v.inc, i2 = v.minc, fmgVal = 0;
    if (d.osc == O_M1) {                         // pitch EG and the MGs, at block rate
      if (v.state == RELEASE) v.penv += v.pRc * (v.pRelL - v.penv);
      else if (v.pseg == 0) { v.penv += v.pAc * (v.pAtkL - v.penv); if (fabsf(v.penv - v.pAtkL) < 0.01f) v.pseg = 1; }
      else v.penv += v.pDc * (0.0f - v.penv);
      v.lfoPh += v.lfoInc; if (v.lfoPh >= 1.0f) v.lfoPh -= 1.0f;
      v.fmgPh += v.fmgInc; if (v.fmgPh >= 1.0f) v.fmgPh -= 1.0f;
      float pf = exp2f((v.penv * 12.0f + mgShape(v.mgWave, v.lfoPh) * v.vibAmt) / 12.0f);
      i1 *= pf; i2 *= pf;
      fmgVal = mgShape(v.fmgWave, v.fmgPh) * v.fmgAmt;
    }
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      if (d.osc != O_M1 && d.osc != O_MWAVE) switch (v.state) {   // (the banks run their own EGs)
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
        case O_M1: {
          // VDA EGs: attack to level, decay to break, slope to sustain.
          if (v.state == RELEASE) v.env += v.eRc * (0.0f - v.env);
          else switch (v.mseg) {
            case 0: v.env += v.eAc * (v.lAtk - v.env + 0.001f); if (v.env >= v.lAtk) { v.env = v.lAtk; v.mseg = 1; } break;
            case 1: v.env += v.eDc * (v.lBreak - v.env); if (fabsf(v.env - v.lBreak) < 0.004f) v.mseg = 2; break;
            default: v.env += v.eSc * (v.lSus - v.env); break;
          }
          float o = m1oscS(v.cat1, v.phase, i1, v.p3, v.mindex, v.rng) * v.env;
          if (v.dual) {                          // osc2 with its own VDA EG
            if (v.state == RELEASE) v.env2 += v.e2Rc * (0.0f - v.env2);
            else switch (v.mseg2) {
              case 0: v.env2 += v.e2Ac * (v.l2Atk - v.env2 + 0.001f); if (v.env2 >= v.l2Atk) { v.env2 = v.l2Atk; v.mseg2 = 1; } break;
              case 1: v.env2 += v.e2Dc * (v.l2Break - v.env2); if (fabsf(v.env2 - v.l2Break) < 0.004f) v.mseg2 = 2; break;
              default: v.env2 += v.e2Sc * (v.l2Sus - v.env2); break;
            }
            o = 0.7f * (o + m1oscS(v.cat2, v.mphase, i2, 0, 0, v.rng) * v.env2 * v.lvl2);
            v.mphase += i2; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
          }
          // The VDF's own EG (signed levels), the MG and the keyboard on the cutoff.
          if (v.state == RELEASE) v.fenv += v.fRc * (v.lfRl - v.fenv);
          else switch (v.fseg) {
            case 0: v.fenv += v.fAc * (v.lfAl - v.fenv); if (fabsf(v.fenv - v.lfAl) < 0.004f) v.fseg = 1; break;
            case 1: v.fenv += v.fDc * (v.lfB - v.fenv); if (fabsf(v.fenv - v.lfB) < 0.004f) v.fseg = 2; break;
            default: v.fenv += v.fSc * (v.lfS - v.fenv); break;
          }
          float fc = v.cutB + v.cutE * v.fenv + fmgVal + v.kbdCut;
          if (fc > 0.3f) fc = 0.3f; else if (fc < 0.0015f) fc = 0.0015f;
          float w = 6.2832f * fc, g = w / (1.0f + w);
          v.f1 += g * (o - v.f1); v.f2 += g * (v.f1 - v.f2);
          v.f3 += g * (v.f2 - v.f3); v.f4 += g * (v.f3 - v.f4);
          s = v.f4 * 1.3f;
          v.mindex *= v.midxC;
          v.p3 += v.inc3; if (v.p3 >= 1.0f) v.p3 -= 1.0f;
          v.phase += i1; if (v.phase >= 1.0f) v.phase -= 1.0f;
          break;
        }
        case O_MWAVE: {
          // VCA ADSR (the M1's segment machinery with attack to full).
          if (v.state == RELEASE) v.env += v.eRc * (0.0f - v.env);
          else switch (v.mseg) {
            case 0: v.env += v.eAc * (1.0f - v.env + 0.001f); if (v.env >= 1.0f) { v.env = 1.0f; v.mseg = 1; } break;
            default: v.env += v.eDc * (v.lSus - v.env); break;
          }
          // The wave sweep: morph sine -> saw -> square from the start wave.
          v.mindex += v.midxC * (1.0f - v.mindex);
          float w = v.wStart + 0.45f * v.mindex; if (w > 1.0f) w = 1.0f;
          float sn = sinT(v.phase);
          float sw = 2.0f * v.phase - 1.0f - polyblep(v.phase, v.inc);
          float s0 = w < 0.5f ? sn + (sw - sn) * (w * 2.0f)
                              : sw + ((v.phase < 0.5f ? 1.0f : -1.0f) - sw) * ((w - 0.5f) * 2.0f);
          // Resonant VCF with its decaying env sweep.
          v.fenv += v.fdC * (0.0f - v.fenv);
          float fc = v.cutB + v.cutE * v.fenv; if (fc > 0.3f) fc = 0.3f;
          float wc = 6.2832f * fc, g = wc / (1.0f + wc);
          float in = s0 - v.resK * v.f4;
          if (in > 1.4f) in = 1.4f; else if (in < -1.4f) in = -1.4f;
          in -= in * in * in * (1.0f / 6.0f);
          v.f1 += g * (in - v.f1); v.f2 += g * (v.f1 - v.f2);
          v.f3 += g * (v.f2 - v.f3); v.f4 += g * (v.f3 - v.f4);
          s = v.f4 * (1.2f + 0.5f * v.resK);
          break;
        }
        default:                               // O_SAW, O_PULSE
          s = d.osc == O_SAW ? 2.0f * v.phase - 1.0f : (v.phase < 0.5f ? 1.0f : -1.0f);
          v.lp += 0.5f * (s - v.lp); s = v.lp;
          break;
      }
      if (d.osc != O_M1) { v.phase += v.inc; if (v.phase >= 1.0f) v.phase -= 1.0f; }
      if (d.osc == O_M1) m1buf[i] += s * amp;    // (its envelopes are already in s)
      else mix[i] += s * v.env * amp;
    }
    if (v.env < 0.0005f && v.state != ATTACK) { v.state = IDLE; active--; }
  }
  _active = active;

  static float outL[AUDIO_BLOCK_SAMPLES], outR[AUDIO_BLOCK_SAMPLES];
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) outL[i] = outR[i] = mix[i];
  {
    const M1Program &pr = M1_BANK[m1Prog];       // the M1 bus + its program's effects
    fxProcess(m1buf, outL, outR, AUDIO_BLOCK_SAMPLES,
              fxCatOf(pr.fx1), pr.fx1Wet / 100.0f, fxCatOf(pr.fx2), pr.fx2Wet / 100.0f);
  }
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    float sl = outL[i], sr = outR[i];
    if (sl > 1.0f) sl = 1.0f; else if (sl < -1.0f) sl = -1.0f;
    if (sr > 1.0f) sr = 1.0f; else if (sr < -1.0f) sr = -1.0f;
    l->data[i] = (int16_t)(sl * 32767.0f);
    r->data[i] = (int16_t)(sr * 32767.0f);
  }
  transmit(l, 0); transmit(r, 1);
  release(l); release(r);

  uint32_t dt = ARM_DWT_CYCCNT - t0;
  if (dt > _maxCycles) _maxCycles = dt;
}

// ---- MIDI in (channel 1): USB and the serial in port ------------------------

static HardwareSerial *midiSerial = nullptr;

void synthMidiBegin() { midiSerial = board.midiInBegin(); }

uint8_t m1Program() { return m1Prog; }

void m1SetProgram(uint8_t p) {
  if (p > 99) return;
  m1Prog = p;
  Serial.printf("m1: program %02u %s\n", p, M1_BANK[p].name);
}

uint8_t mwProgram() { return mwProg; }

void mwSetProgram(uint8_t p) {
  mwProg = p;
  Serial.printf("mwave: sound %03u %s\n", p, MW_BANK[p].name);
}

// A MIDI program change goes to the current engine's bank.
static void setPreset(uint8_t p) {
  if (nav.engine == E_MWAVE) mwSetProgram(p);
  else m1SetProgram(p);
}

void synthServiceMidi() {
  while (usbMIDI.read()) {
    if (usbMIDI.getChannel() != 1) continue;
    uint8_t type = usbMIDI.getType();
    if (type == usbMIDI.NoteOn && usbMIDI.getData2() > 0) synth.noteOn(usbMIDI.getData1(), usbMIDI.getData2());
    else if (type == usbMIDI.NoteOff || type == usbMIDI.NoteOn) synth.noteOff(usbMIDI.getData1());
    else if (type == usbMIDI.ProgramChange) setPreset(usbMIDI.getData1());
  }
  if (!midiSerial) return;
  static uint8_t status = 0, d1 = 255;
  while (midiSerial->available()) {
    uint8_t b = midiSerial->read();
    if (b >= 0xF8) continue;                      // realtime
    if (b & 0x80) { status = b; d1 = 255; continue; }
    if (status == 0xC0) { setPreset(b); continue; }   // channel 1 program change
    if (status == 0x90 || status == 0x80) {       // channel 1 only
      if (d1 == 255) { d1 = b; continue; }
      if (status == 0x90 && b > 0) synth.noteOn(d1, b);
      else synth.noteOff(d1);
      d1 = 255;                                   // (running status kept)
    }
  }
}
