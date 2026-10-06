// input.h — the keys: navigation, editing the selected cell, transport, and
// moving the selection on after a recording.
#pragma once

void pollKeys();                         // call from loop(): reads the keys, repeats held arrows
void followRecording();
bool menuOpensOnRelease();               // letting go of the key held now opens the menu
bool optBeforeShift();                   // SHIFT and B held: B was pressed first
