// usb_drive.h — USB drive mode: the SD card shown to a computer over USB.
//
// The card is shared with MTP, the protocol phones use: build with "USB Type:
// Serial + MTP Disk (Experimental)" (arduino-cli: teensy:avr:teensy40:usb=serialmtp).
// Windows and Linux show it in the file manager; macOS needs an MTP app (OpenMTP,
// Android File Transfer). A mounted drive (USB mass storage) was tried and
// abandoned: Teensyduino has no such type for the Teensy 4 and the attempt
// froze the M8 (branch usb-drive-msc).
//
// usbDriveMode() takes the screen and the card until B has been held for a
// second, then restarts the app (the computer may have changed any file).
#pragma once

namespace looper {

// Called while in drive mode, to keep the app's watchdogs and the power
// button going (set by the sketch).
extern void (*usbDriveTick)();

// false: this build has no MTP (nothing happened).
bool usbDriveMode();

}  // namespace looper
