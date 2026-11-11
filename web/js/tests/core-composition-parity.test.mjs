// Keep legacy composition operations mapped to exercised core workflows.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const classification = new Map(read('lightusd-loader-api-classification.json')
  .map(row => [row.name, row.family]));
const core = new Set(read('core-flatten-session-api-inventory.json'));
const map = read('core-composition-parity-map.json');
const expected = legacy.filter(name => classification.get(name) === 'composition_and_variants' &&
  /^(applyVariantSelection|compose(Inherits|Payload|References|Sublayers|Variants))$/.test(name))
  .sort();
assert.deepEqual(map.map(row => row.legacyMethod), expected,
  'composition workflow map must cover every classified composition method');
for (const row of map) {
  assert.equal(row.coreSurface, 'FlattenSession');
  // Rows promoted by a paired behavior test mirror the matrix status.
  assert.ok(['workflow_covered_behavior_review_required', 'behavior_verified'].includes(row.parityStatus));
  assert.ok(row.workflow.length > 0);
  for (const method of row.workflow) {
    assert.ok(core.has(method), `${row.legacyMethod}: missing workflow operation ${method}`);
  }
}
const matrix = read('core-wasm-parity-gaps.json');
for (const row of map) {
  const matrixRow = matrix.find(item => item.legacyMethod === row.legacyMethod);
  assert.equal(matrixRow?.coreSurface, row.coreSurface);
  assert.equal(matrixRow?.parityStatus, row.parityStatus);
}
console.log(`ok - composition parity map covers ${map.length} legacy operations via FlattenSession workflows`);
