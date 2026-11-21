#!/usr/bin/env node
// usdcat CLI — LightUSD WASM
//
// Load a USD asset, (optionally) flatten/compose it, and write USDA / USDC / USDZ.
// A JS/WASM counterpart to the native `lusdcat` tool (tests/core/lusdcat.cc),
// reusing the same compose-to-fixed-point flatten path as usdzconvert.
//
// Usage:
//   node cli/usdcat.js <input.usd|input.usdz|input-dir> [options]
//
// Options:
//   -o, --output <file>        Output file. Without -o, flattened USDA is printed
//                              to stdout (cat-like). usdc/usdz require -o.
//   --output-format <fmt>      usda | usdc | usdz. Overrides extension inference.
//   --no-flatten               Load + write without composing arcs (default: flatten).
//   --root <relpath>           Root layer within a dir/usdz (default: auto-detect).
//   --max-usdc-mb <N>          Raise the USDC writer size cap to N MB (0 = default).
//   --max-mem-mb <N>           Raise the USDC writer memory cap to N MB (0 = default).
//   -v, --verbose              Verbose logging (to stderr).
//   -h, --help                 Show this help.
//
// Notes:
//   - Very large USDA may exceed the JS engine's max string length; use
//     `--output-format usdc` for huge scenes.
//   - Native-only features (validate, inspect, dumpcrate, comp-graph,
//     extract-variants, json, time/path/attr filters) are intentionally omitted.
//
// Exit codes: 0 ok, 2 usage/IO/parse error.
//
// Supported inputs: .usd, .usda, .usdc, .usdz, folder.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  loadWasm,
  unpackUSDZ,
  rootUsdFromMap,
  isUsdName,
} from '../src/usdzconvert.js';

// Load the Emscripten glue directly (no three.js / vite-node dependency) so the
// CLI runs with plain `node`. Honors LIGHTUSD_WASM64=1 like usdzconvert.js.
function selectWasmGlue() {
  if (process.env.LIGHTUSD_WASM64 === '1') {
    const url64 = new URL('../src/lightusd/lightusd_64.js', import.meta.url);
    if (fs.existsSync(url64)) return url64.href;
    console.error('[usdcat] LIGHTUSD_WASM64=1 but lightusd_64.js not found; using wasm32.');
  }
  return new URL('../src/lightusd/lightusd.js', import.meta.url).href;
}

function printUsage() {
  console.log(`usdcat — LightUSD WASM (load + flatten + write USDA/USDC/USDZ)

USAGE:
  node cli/usdcat.js <input.usd|input.usdz|input-dir> [options]

OPTIONS:
  -o, --output <file>     Output file. Without -o, flattened USDA prints to stdout.
  --output-format <fmt>   usda | usdc | usdz (overrides -o extension inference).
  --no-flatten            Load + write without composing arcs (default: flatten).
  --root <relpath>        Root layer within a dir/usdz (default: auto-detect).
  --max-usdc-mb <N>       Raise the USDC writer size cap to N MB (0 = default).
  --max-mem-mb <N>        Raise the USDC writer memory cap to N MB (0 = default).
  -v, --verbose           Verbose logging (to stderr).
  -h, --help              Show this help.

EXAMPLES:
  node cli/usdcat.js scene.usdz | head -40
  node cli/usdcat.js ./model_dir -o flat.usda
  node cli/usdcat.js scene.usdz -o flat.usdc --max-usdc-mb 4096
  node cli/usdcat.js scene.usdz --no-flatten -o passthrough.usda

SUPPORTED INPUTS: .usd, .usda, .usdc, .usdz, folder`);
}

// fs.writeFileSync caps a single write at 2^31-1 bytes; a flattened multi-GB
// scene exceeds that. Write in <2 GiB chunks instead. (Same as usdzconvert.js.)
function writeFileChunked(p, bytes) {
  const CHUNK = 1 << 30; // 1 GiB
  const fd = fs.openSync(p, 'w');
  try {
    for (let pos = 0; pos < bytes.length; pos += CHUNK) {
      const n = Math.min(CHUNK, bytes.length - pos);
      fs.writeSync(fd, bytes, pos, n, pos);
    }
  } finally {
    fs.closeSync(fd);
  }
}

// Recursively read a directory into a Map<relpath, Uint8Array>.
function readDirToMap(dir) {
  const map = new Map();
  const walk = (cur, prefix) => {
    for (const entry of fs.readdirSync(cur, { withFileTypes: true })) {
      const full = path.join(cur, entry.name);
      const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(full, rel);
      else if (entry.isFile()) map.set(rel, fs.readFileSync(full));
    }
  };
  walk(dir, '');
  return map;
}

export function parseArgs(argv) {
  const args = argv ?? process.argv.slice(2);
  const o = {
    input: null, output: null, format: '', flatten: true, root: null,
    maxUsdcMb: 0, maxMemMb: 0, verbose: false,
  };
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === '-h' || a === '--help') { printUsage(); process.exit(0); }
    else if (a === '-o' || a === '--output') o.output = args[++i];
    else if (a === '--output-format') {
      o.format = String(args[++i] || '').toLowerCase();
      if (!['usda', 'usdc', 'usdz'].includes(o.format)) {
        console.error('Error: --output-format must be usda, usdc, or usdz');
        process.exit(2);
      }
    }
    else if (a === '--no-flatten') o.flatten = false;
    else if (a === '--root') o.root = args[++i];
    else if (a === '--max-usdc-mb') o.maxUsdcMb = parseInt(args[++i], 10) || 0;
    else if (a === '--max-mem-mb') o.maxMemMb = parseInt(args[++i], 10) || 0;
    else if (a === '-v' || a === '--verbose') o.verbose = true;
    else if (a.startsWith('-')) { console.error(`Error: unknown option: ${a}`); process.exit(2); }
    else if (o.input === null) o.input = a;
    else { console.error(`Error: unexpected argument: ${a}`); process.exit(2); }
  }
  return o;
}

// Resolve output format: explicit --output-format > -o extension > default usda.
export function resolveFormat(o) {
  if (o.format) return o.format;
  if (o.output) {
    const ext = path.extname(o.output).toLowerCase();
    if (ext === '.usda') return 'usda';
    if (ext === '.usdc') return 'usdc';
    if (ext === '.usdz') return 'usdz';
  }
  return 'usda';
}

// Build the asset map + root layer name from a path (file, .usdz, or directory).
export function buildInput(inputPath, preferredRoot) {
  const st = fs.statSync(inputPath);
  let map;
  if (st.isDirectory()) {
    map = readDirToMap(inputPath);
  } else if (/\.usdz$/i.test(inputPath)) {
    const { entries } = unpackUSDZ(fs.readFileSync(inputPath));
    map = entries;
    if (!preferredRoot) preferredRoot = entries.keys().next().value;
    if (!isUsdName(preferredRoot)) throw new Error("The first USDZ entry must be a USD layer.");
  } else {
    // Single layer: read its sibling directory so external refs/sublayers resolve.
    const base = path.basename(inputPath);
    map = readDirToMap(path.dirname(inputPath));
    if (!map.has(base)) map.set(base, fs.readFileSync(inputPath));
    if (!preferredRoot) preferredRoot = base;
  }
  const rootName = preferredRoot && map.has(preferredRoot)
    ? preferredRoot
    : rootUsdFromMap(map, preferredRoot);
  if (!rootName) throw new Error('No root USD layer found in input.');
  return { map, rootName };
}

// Core: register assets, load, flatten, export. Returns {format, text?|bytes?}.
// Exported so tests can drive it without spawning a process.
export function runUsdcat(native, { map, rootName, flatten = true, format = 'usda',
                                    maxUsdcMb = 0, maxMemMb = 0, log = () => {} }) {
  let rootBytes = map.get(rootName);
  if (!rootBytes) throw new Error(`Missing root layer: ${rootName}`);
  if (flatten) {
    const session = new native.FlattenSession();
    try {
      if (maxMemMb > 0) {
        const limit = session.setMaxInputBytes(maxMemMb * 1024 * 1024);
        if (!limit.success) throw new Error(limit.error);
      }
      if (maxUsdcMb > 0) {
        const limit = session.setMaxOutputBytes(maxUsdcMb * 1024 * 1024);
        if (!limit.success) throw new Error(limit.error);
      }
      const begun = session.begin(rootBytes, rootName, true);
      if (!begun.success) throw new Error(begun.error);
      session.setAssetPathRemap(Object.fromEntries([...map.keys()].map(key => [key, key])));
      const provided = new Set();
      for (;;) {
        const step = session.step(null);
        if (!step.success) throw new Error(step.error || 'Composition failed');
        if (step.status === 'done') {
          if (step.compositionErrors?.length) throw new Error(step.compositionErrors.join('\n'));
          if (!step.data?.length) throw new Error('Composition produced an empty layer');
          rootBytes = new Uint8Array(step.data);
          break;
        }
        if (step.status === 'ready') continue;
        if (step.status !== 'need-layer') throw new Error(`Unexpected composition status: ${step.status}`);
        if (!map.has(step.key)) throw new Error(`Missing USD dependency: ${step.key}`);
        if (provided.has(step.key)) throw new Error(`Repeated USD dependency: ${step.key}`);
        const result = session.provideLayer(step.key, map.get(step.key));
        if (!result.success) throw new Error(result.error);
        provided.add(step.key);
      }
    } finally { session.delete(); }
  }
  const usd = new native.USDZConverterNative();
  try {
    if ((maxUsdcMb > 0 || maxMemMb > 0) && typeof usd.setUSDCExportLimitMB === 'function') {
      usd.setUSDCExportLimitMB(maxUsdcMb, maxMemMb);
    }
    // Register every non-root layer/asset (textures included, for usdz export);
    // skip nested .usdz archives, which are self-contained.
    for (const [name, bytes] of map) {
      if (name === rootName || /\.usdz$/i.test(name)) continue;
      usd.setAsset(name, bytes);
    }
    if (!usd.loadFromBinary(rootBytes, rootName)) {
      throw new Error('Failed to load USD: ' + usd.error());
    }
    if (typeof usd.warn === 'function' && usd.warn()) log('WARN: ' + usd.warn());

    if (format === 'usda') {
      const text = usd.exportAsUSDA();
      if (!text) throw new Error('USDA export failed: ' + usd.error());
      return { format, text };
    }
    if (format === 'usdc') {
      const data = usd.exportAsUSDC();
      if (!data) throw new Error('USDC export failed: ' + usd.error());
      return { format, bytes: new Uint8Array(data) };
    }
    // usdz: exportAsUSDZ() returns a view into the WASM heap — copy immediately.
    const view = usd.exportAsUSDZ();
    if (!view) throw new Error('USDZ export failed: ' + usd.error());
    return { format, bytes: new Uint8Array(view) };
  } finally {
    usd.delete();
  }
}

async function main() {
  // Behave like a real `cat`: a closed downstream pipe (e.g. `| head`) should
  // exit quietly instead of throwing an EPIPE stack trace.
  process.stdout.on('error', (e) => { if (e && e.code === 'EPIPE') process.exit(0); throw e; });

  const o = parseArgs();
  if (!o.input) { console.error('Error: input is required'); printUsage(); process.exit(2); }
  if (!fs.existsSync(o.input)) { console.error('Error: input not found: ' + o.input); process.exit(2); }

  const format = resolveFormat(o);
  if ((format === 'usdc' || format === 'usdz') && !o.output) {
    console.error(`Error: --output is required for ${format} output`);
    process.exit(2);
  }

  const log = o.verbose ? (m) => console.error(m) : () => {};

  let map, rootName;
  try {
    ({ map, rootName } = buildInput(o.input, o.root));
  } catch (err) {
    console.error('Error: ' + (err && err.message ? err.message : err));
    process.exit(2);
  }
  log(`root layer: ${rootName} (${map.size} asset${map.size === 1 ? '' : 's'}), ` +
      `flatten=${o.flatten}, format=${format}`);

  const native = await loadWasm(() => import(selectWasmGlue()));

  let res;
  try {
    res = runUsdcat(native, {
      map, rootName, flatten: o.flatten, format,
      maxUsdcMb: o.maxUsdcMb, maxMemMb: o.maxMemMb, log,
    });
  } catch (err) {
    console.error('Error: ' + (err && err.message ? err.message : err));
    process.exit(2);
  }

  if (res.format === 'usda') {
    if (o.output) { fs.writeFileSync(o.output, res.text, 'utf-8'); log(`Wrote ${o.output} (${res.text.length} chars)`); }
    else process.stdout.write(res.text);
  } else {
    writeFileChunked(o.output, res.bytes);
    log(`Wrote ${o.output} (${res.bytes.length} bytes)`);
  }
}

// Run only when invoked directly (not when imported by tests).
// Realpath both sides -- see the matching note in cli/urdf-to-usd.js: a
// checkout reached through a symlinked parent otherwise makes this false and
// the CLI silently does nothing and exits 0.
const invokedDirectly = (() => {
  if (!process.argv[1]) return false;
  const self = fileURLToPath(import.meta.url);
  const invoked = path.resolve(process.argv[1]);
  if (self === invoked) return true;
  try {
    return fs.realpathSync(self) === fs.realpathSync(invoked);
  } catch {
    return false;
  }
})();
if (invokedDirectly) {
  main().catch((err) => { console.error('Error:', err && err.message ? err.message : err); process.exit(2); });
}
