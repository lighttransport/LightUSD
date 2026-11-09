#!/usr/bin/env node

import assert from 'node:assert/strict';
import * as THREE from 'three';
import { LightUSDLoaderUtils, TextureLoadingManager } from '../src/lightusd/LightUSDLoaderUtils.js';

globalThis.THREE = THREE;
globalThis.requestAnimationFrame = (fn) => setTimeout(() => fn(Date.now()), 0);

assert.equal(LightUSDLoaderUtils.calculateDomeLightIntensity({ intensity: 1 }), 1);
assert.equal(LightUSDLoaderUtils.calculateDomeLightIntensity({ intensity: 1, exposure: 0 }), 1);
assert.equal(LightUSDLoaderUtils.calculateDomeLightIntensity({ intensity: 1, exposure: 2 }), 4);

{
  const texture = new THREE.Texture();
  LightUSDLoaderUtils.applyTextureMapDefaults(texture, 'map', 'lin_ap1_scene');
  assert.equal(texture.colorSpace, THREE.NoColorSpace);
  LightUSDLoaderUtils.applyTextureMapDefaults(texture, 'map',
    'srgb_p3d65_scene');
  assert.equal(texture.colorSpace, THREE.SRGBColorSpace);
  const scene = {
    getTexture: () => ({ textureImageId: 7 }),
    getImageCopy: () => ({ usdColorSpace: 'lin_acescg', colorSpace: 'lin_srgb' })
  };
  assert.equal(LightUSDLoaderUtils.textureSourceColorSpace(0, scene),
    'lin_acescg');
  const customMetadata = {
    sourceColorSpaceName: 'studio_ap0',
    usdColorSpace: 'custom',
    colorTransformValid: true,
    colorTransformApplied: false,
    colorTransformBypass: false,
    sourceColorIsData: false,
    sourceGamma: 1,
    sourceLinearBias: 0,
    sourceToDisplayLinear: [
      2.521686, -1.134130, -0.387556,
      -0.276480, 1.372719, -0.096239,
      -0.015378, -0.152975, 1.168353
    ]
  };
  const customScene = {
    getTexture: () => ({ textureImageId: 8 }),
    getImageCopy: () => customMetadata
  };
  assert.equal(LightUSDLoaderUtils.textureSourceColorSpace(0, customScene),
    'studio_ap0');
  LightUSDLoaderUtils.applyTextureMapDefaults(
    texture, 'map', 'studio_ap0', customMetadata);
  assert.equal(texture.colorSpace, THREE.NoColorSpace);
  texture.dispose();
}

function makeMesh(materialId) {
  return {
    primName: `Mesh_${materialId}`,
    points: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    faceVertexIndices: new Uint32Array([0, 1, 2]),
    materialId,
  };
}

function makeNode(name, contentId, materialId) {
  return {
    nodeType: 'mesh',
    primName: name,
    displayName: name,
    absPath: `/Root/${name}`,
    contentId,
    materialId,
  };
}

function makeMaterial(color) {
  return {
    hasUsdPreviewSurface: true,
    hasOpenPBR: false,
    surfaceShader: {
      diffuseColor: color,
      roughness: 0.5,
      metallic: 0,
    },
  };
}

{
  let getMaterialCalls = 0;
  let getMaterialWithFormatCalls = 0;
  const usdScene = {
    getMaterial(id) {
      getMaterialCalls++;
      return id === 2 ? makeMaterial([0, 0, 1]) : makeMaterial([1, 0, 0]);
    },
    getMaterialWithFormat(id) {
      getMaterialWithFormatCalls++;
      return { error: false, data: JSON.stringify(this.getMaterial(id)) };
    },
  };

  const options = {
    materialCache: new Map(),
    materialSignatureCache: new Map(),
    fastMaterials: true,
    fastMaterialMode: 'preview',
    preferredMaterialType: 'usdpreviewsurface',
    _debugState: {
      geometryMs: 0,
      materialMs: 0,
      materialSignatureMs: 0,
      materialConvertMs: 0,
      meshCreateMs: 0,
      materialCacheHits: 0,
      materialCacheMisses: 0,
      materialSignatureCacheHits: 0,
      materialSignatureCacheMisses: 0,
      singleMaterialMeshes: 0,
      multiMaterialMeshes: 0,
      subsetMaterialRefs: 0,
    },
  };

  const a = await LightUSDLoaderUtils.setupMesh(makeMesh(0), null, usdScene, options);
  const b = await LightUSDLoaderUtils.setupMesh(makeMesh(1), null, usdScene, options);
  const c = await LightUSDLoaderUtils.setupMesh(makeMesh(2), null, usdScene, options);

  assert.equal(getMaterialCalls, 3);
  assert.equal(getMaterialWithFormatCalls, 3);
  assert.equal(a.material, b.material, 'equivalent preview materials should share signature cache');
  assert.notEqual(a.material, c.material, 'different preview materials should not share signature cache');
  assert.equal(options._debugState.materialSignatureCacheHits, 1);
}

{
  const manager = new TextureLoadingManager();
  const usdScene = {};
  let loads = 0;
  const oldGetTextureFromUSD = LightUSDLoaderUtils.getTextureFromUSD;
  LightUSDLoaderUtils.getTextureFromUSD = async () => {
    loads++;
    return new THREE.Texture();
  };
  try {
    const m0 = new THREE.MeshPhysicalMaterial();
    const m1 = new THREE.MeshPhysicalMaterial();
    manager.queueTexture(m0, 'map', 7, usdScene);
    manager.queueTexture(m1, 'map', 7, usdScene);
    await manager.startLoading({ concurrency: 2 });
    assert.equal(loads, 1, 'duplicate texture IDs should share one load promise');
    assert.equal(m0.map, m1.map);
  } finally {
    LightUSDLoaderUtils.getTextureFromUSD = oldGetTextureFromUSD;
  }
}

{
  const manager = new TextureLoadingManager();
  const usdScene = {
    getTexture() {
      return { textureImageId: 4, wrapS: 'repeat', wrapT: 'repeat' };
    },
    getImageCopy() {
      return { uri: 'packed-metal-roughness.png', colorSpace: 'raw' };
    },
  };
  const material = new THREE.MeshPhysicalMaterial();
  let loads = 0;
  manager.queueTexture(material, 'metalnessMap', 4, usdScene);
  manager.queueTexture(material, 'roughnessMap', 3, usdScene);
  await manager.startLoading({
    loadTexture: async () => {
      loads++;
      return new THREE.Texture();
    },
  });
  assert.equal(loads, 1, 'packed data texture should decode once');
  assert.ok(material.metalnessMap, 'packed texture should bind metalness');
  assert.ok(material.roughnessMap, 'packed texture should bind roughness');
  assert.equal(material.metalnessMap, material.roughnessMap);
}

{
  const usdScene = {
    getTexture(id) {
      return { textureImageId: id < 10 ? 4 : 5, wrapS: 'repeat', wrapT: 'repeat' };
    },
  };
  const a = makeMaterial([1, 1, 1]);
  const b = makeMaterial([1, 1, 1]);
  const c = makeMaterial([1, 1, 1]);
  a.surfaceShader.diffuseColorTextureId = 1;
  b.surfaceShader.diffuseColorTextureId = 2;
  c.surfaceShader.diffuseColorTextureId = 11;

  assert.equal(
    LightUSDLoaderUtils.makeMaterialSignature(a, 'full', usdScene),
    LightUSDLoaderUtils.makeMaterialSignature(b, 'full', usdScene),
    'full material signatures should canonicalize texture IDs to image identity'
  );
  assert.notEqual(
    LightUSDLoaderUtils.makeMaterialSignature(a, 'full', usdScene),
    LightUSDLoaderUtils.makeMaterialSignature(c, 'full', usdScene),
    'different image identities should not share full material signatures'
  );
}

{
  const usdScene = {
    getMeshCopy(id) {
      return makeMesh(id);
    },
    getMaterial(id) {
      return makeMaterial(id === 2 ? [0, 0, 1] : [1, 0, 0]);
    },
    getMaterialWithFormat(id) {
      return { error: false, data: JSON.stringify(this.getMaterial(id)) };
    },
  };
  const root = {
    nodeType: 'xform',
    primName: 'Root',
    displayName: 'Root',
    absPath: '/Root',
    localMatrix: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    children: [
      makeNode('A', 0, 0),
      makeNode('B', 1, 1),
      makeNode('C', 2, 2),
    ],
  };
  const options = {
    fastMaterials: true,
    fastMaterialMode: 'full',
    materialCache: new Map(),
    materialSignatureCache: new Map(),
    preferredMaterialType: 'usdpreviewsurface',
    meshAggregation: 'material',
    yieldMode: 'none',
  };

  const threeRoot = await LightUSDLoaderUtils.buildThreeNode(root, null, usdScene, options);
  const meshes = [];
  threeRoot.traverse((obj) => {
    if (obj.isMesh) meshes.push(obj);
  });

  assert.equal(meshes.length, 2, 'two equivalent-material meshes should aggregate into one mesh');
  assert.equal(options._debugState.aggregateInputMeshes, 2);
  assert.equal(options._debugState.aggregateOutputMeshes, 1);
}

console.log('OK loader-utils-fast-materials');
