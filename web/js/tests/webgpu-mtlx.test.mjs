// SPDX-License-Identifier: Apache-2.0
import test from 'node:test';
import assert from 'node:assert/strict';
import { compileGraph, literal, GraphError } from '../src/webgpu-mtlx/graph.js';
import { packScene, syntheticScene } from '../src/webgpu-mtlx/scene.js';
import { encodeEXR } from '../src/webgpu-mtlx/capture.js';
const constant = (name, value, type = 'float') => ({ name, category: 'constant', type, inputs: { value: { type, value } } });
test('finite typed literals cannot inject shader code', () => {
  assert.equal(literal('color3', [1, 0.25, 0]), 'vec3f(1.0,0.25,0.0)');
  for (const v of [NaN, Infinity, '1.0); return;', [1, 2]]) assert.throws(() => literal('float', v), GraphError);
  assert.throws(() => literal('integer', 1.5)); assert.throws(() => literal('boolean', 'yes'));
});
test('shared dependency emitted once and deterministic', () => {
  const doc = { nodes: [constant('a', 2), { name: 'sum', category: 'add', type: 'float', inputs: { in1: { nodename: 'a' }, in2: { nodename: 'a' } } }] };
  const a = compileGraph(doc); assert.equal(a.body.split('\n').length, 2); assert.deepEqual(a, compileGraph(doc));
});
test('cycle, missing node, mismatch, duplicate, unknown operation fail', () => {
  assert.throws(() => compileGraph({ nodes: [{ name: 'x', category: 'absval', type: 'float', inputs: { in: { nodename: 'x' } } }] }), /cycle/);
  assert.throws(() => compileGraph({ nodes: [{ name: 'x', category: 'absval', type: 'float', inputs: { in: { nodename: 'missing' } } }] }), /unknown node/);
  assert.throws(() => compileGraph({ nodes: [constant('x', [1, 2, 3], 'color3'), { name: 'y', category: 'absval', type: 'float', inputs: { in: { nodename: 'x' } } }] }), /expected float/);
  assert.throws(() => compileGraph({ nodes: [constant('x', 1), constant('x', 2)] }), /x/);
  assert.throws(() => compileGraph({ nodes: [{ name: 'x', category: 'arbitrary', type: 'float' }] }), /not implemented/);
});
test('compound NodeDef inputs bind separately for each instance', () => {
  const doc = { definitions: { ND_double: { name: 'ND_double', node: 'double', outputs: { out: { type: 'float' } }, inputs: { x: { type: 'float', value: 2 } } } }, graphs: { impl: { name: 'impl', nodedef: 'ND_double', inputs: {}, outputs: { out: { nodename: 'sum' } }, nodes: [{ name: 'sum', category: 'add', type: 'float', inputs: { in1: { interfacename: 'x' }, in2: { interfacename: 'x' } } }] } }, nodes: [
    { name: 'a', category: 'double', type: 'float', nodedef: 'ND_double', inputs: { x: { type: 'float', value: 3 } } },
    { name: 'b', category: 'double', type: 'float', nodedef: 'ND_double', inputs: { x: { type: 'float', value: 4 } } },
    { name: 'out', category: 'add', type: 'float', inputs: { in1: { nodename: 'a' }, in2: { nodename: 'b' } } },
  ] };
  const compiled = compileGraph(doc); assert.match(compiled.body, /3.0 \+ 3.0/); assert.match(compiled.body, /4.0 \+ 4.0/);
});
test('graph output cycle fails', () => {
  assert.throws(() => compileGraph({ nodes: [], graphs: { g: { name: 'g', nodes: [], outputs: { out: { nodegraph: 'g', output: 'out' } } } } }, { output: { nodegraph: 'g', output: 'out' } }), /cycle/);
});
test('prototype names do not resolve as definitions or graphs', () => {
  assert.throws(() => compileGraph({ nodes: [{ name: 'x', nodedef: 'constructor', category: 'constant', type: 'float' }] }), /unknown NodeDef/);
  assert.throws(() => compileGraph({ nodes: [] }, { output: { nodegraph: '__proto__' } }), /unknown graph/);
});
test('unsupported physical features never silently become opaque defaults', () => {
  const doc = syntheticScene().materials[1]; doc.nodes[0].inputs.transmission = { type: 'float', value: 1 };
  assert.throws(() => compileGraph(doc, { material: true }), /transmission transport/);
  delete doc.nodes[0].inputs.transmission; doc.nodes[0].inputs.subsurface = { type: 'float', value: 0.5 };
  assert.throws(() => compileGraph(doc, { material: true }), /not yet implemented/);
});
test('BVH escape links progress, leaves cover every triangle exactly once', () => {
  const scene = syntheticScene('graph'), packed = packScene(scene); let leaves = 0;
  for (let i = 0; i < packed.nodeCount; i++) {
    const n = packed.nodeData.subarray(i * 12, i * 12 + 12);
    assert.ok(n[8] > i && n[8] <= packed.nodeCount); leaves += n[7];
    for (let k = 0; k < 3; k++) assert.ok(n[k] <= n[k + 4]);
  }
  assert.equal(leaves, scene.indices.length / 3);
  assert.equal(packed.triangleData.length, leaves * 36);
});
test('scene resource limits and indices validated before GPU allocation', () => {
  const scene = syntheticScene(); assert.throws(() => packScene(scene, { maxTriangles: 1 }), /budget/);
  scene.indices[0] = -1; assert.throws(() => packScene(scene), /index/);
});
test('EXR float roundtrip through independent Three.js decoder', async () => {
  const { EXRLoader } = await import('three/addons/loaders/EXRLoader.js');
  const { FloatType } = await import('three');
  const pixels = new Float32Array([0.25, 2, 32, 1, 0, 0.5, 1, 1]);
  const encoded = encodeEXR(2, 1, pixels); assert.equal(new DataView(encoded.buffer).getInt32(0, true), 20000630);
  const decoded = new EXRLoader().setDataType(FloatType).parse(encoded.buffer);
  assert.equal(decoded.width, 2); assert.equal(decoded.height, 1); assert.deepEqual([...decoded.data], [...pixels]);
});
