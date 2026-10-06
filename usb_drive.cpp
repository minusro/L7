// usb_drive.cpp — see usb_drive.h
#include "usb_drive.h"
#include "ui.h"
#include "storage.h"

#if defined(USB_SERIAL_MSC)
#include "usb_msc.h"                               // (the patched core's: tools/teensy_msc)
#elif defined(USB_MTPDISK) || defined(USB_MTPDISK_SERIAL)
#include <MTP_Teensy.h>
#endif

namespace looper {

void (*usbDriveTick)() = nullptr;
void (*usbDriveReload)() = nullptr;

// The screen: grey (SCREEN), its title and logo light grey (then the hue
// cycle), the connection's state under it white, the hint (and the messages
// that replace it) black; all left-aligned with the menu's list, the hint
// in two lines ("PRESS ANY KEY" / "TO DISCONNECT").
static constexpr uint16_t SCREEN = DIM;
static const char HINT[] = "PRESS ANY KEY", HINT2[] = "TO DISCONNECT";   // (two lines)
// The lines: the title, the connection's state 32 px under it, the hint 56 px
// further down (its second line 16 px under its first), centred together top
// to bottom (on the block grid), left-aligned with the menu's list.
static constexpr int STATE_DY = 32, HINT_DY = STATE_DY + 56, HINT2_DY = HINT_DY + FONT_H + 3 * PX;
static constexpr int TITLE_Y = (240 - (HINT2_DY + FONT_H)) / 2 / PX * PX;
static constexpr int STATE_Y = TITLE_Y + STATE_DY, HINT_Y = TITLE_Y + HINT_DY, HINT2_Y = TITLE_Y + HINT2_DY;
static int textX() { return menuListX(); }

static void line(int y, const char *text, uint16_t ink) {
  board.tft().fillRect(0, y, 320, FONT_H, SCREEN);
  drawText(textX(), y, text, ink, SCREEN);
}

// The hint's place: the hint (two lines), or a message in its first line's place.
static void hint(const char *text = nullptr, uint16_t ink = BG) {
  line(HINT_Y, text ? text : HINT, ink);
  line(HINT2_Y, text ? "" : HINT2, ink);
}

// The title and the corner logo (where and as big as the menu's), in one colour (light grey until the
// computer is connected, then the hue cycle); drawn over
// themselves, no clearing first: no flicker.
static void titleAndLogo(uint16_t c) {
  drawText(textX(), TITLE_Y, "STORAGE MODE", c, SCREEN);
  drawSmallLogo(PX, STATUS_TOP, c);
}

#if defined(USB_SERIAL_MSC)
extern "C" volatile uint8_t usb_configuration;

// The card, sector by sector, as the computer sees it.
static uint32_t cardSectors() { return SD.sdfs.card()->sectorCount(); }
static int cardRead(uint32_t lba, uint8_t *dst, uint32_t n) { return SD.sdfs.card()->readSectors(lba, dst, n); }
static int cardWrite(uint32_t lba, const uint8_t *src, uint32_t n) { return SD.sdfs.card()->writeSectors(lba, src, n); }
static void cardSync() { SD.sdfs.card()->syncDevice(); }
static const usb_msc_media_t MEDIA = {cardSectors, cardRead, cardWrite, cardSync};

// Outside storage mode (no medium): polled from a timer, so the computer's
// probing is always answered (each answer is only queued: microseconds).
static IntervalTimer pollTimer;
void usbDriveBegin() {
  pollTimer.priority(160);                          // (under the audio and the USB itself)
  pollTimer.begin([] { usb_msc_poll(); }, 2000);
}

[[noreturn]] static void restart(const char *why) {
  Serial.printf("storage mode: %s, restarting\n", why);
  cardSync();
  hint(why);
  delay(300);
  SCB_AIRCR = 0x05FA0004;                           // restart: the computer may have changed anything
  for (;;) {}
}

// Back from the computer: the card is the app's again, and everything the app
// knew of it is read again (the computer may have changed any file). Without a
// reload function: a restart.
static bool leave(const char *why, uint16_t ink = BG) {
  Serial.printf("storage mode: %s\n", why);
  cardSync();
  if (!usbDriveReload) restart(why);
  usb_msc_set_media(nullptr);                       // (an empty card reader again, answered from the timer)
  usb_msc_clear_host_state();
  pollTimer.begin([] { usb_msc_poll(); }, 2000);
  hint(why, ink);
  uint32_t shownAt = millis();
  usbDriveReload();
  while (millis() - shownAt < 400) {}               // (the message stays readable)
  board.tft().fillScreen(BG);
  uiGeneration++;
  return true;
}

bool usbDriveMode() {
  // Everything the app holds is written out; the card is the computer's now.
  if (recording.mode != Recording::Idle) return false;
  transportStop();
  flushHeaders();
  saveSessionNow();
  flushScenes();

  auto &t = board.tft();
  t.fillScreen(SCREEN);
  line(TITLE_Y, "STORAGE MODE", LIGHT);
  drawSmallLogo(PX, STATUS_TOP, LIGHT);
  hint();
  uiGeneration++;

  pollTimer.end();                                  // (polled from here from now on: reads and writes wait for the card)
  usb_msc_set_media(&MEDIA);
  Serial.println("storage mode: started");

  int shown = -1;
  uint32_t hueAt = 0;
  uint16_t hue = LIGHT;
  while (board.keys() & (m8::BTN_EDIT | m8::BTN_OPT | m8::BTN_PLAY)) {   // (the press that came here, let go first)
    if (usbDriveTick) usbDriveTick();
    usb_msc_poll();
  }
  for (;;) {
    if (usbDriveTick) usbDriveTick();
    usb_msc_poll();
    static uint32_t seenCmds = 0;
    if (usb_msc_commands != seenCmds) {             // (serial log: the computer's commands, except the reads, writes and polls)
      seenCmds = usb_msc_commands;
      if (usb_msc_last_op != 0x28 && usb_msc_last_op != 0x2A && usb_msc_last_op != 0x00)
        Serial.printf("storage mode: command %02X\n", usb_msc_last_op);
    }
    if (usb_msc_ejected()) return leave("EJECTED");
    int state = !usb_configuration ? 0 : usb_msc_host_seen() ? 2 : 1;
    if (millis() - hueAt >= 40) {                   // the title and logo: light grey, and once connected the hue goes round, once in 4 s
      hueAt = millis();
      uint16_t c = state == 2 ? hueCycle((millis() % 4000) / 4000.0f) : LIGHT;
      if (c != hue) {
        hue = c;
        titleAndLogo(c);
      }
    }
    if (state != shown) {
      shown = state;
      static const char *const TEXT[3] = {"CONNECT A USB CABLE", "WAITING FOR THE COMPUTER", "CONNECTED"};
      line(STATE_Y, TEXT[state], INK);
      Serial.printf("storage mode: %s\n", TEXT[state]);
    }
    if (board.keys()) return leave("WAIT");          // any key: back to the app (ejecting on the computer does too)
  }
}
#elif defined(USB_MTPDISK) || defined(USB_MTPDISK_SERIAL)
extern "C" volatile uint8_t usb_configuration;

bool usbDriveMode() {
  // Everything the app holds is written out; the card is the computer's now.
  if (recording.mode != Recording::Idle) return false;
  transportStop();
  flushHeaders();
  saveSessionNow();
  flushScenes();

  auto &t = board.tft();
  t.fillScreen(SCREEN);
  line(TITLE_Y, "STORAGE MODE", LIGHT);
  drawSmallLogo(PX, STATUS_TOP, LIGHT);
  hint();
  uiGeneration++;

  Serial.println("storage mode: started");
  MTP.begin();
  MTP.addFilesystem(SD, "L7");

  int shown = -1;
  uint32_t hueAt = 0;
  uint16_t hue = LIGHT;
  while (board.keys() & (m8::BTN_EDIT | m8::BTN_OPT | m8::BTN_PLAY)) {   // (the press that came here, let go first)
    if (usbDriveTick) usbDriveTick();
    MTP.loop();
  }
  for (;;) {
    if (usbDriveTick) usbDriveTick();
    MTP.loop();
    int state = !usb_configuration ? 0 : MTP ? 2 : 1;
    if (millis() - hueAt >= 40) {                   // the title and logo: light grey, and once connected the hue goes round, once in 4 s
      hueAt = millis();
      uint16_t c = state == 2 ? hueCycle((millis() % 4000) / 4000.0f) : LIGHT;
      if (c != hue) {
        hue = c;
        titleAndLogo(c);
      }
    }
    if (state != shown) {
      shown = state;
      static const char *const TEXT[3] = {"CONNECT A USB CABLE", "WAITING FOR THE COMPUTER", "CONNECTED"};
      line(STATE_Y, TEXT[state], INK);
      Serial.printf("storage mode: %s\n", TEXT[state]);
    }
    if (board.keys()) {                           // any key: back to the app, restarted (the computer may have changed anything)
      hint("RESTARTING");
      delay(100);
      SCB_AIRCR = 0x05FA0004;
      for (;;) {}
    }
  }
}
#else
bool usbDriveMode() { return false; }   // (not built with a storage USB type)
#endif
#if !defined(USB_SERIAL_MSC)
void usbDriveBegin() {}
#endif

}  // namespace looper
