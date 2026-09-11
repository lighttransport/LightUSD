import { hasInvalidValue } from './indexed-mesh.js';

const finite = (value) => Number.isFinite(value);

export function analyzeSkeleton(skeleton = null) {
  const bones = Array.isArray(skeleton) ? skeleton : skeleton?.bones;
  if (!bones) return { present: false, boneCount: 0, duplicateNames: 0, missingParents: 0, cycles: 0, disconnectedRoots: 0, malformedTransforms: 0, nonFiniteTransforms: 0, nonInvertibleTransforms: 0, valid: true };
  const indices = new Map();
  for (let index = 0; index < bones.length; index++) indices.set(bones[index], index);
  const names = new Map(), parents = new Int32Array(bones.length).fill(-1);
  let duplicateNames = 0, missingParents = 0, malformedTransforms = 0, nonFiniteTransforms = 0, nonInvertibleTransforms = 0, disconnectedRoots = 0;
  for (let index = 0; index < bones.length; index++) {
    const bone = bones[index], name = String(bone?.name || '');
    if (name) { if (names.has(name)) duplicateNames++; else names.set(name, index); }
    if (bone?.parent && bone.parent !== bone) { const parent = indices.get(bone.parent); if (parent == null) missingParents++; else parents[index] = parent; }
    const matrix = bone?.matrixWorld?.elements || bone?.matrix?.elements;
    if (matrix && matrix.length !== 16) malformedTransforms++;
    else if (matrix && hasInvalidValue(matrix, (value) => !finite(value))) nonFiniteTransforms++;
    else if (matrix && Math.abs(matrixDeterminant(matrix)) <= 1e-12) nonInvertibleTransforms++;
    if (parents[index] < 0) disconnectedRoots++;
  }
  let cycles = 0; const state = new Uint8Array(bones.length);
  for (let index = 0; index < bones.length; index++) {
    let cursor = index;
    while (cursor >= 0 && state[cursor] === 0) { state[cursor] = 1; cursor = parents[cursor]; }
    if (cursor >= 0 && state[cursor] === 1) cycles++;
    cursor = index;
    while (cursor >= 0 && state[cursor] === 1) { state[cursor] = 2; cursor = parents[cursor]; }
  }
  return { present: true, boneCount: bones.length, duplicateNames, missingParents, cycles, disconnectedRoots, malformedTransforms, nonFiniteTransforms, nonInvertibleTransforms, valid: !duplicateNames && !missingParents && !cycles && !malformedTransforms && !nonFiniteTransforms && !nonInvertibleTransforms };
}

const matrixElements = (matrix) => matrix?.elements || matrix;
const matrixDeterminant = (m) => m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
const matrixSequence = (value) => {
  if (Array.isArray(value)) return { count: value.length, get: (index) => matrixElements(value[index]) };
  if (!value?.length || value.length % 16) return { count: 0, get: () => null };
  return { count: value.length / 16, get: (index) => value.subarray ? value.subarray(index * 16, index * 16 + 16) : value.slice(index * 16, index * 16 + 16) };
};

export function analyzeBindTransforms(bindMatrices = null, boneCount = null) {
  if (!bindMatrices) return { present: false, count: 0, countMismatch: false, nonFinite: 0, nonInvertible: 0, valid: true };
  const matrices = matrixSequence(bindMatrices);
  let nonFinite = 0, nonInvertible = 0;
  for (let index = 0; index < matrices.count; index++) {
    const matrix = matrices.get(index);
    if (!matrix || matrix.length !== 16 || hasInvalidValue(matrix, (value) => !finite(value))) { nonFinite++; continue; }
    if (Math.abs(matrixDeterminant(matrix)) <= 1e-12) nonInvertible++;
  }
  const countMismatch = boneCount != null && matrices.count !== boneCount;
  return { present: true, count: matrices.count, countMismatch, nonFinite, nonInvertible, valid: !countMismatch && !nonFinite && !nonInvertible };
}

export function analyzeBindPose({ bones = null, bindMatrices = null } = {}) {
  const bind = analyzeBindTransforms(bindMatrices, bones?.length ?? null);
  if (!bind.present || !Array.isArray(bones)) return { present: false, compared: 0, maxResidual: 0, meanResidual: 0, residuals: [], valid: true };
  const matrices = matrixSequence(bindMatrices);
  let compared = 0, sum = 0, maxResidual = 0; const residuals = [];
  for (let boneIndex = 0; boneIndex < Math.min(bones.length, matrices.count); boneIndex++) {
    const world = matrixElements(bones[boneIndex]?.matrixWorld), inverseBind = matrices.get(boneIndex);
    if (!world || world.length !== 16 || !inverseBind || inverseBind.length !== 16 || hasInvalidValue(world, (value) => !finite(value)) || hasInvalidValue(inverseBind, (value) => !finite(value))) continue;
    let residual = 0;
    for (let row = 0; row < 4; row++) for (let column = 0; column < 4; column++) {
      let value = 0; for (let k = 0; k < 4; k++) value += world[k * 4 + row] * inverseBind[column * 4 + k];
      const expected = row === column ? 1 : 0; residual = Math.max(residual, Math.abs(value - expected));
    }
    compared++; sum += residual; maxResidual = Math.max(maxResidual, residual); residuals.push({ index: boneIndex, name: String(bones[boneIndex]?.name || boneIndex), residual });
  }
  const meanResidual = compared ? sum / compared : 0;
  residuals.sort((a, b) => b.residual - a.residual || a.index - b.index);
  return { present: true, compared, maxResidual, meanResidual, residuals: residuals.slice(0, 256), valid: compared === matrices.count && maxResidual <= 1e-4 };
}

// Compare current skinned positions against bind-space positions without
// mutating Three.js objects. boneMatrices are packed column-major matrices.
export function compareSkinnedDeformation({ positions, skinIndices, skinWeights, boneMatrices, maxSamples = 256 } = {}) {
  if (!positions?.length || positions.length % 3 || !skinIndices || !skinWeights || skinIndices.length !== skinWeights.length || skinIndices.length !== positions.length / 3 * 4 || !boneMatrices || boneMatrices.length % 16) throw new Error('Skinned deformation comparison requires matching positions, four-influence skin data, and packed bone matrices.');
  if (hasInvalidValue(positions, (value) => !finite(value)) || hasInvalidValue(skinIndices, (value) => !finite(value)) || hasInvalidValue(skinWeights, (value) => !finite(value)) || hasInvalidValue(boneMatrices, (value) => !finite(value))) throw new Error('Skinned deformation comparison requires finite buffers.');
  const boneCount = boneMatrices.length / 16, count = positions.length / 3, samples = [];
  if (hasInvalidValue(skinIndices, (joint) => !Number.isSafeInteger(joint) || joint < 0 || joint >= boneCount) || hasInvalidValue(skinWeights, (weight) => weight < 0)) throw new Error('Skinned deformation comparison requires non-negative in-range joint indices and weights.');
  let totalDistance = 0, maxDistance = 0;
  for (let vertex = 0; vertex < count; vertex++) {
    const px = positions[vertex * 3], py = positions[vertex * 3 + 1], pz = positions[vertex * 3 + 2]; let x = 0, y = 0, z = 0, weightTotal = 0;
    for (let influence = 0; influence < 4; influence++) {
      const offset = vertex * 4 + influence, joint = skinIndices[offset], weight = skinWeights[offset];
      if (!Number.isInteger(joint) || joint < 0 || joint >= boneCount || weight <= 0) continue;
      const matrix = joint * 16, tx = boneMatrices[matrix] * px + boneMatrices[matrix + 4] * py + boneMatrices[matrix + 8] * pz + boneMatrices[matrix + 12], ty = boneMatrices[matrix + 1] * px + boneMatrices[matrix + 5] * py + boneMatrices[matrix + 9] * pz + boneMatrices[matrix + 13], tz = boneMatrices[matrix + 2] * px + boneMatrices[matrix + 6] * py + boneMatrices[matrix + 10] * pz + boneMatrices[matrix + 14];
      x += tx * weight; y += ty * weight; z += tz * weight; weightTotal += weight;
    }
    if (weightTotal > 1e-12) { x /= weightTotal; y /= weightTotal; z /= weightTotal; } else { x = px; y = py; z = pz; }
    const distance = Math.hypot(x - px, y - py, z - pz); totalDistance += distance; maxDistance = Math.max(maxDistance, distance); samples.push({ vertex, distance });
  }
  samples.sort((a, b) => b.distance - a.distance || a.vertex - b.vertex);
  return { compared: count, meanDistance: totalDistance / count, maxDistance, samples: samples.slice(0, Math.max(1, Math.min(256, Math.floor(maxSamples) || 256))) };
}

const closestPointOnTriangle = (point, a, b, c) => {
  const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], ac = [c[0] - a[0], c[1] - a[1], c[2] - a[2]], ap = [point[0] - a[0], point[1] - a[1], point[2] - a[2]], d1 = ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2], d2 = ac[0] * ap[0] + ac[1] * ap[1] + ac[2] * ap[2];
  if (d1 <= 0 && d2 <= 0) return { point: a, bary: [1, 0, 0] };
  const bp = [point[0] - b[0], point[1] - b[1], point[2] - b[2]], d3 = ab[0] * bp[0] + ab[1] * bp[1] + ab[2] * bp[2], d4 = ac[0] * bp[0] + ac[1] * bp[1] + ac[2] * bp[2];
  if (d3 >= 0 && d4 <= d3) return { point: b, bary: [0, 1, 0] };
  const vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) { const v = d1 / (d1 - d3); return { point: [a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2]], bary: [1 - v, v, 0] }; }
  const cp = [point[0] - c[0], point[1] - c[1], point[2] - c[2]], d5 = ab[0] * cp[0] + ab[1] * cp[1] + ab[2] * cp[2], d6 = ac[0] * cp[0] + ac[1] * cp[1] + ac[2] * cp[2];
  if (d6 >= 0 && d5 <= d6) return { point: c, bary: [0, 0, 1] };
  const vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) { const w = d2 / (d2 - d6); return { point: [a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2]], bary: [1 - w, 0, w] }; }
  const va = d3 * d6 - d5 * d4, v = vb / (va + vb + vc), w = vc / (va + vb + vc);
  return { point: [a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w], bary: [1 - v - w, v, w] };
};

export function transferNearestSkinWeights({ sourcePositions, sourceIndices = null, sourceSkinIndices, sourceSkinWeights, targetPositions, boneCount = null, maxInfluences = 4 } = {}) {
  const source = analyzeSkinning({ skinIndices: sourceSkinIndices, skinWeights: sourceSkinWeights, boneCount });
  if (!source.present || source.malformed || source.nonFiniteValues || source.negativeWeights || source.invalidJointIndices || !sourcePositions?.length || sourcePositions.length % 3 || !targetPositions?.length || targetPositions.length % 3 || hasInvalidValue(sourcePositions, (value) => !finite(value)) || hasInvalidValue(targetPositions, (value) => !finite(value))) throw new Error('Skin transfer requires finite, non-negative source skin data and valid joint indices.');
  if (sourceIndices && (sourceIndices.length % 3 || hasInvalidValue(sourceIndices, (index) => !Number.isInteger(index) || index < 0 || index >= sourcePositions.length / 3))) throw new Error('Skin transfer source topology contains invalid triangle indices.');
  const sourceCount = sourcePositions.length / 3, targetCount = targetPositions.length / 3, influences = Math.max(1, Math.min(4, Math.floor(Number(maxInfluences) || 4))), jointIndices = new Uint16Array(targetCount * 4), jointWeights = new Float32Array(targetCount * 4), distances = new Float32Array(targetCount);
  let distanceSum = 0, maxDistance = 0;
  for (let target = 0; target < targetCount; target++) {
    const targetPoint = [targetPositions[target * 3], targetPositions[target * 3 + 1], targetPositions[target * 3 + 2]];
    let nearest = 0, nearestDistance = Infinity, bary = [1, 0, 0], sourceVertices = [0];
    if (sourceIndices?.length && sourceIndices.length % 3 === 0) for (let offset = 0; offset < sourceIndices.length; offset += 3) {
      const vertexIds = [sourceIndices[offset], sourceIndices[offset + 1], sourceIndices[offset + 2]]; if (vertexIds.some((id) => !Number.isInteger(id) || id < 0 || id >= sourceCount)) continue;
      const result = closestPointOnTriangle(targetPoint, vertexIds.map((id) => [sourcePositions[id * 3], sourcePositions[id * 3 + 1], sourcePositions[id * 3 + 2]])[0], vertexIds.map((id) => [sourcePositions[id * 3], sourcePositions[id * 3 + 1], sourcePositions[id * 3 + 2]])[1], vertexIds.map((id) => [sourcePositions[id * 3], sourcePositions[id * 3 + 1], sourcePositions[id * 3 + 2]])[2]), dx = targetPoint[0] - result.point[0], dy = targetPoint[1] - result.point[1], dz = targetPoint[2] - result.point[2], distance = dx * dx + dy * dy + dz * dz;
      if (distance < nearestDistance) { nearestDistance = distance; bary = result.bary; sourceVertices = vertexIds; }
    }
    if (!Number.isFinite(nearestDistance)) { for (let candidate = 0; candidate < sourceCount; candidate++) { const dx = targetPoint[0] - sourcePositions[candidate * 3], dy = targetPoint[1] - sourcePositions[candidate * 3 + 1], dz = targetPoint[2] - sourcePositions[candidate * 3 + 2], distance = dx * dx + dy * dy + dz * dz; if (distance < nearestDistance) { nearestDistance = distance; nearest = candidate; } } sourceVertices = [nearest]; bary = [1, 0, 0]; }
    const accumulated = new Map();
    sourceVertices.forEach((vertex, corner) => { for (let influence = 0; influence < 4; influence++) { const offset = vertex * 4 + influence, joint = sourceSkinIndices[offset], weight = sourceSkinWeights[offset] * (bary[corner] || 0); if (weight > 1e-12) accumulated.set(joint, (accumulated.get(joint) || 0) + weight); } });
    const selected = [...accumulated.entries()].sort((a, b) => b[1] - a[1] || a[0] - b[0]).slice(0, influences), total = selected.reduce((sum, [, weight]) => sum + weight, 0) || 1;
    selected.forEach(([joint, weight], influence) => { jointIndices[target * 4 + influence] = joint; jointWeights[target * 4 + influence] = weight / total; });
    const distance = Math.sqrt(nearestDistance); distances[target] = distance; distanceSum += distance; maxDistance = Math.max(maxDistance, distance);
  }
  return { jointIndices, jointWeights, distances, targetVertexCount: targetCount, meanDistance: distanceSum / targetCount, maxDistance };
}

// Read-only skin audit. The routine deliberately accepts plain typed arrays so
// it can be shared by browser reports, Node fixtures, and a future worker.
export function analyzeSkinning({ skinIndices, skinWeights, boneCount = null, boneMatrices = null, bindMatrices = null, skeleton = null, maxInfluences = 4 } = {}) {
  const indices = skinIndices, weights = skinWeights;
  if (!indices && !weights) return { present: false, vertices: 0, unweightedVertices: 0, weightSumMismatches: 0, negativeWeights: 0, excessiveInfluences: 0, invalidJointIndices: 0, nonFiniteValues: 0, nonInvertibleBones: 0, influenceSamples: [], skeleton: analyzeSkeleton(skeleton), valid: true };
  const itemSize = 4, vertices = Math.max(indices?.length || 0, weights?.length || 0) / itemSize;
  let unweightedVertices = 0, weightSumMismatches = 0, negativeWeights = 0, excessiveInfluences = 0, invalidJointIndices = 0, nonFiniteValues = 0; const influenceSamples = [];
  const malformed = !Number.isInteger(vertices) || !indices || !weights || indices.length !== weights.length || indices.length % itemSize !== 0;
  if (!malformed) for (let vertex = 0; vertex < vertices; vertex++) {
    let sum = 0, positive = 0; const vertexIssues = new Set();
    for (let influence = 0; influence < itemSize; influence++) {
      const offset = vertex * itemSize + influence, joint = indices[offset], weight = weights[offset];
      if (!Number.isInteger(joint) || joint < 0 || (boneCount != null && (!Number.isInteger(boneCount) || joint >= boneCount))) { invalidJointIndices++; vertexIssues.add('invalid joint'); }
      if (!finite(weight) || !finite(joint)) { nonFiniteValues++; vertexIssues.add('non-finite'); continue; }
      sum += weight; if (weight < -1e-8) { negativeWeights++; vertexIssues.add('negative weight'); } else if (weight > 1e-8) positive++;
    }
    if (sum <= 1e-8) { unweightedVertices++; vertexIssues.add('unweighted'); }
    else if (Math.abs(sum - 1) > 1e-3) { weightSumMismatches++; vertexIssues.add('weight sum'); }
    if (positive > Math.max(1, Math.floor(Number(maxInfluences) || 4))) { excessiveInfluences++; vertexIssues.add('excessive influences'); }
    if (vertexIssues.size && influenceSamples.length < 256) influenceSamples.push({ index: vertex, issues: [...vertexIssues].sort(), weightSum: finite(sum) ? sum : null, positiveInfluences: positive });
  }
  let nonInvertibleBones = 0;
  if (boneMatrices) {
    const matrixStride = 16;
    if (boneMatrices.length % matrixStride) nonInvertibleBones++;
    else for (let offset = 0; offset < boneMatrices.length; offset += matrixStride) {
      const m = boneMatrices.subarray ? boneMatrices.subarray(offset, offset + matrixStride) : boneMatrices.slice(offset, offset + matrixStride);
      if (m.some((value) => !finite(value))) { nonInvertibleBones++; continue; }
      const determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
      if (Math.abs(determinant) <= 1e-12) nonInvertibleBones++;
    }
  }
  const skeletonAudit = analyzeSkeleton(skeleton), bindAudit = analyzeBindTransforms(bindMatrices, boneCount), bindPose = analyzeBindPose({ bones: skeleton?.bones, bindMatrices });
  return { present: true, vertices: malformed ? 0 : vertices, malformed, unweightedVertices, weightSumMismatches, negativeWeights, excessiveInfluences, invalidJointIndices, nonFiniteValues, nonInvertibleBones, influenceSamples, skeleton: skeletonAudit, bindTransforms: bindAudit, bindPose, valid: !malformed && !unweightedVertices && !weightSumMismatches && !negativeWeights && !excessiveInfluences && !invalidJointIndices && !nonFiniteValues && !nonInvertibleBones && skeletonAudit.valid && bindAudit.valid };
}

// Produces a bounded, deterministic point set for a read-only influence preview.
// Keeping the projection data independent of Canvas makes the diagnostic useful
// in tests and leaves the UI free to choose its presentation.
export function buildInfluenceHeatmap({ positions, skinIndices, skinWeights, boneCount = null, maxSamples = 2048 } = {}) {
  const audit = analyzeSkinning({ skinIndices, skinWeights, boneCount });
  if (!audit.present || audit.malformed || !positions?.length || positions.length % 3 || audit.vertices !== positions.length / 3) throw new Error('Influence heatmap requires matching position and skin arrays.');
  const count = positions.length / 3, stride = Math.max(1, Math.ceil(count / Math.max(1, Math.floor(Number(maxSamples) || 2048)))), samples = [];
  let minX = Infinity, minY = Infinity, minZ = Infinity, maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (const value of positions) if (!finite(value)) throw new Error('Influence heatmap requires finite positions.');
  for (let vertex = 0; vertex < count; vertex++) {
    const x = positions[vertex * 3], y = positions[vertex * 3 + 1], z = positions[vertex * 3 + 2];
    minX = Math.min(minX, x); minY = Math.min(minY, y); minZ = Math.min(minZ, z); maxX = Math.max(maxX, x); maxY = Math.max(maxY, y); maxZ = Math.max(maxZ, z);
    if (vertex % stride) continue;
    let dominantJoint = 0, dominantWeight = 0, total = 0;
    for (let influence = 0; influence < 4; influence++) { const offset = vertex * 4 + influence, weight = skinWeights[offset]; if (weight > dominantWeight || (weight === dominantWeight && skinIndices[offset] < dominantJoint)) { dominantJoint = skinIndices[offset]; dominantWeight = weight; } total += Math.max(0, weight); }
    samples.push({ vertex, position: [x, y, z], joint: dominantJoint, weight: total ? dominantWeight / total : 0 });
  }
  const extent = [maxX - minX, maxY - minY, maxZ - minZ], dropAxis = extent.indexOf(Math.max(...extent)), axes = [[1, 2], [0, 2], [0, 1]][dropAxis];
  return { samples, bounds: { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] }, axes, droppedAxis: dropAxis, boneCount: boneCount == null ? null : boneCount, audit };
}
