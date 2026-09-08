// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import createModule from '../src/lightusd/lightusd_next.js';

const module = await createModule();
const encode = text => new TextEncoder().encode(text);
assert.equal(module._lightusd_next_create(0), 0);
assert.equal(module._lightusd_next_create(99), 0);
const stale = module._lightusd_next_create(4) >>> 0;
assert.ok(stale);
module._lightusd_next_destroy(stale);
// Reuse and then retire a generation slot. An old handle stays invalid.
for (let i = 0; i < 4200; ++i) {
  const handle = module._lightusd_next_create(4) >>> 0;
  assert.ok(handle);
  assert.equal(module._lightusd_next_call(stale, 40, 2), 0);
  assert.equal(module._lightusd_next_call(handle, 15, 2), 0); // wrong class
  module._lightusd_next_destroy(handle);
}
module._lightusd_next_destroy(stale); // stale disposal is harmless

const stream = new module.RenderStream();
assert.equal(stream.meshCount(), 0);
assert.equal(stream.numMeshes(), 0);
assert.throws(() => stream.getMesh(), /argument count/);
assert.throws(() => stream.getMesh('0'), /expected number/);
assert.throws(() => stream.setTangentMethod(0), /expected string/);
const converter = new module.NextUSDZConverterNative();
assert.throws(() => stream.meshCount.call(converter), /receiver/);
stream.delete();
assert.equal(stream.isDeleted(), true);
assert.throws(() => stream.meshCount(), /receiver/);
assert.throws(() => stream.delete(), /already deleted/);

const text = `#usda 1.0
def Cube "Box" {
  double size = 2
  custom int64 signedValue = -9223372036854775808
  custom uint64 unsignedValue = 18446744073709551615
  custom float[] samples = [1.25, 2.5]
  custom double[] nonfinite = [nan, inf, -inf]
  custom token[] labels = ["a", "b"]
  custom string label = "quote: \\"; unicode: 日本語"
  matrix4d xformOp:transform = ((1,0,0,0),(0,1,0,0),(0,0,1,0),(2,3,4,1))
}
`;
assert.equal(converter.loadFromBinary(encode(text), 'scene.usda'), true, converter.error());
const json = converter.extractPhysicsSceneJSON();
assert.match(json, /18446744073709551615/);
assert.match(json, /-9223372036854775808/);
const prim = JSON.parse(json).prims.find(p => p.name === 'Box');
assert.ok(prim);
assert.deepEqual(prim.properties.samples, [1.25, 2.5]);
assert.deepEqual(prim.properties.nonfinite, [null, null, null]);
assert.deepEqual(prim.properties.labels, ['a', 'b']);
assert.equal(prim.properties.label, 'quote: "; unicode: 日本語');
assert.equal(prim.matrix.length, 16);
assert.deepEqual(prim.matrix.slice(12), [2, 3, 4, 1]);
assert.equal(prim.geometry.type, 'box');
assert.equal(prim.geometry.size, 2);

for (const options of ['{}', '{bad', '{"groups":[]}',
    '{"groups":["shade"],"groups":["core"]}']) {
  const result = JSON.parse(module.validateFromBinary(encode(text), 'scene.usda', options));
  assert.equal(result.parse_ok, true);
  assert.ok(Array.isArray(result.issues));
  assert.ok(result.checked_groups.includes('core'));
}
const bad = JSON.parse(module.validateFromBinary(encode('not USD'), 'bad.usda', '{}'));
assert.equal(bad.parse_ok, false);
assert.equal(bad.ok, false);
assert.equal(typeof bad.error, 'string');
converter.delete();
console.log('ok - next C dispatch lifetime, validation and JSON semantics');
