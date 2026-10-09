// input.h — the keys: navigation, editing the selected cell, transport, and
// moving the selection on after a recording.
#pragma once
#include <stdint.h>

void pollKeys();                         // call from loop(): reads the keys, repeats held arrows
void followRecording();
bool optBeforeShift();                   // SHIFT and B held: B was pressed first
bool clipRotating();                     // B then A held on a clip's waveform cell: its fine rotate (the arrows turn it)
int32_t clipRotatePending(int t);        // its turn while held (frames; not heard yet), track t's (0: none)
bool tempoNudging();                     // SHIFT+LEFT/RIGHT held on the status row, no external clock: the clock nudged faster / slower
bool screenSwitchPending();              // an arrow is held that goes to the other screen once let go (the selection frame hidden meanwhile)
uint8_t screenSwitchKey();               // that arrow (0: none); the looper's edge mark shows it
