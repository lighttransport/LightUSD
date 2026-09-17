import { parseUSDZEntries } from 'lightusd-js/src/usdzconvert.js';
import { LightUSDComposer } from 'lightusd-js/src/lightusd/LightUSDComposer.js';
export { AssetBudget, chooseRootFile, formatBytes, githubContentsRequest, MAX_ASSET_BYTES, normalizeAssetKey } from './online-usd-viewer-core.js';
import { AssetBudget, chooseRootFile, formatBytes, githubContentsRequest, MAX_ASSET_BYTES, normalizeAssetKey } from './online-usd-viewer-core.js';

function aliases(uri) {
  const raw = String(uri || '');
  const clean = raw.replace(/<[^>]*>\s*$/, '');
  const out = new Set([raw, clean]);
  if (clean.startsWith('./')) out.add(clean.slice(2));
  else if (clean && !clean.startsWith('/') && !/^[a-z]+:/i.test(clean)) out.add(`./${clean}`);
  return [...out];
}

function relativeKey(assetPath, parentAssetPath = '') {
  const raw = String(assetPath || '').replaceAll('\\', '/');
  if (/^(https?:|data:|blob:)/i.test(raw)) return raw;
  const parent = normalizeAssetKey(parentAssetPath);
  const joined = raw.startsWith('/') ? raw.slice(1) : `${parent ? `${parent.slice(0, parent.lastIndexOf('/') + 1)}` : ''}${raw}`;
  const parts = [];
  for (const part of joined.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') parts.pop();
    else parts.push(part);
  }
  return parts.join('/');
}

export class FileAssetResolver {
  constructor(files, budget, onProgress = () => {}) {
    this.files = new Map();
    this.budget = budget;
    this.onProgress = onProgress;
    this.cache = new Map();
    this.urlCache = new Map();
    const entries = [...files];
    const prefixes = entries.map((f) => normalizeAssetKey(f.webkitRelativePath || f.name).split('/')[0]).filter(Boolean);
    const commonPrefix = prefixes.length && prefixes.every((p) => p === prefixes[0]) ? `${prefixes[0]}/` : '';
    for (const file of entries) {
      const original = normalizeAssetKey(file.webkitRelativePath || file.name);
      const key = original.startsWith(commonPrefix) ? original.slice(commonPrefix.length) : original;
      if (key) this.files.set(key, file);
    }
    for (const [key, file] of [...this.files]) {
      const base = key.split('/').pop();
      if (!this.files.has(base)) this.files.set(base, file);
    }
  }

  aliases(uri) { return aliases(uri); }
  setAsset(uri, bytes) { for (const key of aliases(uri)) this.cache.set(key, bytes); }
  getAsset(uri) { return this.cache.get(uri) || null; }
  hasAsset(uri) { return this.cache.has(uri); }
  clearCache() { this.cache.clear(); this.urlCache.clear(); }

  async resolveAsync(assetPath, { parentAssetPath = '' } = {}) {
    for (const key of aliases(assetPath)) if (this.cache.has(key)) return [assetPath, this.cache.get(key), key];
    const key = relativeKey(assetPath, parentAssetPath);
    const file = this.files.get(key) || this.files.get(normalizeAssetKey(assetPath));
    if (!file) throw new Error(`Missing local asset: ${assetPath}`);
    const canonical = normalizeAssetKey(file.webkitRelativePath || file.name);
    this.onProgress({ assetPath, loaded: 0, total: file.size || 0 });
    const bytes = new Uint8Array(await file.arrayBuffer());
    this.onProgress({ assetPath, loaded: bytes.byteLength, total: bytes.byteLength });
    this.budget.claim(canonical, bytes.byteLength);
    this.setAsset(assetPath, bytes);
    this.setAsset(key, bytes);
    return [assetPath, bytes, canonical];
  }
}

export class HttpAssetResolver {
  constructor(baseUrl, budget, onStatus = () => {}, onProgress = () => {}) {
    this.baseUrl = baseUrl;
    this.budget = budget;
    this.onStatus = onStatus;
    this.onProgress = onProgress;
    this.cache = new Map();
    this.urlCache = new Map();
  }

  aliases(uri) { return aliases(uri); }
  setAsset(uri, bytes, url = '') {
    for (const key of aliases(uri)) {
      this.cache.set(key, bytes);
      if (url) this.urlCache.set(key, url);
    }
  }
  getAsset(uri) { return this.cache.get(uri) || null; }
  hasAsset(uri) { return this.cache.has(uri); }
  clearCache() { this.cache.clear(); this.urlCache.clear(); }

  rewrite(assetPath, parentAssetPath = '') {
    const p = String(assetPath || '').replace(/<[^>]*>\s*$/, '');
    if (/^(https?:|data:|blob:)/i.test(p)) return p;
    const base = parentAssetPath && /^(https?:)/i.test(parentAssetPath)
      ? parentAssetPath.slice(0, parentAssetPath.lastIndexOf('/') + 1)
      : this.baseUrl;
    return new URL(p, base).href;
  }

  async resolveAsync(assetPath, { parentAssetPath = '' } = {}) {
    for (const key of aliases(assetPath)) if (this.cache.has(key)) return [assetPath, this.cache.get(key), this.urlCache.get(key)];
    const url = this.rewrite(assetPath, parentAssetPath);
    const github = githubContentsRequest(url);
    const requestUrl = github?.url || url;
    const requestHeaders = github?.headers || { Accept: '*/*' };
    let response;
    try { response = await fetch(requestUrl, { cache: 'no-store', headers: requestHeaders }); }
    catch (error) { throw new Error(`Could not fetch '${assetPath}'. The server may block cross-origin access (CORS): ${error.message}`); }
    if (!response.ok && response.status !== 206) {
      const remaining = response.headers.get('x-ratelimit-remaining');
      const hint = response.status === 403 || response.status === 429
        ? ` (GitHub or the remote asset server denied/rate-limited the request${remaining !== null ? `; GitHub requests remaining: ${remaining}` : ''})`
        : '';
      throw new Error(`HTTP ${response.status} while fetching '${assetPath}'${hint}.`);
    }
    const declared = Number(response.headers.get('content-length')) || 0;
    if (declared) this.budget.claim(url, declared);
    if (!response.body) {
      this.onProgress({ assetPath, loaded: 0, total: declared, url });
      const bytes = new Uint8Array(await response.arrayBuffer());
      if (!declared) this.budget.claim(url, bytes.byteLength);
      this.setAsset(assetPath, bytes, url);
      this.onProgress({ assetPath, loaded: bytes.byteLength, total: bytes.byteLength, url, done: true });
      return [assetPath, bytes, url];
    }
    const reader = response.body.getReader();
    const chunks = [];
    let size = 0;
    this.onProgress({ assetPath, loaded: 0, total: declared, url });
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value); size += value.byteLength;
      if (!declared && !this.budget.records.has(url)) this.budget.claim(url, size);
      this.onProgress({ assetPath, loaded: size, total: declared, url });
      this.onStatus(`Downloading ${assetPath}: ${formatBytes(size)}`);
    }
    if (declared && size > declared) this.budget.claim(`${url}#actual`, size - declared);
    const bytes = new Uint8Array(size);
    let offset = 0; for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
    this.setAsset(assetPath, bytes, url);
    this.onProgress({ assetPath, loaded: size, total: size, url, done: true });
    return [assetPath, bytes, url];
  }
}

export function seedUSDZ(layer, bytes, filename) {
  if (!/\.usdz$/i.test(filename)) return { rootBytes: bytes, rootName: filename, entries: [] };
  const entries = parseUSDZEntries(bytes);
  const root = entries.find((entry) => /\.(usd|usda|usdc)$/i.test(entry.name)) || entries[0];
  if (!root) throw new Error('USDZ archive has no USD layer.');
  for (const entry of entries) layer.setAsset(entry.name, entry.data);
  return { rootBytes: root.data, rootName: root.name, entries };
}

export async function hydrateLayerTextures(layer, resolver, onStatus = () => {}) {
  if (typeof layer.setLoadTextureInNative === 'function') layer.setLoadTextureInNative(false);
  if (!layer.layerToRenderScene()) throw new Error(layer.error?.() || 'Could not create the USD render scene.');
  const count = Number(layer.numImages?.() || 0);
  let fetched = 0;
  for (let i = 0; i < count; i++) {
    const image = layer.getImagePtr?.(i);
    if (!image || image.byteLength > 0 || !image.uri) continue;
    const [, bytes] = await resolver.resolveAsync(image.uri);
    for (const key of resolver.aliases(image.uri)) layer.setAsset(key, bytes);
    fetched++;
    onStatus(`Fetched texture: ${image.uri}`);
  }
  // Keep encoded image bytes in the native scene. LightUSDLoaderUtils then
  // gives browser-supported formats (JPEG/PNG/WebP) to Three.js via Blob and
  // TextureLoader, avoiding native RGB/JPEG decode failures and long WASM
  // texture conversion stalls. EXR/HDR/KTX2 still use their dedicated paths.
  if (typeof layer.setLoadTextureInNative === 'function') layer.setLoadTextureInNative(false);
  if (!layer.layerToRenderScene()) throw new Error(layer.error?.() || 'Could not load USD textures.');
  return fetched;
}

export async function composeLayer({ loader, bytes, filename, resolver, composePayload = false, onStatus = () => {} }) {
  const layer = new loader.native_.LightUSDLoaderNative();
  layer.setMaxMemoryLimitMB?.(512);
  layer.setAllowParentRelativeAssetPaths?.(true);
  layer.setLoadTextureInNative?.(false);
  const archive = seedUSDZ(layer, bytes, filename);
  if (!layer.loadAsLayerFromBinary(archive.rootBytes, archive.rootName)) {
    throw new Error(layer.error?.() || `Failed to parse ${filename}.`);
  }
  // Keep USDZ entries in the JS resolver too. The native layer is seeded above,
  // but texture hydration intentionally goes through the resolver so encoded
  // images can be handled consistently with external files.
  for (const entry of archive.entries) {
    resolver.setAsset?.(entry.name, entry.data);
  }
  const composer = new LightUSDComposer();
  composer.setLayer(layer);
  composer.setUSDLoader(loader);
  composer.setAssetResolver(resolver);
  composer.setBaseWorkingPath('./');
  composer.setAssetSearchPaths(['./']);
  await composer.progressiveComposition({ composePayload });
  await hydrateLayerTextures(layer, resolver, onStatus);
  return { layer, composer, archive, hasPayload: LightUSDComposer.hasPayload(layer) };
}
