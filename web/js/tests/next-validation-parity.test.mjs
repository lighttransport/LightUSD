// Compare the legacy loader validation method with the next module-level API.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

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
const legacy = new legacyModule.LightUSDLoaderNative();
const functionInventory = JSON.parse(fs.readFileSync(new URL(
  './next-module-function-api-inventory.json', import.meta.url), 'utf8'));
for (const name of functionInventory) assert.equal(typeof nextModule[name], 'function');
const cases = [
  '../../../tests/usda/validation/valid/clean.usda',
  '../../../tests/usda/validation/invalid/multi-violation.usda',
  '../../../tests/usda/validation/invalid/primvar-reader-bad-result.usda',
  '../../../tests/usdc/cube-000.usdc'
];
const options = JSON.stringify({groups: ['core', 'geom', 'shade', 'lux', 'physics', 'crate']});
let nextLayer = null;
try {
  assert.equal(typeof nextModule.validateFromBinary, 'function');
  assert.equal(typeof nextModule.LayerDocument, 'function');
  nextLayer = new nextModule.LayerDocument();
  assert.equal(typeof nextLayer.validateLoadedLayer, 'function');
  assert.deepEqual(JSON.parse(nextLayer.validateLoadedLayer('{}')), {
    parse_ok: false, ok: false,
    error: 'No Layer is loaded. Use loadAsLayerFromBinary first.'
  });
  assert.throws(() => nextLayer.validateLoadedLayer(), TypeError);
  assert.throws(() => nextLayer.validateLoadedLayer({}), TypeError);
  for (const fixture of cases) {
    const bytes = new Uint8Array(fs.readFileSync(new URL(fixture, import.meta.url)));
    const filename = path.basename(fixture);
    const expected = JSON.parse(legacy.validateFromBinary(bytes, filename, options));
    const actual = JSON.parse(nextModule.validateFromBinary(bytes, filename, options));
    if (fixture.endsWith('cube-000.usdc')) {
      const normalizedExpected = {...expected};
      assert.match(actual.warn, /Threading is disabled for this build\./,
        'next USDC validation must preserve the legacy CrateReader warning text');
      assert.match(expected.warn, /Threading is disabled/);
      delete actual.warn;
      delete normalizedExpected.warn;
      assert.deepEqual(actual, normalizedExpected,
        'next USDC validation currently omits the crate checked-group');
    } else {
      assert.deepEqual(actual, expected, `validation result differs for ${fixture}`);
    }
  }
  const usdcBytes = new Uint8Array(fs.readFileSync(new URL(cases[3], import.meta.url)));
  const legacyCoreOnly = JSON.parse(
    legacy.validateFromBinary(usdcBytes, 'cube-000.usdc', '{}'));
  const nextCoreOnly = JSON.parse(
    nextModule.validateFromBinary(usdcBytes, 'cube-000.usdc', '{}'));
  assert.match(nextCoreOnly.warn, /Threading is disabled for this build\./);
  delete legacyCoreOnly.warn;
  delete nextCoreOnly.warn;
  assert.deepEqual(nextCoreOnly, legacyCoreOnly,
    'the legacy core group also enables crate checks when options omit explicit groups');
  const cleanBytes = new Uint8Array(fs.readFileSync(new URL(cases[0], import.meta.url)));
  legacy.reset();
  assert.equal(legacy.loadAsLayerFromBinary(cleanBytes, 'clean.usda'), true, legacy.error());
  assert.equal(nextLayer.load(cleanBytes).success, true, nextLayer.error());
  const alloc = nextModule._lightusd_next_alloc;
  const validationAllocations = [];
  nextModule._lightusd_next_alloc = size => {
    validationAllocations.push(Number(size));
    return alloc(size);
  };
  let nextLoadedResult;
  try {
    nextLoadedResult = JSON.parse(nextLayer.validateLoadedLayer(options));
  } finally {
    nextModule._lightusd_next_alloc = alloc;
  }
  assert.ok(validationAllocations.length > 0 &&
    validationAllocations.every(size => size < 1024),
  'loaded-layer validation must not stage a full USDA copy in WASM memory');
  assert.deepEqual(nextLoadedResult,
    JSON.parse(legacy.validateLoadedLayer(options)),
    'loaded-layer validation must preserve the legacy result for authored USDA');
  assert.deepEqual(nextLayer.definePrim('/ValidationAdded', 'Xform'),
    {success: true});
  const afterEdit = JSON.parse(nextLayer.validateLoadedLayer(options));
  assert.equal(afterEdit.parse_ok, true);
  assert.equal(afterEdit.ok, true,
    'validation must use the document current state after edits');
  const badBytes = new TextEncoder().encode('not USD');
  const nextBad = JSON.parse(nextModule.validateFromBinary(badBytes, 'bad.usda', '{}'));
  const legacyBad = JSON.parse(legacy.validateFromBinary(badBytes, 'bad.usda', '{}'));
  assert.equal(nextBad.parse_ok, false);
  assert.equal(legacyBad.parse_ok, false);
  assert.equal(nextBad.ok, false);
  assert.equal(legacyBad.ok, false);
  assert.ok(nextBad.error && legacyBad.error);
  assert.notEqual(nextBad.error, legacyBad.error,
    'parser diagnostic wording is a known legacy/next difference');
} finally {
  nextLayer?.delete();
  legacy.delete();
}
const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
const claimed = matrix.filter(row => row.behaviorTest === 'next-validation-parity.test.mjs');
assert.deepEqual(claimed.map(row => [row.legacyMethod, row.parityStatus]), [
  ['validateFromBinary', 'known_behavior_gap'], ['validateLoadedLayer', 'behavior_verified'],
], 'validation rows must record this paired test and its verified/known-gap outcome');
console.log(`ok - legacy/next validateFromBinary correspondence and known deltas verified (${wasm64 ? 'wasm64' : 'wasm32'})`);
