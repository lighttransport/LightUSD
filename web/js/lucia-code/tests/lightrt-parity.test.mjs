import test from 'node:test';
import assert from 'node:assert/strict';
import createLightUSD from '../../src/lightusd/lightusd.js';
import { bakeOcclusionCPU, buildOcclusionRays, rasterizeOcclusionSurface } from '../src/occlusion-bake.js';
import { buildProjectionRays, raycastTriangles, transferUVsByProjection } from '../src/projection-bake.js';

test('LightRT WASM occlusion bake matches the CPU reference', async () => {
  const positions = new Float32Array([-1, -1, 0, 1, -1, 0, 0, 1, 0]), indices = new Uint32Array([0, 1, 2]), uvs = new Float32Array([.02, .03, .97, .03, .5, .97]), normals = new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]), resolution = 32, samples = 3, radius = 2;
  const expected = bakeOcclusionCPU({ positions, indices, uvs, normals, resolution, samples, radius });
  const module = await createLightUSD(), tracer = new module.LightRTPathTracer(), triangles = indices.length / 3, soupPositions = new Float32Array(triangles * 9), soupNormals = new Float32Array(triangles * 9), soupColors = new Float32Array(triangles * 9).fill(1), params = new Float32Array(triangles * 12), materialIds = new Int32Array(triangles), materials = new Float32Array(10);
  for (let face = 0; face < triangles; face++) for (let corner = 0; corner < 3; corner++) { const source = indices[face * 3 + corner] * 3, target = face * 9 + corner * 3; soupPositions.set(positions.subarray(source, source + 3), target); soupNormals.set(normals.subarray(source, source + 3), target); }
  assert.equal(tracer.build(soupPositions, soupNormals, soupColors, params, materialIds, materials), true, tracer.error());
  try {
    const surface = rasterizeOcclusionSurface({ positions, indices, uvs, normals, resolution }), rays = buildOcclusionRays(surface, samples), visibility = tracer.occluded(rays.origins, rays.directions, radius), hits = new Uint16Array(resolution * resolution), pixels = new Uint8ClampedArray(resolution * resolution * 4);
    for (let index = 0; index < visibility.length; index++) hits[rays.pixelIds[index]] += visibility[index];
    for (let pixel = 0; pixel < surface.covered.length; pixel++) { const value = surface.covered[pixel] ? Math.round((1 - hits[pixel] / rays.samples) * 255) : 0, offset = pixel * 4; pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = value; pixels[offset + 3] = surface.covered[pixel] ? 255 : 0; }
    assert.deepEqual([...pixels], [...expected.pixels]);
    assert.equal(surface.covered.reduce((sum, value) => sum + value, 0), expected.covered);
    assert.equal(expected.missedTexels, expected.total - expected.covered);
  } finally { tracer.delete(); }
});

test('LightRT WASM occlusion texture worker matches the CPU bake', async () => {
  const input = {
    type: 'bake-occlusion',
    positions: new Float32Array([-1, -1, 0, 1, -1, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2]),
    uvs: new Float32Array([.02, .03, .97, .03, .5, .97]),
    normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]),
    resolution: 64,
    samples: 2,
    radius: 2,
  };
  const expected = bakeOcclusionCPU(input), previousSelf = globalThis.self;
  let actual;
  globalThis.self = { postMessage(message) { actual = message; } };
  try {
    await import(`../src/ao-bake-worker.js?light-rt-worker-parity=${Date.now()}`);
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

test('LightRT WASM projection worker matches the CPU closest-hit reference', async () => {
  const target = { positions: new Float32Array([-.8, -.8, 0, .8, -.8, 0, -.8, .8, 0, .8, .8, 0]), indices: new Uint32Array([0, 1, 2, 2, 1, 3]), uvs: new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]), normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]), resolution: 12, rayDistance: 2, cageOffset: .001 };
  const source = { positions: new Float32Array([-2, -2, -1, 2, -2, -1, 0, 2, -1]), indices: new Uint32Array([0, 1, 2]) }, expectedRays = buildProjectionRays(target), expectedHits = raycastTriangles({ origins: expectedRays.origins, directions: expectedRays.directions, positions: source.positions, indices: source.indices, maxDistance: target.rayDistance });
  const previousSelf = globalThis.self;
  let actual = null;
  globalThis.self = { postMessage(message) { actual = message; } };
  try {
    await import(`../src/projection-worker.js?wasm-parity=${Date.now()}`);
    await globalThis.self.onmessage({ data: { type: 'project-rays', target, source } });
  } finally {
    if (previousSelf === undefined) delete globalThis.self;
    else globalThis.self = previousSelf;
  }
  assert.equal(actual?.type, 'result', actual?.message);
  assert.deepEqual([...actual.triangle], [...expectedHits.triangle]);
  assert.deepEqual([...actual.targetTriangle], [...expectedRays.targetTriangles]);
  assert.deepEqual([...actual.covered], [...expectedRays.covered]);
  for (let index = 0; index < actual.distance.length; index++) assert.ok(expectedHits.triangle[index] < 0 && actual.triangle[index] < 0 || Math.abs(actual.distance[index] - expectedHits.distance[index]) < 1e-5);
  for (let index = 0; index < actual.barycentrics.length; index++) assert.ok(Math.abs(actual.barycentrics[index] - expectedHits.barycentrics[index]) < 1e-5);
});

test('LightRT WASM UV transfer worker matches the CPU projection reference', async () => {
  const target = { positions: new Float32Array([-.25, -.25, 0, .25, -.25, 0, 2, 2, 0]), normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]), maxDistance: 2, offset: .001 };
  const source = { positions: new Float32Array([-1, -1, -1, 1, -1, -1, 0, 1, -1]), indices: new Uint16Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, .5, 1]) };
  const expected = transferUVsByProjection({ targetPositions: target.positions, targetNormals: target.normals, sourcePositions: source.positions, sourceIndices: source.indices, sourceUVs: source.uvs, maxDistance: target.maxDistance, offset: target.offset });
  const previousSelf = globalThis.self;
  let actual = null;
  globalThis.self = { postMessage(message) { actual = message; } };
  try {
    await import(`../src/projection-worker.js?wasm-uv-transfer=${Date.now()}`);
    await globalThis.self.onmessage({ data: { type: 'transfer-uvs', target, source } });
  } finally {
    if (previousSelf === undefined) delete globalThis.self;
    else globalThis.self = previousSelf;
  }
  assert.equal(actual?.type, 'transfer-result', actual?.message);
  assert.deepEqual([...actual.hitMask], [...expected.hitMask]);
  assert.equal(actual.hitCount, expected.hitCount);
  assert.equal(actual.missCount, expected.missCount);
  for (let index = 0; index < actual.uvs.length; index++) assert.ok(Math.abs(actual.uvs[index] - expected.uvs[index]) < 1e-5);
});
