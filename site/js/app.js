// The page: pick the two archives and a target, build, download. All work is in js/build.js.
import SevenZip from '../vendor/7z-wasm/7zz.es6.js';
import { buildPayload, assemble } from './build.js';
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

// Downloads the payload-free engine of a target and checks it against engines.json.
async function engineFor(t) {
  const res = await fetch(t.engine);
  if (!res.ok) throw new Error(`${t.engine}: HTTP ${res.status}`);
  const blob = await res.blob();
  if (blob.size !== t.size || await sha256OfBlob(blob) !== t.sha256)
    throw new Error(`${t.engine}: the downloaded engine does not match its checksum; reload the page and try again`);
  return blob;
}

function offer(t, blob) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = t.output;
  a.textContent = `${t.output} (${t.label}, ${MiB(blob.size)})`;
  const li = document.createElement('li');
  li.append(a);
  ui.downloads.append(li);
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
      progress(`Fetching the ${t.label} engine`, 0, 1);
      const game = assemble(await engineFor(t), payload);
      progress(`Checking the ${t.label} game`, 0, game.size);
      let done = 0;
      const same = await sha256OfBlob(game, n => progress(`Checking the ${t.label} game`, (done += n), game.size)) === t.game_sha256;
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
