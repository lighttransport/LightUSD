// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
// Legacy returns size_t asset counts/sizes as Numbers on wasm32 and BigInts on memory64.
const cacheSize = value => wasm64 ? BigInt(value) : value;
const {default: createModule} = await import(wasm64
  ? '../src/lightusd/lightusd_next_64.js'
  : '../src/lightusd/lightusd_next.js');
const module = await createModule();
const encode = text => new TextEncoder().encode(text);
const assertAggregateHeadroomRejectsBeforeBuild = (stream, aggregateMethod,
  itemMethod, itemCount) => {
  assert.ok(itemCount > 0, `${aggregateMethod} fixture must contain records`);
  const remaining = module._lightusd_next_render_remaining_memory_bytes;
  let built = 0;
  module._lightusd_next_render_remaining_memory_bytes = () => 0;
  Object.defineProperty(stream, itemMethod, {
    configurable: true, value: () => { ++built; return {}; }
  });
  try {
    assert.throws(() => stream[aggregateMethod](), /aggregate exceeds remaining memory limit/);
    assert.equal(built, 0, `${aggregateMethod} must preflight before building records`);
  } finally {
    module._lightusd_next_render_remaining_memory_bytes = remaining;
    delete stream[itemMethod];
  }
};
const layerDocument = new module.LayerDocument();
assert.deepEqual(Object.getOwnPropertyNames(module.LayerDocument.prototype)
  .filter(name => name !== 'constructor').sort(),
JSON.parse(fs.readFileSync(new URL('./next-layer-document-api-inventory.json', import.meta.url), 'utf8')));
const authoredLayer = encode('#usda 1.0\n\ndef Xform "Root" { custom string label = "hi" (displayName = "Label") }\n');
const authoredLayerPtr = module['_lightusd_next_alloc'](authoredLayer.length);
assert.ok(authoredLayerPtr);
module.HEAPU8.set(authoredLayer, Number(authoredLayerPtr));
const authoredLayerHeapView = module.HEAPU8.subarray(
  Number(authoredLayerPtr), Number(authoredLayerPtr) + authoredLayer.length);
const originalNextAlloc = module['_lightusd_next_alloc'];
const originalNextFree = module['_lightusd_next_free'];
let layerLoadGrewMemory = false;
module['_lightusd_next_alloc'] = size => {
  if (!layerLoadGrewMemory) {
    const oldBuffer = module.HEAPU8.buffer;
    const pressure = originalNextAlloc(module.HEAPU8.byteLength);
    layerLoadGrewMemory = module.HEAPU8.buffer !== oldBuffer;
    originalNextFree(pressure);
  }
  return originalNextAlloc(size);
};
let layerLoad;
try { layerLoad = layerDocument.load(authoredLayerHeapView); }
finally { module['_lightusd_next_alloc'] = originalNextAlloc; }
assert.equal(layerLoadGrewMemory, true,
  'heap-backed LayerDocument input must survive memory growth before staging');
assert.deepEqual(layerLoad, {success: true, primCount: 1});
module['_lightusd_next_free'](authoredLayerPtr);
assert.equal(layerDocument.loaded(), true);
const layerJSON = layerDocument.exportJSON();
assert.equal(layerJSON.success, true, layerJSON.error);
const parsedLayerJSON = JSON.parse(layerJSON.text);
assert.equal(parsedLayerJSON.typeName, 'Layer');
assert.equal(parsedLayerJSON.primSpecs.Root.typeName, 'Xform');
const layerJSONReload = new module.LayerDocument();
try {
  assert.deepEqual(layerJSONReload.loadJSON(layerJSON.text), {success: true, primCount: 1});
  const allowedTokenJSON = JSON.parse(layerJSON.text);
  allowedTokenJSON.primSpecs.Root.properties.label.attribute.metadata ??= {};
  allowedTokenJSON.primSpecs.Root.properties.label.attribute.metadata.allowedTokens = ['red', 'blue'];
  allowedTokenJSON.metas ??= {};
  allowedTokenJSON.metas.customLayerData = {
    quality: 3,
    nested: {enabled: true, label: 'fixture'},
    samples: {type: 'float[]', value: [0.25, 1.5]}
  };
  allowedTokenJSON.metas.expressionVariables = {asset: './mesh.usd'};
  allowedTokenJSON.metas.hasOwnedSubLayers = true;
  allowedTokenJSON.metas.primChildren = ['Root'];
  allowedTokenJSON.metas.subLayers = [{assetPath: './child.usda',
    layerOffset: {offset: 2.5, scale: 0.5}}];
  allowedTokenJSON.metas.layerRelocates = [{source: '/Old', target: '/New'}];
  allowedTokenJSON.metas.unregisteredMetas = {pipelineTag: 'reviewed'};
  assert.equal(layerJSONReload.loadJSON(JSON.stringify(allowedTokenJSON)).success, true);
  const metadataRoundTrip = JSON.parse(layerJSONReload.exportJSON().text);
  assert.deepEqual(metadataRoundTrip.primSpecs.Root.properties.label.attribute.metadata.allowedTokens,
    ['red', 'blue']);
  assert.deepEqual(metadataRoundTrip.metas.customLayerData,
    allowedTokenJSON.metas.customLayerData);
  assert.deepEqual(metadataRoundTrip.metas.expressionVariables,
    allowedTokenJSON.metas.expressionVariables);
  assert.equal(metadataRoundTrip.metas.hasOwnedSubLayers, true);
  assert.deepEqual(metadataRoundTrip.metas.primChildren, ['Root']);
  assert.deepEqual(metadataRoundTrip.metas.subLayers, allowedTokenJSON.metas.subLayers);
  assert.deepEqual(metadataRoundTrip.metas.layerRelocates,
    allowedTokenJSON.metas.layerRelocates);
  assert.deepEqual(metadataRoundTrip.metas.unregisteredMetas,
    allowedTokenJSON.metas.unregisteredMetas);
  assert.equal(layerJSONReload.loadJSON('{').success, false);
  assert.equal(layerJSONReload.loaded(), false,
    'failed JSON imports must clear the previously loaded Layer, matching legacy');
  assert.ok(layerJSONReload.error());
} finally { layerJSONReload.delete(); }
assert.throws(() => layerDocument.exportJSONWithOptions(true), /expected a boolean/);
assert.throws(() => layerDocument.exportJSONWithOptions(1, 'buffer'), /expected a boolean/);
assert.throws(() => layerDocument.exportJSONWithOptions(true, {}), /expected a boolean/);
assert.throws(() => layerDocument.exportJSON('extra'), /wrong argument count/);
const commentMetadataLayer = new module.LayerDocument();
const commentMetadataReload = new module.LayerDocument();
try {
  assert.equal(commentMetadataLayer.load(authoredLayer).success, true);
  assert.deepEqual(commentMetadataLayer.setStageMetadata('comment', 'authored comment'),
    {success: true});
  assert.deepEqual(commentMetadataLayer.getStageMetadata('comment'),
    {success: true, authored: true, value: 'authored comment'});
  const commentExport = commentMetadataLayer.exportUSDA();
  assert.equal(commentExport.success, true, commentExport.error);
  assert.equal(commentMetadataReload.load(encode(commentExport.text)).success, true);
  assert.equal(commentMetadataReload.getStageMetadata('comment').authored, true);
} finally {
  commentMetadataLayer.delete();
  commentMetadataReload.delete();
}
assert.deepEqual(layerDocument.setStageMetadata('defaultPrim', 'Root'), {success: true});
assert.deepEqual(layerDocument.setStageMetadata('upAxis', 'Y'), {success: true});
assert.deepEqual(layerDocument.setStageMetadata('metersPerUnit', 0.01), {success: true});
assert.deepEqual(layerDocument.setStageMetadata('doc', 'core document'), {success: true});
assert.deepEqual(layerDocument.setStageMetadata('colorConfiguration', 'config.ocio'), {success: true});
assert.deepEqual(layerDocument.setStageMetadata('colorManagementSystem', 'ocio'), {success: true});
assert.deepEqual(layerDocument.getStageMetadata('defaultPrim'),
  {success: true, authored: true, value: 'Root'});
assert.deepEqual(layerDocument.getStageMetadata('upAxis'),
  {success: true, authored: true, value: 'Y'});
assert.deepEqual(layerDocument.getStageMetadata('metersPerUnit'),
  {success: true, authored: true, value: 0.01});
assert.deepEqual(layerDocument.getStageMetadata('doc'),
  {success: true, authored: true, value: 'core document'});
assert.deepEqual(layerDocument.getStageMetadata('colorConfiguration'),
  {success: true, authored: true, value: 'config.ocio'});
assert.deepEqual(layerDocument.getStageMetadata('colorManagementSystem'),
  {success: true, authored: true, value: 'ocio'});
assert.equal(layerDocument.getStageMetadata('endTimeCode').authored, false);
assert.equal(layerDocument.setStageMetadata('upAxis', 'Q').success, false);
assert.throws(() => layerDocument.setStageMetadata('unknownMetadata', 'x'), /unsupported key/);
assert.throws(() => layerDocument.setStageMetadata('metersPerUnit', '0.01'), /value type/);
assert.throws(() => layerDocument.getStageMetadata('unknownMetadata'), /unsupported key/);
assert.deepEqual(layerDocument.definePrim('/Root/Added', 'Scope'), {success: true});
assert.deepEqual(layerDocument.setPrimMetadata('/Root/Added', 'active', false), {success: true});
assert.deepEqual(layerDocument.setPrimMetadata('/Root/Added', 'hidden', true), {success: true});
assert.deepEqual(layerDocument.setPrimMetadata('/Root/Added', 'instanceable', true), {success: true});
assert.deepEqual(layerDocument.setPrimMetadata('/Root/Added', 'kind', 'component'), {success: true});
assert.deepEqual(layerDocument.setPrimMetadata('/Root/Added', 'displayName', 'Added prim'), {success: true});
assert.deepEqual(layerDocument.setPrimMetadata('/Root/Added', 'apiSchemas',
  ['UsdGeomModelAPI', 'UsdSomethingAPI']), {success: true});
assert.deepEqual(layerDocument.getPrimMetadata('/Root/Added', 'active'),
  {success: true, authored: true, value: false});
assert.deepEqual(layerDocument.getPrimMetadata('/Root/Added', 'instanceable'),
  {success: true, authored: true, value: true});
assert.deepEqual(layerDocument.getPrimMetadata('/Root/Added', 'kind'),
  {success: true, authored: true, value: 'component'});
assert.deepEqual(layerDocument.getPrimMetadata('/Root/Added', 'displayName'),
  {success: true, authored: true, value: 'Added prim'});
assert.deepEqual(layerDocument.getPrimMetadata('/Root/Added', 'apiSchemas'),
  {success: true, authored: true, value: ['UsdGeomModelAPI', 'UsdSomethingAPI']});
assert.deepEqual(layerDocument.getPrimMetadata('/Root', 'hidden'),
  {success: true, authored: false});
assert.deepEqual(layerDocument.setPrimMetadata('/Root/Added', 'doc', ''), {success: true});
assert.deepEqual(layerDocument.getPrimMetadata('/Root/Added', 'doc'),
  {success: true, authored: true, value: ''});
assert.throws(() => layerDocument.getPrimMetadata('/Root/Added', 'unknown'),
  /unsupported key/);
assert.throws(() => layerDocument.setPrimMetadata('/Root/Added', 'hidden', 'true'),
  /expected boolean value/);
assert.throws(() => layerDocument.setPrimMetadata('/Root/Added', 'unknown', true),
  /unsupported key/);
assert.deepEqual(layerDocument.setStringAttribute('/Root/Added', 'label', 'created'), {success: true});
assert.deepEqual(layerDocument.setNumberAttribute('/Root/Added', 'weight', 2.5), {success: true});
assert.deepEqual(layerDocument.setAttributeMetadata('/Root/Added', 'label', 'doc', 'label documentation'),
  {success: true});
assert.deepEqual(layerDocument.setAttributeMetadata('/Root/Added', 'label', 'displayGroup', 'Identity'),
  {success: true});
assert.deepEqual(layerDocument.setAttributeMetadata('/Root/Added', 'position', 'weight', 0.75),
  {success: true});
assert.deepEqual(layerDocument.setAttributeMetadata('/Root/Added', 'position', 'elementSize', 3),
  {success: true});
assert.deepEqual(layerDocument.setAttributeMetadata('/Root/Added', 'position', 'interpolation', 'vertex'),
  {success: true});
assert.deepEqual(layerDocument.setAttributeMetadata('/Root/Added', 'position', 'hidden', true),
  {success: true});
assert.deepEqual(layerDocument.getAttributeMetadata('/Root/Added', 'label', 'doc'),
  {success: true, value: 'label documentation'});
assert.deepEqual(layerDocument.getAttributeMetadata('/Root/Added', 'label', 'displayGroup'),
  {success: true, value: 'Identity'});
assert.deepEqual(layerDocument.getAttributeMetadata('/Root/Added', 'position', 'weight'),
  {success: true, value: 0.75});
assert.deepEqual(layerDocument.getAttributeMetadata('/Root/Added', 'position', 'elementSize'),
  {success: true, value: 3});
assert.deepEqual(layerDocument.getAttributeMetadata('/Root/Added', 'position', 'interpolation'),
  {success: true, value: 'vertex'});
assert.deepEqual(layerDocument.getAttributeMetadata('/Root/Added', 'position', 'hidden'),
  {success: true, value: true});
assert.deepEqual(layerDocument.setAttributeMetadata('/Root/Added', 'label', 'displayName', ''),
  {success: true});
assert.deepEqual(layerDocument.getAttributeMetadata('/Root/Added', 'label', 'displayName'),
  {success: true, value: ''});
assert.equal(layerDocument.getAttributeMetadata('/Root/Added', 'label', 'colorSpace').success, false,
  'unauthored property metadata is reported as absent');
assert.throws(() => layerDocument.setAttributeMetadata('/Root/Added', 'label', 'elementSize', 1.5),
  /signed 32-bit integer/);
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'visible', 'bool', true), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'indices', 'int', new Int32Array([2, 4, 6])), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'position', 'point3f', [1, 2, 3]), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'positionD', 'point3d', [4, 5, 6]), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'uv', 'texCoord2d', [0.125, 0.875]), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'orientation', 'quatd', [1, 0, 0, 0]), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'roughness', 'half', 0.25), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'tint', 'color3h', [0.25, 0.5, 0.75]), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'weights', 'half', new Float32Array([0.25, 0.5])), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'samples', 'double', new Float64Array([0.25, 0.5])), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'title', 'token', 'typed'), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'labels', 'string', ['first', '', 'third']), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'tags', 'token', ['hero', 'render']), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'textures', 'asset', ['textures/a.png', 'textures/b.png']), {success: true});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'emptyLabels', 'string', []), {success: true});
assert.deepEqual(layerDocument.setRelationshipTargets('/Root/Added', 'links',
  ['/Root', '/Root/Added']), {success: true});
assert.deepEqual(layerDocument.setRelationshipTargets('/Root/Added', 'links',
  ['/Root']), {success: true});
assert.deepEqual(layerDocument.getRelationshipTargets('/Root/Added', 'links'),
  {success: true, targets: ['/Root']});
assert.throws(() => layerDocument.getRelationshipTargets('/Root/Added'),
  /expected NUL-free path and name/);
assert.equal(layerDocument.getRelationshipTargets('/Root/Added', 'missing').success, false);
assert.throws(() => layerDocument.setRelationshipTargets('/Root/Added', 'links', ['/Root\0bad']),
  /NUL-free target strings/);
assert.throws(() => layerDocument.setRelationshipTargets('/Root/Added', 'links',
  new Array(65537).fill('/Root')), /target count exceeds 65536/);
assert.deepEqual(layerDocument.removeAttribute('/Root/Added', 'weight'), {success: true});
assert.deepEqual(layerDocument.removeAttribute('/Root/Added', 'missing'), {success: false, error: 'no property: missing'});
assert.deepEqual(layerDocument.setAttribute('/Root/Added', 'wide', 'uint64', 9007199254740993n), {success: true});
assert.throws(() => layerDocument.setAttribute('/Root/Added', 'bad', 'int', [1.5]), /out of range/);
assert.throws(() => layerDocument.setAttribute('/Root/Added', 'badHalf', 'half', 65520), /out of range/);
assert.equal(layerDocument.primCount(), 2);
const nextAlloc = module['_lightusd_next_alloc'];
let exportStagingAllocations = 0;
module['_lightusd_next_alloc'] = size => {
  ++exportStagingAllocations;
  return nextAlloc(size);
};
let editedLayer;
try { editedLayer = layerDocument.exportUSDA(); }
finally { module['_lightusd_next_alloc'] = nextAlloc; }
assert.equal(exportStagingAllocations, 0,
  'USDA export copies directly from the retained C output into JS ownership');
assert.equal(editedLayer.success, true, editedLayer.error);
assert.match(editedLayer.text, /defaultPrim = "Root"/);
assert.match(editedLayer.text, /upAxis = "Y"/);
assert.match(editedLayer.text, /metersPerUnit = 0\.01/);
assert.match(editedLayer.text, /doc = "core document"/);
assert.match(editedLayer.text, /string label = "created"/);
assert.match(editedLayer.text, /doc = "label documentation"/);
assert.match(editedLayer.text, /displayGroup = "Identity"/);
assert.match(editedLayer.text, /weight = 0\.75/);
assert.match(editedLayer.text, /elementSize = 3/);
assert.match(editedLayer.text, /interpolation = "vertex"/);
assert.match(editedLayer.text, /hidden = true/);
assert.doesNotMatch(editedLayer.text, /double weight = 2\.5/);
assert.match(editedLayer.text, /bool visible = 1/);
assert.match(editedLayer.text, /int\[\] indices = \[2, 4, 6\]/);
assert.match(editedLayer.text, /point3f position = \(1, 2, 3\)/);
assert.match(editedLayer.text, /point3d positionD = \(4, 5, 6\)/);
assert.match(editedLayer.text, /texCoord2d uv = \(0\.125, 0\.875\)/);
assert.match(editedLayer.text, /quatd orientation = \(1, 0, 0, 0\)/);
assert.match(editedLayer.text, /half roughness = 0\.25/);
assert.match(editedLayer.text, /color3h tint = \(0\.25, 0\.5, 0\.75\)/);
assert.match(editedLayer.text, /half\[\] weights = \[0\.25, 0\.5\]/);
assert.match(editedLayer.text, /double\[\] samples = \[0\.25, 0\.5\]/);
assert.match(editedLayer.text, /token title = "typed"/);
assert.match(editedLayer.text, /string\[\] labels = \["first", "", "third"\]/);
assert.match(editedLayer.text, /token\[\] tags = \["hero", "render"\]/);
assert.match(editedLayer.text, /asset\[\] textures = \[@textures\/a\.png@, @textures\/b\.png@\]/);
assert.match(editedLayer.text, /string\[\] emptyLabels = \[\]/);
assert.match(editedLayer.text, /rel links = <\/Root>/);
assert.doesNotMatch(editedLayer.text, /<\/Root\/Added>/);
assert.match(editedLayer.text, /uint64 wide = 9007199254740993/);
assert.match(editedLayer.text, /active = false/);
assert.match(editedLayer.text, /hidden = true/);
assert.match(editedLayer.text, /instanceable = true/);
assert.match(editedLayer.text, /kind = "component"/);
assert.match(editedLayer.text, /displayName = "Added prim"/);
assert.match(editedLayer.text, /apiSchemas = \["UsdGeomModelAPI", "UsdSomethingAPI"\]/);
const invalidLayer = layerDocument.load(encode('#usda 1.0\ndef Xform {'));
assert.equal(invalidLayer.success, false);
assert.equal(layerDocument.primCount(), 2,
  'a failed replacement load must preserve the previously edited layer');
const reloadLayer = new module.LayerDocument();
try {
  const reloaded = reloadLayer.load(encode(editedLayer.text));
  assert.deepEqual(reloaded, {success: true, primCount: 2});
  assert.equal(reloadLayer.getStageMetadata('defaultPrim').authored, true);
  assert.equal(reloadLayer.getStageMetadata('colorConfiguration').authored, true);
  const roundTrippedLayer = reloadLayer.exportUSDA();
  assert.equal(roundTrippedLayer.success, true, roundTrippedLayer.error);
  assert.match(roundTrippedLayer.text, /string\[\] labels = \["first", "", "third"\]/);
  assert.match(roundTrippedLayer.text, /token\[\] tags = \["hero", "render"\]/);
  assert.match(roundTrippedLayer.text, /asset\[\] textures = \[@textures\/a\.png@, @textures\/b\.png@\]/);
  assert.match(roundTrippedLayer.text, /string\[\] emptyLabels = \[\]/);
  assert.match(roundTrippedLayer.text, /rel links = <\/Root>/);
  assert.deepEqual(reloadLayer.getRelationshipTargets('/Root/Added', 'links'),
    {success: true, targets: ['/Root']});
  assert.match(roundTrippedLayer.text, /half roughness = 0\.25/);
  assert.match(roundTrippedLayer.text, /color3h tint = \(0\.25, 0\.5, 0\.75\)/);
  assert.match(roundTrippedLayer.text, /half\[\] weights = \[0\.25, 0\.5\]/);
  const allocationBeforeUSDCExport = module['_lightusd_next_alloc'];
  let usdcExportStagingAllocations = 0;
  module['_lightusd_next_alloc'] = size => {
    ++usdcExportStagingAllocations;
    return allocationBeforeUSDCExport(size);
  };
  let binaryLayer;
  try { binaryLayer = reloadLayer.exportUSDC(); }
  finally { module['_lightusd_next_alloc'] = allocationBeforeUSDCExport; }
  assert.equal(usdcExportStagingAllocations, 0,
    'USDC export copies directly from retained C bytes without WASM staging allocation');
  assert.equal(binaryLayer.success, true, binaryLayer.error);
  assert.ok(binaryLayer.data instanceof Uint8Array);
  assert.equal(new TextDecoder().decode(binaryLayer.data.subarray(0, 8)), 'PXR-USDC');
  const binaryReload = new module.LayerDocument();
  try {
    const binaryLoad = binaryReload.load(binaryLayer.data);
    assert.equal(binaryLoad.success, true, binaryLoad.error);
    const binaryRoundTrip = binaryReload.exportUSDA();
    assert.equal(binaryRoundTrip.success, true, binaryRoundTrip.error);
    assert.match(binaryRoundTrip.text, /asset\[\] textures = \[@textures\/a\.png@, @textures\/b\.png@\]/);
    assert.match(binaryRoundTrip.text, /half\[\] weights = \[0\.25, 0\.5\]/);
  } finally { binaryReload.delete(); }
  assert.deepEqual(reloadLayer.removeRelationship('/Root/Added', 'links'), {success: true});
  assert.equal(reloadLayer.getRelationshipTargets('/Root/Added', 'links').success, false);
  assert.doesNotMatch(reloadLayer.exportUSDA().text, /rel links/);
  assert.equal(reloadLayer.removePrim('/Root/Added').success, true);
  assert.doesNotMatch(reloadLayer.exportUSDA().text, /Added/);
  assert.equal(reloadLayer.load(new Uint8Array()).success, false);
} finally {
  layerDocument.delete();
  reloadLayer.delete();
}
const nextAssets = new module.NextAssetStore();
assert.deepEqual(Object.getOwnPropertyNames(module.NextAssetStore.prototype)
  .filter(name => name !== 'constructor').sort(),
JSON.parse(fs.readFileSync(new URL('./next-asset-store-api-inventory.json', import.meta.url), 'utf8')));
assert.deepEqual(nextAssets.memoryStats(), {
  assetCount: 0, bytesUsed: 0, limitBytes: 512 * 1024 * 1024
});
assert.equal(nextAssets.getAssetCount(), cacheSize(0));
assert.equal(nextAssets.getBaseWorkingPath(), '.');
nextAssets.setBaseWorkingPath('/scene/root');
assert.equal(nextAssets.getBaseWorkingPath(), '/scene/root');
nextAssets.addAssetSearchPath('/assets/a');
nextAssets.addAssetSearchPath('/assets/b');
assert.deepEqual(nextAssets.getAssetSearchPaths(), ['/assets/a', '/assets/b']);
nextAssets.clearAssetSearchPaths();
assert.deepEqual(nextAssets.getAssetSearchPaths(), []);
assert.equal(nextAssets.getAllowParentRelativeAssetPaths(), true);
nextAssets.setAllowParentRelativeAssetPaths(false);
assert.equal(nextAssets.getAllowParentRelativeAssetPaths(), false);
nextAssets.setAllowParentRelativeAssetPaths(true);
nextAssets.setMemoryLimitBytes(32);
const generatedAssetId = nextAssets.registerMemoryAsset(new Uint8Array([0, 255, 9]));
assert.match(generatedAssetId, /^usd-anon:/);
assert.deepEqual(Array.from(nextAssets.readMemoryAsset(generatedAssetId)), [0, 255, 9]);
assert.deepEqual(nextAssets.assetIdentifiers(), [generatedAssetId]);
assert.equal(nextAssets.getAssetCount(), cacheSize(1));
assert.throws(() => nextAssets.setMemoryLimitBytes(2), /below current use/);
assert.equal(nextAssets.registerMemoryAsset('named.bin', new Uint8Array([1, 2])), 'named.bin');
assert.equal(nextAssets.getAssetCount(), cacheSize(2));
const namedUuid = nextAssets.getAssetUUID('named.bin');
assert.equal(nextAssets.getStreamingAssetUUID('named.bin'), namedUuid);
assert.match(namedUuid, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
assert.equal(nextAssets.findAssetByUUID(namedUuid), 'named.bin');
assert.deepEqual(nextAssets.getAsset('named.bin'), {
  name: 'named.bin', data: new Uint8Array([1, 2]),
  sha256: 'a12871fee210fb8619291eaea194581cbd2531e4b23759d225f6806923f63222',
  uuid: namedUuid
});
assert.deepEqual(nextAssets.getAssetByUUID(namedUuid), nextAssets.getAsset('named.bin'));
const borrowedAssetView = nextAssets.getAssetCacheDataAsMemoryView('named.bin');
assert.ok(borrowedAssetView instanceof Uint8Array);
assert.equal(borrowedAssetView.buffer, module.HEAPU8.buffer);
assert.deepEqual(Array.from(borrowedAssetView), [1, 2]);
borrowedAssetView[0] = 9;
assert.deepEqual(Array.from(nextAssets.readMemoryAsset('named.bin')), [9, 2],
  'borrowed view aliases the retained asset bytes');
borrowedAssetView[0] = 1;
assert.equal(nextAssets.getAssetCacheDataAsMemoryView('missing.bin'), undefined);
const detachedData = nextAssets.getAsset('named.bin').data;
detachedData[0] = 99;
assert.deepEqual(Array.from(nextAssets.readMemoryAsset('named.bin')), [1, 2]);
assert.deepEqual(nextAssets.getAsset('missing.bin'), {});
assert.deepEqual(nextAssets.getAssetByUUID('missing-uuid'),
  {error: 'Asset not found with UUID: missing-uuid'});
assert.equal(nextAssets.getAssetUUID('missing.bin'), '');
assert.equal(nextAssets.getAssetHash('missing.bin'), '');
assert.deepEqual(nextAssets.getAllAssetUUIDs(), {
  [generatedAssetId]: nextAssets.getAssetUUID(generatedAssetId),
  'named.bin': namedUuid
});
assert.equal(nextAssets.getAssetHash('named.bin'),
  'a12871fee210fb8619291eaea194581cbd2531e4b23759d225f6806923f63222');
assert.equal(nextAssets.verifyAssetHash('named.bin', nextAssets.getAssetHash('named.bin')), true);
assert.equal(nextAssets.verifyAssetHash('named.bin', 'wrong-hash'), false);
const rawAssetPointer = module['_lightusd_next_alloc'](5);
assert.ok(rawAssetPointer);
module.HEAPU8.set([99, 4, 5, 6, 99], Number(rawAssetPointer));
const rawAssetSubview = typeof rawAssetPointer === 'bigint' ? rawAssetPointer + 1n : rawAssetPointer + 1;
assert.equal(nextAssets.setAssetFromRawPointer('raw-pointer.bin', rawAssetSubview, 3), false);
assert.equal(nextAssets.setAssetFromRawPointer('raw-pointer.bin', rawAssetSubview, 3), true);
assert.equal(nextAssets.getAssetCount(), cacheSize(3));
assert.deepEqual(Array.from(nextAssets.readMemoryAsset('raw-pointer.bin')), [4, 5, 6]);
assert.throws(() => nextAssets.setAssetFromRawPointer('bad-pointer', module.HEAPU8.byteLength - 1, 3),
  /outside WASM heap/);
module['_lightusd_next_free'](rawAssetPointer);
assert.equal(nextAssets.deleteAsset('raw-pointer.bin'), true);
const rawGuardHandle = module['_lightusd_next_create'](5) >>> 0;
assert.ok(rawGuardHandle);
const invalidRawHeapPointer = typeof rawAssetPointer === 'bigint'
  ? BigInt(module.HEAPU8.byteLength - 1) : module.HEAPU8.byteLength - 1;
assert.equal(module['_lightusd_next_asset_store_set_raw'](
  rawGuardHandle, typeof invalidRawHeapPointer === 'bigint' ? 0n : 0,
  0, invalidRawHeapPointer, 4), -1,
  'the C entry rejects a heap span that crosses the current memory boundary');
module['_lightusd_next_destroy'](rawGuardHandle);
assert.equal(nextAssets.registerMemoryAsset('named.bin', new Uint8Array([3, 4])), 'named.bin');
const replacedUuid = nextAssets.getAssetUUID('named.bin');
assert.notEqual(replacedUuid, namedUuid);
assert.deepEqual(Array.from(nextAssets.getAssetCacheDataAsMemoryView('named.bin')), [3, 4]);
assert.equal(nextAssets.findAssetByUUID(namedUuid), '');
nextAssets.setAlias('alias.bin', 'named.bin');
assert.deepEqual(Array.from(nextAssets.readMemoryAsset('alias.bin')), [3, 4]);
assert.equal(nextAssets.getAssetUUID('alias.bin'), replacedUuid);
assert.deepEqual(nextAssets.getAsset('alias.bin').data, new Uint8Array([3, 4]));
assert.equal(nextAssets.getAssetByUUID(replacedUuid).name, 'named.bin');
assert.equal(nextAssets.getAssetHash('alias.bin'),
  '0ce3940bebf2b22a5d2108ecf0c368a0541c7e3c45703f8540921b4eafc82947');
assert.equal(nextAssets.verifyAssetHash('alias.bin', nextAssets.getAssetHash('alias.bin')), true);
assert.equal(nextAssets.deleteAssetByUUID(replacedUuid), true);
assert.equal(nextAssets.getAssetCount(), cacheSize(1));
assert.equal(nextAssets.readMemoryAsset('named.bin'), null);
assert.equal(nextAssets.readMemoryAsset('alias.bin'), null,
  'aliases remain but stop reading after target removal');
assert.equal(nextAssets.deleteAssetByUUID(replacedUuid), false);
assert.equal(nextAssets.unregisterMemoryAsset(generatedAssetId), true);
assert.equal(nextAssets.getAssetCount(), cacheSize(0));
assert.equal(nextAssets.unregisterMemoryAsset('named.bin'), false);
assert.deepEqual(nextAssets.memoryStats(), {assetCount: 0, bytesUsed: 0, limitBytes: 32});
nextAssets.clear();
assert.deepEqual(nextAssets.memoryStats(), {assetCount: 0, bytesUsed: 0, limitBytes: 32});
nextAssets.setMemoryLimitBytes(256);
const cachedLayerBytes = encode('#usda 1.0\ndef Xform "CachedLayer" {}\n');
assert.equal(nextAssets.setAsset('cached-layer.usda', cachedLayerBytes), false);
const cachedLayerRecord = nextAssets.getAsset('cached-layer.usda');
const cachedLayerDocument = new module.LayerDocument();
try {
  assert.ok(cachedLayerRecord.data instanceof Uint8Array);
  assert.deepEqual(cachedLayerDocument.load(cachedLayerRecord.data),
    {success: true, primCount: 1});
  assert.match(cachedLayerDocument.exportUSDA().text, /CachedLayer/);
} finally { cachedLayerDocument.delete(); }
nextAssets.delete();
const cacheCompat = new module.NextAssetStore();
assert.equal(cacheCompat.setAsset('cache-key', new Uint8Array([5])), false);
const cacheUuid = cacheCompat.getAssetUUID('cache-key');
assert.equal(cacheCompat.setAsset('cache-key', new Uint8Array([6])), true);
assert.notEqual(cacheCompat.getAssetUUID('cache-key'), cacheUuid);
assert.equal(cacheCompat.assetExists('cache-key'), true);
assert.equal(cacheCompat.hasAsset(cacheCompat.getAssetUUID('cache-key')), true);
assert.equal(cacheCompat.deleteAssetByName('cache-key'), true);
assert.equal(cacheCompat.hasAsset('cache-key'), false);
assert.equal(cacheCompat.deleteAssetByName('cache-key'), false);
assert.equal(cacheCompat.setAsset('cache-key', new Uint8Array([7])), false);
assert.equal(cacheCompat.deleteAsset(cacheCompat.getAssetUUID('cache-key')), true);
assert.equal(cacheCompat.setAsset('cache-key', new Uint8Array([8])), false);
cacheCompat.setAsset('empty-cache-key', new Uint8Array(0));
assert.deepEqual(cacheCompat.getAssetCacheDataAsMemoryView('empty-cache-key'), new Uint8Array(0));
cacheCompat.clearAssets();
assert.equal(cacheCompat.assetExists('cache-key'), false);
cacheCompat.delete();
const cacheEvict = new module.NextAssetStore();
assert.equal(cacheEvict.getAssetCacheSizeBytes(), cacheSize(0));
assert.equal(cacheEvict.getAssetCacheMaxSizeBytes(), cacheSize(0));
cacheEvict.setAssetCacheMaxSizeBytes(4);
cacheEvict.setAsset('a', new Uint8Array([1]));
cacheEvict.setAsset('b', new Uint8Array([2]));
assert.equal(cacheEvict.getAssetCacheSizeBytes(), cacheSize(4));
assert.equal(cacheEvict.getAsset('a').data[0], 1);
const evictedBorrow = cacheEvict.getAssetCacheDataAsMemoryView('a');
cacheEvict.setAsset('c', new Uint8Array([3]));
assert.deepEqual(cacheEvict.assetIdentifiers(), ['b', 'c'],
  'legacy cache eviction removes the first sorted key, independent of reads');
assert.equal(cacheEvict.getAssetCacheDataAsMemoryView('a'), undefined);
assert.ok(evictedBorrow instanceof Uint8Array);
assert.equal(cacheEvict.getAssetCacheSizeBytes(), cacheSize(4));
cacheEvict.setAssetCacheMaxSizeBytes(3n);
assert.equal(cacheEvict.getAssetCacheSizeBytes(), cacheSize(4),
  'changing the cache cap does not eagerly evict entries');
cacheEvict.setMemoryLimitBytes(2);
assert.throws(() => cacheEvict.setAsset('d', new Uint8Array([4])), /aggregate cache limit exceeded/);
assert.deepEqual(cacheEvict.assetIdentifiers(), ['b', 'c'],
  'hard payload-limit rejection leaves cache entries intact');
cacheEvict.setMemoryLimitBytes(3);
cacheEvict.setAsset('d', new Uint8Array([4]));
assert.deepEqual(cacheEvict.assetIdentifiers(), ['d']);
assert.equal(cacheEvict.getAssetCacheSizeBytes(), cacheSize(2));
cacheEvict.clearAssets();
cacheEvict.setAssetCacheMaxSizeBytes(0n);
cacheEvict.setAsset('e', new Uint8Array([5]));
cacheEvict.setAsset('f', new Uint8Array([6]));
assert.equal(cacheEvict.getAssetCacheSizeBytes(), cacheSize(4));
cacheEvict.delete();
const metadataBudgetStore = new module.NextAssetStore();
try {
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 0n);
  assert.equal(metadataBudgetStore.metadataLimitBytes(), 0x4000000n);
  metadataBudgetStore.setMetadataLimitBytes(600);
  metadataBudgetStore.registerMemoryAsset('a', new Uint8Array(0));
  metadataBudgetStore.registerMemoryAsset('b', new Uint8Array(0));
  assert.equal(metadataBudgetStore.memoryStats().bytesUsed, 0,
    'zero-byte assets still consume separately bounded metadata');
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 514n);
  assert.throws(() => metadataBudgetStore.registerMemoryAsset('c', new Uint8Array(0)),
    /metadata limit exceeded/);
  assert.deepEqual(metadataBudgetStore.assetIdentifiers(), ['a', 'b'],
    'metadata rejection preserves existing cache entries');
  assert.throws(() => metadataBudgetStore.setAlias('short', 'target'),
    /metadata limit exceeded/);
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 514n);
  metadataBudgetStore.setMetadataLimitBytes(700);
  metadataBudgetStore.setAlias('short', 'target');
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 653n);
  metadataBudgetStore.registerMemoryAsset('b', new Uint8Array(0));
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 653n,
    'replacing an asset charges metadata once');
  assert.throws(() => metadataBudgetStore.setAlias('short', 'x'.repeat(100)),
    /metadata limit exceeded/);
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 653n,
    'rejected alias replacement preserves prior accounting');
  metadataBudgetStore.setAlias('short', 'x');
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 648n,
    'alias replacement releases the old target-name charge');
  assert.throws(() => metadataBudgetStore.setMetadataLimitBytes(643),
    /below current metadata use/);
  assert.equal(metadataBudgetStore.unregisterMemoryAsset('a'), true);
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 391n);
  metadataBudgetStore.clear();
  assert.equal(metadataBudgetStore.metadataBytesUsed(), 0n);
} finally {
  metadataBudgetStore.delete();
}
const subdiv = new module.SubdivStreamer();
assert.ok(subdiv.heapBytes() > 0);
assert.throws(() => subdiv.refineStream(), /wrong argument count/);
const quad = new Float32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]);
const quadCounts = new Uint32Array([4]);
const quadIndices = new Uint32Array([0, 1, 2, 3]);
const quadUV = new Float32Array([0, 0, 1, 0, 1, 1, 0, 1]);
const batches = [];
assert.equal(subdiv.refineStream(quad, quadCounts, quadIndices, quadUV,
  quadIndices, 0, 0, 0, 1, 1, 0, 0, true,
  (positions, normals, indices, faceSource, uv, numVertices, numFaces, batchIndex) => {
    assert.ok(positions instanceof Float32Array);
    assert.ok(normals instanceof Float32Array);
    assert.ok(indices instanceof Uint32Array);
    assert.ok(faceSource instanceof Uint32Array);
    assert.ok(uv instanceof Float32Array);
    assert.equal(positions.length, numVertices * 3);
    assert.equal(normals.length, positions.length);
    assert.equal(faceSource.length, numFaces);
    assert.equal(uv.length, indices.length * 2);
    batches.push({numVertices, numFaces, batchIndex});
  }), '');
assert.ok(batches.length > 0);
const subdivInput = module._lightusd_next_alloc(quad.byteLength);
try {
  const heapQuad = new Float32Array(module.HEAPU8.buffer, Number(subdivInput), quad.length);
  heapQuad.set(quad);
  let untexturedBatches = 0;
  assert.equal(subdiv.refineStream(heapQuad, quadCounts, quadIndices,
    null, null, 0, 0, 0, 1, 1, 0, 0, false,
    (_positions, normals, _indices, _faceSource, uv) => {
      assert.equal(normals, null);
      assert.equal(uv, null);
      ++untexturedBatches;
    }), '');
  assert.ok(untexturedBatches > 0);
} finally { module._lightusd_next_free(subdivInput); }
assert.match(subdiv.refineStream(new Float32Array([0]), quadCounts,
  quadIndices, null, null, 0, 0, 0, 1, 1, 0, 0, false, () => {}),
  /points length must be a multiple of 3/);
assert.throws(() => subdiv.refineStream(quad, quadCounts, quadIndices,
  null, null, 0, 0, 0, 1, 1, 0, 0, false,
  () => { throw new Error('subdiv callback failure'); }), /subdiv callback failure/);
subdiv.delete();
assert.throws(() => subdiv.heapBytes(), /Invalid LightUSD receiver/);
assert.deepEqual(Object.getOwnPropertyNames(module.NextFlattenSession.prototype)
  .filter(name => name !== 'constructor').sort(),
JSON.parse(fs.readFileSync(new URL('./next-flatten-session-api-inventory.json', import.meta.url), 'utf8')));
const flatten = new module.NextFlattenSession();
assert.deepEqual(flatten.provideLayer('dep.usda', encode('#usda 1.0\n')),
  {success: false, error: 'session not started (call begin first)'});
assert.deepEqual(flatten.begin(new Uint8Array(), 'root.usda', true),
  {success: false, error: 'empty root layer buffer'});
assert.equal(flatten.error(), 'empty root layer buffer');
assert.throws(() => flatten.begin(null, 'root.usda', true), /expected byte view/);
assert.throws(() => flatten.provideLayer('dep.usda', null), /expected byte view/);
assert.throws(() => flatten.setVariantOverride('lod'), /wrong argument count/);
const flattenRoot = encode('#usda 1.0\ndef Xform "Root" {}\n');
const flattenPtr = module._lightusd_next_alloc(flattenRoot.length + 2);
try {
  module.HEAPU8.set(flattenRoot, Number(flattenPtr) + 1);
  assert.deepEqual(flatten.begin(module.HEAPU8.subarray(
    Number(flattenPtr) + 1, Number(flattenPtr) + 1 + flattenRoot.length),
    'root.usda', true), {success: true, status: 'ready'});
} finally {
  module._lightusd_next_free(flattenPtr);
}
assert.deepEqual(flatten.setVariantOverride('lod', 'high'), {success: true});
const dependency = encode('x#usda 1.0\ndef Xform "Dep" {}\ny').subarray(1, -1);
const expectedFlattenInputBytes = flattenRoot.length + dependency.length;
assert.deepEqual(flatten.provideLayer('./dep.usda', dependency), {success: true});
assert.equal(flatten.inputBytes(), expectedFlattenInputBytes);
const flattenResult = flatten.step(null);
assert.equal(flattenResult.success, true, flattenResult.error);
assert.equal(flattenResult.status, 'done');
assert.ok(flattenResult.data instanceof Uint8Array);
flatten.end();
assert.equal(flatten.error(), '');
flatten.delete();
assert.throws(() => flatten.end(), /Invalid LightUSD receiver/);
const boundedFlatten = new module.NextFlattenSession();
assert.equal(boundedFlatten.maxInputBytes(), 512 * 1024 * 1024);
assert.deepEqual(boundedFlatten.setMaxInputBytes(expectedFlattenInputBytes),
  {success: true});
assert.deepEqual(boundedFlatten.begin(flattenRoot, 'root.usda', true),
  {success: true, status: 'ready'});
assert.deepEqual(boundedFlatten.provideLayer('dep.usda', dependency), {success: true});
const retainedInputBytes = boundedFlatten.inputBytes();
assert.deepEqual(boundedFlatten.provideLayer('dep.usda', new Uint8Array(dependency.length + 1)), {
  success: false, error: 'Layers exceed configured aggregate input byte limit'
});
assert.equal(boundedFlatten.inputBytes(), retainedInputBytes,
  'rejected layer replacement leaves aggregate input unchanged');
assert.deepEqual(boundedFlatten.setMaxInputBytes(retainedInputBytes - 1), {
  success: false, error: 'Input byte limit is below retained root and layer bytes'
});
boundedFlatten.end();
boundedFlatten.delete();
const outputBoundedFlatten = new module.NextFlattenSession();
assert.equal(outputBoundedFlatten.maxOutputBytes(), 512 * 1024 * 1024);
assert.deepEqual(outputBoundedFlatten.setMaxOutputBytes(64), {success: true});
assert.equal(outputBoundedFlatten.begin(flattenRoot, 'root.usda', true).success, true);
const cappedFlattenOutput = outputBoundedFlatten.step(null);
assert.equal(cappedFlattenOutput.success, false);
assert.match(cappedFlattenOutput.error, /Crate output exceeds configured file-size limit/);
assert.deepEqual(outputBoundedFlatten.setMaxOutputBytes(1024 * 1024), {success: true});
const recoveredFlattenOutput = outputBoundedFlatten.step(null);
assert.equal(recoveredFlattenOutput.success, true, recoveredFlattenOutput.error);
assert.ok(recoveredFlattenOutput.data.length > 64);
assert.deepEqual(outputBoundedFlatten.setMaxOutputBytes(64), {success: true},
  'a returned JS copy does not consume the session output budget');
outputBoundedFlatten.end();
outputBoundedFlatten.delete();
const streamedOutputBound = new module.NextFlattenSession();
assert.deepEqual(streamedOutputBound.setMaxOutputBytes(64), {success: true});
assert.equal(streamedOutputBound.begin(flattenRoot, 'root.usda', true).success, true);
const cappedChunks = [];
const cappedStreamResult = streamedOutputBound.step(chunk => {
  cappedChunks.push(chunk.slice());
  return true;
});
assert.equal(cappedStreamResult.success, false);
assert.match(cappedStreamResult.error, /Crate output exceeds configured file-size limit/);
assert.ok(cappedChunks.reduce((sum, chunk) => sum + chunk.length, 0) <= 64,
  'streaming output never emits bytes beyond the configured cap');
streamedOutputBound.end();
streamedOutputBound.delete();
const diagnosticFlatten = new module.NextFlattenSession();
const diagnosticRoot = encode(`#usda 1.0
def Xform "Root" ( references = @broken.usda@</Missing> ) { }
`);
assert.equal(diagnosticFlatten.begin(diagnosticRoot, 'root.usda', true).success, true);
assert.deepEqual(diagnosticFlatten.step(null), {
  success: true, status: 'need-layer', key: 'broken.usda'
});
assert.deepEqual(diagnosticFlatten.provideLayer('broken.usda',
  encode('#usda 1.0\nthis is not valid USDA\n')), {success: true});
const compositionFailure = diagnosticFlatten.step(null);
assert.equal(compositionFailure.success, false);
assert.equal(compositionFailure.status, 'error');
assert.ok(compositionFailure.compositionErrors?.length > 0,
  'flatten failures preserve structured composition diagnostics');
assert.equal(compositionFailure.compositionErrorCount,
  compositionFailure.compositionErrors.length);
diagnosticFlatten.end();
diagnosticFlatten.delete();
const staleFlatten = module._lightusd_next_create(3) >>> 0;
assert.ok(staleFlatten);
assert.equal(module._lightusd_next_flatten_end(staleFlatten), 0);
module._lightusd_next_destroy(staleFlatten);
assert.equal(module._lightusd_next_flatten_end(staleFlatten), -1);
const streamed = new module.NextFlattenSession();
assert.deepEqual(streamed.step(null),
  {success: false, error: 'session not started (call begin first)'});
assert.throws(() => streamed.step(1), /expected callback or null/);
assert.equal(streamed.begin(flattenRoot, 'root.usda', true).success, true);
const chunks = [];
const streamedResult = streamed.step(chunk => {
  chunks.push(chunk.slice());
  return true;
});
assert.equal(streamedResult.success, true, streamedResult.error);
assert.equal(streamedResult.status, 'done');
assert.equal(Object.hasOwn(streamedResult, 'data'), false);
assert.equal(new TextDecoder().decode(Buffer.concat(chunks).subarray(0, 8)), 'PXR-USDC');
let abortCalls = 0;
assert.deepEqual(streamed.step(() => { ++abortCalls; return false; }),
  {success: true, status: 'ready'});
assert.ok(abortCalls > 0);
assert.throws(() => streamed.step(() => { throw new Error('chunk failure'); }),
  /chunk failure/);
streamed.end();
streamed.delete();
assert.equal(module._lightusd_next_create(0), 0);
assert.equal(module._lightusd_next_create(99), 0);
const stale = module._lightusd_next_create(4) >>> 0;
assert.ok(stale);
assert.equal(module._lightusd_next_render_count(stale, 0), 0);
assert.equal(module._lightusd_next_render_count(stale, 10), 0);
assert.equal(module._lightusd_next_render_count(stale, 255), -1);
assert.equal(module._lightusd_next_render_set_flag(stale, 0, 1), 0);
assert.equal(module._lightusd_next_render_set_flag(stale, 255, 1), -1);
assert.equal(module._lightusd_next_render_control(stale, 0), 0);
assert.equal(module._lightusd_next_render_control(stale, 255), -1);
const emptyTextSettings = () => {
  try {
    assert.equal(module._lightusd_next_render_set_text(stale, 1, 0, 0), 0);
    return module._lightusd_next_render_set_variant_override(stale, 0, 0, 0, 0);
  } catch (error) {
    if (!(error instanceof TypeError)) throw error;
    assert.equal(module._lightusd_next_render_set_text(stale, 1, 0n, 0), 0);
    return module._lightusd_next_render_set_variant_override(stale, 0n, 0, 0n, 0);
  }
};
assert.equal(emptyTextSettings(), 0);
assert.equal(module._lightusd_next_call, undefined);
assert.equal(module._lightusd_next_render_variant_set_count(stale), 0);
assert.equal(module._lightusd_next_render_variant_name_count(stale, 0), -1);
const provideEmptyAsset = () => {
  try { return module._lightusd_next_render_provide_asset(stale, 0, 0, 0, 0); }
  catch (error) {
    if (!(error instanceof TypeError)) throw error;
    return module._lightusd_next_render_provide_asset(stale, 0n, 0, 0n, 0);
  }
};
assert.equal(provideEmptyAsset(), 0);
const removeEmptyAsset = () => {
  try { return module._lightusd_next_render_remove_asset(stale, 0, 0); }
  catch (error) {
    if (!(error instanceof TypeError)) throw error;
    return module._lightusd_next_render_remove_asset(stale, 0n, 0);
  }
};
assert.equal(removeEmptyAsset(), -1);
const invalidVariantString = () => {
  try { return module._lightusd_next_render_variant_string(stale, 0, 0, 0, 0, 0); }
  catch (error) {
    if (!(error instanceof TypeError)) throw error;
    return module._lightusd_next_render_variant_string(stale, 0, 0, 0, 0n, 0);
  }
};
assert.equal(invalidVariantString(), -1);
const nullPointer = () => {
  try { return module._lightusd_next_render_error(stale, 0, 0); }
  catch (error) {
    if (!(error instanceof TypeError)) throw error;
    return module._lightusd_next_render_error(stale, 0n, 0);
  }
};
assert.equal(nullPointer(), 0);
module._lightusd_next_destroy(stale);
// Reuse and then retire a generation slot. An old handle stays invalid.
for (let i = 0; i < 4200; ++i) {
  const handle = module._lightusd_next_create(4) >>> 0;
  assert.ok(handle);
  assert.equal(module._lightusd_next_render_count(stale, 0), -1);
  assert.equal(module._lightusd_next_converter_export(handle, 0), -1); // wrong class
  module._lightusd_next_destroy(handle);
}
module._lightusd_next_destroy(stale); // stale disposal is harmless
assert.equal(module._lightusd_next_render_count(stale, 0), -1);
assert.equal(module._lightusd_next_render_set_flag(stale, 0, 1), -1);
assert.equal(module._lightusd_next_render_control(stale, 0), -1);
assert.equal(module._lightusd_next_render_variant_set_count(stale), -1);
assert.equal(module._lightusd_next_render_set_provided_asset_byte_limit(stale, 1), -1);
assert.equal(module._lightusd_next_render_provided_asset_byte_limit(stale), -1);
assert.equal(module._lightusd_next_render_variant_name_count(stale, 0), -1);
assert.equal(invalidVariantString(), -1);
assert.equal(provideEmptyAsset(), -1);
assert.equal(removeEmptyAsset(), -1);
const staleBegin = () => {
  try { return module._lightusd_next_render_begin(stale, 0, 0); }
  catch (error) {
    if (!(error instanceof TypeError)) throw error;
    return module._lightusd_next_render_begin(stale, 0n, 0);
  }
};
assert.equal(staleBegin(), -1);

const stream = new module.RenderStream();
assert.equal(stream.warning(), '');
const layerRenderDocument = new module.LayerDocument();
try {
  assert.equal(layerRenderDocument.load(authoredLayer).success, true);
  const layerRenderResult = stream.beginFromLayerDocument(layerRenderDocument);
  assert.equal(layerRenderResult.success, true,
    layerRenderResult.error || stream.error());
  assert.ok(stream.getStats().renderSceneNodes > 0,
    'LayerDocument-to-RenderStream path must create render nodes');
} finally {
  layerRenderDocument.delete();
}
const pngPixels = new Uint8Array([255, 0, 0, 255]);
const imageStream = new module.RenderStream();
try {
  const materialFixture = new Uint8Array(fs.readFileSync(new URL(
    '../../../tests/usda/c-core-material-queries.usda', import.meta.url)));
  const imageLoad = imageStream.begin(materialFixture);
  assert.equal(imageLoad.success, true, imageLoad.error || imageStream.error());
  assert.ok(imageStream.numImages() > 0);
  const rootTree = imageStream.getDefaultRootNode();
  assert.equal(rootTree.primName, rootTree.name);
  assert.equal(rootTree.displayName, rootTree.name);
  assert.equal(rootTree.absPath, rootTree.primPath);
  assert.equal(rootTree.nodeType, rootTree.type);
  assert.equal(rootTree.nodeCategory, 'group');
  assert.equal(rootTree.contentId, rootTree.dataId);
  assert.deepEqual(rootTree.globalMatrix, rootTree.worldMatrix);
  const imageRecord = imageStream.getImageCopy(0);
  // RenderStream intentionally catalogs texture references without decoding
  // their payloads; callers still receive the image metadata and URI.
  assert.equal(imageRecord.decoded, false);
  assert.equal(imageRecord.width, 0);
  assert.equal(imageRecord.height, 0);
  assert.equal(imageRecord.channels, 4);
  assert.equal(Object.hasOwn(imageRecord, 'data'), false);
  assert.equal(typeof imageRecord.uri, 'string');
  const imageView = imageStream.getImageView(0);
  assert.equal(imageView.decoded, false);
  assert.equal(Object.hasOwn(imageView, 'data'), false);
  assert.equal(imageView.uri, imageRecord.uri);
  const allImages = imageStream.getAllImages();
  assert.equal(allImages.length, imageStream.numImages());
  assert.deepEqual(allImages[0], imageRecord);
  assert.throws(() => imageStream.getAllImages(1), /wrong argument count/);
  const imageFieldForMetadataProbe = module._lightusd_next_render_image_field;
  const resourceNameForMetadataProbe = module._lightusd_next_render_resource_name;
  const resourcePathForMetadataProbe = module._lightusd_next_render_resource_path;
  try {
    module._lightusd_next_render_resource_name = (...args) => {
      if (args[3] === 0 || args[3] === 0n) return 0x10000000;
      throw new Error('image metadata preflight must not copy names');
    };
    module._lightusd_next_render_resource_path = (...args) => {
      if (args[3] === 0 || args[3] === 0n) return 0;
      throw new Error('image metadata preflight must not copy paths');
    };
    module._lightusd_next_render_image_field = () => {
      throw new Error('image metadata budget must reject before image field queries');
    };
    assert.throws(() => imageStream.getAllImages(),
      /aggregate image metadata exceeds 512 MiB/);
  } finally {
    module._lightusd_next_render_image_field = imageFieldForMetadataProbe;
    module._lightusd_next_render_resource_name = resourceNameForMetadataProbe;
    module._lightusd_next_render_resource_path = resourcePathForMetadataProbe;
  }
  const aggregateImageFieldExport = module._lightusd_next_render_image_field;
  try {
    module._lightusd_next_render_image_field = (handle, id, field) =>
      field === 4 ? 1 : field === 7 ? 0x20000001 : aggregateImageFieldExport(handle, id, field);
    assert.throws(() => imageStream.getAllImages(), /aggregate image data exceeds 512 MiB/);
  } finally {
    module._lightusd_next_render_image_field = aggregateImageFieldExport;
  }
  const imageFieldForMemoryProbe = module._lightusd_next_render_image_field;
  const imageDataForMemoryProbe = module._lightusd_next_render_image_data;
  const remainingForImageProbe = module._lightusd_next_render_remaining_memory_bytes;
  let imagePayloadCopies = 0;
  try {
    module._lightusd_next_render_image_field = (handle, id, field) =>
      field === 4 ? 1 : field === 7 ? 4 : imageFieldForMemoryProbe(handle, id, field);
    module._lightusd_next_render_remaining_memory_bytes = () => 0;
    Object.defineProperty(imageStream, 'resourceNameBuffer', {
      value: () => new Uint8Array(0), configurable: true
    });
    Object.defineProperty(imageStream, 'resourcePathBuffer', {
      value: () => new Uint8Array(0), configurable: true
    });
    module._lightusd_next_render_image_data = (...args) => {
      ++imagePayloadCopies;
      return imageDataForMemoryProbe(...args);
    };
    assert.throws(() => imageStream.getImageCopy(0), /image payload exceeds remaining memory limit/);
    assert.equal(imagePayloadCopies, 0,
      'single image copy must preflight remaining memory before accessing pixel data');
    assert.throws(() => imageStream.getAllImages(), /aggregate exceeds remaining memory limit/);
    assert.equal(imagePayloadCopies, 0,
      'aggregate image copy must preflight remaining memory before accessing pixel data');
  } finally {
    module._lightusd_next_render_image_field = imageFieldForMemoryProbe;
    module._lightusd_next_render_image_data = imageDataForMemoryProbe;
    module._lightusd_next_render_remaining_memory_bytes = remainingForImageProbe;
    delete imageStream.resourceNameBuffer;
    delete imageStream.resourcePathBuffer;
  }
  assert.deepEqual(imageStream.getImageCopy(99999), {});
  assert.deepEqual(imageStream.getImageView(99999), {});
  assert.throws(() => imageStream.getImageCopy(), /one integer image id/);
  assert.throws(() => imageStream.getImageView(), /one integer image id/);
  assert.ok(imageStream.numTextures() > 0);
  const textureRecord = imageStream.getTextureRecord(0);
  assert.equal(textureRecord.id, 0);
  assert.equal(typeof textureRecord.name, 'string');
  assert.equal(typeof textureRecord.uri, 'string');
  assert.equal(typeof textureRecord.uvPrimvar, 'string');
  assert.equal(typeof textureRecord.sourceColorSpace, 'string');
  assert.equal(typeof textureRecord.targetColorSpace, 'string');
  assert.equal(textureRecord.uvPrimvar, '');
  assert.equal(textureRecord.sourceColorSpace, 'auto');
  assert.equal(textureRecord.targetColorSpace, 'lin_rec709_scene');
  assert.equal(textureRecord.colorTransformValid, true);
  assert.equal(typeof textureRecord.colorTransformBypass, 'boolean');
  assert.equal(textureRecord.sourceColorIsData, false);
  assert.ok(textureRecord.sourceGamma > 0);
  assert.ok(Number.isFinite(textureRecord.sourceLinearBias));
  assert.equal(textureRecord.sourceToDisplayLinear.length, 9);
  assert.ok(textureRecord.sourceToDisplayLinear.every(Number.isFinite));
  assert.equal(textureRecord.sourceColorSpace, imageStream.textureString(0, 1));
  const remainingMemoryQuery = module._lightusd_next_render_remaining_memory_bytes;
  const imageStreamAlloc = module._lightusd_next_alloc;
  let stringAllocations = 0;
  try {
    module._lightusd_next_render_remaining_memory_bytes = () => 0;
    module._lightusd_next_alloc = size => { ++stringAllocations; return imageStreamAlloc(size); };
    assert.throws(() => imageStream.textureString(0, 1), /remaining memory limit/);
    assert.equal(stringAllocations, 0,
      'string copies must respect remaining-memory limits before allocation');
  } finally {
    module._lightusd_next_render_remaining_memory_bytes = remainingMemoryQuery;
    module._lightusd_next_alloc = imageStreamAlloc;
  }
  assert.throws(() => imageStream.textureString(0, 3), /string kind 0\.\.2/);
  assert.ok(textureRecord.imageId >= 0);
  assert.equal(textureRecord.loaded, false);
  assert.ok(Array.isArray(textureRecord.sampling));
  assert.equal(textureRecord.hasTransform2d, false);
  assert.deepEqual([textureRecord.txRotation, textureRecord.txScaleU,
    textureRecord.txScaleV, textureRecord.txTranslationU,
    textureRecord.txTranslationV], [0, 1, 1, 0, 0]);
  assert.equal(imageStream.textureHasTransform2d(0), 0);
  assert.equal(textureRecord.isUDIM, false);
  assert.equal(textureRecord.udimTextureId, -1);
  assert.deepEqual(imageStream.getAllTextures()[0], textureRecord);
  assert.deepEqual(imageStream.getTextureRecord(99999), {});
  assert.throws(() => imageStream.getTextureRecord(), /one integer texture id/);
  assert.throws(() => imageStream.getAllTextures(1), /wrong argument count/);
  const udimCountExport = module._lightusd_next_render_udim_tile_count;
  const udimTotalExport = module._lightusd_next_render_udim_count;
  const udimTilesExport = module._lightusd_next_render_udim_tiles;
  const udimStringExport = module._lightusd_next_render_udim_string;
  const udimStrings = ['Tex', '/World/Mat/Tex', 'Texture', 'skin.<UDIM>.png'];
  const udimRecords = new Int32Array([1001, 0, 0, 1, 1011, 0, 1, 2]);
  try {
    module._lightusd_next_render_udim_tile_count = (_handle, id) => id === 0 ? 2 : -1;
    module._lightusd_next_render_udim_count = () => 1;
    module._lightusd_next_render_udim_tiles = (_handle, id, ptr, cap) => {
      if (id !== 0) return -1;
      const bytes = new Uint8Array(udimRecords.buffer);
      if (!ptr || cap < bytes.length) return bytes.length;
      module.HEAPU8.set(bytes, Number(ptr));
      return bytes.length;
    };
    module._lightusd_next_render_udim_string = (_handle, id, kind, ptr, cap) => {
      if (id !== 0 || kind < 0 || kind > 3) return -1;
      const bytes = new TextEncoder().encode(udimStrings[kind]);
      if (!ptr || cap < bytes.length) return bytes.length;
      module.HEAPU8.set(bytes, Number(ptr));
      return bytes.length;
    };
    assert.deepEqual(imageStream.getUDIMTextureRecord(0), {
      id: 0, primName: 'Tex', absPath: '/World/Mat/Tex', displayName: 'Texture',
      assetIdentifier: 'skin.<UDIM>.png',
      tiles: [
        {udim: 1001, u: 0, v: 0, imageId: 1},
        {udim: 1011, u: 0, v: 1, imageId: 2}
      ]
    });
    assert.deepEqual(imageStream.getUDIMTextureRecord(99), {});
    assert.equal(imageStream.numUDIMTextures(), 1);
    assert.deepEqual(Object.keys(imageStream.getUDIMTexture(0)).sort(),
      ['absPath', 'assetIdentifier', 'displayName', 'primName', 'tiles']);
    assert.deepEqual(imageStream.getUDIMTexture(99), {});
} finally {
    module._lightusd_next_render_udim_tile_count = udimCountExport;
    module._lightusd_next_render_udim_count = udimTotalExport;
    module._lightusd_next_render_udim_tiles = udimTilesExport;
    module._lightusd_next_render_udim_string = udimStringExport;
  }
  assert.ok(imageStream.numMaterials() > 0);
  const materialRecord = imageStream.getMaterialRecord(0);
  assert.equal(materialRecord.id, 0);
  assert.equal(typeof materialRecord.name, 'string');
  assert.equal(typeof materialRecord.primPath, 'string');
  assert.ok(Number.isInteger(materialRecord.shaderType));
  assert.ok(Number.isInteger(materialRecord.alphaMode));
  if (materialRecord.shaderType !== 0) {
    assert.equal(materialRecord.baseColor.length, 3);
    assert.equal(materialRecord.emissive.length, 3);
    assert.ok(Object.hasOwn(materialRecord.textureIds, 'roughness'));
  }
  assert.deepEqual(imageStream.getAllMaterials()[0], materialRecord);
  assertAggregateHeadroomRejectsBeforeBuild(imageStream, 'getAllMaterials',
    'getMaterialRecord', imageStream.numMaterials());
  assert.deepEqual(imageStream.getMaterialRecord(99999), {});
  assert.throws(() => imageStream.getMaterialRecord(), /one integer material id/);
  assert.throws(() => imageStream.getAllMaterials(1), /wrong argument count/);
  const serializedMaterial = imageStream.getMaterial(0);
  assert.equal(serializedMaterial.format, 'json');
  const serializedJSON = JSON.parse(serializedMaterial.data);
  assert.equal(typeof serializedJSON.name, 'string');
  assert.equal(typeof serializedJSON.abs_path, 'string');
  assert.equal(typeof serializedJSON.display_name, 'string');
  assert.equal(typeof serializedJSON.hasUsdPreviewSurface, 'boolean');
  assert.equal(typeof serializedJSON.hasOpenPBR, 'boolean');
  const serializedXML = imageStream.getMaterialWithFormat(0, 'xml');
  assert.equal(serializedXML.format, 'xml');
  assert.match(serializedXML.data, /<materialx/);
  assert.deepEqual(imageStream.getMaterialWithFormat(0, 'unsupported'),
    {error: "Unsupported format. Use 'json' or 'xml'"});
  const legacyMaterial = imageStream.getMaterialWithFormat(0, 'legacy');
  assert.equal(typeof legacyMaterial.materialXConfig.authored, 'boolean');
  assert.equal(typeof legacyMaterial.useSpecularWorkflow, 'boolean');
  assert.deepEqual(imageStream.getMaterialWithFormat(0, ''), legacyMaterial);
  assert.deepEqual(imageStream.getMaterialWithFormat(99999, 'json'),
    {error: 'Invalid material ID'});
  assert.throws(() => imageStream.getMaterial(), /one integer material id/);
  assert.throws(() => imageStream.getMaterialWithFormat(0), /integer material id and format string/);
  assert.throws(() => imageStream.getMaterialWithFormat(0, {}), /integer material id and format string/);
  const materialFormatCopy = module._lightusd_next_render_material_format_string;
  const materialAlloc = module._lightusd_next_alloc;
  let materialCopyAllocated = false;
  try {
    module._lightusd_next_render_material_format_string = () => 0x20000001;
    module._lightusd_next_alloc = size => {
      materialCopyAllocated = true;
      return materialAlloc(size);
    };
    assert.throws(() => imageStream.getMaterial(0), /string exceeds 512 MiB limit/);
    assert.equal(materialCopyAllocated, false);
  } finally {
    module._lightusd_next_render_material_format_string = materialFormatCopy;
    module._lightusd_next_alloc = materialAlloc;
  }
  // Complete the pending size-query/copy pair and release the C-side cache.
  assert.equal(imageStream.getMaterial(0).format, 'json');
  const imageFieldExport = module._lightusd_next_render_image_field;
  let imageBufferCopies = 0;
  try {
    Object.defineProperty(imageStream, 'textureLoaded', {
      configurable: true, value: () => 1
    });
    Object.defineProperty(imageStream, 'textureImageBuffer', {
      configurable: true, value: () => { ++imageBufferCopies; return new Uint8Array(0); }
    });
    module._lightusd_next_render_image_field = () => 0x20000001;
    assert.throws(() => imageStream.getTextureRecord(0), /exceeds 512 MiB limit/);
    assert.throws(() => imageStream.getAllTextures(), /exceeds 512 MiB limit/);
    assert.equal(imageBufferCopies, 0,
      'oversized texture payloads must be rejected before allocating a copy');
    const resourceNameQuery = module._lightusd_next_render_resource_name;
    try {
      module._lightusd_next_render_resource_name = (handle, kind, id, ptr, cap) =>
        kind === 3 && id === 0 && (ptr === 0 || ptr === 0n) && cap === 0
          ? 0x10000000
          : resourceNameQuery(handle, kind, id, ptr, cap);
      assert.throws(() => imageStream.getTextureRecord(0), /exceeds 512 MiB limit/);
      assert.throws(() => imageStream.getAllTextures(), /exceeds 512 MiB limit/);
      assert.equal(imageBufferCopies, 0,
        'oversized texture metadata must be rejected before image copies');
    } finally {
      module._lightusd_next_render_resource_name = resourceNameQuery;
    }
    const remainingTextureMemory = module._lightusd_next_render_remaining_memory_bytes;
    let textureRecordsBuilt = 0;
    try {
      module._lightusd_next_render_image_field = imageFieldExport;
      module._lightusd_next_render_remaining_memory_bytes = () => 0;
      Object.defineProperty(imageStream, 'getTextureRecord', {
        configurable: true,
        value: () => { ++textureRecordsBuilt; return {}; }
      });
      assert.throws(() => imageStream.getAllTextures(), /aggregate exceeds remaining memory limit/);
      assert.equal(textureRecordsBuilt, 0,
        'aggregate texture budget must reject before constructing texture records');
    } finally {
      module._lightusd_next_render_remaining_memory_bytes = remainingTextureMemory;
      delete imageStream.getTextureRecord;
    }
  } finally {
    module._lightusd_next_render_image_field = imageFieldExport;
    delete imageStream.textureLoaded;
    delete imageStream.textureImageBuffer;
  }
} finally { imageStream.delete(); }
const transformedTextureStream = new module.RenderStream();
try {
  const transformFixture = new Uint8Array(fs.readFileSync(new URL(
    '../../../tests/usda/shader-network-001.usda', import.meta.url)));
  const transformLoad = transformedTextureStream.begin(transformFixture);
  assert.equal(transformLoad.success, true,
    transformLoad.error || transformedTextureStream.error());
  const transformedTextureId = Array.from(
    {length: transformedTextureStream.numTextures()}, (_, id) => id)
    .find(id => transformedTextureStream.textureHasTransform2d(id) === 1);
  assert.notEqual(transformedTextureId, undefined,
    'authored UsdTransform2d state must survive next conversion');
  const transformedTexture =
    transformedTextureStream.getTextureRecord(transformedTextureId);
  assert.equal(transformedTexture.hasTransform2d, true);
  assert.equal(transformedTexture.txRotation, 45);
  assert.deepEqual([transformedTexture.txScaleU, transformedTexture.txScaleV], [2, 2]);
  assert.deepEqual([transformedTexture.txTranslationU,
    transformedTexture.txTranslationV], [0.5, 0.5]);
} finally { transformedTextureStream.delete(); }
const udimStore = new module.NextAssetStore();
const udimStream = new module.RenderStream();
const nativeTextureStream = new module.RenderStream();
try {
  const udimRoot = encode(`#usda 1.0
def Xform "World" {
  def Material "Mat" {
    token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
    def Shader "Surface" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor.connect = </World/Mat/Tex.outputs:rgb>
      token outputs:surface
    }
    def Shader "Tex" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @tiles/skin.<UDIM>.png@
      color3f outputs:rgb
    }
  }
  def Mesh "SurfaceMesh" {
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    rel material:binding = </World/Mat>
  }
}
`);
  udimStore.registerMemoryAsset('udim-root.usda', udimRoot);
  const udimPng = stream.encodeImageNative(pngPixels, 1, 1, 4, 'png');
  udimStore.registerMemoryAsset('tiles/skin.1001.png', udimPng);
  udimStore.registerMemoryAsset('tiles/skin.1011.png', udimPng);
  assert.equal(udimStream.getLoadTextureInNative(), false);
  const udimLoad = udimStream.beginCachedAsset(udimStore, 'udim-root.usda');
  assert.equal(udimLoad.success, true, udimLoad.error || udimStream.error());
  assert.equal(udimStream.getCombineUDIMTiles(), false,
    'next defaults to sparse UDIM tiles for its editing-oriented product path');
  assert.equal(udimStream.numTextures(), 1);
  assert.equal(udimStream.numUDIMTextures(), 1);
  assert.equal(udimStream.getUpAxis(), udimStream.sceneUpAxisName());
  const texture = udimStream.getTextureRecord(0);
  assert.equal(texture.isUDIM, true);
  assert.equal(texture.udimTextureId, 0);
  const legacyTexture = udimStream.getTexture(0);
  assert.equal(legacyTexture.textureImageId, texture.imageId);
  assert.equal(legacyTexture.isUDIM, true);
  assert.equal(legacyTexture.udimTextureId, 0);
  assert.equal(legacyTexture.bias.length, 4);
  assert.equal(legacyTexture.scale.length, 4);
  const udimRecord = udimStream.getUDIMTextureRecord(0);
  assert.equal(udimRecord.assetIdentifier, 'tiles/skin.<UDIM>.png');
  assert.deepEqual(udimRecord.tiles.map(tile => [tile.udim, tile.u, tile.v]),
    [[1001, 0, 0], [1011, 0, 1]]);
  assert.ok(udimRecord.tiles.every(tile => tile.imageId >= 0));
  assert.deepEqual(udimStream.extractUnresolvedTexturePaths(),
    ['tiles/skin.1001.png', 'tiles/skin.1011.png']);
  assert.deepEqual(udimStream.getUDIMTexture(0), {
    primName: 'Tex', absPath: '/World/Mat/Tex', displayName: 'Tex',
    assetIdentifier: 'tiles/skin.<UDIM>.png', tiles: udimRecord.tiles
  });
  assert.equal(udimStream.getUDIMTextureRecord(1).id, undefined);
  nativeTextureStream.setLoadTextureInNative(true);
  assert.equal(nativeTextureStream.getLoadTextureInNative(), true);
  nativeTextureStream.setCombineUDIMTiles(true);
  assert.equal(nativeTextureStream.getCombineUDIMTiles(), true);
  const decodedLoad = nativeTextureStream.beginCachedAsset(udimStore, 'udim-root.usda');
  assert.equal(decodedLoad.success, true, decodedLoad.error || nativeTextureStream.error());
  assert.equal(nativeTextureStream.textureLoaded(0), 1,
    'enabling native texture loading decodes supplied UDIM tile images');
  assert.equal(nativeTextureStream.numUDIMTextures(), 0,
    'combined mode exposes one atlas rather than sparse tile records');
  const combinedTexture = nativeTextureStream.getTextureRecord(0);
  assert.equal(combinedTexture.isUDIM, true);
  assert.equal(combinedTexture.udimTextureId, -1);
  assert.deepEqual([combinedTexture.udimUvScaleU, combinedTexture.udimUvScaleV,
    combinedTexture.udimUvOffsetU, combinedTexture.udimUvOffsetV], [1, 0.5, 0, 0]);
  assert.deepEqual(Array.from(combinedTexture.data), [255, 0, 0, 255, 255, 0, 0, 255]);
  nativeTextureStream.setCombineUDIMTiles(false);
  assert.equal(nativeTextureStream.getCombineUDIMTiles(), false);
} finally {
  udimStream.delete();
  nativeTextureStream.delete();
  udimStore.delete();
}

const openPbrStream = new module.RenderStream();
try {
  const openPbrFixture = new Uint8Array(fs.readFileSync(new URL(
    '../../../tests/usda/lusdrender-openpbr-lobe-golden.usda', import.meta.url)));
  const openPbrLoad = openPbrStream.begin(openPbrFixture);
  assert.equal(openPbrLoad.success, true, openPbrLoad.error || openPbrStream.error());
  const openPbrMaterials = openPbrStream.getAllMaterials();
  assert.ok(openPbrMaterials.some(material => material.shaderType === 2));
  const openPbr = openPbrMaterials.find(material => material.shaderType === 2);
  assert.equal(openPbr.baseColor.length, 3);
  assert.equal(openPbr.emissive.length, 3);
  assert.equal(typeof openPbr.metallic, 'number');
} finally { openPbrStream.delete(); }
const materialFormatStream = new module.RenderStream();
try {
  const formatFixture = new Uint8Array(fs.readFileSync(new URL(
    '../../../tests/usda/next-material-format-parity.usda', import.meta.url)));
  const formatLoad = materialFormatStream.begin(formatFixture);
  assert.equal(formatLoad.success, true, formatLoad.error || materialFormatStream.error());
  const materialId = materialFormatStream.getAllMaterials()
    .find(material => material.shaderType === 2)?.id;
  assert.ok(Number.isInteger(materialId), 'fixture OpenPBR material should be retained');
  const serialized = materialFormatStream.getMaterialWithFormat(materialId, 'json');
  const data = JSON.parse(serialized.data);
  assert.equal(data.hasOpenPBR, true);
  assert.equal(data.openPBR.base.base_roughness.value, 0.37);
  assert.equal(data.openPBR.base.base_diffuse_roughness.value, 0.37);
  assert.deepEqual(data.openPBR.transmission.transmission_scatter.value,
    [0.1, 0.2, 0.3]);
  assert.equal(data.openPBR.transmission.transmission_scatter_anisotropy.value, 0.42);
  assert.equal(data.openPBR.subsurface.subsurface_anisotropy.value, 0.18);
  assert.equal(data.openPBR.coat.coat_rotation.value, 0.28);
  assert.equal(data.openPBR.coat.coat_affect_color.value, 0.64);
  assert.equal(data.openPBR.coat.coat_affect_roughness.value, 0.73);
  // The compatibility serializer follows the legacy rule: authored fuzz takes
  // precedence over the next model's independently retained sheen values.
  assert.equal(data.openPBR.sheen.sheen_weight.value, 0);
  assert.equal(data.openPBR.fuzz.fuzz_weight.value, 0.81);
  const xml = materialFormatStream.getMaterialWithFormat(materialId, 'xml');
  assert.equal(xml.format, 'xml');
  assert.match(xml.data, /<open_pbr_surface/);
} finally { materialFormatStream.delete(); }
const diagnosticStream = new module.RenderStream();
try {
  const diagnosticFixture = new TextDecoder().decode(fs.readFileSync(new URL(
    '../../../tests/usda/c-core-material-queries.usda', import.meta.url)))
    .replace('UsdPreviewSurface', 'UnknownSurface');
  const diagnosticLoad = diagnosticStream.begin(encode(diagnosticFixture));
  assert.equal(diagnosticLoad.success, true,
    diagnosticLoad.error || diagnosticStream.error());
  const diagnosticStringExport = module._lightusd_next_render_material_diagnostic_string;
  try {
    module._lightusd_next_render_material_diagnostic_string = () => 0x10000000;
    assert.throws(() => diagnosticStream.getMaterialRecord(0),
      /aggregate returned data exceeds 512 MiB/);
    assert.throws(() => diagnosticStream.getAllMaterials(),
      /aggregate returned data exceeds 512 MiB/);
  } finally {
    module._lightusd_next_render_material_diagnostic_string = diagnosticStringExport;
  }
  const diagnosedMaterial = diagnosticStream.getAllMaterials()
    .find(material => material.diagnostics.length > 0);
  assert.ok(diagnosedMaterial, 'unsupported shaders should retain material diagnostics');
  assert.equal(diagnosedMaterial.defaultFallback, true);
  assert.ok(diagnosedMaterial.diagnostics.some(diagnostic =>
    diagnostic.shader_id === 'UnknownSurface' &&
    diagnostic.message === 'unsupported surface shader'));
  assert.deepEqual(diagnosticStream.getMaterialDiagnostics(diagnosedMaterial.id),
    diagnosedMaterial.diagnostics);
  {
    const countQuery = module._lightusd_next_render_material_diagnostic_count;
    const allocate = module._lightusd_next_alloc;
    let allocations = 0;
    module._lightusd_next_render_material_diagnostic_count = () => 65537;
    module._lightusd_next_alloc = size => { ++allocations; return allocate(size); };
    try {
      assert.throws(() => diagnosticStream.getMaterialDiagnostics(diagnosedMaterial.id),
        /excessive diagnostic count/);
      assert.equal(allocations, 0,
        'diagnostic count is bounded before allocating strings');
    } finally {
      module._lightusd_next_render_material_diagnostic_count = countQuery;
      module._lightusd_next_alloc = allocate;
    }
  }
  {
    const stringQuery = module._lightusd_next_render_material_diagnostic_string;
    const allocate = module._lightusd_next_alloc;
    let allocations = 0;
    module._lightusd_next_render_material_diagnostic_string = () => 0x10000001;
    module._lightusd_next_alloc = size => { ++allocations; return allocate(size); };
    try {
      assert.throws(() => diagnosticStream.getMaterialDiagnostics(diagnosedMaterial.id),
        /aggregate exceeds 512 MiB limit/);
      assert.equal(allocations, 0,
        'diagnostic string aggregate is bounded before allocating payloads');
    } finally {
      module._lightusd_next_render_material_diagnostic_string = stringQuery;
      module._lightusd_next_alloc = allocate;
    }
  }
  assert.deepEqual(diagnosticStream.getMaterialDiagnostics(99999), []);
  assert.throws(() => diagnosticStream.getMaterialDiagnostics(),
    /one integer material id/);
} finally { diagnosticStream.delete(); }
const nodeMetadataStream = new module.RenderStream();
try {
  const resetFixture = new Uint8Array(fs.readFileSync(new URL(
    '../../../tests/usda/xform-resetxformstack-001.usda', import.meta.url)));
  const resetLoad = nodeMetadataStream.begin(resetFixture);
  assert.equal(resetLoad.success, true, resetLoad.error || nodeMetadataStream.error());
  assert.equal(nodeMetadataStream.getDefaultRootNode().hasResetXform, true);
  nodeMetadataStream.end();
  const instanceFixture = new Uint8Array(fs.readFileSync(new URL(
    '../../../tests/usda/instancing-001.usda', import.meta.url)));
  const instanceLoad = nodeMetadataStream.begin(instanceFixture);
  assert.equal(instanceLoad.success, true, instanceLoad.error || nodeMetadataStream.error());
  let nativeInstance = null;
  const expectedNativeInstanceIds = [];
  for (let nodeId = 0; nodeId < nodeMetadataStream.nodeCount(); ++nodeId) {
    if (nodeMetadataStream.nodeIsInstance(nodeId) === 1) {
      expectedNativeInstanceIds.push(nodeId);
      if (!nativeInstance) nativeInstance = nodeMetadataStream.getNode(nodeId);
    }
  }
  assert.ok(nativeInstance, 'native instance state should survive render conversion');
  assert.ok(nativeInstance.prototypePath.length > 0);
  const nativeInstanceIds = nodeMetadataStream.nativeInstanceNodeIds();
  assert.deepEqual(nativeInstanceIds, expectedNativeInstanceIds);
  assert.throws(() => nodeMetadataStream.nativeInstanceNodeIds(0), /wrong argument count/);
  nodeMetadataStream.end();
  // Analytic gprims are published after authored meshes; node dataIds must
  // still address the mesh output of their own prim.
  const tri = 'point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n' +
    'int[] faceVertexCounts = [3]\nint[] faceVertexIndices = [0, 1, 2]';
  const mixedLoad = nodeMetadataStream.begin(encode(`#usda 1.0
def Xform "A" {
  def Mesh "M0" { ${tri} }
  def Cube "C0" { double size = 0.5 }
}
def Xform "B" {
  def Mesh "M1" { ${tri} }
  def Cube "C1" {}
}
`));
  assert.equal(mixedLoad.success, true, mixedLoad.error || nodeMetadataStream.error());
  const meshNodePaths = [];
  for (let nodeId = 0; nodeId < nodeMetadataStream.nodeCount(); ++nodeId) {
    const node = nodeMetadataStream.getNode(nodeId);
    if (node.type !== 'mesh') continue;
    meshNodePaths.push(node.primPath);
    assert.equal(nodeMetadataStream.getMesh(node.dataId).primPath, node.primPath,
      `node ${node.primPath} dataId ${node.dataId}`);
  }
  assert.deepEqual(meshNodePaths.sort(), ['/A/C0', '/A/M0', '/B/C1', '/B/M1']);
  nodeMetadataStream.end();
  // Computed purpose: the nearest authored opinion on the prim or an
  // ancestor; guides never merge into renderable groups.
  const purposeSource = encode(`#usda 1.0
def Xform "Visual" {
  def Mesh "V0" { ${tri} }
  def Mesh "V1" { ${tri} }
}
def Xform "Collision" (
) {
  uniform token purpose = "guide"
  def Mesh "G0" { ${tri} }
  def Mesh "Proxy" {
    uniform token purpose = "proxy"
    ${tri}
  }
  def Cube "GC" {}
}
`);
  for (const merge of [false, true]) {
    nodeMetadataStream.setMeshMerge(merge);
    const purposeLoad = nodeMetadataStream.begin(purposeSource);
    assert.equal(purposeLoad.success, true, purposeLoad.error || nodeMetadataStream.error());
    const byPurpose = {};
    for (let meshId = 0; meshId < nodeMetadataStream.numMeshes(); ++meshId) {
      const purpose = nodeMetadataStream.meshPurpose(meshId);
      assert.equal(nodeMetadataStream.getMesh(meshId).purpose, purpose);
      (byPurpose[purpose] ||= []).push(nodeMetadataStream.getMesh(meshId).primPath);
    }
    if (merge) {
      assert.deepEqual(Object.keys(byPurpose).sort(), ['default', 'guide', 'proxy']);
      assert.equal(byPurpose.default.length, 1, 'visual meshes merge together');
    } else {
      assert.deepEqual(byPurpose, {
        default: ['/Visual/V0', '/Visual/V1'], guide: ['/Collision/G0', '/Collision/GC'],
        proxy: ['/Collision/Proxy']});
    }
    assert.equal(nodeMetadataStream.meshPurpose(9999), '');
    assert.throws(() => nodeMetadataStream.meshPurpose(), /one numeric mesh id/);
    nodeMetadataStream.end();
  }
  nodeMetadataStream.setMeshMerge(false);
} finally { nodeMetadataStream.delete(); }
const instanceQueryStream = new module.RenderStream();
try {
  const source = encode(`#usda 1.0
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
`);
  const result = instanceQueryStream.begin(source);
  assert.equal(result.success, true, result.error || instanceQueryStream.error());
  assert.equal(instanceQueryStream.numInstances(), 2);
  const first = instanceQueryStream.getInstance(0);
  const second = instanceQueryStream.getInstance(1);
  assert.deepEqual(Object.keys(first).sort(), ['primName', 'absPath', 'displayName',
    'prototypeIndex', 'meshId', 'materialId', 'localMatrix', 'globalMatrix', 'visible'].sort());
  assert.equal(first.primName, 'Copies[0]');
  assert.equal(first.absPath, '/World/Copies/instance_0');
  assert.equal(first.prototypeIndex, 0);
  assert.deepEqual(first.localMatrix, [2,0,0,0, 0,3,0,0, 0,0,4,0, 1,2,3,1]);
  assert.deepEqual(first.globalMatrix, [2,0,0,0, 0,3,0,0, 0,0,4,0, 11,22,33,1]);
  assert.deepEqual(second.globalMatrix.slice(12), [14,25,36,1]);
  assert.equal(second.meshId, first.meshId);
  assert.deepEqual(instanceQueryStream.getInstancesForMesh(first.meshId), [0, 1]);
  assert.equal(instanceQueryStream.getInstance(0.9).primName, first.primName);
  assert.deepEqual(instanceQueryStream.getInstancesForMesh(first.meshId + 0.9), [0, 1]);
  assert.equal(instanceQueryStream.getInstance(-1), null);
  assert.equal(instanceQueryStream.getInstance(2), null);
  assert.deepEqual(instanceQueryStream.getInstancesForMesh(-1), []);
} finally { instanceQueryStream.delete(); }
const encodedBmp = stream.encodeImageNative(pngPixels, 1, 1, 4, 'bmp');
assert.deepEqual(Array.from(encodedBmp.subarray(0, 2)), [66, 77]);
const encodedTiff = stream.encodeImageNative(pngPixels, 1, 1, 4, 'tiff');
const tiffView = new DataView(encodedTiff.buffer, encodedTiff.byteOffset, encodedTiff.byteLength);
assert.deepEqual(Array.from(encodedTiff.subarray(0, 4)), [73, 73, 42, 0]);
assert.equal(tiffView.getUint32(4, true), 12);
const tiffIfdOffset = tiffView.getUint32(4, true);
const tiffEntryCount = tiffView.getUint16(tiffIfdOffset, true);
assert.equal(tiffEntryCount, 12);
const tiffEntries = new Map();
for (let i = 0; i < tiffEntryCount; ++i) {
  const at = tiffIfdOffset + 2 + i * 12;
  tiffEntries.set(tiffView.getUint16(at, true), tiffView.getUint32(at + 8, true));
}
assert.equal(tiffEntries.get(273), 8, 'TIFF strip begins after the header');
assert.equal(tiffEntries.get(279), 4, 'TIFF strip retains every input pixel byte');
assert.deepEqual(Array.from(encodedTiff.subarray(8, 12)), Array.from(pngPixels));
assert.deepEqual(stream.encodeImageNative(pngPixels, 1, 1, 4, 'dng'), encodedTiff,
  'DNG uses the same baseline TIFF pixel representation as the legacy encoder');
for (let channels = 1; channels <= 4; ++channels) {
  const pixels = Uint8Array.from({length: channels}, (_, i) => 31 + i * 47);
  const encoded = stream.encodeImageNative(pixels, 1, 1, channels, 'tiff');
  const view = new DataView(encoded.buffer, encoded.byteOffset, encoded.byteLength);
  const ifdOffset = view.getUint32(4, true);
  const count = view.getUint16(ifdOffset, true);
  const entries = new Map();
  for (let i = 0; i < count; ++i) {
    const at = ifdOffset + 2 + i * 12;
    entries.set(view.getUint16(at, true), view.getUint32(at + 8, true));
  }
  assert.equal(entries.get(277), channels);
  assert.equal(entries.get(279), channels);
  assert.deepEqual(Array.from(encoded.subarray(8, 8 + channels)), Array.from(pixels));
  assert.equal(entries.has(338), channels === 2 || channels === 4);
}
const originalImageAlloc = module._lightusd_next_alloc;
const imageAllocSizes = [];
module._lightusd_next_alloc = size => {
  imageAllocSizes.push(Number(size));
  return originalImageAlloc(size);
};
let encodedExr;
try {
  encodedExr = stream.encodeImageNative(new Uint8Array([255, 64, 0]), 1, 1, 3, 'exr');
} finally {
  module._lightusd_next_alloc = originalImageAlloc;
}
const exrEnabled = typeof module._lightusd_next_encoded_image_data === 'function';
assert.deepEqual(imageAllocSizes, [6], exrEnabled
  ? 'EXR encodes once without allocating a second WASM output buffer'
  : 'disabled EXR only stages the input before returning unsupported');
if (exrEnabled) {
  assert.ok(encodedExr instanceof Uint8Array && encodedExr.length > 32,
    'next-only image encoder writes bounded EXR output');
  assert.deepEqual(Array.from(encodedExr.subarray(0, 4)), [0x76, 0x2f, 0x31, 0x01]);
  assert.throws(() => stream.encodeImageNative(new Uint8Array([0, 0]), 1, 1, 2, 'exr'),
    /image encoding failed/, 'EXR rejects unsupported two-channel pixels');
} else {
  assert.equal(encodedExr, null, 'EXR stays explicitly unsupported when disabled');
}
assert.deepEqual(stream.encodeImageNative(pngPixels, 0, 1, 4, 'png'),
  {success: false, error: 'Invalid image dimensions.'});
assert.throws(() => stream.encodeImageNative(pngPixels, 1, 1, 4),
  /wrong argument count/);
const crateProgressFixture = new Uint8Array(fs.readFileSync(
  new URL('../../../tests/usdc/xform-nested-namespaces-000.usdc', import.meta.url)));
const progressStream = new module.RenderStream();
const progressEvents = [];
progressStream.setProgressCallback(event => { progressEvents.push(event); });
const progressLoad = progressStream.begin(crateProgressFixture);
assert.equal(progressLoad.success, true, progressLoad.error || progressStream.error());
assert.ok(progressEvents.length > 0);
assert.equal(progressEvents[0].phase, 'bootstrap');
assert.equal(progressEvents.at(-1).phase, 'complete');
progressStream.end();
let cancelledProgressCalls = 0;
progressStream.setProgressCallback(() => { ++cancelledProgressCalls; return false; });
const cancelledLoad = progressStream.begin(crateProgressFixture);
assert.equal(cancelledLoad.success, false);
assert.match(cancelledLoad.error, /cancelled during bootstrap/);
assert.equal(cancelledProgressCalls, 1);
progressStream.setProgressCallback(() => { throw new Error('progress listener failed'); });
assert.throws(() => progressStream.begin(crateProgressFixture), /progress listener failed/);
progressStream.setProgressCallback(null);
const resumedProgressLoad = progressStream.begin(crateProgressFixture);
assert.equal(resumedProgressLoad.success, true, resumedProgressLoad.error || progressStream.error());
progressEvents.length = 0;
progressStream.setProgressCallback(event => { progressEvents.push(event); });
const usdaProgressLoad = progressStream.begin(encode(`#usda 1.0
def Xform "Root" { def Xform "Child" {} }
`));
assert.equal(usdaProgressLoad.success, true, usdaProgressLoad.error || progressStream.error());
assert.equal(progressEvents[0].phase, 'bootstrap');
assert.ok(progressEvents.some(event => event.phase === 'prims'));
assert.equal(progressEvents.at(-1).phase, 'complete');
progressStream.setProgressCallback(event => event.phase !== 'prims');
const cancelledUsda = progressStream.begin(encode('#usda 1.0\ndef Xform "Cancelled" {}'));
assert.equal(cancelledUsda.success, false);
assert.match(cancelledUsda.error, /USDA parse cancelled during prims/);
progressStream.delete();
const asyncStream = new module.RenderStream();
const asyncSource = encode('#usda 1.0\ndef Xform "Async" {}');
let asyncSettled = false;
const asyncLoadPromise = asyncStream.beginAsync(asyncSource, 'async/scene.usda').then(result => {
  asyncSettled = true;
  return result;
});
assert.equal(asyncSettled, false, 'beginAsync yields before starting its load');
asyncSource.fill(0);
const asyncLoadResult = await asyncLoadPromise;
assert.equal(asyncSettled, true);
assert.equal(asyncLoadResult.success, true, asyncLoadResult.error);
assert.equal(asyncLoadResult.nodeCount, 1);
assert.equal(asyncStream.getURI(), 'async/scene.usda');
asyncStream.setMaxInputBytes(1);
assert.deepEqual(await asyncStream.beginAsync(encode('#usda 1.0\n')),
  {success: false, error: 'Input exceeds configured byte limit'});
assert.equal(asyncStream.getURI(), 'async/scene.usda',
  'rejected async preflight leaves the current scene and source URI intact');
assert.throws(() => asyncStream.beginAsync(), /wrong argument count/);
asyncStream.delete();
const assetBytes = new Uint8Array([99, 10, 20, 30, 99]).subarray(1, 4);
assert.equal(stream.maxInputBytes(), 512 * 1024 * 1024);
stream.setMaxInputBytes(2);
assert.equal(stream.maxInputBytes(), 2);
let stagedInputAllocations = 0;
const streamAlloc = module._lightusd_next_alloc;
module._lightusd_next_alloc = size => {
  if (size === 3) ++stagedInputAllocations;
  return streamAlloc(size);
};
try {
  const rejected = stream.begin(new Uint8Array([1, 2, 3]));
  assert.equal(rejected.success, false);
  assert.match(rejected.error, /configured byte limit/);
  assert.equal(stagedInputAllocations, 0, 'input over the limit is rejected before WASM staging');
} finally { module._lightusd_next_alloc = streamAlloc; }
assert.throws(() => stream.setMaxInputBytes(0), /from 1 through 1 GiB/);
assert.throws(() => stream.maxInputBytes(1), /wrong argument count/);
stream.setMaxInputBytes(512 * 1024 * 1024);
stream.setMaxMemoryLimitMB(1);
const overResidentInput = new Uint8Array(1024 * 1024 + 1);
let residentInputStagingAllocations = 0;
module._lightusd_next_alloc = size => {
  if (size === overResidentInput.byteLength) ++residentInputStagingAllocations;
  return streamAlloc(size);
};
try {
  const rejected = stream.begin(overResidentInput);
  assert.equal(rejected.success, false);
  assert.match(rejected.error, /resident memory/);
  assert.equal(residentInputStagingAllocations, 0,
    'resident-memory preflight rejects input before WASM staging');
  const asyncRejected = await stream.beginAsync(overResidentInput);
  assert.equal(asyncRejected.success, false);
  assert.match(asyncRejected.error, /resident memory/);
  assert.equal(residentInputStagingAllocations, 0,
    'async resident-memory preflight rejects before WASM staging');
} finally { module._lightusd_next_alloc = streamAlloc; }
stream.setMaxMemoryLimitMB(1024);
assert.equal(stream.getMaxMemoryLimitMB(), 1024);
assert.equal(typeof module._lightusd_next_render_remaining_memory_bytes, 'function');
stream.setMaxMemoryLimitMB(1);
assert.equal(stream.getMaxMemoryLimitMB(), 1);
assert.equal(stream.maxInputBytes(), 512 * 1024 * 1024,
  'resident-memory and input-byte limits are independent');
assert.throws(() => stream.setMaxMemoryLimitMB(0), /from 1 through 8192/);
assert.throws(() => stream.getMaxMemoryLimitMB(1), /wrong argument count/);
const largePoints = Array.from({length: 50000}, () => '(0,0,0)').join(',');
const largeMeshUsd = encode(`#usda 1.0\ndef Mesh "Large" {\npoint3f[] points = [${largePoints}]\nint[] faceVertexCounts = [3]\nint[] faceVertexIndices = [0,1,2]\n}`);
const largeLoad = stream.begin(largeMeshUsd);
assert.equal(largeLoad.success, true, largeLoad.error || stream.error());
const largeMeshView = stream.getMeshGeometryView(0);
assert.match(largeMeshView.error, /materialization exceeds configured memory limit/);
stream.end();
stream.setMaxMemoryLimitMB(1024);
const assetBudgetStream = new module.RenderStream();
assert.throws(() => assetBudgetStream.getMMapZeroCopy(), /not mmap-backed/);
assert.throws(() => assetBudgetStream.setMMapZeroCopy(true), /not mmap-backed/);
assert.throws(() => assetBudgetStream.setMMapZeroCopy(1), /expected one boolean/);
assert.throws(() => assetBudgetStream.getMMapZeroCopy(0), /wrong argument count/);
assetBudgetStream.setMaxMemoryLimitMB(1);
assert.equal(assetBudgetStream.startStreamingAsset('streamed.usda', 4), true);
const streamedAssetUuid = assetBudgetStream.getStreamingAssetUUID('streamed.usda');
assert.match(streamedAssetUuid, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
assert.deepEqual(assetBudgetStream.getStreamingProgress('streamed.usda'), {
  exists: true, current: 0, total: 4, complete: false, uuid: streamedAssetUuid, percentage: 0
});
assert.deepEqual(assetBudgetStream.streamingAssetProgress('streamed.usda'),
  {bytesWritten: 0, totalBytes: 4, progress: 0, isComplete: false});
assert.equal(assetBudgetStream.appendStreamingAsset('streamed.usda', new Uint8Array([0, 255])), true);
assert.equal(assetBudgetStream.getStreamingProgress('streamed.usda').percentage, 50);
assert.deepEqual(assetBudgetStream.streamingAssetProgress('streamed.usda'),
  {bytesWritten: 2, totalBytes: 4, progress: 0.5, isComplete: false});
assert.equal(assetBudgetStream.finalizeStreamingAsset('streamed.usda'), false);
assert.equal(assetBudgetStream.appendStreamingAsset('streamed.usda', new Uint8Array([128, 1])), true);
assert.equal(assetBudgetStream.finalizeStreamingAsset('streamed.usda'), true);
assert.deepEqual(assetBudgetStream.getStreamingProgress('streamed.usda'), {exists: false});
assert.equal(assetBudgetStream.getStreamingAssetUUID('streamed.usda'), '');
assert.deepEqual(Array.from(assetBudgetStream.getProvidedAsset('streamed.usda')), [0, 255, 128, 1]);
assert.equal(assetBudgetStream.startStreamingAsset('cancelled.usda', 8), true);
assert.equal(assetBudgetStream.cancelStreamingAsset('cancelled.usda'), true);
assert.equal(assetBudgetStream.streamingAssetProgress('cancelled.usda'), null);
assert.deepEqual(assetBudgetStream.getStreamingProgress('cancelled.usda'), {exists: false});
const streamedRoot = new module.RenderStream();
const streamedRootBytes = encode('#usda 1.0\ndef Xform "StreamedRoot" {}\n');
assert.equal(streamedRoot.startStreamingAsset('root.usda', streamedRootBytes.length), true);
assert.equal(streamedRoot.appendStreamingAsset('root.usda', streamedRootBytes.subarray(0, 7)), true);
assert.equal(streamedRoot.beginStreamedAsset('root.usda').success, false,
  'an incomplete streamed root remains available for more chunks');
assert.equal(streamedRoot.getURI(), 'root.usda');
assert.equal(streamedRoot.appendStreamingAsset('root.usda', streamedRootBytes.subarray(7)), true);
const loadedStreamedRoot = streamedRoot.beginStreamedAsset('root.usda');
assert.equal(loadedStreamedRoot.success, true, loadedStreamedRoot.error);
assert.equal(streamedRoot.streamingAssetProgress('root.usda'), null,
  'the root buffer ownership transfers into the loaded stage');
const borrowedStream = new module.RenderStream();
assert.equal(borrowedStream.startStreamingAsset('borrowed.bin', 4), true);
const borrowedView = borrowedStream.getStreamingAssetView('borrowed.bin');
assert.ok(borrowedView instanceof Uint8Array);
borrowedView.set([0, 127, 128, 255]);
assert.equal(borrowedStream.streamingAssetProgress('borrowed.bin').bytesWritten, 0,
  'borrowed writes are counted only when explicitly marked');
assert.equal(borrowedStream.markStreamingAssetBytesWritten('borrowed.bin', 5), false);
assert.equal(borrowedStream.markStreamingAssetBytesWritten('borrowed.bin', 4), true);
assert.equal(borrowedStream.streamingAssetProgress('borrowed.bin').isComplete, true);
assert.equal(borrowedStream.finalizeStreamingAsset('borrowed.bin'), true);
assert.deepEqual(Array.from(borrowedStream.getProvidedAsset('borrowed.bin')),
  [0, 127, 128, 255]);
borrowedStream.delete();
const randomWriteStream = new module.RenderStream();
assert.equal(randomWriteStream.startStreamingAsset('random.bin', 8), true);
assert.throws(() => randomWriteStream.markStreamingAssetRangeWritten(
  'random.bin', 0x100000000, 1), /expected non-negative offset/);
randomWriteStream.getStreamingAssetViewAt('random.bin', 4, 4).set([4, 5, 6, 7]);
assert.equal(randomWriteStream.markStreamingAssetRangeWritten('random.bin', 4, 4), true);
assert.equal(randomWriteStream.streamingAssetProgress('random.bin').bytesWritten, 4);
randomWriteStream.getStreamingAssetViewAt('random.bin', 2, 4).set([2, 3, 4, 5]);
assert.equal(randomWriteStream.markStreamingAssetRangeWritten('random.bin', 2, 4), true);
assert.equal(randomWriteStream.streamingAssetProgress('random.bin').bytesWritten, 6,
  'overlapping ranges count each written byte once');
assert.equal(randomWriteStream.finalizeStreamingAsset('random.bin'), false,
  'overlapping ranges cannot hide an unwritten prefix');
randomWriteStream.getStreamingAssetViewAt('random.bin', 0, 2).set([0, 1]);
assert.equal(randomWriteStream.markStreamingAssetRangeWritten('random.bin', 0, 2), true);
assert.equal(randomWriteStream.finalizeStreamingAsset('random.bin'), true);
assert.deepEqual(Array.from(randomWriteStream.getProvidedAsset('random.bin')),
  [0, 1, 2, 3, 4, 5, 6, 7]);
randomWriteStream.delete();
const zeroCopyCompat = new module.RenderStream();
assert.equal(zeroCopyCompat.startStreamingAsset('chunk-alias.bin', 1), true);
assert.equal(zeroCopyCompat.appendAssetChunk('chunk-alias.bin', new Uint8Array([7])), true);
assert.equal(zeroCopyCompat.finalizeStreamingAsset('chunk-alias.bin'), true);
const zeroCopy = zeroCopyCompat.allocateZeroCopyBuffer('legacy-zero-copy.bin', 4);
assert.equal(zeroCopy.success, true);
assert.match(zeroCopy.uuid, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
assert.equal(zeroCopyCompat.getZeroCopyBufferPtr(zeroCopy.uuid), zeroCopy.bufferPtr);
assert.equal(zeroCopyCompat.getZeroCopyBufferPtrAtOffset(zeroCopy.uuid, 2), zeroCopy.bufferPtr + 2);
assert.deepEqual(zeroCopyCompat.getActiveZeroCopyBuffers().map(item => item.uuid), [zeroCopy.uuid]);
new Uint8Array(module.HEAPU8.buffer, zeroCopy.bufferPtr, 4).set([10, 20, 30, 40]);
assert.equal(zeroCopyCompat.markZeroCopyBytesWritten(zeroCopy.uuid, 4), true);
assert.equal(zeroCopyCompat.getZeroCopyProgress(zeroCopy.uuid).isComplete, true);
assert.equal(zeroCopyCompat.isStreamingAssetComplete('legacy-zero-copy.bin'), true);
assert.equal(zeroCopyCompat.finalizeZeroCopyBuffer(zeroCopy.uuid), true);
assert.deepEqual(Array.from(zeroCopyCompat.getProvidedAsset('legacy-zero-copy.bin')), [10, 20, 30, 40]);
assert.deepEqual(zeroCopyCompat.getZeroCopyProgress(zeroCopy.uuid), {exists: false});
assert.deepEqual(zeroCopyCompat.getActiveZeroCopyBuffers(), []);
assert.deepEqual(zeroCopyCompat.allocateZeroCopyBuffer('empty', 0),
  {success: false, error: 'Size must be greater than 0'});
assert.equal(zeroCopyCompat.startStreamingAsset('legacy-cancel.bin', 1), true);
const cancelUuid = zeroCopyCompat.getStreamingAssetUUID('legacy-cancel.bin');
assert.equal(zeroCopyCompat.cancelZeroCopyBuffer(cancelUuid), true);
assert.equal(zeroCopyCompat.getZeroCopyBufferPtr(cancelUuid), 0);
zeroCopyCompat.delete();
const handoffStore = new module.NextAssetStore();
handoffStore.setMemoryLimitBytes(2);
const handoffStream = new module.RenderStream();
assert.equal(handoffStream.startStreamingAsset('handoff.bin', 3), true);
const handoffUuid = handoffStream.getStreamingAssetUUID('handoff.bin');
assert.equal(handoffStream.finalizeStreamingAssetToStore('handoff.bin', handoffStore), false,
  'an incomplete transfer cannot be adopted by the store');
assert.equal(handoffStream.appendAssetChunk('handoff.bin', new Uint8Array([1, 0, 255])), true);
assert.throws(() => handoffStream.finalizeStreamingAssetToStore('handoff.bin', handoffStore),
  /Asset store memory limit exceeded/);
assert.equal(handoffStream.getStreamingProgress('handoff.bin').uuid, handoffUuid,
  'a rejected handoff keeps the transfer active');
handoffStore.setMemoryLimitBytes(3);
assert.equal(handoffStream.finalizeStreamingAssetToStore('handoff.bin', handoffStore), true);
assert.deepEqual(Array.from(handoffStore.readMemoryAsset('handoff.bin')), [1, 0, 255]);
assert.equal(handoffStore.getAssetUUID('handoff.bin'), handoffUuid,
  'store adoption preserves the streaming UUID');
assert.deepEqual(handoffStream.getStreamingProgress('handoff.bin'), {exists: false});
assert.deepEqual(handoffStore.memoryStats(), {assetCount: 1, bytesUsed: 3, limitBytes: 3});
handoffStream.delete();
handoffStore.delete();
const metadataHandoffStore = new module.NextAssetStore();
metadataHandoffStore.setMetadataLimitBytes(256);
const metadataHandoffStream = new module.RenderStream();
assert.equal(metadataHandoffStream.startStreamingAsset('meta-handoff.bin', 1), true);
assert.equal(metadataHandoffStream.appendAssetChunk('meta-handoff.bin', new Uint8Array([9])), true);
assert.throws(() => metadataHandoffStream.finalizeStreamingAssetToStore(
  'meta-handoff.bin', metadataHandoffStore), /Asset store metadata limit exceeded/);
assert.equal(metadataHandoffStream.streamingAssetProgress('meta-handoff.bin').bytesWritten, 1,
  'metadata-budget failure leaves a completed transfer available for retry');
metadataHandoffStore.setMetadataLimitBytes(512);
assert.equal(metadataHandoffStream.finalizeStreamingAssetToStore(
  'meta-handoff.bin', metadataHandoffStore), true);
assert.equal(metadataHandoffStore.metadataBytesUsed(), 272n);
metadataHandoffStream.delete();
metadataHandoffStore.delete();
const transientStore = new module.NextAssetStore();
const transientStream = new module.RenderStream();
transientStream.setMaxMemoryLimitMB(1);
const transientSize = 700 * 1024;
assert.equal(transientStream.startStreamingAsset('transient.bin', transientSize), true);
const transientView = transientStream.getStreamingAssetView('transient.bin', transientSize);
transientView.fill(23);
assert.equal(transientStream.markStreamingAssetBytesWritten('transient.bin', transientSize), true);
assert.throws(() => transientStream.finalizeStreamingAssetToStore('transient.bin', transientStore),
  /additional resident memory/);
assert.equal(transientStream.streamingAssetProgress('transient.bin').bytesWritten, transientSize,
  'transient-memory rejection keeps all completed stream bytes available');
transientStream.setMaxMemoryLimitMB(2);
assert.equal(transientStream.finalizeStreamingAssetToStore('transient.bin', transientStore), true);
const transientAdopted = transientStore.getAssetCacheDataAsMemoryView('transient.bin');
assert.equal(transientAdopted.length, transientSize);
assert.equal(transientAdopted[0], 23);
assert.equal(transientAdopted[transientSize - 1], 23);
transientStream.delete();
transientStore.delete();
const fragmentedStream = new module.RenderStream();
assert.equal(fragmentedStream.startStreamingAsset('fragmented.bin', 257), true);
for (let i = 0; i < 128; ++i) {
  fragmentedStream.getStreamingAssetViewAt('fragmented.bin', i * 2, 1)[0] = i;
  assert.equal(fragmentedStream.markStreamingAssetRangeWritten('fragmented.bin', i * 2, 1), true);
}
fragmentedStream.getStreamingAssetViewAt('fragmented.bin', 256, 1)[0] = 0;
assert.throws(() => fragmentedStream.markStreamingAssetRangeWritten('fragmented.bin', 256, 1),
  /too many disjoint ranges/);
fragmentedStream.delete();
const cappedStream = new module.RenderStream();
cappedStream.setProvidedAssetByteLimit(3);
assert.throws(() => cappedStream.startStreamingAsset('too-large.usda', 4), /memory limit/);
assert.deepEqual(cappedStream.providedAssetNames(), []);
const replaceBudgetStream = new module.RenderStream();
replaceBudgetStream.setMaxMemoryLimitMB(1);
assert.equal(replaceBudgetStream.startStreamingAsset('large.bin', 800 * 1024), true);
const retainedStreamUuid = replaceBudgetStream.getStreamingAssetUUID('large.bin');
assert.throws(() => replaceBudgetStream.startStreamingAsset('large.bin', 800 * 1024), /memory limit/);
assert.equal(replaceBudgetStream.getStreamingAssetUUID('large.bin'), retainedStreamUuid);
assert.deepEqual(replaceBudgetStream.streamingAssetProgress('large.bin'),
  {bytesWritten: 0, totalBytes: 800 * 1024, progress: 0, isComplete: false},
  'rejected replacement preserves the active transfer');
replaceBudgetStream.delete();
assetBudgetStream.provideAsset('clip.usda', new Uint8Array(900 * 1024));
const paddedRoot = encode('#usda 1.0\n' + '# resident budget check\n'.repeat(9000));
const overAggregateBudget = assetBudgetStream.begin(paddedRoot);
assert.equal(overAggregateBudget.success, false);
assert.match(overAggregateBudget.error, /provided assets leave no resident memory/);
assetBudgetStream.end();
const compositionUsd = `#usda 1.0
class Mesh "_Base" {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
}
def Mesh "Derived" ( add inherits = </_Base> ) { }
`;
const uncomposedStream = new module.RenderStream();
uncomposedStream.setEnableComposition(false);
const uncomposedResult = uncomposedStream.begin(encode(compositionUsd));
assert.equal(uncomposedResult.success, true, uncomposedResult.error);
assert.equal(uncomposedResult.meshCount, 2);
assert.equal(uncomposedStream.hasInherits(), true);
assert.deepEqual([uncomposedStream.meshVertexCount(0),
  uncomposedStream.meshVertexCount(1)], [3, 0]);
const composedStream = new module.RenderStream();
composedStream.setEnableComposition(true);
const composedResult = composedStream.begin(encode(compositionUsd));
assert.equal(composedResult.success, true, composedResult.error);
assert.equal(composedResult.meshCount, 2);
assert.equal(composedStream.hasInherits(), true,
  'inherit presence reflects the authored root layer after composition');
assert.deepEqual([composedStream.meshVertexCount(0),
  composedStream.meshVertexCount(1)], [3, 3]);
const composedNodesBeforeRelease = composedStream.numNodes();
composedStream.releaseSourceLayer();
assert.equal(composedStream.numNodes(), composedNodesBeforeRelease,
  'releaseSourceLayer preserves the composed Stage and its render scene');
const sharedAssetStore = new module.NextAssetStore();
sharedAssetStore.registerMemoryAsset('dep.usda', encode(`#usda 1.0
def Xform "Base" {
  def Mesh "Geo" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  }
}`));
sharedAssetStore.setAlias('alias.usda', 'dep.usda');
const importedAssetStream = new module.RenderStream();
assert.equal(importedAssetStream.importAssetStore(sharedAssetStore), true);
const overBudgetImport = new module.RenderStream();
overBudgetImport.setProvidedAssetByteLimit(1);
assert.throws(() => overBudgetImport.importAssetStore(sharedAssetStore),
  /Imported assets exceed configured memory limit/);
assert.equal(sharedAssetStore.unregisterMemoryAsset('dep.usda'), true,
  'the RenderStream import retains an owning view of the shared payload');
const importedRoot = encode(`#usda 1.0
def Xform "Root" ( references = @alias.usda@</Base> ) { }
`);
const importedComposition = importedAssetStream.begin(importedRoot);
assert.equal(importedComposition.success, true, importedComposition.error);
assert.ok(importedComposition.meshCount > 0,
  'an aliased store asset resolves as a composition dependency');
importedAssetStream.delete();
overBudgetImport.delete();
sharedAssetStore.delete();
const liveAssetStore = new module.NextAssetStore();
liveAssetStore.registerMemoryAsset('dep.usda', encode(`#usda 1.0
def Xform "Base" {
  def Mesh "Geo" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  }
}`));
const liveAssetStream = new module.RenderStream();
liveAssetStream.setAssetStore(liveAssetStore);
const liveAssetRoot = encode(`#usda 1.0
def Xform "Root" ( references = @dep.usda@</Base> ) { }
`);
liveAssetStore.registerMemoryAsset('root.usda', liveAssetRoot);
const attachedStoreStats = liveAssetStream.getMemoryStats();
assert.equal(attachedStoreStats.assetCacheCount, liveAssetStore.memoryStats().assetCount);
assert.equal(attachedStoreStats.assetCacheSizeBytes, liveAssetStore.memoryStats().bytesUsed);
assert.equal(attachedStoreStats.assetCacheMaxBytes, liveAssetStore.memoryStats().limitBytes);
const cachedRootStream = new module.RenderStream();
const cachedRootLoad = cachedRootStream.beginCachedAsset(liveAssetStore, 'root.usda');
assert.equal(cachedRootLoad.success, true, cachedRootLoad.error);
assert.equal(cachedRootStream.getURI(), 'root.usda');
assert.equal(cachedRootLoad.meshCount, 1,
  'cached root loads retain the store for external composition dependencies');
assert.deepEqual(cachedRootStream.beginCachedAsset(liveAssetStore, 'missing.usda'), {
  success: false, error: 'Asset not found in cache'
});
assert.equal(cachedRootStream.getURI(), 'missing.usda');
const limitedCachedStream = new module.RenderStream();
limitedCachedStream.setMaxInputBytes(1);
assert.deepEqual(limitedCachedStream.beginCachedAsset(liveAssetStore, 'root.usda'), {
  success: false, error: 'Input exceeds configured byte limit'
});
cachedRootStream.delete();
limitedCachedStream.delete();
const parentPathStore = new module.NextAssetStore();
parentPathStore.registerMemoryAsset('../dep.usda', encode(`#usda 1.0
def Xform "Base" {
  def Mesh "Geo" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  }
}`));
parentPathStore.registerMemoryAsset('scene/root.usda', encode(`#usda 1.0
def Xform "Root" ( references = @../dep.usda@</Base> ) { }
`));
const parentDeniedStream = new module.RenderStream();
parentPathStore.setAllowParentRelativeAssetPaths(false);
const parentDeniedLoad = parentDeniedStream.beginCachedAsset(parentPathStore, 'scene/root.usda');
assert.equal(parentDeniedLoad.success, true, parentDeniedLoad.error);
assert.equal(parentDeniedLoad.meshCount, 0,
  'parent-relative asset policy is imported into the render resolver');
const parentAllowedStream = new module.RenderStream();
parentPathStore.setAllowParentRelativeAssetPaths(true);
const parentAllowedLoad = parentAllowedStream.beginCachedAsset(parentPathStore, 'scene/root.usda');
assert.equal(parentAllowedLoad.success, true, parentAllowedLoad.error);
assert.equal(parentAllowedLoad.meshCount, 1, JSON.stringify(parentAllowedLoad) +
  'enabling parent-relative paths allows the referenced memory asset');
parentDeniedStream.delete();
parentAllowedStream.delete();
parentPathStore.delete();
const firstLiveComposition = liveAssetStream.begin(liveAssetRoot);
assert.equal(firstLiveComposition.success, true, firstLiveComposition.error);
assert.equal(firstLiveComposition.meshCount, 1);
liveAssetStore.registerMemoryAsset('dep.usda', encode('#usda 1.0\ndef Xform "Base" { int revision = 2 }'));
const refreshedComposition = liveAssetStream.begin(liveAssetRoot);
assert.equal(refreshedComposition.success, true, refreshedComposition.error);
assert.equal(refreshedComposition.meshCount, 0,
  'attached asset stores refresh their shared snapshot before the next load');
liveAssetStore.registerMemoryAsset('dep.usda', encode(`#usda 1.0
def Xform "Base" {
  def Mesh "Geo" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  }
}`));
const restoredComposition = liveAssetStream.begin(liveAssetRoot);
assert.equal(restoredComposition.success, true, restoredComposition.error);
assert.equal(restoredComposition.meshCount, 1);
liveAssetStream.provideAsset('local.bin', new Uint8Array([4, 5, 6]));
assert.equal(liveAssetStream.detachAssetStore(), true);
assert.deepEqual(liveAssetStream.getProvidedAsset('local.bin'),
  new Uint8Array([4, 5, 6]), 'detaching preserves stream-local provided assets');
const detachedComposition = liveAssetStream.begin(liveAssetRoot);
assert.equal(detachedComposition.success, true, detachedComposition.error);
assert.equal(detachedComposition.meshCount, 0,
  'detaching removes imported dependencies from future loads');
assert.throws(() => liveAssetStream.detachAssetStore(true), /wrong argument count/);
liveAssetStream.delete();
liveAssetStore.delete();
const heapGrowthStore = new module.NextAssetStore();
const heapGrowthStream = new module.RenderStream();
heapGrowthStream.setAssetStore(heapGrowthStore);
const heapGrowthInput = encode('#usda 1.0\ndef Xform "HeapBacked" {}');
const heapGrowthPtr = module._lightusd_next_alloc(heapGrowthInput.byteLength);
assert.ok(heapGrowthPtr);
module.HEAPU8.set(heapGrowthInput, Number(heapGrowthPtr));
const heapGrowthView = new Uint8Array(module.HEAPU8.buffer,
  Number(heapGrowthPtr), heapGrowthInput.byteLength);
const priorHeapBuffer = module.HEAPU8.buffer;
const importStoreExport = module._lightusd_next_render_import_asset_store;
let forcedHeapGrowth = false;
module._lightusd_next_render_import_asset_store = (...args) => {
  const status = importStoreExport(...args);
  if (!forcedHeapGrowth) {
    const growthPtr = module._lightusd_next_alloc(module.HEAPU8.byteLength);
    assert.ok(growthPtr, 'heap pressure allocation succeeds');
    module._lightusd_next_free(growthPtr);
    forcedHeapGrowth = module.HEAPU8.buffer !== priorHeapBuffer;
  }
  return status;
};
try {
  const loadedAfterGrowth = heapGrowthStream.begin(heapGrowthView);
  assert.equal(forcedHeapGrowth, true, 'store refresh grows WASM memory after view capture');
  assert.equal(loadedAfterGrowth.success, true, loadedAfterGrowth.error);
  assert.equal(loadedAfterGrowth.nodeCount, 1,
    'the root view is rebound by heap offset after memory growth');
} finally {
  module._lightusd_next_render_import_asset_store = importStoreExport;
  module._lightusd_next_free(heapGrowthPtr);
  heapGrowthStream.delete();
  heapGrowthStore.delete();
}
const sharedBudgetStore = new module.NextAssetStore();
sharedBudgetStore.setMemoryLimitBytes(4);
sharedBudgetStore.registerMemoryAsset('held.bin', new Uint8Array([1, 2, 3]));
const sharedBudgetStream = new module.RenderStream();
sharedBudgetStream.setAssetStore(sharedBudgetStore);
assert.throws(() => sharedBudgetStream.startStreamingAsset('next.bin', 2),
  /Shared asset payload budget exceeded/,
  'store payloads and stream allocations share a retained-byte cap');
sharedBudgetStore.unregisterMemoryAsset('held.bin');
sharedBudgetStream.importAssetStore(sharedBudgetStore);
assert.equal(sharedBudgetStream.startStreamingAsset('next.bin', 2), true,
  'refreshing the store releases leases for payloads no longer retained');
sharedBudgetStream.cancelStreamingAsset('next.bin');
sharedBudgetStream.delete();
sharedBudgetStore.delete();
const sharedHandoffStore = new module.NextAssetStore();
sharedHandoffStore.setMemoryLimitBytes(3);
const sharedHandoffStream = new module.RenderStream();
sharedHandoffStream.setAssetStore(sharedHandoffStore);
assert.equal(sharedHandoffStream.startStreamingAsset('move.bin', 3), true);
assert.equal(sharedHandoffStream.appendAssetChunk('move.bin', new Uint8Array([4, 5, 6])), true);
assert.equal(sharedHandoffStream.finalizeStreamingAssetToStore('move.bin', sharedHandoffStore), true,
  'store handoff transfers the existing shared-budget lease');
assert.throws(() => sharedHandoffStream.startStreamingAsset('over.bin', 1),
  /Shared asset payload budget exceeded/);
sharedHandoffStream.delete();
sharedHandoffStore.delete();
assert.throws(() => composedStream.setEnableComposition(), /wrong argument count/);
assert.throws(() => composedStream.setEnableComposition.call({}, true), /Invalid LightUSD receiver/);
const generatedSphereUsd = encode('#usda 1.0\ndef Xform "World" { def Sphere "Ball" {} }');
const sphereVertexCount = level => {
  const sphereStream = new module.RenderStream();
  assert.equal(sphereStream.getSphereSubdivisions(), 4);
  sphereStream.setSphereSubdivisions(level);
  assert.equal(sphereStream.getSphereSubdivisions(), level);
  const result = sphereStream.begin(generatedSphereUsd);
  assert.equal(result.success, true, result.error || sphereStream.error());
  const count = sphereStream.meshVertexCount(0);
  sphereStream.end();
  return count;
};
assert.ok(sphereVertexCount(2) > sphereVertexCount(0));
assert.throws(() => stream.setSphereSubdivisions(7), /setting rejected/);
assert.throws(() => stream.setSphereSubdivisions(1.5), /expected one integer/);
assert.equal(stream.getEnableBoneReduction(), false);
assert.equal(stream.getTargetBoneCount(), 4);
const skinFixture = new Uint8Array(fs.readFileSync(
  new URL('../../../tests/usda/skeleton-binding-001.usda', import.meta.url)));
const skinInfluenceLengths = reduce => {
  const skinStream = new module.RenderStream();
  skinStream.setEnableBoneReduction(reduce);
  skinStream.setTargetBoneCount(2);
  const result = skinStream.begin(skinFixture);
  assert.equal(result.success, true, result.error || skinStream.error());
  const lengths = [];
  for (let meshId = 0; meshId < skinStream.meshCount(); ++meshId) {
    if (skinStream.meshHasSkin(meshId)) {
      lengths.push(skinStream.meshJointWeightsBuffer(meshId).length);
    }
  }
  skinStream.end();
  return lengths;
};
const roundedSkinWidths = enabled => {
  const skinStream = new module.RenderStream();
  skinStream.setRoundBoneCount(enabled);
  assert.equal(skinStream.getRoundBoneCount(), enabled);
  const result = skinStream.begin(skinFixture);
  assert.equal(result.success, true, result.error || skinStream.error());
  const widths = [];
  for (let meshId = 0; meshId < skinStream.meshCount(); ++meshId) {
    if (skinStream.meshHasSkin(meshId)) {
      widths.push(skinStream.getMeshGeometryView(meshId).elementSize);
    }
  }
  skinStream.end();
  return widths;
};
const fullSkinWeights = skinInfluenceLengths(false);
const reducedSkinWeights = skinInfluenceLengths(true);
assert.ok(fullSkinWeights.length > 0, 'fixture includes bound skinned meshes');
assert.ok(Math.max(...reducedSkinWeights) < Math.max(...fullSkinWeights),
  'bone reduction should lower retained per-mesh influence payload');
assert.deepEqual(roundedSkinWidths(false), [4, 2]);
assert.deepEqual(roundedSkinWidths(true), [4, 4]);
assert.throws(() => stream.setTargetBoneCount(0), /setting rejected/);
assert.throws(() => stream.setEnableBoneReduction(1), /expected boolean/);
assert.throws(() => stream.setRoundBoneCount(1), /expected boolean/);
assert.equal(stream.getValueClipSampleRate(), 0);
assert.equal(stream.getValueClipUseTimeRange(), false);
assert.equal(stream.getValueClipStartTime(), 0);
assert.equal(stream.getValueClipEndTime(), 0);
stream.setValueClipSampleRate(2);
stream.setValueClipUseTimeRange(true);
stream.setValueClipTimeRange(0, 1);
assert.equal(stream.getValueClipSampleRate(), 2);
assert.equal(stream.getValueClipUseTimeRange(), true);
assert.equal(stream.getValueClipStartTime(), 0);
assert.equal(stream.getValueClipEndTime(), 1);
assert.throws(() => stream.setValueClipSampleRate(-1), /setting rejected/);
assert.throws(() => stream.setValueClipUseTimeRange(1), /expected boolean/);
assert.throws(() => stream.setValueClipTimeRange(0, Infinity), /two finite numbers/);
const valueClipRoot = [
  '#usda 1.0', 'def Xform "World" {',
  '  def Xform "Mover" ( clips = {',
  '    dictionary default_clip = {',
  '      double2[] active = [(0, 0), (1, 1)]',
  '      asset[] assetPaths = [@wide.usda@, @tall.usda@]',
  '      string primPath = "/World/Mover"',
  '      double2[] times = [(0, 0), (1, 1)]',
  '    }',
  '  } ) {',
  '    double3 xformOp:translate = (0, 0, 0)',
  '    uniform token[] xformOpOrder = ["xformOp:translate"]',
  '  }', '}'
].join('\n');
const wideClip = [
  '#usda 1.0', 'def Xform "World" {', '  def Xform "Mover" {',
  '    double3 xformOp:translate = (0, 0, 0)',
  '    uniform token[] xformOpOrder = ["xformOp:translate"]',
  '  }', '}'
].join('\n');
const tallClip = [
  '#usda 1.0', 'def Xform "World" {', '  def Xform "Mover" {',
  '    double3 xformOp:translate = (10, 0, 0)',
  '    uniform token[] xformOpOrder = ["xformOp:translate"]',
  '  }', '}'
].join('\n');
const loadValueClip = (configured, clipsEnabled = true) => {
  const clipStream = new module.RenderStream();
  assert.equal(clipStream.getEnableValueClips(), true);
  clipStream.setEnableValueClips(clipsEnabled);
  assert.equal(clipStream.getEnableValueClips(), clipsEnabled);
  clipStream.provideAsset('wide.usda', encode(wideClip));
  clipStream.provideAsset('tall.usda', encode(tallClip));
  if (configured) {
    clipStream.setValueClipSampleRate(2);
    clipStream.setValueClipUseTimeRange(true);
    clipStream.setValueClipTimeRange(0, 1);
  }
  const loaded = clipStream.begin(encode(valueClipRoot));
  assert.equal(loaded.success, true, loaded.error || clipStream.error());
  if (!clipsEnabled) {
    assert.equal(clipStream.numAnimations(), 0);
    return [];
  }
  assert.equal(clipStream.numAnimations(), 1);
  assert.equal(clipStream.animationChannelCount(0), 1);
  return Array.from(clipStream.animationKeyframeTimesBuffer(0, 0));
};
assert.deepEqual(loadValueClip(false), [0, 1]);
assert.deepEqual(loadValueClip(true), [0, 0.5, 1]);
assert.deepEqual(loadValueClip(false, false), []);
const clipFixturePath = '../../../examples/lusdview/tests/fixtures/value-clip/';
const clipFixture = name => new Uint8Array(fs.readFileSync(
  new URL(clipFixturePath + name, import.meta.url)));
const loadClippedCurves = enabled => {
  const curveStream = new module.RenderStream();
  curveStream.setEnableValueClips(enabled);
  curveStream.provideAsset('clip_wide.usda', clipFixture('clip_wide.usda'));
  curveStream.provideAsset('clip_tall.usda', clipFixture('clip_tall.usda'));
  const result = curveStream.begin(clipFixture('main.usda'));
  assert.equal(result.success, true, result.error || curveStream.error());
  const count = curveStream.curvesCount();
  curveStream.end();
  return count;
};
assert.equal(loadClippedCurves(true), 1);
assert.equal(loadClippedCurves(false), 0);
assert.throws(() => stream.setEnableValueClips(), /wrong argument count/);
stream.setProvidedAssetByteLimit(23);
assert.equal(stream.providedAssetByteLimit(), 23);
stream.provideAsset('./clip.usda', assetBytes);
assert.equal(stream.getStats().providedAssetBytes, 3);
assert.deepEqual(stream.getProvidedAsset('clip.usda'), new Uint8Array([10, 20, 30]));
assetBytes.fill(77);
assert.deepEqual(stream.getProvidedAsset('./clip.usda'), new Uint8Array([10, 20, 30]));
const assetPtr = module._lightusd_next_alloc(2);
try {
  module.HEAPU8.set([40, 50], Number(assetPtr));
  const heapView = module.HEAPU8.subarray(Number(assetPtr), Number(assetPtr) + 2);
  const alloc = module._lightusd_next_alloc;
  let assetAllocSize = 0;
  module._lightusd_next_alloc = size => { assetAllocSize = size; return alloc(size); };
  try { stream.provideAsset('./heap.usda', heapView); }
  finally { module._lightusd_next_alloc = alloc; }
  assert.equal(assetAllocSize, new TextEncoder().encode('./heap.usda').length,
    'heap-backed asset input only allocates a name buffer');
  assert.equal(stream.getStats().providedAssetBytes, 5);
  assert.deepEqual(stream.getProvidedAsset('heap.usda'), new Uint8Array([40, 50]));
  assert.throws(() => stream.provideAsset('extra', new Uint8Array([1])), /asset rejected/);
  assert.throws(() => stream.provideAsset('clip.usda', new Uint8Array([1, 2, 3, 4])), /asset rejected/);
  assert.deepEqual(stream.providedAssetNames(), ['clip.usda', 'heap.usda']);
  const assetCountQuery = module._lightusd_next_render_provided_asset_count;
  const allocate = module._lightusd_next_alloc;
  let aggregateAllocations = 0;
  module._lightusd_next_render_provided_asset_count = () => 65537;
  module._lightusd_next_alloc = size => { ++aggregateAllocations; return allocate(size); };
  try {
    assert.throws(() => stream.providedAssetNames(), /excessive asset count/);
    assert.equal(aggregateAllocations, 0,
      'providedAssetNames rejects excessive counts before allocating records');
  } finally {
    module._lightusd_next_render_provided_asset_count = assetCountQuery;
    module._lightusd_next_alloc = allocate;
  }
  assert.throws(() => stream.setProvidedAssetByteLimit(22), /below current asset use/);
  assert.equal(stream.providedAssetByteLimit(), 23);
} finally {
  module._lightusd_next_free(assetPtr);
}
assert.deepEqual(stream.getProvidedAsset('./heap.usda'), new Uint8Array([40, 50]));
assert.throws(() => stream.getProvidedAsset('missing.usda'), /asset not found/);
assert.throws(() => stream.getProvidedAsset(), /expected one string name/);
assert.throws(() => stream.getProvidedAsset.call({}, 'heap.usda'), /Invalid LightUSD receiver/);
assert.equal(stream.removeAsset('./clip.usda'), true);
assert.equal(stream.getStats().providedAssetBytes, 2);
assert.deepEqual(stream.providedAssetNames(), ['heap.usda']);
assert.equal(stream.removeAsset('clip.usda'), false);
assert.equal(stream.getStats().providedAssetBytes, 2);
assert.throws(() => stream.provideAsset('./clip.usda', null), /expected byte view/);
assert.throws(() => stream.provideAsset(1, assetBytes), /expected string/);
assert.throws(() => stream.provideAsset.call({}, 'clip.usda', assetBytes), /Invalid LightUSD receiver/);
assert.throws(() => stream.removeAsset(), /wrong argument count/);
assert.throws(() => stream.removeAsset(''), /non-empty name/);
assert.throws(() => stream.removeAsset(1), /expected string/);
assert.throws(() => stream.removeAsset.call({}, 'clip.usda'), /Invalid LightUSD receiver/);
stream.clearAssets();
assert.equal(stream.getStats().providedAssetBytes, 0);
assert.deepEqual(stream.providedAssetNames(), []);
assert.equal(stream.providedAssetByteLimit(), 23);
stream.provideAsset('', new Uint8Array(0));
assert.deepEqual(stream.getProvidedAsset(''), new Uint8Array(0));
stream.clearAssets();
assert.throws(() => stream.getProvidedAsset(''), /asset not found/);
assert.throws(() => stream.setProvidedAssetByteLimit(), /uint32 limit/);
assert.throws(() => stream.providedAssetByteLimit(1), /wrong argument count/);
assert.throws(() => stream.providedAssetNames(1), /wrong argument count/);
assert.throws(() => stream.providedAssetNames.call({}), /Invalid LightUSD receiver/);
const resetStream = new module.RenderStream();
try {
  resetStream.setMeshMerge(true);
  resetStream.provideAsset('retained.usda', new Uint8Array([1, 2, 3]));
  const resetLoad = resetStream.begin(encode('#usda 1.0\ndef Xform "Root" {}'));
  assert.equal(resetLoad.success, true, resetLoad.error || resetStream.error());
  assert.ok(resetStream.numNodes() > 0);
  assert.deepEqual(resetStream.providedAssetNames(), ['retained.usda']);
  const nodeCountBeforeRelease = resetStream.numNodes();
  resetStream.releaseSourceLayer();
  assert.equal(resetStream.numNodes(), nodeCountBeforeRelease,
    'releaseSourceLayer keeps the composed Stage available');
  assert.throws(() => resetStream.releaseSourceLayer(1), /wrong argument count/);
  resetStream.reset();
  assert.equal(resetStream.numNodes(), 0, 'reset releases the converted scene');
  assert.deepEqual(resetStream.providedAssetNames(), [], 'reset releases retained assets');
  assert.equal(resetStream.getMeshMerge(), true, 'reset preserves stream configuration');
  assert.throws(() => resetStream.reset(1), /wrong argument count/);
} finally { resetStream.delete(); }
assert.throws(() => stream.begin(null), /expected byte view/);
assert.throws(() => stream.begin(new Uint8Array(), 3), /source URI string/);
assert.equal(stream.getURI(), '');
assert.throws(() => stream.getURI(0), /wrong argument count/);
assert.throws(() => stream.beginOwned(new Uint8Array()), /expected string/);
assert.throws(() => stream.begin.call({}, new Uint8Array()), /Invalid LightUSD receiver/);
assert.throws(() => stream.end(1), /wrong argument count/);
assert.throws(() => stream.clearAssets(1), /wrong argument count/);
assert.throws(() => stream.setMeshMerge(), /wrong argument count/);
assert.throws(() => stream.setMeshMerge(1), /expected boolean/);
for (const [setter, getter] of [
  // These four getters implement the legacy getNative* configuration family
  // through next RenderStream's stored typed-C flags.
  ['setMaterialDedup', 'getMaterialDedup'], ['setMeshMerge', 'getMeshMerge'],
  ['setMeshMergeBakeTransform', 'getMeshMergeBakeTransform'],
  ['setFlattenRenderTree', 'getFlattenRenderTree'], ['setMeshOnly', 'getMeshOnly'],
  ['setComputeTangents', 'getComputeTangents'],
  ['setBuildVertexIndices', 'getBuildVertexIndices'],
  ['setEnableComposition', 'getEnableComposition'],
  ['setEnableValueClips', 'getEnableValueClips']
]) {
  stream[setter](true);
  assert.equal(stream[getter](), true, `${getter} reports its typed-C setting`);
  stream[setter](false);
  assert.equal(stream[getter](), false, `${getter} reports a cleared setting`);
  assert.throws(() => stream[getter](0), /wrong argument count/);
}
assert.equal(stream.getDeferTangentComputation(), true,
  'tangent computation is deferred by default, matching the legacy loader');
stream.setDeferTangentComputation(false);
assert.equal(stream.getDeferTangentComputation(), false);
assert.equal(stream.getComputeTangents(), true,
  'legacy eager tangent setting maps to next computeTangents');
stream.setComputeTangents(false);
assert.equal(stream.getDeferTangentComputation(), true,
  'the compatibility getter reflects direct next tangent changes');
assert.throws(() => stream.setDeferTangentComputation(1), /expected boolean/);
assert.throws(() => stream.getDeferTangentComputation(0), /wrong argument count/);
assert.throws(() => stream.setVariantOverride('lod'), /wrong argument count/);
assert.throws(() => stream.setVariantOverride('lod', 1), /expected strings/);
const previousLightUSDDebug = module.onLightUSDDebug;
try {
  const debugEvents = [];
  module.onLightUSDDebug = event => debugEvents.push(event);
  const heapBytes = module.HEAPU8.byteLength;
  assert.deepEqual(stream.debugLogMemory('probe'), {label: 'probe', heapBytes});
  assert.deepEqual(debugEvents, [{phase: 'manual', detail: 'probe', heapBytes,
    inputBytes: 0, isUsdz: false, materialsCurrent: 0,
    materialsTotal: 0, materialName: ''}]);
  const callbackError = new Error('debug callback failure');
  let callbackCount = 0;
  module.onLightUSDDebug = () => { ++callbackCount; throw callbackError; };
  assert.throws(() => stream.debugLogMemory('throws'), error => error === callbackError);
  assert.equal(callbackCount, 1, 'debug callback failures propagate once');
} finally { module.onLightUSDDebug = previousLightUSDDebug; }
assert.throws(() => stream.debugLogMemory(5), /expected one string label/);
assert.throws(() => stream.debugLogMemory(), /expected one string label/);
assert.throws(() => stream.debugLogMemory.call({}, 'probe'), /Invalid LightUSD receiver/);
assert.throws(() => stream.setRenderSettingsPath(1), /expected string/);
const emptyLoad = stream.begin(new Uint8Array());
assert.equal(emptyLoad.success, false);
assert.equal(stream.getURI(), '', 'an unnamed byte load clears the source URI');
assert.equal(emptyLoad.error, stream.error());
assert.deepEqual(stream.listVariants(), []);
assert.throws(() => stream.listVariants(0), /wrong argument count/);
assert.throws(() => stream.listVariants.call({}), /Invalid LightUSD receiver/);
assert.equal(stream.hasVariants(), false);
assert.deepEqual(stream.extractVariants(), []);
assert.throws(() => stream.hasVariants(0), /wrong argument count/);
assert.throws(() => stream.extractVariants(0), /wrong argument count/);
assert.throws(() => stream.extractVariants.call({}), /Invalid LightUSD receiver/);
assert.equal(stream.hasSublayers(), false);
assert.equal(stream.hasReferences(), false);
assert.equal(stream.hasPayload(), false);
assert.equal(stream.hasInherits(), false);
assert.equal(stream.hasSpecializes(), false);
assert.deepEqual(stream.extractSublayerAssetPaths(), []);
assert.deepEqual(stream.extractReferencesAssetPaths(), []);
assert.deepEqual(stream.extractPayloadAssetPaths(), []);
assert.throws(() => stream.hasReferences(0), /wrong argument count/);
assert.throws(() => stream.hasInherits(0), /wrong argument count/);
assert.throws(() => stream.hasSpecializes(0), /wrong argument count/);
assert.throws(() => stream.extractSublayerAssetPaths(0), /wrong argument count/);
assert.throws(() => stream.extractPayloadAssetPaths.call({}), /Invalid LightUSD receiver/);
const arcInspectionStream = new module.RenderStream();
arcInspectionStream.setEnableComposition(false);
const arcInspectionLoad = arcInspectionStream.begin(encode(`#usda 1.0
(
  subLayers = [@strong.usda@, @weak.usda@]
)
def Xform "Root" (
  references = [@reference.usda@</Asset>]
  payload = @payload.usda@</Asset>
  add inherits = </Base>
  specializes = </Base>
) {}
`));
assert.equal(arcInspectionLoad.success, true, arcInspectionLoad.error || arcInspectionStream.error());
assert.equal(arcInspectionStream.hasSublayers(), true);
assert.equal(arcInspectionStream.hasReferences(), true);
assert.equal(arcInspectionStream.hasPayload(), true);
assert.equal(arcInspectionStream.hasInherits(), true);
assert.equal(arcInspectionStream.hasSpecializes(), true);
assert.deepEqual(arcInspectionStream.extractSublayerAssetPaths(), ['strong.usda', 'weak.usda']);
assert.deepEqual(arcInspectionStream.extractReferencesAssetPaths(), ['reference.usda']);
assert.deepEqual(arcInspectionStream.extractPayloadAssetPaths(), ['payload.usda']);
const internalReferenceLoad = arcInspectionStream.begin(encode(`#usda 1.0
def Xform "Root" ( references = </Other> ) {}
def Xform "Other" {}
`));
assert.equal(internalReferenceLoad.success, true,
  internalReferenceLoad.error || arcInspectionStream.error());
assert.equal(arcInspectionStream.hasReferences(), true,
  'hasReferences reports authored internal arcs too');
assert.deepEqual(arcInspectionStream.extractReferencesAssetPaths(), [],
  'reference asset-path extraction omits internal references');
arcInspectionStream.delete();
const variantsStream = new module.RenderStream();
const variantsLoad = variantsStream.begin(new Uint8Array(fs.readFileSync(
  new URL('../../../tests/usda/variantSet-prim-001.usda', import.meta.url))));
assert.equal(variantsLoad.success, true, variantsLoad.error || variantsStream.error());
assert.equal(variantsStream.hasVariants(), true);
assert.deepEqual(variantsStream.extractVariants(), [{
  primPath: '/bora',
  variantSets: [{name: 'shapeVariant', selection: 'Capsule', options: ['Capsule', 'Cone']}]
}]);
variantsStream.delete();
assert.equal(stream.meshCount(), 0);
assert.equal(stream.renderStats().sourceMeshes, 0);
assert.equal(stream.getStats().renderSceneNodes, 0);
assert.equal(Object.hasOwn(stream.getStats(), 'renderSceneMemoryBytes'), false);
assert.throws(() => stream.animationInfo(0), /invalid animation id/);
assert.deepEqual(stream.getAnimationInfo(0), {});
assert.deepEqual(stream.getAllAnimationInfos(), []);
assert.equal(stream.numMeshes(), 0);
assert.equal(stream.nodeType(0), -1);
assert.equal(stream.nodeParentId(0), -1);
assert.equal(stream.nodeDataId(0), -1);
assert.equal(stream.nodeVisible(0), -1);
assert.equal(stream.nodeChildId(0, 0), -1);
assert.deepEqual(stream.getNode(0), {error: 'invalid node index'});
assert.deepEqual(stream.getPoints(0), {error: 'invalid points index'});
assert.deepEqual(stream.getCurves(0), {error: 'invalid curves index'});
assert.deepEqual(stream.getCamera(0), {error: 'invalid camera index'});
assert.deepEqual(stream.getPointInstanceDraw(0), {error: 'invalid point instance draw index'});
assert.deepEqual(stream.getPointInstancer(0), {error: 'invalid point instancer index'});
assert.deepEqual(stream.getSkeleton(0), {error: 'invalid skeleton index'});
assert.deepEqual(stream.getLight(0), {error: 'invalid light index'});
assert.deepEqual(stream.getSceneMetadata(), {});
assert.deepEqual(stream.getUnsupportedRenderables(), []);
assert.equal(stream.rootNodeId(0), -1);
assert.throws(() => stream.nodePathBuffer(0), /invalid node id/);
assert.equal(stream.meshVertexCount(0), -1);
assert.equal(stream.meshFaceCount(0), -1);
assert.equal(stream.meshMaterialId(0), -1);
assert.equal(stream.meshHasNormals(0), -1);
assert.equal(stream.meshHasUVs(0), -1);
assert.throws(() => stream.meshTangentsBuffer(0), /invalid mesh id or kind/);
assert.throws(() => stream.meshJointIndicesBuffer(0), /invalid mesh id or kind/);
assert.throws(() => stream.resourcePathBuffer(1, 0), /invalid resource/);
assert.throws(() => stream.resourceNameBuffer(1, 0), /invalid resource/);
assert.throws(() => stream.pointsPositionsBuffer(0), /invalid points id/);
assert.throws(() => stream.curvesControlPointsBuffer(0), /invalid curves id/);
assert.throws(() => stream.instancerCompactBuffer(0), /invalid instancer id/);
assert.throws(() => stream.instancerPrototypePath(0, 0), /invalid prototype/);
assert.equal(stream.pointInstanceDrawField(-1, 0), -1);
assert.throws(() => stream.pointInstanceDrawField(0, 6), /expected numeric draw id and field/);
assert.throws(() => stream.pointInstanceDrawTransformBuffer(0), /invalid draw id/);
assert.throws(() => stream.sceneName(1), /wrong argument count/);
assert.throws(() => stream.sceneWorkingColorSpace(), /query failed/);
assert.equal(stream.lightField(0, 0), -1);
assert.equal(stream.cameraField(0, 0), -1);
assert.equal(stream.skeletonField(0, 0), -1);
assert.throws(() => stream.skeletonBindMatricesBuffer(0), /invalid skeleton id/);
assert.throws(() => stream.recordPathBuffer(11, 0), /invalid record/);
assert.throws(() => stream.unsupportedRenderableReason(0), /invalid unsupported id/);
assert.equal(stream.animationChannelCount(0), -1);
assert.equal(stream.animationChannelTargetNode(0, 0), -1);
assert.throws(() => stream.animationKeyframeTimesBuffer(0, 0), /invalid animation channel/);
assert.equal(stream.materialShaderType(0), -1);
assert.equal(stream.materialAlphaMode(0), -1);
assert.equal(stream.materialDoubleSided(0), -1);
assert.equal(stream.textureImageId(0), -1);
assert.equal(stream.textureWidth(0), -1);
assert.equal(stream.textureWrapS(0), -1);
assert.equal(stream.textureWrapT(0), -1);
assert.equal(stream.textureOutputChannel(0), -1);
assert.equal(stream.textureRotationScaled(0), -1);
assert.equal(stream.textureHeight(0), -1);
assert.equal(stream.textureChannels(0), -1);
assert.equal(stream.textureMipLevels(0), -1);
assert.equal(stream.textureLoaded(0), -1);
assert.throws(() => stream.materialBaseColorBuffer(0), /invalid material or parameter/);
assert.equal(stream.materialBaseColorTextureId(0), -1);
assert.throws(() => stream.textureImageBuffer(0), /invalid texture id/);
assert.throws(() => stream.textureSamplingBuffer(0), /invalid texture id/);
assert.equal(stream.sceneMetersPerUnitScaled(), -1);
assert.equal(stream.sceneUpAxis(), -1);
assert.throws(() => stream.nodeType(), /one numeric node id/);
assert.throws(() => stream.nodeType('0'), /one numeric node id/);
assert.throws(() => stream.getNode(), /one numeric node id/);
assert.throws(() => stream.getNode('0'), /one numeric node id/);
assert.throws(() => stream.getPoints('0'), /one numeric points id/);
assert.throws(() => stream.getCurves('0'), /one numeric curves id/);
assert.throws(() => stream.getCamera('0'), /one numeric camera id/);
assert.throws(() => stream.getPointInstanceDraw('0'), /one numeric draw id/);
assert.throws(() => stream.getPointInstancer('0'), /one numeric instancer id/);
assert.throws(() => stream.getSkeleton('0'), /one numeric skeleton id/);
assert.throws(() => stream.getAnimationInfo('0'), /one numeric animation id/);
assert.throws(() => stream.getAllAnimationInfos(0), /wrong argument count/);
assert.throws(() => stream.getStats(0), /wrong argument count/);
assert.throws(() => stream.getLight('0'), /one numeric light id/);
assert.throws(() => stream.getSceneMetadata(0), /wrong argument count/);
assert.throws(() => stream.getUnsupportedRenderables(0), /wrong argument count/);
assert.throws(() => stream.nodeChildId(0), /two numeric indices/);
assert.throws(() => stream.nodeChildCount(), /one integer node id/);
assert.throws(() => stream.meshVertexCount(), /one numeric mesh id/);
assert.throws(() => stream.materialShaderType(), /one numeric material id/);
assert.throws(() => stream.textureWidth(), /one numeric texture id/);
assert.throws(() => stream.getMesh(), /argument count/);
assert.throws(() => stream.getMesh('0'), /expected number/);
assert.throws(() => stream.setTangentMethod(0), /expected string/);
const meshUsd = `#usda 1.0
def Mesh "M" {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
}
`;
const meshLoad = stream.begin(encode(meshUsd), 'fixtures/mesh.usda');
assert.equal(meshLoad.success, true, meshLoad.error || stream.error());
const memoryStats = stream.getMemoryStats();
assert.equal(memoryStats.numMeshes, stream.numMeshes());
assert.equal(memoryStats.numNodes, stream.numNodes());
assert.equal(memoryStats.numLights, stream.numLights());
assert.ok(memoryStats.numBuffers > 0 && memoryStats.bufferMemoryBytes > 0,
  'memory stats account for retained mesh buffers');
assert.equal(memoryStats.bufferMemoryMB, memoryStats.bufferMemoryBytes / (1024 * 1024));
assert.equal(memoryStats.assetCacheCount, 0);
assert.equal(memoryStats.reorderedMeshCacheCount, 0);
assert.throws(() => stream.getMemoryStats(0), /wrong argument count/);
assert.equal(stream.getURI(), 'fixtures/mesh.usda');
assert.equal(meshLoad.meshCount, 1);
assert.equal(stream.nodeChildCount(0), stream.getNode(0).children.length);
assert.equal(stream.nodeChildCount(99999), -1);
// The JS buffer boundary must copy both byte and float payloads and reject a
// native short copy after the size query, on either WASM pointer width.
const imageBufferExport = module._lightusd_next_render_texture_buffer;
const samplingBufferExport = module._lightusd_next_render_texture_sampling_buffer;
const fakeImage = new Uint8Array([1, 2, 3, 4]);
const fakeSampling = new Float32Array([0.25, 0.5, 1]);
const fakeSamplingBytes = new Uint8Array(fakeSampling.buffer);
const fakeBufferExport = bytes => (_handle, _id, ptr, cap) => {
  if (wasm64 && typeof ptr !== 'bigint') throw new TypeError('pointer width');
  if (!wasm64 && typeof ptr === 'bigint') throw new TypeError('pointer width');
  if (!ptr || cap < bytes.length) return bytes.length;
  module.HEAPU8.set(bytes, Number(ptr));
  return bytes.length;
};
try {
  module._lightusd_next_render_texture_buffer = fakeBufferExport(fakeImage);
  module._lightusd_next_render_texture_sampling_buffer =
    fakeBufferExport(fakeSamplingBytes);
  assert.deepEqual(Array.from(stream.textureImageBuffer(0)), Array.from(fakeImage));
  assert.deepEqual(Array.from(stream.textureSamplingBuffer(0)),
    Array.from(fakeSampling));
  module._lightusd_next_render_texture_buffer = (_handle, _id, ptr, cap) => {
    if (!ptr || !cap) return fakeImage.length;
    return fakeImage.length - 1;
  };
  assert.throws(() => stream.textureImageBuffer(0), /copy failed/);
  assert.deepEqual(Array.from(stream.textureSamplingBuffer(0)),
    Array.from(fakeSampling));
} finally {
  module._lightusd_next_render_texture_buffer = imageBufferExport;
  module._lightusd_next_render_texture_sampling_buffer = samplingBufferExport;
}
assert.equal(meshLoad.nodes, meshLoad.nodeCount);
assert.equal(meshLoad.points, meshLoad.pointsCount);
assert.equal(meshLoad.pointInstanceDraws, meshLoad.pointInstanceDrawCount);
assert.equal(stream.getStats().nativeInputBytes, encode(meshUsd).length);
assert.equal(stream.beginOwned(meshUsd).success, true);
const meshBytes = encode(meshUsd);
const beginPtr = module._lightusd_next_alloc(meshBytes.length + 2);
try {
  module.HEAPU8.set(meshBytes, Number(beginPtr) + 1);
  const heapLoad = stream.begin(module.HEAPU8.subarray(
    Number(beginPtr) + 1, Number(beginPtr) + 1 + meshBytes.length));
  assert.equal(heapLoad.success, true, heapLoad.error || stream.error());
  assert.equal(heapLoad.meshCount, 1);
} finally {
  module._lightusd_next_free(beginPtr);
}
assert.equal(typeof stream.sceneName(), 'string');
assert.equal(typeof stream.sceneDefaultPrim(), 'string');
assert.equal(typeof stream.sceneWorkingColorSpace(), 'string');
const sceneMetadata = stream.getSceneMetadata();
assert.equal(sceneMetadata.upAxis, 'Y');
assert.equal(sceneMetadata.metersPerUnit, 1);
assert.equal(sceneMetadata.kilogramsPerUnit, 1);
assert.equal(sceneMetadata.timeCodesPerSecond, 24);
assert.equal(sceneMetadata.workingToDisplayLinear.length, 9);
assert.equal(stream.renderStats().sourceMeshes, 1);
assert.equal(stream.renderStats().optimizedMeshes, 1);
const meshStats = stream.getStats();
assert.equal(meshStats.sourceMeshes, 1);
assert.equal(meshStats.optimizedMeshes, 1);
assert.equal(meshStats.renderSceneMeshes, 1);
assert.equal(meshStats.stageMemoryBytes, stream.renderStats().stageMemoryBytes);
assert.equal(meshStats.renderSceneMemoryBytes,
  stream.renderStats().renderSceneMemoryBytes);
assert.equal(typeof meshStats.nativeMaterialConversionMs, 'number');
assert.equal(typeof meshStats.materialGraphCacheHits, 'number');
assert.equal(typeof meshStats.renderMeshPointsBytes, 'number');
assert.equal(stream.sceneMetersPerUnitScaled(), 1000000);
assert.equal(stream.sceneUpAxis(), 0);
assert.equal(stream.sceneFramesPerSecondScaled(), 24000);
assert.equal(new TextDecoder().decode(stream.nodePathBuffer(0)), '/M');
const typedNode = stream.getNode(0);
assert.equal(typedNode.name, 'M');
assert.equal(typedNode.primPath, '/M');
assert.equal(typedNode.type, 'mesh');
assert.equal(typedNode.localMatrix.length, 16);
assert.equal(typedNode.worldMatrix.length, 16);
assert.deepEqual(typedNode.children, []);
assert.equal(stream.rootNodeId(0), 0);
assert.equal(new TextDecoder().decode(stream.resourcePathBuffer(1, 0)), '/M');
assert.equal(new TextDecoder().decode(stream.resourceNameBuffer(0, 0)), 'M');
assert.equal(new TextDecoder().decode(stream.resourceNameBuffer(1, 0)), 'M');
assert.equal(new TextDecoder().decode(stream.recordPathBuffer(0, 0)), '/M');
assert.equal(new TextDecoder().decode(stream.recordPathBuffer(1, 0)), '/M');
assert.equal(new TextDecoder().decode(stream.recordPathBuffer(11, 0)), '/M');
assert.equal(new TextDecoder().decode(stream.resourceNameBuffer(11, 0)), 'M');
assert.throws(() => stream.recordPathBuffer(11, 1), /invalid record/);
assert.deepEqual(Array.from(stream.nodeLocalTransformBuffer(0)), [
  1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1
]);
assert.equal(stream.nodeWorldTransformBuffer(0).length, 16);
assert.equal(stream.meshVertexCount(0), 3);
assert.equal(stream.meshFaceCount(0), 1);
assert.equal(stream.meshMaterialId(0), -1);
assert.equal(stream.meshHasNormals(0), 1);
assert.equal(stream.meshHasUVs(0), 0);
assert.equal(stream.meshHasTangents(0), 0);
assert.equal(stream.meshHasSecondaryUVs(0), 0);
assert.equal(stream.meshHasColors(0), 0);
assert.equal(stream.meshHasSkin(0), 0);
assert.equal(stream.meshHasBounds(0), 1);
assert.equal(stream.meshSkeletonId(0), -1);
assert.equal(stream.getMeshSubsetOutput(0), null);
{
  const subsetQuery = module._lightusd_next_render_mesh_subset_output;
  const allocate = module._lightusd_next_alloc;
  let allocations = 0;
  module._lightusd_next_render_mesh_subset_output = () => 0x10000001;
  module._lightusd_next_alloc = size => { ++allocations; return allocate(size); };
  try {
    assert.throws(() => stream.getMeshSubsetOutput(0), /512 MiB aggregate limit/);
    assert.equal(allocations, 0,
      'subset payload size is bounded before the WASM allocation');
  } finally {
    module._lightusd_next_render_mesh_subset_output = subsetQuery;
    module._lightusd_next_alloc = allocate;
  }
}
assert.throws(() => stream.getMeshSubsetOutput(99), /invalid mesh id/);
assert.equal(stream.getMeshBlendShapes(0), undefined);
assert.throws(() => stream.getMeshBlendShapes(99), /invalid mesh id/);
{
  const countQuery = module._lightusd_next_render_mesh_blend_shape_count;
  const allocate = module._lightusd_next_alloc;
  let allocations = 0;
  module._lightusd_next_render_mesh_blend_shape_count = () => 65537;
  module._lightusd_next_alloc = size => { ++allocations; return allocate(size); };
  try {
    assert.throws(() => stream.getMeshBlendShapes(0), /excessive shape count/);
    assert.equal(allocations, 0,
      'blend shape count is bounded before allocating payloads');
  } finally {
    module._lightusd_next_render_mesh_blend_shape_count = countQuery;
    module._lightusd_next_alloc = allocate;
  }
}
{
  const countQuery = module._lightusd_next_render_mesh_blend_shape_count;
  const nameQuery = module._lightusd_next_render_mesh_blend_shape_name;
  const offsetQuery = module._lightusd_next_render_mesh_blend_shape_offsets;
  const allocate = module._lightusd_next_alloc;
  let allocations = 0;
  module._lightusd_next_render_mesh_blend_shape_count = () => 1;
  module._lightusd_next_render_mesh_blend_shape_name = () => 4;
  module._lightusd_next_render_mesh_blend_shape_offsets = (_handle, _mesh, _shape,
    _between, kind) => kind === 0 ? 0x0b000000 : 0;
  Object.defineProperty(stream, 'meshBlendShapeInfo', {
    configurable: true, value: () => ({weight: 1, inbetweenCount: 0, flags: 0})
  });
  module._lightusd_next_alloc = size => { ++allocations; return allocate(size); };
  try {
    assert.throws(() => stream.getMeshBlendShapes(0), /aggregate exceeds 512 MiB limit/);
    assert.equal(allocations, 0,
      'combined blend-shape offsets are bounded before payload allocation');
  } finally {
    module._lightusd_next_render_mesh_blend_shape_count = countQuery;
    module._lightusd_next_render_mesh_blend_shape_name = nameQuery;
    module._lightusd_next_render_mesh_blend_shape_offsets = offsetQuery;
    module._lightusd_next_alloc = allocate;
    delete stream.meshBlendShapeInfo;
  }
}
assert.equal(stream.meshPrimvarCount(0), 0);
assert.equal(stream.meshPrimvarFormat(0, 0), -1);
assert.throws(() => stream.meshPrimvarName(0, 0), /invalid mesh or primvar id/);
assert.throws(() => stream.meshPrimvarBuffer(0, 0), /invalid mesh or primvar id/);
const primvarStream = new module.RenderStream();
try {
  primvarStream.setMeshOnly(true);
  const primvarLoad = primvarStream.begin(encode(`#usda 1.0
def Mesh "M" {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  float[] primvars:heat = [10, 20, 30, 40, 50, 60] (interpolation = "vertex", elementSize = 2)
  int[] primvars:heat:indices = [2, 0, 1]
}
`));
  assert.equal(primvarLoad.success, true, primvarLoad.error || primvarStream.error());
  assert.equal(primvarStream.meshPrimvarCount(0), 1);
  assert.equal(primvarStream.meshPrimvarFormat(0, 0), 0);
  assert.equal(primvarStream.meshPrimvarElementSize(0, 0), 2);
  assert.equal(primvarStream.meshPrimvarName(0, 0), 'heat');
  const meshPrimvars = JSON.parse(primvarStream.getMeshPrimvarsJSON(0));
  assert.equal(meshPrimvars.version, 1);
  assert.equal(meshPrimvars.primPath, '/M');
  assert.equal(meshPrimvars.primvars.heat.elementSize, 2);
  assert.deepEqual(meshPrimvars.primvars.heat.value.value,
    [[50, 60], [10, 20], [30, 40]]);
  const primvarNameExport = module._lightusd_next_render_mesh_primvar_name;
  try {
    module._lightusd_next_render_mesh_primvar_name = (...args) => {
      if (args[3] === 0 || args[3] === 0n) return 0x10000000;
      throw new Error('primvar name preflight must reject before copying the name');
    };
    assert.throws(() => primvarStream.getMeshPrimvarsJSON(0),
      /aggregate primvar names exceed 512 MiB/);
  } finally {
    module._lightusd_next_render_mesh_primvar_name = primvarNameExport;
  }
  const primvarBufferExport = module._lightusd_next_render_mesh_primvar_buffer;
  try {
    module._lightusd_next_render_mesh_primvar_buffer = (...args) => {
      if (args[4] === 0 || args[4] === 0n) return 0x20000004;
      throw new Error('primvar JSON must reject oversized aggregate before payload copy');
    };
    assert.throws(() => primvarStream.getMeshPrimvarsJSON(0),
      /aggregate primvar data exceeds 512 MiB/);
  } finally {
    module._lightusd_next_render_mesh_primvar_buffer = primvarBufferExport;
  }
  assert.deepEqual(JSON.parse(primvarStream.getMeshPrimvarsJSON(99999)),
    {version: 1, primvars: {}, error: 'invalid mesh id'});
  assert.throws(() => primvarStream.getMeshPrimvarsJSON(), /one integer mesh id/);
} finally {
  primvarStream.end();
}
assert.deepEqual(Array.from(stream.meshPointsBuffer(0)), [0, 0, 0, 1, 0, 0, 0, 1, 0]);
assert.deepEqual(Array.from(stream.meshIndicesBuffer(0)), [0, 1, 2]);
assert.equal(stream.meshColorsBuffer(0).length, 0);
assert.equal(stream.meshTangentsBuffer(0).length, 0);
assert.equal(stream.meshUVBuffer(0).length, 0);
const ownedMeshCopy = stream.getMeshCopy(0);
assert.ok(ownedMeshCopy.points instanceof Float32Array);
assert.ok(ownedMeshCopy.indices instanceof Uint32Array);
assert.deepEqual(Array.from(ownedMeshCopy.points), [0, 0, 0, 1, 0, 0, 0, 1, 0]);
assert.deepEqual(Array.from(ownedMeshCopy.faceVertexIndices), [0, 1, 2]);
assert.notEqual(ownedMeshCopy.points.buffer, module.HEAPU8.buffer,
  'getMeshCopy owns its typed arrays independently of WASM memory');
const borrowedMeshView = stream.getMeshGeometryView(0);
assert.equal(typeof borrowedMeshView.points.ptr, 'number');
assert.deepEqual(Array.from(new Float32Array(module.HEAPU8.buffer,
  borrowedMeshView.points.ptr, borrowedMeshView.points.length)),
Array.from(ownedMeshCopy.points));
assert.deepEqual(stream.getMeshCopy(99999), {error: 'invalid mesh index'});
assert.throws(() => stream.getMeshCopy(), /one integer mesh id/);
const meshBufferExportForCopy = module._lightusd_next_render_mesh_buffer;
try {
  module._lightusd_next_render_mesh_buffer = (handle, meshId, kind, out, cap) => {
    if (kind === 0 && (out === 0 || out === 0n)) return 512 * 1024 * 1024 + 4;
    throw new Error('getMeshCopy must preflight the aggregate before copying');
  };
  assert.throws(() => stream.getMeshCopy(0), /aggregate payload exceeds 512 MiB/);
} finally {
  module._lightusd_next_render_mesh_buffer = meshBufferExportForCopy;
}
const remainingMemoryExportForMeshCopy = module._lightusd_next_render_remaining_memory_bytes;
let meshCopyPayloadCopies = 0;
try {
  module._lightusd_next_render_remaining_memory_bytes = () => 0;
  module._lightusd_next_render_mesh_buffer = (...args) => {
    if (args[3] !== 0 && args[3] !== 0n && args[4] > 0) ++meshCopyPayloadCopies;
    return meshBufferExportForCopy(...args);
  };
  assert.throws(() => stream.getMeshCopy(0), /aggregate payload exceeds remaining memory limit/);
  assert.equal(meshCopyPayloadCopies, 0,
    'getMeshCopy must reject the remaining-memory budget before copying any stream');
} finally {
  module._lightusd_next_render_mesh_buffer = meshBufferExportForCopy;
  module._lightusd_next_render_remaining_memory_bytes = remainingMemoryExportForMeshCopy;
}
stream.end();
const curvesLoad = stream.begin(new Uint8Array(fs.readFileSync(
  new URL('../../../tests/usda/curves.usda', import.meta.url))));
assert.equal(curvesLoad.success, true, curvesLoad.error || stream.error());
assert.equal(stream.curvesCount(), 1);
assert.equal(typeof stream.curvesType(0), 'number');
assert.equal(typeof stream.curvesBasis(0), 'number');
assert.equal(typeof stream.curvesWrap(0), 'number');
assert.equal(typeof stream.curvesIsNurbs(0), 'number');
assert.equal(typeof stream.curvesIsHermite(0), 'number');
assert.equal(typeof stream.curvesWidthsInterpolation(0), 'number');
assert.equal(stream.curvesVertexCountsBuffer(0) instanceof Uint32Array, true);
assert.equal(stream.curvesTessellatedVertexCountsBuffer(0) instanceof Uint32Array, true);
stream.end();
const instancerLoad = stream.begin(new Uint8Array(fs.readFileSync(
  new URL('../../../tests/usda/pointinstancer-full-001.usda', import.meta.url))));
assert.equal(instancerLoad.success, true, instancerLoad.error || stream.error());
assert.equal(stream.pointInstancerCount(), 3);
const instancer = stream.getPointInstancer(0);
assert.equal(instancer.index, 0);
assert.equal(instancer.name, new TextDecoder().decode(stream.resourceNameBuffer(10, 0)));
assert.equal(instancer.primPath, new TextDecoder().decode(stream.resourcePathBuffer(10, 0)));
assert.equal(instancer.primPath, new TextDecoder().decode(stream.recordPathBuffer(10, 0)));
assert.equal(instancer.protoCount, instancer.prototypePaths.length);
assert.deepEqual(instancer.prototypeNodeIds, Array.from(stream.instancerPrototypeNodeIdsBuffer(0)));
assert.deepEqual(instancer.prototypeMeshOffsets, Array.from(stream.instancerPrototypeMeshOffsetsBuffer(0)));
assert.deepEqual(instancer.prototypeMeshIds, Array.from(stream.instancerPrototypeMeshIdsBuffer(0)));
assert.equal(typeof instancer.valid, 'boolean');
assert.equal(typeof instancer.visibleInstanceCount, 'number');
assert.ok(stream.pointInstanceDrawCount() > 0);
const draw = stream.getPointInstanceDraw(0);
assert.equal(instancer.primPath, new TextDecoder().decode(stream.recordPathBuffer(14, 0)));
assert.equal(draw.index, 0);
assert.equal(draw.pointInstancerId, stream.pointInstanceDrawField(0, 0));
assert.equal(draw.instanceIndex, stream.pointInstanceDrawField(0, 1));
assert.equal(draw.prototypeIndex, stream.pointInstanceDrawField(0, 2));
assert.equal(draw.meshId, stream.pointInstanceDrawField(0, 3));
assert.equal(draw.materialId, stream.pointInstanceDrawField(0, 4));
assert.equal(draw.expandedMeshId, stream.pointInstanceDrawField(0, 5));
assert.deepEqual(draw.transform, Array.from(stream.pointInstanceDrawTransformBuffer(0)));
if (draw.meshId >= 0) {
  assert.equal(draw.meshPath, new TextDecoder().decode(stream.resourcePathBuffer(1, draw.meshId)));
}
if (draw.materialId >= 0) {
  assert.equal(draw.materialPath, new TextDecoder().decode(stream.resourcePathBuffer(2, draw.materialId)));
}
assert.equal(typeof stream.instancerPrototypePath(0, 0), 'string');
assert.equal(stream.instancerPrototypeNodeIdsBuffer(0) instanceof Int32Array, true);
assert.equal(stream.instancerPrototypeMeshOffsetsBuffer(0) instanceof Uint32Array, true);
assert.equal(stream.instancerPrototypeMeshIdsBuffer(0) instanceof Int32Array, true);
assert.equal(stream.instancerPrototypeTransformsBuffer(0) instanceof Float32Array, true);
stream.end();
const animationFixture = fs.readFileSync(new URL(
  '../../../tests/usda/skelanimation-full-001.usda', import.meta.url), 'utf8')
  .replace('def Skeleton "Skeleton"',
    'def Skeleton "Skeleton" ( displayName = "Rig Display" )')
  .replace('((1, 0, 0, 0), (0, 1, 0, 0)',
    '((1.00000001, 0, 0, 0), (0, 1, 0, 0)');
const animationLoad = stream.begin(encode(animationFixture));
assert.equal(animationLoad.success, true, animationLoad.error || stream.error());
assert.equal(stream.numAnimations(), 3);
const animationInfos = stream.getAllAnimationInfos();
assert.equal(animationInfos.length, 3);
assertAggregateHeadroomRejectsBeforeBuild(stream, 'getAllAnimationInfos',
  'getAnimationInfo', stream.numAnimations());
assert.deepEqual(stream.getAnimationInfo(0), animationInfos[0]);
assert.equal(animationInfos[0].numTracks, stream.animationChannelCount(0));
assert.equal(animationInfos[0].numSamplers, animationInfos[0].numTracks);
assert.equal(animationInfos[0].duration, animationInfos[0].endTime,
  'duration is the last key time, as in legacy');
assert.equal(animationInfos[0].sourceType, 'SkelAnimation');
assert.equal(animationInfos[0].numClipAssetPaths,
  animationInfos[0].clipAssetPaths.length);
const animationInfoNameQuery = module._lightusd_next_render_resource_name;
try {
  module._lightusd_next_render_resource_name = (handle, kind, id, ptr, cap) =>
    kind === 8 && id === 0 && (ptr === 0 || ptr === 0n) && cap === 0
      ? 0x10000000
      : animationInfoNameQuery(handle, kind, id, ptr, cap);
  assert.throws(() => stream.getAnimationInfo(0), /512 MiB/);
  assert.throws(() => stream.getAllAnimationInfos(), /512 MiB/);
} finally {
  module._lightusd_next_render_resource_name = animationInfoNameQuery;
}
assert.throws(() => stream.animationClipAssetPath(0, 0), /invalid animation or asset id/);
assert.equal(typeof stream.skeletonJointName(0, 0), 'string');
assert.equal(typeof stream.skeletonJointPath(0, 0), 'string');
const skeleton = stream.getSkeleton(0);
assert.equal(skeleton.id, 0);
assert.equal(skeleton.abs_path, skeleton.primPath);
assert.equal(skeleton.prim_name, skeleton.name);
assert.equal(skeleton.display_name, 'Rig Display');
assert.equal(skeleton.anim_id, skeleton.animationId);
assert.equal(skeleton.root_node.joint_id, skeleton.rootJoint);
assert.equal(skeleton.root_node.children.length, 1);
assert.deepEqual(skeleton.root_node.children[0].children[0].children, []);
const flatSkeleton = stream.getSkeletonJointsFlat(0);
assert.deepEqual(flatSkeleton.joint_names, skeleton.joints.map(joint => joint.name));
assert.deepEqual(flatSkeleton.joint_paths, skeleton.joints.map(joint => joint.path));
assert.deepEqual(flatSkeleton.joint_ids, skeleton.joints.map(joint => joint.index));
assert.deepEqual(flatSkeleton.parent_indices, skeleton.joints.map(joint => joint.parentId));
assert.equal(flatSkeleton.bind_matrices.length, skeleton.jointCount * 16);
assert.equal(flatSkeleton.rest_matrices.length, skeleton.jointCount * 16);
assert.equal(flatSkeleton.num_joints, skeleton.jointCount);
assert.equal(stream.skeletonBindMatricesBuffer(0) instanceof Float64Array, true);
assert.equal(stream.skeletonRestMatricesBuffer(0) instanceof Float64Array, true);
assert.equal(flatSkeleton.bind_matrices[0], 1.00000001,
  'matrix4d bind transforms must retain double precision through RenderScene');
assert.deepEqual(stream.getAllSkeletons(), Array.from(
  {length: stream.skeletonCount()}, (_, index) => stream.getSkeleton(index)));
assertAggregateHeadroomRejectsBeforeBuild(stream, 'getAllSkeletons',
  'getSkeleton', stream.skeletonCount());
assert.throws(() => stream.getAllSkeletons(0), /wrong argument count/);
const skeletonBufferBudgetExport = module._lightusd_next_render_skeleton_joint_buffer;
try {
  module._lightusd_next_render_skeleton_joint_buffer =
    (handle, skeletonId, kind) => kind === 0 ? 0x0d000000 : 0;
  assert.throws(() => stream.getSkeleton(0), /aggregate returned data exceeds 512 MiB/);
  assert.throws(() => stream.getSkeletonJointsFlat(0), /aggregate returned data exceeds 512 MiB/);
  assert.throws(() => stream.getAllSkeletons(), /aggregate returned data exceeds 512 MiB/);
} finally {
  module._lightusd_next_render_skeleton_joint_buffer = skeletonBufferBudgetExport;
}
assert.equal(skeleton.jointCount, stream.skeletonField(0, 0));
assert.equal(skeleton.rootJoint, stream.skeletonField(0, 1));
assert.equal(skeleton.animationId, stream.skeletonField(0, 2));
assert.equal(skeleton.animationSourcePath, stream.skeletonAnimationSourcePath(0));
assert.equal(skeleton.joints.length, skeleton.jointCount);
assert.equal(skeleton.jointCount, 3);
assert.deepEqual(skeleton.joints.map(joint => joint.parentId), [-1, 0, 1]);
assert.deepEqual(skeleton.joints.map(joint => joint.children), [[1], [2], []]);
assert.deepEqual(skeleton.joints[0].children,
  Array.from(stream.skeletonJointChildrenBuffer(0, 0)));
assert.deepEqual(skeleton.joints[0].bindMatrix,
  Array.from(stream.skeletonBindMatricesBuffer(0).subarray(0, 16)));
assert.deepEqual(skeleton.joints[0].restMatrix,
  Array.from(stream.skeletonRestMatricesBuffer(0).subarray(0, 16)));
assert.equal(stream.animationChannelCount(0), 1);
assert.equal(stream.animationChannelKeyframeCount(0, 0), 5);
assert.equal(stream.animationChannelElementCount(0, 0), 3);
assert.equal(stream.animationChannelValueStride(0, 0), 4);
assert.equal(typeof stream.animationChannelTargetPrimPath(0, 0), 'string');
assert.equal(typeof stream.animationChannelPropertyName(0, 0), 'string');
assert.equal(typeof stream.animationChannelJointOrder(0, 0, 0), 'string');
assert.equal(stream.animationKeyframeTimesBuffer(0, 0).length, 5);
assert.equal(stream.animationKeyframeValuesBuffer(0, 0).length, 20);
assertAggregateHeadroomRejectsBeforeBuild(stream, 'getAllAnimations',
  'getAnimation', stream.numAnimations());
const animationBufferExport = module._lightusd_next_render_animation_channel_buffer;
try {
  module._lightusd_next_render_animation_channel_buffer = () => 5;
  assert.throws(() => stream.animationKeyframeTimesBuffer(0, 0),
    /invalid buffer size/);
} finally {
  module._lightusd_next_render_animation_channel_buffer = animationBufferExport;
}
assert.equal(stream.animationKeyframeTimesBuffer(0, 0).length, 5);
const animationBufferBudgetExport = module._lightusd_next_render_animation_channel_buffer;
let animationBudgetQueries = 0;
try {
  module._lightusd_next_render_animation_channel_buffer =
    (handle, animationId, channelId, kind) => {
      ++animationBudgetQueries;
      return kind === 1 ? 0x08100000 : 0;
    };
  assert.throws(() => stream.getAnimation(0), /aggregate returned data exceeds 512 MiB/);
  assert.throws(() => stream.getAllAnimations(), /aggregate returned data exceeds 512 MiB/);
  assert.ok(animationBudgetQueries > 0,
    'animation aggregates are preflighted from C buffer sizes before copying');
} finally {
  module._lightusd_next_render_animation_channel_buffer = animationBufferBudgetExport;
}
const animationNameBudgetExport = module._lightusd_next_render_resource_name;
try {
  module._lightusd_next_render_resource_name = () => 0x10000000;
  assert.throws(() => stream.getAnimation(0), /aggregate returned data exceeds 512 MiB/);
} finally {
  module._lightusd_next_render_resource_name = animationNameBudgetExport;
}
assert.equal(stream.animationKeyframeTimesBuffer(0, 0).length, 5);
stream.end();
const skinnedLoad = stream.begin(new Uint8Array(fs.readFileSync(
  new URL('../../../tests/usda/usdskel-001.usda', import.meta.url))));
assert.equal(skinnedLoad.success, true, skinnedLoad.error || stream.error());
assert.equal(stream.numMeshes(), 1);
assert.equal(stream.meshHasSkin(0), 1);
const boneTexture = stream.generateBoneTexture(0);
assert.equal(boneTexture.textureWidth, 8);
assert.equal(boneTexture.textureHeight, 3);
assert.equal(boneTexture.texelsPerVertex, 2);
assert.equal(boneTexture.maxInfluences, 4);
assert.equal(boneTexture.vertexCount, 12);
assert.equal(boneTexture.originalElementSize, 1);
assert.deepEqual(Array.from(boneTexture.textureData.subarray(0, 16)),
  [2, 1, -1, 0, -1, 0, -1, 0, 2, 1, -1, 0, -1, 0, -1, 0]);
assert.deepEqual(Array.from(boneTexture.vertexOffsets),
  Array.from({length: 12}, (_, index) => index * 2));
const boneTexture8 = stream.generateBoneTexture(0, 8);
assert.equal(boneTexture8.maxInfluences, 8);
assert.equal(boneTexture8.texelsPerVertex, 4);
assert.equal(boneTexture8.textureWidth, 8);
assert.equal(boneTexture8.textureHeight, 6);
assert.deepEqual(Array.from(boneTexture8.textureData.subarray(0, 16)),
  [2, 1, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0]);
assert.deepEqual(Array.from(boneTexture8.vertexOffsets),
  Array.from({length: 12}, (_, index) => index * 4));
Object.defineProperties(stream, {
  meshHasSkin: {value: () => 1, configurable: true},
  getMeshGeometryView: {value: () => ({elementSize: 32}), configurable: true},
  meshJointIndicesBuffer: {value: () => ({length: 33554432}), configurable: true},
  meshJointWeightsBuffer: {value: () => ({length: 33554432}), configurable: true}
});
try {
  assert.match(stream.generateBoneTexture(0, 32).error, /exceeds 256 MiB/);
} finally {
  delete stream.meshHasSkin;
  delete stream.getMeshGeometryView;
  delete stream.meshJointIndicesBuffer;
  delete stream.meshJointWeightsBuffer;
}
assert.match(stream.generateBoneTexture(99).error, /Invalid mesh ID/);
const skinnedMesh = stream.getMesh(0);
const aggregateJoints = new Uint16Array(module.HEAPU8.buffer,
  skinnedMesh.jointIndices.ptr, skinnedMesh.jointIndices.length).slice();
const aggregateWeights = new Float32Array(module.HEAPU8.buffer,
  skinnedMesh.jointWeights.ptr, skinnedMesh.jointWeights.length).slice();
const skinnedView = stream.getMeshGeometryView(0);
assert.equal(skinnedView.primName, skinnedMesh.primName);
assert.equal(skinnedView.primPath, skinnedMesh.primPath);
assert.equal(skinnedView.materialId, skinnedMesh.materialId);
assert.equal(skinnedView.skel_id, skinnedMesh.skel_id);
assert.equal(skinnedView.skeletonPath, skinnedMesh.skeletonPath);
assert.deepEqual(skinnedView.geomBindTransform, skinnedMesh.geomBindTransform);
assert.deepEqual(skinnedView.localMatrix, skinnedMesh.localMatrix);
assert.deepEqual(skinnedView.worldMatrix, skinnedMesh.worldMatrix);
assert.deepEqual(new Uint16Array(module.HEAPU8.buffer,
  skinnedView.jointIndices.ptr, skinnedView.jointIndices.length), aggregateJoints);
assert.deepEqual(new Float32Array(module.HEAPU8.buffer,
  skinnedView.jointWeights.ptr, skinnedView.jointWeights.length), aggregateWeights);
assert.deepEqual(stream.meshJointIndicesBuffer(0), aggregateJoints);
assert.deepEqual(stream.meshJointWeightsBuffer(0), aggregateWeights);
assert.equal(aggregateJoints.length,
  stream.meshPointsBuffer(0).length / 3 * skinnedMesh.elementSize);
stream.end();
stream.setComputeTangents(true);
const savedTangentMerge = stream.getMeshMerge();
stream.setMeshMerge(false);
// Like legacy, tangents are produced only for normal-mapped meshes.
const tangentUsd = `#usda 1.0
def Material "Bumpy" {
  token outputs:surface.connect = </Bumpy/S.outputs:surface>
  def Shader "S" {
    uniform token info:id = "UsdPreviewSurface"
    normal3f inputs:normal.connect = </Bumpy/N.outputs:rgb>
    token outputs:surface
  }
  def Shader "N" {
    uniform token info:id = "UsdUVTexture"
    asset inputs:file = @normal.png@
    float3 outputs:rgb
  }
}
def Mesh "TangentTri" (prepend apiSchemas = ["MaterialBindingAPI"]) {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
  normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)] (
    interpolation = "vertex"
  )
  texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] (
    interpolation = "vertex"
  )
  rel material:binding = </Bumpy>
}
def Mesh "PlainTri" {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  point3f[] points = [(2, 0, 0), (3, 0, 0), (2, 1, 0)]
  normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)] (
    interpolation = "vertex"
  )
  texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] (
    interpolation = "vertex"
  )
}`;
const tangentLoad = stream.begin(encode(tangentUsd));
assert.equal(tangentLoad.success, true, tangentLoad.error || stream.error());
assert.equal(stream.computeMeshTangents(0), true,
  'compatibility operation succeeds for a valid mesh id');
assert.equal(stream.computeMeshTangents(1), true);
assert.equal(stream.computeMeshTangents(2), false, 'legacy reports an invalid mesh id as false');
assert.equal(stream.meshHasTangents(1), 0, 'a mesh without a normal map has no tangents');
assert.equal(stream.getMeshGeometryView(1).tangents, undefined);
assert.throws(() => stream.computeMeshTangents(), /expected one integer mesh id/);
const tangentMesh = stream.getMesh(0);
assert.ok(tangentMesh.tangents, 'aggregate getter should compute tangents');
const aggregateTangents = new Float32Array(module.HEAPU8.buffer,
  tangentMesh.tangents.ptr, tangentMesh.tangents.length).slice();
const tangentView = stream.getMeshGeometryView(0);
assert.equal(tangentView.primPath, tangentMesh.primPath);
assert.equal(tangentView.doubleSided, tangentMesh.doubleSided);
assert.equal(tangentView.tangentMethod, tangentMesh.tangentMethod);
assert.deepEqual(new Float32Array(module.HEAPU8.buffer,
  tangentView.tangents.ptr, tangentView.tangents.length), aggregateTangents);
assert.equal(stream.meshHasTangents(0), 1);
assert.deepEqual(stream.meshTangentsBuffer(0), aggregateTangents);
const meshBufferExport = module._lightusd_next_render_mesh_buffer;
const boundedMeshAlloc = module._lightusd_next_alloc;
let oversizedMeshAllocations = 0;
try {
  module._lightusd_next_render_mesh_buffer = () => 0x20000004;
  module._lightusd_next_alloc = size => {
    ++oversizedMeshAllocations;
    return boundedMeshAlloc(size);
  };
  assert.throws(() => stream.meshPointsBuffer(0), /buffer exceeds 512 MiB limit/);
  assert.equal(oversizedMeshAllocations, 0,
    'oversized typed render buffers are rejected before allocating a copy');
} finally {
  module._lightusd_next_render_mesh_buffer = meshBufferExport;
  module._lightusd_next_alloc = boundedMeshAlloc;
}
stream.end();
stream.setComputeTangents(false);
assert.equal(stream.begin(encode(tangentUsd)).success, true, stream.error());
assert.equal(stream.getMeshGeometryView(0).tangents, undefined,
  'deferred tangents are absent until requested');
assert.equal(stream.meshHasTangents(0), 0);
assert.equal(stream.computeMeshTangents(0), true);
assert.deepEqual(stream.meshTangentsBuffer(0), aggregateTangents,
  'a deferred request produces the same tangents as eager conversion');
assert.equal(stream.computeMeshTangents(1), true);
assert.equal(stream.meshHasTangents(1), 0);
stream.end();
stream.setMeshMerge(savedTangentMerge);
const lightCameraUsd = `#usda 1.0
def SphereLight "L" {
  float inputs:intensity = 2
  float inputs:exposure = 1
}
def Camera "C" {
  float focalLength = 50
  float2 clippingRange = (0.1, 1000)
}
`;
const lightCameraLoad = stream.begin(encode(lightCameraUsd));
assert.equal(lightCameraLoad.success, true, lightCameraLoad.error || stream.error());
const firstRootTree = stream.getRootNode(0);
assert.equal(firstRootTree.index, stream.rootNodeId(0));
assert.deepEqual(stream.getDefaultRootNode(), firstRootTree);
assert.ok(Array.isArray(firstRootTree.children));
const rootNameQuery = module._lightusd_next_render_resource_name;
const getNodeMethod = stream.getNode;
let hierarchyMaterializations = 0;
try {
  module._lightusd_next_render_resource_name = (handle, kind, id, ptr, cap) =>
    kind === 0 && id === firstRootTree.index && (ptr === 0 || ptr === 0n) && cap === 0
      ? 0x10000000
      : rootNameQuery(handle, kind, id, ptr, cap);
  Object.defineProperty(stream, 'getNode', {configurable: true, value: function(id) {
    ++hierarchyMaterializations;
    return getNodeMethod.call(this, id);
  }});
  assert.throws(() => stream.getRootNode(0), /hierarchy exceeds 512 MiB/);
  assert.equal(hierarchyMaterializations, 0,
    'hierarchy payloads must be preflighted before node objects are built');
  assert.throws(() => stream.getNode(firstRootTree.index), /node payload exceeds 512 MiB/);
} finally {
  delete stream.getNode;
  module._lightusd_next_render_resource_name = rootNameQuery;
}
assert.throws(() => stream.getDefaultRootNode(0), /wrong argument count/);
assert.throws(() => stream.getRootNode(0.5), /integer root index/);
assert.deepEqual(stream.getRootNode(99999), {});
assert.equal(stream.lightField(0, 1), 2000);
const light = stream.getLight(0);
assert.equal(light.index, 0);
assert.equal(light.type, 'sphere');
assert.equal(light.typeCode, 6);
assert.equal(light.primPath, '/L');
assert.equal(light.intensity, 2);
assert.equal(light.exposure, 1);
assert.equal(light.transform.length, 16);
assert.equal(light.color.length, 3);
assert.equal(typeof light.shapingIesFile, 'string');
assert.ok(Array.isArray(light.lightLinkTargets));
assert.ok(Array.isArray(light.lightLinkMeshIndices));
assert.equal(typeof light.radius, 'number');
const lightStringQuery = module['_lightusd_next_render_light_string'];
module['_lightusd_next_render_light_string'] = function(handle, id, kind, item, out, cap) {
  if (id === 0 && kind === 0 && out === 0 && cap === 0) return 0x10000000;
  return lightStringQuery(handle, id, kind, item, out, cap);
};
try {
  assert.throws(() => stream.getLight(0), /512 MiB/);
  assert.throws(() => stream.getAllLights(), /512 MiB/);
} finally {
  module['_lightusd_next_render_light_string'] = lightStringQuery;
}
const lightResourceNameQuery = module._lightusd_next_render_resource_name;
try {
  module._lightusd_next_render_resource_name = (handle, kind, id, ptr, cap) =>
    kind === 5 && id === 0 && (ptr === 0 || ptr === 0n) && cap === 0
      ? 0x10000000
      : lightResourceNameQuery(handle, kind, id, ptr, cap);
  assert.throws(() => stream.getLight(0), /returned payload exceeds 512 MiB/);
  assert.throws(() => stream.getAllLights(), /returned data exceeds 512 MiB/);
} finally {
  module._lightusd_next_render_resource_name = lightResourceNameQuery;
}
const lightInfoQuery = module._lightusd_next_render_light_info_get;
const lightMeshIdsQuery = module._lightusd_next_render_light_mesh_ids;
const lightResourcePathQuery = module._lightusd_next_render_resource_path;
try {
  Object.defineProperty(stream, 'lightCount', {value: () => 2, configurable: true});
  module._lightusd_next_render_light_info_get = (handle, id, ptr) =>
    lightInfoQuery(handle, 0, ptr);
  module._lightusd_next_render_light_mesh_ids = (handle, id, kind, out, cap) =>
    lightMeshIdsQuery(handle, 0, kind, out, cap);
  module._lightusd_next_render_resource_name = (handle, kind, id, out, cap) =>
    kind === 5 && id < 2
      ? lightResourceNameQuery(handle, kind, 0, out, cap)
      : lightResourceNameQuery(handle, kind, id, out, cap);
  module._lightusd_next_render_resource_path = (handle, kind, id, out, cap) =>
    kind === 5 && id < 2
      ? lightResourcePathQuery(handle, kind, 0, out, cap)
      : lightResourcePathQuery(handle, kind, id, out, cap);
  module._lightusd_next_render_light_string = (handle, id, kind, item, out, cap) =>
    id < 2 && kind === 0 && (out === 0 || out === 0n) && cap === 0
      ? 0x08000000
      : lightStringQuery(handle, id, kind, item, out, cap);
  assert.throws(() => stream.getAllLights(), /aggregate returned data exceeds 512 MiB/);
} finally {
  delete stream.lightCount;
  module._lightusd_next_render_light_info_get = lightInfoQuery;
  module._lightusd_next_render_light_mesh_ids = lightMeshIdsQuery;
  module._lightusd_next_render_resource_name = lightResourceNameQuery;
  module._lightusd_next_render_resource_path = lightResourcePathQuery;
  module._lightusd_next_render_light_string = lightStringQuery;
}
assert.equal(stream.lightField(0, 2), 1000);
assert.equal(stream.lightTransformBuffer(0).length, 16);
assert.equal(stream.lightColorBuffer(0).length, 3);
assert.equal(stream.cameraField(0, 1), 50000);
assert.equal(stream.cameraField(0, 3), 100);
assert.equal(stream.cameraField(0, 4), 1000000);
assert.equal(stream.cameraTransformBuffer(0).length, 16);
assert.equal(stream.cameraOpticsBuffer(0).length, 8);
stream.end();
const fullLightsLoad = stream.begin(new Uint8Array(fs.readFileSync(
  new URL('../../../tests/usda/lights-full-001.usda', import.meta.url))));
assert.equal(fullLightsLoad.success, true, fullLightsLoad.error || stream.error());
for (const countName of ['numImages', 'numMaterials', 'numTextures', 'numRootNodes']) {
  assert.ok(Number.isInteger(stream[countName]()) && stream[countName]() >= 0);
  assert.throws(() => stream[countName](0), /wrong argument count/);
}
assert.equal(stream.getDefaultRootNodeId(), stream.rootNodeId(0));
const fullLights = Array.from({length: stream.lightCount()}, (_, i) => stream.getLight(i));
assert.deepEqual(stream.getAllLights(), fullLights);
assertAggregateHeadroomRejectsBeforeBuild(stream, 'getAllLights',
  'getLight', stream.lightCount());
assert.throws(() => stream.getAllLights(0), /wrong argument count/);
const rectLight = fullLights.find(item => item.type === 'rect');
const cylinderLight = fullLights.find(item => item.type === 'cylinder');
const domeLight = fullLights.find(item => item.type === 'dome');
assert.ok(rectLight && rectLight.width > 0 && rectLight.height > 0);
assert.ok(cylinderLight && cylinderLight.radius > 0 && cylinderLight.length > 0);
assert.ok(domeLight && typeof domeLight.domeTextureFormat === 'string');
assert.equal(domeLight.envmapTextureId, -1);
stream.end();
const linkedLightsLoad = stream.begin(new Uint8Array(fs.readFileSync(
  new URL('../../../tests/usda/lusdview-raster-multilight-links.usda', import.meta.url))));
assert.equal(linkedLightsLoad.success, true, linkedLightsLoad.error || stream.error());
const linkedLights = Array.from({length: stream.lightCount()}, (_, i) => stream.getLight(i));
const redLight = linkedLights.find(item => item.name === 'Red');
assert.ok(redLight);
assert.equal(redLight.lightLinksAll, false);
assert.deepEqual(redLight.lightLinkTargets, ['/World/Left', '/World/Center']);
assert.equal(redLight.lightLinkMeshIndices.length, 2);
stream.end();
const materialUsd = `#usda 1.0
def Mesh "M" {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  rel material:binding = </Mat>
}
def Material "Mat" {
  token outputs:surface.connect = </Mat/Shader.outputs:surface>
  def Shader "Shader" {
    uniform token info:id = "UsdPreviewSurface"
    float inputs:roughness = 0.25
    token outputs:surface
  }
}
`;
const materialLoad = stream.begin(encode(materialUsd));
assert.equal(materialLoad.success, true, materialLoad.error || stream.error());
assert.equal(stream.materialShaderType(0), 1);
assert.equal(stream.materialAlphaMode(0), 0);
assert.equal(stream.materialDoubleSided(0), 0);
assert.equal(stream.materialOpacityScaled(0), 1000000);
assert.equal(stream.materialRoughnessScaled(0), 250000);
assert.equal(stream.materialClearcoatScaled(0), 0);
assert.equal(stream.materialClearcoatRoughnessScaled(0), 9999);
assert.deepEqual(Array.from(stream.materialRoughnessBuffer(0)), [0.25, 0, 0, 0]);
assert.equal(stream.materialRoughnessTextureId(0), -1);
assert.equal(stream.materialBaseColorBuffer(0).length, 4);
const materialBufferExport = module._lightusd_next_render_material_param_buffer;
try {
  module._lightusd_next_render_material_param_buffer =
    (_handle, _materialId, _param, ptr, cap) => {
      if (!ptr || !cap) return 4;
      return 0;
    };
  assert.throws(() => stream.materialRoughnessBuffer(0), /copy failed/);
} finally {
  module._lightusd_next_render_material_param_buffer = materialBufferExport;
}
assert.deepEqual(Array.from(stream.materialRoughnessBuffer(0)), [0.25, 0, 0, 0]);
stream.end();
const metadataUsd = `#usda 1.0
(
  upAxis = "Z"
  metersPerUnit = 0.125
  kilogramsPerUnit = 2.5
  framesPerSecond = 30
  timeCodesPerSecond = 48
  startTimeCode = 3
  endTimeCode = 93
)
def Xform "World" {}
`;
const metadataLoad = stream.begin(encode(metadataUsd));
assert.equal(metadataLoad.success, true, metadataLoad.error || stream.error());
const authoredMetadata = stream.getSceneMetadata();
assert.equal(authoredMetadata.upAxis, 'Z');
assert.equal(authoredMetadata.metersPerUnit, 0.125);
assert.equal(authoredMetadata.kilogramsPerUnit, 2.5);
assert.equal(authoredMetadata.framesPerSecond, 30);
assert.equal(authoredMetadata.timeCodesPerSecond, 48);
assert.equal(authoredMetadata.startTimeCode, 3);
assert.equal(authoredMetadata.endTimeCode, 93);
stream.end();
const unsupportedUsd = `#usda 1.0
def Volume "Unknown" {}
`;
const unsupportedLoad = stream.begin(encode(unsupportedUsd));
assert.equal(unsupportedLoad.success, true, unsupportedLoad.error || stream.error());
assert.equal(stream.unsupportedRenderableCount(), 1);
assert.equal(stream.unsupportedRenderablePath(0), '/Unknown');
assert.equal(new TextDecoder().decode(stream.recordPathBuffer(9, 0)), '/Unknown');
assert.equal(stream.unsupportedRenderableTypeName(0), 'Volume');
assert.ok(stream.unsupportedRenderableReason(0).length > 0);
assert.deepEqual(stream.getUnsupportedRenderables(), [{
  index: 0,
  primPath: '/Unknown',
  type: 'Volume',
  reason: stream.unsupportedRenderableReason(0)
}]);
stream.end();
const converter = new module.NextUSDZConverterNative();
assert.equal(converter.extractPhysicsSceneJSON(), '');
assert.equal(converter.exportAsUSDA(), '');
assert.equal(converter.exportAsUSDC(), null);
assert.equal(converter.exportAsUSDZ(), null);
const sampleConverter = new module.NextUSDZConverterNative();
assert.equal(sampleConverter.createSampleScene(), true, sampleConverter.error());
assert.throws(() => sampleConverter.createSampleScene(true), /wrong argument count/);
const sampleScene = sampleConverter.exportAsUSDA();
assert.match(sampleScene, /defaultPrim = "root"/);
assert.match(sampleScene, /def Mesh "quad"/);
assert.match(sampleScene, /primvars:st/);
assert.match(sampleScene, /UsdPreviewSurface/);
assert.match(sampleScene, /textures\/checkerboard\.png/);
assert.equal(sampleConverter.setAsset('textures/checkerboard.png',
  new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10])), true);
const samplePackage = sampleConverter.exportAsUSDZ();
assert.ok(samplePackage instanceof Uint8Array && samplePackage.length > 8);
assert.deepEqual(Array.from(samplePackage.subarray(0, 4)), [0x50, 0x4b, 0x03, 0x04]);
assert.ok(new TextDecoder('latin1').decode(samplePackage).includes('checkerboard.png'),
  'sample USDZ contains the caller-supplied checkerboard asset');
const remap = {'textures/checkerboard.png': 'images/remapped.png'};
const remappedPackage = sampleConverter.exportAsUSDZWithRemap(remap);
const remappedPackageText = new TextDecoder('latin1').decode(remappedPackage);
assert.ok(remappedPackageText.includes('images/remapped.png'),
  'USDZ remap updates both the authored asset path and packaged asset name');
assert.ok(!remappedPackageText.includes('textures/checkerboard.png'),
  'USDZ remap removes the old asset identifier from the archive');
assert.throws(() => sampleConverter.exportAsUSDZWithRemap({invalid: 7}),
  /remap keys and values/);
assert.equal(sampleConverter.setAsset('images/existing.png',
  new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10])), true);
assert.equal(sampleConverter.exportAsUSDZWithRemap(
  {'textures/checkerboard.png': 'images/existing.png'}), null,
  'remaps that collide with a second packaged asset must fail');
assert.match(sampleConverter.error(), /duplicate packaged asset names/);
assert.throws(() => sampleConverter.exportAsUSDZWithRemap({bad: ''}),
  /remap keys and values/);
const remappedUsdaPackage = sampleConverter.exportAsUSDZWithOptions(remap,
  {rootLayerFormat: 'usda'});
assert.ok(new TextDecoder('latin1').decode(remappedUsdaPackage).includes('images/remapped.png'),
  'USDZ options preserve the remap when selecting a USDA root');
assert.ok(new TextDecoder('latin1').decode(sampleConverter.exportAsUSDZ())
  .includes('textures/checkerboard.png'),
  'remapped export uses a cloned stage and does not mutate later exports');
assert.equal(sampleConverter.setMaxRetainedPayloadBytes(
  sampleConverter.retainedPayloadBytes()).success, true);
assert.equal(sampleConverter.exportAsUSDZWithRemap(remap), null,
  'remapped export rejects a cloned-stage working set above the configured budget');
assert.match(sampleConverter.error(), /remap working set exceeds configured/);
assert.equal(sampleConverter.setMaxRetainedPayloadBytes(1024 * 1024 * 1024).success, true);
sampleConverter.delete();
assert.equal(converter.createURDFPhysicsScene('{bad'), false);
assert.ok(converter.error().length > 0);
const triangle = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
assert.equal(converter.setMaxMeshBytes(64).success, true);
assert.equal(converter.setVisualMesh('triangle', triangle, null, null,
  new Int32Array([0, 1, 2])), true, converter.error());
assert.equal(converter.meshBytes(), 48);
assert.equal(converter.setVisualMesh('second', triangle, null, null,
  new Int32Array([0, 1, 2])), false);
assert.match(converter.error(), /aggregate byte limit/);
assert.equal(converter.setVisualMesh('triangle', triangle, triangle, null,
  new Int32Array([0, 1, 2])), false);
assert.equal(converter.meshBytes(), 48);
assert.equal(converter.setVisualMesh('triangle', triangle, null, null,
  new Int32Array([0, 1, 2])), true);
assert.equal(converter.setMaxMeshBytes(47).success, false);
assert.equal(converter.setVisualMesh('bad-shape', new Float32Array(1), null, null,
  new Uint32Array()), false);
converter.clearURDFMeshBuffers();
assert.equal(converter.setCollisionMesh('invalid', triangle, null, null,
  new Uint32Array([0, 1, 0x80000000])), false);
assert.match(converter.error(), /int32 range/);
assert.equal(converter.meshBytes(), 0);
assert.equal(converter.setUSDCExportLimitMB(64, 128), undefined);
converter.setUSDCExportLimitMB(64, 0);
assert.equal(converter.createURDFPhysicsScene(JSON.stringify({
  name: 'TestBot', upAxis: 'Z', links: [{name: 'base'}], joints: []
})), true, converter.error());
assert.match(converter.exportAsUSDA(), /TestBot|World/);
assert.throws(() => stream.meshCount.call(converter), /receiver/);
stream.delete();
assert.equal(stream.isDeleted(), true);
assert.throws(() => stream.meshCount(), /receiver/);
assert.throws(() => stream.nodeType(0), /receiver/);
assert.throws(() => stream.delete(), /already deleted/);

const text = `#usda 1.0
def Cube "Box" {
  double size = 2
  custom int64 signedValue = -9223372036854775808
  custom uint64 unsignedValue = 18446744073709551615
  custom float[] samples = [1.25, 2.5]
  custom double[] nonfinite = [nan, inf, -inf]
  custom token[] labels = ["a", "b"]
  custom string label = "quote: \\"; unicode: 日本語"
  matrix4d xformOp:transform = ((1,0,0,0),(0,1,0,0),(0,0,1,0),(2,3,4,1))
}
`;
const rewriteInput = encode(`x${text}y`).subarray(1, -1);
const rewriteUSDA = converter.rewriteRoot(rewriteInput, 'scene.usda', {
  rootLayerFormat: 'USDA', maxMemory: 0, usdaLazy: true
});
assert.equal(rewriteUSDA.success, true, rewriteUSDA.error);
assert.equal(rewriteUSDA.sourcePath, 'scene.usda');
assert.equal(rewriteUSDA.rootName, 'root.usda');
assert.equal(rewriteUSDA.rootLayerFormat, 'usda');
assert.ok(new TextDecoder().decode(rewriteUSDA.data).startsWith('#usda 1.0'));
assert.equal(rewriteUSDA.size, rewriteUSDA.data.length);
const rewriteUSDC = converter.rewriteRoot(rewriteInput, 'scene.usda', {});
assert.equal(rewriteUSDC.success, true, rewriteUSDC.error);
assert.equal(rewriteUSDC.rootName, 'root.usdc');
assert.equal(new TextDecoder().decode(rewriteUSDC.data.subarray(0, 8)), 'PXR-USDC');
assert.ok(rewriteUSDC.tokenCount > 0);
const rewritePtr = module._lightusd_next_alloc(rewriteInput.length + 2);
try {
  module.HEAPU8.set(rewriteInput, Number(rewritePtr) + 1);
  const heapInput = module.HEAPU8.subarray(Number(rewritePtr) + 1,
    Number(rewritePtr) + 1 + rewriteInput.length);
  assert.equal(converter.rewriteRoot(heapInput, 'scene.usda', {}).success, true);
} finally { module._lightusd_next_free(rewritePtr); }
assert.throws(() => converter.rewriteRoot(rewriteInput, 'scene.usda',
  {maxMemory: -1}), /invalid maxMemory/);
assert.deepEqual(converter.rewriteRoot(encode('bad'), 'bad.usda', {}),
  {success: false, error: converter.error()});
assert.ok(converter.error().length > 0);
assert.equal(converter.loadFromBinary(encode(text), 'scene.usda'), true, converter.error());
assert.equal(converter.maxAssetBytes(), 512 * 1024 * 1024);
assert.deepEqual(converter.setMaxAssetBytes(16), {success: true});
assert.equal(converter.setAsset('extra.txt', encode('payload')), true);
assert.equal(converter.assetBytes(), 7);
const converterAlloc = module._lightusd_next_alloc;
const converterAllocationSizes = [];
module._lightusd_next_alloc = size => {
  converterAllocationSizes.push(Number(size));
  return converterAlloc(size);
};
let overLimitAssetAccepted;
try {
  overLimitAssetAccepted = converter.setAsset('other.txt', new Uint8Array(10));
} finally { module._lightusd_next_alloc = converterAlloc; }
assert.equal(overLimitAssetAccepted, false,
  'converter packaged assets share one aggregate byte limit');
assert.deepEqual(converterAllocationSizes, [9],
  'an over-limit payload is rejected after staging only its key');
assert.match(converter.error(), /aggregate byte limit/);
assert.equal(converter.assetBytes(), 7);
assert.equal(converter.setAsset('extra.txt', new Uint8Array(17)), false,
  'rejected replacement leaves the previous packaged asset intact');
assert.equal(converter.assetBytes(), 7);
assert.equal(converter.setAsset('extra.txt', encode('updated')), true);
assert.equal(converter.error(), '');
const converterHeapPayload = new Uint8Array([11, 22, 33, 44]);
const converterHeapPtr = converterAlloc(converterHeapPayload.length);
assert.ok(converterHeapPtr);
module.HEAPU8.set(converterHeapPayload, Number(converterHeapPtr));
const converterHeapView = new Uint8Array(module.HEAPU8.buffer,
  Number(converterHeapPtr), converterHeapPayload.length);
assert.equal(converter.setAsset('heap.bin', converterHeapView), true);
module._lightusd_next_free(converterHeapPtr);
assert.equal(converter.assetBytes(), 11);
assert.equal(converter.retainedPayloadBytes(), encode(text).length + 11,
  'root input and packaged assets share retained-payload accounting');
assert.equal(converter.setMaxRetainedPayloadBytes(converter.retainedPayloadBytes()).success, true);
const rootPreflightAlloc = module._lightusd_next_alloc;
const rootPreflightAllocations = [];
module._lightusd_next_alloc = size => {
  rootPreflightAllocations.push(Number(size));
  return rootPreflightAlloc(size);
};
let overBudgetRootAccepted;
try {
  overBudgetRootAccepted = converter.loadFromBinary(encode(text + ' '), 'scene.usda');
} finally { module._lightusd_next_alloc = rootPreflightAlloc; }
assert.equal(overBudgetRootAccepted, false,
  'root replacement is rejected against aggregate root/asset payload usage');
assert.deepEqual(rootPreflightAllocations, [],
  'root aggregate rejection happens before WASM staging allocation');
assert.equal(converter.retainedPayloadBytes(), encode(text).length + 11,
  'rejected root replacement preserves retained state');
assert.match(converter.error(), /aggregate retained byte limit/);
assert.equal(converter.setMaxRetainedPayloadBytes(1024 * 1024).success, true);
assert.deepEqual(converter.setMaxAssetBytes(10), {
  success: false, error: 'Asset byte limit is invalid or below retained payload bytes'
});
converter.setAsset('extra.txt', encode('updated'));
assert.deepEqual(converter.setMaxAssetBytes(6), {
  success: false, error: 'Asset byte limit is invalid or below retained payload bytes'
});
assert.match(converter.error(), /below retained payload bytes/);
const retainedBeforeMesh = converter.retainedPayloadBytes();
assert.equal(converter.setMaxRetainedPayloadBytes(retainedBeforeMesh).success, true);
const meshBudgetAlloc = module._lightusd_next_alloc;
const meshBudgetAllocations = [];
module._lightusd_next_alloc = size => {
  meshBudgetAllocations.push(Number(size));
  return meshBudgetAlloc(size);
};
let aggregateMeshAccepted;
try {
  aggregateMeshAccepted = converter.setCollisionMesh('aggregate-block', triangle,
    null, null, new Uint32Array([0, 1, 2]));
} finally { module._lightusd_next_alloc = meshBudgetAlloc; }
assert.equal(aggregateMeshAccepted, false,
  'mesh payload is charged against the root and asset aggregate');
assert.deepEqual(meshBudgetAllocations, [15],
  'aggregate mesh rejection allocates only the preflight name');
assert.equal(converter.setMaxRetainedPayloadBytes(retainedBeforeMesh + 48).success, true);
assert.equal(converter.setCollisionMesh('aggregate-block', triangle,
  null, null, new Uint32Array([0, 1, 2])), true);
assert.equal(converter.retainedPayloadBytes(), retainedBeforeMesh + 48);
assert.match(converter.exportAsUSDA(), /^#usda 1\.0/);
assert.equal(new TextDecoder().decode(converter.exportAsUSDC().subarray(0, 8)), 'PXR-USDC');
const exportAllocation = module._lightusd_next_alloc;
const exportAllocations = [];
module._lightusd_next_alloc = size => {
  exportAllocations.push(Number(size));
  return exportAllocation(size);
};
let ownedUSDZ;
try { ownedUSDZ = converter.exportAsUSDZ(); }
finally { module._lightusd_next_alloc = exportAllocation; }
assert.deepEqual(exportAllocations, [],
  'converter export copies directly from its retained C++ vector into the owned JS result');
assert.deepEqual(Array.from(ownedUSDZ.subarray(0, 4)), [80, 75, 3, 4]);
ownedUSDZ[0] = 0;
assert.deepEqual(Array.from(converter.exportAsUSDZ().subarray(0, 4)), [80, 75, 3, 4],
  'returned USDZ owns its bytes independently of converter state');
const json = converter.extractPhysicsSceneJSON();
assert.match(json, /18446744073709551615/);
assert.match(json, /-9223372036854775808/);
const prim = JSON.parse(json).prims.find(p => p.name === 'Box');
assert.ok(prim);
assert.deepEqual(prim.properties.samples, [1.25, 2.5]);
assert.deepEqual(prim.properties.nonfinite, [null, null, null]);
assert.deepEqual(prim.properties.labels, ['a', 'b']);
assert.equal(prim.properties.label, 'quote: "; unicode: 日本語');
assert.equal(prim.matrix.length, 16);
assert.deepEqual(prim.matrix.slice(12), [2, 3, 4, 1]);
assert.equal(prim.geometry.type, 'box');
assert.equal(prim.geometry.size, 2);

for (const options of ['{}', '{bad', '{"groups":[]}',
    '{"groups":["shade"],"groups":["core"]}']) {
  const result = JSON.parse(module.validateFromBinary(encode(text), 'scene.usda', options));
  assert.equal(result.parse_ok, true);
  assert.ok(Array.isArray(result.issues));
  assert.ok(result.checked_groups.includes('core'));
}
const validationInput = encode(`x${text}y`).subarray(1, -1);
assert.equal(JSON.parse(module.validateFromBinary(
  validationInput, 'scene.usda', '{}')).parse_ok, true);
const validationPtr = module._lightusd_next_alloc(validationInput.length + 2);
try {
  module.HEAPU8.set(validationInput, Number(validationPtr) + 1);
  const heapInput = module.HEAPU8.subarray(
    Number(validationPtr) + 1, Number(validationPtr) + 1 + validationInput.length);
  assert.equal(JSON.parse(module.validateFromBinary(
    heapInput, 'scene.usda', '{}')).parse_ok, true);
} finally {
  module._lightusd_next_free(validationPtr);
}
assert.throws(() => module.validateFromBinary(null, 'scene.usda', '{}'),
  /expected byte view/);
assert.throws(() => module.validateFromBinary(validationInput, 1, '{}'),
  /expected filename and options strings/);
const bad = JSON.parse(module.validateFromBinary(encode('not USD'), 'bad.usda', '{}'));
assert.equal(bad.parse_ok, false);
assert.equal(bad.ok, false);
assert.equal(typeof bad.error, 'string');
const exportLimitConverter = new module.NextUSDZConverterNative();
let entropySeed = 0x12345678;
let largeLabel = '';
const alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_';
const labelParts = [];
for (let block = 0; block < 1800; ++block) {
  let part = '';
  for (let i = 0; i < 1000; ++i) {
    entropySeed ^= entropySeed << 13;
    entropySeed ^= entropySeed >>> 17;
    entropySeed ^= entropySeed << 5;
    part += alphabet[(entropySeed >>> 0) & 63];
  }
  labelParts.push(part);
}
largeLabel = labelParts.join('');
assert.equal(exportLimitConverter.loadFromBinary(encode(
  `#usda 1.0\ndef Xform "Root" { custom string payload = "${largeLabel}" }`),
  'large.usda'), true, exportLimitConverter.error());
exportLimitConverter.setUSDCExportLimitMB(1, 0);
assert.equal(exportLimitConverter.exportAsUSDC(), null,
  'configured USDC file cap rejects output before publishing bytes');
assert.match(exportLimitConverter.error(), /configured file-size limit/);
exportLimitConverter.setUSDCExportLimitMB(2, 0);
const withinLimitCrate = exportLimitConverter.exportAsUSDC();
assert.ok(withinLimitCrate instanceof Uint8Array && withinLimitCrate.length <= 2 * 1024 * 1024);
exportLimitConverter.setUSDCExportLimitMB(2, 1);
assert.equal(exportLimitConverter.exportAsUSDC(), null,
  'configured estimated writer-memory cap rejects retained scene payload');
assert.match(exportLimitConverter.error(), /estimated working set/);
exportLimitConverter.setUSDCExportLimitMB(2, 4);
const withinMemoryLimitCrate = exportLimitConverter.exportAsUSDC();
assert.ok(withinMemoryLimitCrate instanceof Uint8Array,
  'configured estimated writer-memory cap accepts a sufficient budget');
const usdzAsUSDA = exportLimitConverter.exportAsUSDZWithOptions({rootLayerFormat: 'usda'});
assert.ok(usdzAsUSDA instanceof Uint8Array);
const usdzAsUSDAText = new TextDecoder().decode(usdzAsUSDA);
assert.ok(usdzAsUSDAText.includes('root.usda'), 'USDZ USDA root entry name');
assert.ok(usdzAsUSDAText.includes('#usda 1.0'), 'USDZ USDA root payload');
const usdzAsUSDC = exportLimitConverter.exportAsUSDZWithOptions({rootLayerFormat: 'usdc'});
assert.ok(new TextDecoder().decode(usdzAsUSDC).includes('root.usdc'),
  'USDZ USDC option preserves the default root format');
const packagedUSDA = new TextDecoder().decode(
  converter.exportAsUSDZWithOptions({rootLayerFormat: 'usda'}));
assert.ok(packagedUSDA.includes('root.usda'));
assert.ok(packagedUSDA.includes('extra.txt'), 'USDA root export retains packaged assets');
assert.throws(() => exportLimitConverter.exportAsUSDZWithOptions({rootLayerFormat: 'usd'}),
  /rootLayerFormat must be usdc or usda/);
exportLimitConverter.delete();
converter.delete();
assert.throws(() => converter.exportAsUSDA(), /Invalid LightUSD receiver/);
const staleConverter = module._lightusd_next_create(1) >>> 0;
assert.ok(staleConverter);
assert.equal(module._lightusd_next_converter_set_bytes(
  staleConverter, 0, wasm64 ? 0n : 0, 0, wasm64 ? 0n : 0, 0x40000001), -1,
  'the C entry rejects converter inputs above 1 GiB before reading the supplied pointer');
module._lightusd_next_destroy(staleConverter);
assert.equal(module._lightusd_next_converter_export(staleConverter, 0), -1);
const loadProgressStream = new module.RenderStream();
assert.equal(loadProgressStream.ok(), false);
assert.equal(loadProgressStream.warn(), loadProgressStream.warning());
assert.equal(loadProgressStream.isParsingInProgress(), false);
assert.equal(loadProgressStream.getProgress().stage, 'idle');
let loadProgressEvents = 0;
loadProgressStream.setProgressCallback(() => {
  ++loadProgressEvents;
  assert.equal(loadProgressStream.isParsingInProgress(), true);
  loadProgressStream.cancelParsing();
  return true;
});
const progressInput = encode('#usda 1.0\n' + Array.from({length: 512}, (_, i) =>
  `def Xform "Progress_${i}" {}`).join('\n') + '\n');
const cancelledProgressLoad = loadProgressStream.begin(progressInput);
assert.equal(cancelledProgressLoad.success, false);
assert.ok(loadProgressEvents > 0, 'real USDA parse reports progress before cancellation');
assert.equal(loadProgressStream.wasCancelled(), true);
assert.equal(loadProgressStream.isParsingInProgress(), false);
assert.equal(loadProgressStream.getProgress().stage, 'cancelled');
assert.equal(loadProgressStream.getProgress().cancelRequested, true);
loadProgressStream.resetProgress();
assert.equal(loadProgressStream.wasCancelled(), false);
assert.equal(loadProgressStream.getProgress().stage, 'idle');
loadProgressStream.setProgressCallback(null);
const validAfterCancelLoad = loadProgressStream.begin(encode('#usda 1.0\ndef Xform "Loaded" {}\n'));
assert.equal(validAfterCancelLoad.success, true, validAfterCancelLoad.error);
assert.equal(loadProgressStream.ok(), true);
assert.equal(loadProgressStream.warn(), loadProgressStream.warning());
loadProgressStream.delete();
console.log('ok - next C dispatch lifetime, validation and JSON semantics');
