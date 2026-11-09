// SPDX-License-Identifier: Apache-2.0
// Run against an isolated next-only WASM build: node this-file path/to/lightusd_next.js native-import.usda
import assert from 'node:assert/strict';
import path from 'node:path';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
const {default: factory} = await import(pathToFileURL(path.resolve(process.argv[2])).href);
const module = await factory();
const writer = new module.NextUSDZConverterNative();
const encoder = new TextEncoder();
const obj = encoder.encode('v 0 0 0\nv 1 0 0\nv 0 1 0\nvw 0 0 .25 1 .75\nvw 1 1 2\nvw 2 0 -.5 1 1.5\nf 1 2 3\nl 1 2\np 3\n');
try {
  const parsed = module.usddiff({left:{data:obj, name:'skin.obj'}, right:{data:obj, name:'skin.obj'}});
  assert.ok(parsed.success, parsed.error); assert.equal(parsed.hasDiffs, false);
  const changed = encoder.encode(new TextDecoder().decode(obj).replace('0 .25', '0 .5'));
  assert.equal(module.usddiff({left:{data:obj,name:'skin.obj'}, right:{data:changed,name:'skin.obj'}}).hasDiffs, true);
  const fixture = fs.readFileSync(process.argv[3]);
  const equivalent = module.usddiff({left:{data:obj,name:'skin.obj'}, right:{data:fixture,name:'skin.usda'}, compareMetadata:false, format:'json'});
  assert.ok(equivalent.success, equivalent.error); assert.deepEqual(JSON.parse(equivalent.json).property_diffs, {});
  assert.ok(writer.loadFromBinary(fixture, 'skin.usda'), writer.error());
  const text = writer.exportAsUSDA();
  for (const value of ['SkelBindingAPI', 'primvars:skel:jointWeights', 'BasisCurves', 'Points', 'elementSize = 2']) assert.ok(text.includes(value), value);
  for (const [method, ext] of [['exportAsUSDC', 'usdc'], ['exportAsUSDZ', 'usdz']]) {
    const data = writer[method]();
    const reader = new module.NextUSDZConverterNative();
    try {
      assert.ok(reader.loadFromBinary(data, 'skin.' + ext), reader.error());
      assert.ok(reader.exportAsUSDA().includes('primvars:skel:jointWeights'));
    } finally { reader.delete(); }
  }
  const payload = {sourceFormat:'mjcf', upAxis:'Z', links:[{name:'a'}, {name:'b'}],
    joints:[{name:'j', type:'revolute', parent:'a', child:'b', axis:'X', lower:-1, upper:1,
      dynamics:{damping:.1, friction:.2}}]};
  assert.ok(writer.createURDFPhysicsScene(JSON.stringify(payload)), writer.error());
  assert.ok(writer.exportAsUSDA().includes('PhysicsRevoluteJoint'));
  assert.equal(writer.createURDFPhysicsScene(JSON.stringify({links:[{}], materials:[0,{}]})), false);
  assert.match(writer.error(), /array of objects/);
  console.log('WASM OBJ skin/mixed primitives, binary/package roundtrip and physics: PASS');
} finally { writer.delete(); }
