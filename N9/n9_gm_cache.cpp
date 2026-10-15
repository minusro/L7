// n9_gm_cache.cpp — see n9_gm_cache.h. The flash region sits at a fixed
// address above the app image (checked at init); the app switcher's buffer
// shares it by design, so a switch wipes the cache and the next boot
// reloads what the project needs. Slot writes use the core's eeprom.c flash
// primitives (they disable interrupts around each op); the audio interrupt
// is held off for the whole slot so no voice reads a half-written sample.
#include "n9_gm_cache.h"

#include <Arduino.h>

#include "n9_apps.h"
#include "n9_gm_bank.h"              // GM_NAMES
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
static constexpr uint32_t SLOT = 0x1A000u;       // 104 KB, 26 flash sectors
static constexpr uint32_t SECTOR = 0x1000u;
static constexpr int SLOTS = 9;                  // one per track
static constexpr uint32_t REC_MAGIC = 0x5047394Eu;   // 'N9GP'

static bool bankOk = false;
static int16_t slotProg[SLOTS];
static uint32_t dirOff[128], dirSize[128];
static uint8_t candProg = 255;                   // the settling request
static uint32_t candAt = 0;

static const GmCacheHdr *slotHdr(int s) { return (const GmCacheHdr *)(BASE + s * SLOT); }

const GmCacheHdr *gmCacheFor(uint8_t prog) {
  for (int s = 0; s < SLOTS; s++)
    if (slotProg[s] == prog) return slotHdr(s);
  return nullptr;
}

void gmCacheInit() {
  for (int s = 0; s < SLOTS; s++) slotProg[s] = -1;
  if ((uint32_t)&_flashimagelen > BASE - 0x60000000u - 0x8000u) {
    Serial.println("gm cache: the app image grew into the cache region - cache off");
    return;
  }
  for (int s = 0; s < SLOTS; s++) {              // what a previous run left behind
    const GmCacheHdr *h = slotHdr(s);
    if (h->magic == REC_MAGIC && h->bankVer == GM_BANK_VER && h->prog < 128 &&
        h->nzones <= 4 && h->nsmp <= 4 && h->dataBytes <= SLOT - sizeof(GmCacheHdr))
      slotProg[s] = h->prog;
  }
  FsFile f = appsSd().open("/N9/GM.BNK", O_RDONLY);
  if (!f) { Serial.println("gm cache: no /N9/GM.BNK (GM plays synth patches)"); return; }
  uint32_t head[3];
  bool ok = f.read(head, 12) == 12 && head[0] == 0x4247394Eu && head[1] == GM_BANK_VER;
  if (ok) {
    for (int p = 0; p < 128 && ok; p++) {
      uint32_t row[2];
      ok = f.read(row, 8) == 8;
      dirOff[p] = row[0]; dirSize[p] = row[1];
    }
  }
  f.close();
  bankOk = ok;
  int held = 0;
  for (int s = 0; s < SLOTS; s++) held += slotProg[s] >= 0;
  Serial.printf("gm cache: %s, %d slots held over\n", ok ? "bank ok" : "BANK MISMATCH (regenerate GM.BNK)", held);
}

// The SD record streamed into a flash slot, a sector at a time; the record's
// magic word goes in last, so a torn write never scans as valid.
static bool commit(uint8_t prog) {
  bool used[SLOTS] = {};
  for (int s = 0; s < SLOTS; s++)
    for (int t = 0; t < 9; t++)
      if (trackEngine[t] == E_GM && slotProg[s] == (int16_t)(gmProgram[t] & 127)) used[s] = true;
  int victim = -1;
  for (int s = 0; s < SLOTS && victim < 0; s++) if (slotProg[s] < 0) victim = s;
  for (int s = 0; s < SLOTS && victim < 0; s++) if (!used[s]) victim = s;
  if (victim < 0) return false;
  FsFile f = appsSd().open("/N9/GM.BNK", O_RDONLY);
  if (!f) return false;
  uiMessage("GM LOADING", GM_NAMES[prog]);
  slotProg[victim] = -1;
  synth.pcmPanic();                              // every pool voice off before the ground moves
  delay(6);
  uint32_t addr = BASE + victim * SLOT, size = dirSize[prog], done = 0;
  f.seek(dirOff[prog]);
  // The core's flash_write issues ONE program command for the whole length,
  // and NOR page programming WRAPS at 256-byte pages (the EEPROM emulation
  // only ever writes 4 bytes, so the core never meets this). Writes here go
  // page by page, from an 8-aligned buffer, lengths padded to words (the
  // 0xFF padding lands on erased flash; unaligned sources fault the
  // function's memcpy into the FlexSPI registers).
  static uint8_t buf[SECTOR] __attribute__((aligned(8)));
  AudioNoInterrupts();
  while (done < size) {
    uint32_t n = size - done < SECTOR ? size - done : SECTOR;
    if (f.read(buf, n) != (int)n) break;
    uint32_t w = (n + 3) & ~3u;
    if (w > n) memset(buf + n, 0xFF, w - n);
    if (done == 0) memset(buf, 0xFF, 4);           // the magic goes in last
    eepromemu_flash_erase_sector((void *)(addr + done));
    for (uint32_t o = 0; o < w; o += 256)
      eepromemu_flash_write((void *)(addr + done + o), buf + o,
                            w - o < 256 ? w - o : 256);
    done += n;
  }
  bool ok = done >= size;
  if (ok) {
    uint32_t magic = REC_MAGIC;
    eepromemu_flash_write((void *)addr, &magic, 4);
  }
  AudioInterrupts();
  f.close();
  if (ok) slotProg[victim] = prog;
  Serial.printf("gm cache: %s -> slot %d %s\n", GM_NAMES[prog], victim, ok ? "ok" : "FAILED");
  return ok;
}

bool gmCacheService(bool immediate) {
  if (!bankOk) return false;
  uint8_t need = 255;
  for (int t = 0; t < 9 && need == 255; t++) {
    if (trackEngine[t] != E_GM) continue;
    uint8_t p = gmProgram[t] & 127;
    if (!gmCacheFor(p) && dirSize[p]) need = p;
  }
  if (need == 255) { candProg = 255; return false; }
  if (!immediate) {                              // browsing settles before flashing
    if (need != candProg) { candProg = need; candAt = millis(); return false; }
    if (millis() - candAt < 700) return false;
  }
  candProg = 255;
  return commit(need);
}
