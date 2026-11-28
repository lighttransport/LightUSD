// SPDX-License-Identifier: Apache-2.0
// One canonical cache-key module must produce exactly the keys the three
// former implementations produced, and the old call sites must delegate to it.
import assert from 'node:assert/strict';
import {
  textureColorRole, textureSignature, textureCacheKey,
  textureDataCacheKey, materialCacheKey
} from '../src/lightusd/TextureCacheKeys.js';
import {materialCacheKey as renderSceneMaterialCacheKey} from '../src/lightusd/RenderSceneUtils.js';
import {LightUSDLoaderUtils} from '../src/lightusd/LightUSDLoaderUtils.js';

for (const slot of ['map', 'emissiveMap', 'specularColorMap', 'sheenColorMap', 'attenuationColorMap'])
  assert.equal(textureColorRole(slot), 'color', slot);
for (const slot of ['normalMap', 'roughnessMap', 'metalnessMap', 'aoMap', ''])
  assert.equal(textureColorRole(slot), 'data', slot);

const texture = {
  textureImageId: 7, wrapS: 'repeat', wrapT: 'clamp', hasTransform2d: true,
  txRotation: 0.25, txScaleU: 2, txScaleV: 3, txTranslationU: 0.1, txTranslationV: 0.2,
  isUDIM: true, udimTextureId: 9, udimUvScaleU: 1, udimUvScaleV: 1,
  udimUvOffsetU: 0, udimUvOffsetV: 0
};
const scene = {getTexture: id => id === 3 ? texture : null};
const cache = new Map();
const expectedSignature = {
  imageId: 7, wrapS: 'repeat', wrapT: 'clamp', hasTransform2d: true,
  txRotation: 0.25, txScaleU: 2, txScaleV: 3, txTranslationU: 0.1, txTranslationV: 0.2,
  isUDIM: true, udimTextureId: 9, udimUvScaleU: 1, udimUvScaleV: 1,
  udimUvOffsetU: 0, udimUvOffsetV: 0
};
assert.deepEqual(textureSignature(3, scene, cache), expectedSignature);
assert.deepEqual(textureSignature(3, scene, cache), expectedSignature, 'cache hit');
assert.equal(textureSignature(-1, scene), -1);
assert.equal(textureSignature(99, scene), 99, 'missing texture falls back to id');

assert.equal(textureCacheKey(3, scene, 'map', new Map()),
  `color:${JSON.stringify(expectedSignature)}`);
assert.equal(textureCacheKey(3, scene, 'normalMap', new Map()),
  `data:${JSON.stringify(expectedSignature)}`);
assert.equal(LightUSDLoaderUtils.textureCacheKey(3, scene, 'map'),
  textureCacheKey(3, scene, 'map'));
assert.deepEqual(LightUSDLoaderUtils.textureSignature(3, scene), expectedSignature);
assert.equal(LightUSDLoaderUtils.textureColorRole('map'), 'color');

const oldMaterialXKey = (textureId, texData) => JSON.stringify({
  textureId,
  imageId: texData?.textureImageId,
  wrapS: texData?.wrapS,
  wrapT: texData?.wrapT,
  hasTransform2d: !!texData?.hasTransform2d,
  txRotation: texData?.txRotation,
  txScaleU: texData?.txScaleU,
  txScaleV: texData?.txScaleV,
  txTranslationU: texData?.txTranslationU,
  txTranslationV: texData?.txTranslationV
});
assert.equal(textureDataCacheKey(3, texture), oldMaterialXKey(3, texture));
assert.equal(textureDataCacheKey(5), oldMaterialXKey(5));

const oldMaterialKey = entry => {
  if (Number.isFinite(entry?.materialId) && entry.materialId >= 0)
    return `id:${entry.materialId}`;
  if (entry?.materialKey) return `key:${entry.materialKey}`;
  return `fallback:${JSON.stringify(entry?.material || {})}:${JSON.stringify(entry?.texturePaths || {})}`;
};
for (const entry of [{materialId: 4}, {materialKey: 'k'}, {materialId: -1, materialKey: 'x'},
  {material: {color: 1}, texturePaths: {map: 'a'}}, {}]) {
  assert.equal(materialCacheKey(entry), oldMaterialKey(entry));
  assert.equal(renderSceneMaterialCacheKey(entry), oldMaterialKey(entry),
    'RenderSceneUtils re-exports the canonical key');
}

console.log('ok - canonical texture/material cache keys preserve all three contracts');
