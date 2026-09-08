// SPDX-License-Identifier: Apache-2.0
// Geometry diagnostic adapter. Material overrides are explicit in provenance.
import { BufferGeometry, BufferAttribute, Matrix4 } from 'three';
import { LightUSDComposer } from '../lightusd/LightUSDComposer.js';
import { HttpAssetResolver } from '../../http-asset-resolver.js';
import { surfaceDocument } from './scene.js';
import { appendRectLights } from './usd-lights.js';
import { USDTextureSources } from './usd-texture-sources.js';
import { loadUSDMaterialXLibrary, materialXFromUSD } from './usd-graph.js';
import { compileGraph } from './graph.js';
import { fetchResource, decodeImage, inspectEXRHeader } from './resources.js';
export const SHADERBALL_COMMIT = '3b75c2dad6a494897557dcca0098257bcf42a8c6';
function materialNodes(document) {
  const nodes = [...(document?.nodes || [])];
  const visit = graph => {
    nodes.push(...(graph?.nodes || []));
    for (const child of Object.values(graph?.graphs || {})) visit(child);
  };
  for (const graph of Object.values(document?.graphs || {})) visit(graph);
  return nodes;
}
/** Return static image resource keys used by translated MaterialX nodes. */
export function materialImageKeys(document) {
  const keys = new Set();
  for (const node of materialNodes(document)) {
    for (const input of ['file', 'filex', 'filey', 'filez']) {
      const port = node.inputs?.[input];
      if (port?.type === 'filename' && typeof port.value === 'string' && port.value) keys.add(port.value);
    }
  }
  return [...keys];
}
/** Return the single authored UV slot used by a material graph. */
export function materialUVIndex(document) {
  const slots = new Set();
  for (const node of materialNodes(document)) {
    if (node.category === 'texcoord' && node.inputs?.index?.value !== undefined) slots.add(Number(node.inputs.index.value));
    if (node.category === 'UsdPrimvarReader' || node.category === 'geompropvalue' || node.category === 'geompropvalueuniform') {
      const name = String(node.inputs?.varname?.value ?? node.inputs?.geomprop?.value ?? '').toLowerCase().replace(/[_-]/g, '');
      const match = name.match(/^(?:uv|uvset)([0-9]+)$/);
      if (match) slots.add(Number(match[1]));
    }
  }
  if (slots.size > 1) throw new Error(`Material graph uses multiple UV slots: ${[...slots].join(', ')}`);
  const value = slots.values().next().value;
  if (value !== undefined && (!Number.isInteger(value) || value < 0 || value > 31)) throw new Error('Material UV slot must be an integer in [0,31]');
  return value ?? 0;
}
/** Return static non-standard geometry properties used by a graph. */
export function materialGeompropNames(document) {
  const standard = new Set(['st','uv','uv0','texcoord','texcoord0','p','position','n','normal','t','tangent','b','bitangent','color','displaycolor','opacity','displayopacity']);
  const names = [...new Set(materialNodes(document).flatMap(node => {
    if (!['UsdPrimvarReader','geompropvalue','geompropvalueuniform'].includes(node.category)) return [];
    const raw = node.category === 'UsdPrimvarReader' ? node.inputs?.varname?.value : node.inputs?.geomprop?.value;
    if (typeof raw !== 'string') return [];
    const name = raw.toLowerCase().replace(/[_-]/g, '');
    return standard.has(name) || /^(?:uv|uvset)[0-9]+$/.test(name) ? [] : [raw];
  }))];
  if (names.length > 3) throw new Error(`Material graph uses more than three custom geometry properties: ${names.join(', ')}`);
  return names;
}
/** Return the first custom geometry property for compatibility with older callers. */
export function materialGeompropName(document) {
  return materialGeompropNames(document)[0] || '';
}
function decodeCustomPrimvar(item, vertexCount) {
  if (!item?.value || item.error) return null;
  const type = String(item.value.type || item.type || '').replace(/\[\]$/, '').toLowerCase();
  const components = type === 'float' ? 1 : ['float2','half2'].includes(type) ? 2 : ['float3','half3','color3f','normal3f','point3f','vector3f'].includes(type) ? 3 : ['float4','half4','color4f','vector4f'].includes(type) ? 4 : 0;
  if (!components || !['constant','vertex','varying'].includes(item.interpolation)) return null;
  const raw = item.value.value;
  const values = Array.isArray(raw) ? raw : [raw];
  const one = value => {
    const a = Array.isArray(value) ? value.map(Number) : [Number(value)];
    if (a.length !== components || a.some(v => !Number.isFinite(v))) return null;
    return components === 4 ? a : [...a, 0, 0, 1].slice(0, 3).concat([1]);
  };
  const out = new Array(vertexCount * 4).fill(0);
  if (item.interpolation === 'constant') {
    const value = one(values[0]); if (!value) return null;
    for (let i = 0; i < vertexCount; i++) out.splice(i * 4, 4, ...value);
    return out;
  }
  if (values.length !== vertexCount) return null;
  for (let i = 0; i < vertexCount; i++) {
    const value = one(values[i]); if (!value) return null;
    out.splice(i * 4, 4, ...value);
  }
  return out;
}
/** Expand native index-range submeshes into one material ID per triangle. */
export function triangleMaterialIds(indexCount, fallback, submeshes = []) {
  if (!Number.isInteger(indexCount) || indexCount < 0 || indexCount % 3) throw new Error('index count must be a nonnegative multiple of three');
  if (!Number.isInteger(fallback) || fallback < 0) throw new Error('fallback material ID must be nonnegative');
  const ids = new Array(indexCount / 3).fill(fallback), occupied = new Uint8Array(indexCount / 3);
  for (const part of submeshes || []) {
    const start = Number(part?.start), count = Number(part?.count), material = Number(part?.materialId);
    if (!Number.isInteger(start) || !Number.isInteger(count) || !Number.isInteger(material) || start < 0 || count <= 0 || material < 0 || start % 3 || count % 3 || start + count > indexCount) throw new Error('invalid mesh material submesh');
    for (let i = start / 3, end = (start + count) / 3; i < end; i++) {
      if (occupied[i]) throw new Error('overlapping mesh material submeshes');
      occupied[i] = 1; ids[i] = material;
    }
  }
  return ids;
}
let nativePromise;
// The legacy composer requests merged references without their source layer.
// Retain provenance while loading each layer; reject ambiguous authored keys.
class LayerResolver extends HttpAssetResolver {
  constructor(native, options) { super(options); this.native = native; this.sources = new Map(); this.textures = new USDTextureSources(); }
  registerLayer(layer, source) {
    if (!layer.getShadingGraphJSON) throw new Error('Rebuild the combined WASM module for shading graph inspection');
    const snapshot = layer.getShadingGraphJSON();
    if (!snapshot) throw new Error(layer.error());
    this.textures.register(JSON.parse(snapshot), source);
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
export async function loadShaderBallGeometry(onStatus = () => {}, { authoredLights = false, authoredMaterials = false } = {}) {
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
    const primvarSnapshots = {};
    const collectPrimvars = node => {
      if (node.nodeType?.toLowerCase() === 'mesh' && layer.getMeshPrimvarsJSON) {
        try { const snapshot = JSON.parse(layer.getMeshPrimvarsJSON(node.contentId)); if (snapshot?.primPath) primvarSnapshots[snapshot.primPath] = snapshot; } catch { /* keep mesh loading available */ }
      }
      for (const child of node.children || []) collectPrimvars(child);
    };
    if (layer.getMeshPrimvarsJSON) for (let i = 0; i < layer.numRootNodes(); i++) collectPrimvars(layer.getRootNode(i));
    const availablePrimvars = new Set(Object.values(primvarSnapshots).flatMap(snapshot => Object.keys(snapshot.primvars || {})));
    let mtlxLibrary, translatedMaterials = {}, compiledMaterials = {}, authoredImages = {}, imageDescriptors = {}, textureDiagnostics = [], translationDiagnostics = [];
    if (authoredMaterials) {
      mtlxLibrary = await loadUSDMaterialXLibrary();
      for (const material of shadingGraph.prims.filter(prim => prim.type === 'Material')) {
        try {
          const document = materialXFromUSD(shadingGraph, material.path, { library: mtlxLibrary, resolveAsset: resolver.textures.resolveAsset });
          const geompropNames = materialGeompropNames(document).filter(name => availablePrimvars.has(name));
          document.geompropNames = geompropNames;
          document.geompropName = geompropNames[0] || '';
          translatedMaterials[material.path] = document;
        } catch (error) { translationDiagnostics.push({ path: material.path, error: String(error.message || error) }); }
      }
      const neededAssetKeys = new Set(Object.values(translatedMaterials).flatMap(materialImageKeys));
      for (const [key, request] of resolver.textures.requests) {
        if (!neededAssetKeys.has(key)) continue;
        try {
          const bytes = await fetchResource(request.url);
          let resizedFrom;
          if (/\.exr(?:$|[?#])/i.test(request.url)) resizedFrom = inspectEXRHeader(bytes, Number.MAX_SAFE_INTEGER).dimensions;
          const image = await decodeImage(bytes, { filename: request.url, colorspace: request.colorspace, maxPixels: 256 * 1024, allowDownsample: true });
          if (resizedFrom && (resizedFrom.width !== image.width || resizedFrom.height !== image.height)) textureDiagnostics.push({ key, url: request.url, diagnostic: 'bounded downsample applied', resizedFrom, size: { width: image.width, height: image.height } });
          authoredImages[key] = image;
          imageDescriptors[key] = { offset: 0, width: image.width, height: image.height, levels: 1, colorspace: request.colorspace };
        } catch (error) { textureDiagnostics.push({ key, url: request.url, error: String(error.message || error) }); }
      }
      for (const [path, document] of Object.entries(translatedMaterials)) {
        try { document.uvIndex = materialUVIndex(document); compiledMaterials[path] = compileGraph(document, { material: true, output: document.output, imageDescriptors, uvIndex: document.uvIndex, geompropNames: document.geompropNames || [] }); }
        catch (error) { translationDiagnostics.push({ path, phase: 'compile', error: String(error.message || error) }); }
      }
    }
    // Native material serialization is reduced, NOT an authored graph export.
    // Preserve this diagnostic snapshot without substituting it for source graphs.
    const authored = { materialSerializationIsLossy: true, shadingGraph, translatedMaterials, compiledMaterials, translationDiagnostics, textureDiagnostics, textureSources: resolver.textures.snapshot(), primvarSnapshots, materials: [], lights: [], bindings: [] };
    for (let i = 0; i < layer.numMaterials(); i++) {
      const serialized = layer.getMaterialWithFormat(i, 'json');
      let material = serialized;
      try { material = JSON.parse(serialized.data || serialized); } catch { /* retain serializer diagnostics */ }
      authored.materials.push({ id: i, path: material.abs_path || material.path || '', serialized });
    }
    authored.materialPaths = Object.fromEntries(authored.materials.filter(m => m.path).map(m => [m.path, m.id]));
    const authoredDocuments = {};
    if (authoredMaterials) for (const material of authored.materials) {
      const document = translatedMaterials[material.path];
      if (document && compiledMaterials[material.path]) {
        const used = new Set(materialImageKeys(document));
        document.images = Object.fromEntries([...used].filter(key => authoredImages[key]).map(key => [key, authoredImages[key]]));
        authoredDocuments[material.id] = document;
      }
    }
    for (let i = 0; i < layer.numLights(); i++) authored.lights.push(layer.getLight(i));
    const customGeompropNames = [...new Set(Object.values(authoredDocuments).flatMap(document => document.geompropNames || (document.geompropName ? [document.geompropName] : [])))];
    const geompropSets = Object.fromEntries(customGeompropNames.map(name => [name, []]));
    const positions = [], normals = [], uvs = [], uvSets = [], tangents = [], colors = [], indices = [], materialIds = [];
    const read = d => {
      if (!d?.length) return null;
      if (ArrayBuffer.isView(d)) return d.slice();
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
        authored.bindings.push({ path: mesh.absPath, materialId: mesh.materialId, submeshes: mesh.submeshes || [], hasSubmeshes: !!mesh.hasSubmeshes });
        if (!mesh.singleIndexable || !mesh.triangulated) throw new Error(`USD mesh is not triangulated/single-indexed: ${mesh.absPath}`);
        const p = read(mesh.points), ix = read(mesh.indices); if (!p?.length || !ix?.length) return;
        const geo = new BufferGeometry(); geo.setAttribute('position', new BufferAttribute(p, 3)); geo.setIndex(new BufferAttribute(ix, 1));
        const n = read(mesh.normals), uv = read(mesh.uv0), tangent = read(mesh.tangents), color = read(mesh.colors || mesh.color), opacity = read(mesh.colorOpacities);
        const meshUVSets = [];
        for (const [slot, value] of Object.entries(mesh.uvSets || {})) meshUVSets[Number(slot)] = read(value);
        const uvSlotCount = Math.max(uvSets.length, meshUVSets.length, 1);
        const priorVertexCount = positions.length / 3;
        while (uvSets.length < uvSlotCount) uvSets.push(new Array(priorVertexCount * 2).fill(0));
        if (n?.length === p.length) geo.setAttribute('normal', new BufferAttribute(n, 3)); else geo.computeVertexNormals();
        geo.applyMatrix4(matrix);
        const ps = geo.attributes.position.array, ns = geo.attributes.normal.array, offset = positions.length / 3;
        for (let i = 0; i < ps.length; i++) { positions.push(ps[i]); normals.push(ns[i]); }
        for (let i = 0; i < ps.length / 3 * 4; i++) tangents.push(tangent?.[i] ?? (i % 4 === 3 ? 1 : 0));
        const vertexCount = ps.length / 3, snapshot = primvarSnapshots[mesh.absPath];
        for (const name of customGeompropNames) {
          const values = decodeCustomPrimvar(snapshot?.primvars?.[name], vertexCount) || new Array(vertexCount * 4).fill(0);
          geompropSets[name].push(...values);
        }
        for (let i = 0; i < ps.length / 3 * 2; i++) uvs.push(uv?.[i] ?? 0);
        for (let slot = 0; slot < uvSets.length; slot++) {
          const values = meshUVSets[slot] || (slot === 0 ? uv : null);
          for (let i = 0; i < ps.length / 3 * 2; i++) uvSets[slot].push(values?.[i] ?? 0);
        }
        for (let i = 0; i < ps.length / 3; i++) {
          if (color?.length === ps.length / 3 * 4) colors.push(color[i * 4], color[i * 4 + 1], color[i * 4 + 2], color[i * 4 + 3]);
          else if (color?.length === ps.length / 3 * 3) colors.push(color[i * 3], color[i * 3 + 1], color[i * 3 + 2], opacity?.[i] ?? 1);
          else colors.push(0, 0, 0, 1);
        }
        for (let i = 0; i < ix.length; i++) indices.push(ix[i] + offset);
        const mat = Number.isInteger(mesh.materialId) && mesh.materialId >= 0 ? mesh.materialId : 0;
        materialIds.push(...triangleMaterialIds(ix.length, mat, mesh.submeshes));
        geo.dispose();
      }
      for (const child of node.children || []) visit(child);
    }
    for (let i = 0; i < layer.numRootNodes(); i++) visit(layer.getRootNode(i));
    if (!indices.length) throw new Error('ShaderBall conversion produced no triangles');
    if (!camera) throw new Error('ShaderBall authored camera was not found');
    onStatus(`Prepared ${indices.length / 3} ShaderBall triangles; ${authoredMaterials ? `${Object.keys(authoredDocuments).length} compiled authored MaterialX slots enabled` : 'materials/lights are diagnostic overrides'}`);
    const materialCount = Math.max(2, ...materialIds.map(id => id + 1), ...authored.bindings.map(binding => Number.isInteger(binding.materialId) && binding.materialId >= 0 ? binding.materialId + 1 : 0));
    const materials = Array.from({ length: materialCount }, (_, id) => authoredDocuments[id] || (id === 1 ? surfaceDocument([0.8, 0.45, 0.15], 1, 0.25) : surfaceDocument([0.35, 0.35, 0.35], 0, 0.7)));
    const scene={ positions, normals, uvs, uvSets, tangents, colors, indices, materialIds, geompropSets, authored, materials, camera, provenance: { asset: 'StandardShaderBall', commit: SHADERBALL_COMMIT, variant: 'triangulated', materialOverride: authoredMaterials ? 'partial-authored' : true, authoredMaterialCount: Object.keys(authoredDocuments).length, lightingOverride: true, referenceReady: false } };
    return authoredLights ? appendRectLights(scene,authored.lights) : scene;
  } finally { layer.delete(); }
}
