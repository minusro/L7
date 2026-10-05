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

static void centred(int y, const char *text, uint16_t ink) {
  int w = textWidth(text);
  board.tft().fillRect(0, y, 320, FONT_H, BG);
  drawText((320 - w) / 2 / PX * PX, y, text, ink, BG);
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
  centred(176, why, LIGHT);
  delay(300);
  SCB_AIRCR = 0x05FA0004;                           // restart: the computer may have changed anything
  for (;;) {}
}

bool usbDriveMode() {
  // Everything the app holds is written out; the card is the computer's now.
  if (recording.mode != Recording::Idle) return false;
  transportStop();
  flushHeaders();
  saveSessionNow();
  flushScenes();

  auto &t = board.tft();
  t.fillScreen(BG);
  centred(88, "STORAGE MODE", hueCycle(0));
  centred(176, "EJECT OR HOLD B TO EXIT", DIM);
  uiGeneration++;

  pollTimer.end();                                  // (polled from here from now on: reads and writes wait for the card)
  usb_msc_set_media(&MEDIA);
  Serial.println("storage mode: started");

  int shown = -1;
  uint32_t heldSince = 0, hueAt = 0;
  uint16_t hue = 0;
  while (board.keys() & (m8::BTN_EDIT | m8::BTN_OPT)) {   // (the press that came here, let go first)
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
    if (usb_msc_ejected()) restart("EJECTED");
    if (millis() - hueAt >= 40) {                   // the title's hue goes round, once in 4 s
      hueAt = millis();
      uint16_t c = hueCycle((millis() % 4000) / 4000.0f);
      if (c != hue) {
        hue = c;
        drawText((320 - textWidth("STORAGE MODE")) / 2 / PX * PX, 88, "STORAGE MODE", c, BG);   // (over itself: no clearing, no flicker)
      }
    }
    int state = !usb_configuration ? 0 : usb_msc_host_seen() ? 2 : 1;
    if (state != shown) {
      shown = state;
      static const char *const TEXT[3] = {"CONNECT A USB CABLE", "WAITING FOR THE COMPUTER", "CONNECTED"};
      centred(120, TEXT[state], state == 2 ? INK : LIGHT);
      Serial.printf("storage mode: %s\n", TEXT[state]);
    }
    if (board.keys() & m8::BTN_OPT) {
      if (!heldSince) heldSince = millis();
      if (millis() - heldSince >= 1000) restart("RESTARTING");
    } else {
      heldSince = 0;
    }
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
  t.fillScreen(BG);
  centred(88, "STORAGE MODE", hueCycle(0));
  centred(176, "EJECT OR HOLD B TO EXIT", DIM);
  uiGeneration++;

  Serial.println("storage mode: started");
  MTP.begin();
  MTP.addFilesystem(SD, "L7");

  int shown = -1;
  uint32_t heldSince = 0, hueAt = 0;
  uint16_t hue = 0;
  while (board.keys() & (m8::BTN_EDIT | m8::BTN_OPT)) {   // (the press that came here, let go first)
    if (usbDriveTick) usbDriveTick();
    MTP.loop();
  }
  for (;;) {
    if (usbDriveTick) usbDriveTick();
    MTP.loop();
    if (millis() - hueAt >= 40) {                   // the title's hue goes round, once in 4 s
      hueAt = millis();
      uint16_t c = hueCycle((millis() % 4000) / 4000.0f);
      if (c != hue) {
        hue = c;
        drawText((320 - textWidth("STORAGE MODE")) / 2 / PX * PX, 88, "STORAGE MODE", c, BG);   // (over itself: its box redrawn whole, no clearing first)
      }
    }
    int state = !usb_configuration ? 0 : MTP ? 2 : 1;
    if (state != shown) {
      shown = state;
      static const char *const TEXT[3] = {"CONNECT A USB CABLE", "WAITING FOR THE COMPUTER", "CONNECTED"};
      centred(120, TEXT[state], state == 2 ? INK : LIGHT);
      Serial.printf("storage mode: %s\n", TEXT[state]);
    }
    if (board.keys() & m8::BTN_OPT) {
      if (!heldSince) heldSince = millis();
      if (millis() - heldSince >= 1000) {
        centred(176, "RESTARTING", LIGHT);
        delay(100);
        SCB_AIRCR = 0x05FA0004;                   // restart: the computer may have changed anything
        for (;;) {}
      }
    } else {
      heldSince = 0;
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
