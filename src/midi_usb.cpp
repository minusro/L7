// midi_usb.cpp — see midi_usb.h
#include <Arduino.h>
#include "midi_usb.h"
#include "engine.h"
#include "midi_in.h"
using namespace looper;

#if defined(MIDI_INTERFACE)
static IntervalTimer usbTimer;

static void poll() {
  // Out: realtime bytes as they are; a Song Position (0xF2 and two data
  // bytes, possibly split over two takes) as one message.
  static uint8_t spp = 0, sppLsb = 0;               // its data bytes so far
  uint8_t out[16];
  int n = midiOutTakeUsb(micros(), out, sizeof out);
  for (int i = 0; i < n; i++) {
    uint8_t b = out[i];
    if (b == 0xF2) spp = 1;
    else if (b < 0x80 && spp == 1) { sppLsb = b; spp = 2; }
    else if (b < 0x80 && spp == 2) { spp = 0; usbMIDI.sendSongPosition(sppLsb | b << 7); }
    else if (b >= 0xF8) usbMIDI.sendRealTime(b);
  }
  if (n) usbMIDI.send_now();
  while (usbMIDI.read()) {                          // (everything read; the clock, transport and song position taken)
    uint8_t type = usbMIDI.getType();
    if (type >= 0xF8) midiInByte(type, micros(), MIDI_USB);
    else if (type == 0xF2) midiInSongPosition(usbMIDI.getData1() | usbMIDI.getData2() << 7, micros(), MIDI_USB);
  }
}

void midiUsbBegin() {
  midiOutAttachUsb();
  usbTimer.priority(224);                           // (under the audio interrupt's 208)
  usbTimer.begin(poll, 250);
}
#else
void midiUsbBegin() {}
#endif
