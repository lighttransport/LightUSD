import test from 'node:test';
import assert from 'node:assert/strict';
import { validIdentifier, validPrimPath } from '../src/utils.js';
import { templateUSDA } from '../src/templates.js';
import { LuciaUsdSession, findPrimBlock, normalizeInterpolationToken, normalizeMeshNormals, normalizeSubdivisionMetadata, normalizeSubdivisionScheme, setAttributeText, wireMaterialPrimvarParameters } from '../src/usd-session.js';
import { analyzeAsset, inventoryMaterialGraph } from '../src/asset-report.js';
import { LuciaAssistant } from '../src/assistant.js';
import { cleanupMesh, previewCrackMerge, previewPlanarHoleFill } from '../src/mesh-cleanup.js';
import { correctComponentPreview, extractConnectedComponents, extractCorrectedComponents, previewConnectedComponents } from '../src/mesh-components.js';
import { analyzePhysicsMesh, fitPhysicsPrimitives, generateConvexHull } from '../src/physics-analysis.js';
import { expandIndexedAttribute, forEachIndexedTriangle, forEachMeshTriangle, isSupportedNumericArray, normalizeIndexedMesh, remapIndexedVertices } from '../src/indexed-mesh.js';
import { analyzeBindPose, analyzeBindTransforms, analyzeSkeleton, analyzeSkinning, buildInfluenceHeatmap, compareSkinnedDeformation, transferNearestSkinWeights } from '../src/rig-analysis.js';
import { deriveBoundaryLocks, deriveEdgeLocks, deriveUVSeamLocks, expandSharpChains, MAX_SHARP_CHAINS, MAX_SHARP_CHAIN_TEXT_LENGTH, MAX_SHARP_EDGES, parseSharpChainSelection, parseSharpEdgeSelection, parseVertexLockSelection, validateVertexLockMask } from '../src/retopo-locks.js';
import { compareGeometrySnapshots } from '../src/geometry-comparison.js';
import { recomputeVertexTangents } from '../src/tangent-recompute.js';
import { createDependencyManifest, diagnoseUSD, inferInheritedMaterialBindings, localizeUSDDependencies, normalizeResolverPlugins, parseAssistantActivityManifest, repairInheritedMaterialBindings, repairUSDMetadata, validateUSDZArchive } from '../src/usd-doctor.js';
import { analyzeBakeAlpha, convertBakeColor, convertNormalMapY, decodeEmbeddedICCProfile, encodeEmbeddedICCProfile, encodeNormalVectors, encodeProjectedTangentNormals, estimateBakeWorkingBytes, estimateProjectionWorkingBytes, normalizeBakeColorSpace, normalizeBakeColorTransform, packBakeChannels, rasterizeBaseColor, resizeBakeImage, validateBakeResult, BAKE_MEMORY_LIMIT_BYTES } from '../src/texture-bake.js';
import { recomputeFaceVaryingNormals, recomputeVertexNormals, validateSharpEdges } from '../src/normal-recompute.js';
import { evaluateMaterialGraph, materialGraphRootIds, optimizeMaterialGraph, rewriteMaterialGraph } from '../src/material-graph.js';
import { inspectPBRTranslation, translateAuthoredPBRProperties, translateMaterialGraph, translatePBRInputs } from '../src/material-translation.js';
import { materializePrimvarParameterization, materializeVariantParameterization, normalizeParameterizationProfile, planMaterialParameterization } from '../src/material-parameterization.js';
import { findEquivalentMaterialMapping, foldLiteralUSDShaderNodes, mergeMaterialDefinitions, removeUnreachableMaterialShaders, rewriteMaterialBindings, rewriteMaterialCollectionBindings } from '../src/material-repair.js';
import { LuciaProject } from '../src/project.js';
import { LuciaCommandStack, sessionCommand } from '../src/command-stack.js';
import { LuciaOperations, consolidateMaterialGroups, normalizeCleanupOptions, normalizeOperationProgress, normalizeOperationProgressCallback, normalizeOperationMeshGeometry, normalizeUnwrapOptions, postWorkerMessage, remapRetopoCreaseChains, reorderMaterialGroups, transferXatlasMaterialGroups, validateBakeMeshData, validateGeometryData, validateMeshCleanupResult, validateMeshMaterialGroups, validateProjectionResult, validateRetopoResult, validateSkinTransferResult, validateUVAtlasResult, validateUVTransferNormals, validateUVTransferResult, validateWorkerVector } from '../src/operations.js';
import { projectUVs } from '../src/uv-projection.js';
import { collectXatlasTransferBuffers, remapXatlasAttributes, remapXatlasCustomAttributes } from '../src/xatlas-attributes.js';
import { nextSingleAtlasOptions, validateXatlasRequest } from '../src/xatlas-input.js';
import { buildRepairOperationGraph, proposeRepairPlan, repairOperationNeeded, summarizeRepairPlan } from '../src/repair-plan.js';
import { createQualityGate, evaluateTargetProfile, TARGET_PROFILES, validateQualityGate } from '../src/target-profiles.js';
import { createProcessingRecipe, editProcessingRecipe, orderProcessingRecipe, parseProcessingRecipe, serializeProcessingRecipe } from '../src/processing-recipe.js';
import { choosePreviewBudget, previewGeometryDrawCount, previewTextureScale, previewTriangleBudget } from '../src/preview-budget.js';
import { compareFrameSnapshots } from '../src/frame-comparison.js';
import { buildProjectionRays, buildUVIslandOwners, dilateProjectedPixels, mergeTransferredUVs, projectRayHitAttributes, raycastTriangles, sampleProjectedTexture, transferUVsByProjection, transformProjectionNormals, transformProjectionPositions, transformProjectionTangents } from '../src/projection-bake.js';
import { INDEXED_MESH_MAX_INDICES, INDEXED_MESH_MAX_VERTICES, buildIndexedEdgeUses, buildIndexedEdgeUsesForFaces, inspectIndexedMesh, validateIndexedMesh } from '../src/indexed-mesh.js';
import { createReportCache, reportCacheKey } from '../src/report-cache.js';
import { proposeComponentNames, proposeSemanticSuggestions, uniqueComponentName } from '../src/semantic-suggestions.js';
import { materialSnapshotsDiffer } from '../src/render-bridge.js';
import { expandRetopoFaceVaryingMesh, remapRetopoFaceVaryingAttributes } from '../src/retopo-face-varying.js';
test('session commands are atomic across USD and generated assets', async () => {
  let usda = 'before', emitted = 0;
  const session = { exportUSDA: async () => usda, restore: async (source) => { usda = source; } };
  const project = { assets: new Map([['textures/a.png', { bytes: new Uint8Array([1, 2]), metadata: { channel: 'a' } }]]), exportRemap: { 'a.png': 'textures/a.png' }, emit: () => { emitted++; } };
  const stack = new LuciaCommandStack();
  await assert.rejects(() => stack.execute(sessionCommand(session, 'Broken edit', ['/World/Hero'], async () => { usda = 'partially-authored'; project.assets.set('textures/a.png', { bytes: new Uint8Array([9]) }); throw new Error('expected failure'); }, project)), /expected failure/);
  assert.equal(usda, 'before');
  assert.deepEqual([...project.assets.get('textures/a.png').bytes], [1, 2]);
  assert.equal(stack.canUndo, false);

  await stack.execute(sessionCommand(session, 'Valid edit', ['/World/Hero'], async () => { usda = 'after'; project.assets.set('textures/a.png', { bytes: new Uint8Array([3]), metadata: { channel: 'b' } }); return 'incorrect operation return value'; }, project));
  assert.equal(usda, 'after');
  await stack.undo();
  assert.equal(usda, 'before');
  assert.deepEqual([...project.assets.get('textures/a.png').bytes], [1, 2]);
  await stack.redo();
  assert.equal(usda, 'after');
  assert.deepEqual([...project.assets.get('textures/a.png').bytes], [3]);
  assert.ok(emitted >= 2);
});

test('USD identifier and prim path validation', () => {
  assert.equal(validIdentifier('Hero_01'), true);
  assert.equal(validIdentifier('1Hero'), false);
  assert.equal(validIdentifier('Hero/Child'), false);
  assert.equal(validPrimPath('/World/Hero_01'), true);
  assert.equal(validPrimPath('/World/../Secret'), false);
});

test('operation progress is normalized to a bounded accessible payload', () => {
  assert.deepEqual(normalizeOperationProgress({ percentage: 125, message: 'Done' }), { percentage: 100, message: 'Done' });
  assert.deepEqual(normalizeOperationProgress({ percentage: -4, message: '' }), { percentage: 0, message: 'Working…' });
  assert.deepEqual(normalizeOperationProgress({ percentage: 'bad', message: null }, 'Preparing'), { percentage: 0, message: 'Preparing' });
  const updates = []; normalizeOperationProgressCallback((progress) => updates.push(progress), 'Preparing')({ percentage: 150, message: '' });
  assert.deepEqual(updates, [{ percentage: 100, message: 'Preparing' }]);
  assert.equal(normalizeOperationProgressCallback(null), null);
});

test('material parameterization uses canonical target-profile capabilities', () => {
  assert.deepEqual(TARGET_PROFILES['web-viewer'].materialParameterization, { primvar: true, variant: true });
  assert.deepEqual(TARGET_PROFILES.physx.materialParameterization, { primvar: false, variant: false });
  assert.equal(planMaterialParameterization({ targetProfile: 'web-viewer', materials: [{ path: '/A', baseColor: [1, 0, 0] }, { path: '/B', baseColor: [0, 1, 0] }] }).candidates[0].mode, 'primvar');
  const typed = planMaterialParameterization({ targetProfile: 'web-viewer', channels: ['baseColor'], materials: [{ path: '/A', channels: { baseColor: new Float32Array([1, 0, 0]) } }, { path: '/B', channels: { baseColor: new Float32Array([0, 1, 0]) } }] });
  assert.deepEqual(typed.candidates[0].values, [[1, 0, 0], [0, 1, 0]]);
  assert.equal(typed.lossless, true);
  assert.equal(planMaterialParameterization({ targetProfile: 'web-viewer', channels: ['baseColor'], materials: [{ path: '/A', baseColor: [1, 0, Number.NaN] }, { path: '/B', baseColor: [0, 1, 0] }] }).unsupported[0].reason, 'Material does not provide a finite supported channel value.');
});

test('USD interpolation tokens normalize to canonical spellings', () => {
  assert.equal(normalizeInterpolationToken('face_varying'), 'faceVarying');
  assert.equal(normalizeInterpolationToken('uniform'), 'uniform');
  assert.equal(normalizeInterpolationToken('varying'), 'varying');
  assert.equal(normalizeInterpolationToken('instance'), 'instance');
  assert.equal(normalizeInterpolationToken('constant'), 'constant');
  assert.throws(() => normalizeInterpolationToken('element'), /Unsupported interpolation token/);
});

test('asset report cache is deterministic and rejects corrupt or oversized entries', () => {
  const storage = new Map(), adapter = { getItem: (key) => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value), removeItem: (key) => storage.delete(key) };
  const input = { source: 'scene', assets: new Map([['b.png', { bytes: new Uint8Array([2, 3]) }], ['a.png', { bytes: new Uint8Array([1]) }]]) }, key = reportCacheKey(input), report = { stage: { meshes: 0, triangles: 0, vertices: 0, estimatedGpuBytes: 0 }, materials: [], textures: [], issues: [], score: { geometry: 100, materials: 100, textures: 100, physics: 100 } };
  assert.equal(key, reportCacheKey({ source: 'scene', assets: new Map([['a.png', { bytes: new Uint8Array([1]) }], ['b.png', { bytes: new Uint8Array([2, 3]) }]]) }));
  assert.notEqual(reportCacheKey({ source: 'scene', assets: new Map([['a.png', { bytes: new Uint8Array([1, 4]).buffer }]]) }), reportCacheKey({ source: 'scene', assets: new Map([['a.png', { bytes: new Uint8Array([1, 5]).buffer }]]) }));
  assert.notEqual(reportCacheKey({ source: 'scene', assets: new Map([['a.png', { bytes: new Uint8Array([1]), width: 1, height: 1, colorSpace: 'srgb' }]]) }), reportCacheKey({ source: 'scene', assets: new Map([['a.png', { bytes: new Uint8Array([1]), width: 2, height: 1, colorSpace: 'linear' }]]) }));
  const cache = createReportCache(adapter); assert.equal(cache.save(cache.keyFor(input), report), true); assert.deepEqual(cache.load(cache.keyFor(input)), report);
  storage.set(cache.keyFor(input), '{bad'); assert.equal(cache.load(cache.keyFor(input)), null); assert.equal(storage.has(cache.keyFor(input)), false);
  storage.set(cache.keyFor(input), JSON.stringify({ version: 2, key: cache.keyFor(input), report: { stage: {}, materials: [], textures: [], issues: [], score: {} } })); assert.equal(cache.load(cache.keyFor(input)), null);
  storage.set(cache.keyFor(input), JSON.stringify({ version: 2, key: cache.keyFor(input), report: { ...report, stage: { ...report.stage, invalidTexturePixels: 'bad' } } })); assert.equal(cache.load(cache.keyFor(input)), null);
  assert.equal(createReportCache(adapter, { maxBytes: 10 }).save(cache.keyFor(input), report), false);
  const unicodeReport = { ...report, issues: [{ message: '日本語'.repeat(16) }] }, asciiSize = JSON.stringify({ version: 2, key: cache.keyFor(input), report }).length;
  assert.equal(createReportCache(adapter, { maxBytes: asciiSize + 1 }).save(cache.keyFor(input), unicodeReport), false);
  const limited = createReportCache(adapter, { maxEntries: 1 }), second = { source: 'other' }, firstKey = limited.keyFor(input), secondKey = limited.keyFor(second);
  assert.equal(limited.save(firstKey, report), true); assert.equal(limited.save(secondKey, report), true); assert.equal(limited.load(firstKey), null); assert.deepEqual(limited.load(secondKey), report);
  const lru = createReportCache(adapter, { maxEntries: 2 }), third = { source: 'third' }, thirdKey = lru.keyFor(third);
  assert.equal(lru.save(firstKey, report), true); assert.equal(lru.save(secondKey, report), true); assert.deepEqual(lru.load(firstKey), report); assert.equal(lru.save(thirdKey, report), true); assert.equal(lru.load(secondKey), null); assert.deepEqual(lru.load(firstKey), report);
});

test('shared indexed mesh contract normalizes topology and rejects malformed buffers', () => {
  assert.equal(isSupportedNumericArray(new Float32Array()), true);
  assert.equal(isSupportedNumericArray([]), false);
  assert.equal(isSupportedNumericArray({ constructor: Float32Array, length: 0 }), false);
  const normalized = validateIndexedMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) });
  assert.equal(normalized.vertexCount, 3);
  assert.deepEqual([...normalized.indices], [0, 1, 2]);
  const checked = validateIndexedMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), materializeIndices: false });
  assert.equal(checked.indices, null);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array([0, 0, NaN]) }), /finite/);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1]) }), /triangle triplets/);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 3]) }), /in range/);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array(9), maxVertices: 2 }), /safety limit/);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array(9), maxIndices: 2 }), /Generated index count/);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array(3), maxVertices: Number.NaN }), /safety limits/);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array(3), maxIndices: INDEXED_MESH_MAX_INDICES + 1 }), /global bounds/);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array(3), requireTriangles: 1 }), /mode must be boolean/);
  assert.throws(() => validateIndexedMesh({ positions: new Float32Array(3), materializeIndices: 1 }), /materialization mode must be boolean/);
  assert.equal(inspectIndexedMesh({ positions: new Float32Array(3), maxIndices: -1 }).positionsValid, false);
  assert.equal(INDEXED_MESH_MAX_VERTICES > 0, true);
});

test('operations share canonical mesh normalization', () => {
  const position = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const geometry = { attributes: { position: { count: 3, itemSize: 3, array: position } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } };
  const normalized = normalizeOperationMeshGeometry(geometry);
  assert.equal(normalized.positions, position);
  assert.ok(normalized.indices instanceof Uint32Array);
  assert.deepEqual([...normalized.indices], [0, 1, 2]);
  assert.throws(() => normalizeOperationMeshGeometry({ attributes: { position: { count: 3, itemSize: 3, array: position } }, index: { count: 3, array: new Uint8Array([0, 1, 3]) } }), /complete, in-range triangle triplets/);
});

test('shared indexed mesh normalization provides canonical worker buffers', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indexed = normalizeIndexedMesh({ positions });
  assert.equal(indexed.positions, positions);
  assert.ok(indexed.indices instanceof Uint32Array);
  assert.deepEqual([...indexed.indices], [0, 1, 2]);
  const widened = normalizeIndexedMesh({ positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: new Uint16Array([0, 1, 2]) });
  assert.ok(widened.positions instanceof Float32Array);
  assert.ok(widened.indices instanceof Uint32Array);
  assert.deepEqual([...widened.indices], [0, 1, 2]);
});

test('shared indexed mesh normalization checks limits before widening positions', () => {
  assert.throws(() => normalizeIndexedMesh({ positions: [0, 0, 0, 1, 0, 0], maxVertices: 1 }), /safety limit/);
  assert.throws(() => normalizeIndexedMesh({ positions: new Float64Array([1e40, 0, 0]) }), /finite Float32/);
});

test('shared indexed mesh triangle iteration preserves face and corner order', () => {
  const faces = [];
  forEachIndexedTriangle(new Uint16Array([4, 2, 7, 1, 3, 5]), (...triangle) => faces.push(triangle));
  assert.deepEqual(faces, [[0, 4, 2, 7], [1, 1, 3, 5]]);
});

test('shared indexed mesh expansion preserves triangle-corner order and storage type', () => {
  const values = new Float32Array([10, 11, 20, 21, 30, 31]), indices = new Uint16Array([2, 0, 1, 1, 2, 0]);
  assert.deepEqual([...expandIndexedAttribute(values, indices, 2)], [30, 31, 10, 11, 20, 21, 20, 21, 30, 31, 10, 11]);
  assert.equal(expandIndexedAttribute(values, indices, 2).constructor, Float32Array);
  assert.throws(() => expandIndexedAttribute(values, indices, 0), /positive item size/);
  assert.throws(() => expandIndexedAttribute(values, new Uint16Array([3]), 2), /out-of-range/);
  assert.throws(() => expandIndexedAttribute(new Float32Array([1, 2, 3]), indices, 2), /aligned values/);
});

test('shared indexed mesh edge uses preserve face order and undirected keys', () => {
  const edges = buildIndexedEdgeUses(new Uint32Array([0, 1, 2, 2, 1, 3]), { vertexCount: 4 });
  assert.deepEqual(edges.edges.get('1,2'), [0, 1]);
  assert.deepEqual(edges.edges.get('0,1'), [0]);
  assert.deepEqual(edges.edges.get('1,3'), [1]);
  assert.equal(edges.windingConflicts, 0);
  assert.deepEqual([...edges.adjacency[1]].sort((a, b) => a - b), [0, 2, 3]);
  assert.deepEqual(edges.faceAdjacency, [[1], [0]]);
});

test('shared indexed mesh edge uses support selected source faces', () => {
  const edges = buildIndexedEdgeUsesForFaces(new Uint32Array([0, 1, 2, 2, 1, 3]), [1]);
  assert.deepEqual([...edges.entries()], [['1,2', [1]], ['1,3', [1]], ['2,3', [1]]]);
});

test('shared mesh triangle iteration handles implicit topology without allocation', () => {
  const faces = [];
  forEachMeshTriangle(null, 6, (...triangle) => faces.push(triangle));
  assert.deepEqual(faces, [[0, 0, 1, 2], [1, 3, 4, 5]]);
  forEachMeshTriangle(null, 2.5, () => { throw new Error('fractional topology must not be visited'); });
  forEachMeshTriangle(null, Infinity, () => { throw new Error('unbounded topology must not be visited'); });
});

test('shared indexed mesh vertex remap preserves aligned types and order', () => {
  const result = remapIndexedVertices({ positions: [0, 0, 0, 1, 0, 0], usedVertices: [1, 0], attributes: [{ name: 'joint', itemSize: 1, array: new Uint16Array([7, 3]) }] });
  assert.deepEqual([...result.positions], [1, 0, 0, 0, 0, 0]);
  assert.ok(result.attributes[0].array instanceof Uint16Array);
  assert.deepEqual([...result.attributes[0].array], [3, 7]);
});

test('indexed mesh inspection reports malformed input without throwing', () => {
  const result = inspectIndexedMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 9]) });
  assert.equal(result.positionsValid, true);
  assert.equal(result.indicesValid, false);
  assert.equal(inspectIndexedMesh({ positions: new Float32Array([0, 0, Infinity]) }).positionsValid, false);
});

test('USDZ archive validator rejects truncated and unsafe containers', () => {
  assert.throws(() => validateUSDZArchive(new Uint8Array()), /truncated/);
  assert.throws(() => validateUSDZArchive(new Uint8Array(22)), /missing end-of-central-directory/);
});

test('USDZ export passes archive layout validation', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try { await session.loadUSDA('#usda 1.0\n( defaultPrim = "World" )\ndef Xform "World" {}'); const archive = session.exportUSDZ(); assert.ok(validateUSDZArchive(archive).entries.length > 0); }
  finally { session.dispose(); }
});

test('USDZ archive validator rejects local and central name mismatches', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA('#usda 1.0\n( defaultPrim = "World" )\ndef Xform "World" {}');
    const archive = session.exportUSDZ(), view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
    let central = -1;
    for (let offset = 0; offset + 4 <= archive.length; offset++) if (view.getUint32(offset, true) === 0x02014b50) { central = offset; break; }
    assert.notEqual(central, -1);
    const local = view.getUint32(central + 42, true), nameLength = view.getUint16(local + 26, true);
    assert.ok(nameLength > 0);
    archive[local + 30] ^= 1;
    assert.throws(() => validateUSDZArchive(archive), /local and central member names differ/);
  } finally { session.dispose(); }
});

test('USDZ archive validator rejects corrupted member payloads', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA('#usda 1.0\n( defaultPrim = "World" )\ndef Xform "World" {}');
    const archive = session.exportUSDZ(), view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
    let central = -1;
    for (let offset = 0; offset + 4 <= archive.length; offset++) if (view.getUint32(offset, true) === 0x02014b50) { central = offset; break; }
    const local = view.getUint32(central + 42, true), localNameLength = view.getUint16(local + 26, true), localExtraLength = view.getUint16(local + 28, true), payload = local + 30 + localNameLength + localExtraLength;
    archive[payload] ^= 1;
    assert.throws(() => validateUSDZArchive(archive), /member CRC mismatch/);
  } finally { session.dispose(); }
});

test('USDZ archive validator rejects local and central method mismatches', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA('#usda 1.0\n( defaultPrim = "World" )\ndef Xform "World" {}');
    const archive = session.exportUSDZ(), view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
    let central = -1;
    for (let offset = 0; offset + 4 <= archive.length; offset++) if (view.getUint32(offset, true) === 0x02014b50) { central = offset; break; }
    const local = view.getUint32(central + 42, true);
    view.setUint16(local + 8, 8, true);
    assert.throws(() => validateUSDZArchive(archive), /local and central compression flags differ/);
  } finally { session.dispose(); }
});

test('material graph inventory is deterministic and flags unsupported nodes', () => {
  const graph = inventoryMaterialGraph({ userData: { nodes: { output: { type: 'standard_surface', inputs: { surface: true } }, noise: { type: 'Noise 3D', inputs: { scale: true } }, base: { type: 'color' }, utility: { type: 'saturate', inputs: { value: 2 } }, blend: { type: 'lerp' }, edge: { type: 'smoothstep' } } } });
  assert.deepEqual(graph.nodes.map((node) => node.id), ['base', 'blend', 'edge', 'noise', 'output', 'utility']);
  assert.deepEqual(graph.unsupportedNodes, ['noise']);
  assert.deepEqual(graph.bakeableNodes, ['base', 'blend', 'edge', 'utility']);
  assert.deepEqual(graph.unreachableNodes, ['base', 'blend', 'edge', 'noise', 'utility']);
  assert.deepEqual(inventoryMaterialGraph({}), { nodes: [], unsupportedNodes: [], bakeableNodes: [], unresolvedReferences: [] });
  const broken = inventoryMaterialGraph({ userData: { nodes: { output: { type: 'standard_surface', inputs: { surface: { node: 'missing' } } } } } });
  assert.deepEqual(broken.unresolvedReferences, ['output.surface → missing']);
});

test('material graph inventory uses explicit outputs before generic surface helpers', () => {
  const graph = inventoryMaterialGraph({ userData: { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'surface' } } },
    surface: { type: 'surface', inputs: { roughness: { node: 'roughness' } } },
    roughness: { type: 'constant', value: .4 },
    deadSurface: { type: 'surface' },
  } } });
  assert.deepEqual(graph.unreachableNodes, ['deadSurface']);
});

test('material graph inventory recognizes the optimizer arithmetic subset', () => {
  const graph = inventoryMaterialGraph({ userData: { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'clamp' } } },
    clamp: { type: 'clamp', inputs: { value: { node: 'subtract' }, min: 0, max: 1 } },
    subtract: { type: 'subtract', inputs: { a: 1, b: .25 } },
    unsupported: { type: 'noise' },
  } } });
  assert.deepEqual(graph.unsupportedNodes, ['unsupported']);
  assert.deepEqual(graph.bakeableNodes, ['clamp', 'subtract']);
});

test('material graph optimizer folds literals and removes unreachable nodes', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'add' } } },
    add: { type: 'add', inputs: { left: { node: 'a' }, right: { node: 'b' } } },
    a: { type: 'constant', value: [0.2, 0.3, 0.4] },
    b: { type: 'constant', value: [0.1, 0.2, 0.3] },
    dead: { type: 'multiply', inputs: { left: 2, right: 4 } },
  } });
  assert.deepEqual(result.folded, ['a', 'add', 'b']);
  assert.deepEqual(result.removed, ['dead']);
  assert.deepEqual(result.nodes.find((node) => node.id === 'add').value.map((value) => Number(value.toFixed(6))), [0.3, 0.5, 0.7]);
  assert.deepEqual(result.nodes.find((node) => node.id === 'add').inputs, {});
  assert.equal(result.nodes.some((node) => node.id === 'dead'), false);
});

test('material graph root selection preserves numeric zero IDs', () => {
  assert.deepEqual(materialGraphRootIds(new Map([[0, { type: 'output', inputs: {} }], [1, { type: 'surface', inputs: {} }]])), ['0']);
  assert.deepEqual(optimizeMaterialGraph(new Map([[0, { type: 'output', inputs: { surface: { node: 1 } } }], [1, { type: 'surface', inputs: { roughness: 0.5 } }], [2, { type: 'surface', inputs: {} }]])).removed, ['2']);
});

test('material graph optimizer removes disconnected surface helpers when output roots exist', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'surface' } } },
    surface: { type: 'surface', inputs: { roughness: { node: 'roughness' } } },
    roughness: { type: 'constant', value: .4 },
    deadSurface: { type: 'surface', inputs: { roughness: 1 } },
  } });
  assert.deepEqual(result.removed, ['deadSurface']);
  assert.equal(result.nodes.some((node) => node.id === 'deadSurface'), false);
  assert.equal(result.nodes.some((node) => node.id === 'surface'), true);
});

test('material graph root selection accepts serialized node maps and arrays', () => {
  const nodes = { output: { type: 'output' }, surface: { type: 'surface' }, helper: { type: 'custom' } };
  assert.deepEqual(materialGraphRootIds(nodes), ['output']);
  assert.deepEqual(materialGraphRootIds(Object.entries(nodes).map(([id, node]) => ({ ...node, id }))), ['output']);
  assert.deepEqual(materialGraphRootIds(new Map(Object.entries(nodes))), ['output']);
  assert.deepEqual(materialGraphRootIds([{ type: 'output' }, { type: 'surface' }]), ['node:0']);
});

test('material graph collections preserve numeric zero node IDs', () => {
  const nodes = new Map([[0, { id: 0, type: 'output' }], [1, { id: 1, type: 'surface' }]]);
  assert.deepEqual(materialGraphRootIds(nodes), ['0']);
  assert.equal(optimizeMaterialGraph({ nodes }).nodes[0].id, '0');
  const translated = translateMaterialGraph({ nodes }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.deepEqual(translated.nodes.map((node) => node.id), ['0', '1']);
});

test('material graph optimizer accepts parameterized serialized nodes', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', parameters: { surface: { node: 'sum' } } },
    sum: { type: 'add', parameters: { a: { node: 'one' }, b: 2 } },
    one: { type: 'constant', value: 1 },
  } });
  const sum = result.nodes.find((node) => node.id === 'sum');
  assert.equal(sum.type, 'constant');
  assert.equal(sum.value, 3);
  assert.deepEqual(sum.parameters, {});
  assert.equal(sum.inputs, undefined);
});

test('material graph rewrite preserves the parameters container', () => {
  const result = rewriteMaterialGraph({ nodes: {
    output: { type: 'output', parameters: { surface: { node: 'sum' } } },
    sum: { type: 'add', parameters: { a: 1, b: 2 } },
  } });
  const output = result.nodes.find((node) => node.id === 'output');
  assert.deepEqual(output.parameters, { surface: 3 });
  assert.equal(output.inputs, undefined);
});

test('material graph rewrite reports removed folded inputs deterministically', () => {
  const result = rewriteMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'sum' } } },
    sum: { type: 'add', inputs: { b: 2, a: { node: 'one' } } },
    one: { type: 'constant', value: 1 },
  } });
  assert.deepEqual(result.removedInputs, [{ nodeId: 'sum', inputs: ['a', 'b'] }]);
});

test('material graph optimizer and evaluator accept Map-backed input containers', () => {
  const graph = { nodes: new Map([
    ['output', { type: 'output', inputs: new Map([['surface', { node: 'sum' }]]) }],
    ['sum', { type: 'add', inputs: new Map([['a', 1], ['b', 2]]) }],
  ]) };
  const optimized = optimizeMaterialGraph(graph);
  assert.equal(optimized.nodes.find((node) => node.id === 'sum').value, 3);
  assert.equal(evaluateMaterialGraph(graph).value, 3);
});

test('material graph evaluation fails closed for malformed input containers', () => {
  const graph = { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'sum' } } },
    sum: { type: 'add', inputs: 'not-a-map', parameters: { a: 1, b: 2 } },
  } };
  const optimized = optimizeMaterialGraph(graph);
  assert.equal(optimized.nodes.find((node) => node.id === 'sum').type, 'add');
  const evaluated = evaluateMaterialGraph(graph);
  assert.equal(evaluated.supported, false);
  assert.deepEqual(evaluated.unsupportedNodes, ['sum']);
});

test('material graph optimizer and evaluator fail closed for malformed node collections', () => {
  assert.deepEqual(optimizeMaterialGraph({ nodes: 'not-a-node-map' }), { nodes: [], folded: [], removed: [], changed: false });
  assert.deepEqual(evaluateMaterialGraph({ nodes: 42 }), { value: undefined, nodeId: null, supported: false, unsupportedNodes: [], unresolvedReferences: [], reason: 'No output or surface root was found.' });
});

test('material graph optimizer does not fold incompatible vector shapes', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'add' } } },
    add: { type: 'add', inputs: { left: { node: 'a' }, right: { node: 'b' } } },
    a: { type: 'constant', value: [1, 2, 3] },
    b: { type: 'constant', value: [4, 5] },
  } });
  assert.equal(result.folded.includes('add'), false);
  assert.deepEqual(result.nodes.find((node) => node.id === 'add').inputs, { left: { node: 'a' }, right: { node: 'b' } });
});

test('material graph optimizer folds finite typed-array literals', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'standard_surface', inputs: { base: { node: 'add' } } },
    add: { type: 'add', inputs: { left: { node: 'a' }, right: { node: 'b' } } },
    a: { type: 'constant', value: new Float32Array([.1, .2, .3]) },
    b: { type: 'constant', value: new Float32Array([.2, .3, .4]) },
  } });
  const folded = result.nodes.find((node) => node.id === 'add');
  assert.deepEqual(folded.value.map((value) => Number(value.toFixed(5))), [.3, .5, .7]);
  assert.equal(folded.type, 'constant');
  assert.deepEqual(result.nodes.find((node) => node.id === 'a').value.map((value) => Number(value.toFixed(6))), [.1, .2, .3]);
});

test('material graph optimizer folds bounded scalar arithmetic and unary nodes', () => {
  const result = optimizeMaterialGraph({ nodes: {
    value: { type: 'constant', value: 2 },
    offset: { type: 'constant', value: 5 },
    subtract: { type: 'subtract', inputs: { a: { node: 'offset' }, b: { node: 'value' } } },
    clamp: { type: 'clamp', inputs: { value: { node: 'subtract' }, min: -1, max: 1 } },
    output: { type: 'surface', inputs: { roughness: { node: 'clamp' } } },
    dead: { type: 'divide', inputs: { a: 1, b: 0 } },
  } });
  const output = result.nodes.find((node) => node.id === 'clamp');
  assert.equal(output.type, 'constant');
  assert.equal(output.value, 1);
  assert.ok(result.folded.includes('subtract'));
  assert.ok(result.folded.includes('clamp'));
  assert.ok(result.removed.includes('dead'));
});

test('material graph optimizer folds common literal utility nodes', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'saturate' } } },
    saturate: { type: 'saturate', inputs: { value: { node: 'maximum' } } },
    maximum: { type: 'max', inputs: { a: { node: 'abs' }, b: -.25 } },
    abs: { type: 'abs', inputs: { value: { node: 'sqrt' } } },
    sqrt: { type: 'sqrt', inputs: { value: 4 } },
    minimum: { type: 'min', inputs: { a: 1, b: 2 } },
    floor: { type: 'floor', inputs: { value: 1.9 } },
  } });
  assert.equal(result.nodes.find((node) => node.id === 'saturate').value, 1);
  assert.equal(result.nodes.find((node) => node.id === 'maximum').value, 2);
  assert.equal(result.nodes.find((node) => node.id === 'sqrt').value, 2);
  assert.ok(result.removed.includes('minimum'));
  assert.ok(result.removed.includes('floor'));
  assert.ok(result.folded.includes('abs'));
});

test('material graph optimizer folds extended unary utility nodes', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'fract' }, sign: { node: 'sign' }, round: { node: 'round' }, exp: { node: 'exp' }, log: { node: 'log' }, aliased: { node: 'aliased' }, invalid: { node: 'invalid' } } },
    sign: { type: 'sign', inputs: { value: -2 } },
    round: { type: 'round', inputs: { value: 1.6 } },
    fract: { type: 'fract', inputs: { value: 2.75 } },
    exp: { type: 'exp', inputs: { value: 0 } },
    log: { type: 'log', inputs: { value: 1 } },
    aliased: { type: 'sign', inputs: { metadata: 99, input: -3 } },
    invalid: { type: 'log', inputs: { value: 0 } },
  } });
  assert.ok(result.folded.includes('sign'));
  assert.ok(result.folded.includes('round'));
  assert.equal(result.nodes.find((node) => node.id === 'fract').value, 0.75);
  assert.ok(result.folded.includes('exp'));
  assert.ok(result.folded.includes('log'));
  assert.equal(result.nodes.find((node) => node.id === 'aliased').value, -1);
  assert.equal(result.nodes.find((node) => node.id === 'invalid').type, 'log');
});

test('material graph optimizer folds reciprocal and inverse utilities', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'reciprocal' }, inverse: { node: 'inverse' }, invalid: { node: 'invalid' } } },
    reciprocal: { type: 'reciprocal', inputs: { value: 4 } },
    inverse: { type: 'inverse', inputs: { input: [2, 4] } },
    invalid: { type: 'reciprocal', inputs: { value: 0 } },
  } });
  assert.deepEqual(result.nodes.find((node) => node.id === 'reciprocal').value, 0.25);
  assert.deepEqual(result.nodes.find((node) => node.id === 'inverse').value, [0.5, 0.25]);
  assert.equal(result.nodes.find((node) => node.id === 'invalid').type, 'reciprocal');
});

test('material graph optimizer folds trigonometric utilities', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'sin' }, cos: { node: 'cos' }, tan: { node: 'tan' }, invalid: { node: 'tanInvalid' } } },
    sin: { type: 'sin', inputs: { value: Math.PI / 2 } },
    cos: { type: 'cos', inputs: { input: 0 } },
    tan: { type: 'tan', inputs: { x: Math.PI / 4 } },
    tanInvalid: { type: 'tan', inputs: { value: Infinity } },
  } });
  assert.ok(Math.abs(result.nodes.find((node) => node.id === 'sin').value - 1) < 1e-12);
  assert.equal(result.nodes.find((node) => node.id === 'cos').value, 1);
  assert.ok(Math.abs(result.nodes.find((node) => node.id === 'tan').value - 1) < 1e-12);
  assert.equal(result.nodes.find((node) => node.id === 'tanInvalid').type, 'tan');
});

test('material graph optimizer folds finite vector utility nodes', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'surface' } } },
    surface: { type: 'standard_surface', inputs: { roughness: { node: 'length' }, normal: { node: 'dot' } } },
    dot: { type: 'dot', inputs: { a: { node: 'normal' }, b: { node: 'axis' } } },
    normal: { type: 'normalize', inputs: { value: [3, 0, 4] } },
    axis: { type: 'cross', inputs: { a: [1, 0, 0], b: [0, 1, 0] } },
    length: { type: 'length', inputs: { value: [3, 4] } },
  } });
  assert.deepEqual(result.nodes.find((node) => node.id === 'normal').value.map((value) => Number(value.toFixed(5))), [.6, 0, .8]);
  assert.deepEqual(result.nodes.find((node) => node.id === 'axis').value, [0, 0, 1]);
  assert.equal(result.nodes.find((node) => node.id === 'dot').value, .8);
  assert.equal(result.nodes.find((node) => node.id === 'length').value, 5);
  assert.deepEqual(result.folded, ['axis', 'dot', 'length', 'normal']);
});

test('material graph rewrite inlines folded values and preserves output nodes', () => {
  const source = { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'sum' } } },
    sum: { type: 'add', inputs: { a: { node: 'one' }, b: 2 } },
    one: { type: 'constant', value: 1 },
    unused: { type: 'constant', value: 9 },
  } };
  const result = rewriteMaterialGraph(source);
  assert.deepEqual(result.nodes, [{ id: 'output', type: 'output', inputs: { surface: 3 } }]);
  assert.deepEqual(result.inlined, ['one', 'sum']);
  assert.deepEqual(source.nodes.output.inputs.surface, { node: 'sum' });
});

test('material graph rewrite deep-clones retained opaque inputs', () => {
  const connection = { node: 'external', metadata: { channel: 'roughness' } }, source = { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'unknown' } } },
    unknown: { type: 'custom', inputs: { source: connection } },
  } };
  const result = rewriteMaterialGraph(source), retained = result.nodes.find((node) => node.id === 'unknown');
  retained.inputs.source.metadata.channel = 'changed';
  assert.deepEqual(source.nodes.unknown.inputs.source, connection);
});

test('material graph optimizer broadcasts vector mix factors', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'mix' } } },
    mix: { type: 'mix', inputs: { a: [0, 0, 0], b: [1, 1, 1], factor: [0, .5, 1] } },
  } });
  assert.deepEqual(result.nodes.find((node) => node.id === 'mix').value, [0, .5, 1]);
});

test('material graph optimizer resolves clamp and mix inputs by semantic names', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'mix' }, clamped: { node: 'clamp' } } },
    mix: { type: 'mix', inputs: { factor: .25, b: 1, a: 0 } },
    clamp: { type: 'clamp', inputs: { max: .8, value: 2, min: 0 } },
  } });
  assert.equal(result.nodes.find((node) => node.id === 'mix').value, .25);
  assert.equal(result.nodes.find((node) => node.id === 'clamp').value, .8);
});

test('material graph optimizer resolves binary inputs by semantic names', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'subtract' }, quotient: { node: 'divide' }, exponent: { node: 'power' } } },
    subtract: { type: 'subtract', inputs: { b: 3, a: 10 } },
    divide: { type: 'divide', inputs: { right: 4, left: 12 } },
    power: { type: 'power', inputs: { input2: 3, input1: 2 } },
  } });
  assert.equal(result.nodes.find((node) => node.id === 'subtract').value, 7);
  assert.equal(result.nodes.find((node) => node.id === 'divide').value, 3);
  assert.equal(result.nodes.find((node) => node.id === 'power').value, 8);
});

test('material graph optimizer folds lerp and smoothstep utilities', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'smooth' }, lerped: { node: 'lerp' } } },
    smooth: { type: 'smoothstep', inputs: { x: .5, edge1: 1, edge0: 0 } },
    lerp: { type: 'lerp', inputs: { t: .25, input2: 1, input1: 0 } },
    invalid: { type: 'smoothstep', inputs: { edge0: 1, edge1: 1, x: .5 } },
  } });
  assert.equal(result.nodes.find((node) => node.id === 'smooth').value, .5);
  assert.equal(result.nodes.find((node) => node.id === 'lerp').value, .25);
  assert.ok(result.removed.includes('invalid'));
});

test('material graph optimizer folds scalar select and if nodes', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'selected' }, alternate: { node: 'conditional' }, boolean: { node: 'booleanSelect' }, malformed: { node: 'vectorCondition' } } },
    selected: { type: 'select', inputs: { condition: 1, true: [1, 2, 3], false: [9, 9, 9] } },
    conditional: { type: 'if', inputs: { cond: 0, then: 4, else: 7 } },
    booleanSelect: { type: 'select', inputs: { condition: true, true: 2, false: 3 } },
    vectorCondition: { type: 'select', inputs: { condition: [1], true: 1, false: 2 } },
  } });
  assert.deepEqual(result.nodes.find((node) => node.id === 'selected').value, [1, 2, 3]);
  assert.equal(result.nodes.find((node) => node.id === 'conditional').value, 7);
  assert.equal(result.nodes.find((node) => node.id === 'booleanSelect').value, 2);
  assert.equal(result.nodes.find((node) => node.id === 'vectorCondition').type, 'select');
});

test('material graph optimizer folds scalar comparison branches', () => {
  const result = optimizeMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'greater' }, less: { node: 'less' }, malformed: { node: 'vectorCompare' } } },
    greater: { type: 'ifgreater', inputs: { value1: 3, value2: 2, ifTrue: [1, 0, 0], ifFalse: [0, 1, 0] } },
    less: { type: 'ifless', inputs: { a: 1, b: 2, then: 4, else: 8 } },
    vectorCompare: { type: 'ifequal', inputs: { value1: [1], value2: [1], ifTrue: 1, ifFalse: 2 } },
  } });
  assert.deepEqual(result.nodes.find((node) => node.id === 'greater').value, [1, 0, 0]);
  assert.equal(result.nodes.find((node) => node.id === 'less').value, 4);
  assert.equal(result.nodes.find((node) => node.id === 'vectorCompare').type, 'ifequal');
});

test('material graph evaluator returns literal connected output and fails closed', () => {
  const graph = { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'sum' } } },
    sum: { type: 'add', inputs: { a: 1, b: 2 } },
  } };
  assert.deepEqual(evaluateMaterialGraph(graph), { value: 3, nodeId: 'output', supported: true, unsupportedNodes: [], unresolvedReferences: [] });
  const unsupported = evaluateMaterialGraph({ nodes: { output: { type: 'output', inputs: { surface: { node: 'image' } } }, image: { type: 'image' } } });
  assert.equal(unsupported.supported, false);
  assert.deepEqual(unsupported.unsupportedNodes, ['image']);
});

test('material graph evaluator follows output surface connections by channel', () => {
  const graph = { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'surface' } } },
    surface: { type: 'standard_surface', inputs: { roughness: { node: 'roughness' }, base_color: { node: 'base' } } },
    roughness: { type: 'float', value: .35 },
    base: { type: 'color', value: [1, 0, 0] },
  } };
  assert.equal(evaluateMaterialGraph(graph, { output: 'roughness' }).value, .35);
  assert.deepEqual(evaluateMaterialGraph(graph, { output: 'baseColor' }).value, [1, 0, 0]);
});

test('material graph evaluator accepts typed literal nodes', () => {
  const result = evaluateMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'color' } } },
    color: { type: 'color', value: new Float32Array([.1, .2, .3]) },
  } });
  assert.deepEqual(result.value, [.1, .2, .3].map((value) => new Float32Array([value])[0]));
  assert.equal(result.supported, true);
});

test('material graph evaluator accepts validated texture resolver values', () => {
  const graph = { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'texture' } } },
    texture: { type: 'texture', path: 'albedo.png' },
  } };
  let requested;
  const result = evaluateMaterialGraph(graph, { resolveTexture(node, id) { requested = [node.path, id]; return [.2, .4, .6]; } });
  assert.deepEqual(requested, ['albedo.png', 'texture']);
  assert.deepEqual(result.value, [.2, .4, .6]);
  assert.equal(result.supported, true);
  assert.equal(evaluateMaterialGraph(graph).supported, false);
});

test('material graph evaluator resolves each texture input once', () => {
  let calls = 0;
  const graph = { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'texture' } } },
    texture: { type: 'texture', path: 'albedo.png' },
  } };
  const result = evaluateMaterialGraph(graph, { resolveTexture() { calls++; return [.2, .4, .6]; } });
  assert.equal(result.supported, true);
  assert.equal(calls, 1);
});

test('material graph texture resolvers cannot mutate the source node snapshot', () => {
  const graph = { nodes: { output: { type: 'output', inputs: { surface: { node: 'texture' } } }, texture: { type: 'texture', inputs: { uv: { set: 'st' } } } } };
  evaluateMaterialGraph(graph, { resolveTexture(node) { node.inputs.uv.set = 'mutated'; node.inputs.extra = true; return 1; } });
  assert.deepEqual(graph.nodes.texture.inputs, { uv: { set: 'st' } });
});

test('material graph evaluator fails closed for cyclic texture metadata', () => {
  const metadata = { source: 'albedo.png' };
  metadata.self = metadata;
  const result = evaluateMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'texture' } } },
    texture: { type: 'texture', metadata },
  } }, { resolveTexture() { return 1; } });
  assert.equal(result.supported, false);
  assert.deepEqual(result.unsupportedNodes, ['texture']);
});

test('material graph evaluator returns channel maps for shader roots', () => {
  const result = evaluateMaterialGraph({ nodes: {
    surface: { type: 'standard_surface', inputs: { base_color: { node: 'color' }, roughness: .35 } },
    color: { type: 'color', value: [1, .5, 0] },
  } });
  assert.deepEqual(result.value, { base_color: [1, .5, 0], roughness: .35 });
  assert.equal(result.supported, true);
});

test('material graph evaluator prioritizes explicit output roots', () => {
  const result = evaluateMaterialGraph({ nodes: {
    material: { type: 'standard_surface', inputs: { base_color: .1 } },
    output: { type: 'output', inputs: { surface: { node: 'value' } } },
    value: { type: 'constant', value: .8 },
  } });
  assert.equal(result.nodeId, 'output');
  assert.equal(result.value, .8);
});

test('material graph evaluator isolates a requested shader channel', () => {
  const result = evaluateMaterialGraph({ nodes: {
    surface: { type: 'standard_surface', inputs: { roughness: .35, normal: { node: 'unsupported-texture' } } },
    'unsupported-texture': { type: 'image' },
  } }, { output: 'roughness' });
  assert.equal(result.value, .35);
  assert.equal(result.supported, true);
});

test('material graph evaluator bounds excessive graph depth', () => {
  const nodes = { output: { type: 'output', inputs: { surface: { node: 'n2' } } }, n0: { type: 'add', inputs: { a: 1, b: 2 } }, n1: { type: 'add', inputs: { a: { node: 'n0' }, b: 2 } }, n2: { type: 'add', inputs: { a: { node: 'n1' }, b: 3 } } };
  const result = evaluateMaterialGraph({ nodes }, { maxDepth: 1 });
  assert.equal(result.supported, false);
  assert.deepEqual(result.unsupportedNodes, ['n0', 'n1', 'n2']);
});

test('material graph evaluator bounds visited node count', () => {
  const result = evaluateMaterialGraph({ nodes: {
    output: { type: 'output', inputs: { surface: { node: 'sum' } } },
    sum: { type: 'add', inputs: { a: { node: 'left' }, b: { node: 'right' } } },
    left: { type: 'constant', value: 1 }, right: { type: 'constant', value: 2 },
  } }, { maxNodes: 2 });
  assert.equal(result.supported, false);
  assert.ok(result.unsupportedNodes.length > 0);
});

test('PBR translation preserves the portable channel subset and reports loss', () => {
  const result = translatePBRInputs({ from: 'MaterialX Standard Surface', to: 'UsdPreviewSurface', inputs: { base_color: [0.2, 0.3, 0.4], metalness: 0.7, specular_roughness: 0.4, opacity: 0.9, custom_lobe: 1 } });
  assert.deepEqual(result.inputs, { diffuseColor: [0.2, 0.3, 0.4], metallic: 0.7, roughness: 0.4, opacity: 0.9 });
  assert.deepEqual(result.unsupported, ['custom_lobe']);
  assert.equal(result.approximations.length, 3);
  const loss = result.differences.find((item) => item.input === 'custom_lobe');
  assert.equal(loss.output, null);
  assert.match(loss.reason, /No portable target input/);
  assert.equal(inspectPBRTranslation({ from: 'usd-preview-surface', to: 'metallic-roughness', inputs: { diffuseColor: [1, 1, 1], specularColor: [1, 1, 1] } }).lossless, false);
  assert.throws(() => translatePBRInputs({ from: 'unknown', to: 'metallic-roughness', inputs: {} }), /Unsupported PBR material format/);
  assert.throws(() => translatePBRInputs(null), /Unsupported PBR material format/);
  assert.throws(() => inspectPBRTranslation(null), /Unsupported PBR material format/);
});

test('PBR translation uses canonical aliases independent of input order', () => {
  const first = translatePBRInputs({ from: 'metallic-roughness', to: 'usd-preview-surface', inputs: { base_color: [0.1, 0.2, 0.3], baseColor: [0.8, 0.7, 0.6] } });
  const second = translatePBRInputs({ from: 'metallic-roughness', to: 'usd-preview-surface', inputs: { baseColor: [0.8, 0.7, 0.6], base_color: [0.1, 0.2, 0.3] } });
  assert.deepEqual(first, second);
  assert.deepEqual(first.inputs, { diffuseColor: [0.8, 0.7, 0.6] });
  assert.deepEqual(first.approximations.find((item) => item.input === 'base_color'), { input: 'base_color', output: 'diffuseColor', reason: 'Equivalent alias was ignored in favor of the canonical input; review conflicting authored values.' });
});

test('material graph translation preserves connections and reports unsupported channels', () => {
  const result = translateMaterialGraph({ nodes: {
    shader: { type: 'standard_surface', inputs: { base_color: { node: 'texture' }, specular: .5, roughness: .4 } },
    texture: { type: 'image', inputs: { file: 'albedo.png' } },
  } }, { from: 'materialx-standard-surface', to: 'metallic-roughness' });
  const shader = result.nodes.find((node) => node.id === 'shader');
  assert.equal(shader.type, 'metallic_roughness');
  assert.deepEqual(shader.inputs, { baseColor: { node: 'texture' }, roughness: .4, specular: .5 });
  assert.deepEqual(result.unsupported, ['shader.specular']);
  assert.ok(result.changedNodes.includes('shader.base_color → baseColor'));
});

test('material graph translation preserves the parameters container variant', () => {
  const result = translateMaterialGraph({ nodes: {
    shader: { type: 'usdpreviewsurface', parameters: { diffuseColor: { node: 'color' }, metallic: .8 } },
    color: { type: 'color', value: [1, 0, 0] },
  } }, { from: 'usd-preview-surface', to: 'materialx-standard-surface' });
  const shader = result.nodes.find((node) => node.id === 'shader');
  assert.deepEqual(shader.parameters, { base_color: { node: 'color' }, metalness: .8 });
  assert.equal(shader.inputs, undefined);
});

test('material graph translation accepts Map-backed input containers', () => {
  const result = translateMaterialGraph({ nodes: new Map([
    ['shader', { type: 'standard_surface', inputs: new Map([['base_color', [1, 0, 0]]]) }],
  ]) }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.deepEqual(result.nodes[0].inputs, { diffuseColor: [1, 0, 0] });
});

test('material graph translation accepts Map node collections', () => {
  const nodes = new Map([
    ['shader', { type: 'standard_surface', inputs: { base_color: [1, 0, 0] } }],
  ]);
  const result = translateMaterialGraph({ nodes }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.deepEqual(result.nodes, [{ id: 'shader', type: 'usdpreviewsurface', inputs: { diffuseColor: [1, 0, 0] } }]);
});

test('material graph translation recognizes MaterialX shader implementation IDs', () => {
  const result = translateMaterialGraph({ nodes: {
    shader: { type: 'ND_standard_surface_surfaceshader', inputs: { base_color: [1, 0, 0] } },
  } }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.equal(result.nodes[0].type, 'usdpreviewsurface');
  assert.deepEqual(result.nodes[0].inputs, { diffuseColor: [1, 0, 0] });
  assert.ok(result.changedNodes.includes('shader.type → usdpreviewsurface'));
});

test('material graph translation updates the existing node discriminator field', () => {
  const result = translateMaterialGraph({ nodes: {
    shader: { nodeType: 'standard_surface', inputs: { base_color: [1, 0, 0] } },
  } }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.equal(result.nodes[0].nodeType, 'usdpreviewsurface');
  assert.equal(result.nodes[0].type, undefined);
  assert.ok(result.changedNodes.includes('shader.nodeType → usdpreviewsurface'));
});

test('material graph translation preserves numeric zero IDs in arrays', () => {
  const result = translateMaterialGraph({ nodes: [{ id: 0, type: 'standard_surface', inputs: { roughness: .5 } }] }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.equal(result.nodes[0].id, '0');
});

test('material graph translation ignores malformed node and input collections', () => {
  const malformed = translateMaterialGraph({ nodes: 'not-a-node-map' }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.deepEqual(malformed.nodes, []);
  const result = translateMaterialGraph({ nodes: { shader: { type: 'standard_surface', inputs: ['base_color'] } } }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.deepEqual(result.nodes[0].inputs, ['base_color']);
  assert.deepEqual(result.changedNodes, ['shader.type → usdpreviewsurface']);
});

test('material graph translation deep-clones opaque connections', () => {
  const connection = { node: 'texture', swizzle: ['r', 'g'] }, source = { nodes: {
    shader: { type: 'standard_surface', inputs: { base_color: connection } },
    opaque: { type: 'image', inputs: { file: connection } },
  } };
  const result = translateMaterialGraph(source, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  const shader = result.nodes.find((node) => node.id === 'shader'), opaque = result.nodes.find((node) => node.id === 'opaque');
  shader.inputs.diffuseColor.swizzle[0] = 'b';
  opaque.inputs.file.node = 'changed';
  assert.deepEqual(connection, { node: 'texture', swizzle: ['r', 'g'] });
  assert.deepEqual(source.nodes.opaque.inputs.file, connection);
});

test('material graph translation rejects cyclic opaque values', () => {
  const connection = { node: 'texture' };
  connection.self = connection;
  assert.throws(() => translateMaterialGraph({ nodes: { shader: { type: 'standard_surface', inputs: { base_color: connection } } } }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' }), /acyclic/);
});

test('material graph translation rejects non-plain opaque values', () => {
  const connection = new Map([['node', 'texture']]);
  assert.throws(() => translateMaterialGraph({ nodes: { shader: { type: 'standard_surface', inputs: { base_color: connection } } } }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' }), /plain objects/);
});

test('material graph optimization handles deep serialized graphs without recursion overflow', () => {
  const nodes = { output: { type: 'output', inputs: { surface: { node: 'node-19999' } } } };
  for (let index = 0; index < 20000; index++) nodes[`node-${index}`] = { type: 'add', inputs: { a: index ? { node: `node-${index - 1}` } : 1, b: 1 } };
  const result = optimizeMaterialGraph({ nodes });
  assert.equal(result.nodes.length, 20001);
  assert.equal(result.folded.length, 20000);
  assert.equal(result.removed.length, 0);
});

test('material graph translation orders array nodes deterministically', () => {
  const result = translateMaterialGraph({ nodes: [
    { id: 'z-shader', type: 'standard_surface', inputs: { roughness: .4 } },
    { id: 'a-shader', type: 'standard_surface', inputs: { roughness: .2 } },
  ] }, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.deepEqual(result.nodes.map((node) => node.id), ['a-shader', 'z-shader']);
});

test('PBR translation rejects malformed numeric channel values', () => {
  const result = translatePBRInputs({ from: 'materialx-standard-surface', to: 'usd-preview-surface', inputs: { base_color: [1, 2, 3, 4, 5], metallic: Number.NaN, roughness: 0.5 } });
  assert.deepEqual(result.inputs, { roughness: 0.5 });
  assert.deepEqual(result.unsupported, ['base_color', 'metallic']);
  assert.equal(result.differences.find((item) => item.input === 'metallic').reason, 'Input is not a finite scalar or 1–4 component numeric vector; the contribution is not authored.');
});

test('material parameterization plans deterministic primvar candidates', () => {
  assert.equal(normalizeParameterizationProfile('WEB-VIEWER'), 'web-viewer');
  const result = planMaterialParameterization({ targetProfile: 'web-viewer', materials: [
    { path: '/World/Looks/Blue', channels: { baseColor: [.1, .2, .3], roughness: .4 } },
    { path: '/World/Looks/Red', channels: { baseColor: [.8, .1, .05], roughness: .4 } },
  ] });
  assert.deepEqual(result.candidates.map(({ channel, mode, parameter, materialPaths, distinctValueCount }) => ({ channel, mode, parameter, materialPaths, distinctValueCount })), [{ channel: 'baseColor', mode: 'primvar', parameter: 'primvars:lucia:baseColor', materialPaths: ['/World/Looks/Blue', '/World/Looks/Red'], distinctValueCount: 2 }]);
  assert.equal(result.lossless, true);
  const colliderPlan = planMaterialParameterization({ targetProfile: 'physx', materials: [{ path: '/Mat/A', channels: { baseColor: [1, 0, 0] } }, { path: '/Mat/B', channels: { baseColor: [0, 1, 0] } }] });
  assert.equal(colliderPlan.candidates.length, 0);
  assert.match(colliderPlan.unsupported[0].reason, /does not support material parameterization/);
  assert.throws(() => planMaterialParameterization({ targetProfile: 'unknown', materials: [] }), /Unsupported material parameterization profile/);
});

test('material parameterization materializes reviewed values as face-varying primvars', () => {
  const plan = planMaterialParameterization({ targetProfile: 'web-viewer', channels: ['baseColor'], materials: [
    { path: '/Mat/Red', channels: { baseColor: [1, 0, 0] } },
    { path: '/Mat/Blue', channels: { baseColor: [0, 0, 1] } },
  ] });
  const attribute = materializePrimvarParameterization({ candidate: plan.candidates[0], materialPaths: ['/Mat/Blue', '/Mat/Red'], faceMaterialIndices: new Uint32Array([1, 1, 0]) });
  assert.equal(attribute.name, 'lucia:baseColor');
  assert.equal(attribute.itemSize, 3);
  assert.deepEqual([...attribute.array], [1, 0, 0, 1, 0, 0, 0, 0, 1]);
  assert.deepEqual([...attribute.indices], [0, 1, 2]);
  assert.throws(() => materializePrimvarParameterization({ candidate: { ...plan.candidates[0], mode: 'variant' }, materialPaths: ['/Mat/Blue', '/Mat/Red'], faceMaterialIndices: new Uint32Array([0, 1, 0]) }), /primvar candidate/);
  assert.throws(() => materializePrimvarParameterization({ candidate: plan.candidates[0], materialPaths: ['/Mat/Blue'], faceMaterialIndices: new Uint32Array([0, 1, 0]) }), /out-of-range material/);
  assert.throws(() => materializePrimvarParameterization({ candidate: plan.candidates[0], materialPaths: ['/Mat/Blue', '/Mat/Red'], faceMaterialIndices: { length: INDEXED_MESH_MAX_INDICES + 1 } }), /shared mesh safety limit/);
});

test('assistant parses authored material translation requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Looks/Shader' });
  assert.deepEqual(assistant.mock('translate material shader /World/Looks/Shader from materialx-standard-surface to usd-preview-surface'), { name: 'scene.translate_material', arguments: { path: '/World/Looks/Shader', from: 'materialx-standard-surface', to: 'usd-preview-surface' } });
  assert.deepEqual(assistant.mock('translate material shader /World/Looks/Shader from MaterialX to UsdPreviewSurface'), { name: 'scene.translate_material', arguments: { path: '/World/Looks/Shader', from: 'materialx-standard-surface', to: 'usd-preview-surface' } });
  assert.equal(assistant.validate({ name: 'scene.translate_material', arguments: { path: '/World/Looks/Shader', from: 'standard-surface', to: 'pbr' } }).name, 'scene.translate_material');
  assert.deepEqual(assistant.mock('remove unreachable material graph nodes'), { name: 'scene.optimize_material_graph', arguments: { path: '/World/Looks/Shader' } });
  assert.equal(assistant.validate({ name: 'scene.optimize_material_graph', arguments: { path: '/World/Looks/Mat' } }).name, 'scene.optimize_material_graph');
});

test('session authors translated shader input names in place', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Shader "Shader" {\n  color3f inputs:base_color = (1, 0, 0)\n  float inputs:metalness = 0.5\n}\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.translateMaterialShader('/Shader', 'materialx-standard-surface', 'usd-preview-surface');
  assert.match(session.usda, /inputs:diffuseColor = \(1, 0, 0\)/);
  assert.match(session.usda, /inputs:metallic = 0\.5/);
  assert.doesNotMatch(session.usda, /inputs:base_color|inputs:metalness/);
});

test('session authors conservative unreachable material shader cleanup', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Material "Mat" {\n  token outputs:surface.connect = <./Surface.outputs:surface>\n  def Shader "Surface" {}\n  def Shader "Dead" {}\n}\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.removeUnreachableMaterialShaders('/Mat');
  assert.match(session.usda, /Shader "Surface"/);
  assert.doesNotMatch(session.usda, /Shader "Dead"/);
});

test('session authors safe literal material graph optimization', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Add.outputs:result> } def Shader "Add" { token info:id = "UsdAdd" float inputs:a = 0.2 float inputs:b = 0.3 float outputs:result } }\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.optimizeMaterialGraph('/Mat');
  assert.match(session.usda, /roughness = 0\.5/);
  assert.doesNotMatch(session.usda, /Shader "Add"/);
});

test('USDA material graph folds direct literal scalar arithmetic safely', () => {
  const source = '#usda 1.0\ndef Material "Mat" {\n  token outputs:surface.connect = <./Surface.outputs:surface>\n  def Shader "Surface" {\n    float inputs:roughness = <./Add.outputs:result>\n  }\n  def Shader "Add" {\n    uniform token info:id = "UsdAdd"\n    float inputs:a = 0.2\n    float inputs:b = 0.3\n    float outputs:result\n  }\n}\n';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.equal(result.changed, true);
  assert.deepEqual(result.folded, [{ path: '/Mat/Add', value: .5 }]);
  assert.deepEqual(result.removed, ['/Mat/Add']);
  assert.match(result.source, /inputs:roughness = 0\.5/);
  assert.doesNotMatch(result.source, /Shader "Add"/);
});

test('USDA material graph folds explicit scalar utility inputs', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Clamp.outputs:result> } def Shader "Clamp" { token info:id = "UsdClamp" float inputs:value = 2 float inputs:min = 0 float inputs:max = 1 float outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Clamp', value: 1 }]);
  assert.match(result.source, /roughness = 1/);
  assert.doesNotMatch(result.source, /Shader "Clamp"/);
});

test('USDA material graph folds scalar arithmetic chains in dependency order', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Clamp.outputs:result> } def Shader "Add" { token info:id = "UsdAdd" float inputs:a = 0.2 float inputs:b = 0.3 float outputs:result } def Shader "Clamp" { token info:id = "UsdClamp" float inputs:value = <./Add.outputs:result> float inputs:min = 0 float inputs:max = 0.4 float outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Add', value: .5 }, { path: '/Mat/Clamp', value: .4 }]);
  assert.match(result.source, /roughness = 0\.4/);
  assert.doesNotMatch(result.source, /Shader "Add"|Shader "Clamp"/);
});

test('USDA material graph folds named scalar interpolation utilities', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Lerp.outputs:result> } def Shader "Lerp" { token info:id = "UsdLerp" float inputs:a = 0 float inputs:b = 1 float inputs:t = 0.25 float outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Lerp', value: .25 }]);
  assert.match(result.source, /roughness = 0\.25/);
  assert.doesNotMatch(result.source, /Shader "Lerp"/);
});

test('USDA material graph folds scalar select nodes conservatively', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Select.outputs:result> } def Shader "Select" { token info:id = "UsdSelect" float inputs:condition = 0 float inputs:ifTrue = 0.2 float inputs:ifFalse = 0.8 float outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Select', value: .8 }]);
  assert.match(result.source, /roughness = 0\.8/);
  assert.doesNotMatch(result.source, /Shader "Select"/);
});

test('USDA material graph folds inverse utility nodes', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Inverse.outputs:result> } def Shader "Inverse" { token info:id = "UsdInverse" float inputs:value = 4 float outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Inverse', value: .25 }]);
  assert.match(result.source, /roughness = 0\.25/);
  assert.doesNotMatch(result.source, /Shader "Inverse"/);
});

test('USDA material graph folds boolean select conditions', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Select.outputs:result> } def Shader "Select" { token info:id = "UsdSelect" bool inputs:condition = true float inputs:ifTrue = 0.2 float inputs:ifFalse = 0.8 float outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Select', value: .2 }]);
  assert.match(result.source, /roughness = 0\.2/);
  assert.doesNotMatch(result.source, /Shader "Select"/);
});

test('USDA material graph does not coerce boolean arithmetic inputs', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Add.outputs:result> } def Shader "Add" { token info:id = "UsdAdd" bool inputs:a = true float inputs:b = 0.2 float outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, []);
  assert.match(result.source, /Shader "Add"/);
});

test('USDA material graph folds finite color3f arithmetic with scalar broadcast', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Add.outputs:result> } def Shader "Add" { token info:id = "UsdAdd" color3f inputs:a = (0.1, 0.2, 0.3) float inputs:b = 0.2 color3f outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.equal(result.changed, true);
  assert.equal(result.folded[0].path, '/Mat/Add');
  assert.deepEqual(result.folded[0].value.map((value) => Number(value.toFixed(6))), [0.3, 0.4, 0.5]);
  assert.match(result.source, /baseColor = \(0\.30000000000000004, 0\.4, 0\.5\)/);
  assert.doesNotMatch(result.source, /Shader "Add"/);
});

test('USDA material graph folds finite float4 arithmetic without mixing shapes', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float4 inputs:rgba = <./Add.outputs:result> } def Shader "Add" { token info:id = "UsdAdd" float4 inputs:a = (0.1, 0.2, 0.3, 1) float4 inputs:b = (0.2, 0.3, 0.4, -0.25) float4 outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.equal(result.changed, true);
  assert.deepEqual(result.folded[0].value.map((value) => Number(value.toFixed(6))), [0.3, 0.5, 0.7, 0.75]);
  assert.match(result.source, /rgba = \(0\.30000000000000004, 0\.5, 0\.7, 0.75\)/);
  assert.doesNotMatch(result.source, /Shader "Add"/);
});

test('USDA material graph folds safe unary utility chains', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Sqrt.outputs:result> } def Shader "Sqrt" { token info:id = "UsdSqrt" float inputs:value = <./Negate.outputs:result> float outputs:result } def Shader "Negate" { token info:id = "UsdNegate" float inputs:value = -4 float outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Negate', value: 4 }, { path: '/Mat/Sqrt', value: 2 }]);
  assert.match(result.source, /roughness = 2/);
  assert.doesNotMatch(result.source, /Shader "Sqrt"|Shader "Negate"/);
});

test('USDA material graph folds dimension-checked vector utilities', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Length.outputs:result> } def Shader "Length" { token info:id = "UsdLength" float3 inputs:value = <./Cross.outputs:result> float outputs:result } def Shader "Cross" { token info:id = "UsdCross" float3 inputs:a = (1, 0, 0) float3 inputs:b = (0, 1, 0) float3 outputs:result } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded[0].value, [0, 0, 1]);
  assert.deepEqual(result.folded[1].value, 1);
  assert.match(result.source, /roughness = 1/);
  assert.doesNotMatch(result.source, /Shader "Length"|Shader "Cross"/);
});

test('USDA material graph folds MaterialX mix nodes', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Mix.outputs:out> } def Shader "Mix" { token info:id = "ND_mix_color3" color3f inputs:bg = (0.1, 0.2, 0.3) color3f inputs:fg = (0.9, 0.8, 0.7) float inputs:mix = 0.25 color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded[0].value.map((value) => Number(value.toFixed(6))), [0.3, 0.35, 0.4]);
  assert.match(result.source, /baseColor = \(0.30000000000000004, 0.35000000000000003, 0.39999999999999997\)/);
  assert.doesNotMatch(result.source, /Shader "Mix"/);
});

test('USDA material graph folds MaterialX arithmetic IDs with in1/in2 and out', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Add.outputs:out> } def Shader "Add" { token info:id = "ND_add_color3" color3f inputs:in1 = (0.1, 0.2, 0.3) color3f inputs:in2 = (0.2, 0.3, 0.4) color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded[0].value.map((value) => Number(value.toFixed(6))), [0.3, 0.5, 0.7]);
  assert.match(result.source, /baseColor = \(0\.30000000000000004, 0\.5, 0\.7\)/);
  assert.doesNotMatch(result.source, /Shader "Add"/);
});

test('USDA material graph folds safe MaterialX scalar/vector conversions', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Convert.outputs:out> } def Shader "Convert" { token info:id = "ND_convert_float_color3" float inputs:in = 0.25 color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Convert', value: [0.25, 0.25, 0.25] }]);
  assert.match(result.source, /baseColor = \(0\.25, 0\.25, 0\.25\)/);
  assert.doesNotMatch(result.source, /Shader "Convert"/);
});

test('USDA material graph folds MaterialX typed constant nodes', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color4f inputs:emissive = <./Constant.outputs:out> } def Shader "Constant" { token info:id = "ND_constant_color4" color4f inputs:value = (0.1, 0.2, 0.3, 0.4) color4f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Constant', value: [0.1, 0.2, 0.3, 0.4] }]);
  assert.match(result.source, /emissive = \(0\.1, 0\.2, 0\.3, 0\.4\)/);
  assert.doesNotMatch(result.source, /Shader "Constant"/);
});

test('USDA material graph folds MaterialX conditional branches', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Choose.outputs:out> } def Shader "Choose" { token info:id = "ND_ifgreater_color3" float inputs:value1 = 2 float inputs:value2 = 1 color3f inputs:in1 = (0.1, 0.2, 0.3) color3f inputs:in2 = (0.8, 0.7, 0.6) color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Choose', value: [0.1, 0.2, 0.3] }]);
  assert.match(result.source, /baseColor = \(0\.1, 0\.2, 0\.3\)/);
  assert.doesNotMatch(result.source, /Shader "Choose"/);
});

test('USDA material graph folds MaterialX less-than conditional branches', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Choose.outputs:out> } def Shader "Choose" { token info:id = "ND_iflesseq_float" float inputs:value1 = 1 float inputs:value2 = 1 float inputs:in1 = 0.25 float inputs:in2 = 0.75 float outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Choose', value: 0.25 }]);
  assert.match(result.source, /roughness = 0\.25/);
  assert.doesNotMatch(result.source, /Shader "Choose"/);
});

test('USDA material graph folds MaterialX remap ranges componentwise', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Remap.outputs:out> } def Shader "Remap" { token info:id = "ND_remap_color3" color3f inputs:in = (0.5, 0.25, 0.75) float inputs:inlow = 0 float inputs:inhigh = 1 color3f inputs:outlow = (0.1, 0.2, 0.3) color3f inputs:outhigh = (0.9, 0.8, 0.7) color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded[0].value.map((value) => Number(value.toFixed(6))), [0.5, 0.35, 0.6]);
  assert.match(result.source, /baseColor = \(0\.5, 0\.35000000000000003, 0\.6\)|baseColor = \(0\.5, 0\.35, 0\.6\)/);
  assert.doesNotMatch(result.source, /Shader "Remap"/);
});

test('USDA material graph folds bounded MaterialX component extraction', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Extract.outputs:out> } def Shader "Extract" { token info:id = "ND_extract_color3" color3f inputs:in = (0.1, 0.2, 0.3) int inputs:index = 1 float outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Extract', value: 0.2 }]);
  assert.match(result.source, /roughness = 0\.2/);
  assert.doesNotMatch(result.source, /Shader "Extract"/);
});

test('USDA material graph folds MaterialX channel combine nodes', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Combine.outputs:out> } def Shader "Combine" { token info:id = "ND_combine3_color3" float inputs:in1 = 0.1 float inputs:in2 = 0.2 float inputs:in3 = 0.3 color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Combine', value: [0.1, 0.2, 0.3] }]);
  assert.match(result.source, /baseColor = \(0\.1, 0\.2, 0\.3\)/);
  assert.doesNotMatch(result.source, /Shader "Combine"/);
});

test('USDA material graph folds MaterialX swizzle channels', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Swizzle.outputs:out> } def Shader "Swizzle" { token info:id = "ND_swizzle_color3" color3f inputs:in = (0.1, 0.2, 0.3) string inputs:channels = "bgr" color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Swizzle', value: [0.3, 0.2, 0.1] }]);
  assert.match(result.source, /baseColor = \(0\.3, 0\.2, 0\.1\)/);
  assert.doesNotMatch(result.source, /Shader "Swizzle"/);
});

test('USDA material graph folds MaterialX luminance utility', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Luma.outputs:out> } def Shader "Luma" { token info:id = "ND_luminance_color3" color3f inputs:in = (1, 0, 0) float outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Luma', value: 0.2126 }]);
  assert.match(result.source, /roughness = 0\.2126/);
  assert.doesNotMatch(result.source, /Shader "Luma"/);
});

test('USDA material graph folds MaterialX HSV adjustment', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Adjust.outputs:out> } def Shader "Adjust" { token info:id = "ND_hsvadjust_color3" color3f inputs:in = (1, 0, 0) color3f inputs:amount = (0.5, 1, 1) color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded[0].value.map((value) => Number(value.toFixed(6))), [0, 1, 1]);
  assert.match(result.source, /baseColor = \(0, 1, 1\)/);
  assert.doesNotMatch(result.source, /Shader "Adjust"/);
});

test('USDA material graph folds MaterialX luminance saturation', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Saturate.outputs:out> } def Shader "Saturate" { token info:id = "ND_saturate_color3" color3f inputs:in = (1, 0, 0) float inputs:amount = 0.5 color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded[0].value.map((value) => Number(value.toFixed(6))), [0.6063, 0.1063, 0.1063]);
  assert.match(result.source, /baseColor = \(0\.6063000000000001, 0\.1063, 0\.1063\)/);
  assert.doesNotMatch(result.source, /Shader "Saturate"/);
});

test('USDA material graph folds MaterialX extended unary utilities', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Fract.outputs:out> } def Shader "Sign" { token info:id = "ND_sign_float" float inputs:in = -2 float outputs:out } def Shader "Exp" { token info:id = "ND_exp_float" float inputs:in = <./Sign.outputs:out> float outputs:out } def Shader "Log" { token info:id = "ND_log_float" float inputs:in = 1 float outputs:out } def Shader "Fract" { token info:id = "ND_fract_float" float inputs:in = 2.75 float outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Exp', value: Math.exp(-1) }, { path: '/Mat/Fract', value: 0.75 }, { path: '/Mat/Log', value: 0 }, { path: '/Mat/Sign', value: -1 }]);
  assert.match(result.source, /roughness = 0\.75/);
  assert.doesNotMatch(result.source, /Shader "Fract"/);
});

test('USDA material graph folds MaterialX unary alias utilities', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Negate.outputs:out> } def Shader "Negate" { token info:id = "ND_negative_float" float inputs:in = <./OneMinus.outputs:out> float outputs:out } def Shader "OneMinus" { token info:id = "ND_one_minus_float" float inputs:in = 0.25 float outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Negate', value: -0.75 }, { path: '/Mat/OneMinus', value: 0.75 }]);
  assert.match(result.source, /roughness = -0\.75/);
  assert.doesNotMatch(result.source, /Shader "OneMinus"/);
});

test('USDA material graph folds scalar MaterialX saturate', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Saturate.outputs:out> } def Shader "Saturate" { token info:id = "ND_saturate_float" float inputs:in = 1.5 float outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Saturate', value: 1 }]);
  assert.match(result.source, /roughness = 1/);
  assert.doesNotMatch(result.source, /Shader "Saturate"/);
});

test('USDA material graph folds MaterialX invert color utility', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Invert.outputs:out> } def Shader "Invert" { token info:id = "ND_invert_color3" color3f inputs:in = (0.2, 0.5, 1) color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Invert', value: [0.8, 0.5, 0] }]);
  assert.match(result.source, /baseColor = \(0\.8, 0\.5, 0\)/);
  assert.doesNotMatch(result.source, /Shader "Invert"/);
});

test('USDA material graph folds MaterialX reciprocal aliases', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Inverse.outputs:out> } def Shader "Inverse" { token info:id = "ND_inverse_float" float inputs:in = 4 float outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Inverse', value: 0.25 }]);
  assert.match(result.source, /roughness = 0\.25/);
  assert.doesNotMatch(result.source, /Shader "Inverse"/);
});

test('USDA material graph folds MaterialX trigonometric utilities', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Sin.outputs:out> } def Shader "Sin" { token info:id = "ND_sin_float" float inputs:in = 1.5707963267948966 float outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.equal(result.folded.length, 1);
  assert.equal(result.folded[0].path, '/Mat/Sin');
  assert.ok(Math.abs(result.folded[0].value - 1) < 1e-12);
  assert.match(result.source, /roughness = 1/);
  assert.doesNotMatch(result.source, /Shader "Sin"/);
});

test('USDA material graph folds MaterialX factorized HSV adjustment', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { color3f inputs:baseColor = <./Adjust.outputs:out> } def Shader "Adjust" { token info:id = "ND_hsv_adjust_color3" color3f inputs:in = (1, 0, 0) float inputs:hue = 0.5 float inputs:saturation = 1 float inputs:value = 1 float inputs:fac = 0.5 color3f outputs:out } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded[0].value.map((value) => Number(value.toFixed(6))), [0.5, 0.5, 0.5]);
  assert.match(result.source, /baseColor = \(0.5, 0.5, 0.5\)/);
  assert.doesNotMatch(result.source, /Shader "Adjust"/);
});

test('USDA material graph folds MaterialX separate output channels', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Separate.outputs:outg> } def Shader "Separate" { token info:id = "ND_separate3_color3" color3f inputs:in = (0.1, 0.2, 0.3) float outputs:outr float outputs:outg float outputs:outb } }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Separate', value: [0.1, 0.2, 0.3] }]);
  assert.match(result.source, /roughness = 0\.2/);
  assert.doesNotMatch(result.source, /Shader "Separate"/);
});

test('USDA material graph retains externally referenced arithmetic nodes', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Add.outputs:result> } def Shader "Add" { token info:id = "UsdAdd" float inputs:a = 0.2 float inputs:b = 0.3 float outputs:result } } def Shader "External" { float inputs:value = </Mat/Add.outputs:result> }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.equal(result.changed, false);
  assert.match(result.source, /Shader "Add"/);
});

test('USDA material graph retains arithmetic nodes with internal prim references', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { float inputs:roughness = <./Add.outputs:result> } def Shader "Add" { token info:id = "UsdAdd" float inputs:a = 0.2 float inputs:b = 0.3 float outputs:result } rel source = <./Add> }';
  const result = foldLiteralUSDShaderNodes(source, '/Mat');
  assert.deepEqual(result.folded, [{ path: '/Mat/Add', value: .5 }]);
  assert.deepEqual(result.removed, []);
  assert.match(result.source, /roughness = 0\.5/);
  assert.match(result.source, /Shader "Add"/);
});

test('session resolves the shader connected to a bound material', () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Xform "World" {\n  def Mesh "Mesh" { rel material:binding = </World/Looks/Mat> }\n  def Scope "Looks" {\n    def Material "Mat" { token outputs:surface.connect = </World/Looks/Mat/Shader.outputs:surface> }\n    def Shader "Shader" {}\n  }\n}\n';
  assert.equal(session.getBoundMaterialShaderPath('/World/Mesh'), '/World/Looks/Mat/Shader');
  assert.equal(session.getBoundMaterialShaderPath('/World/Missing'), null);
});

test('authored PBR translation renames inputs without overwriting targets', () => {
  const body = 'color3f inputs:base_color = (1, 0, 0)\nfloat inputs:metalness = 0.5\nfloat inputs:custom = 1\n';
  const result = translateAuthoredPBRProperties(body, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.equal(result.changed, 2);
  assert.match(result.body, /inputs:diffuseColor/);
  assert.match(result.body, /inputs:metallic/);
  assert.match(result.body, /inputs:custom/);
  const skipped = translateAuthoredPBRProperties(`${body}float inputs:diffuseColor = 0.2\n`, { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.deepEqual(skipped.skipped, ['base_color']);
  assert.equal(skipped.changed, 1);
  const lossy = translateAuthoredPBRProperties('float inputs:specular = 0.5\nfloat inputs:ior = 1.5\n', { from: 'usd-preview-surface', to: 'metallic-roughness' });
  assert.deepEqual(lossy.unsupported, ['ior', 'specular']);
  assert.equal(lossy.changed, 0);
  const quoted = translateAuthoredPBRProperties('string inputs:label = "inputs:base_color" # inputs:base_color\ncolor3f inputs:base_color = (1, 0, 0)\n', { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.match(quoted.body, /"inputs:base_color" # inputs:base_color/);
  assert.match(quoted.body, /inputs:diffuseColor/);
  assert.throws(() => translateAuthoredPBRProperties(body, null), /Unsupported PBR material format/);
});

test('authored PBR translation updates recognized shader implementation IDs', () => {
  const result = translateAuthoredPBRProperties('uniform token info:id = "ND_standard_surface_surfaceshader"\nfloat inputs:specular_roughness = 0.5\n', { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.equal(result.changed, 2);
  assert.deepEqual(result.shaderId, { from: 'ND_standard_surface_surfaceshader', to: 'UsdPreviewSurface' });
  assert.match(result.body, /info:id = "UsdPreviewSurface"/);
  const quoted = translateAuthoredPBRProperties('string note = "info:id = \\"ND_standard_surface_surfaceshader\\"" # info:id = "ND_standard_surface_surfaceshader"\n', { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.equal(quoted.changed, 0);
  const reverse = translateAuthoredPBRProperties('token info:id = "UsdPreviewSurface"\n', { from: 'usd-preview-surface', to: 'materialx-standard-surface' });
  assert.deepEqual(reverse.shaderId, { from: 'UsdPreviewSurface', to: 'ND_standard_surface_surfaceshader' });
  const unknown = translateAuthoredPBRProperties('token info:id = "VendorSurface"\n', { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.equal(unknown.changed, 0);
});

test('material binding rewrite changes only exact confirmed targets', () => {
  const source = 'def Xform "A" { string note = "<\/World\/Looks\/Duplicate>" # material:binding = <\/World\/Looks\/Duplicate>\n  rel material:binding = </World/Looks/Duplicate> }\ndef Xform "B" { rel material:binding = </World/Looks/Keep> }';
  const result = rewriteMaterialBindings(source, { '/World/Looks/Duplicate': '/World/Looks/Keep' });
  assert.equal(result.changed, 1);
  assert.match(result.source, /material:binding = <\/World\/Looks\/Keep>/);
  assert.match(result.source, /note = "<\/World\/Looks\/Duplicate>" # material:binding = <\/World\/Looks\/Duplicate>/);
  assert.equal(result.source.match(/material:binding/g).length, 3);
  assert.throws(() => rewriteMaterialBindings(source, { 'Duplicate': '/World/Looks/Keep' }), /absolute USD prim paths/);
  const chained = rewriteMaterialBindings('rel material:binding = </World/Looks/A>\nrel material:binding = </World/Looks/B>', { '/World/Looks/A': '/World/Looks/B', '/World/Looks/B': '/World/Looks/C' });
  assert.match(chained.source, /binding = <\/World\/Looks\/C>/);
  assert.equal(chained.source.match(/material:binding/g).length, 2);
  assert.throws(() => rewriteMaterialBindings(source, { '/World/Looks/Duplicate': '/World/Looks/Keep', '/World/Looks/Keep': '/World/Looks/Duplicate' }), /cycles/);
});

test('collection material binding rewrite is exact, chained, and isolated', () => {
  const source = 'rel material:binding:collection:render = </World/Collections/Old>\nrel material:binding = </World/Looks/Old>\nstring note = "material:binding:collection:render = </World/Collections/Old>"';
  const result = rewriteMaterialCollectionBindings(source, { '/World/Collections/Old': '/World/Collections/New', '/World/Collections/New': '/World/Collections/Final' });
  assert.equal(result.changed, 1);
  assert.match(result.source, /material:binding:collection:render = <\/World\/Collections\/Final>/);
  assert.match(result.source, /material:binding = <\/World\/Looks\/Old>/);
  assert.match(result.source, /string note = "material:binding:collection:render = <\/World\/Collections\/Old>"/);
  assert.throws(() => rewriteMaterialCollectionBindings(source, { '/World/Collections/Old': '/World/Collections/New', '/World/Collections/New': '/World/Collections/Old' }), /cycles/);
  assert.throws(() => rewriteMaterialCollectionBindings(source, { Collections: '/World/Collections/New' }), /absolute USD prim paths/);
});

test('material definition merge rewrites bindings and removes only safe duplicates', () => {
  const source = '#usda 1.0\ndef Xform "World" {\n  def Scope "Looks" {\n    def Material "Keep" { token shader = "preview" }\n    def Material "Duplicate" { token shader = "preview" }\n  }\n  def Mesh "M" { rel material:binding = </World/Looks/Duplicate> }\n}\n';
  const result = mergeMaterialDefinitions(source, { '/World/Looks/Duplicate': '/World/Looks/Keep' });
  assert.equal(result.changed, 1);
  assert.equal(result.removed, 1);
  assert.match(result.source, /material:binding = <\/World\/Looks\/Keep>/);
  assert.doesNotMatch(result.source, /Material "Duplicate"/);
  assert.match(result.source, /Material "Keep"/);
  const compact = 'def Material "Keep" {} def Material "Duplicate" {}\nrel material:binding = </Duplicate>';
  const compactResult = mergeMaterialDefinitions(compact, { '/Duplicate': '/Keep' });
  assert.match(compactResult.source, /Material "Keep"/);
  assert.match(compactResult.source, /material:binding = <\/Keep>/);
});

test('material definition merge refuses non-binding references', () => {
  const source = 'def Scope "Looks" { def Material "Keep" {} def Material "Duplicate" {} }\ndef Xform "World" { rel material:binding = </Looks/Duplicate> rel other = </Looks/Duplicate> }';
  assert.throws(() => mergeMaterialDefinitions(source, { '/Looks/Duplicate': '/Looks/Keep' }), /other than exact material bindings/);
});

test('material equivalence fingerprints folded scalar graph values', () => {
  const source = 'def Scope "Looks" { def Material "First" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { token info:id = "UsdPreviewSurface" float inputs:roughness = 0.5 } } def Material "Second" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { token info:id = "UsdPreviewSurface" float inputs:roughness = <./Add.outputs:result> } def Shader "Add" { token info:id = "UsdAdd" float inputs:a = 0.2 float inputs:b = 0.3 float outputs:result } } }';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('session material equivalence preview is deterministic and non-mutating', () => {
  const session = new LuciaUsdSession();
  session.usda = 'def Scope "Looks" { def Material "Keep" { token shader = "preview" } def Material "Duplicate" { token shader = "preview" } }';
  const source = session.usda;
  assert.deepEqual(session.previewEquivalentMaterialDefinitions(), {
    mapping: { '/Looks/Keep': '/Looks/Duplicate' },
    candidates: [{ from: '/Looks/Keep', to: '/Looks/Duplicate' }],
  });
  assert.equal(session.usda, source);
});

test('material equivalence falls back when graph cleanup cannot analyze a material', () => {
  const source = 'def Scope "Looks" { def Material "First" { def Shader "Surface" { token info:id = "UsdPreviewSurface" float inputs:roughness = <./Root.outputs:result> } def Shader "Root" { token info:id = "UsdAdd" float inputs:a = 0.2 float inputs:b = 0.3 } } def Material "Second" { def Shader "Surface" { token info:id = "UsdPreviewSurface" float inputs:roughness = <./Root.outputs:result> } def Shader "Root" { token info:id = "UsdAdd" float inputs:a = <./Op1.outputs:result> float inputs:b = <./Op2.outputs:result> } def Shader "Op1" { token info:id = "UsdMultiply" float inputs:a = <./Leaf.outputs:result> float inputs:b = 2 } def Shader "Op2" { token info:id = "UsdMultiply" float inputs:a = <./Leaf.outputs:result> float inputs:b = 2 } def Shader "Leaf" { token info:id = "UsdAdd" float inputs:a = 1 float inputs:b = 1 } } }';
  assert.deepEqual(findEquivalentMaterialMapping(source), {});
});

test('material definition merge ignores path text in strings and comments', () => {
  const source = 'def Scope "Looks" { def Material "Keep" {} def Material "Duplicate" {} }\ndef Xform "World" { string note = "</Looks/Duplicate>" # rel other = </Looks/Duplicate>\n rel material:binding = </Looks/Duplicate> }';
  const result = mergeMaterialDefinitions(source, { '/Looks/Duplicate': '/Looks/Keep' });
  assert.equal(result.removed, 1);
  assert.doesNotMatch(result.source, /Material "Duplicate"/);
  assert.match(result.source, /note = "<\/Looks\/Duplicate>" # rel other = <\/Looks\/Duplicate>/);
});

test('material graph cleanup removes only unreachable nested shaders', () => {
  const source = '#usda 1.0\ndef Scope "Looks" {\n  def Material "M" {\n    token outputs:surface.connect = <./Surface.outputs:surface>\n    def Shader "Surface" {\n      token info:id = "UsdPreviewSurface"\n      token inputs:roughness = <./Roughness.outputs:result>\n    }\n    def Shader "Roughness" {\n      token info:id = "UsdPrimvarReader_float"\n    }\n    def Shader "Dead" {\n      token info:id = "Unused"\n    }\n  }\n}\n';
  const result = removeUnreachableMaterialShaders(source, '/Looks/M');
  assert.equal(result.changed, true);
  assert.deepEqual(result.removed, ['/Looks/M/Dead']);
  assert.match(result.source, /Shader "Surface"/);
  assert.match(result.source, /Shader "Roughness"/);
  assert.doesNotMatch(result.source, /Shader "Dead"/);
});

test('material graph cleanup retains unreachable shaders referenced outside the material', () => {
  const source = 'def Scope "Looks" { def Material "M" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" {} def Shader "Shared" {} } }\ndef Shader "Consumer" { token inputs:source = </Looks/M/Shared.outputs:result> }';
  const result = removeUnreachableMaterialShaders(source, '/Looks/M');
  assert.equal(result.changed, false);
  assert.deepEqual(result.removed, []);
  assert.match(result.source, /Shader "Shared"/);
});

test('material graph cleanup retains unreachable shaders referenced as prim paths', () => {
  const source = 'def Scope "Looks" { def Material "M" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" {} def Shader "Shared" {} } }\ndef Xform "Consumer" { rel source = </Looks/M/Shared> }';
  const result = removeUnreachableMaterialShaders(source, '/Looks/M');
  assert.equal(result.changed, false);
  assert.deepEqual(result.removed, []);
  assert.match(result.source, /Shader "Shared"/);
});

test('material graph cleanup ignores nested shader paths in outside strings and comments', () => {
  const source = 'def Scope "Looks" { def Material "M" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" {} def Shader "Dead" {} } }\ndef Xform "World" { string note = "</Looks/M/Dead.outputs:result>" # </Looks/M/Dead.outputs:result> }';
  const result = removeUnreachableMaterialShaders(source, '/Looks/M');
  assert.equal(result.changed, true);
  assert.deepEqual(result.removed, ['/Looks/M/Dead']);
  assert.match(result.source, /note = "<\/Looks\/M\/Dead\.outputs:result>" # <\/Looks\/M\/Dead\.outputs:result>/);
});

test('material graph cleanup ignores connection-shaped text inside graph nodes', () => {
  const source = 'def Scope "Looks" {\n  def Material "M" {\n    token outputs:surface.connect = <./Surface.outputs:surface>\n    def Shader "Surface" { string note = "<./Dead.outputs:result>" # <./Dead.outputs:result>\n    }\n    def Shader "Dead" {}\n  }\n}\n';
  const result = removeUnreachableMaterialShaders(source, '/Looks/M');
  assert.equal(result.changed, true);
  assert.deepEqual(result.removed, ['/Looks/M/Dead']);
  assert.match(result.source, /note = "<\.\/Dead\.outputs:result>" # <\.\/Dead\.outputs:result>/);
});

test('material merge mapping is deterministic for nested equivalent definitions', () => {
  const source = 'def Scope "Looks" {\n  def Material "Zed" { token shader = "preview" }\n  def Material "Alpha" { token shader = "preview" }\n}\ndef Xform "World" { rel material:binding = </Looks/Zed> }';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Zed': '/Looks/Alpha' });
});

test('material equivalence ignores top-level property order but preserves nested values', () => {
  const source = 'def Scope "Looks" {\n  def Material "First" {\n    token shader = "preview"\n    float inputs:roughness = 0.5\n  }\n  def Material "Second" {\n    float inputs:roughness = 0.5\n    token shader = "preview"\n  }\n}\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material equivalence ignores declaration text in strings and comments', () => {
  const source = 'def Scope "Looks" {\n  string note = "def Material \\\"Fake\\\" { def Shader \\\"Ghost\\\" {} }" # def Material "Commented" {}\n  def Material "First" { token shader = "preview" }\n  def Material "Second" { token shader = "preview" }\n}\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material equivalence canonicalizes nested shader graph property order', () => {
  const source = 'def Scope "Looks" {\n  def Material "First" {\n    def Shader "Surface" {\n      token info:id = "UsdPreviewSurface"\n      float inputs:roughness = 0.5\n    }\n  }\n  def Material "Second" {\n    def Shader "Surface" {\n      float inputs:roughness = 0.5\n      token info:id = "UsdPreviewSurface"\n    }\n  }\n}\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material equivalence canonicalizes local shader node names and connections', () => {
  const source = 'def Scope "Looks" {\n  def Material "First" {\n    def Shader "Surface" { token info:id = "UsdPreviewSurface" outputs:surface.connect = <Surface.outputs:surface> }\n  }\n  def Material "Second" {\n    def Shader "Renamed" { token info:id = "UsdPreviewSurface" outputs:surface.connect = <Renamed.outputs:surface> }\n  }\n}\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material equivalence canonicalizes equivalent local connection prefixes', () => {
  const source = 'def Scope "Looks" {\n  def Material "First" {\n    token outputs:surface.connect = <./Surface.outputs:surface>\n    def Shader "Surface" { token info:id = "UsdPreviewSurface" }\n  }\n  def Material "Second" {\n    token outputs:surface.connect = <Surface.outputs:surface>\n    def Shader "Surface" { token info:id = "UsdPreviewSurface" }\n  }\n}\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material equivalence canonicalizes absolute local shader references', () => {
  const source = 'def Scope "Looks" {\n  def Material "First" {\n    token outputs:surface.connect = <./Surface.outputs:surface>\n    def Shader "Surface" { token info:id = "UsdPreviewSurface" }\n  }\n  def Material "Second" {\n    token outputs:surface.connect = </Looks/Second/Surface.outputs:surface>\n    def Shader "Surface" { token info:id = "UsdPreviewSurface" }\n  }\n}\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material equivalence is independent of local shader declaration order', () => {
  const source = 'def Scope "Looks" {\n  def Material "First" {\n    token outputs:surface.connect = <./Surface.outputs:surface>\n    def Shader "Surface" {\n      token inputs:roughness = <./Rough.outputs:result>\n      token info:id = "UsdPreviewSurface"\n    }\n    def Shader "Rough" {\n      float inputs:value = 0.4\n      token info:id = "UsdPrimvarReader_float"\n    }\n  }\n  def Material "Second" {\n    token outputs:surface.connect = <./Main.outputs:surface>\n    def Shader "RoughNode" {\n      token info:id = "UsdPrimvarReader_float"\n      float inputs:value = 0.4\n    }\n    def Shader "Main" {\n      token info:id = "UsdPreviewSurface"\n      token inputs:roughness = <./RoughNode.outputs:result>\n    }\n  }\n}\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material equivalence preserves local graph connection topology', () => {
  const source = 'def Scope "Looks" { def Material "First" { def Shader "Surface" { token info:id = "UsdPreviewSurface" float inputs:roughness = <./Mix.outputs:result> } def Shader "Mix" { token info:id = "CustomMix" float inputs:left = <./Left.outputs:result> float inputs:right = <./Right.outputs:result> } def Shader "Left" { token info:id = "CustomLeft" } def Shader "Right" { token info:id = "CustomRight" } } def Material "Second" { def Shader "Surface" { token info:id = "UsdPreviewSurface" float inputs:roughness = <./Mix.outputs:result> } def Shader "Mix" { token info:id = "CustomMix" float inputs:left = <./Right.outputs:result> float inputs:right = <./Left.outputs:result> } def Shader "Left" { token info:id = "CustomLeft" } def Shader "Right" { token info:id = "CustomRight" } } }';
  assert.deepEqual(findEquivalentMaterialMapping(source), {});
});

test('material equivalence does not rewrite local-looking names in USDA strings', () => {
  const source = 'def Scope "Looks" { def Material "First" { def Shader "Surface" { string inputs:note = "<Left>" } def Shader "Left" { token info:id = "Custom" } } def Material "Second" { def Shader "Surface" { string inputs:note = "<Right>" } def Shader "Right" { token info:id = "Custom" } } }';
  assert.deepEqual(findEquivalentMaterialMapping(source), {});
});

test('material equivalence ignores unreachable nested shader declarations', () => {
  const source = 'def Scope "Looks" {\n  def Material "First" {\n    token outputs:surface.connect = <./Surface.outputs:surface>\n    def Shader "Surface" { token info:id = "UsdPreviewSurface" }\n    def Shader "Dead" { token info:id = "Unused" }\n  }\n  def Material "Second" {\n    token outputs:surface.connect = <./Surface.outputs:surface>\n    def Shader "Surface" { token info:id = "UsdPreviewSurface" }\n  }\n}\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material equivalence preserves hash characters inside quoted values', () => {
  const source = 'def Material "First" { string inputs:label = "red#one" }\ndef Material "Second" { string inputs:label = "red#two" }\n';
  assert.deepEqual(findEquivalentMaterialMapping(source), {});
});

test('material equivalence normalizes relative asset path spelling', () => {
  const source = 'def Scope "Looks" { def Material "First" { asset inputs:file = @./textures/../textures/albedo.png@ } def Material "Second" { asset inputs:file = @textures/albedo.png@ } }';
  assert.deepEqual(findEquivalentMaterialMapping(source), { '/Looks/Second': '/Looks/First' });
});

test('material comparison only reports same-path signature changes', () => {
  const before = { path: '/World/Mesh', triangleCount: 2, triangles: ['red', 'blue'] };
  assert.equal(materialSnapshotsDiffer(before, { ...before, triangles: ['red', 'green'] }), true);
  assert.equal(materialSnapshotsDiffer(before, { ...before, triangles: ['red', 'blue'] }), false);
  assert.equal(materialSnapshotsDiffer(before, { ...before, path: '/World/Other' }), false);
  assert.equal(materialSnapshotsDiffer(before, { ...before, triangleCount: 1, triangles: ['red'] }), false);
  assert.equal(materialSnapshotsDiffer(null, before), false);
});

test('material group consolidation merges adjacent ranges without changing coverage', () => {
  assert.deepEqual(consolidateMaterialGroups([{ start: 0, count: 3, materialIndex: 1 }, { start: 3, count: 6, materialIndex: 1 }, { start: 9, count: 3, materialIndex: 2 }], 12), [{ start: 0, count: 9, materialIndex: 1 }, { start: 9, count: 3, materialIndex: 2 }]);
  assert.throws(() => consolidateMaterialGroups([{ start: 0, count: 3, materialIndex: 1 }, { start: 6, count: 3, materialIndex: 1 }], 9), /complete ordered/);
});

test('material group validation requires a triangle-aligned index count', () => {
  assert.throws(() => consolidateMaterialGroups([{ start: 0, count: 3, materialIndex: 0 }], 4), /triangle-aligned/);
  assert.throws(() => consolidateMaterialGroups([], Number.NaN), /triangle-aligned/);
});

test('material group reordering consolidates non-adjacent assignments without dropping faces', () => {
  const indices = Uint32Array.from([0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4]);
  const result = reorderMaterialGroups([{ start: 0, count: 3, materialIndex: 2 }, { start: 3, count: 3, materialIndex: 1 }, { start: 6, count: 3, materialIndex: 2 }, { start: 9, count: 3, materialIndex: 1 }], indices);
  assert.deepEqual(result.groups, [{ start: 0, count: 6, materialIndex: 1 }, { start: 6, count: 6, materialIndex: 2 }]);
  assert.deepEqual([...result.indices], [2, 3, 0, 6, 7, 4, 0, 1, 2, 4, 5, 6]);
  assert.deepEqual([...result.indices].sort((a, b) => a - b), [...indices].sort((a, b) => a - b));
});

test('material repair normalizes null mapping inputs', () => {
  assert.deepEqual(rewriteMaterialBindings('rel material:binding = </World/Looks/Mat>', null), { source: 'rel material:binding = </World/Looks/Mat>', changed: 0, mappings: [] });
  assert.deepEqual(mergeMaterialDefinitions('def Material "Mat" {}', null), { source: 'def Material "Mat" {}', changed: 0, removed: 0, mappings: [] });
});

test('assistant validates material binding maps before execution', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.throws(() => assistant.validate({ name: 'scene.merge_material_bindings', arguments: { mapping: { Duplicate: '/World/Looks/Keep' } } }), /absolute USD prim paths/);
  assert.throws(() => assistant.validate({ name: 'scene.merge_material_bindings', arguments: { mapping: [] } }), /absolute USD prim paths/);
  assert.equal(assistant.validate({ name: 'scene.merge_material_bindings', arguments: { mapping: { '/World/Looks/Duplicate': '/World/Looks/Keep' } } }).name, 'scene.merge_material_bindings');
  assert.equal(assistant.validate({ name: 'scene.rewrite_collection_material_bindings', arguments: { mapping: { '/World/Collections/Old': '/World/Collections/New' } } }).name, 'scene.rewrite_collection_material_bindings');
  assert.deepEqual(assistant.mock('repair collection material binding /World/Collections/Old to /World/Collections/New'), { name: 'scene.rewrite_collection_material_bindings', arguments: { mapping: { '/World/Collections/Old': '/World/Collections/New' } } });
  assert.throws(() => assistant.validate({ name: 'scene.repair_inherited_material_bindings', arguments: { repairs: [{ path: '/World/M', materialPath: 'Looks/Mat' }] } }), /absolute prim and material paths/);
  assert.equal(assistant.validate({ name: 'scene.repair_inherited_material_bindings', arguments: { repairs: [{ path: '/World/M', materialPath: '/World/Looks/Mat' }] } }).name, 'scene.repair_inherited_material_bindings');
  assert.deepEqual(assistant.mock('repair inherited material binding /World/M to /World/Looks/Mat'), { name: 'scene.repair_inherited_material_bindings', arguments: { repairs: [{ path: '/World/M', materialPath: '/World/Looks/Mat' }] } });
});

test('assistant validates dependency localization maps before execution', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.throws(() => assistant.validate({ name: 'scene.localize_dependencies', arguments: { mapping: [] } }), /non-empty old-to-new path map/);
  assert.equal(assistant.validate({ name: 'scene.localize_dependencies', arguments: { mapping: { '../source/albedo.png': 'textures/albedo.png' } } }).name, 'scene.localize_dependencies');
  assert.deepEqual(assistant.mock('localize dependency @../source/albedo.png@ to @textures/albedo.png@'), { name: 'scene.localize_dependencies', arguments: { mapping: { '../source/albedo.png': 'textures/albedo.png' } } });
});

test('assistant records proposal, decision, and execution outcomes', async () => {
  const decisions = [];
  const assistant = new LuciaAssistant({ executeTool: async () => ({ message: 'ok' }), getSelection: () => '/World/Mesh', getSceneSummary: () => ({}), confirm: async () => false, recordDecision: (call, decision) => decisions.push([call.name, decision]) });
  await assistant.send('generate a UV atlas for the selected mesh');
  assert.deepEqual(decisions, [['scene.unwrap_uv', 'proposed'], ['scene.unwrap_uv', 'declined']]);
  assistant.confirm = async () => true;
  await assistant.send('generate a UV atlas for the selected mesh');
  assert.deepEqual(decisions.slice(-2), [['scene.unwrap_uv', 'proposed'], ['scene.unwrap_uv', 'executed']]);
  assistant.executeTool = async () => { throw new LuciaError('TEST_REJECTED', 'operation failed'); };
  await assistant.send('generate a UV atlas for the selected mesh');
  assert.deepEqual(decisions.slice(-2), [['scene.unwrap_uv', 'proposed'], ['scene.unwrap_uv', 'rejected']]);
});

test('project assistant activity keeps only known decisions and valid prim paths', () => {
  const project = new LuciaProject();
  const entry = project.recordAssistantDecision({ name: 'scene.inspect', arguments: { path: '/World/Z', targetPath: '/World/../private', sourcePath: '/World/A' } }, 'unknown');
  assert.deepEqual({ decision: entry.decision, tool: entry.tool, paths: entry.paths }, { decision: 'rejected', tool: 'scene.inspect', paths: ['/World/A', '/World/Z'] });
  assert.equal(project.dirty, false);
});

test('project assistant activity restore keeps the same prim-path contract', () => {
  const project = new LuciaProject();
  assert.equal(project.restoreAssistantActivity([{ kind: 'assistant', tool: 'scene.inspect', decision: 'executed', paths: ['/World/Z', '/World/../private', '/World/A'] }]), 1);
  assert.deepEqual(project.activity[0].paths, ['/World/A', '/World/Z']);
});

test('assistant routes and validates selected-prim extraction', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Hero' });
  assert.equal(assistant.mock('extract the selected prim to a reference layer').name, 'scene.extract_reference');
  assert.equal(assistant.validate({ name: 'scene.extract_reference', arguments: { path: '/World/Hero', assetPath: 'layers/hero.usda' } }).name, 'scene.extract_reference');
  assert.throws(() => assistant.validate({ name: 'scene.extract_reference', arguments: { path: '/World/Hero', assetPath: '../hero.usda' } }), /Invalid asset path/);
});

test('texture reference inventory excludes composition arcs', () => {
  const operations = new LuciaOperations({ usda: '#usda 1.0\nsubLayers = [@child.usda@]\ndef Material "M" { asset inputs:file = @textures/albedo.png@ }' }, {}, {});
  assert.deepEqual(operations.textureReferences().map(({ assetPath, kind }) => ({ assetPath, kind })), [{ assetPath: 'textures/albedo.png', kind: 'asset' }]);
});

test('xatlas request validation bounds worker inputs', () => {
  const valid = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), options: { resolution: 1024, padding: 2 } };
  assert.equal(validateXatlasRequest(valid), true);
  assert.throws(() => validateXatlasRequest({ ...valid, positions: [...valid.positions] }), /typed buffers/);
  assert.throws(() => validateXatlasRequest({ ...valid, indices: new Uint32Array([0, 1, 3]) }), /in range/);
  assert.throws(() => validateXatlasRequest({ ...valid, options: { resolution: 16384 } }), /resolution/);
  assert.throws(() => validateXatlasRequest({ ...valid, options: { textureSeamWeight: Infinity } }), /textureSeamWeight/);
  assert.throws(() => validateXatlasRequest({ ...valid, options: { maxChartSize: 9000 } }), /maxChartSize/);
  assert.throws(() => validateXatlasRequest({ ...valid, options: { rotateCharts: 'yes' } }), /rotateCharts.*boolean/);
  assert.throws(() => validateXatlasRequest({ ...valid, options: { blockAlign: 'yes' } }), /blockAlign.*boolean/);
  assert.throws(() => validateXatlasRequest({ ...valid, options: { singleAtlasFallback: 'yes' } }), /singleAtlasFallback.*boolean/);
  assert.equal(validateXatlasRequest({ ...valid, options: null }), true);
  assert.throws(() => validateXatlasRequest({ ...valid, options: [] }), /options must be an object/);
  assert.throws(() => validateXatlasRequest({ ...valid, options: 'balanced' }), /options must be an object/);
  assert.throws(() => validateXatlasRequest({ ...valid, options: { resolution: '1024' } }), /resolution/);
  assert.throws(() => validateXatlasRequest({ ...valid, customAttributes: {} }), /custom attributes/);
  assert.throws(() => validateXatlasRequest({ ...valid, customAttributes: [null] }), /custom attributes/);
  assert.throws(() => validateXatlasRequest({ ...valid, customAttributes: [{ name: 'arrayValue', itemSize: 1, array: [1, 2, 3] }] }), /supported numeric typed arrays/);
  assert.throws(() => validateXatlasRequest({ ...valid, normals: new Float32Array(6) }), /normals/);
  assert.throws(() => validateXatlasRequest({ ...valid, normals: [0, 0, 1, 0, 0, 1, 0, 0, 1] }), /typed buffers/);
  assert.throws(() => validateXatlasRequest({ ...valid, tangents: new Float32Array(12).fill(Infinity) }), /tangents/);
  assert.throws(() => validateXatlasRequest({ ...valid, customAttributes: [{ name: 'bad name', itemSize: 1, array: new Float32Array(3) }] }), /custom attributes/);
  assert.throws(() => validateXatlasRequest({ ...valid, customAttributes: [{ name: 'mask', itemSize: 1, array: new Float32Array(3) }, { name: 'mask', itemSize: 1, array: new Float32Array(3) }] }), /duplicated/);
  assert.throws(() => validateXatlasRequest({ ...valid, positions: new Float32Array([0, 0, Infinity]) }), /finite/);
  assert.throws(() => validateXatlasRequest({ ...valid, positions: {} }), /non-empty position/);
  assert.throws(() => validateXatlasRequest({ ...valid, normals: {} }), /normals/);
  assert.throws(() => validateXatlasRequest(null), /non-empty position/);
  assert.throws(() => validateXatlasRequest(7), /non-empty position/);
  const skin = { jointIndices: new Float32Array([0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3]), jointWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) };
  assert.throws(() => validateXatlasRequest({ ...valid, jointIndices: skin.jointIndices }), /provided together/);
  assert.throws(() => validateXatlasRequest({ ...valid, ...skin, jointIndices: new Float32Array([0, 65536, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) }), /jointIndices/);
  assert.throws(() => validateXatlasRequest({ ...valid, ...skin, jointWeights: new Float32Array([1, -1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) }), /jointWeights/);
});

test('safe cleanup preserves material group ranges while removing faces', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1]),
    indices: new Uint32Array([0, 1, 2, 0, 3, 4]),
    groups: [{ start: 0, count: 3, materialIndex: 2 }, { start: 3, count: 3, materialIndex: 4 }],
  });
  assert.deepEqual(result.groups, [{ start: 0, count: 3, materialIndex: 2 }, { start: 3, count: 3, materialIndex: 4 }]);
});

test('safe cleanup rejects ambiguous material group ranges before processing', () => {
  assert.throws(() => validateMeshMaterialGroups([{ start: 0, count: 6, materialIndex: 0 }, { start: 3, count: 3, materialIndex: 1 }], 9), /overlap/);
  assert.throws(() => validateMeshMaterialGroups([{ start: 1, count: 3, materialIndex: 0 }], 9), /overlap or fall outside/);
});

test('xatlas material groups transfer only when source faces remain ordered', () => {
  const groups = [{ start: 0, count: 3, materialIndex: 0 }, { start: 3, count: 3, materialIndex: 1 }];
  const indices = new Uint32Array([0, 1, 2, 3, 4, 5]);
  assert.throws(() => transferXatlasMaterialGroups(groups, indices, indices, new Uint32Array([0, 1])), /invalid source remap/);
  assert.throws(() => transferXatlasMaterialGroups(groups, indices, [0, 1, 2, 3, 4, 5], new Uint32Array([0, 1, 2, 3, 4, 5])), /typed buffers/);
  assert.throws(() => transferXatlasMaterialGroups(groups, indices, indices, new Float32Array([0, 1.5, 2, 3, 4, 5])), /invalid source remap/);
  assert.deepEqual(transferXatlasMaterialGroups(groups, indices, indices, new Uint32Array([0, 1, 2, 3, 4, 5])), groups);
  assert.deepEqual(transferXatlasMaterialGroups(groups, indices, indices, new Uint32Array([0, 1, 2, 3, 4, 5, 6])), groups);
  assert.throws(() => transferXatlasMaterialGroups(groups, indices, new Uint32Array([3, 4, 5, 0, 1, 2]), new Uint32Array([0, 1, 2, 3, 4, 5])), /reordered/);
});

test('UV projections produce finite normalized coordinates', () => {
  const positions = new Float32Array([-1, -1, -1, 1, -1, -1, 1, 1, 1, -1, 1, 1]);
  for (const mode of ['planar', 'box', 'cylindrical', 'spherical']) {
    const uvs = projectUVs({ positions, mode });
    assert.equal(uvs.length, 8);
    assert.ok([...uvs].every((value) => Number.isFinite(value) && value >= 0 && value <= 1), mode);
  }
});

test('UV projection authors the selected UV set', async () => {
  const mesh = { isMesh: true, geometry: {
    attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } },
    index: { count: 3, array: new Uint16Array([0, 1, 2]) },
  } };
  let authored;
  const session = { setMeshGeometry: async (path, data, summary) => { authored = { path, data, summary }; return 'previous'; } };
  const operations = new LuciaOperations(session, {}, { objectForPath: () => mesh });
  assert.equal(await operations.projectUV('/World/Mesh', { mode: 'planar', uvSet: 'lightmap' }), 'previous');
  assert.equal(authored.path, '/World/Mesh');
  assert.equal(authored.data.uvSet, 'lightmap');
  assert.match(authored.summary, /lightmap UVs/);
  await assert.rejects(operations.projectUV('/World/Mesh', { mode: 'planar', uvSet: 'bad' }), /Unsupported UV set/);
});

test('component operation exposes a read-only segmentation preview', async () => {
  const mesh = { isMesh: true, geometry: {
    attributes: { position: { count: 6, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]) }, },
    index: { count: 6, array: new Uint16Array([0, 1, 2, 3, 4, 5]) },
  } };
  const operations = new LuciaOperations({ getMeshMaterialGroups: () => [{ start: 0, count: 3, materialIndex: 1 }, { start: 3, count: 3, materialIndex: 2 }] }, {}, { objectForPath: () => mesh });
  const preview = await operations.previewConnectedComponents('/World/Mesh');
  assert.deepEqual([...preview.faceLabels], [0, 1]);
  assert.deepEqual(preview.components.map((component) => component.materialIndices), [[1], [2]]);
  const filtered = await operations.previewConnectedComponents('/World/Mesh', { minFaces: 2 });
  assert.deepEqual([...filtered.faceLabels], [-1, -1]);
  const bent = { isMesh: true, geometry: { attributes: { position: { count: 4, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]) } }, index: { count: 6, array: new Uint16Array([0, 1, 2, 1, 0, 3]) } } };
  const angleOperations = new LuciaOperations({}, {}, { objectForPath: () => bent });
  assert.equal((await angleOperations.previewConnectedComponents('/World/Mesh', { splitAngle: 45 })).components.length, 2);
  const namingOperations = new LuciaOperations({ tree: () => [{ name: 'Mesh_Part1', children: [] }] }, {}, { objectForPath: () => mesh });
  assert.equal((await namingOperations.proposeComponentNames('/World/Mesh')).at(0).name, 'Mesh_Part1_0');
});

test('component split authors collision-safe names and reports them', async () => {
  const mesh = { isMesh: true, geometry: {
    attributes: { position: { count: 6, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]) } },
    index: { count: 6, array: new Uint16Array([0, 1, 2, 3, 4, 5]) },
  } };
  const authored = [], session = {
    exportUSDA: () => 'before',
    tree: () => [{ name: 'Mesh_Part1', children: [] }, { name: 'Mesh_Part1_0', children: [] }],
    setMeshGeometrySibling: async (path, name) => { authored.push({ path, name }); },
    setVisibility: async (path, visible) => { authored.push({ path, visible }); },
  };
  const operations = new LuciaOperations(session, {}, { objectForPath: () => mesh });
  await operations.splitConnectedComponents('/World/Mesh');
  assert.deepEqual(authored.slice(0, 2), [{ path: '/World/Mesh', name: 'Mesh_Part1_0_2' }, { path: '/World/Mesh', name: 'Mesh_Part2' }]);
  assert.deepEqual(operations.lastStats.parts.map(({ name }) => name), ['Mesh_Part1_0_2', 'Mesh_Part2']);
});

test('corrected component extraction remaps merged face-varying data', () => {
  const data = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]), indices: new Uint32Array([0, 1, 2, 3, 4, 5]), faceVaryingAttributes: [{ name: 'st', itemSize: 2, array: new Float32Array([0, 0, 1, 0, 0, 1, .5, .5, .6, .5, .5, .6]), indices: new Uint32Array([0, 1, 2, 3, 4, 5]) }] };
  const corrected = extractCorrectedComponents(data, { mergeGroups: [[0, 1]] });
  assert.equal(corrected.components.length, 1);
  assert.equal(corrected.components[0].triangleCount, 2);
  assert.equal(corrected.components[0].faceVaryingAttributes[0].indices.length, 6);
});

test('component extraction remaps authored crease edges and lightmap UVs', () => {
  const result = extractConnectedComponents({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 4, 5]),
    sharpEdges: [[0, 1], [3, 4]],
    sharpEdgeSharpness: [0.25, 0.75],
    faceVaryingAttributes: [{ name: 'st1', itemSize: 2, array: new Float32Array([0, 0, 1, 0, 0, 1, .5, .5, 1, .5, .5, 1]), indices: new Uint32Array([0, 1, 2, 3, 4, 5]) }],
  });
  assert.deepEqual(result.map((component) => component.sharpEdges), [[[0, 1]], [[0, 1]]]);
  assert.deepEqual(result.map((component) => component.sharpEdgeSharpness), [[.25], [.75]]);
  assert.deepEqual(result.map((component) => component.faceVaryingAttributes[0].indices.length), [3, 3]);
});

test('material graph operation exposes a read-only optimization preview', async () => {
  const mesh = { isMesh: true, material: { userData: { nodes: { output: { type: 'output', inputs: { surface: { node: 'sum' } } }, sum: { type: 'add', inputs: { a: { node: 'one' }, b: 2 } }, one: { type: 'constant', value: 1 }, unused: { type: 'constant', value: 9 } } } } };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  const preview = await operations.previewMaterialGraph('/World/Mesh');
  assert.deepEqual(preview.folded, ['one', 'sum']);
  assert.deepEqual(preview.removed, ['unused']);
  assert.equal(preview.changed, true);
});

test('mesh authoring preserves a compacted lightmap UV set', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), faceVaryingAttributes: [{ name: 'st1', itemSize: 2, array: new Float32Array([0, 0, 1, 0, 0, 1]), indices: new Uint32Array([0, 1, 2]) }] });
  assert.match(session.usda, /texCoord2f\[\] primvars:st1/);
  assert.match(session.usda, /int\[\] primvars:st1:indices = \[0, 1, 2\]/);
});

test('retopology refuses unindexed authored lightmap UVs', async () => {
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } } };
  const operations = new LuciaOperations({ getMeshUVData: (_path, set) => set === 'lightmap' ? { uvs: new Float32Array([0, 0, 1, 0, 0, 1]) } : null }, {}, { objectForPath: () => mesh });
  await assert.rejects(() => operations.retopo('/World/Mesh'), (error) => error.code === 'LUCIA_RETOPO_LIGHTMAP');
});

test('material graph operation exposes a read-only translation preview', async () => {
  const mesh = { isMesh: true, material: { userData: { nodes: { shader: { type: 'standard_surface', inputs: { base_color: .2 } } } } } };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  const preview = await operations.previewMaterialGraphTranslation('/World/Mesh', { from: 'materialx-standard-surface', to: 'usd-preview-surface' });
  assert.equal(preview.nodes[0].type, 'usdpreviewsurface');
  assert.deepEqual(preview.nodes[0].inputs, { diffuseColor: .2 });
  assert.deepEqual(mesh.material.userData.nodes.shader.inputs, { base_color: .2 });
});

test('material parameterization operation extracts selected mesh materials', async () => {
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } }, material: [
    { name: 'Blue', color: { r: .1, g: .2, b: .3 }, roughness: .4 },
    { name: 'Red', color: { r: .8, g: .1, b: .05 }, roughness: .4 },
  ] };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  const preview = await operations.previewMaterialParameterization('/World/Mesh', { targetProfile: 'web-viewer' });
  assert.deepEqual(preview.candidates.map(({ channel, parameter }) => ({ channel, parameter })), [{ channel: 'baseColor', parameter: 'primvars:lucia:baseColor' }]);
  assert.equal(mesh.material[0].name, 'Blue');
});

test('material parameterization extracts literal graph channel values', async () => {
  const graphMaterial = (path, value) => ({ userData: { 'primMeta.absPath': path, nodes: { output: { type: 'output', inputs: { surface: { node: 'surface' } } }, surface: { type: 'standard_surface', inputs: { base_color: { node: 'base' } } }, base: { type: 'color', value } } } });
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } }, material: [graphMaterial('/Materials/Blue', [0, 0, 1]), graphMaterial('/Materials/Red', [1, 0, 0])] };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  const preview = await operations.previewMaterialParameterization('/World/Mesh', { channels: ['baseColor'] });
  assert.deepEqual(preview.candidates.map(({ channel, materialPaths, values }) => ({ channel, materialPaths, values })), [{ channel: 'baseColor', materialPaths: ['/Materials/Blue', '/Materials/Red'], values: [[0, 0, 1], [1, 0, 0]] }]);
});

test('material parameterization preserves scalar graph channels', async () => {
  const graphMaterial = (path, value) => ({ userData: { 'primMeta.absPath': path, nodes: { output: { type: 'output', inputs: { surface: { node: 'surface' } } }, surface: { type: 'standard_surface', inputs: { roughness: { node: 'roughness' } } }, roughness: { type: 'float', value } } } });
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } }, material: [graphMaterial('/Materials/MatA', .2), graphMaterial('/Materials/MatB', .8)] };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  const preview = await operations.previewMaterialParameterization('/World/Mesh', { channels: ['roughness'] });
  assert.deepEqual(preview.candidates.map(({ channel, values }) => ({ channel, values })), [{ channel: 'roughness', values: [.2, .8] }]);
});

test('material parameterization authors scalar graph channels as scalar primvars', async () => {
  const graphMaterial = (path, value) => ({ userData: { 'primMeta.absPath': path, nodes: { output: { type: 'output', inputs: { surface: { node: 'surface' } } }, surface: { type: 'standard_surface', inputs: { roughness: { node: 'roughness' } } }, roughness: { type: 'float', value } } } });
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 6, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0]) } }, index: { count: 6, array: new Uint32Array([0, 1, 2, 3, 4, 5]) }, groups: [{ start: 0, count: 3, materialIndex: 0 }, { start: 3, count: 3, materialIndex: 1 }] }, material: [graphMaterial('/Materials/MatA', .2), graphMaterial('/Materials/MatB', .8)] };
  let authored;
  const session = { exportUSDA: () => '#usda 1.0', getMeshMaterialGroups: () => mesh.geometry.groups, setMeshGeometry: async (_path, data) => { authored = data; }, restore: async () => {} };
  const operations = new LuciaOperations(session, {}, { objectForPath: () => mesh });
  await operations.parameterizeMaterials('/World/Mesh', { channels: ['roughness'] });
  const [attribute] = authored.faceVaryingAttributes;
  assert.equal(attribute.name, 'lucia:roughness'); assert.equal(attribute.itemSize, 1); assert.deepEqual([...attribute.array].map((value) => Number(value.toFixed(5))), [.2, .2, .2, .8, .8, .8]);
});

test('material parameterization wires typed USDA primvar readers', () => {
  const source = 'def Material "Mat" { token outputs:surface.connect = <./Surface.outputs:surface> def Shader "Surface" { token info:id = "UsdPreviewSurface" color3f inputs:diffuseColor = (0.8, 0.2, 0.1) float inputs:roughness = 0.5 } }';
  const result = wireMaterialPrimvarParameters(source, ['/Mat'], ['baseColor', 'roughness']);
  assert.match(result, /color3f inputs:diffuseColor = <\.\/LuciaPrimvar_baseColor\.outputs:result>/);
  assert.match(result, /float inputs:roughness = <\.\/LuciaPrimvar_roughness\.outputs:result>/);
  assert.match(result, /UsdPrimvarReader_color3f/);
  assert.match(result, /UsdPrimvarReader_float/);
  assert.match(result, /token inputs:varname = "lucia:baseColor"/);
  assert.match(result, /token inputs:varname = "lucia:roughness"/);
});

test('material parameterization operation authors reviewed primvar channels', async () => {
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 6, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0]) }, }, index: { count: 6, array: new Uint32Array([0, 1, 2, 3, 4, 5]) }, groups: [{ start: 0, count: 3, materialIndex: 0 }, { start: 3, count: 3, materialIndex: 1 }] }, material: [{ name: 'Red', color: { r: 1, g: 0, b: 0 } }, { name: 'Blue', color: { r: 0, g: 0, b: 1 } }] };
  const authored = {};
  const session = { exportUSDA: () => '#usda 1.0', getMeshMaterialGroups: () => mesh.geometry.groups, setMeshGeometry: async (_path, data) => { authored.data = data; }, restore: async () => {} };
  const operations = new LuciaOperations(session, {}, { objectForPath: () => mesh });
  const original = await operations.parameterizeMaterials('/World/Mesh', { channels: ['baseColor'] });
  assert.equal(original, '#usda 1.0');
  assert.deepEqual(authored.data.faceVaryingAttributes.map(({ name, itemSize, interpolation }) => ({ name, itemSize, interpolation })), [{ name: 'lucia:baseColor', itemSize: 3, interpolation: 'faceVarying' }]);
  assert.deepEqual([...authored.data.faceVaryingAttributes[0].array], [1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1]);
  assert.equal(operations.lastStats.kind, 'material-parameterization');
});

test('material parameterization restores the original stage when authoring fails', async () => {
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } }, material: [
    { name: 'Red', color: { r: 1, g: 0, b: 0 } },
    { name: 'Blue', color: { r: 0, g: 0, b: 1 } },
  ] };
  let restored = null;
  const session = {
    exportUSDA: () => '#usda 1.0\ndef X "Mesh" {}',
    getMeshMaterialGroups: () => [{ start: 0, count: 3, materialIndex: 0 }],
    setMeshGeometry: async () => { throw new Error('authoring failed'); },
    restore: async (source) => { restored = source; },
  };
  const operations = new LuciaOperations(session, {}, { objectForPath: () => mesh });
  await assert.rejects(() => operations.parameterizeMaterials('/World/Mesh', { channels: ['baseColor'] }), /authoring failed/);
  assert.equal(restored, '#usda 1.0\ndef X "Mesh" {}');
});

test('material parameterization rejects ambiguous multi-material meshes', async () => {
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } }, material: [
    { name: 'Red', color: { r: 1, g: 0, b: 0 } },
    { name: 'Blue', color: { r: 0, g: 0, b: 1 } },
  ] };
  const operations = new LuciaOperations({ exportUSDA: () => '#usda 1.0' }, {}, { objectForPath: () => mesh });
  await assert.rejects(() => operations.parameterizeMaterials('/World/Mesh', { channels: ['baseColor'] }), (error) => error.code === 'LUCIA_MATERIAL_GROUPS');
});

test('material variant parameterization maps reviewed materials to stable variant names', () => {
  const candidate = planMaterialParameterization({ targetProfile: 'web-viewer', mode: 'variant', channels: ['baseColor'], materials: [
    { path: '/Materials/Red', channels: { baseColor: [1, 0, 0] } },
    { path: '/Materials/Blue', channels: { baseColor: [0, 0, 1] } },
  ] }).candidates[0];
  assert.deepEqual(materializeVariantParameterization({ candidate, materialPaths: ['/Materials/Red', '/Materials/Blue'] }), {
    name: 'lucia:baseColor',
    channel: 'baseColor',
    variants: [
      { name: 'material_Blue', materialPath: '/Materials/Blue' },
      { name: 'material_Red', materialPath: '/Materials/Red' },
    ],
  });
});

test('material parameterization operation authors explicit USD material variants', async () => {
  const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } }, material: [
    { color: { r: 1, g: 0, b: 0 }, userData: { 'primMeta.absPath': '/Materials/Red' } },
    { color: { r: 0, g: 0, b: 1 }, userData: { 'primMeta.absPath': '/Materials/Blue' } },
  ] };
  const authored = [];
  const session = { exportUSDA: () => '#usda 1.0', getMeshMaterialPaths: () => ['/Materials/Red', '/Materials/Blue'], setMaterialVariants: async (_path, variants, variantSet) => authored.push({ variants, variantSet }), restore: async () => {} };
  const operations = new LuciaOperations(session, {}, { objectForPath: () => mesh });
  assert.equal(await operations.parameterizeMaterials('/World/Mesh', { mode: 'variant', channels: ['baseColor'] }), '#usda 1.0');
  assert.deepEqual(authored, [{ variantSet: 'lucia_baseColor', variants: [{ name: 'material_Blue', materialPath: '/Materials/Blue' }, { name: 'material_Red', materialPath: '/Materials/Red' }] }]);
  assert.equal(operations.lastStats.mode, 'variant');
});

test('material variant parameterization rejects malformed candidate values', () => {
  const candidate = planMaterialParameterization({ mode: 'variant', channels: ['baseColor'], materials: [
    { path: '/Materials/Red', channels: { baseColor: [1, 0, 0] } },
    { path: '/Materials/Blue', channels: { baseColor: [0, 0, 1] } },
  ] }).candidates[0];
  assert.throws(() => materializeVariantParameterization({ candidate: { ...candidate, values: [candidate.values[0], [NaN, 0, 0]] }, materialPaths: candidate.materialPaths }), /candidate values are malformed/);
});

test('component operation exposes a read-only crack-merge preview', async () => {
  const mesh = { isMesh: true, geometry: {
    attributes: { position: { count: 6, itemSize: 3, array: new Float32Array([.047, 0, 0, 1, 0, 0, .047, 1, 0, .049, 0, 0, 1.001, 0, 0, .049, 1.001, 0]) } },
    index: { count: 6, array: new Uint16Array([0, 1, 2, 3, 4, 5]) },
  } };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  const preview = await operations.previewCrackMerge('/World/Mesh', { tolerance: 0.1 });
  assert.equal(preview.beforeComponents, 2);
  assert.equal(preview.mergedComponents, 1);
});

test('component operation applies read-only merge and discard corrections', async () => {
  const mesh = { isMesh: true, geometry: {
    attributes: { position: { count: 9, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0, 20, 0, 0, 21, 0, 0, 20, 1, 0]) } },
    index: { count: 9, array: new Uint16Array([0, 1, 2, 3, 4, 5, 6, 7, 8]) },
  } };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  const corrected = await operations.correctConnectedComponentsPreview('/World/Mesh', { mergeGroups: [[0, 2]], discard: [1] });
  assert.deepEqual([...corrected.faceLabels], [0, -1, 0]);
  assert.deepEqual(corrected.components[0].sourceIds, [0, 2]);
});

test('assistant proposes crack merging with an explicit tolerance', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('merge cracks within tolerance 0.02'), { name: 'scene.merge_cracks', arguments: { path: '/World/Mesh', tolerance: 0.02 } });
  assert.throws(() => assistant.validate({ name: 'scene.merge_cracks', arguments: { path: '/World/Mesh', tolerance: 0 } }), /positive finite/);
});

test('assistant validates and proposes component corrections', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('merge components 0+2 and discard 1'), { name: 'scene.correct_components', arguments: { path: '/World/Mesh', mergeGroups: [[0, 2]], discard: [1] } });
  assert.equal(assistant.validate({ name: 'scene.correct_components', arguments: { path: '/World/Mesh', mergeGroups: [[0, 2]], discard: [1] } }).confirm, true);
  assert.equal(assistant.validate({ name: 'scene.correct_components', arguments: { path: '/World/Mesh', mergeGroups: [[0, 2]], discard: [1], asChildren: true } }).confirm, true);
  assert.throws(() => assistant.validate({ name: 'scene.correct_components', arguments: { path: '/World/Mesh', mergeGroups: [[0]], discard: [] } }), /at least two/);
});

test('assistant requires confirmation and safe names for component part renames', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('rename component 0 to Wheel and part 1 as Door'), { name: 'scene.rename_component_parts', arguments: { path: '/World/Mesh', names: { 0: 'Wheel', 1: 'Door' } } });
  assert.equal(assistant.validate({ name: 'scene.rename_component_parts', arguments: { path: '/World/Mesh', names: { 0: 'Wheel', 1: 'Door' }, asChildren: true } }).confirm, true);
  assert.throws(() => assistant.validate({ name: 'scene.rename_component_parts', arguments: { path: '/World/Mesh', names: { 0: 'Wheel', 1: 'Wheel' } } }), /unique/);
  assert.throws(() => assistant.validate({ name: 'scene.rename_component_parts', arguments: { path: '/World/Mesh', names: { 0: 'bad/name' } } }), /valid USD names/);
});

test('component authoring hierarchy mode is explicitly validated', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.equal(assistant.validate({ name: 'scene.split_components', arguments: { path: '/World/Mesh', asChildren: true } }).confirm, true);
  assert.throws(() => assistant.validate({ name: 'scene.split_components', arguments: { path: '/World/Mesh', asChildren: 'yes' } }), /asChildren must be boolean/);
});

test('assistant exposes read-only component naming proposals', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('suggest names for the components'), { name: 'scene.propose_component_names', arguments: { path: '/World/Mesh', minFaces: 1 } });
  assert.equal(assistant.validate({ name: 'scene.propose_component_names', arguments: { path: '/World/Mesh' } }).confirm, false);
});

test('projection normals use inverse-transpose transform and remain normalized', () => {
  const normals = transformProjectionNormals(new Float32Array([1, 0, 0]), new Float32Array([2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1]));
  assert.deepEqual([...normals].map((value) => Number(value.toFixed(6))), [1, 0, 0]);
  const diagonal = transformProjectionNormals(new Float32Array([1, 1, 0]), new Float32Array([2, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]));
  assert.ok(Math.abs(diagonal[0] - 0.894427) < 1e-5);
  assert.ok(Math.abs(diagonal[1] - 0.447214) < 1e-5);
});

test('projection tangents use world linear transforms and preserve handedness', () => {
  const transformed = transformProjectionTangents(new Float32Array([1, 0, 0, -1]), new Float32Array([0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1]));
  assert.deepEqual([...transformed], [0, 1, 0, -1]);
  const mirrored = transformProjectionTangents(new Float32Array([1, 0, 0, 1]), new Float32Array([-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]));
  assert.deepEqual([...mirrored], [-1, 0, 0, -1]);
});

test('source UV transfer chooses the nearest valid normal-ray hit', () => {
  const result = transferUVsByProjection({
    targetPositions: new Float32Array([.25, .25, 0]),
    targetNormals: new Float32Array([0, 0, 1]),
    sourcePositions: new Float32Array([0, 0, 1, 1, 0, 1, 0, 1, 1]),
    sourceIndices: new Uint32Array([0, 1, 2]),
    sourceUVs: new Float32Array([0, 0, 1, 0, 0, 1]),
    maxDistance: 2,
  });
  assert.equal(result.hitCount, 1);
  assert.equal(result.missCount, 0);
  assert.deepEqual([...result.hitMask], [1]);
  assert.ok(Math.abs(result.uvs[0] - .25) < 1e-5);
  assert.ok(Math.abs(result.uvs[1] - .25) < 1e-5);
  assert.equal(transferUVsByProjection({ targetPositions: new Float32Array([2, 2, 0]), targetNormals: new Float32Array([0, 0, 1]), sourcePositions: new Float32Array([0, 0, 1, 1, 0, 1, 0, 1, 1]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 0, 1, 0, 0, 1]), maxDistance: .5 }).missCount, 1);
});

test('UV transfer worker results are bounded and finite', () => {
  const valid = { uvs: new Float32Array([0, 0, .5, .5]), hitMask: new Uint8Array([1, 0]), hitCount: 1, missCount: 1 };
  assert.equal(validateUVTransferResult(valid, 2), valid);
  assert.throws(() => validateUVTransferResult({ ...valid, uvs: new Float32Array([0, Infinity, .5, .5]) }, 2), /malformed/);
  assert.throws(() => validateUVTransferResult({ ...valid, missCount: 0 }, 2), /malformed/);
  assert.throws(() => validateUVTransferResult({ ...valid, hitMask: new Uint8Array([1, 1]) }, 2), /malformed/);
  assert.throws(() => validateUVTransferResult({ ...valid, uvs: [...valid.uvs] }, 2), /untyped/);
});

test('UV transfer preserves original coordinates for per-vertex misses', () => {
  const merged = mergeTransferredUVs(new Float32Array([0, .25, 0, 0, .75, .5]), new Float32Array([.5, .75, .25, .5, 1, 1]), new Uint8Array([1, 0, 1]));
  assert.deepEqual([...merged], [0, .25, .25, .5, .75, .5]);
  assert.throws(() => mergeTransferredUVs(new Float32Array(2), new Float32Array(4), new Uint8Array(1)), /incompatible shapes/);
});

test('UV transfer rejects zero-length target normals', () => {
  const normals = new Float32Array([0, 0, 1, 0, 0, 1]);
  assert.equal(validateUVTransferNormals(normals, 2), normals);
  assert.throws(() => validateUVTransferNormals(new Float32Array([0, 0, 0]), 1), /non-zero/);
  assert.throws(() => validateUVTransferNormals(new Float32Array([0, 0, 1]), 2), /one normal/);
});

test('UV transfer normal fallback is deterministic for a UV-less target', () => {
  const normals = recomputeVertexNormals({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) });
  assert.deepEqual([...normals], [0, 0, 1, 0, 0, 1, 0, 0, 1]);
});

test('project revision advances on scene changes and resets', () => {
  const project = new LuciaProject(), initial = project.revision;
  project.changed('test', ['/World/Mesh']);
  assert.equal(project.revision, initial + 1);
  project.reset('next.usda');
  assert.equal(project.revision, initial + 2);
});

test('project tracks domain revisions for selective report invalidation', () => {
  const project = new LuciaProject(), before = { ...project.domainRevisions };
  project.changed('USD metadata repair', ['/'], ['usd']);
  assert.equal(project.domainRevisions.scene, before.scene);
  assert.equal(project.domainRevisions.assets, before.assets);
  assert.equal(project.domainRevisions.usd, before.usd + 1);
});

test('project reset preserves host resolver plugin registrations', () => {
  const project = new LuciaProject();
  project.setResolverPlugins([{ scheme: 'resolver', name: 'Library' }]);
  project.reset('new-scene.usda', '#usda 1.0');
  assert.deepEqual(project.resolverPlugins, [{ scheme: 'resolver', name: 'Library' }]);
});

test('project resolver registration invalidates only USD diagnostics without dirtying the scene', () => {
  const project = new LuciaProject(), before = { ...project.domainRevisions }, events = [];
  project.addEventListener('change', () => events.push(true));
  assert.deepEqual(project.setResolverPlugins([{ scheme: 'Resolver', name: 'Library' }, { scheme: 'resolver', name: 'Library' }]), [{ scheme: 'resolver', name: 'Library' }]);
  assert.equal(project.dirty, false);
  assert.equal(project.domainRevisions.usd, before.usd + 1);
  assert.equal(project.domainRevisions.assets, before.assets);
  assert.equal(events.length, 1);
});

test('project resolver registration is idempotent after normalization', () => {
  const project = new LuciaProject(), before = { ...project.domainRevisions }, events = [];
  project.addEventListener('change', () => events.push(true));
  project.setResolverPlugins([{ scheme: 'resolver', name: 'Library', capabilities: ['USD'] }]);
  project.setResolverPlugins([{ scheme: 'RESOLVER:', name: 'Library', capabilities: ['usd'] }]);
  assert.deepEqual(project.domainRevisions, { ...before, usd: before.usd + 1 });
  assert.equal(events.length, 1);
});

test('project records assistant proposals and decisions without marking the scene dirty', () => {
  const project = new LuciaProject();
  project.recordAssistantDecision({ name: 'scene.retopo', arguments: { path: '/World/Mesh' } }, 'proposed');
  project.recordAssistantDecision({ name: 'scene.localize_dependencies', arguments: { mapping: { '/private/asset.png': 'textures/asset.png' } } }, 'declined');
  assert.equal(project.dirty, false);
  assert.deepEqual(project.activity.map(({ kind, decision, tool, paths }) => ({ kind, decision, tool, paths })), [
    { kind: 'assistant', decision: 'proposed', tool: 'scene.retopo', paths: ['/World/Mesh'] },
    { kind: 'assistant', decision: 'declined', tool: 'scene.localize_dependencies', paths: [] },
  ]);
  project.changed('Applied repair', ['/World/Mesh']);
  assert.equal(project.activity.length, 3);
  const instanceDecision = project.recordAssistantDecision({ name: 'scene.create_instance', arguments: { sourcePath: '/World/Source', transformSourcePath: '/World/Duplicate', name: 'Source_Instance' } }, 'proposed');
  assert.deepEqual(instanceDecision.paths, ['/World/Duplicate', '/World/Source']);
  project.reset('new');
  assert.equal(project.activity.length, 0);
});

test('semantic suggestions are deterministic, conservative, and read-only', () => {
  const report = { meshes: [{ path: '/World/Wheel_FL', components: 1 }, { path: '/World/Door', components: 2 }, { path: '/World/Unknown', components: 1, duplicateOf: '/World/Source' }] };
  const suggestions = proposeSemanticSuggestions(report);
  assert.deepEqual(suggestions.map(({ path, kind, role, confidence, sourcePath }) => ({ path, kind, role, confidence, ...(sourcePath ? { sourcePath } : {}) })), [
    { path: '/World/Door', kind: 'semantic-role', role: 'door', confidence: 0.86 },
    { path: '/World/Door', kind: 'grouping-candidate', role: 'group', confidence: 0.76 },
    { path: '/World/Unknown', kind: 'instancing-candidate', role: 'instance', confidence: 0.8, sourcePath: '/World/Source' },
    { path: '/World/Wheel_FL', kind: 'semantic-role', role: 'wheel', confidence: 0.92 },
  ]);
  assert.equal(suggestions.every((item) => item.deterministic && item.authoringRequired), true);
  assert.deepEqual(suggestions.find((item) => item.kind === 'semantic-role').hierarchy, { mode: 'child', parentPath: '/World/Door', rationale: 'Keep any reviewed semantic role authoring below the existing mesh path until hierarchy changes are explicitly approved.' });
  assert.deepEqual(suggestions.find((item) => item.kind === 'grouping-candidate').hierarchy, { mode: 'child', parentPath: '/World/Door', rationale: 'Keep generated component prims below their source mesh so placement and material review remain local and undoable.' });
  assert.deepEqual(proposeSemanticSuggestions({ meshes: [...report.meshes].reverse() }), suggestions);
  assert.deepEqual(proposeSemanticSuggestions({ meshes: [null, 4, [], ...report.meshes] }).map(({ path, kind }) => ({ path, kind })), suggestions.map(({ path, kind }) => ({ path, kind })));
});

test('semantic suggestions ignore malformed mesh paths', () => {
  const suggestions = proposeSemanticSuggestions({ meshes: [{ path: 'Wheel', components: 1 }, { path: '/World/Wheel', components: 1 }] });
  assert.deepEqual(suggestions.map(({ path, kind }) => ({ path, kind })), [{ path: '/World/Wheel', kind: 'semantic-role' }]);
});

test('component naming suggestions are stable and collision-safe', () => {
  const preview = { components: [{ id: 0, faces: [2, 0] }, { id: 1, faces: [1] }] };
  const suggestions = proposeComponentNames('/World/Car-Body', preview, ['Car_Body_Part1']);
  assert.deepEqual(suggestions.map(({ id, name, sourceFaces, confidence, deterministic, authoringRequired }) => ({ id, name, sourceFaces, confidence, deterministic, authoringRequired })), [
    { id: 0, name: 'Car_Body_Part1_0', sourceFaces: [2, 0], confidence: 0.5, deterministic: true, authoringRequired: true },
    { id: 1, name: 'Car_Body_Part2', sourceFaces: [1], confidence: 0.5, deterministic: true, authoringRequired: true },
  ]);
  const proposal = proposeComponentNames('/World/Car-Body', { components: [{ id: 0, faces: [], boundaryEdges: 4, nonManifoldEdges: 0, normalDeviationDegrees: 3.25, maxDihedralDegrees: 2.5, curvatureEdges: 1, nearlyPlanar: true }] })[0];
  assert.deepEqual(proposal.geometryEvidence, { boundaryEdges: 4, nonManifoldEdges: 0, boundaryLoops: null, planarBoundaryLoops: null, normalDeviationDegrees: 3.25, maxDihedralDegrees: 2.5, curvatureEdges: 1, concavityReliable: null, concaveEdges: null, convexEdges: null, nearlyPlanar: true });
  assert.deepEqual(proposal.hierarchy, { mode: 'child', parentPath: '/World/Car-Body', rationale: 'Keep the generated component below its source mesh so placement and material review remain local and undoable.' });
  assert.deepEqual(proposeComponentNames('/World/Car-Body', { components: [] }), []);
  assert.equal(proposeComponentNames('/World/Car-Body', { components: [{ id: 0, faces: [] }] }, ['Car_Body_Part1', 'Car_Body_Part1_0'])[0].name, 'Car_Body_Part1_0_2');
  assert.equal(proposeComponentNames('/World/Car-Body', { components: [{ id: -1, faces: [] }] }, ['Car_Body_Part1'])[0].name, 'Car_Body_Part1_0');
  assert.equal(proposeComponentNames('not/a/usd/path', { components: [{ id: 0, faces: [] }] }, new Set(['path_Part1']))[0].hierarchy.parentPath, '/');
});

test('component authoring name allocator handles existing and repeated collisions', () => {
  const reserved = new Set(['Mesh_Part1', 'Mesh_Part1_0', 'Mesh_Part1_0_2']);
  assert.equal(uniqueComponentName('Mesh_Part1', 0, reserved), 'Mesh_Part1_0_3');
  assert.equal(uniqueComponentName('Mesh_Part2', 1, reserved), 'Mesh_Part2');
});

test('component naming uses source role evidence without auto-authoring', () => {
  const [proposal] = proposeComponentNames('/World/Wheel', { components: [{ id: 0, faces: [0], nearlyPlanar: false }] });
  assert.equal(proposal.name, 'Wheel_Part1');
  assert.equal(proposal.confidence, 0.864);
  assert.match(proposal.rationale, /role token/);
  assert.equal(proposal.authoringRequired, true);
});

test('component naming uses conservative planar boundary evidence', () => {
  const [proposal] = proposeComponentNames('/World/Unknown', { components: [{ id: 0, faces: [0], nearlyPlanar: true, planarBoundaryLoops: 1 }] });
  assert.equal(proposal.name, 'Panel_Part1');
  assert.equal(proposal.confidence, .64);
  assert.match(proposal.rationale, /planar boundary loop/);
});

test('semantic geometry suggestions expose bounded pivot and orientation data', () => {
  const suggestions = proposeSemanticSuggestions({ meshes: [{ path: '/World/Elongated', components: 3, bounds: { center: [2, 0, 0], size: [8, 1, 1] } }] });
  assert.deepEqual(suggestions.map(({ kind, role, confidence, pivot, axis, componentCount }) => ({ kind, role, confidence, ...(pivot ? { pivot } : {}), ...(axis ? { axis } : {}), ...(componentCount ? { componentCount } : {}) })), [
    { kind: 'pivot-candidate', role: 'pivot', confidence: 0.74, pivot: [2, 0, 0] },
    { kind: 'orientation-candidate', role: 'orientation', confidence: 0.82, axis: 'X' },
    { kind: 'grouping-candidate', role: 'group', confidence: 0.76, componentCount: 3 },
  ]);
  assert.equal(suggestions.every((item) => item.deterministic && item.authoringRequired), true);
});

test('semantic geometry suggestions ignore malformed bounds', () => {
  const suggestions = proposeSemanticSuggestions({ meshes: [{ path: '/World/BadBounds', bounds: { center: [1, 2], size: [Infinity, 1, 1] } }] });
  assert.deepEqual(suggestions, []);
});

test('normal recompute rejects unsupported shading policies', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) };
  assert.throws(() => recomputeVertexNormals({ ...mesh, weighting: 'uniform' }), /weighting must be area or angle/);
  assert.throws(() => recomputeVertexNormals({ ...mesh, smoothingAngle: '180' }), /smoothingAngle must be a finite number/);
  assert.throws(() => recomputeVertexNormals({ ...mesh, smoothingAngle: 181 }), /smoothingAngle must be a finite number/);
});

test('semantic role suggestions recognize plural mesh-name tokens', () => {
  const suggestions = proposeSemanticSuggestions({ meshes: [{ path: '/World/Wheels_FL', components: 1 }, { path: '/World/Door_Handles', components: 1 }, { path: '/World/Headlights', components: 1 }] });
  assert.deepEqual(suggestions.filter((item) => item.kind === 'semantic-role').map(({ path, role, confidence }) => ({ path, role, confidence })), [
    { path: '/World/Door_Handles', role: 'door', confidence: 0.9 },
    { path: '/World/Headlights', role: 'lighting', confidence: 0.94 },
    { path: '/World/Wheels_FL', role: 'wheel', confidence: 0.92 },
  ]);
});

test('semantic suggestions accept only explicitly approved validated inference', () => {
  const inference = [{ path: '/World/Panel', kind: 'name-candidate', role: 'panel', label: 'panel candidate', confidence: .71, rationale: 'Approved model review identified a panel-like part.', suggestedName: 'Panel_Main', hierarchy: { mode: 'child', parentPath: '/World/Panel', rationale: 'Keep the inferred name below the reviewed source prim.' }, source: 'model' }, { path: '/World/Bad', kind: 'name-candidate', role: 'bad', label: 'bad', confidence: 2, rationale: 'invalid', source: 'image' }, { path: '/World/StringConfidence', kind: 'name-candidate', role: 'panel', label: 'bad', confidence: '0.5', rationale: 'invalid', source: 'image' }, { path: '/World/BadName', kind: 'name-candidate', role: 'panel', label: 'bad', confidence: .5, rationale: 'invalid', suggestedName: 'bad/name', source: 'image' }];
  assert.equal(proposeSemanticSuggestions({ meshes: [] }, { inference }).length, 0);
  const suggestions = proposeSemanticSuggestions({ meshes: [] }, { approvedInference: true, inference });
  assert.deepEqual(suggestions, [{ path: '/World/Panel', kind: 'name-candidate', role: 'panel', label: 'panel candidate', confidence: .71, rationale: 'Approved model review identified a panel-like part.', suggestedName: 'Panel_Main', hierarchy: { mode: 'child', parentPath: '/World/Panel', rationale: 'Keep the inferred name below the reviewed source prim.' }, deterministic: false, authoringRequired: true, inferenceApproved: true, inferenceSource: 'model' }]);
});

test('project supports additive selection while preserving a primary path', () => {
  const project = new LuciaProject();
  project.select('/World/A');
  project.select('/World/B', { additive: true });
  assert.deepEqual([...project.selectedPaths], ['/World/A', '/World/B']);
  assert.equal(project.selectedPath, '/World/B');
  project.select('/World/B', { additive: true });
  assert.deepEqual([...project.selectedPaths], ['/World/A']);
  assert.equal(project.selectedPath, '/World/A');
  project.select('/World/C');
  assert.deepEqual([...project.selectedPaths], ['/World/C']);
  project.select('/World/D', { additive: true });
  project.remapSelection('/World/D', '/World/Renamed');
  assert.deepEqual([...project.selectedPaths], ['/World/C', '/World/Renamed']);
  project.removeSelection('/World/C', '/World');
  assert.deepEqual([...project.selectedPaths], ['/World/Renamed']);
  project.select('/World/E', { range: ['/World/E', '/World/F'] });
  assert.deepEqual([...project.selectedPaths], ['/World/E', '/World/F']);
});

test('repair plan is deterministic and orders dependent shading fixes', () => {
  const report = { issues: [
    { fix: 'mesh.recomputeTangents', path: '/World/Mesh', ruleId: 'tangent.missing' },
    { fix: 'mesh.recomputeNormals', path: '/World/Mesh', ruleId: 'normal.nonFinite' },
    { fix: 'mesh.cleanup', path: '/World/Mesh', ruleId: 'topology.duplicateIndex' },
    { fix: 'unsupported.fix', path: '/World/Mesh', ruleId: 'ignored' },
  ] };
  const plan = proposeRepairPlan(report);
  assert.deepEqual(plan.operations.map((item) => item.fix), ['mesh.cleanup', 'mesh.recomputeNormals', 'mesh.recomputeTangents']);
  assert.deepEqual(plan.operations[0].options, { preset: 'game-ready' });
  assert.deepEqual(plan.operations[1].dependsOn, ['mesh.cleanup:/World/Mesh']);
  assert.deepEqual(plan.operations[2].dependsOn, ['mesh.recomputeNormals:/World/Mesh']);
  assert.deepEqual(proposeRepairPlan(report), plan);
});

test('repair plan proposes disconnected-part extraction', () => {
  const report = { issues: [{ fix: 'mesh.splitComponents', path: '/World/Fused', ruleId: 'topology.disconnectedComponent' }] };
  const plan = proposeRepairPlan(report);
  assert.deepEqual(plan.operations, [{ id: 'mesh.splitComponents:/World/Fused', fix: 'mesh.splitComponents', path: '/World/Fused', title: 'Split disconnected parts', benefit: 'Separate disconnected mesh components into editable part siblings.', destructive: true, cost: 'medium', order: 15, options: { minFaces: 1 }, issueIds: ['topology.disconnectedComponent'], dependsOn: [], estimatedCost: 'medium', estimatedWork: 2 }]);
  const ordered = proposeRepairPlan({ issues: [{ fix: 'mesh.splitComponents', path: '/World/Fused', ruleId: 'topology.disconnectedComponent' }, { fix: 'mesh.cleanup', path: '/World/Fused', ruleId: 'topology.duplicateIndex' }] }).operations;
  assert.deepEqual(ordered.map((item) => item.fix), ['mesh.cleanup', 'mesh.splitComponents']);
  assert.deepEqual(ordered[1].dependsOn, ['mesh.cleanup:/World/Fused']);
});

test('repair operation applicability skips resolved analyzer issues', () => {
  const operation = { fix: 'mesh.cleanup', path: '/World/Mesh', issueIds: ['topology.duplicateIndex'] };
  assert.equal(repairOperationNeeded({ issues: [{ path: '/World/Mesh', fix: 'mesh.cleanup', ruleId: 'topology.duplicateIndex' }] }, operation), true);
  assert.equal(repairOperationNeeded({ issues: [] }, operation), false);
  assert.equal(repairOperationNeeded({ issues: [] }, { fix: 'mesh.cleanup', path: '/World/Mesh' }), true);
  assert.equal(repairOperationNeeded({ issues: [] }, { fix: 'mesh.cleanup', path: 7 }), false);
  assert.equal(repairOperationNeeded({ issues: [] }, { fix: 7, path: '/World/Mesh' }), false);
  assert.equal(repairOperationNeeded({ issues: [] }, { fix: 'mesh.cleanup', path: '/World/../Mesh' }), false);
});

test('repair plan summaries expose benefits, cost, work, destructive steps, and dependencies', () => {
  assert.equal(summarizeRepairPlan([
    { benefit: 'Fix topology.', estimatedCost: 'medium', estimatedWork: 12, destructive: true, dependsOn: [] },
    { benefit: 'Fix shading.', estimatedCost: 'high', estimatedWork: 8, destructive: true, dependsOn: ['mesh.cleanup:/World/Mesh'] },
  ]), 'Benefits: Fix shading. Fix topology. Cost levels: high, medium. Estimated work: 20 units. Destructive/undoable steps: 2/2. Dependencies: 1.');
  assert.match(summarizeRepairPlan(null), /No benefit description provided/);
});

test('repair plan exposes a deterministic dependency graph', () => {
  const plan = proposeRepairPlan({ issues: [
    { fix: 'mesh.recomputeTangents', path: '/World/Mesh', ruleId: 'tangent.missing' },
    { fix: 'mesh.recomputeNormals', path: '/World/Mesh', ruleId: 'normal.nonFinite' },
    { fix: 'mesh.cleanup', path: '/World/Mesh', ruleId: 'topology.duplicateIndex' },
  ] });
  assert.deepEqual(buildRepairOperationGraph(plan), {
    nodes: [
      { id: 'issue:normal.nonFinite:/World/Mesh', kind: 'issue', ruleId: 'normal.nonFinite', path: '/World/Mesh' },
      { id: 'issue:tangent.missing:/World/Mesh', kind: 'issue', ruleId: 'tangent.missing', path: '/World/Mesh' },
      { id: 'issue:topology.duplicateIndex:/World/Mesh', kind: 'issue', ruleId: 'topology.duplicateIndex', path: '/World/Mesh' },
      { id: 'mesh.cleanup:/World/Mesh', kind: 'operation', fix: 'mesh.cleanup', path: '/World/Mesh', title: 'Clean mesh topology', benefit: 'Remove degenerate, duplicate, and invalid faces.', destructive: true, cost: 'medium', estimatedWork: 2 },
      { id: 'mesh.recomputeNormals:/World/Mesh', kind: 'operation', fix: 'mesh.recomputeNormals', path: '/World/Mesh', title: 'Recompute vertex normals', benefit: 'Restore finite, consistently oriented shading normals.', destructive: true, cost: 'medium', estimatedWork: 2 },
      { id: 'mesh.recomputeTangents:/World/Mesh', kind: 'operation', fix: 'mesh.recomputeTangents', path: '/World/Mesh', title: 'Recompute vertex tangents', benefit: 'Restore a tangent frame for normal-map shading.', destructive: true, cost: 'medium', estimatedWork: 2 },
    ],
    edges: [
      { from: 'issue:normal.nonFinite:/World/Mesh', to: 'mesh.recomputeNormals:/World/Mesh', kind: 'triggers' },
      { from: 'issue:tangent.missing:/World/Mesh', to: 'mesh.recomputeTangents:/World/Mesh', kind: 'triggers' },
      { from: 'issue:topology.duplicateIndex:/World/Mesh', to: 'mesh.cleanup:/World/Mesh', kind: 'triggers' },
      { from: 'mesh.cleanup:/World/Mesh', to: 'mesh.recomputeNormals:/World/Mesh', kind: 'dependsOn' },
      { from: 'mesh.recomputeNormals:/World/Mesh', to: 'mesh.recomputeTangents:/World/Mesh', kind: 'dependsOn' },
    ],
  });
});

test('repair operation graphs deduplicate external recipe nodes and edges', () => {
  const graph = buildRepairOperationGraph({ operations: [
    { id: 'a', path: '/Mesh', issueIds: ['bad', 'bad'], dependsOn: [] },
    { id: 'a', path: '/Other', issueIds: ['ignored'], dependsOn: [] },
    { id: 'b', path: '/Mesh', issueIds: [], dependsOn: ['a', 'a'] },
  ] });
  assert.equal(graph.nodes.filter((node) => node.kind === 'operation').length, 2);
  assert.deepEqual(graph.edges, [
    { from: 'a', to: 'b', kind: 'dependsOn' },
    { from: 'issue:bad:/Mesh', to: 'a', kind: 'triggers' },
  ]);
});

test('repair plans discard malformed USD prim paths', () => {
  assert.deepEqual(proposeRepairPlan({ issues: [{ fix: 'mesh.cleanup', path: '/World/../Mesh', ruleId: 'topology.bad' }, { fix: 'mesh.cleanup', path: '/World/Mesh', ruleId: 'topology.good' }] }).operations.map((operation) => operation.path), ['/World/Mesh']);
  assert.deepEqual(buildRepairOperationGraph({ operations: [{ id: 'bad', path: '/World/../Mesh' }, { id: 'good', path: '/World/Mesh' }] }).nodes.map((node) => node.id), ['good']);
});

test('cleanup presets normalize deterministic scale-aware tolerances', () => {
  assert.deepEqual(normalizeCleanupOptions({ preset: 'preserve' }), { preset: 'preserve', tolerance: 0, minArea: 0, minComponentArea: 0, minComponentVolume: 0 });
  assert.deepEqual(normalizeCleanupOptions({ preset: 'physics-ready' }), { preset: 'physics-ready', tolerance: 0.001, minArea: 0, minComponentArea: 0, minComponentVolume: 0 });
  assert.deepEqual(normalizeCleanupOptions({ preset: 'unknown' }), { preset: 'game-ready', tolerance: 0.0001, minArea: 0, minComponentArea: 0, minComponentVolume: 0 });
  assert.deepEqual(normalizeCleanupOptions({ preset: 'aggressive', tolerance: 0.2, minArea: 2, minComponentArea: 2, minComponentVolume: 2 }), { preset: 'aggressive', tolerance: 0.1, minArea: 1, minComponentArea: 1, minComponentVolume: 1 });
});

test('cleanup result validation rejects malformed worker output', () => {
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0]), indices: new Uint32Array() }), /mismatched or empty/);
  assert.throws(() => validateMeshCleanupResult({ positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: new Uint32Array([0, 1, 2]) }), /untyped/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0]), indices: new Uint32Array([0, 0, 2]) }), /out-of-range/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), jointIndices: new Uint16Array(12) }), /together/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), jointIndices: new Uint32Array([65536, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), jointWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) }), /joint indices/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0]), indices: new Uint32Array([0, 0, 0]), uvs: new Float32Array([0, 0, 0]) }), /invalid uvs/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), jointIndices: new Uint16Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), jointWeights: new Float32Array([.5, 0, 0, 0, .5, 0, 0, 0, .5, 0, 0, 0]) }), /non-normalized joint weights/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), indices: new Uint32Array([0, 1, 2, 0, 2, 3]), groups: [{ start: 0, count: 6, materialIndex: 0 }, { start: 3, count: 3, materialIndex: 1 }] }), /overlap/);
  assert.throws(() => validateMeshCleanupResult({ positions: {}, indices: new Uint32Array([0, 1, 2]) }), /mismatched or empty/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), customAttributes: {} }), /malformed attribute/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), groups: {} }), /malformed attribute/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), customAttributes: [{ name: 'bad-name', itemSize: 1, array: new Float32Array([1, 2, 3]) }] }), /invalid or duplicate/);
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), customAttributes: [{ name: 'plain', itemSize: 1, array: [1, 2, 3] }] }), /invalid or duplicate/);
  const faceVarying = { name: 'st', itemSize: 2, array: new Float32Array([0, 0, 1, 0, 0, 1]), indices: new Uint32Array([0, 1, 2]) };
  assert.throws(() => validateMeshCleanupResult({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), faceVaryingAttributes: [faceVarying, { ...faceVarying }] }), /invalid or duplicate/);
});

test('safe mesh cleanup rejects malformed top-level input', () => {
  assert.throws(() => cleanupMesh(null), /mesh data object/);
  assert.throws(() => cleanupMesh([]), /mesh data object/);
  assert.throws(() => cleanupMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), fillPlanarHoles: 'false' }), /fillPlanarHoles must be boolean/);
});

test('safe mesh cleanup rejects malformed material groups', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) };
  assert.throws(() => cleanupMesh({ ...mesh, groups: {} }), /material groups must be an array/);
  assert.throws(() => cleanupMesh({ ...mesh, groups: [{ start: 1, count: 3, materialIndex: 0 }] }), /overlap or fall outside/);
  assert.throws(() => cleanupMesh({ ...mesh, groups: [{ start: 0, count: 3, materialIndex: -1 }] }), /overlap or fall outside/);
});

test('safe mesh cleanup rejects malformed attribute entries', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) };
  assert.throws(() => cleanupMesh({ ...mesh, customAttributes: [null] }), /custom attributes/);
  assert.throws(() => cleanupMesh({ ...mesh, faceVaryingAttributes: [{ name: 'st', itemSize: 2, array: {}, indices: new Uint32Array([0, 1, 2]) }] }), /face-varying attributes/);
  const attribute = { name: 'value', itemSize: 1, array: new Float32Array([1, 2, 3]) };
  assert.throws(() => cleanupMesh({ ...mesh, customAttributes: [attribute, { ...attribute }] }), /custom attributes/);
});

test('retopo and UV atlas result validation rejects unsafe skin indices and weights', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), xref: new Uint32Array([0, 1, 2]) };
  assert.throws(() => validateRetopoResult({ ...mesh, jointIndices: new Float32Array([0, 65536, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) }), /joint indices/);
  assert.throws(() => validateRetopoResult({ ...mesh, jointIndices: new Uint16Array(12), jointWeights: new Float32Array([1, -1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) }), /negative joint weights/);
  assert.throws(() => validateUVAtlasResult({ ...mesh, jointIndices: new Float32Array([0, 1.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), jointWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) }), /joint indices/);
  assert.throws(() => validateUVAtlasResult({ ...mesh, customAttributes: {} }), /malformed custom attribute/);
  assert.throws(() => validateUVAtlasResult({ ...mesh, customAttributes: [{ name: 'extra', itemSize: 1, array: {} }] }), /invalid custom attribute/);
  assert.throws(() => validateUVAtlasResult({ ...mesh, positions: {} }), /mismatched mesh/);
  assert.throws(() => validateUVAtlasResult({ ...mesh, xref: {} }), /source remap/);
  assert.throws(() => validateRetopoResult({ ...mesh, customAttributes: {} }), /malformed custom attribute/);
  assert.throws(() => validateRetopoResult({ ...mesh, indices: {} }), /mismatched or empty/);
});

test('bake input validation rejects malformed and non-finite mesh buffers', () => {
  const valid = { attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, uv: { count: 3, itemSize: 2, array: new Float32Array([0, 0, 1, 0, 0, 1]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } };
  assert.equal(validateBakeMeshData(valid), true);
  assert.equal(validateGeometryData(valid), true);
  assert.throws(() => validateGeometryData({ ...valid, attributes: { ...valid.attributes, position: { ...valid.attributes.position, array: [0, 0, 0, 1, 0, 0, 0, 1, 0] } } }), /supported typed/);
  assert.throws(() => validateGeometryData({ ...valid, index: { count: 3, array: [0, 1, 2] } }), /supported typed/);
  assert.throws(() => validateBakeMeshData({ ...valid, index: { count: 4, array: new Uint16Array([0, 1, 2, 0]) } }), /complete/);
  assert.throws(() => validateBakeMeshData({ ...valid, attributes: { ...valid.attributes, position: { ...valid.attributes.position, array: new Float32Array([0, 0, NaN, 1, 0, 0, 0, 1, 0]) } } }), /non-finite/);
});

test('worker output validation rejects detached or non-finite vectors', () => {
  assert.deepEqual([...validateWorkerVector(new Float32Array([1, 2, 3]), 3, 'TEST', 'Vector')], [1, 2, 3]);
  assert.throws(() => validateWorkerVector(new Float32Array([1, NaN, 3]), 3, 'TEST', 'Vector'), /invalid/);
  assert.throws(() => validateWorkerVector(new Float32Array(), 3, 'TEST', 'Vector'), /invalid/);
  assert.throws(() => validateWorkerVector({ length: 3, 0: 1, 1: 2, 2: 3 }, 3, 'TEST', 'Vector'), /invalid/);
  assert.throws(() => validateWorkerVector([1, 2, 3], 3, 'TEST', 'Vector'), /untyped/);
});

test('target profiles evaluate deterministic advisory budgets', () => {
  const report = { stage: { triangles: 200000, estimatedGpuBytes: 300 * 1024 * 1024, meshes: 1, uvMeshes: 0 }, textures: [{ width: 8192, height: 4096 }] };
  const mobile = evaluateTargetProfile(report, 'mobile-ar');
  assert.equal(mobile.profileVersion, 1);
  assert.equal(mobile.pass, false);
  assert.deepEqual(mobile.checks.map((item) => item.id), ['triangles', 'gpu-memory', 'transform-stability', 'materials', 'texture-dimension', 'texture-validity', 'uvs', 'physical-units', 'usd-metadata']);
  const unstable = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, nonFiniteTransforms: 1, extremeTransforms: 2, meshes: 1, uvMeshes: 1 }, meshes: [{ path: '/World/Mesh', nonFiniteTransform: 1, extremeTransform: 2 }], materials: [], textures: [] }, 'web-viewer').checks.find((item) => item.id === 'transform-stability');
  assert.equal(unstable.pass, false);
  assert.equal(unstable.value, 3);
  assert.deepEqual(unstable.affectedPaths, ['/World/Mesh']);
  const staleTotals = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, nonFiniteTransforms: 0, extremeTransforms: 0, meshes: 1, uvMeshes: 1 }, meshes: [{ path: '/World/Mesh', nonFiniteTransform: 1 }], materials: [], textures: [] }, 'web-viewer').checks.find((item) => item.id === 'transform-stability');
  assert.equal(staleTotals.pass, false);
  assert.equal(staleTotals.value, 1);
  const failedMaterials = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, materials: Array.from({ length: 9 }, () => ({})), textures: [] }, 'mobile-ar').checks.find((item) => item.id === 'materials');
  assert.equal(failedMaterials.pass, false);
  assert.deepEqual(failedMaterials.suggestedFixes, []);
  const malformedMaterialEntries = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, materials: [null], textures: [] }, 'mobile-ar').checks.find((item) => item.id === 'materials');
  assert.equal(malformedMaterialEntries.pass, false);
  assert.deepEqual(malformedMaterialEntries.affectedPaths, ['/']);
  const failedUV = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 0 }, meshes: [{ path: '/World/Mesh', hasUVs: false }], materials: [], textures: [] }, 'mobile-ar').checks.find((item) => item.id === 'uvs');
  assert.deepEqual(failedUV.affectedPaths, ['/World/Mesh']);
  assert.deepEqual(failedUV.suggestedFixes, ['mesh.unwrapUV']);
  const failedTexture = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [{ id: 'Textures/albedo.png', width: 8192, height: 4096 }] }, 'apple-ar').checks.find((item) => item.id === 'texture-dimension');
  assert.deepEqual(failedTexture.affectedPaths, ['Textures/albedo.png']);
  assert.deepEqual(failedTexture.suggestedFixes, ['texture.resize']);
  const manyTextures = Array.from({ length: 10000 }, (_, index) => ({ id: `texture-${index}`, width: index === 9999 ? 8192 : 1, height: 1 }));
  assert.equal(evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: manyTextures }, 'web-viewer').checks.find((item) => item.id === 'texture-dimension').value, 8192);
  const malformedTextures = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [null, { id: 'bad', width: 1.5, height: 1 }] }, 'web-viewer');
  assert.equal(malformedTextures.checks.find((item) => item.id === 'texture-validity').pass, false);
  const onlyMalformedTexture = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [null] }, 'web-viewer');
  assert.equal(onlyMalformedTexture.checks.find((item) => item.id === 'texture-dimension').pass, false);
  assert.deepEqual(onlyMalformedTexture.checks.find((item) => item.id === 'texture-dimension').affectedPaths, ['/']);
  assert.equal(onlyMalformedTexture.checks.find((item) => item.id === 'texture-validity').pass, false);
  const malformedMaterials = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, materials: { invalid: true }, textures: [] }, 'web-viewer');
  assert.equal(malformedMaterials.checks.find((item) => item.id === 'materials').pass, false);
  assert.deepEqual(malformedMaterials.checks.find((item) => item.id === 'materials').affectedPaths, ['/']);
  const malformedMeshes = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, meshes: { invalid: true }, materials: [], textures: [] }, 'web-viewer');
  assert.equal(malformedMeshes.checks.find((item) => item.id === 'transform-stability').pass, false);
  assert.deepEqual(malformedMeshes.checks.find((item) => item.id === 'transform-stability').affectedPaths, ['/']);
  const malformedMeshEntry = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, meshes: [null], materials: [], textures: [] }, 'mobile-ar');
  assert.equal(malformedMeshEntry.checks.find((item) => item.id === 'transform-stability').pass, false);
  assert.equal(malformedMeshEntry.checks.find((item) => item.id === 'uvs').pass, false);
  assert.deepEqual(malformedMeshEntry.checks.find((item) => item.id === 'uvs').affectedPaths, ['/']);
  const unnamedMesh = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 0 }, meshes: [{}], materials: [], textures: [] }, 'web-viewer');
  assert.deepEqual(unnamedMesh.checks.find((item) => item.id === 'uvs').affectedPaths, []);
  const malformedIssues = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, issues: { invalid: true }, textures: [] }, 'web-viewer');
  assert.equal(malformedIssues.checks.find((item) => item.id === 'texture-validity').pass, false);
  assert.equal(evaluateTargetProfile(report, 'unknown').profile, 'web-viewer');
  assert.equal(evaluateTargetProfile({ stage: { triangles: 120000, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, textures: [{ width: 4096, height: 4096 }] }, 'apple-ar').pass, false);
  const validTextures = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [] }, 'web-viewer');
  const invalidTextures = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [], issues: [{ ruleId: 'texture.invalidPixels', path: '/World/Mesh' }] }, 'web-viewer');
  assert.equal(validTextures.checks.find((item) => item.id === 'texture-validity').pass, true);
  assert.equal(invalidTextures.checks.find((item) => item.id === 'texture-validity').pass, false);
  const repeatedInvalidTexture = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [], issues: [{ ruleId: 'texture.invalidPixels', path: '/World/Mesh', message: 'same' }, { ruleId: 'texture.invalidPixels', path: '/World/Mesh', message: 'same' }] }, 'web-viewer');
  assert.equal(repeatedInvalidTexture.checks.find((item) => item.id === 'texture-validity').value, 1);
  const missingDoctor = evaluateTargetProfile({ stage: { triangles: 120000, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, textures: [{ width: 4096, height: 4096 }] }, 'portable-usdz');
  assert.equal(missingDoctor.pass, false);
  assert.equal(missingDoctor.checks.find((item) => item.id === 'usd-portability').message, 'Portable USDZ requires a USD Doctor report before export.');
  const portable = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [] }, 'portable-usdz', { issues: [{ severity: 'error', path: '/' }], dependencies: [] });
  assert.equal(portable.checks.find((item) => item.id === 'usd-portability').pass, false);
  const anonymous = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [] }, 'portable-usdz', { issues: [], dependencies: [{ path: 'anon:layer', present: true, status: 'anonymous' }] });
  assert.equal(anonymous.checks.find((item) => item.id === 'usd-portability').pass, false);
  const malformedDoctor = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [] }, 'portable-usdz', { issues: { invalid: true }, dependencies: { invalid: true } });
  assert.equal(malformedDoctor.checks.find((item) => item.id === 'usd-portability').pass, false);
  assert.deepEqual(malformedDoctor.checks.find((item) => item.id === 'usd-portability').affectedPaths, ['/']);
  const noUnits = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [] }, 'portable-usdz', { metersPerUnit: null, issues: [], dependencies: [] });
  assert.equal(noUnits.checks.find((item) => item.id === 'physical-units').pass, false);
  assert.equal(noUnits.checks.find((item) => item.id === 'usd-metadata').pass, false);
  const units = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [] }, 'portable-usdz', { metersPerUnit: 1, issues: [], dependencies: [] });
  assert.equal(units.checks.find((item) => item.id === 'physical-units').pass, true);
  assert.equal(evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [] }, 'portable-usdz', { defaultPrim: 'World', rootPrims: ['World'], upAxis: 'Y', metersPerUnit: 1, issues: [], dependencies: [] }).checks.find((item) => item.id === 'usd-metadata').pass, true);
  const external = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, textures: [] }, 'external-reference', { issues: [{ severity: 'error', path: '/' }], dependencies: [{ path: '../shared.usda', present: false }] });
  assert.equal(external.checks.some((item) => item.id === 'usd-portability'), false);
  assert.equal(external.pass, true);
  const print = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 0, boundaryEdges: 3, nonManifoldEdges: 0 }, meshes: [{ path: '/World/Mesh', boundaryEdges: 3, nonManifoldEdges: 0 }], materials: [], textures: [] }, '3d-printing', { metersPerUnit: 1 });
  assert.equal(print.profileVersion, 1);
  assert.equal(print.pass, false);
  assert.deepEqual(print.checks.find((item) => item.id === 'watertight').affectedPaths, ['/World/Mesh']);
  const printable = { stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0, boundaryEdges: 0, nonManifoldEdges: 0 }, materials: [], textures: [] };
  assert.equal(evaluateTargetProfile(printable, '3d-printing', { metersPerUnit: 1 }).pass, true);
  assert.equal(evaluateTargetProfile({ ...printable, textures: [{ width: 16384, height: 16384 }] }, '3d-printing', { metersPerUnit: 1 }).checks.some((item) => item.id === 'texture-dimension'), false);
  assert.equal(evaluateTargetProfile(printable, '3d-printing').checks.find((item) => item.id === 'physical-units').message, 'Physical-scale output requires a USD Doctor report before export.');
  assert.equal(evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, materials: [], textures: [] }, 'archival').pass, true);
});

test('target profiles fail closed when the stage report is omitted', () => {
  const report = evaluateTargetProfile({ materials: [], textures: [] }, 'web-viewer');
  assert.equal(report.pass, false);
  assert.equal(report.checks.find((item) => item.id === 'triangles').pass, false);
  assert.equal(report.checks.find((item) => item.id === 'uvs').pass, false);
});

test('target profiles fail closed when mesh aggregates lack inventory entries', () => {
  const report = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, materials: [], textures: [] }, 'web-viewer');
  assert.equal(report.pass, false);
  assert.equal(report.checks.find((item) => item.id === 'transform-stability').pass, false);
  assert.equal(report.checks.find((item) => item.id === 'uvs').pass, false);
  assert.deepEqual(report.checks.find((item) => item.id === 'uvs').affectedPaths, ['/']);
});

test('target profiles fail closed when mesh inventory cardinality disagrees', () => {
  const report = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, meshes: [], materials: [], textures: [] }, 'web-viewer');
  assert.equal(report.checks.find((item) => item.id === 'uvs').pass, false);
  assert.deepEqual(report.checks.find((item) => item.id === 'uvs').affectedPaths, ['/']);
});

test('target profiles reject fractional count metrics', () => {
  const report = evaluateTargetProfile({ stage: { triangles: 1.5, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, materials: [], textures: [] }, 'web-viewer');
  assert.equal(report.checks.find((item) => item.id === 'triangles').pass, false);
});

test('target profiles reject inconsistent material aggregate counts', () => {
  const omitted = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0, materialCount: 9 }, textures: [] }, 'mobile-ar');
  assert.equal(omitted.checks.find((item) => item.id === 'materials').pass, false);
  const mismatch = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0, materialCount: 2 }, materials: [{}], textures: [] }, 'web-viewer');
  assert.equal(mismatch.checks.find((item) => item.id === 'materials').pass, false);
  assert.deepEqual(mismatch.checks.find((item) => item.id === 'materials').affectedPaths, ['/']);
});

test('target profiles reject UV aggregate claims contradicted by mesh entries', () => {
  const report = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, meshes: [{ path: '/World/Mesh', hasUVs: false }], materials: [], textures: [] }, 'web-viewer');
  const check = report.checks.find((item) => item.id === 'uvs');
  assert.equal(check.pass, false);
  assert.deepEqual(check.affectedPaths, ['/World/Mesh']);
});

test('quality gates preserve legitimate detailed UV failures', () => {
  const gate = createQualityGate({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 1 }, meshes: [{ path: '/World/Mesh', hasUVs: false }], materials: [], textures: [] }, { profile: 'web-viewer' });
  assert.equal(gate.checks.find((item) => item.id === 'uvs').pass, false);
  assert.equal(validateQualityGate(gate), true);
});

test('physics target profiles expose deterministic engine limits', () => {
  for (const profile of ['physx', 'bullet', 'jolt', 'generic-usd']) {
    const gate = evaluateTargetProfile({ stage: { triangles: 2, estimatedGpuBytes: 2, meshes: 1, uvMeshes: 0, boundaryEdges: 0, nonManifoldEdges: 0 }, meshes: [{ path: '/World/Mesh' }], materials: [], textures: [] }, profile);
    assert.equal(gate.profile, profile);
    assert.equal(gate.pass, true);
    assert.equal(gate.profileVersion, 1);
  }
  const overBudget = evaluateTargetProfile({ stage: { triangles: 2, estimatedGpuBytes: 2, meshes: 1, uvMeshes: 0, boundaryEdges: 0, nonManifoldEdges: 0 }, meshes: [{ path: '/World/Mesh', physics: { convexHull: { vertexCount: 65 } } }], materials: [], textures: [] }, 'physx');
  assert.equal(overBudget.checks.find((item) => item.id === 'hull-vertices').pass, false);
  assert.deepEqual(overBudget.checks.find((item) => item.id === 'hull-vertices').affectedPaths, ['/World/Mesh']);
  const malformedHull = evaluateTargetProfile({ stage: { triangles: 2, estimatedGpuBytes: 2, meshes: 1, uvMeshes: 0, boundaryEdges: 0, nonManifoldEdges: 0 }, meshes: [{ path: '/World/BadHull', physics: { convexHull: { vertexCount: -1 } } }], materials: [], textures: [] }, 'physx');
  assert.equal(malformedHull.checks.find((item) => item.id === 'hull-vertices').pass, false);
  assert.deepEqual(malformedHull.checks.find((item) => item.id === 'hull-vertices').affectedPaths, ['/World/BadHull']);
  const dynamicTriangle = evaluateTargetProfile({ stage: { triangles: 2, estimatedGpuBytes: 2, meshes: 1, uvMeshes: 0, boundaryEdges: 0, nonManifoldEdges: 0 }, meshes: [{ path: '/World/Mesh_TriangleCollider' }], materials: [], textures: [] }, 'physx');
  assert.equal(dynamicTriangle.checks.find((item) => item.id === 'dynamic-concave-collider').pass, false);
  assert.deepEqual(dynamicTriangle.checks.find((item) => item.id === 'dynamic-concave-collider').suggestedFixes, ['scene.generate_convex_hull']);
  const approximate = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 0, boundaryEdges: 0, nonManifoldEdges: 0 }, meshes: [{ path: '/World/Large', physics: { convexHull: { vertexCount: 32, approximate: true } } }], materials: [], textures: [] }, 'physx');
  assert.equal(approximate.checks.find((item) => item.id === 'hull-cooking-readiness').pass, false);
  const genericApproximate = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 1, uvMeshes: 0, boundaryEdges: 0, nonManifoldEdges: 0 }, meshes: [{ path: '/World/Large', physics: { convexHull: { vertexCount: 32, approximate: true } } }], materials: [], textures: [] }, 'generic-usd');
  assert.equal(genericApproximate.checks.some((item) => item.id === 'hull-cooking-readiness'), false);
  const genericTriangle = evaluateTargetProfile({ stage: { triangles: 2, estimatedGpuBytes: 2, meshes: 1, uvMeshes: 0, boundaryEdges: 0, nonManifoldEdges: 0 }, meshes: [{ path: '/World/Mesh_TriangleCollider' }], materials: [], textures: [] }, 'generic-usd');
  assert.equal(genericTriangle.checks.some((item) => item.id === 'dynamic-concave-collider'), false);
});

test('physics mesh analysis reports closed volume and suitability', () => {
  const result = analyzePhysicsMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]),
    indices: new Uint32Array([0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]),
  });
  assert.equal(result.watertight, true);
  assert.equal(result.boundaryEdges, 0);
  assert.equal(result.components, 1);
  assert.ok(Math.abs(result.volume - 1 / 6) < 1e-6);
  assert.equal(result.convexHull.vertexCount, 4);
  assert.equal(result.convexHull.triangleCount, 4);
  assert.ok(Math.abs(result.convexHull.volume - result.volume) < 1e-6);
  assert.ok(Math.abs(result.convexHull.volumeError) < 1e-6);
  assert.equal(result.suitability.dynamic, true);
  assert.equal(result.suitability.staticTriangleMesh, true);
});

test('physics mesh analysis bounds implicit topology before allocation', () => {
  const result = analyzePhysicsMesh({ positions: { length: INDEXED_MESH_MAX_VERTICES * 3 + 3 } });
  assert.equal(result.valid, false);
  assert.equal(result.vertexCount, INDEXED_MESH_MAX_VERTICES + 1);
  assert.equal(result.fits, null);
});

test('physics primitive fits are deterministic and expose normalized errors', () => {
  const positions = new Float32Array([-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1]);
  const fits = fitPhysicsPrimitives(positions);
  assert.deepEqual(fits.box.center, [0, 0, 0]);
  assert.deepEqual(fits.box.size, [2, 2, 2]);
  assert.equal(fits.box.error.normalizedMax, 0);
  assert.equal(fits.sphere.error.normalizedMean, 0);
  assert.equal(fits.capsule.axis, 0);
  assert.equal(fits.cylinder.axis, 0);
  assert.ok(fits.capsule.error.normalizedMax >= 0);
  assert.ok(fits.cylinder.error.normalizedMax >= 0);
});

test('rig audit detects unsafe skin weights and joint matrices', () => {
  const result = analyzeSkinning({ skinIndices: new Uint16Array([0, 1, 2, 9, 0, 0, 0, 0]), skinWeights: new Float32Array([.5, .25, 0, 0, 0, 0, 0, 0]), boneCount: 4, boneMatrices: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]) });
  assert.equal(result.unweightedVertices, 1);
  assert.equal(result.weightSumMismatches, 1);
  assert.equal(result.invalidJointIndices, 1);
  assert.equal(result.nonInvertibleBones, 1);
  assert.deepEqual(result.influenceSamples, [{ index: 0, issues: ['invalid joint', 'weight sum'], weightSum: .75, positiveInfluences: 2 }, { index: 1, issues: ['unweighted'], weightSum: 0, positiveInfluences: 0 }]);
  assert.equal(result.valid, false);
  assert.equal(analyzeSkinning().present, false);
  assert.equal(analyzeSkinning({ skinIndices: new Uint16Array([0, 0, 0, 0]), skinWeights: new Float32Array([1, -0.1, 0, 0]) }).negativeWeights, 1);
  assert.throws(() => transferNearestSkinWeights({ sourcePositions: new Float32Array([0, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceSkinIndices: new Uint16Array([0, 0, 0, 0]), sourceSkinWeights: new Float32Array([1, 0, 0, 0]), targetPositions: new Float32Array([0, 0, 0]) }), /invalid triangle indices/);
});

test('skeleton audit detects duplicate names, foreign parents, and cycles', () => {
  const first = { name: 'Root' }, second = { name: 'Root', parent: first }, foreign = { name: 'Foreign' };
  first.parent = second;
  const result = analyzeSkeleton({ bones: [first, second, { name: 'Detached', parent: foreign }] });
  assert.equal(result.duplicateNames, 1);
  assert.equal(result.missingParents, 1);
  assert.equal(result.cycles, 1);
  assert.equal(result.disconnectedRoots, 1);
  assert.equal(result.valid, false);
});

test('skeleton audit rejects singular bone transforms', () => {
  const singular = [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const result = analyzeSkeleton({ bones: [{ name: 'root', matrixWorld: { elements: singular } }] });
  assert.equal(result.nonInvertibleTransforms, 1);
  assert.equal(result.valid, false);
});

test('skeleton audit rejects malformed bone matrix cardinality', () => {
  const result = analyzeSkeleton({ bones: [{ name: 'root', matrixWorld: { elements: [1, 0, 0] } }] });
  assert.equal(result.malformedTransforms, 1);
  assert.equal(result.valid, false);
});

test('skeleton audit handles deep hierarchies without recursive overflow', () => {
  const bones = Array.from({ length: 20000 }, (_, index) => ({ name: `bone-${index}` }));
  for (let index = 1; index < bones.length; index++) bones[index].parent = bones[index - 1];
  const result = analyzeSkeleton({ bones });
  assert.equal(result.cycles, 0);
  assert.equal(result.disconnectedRoots, 1);
  assert.equal(result.valid, true);
});

test('bind transform audit detects count and invertibility errors', () => {
  const identity = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  const singular = new Float32Array([1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  const result = analyzeBindTransforms([identity, singular], 3);
  assert.equal(result.countMismatch, true);
  assert.equal(result.nonInvertible, 1);
  assert.equal(result.valid, false);
  assert.equal(analyzeSkinning({ skinIndices: new Uint16Array([0, 0, 0, 0]), skinWeights: new Float32Array([1, 0, 0, 0]), boneCount: 1, bindMatrices: [identity] }).bindTransforms.valid, true);
  const packed = new Float32Array([...identity, ...identity]);
  assert.deepEqual(analyzeBindTransforms(packed, 2), { present: true, count: 2, countMismatch: false, nonFinite: 0, nonInvertible: 0, valid: true });
});

test('bind-pose audit reports identity residuals deterministically', () => {
  const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  assert.deepEqual(analyzeBindPose({ bones: [{ name: 'root', matrixWorld: { elements: identity } }], bindMatrices: [identity] }), { present: true, compared: 1, maxResidual: 0, meanResidual: 0, residuals: [{ index: 0, name: 'root', residual: 0 }], valid: true });
  assert.equal(analyzeBindPose({ bones: [{ name: 'root', matrixWorld: { elements: identity } }], bindMatrices: new Float32Array(identity) }).valid, true);
  const translated = [...identity]; translated[12] = 2;
  const moved = analyzeBindPose({ bones: [{ matrixWorld: { elements: translated } }], bindMatrices: [identity] });
  assert.equal(moved.valid, false);
  assert.equal(moved.maxResidual, 2);
});

test('bind-pose residual samples prioritize the largest mismatch deterministically', () => {
  const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const mild = [...identity]; mild[12] = .25;
  const severe = [...identity]; severe[12] = 3;
  const result = analyzeBindPose({ bones: [{ name: 'mild', matrixWorld: { elements: mild } }, { name: 'severe', matrixWorld: { elements: severe } }, { name: 'tie', matrixWorld: { elements: severe } }], bindMatrices: [identity, identity, identity] });
  assert.deepEqual(result.residuals.map((sample) => sample.name), ['severe', 'tie', 'mild']);
  assert.deepEqual(result.residuals.map((sample) => sample.index), [1, 2, 0]);
});

test('skinned deformation comparison reports bounded vertex displacement', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0]), indices = new Uint16Array([0, 0, 0, 0, 0, 0, 0, 0]), weights = new Float32Array([1, 0, 0, 0, 1, 0, 0, 0]), translated = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1]);
  const result = compareSkinnedDeformation({ positions, skinIndices: indices, skinWeights: weights, boneMatrices: translated, maxSamples: 1 });
  assert.equal(result.compared, 2);
  assert.equal(result.meanDistance, 1);
  assert.equal(result.maxDistance, 1);
  assert.deepEqual(result.samples, [{ vertex: 0, distance: 1 }]);
  assert.throws(() => compareSkinnedDeformation({ positions, skinIndices: indices, skinWeights: weights, boneMatrices: new Float32Array(15) }), /packed bone matrices/);
  assert.throws(() => compareSkinnedDeformation({ positions, skinIndices: new Float32Array([1, 0, 0, 0, 0, 0, 0, 0]), skinWeights: weights, boneMatrices: translated }), /in-range joint indices/);
  assert.throws(() => compareSkinnedDeformation({ positions, skinIndices: indices, skinWeights: new Float32Array([-1, 0, 0, 0, 1, 0, 0, 0]), boneMatrices: translated }), /non-negative in-range/);
});

test('skin transfer is deterministic and normalizes copied influences', () => {
  const result = transferNearestSkinWeights({ sourcePositions: new Float32Array([0, 0, 0, 10, 0, 0, 0, 10, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceSkinIndices: new Uint16Array([0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2]), sourceSkinWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]), targetPositions: new Float32Array([5, 0, 0, 1, 1, 0]), maxInfluences: 4 });
  assert.deepEqual([...result.jointIndices], [0, 1, 0, 0, 0, 1, 2, 0]);
  assert.ok(Math.abs(result.jointWeights[0] - .5) < 1e-6 && Math.abs(result.jointWeights[1] - .5) < 1e-6);
  assert.ok(Math.abs(result.jointWeights[4] - .8) < 1e-6 && Math.abs(result.jointWeights[5] - .1) < 1e-6 && Math.abs(result.jointWeights[6] - .1) < 1e-6);
  assert.equal(result.maxDistance, 0);
  assert.deepEqual([...result.distances], [0, 0]);
  assert.throws(() => transferNearestSkinWeights({ sourcePositions: new Float32Array([0, 0, 0]), sourceSkinIndices: new Uint16Array([3, 0, 0, 0]), sourceSkinWeights: new Float32Array([1, 0, 0, 0]), boneCount: 2, targetPositions: new Float32Array([0, 0, 0]) }), /valid joint indices/);
});

test('skin transfer rejects malformed source topology', () => {
  assert.throws(() => transferNearestSkinWeights({ sourcePositions: new Float32Array([0, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceSkinIndices: new Uint16Array([0, 0, 0, 0]), sourceSkinWeights: new Float32Array([1, 0, 0, 0]), targetPositions: new Float32Array([0, 0, 0]) }), /invalid triangle indices/);
});

test('skin transfer result validation enforces normalized bounded influences', () => {
  const valid = { jointIndices: new Uint16Array([0, 1, 0, 0]), jointWeights: new Float32Array([.5, .5, 0, 0]), targetVertexCount: 1, meanDistance: 0, maxDistance: 0 };
  assert.equal(validateSkinTransferResult(valid, 1, 2), valid);
  assert.throws(() => validateSkinTransferResult({ ...valid, distances: new Float32Array([0, 1]) }, 1, 2), /malformed/);
  assert.throws(() => validateSkinTransferResult({ ...valid, distances: new Float32Array([-1]) }, 1, 2), /malformed/);
  assert.throws(() => validateSkinTransferResult({ ...valid, jointWeights: new Float32Array([.25, .25, 0, 0]) }, 1, 2), /non-normalized/);
  assert.throws(() => validateSkinTransferResult({ ...valid, jointIndices: new Uint16Array([2, 0, 0, 0]) }, 1, 2), /joint index/);
  assert.throws(() => validateSkinTransferResult({ ...valid, targetVertexCount: 2 }, 1, 2), /malformed/);
});

test('influence heatmap preview is bounded and chooses a stable projection', () => {
  const result = buildInfluenceHeatmap({ positions: new Float32Array([0, 0, 0, 2, 0, 0, 0, 1, 0]), skinIndices: new Uint16Array([2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]), skinWeights: new Float32Array([.25, .75, 0, 0, 1, 0, 0, 0, .5, .5, 0, 0]), boneCount: 3, maxSamples: 2 });
  assert.equal(result.samples.length, 2);
  assert.deepEqual(result.axes, [1, 2]);
  assert.equal(result.samples[0].joint, 0);
  assert.equal(result.samples[0].weight, .75);
  assert.throws(() => buildInfluenceHeatmap({ positions: new Float32Array([0, 0, 0, 1, 0, 0]), skinIndices: new Uint16Array([0, 0, 0, 0]), skinWeights: new Float32Array([1, 0, 0, 0]), boneCount: 1, maxSamples: 1 }), /matching position/);
});

test('asset report exposes skin audit findings on skinned meshes', () => {
  const mesh = { isMesh: true, name: 'Skinned', geometry: { attributes: { position: { count: 1, array: new Float32Array([0, 0, 0]) }, skinIndex: { count: 1, array: new Uint16Array([4, 0, 0, 0]) }, skinWeight: { count: 1, array: new Float32Array([1, 0, 0, 0]) } } }, skeleton: { bones: [{}] } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.meshes[0].skin.invalidJointIndices, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'rig.invalidJoint'));
});

test('asset report exposes singular skeleton transforms', () => {
  const singular = [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const mesh = { isMesh: true, name: 'SingularRig', geometry: { attributes: { position: { count: 1, array: new Float32Array([0, 0, 0]) }, skinIndex: { count: 1, array: new Uint16Array([0, 0, 0, 0]) }, skinWeight: { count: 1, array: new Float32Array([1, 0, 0, 0]) } } }, skeleton: { bones: [{ matrixWorld: { elements: singular } }] } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.meshes[0].skin.skeleton.nonInvertibleTransforms, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'rig.nonInvertibleBoneTransform'));
});

test('asset report exposes bind-pose mismatch as informational', () => {
  const identity = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]), moved = new Float32Array(identity); moved[12] = 2;
  const mesh = { isMesh: true, name: 'Animated', geometry: { attributes: { position: { count: 1, array: new Float32Array([0, 0, 0]) }, skinIndex: { count: 1, array: new Uint16Array([0, 0, 0, 0]) }, skinWeight: { count: 1, array: new Float32Array([1, 0, 0, 0]) } } }, skeleton: { bones: [{ matrixWorld: { elements: moved } }], boneInverses: [{ elements: identity }] } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.meshes[0].skin.bindPose.valid, false);
  assert.ok(report.issues.some((item) => item.ruleId === 'rig.bindPoseMismatch' && item.severity === 'info'));
});

test('asset report detects skin weights detached from a skeleton', () => {
  const mesh = { isMesh: true, name: 'Detached', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, skinIndex: { count: 3, array: new Uint16Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) }, skinWeight: { count: 3, array: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) } } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.meshes[0].skin.detachedGeometry, true);
  assert.ok(report.issues.some((item) => item.ruleId === 'rig.detachedGeometry'));
});

test('normal recompute respects explicit sharp edges', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), indices: new Uint32Array([0, 1, 2, 0, 2, 3]), smoothingAngle: 180 };
  const smooth = recomputeVertexNormals(mesh), sharp = recomputeVertexNormals({ ...mesh, sharpEdges: [[0, 2]] });
  assert.ok(smooth[0] > 0 && smooth[2] > 0);
  assert.ok(Math.abs(sharp[0]) < 1e-6 && Math.abs(sharp[0 * 3 + 1]) < 1e-6 && Math.abs(sharp[2] - 1) < 1e-6);
  assert.deepEqual(validateSharpEdges([[2, 0], [0, 2]], 4), [[0, 2]]);
  assert.throws(() => validateSharpEdges([[0, 4]], 4), /in-range/);
  assert.throws(() => validateSharpEdges([[1, 1]], 4), /distinct/);
});

test('physics convex hull reduces a cube to its outer shell', () => {
  const hull = generateConvexHull(new Float32Array([-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1]));
  assert.equal(hull.valid, true);
  assert.equal(hull.hullVertexCount, 8);
  assert.equal(hull.triangleCount, 12);
  assert.ok([...hull.indices].every((index) => index < hull.hullVertexCount));
});

test('physics analysis keeps a bounded approximate hull for large meshes', () => {
  const cube = [-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1];
  const positions = new Float32Array(10001 * 3); positions.set(cube);
  for (let vertex = 8; vertex < 10001; vertex++) positions.set([0, 0, 0], vertex * 3);
  const result = analyzePhysicsMesh({ positions, indices: new Uint32Array([0, 1, 2]) });
  assert.equal(result.convexHull?.approximate, true);
  assert.ok(result.convexHull.sampledVertexCount <= 4096);
  assert.equal(result.convexHull.sourceVertexCount, 10001);
});

test('convex hull operation rejects non-finite source vertices', async () => {
  const mesh = { isMesh: true, geometry: { attributes: { position: { array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, NaN, 0, 0]), count: 5 } } } };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  await assert.rejects(() => operations.generateConvexHull('/World/Mesh'), /at least four finite vertices/);
});

test('retopology boundary locks include group borders and omit interior edges', () => {
  const indices = new Uint32Array([0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4]);
  const locks = deriveBoundaryLocks(indices, [{ start: 0, count: 12 }], 5);
  assert.deepEqual([...locks], [1, 1, 1, 1, 0]);
  const grouped = deriveBoundaryLocks(indices, [{ start: 0, count: 6 }, { start: 6, count: 6 }], 5);
  assert.deepEqual([...grouped], [1, 1, 1, 1, 1]);
});

test('retopology UV seam locks preserve duplicated positions with different UVs', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0]);
  const uvs = new Float32Array([0, 0, 1, 0, .5, .5, 0, 1]);
  assert.deepEqual([...deriveUVSeamLocks(positions, uvs, 4)], [1, 0, 1, 0]);
  assert.deepEqual([...deriveUVSeamLocks(positions, new Float32Array([0, 0, 1, 0, 0, 0, 0, 1]), 4)], [0, 0, 0, 0]);
});

test('retopology sharp-edge locks produce a bounded vertex mask', () => {
  assert.deepEqual([...deriveEdgeLocks([[3, 1], [1, 8], [-1, 2], [2, 2]], 4)], [0, 1, 0, 1]);
});

test('retopology user lock selection parses indices and inclusive ranges safely', () => {
  assert.deepEqual([...parseVertexLockSelection('0, 4, 10-12, 4', 14)], [1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0]);
  assert.deepEqual([...parseVertexLockSelection('', 3)], [0, 0, 0]);
  assert.throws(() => parseVertexLockSelection('2-1', 3), /outside the valid range/);
  assert.throws(() => parseVertexLockSelection('3', 3), /outside the valid range/);
  assert.throws(() => parseVertexLockSelection('one', 3), /Invalid vertex lock/);
});

test('retopology lock mask validator checks before copying', () => {
  const source = new Uint8Array([1, 0, 1]);
  const copy = validateVertexLockMask(source, 3);
  assert.deepEqual([...copy], [1, 0, 1]);
  assert.notEqual(copy, source);
  assert.throws(() => validateVertexLockMask(new Float32Array([0, .5, 1]), 3), /only zero or one/);
  assert.throws(() => validateVertexLockMask(new Uint8Array([1, 0]), 3), /one value/);
});

test('assistant retopology calls reject malformed lock masks', () => {
  const assistant = new LuciaAssistant({ executeTool: () => {}, getSelection: () => '/World/Mesh', getSceneSummary: () => ({}), confirm: () => true });
  assert.throws(() => assistant.validate({ name: 'scene.retopo', arguments: { path: '/World/Mesh', lockedVertices: [0, 1, 2] } }), /zero\/one vertex lock mask/);
  assert.throws(() => assistant.validate({ name: 'scene.retopo', arguments: { path: '/World/Mesh', lockBorder: 'yes' } }), /lockBorder must be boolean/);
  assert.throws(() => assistant.validate({ name: 'scene.generate_lods', arguments: { path: '/World/Mesh', lockBorder: 'yes' } }), /lockBorder must be boolean/);
  assert.throws(() => assistant.validate({ name: 'scene.retopo', arguments: { path: '/World/Mesh', lockUVSeams: 'yes' } }), /lockUVSeams must be boolean/);
  assert.equal(assistant.validate({ name: 'scene.retopo', arguments: { path: '/World/Mesh', lockedVertices: [0, 1, 0] } }).name, 'scene.retopo');
});

test('retopology worker rejects malformed optional attributes before WASM', async () => {
  const previousSelf = globalThis.self;
  let message = null;
  globalThis.self = { postMessage(value) { message = value; } };
  try {
    await import(`../src/retopo-worker.js?invalid-attribute=${Date.now()}`);
    await globalThis.self.onmessage({ data: {
      type: 'retopo',
      positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
      indices: new Uint32Array([0, 1, 2]),
      normals: new Float32Array([0, 0, 1, 0, 0, 1, NaN, 0, 1]),
    } });
    assert.equal(message?.type, 'error');
    assert.match(message.message, /invalid normal buffer/);
  } finally {
    if (previousSelf === undefined) delete globalThis.self;
    else globalThis.self = previousSelf;
  }
});

test('retopology operation rejects malformed seam policies', async () => {
  const operations = new LuciaOperations({}, {}, {});
  assert.equal(operations.setUVSeamLockPolicy(false), false);
  assert.equal(operations.lockUVSeamsPolicy, false);
  await assert.rejects(() => operations.retopo('/World/Mesh', { lockBorder: 'yes' }), /lockBorder must be boolean/);
  assert.throws(() => operations.setUVSeamLockPolicy('no'), /lockUVSeams must be boolean/);
  await assert.rejects(() => operations.retopo('/World/Mesh', { lockUVSeams: 'no' }), /lockUVSeams must be boolean/);
  await assert.rejects(() => operations.generateLODChain('/World/Mesh', { lockUVSeams: 'no' }), /lockUVSeams must be boolean/);
});

test('retopology remaps lossless face-varying primvars and expands seams', () => {
  const attributes = [{ name: 'corner', itemSize: 2, array: new Float32Array([0, 0, 1, 0, 0, 1]), indices: new Uint32Array([0, 1, 2]) }];
  const result = { sourceVertexCount: 3, xref: new Uint32Array([2, 0, 1]), indices: new Uint32Array([0, 1, 2]) };
  const remapped = remapRetopoFaceVaryingAttributes(attributes, new Uint32Array([0, 1, 2]), result);
  assert.deepEqual([...remapped[0].array], [0, 1, 0, 0, 1, 0]);
  assert.deepEqual([...remapped[0].indices], [0, 1, 2]);
  assert.throws(() => remapRetopoFaceVaryingAttributes([{ ...attributes[0], indices: new Uint32Array([0, 1, 2]) }], new Uint32Array([0, 1, 0]), result), /changes across a source vertex/);
  const expanded = expandRetopoFaceVaryingMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0]), indices: new Uint32Array([0, 1, 0]), normals: new Float32Array([0, 0, 1, 0, 0, 1]), faceVaryingAttributes: [{ name: 'st', itemSize: 2, array: new Float32Array([0, 0, 1, 0, .5, .5]), indices: new Uint32Array([0, 1, 2]) }] });
  assert.equal(expanded.positions.length / 3, 3);
  assert.deepEqual([...expanded.indices], [0, 1, 2]);
  assert.deepEqual(expanded.sourceVertexCopies, [[0, 2], [1]]);
  assert.deepEqual([...expanded.faceVaryingAttributes[0].indices], [0, 1, 2]);
  const expandedResult = remapRetopoFaceVaryingAttributes(expanded.faceVaryingAttributes, expanded.indices, { sourceVertexCount: 3, xref: new Uint32Array([0, 1, 2]), indices: expanded.indices });
  assert.deepEqual([...expandedResult[0].array], [...expanded.faceVaryingAttributes[0].array]);
  assert.throws(() => expandRetopoFaceVaryingMesh({ positions: new Float32Array(9), indices: new Uint32Array([0, 1, 2]), faceVaryingAttributes: [{ name: 'st', itemSize: 2, array: [0, 0], indices: new Uint32Array([0, 0, 0]) }] }), /finite typed values/);
});

test('retopology crease remap keeps only surviving output-edge paths', () => {
  const sourceVertexCopies = [[0], [1], [2], [3]], xref = new Uint32Array([0, 1, 2, 3]);
  assert.deepEqual(remapRetopoCreaseChains({ chains: [[0, 1, 2]], sharpness: [.5], sourceVertexCopies, indices: new Uint32Array([0, 1, 2]), xref }), { chains: [[0, 1, 2]], sharpness: [.5] });
  assert.deepEqual(remapRetopoCreaseChains({ chains: [[0, 1, 3]], sharpness: [1], sourceVertexCopies, indices: new Uint32Array([0, 1, 2]), xref }), { chains: [], sharpness: [] });
});

test('LOD chains propagate the UV-seam policy to every level', async () => {
  const calls = [], operations = new LuciaOperations({ exportUSDA: () => 'before', setMeshGeometrySibling: async () => {} }, {}, {});
  operations.retopo = async (path, options) => { calls.push({ path, options }); return { positions: new Float32Array([0, 0, 0]), indices: new Uint32Array([0, 0, 0]) }; };
  await operations.generateLODChain('/World/Mesh', { ratios: [.5, .25], lockUVSeams: false });
  assert.deepEqual(calls.map(({ options }) => options.lockUVSeams), [false, false]);
});

test('mock assistant forwards sharp edges in retopology requests', () => {
  const assistant = new LuciaAssistant({ executeTool: () => {}, getSelection: () => '/World/Mesh', getSceneSummary: () => ({}), confirm: () => true });
  assert.deepEqual(assistant.mock('reduce 50% preserving sharp edge 0:2 and crease 4-5'), { name: 'scene.retopo', arguments: { path: '/World/Mesh', targetRatio: .5, tolerance: .0001, lockUVSeams: true, lockBorder: true, sharpEdges: [[0, 2], [4, 5]] } });
  assert.equal(assistant.mock('reduce 50% without border locks').arguments.lockBorder, false);
  assert.equal(assistant.mock('reduce 50% without UV seams').arguments.lockUVSeams, false);
});

test('assistant validates and mocks crease chains', () => {
  const assistant = new LuciaAssistant({ executeTool: () => {}, getSelection: () => '/World/Mesh', getSceneSummary: () => ({}), confirm: () => true });
  assert.deepEqual(assistant.mock('recompute normals preserving sharp chain 0>1>2@0.5'), { name: 'scene.recompute_normals', arguments: { path: '/World/Mesh', weighting: 'area', smoothingAngle: 180, sharpChains: [[0, 1, 2]], sharpChainSharpness: [.5] } });
  assert.equal(assistant.validate({ name: 'scene.recompute_normals', arguments: { sharpChains: [[0, 1, 2]], sharpChainSharpness: [.5] } }).confirm, true);
  assert.throws(() => assistant.validate({ name: 'scene.recompute_normals', arguments: { sharpChains: [[0, 1]], sharpEdges: [[0, 1]] } }), /sharpChains/);
  assert.throws(() => assistant.validate({ name: 'scene.recompute_normals', arguments: { sharpChains: [[0, 1]], sharpChainSharpness: [0] } }), /sharpChains/);
  assert.deepEqual(assistant.mock('retopo 50% with sharp chain 0>1>2@0.25'), { name: 'scene.retopo', arguments: { path: '/World/Mesh', targetRatio: .5, tolerance: .0001, lockUVSeams: true, lockBorder: true, sharpChains: [[0, 1, 2]], sharpChainSharpness: [.25] } });
  assert.equal(assistant.validate({ name: 'scene.retopo', arguments: { sharpChains: [[0, 1, 2]], sharpChainSharpness: [.25] } }).confirm, true);
});

test('sharp-chain selection parses and expands deterministic barriers', () => {
  assert.deepEqual(parseSharpChainSelection('2>1>0@0.5; 4>5', 6), { chains: [[2, 1, 0], [4, 5]], sharpness: [.5, 1] });
  assert.deepEqual(parseSharpChainSelection('0>1@.5; 2>3@1e-3', 4).sharpness, [.5, .001]);
  assert.deepEqual(expandSharpChains([[2, 1, 0], [0, 1]], [.5, 1]).edges, [[0, 1], [1, 2]]);
  assert.deepEqual(expandSharpChains([[2, 1, 0], [0, 1]], [.5, 1]).sharpness, [1, .5]);
  assert.throws(() => parseSharpChainSelection('0>1@0; 2>3', 4), /positive finite/);
  assert.throws(() => parseSharpChainSelection('0:1', 4), /Invalid sharp chain/);
  assert.throws(() => parseSharpChainSelection('0>0', 1), /distinct/);
  assert.throws(() => expandSharpChains(new Array(MAX_SHARP_CHAINS + 1).fill([0, 1])), /at most/);
  assert.throws(() => parseSharpChainSelection('x'.repeat(MAX_SHARP_CHAIN_TEXT_LENGTH + 1), 2), /at most/);
  assert.throws(() => validateSharpEdges(new Array(MAX_SHARP_EDGES + 1).fill([0, 1]), 2), /at most/);
});

test('sharp-edge selection parses canonical pairs and rejects invalid endpoints', () => {
  assert.deepEqual(parseSharpEdgeSelection('2:0; 4-5; 0:2', 6), [[0, 2], [4, 5]]);
  assert.deepEqual(parseSharpEdgeSelection('', 6), []);
  assert.throws(() => parseSharpEdgeSelection('2:2', 6), /identical endpoints/);
  assert.throws(() => parseSharpEdgeSelection('1:6', 6), /outside the valid vertex range/);
  assert.throws(() => parseSharpEdgeSelection('1,2', 6), /Invalid sharp edge/);
});

test('geometry comparison reports correspondence-independent displacement heatmap samples', () => {
  const before = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), current = new Float32Array([0, 0, 0, 1.1, 0, 0, 0, 1, 0]);
  const comparison = compareGeometrySnapshots(before, current, { maxSamples: 16 });
  assert.equal(comparison.compared, 3);
  assert.equal(comparison.samples[0].distance, 0);
  assert.ok(comparison.maxDistance > 0);
  assert.ok(comparison.normalizedMax > 0);
});

test('quality gates use a versioned validated envelope', () => {
  const gate = createQualityGate({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, materials: [], textures: [] }, { profile: 'mobile-ar', blocking: true });
  assert.equal(gate.schemaVersion, 1);
  assert.equal(gate.blocking, true);
  assert.equal(validateQualityGate(gate), true);
  const malformed = createQualityGate({ stage: { triangles: NaN, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, materials: [], textures: [] }, { profile: 'web-viewer' });
  assert.equal(malformed.checks.find((item) => item.id === 'triangles').pass, false);
  assert.equal(validateQualityGate(malformed), true);
  assert.equal(evaluateTargetProfile({ stage: 'malformed', materials: [], textures: [] }, 'web-viewer').pass, false);
  const malformedTexture = evaluateTargetProfile({ stage: { triangles: 1, estimatedGpuBytes: 1, meshes: 0, uvMeshes: 0 }, materials: [], textures: [{ id: 'albedo.png', hasImage: true, width: 1.5, height: 2, pixelCount: 8, expectedPixelCount: 16 }] }, 'web-viewer');
  assert.equal(malformedTexture.checks.find((item) => item.id === 'texture-validity').pass, false);
  assert.equal(validateQualityGate({ ...gate, checks: gate.checks.map((item, index) => index === 0 ? { ...item, value: -1 } : item) }), false);
  assert.equal(validateQualityGate({ ...gate, checks: gate.checks.map((item, index) => index === 0 ? { ...item, affectedPaths: ['/World/B', '/World/A'] } : item) }), false);
  assert.equal(validateQualityGate({ ...gate, checks: gate.checks.map((item, index) => index === 0 ? { ...item, suggestedFixes: ['mesh.cleanup', 'mesh.cleanup'] } : item) }), false);
  assert.equal(validateQualityGate({ ...gate, checks: gate.checks.slice(1) }), false);
  assert.equal(validateQualityGate({ ...gate, checks: [...gate.checks, { ...gate.checks[0], id: 'unexpected' }] }), false);
  assert.equal(validateQualityGate({ ...gate, checks: [...gate.checks, gate.checks[0]] }), false);
  assert.equal(validateQualityGate({ ...gate, checks: [{ ...gate.checks[0], affectedPaths: 'not-an-array' }] }), false);
  assert.equal(validateQualityGate({ ...gate, checks: [{ ...gate.checks[0], value: Infinity }] }), false);
  assert.equal(validateQualityGate({ ...gate, checks: [{ ...gate.checks[0], suggestedFixes: [42] }] }), false);
  assert.equal(validateQualityGate({ ...gate, profile: 'unknown' }), false);
  assert.equal(validateQualityGate({ ...gate, profileVersion: 2 }), false);
  assert.equal(validateQualityGate({ ...gate, label: 'Wrong profile label' }), false);
  assert.equal(validateQualityGate({ ...gate, schemaVersion: 2 }), false);
  assert.equal(validateQualityGate({ ...gate, checks: gate.checks.map((item, index) => index === 0 ? { ...item, value: item.limit + 1, pass: true } : item) }), false);
  assert.equal(validateQualityGate({ ...gate, pass: !gate.pass }), false);
});

test('preview budget degrades deterministically for large scenes', () => {
  const normal = choosePreviewBudget({ triangles: 1000, meshes: 1, textureBytes: 1024 });
  assert.equal(normal.geometrySampleRatio, 1);
  assert.equal(normal.maxTextureBytes, 512 * 1024 * 1024);
  assert.equal(normal.reason, null);
  const large = choosePreviewBudget({ triangles: 3000000, meshes: 20, textureBytes: 100 * 1024 * 1024 });
  assert.equal(large.pixelRatio, 1);
  assert.equal(large.maxTextureDimension, 4096);
  assert.equal(large.geometrySampleRatio, 0.75);
  assert.equal(large.maxTextureBytes, 128 * 1024 * 1024);
  assert.match(large.reason, /Large scene/);
  const huge = choosePreviewBudget({ triangles: 6000000, meshes: 100, textureBytes: 2 * 1024 * 1024 * 1024 });
  assert.equal(huge.pixelRatio, 0.75);
  assert.equal(huge.geometrySampleRatio, 0.5);
  assert.equal(choosePreviewBudget({ triangles: NaN, meshes: -1, textureBytes: Infinity }).estimatedBytes, 0);
});

test('preview texture scale combines dimension and aggregate byte limits', () => {
  assert.equal(previewTextureScale({ width: 8192, height: 4096, maxDimension: 4096, maxBytes: Infinity, totalBytes: 0 }), .5);
  assert.equal(previewTextureScale({ width: 1024, height: 1024, maxDimension: 4096, maxBytes: 1024 * 1024, totalBytes: 4 * 1024 * 1024 }), .5);
  assert.equal(previewTextureScale({ width: 0, height: 0, maxDimension: 1024, maxBytes: 1 }), 1);
});

test('preview geometry draw count stays triangle aligned and bounded', () => {
  assert.equal(previewGeometryDrawCount({ indexCount: 100, sampleRatio: .5 }), 48);
  assert.equal(previewGeometryDrawCount({ vertexCount: 10, sampleRatio: 1 }), 9);
  assert.equal(previewGeometryDrawCount({ indexCount: 2, sampleRatio: .5 }), 0);
  assert.equal(previewGeometryDrawCount({ indexCount: 6, sampleRatio: .1 }), 3);
});

test('preview triangle budget follows the scene geometry ratio', () => {
  assert.equal(previewTriangleBudget({ geometrySampleRatio: 1 }), 100000);
  assert.equal(previewTriangleBudget({ geometrySampleRatio: .5 }), 50000);
  assert.equal(previewTriangleBudget({ geometrySampleRatio: 0 }), 10000);
  assert.equal(previewTriangleBudget({ maxTriangles: 20000, geometrySampleRatio: .75 }), 15000);
});

test('frame comparison reports deterministic angular shading deltas', () => {
  const before = { count: 2, positions: new Float32Array([0, 0, 0, 1, 0, 0]), normals: new Float32Array([0, 0, 1, 1, 0, 0]), tangents: new Float32Array([1, 0, 0, 0, 1, 0]) };
  const current = { count: 2, positions: new Float32Array([0, 0, 0, 1, 0, 0]), normals: new Float32Array([0, 1, 0, 1, 0, 0]), tangents: new Float32Array([0, 1, 0, 0, 1, 0]) };
  const result = compareFrameSnapshots(before, current);
  assert.equal(result.compared, 2);
  assert.equal(result.normals.compared, 2);
  assert.ok(Math.abs(result.normals.meanDegrees - 45) < 1e-6);
  assert.equal(result.normals.maxDegrees, 90);
  assert.deepEqual(result.normals.samples[0], { position: [0, 0, 0], degrees: 90, normalized: .5 });
  assert.equal(result.tangents.meanDegrees, 45);
  assert.equal(compareFrameSnapshots(before, { count: 1, normals: current.normals }).tangents, null);
});

test('ray-hit projection interpolates source attributes and preserves misses', () => {
  const result = projectRayHitAttributes({ triangle: new Int32Array([0, -1]), barycentrics: new Float32Array([.25, .25, .5, 0, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceValues: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), itemSize: 3, missValue: -1 });
  assert.equal(result.hitCount, 1);
  assert.equal(result.missCount, 1);
  assert.deepEqual([...result.values.slice(0, 3)], [.25, .5, 0]);
  assert.deepEqual([...result.values.slice(3)], [-1, -1, -1]);
  assert.throws(() => projectRayHitAttributes({ triangle: { length: 1 }, barycentrics: new Float32Array(3), sourceIndices: new Uint32Array([0, 1, 2]), sourceValues: new Float32Array(9) }), /shapes/);
  assert.throws(() => projectRayHitAttributes({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceValues: new Float32Array(9) }), /shapes/);
});

test('CPU projection ray reference reports closest face with LightRT barycentric ordering', () => {
  const result = raycastTriangles({ origins: new Float32Array([0, 0, 1, 2, 0, 1]), directions: new Float32Array([0, 0, -1, 0, 0, -1]), positions: new Float32Array([-1, -1, 0, 1, -1, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), maxDistance: 10 });
  assert.deepEqual([...result.triangle], [0, -1]);
  assert.ok(Math.abs(result.distance[0] - 1) < 1e-6);
  assert.deepEqual([...result.barycentrics].map((value) => Number(value.toFixed(6))), [.25, .25, .5, 0, 0, 0]);
});

test('projection ray rasterization is deterministic and applies cage policy', () => {
  const result = buildProjectionRays({ positions: new Float32Array([-1, -1, 0, 1, -1, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, .5, 1]), normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]), resolution: 4, rayDistance: 2, cageOffset: .25 });
  assert.equal(result.origins.length, result.directions.length);
  assert.equal(result.origins.length, result.pixels.length * 3);
  assert.equal(result.covered.reduce((sum, value) => sum + value, 0), result.pixels.length);
  assert.equal(result.owners.length, 16);
  assert.deepEqual([...result.targetTriangles], [0, ...result.targetTriangles.slice(1)]);
  assert.equal(result.islandCount, 1);
  assert.doesNotThrow(() => validateProjectionResult({ resolution: 1, distance: new Float32Array([1]), triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), pixels: new Uint32Array([0]), covered: new Uint8Array([1]), owners: new Int32Array([0]), islandCount: 1, targetTriangle: new Int32Array([2]), targetBarycentrics: new Float32Array([1, 0, 0]) }));
  assert.throws(() => validateProjectionResult({ resolution: 1, distance: new Float32Array([1]), triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), pixels: new Uint32Array([0]), covered: new Uint8Array([1]), owners: new Int32Array([0]), islandCount: -1 }), /raster buffers/);
  assert.equal(result.rayDistance, 2);
  assert.equal(result.cageOffset, .25);
  assert.ok(result.origins[2] > 0 && result.directions[2] < 0);
});

test('projected dilation ownership follows indexed UV islands and respects seam splits', () => {
  const shared = buildUVIslandOwners(new Uint32Array([0, 1, 2, 2, 1, 3]));
  assert.equal(shared.islandCount, 1);
  assert.deepEqual([...shared.labels], [0, 0]);
  const split = buildUVIslandOwners(new Uint32Array([0, 1, 2, 3, 4, 5]));
  assert.equal(split.islandCount, 2);
  assert.deepEqual([...split.labels], [0, 1]);
  assert.throws(() => buildUVIslandOwners({ length: 3 }), /iterable index buffer/);
  const raster = buildProjectionRays({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0]), indices: new Uint32Array([0, 1, 2, 2, 1, 3]), uvs: new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]), normals: new Float32Array(12).fill(0).map((value, index) => index % 3 === 2 ? 1 : value), resolution: 8 });
  assert.deepEqual([...new Set(raster.targetTriangles)], [0, 1]);
  assert.deepEqual([...new Set([...raster.owners].filter((owner) => owner >= 0))], [0]);
});

test('projection result validation covers raster cardinality and ownership', () => {
  const valid = { resolution: 2, distance: new Float32Array([1]), triangle: new Int32Array([0]), barycentrics: new Float32Array([.25, .25, .5]), pixels: new Uint32Array([0]), covered: new Uint8Array([1, 0, 0, 0]), owners: new Int32Array([3, -1, -1, -1]), targetTriangle: new Int32Array([3]), targetBarycentrics: new Float32Array([.5, .25, .25]) };
  assert.equal(validateProjectionResult(valid), valid);
  assert.throws(() => validateProjectionResult(null), /invalid result object/);
  assert.throws(() => validateProjectionResult([]), /invalid result object/);
  assert.throws(() => validateProjectionResult({ ...valid, pixels: new Uint32Array([4]) }), /out-of-range/);
  assert.throws(() => validateProjectionResult({ ...valid, triangle: new Int32Array([-2]) }), /triangle sentinel/);
  assert.throws(() => validateProjectionResult({ ...valid, covered: new Uint8Array([2, 0, 0, 0]) }), /coverage mask/);
  assert.throws(() => validateProjectionResult({ ...valid, distance: new Float32Array([Number.NaN]) }), /hit distance/);
  assert.throws(() => validateProjectionResult({ ...valid, owners: new Int32Array([-1, -1, -1, -1]) }), /ownership/);
  assert.throws(() => validateProjectionResult({ ...valid, covered: new Uint8Array([1]) }), /raster buffers/);
  assert.doesNotThrow(() => validateProjectionResult({ ...valid, targetTriangle: new Int32Array([2]) }));
  assert.throws(() => validateProjectionResult({ ...valid, targetTriangle: new Int32Array([-1]) }), /target triangle ordinal/);
  assert.throws(() => validateProjectionResult({ ...valid, targetBarycentrics: new Float32Array([1, 0, 0, 0]) }), /raster buffers/);
  assert.throws(() => validateProjectionResult({ ...valid, distance: new Float32Array([Infinity]) }), /hit distance/);
  assert.doesNotThrow(() => validateProjectionResult({ ...valid, triangle: new Int32Array([-1]), distance: new Float32Array([Infinity]), covered: new Uint8Array([0, 0, 0, 0]), owners: new Int32Array([-1, -1, -1, -1]) }));
  assert.throws(() => validateProjectionResult({ ...valid, sourceTriangleCount: 1, triangle: new Int32Array([1]) }), /source triangle sentinel/);
  assert.throws(() => validateProjectionResult({ ...valid, targetTriangleCount: 3, owners: new Int32Array([2, -1, -1, -1]) }), /target triangle ordinal/);
  assert.throws(() => validateProjectionResult({ ...valid, triangle: { length: 1 } }), /raster buffers/);
  assert.throws(() => validateProjectionResult({ ...valid, targetTriangle: { length: 1 } }), /raster buffers/);
  assert.throws(() => validateProjectionResult({ ...valid, targetTriangleCount: 3, targetTriangle: new Int32Array([2]), owners: new Int32Array([3, -1, -1, -1]) }), /target-face owner/);
  assert.throws(() => validateProjectionResult({ ...valid, distance: [1] }), /untyped buffers/);
});

test('projected dilation stays within target face ownership', () => {
  const result = dilateProjectedPixels({ pixels: new Uint8ClampedArray([10, 20, 30, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), hitMask: new Uint8Array([1, 0, 0, 0]), owners: new Int32Array([4, 4, 9, -1]), resolution: 2, passes: 1 });
  assert.equal(result.filledCount, 1);
  assert.deepEqual([...result.pixels.slice(4, 8)], [10, 20, 30, 255]);
  assert.equal(result.hitMask[2], 0);
});

test('projection geometry uses column-major world transforms', () => {
  const translated = transformProjectionPositions(new Float32Array([0, 0, 0, 1, 0, 0]), new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1]));
  assert.deepEqual([...translated], [3, 4, 5, 4, 4, 5]);
  assert.deepEqual([...transformProjectionPositions(new Float32Array([1, 2, 3]), null)], [1, 2, 3]);
});

test('projected source UVs sample RGBA textures with deterministic V orientation', () => {
  const result = sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([.25, .25, .5]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 0, 1, 0, 0, 1]), texturePixels: new Uint8Array([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]), width: 2, height: 2 });
  assert.deepEqual([...result].map((value) => Number(value.toFixed(6))), [0, 0, 1]);
});

test('projected scalar textures collapse to the requested data channel', () => {
  const result = sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 1, 1, 1, 0, 1]), texturePixels: new Uint8Array([64, 128, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255]), width: 2, height: 2, channelIndex: 0 });
  assert.ok([...result].every((value) => Math.abs(value - 64 / 255) < 1e-6));
});

test('projected color sampling converts linear source images before sRGB encoding', () => {
  const result = sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 1, 1, 1, 0, 1]), texturePixels: new Float32Array([.25, .5, .75, 1]), width: 1, height: 1, sourceColorSpace: 'linear', destinationColorSpace: 'srgb' });
  assert.ok(Math.abs(result[0] - 0.5370987) < 1e-6);
  assert.ok(Math.abs(result[1] - 0.7353569) < 1e-6);
  assert.ok(Math.abs(result[2] - 0.880825) < 1e-6);
});

test('projected texture sampling honors resolved LightUSD color transforms', () => {
  const result = sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 1, 1, 1, 0, 1]), texturePixels: new Float32Array([.5, .5, .5, 1]), width: 1, height: 1, sourceColorSpace: 'srgb', destinationColorSpace: 'linear-srgb', sourceColorTransform: { canonical: 'g22_rec709_scene', colorRole: 'data', gamma: 2.2, linearBias: 0, matrix: [1, 0, 0, 0, 1, 0, 0, 0, 1] } });
  assert.ok(Math.abs(result[0] - .5 ** 2.2) < 1e-6);
});

test('projected texture sampling follows source face material indices', () => {
  const result = sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 1, 1, 1, 0, 1]), sourceMaterialIndices: new Uint32Array([1]), materialTextures: [{ pixels: new Uint8Array([255, 0, 0, 255]), width: 1, height: 1 }, { pixels: new Uint8Array([0, 255, 0, 255]), width: 1, height: 1 }], width: 1, height: 1 });
  assert.deepEqual([...result], [0, 1, 0]);
  assert.throws(() => sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 1, 1, 1, 0, 1]), sourceMaterialIndices: new Uint32Array([1]), materialTextures: [null] }), /descriptors are missing/);
  assert.throws(() => sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 1, 1, 1, 0, 1]), materialTextures: [null] }), /descriptors are missing/);
  assert.throws(() => sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 1, 1, 1, 0, 1]), texturePixels: new Float32Array([NaN, 0, 0, 1]), width: 1, height: 1 }), /finite pixel/);
  assert.throws(() => sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([NaN, 1, 1, 1, 0, 1]), texturePixels: new Uint8Array([255, 0, 0, 255]), width: 1, height: 1 }), /source UVs/);
  assert.throws(() => sampleProjectedTexture({ triangle: new Int32Array([0]), barycentrics: new Float32Array([1, 0, 0]), sourceIndices: new Uint32Array([0, 1, 2]), sourceUVs: new Float32Array([0, 1, 1, 1, 0, 1]), texturePixels: new Uint8Array([255, 0, 0, 255]), width: 2, height: 1, materialTextures: [null] }), /texture buffers/);
});

test('projected attribute interpolation supports per-face material fallback', () => {
  const result = projectRayHitAttributes({ triangle: new Int32Array([0]), barycentrics: new Float32Array([.2, .3, .5]), sourceIndices: new Uint32Array([0, 1, 2]), sourceValues: new Float32Array([1, 0, 0, 1, 0, 0, 1, 0, 0]), faceValues: new Float32Array([0, 1, 0]), itemSize: 3 });
  assert.deepEqual([...result.values], [0, 1, 0]);
});

test('bake color conversion keeps sRGB identity and encodes linear color channels', () => {
  assert.equal(normalizeBakeColorSpace('Display P3'), 'display-p3');
  assert.equal(normalizeBakeColorSpace('linear-rec2020'), 'linear-rec2020');
  assert.equal(normalizeBakeColorSpace('Adobe RGB (1998)'), 'adobergb');
  assert.equal(normalizeBakeColorSpace('lin_adobergb'), 'linear-adobergb');
  assert.throws(() => normalizeBakeColorSpace('camera-log'), /Unsupported bake color space/);
  assert.deepEqual(convertBakeColor([.2, .5, .8], 'srgb', 'srgb'), [.2, .5, .8]);
  const encoded = convertBakeColor([.25, .25, .25], 'linear', 'srgb');
  assert.ok(Math.abs(encoded[0] - 0.5370987) < 1e-6);
  assert.deepEqual(convertBakeColor([.2, .5, .8], 'SRGBColorSpace', 'srgb'), [.2, .5, .8]);
  assert.deepEqual(convertBakeColor([.2, .5, .8], 'srgb_texture', 'raw'), [.2, .5, .8]);
  assert.deepEqual(convertBakeColor([.2, .5, .8], 'linear', 'raw'), [.2, .5, .8]);
  const p3 = convertBakeColor([.2, .5, .8], 'display-p3', 'srgb'), acescg = convertBakeColor([.2, .5, .8], 'acescg', 'srgb'), aces2065 = convertBakeColor([.2, .5, .8], 'aces2065-1', 'srgb'), rec2020 = convertBakeColor([.2, .5, .8], 'rec2020', 'srgb');
  assert.ok(p3.every((value) => Number.isFinite(value) && value >= 0 && value <= 1));
  assert.ok(acescg.every((value) => Number.isFinite(value) && value >= 0 && value <= 1));
  assert.ok(aces2065.every((value) => Number.isFinite(value) && value >= 0 && value <= 1));
  assert.ok(rec2020.every((value) => Number.isFinite(value) && value >= 0 && value <= 1));
  assert.ok(Math.abs(convertBakeColor([.2, .5, .8], 'linear-display-p3', 'linear-srgb')[0] - .1325179471) < 1e-6);
  assert.ok(Math.abs(convertBakeColor(convertBakeColor([.2, .5, .8], 'linear-srgb', 'acescg'), 'acescg', 'linear-srgb')[1] - .5) < 1e-5);
  assert.ok(Math.abs(convertBakeColor(convertBakeColor([.2, .5, .8], 'linear-srgb', 'linear-rec2020'), 'linear-rec2020', 'linear-srgb')[2] - .8) < 1e-5);
  const adobe = convertBakeColor([.2, .5, .8], 'adobergb', 'srgb');
  assert.ok(adobe.every((value) => Number.isFinite(value) && value >= 0 && value <= 1));
  const adobeRoundTrip = convertBakeColor(convertBakeColor([.2, .5, .8], 'linear-srgb', 'linear-adobergb'), 'linear-adobergb', 'linear-srgb');
  adobeRoundTrip.forEach((value, index) => assert.ok(Math.abs(value - [.2, .5, .8][index]) < 1e-6));
  assert.throws(() => convertBakeColor([.5, .5], 'srgb', 'linear'), /exactly three/);
  assert.throws(() => convertBakeColor([.5, Number.NaN, .5], 'srgb', 'linear'), /finite RGB/);
  assert.throws(() => convertBakeColor({ length: 3, 0: .5, 1: .5, 2: .5 }, 'srgb', 'linear'), /exactly three/);
  assert.throws(() => convertBakeColor([.2, .5, .8], 'camera-log', 'camera-log'), /Unsupported bake color space/);
});

test('embedded ICC matrix-shaper profiles decode into bake transforms', () => {
  const profile = new Uint8Array(312), view = new DataView(profile.buffer), write = (offset, value) => [...value].forEach((character, index) => view.setUint8(offset + index, character.charCodeAt(0)));
  view.setUint32(0, profile.length, false); write(12, 'mntr'); write(16, 'RGB '); write(36, 'acsp'); view.setUint32(128, 6, false);
  const xyz = [[.436, .222, .014], [.385, .717, .097], [.143, .061, .714]], offsets = [204, 224, 244];
  ['rXYZ', 'gXYZ', 'bXYZ'].forEach((tag, index) => { const table = 132 + index * 12, offset = offsets[index]; write(table, tag); view.setUint32(table + 4, offset, false); view.setUint32(table + 8, 20, false); write(offset, 'XYZ '); xyz[index].forEach((value, component) => view.setInt32(offset + 8 + component * 4, Math.round(value * 65536), false)); });
  [264, 280, 296].forEach((offset, index) => { const table = 132 + (index + 3) * 12; write(table, ['rTRC', 'gTRC', 'bTRC'][index]); view.setUint32(table + 4, offset, false); view.setUint32(table + 8, 14, false); write(offset, 'curv'); view.setUint32(offset + 8, 1, false); view.setUint16(offset + 12, Math.round(2.2 * 256), false); });
  const transform = decodeEmbeddedICCProfile(profile);
  assert.equal(transform.canonical, 'icc-matrix-shaper');
  assert.ok(Math.abs(transform.gamma - 2.2) < .005);
  assert.equal(transform.matrix.length, 9);
  assert.ok(transform.matrix.every(Number.isFinite));
  assert.ok(convertBakeColor([.2, .5, .8], 'srgb', 'linear-srgb', transform).every(Number.isFinite));
  const roundTrip = decodeEmbeddedICCProfile(encodeEmbeddedICCProfile(transform));
  assert.ok(Math.abs(roundTrip.gamma - transform.gamma) < .005);
  roundTrip.matrix.forEach((value) => assert.ok(Number.isFinite(value)));
  const nonUniform = decodeEmbeddedICCProfile(encodeEmbeddedICCProfile({ ...transform, gamma: [2.2, 2, 1.8] }));
  assert.deepEqual(nonUniform.gamma.map((value) => Number(value.toFixed(3))), [2.199, 2, 1.801]);
  assert.equal(normalizeBakeColorTransform({ ...transform, gamma: [2.2, 2, 1.8] }).gamma.length, 3);
  assert.ok(convertBakeColor([.2, .5, .8], 'srgb', 'linear-srgb', { ...transform, gamma: [2.2, 2, 1.8] }).every(Number.isFinite));
  assert.throws(() => encodeEmbeddedICCProfile({ matrix: [1, 0, 0, 0, 1, 0, 0, 0, 1], gamma: 0 }), /gamma/);
  assert.deepEqual(normalizeBakeColorTransform({ iccProfile: profile }).canonical, 'icc-matrix-shaper');
  assert.equal(normalizeBakeColorTransform({ iccProfile: profile.subarray(0, 100) }), null);
  assert.throws(() => decodeEmbeddedICCProfile(profile.subarray(0, 100)), /truncated/);
  view.setUint8(36, 0); assert.throws(() => decodeEmbeddedICCProfile(profile), /supported RGB/);
});

test('UV bake honors resolved LightUSD color transforms', () => {
  const transform = { canonical: 'g22_rec709_scene', colorRole: 'data', gamma: 2.2, linearBias: 0, matrix: [1, 0, 0, 0, 1, 0, 0, 0, 1] };
  const actual = convertBakeColor([.25, .5, .75], 'srgb', 'linear-srgb', transform);
  const expected = [.25 ** 2.2, .5 ** 2.2, .75 ** 2.2];
  expected.forEach((value, index) => assert.ok(Math.abs(actual[index] - value) < 1e-12));
  assert.deepEqual(normalizeBakeColorTransform({ matrix: [1, 2] }), null);
  assert.deepEqual(normalizeBakeColorTransform({ matrix: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]), gamma: 1 }), { matrix: [1, 0, 0, 0, 1, 0, 0, 0, 1], gamma: 1, bias: 0, canonical: '', colorRole: '' });
  assert.deepEqual(normalizeBakeColorTransform({ sourceToDisplayLinear: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]), sourceGamma: 2.2, sourceLinearBias: 0, sourceColorIsData: true, canonical: 'g22_rec709_scene' }), { matrix: [1, 0, 0, 0, 1, 0, 0, 0, 1], gamma: 2.2, bias: 0, canonical: 'g22_rec709_scene', colorRole: 'data' });
  const resolvedMetadata = convertBakeColor([.25, .5, .75], 'srgb', 'linear-srgb', { sourceToDisplayLinear: [1, 0, 0, 0, 1, 0, 0, 0, 1], sourceGamma: 2.2, sourceLinearBias: 0, sourceColorIsData: true });
  resolvedMetadata.forEach((value, index) => assert.ok(Math.abs(value - [.25, .5, .75][index] ** 2.2) < 1e-12));
  assert.notDeepEqual(convertBakeColor([.25, .5, .75], 'srgb', 'srgb', transform), [.25, .5, .75]);
});

test('processing recipes are portable and deterministic', () => {
  const recipe = createProcessingRecipe({ profile: 'mobile-ar', operations: [{ id: 'mesh.cleanup:/World/Mesh', fix: 'mesh.cleanup', path: '/World/Mesh', dependsOn: [] }, { id: 'unsafe', fix: 'mesh.cleanup', path: '/home/private/mesh' }] });
  assert.equal(recipe.operations.length, 1);
  const serialized = serializeProcessingRecipe(recipe);
  assert.equal(serialized.endsWith('\n'), true);
  assert.deepEqual(parseProcessingRecipe(serialized), recipe);
  assert.throws(() => parseProcessingRecipe('{"schemaVersion":1,"profile":"web-viewer","operations":[{"id":"x","fix":"mesh.cleanup","path":"/home/private"}]}'), /unsafe/);
  assert.throws(() => parseProcessingRecipe('{"schemaVersion":1,"profile":"web-viewer","operations":[{"id":"x","fix":"mesh.cleanup","path":"/Mesh","dependsOn":4}]}'), /malformed operation collections/);
  assert.throws(() => parseProcessingRecipe('{"schemaVersion":1,"profile":"web-viewer","operations":[{"id":"x","fix":"mesh.cleanup","path":"/Mesh","options":[]}]}'), /malformed operation collections/);
  assert.throws(() => parseProcessingRecipe('{"schemaVersion":1,"profile":"web-viewer","operations":[{"id":"x","fix":"mesh.cleanup","path":"/Mesh","issueIds":[null]}]}'), /malformed operation collections/);
  assert.throws(() => orderProcessingRecipe({ operations: [{ id: 'a', dependsOn: ['b'] }, { id: 'b', dependsOn: ['a'] }] }), /cycle/);
  assert.throws(() => parseProcessingRecipe(JSON.stringify({ schemaVersion: 1, profile: 'web-viewer', operations: [{ id: 'a', fix: 'mesh.cleanup', path: '/World/Mesh', options: { sourcePath: 'cache/../private.usda' } }] })), /unsafe or incomplete/);
});

test('processing recipe normalization tolerates malformed collection fields', () => {
  assert.deepEqual(createProcessingRecipe(null).operations, []);
  assert.deepEqual(createProcessingRecipe([]).operations, []);
  const recipe = createProcessingRecipe({ operations: null });
  assert.deepEqual(recipe.operations, []);
  const normalized = createProcessingRecipe({ operations: [{ id: 'a', fix: 'mesh.cleanup', path: '/Mesh', dependsOn: [null, 'b'], issueIds: [null, 'bad'], options: [] }] });
  assert.deepEqual(normalized.operations[0].dependsOn, ['b']);
  assert.deepEqual(normalized.operations[0].issueIds, ['bad']);
  assert.deepEqual(normalized.operations[0].options, {});
  assert.deepEqual(orderProcessingRecipe({ operations: null }), []);
  const editable = createProcessingRecipe({ operations: [{ id: 'a', fix: 'mesh.cleanup', path: '/Mesh' }] });
  assert.deepEqual(editProcessingRecipe(editable, null).operations.map((operation) => operation.id), ['a']);
  assert.throws(() => editProcessingRecipe(editable, { orderedIds: 'a' }), /each enabled operation/);
});

test('processing recipe editing preserves dependencies while disabling and reordering', () => {
  const recipe = createProcessingRecipe({ operations: [{ id: 'a', fix: 'mesh.cleanup', path: '/M', dependsOn: [] }, { id: 'b', fix: 'mesh.recomputeNormals', path: '/M', dependsOn: ['a'] }, { id: 'c', fix: 'mesh.recomputeTangents', path: '/M', dependsOn: ['b'] }] });
  const edited = editProcessingRecipe(recipe, { orderedIds: ['a', 'b', 'c'] });
  assert.deepEqual(edited.operations.map((operation) => operation.id), ['a', 'b', 'c']);
  assert.throws(() => editProcessingRecipe(recipe, { disabledIds: ['b'] }), /depends on it/);
  assert.throws(() => editProcessingRecipe(recipe, { orderedIds: ['b', 'a', 'c'] }), /dependency after/);
  const reduced = editProcessingRecipe(recipe, { disabledIds: ['c'] });
  assert.deepEqual(reduced.operations.map((operation) => operation.id), ['a', 'b']);
});

test('processing recipes retain analyzer issue provenance for no-op skipping', () => {
  const recipe = createProcessingRecipe({ operations: [{ id: 'mesh.cleanup:/Mesh', fix: 'mesh.cleanup', path: '/Mesh', issueIds: ['topology.unusedVertex', 'topology.duplicateIndex'] }] });
  assert.deepEqual(recipe.operations[0].issueIds, ['topology.duplicateIndex', 'topology.unusedVertex']);
  assert.deepEqual(parseProcessingRecipe(serializeProcessingRecipe(recipe)).operations[0].issueIds, recipe.operations[0].issueIds);
});

test('repair plans expose deterministic mesh-size work estimates', () => {
  const plan = proposeRepairPlan({ meshes: [{ path: '/World/Mesh', triangles: 100, vertices: 60 }], issues: [{ fix: 'mesh.cleanup', path: '/World/Mesh', ruleId: 'topology.zeroArea' }] });
  assert.equal(plan.operations[0].estimatedWork, 200);
  const recipe = createProcessingRecipe(plan);
  assert.equal(parseProcessingRecipe(serializeProcessingRecipe(recipe)).operations[0].estimatedWork, 200);
  assert.equal(proposeRepairPlan({ meshes: [{ path: '/World/Mesh', triangles: -10, vertices: 2.5 }], issues: [{ fix: 'mesh.cleanup', path: '/World/Mesh', ruleId: 'topology.bad' }] }).operations[0].estimatedWork, 2);
  assert.equal(proposeRepairPlan({ meshes: [{ path: '/World/Mesh', triangles: Number.MAX_SAFE_INTEGER, vertices: 0 }], issues: [{ fix: 'mesh.cleanup', path: '/World/Mesh', ruleId: 'topology.bad' }] }).operations[0].estimatedWork, Number.MAX_SAFE_INTEGER);
});

test('repair planning fails closed for malformed report and graph collections', () => {
  assert.deepEqual(proposeRepairPlan({ issues: { invalid: true }, meshes: null }).operations, []);
  assert.deepEqual(proposeRepairPlan({ issues: [{ fix: 'mesh.cleanup', path: '/Mesh' }] }).operations[0].issueIds, []);
  assert.deepEqual(buildRepairOperationGraph({ operations: [null, { id: 'bad' }, { id: 'ok', path: '/Mesh', issueIds: 'bad', dependsOn: 4 }] }), { nodes: [{ id: 'ok', kind: 'operation', fix: undefined, path: '/Mesh', title: undefined, benefit: undefined, destructive: false, cost: undefined, estimatedWork: undefined }], edges: [] });
  assert.equal(repairOperationNeeded({ issues: null }, { fix: 'mesh.cleanup', path: '/Mesh', issueIds: ['topology.bad'] }), false);
  assert.equal(repairOperationNeeded({ issues: null }, { fix: 'mesh.cleanup', path: '/Mesh' }), true);
});

test('command snapshots restore project package state on undo and redo', async () => {
  const project = new LuciaProject(), session = { value: 'before', exportUSDA: () => session.value, restore: (value) => { session.value = value; } }, stack = new LuciaCommandStack();
  project.assets.set('old.png', { bytes: new Uint8Array([1]) });
  await stack.execute(sessionCommand(session, 'rename', ['/'], async () => { const before = session.value; session.value = 'after'; project.assets = new Map([['new.png', { bytes: new Uint8Array([2]) }]]); project.exportRemap = { 'old.png': 'new.png' }; return before; }, project));
  assert.equal(project.assets.has('new.png'), true);
  await stack.undo();
  assert.equal(project.assets.has('old.png'), true);
  assert.deepEqual(project.exportRemap, {});
  await stack.redo();
  assert.equal(project.assets.has('new.png'), true);
  assert.deepEqual(project.exportRemap, { 'old.png': 'new.png' });
});

test('command execution reports busy rejection', async () => {
  const stack = new LuciaCommandStack();
  stack.busy = true;
  assert.equal(await stack.execute({ do: async () => 'unused' }), false);
});

test('worker cancellation rejects the active operation and clears resources', () => {
  const operations = new LuciaOperations({}, {}, {}), worker = { terminated: false, terminate() { this.terminated = true; } };
  let error = null;
  operations.worker = worker;
  operations.workerReject = (value) => { error = value; };
  operations.cancel();
  assert.equal(worker.terminated, true);
  assert.equal(operations.worker, null);
  assert.equal(operations.workerReject, null);
  assert.equal(error.code, 'LUCIA_CANCELLED');
});

test('UV transfer result validation rejects non-iterable buffers', () => {
  const valid = { uvs: new Float32Array([0, 0, 1, 0]), hitCount: 2, missCount: 0, hitMask: new Uint8Array([1, 1]) };
  assert.equal(validateUVTransferResult(valid, 2), valid);
  assert.throws(() => validateUVTransferResult({ ...valid, uvs: { length: 4 } }, 2), /malformed/);
  assert.throws(() => validateUVTransferResult({ ...valid, hitMask: { length: 2 } }, 2), /malformed/);
});

test('UV transfer normal validation rejects non-iterable buffers', () => {
  assert.deepEqual([...validateUVTransferNormals(new Float32Array([0, 0, 1]), 1)], [0, 0, 1]);
  assert.throws(() => validateUVTransferNormals({ length: 3, 0: 0, 1: 0, 2: 1 }, 1), /one normal/);
  assert.throws(() => validateUVTransferNormals([0, 0, 1], 1), /typed buffer/);
});

test('skin transfer result validation rejects non-iterable buffers', () => {
  const valid = { jointIndices: new Uint16Array([0, 0, 0, 0]), jointWeights: new Float32Array([1, 0, 0, 0]), targetVertexCount: 1, meanDistance: 0, maxDistance: 0, distances: new Float32Array([0]) };
  assert.equal(validateSkinTransferResult(valid, 1), valid);
  assert.throws(() => validateSkinTransferResult({ ...valid, jointIndices: { length: 4 } }, 1), /malformed/);
  assert.throws(() => validateSkinTransferResult({ ...valid, distances: { length: 1 } }, 1), /malformed/);
  assert.throws(() => validateSkinTransferResult({ ...valid, jointWeights: [...valid.jointWeights] }, 1), /untyped/);
});

test('successful worker operations detach their worker handles', async () => {
  const previousWorker = globalThis.Worker, workers = [];
  class SuccessfulWorker {
    constructor() { this.terminated = false; workers.push(this); }
    postMessage() { queueMicrotask(() => this.onmessage?.({ data: { type: 'result', normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]) } })); }
    terminate() { this.terminated = true; }
  }
  globalThis.Worker = SuccessfulWorker;
  try {
    const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } } }, operations = new LuciaOperations({ setMeshGeometry: async () => null }, {}, { objectForPath: () => mesh });
    await operations.recomputeNormals('/Mesh');
    assert.equal(operations.worker, null);
    assert.equal(operations.workerReject, null);
    assert.equal(workers.length, 1);
    assert.equal(workers[0].terminated, true);
  } finally {
    if (previousWorker === undefined) delete globalThis.Worker; else globalThis.Worker = previousWorker;
  }
});

test('normal operation authors face-varying worker output with corner indices', async () => {
  const previousWorker = globalThis.Worker; let authored = null;
  class FaceVaryingWorker {
    constructor() { this.terminated = false; }
    postMessage() { queueMicrotask(() => this.onmessage?.({ data: { type: 'result', normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]) } })); }
    terminate() { this.terminated = true; }
  }
  globalThis.Worker = FaceVaryingWorker;
  try {
    const mesh = { isMesh: true, geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } } }, operations = new LuciaOperations({ getMeshSharpChainData: () => ({ chains: [[0, 1, 2]], sharpness: [.5] }), setMeshGeometry: async (_path, data) => { authored = data; return 'previous'; } }, {}, { objectForPath: () => mesh });
    await operations.recomputeNormals('/Mesh', { interpolation: 'faceVarying' });
    assert.equal(authored.normals.length, 9);
    assert.deepEqual([...authored.normalIndices], [0, 1, 2]);
    assert.deepEqual(authored.sharpChains, [[0, 1, 2]]);
    assert.deepEqual(authored.sharpChainSharpness, [.5]);
  } finally {
    if (previousWorker === undefined) delete globalThis.Worker; else globalThis.Worker = previousWorker;
  }
});

test('worker postMessage failures terminate and detach the active worker', () => {
  const owner = { worker: null, workerReject: null }, worker = { terminated: false, postMessage() { throw new Error('clone failed'); }, terminate() { this.terminated = true; } };
  owner.worker = worker;
  let error = null;
  postWorkerMessage(owner, worker, {}, [], (value) => { error = value; }, 'LUCIA_TEST_WORKER');
  assert.equal(worker.terminated, true);
  assert.equal(owner.worker, null);
  assert.equal(owner.workerReject, null);
  assert.equal(error.code, 'LUCIA_TEST_WORKER');
  assert.match(error.message, /clone failed/);
});

test('bake rejects unsupported channel names before touching scene state', async () => {
  const operations = new LuciaOperations({}, {}, {});
  await assert.rejects(operations.bake('/World/Mesh', { channel: 'unknown' }), /Unsupported bake channel/);
  await assert.rejects(operations.bake('/World/Mesh', { resolution: 32 }), /supported range/);
  await assert.rejects(operations.bakeProjected('/World/Target', '/World/High', { rayDistance: 0 }), /supported range/);
  await assert.rejects(operations.bake('/World/Mesh', { channel: 'normal', normalY: 'vulkan' }), /opengl or directx/);
  await assert.rejects(operations.bake('/World/Mesh', { channel: 'normal', normalSpace: 'world' }), /object or tangent/);
});

test('xatlas options are normalized deterministically', () => {
  assert.deepEqual(normalizeUnwrapOptions({ resolution: 99999, padding: -3, rotateCharts: false, rotateChartsToAxis: false, blockAlign: true, singleAtlasFallback: false, maxIterations: 0, maxCost: 999 }), { resolution: 8192, padding: 0, texelsPerUnit: 0, maxChartSize: 0, rotateChartsToAxis: false, rotateCharts: false, blockAlign: true, bruteForce: false, singleAtlasFallback: false, maxIterations: 1, maxCost: 100 });
});

test('xatlas UV presets provide deterministic defaults', () => {
  assert.deepEqual(normalizeUnwrapOptions({ preset: 'lightmap' }), { resolution: 1024, padding: 4, texelsPerUnit: 0, maxChartSize: 0, rotateChartsToAxis: true, rotateCharts: true, blockAlign: false, bruteForce: false, singleAtlasFallback: true, preset: 'lightmap', textureSeamWeight: 2 });
  assert.deepEqual(normalizeUnwrapOptions(null), normalizeUnwrapOptions({}));
});

test('xatlas single-atlas fallback options are bounded', () => {
  assert.deepEqual(nextSingleAtlasOptions({ resolution: 1024, texelsPerUnit: 64 }), { resolution: 2048, texelsPerUnit: 0 });
  assert.equal(nextSingleAtlasOptions({ resolution: 8192 }), null);
  assert.throws(() => nextSingleAtlasOptions('bad'), /fallback options must be an object/);
  assert.throws(() => nextSingleAtlasOptions([]), /fallback options must be an object/);
  assert.deepEqual(nextSingleAtlasOptions({ resolution: '1024' }), { resolution: 2048, texelsPerUnit: 0 });
});

test('UV atlas result validation rejects malformed WASM output', () => {
  const valid = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), indices: new Uint32Array([0, 1, 2]), xref: new Uint32Array([0, 1, 2]) };
  assert.equal(validateUVAtlasResult(valid), valid);
  assert.throws(() => validateUVAtlasResult({ ...valid, indices: new Uint32Array([0, 1, 3]) }), /out-of-range/);
  assert.throws(() => validateUVAtlasResult({ ...valid, uvs: new Float32Array([0, 0]) }), /mismatched/);
  assert.throws(() => validateUVAtlasResult({ ...valid, uvs: new Float32Array([0, 1.1, 1, 0, 0, 1]) }), /out-of-range/);
  assert.throws(() => validateUVAtlasResult({ ...valid, atlasCount: 2 }), /sub-atlases/);
  assert.throws(() => validateUVAtlasResult({ ...valid, singleAtlasFallback: 'yes' }), /fallback marker/);
  assert.throws(() => validateUVAtlasResult({ ...valid, width: 0 }), /dimensions/);
  assert.throws(() => validateUVAtlasResult({ ...valid, height: 16384 }), /dimensions/);
  assert.throws(() => validateUVAtlasResult({ ...valid, xref: null }), /source remap/);
  assert.throws(() => validateUVAtlasResult({ ...valid, chartCount: 0 }), /chart count/);
  assert.throws(() => validateUVAtlasResult({ ...valid, chartIndices: new Int32Array([0]) }), /chartIndices/);
  assert.throws(() => validateUVAtlasResult({ ...valid, sourceVertexCount: 2, xref: new Uint32Array([0, 1, 2]) }), /source remap/);
  assert.throws(() => validateUVAtlasResult({ ...valid, xref: new Float32Array([0, 1, -1]) }), /source remap/);
  assert.throws(() => validateUVAtlasResult({ ...valid, xref: new Float64Array([0, 1, 4294967297]) }), /source remap/);
  assert.throws(() => validateUVAtlasResult({ ...valid, chartCount: 1, chartIndices: new Int32Array([0, 1, 0]) }), /out-of-range chart/);
  assert.throws(() => validateUVAtlasResult({ ...valid, atlasCount: 1, atlasIndices: new Int32Array([0, 2, 0]) }), /out-of-range sub-atlas/);
  assert.throws(() => validateUVAtlasResult({ ...valid, sourceVertexCount: 0 }), /source vertex count/);
  assert.throws(() => validateUVAtlasResult({ ...valid, chartIndices: { length: 3 } }), /chartIndices/);
  assert.throws(() => validateUVAtlasResult({ ...valid, chartIndices: new Int32Array([0, 0, 0]) }), /chart count/);
  assert.throws(() => validateUVAtlasResult({ ...valid, atlasIndices: new Int32Array([0, 0, 0]) }), /atlas count/);
  assert.throws(() => validateUVAtlasResult({ positions: new Float32Array(), uvs: new Float32Array(), indices: new Uint32Array(), xref: new Uint32Array() }), /mismatched mesh/);
});

test('UV atlas rejects an unsupported UV set before starting a worker', async () => {
  const mesh = { isMesh: true, geometry: {
    attributes: { position: { count: 3, itemSize: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } },
    index: { count: 3, array: new Uint16Array([0, 1, 2]) },
  } };
  const operations = new LuciaOperations({}, {}, { objectForPath: () => mesh });
  await assert.rejects(operations.unwrap('/World/Mesh', { uvSet: 'secondary' }), /Unsupported UV set/);
  await assert.rejects(operations.transferUVs('/World/Target', '/World/Source', { uvSet: 'secondary' }), /Unsupported UV set/);
});

test('UV atlas result validation checks custom attribute buffers', () => {
  const result = { positions: new Float32Array(9), uvs: new Float32Array(6), indices: new Uint32Array([0, 1, 2]), xref: new Uint32Array([0, 1, 2]), customAttributes: [{ name: 'temperature', itemSize: 1, array: new Float32Array([1, 2]) }] };
  assert.throws(() => validateUVAtlasResult({ ...result, positions: [0, 0, 0, 1, 0, 0, 0, 1, 0] }), /untyped mesh buffers/);
  assert.throws(() => validateUVAtlasResult(result), /custom attribute buffer/);
  assert.throws(() => validateUVAtlasResult({ ...result, customAttributes: [{ name: 'temperature', itemSize: 1, array: [1, 2, 3] }] }), /custom attribute buffer/);
  assert.throws(() => validateUVAtlasResult({ ...result, customAttributes: [{ name: 'st', itemSize: 1, array: new Float32Array([1, 2, 3]) }] }), /reserved/);
  assert.throws(() => validateUVAtlasResult({ ...result, customAttributes: [{ name: 'normal', itemSize: 3, array: new Float32Array(9) }] }), /reserved/);
  assert.throws(() => validateUVAtlasResult({ ...result, customAttributes: [{ name: 'temperature', itemSize: 1, array: new Float32Array([1, 2, NaN]) }] }), /non-finite temperature/);
});

test('UV atlas remapping rejects duplicate custom attribute names', () => {
  const attribute = { name: 'mask', itemSize: 1, array: new Float32Array([1, 2, 3]) };
  assert.throws(() => remapXatlasCustomAttributes([attribute, { ...attribute, array: new Float32Array(attribute.array) }], new Uint32Array([0, 1, 2])), /duplicated/);
  assert.throws(() => validateUVAtlasResult({ positions: new Float32Array(9), uvs: new Float32Array(6), indices: new Uint32Array([0, 1, 2]), xref: new Uint32Array([0, 1, 2]), customAttributes: [attribute, { ...attribute, array: new Float32Array(attribute.array) }] }), /duplicate custom attribute/);
});

test('UV atlas result validation checks aligned seam buffers', () => {
  const result = { positions: new Float32Array(9), uvs: new Float32Array(6), indices: new Uint32Array([0, 1, 2]), xref: new Uint32Array([0, 1, 2]), normals: new Float32Array([0, 0, 1, 0, 0, 1, NaN, 0, 1]) };
  assert.throws(() => validateUVAtlasResult(result), /non-finite normals/);
  assert.throws(() => validateUVAtlasResult({ ...result, normals: new Float32Array(9), chartIndices: [0, 0, 0], chartCount: 1 }), /typed/);
});

test('retopology result validation accepts meshes without UVs', () => {
  const result = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), xref: new Uint32Array([0, 1, 2]), colors: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]) };
  assert.equal(validateRetopoResult(result), result);
  assert.throws(() => validateRetopoResult({ ...result, positions: [...result.positions] }), /untyped/);
  assert.throws(() => validateRetopoResult({ ...result, indices: new Uint32Array([0, 1, 4]) }), /out-of-range/);
  assert.throws(() => validateRetopoResult({ ...result, sourceVertexCount: 2 }), /source remap/);
  assert.throws(() => validateRetopoResult({ ...result, sourceVertexCount: Infinity }), /source vertex count/);
  assert.throws(() => validateRetopoResult({ ...result, groups: [{ start: 0, count: 0, materialIndex: 0 }] }), /material groups/);
  const custom = { name: 'detail', itemSize: 1, array: new Float32Array([1, 2, 3]) };
  assert.throws(() => validateRetopoResult({ ...result, customAttributes: [custom, { ...custom, array: new Float32Array(custom.array) }] }), /custom attribute/);
});

test('all deterministic templates contain a valid World definition', () => {
  for (const kind of ['product', 'room', 'materials', 'empty']) {
    const source = templateUSDA(kind);
    assert.match(source, /^#usda 1\.0/);
    assert.match(source, /def Xform "World"/);
  }
  assert.equal(templateUSDA('product'), templateUSDA('product'));
});

test('prim block lookup respects hierarchy', () => {
  const source = templateUSDA('product');
  const world = findPrimBlock(source, '/World');
  const hero = findPrimBlock(source, '/World/Hero');
  assert.ok(world && hero);
  assert.ok(hero.start > world.start && hero.end < world.end);
  assert.equal(findPrimBlock(source, '/World/Missing'), null);
});

test('USD session authors selectable material variants without replacing materials', async () => {
  const session = new LuciaUsdSession();
  try {
    await session.init();
    await session.loadUSDA('#usda 1.0\ndef Scope "Materials" {\n    def Material "Red" {}\n    def Material "Blue" {}\n}\ndef Xform "World" {\n    def Mesh "M" (\n        variants = { string lod = "high" }\n    ) {\n        rel material:binding = </Materials/Red>\n    }\n}');
    assert.deepEqual(session.getMeshMaterialPaths('/World/M'), ['/Materials/Red']);
    await session.setMaterialVariants('/World/M', [
      { name: 'material_Blue', materialPath: '/Materials/Blue' },
      { name: 'material_Red', materialPath: '/Materials/Red' },
    ]);
    assert.match(session.usda, /variantSet "luciaMaterial"/);
    assert.match(session.usda, /string lod = "high"/);
    assert.match(session.usda, /string luciaMaterial = "material_Blue"/);
    assert.match(session.usda, /rel material:binding = <\/Materials\/Red>/);
    assert.ok(findPrimBlock(session.usda, '/World/M'));
  } finally { session.dispose(); }
});

test('USD material variants override stronger GeomSubset bindings', async () => {
  const session = new LuciaUsdSession();
  try {
    await session.init();
    await session.loadUSDA('#usda 1.0\ndef Scope "Materials" {\n    def Material "Red" {}\n    def Material "Blue" {}\n}\ndef Xform "World" {\n    def Mesh "M" {\n        def GeomSubset "A" {\n            int[] indices = [0]\n            rel material:binding = </Materials/Red>\n        }\n        def GeomSubset "B" {\n            int[] indices = [1]\n            rel material:binding = </Materials/Blue>\n        }\n    }\n}');
    await session.setMaterialVariants('/World/M', [
      { name: 'material_Blue', materialPath: '/Materials/Blue' },
      { name: 'material_Red', materialPath: '/Materials/Red' },
    ]);
    assert.match(session.usda, /over "A"/);
    assert.match(session.usda, /over "B"/);
    assert.match(session.usda, /over "B"\s*\{\s*rel material:binding = <\/Materials\/Red>/);
  } finally { session.dispose(); }
});

test('USD session rejects duplicate material variant entries', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" {}';
  session.replaceUSDA = async () => { throw new Error('must not author'); };
  await assert.rejects(() => session.setMaterialVariants('/M', [
    { name: 'same', materialPath: '/Materials/Red' },
    { name: 'same', materialPath: '/Materials/Blue' },
  ]), (error) => error.code === 'LUCIA_MATERIAL_VARIANT');
});

test('USD session requires material prims for material variants', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" {}\ndef Mesh "NotMaterial" {}\ndef Material "ValidMaterial" {}';
  session.replaceUSDA = async () => { throw new Error('must not author'); };
  await assert.rejects(() => session.setMaterialVariants('/M', [
    { name: 'mesh', materialPath: '/NotMaterial' },
    { name: 'other', materialPath: '/ValidMaterial' },
  ]), (error) => error.code === 'LUCIA_MATERIAL_VARIANT_PATH');
});

test('USD session rejects collisions with existing variant selections', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Material "Red" {}\ndef Material "Blue" {}\ndef Mesh "M" (\n    variants = { string lod = "high" }\n) {}';
  session.replaceUSDA = async () => { throw new Error('must not author'); };
  await assert.rejects(() => session.setMaterialVariants('/M', [
    { name: 'red', materialPath: '/Red' },
    { name: 'blue', materialPath: '/Blue' },
  ], 'lod'), (error) => error.code === 'LUCIA_MATERIAL_VARIANT_COLLISION');
});

test('attribute edit replaces exact property and inserts missing property', () => {
  let source = templateUSDA('product');
  source = setAttributeText(source, '/World/Hero', 'double3 xformOp:translate', '(1, 2, 3)');
  assert.match(source, /double3 xformOp:translate = \(1, 2, 3\)/);
  source = setAttributeText(source, '/World/Hero', 'token visibility', '"invisible"');
  assert.match(source, /token visibility = "invisible"/);
});

test('display color authoring writes constant interpolation and removes stale indices', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { color3f[] primvars:displayColor = [(0, 1, 0)] ( interpolation = "faceVarying" ) int[] primvars:displayColor:indices = [0] }\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setDisplayColor('/M', [2, 0.25, -1]);
  assert.match(session.usda, /primvars:displayColor[\s\S]*interpolation = "constant"/);
  assert.match(session.usda, /\(1\.0000, 0\.2500, 0\.0000\)/);
  assert.doesNotMatch(session.usda, /primvars:displayColor:indices/);
  session.usda = '#usda 1.0\ndef Mesh "M" {\n  color3f[] primvars:displayColor = [\n    (0, 1, 0),\n    (1, 0, 0)\n  ] ( interpolation = "faceVarying" )\n  int[] primvars:displayColor:indices = [0, 1]\n}\n';
  await session.setDisplayColor('/M', [0.1, 0.2, 0.3]);
  assert.equal((session.usda.match(/color3f\[\] primvars:displayColor/g) || []).length, 1);
  assert.match(session.usda, /\(0\.1000, 0\.2000, 0\.3000\)/);
  assert.doesNotMatch(session.usda, /\(0, 1, 0\)/);
  assert.doesNotMatch(session.usda, /primvars:displayColor:indices/);
  await assert.rejects(() => session.setDisplayColor('/M', [1, Infinity, 0]), /three finite numeric/);
});

test('mesh authoring supports face-varying UV indices', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { }\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0]), uvIndices: new Uint32Array([0, 1, 2]) });
  assert.match(session.usda, /interpolation = "faceVarying"/);
  assert.match(session.usda, /primvars:st:indices = \[0, 1, 2\]/);
});

test('USD session reads authored face-varying UV buffers for topology operations', () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] ( interpolation = "faceVarying" ) int[] primvars:st:indices = [0, 1, 2] }\n';
  const result = session.getMeshUVData('/M');
  assert.deepEqual([...result.uvs], [0, 0, 1, 0, 0, 1]);
  assert.deepEqual([...result.uvIndices], [0, 1, 2]);
});

test('USD session reads typed face-varying primvars for topology operations', () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { color3f[] primvars:cornerColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)] ( interpolation = "faceVarying" ) int[] primvars:cornerColor:indices = [2, 1, 0] }\n';
  const result = session.getMeshFaceVaryingPrimvars('/M', 3);
  assert.equal(result.length, 1);
  assert.equal(result[0].itemSize, 3);
  assert.deepEqual([...result[0].indices], [2, 1, 0]);
  assert.deepEqual([...result[0].array], [1, 0, 0, 0, 1, 0, 0, 0, 1]);
});

test('USD session rejects malformed authored face-varying primvars before cleanup', () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { float3[] primvars:corner = [(1, 2, 3)] ( interpolation = "faceVarying" ) int[] primvars:corner:indices = [1] }\n';
  assert.throws(() => session.getMeshFaceVaryingPrimvars('/M', 3), /malformed values or corner indices/);
});

test('USD session scopes UV interpolation to the exact authored declaration', () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] ( interpolation = "vertex" ) color3f[] primvars:corner = [(1, 0, 0)] ( interpolation = "faceVarying" ) int[] primvars:corner:indices = [0, 0, 0] }\n';
  const result = session.getMeshUVData('/M');
  assert.equal(result.uvIndices, null);
});

test('USD session fails closed on malformed authored lightmap UV indices', () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { texCoord2f[] primvars:st1 = [(0, 0), (1, 0), (0, 1)] ( interpolation = "faceVarying" ) }\n';
  assert.throws(() => session.getMeshUVData('/M', 'lightmap'), /Face-varying mesh UV indices are malformed/);
});

test('mesh authoring rejects invalid indexed topology before serialization', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA('#usda 1.0\ndef Mesh "M" {}');
    await assert.rejects(() => session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, NaN]), indices: new Uint32Array([0, 0, 0]) }), /invalid indexed data/);
  } finally { session.dispose(); }
});

test('mesh authoring normalizes allowlisted subdivision schemes', async () => {
  assert.equal(normalizeSubdivisionScheme('none'), 'none');
  assert.equal(normalizeSubdivisionScheme('catmull-clark'), 'catmullClark');
  assert.equal(normalizeSubdivisionScheme('CATMULL_CLARK'), 'catmullClark');
  assert.equal(normalizeSubdivisionScheme('LOOP'), 'loop');
  assert.equal(normalizeSubdivisionScheme(null), null);
  assert.throws(() => normalizeSubdivisionScheme('adaptive'), /Unsupported subdivisionScheme/);
  const session = new LuciaUsdSession(); await session.init();
  try { await session.loadUSDA('#usda 1.0\ndef Mesh "M" {}'); await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), subdivisionScheme: 'none' }); assert.match(session.usda, /token subdivisionScheme = "none"/); } finally { session.dispose(); }
});

test('mesh authoring normalizes bounded subdivision metadata', async () => {
  assert.equal(normalizeSubdivisionMetadata('interpolateBoundary', 'edge_and_corner'), 'edgeAndCorner');
  assert.equal(normalizeSubdivisionMetadata('faceVaryingLinearInterpolation', 'corners-plus-1'), 'cornersPlus1');
  assert.equal(normalizeSubdivisionMetadata('triangleSubdivisionRule', 'SMOOTH'), 'smooth');
  assert.throws(() => normalizeSubdivisionMetadata('interpolateBoundary', 'adaptive'), /Unsupported interpolateBoundary/);
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" {}\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  try { await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), interpolateBoundary: 'edge-and-corner', faceVaryingLinearInterpolation: 'cornersPlus2', triangleSubdivisionRule: 'smooth' }); assert.match(session.usda, /token interpolateBoundary = "edgeAndCorner"/); assert.match(session.usda, /token faceVaryingLinearInterpolation = "cornersPlus2"/); assert.match(session.usda, /token triangleSubdivisionRule = "smooth"/); } finally { session.dispose(); }
});

test('mesh authoring normalizes interpolation token spellings', async () => {
  assert.equal(normalizeInterpolationToken('face-varying'), 'faceVarying');
  assert.equal(normalizeInterpolationToken('VERTEX'), 'vertex');
  assert.equal(normalizeInterpolationToken('uniform'), 'uniform');
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" {}\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), customAttributes: [{ name: 'temperature', itemSize: 1, array: new Float32Array([1, 2, 3]), interpolation: 'VERTEX' }] });
  assert.match(session.usda, /primvars:temperature[\s\S]*interpolation = "vertex"/);
});

test('mesh authoring normalizes finite normals and rejects zero vectors', async () => {
  const normalized = normalizeMeshNormals(new Float32Array([0, 3, 4]));
  assert.equal(normalized[0], 0); assert.ok(Math.abs(normalized[1] - 0.6) < 1e-6); assert.ok(Math.abs(normalized[2] - 0.8) < 1e-6);
  assert.throws(() => normalizeMeshNormals(new Float32Array([0, 0, 0])), /non-zero finite vectors/);
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" {}\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), normals: new Float32Array([0, 3, 4, 0, 0, 2, 1, 0, 0]) });
  assert.match(session.usda, /normal3f\[\] normals[\s\S]*\(0, 0\.6000000[0-9]*, 0\.8000000[0-9]*\)/);
  await assert.rejects(() => session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), normals: new Float32Array([0, 0, 0, 0, 0, 1, 1, 0, 0]) }), /non-zero finite vectors/);
});

test('mesh authoring persists normalized sharp-edge crease chains', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), sharpEdges: [[2, 0], [0, 2]] });
  assert.match(session.usda, /int\[\] creaseIndices = \[0, 2\]/);
  assert.match(session.usda, /int\[\] creaseLengths = \[2\]/);
  assert.match(session.usda, /float\[\] creaseSharpness = \[1\]/);
  assert.deepEqual(session.getMeshSharpEdges('/M'), [[0, 2]]);
  assert.deepEqual(session.getMeshSharpEdgeData('/M'), { edges: [[0, 2]], sharpness: [1] });
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), sharpEdges: [] });
  assert.deepEqual(session.getMeshSharpEdges('/M'), []);
  await assert.rejects(() => session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), sharpEdges: [[1, 1]] }), /distinct/);
});

test('mesh authoring preserves optional per-edge crease sharpness', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), sharpEdges: [[2, 0], [1, 2]], sharpEdgeSharpness: [0.25, 0.75] });
  assert.match(session.usda, /float\[\] creaseSharpness = \[0\.25, 0\.75\]/);
  await assert.rejects(() => session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), sharpEdges: [[0, 1]], sharpEdgeSharpness: [0, 0] }), /positive finite value/);
  await assert.rejects(() => session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), sharpEdges: [[0, 1]], sharpEdgeSharpness: [1, 2] }), /one positive finite value/);
});

test('mesh authoring preserves authored crease chains and chain sharpness', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), indices: new Uint32Array([0, 1, 2]), sharpChains: [[0, 1, 2], [2, 3]], sharpChainSharpness: [.5, 1.25] });
  assert.match(session.usda, /int\[\] creaseIndices = \[0, 1, 2, 2, 3\]/);
  assert.match(session.usda, /int\[\] creaseLengths = \[3, 2\]/);
  assert.deepEqual(session.getMeshSharpChainData('/M'), { chains: [[0, 1, 2], [2, 3]], sharpness: [.5, 1.25] });
  await assert.rejects(() => session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), sharpChains: [[0, 1]], sharpChainSharpness: [0, 0] }), /one positive finite value/);
});

test('mesh authoring rejects malformed aligned primvars instead of retaining stale data', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try { await session.loadUSDA('#usda 1.0\ndef Mesh "M" {}'); await assert.rejects(() => session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0]) }), /UVs must contain exactly/); } finally { session.dispose(); }
});

test('mesh authoring rejects malformed custom and face-varying attributes', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA('#usda 1.0\ndef Mesh "M" {}');
    const base = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) };
    await assert.rejects(() => session.setMeshGeometry('/M', { ...base, customAttributes: [{ name: 'bad', itemSize: 2, array: new Float32Array([1, 2]) }] }), /vertex\/corner aligned/);
    await assert.rejects(() => session.setMeshGeometry('/M', { ...base, customAttributes: [{ name: 'arrayValue', itemSize: 1, array: [1, 2, 3] }] }), /supported numeric typed array/);
    await assert.rejects(() => session.setMeshGeometry('/M', { ...base, faceVaryingAttributes: [{ name: 'corner', itemSize: 1, array: new Float32Array([1, 2, 3]), indices: new Uint32Array([0, 1]) }] }), /face corners/);
    await assert.rejects(() => session.setMeshGeometry('/M', { ...base, customAttributes: [{ name: 'uniformValue', itemSize: 1, array: new Float32Array([1, 2, 3]), interpolation: 'uniform' }] }), /only vertex interpolation/);
    await assert.rejects(() => session.setMeshGeometry('/M', { ...base, faceVaryingAttributes: [{ name: 'corner', itemSize: 1, array: new Float32Array([1, 2, 3]), indices: new Uint32Array([0, 1, 2]), interpolation: 'vertex' }] }), /only faceVarying interpolation/);
  } finally { session.dispose(); }
});

test('mesh authoring removes stale UV indices when switching to vertex interpolation', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] ( interpolation = "faceVarying" ) int[] primvars:st:indices = [0, 1, 2] }\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]) });
  assert.match(session.usda, /interpolation = "vertex"/);
  assert.doesNotMatch(session.usda, /primvars:st:indices/);
});

test('mesh authoring removes same-line stale UV indices', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] ( interpolation = "faceVarying" ) int[] primvars:st:indices = [0, 1, 2] }\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]) });
  assert.match(session.usda, /interpolation = "vertex"/);
  assert.doesNotMatch(session.usda, /primvars:st:indices/);
});

test('USD session extracts authored crease chains as sharp edges', () => {
  const session = Object.create(LuciaUsdSession.prototype);
  session.usda = '#usda 1.0\ndef Mesh "M" { int[] creaseIndices = [0, 2, 3, 4, 5] int[] creaseLengths = [3, 2] }';
  assert.deepEqual(session.getMeshSharpEdges('/M'), [[0, 2], [2, 3], [4, 5]]);
  session.usda = '#usda 1.0\ndef Mesh "M" { int[] creaseIndices = [0, 2] int[] creaseLengths = [3] }';
  assert.throws(() => session.getMeshSharpEdges('/M'), /invalid chain length/);
  session.usda = '#usda 1.0\ndef Mesh "M" { uniform int[] creaseIndices = [0, 2] uniform int[] creaseLengths = [2] }';
  assert.deepEqual(session.getMeshSharpEdges('/M'), [[0, 2]]);
  session.usda = '#usda 1.0\ndef Mesh "M" { int[] creaseIndices = [0, 1, 2, 3] int[] creaseLengths = [2, 2] float[] creaseSharpness = [0, 1] }';
  assert.deepEqual(session.getMeshSharpEdges('/M'), [[2, 3]]);
  assert.deepEqual(session.getMeshSharpEdgeData('/M'), { edges: [[2, 3]], sharpness: [1] });
  session.usda = '#usda 1.0\ndef Mesh "M" { int[] creaseIndices = [0, 1, 1, 0] int[] creaseLengths = [2, 2] float[] creaseSharpness = [0.25, 0.75] }';
  assert.deepEqual(session.getMeshSharpEdgeData('/M'), { edges: [[0, 1]], sharpness: [0.75] });
  session.usda = '#usda 1.0\ndef Mesh "M" { int[] creaseIndices = [0, 1] int[] creaseLengths = [2] float[] creaseSharpness = [1, 0] }';
  assert.throws(() => session.getMeshSharpEdges('/M'), /one value per crease chain/);
  session.usda = '#usda 1.0\ndef Mesh "M" { int[] creaseIndices = [0, 2, 3, 4, 5] int[] creaseLengths = [3, 2] float[] creaseSharpness = [0.25, 0.75] }';
  assert.deepEqual(session.getMeshSharpChainData('/M'), { chains: [[0, 2, 3], [4, 5]], sharpness: [.25, .75] });
});

test('USD session returns a stable empty sharp-edge data shape', () => {
  const session = Object.create(LuciaUsdSession.prototype);
  session.usda = '#usda 1.0\ndef Mesh "M" {}';
  assert.deepEqual(session.getMeshSharpEdgeData('/M'), { edges: [], sharpness: [] });
  assert.deepEqual(session.getMeshSharpEdges('/M'), []);
});

test('USD session authors guide mesh siblings without render subsets', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Xform "World" {\n  def Mesh "Source" {\n    rel material:binding = </World/Looks/Mat>\n    def GeomSubset "Subset" { int[] indices = [0] }\n  }\n}\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.createGuideMeshSibling('/World/Source', 'Source_Collider', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) });
  assert.match(session.usda, /def Mesh "Source_Collider"/);
  assert.match(session.usda, /apiSchemas = \["PhysicsCollisionAPI", "PhysicsMeshCollisionAPI"\]/);
  assert.match(session.usda, /purpose = "guide"/);
  assert.match(session.usda, /uniform token physics:approximation = "convexHull"/);
  assert.equal((session.usda.match(/def GeomSubset/g) || []).length, 1);
  assert.equal((session.usda.match(/material:binding/g) || []).length, 1);
});

test('USD session authors explicit triangle collider approximation metadata', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Xform "World" {\n  def Mesh "Source" {}\n}\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.createGuideMeshSibling('/World/Source', 'Source_TriangleCollider', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) }, 'Create triangle collider', 'none');
  assert.match(session.usda, /PhysicsMeshCollisionAPI/);
  assert.match(session.usda, /uniform token physics:approximation = "none"/);
});

test('USD session authors collision groups with collection membership and filters', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Xform "World" {\n  def Mesh "ColliderA" {}\n  def Mesh "ColliderB" {}\n}\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.createCollisionGroup('/World', 'PlayerColliders', ['/World/ColliderA', '/World/ColliderB'], ['/World/EnemyColliders'], 'Create collision group', { mergeGroup: 'characters', invertFilteredGroups: true });
  assert.match(session.usda, /def PhysicsCollisionGroup "PlayerColliders"/);
  assert.match(session.usda, /prepend apiSchemas = \["CollectionAPI:colliders"\]/);
  assert.match(session.usda, /collection:colliders:includes = \[<\/World\/ColliderA>, <\/World\/ColliderB>\]/);
  assert.match(session.usda, /physics:filteredGroups = \[<\/World\/EnemyColliders>\]/);
  assert.match(session.usda, /string physics:mergeGroup = "characters"/);
  assert.match(session.usda, /bool physics:invertFilteredGroups = 1/);
  await assert.rejects(() => session.createCollisionGroup('/World', 'Bad', ['/World/../Unsafe']), /valid USD prim paths/);
});

test('mesh authoring updates existing GeomSubset face indices', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" {\n  int[] faceVertexCounts = [3, 3]\n  def GeomSubset "Red" {\n    uniform token elementType = "face"\n    int[] indices = [0]\n  }\n  def GeomSubset "Blue" {\n    uniform token elementType = "face"\n    int[] indices = [1]\n  }\n}\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', {
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1]),
    indices: new Uint32Array([0, 1, 2, 0, 3, 4]),
    groups: [{ start: 0, count: 3, materialIndex: 1 }, { start: 3, count: 3, materialIndex: 0 }],
  });
  assert.match(session.usda, /def GeomSubset "Red"[\s\S]*?int\[\] indices = \[1\]/);
  assert.match(session.usda, /def GeomSubset "Blue"[\s\S]*?int\[\] indices = \[0\]/);
});

test('mesh authoring clears stale GeomSubset indices when groups are removed', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" {\n  def GeomSubset "Red" { int[] indices = [0, 1] }\n}\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', {
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2]),
  });
  assert.match(session.usda, /def GeomSubset "Red"[\s\S]*?int\[\] indices = \[\]/);
});

test('mesh authoring clears GeomSubsets when groups are empty', async () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" {\n  def GeomSubset "Red" { int[] indices = [0] }\n}\n';
  session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), groups: [] });
  assert.match(session.usda, /def GeomSubset "Red"[\s\S]*?int\[\] indices = \[\]/);
});

test('USD session derives cleanup groups from authored GeomSubsets', () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" {\n  def GeomSubset "Red" { int[] indices = [1] }\n  def GeomSubset "Blue" { int[] indices = [0] }\n}\n';
  assert.deepEqual(session.getMeshMaterialGroups('/M', 6), [
    { start: 0, count: 3, materialIndex: 1 },
    { start: 3, count: 3, materialIndex: 0 },
  ]);
});

test('USD session rejects overflowing GeomSubset indices', () => {
  const session = new LuciaUsdSession();
  session.usda = '#usda 1.0\ndef Mesh "M" { def GeomSubset "Bad" { int[] indices = [999999999999999999999] } }\n';
  assert.throws(() => session.getMeshMaterialGroups('/M', 3), /indices are not readable/);
});

test('mesh authoring can target the separate lightmap UV set', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), uvSet: 'lightmap' });
  assert.match(session.usda, /texCoord2f\[\] primvars:st1/);
  assert.doesNotMatch(session.usda, /texCoord2f\[\] primvars:st\s*=/);
  await assert.rejects(session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0]), indices: new Uint32Array(), uvs: new Float32Array([0, 0]), uvSet: 'secondary' }), /Unsupported UV set/);
});

test('mesh authoring writes vertex tangent directions and preserves handedness convention', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), tangents: new Float32Array([1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1]) });
  assert.match(session.usda, /float3\[\] primvars:tangents/);
  assert.match(session.usda, /interpolation = "vertex"/);
  assert.match(session.usda, /\(1, 0, 0\)/);
});

test('mesh authoring invalidates stale tangents when no replacement is supplied', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" {\n  float3[] primvars:tangents = [(1, 0, 0), (1, 0, 0), (1, 0, 0)] ( interpolation = "vertex" )\n}\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) });
  assert.doesNotMatch(session.usda, /primvars:tangents/);
});

test('mesh authoring removes stale indices for vertex primvars', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { normal3f[] normals = [(0, 0, 1)] ( interpolation = "faceVarying" ) int[] normals:indices = [0] color3f[] primvars:displayColor = [(1, 0, 0)] ( interpolation = "faceVarying" ) int[] primvars:displayColor:indices = [0] }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]), colors: new Float32Array([1, 0, 0, 1, 0, 0, 1, 0, 0]) });
  assert.equal((session.usda.match(/normals:indices/g) || []).length, 0);
  assert.equal((session.usda.match(/primvars:displayColor:indices/g) || []).length, 0);
  assert.match(session.usda, /normal3f\[\].*interpolation = "vertex"/s);
  assert.match(session.usda, /primvars:displayColor.*interpolation = "vertex"/s);
});

test('mesh authoring writes face-varying normals and corner indices', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" {}\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]), normalIndices: new Uint32Array([0, 1, 2]) });
  assert.match(session.usda, /normal3f\[\] normals = \[\(0, 0, 1\), \(0, 0, 1\), \(0, 0, 1\)\] \( interpolation = "faceVarying" \)/);
  assert.match(session.usda, /int\[\] normals:indices = \[0, 1, 2\]/);
});

test('cleanup authoring can clear aligned primvars omitted from the result', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { normal3f[] normals = [(0, 0, 1)] int[] normals:indices = [0] texCoord2f[] primvars:st = [(0, 0)] color3f[] primvars:displayColor = [(1, 0, 0)] int[] primvars:skel:jointIndices = [0, 0, 0, 0] float[] primvars:skel:jointWeights = [1, 0, 0, 0] float3[] primvars:tangents = [(1, 0, 0)] }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), clearMissingAttributes: true });
  for (const token of ['normals', 'primvars:st', 'primvars:displayColor', 'primvars:skel:jointIndices', 'primvars:skel:jointWeights', 'primvars:tangents']) assert.equal(session.usda.includes(token), false, token);
});

test('cleanup authoring clears omitted face-varying primvars and indices', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { color3f[] primvars:cornerColor = [(1, 0, 0)] ( interpolation = "faceVarying" ) int[] primvars:cornerColor:indices = [0] }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), clearMissingAttributes: true });
  assert.equal(session.usda.includes('primvars:cornerColor'), false);
});

test('cleanup authoring clears only omitted known custom primvars', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { float[] primvars:stale = [1] float3[] primvars:kept = [(1, 2, 3)] }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), clearMissingAttributes: true, customAttributeNames: ['stale', 'kept'], customAttributes: [{ name: 'kept', itemSize: 3, array: new Float32Array([1, 2, 3, 1, 2, 3, 1, 2, 3]) }] });
  assert.equal(session.usda.includes('primvars:stale'), false);
  assert.equal(session.usda.includes('primvars:kept'), true);
});

test('mesh authoring preserves four-influence skin primvars', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), jointIndices: new Uint16Array([0, 1, 2, 3, 3, 2, 1, 0, 1, 1, 0, 2]), jointWeights: new Float32Array([1, 0, 0, 0, .5, .5, 0, 0, .25, .25, .25, .25]) });
  assert.match(session.usda, /int\[\] primvars:skel:jointIndices/);
  assert.match(session.usda, /float\[\] primvars:skel:jointWeights/);
  assert.match(session.usda, /interpolation = "vertex"/);
});

test('mesh authoring rejects incomplete or unsafe skin primvars', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  const base = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), jointWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) };
  await assert.rejects(() => session.setMeshGeometry('/M', base), /provided together/);
  await assert.rejects(() => session.setMeshGeometry('/M', { ...base, jointIndices: new Uint16Array([0, 1, 2, 3, 3, 2, 1, 0, 1, 1, 0, 2]), jointWeights: new Float32Array([1, -1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) }), /non-negative/);
  await assert.rejects(() => session.setMeshGeometry('/M', { ...base, jointIndices: new Float64Array([0, 65536, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), jointWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) }), /unsigned 16-bit/);
});

test('xatlas attribute remapping preserves aligned seam data', () => {
  const result = remapXatlasAttributes({ xref: new Uint32Array([2, 0, 2]), normals: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]), colors: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]), tangents: new Float32Array([1, 0, 0, 1, 0, 1, 0, -1, 0, 0, 1, 1]), jointIndices: new Uint16Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]), jointWeights: new Float32Array([1, 0, 0, 0, .5, .5, 0, 0, .25, .25, .25, .25]) });
  assert.deepEqual([...result.normals], [0, 0, 1, 1, 0, 0, 0, 0, 1]);
  assert.deepEqual([...result.tangents], [0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1]);
  assert.deepEqual([...result.jointIndices], [8, 9, 10, 11, 0, 1, 2, 3, 8, 9, 10, 11]);
  assert.deepEqual([...result.jointWeights], [0.25, 0.25, 0.25, 0.25, 1, 0, 0, 0, 0.25, 0.25, 0.25, 0.25]);
});

test('xatlas remaps arbitrary aligned vertex attributes through seam duplication', () => {
  const attributes = remapXatlasCustomAttributes([{ name: 'temperature', itemSize: 1, array: new Float32Array([10, 20, 30]) }, { name: 'jointMask', itemSize: 2, array: new Uint16Array([1, 2, 3, 4, 5, 6]) }], new Uint32Array([2, 0, 2]));
  assert.deepEqual([...attributes[0].array], [30, 10, 30]);
  assert.deepEqual([...attributes[1].array], [5, 6, 1, 2, 5, 6]);
  assert.throws(() => remapXatlasCustomAttributes([{ name: 'bad name', itemSize: 1, array: new Float32Array([1, 2, 3]) }], new Uint32Array([0])), /malformed/);
  assert.throws(() => remapXatlasCustomAttributes([{ name: 'temperature', itemSize: 1.5, array: new Float32Array([1, 2, 3]) }], new Uint32Array([0])), /malformed/);
  assert.throws(() => remapXatlasCustomAttributes([null], new Uint32Array([0])), /malformed/);
  assert.throws(() => remapXatlasCustomAttributes({}, new Uint32Array([0])), /must be an array/);
  assert.throws(() => remapXatlasCustomAttributes([], new Uint32Array()), /non-empty iterable/);
  assert.throws(() => remapXatlasAttributes({ xref: new Int32Array([-1]), normals: new Float32Array([1, 0, 0]) }), /invalid vertex/);
  assert.throws(() => remapXatlasAttributes({ xref: new Float64Array([4294967297]), normals: new Float32Array([1, 0, 0, 0, 1, 0]) }), /invalid vertex/);
  assert.throws(() => remapXatlasAttributes(null), /non-empty iterable/);
  assert.throws(() => remapXatlasAttributes({ xref: {} }), /non-empty iterable/);
  assert.throws(() => remapXatlasAttributes({ xref: [0] }), /typed numeric array/);
  assert.throws(() => remapXatlasAttributes({ xref: new Uint32Array([2]), normals: new Float32Array([1, 0, 0]) }), /shorter/);
});

test('xatlas transfer buffers are deduplicated before worker ownership transfer', () => {
  const storage = new ArrayBuffer(32), shared = new Uint8Array(storage), result = { positions: shared, uvs: new Uint8Array(storage, 0, 8), indices: new Uint16Array(3), customAttributes: [{ array: new Uint8Array(storage, 8, 8) }] };
  const buffers = collectXatlasTransferBuffers(result);
  assert.equal(buffers.length, 2);
  assert.equal(buffers[0], storage);
  assert.equal(buffers[1], result.indices.buffer);
});

test('mesh authoring writes custom aligned primvars', async () => {
  const session = new LuciaUsdSession(); session.usda = '#usda 1.0\ndef Mesh "M" { }\n'; session.replaceUSDA = async (source) => { session.usda = source; return source; };
  await session.setMeshGeometry('/M', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), customAttributes: [{ name: 'temperature', itemSize: 1, array: new Float32Array([10, 20, 30]) }, { name: 'jointMask', itemSize: 2, array: new Uint16Array([1, 2, 3, 4, 5, 6]) }, { name: 'packedColor', itemSize: 3, array: new Uint8ClampedArray([1, 2, 3, 4, 5, 6, 7, 8, 9]) }] });
  assert.match(session.usda, /float\[\] primvars:temperature = \[10, 20, 30\]/);
  assert.match(session.usda, /int2\[\] primvars:jointMask = \[\(1, 2\), \(3, 4\), \(5, 6\)\]/);
  assert.match(session.usda, /int3\[\] primvars:packedColor = \[\(1, 2, 3\), \(4, 5, 6\), \(7, 8, 9\)\]/);
  assert.match(session.usda, /interpolation = "vertex"/);
});

test('asset report is deterministic and finds missing UVs and degenerate faces', () => {
  const mesh = { isMesh: true, name: 'Mesh', userData: { 'primMeta.absPath': '/World/Mesh' }, geometry: {
    attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 0, 0]) } },
    index: { count: 3, array: new Uint32Array([0, 1, 2]) },
  }, material: null };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.triangles, 1);
  assert.equal(report.stage.vertices, 3);
  assert.equal(report.score.physics, 94);
  assert.deepEqual(report.issues.map((item) => item.ruleId), ['geom.missingNormals', 'geom.missingUV', 'geom.zeroAreaFace', 'topology.boundaryEdge']);
  assert.equal(report.issues.find((item) => item.ruleId === 'geom.missingNormals').fix, 'mesh.recomputeNormals');
  assert.equal(analyzeAsset({ traverse(callback) { callback(mesh); } }).issues[0].ruleId, report.issues[0].ruleId);
});

test('asset report flags negative and extreme mesh scale axes', () => {
  const mesh = { isMesh: true, name: 'Scaled', scale: { x: -1, y: 20000, z: 1 }, geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint32Array([0, 1, 2]) } }, material: null };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.negativeScales, 1);
  assert.equal(report.stage.extremeTransforms, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'transform.negativeScale'));
  assert.ok(report.issues.some((item) => item.ruleId === 'transform.extremeScale'));
});

test('asset report flags non-finite and extreme pose transforms', () => {
  const mesh = { isMesh: true, name: 'Pose', position: { x: 2e6, y: 0, z: 0 }, rotation: { x: NaN, y: 0, z: 0 }, scale: { x: 1, y: 1, z: 1 }, geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint32Array([0, 1, 2]) } }, material: null };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.nonFiniteTransforms, 1);
  assert.equal(report.stage.extremeTransforms, 1);
  assert.equal(report.meshes[0].nonFiniteTransform, 1);
  assert.equal(report.meshes[0].extremeTransform, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'transform.nonFinite'));
  assert.ok(report.issues.some((item) => item.ruleId === 'transform.extremeTranslation'));
});

test('asset report detects shared edges with inconsistent winding', () => {
  const mesh = { isMesh: true, name: 'Winding', geometry: { attributes: { position: { count: 4, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0]) } }, index: { count: 6, array: new Uint32Array([0, 1, 2, 0, 1, 3]) } }, material: null };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.inconsistentWinding, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'topology.inconsistentWinding' && item.fix === 'mesh.cleanup'));
});

test('asset report flags invalid UV components', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: {
    attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, uv: { count: 3, array: new Float32Array([0, 0, 2, Number.NaN, 0.5, 0.5]) } },
    index: { count: 3, array: new Uint16Array([0, 1, 2]) },
  } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.uvMeshes, 1);
  assert.equal(report.stage.uvNonFinite, 1);
  assert.equal(report.stage.uvOutOfRange, 1);
  assert.deepEqual(report.issues.map((item) => item.ruleId), ['geom.missingNormals', 'topology.boundaryEdge', 'uv.nonFinite', 'uv.outOfRange']);
});

test('asset report flags invalid normal data', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: {
    attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, normal: { count: 2, array: new Float32Array([0, 0, 0, Number.NaN, 0, 1]) }, uv: { count: 3, array: new Float32Array([0, 0, 1, 0, 0, 1]) } },
    index: { count: 3, array: new Uint16Array([0, 1, 2]) },
  } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.deepEqual(report.issues.map((item) => item.ruleId), ['normal.countMismatch', 'normal.nonFinite', 'normal.zeroLength', 'topology.boundaryEdge']);
});

test('asset report detects inverted authored normals', () => {
  const mesh = { isMesh: true, name: 'Inverted', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, normal: { count: 3, array: new Float32Array([0, 0, -1, 0, 0, -1, 0, 0, -1]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: null };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.invertedNormals, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'normal.inverted' && item.fix === 'mesh.recomputeNormals'));
});

test('asset report only requires tangents for normal-map materials', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, normal: { count: 3, array: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]) }, uv: { count: 3, array: new Float32Array([0, 0, 1, 0, 0, 1]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: { normalMap: { isTexture: true, uuid: 'normal' } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.missingTangents, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'tangent.missing'));
});

test('asset report detects byte-identical texture identities', () => {
  const pixels = new Uint8Array([255, 0, 0, 255]);
  const texture = (uuid) => ({ isTexture: true, uuid, image: { width: 1, height: 1, data: pixels } });
  const mesh = { isMesh: true, name: 'Textured', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, uv: { count: 3, array: new Float32Array([0, 0, 1, 0, 0, 1]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: { map: texture('a'), emissiveMap: texture('b') } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.ok(report.issues.some((item) => item.ruleId === 'texture.duplicate'));
  assert.equal(report.textures.find((item) => item.id === 'b').duplicateOf, 'a');
});

test('asset report finds topology connectivity issues', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 7, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 0, 0, 3, 0, 0, 2, 1, 0, 9, 9, 9]) } }, index: { count: 9, array: new Uint32Array([0, 1, 2, 3, 4, 5, 3, 3, 4]) } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.unusedVertices, 1);
  assert.equal(report.stage.duplicateIndices, 1);
  assert.equal(report.stage.disconnectedComponents, 2);
  assert.ok(report.issues.some((item) => item.ruleId === 'topology.boundaryEdge'));
  assert.ok(report.issues.some((item) => item.ruleId === 'topology.duplicateIndex'));
});

test('asset report detects malformed position and index array metadata', () => {
  const mesh = { isMesh: true, name: 'Malformed', geometry: { attributes: { position: { count: 3, itemSize: 2, array: new Float32Array([0, 0, 0, 1, 0, 0]) } }, index: { count: 2, array: new Uint16Array([0, 1, 2]) } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.malformedAttributes, 1);
  assert.equal(report.stage.indexCountMismatches, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'topology.positionAttribute'));
  assert.ok(report.issues.some((item) => item.ruleId === 'topology.indexCountMismatch'));
});

test('asset report diagnoses missing and invalid topology arrays', () => {
  const missing = { isMesh: true, name: 'MissingPosition', geometry: { attributes: {} } };
  const invalidIndex = { isMesh: true, name: 'InvalidIndex', geometry: { attributes: { position: { count: 3, array: new Float32Array(9) } }, index: { count: 3 } } };
  const report = analyzeAsset({ traverse(callback) { callback(missing); callback(invalidIndex); } });
  assert.ok(report.issues.some((item) => item.ruleId === 'topology.missingPosition'));
  assert.ok(report.issues.some((item) => item.ruleId === 'topology.indexArray'));
  assert.equal(report.stage.malformedAttributes, 2);
});

test('asset report detects zero-area UV triangles and bounds', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: {
    attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, uv: { count: 3, array: new Float32Array([0, 0, 0.5, 0.5, 1, 1]) } },
    index: { count: 3, array: new Uint16Array([0, 1, 2]) },
  } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.uvZeroArea, 1);
  assert.equal(report.stage.uvBounds.area, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'uv.zeroAreaFace'));
});

test('asset report scores textures with missing image data', () => {
  const material = { name: 'Mat', map: { isTexture: true, uuid: 'tex-1', image: null } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.missingTextureImages, 1);
  assert.equal(report.score.textures, 80);
  assert.ok(report.issues.some((item) => item.ruleId === 'texture.missingImage'));
});

test('asset report distinguishes malformed texture dimensions from missing images', () => {
  const material = { name: 'Mat', map: { isTexture: true, uuid: 'tex-invalid', image: { width: 4.5, height: 2 } } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.missingTextureImages, 0);
  assert.equal(report.stage.invalidTextureDimensions, 1);
  assert.deepEqual(report.textures[0], { id: 'tex-invalid', width: 0, height: 0, bytes: 0, hasImage: false, pixelCount: null, expectedPixelCount: null, invalidPixels: false, colorSpace: 'unknown', normalY: null, slots: ['map'] });
  assert.ok(report.issues.some((item) => item.ruleId === 'texture.invalidDimensions' && item.severity === 'error'));
  assert.equal(report.issues.some((item) => item.ruleId === 'texture.missingImage'), false);
});

test('asset report rejects texture dimensions with unsafe RGBA cardinality', () => {
  const material = { name: 'Mat', map: { isTexture: true, uuid: 'tex-huge', image: { width: Number.MAX_SAFE_INTEGER, height: 2 } } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array(9) } } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.invalidTextureDimensions, 1);
  assert.equal(report.textures[0].hasImage, false);
  assert.ok(report.issues.some((item) => item.ruleId === 'texture.invalidDimensions'));
});

test('asset report normalizes malformed material and texture identifiers', () => {
  const material = { uuid: { invalid: true }, name: 'Fallback', type: 17, map: { isTexture: true, uuid: 42, image: { width: 1, height: 1 } } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.materials[0].id, 'Fallback');
  assert.equal(report.materials[0].shader, '17');
  assert.equal(report.textures[0].id, '42');
  assert.equal(report.materials[0].bindings[0].textureId, '42');
});

test('asset report detects malformed RGBA pixel cardinality', () => {
  const material = { name: 'Mat', map: { isTexture: true, uuid: 'tex-pixels', image: { width: 2, height: 2, pixels: new Uint8Array(3) } } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.invalidTexturePixels, 1);
  assert.equal(report.score.textures, 80);
  assert.deepEqual(report.textures[0], { id: 'tex-pixels', width: 2, height: 2, bytes: 16, hasImage: true, pixelCount: 3, expectedPixelCount: 16, invalidPixels: true, colorSpace: 'unknown', normalY: null, slots: ['map'] });
  assert.ok(report.issues.some((item) => item.ruleId === 'texture.invalidPixels' && item.severity === 'error'));
});

test('asset report diagnoses non-iterable and non-finite texture pixels safely', () => {
  const makeMesh = (texture) => ({ isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: { map: texture } });
  const malformed = analyzeAsset({ traverse(callback) { callback(makeMesh({ isTexture: true, uuid: 'object-pixels', image: { width: 1, height: 1, data: { length: 4 } } })); } });
  assert.equal(malformed.stage.invalidTexturePixels, 1);
  const nonFinite = analyzeAsset({ traverse(callback) { callback(makeMesh({ isTexture: true, uuid: 'nan-pixels', image: { width: 1, height: 1, data: new Float32Array([1, 0, NaN, 1]) } })); } });
  assert.equal(nonFinite.stage.invalidTexturePixels, 1);
  assert.match(nonFinite.issues.find((item) => item.ruleId === 'texture.invalidPixels').message, /non-finite/);
});

test('asset report diagnoses malformed scene traversal nodes safely', () => {
  const report = analyzeAsset({ traverse(callback) { callback(null); callback([]); } });
  assert.equal(report.stage.malformedAttributes, 2);
  assert.deepEqual(report.issues.map((item) => item.ruleId), ['scene.invalidNode', 'scene.invalidNode']);
});

test('asset report diagnoses malformed optional mesh buffers safely', () => {
  const mesh = {
    isMesh: true,
    name: 'Mesh',
    geometry: {
      attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) }, normal: { count: 3, array: {} }, uv: { count: 3, array: {} }, tangent: { count: 3, array: {} } },
      index: { count: 3, array: new Uint16Array([0, 1, 2]) },
    },
  };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.malformedAttributes, 3);
  assert.deepEqual(report.issues.filter((item) => item.ruleId === 'topology.attributeArray').map((item) => item.message), ['normal attribute has a non-iterable backing array.', 'tangent attribute has a non-iterable backing array.', 'uv attribute has a non-iterable backing array.']);
});

test('asset report diagnoses malformed material-group collections safely', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, groups: {}, index: { count: 3, array: new Uint16Array([0, 1, 2]) } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.malformedAttributes, 1);
  assert.deepEqual(report.issues.filter((item) => item.ruleId === 'topology.groupCollection').map((item) => item.message), ['Mesh material groups must be an array.']);
});

test('asset report does not fingerprint malformed geometry as duplicate', () => {
  const makeMesh = (name) => ({ isMesh: true, name, geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, NaN, 1, 0, 0, 0, 1, 0]) } } } });
  const meshes = [makeMesh('A'), makeMesh('B')], report = analyzeAsset({ traverse(callback) { meshes.forEach(callback); } });
  assert.deepEqual(report.meshes.map((mesh) => mesh.duplicateOf), [null, null]);
  assert.equal(report.stage.duplicateGeometries, 0);
});

test('asset report inventories shader bindings and color spaces', () => {
  const texture = { isTexture: true, uuid: 'albedo-1', image: { width: 4, height: 2 }, colorSpace: 'srgb' };
  const material = { uuid: 'mat-1', name: 'Paint', type: 'MeshStandardMaterial', map: texture };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.deepEqual(report.materials[0], { id: 'mat-1', name: 'Paint', shader: 'MeshStandardMaterial', paths: ['Mesh'], bindings: [{ slot: 'map', textureId: 'albedo-1', colorSpace: 'srgb' }], subsets: [] });
  assert.deepEqual(report.textures[0].slots, ['map']);
});

test('asset report summarizes material counts and shader types', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: [{ uuid: 'standard', type: 'MeshStandardMaterial' }, { uuid: 'custom', type: 'ShaderMaterial' }] };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.materialCount, 2);
  assert.deepEqual(report.stage.shaderTypes, ['MeshStandardMaterial', 'ShaderMaterial']);
  assert.equal(report.stage.unsupportedShaderCount, 1);
});

test('asset report exposes graph inventory without changing legacy materials', () => {
  const material = { uuid: 'mat-graph', name: 'Graph', type: 'MeshStandardMaterial', userData: { nodes: { output: { type: 'standard_surface', inputs: { base: 'color' } }, color: { type: 'Color' }, custom: { type: 'CustomNode' } } } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.deepEqual(report.materials[0].nodes.map((node) => node.id), ['color', 'custom', 'output']);
  assert.deepEqual(report.materials[0].unsupportedNodes, ['custom']);
  assert.deepEqual(report.materials[0].unreachableNodes, ['custom']);
  assert.ok(report.issues.some((item) => item.ruleId === 'material.unsupportedNode'));
  assert.equal(report.score.materials, 90);
});

test('asset report ignores malformed material graph input containers', () => {
  const material = { uuid: 'malformed-graph', type: 'MeshStandardMaterial', userData: { nodes: {
    output: { type: 'output', inputs: { surface: { node: 'noise' } } },
    noise: { type: 'noise', inputs: 'not-a-map' },
  } } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  const entry = report.materials[0];
  assert.deepEqual(entry.nodes.find((node) => node.id === 'noise').inputs, []);
  assert.deepEqual(entry.unresolvedReferences, []);
});

test('asset report inventories Map-backed material graphs', () => {
  const material = { userData: { nodes: new Map([
    [0, { id: 0, type: 'output', inputs: { surface: { node: 1 } } }],
    [1, { id: 1, type: 'standard_surface', inputs: { roughness: .5 } }],
  ]) } };
  const report = inventoryMaterialGraph(material);
  assert.deepEqual(report.nodes.map((node) => node.id), ['0', '1']);
  assert.deepEqual(report.unreachableNodes, []);
});

test('asset report compares equivalent material graphs independently of node IDs', () => {
  const makeMaterial = (uuid, constantId, addId) => ({ uuid, type: 'MeshStandardMaterial', userData: { nodes: {
    output: { type: 'output', inputs: { surface: { node: addId } } },
    [addId]: { type: 'add', inputs: { a: { node: constantId }, b: 2 } },
    [constantId]: { type: 'constant', value: 1 },
  } } });
  const first = makeMaterial('m1', 'a', 'sum'); first.userData.nodes.dead = { type: 'custom', value: 'ignored' };
  const second = makeMaterial('m2', 'constant-renamed', 'add-renamed'), meshes = [
    { isMesh: true, name: 'First', material: first, geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } } },
    { isMesh: true, name: 'Second', material: second, geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } } } },
  ];
  const report = analyzeAsset({ traverse(callback) { meshes.forEach(callback); } });
  assert.equal(report.materials.find((item) => item.id === 'm2').duplicateOf, 'm1');
  assert.ok(report.issues.some((item) => item.ruleId === 'material.duplicate'));
});

test('asset report detects equivalent materials without merging them', () => {
  const material = (uuid, name) => ({ uuid, name, type: 'MeshStandardMaterial', roughness: .4, metalness: .2, opacity: 1, color: { r: .5, g: .25, b: .75 } });
  const makeMesh = (name, material) => ({ isMesh: true, name, geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material });
  const report = analyzeAsset({ traverse(callback) { callback(makeMesh('A', material('mat-a', 'A'))); callback(makeMesh('B', material('mat-b', 'B'))); } });
  assert.equal(report.materials.length, 2);
  assert.equal(report.materials.find((item) => item.id === 'mat-b').duplicateOf, 'mat-a');
  assert.ok(report.issues.some((item) => item.ruleId === 'material.duplicate' && item.path === 'B'));
});

test('asset report flags sRGB data textures', () => {
  const material = { name: 'Data', type: 'MeshStandardMaterial', roughnessMap: { isTexture: true, uuid: 'rough-1', image: { width: 2, height: 2 }, colorSpace: 'srgb' } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.ok(report.issues.some((item) => item.ruleId === 'texture.dataColorSpace'));
  assert.equal(report.score.materials, 90);
});

test('asset report flags oversized texture dimensions', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: { map: { isTexture: true, uuid: 'large', image: { width: 16384, height: 1024 }, colorSpace: 'srgb' } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.ok(report.issues.some((item) => item.ruleId === 'texture.oversized'));
});

test('asset report inventories normal-map Y convention metadata', () => {
  const texture = { isTexture: true, uuid: 'normal-1', image: { width: 2, height: 2 }, colorSpace: 'linear', userData: { normalY: 'directx' } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: { name: 'Mat', type: 'MeshStandardMaterial', normalMap: texture } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.textures[0].normalY, 'directx');
  assert.equal(report.materials[0].bindings[0].normalY, 'directx');
});

test('asset report flags invalid normal-map Y convention metadata', () => {
  const texture = { isTexture: true, uuid: 'normal-invalid', image: { width: 2, height: 2 }, userData: { normalY: 'vulkan' } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: { type: 'MeshStandardMaterial', normalMap: texture } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.ok(report.issues.some((item) => item.ruleId === 'texture.normalConvention'));
});

test('asset report inventories material subsets', () => {
  const materials = [{ uuid: 'm0', type: 'MeshStandardMaterial' }, { uuid: 'm1', type: 'MeshPhysicalMaterial' }];
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 6, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0]) } }, index: { count: 6, array: new Uint16Array([0, 1, 2, 3, 4, 5]) }, groups: [{ start: 0, count: 3, materialIndex: 0 }, { start: 3, count: 3, materialIndex: 1 }] }, material: materials };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.materials.find((item) => item.id === 'm0').subsets[0].count, 3);
  assert.equal(report.materials.find((item) => item.id === 'm1').subsets[0].start, 3);
});

test('asset report flags opaque shader materials', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material: { name: 'Custom', type: 'ShaderMaterial' } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.ok(report.issues.some((item) => item.ruleId === 'material.unsupportedShader'));
  assert.equal(report.score.materials, 75);
});

test('mock assistant can request an asset health report', () => {
  const assistant = new LuciaAssistant({ executeTool() {}, getSelection: () => '/World/Mesh', getSceneSummary: () => ({}), confirm() {} });
  assert.deepEqual(assistant.mock('diagnose this asset'), { name: 'scene.health_report', arguments: {} });
});

test('assistant routes material graph inspection as read-only', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('inspect the material shader graph'), { name: 'scene.inspect_material_graph', arguments: { path: '/World/Mesh' } });
  assert.deepEqual(assistant.mock('evaluate the material graph output roughness'), { name: 'scene.inspect_material_graph', arguments: { path: '/World/Mesh', output: 'roughness' } });
  assert.equal(assistant.validate({ name: 'scene.inspect_material_graph', arguments: { path: '/World/Mesh' } }).confirm, false);
  assert.equal(assistant.validate({ name: 'scene.inspect_material_graph', arguments: { path: '/World/Mesh', output: 'base_color' } }).confirm, false);
  assert.throws(() => assistant.validate({ name: 'scene.inspect_material_graph', arguments: { path: '/World/Mesh', output: 'bad output' } }), /output/);
});

test('assistant routes equivalent material inspection as read-only', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('preview equivalent materials'), { name: 'scene.inspect_material_merges', arguments: {} });
  assert.deepEqual(assistant.mock('show duplicate material candidates'), { name: 'scene.inspect_material_merges', arguments: {} });
  assert.equal(assistant.validate({ name: 'scene.inspect_material_merges', arguments: {} }).confirm, false);
});

test('assistant routes material parameterization inspection as read-only', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('inspect material primvar parameterization'), { name: 'scene.inspect_material_parameterization', arguments: { path: '/World/Mesh' } });
  assert.equal(assistant.validate({ name: 'scene.inspect_material_parameterization', arguments: { path: '/World/Mesh' } }).confirm, false);
  assert.throws(() => assistant.validate({ name: 'scene.inspect_material_parameterization', arguments: { path: '/World/Mesh', mode: 'shader' } }), /mode/);
});

test('assistant routes material parameterization authoring with confirmation', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh', executeTool: async () => {}, getSceneSummary: () => ({}), confirm: async () => true, recordDecision: () => {} });
  assert.deepEqual(assistant.mock('apply material primvar parameterization'), { name: 'scene.parameterize_materials', arguments: { path: '/World/Mesh', mode: 'primvar' } });
  assert.equal(assistant.validate({ name: 'scene.parameterize_materials', arguments: { path: '/World/Mesh', mode: 'primvar' } }).confirm, true);
  assert.equal(assistant.validate({ name: 'scene.parameterize_materials', arguments: { path: '/World/Mesh', mode: 'variant' } }).confirm, true);
  assert.deepEqual(assistant.mock('author material variant parameterization'), { name: 'scene.parameterize_materials', arguments: { path: '/World/Mesh', mode: 'variant' } });
});

test('assistant routes semantic suggestions as read-only', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Wheel' });
  assert.deepEqual(assistant.mock('inspect semantic roles'), { name: 'scene.inspect_semantic_suggestions', arguments: { path: '/World/Wheel' } });
  assert.equal(assistant.validate({ name: 'scene.inspect_semantic_suggestions', arguments: { path: '/World/Wheel' } }).confirm, false);
  assert.equal(assistant.validate({ name: 'scene.inspect_semantic_suggestions', arguments: { path: '/World/Wheel', approvedInference: true, inference: [] } }).confirm, false);
  assert.throws(() => assistant.validate({ name: 'scene.inspect_semantic_suggestions', arguments: { approvedInference: 'yes' } }), /approvedInference/);
  assert.throws(() => assistant.validate({ name: 'scene.inspect_semantic_suggestions', arguments: { inference: {} } }), /inference/);
});

test('mock assistant routes LOD-chain requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('generate LODs for this mesh'), { name: 'scene.generate_lods', arguments: { path: '/World/Mesh', ratios: [.5, .25, .125], targetError: 0, lockUVSeams: true, lockBorder: true } });
  assert.equal(assistant.mock('generate LODs without open boundaries').arguments.lockBorder, false);
  assert.equal(assistant.mock('generate LODs without UV seams').arguments.lockUVSeams, false);
  assert.throws(() => assistant.validate({ name: 'scene.generate_lods', arguments: { path: '/World/Mesh', lockUVSeams: 'no' } }), /lockUVSeams must be boolean/);
});

test('mock assistant routes reduced triangle collider requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('generate a reduced triangle collider'), { name: 'scene.generate_triangle_collider', arguments: { path: '/World/Mesh', targetRatio: .25, targetError: .01 } });
});

test('assistant validates and routes collision group requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Collider', getSceneSummary: () => ({}) });
  assert.deepEqual(assistant.mock('create a collision group called PlayerColliders'), { name: 'scene.create_collision_group', arguments: { path: '/World/Collider', name: 'PlayerColliders' } });
  assert.equal(assistant.validate({ name: 'scene.create_collision_group', arguments: { path: '/World/Collider', colliderPaths: ['/World/Collider'], mergeGroup: 'characters', invertFilteredGroups: true } }).name, 'scene.create_collision_group');
  assert.throws(() => assistant.validate({ name: 'scene.create_collision_group', arguments: { path: '/World/Collider', filteredGroupPaths: ['/World/../Bad'] } }), /valid USD prim paths/);
});

test('assistant validates confirmed instance authoring', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('create an instance of /World/Source named Source_Instance'), { name: 'scene.create_instance', arguments: { sourcePath: '/World/Source', name: 'Source_Instance' } });
  assert.deepEqual(assistant.mock('create an instance of /World/Source with placement from /World/Duplicate named Source_Instance'), { name: 'scene.create_instance', arguments: { sourcePath: '/World/Source', name: 'Source_Instance', transformSourcePath: '/World/Duplicate' } });
  assert.deepEqual(assistant.mock('make an instance from this mesh'), { name: 'scene.create_instance', arguments: { sourcePath: '/World/Mesh', name: 'Mesh_Instance' } });
  assert.equal(assistant.validate({ name: 'scene.create_instance', arguments: { sourcePath: '/World/Mesh', name: 'Mesh_Instance' } }).confirm, true);
  assert.throws(() => assistant.validate({ name: 'scene.create_instance', arguments: { sourcePath: '/World/Mesh', name: 'bad/name' } }), /valid USD identifier/);
});

test('LOD sibling authoring clones the mesh declaration without replacing the source', async () => {
  const source = '#usda 1.0\n\ndef Xform "World"\n{\n    def Mesh "Mesh"\n    {\n        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n        int[] faceVertexIndices = [0, 1, 2]\n        int[] faceVertexCounts = [3]\n    }\n}\n';
  const fake = Object.create(LuciaUsdSession.prototype); fake.usda = source; fake.replaceUSDA = async function (next) { this.usda = next; return 'ignored'; };
  const previous = await fake.setMeshGeometrySibling('/World/Mesh', 'Mesh_LOD1', { positions: new Float32Array([0, 0, 0, .5, 0, 0, 0, .5, 0]), indices: new Uint32Array([0, 1, 2]) });
  assert.equal(previous, source); assert.match(fake.usda, /def Mesh "Mesh"/); assert.match(fake.usda, /def Mesh "Mesh_LOD1"/); assert.equal((fake.usda.match(/def Mesh/g) || []).length, 2);
});

test('USD session can author a stable child mesh without replacing its source', async () => {
  const source = '#usda 1.0\n\ndef Xform "World"\n{\n    def Mesh "Mesh"\n    {\n        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n        int[] faceVertexIndices = [0, 1, 2]\n        int[] faceVertexCounts = [3]\n    }\n}\n';
  const fake = Object.create(LuciaUsdSession.prototype); fake.usda = source; fake.replaceUSDA = async function (next) { this.usda = next; return 'ignored'; };
  const previous = await fake.setMeshGeometryChild('/World/Mesh', 'Part_1', { positions: new Float32Array([0, 0, 0, .5, 0, 0, 0, .5, 0]), indices: new Uint32Array([0, 1, 2]) });
  assert.equal(previous, source); assert.match(fake.usda, /def Mesh "Mesh"/); assert.match(fake.usda, /def Mesh "Part_1"/); assert.equal((fake.usda.match(/def Mesh/g) || []).length, 2); assert.match(fake.usda, /Part_1[\s\S]*0\.5/);
  const authored = fake.usda;
  await assert.rejects(() => fake.setMeshGeometryChild('/World/Mesh', 'Part_1', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) }), /already exists/);
  assert.equal(fake.usda, authored);
  await assert.rejects(() => fake.setMeshGeometryChild('/World/Mesh', 'bad/name', { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) }), /valid USD identifiers/);
  assert.equal(fake.usda, authored);
});

test('USD session can author an internal-reference instance sibling', async () => {
  const source = '#usda 1.0\n\ndef Xform "World"\n{\n    def Mesh "Mesh"\n    {\n        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n        int[] faceVertexIndices = [0, 1, 2]\n        int[] faceVertexCounts = [3]\n    }\n}\n';
  const fake = Object.create(LuciaUsdSession.prototype); fake.usda = source; fake.replaceUSDA = async function (next) { this.usda = next; return 'ignored'; };
  const previous = await fake.createInstanceSibling('/World/Mesh', 'Mesh_Instance');
  assert.equal(previous, source); assert.match(fake.usda, /def Mesh "Mesh_Instance"/); assert.match(fake.usda, /instanceable = true/); assert.match(fake.usda, /references = <\/World\/Mesh>/);
  await assert.rejects(() => fake.createInstanceSibling('/World/Mesh', 'Mesh_Instance'), /already exists/);
});

test('USD session can author an instance beside a root-level source prim', async () => {
  const source = '#usda 1.0\n\ndef Mesh "Mesh" {}\n';
  const fake = Object.create(LuciaUsdSession.prototype); fake.usda = source; fake.replaceUSDA = async function (next) { this.usda = next; return 'ignored'; };
  await fake.createInstanceSibling('/Mesh', 'Mesh_Instance');
  assert.match(fake.usda, /def Mesh "Mesh_Instance"/);
  assert.match(fake.usda, /references = <\/Mesh>/);
});

test('USD instance authoring copies only reviewed local transform operations', async () => {
  const source = '#usda 1.0\n\ndef Xform "World"\n{\n    def Mesh "Mesh" { }\n    def Mesh "Mesh_Dup" {\n        double3 xformOp:translate = (2, 0, 0)\n        uniform token[] xformOpOrder = ["xformOp:translate"]\n        point3f[] points = [(9, 9, 9)]\n    }\n}\n';
  const fake = Object.create(LuciaUsdSession.prototype); fake.usda = source; fake.replaceUSDA = async function (next) { this.usda = next; return 'ignored'; };
  await fake.createInstanceSibling('/World/Mesh', 'Mesh_Instance', 'Create instance', '/World/Mesh_Dup');
  assert.match(fake.usda, /def Mesh "Mesh_Instance" \([\s\S]*double3 xformOp:translate = \(2, 0, 0\)[\s\S]*xformOpOrder/);
  assert.doesNotMatch(fake.usda.slice(fake.usda.indexOf('def Mesh "Mesh_Instance"')), /points =/);
});

test('instance operation preserves the legacy progress-callback signature', async () => {
  const progress = [], operations = new LuciaOperations({ createInstanceSibling: async () => 'before' }, {}, {});
  assert.equal(await operations.createInstance('/World/Mesh', 'Mesh_Instance', (value) => progress.push(value.percentage)), 'before');
  assert.deepEqual(progress, [25, 100]);
});

test('mock assistant routes source UV transfer requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Low' });
  assert.deepEqual(assistant.mock('transfer UVs from /World/High'), { name: 'scene.transfer_uvs', arguments: { targetPath: '/World/Low', sourcePath: '/World/High' } });
  assert.deepEqual(assistant.mock('transfer lightmap UVs from /World/High'), { name: 'scene.transfer_uvs', arguments: { targetPath: '/World/Low', sourcePath: '/World/High', uvSet: 'lightmap' } });
});

test('assistant validates face-varying normal interpolation', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('recompute face-varying normals'), { name: 'scene.recompute_normals', arguments: { path: '/World/Mesh', weighting: 'area', smoothingAngle: 180, interpolation: 'faceVarying' } });
  assert.equal(assistant.validate({ name: 'scene.recompute_normals', arguments: { path: '/World/Mesh', interpolation: 'faceVarying' } }).confirm, true);
  assert.throws(() => assistant.validate({ name: 'scene.recompute_normals', arguments: { path: '/World/Mesh', interpolation: 'uniform' } }), /interpolation/);
});

test('mock assistant routes skin transfer requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Low' });
  assert.deepEqual(assistant.mock('transfer skin weights from /World/High'), { name: 'scene.transfer_skin_weights', arguments: { targetPath: '/World/Low', sourcePath: '/World/High' } });
});

test('mock assistant forwards cleanup preset requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('clean mesh with physics-ready preset'), { name: 'scene.cleanup_mesh', arguments: { path: '/World/Mesh', preset: 'physics-ready' } });
});

test('mock assistant routes connected component splitting', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('split the mesh into connected parts'), { name: 'scene.split_components', arguments: { path: '/World/Mesh', minFaces: 1 } });
  assert.deepEqual(assistant.mock('split components at crease angle 30'), { name: 'scene.split_components', arguments: { path: '/World/Mesh', minFaces: 1, splitAngle: 30 } });
  assert.deepEqual(assistant.mock('split concave components'), { name: 'scene.split_components', arguments: { path: '/World/Mesh', minFaces: 1, splitConcavity: true } });
});

test('mock assistant routes component segmentation evidence inspection', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('preview component boundary and curvature evidence'), { name: 'scene.inspect_component_segmentation', arguments: { path: '/World/Mesh', minFaces: 1 } });
});

test('mock assistant parses sharp-edge normal repair requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('recompute normals preserving sharp edge 0-2'), { name: 'scene.recompute_normals', arguments: { path: '/World/Mesh', weighting: 'area', smoothingAngle: 180, sharpEdges: [[0, 2]] } });
  assert.deepEqual(assistant.mock('recompute normals preserving crease 4:5'), { name: 'scene.recompute_normals', arguments: { path: '/World/Mesh', weighting: 'area', smoothingAngle: 180, sharpEdges: [[4, 5]] } });
  assert.throws(() => assistant.validate({ name: 'scene.recompute_normals', arguments: { sharpEdges: [[-1, 2]] } }), /non-negative integer/);
  assert.throws(() => assistant.validate({ name: 'scene.recompute_normals', arguments: { sharpEdges: [[2, 2]] } }), /distinct/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_shading', arguments: { normalSpace: 'world' } }), /normalSpace/);
  assert.equal(assistant.validate({ name: 'scene.bake_shading', arguments: { normalSpace: 'tangent' } }).name, 'scene.bake_shading');
  assert.throws(() => assistant.validate({ name: 'scene.bake_shading', arguments: { channel: 'height' } }), /channel/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_shading', arguments: { normalY: 'vulkan' } }), /normalY/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_shading', arguments: { resolution: 32 } }), /resolution/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_shading', arguments: { maxResolution: Infinity } }), /maximum dimension/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_shading', arguments: { dilation: 33 } }), /dilation/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_shading', arguments: { samples: 0 } }), /samples/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_shading', arguments: { radius: 0 } }), /radius/);
  assert.equal(assistant.validate({ name: 'scene.bake_shading', arguments: { resolution: 2048, maxResolution: 4096, dilation: 2, samples: 4, radius: 1 } }).name, 'scene.bake_shading');
  assert.throws(() => assistant.validate({ name: 'scene.bake_projection', arguments: { channel: 'height' } }), /channel/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_projection', arguments: { resolution: 8192 } }), /resolution/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_projection', arguments: { rayDistance: 0 } }), /distance/);
  assert.throws(() => assistant.validate({ name: 'scene.bake_projection', arguments: { cageOffset: -1 } }), /cage/);
});

test('mock assistant forwards normalized minimum face area requests', () => {
  const assistant = new LuciaAssistant({ getSelection: () => '/World/Mesh' });
  assert.deepEqual(assistant.mock('clean mesh with minimum face area 0.0001'), { name: 'scene.cleanup_mesh', arguments: { path: '/World/Mesh', minArea: 0.0001 } });
  assert.deepEqual(assistant.mock('clean mesh with minimum isolated component area 0.01'), { name: 'scene.cleanup_mesh', arguments: { path: '/World/Mesh', minComponentArea: 0.01 } });
  assert.deepEqual(assistant.mock('clean mesh with minimum closed component volume 0.01'), { name: 'scene.cleanup_mesh', arguments: { path: '/World/Mesh', minComponentVolume: 0.01 } });
});

test('mock assistant forwards normal bake convention and maximum dimension', () => {
  const assistant = new LuciaAssistant({ executeTool() {}, getSelection: () => '/World/Mesh', getSceneSummary: () => ({}), confirm() {} });
  assert.deepEqual(assistant.mock('bake a 2048px DirectX normal map with maximum dimension 1024'), { name: 'scene.bake_shading', arguments: { path: '/World/Mesh', resolution: 2048, maxResolution: 1024, normalY: 'directx', normalSpace: 'object', channel: 'normal' } });
  assert.equal(assistant.mock('bake a 512px tangent-space normal map').arguments.normalSpace, 'tangent');
  assert.equal(assistant.mock('bake a 512px object ID map').arguments.channel, 'objectID');
  assert.equal(assistant.mock('bake a 512px material id map').arguments.channel, 'materialID');
});

test('mock assistant maps high-to-low projection bake requests', () => {
  const assistant = new LuciaAssistant({ executeTool() {}, getSelection: () => '/World/Target', getSceneSummary: () => ({}), confirm() {} });
  assert.deepEqual(assistant.mock('bake a 512px roughness map from /World/High to /World/Target'), { name: 'scene.bake_projection', arguments: { sourcePath: '/World/High', targetPath: '/World/Target', resolution: 512, normalSpace: 'object', channel: 'roughness' } });
  assert.throws(() => assistant.validate({ name: 'scene.bake_projection', arguments: { sourcePath: '../High', targetPath: '/World/Target' } }), /Invalid USD path/);
});

test('assistant maps texture resize requests to the asset operation', () => {
  const assistant = new LuciaAssistant({ executeTool() {}, getSelection: () => '/World/Mesh', getSceneSummary: () => ({}), confirm() {} });
  assert.deepEqual(assistant.mock('pack channels R from textures/ao.png, G channel 1 from textures/roughness.png into textures/orm.png'), { name: 'scene.pack_channels', arguments: { name: 'textures/orm.png', channels: { r: { path: 'textures/ao.png' }, g: { path: 'textures/roughness.png', channel: 1 } } } });
  assert.equal(assistant.mock('pack channels R from textures/ao.png into textures/orm.png with 3 output channels').arguments.outputChannels, 3);
  assert.deepEqual(assistant.mock('resize texture textures/albedo.png to 2048'), { name: 'scene.resize_texture', arguments: { assetPath: 'textures/albedo.png', maxDimension: 2048 } });
  assert.throws(() => assistant.validate({ name: 'scene.resize_texture', arguments: { assetPath: '../private.png' } }), /asset path/);
  assert.throws(() => assistant.validate({ name: 'scene.resize_texture', arguments: { maxDimension: 0 } }), /dimension/);
  assert.throws(() => assistant.validate({ name: 'scene.resize_texture', arguments: { maxDimension: Infinity } }), /dimension/);
  assert.throws(() => assistant.validate({ name: 'scene.pack_channels', arguments: { outputChannels: 0 } }), /output channels/);
  assert.throws(() => assistant.validate({ name: 'scene.pack_channels', arguments: { outputChannels: 1, channels: { g: { path: 'ao.png' } } } }), /beyond the requested output/);
  assert.throws(() => assistant.validate({ name: 'scene.pack_channels', arguments: { colorSpace: 'display-p3' } }), /color space/);
  assert.throws(() => assistant.validate({ name: 'scene.pack_channels', arguments: { channels: { r: { path: 'a.png', channel: 4 } } } }), /source channels/);
  assert.throws(() => assistant.validate({ name: 'scene.pack_channels', arguments: { name: '../packed.png' } }), /package-relative/);
});

test('direct texture resize rejects invalid dimensions before browser decoding', async () => {
  const project = new LuciaProject();
  project.assets.set('textures/albedo.png', { bytes: new Uint8Array([1]) });
  const operations = new LuciaOperations({}, project, {});
  await assert.rejects(operations.resizeTexture('textures/albedo.png', 0), /dimension/);
  await assert.rejects(operations.resizeTexture('textures/albedo.png', 1.5), /dimension/);
});

test('texture rename rejects unsafe package paths', async () => {
  const session = { usda: '#usda 1.0\ndef Xform "World" { asset tex = @textures/albedo.png@ }', replaceUSDA: async (source) => { session.usda = source; return 'before'; } };
  const operations = new LuciaOperations(session, new LuciaProject(), {});
  await assert.rejects(operations.renameTexture('textures/albedo.png', 'textures\\albedo.png'), /safe package-relative/);
  await assert.rejects(operations.renameTexture('textures/albedo.png', 'textures//albedo.png'), /safe package-relative/);
  const assistant = new LuciaAssistant({ executeTool() {}, getSelection: () => '/World/Mesh' });
  assert.throws(() => assistant.validate({ name: 'scene.rename_texture', arguments: { oldPath: 'textures/albedo.png', newPath: 'textures\\albedo.png' } }), /Invalid texture path/);
});

test('channel packing uses LightUSD and records deterministic provenance', async () => {
  const project = new LuciaProject();
  project.assets.set('ao.png', { bytes: new Uint8Array([1, 2, 3]) });
  const session = { module: { LightUSDLoaderNative: class { repackChannels(options) { assert.equal(options.channels, 4); assert.equal(options.r.channel, 0); return { success: true, data: new Uint8Array([9, 8]), width: 2, height: 2, channels: 4 }; } delete() {} } }, exportUSDA: () => '#usda 1.0\n' };
  const operations = new LuciaOperations(session, project, {});
  await assert.rejects(operations.packChannels({ name: 'textures/bad.png', channels: { r: { path: 'ao.png', channel: 4 } } }), /source channels/);
  await assert.rejects(operations.packChannels({ name: 'textures/bad.png', colorSpace: 'display-p3', channels: { r: { path: 'ao.png', channel: 0 } } }), /color space/);
  await assert.rejects(operations.packChannels({ name: 'textures/bad.png', channels: null }), /r\/g\/b\/a/);
  await assert.rejects(operations.packChannels({ name: 'textures/bad.png', channels: { r: 42 } }), /source channels/);
  await assert.rejects(operations.packChannels({ name: 'textures//bad.png', channels: { r: { path: 'ao.png', channel: 0 } } }), /package-relative/);
  await assert.rejects(operations.packChannels({ name: 'textures/bad.png', outputChannels: 1, channels: { g: { path: 'ao.png', channel: 0 } } }), /beyond the requested output/);
  const result = await operations.packChannels({ name: 'textures/orm.png', channels: { r: { path: 'ao.png', channel: 0 } } });
  assert.equal(result, '#usda 1.0\n');
  assert.deepEqual(project.assets.get('textures/orm.png').packedSlots, { r: { path: 'ao.png', channel: 0 } });
  assert.equal(project.assets.get('textures/orm.png').dataTexture, true);
});

test('channel packing rejects malformed native result metadata before authoring an asset', async () => {
  const project = new LuciaProject();
  project.assets.set('ao.png', { bytes: new Uint8Array([1, 2, 3]) });
  const session = { module: { LightUSDLoaderNative: class { repackChannels() { return { success: true, data: new Uint8Array([9]), width: 0, height: 2, channels: 4 }; } delete() {} } }, exportUSDA: () => '#usda 1.0\n' };
  const operations = new LuciaOperations(session, project, {});
  await assert.rejects(operations.packChannels({ name: 'textures/orm.png', channels: { r: { path: 'ao.png', channel: 0 } } }), /invalid packed texture dimensions/);
  assert.equal(project.assets.has('textures/orm.png'), false);
  const floatSession = { module: { LightUSDLoaderNative: class { repackChannels() { return { success: true, data: new Float32Array([1]), width: 1, height: 1, channels: 4 }; } delete() {} } }, exportUSDA: () => '#usda 1.0\n' };
  await assert.rejects(new LuciaOperations(floatSession, project, {}).packChannels({ name: 'textures/float.png', channels: { r: { path: 'ao.png', channel: 0 } } }), /encoded byte data/);
  assert.equal(project.assets.has('textures/float.png'), false);
});

test('unused alpha stripping requires proof and preserves project metadata', async () => {
  const project = new LuciaProject();
  project.assets.set('baked.png', { bytes: new Uint8Array([1, 2, 3]), generated: true, bakeStats: { unusedAlpha: true }, colorSpace: 'srgb' });
  const session = { module: { LightUSDLoaderNative: class { repackChannels(options) { assert.equal(options.channels, 3); assert.equal(options.r.channel, 0); assert.equal(options.g.channel, 1); assert.equal(options.b.channel, 2); return { success: true, data: new Uint8Array([7]), width: 1, height: 1, channels: 3 }; } delete() {} } }, exportUSDA: () => '#usda 1.0\n' };
  const operations = new LuciaOperations(session, project, {});
  await operations.stripUnusedAlpha('baked.png');
  assert.equal(project.assets.get('baked.png').alphaRemoved, true);
  assert.equal(project.assets.get('baked.png').channels, 3);
  await assert.rejects(() => operations.stripUnusedAlpha('missing-proof.png'), /No image asset/);
});

test('dependency localization moves loaded assets and records export remaps', async () => {
  const project = new LuciaProject();
  project.assets.set('../source/albedo.png', { bytes: new Uint8Array([1, 2, 3]) });
  const session = { usda: '#usda 1.0\ndef Xform "World" { asset tex = @../source/albedo.png@ }', replaceUSDA: async (source) => { session.usda = source; return 'before'; } };
  const operations = new LuciaOperations(session, project, {});
  assert.equal(await operations.localizeDependencies({ '../source/albedo.png': 'textures/albedo.png' }), 'before');
  assert.equal(session.usda.includes('@textures/albedo.png@'), true);
  assert.equal(project.assets.has('../source/albedo.png'), false);
  assert.equal(project.assets.has('textures/albedo.png'), true);
  assert.equal(project.exportRemap['../source/albedo.png'], 'textures/albedo.png');
  await assert.rejects(() => operations.localizeDependencies({ 'missing.png': 'textures/albedo.png' }), /No exact USD asset references/);
});

test('dependency localization preserves chained asset moves atomically', async () => {
  const project = new LuciaProject();
  project.assets.set('a.png', { marker: 'a' }); project.assets.set('b.png', { marker: 'b' });
  const session = { usda: '#usda 1.0\ndef Xform "World" { asset a = @a.png@ asset b = @b.png@ }', replaceUSDA: async (source) => { session.usda = source; return ''; } };
  const operations = new LuciaOperations(session, project, {});
  await operations.localizeDependencies({ 'a.png': 'b.png', 'b.png': 'c.png' });
  assert.equal(project.assets.get('b.png').marker, 'a');
  assert.equal(project.assets.get('c.png').marker, 'b');
  assert.equal(project.assets.has('a.png'), false);
});

test('extract selected prim creates a package layer and same-path reference', async () => {
  const session = { usda: '#usda 1.0\ndef Xform "World" {\n    def Mesh "Hero" {\n        int[] faceVertexIndices = [0, 1, 2]\n    }\n}\n', replaceUSDA: async function (source) { this.usda = source; return source; } };
  const project = { assets: new Map() }, operations = new LuciaOperations(session, project, {});
  const result = await operations.extractSelectedToReference('/World/Hero', 'layers/hero.usda');
  assert.equal(result.assetPath, 'layers/hero.usda');
  assert.match(session.usda, /def Mesh "Hero" \( references = @layers\/hero\.usda@ \) \{ \}/);
  assert.match(new TextDecoder().decode(project.assets.get('layers/hero.usda').bytes), /defaultPrim = "Hero"/);
  assert.match(new TextDecoder().decode(project.assets.get('layers/hero.usda').bytes), /faceVertexIndices = \[0, 1, 2\]/);
  await assert.rejects(() => operations.extractSelectedToReference('/World/Hero', 'layers/hero.usda'), /already exists/);
  await assert.rejects(() => operations.extractSelectedToReference('/World/Hero', '../escape.usda'), /safe package-relative/);
});

test('extract selected nested subtree preserves quoted braces and children', async () => {
  const session = { usda: '#usda 1.0\ndef Xform "World" {\n    def Xform "Group" {\n        string note = "brace } remains text"\n        def Mesh "Child" { int[] faceVertexIndices = [0, 1, 2] }\n    }\n}\n', replaceUSDA: async function (source) { this.usda = source; return source; } };
  const project = { assets: new Map() }, operations = new LuciaOperations(session, project, {});
  await operations.extractSelectedToReference('/World/Group', 'layers/group.usda');
  assert.match(new TextDecoder().decode(project.assets.get('layers/group.usda').bytes), /brace \} remains text/);
  assert.match(new TextDecoder().decode(project.assets.get('layers/group.usda').bytes), /def Mesh "Child"/);
  assert.match(session.usda, /def Xform "Group" \( references = @layers\/group\.usda@ \) \{ \}/);
});

test('flatten selected package reference restores the USDA subtree', async () => {
  const layer = '#usda 1.0\ndef Mesh "Hero" {\n    string note = "restored"\n}\n';
  const session = { usda: '#usda 1.0\ndef Xform "World" {\n    def Mesh "Hero" ( references = @layers/hero.usda@ ) { }\n}\n', replaceUSDA: async function (source) { this.usda = source; return source; } };
  const project = { assets: new Map([['layers/hero.usda', { bytes: new TextEncoder().encode(layer) }]]) }, operations = new LuciaOperations(session, project, {});
  await operations.flattenSelectedReference('/World/Hero');
  assert.match(session.usda, /def Mesh "Hero" \{\n    string note = "restored"\n\}/);
  await assert.rejects(() => operations.flattenSelectedReference('/World/Hero'), /no package-local USDA reference/);
});

test('flatten selected reference rejects resolver and traversal paths', async () => {
  const session = { usda: '#usda 1.0\ndef Xform "World" {\n    def Mesh "Hero" ( references = @resolver://hero.usda@ ) { }\n}\n', replaceUSDA: async function (source) { this.usda = source; return source; } };
  const project = { assets: new Map([['resolver://hero.usda', { bytes: new TextEncoder().encode('#usda 1.0\ndef Mesh "Hero" {}') }]]) }, operations = new LuciaOperations(session, project, {});
  await assert.rejects(() => operations.flattenSelectedReference('/World/Hero'), /safe package-relative/);
});

test('unused alpha stripping restores package state through undo and redo', async () => {
  const project = new LuciaProject(), original = new Uint8Array([1, 2, 3]);
  project.assets.set('baked.png', { bytes: original, bakeStats: { unusedAlpha: true } });
  const session = { value: '#usda 1.0\n', module: { LightUSDLoaderNative: class { repackChannels() { return { success: true, data: new Uint8Array([7]), width: 1, height: 1, channels: 3 }; } delete() {} } }, exportUSDA: () => session.value, restore: (value) => { session.value = value; } };
  const operations = new LuciaOperations(session, project, {}), stack = new LuciaCommandStack();
  await stack.execute(sessionCommand(session, 'Remove unused alpha', [], () => operations.stripUnusedAlpha('baked.png'), project));
  assert.equal(project.assets.get('baked.png').alphaRemoved, true);
  await stack.undo();
  assert.deepEqual([...project.assets.get('baked.png').bytes], [...original]);
  assert.equal(project.assets.get('baked.png').alphaRemoved, undefined);
  await stack.redo();
  assert.equal(project.assets.get('baked.png').alphaRemoved, true);
});

test('tangent recompute returns an orthonormal frame with handedness', () => {
  const tangents = recomputeVertexTangents({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2]),
    uvs: new Float32Array([0, 0, 1, 0, 0, 1]),
    normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]),
  });

  assert.equal(tangents.length, 12);
  for (let i = 0; i < 3; i++) assert.deepEqual([...tangents.slice(i * 4, i * 4 + 4)], [1, 0, 0, 1]);
});

test('asset report scores unresolved material graph references', () => {
  const material = { uuid: 'mat-broken', name: 'Broken', type: 'MeshStandardMaterial', userData: { nodes: { output: { type: 'standard_surface', inputs: { surface: { node: 'missing' } } } } } };
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } }, material };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.deepEqual(report.materials[0].unresolvedReferences, ['output.surface → missing']);
  assert.equal(report.score.materials, 92);
  assert.ok(report.issues.some((item) => item.ruleId === 'material.unresolvedNodeReference'));
});

test('safe mesh cleanup removes bad faces and preserves aligned attributes', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 5, 5, 5]),
    indices: new Uint32Array([0, 1, 2, 2, 1, 0, 0, 0, 3]),
    normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0]),
    uvs: new Float32Array([0, 0, 1, 0, 0, 1, .5, .5]),
    colors: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, .5, .5, .5]),
    tangents: new Float32Array([1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, -1, 0, 0, 1]),
    jointIndices: new Uint16Array([0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0]),
    jointWeights: new Float32Array([1, 0, 0, 0, .75, .25, 0, 0, .5, .5, 0, 0, 1, 0, 0, 0]),
    customAttributes: [{ name: 'temperature', itemSize: 1, array: new Float32Array([10, 20, 30, 40]) }],
  });
  assert.equal(result.beforeTriangles, 3);
  assert.equal(result.afterTriangles, 1);
  assert.equal(result.beforeVertices, 4);
  assert.equal(result.afterVertices, 3);
  assert.deepEqual([...result.indices], [0, 1, 2]);
  assert.deepEqual([...result.normals], [0, 0, 1, 0, 0, 1, 0, 0, 1]);
  assert.deepEqual([...result.uvs], [0, 0, 1, 0, 0, 1]);
  assert.deepEqual([...result.colors], [1, 0, 0, 0, 1, 0, 0, 0, 1]);
  assert.deepEqual([...result.tangents], [1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1]);
  assert.deepEqual([...result.jointIndices], [0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0]);
  assert.deepEqual([...result.jointWeights], [1, 0, 0, 0, .75, .25, 0, 0, .5, .5, 0, 0]);
  assert.deepEqual([...result.customAttributes[0].array], [10, 20, 30]);
  const second = cleanupMesh(result);
  assert.equal(second.afterTriangles, result.afterTriangles);
  assert.equal(second.afterVertices, result.afterVertices);
  assert.deepEqual([...second.indices], [...result.indices]);
});

test('safe mesh cleanup indexes non-indexed input and drops isolated vertices', () => {
  const result = cleanupMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 2, 2]) });
  assert.equal(result.beforeTriangles, 1);
  assert.equal(result.afterTriangles, 1);
  assert.equal(result.afterVertices, 3);
  assert.deepEqual([...result.indices], [0, 1, 2]);
});

test('safe mesh cleanup rejects joint data that would be lossy or unsafe to compact', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) };
  assert.throws(() => cleanupMesh({ ...mesh, jointIndices: new Uint16Array([0, 0, 0, 0]) }), /four influences/);
  assert.throws(() => cleanupMesh({ ...mesh, jointIndices: new Float32Array([0, 1.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) }), /joint indices/);
  assert.throws(() => cleanupMesh({ ...mesh, jointWeights: new Float32Array([1, -0.1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]) }), /joint weights/);
});

test('safe mesh cleanup rejects malformed optional aligned attributes', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) };
  assert.throws(() => cleanupMesh({ ...mesh, normals: new Float32Array([0, 0, 1]) }), /normals.*vertex-aligned/);
  assert.throws(() => cleanupMesh({ ...mesh, customAttributes: [{ name: 'bad', itemSize: 1, array: new Float32Array([Infinity, 1, 2]) }] }), /custom attributes.*finite/);
  assert.throws(() => cleanupMesh({ ...mesh, faceVaryingAttributes: [{ name: 'st', itemSize: 2, array: new Float32Array([0, 0, 1, 0]), indices: new Uint32Array([0]) }] }), /face-varying.*one valid index/);
  assert.throws(() => cleanupMesh({ ...mesh, customAttributes: {} }), /custom attributes must be an array/);
  assert.throws(() => cleanupMesh({ ...mesh, faceVaryingAttributes: {} }), /face-varying attributes must be an array/);
});

test('safe mesh cleanup accepts plain arrays for remapped attributes', () => {
  const result = cleanupMesh({
    positions: [0, 0, 0, 1, 0, 0, 0, 1, 0],
    indices: [0, 1, 2],
    customAttributes: [{ name: 'temperature', itemSize: 1, array: [1, 2, 3] }],
    faceVaryingAttributes: [{ name: 'cornerValue', itemSize: 1, array: [2, 3, 4], indices: [0, 1, 2] }],
  });
  assert.deepEqual([...result.customAttributes[0].array], [1, 2, 3]);
  assert.deepEqual([...result.faceVaryingAttributes[0].array], [2, 3, 4]);
});

test('connected component extraction preserves material ranges and filters small parts', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]);
  const result = extractConnectedComponents({ positions, indices: new Uint32Array([0, 1, 2, 3, 4, 5]), attributes: [{ name: 'uvs', itemSize: 2, array: new Float32Array([0, 0, 1, 0, 0, 1, .5, .5, 1, .5, .5, 1]) }], groups: [{ start: 0, count: 3, materialIndex: 2 }, { start: 3, count: 3, materialIndex: 4 }] });
  assert.equal(result.length, 2);
  assert.deepEqual([...result[0].indices], [0, 1, 2]);
  assert.deepEqual(result[0].groups, [{ start: 0, count: 3, materialIndex: 2 }]);
  assert.deepEqual(result[1].groups, [{ start: 0, count: 3, materialIndex: 4 }]);
  assert.deepEqual([...result[0].attributes[0].array], [0, 0, 1, 0, 0, 1]);
  assert.deepEqual([...result[1].attributes[0].array], [.5, .5, 1, .5, .5, 1]);
  assert.equal(extractConnectedComponents({ positions, indices: new Uint32Array([0, 1, 2, 3, 4, 5]), minFaces: 2 }).length, 0);
  assert.equal(extractConnectedComponents({ positions: [...positions], indices: [0, 1, 2, 3, 4, 5] }).length, 2);
  assert.throws(() => extractConnectedComponents({ positions, indices: new Uint32Array([0, 1, 2, 3, 4, 5]), faceVaryingAttributes: {} }), /face-varying attributes must be an array/);
});

test('component extraction can split sharp dihedral boundaries deterministically', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), indices = new Uint32Array([0, 1, 2, 1, 0, 3]);
  const connected = extractConnectedComponents({ positions, indices });
  assert.equal(connected.length, 1); assert.equal(connected[0].curvatureEdges, 1); assert.equal(connected[0].maxDihedralDegrees, 90);
  const split = extractConnectedComponents({ positions, indices, splitAngle: 45 });
  assert.equal(split.length, 2);
  assert.deepEqual(split.map((component) => component.sourceFaces), [[0], [1]]);
  assert.deepEqual(split.map((component) => ({ boundaryEdges: component.boundaryEdges, nearlyPlanar: component.nearlyPlanar })), [{ boundaryEdges: 3, nearlyPlanar: true }, { boundaryEdges: 3, nearlyPlanar: true }]);
  assert.throws(() => extractConnectedComponents({ positions, indices, splitAngle: 181 }), /between 0 and 180/);
});

test('component extraction classifies concavity only for closed manifold components', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), indices = new Uint32Array([0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]);
  const tetrahedron = extractConnectedComponents({ positions, indices })[0];
  assert.equal(tetrahedron.concavityReliable, true); assert.equal(tetrahedron.concaveEdges, 0); assert.equal(tetrahedron.convexEdges, 6);
  const open = extractConnectedComponents({ positions, indices: new Uint32Array([0, 2, 1]) })[0];
  assert.equal(open.concavityReliable, false); assert.equal(open.concaveEdges, 0); assert.equal(open.convexEdges, 0);
  assert.equal(open.boundaryLoops, 1); assert.equal(open.planarBoundaryLoops, 1);
  assert.deepEqual(open.planarBoundaryLoopVertices, [[0, 1, 2]]);
  assert.deepEqual(open.planarBoundaryGroups, [[0]]);
  assert.equal(extractConnectedComponents({ positions, indices, splitConcavity: true }).length, 1);
  assert.equal(extractConnectedComponents({ positions, indices: new Uint32Array([0, 2, 1]), splitConcavity: true }).length, 1);
  assert.throws(() => extractConnectedComponents({ positions, indices, splitConcavity: 'yes' }), /must be boolean/);
  const polygon = [[0, 0], [2, 0], [2, 1], [1, 1], [1, 2], [0, 2]], prismPositions = new Float32Array(polygon.flatMap(([x, y]) => [x, y, 0]).concat(polygon.flatMap(([x, y]) => [x, y, 1]))), prismIndices = [];
  for (const face of [[0, 2, 1], [0, 3, 2], [0, 4, 3], [0, 5, 4], [6, 7, 8], [6, 8, 9], [6, 9, 10], [6, 10, 11]]) prismIndices.push(...face);
  for (let edge = 0; edge < 6; edge++) { const a = edge, b = (edge + 1) % 6, ap = a + 6, bp = b + 6; prismIndices.push(a, b, bp, a, bp, ap); }
  const concavePrism = extractConnectedComponents({ positions: prismPositions, indices: Uint32Array.from(prismIndices), splitConcavity: true });
  assert.equal(concavePrism.length, 1); assert.equal(concavePrism[0].concavityReliable, true); assert.ok(concavePrism[0].concaveEdges > 0);
  const degenerate = extractConnectedComponents({ positions, indices: new Uint32Array([0, 0, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]), splitConcavity: true });
  assert.equal(degenerate[0].concavityReliable, false);
  const degenerateOnly = extractConnectedComponents({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 0, 1]) })[0];
  assert.equal(degenerateOnly.nearlyPlanar, false);
});

test('component preview exposes stable face labels and filtered sentinels', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]);
  const preview = previewConnectedComponents({ positions, indices: new Uint32Array([0, 1, 2, 3, 4, 5]), groups: [{ start: 0, count: 3, materialIndex: 2 }, { start: 3, count: 3, materialIndex: 4 }] });
  assert.deepEqual([...preview.faceLabels], [0, 1]);
  assert.deepEqual(preview.components.map(({ id, faces, triangleCount, vertexCount, materialIndices }) => ({ id, faces, triangleCount, vertexCount, materialIndices })), [{ id: 0, faces: [0], triangleCount: 1, vertexCount: 3, materialIndices: [2] }, { id: 1, faces: [1], triangleCount: 1, vertexCount: 3, materialIndices: [4] }]);
  assert.deepEqual([...previewConnectedComponents({ positions, indices: new Uint32Array([0, 1, 2, 3, 4, 5]), minFaces: 2 }).faceLabels], [-1, -1]);
  assert.throws(() => previewConnectedComponents({ positions, indices: [0, 1] }), /complete indexed triangle geometry/);
});

test('component corrections deterministically merge and discard preview IDs', () => {
  const preview = { faceLabels: Int32Array.from([0, 1, 2, 1]), components: [{ id: 0, faces: [0], triangleCount: 1, vertexCount: 3, materialIndices: [2] }, { id: 1, faces: [1, 3], triangleCount: 2, vertexCount: 4, materialIndices: [4] }, { id: 2, faces: [2], triangleCount: 1, vertexCount: 3, materialIndices: [5] }] };
  const corrected = correctComponentPreview(preview, { mergeGroups: [[0, 2]], discard: [1] });
  assert.deepEqual([...corrected.faceLabels], [0, -1, 0, -1]);
  assert.deepEqual(corrected.discardedIds, [1]);
  assert.deepEqual(corrected.mergedGroups, [[0, 2]]);
  assert.equal(corrected.changed, true);
  assert.deepEqual(corrected.components, [{ id: 0, sourceIds: [0, 2], faces: [0, 2], triangleCount: 2, vertexCount: 6, materialIndices: [2, 5] }]);
  assert.throws(() => correctComponentPreview(preview, { mergeGroups: [[0, 1], [1, 2]] }), /must not overlap/);
  assert.throws(() => correctComponentPreview({ faceLabels: { length: 1.5 }, components: [] }), /valid preview/);
});

test('connected component extraction compacts face-varying primvars per component', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]);
  const indices = new Uint32Array([0, 1, 2, 3, 4, 5]);
  const uv = { name: 'st', itemSize: 2, array: new Float32Array([0, 0, 1, 0, 0, 1, .2, .2, .8, .2, .2, .8]), indices: new Uint32Array([0, 1, 2, 3, 4, 5]), interpolation: 'faceVarying' };
  const parts = extractConnectedComponents({ positions, indices, faceVaryingAttributes: [uv] });
  assert.equal(parts.length, 2);
  assert.deepEqual([...parts[0].faceVaryingAttributes[0].array], [0, 0, 1, 0, 0, 1]);
  assert.deepEqual([...parts[1].faceVaryingAttributes[0].indices], [0, 1, 2]);
  const plainParts = extractConnectedComponents({ positions: [...positions], indices: [...indices], faceVaryingAttributes: [{ ...uv, array: [...uv.array], indices: [...uv.indices] }] });
  assert.deepEqual([...plainParts[1].faceVaryingAttributes[0].array], [...new Float32Array([.2, .2, .8, .2, .2, .8])]);
});

test('safe mesh cleanup preserves face-varying primvar indices', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2, 0, 2, 3]),
    faceVaryingAttributes: [{ name: 'st', itemSize: 2, array: new Float32Array([0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1]), indices: new Uint32Array([0, 1, 2, 3, 4, 5]) }],
  });
  assert.equal(result.afterTriangles, 2);
  assert.equal(result.faceVaryingAttributes.length, 1);
  assert.deepEqual([...result.faceVaryingAttributes[0].indices], [0, 1, 2, 3, 4, 5]);
  assert.deepEqual([...result.faceVaryingAttributes[0].array], [0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1]);
});

test('safe mesh cleanup preserves explicit face-varying UV buffers while welding positions', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 4, 5]),
    uvIndices: new Uint32Array([0, 1, 2, 3, 4, 5]),
    uvs: new Float32Array([0, 0, 1, 0, 0, 1, .5, .5, .25, .25, .75, .75]),
    tolerance: 0,
  });
  assert.equal(result.afterVertices, 4);
  assert.deepEqual([...result.uvIndices], [0, 1, 2, 3, 4, 5]);
  assert.deepEqual([...result.uvs], [0, 0, 1, 0, 0, 1, .5, .5, .75, .75, .25, .25]);
});

test('safe mesh cleanup welds exact duplicates without crossing UV seams', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 1, 2]),
    normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]),
    uvs: new Float32Array([0, 0, 1, 0, 0, 1, .5, .5]),
  });
  assert.equal(result.afterTriangles, 2);
  assert.equal(result.afterVertices, 4);
  const welded = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 1, 2]),
    normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]),
    uvs: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0]),
  });
  assert.equal(welded.afterVertices, 3);
});

test('safe mesh cleanup remaps authored crease edges and weights', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 10, 10]),
    indices: new Uint32Array([0, 1, 2]),
    sharpEdges: [[0, 1], [1, 3]],
    sharpEdgeSharpness: [0.5, 0.8],
  });
  assert.deepEqual([...result.sharpEdges], [[0, 1]]);
  assert.deepEqual(result.sharpEdgeSharpness, [0.5]);
  assert.throws(() => cleanupMesh({ ...result, sharpEdgeSharpness: [0] }), /positive finite/);
});

test('mesh cleanup result validation checks remapped crease metadata', () => {
  const result = cleanupMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), sharpEdges: [[0, 1]] });
  assert.deepEqual(validateMeshCleanupResult(result).sharpEdges, [[0, 1]]);
  assert.throws(() => validateMeshCleanupResult({ ...result, sharpEdgeSharpness: [0] }), /positive finite sharpness/);
});

test('safe mesh cleanup uses mesh scale for tolerance welding', () => {
  const result = cleanupMesh({
    tolerance: 0.001,
    positions: new Float32Array([0, 0, 0, 1000, 0, 0, 0, 1000, 0, .4, 0, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 1, 2]),
  });
  assert.equal(result.afterVertices, 3);
});

test('safe mesh cleanup welds across tolerance bucket boundaries', () => {
  const result = cleanupMesh({
    tolerance: 0.1,
    positions: new Float32Array([.047, 0, 0, 1, 0, 0, .047, 1, 0, .049, 0, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 1, 2]),
  });
  assert.equal(result.afterVertices, 3);
  assert.equal(result.afterTriangles, 1);
});

test('crack-merge preview reports component reduction before authoring', () => {
  const preview = previewCrackMerge({
    tolerance: 0.1,
    positions: new Float32Array([.047, 0, 0, 1, 0, 0, .047, 1, 0, .049, 0, 0, 1.001, 0, 0, .049, 1.001, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 4, 5]),
  });
  assert.equal(preview.beforeComponents, 2);
  assert.equal(preview.afterComponents, 1);
  assert.equal(preview.mergedComponents, 1);
  assert.equal(preview.changed, true);
  assert.throws(() => previewCrackMerge({ positions: new Float32Array([0, 0, 0]), indices: new Uint32Array([0, 0, 0]) }, { tolerance: 0 }), /positive finite tolerance/);
});

test('safe mesh cleanup does not weld diagonal vertices beyond tolerance', () => {
  const result = cleanupMesh({
    tolerance: 0.1,
    positions: new Float32Array([.46, .46, 0, 1.46, .46, 0, .46, 1.46, 0, .54, .54, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 1, 2]),
  });
  assert.equal(result.afterVertices, 4);
  assert.equal(result.afterTriangles, 2);
});

test('safe mesh cleanup does not over-weld tiny meshes', () => {
  const result = cleanupMesh({
    tolerance: 0.001,
    positions: new Float32Array([0, 0, 0, 0.001, 0, 0, 0, 0.001, 0, 0.0004, 0, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 1, 2]),
  });
  assert.equal(result.afterVertices, 4);
  assert.throws(() => cleanupMesh({ tolerance: Infinity, positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) }), /finite and non-negative/);
  assert.throws(() => cleanupMesh({ tolerance: -1, positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) }), /finite and non-negative/);
});

test('safe mesh cleanup honors scale-normalized minimum face area', () => {
  const result = cleanupMesh({
    minArea: 0.01,
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0.01, 0, 0]),
    indices: new Uint32Array([0, 1, 2, 0, 3, 2]),
  });
  assert.equal(result.afterTriangles, 1);
});

test('safe mesh cleanup can drop isolated components below area threshold', () => {
  const result = cleanupMesh({
    minComponentArea: 0.01,
    positions: new Float32Array([0, 0, 0, 10, 0, 0, 0, 10, 0, 1, 1, 0, 1.1, 1, 0, 1, 1.1, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 4, 5]),
    groups: [{ start: 0, count: 3, materialIndex: 0 }, { start: 3, count: 3, materialIndex: 1 }],
  });
  assert.equal(result.afterTriangles, 1);
  assert.deepEqual(result.groups, [{ start: 0, count: 3, materialIndex: 0 }]);
});

test('safe mesh cleanup group normalization rejects uncovered index ranges', () => {
  assert.throws(() => consolidateMaterialGroups([{ start: 3, count: 3, materialIndex: 0 }], 6), /complete ordered index buffer/);
});

test('safe mesh cleanup filters only closed components below volume threshold', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]);
  const indices = new Uint32Array([0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]);
  assert.equal(cleanupMesh({ positions, indices, minComponentVolume: 0.2 }).afterTriangles, 0);
  assert.equal(cleanupMesh({ positions, indices, minComponentVolume: 0.1 }).afterTriangles, 4);
  assert.equal(cleanupMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), minComponentVolume: 1 }).afterTriangles, 1);
});

test('safe mesh cleanup normalizes winding across manifold edges', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0]),
    indices: new Uint32Array([0, 1, 2, 1, 2, 3]),
  });
  assert.deepEqual([...result.indices], [0, 1, 2, 1, 3, 2]);
});

test('safe mesh cleanup optionally fills a simple planar boundary loop', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]),
    indices: new Uint32Array([0, 2, 1, 0, 1, 3, 0, 3, 2]),
    fillPlanarHoles: true,
  });
  assert.equal(result.beforeTriangles, 3);
  assert.equal(result.afterTriangles, 4);
  assert.equal(result.filledHoles, 1);
  assert.equal(new Set(result.indices.slice(-3)).size, 3);
  assert.deepEqual(previewPlanarHoleFill({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), indices: new Uint32Array([0, 2, 1, 0, 1, 3, 0, 3, 2]) }), { filledHoles: 1, beforeTriangles: 3, retainedTriangles: 3, removedTriangles: 0, afterTriangles: 4, addedTriangles: 1, proposedTriangles: [[3, 2, 1]] });
});

test('safe mesh cleanup rebuilds topology before filtering filled components', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]);
  const indices = new Uint32Array([0, 2, 1, 0, 1, 3, 0, 3, 2]);
  const result = cleanupMesh({ positions, indices, fillPlanarHoles: true, minComponentVolume: 0.1 });
  assert.equal(result.filledHoles, 1);
  assert.equal(result.afterTriangles, 4);
  assert.equal(result.removedTriangles, 0);
});

test('safe mesh cleanup reports post-filter triangle accounting', () => {
  const result = cleanupMesh({
    positions: new Float32Array([0, 0, 0, 10, 0, 0, 0, 10, 0, 20, 0, 0, 20.1, 0, 0, 20, .1, 0]),
    indices: new Uint32Array([0, 1, 2, 3, 4, 5]),
    minComponentArea: .001,
  });
  assert.equal(result.beforeTriangles, 2);
  assert.equal(result.retainedTriangles, 1);
  assert.equal(result.removedTriangles, 1);
  assert.equal(result.afterTriangles, 1);
});

test('USD Doctor reports deterministic portability issues', () => {
  const report = diagnoseUSD('#usda 1.0\n(\n defaultPrim = "Missing"\n metersPerUnit = 0\n upAxis = "X"\n)\ndef Xform "World" {}\n asset file = @textures/missing.png@');
  assert.deepEqual(report.issues.map((item) => item.ruleId), ['usd.invalidDefaultPrim', 'usd.invalidMetersPerUnit', 'usd.invalidUpAxis', 'usd.unresolvedAsset']);
  assert.equal(report.score, 17);
  assert.equal(report.dependencies[0].kind, 'asset');
});

test('USD Doctor validates defaultPrim against root prims only', () => {
  const source = '#usda 1.0\n( defaultPrim = "Nested" )\ndef Xform "World" {\n    def Xform "Nested" {}\n}\n';
  const report = diagnoseUSD(source);
  assert.deepEqual(report.rootPrims, ['World']);
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.invalidDefaultPrim'));
});

test('USD Doctor discovers root prims with declaration metadata clauses', () => {
  const report = diagnoseUSD('#usda 1.0\n( defaultPrim = "World" )\ndef Xform "World" ( kind = "model" ) {}\n');
  assert.deepEqual(report.rootPrims, ['World']);
  assert.equal(report.defaultPrim, 'World');
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.invalidDefaultPrim'), false);
});

test('USD Doctor does not treat prim properties as layer metadata', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { string defaultPrim = "NotTheRoot" string upAxis = "X" double metersPerUnit = 0 }\n');
  assert.equal(report.defaultPrim, null);
  assert.equal(report.upAxis, null);
  assert.equal(report.metersPerUnit, null);
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.missingDefaultPrim'));
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.missingUpAxis'));
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.missingMetersPerUnit'));
});

test('USD Doctor scopes kind and purpose diagnostics to prim metadata', () => {
  const report = diagnoseUSD('#usda 1.0\n( kind = "hero" purpose = "shadow" upAxis = "Y" metersPerUnit = 1 )\ndef Xform "World" { kind = "model" purpose = "render" }\n');
  assert.equal(report.kindValues.includes('hero'), false);
  assert.equal(report.purposeValues.includes('shadow'), false);
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.invalidKind'), false);
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.invalidPurpose'), false);
});

test('USD Doctor metadata repair preserves parentheses inside metadata strings', () => {
  const source = '#usda 1.0\n(\n string note = ")"\n)\ndef Xform "World" {}\n';
  const repaired = repairUSDMetadata(source, { upAxis: 'Y' });
  assert.ok(repaired.includes('string note = ")"'));
  assert.match(repaired, /upAxis = "Y"/);
  assert.equal((repaired.match(/\n\)\n/g) || []).length, 1);
});

test('asset report uses mesh scale for tiny-triangle detection', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 6, array: new Float32Array([0, 0, 0, 1000, 0, 0, 0, 1000, 0, 0, 0, 0, .001, 0, 0, 0, .001, 0]) } }, index: { count: 6, array: new Uint32Array([0, 1, 2, 3, 4, 5]) } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.meshes[0].tinyFaces, 1);
  assert.ok(report.issues.some((item) => item.ruleId === 'geom.tinyFace'));
});

test('asset report flags out-of-range and negative indices', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 3, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]) } }, index: { count: 6, array: new Int32Array([0, 1, 9, -1, 1, 2]) } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.equal(report.stage.invalidIndices, 2);
  assert.ok(report.issues.some((item) => item.ruleId === 'topology.invalidIndex' && item.severity === 'error'));
});

test('asset report detects coarse UV overlap', () => {
  const mesh = { isMesh: true, name: 'Mesh', geometry: { attributes: { position: { count: 6, array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1]) }, uv: { count: 6, array: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]) } }, index: { count: 6, array: new Uint32Array([0, 1, 2, 3, 4, 5]) } } };
  const report = analyzeAsset({ traverse(callback) { callback(mesh); } });
  assert.ok(report.stage.uvOverlap > 0);
  assert.equal(report.stage.uvIslands, 2);
  assert.equal(report.stage.uvOverlapGrid.length, 64 * 64);
  assert.ok(report.issues.some((item) => item.ruleId === 'uv.overlap'));
});

test('asset report detects identical mesh geometry suitable for instancing', () => {
  const makeMesh = (name, offset = 0) => ({ isMesh: true, name, geometry: { attributes: { position: { count: 3, array: new Float32Array([0 + offset, 0, 0, 1 + offset, 0, 0, 0 + offset, 1, 0]) } }, index: { count: 3, array: new Uint16Array([0, 1, 2]) } } });
  const first = makeMesh('A'), duplicate = makeMesh('B'), different = makeMesh('C', 2);
  const report = analyzeAsset({ traverse(callback) { callback(first); callback(duplicate); callback(different); } });
  assert.equal(report.stage.duplicateGeometries, 1);
  assert.equal(report.meshes.find((mesh) => mesh.path === 'B').duplicateOf, 'A');
  assert.ok(report.issues.some((item) => item.ruleId === 'geom.duplicate' && item.path === 'B'));
  assert.equal(report.meshes.find((mesh) => mesh.path === 'B').materialCompatible, true);
  assert.equal(report.meshes.find((mesh) => mesh.path === 'B').transformCompatible, true);
});

test('asset report canonicalizes duplicate geometry across vertex and triangle order', () => {
  const first = { isMesh: true, name: 'A', geometry: { attributes: { position: { count: 4, array: new Float32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]) } }, index: { count: 6, array: new Uint16Array([0, 1, 2, 0, 2, 3]) } } };
  const duplicate = { isMesh: true, name: 'B', geometry: { attributes: { position: { count: 4, array: new Float32Array([1, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0]) } }, index: { count: 6, array: new Uint16Array([2, 3, 0, 2, 0, 1]) } } };
  const report = analyzeAsset({ traverse(callback) { callback(first); callback(duplicate); } });
  assert.equal(report.stage.duplicateGeometries, 1);
  assert.equal(report.meshes[1].duplicateOf, 'A');
});

test('asset report uses bounded exact matching for large duplicate geometry', () => {
  const positions = new Float32Array(100001 * 3);
  positions.set([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const makeMesh = (name) => ({ isMesh: true, name, geometry: { attributes: { position: { count: 100001, array: new Float32Array(positions) } }, index: { count: 3, array: new Uint32Array([0, 1, 2]) } } });
  const report = analyzeAsset({ traverse(callback) { callback(makeMesh('A')); callback(makeMesh('B')); } });
  assert.equal(report.stage.duplicateGeometries, 1);
  assert.equal(report.meshes[1].duplicateOf, 'A');
});

test('USD Doctor classifies available composition dependencies', () => {
  const source = '#usda 1.0\n( defaultPrim = "World" )\ndef Xform "World" { references = @base.usda@ payload = @payload.usdc@ subLayers = [@layer.usda@] }';
  const asset = { bytes: new Uint8Array([1]) };
  const report = diagnoseUSD(source, new Map([['base.usda', asset], ['payload.usdc', asset], ['layer.usda', asset]]));
  assert.deepEqual(report.dependencies.map((item) => item.kind), ['references', 'sublayers', 'payload']);
  assert.deepEqual(report.dependencies.map((item) => item.status), ['available', 'available', 'available']);
  assert.deepEqual(report.dependencyGraph.edges.map((edge) => edge.to), ['asset:base.usda', 'asset:layer.usda', 'asset:payload.usdc']);
  assert.equal(report.dependencyGraph.nodes.length, 4);
  assert.deepEqual(report.issues.map((item) => item.ruleId), ['usd.missingMetersPerUnit', 'usd.missingUpAxis', 'usdz.duplicateAsset', 'usdz.duplicateAsset']);
});

test('USD Doctor accepts plain package asset maps at the API boundary', () => {
  const source = '#usda 1.0\n( defaultPrim = "World" )\ndef Xform "World" { references = @base.usda@ }';
  const report = diagnoseUSD(source, { 'base.usda': { bytes: new Uint8Array([1]) } });
  assert.equal(report.dependencies[0].status, 'available');
  assert.equal(report.packageAssets[0].referenced, true);
});

test('USD Doctor ignores malformed Map package-member keys', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" {}', new Map([[4, { bytes: new Uint8Array([1]) }], ['ok.png', { bytes: new Uint8Array([1]) }]]));
  assert.deepEqual(report.packageAssets.map((asset) => asset.path), ['ok.png']);
});

test('USD Doctor tolerates malformed package-member byte payloads', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" {}', new Map([['a.png', { bytes: {} }], ['b.png', { bytes: {} }]]));
  assert.equal(report.packageAssets.length, 2);
  assert.ok(report.issues.some((item) => item.ruleId === 'usdz.emptyAsset'));
});

test('USD Doctor creates a deterministic dependency manifest', () => {
  const report = diagnoseUSD('#usda 1.0\n( defaultPrim = "World" metersPerUnit = 1 upAxis = "Y" )\ndef Xform "World" { references = @base.usda@ }', new Map([['base.usda', { bytes: new Uint8Array([1]) }]]));
  assert.deepEqual(createDependencyManifest(report, 'model.usda'), {
    schemaVersion: 1,
    source: 'model.usda',
    format: 'USDA 1.0',
    metadata: { defaultPrim: 'World', upAxis: 'Y', metersPerUnit: 1 },
    dependencies: [{ path: 'base.usda', kind: 'references', present: true, status: 'available' }],
    dependencyStatusCounts: { available: 1, missing: 0, unsafe: 0, resolver: 0, anonymous: 0 },
    packageAssets: [{ path: 'base.usda', bytes: 1, referenced: true }],
    dependencyGraph: {
      nodes: [{ id: 'asset:base.usda', kind: 'references', path: 'base.usda', present: true, status: 'available' }, { id: 'root', kind: 'layer', path: null, present: true, status: 'available' }],
    edges: [{ from: 'root', to: 'asset:base.usda', kind: 'references', status: 'available' }],
    },
    issueCount: 0,
    score: 100,
  });
});

test('USD Doctor manifest preserves non-automatic material binding review', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { rel material:binding:collection:render = </World/Collections/Render> }');
  assert.deepEqual(createDependencyManifest(report).materialBindingReview, [{ kind: 'collection', authored: 'material:binding:collection:render', path: '/World', automaticFixSafe: false, reason: 'Requires composed collection membership evaluation.' }]);
  assert.deepEqual(createDependencyManifest({ materialBindingReview: [{ kind: 'collection', authored: ' x\n\t', reason: '\u0000review' }] }).materialBindingReview, [{ kind: 'collection', authored: 'x', path: '/', automaticFixSafe: false, reason: 'review' }]);
});

test('USD Doctor manifests preserve inferred inherited binding targets', () => {
  const manifest = createDependencyManifest({ materialBindingReview: [{ kind: 'inferred-inheritance', authored: 'inherits /World/Base', path: '/World/M', materialPath: '/World/Looks/Mat', sourcePath: '/World/Base', reason: 'review' }] });
  assert.deepEqual(manifest.materialBindingReview, [{ kind: 'inferred-inheritance', authored: 'inherits /World/Base', path: '/World/M', automaticFixSafe: false, reason: 'review', materialPath: '/World/Looks/Mat', sourcePath: '/World/Base' }]);
});

test('USD Doctor manifest serialization tolerates malformed report collections', () => {
  const manifest = createDependencyManifest({ dependencies: { invalid: true }, packageAssets: [null, { path: 4 }, { path: 'ok.png', bytes: 'invalid', referenced: 'yes' }], dependencyGraph: { nodes: [null, { path: 4 }], edges: 'invalid' }, issues: null, score: 'invalid' });
  assert.deepEqual(manifest.dependencies, []);
  assert.deepEqual(manifest.packageAssets, [{ path: 'ok.png', bytes: 0, referenced: false }]);
  assert.deepEqual(manifest.dependencyGraph, { nodes: [], edges: [] });
  assert.equal(manifest.issueCount, 0);
  assert.equal(manifest.score, 0);
  const reordered = createDependencyManifest({ dependencyGraph: { nodes: [{ id: 'z', path: null }, { id: 'a', path: null }], edges: [{ from: 'z', to: 'a', kind: 'references' }, { from: 'a', to: 'z', kind: 'payload' }] } });
  assert.deepEqual(reordered.dependencyGraph.nodes.map((node) => node.id), ['a', 'z']);
  assert.deepEqual(reordered.dependencyGraph.edges.map((edge) => edge.from), ['a', 'z']);
  const invalidStatus = createDependencyManifest({ dependencies: [{ path: 'layer.usda', present: true, status: 'healthy' }] });
  assert.equal(invalidStatus.dependencies[0].status, 'available');
  const inconsistentCounts = createDependencyManifest({ dependencies: [{ path: 'layer.usda', present: true, status: 'available' }], dependencyStatusCounts: { available: 0, missing: 99 } });
  assert.deepEqual(inconsistentCounts.dependencyStatusCounts, { anonymous: 0, available: 1, missing: 0, resolver: 0, unsafe: 0 });
  assert.equal(createDependencyManifest({ score: 120 }).score, 100);
  assert.equal(createDependencyManifest({ score: -5 }).score, 0);
});

test('USD Doctor manifest source names do not expose filesystem paths', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" {}');
  assert.equal(createDependencyManifest(report, '/home/private/model.usda').source, 'model.usda');
  assert.equal(createDependencyManifest(report, 'C:\\Users\\private\\model.usda').source, 'model.usda');
});

test('dependency manifest can include an advisory quality gate', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" {}');
  const qualityGate = createQualityGate({ stage: {}, materials: [], textures: [] }, { profile: 'web-viewer' });
  assert.deepEqual(createDependencyManifest(report, 'scene.usda', qualityGate).qualityGate, qualityGate);
  assert.throws(() => createDependencyManifest(report, 'scene.usda', { schemaVersion: 1 }), /quality-gate schema/);
});

test('dependency manifest persists sanitized assistant activity', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" {}'), activity = [
    { kind: 'assistant', tool: 'scene.retopo', decision: 'executed', paths: ['/World/Mesh', '/World/../bad', '/World/Bad-Path'] },
    { kind: 'assistant', tool: 'scene.localize_dependencies', decision: 'declined', paths: ['textures/private.png'] },
    { kind: 'assistant', tool: 'scene.validate', decision: 'other', paths: 'not-an-array' },
    { kind: 'change', tool: 'scene.delete_prim', decision: 'executed', paths: ['/World/Other'] },
  ];
  assert.deepEqual(createDependencyManifest(report, 'scene.usda', null, activity).assistantActivity, [
    { tool: 'scene.retopo', decision: 'executed', paths: ['/World/Mesh'] },
    { tool: 'scene.localize_dependencies', decision: 'declined', paths: [] },
  ]);
});

test('dependency manifest assistant activity can be read back safely', () => {
  const manifest = {
    schemaVersion: 1,
    assistantActivity: [
      { tool: 'scene.retopo', decision: 'executed', paths: ['/World/Mesh', '/World/Mesh', '/World/Bad-Path'] },
      { tool: 'scene.validate', decision: 'future-state', paths: ['/World'] },
      { tool: 'scene.clean', decision: 'declined', paths: 'not-an-array' },
      null,
    ],
  };
  assert.deepEqual(parseAssistantActivityManifest(manifest), [
    { kind: 'assistant', tool: 'scene.retopo', decision: 'executed', paths: ['/World/Mesh'] },
    { kind: 'assistant', tool: 'scene.clean', decision: 'declined', paths: [] },
  ]);
  assert.deepEqual(parseAssistantActivityManifest({ schemaVersion: 2, assistantActivity: manifest.assistantActivity }), []);
  assert.deepEqual(parseAssistantActivityManifest({ schemaVersion: 1, assistantActivity: 'invalid' }), []);
});

test('project restores assistant activity without dirtying the scene', () => {
  const project = new LuciaProject();
  project.dirty = false;
  assert.equal(project.restoreAssistantActivity([
    { kind: 'assistant', tool: 'scene.validate', decision: 'executed', paths: ['/World'] },
    { kind: 'assistant', tool: 'scene.clean', decision: 'future-state', paths: ['/World'] },
    { kind: 'change', tool: 'scene.delete_prim', decision: 'executed', paths: ['/World'] },
  ]), 1);
  assert.equal(project.dirty, false);
  assert.equal(project.activity.length, 1);
  assert.equal(project.activity[0].summary, 'Assistant executed: scene.validate');
  assert.equal(project.restoreAssistantActivity('invalid'), 0);
});

test('USD Doctor rejects unsupported USDZ package member types', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" {}', new Map([
    ['textures/albedo.png', { bytes: new Uint8Array([1]) }],
    ['scripts/convert.py', { bytes: new Uint8Array([1]) }],
  ]));
  assert.ok(report.issues.some((item) => item.ruleId === 'usdz.unsupportedAssetType' && item.message.includes('convert.py')));
  assert.equal(report.issues.some((item) => item.ruleId === 'usdz.unsupportedAssetType' && item.message.includes('albedo.png')), false);
});

test('USD Doctor walks available nested USDA dependencies', () => {
  const nested = new TextEncoder().encode('#usda 1.0\ndef Xform "Nested" { references = @leaf.usda@ }');
  const leaf = new TextEncoder().encode('#usda 1.0\ndef Xform "Leaf" {}');
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @nested.usda@ }', new Map([
    ['nested.usda', { bytes: nested }],
    ['leaf.usda', { bytes: leaf }],
  ]));
  assert.deepEqual(report.dependencyGraph.edges, [
    { from: 'asset:nested.usda', to: 'asset:leaf.usda', kind: 'references', status: 'available' },
    { from: 'root', to: 'asset:nested.usda', kind: 'references', status: 'available' },
  ]);
});

test('USD Doctor resolves nested layer references relative to the layer directory', () => {
  const nested = new TextEncoder().encode('#usda 1.0\ndef Xform "Nested" { references = @leaf.usda@ }');
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @nested/layer.usda@ }', new Map([
    ['nested/layer.usda', { bytes: nested }],
    ['nested/leaf.usda', { bytes: new TextEncoder().encode('#usda 1.0\ndef Xform "Leaf" {}') }],
  ]));
  assert.deepEqual(report.dependencyGraph.edges, [
    { from: 'asset:nested/layer.usda', to: 'asset:nested/leaf.usda', kind: 'references', status: 'available' },
    { from: 'root', to: 'asset:nested/layer.usda', kind: 'references', status: 'available' },
  ]);
  assert.equal(report.dependencyGraph.nodes.find((node) => node.id === 'asset:nested/leaf.usda').present, true);
  assert.deepEqual(createDependencyManifest(report).dependencies.map((item) => item.path), ['nested/layer.usda', 'nested/leaf.usda']);
});

test('USD Doctor reports recursive composition dependency cycles', () => {
  const encode = (value) => ({ bytes: new TextEncoder().encode(value) });
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @a.usda@ }', new Map([
    ['a.usda', encode('#usda 1.0\ndef Xform "A" { references = @b.usda@ }')],
    ['b.usda', encode('#usda 1.0\ndef Xform "B" { references = @a.usda@ }')],
  ]));
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.dependencyCycle'));
  assert.equal(report.issues.filter((item) => item.ruleId === 'usd.dependencyCycle').length, 1);
});

test('USD Doctor handles deep dependency chains without recursive overflow', () => {
  const count = 5000, assets = new Map();
  for (let index = 0; index < count; index++) assets.set(`layer-${index}.usda`, { bytes: new TextEncoder().encode(index + 1 < count ? `#usda 1.0\ndef Xform "Layer" { references = @layer-${index + 1}.usda@ }` : '#usda 1.0\ndef Xform "Leaf" {}') });
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @layer-0.usda@ }', assets);
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.dependencyCycle'), false);
  assert.equal(report.dependencyGraph.nodes.filter((node) => node.kind !== 'layer').length, count);
});

test('USD Doctor reports unresolved nested dependencies and preserves package ownership', () => {
  const nested = new TextEncoder().encode('#usda 1.0\ndef Xform "Nested" { references = @missing.usda@ }');
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @nested/layer.usda@ }', new Map([
    ['nested/layer.usda', { bytes: nested }],
    ['nested/used.png', { bytes: new Uint8Array([1]) }],
  ]));
  assert.equal(report.dependencies.find((item) => item.path === 'nested/missing.usda')?.present, false);
  assert.equal(report.dependencies.find((item) => item.path === 'nested/missing.usda')?.status, 'missing');
  assert.equal(report.dependencyGraph.edges.find((item) => item.to === 'asset:nested/missing.usda')?.status, 'missing');
  assert.equal(createDependencyManifest(report).dependencyGraph.edges.find((item) => item.to === 'asset:nested/missing.usda')?.status, 'missing');
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.unresolvedAsset' && item.message.includes('nested/missing.usda')));
  assert.equal(report.packageAssets.find((item) => item.path === 'nested/layer.usda')?.referenced, true);
});

test('USD Doctor repairs only safe stage metadata', () => {
  const source = '#usda 1.0\n(\n)\ndef Xform "World" {}\n';
  const repaired = repairUSDMetadata(source, { defaultPrim: 'World', upAxis: 'Y', metersPerUnit: 1 });
  assert.match(repaired, /defaultPrim = "World"/);
  assert.match(repaired, /upAxis = "Y"/);
  assert.match(repaired, /metersPerUnit = 1/);
  assert.match(repaired, /def Xform "World"/);
});

test('USD Doctor inserts a missing layer metadata block for safe repairs', () => {
  const source = '#usda 1.0\ndef Xform "World" {}\n';
  const repaired = repairUSDMetadata(source, { defaultPrim: 'World', upAxis: 'Y', metersPerUnit: 1 });
  assert.match(repaired, /#usda 1\.0\n\(\n    defaultPrim = "World"\n    upAxis = "Y"\n    metersPerUnit = 1\n\)\ndef Xform "World"/);
  assert.equal(repairUSDMetadata(source, { kind: 'model', purpose: 'default' }), source);
});

test('USD Doctor repairs invalid upAxis values', () => {
  const source = '#usda 1.0\n( upAxis = "X" )\ndef Xform "World" {}\n';
  assert.match(repairUSDMetadata(source, { upAxis: 'Y' }), /upAxis = "Y"/);
});

test('USD Doctor refuses defaultPrim repairs that do not name a root prim', () => {
  const source = '#usda 1.0\n( defaultPrim = "Missing" )\ndef Xform "World" {}\n';
  assert.equal(repairUSDMetadata(source, { defaultPrim: 'Nested' }), source);
});

test('USD Doctor repairs invalid kind and purpose tokens with safe defaults', () => {
  const source = '#usda 1.0\n( upAxis = "Y" metersPerUnit = 1 )\ndef Xform "World" { string note = "kind = \\\"hero\\\"" # purpose = "shadow"\n kind = "hero" purpose = "shadow" }\n';
  const repaired = repairUSDMetadata(source, { kind: 'model', purpose: 'default' });
  assert.match(repaired, /kind = "model"/);
  assert.match(repaired, /purpose = "default"/);
  assert.match(repaired, /note = "kind = \\\"hero\\\"" # purpose = "shadow"/);
  assert.equal(repairUSDMetadata(source, { kind: 'invalid', purpose: 'invalid' }), source);
  assert.equal(repairUSDMetadata(source, null), source);
});

test('USD Doctor authors only explicitly reviewed inherited material overrides', () => {
  const source = '#usda 1.0\ndef Xform "World" {\n  def Mesh "M" (\n    inherits = </World/Base>\n  ) {\n  }\n  def Mesh "Base" {\n    rel material:binding = </World/Looks/Mat>\n  }\n}\n';
  const repaired = repairInheritedMaterialBindings(source, [{ path: '/World/M', materialPath: '/World/Looks/Mat' }]);
  assert.equal(repaired.changed, 1);
  assert.match(repaired.source, /def Mesh "M"[\s\S]*?rel material:binding = <\/World\/Looks\/Mat>/);
  assert.throws(() => repairInheritedMaterialBindings(repaired.source, [{ path: '/World/M', materialPath: '/World/Looks/Other' }]), /overwrite an explicit binding/);
  assert.throws(() => repairInheritedMaterialBindings(source, [{ path: '/World/M', materialPath: 'Looks/Mat' }]), /absolute prim and material paths/);
});

test('USD Doctor infers same-layer inherited material repair candidates read-only', () => {
  const source = '#usda 1.0\ndef Xform "World" {\n  def Mesh "M" ( inherits = </World/Base> ) {}\n  def Mesh "Base" { rel material:binding = </World/Looks/Mat> }\n}\n';
  assert.deepEqual(inferInheritedMaterialBindings(source), [{ path: '/World/M', materialPath: '/World/Looks/Mat', sourcePath: '/World/Base', arcKind: 'inherits' }]);
  const chain = '#usda 1.0\ndef Xform "World" {\n  def Mesh "M" ( inherits = </World/Mid> ) {}\n  def Mesh "Mid" ( specializes = </World/Base> ) {}\n  def Mesh "Base" { rel material:binding = </World/Looks/Mat> }\n}\n';
  assert.deepEqual(inferInheritedMaterialBindings(chain), [{ path: '/World/M', materialPath: '/World/Looks/Mat', sourcePath: '/World/Mid', arcKind: 'inherits' }, { path: '/World/Mid', materialPath: '/World/Looks/Mat', sourcePath: '/World/Base', arcKind: 'specializes' }]);
  assert.deepEqual(inferInheritedMaterialBindings('#usda 1.0\ndef Mesh "M" ( inherits = </External/Base> ) {}\n'), []);
});

test('USD Doctor repairs only invalid kind and purpose assignments', () => {
  const source = '#usda 1.0\n( upAxis = "Y" metersPerUnit = 1 )\ndef Xform "World" { kind = "model" purpose = "render" def Xform "Broken" { kind = "hero" purpose = "shadow" } }\n';
  const repaired = repairUSDMetadata(source, { kind: 'component', purpose: 'default' });
  assert.match(repaired, /kind = "model" purpose = "render"/);
  assert.match(repaired, /kind = "component" purpose = "default"/);
});

test('USD Doctor preserves kind and purpose text in the layer metadata block', () => {
  const source = '#usda 1.0\n( kind = "hero" purpose = "shadow" upAxis = "Y" metersPerUnit = 1 )\ndef Xform "World" { kind = "hero" purpose = "shadow" }\n';
  const repaired = repairUSDMetadata(source, { kind: 'model', purpose: 'default' });
  assert.match(repaired, /\( kind = "hero" purpose = "shadow" upAxis = "Y" metersPerUnit = 1 \)/);
  assert.match(repaired, /def Xform "World" \{ kind = "model" purpose = "default" \}/);
});

test('USD Doctor does not add prim metadata to the layer metadata block', () => {
  const source = '#usda 1.0\n( upAxis = "Y" metersPerUnit = 1 )\ndef Xform "World" {}\n';
  assert.equal(repairUSDMetadata(source, { kind: 'model', purpose: 'default' }), source);
});

test('USD Doctor ignores unsafe metadata repair values and unknown keys', () => {
  const source = '#usda 1.0\n( upAxis = "Y" metersPerUnit = 1 )\ndef Xform "World" {}\n';
  const repaired = repairUSDMetadata(source, {
    defaultPrim: 'World"\n    custom = "injected',
    upAxis: 'X',
    metersPerUnit: 0,
    customMetadata: 'should-not-be-authored',
  });
  assert.equal(repaired, source);
});

test('USD Doctor detects normalized package path collisions', () => {
  const assets = new Map([['textures/../shared.png', { bytes: new Uint8Array([1]) }], ['shared.png', { bytes: new Uint8Array([1]) }]]);
  const report = diagnoseUSD('#usda 1.0\n( defaultPrim = "World" metersPerUnit = 1 upAxis = "Y" )\ndef Xform "World" {}', assets);
  assert.ok(report.issues.some((item) => item.ruleId === 'usdz.pathCollision'));
});

test('USD Doctor classifies resolver-dependent references', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @resolver://library/asset.usda@ }');
  assert.equal(report.dependencies[0].status, 'resolver');
  assert.equal(report.dependencies[0].present, false);
  assert.deepEqual(report.dependencyStatusCounts, { available: 0, missing: 0, unsafe: 0, resolver: 1, anonymous: 0 });
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.resolverDependency'));
});

test('USD Doctor inventories registered resolver plugins deterministically', () => {
  assert.deepEqual(normalizeResolverPlugins([{ scheme: 'Resolver', name: 'Library', version: '2.0', capabilities: ['USD', 'texture', 'bad value'] }, { scheme: 'resolver', name: 'Override' }, { scheme: 'Other', name: 'Files' }, { scheme: 'bad scheme', name: 'Ignored' }, null]), [{ scheme: 'other', name: 'Files' }, { scheme: 'resolver', name: 'Library', version: '2.0', capabilities: ['texture', 'usd'] }]);
  assert.deepEqual(normalizeResolverPlugins([{ scheme: 'resolver', name: 'Zeta' }, { scheme: 'resolver', name: 'Alpha' }]), normalizeResolverPlugins([{ scheme: 'resolver', name: 'Alpha' }, { scheme: 'resolver', name: 'Zeta' }]));
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @resolver://library/asset.usda@ asset tex = @other://albedo.png@ }', new Map(), { resolverPlugins: [{ scheme: 'resolver', name: 'Library', capabilities: ['USD'] }] });
  assert.deepEqual(report.resolverPlugins, [{ scheme: 'resolver', name: 'Library', capabilities: ['usd'] }]);
  assert.deepEqual(report.resolverDependencies, [{ path: 'other://albedo.png', scheme: 'other', registered: false, requiredCapability: 'texture', capabilitySupported: false }, { path: 'resolver://library/asset.usda', scheme: 'resolver', registered: true, plugin: 'Library', requiredCapability: 'usd', capabilitySupported: true }]);
  assert.deepEqual(createDependencyManifest(report).resolverPlugins, [{ scheme: 'resolver', name: 'Library', capabilities: ['usd'] }]);
  assert.deepEqual(createDependencyManifest(report).resolverDependencies, report.resolverDependencies);
});

test('USD Doctor manifests normalize resolver capability claims', () => {
  assert.deepEqual(createDependencyManifest({ resolverPlugins: [{ scheme: 'resolver', name: 'Library' }], resolverDependencies: [{ path: 'resolver://asset.usda', scheme: 'RESOLVER', registered: false, requiredCapability: 'USD', capabilitySupported: true }] }).resolverDependencies, [{ path: 'resolver://asset.usda', scheme: 'resolver', registered: false, requiredCapability: 'usd', capabilitySupported: false }]);
});

test('USD Doctor manifests require resolver identity for registration claims', () => {
  assert.deepEqual(createDependencyManifest({ resolverPlugins: [{ scheme: 'resolver', name: 'Library' }], resolverDependencies: [{ path: 'resolver://asset.usda', scheme: 'resolver', registered: true, requiredCapability: 'usd', capabilitySupported: true }] }).resolverDependencies, [{ path: 'resolver://asset.usda', scheme: 'resolver', registered: false, requiredCapability: 'usd', capabilitySupported: false }]);
});

test('USD Doctor flags resolver schemes without a host plugin', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @missing://library/asset.usda@ }', new Map(), { resolverPlugins: [{ scheme: 'other', name: 'Other' }] });
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.unregisteredResolver'));
  assert.equal(report.score, 52);
});

test('USD Doctor flags resolver capability mismatches', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { references = @resolver://library/asset.usda@ }', new Map(), { resolverPlugins: [{ scheme: 'resolver', name: 'Images', capabilities: ['texture'] }] });
  assert.deepEqual(report.resolverDependencies, [{ path: 'resolver://library/asset.usda', scheme: 'resolver', registered: true, plugin: 'Images', requiredCapability: 'usd', capabilitySupported: false }]);
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.resolverCapability'));
});

test('USD Doctor classifies modern resolver texture formats', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { asset tex = @resolver://library/albedo.ktx2@ }', new Map(), { resolverPlugins: [{ scheme: 'resolver', name: 'Textures', capabilities: ['texture'] }] });
  assert.deepEqual(report.resolverDependencies, [{ path: 'resolver://library/albedo.ktx2', scheme: 'resolver', registered: true, plugin: 'Textures', requiredCapability: 'texture', capabilitySupported: true }]);
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.resolverCapability'), false);
});

test('USD Doctor never treats escaping references as package-resolved', () => {
  const assets = new Map([['shared.png', { bytes: new Uint8Array([1]) }]]);
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { asset file = @../shared.png@ }', assets);
  assert.equal(report.dependencies[0].present, false);
  assert.equal(report.dependencies[0].status, 'unsafe');
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.parentAssetPath'));
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.unresolvedAsset'));
});

test('USD Doctor localizes explicitly mapped dependencies to safe package paths', () => {
  const source = '#usda 1.0\ndef Xform "World" { string note = "@../source/albedo.png@" # @resolver://lib/asset.usda@\n asset texture = @../source/albedo.png@ asset other = @resolver://lib/asset.usda@ }';
  assert.deepEqual(localizeUSDDependencies(source, null), { source, changed: 0, mappings: [] });
  const localized = localizeUSDDependencies(source, { '../source/albedo.png': 'textures/albedo.png', 'resolver://lib/asset.usda': 'layers/asset.usda' });
  assert.equal(localized.changed, 2);
  assert.match(localized.source, /note = "@\.\.\/source\/albedo\.png@" # @resolver:\/\/lib\/asset\.usda@/);
  assert.match(localized.source, /@textures\/albedo\.png@/);
  assert.match(localized.source, /@layers\/asset\.usda@/);
  assert.deepEqual(localized.mappings, [
    { from: '../source/albedo.png', to: 'textures/albedo.png' },
    { from: 'resolver://lib/asset.usda', to: 'layers/asset.usda' },
  ]);
  assert.throws(() => localizeUSDDependencies(source, { 'a.png': '../outside.png' }), /safe dependency path keys/);
  assert.throws(() => localizeUSDDependencies(source, { 'a.png': 'textures/\u0000bad.png' }), /safe dependency path keys/);
  assert.throws(() => localizeUSDDependencies(source, { 'a\u0000.png': 'textures/albedo.png' }), /safe dependency path keys/);
  assert.throws(() => localizeUSDDependencies(source, { 'a.png': 'textures/shared.png', 'b.png': 'textures/shared.png' }), /destination collision/);
});

test('USD Doctor detects duplicate package payloads', () => {
  const bytes = new Uint8Array([1, 2, 3]);
  const report = diagnoseUSD('#usda 1.0\n( defaultPrim = "World" metersPerUnit = 1 upAxis = "Y" )\ndef Xform "World" {}', new Map([['a.png', { bytes }], ['b.png', { bytes: new Uint8Array(bytes) }]]));
  assert.ok(report.issues.some((item) => item.ruleId === 'usdz.duplicateAsset'));
});

test('USD Doctor reports explicitly unbound materials conservatively', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" {\n  rel material:binding = </World/Looks/Used>\n}\ndef Material "Used" {}\ndef Material "Unused" {}');
  assert.deepEqual(report.materials, { definitions: ['Unused', 'Used'], explicitBindings: ['Used'] });
  assert.deepEqual(report.orphanMaterials, ['Unused']);
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.unusedMaterial' && item.path === '/Unused'));
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.unusedMaterial' && item.path === '/Used'), false);
});

test('USD Doctor ignores material binding text in strings and comments', () => {
  const source = '#usda 1.0\ndef Xform "World" {\n  string note = "material:binding = </Used>" # rel material:binding = </Used>\n}\ndef Material "Used" {}';
  const report = diagnoseUSD(source);
  assert.deepEqual(report.materials, { definitions: ['Used'], explicitBindings: [] });
  assert.deepEqual(report.orphanMaterials, ['Used']);
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.unusedMaterial' && item.path === '/Used'));
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.externalMaterialBinding'), false);
});

test('USD Doctor ignores asset reference text in strings and comments', () => {
  const source = '#usda 1.0\ndef Xform "World" { string note = "@missing.usda@" # @also-missing.png@ }';
  const report = diagnoseUSD(source);
  assert.deepEqual(report.dependencies, []);
  assert.deepEqual(report.referencedAssets, []);
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.unresolvedAsset'), false);
});

test('USD Doctor ignores metadata assignments in strings and comments', () => {
  const source = '#usda 1.0\ndef Xform "World" { string note = "defaultPrim = \\\"Missing\\\" upAxis = \\\"X\\\" kind = \\\"hero\\\" purpose = \\\"shadow\\\"" # upAxis = "X" }';
  const report = diagnoseUSD(source);
  for (const ruleId of ['usd.invalidDefaultPrim', 'usd.invalidUpAxis', 'usd.invalidKind', 'usd.invalidPurpose']) assert.equal(report.issues.some((item) => item.ruleId === ruleId), false, ruleId);
});

test('USD Doctor exposes orphan package members as read-only cleanup candidates', () => {
  const bytes = new Uint8Array([1]);
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { asset texture = @used.png@ }', new Map([['used.png', { bytes }], ['orphan.png', { bytes }]]));
  assert.deepEqual(report.orphanAssets, ['orphan.png']);
  assert.equal(report.packageAssets.find((asset) => asset.path === 'orphan.png').referenced, false);
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.orphanedAsset'));
  assert.deepEqual(createDependencyManifest(report).orphanAssets, ['orphan.png']);
});

test('USD Doctor manifests preserve only valid inactive prim paths', () => {
  const manifest = createDependencyManifest({ inactivePrimPaths: ['/World/Hidden', '/World/Hidden', 'not-a-path', 4] });
  assert.deepEqual(manifest.inactivePrimPaths, ['/World/Hidden']);
});

test('USD Doctor distinguishes bindings whose targets are outside the layer', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { rel material:binding = </World/Looks/External> }');
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.externalMaterialBinding'));
  assert.deepEqual(report.materials, { definitions: [], explicitBindings: ['External'] });
});

test('USD Doctor does not misclassify materials when collection bindings are present', () => {
  const source = '#usda 1.0\ndef Scope "Looks" { def Material "MaybeUsed" {} }\ndef Xform "World" { rel material:binding:collection:render = </World/Collections/Render> }';
  const report = diagnoseUSD(source);
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.collectionMaterialBinding'));
  assert.deepEqual(report.orphanMaterials, []);
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.unusedMaterial'), false);
});

test('USD Doctor does not misclassify materials when inherited arcs are present', () => {
  const source = '#usda 1.0\ndef Scope "Looks" { def Material "MaybeUsed" {} }\ndef Xform "World" { inherits = </Base> }';
  const report = diagnoseUSD(source);
  assert.ok(report.issues.some((item) => item.ruleId === 'usd.inheritedMaterialBinding'));
  assert.deepEqual(report.orphanMaterials, []);
  assert.equal(report.issues.some((item) => item.ruleId === 'usd.unusedMaterial'), false);
});

test('USD Doctor exposes non-automatic material binding review candidates', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { rel material:binding:collection:render = </World/Collections/Render> inherits = </Base> }');
  assert.deepEqual(report.materialBindingReview.map(({ kind, authored, automaticFixSafe }) => ({ kind, authored, automaticFixSafe })), [
    { kind: 'collection', authored: 'material:binding:collection:render', automaticFixSafe: false },
    { kind: 'inheritance', authored: 'inherits', automaticFixSafe: false },
]);
});

test('USD Doctor attaches binding review candidates to authored prim paths', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" {\n  def Xform "Car" { rel material:binding:collection:render = </World/Collections/Render> inherits = </Base> }\n}');
  assert.deepEqual(report.materialBindingReview.map((item) => item.path), ['/World/Car', '/World/Car']);
  assert.deepEqual(report.issues.filter((item) => item.ruleId.endsWith('MaterialBinding')).map((item) => item.path), ['/World/Car', '/World/Car']);
});

test('USD Doctor validates kind and purpose tokens', () => {
  const invalid = diagnoseUSD('#usda 1.0\ndef Xform "World" { kind = "hero" purpose = "shadow" }');
  assert.deepEqual(invalid.kindValues, ['hero']);
  assert.deepEqual(invalid.purposeValues, ['shadow']);
  assert.ok(invalid.issues.some((item) => item.ruleId === 'usd.invalidKind'));
  assert.ok(invalid.issues.some((item) => item.ruleId === 'usd.invalidPurpose'));
  const valid = diagnoseUSD('#usda 1.0\ndef Xform "World" { kind = "component" purpose = "render" }');
  assert.equal(valid.issues.some((item) => item.ruleId === 'usd.invalidKind' || item.ruleId === 'usd.invalidPurpose'), false);
});

test('USD Doctor does not confuse namespaced metadata with bare assignments', () => {
  const source = '#usda 1.0\n( custom:kind = "custom" custom:purpose = "custom" )\ndef Xform "World" { kind = "hero" purpose = "shadow" }\n';
  const report = diagnoseUSD(source);
  assert.deepEqual(report.kindValues, ['hero']);
  assert.deepEqual(report.purposeValues, ['shadow']);
  const repaired = repairUSDMetadata(source, { kind: 'model', purpose: 'default' });
  assert.match(repaired, /custom:kind = "custom"/);
  assert.match(repaired, /custom:purpose = "custom"/);
  assert.match(repaired, /kind = "model" purpose = "default"/);
});

test('USD Doctor ignores material declarations in strings and comments', () => {
  const source = '#usda 1.0\ndef Xform "World" { string note = "def Material \\\"Fake\\\" {}" # def Material "CommentFake" {}\n}\ndef Material "Real" {}';
  const report = diagnoseUSD(source);
  assert.deepEqual(report.materials.definitions, ['Real']);
  assert.deepEqual(report.orphanMaterials, ['Real']);
  assert.equal(report.materials.definitions.includes('Fake'), false);
  assert.equal(report.materials.definitions.includes('CommentFake'), false);
});

test('USD Doctor preserves hash characters in strings while scanning inactive prims', () => {
  const report = diagnoseUSD('#usda 1.0\ndef Xform "World" { string custom:label = "keep#text" }\ndef Xform "Inactive" ( active = false ) {}');
  assert.equal(report.inactivePrimCount, 1);
  assert.deepEqual(report.inactivePrimPaths, ['/Inactive']);
  assert.equal(report.issues.filter((item) => item.ruleId === 'usd.inactivePrim').length, 1);
});

test('USD Doctor reports inactive prims without changing authored state', () => {
  const source = '#usda 1.0\ndef Xform "World" { active = false def Mesh "Hidden" { active = false } }';
  const report = diagnoseUSD(source);
  assert.equal(report.inactivePrimCount, 2);
  assert.deepEqual(report.inactivePrimPaths, ['/World', '/World/Hidden']);
  assert.equal(report.issues.filter((item) => item.ruleId === 'usd.inactivePrim').length, 1);
  assert.equal(report.issues.find((item) => item.ruleId === 'usd.inactivePrim').severity, 'info');
  const quoted = diagnoseUSD('#usda 1.0\n# active = false\ndef Xform "World" { string note = "active = false" }');
  assert.equal(quoted.inactivePrimCount, 0);
});

test('bake channel packing is deterministic for normalized and byte planes', () => {
  const packed = packBakeChannels({ red: new Float32Array([1, .5, 0, .25]), green: new Uint8Array([2, 4, 6, 8]), alpha: new Float32Array([0, .25, .5, 1]), resolution: 2 });
  assert.deepEqual([...packed], [255, 2, 0, 0, 128, 4, 0, 64, 0, 6, 0, 128, 64, 8, 0, 255]);
  assert.equal(packBakeChannels({ resolution: 2 }).length, 16);
  assert.throws(() => packBakeChannels({ resolution: 0 }), /resolution/);
  assert.throws(() => packBakeChannels({ red: new Float32Array([1]), resolution: 2 }), /exactly one value/);
  assert.throws(() => packBakeChannels({ red: new Float32Array([1, 0, Number.NaN, 0]), resolution: 2 }), /finite values/);
});

test('bake working-memory estimate bounds high-resolution ray baking', () => {
  const base = estimateBakeWorkingBytes({ resolution: 4096, vertexCount: 1000, indexCount: 3000 });
  const heavyAO = estimateBakeWorkingBytes({ resolution: 2048, vertexCount: 10000, indexCount: 30000, occlusion: true, samples: 16 });
  assert.ok(base < BAKE_MEMORY_LIMIT_BYTES);
  assert.ok(heavyAO > BAKE_MEMORY_LIMIT_BYTES);
});

test('projected bake working-memory estimate includes both meshes and ray buffers', () => {
  const light = estimateProjectionWorkingBytes({ resolution: 256, targetVertexCount: 100, targetIndexCount: 300, sourceVertexCount: 1000, sourceIndexCount: 3000, sourceTextureBytes: 1024 });
  const heavy = estimateProjectionWorkingBytes({ resolution: 4096, targetVertexCount: 10000, targetIndexCount: 30000, sourceVertexCount: 100000, sourceIndexCount: 300000, sourceTextureBytes: 16 * 1024 * 1024 });
  assert.ok(light > 0 && heavy > light);
  assert.ok(heavy > BAKE_MEMORY_LIMIT_BYTES);
});

test('bake result validation protects texture authoring', () => {
  const valid = { resolution: 2, pixels: new Uint8Array(16), covered: 3, total: 4, missedTexels: 1, coveredRatio: .75, visualDifference: { meanAbsoluteError: 0, maximumAbsoluteError: .1, comparedTexels: 3 } };
  assert.equal(validateBakeResult(valid), valid);
  assert.throws(() => validateBakeResult(null), /invalid result object/);
  assert.throws(() => validateBakeResult([]), /invalid result object/);
  assert.throws(() => validateBakeResult({ ...valid, pixels: new Uint8Array(4) }), /RGBA/);
  assert.throws(() => validateBakeResult({ ...valid, pixels: new Float32Array(16) }), /RGBA/);
  assert.throws(() => validateBakeResult({ ...valid, coveredRatio: 2 }), /coverage/);
  assert.throws(() => validateBakeResult({ ...valid, missedTexels: 0 }), /coverage/);
  assert.throws(() => validateBakeResult({ ...valid, coveredRatio: .5 }), /coverage/);
  assert.throws(() => validateBakeResult({ ...valid, islandCount: -1 }), /coverage/);
  assert.throws(() => validateBakeResult({ ...valid, islandCount: 4 }), /coverage/);
  assert.throws(() => validateBakeResult({ ...valid, visualDifference: { ...valid.visualDifference, maximumAbsoluteError: 2 } }), /visual-difference/);
});

test('bake image resizing is bounded and deterministic', () => {
  const source = Uint8Array.from([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]);
  const result = resizeBakeImage({ pixels: source, fromResolution: 2, resolution: 8, maxResolution: 4 });
  assert.equal(result.resolution, 4);
  assert.equal(result.pixels.length, 4 * 4 * 4);
  assert.deepEqual([...resizeBakeImage({ pixels: source, fromResolution: 2, resolution: 2 }).pixels], [...source]);
  const rectangle = resizeBakeImage({ pixels: Uint8Array.from([...source, ...source]), fromWidth: 2, fromHeight: 4, width: 1, height: 2 });
  assert.deepEqual([rectangle.width, rectangle.height], [1, 2]);
  assert.equal(rectangle.pixels.length, 1 * 2 * 4);
  assert.throws(() => resizeBakeImage({ pixels: Float32Array.from([...source.slice(0, 15), Number.NaN]), fromResolution: 2, resolution: 1 }), /finite values/);
  assert.throws(() => resizeBakeImage({ pixels: source, fromResolution: 2, resolution: 1.5 }), /dimensions/);
  assert.throws(() => resizeBakeImage({ pixels: source, fromWidth: 1.5, fromHeight: 2, width: 1, height: 1 }), /dimensions/);
  const monochrome = Uint8Array.from([0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255]);
  assert.equal(resizeBakeImage({ pixels: monochrome, fromResolution: 2, resolution: 1, colorSpace: 'linear' }).pixels[0], 128);
  assert.equal(resizeBakeImage({ pixels: monochrome, fromResolution: 2, resolution: 1, colorSpace: 'srgb' }).pixels[0], 188);
  const rec2020 = Uint8Array.from([0, 0, 0, 255, 255, 0, 0, 255, 0, 0, 0, 255, 255, 0, 0, 255]);
  assert.equal(resizeBakeImage({ pixels: rec2020, fromResolution: 2, resolution: 1, colorSpace: 'rec2020' }).pixels[0], 180);
  assert.equal(resizeBakeImage({ pixels: rec2020, fromResolution: 2, resolution: 1, colorSpace: 'linear-rec2020' }).pixels[0], 128);
  assert.equal(resizeBakeImage({ pixels: rec2020, fromResolution: 2, resolution: 1, colorSpace: 'display-p3' }).pixels[0], 188);
  assert.equal(resizeBakeImage({ pixels: rec2020, fromResolution: 2, resolution: 1, colorSpace: 'adobergb' }).pixels[0], 186);
  assert.throws(() => resizeBakeImage({ pixels: source, fromResolution: 2, resolution: 2, colorSpace: 'unsupported-space' }), /Unsupported bake color space/);
});

test('bake alpha analysis identifies removable opaque alpha', () => {
  const opaque = Uint8Array.from([1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255]);
  const mixed = Uint8Array.from([1, 2, 3, 255, 4, 5, 6, 0, 7, 8, 9, 128, 10, 11, 12, 255]);
  assert.deepEqual(analyzeBakeAlpha({ pixels: opaque, resolution: 2 }), { transparentTexels: 0, coverageRatio: 1, allOpaque: true, allTransparent: false, unusedAlpha: true });
  assert.equal(analyzeBakeAlpha({ pixels: mixed, resolution: 2 }).unusedAlpha, false);
  assert.throws(() => analyzeBakeAlpha({ pixels: Float32Array.from([...opaque.slice(0, 15), Number.NaN]), resolution: 2 }), /finite values/);
  assert.throws(() => analyzeBakeAlpha({ pixels: opaque, resolution: 0 }), /resolution/);
});

test('normal-map Y conversion flips only the green channel', () => {
  const pixels = Uint8Array.from([10, 20, 30, 255, 40, 128, 60, 200, 70, 0, 80, 100, 90, 255, 100, 1]);
  assert.deepEqual([...convertNormalMapY({ pixels, resolution: 2, convention: 'directx' })], [10, 235, 30, 255, 40, 127, 60, 200, 70, 255, 80, 100, 90, 0, 100, 1]);
  assert.deepEqual([...convertNormalMapY({ pixels, resolution: 2 })], [...pixels]);
  assert.throws(() => convertNormalMapY({ pixels, resolution: 2, convention: 'vulkan' }), /Unsupported normal-map/);
  assert.throws(() => convertNormalMapY({ pixels: Float32Array.from([...pixels.slice(0, 15), Number.NaN]), resolution: 2, convention: 'directx' }), /finite values/);
});

test('normal baking encodes deterministic tangent-space normals', () => {
  const normals = new Float32Array([0, 0, 1, 0, 1, 0]);
  const tangents = new Float32Array([1, 0, 0, 1, 1, 0, 0, -1]);
  assert.deepEqual([...encodeNormalVectors({ normals, tangents, space: 'tangent' })].map((value) => Number(value.toFixed(6))), [.5, .5, 1, .5, .5, 1]);
  assert.throws(() => encodeNormalVectors({ normals, space: 'tangent' }), /requires one vec4 tangent/);
  assert.throws(() => encodeNormalVectors({ normals: { length: 3, 0: 0, 1: 0, 2: 1 }, space: 'object' }), /three components/);
  assert.throws(() => encodeNormalVectors({ normals: new Float32Array([0, 0, 0]), tangents: new Float32Array([1, 0, 0, 1]), space: 'tangent' }), /non-zero normals/);
});

test('projected normal baking uses target-face tangent frames', () => {
  const values = encodeProjectedTangentNormals({ values: new Float32Array([0, 0, 1]), targetTriangle: new Int32Array([0]), targetBarycentrics: new Float32Array([1, 0, 0]), targetIndices: new Uint32Array([0, 1, 2]), targetNormals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]), targetTangents: new Float32Array([1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1]) });
  assert.deepEqual([...values].map((value) => Number(value.toFixed(6))), [.5, .5, 1]);
});

test('normal-map Y conversion supports non-square images', () => {
  const pixels = new Uint8ClampedArray([10, 20, 30, 255, 40, 80, 60, 255]);
  assert.deepEqual([...convertNormalMapY({ pixels, width: 2, height: 1, convention: 'directx' })], [10, 235, 30, 255, 40, 175, 60, 255]);
});

test('UV baker rasterizes covered texels instead of a gradient', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), color: [1, .25, 0], resolution: 8, dilation: 0 });
  assert.ok(result.covered > 0 && result.covered < result.total);
  assert.deepEqual([...result.pixels.slice(0, 4)], [255, 64, 0, 255]);
  const dilated = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), color: [1, .25, 0], resolution: 8, dilation: 2 });
  assert.ok(dilated.covered > result.covered);
  assert.equal(dilated.dilatedTexels, dilated.covered - result.covered);
  assert.equal(dilated.islandCount, 1);
  const twoIslands = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 0, 0, 3, 0, 0, 2, 1, 0]), indices: new Uint32Array([0, 1, 2, 3, 4, 5]), uvs: new Float32Array([0, 0, .25, 0, 0, .25, .75, .75, 1, .75, .75, 1]), color: [1, .25, 0], resolution: 16, dilation: 0 });
  assert.equal(twoIslands.islandCount, 2);
  assert.equal(result.rasterizedFaces, 1);
  assert.equal(result.missedTexels, result.total - result.covered);
  assert.ok(result.visualDifference.comparedTexels > 0);
  assert.ok(result.visualDifference.meanAbsoluteError < 0.01);
});

test('UV bake worker matches the CPU raster parity reference', async () => {
  const data = {
    type: 'bake-base-color',
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2]),
    uvs: new Float32Array([.05, .05, .95, .05, .05, .95]),
    colors: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]),
    sourceColorTransform: { canonical: 'g22_rec709_scene', colorRole: 'data', gamma: 2.2, linearBias: 0, matrix: [1, 0, 0, 0, 1, 0, 0, 0, 1] },
    destinationColorSpace: 'linear-srgb',
    resolution: 16,
    dilation: 2,
    samples: 4,
  };
  const expected = rasterizeBaseColor(data), previousSelf = globalThis.self;
  let actual = null;
  globalThis.self = { postMessage(message) { actual = message; } };
  try {
    await import(`../src/bake-worker.js?parity=${Date.now()}`);
    await globalThis.self.onmessage({ data });
  } finally {
    if (previousSelf === undefined) delete globalThis.self;
    else globalThis.self = previousSelf;
  }
  assert.equal(actual?.type, 'result');
  assert.deepEqual([...actual.pixels], [...expected.pixels]);
  assert.equal(actual.covered, expected.covered);
  assert.equal(actual.total, expected.total);
  assert.equal(actual.missedTexels, expected.missedTexels);
  assert.equal(actual.coveredRatio, expected.coveredRatio);
});

test('normal bake worker matches the CPU raster parity reference', async () => {
  const normals = new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]), tangents = new Float32Array([1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1]), data = {
    type: 'bake-base-color',
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
    indices: new Uint32Array([0, 1, 2]),
    uvs: new Float32Array([.05, .05, .95, .05, .05, .95]),
    colors: encodeNormalVectors({ normals, tangents, space: 'tangent' }),
    resolution: 16,
    dilation: 2,
    samples: 4,
  };
  const expected = rasterizeBaseColor(data), previousSelf = globalThis.self;
  let actual = null;
  globalThis.self = { postMessage(message) { actual = message; } };
  try {
    await import(`../src/bake-worker.js?normal-parity=${Date.now()}`);
    await globalThis.self.onmessage({ data });
  } finally {
    if (previousSelf === undefined) delete globalThis.self;
    else globalThis.self = previousSelf;
  }
  assert.equal(actual?.type, 'result');
  assert.deepEqual([...actual.pixels], [...expected.pixels]);
  assert.equal(actual.covered, expected.covered);
  assert.equal(actual.missedTexels, expected.missedTexels);
  assert.equal(actual.dilatedTexels, expected.dilatedTexels);
});

test('UV baker reports malformed and degenerate faces', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 3, 0, 1, 2]), uvs: new Float32Array([0, 0, 0, 0, 0, 0]), resolution: 8, dilation: 0 });
  assert.equal(result.skippedFaces, 1);
  assert.equal(result.degenerateFaces, 1);
  assert.equal(result.rasterizedFaces, 0);
});

test('UV baker rejects unsafe geometry before typed-array coercion', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), uvs = new Float32Array([0, 0, 1, 0, 0, 1]);
  assert.throws(() => rasterizeBaseColor({ positions: new Float64Array([1e40, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs }), /finite position\/UV buffers/);
  assert.throws(() => rasterizeBaseColor({ positions, indices: new Float32Array([-1, 1, 2]), uvs }), /finite position\/UV buffers/);
  assert.throws(() => rasterizeBaseColor({ positions, indices: new Float32Array([0.5, 1, 2]), uvs }), /finite position\/UV buffers/);
  assert.throws(() => rasterizeBaseColor({ positions, indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0]) }), /finite position\/UV buffers/);
});

test('UV baker supports deterministic supersampling coverage', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, .5, 0, 0, .5]), color: [1, 0, 0], resolution: 8, dilation: 0, samples: 4 });
  assert.ok([...result.pixels].some((value, index) => index % 4 === 3 && value > 0 && value < 255));
  assert.equal(result.coveredRatio, result.covered / result.total);
});

test('UV baker keeps triangle island ownership stable during dilation', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 10, 0, 0, 11, 0, 0, 10, 1, 0]);
  const indices = new Uint32Array([0, 1, 2, 3, 4, 5]);
  const uvs = new Float32Array([0, 0, .25, 0, 0, .25, .75, .75, 1, .75, .75, 1]);
  const result = rasterizeBaseColor({ positions, indices, uvs, color: [1, 0, 0], resolution: 8, dilation: 2 });
  assert.equal(result.rasterizedFaces, 2);
  assert.ok(result.covered > 0);
  assert.equal(result.missedTexels, result.total - result.covered);
});

test('UV baker accepts scalar material channels as RGB data', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), color: [.25, .25, .25], resolution: 8, dilation: 0 });
  const offset = (0 * 8 + 0) * 4;
  assert.deepEqual([...result.pixels.slice(offset, offset + 4)], [64, 64, 64, 255]);
});

test('UV baker consumes per-face material buffers before vertex fallback', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), materialIndices: new Uint32Array([1]), materialColors: new Float32Array([1, 0, 0, 0, 1, 0]), resolution: 8, dilation: 0 });
  assert.equal(result.pixels.some((value, index) => index % 4 === 1 && value > 200), true);
  assert.equal(result.pixels.some((value, index) => index % 4 === 0 && value > 200), false);
});

test('UV baker rejects malformed material buffers', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), resolution: 8, dilation: 0 };
  assert.throws(() => rasterizeBaseColor({ ...mesh, materialIndices: new Uint32Array([0, 1]) }), /one non-negative integer/);
  assert.throws(() => rasterizeBaseColor({ ...mesh, materialIndices: new Int32Array([-1]) }), /one non-negative integer/);
  assert.throws(() => rasterizeBaseColor({ ...mesh, materialColors: new Float32Array([1, 0, NaN]) }), /finite RGB/);
});

test('UV baker interpolates valid vertex colors', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), colors: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1]), resolution: 8, dilation: 0 });
  const center = (3 * 8 + 2) * 4;
  assert.ok(result.pixels[center] > 0 && result.pixels[center + 1] > 0 && result.pixels[center + 2] > 0);
});

test('UV baker samples texture-driven RGBA input in UV space', () => {
  const texturePixels = new Uint8Array([0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255]);
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), texturePixels, textureWidth: 2, textureHeight: 2, color: [1, 0, 0], resolution: 8, dilation: 0 });
  const covered = [...result.pixels].some((value, index) => index % 4 === 3 && value > 0 && result.pixels[index - 3] === 0 && result.pixels[index - 2] === 255);
  assert.equal(covered, true);
});

test('UV baker applies explicit color-space conversion to color textures', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), materialTextures: [{ pixels: new Float32Array([.5, .5, .5, 1]), width: 1, height: 1, colorSpace: 'linear' }], destinationColorSpace: 'srgb', resolution: 8, dilation: 0 });
  assert.ok([...result.pixels].some((value, index) => index % 4 === 3 && value > 0 && result.pixels[index - 3] === 188));
});

test('UV baker selects the texture buffer for each material index', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), materialIndices: new Uint32Array([1]), materialTextures: [
    { pixels: new Uint8Array([255, 0, 0, 255]), width: 1, height: 1 },
    { pixels: new Uint8Array([0, 255, 0, 255]), width: 1, height: 1 },
  ], textureWidth: 1, textureHeight: 1, resolution: 8, dilation: 0 });
  assert.equal(result.pixels.some((value, index) => index % 4 === 1 && value > 200), true);
  assert.equal(result.pixels.some((value, index) => index % 4 === 0 && value > 200), false);
});

test('UV baker rejects malformed per-material texture descriptors', () => {
  const mesh = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), resolution: 8, dilation: 0 };
  assert.throws(() => rasterizeBaseColor({ ...mesh, materialTextures: [{ pixels: new Uint8Array(3), width: 1, height: 1 }] }), /valid dimensions and RGBA/);
  assert.throws(() => rasterizeBaseColor({ ...mesh, materialTextures: [{ pixels: new Uint8Array(4), width: 0, height: 1 }] }), /valid dimensions and RGBA/);
});

test('UV baker can collapse texture input to a deterministic scalar channel', () => {
  const texturePixels = new Uint8Array([64, 192, 255, 255, 64, 192, 255, 255, 64, 192, 255, 255, 64, 192, 255, 255]);
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), texturePixels, textureWidth: 2, textureHeight: 2, textureChannel: 1, resolution: 8, dilation: 0 });
  const covered = [...result.pixels].some((value, index) => index % 4 === 3 && value > 0 && result.pixels[index - 3] === 192 && result.pixels[index - 2] === 192 && result.pixels[index - 1] === 192);
  assert.equal(covered, true);
});

test('UV baker preserves deterministic per-face ID colors', () => {
  const result = rasterizeBaseColor({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]), uvs: new Float32Array([0, 0, 1, 0, 0, 1]), faceColors: new Float32Array([.25, .5, .75]), resolution: 8, dilation: 0 });
  const pixel = result.pixels.findIndex((value, index) => index % 4 === 0 && value === 64);
  assert.ok(pixel >= 0);
  assert.deepEqual([...result.pixels.slice(pixel, pixel + 4)], [64, 128, 191, 255]);
});

test('normal recompute creates normalized area-weighted vertex normals', () => {
  const normals = recomputeVertexNormals({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]) });
  assert.equal(normals.length, 9);
  for (let i = 0; i < 3; i++) assert.deepEqual([...normals.slice(i * 3, i * 3 + 3)], [0, 0, 1]);
});

test('normal recompute creates one normalized face-varying normal per corner', () => {
  const normals = recomputeFaceVaryingNormals({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]), indices: new Uint32Array([0, 1, 2, 0, 3, 1]) });
  assert.equal(normals.length, 18);
  assert.deepEqual([...normals.slice(0, 9)], [0, 0, 1, 0, 0, 1, 0, 0, 1]);
  assert.deepEqual([...normals.slice(9)], [0, 1, 0, 0, 1, 0, 0, 1, 0]);
});

test('normal recompute supports angle weighting and bounded smoothing', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]);
  const indices = new Uint32Array([0, 1, 2, 0, 3, 1]);
  const smooth = recomputeVertexNormals({ positions, indices, weighting: 'angle', smoothingAngle: 180 });
  const normals = recomputeVertexNormals({ positions, indices, weighting: 'angle', smoothingAngle: 35 });
  assert.ok(normals.every(Number.isFinite));
  assert.ok(Math.abs(Math.hypot(normals[0], normals[1], normals[2]) - 1) < 1e-5);
  assert.ok(Math.abs(smooth[1] - smooth[2]) < 1e-5 && smooth[1] > 0.7);
  assert.deepEqual([...normals.slice(0, 3)], [0, 0, 1]);
});

test('normal recompute does not smooth faces that only meet at a vertex', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0]);
  const normals = recomputeVertexNormals({ positions, indices: new Uint32Array([0, 1, 2, 0, 3, 4]), smoothingAngle: 180 });
  assert.deepEqual([...normals.slice(0, 3)], [0, 0, 1]);
});

test('normal recompute can preserve material boundaries', () => {
  const positions = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1]);
  const indices = new Uint32Array([0, 1, 2, 1, 0, 3]);
  const groups = [{ start: 0, count: 3, materialIndex: 0 }, { start: 3, count: 3, materialIndex: 1 }];
  const preserved = recomputeVertexNormals({ positions, indices, groups, smoothingAngle: 180, preserveMaterialBoundaries: true });
  const blended = recomputeVertexNormals({ positions, indices, groups, smoothingAngle: 180, preserveMaterialBoundaries: false });
  assert.deepEqual([...preserved.slice(0, 3)], [0, 0, 1]);
  assert.ok(blended[1] > 0.6 && blended[2] > 0.6);
});
