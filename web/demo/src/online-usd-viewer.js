import { flattenNextOverHttp } from 'lightusd-js/http-asset-resolver.js';
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

export async function composeLayer({ loader, bytes, filename, resolver, onStatus = () => {} }) {
  const flattened = await flattenNextOverHttp({
    renderer: { native: loader.native_ }, rootBytes: bytes, filename, resolver, onStatus,
  });
  const layer = await new Promise((resolve, reject) => {
    loader.parse(flattened.usdz, 'scene.usdz', resolve, reject, {
      backend: 'next', maxMemoryLimitMB: 512,
    });
  });
  return { layer, composition: 'references and payloads' };
}
