// input.h — the keys: navigation, editing the selected cell, transport, and
// moving the selection on after a recording.
#pragma once

void pollKeys();                         // call from loop(): reads the keys, repeats held arrows
void followRecording();
bool optBeforeShift();                   // SHIFT and B held: B was pressed first
bool screenSwitchPending();              // an arrow is held that goes to the other screen once let go (the edge marks show it)
