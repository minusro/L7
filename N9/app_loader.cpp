// app_loader.cpp — see app_loader.h.
#include "app_loader.h"

extern "C" {
#include "FlashTxx.h"
}

// FlasherX refuses an image that doesn't contain the target's ID string —
// and the RUNNING app must carry it too, so the image of this very app
// passes the check when flashed from the other side.
__attribute__((used)) const char appLoaderFlashId[] = FLASH_ID;

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int hexByte(const char *p) {
  int h = hexNibble(p[0]), l = hexNibble(p[1]);
  return h < 0 || l < 0 ? -1 : h << 4 | l;
}

bool appLoaderFlash(FsFile &file, const char **err,
                    void (*progress)(int pct), void (*preMove)()) {
  uint32_t baddr = 0, bsize = 0;
  if (firmware_buffer_init(&baddr, &bsize) == 0) { *err = "NO FLASH BUFFER"; return false; }

  uint32_t base = 0, minA = 0xFFFFFFFF, maxA = 0;
  uint32_t total = file.size(), seen = 0;
  int lastPct = -1;
  bool eof = false;
  static char line[128];
  static char data[64] __attribute__((aligned(8)));

  while (!eof) {
    int n = file.fgets(line, sizeof line);
    if (n <= 0) { *err = "TRUNCATED HEX"; goto fail; }
    seen += n;
    if (line[0] != ':') continue;
    int len = hexByte(line + 1);
    int a16 = (hexByte(line + 3) << 8) | hexByte(line + 5);
    int type = hexByte(line + 7);
    if (len < 0 || a16 < 0 || type < 0 || len > (int)sizeof data) { *err = "BAD HEX LINE"; goto fail; }
    int sum = len + (a16 >> 8) + (a16 & 255) + type;
    for (int i = 0; i <= len; i++) {               // data + checksum byte
      int b = hexByte(line + 9 + i * 2);
      if (b < 0) { *err = "BAD HEX LINE"; goto fail; }
      if (i < len) data[i] = (char)b;
      sum += b;
    }
    if (sum & 255) { *err = "HEX CHECKSUM"; goto fail; }
    switch (type) {
      case 0: {                                    // data
        uint32_t dest = base + a16;
        if (dest < FLASH_BASE_ADDR || dest - FLASH_BASE_ADDR + len > bsize) { *err = "IMAGE TOO LARGE"; goto fail; }
        if (flash_write_block(baddr + dest - FLASH_BASE_ADDR, data, len)) { *err = "FLASH WRITE"; goto fail; }
        if (dest < minA) minA = dest;
        if (dest + len > maxA) maxA = dest + len;
        break;
      }
      case 1: eof = true; break;                   // end of file
      case 2: base = (uint32_t)((hexByte(line + 9) << 8 | hexByte(line + 11)) << 4); break;
      case 4: base = (uint32_t)(hexByte(line + 9) << 8 | hexByte(line + 11)) << 16; break;
      default: break;                              // start address records
    }
    if (progress && total) {
      int pct = (int)((uint64_t)seen * 100 / total);
      if (pct != lastPct) { lastPct = pct; progress(pct); }
    }
  }

  if (minA != FLASH_BASE_ADDR) { *err = "BAD IMAGE BASE"; goto fail; }
  if (!check_flash_id(baddr, maxA - minA)) { *err = "NOT A TEENSY40 IMAGE"; goto fail; }

  if (preMove) preMove();
  flash_move(FLASH_BASE_ADDR, baddr, maxA - minA);   // erases, copies, and the
  REBOOT;                                            //   device restarts in the other app
  return true;                                       // (never reached)

fail:
  firmware_buffer_free(baddr, bsize);
  return false;
}
