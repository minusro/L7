// Stand-in for SdFat.h in the demo renderer (app_loader.h includes it): the
// desktop stand-ins' SD.h, and the Audio library's interrupt switches the
// app switch uses (no audio interrupt to hold off here).
#pragma once
#include "SD.h"
inline void AudioNoInterrupts() {}
inline void AudioInterrupts() {}
