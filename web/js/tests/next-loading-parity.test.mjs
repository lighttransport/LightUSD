// Map compatible legacy binary loading calls to next document/render workflows.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const render = new Set(read('renderstream-api-inventory.json'));
const layer = new Set(read('next-layer-document-api-inventory.json'));
const assets = new Set(read('next-asset-store-api-inventory.json'));
const map = read('next-loading-parity-map.json');
const expected = ['loadAsLayerFromBinary', 'loadAsLayerFromCachedAsset',
  'loadFromBinary', 'loadFromBinaryAsync', 'loadFromBinaryWithProgress',
  'loadFromCachedAsset'].sort();
assert.ok(expected.every(name => legacy.includes(name)));
assert.deepEqual(map.map(row => row.legacyMethod), expected,
  'loading map must cover the selected compatible legacy binary load APIs');
const matrix = read('next-wasm-parity-gaps.json');
for (const row of map) {
  const targets = row.nextSurface === 'RenderStream' ? render
    : row.nextSurface === 'NextLayerDocument' ? layer : null;
  assert.ok(targets, `${row.legacyMethod}: invalid next surface`);
  assert.ok(targets.has(row.nextEquivalent),
    `${row.legacyMethod}: missing next workflow operation ${row.nextEquivalent}`);
  // Rows promoted by a paired behavior test mirror the matrix status.
  assert.ok(['workflow_covered_behavior_review_required', 'behavior_verified'].includes(row.parityStatus));
  const matrixRow = matrix.find(item => item.legacyMethod === row.legacyMethod);
  assert.equal(matrixRow?.nextSurface, row.nextSurface);
  assert.equal(matrixRow?.nextEquivalent, row.nextEquivalent);
  assert.deepEqual(matrixRow?.workflow, row.workflow);
  assert.equal(matrixRow?.parityStatus, row.parityStatus);
  if (row.workflow) {
    for (const step of row.workflow) {
      const inventory = step.surface === 'NextAssetStore' ? assets
        : step.surface === 'NextLayerDocument' ? layer
        : step.surface === 'RenderStream' ? render : null;
      assert.ok(inventory?.has(step.method),
        `${row.legacyMethod}: missing workflow step ${step.surface}.${step.method}`);
    }
  }
}
console.log(`ok - loading parity map covers ${map.length} legacy binary-load operations via next document/render workflows`);
