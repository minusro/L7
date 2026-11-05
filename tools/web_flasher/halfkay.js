// HalfKay, the Teensy 4 bootloader's USB HID protocol, as PJRC's
// teensy_loader_cli and Koromix's TyTools speak it. No browser APIs here, so
// check.mjs runs the same code under Node.
//
// - The loader is a HID device, VID 0x16C0 PID 0x0478, usage page 0xFF9C. Its
//   usage names the board: 0x24 Teensy 4.0 (the M8 Model:01), 0x25 Teensy 4.1,
//   0x26 Teensy MicroMod (the M8 Model:02).
// - Firmware goes in 1024-byte blocks, each an output report of 64 + 1024 bytes:
//   the block's flash address in bytes 0-2 (little-endian), zeros to byte 63,
//   then the data. The first block written erases the chip, which takes seconds.
// - A report with address 0xFFFFFF reboots into the new firmware; with
//   B7 31 C2 89 and a 64-bit count of 1/32768 s in the data, it also sets the
//   clock (local time, as Teensy Loader sets it).
// - Teensy 4 .hex files put flash at 0x60000000 (FlexSPI); the loader counts
//   from 0.

export const VENDOR_ID = 0x16c0;
export const LOADER_PRODUCT_ID = 0x0478;
export const LOADER_USAGE_PAGE = 0xff9c;
export const BLOCK_SIZE = 1024;
export const HEADER_SIZE = 64;
export const REPORT_SIZE = HEADER_SIZE + BLOCK_SIZE;

const FLEXSPI = 0x60000000;
const FLEXSPI_SPAN = 0x1000000;

// codeSize: the bytes the loader accepts (teensy_loader_cli's table).
export const BOARDS = {
  teensy40: { id: 'teensy40', name: 'Teensy 4.0', usage: 0x24, codeSize: 2031616, m8: 'Model:01', key: 'model01' },
  teensy41: { id: 'teensy41', name: 'Teensy 4.1', usage: 0x25, codeSize: 8126464, m8: null, key: null },
  teensyMM: { id: 'teensyMM', name: 'Teensy MicroMod', usage: 0x26, codeSize: 16515072, m8: 'Model:02', key: 'model02' },
};

export function boardFromUsage(usage) {
  return Object.values(BOARDS).find((b) => b.usage === usage) || null;
}

export class HexError extends Error {}

// Intel HEX text -> { blocks: Map<block index, Uint8Array(1024)>, bytes, size }.
// Bytes the file doesn't set stay 0xFF (erased flash). size is one past the
// highest address written, counted from the start of flash.
export function parseHex(text) {
  const blocks = new Map();
  let ext = 0;
  let ended = false;
  let bytes = 0;
  let size = 0;
  const lines = text.split(/\r?\n/);

  for (let n = 0; n < lines.length && !ended; n++) {
    const line = lines[n].trim();
    if (!line) continue;
    const where = `line ${n + 1}`;
    if (line[0] !== ':' || line.length < 11 || line.length % 2 === 0 || /[^0-9a-f]/i.test(line.slice(1))) {
      throw new HexError(`${where} isn't Intel HEX. Is this a .hex firmware file?`);
    }
    const rec = new Uint8Array((line.length - 1) / 2);
    for (let i = 0; i < rec.length; i++) rec[i] = parseInt(line.substr(1 + 2 * i, 2), 16);
    const len = rec[0];
    if (rec.length !== len + 5) throw new HexError(`${where} has the wrong length.`);
    let sum = 0;
    for (const b of rec) sum += b;
    if (sum & 0xff) throw new HexError(`${where} fails its checksum. The file may be damaged.`);

    const offset = (rec[1] << 8) | rec[2];
    const type = rec[3];
    const data = rec.subarray(4, 4 + len);
    switch (type) {
      case 0x00: {
        let addr = ext + offset;
        if (addr < FLEXSPI || addr + len > FLEXSPI + FLEXSPI_SPAN) {
          throw new HexError(`${where} writes to 0x${addr.toString(16)}, outside a Teensy 4's flash. This isn't firmware for an M8.`);
        }
        addr -= FLEXSPI;
        for (let i = 0; i < len; i++, addr++) {
          const index = Math.floor(addr / BLOCK_SIZE);
          let block = blocks.get(index);
          if (!block) {
            block = new Uint8Array(BLOCK_SIZE).fill(0xff);
            blocks.set(index, block);
          }
          block[addr % BLOCK_SIZE] = data[i];
        }
        bytes += len;
        size = Math.max(size, addr);
        break;
      }
      case 0x01:
        ended = true;
        break;
      case 0x02:
        ext = ((data[0] << 8) | data[1]) * 16;
        break;
      case 0x04:
        ext = ((data[0] << 8) | data[1]) * 65536;
        break;
      case 0x03:
      case 0x05:
        break; // start addresses: the loader doesn't need them
      default:
        throw new HexError(`${where} has an unknown record type (${type}).`);
    }
  }

  if (!ended) throw new HexError('The file stops before its end record. The download may be incomplete.');
  if (!blocks.has(0)) throw new HexError("The file has nothing at the start of flash. This isn't Teensy 4 firmware.");
  return { blocks, bytes, size };
}

// Which board a parsed firmware was built for, from the FlexSPI config block
// at the start of flash (as TyTools reads it): its "FCFB" tag, then the flash
// size at byte 80. null if it isn't Teensy 4 firmware.
export function identifyFirmware(fw) {
  const b = fw.blocks.get(0);
  const tag = [0x46, 0x43, 0x46, 0x42, 0x00, 0x00, 0x01, 0x56];
  if (!b || !tag.every((v, i) => b[i] === v)) return null;
  const flashSize = (b[80] | (b[81] << 8) | (b[82] << 16) | (b[83] << 24)) >>> 0;
  if (flashSize === 0x1000000) return BOARDS.teensyMM;
  if (flashSize === 0x800000) return BOARDS.teensy41;
  return BOARDS.teensy40;
}

// The reports to send, in address order. The first block always goes (it
// erases the chip); after it, blocks that are all 0xFF are already right.
export function dataReports(fw) {
  const reports = [];
  for (const index of [...fw.blocks.keys()].sort((a, b) => a - b)) {
    const block = fw.blocks.get(index);
    if (index !== 0 && block.every((v) => v === 0xff)) continue;
    const addr = index * BLOCK_SIZE;
    const r = new Uint8Array(REPORT_SIZE);
    r[0] = addr & 0xff;
    r[1] = (addr >> 8) & 0xff;
    r[2] = (addr >> 16) & 0xff;
    r.set(block, HEADER_SIZE);
    reports.push(r);
  }
  return reports;
}

// Local wall-clock time as seconds since 1970, the way Teensy Loader sets the RTC.
export function localClockSeconds(date = new Date()) {
  return Math.floor(date.getTime() / 1000) - date.getTimezoneOffset() * 60;
}

export function bootReport(rtcSeconds = null) {
  const r = new Uint8Array(REPORT_SIZE);
  r[0] = r[1] = r[2] = 0xff;
  if (rtcSeconds != null) {
    const ticks = BigInt(Math.floor(rtcSeconds)) << 15n;
    r.set([0xb7, 0x31, 0xc2, 0x89], HEADER_SIZE);
    for (let i = 0; i < 8; i++) r[HEADER_SIZE + 4 + i] = Number((ticks >> BigInt(8 * i)) & 0xffn);
  }
  return r;
}

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// The loader refuses writes while it's busy (erasing, or writing the last
// block), so a failed write is tried again until the timeout, as both
// reference loaders do.
async function sendRetrying(send, report, timeoutMs, signal) {
  const start = Date.now();
  for (;;) {
    if (signal?.aborted) throw new Error('The M8 was disconnected.');
    try {
      await send(report);
      return;
    } catch (e) {
      if (Date.now() - start > timeoutMs) {
        throw new Error(`The M8 stopped accepting data (${e?.message || e}).`);
      }
    }
    await wait(20);
  }
}

// send(report) writes one output report (report ID 0) and resolves when done.
// onProgress(sent, total) after each block.
export async function upload(fw, send, { onProgress, signal } = {}) {
  const reports = dataReports(fw);
  for (let n = 0; n < reports.length; n++) {
    // The first few blocks wait out the erase (teensy_loader_cli allows 45 s).
    await sendRetrying(send, reports[n], n < 4 ? 45000 : 10000, signal);
    if (n === 0) await wait(500);
    onProgress?.(n + 1, reports.length);
  }
  return reports.length;
}

// The loader may vanish into the new firmware before it acknowledges, so the
// caller should treat a failure here as "probably fine".
export async function boot(send, rtcSeconds = localClockSeconds()) {
  await sendRetrying(send, bootReport(rtcSeconds), 500);
}
