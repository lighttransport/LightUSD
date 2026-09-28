// Map simple layer string/export and flatten operations to next document/session workflows.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const flatten = new Set(read('next-flatten-session-api-inventory.json'));
const layer = new Set(read('next-layer-document-api-inventory.json'));
const assets = new Set(read('next-asset-store-api-inventory.json'));
const render = new Set(read('renderstream-api-inventory.json'));
const map = read('next-layer-export-parity-map.json');

function readStoredZipEntries(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const decoder = new TextDecoder();
  const entries = new Map();
  let offset = 0;
  while (offset + 30 <= bytes.length && view.getUint32(offset, true) === 0x04034b50) {
    const method = view.getUint16(offset + 8, true);
    const compressedSize = view.getUint32(offset + 18, true);
    const nameLength = view.getUint16(offset + 26, true);
    const extraLength = view.getUint16(offset + 28, true);
    const nameStart = offset + 30;
    const dataStart = nameStart + nameLength + extraLength;
    const dataEnd = dataStart + compressedSize;
    assert.equal(method, 0, 'USDZ package entries are stored');
    assert.ok(dataEnd <= bytes.length, 'ZIP local entry is in bounds');
    const name = decoder.decode(bytes.subarray(nameStart, nameStart + nameLength));
    entries.set(name, bytes.slice(dataStart, dataEnd));
    offset = dataEnd;
  }
  return entries;
}
assert.deepEqual(map.map(row => row.legacyMethod), [
  'loadLayerFromJSON',
  'exportAsUSDA', 'exportAsUSDC', 'exportLayerAsUSDCToBufferWithOptions',
  'exportLayerAsUSDCWithOptions', 'exportLayerAsUSDZWithOptions',
  'exportStageAsUSDCToBufferWithOptions',
  'flattenLayer', 'layerToString', 'layerToJSON', 'layerToJSONWithOptions',
]);
for (const row of map) {
  assert.ok(legacy.includes(row.legacyMethod));
  // Rows promoted by a paired behavior test mirror the matrix status.
  assert.ok(['workflow_covered_behavior_review_required', 'behavior_verified'].includes(row.parityStatus));
  const target = row.nextSurface === 'NextFlattenSession' ? flatten
    : row.nextSurface === 'NextLayerDocument' ? layer
    : row.nextSurface === 'RenderStream' ? render : null;
  assert.ok(target, `${row.legacyMethod}: invalid next surface`);
  for (const method of row.workflow) {
    if (typeof method === 'string') {
      assert.ok(target.has(method), `${row.legacyMethod}: missing ${row.nextSurface}.${method}`);
    } else {
      const inventory = method.surface === 'NextAssetStore' ? assets
        : method.surface === 'NextLayerDocument' ? layer : null;
      assert.ok(inventory?.has(method.method),
        `${row.legacyMethod}: missing ${method.surface}.${method.method}`);
    }
  }
  for (const method of row.assetWorkflow || []) assert.ok(assets.has(method.method));
  assert.ok(target.has(row.nextEquivalent));
  const matrix = read('next-wasm-parity-gaps.json');
  const rowInMatrix = matrix.find(item => item.legacyMethod === row.legacyMethod);
  assert.equal(rowInMatrix?.nextSurface, row.nextSurface);
  assert.equal(rowInMatrix?.nextEquivalent, row.nextEquivalent);
  assert.deepEqual(rowInMatrix?.workflow, row.workflow);
  assert.equal(rowInMatrix?.parityStatus, row.parityStatus);
}
console.log(`ok - layer export parity map covers ${map.length} operations via next layer/session workflows`);

// Compare the optioned legacy layer export with the next LayerDocument path.
// The legacy binding currently ignores this method's options object.
import {loadWasm} from '../src/usdzconvert.js';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const combinedDefault = wasm64 ? '../src/lightusd/lightusd_combined_64.js'
                               : '../src/lightusd/lightusd_combined.js';
const combinedUrl = process.env.LIGHTUSD_COMBINED_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_COMBINED_MODULE)).href
  : new URL(combinedDefault, import.meta.url).href;
const legacyModule = await loadWasm(() => import(combinedUrl));
const nextGlue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                        : '../src/lightusd/lightusd_next.js';
const {default: createNextModule} = await import(new URL(nextGlue, import.meta.url));
const nextModule = await createNextModule();
const bytes = new Uint8Array(fs.readFileSync(new URL(
  '../../../tests/usda/c-core-material-queries.usda', import.meta.url)));
const legacyLoader = new legacyModule.LightUSDLoaderNative();
const nextDocument = new nextModule.LayerDocument();
const legacyReload = new nextModule.LayerDocument();
const nextReload = new nextModule.LayerDocument();
try {
  const unloadedLegacy = new legacyModule.LightUSDLoaderNative();
  const unloadedNext = new nextModule.LayerDocument();
  try {
    const oldResult = unloadedLegacy.exportLayerAsUSDCToBufferWithOptions(null, {});
    const newResult = unloadedNext.exportUSDCToBuffer(null, {});
    assert.deepEqual(newResult, oldResult,
      'unloaded-layer error must precede caller-buffer validation');
  } finally { unloadedLegacy.delete(); unloadedNext.delete(); }
  assert.equal(legacyLoader.loadAsLayerFromBinary(bytes, 'layer-export-parity.usda'), true, legacyLoader.error());
  const loaded = nextDocument.load(bytes);
  assert.equal(loaded.success, true, loaded.error);
  const jsonSource = new TextEncoder().encode([
    '#usda 1.0',
    '( comment = "JSON parity" playbackMode = "loop" colorConfiguration = @ocio://default@ colorManagementSystem = "ocio" customLayerData = { float[] samples = [0.25, 1.5] } expressionVariables = { string ASSET = "./mesh.usd" } )',
    'def Xform "Base" {}',
    'def Xform "Root" (',
    '  prepend inherits = </Base>',
    '  append specializes = </Base>',
    '  prepend references = @asset.usda@</Geom> (offset = 2; scale = 0.5)',
    '  append payload = @payload.usda@</Geom>',
    '  prepend variantSets = "shape"',
    '  variants = { string shape = "round" }',
    ') {',
    '  custom string label = "hello" (displayName = "Label" customData = { bool enabled = 1 dictionary nested = { string owner = "team" bool[] flags = [true, false] color3f[] colors = [(0.25, 0.5, 0.75), (1, 0, 0)] } } sdrMetadata = { string role = "input" })',
    '  float weight.timeSamples = { 0: 1, 1: 2 }',
    '  float[] samples = [0.25, 1.5, 2.75]',
    '  float blocked = None',
    '  float emptyAttr',
    '  custom token customEmpty',
    '  float sampleBlock.timeSamples = { 0: None, 1: 2 }',
    '  rel child = </Root/Child>',
    '  rel blockedRel = None',
    '  rel declaredRel',
    '  rel emptyTargets = []',
    '  add rel added = [</Root/Child>]',
    '  prepend rel prepended = [</Root/Child>]',
    '  append rel appended = [</Root/Child>]',
    '  delete rel deleted = [</Root/Child>]',
    '  def Scope "Child" {}', '}',
    'def Xform "AssetOnly" (inherits = </Base> references = @asset.usda@ payload = @payload.usda@) {}', ''
  ].join('\n'));
  const jsonLegacy = new legacyModule.LightUSDLoaderNative();
  const jsonNext = new nextModule.LayerDocument();
  try {
    assert.equal(jsonLegacy.loadAsLayerFromBinary(jsonSource, 'json-parity.usda'), true,
      jsonLegacy.error());
    assert.equal(jsonNext.load(jsonSource).success, true);
    const oldJSON = JSON.parse(jsonLegacy.layerToJSON());
    assert.equal(oldJSON.primSpecs.Root.properties.blocked.attribute.valueType,
      'blocked');
    assert.equal(oldJSON.primSpecs.Root.properties.blockedRel.relationship.valueType,
      'valueBlock');
    assert.equal(oldJSON.primSpecs.Root.properties.blockedRel.relationship.blocked,
      true);
    assert.deepEqual(oldJSON.primSpecs.Root.properties.label.attribute.metadata.customData,
      {enabled: true, nested: {owner: 'team',
        flags: {type: 'bool[]', value: [true, false]},
        colors: {type: 'color3f[]', value: [[0.25, 0.5, 0.75], [1, 0, 0]]}}});
    assert.deepEqual(oldJSON.primSpecs.Root.properties.label.attribute.metadata.sdrMetadata,
      {role: 'input'});
    assert.equal(oldJSON.primSpecs.Root.properties.emptyAttr.propertyType,
      'emptyAttribute');
    assert.equal(oldJSON.primSpecs.Root.properties.declaredRel.propertyType,
      'noTargetsRelationship');
    assert.deepEqual(oldJSON.primSpecs.Root.properties.emptyTargets.relationship,
      {type: 'relationship', listEditQual: 'resetToExplicit',
        valueType: 'pathVector', hasTargets: true, targets: [], targetCount: 0});
    assert.equal(oldJSON.primSpecs.Root.properties.appended.listEditQual, 'append');
    assert.deepEqual(oldJSON.primSpecs.Root.inherits,
      [{op: 'prepend', items: ['/Base']}]);
    assert.deepEqual(oldJSON.primSpecs.Root.specializes,
      [{op: 'append', items: ['/Base']}]);
    assert.deepEqual(oldJSON.primSpecs.Root.references,
      [{op: 'prepend', items: [{assetPath: 'asset.usda', primPath: '/Geom',
        offset: 2, scale: 0.5}]}]);
    assert.deepEqual(oldJSON.primSpecs.Root.payloads,
      [{op: 'append', items: [{assetPath: 'payload.usda', primPath: '/Geom'}]}]);
    assert.deepEqual(oldJSON.primSpecs.Root.variantSets,
      [{op: 'prepend', items: ['shape']}]);
    assert.deepEqual(oldJSON.primSpecs.Root.variants, {shape: 'round'});
    assert.deepEqual(oldJSON.primSpecs.AssetOnly.references,
      [{op: '', items: [{assetPath: 'asset.usda', primPath: '#INVALID#'}]}]);
    assert.deepEqual(oldJSON.primSpecs.AssetOnly.inherits,
      [{op: '', items: ['/Base']}]);
    for (const [name, qualifier] of [['added', 'add'], ['prepended', 'prepend'],
                                     ['deleted', 'delete']]) {
      assert.equal(oldJSON.primSpecs.Root.properties[name].listEditQual, qualifier);
    }
    assert.equal(oldJSON.primSpecs.Root.properties.sampleBlock.attribute.timeSamples[0].blocked,
      true);
    const originalMetas = structuredClone(oldJSON.metas);
    assert.deepEqual(oldJSON.metas.customLayerData.samples,
      {type: 'float[]', value: [0.25, 1.5]},
      'legacy serializes authored metadata arrays with an explicit USD type');
    assert.deepEqual(oldJSON.metas.expressionVariables, {ASSET: './mesh.usd'});
    assert.equal(oldJSON.primSpecs.Root.properties.samples.attribute.value,
      '[0.25, 1.5, 2.75]');
    assert.equal(oldJSON.metas.colorConfiguration, 'ocio://default');
    assert.equal(oldJSON.metas.colorManagementSystem, 'ocio');
    assert.equal(oldJSON.metas.playbackMode, 'loop');
    oldJSON.metas.hasOwnedSubLayers = true;
    oldJSON.metas.primChildren = ['Base', 'Root', 'AssetOnly'];
    oldJSON.metas.subLayers = [{assetPath: './child.usda',
      layerOffset: {offset: 2.5, scale: 0.5}}];
    oldJSON.metas.layerRelocates = [{source: '/Old', target: '/New'}];
    oldJSON.metas.unregisteredMetas = {pipelineTag: 'reviewed'};
    const importResult = jsonNext.loadJSON(JSON.stringify(oldJSON));
    assert.equal(importResult.success, true, importResult.error);
    const importedPrimSpecs = JSON.parse(jsonNext.exportJSON().text).primSpecs;
    assert.deepEqual(importedPrimSpecs, oldJSON.primSpecs,
      'legacy JSON import preserves the tested prim-spec subset');
    const importedUSDA = jsonNext.exportUSDA();
    assert.equal(importedUSDA.success, true, importedUSDA.error);
    assert.match(importedUSDA.text, /prepend inherits = <\/Base>/);
    assert.match(importedUSDA.text, /append specializes = <\/Base>/);
    assert.match(importedUSDA.text,
      /prepend references = @asset\.usda@<\/Geom> \(offset = 2; scale = 0\.5\)/);
    assert.match(importedUSDA.text, /append payload = @payload\.usda@<\/Geom>/);
    assert.match(importedUSDA.text, /prepend variantSets = \["shape"\]/);
    assert.match(importedUSDA.text, /string shape = "round"/);
    assert.match(importedUSDA.text, /references = @asset\.usda@/);
    assert.match(importedUSDA.text, /rel declaredRel\n/);
    assert.match(importedUSDA.text, /rel emptyTargets = \[\]/);
    const assetInfoJSON = structuredClone(oldJSON);
    assetInfoJSON.metas = {comment: 'JSON parity', doc: 'Layer documentation',
      owner: 'render-team', renderSettingsPrimPath: '/Root',
      colorConfiguration: 'ocio://default', colorManagementSystem: 'ocio',
      playbackMode: 'loop'};
    assetInfoJSON.primSpecs.Root.properties.label.attribute.metadata.assetInfo =
      {identifier: 'asset.usda', nested: {approved: true}};
    const assetInfoDoc = new nextModule.LayerDocument();
    try {
      const loadedAssetInfo = assetInfoDoc.loadJSON(JSON.stringify(assetInfoJSON));
      assert.equal(loadedAssetInfo.success, true, loadedAssetInfo.error);
      assert.deepEqual(JSON.parse(assetInfoDoc.exportJSON().text).primSpecs.Root
        .properties.label.attribute.metadata.assetInfo,
      assetInfoJSON.primSpecs.Root.properties.label.attribute.metadata.assetInfo,
      'property assetInfo dictionary round-trips through next Layer JSON');
      const assetInfoUSDA = assetInfoDoc.exportUSDA();
      assert.equal(assetInfoUSDA.success, true, assetInfoUSDA.error);
      const assetInfoReload = new nextModule.LayerDocument();
      try {
        const reloaded = assetInfoReload.load(new TextEncoder().encode(assetInfoUSDA.text));
        assert.equal(reloaded.success, true, reloaded.error);
        assert.deepEqual(JSON.parse(assetInfoReload.exportJSON().text).primSpecs.Root
          .properties.label.attribute.metadata.assetInfo,
        assetInfoJSON.primSpecs.Root.properties.label.attribute.metadata.assetInfo,
        'property assetInfo survives next USDA export and reload');
        const assetInfoUSDC = assetInfoDoc.exportUSDC();
        assert.equal(assetInfoUSDC.success, true, assetInfoUSDC.error);
        const crateReload = assetInfoReload.load(assetInfoUSDC.data);
        assert.equal(crateReload.success, true, crateReload.error);
        assert.deepEqual(JSON.parse(assetInfoReload.exportJSON().text).primSpecs.Root
          .properties.label.attribute.metadata.assetInfo,
        assetInfoJSON.primSpecs.Root.properties.label.attribute.metadata.assetInfo,
        'property assetInfo survives next USDC export and reload');
        const crateMetas = JSON.parse(assetInfoReload.exportJSON().text).metas;
        for (const [key, value] of Object.entries(assetInfoJSON.metas)) {
          assert.equal(crateMetas[key], value,
            `layer ${key} survives next USDC export and reload`);
        }
      } finally { assetInfoReload.delete(); }
    } finally { assetInfoDoc.delete(); }
    const typedAssetJSON = JSON.stringify({typeName: 'Layer',
      metas: {customLayerData: {
        inputFile: {type: 'asset',
          value: {assetPath: 'foo.usda', resolvedPath: '/resolved/foo.usda'}},
        matrix: {type: 'matrix4d', value: [
          [1, 2, 3, 4], [5, 6, 7, 8], [9, 10, 11, 12], [13, 14, 15, 16]]},
        matrix2: {type: 'matrix2d', value: [[1, 2], [3, 4]]},
        matrix3: {type: 'matrix3f', value: [
          [1, 2, 3], [4, 5, 6], [7, 8, 9]]},
        orientation: {type: 'quatf', value: [1, 2, 3, 4]},
        pair: {type: 'int2', value: [1, -2]},
        triple: {type: 'int3', value: [1, -2, 3]},
        quad: {type: 'int4', value: [1, -2, 3, -4]},
        frame: {type: 'timecode', value: 3.5},
        flags: {type: 'bool[]', value: [true, false, true]},
        colors: {type: 'color3f[]', value: [[0.1, 0.2, 0.3], [0.4, 0.5, 0.6]]}
      }}});
    const typedAssetLegacy = new legacyModule.LightUSDLoaderNative();
    const typedAssetNext = new nextModule.LayerDocument();
    try {
      assert.equal(typedAssetLegacy.loadLayerFromJSON(typedAssetJSON), true,
        typedAssetLegacy.error());
      assert.equal(typedAssetNext.loadJSON(typedAssetJSON).success, true);
      assert.deepEqual(JSON.parse(typedAssetNext.exportJSON().text).metas,
        JSON.parse(typedAssetLegacy.layerToJSON()).metas,
        'typed asset metadata imports the authored path like legacy');
      assert.match(typedAssetNext.exportUSDA().text,
        /asset inputFile = @foo\.usda@/,
        'typed asset metadata remains an asset in USDA');
      const typedDictionaryJSON = JSON.stringify({typeName: 'Layer',
        metas: {customLayerData: {settings: {type: 'dictionary', value: {
          count: {type: 'int', value: 3},
          direction: {type: 'float3', value: [1, 2, 3]},
          file: {type: 'asset', value: {assetPath: 'nested.usda', resolvedPath: ''}},
          nested: {type: 'dictionary', value: {
            enabled: {type: 'bool', value: true}}}}}}}});
      assert.equal(typedAssetLegacy.loadLayerFromJSON(typedDictionaryJSON), true,
        typedAssetLegacy.error());
      const legacyDictionary = JSON.parse(typedAssetLegacy.layerToJSON()).metas;
      assert.equal(legacyDictionary.customLayerData.settings.type, 'dictionary');
      const importedDictionary = typedAssetNext.loadJSON(typedDictionaryJSON);
      assert.equal(importedDictionary.success, true, importedDictionary.error);
      assert.deepEqual(JSON.parse(typedAssetNext.exportJSON().text).metas,
        legacyDictionary, 'typed dictionary wrapper and children match legacy');
      const malformedDictionary = JSON.parse(typedDictionaryJSON);
      malformedDictionary.metas.customLayerData.settings.value.count = 3;
      assert.equal(typedAssetNext.loadJSON(JSON.stringify(malformedDictionary)).success,
        false, 'typed dictionary children require typed value records');
    } finally { typedAssetLegacy.delete(); typedAssetNext.delete(); }
    const halfMetadata = {
      scalar: {type: 'half', value: 0.5},
      pair: {type: 'half2', value: [0.5, 1.5]},
      color: {type: 'color3h', value: [0.25, 0.5, 0.75]},
      orientation: {type: 'quath', value: [1, 0, 0, 0]},
      samples: {type: 'half3[]', value: [[0.5, 1, 1.5], [2, 2.5, 3]]}
    };
    const halfMetadataDoc = new nextModule.LayerDocument();
    try {
      const loadedHalf = halfMetadataDoc.loadJSON(JSON.stringify({typeName: 'Layer',
        metas: {customLayerData: halfMetadata}}));
      assert.equal(loadedHalf.success, true, loadedHalf.error);
      assert.deepEqual(JSON.parse(halfMetadataDoc.exportJSON().text).metas
        .customLayerData, halfMetadata,
      'half metadata retains scalar, role, quaternion and array payloads');
    } finally { halfMetadataDoc.delete(); }
    const propertyMetaUSDA = new TextEncoder().encode([
      '#usda 1.0', 'def Xform "Root" {',
      ' float[] primvars:weights = [0.25, 0.5] (interpolation = "vertex" elementSize = 2 hidden = false renderType = "float" colorSpace = "raw" documentation = "Notes")',
      ' token outputs:surface.connect = </Root.primvars:weights>', '}', ''
    ].join('\n'));
    const propertyMetaLegacy = new legacyModule.LightUSDLoaderNative();
    const propertyMetaNext = new nextModule.LayerDocument();
    try {
      assert.equal(propertyMetaLegacy.loadAsLayerFromBinary(
        propertyMetaUSDA, 'property-meta.usda'), true,
      propertyMetaLegacy.error());
      assert.equal(propertyMetaNext.load(propertyMetaUSDA).success, true);
      const oldProperties = JSON.parse(propertyMetaLegacy.layerToJSON())
        .primSpecs.Root.properties;
      assert.deepEqual(JSON.parse(propertyMetaNext.exportJSON().text)
        .primSpecs.Root.properties, oldProperties,
      'connection emptiness and canonical property metadata match legacy');
      assert.equal(propertyMetaNext.loadJSON(propertyMetaLegacy.layerToJSON())
        .success, true, 'legacy metadata JSON imports into next');
      assert.deepEqual(JSON.parse(propertyMetaNext.exportJSON().text)
        .primSpecs.Root.properties, oldProperties,
      'legacy metadata JSON preserves its authored types on next re-export');
    } finally { propertyMetaLegacy.delete(); propertyMetaNext.delete(); }
    const multilineMetaBytes = new Uint8Array(fs.readFileSync(new URL(
      '../../../tests/usda/attr-meta-multiline-string-000.usda', import.meta.url)));
    const multilineLegacy = new legacyModule.LightUSDLoaderNative();
    const multilineNext = new nextModule.LayerDocument();
    try {
      assert.equal(multilineLegacy.loadAsLayerFromBinary(
        multilineMetaBytes, 'attr-meta-multiline-string-000.usda'), true,
      multilineLegacy.error());
      assert.equal(multilineNext.load(multilineMetaBytes).success, true);
      const legacyMeta = JSON.parse(multilineLegacy.layerToJSON()).primSpecs;
      assert.deepEqual(JSON.parse(multilineNext.exportJSON().text).primSpecs,
        legacyMeta, 'multiline property documentation matches legacy JSON');
      assert.equal(multilineNext.loadJSON(multilineLegacy.layerToJSON()).success,
        true, 'legacy triple-quoted documentation imports into next');
      assert.deepEqual(JSON.parse(multilineNext.exportJSON().text).primSpecs,
        legacyMeta, 'multiline documentation retains its canonical JSON form');
    } finally { multilineLegacy.delete(); multilineNext.delete(); }
    const customSamplesBytes = new Uint8Array(fs.readFileSync(new URL(
      '../../../tests/usda/c-core-animation-queries.usda', import.meta.url)));
    const customSamplesLegacy = new legacyModule.LightUSDLoaderNative();
    const customSamplesNext = new nextModule.LayerDocument();
    try {
      assert.equal(customSamplesLegacy.loadAsLayerFromBinary(
        customSamplesBytes, 'c-core-animation-queries.usda'), true,
      customSamplesLegacy.error());
      assert.equal(customSamplesNext.load(customSamplesBytes).success, true);
      assert.deepEqual(JSON.parse(customSamplesNext.exportJSON().text).primSpecs,
        JSON.parse(customSamplesLegacy.layerToJSON()).primSpecs,
      'custom time-sample-only attributes retain authored property flags');
    } finally { customSamplesLegacy.delete(); customSamplesNext.delete(); }
    const relationshipBytes = new Uint8Array(fs.readFileSync(new URL(
      '../../../tests/usda/c-core-relationships.usda', import.meta.url)));
    const relationshipLegacy = new legacyModule.LightUSDLoaderNative();
    const relationshipNext = new nextModule.LayerDocument();
    try {
      assert.equal(relationshipLegacy.loadAsLayerFromBinary(
        relationshipBytes, 'c-core-relationships.usda'), true,
      relationshipLegacy.error());
      assert.equal(relationshipNext.load(relationshipBytes).success, true);
      assert.deepEqual(JSON.parse(relationshipNext.exportJSON().text).primSpecs,
        JSON.parse(relationshipLegacy.layerToJSON()).primSpecs,
      'multi-target relationships expose the legacy first-target alias');
    } finally { relationshipLegacy.delete(); relationshipNext.delete(); }
    const matrixArray = {type: 'matrix4d[]', value: [
      [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
      [[2, 0, 0, 0], [0, 2, 0, 0], [0, 0, 2, 0], [0, 0, 0, 1]]
    ]};
    const matrixArrayDoc = new nextModule.LayerDocument();
    try {
      const loadedArray = matrixArrayDoc.loadJSON(JSON.stringify({typeName: 'Layer',
        metas: {customLayerData: {matrixArray}}}));
      assert.equal(loadedArray.success, true, loadedArray.error);
      assert.deepEqual(JSON.parse(matrixArrayDoc.exportJSON().text).metas
        .customLayerData.matrixArray, matrixArray,
      'compound matrix arrays retain nested element/row dimensions');
    } finally { matrixArrayDoc.delete(); }
    // The USDC reload retains this array as a lazy crate reference. Its
    // decoded matrix buffer exceeds the Layer JSON working-set allowance even
    // though the retained Layer itself is small.
    const matrixValue = '((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))';
    const largeArrayUSDA = new TextEncoder().encode(
      `#usda 1.0\ndef Xform "Root" { matrix4d[] big = [${Array(270000)
        .fill(matrixValue).join(', ')}] }\n`);
    const largeArraySource = new nextModule.LayerDocument();
    const lazyArrayDoc = new nextModule.LayerDocument();
    try {
      assert.equal(largeArraySource.load(largeArrayUSDA).success, true);
      const crate = largeArraySource.exportUSDC();
      assert.equal(crate.success, true, crate.error);
      assert.equal(lazyArrayDoc.load(crate.data).success, true);
      const rejected = lazyArrayDoc.exportJSON();
      assert.equal(rejected.success, false,
        'lazy decoded matrix cost rejects Layer JSON before materialization');
      assert.match(rejected.error, /Layer JSON output exceeds 512 MiB limit/);
      assert.equal(lazyArrayDoc.exportUSDC().success, true,
        'rejected JSON export leaves the crate-backed layer usable');
    } finally { largeArraySource.delete(); lazyArrayDoc.delete(); }
    const invalidPropertyMetaJSON = structuredClone(oldJSON);
    invalidPropertyMetaJSON.primSpecs.Root.properties.label.attribute.metadata.customData =
      ['unsupported-untyped-array'];
    const invalidPropertyMetaDoc = new nextModule.LayerDocument();
    try {
      const rejected = invalidPropertyMetaDoc.loadJSON(
        JSON.stringify(invalidPropertyMetaJSON));
      assert.equal(rejected.success, false,
        'property dictionary metadata must reject untyped JSON arrays');
    } finally { invalidPropertyMetaDoc.delete(); }
    const unsupportedArcJSON = structuredClone(oldJSON);
    unsupportedArcJSON.primSpecs.Root.references[0].items[0].customData =
      {tag: 'requires-arc-metadata'};
    const unsupportedArcDoc = new nextModule.LayerDocument();
    try {
      const rejected = unsupportedArcDoc.loadJSON(JSON.stringify(unsupportedArcJSON));
      assert.equal(rejected.success, false,
        'reference customData must be rejected until the arc model retains it');
    } finally { unsupportedArcDoc.delete(); }
    assert.equal(oldJSON.primSpecs.Root.properties.label.isCustom, true);
    assert.equal(importedPrimSpecs.Root.properties.label.isCustom, true);
    assert.deepEqual(JSON.parse(jsonNext.exportJSON().text).metas,
      oldJSON.metas, 'legacy Layer JSON metadata fields round-trip through next');
    assert.equal(jsonNext.load(jsonSource).success, true);
    const newJSON = jsonNext.exportJSON();
    assert.equal(newJSON.success, true, newJSON.error);
    const currentJSON = JSON.parse(newJSON.text);
    assert.deepEqual(currentJSON.primSpecs, oldJSON.primSpecs,
      'next layer JSON preserves the legacy prim-spec/property JSON shape');
    assert.deepEqual(currentJSON.metas, originalMetas,
      'next layer JSON preserves authored layer metadata');
    for (const mode of ['base64', 'buffer', 'unknown']) {
      const oldOptioned = JSON.parse(jsonLegacy.layerToJSONWithOptions(true, mode));
      assert.equal(Object.hasOwn(oldOptioned, 'buffers'), false,
        'legacy Layer PrimSpec arrays stay in canonical USD text in each JSON mode');
      const newOptioned = jsonNext.exportJSONWithOptions(true, mode);
      assert.equal(newOptioned.success, true, newOptioned.error);
      const optioned = JSON.parse(newOptioned.text);
      assert.deepEqual(optioned.primSpecs, oldOptioned.primSpecs,
        `next layer JSON preserves the legacy prim-spec schema in ${mode} mode`);
      assert.deepEqual(optioned.metas, oldOptioned.metas,
        `next layer JSON preserves authored metadata in ${mode} mode`);
    }
    const tableJSON = {name: '', typeName: 'Layer',
      buffers: [{byteLength: 3, uri: 'data:application/octet-stream;base64,AQID'}],
      bufferViews: [{buffer: 0, byteOffset: 0, byteLength: 3}],
      accessors: [{bufferView: 0, componentType: 'UNSIGNED_BYTE', count: 3,
        type: 'SCALAR'}]};
    const legacyTables = new legacyModule.LightUSDLoaderNative();
    const nextTables = new nextModule.LayerDocument();
    try {
      assert.equal(legacyTables.loadLayerFromJSON(JSON.stringify(tableJSON)), true,
        legacyTables.error());
      assert.equal(nextTables.loadJSON(JSON.stringify(tableJSON)).success, true);
      for (const change of [
        doc => { doc.buffers[0].byteLength = 4; },
        doc => { doc.buffers[0].uri = 'data:application/octet-stream;base64,AQI!'; },
        doc => { doc.buffers[0].uri = 'asset.bin'; },
        doc => { doc.bufferViews[0].byteOffset = -1; },
        doc => { doc.accessors[0].count = 'three'; },
        doc => { doc.buffers = null; }
      ]) {
        const invalid = structuredClone(tableJSON);
        change(invalid);
        const input = JSON.stringify(invalid);
        assert.equal(legacyTables.loadLayerFromJSON(input), false,
          'legacy rejects malformed optional Layer JSON tables');
        assert.equal(nextTables.loadJSON(input).success, false,
          'next rejects the same malformed optional Layer JSON tables');
      }
    } finally { legacyTables.delete(); nextTables.delete(); }
    // Array edits keep their canonical `edit [...]` text and matrices use the
    // legacy `( (...) )` spelling in Layer JSON; both re-import into next.
    const editSource = new TextEncoder().encode([
      '#usda 1.0', 'def Xform "W" {',
      '  custom int[] ints = edit [write 123 to [0]; append 234; prepend 99; erase [1]; resize 8 fill 7]',
      '  custom string[] names = edit [minsize 4; append "hello"; write "world" to [2]]',
      '  matrix4d xform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, -0.5, 1) )',
      '  matrix2d[] pairs = [( (1, 0), (0, 1) ), ( (2, 0), (0, 2) )]',
      '  matrix3d sampled.timeSamples = { 0: ( (1, 0, 0), (0, 1, 0), (0, 0, 1) ) }',
      '}', ''].join('\n'));
    const editLegacy = new legacyModule.LightUSDLoaderNative();
    const editNext = new nextModule.LayerDocument();
    const editReload = new nextModule.LayerDocument();
    try {
      assert.equal(editLegacy.loadAsLayerFromBinary(editSource, 'edit-parity.usda'), true,
        editLegacy.error());
      assert.equal(editNext.load(editSource).success, true);
      const legacyEdit = JSON.parse(editLegacy.layerToJSON()).primSpecs.W.properties;
      const nextEdit = JSON.parse(editNext.exportJSON().text).primSpecs.W.properties;
      assert.equal(nextEdit.ints.attribute.value,
        'edit [write 123 to [0]; append 234; prepend 99; erase [1]; resize 8 fill 7]');
      assert.equal(nextEdit.pairs.attribute.value, '[( (1, 0), (0, 1) ), ( (2, 0), (0, 2) )]');
      assert.deepEqual(nextEdit, legacyEdit,
        'next Layer JSON matches legacy for array edits and matrix text');
      const reimported = editReload.loadJSON(editLegacy.layerToJSON());
      assert.equal(reimported.success, true, reimported.error);
      assert.deepEqual(JSON.parse(editReload.exportJSON().text).primSpecs.W.properties,
        legacyEdit, 'legacy array-edit/matrix Layer JSON round-trips through next');
      assert.match(editReload.exportUSDA().text,
        /int\[\] ints = edit \[write 123 to \[0\]; append 234; prepend 99; erase \[1\]; resize 8 fill 7\]/);
      const badEdit = structuredClone(JSON.parse(editLegacy.layerToJSON()));
      badEdit.primSpecs.W.properties.ints.attribute.value = 'edit [append "text"]';
      assert.equal(editReload.loadJSON(JSON.stringify(badEdit)).success, false,
        'next rejects array-edit literals of the wrong element type');
      // Legacy drops `.connect` when a default is also authored, exporting a
      // targetless "connection" record; next keeps both and imports that
      // legacy record as a value-only attribute.
      const bothSource = new TextEncoder().encode([
        '#usda 1.0', 'def Shader "S" {',
        '  color3f inputs:c = (0.2, 0.4, 0.6)',
        '  color3f inputs:c.connect = </T.outputs:rgb>', '}', ''].join('\n'));
      const bothLegacy = new legacyModule.LightUSDLoaderNative();
      const bothNext = new nextModule.LayerDocument();
      const bothReload = new nextModule.LayerDocument();
      try {
        assert.equal(bothLegacy.loadAsLayerFromBinary(bothSource, 'both.usda'), true,
          bothLegacy.error());
        assert.equal(bothNext.load(bothSource).success, true);
        const legacyBoth = JSON.parse(bothLegacy.layerToJSON()).primSpecs.S.properties['inputs:c'];
        const nextBoth = JSON.parse(bothNext.exportJSON().text).primSpecs.S.properties['inputs:c'];
        assert.equal(legacyBoth.attribute.connection, undefined);
        assert.equal(nextBoth.attribute.connection, '/T.outputs:rgb');
        assert.equal(nextBoth.attribute.value, legacyBoth.attribute.value);
        const imported = bothReload.loadJSON(bothLegacy.layerToJSON());
        assert.equal(imported.success, true, imported.error);
        const reloaded = JSON.parse(bothReload.exportJSON().text).primSpecs.S.properties['inputs:c'];
        assert.equal(reloaded.propertyType, 'attribute');
        assert.equal(reloaded.attribute.value, '(0.2, 0.4, 0.6)');
        const valueless = structuredClone(JSON.parse(bothLegacy.layerToJSON()));
        const record = valueless.primSpecs.S.properties['inputs:c'].attribute;
        record.hasValue = false; record.value = null; record.valueType = 'empty';
        assert.equal(bothReload.loadJSON(JSON.stringify(valueless)).success, false,
          'a targetless connection without a value is rejected');
      } finally { bothLegacy.delete(); bothNext.delete(); bothReload.delete(); }
    } finally { editLegacy.delete(); editNext.delete(); editReload.delete(); }
  } finally { jsonLegacy.delete(); jsonNext.delete(); }
  const progressEvents = [];
  const progressDoc = new nextModule.LayerDocument();
  const profileDoc = new nextModule.LayerDocument();
  const legacyProfileDoc = new legacyModule.LightUSDLoaderNative();
  const shadingDoc = new nextModule.LayerDocument();
  const legacyShadingDoc = new legacyModule.LightUSDLoaderNative();
  try {
    assert.equal(profileDoc.getMhProfileJSON(), '[]');
    assert.equal(profileDoc.getShadingGraphJSON(), '');
    assert.throws(() => profileDoc.getMhProfileJSON(0), TypeError);
    const profileSource = new TextEncoder().encode(`#usda 1.0
def SkelRoot "Rig" {
  custom string mh:label = "顔"
  custom rel mh:skeleton = </Rig/Skeleton>
  def Skeleton "Skeleton" {
    custom float[] mh:rig:guiControlValues.timeSamples = {
      0: [0, 1],
      1: [2, 3],
    }
  }
}
`);
    const profileLoad = profileDoc.load(profileSource);
    assert.equal(profileLoad.success, true, profileLoad.error);
    assert.equal(legacyProfileDoc.loadAsLayerFromBinary(profileSource,
      'mh-profile-parity.usda'), true, legacyProfileDoc.error());
    const profile = JSON.parse(profileDoc.getMhProfileJSON());
    assert.deepEqual(profile.map(record => record.path), ['/Rig', '/Rig/Skeleton']);
    assert.equal(profile[0].attrs['mh:label'], '顔');
    assert.deepEqual(profile[0].rels['mh:skeleton'], ['/Rig/Skeleton']);
    assert.deepEqual(profile[1].attrs['mh:rig:guiControlValues'], {
      timeSamples: [{t: 0, v: [0, 1]}, {t: 1, v: [2, 3]}]
    });
    assert.deepEqual(JSON.parse(profileDoc.getMhProfileJSON()),
      JSON.parse(legacyProfileDoc.getMhProfileJSON()),
      'next authored profile JSON matches the legacy schema and values');
    const shadingSource = new TextEncoder().encode(`#usda 1.0
def Material "Mat" (
  prepend apiSchemas = ["ColorSpaceAPI"]
) {
  token colorSpace:name = "lin_rec709_scene"
  token outputs:surface.connect = </Mat/Shader.outputs:surface>
  def Shader "Shader" {
  uniform token info:id = "UsdPreviewSurface"
    color3f inputs:diffuseColor = (0.25, 0.5, 1) (colorSpace = "srgb_texture")
    asset inputs:file = @textures/face.png@
    custom rel source:material = </Mat>
    token outputs:surface
  }
}
`);
    assert.equal(shadingDoc.load(shadingSource).success, true);
    assert.equal(legacyShadingDoc.loadAsLayerFromBinary(shadingSource,
      'shading-graph-parity.usda'), true, legacyShadingDoc.error());
    const shadingText = shadingDoc.getShadingGraphJSON();
    const shading = JSON.parse(shadingText);
    assert.equal(shading.version, 1);
    assert.deepEqual(shading.prims.map(record => record.path), ['/Mat', '/Mat/Shader']);
    assert.deepEqual(shading.prims[0].properties['outputs:surface'].connections,
      ['/Mat/Shader.outputs:surface']);
    assert.deepEqual(shading.prims[1].properties['inputs:diffuseColor'].value,
      [0.25, 0.5, 1]);
    assert.equal(shading.prims[1].properties['inputs:diffuseColor'].colorSpace,
      'srgb_texture');
    assert.deepEqual(shading.prims[1].properties['source:material'].targets, ['/Mat']);
    assert.equal(shading.assetPaths[0].authored, 'textures/face.png');
    assert.deepEqual(shading, JSON.parse(legacyShadingDoc.getShadingGraphJSON()),
      'next authored shading graph JSON matches the legacy structure and values');
    const packageAssets = new nextModule.NextAssetStore();
    const legacyAsset = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10, 1, 2, 3]);
    packageAssets.setAsset('textures/face.png', legacyAsset);
    legacyShadingDoc.setAsset('textures/face.png', legacyAsset);
    try {
      for (const rootLayerFormat of ['usda', 'usdc']) {
        const oldPackage = legacyShadingDoc.exportLayerAsUSDZWithOptions({rootLayerFormat});
        const nextPackage = shadingDoc.exportUSDZ({rootLayerFormat}, packageAssets);
        assert.ok(oldPackage?.length > 0, legacyShadingDoc.error());
        assert.equal(nextPackage.success, true, nextPackage.error);
        for (const [label, data] of [['legacy', oldPackage], ['next', nextPackage.data]]) {
          const entries = readStoredZipEntries(data);
          const rootName = rootLayerFormat === 'usda' ? 'root.usda' : 'root.usdc';
          assert.ok(entries.get(rootName)?.length > 0, `${label}: expected ${rootName}`);
          assert.deepEqual(entries.get('textures/face.png'), legacyAsset,
            `${label}: packaged asset bytes match the store`);
        }
        const oldEntries = readStoredZipEntries(oldPackage);
        const nextEntries = readStoredZipEntries(nextPackage.data);
        const oldRoot = new nextModule.LayerDocument();
        const nextRoot = new nextModule.LayerDocument();
        try {
          assert.equal(oldRoot.load(oldEntries.get(rootLayerFormat === 'usda'
            ? 'root.usda' : 'root.usdc')).success, true);
          assert.equal(nextRoot.load(nextEntries.get(rootLayerFormat === 'usda'
            ? 'root.usda' : 'root.usdc')).success, true);
          assert.equal(oldRoot.exportUSDA().text, nextRoot.exportUSDA().text,
            `${rootLayerFormat}: authored root layer round-trips equally`);
        } finally { oldRoot.delete(); nextRoot.delete(); }
      }
    } finally { packageAssets.delete(); }
    const progressResult = progressDoc.loadWithProgress(bytes, event => {
      progressEvents.push(event);
    });
    assert.equal(progressResult.success, true, progressResult.error);
    assert.ok(progressEvents.length > 0, 'USDA load reports parser progress');
    assert.ok(progressEvents.every(event => event.phase && event.total >= event.current));
    const beforeCancel = progressDoc.exportUSDA();
    const cancelled = progressDoc.loadWithProgress(bytes, () => false);
    assert.equal(cancelled.success, false, 'callback false cancels parsing');
    assert.equal(progressDoc.exportUSDA().data, beforeCancel.data,
      'cancelled load leaves the previously loaded document intact');
    let callbackCalls = 0;
    assert.throws(() => progressDoc.loadWithProgress(bytes, () => {
      ++callbackCalls;
      throw new Error('progress callback failure');
    }), /progress callback failure/);
    assert.equal(callbackCalls, 1, 'callback exceptions are delivered once');
    assert.equal(progressDoc.exportUSDA().data, beforeCancel.data,
      'callback exception leaves the previously loaded document intact');
  } finally {
    legacyShadingDoc.delete(); shadingDoc.delete(); legacyProfileDoc.delete();
    profileDoc.delete(); progressDoc.delete();
  }
  const legacyBytes = legacyLoader.exportLayerAsUSDCWithOptions({});
  assert.ok(legacyBytes?.length > 0, legacyLoader.error());
  const tooSmallLegacy = new Uint8Array(legacyBytes.length - 1).fill(0xa5);
  const tooSmallNext = new Uint8Array(legacyBytes.length - 1).fill(0xa5);
  const legacySmallResult = legacyLoader.exportLayerAsUSDCToBufferWithOptions(tooSmallLegacy, {});
  const nextSmallResult = nextDocument.exportUSDCToBuffer(tooSmallNext, {});
  assert.equal(legacySmallResult.success, false);
  assert.equal(nextSmallResult.success, false);
  assert.equal(nextSmallResult.error, legacySmallResult.error);
  assert.deepEqual(tooSmallNext, new Uint8Array(legacyBytes.length - 1).fill(0xa5),
    'too-small next output buffer remains unchanged');
  assert.deepEqual(tooSmallLegacy, new Uint8Array(legacyBytes.length - 1).fill(0xa5),
    'too-small legacy output buffer remains unchanged');
  for (const [badBuffer, expectedError] of [
    [null, 'USDC export output buffer is null.'],
    [{}, 'USDC export output must be a Uint8Array.'],
    [new Uint8Array(0), 'USDC export output buffer is empty.']
  ]) {
    const oldResult = legacyLoader.exportLayerAsUSDCToBufferWithOptions(badBuffer, {});
    const newResult = nextDocument.exportUSDCToBuffer(badBuffer, {});
    assert.equal(oldResult.error, expectedError);
    assert.equal(newResult.error, oldResult.error);
  }
  const nextExport = nextDocument.exportUSDC();
  assert.equal(nextExport.success, true, nextExport.error);
  assert.ok(nextExport.data.length > 0);
  const crateProgress = [];
  const crateProgressResult = nextReload.loadWithProgress(nextExport.data, event => {
    crateProgress.push(event);
  });
  assert.equal(crateProgressResult.success, true, crateProgressResult.error);
  assert.ok(crateProgress.length > 0, 'USDC load reports crate progress');
  const legacyOutputBuffer = new Uint8Array(legacyBytes.length + 4).fill(0xcc);
  const nextOutputBuffer = new Uint8Array(nextExport.data.length + 4).fill(0xcc);
  const legacyBufferResult = legacyLoader.exportLayerAsUSDCToBufferWithOptions(
    legacyOutputBuffer, {});
  const nextBufferResult = nextDocument.exportUSDCToBuffer(nextOutputBuffer, {});
  assert.equal(legacyBufferResult.success, true, legacyBufferResult.error);
  assert.equal(nextBufferResult.success, true, nextBufferResult.error);
  assert.equal(nextBufferResult.size, nextExport.data.length);
  assert.deepEqual(nextOutputBuffer.subarray(0, nextBufferResult.size), nextExport.data);
  assert.deepEqual(nextOutputBuffer.subarray(nextBufferResult.size), new Uint8Array(4).fill(0xcc),
    'next output buffer tail remains unchanged');
  assert.deepEqual(legacyOutputBuffer.subarray(0, legacyBufferResult.size), legacyBytes);
  const oldReload = legacyReload.load(legacyBytes);
  assert.equal(oldReload.success, true, oldReload.error);
  const newReload = nextReload.load(nextExport.data);
  assert.equal(newReload.success, true, newReload.error);
  const legacyUSDA = legacyReload.exportUSDA();
  const nextUSDA = nextReload.exportUSDA();
  assert.equal(legacyUSDA.success, true, legacyUSDA.error);
  assert.equal(nextUSDA.success, true, nextUSDA.error);
  assert.equal(nextUSDA.text, legacyUSDA.text,
    'optioned legacy layer USDC and next LayerDocument USDC must reload identically');
  const nextStream = new nextModule.RenderStream();
  try {
    const streamLoad = nextStream.begin(bytes);
    assert.equal(streamLoad.success, true, streamLoad.error || nextStream.error());
    const legacyStageBytes = legacyLoader.exportAsUSDC();
    assert.ok(legacyStageBytes?.length > 0, legacyLoader.error());
    for (const [target, expectedError] of [
      [null, 'USDC export output buffer is null.'],
      [{}, 'USDC export output must be a Uint8Array.'],
      [new Uint8Array(0), 'USDC export output buffer is empty.']
    ]) {
      const oldResult = legacyLoader.exportStageAsUSDCToBufferWithOptions(target, {});
      const newResult = nextStream.exportUSDCToBuffer(target, {});
      assert.equal(oldResult.error, expectedError);
      assert.equal(newResult.error, oldResult.error);
    }
    const oldSmall = new Uint8Array(legacyStageBytes.length - 1).fill(0x5a);
    const newSmall = new Uint8Array(legacyStageBytes.length - 1).fill(0x5a);
    const oldSmallResult = legacyLoader.exportStageAsUSDCToBufferWithOptions(oldSmall, {});
    const newSmallResult = nextStream.exportUSDCToBuffer(newSmall, {});
    assert.equal(oldSmallResult.error, 'USDC export output buffer too small.');
    assert.equal(newSmallResult.error, oldSmallResult.error);
    assert.deepEqual(newSmall, new Uint8Array(legacyStageBytes.length - 1).fill(0x5a),
      'failed next Stage export leaves caller bytes untouched');
    const stageOutputCapacity = legacyStageBytes.length * 2 + 4;
    const oldStageOutput = new Uint8Array(stageOutputCapacity).fill(0xcc);
    const newStageOutput = new Uint8Array(stageOutputCapacity).fill(0xcc);
    const oldStageResult = legacyLoader.exportStageAsUSDCToBufferWithOptions(oldStageOutput, {});
    const newStageResult = nextStream.exportUSDCToBuffer(newStageOutput, {});
    assert.equal(oldStageResult.success, true, oldStageResult.error);
    assert.equal(newStageResult.success, true, newStageResult.error);
    assert.deepEqual(newStageOutput.subarray(newStageResult.size),
      new Uint8Array(stageOutputCapacity - newStageResult.size).fill(0xcc));
    assert.deepEqual(oldStageOutput.subarray(oldStageResult.size),
      new Uint8Array(stageOutputCapacity - oldStageResult.size).fill(0xcc));
    const oldStageReload = new nextModule.LayerDocument();
    const newStageReload = new nextModule.LayerDocument();
    try {
      assert.equal(oldStageReload.load(oldStageOutput.subarray(0, oldStageResult.size)).success, true);
      assert.equal(newStageReload.load(newStageOutput.subarray(0, newStageResult.size)).success, true);
      const oldStageUSDA = oldStageReload.exportUSDA();
      const newStageUSDA = newStageReload.exportUSDA();
      assert.equal(oldStageUSDA.success, true, oldStageUSDA.error);
      assert.equal(newStageUSDA.success, true, newStageUSDA.error);
      assert.equal(newStageUSDA.text, oldStageUSDA.text,
        'Stage-oriented legacy USDC and next RenderStream USDC must reload identically');
    } finally { oldStageReload.delete(); newStageReload.delete(); }
  } finally { nextStream.delete(); }
} finally {
  legacyLoader.delete(); nextDocument.delete(); legacyReload.delete(); nextReload.delete();
}
console.log('ok - legacy optioned layer USDC export and caller-buffer contract match NextLayerDocument workflow');
