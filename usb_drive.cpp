// usb_drive.cpp — see usb_drive.h
#include "usb_drive.h"
#include "ui.h"
#include "storage.h"

#if defined(USB_MTPDISK) || defined(USB_MTPDISK_SERIAL)
#include <MTP_Teensy.h>
#endif

namespace looper {

void (*usbDriveTick)() = nullptr;

static void centred(int y, const char *text, uint16_t ink) {
  int w = textWidth(text);
  board.tft().fillRect(0, y, 320, FONT_H, BG);
  drawText((320 - w) / 2 / PX * PX, y, text, ink, BG);
}

#if defined(USB_MTPDISK) || defined(USB_MTPDISK_SERIAL)
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
  centred(88, "USB DRIVE MODE", hueCycle(0));
  centred(176, "HOLD B TO EXIT", DIM);
  uiGeneration++;

  Serial.println("USB drive mode: started");
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
        centred(88, "USB DRIVE MODE", c);
      }
    }
    int state = !usb_configuration ? 0 : MTP ? 2 : 1;
    if (state != shown) {
      shown = state;
      static const char *const TEXT[3] = {"CONNECT A USB CABLE", "WAITING FOR THE COMPUTER", "CONNECTED"};
      centred(120, TEXT[state], state == 2 ? INK : LIGHT);
      Serial.printf("USB drive mode: %s\n", TEXT[state]);
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
bool usbDriveMode() { return false; }   // (not built with an MTP USB type)
#endif

}  // namespace looper
