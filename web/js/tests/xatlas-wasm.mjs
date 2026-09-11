import assert from 'node:assert/strict';
import createLightUSD from '../src/lightusd/lightusd.js';
import { transferXatlasMaterialGroups } from '../lucia-code/src/operations.js';

const module = await createLightUSD();
assert.equal(typeof module.XAtlasNative, 'function');
const native = new module.XAtlasNative();
try {
  const result = native.generate(
    new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    new Uint32Array([0, 1, 2]),
    { resolution: 64, padding: 2 },
  );
  assert.equal(result.error, undefined, result.error);
  assert.equal(result.positions.length, 9);
  assert.equal(result.uvs.length, 6);
  assert.equal(result.indices.length, 3);
  assert.equal(result.xref.length, 3);
  assert.deepEqual([...result.xref], [0, 1, 2]);
  assert.equal(Number.isInteger(result.chartCount), true);
  assert.equal(Number.isInteger(result.atlasCount), true);
  const repeat = native.generate(
    new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    new Uint32Array([0, 1, 2]),
    { resolution: 64, padding: 2, rotateChartsToAxis: false, rotateCharts: false, blockAlign: true },
  );
  assert.equal(repeat.error, undefined, repeat.error);
  const repeatAgain = native.generate(
    new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    new Uint32Array([0, 1, 2]),
    { resolution: 64, padding: 2, rotateChartsToAxis: false, rotateCharts: false, blockAlign: true },
  );
  assert.deepEqual([...repeat.positions], [...repeatAgain.positions]);
  assert.deepEqual([...repeat.uvs], [...repeatAgain.uvs]);
  assert.deepEqual([...repeat.indices], [...repeatAgain.indices]);
  const multi = native.generate(
    new Float32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]),
    new Uint32Array([0, 1, 2, 0, 2, 3, 4, 5, 6]),
    { resolution: 64, padding: 2 },
  );
  assert.equal(multi.error, undefined, multi.error);
  assert.equal(multi.indices.length, 9);
  for (let face = 0; face < 3; face++) {
    const output = [multi.xref[multi.indices[face * 3]], multi.xref[multi.indices[face * 3 + 1]], multi.xref[multi.indices[face * 3 + 2]]].sort((a, b) => a - b);
    const source = [0, 1, 2, 0, 2, 3, 4, 5, 6].slice(face * 3, face * 3 + 3).sort((a, b) => a - b);
    assert.deepEqual(output, source);
  }
  assert.deepEqual(transferXatlasMaterialGroups([{ start: 0, count: 6, materialIndex: 0 }, { start: 6, count: 3, materialIndex: 1 }], new Uint32Array([0, 1, 2, 0, 2, 3, 4, 5, 6]), multi.indices, multi.xref), [{ start: 0, count: 6, materialIndex: 0 }, { start: 6, count: 3, materialIndex: 1 }]);
} finally {
  native.delete();
}

const previousSelf = globalThis.self;
let workerMessage = null;
globalThis.self = { postMessage(message) { workerMessage = message; } };
try {
  await import(`../lucia-code/src/xatlas-worker.js?regression=${Date.now()}`);
  await globalThis.self.onmessage({ data: {
    type: 'unwrap',
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 4, 5]),
    options: { resolution: 64, texelsPerUnit: 64, padding: 2, singleAtlasFallback: true },
  } });
  assert.equal(workerMessage.type, 'result', workerMessage?.message);
  assert.equal(workerMessage.result.atlasCount, 1);
  assert.equal(workerMessage.result.singleAtlasFallback, true);
} finally {
  if (previousSelf === undefined) delete globalThis.self;
  else globalThis.self = previousSelf;
}
console.log('xatlas WASM mapping: PASS');
