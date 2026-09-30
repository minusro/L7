// M8Board.h — minimal board-support layer for the Dirtywave M8 Model:01 (Teensy 4.0)
//
// Everything in here was recovered by disassembling the stock firmware
// M8_V6_6_2A_MODEL01. See HARDWARE.md for the evidence behind each value.
//
// The stock firmware supports two Model:01 board variants and tells them apart
// at boot by reading pin 17 with a pull-down:
//   pin 17 reads HIGH -> VARIANT_A (pin 17 is SDA of a populated Wire1 bus)
//   pin 17 reads LOW  -> VARIANT_B
// This library does the same, so one binary runs on either board.

#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <ILI9341_t3n.h>

namespace m8 {

enum Variant : uint8_t { VARIANT_A = 0, VARIANT_B = 1 };

// Key bitmask. The stock firmware stores ~expanderGPIO directly as its key
// state, so bit N == expander pin GPn. The names below follow the M8 headless
// remote-display protocol layout; confirm them on your unit with the demo
// sketch (it shows the raw bits on screen).
enum Button : uint8_t {
  BTN_EDIT  = 1 << 0,
  BTN_OPT   = 1 << 1,
  BTN_RIGHT = 1 << 2,
  BTN_PLAY  = 1 << 3,
  BTN_SHIFT = 1 << 4,
  BTN_DOWN  = 1 << 5,
  BTN_UP    = 1 << 6,
  BTN_LEFT  = 1 << 7,
};

struct Pins {
  // Display: ILI9341 320x240 on hardware SPI (LPSPI4: MOSI 11, SCK 13, MISO 12)
  uint8_t tftCS, tftDC, tftRST;
  uint8_t backlight;          // PWM
  float   backlightPwmHz;     // what the stock firmware sets
  uint8_t tftRotation;        // what the stock firmware sets
  // Keys: 8-bit I2C GPIO expander at 0x20
  TwoWire *keyBus;
  uint8_t keyInt;             // expander interrupt output (active low)
  // Misc (see HARDWARE.md for how certain each one is)
  uint8_t sdDetect;           // INPUT_PULLUP, HIGH = card removed
  uint8_t hpSense;            // analog input, INPUT_PULLDOWN (likely headphone detect)
  uint8_t hpEnable;           // driven HIGH at boot by stock fw (likely amp enable)
  uint8_t touchInt;           // optional FocalTech touch controller INT
};

extern const Pins PINS_A;
extern const Pins PINS_B;

class Board {
public:
  // Detects the variant, brings up I2C, keys, display and backlight.
  // Returns false if the key expander did not answer.
  bool begin(uint32_t spiHz = 30000000);   // stock firmware uses 70 MHz

  Variant variant() const { return _variant; }
  const Pins &pins() const { return *_pins; }
  ILI9341_t3n &tft() { return *_tft; }

  // Current key state (bit set = pressed). Cheap to call every loop;
  // only touches I2C when the expander's INT line says something changed.
  uint8_t keys();
  bool keysChanged(uint8_t &now);   // true once per change

  void setBacklight(uint8_t level);  // 0..255

private:
  bool initMCP23008();
  bool initSX1508();
  uint8_t readExpander();
  bool writeReg(uint8_t reg, uint8_t val);
  int  readReg(uint8_t reg);
  void applyVariantBPanelTuning();
  bool initChargerB();

  Variant _variant = VARIANT_A;
  const Pins *_pins = nullptr;
  ILI9341_t3n *_tft = nullptr;
  uint8_t _keys = 0, _lastReported = 0;
  bool _first = true;
};

} // namespace m8
