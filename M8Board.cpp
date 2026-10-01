// M8Board.cpp — see M8Board.h and HARDWARE.md
#include "M8Board.h"

namespace m8 {

static constexpr uint8_t EXPANDER_ADDR = 0x20;
static constexpr uint8_t VARIANT_DETECT_PIN = 17;

//                      CS  DC RST  BL  BL-Hz    rot  keyBus   INT  SDdet HPsense HPen  touchINT
const Pins PINS_A = {   10,  2,  3,  6,   261.0f,  3, &Wire1,    5,    4,    26,   30,    9 };
const Pins PINS_B = {   10,  6,  5,  4, 44100.0f,  1, &Wire,    15,   17,    22,    9,   16 };

// ILI9341_t3n with access to its protected command helpers, so we can apply
// the panel tuning the stock firmware uses on variant B.
class M8Display : public ILI9341_t3n {
public:
  using ILI9341_t3n::ILI9341_t3n;
  void sendCmd(uint8_t cmd, const uint8_t *data, uint8_t n) {
    beginSPITransaction(_SPI_CLOCK);
    if (n == 0) {
      writecommand_last(cmd);
    } else {
      writecommand_cont(cmd);
      for (uint8_t i = 0; i < n; i++) {
        if (i == n - 1) writedata8_last(data[i]);
        else            writedata8_cont(data[i]);
      }
    }
    endSPITransaction();
  }
};

bool Board::begin(uint32_t spiHz) {
  // --- 1. Detect board variant exactly like the stock firmware ---
  pinMode(VARIANT_DETECT_PIN, INPUT_PULLDOWN);
  delay(1);
  _variant = digitalRead(VARIANT_DETECT_PIN) ? VARIANT_A : VARIANT_B;
  _pins = (_variant == VARIANT_A) ? &PINS_A : &PINS_B;
  const Pins &p = *_pins;

  // --- 2. Early power-up sequence (mirrors stock setup()) ---
  if (_variant == VARIANT_B) {
    pinMode(1, OUTPUT);          // stock fw drives pin 1 HIGH first on variant B
    digitalWrite(1, HIGH);
    delay(30);
  }
  pinMode(p.backlight, OUTPUT);  // backlight off until the panel is initialised
  digitalWrite(p.backlight, LOW);

  Wire.begin();
  Wire.setClock(400000);
  if (_variant == VARIANT_A) {
    Wire1.begin();
    Wire1.setClock(400000);
  }

  if (_variant == VARIANT_B) initChargerB();

  pinMode(p.hpEnable, OUTPUT);   // stock fw sets this HIGH during audio/HP init
  digitalWrite(p.hpEnable, HIGH);
  pinMode(p.hpSense, INPUT_PULLDOWN);
  pinMode(p.sdDetect, INPUT_PULLUP);

  // --- 3. Keys ---
  pinMode(p.keyInt, _variant == VARIANT_A ? INPUT : INPUT_PULLUP);
  bool keysOk = (_variant == VARIANT_A) ? initMCP23008() : initSX1508();

  // --- 4. Display ---
  // MOSI 11, SCK 13, MISO 12 -> LPSPI4 ("SPI")
  M8Display *d = new M8Display(p.tftCS, p.tftDC, p.tftRST, 11, 13, 12);
  _tft = d;
  d->begin(spiHz);
  if (_variant == VARIANT_B) {
    applyVariantBPanelTuning();
    // The variant-B panel shows a negative image unless inversion is on
    // (observed on hardware; typical for IPS ILI9341 panels).
    d->invertDisplay(true);
  }
  d->setRotation(p.tftRotation);
  d->fillScreen(ILI9341_BLACK);

  // --- 5. Backlight on ---
  analogWriteFrequency(p.backlight, p.backlightPwmHz);
  setBacklight(200);

  return keysOk;
}

void Board::applyVariantBPanelTuning() {
  // Stock variant-B init table differs from ILI9341_t3n's default only in
  // these entries (recovered verbatim from the firmware image).
  static const uint8_t pwctr1[] = {0x16};
  static const uint8_t pwctr2[] = {0x00};
  static const uint8_t gamset[] = {0x00};
  static const uint8_t gmctrp1[] = {0x0F,0x3F,0x2D,0x0C,0x10,0x0A,0x2C,0xD5,
                                    0x22,0x0A,0x13,0x03,0x08,0x03,0x00};
  static const uint8_t gmctrn1[] = {0x00,0x00,0x12,0x04,0x0F,0x05,0x2C,0xA2,
                                    0x3F,0x05,0x0E,0x0C,0x37,0x3C,0x0F};
  M8Display *d = static_cast<M8Display *>(_tft);
  d->sendCmd(0xC0, pwctr1, 1);
  d->sendCmd(0xC1, pwctr2, 1);
  d->sendCmd(0x26, gamset, 1);
  d->sendCmd(0xE0, gmctrp1, sizeof gmctrp1);
  d->sendCmd(0xE1, gmctrn1, sizeof gmctrn1);
}

void Board::setBacklight(uint8_t level) { analogWrite(_pins->backlight, level); }

// ---------------- Power button (SNVS On/Off) ----------------
static constexpr uint32_t SNVS_LPSR_EO  = 1u << 17;  // emergency-off request
static constexpr uint32_t SNVS_LPSR_SPO = 1u << 18;  // set-power-off request (short press)
static constexpr uint32_t SNVS_HPSR_BTN = 1u << 6;   // live button value

bool Board::powerButtonPressed() {
  if (!(SNVS_LPSR & SNVS_LPSR_SPO)) return false;
  SNVS_LPSR = SNVS_LPSR_SPO | SNVS_LPSR_EO;  // write-1-to-clear, as stock does
  return true;
}

bool Board::powerButtonHeld() { return SNVS_HPSR & SNVS_HPSR_BTN; }

void Board::powerOff() {
  setBacklight(0);
  SNVS_LPCR |= SNVS_LPCR_DP_EN | SNVS_LPCR_TOP;  // stock sets TOP, then WFI forever
  asm volatile("dsb");
  while (true) asm volatile("wfi");
}

// Variant B: TI BQ2589x charger @0x6A on Wire. Replays the stock firmware's
// init sequence verbatim. Without this the chip runs on power-on defaults
// (2 A fast charge, 40 s I2C watchdog) whenever it has been reset, e.g.
// after the battery was disconnected or ship mode was used.
static bool bqWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x6A);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static int bqRead(uint8_t reg) {
  Wire.beginTransmission(0x6A);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)0x6A, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

bool Board::initChargerB() {
  delay(2);
  int r14 = bqRead(0x14);
  if (r14 < 0) return false;             // charger not answering
  bqWrite(0x14, (uint8_t)r14 | 0x80);    // REG_RST: registers to defaults
  delay(10);
  bqWrite(0x02, 0xE0);  delay(20);       // ADC continuous conversion on
  bqWrite(0x03, 0x36);  delay(10);       // CHG_CONFIG=1, OTG_CONFIG=1, SYS_MIN 3.3 V
  bqWrite(0x04, 0x08);                   // ICHG = 8 x 64 mA = 512 mA
  bqWrite(0x07, 0x8D);                   // EN_TERM, I2C watchdog OFF, 12 h safety timer
  bqWrite(0x09, 0x44);                   // (power-on default value)
  bqWrite(0x0A, 0x03);                   // boost settings as stock
  delay(20);
  return true;
}

// ---------------- I2C expander helpers ----------------
bool Board::writeReg(uint8_t reg, uint8_t val) {
  TwoWire &w = *_pins->keyBus;
  w.beginTransmission(EXPANDER_ADDR);
  w.write(reg);
  w.write(val);
  return w.endTransmission() == 0;
}

int Board::readReg(uint8_t reg) {
  TwoWire &w = *_pins->keyBus;
  w.beginTransmission(EXPANDER_ADDR);
  w.write(reg);
  if (w.endTransmission(false) != 0) return -1;
  if (w.requestFrom(EXPANDER_ADDR, (uint8_t)1) != 1) return -1;
  return w.read();
}

// Variant A: Microchip MCP23008 @0x20 on Wire1 (SDA 17 / SCL 16), INT -> pin 5
bool Board::initMCP23008() {
  bool ok = true;
  ok &= writeReg(0x05, 0x00);  // IOCON: INT active-low, push-pull (as stock)
  ok &= writeReg(0x00, 0xFF);  // IODIR: all inputs
  ok &= writeReg(0x06, 0xFF);  // GPPU: pull-ups on
  ok &= writeReg(0x04, 0x00);  // INTCON: interrupt on any change
  ok &= writeReg(0x02, 0xFF);  // GPINTEN: all pins
  readReg(0x09);               // read GPIO to clear pending INT
  return ok;
}

// Variant B: Semtech SX1508 @0x20 on Wire (SDA 18 / SCL 19), INT -> pin 15
bool Board::initSX1508() {
  bool ok = true;
  ok &= writeReg(0x07, 0xFF);  // RegDir: all inputs
  ok &= writeReg(0x03, 0xFF);  // RegPullUp
  ok &= writeReg(0x0F, 0x40);  // RegClock: internal 2 MHz oscillator
  ok &= writeReg(0x10, 0x10);  // RegMisc: clkX = fOSC/1 (needed for debounce)
  ok &= writeReg(0x12, 0x05);  // RegDebounceConfig: 16 ms (stock value)
  ok &= writeReg(0x13, 0xFF);  // RegDebounceEnable: all pins
  ok &= writeReg(0x0A, 0xFF);  // RegSenseHigh: both edges, pins 7..4
  ok &= writeReg(0x0B, 0xFF);  // RegSenseLow:  both edges, pins 3..0
  ok &= writeReg(0x09, 0x00);  // RegInterruptMask: all enabled
  writeReg(0x0C, 0xFF);        // clear RegInterruptSource
  return ok;
}

// ---------------- Touch panel (FocalTech FT5x/FT6x) ----------------
static constexpr uint8_t TOUCH_ADDR = 0x38;

static int touchReadRegs(TwoWire &w, uint8_t reg, uint8_t *buf, uint8_t n) {
  w.beginTransmission(TOUCH_ADDR);
  w.write(reg);
  if (w.endTransmission(false) != 0) return -1;
  if (w.requestFrom(TOUCH_ADDR, n) != n) return -1;
  for (uint8_t i = 0; i < n; i++) buf[i] = w.read();
  return n;
}

bool Board::touchBegin() {
  pinMode(_pins->touchInt, INPUT_PULLUP);
  uint8_t id;
  if (touchReadRegs(*_pins->keyBus, 0xA3, &id, 1) < 0) return false;   // chip ID
  _touchId = id;
  return true;
}

Board::Touch Board::touchRead() {
  Touch t = {false, 0, 0, 0, 0};
  uint8_t r[5];                                      // TD_STATUS, P1_XH, P1_XL, P1_YH, P1_YL
  if (!_touchId || touchReadRegs(*_pins->keyBus, 0x02, r, 5) < 0) return t;
  if ((r[0] & 0x0F) == 0) return t;
  t.down = true;
  t.rawX = ((r[1] & 0x0F) << 8) | r[2];              // panel-native portrait, 240 wide
  t.rawY = ((r[3] & 0x0F) << 8) | r[4];              // 320 tall
  // Map portrait panel coordinates onto the landscape screen (verified on a
  // variant-B unit, rotation 1; rotation 3 is assumed to be the mirror image).
  if (_pins->tftRotation == 1) { t.x = 319 - t.rawY; t.y = t.rawX; }
  else                         { t.x = t.rawY; t.y = 239 - t.rawX; }
  t.x = constrain(t.x, 0, 319);
  t.y = constrain(t.y, 0, 239);
  return t;
}

uint8_t Board::readExpander() {
  int v;
  if (_variant == VARIANT_A) {
    v = readReg(0x09);                   // MCP23008 GPIO
  } else {
    v = readReg(0x08);                   // SX1508 RegData
    writeReg(0x0C, 0xFF);                // clear interrupt source
  }
  if (v < 0) return _keys;               // bus error: keep last state
  return (uint8_t)~v;                    // keys pull to GND -> invert
}

uint8_t Board::keys() {
  // Only hit the bus when INT is asserted (low), or on the very first call.
  if (_first || digitalReadFast(_pins->keyInt) == LOW) {
    _keys = readExpander();
    _first = false;
  }
  return _keys;
}

bool Board::keysChanged(uint8_t &now) {
  now = keys();
  if (now == _lastReported) return false;
  _lastReported = now;
  return true;
}

} // namespace m8
