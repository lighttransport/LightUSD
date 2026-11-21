// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const classification = new Map(read('lightusd-loader-api-classification.json')
  .map(row => [row.name, row.family]));
const core = new Set(read('core-asset-store-api-inventory.json'));
const matrix = read('core-asset-store-parity-gaps.json');
const combined = new Map(read('core-wasm-parity-gaps.json').map(row => [row.legacyMethod, row]));
const expected = legacy.filter(name => classification.get(name) === 'asset_resolution_and_cache');
assert.deepEqual(matrix.map(row => row.legacyMethod), expected,
  'asset-store parity matrix must cover the classified legacy asset API');
for (const row of matrix) {
  assert.equal(row.coreSurface, 'AssetStore');
  assert.equal(row.coreNameMatch, core.has(row.legacyMethod),
    `AssetStore inventory mismatch for ${row.legacyMethod}`);
  // Paired behavior results live in the combined matrix; mirror them here.
  const reviewed = combined.get(row.legacyMethod);
  assert.equal(row.parityStatus, reviewed?.behaviorTest ? reviewed.parityStatus
    : row.coreNameMatch ? 'name_match_behavior_review_required'
    : 'missing_next_asset_store_method');
}
const missing = matrix.filter(row => !row.coreNameMatch);
console.log(`ok - AssetStore parity matrix covers ${matrix.length} asset methods; ${missing.length} lack a same-name method`);
