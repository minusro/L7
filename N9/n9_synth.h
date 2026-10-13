// n9_synth.h — N9's first sound: 32 voices shared across the engines, played
// from MIDI channel 1 (USB and serial). A placeholder for the real per-track
// engines: one engine at a time (nav.engine), fixed envelopes, no modulation.
//
// Engines (ENGINE_NAMES, n9_state.h):
//   0 SAW    naive sawtooth through a one-pole lowpass
//   1 PULSE  naive square through the same lowpass
//   2 FM2    two-operator FM, 2:1, the index easing off after the attack
//   3 PLUCK  two-operator FM, 1:1, a fast-decaying index and a plucked decay
//            (no sustain: the note rings down on its own)
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
    float mphase = 0, minc = 0;      // modulator
    float mindex = 0;                // FM index, decaying
    float env = 0, lp = 0;
    float vel = 0;
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
