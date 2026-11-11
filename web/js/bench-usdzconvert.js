// SPDX-License-Identifier: Apache-2.0
// Browser bench for the scene->USDZ conversion pipeline (folder "upload" ->
// flatten/compose -> texture decode/resize/re-encode -> USDZ pack). URL-driven
// so Puppeteer can drive it (see tests/bench-usdzconvert-browser.mjs):
//
//   bench-usdzconvert.html?manifest=<url>&root=<rel>&resize=1024
//     &textureFormat=png&codec=browser|wasm&pipeline=memory|stream|stream-core
//     &wasm64=1&concurrency=8&textureMemoryBudget=1GB
//
// Results land in window.__usdzBench = { ready|error, timings, stats, gpu }.

import {
  convertFolderToUSDZ,
  convertSourceToUSDZStreaming,
  loadWasm,
  parseByteSize,
  parseUSDZEntries,
} from './src/usdzconvert.js';
import { createBrowserTextureProcessor } from './src/texture-processor-browser.mjs';

const statusEl = document.getElementById('status');
const lines = [];
function status(msg) {
  lines.push(msg);
  if (statusEl) statusEl.textContent = lines.slice(-30).join('\n');
}

function webglRenderer() {
  try {
    const gl = document.createElement('canvas').getContext('webgl2');
    const ext = gl.getExtension('WEBGL_debug_renderer_info');
    return ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL)
               : gl.getParameter(gl.RENDERER);
  } catch (_) {
    return 'unknown';
  }
}

async function fetchAll(entries, concurrency, onProgress) {
  const map = new Map();
  let next = 0;
  let bytes = 0;
  const run = async () => {
    while (next < entries.length) {
      const e = entries[next++];
      const res = await fetch(e.url);
      if (!res.ok) throw new Error(`fetch ${e.url}: HTTP ${res.status}`);
      const buf = new Uint8Array(await res.arrayBuffer());
      map.set(e.key, buf);
      bytes += buf.length;
      if ((map.size % 100) === 0) onProgress(map.size, entries.length, bytes);
    }
  };
  await Promise.all(Array.from({ length: Math.min(concurrency, entries.length) }, run));
  return { map, bytes };
}

async function main() {
  const params = new URLSearchParams(window.location.search);
  const manifestUrl = params.get('manifest');
  const rootPath = params.get('root');
  if (!manifestUrl || !rootPath) {
    throw new Error('manifest and root URL params are required');
  }
  const resize = Number(params.get('resize') || 0);
  const textureFormat = params.get('textureFormat') || 'keep';
  const codec = params.get('codec') || 'wasm';
  const pipeline = params.get('pipeline') || 'memory';  // memory | stream | stream-core
  const streaming = pipeline === 'stream' || pipeline === 'stream-core';
  const wasm64 = params.get('wasm64') !== '0';
  const concurrency = Number(params.get('concurrency') || 8);
  const textureMemoryBudget = parseByteSize(params.get('textureMemoryBudget'));
  const jpegQuality = Number(params.get('jpegQuality') || 90);
  const includeUnusedTextures = params.get('includeUnusedTextures') === '1';
  // Raise the conservative default USDC writer caps (0 = keep default). Needed
  // for scenes whose flattened root crate exceeds ~100 MB on wasm32.
  const maxUsdcMb = Number(params.get('maxUsdcMb') || 0);
  const maxMemMb = Number(params.get('maxMemMb') || 0);
  // keep + no resize = passthrough (otherwise 'keep' would still re-encode).
  const reencode = params.get('reencode') === '0' ? false
      : !(textureFormat === 'keep' && resize <= 0);

  const timings = {};
  const gpu = webglRenderer();
  status(`gpu: ${gpu}`);

  // 1. Manifest; for the in-memory pipeline, fetch the whole scene up front
  //    (folder-upload simulation). Streaming pipelines fetch lazily instead.
  let t = performance.now();
  status(`fetching manifest ${manifestUrl} ...`);
  const manifest = await (await fetch(manifestUrl)).json();
  let assetMap = null;
  let sceneBytes = 0;
  if (!streaming) {
    status(`fetching ${manifest.length} file(s) ...`);
    const fetched = await fetchAll(
        manifest, 16,
        (done, total, b) => status(`  fetched ${done}/${total} (${(b / 1e6).toFixed(0)} MB)`));
    assetMap = fetched.map;
    sceneBytes = fetched.bytes;
    status(`scene in memory: ${manifest.length} files, ${(sceneBytes / 1e6).toFixed(1)} MB`);
  }
  timings.fetchMs = performance.now() - t;

  // 2. WASM init.
  t = performance.now();
  const glue = wasm64 ? './src/lightusd/lightusd_64.js' : './src/lightusd/lightusd.js';
  const native = await loadWasm(() => import(/* @vite-ignore */ glue));
  timings.wasmInitMs = performance.now() - t;
  status(`wasm ready (${wasm64 ? 'wasm64' : 'wasm32'}, ${timings.wasmInitMs.toFixed(0)} ms)`);

  // 3. Convert (compose/flatten + textures + usdz pack).
  const tp = codec === 'browser' ? createBrowserTextureProcessor({
    concurrency,
    memoryBudgetBytes: textureMemoryBudget,
  }) : null;
  const convertOpts = {
    rootPath,
    maxTextureSize: resize,
    textureFormat,
    jpegQuality,
    maxUsdcMb,
    maxMemMb,
    reencode,
    textureProcessor: tp ? tp.processor : undefined,
    textureConcurrency: tp ? tp.concurrency : (streaming ? 4 : 0),
    includeUnusedTextures,
    log: (m) => { if ((lines.length % 25) === 0) status(String(m)); },
  };
  t = performance.now();
  let usdz = null, stats, usdzBytes = 0;
  if (streaming) {
    // Lazy source over HTTP: textures fetched -> processed -> zip-appended ->
    // released; only USD layers enter the wasm cache. A real app would point
    // the sink at a File System Access stream; the bench collects chunks and
    // only concatenates when the result is small enough to reload-validate
    // (a multi-GB output exceeds Chrome's single-ArrayBuffer cap).
    const byKey = new Map(manifest.map((e) => [e.key, e.url]));
    const source = {
      keys: [...byKey.keys()],
      fetch: async (key) => {
        const res = await fetch(byKey.get(key));
        if (!res.ok) throw new Error(`fetch ${byKey.get(key)}: HTTP ${res.status}`);
        const buf = new Uint8Array(await res.arrayBuffer());
        sceneBytes += buf.length;
        return buf;
      },
    };
    const chunks = [];
    const zipSink = { write: (c) => { usdzBytes += c.length; chunks.push(c.slice ? c.slice() : new Uint8Array(c)); } };
    const streamOpts = pipeline === 'stream-core'
      ? { ...convertOpts, pipeline: 'core', preloadUsdLayers: true, zipSink }
      : { ...convertOpts, zipSink };
    ({ stats } = await convertSourceToUSDZStreaming(native, source, streamOpts));
    const kValidatableBytes = wasm64 ? 4e9 : 800e6;  // reload copies into the wasm heap
    if (usdzBytes <= kValidatableBytes) {
      usdz = new Uint8Array(usdzBytes);
      let pos = 0;
      for (const c of chunks) { usdz.set(c, pos); pos += c.length; }
    }
    chunks.length = 0;
  } else {
    ({ usdz, stats } = await convertFolderToUSDZ(native, assetMap, convertOpts));
    usdzBytes = usdz.length;
  }
  timings.convertMs = performance.now() - t;
  status(`converted: ${usdzBytes} bytes (${timings.convertMs.toFixed(0)} ms)`);

  // wasm heap high-water (post-convert committed size) + JS heap (Chrome).
  const wasmHeapBytes = (() => {
    try { return native.HEAPU8 ? Number(native.HEAPU8.length) : 0; } catch (_) { return 0; }
  })();
  const jsHeapBytes = (performance.memory && performance.memory.usedJSHeapSize) || 0;
  status(`wasm heap: ${(wasmHeapBytes / 1e6).toFixed(0)} MB, js heap: ${(jsHeapBytes / 1e6).toFixed(0)} MB`);

  // 4. Validate: re-load the produced USDZ with a fresh loader instance
  //    (skipped when the output is too large to copy into this wasm heap).
  t = performance.now();
  let validate = { ok: false };
  if (!usdz) {
    validate = { ok: true, skipped: true };
    status('validate: skipped (output exceeds in-heap reload size)');
  } else {
    try {
      const entries = parseUSDZEntries(usdz);
      if (!entries.length || !/\.usd[ac]?$/i.test(entries[0].name)) {
        throw new Error('USDZ first entry is not a USD root layer');
      }
      const document = new native.LayerDocument();
      try {
        const loaded = document.load(entries[0].data);
        validate.ok = !!loaded.success;
        if (!validate.ok) validate.error = loaded.error || document.error();
      } finally { document.delete(); }
    } catch (err) {
      validate.error = String(err && err.message ? err.message : err);
    }
    status(`validate(reload usdz): ${validate.ok ? 'OK' : 'FAIL ' + (validate.error || '')}`);
  }
  timings.validateMs = performance.now() - t;

  window.__usdzBench = {
    ready: true,
    gpu,
    codec,
    pipeline,
    wasm64,
    textureConcurrency: tp ? tp.concurrency : convertOpts.textureConcurrency,
    sceneFiles: manifest.length,
    sceneBytes,
    usdzBytes,
    wasmHeapBytes,
    jsHeapBytes,
    timings,
    textureStats: tp ? tp.stats() : null,
    stats,
    validate,
  };
  // Keep the usdz around for optional chunked retrieval by the driver.
  window.__usdzBenchData = usdz;
  status('DONE');
}

main().catch((err) => {
  status(`ERROR: ${err && err.message ? err.message : err}`);
  window.__usdzBench = { error: String(err && err.message ? err.message : err) };
});
