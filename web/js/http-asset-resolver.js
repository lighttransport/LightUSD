// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-present Light Transport Entertainment, Inc.
//
// http-asset-resolver.js — HTTP(fetch)-based USD asset resolution demo.
//
// Shows two flows for resolving a USD scene's external assets (sublayers,
// references, payloads, textures) over the network with fetch():
//
//   Demo 1 — Local USDA, HTTP asset path:
//     A USDA loaded from the same origin whose texture is authored as an
//     absolute https:// URL. The unresolved texture is fetched over HTTP and
//     fed back into the converter, then rendered.
//
//   Demo 2 — Root USD over HTTP, rewrite every reference to HTTP:
//     A root .usd/.usda/.usdc/.usdz loaded from a remote host (e.g. the
//     usd-wg/assets test_assets on raw.githubusercontent.com). ALL relative
//     asset paths it references are rewritten onto that host's directory and
//     pulled over HTTP — composition arcs first (references/payloads/sublayers),
//     then textures — so the whole scene resolves from the remote location.
//
// Core composition requests missing layers through NextFlattenSession. Asset
// paths retain their authoring-layer anchors, then fetched bytes are packaged
// under safe archive names for the incremental RenderStream renderer.

import { StreamingUSDRenderer } from './streaming.js';
import { parseUSDZEntries, ZipStreamWriter } from './src/usdzconvert.js';

// ---------------------------------------------------------------------------
// HttpAssetResolver — fetch + base-URL rewrite, keyed by the authored path.
// ---------------------------------------------------------------------------

export class HttpAssetResolver {
  constructor({ baseUrl = '' } = {}) {
    this.baseUrl = baseUrl;
    this.assetCache = new Map();   // authored path -> ArrayBuffer
    this.urlCache = new Map();     // authored path -> resolved absolute URL
    this.urlBytes = new Map();     // resolved absolute URL -> ArrayBuffer
    this.fetchLog = [];            // [{ path, url, bytes, ok, error? }]
    this.bytesFetched = 0;
  }

  setBaseUrl(url) { this.baseUrl = url || ''; }

  static isAbsolute(p) { return /^(https?:|data:|blob:)/i.test(String(p || '')); }

  assetFilePart(assetPath) {
    return String(assetPath || '').replace(/<[^>]*>\s*$/, '');
  }

  aliases(assetPath) {
    const raw = String(assetPath || '');
    const p = this.assetFilePart(raw);
    const out = new Set([raw, p]);
    if (p.startsWith('./')) out.add(p.slice(2));
    else if (p && !p.startsWith('/') && !HttpAssetResolver.isAbsolute(p)) out.add('./' + p);
    return [...out];
  }

  // Resolve an authored asset path to an absolute URL to fetch.
  rewrite(assetPath, opts = {}) {
    const p = this.assetFilePart(assetPath);
    if (HttpAssetResolver.isAbsolute(p)) return p;
    const parent = opts.parentAssetPath || '';
    let base = this.baseUrl || (typeof location !== 'undefined' ? location.href : '');
    if (parent) {
      const parentUrl = HttpAssetResolver.isAbsolute(parent) ? parent : new URL(parent, base).href;
      base = parentUrl.slice(0, parentUrl.lastIndexOf('/') + 1);
    }
    if (!base) return p;
    // `new URL` collapses ./ and ../ relative to the base directory.
    return new URL(p, base).href;
  }

  fallbackUrls(assetPath, primaryUrl) {
    const p = this.assetFilePart(assetPath);
    if (HttpAssetResolver.isAbsolute(p) || p.startsWith('/') || p.includes('/')) return [];
    const base = this.baseUrl || (typeof location !== 'undefined' ? location.href : '');
    if (!base) return [];
    const dirs = ['geo/', 'assets/', 'materials/', 'layers/', 'payloads/'];
    return dirs.map((d) => new URL(d + p, base).href).filter((u) => u !== primaryUrl);
  }

  async resolveAsync(assetPath, opts = {}) {
    for (const key of this.aliases(assetPath)) {
      if (this.assetCache.has(key)) {
        return [assetPath, this.assetCache.get(key), this.urlCache.get(key)];
      }
    }
    const url = this.rewrite(assetPath, opts);
    let resolvedUrl = url;
    let bytes;
    let transferred = false;
    const candidates = [url, ...this.fallbackUrls(assetPath, url)];
    for (let i = 0; i < candidates.length; i++) {
      resolvedUrl = candidates[i];
      // Canonical-URL cache: two authored aliases that rewrite to the same
      // URL share one transfer.
      if (this.urlBytes.has(resolvedUrl)) {
        bytes = this.urlBytes.get(resolvedUrl);
        break;
      }
      try {
        const resp = await fetch(resolvedUrl, { cache: 'no-store', headers: { Accept: '*/*' } });
        if (!resp.ok && resp.status !== 206) {
          throw new Error(`HTTP ${resp.status} ${resp.statusText}`);
        }
        bytes = await resp.arrayBuffer();
        transferred = true;
        break;
      } catch (e) {
        if (i + 1 === candidates.length) {
          this.fetchLog.push({ path: assetPath, url: resolvedUrl, bytes: 0, ok: false, error: e.message });
          throw new Error(`Failed to fetch '${assetPath}' (${resolvedUrl}): ${e.message}`);
        }
      }
    }
    this.urlBytes.set(resolvedUrl, bytes);
    for (const key of this.aliases(assetPath)) {
      this.assetCache.set(key, bytes);
      this.urlCache.set(key, resolvedUrl);
    }
    if (transferred) this.bytesFetched += bytes.byteLength;
    this.fetchLog.push({ path: assetPath, url: resolvedUrl, bytes: bytes.byteLength, ok: true });
    return [assetPath, bytes, resolvedUrl];
  }

  getAsset(uri) { return this.assetCache.has(uri) ? this.assetCache.get(uri) : null; }
  hasAsset(uri) { return this.assetCache.has(uri); }
  setAsset(uri, data, resolvedUrl = '') {
    for (const key of this.aliases(uri)) {
      this.assetCache.set(key, data);
      if (resolvedUrl) this.urlCache.set(key, resolvedUrl);
    }
    if (resolvedUrl) this.urlBytes.set(resolvedUrl, data);
  }
  clearCache() {
    this.assetCache.clear();
    this.urlCache.clear();
    this.urlBytes.clear();
    this.fetchLog = [];
    this.bytesFetched = 0;
  }
}

// ---------------------------------------------------------------------------
// Heuristic: does this root reference a MaterialX material? Used only to label
// the shading mode — tydra unifies MaterialX and UsdPreviewSurface into one
// material record, so both render through the same path.
export function detectMaterialX(bytes) {
  // Sniff the first chunk as text (works for .usda; harmless for binary crates).
  const head = new TextDecoder('utf-8', { fatal: false }).decode(
    bytes.subarray(0, Math.min(bytes.length, 1 << 16)));
  return /\.mtlx\b/i.test(head) ||
    /info:implementationSource\s*=\s*"sourceAsset"/.test(head) ||
    /\bND_[a-zA-Z0-9_]+/.test(head) ||
    /MaterialX/i.test(head) ||
    /info:mtlx:/i.test(head);
}

function isUsdz(name) { return /\.usdz$/i.test(name || ''); }
function isUsdName(name) { return /\.(usd|usda|usdc)$/i.test(name || ''); }
function normalizeBackend() { return 'core'; }

function preferredUrlParam(params) {
  return params.get('uri') || params.get('url') || params.get('src') || params.get('model') || '';
}

function jsHeapBytes() {
  return (typeof performance !== 'undefined' && performance.memory)
    ? performance.memory.usedJSHeapSize
    : 0;
}

function normalizeAssetKey(path) {
  return String(path || '').replace(/\\/g, '/').replace(/^\.\/+/, '').replace(/^\/+/, '');
}

function findAssetByKey(assetMap, key) {
  const norm = normalizeAssetKey(key);
  if (assetMap.has(norm)) return assetMap.get(norm);
  if (assetMap.has(key)) return assetMap.get(key);
  return null;
}

function buildUsdzFromEntries(rootName, rootBytes, entries) {
  const chunks = [];
  const writer = new ZipStreamWriter((bytes) => {
    chunks.push(bytes.slice ? bytes.slice() : new Uint8Array(bytes));
  });
  writer.addEntry(rootName, rootBytes);
  for (const entry of entries) {
    if (!entry || !entry.name || !entry.data || isUsdName(entry.name)) continue;
    writer.addEntry(entry.name, entry.data);
  }
  writer.finalize();
  const total = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  return out;
}

function prepareNextRoot(rootBytes, filename, resolver) {
  const seedAssets = new Map();
  if (isUsdz(filename)) {
    const entries = parseUSDZEntries(rootBytes);
    const root = entries[0];
    if (!root || !isUsdName(root.name)) {
      throw new Error('next HTTP composition requires a USD root layer in USDZ input');
    }
    for (const entry of entries) {
      if (!entry.name.endsWith('/')) {
        seedAssets.set(normalizeAssetKey(entry.name), entry.data);
      }
    }
    return {
      rootBytes: root.data,
      rootName: normalizeAssetKey(root.name),
      seedAssets
    };
  }
  // Plain roots: any USD format (.usdc/.usda/.usd) — the native session
  // content-sniffs the bytes.
  const bytes = rootBytes instanceof Uint8Array ? rootBytes : new Uint8Array(rootBytes);
  return {
    rootBytes: bytes,
    rootName: resolver.baseUrl ? new URL(filename, resolver.baseUrl).href : filename || 'root.usd',
    seedAssets
  };
}

function openNextFlattenSession(native, prepared) {
  const session = new native.NextFlattenSession();
  try {
    const begin = session.begin(prepared.rootBytes, prepared.rootName, true);
    if (!begin?.success) throw new Error(`Core flatten begin failed: ${begin?.error || 'unknown'}`);
    session.setRebaseAssetPaths(true);
    return session;
  } catch (error) { session.delete(); throw error; }
}

async function resolveNextAsset(resolver, assetPath) {
  const path = normalizeAssetKey(assetPath);
  // A root served from an assets directory may also author that prefix.
  const candidates = path.startsWith('assets/') ? [path, path.slice(7)] : [path];
  let lastError = null;
  for (const candidate of candidates) {
    try {
      const result = await resolver.resolveAsync(candidate);
      const bytes = result[1] instanceof Uint8Array ? result[1] : new Uint8Array(result[1]);
      const prefix = new TextDecoder().decode(bytes.subarray(0, 32));
      if (/^\s*(<!doctype html|<html)/i.test(prefix)) {
        throw new Error(`Asset URL returned an HTML page: ${candidate}`);
      }
      return [result[0], bytes, result[2]];
    } catch (error) {
      lastError = error;
    }
  }
  throw lastError || new Error(`Could not resolve ${assetPath}`);
}

export async function flattenNextOverHttp({ renderer, rootBytes, filename, resolver, onStatus, preloadedAssets = [], packageAssets = true, variantOverrides = {} }) {
  const native = renderer.native;
  if (!native?.NextFlattenSession) throw new Error('Core HTTP composition requires NextFlattenSession');
  const prepared = prepareNextRoot(rootBytes, filename, resolver);
  for (const asset of preloadedAssets) {
    if (!asset?.key || !asset.bytes) continue;
    const url = asset.url || resolver.rewrite(asset.key);
    resolver.setAsset(asset.key, asset.bytes, url);
    resolver.setAsset(url, asset.bytes, url);
    prepared.seedAssets.set(asset.key, asset.bytes);
    prepared.seedAssets.set(url, asset.bytes);
  }
  const session = openNextFlattenSession(native, prepared);
  try {
    for(const [key,selection] of Object.entries(variantOverrides))session.setVariantOverride(key,selection);
    let result;
    for (;;) {
      const step = session.step(null);
      if (!step?.success) throw new Error(`Core flatten failed: ${step?.error || 'unknown'}`);
      if (step.status === 'need-layer') {
        const key = step.key;
        const bytes = findAssetByKey(prepared.seedAssets, key) || (await resolveNextAsset(resolver, key))[1];
        const provided = session.provideLayer(key, bytes);
        if (!provided?.success) throw new Error(`Core dependency failed: ${key}: ${provided?.error || 'unknown'}`);
        onStatus?.(`Resolved dependency layer: ${key}`);
      } else if (step.status === 'done' || step.status === 'ready') { result = step; break; }
      else throw new Error(`Unexpected core flatten status: ${step.status}`);
    }
    const referencedAssets = Array.from(result.assetPaths || []);
    if(!packageAssets)return {rootBytes:new Uint8Array(result.data),stats:result,
      fetchedAssets:0,packagedAssets:0,referencedAssets:referencedAssets.length};
    const entries = [];
    const remap = {};
    const packaged = new Map();
    let fetchedAssets = 0;
    for (const assetPath of referencedAssets) {
      if (!assetPath || isUsdName(assetPath)) continue;
      let bytes = findAssetByKey(prepared.seedAssets, assetPath);
      let identity = assetPath;
      let fetchedPayload = false;
      if (!bytes) {
        try {
          const [, fetched, url] = await resolveNextAsset(resolver, assetPath);
          bytes = fetched;
          identity = url || assetPath;
          fetchedPayload = true;
        } catch (error) {
          onStatus?.(`Asset missing (skipped): ${assetPath}`);
          continue;
        }
      }
      let name = packaged.get(identity);
      if (!name) {
        if(fetchedPayload)fetchedAssets++;
        const extension = /\.([a-zA-Z0-9]{1,8})(?:[?#].*)?$/.exec(assetPath)?.[1] || 'bin';
        name = `assets/${entries.length}.${extension}`;
        entries.push({name, data: bytes});
        packaged.set(identity, name);
      }
      remap[assetPath] = name;
    }
    session.setAssetPathRemap(remap);
    const final = session.step(null);
    if (!final?.success || !final.data) throw new Error(`Core asset remap failed: ${final?.error || final?.status || 'unknown'}`);
    return {
      usdz: buildUsdzFromEntries('root.usdc', new Uint8Array(final.data), entries),
      stats: final, fetchedAssets, packagedAssets: entries.length,
      referencedAssets: referencedAssets.length
    };
  } finally { session.delete(); }
}

// Compose to a portable, core-renderable package without retaining a native handle.
export const composeOverHttp = flattenNextOverHttp;

export async function renderHttpUSD({ renderer, rootBytes, filename, baseUrl, label, onStatus, preloadedAssets = [] }) {
  const loadStart = performance.now();
  const jsStart = jsHeapBytes();
  const resolver = new HttpAssetResolver({baseUrl});
  onStatus?.(`Loading ${label} with core HTTP composition...`);
  const flattened = await flattenNextOverHttp({renderer, rootBytes, filename, resolver, onStatus, preloadedAssets});
  const result = await renderer.loadBytesIncremental(flattened.usdz, 'http-composed.usdz');
  return {
    backend: 'core', requestedBackend: 'core', result,
    inputBytes: rootBytes.byteLength + resolver.bytesFetched,
    fetchedBytes: resolver.bytesFetched, fetches: resolver.fetchLog.length,
    fetchLog: resolver.fetchLog, shadeLabel: 'core render scene',
    loadMs: performance.now() - loadStart, jsHeapDelta: jsHeapBytes() - jsStart,
    note: `Core HTTP composition: ${flattened.packagedAssets} packaged asset(s), ${flattened.referencedAssets} referenced asset path(s)`
  };
}

// ---------------------------------------------------------------------------
// Demo bootstrap.
// ---------------------------------------------------------------------------

const LOCAL_HTTP_TEXTURE_USDA = new URL('./assets/http-cat-plane.usda', import.meta.url).href;

// A few usd-wg/assets test_assets (raw GitHub serves permissive CORS). These are
// editable in the UI; pick the one that exists / renders best for your build.
const TEST_ASSET_BASE = 'https://raw.githubusercontent.com/usd-wg/assets/1b91f3c464891af259d51d9ee9ee9e6c357f7079/test_assets/';
const DEMO2_PRESETS = [
  // UsdPreviewSurface; normal/bump textures referenced (./r_*.png) in the same
  // remote directory -> rewritten onto the host and pulled over HTTP.
  { label: 'NormalsTextureBiasAndScale', url: TEST_ASSET_BASE + 'NormalsTextureBiasAndScale/NormalsTextureBiasAndScale.usda' },
  // MaterialX (flattened): inline geometry + MaterialX materials; textures
  // (textures/brass_*.jpg) pulled over HTTP. Shows "simple MaterialX shading"
  // (tydra-unified).
  { label: 'MaterialXTest (flattened)', url: TEST_ASSET_BASE + 'MaterialXTest/basicTextured_flatten.usda' },
  // Textured quads with texture-transform; textures live under ./0 (subdir).
  { label: 'TextureTransformTest', url: TEST_ASSET_BASE + 'TextureTransformTest/TextureTransformTest.usd' },
];

function fmtMB(b) { return (b / 1048576).toFixed(1) + ' MB'; }

export async function mountHttpAssetResolverDemo(opts = {}) {
  window.renderComplete = false;
  window.renderError = null;
  const canvas = opts.canvas || document.getElementById('gl');
  const statusEl = opts.status || document.getElementById('status');
  const logEl = opts.log || document.getElementById('log');
  const panel = opts.panel || document.getElementById('mem-panel');
  const urlInput = opts.urlInput || document.getElementById('asset-url');
  const btnLocal = opts.btnLocal || document.getElementById('btn-demo1');
  const btnHttp = opts.btnHttp || document.getElementById('btn-demo2');
  const fileInput = opts.fileInput || document.getElementById('file-input');
  const backendSelect = opts.backendSelect || document.getElementById('backend');
  const presetSel = opts.presetSelect || document.getElementById('preset');

  const renderer = new StreamingUSDRenderer(canvas);
  await renderer.init();

  let busy = false;
  let fps = 0;
  let fpsFrames = 0;
  let fpsLast = performance.now();
  const updateFps = (now) => {
    fpsFrames++;
    if (now - fpsLast >= 500) {
      fps = Math.round((fpsFrames * 1000) / (now - fpsLast));
      fpsFrames = 0;
      fpsLast = now;
    }
    requestAnimationFrame(updateFps);
  };
  requestAnimationFrame(updateFps);
  const setStatus = (s) => { if (statusEl) statusEl.textContent = s; };
  const appendLog = (s) => {
    if (!logEl) { console.log('[http-asset-resolver]', s); return; }
    const line = document.createElement('div');
    line.textContent = s;
    logEl.appendChild(line);
    logEl.scrollTop = logEl.scrollHeight;
  };

  // Live + final memory panel (same shape StreamingUSDRenderer emits).
  const renderPanel = (m) => {
    if (!panel) return;
    const ratio = m.inputBytes ? (m.heapReserved / m.inputBytes).toFixed(2) : '–';
    const rows = [
      ['Input + fetched', fmtMB(m.inputBytes)],
      ['WASM heap (reserved peak)', fmtMB(m.heapReserved)],
      ['WASM render buffers (live)', fmtMB(m.renderBuffers)],
      ['Heap / input', ratio + '×'],
      ['FPS', String(fps || '–')],
    ];
    let html = '<table>' + rows.map(([k, v]) =>
      `<tr><td>${k}</td><td style="text-align:right">${v}</td></tr>`).join('') + '</table>';
    if (m.last && m.last.phases) {
      html += '<div class="phases"><b>Load phases (heap / live buffers)</b><table>' +
        m.last.phases.map((p) =>
          `<tr><td>${p.label}</td><td style="text-align:right">${fmtMB(p.heapReserved)}</td>` +
          `<td style="text-align:right">${fmtMB(p.renderBuffers || 0)}</td></tr>`).join('') +
        '</table></div>';
    }
    panel.innerHTML = html;
  };
  renderer.onMemory(renderPanel);
  renderer.startMemoryPolling(500);

  // Shared loader for a root that's already bytes (local file path or remote URL).
  async function run({ rootBytes, filename, baseUrl, label, backend }) {
    if (busy) return;
    busy = true;
    if (btnLocal) btnLocal.disabled = true;
    if (btnHttp) btnHttp.disabled = true;
    if (logEl) logEl.innerHTML = '';
    try {
      const t0 = performance.now();
      const selectedBackend = normalizeBackend(backend || backendSelect?.value);
      const loaded = await renderHttpUSD({
        renderer,
        rootBytes,
        filename,
        baseUrl,
        label,
        backend: selectedBackend,
        onStatus: appendLog
      });
      const r = loaded.result;
      const dt = (performance.now() - t0).toFixed(0);

      const s = r.memory.summary;
      setStatus(`${label}: ${r.meshes} meshes, ${r.textures} textures, ${r.materials} materials ` +
        `in ${dt} ms — backend ${loaded.backend} (${loaded.shadeLabel}); peak WASM heap ${s.peakHeapMB.toFixed(1)} MB ` +
        `(${s.ratio.toFixed(2)}× of ${fmtMB(loaded.inputBytes)} input).`);
      if (loaded.note) appendLog(loaded.note);
      appendLog(`HTTP fetches: ${loaded.fetches} ` +
        `(${loaded.fetchLog.filter((f) => f.ok).length} ok), ${fmtMB(loaded.fetchedBytes)} total.`);
      appendLog(`JS heap delta: ${fmtMB(Math.max(0, loaded.jsHeapDelta || 0))}`);
    } catch (e) {
      console.error(e);
      setStatus('Error: ' + (e && e.message));
      appendLog('Error: ' + (e && e.message));
      window.renderError = e?.message || String(e);
    } finally {
      busy = false;
      if (btnLocal) btnLocal.disabled = false;
      if (btnHttp) btnHttp.disabled = false;
      window.renderComplete = true;
    }
  }

  // Demo 1 — local USDA whose texture is an absolute https:// URL.
  async function loadLocalHttpTexture() {
    setStatus(`Fetching local ${LOCAL_HTTP_TEXTURE_USDA}…`);
    const resp = await fetch(LOCAL_HTTP_TEXTURE_USDA);
    if (!resp.ok) {
      const message = `Cannot load ${LOCAL_HTTP_TEXTURE_USDA}: HTTP ${resp.status}`;
      setStatus(message);
      window.renderError = message;
      window.renderComplete = true;
      return;
    }
    const bytes = new Uint8Array(await resp.arrayBuffer());
    // baseUrl '' — the texture path is already absolute http, fetched directly.
    await run({ rootBytes: bytes, filename: 'http-cat-plane.usda', baseUrl: '', label: 'Local USDA (HTTP texture)' });
  }

  // Demo 2 — root USD over HTTP; rewrite all relative refs/textures onto its dir.
  async function loadOverHttp(url) {
    const target = url || (urlInput && urlInput.value) || DEMO2_PRESETS[0].url;
    setStatus(`Fetching root ${target}…`);
    let bytes;
    try {
      const resp = await fetch(target, { cache: 'no-store' });
      if (!resp.ok) throw new Error(`HTTP ${resp.status} ${resp.statusText}`);
      bytes = new Uint8Array(await resp.arrayBuffer());
    } catch (e) {
      setStatus(`Cannot fetch ${target}: ${e.message}`);
      window.renderError = e?.message || String(e);
      window.renderComplete = true;
      return;
    }
    const filename = target.split('/').pop() || 'root.usd';
    // baseUrl = the root's directory, so relative refs/textures rewrite onto it.
    const baseUrl = target.slice(0, target.lastIndexOf('/') + 1);
    await run({ rootBytes: bytes, filename, baseUrl, label: filename });
  }

  async function loadLocalFile(file) {
    if (!file || !/\.(usd|usda|usdc|usdz)$/i.test(file.name)) {
      setStatus('Drop or select a USD file (.usd, .usda, .usdc, .usdz).');
      return;
    }
    setStatus(`Reading ${file.name}...`);
    const bytes = new Uint8Array(await file.arrayBuffer());
    await run({ rootBytes: bytes, filename: file.name, baseUrl: '', label: file.name });
  }

  // Wire UI.
  if (presetSel) {
    presetSel.innerHTML = DEMO2_PRESETS.map((p, i) =>
      `<option value="${p.url}"${i === 0 ? ' selected' : ''}>${p.label}</option>`).join('');
    presetSel.addEventListener('change', () => { if (urlInput) urlInput.value = presetSel.value; });
  }
  if (urlInput && !urlInput.value) urlInput.value = DEMO2_PRESETS[0].url;
  if (btnLocal) btnLocal.addEventListener('click', () => loadLocalHttpTexture());
  if (btnHttp) btnHttp.addEventListener('click', () => loadOverHttp());
  if (fileInput) fileInput.addEventListener('change', () => {
    const file = fileInput.files && fileInput.files[0];
    if (file) loadLocalFile(file);
    fileInput.value = '';
  });
  if (canvas) {
    canvas.addEventListener('dragover', (e) => {
      e.preventDefault();
      document.body.classList.add('drag-over');
    });
    canvas.addEventListener('dragleave', () => document.body.classList.remove('drag-over'));
    canvas.addEventListener('drop', (e) => {
      e.preventDefault();
      document.body.classList.remove('drag-over');
      const file = e.dataTransfer.files && e.dataTransfer.files[0];
      loadLocalFile(file);
    });
  }

  // Optional auto-run via ?demo=1|2&url=…
  if (opts.autoRun !== false && typeof location !== 'undefined') {
    const q = new URLSearchParams(location.search);
    const backend = normalizeBackend(q.get('backend'));
    if (backendSelect) backendSelect.value = backend;
    const url = preferredUrlParam(q);
    if (q.get('demo') === '1') loadLocalHttpTexture();
    else if (q.get('demo') === '2') loadOverHttp(url || undefined);
    else if (url) loadOverHttp(url);
    else {
      setStatus('Pick a demo: load the local USDA (HTTP texture), or a USD over HTTP.');
      window.renderComplete = true;
    }
  }

  return { renderer, loadLocalHttpTexture, loadOverHttp, loadLocalFile, parseUSDZEntries };
}

export default mountHttpAssetResolverDemo;
