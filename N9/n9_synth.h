// n9_synth.h — N9's first sound: 32 voices shared across the engines, played
// from MIDI channel 1 (USB and serial). A placeholder for the real per-track
// engines: one engine at a time (nav.engine), fixed envelopes, no modulation.
//
// The full lineup is in the selector (ENGINE_NAMES, n9_state.h); the sounds
// behind it are simple stand-ins so far (ENGINES in n9_synth.cpp): saw /
// pulse / three detuned saws / 2-op FM / noise, each through a one-pole
// lowpass, with per-engine envelope settings. The emulations (303, JUNO,
// PRO5, MWAVE, M1) and FM4/WT/PLUCK-as-string are still to be written.
#pragma once
#include <Audio.h>

class N9Synth : public AudioStream {
public:
  N9Synth() : AudioStream(0, nullptr) {}
  void update() override;

  // Called from loop(); the audio interrupt only reads what these set whole.
  void noteOn(uint8_t note, uint8_t vel);
  void noteOff(uint8_t note);

  uint8_t active() const { return _active; }       // voices sounding
  // The last audio block's cost in CPU cycles (max since last asked).
  uint32_t takeMaxCycles() { uint32_t c = _maxCycles; _maxCycles = 0; return c; }

private:
  enum : uint8_t { IDLE, ATTACK, HOLD, RELEASE };
  struct Voice {
    volatile uint8_t state = IDLE;
    uint8_t note = 0, engine = 0;
    bool accent = false;             // 303: velocity >= 100
    // M1: the program's parameters, cached at noteOn (n9_m1_bank.h)
    uint8_t cat1 = 0, cat2 = 0, mseg = 0;
    bool dual = false;
    float eAc = 0, eDc = 0, eSc = 0, eRc = 0;       // VDA EG coefficients
    float lAtk = 1, lBreak = 0, lSus = 0;           // and its levels
    float cutB = 0, cutE = 0, fSus = 0, fdC = 1;    // VDF: base, env amount, sustain, decay
    float midxC = 1;                                // FM stand-ins: index decay; MWAVE: wave-env rise
    float wStart = 0, resK = 0;                     // MWAVE: wave start position, filter resonance
    // M1 extended: VDF EG, osc2's own VDA EG, pitch EG, the two MGs
    uint8_t fseg = 0, mseg2 = 0, pseg = 0, mgWave = 0, fmgWave = 0;
    float fAc = 0, fDc = 0, fSc = 0, fRc = 0;       // VDF EG coefficients
    float lfAl = 0, lfB = 0, lfS = 0, lfRl = 0;     // and its levels (signed)
    float env2 = 0, e2Ac = 0, e2Dc = 0, e2Sc = 0, e2Rc = 0;   // VDA2 EG
    float l2Atk = 1, l2Break = 0, l2Sus = 0, lvl2 = 0;
    float pAc = 0, pDc = 0, pRc = 0;                // pitch EG (block rate)
    float pAtkL = 0, pRelL = 0, penv = 0;
    float lfoPh = 0, lfoInc = 0, vibAmt = 0;        // pitch MG
    float fmgPh = 0, fmgInc = 0, fmgAmt = 0;        // VDF MG
    float kbdCut = 0;                               // cutoff offset from the keyboard
    float phase = 0, inc = 0;        // carrier, 0..1 per sample
    float tinc = 0;                  // 303: the slide's target increment
    float mphase = 0, minc = 0;      // FM modulator / SUPER's second saw
    float p3 = 0, inc3 = 0;          // SUPER's third saw
    float mindex = 0;                // FM index, decaying
    float f1 = 0, f2 = 0, f3 = 0, f4 = 0, fenv = 0;   // 303: ladder stages + filter env
    float env = 0, lp = 0;
    float vel = 0;
    uint32_t rng = 1;                // the NOISE engine's generator
    uint32_t age = 0;                // for stealing: noteOn order
  };
  Voice _v[32];
  uint32_t _ageCounter = 0;
  volatile uint8_t _active = 0;
  volatile uint32_t _maxCycles = 0;
};

extern N9Synth synth;

// Reads MIDI (USB + the serial in port) and plays channel 1. Call every loop.
void synthServiceMidi();
void synthMidiBegin();

// The M1 engine's program (0-99): MIDI program change and the INST screen's
// preset cell both go through these.
uint8_t m1Program();
void m1SetProgram(uint8_t p);

// The Microwave engine's sound (0-255), likewise.
uint8_t mwProgram();
void mwSetProgram(uint8_t p);
