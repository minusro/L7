// screen.h — which M8 the build is for, and how big its screen is. The one
// place that knows: the layout (ui.h) is measured from these, and the host
// builds' stand-in displays (tools/*/stubs/M8Board.h) size their frame
// buffer from them too, so a preview shows what the device will.
#pragma once

// 1: Model:01, the ILI9341 320 x 240 on a Teensy 4.0. 2: Model:02, the
// ILI9488 480 x 320 on a Teensy MicroMod (docs/HARDWARE.md, its Model:02
// section). A sketch build takes it from the board it is built for, so
// -b teensy:avr:teensyMM is the Model:02; the host builds pass -DM8_MODEL=2.
#ifndef M8_MODEL
#if defined(ARDUINO_TEENSY_MICROMOD)
#define M8_MODEL 2
#else
#define M8_MODEL 1
#endif
#endif

#if M8_MODEL == 1
constexpr int SCREEN_W = 320, SCREEN_H = 240;
#elif M8_MODEL == 2
constexpr int SCREEN_W = 480, SCREEN_H = 320;
#else
#error "M8_MODEL: 1 for the Model:01, 2 for the Model:02"
#endif
