import test from 'node:test';
import assert from 'node:assert/strict';
import { diagnoseUSD, repairUSDMetadata, validateUSDZArchive } from '../src/usd-doctor.js';
import { cleanupMesh } from '../src/mesh-cleanup.js';
import { extractConnectedComponents } from '../src/mesh-components.js';
import { normalizeIndexedMesh } from '../src/indexed-mesh.js';
import { recomputeVertexNormals } from '../src/normal-recompute.js';
import { recomputeVertexTangents } from '../src/tangent-recompute.js';
import { analyzePhysicsMesh } from '../src/physics-analysis.js';
import { projectUVs } from '../src/uv-projection.js';

// This is intentionally deterministic so it can run in the normal Node gate.
// It exercises malformed boundaries rather than asserting that arbitrary USD
// is valid; parser errors are expected, incidental JS/runtime failures are not.
const next = (state) => (Math.imul(state, 1664525) + 1013904223) >>> 0;
const bytesFor = (seed, length) => {
  const bytes = new Uint8Array(length);
  let state = seed;
  for (let i = 0; i < bytes.length; i++) { state = next(state); bytes[i] = state & 255; }
  return bytes;
};
const arrayFor = (seed, length, invalid = false) => {
  const values = new Float32Array(length);
  let state = seed;
  for (let i = 0; i < values.length; i++) { state = next(state); values[i] = invalid && i % 7 === 0 ? Number.NaN : ((state % 200) - 100) / 10; }
  return values;
};

function invokeSafely(fn) {
  try {
    fn();
  } catch (error) {
    assert.ok(error instanceof Error, 'entry points must reject with Error objects');
  }
}

test('public USD and geometry entry points survive deterministic malformed-input fuzz smoke', () => {
  for (let iteration = 0; iteration < 160; iteration++) {
    const seed = 0x51f15e + iteration * 97;
    const text = new TextDecoder().decode(bytesFor(seed, iteration % 96));
    invokeSafely(() => diagnoseUSD(text, { [`asset-${iteration}`]: bytesFor(seed + 1, iteration % 32) }));
    invokeSafely(() => repairUSDMetadata(text, { defaultPrim: iteration % 3 ? 'World' : 17 }));
    invokeSafely(() => validateUSDZArchive(bytesFor(seed + 2, iteration % 128)));

    const vertexCount = iteration % 13;
    const positions = arrayFor(seed + 3, vertexCount * 3, iteration % 5 === 0);
    const indices = iteration % 4 === 0 ? null : Uint32Array.from(bytesFor(seed + 4, (iteration % 11) * 3));
    const mesh = { positions, indices };
    invokeSafely(() => normalizeIndexedMesh(mesh));
    invokeSafely(() => cleanupMesh(mesh));
    invokeSafely(() => extractConnectedComponents(mesh));
    invokeSafely(() => analyzePhysicsMesh(mesh));
    invokeSafely(() => projectUVs({ positions, mode: ['planar', 'box', 'cylindrical', 'spherical', 'unknown'][iteration % 5] }));
    invokeSafely(() => recomputeVertexNormals(mesh));
    invokeSafely(() => recomputeVertexTangents({ ...mesh, uvs: arrayFor(seed + 5, vertexCount * 2) }));
  }
});
