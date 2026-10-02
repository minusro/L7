# Dirtywave M8 Model:01 hardware map

Recovered by static analysis of `M8_V6_6_2A_MODEL01_TyUploader.hex` (firmware 6.6.2A).
Confidence: **confirmed** means the value is read directly from code or data. **Inferred** means the pin's
electrical setup is confirmed but its *purpose* is my best interpretation.

## MCU

| Item | Value | Confidence |
|---|---|---|
| Board | Teensy 4.0 (i.MX RT1062, 600 MHz). Its core pin table has exactly 40 entries | confirmed |
| Image | Plain `.hex`, not `.ehex`, so the Teensy is **not secure-locked** and accepts any firmware | confirmed |
| Layout | ITCM code 0x0000–0x2ABBF (loaded from flash 0x60001680), `.data` 0x20000000 (from 0x601D9260) | confirmed |

## Two board variants

At boot, `setup()` calls a routine (flash `0x600A03A8`) that does `pinMode(17, INPUT_PULLDOWN); delay(1)`,
then `variant = digitalRead(17) ? A : B`. On variant A, pin 17 is SDA of a populated `Wire1` bus, so its
pull-ups make the pin read HIGH. Every peripheral init below branches on this flag. The firmware also
contains a third code path (480×320 display, pins 28/40). That path is shared code from the Model:02
build and is unreachable on Model:01, because the flag can only be 0 or 1.

## Pin map

| Function | Variant A (pin 17 = HIGH) | Variant B (pin 17 = LOW) | Confidence |
|---|---|---|---|
| **Display** ILI9341 320×240, SPI (LPSPI4) | MOSI 11, SCK 13, MISO 12, **CS 10, DC 2, RST 3** | MOSI 11, SCK 13, MISO 12, **CS 10, DC 6, RST 5** | confirmed |
| Display rotation | `setRotation(3)` | `setRotation(1)` | confirmed |
| Display SPI clock | 70 MHz write / 2 MHz read | same | confirmed |
| Display init table | ILI9341_t3n default | custom table (PWCTR1=0x16, PWCTR2=0x00, GAMMASET=0x00, custom E0/E1 gamma) | confirmed |
| **Backlight** PWM | **pin 6**, `analogWriteFrequency` 261 Hz | **pin 4**, 44 100 Hz | confirmed |
| **Keys** (8) | **MCP23008** @0x20 on **Wire1** (SDA 17/SCL 16), INT → **pin 5** (INPUT) | **SX1508** @0x20 on **Wire** (SDA 18/SCL 19), INT → **pin 15** (INPUT_PULLUP) | confirmed |
| I²C buses in use | Wire + Wire1, both 400 kHz | Wire only, 400 kHz | confirmed |
| **Audio** I²S, SAI1 | MCLK 23, BCLK 21, LRCLK 20, TX 7, RX 8 | same | confirmed |
| Audio codec control | none: no codec I²C traffic exists, so the DAC/ADC are hardware-configured | same | confirmed |
| **SD card** | SDIO / USDHC1 on bottom pads 34–39 (`BUILTIN_SDCARD`) | same | confirmed |
| SD card detect | pin 4, INPUT_PULLUP, CHANGE IRQ | pin 17, INPUT_PULLUP, CHANGE IRQ | pin setup confirmed, purpose inferred |
| **Serial MIDI**, 31 250 baud | port 1: Serial7 (RX 28/TX 29); port 2: Serial1 (RX 0/TX 1) | port 1: Serial1 (RX 0/TX 1); port 2: Serial3 (TX 14. RX 15 is the key INT) | confirmed |
| Headphone sense (analog, INPUT_PULLDOWN, low-pass filtered) | pin 26 (A12) | pin 22 (A8) | variant B confirmed on hardware: `analogRead` ≈ 10 with headphones plugged in, ≈ 580 without; A inferred |
| Speaker amp enable (OUTPUT, HIGH at init) | pin 30 | pin 9 | variant B confirmed on hardware: HIGH = speaker on, LOW = speaker off; A inferred |
| **Charger** | no I²C charger; status pins 14 and 15 (INPUT_PULLUP); battery voltage on pin 22 (A8); pin 24 input | **TI BQ2589x** @0x6A on Wire; pin 2 INPUT (probably charger INT) | chips confirmed, pin roles inferred |
| **Fuel gauge** | n/a | **ST STC3115/3117** @0x70 on Wire | confirmed (register map matches) |
| Early boot line | n/a | pin 1 OUTPUT HIGH, then 30 ms delay. **Keep it HIGH**: using it as Serial1 TX (MIDI out) made the SD card reset repeatedly (command timeouts on every SD command) until the card was re-initialised. Likely a power enable | setup confirmed; effect observed on hardware |
| Optional touch panel | FocalTech FT5x/FT6x @0x38 on Wire1, chip ID 0x54, INT pin 9 | @0x38 on Wire, chip ID 0x06/0x36, INT pin 16 | confirmed probe; your unit probably has none |
| Random seed | `randomSeed(analogRead(A13 = pin 27))` | same | confirmed |

## Key expander details

The stock firmware keeps `~GPIO` from the expander as its key bitmask (bit *n* = expander pin GP*n*, pressed = 1).
The key reader only polls the expander when the INT pin is low. The names in `M8Board.h` assume the layout of the M8
headless remote-display protocol (bit 0 EDIT, 1 OPT, 2 RIGHT, 3 PLAY, 4 SHIFT, 5 DOWN, 6 UP, 7 LEFT).
**Check them on your unit with the demo.** It prints the raw bits.

- MCP23008 setup (variant A): IODIR=0xFF, GPPU=0xFF, IOCON INTPOL=0/ODR=0, INTCON=0x00, GPINTEN=0xFF; read GPIO (0x09).
- SX1508 setup (variant B): per pin INPUT_PULLUP (RegDir 0x07, RegPullUp 0x03) and debounce enable (0x13).
  Also RegClock 0x0F=0x40 (internal 2 MHz), debounce time 16 ms, RegInterruptMask 0x09=0x00, RegSense 0x0A/0x0B=0xFF.
  Keys are read from RegData (0x08), and RegInterruptSource (0x0C) is cleared with 0xFF.

## BQ2589x charger init (variant B), verbatim from stock

The driver starts by setting REG_RST (REG14 |= 0x80). It then writes:

| Reg | Value | Meaning |
|---|---|---|
| 0x02 | 0xE0 | ADC continuous conversion |
| 0x03 | 0x36 | CHG_CONFIG=1, OTG_CONFIG=1, SYS_MIN = 3.3 V |
| 0x04 | 0x08 | **ICHG = 512 mA**. The chip's power-on default is 2048 mA |
| 0x07 | 0x8D | charge termination on, **I²C watchdog disabled**, 12 h safety timer |
| 0x09 | 0x44 | default (BATFET_DIS is bit 5 of this register, which is what "BATT.SHIPMODE" uses) |
| 0x0A | 0x03 | boost settings |

`M8Board::begin()` replays this sequence. Keep it in any custom firmware for variant B.

## Where to look in the image (for further digging)

| What | Address |
|---|---|
| `setup()` | 0x60081544 |
| `pinMode` / `digitalWrite` / `digitalRead` | 0x600A9AD4 / 0x600A9A5C / 0x600A9AB0 |
| Pin-info table (`digital_pin_to_info_PGM`) | 0x200050C4 (RAM) |
| Board-variant detect | 0x600A03A8, result byte at 0x20013824 |
| Display object / begin / constructor | 0x2001D0AC / 0x6008C984 / 0x6008C51C |
| Display init path | 0x60077C4C, variant-B init table at 0x20003C78 |
| Key init / key read | 0x6007A302 / 0x6007A3D0 |
| Touch / headphone(?) init | 0x6007A7EC / 0x6007BB4C |
| Power / charger init | 0x6007B5E4 (BQ2589x writes at 0x6007B704) |
| STC3115 driver | 0x600900A0–0x60090540 |
| I²S (SAI1) setup, ITCM | 0x1DF00, 0x1DFC0, 0x218B4 |
| SD (USDHC1) pad setup, ITCM | 0x29538 |
