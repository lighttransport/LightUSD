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
// How resolution works (reusing the existing pieces):
//   - HttpAssetResolver rewrites a USD asset path (relative, or already
//     absolute http(s)/data/blob) to an absolute URL against a configurable
//     base, fetches it, and caches it under the path AS WRITTEN in the USD.
//   - LightUSDComposer.progressiveComposition() runs the LIVRPS loop:
//     extract*AssetPaths() -> resolver.resolveAsync() (fetch) -> layer.setAsset()
//     -> compose*(). We inject HttpAssetResolver via composer.setAssetResolver().
//   - Textures are resolved in a second pass: layerToRenderScene() leaves remote
//     textures unresolved (image.bufferId == -1); we read each unresolved image's
//     authored path via getImagePtr().uri, fetch it over HTTP, setAsset(), and
//     re-convert. tydra resolves MaterialX and UsdPreviewSurface into the SAME
//     material record, so "simple MaterialX shading" needs no JS network eval.
//
// Memory: the legacy path keeps textures encoded in WASM until JS decodes them
// and resets the scene after upload. The next path resolves dependency layers
// with NextFlattenSession and packages the composed layer with fetched assets
// before the RenderStream loader builds the scene.
//
// Usage (module): include http-asset-resolver.html, which calls
// mountHttpAssetResolverDemo().

import { StreamingUSDRenderer } from './streaming.js';
import { LightUSDComposer } from './src/lightusd/LightUSDComposer.js';
import { parseUSDZEntries, ZipStreamWriter } from './src/usdzconvert.js';

// ---------------------------------------------------------------------------
// HttpAssetResolver — fetch + base-URL rewrite, keyed by the authored path.
// ---------------------------------------------------------------------------

// Implements the resolver interface LightUSDComposer expects:
//   resolveAsync(assetPath, { parentAssetPath? }) -> Promise<[assetPath, ArrayBuffer, resolvedUrl]>
//   getAsset / hasAsset / setAsset / clearCache
// plus rewrite(): relative paths resolve against `baseUrl`, absolute ones
// (http(s)/data/blob) pass through unchanged. resolveAsync RETURNS THE ORIGINAL
// authored path as the key, so layer.setAsset(path, bytes) stores it under the
// name the native composer/converter looks it up by (a direct cache hit).
export class HttpAssetResolver {
  constructor({ baseUrl = '' } = {}) {
    this.baseUrl = baseUrl;
    this.assetCache = new Map();   // authored path -> ArrayBuffer
    this.urlCache = new Map();     // authored path -> resolved absolute URL
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
    const candidates = [url, ...this.fallbackUrls(assetPath, url)];
    for (let i = 0; i < candidates.length; i++) {
      resolvedUrl = candidates[i];
      try {
        const resp = await fetch(resolvedUrl, { cache: 'no-store', headers: { Accept: '*/*' } });
        if (!resp.ok && resp.status !== 206) {
          throw new Error(`HTTP ${resp.status} ${resp.statusText}`);
        }
        bytes = await resp.arrayBuffer();
        break;
      } catch (e) {
        if (i + 1 === candidates.length) {
          this.fetchLog.push({ path: assetPath, url: resolvedUrl, bytes: 0, ok: false, error: e.message });
          throw new Error(`Failed to fetch '${assetPath}' (${resolvedUrl}): ${e.message}`);
        }
      }
    }
    for (const key of this.aliases(assetPath)) {
      this.assetCache.set(key, bytes);
      this.urlCache.set(key, resolvedUrl);
    }
    this.bytesFetched += bytes.byteLength;
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
  }
  clearCache() { this.assetCache.clear(); this.urlCache.clear(); this.fetchLog = []; this.bytesFetched = 0; }
}

// ---------------------------------------------------------------------------
// Texture resolution over HTTP (two-pass).
// ---------------------------------------------------------------------------

// Convert the composed layer to a render scene, then fetch — in JS, off the
// WASM heap — the encoded bytes for every texture the WASM side left unresolved
// (image.bufferId < 0). We deliberately do NOT round-trip texture bytes back
// through the native asset resolver: SanitizeAssetPath() collapses URL-ish and
// `..` paths (e.g. `https://` -> `https:/`), so a setAsset(path)/re-convert
// would never match. Instead we hand the encoded bytes to the renderer keyed by
// image id; it decodes them with createImageBitmap (off-heap) and binds them to
// the materials that already reference that image. Returns
// { textureBytesById: Map<imageId, Uint8Array>, fetched, missing }.
export async function resolveTexturesOverHttp(usd, resolver, { onStatus } = {}) {
  if (!usd.layerToRenderScene()) {
    throw new Error('layerToRenderScene failed: ' + (usd.error ? usd.error() : 'unknown'));
  }
  const textureBytesById = new Map();
  let fetched = 0;
  let missing = 0;

  let n = 0;
  try { n = usd.numImages(); } catch (_) { n = 0; }
  for (let i = 0; i < n; i++) {
    let m;
    try { m = usd.getImagePtr(i); } catch (_) { continue; }
    // Already resolved into the WASM heap (e.g. a .usdz-embedded texture) — the
    // renderer reads it straight from the heap; nothing to fetch.
    if (!m || (typeof m.byteLength === 'number' && m.byteLength > 0)) continue;
    const uri = m.uri;
    if (!uri) { missing++; continue; }
    try {
      const [, bytes] = await resolver.resolveAsync(uri); // rewritten onto the host
      textureBytesById.set(i, new Uint8Array(bytes));
      fetched++;
      onStatus && onStatus(`Fetched texture: ${uri}`);
    } catch (e) {
      missing++;
      onStatus && onStatus(`Texture missing (skipped): ${uri}`);
    }
  }
  return { textureBytesById, fetched, missing };
}

// ---------------------------------------------------------------------------
// Compose a (possibly remote) USD over HTTP and hand it to the renderer.
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
function isUsdc(name) { return /\.usdc$/i.test(name || ''); }
function isUsdName(name) { return /\.(usd|usda|usdc)$/i.test(name || ''); }
function isTextureName(name) { return /\.(png|jpe?g|webp|gif|bmp|tiff?|exr|hdr)$/i.test(String(name || '').split(/[?#]/)[0]); }
function normalizeBackend(value, fallback = 'legacy') {
  return (value === 'next' || value === 'auto' || value === 'legacy') ? value : fallback;
}

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
  for (const [candidate, value] of assetMap) {
    const c = normalizeAssetKey(candidate);
    if (c === norm || c.endsWith('/' + norm) || norm.endsWith('/' + c)) {
      return value;
    }
  }
  return null;
}

function allocateNextInput(native, usd, bytes) {
  if (typeof usd.allocateZeroCopyBuffer !== 'function') {
    throw new Error('next HTTP composition requires allocateZeroCopyBuffer in the WASM module');
  }
  const info = usd.allocateZeroCopyBuffer('__next_http_root__', bytes.length, 0);
  if (!info || !info.success) {
    throw new Error(`next input allocation failed: ${info?.error || 'unknown'}`);
  }
  const ptr = Number(info.bufferPtr);
  const chunk = 16 * 1024 * 1024;
  for (let off = 0; off < bytes.length; off += chunk) {
    const end = Math.min(off + chunk, bytes.length);
    native.HEAPU8.set(bytes.subarray(off, end), ptr + off);
  }
  return info.uuid;
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

function prepareNextRoot(rootBytes, filename) {
  const seedAssets = new Map();
  if (isUsdz(filename)) {
    const entries = parseUSDZEntries(rootBytes);
    // Root layer: first .usdc entry, else first .usda entry (the next loader
    // content-sniffs, so either format works as a session root).
    const root = entries.find((entry) => isUsdc(entry.name)) ||
      entries.find((entry) => isUsdName(entry.name));
    if (!root) {
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
    rootName: normalizeAssetKey(filename.split('/').pop() || 'root.usd'),
    seedAssets
  };
}

// Session handle abstraction over the two WASM flavors:
// - next-only module: instance-based `NextFlattenSession` class
// - legacy module: `LightUSDLoaderNative.nextFlattenAsync*` session-id protocol
function openNextFlattenSession(native, usd, prepared) {
  if (usd && typeof usd.nextFlattenAsyncBegin === 'function') {
    const uuid = allocateNextInput(native, usd, prepared.rootBytes);
    const begin = usd.nextFlattenAsyncBegin(uuid, prepared.rootName, true);
    if (!begin || !begin.success) {
      throw new Error(`next flatten begin failed: ${begin?.error || 'unknown'}`);
    }
    const session = begin.session;
    return {
      step: () => usd.nextFlattenAsyncStep(session, null),
      provideLayer: (key, bytes) => usd.nextFlattenAsyncProvideLayer(session, key, bytes),
      close: () => usd.nextFlattenAsyncEnd(session)
    };
  }
  if (typeof native.NextFlattenSession === 'function') {
    const session = new native.NextFlattenSession();
    const begin = session.begin(prepared.rootBytes, prepared.rootName, true);
    if (!begin || !begin.success) {
      session.delete();
      throw new Error(`next flatten begin failed: ${begin?.error || 'unknown'}`);
    }
    return {
      step: () => session.step(null),
      provideLayer: (key, bytes) => session.provideLayer(key, bytes),
      close: () => {
        session.end();
        if (typeof session.delete === 'function') session.delete();
      }
    };
  }
  throw new Error('next flatten bindings are unavailable in this WASM module');
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

export async function flattenNextOverHttp({ renderer, rootBytes, filename, resolver, onStatus }) {
  const native = renderer.native;
  if (!native) {
    throw new Error('next HTTP composition requires a LightUSD WASM module');
  }
  const usd = typeof native.LightUSDLoaderNative === 'function'
    ? new native.LightUSDLoaderNative() : null;
  try {
    const prepared = prepareNextRoot(rootBytes, filename);
    const handle = openNextFlattenSession(native, usd, prepared);
    let result = null;
    try {
      for (;;) {
        const step = handle.step();
        if (!step || !step.success) {
          throw new Error(`next flatten failed: ${step?.error || 'unknown'}`);
        }
        if (step.status === 'need-layer') {
          const key = normalizeAssetKey(step.key);
          let bytes = findAssetByKey(prepared.seedAssets, key);
          if (!bytes) {
            const [, fetched, url] = await resolveNextAsset(resolver, key);
            bytes = new Uint8Array(fetched);
            resolver.setAsset(key, bytes, url || '');
            onStatus && onStatus(`Fetched next dependency layer: ${key}`);
          } else {
            onStatus && onStatus(`Using in-archive next dependency layer: ${key}`);
          }
          // Dependency layers may be USDC, USDA, or USDZ — the native loader
          // content-sniffs the bytes.
          const provided = handle.provideLayer(key, bytes);
          if (!provided || !provided.success) {
            throw new Error(`next flatten provide failed for ${key}: ${provided?.error || 'unknown'}`);
          }
          continue;
        }
        if (step.status === 'done' || step.status === 'ready') {
          result = step;
          break;
        }
        throw new Error(`next flatten returned unexpected status: ${step.status}`);
      }
    } finally {
      handle.close();
    }
    if (!result || !result.data) {
      throw new Error('next flatten produced no root layer');
    }

    const rootOut = new Uint8Array(result.data);
    const passthrough = [];
    const added = new Set();
    const addEntry = (name, data) => {
      const key = normalizeAssetKey(name);
      if (!key || added.has(key) || isUsdName(key)) return;
      added.add(key);
      passthrough.push({ name: key, data: data instanceof Uint8Array ? data : new Uint8Array(data) });
    };

    for (const [name, data] of prepared.seedAssets) {
      if (!isUsdName(name)) addEntry(name, data);
    }

    const referencedAssets = Array.isArray(result.assetPaths) ? result.assetPaths : [];
    let fetchedAssets = 0;
    for (const assetPath of referencedAssets) {
      if (!assetPath || isUsdName(assetPath) || !isTextureName(assetPath)) continue;
      const key = normalizeAssetKey(assetPath);
      if (added.has(key)) continue;
      let bytes = findAssetByKey(prepared.seedAssets, key);
      if (!bytes) {
        try {
          const [, fetched, url] = await resolveNextAsset(resolver, assetPath);
          bytes = new Uint8Array(fetched);
          resolver.setAsset(assetPath, bytes, url || '');
          fetchedAssets++;
          onStatus && onStatus(`Fetched next texture asset: ${assetPath}`);
        } catch (e) {
          onStatus && onStatus(`Next texture missing (skipped): ${assetPath}`);
          continue;
        }
      }
      addEntry(assetPath, bytes);
    }

    const usdz = buildUsdzFromEntries('root.usdc', rootOut, passthrough);
    return {
      usdz,
      stats: result,
      fetchedAssets,
      packagedAssets: passthrough.length,
      referencedAssets: referencedAssets.length
    };
  } finally {
    if (usd && typeof usd.delete === 'function') usd.delete();
  }
}

// Build a composed native Layer instance from root bytes, fetching every
// external arc + texture over HTTP via `resolver`. Returns
// { usd, textureBytesById } — the native instance (already through
// layerToRenderScene; caller renders then owns/frees it) plus the JS-held
// encoded bytes for HTTP textures keyed by image id.
export async function composeOverHttp({ renderer, rootBytes, filename, resolver, onStatus, preloadedAssets = [] }) {
  const native = renderer.native;
  const u8 = rootBytes instanceof Uint8Array ? rootBytes : new Uint8Array(rootBytes);

  const layer = new native.LightUSDLoaderNative();
  // Keep textures ENCODED in the heap (JS decodes them off-heap) — low memory.
  if (typeof layer.setLoadTextureInNative === 'function') layer.setLoadTextureInNative(false);
  // Allow USD `../` parent-dir references during composition (resolved by the
  // sandboxed in-memory resolver). On by default in recent WASM builds; opt in
  // explicitly so older builds that default off still get it where available.
  if (typeof layer.setAllowParentRelativeAssetPaths === 'function') {
    layer.setAllowParentRelativeAssetPaths(true);
  }

  // Seed the in-archive assets for a .usdz root, then load the root crate.
  let rootForLoad = u8;
  if (isUsdz(filename)) {
    let entries;
    try { entries = parseUSDZEntries(u8); } catch (e) { entries = null; }
    if (entries && entries.length) {
      const root = entries.find((e) => /\.usd[ac]?$/i.test(e.name)) || entries[0];
      for (const e of entries) layer.setAsset(e.name, e.data); // in-archive textures/layers
      rootForLoad = root.data;
      filename = root.name;
      onStatus && onStatus(`USDZ: seeded ${entries.length} in-archive assets`);
    }
  }

  if (!layer.loadAsLayerFromBinary(rootForLoad, filename)) {
    const err = layer.error ? layer.error() : 'unknown';
    if (typeof layer.delete === 'function') layer.delete();
    throw new Error(`loadAsLayerFromBinary failed for ${filename}: ${err}`);
  }

  for (const asset of preloadedAssets) {
    if (!asset || !asset.key || !asset.bytes) continue;
    resolver.setAsset(asset.key, asset.bytes, asset.url || '');
    const aliases = typeof resolver.aliases === 'function' ? resolver.aliases(asset.key) : [asset.key];
    for (const key of aliases) layer.setAsset(key, asset.bytes);
  }

  // Composition arcs (references/payloads/sublayers) over HTTP, reusing the
  // proven LIVRPS loop with our HTTP resolver injected.
  const composer = new LightUSDComposer();
  composer.setLayer(layer);
  composer.setUSDLoader(renderer.loader);
  composer.setAssetResolver(resolver);
  composer.setBaseWorkingPath('./');
  composer.setAssetSearchPaths(['./']);
  try {
    await composer.progressiveComposition();
  } catch (e) {
    onStatus && onStatus(`Composition note: ${e.message}`);
  }

  // Textures over HTTP: convert once, then fetch each unresolved image's bytes
  // in JS (off the WASM heap) keyed by image id for the renderer to decode.
  const tex = await resolveTexturesOverHttp(layer, resolver, { onStatus });
  onStatus && onStatus(
    `Resolved ${tex.fetched} texture(s) over HTTP` +
    (tex.missing ? `, ${tex.missing} unresolved` : ''));

  return { usd: layer, textureBytesById: tex.textureBytesById };
}

export async function renderHttpUSD({
  renderer,
  rootBytes,
  filename,
  baseUrl,
  label,
  backend = 'legacy',
  onStatus,
  preloadedAssets = []
}) {
  const requestedBackend = normalizeBackend(backend);
  const loadStart = performance.now();
  const jsStart = jsHeapBytes();
  const inputRootBytes = rootBytes.byteLength || 0;
  const shadeLabel = detectMaterialX(rootBytes) ? 'MaterialX/tydra' : 'UsdPreviewSurface';

  if (requestedBackend === 'next' || requestedBackend === 'auto') {
    try {
      const resolver = new HttpAssetResolver({ baseUrl });
      onStatus && onStatus(`Loading ${label} with next HTTP composition...`);
      const flattened = await flattenNextOverHttp({
        renderer,
        rootBytes,
        filename,
        resolver,
        onStatus
      });
      const result = await renderer.loadBytesIncremental(flattened.usdz, 'next-http-composed.usdz');
      const isNextResult = !!result?.memory?.summary?.incremental;
      if (isNextResult) {
        return {
          backend: 'next',
          requestedBackend,
          result,
          inputBytes: inputRootBytes + resolver.bytesFetched,
          fetchedBytes: resolver.bytesFetched,
          fetches: resolver.fetchLog.length,
          fetchLog: resolver.fetchLog,
          shadeLabel: 'next render scene',
          loadMs: performance.now() - loadStart,
          jsHeapDelta: jsHeapBytes() - jsStart,
          note: `next HTTP composition: ${flattened.packagedAssets} packaged asset(s), ` +
            `${flattened.referencedAssets} referenced asset path(s)`
        };
      }
      if (requestedBackend === 'next') {
        throw new Error('next backend did not produce an incremental render scene result');
      }
      onStatus && onStatus('next backend not applicable; falling back to legacy HTTP composition...');
    } catch (e) {
      if (requestedBackend === 'next') throw e;
      onStatus && onStatus(`next backend failed; falling back to legacy HTTP composition: ${e.message}`);
    }
  }

  const resolver = new HttpAssetResolver({ baseUrl });
  const { usd, textureBytesById } = await composeOverHttp({
    renderer,
    rootBytes,
    filename,
    resolver,
    onStatus,
    preloadedAssets,
  });
  const inputBytes = inputRootBytes + resolver.bytesFetched;
  const result = await renderer.renderComposedNative(usd, label, { inputBytes, textureBytesById });
  return {
    backend: 'legacy',
    requestedBackend,
    result,
    inputBytes,
    fetchedBytes: resolver.bytesFetched,
    fetches: resolver.fetchLog.length,
    fetchLog: resolver.fetchLog,
    shadeLabel,
    loadMs: performance.now() - loadStart,
    jsHeapDelta: jsHeapBytes() - jsStart,
  };
}

// ---------------------------------------------------------------------------
// Demo bootstrap.
// ---------------------------------------------------------------------------

const LOCAL_HTTP_TEXTURE_USDA = './assets/http-cat-plane.usda';

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
    const backend = normalizeBackend(q.get('backend'), 'legacy');
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

  return { renderer, loadLocalHttpTexture, loadOverHttp, loadLocalFile };
}

export default mountHttpAssetResolverDemo;
