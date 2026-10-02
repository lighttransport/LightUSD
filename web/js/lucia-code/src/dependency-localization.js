import { scanUSDAssetReferences } from './usd-dependencies.js';
import { localizeUSDDependencies } from './usd-doctor.js';
import { LuciaError, encoder, decoder } from './utils.js';

function normalize(path) {
  if (/^(?:[A-Za-z][\w+.-]*:|\/)/.test(path)) return path;
  const parts = [];
  for (const part of path.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') { if (!parts.length) throw new LuciaError('LUCIA_DEPENDENCY_PATH', 'Dependency escapes the project.'); parts.pop(); }
    else parts.push(part);
  }
  return parts.join('/');
}
const directory = path => path.includes('/') ? path.slice(0, path.lastIndexOf('/') + 1) : '';
function relative(from, to) {
  const base = directory(from).split('/').filter(Boolean), target = to.split('/');
  while (base.length && target.length && base[0] === target[0]) { base.shift(); target.shift(); }
  return [...base.map(() => '..'), ...target].join('/');
}
function rewrite(source, oldLayer, newLayer, mapping) {
  let changed = 0, result = source;
  for (const asset of scanUSDAssetReferences(source, { locations: true }).reverse()) {
    const external = /^(?:[A-Za-z][\w+.-]*:|\/)/.test(asset.path);
    const absolute = !oldLayer && mapping.has(asset.path) ? asset.path
      : !external && /^[A-Za-z][\w+.-]*:\/\//.test(oldLayer) ? new URL(asset.path, oldLayer).href
      : normalize(external ? asset.path : directory(oldLayer) + asset.path);
    const target = mapping.get(absolute) || absolute;
    const replacement = /^(?:[A-Za-z][\w+.-]*:|\/)/.test(target) ? target : relative(newLayer, target);
    if (replacement !== asset.path) { result = result.slice(0, asset.start) + replacement + result.slice(asset.end); changed++; }
  }
  return { source: result, changed };
}
export async function localizeProjectDependencies(source, assets, pathMap, resolveAsset = null) {
  const validated = localizeUSDDependencies('', pathMap), mapping = new Map(validated.mappings.map(({ from, to }) => [from, to]));
  const next = new Map(assets), loaded = new Map(assets), moved = new Map();
  let fetchedBytes = 0;
  for (const [from, to] of mapping) {
    let asset = assets.get(from);
    if ((asset || resolveAsset) && assets.has(to) && !mapping.has(to)) throw new LuciaError('LUCIA_ASSET_COLLISION', `An asset already exists at ${to}.`);
    if (!asset && resolveAsset) {
      const bytes = await resolveAsset(from);
      if (!(bytes instanceof Uint8Array) || bytes.byteLength > 256 * 1024 * 1024) throw new LuciaError('LUCIA_DEPENDENCY_RESOLVER', `Resolver returned invalid asset bytes: ${from}`);
      fetchedBytes += bytes.byteLength;
      if (fetchedBytes > 256 * 1024 * 1024) throw new LuciaError('LUCIA_DEPENDENCY_RESOLVER', 'Resolved dependencies exceed the 256 MiB budget.');
      asset = { bytes: bytes.slice() };
      loaded.set(from, asset);
    }
    if (asset) moved.set(to, asset);
    next.delete(from);
  }
  for (const [to, asset] of moved) next.set(to, asset);
  let changed = 0;
  for (const [oldPath, asset] of loaded) {
    if (!/\.usd[ac]?$/i.test(oldPath) || !(asset.bytes instanceof Uint8Array)) continue;
    const newPath = mapping.get(oldPath) || oldPath, binary = decoder.decode(asset.bytes.subarray(0, 8)) === 'PXR-USDC';
    let text, document;
    try {
      if (binary) {
        const module = await import('../../src/lightusd/lightusd_next.js').then(({ default: factory }) => factory());
        document = new module.LayerDocument();
        const result = document.load(asset.bytes);
        if (!result.success) throw new LuciaError('LUCIA_DEPENDENCY_LAYER', result.error);
        text = document.exportUSDA().text;
      } else text = new TextDecoder('utf-8', { fatal: true }).decode(asset.bytes);
      const edited = rewrite(text, oldPath, newPath, mapping);
      changed += edited.changed;
      if (edited.changed) {
        let bytes = encoder.encode(edited.source);
        if (document) {
          const result = document.load(bytes);
          if (!result.success) throw new LuciaError('LUCIA_DEPENDENCY_LAYER', result.error);
          const exported = document.exportUSDC();
          if (!exported.success) throw new LuciaError('LUCIA_DEPENDENCY_LAYER', exported.error);
          bytes = exported.data;
        }
        next.set(newPath, { ...asset, bytes });
      }
    } finally { document?.delete(); }
  }
  const root = rewrite(source, '', '', mapping); changed += root.changed;
  if (!changed && !moved.size) throw new LuciaError('LUCIA_DEPENDENCY_LOCALIZATION_NOOP', 'No exact USD asset references matched the localization map.');
  return { source: root.source, assets: next, changed, mappings: validated.mappings };
}
