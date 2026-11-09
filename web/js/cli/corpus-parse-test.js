// corpus-parse-test.js — WASM-CLI parse regression over the usd-wg/assets corpus.
//
// Loads every .usd/.usda/.usdc/.usdz under the corpus through the WASM module
// (LayerDocument.load for next, loadAsLayerFromBinary for legacy: a pure parse,
// no composition or render-scene conversion — the WASM analogue of the native
// `lusdcat -l` parse regression), classifies PASS/FAIL, and (optionally)
// gates on a max failure count for use as a regression test.
//
// Run:  npm run test:corpus           (vite-node resolves the 'lightusd/' alias)
//   or  vite-node cli/corpus-parse-test.js [--assets DIR] [--max-fail N] [--wasm64]
//
// Corpus dir: --assets, else $USD_WG_ASSETS_DIR, else the default below.
// If the corpus is absent the test SKIPS (exit 0) so CI without it still passes.

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import fs from 'node:fs';
import path from 'node:path';

const DEFAULT_ASSETS = process.env.USD_WG_ASSETS_DIR || '';
const EXTS = new Set(['.usd', '.usda', '.usdc', '.usdz']);

function parseArgs(argv) {
  const a = {
    assets: process.env.USD_WG_ASSETS_DIR || DEFAULT_ASSETS,
    maxFail: Infinity,
    useMemory64: false,
    limit: Infinity, // for quick local runs
  };
  for (let i = 0; i < argv.length; i++) {
    switch (argv[i]) {
      case '--assets': a.assets = argv[++i]; break;
      case '--max-fail': a.maxFail = parseInt(argv[++i], 10); break;
      case '--limit': a.limit = parseInt(argv[++i], 10); break;
      case '--wasm64': a.useMemory64 = true; break;
      case '--wasm32': a.useMemory64 = false; break;
      case '-h': case '--help':
        console.log('Usage: vite-node cli/corpus-parse-test.js [--assets DIR] [--max-fail N] [--wasm64] [--limit N]');
        process.exit(0);
        break;
      default:
        console.error('Unknown arg: ' + argv[i]); process.exit(2);
    }
  }
  return a;
}

function walk(dir, out = []) {
  for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, ent.name);
    if (ent.isDirectory()) walk(p, out);
    else if (EXTS.has(path.extname(ent.name).toLowerCase())) out.push(p);
  }
  return out;
}

async function main() {
  const a = parseArgs(process.argv.slice(2));

  // Graceful skip when the corpus is not present.
  if (!fs.existsSync(a.assets) || !fs.statSync(a.assets).isDirectory()) {
    console.log(`SKIPPED: usd-wg asset corpus not found at ${a.assets} ` +
      `(set USD_WG_ASSETS_DIR or pass --assets).`);
    process.exit(0);
  }

  let files = walk(a.assets).sort();
  if (Number.isFinite(a.limit)) files = files.slice(0, a.limit);
  if (!files.length) {
    console.log(`SKIPPED: no USD files under ${a.assets}.`);
    process.exit(0);
  }

  const loader = new LightUSDLoader();
  await loader.init({ useMemory64: a.useMemory64 });
  loader.setMaxMemoryLimitMB(2048);
  const native = loader.native_;
  const layerType = loader.nextOnlyNative_ ? 'LayerDocument' : 'LightUSDLoaderNative';
  if (!native || typeof native[layerType] !== 'function') {
    console.error(`WASM module / ${layerType} not available.`);
    process.exit(2);
  }

  const t0 = performance.now();
  let pass = 0, fail = 0;
  const failures = [];
  for (const f of files) {
    const bytes = new Uint8Array(fs.readFileSync(f));
    const u = new native[layerType]();
    let ok = false, err = '';
    try {
      if (layerType === 'LayerDocument') {
        const result = u.load(bytes);
        ok = result.success;
        if (!ok) err = result.error || 'unknown';
      } else {
        u.setMaxMemoryLimitMB(2048);
        ok = u.loadAsLayerFromBinary(bytes, path.basename(f));
        if (!ok) err = (typeof u.error === 'function') ? u.error() : 'unknown';
      }
    } catch (e) {
      ok = false; err = String(e && e.message || e);
    } finally {
      if (typeof u.delete === 'function') u.delete();
    }
    if (ok) pass++;
    else { fail++; failures.push({ rel: path.relative(a.assets, f), err: (err || '').split('\n')[0].slice(0, 200) }); }
  }
  const sec = ((performance.now() - t0) / 1000).toFixed(1);

  console.log(`\n== WASM corpus parse (${layerType}) ==`);
  console.log(`  corpus : ${a.assets}`);
  console.log(`  wasm   : ${a.useMemory64 ? 'wasm64' : 'wasm32'}`);
  console.log(`  files  : ${files.length}  PASS ${pass}  FAIL ${fail}  (${sec}s)`);
  if (failures.length) {
    console.log(`\n  Failures (${failures.length}):`);
    for (const x of failures.slice(0, 40)) console.log(`    ${x.rel}\n      ${x.err}`);
    if (failures.length > 40) console.log(`    … and ${failures.length - 40} more`);
  }

  if (Number.isFinite(a.maxFail) && fail > a.maxFail) {
    console.error(`\nREGRESSION: ${fail} WASM parse failures > --max-fail ${a.maxFail}.`);
    process.exit(1);
  }
  if (Number.isFinite(a.maxFail)) {
    console.log(`Regression gate OK: ${fail} failures <= --max-fail ${a.maxFail}.`);
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
