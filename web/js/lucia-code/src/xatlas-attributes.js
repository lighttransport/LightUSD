import { hasInvalidValue, isSupportedNumericArray } from './indexed-mesh.js';

const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';

function remapAttribute(source, xref, stride, Type) {
  let maxIndex = -1; for (const index of xref) maxIndex = Math.max(maxIndex, index);
  if (!source) return null;
  if (!isIterableBuffer(source) || typeof source.subarray !== 'function' || source.length < (maxIndex + 1) * stride) throw new Error('xatlas aligned attribute is malformed or shorter than its source remap.');
  const output = new Type(xref.length * stride);
  for (let i = 0; i < xref.length; i++) output.set(source.subarray(xref[i] * stride, xref[i] * stride + stride), i * stride);
  return output;
}

function validateXref(xref) {
  if (!isIterableBuffer(xref) || !isSupportedNumericArray(xref) || !xref.length) throw new Error('xatlas source remap must be a non-empty iterable typed numeric array.');
  for (const index of xref) if (!Number.isSafeInteger(index) || index < 0 || index > 0xffffffff) throw new Error('xatlas source remap contains an invalid vertex index.');
  return true;
}

export function remapXatlasAttributes(request = {}) {
  const { xref, normals = null, colors = null, tangents = null, jointIndices = null, jointWeights = null } = request && typeof request === 'object' ? request : {};
  validateXref(xref);
  const map = new Uint32Array(xref || []);
  return {
    normals: remapAttribute(normals, map, 3, Float32Array),
    colors: remapAttribute(colors, map, 3, Float32Array),
    tangents: remapAttribute(tangents, map, 4, Float32Array),
    jointIndices: remapAttribute(jointIndices, map, 4, Uint16Array),
    jointWeights: remapAttribute(jointWeights, map, 4, Float32Array),
  };
}

export function remapXatlasCustomAttributes(attributes = [], xref) {
  if (!Array.isArray(attributes)) throw new Error('xatlas custom attributes must be an array.');
  validateXref(xref);
  const names = new Set();
  return attributes.map((attribute) => {
    if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || !Number.isInteger(attribute?.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || !isIterableBuffer(attribute?.array) || !isSupportedNumericArray(attribute.array) || !attribute.array.length || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) throw new Error('xatlas custom attribute is malformed or must use a supported numeric typed array and be finite.');
    if (names.has(attribute.name)) throw new Error(`xatlas custom attribute name is duplicated: ${attribute.name}`);
    names.add(attribute.name);
    const itemSize = attribute.itemSize;
    const Type = attribute.array?.constructor || Float32Array;
    const array = remapAttribute(attribute.array, xref, itemSize, Type);
    return array ? { name: attribute.name, itemSize, array } : null;
  }).filter(Boolean);
}

export function collectXatlasTransferBuffers(result = {}) {
  const buffers = [], seen = new Set(), add = (value) => {
    const buffer = value?.buffer;
    if (!(buffer instanceof ArrayBuffer) || seen.has(buffer)) return;
    seen.add(buffer); buffers.push(buffer);
  };
  for (const name of ['positions', 'uvs', 'indices', 'xref', 'chartIndices', 'atlasIndices', 'normals', 'colors', 'tangents', 'jointIndices', 'jointWeights']) add(result[name]);
  for (const attribute of result.customAttributes || []) add(attribute?.array);
  return buffers;
}
