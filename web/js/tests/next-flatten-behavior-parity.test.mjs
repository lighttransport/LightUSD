// SPDX-License-Identifier: Apache-2.0
// Paired behavior for the combined module's next-flatten API (legacy
// LightUSDLoaderNative.nextFlatten*) and the next-only NextFlattenSession.
// Both run the same next flatten pipeline, so the flattened Crate bytes must
// be identical for every workflow: single USDC buffers, sinks, remap and
// variant overrides, multi-buffer fetch, and the async session protocol.
// Rows naming this test in next-wasm-parity-gaps.json must match what it
// exercises.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-flatten-behavior-parity.test.mjs';
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

const exercised = new Map();  // method -> 'match' | 'delta'
const note = (method, kind) => {
  if (exercised.get(method) !== 'delta') exercised.set(method, kind);
};

const singleUSDA = encode(`#usda 1.0
def Xform "A" (
 variants = {
  string shape = "round"
 }
 prepend variantSets = "shape"
) {
 asset tex = @old.png@
 variantSet "shape" = {
  "round" {
   def Sphere "S" {
   }
  }
  "square" {
   def Cube "C" {
   }
  }
 }
}
`);
const rootUSDA = encode(`#usda 1.0\ndef Xform "Inst" (\n references = @ref.usda@</Asset>\n) {\n asset tex = @old.png@\n}\n`);
const depUSDA = encode(`#usda 1.0\ndef Xform "Asset" {\n float weight = 2\n}\n`);
function toUSDC(bytes) {
  const document = new nextModule.LayerDocument();
  try {
    assert.equal(document.load(bytes).success, true);
    const crate = document.exportUSDC();
    assert.equal(crate.success, true);
    return new Uint8Array(crate.data);
  } finally { document.delete(); }
}
const singleUSDC = toUSDC(singleUSDA);
const remap = {'old.png': 'textures/new.png'};
const variants = {shape: 'square'};
const concat = chunks => {
  const out = new Uint8Array(chunks.reduce((sum, chunk) => sum + chunk.length, 0));
  let offset = 0;
  for (const chunk of chunks) { out.set(chunk, offset); offset += chunk.length; }
  return out;
};

// Next-only reference output for one configuration.
function nextFlatten(root, {remapping, overrides, sink} = {}) {
  const session = new nextModule.NextFlattenSession();
  try {
    if (overrides) for (const [set, selection] of Object.entries(overrides))
      session.setVariantOverride(set, selection);
    assert.equal(session.begin(root, 'root.usda', true).success, true);
    if (remapping) session.setAssetPathRemap(remapping);
    const chunks = [];
    for (let i = 0; i < 8; ++i) {
      const step = session.step(sink ? chunk => { chunks.push(chunk.slice()); return true; } : null);
      assert.equal(step.success, true, step.error);
      if (step.status === 'need-layer') {
        assert.equal(step.key, 'ref.usda');
        session.provideLayer(step.key, depUSDA);
        continue;
      }
      return sink ? concat(chunks) : new Uint8Array(step.data);
    }
    throw new Error('flatten did not converge');
  } finally { session.end(); session.delete(); }
}

const loader = new legacyModule.LightUSDLoaderNative();
const buffer = (name, bytes) => {
  const allocation = loader.allocateZeroCopyBuffer(name, bytes.length, 0);
  assert.equal(allocation.success, true);
  legacyModule.HEAPU8.set(bytes, Number(allocation.bufferPtr));
  assert.equal(loader.markZeroCopyBytesWritten(allocation.uuid, bytes.length), true);
  return allocation.uuid;
};
const outputOf = result => {
  assert.equal(result.success, true, result.error);
  return new Uint8Array(result.data);
};
try {
  // Single USDC buffers. The legacy entry points require a Crate root; next
  // sessions also accept USDA roots.
  assert.deepEqual(outputOf(loader.nextFlattenUSDC(singleUSDC, true)), nextFlatten(singleUSDC));
  assert.equal(loader.nextFlattenUSDC(singleUSDA, true).success, false);
  const layerText = bytes => {
    const document = new nextModule.LayerDocument();
    try { assert.equal(document.load(bytes).success, true); return document.exportUSDA().text; }
    finally { document.delete(); }
  };
  assert.equal(layerText(nextFlatten(singleUSDA)), layerText(nextFlatten(singleUSDC)),
    'a USDA root flattens to the same layer as its Crate encoding');
  exercised.set('nextFlattenUSDC', 'delta');
  assert.deepEqual(outputOf(loader.nextFlattenBuffer(buffer('a.usdc', singleUSDC), true)),
    nextFlatten(singleUSDC));
  assert.equal(loader.nextFlattenBuffer(buffer('a2.usda', singleUSDA), true).success, false);
  exercised.set('nextFlattenBuffer', 'delta');
  assert.deepEqual(outputOf(loader.nextFlattenBufferRemap(buffer('b.usdc', singleUSDC), true, remap)),
    nextFlatten(singleUSDC, {remapping: remap}));
  note('nextFlattenBufferRemap', 'match');
  const withVariants = nextFlatten(singleUSDC, {remapping: remap, overrides: variants});
  assert.notDeepEqual(withVariants, nextFlatten(singleUSDC, {remapping: remap}),
    'the variant override changes the flattened output');
  assert.deepEqual(outputOf(loader.nextFlattenBufferRemapVariants(buffer('c.usdc', singleUSDC),
    true, remap, variants)), withVariants);
  note('nextFlattenBufferRemapVariants', 'match');

  // Streaming sinks.
  const sinkRun = (method, ...extra) => {
    const chunks = [];
    const result = loader[method](buffer(`${method}.usdc`, singleUSDC), true,
      chunk => { chunks.push(chunk.slice()); return true; }, ...extra);
    assert.equal(result.success, true, result.error);
    return concat(chunks);
  };
  assert.deepEqual(sinkRun('nextFlattenBufferToSink'), nextFlatten(singleUSDC, {sink: true}));
  note('nextFlattenBufferToSink', 'match');
  assert.deepEqual(sinkRun('nextFlattenBufferToSinkRemap', remap),
    nextFlatten(singleUSDC, {sink: true, remapping: remap}));
  note('nextFlattenBufferToSinkRemap', 'match');
  assert.deepEqual(sinkRun('nextFlattenBufferToSinkRemapVariants', remap, variants),
    nextFlatten(singleUSDC, {sink: true, remapping: remap, overrides: variants}));
  note('nextFlattenBufferToSinkRemapVariants', 'match');

  // Multi-buffer flatten with dependency fetch callbacks.
  const expectedMulti = nextFlatten(rootUSDA, {sink: true});
  for (const [method, extra, options] of [
    ['nextFlattenMultiBufferToSink', [], {}],
    ['nextFlattenMultiBufferToSinkFetch', [], {}],
    ['nextFlattenMultiBufferToSinkFetchRemap', [remap], {remapping: remap}],
    ['nextFlattenMultiBufferToSinkFetchRemapVariants', [remap, {}], {remapping: remap}]]) {
    const chunks = [];
    const fetched = [];
    const result = loader[method](buffer(`${method}.usda`, rootUSDA), 'root.usda', true,
      chunk => { chunks.push(chunk.slice()); return true; },
      key => key === 'ref.usda', key => { fetched.push(key); return depUSDA; }, ...extra);
    assert.equal(result.success, true, `${method}: ${result.error}`);
    assert.deepEqual(fetched, ['ref.usda'], method);
    assert.deepEqual(concat(chunks), Object.keys(options).length
      ? nextFlatten(rootUSDA, {sink: true, ...options}) : expectedMulti, method);
    note(method, 'match');
  }

  // Async session protocol: begin -> step (need-layer) -> provide -> step -> end.
  for (const [method, extra, options] of [['nextFlattenAsyncBegin', [], {}],
    ['nextFlattenAsyncBeginRemap', [remap], {remapping: remap}],
    ['nextFlattenAsyncBeginRemapVariants', [remap, {}], {remapping: remap}]]) {
    const begun = loader[method](buffer(`${method}.usda`, rootUSDA), 'root.usda', true, ...extra);
    assert.equal(begun.success, true, begun.error);
    const chunks = [];
    const sink = chunk => { chunks.push(chunk.slice()); return true; };
    let step = loader.nextFlattenAsyncStep(begun.session, sink);
    assert.deepEqual([step.status, step.key], ['need-layer', 'ref.usda']);
    assert.equal(loader.nextFlattenAsyncProvideLayer(begun.session, step.key, depUSDA).success, true);
    step = loader.nextFlattenAsyncStep(begun.session, sink);
    assert.equal(step.status, 'done');
    assert.equal(loader.nextFlattenAsyncEnd(begun.session).success, true);
    assert.deepEqual(concat(chunks), nextFlatten(rootUSDA, {sink: true, ...options}), method);
    note(method, 'match');
  }
  for (const method of ['nextFlattenAsyncStep', 'nextFlattenAsyncProvideLayer', 'nextFlattenAsyncEnd'])
    note(method, 'match');
} finally { loader.delete(); }

// ---------------------------------------------------------- matrix sync
const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
const claimed = matrix.filter(row => row.behaviorTest === TEST_NAME);
assert.deepEqual(claimed.map(row => row.legacyMethod).sort(), [...exercised.keys()].sort(),
  'every method exercised here must carry this behaviorTest in the parity matrix, and vice versa');
for (const row of claimed) {
  assert.equal(row.parityStatus, 'behavior_verified', row.legacyMethod);
  assert.equal(Boolean(row.edgeDifference), exercised.get(row.legacyMethod) === 'delta',
    `${row.legacyMethod}: edgeDifference must be documented exactly when a delta is pinned`);
}
console.log(`ok - next flatten behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length} methods byte-identical to NextFlattenSession`);
