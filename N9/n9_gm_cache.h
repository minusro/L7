// n9_gm_cache.h — the GM melodic samples: full-quality records on the card
// (/N9/GM.BNK, tools/sf2_gm_bank.py) cached into a 960 KB flash region above
// the app image (nine slots of 104 KB, one per program in use). A program
// plays its synth patch until its record is cached; the cache survives
// reboots (slots are validated against the bank's version) but not an app
// switch (the switcher's buffer shares the region - the next boot reloads).
// Writing a slot silences the audio for a second or two: flash can't be
// read while it's erased/written.
#pragma once
#include <stdint.h>

struct GmCacheZone { int16_t smp; uint8_t hi, root; int16_t cents; uint16_t pad; };
struct GmCacheSmp { uint32_t off, len, loopS, loopE; uint16_t rateQ; uint8_t gain, pad; };
struct GmCacheHdr {
  uint32_t magic;                  // 'N9GP'
  uint32_t bankVer;                // must equal GM_BANK_VER (n9_gm_pcm.h)
  uint8_t prog, nzones, nsmp, pad;
  uint32_t dataBytes;              // mu-law payload after this header
  GmCacheZone zone[4];
  GmCacheSmp smp[4];               // smp.off is relative to the payload
};
static_assert(sizeof(GmCacheHdr) == 128, "the SD records are written to this layout");

void gmCacheInit();                      // boot: bank directory in, flash slots scanned
const GmCacheHdr *gmCacheFor(uint8_t prog);   // the cached record, or null
// Commits one needed program per call (the tracks' GM programs are polled;
// a change settles ~0.7 s before flashing unless immediate). true: worked.
bool gmCacheService(bool immediate = false);
