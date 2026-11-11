// Map compatible legacy binary loading calls to core document/render workflows.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const render = new Set(read('renderstream-api-inventory.json'));
const layer = new Set(read('core-layer-document-api-inventory.json'));
const assets = new Set(read('core-asset-store-api-inventory.json'));
const map = read('core-loading-parity-map.json');
const expected = ['loadAsLayerFromBinary', 'loadAsLayerFromCachedAsset',
  'loadFromBinary', 'loadFromBinaryAsync', 'loadFromBinaryWithProgress',
  'loadFromCachedAsset'].sort();
assert.ok(expected.every(name => legacy.includes(name)));
assert.deepEqual(map.map(row => row.legacyMethod), expected,
  'loading map must cover the selected compatible legacy binary load APIs');
const matrix = read('core-wasm-parity-gaps.json');
for (const row of map) {
  const targets = row.coreSurface === 'RenderStream' ? render
    : row.coreSurface === 'LayerDocument' ? layer : null;
  assert.ok(targets, `${row.legacyMethod}: invalid core surface`);
  assert.ok(targets.has(row.coreEquivalent),
    `${row.legacyMethod}: missing core workflow operation ${row.coreEquivalent}`);
  // Rows promoted by a paired behavior test mirror the matrix status.
  assert.ok(['workflow_covered_behavior_review_required', 'behavior_verified'].includes(row.parityStatus));
  const matrixRow = matrix.find(item => item.legacyMethod === row.legacyMethod);
  assert.equal(matrixRow?.coreSurface, row.coreSurface);
  assert.equal(matrixRow?.coreEquivalent, row.coreEquivalent);
  assert.deepEqual(matrixRow?.workflow, row.workflow);
  assert.equal(matrixRow?.parityStatus, row.parityStatus);
  if (row.workflow) {
    for (const step of row.workflow) {
      const inventory = step.surface === 'AssetStore' ? assets
        : step.surface === 'LayerDocument' ? layer
        : step.surface === 'RenderStream' ? render : null;
      assert.ok(inventory?.has(step.method),
        `${row.legacyMethod}: missing workflow step ${step.surface}.${step.method}`);
    }
  }
}
console.log(`ok - loading parity map covers ${map.length} legacy binary-load operations via core document/render workflows`);
