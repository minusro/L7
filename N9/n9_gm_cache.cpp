// n9_gm_cache.cpp — see n9_gm_cache.h. The GM melodic bank lives whole in a
// flash region above the app image, loaded ONCE at boot from the card's
// /N9/GM.FLB (a verbatim flash image: header, a 128-entry directory, then
// self-contained program records). Runtime never touches flash again, so
// preset and project switches play on uninterrupted. The app switcher's
// buffer shares the region by design: a switch wipes it and the next boot
// reloads (~10 s). The region survives normal reboots (header + version).
#include "n9_gm_cache.h"

#include <Arduino.h>

#include "n9_apps.h"
#include "n9_gm_pcm.h"               // GM_BANK_VER
#include "n9_state.h"
#include "n9_synth.h"
#include "n9_ui.h"

extern "C" {
void eepromemu_flash_write(void *addr, const void *data, uint32_t len);
void eepromemu_flash_erase_sector(void *addr);
}
extern "C" unsigned long _flashimagelen;

static constexpr uint32_t BASE = 0x60100000u;    // 1 MB in: past the app image
static constexpr uint32_t REGION = 0xF0000u;     // to 0x601F0000 (the EEPROM reserve)
static constexpr uint32_t SECTOR = 0x1000u;
static constexpr uint32_t REG_MAGIC = 0x4647394Eu;   // 'N9GF'

struct RegionHdr {
  uint32_t magic, ver;
  struct { uint32_t off, size; } dir[128];
};

static bool regionOk = false;
static uint8_t availList[128];
static int availN = 0;

int gmAvailCount() { return availN; }
uint8_t gmAvailAt(int i) { return availList[i < 0 ? 0 : i >= availN ? availN - 1 : i]; }
int gmAvailIndexOf(uint8_t prog) {
  for (int i = 0; i < availN; i++) if (availList[i] == prog) return i;
  return -1;
}
static void availScan() {
  availN = 0;
  if (!regionOk) return;
  const RegionHdr *rh = (const RegionHdr *)BASE;
  for (int p = 0; p < 128; p++)
    if (rh->dir[p].size) availList[availN++] = (uint8_t)p;
}

const GmCacheHdr *gmCacheFor(uint8_t prog) {
  if (!regionOk || prog > 127) return nullptr;
  const RegionHdr *rh = (const RegionHdr *)BASE;
  uint32_t off = rh->dir[prog].off;
  if (!rh->dir[prog].size || (off & 3) || off + sizeof(GmCacheHdr) > REGION) return nullptr;
  const GmCacheHdr *h = (const GmCacheHdr *)(BASE + off);
  if (h->magic != 0x5047394Eu || h->prog != prog || h->nzones > 4 || h->nsmp > 4) return nullptr;
  return h;
}

static bool regionValid() {
  const RegionHdr *rh = (const RegionHdr *)BASE;
  return rh->magic == REG_MAGIC && rh->ver == GM_BANK_VER;
}

void gmCacheInit() {
  regionOk = false;
  if ((uint32_t)&_flashimagelen > BASE - 0x60000000u - 0x8000u) {
    Serial.println("gm: the app image grew into the sample region - GM on synth patches");
    return;
  }
  if (regionValid()) {
    regionOk = true;
    availScan();
    Serial.printf("gm: the sample region held over (%d programs)\n", availN);
    return;
  }
  FsFile f = appsSd().open("/N9/GM.FLB", O_RDONLY);
  if (!f) { Serial.println("gm: no /N9/GM.FLB (GM plays synth patches)"); return; }
  uint32_t head[2];
  uint32_t size = f.size();
  bool ok = f.read(head, 8) == 8 && head[0] == REG_MAGIC && head[1] == GM_BANK_VER &&
            size > sizeof(RegionHdr) && size <= REGION;
  if (!ok) {
    Serial.println("gm: GM.FLB MISMATCH (regenerate with tools/sf2_gm_bank.py)");
    f.close();
    return;
  }
  Serial.printf("gm: loading the sample region (%lu KB)...\n", (unsigned long)(size / 1024));
  f.seek(0);
  synth.pcmPanic();
  delay(6);
  static uint8_t buf[SECTOR] __attribute__((aligned(8)));
  AudioNoInterrupts();
  uint32_t done = 0;
  while (done < size) {
    uint32_t n = size - done < SECTOR ? size - done : SECTOR;
    if (f.read(buf, n) != (int)n) break;
    uint32_t w = (n + 3) & ~3u;
    if (w > n) memset(buf + n, 0xFF, w - n);
    if (done == 0) memset(buf, 0xFF, 4);           // the magic goes in last
    eepromemu_flash_erase_sector((void *)(BASE + done));
    for (uint32_t o = 0; o < w; o += 256)          // NOR pages wrap at 256 bytes
      eepromemu_flash_write((void *)(BASE + done + o), buf + o, w - o < 256 ? w - o : 256);
    done += n;
    if ((done & 0xFFFF) == 0) {
      AudioInterrupts();
      char msg[20];
      snprintf(msg, sizeof msg, "%lu%%", (unsigned long)(done * 100 / size));
      uiMessage("GM SAMPLES", msg);
      AudioNoInterrupts();
    }
  }
  bool full = done >= size;
  if (full) {
    uint32_t magic = REG_MAGIC;
    eepromemu_flash_write((void *)BASE, &magic, 4);
  }
  AudioInterrupts();
  f.close();
  regionOk = full && regionValid();
  availScan();
  Serial.printf("gm: sample region %s (%d programs)\n", regionOk ? "loaded" : "LOAD FAILED", availN);
}

bool gmCacheService(bool) { return false; }      // nothing to do at runtime any more
