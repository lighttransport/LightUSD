// SPDX-License-Identifier: Apache-2.0
// Allocation-failure gate for the next-only WASM module. Every JS wrapper
// stages inputs/outputs through _lightusd_next_alloc; this test fails the
// k-th allocation of each operation (for every k the operation performs) and
// requires that the call:
//   - reports the failure as a JS Error or a failure result (never a WASM
//     trap/abort),
//   - writes nothing through a null pointer (heap bytes at address 0 are
//     unchanged),
//   - releases every staging buffer it allocated, and
//   - leaves the receiving object usable: the same operation succeeds again
//     once allocation recovers.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import zlib from 'node:zlib';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const {default: createNext} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js',
  import.meta.url));
const M = await createNext();
const encode = text => new TextEncoder().encode(text);
const fixture = name => new Uint8Array(fs.readFileSync(new URL(`../../../${name}`, import.meta.url)));

// ---------------------------------------------------------------- injector
const realAlloc = M._lightusd_next_alloc;
const realFree = M._lightusd_next_free;
const key = ptr => String(ptr);
const injector = {armed: false, failAt: 0, count: 0, live: new Set(), injected: 0};
M._lightusd_next_alloc = size => {
  if (injector.armed && ++injector.count === injector.failAt) {
    ++injector.injected;
    return typeof realAlloc(0) === 'bigint' ? 0n : 0;  // probe keeps pointer width
  }
  const ptr = realAlloc(size);
  if (injector.armed && ptr) injector.live.add(key(ptr));
  return ptr;
};
M._lightusd_next_free = ptr => {
  injector.live.delete(key(ptr));
  return realFree(ptr);
};
// The width probe above allocates a zero-size block; release it eagerly.
{
  const probe = realAlloc(0);
  if (probe) realFree(probe);
}
function countAllocations(fn) {
  Object.assign(injector, {armed: true, failAt: 0, count: 0});
  try { fn(); } finally { injector.armed = false; }
  return injector.count;
}

// ----------------------------------------------------------------- fixtures
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
const png = onePixelPNG();
const materialScene = fixture('tests/usda/c-core-material-queries.usda');
const skinScene = fixture('tests/usda/skelanimation-full-001.usda');
const lightScene = fixture('tests/usda/lights-full-001.usda');
const cameraScene = fixture('tests/usda/camera-full-001.usda');
const referenceRoot = encode('#usda 1.0\ndef Xform "Inst" (\n references = @ref.usda@</Asset>\n) {\n}\n');
const referenceDep = encode('#usda 1.0\ndef Xform "Asset" {\n float weight = 2\n}\n');

function loadedStream(bytes, name, setup = () => {}) {
  const stream = new M.RenderStream();
  setup(stream);
  assert.equal(stream.begin(bytes, name).success, true, stream.error());
  return stream;
}
function loadedDocument(bytes = materialScene) {
  const document = new M.LayerDocument();
  assert.equal(document.load(bytes).success, true);
  return document;
}
function storeWithAssets() {
  const store = new M.NextAssetStore();
  store.setAsset('a.png', png);
  store.setAsset('b.bin', new Uint8Array([1, 2, 3]));
  return store;
}
const ok = result => result !== null && result !== undefined && result.success !== false &&
  result.error === undefined;

// Each scenario: make() creates fresh receivers (not counted); run() performs
// the operation under test and returns its result; done() releases them.
const scenarios = [
  // RenderStream loading and queries
  ['RenderStream.begin', () => ({stream: new M.RenderStream()}),
    s => s.stream.begin(materialScene, 'm.usda')],
  ['RenderStream.beginCachedAsset', () => {
    const store = new M.NextAssetStore(); store.setAsset('root.usda', materialScene);
    return {stream: new M.RenderStream(), store};
  }, s => s.stream.beginCachedAsset(s.store, 'root.usda')],
  ['RenderStream.getMesh', () => ({stream: loadedStream(materialScene, 'm.usda')}), s => s.stream.getMesh(0)],
  ['RenderStream.getMeshCopy', () => ({stream: loadedStream(materialScene, 'm.usda')}), s => s.stream.getMeshCopy(0)],
  ['RenderStream.getMeshPrimvarsJSON', () => ({stream: loadedStream(materialScene, 'm.usda')}),
    s => s.stream.getMeshPrimvarsJSON(0)],
  ['RenderStream.getMaterialRecord', () => ({stream: loadedStream(materialScene, 'm.usda')}),
    s => s.stream.getMaterialRecord(0)],
  ['RenderStream.getMaterialWithFormat', () => ({stream: loadedStream(materialScene, 'm.usda')}),
    s => s.stream.getMaterialWithFormat(0, 'json')],
  ['RenderStream.getAllTextures', () => ({stream: loadedStream(materialScene, 'm.usda')}),
    s => s.stream.getAllTextures()],
  ['RenderStream.getImageCopy', () => {
    const store = new M.NextAssetStore(); store.setAsset('surface.png', png);
    return {store, stream: loadedStream(materialScene, 'm.usda', stream => {
      stream.setAssetStore(store); stream.setLoadTextureInNative(true);
    })};
  }, s => s.stream.getImageCopy(0)],
  ['RenderStream.getAllLights', () => ({stream: loadedStream(lightScene, 'l.usda')}), s => s.stream.getAllLights()],
  ['RenderStream.getLightWithFormat', () => ({stream: loadedStream(lightScene, 'l.usda')}),
    s => s.stream.getLightWithFormat(0, 'json')],
  ['RenderStream.getCamera', () => ({stream: loadedStream(cameraScene, 'c.usda')}), s => s.stream.getCamera(0)],
  ['RenderStream.getAllSkeletons', () => ({stream: loadedStream(skinScene, 's.usda')}),
    s => s.stream.getAllSkeletons()],
  ['RenderStream.getAllAnimations', () => ({stream: loadedStream(skinScene, 's.usda')}),
    s => s.stream.getAllAnimations()],
  ['RenderStream.getDefaultRootNode', () => ({stream: loadedStream(skinScene, 's.usda')}),
    s => s.stream.getDefaultRootNode()],
  ['RenderStream.getSceneMetadata', () => ({stream: loadedStream(skinScene, 's.usda')}),
    s => s.stream.getSceneMetadata()],
  ['RenderStream.getStats', () => ({stream: loadedStream(skinScene, 's.usda')}), s => s.stream.getStats()],
  ['RenderStream.compositionReport', () => ({stream: loadedStream(materialScene, 'm.usda')}),
    s => s.stream.compositionReport()],
  ['RenderStream.extractVariants', () => ({stream: loadedStream(materialScene, 'm.usda')}),
    s => s.stream.extractVariants()],
  ['RenderStream.exportUSDCToBuffer', () => ({stream: loadedStream(materialScene, 'm.usda')}),
    s => s.stream.exportUSDCToBuffer(new Uint8Array(1 << 16), {})],
  ['RenderStream.provideAsset', () => ({stream: new M.RenderStream()}),
    s => (s.stream.provideAsset('clip.usda', referenceDep), {success: s.stream.getProvidedAsset('clip.usda').length > 0})],
  ['RenderStream.streaming', () => ({stream: new M.RenderStream()}), s => {
    assert.equal(s.stream.startStreamingAsset('s.bin', 3), true);
    return {success: s.stream.appendAssetChunk('s.bin', new Uint8Array([1, 2, 3])) &&
      s.stream.finalizeStreamingAsset('s.bin')};
  }],
  ['RenderStream.allocateZeroCopyBuffer', () => ({stream: new M.RenderStream()}),
    s => s.stream.allocateZeroCopyBuffer('z.bin', 16)],
  ['RenderStream.encodeImageNative', () => ({stream: new M.RenderStream()}),
    s => s.stream.encodeImageNative(new Uint8Array(16).fill(200), 2, 2, 4, 'png')],
  // LayerDocument
  ['LayerDocument.load', () => ({document: new M.LayerDocument()}), s => s.document.load(materialScene)],
  ['LayerDocument.exportUSDA', () => ({document: loadedDocument()}), s => s.document.exportUSDA()],
  ['LayerDocument.exportUSDC', () => ({document: loadedDocument()}), s => s.document.exportUSDC()],
  ['LayerDocument.exportJSON', () => ({document: loadedDocument()}), s => s.document.exportJSON()],
  ['LayerDocument.loadJSON', () => {
    const source = loadedDocument();
    const json = source.exportJSON().text;
    source.delete();
    return {document: new M.LayerDocument(), json};
  }, s => s.document.loadJSON(s.json)],
  ['LayerDocument.setStringAttribute', () => ({document: loadedDocument()}),
    s => s.document.setStringAttribute('/World', 'label', 'hello')],
  ['LayerDocument.validateLoadedLayer', () => ({document: loadedDocument()}),
    s => JSON.parse(s.document.validateLoadedLayer('{}'))],
  // NextAssetStore
  ['NextAssetStore.setAsset', () => ({store: new M.NextAssetStore()}),
    s => ({success: s.store.setAsset('x.bin', new Uint8Array([7, 8])) !== undefined})],
  ['NextAssetStore.getAsset', () => ({store: storeWithAssets()}), s => s.store.getAsset('a.png')],
  ['NextAssetStore.getAllAssetUUIDs', () => ({store: storeWithAssets()}), s => s.store.getAllAssetUUIDs()],
  ['NextAssetStore.getAssetHash', () => ({store: storeWithAssets()}), s => ({success: !!s.store.getAssetHash('a.png')})],
  // NextFlattenSession
  ['NextFlattenSession', () => ({session: new M.NextFlattenSession()}), s => {
    assert.equal(s.session.begin(referenceRoot, 'root.usda', true).success, true);
    for (let i = 0; i < 4; ++i) {
      const step = s.session.step(null);
      if (!step.success) return step;
      if (step.status === 'need-layer') { s.session.provideLayer(step.key, referenceDep); continue; }
      return step;
    }
    return {success: false};
  }],
  // NextUSDZConverterNative
  ['NextUSDZConverterNative.exportAsUSDZ', () => {
    const converter = new M.NextUSDZConverterNative();
    assert.equal(converter.loadFromBinary(materialScene, 'm.usda'), true);
    converter.setAsset('surface.png', png);
    return {converter};
  }, s => ({success: s.converter.exportAsUSDZ() instanceof Uint8Array})],
  ['NextUSDZConverterNative.setVisualMesh', () => ({converter: new M.NextUSDZConverterNative()}),
    s => ({success: s.converter.setVisualMesh('tri', new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
      null, null, new Int32Array([0, 1, 2]))})],
];
const release = s => { for (const value of Object.values(s)) if (value?.delete && !value.isDeleted?.()) value.delete(); };

const summary = [];
for (const [label, make, run] of scenarios) {
  // Baseline: the operation succeeds and performs n allocations.
  const base = make();
  let allocations;
  try {
    allocations = countAllocations(() => assert.ok(ok(run(base)), `${label}: baseline`));
  } finally { release(base); }
  for (let k = 1; k <= allocations; ++k) {
    const scene = make();
    try {
      const nullBefore = M.HEAPU8.slice(0, 64);
      Object.assign(injector, {armed: true, failAt: k, count: 0, live: new Set()});
      let outcome;
      try {
        outcome = {result: run(scene)};
      } catch (error) {
        outcome = {error};
      } finally {
        injector.armed = false;
      }
      if (outcome.error) {
        assert.ok(!(outcome.error instanceof WebAssembly.RuntimeError),
          `${label}: allocation ${k} trapped: ${outcome.error.message}`);
        assert.ok(outcome.error instanceof Error, `${label}: allocation ${k} threw a non-Error`);
      }
      assert.deepEqual(M.HEAPU8.slice(0, 64), nullBefore,
        `${label}: allocation ${k} wrote through a null pointer`);
      assert.deepEqual([...injector.live], [],
        `${label}: allocation ${k} leaked ${injector.live.size} staging buffer(s)`);
      // Recovery: the same receiver completes the operation afterwards.
      assert.ok(ok(run(scene)), `${label}: receiver unusable after allocation ${k} failed`);
    } finally { release(scene); }
  }
  summary.push(`${label}:${allocations}`);
}
assert.ok(injector.injected > 0, 'the gate injected allocation failures');

// Budget preflight: native-side allocations are bounded by limits checked
// before the large allocation. Each producer must reject an over-limit
// request cleanly (no trap), then succeed on the same object once the limit
// is raised.
const bigMesh = (() => {
  const n = 120000;  // > 1 MiB of Crate points, above the tightened caps
  const points = Array.from({length: n}, (_, i) => `(${i % 200},${Math.floor(i / 200)},0)`).join(',');
  const indices = Array.from({length: n}, (_, i) => i).join(',');
  return encode(`#usda 1.0\ndef Mesh "Big" {\n int[] faceVertexCounts = [${Array(n / 4).fill(4).join(',')}]\n` +
    ` int[] faceVertexIndices = [${indices}]\n point3f[] points = [${points}]\n}\n`);
})();
function rejectsThenRecovers(label, attempt, tighten, loosen) {
  tighten();
  let outcome;
  try { outcome = {result: attempt()}; } catch (error) { outcome = {error}; }
  if (outcome.error) {
    assert.ok(!(outcome.error instanceof WebAssembly.RuntimeError), `${label}: trapped`);
  } else {
    assert.ok(!ok(outcome.result), `${label}: over-limit request was accepted`);
  }
  loosen();
  assert.ok(ok(attempt()), `${label}: did not recover after raising the limit`);
}
// Each limit gets a fresh stream: lowering a limit below what a loaded
// scene already uses is itself (correctly) rejected.
for (const [label, run, tighten, loosen] of [
  ['RenderStream input limit', stream => stream.begin(bigMesh, 'big.usda'),
    stream => stream.setMaxInputBytes(1024), stream => stream.setMaxInputBytes(64 << 20)],
  ['RenderStream resident limit', stream => stream.begin(bigMesh, 'big.usda'),
    stream => stream.setMaxMemoryLimitMB(1), stream => stream.setMaxMemoryLimitMB(1024)],
  ['RenderStream provided-asset limit', stream => {
    stream.provideAsset('dep.usda', bigMesh);
    return {success: true};
  }, stream => stream.setProvidedAssetByteLimit(1024), stream => stream.setProvidedAssetByteLimit(0)]]) {
  const stream = new M.RenderStream();
  try {
    rejectsThenRecovers(label, () => run(stream), () => tighten(stream), () => loosen(stream));
  } finally { stream.delete(); }
}
{
  const store = new M.NextAssetStore();
  try {
    rejectsThenRecovers('NextAssetStore memory limit', () => {
      store.setAsset('big.usda', bigMesh);
      return {success: store.hasAsset('big.usda')};
    }, () => store.setMemoryLimitBytes(1024), () => store.setMemoryLimitBytes(64 << 20));
  } finally { store.delete(); }
}
{
  const flatten = () => {
    const session = new M.NextFlattenSession();
    return session;
  };
  const session = flatten();
  const run = () => {
    const begun = session.begin(bigMesh, 'big.usda', true);
    if (!begun.success) return begun;
    const step = session.step(null);
    session.end();
    return step;
  };
  try {
    rejectsThenRecovers('NextFlattenSession input limit', run,
      () => session.setMaxInputBytes(1024), () => session.setMaxInputBytes(64 << 20));
    rejectsThenRecovers('NextFlattenSession output limit', run,
      () => session.setMaxOutputBytes(1024), () => session.setMaxOutputBytes(64 << 20));
  } finally { session.delete(); }
}
{
  const converter = new M.NextUSDZConverterNative();
  try {
    assert.equal(converter.loadFromBinary(bigMesh, 'big.usda'), true);
    rejectsThenRecovers('NextUSDZConverterNative USDC limit', () =>
      ({success: converter.exportAsUSDC() instanceof Uint8Array}),
    () => converter.setUSDCExportLimitMB(1, 1), () => converter.setUSDCExportLimitMB(64, 256));
  } finally { converter.delete(); }
}
console.log(`ok - allocation-failure gate (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${scenarios.length} operations, ${injector.injected} injected failures, all recovered without leaks; ` +
  'budget preflight rejects and recovers on 7 limits');
if (process.env.VERBOSE) console.log(summary.join(' '));
