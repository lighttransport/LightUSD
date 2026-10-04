import { LuciaError } from '../utils.js';
import { registerNode, getNodeType, listNodeTypes } from './registry.js';
import { EMPTY_GEOMETRY, composeTRS, createGeometry, joinMeshes, meshGeometry, mulberry32, transformPositions, vertexNormals } from './geometry.js';

export { registerNode, getNodeType, listNodeTypes };

export const GEONODES_MAX_ELEMENTS = 2_000_000;

// ---------------------------------------------------------------------------
// Minimal field system: a field is evaluated per element with a context of
// { position: [x,y,z], normal: [x,y,z], index }.
export function isField(value) { return value?.kind === 'field'; }
export function makeField(type, evaluate) { return { kind: 'field', type, evaluate }; }
export function resolveField(value, ctx) { return isField(value) ? value.evaluate(ctx) : value; }
// Lift a pure function over constant-or-field arguments.
function lift(type, fn, ...args) {
  if (!args.some(isField)) return fn(...args);
  return makeField(type, (ctx) => fn(...args.map((arg) => resolveField(arg, ctx))));
}
function evaluateFieldOnMesh(field, mesh, normals = null) {
  const count = mesh.positions.length / 3, values = new Array(count), ctx = { position: [0, 0, 0], normal: [0, 0, 1], index: 0 };
  const n = normals || (isField(field) ? vertexNormals(mesh.positions, mesh.indices || new Uint32Array()) : null);
  for (let i = 0; i < count; i++) {
    ctx.position = [mesh.positions[i * 3], mesh.positions[i * 3 + 1], mesh.positions[i * 3 + 2]];
    if (n) ctx.normal = [n[i * 3], n[i * 3 + 1], n[i * 3 + 2]];
    ctx.index = i;
    values[i] = resolveField(field, ctx);
  }
  return values;
}

const vec = (value, fallback = [0, 0, 0]) => Array.isArray(value) && value.length === 3 ? value.map(Number) : typeof value === 'number' ? [value, value, value] : fallback;
const int = (value, min, max) => Math.min(max, Math.max(min, Math.round(Number(value) || 0)));
const geometryIn = (value) => value?.kind === 'geometry' ? value : EMPTY_GEOMETRY;
// Elements a geometry value holds (vertices + points + instances); untrusted
// graphs (stored in USD files) must not be able to grow this unboundedly.
const elementCount = (g) => (g.mesh ? g.mesh.positions.length / 3 : 0) + (g.points ? g.points.positions.length / 3 : 0) + g.instances.reduce((sum, group) => sum + group.transforms.length / 16, 0);
const checkBudget = (count, label) => { if (count > GEONODES_MAX_ELEMENTS) throw new LuciaError('LUCIA_GEONODES_BUDGET', `${label} would create ${count} elements (limit ${GEONODES_MAX_ELEMENTS}).`); };

// ---------------------------------------------------------------------------
// Group IO
registerNode('GroupInput', { label: 'Group Input', category: 'Input', outputs: [{ name: 'geometry', type: 'geometry' }], evaluate: (_inputs, ctx) => ({ geometry: ctx.groupInput || EMPTY_GEOMETRY }) });
// realizeInstances=false authors instances as a UsdGeomPointInstancer on commit.
registerNode('GroupOutput', { label: 'Group Output', category: 'Output', inputs: [{ name: 'geometry', type: 'geometry' }, { name: 'realizeInstances', type: 'bool', default: false }], evaluate: ({ geometry, realizeInstances }) => ({ geometry: geometryIn(geometry), realizeInstances: Boolean(realizeInstances) }) });
registerNode('Value', { label: 'Value', category: 'Input', inputs: [{ name: 'value', type: 'float', default: 1 }], outputs: [{ name: 'value', type: 'float' }], evaluate: ({ value }) => ({ value: Number(value) }) });
registerNode('Vector', { label: 'Vector', category: 'Input', inputs: [{ name: 'x', type: 'float', default: 0 }, { name: 'y', type: 'float', default: 0 }, { name: 'z', type: 'float', default: 0 }], outputs: [{ name: 'vector', type: 'vector' }], evaluate: ({ x, y, z }) => ({ vector: lift('vector', (a, b, c) => [a, b, c], x, y, z) }) });
registerNode('Position', { label: 'Position', category: 'Field', outputs: [{ name: 'position', type: 'vector' }], evaluate: () => ({ position: makeField('vector', (ctx) => ctx.position) }) });
registerNode('Normal', { label: 'Normal', category: 'Field', outputs: [{ name: 'normal', type: 'vector' }], evaluate: () => ({ normal: makeField('vector', (ctx) => ctx.normal) }) });
registerNode('Index', { label: 'Index', category: 'Field', outputs: [{ name: 'index', type: 'int' }], evaluate: () => ({ index: makeField('float', (ctx) => ctx.index) }) });

// ---------------------------------------------------------------------------
// Math
const MATH_OPS = { add: (a, b) => a + b, subtract: (a, b) => a - b, multiply: (a, b) => a * b, divide: (a, b) => b === 0 ? 0 : a / b, power: (a, b) => Math.pow(a, b), minimum: Math.min, maximum: Math.max, sine: (a) => Math.sin(a), cosine: (a) => Math.cos(a), absolute: (a) => Math.abs(a) };
registerNode('Math', { label: 'Math', category: 'Utility', inputs: [{ name: 'operation', type: 'enum', options: Object.keys(MATH_OPS), default: 'add' }, { name: 'a', type: 'float', default: 0, field: true }, { name: 'b', type: 'float', default: 0, field: true }], outputs: [{ name: 'value', type: 'float' }],
  evaluate: ({ operation, a, b }) => { const op = MATH_OPS[operation]; if (!op) throw new LuciaError('LUCIA_GEONODES_PARAM', `Unknown math operation: ${operation}`); return { value: lift('float', (x, y) => op(Number(x), Number(y)), a, b) }; } });
const VECTOR_OPS = {
  add: (a, b) => a.map((v, i) => v + b[i]), subtract: (a, b) => a.map((v, i) => v - b[i]), multiply: (a, b) => a.map((v, i) => v * b[i]),
  scale: (a, _b, s) => a.map((v) => v * s), normalize: (a) => { const l = Math.hypot(...a); return l > 0 ? a.map((v) => v / l) : [0, 0, 0]; },
  cross: (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]],
};
registerNode('VectorMath', { label: 'Vector Math', category: 'Utility', inputs: [{ name: 'operation', type: 'enum', options: Object.keys(VECTOR_OPS), default: 'add' }, { name: 'a', type: 'vector', default: [0, 0, 0], field: true }, { name: 'b', type: 'vector', default: [0, 0, 0], field: true }, { name: 'scale', type: 'float', default: 1, field: true }], outputs: [{ name: 'vector', type: 'vector' }],
  evaluate: ({ operation, a, b, scale }) => { const op = VECTOR_OPS[operation]; if (!op) throw new LuciaError('LUCIA_GEONODES_PARAM', `Unknown vector operation: ${operation}`); return { vector: lift('vector', (x, y, s) => op(vec(x), vec(y), Number(s)), a, b, scale) }; } });
registerNode('SeparateXYZ', { label: 'Separate XYZ', category: 'Utility', inputs: [{ name: 'vector', type: 'vector', default: [0, 0, 0], field: true }], outputs: [{ name: 'x', type: 'float' }, { name: 'y', type: 'float' }, { name: 'z', type: 'float' }],
  evaluate: ({ vector }) => ({ x: lift('float', (v) => vec(v)[0], vector), y: lift('float', (v) => vec(v)[1], vector), z: lift('float', (v) => vec(v)[2], vector) }) });

// 3D value noise in [-1, 1], deterministic for a given seed.
function latticeHash(x, y, z, seed) {
  let h = Math.imul(x, 374761393) ^ Math.imul(y, 668265263) ^ Math.imul(z, 2147483647) ^ Math.imul(seed, 144665);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967295 * 2 - 1;
}
export function valueNoise3(x, y, z, seed = 0) {
  const xi = Math.floor(x), yi = Math.floor(y), zi = Math.floor(z), fx = x - xi, fy = y - yi, fz = z - zi;
  const s = (t) => t * t * (3 - 2 * t), u = s(fx), v = s(fy), w = s(fz), lerp = (a, b, t) => a + (b - a) * t;
  const c = (dx, dy, dz) => latticeHash(xi + dx, yi + dy, zi + dz, seed);
  return lerp(lerp(lerp(c(0, 0, 0), c(1, 0, 0), u), lerp(c(0, 1, 0), c(1, 1, 0), u), v), lerp(lerp(c(0, 0, 1), c(1, 0, 1), u), lerp(c(0, 1, 1), c(1, 1, 1), u), v), w);
}
registerNode('NoiseTexture', { label: 'Noise Texture', category: 'Texture', inputs: [{ name: 'vector', type: 'vector', default: null, field: true }, { name: 'scale', type: 'float', default: 2 }, { name: 'seed', type: 'int', default: 0 }], outputs: [{ name: 'value', type: 'float' }],
  evaluate: ({ vector, scale, seed }) => ({ value: makeField('float', (ctx) => { const p = vec(resolveField(vector ?? ctx.position, ctx)); return valueNoise3(p[0] * scale, p[1] * scale, p[2] * scale, int(seed, 0, 1e9)); }) }) });

// ---------------------------------------------------------------------------
// Primitives
function gridMesh(sizeX, sizeY, vx, vy) {
  vx = int(vx, 2, 2048); vy = int(vy, 2, 2048); checkBudget(vx * vy, 'Grid');
  const positions = new Float32Array(vx * vy * 3), indices = new Uint32Array((vx - 1) * (vy - 1) * 6);
  for (let j = 0; j < vy; j++) for (let i = 0; i < vx; i++) { const o = (j * vx + i) * 3; positions[o] = (i / (vx - 1) - 0.5) * sizeX; positions[o + 1] = (j / (vy - 1) - 0.5) * sizeY; }
  let k = 0;
  for (let j = 0; j < vy - 1; j++) for (let i = 0; i < vx - 1; i++) { const a = j * vx + i, b = a + 1, c = a + vx, d = c + 1; indices.set([a, b, d, a, d, c], k); k += 6; }
  return meshGeometry(positions, indices);
}
registerNode('MeshGrid', { label: 'Grid', category: 'Mesh Primitives', inputs: [{ name: 'sizeX', type: 'float', default: 2 }, { name: 'sizeY', type: 'float', default: 2 }, { name: 'verticesX', type: 'int', default: 10 }, { name: 'verticesY', type: 'int', default: 10 }], outputs: [{ name: 'mesh', type: 'geometry' }],
  evaluate: ({ sizeX, sizeY, verticesX, verticesY }) => ({ mesh: gridMesh(Number(sizeX), Number(sizeY), verticesX, verticesY) }) });
registerNode('MeshCube', { label: 'Cube', category: 'Mesh Primitives', inputs: [{ name: 'size', type: 'vector', default: [1, 1, 1] }], outputs: [{ name: 'mesh', type: 'geometry' }],
  evaluate: ({ size }) => {
    const [sx, sy, sz] = vec(size, [1, 1, 1]).map((v) => v / 2), positions = new Float32Array(24 * 3), indices = new Uint32Array(36);
    // Four vertices per face so each face keeps flat normals when written to USD.
    const faces = [[[1, 0, 0], [0, 1, 0], [0, 0, 1]], [[-1, 0, 0], [0, 0, 1], [0, 1, 0]], [[0, 1, 0], [0, 0, 1], [1, 0, 0]], [[0, -1, 0], [1, 0, 0], [0, 0, 1]], [[0, 0, 1], [1, 0, 0], [0, 1, 0]], [[0, 0, -1], [0, 1, 0], [1, 0, 0]]];
    faces.forEach(([n, u, v], f) => {
      [[-1, -1], [1, -1], [1, 1], [-1, 1]].forEach(([a, b], c) => { const o = (f * 4 + c) * 3; for (let i = 0; i < 3; i++) positions[o + i] = (n[i] + u[i] * a + v[i] * b) * [sx, sy, sz][i]; });
      const base = f * 4; indices.set([base, base + 1, base + 2, base, base + 2, base + 3], f * 6);
    });
    return { mesh: meshGeometry(positions, indices) };
  } });
registerNode('MeshUVSphere', { label: 'UV Sphere', category: 'Mesh Primitives', inputs: [{ name: 'segments', type: 'int', default: 32 }, { name: 'rings', type: 'int', default: 16 }, { name: 'radius', type: 'float', default: 1 }], outputs: [{ name: 'mesh', type: 'geometry' }],
  evaluate: ({ segments, rings, radius }) => {
    const seg = int(segments, 3, 1024), ring = int(rings, 2, 1024), r = Number(radius); checkBudget(seg * ring, 'UV Sphere');
    const positions = [0, 0, r], indices = [];
    for (let j = 1; j < ring; j++) { const phi = Math.PI * j / ring; for (let i = 0; i < seg; i++) { const theta = 2 * Math.PI * i / seg; positions.push(r * Math.sin(phi) * Math.cos(theta), r * Math.sin(phi) * Math.sin(theta), r * Math.cos(phi)); } }
    const south = positions.length / 3; positions.push(0, 0, -r);
    const at = (j, i) => 1 + (j - 1) * seg + (i % seg);
    for (let i = 0; i < seg; i++) indices.push(0, at(1, i), at(1, i + 1));
    for (let j = 1; j < ring - 1; j++) for (let i = 0; i < seg; i++) indices.push(at(j, i), at(j + 1, i), at(j + 1, i + 1), at(j, i), at(j + 1, i + 1), at(j, i + 1));
    for (let i = 0; i < seg; i++) indices.push(south, at(ring - 1, i + 1), at(ring - 1, i));
    return { mesh: meshGeometry(new Float32Array(positions), new Uint32Array(indices)) };
  } });
registerNode('MeshCylinder', { label: 'Cylinder', category: 'Mesh Primitives', inputs: [{ name: 'vertices', type: 'int', default: 32 }, { name: 'radius', type: 'float', default: 1 }, { name: 'depth', type: 'float', default: 2 }], outputs: [{ name: 'mesh', type: 'geometry' }],
  evaluate: ({ vertices, radius, depth }) => {
    const n = int(vertices, 3, 4096), r = Number(radius), h = Number(depth) / 2, positions = [], indices = [];
    for (const z of [-h, h]) for (let i = 0; i < n; i++) { const t = 2 * Math.PI * i / n; positions.push(r * Math.cos(t), r * Math.sin(t), z); }
    const bottom = positions.length / 3; positions.push(0, 0, -h); const top = bottom + 1; positions.push(0, 0, h);
    for (let i = 0; i < n; i++) { const a = i, b = (i + 1) % n, c = a + n, d = b + n; indices.push(a, b, d, a, d, c, bottom, b, a, top, c, d); }
    return { mesh: meshGeometry(new Float32Array(positions), new Uint32Array(indices)) };
  } });

// ---------------------------------------------------------------------------
// Geometry operations
function mapMesh(geometry, fn) {
  const g = geometryIn(geometry);
  return createGeometry({ mesh: g.mesh ? fn(g.mesh) : null, points: g.points, instances: g.instances });
}
registerNode('Transform', { label: 'Transform Geometry', category: 'Geometry', inputs: [{ name: 'geometry', type: 'geometry' }, { name: 'translation', type: 'vector', default: [0, 0, 0] }, { name: 'rotation', type: 'vector', default: [0, 0, 0] }, { name: 'scale', type: 'vector', default: [1, 1, 1] }], outputs: [{ name: 'geometry', type: 'geometry' }],
  evaluate: ({ geometry, translation, rotation, scale }) => {
    const m = composeTRS(vec(translation), vec(rotation), vec(scale, [1, 1, 1])), g = geometryIn(geometry);
    return { geometry: createGeometry({
      mesh: g.mesh ? { positions: transformPositions(g.mesh.positions, m), indices: g.mesh.indices } : null,
      points: g.points ? { positions: transformPositions(g.points.positions, m), normals: g.points.normals && transformNormals(g.points.normals, m) } : null,
      instances: g.instances.map((group) => ({ mesh: group.mesh, transforms: multiplyTransforms(m, group.transforms) })),
    }) };
  } });
// Normals transform by the inverse-transpose, which is the cofactor matrix
// (row-major `cof`) up to 1/det,
// then renormalize; correct under non-uniform scale.
export function transformNormals(normals, m) {
  const a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
  const cof = [e * i - f * h, -(d * i - f * g), d * h - e * g, -(b * i - c * h), a * i - c * g, -(a * h - b * g), b * f - c * e, -(a * f - c * d), a * e - b * d];
  // cof = det * inverse-transpose; mirroring (det < 0) must not flip normals.
  const sign = a * cof[0] + b * cof[1] + c * cof[2] < 0 ? -1 : 1, out = new Float32Array(normals.length);
  for (let k = 0; k < normals.length; k += 3) {
    const x = normals[k], y = normals[k + 1], z = normals[k + 2];
    const nx = cof[0] * x + cof[1] * y + cof[2] * z, ny = cof[3] * x + cof[4] * y + cof[5] * z, nz = cof[6] * x + cof[7] * y + cof[8] * z, l = Math.hypot(nx, ny, nz) || 1;
    out[k] = sign * nx / l; out[k + 1] = sign * ny / l; out[k + 2] = sign * nz / l;
  }
  return out;
}
function multiplyTransforms(m, transforms) {
  const out = new Float32Array(transforms.length);
  for (let t = 0; t < transforms.length; t += 16) for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
    let sum = 0; for (let k = 0; k < 4; k++) sum += m[k * 4 + r] * transforms[t + c * 4 + k]; out[t + c * 4 + r] = sum;
  }
  return out;
}
registerNode('SetPosition', { label: 'Set Position', category: 'Geometry', inputs: [{ name: 'geometry', type: 'geometry' }, { name: 'position', type: 'vector', default: null, field: true }, { name: 'offset', type: 'vector', default: [0, 0, 0], field: true }], outputs: [{ name: 'geometry', type: 'geometry' }],
  evaluate: ({ geometry, position, offset }) => ({ geometry: mapMesh(geometry, (mesh) => {
    const positionValues = position == null ? null : evaluateFieldOnMesh(position, mesh), offsetValues = evaluateFieldOnMesh(offset, mesh), out = new Float32Array(mesh.positions.length);
    for (let i = 0; i < out.length / 3; i++) { const p = positionValues ? vec(positionValues[i]) : [mesh.positions[i * 3], mesh.positions[i * 3 + 1], mesh.positions[i * 3 + 2]], o = vec(offsetValues[i]); for (let k = 0; k < 3; k++) out[i * 3 + k] = p[k] + o[k]; }
    return { positions: out, indices: mesh.indices };
  }) }) });
registerNode('Displace', { label: 'Displace Along Normal', category: 'Geometry', inputs: [{ name: 'geometry', type: 'geometry' }, { name: 'height', type: 'float', default: 0, field: true }, { name: 'strength', type: 'float', default: 1 }], outputs: [{ name: 'geometry', type: 'geometry' }],
  evaluate: ({ geometry, height, strength }) => ({ geometry: mapMesh(geometry, (mesh) => {
    const normals = vertexNormals(mesh.positions, mesh.indices), heights = evaluateFieldOnMesh(height, mesh, normals), out = new Float32Array(mesh.positions);
    for (let i = 0; i < heights.length; i++) { const d = Number(heights[i]) * Number(strength); for (let k = 0; k < 3; k++) out[i * 3 + k] += normals[i * 3 + k] * d; }
    return { positions: out, indices: mesh.indices };
  }) }) });
export function subdivideMidpoint(mesh) {
  const positions = Array.from(mesh.positions), indices = [], midpoints = new Map();
  const mid = (a, b) => { const key = a < b ? a * 0x100000000 + b : b * 0x100000000 + a; let m = midpoints.get(key); if (m == null) { m = positions.length / 3; for (let k = 0; k < 3; k++) positions.push((positions[a * 3 + k] + positions[b * 3 + k]) / 2); midpoints.set(key, m); } return m; };
  for (let f = 0; f < mesh.indices.length; f += 3) { const a = mesh.indices[f], b = mesh.indices[f + 1], c = mesh.indices[f + 2], ab = mid(a, b), bc = mid(b, c), ca = mid(c, a); indices.push(a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca); }
  return { positions: new Float32Array(positions), indices: new Uint32Array(indices) };
}
registerNode('Subdivide', { label: 'Subdivide Mesh', category: 'Geometry', inputs: [{ name: 'geometry', type: 'geometry' }, { name: 'level', type: 'int', default: 1 }], outputs: [{ name: 'geometry', type: 'geometry' }],
  evaluate: ({ geometry, level }) => ({ geometry: mapMesh(geometry, (mesh) => { const levels = int(level, 0, 6); checkBudget(mesh.indices.length / 3 * 4 ** levels, 'Subdivide'); let out = mesh; for (let i = 0; i < levels; i++) out = subdivideMidpoint(out); return out; }) }) });
export function mergeByDistance(mesh, distance) {
  const d = Math.max(Number(distance) || 0, 0), count = mesh.positions.length / 3, remap = new Uint32Array(count), cell = d > 0 ? d : 1e-12, grid = new Map(), kept = [];
  const key = (x, y, z) => `${x},${y},${z}`;
  for (let i = 0; i < count; i++) {
    const x = mesh.positions[i * 3], y = mesh.positions[i * 3 + 1], z = mesh.positions[i * 3 + 2], cx = Math.floor(x / cell), cy = Math.floor(y / cell), cz = Math.floor(z / cell);
    let target = -1;
    for (let dx = -1; dx <= 1 && target < 0; dx++) for (let dy = -1; dy <= 1 && target < 0; dy++) for (let dz = -1; dz <= 1 && target < 0; dz++) for (const j of grid.get(key(cx + dx, cy + dy, cz + dz)) || []) {
      if (Math.hypot(kept[j * 3] - x, kept[j * 3 + 1] - y, kept[j * 3 + 2] - z) <= d) { target = j; break; }
    }
    if (target < 0) { target = kept.length / 3; kept.push(x, y, z); const k = key(cx, cy, cz); (grid.get(k) || grid.set(k, []).get(k)).push(target); }
    remap[i] = target;
  }
  const indices = [];
  for (let f = 0; f < mesh.indices.length; f += 3) { const a = remap[mesh.indices[f]], b = remap[mesh.indices[f + 1]], c = remap[mesh.indices[f + 2]]; if (a !== b && b !== c && a !== c) indices.push(a, b, c); }
  return { positions: new Float32Array(kept), indices: new Uint32Array(indices) };
}
registerNode('MergeByDistance', { label: 'Merge by Distance', category: 'Geometry', inputs: [{ name: 'geometry', type: 'geometry' }, { name: 'distance', type: 'float', default: 0.001 }], outputs: [{ name: 'geometry', type: 'geometry' }],
  evaluate: ({ geometry, distance }) => ({ geometry: mapMesh(geometry, (mesh) => mergeByDistance(mesh, distance)) }) });
registerNode('JoinGeometry', { label: 'Join Geometry', category: 'Geometry', inputs: [{ name: 'a', type: 'geometry' }, { name: 'b', type: 'geometry' }, { name: 'c', type: 'geometry' }], outputs: [{ name: 'geometry', type: 'geometry' }],
  evaluate: ({ a, b, c }) => {
    const parts = [a, b, c].map(geometryIn), pointParts = parts.map((g) => g.points).filter(Boolean);
    checkBudget(parts.reduce((sum, g) => sum + elementCount(g), 0), 'Join Geometry');
    const points = pointParts.length ? { positions: Float32Array.from(pointParts.flatMap((p) => Array.from(p.positions))), normals: pointParts.every((p) => p.normals) ? Float32Array.from(pointParts.flatMap((p) => Array.from(p.normals))) : null } : null;
    return { geometry: createGeometry({ mesh: joinMeshes(parts.map((g) => g.mesh)), points, instances: parts.flatMap((g) => g.instances) }) };
  } });

// ---------------------------------------------------------------------------
// Points & instancing
registerNode('DistributePointsOnFaces', { label: 'Distribute Points on Faces', category: 'Point', inputs: [{ name: 'mesh', type: 'geometry' }, { name: 'density', type: 'float', default: 10 }, { name: 'seed', type: 'int', default: 0 }], outputs: [{ name: 'points', type: 'geometry' }],
  evaluate: ({ mesh: input, density, seed }) => {
    const mesh = geometryIn(input).mesh;
    if (!mesh) return { points: EMPTY_GEOMETRY };
    const random = mulberry32(int(seed, 0, 2 ** 31)), p = mesh.positions, positions = [], normals = [], rate = Math.max(0, Number(density) || 0);
    for (let f = 0; f < mesh.indices.length; f += 3) {
      const a = mesh.indices[f] * 3, b = mesh.indices[f + 1] * 3, c = mesh.indices[f + 2] * 3;
      const ux = p[b] - p[a], uy = p[b + 1] - p[a + 1], uz = p[b + 2] - p[a + 2], vx = p[c] - p[a], vy = p[c + 1] - p[a + 1], vz = p[c + 2] - p[a + 2];
      const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, len = Math.hypot(nx, ny, nz), area = len / 2;
      const count = Math.floor(area * rate + random());
      checkBudget(positions.length / 3 + count, 'Distribute Points');
      for (let i = 0; i < count; i++) {
        let s = random(), t = random(); if (s + t > 1) { s = 1 - s; t = 1 - t; }
        positions.push(p[a] + ux * s + vx * t, p[a + 1] + uy * s + vy * t, p[a + 2] + uz * s + vz * t);
        normals.push(len ? nx / len : 0, len ? ny / len : 0, len ? nz / len : 1);
      }
    }
    return { points: createGeometry({ points: { positions: new Float32Array(positions), normals: new Float32Array(normals) } }) };
  } });
function alignZToNormal(n) {
  // Rotation taking +Z onto n (Rodrigues); returns 3x3 column-major.
  const [x, y, z] = n, c = z;
  if (c < -0.999999) return [1, 0, 0, 0, -1, 0, 0, 0, -1];
  const k = 1 / (1 + c);
  return [y * y * k + c, -x * y * k, -x, -x * y * k, x * x * k + c, -y, x, y, c];
}
registerNode('InstanceOnPoints', { label: 'Instance on Points', category: 'Instances', inputs: [{ name: 'points', type: 'geometry' }, { name: 'instance', type: 'geometry' }, { name: 'scale', type: 'vector', default: [1, 1, 1], field: true }, { name: 'alignToNormal', type: 'bool', default: true }], outputs: [{ name: 'instances', type: 'geometry' }],
  evaluate: ({ points: input, instance, scale, alignToNormal }) => {
    const points = geometryIn(input).points, source = realize(geometryIn(instance)).mesh;
    if (!points || !source) return { instances: EMPTY_GEOMETRY };
    const count = points.positions.length / 3; checkBudget(count * (source.positions.length / 3), 'Instance on Points');
    const transforms = new Float32Array(count * 16), ctx = { position: [0, 0, 0], normal: [0, 0, 1], index: 0 };
    for (let i = 0; i < count; i++) {
      ctx.position = [points.positions[i * 3], points.positions[i * 3 + 1], points.positions[i * 3 + 2]];
      ctx.normal = points.normals ? [points.normals[i * 3], points.normals[i * 3 + 1], points.normals[i * 3 + 2]] : [0, 0, 1];
      ctx.index = i;
      const s = vec(resolveField(scale, ctx), [1, 1, 1]), r = alignToNormal ? alignZToNormal(ctx.normal) : [1, 0, 0, 0, 1, 0, 0, 0, 1], o = i * 16;
      for (let col = 0; col < 3; col++) for (let row = 0; row < 3; row++) transforms[o + col * 4 + row] = r[col * 3 + row] * s[col];
      transforms.set([...ctx.position, 1], o + 12);
    }
    return { instances: createGeometry({ instances: [{ mesh: source, transforms }] }) };
  } });
export function realize(geometry) {
  const g = geometryIn(geometry);
  if (!g.instances.length) return g;
  checkBudget((g.mesh ? g.mesh.positions.length / 3 : 0) + g.instances.reduce((sum, group) => sum + group.transforms.length / 16 * group.mesh.positions.length / 3, 0), 'Realize Instances');
  const meshes = [g.mesh];
  for (const group of g.instances) for (let t = 0; t < group.transforms.length; t += 16) meshes.push({ positions: transformPositions(group.mesh.positions, group.transforms.subarray(t, t + 16)), indices: group.mesh.indices });
  return createGeometry({ mesh: joinMeshes(meshes), points: g.points });
}
registerNode('RealizeInstances', { label: 'Realize Instances', category: 'Instances', inputs: [{ name: 'geometry', type: 'geometry' }], outputs: [{ name: 'geometry', type: 'geometry' }], evaluate: ({ geometry }) => ({ geometry: realize(geometry) }) });
