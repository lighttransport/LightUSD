import { workerPayloadBytes } from './worker-policy.js';
import { LuciaError } from './utils.js';

const textureSlots = ['map', 'normalMap', 'roughnessMap', 'metalnessMap', 'aoMap', 'emissiveMap', 'alphaMap', 'bumpMap', 'displacementMap'];
const materialFields = ['uuid', 'name', 'type', 'isMeshPhysicalMaterial', 'isMeshStandardMaterial', 'isMeshBasicMaterial', 'roughness', 'metalness', 'opacity', 'transparent', 'side', 'alphaTest', 'emissiveIntensity'];

export function snapshotHealthInput(root, maxBytes = 256 * 1024 * 1024) {
  if (!Number.isSafeInteger(maxBytes) || maxBytes < 1 || maxBytes > 256 * 1024 * 1024) throw new LuciaError('LUCIA_ANALYSIS_MEMORY', 'Invalid Health analysis memory budget.');
  const nodes = [], buffers = new Set();
  let bytes = 0;
  const numeric = array => {
    if (ArrayBuffer.isView(array) && !buffers.has(array.buffer)) {
      buffers.add(array.buffer); bytes += array.buffer.byteLength;
      if (bytes > maxBytes) throw new LuciaError('LUCIA_ANALYSIS_MEMORY', 'Health analysis exceeds the 256 MiB worker input budget.');
    }
    return array;
  };
  const vector = value => value ? { x: value.x, y: value.y, z: value.z } : undefined;
  const attribute = value => value ? { array: numeric(value.array), count: value.count, itemSize: value.itemSize } : undefined;
  const material = value => {
    if (!value) return value;
    const result = Object.fromEntries(materialFields.map(key => [key, value[key]]));
    for (const key of ['color', 'emissive']) result[key] = value[key]?.isColor ? value[key].getHexString() : value[key];
    result.userData = { nodes: value.userData?.nodes };
    result.nodes = value.nodes;
    for (const key of textureSlots) {
      const texture = value[key];
      if (!texture) continue;
      const image = texture.image;
      result[key] = { uuid: texture.uuid, colorSpace: texture.colorSpace, normalY: texture.normalY,
        userData: { normalY: texture.userData?.normalY }, image: image ? {
          width: image.width, height: image.height, colorSpace: image.colorSpace, normalY: image.normalY,
          data: numeric(image.data), pixels: numeric(image.pixels) } : null };
    }
    return result;
  };
  root?.traverse?.(object => {
    if (nodes.length >= 100000) throw new LuciaError('LUCIA_ANALYSIS_MEMORY', 'Health analysis exceeds 100000 scene nodes.');
    if (!object || typeof object !== 'object' || Array.isArray(object)) { nodes.push(object); return; }
    const node = { name: object.name, isMesh: object.isMesh,
      userData: { 'primMeta.absPath': object.userData?.['primMeta.absPath'] },
      position: vector(object.position), rotation: vector(object.rotation), scale: vector(object.scale) };
    if (object.isMesh) {
      node.geometry = { attributes: Object.fromEntries(Object.entries(object.geometry?.attributes || {}).map(([name, value]) => [name, attribute(value)])),
        index: attribute(object.geometry?.index), groups: object.geometry?.groups };
      node.material = Array.isArray(object.material) ? object.material.map(material) : material(object.material);
      const skeleton = object.skeleton;
      if (skeleton) {
        if (skeleton.bones?.length > 100000) throw new LuciaError('LUCIA_ANALYSIS_MEMORY', 'Health analysis exceeds 100000 bones.');
        const boneIndices = new Map((skeleton.bones || []).map((bone, index) => [bone, index]));
        const bones = skeleton.bones?.map(bone => ({ name: bone.name, matrixWorld: { elements: bone.matrixWorld?.elements } })) || [];
        bones.forEach((bone, index) => { const original = skeleton.bones[index].parent, parent = boneIndices.get(original); bone.parent = parent == null ? original ? {} : null : bones[parent]; });
        node.skeleton = { bones, boneMatrices: numeric(skeleton.boneMatrices), boneInverses: skeleton.boneInverses?.map(matrix => ({ elements: matrix.elements })) };
      }
    }
    nodes.push(node);
  });
  return { nodes, bytes };
}

export class HealthAnalysis {
  cancel() { this.pending?.resolve(null); this.pending?.worker.terminate(); this.pending = null; }
  async analyze(root) {
    this.cancel();
    const input = snapshotHealthInput(root);
    workerPayloadBytes(input.nodes);
    return new Promise((resolve, reject) => {
      const worker = new Worker(new URL('./health-worker.js', import.meta.url), { type: 'module' });
      const timer = setTimeout(() => finish(new LuciaError('LUCIA_ANALYSIS_TIMEOUT', 'Health analysis timed out.')), 60000);
      const finish = (error, report = null) => {
        clearTimeout(timer); worker.terminate();
        if (this.pending?.worker === worker) this.pending = null;
        error ? reject(error) : resolve(report);
      };
      this.pending = { worker, resolve: () => finish(null) };
      worker.onmessage = ({ data }) => data?.error ? finish(new LuciaError('LUCIA_ANALYSIS', data.error)) : finish(null, data.report);
      worker.onerror = event => { event.preventDefault(); finish(new LuciaError('LUCIA_ANALYSIS', event.message || 'Health worker failed.')); };
      worker.onmessageerror = () => finish(new LuciaError('LUCIA_ANALYSIS', 'Health worker returned an unreadable result.'));
      try { worker.postMessage({ nodes: input.nodes }); } catch (error) { finish(error); }
    });
  }
}
