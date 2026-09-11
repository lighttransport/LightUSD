import { LuciaError, decoder, encoder, escapeRegExp, validIdentifier, validPrimPath } from './utils.js';
import { recomputeVertexTangents } from './tangent-recompute.js';
import { projectUVs } from './uv-projection.js';
import { dilateProjectedPixels, mergeTransferredUVs, projectRayHitAttributes, sampleProjectedTexture, transformProjectionNormals, transformProjectionPositions, transformProjectionTangents } from './projection-bake.js';
import { deriveEdgeLocks, expandSharpChains, validateSharpChains, validateVertexLockMask } from './retopo-locks.js';
import { analyzeBakeAlpha, convertNormalMapY, encodeNormalVectors, encodeProjectedTangentNormals, estimateBakeWorkingBytes, estimateProjectionWorkingBytes, normalizeBakeColorTransform, resizeBakeImage, validateBakeResult, BAKE_MEMORY_LIMIT_BYTES } from './texture-bake.js';
import { generateConvexHull } from './physics-analysis.js';
import { correctComponentPreview, extractConnectedComponents, extractCorrectedComponents, previewConnectedComponents as previewComponentLabels } from './mesh-components.js';
import { previewCrackMerge as previewCleanupCrackMerge } from './mesh-cleanup.js';
import { hasInvalidValue, isSupportedNumericArray, normalizeIndexedMesh, validateIndexedMesh } from './indexed-mesh.js';
import { recomputeVertexNormals, validateSharpEdges } from './normal-recompute.js';
import { localizeUSDDependencies } from './usd-doctor.js';
import { evaluateMaterialGraph, rewriteMaterialGraph } from './material-graph.js';
import { translateMaterialGraph } from './material-translation.js';
import { materializePrimvarParameterization, materializeVariantParameterization, planMaterialParameterization } from './material-parameterization.js';
import { proposeComponentNames, uniqueComponentName } from './semantic-suggestions.js';
import { findPrimBlock } from './usd-session.js';
import { expandRetopoFaceVaryingMesh, remapRetopoFaceVaryingAttributes } from './retopo-face-varying.js';

const stableColor = (value) => { let hash = 2166136261; for (const character of String(value || '')) { hash ^= character.charCodeAt(0); hash = Math.imul(hash, 16777619); } return [(hash & 255) / 255, ((hash >>> 8) & 255) / 255, ((hash >>> 16) & 255) / 255]; };
const hasInvalidNormal = (values) => { for (let i = 0; i < values.length; i += 3) if (!Number.isFinite(values[i]) || !Number.isFinite(values[i + 1]) || !Number.isFinite(values[i + 2]) || Math.hypot(values[i], values[i + 1], values[i + 2]) <= 1e-12) return true; return false; };
const hasInvalidFinite = (values) => hasInvalidValue(values, (value) => !Number.isFinite(value));
const hasInvalidTangent = (values, requireDirection = false) => { for (let i = 0; i < values.length; i += 4) { if (!Number.isFinite(values[i]) || !Number.isFinite(values[i + 1]) || !Number.isFinite(values[i + 2]) || !Number.isFinite(values[i + 3]) || requireDirection && Math.hypot(values[i], values[i + 1], values[i + 2]) <= 1e-12) return true; } return false; };
const validateBakeControl = (value, { integer = false, minimum = 0, maximum }, label) => { if (value == null || typeof value !== 'number' || !Number.isFinite(value) || integer && !Number.isSafeInteger(value) || value < minimum || value > maximum) throw new LuciaError('LUCIA_BAKE_OPTIONS', `${label} is outside its supported range.`); };
const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';
const isByteBuffer = (value) => isIterableBuffer(value) && (value.constructor === Uint8Array || value.constructor === Uint8ClampedArray);
const validatePackedResult = (result, expectedChannels) => {
  if (!result?.success || !isByteBuffer(result.data) || !result.data.length) throw new LuciaError('LUCIA_PACK_FAILED', result?.error || 'LightUSD returned no encoded byte data for the packed texture.');
  if (!Number.isSafeInteger(result.width) || result.width < 1 || result.width > 8192 || !Number.isSafeInteger(result.height) || result.height < 1 || result.height > 8192 || !Number.isSafeInteger(result.channels) || result.channels < 1 || result.channels > 4 || result.channels !== expectedChannels) throw new LuciaError('LUCIA_PACK_RESULT', 'LightUSD returned invalid packed texture dimensions or channel metadata.');
};
const materialGraphChannel = (entry, requested) => { const graph = entry?.userData?.nodes || entry?.nodes, aliases = { baseColor: ['baseColor', 'base_color', 'diffuseColor', 'diffuse_color'], roughness: ['roughness', 'specular_roughness'], metallic: ['metallic', 'metalness'], opacity: ['opacity', 'alpha'], emissive: ['emissive', 'emissiveColor', 'emission_color', 'emissionColor'] }[requested] || []; for (const output of aliases) { let result; try { result = graph ? evaluateMaterialGraph(graph, { output }) : null; } catch { result = null; } if (!result?.supported) continue; const value = Array.isArray(result.value) ? result.value : [result.value, result.value, result.value]; if (value.length >= 3 && value.every((component) => Number.isFinite(component))) return value.slice(0, 3); } return null; };
const materialGraphParameterizationChannel = (entry, requested) => { const graph = entry?.userData?.nodes || entry?.nodes, aliases = { baseColor: ['baseColor', 'base_color', 'diffuseColor', 'diffuse_color'], roughness: ['roughness', 'specular_roughness'], metallic: ['metallic', 'metalness'], opacity: ['opacity', 'alpha'], emissive: ['emissive', 'emissiveColor', 'emission_color', 'emissionColor'] }[requested] || [], scalar = new Set(['roughness', 'metallic', 'opacity']); for (const output of aliases) { let result; try { result = graph ? evaluateMaterialGraph(graph, { output }) : null; } catch { result = null; } if (!result?.supported) continue; const value = result.value; if (scalar.has(requested) && typeof value === 'number' && Number.isFinite(value)) return value; if (!scalar.has(requested) && Array.isArray(value) && value.length === 3 && value.every((component) => Number.isFinite(component))) return [...value]; } return null; };
const materialChannelValues = (entry, requested) => { const graphValue = materialGraphChannel(entry, requested); if (graphValue) return graphValue; if (requested === 'roughness') return [entry?.roughness ?? .5, entry?.roughness ?? .5, entry?.roughness ?? .5]; if (requested === 'metallic') return [entry?.metalness ?? 0, entry?.metalness ?? 0, entry?.metalness ?? 0]; if (requested === 'opacity') return [entry?.opacity ?? 1, entry?.opacity ?? 1, entry?.opacity ?? 1]; if (requested === 'emissive') return [entry?.emissive?.r ?? 0, entry?.emissive?.g ?? 0, entry?.emissive?.b ?? 0]; return [entry?.color?.r ?? .7, entry?.color?.g ?? .7, entry?.color?.b ?? .7]; };
const parameterizationChannelValues = (entry, requested) => {
  const graphValue = materialGraphParameterizationChannel(entry, requested);
  if (graphValue) return graphValue;
  if (requested === 'baseColor' && entry?.color && [entry.color.r, entry.color.g, entry.color.b].every(Number.isFinite)) return [entry.color.r, entry.color.g, entry.color.b];
  if (requested === 'metallic' && Number.isFinite(entry?.metalness)) return entry.metalness;
  if (requested === 'roughness' && Number.isFinite(entry?.roughness)) return entry.roughness;
  if (requested === 'opacity' && Number.isFinite(entry?.opacity)) return entry.opacity;
  if (requested === 'emissive' && entry?.emissive && [entry.emissive.r, entry.emissive.g, entry.emissive.b].every(Number.isFinite)) return [entry.emissive.r, entry.emissive.g, entry.emissive.b];
  return null;
};
export const remapRetopoCreaseChains = ({ chains, sharpness, sourceVertexCopies, indices, xref }) => {
  const outputBySource = new Map(), edgeSet = new Set(), edgeKey = (a, b) => a < b ? `${a}:${b}` : `${b}:${a}`;
  for (let output = 0; output < xref.length; output++) if (!outputBySource.has(xref[output])) outputBySource.set(xref[output], output);
  for (let offset = 0; offset < indices.length; offset += 3) for (let edge = 0; edge < 3; edge++) edgeSet.add(edgeKey(indices[offset + edge], indices[offset + (edge + 1) % 3]));
  const mappedChains = [], mappedSharpness = [];
  for (let chainIndex = 0; chainIndex < chains.length; chainIndex++) {
    const candidates = chain => (sourceVertexCopies[chain] || []).map((source) => outputBySource.get(source)).filter((value, index, values) => value != null && values.indexOf(value) === index);
    const path = [], visit = (position, previous) => {
      if (position === chains[chainIndex].length) return true;
      for (const candidate of candidates(chains[chainIndex][position])) {
        if (candidate === previous || previous != null && !edgeSet.has(edgeKey(previous, candidate))) continue;
        path.push(candidate);
        if (visit(position + 1, candidate)) return true;
        path.pop();
      }
      return false;
    };
    if (visit(0, null)) { mappedChains.push(path.slice()); mappedSharpness.push(Number(sharpness?.[chainIndex] ?? 1)); }
  }
  return { chains: mappedChains, sharpness: mappedSharpness };
};
const remapRetopoCreaseEdges = ({ edges, sourceVertexCopies, indices, xref }) => {
  const chains = remapRetopoCreaseChains({ chains: edges, sourceVertexCopies, indices, xref });
  const output = [], sharpness = [];
  for (let index = 0; index < chains.chains.length; index++) { output.push([chains.chains[index][0], chains.chains[index][1]]); sharpness.push(chains.sharpness[index]); }
  return { edges: output, sharpness };
};

export function normalizeOperationProgress(progress = {}, fallbackMessage = 'Working…') {
  const percentage = Number(progress?.percentage), message = typeof progress?.message === 'string' && progress.message.trim() ? progress.message : fallbackMessage;
  return { percentage: Number.isFinite(percentage) ? Math.max(0, Math.min(100, percentage)) : 0, message };
}
export function normalizeOperationProgressCallback(onProgress, fallbackMessage = 'Working…') {
  return typeof onProgress === 'function' ? (progress) => onProgress(normalizeOperationProgress(progress, fallbackMessage)) : null;
}

export function postWorkerMessage(owner, worker, data, transfer, reject, code) {
  try { worker.postMessage(data, transfer); }
  catch (error) {
    if (owner.worker === worker) { owner.worker = null; owner.workerReject = null; }
    worker.terminate();
    reject(new LuciaError(code, error?.message || String(error)));
  }
}

export function validateGeometryData(geometry) {
  const position = geometry?.attributes?.position;
  if (!isSupportedNumericArray(position?.array) || !Number.isInteger(position.count) || position.count < 0 || position.array.length !== position.count * 3 || (position.itemSize != null && position.itemSize !== 3)) throw new LuciaError('LUCIA_BAKE_POSITION', 'Position data has an invalid count, stride, or supported typed backing array.');
  try { validateIndexedMesh({ positions: position.array, materializeIndices: false }); } catch (error) { throw new LuciaError('LUCIA_BAKE_POSITION', `The selected mesh contains invalid position data: ${error.message}`); }
  const index = geometry.index;
  if (index && (!isSupportedNumericArray(index.array) || !Number.isInteger(index.count) || index.count < 0 || index.count !== index.array.length || index.count % 3 || hasInvalidValue(index.array, (value) => !Number.isInteger(value) || value < 0 || value >= position.count))) throw new LuciaError('LUCIA_BAKE_INDEX', 'Index data must contain complete, in-range triangle triplets in a supported typed buffer.');
  if (index) try { validateIndexedMesh({ positions: position.array, indices: index.array, materializeIndices: false }); } catch (error) { throw new LuciaError('LUCIA_BAKE_INDEX', `Index data is invalid: ${error.message}`); }
  return true;
}

export function normalizeOperationMeshGeometry(geometry) {
  validateGeometryData(geometry);
  try { return normalizeIndexedMesh({ positions: geometry.attributes.position.array, indices: geometry.index?.array || null }); }
  catch (error) { throw new LuciaError('LUCIA_MESH_TOPOLOGY', `Mesh topology normalization failed: ${error.message}`); }
}

export function validateWorkerVector(values, length, code, label) {
  if (!isIterableBuffer(values) || !isSupportedNumericArray(values) || values.length !== length || hasInvalidValue(values, (value) => !Number.isFinite(value))) throw new LuciaError(code, `${label} worker returned an invalid or untyped vertex-aligned buffer.`);
  return values;
}

export function validateProjectionResult(result) {
  if (!result || typeof result !== 'object' || Array.isArray(result)) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned an invalid result object.');
  const size = result?.resolution, rayCount = result?.triangle?.length;
  if (!isIterableBuffer(result?.distance) || !isSupportedNumericArray(result.distance) || !isIterableBuffer(result.triangle) || !isSupportedNumericArray(result.triangle) || !isIterableBuffer(result.barycentrics) || !isSupportedNumericArray(result.barycentrics) || result.distance.length !== rayCount || result.barycentrics.length !== rayCount * 3 || !Number.isInteger(size) || size < 1 || size > 4096 || !isIterableBuffer(result.pixels) || !isSupportedNumericArray(result.pixels) || result.pixels.length !== rayCount || !isIterableBuffer(result.covered) || !isSupportedNumericArray(result.covered) || result.covered.length !== size * size || !isIterableBuffer(result.owners) || !isSupportedNumericArray(result.owners) || result.owners.length !== size * size || result.islandCount != null && (!Number.isInteger(result.islandCount) || result.islandCount < 0) || result.sourceTriangleCount != null && (!Number.isSafeInteger(result.sourceTriangleCount) || result.sourceTriangleCount < 1) || result.targetTriangleCount != null && (!Number.isSafeInteger(result.targetTriangleCount) || result.targetTriangleCount < 1) || result.targetTriangle != null && (!isIterableBuffer(result.targetTriangle) || !isSupportedNumericArray(result.targetTriangle) || result.targetTriangle.length !== rayCount) || result.targetBarycentrics != null && (!isIterableBuffer(result.targetBarycentrics) || !isSupportedNumericArray(result.targetBarycentrics) || result.targetBarycentrics.length !== rayCount * 3)) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned mismatched hit or raster buffers, including untyped buffers.');
  for (let i = 0; i < size * size; i++) { if (result.covered[i] !== 0 && result.covered[i] !== 1) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned an invalid coverage mask.'); if (!Number.isInteger(result.owners[i]) || result.owners[i] < -1 || result.owners[i] >= 0 && Number.isInteger(result.targetTriangleCount) && result.owners[i] >= result.targetTriangleCount) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned an invalid target-face owner.'); }
  for (let i = 0; i < rayCount; i++) { if (!Number.isInteger(result.triangle[i]) || result.triangle[i] < -1 || result.triangle[i] >= 0 && Number.isInteger(result.sourceTriangleCount) && result.triangle[i] >= result.sourceTriangleCount) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned an invalid source triangle sentinel.'); if (!Number.isInteger(result.pixels[i]) || result.pixels[i] < 0 || result.pixels[i] >= size * size) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned an out-of-range target pixel.'); if (result.triangle[i] >= 0 && (!result.covered[result.pixels[i]] || result.owners[result.pixels[i]] < 0)) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned a hit without target-face ownership.'); }
  if (result.targetTriangle) for (let i = 0; i < rayCount; i++) if (!Number.isInteger(result.targetTriangle[i]) || result.targetTriangle[i] < 0 || Number.isInteger(result.targetTriangleCount) && result.targetTriangle[i] >= result.targetTriangleCount) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned an invalid target triangle ordinal.');
  for (let i = 0; i < result.triangle.length; i++) { const distance = result.distance[i], hit = result.triangle[i] >= 0; if (hit ? !Number.isFinite(distance) || distance < 0 : distance !== Infinity && (!Number.isFinite(distance) || distance < 0)) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned an invalid hit distance.'); if (hit) { const offset = i * 3, sum = result.barycentrics[offset] + result.barycentrics[offset + 1] + result.barycentrics[offset + 2]; if (![result.barycentrics[offset], result.barycentrics[offset + 1], result.barycentrics[offset + 2]].every(Number.isFinite) || Math.abs(sum - 1) > 1e-3) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned invalid barycentrics.'); } }
  if (result.targetBarycentrics) for (let i = 0; i < rayCount; i++) { const offset = i * 3, sum = result.targetBarycentrics[offset] + result.targetBarycentrics[offset + 1] + result.targetBarycentrics[offset + 2]; if (![result.targetBarycentrics[offset], result.targetBarycentrics[offset + 1], result.targetBarycentrics[offset + 2]].every(Number.isFinite) || Math.abs(sum - 1) > 1e-3) throw new LuciaError('LUCIA_PROJECTION_RESULT', 'Projection worker returned invalid target barycentrics.'); }
  return result;
}

export function validateUVTransferResult(result, vertexCount) {
  if (!isIterableBuffer(result?.uvs) || !isSupportedNumericArray(result.uvs) || !Number.isInteger(vertexCount) || vertexCount < 1 || result.uvs.length !== vertexCount * 2 || !Number.isInteger(result.hitCount) || !Number.isInteger(result.missCount) || result.hitCount < 0 || result.missCount < 0 || result.hitCount + result.missCount !== vertexCount || hasInvalidValue(result.uvs, (value) => !Number.isFinite(value)) || result.hitMask != null && (!isIterableBuffer(result.hitMask) || !isSupportedNumericArray(result.hitMask) || result.hitMask.length !== vertexCount || hasInvalidValue(result.hitMask, (value) => value !== 0 && value !== 1) || result.hitCount !== result.hitMask.reduce((sum, value) => sum + value, 0))) throw new LuciaError('LUCIA_UV_TRANSFER_RESULT', 'UV transfer worker returned malformed, untyped, or non-finite vertex data.');
  return result;
}

export function validateUVTransferNormals(values, vertexCount) {
  if (!isIterableBuffer(values) || !isSupportedNumericArray(values) || !Number.isInteger(vertexCount) || vertexCount < 1 || values.length !== vertexCount * 3) throw new LuciaError('LUCIA_UV_TRANSFER_NORMAL', 'The target mesh needs one normal per target vertex in a typed buffer.');
  for (let i = 0; i < values.length; i += 3) if (!Number.isFinite(values[i]) || !Number.isFinite(values[i + 1]) || !Number.isFinite(values[i + 2]) || Math.hypot(values[i], values[i + 1], values[i + 2]) <= 1e-12) throw new LuciaError('LUCIA_UV_TRANSFER_NORMAL', 'The target mesh needs finite, non-zero vertex normals for two-sided projection.');
  return values;
}

export function validateSkinTransferResult(result, vertexCount, boneCount = null) {
  if (!isIterableBuffer(result?.jointIndices) || !isSupportedNumericArray(result.jointIndices) || !isIterableBuffer(result?.jointWeights) || !isSupportedNumericArray(result.jointWeights) || !Number.isInteger(vertexCount) || vertexCount < 1 || result.jointIndices.length !== vertexCount * 4 || result.jointWeights.length !== vertexCount * 4 || result.targetVertexCount !== vertexCount || !Number.isFinite(result.meanDistance) || result.meanDistance < 0 || !Number.isFinite(result.maxDistance) || result.maxDistance < 0 || result.distances != null && (!isIterableBuffer(result.distances) || !isSupportedNumericArray(result.distances) || result.distances.length !== vertexCount || hasInvalidValue(result.distances, (value) => !Number.isFinite(value) || value < 0))) throw new LuciaError('LUCIA_SKIN_TRANSFER_RESULT', 'Skin transfer worker returned malformed, untyped, or invalid result statistics or influence buffers.');
  for (let vertex = 0; vertex < vertexCount; vertex++) {
    let total = 0;
    for (let influence = 0; influence < 4; influence++) { const offset = vertex * 4 + influence, joint = result.jointIndices[offset], weight = result.jointWeights[offset]; if (!Number.isInteger(joint) || joint < 0 || boneCount != null && joint >= boneCount || !Number.isFinite(weight) || weight < 0) throw new LuciaError('LUCIA_SKIN_TRANSFER_RESULT', 'Skin transfer worker returned an invalid joint index or weight.'); total += weight; }
    if (!Number.isFinite(total) || total <= 1e-8 || Math.abs(total - 1) > 1e-4) throw new LuciaError('LUCIA_SKIN_TRANSFER_RESULT', 'Skin transfer worker returned non-normalized vertex influences.');
  }
  return result;
}

export function validateBakeMeshData(geometry) {
  validateGeometryData(geometry);
  const position = geometry.attributes.position, uv = geometry?.attributes?.uv;
  if (!uv?.array || !Number.isInteger(uv.count) || uv.count !== position.count || uv.array.length !== uv.count * 2 || (uv.itemSize != null && uv.itemSize !== 2)) throw new LuciaError('LUCIA_BAKE_UV_MISMATCH', 'UV data must have exactly two finite components per position.');
  if (hasInvalidValue(uv.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_BAKE_UV_INVALID', 'The selected mesh contains non-finite UV coordinates.');
  return true;
}

async function computeTangentsInWorker(data, owner, onProgress) {
  onProgress = normalizeOperationProgressCallback(onProgress, 'Computing tangent frame…');
  const expectedLength = data.positions.length / 3 * 4;
  return await new Promise((resolve, reject) => {
    owner.cancel(); owner.workerReject = reject;
    const worker = owner.worker = new Worker(new URL('./tangent-worker.js', import.meta.url), { type: 'module' });
    const finish = () => { if (owner.worker === worker) { worker.terminate(); owner.worker = null; owner.workerReject = null; } };
    worker.onerror = (event) => { if (owner.workerReject === reject) owner.workerReject = null; finish(); reject(new LuciaError('LUCIA_TANGENT_WORKER', event.message)); };
    worker.onmessage = ({ data: result }) => { if (owner.worker !== worker) return; if (result.type === 'result') { try { const tangents = validateWorkerVector(result.tangents, expectedLength, 'LUCIA_TANGENT_RESULT', 'Tangent'); finish(); resolve(tangents); } catch (error) { finish(); reject(error); } } else if (result.type === 'error') { finish(); reject(new LuciaError('LUCIA_TANGENT_WORKER', result.message)); } };
    onProgress?.({ percentage: 55, message: 'Computing tangent frame…' });
    postWorkerMessage(owner, worker, data, [data.positions.buffer, data.indices.buffer, data.uvs.buffer, data.normals.buffer], reject, 'LUCIA_TANGENT_WORKER');
  });
}

export function normalizeUnwrapOptions(options = {}) {
  options = options || {};
  const number = (value, fallback, min, max) => Number.isFinite(Number(value)) ? Math.max(min, Math.min(max, Number(value))) : fallback;
  const presets = { balanced: { padding: 2, texelsPerUnit: 0, maxChartSize: 0, rotateCharts: true, textureSeamWeight: 0 }, lightmap: { padding: 4, texelsPerUnit: 0, maxChartSize: 0, rotateCharts: true, textureSeamWeight: 2 }, quality: { padding: 4, texelsPerUnit: 0, maxChartSize: 0, rotateCharts: true, textureSeamWeight: 10 } };
  const presetName = presets[options.preset] ? options.preset : 'balanced';
  const preset = presets[presetName];
  const normalized = { resolution: Math.round(number(options.resolution, 1024, 64, 8192)), padding: Math.round(number(options.padding, preset.padding, 0, 256)), texelsPerUnit: number(options.texelsPerUnit, preset.texelsPerUnit, 0, 1e8), maxChartSize: Math.round(number(options.maxChartSize, preset.maxChartSize, 0, 8192)), rotateChartsToAxis: options.rotateChartsToAxis !== false, rotateCharts: options.rotateCharts == null ? preset.rotateCharts : options.rotateCharts !== false, blockAlign: options.blockAlign === true, bruteForce: options.bruteForce === true, singleAtlasFallback: options.singleAtlasFallback !== false };
  if (options.maxIterations != null) normalized.maxIterations = Math.round(number(options.maxIterations, 1, 1, 10000));
  if (options.maxCost != null) normalized.maxCost = number(options.maxCost, 2, 0, 100);
  if (options.preset != null) normalized.preset = presetName;
  if (options.textureSeamWeight != null || options.preset != null) normalized.textureSeamWeight = number(options.textureSeamWeight, preset.textureSeamWeight, 0, 100);
  return normalized;
}

export function normalizeCleanupOptions(options = {}) {
  options = options || {};
  const presets = { preserve: 0, 'game-ready': 0.0001, 'physics-ready': 0.001, aggressive: 0.005 };
  const preset = presets[options.preset] == null ? 'game-ready' : options.preset;
  const value = options.tolerance == null ? presets[preset] : Number(options.tolerance);
  const minArea = Number(options.minArea), minComponentArea = Number(options.minComponentArea), minComponentVolume = Number(options.minComponentVolume);
  return { preset, tolerance: Number.isFinite(value) ? Math.max(0, Math.min(0.1, value)) : presets[preset], minArea: Number.isFinite(minArea) ? Math.max(0, Math.min(1, minArea)) : 0, minComponentArea: Number.isFinite(minComponentArea) ? Math.max(0, Math.min(1, minComponentArea)) : 0, minComponentVolume: Number.isFinite(minComponentVolume) ? Math.max(0, Math.min(1, minComponentVolume)) : 0, ...(options.fillPlanarHoles === true ? { fillPlanarHoles: true } : {}) };
}

export function validateUVAtlasResult(result) {
  if (!isIterableBuffer(result?.positions) || !isSupportedNumericArray(result.positions) || !isIterableBuffer(result?.uvs) || !isSupportedNumericArray(result.uvs) || !isIterableBuffer(result?.indices) || !isSupportedNumericArray(result.indices) || result.positions.length < 3 || result.positions.length % 3 || result.uvs.length !== result.positions.length / 3 * 2 || result.indices.length < 3 || result.indices.length % 3) throw new LuciaError('LUCIA_UV_RESULT_SHAPE', 'UV atlas returned mismatched mesh or untyped mesh buffers.');
  if ((result.width != null && (!Number.isInteger(result.width) || result.width < 1 || result.width > 8192)) || (result.height != null && (!Number.isInteger(result.height) || result.height < 1 || result.height > 8192))) throw new LuciaError('LUCIA_UV_RESULT_ATLAS', 'UV atlas returned invalid dimensions.');
  if (result.atlasCount != null && (!Number.isInteger(result.atlasCount) || result.atlasCount < 1)) throw new LuciaError('LUCIA_UV_RESULT_ATLAS', 'UV atlas returned an invalid atlas count.');
  if (result.chartCount != null && (!Number.isInteger(result.chartCount) || result.chartCount < 1)) throw new LuciaError('LUCIA_UV_RESULT_ATLAS', 'UV atlas returned an invalid chart count.');
  if (result.singleAtlasFallback != null && typeof result.singleAtlasFallback !== 'boolean') throw new LuciaError('LUCIA_UV_RESULT_ATLAS', 'UV atlas returned an invalid fallback marker.');
  if (result.atlasCount > 1) throw new LuciaError('LUCIA_UV_MULTI_ATLAS', `xatlas produced ${result.atlasCount} sub-atlases; Lucia requires a single atlas for one primvars:st set.`);
  const vertexCount = result.positions.length / 3;
  if (result.sourceVertexCount != null && (!Number.isSafeInteger(result.sourceVertexCount) || result.sourceVertexCount < 1)) throw new LuciaError('LUCIA_UV_RESULT_REMAP', 'UV atlas returned an invalid source vertex count.');
  if (Boolean(result.jointIndices) !== Boolean(result.jointWeights)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', 'UV atlas must return joint indices and weights together.');
  for (const value of result.positions) if (!Number.isFinite(value)) throw new LuciaError('LUCIA_UV_RESULT_FINITE', 'UV atlas returned non-finite positions or coordinates.');
  for (const value of result.uvs) if (!Number.isFinite(value) || value < -1e-6 || value > 1 + 1e-6) throw new LuciaError('LUCIA_UV_RESULT_UV', 'UV atlas returned non-finite or out-of-range coordinates.');
  for (const index of result.indices) if (!Number.isInteger(index) || index < 0 || index >= vertexCount) throw new LuciaError('LUCIA_UV_RESULT_INDEX', 'UV atlas returned an out-of-range index.');
  if (!isIterableBuffer(result.xref) || !isSupportedNumericArray(result.xref) || result.xref.length !== vertexCount) throw new LuciaError('LUCIA_UV_RESULT_REMAP', 'UV atlas must return one typed source remap index per output vertex.');
  for (const index of result.xref) if (!Number.isSafeInteger(index) || index < 0 || index > 0xffffffff || (Number.isInteger(result.sourceVertexCount) && index >= result.sourceVertexCount)) throw new LuciaError('LUCIA_UV_RESULT_REMAP', 'UV atlas source remap contains an out-of-range vertex.');
  for (const name of ['chartIndices', 'atlasIndices']) if (result[name] != null && (!isIterableBuffer(result[name]) || !isSupportedNumericArray(result[name]) || result[name].length !== vertexCount)) throw new LuciaError('LUCIA_UV_RESULT_ATLAS', `UV atlas ${name} must be typed and match the output vertex count.`);
  if (result.chartIndices != null && !Number.isInteger(result.chartCount)) throw new LuciaError('LUCIA_UV_RESULT_ATLAS', 'UV atlas chart indices require a chart count.');
  if (result.atlasIndices != null && !Number.isInteger(result.atlasCount)) throw new LuciaError('LUCIA_UV_RESULT_ATLAS', 'UV atlas sub-atlas indices require an atlas count.');
  if (result.chartIndices && Number.isInteger(result.chartCount)) for (const index of result.chartIndices) if (!Number.isInteger(index) || index < -1 || index >= result.chartCount) throw new LuciaError('LUCIA_UV_RESULT_ATLAS', 'UV atlas returned an out-of-range chart index.');
  if (result.atlasIndices && Number.isInteger(result.atlasCount)) for (const index of result.atlasIndices) if (!Number.isInteger(index) || index < -1 || index >= result.atlasCount) throw new LuciaError('LUCIA_UV_RESULT_ATLAS', 'UV atlas returned an out-of-range sub-atlas index.');
  for (const [name, stride] of [['normals', 3], ['colors', 3], ['tangents', 4], ['jointIndices', 4], ['jointWeights', 4]]) {
    if (result[name] == null) continue;
    if (!isIterableBuffer(result[name]) || !isSupportedNumericArray(result[name]) || result[name].length !== vertexCount * stride) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', `UV atlas returned an invalid typed ${name} buffer.`);
    for (const value of result[name]) if (!Number.isFinite(value)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', `UV atlas returned a non-finite ${name} value.`);
  }
  if (result.jointIndices && hasInvalidValue(result.jointIndices, (value) => !Number.isSafeInteger(value) || value < 0 || value > 65535)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', 'UV atlas returned invalid joint indices.');
  if (result.jointWeights && hasInvalidValue(result.jointWeights, (value) => value < 0)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', 'UV atlas returned negative joint weights.');
  if (result.customAttributes != null && !Array.isArray(result.customAttributes)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', 'UV atlas returned a malformed custom attribute collection.');
  const reservedCustomNames = new Set(['position', 'normal', 'uv', 'color', 'tangent', 'skinIndex', 'skinWeight', 'st', 'st1', 'displayColor', 'tangents', 'skel:jointIndices', 'skel:jointWeights']);
  const customNames = new Set();
  for (const attribute of result.customAttributes || []) {
    if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || !Number.isInteger(attribute?.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || !isIterableBuffer(attribute?.array) || !isSupportedNumericArray(attribute.array) || attribute.array.length !== vertexCount * attribute.itemSize) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', 'UV atlas returned an invalid custom attribute buffer.');
    if (reservedCustomNames.has(attribute.name)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', `UV atlas returned a reserved custom attribute name: ${attribute.name}`);
    for (const value of attribute.array) if (!Number.isFinite(value)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', `UV atlas returned non-finite ${attribute.name} values.`);
    if (customNames.has(attribute.name)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', `UV atlas returned duplicate custom attribute name: ${attribute.name}`);
    customNames.add(attribute.name);
    for (const value of attribute.array) if (!Number.isFinite(value)) throw new LuciaError('LUCIA_UV_RESULT_ATTRIBUTE', 'UV atlas returned a non-finite custom attribute.');
  }
  return result;
}

export function validateRetopoResult(result) {
  if (!isIterableBuffer(result?.positions) || !isSupportedNumericArray(result.positions) || !isIterableBuffer(result?.indices) || !isSupportedNumericArray(result.indices) || result.positions.length % 3 || result.indices.length % 3 || !result.indices.length) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology returned mismatched or empty, or untyped, mesh buffers.');
  const vertexCount = result.positions.length / 3;
  if (Boolean(result.jointIndices) !== Boolean(result.jointWeights)) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology must return joint indices and weights together.');
  if (result.sourceVertexCount != null && (!Number.isSafeInteger(result.sourceVertexCount) || result.sourceVertexCount < 1)) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology returned an invalid source vertex count.');
  if (result.xref != null && (!isIterableBuffer(result.xref) || !isSupportedNumericArray(result.xref) || result.xref.length !== vertexCount)) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology source remap must be typed and match the output vertex count.');
  if (result.xref) { const sourceVertexCount = Number.isInteger(result.sourceVertexCount) ? result.sourceVertexCount : Infinity; for (const index of result.xref) if (!Number.isInteger(index) || index < 0 || index >= sourceVertexCount) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology source remap contains an out-of-range vertex.'); }
  for (const value of result.positions) if (!Number.isFinite(value)) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology returned non-finite positions.');
  for (const index of result.indices) if (!Number.isInteger(index) || index < 0 || index >= vertexCount) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology returned an out-of-range index.');
  for (const [name, stride] of [['normals', 3], ['uvs', 2], ['colors', 3], ['tangents', 4], ['jointIndices', 4], ['jointWeights', 4]]) if (result[name] != null) { if (!isIterableBuffer(result[name]) || !isSupportedNumericArray(result[name]) || result[name].length !== vertexCount * stride || hasInvalidValue(result[name], (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_RETOPO_RESULT', `Retopology returned an invalid typed ${name} buffer.`); }
  if (result.jointIndices && hasInvalidValue(result.jointIndices, (value) => !Number.isSafeInteger(value) || value < 0 || value > 65535)) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology returned invalid joint indices.');
  if (result.jointWeights && hasInvalidValue(result.jointWeights, (value) => value < 0)) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology returned negative joint weights.');
  if (result.customAttributes != null && !Array.isArray(result.customAttributes)) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology returned a malformed custom attribute collection.');
  const customNames = new Set();
  for (const attribute of result.customAttributes || []) { if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || customNames.has(attribute.name) || !isIterableBuffer(attribute?.array) || !isSupportedNumericArray(attribute.array) || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length !== vertexCount * attribute.itemSize || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_RETOPO_RESULT', 'Retopology returned an invalid or duplicate custom attribute.'); customNames.add(attribute.name); }
  if (result.groups) consolidateMaterialGroups(result.groups, result.indices.length);
  return result;
}

export function validateMeshCleanupResult(result) {
  if (!isIterableBuffer(result?.positions) || !isSupportedNumericArray(result.positions) || !isIterableBuffer(result?.indices) || !isSupportedNumericArray(result.indices) || result.positions.length % 3 || result.indices.length % 3 || !result.indices.length) throw new LuciaError('LUCIA_CLEANUP_RESULT_SHAPE', 'Mesh cleanup returned mismatched or empty buffers (untyped buffers are rejected).');
  const vertexCount = result.positions.length / 3;
  if (Boolean(result.jointIndices) !== Boolean(result.jointWeights)) throw new LuciaError('LUCIA_CLEANUP_RESULT_ATTRIBUTE', 'Mesh cleanup must return joint indices and weights together.');
  if (result.uvIndices != null && (!isIterableBuffer(result.uvIndices) || !isSupportedNumericArray(result.uvIndices) || !isIterableBuffer(result.uvs) || !isSupportedNumericArray(result.uvs))) throw new LuciaError('LUCIA_CLEANUP_RESULT_ATTRIBUTE', 'Mesh cleanup returned UV indices without a valid typed UV value buffer.');
  for (const value of result.positions) if (!Number.isFinite(value)) throw new LuciaError('LUCIA_CLEANUP_RESULT_FINITE', 'Mesh cleanup returned non-finite positions.');
  for (const index of result.indices) if (!Number.isInteger(index) || index < 0 || index >= vertexCount) throw new LuciaError('LUCIA_CLEANUP_RESULT_INDEX', 'Mesh cleanup returned an out-of-range index.');
  for (const [name, stride] of [['normals', 3], ['colors', 3], ['tangents', 4], ['jointIndices', 4], ['jointWeights', 4]]) if (result[name] != null) { if (!isIterableBuffer(result[name]) || result[name].length !== vertexCount * stride || hasInvalidValue(result[name], (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_CLEANUP_RESULT_ATTRIBUTE', `Mesh cleanup returned an invalid ${name} buffer.`); }
  if (result.sharpEdges != null) { try { validateSharpEdges(result.sharpEdges, vertexCount); } catch (error) { throw new LuciaError('LUCIA_CLEANUP_RESULT_SHARP_EDGES', error.message); } if (!Array.isArray(result.sharpEdgeSharpness) || result.sharpEdgeSharpness.length !== result.sharpEdges.length || result.sharpEdgeSharpness.some((value) => !Number.isFinite(Number(value)) || Number(value) <= 0)) throw new LuciaError('LUCIA_CLEANUP_RESULT_SHARP_EDGES', 'Mesh cleanup returned sharp edges without aligned positive finite sharpness values.'); }
  if (result.uvs != null && (!isIterableBuffer(result.uvs) || !isSupportedNumericArray(result.uvs) || result.uvIndices == null && result.uvs.length !== vertexCount * 2 || result.uvIndices != null && (result.uvs.length < 2 || result.uvs.length % 2 || result.uvIndices.length !== result.indices.length || hasInvalidValue(result.uvIndices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= result.uvs.length / 2)) || hasInvalidValue(result.uvs, (value) => !Number.isFinite(value)))) throw new LuciaError('LUCIA_CLEANUP_RESULT_ATTRIBUTE', 'Mesh cleanup returned invalid uvs buffers (typed buffers are required).');
  if (result.jointIndices && hasInvalidValue(result.jointIndices, (value) => !Number.isSafeInteger(value) || value < 0 || value > 65535)) throw new LuciaError('LUCIA_CLEANUP_RESULT_ATTRIBUTE', 'Mesh cleanup returned invalid joint indices.');
  if (result.jointWeights) for (let vertex = 0; vertex < vertexCount; vertex++) { const offset = vertex * 4, total = result.jointWeights[offset] + result.jointWeights[offset + 1] + result.jointWeights[offset + 2] + result.jointWeights[offset + 3]; if ([0, 1, 2, 3].some((influence) => result.jointWeights[offset + influence] < 0) || Math.abs(total - 1) > 1e-4) throw new LuciaError('LUCIA_CLEANUP_RESULT_ATTRIBUTE', 'Mesh cleanup returned non-normalized joint weights.'); }
  if (result.customAttributes != null && !Array.isArray(result.customAttributes) || result.faceVaryingAttributes != null && !Array.isArray(result.faceVaryingAttributes) || result.groups != null && !Array.isArray(result.groups)) throw new LuciaError('LUCIA_CLEANUP_RESULT_ATTRIBUTE', 'Mesh cleanup returned malformed attribute or material-group collections.');
  const customNames = new Set();
  for (const attribute of result.customAttributes || []) { if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || customNames.has(attribute.name) || !isIterableBuffer(attribute?.array) || !isSupportedNumericArray(attribute.array) || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length !== vertexCount * attribute.itemSize || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_CLEANUP_RESULT_ATTRIBUTE', 'Mesh cleanup returned an invalid or duplicate custom attribute buffer.'); customNames.add(attribute.name); }
  for (const attribute of result.faceVaryingAttributes || []) { if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || customNames.has(attribute.name) || !isIterableBuffer(attribute?.array) || !isSupportedNumericArray(attribute.array) || !isIterableBuffer(attribute?.indices) || !isSupportedNumericArray(attribute.indices) || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length % attribute.itemSize || attribute.indices.length !== result.indices.length || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value)) || hasInvalidValue(attribute.indices, (index) => !Number.isInteger(index) || index < 0 || index >= attribute.array.length / attribute.itemSize)) throw new LuciaError('LUCIA_CLEANUP_RESULT_FACEVARYING', 'Mesh cleanup returned invalid or duplicate face-varying primvar buffers.'); customNames.add(attribute.name); }
  for (const group of result.groups || []) if (!Number.isInteger(group?.start) || !Number.isInteger(group?.count) || !Number.isInteger(group?.materialIndex) || group.start < 0 || group.count <= 0 || group.start + group.count > result.indices.length || group.start % 3 || group.count % 3 || group.materialIndex < 0) throw new LuciaError('LUCIA_CLEANUP_RESULT_GROUP', 'Mesh cleanup returned invalid material groups.');
  try { validateMeshMaterialGroups(result.groups || [], result.indices.length); } catch (error) { throw new LuciaError('LUCIA_CLEANUP_RESULT_GROUP', error.message); }
  return result;
}

function copySkinAttributes(geometry, vertexCount, code) {
  const skinIndex = geometry.attributes.skinIndex, skinWeight = geometry.attributes.skinWeight;
  if (!skinIndex && !skinWeight) return { jointIndices: null, jointWeights: null };
  if (!skinIndex?.array || !skinWeight?.array || skinIndex.itemSize !== 4 || skinWeight.itemSize !== 4 || skinIndex.array.length !== vertexCount * 4 || skinWeight.array.length !== vertexCount * 4) throw new LuciaError(code, 'skinIndex and skinWeight must be complete four-influence vertex attributes.');
  if (!isSupportedNumericArray(skinIndex.array) || !isSupportedNumericArray(skinWeight.array)) throw new LuciaError(code, 'skinIndex and skinWeight must use supported numeric typed arrays.');
  if (hasInvalidValue(skinIndex.array, (value) => !Number.isSafeInteger(value) || value < 0 || value > 65535)) throw new LuciaError(code, 'skinIndex must contain non-negative 16-bit integer joint indices.');
  if (hasInvalidValue(skinWeight.array, (value) => !Number.isFinite(value) || value < 0)) throw new LuciaError(code, 'skinWeight must contain finite non-negative values.');
  return { jointIndices: new Uint16Array(skinIndex.array), jointWeights: new Float32Array(skinWeight.array) };
}

export function validateMeshMaterialGroups(groups, indexCount) {
  if (!Array.isArray(groups)) throw new LuciaError('LUCIA_CLEANUP_GROUP', 'Mesh material groups must be an array.');
  if (!Number.isSafeInteger(indexCount) || indexCount < 0 || indexCount % 3) throw new LuciaError('LUCIA_CLEANUP_GROUP', 'Material groups require a non-negative triangle-aligned index count.');
  const ordered = groups.map((group) => ({ start: group?.start, count: group?.count, materialIndex: group?.materialIndex })).sort((a, b) => (a.start || 0) - (b.start || 0));
  let end = 0;
  for (const group of ordered) {
    if (!Number.isInteger(group.start) || !Number.isInteger(group.count) || !Number.isInteger(group.materialIndex) || group.start < 0 || group.count <= 0 || group.start % 3 || group.count % 3 || group.start + group.count > indexCount || group.start < end || group.materialIndex < 0) throw new LuciaError('LUCIA_CLEANUP_GROUP', 'Mesh material groups overlap or fall outside the index buffer.');
    end = group.start + group.count;
  }
  return ordered;
}

export function consolidateMaterialGroups(groups, indexCount) {
  const ordered = validateMeshMaterialGroups(groups, indexCount);
  if (!ordered.length) return [];
  if (ordered[0].start !== 0 || ordered.at(-1).start + ordered.at(-1).count !== indexCount || ordered.some((group, index) => index > 0 && group.start !== ordered[index - 1].start + ordered[index - 1].count)) throw new LuciaError('LUCIA_CLEANUP_GROUP', 'Material groups must cover the complete ordered index buffer.');
  return ordered.reduce((result, group) => {
    const previous = result.at(-1);
    if (previous && previous.materialIndex === group.materialIndex && previous.start + previous.count === group.start) previous.count += group.count;
    else result.push({ ...group });
    return result;
  }, []);
}

export function reorderMaterialGroups(groups, indices) {
  const source = indices instanceof Uint32Array ? indices : Uint32Array.from(indices || []), ordered = validateMeshMaterialGroups(groups, source.length);
  if (!ordered.length) return { indices: new Uint32Array(source), groups: [] };
  if (ordered[0].start !== 0 || ordered.at(-1).start + ordered.at(-1).count !== source.length || ordered.some((group, index) => index > 0 && group.start !== ordered[index - 1].start + ordered[index - 1].count)) throw new LuciaError('LUCIA_CLEANUP_GROUP', 'Material groups must cover the complete ordered index buffer.');
  const ranked = ordered.map((group, order) => ({ ...group, order })).sort((a, b) => a.materialIndex - b.materialIndex || a.order - b.order), output = new Uint32Array(source.length), result = [];
  let cursor = 0;
  for (const group of ranked) {
    output.set(source.subarray(group.start, group.start + group.count), cursor);
    const previous = result.at(-1);
    if (previous && previous.materialIndex === group.materialIndex) previous.count += group.count;
    else result.push({ start: cursor, count: group.count, materialIndex: group.materialIndex });
    cursor += group.count;
  }
  return { indices: output, groups: result };
}

export function transferXatlasMaterialGroups(groups, sourceIndices, resultIndices, xref) {
  const ordered = validateMeshMaterialGroups(groups, sourceIndices.length);
  if (!isIterableBuffer(resultIndices) || !isSupportedNumericArray(resultIndices) || resultIndices.length !== sourceIndices.length || !isIterableBuffer(xref) || !isSupportedNumericArray(xref) || !xref.length || hasInvalidValue(resultIndices, (value) => !Number.isSafeInteger(value) || value < 0 || value >= xref.length) || hasInvalidValue(xref, (value) => !Number.isSafeInteger(value) || value < 0)) throw new LuciaError('LUCIA_UV_MATERIALS', 'xatlas changed face topology or returned an invalid source remap (typed buffers are required); material subset ranges cannot be transferred safely.');
  for (let offset = 0; offset < sourceIndices.length; offset += 3) {
    const sourceFace = [sourceIndices[offset], sourceIndices[offset + 1], sourceIndices[offset + 2]].sort((a, b) => a - b);
    const outputFace = [xref[resultIndices[offset]], xref[resultIndices[offset + 1]], xref[resultIndices[offset + 2]]].sort((a, b) => a - b);
    if (sourceFace.some((value, index) => value !== outputFace[index])) throw new LuciaError('LUCIA_UV_MATERIALS', 'xatlas reordered or changed faces; material subset ranges cannot be transferred safely.');
  }
  return ordered;
}

export class LuciaOperations {
  constructor(session, project, renderBridge) { this.session = session; this.project = project; this.renderBridge = renderBridge; this.worker = null; this.workerReject = null; this.lastStats = null; }
  setUVSeamLockPolicy(value) { if (typeof value !== 'boolean') throw new LuciaError('LUCIA_RETOPO_SEAMS', 'lockUVSeams must be boolean.'); this.lockUVSeamsPolicy = value; return value; }

  async extractSelectedToReference(path, assetPath = null) {
    if (typeof path !== 'string' || !path.startsWith('/') || path === '/') throw new LuciaError('LUCIA_EXTRACT_PATH', 'Extract requires a non-root USD prim path.');
    const block = this.session.usda && findPrimBlock(this.session.usda, path);
    if (!block) throw new LuciaError('LUCIA_EXTRACT_PATH', `Prim not found: ${path}.`);
    const requested = assetPath || `layers/${path.split('/').at(-1)}.usda`;
    if (!/^(?!\/)(?!.*(?:^|\/)\.\.(?:\/|$))[A-Za-z0-9_./-]+\.usda$/i.test(requested) || requested.includes('\\') || requested.includes('//')) throw new LuciaError('LUCIA_EXTRACT_ASSET', 'Extracted layer path must be a safe package-relative .usda path.');
    if (this.project.assets.has(requested)) throw new LuciaError('LUCIA_EXTRACT_COLLISION', `Package asset already exists: ${requested}`);
    const source = this.session.usda, declarationStart = source.lastIndexOf('\n', block.open) + 1, declaration = source.slice(declarationStart, block.open).trim();
    if (!/^\s*(?:def|over|class)\s+[A-Za-z_][\w:]*/.test(declaration)) throw new LuciaError('LUCIA_EXTRACT_DECLARATION', `Prim declaration is not safely extractable: ${path}.`);
    const body = source.slice(block.start, block.close), rootName = path.split('/').at(-1);
    const layer = `#usda 1.0\n( defaultPrim = "${rootName}" )\n${declaration} {${body}}`;
    const replacement = `${source.slice(0, declarationStart)}${declaration}${declaration.endsWith(')') ? ` references = @${requested}@ )` : ` ( references = @${requested}@ )`} { }${source.slice(block.close + 1)}`;
    this.project.assets.set(requested, { bytes: encoder.encode(layer), mime: 'text/plain', generated: true, sourcePath: path, kind: 'usd-layer' });
    try { await this.session.replaceUSDA(replacement, `Extract ${path} to ${requested}`); return { assetPath: requested, bytes: encoder.encode(layer) }; }
    catch (error) { this.project.assets.delete(requested); throw error; }
  }
  async flattenSelectedReference(path) {
    if (typeof path !== 'string' || !path.startsWith('/') || path === '/') throw new LuciaError('LUCIA_FLATTEN_PATH', 'Flatten requires a non-root USD prim path.');
    const source = this.session.usda, block = findPrimBlock(source, path);
    if (!block) throw new LuciaError('LUCIA_FLATTEN_PATH', `Prim not found: ${path}.`);
    const declarationStart = source.lastIndexOf('\n', block.open) + 1, declaration = source.slice(declarationStart, block.open), reference = declaration.match(/@([^@]+\.usda)@/i);
    if (!reference) throw new LuciaError('LUCIA_FLATTEN_REFERENCE', `Selected prim has no package-local USDA reference: ${path}.`);
    const assetPath = reference[1], asset = this.project.assets.get(assetPath), bytes = asset?.bytes;
    if (!/^(?!\/)(?!.*(?:^|\/)\.\.(?:\/|$))[A-Za-z0-9_./-]+\.usda$/i.test(assetPath) || assetPath.includes('\\') || assetPath.includes('//')) throw new LuciaError('LUCIA_FLATTEN_REFERENCE', `Reference is not a safe package-relative USDA path: ${assetPath}.`);
    if (!bytes || !(bytes instanceof Uint8Array)) throw new LuciaError('LUCIA_FLATTEN_ASSET', `Referenced USDA layer is not loaded: ${assetPath}.`);
    const layer = decoder.decode(bytes), name = path.split('/').at(-1), layerBlock = findPrimBlock(layer, `/${name}`);
    if (!layerBlock) throw new LuciaError('LUCIA_FLATTEN_LAYER', `Referenced layer does not contain root prim ${name}.`);
    const layerDeclarationStart = layer.lastIndexOf('\n', layerBlock.open) + 1, flattened = layer.slice(layerDeclarationStart, layerBlock.close + 1);
    return this.session.replaceUSDA(`${source.slice(0, declarationStart)}${flattened}${source.slice(block.close + 1)}`, `Flatten ${path} from ${assetPath}`);
  }
  textureReferences() {
    const refs = [], re = /@([^@]+)@/g; let match;
    while ((match = re.exec(this.session.usda))) {
      const context = this.session.usda.slice(Math.max(0, match.index - 120), match.index), kinds = [...context.matchAll(/\b(references|payload|subLayers|asset)\b/gi)], kind = kinds.at(-1)?.[1].toLowerCase() || 'asset';
      if (kind !== 'asset') continue;
      refs.push({ assetPath: match[1].trim(), offset: match.index, kind });
    }
    return refs;
  }
  async renameTexture(oldPath, newPath) {
    if (typeof oldPath !== 'string' || !oldPath || typeof newPath !== 'string' || !/^[A-Za-z0-9_./-]+$/.test(newPath) || newPath.includes('..') || newPath.startsWith('/') || newPath.includes('\\') || newPath.includes('//')) throw new LuciaError('LUCIA_ASSET_PATH', 'Texture paths must be safe package-relative paths.');
    if (this.project.assets.has(newPath) && newPath !== oldPath) throw new LuciaError('LUCIA_ASSET_COLLISION', `An asset already exists at ${newPath}.`);
    const exact = new RegExp(`@${escapeRegExp(oldPath)}@`, 'g');
    if (!exact.test(this.session.usda)) throw new LuciaError('LUCIA_ASSET_NOT_FOUND', `No exact USD asset reference uses ${oldPath}.`);
    const source = this.session.usda.replace(exact, `@${newPath}@`);
    const previous = await this.session.replaceUSDA(source, `Rename texture ${oldPath} to ${newPath}`);
    if (this.project.assets.has(oldPath)) { const asset = this.project.assets.get(oldPath); this.project.assets.delete(oldPath); this.project.assets.set(newPath, asset); }
    this.project.exportRemap[oldPath] = newPath;
    return previous;
  }
  async localizeDependencies(mapping) {
    const result = localizeUSDDependencies(this.session.usda, mapping), entries = result.mappings;
    if (!result.changed) throw new LuciaError('LUCIA_DEPENDENCY_LOCALIZATION_NOOP', 'No exact USD asset references matched the requested localization map.');
    const sourcePaths = new Set(entries.map(({ from }) => from));
    for (const { to } of entries) if (this.project.assets.has(to) && !sourcePaths.has(to)) throw new LuciaError('LUCIA_ASSET_COLLISION', `An asset already exists at ${to}.`);
    const previous = await this.session.replaceUSDA(result.source, `Localize ${result.changed} USD dependenc${result.changed === 1 ? 'y' : 'ies'}`);
    const originalAssets = this.project.assets, localizedAssets = new Map(originalAssets);
    for (const { from } of entries) localizedAssets.delete(from);
    for (const { from, to } of entries) if (originalAssets.has(from)) localizedAssets.set(to, originalAssets.get(from));
    this.project.assets = localizedAssets;
    for (const { from, to } of entries) this.project.exportRemap[from] = to;
    return previous;
  }
  async packChannels({ name = 'textures/packed.png', channels = {}, outputChannels = 4, colorSpace = 'linear' } = {}) {
    if (typeof name !== 'string' || !/^(?!\/)(?!.*(?:^|\/)\.\.(?:\/|$))[A-Za-z0-9_./-]+\.png$/i.test(name) || name.includes('\\') || name.includes('//')) throw new LuciaError('LUCIA_ASSET_PATH', 'Packed texture output must be a safe package-relative .png path.');
    if (this.project.assets.has(name)) throw new LuciaError('LUCIA_ASSET_COLLISION', `An asset already exists at ${name}.`);
    if (!Number.isSafeInteger(outputChannels) || outputChannels < 1 || outputChannels > 4) throw new LuciaError('LUCIA_PACK_CHANNELS', 'Packed output channels must be a safe integer from 1 through 4.');
    if (!['linear', 'srgb', 'raw'].includes(colorSpace)) throw new LuciaError('LUCIA_PACK_COLOR_SPACE', 'Packed texture color space must be linear, srgb, or raw.');
    if (!channels || Array.isArray(channels) || typeof channels !== 'object') throw new LuciaError('LUCIA_PACK_CHANNELS', 'Packed channels must be an object containing r/g/b/a slots.');
    if (Object.keys(channels).some((slot) => !['r', 'g', 'b', 'a'].includes(slot))) throw new LuciaError('LUCIA_PACK_CHANNELS', 'Packed channels must use only r/g/b/a slots.');
    if (Object.entries(channels).some(([slot, value]) => value != null && ['r', 'g', 'b', 'a'].indexOf(slot) >= outputChannels)) throw new LuciaError('LUCIA_PACK_CHANNELS', 'Packed channels cannot specify sources beyond the requested output channel count.');
    const options = { channels: Math.max(1, Math.min(4, Number(outputChannels) || 4)), format: 'png' }, packedSlots = {};
    for (const slot of ['r', 'g', 'b', 'a']) {
      const value = channels[slot];
      if (value == null) continue;
      const descriptor = typeof value === 'string' ? { path: value } : value;
      if (!descriptor || typeof descriptor.path !== 'string' || descriptor.channel != null && (!Number.isSafeInteger(descriptor.channel) || descriptor.channel < 0 || descriptor.channel > 3)) throw new LuciaError('LUCIA_PACK_CHANNELS', 'Packed channels must use safe integer source channels from 0 through 3.');
      const asset = this.project.assets.get(descriptor.path);
      if (!asset?.bytes?.length) throw new LuciaError('LUCIA_ASSET_NOT_FOUND', `No image asset is available at ${descriptor.path}.`);
      options[slot] = { data: asset.bytes, channel: Math.max(0, Math.min(3, Number(descriptor.channel) || 0)) };
      packedSlots[slot] = { path: descriptor.path, channel: options[slot].channel };
    }
    if (!Object.keys(packedSlots).length) throw new LuciaError('LUCIA_PACK_INPUT', 'Choose at least one source texture channel.');
    const native = new this.session.module.LightUSDLoaderNative();
    try {
      const result = native.repackChannels(options);
      validatePackedResult(result, outputChannels);
      this.project.assets.set(name, { bytes: new Uint8Array(result.data), mimeType: 'image/png', generated: true, colorSpace, dataTexture: colorSpace !== 'srgb', packedSlots, width: result.width, height: result.height, channels: result.channels });
      this.project.emit();
      // sessionCommand uses the operation return value as the USDA snapshot.
      // Packing changes only project assets, but returning the unchanged stage
      // text keeps undo/redo valid while project snapshots restore the asset.
      return this.session.exportUSDA();
    } finally { native.delete(); }
  }
  async stripUnusedAlpha(name) {
    const asset = this.project.assets.get(name);
    if (!asset?.bytes?.length) throw new LuciaError('LUCIA_ASSET_NOT_FOUND', `No image asset is available at ${name}.`);
    if (asset.bakeStats?.unusedAlpha !== true && asset.unusedAlpha !== true) throw new LuciaError('LUCIA_ALPHA_UNKNOWN', 'Alpha removal requires a deterministic opaque-alpha analysis result.');
    const native = new this.session.module.LightUSDLoaderNative();
    try {
      const result = native.repackChannels({ channels: 3, format: 'png', r: { data: asset.bytes, channel: 0 }, g: { data: asset.bytes, channel: 1 }, b: { data: asset.bytes, channel: 2 } });
      if (!result?.success || !isByteBuffer(result.data) || !result.data.length || !Number.isSafeInteger(result.width) || result.width < 1 || result.width > 8192 || !Number.isSafeInteger(result.height) || result.height < 1 || result.height > 8192 || result.channels !== 3) throw new LuciaError('LUCIA_ALPHA_STRIP', result?.error || 'LightUSD returned invalid encoded RGB texture data while removing the unused alpha channel.');
      this.project.assets.set(name, { ...asset, bytes: new Uint8Array(result.data), channels: 3, unusedAlpha: false, alphaRemoved: true, width: result.width, height: result.height });
      this.project.emit();
      return this.session.exportUSDA();
    } finally { native.delete(); }
  }
  async convertNormalMapY(name, from, to) {
    const asset = this.project.assets.get(name), source = String(from || asset?.normalY || '').toLowerCase(), target = String(to || '').toLowerCase();
    if (!asset?.bytes?.length) throw new LuciaError('LUCIA_ASSET_NOT_FOUND', `No image asset is available at ${name}.`);
    if (!['opengl', 'directx'].includes(source) || !['opengl', 'directx'].includes(target)) throw new LuciaError('LUCIA_NORMAL_CONVENTION', 'Normal-map convention must be OpenGL or DirectX.');
    if (source === target) return this.session.exportUSDA();
    if (typeof createImageBitmap !== 'function' || typeof document === 'undefined') throw new LuciaError('LUCIA_NORMAL_DECODE', 'Browser image decoding is unavailable for normal-map conversion.');
    const bitmap = await createImageBitmap(new Blob([asset.bytes], { type: asset.mimeType || 'image/png' })), canvas = document.createElement('canvas');
    canvas.width = bitmap.width; canvas.height = bitmap.height;
    const context = canvas.getContext('2d'); if (!context) throw new LuciaError('LUCIA_NORMAL_CANVAS', 'Could not create an image conversion canvas.');
    context.drawImage(bitmap, 0, 0); bitmap.close?.();
    const image = context.getImageData(0, 0, canvas.width, canvas.height), pixels = convertNormalMapY({ pixels: image.data, width: canvas.width, height: canvas.height, convention: 'directx' });
    image.data.set(pixels); context.putImageData(image, 0, 0);
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/png')); if (!blob) throw new LuciaError('LUCIA_NORMAL_ENCODE', 'Could not encode the converted normal map.');
    this.project.assets.set(name, { ...asset, bytes: new Uint8Array(await blob.arrayBuffer()), normalY: target, width: canvas.width, height: canvas.height });
    this.project.emit();
    return this.session.exportUSDA();
  }
  async resizeTexture(name, maxDimension = 4096) {
    const asset = this.project.assets.get(name);
    if (!asset?.bytes?.length) throw new LuciaError('LUCIA_ASSET_NOT_FOUND', `No image asset is available at ${name}.`);
    if (typeof maxDimension !== 'number' || !Number.isSafeInteger(maxDimension) || maxDimension < 1 || maxDimension > 8192) throw new LuciaError('LUCIA_TEXTURE_DIMENSION', 'Texture maximum dimension must be a safe integer from 1 through 8192.');
    if (typeof createImageBitmap !== 'function' || typeof document === 'undefined') throw new LuciaError('LUCIA_TEXTURE_DECODE', 'Browser image decoding is unavailable for texture resizing.');
    const bitmap = await createImageBitmap(new Blob([asset.bytes], { type: asset.mimeType || 'image/png' }), { colorSpaceConversion: 'none' }), sourceWidth = bitmap.width, sourceHeight = bitmap.height, limit = Math.max(1, Math.min(8192, Math.floor(Number(maxDimension) || 4096))), scale = Math.min(1, limit / Math.max(sourceWidth, sourceHeight)), width = Math.max(1, Math.round(sourceWidth * scale)), height = Math.max(1, Math.round(sourceHeight * scale));
    if (width === sourceWidth && height === sourceHeight) { bitmap.close?.(); return this.session.exportUSDA(); }
    const sourceCanvas = document.createElement('canvas'); sourceCanvas.width = sourceWidth; sourceCanvas.height = sourceHeight;
    const sourceContext = sourceCanvas.getContext('2d'); if (!sourceContext) { bitmap.close?.(); throw new LuciaError('LUCIA_TEXTURE_CANVAS', 'Could not create a texture decode context.'); }
    sourceContext.drawImage(bitmap, 0, 0); bitmap.close?.();
    const sourcePixels = sourceContext.getImageData(0, 0, sourceWidth, sourceHeight).data, resized = resizeBakeImage({ pixels: sourcePixels, fromWidth: sourceWidth, fromHeight: sourceHeight, width, height, maxResolution: limit, colorSpace: asset.colorSpace });
    const canvas = document.createElement('canvas'); canvas.width = width; canvas.height = height;
    const context = canvas.getContext('2d'); if (!context) throw new LuciaError('LUCIA_TEXTURE_CANVAS', 'Could not create a texture resize canvas.');
    context.putImageData(new ImageData(resized.pixels, width, height), 0, 0);
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/png')); if (!blob) throw new LuciaError('LUCIA_TEXTURE_ENCODE', 'Could not encode the resized texture.');
    this.project.assets.set(name, { ...asset, bytes: new Uint8Array(await blob.arrayBuffer()), mimeType: 'image/png', width, height, resizedFrom: { width: sourceWidth, height: sourceHeight }, resizeColorSpace: asset.colorSpace || 'linear' });
    this.project.emit();
    return this.session.exportUSDA();
  }
  async retopo(path, { tolerance = .0001, targetRatio = 1, targetError = 0, lockBorder = true, lockUVSeams = true, lockedVertices = null, sharpEdges = null, sharpChains = null, sharpChainSharpness = null, previewOnly = false } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Retopologizing mesh…');
    if (typeof lockBorder !== 'boolean') throw new LuciaError('LUCIA_RETOPO_BORDER', 'lockBorder must be boolean.');
    if (typeof lockUVSeams !== 'boolean') throw new LuciaError('LUCIA_RETOPO_SEAMS', 'lockUVSeams must be boolean.');
    if (sharpChains != null && sharpEdges != null) throw new LuciaError('LUCIA_RETOPO_SHARP_CHAINS', 'Provide sharpChains or sharpEdges, not both.');
    const object = this.renderBridge.objectForPath(path);
    const mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_RETOPO_MESH', 'Select an editable mesh with position data.');
    if (mesh.isSkinnedMesh || mesh.morphTargetInfluences) throw new LuciaError('LUCIA_RETOPO_UNSUPPORTED', 'Skinned and morph-target meshes are not supported by the Lucia MVP retopology pass.');
    let { positions, indices } = normalizeOperationMeshGeometry(mesh.geometry), normals = mesh.geometry.attributes.normal?.array ? new Float32Array(mesh.geometry.attributes.normal.array) : null, uvs = mesh.geometry.attributes.uv?.array ? new Float32Array(mesh.geometry.attributes.uv.array) : null, colors = mesh.geometry.attributes.color?.array ? new Float32Array(mesh.geometry.attributes.color.array) : null, tangents = mesh.geometry.attributes.tangent?.array && mesh.geometry.attributes.tangent.itemSize === 4 ? new Float32Array(mesh.geometry.attributes.tangent.array) : null, { jointIndices, jointWeights } = copySkinAttributes(mesh.geometry, positions.length / 3, 'LUCIA_RETOPO_SKIN'), customAttributes = Object.entries(mesh.geometry.attributes).filter(([name, attribute]) => !['position', 'normal', 'uv', 'color', 'tangent', 'skinIndex', 'skinWeight'].includes(name) && attribute?.array && attribute.count === mesh.geometry.attributes.position.count && Number.isInteger(attribute.itemSize) && attribute.itemSize > 0 && attribute.itemSize <= 4).map(([name, attribute]) => ({ name, itemSize: attribute.itemSize, array: new attribute.array.constructor(attribute.array) }));
    const sourceVertexCount = positions.length / 3;
    const validateRetopoAttribute = (attribute, itemSize, label) => {
      if (!attribute) return;
      if (attribute.itemSize !== itemSize || attribute.count !== sourceVertexCount || !attribute.array || attribute.array.length !== sourceVertexCount * itemSize || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_RETOPO_ATTRIBUTE', `${label} must be a finite vertex-aligned attribute.`);
    };
    validateRetopoAttribute(mesh.geometry.attributes.normal, 3, 'Normals');
    validateRetopoAttribute(mesh.geometry.attributes.uv, 2, 'UVs');
    validateRetopoAttribute(mesh.geometry.attributes.color, 3, 'Colors');
    if (mesh.geometry.attributes.tangent) validateRetopoAttribute(mesh.geometry.attributes.tangent, 4, 'Tangents');
    for (const [name, attribute] of Object.entries(mesh.geometry.attributes)) if (!['position', 'normal', 'uv', 'color', 'tangent', 'skinIndex', 'skinWeight'].includes(name) && attribute?.array) validateRetopoAttribute(attribute, attribute.itemSize, 'Custom attributes');
    const authoredGroups = this.session.getMeshMaterialGroups?.(path, indices.length), rawGroups = authoredGroups || mesh.geometry.groups || [], indexCount = indices.length, groups = rawGroups.length ? consolidateMaterialGroups(rawGroups, indexCount) : [], authoredLightmapUV = this.session.getMeshUVData?.(path, 'lightmap'), authoredDefaultUV = this.session.getMeshUVData?.(path, 'default'), lightmapAttribute = authoredLightmapUV?.uvIndices ? { name: 'st1', itemSize: 2, array: authoredLightmapUV.uvs, indices: authoredLightmapUV.uvIndices, interpolation: 'faceVarying' } : null, faceVaryingAttributes = [...(this.session.getMeshFaceVaryingPrimvars?.(path, indexCount) || []), ...(authoredDefaultUV?.uvIndices ? [{ name: 'st', itemSize: 2, array: authoredDefaultUV.uvs, indices: authoredDefaultUV.uvIndices, interpolation: 'faceVarying' }] : []), ...(lightmapAttribute ? [lightmapAttribute] : [])];
    if (authoredLightmapUV && !lightmapAttribute) throw new LuciaError('LUCIA_RETOPO_LIGHTMAP', 'Retopology requires authored lightmap UVs to include explicit face-corner indices; regenerate the lightmap set after reduction when only vertex-aligned data is available.');
    let authoredSharpChainData = null, authoredSharpEdges;
    if (sharpChains != null) { try { authoredSharpChainData = validateSharpChains(sharpChains, sharpChainSharpness, sourceVertexCount); } catch (error) { throw new LuciaError('LUCIA_RETOPO_SHARP_CHAINS', error.message); } authoredSharpEdges = expandSharpChains(authoredSharpChainData.chains, authoredSharpChainData.sharpness).edges; }
    else if (Array.isArray(sharpEdges)) authoredSharpEdges = sharpEdges;
    else { authoredSharpChainData = this.session.getMeshSharpChainData?.(path) || null; authoredSharpEdges = authoredSharpChainData ? expandSharpChains(authoredSharpChainData.chains, authoredSharpChainData.sharpness).edges : this.session.getMeshSharpEdges?.(path) || []; }
    try { validateSharpEdges(authoredSharpEdges, sourceVertexCount); } catch (error) { throw new LuciaError('LUCIA_RETOPO_SHARP_EDGES', error.message); }
    let suppliedSourceLocks = null;
    try { suppliedSourceLocks = validateVertexLockMask(lockedVertices, sourceVertexCount); } catch (error) { throw new LuciaError('LUCIA_RETOPO_LOCKS', error.message); }
    const topology = expandRetopoFaceVaryingMesh({ positions, indices, normals, uvs, colors, tangents, jointIndices, jointWeights, customAttributes, faceVaryingAttributes });
    ({ positions, indices, normals, uvs, colors, tangents, jointIndices, jointWeights, customAttributes } = topology);
    const vertexCount = positions.length / 3, topologyIndices = indices, reordered = groups.length ? reorderMaterialGroups(groups, topologyIndices) : { indices: topologyIndices, groups: [] };
    let suppliedLocks = null;
    const expandedSharpEdges = authoredSharpEdges.flatMap(([left, right]) => (topology.sourceVertexCopies[left] || []).flatMap((expandedLeft) => (topology.sourceVertexCopies[right] || []).map((expandedRight) => [expandedLeft, expandedRight])));
    if (suppliedSourceLocks) { suppliedLocks = new Uint8Array(vertexCount); for (let sourceVertex = 0; sourceVertex < suppliedSourceLocks.length; sourceVertex++) if (suppliedSourceLocks[sourceVertex]) for (const expandedVertex of topology.sourceVertexCopies[sourceVertex] || []) suppliedLocks[expandedVertex] = 1; }
    const sharpLocks = deriveEdgeLocks(expandedSharpEdges, vertexCount), locks = suppliedLocks == null ? (sharpLocks.some(Boolean) ? sharpLocks : null) : suppliedLocks;
    if (locks && lockedVertices != null) for (let index = 0; index < locks.length; index++) locks[index] |= sharpLocks[index];
    onProgress?.({ percentage: 10, message: 'Preparing mesh…' });
    const result = await new Promise((resolve, reject) => {
      this.cancel(); this.workerReject = reject;
      const worker = this.worker = new Worker(new URL('./retopo-worker.js', import.meta.url), { type: 'module' });
      const finish = () => { if (this.worker === worker) { worker.terminate(); this.worker = null; this.workerReject = null; } else worker.terminate(); };
      worker.onerror = (event) => { if (this.workerReject === reject) this.workerReject = null; finish(); reject(new LuciaError('LUCIA_RETOPO_WORKER', event.message)); };
      worker.onmessage = ({ data }) => { if (this.worker !== worker) return; if (data.type === 'result') { finish(); resolve(data); } else if (data.type === 'error') { finish(); reject(new LuciaError('LUCIA_RETOPO_WORKER', data.message)); } };
      postWorkerMessage(this, worker, { type: 'retopo', positions, indices: reordered.indices, normals, uvs, colors, tangents, jointIndices, jointWeights, customAttributes, locks, groups: reordered.groups.length ? reordered.groups : null, tolerance, targetRatio, targetError, lockBorder, lockUVSeams: lockUVSeams !== false }, [positions.buffer, reordered.indices.buffer, ...(normals ? [normals.buffer] : []), ...(uvs ? [uvs.buffer] : []), ...(colors ? [colors.buffer] : []), ...(tangents ? [tangents.buffer] : []), ...(jointIndices ? [jointIndices.buffer] : []), ...(jointWeights ? [jointWeights.buffer] : []), ...(locks ? [locks.buffer] : []), ...customAttributes.map((attribute) => attribute.array.buffer)], reject, 'LUCIA_RETOPO_WORKER');
    });
    this.releaseWorker();
    validateRetopoResult(result);
    let remappedFaceVarying = [];
    try { remappedFaceVarying = remapRetopoFaceVaryingAttributes(topology.faceVaryingAttributes, indices, result); }
    catch (error) { throw new LuciaError('LUCIA_RETOPO_FACEVARYING', error.message); }
    const defaultUV = remappedFaceVarying.find((attribute) => attribute.name === 'st');
    if (defaultUV) { result.uvs = defaultUV.array; result.uvIndices = defaultUV.indices; }
    if (remappedFaceVarying.length) result.faceVaryingAttributes = remappedFaceVarying.filter((attribute) => attribute.name !== 'st');
    if (result.xref && (authoredSharpChainData?.chains?.length || authoredSharpEdges.length)) {
      const remappedCreases = authoredSharpChainData
        ? remapRetopoCreaseChains({ chains: authoredSharpChainData.chains, sharpness: authoredSharpChainData.sharpness, sourceVertexCopies: topology.sourceVertexCopies, indices: result.indices, xref: result.xref })
        : remapRetopoCreaseEdges({ edges: authoredSharpEdges, sourceVertexCopies: topology.sourceVertexCopies, indices: result.indices, xref: result.xref });
      if (authoredSharpChainData) { result.sharpChains = remappedCreases.chains; result.sharpChainSharpness = remappedCreases.sharpness; }
      else { result.sharpEdges = remappedCreases.edges; result.sharpEdgeSharpness = remappedCreases.sharpness; }
    }
    if (reordered.groups.length) result.groups = result.groups?.length ? result.groups : null;
    this.lastStats = { kind: 'reduction', beforeTriangles: result.before, afterTriangles: result.after, reductionRatio: result.before ? result.after / result.before : 1, normalizedError: result.error || 0, targetIndexCount: result.targetIndexCount };
    if (previewOnly) return result;
    onProgress?.({ percentage: 75, message: 'Writing USD topology…' });
    const previous = await this.session.setMeshGeometry(path, result, `Retopologize ${path}`);
    onProgress?.({ percentage: 100, message: `${result.before} → ${result.after} triangles` });
    return previous;
  }
  async generateLODChain(path, options = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Generating LODs…');
    const { ratios = [.5, .25, .125], targetError = 0, lockBorder = true, lockUVSeams = this.lockUVSeamsPolicy ?? true } = options;
    if (typeof lockBorder !== 'boolean') throw new LuciaError('LUCIA_LOD_BORDER', 'lockBorder must be boolean.');
    if (typeof lockUVSeams !== 'boolean') throw new LuciaError('LUCIA_LOD_SEAMS', 'lockUVSeams must be boolean.');
    const requested = Array.isArray(ratios) ? ratios : [ratios];
    const levels = [...new Set(requested.map((value) => Number(value)).filter((value) => Number.isFinite(value) && value > 0 && value < 1))].sort((a, b) => b - a);
    if (!levels.length) throw new LuciaError('LUCIA_LOD_TARGET', 'LOD ratios must contain at least one value between 0 and 1.');
    const generated = [];
    for (let i = 0; i < levels.length; i++) {
      const ratio = levels[i];
      onProgress?.({ percentage: Math.round(i / (levels.length + 1) * 70), message: `Generating LOD ${i + 1}/${levels.length} (${Math.round(ratio * 100)}%)…` });
      generated.push(await this.retopo(path, { targetRatio: ratio, targetError, lockBorder, lockUVSeams, previewOnly: true }, onProgress));
    }
    const initial = this.session.exportUSDA();
    const base = path.split('/').at(-1);
    for (let i = 0; i < generated.length; i++) {
      const name = `${base}_LOD${i + 1}`;
      await this.session.setMeshGeometrySibling(path, name, generated[i], `Create ${name} from ${path}`);
      onProgress?.({ percentage: 70 + Math.round((i + 1) / generated.length * 30), message: `Authored ${name} (${generated[i].after} triangles)` });
    }
    this.lastStats = { kind: 'lod-chain', levels: generated.map((result, index) => ({ name: `${base}_LOD${index + 1}`, ratio: levels[index], beforeTriangles: result.before, afterTriangles: result.after, normalizedError: result.error || 0 })) };
    return initial;
  }
  async generateConvexHull(path, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Generating convex hull…');
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), position = mesh?.geometry?.attributes?.position;
    if (!position?.array || position.count < 4 || position.array.length !== position.count * 3 || hasInvalidValue(position.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_HULL_INPUT', 'Select a mesh with at least four finite vertices.');
    onProgress?.({ percentage: 25, message: 'Generating convex hull…' });
    const result = generateConvexHull(position.array);
    if (!result.valid || !result.indices.length) throw new LuciaError('LUCIA_HULL_DEGENERATE', 'The selected vertices do not form a three-dimensional convex hull.');
    const name = `${path.split('/').at(-1)}_Collider`;
    const previous = await this.session.createGuideMeshSibling(path, name, result, `Create convex hull ${path}`);
    this.lastStats = { kind: 'convex-hull', sourceVertices: result.sourceVertexCount, hullVertices: result.hullVertexCount, triangles: result.triangleCount, path: `${path.slice(0, path.lastIndexOf('/')) || ''}/${name}` };
    onProgress?.({ percentage: 100, message: `Created guide convex hull (${result.triangleCount} triangles)` });
    return previous;
  }

  async createInstance(sourcePath, name, transformSourcePath = null, onProgress) {
    if (typeof transformSourcePath === 'function' && onProgress == null) { onProgress = transformSourcePath; transformSourcePath = null; }
    onProgress = normalizeOperationProgressCallback(onProgress, 'Authoring instance…');
    if (!validPrimPath(sourcePath) || sourcePath === '/' || !validIdentifier(name)) throw new LuciaError('LUCIA_INSTANCE_INPUT', 'Instances require a valid source prim path and identifier.');
    onProgress?.({ percentage: 25, message: 'Authoring USD instance reference…' });
    const previous = await this.session.createInstanceSibling(sourcePath, name, `Create instance ${name} from ${sourcePath}`, transformSourcePath);
    this.lastStats = { kind: 'instance', sourcePath, transformSourcePath, instancePath: `${sourcePath.slice(0, sourcePath.lastIndexOf('/')) || ''}/${name}` };
    onProgress?.({ percentage: 100, message: `Created instance ${name}` });
    return previous;
  }

  async splitConnectedComponents(path, { minFaces = 1, corrections = null, asChildren = false, splitAngle = null, splitConcavity = false } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Splitting connected components…');
    if (typeof asChildren !== 'boolean') throw new LuciaError('LUCIA_COMPONENT_HIERARCHY', 'asChildren must be boolean.');
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_COMPONENT_MESH', 'Select an editable mesh with position data.');
    if (mesh.isSkinnedMesh || mesh.morphTargetInfluences) throw new LuciaError('LUCIA_COMPONENT_UNSUPPORTED', 'Connected-component splitting does not support skinned or morph-target meshes.');
    const geometry = mesh.geometry, { positions, indices } = normalizeOperationMeshGeometry(geometry), groups = this.session.getMeshMaterialGroups?.(path, indices.length) || geometry.groups || [], authoredUV = this.session.getMeshUVData?.(path, 'default'), authoredLightmapUV = this.session.getMeshUVData?.(path, 'lightmap'), authoredSharpEdgeData = this.session.getMeshSharpEdgeData?.(path) || null, authoredFaceVarying = this.session.getMeshFaceVaryingPrimvars?.(path, indices.length) || [], faceVaryingAttributes = [...authoredFaceVarying, ...(authoredUV?.uvIndices ? [{ name: 'st', itemSize: 2, array: authoredUV.uvs, indices: authoredUV.uvIndices, interpolation: 'faceVarying' }] : []), ...(authoredLightmapUV ? [{ name: 'st1', itemSize: 2, array: new Float32Array(authoredLightmapUV.uvs), indices: authoredLightmapUV.uvIndices ? new Uint32Array(authoredLightmapUV.uvIndices) : new Uint32Array(indices), interpolation: 'faceVarying' }] : [])], alignedNames = new Set(['position', 'normal', 'color', 'uv', 'skinIndex', 'skinWeight']), attributes = Object.entries(geometry.attributes).filter(([name, attribute]) => !alignedNames.has(name) && attribute?.array && attribute.count === geometry.attributes.position.count && Number.isInteger(attribute.itemSize) && attribute.itemSize > 0 && attribute.itemSize <= 4).map(([name, attribute]) => ({ name, itemSize: attribute.itemSize, array: new attribute.array.constructor(attribute.array) })), componentData = { positions, indices, groups, sharpEdges: authoredSharpEdgeData?.edges || null, sharpEdgeSharpness: authoredSharpEdgeData?.sharpness || null, attributes: [{ name: 'normals', itemSize: 3, array: geometry.attributes.normal?.array }, { name: 'uvs', itemSize: 2, array: geometry.attributes.uv?.array }, { name: 'colors', itemSize: 3, array: geometry.attributes.color?.array }, ...attributes], faceVaryingAttributes, minFaces, splitAngle, splitConcavity }, components = corrections ? extractCorrectedComponents(componentData, corrections).components : extractConnectedComponents(componentData);
    if (components.length < 2) throw new LuciaError('LUCIA_COMPONENT_COUNT', 'The selected mesh has fewer than two connected components above the minimum face threshold.');
    const original = this.session.exportUSDA(), base = path.split('/').at(-1), reserved = new Set();
    try {
      const collectNames = (nodes) => { if (!Array.isArray(nodes)) return; for (const node of nodes) { if (typeof node?.name === 'string') reserved.add(node.name); collectNames(node.children); } };
      collectNames(this.session.tree?.());
    } catch { /* A render-only host may not expose a USD tree. */ }
    try {
      const authoredNames = [];
      for (let index = 0; index < components.length; index++) { const name = uniqueComponentName(`${base}_Part${index + 1}`, index, reserved); reserved.add(name); authoredNames.push(name); const component = components[index], componentAttributes = Object.fromEntries((component.attributes || []).map((attribute) => [attribute.name, attribute.array])), componentFaceVarying = Object.fromEntries((component.faceVaryingAttributes || []).map((attribute) => [attribute.name, attribute])), uvAttribute = componentFaceVarying.st, data = { ...component, normals: componentAttributes.normals, uvs: uvAttribute?.array || componentAttributes.uvs, ...(uvAttribute ? { uvIndices: uvAttribute.indices } : {}), colors: componentAttributes.colors, customAttributes: attributes.length ? attributes.map((attribute) => ({ ...attribute, array: componentAttributes[attribute.name] })) : [], faceVaryingAttributes: (component.faceVaryingAttributes || []).filter((attribute) => attribute.name !== 'st') }; delete data.attributes; onProgress?.({ percentage: Math.round(index / (components.length + 1) * 80), message: `Authoring component ${index + 1}/${components.length}…` }); if (asChildren) await this.session.setMeshGeometryChild(path, name, data, `Create child ${name} from ${path}`); else await this.session.setMeshGeometrySibling(path, name, data, `Create ${name} from ${path}`); }
      await this.session.setVisibility(path, false);
      this.lastStats = { kind: 'components', sourcePath: path, count: components.length, minFaces: Math.max(1, Math.floor(Number(minFaces) || 1)), discardedFaces: Math.max(0, Math.floor(indices.length / 3) - components.reduce((sum, component) => sum + component.triangleCount, 0)), parts: components.map((component, index) => ({ name: authoredNames[index], triangles: component.triangleCount, vertices: component.sourceVertexCount })) };
      onProgress?.({ percentage: 100, message: `Split mesh into ${components.length} connected components.` });
      return original;
    } catch (error) { await this.session.restore(original); throw error; }
  }

  async previewConnectedComponents(path, { minFaces = 1, splitAngle = null, splitConcavity = false } = {}) {
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_COMPONENT_MESH', 'Select an editable mesh with position data.');
    if (mesh.isSkinnedMesh || mesh.morphTargetInfluences) throw new LuciaError('LUCIA_COMPONENT_UNSUPPORTED', 'Connected-component preview does not support skinned or morph-target meshes.');
    const geometry = mesh.geometry, { positions, indices } = normalizeOperationMeshGeometry(geometry), groups = this.session.getMeshMaterialGroups?.(path, indices.length) || geometry.groups || [];
    return previewComponentLabels({ positions, indices, groups, minFaces, splitAngle, splitConcavity });
  }

  async previewMaterialGraph(path) {
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), material = Array.isArray(mesh?.material) ? mesh.material[0] : mesh?.material, graph = material?.userData?.nodes || material?.nodes;
    if (!graph || typeof graph !== 'object' || Array.isArray(graph) && !graph.length) throw new LuciaError('LUCIA_MATERIAL_GRAPH', 'Selected mesh has no serializable material graph.');
    return rewriteMaterialGraph(graph);
  }

  async previewMaterialGraphTranslation(path, options = {}) {
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), material = Array.isArray(mesh?.material) ? mesh.material[0] : mesh?.material, graph = material?.userData?.nodes || material?.nodes;
    if (!graph || typeof graph !== 'object' || Array.isArray(graph) && !graph.length) throw new LuciaError('LUCIA_MATERIAL_GRAPH', 'Selected mesh has no serializable material graph.');
    try { return translateMaterialGraph(graph, options); } catch (error) { throw new LuciaError('LUCIA_MATERIAL_GRAPH_TRANSLATION', error.message); }
  }

  async previewMaterialParameterization(path, options = {}) {
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), materials = Array.isArray(mesh?.material) ? mesh.material : [mesh?.material];
    if (!mesh?.geometry?.attributes?.position || !materials.some(Boolean)) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', 'Selected mesh has no material entries to parameterize.');
    const authoredMaterialPaths = this.session.getMeshMaterialPaths?.(path), materialPaths = authoredMaterialPaths?.length === materials.length ? authoredMaterialPaths : materials.map((material, index) => material?.userData?.['primMeta.absPath'] || material.name || material.uuid || `${path}:material:${index}`);
    const channels = (material) => Object.fromEntries(['baseColor', 'metallic', 'roughness', 'opacity', 'emissive'].flatMap((channel) => { const value = parameterizationChannelValues(material, channel); return value == null ? [] : [[channel, value]]; }));
    try { return planMaterialParameterization({ ...options, materials: materials.filter(Boolean).map((material, index) => ({ path: materialPaths[index], channels: channels(material) })) }); } catch (error) { throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', error.message); }
  }

  async parameterizeMaterials(path, options = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Parameterizing materials…');
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), geometry = mesh?.geometry, position = geometry?.attributes?.position;
    if (!position?.array || !mesh?.material) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', 'Selected mesh has no material entries to parameterize.');
    const { positions, indices } = normalizeOperationMeshGeometry(geometry), materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material], channels = (material) => Object.fromEntries(['baseColor', 'metallic', 'roughness', 'opacity', 'emissive'].flatMap((channel) => { const value = parameterizationChannelValues(material, channel); return value == null ? [] : [[channel, value]]; })), authoredMaterialPaths = this.session.getMeshMaterialPaths?.(path), materialPaths = authoredMaterialPaths?.length === materials.length ? authoredMaterialPaths : materials.map((material, index) => material?.userData?.['primMeta.absPath'] || material?.name || material?.uuid || `${path}:material:${index}`);
    let plan;
    try { plan = planMaterialParameterization({ ...options, materials: materials.map((material, index) => ({ path: materialPaths[index], channels: channels(material) })) }); } catch (error) { throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', error.message); }
    const requestedMode = options.mode === 'variant' ? 'variant' : 'primvar';
    const candidates = plan.candidates.filter((candidate) => candidate.mode === requestedMode);
    if (!candidates.length) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION_NOOP', `No supported ${requestedMode === 'variant' ? 'material variant' : 'primvar parameterization'} candidate was found.`);
    const original = this.session.exportUSDA();
    if (requestedMode === 'variant') {
      if (typeof this.session.setMaterialVariants !== 'function') throw new LuciaError('LUCIA_MATERIAL_VARIANT', 'The active USD session does not support material variant authoring.');
      try {
        const authored = candidates.map((candidate) => materializeVariantParameterization({ candidate, materialPaths }));
        for (const variantSet of authored) await this.session.setMaterialVariants(path, variantSet.variants, `lucia_${variantSet.channel}`);
        this.lastStats = { kind: 'material-parameterization', mode: 'variant', channels: authored.map(({ channel }) => channel).sort(), variantSets: authored.map(({ name }) => name).sort() };
        onProgress?.({ percentage: 100, message: `Authored ${authored.length} material variant set${authored.length === 1 ? '' : 's'}.` });
        return original;
      } catch (error) { await this.session.restore(original); throw error; }
    }
    const groups = this.session.getMeshMaterialGroups?.(path, indices.length) || geometry.groups || [];
    if (materials.length > 1 && !groups.length) throw new LuciaError('LUCIA_MATERIAL_GROUPS', 'Cannot parameterize multiple materials without complete face material assignments.');
    const orderedGroups = groups.length ? consolidateMaterialGroups(groups, indices.length) : [], faceMaterialIndices = new Uint32Array(indices.length);
    for (const group of orderedGroups) faceMaterialIndices.fill(group.materialIndex, group.start, group.start + group.count);
    const faceVaryingAttributes = candidates.map((candidate) => materializePrimvarParameterization({ candidate, materialPaths, faceMaterialIndices }));
    onProgress?.({ percentage: 25, message: `Materializing ${faceVaryingAttributes.length} primvar channel${faceVaryingAttributes.length === 1 ? '' : 's'}…` });
    try {
      await this.session.setMeshGeometry(path, { positions, indices, faceVaryingAttributes }, `Parameterize materials ${path}`);
      if (typeof this.session.wireMaterialPrimvarParameters === 'function' && materialPaths.every((materialPath) => /^\/(?:[A-Za-z_][A-Za-z0-9_]*)(?:\/(?:[A-Za-z_][A-Za-z0-9_]*))*$/.test(materialPath))) await this.session.wireMaterialPrimvarParameters(materialPaths, candidates.map((candidate) => candidate.channel));
      this.lastStats = { kind: 'material-parameterization', mode: 'primvar', channels: candidates.map((candidate) => candidate.channel).sort(), attributes: faceVaryingAttributes.map((attribute) => attribute.name).sort() };
      onProgress?.({ percentage: 100, message: `Authored ${faceVaryingAttributes.length} material primvar channel${faceVaryingAttributes.length === 1 ? '' : 's'}.` });
      return original;
    } catch (error) { await this.session.restore(original); throw error; }
  }

  async evaluateMaterialGraph(path, options = {}) {
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), material = Array.isArray(mesh?.material) ? mesh.material[0] : mesh?.material, graph = material?.userData?.nodes || material?.nodes;
    if (!graph || typeof graph !== 'object' || Array.isArray(graph) && !graph.length) throw new LuciaError('LUCIA_MATERIAL_GRAPH', 'Selected mesh has no serializable material graph.');
    return evaluateMaterialGraph(graph, options);
  }

  async correctConnectedComponentsPreview(path, corrections = {}, { minFaces = 1, splitAngle = null, splitConcavity = false } = {}) {
    return correctComponentPreview(await this.previewConnectedComponents(path, { minFaces, splitAngle, splitConcavity }), corrections);
  }

  async proposeComponentNames(path, { minFaces = 1, splitAngle = null, splitConcavity = false } = {}) {
    const reserved = [], collectNames = (nodes) => { if (!Array.isArray(nodes)) return; for (const node of nodes) { if (typeof node?.name === 'string') reserved.push(node.name); collectNames(node.children); } };
    try { collectNames(this.session.tree?.()); } catch { /* A render-only host may not expose a USD tree. */ }
    return proposeComponentNames(path, await this.previewConnectedComponents(path, { minFaces, splitAngle, splitConcavity }), reserved);
  }

  async previewCrackMerge(path, { tolerance } = {}) {
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_COMPONENT_MESH', 'Select an editable mesh with position data.');
    if (mesh.isSkinnedMesh || mesh.morphTargetInfluences) throw new LuciaError('LUCIA_COMPONENT_UNSUPPORTED', 'Crack-merge preview does not support skinned or morph-target meshes.');
    const geometry = mesh.geometry, { positions, indices } = normalizeOperationMeshGeometry(geometry);
    return previewCleanupCrackMerge({ positions, indices }, { tolerance });
  }

  async mergeCracks(path, { tolerance = 0.0001, ...options } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Merging mesh cracks…');
    if (!Number.isFinite(tolerance) || tolerance <= 0) throw new LuciaError('LUCIA_CRACK_MERGE_TOLERANCE', 'Crack-merge tolerance must be a positive finite number.');
    const previous = await this.cleanup(path, { ...options, tolerance }, onProgress);
    this.lastStats = { ...this.lastStats, kind: 'crack-merge', tolerance };
    return previous;
  }

  async generateTriangleCollider(path, { targetRatio = 0.25, targetError = 0.01 } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Generating triangle collider…');
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.position || mesh.isSkinnedMesh || mesh.morphTargetInfluences) throw new LuciaError('LUCIA_TRIANGLE_COLLIDER_INPUT', 'Static triangle colliders require an unskinned mesh with position data.');
    onProgress?.({ percentage: 15, message: 'Reducing static collider mesh…' });
    const reduced = await this.retopo(path, { targetRatio: Math.max(0.05, Math.min(1, Number(targetRatio) || .25)), targetError: Math.max(0, Math.min(1, Number(targetError) || .01)), lockBorder: true, previewOnly: true }, onProgress);
    if (!reduced?.indices?.length) throw new LuciaError('LUCIA_TRIANGLE_COLLIDER_RESULT', 'Triangle collider reduction produced no valid faces.');
    const name = `${path.split('/').at(-1)}_TriangleCollider`, previous = await this.session.createGuideMeshSibling(path, name, { positions: reduced.positions, indices: reduced.indices }, `Create triangle collider ${path}`, 'none');
    this.lastStats = { kind: 'triangle-collider', sourceTriangles: reduced.before, colliderTriangles: reduced.after, normalizedError: reduced.error || 0, path: `${path.slice(0, path.lastIndexOf('/')) || ''}/${name}` };
    onProgress?.({ percentage: 100, message: `Created guide triangle collider (${reduced.after} triangles)` });
    return previous;
  }

  async createCollisionGroup(path, { name = '', colliderPaths = null, filteredGroupPaths = [], mergeGroup = '', invertFilteredGroups = false } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Authoring collision group…');
    if (!path || typeof path !== 'string') throw new LuciaError('LUCIA_COLLISION_GROUP_PATH', 'A source collider path is required.');
    const parentPath = path.slice(0, path.lastIndexOf('/')) || '/', groupName = name || `${path.split('/').at(-1)}_CollisionGroup`, members = colliderPaths == null ? [path] : colliderPaths;
    if (!Array.isArray(members) || !members.length) throw new LuciaError('LUCIA_COLLISION_GROUP_TARGET', 'Collision groups require at least one collider path.');
    onProgress?.({ percentage: 50, message: 'Authoring collision group…' });
    const previous = await this.session.createCollisionGroup(parentPath, groupName, members, filteredGroupPaths, `Create collision group ${groupName}`, { mergeGroup, invertFilteredGroups });
    this.lastStats = { kind: 'collision-group', groupPath: `${parentPath === '/' ? '' : parentPath}/${groupName}`, members: [...members], filteredGroups: [...filteredGroupPaths], mergeGroup, invertFilteredGroups };
    onProgress?.({ percentage: 100, message: `Created collision group ${groupName}` });
    return previous;
  }

  async cleanup(path, options = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Cleaning mesh…');
    const object = this.renderBridge.objectForPath(path);
    const mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_CLEANUP_MESH', 'Select an editable mesh with position data.');
    if (mesh.morphTargetInfluences) throw new LuciaError('LUCIA_CLEANUP_UNSUPPORTED', 'Morph-target meshes are not supported by safe cleanup.');
    const geometry = mesh.geometry, { positions: normalizedPositions, indices } = normalizeOperationMeshGeometry(geometry), positions = new Float32Array(normalizedPositions);
    const authoredGroups = this.session.getMeshMaterialGroups?.(path, indices?.length || positions.length / 3);
    const rawGroups = authoredGroups || geometry.groups || [], groups = rawGroups.length ? consolidateMaterialGroups(rawGroups, indices?.length || positions.length / 3) : [];
    if (Array.isArray(mesh.material) && mesh.material.length > 1 && !groups.length) throw new LuciaError('LUCIA_CLEANUP_MATERIALS', 'Material arrays require geometry groups so cleanup can preserve per-face assignments.');
    const { tolerance, minArea, minComponentArea, minComponentVolume, fillPlanarHoles } = normalizeCleanupOptions(options);
    const vertexCount = geometry.attributes.position.count, validateAligned = (name, itemSize) => { const attribute = geometry.attributes[name]; if (!attribute) return null; if (!isSupportedNumericArray(attribute.array) || attribute.itemSize !== itemSize || attribute.count !== vertexCount || attribute.array.length !== vertexCount * itemSize || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_CLEANUP_ATTRIBUTE', `${name} must be a finite ${itemSize}-component vertex-aligned typed attribute for safe cleanup.`); return new attribute.array.constructor(attribute.array); };
    const authoredUV = this.session.getMeshUVData?.(path, 'default'), authoredLightmapUV = this.session.getMeshUVData?.(path, 'lightmap'), sourceCorners = indices, lightmapAttribute = authoredLightmapUV ? { name: 'st1', itemSize: 2, array: new Float32Array(authoredLightmapUV.uvs), indices: authoredLightmapUV.uvIndices ? new Uint32Array(authoredLightmapUV.uvIndices) : new Uint32Array(sourceCorners) } : null, authoredSharpEdgeData = this.session.getMeshSharpEdgeData?.(path) || null, faceVaryingAttributes = [...(this.session.getMeshFaceVaryingPrimvars?.(path, indices.length) || []), ...(lightmapAttribute ? [lightmapAttribute] : [])], faceVaryingNames = new Set(faceVaryingAttributes.map((attribute) => attribute.name)), normals = validateAligned('normal', 3), uvs = authoredUV?.uvIndices ? new Float32Array(authoredUV.uvs) : validateAligned('uv', 2), uvIndices = authoredUV?.uvIndices ? new Uint32Array(authoredUV.uvIndices) : null, colors = faceVaryingNames.has('displayColor') ? null : validateAligned('color', 3), tangents = validateAligned('tangent', 4), skinIndexAttribute = geometry.attributes.skinIndex, skinWeightAttribute = geometry.attributes.skinWeight;
    if (mesh.isSkinnedMesh && (!skinIndexAttribute?.array || skinIndexAttribute.itemSize !== 4 || !skinWeightAttribute?.array || skinWeightAttribute.itemSize !== 4)) throw new LuciaError('LUCIA_CLEANUP_SKIN', 'Skinned meshes require complete four-influence skinIndex and skinWeight attributes for safe cleanup.');
    if ((skinIndexAttribute && !skinIndexAttribute.array) || (skinWeightAttribute && !skinWeightAttribute.array) || (skinIndexAttribute && skinIndexAttribute.itemSize !== 4) || (skinWeightAttribute && skinWeightAttribute.itemSize !== 4) || Boolean(skinIndexAttribute?.array) !== Boolean(skinWeightAttribute?.array)) throw new LuciaError('LUCIA_CLEANUP_SKIN', 'skinIndex and skinWeight must be provided together with four influences per vertex.');
    if (skinIndexAttribute?.array && (skinIndexAttribute.array.length !== geometry.attributes.position.count * 4 || hasInvalidValue(skinIndexAttribute.array, (value) => !Number.isSafeInteger(value) || value < 0 || value > 65535))) throw new LuciaError('LUCIA_CLEANUP_SKIN', 'Skin indices must contain non-negative 16-bit integer joint indices.');
    if (skinWeightAttribute?.array && (skinWeightAttribute.array.length !== geometry.attributes.position.count * 4 || hasInvalidValue(skinWeightAttribute.array, (value) => !Number.isFinite(value) || value < 0))) throw new LuciaError('LUCIA_CLEANUP_SKIN', 'Skin weights must be finite non-negative values.');
    const jointIndices = skinIndexAttribute?.array ? new Uint16Array(skinIndexAttribute.array) : null, jointWeights = skinWeightAttribute?.array ? new Float32Array(skinWeightAttribute.array) : null;
    const alignedNames = new Set(['position', 'normal', 'color', 'uv', 'tangent', 'skinIndex', 'skinWeight']), customAttributeNames = Object.keys(geometry.attributes).filter((name) => !alignedNames.has(name));
    const customAttributes = Object.entries(geometry.attributes).filter(([name]) => !alignedNames.has(name)).map(([name, attribute]) => { if (!isSupportedNumericArray(attribute?.array) || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.count !== vertexCount || attribute.array.length !== vertexCount * attribute.itemSize || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_CLEANUP_ATTRIBUTE', `${name} must be a finite, vertex-aligned typed attribute with itemSize 1–4 for safe cleanup.`); return { name, itemSize: attribute.itemSize, array: new attribute.array.constructor(attribute.array) }; });
    onProgress?.({ percentage: 10, message: 'Cleaning mesh topology…' });
    const result = await new Promise((resolve, reject) => {
      this.cancel(); this.workerReject = reject; const worker = this.worker = new Worker(new URL('./cleanup-worker.js', import.meta.url), { type: 'module' });
      const finish = () => { if (this.worker === worker) { worker.terminate(); this.worker = null; this.workerReject = null; } else worker.terminate(); };
      worker.onerror = (event) => { if (this.workerReject === reject) this.workerReject = null; finish(); reject(new LuciaError('LUCIA_CLEANUP_WORKER', event.message)); };
      worker.onmessage = ({ data }) => { if (this.worker !== worker) return; if (data.type === 'result') { finish(); resolve(data); } else if (data.type === 'error') { finish(); reject(new LuciaError('LUCIA_CLEANUP_WORKER', data.message)); } };
      postWorkerMessage(this, worker, { type: 'cleanup', positions, indices, groups: groups.map(({ start, count, materialIndex }) => ({ start, count, materialIndex })), normals, uvs, uvIndices, colors, tangents, jointIndices, jointWeights, customAttributes, faceVaryingAttributes, sharpEdges: authoredSharpEdgeData?.edges || null, sharpEdgeSharpness: authoredSharpEdgeData?.sharpness || null, tolerance, minArea, minComponentArea, minComponentVolume, fillPlanarHoles, customAttributeNames }, [positions.buffer, ...(indices ? [indices.buffer] : []), ...(normals ? [normals.buffer] : []), ...(uvs ? [uvs.buffer] : []), ...(uvIndices ? [uvIndices.buffer] : []), ...(colors ? [colors.buffer] : []), ...(tangents ? [tangents.buffer] : []), ...(jointIndices ? [jointIndices.buffer] : []), ...(jointWeights ? [jointWeights.buffer] : []), ...customAttributes.map((attribute) => attribute.array.buffer), ...faceVaryingAttributes.flatMap((attribute) => [attribute.array.buffer, attribute.indices.buffer])], reject, 'LUCIA_CLEANUP_WORKER');
    });
    this.releaseWorker();
    validateMeshCleanupResult(result);
    if (result.normals?.length === result.positions.length && result.uvs?.length === result.positions.length / 3 * 2) {
      result.tangents = await computeTangentsInWorker({ type: 'recompute', positions: new Float32Array(result.positions), indices: new Uint32Array(result.indices), uvs: new Float32Array(result.uvs), normals: new Float32Array(result.normals) }, this, onProgress);
      this.releaseWorker();
    }
    this.lastStats = { kind: 'cleanup', beforeVertices: result.beforeVertices, afterVertices: result.afterVertices, beforeTriangles: result.beforeTriangles, afterTriangles: result.afterTriangles, filledHoles: result.filledHoles || 0 };
    onProgress?.({ percentage: 75, message: 'Writing cleaned mesh…' });
    result.clearMissingAttributes = true;
    result.customAttributeNames = customAttributeNames;
    const previous = await this.session.setMeshGeometry(path, result, `Clean mesh ${path}`);
    onProgress?.({ percentage: 100, message: `${result.beforeTriangles} → ${result.afterTriangles} triangles, ${result.beforeVertices} → ${result.afterVertices} vertices` });
    return previous;
  }
  async recomputeNormals(path, { weighting = 'area', smoothingAngle = 180, preserveMaterialBoundaries = true, sharpEdges = null, sharpChains = null, sharpChainSharpness = null, interpolation = 'vertex' } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Recomputing normals…');
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_NORMAL_MESH', 'Select an editable mesh with position data.');
    if (mesh.isSkinnedMesh || mesh.morphTargetInfluences) throw new LuciaError('LUCIA_NORMAL_UNSUPPORTED', 'Skinned and morph-target meshes are not supported by normal recompute.');
    if (!['vertex', 'faceVarying'].includes(interpolation)) throw new LuciaError('LUCIA_NORMAL_INTERPOLATION', 'Normal interpolation must be vertex or faceVarying.');
    if (sharpChains != null && sharpEdges != null) throw new LuciaError('LUCIA_NORMAL_SHARP_CHAINS', 'Provide sharpChains or sharpEdges, not both.');
    const geometry = mesh.geometry, { positions: sourcePositions, indices } = normalizeOperationMeshGeometry(geometry), positions = new Float32Array(sourcePositions), expectedNormals = interpolation === 'faceVarying' ? indices.length * 3 : positions.length, authoredIndices = new Uint32Array(indices), groups = validateMeshMaterialGroups((geometry.groups || []).map(({ start, count, materialIndex }) => ({ start, count, materialIndex })), authoredIndices.length);
    const suppliedSharpChainData = sharpChains == null ? null : (() => { try { return validateSharpChains(sharpChains, sharpChainSharpness, geometry.attributes.position.count); } catch (error) { throw new LuciaError('LUCIA_NORMAL_SHARP_CHAINS', error.message); } })(), authoredSharpChainData = suppliedSharpChainData || (Array.isArray(sharpEdges) ? null : this.session.getMeshSharpChainData?.(path) || null), authoredSharpEdgeData = suppliedSharpChainData ? expandSharpChains(suppliedSharpChainData.chains, suppliedSharpChainData.sharpness) : Array.isArray(sharpEdges) ? { edges: sharpEdges, sharpness: null } : this.session.getMeshSharpEdgeData?.(path) || { edges: this.session.getMeshSharpEdges?.(path) || mesh.geometry.userData?.sharpEdges || [], sharpness: null }, authoredSharpEdges = authoredSharpEdgeData.edges;
    try { validateSharpEdges(authoredSharpEdges, geometry.attributes.position.count); } catch (error) { throw new LuciaError('LUCIA_NORMAL_SHARP_EDGES', error.message); }
    onProgress?.({ percentage: 20, message: `Computing ${interpolation === 'faceVarying' ? 'face-varying' : 'vertex'} normals…` });
    const normals = await new Promise((resolve, reject) => {
      this.cancel(); this.workerReject = reject; const worker = this.worker = new Worker(new URL('./normal-worker.js', import.meta.url), { type: 'module' });
      const finish = () => { if (this.worker === worker) { worker.terminate(); this.worker = null; this.workerReject = null; } else worker.terminate(); };
      worker.onerror = (event) => { finish(); reject(new LuciaError('LUCIA_NORMAL_WORKER', event.message)); };
      worker.onmessage = ({ data }) => { if (this.worker !== worker) return; if (data.type === 'result') { try { const value = validateWorkerVector(data.normals, expectedNormals, 'LUCIA_NORMAL_RESULT', 'Normal'); finish(); resolve(value); } catch (error) { finish(); reject(error); } } else if (data.type === 'error') { finish(); reject(new LuciaError('LUCIA_NORMAL_WORKER', data.message)); } };
      postWorkerMessage(this, worker, { type: 'recompute', positions, indices, groups, sharpEdges: authoredSharpEdges, weighting, smoothingAngle, preserveMaterialBoundaries, interpolation }, [positions.buffer, indices.buffer], reject, 'LUCIA_NORMAL_WORKER');
    });
    this.releaseWorker();
    const data = { positions: new Float32Array(geometry.attributes.position.array), indices: authoredIndices, normals, ...(interpolation === 'faceVarying' ? { normalIndices: authoredIndices } : {}), uvs: geometry.attributes.uv?.array ? new Float32Array(geometry.attributes.uv.array) : null, ...(authoredSharpChainData ? { sharpChains: authoredSharpChainData.chains, sharpChainSharpness: authoredSharpChainData.sharpness } : { sharpEdges: authoredSharpEdges, ...(authoredSharpEdgeData.sharpness?.length === authoredSharpEdges.length ? { sharpEdgeSharpness: authoredSharpEdgeData.sharpness } : {}) }) };
    if (data.uvs && interpolation === 'vertex') {
      data.tangents = await computeTangentsInWorker({ type: 'recompute', positions: new Float32Array(data.positions), indices: new Uint32Array(data.indices), uvs: new Float32Array(data.uvs), normals: new Float32Array(data.normals) }, this, onProgress);
      this.releaseWorker();
    }
    onProgress?.({ percentage: 75, message: 'Writing recomputed normals…' });
    const previous = await this.session.setMeshGeometry(path, data, `Recompute normals ${path}`);
    onProgress?.({ percentage: 100, message: `${interpolation === 'faceVarying' ? 'Face-varying' : 'Vertex'} normals recomputed` });
    return previous;
  }
  async recomputeTangents(path, {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Recomputing tangents…');
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), geometry = mesh?.geometry;
    if (!geometry?.attributes?.position || !geometry.attributes.uv || !geometry.attributes.normal) throw new LuciaError('LUCIA_TANGENT_INPUT', 'Tangents require positions, UVs, and normals.');
    validateBakeMeshData(geometry);
    if (!geometry.attributes.normal.array || geometry.attributes.normal.count !== geometry.attributes.position.count || geometry.attributes.normal.array.length !== geometry.attributes.position.count * 3 || hasInvalidValue(geometry.attributes.normal.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_TANGENT_INPUT', 'Normals must have one finite 3-component value per position.');
    if (mesh.isSkinnedMesh || mesh.morphTargetInfluences) throw new LuciaError('LUCIA_TANGENT_UNSUPPORTED', 'Skinned and morph-target meshes are not supported by tangent recompute.');
    const { positions: sourcePositions, indices } = normalizeOperationMeshGeometry(geometry), positions = new Float32Array(sourcePositions), uvs = new Float32Array(geometry.attributes.uv.array), normals = new Float32Array(geometry.attributes.normal.array);
    onProgress?.({ percentage: 20, message: 'Preparing tangent frame…' });
    const tangents = await computeTangentsInWorker({ type: 'recompute', positions, indices, uvs, normals }, this, onProgress);
    this.releaseWorker();
    onProgress?.({ percentage: 75, message: 'Writing tangents…' });
    return this.session.setMeshGeometry(path, { positions: new Float32Array(geometry.attributes.position.array), indices, uvs: new Float32Array(geometry.attributes.uv.array), normals: new Float32Array(geometry.attributes.normal.array), tangents }, `Recompute tangents ${path}`);
  }
  async transferSkinWeights(targetPath, sourcePath, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Transferring skin weights…');
    const targetObject = this.renderBridge.objectForPath(targetPath), sourceObject = this.renderBridge.objectForPath(sourcePath), target = targetObject?.isMesh ? targetObject : targetObject?.getObjectByProperty?.('isMesh', true), source = sourceObject?.isMesh ? sourceObject : sourceObject?.getObjectByProperty?.('isMesh', true), targetGeometry = target?.geometry, sourceGeometry = source?.geometry;
    if (!targetGeometry?.attributes?.position || !sourceGeometry?.attributes?.position || !sourceGeometry.attributes.skinIndex || !sourceGeometry.attributes.skinWeight) throw new LuciaError('LUCIA_SKIN_TRANSFER_INPUT', 'Source and target meshes need position data; the source also needs skin indices and weights.');
    const { positions: normalizedTargetPositions, indices: normalizedTargetIndices } = normalizeOperationMeshGeometry(targetGeometry), { positions: normalizedSourcePositions, indices: normalizedSourceIndices } = normalizeOperationMeshGeometry(sourceGeometry);
    if (target.isSkinnedMesh || target.morphTargetInfluences) throw new LuciaError('LUCIA_SKIN_TRANSFER_TARGET', 'Skin transfer target must be an unskinned reduced mesh.');
    const sourcePositions = new Float32Array(normalizedSourcePositions), sourceIndices = normalizedSourceIndices, sourceSkinIndices = new Uint16Array(sourceGeometry.attributes.skinIndex.array), sourceSkinWeights = new Float32Array(sourceGeometry.attributes.skinWeight.array), targetPositions = new Float32Array(normalizedTargetPositions), result = await new Promise((resolve, reject) => {
      this.cancel(); this.workerReject = reject; const worker = this.worker = new Worker(new URL('./rig-worker.js', import.meta.url), { type: 'module' });
      const finish = () => { if (this.worker === worker) { worker.terminate(); this.worker = null; this.workerReject = null; } else worker.terminate(); };
      worker.onerror = (event) => { finish(); reject(new LuciaError('LUCIA_SKIN_TRANSFER_WORKER', event.message)); };
      worker.onmessage = ({ data }) => { if (this.worker !== worker) return; if (data.type === 'result') { finish(); resolve(data.result); } else if (data.type === 'error') { finish(); reject(new LuciaError('LUCIA_SKIN_TRANSFER_WORKER', data.message)); } };
      const transfer = [sourcePositions.buffer, sourceSkinIndices.buffer, sourceSkinWeights.buffer, targetPositions.buffer]; if (sourceIndices) transfer.push(sourceIndices.buffer);
      postWorkerMessage(this, worker, { type: 'transfer', sourcePositions, sourceIndices, sourceSkinIndices, sourceSkinWeights, boneCount: source.skeleton?.bones?.length ?? null, targetPositions }, transfer, reject, 'LUCIA_SKIN_TRANSFER_WORKER');
    });
    this.releaseWorker();
    validateSkinTransferResult(result, targetGeometry.attributes.position.count, source.skeleton?.bones?.length ?? null);
    const data = { positions: new Float32Array(normalizedTargetPositions), indices: normalizedTargetIndices, jointIndices: result.jointIndices, jointWeights: result.jointWeights };
    if (targetGeometry.attributes.normal?.array) data.normals = new Float32Array(targetGeometry.attributes.normal.array);
    if (targetGeometry.attributes.uv?.array) data.uvs = new Float32Array(targetGeometry.attributes.uv.array);
    onProgress?.({ percentage: 70, message: `Transferred skin weights (${result.meanDistance.toPrecision(4)} mean distance)` });
    this.renderBridge.pendingTransferError = { path: targetPath, distances: result.distances };
    let previous; try { previous = await this.session.setMeshGeometry(targetPath, data, `Transfer skin weights ${sourcePath} to ${targetPath}`); } catch (error) { this.renderBridge.pendingTransferError = null; throw error; } this.lastStats = { kind: 'skin-transfer', sourcePath, targetPath, meanDistance: result.meanDistance, maxDistance: result.maxDistance, distances: result.distances }; onProgress?.({ percentage: 100, message: 'Skin weights transferred' }); return previous;
  }
  async unwrap(path, options = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Generating UV atlas…');
    const object = this.renderBridge.objectForPath(path);
    const mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_UV_MESH', 'Select an editable mesh with position data.');
    if (mesh.morphTargetInfluences) throw new LuciaError('LUCIA_UV_UNSUPPORTED', 'Morph-target meshes are not supported by the UV unwrap pass.');
    if (Array.isArray(mesh.material) && mesh.material.length > 1 && !mesh.geometry.groups?.length) throw new LuciaError('LUCIA_UV_MATERIALS', 'Material arrays require geometry groups so xatlas can preserve per-face assignments.');
    if (options.uvSet != null && !['default', 'lightmap'].includes(options.uvSet)) throw new LuciaError('LUCIA_UV_SET', `Unsupported UV set: ${options.uvSet}`);
    const normalizedOptions = normalizeUnwrapOptions(options), geometry = mesh.geometry, { positions, indices } = normalizeOperationMeshGeometry(geometry), normals = geometry.attributes.normal?.array ? new Float32Array(geometry.attributes.normal.array) : null, colors = geometry.attributes.color?.array ? new Float32Array(geometry.attributes.color.array) : null, tangents = geometry.attributes.tangent?.array && geometry.attributes.tangent.itemSize === 4 ? new Float32Array(geometry.attributes.tangent.array) : null, { jointIndices, jointWeights } = copySkinAttributes(geometry, positions.length / 3, 'LUCIA_UV_SKIN');
    const alignedNames = new Set(['position', 'normal', 'color', 'tangent', 'uv', 'skinIndex', 'skinWeight']);
    const customAttributes = Object.entries(geometry.attributes).filter(([name, attribute]) => !alignedNames.has(name) && attribute?.array && attribute.count === geometry.attributes.position.count && Number.isInteger(attribute.itemSize) && attribute.itemSize > 0 && attribute.itemSize <= 4).map(([name, attribute]) => ({ name, itemSize: attribute.itemSize, array: new attribute.array.constructor(attribute.array) }));
    const sourceIndices = new Uint32Array(indices);
    onProgress?.({ percentage: 10, message: 'Preparing UV atlas…' });
    const result = await new Promise((resolve, reject) => {
      this.cancel(); this.workerReject = reject;
      const worker = this.worker = new Worker(new URL('./xatlas-worker.js', import.meta.url), { type: 'module' });
      const finish = () => { if (this.worker === worker) { worker.terminate(); this.worker = null; this.workerReject = null; } else worker.terminate(); };
      worker.onerror = (event) => { finish(); reject(new LuciaError('LUCIA_UV_WORKER', event.message)); };
      worker.onmessage = ({ data }) => { if (this.worker !== worker) return; if (data.type === 'result') { finish(); resolve(data.result); } else if (data.type === 'error') { finish(); reject(new LuciaError('LUCIA_UV_ATLAS', data.message)); } };
      postWorkerMessage(this, worker, { type: 'unwrap', positions, indices, normals, colors, tangents, jointIndices, jointWeights, customAttributes, options: normalizedOptions }, [positions.buffer, indices.buffer, ...(normals ? [normals.buffer] : []), ...(colors ? [colors.buffer] : []), ...(tangents ? [tangents.buffer] : []), ...(jointIndices ? [jointIndices.buffer] : []), ...(jointWeights ? [jointWeights.buffer] : []), ...customAttributes.map((attribute) => attribute.array.buffer)], reject, 'LUCIA_UV_WORKER');
    });
    this.releaseWorker();
    validateUVAtlasResult(result);
    if (mesh.geometry.groups?.length) result.groups = transferXatlasMaterialGroups(mesh.geometry.groups, sourceIndices, result.indices, result.xref);
    if (result.normals?.length === result.positions.length && result.uvs?.length === result.positions.length / 3 * 2) {
      const tangentPositions = new Float32Array(result.positions), tangentIndices = new Uint32Array(result.indices), tangentUvs = new Float32Array(result.uvs), tangentNormals = new Float32Array(result.normals);
      result.tangents = await computeTangentsInWorker({ type: 'recompute', positions: tangentPositions, indices: tangentIndices, uvs: tangentUvs, normals: tangentNormals }, this, onProgress);
      this.releaseWorker();
    } else if (result.tangents?.length !== result.positions.length / 3 * 4 || hasInvalidTangent(result.tangents)) result.tangents = null;
    onProgress?.({ percentage: 75, message: 'Writing UV primvars…' });
    const previous = await this.session.setMeshGeometry(path, { ...result, uvSet: options.uvSet === 'lightmap' ? 'lightmap' : 'default' }, `Unwrap ${options.uvSet === 'lightmap' ? 'lightmap UVs' : 'UVs'} ${path}`);
    onProgress?.({ percentage: 100, message: `${result.singleAtlasFallback ? 'Packed with single-atlas fallback: ' : 'Packed '}${result.chartCount} UV charts (${result.width}×${result.height})` });
    return previous;
  }
  async projectUV(path, { mode = 'planar', uvSet = 'default' } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Projecting UVs…');
    const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), geometry = mesh?.geometry;
    if (!geometry?.attributes?.position) throw new LuciaError('LUCIA_UV_MESH', 'Select an editable mesh with position data.');
    if (!['planar', 'box', 'cylindrical', 'spherical'].includes(mode)) throw new LuciaError('LUCIA_UV_PROJECTION', `Unsupported UV projection: ${mode}`);
    if (!['default', 'lightmap'].includes(uvSet)) throw new LuciaError('LUCIA_UV_SET', `Unsupported UV set: ${uvSet}`);
    const { positions, indices } = normalizeOperationMeshGeometry(geometry), uvs = projectUVs({ positions, mode }), normals = geometry.attributes.normal?.array ? new Float32Array(geometry.attributes.normal.array) : null;
    onProgress?.({ percentage: 60, message: `Projecting ${mode} UVs…` });
    const data = { positions, indices, uvs, uvSet };
    if (normals?.length === positions.length) {
      data.tangents = await computeTangentsInWorker({ type: 'recompute', positions: new Float32Array(positions), indices: new Uint32Array(indices), uvs: new Float32Array(uvs), normals: new Float32Array(normals) }, this, onProgress);
      this.releaseWorker();
    }
    const previous = await this.session.setMeshGeometry(path, data, `Project ${mode} ${uvSet === 'lightmap' ? 'lightmap ' : ''}UVs ${path}`);
    onProgress?.({ percentage: 100, message: `${mode} UV projection applied` });
    return previous;
  }
  async transferUVs(targetPath, sourcePath, { maxDistance = Infinity, offset = 1e-6, uvSet = 'default' } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Transferring UVs…');
    if (!['default', 'lightmap'].includes(uvSet)) throw new LuciaError('LUCIA_UV_SET', `Unsupported UV set: ${uvSet}`);
    const findMesh = (path) => { const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true); if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_UV_TRANSFER_MESH', `No editable mesh was found at ${path}.`); return mesh; };
    const target = findMesh(targetPath), source = findMesh(sourcePath), targetGeometry = target.geometry, sourceGeometry = source.geometry;
    const { positions: normalizedTargetPositions, indices: normalizedTargetIndices } = normalizeOperationMeshGeometry(targetGeometry), { positions: normalizedSourcePositions, indices: normalizedSourceIndices } = normalizeOperationMeshGeometry(sourceGeometry);
    const targetNormal = targetGeometry.attributes.normal, sourceUV = (uvSet === 'lightmap' ? sourceGeometry.attributes.uv1 : sourceGeometry.attributes.uv), targetUV = (uvSet === 'lightmap' ? targetGeometry.attributes.uv1 : targetGeometry.attributes.uv);
    if (!sourceUV?.array || sourceUV.array.length !== sourceGeometry.attributes.position.count * 2 || hasInvalidValue(sourceUV.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_UV_TRANSFER_SOURCE', 'The source mesh needs a finite UV set to transfer.');
    const localTargetPositions = new Float32Array(normalizedTargetPositions), targetPositions = transformProjectionPositions(localTargetPositions, target.matrixWorld?.elements), sourcePositions = transformProjectionPositions(new Float32Array(normalizedSourcePositions), source.matrixWorld?.elements), targetIndices = normalizedTargetIndices, sourceIndices = normalizedSourceIndices, groups = validateMeshMaterialGroups((targetGeometry.groups || []).map(({ start, count, materialIndex }) => ({ start, count, materialIndex })), targetIndices.length), targetNormals = targetNormal?.array ? new Float32Array(targetNormal.array) : recomputeVertexNormals({ positions: localTargetPositions, indices: targetIndices });
    validateUVTransferNormals(targetNormals, targetGeometry.attributes.position.count);
    onProgress?.({ percentage: 30, message: 'Projecting source UVs onto target vertices…' });
    const result = await new Promise((resolve, reject) => {
      this.cancel(); this.workerReject = reject; const worker = this.worker = new Worker(new URL('./projection-worker.js', import.meta.url), { type: 'module' });
      const finish = () => { if (this.worker === worker) { worker.terminate(); this.worker = null; this.workerReject = null; } else worker.terminate(); };
      worker.onerror = (event) => { finish(); reject(new LuciaError('LUCIA_UV_TRANSFER_WORKER', event.message)); };
      worker.onmessage = ({ data }) => { if (this.worker !== worker) return; if (data.type === 'transfer-result') { finish(); resolve(data); } else if (data.type === 'error') { finish(); reject(new LuciaError('LUCIA_UV_TRANSFER_WORKER', data.message)); } };
      const targetData = { positions: targetPositions, normals: transformProjectionNormals(targetNormals, target.matrixWorld?.elements), maxDistance: Number.isFinite(Number(maxDistance)) ? Math.max(0, Number(maxDistance)) : Infinity, offset: Math.max(0, Number(offset) || 0) }, sourceData = { positions: sourcePositions, indices: sourceIndices, uvs: new Float32Array(sourceUV.array) };
      postWorkerMessage(this, worker, { type: 'transfer-uvs', target: targetData, source: sourceData }, [targetPositions.buffer, targetData.normals.buffer, sourcePositions.buffer, sourceIndices.buffer, sourceData.uvs.buffer], reject, 'LUCIA_UV_TRANSFER_WORKER');
    });
    this.releaseWorker();
    validateUVTransferResult(result, targetGeometry.attributes.position.count);
    if (result.hitCount === 0) throw new LuciaError('LUCIA_UV_TRANSFER_MISS', 'No target vertices reached the source surface within the requested distance.');
    const authoredUVs = targetUV?.array, transferredUVs = result.hitMask && authoredUVs?.length === result.uvs.length ? mergeTransferredUVs(result.uvs, authoredUVs, result.hitMask) : new Float32Array(result.uvs);
    let tangents = null;
    if (targetNormals.length === targetGeometry.attributes.position.count * 3) {
      const tangentUVs = uvSet === 'lightmap' ? targetGeometry.attributes.uv?.array : transferredUVs;
      if (tangentUVs?.length === targetGeometry.attributes.position.count * 2 && !hasInvalidFinite(tangentUVs)) {
        tangents = await computeTangentsInWorker({ type: 'recompute', positions: new Float32Array(targetGeometry.attributes.position.array), indices: new Uint32Array(targetIndices), uvs: new Float32Array(tangentUVs), normals: new Float32Array(targetNormals) }, this, onProgress);
        this.releaseWorker();
      } else if (targetGeometry.attributes.tangent?.array?.length === targetGeometry.attributes.position.count * 4 && !hasInvalidFinite(targetGeometry.attributes.tangent.array)) {
        tangents = new Float32Array(targetGeometry.attributes.tangent.array);
      }
    }
    const data = { positions: new Float32Array(targetGeometry.attributes.position.array), indices: targetIndices, uvs: transferredUVs, normals: new Float32Array(targetNormals), tangents, ...(groups.length ? { groups } : {}) };
    onProgress?.({ percentage: 75, message: `Writing transferred UVs (${result.hitCount} hits, ${result.missCount} misses)…` });
    data.uvSet = uvSet;
    const previous = await this.session.setMeshGeometry(targetPath, data, `Transfer ${uvSet === 'lightmap' ? 'lightmap ' : ''}UVs from ${sourcePath} to ${targetPath}`);
    onProgress?.({ percentage: 100, message: `Transferred UVs to ${result.hitCount} of ${targetGeometry.attributes.position.count} vertices` });
    return previous;
  }
  async projectFromSource(targetPath, sourcePath, { rayDistance = 1, cageOffset = 1e-4, resolution = 1024 } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Projecting source attributes…');
    const findMesh = (path) => { const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true); if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_PROJECTION_MESH', `No editable mesh was found at ${path}.`); return mesh; };
    const target = findMesh(targetPath), source = findMesh(sourcePath), targetGeometry = target.geometry, sourceGeometry = source.geometry;
    const { positions: normalizedTargetPositions, indices: normalizedTargetIndices } = normalizeOperationMeshGeometry(targetGeometry), { positions: normalizedSourcePositions, indices: normalizedSourceIndices } = normalizeOperationMeshGeometry(sourceGeometry);
    const targetUV = targetGeometry.attributes.uv;
    if (!targetUV?.array || targetUV.array.length !== targetGeometry.attributes.position.count * 2 || hasInvalidValue(targetUV.array, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_PROJECTION_UV', 'The target mesh needs finite two-component UVs.');
    const targetPositions = transformProjectionPositions(new Float32Array(normalizedTargetPositions), target.matrixWorld?.elements), targetIndices = normalizedTargetIndices, targetUVs = new Float32Array(targetUV.array), sourcePositions = transformProjectionPositions(new Float32Array(normalizedSourcePositions), source.matrixWorld?.elements), sourceIndices = normalizedSourceIndices, targetNormals = targetGeometry.attributes.normal?.array ? transformProjectionNormals(new Float32Array(targetGeometry.attributes.normal.array), target.matrixWorld?.elements) : null;
    onProgress?.({ percentage: 10, message: 'Projecting target UVs onto source mesh…' });
    const result = await new Promise((resolve, reject) => {
      this.cancel(); this.workerReject = reject; const worker = this.worker = new Worker(new URL('./projection-worker.js', import.meta.url), { type: 'module' });
      const finish = () => { if (this.worker === worker) { worker.terminate(); this.worker = null; this.workerReject = null; } else worker.terminate(); };
      worker.onerror = (event) => { finish(); reject(new LuciaError('LUCIA_PROJECTION_WORKER', event.message)); };
      worker.onmessage = ({ data }) => { if (this.worker !== worker) return; if (data.type === 'result') { finish(); resolve(data); } else if (data.type === 'error') { finish(); reject(new LuciaError('LUCIA_PROJECTION_WORKER', data.message)); } };
      const targetData = { positions: targetPositions, indices: targetIndices, uvs: targetUVs, normals: targetNormals, resolution, rayDistance, cageOffset }, sourceData = { positions: sourcePositions, indices: sourceIndices };
      postWorkerMessage(this, worker, { type: 'project-rays', target: targetData, source: sourceData }, [targetPositions.buffer, targetIndices.buffer, targetUVs.buffer, sourcePositions.buffer, sourceIndices.buffer, ...(targetNormals ? [targetNormals.buffer] : [])], reject, 'LUCIA_PROJECTION_WORKER');
    });
    this.releaseWorker();
    validateProjectionResult(result); onProgress?.({ percentage: 100, message: `Projected ${result.triangle.length} target texel rays (${[...result.triangle].filter((face) => face >= 0).length} hits)` }); return result;
  }
  async bakeProjected(targetPath, sourcePath, { resolution = 1024, maxResolution = 2048, channel = 'baseColor', normalSpace = 'object', rayDistance = 1, cageOffset = 1e-4, dilation = 2 } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Baking projected texture…');
    if (!['baseColor', 'normal', 'roughness', 'metallic', 'opacity', 'emissive'].includes(channel)) throw new LuciaError('LUCIA_PROJECTION_CHANNEL', 'Projected baking supports baseColor, normal, roughness, metallic, opacity, and emissive channels.');
    if (resolution != null) validateBakeControl(resolution, { integer: true, minimum: 64, maximum: 4096 }, 'Projected-bake resolution');
    if (maxResolution != null) validateBakeControl(maxResolution, { integer: true, minimum: 64, maximum: 4096 }, 'Projected-bake maximum dimension');
    if (dilation != null) validateBakeControl(dilation, { integer: true, minimum: 0, maximum: 32 }, 'Projected-bake dilation');
    if (rayDistance != null) validateBakeControl(rayDistance, { minimum: 1e-6, maximum: 1e6 }, 'Projected-bake ray distance');
    if (cageOffset != null) validateBakeControl(cageOffset, { minimum: 0, maximum: 1e6 }, 'Projected-bake cage offset');
    const findMesh = (path) => { const object = this.renderBridge.objectForPath(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true); if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_PROJECTION_MESH', `No editable mesh was found at ${path}.`); return mesh; };
    const target = findMesh(targetPath), source = findMesh(sourcePath), sourceGeometry = source.geometry, targetGeometry = target.geometry, sourcePosition = sourceGeometry.attributes.position, { positions: normalizedSourcePositions, indices: sourceIndices } = normalizeOperationMeshGeometry(sourceGeometry), { positions: normalizedTargetPositions, indices: normalizedTargetIndices } = normalizeOperationMeshGeometry(targetGeometry), limit = Math.max(64, Math.min(4096, Number(maxResolution) || 2048)), size = Math.max(64, Math.min(limit, Number(resolution) || 1024));
    const sourceAttribute = channel === 'normal' ? sourceGeometry.attributes.normal : channel === 'baseColor' ? sourceGeometry.attributes.color : null, sourceMaterials = Array.isArray(source.material) ? source.material : [source.material], material = sourceMaterials[0], textureProperty = { baseColor: 'map', roughness: 'roughnessMap', metallic: 'metalnessMap', opacity: 'alphaMap', emissive: 'emissiveMap' }[channel], sourceTexture = textureProperty ? material?.[textureProperty]?.image : null, sourcePixels = sourceTexture?.data || sourceTexture?.pixels, sourceColorTransform = material?.userData?.lightusdTextureColorTransforms?.[textureProperty] || null, sourceTextureImages = sourceMaterials.map((entry) => { const image = textureProperty ? entry?.[textureProperty]?.image : null, pixels = image?.data || image?.pixels, colorTransform = entry?.userData?.lightusdTextureColorTransforms?.[textureProperty] || null; return pixels && image.width && image.height && pixels.length === image.width * image.height * 4 ? { pixels: new (pixels instanceof Float32Array ? Float32Array : Uint8Array)(pixels), width: image.width, height: image.height, colorSpace: image.colorSpace || entry?.[textureProperty]?.colorSpace || 'srgb', ...(colorTransform ? { colorTransform } : {}) } : null; }), sourceUVAttribute = sourceGeometry.attributes.uv, sourceMaterialIndices = Uint32Array.from({ length: Math.floor(sourceIndices.length / 3) }, (_, face) => sourceGeometry.groups?.find((group) => face * 3 >= group.start && face * 3 < group.start + group.count)?.materialIndex || 0), hasSourceTexture = Boolean(sourceTextureImages.some(Boolean) && sourceUVAttribute?.array?.length === sourcePosition.count * 2), scalar = channel === 'roughness' ? material?.roughness ?? .5 : channel === 'metallic' ? material?.metalness ?? 0 : channel === 'opacity' ? material?.opacity ?? 1 : 0, fallback = channel === 'normal' ? [.5, .5, 1] : channel === 'emissive' ? [material?.emissive?.r ?? 0, material?.emissive?.g ?? 0, material?.emissive?.b ?? 0] : channel === 'baseColor' ? [material?.color?.r ?? .7, material?.color?.g ?? .7, material?.color?.b ?? .7] : [scalar, scalar, scalar];
    const originalSourceMaterials = sourceMaterials;
    {
    const sourceMaterials = originalSourceMaterials.slice();
    if (channel !== 'normal') sourceMaterials.forEach((entry, index) => { const values = materialChannelValues(entry, channel); sourceMaterials[index] = { ...entry, color: { r: values[0], g: values[1], b: values[2] }, roughness: values[0], metalness: values[0], opacity: values[0], emissive: { r: values[0], g: values[1], b: values[2] } }; });
    if (channel === 'normal' && (!sourceAttribute?.array || sourceAttribute.array.length !== sourcePosition.count * 3 || hasInvalidValue(sourceAttribute.array, (value) => !Number.isFinite(value)))) throw new LuciaError('LUCIA_PROJECTION_NORMAL', 'The source mesh needs finite vertex normals for projected normal baking.');
    if (channel === 'normal' && !['object', 'tangent'].includes(normalSpace)) throw new LuciaError('LUCIA_PROJECTION_NORMAL_SPACE', 'Projected normal baking supports object or tangent space.');
    if (channel === 'normal' && normalSpace === 'tangent' && !target.geometry.attributes.uv?.array) throw new LuciaError('LUCIA_PROJECTION_NORMAL', 'Tangent-space projected normal baking requires target UVs.');
    const targetPosition = target.geometry.attributes.position, targetIndex = target.geometry.index, estimatedBytes = estimateProjectionWorkingBytes({ resolution: size, targetVertexCount: targetPosition.count, targetIndexCount: targetIndex?.array?.length || targetPosition.count, sourceVertexCount: sourcePosition.count, sourceIndexCount: sourceIndices.length, sourceTextureBytes: sourceTextureImages.reduce((sum, entry) => sum + (entry?.pixels?.byteLength || 0), 0) });
    if (estimatedBytes > BAKE_MEMORY_LIMIT_BYTES) throw new LuciaError('LUCIA_PROJECTION_MEMORY', `Projected bake would require about ${(estimatedBytes / (1024 * 1024)).toFixed(0)} MiB; reduce resolution below the ${BAKE_MEMORY_LIMIT_BYTES / (1024 * 1024)} MiB Lucia limit.`);
    const sourceValues = sourceAttribute?.array?.length === sourcePosition.count * 3 ? new Float32Array(sourceAttribute.array) : Float32Array.from({ length: sourcePosition.count * 3 }, (_, index) => fallback[index % 3]), targetPositionValues = new Float32Array(normalizedTargetPositions), targetIndices = normalizedTargetIndices, targetNormals = channel === 'normal' && targetGeometry.attributes.normal?.array?.length === targetPosition.count * 3 ? new Float32Array(targetGeometry.attributes.normal.array) : null;
    if (channel === 'normal' && normalSpace === 'tangent' && (!targetNormals || hasInvalidNormal(targetNormals))) throw new LuciaError('LUCIA_PROJECTION_NORMAL', 'The target mesh needs finite, non-zero vertex normals for tangent-space projected normal baking.');
    const candidateTargetTangents = targetGeometry.attributes.tangent?.array ? new Float32Array(targetGeometry.attributes.tangent.array) : null, validTargetTangents = candidateTargetTangents?.length === targetPosition.count * 4 && !hasInvalidTangent(candidateTargetTangents, true) ? candidateTargetTangents : null, targetTangents = channel === 'normal' && normalSpace === 'tangent' ? validTargetTangents || recomputeVertexTangents({ positions: targetPositionValues, indices: targetIndices, uvs: new Float32Array(targetGeometry.attributes.uv.array), normals: targetNormals }) : null;
    if (channel === 'normal' && normalSpace === 'tangent' && !targetTangents) throw new LuciaError('LUCIA_PROJECTION_NORMAL', 'Tangent-space projected normal baking requires target vertex tangents.');
    const projectedSourceValues = channel === 'normal' && normalSpace === 'tangent' ? transformProjectionNormals(sourceValues, source.matrixWorld?.elements) : sourceValues, projectedTargetNormals = channel === 'normal' && normalSpace === 'tangent' ? transformProjectionNormals(targetNormals, target.matrixWorld?.elements) : targetNormals, projectedTargetTangents = channel === 'normal' && normalSpace === 'tangent' ? transformProjectionTangents(targetTangents, target.matrixWorld?.elements) : targetTangents;
    onProgress?.({ percentage: 10, message: 'Projecting source attributes…' });
    const projection = await this.projectFromSource(targetPath, sourcePath, { resolution: size, rayDistance, cageOffset }, (progress) => onProgress?.({ percentage: 10 + progress.percentage * .65, message: progress.message }));
    const sourceMaterialValues = Float32Array.from({ length: sourceMaterialIndices.length * 3 }, (_, index) => { const entry = sourceMaterials[sourceMaterialIndices[Math.floor(index / 3)]] || material, color = entry?.color || { r: .7, g: .7, b: .7 }; return channel === 'roughness' ? entry?.roughness ?? .5 : channel === 'metallic' ? entry?.metalness ?? 0 : channel === 'opacity' ? entry?.opacity ?? 1 : channel === 'emissive' ? [entry?.emissive?.r ?? 0, entry?.emissive?.g ?? 0, entry?.emissive?.b ?? 0][index % 3] : [color.r, color.g, color.b][index % 3]; }), sourceColorSpace = sourceTexture?.colorSpace || sourceTexture?.userData?.colorSpace || 'srgb', outputColorSpace = ['baseColor', 'emissive'].includes(channel) ? 'srgb' : 'raw', fallbackTextures = sourceMaterials.map((entry) => { const entryColor = entry?.color || { r: .7, g: .7, b: .7 }, values = channel === 'roughness' ? [entry?.roughness ?? .5, entry?.roughness ?? .5, entry?.roughness ?? .5] : channel === 'metallic' ? [entry?.metalness ?? 0, entry?.metalness ?? 0, entry?.metalness ?? 0] : channel === 'opacity' ? [entry?.opacity ?? 1, entry?.opacity ?? 1, entry?.opacity ?? 1] : channel === 'emissive' ? [entry?.emissive?.r ?? 0, entry?.emissive?.g ?? 0, entry?.emissive?.b ?? 0] : [entryColor.r, entryColor.g, entryColor.b]; return { pixels: new Float32Array([...values, 1]), width: 1, height: 1, colorSpace: ['baseColor', 'emissive'].includes(channel) ? 'linear' : 'raw' }; }), sampledTextures = sourceTextureImages.map((entry, index) => entry || fallbackTextures[index] || fallbackTextures[0]), values = hasSourceTexture ? sampleProjectedTexture({ triangle: projection.triangle, barycentrics: projection.barycentrics, sourceIndices, sourceUVs: new Float32Array(sourceUVAttribute.array), texturePixels: sourcePixels, materialTextures: sampledTextures, sourceMaterialIndices: projection.triangle.map((face) => sourceMaterialIndices[face] || 0), width: sourceTexture?.width || 1, height: sourceTexture?.height || 1, channelIndex: ['roughness', 'metallic', 'opacity'].includes(channel) ? 0 : null, sourceColorSpace: channel === 'baseColor' || channel === 'emissive' ? sourceColorSpace : 'raw', sourceColorTransform: channel === 'baseColor' || channel === 'emissive' ? sourceColorTransform : null, destinationColorSpace: outputColorSpace }) : projectRayHitAttributes({ triangle: projection.triangle, barycentrics: projection.barycentrics, sourceIndices, sourceValues: projectedSourceValues, faceValues: sourceAttribute ? null : sourceMaterialValues, itemSize: 3, missValue: 0 }).values;
    if (channel === 'normal' && normalSpace === 'tangent') values.set(encodeProjectedTangentNormals({ values, targetTriangle: projection.targetTriangle, targetBarycentrics: projection.targetBarycentrics, targetIndices, targetNormals: projectedTargetNormals, targetTangents: projectedTargetTangents }));
    const pixels = new Uint8ClampedArray(size * size * 4), hitMask = new Uint8Array(size * size), owners = new Int32Array(projection.owners || new Int32Array(size * size).fill(-1));
    let hitCount = 0;
    for (let ray = 0; ray < projection.triangle.length; ray++) { const pixel = projection.pixels[ray], valueOffset = ray * 3, outputOffset = pixel * 4; if (projection.triangle[ray] < 0) continue; hitCount++; for (let component = 0; component < 3; component++) pixels[outputOffset + component] = Math.max(0, Math.min(255, Math.round((channel === 'normal' ? values[valueOffset + component] * .5 + .5 : values[valueOffset + component]) * 255))); pixels[outputOffset + 3] = 255; }
    for (let ray = 0; ray < projection.triangle.length; ray++) if (projection.triangle[ray] >= 0) hitMask[projection.pixels[ray]] = 1;
    const dilated = dilateProjectedPixels({ pixels, hitMask, owners, resolution: size, passes: dilation });
    const canvas = document.createElement('canvas'); canvas.width = canvas.height = size; canvas.getContext('2d').putImageData(new ImageData(dilated.pixels, size, size), 0, 0); const blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/png')); if (!blob) throw new LuciaError('LUCIA_PROJECTION_ENCODE', 'Could not encode the projected texture.');
    const safeTarget = targetPath.split('/').at(-1).toLowerCase(), safeSource = sourcePath.split('/').at(-1).toLowerCase(), name = `textures/${safeTarget}_${safeSource}_projected_${channel}_${size}.png`, total = size * size, coveredCount = projection.covered.reduce((sum, value) => sum + value, 0);
    this.project.assets.set(name, { bytes: new Uint8Array(await blob.arrayBuffer()), mimeType: 'image/png', generated: true, colorSpace: ['baseColor', 'emissive'].includes(channel) ? 'srgb' : 'linear', dataTexture: !['baseColor', 'emissive'].includes(channel), normalY: channel === 'normal' ? 'opengl' : null, normalSpace: channel === 'normal' ? normalSpace : null, bakeStats: { channel, resolution: size, covered: coveredCount, projectedHits: hitCount, dilatedTexels: dilated.filledCount, islandCount: projection.islandCount || 0, total, missedTexels: total - hitCount, coveredRatio: coveredCount / total, hitRatio: hitCount / total, rayDistance, cageOffset, dilation, projectedFrom: sourcePath, sourceTextureSampled: hasSourceTexture, sourceColorSpace, sourceColorTransform: normalizeBakeColorTransform(sourceColorTransform), outputColorSpace, normalSpace: channel === 'normal' ? normalSpace : null } });
    const dilatedTexels = dilated.filledCount;
    onProgress?.({ percentage: 100, message: `Created ${name} (${hitCount} projected hits, ${dilatedTexels} dilated texels)` }); this.project.emit(); return { source: this.session.usda, assetName: name, resolution: size, covered: coveredCount, projectedHits: hitCount, total, missedTexels: total - hitCount, coveredRatio: coveredCount / total, hitRatio: hitCount / total, dilatedTexels, islandCount: projection.islandCount || 0, dilation };
    }
  }
  releaseWorker() { this.worker?.terminate(); this.worker = null; this.workerReject = null; }
  cancel() { const reject = this.workerReject; this.releaseWorker(); reject?.(new LuciaError('LUCIA_CANCELLED', 'The worker operation was cancelled.')); }
  async bake(path, { resolution = 1024, maxResolution = 2048, dilation = 2, samples = 1, radius = 1, channel = 'baseColor', normalY = 'opengl', normalSpace = 'object' } = {}, onProgress) {
    onProgress = normalizeOperationProgressCallback(onProgress, 'Baking texture…');
    if (!new Set(['baseColor', 'normal', 'roughness', 'metallic', 'opacity', 'emissive', 'objectID', 'materialID', 'occlusion']).has(channel)) throw new LuciaError('LUCIA_BAKE_CHANNEL', `Unsupported bake channel: ${channel}`);
    if (resolution != null) validateBakeControl(resolution, { integer: true, minimum: 64, maximum: 4096 }, 'Bake resolution');
    if (maxResolution != null) validateBakeControl(maxResolution, { integer: true, minimum: 64, maximum: 4096 }, 'Bake maximum dimension');
    if (dilation != null) validateBakeControl(dilation, { integer: true, minimum: 0, maximum: 32 }, 'Bake dilation');
    if (samples != null) validateBakeControl(samples, { integer: true, minimum: 1, maximum: 16 }, 'Bake samples');
    if (radius != null) validateBakeControl(radius, { minimum: 1e-5, maximum: 1e6 }, 'Bake ray radius');
    if (channel === 'normal' && !['opengl', 'directx'].includes(normalY)) throw new LuciaError('LUCIA_BAKE_NORMAL_Y', 'Normal-map output convention must be opengl or directx.');
    if (channel === 'normal' && !['object', 'tangent'].includes(normalSpace)) throw new LuciaError('LUCIA_BAKE_NORMAL_SPACE', 'Normal-map output space must be object or tangent.');
    const object = this.renderBridge.objectForPath(path);
    const mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh?.geometry?.attributes?.uv) throw new LuciaError('LUCIA_BAKE_UV', 'The selected mesh has no UV coordinates.');
    validateBakeMeshData(mesh.geometry);
    const limit = Math.max(64, Math.min(4096, Number(maxResolution) || 2048)), size = Math.max(64, Math.min(limit, Number(resolution) || 1024)), geometry = mesh.geometry, { positions, indices } = normalizeOperationMeshGeometry(geometry), uvs = new Float32Array(geometry.attributes.uv.array), normalArray = geometry.attributes.normal?.array ? new Float32Array(geometry.attributes.normal.array) : null, baseColors = geometry.attributes.color?.array ? new Float32Array(geometry.attributes.color.array) : null, materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material], material = materials[0], color = material?.color || { r: .7, g: .7, b: .7 }, normalChannel = channel === 'normal';
    if ((normalChannel || channel === 'occlusion') && !normalArray) throw new LuciaError('LUCIA_BAKE_NORMAL', 'The selected mesh has no normals to bake.');
    const channelColor = materialChannelValues(material, channel);
    const textureProperty = { baseColor: 'map', roughness: 'roughnessMap', metallic: 'metalnessMap', opacity: 'alphaMap', emissive: 'emissiveMap' }[channel], textureImage = textureProperty ? material?.[textureProperty]?.image : null, sourcePixels = textureImage?.data || textureImage?.pixels, sourceColorTransform = material?.userData?.lightusdTextureColorTransforms?.[textureProperty] || null, texturePixels = sourcePixels && textureImage.width && textureImage.height && sourcePixels.length === textureImage.width * textureImage.height * 4 ? new (sourcePixels instanceof Float32Array ? Float32Array : Uint8Array)(sourcePixels) : null, materialTextureImages = materials.map((entry) => { const texture = textureProperty ? entry?.[textureProperty] : null, image = texture?.image, pixels = image?.data || image?.pixels, colorTransform = entry?.userData?.lightusdTextureColorTransforms?.[textureProperty] || null; return pixels && image.width && image.height && pixels.length === image.width * image.height * 4 ? { pixels: new (pixels instanceof Float32Array ? Float32Array : Uint8Array)(pixels), width: image.width, height: image.height, colorSpace: image.colorSpace || texture?.colorSpace || 'srgb', ...(colorTransform ? { colorTransform } : {}) } : null; });
    const estimatedBytes = estimateBakeWorkingBytes({ resolution: size, vertexCount: positions.length / 3, indexCount: indices.length, sourceTextureBytes: materialTextureImages.reduce((sum, entry) => sum + (entry?.pixels?.byteLength || 0), 0), occlusion: channel === 'occlusion', samples });
    if (estimatedBytes > BAKE_MEMORY_LIMIT_BYTES) throw new LuciaError('LUCIA_BAKE_MEMORY', `Bake would require about ${(estimatedBytes / (1024 * 1024)).toFixed(0)} MiB of working memory; reduce resolution or samples below the ${BAKE_MEMORY_LIMIT_BYTES / (1024 * 1024)} MiB Lucia limit.`);
    const candidateTangentArray = geometry.attributes.tangent?.array ? new Float32Array(geometry.attributes.tangent.array) : null, authoredTangentArray = candidateTangentArray?.length === positions.length / 3 * 4 && !hasInvalidTangent(candidateTangentArray) ? candidateTangentArray : null, tangentArray = normalSpace === 'tangent' ? authoredTangentArray || recomputeVertexTangents({ positions, indices, uvs, normals: normalArray }) : null, tangentSource = normalSpace === 'tangent' ? authoredTangentArray ? 'authored' : 'recomputed' : null;
    const colors = normalChannel ? encodeNormalVectors({ normals: normalArray, tangents: normalSpace === 'tangent' ? tangentArray : null, space: normalSpace }) : channel === 'baseColor' ? baseColors : null;
    const groups = geometry.groups || [], faceCount = Math.floor(indices.length / 3), materialIndices = Uint32Array.from({ length: faceCount }, (_, face) => groups.find((candidate) => face * 3 >= candidate.start && face * 3 < candidate.start + candidate.count)?.materialIndex || 0), materialColors = channel === 'normal' || channel === 'objectID' || channel === 'materialID' ? null : Float32Array.from(materials.flatMap((entry) => materialChannelValues(entry, channel))), faceColors = channel === 'objectID' ? Float32Array.from({ length: faceCount * 3 }, () => 0).map((_, index) => stableColor(path)[index % 3]) : channel === 'materialID' ? (() => { const output = new Float32Array(faceCount * 3); for (let face = 0; face < faceCount; face++) output.set(stableColor(materials[materialIndices[face]]?.uuid || materials[materialIndices[face]]?.name || `material:${materialIndices[face]}`), face * 3); return output; })() : null;
    onProgress?.({ percentage: 20, message: 'Rasterizing UV triangles…' });
    this.cancel();
    let result = await new Promise((resolve, reject) => {
      this.workerReject = reject;
      const worker = this.worker = new Worker(new URL(channel === 'occlusion' ? './ao-bake-worker.js' : './bake-worker.js', import.meta.url), { type: 'module' });
      const finish = () => { if (this.worker === worker) { worker.terminate(); this.worker = null; this.workerReject = null; } else worker.terminate(); };
      worker.onerror = (event) => { finish(); reject(new LuciaError('LUCIA_BAKE_WORKER', event.message)); };
      worker.onmessage = ({ data }) => { if (this.worker !== worker) return; if (data.type === 'result') { finish(); resolve(data); } else if (data.type === 'error') { finish(); reject(new LuciaError('LUCIA_BAKE_WORKER', data.message)); } };
      if (channel === 'occlusion') postWorkerMessage(this, worker, { type: 'bake-occlusion', positions, indices, uvs, normals: normalArray, resolution: size, samples, radius: Math.max(1e-5, Math.min(1e6, Number(radius) || 1)) }, [positions.buffer, indices.buffer, uvs.buffer, normalArray.buffer], reject, 'LUCIA_BAKE_WORKER');
      else postWorkerMessage(this, worker, { type: 'bake-base-color', positions, indices, uvs, colors, faceColors, materialColors, materialIndices, texturePixels, materialTextures: materialTextureImages, textureWidth: textureImage?.width || 0, textureHeight: textureImage?.height || 0, sourceColorSpace: textureImage?.colorSpace || (textureProperty ? material?.[textureProperty]?.colorSpace : null) || 'srgb', ...(sourceColorTransform ? { sourceColorTransform } : {}), destinationColorSpace: channel === 'baseColor' || channel === 'emissive' ? 'srgb' : 'raw', textureChannel: textureProperty && channel !== 'baseColor' && channel !== 'emissive' ? 0 : null, color: normalChannel ? [.5, .5, 1] : channelColor, resolution: size, dilation, samples }, [positions.buffer, indices.buffer, uvs.buffer, ...(colors ? [colors.buffer] : []), ...(faceColors ? [faceColors.buffer] : []), ...(materialColors ? [materialColors.buffer] : []), ...(materialIndices ? [materialIndices.buffer] : []), ...materialTextureImages.filter(Boolean).map((entry) => entry.pixels.buffer), ...(texturePixels && !materialTextureImages.some((entry) => entry?.pixels?.buffer === texturePixels.buffer) ? [texturePixels.buffer] : [])], reject, 'LUCIA_BAKE_WORKER');
    });
    this.releaseWorker();
    validateBakeResult(result);
    if (normalChannel && normalY === 'directx') result = { ...result, pixels: convertNormalMapY({ pixels: result.pixels, resolution: result.resolution, convention: normalY }) };
    const canvas = document.createElement('canvas'); canvas.width = canvas.height = result.resolution;
    canvas.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(result.pixels), result.resolution, result.resolution), 0, 0);
    onProgress?.({ percentage: 70, message: `Encoded ${result.covered} covered texels (${result.missedTexels} missed)…` });
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/png'));
    if (!blob) throw new LuciaError('LUCIA_BAKE_ENCODE', 'Could not encode the baked texture.');
    const name = `textures/${path.split('/').at(-1).toLowerCase()}_${channel}_${size}.png`;
    const alpha = analyzeBakeAlpha({ pixels: result.pixels, resolution: result.resolution }), colorSpace = channel === 'baseColor' || channel === 'emissive' ? 'srgb' : channel === 'objectID' || channel === 'materialID' ? 'raw' : 'linear';
    this.project.assets.set(name, { bytes: new Uint8Array(await blob.arrayBuffer()), mimeType: 'image/png', generated: true, colorSpace, dataTexture: colorSpace === 'linear', normalY: normalChannel ? normalY : null, normalSpace: normalChannel ? normalSpace : null, tangentSource, bakeStats: { channel, resolution: result.resolution, covered: result.covered, total: result.total, missedTexels: result.missedTexels, coveredRatio: result.coveredRatio, rasterizedFaces: result.rasterizedFaces, skippedFaces: result.skippedFaces, degenerateFaces: result.degenerateFaces, dilatedTexels: result.dilatedTexels || 0, islandCount: result.islandCount || 0, visualDifference: result.visualDifference, colorSpace, dataTexture: colorSpace === 'linear', sourceTextureSampled: Boolean(texturePixels), sourceColorTransform: normalizeBakeColorTransform(sourceColorTransform), normalY: normalChannel ? normalY : null, normalSpace: normalChannel ? normalSpace : null, tangentSource, ...alpha } });
    onProgress?.({ percentage: 100, message: `Created ${name}` });
    this.project.emit();
    return { source: this.session.usda, assetName: name, ...result };
  }
  dispose() { this.cancel(); }
}
