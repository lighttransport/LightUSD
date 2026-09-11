import { forEachIndexedTriangle, validateIndexedMesh } from './indexed-mesh.js';
import { MAX_SHARP_EDGES } from './retopo-locks.js';

export function validateSharpEdges(edges = [], vertexCount = Infinity) {
  if (!Array.isArray(edges) || edges.length > MAX_SHARP_EDGES) throw new Error(`Sharp edges must be an array of at most ${MAX_SHARP_EDGES.toLocaleString()} vertex pairs.`);
  const unique = new Map();
  for (const edge of edges) {
    if (!Array.isArray(edge) || edge.length !== 2 || !edge.every((value) => Number.isSafeInteger(value)) || edge[0] < 0 || edge[1] < 0 || edge[0] >= vertexCount || edge[1] >= vertexCount || edge[0] === edge[1]) throw new Error('Sharp edges must contain distinct, in-range vertex pairs.');
    const key = edge[0] < edge[1] ? `${edge[0]}:${edge[1]}` : `${edge[1]}:${edge[0]}`;
    unique.set(key, key.split(':').map(Number));
  }
  return [...unique.values()];
}

export function recomputeVertexNormals({ positions, indices, groups = [], sharpEdges = [], weighting = 'area', smoothingAngle = 180, preserveMaterialBoundaries = false }) {
  if (!['area', 'angle'].includes(weighting)) throw new Error('Normal weighting must be area or angle.');
  if (typeof smoothingAngle !== 'number' || !Number.isFinite(smoothingAngle) || smoothingAngle < 0 || smoothingAngle > 180) throw new Error('Normal smoothingAngle must be a finite number between 0 and 180 degrees.');
  const normalized = validateIndexedMesh({ positions, indices }), vertexCount = normalized.vertexCount, triangles = normalized.indices, faces = [], incident = Array.from({ length: vertexCount }, () => []);
  const angleLimit = smoothingAngle, cosLimit = Math.cos(angleLimit * Math.PI / 180);
  const sharp = new Set(validateSharpEdges(sharpEdges || [], vertexCount).map(([a, b]) => `${a}:${b}`));
  const edgeIsSharp = (a, b) => sharp.has(a < b ? `${a}:${b}` : `${b}:${a}`);
  const materialForFace = (faceIndex) => { const offset = faceIndex * 3, group = groups.find((candidate) => offset >= candidate.start && offset < candidate.start + candidate.count); return group?.materialIndex ?? 0; };
  forEachIndexedTriangle(triangles, (face, ia, ib, ic) => {
    if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount || ia === ib || ib === ic || ia === ic) return;
    const ax = positions[ib * 3] - positions[ia * 3], ay = positions[ib * 3 + 1] - positions[ia * 3 + 1], az = positions[ib * 3 + 2] - positions[ia * 3 + 2];
    const bx = positions[ic * 3] - positions[ia * 3], by = positions[ic * 3 + 1] - positions[ia * 3 + 1], bz = positions[ic * 3 + 2] - positions[ia * 3 + 2];
    const nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx, area = Math.hypot(nx, ny, nz);
    if (!Number.isFinite(area) || area <= 1e-10) return;
    const normal = [nx / area, ny / area, nz / area], vectors = [[ax, ay, az], [bx - ax, by - ay, bz - az], [-bx, -by, -bz]];
    const corners = [[vectors[0], vectors[1]], [[-ax, -ay, -az], vectors[2]], [[-bx, -by, -bz], [ax - bx, ay - by, az - bz]]];
    const weights = corners.map(([u, v]) => Math.acos(Math.max(-1, Math.min(1, (u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) / (Math.hypot(...u) * Math.hypot(...v))))));
    const faceData = { indices: [ia, ib, ic], normal, area, weights, materialIndex: materialForFace(face) }, faceIndex = faces.push(faceData) - 1;
    for (let corner = 0; corner < 3; corner++) incident[faceData.indices[corner]].push({ face: faceIndex, weight: weighting === 'angle' ? weights[corner] : area });
  });
  const normals = new Float32Array(vertexCount * 3);
  for (let vertex = 0; vertex < vertexCount; vertex++) {
    const entries = incident[vertex]; if (!entries.length) { normals[vertex * 3 + 1] = 1; continue; }
    let rx = 0, ry = 0, rz = 0;
    const referenceFace = faces[entries[0].face], referenceMaterial = referenceFace.materialIndex, crossesSharpEdge = (face) => { if (face === referenceFace) return false; const neighbors = referenceFace.indices.filter((index) => index !== vertex && face.indices.includes(index)); if (!neighbors.length) return true; return sharp.size > 0 && neighbors.some((neighbor) => edgeIsSharp(vertex, neighbor)); };
    for (const entry of entries) { const face = faces[entry.face]; if (crossesSharpEdge(face) || preserveMaterialBoundaries && face.materialIndex !== referenceMaterial) continue; rx += face.normal[0] * face.area; ry += face.normal[1] * face.area; rz += face.normal[2] * face.area; }
    const referenceLength = Math.hypot(rx, ry, rz); if (referenceLength > 1e-20) { rx /= referenceLength; ry /= referenceLength; rz /= referenceLength; }
    let nx = 0, ny = 0, nz = 0, total = 0;
    for (const entry of entries) { const face = faces[entry.face]; if (crossesSharpEdge(face) || preserveMaterialBoundaries && face.materialIndex !== referenceMaterial) continue; const dot = face.normal[0] * rx + face.normal[1] * ry + face.normal[2] * rz; if (dot < cosLimit && entries.length > 1) continue; const weight = weighting === 'angle' ? entry.weight : face.area; nx += face.normal[0] * weight; ny += face.normal[1] * weight; nz += face.normal[2] * weight; total += weight; }
    if (!total) { const face = faces[entries[0].face]; nx = face.normal[0]; ny = face.normal[1]; nz = face.normal[2]; }
    const length = Math.hypot(nx, ny, nz), offset = vertex * 3;
    if (length > 1e-20 && Number.isFinite(length)) { normals[offset] = nx / length; normals[offset + 1] = ny / length; normals[offset + 2] = nz / length; } else normals[offset + 1] = 1;
  }
  return normals;
}

// Face-varying normals intentionally use one normalized face normal per corner.
// This is the conservative representation for USD meshes whose normal seams
// must remain independent from the position topology.
export function recomputeFaceVaryingNormals({ positions, indices }) {
  const normalized = validateIndexedMesh({ positions, indices }), vertexCount = normalized.vertexCount, triangles = normalized.indices, normals = new Float32Array(triangles.length * 3);
  forEachIndexedTriangle(triangles, (face, ia, ib, ic) => {
    const ax = positions[ib * 3] - positions[ia * 3], ay = positions[ib * 3 + 1] - positions[ia * 3 + 1], az = positions[ib * 3 + 2] - positions[ia * 3 + 2], bx = positions[ic * 3] - positions[ia * 3], by = positions[ic * 3 + 1] - positions[ia * 3 + 1], bz = positions[ic * 3 + 2] - positions[ia * 3 + 2], nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx, length = Math.hypot(nx, ny, nz), offset = face * 9;
    const value = Number.isFinite(length) && length > 1e-20 ? [nx / length, ny / length, nz / length] : [0, 1, 0];
    for (let corner = 0; corner < 3; corner++) normals.set(value, offset + corner * 3);
  });
  return normals;
}
