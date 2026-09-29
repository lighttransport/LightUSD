// SPDX-License-Identifier: Apache-2.0
// Paired legacy/next behavior for the loading workflows: render-scene loads
// (binary, async, with progress, cached asset) against RenderStream, and
// layer loads (binary, with progress, cached asset, loadTest, JSON) against
// NextLayerDocument. A curated fixture table pins which inputs load on each
// product: the shared corpus must agree, next's untrusted input policy
// (strict AOUSD grammar, fatal missing composition assets) and legacy-only
// parse failures are pinned per fixture. Rows naming this test in
// next-wasm-parity-gaps.json must match what it exercises.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-loading-behavior-parity.test.mjs';
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

// [fixture, legacy loads, next loads]. Shared successes span USDA, USDC and
// USDZ with meshes, skinning, lights, materials, variants and relocates.
const renderCases = [
  ['tests/usda/cube-000.usda', true, true],
  ['tests/usda/skelanimation-full-001.usda', true, true],
  ['tests/usda/lights-full-001.usda', true, true],
  ['tests/usda/shader.usda', true, true],
  ['tests/usda/references-001.usda', true, true],
  ['tests/usda/relocates_basic.usda', true, true],
  ['tests/usda/blendshape-001.usda', true, true],
  ['tests/usdc/cube-000.usdc', true, true],
  ['tests/usdc/variantSet-000.usdc', true, true],
  ['models/cube.usdz', true, true],
  ['models/texture-cat-plane.usdz', true, true],
  // Unregistered stage/prim metadata is preserved (AOUSD layer grammar).
  ['tests/usda/aousd-unknown-property-metadata.usda', true, true],
  // Untrusted policy: a missing sublayer asset fails composition. Legacy
  // never composes on load and accepts it.
  ['tests/usda/sublayers-001.usda', true, false],
  // Legacy parser/converter failures that next loads.
  ['tests/usda/geommesh-001.usda', false, true],
  ['tests/usda/simple-mesh-point.usda', false, true],
];
for (const [name, legacyOk, nextOk] of renderCases) {
  const bytes = fixture(name);
  const file = name.split('/').pop();
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    const legacyResult = loader.loadFromBinary(bytes, file);
    const nextResult = stream.begin(bytes, file);
    assert.equal(typeof legacyResult, 'boolean');
    assert.equal(legacyResult, legacyOk, `legacy loadFromBinary ${name}: ${loader.error()}`);
    assert.equal(nextResult.success, nextOk, `next begin ${name}: ${stream.error()}`);
    assert.equal(stream.ok(), nextOk);
    if (nextOk) assert.equal(nextResult.meshCount, stream.numMeshes());
    else assert.equal(nextResult.error, stream.error());
  } finally { loader.delete(); stream.delete(); }
}
exercised.set('loadFromBinary', 'delta');

// Async: both resolve after the call returns; legacy reports a small record,
// next the full begin() record.
{
  const bytes = fixture('tests/usda/cube-000.usda');
  const broken = encode('#usda 1.0\ndef Xform "A" {\n float x = \n');
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    const legacyPromise = loader.loadFromBinaryAsync(bytes, 'cube.usda');
    const nextPromise = stream.beginAsync(bytes, 'cube.usda');
    assert.ok(legacyPromise instanceof Promise && nextPromise instanceof Promise);
    const [legacyResult, nextResult] = await Promise.all([legacyPromise, nextPromise]);
    assert.deepEqual(Object.keys(legacyResult).sort(),
      ['materialCount', 'meshCount', 'success', 'textureCount']);
    assert.equal(nextResult.success, legacyResult.success);
    assert.equal(nextResult.meshCount, legacyResult.meshCount);
    assert.equal(stream.numMaterials(), legacyResult.materialCount);
    assert.equal(stream.numTextures(), legacyResult.textureCount);
    const [legacyBad, nextBad] = await Promise.all([
      loader.loadFromBinaryAsync(broken, 'bad.usda'), stream.beginAsync(broken, 'bad.usda')]);
    assert.equal(nextBad.success, legacyBad.success);
    assert.equal(nextBad.success, false);
    assert.ok(legacyBad.error && nextBad.error);
  } finally { loader.delete(); stream.delete(); }
  exercised.set('loadFromBinaryAsync', 'delta');
}

// With progress: legacy updates its polled record synchronously; next
// reports phases to the progress callback during begin().
{
  const bytes = fixture('tests/usda/skelanimation-full-001.usda');
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    const phases = [];
    stream.setProgressCallback(event => { phases.push(event.phase); return true; });
    assert.equal(loader.loadFromBinaryWithProgress(bytes, 'skel.usda'), true);
    assert.equal(stream.begin(bytes, 'skel.usda').success, true);
    assert.equal(stream.numMeshes(), loader.numMeshes());
    assert.equal(loader.getProgress().progress, 1);
    assert.equal(stream.getProgress().progress, 1);
    assert.ok(phases.length > 0 && phases.at(-1) === 'complete', phases.join(','));
  } finally { loader.delete(); stream.delete(); }
  exercised.set('loadFromBinaryWithProgress', 'delta');
}

// Cached assets: legacy loads from its own setAsset cache; next from a
// NextAssetStore passed to beginCachedAsset / read into LayerDocument.load.
{
  const bytes = fixture('tests/usda/cube-000.usda');
  const loader = new legacyModule.LightUSDLoaderNative();
  const store = new nextModule.NextAssetStore();
  const stream = new nextModule.RenderStream();
  const document = new nextModule.LayerDocument();
  try {
    loader.setAsset('root.usda', bytes);
    store.setAsset('root.usda', bytes);
    assert.equal(loader.loadFromCachedAsset('root.usda'), true);
    assert.equal(stream.beginCachedAsset(store, 'root.usda').success, true);
    assert.equal(stream.numMeshes(), loader.numMeshes());
    assert.equal(loader.loadFromCachedAsset('missing.usda'), false);
    assert.equal(stream.beginCachedAsset(store, 'missing.usda').success, false);
    assert.match(loader.error(), /^Asset not found in cache/);
    assert.match(stream.error(), /^Asset not found in cache/);
    note('loadFromCachedAsset', 'match');

    assert.equal(loader.loadAsLayerFromCachedAsset('root.usda'), true);
    assert.equal(document.load(store.getAsset('root.usda').data).success, true);
    assert.equal(loader.loadAsLayerFromCachedAsset('missing.usda'), false);
    assert.deepEqual(store.getAsset('missing.usda'), {});
    note('loadAsLayerFromCachedAsset', 'match');
  } finally { loader.delete(); store.delete(); stream.delete(); document.delete(); }
}

// Layer loads: legacy returns a boolean, next a {success, primCount} record.
const layerCases = [
  ['tests/usda/cube-000.usda', true, true],
  ['tests/usda/references-001.usda', true, true],
  ['tests/usda/relocates_basic.usda', true, true],
  ['tests/usda/sublayers-001.usda', true, true],
  ['tests/usdc/cube-000.usdc', true, true],
  ['tests/usdc/variantSet-000.usdc', true, true],
  ['tests/usda/aousd-unknown-property-metadata.usda', true, true],
  // Strict AOUSD parsing under the untrusted policy.
  ['tests/usdc/refs-customdata-001.usdc', true, false],
];
for (const [name, legacyOk, nextOk] of layerCases) {
  const bytes = fixture(name);
  const file = name.split('/').pop();
  const loader = new legacyModule.LightUSDLoaderNative();
  const plain = new nextModule.LayerDocument();
  const progress = new nextModule.LayerDocument();
  try {
    assert.equal(loader.loadAsLayerFromBinary(bytes, file), legacyOk, `legacy layer ${name}`);
    const loaded = plain.load(bytes);
    assert.equal(loaded.success, nextOk, `next layer ${name}: ${loaded.error}`);
    if (nextOk) assert.ok(Number.isInteger(loaded.primCount));
    assert.equal(loader.loadAsLayerFromBinaryWithProgress(bytes, file), legacyOk);
    let callbacks = 0;
    assert.equal(progress.loadWithProgress(bytes, () => { ++callbacks; return true; }).success, nextOk);
    if (nextOk) assert.ok(callbacks > 0);
    assert.equal(loader.loadTest(file, bytes), legacyOk, `legacy loadTest ${name}`);
  } finally { loader.delete(); plain.delete(); progress.delete(); }
}
exercised.set('loadAsLayerFromBinary', 'delta');
exercised.set('loadAsLayerFromBinaryWithProgress', 'delta');
exercised.set('loadTest', 'delta');

// Layer JSON: next imports legacy-exported JSON and rejects malformed input.
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const document = new nextModule.LayerDocument();
  const reloaded = new legacyModule.LightUSDLoaderNative();
  try {
    assert.equal(loader.loadAsLayerFromBinary(fixture('tests/usda/references-001.usda'), 'r.usda'), true);
    const json = loader.layerToJSON();
    assert.equal(reloaded.loadLayerFromJSON(json), true, reloaded.error());
    assert.equal(document.loadJSON(json).success, true);
    assert.equal(reloaded.loadLayerFromJSON('not json'), false);
    assert.equal(document.loadJSON('not json').success, false);
    // Legacy accepts a root whose typeName is not "Layer"; next rejects it.
    assert.equal(reloaded.loadLayerFromJSON('{"typeName": "Mesh"}'), true);
    assert.equal(document.loadJSON('{"typeName": "Mesh"}').success, false);
  } finally { loader.delete(); document.delete(); reloaded.delete(); }
  exercised.set('loadLayerFromJSON', 'delta');
}

// ---------------------------------------------------------- matrix sync
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
console.log(`ok - loading behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length - gaps} methods verified against legacy, ${gaps} documented gaps`);
