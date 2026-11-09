import { udimOptions, discoverUDIMTiles, selectUDIMSites } from '../../src/udim-bake.js';
import { unpackUSDZ } from '../../src/usdzconvert.js';
import { LuciaError } from './utils.js';

export const UDIM_DEFAULTS = Object.freeze({ udimBake: 'grid', udimMaxTiles: 100,
  udimMaxAtlasSize: 8192, udimMemoryBudgetBytes: 256 * 1024 * 1024,
  udimCrossTile: 'reject', udimDensePadding: 2, udimSubdivisionLevel: 2 });
export const PROJECT_ASSET_LIMIT = 256 * 1024 * 1024;
export function safeAssetPath(path) {
  if (typeof path !== 'string' || !path || /^[\/\\]|^[A-Za-z]:/.test(path) ||
      path.includes('\\') || path.includes('\0') || path.split('/').some(p => !p || p === '..')) {
    throw new LuciaError('LUCIA_ASSET_PATH', 'Use an unambiguous relative asset path without parent traversal.');
  }
  const normalized = path.split('/').filter(p => p !== '.').join('/');
  if (!normalized) throw new LuciaError('LUCIA_ASSET_PATH', 'Asset paths cannot be empty.');
  return normalized;
}
export function stageAssetFiles(assets, entries) {
  const next = new Map(assets);
  for (const { name, bytes } of entries) {
    const path = safeAssetPath(name);
    if (!(bytes instanceof Uint8Array)) throw new LuciaError('LUCIA_ASSET_BYTES', `Missing bytes: ${path}`);
    const existing = next.get(path)?.bytes;
    if (existing && (existing.length !== bytes.length || !existing.every((v, i) => v === bytes[i])))
      throw new LuciaError('LUCIA_ASSET_COLLISION', `Different assets resolve to ${path}. Rename or remove the conflicting input.`);
    if (!existing) next.set(path, { bytes, generated: false });
  }
  const size = [...next.values()].reduce((n, a) => n + (a.bytes?.byteLength || 0), 0);
  if (size > PROJECT_ASSET_LIMIT) throw new LuciaError('LUCIA_ASSET_MEMORY', 'Project assets exceed the 256 MiB browser limit.');
  return next;
}
export function packageAssets(bytes) {
  const { entries, order } = unpackUSDZ(bytes);
  const rootName = order[0];
  if (!/\.usd[ac]?$/i.test(rootName || '')) throw new LuciaError('LUCIA_PACKAGE_ROOT', 'USDZ must start with a USD layer.');
  const assets = stageAssetFiles(new Map(), [...entries].filter(([name]) => name !== rootName).map(([name, bytes]) => ({name, bytes})));
  return { rootName, rootBytes: entries.get(rootName), assets };
}
export function udimInventory(sites, keys, baseDir = '') {
  const groups = new Map();
  for (const site of sites) {
    const item = groups.get(site.path) || { path: site.path, patterns: [], ids: new Set(), consumers: new Set(), error: null };
    if (!item.patterns.includes(site.pattern)) item.patterns.push(site.pattern);
    for (const path of site.consumers || []) item.consumers.add(path);
    try { for (const tile of discoverUDIMTiles(keys, site.pattern, baseDir, 8999)) item.ids.add(tile.id); }
    catch (error) { item.error = error.message; }
    groups.set(site.path, item);
  }
  return [...groups.values()].map(item => ({ ...item, ids: [...item.ids].sort((a,b) => a-b), consumers:[...item.consumers].sort() }));
}
export function validateUDIMPreview(result, options) {
  if (!result || typeof result.usda !== 'string' || !result.usda.startsWith('#usda') ||
      !Array.isArray(result.assets) || !Array.isArray(result.paths) || !result.paths.length ||
      result.paths.some(path=>typeof path !== 'string') || new Set(result.paths).size !== result.paths.length ||
      !Number.isSafeInteger(result.tiles) || result.tiles < 1 ||
      !Array.isArray(options.udimShaderPaths) || result.paths.length !== options.udimShaderPaths.length ||
      result.paths.some(path=>!options.udimShaderPaths.includes(path)))
    throw new LuciaError('LUCIA_UDIM_RESULT', 'The UDIM worker returned an incomplete preview.');
  const limits = udimOptions(options);
  let bytes = new TextEncoder().encode(result.usda).length;
  const names = new Set();
  for (const asset of result.assets) {
    const name = safeAssetPath(asset.name);
    if (names.has(name) || !(asset.data instanceof Uint8Array) || !asset.data.length ||
        !Number.isSafeInteger(asset.layout?.width) || !Number.isSafeInteger(asset.layout?.height) ||
        asset.layout.width < 1 || asset.layout.height < 1 ||
        Math.max(asset.layout.width, asset.layout.height) > limits.maxAtlasSize)
      throw new LuciaError('LUCIA_UDIM_RESULT', 'The UDIM worker returned invalid atlas data.');
    if (asset.thumbnail != null && !(asset.thumbnail instanceof Uint8Array))
      throw new LuciaError('LUCIA_UDIM_RESULT', 'Invalid atlas thumbnail.');
    names.add(name); bytes += asset.data.length + (asset.thumbnail?.byteLength || 0);
  }
  if (bytes > limits.memoryBudgetBytes) throw new LuciaError('LUCIA_UDIM_MEMORY', 'Retained preview exceeds the configured memory budget.');
  return result;
}

export class LuciaUDIMWorkflow {
  constructor(session, project, owner) { this.session = session; this.project = project; this.owner = owner; this.preview = null; }
  stamp(options) {
    return { source: this.session.exportUSDA(), revision: this.project.revision,
      usdRevision: this.project.domainRevisions.usd, assetsRevision: this.project.domainRevisions.assets,
      options: JSON.stringify(options), assets: [...this.project.assets].map(([path, asset]) => [path, asset.bytes]) };
  }
  matches(stamp, options) {
    const now = this.stamp(options);
    return stamp.source === now.source && stamp.revision === now.revision &&
      stamp.usdRevision === now.usdRevision && stamp.assetsRevision === now.assetsRevision &&
      stamp.options === now.options && stamp.assets.length === now.assets.length &&
      stamp.assets.every(([path, bytes], i) => path === now.assets[i][0] && bytes === now.assets[i][1]);
  }
  request(type, options, onProgress) {
    const budget = udimOptions(options).memoryBudgetBytes;
    this.owner.cancel();
    return new Promise((resolve, reject) => {
      const worker = this.owner.worker = new Worker(new URL('./udim-worker.js', import.meta.url), {type: 'module'});
      const finish = () => { if (this.owner.worker === worker) this.owner.releaseWorker(); };
      const fail = error => { finish(); reject(error); };
      this.owner.workerReject = reject;
      worker.onerror = event => fail(new LuciaError('LUCIA_UDIM_WORKER', event.message || 'UDIM worker failed.'));
      worker.onmessageerror = () => fail(new LuciaError('LUCIA_UDIM_WORKER', 'Unreadable UDIM worker response.'));
      worker.onmessage = ({data}) => {
        if (this.owner.worker !== worker) return;
        try {
          if (data.type === 'fetch') {
            const bytes = this.project.assets.get(data.key)?.bytes;
            if (!Number.isSafeInteger(data.id) || data.id < 1 || !Number.isSafeInteger(data.maxBytes) ||
                data.maxBytes < 1 || data.maxBytes > budget ||
                !(bytes instanceof Uint8Array) || bytes.length > data.maxBytes) throw new LuciaError('LUCIA_UDIM_ASSET', `Missing or oversized tile: ${data.key}`);
            const copy = bytes.slice(); worker.postMessage({type:'asset', id:data.id, bytes:copy}, [copy.buffer]);
          } else if (data.type === 'progress') onProgress?.(data.progress);
          else if (data.type === 'error') fail(new LuciaError('LUCIA_UDIM_BAKE', data.message));
          else if (data.type === 'result') { finish(); resolve(data.result); }
          else throw new LuciaError('LUCIA_UDIM_WORKER', 'Unexpected UDIM worker message.');
        } catch (error) { fail(error); }
      };
      try {
        onProgress?.({percentage:0,message:type === 'inspect' ? 'Detecting UDIM sets…' : 'Stitching UDIM preview…'});
        worker.postMessage({type, source:this.session.exportUSDA(), filename:this.session.filename,
        keys:[...this.project.assets.keys()], options}); } catch (error) { fail(error); }
    });
  }
  async inspect(onProgress) {
    const stamp = this.stamp(UDIM_DEFAULTS);
    const result = await this.request('inspect', UDIM_DEFAULTS, onProgress);
    if (!this.matches(stamp, UDIM_DEFAULTS))
      throw new LuciaError('LUCIA_UDIM_STALE', 'Scene or assets changed during detection. Detect sets again.');
    return result;
  }
  async bake(options, onProgress) {
    this.discard();
    const stamp = this.stamp(options);
    const result = validateUDIMPreview(await this.request('bake', options, onProgress), options);
    if (!this.matches(stamp, options)) throw new LuciaError('LUCIA_UDIM_STALE', 'Scene or assets changed during baking. Preview again.');
    this.preview = { ...result, stamp, options:{...options} };
    return this.preview;
  }
  stagedAssets(preview = this.preview) {
    if (!preview) throw new LuciaError('LUCIA_UDIM_PREVIEW', 'Preview the UDIM operation first.');
    const assets = stageAssetFiles(this.project.assets, preview.assets.map(a => ({name:a.name,bytes:a.data})));
    for (const asset of preview.assets) assets.set(asset.name, {bytes:asset.data, generated:true, udimLayout:asset.layout});
    return assets;
  }
  async apply(options) {
    const preview = this.preview;
    if (!preview || !this.matches(preview.stamp, options)) throw new LuciaError('LUCIA_UDIM_STALE', 'The preview is stale. Preview again before applying.');
    const previous = this.project.assets;
    this.project.assets = this.stagedAssets(preview);
    try { await this.session.replaceUSDA(preview.usda, 'Stitch UDIM texture sets'); }
    catch (error) { this.project.assets = previous; await this.session.restore(preview.stamp.source); throw error; }
    this.owner.lastStats = {kind:'udim', sets:preview.paths.length, tiles:preview.tiles, atlases:preview.assets.length};
    this.discard();
  }
  discard() { this.preview = null; }
}
export { selectUDIMSites };
