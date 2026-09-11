import assert from 'node:assert/strict';
import test from 'node:test';
import { bakeOcclusionCPU } from '../src/occlusion-bake.js';

test('bake workers ignore malformed control messages', async () => {
  const previousSelf = globalThis.self;
  globalThis.self = { postMessage() { throw new Error('unexpected worker response'); } };
  try {
    await import(`../src/ao-bake-worker.js?malformed-control=${Date.now()}`);
    await globalThis.self.onmessage({ data: null });
    await import(`../src/bake-worker.js?malformed-control=${Date.now()}`);
    globalThis.self.onmessage({ data: null });
  } finally {
    if (previousSelf === undefined) delete globalThis.self;
    else globalThis.self = previousSelf;
  }
});

test('geometry workers ignore malformed control messages', async () => {
  const previousSelf = globalThis.self;
  globalThis.self = { postMessage() { throw new Error('unexpected worker response'); } };
  try {
    for (const [file, query] of [['normal-worker.js', 'normal-control'], ['tangent-worker.js', 'tangent-control'], ['cleanup-worker.js', 'cleanup-control'], ['projection-worker.js', 'projection-control']]) {
      await import(`../src/${file}?${query}=${Date.now()}`);
      await globalThis.self.onmessage({ data: null });
    }
  } finally {
    if (previousSelf === undefined) delete globalThis.self;
    else globalThis.self = previousSelf;
  }
});

test('WASM occlusion worker matches the shared CPU reference', async () => {
  const input = {
    type: 'bake-occlusion',
    positions: new Float32Array([-1, -1, 0, 1, -1, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2]),
    uvs: new Float32Array([0, 0, 1, 0, .5, 1]),
    normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]),
    resolution: 64,
    samples: 2,
    radius: 2,
  };
  const expected = bakeOcclusionCPU(input), previousSelf = globalThis.self;
  let actual;
  globalThis.self = { postMessage(message) { actual = message; } };
  try {
    await import(`../src/ao-bake-worker.js?cpu-parity=${Date.now()}`);
    await globalThis.self.onmessage({ data: input });
  } finally {
    if (previousSelf === undefined) delete globalThis.self;
    else globalThis.self = previousSelf;
  }
  assert.equal(actual?.type, 'result', actual?.message);
  assert.deepEqual([...actual.pixels], [...expected.pixels]);
  assert.equal(actual.covered, expected.covered);
  assert.equal(actual.total, expected.total);
  assert.equal(actual.missedTexels, expected.missedTexels);
  assert.equal(actual.coveredRatio, expected.coveredRatio);
});
