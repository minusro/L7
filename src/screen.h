// screen.h — which M8 the build is for, and how big its screen is. The one
// place that knows: the layout (ui.h) is measured from these, and the host
// builds' stand-in displays (tools/*/stubs/M8Board.h) size their frame
// buffer from them too, so a preview shows what the device will.
#pragma once

// 1: Model:01, the ILI9341 320 x 240 (see docs/HARDWARE.md). 2: Model:02,
// 480 x 320 (the size the stock Model:01 firmware's unreachable third
// display path is built for). Pass -DM8_MODEL=2 to build for the other.
#ifndef M8_MODEL
#define M8_MODEL 1
#endif

#if M8_MODEL == 1
constexpr int SCREEN_W = 320, SCREEN_H = 240;
#elif M8_MODEL == 2
constexpr int SCREEN_W = 480, SCREEN_H = 320;
#else
#error "M8_MODEL: 1 for the Model:01, 2 for the Model:02"
#endif
