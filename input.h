// input.h — the keys: navigation, editing the selected cell, transport, and
// moving the selection on after a recording.
#pragma once

void pollKeys();                         // call from loop(): reads the keys, repeats held arrows
void followRecording();                  // call from loop(): selects the next track after a recording
