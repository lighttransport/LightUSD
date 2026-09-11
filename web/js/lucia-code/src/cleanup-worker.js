import { cleanupMesh } from './mesh-cleanup.js';

self.onmessage = ({ data }) => {
  if (data?.type !== 'cleanup') return;
  try {
    const result = cleanupMesh(data), transfer = [result.positions.buffer, result.indices.buffer];
    if (result.normals) transfer.push(result.normals.buffer);
    if (result.uvs) transfer.push(result.uvs.buffer);
    if (result.uvIndices) transfer.push(result.uvIndices.buffer);
    if (result.colors) transfer.push(result.colors.buffer);
    if (result.tangents) transfer.push(result.tangents.buffer);
    if (result.jointIndices) transfer.push(result.jointIndices.buffer);
    if (result.jointWeights) transfer.push(result.jointWeights.buffer);
    for (const attribute of result.customAttributes || []) transfer.push(attribute.array.buffer);
    for (const attribute of result.faceVaryingAttributes || []) { transfer.push(attribute.array.buffer, attribute.indices.buffer); }
    self.postMessage({ type: 'result', ...result }, transfer);
  } catch (error) {
    self.postMessage({ type: 'error', message: error?.message || String(error) });
  }
};
