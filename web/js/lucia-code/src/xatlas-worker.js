let modulePromise;
import { collectXatlasTransferBuffers, remapXatlasAttributes, remapXatlasCustomAttributes } from './xatlas-attributes.js';
import { nextSingleAtlasOptions, validateXatlasRequest } from './xatlas-input.js';
import { normalizeIndexedMesh } from './indexed-mesh.js';

async function getModule() {
  modulePromise ||= import('../../src/lightusd/lightusd.js').then(({ default: factory }) => factory());
  return modulePromise;
}

self.onmessage = async ({ data }) => {
  if (data?.type !== 'unwrap') return;
  let native = null;
  try {
    validateXatlasRequest(data);
    const mesh = normalizeIndexedMesh({ positions: data.positions, indices: data.indices });
    const module = await getModule();
    native = new module.XAtlasNative();
    let result = native.generate(mesh.positions, mesh.indices, data.options || {});
    if (result.error) throw new Error(result.error);
    if (result.atlasCount > 1 && data.options?.singleAtlasFallback !== false) {
      let fallbackOptions = { ...(data.options || {}), texelsPerUnit: 0 };
      for (let attempt = 0; attempt < 3 && result.atlasCount > 1; attempt++) {
        const nextOptions = nextSingleAtlasOptions(fallbackOptions);
        if (!nextOptions) break;
        fallbackOptions = nextOptions;
        const retry = native.generate(mesh.positions, mesh.indices, fallbackOptions);
        if (retry.error) break;
        result = retry;
      }
      if (result.atlasCount === 1) result.singleAtlasFallback = true;
    }
    if (!result.positions || !result.uvs || !result.indices || !result.xref) throw new Error('xatlas returned incomplete atlas buffers.');
    // Keep the source cardinality alongside the remap.  The result crosses a
    // worker boundary, so the consumer cannot otherwise distinguish a valid
    // source vertex index from a fabricated one when no aligned attributes are
    // present to exercise the remapper.
    result.sourceVertexCount = mesh.vertexCount;
    const sourceNormals = data.normals && data.normals.length === mesh.positions.length ? new Float32Array(data.normals) : null;
    const sourceColors = data.colors && data.colors.length === mesh.positions.length ? new Float32Array(data.colors) : null;
    const sourceTangents = data.tangents && data.tangents.length === mesh.vertexCount * 4 ? new Float32Array(data.tangents) : null;
    const sourceJointIndices = data.jointIndices && data.jointIndices.length === mesh.vertexCount * 4 ? new Uint16Array(data.jointIndices) : null;
    const sourceJointWeights = data.jointWeights && data.jointWeights.length === mesh.vertexCount * 4 ? new Float32Array(data.jointWeights) : null;
    if (result.xref?.length) {
      Object.assign(result, remapXatlasAttributes({ xref: result.xref, normals: sourceNormals, colors: sourceColors, tangents: sourceTangents, jointIndices: sourceJointIndices, jointWeights: sourceJointWeights }));
      result.customAttributes = remapXatlasCustomAttributes(data.customAttributes, result.xref);
    }
    const transfer = collectXatlasTransferBuffers(result);
    self.postMessage({ type: 'result', result }, transfer);
  } catch (error) {
    self.postMessage({ type: 'error', message: error?.message || String(error) });
  } finally {
    native?.delete();
  }
};
