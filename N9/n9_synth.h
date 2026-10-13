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
    float phase = 0, inc = 0;        // carrier, 0..1 per sample
    float mphase = 0, minc = 0;      // FM modulator / SUPER's second saw
    float p3 = 0, inc3 = 0;          // SUPER's third saw
    float mindex = 0;                // FM index, decaying
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
