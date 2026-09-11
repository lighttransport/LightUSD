import { deriveBoundaryLocks, deriveUVSeamLocks, validateVertexLockMask } from './retopo-locks.js';
import { hasInvalidValue, normalizeIndexedMesh } from './indexed-mesh.js';

let modulePromise;
const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';

async function getModule() {
  modulePromise ||= import('../../src/lightusd/lightusd.js').then(({ default: factory }) => factory());
  return modulePromise;
}

function compactResult(source, simplified, simplifiedGroups = null) {
  const used = new Map(), xref = [], positions = [], normals = [], uvs = [], colors = [], tangents = [], jointIndices = [], jointWeights = [];
  const sourceNormals = source.normals && source.normals.length === source.positions.length ? source.normals : null;
  const sourceUvs = source.uvs && source.uvs.length === source.positions.length / 3 * 2 ? source.uvs : null;
  const mapVertex = (old) => {
    let next = used.get(old);
    if (next != null) return next;
    next = xref.length; used.set(old, next); xref.push(old);
    positions.push(source.positions[old * 3], source.positions[old * 3 + 1], source.positions[old * 3 + 2]);
    if (sourceNormals) normals.push(sourceNormals[old * 3], sourceNormals[old * 3 + 1], sourceNormals[old * 3 + 2]);
    if (sourceUvs) uvs.push(sourceUvs[old * 2], sourceUvs[old * 2 + 1]);
    if (source.colors) colors.push(source.colors[old * 3], source.colors[old * 3 + 1], source.colors[old * 3 + 2]);
    if (source.tangents) tangents.push(source.tangents[old * 4], source.tangents[old * 4 + 1], source.tangents[old * 4 + 2], source.tangents[old * 4 + 3]);
    if (source.jointIndices) jointIndices.push(source.jointIndices[old * 4], source.jointIndices[old * 4 + 1], source.jointIndices[old * 4 + 2], source.jointIndices[old * 4 + 3]);
    if (source.jointWeights) jointWeights.push(source.jointWeights[old * 4], source.jointWeights[old * 4 + 1], source.jointWeights[old * 4 + 2], source.jointWeights[old * 4 + 3]);
    for (const attribute of source.customAttributes || []) for (let component = 0; component < attribute.itemSize; component++) attribute.output.push(attribute.array[old * attribute.itemSize + component]);
    return next;
  };
  const indices = [];
  const outputGroups = [], ranges = simplifiedGroups || [{ start: 0, count: simplified.length, materialIndex: 0 }];
  for (const group of ranges) {
    const groupStart = indices.length;
    for (let i = group.start; i < group.start + group.count; i += 3) {
    const a = mapVertex(simplified[i]), b = mapVertex(simplified[i + 1]), c = mapVertex(simplified[i + 2]);
    if (a !== b && b !== c && a !== c) indices.push(a, b, c);
    }
    if (indices.length > groupStart) outputGroups.push({ start: groupStart, count: indices.length - groupStart, materialIndex: group.materialIndex });
  }
  const result = { positions: new Float32Array(positions), indices: new Uint32Array(indices), xref: Uint32Array.from(xref), groups: outputGroups, sourceVertexCount: source.positions.length / 3, before: source.indices.length / 3, after: indices.length / 3, error: simplified.error };
  if (sourceNormals) result.normals = new Float32Array(normals);
  if (sourceUvs) result.uvs = new Float32Array(uvs);
  if (source.colors) result.colors = new Float32Array(colors);
  if (source.tangents) result.tangents = new Float32Array(tangents);
  if (source.jointIndices) result.jointIndices = new Uint16Array(jointIndices);
  if (source.jointWeights) result.jointWeights = new Float32Array(jointWeights);
  if (source.customAttributes?.length) result.customAttributes = source.customAttributes.map((attribute) => ({ name: attribute.name, itemSize: attribute.itemSize, array: new attribute.array.constructor(attribute.output) }));
  return result;
}

self.onmessage = async ({ data }) => {
  if (data?.type !== 'retopo') return;
  let native = null;
  try {
    if (data.lockBorder != null && typeof data.lockBorder !== 'boolean') throw new Error('lockBorder must be boolean.');
    if (data.lockUVSeams != null && typeof data.lockUVSeams !== 'boolean') throw new Error('lockUVSeams must be boolean.');
    const inputPositions = new Float32Array(data.positions), inputIndices = data.indices ? new Uint32Array(data.indices) : null;
    const mesh = normalizeIndexedMesh({ positions: inputPositions, indices: inputIndices }), positions = mesh.positions, indices = mesh.indices;
    if (!positions.length || !indices.length || indices.length % 3) throw new Error('Mesh reduction requires a non-empty triangle mesh.');
    const vertexCount = positions.length / 3;
    if (data.customAttributes != null && !Array.isArray(data.customAttributes)) throw new Error('Mesh reduction custom attributes must be an array.');
    const validateAligned = (values, stride, label, predicate = (value) => !Number.isFinite(value)) => {
      if (values == null) return;
      if (!isIterableBuffer(values) || values.length !== vertexCount * stride || hasInvalidValue(values, predicate)) throw new Error(`Mesh reduction received an invalid ${label} buffer.`);
    };
    validateAligned(data.normals, 3, 'normal');
    validateAligned(data.uvs, 2, 'UV');
    validateAligned(data.colors, 3, 'color');
    validateAligned(data.tangents, 4, 'tangent');
    validateAligned(data.jointIndices, 4, 'joint-index', (value) => !Number.isSafeInteger(value) || value < 0 || value > 65535);
    validateAligned(data.jointWeights, 4, 'joint-weight', (value) => !Number.isFinite(value) || value < 0);
    const customNames = new Set();
    for (const attribute of data.customAttributes || []) {
      if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || customNames.has(attribute.name) || !isIterableBuffer(attribute?.array) || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length !== vertexCount * attribute.itemSize || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) throw new Error('Mesh reduction received an invalid or duplicate custom attribute buffer.');
      customNames.add(attribute.name);
    }
    const ratio = Math.max(0.05, Math.min(1, Number(data.targetRatio) || 1));
    const targetIndexCount = Math.max(3, Math.min(indices.length, Math.floor(indices.length * ratio / 3) * 3));
    const targetError = Math.max(0, Math.min(1, Number(data.targetError) || 0));
    const module = await getModule();
    native = new module.MeshoptSimplifier();
    const source = { positions, indices, normals: data.normals ? new Float32Array(data.normals) : null, uvs: data.uvs ? new Float32Array(data.uvs) : null, colors: data.colors ? new Float32Array(data.colors) : null, tangents: data.tangents ? new Float32Array(data.tangents) : null, jointIndices: data.jointIndices ? new Uint16Array(data.jointIndices) : null, jointWeights: data.jointWeights ? new Float32Array(data.jointWeights) : null, customAttributes: (data.customAttributes || []).map((attribute) => ({ ...attribute, array: new attribute.array.constructor(attribute.array), output: [] })) };
    for (const attribute of [source.colors, source.tangents, source.jointIndices, source.jointWeights]) if (attribute && attribute.length !== positions.length / 3 * (attribute === source.colors ? 3 : 4)) throw new Error('Aligned retopology attributes must match the source vertex count.');
    const suppliedLocks = validateVertexLockMask(data.locks, positions.length / 3) || new Uint8Array();
    const inputGroups = Array.isArray(data.groups) && data.groups.length ? data.groups : [{ start: 0, count: indices.length, materialIndex: 0 }];
    const locks = data.lockBorder === false ? new Uint8Array(positions.length / 3) : deriveBoundaryLocks(indices, inputGroups, positions.length / 3);
    if (uvs && data.lockUVSeams !== false) { const seamLocks = deriveUVSeamLocks(positions, uvs, positions.length / 3); for (let i = 0; i < locks.length; i++) locks[i] |= seamLocks[i]; }
    if (suppliedLocks.length) for (let i = 0; i < locks.length; i++) locks[i] |= suppliedLocks[i] ? 1 : 0;
    let covered = 0, reducedIndices = [], reducedGroups = [], weightedError = 0;
    for (const group of inputGroups) {
      const start = Number(group.start), count = Number(group.count);
      if (!Number.isInteger(start) || !Number.isInteger(count) || start !== covered || count < 3 || count % 3 || start + count > indices.length) throw new Error('Material groups must cover complete, ordered triangle ranges.');
      const groupIndices = indices.slice(start, start + count), groupTarget = Math.max(3, Math.min(count, Math.floor(count * ratio / 3) * 3));
      const simplified = native.simplify(positions, groupIndices, source.normals || new Float32Array(), source.uvs || new Float32Array(), locks, groupTarget, targetError, data.lockBorder === false ? 0 : 1);
      if (!simplified?.indices?.length) throw new Error('meshoptimizer simplification failed validation.');
      const reducedStart = reducedIndices.length; reducedIndices.push(...simplified.indices); reducedGroups.push({ start: reducedStart, count: simplified.indices.length, materialIndex: Number.isInteger(group.materialIndex) ? group.materialIndex : 0 });
      weightedError = Math.max(weightedError, Number(simplified.error) || 0); covered += count;
    }
    if (covered !== indices.length) throw new Error('Material groups must cover every triangle for safe reduction.');
    const result = compactResult(source, Uint32Array.from(reducedIndices), reducedGroups);
    result.error = weightedError;
    result.targetIndexCount = targetIndexCount;
    const transfer = [result.positions.buffer, result.indices.buffer, result.xref.buffer];
    if (result.normals) transfer.push(result.normals.buffer);
    if (result.uvs) transfer.push(result.uvs.buffer);
    for (const attribute of [result.colors, result.tangents, result.jointIndices, result.jointWeights]) if (attribute) transfer.push(attribute.buffer);
    for (const attribute of result.customAttributes || []) transfer.push(attribute.array.buffer);
    self.postMessage({ type: 'result', ...result }, transfer);
  } catch (error) {
    self.postMessage({ type: 'error', message: error?.message || String(error) });
  } finally {
    native?.delete();
  }
};
