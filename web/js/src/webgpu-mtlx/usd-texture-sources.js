// SPDX-License-Identifier: Apache-2.0
import { GraphError } from './graph.js';

const udimPattern = /<UDIM>|<UVTILE>|%04d|%\(UDIM\)d/i;

/** Conservative source-layer index collected before composition remaps prims.
 * An authored key is usable only if every observed source agrees on its URL.
 * This is not a replacement for a full USD property-stack asset resolver.
 */
export class USDTextureSources {
  constructor() { this.assets = new Map(); this.requests = new Map(); }
  register(snapshot, sourceURL) {
    const source = new URL(sourceURL);
    if (!['http:', 'https:'].includes(source.protocol) || source.username || source.password) throw new Error('USD source must be an HTTP(S) URL without credentials');
    const records = snapshot.assetPaths || (snapshot.prims || []).flatMap(prim =>
      Object.entries(prim.properties || {}).filter(([, p]) => p.type === 'asset')
        .map(([name, p]) => ({ authored: p.value, propertyPath: `${prim.path}.${name}` })));
    for (const record of records) {
        const authored = record.authored;
        if (typeof authored !== 'string' || !authored) continue;
        // Store unsupported paths too: unresolved data must not disappear.
        let url;
        try {
          if (/[\[\]\\]/.test(authored) || /[<>]/.test(authored) && !udimPattern.test(authored)) throw new Error('package, tiled, or platform path');
          const target = new URL(authored, source);
          if (!['http:', 'https:'].includes(target.protocol) || target.origin !== source.origin || target.username || target.password || target.hash) throw new Error('nonlocal or unsupported URL');
          url = target.href;
        } catch { url = null; }
        if (!this.assets.has(authored)) this.assets.set(authored, new Map());
        this.assets.get(authored).set(`${source.href}\n${record.propertyPath}`, { source: source.href, propertyPath: record.propertyPath, url, authored });
    }
  }
  resolveAsset = (authored, { propertyPath, colorspace }) => {
    const sources = this.assets.get(authored);
    const fail = message => { throw new GraphError('USD_ASSET', propertyPath, message); };
    if (!sources?.size) fail(`no source-layer provenance for ${authored}`);
    const entries = [...sources.values()], urls = new Set(entries.map(entry => entry.url));
    if (urls.has(null)) fail(`unsupported source asset path ${authored}`);
    if (urls.size !== 1) fail(`ambiguous source layers for ${authored}`);
    if (typeof colorspace !== 'string' || !colorspace) fail('texture color space must be resolved first');
    const url = entries[0].url, key = JSON.stringify([url, colorspace]);
    this.requests.set(key, { url, colorspace, sources: entries, ...(udimPattern.test(authored) ? { udim: true, authored } : {}) });
    return key;
  };
  snapshot() { return [...this.assets].map(([authored, sources]) => ({ authored, sources: [...sources.values()] })); }
}
