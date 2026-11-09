// SPDX-License-Identifier: Apache-2.0
// Two authored texture aliases that rewrite to one URL must share one
// network transfer while keeping both packaged entries (references stay valid).
import assert from 'node:assert/strict';
import { flattenNextOverHttp, HttpAssetResolver } from '../http-asset-resolver.js';
import { parseUSDZEntries } from '../src/usdzconvert.js';

const encode = (value) => new TextEncoder().encode(value);
const PNG = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3]);

let fetches = 0;
const realFetch = globalThis.fetch;
globalThis.fetch = async () => {
  fetches++;
  return new Response(PNG, { status: 200, headers: { 'content-type': 'image/png' } });
};

const resolver = new HttpAssetResolver({ baseUrl: 'https://example.test/' });

class FlattenSession {
  begin() { return { success: true }; }
  step() {
    return {
      success: true,
      status: 'done',
      data: encode('PXR-USDC'),
      // `sub/../a.png` rewrites to the same URL as `a.png` but is not one of
      // the resolver's cosmetic aliases, so only the URL cache can dedup it.
      assetPaths: ['a.png', 'sub/../a.png'],
    };
  }
  provideLayer() { return { success: true }; }
  end() {}
  delete() {}
}

try {
  const result = await flattenNextOverHttp({
    renderer: { native: { NextFlattenSession: FlattenSession } },
    rootBytes: encode('#usda 1.0\n'),
    filename: 'root.usda',
    resolver,
  });

  assert.equal(fetches, 1, 'aliased texture URLs share one network transfer');
  assert.equal(resolver.fetchLog.length, 2, 'both authored paths were resolved');
  assert.equal(result.fetchedAssets, 1, 'flatten counts one fetched payload');

  const entries = parseUSDZEntries(result.usdz);
  const textures = entries.filter((entry) => entry.name.endsWith('.png'));
  assert.equal(textures.length, 2, 'both authored aliases stay packaged');
  for (const texture of textures) {
    assert.deepEqual(new Uint8Array(texture.data), PNG);
  }
  console.log('ok - HTTP texture aliases share one fetch');
} finally {
  globalThis.fetch = realFetch;
}
