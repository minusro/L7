// app_loader.h — switching between the M8's apps (L7 and N9) on the device:
// reads the other app's Intel-hex image from the SD card into a buffer in
// upper flash (FlasherX's FlashTxx primitives, public domain), verifies it,
// then copies it over the running program and reboots. The last copy is the
// only unsafe window (a few seconds): losing power there needs a USB
// reflash. This file and FlashTxx.c/.h are kept as identical copies in L7
// and N9 (like M8Board); the images live on the card as /APPS/L7.HEX and
// /APPS/N9.HEX (tools/make_apps.sh builds both).
#pragma once
#include <SdFat.h>

// Flashes the image and reboots; returns only on failure with a message.
// progress gets 0-100 while the image is read; preMove runs just before the
// final (irreversible) copy - stop audio there.
bool appLoaderFlash(FsFile &file, const char **err,
                    void (*progress)(int pct), void (*preMove)());
