// SPDX-License-Identifier: Apache-2.0
// Render-scene queries: next's RenderStream record shapes are the product
// contract. For every legacy render-scene query this test pairs the combined
// legacy loader with the next-only RenderStream on representative fixtures,
// matches resources by prim path (the two converters order them
// differently), and pins the exact contract difference: fields only legacy
// returns, fields only next returns, and shared fields whose values differ.
// The reviewed snapshot lives in next-render-scene-contract.json; run with
// PIN_UPDATE=1 to regenerate it after an intentional change. Rows naming this
// test in next-wasm-parity-gaps.json must match what it exercises.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-render-scene-behavior-parity.test.mjs';
const SNAPSHOT = new URL('./next-render-scene-contract.json', import.meta.url);
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
const fixture = name => new Uint8Array(fs.readFileSync(new URL(`../../../${name}`, import.meta.url)));
const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
// Call each legacy query's mapped next method (e.g. getMeshPtr ->
// getMeshGeometryView, getImage -> getImageCopy).
const nextName = new Map(matrix.map(row => [row.legacyMethod, row.nextEquivalent || row.legacyMethod]));

// Values are compared after rounding (float32 vs double storage) and with
// typed arrays as plain arrays. Heap pointers, memory-dependent sizes and
// cross-resource ids (which follow each product's enumeration order; legacy's
// order even varies between wasm32 and memory64 builds) are never compared.
const VOLATILE = new Set(['ptr', 'bufferPtr', 'byteOffset', 'heapBytes', 'id', 'anim_id', 'index',
  'skeleton_id', 'target_node']);
const norm = value => {
  if (ArrayBuffer.isView(value)) return Array.from(value, norm);
  if (Array.isArray(value)) return value.map(norm);
  if (typeof value === 'number') return Number.isFinite(value) ? Math.round(value * 1e4) / 1e4 : String(value);
  if (typeof value === 'bigint') return `${value}n`;
  if (value && typeof value === 'object') {
    const out = {};
    for (const key of Object.keys(value).sort()) if (!VOLATILE.has(key)) out[key] = norm(value[key]);
    return out;
  }
  return value;
};
// Large values are pinned by length and digest to keep the snapshot readable.
const digest = value => {
  const text = JSON.stringify(value);
  if (text === undefined || text.length <= 120) return value;
  let hash = 2166136261;
  for (let i = 0; i < text.length; ++i) hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
  return `#${text.length}:${hash.toString(16)}`;
};
const contract = (legacyValue, nextValue) => {
  const a = norm(legacyValue);
  const b = norm(nextValue);
  const plain = x => !x || typeof x !== 'object' || Array.isArray(x);
  if (plain(a) || plain(b)) {
    return JSON.stringify(a) === JSON.stringify(b) ? {same: true}
      : {legacy: digest(a), next: digest(b)};
  }
  const legacyOnly = Object.keys(a).filter(key => !(key in b));
  const nextOnly = Object.keys(b).filter(key => !(key in a));
  const differ = {};
  for (const key of Object.keys(a)) {
    if (key in b && JSON.stringify(a[key]) !== JSON.stringify(b[key]))
      differ[key] = {legacy: digest(a[key]), next: digest(b[key])};
  }
  const out = {};
  if (legacyOnly.length) out.legacyOnly = legacyOnly;
  if (nextOnly.length) out.nextOnly = nextOnly;
  if (Object.keys(differ).length) out.differ = differ;
  return Object.keys(out).length ? out : {same: true};
};
const outcome = fn => {
  try { return fn(); } catch (error) { return {throws: error.constructor.name}; }
};
const keyOf = record => record?.absPath ?? record?.abs_path ?? record?.primPath ??
  record?.prim_path ?? record?.path ?? record?.name;

const observed = {};  // "fixture :: call" -> contract
const methods = new Set();
function record(label, method, legacyValue, nextValue) {
  methods.add(method);
  observed[`${label} :: ${method}`] = contract(legacyValue, nextValue);
}
function loadBoth(name, {assets = {}} = {}) {
  const loader = new legacyModule.LightUSDLoaderNative();
  const store = new nextModule.NextAssetStore();
  const stream = new nextModule.RenderStream();
  for (const [asset, bytes] of Object.entries(assets)) {
    loader.setAsset(asset, bytes);
    store.setAsset(asset, bytes);
  }
  if (Object.keys(assets).length) stream.setAssetStore(store);
  const bytes = fixture(name);
  const file = name.split('/').pop();
  assert.equal(loader.loadFromBinary(bytes, file), true, `${name}: ${loader.error()}`);
  assert.equal(stream.begin(bytes, file).success, true, `${name}: ${stream.error()}`);
  return {loader, stream, done() { loader.delete(); stream.delete(); store.delete(); }};
}
// Same call with the same arguments on both products.
function same(scene, label, method, args = []) {
  record(label, method, outcome(() => scene.loader[method](...args)),
    outcome(() => scene.stream[nextName.get(method)](...args)));
}
// Collection getters: pair records by prim path.
function byPath(scene, label, countMethod, method) {
  same(scene, label, countMethod);
  const collect = target => {
    const out = new Map();
    for (let i = 0; i < target[countMethod](); ++i) {
      const item = target[method](i);
      out.set(keyOf(item) ?? `#${i}`, item);
    }
    return out;
  };
  const legacyItems = collect(scene.loader);
  const nextItems = collect(scene.stream);
  const keys = [...new Set([...legacyItems.keys(), ...nextItems.keys()])].sort();
  for (const key of keys) {
    record(`${label} ${key}`, method, legacyItems.get(key) ?? {missing: true},
      nextItems.get(key) ?? {missing: true});
  }
}

// Meshes, nodes and scene metadata.
{
  const scene = loadBoth('tests/usda/cube-000.usda');
  try {
    const label = 'cube-000';
    for (const method of ['numMeshes', 'numRootNodes', 'getDefaultRootNodeId',
      'getDefaultRootNode', 'getSceneMetadata', 'getUpAxis', 'getURI'])
      same(scene, label, method);
    for (const method of ['getMesh', 'getMeshCopy', 'getMeshPtr', 'getMeshPrimvarsJSON',
      'getRootNode'])
      same(scene, label, method, [0]);
  } finally { scene.done(); }
}
{
  const scene = loadBoth('tests/usda/primvar-interpolation-001.usda');
  try {
    same(scene, 'primvar-interpolation-001', 'getMeshPrimvarsJSON', [0]);
  } finally { scene.done(); }
}
// Lights and cameras.
{
  const scene = loadBoth('tests/usda/lights-full-001.usda');
  try {
    byPath(scene, 'lights-full-001', 'numLights', 'getLight');
    const paths = list => list.map(keyOf).sort();
    record('lights-full-001', 'getAllLights', paths(scene.loader.getAllLights()),
      paths(scene.stream.getAllLights()));
  } finally { scene.done(); }
}
{
  const scene = loadBoth('tests/usda/camera-full-001.usda');
  try { byPath(scene, 'camera-full-001', 'numCameras', 'getCamera'); }
  finally { scene.done(); }
}
// Skeletons and animations.
{
  const scene = loadBoth('tests/usda/skelanimation-full-001.usda');
  try {
    const label = 'skelanimation-full-001';
    byPath(scene, label, 'numSkeletons', 'getSkeleton');
    const flat = target => new Map(Array.from({length: target.numSkeletons()},
      (_, i) => [keyOf(target.getSkeleton(i)), target.getSkeletonJointsFlat(i)]));
    const legacyFlat = flat(scene.loader);
    const nextFlat = flat(scene.stream);
    for (const key of [...legacyFlat.keys()].sort())
      record(`${label} ${key}`, 'getSkeletonJointsFlat', legacyFlat.get(key), nextFlat.get(key));
    const paths = list => list.map(keyOf).sort();
    record(label, 'getAllSkeletons', paths(scene.loader.getAllSkeletons()),
      paths(scene.stream.getAllSkeletons()));
    same(scene, label, 'numAnimations');
    // Pair clips by their source prim path.
    const clips = target => new Map(Array.from({length: target.numAnimations()},
      (_, i) => [keyOf(target.getAnimation(i)), i]));
    const legacyClips = clips(scene.loader);
    const nextClips = clips(scene.stream);
    for (const key of [...legacyClips.keys()].sort()) {
      const [a, b] = [legacyClips.get(key), nextClips.get(key)];
      record(`${label} ${key}`, 'getAnimationInfo', scene.loader.getAnimationInfo(a),
        b === undefined ? {missing: true} : scene.stream.getAnimationInfo(b));
      record(`${label} ${key}`, 'getAnimation', scene.loader.getAnimation(a),
        b === undefined ? {missing: true} : scene.stream.getAnimation(b));
    }
    record(label, 'getAllAnimations', scene.loader.getAllAnimations().length,
      scene.stream.getAllAnimations().length);
    record(label, 'getAllAnimationInfos', scene.loader.getAllAnimationInfos().length,
      scene.stream.getAllAnimationInfos().length);
  } finally { scene.done(); }
}
// Textures and images (texture bytes supplied to both products).
function onePixelPNG() {
  const table = Array.from({length: 256}, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc = bytes => {
    let c = 0xffffffff;
    for (const b of bytes) c = table[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type, data) => {
    const head = Buffer.alloc(4); head.writeUInt32BE(data.length);
    const tail = Buffer.alloc(4); tail.writeUInt32BE(crc(Buffer.concat([Buffer.from(type), data])));
    return Buffer.concat([head, Buffer.from(type), data, tail]);
  };
  const header = Buffer.alloc(13);
  header.writeUInt32BE(1, 0); header.writeUInt32BE(1, 4); header[8] = 8; header[9] = 2;
  return new Uint8Array(Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', header), chunk('IDAT', zlib.deflateSync(Buffer.from([0, 255, 0, 0]))),
    chunk('IEND', Buffer.alloc(0))]));
}
{
  const scene = loadBoth('tests/usda/c-core-material-queries.usda',
    {assets: {'surface.png': onePixelPNG()}});
  try {
    const label = 'c-core-material-queries';
    for (const method of ['numTextures', 'numImages', 'extractUnresolvedTexturePaths'])
      same(scene, label, method);
    for (const method of ['getTexture', 'getImage', 'getImageCopy', 'getImagePtr'])
      same(scene, label, method, [0]);
  } finally { scene.done(); }
}
{
  const scene = loadBoth('tests/usda/shader.usda');
  try { same(scene, 'shader', 'extractUnresolvedTexturePaths'); }
  finally { scene.done(); }
}
{
  const scene = loadBoth('tests/usda/udim-material-bound.usda');
  try {
    same(scene, 'udim-material-bound', 'numUDIMTextures');
    same(scene, 'udim-material-bound', 'getUDIMTexture', [0]);
  } finally { scene.done(); }
}
// Native instancing.
{
  const scene = loadBoth('tests/usda/instancing-001.usda');
  try {
    same(scene, 'instancing-001', 'numInstances');
    // Legacy's instance order is not stable across builds, so compare the
    // sorted set of instance paths rather than one index.
    const instancePaths = target => Array.from({length: target.numInstances()},
      (_, i) => keyOf(target.getInstance(i))).sort();
    record('instancing-001', 'getInstance', instancePaths(scene.loader),
      instancePaths(scene.stream));
    same(scene, 'instancing-001', 'getInstancesForMesh', [0]);
  } finally { scene.done(); }
}
// Render-optimization flag getters (legacy getNative* names).
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    for (const [legacyName, nextName] of [['getNativeMeshMerge', 'getMeshMerge'],
      ['getNativeMaterialDedup', 'getMaterialDedup'],
      ['getNativeMeshMergeBakeTransform', 'getMeshMergeBakeTransform'],
      ['getNativeFlattenRenderTree', 'getFlattenRenderTree']]) {
      const setter = nextName.replace(/^get/, 'set');
      for (const value of [true, false]) {
        loader[legacyName.replace(/^get/, 'set')](value);
        stream[setter](value);
        record(`flags ${value}`, legacyName, loader[legacyName](), stream[nextName]());
      }
    }
  } finally { loader.delete(); stream.delete(); }
}
// Layer shading/profile JSON on the loaded layer.
{
  const bytes = fixture('tests/usda/c-core-material-queries.usda');
  const loader = new legacyModule.LightUSDLoaderNative();
  const document = new nextModule.LayerDocument();
  try {
    assert.equal(loader.loadAsLayerFromBinary(bytes, 'm.usda'), true);
    assert.equal(document.load(bytes).success, true);
    for (const method of ['getShadingGraphJSON', 'getMhProfileJSON']) {
      const parse = value => { try { return JSON.parse(value?.text ?? value); } catch { return value; } };
      record('c-core-material-queries layer', method, parse(outcome(() => loader[method]())),
        parse(outcome(() => document[method]())));
    }
  } finally { loader.delete(); document.delete(); }
}

// ------------------------------------------------------------ snapshot
if (process.env.PIN_UPDATE === '1') {
  fs.writeFileSync(SNAPSHOT, JSON.stringify(observed, null, 2) + '\n');
  console.log(`wrote ${Object.keys(observed).length} contract entries to ${SNAPSHOT.pathname}`);
} else {
  assert.deepEqual(JSON.parse(JSON.stringify(observed)), JSON.parse(fs.readFileSync(SNAPSHOT, 'utf8')),
    'render-scene contract drifted from the reviewed snapshot; rerun with PIN_UPDATE=1 after review');
}

// ---------------------------------------------------------- matrix sync
const claimed = matrix.filter(row => row.behaviorTest === TEST_NAME);
assert.deepEqual(claimed.map(row => row.legacyMethod).sort(), [...methods].sort(),
  'every method exercised here must carry this behaviorTest in the parity matrix, and vice versa');
for (const row of claimed) {
  const identical = Object.entries(observed)
    .filter(([key]) => key.endsWith(` :: ${row.legacyMethod}`))
    .every(([, value]) => value.same === true);
  assert.equal(row.parityStatus, identical ? 'behavior_verified' : 'known_behavior_gap',
    `${row.legacyMethod}: status must follow the pinned contract`);
  if (!identical) assert.ok(row.knownDifference, `${row.legacyMethod}: document the difference`);
}
const gaps = claimed.filter(row => row.parityStatus === 'known_behavior_gap').length;
console.log(`ok - render-scene behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length - gaps} methods identical to legacy, ${gaps} pinned next-contract differences`);
