// The page: pick the two archives and a target, build, download. All work is in js/build.js.
import SevenZip from '../vendor/7z-wasm/7zz.es6.js';
import { buildPayload, assemble, appZip } from './build.js';
import { sha256OfBlob } from './hash.js';

const $ = id => document.getElementById(id);
const ui = { ja: $('ja'), en: $('en'), target: $('target'), build: $('build'), progress: $('progress'),
             stage: $('stage'), log: $('log'), downloads: $('downloads') };
const sevenZip = opts => SevenZip({ ...opts, locateFile: f => new URL(`../vendor/7z-wasm/${f}`, import.meta.url).href });
const MiB = n => `${(n / 1048576).toFixed(1)} MB`;

let targets = [];

function log(line) { ui.log.textContent += line + '\n'; }

function progress(stage, done, total) {
  ui.stage.textContent = stage;
  ui.progress.max = total || 1;
  ui.progress.value = done;
}

function ready() {
  ui.build.disabled = !(ui.ja.files.length && ui.en.files.length && ui.target.value);
}

async function loadTargets() {
  const res = await fetch('engines/engines.json');
  if (!res.ok) throw new Error(`engines/engines.json: HTTP ${res.status}`);
  targets = (await res.json()).targets;
  ui.target.innerHTML = '';
  for (const t of targets) ui.target.add(new Option(t.label, t.id));
  if (targets.length > 1) ui.target.add(new Option('All of the above', '*'));
  ready();
}

// Downloads a payload-free engine file ({ engine, size, sha256 }) and checks it against engines.json.
async function fetchChecked(f) {
  const res = await fetch(f.engine);
  if (!res.ok) throw new Error(`${f.engine}: HTTP ${res.status}`);
  const blob = await res.blob();
  if (blob.size !== f.size || await sha256OfBlob(blob) !== f.sha256)
    throw new Error(`${f.engine}: the downloaded engine does not match its checksum; reload the page and try again`);
  return blob;
}

// The finished game of a target and whether it is identical to the reference build.
async function gameFor(t, payload) {
  progress(`Fetching the ${t.label} engine`, 0, 1);
  if (t.kind === 'app') {
    const files = [];
    for (const f of t.files) files.push({ path: f.path, mode: f.mode, blob: await fetchChecked(f) });
    let done = 0;
    const { zip, payloadSha256 } = await appZip(files, payload, t.payload_path,
      n => progress(`Packing the ${t.label} app`, (done += n), payload.size));
    return { game: zip, same: payloadSha256 === t.payload_sha256 };
  }
  const game = assemble(await fetchChecked(t), payload);
  let done = 0;
  const sha = await sha256OfBlob(game, n => progress(`Checking the ${t.label} game`, (done += n), game.size));
  return { game, same: sha === t.game_sha256 };
}

function offer(t, blob) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = t.output;
  a.textContent = `${t.output} (${t.label}, ${MiB(blob.size)})`;
  ui.downloads.append(a, document.createElement('br'));
}

async function build() {
  ui.build.disabled = true;
  ui.log.textContent = '';
  ui.downloads.innerHTML = '';
  const chosen = ui.target.value === '*' ? targets : targets.filter(t => t.id === ui.target.value);
  const t0 = performance.now();
  try {
    const payload = await buildPayload({ ja: ui.ja.files[0], en: ui.en.files[0], sevenZip, progress });
    log(`Game data packed: ${payload.count} files, ${MiB(payload.size)}.`);
    for (const t of chosen) {
      const { game, same } = await gameFor(t, payload);
      log(same ? `${t.label}: ready, identical to the reference build.`
               : `${t.label}: ready, but it differs from the reference build (SHA-256 mismatch).`);
      offer(t, game);
    }
    progress(`Done in ${((performance.now() - t0) / 1000).toFixed(0)} s. Download below.`, 1, 1);
  } catch (err) {
    progress('Failed.', 0, 1);
    log(`Error: ${err.message}`);
  } finally {
    ready();
  }
}

ui.ja.addEventListener('change', ready);
ui.en.addEventListener('change', ready);
ui.target.addEventListener('change', ready);
ui.build.addEventListener('click', build);
loadTargets().catch(err => log(`Error: ${err.message}`));
