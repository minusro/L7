// input.h — the keys: navigation, editing the selected cell, transport, and
// moving the selection on after a recording.
#pragma once
#include <stdint.h>

void pollKeys();                         // call from loop(): reads the keys, repeats held arrows
void followRecording();
bool optBeforeShift();                   // SHIFT and B held: B was pressed first
bool screenSwitchPending();              // an arrow is held that goes to the other screen once let go (the selection frame hidden meanwhile)
uint8_t screenSwitchKey();               // that arrow (0: none); LEFT into the menu / RIGHT out of it: the edge marks show it
