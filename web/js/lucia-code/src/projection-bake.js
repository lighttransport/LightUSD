import { convertBakeColor } from './texture-bake.js';
import { hasInvalidValue, validateIndexedMesh } from './indexed-mesh.js';

const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';

export function buildUVIslandOwners(indices) {
  if (!isIterableBuffer(indices) || indices.length % 3) throw new Error('UV-island ownership requires an iterable index buffer with complete triplets.');
  const faceCount = indices.length / 3, parent = Int32Array.from({ length: faceCount }, (_, face) => face), firstFaceByVertex = new Map();
  const find = (face) => { let root = face; while (parent[root] !== root) root = parent[root]; while (parent[face] !== face) { const next = parent[face]; parent[face] = root; face = next; } return root; };
  const unite = (a, b) => { const left = find(a), right = find(b); if (left === right) return; if (left < right) parent[right] = left; else parent[left] = right; };
  for (let face = 0; face < faceCount; face++) for (let corner = 0; corner < 3; corner++) {
    const vertex = indices[face * 3 + corner];
    if (!Number.isInteger(vertex) || vertex < 0) continue;
    const previous = firstFaceByVertex.get(vertex);
    if (previous == null) firstFaceByVertex.set(vertex, face);
    else unite(face, previous);
  }
  const labels = new Int32Array(faceCount), labelByRoot = new Map(); let nextLabel = 0;
  for (let face = 0; face < faceCount; face++) { const root = find(face); if (!labelByRoot.has(root)) labelByRoot.set(root, nextLabel++); labels[face] = labelByRoot.get(root); }
  return { labels, islandCount: nextLabel };
}

export function projectRayHitAttributes({ triangle, barycentrics, sourceIndices, sourceValues, faceValues = null, itemSize = 3, missValue = 0 } = {}) {
  if (!isIterableBuffer(triangle) || !isIterableBuffer(barycentrics) || !isIterableBuffer(sourceIndices) || !isIterableBuffer(sourceValues) || faceValues != null && !isIterableBuffer(faceValues) || !Number.isInteger(itemSize) || itemSize < 1 || itemSize > 4 || barycentrics.length !== triangle.length * 3 || sourceIndices.length % 3 || sourceValues.length % itemSize || faceValues && faceValues.length % itemSize) throw new Error('Projection buffers have incompatible shapes.');
  const sourceVertexCount = sourceValues.length / itemSize, output = new Float32Array(triangle.length * itemSize).fill(Number(missValue) || 0); let hitCount = 0;
  for (let ray = 0; ray < triangle.length; ray++) {
    const face = triangle[ray], w = barycentrics[ray * 3], u = barycentrics[ray * 3 + 1], v = barycentrics[ray * 3 + 2];
    if (!Number.isInteger(face) || face < 0 || face * 3 + 2 >= sourceIndices.length || ![w, u, v].every(Number.isFinite) || Math.abs(w + u + v - 1) > 1e-4) continue;
    const a = sourceIndices[face * 3], b = sourceIndices[face * 3 + 1], c = sourceIndices[face * 3 + 2];
    if (![a, b, c].every((index) => Number.isInteger(index) && index >= 0 && index < sourceVertexCount)) continue;
    for (let component = 0; component < itemSize; component++) output[ray * itemSize + component] = faceValues && face * itemSize + component < faceValues.length ? faceValues[face * itemSize + component] : w * sourceValues[a * itemSize + component] + u * sourceValues[b * itemSize + component] + v * sourceValues[c * itemSize + component];
    hitCount++;
  }
  return { values: output, hitCount, missCount: triangle.length - hitCount };
}

// Deterministic CPU reference for the LightRT closest-hit ABI.  Keep this
// intentionally small and allocation-free so it can also serve as the native
// parity oracle for worker implementations.
export function raycastTriangles({ origins, directions, positions, indices, maxDistance = Infinity } = {}) {
  if (!origins || !directions || origins.length !== directions.length || origins.length % 3 || !positions || positions.length % 3 || !indices || indices.length % 3) throw new Error('Raycast buffers have incompatible shapes.');
  try { validateIndexedMesh({ positions, indices }); } catch (error) { throw new Error(`Raycast mesh buffers are invalid: ${error.message}`); }
  const output = { distance: new Float32Array(origins.length / 3), triangle: new Int32Array(origins.length / 3), barycentrics: new Float32Array(origins.length) };
  output.distance.fill(Infinity); output.triangle.fill(-1);
  const limit = Number.isFinite(Number(maxDistance)) ? Math.max(0, Number(maxDistance)) : Infinity;
  for (let ray = 0; ray < output.triangle.length; ray++) {
    const ro = ray * 3, ox = origins[ro], oy = origins[ro + 1], oz = origins[ro + 2], dx = directions[ro], dy = directions[ro + 1], dz = directions[ro + 2];
    if (![ox, oy, oz, dx, dy, dz].every(Number.isFinite)) continue;
    let best = limit, bestFace = -1, bestU = 0, bestV = 0;
    for (let face = 0; face < indices.length / 3; face++) {
      const ia = indices[face * 3] * 3, ib = indices[face * 3 + 1] * 3, ic = indices[face * 3 + 2] * 3;
      if (![ia, ib, ic].every((index) => Number.isInteger(index) && index >= 0 && index + 2 < positions.length)) continue;
      const e1x = positions[ib] - positions[ia], e1y = positions[ib + 1] - positions[ia + 1], e1z = positions[ib + 2] - positions[ia + 2];
      const e2x = positions[ic] - positions[ia], e2y = positions[ic + 1] - positions[ia + 1], e2z = positions[ic + 2] - positions[ia + 2];
      const px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x, det = e1x * px + e1y * py + e1z * pz;
      if (Math.abs(det) <= 1e-12) continue;
      const invDet = 1 / det, tx = ox - positions[ia], ty = oy - positions[ia + 1], tz = oz - positions[ia + 2], u = (tx * px + ty * py + tz * pz) * invDet;
      if (u < 0 || u > 1) continue;
      const qx = ty * e1z - tz * e1y, qy = tz * e1x - tx * e1z, qz = tx * e1y - ty * e1x, v = (dx * qx + dy * qy + dz * qz) * invDet, distance = (e2x * qx + e2y * qy + e2z * qz) * invDet;
      if (v < 0 || u + v > 1 || distance < 0 || distance >= best) continue;
      best = distance; bestFace = face; bestU = u; bestV = v;
    }
    if (bestFace >= 0) { output.distance[ray] = best; output.triangle[ray] = bestFace; output.barycentrics[ro] = 1 - bestU - bestV; output.barycentrics[ro + 1] = bestU; output.barycentrics[ro + 2] = bestV; }
  }
  return output;
}

// Transfer a source UV set onto target vertices by shooting a pair of normal
// rays. This is deliberately deterministic and conservative: ambiguous
// vertices remain misses rather than receiving a UV from an arbitrary side.
export function transferUVsByProjection({ targetPositions, targetNormals, sourcePositions, sourceIndices, sourceUVs, maxDistance = Infinity, offset = 1e-6 } = {}) {
  if (!targetPositions || targetPositions.length % 3 || !targetNormals || targetNormals.length !== targetPositions.length || !sourcePositions || sourcePositions.length % 3 || !sourceIndices || sourceIndices.length % 3 || !sourceUVs || sourceUVs.length !== sourcePositions.length / 3 * 2) throw new Error('UV projection buffers have incompatible shapes.');
  const vertexCount = targetPositions.length / 3, origins = new Float32Array(vertexCount * 2 * 3), directions = new Float32Array(vertexCount * 2 * 3);
  for (let vertex = 0; vertex < vertexCount; vertex++) {
    const source = vertex * 3, nx = targetNormals[source], ny = targetNormals[source + 1], nz = targetNormals[source + 2], length = Math.hypot(nx, ny, nz);
    if (!Number.isFinite(length) || length <= 1e-12) continue;
    const x = targetPositions[source], y = targetPositions[source + 1], z = targetPositions[source + 2], normalX = nx / length, normalY = ny / length, normalZ = nz / length;
    for (let side = 0; side < 2; side++) {
      const ray = (vertex * 2 + side) * 3, sign = side ? -1 : 1;
      origins[ray] = x + normalX * offset * sign; origins[ray + 1] = y + normalY * offset * sign; origins[ray + 2] = z + normalZ * offset * sign;
      directions[ray] = normalX * sign; directions[ray + 1] = normalY * sign; directions[ray + 2] = normalZ * sign;
    }
  }
  const hits = raycastTriangles({ origins, directions, positions: sourcePositions, indices: sourceIndices, maxDistance }), uvs = new Float32Array(vertexCount * 2), hitMask = new Uint8Array(vertexCount); let hitCount = 0;
  for (let vertex = 0; vertex < vertexCount; vertex++) {
    const first = vertex * 2, second = first + 1, firstDistance = hits.distance[first], secondDistance = hits.distance[second], ray = firstDistance <= secondDistance ? first : second;
    if (hits.triangle[ray] < 0 || !Number.isFinite(hits.distance[ray])) continue;
    const value = projectRayHitAttributes({ triangle: new Int32Array([hits.triangle[ray]]), barycentrics: hits.barycentrics.subarray(ray * 3, ray * 3 + 3), sourceIndices, sourceValues: sourceUVs, itemSize: 2 }).values;
    uvs[vertex * 2] = value[0]; uvs[vertex * 2 + 1] = value[1]; hitMask[vertex] = 1; hitCount++;
  }
  return { uvs, hitMask, hitCount, missCount: vertexCount - hitCount };
}

export function mergeTransferredUVs(transferred, original, hitMask) {
  if (!transferred || !original || !hitMask || transferred.length !== original.length || transferred.length !== hitMask.length * 2) throw new Error('UV transfer merge buffers have incompatible shapes.');
  const output = new Float32Array(transferred);
  for (let vertex = 0; vertex < hitMask.length; vertex++) {
    if (hitMask[vertex]) continue;
    output[vertex * 2] = original[vertex * 2]; output[vertex * 2 + 1] = original[vertex * 2 + 1];
  }
  return output;
}

export function buildProjectionRays({ positions, indices, uvs, normals = null, resolution = 1024, rayDistance = 1, cageOffset = 1e-4 } = {}) {
  if (!positions || positions.length % 3 || !indices || indices.length % 3 || !uvs || uvs.length !== positions.length / 3 * 2) throw new Error('Projection mesh buffers have incompatible shapes.');
  try { validateIndexedMesh({ positions, indices }); } catch (error) { throw new Error(`Projection mesh buffers are invalid: ${error.message}`); }
  const size = Math.max(1, Math.min(8192, Math.floor(Number(resolution) || 1024))), distance = Math.max(1e-6, Number(rayDistance) || 1), offset = Math.max(0, Math.min(distance, Number(cageOffset) || 0)), islandOwners = buildUVIslandOwners(indices);
  const origins = [], directions = [], pixels = [], barycentrics = [], targetTriangles = [], covered = new Uint8Array(size * size), owners = new Int32Array(size * size).fill(-1);
  for (let face = 0; face < indices.length / 3; face++) {
    const ids = [indices[face * 3], indices[face * 3 + 1], indices[face * 3 + 2]];
    if (!ids.every((id) => Number.isInteger(id) && id >= 0 && id < positions.length / 3)) continue;
    const uv = ids.map((id) => [uvs[id * 2] * size, (1 - uvs[id * 2 + 1]) * size]);
    const minX = Math.max(0, Math.floor(Math.min(...uv.map((point) => point[0])))), maxX = Math.min(size - 1, Math.ceil(Math.max(...uv.map((point) => point[0]))));
    const minY = Math.max(0, Math.floor(Math.min(...uv.map((point) => point[1])))), maxY = Math.min(size - 1, Math.ceil(Math.max(...uv.map((point) => point[1]))));
    const denominator = (uv[1][1] - uv[2][1]) * (uv[0][0] - uv[2][0]) + (uv[2][0] - uv[1][0]) * (uv[0][1] - uv[2][1]);
    if (Math.abs(denominator) <= 1e-12) continue;
    for (let y = minY; y <= maxY; y++) for (let x = minX; x <= maxX; x++) {
      const px = x + .5, py = y + .5, a = ((uv[1][1] - uv[2][1]) * (px - uv[2][0]) + (uv[2][0] - uv[1][0]) * (py - uv[2][1])) / denominator, b = ((uv[2][1] - uv[0][1]) * (px - uv[2][0]) + (uv[0][0] - uv[2][0]) * (py - uv[2][1])) / denominator, c = 1 - a - b;
      if (a < 0 || b < 0 || c < 0) continue;
      const pixel = y * size + x; if (covered[pixel]) continue;
      const point = [0, 0, 0], normal = [0, 0, 0], weights = [a, b, c];
      for (let k = 0; k < 3; k++) { const vertex = ids[k] * 3; for (let component = 0; component < 3; component++) point[component] += weights[k] * positions[vertex + component]; if (normals?.length === positions.length) for (let component = 0; component < 3; component++) normal[component] += weights[k] * normals[vertex + component]; }
      if (normals?.length !== positions.length) { const ax = positions[ids[1] * 3] - positions[ids[0] * 3], ay = positions[ids[1] * 3 + 1] - positions[ids[0] * 3 + 1], az = positions[ids[1] * 3 + 2] - positions[ids[0] * 3 + 2], bx = positions[ids[2] * 3] - positions[ids[0] * 3], by = positions[ids[2] * 3 + 1] - positions[ids[0] * 3 + 1], bz = positions[ids[2] * 3 + 2] - positions[ids[0] * 3 + 2]; normal[0] = ay * bz - az * by; normal[1] = az * bx - ax * bz; normal[2] = ax * by - ay * bx; }
      const length = Math.hypot(...normal); if (!Number.isFinite(length) || length <= 1e-12) continue; for (let component = 0; component < 3; component++) normal[component] /= length; covered[pixel] = 1; owners[pixel] = islandOwners.labels[face];
      origins.push(point[0] + normal[0] * offset, point[1] + normal[1] * offset, point[2] + normal[2] * offset); directions.push(-normal[0], -normal[1], -normal[2]); pixels.push(pixel); barycentrics.push(a, b, c);
      targetTriangles.push(face);
    }
  }
  return { origins: new Float32Array(origins), directions: new Float32Array(directions), pixels: new Uint32Array(pixels), barycentrics: new Float32Array(barycentrics), targetTriangles: new Int32Array(targetTriangles), covered, owners, islandCount: islandOwners.islandCount, resolution: size, rayDistance: distance, cageOffset: offset };
}

export function dilateProjectedPixels({ pixels, hitMask, owners, resolution, passes = 2 } = {}) {
  const size = Math.max(1, Math.floor(Number(resolution) || 1)), count = size * size;
  if (!pixels || pixels.length !== count * 4 || !hitMask || hitMask.length !== count || !owners || owners.length !== count) throw new Error('Projected dilation buffers have incompatible shapes.');
  const output = new Uint8ClampedArray(pixels), filled = new Uint8Array(hitMask), rounds = Math.max(0, Math.min(32, Math.floor(Number(passes) || 0))); let filledCount = 0;
  for (let pass = 0; pass < rounds; pass++) {
    const next = new Uint8Array(filled), nextOwners = new Int32Array(owners);
    for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) { const pixel = y * size + x; if (filled[pixel] || owners[pixel] < 0) continue; const neighbors = [[x - 1, y], [x + 1, y], [x, y - 1], [x, y + 1]].filter(([nx, ny]) => nx >= 0 && ny >= 0 && nx < size && ny < size && filled[ny * size + nx] && owners[ny * size + nx] === owners[pixel]); if (!neighbors.length) continue; const [nx, ny] = neighbors[0], from = (ny * size + nx) * 4, to = pixel * 4; output.set(output.subarray(from, from + 4), to); next[pixel] = 1; nextOwners[pixel] = owners[pixel]; filledCount++; }
    filled.set(next); owners.set(nextOwners);
  }
  return { pixels: output, hitMask: filled, owners, filledCount };
}

export function transformProjectionPositions(positions, matrixElements) {
  if (!positions || positions.length % 3) throw new Error('Projection positions must contain xyz triplets.');
  if (!matrixElements || matrixElements.length !== 16) return new Float32Array(positions);
  const output = new Float32Array(positions);
  for (let i = 0; i < positions.length; i += 3) { const x = positions[i], y = positions[i + 1], z = positions[i + 2]; output[i] = matrixElements[0] * x + matrixElements[4] * y + matrixElements[8] * z + matrixElements[12]; output[i + 1] = matrixElements[1] * x + matrixElements[5] * y + matrixElements[9] * z + matrixElements[13]; output[i + 2] = matrixElements[2] * x + matrixElements[6] * y + matrixElements[10] * z + matrixElements[14]; }
  return output;
}

export function transformProjectionNormals(normals, matrixElements) {
  if (!normals || normals.length % 3) throw new Error('Projection normals must contain xyz triplets.');
  if (!matrixElements || matrixElements.length !== 16) return new Float32Array(normals);
  const a = matrixElements[0], b = matrixElements[4], c = matrixElements[8], d = matrixElements[1], f = matrixElements[5], g = matrixElements[9], h = matrixElements[2], i = matrixElements[6], j = matrixElements[10], determinant = a * (f * j - g * i) - b * (d * j - g * h) + c * (d * i - f * h);
  if (!Number.isFinite(determinant) || Math.abs(determinant) <= 1e-12) return new Float32Array(normals);
  const inv = 1 / determinant, i00 = (f * j - g * i) * inv, i01 = (c * i - b * j) * inv, i02 = (b * g - c * f) * inv, i10 = (g * h - d * j) * inv, i11 = (a * j - c * h) * inv, i12 = (c * d - a * g) * inv, i20 = (d * i - f * h) * inv, i21 = (b * h - a * i) * inv, i22 = (a * f - b * d) * inv, output = new Float32Array(normals);
  for (let offset = 0; offset < normals.length; offset += 3) { const x = i00 * normals[offset] + i10 * normals[offset + 1] + i20 * normals[offset + 2], y = i01 * normals[offset] + i11 * normals[offset + 1] + i21 * normals[offset + 2], z = i02 * normals[offset] + i12 * normals[offset + 1] + i22 * normals[offset + 2], length = Math.hypot(x, y, z); if (Number.isFinite(length) && length > 1e-12) { output[offset] = x / length; output[offset + 1] = y / length; output[offset + 2] = z / length; } }
  return output;
}

export function transformProjectionTangents(tangents, matrixElements) {
  if (!tangents || tangents.length % 4) throw new Error('Projection tangents must contain xyzw quartets.');
  if (!matrixElements || matrixElements.length !== 16) return new Float32Array(tangents);
  const a = matrixElements[0], b = matrixElements[4], c = matrixElements[8], d = matrixElements[1], e = matrixElements[5], f = matrixElements[9], g = matrixElements[2], h = matrixElements[6], i = matrixElements[10], determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g), handednessSign = Number.isFinite(determinant) && Math.abs(determinant) > 1e-12 && determinant < 0 ? -1 : 1, output = new Float32Array(tangents);
  for (let offset = 0; offset < tangents.length; offset += 4) {
    const x = matrixElements[0] * tangents[offset] + matrixElements[4] * tangents[offset + 1] + matrixElements[8] * tangents[offset + 2], y = matrixElements[1] * tangents[offset] + matrixElements[5] * tangents[offset + 1] + matrixElements[9] * tangents[offset + 2], z = matrixElements[2] * tangents[offset] + matrixElements[6] * tangents[offset + 1] + matrixElements[10] * tangents[offset + 2], length = Math.hypot(x, y, z);
    if (Number.isFinite(length) && length > 1e-12) { output[offset] = x / length; output[offset + 1] = y / length; output[offset + 2] = z / length; output[offset + 3] = tangents[offset + 3] * handednessSign; }
  }
  return output;
}

export function sampleProjectedTexture({ triangle, barycentrics, sourceIndices, sourceUVs, texturePixels, width, height, materialTextures = null, sourceMaterialIndices = null, channelIndex = null, sourceColorSpace = 'srgb', sourceColorTransform = null, destinationColorSpace = 'srgb' } = {}) {
  if (!sourceUVs || sourceUVs.length % 2 || hasInvalidValue(sourceUVs, (value) => !Number.isFinite(value))) throw new Error('Projected source UVs must contain finite pairs.');
  if (materialTextures == null && !texturePixels || texturePixels && (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || texturePixels.length !== width * height * 4)) throw new Error('Projected texture buffers are invalid.');
  if (texturePixels && hasInvalidValue(texturePixels, (value) => !Number.isFinite(value))) throw new Error('Projected texture buffers must contain finite pixel values.');
  if (sourceMaterialIndices && (sourceMaterialIndices.length !== triangle.length || hasInvalidValue(sourceMaterialIndices, (value) => !Number.isSafeInteger(value) || value < 0))) throw new Error('Projected material indices are invalid.');
  if (materialTextures?.some((entry) => entry && (!Number.isInteger(entry.width) || !Number.isInteger(entry.height) || entry.width < 1 || entry.height < 1 || !entry.pixels || entry.pixels.length !== entry.width * entry.height * 4 || hasInvalidValue(entry.pixels, (value) => !Number.isFinite(value))))) throw new Error('Projected material texture buffers are invalid.');
  if (materialTextures && !texturePixels && triangle.some((_value, ray) => !materialTextures[sourceMaterialIndices?.[ray] ?? 0])) throw new Error('Projected material texture descriptors are missing for one or more source materials.');
  const uvs = projectRayHitAttributes({ triangle, barycentrics, sourceIndices, sourceValues: sourceUVs, itemSize: 2, missValue: 0 }).values, output = new Float32Array(triangle.length * 3), scale = texturePixels instanceof Uint8Array || texturePixels instanceof Uint8ClampedArray ? 1 / 255 : 1;
  for (let ray = 0; ray < triangle.length; ray++) { if (triangle[ray] < 0) continue; const descriptor = materialTextures?.[sourceMaterialIndices?.[ray] ?? 0], pixels = descriptor?.pixels || texturePixels, imageWidth = descriptor?.width || width, imageHeight = descriptor?.height || height, imageScale = pixels instanceof Uint8Array || pixels instanceof Uint8ClampedArray ? 1 / 255 : 1, u = Math.max(0, Math.min(1, uvs[ray * 2])), v = Math.max(0, Math.min(1, uvs[ray * 2 + 1])), x = Math.min(imageWidth - 1, Math.floor(u * imageWidth)), y = Math.min(imageHeight - 1, Math.floor((1 - v) * imageHeight)), source = (y * imageWidth + x) * 4, target = ray * 3; if (channelIndex == null) { const color = convertBakeColor([pixels[source] * imageScale, pixels[source + 1] * imageScale, pixels[source + 2] * imageScale], descriptor?.colorSpace || sourceColorSpace, destinationColorSpace, descriptor?.colorTransform || sourceColorTransform); output.set(color, target); } else { const value = pixels[source + Math.max(0, Math.min(3, Math.floor(channelIndex)))] * imageScale; output[target] = output[target + 1] = output[target + 2] = value; } }
  return output;
}
