// The game build, in the browser: original archives -> canonical data tree -> SAYOPAK payload -> engine + payload.
// It reproduces `make data` + `sayopack create` + `sayopack append` exactly (docs/PAYLOAD.md); the Node test
// tools/site/build.test.mjs checks the result byte for byte against the Makefile's output.

import { Sha256, crc32, sha256OfBlob } from './hash.js';
import { zipEntries, zipRead } from './zip.js';

// The only inputs accepted: the JA download edition and the English patch v1.1, exactly as distributed.
export const KNOWN_INPUTS = {
  ja: { size: 488004208, sha256: 'fb61c609b6e11ea80efde830f2638f3b4aae53c72406edd5860b59519228a9ec',
        what: 'Sayonara o Oshiete ~Comment te Dire Adieu~ (Windows, JA download edition)' },
  en: { size: 7209738, sha256: '88e6795ac5d656fe7be2956e5fc29f643ecc9d6e1aaafd3df5af6c289320e9fe',
        what: 'Sayonara wo Oshiete English Patch v1.1' },
};

const READ_STEP = 8 << 20;
const MIN_G00 = 600;                                  // 617 in the JA edition
const JA_SKIP = /\.(exe|ico|ini|txt|dll)$|^savedata$/; // top-level GameData names the engine does not need

// ---------------------------------------------------------------------------------------------- inputs --

// Checks a supplied archive against KNOWN_INPUTS[kind]; throws with a readable message on a mismatch.
export async function verifyInput(file, kind, onBytes) {
  const want = KNOWN_INPUTS[kind];
  if (file.size !== want.size)
    throw new Error(`${file.name}: ${file.size} bytes, but ${want.what} is ${want.size} bytes`);
  const got = await sha256OfBlob(file, onBytes);
  if (got !== want.sha256) throw new Error(`${file.name}: SHA-256 ${got} does not match ${want.what}`);
  return got;
}

// ----------------------------------------------------------------------------------------- data tree --

// ja/...: the GameData folder of the JA zip (the folder holding Scene.pck), minus what the engine never reads.
export async function jaFiles(zipFile) {
  const entries = await zipEntries(zipFile);
  const scene = entries.find(e => !e.dir && /(^|\/)scene\.pck$/i.test(e.name));
  if (!scene) throw new Error('no Scene.pck in the JA archive');
  const base = scene.name.slice(0, scene.name.length - 'Scene.pck'.length);
  const files = [];
  for (const e of entries) {
    if (e.dir || !e.name.startsWith(base)) continue;
    const rel = e.name.slice(base.length);
    if (JA_SKIP.test(rel.split('/')[0].toLowerCase())) continue;
    files.push({ path: 'ja/' + rel, read: () => zipRead(zipFile, e) });
  }
  const g00 = files.filter(f => /\.g00$/i.test(f.path)).length;
  if (g00 < MIN_G00) throw new Error(`only ${g00} g00 files in the JA data (expected 617)`);
  return files;
}

// en/...: the EN patch overrides, from the NSIS installer inside the patch zip (unpacked with 7-Zip/WASM).
export async function enFiles(zipFile, sevenZip) {
  const sz = await sevenZip({ print: () => {}, printErr: () => {} });
  sz.FS.writeFile('/en.zip', new Uint8Array(await zipFile.arrayBuffer()));
  sz.callMain(['x', '-y', '-bd', '-o/x1', '/en.zip']);
  const exe = walk(sz, '/x1').find(p => /\.exe$/i.test(p));
  if (!exe) throw new Error('no installer .exe in the EN patch archive');
  sz.callMain(['x', '-y', '-bd', '-o/x2', exe]);
  const all = walk(sz, '/x2');
  const pick = name => all.find(p => p.slice(p.lastIndexOf('/') + 1) === name);
  const take = (from, to) => ({ path: to, read: async () => new Blob([sz.FS.readFile(from)]) });
  const files = [];
  for (const p of ['Scene.pck', 'Gameexe.dat']) {
    const f = pick(p + '.new');
    if (!f) throw new Error(`EN patch: ${p}.new not found`);
    files.push(take(f, 'en/' + p));
  }
  const font = pick('sayooshieng-Regular.ttf.new');
  if (font) files.push(take(font, 'en/font/sayooshieng-Regular.ttf'));
  const guide = pick('routeguide.txt');
  if (guide) files.push(take(guide, 'en/routeguide.txt'));
  for (const p of all.filter(p => p.endsWith('.g00.new')))
    files.push(take(p, 'en/g00/' + p.slice(p.lastIndexOf('/') + 1, -'.new'.length)));
  return files;
}

function walk(sz, dir) {
  return sz.FS.readdir(dir).filter(n => n !== '.' && n !== '..').flatMap(n => {
    const p = `${dir}/${n}`;
    return sz.FS.isDir(sz.FS.stat(p).mode) ? walk(sz, p) : [p];
  });
}

// Materialises each file once: { path, blob, size, crc, sha256 }.
export async function digestFiles(files, onFile) {
  const out = [];
  for (const f of files) {
    const blob = await f.read();
    let crc = 0;
    const sha = new Sha256();
    for (let at = 0; at < blob.size; at += READ_STEP) {
      const chunk = new Uint8Array(await blob.slice(at, at + READ_STEP).arrayBuffer());
      crc = crc32(crc, chunk);
      sha.update(chunk);
    }
    out.push({ path: f.path, blob, size: blob.size, crc, sha256: sha.hex() });
    onFile(out.length, files.length, f.path);
  }
  return out;
}

// manifest.txt, exactly as `make data` writes it. It holds no time stamps or file names, so the same archives
// always give the same bytes: builds are reproducible and comparable with engines.json's reference hashes.
export function manifest(tree, sources) {
  const lines = [
    '# sayooshi data tree. Format 1. Layout: docs/PAYLOAD.md',
    'format: 1',
    `ja_source: sha256=${sources.ja.sha256}`,
    `en_source: sha256=${sources.en.sha256}`,
    'languages: ja en',
    '# sha256 size path',
    ...sortByPath(tree).map(f => `${f.sha256} ${f.size} ${f.path}`),
  ];
  return lines.join('\n') + '\n';
}

// ------------------------------------------------------------------------------------------- SAYOPAK --

const enc = new TextEncoder();
const ALIGN = 16, HEADER_SIZE = 16, TRAILER_SIZE = 48, ENTRY_FIXED = 24;

// Byte-wise order of the UTF-8 paths (LC_ALL=C sort).
function sortByPath(files) {
  const key = f => enc.encode(f.path);
  return [...files].sort((a, b) => {
    const x = key(a), y = key(b);
    for (let i = 0; i < Math.min(x.length, y.length); i++) if (x[i] !== y[i]) return x[i] - y[i];
    return x.length - y.length;
  });
}

// The payload as Blob parts (no copy of the file data): header, 16-byte aligned files, table, trailer.
export function sayopak(files) {
  const sorted = sortByPath(files);
  const header = new Uint8Array(HEADER_SIZE);
  header.set(enc.encode('SAYOPAKH'));
  new DataView(header.buffer).setUint32(8, 1, true);
  const parts = [header];
  const entries = [];
  let pos = HEADER_SIZE;
  for (const f of sorted) {
    const pad = (ALIGN - pos % ALIGN) % ALIGN;
    if (pad) parts.push(new Uint8Array(pad));
    pos += pad;
    entries.push({ offset: pos, f, path: enc.encode(f.path) });
    parts.push(f.blob);
    pos += f.size;
  }
  const table = new Uint8Array(entries.reduce((n, e) => n + ENTRY_FIXED + e.path.length, 0));
  const tv = new DataView(table.buffer);
  let t = 0;
  for (const e of entries) {
    tv.setBigUint64(t, BigInt(e.offset), true);
    tv.setBigUint64(t + 8, BigInt(e.f.size), true);
    tv.setUint32(t + 16, e.f.crc, true);
    tv.setUint16(t + 20, e.path.length, true);
    tv.setUint16(t + 22, 0, true);
    table.set(e.path, t + ENTRY_FIXED);
    t += ENTRY_FIXED + e.path.length;
  }
  const tableOffset = pos;
  pos += table.length;
  const payloadSize = pos + TRAILER_SIZE;
  const trailer = new Uint8Array(TRAILER_SIZE);
  const rv = new DataView(trailer.buffer);
  rv.setUint32(0, 1, true);
  rv.setUint32(4, entries.length, true);
  rv.setBigUint64(8, BigInt(tableOffset), true);
  rv.setBigUint64(16, BigInt(table.length), true);
  rv.setUint32(24, crc32(0, table), true);
  rv.setUint32(28, 0, true);
  rv.setBigUint64(32, BigInt(payloadSize), true);
  trailer.set(enc.encode('SAYOPAK1'), 40);
  parts.push(table, trailer);
  return { parts, size: payloadSize, count: entries.length };
}

// ------------------------------------------------------------------------------------------- the build --

// Everything up to the payload. opts: { ja: File, en: File, sevenZip, progress(stage, done, total) }.
export async function buildPayload({ ja, en, sevenZip, progress = () => {} }) {
  const total = ja.size + en.size;
  let hashed = 0;
  const onBytes = n => progress('Checking the archives', (hashed += n), total);
  const sources = {
    ja: { sha256: await verifyInput(ja, 'ja', onBytes) },
    en: { sha256: await verifyInput(en, 'en', onBytes) },
  };
  progress('Reading the archives', 0, 1);
  const listed = [...await jaFiles(ja), ...await enFiles(en, sevenZip)];
  const tree = await digestFiles(listed, (i, n) => progress('Unpacking the game files', i, n));
  const text = new Blob([manifest(tree, sources)]);
  const bytes = new Uint8Array(await text.arrayBuffer());
  const man = { path: 'manifest.txt', blob: text, size: bytes.length, crc: crc32(0, bytes) };
  return sayopak([...tree, man]);
}

// engine (a payload-free build) + payload = the self-contained game.
export function assemble(engineBlob, payload) {
  return new Blob([engineBlob, ...payload.parts], { type: 'application/octet-stream' });
}
