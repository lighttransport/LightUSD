// SPDX-License-Identifier: Apache-2.0
// Paired legacy/next behavior for layer export and validation workflows.
// Serialized outputs differ in formatting and byte layout between the two
// writers, so every output is compared semantically: USDA/USDC layers are
// reloaded through NextLayerDocument and compared as prim path -> type,
// specifier and property values; USDZ packages by entry names, asset bytes
// and the reloaded root layer. Rows naming this test in
// next-wasm-parity-gaps.json must match what it exercises.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-layer-export-behavior-parity.test.mjs';
const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const combinedUrl = process.env.LIGHTUSD_COMBINED_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_COMBINED_MODULE)).href
  : new URL(wasm64 ? '../src/lightusd/lightusd_combined_64.js'
                   : '../src/lightusd/lightusd_combined.js', import.meta.url).href;
const legacyModule = await loadWasm(() => import(combinedUrl));
const {default: createNext} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js',
  import.meta.url));
const nextModule = await createNext();
const encode = text => new TextEncoder().encode(text);
const fixture = name => new Uint8Array(fs.readFileSync(new URL(`../../../${name}`, import.meta.url)));

const exercised = new Map();  // method -> 'match' | 'delta'
const note = (method, kind) => {
  if (exercised.get(method) !== 'delta') exercised.set(method, kind);
};

function prims(json) {
  const out = {};
  const walk = (specs, base) => {
    for (const [name, prim] of Object.entries(specs || {})) {
      const primPath = `${base}/${name}`;
      const properties = {};
      for (const [prop, record] of Object.entries(prim.properties || {}))
        properties[prop] = record.attribute?.value ?? record.relationship?.targets ?? record.propertyType;
      out[primPath] = {type: prim.typeName, specifier: prim.specifier, properties};
      walk(prim.children, primPath);
    }
  };
  walk(json.primSpecs, '');
  return out;
}
// Any USDA text or USDC bytes -> semantic prim table.
function layerPrims(data) {
  const document = new nextModule.LayerDocument();
  try {
    const bytes = typeof data === 'string' ? encode(data) : data;
    const loaded = document.load(bytes);
    assert.equal(loaded.success, true, loaded.error);
    return prims(JSON.parse(document.exportJSON().text));
  } finally { document.delete(); }
}
function storedZipEntries(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const entries = new Map();
  let offset = 0;
  while (offset + 30 <= bytes.length && view.getUint32(offset, true) === 0x04034b50) {
    assert.equal(view.getUint16(offset + 8, true), 0, 'USDZ entries are stored');
    const size = view.getUint32(offset + 18, true);
    const nameLength = view.getUint16(offset + 26, true);
    const start = offset + 30 + nameLength + view.getUint16(offset + 28, true);
    entries.set(new TextDecoder().decode(bytes.subarray(offset + 30, offset + 30 + nameLength)),
      bytes.slice(start, start + size));
    offset = start + size;
  }
  return entries;
}
// USDZ -> {root entry format, asset entries with bytes, root layer prims}.
function packageContents(bytes) {
  assert.ok(bytes instanceof Uint8Array && bytes.length > 0);
  const entries = storedZipEntries(bytes);
  const names = [...entries.keys()];
  const root = names.find(name => /\.usd[ac]?$/.test(name));
  const assets = Object.fromEntries(names.filter(name => name !== root)
    .map(name => [name, Array.from(entries.get(name))]));
  return {rootFormat: root.split('.').pop(), assets, prims: layerPrims(entries.get(root))};
}

const png = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10, 7, 7, 7]);
const materialBytes = fixture('tests/usda/c-core-material-queries.usda');

// Layer serialization: USDA text, JSON, USDC.
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const document = new nextModule.LayerDocument();
  try {
    assert.equal(loader.loadAsLayerFromBinary(materialBytes, 'm.usda'), true);
    assert.equal(document.load(materialBytes).success, true);
    const expected = layerPrims(document.exportUSDA().text);
    for (const method of ['layerToString', 'exportAsUSDA']) {
      assert.deepEqual(layerPrims(loader[method]()), expected, method);
      note(method, 'match');
    }
    // Known delta: an attribute authoring both a default and `.connect` keeps
    // its connection in next; legacy drops it. Pin it, then compare the rest.
    const dropValueConnections = json => {
      let dropped = 0;
      const walk = specs => {
        for (const prim of Object.values(specs || {})) {
          for (const record of Object.values(prim.properties || {})) {
            const attribute = record.attribute;
            if (attribute?.connection && attribute.hasValue) {
              delete attribute.connection;
              attribute.isConnection = false;
              record.isAttributeConnection = false;
              ++dropped;
            }
          }
          walk(prim.children);
        }
      };
      walk(json.primSpecs);
      return dropped;
    };
    const nextJSON = JSON.parse(document.exportJSON().text);
    assert.equal(dropValueConnections(nextJSON), 2, 'value+connection attributes keep connections');
    assert.equal(dropValueConnections(JSON.parse(loader.layerToJSON())), 0);
    assert.deepEqual(nextJSON, JSON.parse(loader.layerToJSON()), 'layerToJSON');
    exercised.set('layerToJSON', 'delta');
    // Both products emit the same document for either embed/array mode.
    for (const [embed, mode] of [[false, 'base64'], [true, 'buffer']]) {
      const nextOptioned = JSON.parse(document.exportJSONWithOptions(embed, mode).text);
      dropValueConnections(nextOptioned);
      assert.deepEqual(nextOptioned, JSON.parse(loader.layerToJSONWithOptions(embed, mode)),
        `layerToJSONWithOptions ${embed}/${mode}`);
    }
    exercised.set('layerToJSONWithOptions', 'delta');
    const nextCrate = document.exportUSDC();
    assert.equal(nextCrate.success, true);
    assert.deepEqual(layerPrims(new Uint8Array(nextCrate.data)), expected);
    for (const method of ['exportAsUSDC', 'exportLayerAsUSDCWithOptions']) {
      const bytes = method === 'exportAsUSDC' ? loader.exportAsUSDC()
        : loader.exportLayerAsUSDCWithOptions({});
      assert.deepEqual(layerPrims(bytes), expected, method);
      note(method, 'match');
    }
    const legacyBuffer = new Uint8Array(1 << 16);
    const nextBuffer = new Uint8Array(1 << 16);
    const legacyWritten = loader.exportLayerAsUSDCToBufferWithOptions(legacyBuffer, {});
    const nextWritten = document.exportUSDCToBuffer(nextBuffer, {});
    assert.equal(legacyWritten.success, true);
    assert.equal(nextWritten.success, true);
    assert.deepEqual(layerPrims(nextBuffer.subarray(0, nextWritten.size)),
      layerPrims(legacyBuffer.subarray(0, legacyWritten.size)));
    note('exportLayerAsUSDCToBufferWithOptions', 'match');
  } finally { loader.delete(); document.delete(); }
}

// Stage USDC export from the converted scene.
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    assert.equal(loader.loadFromBinary(materialBytes, 'm.usda'), true);
    assert.equal(stream.begin(materialBytes, 'm.usda').success, true);
    const legacyBuffer = new Uint8Array(1 << 16);
    const nextBuffer = new Uint8Array(1 << 16);
    const legacyWritten = loader.exportStageAsUSDCToBufferWithOptions(legacyBuffer, {});
    const nextWritten = stream.exportUSDCToBuffer(nextBuffer, {});
    assert.equal(legacyWritten.success, true);
    assert.equal(nextWritten.success, true);
    // Known delta: legacy's stage export loses the mesh geometry arrays the
    // converter consumed; next keeps them. Everything else matches.
    const nextStage = layerPrims(nextBuffer.subarray(0, nextWritten.size));
    const geometry = ['points', 'faceVertexCounts', 'faceVertexIndices'];
    assert.deepEqual(geometry.filter(key => key in nextStage['/World/Triangle'].properties), geometry);
    for (const key of geometry) delete nextStage['/World/Triangle'].properties[key];
    assert.deepEqual(nextStage, layerPrims(legacyBuffer.subarray(0, legacyWritten.size)));
    exercised.set('exportStageAsUSDCToBufferWithOptions', 'delta');
  } finally { loader.delete(); stream.delete(); }
}

// Layer -> render scene.
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const document = new nextModule.LayerDocument();
  const stream = new nextModule.RenderStream();
  try {
    assert.equal(loader.loadAsLayerFromBinary(materialBytes, 'm.usda'), true);
    assert.equal(document.load(materialBytes).success, true);
    assert.equal(loader.layerToRenderScene(), true);
    assert.equal(stream.beginFromLayerDocument(document).success, true);
    for (const count of ['numMeshes', 'numMaterials', 'numTextures', 'numLights', 'numCameras'])
      assert.equal(stream[count](), loader[count](), `layerToRenderScene ${count}`);
    note('layerToRenderScene', 'match');
  } finally { loader.delete(); document.delete(); stream.delete(); }
}

// Flattening and asset-path remapping of the loaded layer.
const referenceRoot = `#usda 1.0\ndef Xform "Inst" (\n references = @ref.usda@</Asset>\n) {\n}\n`;
const referenceDep = `#usda 1.0\ndef Xform "Asset" {\n asset tex = @old.png@\n float weight = 2\n}\n`;
function nextFlatten(remap) {
  const session = new nextModule.NextFlattenSession();
  try {
    assert.equal(session.begin(encode(referenceRoot), 'root.usda', true).success, true);
    let count;
    if (remap) count = session.remapLayerAssetPaths(remap);
    for (let i = 0; i < 8; ++i) {
      const step = session.step(null);
      assert.equal(step.success, true, step.error);
      if (step.status === 'need-layer') {
        session.provideLayer(step.key, encode(referenceDep));
        continue;
      }
      return {count, prims: layerPrims(new Uint8Array(step.data))};
    }
    throw new Error('flatten did not converge');
  } finally { session.end(); session.delete(); }
}
{
  const loader = new legacyModule.LightUSDLoaderNative();
  try {
    loader.setAsset('ref.usda', encode(referenceDep));
    assert.equal(loader.loadAsLayerFromBinary(encode(referenceRoot), 'root.usda'), true);
    assert.equal(loader.flattenLayer(), true, loader.error());
    const flattened = layerPrims(loader.layerToString());
    // Known delta (next's relative asset path policy): legacy re-anchors a
    // referenced layer's asset as "./old.png"; next keeps the authored text.
    assert.equal(flattened['/Inst'].properties.tex, '@./old.png@');
    flattened['/Inst'].properties.tex = '@old.png@';
    assert.deepEqual(nextFlatten().prims, flattened, 'flattenLayer');
    exercised.set('flattenLayer', 'delta');
  } finally { loader.delete(); }
}
{
  // remapLayerAssetPaths rewrites asset values on the retained root layer.
  const root = `#usda 1.0\ndef Xform "A" {\n asset tex = @old.png@\n asset other = @keep.png@\n}\n`;
  const loader = new legacyModule.LightUSDLoaderNative();
  const session = new nextModule.NextFlattenSession();
  try {
    assert.equal(loader.loadAsLayerFromBinary(encode(root), 'root.usda'), true);
    assert.equal(session.begin(encode(root), 'root.usda', true).success, true);
    const remap = {'old.png': 'textures/new.png'};
    assert.equal(session.remapLayerAssetPaths(remap), loader.remapLayerAssetPaths(remap));
    const step = session.step(null);
    assert.equal(step.status, 'done');
    assert.deepEqual(layerPrims(new Uint8Array(step.data)), layerPrims(loader.layerToString()));
    note('remapLayerAssetPaths', 'match');
  } finally { loader.delete(); session.end(); session.delete(); }
}

// USDZ packaging (converter and layer-document packages).
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const converter = new nextModule.NextUSDZConverterNative();
  try {
    loader.setAsset('surface.png', png);
    assert.equal(loader.loadAsLayerFromBinary(materialBytes, 'm.usda'), true);
    assert.equal(converter.loadFromBinary(materialBytes, 'm.usda'), true);
    converter.setAsset('surface.png', png);
    const legacyPlain = packageContents(loader.exportAsUSDZ());
    assert.deepEqual(packageContents(converter.exportAsUSDZ()), legacyPlain, 'exportAsUSDZ');
    note('exportAsUSDZ', 'match');
    assert.deepEqual(packageContents(converter.exportAsUSDZWithOptions({rootLayerFormat: 'usda'})),
      packageContents(loader.exportAsUSDZWithOptions({}, {rootLayerFormat: 'usda'})),
      'exportAsUSDZWithOptions');
    note('exportAsUSDZWithOptions', 'match');
    // Known delta: both rewrite the root layer's texture path, but legacy
    // packs the asset under its original name (a dangling reference); next
    // packs it under the remapped name.
    const remap = {'surface.png': 'tex/surface.png'};
    const nextRemapped = packageContents(converter.exportAsUSDZWithRemap(remap));
    const legacyRemapped = packageContents(loader.exportAsUSDZWithRemap(remap));
    assert.deepEqual(Object.keys(nextRemapped.assets), ['tex/surface.png']);
    assert.deepEqual(Object.keys(legacyRemapped.assets), ['surface.png']);
    assert.deepEqual(nextRemapped.prims, legacyRemapped.prims, 'remapped root layer');
    assert.deepEqual(nextRemapped.assets['tex/surface.png'], legacyRemapped.assets['surface.png']);
    exercised.set('exportAsUSDZWithRemap', 'delta');
  } finally { loader.delete(); converter.delete(); }
}
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const document = new nextModule.LayerDocument();
  const store = new nextModule.NextAssetStore();
  try {
    loader.setAsset('surface.png', png);
    store.setAsset('surface.png', png);
    assert.equal(loader.loadAsLayerFromBinary(materialBytes, 'm.usda'), true);
    assert.equal(document.load(materialBytes).success, true);
    for (const rootLayerFormat of ['usdc', 'usda']) {
      const exported = document.exportUSDZ({rootLayerFormat}, store);
      assert.equal(exported.success, true, exported.error);
      assert.deepEqual(packageContents(exported.data),
        packageContents(loader.exportLayerAsUSDZWithOptions({rootLayerFormat})),
        `exportLayerAsUSDZWithOptions ${rootLayerFormat}`);
    }
    note('exportLayerAsUSDZWithOptions', 'match');
  } finally { loader.delete(); document.delete(); store.delete(); }
}

// ---------------------------------------------------------- matrix sync
const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
const claimed = matrix.filter(row => row.behaviorTest === TEST_NAME);
assert.deepEqual(claimed.map(row => row.legacyMethod).sort(), [...exercised.keys()].sort(),
  'every method exercised here must carry this behaviorTest in the parity matrix, and vice versa');
for (const row of claimed) {
  const kind = exercised.get(row.legacyMethod);
  if (row.parityStatus === 'behavior_verified') {
    assert.equal(Boolean(row.edgeDifference), kind === 'delta', row.legacyMethod);
  } else {
    assert.equal(row.parityStatus, 'known_behavior_gap', row.legacyMethod);
    assert.equal(kind, 'delta', row.legacyMethod);
  }
}
const gaps = claimed.filter(row => row.parityStatus === 'known_behavior_gap').length;
console.log(`ok - layer export behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length - gaps} methods verified against legacy, ${gaps} documented gaps`);
