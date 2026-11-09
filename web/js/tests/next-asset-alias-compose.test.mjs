// SPDX-License-Identifier: Apache-2.0
// WASM composition resolves OBJ/MTL texture aliases: the store transfers alias
// relationships (not duplicate asset keys) into the render resolver, so a
// material authored against `a.png` / `b.png` still finds the shared payload.
import assert from 'node:assert/strict';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const { default: createNext } = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js',
  import.meta.url));
const next = await createNext();
const encode = (text) => new TextEncoder().encode(text);
const png = Uint8Array.from(Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j1ZkAAAAASUVORK5CYII=',
  'base64'));

const store = new next.NextAssetStore();
const stream = new next.RenderStream();
try {
  store.setAsset('root.usda', encode(
    '#usda 1.0\ndef Xform "Asset" (prepend references = @mesh.obj@) {}\n'));
  store.setAsset('mesh.obj', encode(
    'mtllib materials.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl a\nf 1 2 3\n'));
  store.setAsset('materials.mtl', encode('newmtl a\nmap_Kd a.png\nmap_Ks b.png\n'));
  store.setAsset('tex.png', png);
  store.setAlias('a.png', 'tex.png');
  store.setAlias('b.png', 'tex.png');

  const loaded = stream.beginCachedAsset(store, 'root.usda');
  assert.equal(loaded.success, true, stream.error());
  assert.ok(stream.numMeshes() >= 1, 'OBJ mesh composes through aliases');
  assert.ok(stream.numMaterials() >= 1, 'MTL material composes through aliases');
  assert.ok(stream.numTextures() >= 1, 'aliased textures resolve');
  for (const texture of stream.getAllTextures()) {
    assert.ok((texture.uri || '').length > 0, 'resolved texture retains its URI');
  }
  console.log('ok - WASM composition resolves aliased OBJ textures');
} finally {
  stream.delete();
  store.delete();
}
