// Map legacy mesh ownership APIs to the core bounded-copy and view surfaces.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const render = new Set(read('renderstream-api-inventory.json'));
const map = read('core-mesh-access-parity-map.json');
assert.deepEqual(map.map(row => row.legacyMethod), ['getMeshCopy', 'getMeshPtr']);
for (const row of map) {
  assert.ok(legacy.includes(row.legacyMethod));
  assert.ok(render.has(row.coreEquivalent),
    `${row.legacyMethod}: missing RenderStream.${row.coreEquivalent}`);
  // Rows reviewed by a paired behavior test mirror the matrix status.
  assert.ok(['mapped_behavior_review_required', 'behavior_verified', 'known_behavior_gap']
    .includes(row.parityStatus));
  assert.ok(row.ownership.length > 0);
  const matrix = read('core-wasm-parity-gaps.json');
  const matrixRow = matrix.find(item => item.legacyMethod === row.legacyMethod);
  assert.equal(matrixRow?.coreSurface, row.coreSurface);
  assert.equal(matrixRow?.coreEquivalent, row.coreEquivalent);
  assert.equal(matrixRow?.parityStatus, row.parityStatus);
}
console.log(`ok - mesh ownership parity map covers ${map.length} owned/borrowed mesh APIs`);
