// midi_usb.h — MIDI clock over USB (the USB type "Serial + MIDI + Mass
// Storage", tools/teensy_msc/patch.py): out, the same clock, Start and Stop
// as the serial MIDI out (engine.cpp); in, clock / Start / Continue / Stop
// followed as the serial MIDI in's (midi_in.h). Its own timer, below the
// audio interrupt's priority: a computer not reading USB MIDI can hold a send
// up a moment (the core waits up to 40 ms, once), never the audio. Other USB
// types: nothing.
#pragma once

void midiUsbBegin();
