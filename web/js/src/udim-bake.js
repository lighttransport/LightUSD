// SPDX-License-Identifier: Apache-2.0
// Atlas image work runs in the same bounded C++ core in every WASM product.
const decoder = new TextDecoder();

export function udimOptions(opts = {}) {
  const mode = String(opts.udimBake ?? 'off').toLowerCase();
  if (!['off', 'grid', 'dense'].includes(mode)) throw new Error('udimBake must be off, grid, or dense');
  const number = (name, fallback, lo, hi) => {
    const n = opts[name] ?? fallback;
    if (!Number.isSafeInteger(n) || n < lo || n > hi) throw new RangeError(`${name} must be ${lo}..${hi}`);
    return n;
  };
  const policy = opts.udimCrossTile ?? 'reject';
  if (!['reject', 'split'].includes(policy)) throw new Error('udimCrossTile must be reject or split');
  return {
    mode, maxTiles: number('udimMaxTiles', 100, 1, 8999),
    maxAtlasSize: number('udimMaxAtlasSize', 8192, 1, 32768),
    memoryBudgetBytes: number('udimMemoryBudgetBytes', 512 * 1024 * 1024, 1, 0x7fffffff),
    densePadding: number('udimDensePadding', 2, 0, 1024),
    subdivisionLevel: number('udimSubdivisionLevel', 2, 1, 8),
    crossTile: policy,
  };
}
export function splitUDIMPattern(path) {
  const hits = [...String(path).matchAll(/<UDIM>|%04d|%\(UDIM\)d/g)];
  if (hits.length !== 1) return null;
  const hit = hits[0];
  return {prefix: path.slice(0, hit.index), suffix: path.slice(hit.index + hit[0].length)};
}
function normalize(path) {
  const parts = [];
  for (const part of String(path).replace(/\\/g, '/').split('/')) {
    if (!part || part === '.') continue;
    if (part === '..' && parts.length && parts[parts.length - 1] !== '..') parts.pop();
    else parts.push(part);
  }
  return parts.join('/');
}
export function discoverUDIMTiles(keys, pattern, baseDir = '', maxTiles = 100) {
  const split = splitUDIMPattern(normalize(baseDir + '/' + pattern));
  if (!split) throw new Error(`Invalid UDIM pattern: ${pattern}`);
  const result = [];
  const seen = new Set();
  for (const key of keys) {
    const path = normalize(key);
    if (!path.startsWith(split.prefix) || !path.endsWith(split.suffix)) continue;
    const digits = path.slice(split.prefix.length, path.length - split.suffix.length || undefined);
    if (!/^[0-9]{4}$/.test(digits)) continue;
    const id = Number(digits);
    if (id < 1001 || id > 9999) continue;
    if (seen.has(id)) throw new Error(`UDIM bake: ambiguous tile ${id} for ${pattern}`);
    seen.add(id); result.push({id, key});
    if (result.length > maxTiles) throw new Error(`UDIM bake: tile count exceeds configured limit for ${pattern}`);
  }
  if (!result.length) throw new Error(`UDIM bake: no tiles resolved for ${pattern}`);
  result.sort((a, b) => a.id - b.id);
  return result;
}
function wasmBytes(native, bytes, call, extra = 0) {
  const alloc = native._lightusd_next_alloc || native._lightusd_combined_alloc || native._lightusd_udim_alloc || native._malloc;
  const free = native._lightusd_next_free || native._lightusd_combined_free || native._lightusd_udim_free || native._free;
  if (typeof alloc !== 'function' || typeof free !== 'function') throw new Error('WASM allocator unavailable');
  const source = bytes.buffer === native.HEAPU8.buffer ? bytes.slice() : bytes;
  const outputOffset = (source.byteLength + 7) - (source.byteLength + 7) % 8;
  const ptr = alloc(Math.max(1, outputOffset + extra));
  if (!ptr) throw new Error('UDIM bake: WASM input allocation failed');
  try {
    native.HEAPU8.set(source, Number(ptr));
    const end = typeof ptr === 'bigint' ? ptr + BigInt(outputOffset) : ptr + outputOffset;
    return call(ptr, end);
  } finally { free(ptr); }
}
function info(native, handle) {
  const size = native._lightusd_udim_info_size(handle);
  const ptr = Number(native._lightusd_udim_info_data(handle));
  if (size < 0 || ptr < 0 || ptr + size > native.HEAPU8.byteLength) throw new Error('UDIM bake: invalid WASM metadata');
  const text = decoder.decode(native.HEAPU8.subarray(ptr, ptr + size));
  return handle ? JSON.parse(text) : {error: text};
}
export async function inspectUDIMTiles(native, source, pattern, opts = {}, baseDir = '') {
  const options = udimOptions(opts);
  const tiles = discoverUDIMTiles(source.keys, pattern, baseDir, options.maxTiles);
  const fetch = async key => {
    const data = await source.fetch(key, {maxBytes: options.memoryBudgetBytes});
    if (!ArrayBuffer.isView(data) || data.byteLength >= options.memoryBudgetBytes) {
      throw new Error('UDIM bake: encoded tile exceeds memory limit');
    }
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  };
  let width = 0, height = 0;
  for (const tile of tiles) {
    const bytes = await fetch(tile.key);
    const dims = wasmBytes(native, bytes, (ptr, out) => {
      if (native._lightusd_udim_image_info(ptr, bytes.byteLength, out, options.memoryBudgetBytes - bytes.byteLength) !== 1) throw new Error(`UDIM bake: invalid image header: ${tile.key}`);
      const dv = new DataView(native.HEAPU8.buffer, Number(out), 12);
      return [dv.getUint32(0, true), dv.getUint32(4, true)];
    }, 12);
    let [w, h] = dims;
    if (!w || !h || w > 32768 || h > 32768) throw new Error(`UDIM bake: oversized tile: ${tile.key}`);
    const cap = opts.maxTextureSize || 0;
    if (cap > 0 && Math.max(w, h) > cap) {
      const scale = cap / Math.max(w, h);
      w = Math.max(1, Math.round(w * scale)); h = Math.max(1, Math.round(h * scale));
    }
    width = Math.max(width, w); height = Math.max(height, h);
  }
  return {tiles, width, height};
}
export async function bakeUDIMAtlas(native, source, pattern, opts = {}, baseDir = '', srgb = true) {
  const options = udimOptions(opts);
  if (options.mode === 'off') throw new Error('UDIM baking is disabled');
  if (typeof native._lightusd_udim_begin !== 'function') throw new Error('Rebuild WASM to enable UDIM baking');
  const tiles = discoverUDIMTiles(source.keys, pattern, baseDir, options.maxTiles);
  let atlasBytes = 0;
  const fetch = async key => {
    const limit = options.memoryBudgetBytes - atlasBytes;
    const data = await source.fetch(key, {maxBytes: limit});
    if (!ArrayBuffer.isView(data)) throw new Error(`UDIM bake: missing bytes for ${key}`);
    if (data.byteLength > limit) throw new Error(`UDIM bake: encoded tile exceeds memory limit: ${key}`);
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  };
  const metadata = opts.udimSharedLayout
    ? {width: opts.udimSharedLayout.tileWidth, height: opts.udimSharedLayout.tileHeight}
    : await inspectUDIMTiles(native, source, pattern, opts, baseDir);
  const {width, height} = metadata;
  const ids = new Uint32Array(opts.udimSharedLayout?.ids || tiles.map(tile => tile.id));
  const handle = wasmBytes(native, new Uint8Array(ids.buffer), ptr => native._lightusd_udim_begin(
    ptr, ids.length, width, height, options.mode === 'dense' ? 2 : 1,
    options.maxTiles, options.maxAtlasSize, options.densePadding,
    options.memoryBudgetBytes, srgb ? 1 : 0));
  if (!handle) throw new Error(info(native, 0).error || 'UDIM bake: builder allocation failed');
  // Dense atlas dimensions include gutters and the reserved blank cell.
  try {
    const dimensions = info(native, handle); atlasBytes = dimensions.width * dimensions.height * 16;
    for (let i = 0; i < tiles.length; ++i) {
      const tile = tiles[i];
      const bytes = await fetch(tile.key);
      if (wasmBytes(native, bytes, ptr => native._lightusd_udim_add(handle, tile.id, ptr, bytes.byteLength)) !== 1)
        throw new Error(info(native, handle).error || `UDIM bake: tile decode failed: ${tile.key}`);
      opts.progress?.({stage: 'udim', current: i + 1, total: tiles.length, message: 'Stitching UDIM tiles', path: tile.key});
    }
    const present = new Set(tiles.map(tile => tile.id));
    for (const id of ids) {
      if (!present.has(id) && native._lightusd_udim_blank(handle, id) !== 1) {
        throw new Error(info(native, handle).error);
      }
    }
    const format = String(opts.textureFormat || 'keep').toLowerCase();
    const encoding = {keep: 0, png: 1, jpg: 2, jpeg: 2, exr: 3}[format];
    if (encoding === undefined) throw new Error('UDIM bake: unsupported texture format');
    if (native._lightusd_udim_finish(handle, encoding, opts.jpegQuality ?? 90) !== 1) throw new Error(info(native, handle).error);
    const layout = info(native, handle);
    const size = native._lightusd_udim_data_size(handle);
    const ptr = Number(native._lightusd_udim_data(handle));
    if (size <= 0 || ptr <= 0 || ptr + size > native.HEAPU8.byteLength) throw new Error('UDIM bake: invalid WASM output');
    return {data: native.HEAPU8.slice(ptr, ptr + size), layout, name: layout.name, tiles};
  } finally { native._lightusd_udim_release(handle); }
}

export async function bakeLayerUDIM(native, layer, source, opts, baseDir = '', retain = true) {
  const options = udimOptions(opts);
  if (options.mode === 'off') return {assets: [], consumed: new Set(), jobs: [], tiles: 0};
  if (typeof layer.describeUDIM !== 'function' || typeof layer.applyUDIM !== 'function') throw new Error('Rebuild WASM to enable UDIM layer edits');
  const description = layer.describeUDIM();
  if (!description.success) throw new Error(description.error);
  const groups = new Map(), sharedLayouts = new Map();
  for (const site of description.sites) {
    if (!groups.has(site.path)) groups.set(site.path, []);
    groups.get(site.path).push(site);
  }
  for (const [path, sites] of groups) {
    if (sites.length <= 1) continue;
    const patterns = [...new Set(sites.map(site => site.pattern))];
    const ids = new Set();
    for (const pattern of patterns) {
      const tiles = discoverUDIMTiles(source.keys, pattern, baseDir, options.maxTiles);
      for (const tile of tiles) ids.add(tile.id);
    }
    if (ids.size > options.maxTiles) throw new Error('UDIM bake: animation tile count exceeds configured limit');
    let width = 0, height = 0;
    for (const pattern of patterns) {
      const metadata = await inspectUDIMTiles(native, source, pattern, opts, baseDir);
      width = Math.max(width, metadata.width);
      height = Math.max(height, metadata.height);
    }
    sharedLayouts.set(path, {ids: [...ids].sort((a, b) => a - b), tileWidth: width, tileHeight: height});
  }
  const cache = new Map(), plans = [], assets = [], consumed = new Set(), jobs = [];
  let retained = 0, tiles = 0;
  for (const site of description.sites) {
    const shared = sharedLayouts.get(site.path);
    const key = JSON.stringify([site.pattern, site.srgb, shared]);
    let atlas = cache.get(key);
    if (!atlas) {
      atlas = await bakeUDIMAtlas(native, source, site.pattern, {...opts, udimSharedLayout: shared, udimMemoryBudgetBytes: options.memoryBudgetBytes - retained}, baseDir, site.srgb);
      const baseName = atlas.name;
      for (let suffix = 0; source.keys.includes(normalize(baseDir + '/' + atlas.name));) {
        const original = await source.fetch(normalize(baseDir + '/' + atlas.name), {maxBytes: options.memoryBudgetBytes - retained - atlas.data.length});
        if (original.length === atlas.data.length && original.every((byte, i) => byte === atlas.data[i])) break;
        atlas.name = baseName.replace(/(\.[^.]+)$/, `_${++suffix}$1`);
        if (suffix > 100000) throw new Error('UDIM bake: atlas name collision limit');
      }
      for (const tile of atlas.tiles) consumed.add(tile.key);
      tiles += atlas.tiles.length;
      const duplicate = jobs.some(job => job.name === atlas.name);
      if (!duplicate) jobs.push({site, sharedLayout: shared, name: atlas.name, digestName: atlas.layout.name});
      if (retain && !duplicate) {
        retained += atlas.data.length;
        if (retained > options.memoryBudgetBytes) throw new Error('UDIM bake: retained atlases exceed memory limit');
        assets.push({name: atlas.name, data: atlas.data});
      }
      // The layer edit cache contains only layouts and names.
      cache.set(key, {layout: atlas.layout, name: atlas.name});
    }
    plans.push({path: site.path, pattern: site.pattern, time: site.time, asset: atlas.name, layout: atlas.layout});
  }
  const result = layer.applyUDIM({options: {...options, memoryBudgetBytes: options.memoryBudgetBytes - retained}, plans});
  if (!result.success) throw new Error(result.error);
  for (const reference of result.remainingReferences || []) {
    consumed.delete(normalize(baseDir + '/' + reference));
    consumed.delete(normalize(reference));
  }
  return {assets, consumed, jobs, tiles};
}
