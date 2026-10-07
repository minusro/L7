// midi_usb.cpp — see midi_usb.h
#include <Arduino.h>
#include "midi_usb.h"
#include "engine.h"
#include "midi_in.h"
using namespace looper;

#if defined(MIDI_INTERFACE)
static IntervalTimer usbTimer;

static void poll() {
  uint8_t out[16];
  int n = midiOutTakeUsb(micros(), out, sizeof out);
  for (int i = 0; i < n; i++) usbMIDI.sendRealTime(out[i]);
  if (n) usbMIDI.send_now();
  while (usbMIDI.read()) {                          // (everything read; only the clock's taken)
    uint8_t type = usbMIDI.getType();
    if (type >= 0xF8) midiInByte(type, micros(), MIDI_USB);
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
