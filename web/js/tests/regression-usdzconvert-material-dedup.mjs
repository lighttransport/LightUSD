// Regression tests for usdzconvert and material-dedup/next RenderStream paths.
// Fixtures are deliberately anonymous: Prim/Mat/Texture names only.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import * as THREE from 'three';

import {
  buildUSDZWithNewRoot,
  ZipStreamWriter,
  convertFolderToUSDZ,
  loadWasm,
  nextFlattenViaStreaming,
  parseUSDZEntries,
  unpackUSDZ,
} from '../src/usdzconvert.js';
import {
  NextRenderSceneAdapter,
} from '../src/lightusd/LightUSDLoader.js';
import {
  buildNextThreeNode,
  findNextArchiveEntry,
  loadNextArchiveTexture,
  nextArchiveUDIMLayout,
} from '../src/lightusd/NextRenderSceneUtils.js';
import { extractSkinnedMeshData } from '../src/lightusd/USDSceneSkinningData.js';
import { buildSkeletonDataFromUSD } from '../src/lightusd/USDSkeletonData.js';
import { applyUSDSceneSkinningPipeline } from '../src/lightusd/USDSceneSkinningPipeline.js';
import {
  NextTextureLoadingManager,
  compactMaterialGroups,
  createNextMaterial,
  nextTextureWrapMode,
} from '../src/lightusd/NextRenderSceneUtils.js';
import {
  INVALID_FACE_INDEX_USDA,
  ORIENTED_TRIANGLE_USDA,
  SIMPLE_TRIANGLE_USDA,
  TEXTURED_TWO_MATERIAL_USDA,
} from './fixtures/regression-fixtures.mjs';

const encoder = new TextEncoder();
let passed = 0;
let failed = 0;

async function test(name, fn) {
  try {
    await fn();
    passed++;
    console.log(`ok - ${name}`);
  } catch (error) {
    failed++;
    console.error(`not ok - ${name}`);
    console.error(error);
  }
}

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const glue = wasm64 ? '../src/lightusd/lightusd_64.js' : '../src/lightusd/lightusd.js';
const native = await loadWasm(() => import(new URL(glue, import.meta.url).href));
const nextGlue = wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js';
const nextNative = await loadWasm(
  () => import(new URL(nextGlue, import.meta.url).href),
  { locateFile: (file) => new URL('../src/lightusd/' + file, import.meta.url).href }
);

function makePngTexture() {
  const png = native.repackChannels({
    channels: 3,
    width: 4,
    height: 4,
    r: { const: 190 },
    g: { const: 120 },
    b: { const: 40 },
  });
  assert.ok(png && png.success, 'fixture PNG generation should succeed');
  return new Uint8Array(png.data);
}

async function makeUsdz(usda, extraEntries = {}, options = {}) {
  const map = new Map([['scene.usda', encoder.encode(usda)]]);
  for (const [name, bytes] of Object.entries(extraEntries)) {
    map.set(name, bytes);
  }
  return convertFolderToUSDZ(native, map, {
    rootPath: 'scene.usda',
    rootLayerFormat: 'usdc',
    reencode: false,
    ...options,
  });
}

function rootUsdcFromUsdz(usdz) {
  const { entries } = unpackUSDZ(usdz);
  const root = entries.get('root.usdc') || entries.get('scene.usdc');
  assert.ok(root instanceof Uint8Array && root.length > 0, 'USDC root should be present');
  return root;
}

function assertReloadable(usdz, label) {
  const usd = new native.LightUSDLoaderNative();
  try {
    assert.ok(usd.loadFromBinary(usdz, `${label}.usdz`),
      `${label} should reload: ${usd.error()}`);
  } finally {
    usd.delete();
  }
}

function renderStreamFor(usdcBytes, options = {}) {
  const stream = new native.RenderStream();
  if (typeof stream.setMaterialDedup === 'function') {
    stream.setMaterialDedup(!!options.materialDedup);
  }
  if (typeof stream.setMeshMerge === 'function') {
    stream.setMeshMerge(!!options.mergeMeshes);
  }
  if (typeof stream.setMeshMergeBakeTransform === 'function') {
    stream.setMeshMergeBakeTransform(options.mergeMeshesBakeTransform !== false);
  }
  const begin = stream.begin(usdcBytes);
  assert.ok(begin && begin.success, `RenderStream begin should succeed: ${begin?.error || ''}`);
  return stream;
}

function materializeAllMeshes(stream) {
  const count = stream.meshCount();
  for (let i = 0; i < count; i++) {
    const mesh = stream.getMesh(i);
    assert.ok(mesh && !mesh.error, `mesh ${i} should materialize: ${mesh?.error || ''}`);
  }
}

await test('combined WASM next RenderStream accepts an in-memory USDA layer', () => {
  const stream = renderStreamFor(encoder.encode(SIMPLE_TRIANGLE_USDA));
  try {
    assert.equal(stream.meshCount(), 1, 'USDA fixture should expose one mesh');
    materializeAllMeshes(stream);
  } finally {
    stream.end();
    stream.delete();
  }
});

await test('next default material matches legacy fallback shading', () => {
  const material = createNextMaterial({
    material: { primPath: '__default', baseColor: [0.8, 0.8, 0.8] },
    texturePaths: {}
  }, null, null, true);
  try {
    assert.equal(material.type, 'MeshStandardMaterial');
    assert.equal(material.color.getHex(), 0x888888);
    assert.equal(material.roughness, 0.6);
    assert.equal(material.metalness, 0);
  } finally {
    material.dispose();
  }
});

await test('next scene binds USD skeleton meshes as SkinnedMesh', async () => {
  const source = fs.readFileSync(
    new URL('../../../models/synthetic-skin-16influences.usda', import.meta.url), 'utf8')
    .replace('def SkelRoot "root"\n{', `def SkelRoot "root"
{
    double3 xformOp:scale = (0.01, 0.01, 0.01)
    float3 xformOp:rotateXYZ = (90, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:scale", "xformOp:rotateXYZ"]`);
  const bytes = encoder.encode(source);
  const usd = await NextRenderSceneAdapter.create(
    nextNative, bytes, 'synthetic-skin-16influences.usda', { meshOnly: false });
  try {
    const skinData = extractSkinnedMeshData(usd, { verbose: false });
    assert.ok(skinData.hasSkinnedMeshData, 'fixture should expose skin attributes');
    const skeletonData = buildSkeletonDataFromUSD(usd, {
      hasSkinnedMeshData: true,
      logger: { log() {}, warn() {}, error() {} }
    });
    assert.equal(skeletonData.skeletonDataArray.length, 1);
    const built = buildNextThreeNode(usd, { skipTextures: true, releaseBuildData: false });
    const characterGroup = new THREE.Group();
    const result = applyUSDSceneSkinningPipeline({
      threeNode: built.node,
      characterGroup,
      skeletonDataArray: skeletonData.skeletonDataArray,
      allSkinnedMeshUSDData: skinData.allSkinnedMeshUSDData,
      skinnedMeshDataByName: skinData.skinnedMeshDataByName,
      usdScene: usd,
      logger: { log() {}, warn() {}, error() {} }
    });
    assert.equal(result.processedSkinnedCount, 1);
    assert.ok(result.firstSkinnedMesh?.isSkinnedMesh,
      'next backend must bind the mesh instead of rendering undeformed bind geometry');
    const mesh = result.firstSkinnedMesh;
    characterGroup.updateMatrixWorld(true);
    mesh.skeleton.update();
    const deformedBox = new THREE.Box3();
    const point = new THREE.Vector3();
    const positions = mesh.geometry.attributes.position;
    for (let i = 0; i < positions.count; ++i) {
      mesh.getVertexPosition(i, point);
      point.applyMatrix4(mesh.matrixWorld);
      deformedBox.expandByPoint(point);
    }
    const deformedSize = deformedBox.getSize(new THREE.Vector3());
    assert.ok(Math.max(...deformedSize.toArray()) < 0.2,
      `scaled/rotated SkelRoot must not inflate in GPU skinning space: ${deformedSize.toArray()}`);
  } finally {
    usd.delete();
  }
});

await test('combined WASM next RenderStream preserves xformOp:orient', () => {
  const stream = renderStreamFor(encoder.encode(ORIENTED_TRIANGLE_USDA));
  try {
    const mesh = stream.getMesh(0);
    assert.ok(mesh && !mesh.error, `oriented mesh should materialize: ${mesh?.error || ''}`);
    assert.ok(Array.isArray(mesh.worldMatrix) && mesh.worldMatrix.length === 16,
      'oriented mesh should expose a world matrix');
    assert.ok(Math.abs(mesh.worldMatrix[6] + 1) < 1e-4,
      'xformOp:orient should rotate the row-major world matrix');
    assert.ok(Math.abs(mesh.worldMatrix[9] - 1) < 1e-4,
      'xformOp:orient should preserve the expected inverse-axis term');
  } finally {
    stream.end();
    stream.delete();
  }
});

await test('combined WASM next adapter extracts a USDA root from USDZ', async () => {
  const dependencyPackage = await makeUsdz(TEXTURED_TWO_MATERIAL_USDA);
  const dependency = parseUSDZEntries(dependencyPackage.usdz)
    .find((entry) => /\.usdc$/i.test(entry.name));
  assert.ok(dependency, 'fixture should contain a USDC dependency layer');
  const packaged = buildUSDZWithNewRoot(
    'root.usda', encoder.encode(SIMPLE_TRIANGLE_USDA), [{
      ...dependency,
      name: 'dependency.usdc',
    }]);
  const scene = await NextRenderSceneAdapter.create(
    native, packaged, 'fixture.usdz', { meshOnly: true });
  try {
    assert.equal(scene.numMeshes(), 1, 'USDA-root USDZ should expose one mesh');
    const mesh = scene.getMeshCopy(0);
    assert.ok(mesh && !mesh.error, `USDA-root mesh should materialize: ${mesh?.error || ''}`);
  } finally {
    scene.delete();
  }
});

await test('next flatten does not allocate when required remap API is missing', () => {
  let allocations = 0;
  const fakeNative = { HEAPU8: new Uint8Array(16) };
  const fakeUsd = {
    nextFlattenBuffer() {
      throw new Error('must not flatten without remap support');
    },
    allocateZeroCopyBuffer() {
      allocations++;
      return { success: true, bufferPtr: 0, uuid: 'fixture-buffer' };
    },
  };
  const result = nextFlattenViaStreaming(
    fakeNative,
    fakeUsd,
    new Uint8Array([1, 2, 3]),
    () => {},
    true,
    0,
    { 'Texture.png': 'Texture.jpg' },
  );
  assert.equal(result, null);
  assert.equal(allocations, 0, 'zero-copy input must not be allocated on capability decline');
});

await test('ZipStreamWriter binds object sinks and writes a valid ZIP stream', () => {
  const sink = {
    chunks: [],
    write(chunk) {
      this.chunks.push(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk));
    },
  };
  const writer = new ZipStreamWriter(sink);
  writer.addEntry('root.usdc', new Uint8Array([1, 2, 3, 4]));
  writer.finalize();
  const out = new Uint8Array(sink.chunks.reduce((sum, c) => sum + c.length, 0));
  let offset = 0;
  for (const chunk of sink.chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  assert.deepEqual(Array.from(out.slice(0, 4)), [0x50, 0x4b, 0x03, 0x04]);
});

await test('next usdzconvert remaps texture paths when texture extension changes', async () => {
  const base = await makeUsdz(TEXTURED_TWO_MATERIAL_USDA, {
    'Texture.png': makePngTexture(),
  });
  assertReloadable(base.usdz, 'base-textured');

  const log = [];
  const converted = await convertFolderToUSDZ(native, new Map([['input.usdz', base.usdz]]), {
    rootPath: 'input.usdz',
    pipeline: 'next',
    textureFormat: 'jpeg',
    reencode: false,
    log: (msg) => log.push(String(msg)),
  });

  assert.equal(converted.stats.pipeline, 'next', `next pipeline should engage:\n${log.join('\n')}`);
  assert.ok(converted.stats.assetPathsRemapped > 0, 'texture asset paths should be remapped');
  const { entries } = unpackUSDZ(converted.usdz);
  assert.ok(entries.has('Texture.jpg'), 'renamed texture should be packaged');
  assert.ok(!entries.has('Texture.png'), 'old texture entry should be removed');
  assertReloadable(converted.usdz, 'next-remapped');
});

await test('next RenderStream material dedup collapses identical anonymous materials', async () => {
  const base = await makeUsdz(TEXTURED_TWO_MATERIAL_USDA, {
    'Texture.png': makePngTexture(),
  });
  const root = rootUsdcFromUsdz(base.usdz);

  const withoutDedup = renderStreamFor(root, { materialDedup: false });
  try {
    materializeAllMeshes(withoutDedup);
    assert.equal(withoutDedup.getStats().optimizedMaterials, 2,
      'two distinct material prims should stay separate without dedup');
  } finally {
    withoutDedup.end();
    withoutDedup.delete();
  }

  const withDedup = renderStreamFor(root, { materialDedup: true });
  try {
    materializeAllMeshes(withDedup);
    assert.equal(withDedup.getStats().optimizedMaterials, 1,
      'identical material records should collapse with dedup enabled');
    assert.equal(withDedup.getStats().optimizedTextures, 1,
      'shared texture identity should be counted once');
  } finally {
    withDedup.end();
    withDedup.delete();
  }
});

await test('next RenderStream rejects malformed mesh topology before JS/WebGL exposure', async () => {
  const base = await makeUsdz(INVALID_FACE_INDEX_USDA);
  const root = rootUsdcFromUsdz(base.usdz);
  const stream = renderStreamFor(root);
  try {
    const mesh = stream.getMesh(0);
    assert.ok(mesh && mesh.error, 'invalid topology should return a mesh error');
    assert.match(String(mesh.error), /out of point range/);
  } finally {
    stream.end();
    stream.delete();
  }
});

await test('next lazy texture reset defers archive release until active loads settle', async () => {
  const manager = new NextTextureLoadingManager();
  let releaseCount = 0;
  let resolveLoad;
  const loadDone = new Promise((resolve) => { resolveLoad = resolve; });
  const adapter = {
    getArchiveTextureBytes() {
      return new Uint8Array([1, 2, 3]);
    },
    releaseArchiveTextureBytes() {
      releaseCount++;
    },
  };
  const material = {};
  manager.queueTexture(material, 'map', adapter, 'Texture.png', 'color');
  manager.loadTexture = async () => {
    await loadDone;
    return { isTexture: true };
  };

  const loading = manager.startLoading({ concurrency: 1 });
  await new Promise((resolve) => setTimeout(resolve, 0));
  manager.abort();
  manager.reset();
  assert.equal(releaseCount, 0, 'archive bytes must remain while load is in flight');
  resolveLoad();
  await loading;
  assert.equal(releaseCount, 1, 'archive bytes should release after in-flight load settles');
  assert.equal(material.map, undefined, 'aborted texture load should not mutate stale material');
});

await test('next archive textures reuse worker-prefetched browser images', async () => {
  const image = { width: 4, height: 4 };
  let requestedPath = '';
  const texture = await loadNextArchiveTexture({
    getArchiveTextureBytes() {
      return new Uint8Array([1]);
    },
    getPrefetchedArchiveImage(path) {
      requestedPath = path;
      return Promise.resolve({ image, flipY: true });
    },
  }, 'textures/card.png', 'color', { wrapS: 'mirror', wrapT: 'clamp' });
  assert.equal(requestedPath, 'textures/card.png');
  assert.equal(texture.image, image);
  assert.equal(texture.colorSpace, THREE.SRGBColorSpace);
  assert.equal(texture.wrapS, THREE.MirroredRepeatWrapping);
  assert.equal(texture.wrapT, THREE.ClampToEdgeWrapping);
  assert.equal(texture.flipY, true);
});

await test('next worker archive lookup retains anonymous UDIM tiles', async () => {
  const first = new Uint8Array([1, 2, 3]);
  const second = new Uint8Array([4, 5, 6]);
  const entries = new Map([
    ['textures/surface.1001.png', first],
    ['textures/surface.1002.png', second],
  ]);
  assert.equal(findNextArchiveEntry(entries, './textures/surface.1001.png')?.bytes,
    first);
  const layout = nextArchiveUDIMLayout(entries, './textures/surface.<UDIM>.png');
  assert.equal(layout.columns, 2);
  assert.equal(layout.rows, 1);
  assert.deepEqual(layout.tiles.map(({ id, u, v, path }) => ({ id, u, v, path })), [
    { id: 1001, u: 0, v: 0, path: 'textures/surface.1001.png' },
    { id: 1002, u: 1, v: 0, path: 'textures/surface.1002.png' },
  ]);
  assert.equal(nextArchiveUDIMLayout(entries, './textures/missing.<UDIM>.png'), null);
});

await test('next missing UDIM diagnostics are deduplicated and preserve fallbacks', async () => {
  const manager = new NextTextureLoadingManager();
  const material = { color: new THREE.Color(0.2, 0.4, 0.8) };
  const adapter = {
    getArchiveUDIMTiles() { return null; },
    getArchiveTextureBytes() { return null; },
  };
  manager.queueTexture(material, 'map', adapter,
    'textures/missing.<UDIM>.png', 'color');
  manager.queueTexture(material, 'normalMap', adapter,
    'textures/missing.<UDIM>.png', 'data');
  const previousWarn = console.warn;
  const warnings = [];
  console.warn = (...args) => warnings.push(args);
  try {
    const status = await manager.startLoading({ concurrency: 2, yieldInterval: 0 });
    assert.equal(status.failed, 2);
  } finally {
    console.warn = previousWarn;
  }
  assert.equal(warnings.length, 1);
  assert.equal(material.map, undefined);
  assert.equal(material.normalMap, undefined);
  assert.deepEqual(material.color.toArray(), [0.2, 0.4, 0.8]);
});

await test('next materials preserve opacity maps without double-applying RGBA alpha', () => {
  const queued = [];
  const manager = {
    queueTexture(_material, property, _adapter, assetPath) {
      queued.push([property, assetPath]);
    },
  };
  const shared = createNextMaterial({
    material: { opacity: 1 },
    texturePaths: { baseColor: 'rgba.png', opacity: 'rgba.png' },
  }, {}, manager, false);
  assert.equal(shared.transparent, true, 'an opacity connection must enable blending');
  assert.equal(shared.alphaTest, 1 / 255,
    'opacity-textured cards should discard effectively transparent fragments');
  assert.deepEqual(queued, [['map', 'rgba.png']],
    'shared RGBA alpha should come from map and must not be multiplied twice');

  queued.length = 0;
  createNextMaterial({
    material: { opacity: 1 },
    texturePaths: { baseColor: 'color.png', opacity: 'mask.png' },
  }, {}, manager, false);
  assert.deepEqual(queued, [['map', 'color.png'], ['alphaMap', 'mask.png']],
    'a distinct opacity texture should be queued as alphaMap');
});

await test('next textures preserve authored USD wrap modes', () => {
  assert.equal(nextTextureWrapMode('black'), THREE.ClampToEdgeWrapping);
  assert.equal(nextTextureWrapMode('clamp'), THREE.ClampToEdgeWrapping);
  assert.equal(nextTextureWrapMode('useMetadata'), THREE.ClampToEdgeWrapping);
  assert.equal(nextTextureWrapMode('repeat'), THREE.RepeatWrapping);
  assert.equal(nextTextureWrapMode('mirror'), THREE.MirroredRepeatWrapping);

  const queued = [];
  createNextMaterial({
    material: {
      textureMetadata: { baseColor: { wrapS: 'black', wrapT: 'mirror' } },
    },
    texturePaths: { baseColor: 'card.png' },
  }, {}, {
    queueTexture(_material, property, _adapter, assetPath, _role, sampler) {
      queued.push([property, assetPath, sampler.wrapS, sampler.wrapT]);
    },
  }, false);
  assert.deepEqual(queued, [['map', 'card.png', 'black', 'mirror']]);

  const manager = new NextTextureLoadingManager();
  manager.queueTexture({}, 'map', {}, 'shared.png', 'color',
    { wrapS: 'black', wrapT: 'black' });
  manager.queueTexture({}, 'map', {}, 'shared.png', 'color',
    { wrapS: 'repeat', wrapT: 'black' });
  assert.equal(manager.total, 2,
    'one image with different authored samplers must not share a texture object');

  const scopedTransforms = new NextTextureLoadingManager();
  const firstTransform = {
    canonical: 'studio', bypass: false, gamma: 1, linearBias: 0,
    matrix: [1, 0, 0, 0, 1, 0, 0, 0, 1]
  };
  const secondTransform = {
    canonical: 'studio', bypass: false, gamma: 2.2, linearBias: 0,
    matrix: [0.9, 0.1, 0, 0, 1, 0, 0, 0, 1]
  };
  scopedTransforms.queueTexture({}, 'map', {}, 'shared.png', 'data', null,
    null, firstTransform);
  scopedTransforms.queueTexture({}, 'map', {}, 'shared.png', 'data', null,
    null, secondTransform);
  assert.equal(scopedTransforms.total, 2,
    'scoped definitions with the same name but different transforms must not deduplicate');
});

await test('next material subsets compact alternating faces into bounded draw groups', () => {
  const indices = new Uint32Array([
    0, 1, 2,
    3, 4, 5,
    6, 7, 8,
    9, 10, 11,
  ]);
  const compacted = compactMaterialGroups(indices, [
    { start: 0, count: 3, materialIndex: 0 },
    { start: 6, count: 3, materialIndex: 0 },
  ], 2, 1);
  assert.deepEqual(compacted.groups, [
    { start: 0, count: 6, materialIndex: 0 },
    { start: 6, count: 6, materialIndex: 1 },
  ]);
  assert.deepEqual(Array.from(compacted.indices), [
    0, 1, 2, 6, 7, 8,
    3, 4, 5, 9, 10, 11,
  ]);
  const soup = compactMaterialGroups(null, [
    { start: 3, count: 3, materialIndex: 0 },
  ], 2, 1, 6);
  assert.deepEqual(soup.groups, [
    { start: 0, count: 3, materialIndex: 0 },
    { start: 3, count: 3, materialIndex: 1 },
  ]);
  assert.deepEqual(Array.from(soup.indices), [3, 4, 5, 0, 1, 2],
    'non-indexed triangle soup should compact through an identity index buffer');
});

await test('simple anonymous fixture remains reloadable through legacy usdzconvert', async () => {
  const base = await makeUsdz(SIMPLE_TRIANGLE_USDA);
  assertReloadable(base.usdz, 'simple');
});

if (failed > 0) {
  console.error(`regression tests failed: ${failed}/${passed + failed}`);
  process.exit(1);
}
console.log(`regression tests passed: ${passed}/${passed + failed} (${wasm64 ? 'wasm64' : 'wasm32'})`);
