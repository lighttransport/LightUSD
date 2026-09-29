// Regression for LightUSDLoaderNative.applyVariantSelection embind overloads.
//   node web/js/tests/apply-variant-selection-overload.test.mjs

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

import { loadWasm, parseUSDZEntries } from '../src/usdzconvert.js';
import { TEXTURED_TWO_MATERIAL_USDA } from './fixtures/regression-fixtures.mjs';

async function testAsync(name, fn) {
  try {
    await fn();
    console.log(`ok - ${name}`);
  } catch (err) {
    console.error(`not ok - ${name}`);
    console.error(err);
    process.exitCode = 1;
  }
}

const fixtureUrl = new URL('../../../tests/usda/variantSet-apply-selection-overload.usda', import.meta.url);
const fixtureBytes = new Uint8Array(fs.readFileSync(fixtureUrl));

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const defaultGlue = wasm64 ? '../src/lightusd/lightusd_combined_64.js'
                           : '../src/lightusd/lightusd_combined.js';
const glueUrl = process.env.LIGHTUSD_COMBINED_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_COMBINED_MODULE)).href
  : new URL(defaultGlue, import.meta.url).href;
const native = await loadWasm(() => import(glueUrl));
const nextGlue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                        : '../src/lightusd/lightusd_next.js';
const nextRuntime = await loadWasm(() => import(nextGlue));

await testAsync('combined LightUSDLoaderNative methods match the checked API inventory', () => {
  const expected = JSON.parse(fs.readFileSync(
    new URL('./lightusd-loader-api-inventory.json', import.meta.url), 'utf8'));
  const actual = Object.getOwnPropertyNames(native.LightUSDLoaderNative.prototype)
    .filter((name) => name !== 'constructor').sort();
  assert.deepEqual(actual, expected,
    'intentional legacy loader API changes must update the checked inventory');
  const classified = JSON.parse(fs.readFileSync(
    new URL('./lightusd-loader-api-classification.json', import.meta.url), 'utf8'));
  const classifiedNames = classified.map(({name}) => name).sort();
  assert.deepEqual(classifiedNames, expected,
    'each combined loader method must have exactly one semantic family');
  assert.equal(new Set(classifiedNames).size, classified.length,
    'combined loader methods must not be classified more than once');
  const families = [...new Set(classified.map(({family}) => family))].sort();
  assert.deepEqual(families, [
    'asset_resolution_and_cache', 'composition_and_variants',
    'layer_export_and_validation', 'loader_configuration',
    'loading_and_diagnostics', 'mcp', 'next_flatten',
    'render_scene_queries', 'schema_and_image_utilities', 'streaming_buffers',
    'test_only',
  ]);
  assert.ok(classified.every(({boundary}) =>
    boundary === 'embind' || boundary === 'typed-c'));
  assert.deepEqual(classified.filter(({boundary}) => boundary === 'typed-c')
    .map(({name}) => name).sort(), [
      'getAnimation',
      'getAllAnimations',
      'getAnimationInfo',
      'getAllAnimationInfos',
      'getMeshPrimvarsJSON', 'computeMeshTangents',
      'generateBoneTexture',
      'getMeshPtr',
      'getMesh', 'getMeshCopy',
      'getMaterial', 'getMaterialWithFormat',
      'extractPhysicsSceneJSON', 'createSampleScene', 'clearURDFMeshBuffers',
      'setVisualMesh', 'setCollisionMesh', 'createURDFPhysicsScene',
      'encodeImageNative',
      'cancelParsing',
      'error',
      'getProgress',
      'isParsingInProgress',
      'loadAsLayerFromBinary',
      'loadAsLayerFromBinaryWithProgress',
      'loadAsLayerFromCachedAsset',
      'loadFromBinary',
      'loadFromBinaryAsync',
      'loadFromBinaryWithProgress',
      'loadFromCachedAsset',
      'loadLayerFromJSON',
      'loadTest',
      'ok',
      'releaseSourceLayer',
      'reset',
      'resetProgress',
      'testValueMemoryUsage',
      'validateFromBinary',
      'validateLoadedLayer',
      'warn',
      'wasCancelled',
      'mcpCreateContext',
      'mcpResourcesList',
      'mcpResourcesRead',
      'mcpSelectContext',
      'mcpToolsCall',
      'mcpToolsList',
      'layerToString',
      'layerToJSON',
      'layerToJSONWithOptions',
      'exportAsUSDA',
      'flattenLayer',
      'layerToRenderScene',
      'exportAsUSDC',
      'exportLayerAsUSDCWithOptions',
      'exportStageAsUSDCToBufferWithOptions',
      'exportLayerAsUSDCToBufferWithOptions',
      'exportAsUSDZ',
      'exportAsUSDZWithRemap',
      'exportAsUSDZWithOptions',
      'exportLayerAsUSDZWithOptions',
      'remapLayerAssetPaths',
      'numMeshes',
      'numInstances',
      'numMaterials',
      'numTextures',
      'numImages',
      'numLights',
      'numCameras',
      'numUDIMTextures',
      'numRootNodes',
      'numAnimations',
      'numSkeletons',
      'getDefaultRootNodeId',
      'getURI',
      'getUpAxis',
      'getNativeMaterialDedup',
      'getNativeMeshMerge',
      'getNativeMeshMergeBakeTransform',
      'getNativeFlattenRenderTree',
      'getCamera',
      'getTexture',
      'getSceneMetadata',
      'getSkeleton',
      'getAllSkeletons',
      'getSkeletonJointsFlat',
      'getLight',
      'getAllLights',
      'getLightWithFormat',
      'getImage',
      'getImagePtr',
      'getImageCopy',
      'getUDIMTexture',
      'extractUnresolvedTexturePaths',
      'getRootNode',
      'getDefaultRootNode',
      'getInstance',
      'getInstancesForMesh',
      'getMhProfileJSON',
      'getShadingGraphJSON',
      'applyVariantSelection', 'composeInherits', 'composePayload',
      'composeReferences', 'composeSublayers', 'composeVariants',
      'debugLogMemory', 'extractPayloadAssetPaths',
      'extractReferencesAssetPaths', 'extractSublayerAssetPaths',
      'extractVariants', 'getCombineUDIMTiles', 'getDeferTangentComputation',
      'getEnableBoneReduction', 'getEnableValueClips', 'getMaxMemoryLimitMB',
      'getMemoryStats', 'getRoundBoneCount', 'getSphereSubdivisions',
      'getTargetBoneCount', 'getValueClipEndTime', 'getValueClipSampleRate',
      'getValueClipStartTime', 'getValueClipUseTimeRange', 'hasInherits',
      'hasPayload', 'hasReferences', 'hasSublayers', 'hasVariants',
      'lodVariantCount', 'nextFlattenAsyncBegin', 'nextFlattenAsyncBeginRemap',
      'nextFlattenAsyncBeginRemapVariants', 'nextFlattenAsyncEnd',
      'nextFlattenAsyncProvideLayer', 'nextFlattenAsyncStep',
      'nextFlattenBuffer', 'nextFlattenBufferRemap',
      'nextFlattenBufferRemapVariants', 'nextFlattenBufferToSink',
      'nextFlattenBufferToSinkRemap', 'nextFlattenBufferToSinkRemapVariants',
      'nextFlattenMultiBufferToSink', 'nextFlattenMultiBufferToSinkFetch',
      'nextFlattenMultiBufferToSinkFetchRemap',
      'nextFlattenMultiBufferToSinkFetchRemapVariants', 'nextFlattenUSDC',
      'setCombineUDIMTiles', 'setDeferTangentComputation',
      'setEnableBoneReduction', 'setEnableComposition', 'setEnableValueClips',
      'setLoadTextureInNative', 'setMaxMemoryLimitMB',
      'setNativeFlattenRenderTree', 'setNativeMaterialDedup',
      'setNativeMeshMerge', 'setNativeMeshMergeBakeTransform',
      'setRoundBoneCount', 'setSphereSubdivisions', 'setTargetBoneCount',
      'setUSDCExportLimitMB', 'setValueClipSampleRate', 'setValueClipTimeRange',
      'setValueClipUseTimeRange',
      'allocateZeroCopyBuffer',
      'appendAssetChunk',
      'cancelZeroCopyBuffer',
      'finalizeStreamingAsset',
      'finalizeZeroCopyBuffer',
      'getActiveZeroCopyBuffers',
      'getMMapZeroCopy',
      'getStreamingProgress',
      'getZeroCopyBufferPtr',
      'getZeroCopyBufferPtrAtOffset',
      'getZeroCopyProgress',
      'isStreamingAssetComplete',
      'markZeroCopyBytesWritten',
      'setMMapZeroCopy',
      'startStreamingAsset',
      'addAssetSearchPath',
      'assetExists',
      'clearAssetSearchPaths',
      'clearAssets',
      'deleteAsset',
      'deleteAssetByName',
      'deleteAssetByUUID',
      'findAssetByUUID',
      'getAllAssetUUIDs',
      'getAllowParentRelativeAssetPaths',
      'getAsset',
      'getAssetByUUID',
      'getAssetCacheDataAsMemoryView',
      'getAssetCacheMaxSizeBytes',
      'getAssetCacheSizeBytes',
      'getAssetCount',
      'getAssetHash',
      'getAssetSearchPaths',
      'getAssetUUID',
      'getBaseWorkingPath',
      'getStreamingAssetUUID',
      'hasAsset',
      'setAllowParentRelativeAssetPaths',
      'setAsset',
      'setAssetCacheMaxSizeBytes',
      'setAssetFromRawPointer',
      'setBaseWorkingPath',
      'verifyAssetHash'
    ].sort());
  assert.equal(native.LightUSDLoaderNative.prototype.testLayer, undefined,
    'legacy debug-only testLayer must not remain in the combined browser API');
});

function withLoadedLayer(fn) {
  const usd = new native.LightUSDLoaderNative();
  try {
    assert.ok(
      usd.loadAsLayerFromBinary(fixtureBytes, 'variantSet-apply-selection-overload.usda'),
      `loadAsLayerFromBinary failed: ${usd.error()}`
    );
    return fn(usd);
  } finally {
    usd.delete();
  }
}

function assertSelected(usda, selectedName, selectedLevel, rejectedNames) {
  assert.ok(usda.includes(selectedName), `expected selected variant mesh ${selectedName}`);
  assert.ok(usda.includes(`lodLevel = ${selectedLevel}`), `expected lodLevel ${selectedLevel}`);
  for (const name of rejectedNames) {
    assert.ok(!usda.includes(name), `did not expect unselected variant mesh ${name}`);
  }
}

await testAsync('applyVariantSelection(primPath, variantSet, variantName) selects targeted LOD', () => {
  withLoadedLayer((usd) => {
    assert.equal(usd.lodVariantCount(), 3);
    assert.ok(
      usd.applyVariantSelection('/World', 'LOD', 'LOD1'),
      `3-arg applyVariantSelection failed: ${usd.error()}`
    );
    assertSelected(usd.exportAsUSDA(), 'LOD1Mesh', 1, ['LOD0Mesh', 'LOD2Mesh']);
  });
});

await testAsync('applyVariantSelection(variantName) selects authored LOD set globally', () => {
  withLoadedLayer((usd) => {
    assert.equal(usd.lodVariantCount(), 3);
    assert.ok(
      usd.applyVariantSelection('LOD2'),
      `1-arg applyVariantSelection failed: ${usd.error()}`
    );
    assertSelected(usd.exportAsUSDA(), 'LOD2Mesh', 2, ['LOD0Mesh', 'LOD1Mesh']);
  });
});

await testAsync('combined nextFlattenUSDC uses the typed copy adapter', () => {
  const inputUrl = new URL('../../../tests/usdc/dedup-global-001.usdc', import.meta.url);
  const fixture = new Uint8Array(fs.readFileSync(inputUrl));
  const padded = new Uint8Array(fixture.length + 7);
  padded.set(fixture, 3);
  assert.throws(() => native.LightUSDLoaderNative.prototype.nextFlattenUSDC.call(
    {}, fixture, true), TypeError);
  const deleted = new native.LightUSDLoaderNative();
  deleted.delete();
  assert.throws(() => native.LightUSDLoaderNative.prototype.nextFlattenUSDC.call(
    deleted, fixture, true), TypeError);

  const loader = new native.LightUSDLoaderNative();
  try {
    const zeroCopy = loader.allocateZeroCopyBuffer('flatten-buffer.usdc', fixture.length, 0);
    assert.ok(zeroCopy.success, zeroCopy.error);
    native.HEAPU8.set(fixture, Number(zeroCopy.bufferPtr));
    const buffered = loader.nextFlattenBuffer(zeroCopy.uuid, true);
    assert.equal(buffered.success, true, buffered.error);
    assert.equal(buffered.inputBytes, fixture.length);
    assert.ok(buffered.data instanceof Uint8Array);
    assert.ok(buffered.data.length > 0);
    const remapBuffer = loader.allocateZeroCopyBuffer(
      'flatten-buffer-remap.usdc', fixture.length, 0);
    assert.ok(remapBuffer.success, remapBuffer.error);
    native.HEAPU8.set(fixture, Number(remapBuffer.bufferPtr));
    const remapped = loader.nextFlattenBufferRemap(
      remapBuffer.uuid, true, {'dep.usdc': 'resolved.usdc'});
    assert.equal(remapped.success, true, remapped.error);
    assert.ok(remapped.data.length > 0);

    const remapVariantBuffer = loader.allocateZeroCopyBuffer(
      'flatten-buffer-remap-variants.usdc', fixture.length, 0);
    assert.ok(remapVariantBuffer.success, remapVariantBuffer.error);
    native.HEAPU8.set(fixture, Number(remapVariantBuffer.bufferPtr));
    const remappedVariants = loader.nextFlattenBufferRemapVariants(
      remapVariantBuffer.uuid, true, {'dep.usdc': 'resolved.usdc'}, {
        '/World{LOD}': 'LOD1', '': 'ignored', '/World{Unused}': null,
        '/World{Empty}': ''
      });
    assert.equal(remappedVariants.success, true, remappedVariants.error);
    assert.ok(remappedVariants.data.length > 0);
    const bufferAgain = loader.nextFlattenBuffer(zeroCopy.uuid, true);
    assert.equal(bufferAgain.success, false);
    assert.match(bufferAgain.error, /Unknown or empty zero-copy buffer/);

    const sessionBuffer = loader.allocateZeroCopyBuffer(
      'flatten-session.usdc', fixture.length, 0);
    assert.ok(sessionBuffer.success, sessionBuffer.error);
    native.HEAPU8.set(fixture, Number(sessionBuffer.bufferPtr));
    const session = loader.nextFlattenAsyncBegin(
      sessionBuffer.uuid, 'root.usdc', true);
    assert.ok(session.success, session.error);
    assert.equal(session.status, 'ready');
    const missingSessionBuffer = loader.nextFlattenAsyncBegin(
      'missing-buffer', 'root.usdc', true);
    assert.equal(missingSessionBuffer.success, false);
    assert.match(missingSessionBuffer.error, /Unknown or empty zero-copy buffer/);
    assert.deepEqual(loader.nextFlattenAsyncProvideLayer(
      session.session, 'dep.usdc', fixture), {success: true});
    const invalidLayer = loader.nextFlattenAsyncProvideLayer(
      session.session, 'empty.usdc', new Uint8Array());
    assert.equal(invalidLayer.success, false);
    assert.match(invalidLayer.error, /Invalid or empty layer data/);
    const unknownSession = loader.nextFlattenAsyncProvideLayer(
      'missing-session', 'dep.usdc', fixture);
    assert.equal(unknownSession.success, false);
    assert.match(unknownSession.error, /Unknown next flatten session/);
    assert.deepEqual(loader.nextFlattenAsyncEnd(session.session), {success: true});
    assert.deepEqual(loader.nextFlattenAsyncEnd(session.session), {success: false});

    const asyncRemapBuffer = loader.allocateZeroCopyBuffer(
      'flatten-remap.usdc', fixture.length, 0);
    assert.ok(asyncRemapBuffer.success, asyncRemapBuffer.error);
    native.HEAPU8.set(fixture, Number(asyncRemapBuffer.bufferPtr));
    const remappedSession = loader.nextFlattenAsyncBeginRemap(
      asyncRemapBuffer.uuid, 'root.usdc', true, {'dep.usdc': 'resolved.usdc'});
    assert.ok(remappedSession.success, remappedSession.error);
    assert.deepEqual(loader.nextFlattenAsyncEnd(remappedSession.session), {success: true});

    const variantBuffer = loader.allocateZeroCopyBuffer(
      'flatten-variant.usdc', fixture.length, 0);
    assert.ok(variantBuffer.success, variantBuffer.error);
    native.HEAPU8.set(fixture, Number(variantBuffer.bufferPtr));
    const variantSession = loader.nextFlattenAsyncBeginRemapVariants(
      variantBuffer.uuid, 'root.usdc', true, {'dep.usdc': 'resolved.usdc'}, {
        '/World{LOD}': 'LOD1', '': 'ignored', '/World{Unused}': null,
        '/World{Empty}': ''
      });
    assert.ok(variantSession.success, variantSession.error);
    assert.deepEqual(loader.nextFlattenAsyncEnd(variantSession.session), {success: true});

    const result = loader.nextFlattenUSDC(padded.subarray(3, 3 + fixture.length), true);
    assert.equal(result.success, true, result.error);
    assert.ok(result.data instanceof Uint8Array);
    assert.ok(result.data.length > 0);
    assert.equal(result.inputBytes, fixture.length);
    assert.ok(result.outputBytes > 0);
    assert.ok(result.primCount > 0);

    const reloaded = new native.LightUSDLoaderNative();
    try {
      assert.ok(reloaded.loadAsLayerFromBinary(result.data, 'flattened.usdc'),
        `flatten output did not reload: ${reloaded.error()}`);
    } finally {
      reloaded.delete();
    }

    const failed = loader.nextFlattenUSDC(new Uint8Array([1, 2, 3]), true);
    assert.equal(failed.success, false);
    assert.ok(failed.error.length > 0);
  } finally {
    loader.delete();
  }
});

const FLATTEN_ROOT_USDA = `#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World" (
    prepend references = @dep.usda@</Dep>
)
{
}
`;
const FLATTEN_DEP_USDA = `#usda 1.0

def Xform "Dep"
{
    def Sphere "Ball"
    {
        double radius = 2
    }
}
`;

function concatChunks(chunks) {
  const size = chunks.reduce((total, chunk) => total + chunk.length, 0);
  const out = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  return out;
}

function stageZeroCopy(loader, name, bytes) {
  const info = loader.allocateZeroCopyBuffer(name, bytes.length, 0);
  assert.ok(info.success, info.error);
  native.HEAPU8.set(bytes, Number(info.bufferPtr));
  return info.uuid;
}

function assertFlattenStats(result) {
  for (const key of ['inputBytes', 'outputBytes', 'primCount',
    'arraysPassedThrough', 'arraysReencoded', 'assetPathsRemapped',
    'readMs', 'composeMs', 'writeMs']) {
    assert.equal(typeof result[key], 'number', `missing stat ${key}`);
  }
}

await testAsync('combined next flatten sink adapters preserve streaming results', () => {
  const inputUrl = new URL('../../../tests/usdc/dedup-global-001.usdc', import.meta.url);
  const fixture = new Uint8Array(fs.readFileSync(inputUrl));
  const loader = new native.LightUSDLoaderNative();
  try {
    const buffered = loader.nextFlattenBuffer(
      stageZeroCopy(loader, 'sink-reference.usdc', fixture), true);
    assert.equal(buffered.success, true, buffered.error);

    const chunks = [];
    const streamed = loader.nextFlattenBufferToSink(
      stageZeroCopy(loader, 'sink.usdc', fixture), true,
      (view) => { chunks.push(view.slice()); return true; });
    assert.equal(streamed.success, true, streamed.error);
    assert.equal(streamed.data, undefined);
    assertFlattenStats(streamed);
    assert.equal(streamed.inputBytes, fixture.length);
    assert.ok(chunks.length > 0);
    assert.deepEqual(concatChunks(chunks), buffered.data);

    const remapChunks = [];
    const remapped = loader.nextFlattenBufferToSinkRemap(
      stageZeroCopy(loader, 'sink-remap.usdc', fixture), true,
      (view) => { remapChunks.push(view.slice()); }, {'dep.usdc': 'resolved.usdc'});
    assert.equal(remapped.success, true, remapped.error);
    assert.deepEqual(concatChunks(remapChunks), buffered.data);

    const variantChunks = [];
    const variants = loader.nextFlattenBufferToSinkRemapVariants(
      stageZeroCopy(loader, 'sink-variants.usdc', fixture), true,
      (view) => { variantChunks.push(view.slice()); return 1; },
      {'dep.usdc': 'resolved.usdc'},
      {'/World{LOD}': 'LOD1', '': 'ignored', '/World{Unused}': null});
    assert.equal(variants.success, true, variants.error);
    assert.ok(variantChunks.length > 0);

    assert.deepEqual(loader.nextFlattenBufferToSink(
      stageZeroCopy(loader, 'sink-abort.usdc', fixture), true, () => false),
    {success: false, error: 'aborted by sink'});

    const thrown = new Error('sink failed');
    assert.throws(() => loader.nextFlattenBufferToSink(
      stageZeroCopy(loader, 'sink-throw.usdc', fixture), true,
      () => { throw thrown; }), (error) => error === thrown);

    const missing = loader.nextFlattenBufferToSink('missing-sink', true, () => true);
    assert.equal(missing.success, false);
    assert.match(missing.error, /Unknown or empty zero-copy buffer: missing-sink/);

    const proto = native.LightUSDLoaderNative.prototype;
    assert.throws(() => proto.nextFlattenBufferToSink.call({}, 'x', true, () => true),
      TypeError);
    assert.throws(() => proto.nextFlattenAsyncStep.call({}, 'x', null), TypeError);
    assert.throws(() => proto.nextFlattenMultiBufferToSink.call(
      {}, 'x', 'root.usda', true, null), TypeError);
    assert.throws(() => loader.nextFlattenBufferToSink('x', true, null), TypeError);
    assert.throws(() => loader.nextFlattenMultiBufferToSinkFetch(
      'x', 'root.usda', true, null, 'not-a-function', null), TypeError);
    assert.throws(() => loader.nextFlattenBufferToSinkRemap(
      'x', true, () => true, {'a.usdc': 1}), TypeError);
    assert.throws(() => loader.nextFlattenAsyncStep('x'), TypeError);

    // A sink that re-enters another flatten must not disturb the outer one.
    const outerChunks = [];
    let inner = null;
    const outer = loader.nextFlattenBufferToSink(
      stageZeroCopy(loader, 'sink-outer.usdc', fixture), true, (view) => {
        outerChunks.push(view.slice());
        if (!inner) {
          inner = loader.nextFlattenBuffer(
            stageZeroCopy(loader, 'sink-inner.usdc', fixture), true);
        }
      });
    assert.equal(outer.success, true, outer.error);
    assert.equal(inner.success, true, inner.error);
    assert.deepEqual(inner.data, buffered.data);
    assert.deepEqual(concatChunks(outerChunks), buffered.data);
  } finally {
    loader.delete();
  }
});

await testAsync('combined next multi-buffer and session step adapters resolve layers', () => {
  const encoder = new TextEncoder();
  const root = encoder.encode(FLATTEN_ROOT_USDA);
  const dep = encoder.encode(FLATTEN_DEP_USDA);
  const loader = new native.LightUSDLoaderNative();
  try {
    loader.setAsset('dep.usda', dep);
    const buffered = loader.nextFlattenMultiBufferToSink(
      stageZeroCopy(loader, 'multi-root.usda', root), 'root.usda', true, null);
    assert.equal(buffered.success, true, buffered.error);
    assertFlattenStats(buffered);
    assert.ok(buffered.data instanceof Uint8Array && buffered.data.length > 0);
    assert.ok(buffered.primCount >= 2);
    assert.ok(Array.isArray(buffered.assetPaths));
    assert.equal(buffered.assetPathCount, buffered.assetPaths.length);
    assert.deepEqual(buffered.compositionErrors, []);
    assert.equal(buffered.compositionErrorCount, 0);

    const existsCalls = [];
    const fetchCalls = [];
    const chunks = [];
    const fetched = loader.nextFlattenMultiBufferToSinkFetch(
      stageZeroCopy(loader, 'multi-fetch-root.usda', root), 'root.usda', true,
      (view) => { chunks.push(view.slice()); return true; },
      (key) => { existsCalls.push(key); return key === 'dep.usda'; },
      (key) => { fetchCalls.push(key); return dep; });
    assert.equal(fetched.success, true, fetched.error);
    assert.equal(fetched.data, undefined);
    assert.ok(existsCalls.includes('dep.usda'));
    assert.deepEqual(fetchCalls, ['dep.usda']);
    assert.deepEqual(concatChunks(chunks), buffered.data);
    assert.deepEqual(fetched.assetPaths, buffered.assetPaths);

    const remapped = loader.nextFlattenMultiBufferToSinkFetchRemap(
      stageZeroCopy(loader, 'multi-remap-root.usda', root), 'root.usda', true,
      null, (key) => key === 'dep.usda', () => dep, {'tex.png': 'tex.ktx2'});
    assert.equal(remapped.success, true, remapped.error);
    assert.deepEqual(remapped.data, buffered.data);

    const variants = loader.nextFlattenMultiBufferToSinkFetchRemapVariants(
      stageZeroCopy(loader, 'multi-variant-root.usda', root), 'root.usda', true,
      null, undefined, undefined, undefined, {'/World{LOD}': 'LOD1'});
    assert.equal(variants.success, true, variants.error);
    assert.ok(variants.compositionErrorCount >= 1,
      'unresolved dependency should be a non-fatal composition error');
    assert.equal(typeof variants.compositionErrors[0], 'string');

    const failedFetch = loader.nextFlattenMultiBufferToSinkFetch(
      stageZeroCopy(loader, 'multi-failed-fetch.usda', root), 'root.usda', true,
      null, (key) => key === 'dep.usda', () => undefined);
    assert.equal(failedFetch.success, true, failedFetch.error);
    assert.ok(failedFetch.compositionErrors.some(
      (message) => message.includes('asset fetch failed: dep.usda')));

    const thrown = new Error('fetch failed');
    assert.throws(() => loader.nextFlattenMultiBufferToSinkFetch(
      stageZeroCopy(loader, 'multi-throw.usda', root), 'root.usda', true,
      null, (key) => key === 'dep.usda', () => { throw thrown; }),
    (error) => error === thrown);

    assert.deepEqual(loader.nextFlattenMultiBufferToSink(
      stageZeroCopy(loader, 'multi-abort.usda', root), 'root.usda', true,
      () => false).error, 'aborted by sink');
    const missing = loader.nextFlattenMultiBufferToSink(
      'missing-multi', 'root.usda', true, null);
    assert.equal(missing.success, false);
    assert.match(missing.error, /Unknown or empty zero-copy buffer: missing-multi/);

    const begin = loader.nextFlattenAsyncBegin(
      stageZeroCopy(loader, 'step-root.usda', root), 'root.usda', true);
    assert.ok(begin.success, begin.error);
    const need = loader.nextFlattenAsyncStep(begin.session, null);
    assert.deepEqual(need, {success: true, status: 'need-layer', key: 'dep.usda'});
    assert.deepEqual(loader.nextFlattenAsyncProvideLayer(
      begin.session, need.key, dep), {success: true});
    assert.deepEqual(loader.nextFlattenAsyncStep(begin.session, () => false),
      {success: true, status: 'ready'});
    const done = loader.nextFlattenAsyncStep(begin.session, null);
    assert.equal(done.success, true, done.error);
    assert.equal(done.status, 'done');
    assertFlattenStats(done);
    assert.deepEqual(done.data, buffered.data);
    assert.equal(done.assetPathCount, done.assetPaths.length);
    assert.equal(done.compositionErrors, undefined);
    const stepChunks = [];
    const streamedDone = loader.nextFlattenAsyncStep(begin.session,
      (view) => { stepChunks.push(view.slice()); return true; });
    assert.equal(streamedDone.status, 'done');
    assert.equal(streamedDone.data, undefined);
    assert.deepEqual(concatChunks(stepChunks), buffered.data);
    const thrown2 = new Error('step sink failed');
    assert.throws(() => loader.nextFlattenAsyncStep(begin.session,
      () => { throw thrown2; }), (error) => error === thrown2);
    assert.deepEqual(loader.nextFlattenAsyncEnd(begin.session), {success: true});
    assert.deepEqual(loader.nextFlattenAsyncStep(begin.session, null), {
      success: false, error: `Unknown next flatten session: ${begin.session}`});

    const broken = loader.nextFlattenAsyncBegin(
      stageZeroCopy(loader, 'step-broken.usda', root), 'root.usda', true);
    assert.ok(broken.success, broken.error);
    assert.deepEqual(loader.nextFlattenAsyncProvideLayer(
      broken.session, 'dep.usda', encoder.encode('not a usd layer')), {success: true});
    const failed = loader.nextFlattenAsyncStep(broken.session, null);
    assert.equal(failed.success, false);
    assert.equal(failed.status, 'error');
    assert.ok(failed.error.length > 0);
    loader.nextFlattenAsyncEnd(broken.session);
  } finally {
    loader.delete();
  }
});

const COMPOSE_ROOT_USDA = `#usda 1.0
(
    subLayers = [@sub.usda@]
)

class "_Base"
{
    double baseAttr = 1
}

def Xform "World" (
    prepend inherits = </_Base>
    prepend references = @ref.usda@</Ref>
    prepend payload = @payload.usda@</Pay>
    variants = {
        string shape = "cube"
    }
    prepend variantSets = ["shape", "LOD"]
)
{
    variantSet "shape" = {
        "cube" { def Cube "Geo" {} }
        "sphere" { def Sphere "Geo" {} }
    }
    variantSet "LOD" = {
        "LOD0" { double lod = 0 }
        "LOD1" { double lod = 1 }
        "LOD2" { double lod = 2 }
    }
}
`;

await testAsync('combined layer composition and variant queries keep legacy results', () => {
  const encoder = new TextEncoder();
  const usd = new native.LightUSDLoaderNative();
  const queries = () => ({
    sublayers: usd.hasSublayers(), references: usd.hasReferences(),
    payload: usd.hasPayload(), inherits: usd.hasInherits(),
    variants: usd.hasVariants(), lod: usd.lodVariantCount()
  });
  try {
    usd.setAsset('sub.usda', encoder.encode('#usda 1.0\ndef Xform "FromSub" {}\n'));
    usd.setAsset('ref.usda', encoder.encode(
      '#usda 1.0\ndef Xform "Ref" { def Xform "RefChild" {} }\n'));
    usd.setAsset('payload.usda', encoder.encode(
      '#usda 1.0\ndef Xform "Pay" { def Xform "PayChild" {} }\n'));
    assert.ok(usd.loadAsLayerFromBinary(encoder.encode(COMPOSE_ROOT_USDA), 'root.usda'),
      usd.error());
    assert.deepEqual(queries(), {sublayers: true, references: true, payload: true,
      inherits: true, variants: true, lod: 3});
    assert.deepEqual(usd.extractSublayerAssetPaths(), ['sub.usda']);
    assert.deepEqual(usd.extractReferencesAssetPaths(), ['ref.usda']);
    assert.deepEqual(usd.extractPayloadAssetPaths(), ['payload.usda']);
    const nextPaths = new nextRuntime.RenderStream();
    try {
      nextPaths.setEnableComposition(false);
      const loaded = nextPaths.begin(encoder.encode(COMPOSE_ROOT_USDA), 'root.usda');
      assert.equal(loaded.success, true, loaded.error || nextPaths.error());
      assert.equal(nextPaths.getURI(), usd.getURI());
      assert.equal(nextPaths.getUpAxis(), usd.getUpAxis());
      assert.deepEqual(nextPaths.extractSublayerAssetPaths(), usd.extractSublayerAssetPaths());
      assert.deepEqual(nextPaths.extractReferencesAssetPaths(), usd.extractReferencesAssetPaths());
      assert.deepEqual(nextPaths.extractPayloadAssetPaths(), usd.extractPayloadAssetPaths());
      assert.deepEqual({
        sublayers: nextPaths.hasSublayers(), references: nextPaths.hasReferences(),
        payload: nextPaths.hasPayload(), inherits: nextPaths.hasInherits(),
        variants: nextPaths.hasVariants(), lod: nextPaths.lodVariantCount()
      }, queries());
      assert.deepEqual(nextPaths.extractVariants(), usd.extractVariants());
    } finally { nextPaths.delete(); }
    assert.deepEqual(usd.extractVariants(), [{
      primPath: '/World',
      variantSets: [
        {name: 'shape', selection: 'cube', options: ['cube', 'sphere']},
        {name: 'LOD', selection: '', options: ['LOD0', 'LOD1', 'LOD2']}
      ]
    }]);

    assert.equal(usd.composeSublayers(), true);
    assert.equal(usd.hasSublayers(), false);
    assert.equal(usd.composeReferences(), true);
    assert.equal(usd.hasReferences(), false);
    assert.equal(usd.composePayload(), true);
    assert.equal(usd.hasPayload(), false);
    assert.equal(usd.composeInherits(), true);
    assert.equal(usd.hasInherits(), false);
    assert.equal(usd.composeVariants(), true);
    assert.deepEqual(queries(), {sublayers: false, references: false, payload: false,
      inherits: false, variants: false, lod: 0});
    assert.deepEqual(usd.extractVariants(), []);
    const composed = usd.layerToString();
    for (const name of ['FromSub', 'RefChild', 'PayChild', 'baseAttr', 'Cube "Geo"']) {
      assert.ok(composed.includes(name), `composed layer should contain ${name}`);
    }
    assert.ok(!composed.includes('Sphere'));

    assert.equal(usd.applyVariantSelection('', 'LOD', 'LOD1'), false);
    assert.equal(usd.error(),
      'prim path, variant set name, and variant name are required.');
  } finally {
    usd.delete();
  }

  const empty = new native.LightUSDLoaderNative();
  try {
    assert.equal(empty.hasSublayers(), false);
    assert.equal(empty.lodVariantCount(), 0);
    assert.deepEqual(empty.extractVariants(), []);
    assert.deepEqual(empty.extractPayloadAssetPaths(), []);
    assert.equal(empty.applyVariantSelection('LOD1'), false);
    assert.equal(empty.error(), 'not loaded as layer');
    assert.equal(empty.applyVariantSelection('/World', 'LOD', 'LOD1'), false);
    assert.equal(empty.error(), 'No Layer is loaded. Use loadAsLayerFromBinary first.');
    // Legacy quirk kept for compatibility: inherits composition of an empty
    // layer succeeds.
    assert.equal(empty.composeInherits(), true);

    const proto = native.LightUSDLoaderNative.prototype;
    assert.throws(() => proto.hasSublayers.call({}), TypeError);
    assert.throws(() => proto.extractVariants.call({}), TypeError);
    assert.throws(() => proto.applyVariantSelection.call({}, 'LOD1'), TypeError);
    assert.throws(() => empty.hasVariants(1), TypeError);
    assert.throws(() => empty.applyVariantSelection('/World', 'LOD'), TypeError);
    assert.throws(() => empty.applyVariantSelection(1), TypeError);
  } finally {
    empty.delete();
  }
});

await testAsync('combined loader configuration keeps Embind coercion and results', () => {
  const usd = new native.LightUSDLoaderNative();
  const nextDefaults = new nextRuntime.RenderStream();
  try {
    assert.deepEqual([
      usd.getCombineUDIMTiles(), usd.getDeferTangentComputation(),
      usd.getEnableBoneReduction(), usd.getEnableValueClips(),
      usd.getRoundBoneCount(), usd.getSphereSubdivisions(),
      usd.getTargetBoneCount(), usd.getValueClipEndTime(),
      usd.getValueClipSampleRate(), usd.getValueClipStartTime(),
      usd.getValueClipUseTimeRange()
    ], [true, true, false, true, false, 4, 4, 0, 0, 0, false]);
    assert.equal(usd.getMaxMemoryLimitMB(), wasm64 ? 8192 : 2048);
    assert.deepEqual([
      nextDefaults.getCombineUDIMTiles(), nextDefaults.getDeferTangentComputation(),
      nextDefaults.getEnableBoneReduction(), nextDefaults.getEnableValueClips(),
      nextDefaults.getRoundBoneCount(), nextDefaults.getSphereSubdivisions(),
      nextDefaults.getTargetBoneCount(), nextDefaults.getValueClipEndTime(),
      nextDefaults.getValueClipSampleRate(), nextDefaults.getValueClipStartTime(),
      nextDefaults.getValueClipUseTimeRange()
    ], [false, true, false, true, false, 4, 4, 0, 0, 0, false]);
    assert.deepEqual([
      nextDefaults.getDeferTangentComputation(), nextDefaults.getEnableBoneReduction(),
      nextDefaults.getEnableValueClips(), nextDefaults.getRoundBoneCount(),
      nextDefaults.getSphereSubdivisions(), nextDefaults.getTargetBoneCount(),
      nextDefaults.getValueClipEndTime(), nextDefaults.getValueClipSampleRate(),
      nextDefaults.getValueClipStartTime(), nextDefaults.getValueClipUseTimeRange()
    ], [
      usd.getDeferTangentComputation(), usd.getEnableBoneReduction(),
      usd.getEnableValueClips(), usd.getRoundBoneCount(),
      usd.getSphereSubdivisions(), usd.getTargetBoneCount(),
      usd.getValueClipEndTime(), usd.getValueClipSampleRate(),
      usd.getValueClipStartTime(), usd.getValueClipUseTimeRange()
    ], 'shared next configuration defaults match legacy exactly');
    assert.equal(nextDefaults.getMaxMemoryLimitMB(), 1024);
    usd.setMaxMemoryLimitMB(-5);
    assert.equal(usd.getMaxMemoryLimitMB(), -5);
    assert.throws(() => nextDefaults.setMaxMemoryLimitMB(-5), /from 1 through 8192/);

    const acceptedSettings = [
      ['setCombineUDIMTiles', 'getCombineUDIMTiles', false],
      ['setDeferTangentComputation', 'getDeferTangentComputation', false],
      ['setEnableBoneReduction', 'getEnableBoneReduction', true],
      ['setEnableValueClips', 'getEnableValueClips', false],
      ['setRoundBoneCount', 'getRoundBoneCount', true],
      ['setSphereSubdivisions', 'getSphereSubdivisions', 3],
      ['setTargetBoneCount', 'getTargetBoneCount', 64],
      ['setValueClipSampleRate', 'getValueClipSampleRate', 0.25],
      ['setValueClipUseTimeRange', 'getValueClipUseTimeRange', true]
    ];
    for (const [setter, getter, value] of acceptedSettings) {
      assert.equal(usd[setter](value), nextDefaults[setter](value), setter);
      assert.deepEqual(usd[getter](), nextDefaults[getter](), getter);
    }
    assert.equal(usd.setValueClipTimeRange(0.5, 2),
      nextDefaults.setValueClipTimeRange(0.5, 2));
    assert.deepEqual([usd.getValueClipStartTime(), usd.getValueClipEndTime()],
      [nextDefaults.getValueClipStartTime(), nextDefaults.getValueClipEndTime()]);

    const boolPairs = [
      ['setCombineUDIMTiles', 'getCombineUDIMTiles'],
      ['setDeferTangentComputation', 'getDeferTangentComputation'],
      ['setEnableBoneReduction', 'getEnableBoneReduction'],
      ['setEnableValueClips', 'getEnableValueClips'],
      ['setRoundBoneCount', 'getRoundBoneCount'],
      ['setValueClipUseTimeRange', 'getValueClipUseTimeRange']
    ];
    for (const [set, get] of boolPairs) {
      for (const [input, expected] of [['x', true], [0, false], [undefined, false],
        [{}, true], [1, true], [false, false]]) {
        assert.equal(usd[set](input), undefined);
        assert.equal(usd[get](), expected, `${set}(${String(input)})`);
      }
    }

    usd.setSphereSubdivisions(3.7);
    assert.equal(usd.getSphereSubdivisions(), 3);
    usd.setSphereSubdivisions(true);
    assert.equal(usd.getSphereSubdivisions(), 1);
    usd.setSphereSubdivisions(9);
    assert.equal(usd.getSphereSubdivisions(), 1, 'out-of-range subdivisions are ignored');
    assert.throws(() => usd.setSphereSubdivisions('2'), TypeError);
    usd.setMaxMemoryLimitMB(NaN);
    assert.equal(usd.getMaxMemoryLimitMB(), 0);
    usd.setMaxMemoryLimitMB(-5);
    assert.equal(usd.getMaxMemoryLimitMB(), -5);
    assert.throws(() => usd.setMaxMemoryLimitMB(2 ** 31), TypeError);
    assert.throws(() => usd.setMaxMemoryLimitMB(5n), TypeError);
    usd.setMaxMemoryLimitMB(-(2 ** 31));
    assert.equal(usd.getMaxMemoryLimitMB(), -(2 ** 31));

    usd.setTargetBoneCount(64);
    assert.equal(usd.getTargetBoneCount(), 64);
    usd.setTargetBoneCount(200);
    assert.equal(usd.getTargetBoneCount(), 64, 'out-of-range bone counts are ignored');
    assert.throws(() => usd.setTargetBoneCount(-1), TypeError);
    assert.throws(() => usd.setTargetBoneCount(2 ** 32), TypeError);

    usd.setValueClipSampleRate(0.1);
    assert.equal(usd.getValueClipSampleRate(), Math.fround(0.1));
    assert.throws(() => usd.setValueClipSampleRate('3'), TypeError);
    assert.throws(() => usd.setValueClipSampleRate(null), TypeError);
    usd.setValueClipTimeRange(1.5, 1e300);
    assert.deepEqual([usd.getValueClipStartTime(), usd.getValueClipEndTime()], [1.5, 1e300]);
    usd.setValueClipTimeRange(NaN, -Infinity);
    assert.ok(Number.isNaN(usd.getValueClipStartTime()));
    assert.equal(usd.getValueClipEndTime(), -Infinity);

    for (const set of ['setEnableComposition', 'setLoadTextureInNative',
      'setNativeFlattenRenderTree', 'setNativeMaterialDedup', 'setNativeMeshMerge',
      'setNativeMeshMergeBakeTransform']) {
      assert.equal(usd[set](true), undefined);
      assert.equal(usd[set](0), undefined);
    }
    assert.equal(usd.setUSDCExportLimitMB(10, 20), undefined);

    assert.throws(() => usd.setEnableValueClips());
    assert.throws(() => usd.getEnableValueClips(1));
    assert.throws(() => usd.setUSDCExportLimitMB(10));
    assert.throws(() => usd.setValueClipTimeRange(1));
    assert.throws(() => usd.debugLogMemory(5));
    // The typed adapters report arity and receiver errors as TypeError.
    assert.throws(() => usd.setEnableValueClips(), TypeError);
    assert.throws(() => usd.setUSDCExportLimitMB(10), TypeError);
    const proto = native.LightUSDLoaderNative.prototype;
    assert.throws(() => proto.getSphereSubdivisions.call({}), TypeError);
    assert.throws(() => proto.setSphereSubdivisions.call({}, 2), TypeError);
    assert.throws(() => proto.getMemoryStats.call({}), TypeError);
    assert.throws(() => proto.debugLogMemory.call({}, 'x'), TypeError);
    assert.deepEqual(usd.debugLogMemory(''), {label: '', heapBytes: native.HEAPU8.length});

    const logged = usd.debugLogMemory('probe');
    assert.deepEqual(Object.keys(logged), ['label', 'heapBytes']);
    assert.equal(logged.label, 'probe');
    assert.equal(logged.heapBytes, native.HEAPU8.length);

    const emptyStats = usd.getMemoryStats();
    assert.deepEqual(emptyStats, {
      numMeshes: 0, numMaterials: 0, numTextures: 0, numImages: 0, numBuffers: 0,
      numNodes: 0, numLights: 0, bufferMemoryBytes: 0, bufferMemoryMB: 0,
      assetCacheCount: 0, assetCacheSizeBytes: 0,
      assetCacheMaxBytes: 0, reorderedMeshCacheCount: 0
    });
    usd.setAsset('cached.usda', new Uint8Array(1000));
    const scene = new TextEncoder().encode(`#usda 1.0
def Xform "World"
{
    def Mesh "Tri"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
    def SphereLight "Light" {}
}
`);
    assert.ok(usd.loadFromBinary(scene, 'scene.usda'), usd.error());
    assert.deepEqual(usd.getMemoryStats(), {
      numMeshes: 1, numMaterials: 0, numTextures: 0, numImages: 0, numBuffers: 0,
      numNodes: 1, numLights: 1, bufferMemoryBytes: 0, bufferMemoryMB: 0,
      assetCacheCount: 1, assetCacheSizeBytes: 1011, assetCacheMaxBytes: 0,
      reorderedMeshCacheCount: 0
    });
  } finally {
    nextDefaults.delete();
    usd.delete();
  }
});

await testAsync('streaming buffers preserve progress, bytes, ownership and failures', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    loader.setMMapZeroCopy(false);
    assert.equal(loader.getMMapZeroCopy(), false);
    loader.setMMapZeroCopy(true);
    assert.equal(loader.getMMapZeroCopy(), true);
    assert.deepEqual(loader.getStreamingProgress('missing'), {exists: false});
    assert.equal(loader.appendAssetChunk('missing', 'x'), false);
    assert.equal(loader.finalizeStreamingAsset('missing'), false);
    assert.equal(loader.isStreamingAssetComplete('missing'), false);
    const name = 'stream-\u2603.bin';
    assert.equal(loader.startStreamingAsset(name, 4), true);
    const initial = loader.getStreamingProgress(name);
    assert.deepEqual(initial, {exists: true, current: 0, total: 4,
      complete: false, uuid: initial.uuid, percentage: 0});
    assert.ok(initial.uuid.length);
    assert.equal(loader.appendAssetChunk(name, new Uint8Array([0, 255])), true);
    assert.equal(loader.getStreamingProgress(name).percentage, 50);
    assert.equal(loader.finalizeStreamingAsset(name), false);
    assert.equal(loader.appendAssetChunk(name, new Uint8Array([128, 1])), true);
    assert.equal(loader.isStreamingAssetComplete(name), true);
    assert.equal(loader.finalizeStreamingAsset(name), true);
    assert.deepEqual(loader.getStreamingProgress(name), {exists: false});
    assert.equal(loader.getAssetUUID(name), initial.uuid);
    assert.deepEqual([...loader.getAsset(name).data], [0, 255, 128, 1]);
    assert.equal(loader.startStreamingAsset('over', 1), true);
    assert.equal(loader.appendAssetChunk('over', 'ab'), false);
    assert.equal(loader.getStreamingProgress('over').current, 2);
    assert.equal(loader.isStreamingAssetComplete('over'), true);
    assert.equal(loader.finalizeStreamingAsset('over'), true);
    assert.equal(loader.startStreamingAsset('empty', 0), true);
    assert.equal(loader.isStreamingAssetComplete('empty'), false);

    assert.deepEqual(loader.allocateZeroCopyBuffer('bad', 0, 0),
      {success: false, error: 'Size must be greater than 0'});
    assert.deepEqual(loader.allocateZeroCopyBuffer('bad', 5, 4),
      {success: false, error: 'Buffer size exceeds 0 MiB limit'});
    assert.deepEqual(loader.getZeroCopyProgress('missing'), {exists: false});
    assert.equal(loader.getZeroCopyBufferPtr('missing'), 0);
    assert.equal(loader.getZeroCopyBufferPtrAtOffset('missing', 1), 0);
    assert.equal(loader.markZeroCopyBytesWritten('missing', 1), false);
    assert.equal(loader.finalizeZeroCopyBuffer('missing'), false);
    assert.equal(loader.cancelZeroCopyBuffer('missing'), false);
    const a = loader.allocateZeroCopyBuffer('zero-\u2603.bin', 4, 0);
    assert.equal(a.success, true);
    assert.equal(a.assetName, 'zero-\u2603.bin');
    assert.equal(a.totalSize, 4);
    assert.equal(typeof a.bufferPtr, 'number');
    assert.ok(a.bufferPtr > 0);
    assert.equal(loader.getZeroCopyBufferPtr(a.uuid), a.bufferPtr);
    assert.equal(loader.getZeroCopyBufferPtrAtOffset(a.uuid, 3), a.bufferPtr + 3);
    assert.equal(loader.getZeroCopyBufferPtrAtOffset(a.uuid, 4), 0);
    native.HEAPU8.set([0, 255, 128, 1], a.bufferPtr);
    assert.equal(loader.markZeroCopyBytesWritten(a.uuid, 1), true);
    const progress = loader.getZeroCopyProgress(a.uuid);
    assert.deepEqual(progress, {exists: true, uuid: a.uuid, assetName: a.assetName,
      totalSize: 4, bytesWritten: 1, progress: 0.25, isComplete: false,
      finalized: false, bufferPtr: a.bufferPtr});
    assert.equal(loader.finalizeZeroCopyBuffer(a.uuid), false);
    const b = loader.allocateZeroCopyBuffer('', 3, 0);
    const active = loader.getActiveZeroCopyBuffers();
    assert.deepEqual(active.map(x => x.uuid), [a.uuid, b.uuid].sort());
    const {exists, ...info} = progress;
    assert.deepEqual(active.find(x => x.uuid === a.uuid).info, info);
    assert.equal(loader.markZeroCopyBytesWritten(a.uuid, 4), false);
    assert.equal(loader.getZeroCopyProgress(a.uuid).bytesWritten, 4);
    assert.equal(loader.finalizeZeroCopyBuffer(a.uuid), true);
    assert.deepEqual(loader.getZeroCopyProgress(a.uuid), {exists: false});
    assert.equal(loader.getAssetUUID(a.assetName), a.uuid);
    assert.deepEqual([...loader.getAsset(a.assetName).data], [0, 255, 128, 1]);
    assert.equal(loader.cancelZeroCopyBuffer(b.uuid), true);
    assert.equal(loader.cancelZeroCopyBuffer(b.uuid), false);
    assert.deepEqual(loader.getActiveZeroCopyBuffers(), []);
  } finally {
    loader.delete();
  }
});

await testAsync('streaming methods use checked typed calls and reject stale receivers', () => {
  assert.equal(typeof native._lightusd_combined_stream_op, 'function');
  assert.equal(typeof native._lightusd_combined_stream_info_get, 'function');
  const savedOp = native._lightusd_combined_stream_op;
  const savedInfo = native._lightusd_combined_stream_info_get;
  const savedAllocate = native._lightusd_combined_stream_allocate;
  let ops = 0, infos = 0;
  native._lightusd_combined_stream_op = (...args) => { ++ops; return savedOp(...args); };
  native._lightusd_combined_stream_info_get = (...args) => { ++infos; return savedInfo(...args); };
  native._lightusd_combined_stream_allocate = (...args) => { ++infos; return savedAllocate(...args); };
  const loader = new native.LightUSDLoaderNative();
  try {
    const b = loader.allocateZeroCopyBuffer('overflow', 4, 0);
    assert.equal(loader.markZeroCopyBytesWritten(b.uuid, 1), true);
    assert.equal(loader.markZeroCopyBytesWritten(b.uuid, 0xffffffff), false);
    assert.equal(loader.getZeroCopyProgress(b.uuid).bytesWritten, 4);
    assert.equal(loader.cancelZeroCopyBuffer(b.uuid), true);
    assert.ok(ops >= 4 && infos >= 2);
    assert.throws(() => loader.allocateZeroCopyBuffer('bad', -1, 0), TypeError);
    assert.throws(() => loader.allocateZeroCopyBuffer('bad', Infinity, 0),
      wasm64 ? RangeError : TypeError);
    assert.throws(() => loader.getZeroCopyBufferPtrAtOffset('missing', -1), TypeError);
    assert.throws(() => loader.getStreamingProgress(), TypeError);
    assert.throws(() => loader.appendAssetChunk('missing', new Uint16Array(2)), TypeError);
    assert.throws(() => loader.getActiveZeroCopyBuffers.call({}), TypeError);
    // A retained raw view is read before native temporary allocations can grow
    // memory. Its bytes, embedded NULs and offset must reach the asset unchanged.
    const source = loader.allocateZeroCopyBuffer('source', 6, 0);
    native.HEAPU8.set([9, 0, 255, 128, 1, 9], source.bufferPtr);
    assert.equal(loader.startStreamingAsset('heap-view', 4), true);
    assert.equal(loader.appendAssetChunk('heap-view',
      native.HEAPU8.subarray(source.bufferPtr + 1, source.bufferPtr + 5)), true);
    assert.equal(loader.finalizeStreamingAsset('heap-view'), true);
    assert.deepEqual([...loader.getAsset('heap-view').data], [0, 255, 128, 1]);
    assert.equal(loader.cancelZeroCopyBuffer(source.uuid), true);
  } finally {
    loader.delete();
    native._lightusd_combined_stream_op = savedOp;
    native._lightusd_combined_stream_info_get = savedInfo;
    native._lightusd_combined_stream_allocate = savedAllocate;
  }
  assert.throws(() => loader.getZeroCopyProgress('missing'), TypeError);
  assert.throws(() => loader.startStreamingAsset('stale', 1), TypeError);
});

await testAsync('stream C boundary rejects invalid records, sizes and keys', () => {
  const loader = new native.LightUSDLoaderNative();
  const ptr = value => wasm64 ? BigInt(value) : Number(value);
  const out = native._lightusd_combined_alloc(40);
  try {
    const handle = ptr(loader.$$.ptr);
    const zero = ptr(0);
    const record = ptr(out);
    const op = (code, value = 0) => native._lightusd_combined_stream_op(
      handle, code, zero, 0, zero, 0, value);
    assert.equal(op(999), -1);
    assert.equal(op(0, -1), -1);
    assert.equal(op(0, 0.5), -1);
    assert.equal(op(0, Infinity), -1);
    assert.equal(op(0, NaN), -1);
    assert.equal(native._lightusd_combined_stream_op(
      handle, 1, zero, 1, zero, 0, 0), -1);
    assert.equal(native._lightusd_combined_stream_op(
      handle, 1, zero, 0, zero, 1, 0), -1);
    assert.equal(native._lightusd_combined_stream_op(
      zero, 0, zero, 0, zero, 0, 0), -1);
    const info = (kind, size = 0, cap = 0) =>
      native._lightusd_combined_stream_info_get(handle, kind, zero, 0, size, cap, record);
    new DataView(native.HEAPU8.buffer, Number(out), 40).setUint32(0, 39, true);
    assert.equal(info(2, 4), -1);
    new DataView(native.HEAPU8.buffer, Number(out), 40).setUint32(0, 40, true);
    assert.equal(info(999), -1);
    assert.equal(info(2, -1), -1);
    assert.equal(info(2, 4, -1), -1);
    assert.equal(native._lightusd_combined_stream_info_get(
      handle, 2, zero, 0, 4, 0, zero), -1);
    assert.deepEqual(loader.getActiveZeroCopyBuffers(), []);
  } finally {
    native._lightusd_combined_free(out);
    loader.delete();
  }
});

await testAsync('asset cache preserves owned copies, UUIDs, paths and eviction rules', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.equal(loader.getAssetCount(), wasm64 ? 0n : 0);
    assert.equal(loader.getAssetCacheMaxSizeBytes(), wasm64 ? 0n : 0);
    assert.equal(loader.getAssetCacheSizeBytes(), wasm64 ? 0n : 0);
    assert.deepEqual(loader.getAsset('missing'), {});
    assert.deepEqual(loader.getAssetByUUID('missing'), {error: 'Asset not found with UUID: missing'});
    assert.equal(loader.getAssetCacheDataAsMemoryView('missing'), undefined);
    assert.equal(loader.getAssetHash('missing'), '');
    assert.equal(loader.getAssetUUID('missing'), '');
    assert.equal(loader.getStreamingAssetUUID('missing'), '');
    assert.equal(loader.findAssetByUUID('missing'), '');
    assert.equal(loader.verifyAssetHash('missing', ''), false);
    assert.equal(loader.assetExists('missing'), false);
    assert.equal(loader.deleteAsset('missing'), false);
    assert.equal(loader.deleteAssetByName('missing'), false);
    assert.equal(loader.deleteAssetByUUID('missing'), false);
    assert.deepEqual(loader.getAllAssetUUIDs(), {});
    loader.clearAssetSearchPaths();
    assert.deepEqual(loader.getAssetSearchPaths(), []);
    assert.equal(loader.addAssetSearchPath('folder/\u2603'), undefined);
    loader.addAssetSearchPath('folder/\u2603');
    loader.addAssetSearchPath('../relative');
    assert.deepEqual(loader.getAssetSearchPaths(), ['folder/\u2603', 'folder/\u2603', '../relative']);
    assert.equal(loader.setBaseWorkingPath('root/\u2603'), undefined);
    assert.equal(loader.getBaseWorkingPath(), 'root/\u2603');
    loader.setAllowParentRelativeAssetPaths(true);
    assert.equal(loader.getAllowParentRelativeAssetPaths(), true);
    loader.setAllowParentRelativeAssetPaths(false);
    assert.equal(loader.getAllowParentRelativeAssetPaths(), false);

    const name = 'raw-\u2603.bin';
    const bytes = new Uint8Array([0, 255, 128, 1]);
    assert.equal(loader.setAsset(name, bytes), undefined);
    assert.equal(loader.hasAsset(name), true);
    assert.equal(loader.getAssetCount(), wasm64 ? 1n : 1);
    const saved = loader.getAsset(name);
    assert.equal(saved.name, name);
    assert.deepEqual([...saved.data], [...bytes]);
    assert.equal(saved.uuid, loader.getAssetUUID(name));
    assert.equal(saved.sha256, loader.getAssetHash(name));
    assert.equal(saved.sha256, createHash('sha256').update(bytes).digest('hex'));
    assert.equal(loader.verifyAssetHash(name, saved.sha256), true);
    assert.equal(loader.verifyAssetHash(name, 'bad'), false);
    assert.equal(loader.hasAsset(saved.uuid), false);
    assert.equal(loader.assetExists(saved.uuid), true);
    assert.equal(loader.findAssetByUUID(saved.uuid), name);
    assert.deepEqual(loader.getAssetByUUID(saved.uuid), saved);
    assert.deepEqual(loader.getAllAssetUUIDs(), {[name]: saved.uuid});
    const view = loader.getAssetCacheDataAsMemoryView(name);
    assert.equal(view.buffer, native.HEAPU8.buffer);
    assert.deepEqual([...view], [...bytes]);
    // Copying getters must not alias the retained bytes or each other.
    saved.data[0] = 42;
    assert.equal(loader.getAsset(name).data[0], 0);
    assert.equal(loader.deleteAsset(saved.uuid), true);
    assert.deepEqual([...saved.data], [42, 255, 128, 1]);
    assert.equal(loader.assetExists(saved.uuid), false);

    loader.setAsset('empty', new Uint8Array());
    assert.equal(loader.getAsset('empty').data.length, 0);
    assert.equal(loader.getAssetCacheDataAsMemoryView('empty').length, 0);
    assert.equal(loader.deleteAssetByName('empty'), true);
    loader.setAsset('', 'empty name');
    const emptyName = loader.getAsset('');
    assert.deepEqual(loader.getAssetByUUID(emptyName.uuid), emptyName);
    assert.equal(loader.deleteAssetByUUID(emptyName.uuid), true);
    loader.setAsset('same', 'first');
    const first = loader.getAssetUUID('same');
    loader.setAsset('same', 'second');
    assert.notEqual(loader.getAssetUUID('same'), first);
    assert.equal(loader.assetExists(first), false);
    loader.clearAssets();

    // The legacy limit is an eviction target: overwrite and individually
    // oversized entries are still admitted, and name order chooses eviction.
    assert.equal(loader.setAssetCacheMaxSizeBytes(6), undefined);
    loader.setAsset('b', '12');
    loader.setAsset('a', '34');
    assert.equal(loader.getAssetCacheSizeBytes(), wasm64 ? 6n : 6);
    loader.setAsset('c', '56');
    assert.equal(loader.hasAsset('a'), false);
    assert.equal(loader.hasAsset('b'), true);
    loader.setAsset('b', '123456789');
    assert.equal(loader.getAssetCacheSizeBytes(), wasm64 ? 13n : 13);
    loader.setAsset('large', '123456789');
    assert.equal(loader.getAssetCount(), wasm64 ? 1n : 1);
    assert.equal(loader.getAssetCacheSizeBytes(), wasm64 ? 14n : 14);
    assert.equal(loader.getAssetCacheMaxSizeBytes(), wasm64 ? 6n : 6);
    loader.setAssetCacheMaxSizeBytes(0);
    loader.clearAssets();

    const source = loader.allocateZeroCopyBuffer('source', 4, 0);
    native.HEAPU8.set(bytes, source.bufferPtr);
    // Return value means overwritten, not successfully inserted.
    assert.equal(loader.setAssetFromRawPointer('ingested', source.bufferPtr, 4), false);
    assert.deepEqual([...loader.getAsset('ingested').data], [...bytes]);
    assert.equal(loader.setAssetFromRawPointer('ingested', source.bufferPtr, 4), true);
    assert.equal(loader.setAssetFromRawPointer('bad', 0, 4), false);
    assert.equal(loader.setAssetFromRawPointer('bad', source.bufferPtr, 0), false);
    assert.equal(loader.setAssetFromRawPointer('bad', source.bufferPtr, 0x40000001), false);
    assert.equal(loader.hasAsset('bad'), false);
    loader.startStreamingAsset('pending', 2);
    const streamUuid = loader.getStreamingAssetUUID('pending');
    assert.ok(streamUuid.length);
    assert.equal(loader.getStreamingProgress('pending').uuid, streamUuid);
    loader.clearAssets();
    assert.equal(loader.getAssetCount(), wasm64 ? 0n : 0);
    assert.equal(loader.getStreamingAssetUUID('pending'), '');
    assert.equal(loader.getZeroCopyProgress(source.uuid).exists, true);
    loader.cancelZeroCopyBuffer(source.uuid);
    assert.equal(loader.getAssetCacheSizeBytes(), wasm64 ? 0n : 0);
  } finally {
    loader.delete();
  }
});

await testAsync('asset size_t conversion preserves width and exact cache limits', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    const maximum = wasm64 ? 0xffffffffffffffffn : 0xffffffff;
    loader.setAssetCacheMaxSizeBytes(maximum);
    assert.equal(loader.getAssetCacheMaxSizeBytes(), maximum);
    if (wasm64) {
      loader.setAssetCacheMaxSizeBytes(0x20000000000001n);
      assert.equal(loader.getAssetCacheMaxSizeBytes(), 0x20000000000001n);
      for (const value of [true, '1', -1, -1n, 0x10000000000000000n]) {
        assert.throws(() => loader.setAssetCacheMaxSizeBytes(value), TypeError);
      }
      for (const value of [1.5, NaN, Infinity]) {
        assert.throws(() => loader.setAssetCacheMaxSizeBytes(value), RangeError);
      }
    } else {
      loader.setAssetCacheMaxSizeBytes(true);
      assert.equal(loader.getAssetCacheMaxSizeBytes(), 1);
      loader.setAssetCacheMaxSizeBytes(1.5);
      assert.equal(loader.getAssetCacheMaxSizeBytes(), 1);
      loader.setAssetCacheMaxSizeBytes(NaN);
      assert.equal(loader.getAssetCacheMaxSizeBytes(), 0);
      for (const value of [-1, 0x100000000, 1n, '1', Infinity]) {
        assert.throws(() => loader.setAssetCacheMaxSizeBytes(value));
      }
    }
  } finally {
    loader.delete();
  }
});

await testAsync('streaming size_t inputs preserve memory64 BigInt conversion', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    const maximum = wasm64 ? 0xffffffffffffffffn : 0xffffffff;
    const zero = wasm64 ? 0n : 0;
    const three = wasm64 ? 3n : 3;
    assert.deepEqual(loader.allocateZeroCopyBuffer('empty', zero, maximum),
      {success: false, error: 'Size must be greater than 0'});
    assert.equal(loader.startStreamingAsset('huge', maximum), true);
    assert.equal(loader.getStreamingProgress('huge').total, Number(maximum));
    assert.equal(loader.isStreamingAssetComplete('huge'), false);
    const buffer = loader.allocateZeroCopyBuffer('sized', three, maximum);
    assert.equal(buffer.success, true);
    assert.equal(loader.getZeroCopyBufferPtrAtOffset(buffer.uuid, maximum), 0);
    assert.equal(loader.markZeroCopyBytesWritten(buffer.uuid, maximum), false);
    assert.equal(loader.getZeroCopyProgress(buffer.uuid).bytesWritten, 3);
    assert.equal(loader.cancelZeroCopyBuffer(buffer.uuid), true);
    if (wasm64) {
      for (const value of [true, '1', -1, -1n, 0x10000000000000000n]) {
        assert.throws(() => loader.startStreamingAsset('bad', value), TypeError);
      }
      for (const value of [1.5, NaN, Infinity]) {
        assert.throws(() => loader.startStreamingAsset('bad', value), RangeError);
        assert.throws(() => loader.allocateZeroCopyBuffer('bad', value, 0), RangeError);
      }
    } else {
      assert.equal(loader.startStreamingAsset('fraction', 1.5), true);
      assert.equal(loader.getStreamingProgress('fraction').total, 1);
      assert.equal(loader.startStreamingAsset('nan', NaN), true);
      assert.equal(loader.getStreamingProgress('nan').total, 0);
      assert.throws(() => loader.startStreamingAsset('bad', 1n));
    }
  } finally {
    loader.delete();
  }
});

await testAsync('asset methods use typed C and reject invalid spans and stale receivers', () => {
  const exports = ['_lightusd_combined_asset_op', '_lightusd_combined_asset_size_op',
    '_lightusd_combined_asset_info_get', '_lightusd_combined_asset_strings',
    '_lightusd_combined_asset_set_raw'];
  const saved = new Map();
  const calls = new Map();
  for (const name of exports) {
    assert.equal(typeof native[name], 'function');
    saved.set(name, native[name]);
    calls.set(name, 0);
    native[name] = (...args) => {
      calls.set(name, calls.get(name) + 1);
      return saved.get(name)(...args);
    };
  }
  const loader = new native.LightUSDLoaderNative();
  const ptr = value => wasm64 ? BigInt(value) : Number(value);
  const out = native._lightusd_combined_alloc(24);
  try {
    loader.setAsset('typed', new Uint8Array([0, 255, 1]));
    assert.deepEqual([...loader.getAsset('typed').data], [0, 255, 1]);
    assert.ok(loader.getAssetUUID('typed').length);
    assert.equal(loader.getAssetCount(), wasm64 ? 1n : 1);
    const maximum = wasm64 ? 0xffffffffffffffffn : 0xffffffff;
    assert.equal(loader.setAssetFromRawPointer('oob', maximum, 4), false);
    assert.equal(loader.setAssetFromRawPointer('oob', 1, maximum), false);
    assert.equal(loader.hasAsset('oob'), false);
    for (const name of exports) assert.ok(calls.get(name) > 0, name);
    assert.throws(() => loader.getAsset(), TypeError);
    assert.throws(() => loader.setAsset('bad', new Uint16Array(1)), TypeError);
    assert.throws(() => loader.getAllAssetUUIDs.call({}), TypeError);

    const handle = ptr(loader.$$.ptr);
    const zero = ptr(0);
    assert.equal(native._lightusd_combined_asset_op(handle, 999, zero, 0, zero, 0, 0), -1);
    assert.equal(native._lightusd_combined_asset_op(handle, 0, zero, 1, zero, 0, 0), -1);
    assert.equal(native._lightusd_combined_asset_op(handle, 0, zero, 0, zero, 1, 0), -1);
    assert.equal(native._lightusd_combined_asset_op(zero, 10, zero, 0, zero, 0, 0), -1);
    assert.equal(native._lightusd_combined_asset_strings(handle, 999, zero, 0), -1);
    assert.equal(native._lightusd_combined_asset_size_op(handle, 5, 0, 0, zero), -1);
    assert.equal(native._lightusd_combined_asset_size_op(handle, 999, 0, 0, ptr(out)), -1);
    if (!wasm64) {
      assert.equal(native._lightusd_combined_asset_size_op(handle, 7, 0, 1, zero), -1);
      assert.equal(loader.getAssetCacheMaxSizeBytes(), 0);
    }
    new DataView(native.HEAPU8.buffer, Number(out), 24).setUint32(0, 23, true);
    assert.equal(native._lightusd_combined_asset_info_get(handle, 0, zero, 0, ptr(out)), -1);
    new DataView(native.HEAPU8.buffer, Number(out), 24).setUint32(0, 24, true);
    assert.equal(native._lightusd_combined_asset_info_get(handle, 2, zero, 0, ptr(out)), -1);
    assert.equal(native._lightusd_combined_asset_info_get(handle, 0, zero, 0, zero), -1);
    assert.equal(native._lightusd_combined_asset_info_get(handle, 0, zero, 1, ptr(out)), -1);
    assert.equal(loader.hasAsset('typed'), true);
  } finally {
    for (const [name, fn] of saved) native[name] = fn;
    native._lightusd_combined_free(out);
    loader.delete();
  }
  assert.throws(() => loader.getAsset('typed'), TypeError);
  assert.throws(() => loader.clearAssets(), TypeError);
});

await testAsync('asset SHA-256 matches standard digests across padding boundaries', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    for (const size of [0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 1000]) {
      const bytes = Uint8Array.from({length: size}, (_, i) => i & 255);
      loader.setAsset('digest', bytes);
      assert.equal(loader.getAssetHash('digest'),
        createHash('sha256').update(bytes).digest('hex'), `length ${size}`);
    }
  } finally {
    loader.delete();
  }
});

await testAsync('loader lifecycle, progress and validation preserve observable state', () => {
  const loader = new native.LightUSDLoaderNative();
  const scene = new TextEncoder().encode('#usda 1.0\ndef Xform "World" {}\n');
  const idle = {progress: 0, stage: 'idle', currentOperation: '',
    cancelRequested: false, errorMessage: '', bytesProcessed: 0, totalBytes: 0,
    percentage: 0, meshesProcessed: 0, meshesTotal: 0, currentMeshName: '',
    materialsProcessed: 0, materialsTotal: 0, tydraStage: ''};
  try {
    assert.equal(loader.ok(), false);
    assert.equal(loader.error(), '');
    assert.equal(loader.warn(), '');
    assert.deepEqual(loader.getProgress(), idle);
    assert.equal(loader.cancelParsing(), undefined);
    assert.deepEqual(loader.getProgress(), {...idle, cancelRequested: true});
    assert.equal(loader.wasCancelled(), false);
    assert.equal(loader.isParsingInProgress(), false);
    assert.equal(loader.resetProgress(), undefined);
    assert.deepEqual(loader.getProgress(), idle);
    assert.equal(loader.loadFromCachedAsset('missing'), false);
    assert.equal(loader.error(), 'Asset not found in cache: missing');
    loader.setAsset('empty', new Uint8Array());
    assert.equal(loader.loadAsLayerFromCachedAsset('empty'), false);
    assert.equal(loader.error(), 'Cached asset is empty: empty');
    assert.equal(loader.reset(), undefined);
    assert.equal(loader.error(), '');
    assert.equal(loader.warn(), '');
    assert.equal(loader.getAssetCount(), wasm64 ? 0n : 0);
    assert.deepEqual(JSON.parse(loader.validateLoadedLayer('{}')), {
      error: 'No Layer is loaded. Use loadAsLayerFromBinary first.', ok: false, parse_ok: false
    });
    assert.equal(loader.loadAsLayerFromBinary(scene, 'layer.usda'), true);
    assert.equal(loader.ok(), true);
    const validation = JSON.parse(loader.validateLoadedLayer('{}'));
    assert.equal(validation.parse_ok, true);
    assert.equal(validation.ok, true);
    assert.deepEqual(JSON.parse(loader.validateFromBinary(scene, 'layer.usda', '{}')), validation);
    const json = loader.layerToJSON();
    loader.reset();
    assert.equal(loader.loadLayerFromJSON(json), true);
    assert.match(loader.layerToString(), /World/);
    assert.equal(loader.loadLayerFromJSON('{'), false);
    assert.equal(loader.ok(), false);
    assert.ok(loader.error().length);
    loader.reset();
    assert.equal(loader.loadAsLayerFromBinaryWithProgress(scene, 'progress.usda'), true);
    const layerProgress = loader.getProgress();
    assert.equal(layerProgress.stage, 'complete');
    assert.equal(layerProgress.currentOperation, 'Complete');
    assert.equal(layerProgress.progress, 1);
    assert.equal(layerProgress.percentage, 100);
    assert.equal(layerProgress.totalBytes, scene.length);
    assert.equal(loader.isParsingInProgress(), false);
    assert.equal(loader.releaseSourceLayer(), undefined);
    assert.match(loader.layerToString(), /World/);
    loader.reset();
    loader.setAsset('cached.usda', scene);
    assert.equal(loader.loadAsLayerFromCachedAsset('cached.usda'), true);
    assert.equal(loader.loadFromCachedAsset('cached.usda'), true);
    assert.equal(loader.ok(), true);
    loader.reset();
    assert.equal(loader.loadFromBinaryWithProgress(scene, 'render.usda'), true);
    assert.equal(loader.getProgress().stage, 'complete');
    assert.equal(loader.getProgress().totalBytes, scene.length);
    assert.equal(loader.loadFromBinaryWithProgress('bad', 'bad.usda'), false);
    assert.equal(loader.getProgress().stage, 'error');
    assert.equal(loader.getProgress().errorMessage, loader.error());
    assert.equal(loader.wasCancelled(), false);
    loader.reset();
    assert.equal(loader.loadTest('probe.usda', scene), true);
    assert.equal(loader.ok(), true);
    assert.equal(loader.getURI(), 'probe.usda');
    assert.equal(loader.layerToString(), '');
    assert.equal(loader.loadTest('probe.usda', new Uint8Array()), false);
    loader.reset();
    assert.equal(loader.loadFromBinary(scene, 'plain.usda'), true);
    assert.equal(loader.ok(), true);
    assert.deepEqual(loader.getProgress(), {...idle, progress: 1, percentage: 100,
      currentOperation: 'Conversion complete', tydraStage: 'complete'});
    const invalid = JSON.parse(loader.validateFromBinary('bad', 'bad.usda', '{}'));
    assert.equal(invalid.parse_ok, false);
    assert.equal(invalid.ok, false);
    assert.ok(invalid.error.length);
  } finally {
    loader.delete();
  }
});

await testAsync('async loading preserves phase yields and result payloads', async () => {
  const loader = new native.LightUSDLoaderNative();
  const phaseCallback = native.onAsyncPhaseStart;
  const phases = [];
  const bytes = new TextEncoder().encode(`#usda 1.0
  def Mesh "Triangle" {
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0,1,2]
    uniform token subdivisionScheme = "none"
  }`);
  native.onAsyncPhaseStart = value => phases.push(value.phase);
  try {
    let eventLoopRan = false;
    setTimeout(() => { eventLoopRan = true; }, 0);
    const pending = loader.loadFromBinaryAsync(bytes, 'async.usda');
    assert.equal(typeof pending.then, 'function');
    assert.deepEqual(phases, ['detecting']);
    const result = await pending;
    assert.equal(eventLoopRan, true);
    assert.deepEqual(phases, ['detecting', 'parsing', 'setup', 'assets', 'meshes', 'complete']);
    assert.deepEqual(result, {success: true, meshCount: 1, materialCount: 0, textureCount: 0});
    assert.equal(loader.ok(), true);
    assert.equal(loader.numMeshes(), 1);
    phases.length = 0;
    const bad = await loader.loadFromBinaryAsync('bad', 'bad.usda');
    assert.equal(bad.success, false);
    assert.ok(bad.error.length);
    assert.deepEqual(phases, ['detecting', 'parsing']);
    assert.equal(loader.ok(), false);
    assert.equal(bad.error, loader.error());
  } finally {
    native.onAsyncPhaseStart = phaseCallback;
    loader.delete();
  }
});

await testAsync('value memory probe retains names, sizes and totals', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    for (const size of [0, 3, 10, null, undefined]) {
      const result = loader.testValueMemoryUsage(size);
      assert.equal(result.success, true);
      assert.equal(result.arrayLength, size == null ? 10000 : size);
      assert.equal(result.totalTests, 19);
      assert.equal(result.tests.length, 19);
      assert.equal(result.tests[0].name, 'Empty value');
      assert.equal(result.tests[18].name, 'quatf');
      const expected = result.tests.reduce((sum, test) => sum + test.bytes, wasm64 ? 0n : 0);
      assert.equal(result.totalMemory, expected);
      assert.ok(result.tests.every(test => typeof test.bytes === (wasm64 ? 'bigint' : 'number')));
      assert.ok(result.tests.some(test => test.name === `float array (${result.arrayLength} elements)`));
    }
  } finally {
    loader.delete();
  }
});

await testAsync('typed synchronous loading preserves byte views and unwinds callback errors', () => {
  const loader = new native.LightUSDLoaderNative();
  const scene = new TextEncoder().encode('#usda 1.0\ndef Xform "World" {}\n');
  const padded = new Uint8Array(scene.length + 8);
  padded.set(scene, 4);
  const nextDocument = new nextRuntime.LayerDocument();
  const previous = native.onLightUSDDebug;
  try {
    assert.equal(loader.loadTest('view.usda', new DataView(padded.buffer, 4, scene.length)), true);
    const nextLoad = nextDocument.load(new DataView(padded.buffer, 4, scene.length));
    assert.equal(nextLoad.success, true, nextLoad.error || nextDocument.error());
    assert.equal(nextLoad.primCount, 1,
      'next LayerDocument.load consumes the same DataView slice into one authored prim');
    const error = new TypeError('intentional debug callback failure');
    let calls = 0;
    native.onLightUSDDebug = () => { ++calls; throw error; };
    assert.throws(() => loader.loadFromBinary(scene, 'debug.usda'), value => value === error);
    assert.equal(calls, 1);
    native.onLightUSDDebug = previous;
    assert.equal(loader.loadFromBinary(scene, 'recovered.usda'), true);
    assert.equal(loader.ok(), true);
  } finally {
    native.onLightUSDDebug = previous;
    loader.delete();
    nextDocument.delete();
  }
});

await testAsync('typed loading validates records and releases async owners on callback failure', async () => {
  const loader = new native.LightUSDLoaderNative();
  const scene = new TextEncoder().encode('#usda 1.0\ndef Xform "World" {}\n');
  const ptr = value => wasm64 ? BigInt(value) : Number(value);
  const out = native._lightusd_combined_alloc(72);
  const handle = ptr(loader.$$.ptr);
  const phaseCallback = native.onAsyncPhaseStart;
  try {
    assert.equal(native._lightusd_combined_loading_op(ptr(0), 0, ptr(0), 0, ptr(0), 0, ptr(0), 0), -1);
    assert.equal(native._lightusd_combined_loading_op(handle, 999, ptr(0), 0, ptr(0), 0, ptr(0), 0), -1);
    assert.equal(native._lightusd_combined_loading_op(handle, 0, ptr(0), 1, ptr(0), 0, ptr(0), 0), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 71, true);
    assert.equal(native._lightusd_combined_loading_progress_get(handle, out), -1);
    assert.equal(native._lightusd_combined_loading_progress_get(handle, ptr(0)), -1);
    assert.equal(native._lightusd_combined_loading_async_begin(handle, ptr(0), 1, ptr(0), 0), 0);
    assert.equal(native._lightusd_combined_loading_async_step(handle, 123, out), -1);
    const task = native._lightusd_combined_loading_async_begin(handle, ptr(0), 0, ptr(0), 0);
    assert.ok(task > 0);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 15, true);
    assert.equal(native._lightusd_combined_loading_async_step(handle, task, out), -1);
    assert.equal(native._lightusd_combined_loading_async_end(handle, task), 0);
    assert.equal(native._lightusd_combined_loading_async_end(handle, task), -1);
    assert.equal(loader.testValueMemoryUsage(-1).success, false);
    loader.setMaxMemoryLimitMB(1);
    assert.equal(loader.testValueMemoryUsage(2147483647).success, false);
    loader.setMaxMemoryLimitMB(128);
    let callbacks = 0;
    const thrown = new TypeError('intentional phase callback failure');
    native.onAsyncPhaseStart = () => { ++callbacks; throw thrown; };
    await assert.rejects(loader.loadFromBinaryAsync(scene, 'callback.usda'), error => error === thrown);
    assert.equal(callbacks, 1, 'callback TypeErrors must not retry a native operation');
    // The callback must stop touching the deleted JS receiver on subsequent phases.
    native.onAsyncPhaseStart = event => {
      if (event.phase === 'detecting') {
        assert.equal(loader.ok(), false);
        loader.delete();
        native.onAsyncPhaseStart = () => {};
      }
    };
    assert.equal((await loader.loadFromBinaryAsync(scene, 'retained.usda')).success, true);
    assert.throws(() => loader.getProgress(), TypeError);
    assert.throws(() => loader.loadFromBinaryAsync(scene, 'dead.usda'), TypeError);
  } finally {
    native.onAsyncPhaseStart = phaseCallback;
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('MCP contexts preserve JSON results and isolate session assets', () => {
  const loader = new native.LightUSDLoaderNative();
  const other = new native.LightUSDLoaderNative();
  const parse = text => JSON.parse(text);
  try {
    for (const result of [loader.mcpToolsList(), loader.mcpResourcesList(),
      loader.mcpToolsCall('get_version', '{}'), loader.mcpResourcesRead('missing')]) {
      assert.deepEqual(parse(result), {error: 'invalid session_id'});
    }
    assert.equal(loader.mcpSelectContext('missing'), false);
    assert.equal(loader.mcpCreateContext('session/日本語\0a'), true);
    assert.equal(loader.mcpCreateContext('session/日本語\0a'), false);
    const tools = parse(loader.mcpToolsList()).tools;
    assert.ok(tools.some(tool => tool.name === 'store_asset'));
    assert.ok(tools.some(tool => tool.name === 'get_version'));
    assert.deepEqual(parse(loader.mcpResourcesList()), {resources: []});
    assert.deepEqual(parse(loader.mcpToolsCall('get_version', '{')), {error: 'Invalid JSON'});
    assert.equal(parse(loader.mcpToolsCall('unknown', '{}')).isError, true);
    assert.deepEqual(parse(loader.mcpResourcesRead('missing')), {isError: true});
    const name = 'asset/日本語\0.bin';
    const store = data => parse(loader.mcpToolsCall('store_asset', JSON.stringify({name, data})));
    const first = store('AP8B');
    assert.equal(first.content[0].type, 'text');
    assert.ok(first.content[0].text.length > 0);
    const resource = {contents: [{type: 'text', text: 'AP8B'}]};
    assert.deepEqual(parse(loader.mcpResourcesRead(name)), resource);
    assert.deepEqual(parse(loader.mcpResourcesList()), {
      resources: [{name, mimeType: 'application/octet-stream'}]});
    assert.equal(loader.mcpCreateContext('second'), true);
    assert.deepEqual(parse(loader.mcpResourcesList()), {resources: []});
    store('second-payload');
    assert.equal(loader.mcpSelectContext('missing'), false);
    assert.equal(parse(loader.mcpResourcesRead(name)).contents[0].text, 'second-payload');
    assert.equal(loader.mcpSelectContext('session/日本語\0a'), true);
    assert.deepEqual(parse(loader.mcpResourcesRead(name)), resource);
    assert.equal(other.mcpCreateContext('session/日本語\0a'), true);
    assert.deepEqual(parse(other.mcpResourcesList()), {resources: []});
    assert.equal(loader.mcpCreateContext(''), true);
    assert.equal(loader.mcpSelectContext(''), true);
    assert.equal(loader.mcpCreateContext(new TextEncoder().encode('byte-session')), true);
    assert.equal(loader.mcpSelectContext('byte-session'), true);
  } finally { loader.delete(); other.delete(); }
});

await testAsync('MCP adapters use counted C calls without replaying failures', () => {
  const loader = new native.LightUSDLoaderNative();
  const key = '_lightusd_combined_mcp_op';
  const original = native[key];
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr);
  const ops = [];
  assert.equal(typeof original, 'function');
  native[key] = (...args) => { ops.push(args[1]); return original(...args); };
  try {
    assert.equal(loader.mcpCreateContext('typed'), true);
    assert.equal(loader.mcpSelectContext('typed'), true);
    loader.mcpToolsList();
    loader.mcpToolsCall('get_version', '{}');
    loader.mcpResourcesList();
    loader.mcpResourcesRead('absent');
    assert.deepEqual(ops, [0, 1, 2, 3, 4, 5]);
    assert.equal(original(pointer(0), 0, pointer(0), 0, pointer(0), 0), -1);
    assert.equal(original(handle, 999, pointer(0), 0, pointer(0), 0), -1);
    assert.equal(original(handle, 0, pointer(0), 1, pointer(0), 0), -1);
    assert.equal(original(handle, 3, pointer(0), 0, pointer(0), 1), -1);
    assert.throws(() => loader.mcpCreateContext(), TypeError);
    assert.throws(() => loader.mcpToolsList('extra'), TypeError);
    assert.throws(() => loader.mcpToolsCall('get_version', {}), TypeError);
    assert.throws(() => loader.mcpResourcesRead(new Uint16Array(2)), TypeError);
    let count = 0;
    const failure = new TypeError('deliberate tool dispatch error');
    native[key] = () => { ++count; throw failure; };
    assert.throws(() => loader.mcpToolsCall('get_version', '{}'), error => error === failure);
    assert.equal(count, 1);
    native[key] = original;
    assert.equal(loader.mcpSelectContext('typed'), true);
    loader.delete();
    assert.throws(() => loader.mcpToolsList(), TypeError);
    assert.throws(() => native.LightUSDLoaderNative.prototype.mcpToolsList.call({}), TypeError);
  } finally {
    native[key] = original;
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('layer text, JSON, composition and owned USDC exports preserve results', () => {
  const loader = new native.LightUSDLoaderNative();
  const reopened = new native.LightUSDLoaderNative();
  const source = '#usda 1.0\ndef Xform "World" { custom string label = "hello" }\n';
  try {
    assert.equal(loader.layerToString(), '');
    assert.deepEqual(JSON.parse(loader.layerToJSON()), {error: 'No layer loaded'});
    assert.deepEqual(JSON.parse(loader.layerToJSONWithOptions(false, 'buffer')), {error: 'No layer loaded'});
    assert.equal(loader.exportAsUSDA(), '');
    assert.equal(loader.exportAsUSDC(), null);
    assert.equal(loader.exportLayerAsUSDCWithOptions({}), null);
    assert.equal(loader.flattenLayer(), false);
    assert.equal(loader.layerToRenderScene(), false);
    assert.equal(loader.loadAsLayerFromBinary(source, 'export.usda'), true);
    assert.ok(loader.layerToString().includes('hello'));
    const json = JSON.parse(loader.layerToJSON());
    for (const mode of ['base64', 'buffer', 'unknown']) {
      const text = loader.layerToJSONWithOptions(true, mode);
      assert.equal(typeof JSON.parse(text), 'object');
      assert.ok(text.includes('hello'));
      if (mode === 'unknown') assert.equal(text, loader.layerToJSONWithOptions(true, 'base64'));
    }
    assert.equal(reopened.loadLayerFromJSON(JSON.stringify(json)), false);
    assert.equal(loader.flattenLayer(), true);
    assert.equal(loader.layerToRenderScene(), true);
    assert.ok(loader.exportAsUSDA().includes('hello'));
    const stage = loader.exportAsUSDC();
    const layer = loader.exportLayerAsUSDCWithOptions(null);
    assert.ok(stage instanceof Uint8Array && stage.length > 8);
    assert.ok(layer instanceof Uint8Array && layer.length > 8);
    assert.notEqual(stage.buffer, native.HEAPU8.buffer);
    assert.notEqual(layer.buffer, native.HEAPU8.buffer);
    const expected = layer.slice();
    const buffer = new Uint8Array(layer.length + 4);
    const result = loader.exportLayerAsUSDCToBufferWithOptions(buffer, {});
    assert.equal(result.success, true);
    assert.equal(result.size, layer.length);
    assert.deepEqual(buffer.slice(0, result.size), layer);
    loader.reset();
    loader.delete();
    assert.deepEqual(layer, expected);
    for (const bytes of [stage, layer]) {
      assert.equal(reopened.loadAsLayerFromBinary(bytes, 'export.usdc'), true);
      assert.ok(reopened.layerToString().includes('hello'));
    }
  } finally {
    if (!loader.isDeleted()) loader.delete();
    reopened.delete();
  }
});

await testAsync('typed export records reject invalid calls and retain bytes independently', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const ptr = pointer(loader.$$.ptr);
  const out = native._lightusd_combined_alloc(24);
  let result = 0;
  try {
    assert.equal(native._lightusd_combined_export_op(pointer(0), 0, 0, pointer(0), 0), -1);
    assert.equal(native._lightusd_combined_export_op(ptr, 999, 0, pointer(0), 0), -1);
    assert.equal(native._lightusd_combined_export_op(ptr, 2, 2, pointer(0), 0), -1);
    assert.equal(native._lightusd_combined_export_op(ptr, 2, 0, pointer(0), 1), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 23, true);
    assert.equal(native._lightusd_combined_export_usdc(ptr, 0, out), pointer(0));
    assert.equal(native._lightusd_combined_export_usdc(ptr, 0, pointer(0)), pointer(0));
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 24, true);
    assert.equal(native._lightusd_combined_export_usdc(ptr, 2, out), pointer(0));
    assert.equal(native._lightusd_combined_export_usdc(ptr, 0, out), pointer(0));
    assert.throws(() => loader.exportAsUSDC({}), TypeError);
    assert.throws(() => loader.exportLayerAsUSDCWithOptions(), TypeError);
    assert.throws(() => loader.layerToJSONWithOptions(false, {}), TypeError);
    assert.equal(loader.loadAsLayerFromBinary('#usda 1.0\ndef Xform "World" {}\n', 'owned.usda'), true);
    result = native._lightusd_combined_export_usdc(ptr, 1, out);
    assert.ok(result);
    const view = new DataView(native.HEAPU8.buffer, Number(out), 24);
    const size = view.getFloat64(8, true), start = view.getFloat64(16, true);
    const expected = native.HEAPU8.slice(start, start + size);
    loader.exportAsUSDC();
    loader.reset();
    loader.delete();
    assert.deepEqual(native.HEAPU8.slice(start, start + size), expected);
    assert.throws(() => loader.exportAsUSDC(), TypeError);
    assert.throws(() => loader.layerToString(), TypeError);
    const reopened = new native.LightUSDLoaderNative();
    try {
      assert.equal(reopened.loadAsLayerFromBinary(expected, 'owned.usdc'), true);
      assert.ok(reopened.layerToString().includes('World'));
    } finally { reopened.delete(); }
  } finally {
    if (result) native._lightusd_combined_export_release(result);
    native._lightusd_combined_export_release(pointer(0));
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('USDC buffer exports preserve validation order, copies and retries', () => {
  const loader = new native.LightUSDLoaderNative();
  const source = '#usda 1.0\ndef Xform "World" {}\n';
  const layer = 'exportLayerAsUSDCToBufferWithOptions';
  const stage = 'exportStageAsUSDCToBufferWithOptions';
  const failure = error => ({success: false, size: 0, error});
  try {
    let touched = false;
    const unread = {get byteLength() { touched = true; throw new Error('unread'); }};
    assert.deepEqual(loader[layer](unread, {}), failure('No layer loaded'));
    assert.equal(touched, false);
    assert.deepEqual(loader[stage](null, {}), failure('USDC export output buffer is null.'));
    assert.equal(loader.loadAsLayerFromBinary(source, 'buffer.usda'), true);
    for (const name of [layer, stage]) {
      for (const value of [null, undefined])
        assert.deepEqual(loader[name](value, {}), failure('USDC export output buffer is null.'));
      for (const value of [{}, {byteLength: '12'}, {byteLength: -1}, {byteLength: Infinity}])
        assert.deepEqual(loader[name](value, {}), failure('USDC export output must be a Uint8Array.'));
      assert.deepEqual(loader[name](new Uint8Array(0), {}), failure('USDC export output buffer is empty.'));
      assert.deepEqual(loader[name](new Uint8Array(1), {}), failure('USDC export output buffer too small.'));
      assert.equal(loader.error(), 'USDC export output buffer too small.');
      const expected = name === layer ? loader.exportLayerAsUSDCWithOptions({}) : loader.exportAsUSDC();
      const padded = new Uint8Array(expected.length + 16).fill(211);
      const target = padded.subarray(7, padded.length - 7);
      const result = loader[name](target, {ignored: true});
      assert.deepEqual(result, {success: true, size: expected.length, warn: loader.warn()});
      assert.deepEqual(target.slice(0, result.size), expected);
      assert.ok(padded.subarray(0, 7).every(x => x === 211));
      assert.ok(padded.subarray(7 + result.size).every(x => x === 211));
      // The legacy API accepts byteLength/set duck typing despite its diagnostic.
      let calls = 0;
      const custom = {byteLength: expected.length, set(bytes, offset) {
        ++calls; assert.equal(offset, 0); assert.deepEqual(bytes, expected);
        // A nested export must not invalidate the outer native result.
        assert.ok(loader.exportAsUSDC().length > 0);
        assert.deepEqual(bytes, expected);
      }};
      assert.equal(loader[name](custom, null).success, true);
      assert.equal(calls, 1);
      const thrown = new TypeError('copy callback failed');
      assert.throws(() => loader[name]({byteLength: expected.length, set() { throw thrown; }}, {}),
        error => error === thrown);
      assert.equal(loader[name](new Uint8Array(expected.length), {}).success, true);
    }
  } finally { loader.delete(); }
});

await testAsync('USDZ exports preserve root formats, cache filtering and remap ownership', () => {
  const loader = new native.LightUSDLoaderNative();
  const reader = new native.LightUSDLoaderNative();
  const source = '#usda 1.0\n(upAxis = "Z")\ndef Shader "Texture" {\n uniform token info:id = "UsdUVTexture"\n asset inputs:file = @old.png@\n}\n';
  const rootText = bytes => {
    const entries = parseUSDZEntries(bytes);
    assert.equal(reader.loadAsLayerFromBinary(entries[0].data, entries[0].name), true);
    return {entries, text: reader.layerToString()};
  };
  try {
    assert.equal(loader.exportAsUSDZ(), null);
    assert.equal(loader.exportAsUSDZWithRemap(null), null);
    assert.equal(loader.exportLayerAsUSDZWithOptions({optimizeMaterials: 'bad'}), null);
    assert.equal(loader.error(), 'No layer loaded');
    assert.equal(loader.remapLayerAssetPaths(null), -1);
    assert.equal(loader.loadAsLayerFromBinary(source, 'package.usda'), true);
    loader.setAsset('old.png', new Uint8Array([1, 2, 3]));
    loader.setAsset('new.jpg', new Uint8Array([4, 5]));
    loader.setAsset('ignored.bin', new Uint8Array([6]));
    loader.setAsset('dependency.usda', '#usda 1.0\ndef Xform "Dependency" {}\n');
    assert.throws(() => loader.exportAsUSDZWithRemap(null), TypeError);
    assert.throws(() => loader.remapLayerAssetPaths(null), TypeError);
    for (const [name, args, root, remapped, includesLayers] of [
      ['exportAsUSDZ', [], '.usdc', false, false],
      ['exportAsUSDZWithRemap', [{'old.png': 'new.jpg'}], '.usdc', true, false],
      ['exportAsUSDZWithOptions', [{'old.png': 'new.jpg'}, {rootLayerFormat: 'USDA'}], '.usda', true, false],
      ['exportAsUSDZWithOptions', [null, {rootLayerFormat: 'unknown'}], '.usdc', false, false],
      ['exportLayerAsUSDZWithOptions', [null], '.usda', false, true],
      ['exportLayerAsUSDZWithOptions', [{rootLayerFormat: 'USDC'}], '.usdc', false, true],
    ]) {
      const view = loader[name](...args);
      assert.ok(view instanceof Uint8Array);
      assert.equal(view.buffer, native.HEAPU8.buffer, 'USDZ remains a borrowed view');
      const {entries, text} = rootText(view.slice());
      assert.ok(entries[0].name.endsWith(root));
      assert.ok(text.includes(remapped ? '@new.jpg@' : '@old.png@'));
      assert.equal(entries.some(e => e.name === 'dependency.usda'), includesLayers);
      assert.equal(entries.some(e => e.name === 'ignored.bin'), false);
      assert.deepEqual([...entries.find(e => e.name === 'new.jpg').data], [4, 5]);
      assert.ok(loader.layerToString().includes('@old.png@'));
    }
    const arkit = rootText(loader.exportAsUSDZWithOptions(null,
      {rootLayerFormat: 'usda', arkitCompatible: true}).slice());
    assert.ok(arkit.entries[0].name.endsWith('.usdc'));
    assert.ok(arkit.text.includes('upAxis = "Y"'));
    for (const key of ['optimizeMaterials', 'materialOptimization']) {
      assert.equal(loader.exportAsUSDZWithOptions(null, {[key]: 'invalid'}), null);
      assert.equal(loader.error(), 'Invalid material optimization mode: invalid');
    }
    for (const key of ['optimizeGeometry', 'optimizeMeshes', 'geometryOptimization']) {
      assert.equal(loader.exportAsUSDZWithOptions(null, {[key]: 'invalid'}), null);
      assert.equal(loader.error(), 'Invalid geometry optimization mode: invalid');
    }
    assert.ok(loader.exportAsUSDZWithOptions(null, {optimizeMaterials: 'OFF', optimizeGeometry: 'none'}));
    assert.equal(loader.remapLayerAssetPaths({'old.png': new TextEncoder().encode('renamed.png')}), 1);
    assert.ok(loader.layerToString().includes('@renamed.png@'));
    assert.equal(loader.remapLayerAssetPaths({'old.png': 'other.png'}), 0);
    assert.equal(loader.flattenLayer(), true);
    assert.equal(parseUSDZEntries(loader.exportLayerAsUSDZWithOptions({}).slice())
      .some(e => e.name === 'dependency.usda'), false);
  } finally { loader.delete(); reader.delete(); }
});

await testAsync('USDZ option and remap getters retain preparation order', () => {
  const loader = new native.LightUSDLoaderNative();
  const reader = new native.LightUSDLoaderNative();
  const original = '#usda 1.0\ndef Xform "Original" {}\n';
  const replacement = '#usda 1.0\ndef Xform "Replacement" {}\n';
  const readRoot = bytes => {
    const entry = parseUSDZEntries(bytes)[0];
    assert.equal(reader.loadAsLayerFromBinary(entry.data, entry.name), true);
    return reader.layerToString();
  };
  try {
    assert.equal(loader.loadAsLayerFromBinary(original, 'original.usda'), true);
    const remap = {get missing() {
      assert.equal(loader.loadAsLayerFromBinary(replacement, 'replacement.usda'), true);
      return 'other';
    }};
    assert.ok(readRoot(loader.exportAsUSDZWithRemap(remap).slice()).includes('Original'));
    assert.equal(loader.loadAsLayerFromBinary(original, 'original.usda'), true);
    const options = {get rootLayerFormat() {
      assert.equal(loader.loadAsLayerFromBinary(replacement, 'replacement.usda'), true);
      return 'usda';
    }};
    assert.ok(readRoot(loader.exportAsUSDZWithOptions(null, options).slice()).includes('Original'));
    assert.equal(loader.loadAsLayerFromBinary(original, 'original.usda'), true);
    assert.ok(readRoot(loader.exportLayerAsUSDZWithOptions(options).slice()).includes('Replacement'));
    const unread = () => { throw new Error('disabled optimization must not read size'); };
    assert.ok(loader.exportAsUSDZWithOptions(null, {optimizeMaterials: 'off',
      get materialAtlasSize() { return unread(); }, optimizeGeometry: 'none',
      get meshMergeMinGroupSize() { return unread(); }}));
    for (const mode of ['dedupe', 'DEDUP', 'preview', 'previewsurface', 'usdpreviewsurface', 'atlas']) {
      assert.equal(loader.loadAsLayerFromBinary(original, 'optimization.usda'), true);
      assert.ok(loader.exportAsUSDZWithOptions(null, {optimizeMaterials: mode,
        materialAtlasSize: 64, materialAtlasTileSize: 16, materialAtlasPadding: 1,
        materialAtlasMinGroupSize: 2}));
    }
    for (const mode of ['merge', 'mergemeshes', 'meshmerge']) {
      assert.equal(loader.loadAsLayerFromBinary(original, 'optimization.usda'), true);
      assert.ok(loader.exportAsUSDZWithOptions(null, {optimizeGeometry: mode,
        meshMergeMaxInputFaces: 32, meshMergeMaxInputPoints: 64,
        meshMergeMaxAggregateFaces: 128, meshMergeMinGroupSize: 2}));
    }
    for (const value of [true, 1.5, NaN, -1, null]) {
      assert.ok(loader.exportAsUSDZWithOptions(null, {optimizeMaterials: 'dedupe', materialAtlasSize: value}));
    }
    for (const value of ['64', 64n, Infinity, {}, 2147483648]) {
      assert.throws(() => loader.exportAsUSDZWithOptions(null,
        {optimizeMaterials: 'dedupe', materialAtlasSize: value}), TypeError);
    }
    const failure = new TypeError('option getter failed');
    assert.throws(() => loader.exportAsUSDZWithOptions(null, {get rootLayerFormat() { throw failure; }}),
      error => error === failure);
    assert.ok(loader.exportAsUSDZ());
  } finally { loader.delete(); reader.delete(); }
});

await testAsync('package and buffer C exports validate records and clean up thrown JS copies', () => {
  const loader = new native.LightUSDLoaderNative();
  const other = new native.LightUSDLoaderNative();
  const ptr = value => wasm64 ? BigInt(value) : Number(value);
  const handle = ptr(loader.$$.ptr);
  const out = native._lightusd_combined_alloc(24);
  const options = native._lightusd_combined_alloc(24);
  const bad = native._lightusd_combined_alloc(4);
  const savedRelease = native._lightusd_combined_export_release;
  const savedEnd = native._lightusd_combined_package_end;
  let task = 0, releases = 0, ends = 0;
  try {
    assert.equal(native._lightusd_combined_export_layer_ready(ptr(0)), -1);
    assert.equal(native._lightusd_combined_export_buffer_finish(ptr(0), ptr(0)), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 23, true);
    assert.equal(native._lightusd_combined_export_usdc_buffer(handle, 0, 0, 4096, out), ptr(0));
    assert.equal(native._lightusd_combined_package_begin(handle, 2), ptr(0));
    assert.equal(native._lightusd_combined_package_begin(ptr(0), 0), ptr(0));
    new DataView(native.HEAPU8.buffer).setUint32(Number(options), 23, true);
    assert.equal(native._lightusd_combined_export_optimize(handle, 0, ptr(0), 0, options), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(options), 24, true);
    new DataView(native.HEAPU8.buffer).setUint32(Number(options) + 4, 16, true);
    assert.equal(native._lightusd_combined_export_optimize(handle, 0, ptr(0), 0, options), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(options) + 4, 0, true);
    assert.equal(native._lightusd_combined_export_optimize(handle, 2, ptr(0), 0, options), -1);
    assert.equal(native._lightusd_combined_export_optimize(handle, 0, ptr(0), 1, options), -1);
    assert.equal(native._lightusd_combined_export_optimize(handle, 0, ptr(0), 0, options), 1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(bad), 1, true);
    assert.equal(native._lightusd_combined_export_remap(handle, bad, 4), -2);
    assert.equal(native._lightusd_combined_export_remap(ptr(0), ptr(0), 0), -2);
    assert.equal(loader.loadAsLayerFromBinary('#usda 1.0\ndef Xform "World" {}\n', 'safe.usda'), true);
    task = native._lightusd_combined_package_begin(handle, 0);
    assert.ok(task);
    const write = (receiver, state, record) => native._lightusd_combined_package_write(
      receiver, state, ptr(0), 0, ptr(0), 0, 0, record);
    assert.equal(write(handle, task, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 24, true);
    assert.equal(write(ptr(other.$$.ptr), task, out), -1);
    assert.equal(write(handle, ptr(0), out), -1);
    assert.equal(native._lightusd_combined_package_write(handle, task, bad, 4, ptr(0), 0, 0, out), -1);
    assert.equal(write(handle, task, out), 1);
    const info = new DataView(native.HEAPU8.buffer, Number(out), 24);
    assert.ok(parseUSDZEntries(native.HEAPU8.slice(info.getFloat64(16, true),
      info.getFloat64(16, true) + info.getFloat64(8, true))).length > 0);
    native._lightusd_combined_export_release = value => { ++releases; savedRelease(value); };
    native._lightusd_combined_package_end = value => { ++ends; savedEnd(value); };
    const failure = new TypeError('deliberate output failure');
    assert.throws(() => loader.exportStageAsUSDCToBufferWithOptions(
      {byteLength: 1048576, set() { throw failure; }}, {}), error => error === failure);
    assert.equal(releases, 1);
    assert.throws(() => loader.exportAsUSDZWithOptions(null,
      {get rootLayerFormat() { throw failure; }}), error => error === failure);
    assert.equal(ends, 1);
    assert.throws(() => loader.exportAsUSDZWithRemap(), TypeError);
    assert.throws(() => loader.exportLayerAsUSDZWithOptions(), TypeError);
    assert.throws(() => loader.remapLayerAssetPaths({'old': 123}), TypeError);
    assert.ok(loader.exportAsUSDZ());
  } finally {
    native._lightusd_combined_export_release = savedRelease;
    native._lightusd_combined_package_end = savedEnd;
    if (task) savedEnd(task);
    savedEnd(ptr(0));
    native._lightusd_combined_free(bad);
    native._lightusd_combined_free(options);
    native._lightusd_combined_free(out);
    loader.delete(); other.delete();
  }
});

await testAsync('USDZ optimization options deduplicate materials and obey mesh merge limits', () => {
  const loader = new native.LightUSDLoaderNative();
  const count = (text, kind) => (text.match(new RegExp(`def ${kind} `, 'g')) || []).length;
  try {
    for (const method of ['exportAsUSDZWithOptions', 'exportLayerAsUSDZWithOptions']) {
      for (const [settings, meshes, materials] of [
        [{}, 2, 2],
        [{optimizeMaterials: 'dedupe'}, 2, 2],
        [{materialOptimization: 'preview'}, 2, 1],
        [{optimizeMaterials: 'atlas', materialAtlasSize: 64, materialAtlasTileSize: 16}, 2, 1],
        [{optimizeMaterials: 'preview', optimizeGeometry: 'merge'}, 3, 1],
        [{optimizeMaterials: 'preview', optimizeMeshes: 'merge', meshMergeMaxInputFaces: 0}, 2, 1],
        [{optimizeMaterials: 'preview', geometryOptimization: 'merge', meshMergeMinGroupSize: 3}, 2, 1],
        [{optimizeMaterials: 'preview', optimizeGeometry: 'merge', meshMergeMaxAggregateFaces: 1}, 2, 1],
      ]) {
        assert.equal(loader.loadAsLayerFromBinary(TEXTURED_TWO_MATERIAL_USDA, 'opt.usda'), true);
        const options = {...settings, rootLayerFormat: 'usda'};
        const view = method === 'exportAsUSDZWithOptions'
          ? loader[method](null, options) : loader[method](options);
        assert.ok(view, loader.error());
        const text = new TextDecoder().decode(parseUSDZEntries(view.slice())[0].data);
        assert.equal(count(text, 'Mesh'), meshes, JSON.stringify(settings));
        if (meshes === 3) assert.equal((text.match(/active = false/g) || []).length, 2);
        assert.equal(count(text, 'Material'), materials, JSON.stringify(settings));
        assert.equal(count(loader.layerToString(), 'Mesh'), meshes);
        assert.equal(count(loader.layerToString(), 'Material'), materials);
      }
    }
  } finally { loader.delete(); }
});

await testAsync('render scalar, metadata, camera and texture queries preserve payloads', () => {
  const loader = new native.LightUSDLoaderNative();
  const counts = ['numMeshes', 'numInstances', 'numMaterials', 'numTextures', 'numImages',
    'numLights', 'numCameras', 'numUDIMTextures', 'numRootNodes', 'numAnimations', 'numSkeletons'];
  const cameraSource = `#usda 1.0
(upAxis = "Z" metersPerUnit = 1 kilogramsPerUnit = 2 startTimeCode = 2 endTimeCode = 8 framesPerSecond = 30 timeCodesPerSecond = 60 autoPlay = false comment = "metadata comment")
def Camera "Camera" {
 float focalLength = 50
 float horizontalAperture = 36
 float verticalAperture = 24
 float2 clippingRange = (0.5, 500)
 token projection = "orthographic"
}
`;
  try {
    for (const name of counts) assert.equal(loader[name](), 0, name);
    assert.equal(loader.getURI(), '');
    assert.equal(loader.getUpAxis(), 'Y');
    assert.deepEqual(loader.getSceneMetadata(), {});
    assert.deepEqual(loader.getCamera(0), {error: 'Scene not loaded'});
    assert.deepEqual(loader.getTexture(0), {});
    for (const suffix of ['MaterialDedup', 'MeshMerge', 'MeshMergeBakeTransform', 'FlattenRenderTree']) {
      loader[`setNative${suffix}`](true); assert.equal(loader[`getNative${suffix}`](), true);
      loader[`setNative${suffix}`](false); assert.equal(loader[`getNative${suffix}`](), false);
    }
    assert.equal(loader.loadAsLayerFromBinary(cameraSource, 'camera.usda'), true);
    assert.equal(loader.layerToRenderScene(), true);
    assert.equal(loader.getURI(), 'camera.usda');
    assert.equal(loader.getUpAxis(), 'Z');
    assert.equal(loader.numCameras(), 1);
    const camera = loader.getCamera(0);
    assert.equal(camera.projection, 'orthographic');
    assert.equal(camera.focalLength, 50);
    assert.equal(camera.verticalAperture, 24);
    assert.equal(camera.horizontalAperture, 36);
    assert.equal(camera.znear, 0.5); assert.equal(camera.zfar, 500);
    assert.equal(camera.aspectRatio, 1.5);
    assert.ok(Math.abs(camera.yfov - 2 * Math.atan(0.24)) < 1e-6);
    assert.ok(Math.abs(camera.xfov - 2 * Math.atan(0.36)) < 1e-6);
    assert.deepEqual(Object.keys(camera).sort(), ['name', 'absPath', 'displayName', 'focalLength',
      'verticalAperture', 'horizontalAperture', 'znear', 'zfar', 'yfov', 'xfov', 'aspectRatio', 'projection'].sort());
    assert.deepEqual(loader.getCamera(-1), {error: 'Invalid camera ID'});
    assert.deepEqual(loader.getCamera(1), {error: 'Invalid camera ID'});
    assert.deepEqual(loader.getCamera(NaN), camera);
    const meta = loader.getSceneMetadata();
    assert.equal(meta.upAxis, 'Z'); assert.equal(meta.metersPerUnit, 1);
    assert.equal(meta.kilogramsPerUnit, 2);
    assert.equal(meta.startTimeCode, 2); assert.equal(meta.endTimeCode, 8);
    assert.equal(meta.framesPerSecond, 30); assert.equal(meta.timeCodesPerSecond, 60);
    assert.equal(meta.comment, 'metadata comment');
    assert.equal(meta.autoPlay, false);
    assert.equal(meta.workingToDisplayLinear.length, 9);
    const nextMetadataStream = new nextRuntime.RenderStream();
    try {
      const loaded = nextMetadataStream.begin(new TextEncoder().encode(cameraSource), 'camera.usda');
      assert.equal(loaded.success, true, loaded.error || nextMetadataStream.error());
      assert.deepEqual(nextMetadataStream.getSceneMetadata(), meta,
        'next scene metadata preserves the legacy record and authored comment');
    } finally { nextMetadataStream.delete(); }
    const nextMetadataLayer = new nextRuntime.LayerDocument();
    try {
      const loaded = nextMetadataLayer.load(new TextEncoder().encode(cameraSource));
      assert.equal(loaded.success, true, loaded.error || nextMetadataLayer.error());
      const exported = nextMetadataLayer.exportUSDA();
      assert.equal(exported.success, true, exported.error);
      assert.match(exported.text, /autoPlay = false/,
        'next USDA export retains the authored autoPlay value');
      const crate = nextMetadataLayer.exportUSDC();
      assert.equal(crate.success, true, crate.error);
      const crateReload = new nextRuntime.LayerDocument();
      try {
        const reloaded = crateReload.load(crate.data);
        assert.equal(reloaded.success, true, reloaded.error || crateReload.error());
        const crateRoundtrip = crateReload.exportUSDA();
        assert.equal(crateRoundtrip.success, true, crateRoundtrip.error);
        assert.match(crateRoundtrip.text, /autoPlay = false/,
          'next USDC writer and reader retain authored autoPlay');
      } finally { crateReload.delete(); }
    } finally { nextMetadataLayer.delete(); }
    const savedMatrix = meta.workingToDisplayLinear.slice();
    meta.workingToDisplayLinear[0] = 123;
    assert.deepEqual(loader.getSceneMetadata().workingToDisplayLinear, savedMatrix);
    assert.equal(typeof loader.getDefaultRootNodeId(), 'number');
    assert.equal(loader.loadFromBinary(TEXTURED_TWO_MATERIAL_USDA, 'textures.usda'), true);
    assert.equal(loader.numMeshes(), 2); assert.equal(loader.numMaterials(), 2);
    assert.ok(loader.numTextures() > 0);
    const texture = loader.getTexture(0);
    assert.deepEqual(Object.keys(texture).sort(), ['textureImageId', 'wrapS', 'wrapT', 'hasTransform2d',
      'txRotation', 'txScaleU', 'txScaleV', 'txTranslationU', 'txTranslationV', 'bias', 'scale', 'isUDIM'].sort());
    assert.deepEqual(texture.bias, [0, 0, 0, 0]); assert.deepEqual(texture.scale, [1, 1, 1, 1]);
    assert.equal(texture.isUDIM, false);
    assert.equal(texture.hasTransform2d, false);
    assert.deepEqual(loader.getTexture(-1), {});
    assert.deepEqual(loader.getTexture(loader.numTextures()), {});
    const saved = loader.getTexture(0);
    texture.bias[0] = 123;
    assert.deepEqual(loader.getTexture(0), saved);
    const noTimes = loader.getSceneMetadata();
    assert.equal(noTimes.startTimeCode, null); assert.equal(noTimes.endTimeCode, null);
    loader.reset();
    for (const name of counts) assert.equal(loader[name](), 0, name);
    assert.deepEqual(loader.getSceneMetadata(), {});
  } finally { loader.delete(); }
});

await testAsync('render metadata reads copyright strings from customLayerData', () => {
  const source = [
    '#usda 1.0',
    '(metersPerUnit = 1 customLayerData = {',
    ' string copyright = "metadata copyright"',
    '})',
    'def Xform "World" {}',
    ''
  ].join('\n');
  const bytes = new TextEncoder().encode(source);
  const legacy = new native.LightUSDLoaderNative();
  const next = new nextRuntime.RenderStream();
  try {
    assert.equal(legacy.loadAsLayerFromBinary(source, 'copyright.usda'), true,
      legacy.error());
    assert.equal(legacy.layerToRenderScene(), true, legacy.error());
    const legacyMetadata = legacy.getSceneMetadata();
    assert.equal(legacyMetadata.copyright, 'metadata copyright');
    const loaded = next.begin(bytes, 'copyright.usda');
    assert.equal(loaded.success, true, loaded.error || next.error());
    assert.deepEqual(next.getSceneMetadata(), legacyMetadata);
  } finally {
    legacy.delete();
    next.delete();
  }
});

await testAsync('unauthored render units retain the legacy meter fallback', () => {
  const source = '#usda 1.0\ndef Xform "World" {}\n';
  const legacy = new native.LightUSDLoaderNative();
  const next = new nextRuntime.RenderStream();
  try {
    assert.equal(legacy.loadAsLayerFromBinary(source, 'units.usda'), true,
      legacy.error());
    assert.equal(legacy.layerToRenderScene(), true, legacy.error());
    const expected = legacy.getSceneMetadata();
    assert.equal(expected.metersPerUnit, 1);
    const loaded = next.begin(new TextEncoder().encode(source), 'units.usda');
    assert.equal(loaded.success, true, loaded.error || next.error());
    assert.deepEqual(next.getSceneMetadata(), expected);
  } finally {
    legacy.delete();
    next.delete();
  }
});

await testAsync('texture records preserve transforms, color adjustments and UDIM linkage', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    const png = loader.encodeImageNative(new Uint8Array([255, 128, 64, 255]), 1, 1, 4, 'png').slice();
    loader.setAsset('surface.1001.png', png);
    loader.setAsset('surface.1012.png', png);
    loader.setLoadTextureInNative(true);
    loader.setCombineUDIMTiles(false);
    let source = TEXTURED_TWO_MATERIAL_USDA.replaceAll('@Texture.png@', '@surface.<UDIM>.png@');
    for (const name of ['Mat_A', 'Mat_B']) {
      source = source.replace(`def Material "${name}"\n    {`, `def Material "${name}"\n    {
        def Shader "Reader" {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "st"
            float2 outputs:result
        }
        def Shader "Transform" {
            float2 inputs:in.connect = </World/${name}/Reader.outputs:result>
            uniform token info:id = "UsdTransform2d"
            float inputs:rotation = 90
            float2 inputs:scale = (2, 3)
            float2 inputs:translation = (0.25, 0.5)
            float2 outputs:result
        }`);
      const start = source.indexOf(`def Material "${name}"`);
      source = source.slice(0, start) + source.slice(start).replace(`asset inputs:file = @surface.<UDIM>.png@`,
        `asset inputs:file = @surface.<UDIM>.png@
            float2 inputs:st.connect = </World/${name}/Transform.outputs:result>
            float4 inputs:bias = (0.1, 0.2, 0.3, 0.4)
            float4 inputs:scale = (2, 3, 4, 5)
            token inputs:wrapS = "repeat"
            token inputs:wrapT = "clamp"`);
    }
    assert.equal(loader.loadAsLayerFromBinary(source, 'udim.usda'), true);
    const archive = loader.exportLayerAsUSDZWithOptions({rootLayerFormat: 'usda'}).slice();
    assert.equal(loader.loadFromBinary(source, 'cached-udim.usda'), true, loader.error());
    assert.ok(loader.numUDIMTextures() > 0);
    const texture = loader.getTexture(0);
    assert.equal(texture.hasTransform2d, true);
    assert.equal(texture.txRotation, 90);
    assert.equal(texture.txScaleU, 2); assert.equal(texture.txScaleV, 3);
    assert.equal(texture.txTranslationU, 0.25); assert.equal(texture.txTranslationV, 0.5);
    assert.deepEqual(texture.bias, [0.1, 0.2, 0.3, 0.4].map(Math.fround));
    assert.deepEqual(texture.scale, [2, 3, 4, 5]);
    assert.equal(texture.isUDIM, true);
    assert.ok(texture.udimTextureId >= 0);
    assert.deepEqual([texture.udimUvScaleU, texture.udimUvScaleV, texture.udimUvOffsetU, texture.udimUvOffsetV], [1, 1, 0, 0]);
    const udim = loader.getUDIMTexture(texture.udimTextureId);
    assert.equal(udim.tiles.length, 2);
    assert.deepEqual(Object.keys(udim).sort(), ['primName', 'absPath', 'displayName', 'assetIdentifier', 'tiles'].sort());
    assert.equal(udim.assetIdentifier, 'surface.<UDIM>.png');
    assert.deepEqual(udim.tiles.map(t => [t.udim, t.u, t.v]), [[1012, 1, 1], [1001, 0, 0]]);
    assert.ok(udim.tiles.every(t => Number.isInteger(t.imageId) && t.imageId >= 0));
    assert.deepEqual(loader.extractUnresolvedTexturePaths(), []);
    udim.tiles[0].imageId = -99;
    assert.notEqual(loader.getUDIMTexture(texture.udimTextureId).tiles[0].imageId, -99);
    assert.deepEqual(loader.getUDIMTexture(-1), {});
    assert.deepEqual(loader.getUDIMTexture(loader.numUDIMTextures()), {});
    loader.setCombineUDIMTiles(true);
    assert.equal(loader.loadFromBinary(archive, 'tiles.usdz'), true, loader.error());
    const atlas = loader.getTexture(0);
    assert.equal(atlas.isUDIM, true);
    assert.equal(atlas.udimTextureId, -1);
    assert.equal(loader.numUDIMTextures(), 0);
    assert.equal(atlas.udimUvScaleU, 0.5); assert.equal(atlas.udimUvScaleV, 0.5);
    assert.ok(atlas.udimUvOffsetU === 0 && atlas.udimUvOffsetV === 0);
  } finally { loader.delete(); }
});

await testAsync('render C queries validate records and reject stale JS receivers', () => {
  const loader = new native.LightUSDLoaderNative();
  const ptr = value => wasm64 ? BigInt(value) : Number(value);
  const handle = ptr(loader.$$.ptr), out = native._lightusd_combined_alloc(152);
  try {
    assert.equal(native._lightusd_combined_render_scalar(ptr(0), 0, out), -1);
    assert.equal(native._lightusd_combined_render_scalar(handle, 999, out), -1);
    assert.equal(native._lightusd_combined_render_scalar(handle, 0, ptr(0)), -1);
    for (const [name, size, indexed] of [['_lightusd_combined_camera_get', 72, true],
      ['_lightusd_combined_metadata_get', 128, false], ['_lightusd_combined_texture_get', 152, true]]) {
      new DataView(native.HEAPU8.buffer).setUint32(Number(out), size - 1, true);
      const invoke = (receiver, record) => indexed ? native[name](receiver, 0, record) : native[name](receiver, record);
      assert.equal(invoke(handle, out), -1);
      assert.equal(invoke(handle, ptr(0)), -1);
      assert.equal(invoke(ptr(0), out), -1);
      new DataView(native.HEAPU8.buffer).setUint32(Number(out), size, true);
      assert.equal(invoke(handle, out), 0);
    }
    for (const name of ['getCamera', 'getTexture']) {
      for (const value of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader[name](value), TypeError);
      assert.throws(() => loader[name](), TypeError);
    }
    assert.throws(() => loader.getSceneMetadata(0), TypeError);
    assert.throws(() => loader.numMeshes(0), TypeError);
    assert.throws(() => loader.getURI.call({}), TypeError);
    loader.delete();
    for (const name of ['numMeshes', 'getSceneMetadata', 'getTexture', 'getCamera', 'getNativeMaterialDedup'])
      assert.throws(() => loader[name](0), TypeError);
  } finally {
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('render inspection JSON preserves authored values and unloaded errors', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.equal(loader.getMhProfileJSON(), '[]');
    assert.equal(loader.getShadingGraphJSON(), '');
    assert.equal(loader.error(), 'Shading graph inspection requires a loaded Layer');
    assert.equal(loader.loadAsLayerFromBinary(`#usda 1.0
 def Material "Mat" {
   custom string mh:label = "顔"
   custom float mh:weight = 0.5
   custom rel mh:target = </Mat/Shader>
   token outputs:surface.connect = </Mat/Shader.outputs:surface>
   def Shader "Shader" {
     uniform token info:id = "UsdPreviewSurface"
     color3f inputs:diffuseColor = (0.25, 0.5, 1)
     asset inputs:file = @textures/顔.png@
     token outputs:surface
   }
 }
`, 'inspection.usda'), true, loader.error());
    const profile = JSON.parse(loader.getMhProfileJSON());
    assert.equal(profile.length, 1);
    assert.equal(profile[0].path, '/Mat');
    assert.equal(profile[0].attrs['mh:label'], '顔');
    assert.equal(profile[0].attrs['mh:weight'], 0.5);
    assert.deepEqual(profile[0].rels['mh:target'], ['/Mat/Shader']);
    const graphText = loader.getShadingGraphJSON();
    const graph = JSON.parse(graphText);
    assert.equal(graph.version, 1);
    assert.deepEqual(graph.prims.map(p => p.path), ['/Mat', '/Mat/Shader']);
    assert.deepEqual(graph.prims[0].properties['outputs:surface'].connections, ['/Mat/Shader.outputs:surface']);
    assert.deepEqual(graph.prims[1].properties['inputs:diffuseColor'].value, [0.25, 0.5, 1]);
    assert.equal(graph.assetPaths[0].authored, 'textures/顔.png');
    loader.reset();
    assert.equal(loader.getMhProfileJSON(), '[]');
    assert.equal(loader.getShadingGraphJSON(), '');
    assert.equal(JSON.parse(graphText).prims.length, 2);
  } finally { loader.delete(); }
});

await testAsync('inspection JSON dispatch validates receivers and never replays TypeErrors', () => {
  const loader = new native.LightUSDLoaderNative();
  const key = '_lightusd_combined_render_json', original = native[key];
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const calls = [];
  native[key] = (...args) => { calls.push(args[1]); return original(...args); };
  try {
    assert.equal(loader.getMhProfileJSON(), '[]');
    assert.equal(loader.getShadingGraphJSON(), '');
    assert.deepEqual(calls, [0, 1]);
    assert.equal(original(pointer(0), 0), -1);
    assert.equal(original(pointer(loader.$$.ptr), 2), -1);
    for (const name of ['getMhProfileJSON', 'getShadingGraphJSON']) {
      assert.throws(() => loader[name](0), TypeError);
      assert.throws(() => loader[name].call({}), TypeError);
    }
    const failure = new TypeError('inspection dispatch failure');
    let count = 0;
    native[key] = () => { ++count; throw failure; };
    assert.throws(() => loader.getMhProfileJSON(), error => error === failure);
    assert.equal(count, 1);
    native[key] = original;
    assert.equal(loader.getMhProfileJSON(), '[]');
    loader.delete();
    assert.throws(() => loader.getMhProfileJSON(), TypeError);
    assert.throws(() => loader.getShadingGraphJSON(), TypeError);
  } finally {
    native[key] = original;
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('instance queries preserve ordered mesh matches and independent matrices', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.equal(loader.getInstance(0), null);
    assert.deepEqual(loader.getInstancesForMesh(-1), []);
    const source = `#usda 1.0
 def Xform "World" {
   double3 xformOp:translate = (10, 20, 30)
   uniform token[] xformOpOrder = ["xformOp:translate"]
   def Mesh "Prototype" {
     point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
     int[] faceVertexCounts = [3]
     int[] faceVertexIndices = [0, 1, 2]
   }
   def PointInstancer "Copies" {
     rel prototypes = [</World/Prototype>]
     int[] protoIndices = [0, 0]
     point3f[] positions = [(1, 2, 3), (4, 5, 6)]
     float3[] scales = [(2, 3, 4), (1, 1, 1)]
   }
 }
`;
    assert.equal(loader.loadFromBinary(source, 'instances.usda'), true, loader.error());
    assert.equal(loader.numInstances(), 2);
    const first = loader.getInstance(0), second = loader.getInstance(1);
    assert.deepEqual(Object.keys(first).sort(), ['primName', 'absPath', 'displayName',
      'prototypeIndex', 'meshId', 'materialId', 'localMatrix', 'globalMatrix', 'visible'].sort());
    assert.equal(first.primName, 'Copies[0]');
    assert.equal(first.absPath, '/World/Copies/instance_0');
    assert.equal(first.displayName, '');
    assert.equal(first.prototypeIndex, 0);
    assert.equal(first.materialId, -1);
    assert.equal(first.visible, true);
    assert.deepEqual(first.localMatrix, [2,0,0,0, 0,3,0,0, 0,0,4,0, 1,2,3,1]);
    assert.deepEqual(first.globalMatrix, [2,0,0,0, 0,3,0,0, 0,0,4,0, 11,22,33,1]);
    assert.deepEqual(second.globalMatrix.slice(12), [14,25,36,1]);
    assert.equal(second.meshId, first.meshId);
    assert.deepEqual(loader.getInstancesForMesh(first.meshId), [0, 1]);
    assert.deepEqual(loader.getInstancesForMesh(2147483647), []);
    assert.deepEqual(loader.getInstancesForMesh(-1), []);
    assert.equal(loader.getInstance(-1), null);
    assert.equal(loader.getInstance(2), null);
    first.localMatrix[0] = 99;
    assert.equal(loader.getInstance(0).localMatrix[0], 2);
    assert.equal(loader.getInstance(0.9).primName, first.primName);
    loader.reset();
    assert.equal(loader.getInstance(0), null);
    assert.deepEqual(loader.getInstancesForMesh(0), []);
    assert.equal(second.globalMatrix[12], 14);
    assert.equal(loader.loadFromBinary(`#usda 1.0
 def Xform "Proto" {}
 def Xform "First" (instanceable = true
 prepend references = </Proto>) {}
 def Xform "Second" (instanceable = true
 prepend references = </Proto>) {}
`, 'empty-instances.usda'), true, loader.error());
    assert.equal(loader.numInstances(), 2);
    assert.equal(loader.getInstance(0).meshId, -1);
    assert.equal(loader.getInstance(1).meshId, -1);
    assert.deepEqual(loader.getInstancesForMesh(-1), [0, 1]);
    assert.deepEqual(loader.getInstancesForMesh(0), []);

  } finally { loader.delete(); }
});

await testAsync('instance C records reject invalid calls and JS stale receivers', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), out = native._lightusd_combined_alloc(280);
  try {
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 279, true);
    assert.equal(native._lightusd_combined_instance_get(handle, 0, out), -1);
    assert.equal(native._lightusd_combined_instance_get(handle, 0, pointer(0)), -1);
    assert.equal(native._lightusd_combined_instance_get(pointer(0), 0, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 280, true);
    assert.equal(native._lightusd_combined_instance_get(handle, 0, out), 0);
    assert.equal(native._lightusd_combined_instances_for_mesh(pointer(0), 0), -1);
    assert.equal(native._lightusd_combined_instances_for_mesh(handle, -1), 0);
    const query = native._lightusd_combined_instances_for_mesh;
    try {
      native._lightusd_combined_instances_for_mesh = () => 0x40000000;
      assert.throws(() => loader.getInstancesForMesh(0), RangeError);
    } finally { native._lightusd_combined_instances_for_mesh = query; }

    for (const name of ['getInstance', 'getInstancesForMesh']) {
      for (const value of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader[name](value), TypeError);
      assert.throws(() => loader[name](), TypeError);
      assert.throws(() => loader[name](0, 1), TypeError);
      assert.throws(() => loader[name].call({}, 0), TypeError);
    }
    loader.delete();
    assert.throws(() => loader.getInstance(0), TypeError);
    assert.throws(() => loader.getInstancesForMesh(0), TypeError);
  } finally {
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('root queries preserve hierarchy order, reset transforms and owned values', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.deepEqual(loader.getDefaultRootNode(), {});
    assert.deepEqual(loader.getRootNode(-1), {});
    assert.deepEqual(loader.getRootNode(0), {});
    assert.equal(loader.loadFromBinary(`#usda 1.0
(defaultPrim = "World")
def Xform "World" (displayName = "世界") {
 double3 xformOp:translate = (10, 20, 30)
 uniform token[] xformOpOrder = ["xformOp:translate"]
 def Xform "Child" {
  double3 xformOp:translate = (1, 2, 3)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  def Xform "Reset" {
   double3 xformOp:translate = (4, 5, 6)
   uniform token[] xformOpOrder = ["!resetXformStack!", "xformOp:translate"]
  }
 }
 def Scope "Sibling" {}
}
def Xform "Other" {}
`, 'tree.usda'), true, loader.error());
    assert.equal(loader.numRootNodes(), 2);
    const root = loader.getDefaultRootNode();
    assert.deepEqual(root, loader.getRootNode(loader.getDefaultRootNodeId()));
    assert.deepEqual(root, loader.getRootNode(0.9));
    assert.equal(root.primName, 'World');
    assert.equal(root.displayName, '世界');
    assert.equal(root.absPath, '/World');
    assert.equal(root.nodeCategory, 'group');
    assert.equal(root.nodeType, 'xform');
    assert.equal(root.contentId, -1);
    assert.equal(root.isInstance, false);
    assert.equal(root.prototypeIndex, -1);
    assert.equal(root.instanceId, -1);
    assert.deepEqual(root.children.map(n => n.primName), ['Child', 'Sibling']);
    const child = root.children[0], reset = child.children[0];
    assert.deepEqual(child.localMatrix.slice(12), [1, 2, 3, 1]);
    assert.deepEqual(child.globalMatrix.slice(12), [11, 22, 33, 1]);
    assert.equal(reset.hasResetXform, true);
    assert.deepEqual(reset.globalMatrix.slice(12), [4, 5, 6, 1]);
    assert.deepEqual(reset.children, []);
    assert.equal(loader.getRootNode(1).primName, 'Other');
    assert.deepEqual(loader.getRootNode(2), {});
    child.localMatrix[12] = 999;
    root.children.pop();
    assert.equal(loader.getDefaultRootNode().children.length, 2);
    assert.equal(loader.getDefaultRootNode().children[0].localMatrix[12], 1);
    loader.reset();
    assert.deepEqual(loader.getDefaultRootNode(), {});
    assert.equal(reset.globalMatrix[12], 4);
  } finally { loader.delete(); }
});

await testAsync('hierarchy cursors validate records, close on exceptions and retain the receiver', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), out = native._lightusd_combined_alloc(288);
  let cursor = 0;
  const readCursor = () => {
    const view = new DataView(native.HEAPU8.buffer, Number(out), 8);
    return wasm64 ? view.getBigUint64(0, true) : view.getUint32(0, true);
  };
  const nextName = '_lightusd_combined_nodes_next', endName = '_lightusd_combined_nodes_end';
  const next = native[nextName], end = native[endName];
  try {
    assert.equal(native._lightusd_combined_nodes_begin(pointer(0), 0, 0, out), -1);
    assert.equal(native._lightusd_combined_nodes_begin(handle, 0, 2, out), -1);
    assert.equal(native._lightusd_combined_nodes_begin(handle, 0, 0, pointer(0)), -1);
    assert.equal(native._lightusd_combined_nodes_begin(handle, 0, 0, out), 0);
    assert.equal(Number(readCursor()), 0);
    assert.equal(next(pointer(0), out), -1);
    native[endName](pointer(0));
    assert.equal(loader.loadFromBinary('#usda 1.0\ndef Xform "Root" { def Xform "Child" {} }', 'cursor.usda'), true);
    assert.equal(native._lightusd_combined_nodes_begin(handle, 0, 0, out), 1);
    cursor = readCursor();
    assert.equal(next(cursor, pointer(0)), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 287, true);
    assert.equal(next(cursor, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 288, true);
    assert.equal(next(cursor, out), 1);
    assert.equal(new DataView(native.HEAPU8.buffer).getBigUint64(Number(out) + 24, true), 1n);
    assert.equal(next(cursor, out), 1);
    assert.equal(new DataView(native.HEAPU8.buffer).getBigUint64(Number(out) + 24, true), 0n);
    assert.equal(next(cursor, out), 0);
    assert.equal(next(cursor, out), 0);
    end(cursor); cursor = 0;
    for (const value of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader.getRootNode(value), TypeError);
    assert.throws(() => loader.getRootNode(), TypeError);
    assert.throws(() => loader.getDefaultRootNode(0), TypeError);
    assert.throws(() => loader.getRootNode.call({}, 0), TypeError);
    let ended = 0, calls = 0;
    const failure = new TypeError('hierarchy traversal failure');
    native[endName] = cursor => { ++ended; end(cursor); };
    native[nextName] = () => { ++calls; throw failure; };
    assert.throws(() => loader.getRootNode(0), error => error === failure);
    assert.equal(calls, 1);
    assert.equal(ended, 1);
    native[nextName] = (cursor, out) => {
      const status = next(cursor, out);
      if (!loader.isDeleted()) loader.delete();
      return status;
    };
    assert.equal(loader.getRootNode(0).children[0].primName, 'Child');
    assert.equal(ended, 2);
    assert.throws(() => loader.getRootNode(0), TypeError);
    assert.throws(() => loader.getDefaultRootNode(), TypeError);
  } finally {
    native[nextName] = next; native[endName] = end;
    if (cursor) end(cursor);
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('root records preserve resource categories and native instance flags', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.equal(loader.loadFromBinary(`#usda 1.0
 def Xform "Proto" {}
 def Xform "First" (instanceable = true
 prepend references = </Proto>) {}
 def Xform "Second" (instanceable = true
 prepend references = </Proto>) {}
 def Mesh "Triangle" {
 point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
 int[] faceVertexCounts = [3]
 int[] faceVertexIndices = [0,1,2]
 }
 def Camera "Cam" {}
 def DistantLight "Light" {}
`, 'flags.usda'), true, loader.error());
    const rows = Array.from({length: loader.numRootNodes()}, (_, i) => loader.getRootNode(i))
      .map(r => [r.primName, r.nodeCategory, r.nodeType, r.contentId,
        r.isInstance, r.prototypeIndex, r.instanceId]);
    assert.deepEqual(rows, [
      ['Proto', 'group', 'xform', -1, false, -1, -1],
      ['First', 'group', 'xform', -1, true, -1, -1],
      ['Second', 'group', 'xform', -1, true, -1, -1],
      ['Triangle', 'geom', 'mesh', 0, false, -1, -1],
      ['Cam', 'camera', 'camera', 0, false, -1, -1],
      ['Light', 'light', 'directionalLight', 0, false, -1, -1]
    ]);
  } finally { loader.delete(); }
});

await testAsync('unresolved texture paths retain image ordering and Unicode identifiers', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.deepEqual(loader.getUDIMTexture(0), {});
    assert.deepEqual(loader.extractUnresolvedTexturePaths(), []);
    const source = TEXTURED_TWO_MATERIAL_USDA.replace('@Texture.png@', '@textures/顔.png@')
      .replace('@Texture.png@', '@textures/other.png@');
    assert.equal(loader.loadFromBinary(source, 'unresolved.usda'), true, loader.error());
    const expected = Array.from({length: loader.numImages()}, (_, i) => loader.getImageCopy(i))
      .filter(image => image.bufferId === -1).map(image => image.uri);
    assert.ok(expected.length >= 2);
    assert.ok(expected.includes('textures/顔.png'));
    assert.ok(expected.includes('textures/other.png'));
    const paths = loader.extractUnresolvedTexturePaths();
    assert.deepEqual(paths, expected);
    const nextStream = new nextRuntime.RenderStream();
    try {
      nextStream.setLoadTextureInNative(false);
      const loaded = nextStream.begin(new TextEncoder().encode(source), 'unresolved.usda');
      assert.equal(loaded.success, true, loaded.error || nextStream.error());
      assert.deepEqual(nextStream.extractUnresolvedTexturePaths(), paths,
        'next preserves legacy unresolved URI order and duplicates');
    } finally { nextStream.delete(); }
    paths[0] = 'changed';
    assert.deepEqual(loader.extractUnresolvedTexturePaths(), expected);
    loader.reset();
    assert.deepEqual(loader.extractUnresolvedTexturePaths(), []);
    assert.deepEqual(loader.getUDIMTexture(0), {});
  } finally { loader.delete(); }
});

await testAsync('UDIM and unresolved-path C queries validate inputs and size limits', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), out = native._lightusd_combined_alloc(4);
  const key = '_lightusd_combined_udim_get', original = native[key];
  try {
    assert.equal(original(pointer(0), 0, out), -1);
    assert.equal(original(handle, 0, pointer(0)), -1);
    assert.equal(original(handle, 0, out), 0);
    assert.equal(new DataView(native.HEAPU8.buffer).getUint32(Number(out), true), 0);
    assert.equal(native._lightusd_combined_unresolved_textures(pointer(0)), -1);
    assert.equal(native._lightusd_combined_unresolved_textures(handle), 0);
    for (const value of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader.getUDIMTexture(value), TypeError);
    assert.throws(() => loader.getUDIMTexture(), TypeError);
    assert.throws(() => loader.getUDIMTexture(0, 1), TypeError);
    assert.throws(() => loader.extractUnresolvedTexturePaths(0), TypeError);
    assert.throws(() => loader.getUDIMTexture.call({}, 0), TypeError);
    assert.throws(() => loader.extractUnresolvedTexturePaths.call({}), TypeError);
    native[key] = (loader, id, out) => {
      new DataView(native.HEAPU8.buffer).setUint32(Number(out), 0x10000000, true);
      return 1;
    };
    assert.throws(() => loader.getUDIMTexture(0), RangeError);
    native[key] = original;
    assert.deepEqual(loader.getUDIMTexture(0), {});
    loader.delete();
    assert.throws(() => loader.getUDIMTexture(0), TypeError);
    assert.throws(() => loader.extractUnresolvedTexturePaths(), TypeError);
  } finally {
    native[key] = original;
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('image queries preserve metadata, borrowed bytes, copies and lazy archive reads', () => {
  const loader = new native.LightUSDLoaderNative();
  const warn = console.warn, warnings = [];
  console.warn = (...args) => warnings.push(args.join(' '));
  try {
    for (const name of ['getImage', 'getImagePtr', 'getImageCopy']) assert.deepEqual(loader[name](-1), {});
    const png = loader.encodeImageNative(new Uint8Array([255, 128, 64, 255]), 1, 1, 4, 'png').slice();
    loader.setAsset('Texture.png', png);
    assert.equal(loader.loadAsLayerFromBinary(TEXTURED_TWO_MATERIAL_USDA, 'images.usda'), true);
    const archive = loader.exportLayerAsUSDZWithOptions({rootLayerFormat: 'usda'}).slice();
    loader.setLoadTextureInNative(true);
    assert.equal(loader.loadFromBinary(TEXTURED_TWO_MATERIAL_USDA, 'decoded.usda'), true, loader.error());
    const id = loader.getTexture(0).textureImageId;
    const owned = loader.getImageCopy(id), ptr = loader.getImagePtr(id), borrowed = loader.getImage(id);
    const metadata = ({data, ptr, byteLength, ...rest}) => rest;
    assert.deepEqual(metadata(owned), metadata(ptr));
    assert.deepEqual(metadata(owned), metadata(borrowed));
    assert.equal(owned.width, 1); assert.equal(owned.height, 1); assert.equal(owned.channels, 4);
    assert.equal(owned.decoded, true);
    assert.equal(owned.uri, 'Texture.png');
    assert.equal(owned.sourceToDisplayLinear.length, 9);
    assert.ok(owned.data instanceof Uint8Array);
    assert.equal(ptr.byteLength, owned.data.length);
    assert.deepEqual(owned.data, borrowed.data);
    assert.equal(borrowed.data.buffer, native.HEAPU8.buffer);
    assert.equal(borrowed.data.byteOffset, ptr.ptr);
    assert.notEqual(owned.data.buffer, native.HEAPU8.buffer);
    const stringCopy = native._lightusd_combined_table_string;
    let growth = 0;
    const previousHeap = native.HEAPU8.buffer;
    try {
      native._lightusd_combined_table_string = (...args) => {
        const result = stringCopy(...args);
        if (!growth) growth = native._lightusd_combined_alloc(native.HEAPU8.byteLength + 65536);
        return result;
      };
      const afterGrowth = loader.getImageCopy(id);
      assert.ok(growth);
      assert.notEqual(native.HEAPU8.buffer, previousHeap);
      assert.deepEqual(afterGrowth.data, owned.data);
    } finally {
      native._lightusd_combined_table_string = stringCopy;
      if (growth) native._lightusd_combined_free(growth);
    }
    // Reacquire a borrowed view after growth detached the earlier one.
    borrowed.data = loader.getImage(id).data;
    const saved = owned.data[0];
    owned.data[0] ^= 255;
    assert.equal(loader.getImageCopy(id).data[0], saved);
    borrowed.data[0] ^= 127;
    assert.equal(loader.getImageCopy(id).data[0], saved ^ 127);
    borrowed.data[0] = saved;
    owned.sourceToDisplayLinear[0] = 999;
    assert.notEqual(loader.getImageCopy(id).sourceToDisplayLinear[0], 999);
    assert.deepEqual(loader.getImagePtr(loader.numImages()), {});
    loader.setLoadTextureInNative(false);
    assert.equal(loader.loadFromBinary(archive, 'lazy.usdz'), true, loader.error());
    const lazyId = loader.getTexture(0).textureImageId;
    const beforeLazyCopy = loader.getImagePtr(lazyId);
    assert.equal(beforeLazyCopy.bufferId, -1);
    assert.equal('ptr' in beforeLazyCopy, false);
    assert.equal('byteLength' in beforeLazyCopy, false);
    assert.deepEqual([beforeLazyCopy.width, beforeLazyCopy.height, beforeLazyCopy.channels], [-1, -1, -1]);
    assert.equal(beforeLazyCopy.sourceGamma, Math.fround(2.4));
    assert.equal(beforeLazyCopy.sourceLinearBias, Math.fround(0.055));
    const lazy = loader.getImageCopy(lazyId);
    assert.equal(lazy.decoded, false);
    assert.deepEqual(lazy.data, png);
    const live = loader.getImagePtr(lazyId);
    assert.equal(live.byteLength, png.length);
    assert.deepEqual(new Uint8Array(native.HEAPU8.buffer, live.ptr, live.byteLength), png);
    loader.reset();
    assert.deepEqual(lazy.data, png);
    assert.deepEqual(loader.getImage(0), {});
    assert.equal(warnings.filter(w => w.includes('getImage() is deprecated')).length, 1);
  } finally { console.warn = warn; loader.delete(); }
});

await testAsync('image C records and warning adapters preserve validation and one-shot dispatch', () => {
  const loader = new native.LightUSDLoaderNative();
  const clone = loader.clone();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), out = native._lightusd_combined_alloc(128);
  const key = '_lightusd_combined_image_get', original = native[key], warn = console.warn;
  try {
    assert.equal(original(pointer(0), 0, 0, out), -1);
    assert.equal(original(handle, 0, 2, out), -1);
    assert.equal(original(handle, 0, 0, pointer(0)), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 127, true);
    assert.equal(original(handle, 0, 1, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 128, true);
    assert.equal(original(handle, 0, 0, out), 0);
    assert.equal(native._lightusd_combined_image_warn(pointer(0)), -1);
    for (const name of ['getImage', 'getImagePtr', 'getImageCopy']) {
      for (const value of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader[name](value), TypeError);
      assert.throws(() => loader[name](), TypeError);
      assert.throws(() => loader[name](0, 1), TypeError);
      assert.throws(() => loader[name].call({}, 0), TypeError);
    }
    const failure = new TypeError('warning callback failed');
    let warnings = 0;
    console.warn = () => { ++warnings; throw failure; };
    assert.throws(() => loader.getImage(0), error => error === failure);
    assert.deepEqual(clone.getImage(0), {});
    assert.equal(warnings, 1);
    let calls = 0;
    native[key] = () => { ++calls; throw failure; };
    assert.throws(() => loader.getImageCopy(0), error => error === failure);
    assert.equal(calls, 1);
    native[key] = original;
    loader.delete();
    for (const name of ['getImage', 'getImagePtr', 'getImageCopy']) assert.throws(() => loader[name](0), TypeError);
    assert.deepEqual(clone.getImageCopy(0), {});
  } finally {
    native[key] = original; console.warn = warn;
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
    clone.delete();
  }
});

await testAsync('image records retain custom color transforms without pixel buffers', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    const source = TEXTURED_TWO_MATERIAL_USDA.replace('def Xform "World"\n{', `def Xform "World" (
      prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio_ap0"]
    ) {
      uniform token colorSpaceDefinition:studio_ap0:name = "studio_ap0"
      float2 colorSpaceDefinition:studio_ap0:redChroma = (0.7348552434, 0.2642253252)
      float2 colorSpaceDefinition:studio_ap0:greenChroma = (-0.0061709125, 1.0113149590)
      float2 colorSpaceDefinition:studio_ap0:blueChroma = (0.0159675593, -0.0642355031)
      float2 colorSpaceDefinition:studio_ap0:whitePoint = (0.3127, 0.3290)
      float colorSpaceDefinition:studio_ap0:gamma = 1
      float colorSpaceDefinition:studio_ap0:linearBias = 0
    `).replaceAll('@Texture.png@', '@missing-custom.png@ (colorSpace = "studio_ap0")');
    assert.equal(loader.loadFromBinary(source, 'custom-image.usda'), true, loader.error());
    const id = loader.getTexture(0).textureImageId;
    const image = loader.getImageCopy(id);
    assert.equal(image.sourceColorSpaceName, 'studio_ap0');
    assert.equal(image.colorTransformValid, true);
    assert.equal(image.colorTransformApplied, false);
    assert.equal(image.colorTransformBypass, false);
    assert.equal(image.sourceColorIsData, false);
    assert.equal(image.sourceGamma, 1);
    assert.equal(image.sourceLinearBias, 0);
    const expected = [2.521686, -1.134130, -0.387556, -0.276480, 1.372719,
      -0.096239, -0.015378, -0.152975, 1.168353];
    for (let i = 0; i < 9; ++i) assert.ok(Math.abs(image.sourceToDisplayLinear[i] - expected[i]) < 3e-4);
    assert.equal('data' in image, false);
    assert.equal('ptr' in loader.getImagePtr(id), false);
    assert.equal('byteLength' in loader.getImagePtr(id), false);
  } finally { loader.delete(); }
});

await testAsync('light queries retain complete baseline payloads and serialized formats', () => {
  // JSON normally loses signed zero; the baseline preserves it explicitly.
  const snapshots = JSON.parse(fs.readFileSync(new URL('./fixtures/combined-light-queries.json', import.meta.url), 'utf8'),
    (key, value) => value && value.$negativeZero === true ? -0 : value);
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.deepEqual(loader.getAllLights(), []);
    assert.deepEqual(loader.getLight(0), {error: 'Scene not loaded'});
    assert.deepEqual(loader.getLightWithFormat(-1, 'bad'), {error: 'Scene not loaded'});
    for (const [file, expected] of Object.entries(snapshots)) {
      const source = new Uint8Array(fs.readFileSync(new URL('../../../tests/usda/' + file, import.meta.url)));
      assert.equal(loader.loadFromBinary(source, file), true, loader.error());
      assert.deepEqual(loader.getAllLights(), expected.objects, file);
      for (let i = 0; i < expected.objects.length; ++i) {
        assert.deepEqual(loader.getLight(i), expected.objects[i]);
        for (const format of ['json', 'xml']) assert.deepEqual(loader.getLightWithFormat(i, format), expected.formats[i][format]);
      }
      assert.deepEqual(loader.getLight(0.9), expected.objects[0]);
      assert.deepEqual(loader.getLightWithFormat(0, new TextEncoder().encode('json')), expected.formats[0].json);
      assert.deepEqual(loader.getLight(-1), {error: 'Invalid light ID'});
      assert.deepEqual(loader.getLightWithFormat(-1, 'bad'), {error: 'Invalid light ID'});
      assert.deepEqual(loader.getLightWithFormat(0, 'json\0'), {error: "Unsupported format. Use 'json' or 'xml'"});
      const owned = loader.getLight(0);
      owned.color[0] = -999;
      owned.transform[0] = -999;
      if (owned.spectralEmission?.samples.length) owned.spectralEmission.samples[0][0] = -999;
      assert.deepEqual(loader.getLight(0), expected.objects[0]);
      loader.reset();
      assert.deepEqual(loader.getAllLights(), []);
    }
  } finally { loader.delete(); }
});

await testAsync('light C records and format calls reject invalid inputs', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), out = native._lightusd_combined_alloc(424);
  const key = '_lightusd_combined_light_format', original = native[key];
  try {
    assert.equal(native._lightusd_combined_light_get(pointer(0), 0, out), -1);
    assert.equal(native._lightusd_combined_light_get(handle, 0, pointer(0)), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 423, true);
    assert.equal(native._lightusd_combined_light_get(handle, 0, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 424, true);
    assert.equal(native._lightusd_combined_light_get(handle, 0, out), 0);
    assert.equal(native._lightusd_combined_lights_count(pointer(0)), -1);
    assert.equal(native._lightusd_combined_lights_count(handle), 0);
    assert.equal(original(pointer(0), 0, pointer(0), 0), -1);
    assert.equal(original(handle, 0, pointer(0), 1), -1);
    for (const value of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader.getLight(value), TypeError);
    assert.throws(() => loader.getLight(), TypeError);
    assert.throws(() => loader.getLightWithFormat(0), TypeError);
    assert.throws(() => loader.getLightWithFormat(0, {}), TypeError);
    assert.throws(() => loader.getAllLights(0), TypeError);
    assert.throws(() => loader.getLight.call({}, 0), TypeError);
    let calls = 0;
    const failure = new TypeError('light serialization failure');
    native[key] = () => { ++calls; throw failure; };
    assert.throws(() => loader.getLightWithFormat(0, 'json'), error => error === failure);
    assert.equal(calls, 1);
    native[key] = original;
    loader.delete();
    assert.throws(() => loader.getLight(0), TypeError);
    assert.throws(() => loader.getAllLights(), TypeError);
    assert.throws(() => loader.getLightWithFormat(0, 'json'), TypeError);
  } finally {
    native[key] = original;
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('light adapter copies spectral pairs and validates their borrowed span', () => {
  const loader = new native.LightUSDLoaderNative();
  const bytes = native._lightusd_combined_alloc(16);
  const get = native._lightusd_combined_light_get, copy = native._lightusd_combined_table_string;
  const encoded = ['held', 'micrometers', 'd50'].map(s => new TextEncoder().encode(s));
  let count = 2;
  try {
    assert.equal(loader.loadFromBinary('#usda 1.0\ndef DistantLight "Sun" {}', 'spectral-record.usda'), true);
    new Float32Array(native.HEAPU8.buffer, Number(bytes), 4).set([-0, 1.25, 780, 2.5]);
    native._lightusd_combined_light_get = (ptr, id, out) => {
      const status = get(ptr, id, out), view = new DataView(native.HEAPU8.buffer);
      if (status === 1) {
        view.setUint32(Number(out) + 4, view.getUint32(Number(out) + 4, true) | 16, true);
        view.setBigUint64(Number(out) + 16, BigInt(bytes), true);
        view.setBigUint64(Number(out) + 24, BigInt(count), true);
      }
      return status;
    };
    native._lightusd_combined_table_string = (index, out, cap) => {
      if (index < 8) return copy(index, out, cap);
      const value = encoded[index - 8];
      if (Number(out)) {
        if (cap < value.length) return -1;
        native.HEAPU8.set(value, Number(out));
      }
      return value.length;
    };
    const emission = loader.getLight(0).spectralEmission;
    assert.deepEqual(emission, {samples: [[-0, 1.25], [780, 2.5]], interpolation: 'held', unit: 'micrometers', preset: 'd50'});
    new Float32Array(native.HEAPU8.buffer, Number(bytes), 4)[1] = 999;
    assert.equal(emission.samples[0][1], 1.25);
    count = 0;
    assert.deepEqual(loader.getLight(0).spectralEmission.samples, []);
    count = native.HEAPU8.byteLength;
    assert.throws(() => loader.getLight(0), RangeError);
  } finally {
    native._lightusd_combined_light_get = get;
    native._lightusd_combined_table_string = copy;
    native._lightusd_combined_free(bytes);
    loader.delete();
  }
});

const SKELETON_QUERY_USDA = `#usda 1.0
 def SkelRoot "World" {
  def Skeleton "Rig" (displayName = "骨格") {
   uniform token[] joints = ["Root", "Root/Child", "Root/Other", "Root/Child/Tip"]
   uniform matrix4d[] bindTransforms = [
     ((1,0,0,0),(0,1,0,0),(0,0,1,0),(10,0,0,1)),
     ((1,0,0,0),(0,1,0,0),(0,0,1,0),(20,0,0,1)),
     ((1,0,0,0),(0,1,0,0),(0,0,1,0),(30,0,0,1)),
     ((1,0,0,0),(0,1,0,0),(0,0,1,0),(40,0,0,1))]
   uniform matrix4d[] restTransforms = [
     ((2,0,0,0),(0,3,0,0),(0,0,4,0),(1,2,3,1)),
     ((1,0,0,0),(0,1,0,0),(0,0,1,0),(4,5,6,1)),
     ((1,0,0,0),(0,1,0,0),(0,0,1,0),(7,8,9,1)),
     ((1,0,0,0),(0,1,0,0),(0,0,1,0),(10,11,12,1))]
  }
  def Mesh "Skin" (prepend apiSchemas = ["SkelBindingAPI"]) {
   rel skel:skeleton = </World/Rig>
   int[] faceVertexCounts = [3]
   int[] faceVertexIndices = [0,1,2]
   point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
   int[] primvars:skel:jointIndices = [0,2,3] (interpolation = "vertex"
 elementSize = 1)
   float[] primvars:skel:jointWeights = [1,1,1] (interpolation = "vertex"
 elementSize = 1)
  }
 }
`;
await testAsync('skeleton queries preserve hierarchy, preorder topology and owned transforms', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.deepEqual(loader.getAllSkeletons(), []);
    assert.deepEqual(loader.getSkeleton(0), {error: 'Scene not loaded'});
    assert.deepEqual(loader.getSkeletonJointsFlat(0), {error: 'Scene not loaded'});
    assert.equal(loader.loadFromBinary(SKELETON_QUERY_USDA, 'skeleton.usda'), true, loader.error());
    assert.equal(loader.numSkeletons(), 1);
    const skel = loader.getSkeleton(0), flat = loader.getSkeletonJointsFlat(0);
    assert.equal(skel.id, 0); assert.equal(skel.anim_id, -1);
    assert.equal(skel.prim_name, 'Rig'); assert.equal(skel.abs_path, '/World/Rig');
    assert.equal(skel.display_name, '骨格');
    assert.deepEqual(loader.getAllSkeletons(), [skel]);
    assert.deepEqual(loader.getSkeleton(0.9), skel);
    assert.deepEqual(flat.joint_ids, [0, 1, 3, 2]);
    assert.deepEqual(flat.parent_indices, [-1, 0, 1, 0]);
    assert.deepEqual(flat.joint_paths, ['Root', 'Root/Child', 'Root/Child/Tip', 'Root/Other']);
    assert.deepEqual(flat.joint_names, flat.joint_paths);
    assert.equal(flat.num_joints, 4);
    const nodes = [skel.root_node, skel.root_node.children[0], skel.root_node.children[0].children[0], skel.root_node.children[1]];
    assert.deepEqual(nodes.map(n => n.joint_id), flat.joint_ids);
    assert.deepEqual(nodes.flatMap(n => n.bind_transform), flat.bind_matrices);
    assert.deepEqual(nodes.flatMap(n => n.rest_transform), flat.rest_matrices);
    assert.deepEqual(nodes.map(n => n.bind_transform[12]), [10, 20, 40, 30]);
    assert.deepEqual(nodes.map(n => n.rest_transform.slice(12)), [[1,2,3,1], [4,5,6,1], [10,11,12,1], [7,8,9,1]]);
    assert.deepEqual(nodes[0].rest_transform.slice(0,12), [2,0,0,0,0,3,0,0,0,0,4,0]);
    assert.deepEqual(loader.getSkeleton(-1), {error: 'Invalid skeleton ID'});
    assert.deepEqual(loader.getSkeletonJointsFlat(1), {error: 'Invalid skeleton ID'});
    flat.bind_matrices[12] = 999;
    nodes[0].children.length = 0;
    assert.equal(loader.getSkeletonJointsFlat(0).bind_matrices[12], 10);
    assert.equal(loader.getSkeleton(0).root_node.children.length, 2);
    loader.reset();
    assert.deepEqual(loader.getAllSkeletons(), []);
    assert.equal(flat.rest_matrices[12], 1);
    const animated = fs.readFileSync(new URL('../../../tests/usda/usdskel-001.usda', import.meta.url), 'utf8');
    const second = animated.slice(animated.indexOf('def SkelRoot'))
      .replaceAll('"Model"', '"ModelB"').replaceAll('/Model/', '/ModelB/');
    assert.equal(loader.loadFromBinary(animated + '\n' + second, 'animated-rigs.usda'), true, loader.error());
    const rigs = loader.getAllSkeletons();
    assert.equal(rigs.length, 2);
    assert.deepEqual(rigs.map(r => r.id), [0, 1]);
    assert.deepEqual(rigs.map(r => r.abs_path).sort(), ['/Model/Skel', '/ModelB/Skel']);
    assert.deepEqual(rigs.map(r => r.anim_id).sort(), [0, 1]);
    for (const rig of rigs) {
      assert.deepEqual(loader.getSkeleton(rig.id), rig);
      assert.equal(rig.root_node.joint_path, 'Shoulder');
      assert.deepEqual(loader.getSkeletonJointsFlat(rig.id).parent_indices, [-1, 0, 1]);
    }

  } finally { loader.delete(); }
});

await testAsync('skeleton cursors validate records and close after exceptions', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), out = native._lightusd_combined_alloc(272);
  const nextKey = '_lightusd_combined_skeleton_next', endKey = '_lightusd_combined_skeleton_end';
  const next = native[nextKey], end = native[endKey];
  let cursor = 0;
  try {
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 15, true);
    assert.equal(native._lightusd_combined_skeleton_begin(handle, 0, out), -1);
    assert.equal(native._lightusd_combined_skeleton_begin(pointer(0), 0, out), -1);
    assert.equal(native._lightusd_combined_skeleton_begin(handle, 0, pointer(0)), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 16, true);
    assert.equal(native._lightusd_combined_skeleton_begin(handle, 0, out), 0);
    assert.equal(next(pointer(0), out), -1);
    end(pointer(0));
    assert.equal(loader.loadFromBinary(SKELETON_QUERY_USDA, 'cursor.usda'), true);
    assert.equal(native._lightusd_combined_skeleton_begin(handle, 0, out), 1);
    cursor = pointer(new DataView(native.HEAPU8.buffer).getBigUint64(Number(out) + 8, true));
    assert.equal(next(cursor, pointer(0)), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 271, true);
    assert.equal(next(cursor, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 272, true);
    const ids = [];
    for (let i = 0; i < 4; ++i) {
      assert.equal(next(cursor, out), 1);
      ids.push(new DataView(native.HEAPU8.buffer).getInt32(Number(out) + 4, true));
    }
    assert.deepEqual(ids, [0,1,3,2]);
    assert.equal(next(cursor, out), 0);
    assert.equal(next(cursor, out), 0);
    end(cursor); cursor = 0;
    for (const name of ['getSkeleton', 'getSkeletonJointsFlat']) {
      for (const value of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader[name](value), TypeError);
      assert.throws(() => loader[name](), TypeError);
      assert.throws(() => loader[name].call({}, 0), TypeError);
    }
    assert.throws(() => loader.getAllSkeletons(0), TypeError);
    let closed = 0, calls = 0;
    const failure = new TypeError('joint cursor failure');
    native[endKey] = cursor => { ++closed; end(cursor); };
    native[nextKey] = () => { ++calls; throw failure; };
    assert.throws(() => loader.getSkeletonJointsFlat(0), error => error === failure);
    assert.equal(calls, 1); assert.equal(closed, 1);
    native[nextKey] = (cursor, out) => {
      const status = next(cursor, out);
      if (!loader.isDeleted()) loader.delete();
      return status;
    };
    assert.deepEqual(loader.getSkeletonJointsFlat(0).joint_ids, [0,1,3,2]);
    assert.equal(closed, 2);
    assert.throws(() => loader.getSkeleton(0), TypeError);
    assert.throws(() => loader.getSkeletonJointsFlat(0), TypeError);
    assert.throws(() => loader.getAllSkeletons(), TypeError);
  } finally {
    native[nextKey] = next; native[endKey] = end;
    if (cursor) end(cursor);
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('animation queries preserve baseline summaries, channels and owned track data', () => {
  const snapshots = JSON.parse(fs.readFileSync(new URL('./fixtures/combined-animation-queries.json', import.meta.url), 'utf8'),
    (key, value) => value && value.$negativeZero === true ? -0 : value && value.$float32
      ? new Float32Array(value.$float32) : value)[wasm64 ? 'wasm64' : 'wasm32'];
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.deepEqual(loader.getAllAnimations(), []);
    assert.deepEqual(loader.getAllAnimationInfos(), []);
    assert.deepEqual(loader.getAnimation(0), {});
    assert.deepEqual(loader.getAnimationInfo(0), {});
    for (const [file, expected] of Object.entries(snapshots)) {
      const source = new Uint8Array(fs.readFileSync(new URL('../../../tests/usda/' + file, import.meta.url)));
      assert.equal(loader.loadFromBinary(source, file), true, loader.error());
      assert.deepEqual(loader.getAllAnimations(), expected.animations, file);
      assert.deepEqual(loader.getAllAnimationInfos(), expected.infos, file);
      for (let i = 0; i < expected.animations.length; ++i) {
        assert.deepEqual(loader.getAnimation(i), expected.animations[i]);
        assert.deepEqual(loader.getAnimationInfo(i), expected.infos[i]);
      }
      assert.deepEqual(loader.getAnimation(-1), {});
      assert.deepEqual(loader.getAnimation(loader.numAnimations()), {});
      assert.deepEqual(loader.getAnimationInfo(loader.numAnimations()), {});
      assert.deepEqual(loader.getAnimation(0.9), expected.animations[0]);
      assert.deepEqual(loader.getAnimationInfo(0.9), expected.infos[0]);
      const owned = loader.getAnimation(0), sampler = owned.samplers[0];
      const rawFirst = sampler.values[0], trackFirst = owned.tracks[0].values[0];
      sampler.values[0] = 999;
      assert.equal(owned.tracks[0].values[0], trackFirst);
      owned.tracks[0].times[0] = 999;
      assert.notEqual(sampler.times[0], 999);
      assert.equal(loader.getAnimation(0).samplers[0].values[0], rawFirst);
      loader.reset();
      assert.equal(sampler.values[0], 999);
      assert.deepEqual(loader.getAllAnimations(), []);
    }
  } finally { loader.delete(); }
});

await testAsync('animation C records validate indices, spans and retained ownership', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), out = native._lightusd_combined_alloc(64);
  const key = '_lightusd_combined_animation_sampler', sampler = native[key];
  try {
    assert.equal(native._lightusd_combined_animations_count(pointer(0)), -1);
    assert.equal(native._lightusd_combined_animations_count(handle), 0);
    for (const [name, size, indexed] of [['get', 64, false], ['sampler', 40, true], ['channel', 32, true]]) {
      const fn = native['_lightusd_combined_animation_' + name];
      const call = (owner, id, index, record) => indexed ? fn(owner, id, index, record) : fn(owner, id, record);
      new DataView(native.HEAPU8.buffer).setUint32(Number(out), size - 1, true);
      assert.equal(call(handle, 0, 0, out), -1);
      new DataView(native.HEAPU8.buffer).setUint32(Number(out), size, true);
      assert.equal(call(pointer(0), 0, 0, out), -1);
      assert.equal(call(handle, 0, 0, pointer(0)), -1);
      assert.equal(call(handle, 0, 0, out), 0);
    }
    const bytes = new Uint8Array(fs.readFileSync(new URL('../../../tests/usda/xform-timesamples-001.usda', import.meta.url)));
    assert.equal(loader.loadFromBinary(bytes, 'animation.usda'), true, loader.error());
    assert.deepEqual(loader.getAnimationInfo(-1), {});
    for (const [name, size] of [['get', 64], ['sampler', 40], ['channel', 32]]) {
      const fn = native['_lightusd_combined_animation_' + name];
      for (const id of [-1, 9999]) {
        new DataView(native.HEAPU8.buffer).setUint32(Number(out), size, true);
        assert.equal(name === 'get' ? fn(handle, id, out) : fn(handle, id, 0, out), 0);
      }
      if (name !== 'get') for (const index of [-1, 9999]) {
        new DataView(native.HEAPU8.buffer).setUint32(Number(out), size, true);
        assert.equal(fn(handle, 0, index, out), 0);
      }
    }
    for (const name of ['getAnimation', 'getAnimationInfo']) {
      for (const id of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader[name](id), TypeError);
      assert.throws(() => loader[name](), TypeError);
      assert.throws(() => loader[name].call({}, 0), TypeError);
    }
    for (const name of ['getAllAnimations', 'getAllAnimationInfos']) assert.throws(() => loader[name](0), TypeError);
    for (const [mode, expected] of [[1, 'STEP'], [2, 'CUBICSPLINE']]) {
      native[key] = (...args) => {
        const status = sampler(...args);
        new DataView(native.HEAPU8.buffer).setUint32(Number(args[3]) + 4, mode, true);
        return status;
      };
      const animation = loader.getAnimation(0);
      assert.equal(animation.samplers[0].interpolation, expected);
      assert.equal(animation.tracks[0].interpolation, expected);
    }
    native[key] = (...args) => {
      const status = sampler(...args);
      new DataView(native.HEAPU8.buffer).setBigUint64(Number(args[3]) + 8, 0xffffffffffffffffn, true);
      return status;
    };
    assert.throws(() => loader.getAnimation(0), /invalid sampler span/);
    let calls = 0;
    const failure = new TypeError('sampler failure');
    native[key] = () => { ++calls; throw failure; };
    assert.throws(() => loader.getAnimation(0), error => error === failure);
    assert.equal(calls, 1);
    native[key] = (...args) => {
      const status = sampler(...args);
      if (!loader.isDeleted()) loader.delete();
      return status;
    };
    const retained = loader.getAnimation(0);
    assert.ok(retained.tracks[0].times.length);
    for (const name of ['getAnimation', 'getAnimationInfo', 'getAllAnimations', 'getAllAnimationInfos'])
      assert.throws(() => loader[name](0), TypeError);
  } finally {
    native[key] = sampler;
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('mesh primvar inspection and deferred tangents preserve behavior', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.deepEqual(JSON.parse(loader.getMeshPrimvarsJSON(0)), {version: 1, primvars: {}, error: 'invalid mesh id'});
    assert.equal(loader.computeMeshTangents(0), false);
    loader.setDeferTangentComputation(true);
    const scene = `#usda 1.0
 def Material "Material" {
  token outputs:surface.connect = </Material/Surface.outputs:surface>
  def Shader "Surface" {
   uniform token info:id = "UsdPreviewSurface"
   normal3f inputs:normal.connect = </Material/Normal.outputs:rgb>
   token outputs:surface
  }
  def Shader "Normal" {
   uniform token info:id = "UsdUVTexture"
   asset inputs:file = @normal.png@
   float3 outputs:rgb
  }
 }
 def Mesh "Triangle" {
  rel material:binding = </Material>
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
  normal3f[] normals = [(0,0,1),(0,0,1),(0,0,1)] (interpolation = "vertex")
  texCoord2f[] primvars:st = [(0,0),(1,0),(0,1)] (interpolation = "vertex")
  float[] primvars:heat = [5,9] (interpolation = "vertex")
  int[] primvars:heat:indices = [1,0,1]
 }`;
    assert.equal(loader.loadFromBinary(scene, 'primvars.usda'), true, loader.error());
    assert.deepEqual(JSON.parse(loader.getMeshPrimvarsJSON(0)).primvars, {});
    assert.equal(loader.loadAsLayerFromBinary(scene, 'primvars.usda'), true);
    assert.equal(loader.layerToRenderScene(), true, loader.error());
    const before = loader.getMeshPrimvarsJSON(0), parsed = JSON.parse(before);
    assert.equal(parsed.version, 1); assert.equal(parsed.primPath, '/Triangle');
    assert.equal(parsed.primvars.heat.name, 'heat');
    assert.equal(parsed.primvars.heat.type, 'float[]');
    assert.equal(parsed.primvars.heat.interpolation, 'vertex');
    assert.equal(parsed.primvars.heat.elementSize, 1);
    // The typed C string path exposes the C++ JSON contract on both widths;
    // an earlier wasm32 Embind build produced an extra nested-array suffix.
    assert.deepEqual(parsed.primvars.heat.value, {type: 'float[]', value: [9,5,9]});
    assert.equal(loader.getMeshPrimvarsJSON(0.9), before);
    assert.equal(loader.computeMeshTangents(-1), false);
    assert.equal(loader.computeMeshTangents(99), false);
    assert.equal(loader.computeMeshTangents(0.9), true);
    const mesh = loader.getMeshCopy(0);
    assert.ok(mesh.tangents && mesh.tangents.length);
    assert.equal(loader.computeMeshTangents(0), true);
    assert.deepEqual(loader.getMeshCopy(0).tangents, mesh.tangents);
    assert.equal(loader.getMeshPrimvarsJSON(0), before);
    loader.reset();
    assert.equal(loader.computeMeshTangents(0), false);
    assert.equal(JSON.parse(before).primPath, '/Triangle');
  } finally { loader.delete(); }
});

await testAsync('mesh operations reject invalid calls and do not replay mutations', () => {
  const loader = new native.LightUSDLoaderNative();
  const key = '_lightusd_combined_mesh_operation', original = native[key];
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  try {
    assert.equal(original(pointer(0), 0, 0), -1);
    assert.equal(original(pointer(loader.$$.ptr), 2, 0), -1);
    for (const name of ['getMeshPrimvarsJSON', 'computeMeshTangents']) {
      assert.throws(() => loader[name](), TypeError);
      assert.throws(() => loader[name](0, 0), TypeError);
      assert.throws(() => loader[name].call({}, 0), TypeError);
      for (const id of ['0', 0n, {}, Infinity, 2147483648]) assert.throws(() => loader[name](id), TypeError);
      let calls = 0;
      const failure = new TypeError('mesh operation failure');
      native[key] = () => { ++calls; throw failure; };
      assert.throws(() => loader[name](0), error => error === failure);
      assert.equal(calls, 1);
      native[key] = original;
    }
    native[key] = (...args) => {
      loader.delete();
      return original(...args);
    };
    assert.equal(JSON.parse(loader.getMeshPrimvarsJSON(0)).error, 'invalid mesh id');
    assert.throws(() => loader.getMeshPrimvarsJSON(0), TypeError);
    assert.throws(() => loader.computeMeshTangents(0), TypeError);
  } finally {
    native[key] = original;
    native._lightusd_combined_table_release();
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('bone textures preserve influence rounding, layout and owned arrays', () => {
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.deepEqual(loader.generateBoneTexture(0, 0), {error: 'Invalid mesh ID or scene not loaded'});
    assert.equal(loader.loadFromBinary(SKELETON_QUERY_USDA, 'skin.usda'), true, loader.error());
    for (const [requested, maximum] of [[0,4],[-1,4],[1,4],[5,8],[9,16],[17,32],[33,48],[49,64],[65,80],[81,96],[97,128],[129,128]]) {
      const result = loader.generateBoneTexture(0, requested);
      assert.equal(result.maxInfluences, maximum);
      assert.equal(result.vertexCount, 3); assert.equal(result.originalElementSize, 1);
      assert.equal(result.texelsPerVertex, maximum / 2);
      let width = 1;
      while (width * width < 3 * maximum / 2) width *= 2;
      assert.equal(result.textureWidth, width);
      assert.equal(result.textureHeight, Math.ceil(3 * maximum / 2 / width));
      const expected = new Float32Array(width * result.textureHeight * 4);
      for (let v = 0; v < 3; ++v) {
        for (let j = 0; j < maximum; ++j) expected[v * maximum * 2 + j * 2] = -1;
        expected[v * maximum * 2] = [0,2,3][v];
        expected[v * maximum * 2 + 1] = 1;
      }
      assert.deepEqual(result.textureData, expected);
      assert.deepEqual(result.vertexOffsets, new Float32Array([0,maximum/2,maximum]));
    }
    const multi = SKELETON_QUERY_USDA.replace('[0,2,3]', '[0,1,2,3,0,1, 0,1,2,3,0,1, 0,1,2,3,0,1]')
      .replace('[1,1,1]', '[0.1,0.4,0,-0.1,0.2,0.3, 0.1,0.4,0,-0.1,0.2,0.3, 0.1,0.4,0,-0.1,0.2,0.3]')
      .replaceAll('elementSize = 1', 'elementSize = 6');
    assert.equal(loader.loadFromBinary(multi, 'influences.usda'), true, loader.error());
    assert.deepEqual(loader.generateBoneTexture(0,4).textureData.slice(0,8), new Float32Array([1,.4,0,.1,-1,0,-1,0]));
    assert.deepEqual(loader.generateBoneTexture(0,8).textureData.slice(0,16), new Float32Array([1,.4,1,.3,0,.2,0,.1,-1,0,-1,0,-1,0,-1,0]));
    assert.equal(loader.loadFromBinary(SKELETON_QUERY_USDA, 'skin.usda'), true);
    const first = loader.generateBoneTexture(0, 0);
    first.textureData[0] = 999; first.vertexOffsets[0] = 999;
    assert.equal(loader.generateBoneTexture(0, 0).textureData[0], 0);
    assert.equal(loader.generateBoneTexture(0, 0).vertexOffsets[0], 0);
    assert.deepEqual(loader.generateBoneTexture(-1, 0), {error: 'Invalid mesh ID or scene not loaded'});
    assert.deepEqual(loader.generateBoneTexture(99, 0), {error: 'Invalid mesh ID or scene not loaded'});
    assert.deepEqual(loader.generateBoneTexture(0.9, 4.9), loader.generateBoneTexture(0, 4));
    loader.reset();
    assert.equal(first.textureData[0], 999);
    assert.equal(loader.loadFromBinary(SKELETON_QUERY_USDA.replace('int[] primvars:skel:jointIndices', 'int[] ignoredIndices').replace('float[] primvars:skel:jointWeights', 'float[] ignoredWeights'), 'empty.usda'), true);
    assert.deepEqual(loader.generateBoneTexture(0, 0), {error: 'Mesh has no skinning data'});
  } finally { loader.delete(); }
});

await testAsync('bone texture C results own their arrays and release after adapter failures', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const beginKey = '_lightusd_combined_bone_texture_begin', endKey = '_lightusd_combined_bone_texture_end';
  const begin = native[beginKey], end = native[endKey];
  const out = native._lightusd_combined_alloc(72), handle = pointer(loader.$$.ptr);
  let result = 0;
  try {
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 71, true);
    assert.equal(begin(handle, 0, 0, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 72, true);
    assert.equal(begin(pointer(0), 0, 0, out), -1);
    assert.equal(begin(handle, 0, 0, pointer(0)), -1);
    assert.equal(begin(handle, 0, 0, out), 0);
    end(pointer(0));
    assert.equal(loader.loadFromBinary(SKELETON_QUERY_USDA, 'skin.usda'), true);
    assert.equal(begin(handle, 0, 0, out), 1);
    let view = new DataView(native.HEAPU8.buffer, Number(out), 72);
    result = pointer(view.getBigUint64(32, true));
    const address = Number(view.getBigUint64(40, true)), count = Number(view.getBigUint64(48, true));
    const saved = new Float32Array(native.HEAPU8.buffer, address, count).slice();
    loader.generateBoneTexture(0, 128);
    loader.reset();
    assert.deepEqual(new Float32Array(native.HEAPU8.buffer, address, count), saved);
    end(result); result = 0;
    for (const args of [[], [0], [0,0,0], ['0',0], [0,0n], [Infinity,0], [0,2147483648]])
      assert.throws(() => loader.generateBoneTexture(...args), TypeError);
    assert.throws(() => loader.generateBoneTexture.call({}, 0, 0), TypeError);
    assert.equal(loader.loadFromBinary(SKELETON_QUERY_USDA, 'skin.usda'), true);
    let closed = 0, calls = 0;
    native[endKey] = ptr => { ++closed; end(ptr); };
    native[beginKey] = (...args) => {
      const status = begin(...args);
      new DataView(native.HEAPU8.buffer).setBigUint64(Number(args[3]) + 48, 0xffffffffffffffffn, true);
      return status;
    };
    assert.throws(() => loader.generateBoneTexture(0, 0), /invalid result span/);
    assert.equal(closed, 1);
    const failure = new TypeError('bone generation failure');
    native[beginKey] = () => { ++calls; throw failure; };
    assert.throws(() => loader.generateBoneTexture(0, 0), error => error === failure);
    assert.equal(calls, 1);
    native[beginKey] = (...args) => {
      const status = begin(...args);
      loader.delete();
      return status;
    };
    const retained = loader.generateBoneTexture(0, 0);
    assert.equal(retained.textureData[1], 1);
    assert.equal(closed, 2);
    assert.throws(() => loader.generateBoneTexture(0, 0), TypeError);
  } finally {
    native[beginKey] = begin; native[endKey] = end;
    if (result) end(result);
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});


const meshQuerySources = {
  textured: TEXTURED_TWO_MATERIAL_USDA,
  skin: SKELETON_QUERY_USDA,
  ...Object.fromEntries(['c-core-mesh-queries.usda', 'geomsubset-001.usda',
    'geometry-optimize-subset-partial-001.usda', 'geometry-optimize-subset-overlap-001.usda']
    .map(name => [name, fs.readFileSync(new URL('../../../tests/usda/' + name, import.meta.url), 'utf8')]))
};
function snapshotMesh(value) {
  if (typeof value === 'bigint') return {$bigint: String(value)};
  if (typeof value === 'number' && Object.is(value, -0)) return {$negativeZero: true};
  if (ArrayBuffer.isView(value)) return {$type: value.constructor.name, data: Array.from(value, snapshotMesh)};
  if (Array.isArray(value)) return value.map(snapshotMesh);
  if (value && typeof value === 'object') {
    if ('ptr' in value && 'dtype' in value) {
      const constructors = {f32: Float32Array, u32: Uint32Array, snorm8: Int8Array, snorm16: Int16Array, u8: Uint8Array, i8: Int8Array};
      const {ptr, ...rest} = value;
      return {...rest, data: snapshotMesh(new constructors[value.dtype](native.HEAPU8.buffer, ptr, value.length))};
    }
    return Object.fromEntries(Object.entries(value).map(([k,v]) => [k,snapshotMesh(v)]));
  }
  return value;
}
await testAsync('mesh accessors preserve complete payloads, descriptors and material ordering', () => {
  const expected = JSON.parse(fs.readFileSync(new URL('./fixtures/combined-mesh-queries.json', import.meta.url), 'utf8'))[wasm64 ? 'wasm64' : 'wasm32'];
  // Preserve original snapshots while correcting the known memory64 size_t
  // conversion in UV vertex counts (previously BigInts).
  if (wasm64) for (const meshes of Object.values(expected)) for (const mesh of meshes) {
    for (const phase of ['before', 'after']) for (const uv of Object.values(mesh[phase].copy.uvSets)) {
      if (uv.vertexCount?.$bigint) uv.vertexCount = Number(BigInt(uv.vertexCount.$bigint));
    }
  }
  const loader = new native.LightUSDLoaderNative();
  try {
    for (const method of ['getMesh', 'getMeshCopy', 'getMeshPtr']) assert.deepEqual(loader[method](0), {});
    // Eager generation pins the pre-fix snapshot independently of the deferred path.
    loader.setDeferTangentComputation(false);
    for (const [name, source] of Object.entries(meshQuerySources)) {
      assert.equal(loader.loadFromBinary(source, name), true, loader.error());
      assert.equal(loader.numMeshes(), expected[name].length);
      for (let i = 0; i < loader.numMeshes(); ++i) {
        const check = reference => {
          assert.deepEqual(snapshotMesh(loader.getMeshCopy(i)), reference.copy, `${name}: copy ${i}`);
          assert.deepEqual(snapshotMesh(loader.getMesh(i)), reference.copy, `${name}: view ${i}`);
          assert.deepEqual(snapshotMesh(loader.getMeshPtr(i)), reference.descriptor, `${name}: descriptor ${i}`);
        };
        check(expected[name][i].before);
        assert.equal(loader.computeMeshTangents(i), expected[name][i].computed);
        check(expected[name][i].after);
        const copy = loader.getMeshCopy(i), saved = copy.points[0];
        copy.points[0] = 987;
        assert.equal(loader.getMeshCopy(i).points[0], saved);
        assert.deepEqual(snapshotMesh(loader.getMeshPtr(i + 0.9)), expected[name][i].after.descriptor);
      }
      for (const method of ['getMesh', 'getMeshCopy', 'getMeshPtr']) {
        assert.deepEqual(loader[method](-1), {});
        assert.deepEqual(loader[method](loader.numMeshes()), {});
      }
      const owned = loader.getMeshCopy(0), snapshot = snapshotMesh(owned);
      const tangents = loader.getMeshPtr(0).tangents;
      const savedTangents = tangents?.slice();
      loader.reset();
      assert.deepEqual(snapshotMesh(owned), snapshot);
      assert.deepEqual(tangents, savedTangents);
      assert.deepEqual(loader.getMeshPtr(0), {});
    }
  } finally { loader.delete(); }
});

await testAsync('deferred packed normals produce stable tangents across all mesh accessors', () => {
  const eager = new native.LightUSDLoaderNative(), deferred = new native.LightUSDLoaderNative();
  const source = fs.readFileSync(new URL('../../../tests/usda/c-core-mesh-queries.usda', import.meta.url), 'utf8');
  try {
    eager.setDeferTangentComputation(false);
    assert.equal(eager.loadFromBinary(source, 'eager.usda'), true, eager.error());
    const expected = eager.getMeshCopy(0).tangents;
    for (let iteration = 0; iteration < 3; ++iteration) {
      deferred.setDeferTangentComputation(true);
      assert.equal(deferred.loadFromBinary(source, 'deferred.usda'), true, deferred.error());
      assert.equal(deferred.getMeshCopy(0).tangents, undefined);
      assert.equal(deferred.computeMeshTangents(0), true);
      const mesh = deferred.getMeshCopy(0);
      assert.equal(mesh.normalsFormat, 'snorm8');
      assert.ok(mesh.tangentsPacked instanceof Uint32Array);
      assert.deepEqual(mesh.tangents, expected);
      assert.deepEqual(deferred.getMeshPtr(0).tangents, expected);
      assert.deepEqual(deferred.getMesh(0).tangents, expected);
      for (const component of mesh.tangents) assert.ok(Number.isFinite(component));
      assert.equal(deferred.computeMeshTangents(0), true);
      assert.deepEqual(deferred.getMeshCopy(0).tangents, expected);
      deferred.reset();
      assert.deepEqual(mesh.tangents, expected);
    }
  } finally { eager.delete(); deferred.delete(); }
});

await testAsync('mesh pointer C records validate spans and release retained results', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const begin = native._lightusd_combined_mesh_pointer_begin;
  const attributeKey = '_lightusd_combined_mesh_pointer_attribute', endKey = '_lightusd_combined_mesh_pointer_end';
  const attribute = native[attributeKey], end = native[endKey], submesh = native._lightusd_combined_mesh_pointer_submesh;
  const out = native._lightusd_combined_alloc(64), handle = pointer(loader.$$.ptr);
  let result = 0;
  try {
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 63, true);
    assert.equal(begin(handle, 0, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 64, true);
    assert.equal(begin(pointer(0), 0, out), -1);
    assert.equal(begin(handle, 0, pointer(0)), -1);
    assert.equal(begin(handle, 0, out), 0);
    end(pointer(0));
    loader.setDeferTangentComputation(false);
    assert.equal(loader.loadFromBinary(meshQuerySources['c-core-mesh-queries.usda'], 'mesh.usda'), true);
    assert.equal(begin(handle, 0, out), 1);
    const info = new DataView(native.HEAPU8.buffer, Number(out), 64);
    result = pointer(info.getBigUint64(56, true));
    const attributeCount = info.getUint32(24, true), submeshCount = info.getUint32(28, true);
    assert.ok(attributeCount > 0 && submeshCount > 0);
    for (const [read, size, count] of [[attribute,40,attributeCount], [submesh,16,submeshCount]]) {
      new DataView(native.HEAPU8.buffer).setUint32(Number(out), size - 1, true);
      assert.equal(read(result, 0, out), -1);
      new DataView(native.HEAPU8.buffer).setUint32(Number(out), size, true);
      assert.equal(read(pointer(0), 0, out), -1);
      assert.equal(read(result, 0, pointer(0)), -1);
      assert.equal(read(result, -1, out), -1);
      assert.equal(read(result, count, out), 0);
      assert.equal(read(result, 0, out), 1);
    }
    end(result); result = 0;
    for (const args of [[],[0,0],['0'],[0n],[Infinity],[2147483648]]) assert.throws(() => loader.getMeshPtr(...args), TypeError);
    assert.throws(() => loader.getMeshPtr.call({}, 0), TypeError);
    const descriptors = loader.getMeshPtr(0);
    assert.notEqual(descriptors.uvSets['0'], descriptors.uv0);
    const oldLength = descriptors.uv0.length;
    descriptors.uvSets['0'].length = 999;
    assert.equal(descriptors.uv0.length, oldLength);
    let closed = 0, calls = 0;
    native[endKey] = ptr => { ++closed; end(ptr); };
    native[attributeKey] = (...args) => {
      const status = attribute(...args);
      new DataView(native.HEAPU8.buffer).setBigUint64(Number(args[2]) + 32, 0xffffffffffffffffn, true);
      return status;
    };
    assert.throws(() => loader.getMeshPtr(0), /invalid attribute span/);
    assert.equal(closed, 1);
    const failure = new TypeError('descriptor read failure');
    native[attributeKey] = () => { ++calls; throw failure; };
    assert.throws(() => loader.getMeshPtr(0), error => error === failure);
    assert.equal(calls, 1); assert.equal(closed, 2);
    native[attributeKey] = (...args) => {
      const status = attribute(...args);
      if (!loader.isDeleted()) loader.delete();
      return status;
    };
    const retained = loader.getMeshPtr(0);
    assert.equal(retained.tangents.length, descriptors.tangents.length);
    assert.deepEqual(retained.tangents, descriptors.tangents);
    assert.equal(closed, 3);
    assert.throws(() => loader.getMeshPtr(0), TypeError);
  } finally {
    native[attributeKey] = attribute; native[endKey] = end;
    if (result) end(result);
    native._lightusd_combined_free(out);
    native._lightusd_combined_table_release();
    if (!loader.isDeleted()) loader.delete();
  }
});

await testAsync('mesh value C records preserve views, copies, warnings and cleanup', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const begin = native._lightusd_combined_mesh_value_begin;
  const key = '_lightusd_combined_mesh_pointer_attribute', endKey = '_lightusd_combined_mesh_pointer_end';
  const attribute = native[key], end = native[endKey];
  const warn = console.warn;
  const out = native._lightusd_combined_alloc(80), handle = pointer(loader.$$.ptr);
  let result = 0;
  try {
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 79, true);
    assert.equal(begin(handle, 0, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 80, true);
    assert.equal(begin(pointer(0), 0, out), -1);
    assert.equal(begin(handle, 0, pointer(0)), -1);
    assert.equal(begin(handle, 0, out), 0);
    assert.equal(native._lightusd_combined_mesh_warn(pointer(0)), -1);
    let warnings = 0;
    const failure = new TypeError('warning failed');
    console.warn = () => { ++warnings; throw failure; };
    assert.throws(() => loader.getMesh(0), error => error === failure);
    assert.equal(warnings, 1);
    assert.deepEqual(loader.getMesh(0), {});
    loader.reset();
    const clone = loader.clone();
    try { assert.deepEqual(clone.getMesh(0), {}); } finally { clone.delete(); }
    assert.equal(warnings, 1);
    console.warn = warn;
    assert.equal(loader.loadFromBinary(SKELETON_QUERY_USDA, 'skin.usda'), true);
    assert.equal(begin(handle, 0, out), 1);
    result = pointer(new DataView(native.HEAPU8.buffer).getBigUint64(Number(out) + 72, true));
    assert.ok(result); end(result); result = 0;
    const view = loader.getMesh(0), copy = loader.getMeshCopy(0);
    assert.equal(view.points.buffer, native.HEAPU8.buffer);
    assert.equal(view.geomBindTransform.buffer, native.HEAPU8.buffer);
    assert.notEqual(copy.points.buffer, native.HEAPU8.buffer);
    assert.notEqual(copy.geomBindTransform.buffer, native.HEAPU8.buffer);
    assert.ok(copy.jointIndices instanceof Int32Array);
    assert.ok(copy.geomBindTransform instanceof Float64Array);
    const first = copy.points[0];
    view.points[0] = 17;
    assert.equal(copy.points[0], first);
    assert.equal(loader.getMeshCopy(0).points[0], 17);
    view.points[0] = first;
    for (const name of ['getMesh', 'getMeshCopy']) {
      for (const args of [[],[0,0],['0'],[0n],[Infinity],[2147483648]]) assert.throws(() => loader[name](...args), TypeError);
      assert.throws(() => loader[name].call({}, 0), TypeError);
    }
    let closed = 0, calls = 0;
    native[endKey] = ptr => { ++closed; end(ptr); };
    native[key] = (...args) => {
      const status = attribute(...args);
      new DataView(native.HEAPU8.buffer).setBigUint64(Number(args[2]) + 32, 0xffffffffffffffffn, true);
      return status;
    };
    assert.throws(() => loader.getMeshCopy(0), /invalid attribute span/);
    assert.equal(closed, 1);
    native[key] = () => { ++calls; throw failure; };
    assert.throws(() => loader.getMesh(0), error => error === failure);
    assert.equal(calls, 1); assert.equal(closed, 2);
    native[key] = (...args) => {
      const status = attribute(...args);
      if (!loader.isDeleted()) loader.delete();
      return status;
    };
    const retained = loader.getMeshCopy(0);
    assert.deepEqual(retained.points, copy.points);
    assert.equal(closed, 3);
    assert.throws(() => loader.getMesh(0), TypeError);
    assert.throws(() => loader.getMeshCopy(0), TypeError);
  } finally {
    console.warn = warn;
    native[key] = attribute; native[endKey] = end;
    if (result) end(result);
    native._lightusd_combined_free(out);
    native._lightusd_combined_table_release();
    if (!loader.isDeleted()) loader.delete();
  }
});

const materialQuerySources = {
  textured: TEXTURED_TWO_MATERIAL_USDA,
  ...Object.fromEntries(['c-core-material-queries.usda', 'colorspace-materialx-config-render.usda',
    'lusdrender-openpbr-lobe-golden.usda'].map(name => [name,
    fs.readFileSync(new URL('../../../tests/usda/' + name, import.meta.url), 'utf8')]))
};
function snapshotMaterial(result) {
  if (typeof result.data === 'string') return {format: result.format,
    sha256: createHash('sha256').update(result.data).digest('hex')};
  return result;
}
await testAsync('material queries preserve serialized bytes and legacy property payloads', () => {
  const expected = JSON.parse(fs.readFileSync(new URL('./fixtures/combined-material-queries.json', import.meta.url), 'utf8'))[wasm64 ? 'wasm64' : 'wasm32'];
  const loader = new native.LightUSDLoaderNative();
  try {
    assert.deepEqual(loader.getMaterial(0), {error: 'Scene not loaded'});
    assert.deepEqual(loader.getMaterialWithFormat(-1, 'bad'), {error: 'Scene not loaded'});
    for (const [name, source] of Object.entries(materialQuerySources)) {
      assert.equal(loader.loadFromBinary(source, name), true, loader.error());
      assert.equal(loader.numMaterials(), expected[name].length);
      assert.ok(loader.numMaterials() > 0);
      for (let i = 0; i < loader.numMaterials(); ++i) {
        for (const format of ['json', 'xml', 'legacy', '']) {
          const result = loader.getMaterialWithFormat(i, format);
          assert.deepEqual(snapshotMaterial(result), expected[name][i][format]);
          assert.deepEqual(loader.getMaterialWithFormat(i + 0.9, new TextEncoder().encode(format)), result);
          if (format === 'json') assert.deepEqual(loader.getMaterial(i), result);
          if (result.diffuseColor) {
            result.diffuseColor[0] = 999;
            assert.notEqual(loader.getMaterialWithFormat(i, format).diffuseColor[0], 999);
          }
        }
        for (const format of ['JSON','bad','json\0']) assert.deepEqual(loader.getMaterialWithFormat(i,format),
          {error: "Unsupported format. Use 'json' or 'xml'"});
      }
      assert.deepEqual(loader.getMaterial(-1), {error: 'Invalid material ID'});
      assert.deepEqual(loader.getMaterialWithFormat(loader.numMaterials(), 'bad'), {error: 'Invalid material ID'});
      const owned = loader.getMaterialWithFormat(0, 'legacy'), snapshot = structuredClone(owned);
      loader.reset();
      assert.deepEqual(owned, snapshot);
    }
  } finally { loader.delete(); }
});

await testAsync('material C records validate inputs and preserve property masks', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const key = '_lightusd_combined_material_get', get = native[key];
  const out = native._lightusd_combined_alloc(240), handle = pointer(loader.$$.ptr);
  try {
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 239, true);
    assert.equal(get(handle, 0, pointer(0), 0, out), -1);
    new DataView(native.HEAPU8.buffer).setUint32(Number(out), 240, true);
    assert.equal(get(pointer(0), 0, pointer(0), 0, out), -1);
    assert.equal(get(handle, 0, pointer(0), 0, pointer(0)), -1);
    assert.equal(get(handle, 0, pointer(0), 1, out), -1);
    assert.equal(get(handle, 0, pointer(0), 0, out), 0);
    assert.equal(loader.loadFromBinary(TEXTURED_TWO_MATERIAL_USDA, 'materials.usda'), true);
    assert.equal(get(handle, 0, pointer(0), 0, out), 2);
    assert.equal(get(handle, -1, pointer(0), 0, out), 0);
    for (const value of ['0',0n,{},Infinity,2147483648]) {
      assert.throws(() => loader.getMaterial(value), TypeError);
      assert.throws(() => loader.getMaterialWithFormat(value, 'json'), TypeError);
    }
    assert.throws(() => loader.getMaterial(), TypeError);
    assert.throws(() => loader.getMaterial(0, 'json'), TypeError);
    assert.throws(() => loader.getMaterialWithFormat(0), TypeError);
    assert.throws(() => loader.getMaterialWithFormat(0, {}), TypeError);
    assert.throws(() => loader.getMaterial.call({}, 0), TypeError);
    for (const specular of [false, true]) {
      native[key] = (...args) => {
        const status = get(...args);
        const view = new DataView(native.HEAPU8.buffer, Number(args[4]), 240);
        view.setUint32(4, 2 | (specular ? 4 : 0), true);
        view.setUint32(8, 8191, true);
        for (let i=0;i<13;++i) view.setInt32(16+i*4,i-3,true);
        for (let i=0;i<21;++i) view.setFloat64(72+i*8,i+0.25,true);
        return status;
      };
      const material = loader.getMaterialWithFormat(0, 'legacy');
      assert.deepEqual(material.diffuseColor, [.25,1.25,2.25]);
      assert.deepEqual(material.emissiveColor, [3.25,4.25,5.25]);
      assert.deepEqual(material.normal, [9.25,10.25,11.25]);
      assert.equal(material.useSpecularWorkflow, specular);
      if (specular) {
        assert.deepEqual(material.specularColor, [6.25,7.25,8.25]);
        assert.equal(material.specularColorTextureId, -1);
        assert.equal('metallic' in material, false);
      } else {
        assert.equal(material.metallic, 12.25); assert.equal(material.metallicTextureId, 0);
        assert.equal('specularColor' in material, false);
      }
      for (const [name, offset, texture] of [['roughness',13,1],['clearcoat',14,2],['clearcoatRoughness',15,3],
        ['opacity',16,4],['opacityThreshold',17,5],['ior',18,6],['displacement',19,8],['occlusion',20,9]]) {
        assert.equal(material[name], offset + .25);
        assert.equal(material[name + 'TextureId'], texture);
      }
      assert.equal(material.normalTextureId, 7);
    }
    let calls = 0;
    const failure = new TypeError('material dispatch failed');
    native[key] = () => { ++calls; throw failure; };
    assert.throws(() => loader.getMaterial(0), error => error === failure);
    assert.equal(calls, 1);
    native[key] = (...args) => {
      const result = get(...args);
      loader.delete();
      return result;
    };
    const retained = loader.getMaterialWithFormat(0, 'legacy');
    assert.equal(retained.diffuseColor.length, 3);
    assert.throws(() => loader.getMaterial(0), TypeError);
    assert.throws(() => loader.getMaterialWithFormat(0, 'json'), TypeError);
  } finally {
    native[key] = get;
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

function schemaUtilitySnapshot() {
  const loader = new native.LightUSDLoaderNative();
  const result = {};
  const hash = text => createHash('sha256').update(text).digest('hex');
  const capture = label => { result[label] = {usda: hash(loader.exportAsUSDA()),
    physics: hash(loader.extractPhysicsSceneJSON()), error: loader.error(), warn: loader.warn()}; };
  const positions = new Float32Array([0,0,0, 1,0,0, 0,1,0]);
  const normals = new Float32Array([0,0,1, 0,0,1, 0,0,1]);
  const uv = new Float32Array([0,0, 1,0, 0,1]);
  const indices = new Int32Array([0,1,2]);
  const robot = JSON.stringify({name: 'TestBot', upAxis: 'Z', links: [{name: 'base',
    visuals: [{name: 'visual', meshRef: '三角'}], collisions: [{name: 'collision', meshRef: 'collider'}]}], joints: []});
  try {
    result.unloaded = loader.extractPhysicsSceneJSON();
    result.sample = loader.createSampleScene(); capture('sampleScene');
    loader.reset();
    result.badRobot = loader.createURDFPhysicsScene('{bad'); result.badRobotError = loader.error();
    result.invalid = [];
    for (const args of [['', null, null, null, null], ['bad', new Float32Array(3), null, null, null],
      ['bad', positions, new Float32Array(2), null, null], ['bad', positions, null, new Float32Array(1), null],
      ['bad', positions, null, null, new Int32Array(2)]]) {
      result.invalid.push([loader.setVisualMesh(...args), loader.error()]);
    }
    assert.equal(loader.setVisualMesh('三角', positions, normals, uv, indices), true, loader.error());
    assert.equal(loader.setCollisionMesh('collider', positions, undefined, null, new Uint32Array(indices)), true);
    // Input buffers must be copied before later caller mutation.
    positions.fill(9); normals.fill(9); uv.fill(9); indices.fill(0);
    result.robot = loader.createURDFPhysicsScene(robot); capture('robotScene');
    result.clear = loader.clearURDFMeshBuffers() === undefined;
    result.withoutMeshes = loader.createURDFPhysicsScene(robot); capture('withoutMeshesScene');
    return result;
  } finally { loader.delete(); }
}

await testAsync('schema utilities preserve scene exports, diagnostics and copied mesh uploads', () => {
  const expected = JSON.parse(fs.readFileSync(new URL('./fixtures/combined-schema-utilities.json', import.meta.url)));
  assert.deepEqual(schemaUtilitySnapshot(), expected[wasm64 ? 'wasm64' : 'wasm32']);
});

await testAsync('schema C calls validate spans, retain receivers and never replay mutations', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), nil = pointer(0);
  const opKey = '_lightusd_combined_schema_operation', meshKey = '_lightusd_combined_schema_mesh';
  const op = native[opKey], mesh = native[meshKey];
  const data = native._lightusd_combined_alloc(64);
  try {
    assert.equal(op(nil, 1, nil, 0), -1);
    assert.equal(op(handle, 4, nil, 0), -1);
    assert.equal(op(handle, 3, nil, 1), -1);
    assert.equal(op(handle, 2, data, 1), -1);
    const empty = [nil, 0, nil, 0, nil, 0, nil, 0];
    assert.equal(mesh(nil, nil, 0, ...empty), -1);
    assert.equal(mesh(handle, nil, 1, ...empty), -1);
    assert.equal(mesh(handle, nil, 0, nil, 1, ...empty.slice(2)), -1);
    assert.equal(mesh(handle, nil, 0, data, 0x10000001, ...empty.slice(2)), -1);
    assert.equal(mesh(handle, nil, 0, pointer(Number(data) + 1), 9, ...empty.slice(2)), -1);
    assert.equal(mesh(handle, nil, 0, ...empty), 0);
    assert.match(loader.error(), /non-empty mesh name/);
    for (const name of ['createSampleScene', 'clearURDFMeshBuffers', 'extractPhysicsSceneJSON']) {
      assert.throws(() => loader[name](1), TypeError);
      assert.throws(() => loader[name].call({}), TypeError);
    }
    assert.throws(() => loader.createURDFPhysicsScene(), TypeError);
    assert.throws(() => loader.createURDFPhysicsScene({}), TypeError);
    assert.throws(() => loader.setVisualMesh('mesh'), TypeError);
    assert.throws(() => loader.setCollisionMesh('mesh', [], null, null, null), TypeError);
    assert.throws(() => loader.setVisualMesh('mesh', new DataView(new ArrayBuffer(36)), null, null, null), TypeError);
    assert.throws(() => loader.setVisualMesh('mesh', new Uint8Array(new ArrayBuffer(64), 1, 9), null, null, null), RangeError);
    // Empty names retain native diagnostic precedence over malformed arrays.
    assert.equal(loader.setVisualMesh('', {}, {}, {}, {}), false);
    const points = new Float32Array(native.HEAPU8.buffer, Number(data), 9);
    points.set([0,0,0, 2,0,0, 0,3,0]);
    const indices = new Uint32Array(native.HEAPU8.buffer, Number(data) + 36, 3);
    indices.set([0,1,2]);
    assert.equal(loader.setVisualMesh('triangle', points, null, null, indices), true);
    // A failed replacement must not discard the previous mesh.
    assert.equal(loader.setCollisionMesh('triangle', new Float32Array(3), null, null, null), false);
    native.HEAPU8.fill(0, Number(data), Number(data) + 48);
    const robot = JSON.stringify({name: 'Bot', links: [{name: 'base', visuals: [{meshRef: 'triangle'}]}], joints: []});
    assert.equal(loader.createURDFPhysicsScene(new TextEncoder().encode(robot)), true);
    assert.match(loader.exportAsUSDA(), /\(2, 0, 0\)/);
    assert.match(loader.exportAsUSDA(), /\(0, 3, 0\)/);
    for (const [key, call] of [[opKey, () => loader.createSampleScene()],
      [meshKey, () => loader.setVisualMesh('mesh', new Float32Array(9), null, null, null)]]) {
      let calls = 0;
      const failure = new TypeError('schema dispatch failure');
      const original = native[key];
      native[key] = () => { ++calls; throw failure; };
      try { assert.throws(call, error => error === failure); assert.equal(calls, 1); }
      finally { native[key] = original; }
    }
    native[opKey] = (...args) => { const status = op(...args); loader.delete(); return status; };
    const physics = loader.extractPhysicsSceneJSON();
    assert.equal(JSON.parse(physics).prims.length > 0, true);
    assert.throws(() => loader.clearURDFMeshBuffers(), TypeError);
    assert.throws(() => loader.setVisualMesh('mesh', null, null, null, null), TypeError);
  } finally {
    native[opKey] = op; native[meshKey] = mesh;
    native._lightusd_combined_table_release();
    native._lightusd_combined_free(data);
    if (!loader.isDeleted()) loader.delete();
  }
  const retained = new native.LightUSDLoaderNative();
  try {
    native[meshKey] = (...args) => { const status = mesh(...args); retained.delete(); return status; };
    assert.equal(retained.setCollisionMesh('mesh', new Float32Array(9), null, null, null), true);
  } finally {
    native[meshKey] = mesh;
    if (!retained.isDeleted()) retained.delete();
  }
});

function imageEncodeSnapshot() {
  const loader = new native.LightUSDLoaderNative();
  const result = [];
  try {
    for (const format of ['png', 'bmp', 'tiff', 'dng', 'exr', 'jpeg', 'PNG', '']) {
      for (const channels of [1, 2, 3, 4]) {
        const input = new Uint8Array(2 * 2 * channels);
        for (let i=0;i<input.length;++i) input[i] = (i * 29 + 127) & 255;
        const value = loader.encodeImageNative(input, 2, 2, channels, format);
        result.push({format, channels, value: value instanceof Uint8Array
          ? {length: value.length, sha256: createHash('sha256').update(value).digest('hex')}
          : value, error: loader.error()});
      }
    }
    for (const [width,height,channels,format,input] of [[0,1,4,'png',new Uint8Array()],
      [1,0,4,'png',new Uint8Array()], [65537,1,4,'bad',new Uint8Array()],
      [1,1,0,'png',new Uint8Array()], [1,1,5,'png',new Uint8Array()],
      [2,2,4,'png',new Uint8Array(3)], [1,1,4,'bmp',new Uint8Array([0,0,0,255,1,2,3,4])]]) {
      const value=loader.encodeImageNative(input,width,height,channels,format);
      result.push({width,height,channels,format,value: value instanceof Uint8Array
        ? {length:value.length,sha256:createHash('sha256').update(value).digest('hex')}:value,error:loader.error()});
    }
    return result;
  } finally {loader.delete();}
}

await testAsync('image encoding preserves bytes, format failures and dimension diagnostics', () => {
  const expected = JSON.parse(fs.readFileSync(new URL('./fixtures/combined-image-encoding.json', import.meta.url)));
  assert.deepEqual(imageEncodeSnapshot(), expected[wasm64 ? 'wasm64' : 'wasm32']);
});

await testAsync('image encoding C spans and borrowed output retain boundary safety', () => {
  const loader = new native.LightUSDLoaderNative();
  const pointer = value => wasm64 ? BigInt(value) : Number(value);
  const handle = pointer(loader.$$.ptr), nil = pointer(0);
  const key = '_lightusd_combined_encode_image', encode = native[key];
  const out = native._lightusd_combined_alloc(24);
  const pixels = new Uint8Array([19,127,235,255]);
  try {
    const setSize = size => new DataView(native.HEAPU8.buffer).setUint32(Number(out), size, true);
    setSize(23);
    assert.equal(encode(handle,nil,0,1,1,4,nil,0,out), -1);
    setSize(24);
    assert.equal(encode(nil,nil,0,1,1,4,nil,0,out), -1);
    assert.equal(encode(handle,nil,0,1,1,4,nil,0,nil), -1);
    assert.equal(encode(handle,nil,1,1,1,4,nil,0,out), -1);
    assert.equal(encode(handle,nil,0,1,1,4,nil,1,out), -1);
    assert.equal(encode(handle,nil,0,0,1,4,nil,0,out), 2);
    assert.equal(encode(handle,nil,0,1,1,4,nil,0,out), 0);
    assert.equal(loader.error(), 'Unsupported image format: ');
    for (const value of ['1',1n,{},Infinity,2147483648]) {
      assert.throws(() => loader.encodeImageNative(pixels,value,1,4,'png'), TypeError);
      assert.throws(() => loader.encodeImageNative(pixels,1,value,4,'png'), TypeError);
      assert.throws(() => loader.encodeImageNative(pixels,1,1,value,'png'), TypeError);
    }
    assert.throws(() => loader.encodeImageNative(pixels,1,1,4), TypeError);
    assert.throws(() => loader.encodeImageNative.call({},pixels,1,1,4,'png'), TypeError);
    assert.throws(() => loader.encodeImageNative({},1,1,4,'png'), TypeError);
    assert.throws(() => loader.encodeImageNative(pixels,1,1,4,{}), TypeError);
    let address = 0, size = 0;
    native[key] = (...args) => {
      const status = encode(...args);
      const view = new DataView(native.HEAPU8.buffer, Number(args[8]), 24);
      size = view.getFloat64(8,true); address = view.getFloat64(16,true);
      return status;
    };
    const view = loader.encodeImageNative(pixels,1.9,1,4,new TextEncoder().encode('png'));
    assert.equal(view.buffer, native.HEAPU8.buffer);
    assert.equal(view.byteOffset, address); assert.equal(view.byteLength, size);
    const copy = view.slice();
    assert.deepEqual(copy.subarray(0,8), new Uint8Array([137,80,78,71,13,10,26,10]));
    for (const [badSize,badAddress] of [[-1,0],[1,0],[1,NaN],[1,Number.MAX_SAFE_INTEGER],[Infinity,8]]) {
      native[key] = (...args) => {
        const status = encode(...args);
        const record = new DataView(native.HEAPU8.buffer,Number(args[8]),24);
        record.setFloat64(8,badSize,true); record.setFloat64(16,badAddress,true);
        return status;
      };
      assert.throws(() => loader.encodeImageNative(pixels,1,1,4,'png'), RangeError);
    }
    let calls = 0;
    const failure = new TypeError('image encode failed');
    native[key] = () => { ++calls; throw failure; };
    assert.throws(() => loader.encodeImageNative(pixels,1,1,4,'png'), error => error === failure);
    assert.equal(calls,1);
    native[key] = encode;
    assert.deepEqual(loader.encodeImageNative(pixels,1,1,4,'png').slice(),copy);
    loader.reset();
    assert.deepEqual(copy.subarray(0,8),new Uint8Array([137,80,78,71,13,10,26,10]));
    // Native encoding remains valid through dispatch if the original wrapper
    // is deleted. The returned view then expires with the retained owner.
    native[key] = (...args) => { loader.delete(); return encode(...args); };
    assert.ok(loader.encodeImageNative(pixels,1,1,4,'png') instanceof Uint8Array);
    assert.throws(() => loader.encodeImageNative(pixels,1,1,4,'png'), TypeError);
  } finally {
    native[key] = encode;
    native._lightusd_combined_free(out);
    if (!loader.isDeleted()) loader.delete();
  }
});

console.log(`apply-variant-selection-overload tests done (${wasm64 ? 'wasm64' : 'wasm32'})`);
