// n9_synth.cpp — see n9_synth.h.
#include "n9_synth.h"

#include "M8Board.h"
#include "n9_gm_bank.h"
#include "n9_gm_pcm.h"
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
  if (e == E_GM) return "GM";
  if (e == E_GMDRUM) return "GM DRUM";
  if (e == E_DM) return "DRUMS";
  return e < E_MI_FIRST ? BASE_NAMES[e] : MI_MODEL_NAMES[e - E_MI_FIRST];
}

uint8_t gmProgram[9];
uint8_t drumMachine[9];

// The INST cells per engine kind.
static const ParamInfo PROPHET_P[8] = {{"DETUNE", 20}, {"CUTOFF", 60}, {"RES", 25}, {"ENV", 50},
                                       {"ATK", 5}, {"DEC", 45}, {"SUS", 60}, {"REL", 35}};
static const ParamInfo P303_P[8] = {{"CUTOFF", 40}, {"RES", 85}, {"ENVMOD", 80}, {"DECAY", 40},
                                    {"SLIDE", 30}, {"ACCENT", 70}, {"WAVE", 0}, {"LEVEL", 80}};
static const ParamInfo JUNO_P[8] = {{"DETUNE", 25}, {"SUB", 60}, {"CUTOFF", 65}, {"RES", 15},
                                    {"ATK", 3}, {"DEC", 50}, {"SUS", 70}, {"REL", 40}};
static const ParamInfo MI_P[8] = {{"HARMON", 50}, {"TIMBRE", 50}, {"MORPH", 50}, {"DECAY", 55},
                                  {"LPG", 30}, {"LEVEL", 80}, {"-", 0}, {"-", 0}};
// GM: the cells are offsets over the program's own patch (50 = as authored).
static const ParamInfo GM_P[8] = {{"CUT", 50}, {"RES", 50}, {"ENV", 50}, {"ATK", 50},
                                  {"DEC", 50}, {"REL", 50}, {"VIB", 50}, {"LEVEL", 80}};
static const ParamInfo GMD_P[8] = {{"-", 0}, {"-", 0}, {"-", 0}, {"-", 0},
                                   {"-", 0}, {"-", 0}, {"-", 0}, {"LEVEL", 80}};
// DRUMS: offsets over the machine's sounds (50 = as authored), at note-on.
static const ParamInfo DM_P[8] = {{"TUNE", 50}, {"DECAY", 50}, {"SNAP", 50}, {"TONE", 50},
                                  {"-", 0}, {"-", 0}, {"-", 0}, {"LEVEL", 80}};

const ParamInfo *engineParams(uint8_t e) {
  return e == E_PRO5 ? PROPHET_P : e == E_303 ? P303_P : e == E_JUNO ? JUNO_P
       : e == E_GM ? GM_P : e == E_GMDRUM ? GMD_P : e == E_DM ? DM_P : MI_P;
}

uint8_t trackEngine[9];
uint8_t engParams[9][ENGINE_COUNT][8];

void paramsInit() {
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
  O_DM,      // DRUMS: synthesized 606/707/808/909 hits, 12 sounds an octave
             // (C kick, C# closed hat, D snare, D# open hat, E clap, F/G/A
             // toms, F#/G#/A# crash/ride/cymbal, B rim)
  O_GM,      // General MIDI, Sound Canvas style but synthesized
             // (n9_gm_bank.h): two polyBLEP/table oscillators per the
             // program's patch, ladder filter with its own decaying env,
             // full ADSR amp, block-rate vibrato that fades in
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
static const EngineDef GM_DEF = {O_GM, 3, 300, false, 0, 0, 0, 0};   // (everything from the patch)
static const EngineDef DM_DEF = {O_DM, 0, 10, false, 0, 0, 0, 0};
static inline const EngineDef &engineDefOf(uint8_t e) {
  return e == E_GM || e == E_GMDRUM ? GM_DEF : e == E_DM ? DM_DEF : ENGINES[e];
}

// ---- DRUMS: the four machines' kits -----------------------------------------
// One analog hit per octave slot; flavoured by machine. tone doubles as the
// kick's drive and the hats' brightness; sweep/pdecMs are the pitch env.
enum : uint8_t { DM_KICK, DM_HAT, DM_SNARE, DM_CLAP, DM_TOM, DM_CYM, DM_RIM };
struct DmSound { uint8_t type; float f, decMs, noise, tone, lvl, sweep, pdecMs; };
// Slots: 0 kick, 1 CH, 2 snare, 3 OH, 4 clap, 5 LT, 6 crash, 7 MT, 8 ride,
// 9 HT, 10 cym 2, 11 rim.
static const DmSound DM_KIT[4][12] = {
  { // 606: sharp and tinny
    {DM_KICK,  58, 150, 0.0f, 0.15f, 0.95f, 1.5f, 28},
    {DM_HAT, 4900,  40, 0.55f, 0.75f, 0.70f, 0, 0},
    {DM_SNARE, 192, 105, 0.85f, 0.45f, 0.90f, 0.35f, 24},
    {DM_HAT, 4900, 300, 0.55f, 0.70f, 0.70f, 0, 0},
    {DM_CLAP,   0, 420, 1.0f, 0.5f, 0.85f, 0, 0},
    {DM_TOM,  105, 190, 0.12f, 0.3f, 0.95f, 0.8f, 42},
    {DM_CYM, 4200, 850, 0.60f, 0.55f, 0.70f, 0, 0},
    {DM_TOM,  150, 170, 0.12f, 0.3f, 0.95f, 0.8f, 40},
    {DM_CYM, 5600, 650, 0.45f, 0.80f, 0.62f, 0, 0},
    {DM_TOM,  215, 150, 0.12f, 0.3f, 0.95f, 0.8f, 38},
    {DM_CYM, 4600, 420, 0.60f, 0.60f, 0.66f, 0, 0},
    {DM_RIM, 1750,  26, 0.2f, 0.5f, 0.85f, 0, 0},
  },
  { // 707: boxy and punchy
    {DM_KICK,  56, 190, 0.0f, 0.30f, 0.95f, 1.3f, 32},
    {DM_HAT, 4300,  48, 0.65f, 0.65f, 0.72f, 0, 0},
    {DM_SNARE, 186, 125, 0.75f, 0.50f, 0.92f, 0.3f, 26},
    {DM_HAT, 4300, 330, 0.65f, 0.60f, 0.72f, 0, 0},
    {DM_CLAP,   0, 430, 1.0f, 0.5f, 0.88f, 0, 0},
    {DM_TOM,  100, 230, 0.15f, 0.3f, 0.95f, 0.7f, 48},
    {DM_CYM, 4100, 950, 0.60f, 0.55f, 0.72f, 0, 0},
    {DM_TOM,  145, 210, 0.15f, 0.3f, 0.95f, 0.7f, 46},
    {DM_CYM, 5400, 800, 0.40f, 0.82f, 0.62f, 0, 0},
    {DM_TOM,  205, 190, 0.15f, 0.3f, 0.95f, 0.7f, 44},
    {DM_CYM, 4500, 520, 0.60f, 0.60f, 0.66f, 0, 0},
    {DM_RIM, 1700,  30, 0.25f, 0.5f, 0.88f, 0, 0},
  },
  { // 808: round and long
    {DM_KICK,  48, 640, 0.0f, 0.10f, 1.00f, 2.2f, 44},
    {DM_HAT, 3800,  42, 0.50f, 0.70f, 0.66f, 0, 0},
    {DM_SNARE, 180, 140, 0.70f, 0.45f, 0.92f, 0.3f, 24},
    {DM_HAT, 3800, 430, 0.50f, 0.65f, 0.66f, 0, 0},
    {DM_CLAP,   0, 450, 1.0f, 0.5f, 0.88f, 0, 0},
    {DM_TOM,   88, 320, 0.10f, 0.3f, 0.95f, 0.6f, 60},
    {DM_CYM, 4000, 1300, 0.55f, 0.50f, 0.70f, 0, 0},
    {DM_TOM,  128, 290, 0.10f, 0.3f, 0.95f, 0.6f, 56},
    {DM_CYM, 5200, 900, 0.40f, 0.78f, 0.60f, 0, 0},
    {DM_TOM,  180, 260, 0.10f, 0.3f, 0.95f, 0.6f, 52},
    {DM_CYM, 4400, 600, 0.55f, 0.58f, 0.64f, 0, 0},
    {DM_RIM, 1720,  22, 0.15f, 0.5f, 0.85f, 0, 0},
  },
  { // 909: hard and driven
    {DM_KICK,  51, 420, 0.0f, 0.75f, 1.00f, 2.0f, 55},
    {DM_HAT, 5200,  52, 0.80f, 0.80f, 0.72f, 0, 0},
    {DM_SNARE, 184, 130, 0.95f, 0.55f, 0.95f, 0.45f, 28},
    {DM_HAT, 5200, 380, 0.80f, 0.75f, 0.72f, 0, 0},
    {DM_CLAP,   0, 400, 1.0f, 0.55f, 0.90f, 0, 0},
    {DM_TOM,   98, 260, 0.25f, 0.3f, 0.98f, 0.9f, 50},
    {DM_CYM, 4300, 1100, 0.70f, 0.60f, 0.74f, 0, 0},
    {DM_TOM,  140, 240, 0.25f, 0.3f, 0.98f, 0.9f, 48},
    {DM_CYM, 5600, 950, 0.50f, 0.85f, 0.64f, 0, 0},
    {DM_TOM,  200, 220, 0.25f, 0.3f, 0.98f, 0.9f, 46},
    {DM_CYM, 4700, 560, 0.70f, 0.65f, 0.68f, 0, 0},
    {DM_RIM, 1760,  32, 0.3f, 0.5f, 0.90f, 0, 0},
  },
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

static float MULAWF[256];
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
  } else if (v.engine == E_GM) {               // the program's patch, cells as offsets
    const GmPatch &gp = GM_BANK[gmProgram[v.track] & 127];
    auto ofs = [](int base, uint8_t cell) { int r = base + cell - 50; return r < 0 ? 0 : r > 99 ? 99 : r; };
    if (v.pcm) {                               // a sample: its timbre is baked; level + release
      v.eRc = pcoef(ofs(gp.r, p[5]));
      set(v.vel, v.velN * (gp.lvl / 99.0f) * (p[7] / 80.0f));
      return;
    }
    float c = ofs(gp.cut, p[0]) / 99.0f;
    set(v.cutB, 0.004f + 0.22f * c * c);
    set(v.resK, 3.2f * ofs(gp.res, p[1]) / 99.0f);
    set(v.cutE, 0.24f * (ofs(gp.env, p[2]) / 99.0f) * (0.5f + 0.5f * v.velN));
    v.eAc = pcoef(ofs(gp.a, p[3]));
    v.eDc = pcoef(ofs(gp.d, p[4]));
    set(v.lSus, gp.s / 99.0f);
    v.eRc = pcoef(ofs(gp.r, p[5]));
    v.fdC = decay(8.0f * pms(gp.fd));
    v.vibAmt = 0.4f * ofs(gp.vib, p[6]) / 99.0f;
    set(v.vel, v.velN * (gp.lvl / 99.0f) * (p[7] / 80.0f));
  }
}

// polyBLEP residual: subtracts the saw's step discontinuity alias
static inline float polyblep(float t, float dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}

// A GM oscillator by wave (n9_gm_bank.h): polyBLEP saw, square and 25% pulse
// (as saw differences), triangle, table sine, noise.
static inline float gmOsc(uint8_t w, float ph, float dt, uint32_t &rng) {
  switch (w) {
    case GW_SAW: return 2.0f * ph - 1.0f - polyblep(ph, dt);
    case GW_SQR: {
      float p2 = ph + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
      return ((2.0f * ph - 1.0f - polyblep(ph, dt)) - (2.0f * p2 - 1.0f - polyblep(p2, dt))) * 0.7f;
    }
    case GW_PUL: {
      float p2 = ph + 0.75f; if (p2 >= 1.0f) p2 -= 1.0f;
      return ((2.0f * ph - 1.0f - polyblep(ph, dt)) - (2.0f * p2 - 1.0f - polyblep(p2, dt)) - 0.5f) * 0.8f;
    }
    case GW_TRI: return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
    case GW_SIN: return sinT(ph);
    default: rng = rng * 1664525u + 1013904223u; return (int32_t)rng * 4.6566e-10f;
  }
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
  if (engine == E_GMDRUM && GM_DRUMMAP[note & 127].smp < 0) return;   // no hit on that key
  if (engine >= E_MI_FIRST && engine < E_GM) {            // the plaits pool plays the Mutable models
    miNoteOn(track, note, vel, engine - E_MI_FIRST);
    return;
  }
  const EngineDef &d = engineDefOf(engine);
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
  v->pcm = nullptr;
  v->dmT = 0;
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
  } else if (engine == E_GM) {             // cache the GM program into the voice
    const GmPatch &gp = GM_BANK[gmProgram[track] & 127];
    v->velN = vel / 127.0f;
    const GmZone *zone = nullptr;          // a baked SC-88 sample for this program?
    for (int z = 0; z < GM_ZONE_MAX; z++) {
      const GmZone &zz = GM_ZONES[gmProgram[track] & 127][z];
      if (zz.smp >= 0 && note <= zz.hi) { zone = &zz; break; }
    }
    if (zone) {
      const GmSample &gs = GM_SAMPLES[zone->smp];
      v->pcm = GM_PCM + gs.off;
      v->pcmLen = gs.len; v->pcmLoopS = gs.loopS; v->pcmLoopE = gs.loopE;
      v->pcmPos = 0;
      v->pcmRate = 0.5f * exp2f((note - zone->root) / 12.0f + zone->cents / 1200.0f);   // (22050 Hz data)
      v->velN *= gs.gain / 64.0f;            // the soundfont's balance between samples
      v->eAc = pcoef(2);
      applyLiveParams(*v, true);
      v->age = ++_ageCounter;
      v->state = ATTACK;
      return;
    }
    v->cat1 = gp.w1;
    v->cat2 = gp.w2;
    v->inc3 = exp2f(gp.semi2 / 12.0f + gp.det / 1200.0f);   // osc 2's ratio
    v->minc = inc * v->inc3;
    v->lfAl = gp.mix / 99.0f;
    v->mseg = 0;
    v->fenv = 0.5f + 0.5f * v->velN;       // velocity opens the sweep
    v->lfoPh = 0;
    v->lfoInc = 5.3f / 44100.0f * AUDIO_BLOCK_SAMPLES;      // vibrato, block rate
    v->env2 = 0;                           // its fade-in
    v->e2Ac = 1.0f - expf(-2.902f / 500.0f);
    applyLiveParams(*v, true);
  } else if (engine == E_GMDRUM) {         // the GM drum map (SC-88 one-shots)
    const GmDrumHit &dh = GM_DRUMMAP[note & 127];
    const GmSample &gs = GM_SAMPLES[dh.smp];
    v->velN = vel / 127.0f;
    v->pcm = GM_PCM + gs.off;
    v->pcmLen = gs.len; v->pcmLoopS = 0; v->pcmLoopE = 0;
    v->pcmPos = 0;
    v->pcmRate = 0.5f * dh.rate256 / 256.0f;
    v->eAc = pcoef(0);
    v->eRc = pcoef(20);
    v->vel = v->velN * (gs.gain / 64.0f) * (dh.gain / 64.0f) * (engParams[track][E_GMDRUM][7] / 80.0f);
    if (note == 42 || note == 44)          // a closed/pedal hat chokes the open one
      for (auto &c : _v)
        if (&c != v && c.state != IDLE && c.engine == E_GMDRUM && c.track == track && c.note == 46)
          { c.state = RELEASE; c.eRc = pcoef(6); }
  } else if (engine == E_DM) {             // DRUMS: one analog hit per octave slot
    const DmSound &ds = DM_KIT[drumMachine[track] & 3][note % 12];
    const uint8_t *cp = engParams[track][E_DM];
    v->velN = vel / 127.0f;
    v->cat1 = ds.type;
    v->inc = ds.f * exp2f((cp[0] - 50) / 60.0f) / 44100.0f;
    v->minc = v->inc * (ds.type == DM_SNARE ? 1.83f : 1.4471f);
    float dsc = exp2f((cp[1] - 50) / 30.0f);
    v->eDc = decay(ds.decMs * dsc);
    v->eRc = decay(ds.decMs * 0.5f * dsc);     // the snare's noise env
    v->lfB = ds.noise * (0.4f + 1.2f * cp[2] / 99.0f);
    v->lfAl = ds.tone * (0.4f + 1.2f * cp[3] / 99.0f);
    v->cutE = ds.sweep;
    v->fdC = decay(ds.pdecMs);
    v->fenv = 1.0f; v->env = 1.0f; v->env2 = 1.0f;
    v->f1 = v->f2 = 0; v->lp = 0;
    v->vel = v->velN * ds.lvl * (cp[7] / 80.0f);
    int slot = note % 12;
    if (slot == 1 || slot == 3)            // a new hat chokes the other one
      for (auto &c : _v)
        if (&c != v && c.state != IDLE && c.engine == E_DM && c.track == track &&
            (c.note % 12 == 1 || c.note % 12 == 3))
          c.eDc = 0.996f;
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
    if (c.state != IDLE && c.state != RELEASE && c.track == track && c.note == note) {
      if (c.engine == E_GMDRUM || c.engine == E_DM) continue;   // one-shots play through
      c.state = RELEASE;
    }
}

void N9Synth::seqToggle() {
  if (_seqOn) _seqStopReq = true;
  else _seqStartReq = true;
}

// Offs due this step first; on a bar line each track's pending launch or
// stop is taken (the grid's A, quantised to the bar); then every playing
// clip's notes that start on this step fire. Each clip loops its own bars
// from the bar it was launched on (polymeter between tracks is free).
static uint32_t clipLaunchAt[9];                 // abs sixteenth of each launch (interrupt only)
void N9Synth::fireStep(uint8_t step) {
  for (int i = 0; i < _seqNoteCount;) {
    if (--_seqNotes[i].left == 0) { noteOffNow(_seqNotes[i].track, _seqNotes[i].pitch); _seqNotes[i] = _seqNotes[--_seqNoteCount]; }
    else i++;
  }
  if (clipFreeze) return;                        // a project load is swapping the clips
  uint32_t abs = (uint32_t)_seqBar * 16 + step;
  bool barLine = step == 0 || clipTakeNow;       // takeNow: a project came in on the beat
  clipTakeNow = 0;
  for (uint8_t t = 0; t < 9; t++) {
    if (barLine && clipPending[t] != -1) {
      int8_t p = clipPending[t];
      clipPending[t] = -1;
      if (p == CLIP_STOP) clipPlaying[t] = -1;
      else { clipPlaying[t] = p; clipLaunchAt[t] = abs; }
    }
    int8_t s = clipPlaying[t];
    if (s < 0) continue;
    const Clip &c = clipAt(t, (uint8_t)s);
    uint16_t local = (abs - clipLaunchAt[t]) % (c.bars * 16);
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
    for (int i = 0; i < 256; i++) {              // mu-law expand (tools/sc88_bank.py's encode)
      float y = i / 127.5f - 1.0f;
      MULAWF[i] = (y < 0 ? -1.0f : 1.0f) * (expf(fabsf(y) * 5.5452f) - 1.0f) / 255.0f;
    }
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
    for (int t = 0; t < 9; t++) clipLaunchAt[t] = 0;   // still-armed clips: from their tops
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
    const EngineDef &d = engineDefOf(v.engine);
    float atkC = 1.0f - decay(d.atkMs < 0.1f ? 0.1f : d.atkMs);
    float decC = d.pluck ? decay(d.decMs) : 1.0f;
    float relC = decay(d.relMs);
    float idxC = d.osc == O_FM ? decay(d.idxDecMs) : 1.0f;
    // (the 303's and PROPHET's filter decays are per-voice now: v.fdC)
    float amp = v.vel * 0.12f;
    if (v.track == nav.gridT && v.engine == curEngine() && (v.engine < E_MI_FIRST || v.engine == E_GM))
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
    } else if (d.osc == O_GM && v.vibAmt > 0.001f) {   // vibrato, fading in
      v.lfoPh += v.lfoInc; if (v.lfoPh >= 1.0f) v.lfoPh -= 1.0f;
      v.env2 += v.e2Ac * (1.0f - v.env2);
      float pf = exp2f(mgShape(0, v.lfoPh) * v.vibAmt * v.env2 / 12.0f);
      v.inc = v.tinc * pf;
      v.minc = v.tinc * v.inc3 * pf;
    }
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      if (d.osc != O_M1 && d.osc != O_MWAVE && d.osc != O_PRO5 && d.osc != O_JUNO && d.osc != O_GM && d.osc != O_DM)
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
        case O_GM: {
          if (v.pcm) {                       // a baked SC-88 sample from flash
            if (v.state == RELEASE) v.env += v.eRc * (0.0f - v.env);
            else { v.env += v.eAc * (1.0f - v.env); if (v.state == ATTACK && v.env > 0.98f) v.state = HOLD; }
            uint32_t ip = (uint32_t)v.pcmPos;
            if (v.pcmLoopE && ip >= v.pcmLoopE) {            // the loop wraps (the pad covers ip+1)
              v.pcmPos -= (float)(v.pcmLoopE - v.pcmLoopS);
              ip = (uint32_t)v.pcmPos;
            }
            if (ip + 1 >= v.pcmLen) {                        // a one-shot played out
              s = 0;
              v.env = 0;
              if (v.state == ATTACK) v.state = HOLD;
            } else {
              float fr = v.pcmPos - ip;
              float a = MULAWF[v.pcm[ip]], b = MULAWF[v.pcm[ip + 1]];
              s = a + (b - a) * fr;
              v.pcmPos += v.pcmRate;
            }
            break;
          }
          if (v.state == RELEASE) v.env += v.eRc * (0.0f - v.env);
          else if (v.mseg == 0) { v.env += v.eAc * (1.0f - v.env + 0.001f); if (v.env >= 1.0f) { v.env = 1.0f; v.mseg = 1; } }
          else v.env += v.eDc * (v.lSus - v.env);
          v.fenv *= v.fdC;
          float s0 = (gmOsc(v.cat1, v.phase, v.inc, v.rng) + gmOsc(v.cat2, v.mphase, v.minc, v.rng) * v.lfAl) * 0.6f;
          float fc = v.cutB + v.cutE * v.fenv;
          float w = 6.2832f * fc, g = w / (1.0f + w);
          float in = s0 - v.resK * v.f4;
          if (in > 1.4f) in = 1.4f; else if (in < -1.4f) in = -1.4f;
          in -= in * in * in * (1.0f / 6.0f);
          v.f1 += g * (in - v.f1); v.f2 += g * (v.f1 - v.f2);
          v.f3 += g * (v.f2 - v.f3); v.f4 += g * (v.f3 - v.f4);
          s = v.f4 * (1.6f + 0.3f * v.resK);
          v.mphase += v.minc; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
          break;
        }
        case O_DM: {                       // DRUMS: one analog hit per slot
          if (v.state == ATTACK) v.state = HOLD;   // (instant on; the quiet check kills it)
          v.rng = v.rng * 1664525u + 1013904223u;
          float n = (int32_t)v.rng * 4.6566e-10f, s0 = 0;
          switch (v.cat1) {
            case DM_KICK: {
              v.fenv *= v.fdC;
              s0 = sinT(v.phase);
              if (v.dmT < 32) s0 += 0.5f * n;                // the click
              s0 *= 1.0f + 2.5f * v.lfAl;                    // tone = drive
              if (s0 > 1.4f) s0 = 1.4f; else if (s0 < -1.4f) s0 = -1.4f;
              s0 -= s0 * s0 * s0 * (1.0f / 6.0f);
              v.phase += v.inc * (1.0f + v.cutE * v.fenv); if (v.phase >= 1.0f) v.phase -= 1.0f;
              v.env *= v.eDc;
              s0 *= v.env;
              break;
            }
            case DM_TOM: {
              v.fenv *= v.fdC;
              s0 = sinT(v.phase) + 0.25f * v.lfB * n;
              v.phase += v.inc * (1.0f + v.cutE * v.fenv); if (v.phase >= 1.0f) v.phase -= 1.0f;
              v.env *= v.eDc;
              s0 *= v.env;
              break;
            }
            case DM_SNARE: {
              v.fenv *= v.fdC;
              float tone = (sinT(v.phase) + 0.6f * sinT(v.mphase)) * 0.55f;
              v.phase += v.inc * (1.0f + 0.6f * v.cutE * v.fenv); if (v.phase >= 1.0f) v.phase -= 1.0f;
              v.mphase += v.minc; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
              v.f1 += 0.22f * (n - v.f1);                    // the snap, highpassed
              v.env *= v.eDc; v.env2 *= v.eRc;
              s0 = tone * v.env * (1.1f - 0.6f * v.lfB) + (n - v.f1) * 1.4f * v.lfB * v.env2;
              break;
            }
            case DM_CLAP: {
              if (v.dmT == 480 || v.dmT == 960) v.env2 = 1.0f;   // three bursts, ~11 ms apart
              v.f1 += 0.32f * (n - v.f1);
              v.f2 += 0.07f * (n - v.f2);
              s0 = (v.f1 - v.f2) * 2.6f * v.env2;
              v.env2 *= v.dmT < 1400 ? 0.994f : v.eDc;
              v.env = v.env2;                                // (the quiet check reads env)
              break;
            }
            case DM_HAT:
            case DM_CYM: {
              float sq1 = v.phase < 0.5f ? 1.0f : -1.0f, sq2 = v.mphase < 0.5f ? 1.0f : -1.0f;
              v.phase += v.inc; if (v.phase >= 1.0f) v.phase -= 1.0f;
              v.mphase += v.minc; if (v.mphase >= 1.0f) v.mphase -= 1.0f;
              float metal = 0.7f * sq1 * sq2 + n * v.lfB;
              v.f1 += (0.10f + 0.3f * v.lfAl) * (metal - v.f1);
              s0 = metal - v.f1;                             // highpassed ring
              if (v.cat1 == DM_CYM) { v.f2 += 0.035f * (metal - v.f2); s0 += 0.3f * (metal - v.f2); }
              v.env *= v.eDc;
              s0 *= 0.8f * v.env;
              break;
            }
            default: {                                       // DM_RIM
              s0 = 1.2f * sinT(v.phase);
              if (v.dmT < 24) s0 += 0.8f * n;
              v.phase += v.inc; if (v.phase >= 1.0f) v.phase -= 1.0f;
              v.env *= v.eDc;
              s0 *= v.env;
              break;
            }
          }
          v.dmT++;
          s = s0;
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
      if (d.osc != O_M1 && d.osc != O_DM && !(d.osc == O_GM && v.pcm))
        { v.phase += v.inc; if (v.phase >= 1.0f) v.phase -= 1.0f; }
      tbus[v.track][i] += (d.osc == O_M1 || d.osc == O_DM) ? s * amp : s * v.env * amp;
    }
    bool quiet = v.env < 0.002f && (!v.dual || v.env2 < 0.002f);
    if (quiet && v.state != ATTACK) { v.state = IDLE; active--; }
  }
  miRender(tbus, AUDIO_BLOCK_SAMPLES);
  _active = active + miActive();

  static float outL[AUDIO_BLOCK_SAMPLES], outR[AUDIO_BLOCK_SAMPLES];
  memset(outL, 0, sizeof outL); memset(outR, 0, sizeof outR);
  fxBeginBlock(AUDIO_BLOCK_SAMPLES);
  for (uint8_t t = 0; t < 9; t++) {
    float g = trackMute[t] ? 0.0f : (trackVol[t] / 80.0f) * (trackVol[t] / 80.0f);
    fxTrackChain(t, tbus[t], AUDIO_BLOCK_SAMPLES, g);   // inserts + post-fader send taps
    float pn = (trackPan[t] - 50) / 50.0f;              // the strip: volume, pan, meter
    float gl = g * sqrtf(0.5f * (1.0f - pn)), gr = g * sqrtf(0.5f * (1.0f + pn));
    float pk = 0;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      float x = tbus[t][i];
      outL[i] += x * gl; outR[i] += x * gr;
      float a = fabsf(x) * g; if (a > pk) pk = a;
    }
    int m = (int)(pk * 255.0f); if (m > 255) m = 255;
    _tpk[t] = m > _tpk[t] ? m : (uint8_t)(_tpk[t] * 0.88f);
  }
  fxRenderSends(outL, outR, AUDIO_BLOCK_SAMPLES);       // the three send effects return here
  fxProcessMaster(outL, outR, AUDIO_BLOCK_SAMPLES);     // the master's stereo effect
  float gm = (masterVol / 80.0f) * (masterVol / 80.0f); // master volume, then the clipper
  float pkl = 0, pkr = 0;
  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
    float sl = fxSoftClip(outL[i] * gm), sr = fxSoftClip(outR[i] * gm);
    if (fabsf(sl) > pkl) pkl = fabsf(sl);
    if (fabsf(sr) > pkr) pkr = fabsf(sr);
    l->data[i] = (int16_t)(sl * 32767.0f);
    r->data[i] = (int16_t)(sr * 32767.0f);
  }
  int ml = (int)(pkl * 255.0f); if (ml > 255) ml = 255;
  int mr = (int)(pkr * 255.0f); if (mr > 255) mr = 255;
  _pkL = ml > _pkL ? ml : (uint8_t)(_pkL * 0.88f);
  _pkR = mr > _pkR ? mr : (uint8_t)(_pkR * 0.88f);
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
  if (curEngine() == E_GM) gmProgram[nav.gridT] = p & 127;
  else if (curEngine() == E_DM) drumMachine[nav.gridT] = p & 3;
  else if (curEngine() == E_MWAVE) mwSetProgram(p);
  else m1SetProgram(p);
}

// Drum tracks take notes as played: the scale would wreck their maps.
static uint8_t snapIn(uint8_t n) {
  uint8_t e = trackEngine[nav.gridT];
  return (e == E_GMDRUM || e == E_DM) ? n : scaleSnap(n);
}

void synthServiceMidi() {
  while (usbMIDI.read()) {
    if (usbMIDI.getChannel() != 1) continue;
    uint8_t type = usbMIDI.getType();
    if (type == usbMIDI.NoteOn && usbMIDI.getData2() > 0) synth.noteOn(snapIn(usbMIDI.getData1()), usbMIDI.getData2());
    else if (type == usbMIDI.NoteOff || type == usbMIDI.NoteOn) synth.noteOff(snapIn(usbMIDI.getData1()));
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
      if (status == 0x90 && b > 0) synth.noteOn(snapIn(d1), b);
      else synth.noteOff(snapIn(d1));
      d1 = 255;                                   // (running status kept)
    }
  }
}
