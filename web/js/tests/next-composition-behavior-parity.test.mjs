// SPDX-License-Identifier: Apache-2.0
// Paired legacy/next behavior for the composition family. Legacy composes one
// arc kind at a time in place on its loaded layer (composeSublayers, ...);
// next composes every arc in a NextFlattenSession. Single-arc fixtures make
// the two comparable: the resulting layers must hold the same prims, types,
// specifiers and property values. Arc/variant queries on the authored root
// layer must return identical results. Rows naming this test in
// next-wasm-parity-gaps.json must match what it exercises.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-composition-behavior-parity.test.mjs';
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

const mesh = name => `def Mesh "${name}" {
 int[] faceVertexCounts = [3]
 int[] faceVertexIndices = [0,1,2]
 point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
}`;
const variantRoot = `#usda 1.0
def Xform "V" (
 variants = {
  string shape = "round"
 }
 prepend variantSets = "shape"
) {
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
`;
const lodRoot = `#usda 1.0
def Xform "Asset" (
 variants = {
  string LOD = "LOD0"
 }
 prepend variantSets = "LOD"
) {
 variantSet "LOD" = {
  "LOD0" {
   def Cube "Hi" {
   }
  }
  "LOD1" {
   def Cube "Mid" {
   }
  }
  "LOD2" {
   def Cube "Lo" {
   }
  }
 }
}
`;
const cases = {
  sublayers: {compose: 'composeSublayers',
    root: `#usda 1.0\n(\n subLayers = [@sub.usda@]\n)\ndef Xform "Local" {\n}\n`,
    deps: {'sub.usda': `#usda 1.0\n${mesh('FromSub')}\n`}},
  references: {compose: 'composeReferences',
    root: `#usda 1.0\ndef Xform "Inst" (\n references = @ref.usda@</Asset>\n) {\n}\n`,
    deps: {'ref.usda': `#usda 1.0\ndef Xform "Asset" {\n float weight = 2\n ${mesh('Geo')}\n}\n`}},
  payload: {compose: 'composePayload',
    root: `#usda 1.0\ndef Xform "Inst" (\n payload = @pay.usda@</Asset>\n) {\n}\n`,
    deps: {'pay.usda': `#usda 1.0\ndef Xform "Asset" {\n float weight = 3\n ${mesh('Geo')}\n}\n`}},
  inherits: {compose: 'composeInherits',
    root: `#usda 1.0\nclass Xform "Base" {\n float x = 1\n}\ndef Xform "A" (\n inherits = </Base>\n) {\n}\n`,
    deps: {}},
  variants: {compose: 'composeVariants', root: variantRoot, deps: {}},
  lod: {root: lodRoot, deps: {}},
};

// Layer JSON -> {path: {type, specifier, properties}} (order-insensitive).
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
function legacyLayer(c) {
  const loader = new legacyModule.LightUSDLoaderNative();
  for (const [name, text] of Object.entries(c.deps)) loader.setAsset(name, encode(text));
  assert.equal(loader.loadAsLayerFromBinary(encode(c.root), 'root.usda'), true, loader.error());
  return loader;
}
function nextFlatten(c, overrides = []) {
  const session = new nextModule.NextFlattenSession();
  try {
    // Overrides set before begin() are session configuration.
    for (const [key, selection] of overrides) session.setVariantOverride(key, selection);
    assert.equal(session.begin(encode(c.root), 'root.usda', true).success, true);
    for (let i = 0; i < 32; ++i) {
      const step = session.step(null);
      assert.equal(step.success, true, step.error);
      if (step.status === 'need-layer') {
        assert.ok(c.deps[step.key], `unexpected dependency ${step.key}`);
        session.provideLayer(step.key, encode(c.deps[step.key]));
        continue;
      }
      const document = new nextModule.LayerDocument();
      try {
        assert.equal(document.load(new Uint8Array(step.data)).success, true);
        return prims(JSON.parse(document.exportJSON().text));
      } finally { document.delete(); }
    }
    throw new Error('flatten did not converge');
  } finally { session.end(); session.delete(); }
}

// Per-arc composition: legacy's in-place compose equals next's flatten.
for (const [name, c] of Object.entries(cases)) {
  if (!c.compose) continue;
  const loader = legacyLayer(c);
  try {
    assert.equal(loader[c.compose](), true, `${c.compose}: ${loader.error()}`);
    assert.deepEqual(nextFlatten(c), prims(JSON.parse(loader.layerToJSON())),
      `${name}: ${c.compose} must match next composition`);
    note(c.compose, 'match');
  } finally { loader.delete(); }
}

// Variant selection. The three-argument form maps to a prim-scoped override
// ("/V{shape}"); the one-argument form forces a selection on every variant
// set, which next expresses per set name ("LOD").
{
  const loader = legacyLayer(cases.variants);
  try {
    assert.equal(loader.applyVariantSelection('/V', 'shape', 'square'), true);
    const expected = prims(JSON.parse(loader.layerToJSON()));
    assert.ok(expected['/V/C'] && !expected['/V/S']);
    assert.deepEqual(nextFlatten(cases.variants, [['/V{shape}', 'square']]), expected);
    assert.deepEqual(nextFlatten(cases.variants, [['shape', 'square']]), expected);
  } finally { loader.delete(); }
  const lodLoader = legacyLayer(cases.lod);
  try {
    assert.equal(lodLoader.applyVariantSelection('LOD2'), true);
    const expected = prims(JSON.parse(lodLoader.layerToJSON()));
    assert.ok(expected['/Asset/Lo'] && !expected['/Asset/Hi']);
    assert.deepEqual(nextFlatten(cases.lod, [['LOD', 'LOD2']]), expected);
  } finally { lodLoader.delete(); }
  exercised.set('applyVariantSelection', 'delta');
}

// Arc and variant queries on the authored root layer (legacy layer load vs
// next RenderStream after in-place composition, dependencies supplied).
const queries = ['hasSublayers', 'hasReferences', 'hasPayload', 'hasInherits', 'hasVariants',
  'lodVariantCount', 'extractSublayerAssetPaths', 'extractReferencesAssetPaths',
  'extractPayloadAssetPaths', 'extractVariants'];
for (const [name, c] of Object.entries(cases)) {
  const loader = legacyLayer(c);
  const store = new nextModule.NextAssetStore();
  const stream = new nextModule.RenderStream();
  try {
    for (const [dep, text] of Object.entries(c.deps)) store.setAsset(dep, encode(text));
    stream.setAssetStore(store);
    assert.equal(stream.begin(encode(c.root), 'root.usda').success, true, stream.error());
    for (const method of queries) {
      assert.deepEqual(stream[method](), loader[method](), `${name}: ${method}`);
      note(method, 'match');
    }
  } finally { loader.delete(); stream.delete(); store.delete(); }
}

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
console.log(`ok - composition behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length} methods verified against legacy`);
