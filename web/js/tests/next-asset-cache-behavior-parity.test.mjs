// SPDX-License-Identifier: Apache-2.0
// Paired legacy/next behavior for the asset-cache and streaming-buffer
// families: one scripted scenario runs against the combined legacy loader and
// the next-only NextAssetStore/RenderStream, comparing every result after
// normalizing per-side UUIDs and heap pointers. Rows promoted to
// `behavior_verified` in next-wasm-parity-gaps.json must be exercised here, and
// every documented edge delta is pinned on both sides.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-asset-cache-behavior-parity.test.mjs';
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

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const POINTER_KEYS = new Set(['bufferPtr']);
function normalizer() {
  const tokens = new Map();
  const uuid = value => {
    if (!tokens.has(value)) tokens.set(value, `UUID${tokens.size}`);
    return tokens.get(value);
  };
  const norm = (value, key) => {
    if (typeof value === 'string') return UUID.test(value) ? uuid(value) : value;
    if (POINTER_KEYS.has(key)) return value ? 'PTR' : 0;
    if (typeof value === 'bigint') return `${value}n`;
    if (ArrayBuffer.isView(value)) return `${value.constructor.name}[${Array.from(value)}]`;
    if (Array.isArray(value)) return value.map(item => norm(item));
    if (value && typeof value === 'object') {
      const out = {};
      for (const k of Object.keys(value).sort()) out[norm(k)] = norm(value[k], k);
      return out;
    }
    return value;
  };
  return {norm, uuid};
}
const legacySide = normalizer();
const nextSide = normalizer();

const outcome = (fn, side) => {
  try { return {value: side.norm(fn())}; }
  catch (error) { return {throws: error.constructor.name}; }
};
const exercised = new Map();  // method -> 'match' | 'delta'
function note(method, kind) {
  if (exercised.get(method) !== 'delta') exercised.set(method, kind);
}
// Same arguments (or per-side arguments) on both products must agree.
function same(method, legacyTarget, nextTarget, legacyArgs, nextArgs = legacyArgs) {
  const a = outcome(() => legacyTarget[method](...legacyArgs), legacySide);
  const b = outcome(() => nextTarget[method](...nextArgs), nextSide);
  assert.deepEqual(b, a, `${method}(${legacyArgs.map(String).join(', ')}) must match legacy`);
  note(method, 'match');
  return a.value;
}
// A documented delta: pin each product's result so a change on either side fails.
function delta(method, legacyTarget, nextTarget, args, legacyExpected, nextExpected) {
  assert.deepEqual(outcome(() => legacyTarget[method](...args), legacySide), legacyExpected,
    `legacy ${method} delta changed`);
  assert.deepEqual(outcome(() => nextTarget[method](...args), nextSide), nextExpected,
    `next ${method} delta changed`);
  exercised.set(method, 'delta');
}
const bytes = text => new TextEncoder().encode(text);

// ---------------------------------------------------------------- asset cache
const loader = new legacyModule.LightUSDLoaderNative();
const store = new nextModule.NextAssetStore();
const stream = new nextModule.RenderStream();
try {
  for (const method of ['getAssetCount', 'getAssetCacheSizeBytes', 'getAssetCacheMaxSizeBytes',
    'getAssetSearchPaths', 'getAllowParentRelativeAssetPaths', 'getAllAssetUUIDs'])
    same(method, loader, store, []);
  // Legacy defaults its base path to "./"; next reports the resolver's ".".
  delta('getBaseWorkingPath', loader, store, [], {value: './'}, {value: '.'});
  for (const method of ['hasAsset', 'assetExists', 'getAssetHash', 'getAssetUUID'])
    same(method, loader, store, ['a.usda']);
  same('getAsset', loader, store, ['missing']);
  same('getAssetCacheDataAsMemoryView', loader, store, ['missing']);

  // setAsset: legacy returns undefined; next reports whether it replaced an entry.
  delta('setAsset', loader, store, ['a.usda', bytes('hello')], {value: undefined}, {value: false});
  delta('setAsset', loader, store, ['a.usda', bytes('hello2')], {value: undefined}, {value: true});
  delta('setAsset', loader, store, ['b.png', bytes('xyz')], {value: undefined}, {value: false});
  // Legacy stores an empty identifier; next rejects it.
  const emptyLoader = new legacyModule.LightUSDLoaderNative();
  try {
    delta('setAsset', emptyLoader, store, ['', bytes('x')],
      {value: undefined}, {throws: 'TypeError'});
    assert.equal(Number(emptyLoader.getAssetCount()), 1);
  } finally { emptyLoader.delete(); }

  for (const name of ['a.usda', './a.usda', 'b.png']) {
    same('hasAsset', loader, store, [name]);
    same('assetExists', loader, store, [name]);
  }
  same('getAssetCount', loader, store, []);
  same('getAssetCacheSizeBytes', loader, store, []);
  same('getAsset', loader, store, ['a.usda']);
  same('getAssetHash', loader, store, ['a.usda']);
  same('verifyAssetHash', loader, store, ['a.usda', 'deadbeef']);
  same('verifyAssetHash', loader, store, ['a.usda', loader.getAssetHash('a.usda')],
    ['a.usda', store.getAssetHash('a.usda')]);
  const legacyUuid = loader.getAssetUUID('a.usda');
  const nextUuid = store.getAssetUUID('a.usda');
  assert.match(legacyUuid, UUID);
  assert.match(nextUuid, UUID);
  same('getAssetUUID', loader, store, ['a.usda']);
  same('findAssetByUUID', loader, store, [legacyUuid], [nextUuid]);
  same('findAssetByUUID', loader, store, ['00000000-0000-4000-8000-000000000000']);
  same('getAssetByUUID', loader, store, [legacyUuid], [nextUuid]);
  same('getAssetByUUID', loader, store, ['00000000-0000-4000-8000-000000000000']);
  same('getAllAssetUUIDs', loader, store, []);
  same('getAssetCacheDataAsMemoryView', loader, store, ['a.usda']);

  // Raw heap pointers address each product's own WASM memory.
  const rawLegacy = legacyModule._lightusd_combined_alloc(4);
  const rawNext = nextModule._lightusd_next_alloc(4);
  try {
    legacyModule.HEAPU8.set([1, 2, 3, 4], Number(rawLegacy));
    nextModule.HEAPU8.set([1, 2, 3, 4], Number(rawNext));
    same('setAssetFromRawPointer', loader, store, ['raw.bin', rawLegacy, 4], ['raw.bin', rawNext, 4]);
    same('setAssetFromRawPointer', loader, store, ['raw.bin', rawLegacy, 4], ['raw.bin', rawNext, 4]);
  } finally {
    legacyModule._lightusd_combined_free(rawLegacy);
    nextModule._lightusd_next_free(rawNext);
  }
  same('getAsset', loader, store, ['raw.bin']);

  // Resolution configuration round-trips.
  same('setBaseWorkingPath', loader, store, ['/base']);
  same('getBaseWorkingPath', loader, store, []);
  for (const dir of ['/s1', '/s2', '/s1']) same('addAssetSearchPath', loader, store, [dir]);
  same('getAssetSearchPaths', loader, store, []);
  same('clearAssetSearchPaths', loader, store, []);
  same('getAssetSearchPaths', loader, store, []);
  for (const allow of [false, true]) {
    same('setAllowParentRelativeAssetPaths', loader, store, [allow]);
    same('getAllowParentRelativeAssetPaths', loader, store, []);
  }

  // Compatibility cap: sorted-key eviction on insertion, no eager eviction.
  same('setAssetCacheMaxSizeBytes', loader, store, [8]);
  same('getAssetCacheMaxSizeBytes', loader, store, []);
  delta('setAsset', loader, store, ['c.bin', bytes('12345')], {value: undefined}, {value: false});
  for (const name of ['a.usda', 'b.png', 'c.bin', 'raw.bin']) same('hasAsset', loader, store, [name]);
  same('getAssetCount', loader, store, []);
  same('getAssetCacheSizeBytes', loader, store, []);
  same('setAssetCacheMaxSizeBytes', loader, store, [0]);

  // Deletion by name, UUID, and the generic alias.
  same('deleteAssetByName', loader, store, ['b.png']);
  same('deleteAsset', loader, store, ['c.bin']);
  same('deleteAsset', loader, store, ['c.bin']);
  delta('setAsset', loader, store, ['d.bin', bytes('dd')], {value: undefined}, {value: false});
  same('deleteAssetByUUID', loader, store, [loader.getAssetUUID('d.bin')],
    [store.getAssetUUID('d.bin')]);
  same('deleteAssetByUUID', loader, store, ['00000000-0000-4000-8000-000000000000']);
  same('hasAsset', loader, store, ['d.bin']);
  same('clearAssets', loader, store, []);
  same('getAssetCount', loader, store, []);
  same('getAssetCacheSizeBytes', loader, store, []);

  // ------------------------------------------------------ chunked streaming
  // Legacy keeps streams on the loader; next keeps them on RenderStream.
  for (const method of ['isStreamingAssetComplete', 'getStreamingProgress', 'getStreamingAssetUUID'])
    same(method, loader, stream, ['s.usda']);
  same('startStreamingAsset', loader, stream, ['s.usda', 6]);
  same('getStreamingAssetUUID', loader, stream, ['s.usda']);
  same('appendAssetChunk', loader, stream, ['s.usda', bytes('abc')]);
  same('getStreamingProgress', loader, stream, ['s.usda']);
  same('isStreamingAssetComplete', loader, stream, ['s.usda']);
  same('finalizeStreamingAsset', loader, stream, ['s.usda']);
  same('appendAssetChunk', loader, stream, ['s.usda', 'def']);  // legacy accepts strings
  // Overflowing chunk: both reject, but legacy still appends the byte.
  same('appendAssetChunk', loader, stream, ['s.usda', bytes('g')]);
  const legacyStreamToken = legacySide.norm(loader.getStreamingAssetUUID('s.usda'));
  const nextStreamToken = nextSide.norm(stream.getStreamingAssetUUID('s.usda'));
  delta('getStreamingProgress', loader, stream, ['s.usda'],
    {value: {complete: true, current: 7, exists: true, percentage: 700 / 6, total: 6, uuid: legacyStreamToken}},
    {value: {complete: true, current: 6, exists: true, percentage: 100, total: 6,
      uuid: nextStreamToken}});
  same('isStreamingAssetComplete', loader, stream, ['s.usda']);
  same('finalizeStreamingAsset', loader, stream, ['s.usda']);
  for (const method of ['isStreamingAssetComplete', 'getStreamingProgress', 'getStreamingAssetUUID'])
    same(method, loader, stream, ['s.usda']);
  assert.equal(loader.hasAsset('s.usda'), true);
  assert.deepEqual(stream.providedAssetNames(), ['s.usda']);
  same('appendAssetChunk', loader, stream, ['nope', bytes('x')]);
  same('finalizeStreamingAsset', loader, stream, ['nope']);
  // A zero-length stream never completes in legacy; next finalizes it.
  same('startStreamingAsset', loader, stream, ['z.usda', 0]);
  delta('isStreamingAssetComplete', loader, stream, ['z.usda'], {value: false}, {value: true});
  delta('finalizeStreamingAsset', loader, stream, ['z.usda'], {value: false}, {value: true});

  // ---------------------------------------------------- zero-copy buffers
  same('getActiveZeroCopyBuffers', loader, stream, []);
  same('allocateZeroCopyBuffer', loader, stream, ['zero', 0, 0], ['zero', 0]);
  same('allocateZeroCopyBuffer', loader, stream, ['big', 9, 8], ['big', 9, 8]);
  const legacyBuffer = loader.allocateZeroCopyBuffer('zc.bin', 4, 0);
  const nextBuffer = stream.allocateZeroCopyBuffer('zc.bin', 4);
  assert.deepEqual(nextSide.norm(nextBuffer), legacySide.norm(legacyBuffer));
  note('allocateZeroCopyBuffer', 'match');
  const [lu, nu] = [legacyBuffer.uuid, nextBuffer.uuid];
  same('getActiveZeroCopyBuffers', loader, stream, []);
  const legacyPtr = loader.getZeroCopyBufferPtr(lu);
  const nextPtr = stream.getZeroCopyBufferPtr(nu);
  assert.equal(legacyPtr, legacyBuffer.bufferPtr);
  assert.equal(nextPtr, nextBuffer.bufferPtr);
  note('getZeroCopyBufferPtr', 'match');
  assert.equal(loader.getZeroCopyBufferPtrAtOffset(lu, 3), legacyPtr + 3);
  assert.equal(stream.getZeroCopyBufferPtrAtOffset(nu, 3), nextPtr + 3);
  same('getZeroCopyBufferPtrAtOffset', loader, stream, [lu, 4], [nu, 4]);
  legacyModule.HEAPU8.set([9, 8, 7, 6], legacyPtr);
  nextModule.HEAPU8.set([9, 8, 7, 6], nextPtr);
  same('getZeroCopyProgress', loader, stream, [lu], [nu]);
  same('markZeroCopyBytesWritten', loader, stream, [lu, 2], [nu, 2]);
  same('getZeroCopyProgress', loader, stream, [lu], [nu]);
  same('finalizeZeroCopyBuffer', loader, stream, [lu], [nu]);
  same('markZeroCopyBytesWritten', loader, stream, [lu, 2], [nu, 2]);
  same('getActiveZeroCopyBuffers', loader, stream, []);
  same('finalizeZeroCopyBuffer', loader, stream, [lu], [nu]);
  assert.deepEqual(Array.from(loader.getAsset('zc.bin').data), [9, 8, 7, 6]);
  assert.deepEqual(Array.from(stream.getProvidedAsset('zc.bin')), [9, 8, 7, 6]);
  for (const method of ['getZeroCopyProgress', 'getZeroCopyBufferPtr', 'cancelZeroCopyBuffer'])
    same(method, loader, stream, [lu], [nu]);
  same('getActiveZeroCopyBuffers', loader, stream, []);
  for (const method of ['cancelZeroCopyBuffer', 'getZeroCopyProgress', 'getZeroCopyBufferPtr'])
    same(method, loader, stream, ['bogus']);
  same('markZeroCopyBytesWritten', loader, stream, ['bogus', 1]);
  same('getZeroCopyBufferPtrAtOffset', loader, stream, ['bogus', 0]);

  // Over-count marks fail on both; legacy still clamps its written count.
  const lp = loader.allocateZeroCopyBuffer('p.bin', 3, 0).uuid;
  const np = stream.allocateZeroCopyBuffer('p.bin', 3).uuid;
  same('markZeroCopyBytesWritten', loader, stream, [lp, 4], [np, 4]);
  assert.equal(loader.getZeroCopyProgress(lp).bytesWritten, 3);
  assert.equal(stream.getZeroCopyProgress(np).bytesWritten, 0);
  exercised.set('markZeroCopyBytesWritten', 'delta');
  same('cancelZeroCopyBuffer', loader, stream, [lp], [np]);
  same('getActiveZeroCopyBuffers', loader, stream, []);
  // Chunked streams are not listed as zero-copy buffers on either product.
  same('startStreamingAsset', loader, stream, ['chunked.bin', 2]);
  same('getActiveZeroCopyBuffers', loader, stream, []);

  // mmap: legacy toggles a flag; next WASM storage is never mmap-backed.
  delta('getMMapZeroCopy', loader, stream, [], {value: false}, {throws: 'Error'});
  delta('setMMapZeroCopy', loader, stream, [true], {value: undefined}, {throws: 'Error'});
} finally {
  loader.delete();
  store.delete();
  stream.delete();
}

// ---------------------------------------------------------------- matrix sync
const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
const claimed = matrix.filter(row => row.behaviorTest === TEST_NAME);
assert.deepEqual(claimed.map(row => row.legacyMethod).sort(), [...exercised.keys()].sort(),
  'every method exercised here must carry this behaviorTest in the parity matrix, and vice versa');
for (const row of claimed) {
  const kind = exercised.get(row.legacyMethod);
  if (row.parityStatus === 'behavior_verified') {
    assert.equal(Boolean(row.edgeDifference), kind === 'delta',
      `${row.legacyMethod}: edgeDifference must be documented exactly when a delta is pinned`);
  } else {
    assert.equal(row.parityStatus, 'known_behavior_gap', row.legacyMethod);
    assert.equal(kind, 'delta', `${row.legacyMethod}: a known gap must pin its delta`);
  }
}
const gaps = claimed.filter(row => row.parityStatus === 'known_behavior_gap').length;
console.log(`ok - asset cache/streaming behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length - gaps} methods verified against legacy, ${gaps} documented gaps`);
