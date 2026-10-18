// k6_apps.cpp — see k6_apps.h.
#include "k6_apps.h"

#include <Audio.h>
#include <SdFat.h>

#include "app_loader.h"
#include "k6_project.h"
#include "k6_ui.h"

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
bool appsCardRestartDma() { sdUp = sd.begin(SdioConfig(DMA_SDIO)); return sdUp; }
bool appsCardResponds() { return sdUp && sd.card() && sd.card()->status() != 0; }

void appsInit() {
  if (!sdBegin()) return;           // no card: nothing to set up
  sd.mkdir("/K6");
  sd.mkdir("/K6/PROJECTS");
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
  projectSaveCurrent();             // the state survives the trip to L7
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
