const finite = (value) => Number.isFinite(value);
import { Vector3 } from 'three';
import { ConvexHull } from 'three/addons/math/ConvexHull.js';
import { buildIndexedEdgeUses, forEachIndexedTriangle, INDEXED_MESH_MAX_INDICES, INDEXED_MESH_MAX_VERTICES } from './indexed-mesh.js';

function oversizedPhysicsReport(vertexCount, triangleCount) {
  return { valid: false, vertexCount, triangleCount, validTriangleCount: 0, components: 0, boundaryEdges: 0, nonManifoldEdges: 0, watertight: false, signedVolume: 0, volume: 0, centerOfMass: [0, 0, 0], inertiaDiagonal: [0, 0, 0], fits: null, convexHull: null, suitability: { dynamic: false, staticTriangleMesh: false } };
}

function pointsOf(positions) {
  const values = positions instanceof Float32Array || positions instanceof Float64Array ? positions : new Float32Array(positions || []);
  const points = [];
  for (let i = 0; i + 2 < values.length; i += 3) if ([values[i], values[i + 1], values[i + 2]].every(finite)) points.push([values[i], values[i + 1], values[i + 2]]);
  return points;
}

function fitError(points, distance, scale) {
  if (!points.length) return { mean: 0, max: 0, normalizedMean: 0, normalizedMax: 0 };
  let sum = 0, max = 0;
  for (const point of points) { const error = distance(point); sum += error; max = Math.max(max, error); }
  const mean = sum / points.length;
  return { mean, max, normalizedMean: mean / Math.max(scale, 1e-12), normalizedMax: max / Math.max(scale, 1e-12) };
}

function triangleMeshVolume(positions, indices) {
  let total = 0;
  forEachIndexedTriangle(indices, (face, ia, ib, ic) => {
    const a = ia * 3, b = ib * 3, c = ic * 3;
    if ([a, b, c].some((offset) => offset < 0 || offset + 2 >= positions.length)) return;
    total += (positions[a] * (positions[b + 1] * positions[c + 2] - positions[b + 2] * positions[c + 1]) - positions[a + 1] * (positions[b] * positions[c + 2] - positions[b + 2] * positions[c]) + positions[a + 2] * (positions[b] * positions[c + 1] - positions[b + 1] * positions[c])) / 6;
  });
  return Math.abs(total);
}

const HULL_SAMPLE_LIMIT = 4096;
function boundedHullPositions(position) {
  const vertexCount = Math.floor(position.length / 3);
  if (vertexCount <= HULL_SAMPLE_LIMIT) return { positions: position, sampled: false, sourceVertexCount: vertexCount };
  const selected = new Set();
  for (let axis = 0; axis < 3; axis++) {
    let min = 0, max = 0;
    for (let vertex = 1; vertex < vertexCount; vertex++) {
      if (position[vertex * 3 + axis] < position[min * 3 + axis]) min = vertex;
      if (position[vertex * 3 + axis] > position[max * 3 + axis]) max = vertex;
    }
    selected.add(min); selected.add(max);
  }
  for (let sample = 0; selected.size < HULL_SAMPLE_LIMIT && sample < HULL_SAMPLE_LIMIT * 2; sample++) selected.add(Math.min(vertexCount - 1, Math.floor(sample * vertexCount / (HULL_SAMPLE_LIMIT * 2))));
  const output = new Float32Array(selected.size * 3);
  [...selected].sort((a, b) => a - b).forEach((source, index) => output.set(position.subarray(source * 3, source * 3 + 3), index * 3));
  return { positions: output, sampled: true, sourceVertexCount: vertexCount };
}

export function fitPhysicsPrimitives(positions) {
  const points = pointsOf(positions); if (!points.length) return { box: null, sphere: null, capsule: null, cylinder: null };
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (const point of points) for (let axis = 0; axis < 3; axis++) { min[axis] = Math.min(min[axis], point[axis]); max[axis] = Math.max(max[axis], point[axis]); }
  const center = min.map((value, axis) => (value + max[axis]) / 2), size = max.map((value, axis) => value - min[axis]);
  const scale = Math.max(size[0], size[1], size[2], 1e-12), radius = Math.hypot(size[0], size[1], size[2]) / 2;
  const boxDistance = (point) => Math.max(min[0] - point[0], 0, point[0] - max[0], min[1] - point[1], 0, point[1] - max[1], min[2] - point[2], 0, point[2] - max[2]);
  let sphereRadius = 0;
  for (const point of points) sphereRadius = Math.max(sphereRadius, Math.hypot(point[0] - center[0], point[1] - center[1], point[2] - center[2]));
  const sphereDistance = (point) => Math.abs(Math.hypot(point[0] - center[0], point[1] - center[1], point[2] - center[2]) - sphereRadius);
  const axis = size.indexOf(Math.max(size[0], size[1], size[2])), other = [0, 1, 2].filter((value) => value !== axis), halfLength = size[axis] / 2, capsuleRadius = Math.min(Math.max(size[other[0]], size[other[1]]) / 2, halfLength), segmentHalf = Math.max(0, halfLength - capsuleRadius);
  const capsuleDistance = (point) => { const axial = Math.max(-segmentHalf, Math.min(segmentHalf, point[axis] - center[axis])), radial = Math.hypot(point[other[0]] - center[other[0]], point[other[1]] - center[other[1]]); return Math.abs(Math.hypot(radial, point[axis] - center[axis] - axial) - capsuleRadius); };
  let cylinderRadius = 0;
  for (const point of points) cylinderRadius = Math.max(cylinderRadius, Math.hypot(point[other[0]] - center[other[0]], point[other[1]] - center[other[1]]));
  const cylinderDistance = (point) => { const radial = Math.hypot(point[other[0]] - center[other[0]], point[other[1]] - center[other[1]]), axial = Math.max(Math.abs(point[axis] - center[axis]) - halfLength, 0); return Math.hypot(Math.max(radial - cylinderRadius, 0), axial); };
  return {
    box: { center, size, error: fitError(points, boxDistance, scale) },
    sphere: { center, radius: sphereRadius, error: fitError(points, sphereDistance, scale) },
    capsule: { axis, center, radius: capsuleRadius, halfHeight: segmentHalf, error: fitError(points, capsuleDistance, scale) },
    cylinder: { axis, center, radius: cylinderRadius, halfHeight: halfLength, error: fitError(points, cylinderDistance, scale) },
  };
}

export function generateConvexHull(positions) {
  const points = pointsOf(positions); if (points.length < 4) return { positions: new Float32Array(points.flat()), indices: new Uint32Array(), valid: false, reason: 'at-least-four-points-required' };
  const vectors = points.map((point) => new Vector3(...point)), hull = new ConvexHull().setFromPoints(vectors), indices = [], used = new Set();
  const indexFor = (point) => { let best = -1, distance = Infinity; for (let index = 0; index < vectors.length; index++) { const candidate = vectors[index].distanceToSquared(point); if (candidate < distance) { distance = candidate; best = index; } } return best; };
  for (const face of hull.faces) { const edge = face.edge, a = indexFor(edge.tail().point), b = indexFor(edge.head().point), c = indexFor(edge.next.head().point); if (a < 0 || b < 0 || c < 0 || a === b || b === c || a === c) continue; indices.push(a, b, c); used.add(a); used.add(b); used.add(c); }
  const remap = new Map([...used].sort((a, b) => a - b).map((source, target) => [source, target])), outputPositions = new Float32Array(remap.size * 3), outputIndices = new Uint32Array(indices.length);
  for (const [source, target] of remap) outputPositions.set(points[source], target * 3);
  indices.forEach((value, index) => { outputIndices[index] = remap.get(value); });
  return { positions: outputPositions, indices: outputIndices, valid: outputIndices.length >= 12, sourceVertexCount: points.length, hullVertexCount: remap.size, triangleCount: outputIndices.length / 3 };
}

export function analyzePhysicsMesh({ positions, indices } = {}) {
  const rawPositionLength = Number.isSafeInteger(positions?.length) ? positions.length : 0, rawVertexCount = rawPositionLength / 3, rawIndexCount = indices == null ? rawVertexCount : Number.isSafeInteger(indices?.length) ? indices.length : 0;
  if (!Number.isSafeInteger(rawVertexCount) || rawPositionLength % 3 || rawVertexCount > INDEXED_MESH_MAX_VERTICES || !Number.isSafeInteger(rawIndexCount) || rawIndexCount > INDEXED_MESH_MAX_INDICES) return oversizedPhysicsReport(Number.isSafeInteger(rawVertexCount) ? rawVertexCount : 0, Math.floor(rawIndexCount / 3));
  const position = positions instanceof Float32Array || positions instanceof Float64Array ? positions : new Float32Array(positions || []);
  const source = indices ? (indices instanceof Uint32Array || indices instanceof Uint16Array || indices instanceof Uint8Array ? indices : new Uint32Array(indices)) : Uint32Array.from({ length: Math.floor(position.length / 3) }, (_, index) => index);
  const faceCount = Math.floor(source.length / 3), vertexCount = Math.floor(position.length / 3), validFaces = [];
  let minX = Infinity, minY = Infinity, minZ = Infinity, maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (let i = 0; i + 2 < position.length; i += 3) { const p = [position[i], position[i + 1], position[i + 2]]; if (p.every(finite)) { minX = Math.min(minX, p[0]); minY = Math.min(minY, p[1]); minZ = Math.min(minZ, p[2]); maxX = Math.max(maxX, p[0]); maxY = Math.max(maxY, p[1]); maxZ = Math.max(maxZ, p[2]); } }
  forEachIndexedTriangle(source, (face, a, b, c) => {
    const ids = [a, b, c];
    if (!ids.every((id) => Number.isInteger(id) && id >= 0 && id * 3 + 2 < position.length)) return;
    if (ids.some((id) => ![position[id * 3], position[id * 3 + 1], position[id * 3 + 2]].every(finite))) return;
    validFaces.push(face);
  });
  const topology = buildIndexedEdgeUses(source, { vertexCount, accept: (_face, a, b, c) => [a, b, c].every((id) => Number.isInteger(id) && id >= 0 && id * 3 + 2 < position.length) && [a, b, c].every((id) => [position[id * 3], position[id * 3 + 1], position[id * 3 + 2]].every(finite)) }), edgeUses = topology.edges, adjacency = topology.faceAdjacency;
  const boundaryEdges = [...edgeUses.values()].filter((entries) => entries.length === 1).length, nonManifoldEdges = [...edgeUses.values()].filter((entries) => entries.length > 2).length;
  const components = [], visited = new Uint8Array(faceCount);
  for (const root of validFaces) if (!visited[root]) { const queue = [root]; visited[root] = 1; for (let head = 0; head < queue.length; head++) for (const neighbor of adjacency[queue[head]]) if (!visited[neighbor]) { visited[neighbor] = 1; queue.push(neighbor); } components.push(queue); }
  let signedVolume = 0, weightedCenter = [0, 0, 0];
  for (const face of validFaces) { const ids = [source[face * 3], source[face * 3 + 1], source[face * 3 + 2]], a = ids[0] * 3, b = ids[1] * 3, c = ids[2] * 3, cross = [position[b + 1] * position[c + 2] - position[b + 2] * position[c + 1], position[b + 2] * position[c] - position[b] * position[c + 2], position[b] * position[c + 1] - position[b + 1] * position[c]], volume = (position[a] * cross[0] + position[a + 1] * cross[1] + position[a + 2] * cross[2]) / 6; signedVolume += volume; for (let axis = 0; axis < 3; axis++) weightedCenter[axis] += volume * (position[a + axis] + position[b + axis] + position[c + axis]) / 4; }
  const volume = Math.abs(signedVolume), centerOfMass = volume > 1e-12 ? weightedCenter.map((value) => value / signedVolume) : [Number.isFinite(minX) ? (minX + maxX) / 2 : 0, Number.isFinite(minY) ? (minY + maxY) / 2 : 0, Number.isFinite(minZ) ? (minZ + maxZ) / 2 : 0];
  let convexHull = null;
  if (vertexCount >= 4) {
    try { const bounded = boundedHullPositions(position), hull = generateConvexHull(bounded.positions); if (hull.valid) { const hullVolume = triangleMeshVolume(hull.positions, hull.indices); convexHull = { vertexCount: hull.hullVertexCount, triangleCount: hull.triangleCount, volume: hullVolume, volumeRatio: volume > 1e-12 ? hullVolume / volume : null, volumeError: volume > 1e-12 ? Math.abs(hullVolume - volume) / volume : null, ...(bounded.sampled ? { approximate: true, sampledVertexCount: bounded.positions.length / 3, sourceVertexCount: bounded.sourceVertexCount } : {}) }; } } catch { convexHull = null; }
  }
  const size = [Math.max(0, maxX - minX), Math.max(0, maxY - minY), Math.max(0, maxZ - minZ)], mass = volume || 1, inertiaDiagonal = [mass * (size[1] ** 2 + size[2] ** 2) / 12, mass * (size[0] ** 2 + size[2] ** 2) / 12, mass * (size[0] ** 2 + size[1] ** 2) / 12];
  const watertight = validFaces.length > 0 && validFaces.length === faceCount && boundaryEdges === 0 && nonManifoldEdges === 0;
  return { valid: validFaces.length === faceCount && vertexCount > 0, vertexCount, triangleCount: faceCount, validTriangleCount: validFaces.length, components: components.length, boundaryEdges, nonManifoldEdges, watertight, signedVolume: finite(signedVolume) ? signedVolume : 0, volume: finite(volume) ? volume : 0, centerOfMass: centerOfMass.map((value) => finite(value) ? value : 0), inertiaDiagonal: inertiaDiagonal.map((value) => finite(value) ? value : 0), fits: fitPhysicsPrimitives(position), convexHull, suitability: { dynamic: watertight && volume > 1e-12, staticTriangleMesh: validFaces.length > 0 && nonManifoldEdges === 0 } };
}
