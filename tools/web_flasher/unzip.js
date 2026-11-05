// A small .zip reader for the page: lists a zip's files from its central
// directory and extracts one (stored or deflated), checking its CRC-32. Enough
// for Dirtywave's M8Firmware.zip; no ZIP64, encryption or multi-part archives.

const EOCD_SIG = 0x06054b50;
const CENTRAL_SIG = 0x02014b50;
const LOCAL_SIG = 0x04034b50;

export class ZipError extends Error {}

// [{ name, method, compressedSize, size, crc, offset }]. buf can be just the
// end of the zip (from a Range request) starting at byte `base` of it, as long
// as it holds the whole central directory.
export function listZip(buf, base = 0) {
  const bytes = new Uint8Array(buf);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  // The end-of-central-directory record is in the last 22 + 65535 bytes
  let eocd = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 22 - 65535); i--) {
    if (view.getUint32(i, true) === EOCD_SIG) { eocd = i; break; }
  }
  if (eocd < 0) throw new ZipError("The download isn't a zip file.");
  const count = view.getUint16(eocd + 10, true);
  let p = view.getUint32(eocd + 16, true) - base;
  if (p < 0) throw new ZipError("The zip's file list starts before the part that was read.");
  const names = new TextDecoder();
  const entries = [];
  for (let n = 0; n < count; n++) {
    if (p + 46 > bytes.length || view.getUint32(p, true) !== CENTRAL_SIG) throw new ZipError('The zip file is damaged.');
    const nameLen = view.getUint16(p + 28, true);
    const extraLen = view.getUint16(p + 30, true);
    const commentLen = view.getUint16(p + 32, true);
    entries.push({
      name: names.decode(bytes.subarray(p + 46, p + 46 + nameLen)),
      method: view.getUint16(p + 10, true),
      crc: view.getUint32(p + 16, true),
      compressedSize: view.getUint32(p + 20, true),
      size: view.getUint32(p + 24, true),
      offset: view.getUint32(p + 42, true),
    });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

let crcTable = null;
export function crc32(bytes) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let c = i;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[i] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = crcTable[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

async function inflateRawStream(data) {
  const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// The entry's contents as a Uint8Array. inflateRaw(Uint8Array) -> Uint8Array
// (or a promise of one) can be passed in where DecompressionStream lacks
// 'deflate-raw' (Node before 21).
export async function extractZip(buf, entry, inflateRaw = inflateRawStream) {
  const bytes = new Uint8Array(buf);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const p = entry.offset;
  if (view.getUint32(p, true) !== LOCAL_SIG) throw new ZipError('The zip file is damaged.');
  const start = p + 30 + view.getUint16(p + 26, true) + view.getUint16(p + 28, true);
  const data = bytes.subarray(start, start + entry.compressedSize);
  let out;
  if (entry.method === 0) out = data.slice();
  else if (entry.method === 8) out = await inflateRaw(data);
  else throw new ZipError(`${entry.name} is packed in a way the page can't unpack (method ${entry.method}).`);
  if (out.length !== entry.size || crc32(out) !== entry.crc) throw new ZipError(`${entry.name} didn't unpack intact.`);
  return out;
}

// Dirtywave's M8Firmware.zip: Firmware/Model01/M8_V6_6_3F_MODEL01 (TyUploader).hex
// and the same for Model02, beside .bin files (for the M8's own SD-card
// update) and macOS's __MACOSX/ copies. The .hex entry for an M8 model
// ('model01' or 'model02'), or null.
export function m8FirmwareHex(entries, model) {
  const tag = model.replace(/^model/i, '');
  const hexes = entries.filter((e) => {
    const parts = e.name.split('/');
    const base = parts[parts.length - 1];
    return !parts.includes('__MACOSX') && !base.startsWith('._') && /\.hex$/i.test(base)
      && new RegExp(`model[ _-]?${tag}`, 'i').test(e.name);
  });
  return hexes[0] || null;
}

// "M8_V6_6_3F_MODEL01 (TyUploader).hex" -> "6.6.3 F"; null if it doesn't say
export function m8FirmwareVersion(name) {
  const m = /V(\d+)_(\d+)_(\d+)([A-Z]*)_?MODEL/i.exec(name);
  return m ? `${m[1]}.${m[2]}.${m[3]}${m[4] ? ` ${m[4].toUpperCase()}` : ''}` : null;
}
