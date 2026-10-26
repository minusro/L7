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
| **Serial MIDI**, 31 250 baud | port 1: Serial7 (RX 28/TX 29); port 2: Serial1 (RX 0/TX 1) | port 1: Serial1 (RX 0/TX 1); port 2: Serial3 (TX 14. RX 15 is the key INT) | confirmed. MIDI in is read on variant B's port 1 RX (pin 0, the port opened receive-only and pin 1 put back HIGH); that the MIDI in jack is wired there is not yet confirmed on hardware |
| Headphone sense (analog, INPUT_PULLDOWN, low-pass filtered) | pin 26 (A12) | pin 22 (A8) | variant B confirmed on hardware: `analogRead` ≈ 10 with headphones plugged in, ≈ 580 without; A inferred |
| Speaker amp enable (OUTPUT, HIGH at init) | pin 30 | pin 9 | variant B confirmed on hardware: HIGH = speaker on, LOW = speaker off; A inferred |
| **Charger** | no I²C charger; status pins 14 and 15 (INPUT_PULLUP); battery voltage on pin 22 (A8); pin 24 input | **TI BQ2589x** @0x6A on Wire; pin 2 INPUT (probably charger INT) | chips confirmed, pin roles inferred |
| **Fuel gauge** | n/a | **ST STC3115/3117** @0x70 on Wire | confirmed (register map matches). `batteryPercent()` reads its REG_SOC while REG_MODE's GG_RUN is set (never writes it), and REG_CURRENT (signed; positive while charging: +440 raw at 4.21 V, 97 %, USB in) for `batteryCharging()`; else the charger's REG0E (BATV: 2.304 V + 20 mV a step), mapped to a charge roughly. Measured 2026-10-09: the gauge running (mode 0x10); the charger's REG0B read 0xFF and REG0E didn't answer, so the charger isn't relied on. Logged on serial as it changes, and every 30 s (`batteryLog()`) |
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

# Dirtywave M8 Model:02 hardware map

Recovered by static analysis of `M8_V6_6_3C_MODEL02.bin` (the stock Model:02 firmware, V6.6.3C), read side by side
with the V6.6.3C Model:01 image. Nothing of it has been checked on a device: there is no Model:02 here.
**Confirmed** means read straight from the code or data; **inferred** means the code path is certain but what the
pin is for, or which bus object an address is, is my best reading.

How it was done, so it can be redone: the Teensy core's `pinMode` / `digitalWrite` / `analogWriteFrequency` were
found in the Model:01 image (its own pin-17 variant read is a known anchor), their bytes lifted with the pin-count
immediate masked, and matched in the Model:02 image; then every `bl` to them was decoded for the pin in `r0`.
The board-variant byte lives at RAM `0x2001382C` in both images; every literal reference to it was classified, and
the sites that compare it with 2 enumerate everything that is Model:02's own.

## MCU: a Teensy MicroMod

| Item | Value | Confidence |
|---|---|---|
| Board | **Teensy MicroMod** (i.MX RT1062, 16 MB flash). Build target `teensy:avr:teensyMM` | the pin count confirmed; the board inferred from it |
| Evidence | the core's pin-range checks are `cmp r0, #45` (46 pins) where the Model:01 image has `cmp r0, #39` (40); every other compare in the two images' histograms is identical. `cores/teensy4/core_pins.h` has `CORE_NUM_DIGITAL 46` only under `ARDUINO_TEENSY_MICROMOD` (4.0: 40, 4.1: 55) | confirmed |
| Layout | ITCM code from flash `0x60001680` to `0x0`; `.data` from flash `0x601DE9C0` to `0x20000000` | confirmed |

The pin numbers below are MicroMod pin numbers as the firmware uses them. For 0–39 they carry the same peripheral
functions as on a Teensy 4.0 (SPI on 10–13, I²C on 18/19, SAI1 on 7/8/20/21/23), so the Model:01 names still read.

## One board, hard-coded as "variant 2"

The shared code keeps the Model:01's two variants and a third. The Model:01 reads pin 17 to pick 0 or 1; the
Model:02 image replaces that routine with `movs r3, #2; strb r3, [r0]` (`0x600A664C`, called from board-init
`0x600828FE`). So on a Model:02 every `variant == 2` branch is live, and everything that only tests zero against
non-zero follows **Model:01 variant B**. That is most of the board: keys, charger, gauge, headphone sense, speaker
enable, MIDI ports and the backlight pin are variant B's. What is Model:02's own is the display, a few lines at
boot, and the touch controller's chip ID.

## Pin map

| Function | Model:02 | Confidence |
|---|---|---|
| **Display** ILI9488 480×320, SPI (LPSPI4) | MOSI 11, SCK 13, MISO 12 (the driver's own pin fields are 255: the port's defaults; the constructor stores `&SPI`, `0x2000A8A0`), **DC 10, RST 5**, the CS field 10 as well (see below) | confirmed (the fields); the DC/CS reading inferred |
| Display SPI clock | **56 MHz** (`begin()` is passed `0x03567E00`) | confirmed |
| Display rotation | `setRotation(3)` | confirmed |
| Pixel format | 18 bits (`COLMOD 0x66`), 3 bytes a pixel; see "What the 18-bit format means" | confirmed |
| **Backlight** PWM | **pin 4**, `analogWriteFrequency(4, 44100)`, LOW at init (variant B's; variant 0's pin 6 at 261 Hz is the Model:01 A) | confirmed |
| **Keys** (8) | **SX1508 @0x20 on `Wire`** (object `0x2000AAC0`, the one variant B uses: SDA 18 / SCL 19), **INT pin 15** `INPUT_PULLUP`; the key-init only tests zero/non-zero, no variant-2 branch | confirmed (path and pin); the bus's pins inferred |
| **Touch** | FocalTech @0x38 on `Wire`, probed for chip ID **0x36** (variant B probes 0x06), **INT pin 16** `INPUT_PULLDOWN` (variant 0: pin 9) | confirmed |
| **Audio** I²S, SAI1 | MCLK 23, BCLK 21, LRCLK 20, TX 7, RX 8; no codec I²C — the Model:01's shared path, not re-read | inferred |
| **SD card** | SDIO / USDHC1 (`BUILTIN_SDCARD`) — shared path, not re-read | inferred |
| Headphone sense | **pin 22** `INPUT_PULLDOWN` (variant 0: 26) | confirmed |
| Speaker amp enable | **pin 9** `OUTPUT` (variant 0: 30) | confirmed |
| **Charger / fuel gauge** | variant B's: BQ2589x @0x6A and STC3115 @0x70 on `Wire`, charger INT **pin 2** `INPUT` | inferred from the shared path |
| Model:02-only at charger init | **pin 40** `OUTPUT`, driven LOW for 10 ms then HIGH (`0x6007CE2C`–`0x6007CE46`): a reset or enable for one of the power parts | pin confirmed, purpose inferred |
| Model:02-only at board-init | **pin 28** `OUTPUT` set HIGH, and **pin 6** `OUTPUT` set HIGH then LOW after 1 ms (`0x60082926`–`0x6008294E`); the Model:01's early-boot **pin 1** `OUTPUT` HIGH is kept too (`0x60082908`, `0x6007BF52`) | pins confirmed, purposes inferred |
| **Serial MIDI** | variant B's: port 1 `Serial1` (RX 0 / TX 1), port 2 `Serial3` (TX 14; RX 15 is the key INT) — shared path, not re-read | inferred |
| USB "connected" | read from USB1 `PORTSC1` bit 0 (variant 2 only; the others use bit 7 inverted) | confirmed |
| Random seed | `analogRead` pin 27 (`INPUT`) | confirmed |

**DC and CS both 10.** The display-init routine (`0x60079388`) writes `rst 5, cs 10, dc 10` into the display
object on the non-zero path, where the Model:01 image writes `dc 6` on the same path — so Dirtywave's Model:02
build changed that one constant. Pin 10 is LPSPI4's PCS0, and an ILI9488_t3-style `begin()` checks
`pinIsChipSelect(dc)` and, when it is one, drives D/C through the hardware chip-select; the `pinMode(cs, OUTPUT)`
on the same pin is then overridden when the pad is handed to the SPI block. The reading that fits is: **D/C on
pin 10 as hardware PCS, no CS line to the panel** (tied). Unverified: if a Model:02 shows nothing, this is the
first thing to look at.

## The display's init sequence (the live one)

`begin()` is passed a table at RAM `0x20003C2C`; the command list sits `0x200` bytes into it (file `0x1E27EC`).
This is the one the panel is set up with; the ILI9488 driver's stock table is also in the image (`0x1DB128`,
`.rodata`) but this path does not use it. Verbatim (`command: data`):

| Cmd | Data | What |
|---|---|---|
| `C0` | `01 1f` | power control 1 |
| `C1` | `40` | power control 2 |
| `C2` | `00` | power control 3 |
| `C5` | `00 48 80` | VCOM control |
| `36` | `48` | MADCTL |
| `3A` | `66` | COLMOD: 18 bits a pixel |
| `B0` | `00` | interface mode control |
| `B1` | `c0` | frame rate control |
| `B4` | `02` | display inversion control |
| `B6` | `02 02` | display function control |
| `E0` | `00 08 0c 02 0e 04 30 45 47 04 0c 0a 2e 34 0f` | positive gamma |
| `E1` | `00 11 0d 01 0f 05 39 36 51 06 0f 0d 33 37 0f` | negative gamma |
| `E9` | `00` | set image function |
| `F7` | `a9 51 2c 82` | adjust control 3 |
| `F8` | `21 07` | adjust control 4 |

## What the 18-bit format means for a port

**The ILI9488 has no 16-bit mode over SPI.** L7 draws in RGB565 throughout and hands the display `uint16_t`
buffers (`writeRect()`, the row buffers in `ui_splash.cpp` and `ui_help.cpp`, the cell buffers in `ui_mix.cpp` and
`ui_filter.cpp`); none of that reaches an ILI9488 unchanged. A port needs a driver that expands each pixel to 3
bytes on the wire — half as much again to push, at 56 MHz against the Model:01's 70 — which is the thing to
measure first, since the screen already shares its time with SD streaming. `ILI9341_t3n` does not drive this
part; `ILI9488_t3` is the usual one.

## What a board layer needs, then

Less than it looked: build for `teensy:avr:teensyMM`, and take `M8Board`'s variant B as the starting point —
keys, touch (chip ID 0x36), charger, gauge, headphone sense, speaker enable, MIDI, audio and card are its. On top
of that: the ILI9488 driver at 56 MHz with D/C on 10 and RST on 5, rotation 3 and the table above; the backlight
on pin 4 at 44.1 kHz (as B); pin 1 held HIGH from boot; at board-init pin 28 HIGH and pin 6 pulsed; at charger init
pin 40 pulsed LOW. And the RGB666 path under the drawing code.

## Where to look in the Model:02 image (6.6.3C; flash = `0x60000000` + file offset)

| What | Address |
|---|---|
| `pinMode` / `digitalWrite` / `delay` / `analogWriteFrequency` | `0x600AE86C` / `0x600AE818` / `0x600AE770` / `0x600AEF40` |
| Board-variant byte / its setter / board-init | `0x2001382C` (RAM) / `0x600A664C` / `0x600828DC` |
| Display object / constructor / its static-init | `0x2001D230` (RAM) / `0x6008F0A4` / `0x600A65F0` |
| Display init + backlight / `begin()` / `setRotation` | `0x60079324` / `0x6008E508` / `0x6008DF28` |
| Display init table | RAM `0x20003C2C` (+`0x200`), file `0x1E25EC` |
| Key init / touch init | `0x6007B9F4` / `0x6007BEF4` |
| Charger init (pins 2, 40) / headphone + speaker (22, 9) | `0x6007CE16` / `0x6007D2E0` |
| `.data` load / ITCM load | flash `0x601DE9C0` → `0x20000000` / flash `0x60001680` → `0x0` |

## About the Model:01 section's "third code path (pins 28/40)"

That note above is right that the Model:01 image carries a third variant: the `variant == 2` branches are in it
too (17 sites). But pins 28 and 40 are not that variant's display pins — they are the power/reset lines in the
shared init code listed above — and the Model:01 image has no ILI9488 table and writes the Model:01's own display
pins on that path, so it was never a complete Model:02 configuration. The Model:02 image is.
