// k6_mi.h — the Mutable Instruments engines (vendored plaits in
// libraries/mi, MIT): a four-voice plaits pool behind two N9 engines.
// PLAITS exposes all 24 plaits models (the preset cell steps them); SPELL is
// the speech model pinned — the Speak & Spell flavoured LPC voice.
// Everything here runs in the audio interrupt (called from the synth).
#pragma once
#include <stdint.h>

constexpr uint8_t MI_MODELS = 24;
extern const char *const MI_MODEL_NAMES[MI_MODELS];

void miNoteOn(uint8_t track, uint8_t note, uint8_t vel, uint8_t model);
void miNoteOff(uint8_t track, uint8_t note);
void miRender(float (*bus)[128], int n);   // adds into the track buses
uint8_t miActive();

uint8_t miModel();
void miSetModel(uint8_t m);
