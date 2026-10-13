// n9_apps.cpp — see n9_apps.h.
#include "n9_apps.h"

#include <Audio.h>
#include <SdFat.h>

#include "app_loader.h"
#include "n9_ui.h"

static SdFs sd;
static bool sdUp = false;

static bool sdBegin() {
  if (sdUp) return true;
  // FIFO mode: polled, no DMA interrupt entanglement with the audio ISR.
  sdUp = sd.begin(SdioConfig(FIFO_SDIO));
  return sdUp;
}

SdFs &appsSd() { return sd; }
bool appsCardRestart() { sdUp = sd.begin(SdioConfig(FIFO_SDIO)); return sdUp; }
bool appsCardResponds() { return sdUp && sd.card() && sd.card()->status() != 0; }

void appsInit() {
  if (!sdBegin()) return;           // no card: nothing to set up
  sd.mkdir("/N9");
  sd.mkdir("/N9/PROJECTS");
  sd.mkdir("/APPS");
}

static void progress(int pct) {
  char ln[24];
  snprintf(ln, sizeof ln, "READING  %d", pct);
  uiMessage("SWITCHING TO L7", ln);
}

static void preMove() {
  AudioNoInterrupts();              // the last copy: a few silent seconds
  uiMessage("FLASHING L7", "DON'T POWER OFF");
}

void appSwitchToL7() {
  uiMessage("SWITCHING TO L7", "READING CARD");
  const char *err = "NO CARD";
  if (sdBegin()) {
    appsInit();
    FsFile f = sd.open("/APPS/L7.HEX", O_RDONLY);
    if (!f) err = "NO /APPS/L7.HEX";
    else {
      if (appLoaderFlash(f, &err, progress, preMove)) return;   // (never returns)
      f.close();
      AudioInterrupts();            // a failure before the move: audio back on
    }
  }
  uiMessage("SWITCH FAILED", err);
  delay(1500);
}
