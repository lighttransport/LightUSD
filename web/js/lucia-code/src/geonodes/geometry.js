import { normalizeIndexedMesh } from '../indexed-mesh.js';
import { LuciaError } from '../utils.js';

// Geometry value flowing between nodes. Every component is optional:
//   mesh      { positions: Float32Array, indices: Uint32Array }
//   points    { positions: Float32Array, normals: Float32Array|null }
//   instances [{ mesh, transforms: Float32Array (n * 16, column-major) }]
// Values are treated as immutable; nodes return new objects/arrays.
export function createGeometry({ mesh = null, points = null, instances = [] } = {}) {
  return { kind: 'geometry', mesh, points, instances };
}

export const EMPTY_GEOMETRY = Object.freeze(createGeometry());

export function isGeometry(value) {
  return value?.kind === 'geometry';
}

export function meshGeometry(positions, indices) {
  let normalized;
  try { normalized = normalizeIndexedMesh({ positions, indices }); }
  catch (error) { throw new LuciaError('LUCIA_GEONODES_MESH', `Invalid node mesh: ${error.message}`); }
  return createGeometry({ mesh: { positions: Float32Array.from(normalized.positions), indices: Uint32Array.from(normalized.indices) } });
}

export function geometryStats(geometry) {
  const mesh = geometry?.mesh;
  return {
    vertices: mesh ? mesh.positions.length / 3 : 0,
    triangles: mesh ? mesh.indices.length / 3 : 0,
    points: geometry?.points ? geometry.points.positions.length / 3 : 0,
    instances: (geometry?.instances || []).reduce((sum, group) => sum + group.transforms.length / 16, 0),
  };
}

export function joinMeshes(meshes) {
  const parts = meshes.filter(Boolean);
  if (!parts.length) return null;
  if (parts.length === 1) return parts[0];
  const vertexCount = parts.reduce((sum, mesh) => sum + mesh.positions.length, 0), indexCount = parts.reduce((sum, mesh) => sum + mesh.indices.length, 0);
  const positions = new Float32Array(vertexCount), indices = new Uint32Array(indexCount);
  let vertexOffset = 0, indexOffset = 0;
  for (const mesh of parts) {
    positions.set(mesh.positions, vertexOffset);
    const base = vertexOffset / 3;
    for (let i = 0; i < mesh.indices.length; i++) indices[indexOffset + i] = mesh.indices[i] + base;
    vertexOffset += mesh.positions.length; indexOffset += mesh.indices.length;
  }
  return { positions, indices };
}

export function transformPositions(positions, matrix) {
  const out = new Float32Array(positions.length), m = matrix;
  for (let i = 0; i < positions.length; i += 3) {
    const x = positions[i], y = positions[i + 1], z = positions[i + 2];
    out[i] = m[0] * x + m[4] * y + m[8] * z + m[12];
    out[i + 1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    out[i + 2] = m[2] * x + m[6] * y + m[10] * z + m[14];
  }
  return out;
}

export function composeTRS(translate = [0, 0, 0], rotateDegrees = [0, 0, 0], scale = [1, 1, 1]) {
  // XYZ Euler order, matching USD xformOp:rotateXYZ.
  const [rx, ry, rz] = rotateDegrees.map((value) => value * Math.PI / 180);
  const cx = Math.cos(rx), sx = Math.sin(rx), cy = Math.cos(ry), sy = Math.sin(ry), cz = Math.cos(rz), sz = Math.sin(rz);
  const r00 = cy * cz, r01 = sx * sy * cz - cx * sz, r02 = cx * sy * cz + sx * sz;
  const r10 = cy * sz, r11 = sx * sy * sz + cx * cz, r12 = cx * sy * sz - sx * cz;
  const r20 = -sy, r21 = sx * cy, r22 = cx * cy;
  const [kx, ky, kz] = scale;
  return new Float32Array([r00 * kx, r10 * kx, r20 * kx, 0, r01 * ky, r11 * ky, r21 * ky, 0, r02 * kz, r12 * kz, r22 * kz, 0, translate[0], translate[1], translate[2], 1]);
}

export function vertexNormals(positions, indices) {
  const normals = new Float32Array(positions.length);
  for (let f = 0; f < indices.length; f += 3) {
    const a = indices[f] * 3, b = indices[f + 1] * 3, c = indices[f + 2] * 3;
    const ux = positions[b] - positions[a], uy = positions[b + 1] - positions[a + 1], uz = positions[b + 2] - positions[a + 2];
    const vx = positions[c] - positions[a], vy = positions[c + 1] - positions[a + 1], vz = positions[c + 2] - positions[a + 2];
    // Unnormalized cross product == area weighting.
    const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    for (const v of [a, b, c]) { normals[v] += nx; normals[v + 1] += ny; normals[v + 2] += nz; }
  }
  for (let i = 0; i < normals.length; i += 3) {
    const length = Math.hypot(normals[i], normals[i + 1], normals[i + 2]);
    if (length > 0) { normals[i] /= length; normals[i + 1] /= length; normals[i + 2] /= length; }
    else normals[i + 2] = 1;
  }
  return normals;
}

// Deterministic PRNG for seeded nodes.
export function mulberry32(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6D2B79F5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// FNV-1a over typed-array bytes and JSON; used for cache keys and staleness stamps.
export function hashValue(value, seed = 0x811c9dc5) {
  let hash = seed >>> 0;
  const mix = (byte) => { hash ^= byte; hash = Math.imul(hash, 0x01000193) >>> 0; };
  const visit = (item) => {
    if (ArrayBuffer.isView(item)) {
      const bytes = new Uint8Array(item.buffer, item.byteOffset, item.byteLength);
      mix(item.constructor.name.length);
      for (let i = 0; i < bytes.length; i++) mix(bytes[i]);
    } else if (Array.isArray(item)) { mix(91); item.forEach(visit); mix(93); }
    else if (item && typeof item === 'object') { mix(123); for (const key of Object.keys(item).sort()) { if (typeof item[key] === 'function') continue; for (const ch of key) mix(ch.charCodeAt(0) & 255); visit(item[key]); } mix(125); }
    else for (const ch of String(item)) mix(ch.charCodeAt(0) & 255);
  };
  visit(value);
  return hash.toString(16).padStart(8, '0');
}
