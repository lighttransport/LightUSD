// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const classification = new Map(read('lightusd-loader-api-classification.json')
  .map(row => [row.name, row.family]));
const session = new Set(read('core-flatten-session-api-inventory.json'));
const map = read('core-flatten-parity-map.json');
const expected = legacy.filter(name => classification.get(name) === 'flatten');
assert.deepEqual(map.map(row => row.legacyMethod), expected,
  'flatten workflow map must cover the entire classified legacy family');
for (const row of map) {
  assert.equal(row.coreSurface, 'FlattenSession');
  // Rows promoted by a paired behavior test mirror the matrix status.
  assert.ok(['workflow_covered_behavior_review_required', 'behavior_verified'].includes(row.parityStatus));
  assert.ok(Array.isArray(row.coreWorkflow) && row.coreWorkflow.length > 0,
    `empty flatten workflow mapping for ${row.legacyMethod}`);
  for (const operation of row.coreWorkflow) {
    assert.ok(session.has(operation),
      `${row.legacyMethod} maps to missing FlattenSession.${operation}`);
  }
}
for (const name of ['flattenBufferToSink', 'flattenBufferToSinkRemap',
  'flattenBufferToSinkRemapVariants', 'flattenMultiBufferToSink',
  'flattenMultiBufferToSinkFetch', 'flattenMultiBufferToSinkFetchRemap',
  'flattenMultiBufferToSinkFetchRemapVariants']) {
  const row = map.find(item => item.legacyMethod === name);
  assert.ok(row.coreWorkflow.includes('step'), `${name} must map to streaming step output`);
}
console.log(`ok - flatten parity map covers ${map.length} legacy operations across the FlattenSession API`);
