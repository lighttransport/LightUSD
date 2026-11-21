import assert from 'node:assert/strict';

import createLightUSD from '../src/lightusd/lightusd_legacy.js';

const lightusd = await createLightUSD();
assert.equal(typeof lightusd.compressTextureToUni, 'function');
assert.equal(typeof lightusd.transcodeTextureUni, 'function');

const width = 8;
const height = 8;
const rgba = new Uint8Array(width * height * 4);
for (let y = 0; y < height; ++y) {
  for (let x = 0; x < width; ++x) {
    const i = (y * width + x) * 4;
    rgba[i + (y < height / 2 ? 0 : 2)] = 255;
    rgba[i + 3] = 255;
  }
}

const uni = lightusd.compressTextureToUni(rgba, width, height, true);
assert.equal(uni.success, true, uni.error);
assert.equal(uni.data.byteLength, 64);

for (const target of ['bc7', 'astc4x4', 'etc2rgba']) {
  const result = lightusd.transcodeTextureUni(uni.data, width, height, target);
  assert.equal(result.success, true, `${target}: ${result.error}`);
  assert.equal(result.data.byteLength, 64);
}

const decoded = lightusd.transcodeTextureUni(
  uni.data, width, height, 'rgba8');
assert.equal(decoded.success, true, decoded.error);
assert.equal(decoded.data.byteLength, rgba.byteLength);
// flipY=true makes the formerly-blue bottom row the first encoded row.
assert.ok(decoded.data[2] > decoded.data[0]);
const last = decoded.data.byteLength - 4;
assert.ok(decoded.data[last] > decoded.data[last + 2]);

const invalid = lightusd.compressTextureToUni(
  new Uint8Array(3), width, height, false);
assert.equal(invalid.success, false);
const unsupported = lightusd.transcodeTextureUni(
  uni.data, width, height, 'not-a-format');
assert.equal(unsupported.success, false);

console.log('texture compression WASM ABI: PASS');
