import { hasInvalidValue, isSupportedNumericArray } from './indexed-mesh.js';

const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';

function sourceValuesByVertex(attribute, sourceIndices, vertexCount, requireConsistent = true) {
  if (!attribute || !isSupportedNumericArray(attribute.array) || !isSupportedNumericArray(attribute.indices) || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length % attribute.itemSize || attribute.indices.length !== sourceIndices.length || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value)) || hasInvalidValue(attribute.indices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= attribute.array.length / attribute.itemSize)) throw new Error('Face-varying retopology attributes must contain finite typed values and one valid index per source corner.');
  const valueByVertex = Array(vertexCount).fill(null);
  for (let corner = 0; corner < sourceIndices.length; corner++) {
    const vertex = sourceIndices[corner], valueIndex = attribute.indices[corner], offset = valueIndex * attribute.itemSize, value = Array.from({ length: attribute.itemSize }, (_, component) => attribute.array[offset + component]);
    const previous = valueByVertex[vertex];
    if (requireConsistent && previous && previous.some((component, index) => component !== value[index])) throw new Error(`Face-varying attribute ${attribute.name || '(unnamed)'} changes across a source vertex; retopology cannot preserve that seam without face-corner provenance.`);
    valueByVertex[vertex] = value;
  }
  return valueByVertex;
}

export function remapRetopoFaceVaryingAttributes(attributes, sourceIndices, result, sourceVertexCount = result?.sourceVertexCount) {
  if (attributes == null) return [];
  if (!Array.isArray(attributes)) throw new Error('Face-varying retopology attributes must be an array.');
  if (!isIterableBuffer(sourceIndices) || !isIterableBuffer(result?.xref) || !isIterableBuffer(result?.indices) || !Number.isSafeInteger(sourceVertexCount) || sourceVertexCount < 1) throw new Error('Face-varying retopology remap requires typed source indices, xref, indices, and a positive source vertex count.');
  const maps = attributes.map((attribute) => sourceValuesByVertex(attribute, sourceIndices, sourceVertexCount));
  return attributes.map((attribute, attributeIndex) => {
    const values = maps[attributeIndex], output = new attribute.array.constructor(result.xref.length * attribute.itemSize);
    for (let vertex = 0; vertex < result.xref.length; vertex++) {
      const sourceVertex = result.xref[vertex], value = values[sourceVertex];
      if (!value) throw new Error(`Face-varying attribute ${attribute.name || '(unnamed)'} has no value for a reduced vertex.`);
      output.set(value, vertex * attribute.itemSize);
    }
    return { ...attribute, array: output, indices: new Uint32Array(result.indices), interpolation: 'faceVarying' };
  });
}

export function expandRetopoFaceVaryingMesh({ positions, indices, normals = null, uvs = null, colors = null, tangents = null, jointIndices = null, jointWeights = null, customAttributes = [], faceVaryingAttributes = [] }) {
  if (!faceVaryingAttributes.length) return { positions, indices, normals, uvs, colors, tangents, jointIndices, jointWeights, customAttributes, faceVaryingAttributes, sourceVertexCopies: Array.from({ length: positions.length / 3 }, (_, vertex) => [vertex]) };
  if (!isIterableBuffer(indices) || indices.length % 3 || !isSupportedNumericArray(indices) || !isSupportedNumericArray(positions) || positions.length % 3) throw new Error('Face-varying retopology expansion requires typed triangle and position buffers.');
  const sourceVertexCount = positions.length / 3;
  if (hasInvalidValue(indices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= sourceVertexCount)) throw new Error('Face-varying retopology expansion received an out-of-range source index.');
  for (const attribute of faceVaryingAttributes) sourceValuesByVertex(attribute, indices, sourceVertexCount, false);
  const strides = { normals: 3, uvs: 2, colors: 3, tangents: 4, jointIndices: 4, jointWeights: 4 }, aligned = { normals, uvs, colors, tangents, jointIndices, jointWeights }, output = Object.fromEntries(Object.entries(aligned).map(([name, values]) => [name, values ? [] : null])), customOutput = customAttributes.map((attribute) => ({ ...attribute, array: [] })), faceOutput = faceVaryingAttributes.map((attribute) => ({ ...attribute, array: [], indices: [] })), vertices = [], remap = new Map(), expandedIndices = [];
  for (let corner = 0; corner < indices.length; corner++) {
    const sourceVertex = indices[corner], values = faceVaryingAttributes.map((attribute) => { const valueIndex = attribute.indices[corner]; return Array.from({ length: attribute.itemSize }, (_, component) => attribute.array[valueIndex * attribute.itemSize + component]); }), key = `${sourceVertex}|${values.flat().map((value) => Object.is(value, -0) ? '-0' : String(value)).join(',')}`;
    let expandedVertex = remap.get(key);
    if (expandedVertex == null) {
      expandedVertex = vertices.length; remap.set(key, expandedVertex); vertices.push(sourceVertex);
      for (const [name, valuesBuffer] of Object.entries(aligned)) if (valuesBuffer) { const stride = strides[name]; for (let component = 0; component < stride; component++) output[name].push(valuesBuffer[sourceVertex * stride + component]); }
      for (let attributeIndex = 0; attributeIndex < customAttributes.length; attributeIndex++) { const attribute = customAttributes[attributeIndex]; for (let component = 0; component < attribute.itemSize; component++) customOutput[attributeIndex].array.push(attribute.array[sourceVertex * attribute.itemSize + component]); }
      for (let attributeIndex = 0; attributeIndex < faceVaryingAttributes.length; attributeIndex++) faceOutput[attributeIndex].array.push(...values[attributeIndex]);
    }
    expandedIndices.push(expandedVertex);
  }
  for (const attribute of faceOutput) attribute.indices = Uint32Array.from(expandedIndices);
  const sourceVertexCopies = Array.from({ length: positions.length / 3 }, () => []);
  vertices.forEach((sourceVertex, expandedVertex) => sourceVertexCopies[sourceVertex].push(expandedVertex));
  return { positions: Float32Array.from(vertices.flatMap((sourceVertex) => [positions[sourceVertex * 3], positions[sourceVertex * 3 + 1], positions[sourceVertex * 3 + 2]])), indices: Uint32Array.from(expandedIndices), ...Object.fromEntries(Object.entries(output).map(([name, values]) => [name, values && new (aligned[name].constructor)(values)])), customAttributes: customOutput.map((attribute, index) => ({ ...attribute, array: new customAttributes[index].array.constructor(attribute.array) })), faceVaryingAttributes: faceOutput.map((attribute, index) => ({ ...attribute, array: new faceVaryingAttributes[index].array.constructor(attribute.array) })), sourceVertexCopies };
}
