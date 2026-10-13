// n9_usb_drive.h — storage mode, L7's (../usb_drive.h): the SD card shown to
// a computer over USB as a mounted drive (USB mass storage, the patched
// serialmsc core's usb_msc.c). Outside storage mode the M8 answers the
// computer's disk probing as an empty card reader from a timer (without an
// answer macOS keeps resetting the USB connection).
#pragma once

// At start-up: with the mass-storage USB type, answers the computer's disk
// commands from a timer from now on.
void usbDriveBegin();

// Takes the screen and the card until the disk is ejected on the computer or
// any key is pressed, then gives them back (N9 caches nothing from the card
// yet: no reload needed). false: this build has no mass storage.
bool usbDriveMode();
