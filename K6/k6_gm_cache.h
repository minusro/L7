// k6_gm_cache.h — the GM melodic bank: the whole thing (all programs,
// starts-accurate quality ladder) lives in a 960 KB flash region above the
// app image, loaded ONCE at boot from the card's /K6/GM.FLB
// (tools/sf2_gm_bank.py). Runtime never writes flash, so preset and project
// switches play on uninterrupted. The region survives reboots; an app
// switch wipes it (the switcher's buffer shares the space) and the next
// boot reloads it with a progress message.
#pragma once
#include <stdint.h>

struct GmCacheZone { int16_t smp; uint8_t hi, root; int16_t cents; uint16_t pad; };
struct GmCacheSmp { uint32_t off, len, loopS, loopE; uint16_t rateQ; uint8_t gain, pad; };
struct GmCacheHdr {
  uint32_t magic;                  // 'N9GP'
  uint32_t bankVer;                // must equal GM_BANK_VER (k6_gm_pcm.h)
  uint8_t prog, nzones, nsmp, pad;
  uint32_t dataBytes;              // mu-law payload after this header
  GmCacheZone zone[4];
  GmCacheSmp smp[4];               // smp.off is relative to the payload
};
static_assert(sizeof(GmCacheHdr) == 128, "the SD records are written to this layout");

void gmCacheInit();                      // boot: bank directory in, flash slots scanned
const GmCacheHdr *gmCacheFor(uint8_t prog);   // the cached record, or null

// The banked programs, as a continuous 1..N list (the preset selector shows
// only these; programs that fell back to synth patches are hidden).
int gmAvailCount();
uint8_t gmAvailAt(int i);
int gmAvailIndexOf(uint8_t prog);        // -1: not banked
bool gmCacheService(bool immediate = false);   // (a no-op now: boot loads everything)
