// n9_usb_drive.cpp — see n9_usb_drive.h. L7's usb_drive.cpp, trimmed to N9:
// nothing to save before handing the card over (no project files yet) and
// nothing to reload after (the card isn't cached), the screen drawn whole
// through the frame buffer (uiStorageDraw) only when something changes.
#include "n9_usb_drive.h"

#include <Arduino.h>

#include "M8Board.h"
#include "n9_apps.h"
#include "n9_synth.h"
#include "n9_ui.h"

#if defined(USB_SERIAL_MSC)
#include "usb_msc.h"                               // (the patched core's: tools/teensy_msc)
extern "C" volatile uint8_t usb_configuration;

// The card, sector by sector, as the computer sees it.
static uint32_t cardSectors() { return appsSd().card()->sectorCount(); }
static int cardRead(uint32_t lba, uint8_t *dst, uint32_t n) { return appsSd().card()->readSectors(lba, dst, n); }
static int cardWrite(uint32_t lba, const uint8_t *src, uint32_t n) { return appsSd().card()->writeSectors(lba, src, n); }
static void cardSync() { appsSd().card()->syncDevice(); }
static const usb_msc_media_t MEDIA = {cardSectors, cardRead, cardWrite, cardSync};

// Outside storage mode (no medium): polled from a timer, so the computer's
// probing is always answered (each answer is only queued: microseconds).
static IntervalTimer pollTimer;
void usbDriveBegin() {
  pollTimer.priority(160);                          // (under the audio and the USB itself)
  pollTimer.begin([] { usb_msc_poll(); }, 2000);
}

// The app's housekeeping, kept going from the blocking loop here.
static void tick() {
  board.chargeGuard();
  bool hp;
  if (board.headphonesChanged(hp)) board.setSpeaker(!hp);
  if (board.powerButtonPressed()) board.powerOff();
}

// Back from the computer: the card is the app's again (it may have changed
// any file; N9 holds nothing read from it, so a fresh begin is enough).
static bool leave(const char *why, uint8_t state) {
  Serial.printf("storage mode: %s\n", why);
  if (appsCardResponds()) cardSync();
  usb_msc_set_media(nullptr);                       // (an empty card reader again, answered from the timer)
  usb_msc_clear_host_state();
  pollTimer.begin([] { usb_msc_poll(); }, 2000);
  uiStorageDraw(state, why);
  appsCardRestart();
  appsInit();                                       // (the folders back, were they wiped)
  for (uint32_t at = millis(); millis() - at < 400;) tick();   // (the message stays readable)
  while (board.keys()) tick();                      // (the press that left, let go: not the menu's)
  return true;
}

FLASHMEM bool usbDriveMode() {
  if (synth.seqOn()) synth.seqToggle();             // the transport stopped; nothing to save yet

  pollTimer.end();                                  // (polled from here from now on: reads and writes wait for the card)
  // The card started again (another may have been put in since it was last
  // read); none: an empty card reader until one is put in. Checked twice a
  // second: taken out (or swapped), the computer is told it's gone, then
  // that the new one arrived.
  bool card = appsCardRestartDma();              // MSC against DMA_SDIO, as L7 runs it
  usb_msc_set_media(card ? &MEDIA : nullptr);
  usb_msc_clear_host_state();                       // (the computer's asking counted from now)
  Serial.printf("storage mode: started%s\n", card ? "" : " (no card)");
  uint32_t cardAt = millis();
  // The computer connected but not asking about the drive for 3 s (its disk
  // driver gave up on it earlier, a card taken out under it say, and won't
  // look again): the USB connection dropped for a moment, so it looks afresh
  // (once per card; the serial port and USB audio and MIDI come back with it).
  uint32_t askAt = millis();
  bool reconnected = false;

  int shown = -1;
  while (board.keys()) {                            // (the press that came here, let go first)
    tick();
    usb_msc_poll();
  }
  for (;;) {
    tick();
    usb_msc_poll();
    static uint32_t statsAt = 0, shownStats = 0;
    if (millis() - statsAt >= 2000) {               // (serial log: what the computer did, when anything changed)
      statsAt = millis();
      uint32_t k = usb_msc_commands * 31 + usb_msc_failed * 7 + usb_msc_broken * 3 + usb_msc_restarts;
      if (k != shownStats) {
        shownStats = k;
        Serial.printf("storage mode: %lu commands, %lu reads, %lu writes; %lu failed (the last: %02X, sense %06lX), %lu broken, %lu restarts\n",
                      (unsigned long)usb_msc_commands, (unsigned long)usb_msc_reads, (unsigned long)usb_msc_writes,
                      (unsigned long)usb_msc_failed, usb_msc_failed_op, (unsigned long)usb_msc_last_sense,
                      (unsigned long)usb_msc_broken, (unsigned long)usb_msc_restarts);
      }
    }
    if (millis() - cardAt >= 500) {
      cardAt = millis();
      if (card && !appsCardResponds()) {
        card = false;
        usb_msc_set_media(nullptr);                 // (no medium: the computer drops the drive)
        Serial.println("storage mode: the card taken out");
      } else if (!card && appsCardRestartDma()) {
        card = true;
        usb_msc_set_media(&MEDIA);                  // (a new medium: the computer mounts it)
        usb_msc_clear_host_state();
        askAt = millis();
        reconnected = false;
        Serial.println("storage mode: a card put in");
      }
    }
    if (card && !reconnected && usb_configuration && !usb_msc_host_seen() && millis() - askAt >= 3000) {
      reconnected = true;
      Serial.println("storage mode: the computer isn't asking for the drive: reconnecting USB");
      USB1_USBCMD &= ~USB_USBCMD_RS;                // (detached: the computer sees it unplugged)
      for (uint32_t at = millis(); millis() - at < 300;) tick();
      USB1_USBCMD |= USB_USBCMD_RS;                 // (attached again: enumerated afresh)
      askAt = millis();
    }
    int state = !card ? 3 : !usb_configuration ? 0 : usb_msc_host_seen() ? 2 : 1;
    if (usb_msc_ejected()) return leave("EJECTED", state);
    if (state != shown) {
      shown = state;
      uiStorageDraw(state, nullptr);
      Serial.printf("storage mode: state %d\n", state);
    }
    if (board.keys()) return leave("WAIT", state);   // any key: back to the app (ejecting on the computer does too)
  }
}
#else
void usbDriveBegin() {}
bool usbDriveMode() { return false; }   // (not built with a storage USB type)
#endif
