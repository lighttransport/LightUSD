const CACHE_VERSION = 2;
const DEFAULT_MAX_BYTES = 2 * 1024 * 1024;
const validReport = (report) => Boolean(report && report.stage && report.score && Array.isArray(report.issues) && Array.isArray(report.materials) && Array.isArray(report.textures) && ['triangles', 'vertices', 'estimatedGpuBytes'].every((key) => Number.isFinite(report.stage[key])) && ['missingTextureImages', 'invalidTextureDimensions', 'invalidTexturePixels'].every((key) => report.stage[key] == null || (Number.isFinite(report.stage[key]) && report.stage[key] >= 0)) && ['geometry', 'materials', 'textures', 'physics'].every((key) => Number.isFinite(report.score[key])));

function hashText(text) {
  let first = 2166136261, second = 2246822519;
  for (const character of String(text)) { const code = character.charCodeAt(0); first = Math.imul(first ^ code, 16777619); second = Math.imul(second ^ code, 3266489917); }
  return `${(first >>> 0).toString(16).padStart(8, '0')}${(second >>> 0).toString(16).padStart(8, '0')}`;
}

function bytesHash(bytes) {
  let first = 2166136261, second = 2246822519;
  const view = bytes instanceof ArrayBuffer ? new Uint8Array(bytes) : ArrayBuffer.isView(bytes) ? new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength) : bytes || [];
  for (const value of view) { first = Math.imul(first ^ value, 16777619); second = Math.imul(second ^ value, 3266489917); }
  return `${(first >>> 0).toString(16).padStart(8, '0')}${(second >>> 0).toString(16).padStart(8, '0')}`;
}

function assetFingerprint(asset) {
  const bytes = asset?.bytes;
  const metadata = `${asset?.width ?? ''},${asset?.height ?? ''},${asset?.colorSpace ?? ''},${asset?.normalY ?? ''},${asset?.mimeType ?? ''}`;
  if (bytes instanceof ArrayBuffer || ArrayBuffer.isView(bytes)) return `${bytes.byteLength}:${bytesHash(bytes)}:${metadata}`;
  if (bytes && typeof bytes.length === 'number') return `${bytes.length}:${bytesHash(bytes)}:${metadata}`;
  return `${asset?.url || ''}:${asset?.size || 0}:${metadata}`;
}

export function reportCacheKey({ source = '', assets = new Map() } = {}) {
  const entries = [...(assets instanceof Map ? assets.entries() : Object.entries(assets || {}))]
    .map(([path, asset]) => `${path}\0${assetFingerprint(asset)}`).sort().join('\n');
  return `v${CACHE_VERSION}:${hashText(`${source}\n${entries}`)}`;
}

export function createReportCache(storage = globalThis.localStorage, { prefix = 'lucia:asset-report:', maxBytes = DEFAULT_MAX_BYTES, maxEntries = 4 } = {}) {
  const indexKey = `${prefix}index`, entryLimit = Math.max(1, Math.floor(maxEntries) || 1), keyFor = (input) => `${prefix}${reportCacheKey(input)}`;
  const readIndex = () => { try { const value = JSON.parse(storage?.getItem?.(indexKey) || '[]'); return Array.isArray(value) ? value.filter((key) => typeof key === 'string' && key !== indexKey) : []; } catch { return []; } };
  const writeIndex = (keys) => { try { storage?.setItem?.(indexKey, JSON.stringify(keys)); } catch { /* Storage is optional or full. */ } };
  const remove = (key) => { try { storage?.removeItem?.(key); writeIndex(readIndex().filter((entry) => entry !== key)); } catch { /* Storage is optional. */ } };
  const touch = (key) => { const keys = readIndex(), index = keys.indexOf(key); if (index >= 0 && index !== keys.length - 1) { keys.splice(index, 1); keys.push(key); writeIndex(keys); } };
  const load = (key) => {
    try {
      if (!storage?.getItem) return null;
      const value = JSON.parse(storage.getItem(key));
      if (value?.version !== CACHE_VERSION || value.key !== key || !validReport(value.report)) { remove(key); return null; }
      touch(key);
      return value.report;
    } catch { remove(key); return null; }
  };
  const save = (key, report) => {
    try {
      if (!storage?.setItem || !validReport(report)) return false;
      const value = JSON.stringify({ version: CACHE_VERSION, key, report });
      if (new TextEncoder().encode(value).byteLength > maxBytes) return false;
      storage.setItem(key, value);
      const keys = [...readIndex().filter((entry) => entry !== key), key];
      while (keys.length > entryLimit) storage.removeItem(keys.shift());
      writeIndex(keys);
      return true;
    } catch { return false; }
  };
  return { keyFor, load, save, remove };
}
