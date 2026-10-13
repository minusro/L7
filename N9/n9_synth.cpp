// n9_synth.cpp — see n9_synth.h.
#include "n9_synth.h"

#include "M8Board.h"
#include "n9_m1_bank.h"
#include "n9_mw_bank.h"
#include "n9_fx.h"
#include "n9_mi.h"
#include "n9_song.h"
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
static const char *const BASE_NAMES[E_MI_FIRST] = {"PROPHET", "303", "JUNO"};
const char *engineName(uint8_t e) {
  return e < E_MI_FIRST ? BASE_NAMES[e] : MI_MODEL_NAMES[e - E_MI_FIRST];
}

// The INST cells per engine kind.
static const ParamInfo PROPHET_P[8] = {{"DETUNE", 20}, {"CUTOFF", 60}, {"RES", 25}, {"ENV", 50},
                                       {"ATK", 5}, {"DEC", 45}, {"SUS", 60}, {"REL", 35}};
static const ParamInfo P303_P[8] = {{"CUTOFF", 40}, {"RES", 85}, {"ENVMOD", 80}, {"DECAY", 40},
                                    {"SLIDE", 30}, {"ACCENT", 70}, {"WAVE", 0}, {"LEVEL", 80}};
static const ParamInfo JUNO_P[8] = {{"DETUNE", 25}, {"SUB", 60}, {"CUTOFF", 65}, {"RES", 15},
                                    {"ATK", 3}, {"DEC", 50}, {"SUS", 70}, {"REL", 40}};
static const ParamInfo MI_P[8] = {{"HARMON", 50}, {"TIMBRE", 50}, {"MORPH", 50}, {"DECAY", 55},
                                  {"LPG", 30}, {"LEVEL", 80}, {"-", 0}, {"-", 0}};

const ParamInfo *engineParams(uint8_t e) {
  return e == E_PRO5 ? PROPHET_P : e == E_303 ? P303_P : e == E_JUNO ? JUNO_P : MI_P;
}

uint8_t trackEngine[9];
uint8_t engParams[9][ENGINE_COUNT][8];

static void paramsInit() {
  for (uint8_t t = 0; t < 9; t++) {
    trackEngine[t] = 0;
    for (uint8_t e = 0; e < ENGINE_COUNT; e++) {
      const ParamInfo *pi = engineParams(e);
      for (int i = 0; i < 8; i++) engParams[t][e][i] = pi[i].def;
    }
    engParams[t][E_MI_FIRST + 15][0] = 80;       // SPEECH starts in the word banks
  }
}

// 0-99 as an easing time constant (ms) for the analog engines' envelopes.
static inline float pms(uint8_t v) { return 1.5f * expf(0.069f * v); }
static inline float pcoef(uint8_t v) { return 1.0f - expf(-1.0f / (pms(v) * 44.1f)); }

static inline float decay(float ms);
static void applyLiveParams(N9Synth::Voice &v, bool snap);

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
  O_JUNO,    // poly DCO: saw + detuned saw + square sub into the ladder,
             // one ADSR for amp with a touch of it on the cutoff
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
// Only the first three engines use the pool voices; the Mutable models have
// their own pool (n9_mi).
static const EngineDef ENGINES[E_MI_FIRST] = {
    {O_PRO5, 5, 350, false, 0, 1.004f, 0, 0},          // PROPHET (ratio = osc B's detune)
    {O_303, 3, 12, true, 3500, 0, 0, 0},               // 303 (the VCA decays while gated, off fast)
    {O_JUNO, 3, 300, false, 0, 0, 0, 0},               // JUNO (ADSR comes from the cells)
};

// M1 helpers: a time value 0-99 as an easing time constant in ms (the M1's
// log scale over ~7 time constants of audible decay: value 99 ~ a 6-7 s
// tail, not a 50 s one), and as a per-sample easing coefficient.
static inline float m1ms(uint8_t v) { return 1.0f * expf(0.07f * v); }
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

// The analog engines' cells applied to a voice: snapped at noteOn, then,
// while it sounds and its engine is on screen, eased toward the cells every
// block (~20 ms) so edits land on held notes without zipper noise.
static void applyLiveParams(N9Synth::Voice &v, bool snap) {
  const uint8_t *p = engParams[v.track][v.engine];
  auto set = [snap](float &f, float target) { if (snap) f = target; else f += 0.14f * (target - f); };
  if (v.engine == E_PRO5) {
    set(v.minc, v.inc * (1.0f + 0.012f * p[0] / 99.0f));
    float c = p[1] / 99.0f;
    set(v.cutB, 0.004f + 0.20f * c * c);
    set(v.resK, 3.0f * p[2] / 99.0f);
    set(v.cutE, 0.16f * (p[3] / 99.0f) * (0.6f + 0.4f * v.velN));
    v.eAc = pcoef(p[4]); v.eDc = pcoef(p[5]);
    set(v.lSus, p[6] / 99.0f);
    v.eRc = pcoef(p[7]);
    v.fdC = decay(60.0f + 8.0f * pms(p[5]));
  } else if (v.engine == E_303) {
    v.cat1 = p[6] < 50 ? 0 : 1;                  // saw / square
    set(v.cutB, 0.003f + 0.02f * p[0] / 99.0f);
    set(v.resK, 3.6f * p[1] / 99.0f);
    set(v.cutE, (0.13f + (v.accent ? 0.05f * p[5] / 99.0f : 0)) * p[2] / 99.0f);
    v.fdC = decay(v.accent ? 110 : 60 + 500.0f * p[3] / 99.0f);
    v.midxC = 1.0f - expf(-1.0f / ((5.0f + 120.0f * p[4] / 99.0f) * 44.1f));   // the slide
    set(v.vel, v.velN * (p[7] / 99.0f) * (v.accent ? 1.0f + 0.5f * p[5] / 99.0f : 1.0f));
  } else if (v.engine == E_JUNO) {
    set(v.inc3, v.inc * (1.0f + 0.010f * p[0] / 99.0f));
    set(v.lfAl, 0.5f * p[0] / 99.0f);            // the detuned saw's level
    set(v.lfB, 0.8f * p[1] / 99.0f);             // the sub's
    v.minc = v.inc * 0.5f;
    float c = p[2] / 99.0f;
    set(v.cutB, 0.005f + 0.19f * c * c);
    set(v.resK, 3.0f * p[3] / 99.0f);
    v.eAc = pcoef(p[4]); v.eDc = pcoef(p[5]);
    set(v.lSus, p[6] / 99.0f);
    v.eRc = pcoef(p[7]);
  }
}

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

void N9Synth::noteOnNow(uint8_t track, uint8_t note, uint8_t vel) {
  uint8_t engine = trackEngine[track];
  if (engine >= E_MI_FIRST) {            // the plaits pool plays the Mutable models
    miNoteOn(track, note, vel, engine - E_MI_FIRST);
    return;
  }
  const EngineDef &d = ENGINES[engine];
  float hz = 440.0f * exp2f((note - 69) / 12.0f);
  float inc = hz / 44100.0f;
  Voice *v = nullptr;
  if (engine == E_303) {                   // mono per track: its 303 voice, reused
    for (auto &c : _v) if (c.state != IDLE && c.engine == E_303 && c.track == track) { v = &c; break; }
    if (v && v->state != RELEASE) {            // legato: slide there, envelopes kept running
      v->tinc = inc;
      v->note = note;
      v->accent = vel >= 100;
      return;
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
  v->note = note; v->engine = engine; v->track = track;
  v->accent = vel >= 100;
  v->inc = v->tinc = inc;
  v->minc = d.osc == O_SUPER ? inc * 1.0045f : inc * d.ratio;
  v->inc3 = inc * 0.9955f;
  v->phase = v->mphase = v->p3 = 0;
  v->mindex = d.index0;
  v->f1 = v->f2 = v->f3 = v->f4 = 0;
  v->fenv = v->accent ? 1.25f : 0.85f;
  v->env = 0; v->lp = 0;
  v->vel = vel / 127.0f;
  if (engine == E_303 && v->accent) v->vel = 1.2f;   // the accent's kick
  v->rng = _ageCounter * 2654435761u + note * 7919u + 1;
  if (engine == E_M1) {                  // cache the program into the voice
    const M1Program &pr = M1_BANK[m1Prog];
    float velN = vel / 127.0f;
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
    v->kbdCut = ((int)note - pr.kbdCenter) / 12.0f * (pr.cutKbd / 99.0f) * 0.05f;
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
  } else if (engine == E_MWAVE) {        // cache the sound into the voice
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
  } else {                                   // the analog engines read the INST cells
    v->velN = vel / 127.0f;
    if (engine == E_PRO5 || engine == E_JUNO) { v->mseg = 0; v->fenv = 1.0f; }
    if (engine == E_303) v->fenv = 1.0f;
    applyLiveParams(*v, true);
  }
  v->age = ++_ageCounter;
  v->state = ATTACK;
}

void N9Synth::noteOffNow(uint8_t track, uint8_t note) {
  miNoteOff(track, note);
  for (auto &c : _v)
    if (c.state != IDLE && c.state != RELEASE && c.track == track && c.note == note) c.state = RELEASE;
}

void N9Synth::seqToggle() {
  if (_seqOn) _seqStopReq = true;
  else _seqStartReq = true;
}

// Offs due this step first, then the clip's notes that start on it.
// Offs due this step first, then every track's clip notes that start on it.
void N9Synth::fireStep(uint8_t step) {
  for (int i = 0; i < _seqNoteCount;) {
    if (--_seqNotes[i].left == 0) { noteOffNow(_seqNotes[i].track, _seqNotes[i].pitch); _seqNotes[i] = _seqNotes[--_seqNoteCount]; }
    else i++;
  }
  uint32_t abs = (uint32_t)_seqBar * 16 + step;
  for (uint8_t t = 0; t < 9; t++) {
    const Clip &c = clipAt(t, nav.gridS);
    uint16_t local = abs % (c.bars * 16);        // each clip loops its own bars
    uint8_t n = c.count;
    for (uint8_t i = 0; i < n && i < CLIP_MAX_NOTES; i++) {
      const ClipNote &cn = c.notes[i];
      if (cn.start == local && _seqNoteCount < 32) {
        noteOnNow(t, cn.pitch, cn.vel);
        _seqNotes[_seqNoteCount++] = {t, cn.pitch, cn.len};
      }
    }
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
    if (e.on) noteOnNow(nav.gridT, e.note, e.vel);   // MIDI plays the selected track
    else noteOffNow(nav.gridT, e.note);
  }

  // The transport.
  if (_seqStopReq) {
    _seqStopReq = false; _seqOn = false;
    for (int i = 0; i < _seqNoteCount; i++) noteOffNow(_seqNotes[i].track, _seqNotes[i].pitch);
    _seqNoteCount = 0;
  }
  if (_seqStartReq) {
    _seqStartReq = false; _seqOn = true;
    _seqStep = 0; _seqBar = 0; _seqAcc = 0;
    fireStep(0);
  }
  if (_seqOn) {
    float spp = 44100.0f * 60.0f / (_seqBpm * 4.0f);
    _seqAcc += AUDIO_BLOCK_SAMPLES;
    while (_seqAcc >= spp) {
      _seqAcc -= spp;
      uint8_t st = _seqStep + 1;
      if (st >= 16) { st = 0; _seqBar = _seqBar + 1; }
      _seqStep = st;
      fireStep(st);
    }
  }

  audio_block_t *l = allocate();
  audio_block_t *r = allocate();
  if (!l || !r) { if (l) release(l); if (r) release(r); return; }

  static float tbus[9][AUDIO_BLOCK_SAMPLES];
  memset(tbus, 0, sizeof tbus);

  uint8_t active = 0;
  for (auto &v : _v) {
    if (v.state == IDLE) continue;
    active++;
    const EngineDef &d = ENGINES[v.engine];
    float atkC = 1.0f - decay(d.atkMs < 0.1f ? 0.1f : d.atkMs);
    float decC = d.pluck ? decay(d.decMs) : 1.0f;
    float relC = decay(d.relMs);
    float idxC = d.osc == O_FM ? decay(d.idxDecMs) : 1.0f;
    // (the 303's and PROPHET's filter decays are per-voice now: v.fdC)
    float amp = v.vel * 0.12f;
    if (v.track == nav.gridT && v.engine == curEngine() && v.engine < E_MI_FIRST)
      applyLiveParams(v, false);                 // live cell edits, eased
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
      if (d.osc != O_M1 && d.osc != O_MWAVE && d.osc != O_PRO5 && d.osc != O_JUNO)
      switch (v.state) {                         // (those engines run their own EGs)
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
          v.inc += (v.tinc - v.inc) * v.midxC;             // slide (SLIDE cell)
          v.fenv *= v.fdC;
          float o = 2.0f * v.phase - 1.0f - polyblep(v.phase, v.inc);
          if (v.cat1) {                                    // WAVE: square (two bleps)
            float ph2 = v.phase + 0.5f; if (ph2 >= 1.0f) ph2 -= 1.0f;
            o = (o - (2.0f * ph2 - 1.0f - polyblep(ph2, v.inc))) * 0.7f;
          }
          float fc = v.cutB + v.cutE * v.fenv;
          float w = 6.2832f * fc, g = w / (1.0f + w);
          float in = o - v.resK * v.f4;                    // resonance feedback
          if (in > 1.4f) in = 1.4f; else if (in < -1.4f) in = -1.4f;
          in -= in * in * in * (1.0f / 6.0f);              // the ladder's soft squash
          v.f1 += g * (in - v.f1); v.f2 += g * (v.f1 - v.f2);
          v.f3 += g * (v.f2 - v.f3); v.f4 += g * (v.f3 - v.f4);
          s = v.f4 * (2.0f + 0.3f * v.resK);
          break;
        }
        case O_PRO5: {
          if (v.state == RELEASE) v.env += v.eRc * (0.0f - v.env);
          else if (v.mseg == 0) { v.env += v.eAc * (1.0f - v.env + 0.001f); if (v.env >= 1.0f) { v.env = 1.0f; v.mseg = 1; } }
          else v.env += v.eDc * (v.lSus - v.env);
          v.fenv *= v.fdC;
          float sawA = 2.0f * v.phase - 1.0f - polyblep(v.phase, v.inc);
          float sawB = 2.0f * v.mphase - 1.0f - polyblep(v.mphase, v.minc);
          float s0 = (sawA + sawB) * 0.5f;
          float fc = v.cutB + v.cutE * (0.3f + 0.7f * v.fenv);
          float w = 6.2832f * fc, g = w / (1.0f + w);
          float in = s0 - v.resK * v.f4;
          if (in > 1.4f) in = 1.4f; else if (in < -1.4f) in = -1.4f;
          in -= in * in * in * (1.0f / 6.0f);
          v.f1 += g * (in - v.f1); v.f2 += g * (v.f1 - v.f2);
          v.f3 += g * (v.f2 - v.f3); v.f4 += g * (v.f3 - v.f4);
          s = v.f4 * (1.5f + 0.4f * v.resK);
          v.mphase += v.minc; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
          break;
        }
        case O_JUNO: {
          if (v.state == RELEASE) v.env += v.eRc * (0.0f - v.env);
          else if (v.mseg == 0) { v.env += v.eAc * (1.0f - v.env + 0.001f); if (v.env >= 1.0f) { v.env = 1.0f; v.mseg = 1; } }
          else v.env += v.eDc * (v.lSus - v.env);
          float saw = 2.0f * v.phase - 1.0f - polyblep(v.phase, v.inc);
          float saw2 = 2.0f * v.p3 - 1.0f - polyblep(v.p3, v.inc3);
          float sub = v.mphase < 0.5f ? 1.0f : -1.0f;
          float s0 = saw * 0.7f + saw2 * v.lfAl + sub * v.lfB;
          float fc = v.cutB + 0.08f * v.env;               // a touch of the ADSR on the cutoff
          float w = 6.2832f * fc, g = w / (1.0f + w);
          float in = s0 - v.resK * v.f4;
          if (in > 1.4f) in = 1.4f; else if (in < -1.4f) in = -1.4f;
          in -= in * in * in * (1.0f / 6.0f);
          v.f1 += g * (in - v.f1); v.f2 += g * (v.f1 - v.f2);
          v.f3 += g * (v.f2 - v.f3); v.f4 += g * (v.f3 - v.f4);
          s = v.f4 * 1.5f;
          v.p3 += v.inc3; if (v.p3 >= 1.0f) v.p3 -= 1.0f;
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
      tbus[v.track][i] += d.osc == O_M1 ? s * amp : s * v.env * amp;
    }
    bool quiet = v.env < 0.002f && (!v.dual || v.env2 < 0.002f);
    if (quiet && v.state != ATTACK) { v.state = IDLE; active--; }
  }
  miRender(tbus, AUDIO_BLOCK_SAMPLES);
  _active = active + miActive();

  static float outL[AUDIO_BLOCK_SAMPLES], outR[AUDIO_BLOCK_SAMPLES];
  memset(outL, 0, sizeof outL); memset(outR, 0, sizeof outR);
  for (uint8_t t = 0; t < 9; t++) {                // each track: its mono insert, into the mix
    fxProcessTrack(t, tbus[t], AUDIO_BLOCK_SAMPLES);
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { outL[i] += tbus[t][i]; outR[i] += tbus[t][i]; }
  }
  fxProcessMaster(outL, outR, AUDIO_BLOCK_SAMPLES);   // the master's stereo effect
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {     // (master volume is unity for now,)
    l->data[i] = (int16_t)(fxSoftClip(outL[i]) * 32767.0f);   // (then the soft clipper)
    r->data[i] = (int16_t)(fxSoftClip(outR[i]) * 32767.0f);
  }
  transmit(l, 0); transmit(r, 1);
  release(l); release(r);

  uint32_t dt = ARM_DWT_CYCCNT - t0;
  if (dt > _maxCycles) _maxCycles = dt;
}

// ---- MIDI in (channel 1): USB and the serial in port ------------------------

static HardwareSerial *midiSerial = nullptr;

void synthMidiBegin() {
  paramsInit();
  fxSlotsInit();
  midiSerial = board.midiInBegin();
}

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
  if (curEngine() == E_MWAVE) mwSetProgram(p);
  else m1SetProgram(p);
}

void synthServiceMidi() {
  while (usbMIDI.read()) {
    if (usbMIDI.getChannel() != 1) continue;
    uint8_t type = usbMIDI.getType();
    if (type == usbMIDI.NoteOn && usbMIDI.getData2() > 0) synth.noteOn(scaleSnap(usbMIDI.getData1()), usbMIDI.getData2());
    else if (type == usbMIDI.NoteOff || type == usbMIDI.NoteOn) synth.noteOff(scaleSnap(usbMIDI.getData1()));
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
      if (status == 0x90 && b > 0) synth.noteOn(scaleSnap(d1), b);
      else synth.noteOff(scaleSnap(d1));
      d1 = 255;                                   // (running status kept)
    }
  }
}
