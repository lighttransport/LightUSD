// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {loadWasm, unpackUSDZ} from '../src/usdzconvert.js';
const suffix = process.env.LIGHTUSD_WASM64 === '1' ? '_64' : '';
const native = await loadWasm(() => import(`../src/lightusd/lightusd${suffix}.js`), {
  locateFile: file => new URL('../src/lightusd/' + file, import.meta.url).pathname,
});
const source = fs.readFileSync(new URL('../../../tests/usda/geometry-optimize-nonuniform-normal-001.usda', import.meta.url));
const converter = new native.NextUSDZConverterNative();
try {
  assert.equal(converter.loadFromBinary(new Uint8Array(source), 'scene.usda'), true, converter.error());
  for (const rootLayerFormat of ['usda', 'usdc']) {
    const bytes = converter.exportAsUSDZWithOptions({rootLayerFormat, optimizeGeometry: 'mergeMeshes'});
    assert.ok(bytes?.length, converter.error());
    const reader = new native.NextUSDZConverterNative();
    try {
      assert.equal(reader.loadFromBinary(bytes, 'result.usdz'), true, reader.error());
      const text = reader.exportAsUSDA();
      assert.equal((text.match(/def Mesh /g) || []).length, 1, text);
      assert.match(text, /Merged_0/);
      assert.match(text, /0\.447213/); // Inverse transpose, not the original 0.707 normal.
      assert.ok([...unpackUSDZ(bytes).entries.keys()].some(name => name.endsWith('.' + rootLayerFormat)));
    } finally { reader.delete(); }
  }
  assert.equal((converter.exportAsUSDA().match(/def Mesh /g) || []).length, 2,
    'configured exports must not mutate their source stage');
  assert.equal(converter.exportAsUSDZWithOptions({optimizeGeometry:'mergeMeshes', meshMergeMinGroupSize:1}), null);
  assert.match(converter.error(), /at least two/);
  for (const value of [0, -1, 1.5, '2', true, 2147483648]) {
    assert.equal(converter.exportAsUSDZWithOptions({optimizeGeometry:'mergeMeshes', meshMergeMaxInputFaces:value}), null);
    assert.match(converter.error(), /integers/);
  }
  console.log('PASS: shared core geometry merging and validated WASM export limits');
} finally { converter.delete(); }
