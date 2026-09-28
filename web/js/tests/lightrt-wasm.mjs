import assert from 'node:assert/strict';
const {default: createLightUSD} = await import(process.env.LIGHTUSD_WASM64 === '1'
  ? '../src/lightusd/lightusd_next_64.js'
  : '../src/lightusd/lightusd_next.js');
import { raycastTriangles } from '../lucia-code/src/projection-bake.js';

const module = await createLightUSD();
const simplifier = new module.MeshoptSimplifier();
const simplifyPositions = new Float32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]);
const simplifyIndices = new Uint32Array([0, 1, 2, 0, 2, 3]);
const simplified = simplifier.simplify(simplifyPositions, simplifyIndices,
  new Float32Array(12), new Float32Array(8), new Uint8Array(4), 3, 1, 0);
assert.ok(simplified.indices instanceof Uint32Array);
assert.ok(simplified.indices.length >= 3 && simplified.indices.length <= 6);
assert.equal(simplified.sourceVertexCount, 4);
assert.equal(simplified.vertexCacheOptimized, true);
assert.equal(simplifier.simplify(simplifyPositions, simplifyIndices,
  null, null, null, 2, 1, 0), undefined);
simplifier.delete();
assert.throws(() => simplifier.simplify(simplifyPositions, simplifyIndices,
  null, null, null, 3, 1, 0), /already deleted/);
const previousRetopoSelf = globalThis.self;
let retopoMessage = null;
globalThis.self = {postMessage(message) { retopoMessage = message; }};
try {
  await import(`../lucia-code/src/retopo-worker.js?regression=${Date.now()}`);
  await globalThis.self.onmessage({data: {
    type: 'retopo', positions: simplifyPositions, indices: simplifyIndices,
    targetRatio: .5, targetError: 1, lockBorder: false, lockUVSeams: false
  }});
  assert.equal(retopoMessage?.type, 'result', retopoMessage?.message);
  assert.ok(retopoMessage.indices instanceof Uint32Array);
  assert.ok(retopoMessage.indices.length >= 3);
  retopoMessage = null;
  await globalThis.self.onmessage({data: {
    type: 'retopo', positions: simplifyPositions, indices: simplifyIndices,
    uvs: new Float32Array([0, 0, 1, 0, 1, 1, 0, 1]),
    targetRatio: .5, targetError: 1, lockBorder: false, lockUVSeams: true
  }});
  assert.equal(retopoMessage?.type, 'result', retopoMessage?.message);
  assert.ok(retopoMessage.uvs instanceof Float32Array);
} finally {
  if (previousRetopoSelf === undefined) delete globalThis.self;
  else globalThis.self = previousRetopoSelf;
}
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
assert.deepEqual([...gpu.normals], [...normals]);
assert.deepEqual([...gpu.colors], [...colors]);
assert.deepEqual([...gpu.materialIds], [0]);
assert.deepEqual([...gpu.materials], [...materials]);
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
assert.equal(tracer.build(positions, normals, colors, new Float32Array(11),
  new Int32Array([0]), materials), false);
assert.match(tracer.error(), /attribute array size mismatch/);
assert.equal(tracer.triangleCount(), 0);
assert.equal(tracer.build(new Float32Array(8), new Float32Array(8),
  new Float32Array(8), params, new Int32Array([0]), materials), false);
assert.match(tracer.error(), /positions must contain triangle soup/);
const staleHandle = module._lightusd_lrt_create();
assert.ok(staleHandle);
module._lightusd_lrt_destroy(staleHandle);
assert.equal(module._lightusd_lrt_triangle_count(staleHandle), -1);
tracer.clear();
assert.equal(tracer.triangleCount(), 0);
assert.equal(tracer.webGPUScene(), undefined);
tracer.delete();
assert.equal(tracer.isDeleted(), true);
assert.throws(() => tracer.error(), /already deleted/);

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
