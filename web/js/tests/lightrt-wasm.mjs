import assert from 'node:assert/strict';
import createLightUSD from '../src/lightusd/lightusd_next.js';
import { raycastTriangles } from '../lucia-code/src/projection-bake.js';

const module = await createLightUSD();
assert.equal(typeof module.LightRTPathTracer, 'function');
const tracer = new module.LightRTPathTracer();
const positions = new Float32Array([-1, -1, 0, 1, -1, 0, 0, 1, 0]);
const normals = new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]);
const colors = new Float32Array(9).fill(1);
const params = new Float32Array([0, .5, 0, 0, 0, .5, 0, 0, 0, .5, 0, 0]);
const materials = new Float32Array([.8, .2, .1, 0, .5, 0, 0, 0, 0, 0]);
assert.equal(tracer.build(positions, normals, colors, params, new Int32Array([0]), materials), true, tracer.error());
assert.equal(tracer.triangleCount(), 1);
const gpu = tracer.webGPUScene();
assert.equal(gpu.width, 4);
assert.ok(gpu.blocks.length > 0);
const pixels = tracer.trace(new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
  new Float32Array([0, 0, 2]), 16, 16, 0, 1, 2, 1);
assert.equal(pixels.length, 16 * 16 * 4);
let min = Infinity, max = -Infinity;
for (let i = 0; i < pixels.length; i += 4) { min = Math.min(min, pixels[i]); max = Math.max(max, pixels[i]); }
assert.ok(max - min > 0.05, `expected non-uniform result, range=${max - min}`);
const rayOrigins = new Float32Array([0, 0, 1, 0, 0, -1]);
const rayDirections = new Float32Array([0, 0, -1, 0, 0, -1]);
const visibility = tracer.occluded(rayOrigins, rayDirections, 10);
assert.deepEqual([...visibility], [1, 0]);
const hits = tracer.raycast(new Float32Array([0, 0, 1, 2, 0, 1]), new Float32Array([0, 0, -1, 0, 0, -1]), 10);
assert.equal(hits.triangle[0], 0);
assert.equal(hits.triangle[1], -1);
assert.ok(Math.abs(hits.distance[0] - 1) < 1e-5);
assert.ok(Math.abs(hits.barycentrics[0] + hits.barycentrics[1] + hits.barycentrics[2] - 1) < 1e-5);
const reference = raycastTriangles({ origins: new Float32Array([0, 0, 1, 2, 0, 1]), directions: new Float32Array([0, 0, -1, 0, 0, -1]), positions, indices: new Uint32Array([0, 1, 2]), maxDistance: 10 });
assert.deepEqual([...hits.triangle], [...reference.triangle]);
assert.ok(Math.abs(hits.distance[0] - reference.distance[0]) < 1e-5);
for (let i = 0; i < hits.barycentrics.length; i++) assert.ok(Math.abs(hits.barycentrics[i] - reference.barycentrics[i]) < 1e-5);
tracer.delete();

const previousSelf = globalThis.self;
let workerMessage = null;
globalThis.self = { postMessage(message) { workerMessage = message; } };
try {
  await import(`../lucia-code/src/ao-bake-worker.js?regression=${Date.now()}`);
  const bakeInput = {
    type: 'bake-occlusion',
    positions: new Float32Array([-1, -1, 0, 1, -1, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2]),
    uvs: new Float32Array([0, 0, 1, 0, 0.5, 1]),
    normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]),
    resolution: 64,
    samples: 2,
    radius: 2,
  };
  await globalThis.self.onmessage({ data: bakeInput });
  assert.equal(workerMessage?.type, 'result', workerMessage?.message);
  const first = workerMessage;
  assert.equal(first.resolution, 64);
  assert.equal(first.pixels.length, 64 * 64 * 4);
  assert.equal(first.total, 64 * 64);
  assert.equal(first.covered + first.missedTexels, first.total);
  assert.ok(first.covered > 0);
  workerMessage = null;
  await globalThis.self.onmessage({ data: bakeInput });
  assert.equal(workerMessage?.type, 'result', workerMessage?.message);
  assert.deepEqual([...workerMessage.pixels], [...first.pixels]);
  assert.equal(workerMessage.covered, first.covered);
  assert.equal(workerMessage.missedTexels, first.missedTexels);
} finally {
  if (previousSelf === undefined) delete globalThis.self;
  else globalThis.self = previousSelf;
}

const projectionPreviousSelf = globalThis.self;
let projectionMessage = null;
globalThis.self = { postMessage(message) { projectionMessage = message; } };
try {
  await import(`../lucia-code/src/projection-worker.js?regression=${Date.now()}`);
  const target = {
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0]),
    indices: new Uint32Array([0, 1, 2, 2, 1, 3]),
    uvs: new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]),
    normals: new Float32Array(12).fill(0).map((value, index) => index % 3 === 2 ? 1 : value),
    resolution: 8,
    rayDistance: 2,
    cageOffset: .001,
  };
  const source = { positions: new Float32Array([-2, -2, -1, 2, -2, -1, 0, 2, -1]), indices: new Uint32Array([0, 1, 2]) };
  await globalThis.self.onmessage({ data: { type: 'project-rays', target, source } });
  assert.equal(projectionMessage?.type, 'result', projectionMessage?.message);
  assert.equal(projectionMessage.islandCount, 1);
  assert.deepEqual([...new Set(projectionMessage.targetTriangle)], [0, 1]);
  assert.deepEqual([...new Set([...projectionMessage.owners].filter((owner) => owner >= 0))], [0]);
  assert.ok([...projectionMessage.triangle].some((face) => face === 0));
  assert.ok([...projectionMessage.triangle].every((face) => face === -1 || face === 0));
} finally {
  if (projectionPreviousSelf === undefined) delete globalThis.self;
  else globalThis.self = projectionPreviousSelf;
}
console.log('LightRT WASM path tracer: PASS');
