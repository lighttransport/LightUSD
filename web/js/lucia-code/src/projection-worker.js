import { buildProjectionRays } from './projection-bake.js';
import { expandIndexedAttribute, normalizeIndexedMesh } from './indexed-mesh.js';

let createLightUSD;

function sourceSoup({ positions, indices }) {
  return expandIndexedAttribute(positions, indices, 3);
}

function soupNormals(positions) {
  const normals = new Float32Array(positions.length);
  for (let offset = 0; offset < positions.length; offset += 9) {
    const ax = positions[offset + 3] - positions[offset], ay = positions[offset + 4] - positions[offset + 1], az = positions[offset + 5] - positions[offset + 2], bx = positions[offset + 6] - positions[offset], by = positions[offset + 7] - positions[offset + 1], bz = positions[offset + 8] - positions[offset + 2], nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx, length = Math.hypot(nx, ny, nz) || 1;
    for (let corner = 0; corner < 3; corner++) { normals[offset + corner * 3] = nx / length; normals[offset + corner * 3 + 1] = ny / length; normals[offset + corner * 3 + 2] = nz / length; }
  }
  return normals;
}

function vertexTransferRays(positions, normals, offset = 1e-6) {
  const count = positions.length / 3, origins = new Float32Array(count * 2 * 3), directions = new Float32Array(count * 2 * 3);
  for (let vertex = 0; vertex < count; vertex++) {
    const source = vertex * 3, length = Math.hypot(normals[source], normals[source + 1], normals[source + 2]); if (!Number.isFinite(length) || length <= 1e-12) continue;
    const nx = normals[source] / length, ny = normals[source + 1] / length, nz = normals[source + 2] / length;
    for (let side = 0; side < 2; side++) { const ray = (vertex * 2 + side) * 3, sign = side ? -1 : 1; origins[ray] = positions[source] + nx * offset * sign; origins[ray + 1] = positions[source + 1] + ny * offset * sign; origins[ray + 2] = positions[source + 2] + nz * offset * sign; directions[ray] = nx * sign; directions[ray + 1] = ny * sign; directions[ray + 2] = nz * sign; }
  }
  return { origins, directions };
}

function soupUVs(uvs, indices) {
  return expandIndexedAttribute(uvs, indices, 2);
}

function validateTransferInput(target, source) {
  if (!target?.positions || target.positions.length % 3 || !target.normals || target.normals.length !== target.positions.length || !source?.positions || source.positions.length % 3 || !source.indices || source.indices.length % 3 || !source.uvs || source.uvs.length !== source.positions.length / 3 * 2) throw new Error('UV transfer worker received incompatible buffers.');
  let targetMesh, sourceMesh;
  try { targetMesh = normalizeIndexedMesh({ positions: target.positions }); sourceMesh = normalizeIndexedMesh({ positions: source.positions, indices: source.indices }); } catch (error) { throw new Error(`UV transfer worker received invalid mesh buffers: ${error.message}`); }
  const finite = (values) => { for (const value of values) if (!Number.isFinite(value)) return false; return true; }, validIndices = (values, vertexCount) => { for (const value of values) if (!Number.isInteger(value) || value < 0 || value >= vertexCount) return false; return true; }, validNormals = (values) => { for (let i = 0; i < values.length; i += 3) if (Math.hypot(values[i], values[i + 1], values[i + 2]) <= 1e-12) return false; return true; };
  if (!finite(targetMesh.positions) || !finite(target.normals) || !validNormals(target.normals) || !finite(sourceMesh.positions) || !finite(source.uvs) || !validIndices(sourceMesh.indices, sourceMesh.positions.length / 3)) throw new Error('UV transfer worker received non-finite, zero-length, or out-of-range data.');
  return { target: { ...target, ...targetMesh }, source: { ...source, ...sourceMesh } };
}

self.onmessage = async ({ data }) => {
  if (data?.type !== 'project-rays' && data?.type !== 'transfer-uvs') return;
  try {
    createLightUSD ||= (await import('../../src/lightusd/lightusd.js')).default;
    if (data.type === 'transfer-uvs') {
      const normalized = validateTransferInput(data.target, data.source), targetData = normalized.target, sourceData = normalized.source, targetRays = vertexTransferRays(targetData.positions, targetData.normals, targetData.offset), module = await createLightUSD(), tracer = new module.LightRTPathTracer(), sourceSoupPositions = sourceSoup(sourceData), sourceNormals = soupNormals(sourceSoupPositions), sourceColors = new Float32Array(sourceSoupPositions.length).fill(1), params = new Float32Array(sourceData.indices.length / 3 * 12), materialIds = new Int32Array(sourceData.indices.length / 3), materials = new Float32Array(10);
      try {
        if (!tracer.build(sourceSoupPositions, sourceNormals, sourceColors, params, materialIds, materials)) throw new Error(tracer.error());
        const hits = targetRays.origins.length ? tracer.raycast(targetRays.origins, targetRays.directions, targetData.maxDistance) : { distance: new Float32Array(), triangle: new Int32Array(), barycentrics: new Float32Array() }, sourceUVSoup = soupUVs(sourceData.uvs, sourceData.indices), vertexCount = targetData.positions.length / 3, uvs = new Float32Array(vertexCount * 2), hitMask = new Uint8Array(vertexCount); let hitCount = 0;
        for (let vertex = 0; vertex < vertexCount; vertex++) { const first = vertex * 2, second = first + 1, firstHit = hits.triangle[first] >= 0, secondHit = hits.triangle[second] >= 0, ray = firstHit && secondHit ? (hits.distance[first] <= hits.distance[second] ? first : second) : firstHit ? first : second, face = hits.triangle[ray]; if (face < 0 || !Number.isFinite(hits.distance[ray])) continue; const barycentric = ray * 3, uv = face * 6, w = hits.barycentrics[barycentric], u = hits.barycentrics[barycentric + 1], v = hits.barycentrics[barycentric + 2]; uvs[vertex * 2] = w * sourceUVSoup[uv] + u * sourceUVSoup[uv + 2] + v * sourceUVSoup[uv + 4]; uvs[vertex * 2 + 1] = w * sourceUVSoup[uv + 1] + u * sourceUVSoup[uv + 3] + v * sourceUVSoup[uv + 5]; hitMask[vertex] = 1; hitCount++; }
        self.postMessage({ type: 'transfer-result', uvs, hitMask, hitCount, missCount: vertexCount - hitCount }, [uvs.buffer, hitMask.buffer]); return;
      } finally { tracer.delete(); }
    }
    const targetMesh = normalizeIndexedMesh({ positions: data.target.positions, indices: data.target.indices }), sourceMesh = normalizeIndexedMesh({ positions: data.source.positions, indices: data.source.indices }), target = buildProjectionRays({ ...data.target, ...targetMesh }), module = await createLightUSD(), tracer = new module.LightRTPathTracer(), sourcePositions = sourceSoup(sourceMesh), sourceNormals = soupNormals(sourcePositions), sourceColors = new Float32Array(sourcePositions.length).fill(1), params = new Float32Array(sourceMesh.indices.length / 3 * 12), materialIds = new Int32Array(sourceMesh.indices.length / 3), materials = new Float32Array(10);
    try {
      if (!tracer.build(sourcePositions, sourceNormals, sourceColors, params, materialIds, materials)) throw new Error(tracer.error());
      const hits = target.origins.length ? tracer.raycast(target.origins, target.directions, target.rayDistance) : { distance: new Float32Array(), triangle: new Int32Array(), barycentrics: new Float32Array() };
      const targetTriangles = target.targetTriangles;
      self.postMessage({ type: 'result', resolution: target.resolution, pixels: target.pixels, covered: target.covered, owners: target.owners, islandCount: target.islandCount, sourceTriangleCount: sourceMesh.indices.length / 3, targetTriangleCount: targetMesh.indices.length / 3, targetTriangle: targetTriangles, targetBarycentrics: target.barycentrics, ...hits }, [target.pixels.buffer, target.covered.buffer, target.owners.buffer, targetTriangles.buffer, target.barycentrics.buffer, hits.distance.buffer, hits.triangle.buffer, hits.barycentrics.buffer]);
    } finally { tracer.delete(); }
  } catch (error) { self.postMessage({ type: 'error', message: error?.message || String(error) }); }
};
