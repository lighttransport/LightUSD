import { hasInvalidValue, isSupportedNumericArray, validateIndexedMesh } from './indexed-mesh.js';

const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';

export function validateXatlasRequest(request = {}) {
  const { positions, indices, options = {}, normals = null, colors = null, tangents = null, jointIndices = null, jointWeights = null, customAttributes = [] } = request && typeof request === 'object' ? request : {};
  if (!isIterableBuffer(positions) || !isSupportedNumericArray(positions) || !isIterableBuffer(indices) || !isSupportedNumericArray(indices) || positions.length % 3 || indices.length % 3 || !positions.length || !indices.length) throw new Error('xatlas requires non-empty position and triangle-index buffers (typed buffers are required).');
  const vertexCount = positions.length / 3;
  try { validateIndexedMesh({ positions, indices }); } catch (error) { throw new Error(`xatlas mesh buffers are invalid: ${error.message}`); }
  const safeOptions = options == null ? {} : options;
  if (typeof safeOptions !== 'object' || Array.isArray(safeOptions)) throw new Error('xatlas options must be an object.');
  for (const name of ['rotateCharts', 'rotateChartsToAxis', 'blockAlign', 'bruteForce', 'singleAtlasFallback']) if (safeOptions[name] != null && typeof safeOptions[name] !== 'boolean') throw new Error(`xatlas ${name} must be boolean.`);
  if (!Array.isArray(customAttributes)) throw new Error('xatlas custom attributes must be an array.');
  if (Boolean(jointIndices) !== Boolean(jointWeights)) throw new Error('xatlas jointIndices and jointWeights must be provided together.');
  const ranges = { resolution: [64, 8192], padding: [0, 256], texelsPerUnit: [0, 1e8], maxChartSize: [0, 8192], textureSeamWeight: [0, 100], maxIterations: [1, 10000], maxCost: [0, 100] };
  for (const [name, [minimum, maximum]] of Object.entries(ranges)) if (safeOptions[name] != null && (typeof safeOptions[name] !== 'number' || !Number.isFinite(safeOptions[name]) || safeOptions[name] < minimum || safeOptions[name] > maximum)) throw new Error(`xatlas ${name} is outside the supported range ${minimum}–${maximum}.`);
  for (const [name, value, stride] of [['normals', normals, 3], ['colors', colors, 3], ['tangents', tangents, 4], ['jointIndices', jointIndices, 4], ['jointWeights', jointWeights, 4]]) {
    if (value == null) continue;
    if (!isIterableBuffer(value) || !isSupportedNumericArray(value) || value.length !== vertexCount * stride || hasInvalidValue(value, (component) => !Number.isFinite(component) || (name === 'jointIndices' && (!Number.isSafeInteger(component) || component < 0 || component > 65535)) || (name === 'jointWeights' && component < 0))) throw new Error(`xatlas ${name} must be a finite vertex-aligned buffer (typed buffers are required).`);
  }
  const customNames = new Set();
  for (const attribute of customAttributes) {
    if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || !Number.isInteger(attribute?.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || !isIterableBuffer(attribute?.array) || !isSupportedNumericArray(attribute.array) || !attribute.array.length || attribute.array.length !== vertexCount * attribute.itemSize || hasInvalidValue(attribute.array, (component) => !Number.isFinite(component))) throw new Error('xatlas custom attributes must use supported numeric typed arrays and be finite, named, and vertex-aligned.');
    if (customNames.has(attribute.name)) throw new Error(`xatlas custom attribute name is duplicated: ${attribute.name}`);
    customNames.add(attribute.name);
  }
  return true;
}

export function nextSingleAtlasOptions(options = {}) {
  if (options != null && (typeof options !== 'object' || Array.isArray(options))) throw new Error('xatlas fallback options must be an object.');
  const previousResolution = typeof options?.resolution === 'number' && Number.isFinite(options.resolution) && options.resolution > 0 ? options.resolution : 1024;
  const resolution = Math.min(8192, Math.max(64, previousResolution * 2));
  if (resolution === previousResolution) return null;
  return { ...(options || {}), resolution, texelsPerUnit: 0 };
}
