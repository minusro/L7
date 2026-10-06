// usb_drive.h — storage mode: the SD card shown to a computer over USB as a
// mounted drive (USB mass storage), to copy files to and from it.
//
// Teensyduino has no mass-storage USB type for the Teensy 4, so the core is
// patched (tools/teensy_msc/patch.py: a "Serial + Mass Storage" type, usb_msc.c
// the bulk-only SCSI engine) and the sketch is built with it (arduino-cli:
// teensy:avr:teensy40:usb=serialmsc; Arduino IDE: Tools > USB Type > "Serial +
// Mass Storage (L7 patch)"). Outside storage mode the M8 answers the computer's
// disk commands as an empty card reader (from a timer): without an answer macOS
// keeps resetting the USB connection. An MTP build (usb=serialmtp, no patch;
// shows in Android File Transfer, not as a drive) also works.
//
// usbDriveMode() writes everything out and takes the screen and the card until
// the disk is ejected on the computer, or START is pressed, then
// gives them back: the app forgets everything it knew of the card and reads it
// all again (usbDriveReload, as at a start: the computer may have changed any
// file). Without a reload function (or in the MTP build) it restarts instead.
#pragma once

namespace looper {

// Called while in storage mode, to keep the app's watchdogs and the power
// button going (set by the sketch).
extern void (*usbDriveTick)();

// Reads everything from the card again, as at a start (set by the sketch).
extern void (*usbDriveReload)();

// At start-up: with the mass-storage USB type, answers the computer's disk
// commands from a timer from now on. Nothing for the MTP type.
void usbDriveBegin();

// false: this build has no mass storage or MTP (nothing happened).
bool usbDriveMode();

}  // namespace looper
