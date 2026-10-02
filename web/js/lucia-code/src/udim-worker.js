import { bakeLayerUDIM, selectUDIMSites } from '../../src/udim-bake.js';
import { udimInventory } from './udim-workflow.js';
let nativePromise, nextId = 0;
const pending = new Map();
self.onmessage = async ({data}) => {
  if (data.type === 'asset') { const resolve = pending.get(data.id); pending.delete(data.id); resolve?.(data.bytes); return; }
  if (!['inspect','bake'].includes(data.type)) return;
  let converter;
  try {
    nativePromise ||= import('../../src/lightusd/lightusd_next.js').then(({default:factory}) => factory());
    const native = await nativePromise;
    converter = new native.NextUSDZConverterNative();
    const budget = Math.min(data.options.udimMemoryBudgetBytes, 1024 * 1024 * 1024);
    converter.setMaxRetainedPayloadBytes(budget);
    if (!converter.loadFromBinary(new TextEncoder().encode(data.source), data.filename)) throw new Error(converter.error());
    const description = converter.describeUDIM();
    if (!description.success) throw new Error(description.error);
    const baseDir = data.filename.includes('/') ? data.filename.slice(0,data.filename.lastIndexOf('/')) : '';
    const inventory = udimInventory(description.sites, data.keys, baseDir);
    if (data.type === 'inspect') { self.postMessage({type:'result',result:{inventory}}); return; }
    const sites = selectUDIMSites(description.sites, data.options.udimShaderPaths);
    const source = {keys:data.keys, fetch: (key,{maxBytes}) => new Promise(resolve => {
      const id = ++nextId; pending.set(id,resolve); self.postMessage({type:'fetch',id,key,maxBytes});
    })};
    const baked = await bakeLayerUDIM(native, converter, source, {...data.options, udimThumbnails:true, progress:p => self.postMessage({type:'progress',progress:{message:p.message,percentage:100*p.current/p.total}})}, baseDir);
    const assets = baked.assets.map(asset => ({...asset, name:baseDir ? `${baseDir}/${asset.name}` : asset.name}));
    const result = {usda:converter.exportAsUSDA(), assets, tiles:baked.tiles,
      paths:[...new Set(sites.map(site=>site.path))], inventory};
    self.postMessage({type:'result',result}, result.assets.flatMap(asset=>[asset.data.buffer,...(asset.thumbnail?[asset.thumbnail.buffer]:[])]));
  } catch(error) { self.postMessage({type:'error',message:error?.message || String(error)}); }
  finally { converter?.delete(); }
};
