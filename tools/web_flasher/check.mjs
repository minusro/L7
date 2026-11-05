// Offline check of halfkay.js against real firmware files:
//   node tools/web_flasher/check.mjs <file.hex>... [--objcopy <arm-none-eabi-objcopy>]
// For each file: parses it, names the board it's for, counts the blocks the
// page would send, and (with --objcopy) compares the parsed image byte for byte
// with objcopy's own conversion of the .hex. Also checks the boot report's
// clock encoding against TyTools' arithmetic.
import { readFileSync, mkdtempSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import * as HK from './halfkay.js';

const args = process.argv.slice(2);
const oi = args.indexOf('--objcopy');
const objcopy = oi >= 0 ? args.splice(oi, 2)[1] : null;
let failed = false;
const fail = (msg) => { console.log(`  FAIL ${msg}`); failed = true; };

for (const file of args) {
  console.log(file);
  const fw = HK.parseHex(readFileSync(file, 'utf8'));
  const board = HK.identifyFirmware(fw);
  const reports = HK.dataReports(fw);
  console.log(`  ${fw.bytes} bytes of data, flash used to 0x${fw.size.toString(16)}`);
  console.log(`  for: ${board ? `${board.name}${board.m8 ? ` (M8 ${board.m8})` : ''}` : 'unknown'}`);
  console.log(`  ${reports.length} blocks to send, ${reports.length * HK.REPORT_SIZE} bytes over USB`);
  if (!board) fail('not recognised as Teensy 4 firmware');
  else if (fw.size > board.codeSize) fail(`too big for a ${board.name}`);
  if (reports[0][0] | reports[0][1] | reports[0][2]) fail('first report is not block 0');
  if (reports.some((r) => r.length !== HK.REPORT_SIZE)) fail('report of the wrong size');

  if (objcopy) {
    const dir = mkdtempSync(join(tmpdir(), 'halfkay-'));
    try {
      const bin = join(dir, 'fw.bin');
      execFileSync(objcopy, ['-I', 'ihex', '-O', 'binary', '--gap-fill', '0xff', file, bin]);
      const ref = readFileSync(bin);
      if (ref.length !== fw.size) fail(`objcopy image is ${ref.length} bytes, ours ${fw.size}`);
      let diffs = 0;
      for (let a = 0; a < ref.length; a++) {
        const block = fw.blocks.get(Math.floor(a / HK.BLOCK_SIZE));
        const ours = block ? block[a % HK.BLOCK_SIZE] : 0xff;
        if (ours !== ref[a] && diffs++ < 5) console.log(`  differs at 0x${a.toString(16)}: ${ours} vs ${ref[a]}`);
      }
      // ...and the reports carry exactly that image, every non-blank block included
      const sent = new Uint8Array(ref.length).fill(0xff);
      for (const r of reports) {
        const addr = r[0] | (r[1] << 8) | (r[2] << 16);
        sent.set(r.subarray(HK.HEADER_SIZE, HK.HEADER_SIZE + Math.min(HK.BLOCK_SIZE, ref.length - addr)), addr);
      }
      if (!sent.every((v, a) => v === ref[a])) fail('the reports do not rebuild the image');
      if (diffs) fail(`${diffs} bytes differ from objcopy`);
      else console.log('  matches objcopy byte for byte');
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  }
}

// Boot report: TyTools sends lo = rtc << 15, hi = rtc >> 17 (32 bits each, LE)
const rtc = 1793912559;
const r = HK.bootReport(rtc);
const lo = (rtc * 32768) % 2 ** 32;
const hi = Math.floor(rtc / 2 ** 17);
const le = (v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
const want = [0xb7, 0x31, 0xc2, 0x89, ...le(lo), ...le(hi)];
if (r[0] !== 255 || r[1] !== 255 || r[2] !== 255 || !want.every((v, i) => r[HK.HEADER_SIZE + i] === v)) fail('boot report encoding');
else console.log('boot report: matches TyTools');

// Bad input is refused
for (const [bad, why] of [[':10000000\n', 'short line'], [':0400000560001000870\n', 'odd length'],
  [':020000046000' + '9B\n:00000001FF\n', 'checksum'], [':0200000460009A\n:0100000000FF\n', 'no end record']]) {
  try { HK.parseHex(bad); fail(`accepted a file with a ${why}`); } catch (e) { if (!(e instanceof HK.HexError)) throw e; }
}
console.log(failed ? 'FAILED' : 'all checks passed');
process.exit(failed ? 1 : 0);
