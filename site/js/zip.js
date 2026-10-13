// Minimal ZIP reader for a Blob/File: central directory, stored and deflated entries (the original JA zip).
// Entries are inflated with the platform's DecompressionStream, so a 500 MB archive is never read at once.
// Names without the UTF-8 flag are decoded as CP932 (Shift_JIS), like `7z -mcp=932`.

const SIG_EOCD = 0x06054b50, SIG_CDIR = 0x02014b50, SIG_LOCAL = 0x04034b50;
const EOCD_MIN = 22, EOCD_SEARCH = EOCD_MIN + 0xffff;

async function bytes(blob, start, end) {
  return new DataView(await blob.slice(start, end).arrayBuffer());
}

function decodeName(raw, utf8) {
  return new TextDecoder(utf8 ? 'utf-8' : 'shift_jis').decode(raw);
}

// The end-of-central-directory record: { count, cdOffset, cdSize }.
async function readEocd(blob) {
  const from = Math.max(0, blob.size - EOCD_SEARCH);
  const v = await bytes(blob, from, blob.size);
  for (let i = v.byteLength - EOCD_MIN; i >= 0; i--) {
    if (v.getUint32(i, true) !== SIG_EOCD) continue;
    const count = v.getUint16(i + 10, true), cdSize = v.getUint32(i + 12, true), cdOffset = v.getUint32(i + 16, true);
    if (count === 0xffff || cdSize === 0xffffffff || cdOffset === 0xffffffff) throw new Error('ZIP64 archives are not supported');
    if (cdOffset + cdSize > blob.size) throw new Error('damaged zip: central directory outside the file');
    return { count, cdOffset, cdSize };
  }
  throw new Error('not a zip file (no end-of-central-directory record)');
}

// Lists the entries: [{ name, dir, method, crc, compSize, size, localOffset }].
export async function zipEntries(blob) {
  const { count, cdOffset, cdSize } = await readEocd(blob);
  const v = await bytes(blob, cdOffset, cdOffset + cdSize);
  const raw = new Uint8Array(v.buffer);
  const out = [];
  let p = 0;
  for (let i = 0; i < count; i++) {
    if (p + 46 > v.byteLength || v.getUint32(p, true) !== SIG_CDIR) throw new Error('damaged zip: central directory');
    const flags = v.getUint16(p + 8, true), method = v.getUint16(p + 10, true);
    const nameLen = v.getUint16(p + 28, true), extraLen = v.getUint16(p + 30, true), commentLen = v.getUint16(p + 32, true);
    const name = decodeName(raw.subarray(p + 46, p + 46 + nameLen), (flags & 0x800) !== 0);
    out.push({
      name, dir: name.endsWith('/'), method, flags,
      crc: v.getUint32(p + 16, true), compSize: v.getUint32(p + 20, true), size: v.getUint32(p + 24, true),
      localOffset: v.getUint32(p + 42, true),
    });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return out;
}

// The entry's contents as a Blob (inflated when deflated).
export async function zipRead(blob, e) {
  if (e.flags & 1) throw new Error(`${e.name}: encrypted zip entries are not supported`);
  const h = await bytes(blob, e.localOffset, e.localOffset + 30);
  if (h.getUint32(0, true) !== SIG_LOCAL) throw new Error(`damaged zip: local header of ${e.name}`);
  const start = e.localOffset + 30 + h.getUint16(26, true) + h.getUint16(28, true);
  const data = blob.slice(start, start + e.compSize);
  if (e.method === 0) return data;
  if (e.method !== 8) throw new Error(`${e.name}: unsupported zip method ${e.method}`);
  const out = await new Response(data.stream().pipeThrough(new DecompressionStream('deflate-raw'))).blob();
  if (out.size !== e.size) throw new Error(`damaged zip: ${e.name} inflates to ${out.size} bytes, expected ${e.size}`);
  return out;
}
