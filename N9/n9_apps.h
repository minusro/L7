// n9_apps.h — N9's side of the on-device app switcher, and its SD layout.
// SHIFT + B + A (A last) flashes /APPS/L7.HEX from the card and reboots
// into L7 (app_loader.h; L7 carries the mirror switch back). At boot the
// card gets N9's folders: /N9, /N9/PROJECTS (L7-style layout), and /APPS.
#pragma once
#include <SdFat.h>

void appsInit();                    // best-effort SD layout, quick, card optional
void appSwitchToL7();               // returns only on failure (a message was shown)

// The card itself, for storage mode (n9_usb_drive.cpp): the shared SdFs,
// a fresh begin() (another card may be in), and a liveness check.
SdFs &appsSd();
bool appsCardRestart();
bool appsCardResponds();
