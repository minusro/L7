// input.h — the keys: navigation, editing the selected cell, transport, and
// moving the selection on after a recording.
#pragma once

void pollKeys();                         // call from loop(): reads the keys, repeats held arrows
void followRecording();
bool optBeforeShift();                   // SHIFT and B held: B was pressed first
bool shiftTwiceHeld();                   // SHIFT tapped twice, still held: A resamples / overdubs
// The menu's project slot whose cut (SHIFT+A) waits for a second A (SHIFT
// still held), or -1.
int cutConfirmSlot();
