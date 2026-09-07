// SPDX-License-Identifier: Apache-2.0
// Geometry diagnostic adapter. Material overrides are explicit in provenance.
import { BufferGeometry, BufferAttribute, Matrix4 } from 'three';
import { LightUSDComposer } from '../lightusd/LightUSDComposer.js';
import { HttpAssetResolver } from '../../http-asset-resolver.js';
import { surfaceDocument } from './scene.js';
import { appendRectLights } from './usd-lights.js';
export const SHADERBALL_COMMIT = '3b75c2dad6a494897557dcca0098257bcf42a8c6';
let nativePromise;
// The legacy composer requests merged references without their source layer.
// Retain provenance while loading each layer; reject ambiguous authored keys.
class LayerResolver extends HttpAssetResolver {
  constructor(native, options) { super(options); this.native = native; this.sources = new Map(); }
  registerLayer(layer, source) {
    for (const method of ['extractSublayerAssetPaths', 'extractReferencesAssetPaths', 'extractPayloadAssetPaths']) {
      for (const key of layer[method]?.() || []) {
        const url = this.rewrite(key, { parentAssetPath: source });
        const prior = this.sources.get(key);
        if (prior && this.rewrite(key, { parentAssetPath: prior }) !== url) throw new Error(`Ambiguous merged asset path: ${key}`);
        this.sources.set(key, source);
      }
    }
  }
  async resolveAsync(key, options = {}) {
    const result = await super.resolveAsync(key, { parentAssetPath: options.parentAssetPath || this.sources.get(key) });
    if (/\.usd[ac]?(?:$|[?#])/.test(result[2])) {
      const layer = new this.native.LightUSDLoaderNative();
      try { if (layer.loadAsLayerFromBinary(new Uint8Array(result[1]), key)) { if (layer.hasVariants()) layer.composeVariants(); this.registerLayer(layer, result[2]); } }
      finally { layer.delete(); }
    }
    return result;
  }
}
async function nativeModule() {
  if (!nativePromise) {
    const url = new URL('../lightusd/lightusd_combined.js', import.meta.url).href;
    nativePromise = import(/* @vite-ignore */ url).then(m => m.default()).catch(e => { nativePromise = null; throw new Error(`Build the combined LightUSD WASM module first: ${e.message}`); });
  }
  return nativePromise;
}
export async function loadShaderBallGeometry(onStatus = () => {}, { authoredLights = false } = {}) {
  const url = new URL('/__assets/full_assets/StandardShaderBall/standard_shader_ball_scene.usda', location.href);
  onStatus('Loading USD module…'); const native = await nativeModule();
  const response = await fetch(url); if (!response.ok) throw new Error('ShaderBall checkout is missing');
  const layer = new native.LightUSDLoaderNative();
  try {
    layer.setLoadTextureInNative(false);
    layer.setAllowParentRelativeAssetPaths?.(true);
    if (!layer.loadAsLayerFromBinary(new Uint8Array(await response.arrayBuffer()), 'standard_shader_ball_scene.usda')) throw new Error(layer.error());
    if (!layer.applyVariantSelection('/standard_shader_ball_scene', 'surface_geometry', 'triangulated')) throw new Error('Unable to select triangulated ShaderBall geometry');
    const composer = new LightUSDComposer(); composer.setLayer(layer); composer.setUSDLoader({ native_: native });
    const resolver = new LayerResolver(native, { baseUrl: new URL('.', url).href }); resolver.registerLayer(layer, url.href); composer.setAssetResolver(resolver);
    composer.setBaseWorkingPath('./'); composer.setAssetSearchPaths(['./']);
    onStatus('Composing ShaderBall layers and references…'); await composer.progressiveComposition();
    if (layer.hasReferences() || layer.hasPayload()) throw new Error('ShaderBall composition is incomplete');
    if (!layer.getShadingGraphJSON) throw new Error('Rebuild the combined WASM module for shading graph inspection');
    const shadingGraphJSON = layer.getShadingGraphJSON();
    if (!shadingGraphJSON) throw new Error(layer.error());
    const shadingGraph = JSON.parse(shadingGraphJSON);
    if (!layer.layerToRenderScene()) throw new Error(layer.error());
    // Native material serialization is reduced, NOT an authored graph export.
    // Preserve this diagnostic snapshot without substituting it for source graphs.
    const authored = { materialSerializationIsLossy: true, shadingGraph, materials: [], lights: [], bindings: [] };
    for (let i = 0; i < layer.numMaterials(); i++) authored.materials.push(layer.getMaterialWithFormat(i, 'json'));
    for (let i = 0; i < layer.numLights(); i++) authored.lights.push(layer.getLight(i));
    const positions = [], normals = [], uvs = [], indices = [], materialIds = [];
    const read = d => {
      if (!d?.length) return null;
      const C = ({ f32: Float32Array, u32: Uint32Array, snorm8: Int8Array, snorm16: Int16Array })[d.dtype];
      if (!C) throw new Error(`Unsupported USD attribute encoding ${d.dtype}`);
      const copy = new C(native.HEAPU8.buffer, d.ptr, d.length).slice();
      if (d.dtype === 'snorm8' || d.dtype === 'snorm16') return Float32Array.from(copy, v => Math.max(-1, v / (d.dtype === 'snorm8' ? 127 : 32767)));
      return copy;
    };
    let camera;
    function visit(node) {
      const matrix = new Matrix4().fromArray(node.globalMatrix || new Matrix4().elements);
      if (node.nodeType?.toLowerCase() === 'camera') {
        const c = layer.getCamera(node.contentId), m = matrix.elements;
        camera = { origin: [m[12], m[13], m[14]], target: [m[12] - m[8], m[13] - m[9], m[14] - m[10]], fov: c.yfov * 180 / Math.PI };
      }
      if (node.nodeType?.toLowerCase() === 'mesh') {
        const mesh = layer.getMeshPtr(node.contentId);
        authored.bindings.push({ path: mesh.absPath, materialId: mesh.materialId, submeshes: mesh.submeshes });
        if (!mesh.singleIndexable || !mesh.triangulated) throw new Error(`USD mesh is not triangulated/single-indexed: ${mesh.absPath}`);
        const p = read(mesh.points), ix = read(mesh.indices); if (!p?.length || !ix?.length) return;
        const geo = new BufferGeometry(); geo.setAttribute('position', new BufferAttribute(p, 3)); geo.setIndex(new BufferAttribute(ix, 1));
        const n = read(mesh.normals), uv = read(mesh.uv0);
        if (n?.length === p.length) geo.setAttribute('normal', new BufferAttribute(n, 3)); else geo.computeVertexNormals();
        geo.applyMatrix4(matrix);
        const ps = geo.attributes.position.array, ns = geo.attributes.normal.array, offset = positions.length / 3;
        for (let i = 0; i < ps.length; i++) { positions.push(ps[i]); normals.push(ns[i]); }
        for (let i = 0; i < ps.length / 3 * 2; i++) uvs.push(uv?.[i] ?? 0);
        for (let i = 0; i < ix.length; i++) indices.push(ix[i] + offset);
        const mat = /material_surface/.test(mesh.absPath || '') ? 1 : 0;
        for (let i = 0; i < ix.length / 3; i++) materialIds.push(mat);
        geo.dispose();
      }
      for (const child of node.children || []) visit(child);
    }
    for (let i = 0; i < layer.numRootNodes(); i++) visit(layer.getRootNode(i));
    if (!indices.length) throw new Error('ShaderBall conversion produced no triangles');
    if (!camera) throw new Error('ShaderBall authored camera was not found');
    onStatus(`Prepared ${indices.length / 3} ShaderBall triangles; materials/lights are diagnostic overrides`);
    const scene={ positions, normals, uvs, indices, materialIds, authored, materials: [surfaceDocument([0.35, 0.35, 0.35], 0, 0.7), surfaceDocument([0.8, 0.45, 0.15], 1, 0.25)], camera, provenance: { asset: 'StandardShaderBall', commit: SHADERBALL_COMMIT, variant: 'triangulated', materialOverride: true, lightingOverride: true, referenceReady: false } };
    return authoredLights ? appendRectLights(scene,authored.lights) : scene;
  } finally { layer.delete(); }
}
