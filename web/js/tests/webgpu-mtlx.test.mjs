// SPDX-License-Identifier: Apache-2.0
import test from 'node:test';
import assert from 'node:assert/strict';
import { compileGraph, literal, GraphError } from '../src/webgpu-mtlx/graph.js';
import { packScene, syntheticScene } from '../src/webgpu-mtlx/scene.js';
import { encodeEXR } from '../src/webgpu-mtlx/capture.js';
import { packImages } from '../src/webgpu-mtlx/textures.js';
import { shaderSource } from '../src/webgpu-mtlx/shaders.js';
import { validateSpectrum, sampleSpectrum } from '../src/webgpu-mtlx/spectrum.js';
import { cieXYZ } from '../src/webgpu-mtlx/cie-data.js';
import { refineDisplacementScene } from '../src/webgpu-mtlx/displacement.js';
const constant = (name, value, type = 'float') => ({ name, category: 'constant', type, inputs: { value: { type, value } } });
test('native closures compile and unsupported uniform inputs are diagnosed',()=>{
  for(const preset of ['native-copper','native-glass']) {
    const resources={};const source=shaderSource(syntheticScene(preset).materials,resources);
    assert.equal(resources.requiresPhysical,true);assert.match(source,/surfaceEmission/);
  }
  const doc=syntheticScene('native-glass').materials[1];
  doc.nodes[0].inputs.scatter_mode={nodename:'dynamic_mode'};
  assert.throws(()=>compileGraph(doc,{material:true}),/connected scatter_mode/);
  doc.nodes[0].inputs.scatter_mode={value:'RT'};
  doc.nodes[0].inputs.thinfilm_thickness={type:'float',value:100};
  assert.throws(()=>compileGraph(doc,{material:true}),/thin film/);
  const volume=syntheticScene('sss').materials[1];volume.mediumMajorant=-1;
  assert.throws(()=>shaderSource([volume]),/majorant/);
});
test('displacement refinement preserves bounds, typed indices and material assignment',()=>{
  const scene={positions:new Float32Array([0,0,0,1,0,0,0,1,0]),indices:new Uint32Array([0,1,2]),materials:[{}]};
  const r=refineDisplacementScene(scene,2);assert.equal(r.indices.length,48);assert.equal(r.materialIds.length,16);assert.ok(r.positions.every(v=>v>=0&&v<=1));
  assert.throws(()=>refineDisplacementScene(scene,5,1),/budget/);
});
test('CIE table agrees with official column-sum validation', () => {
  assert.equal(cieXYZ.length,471);
  [106.865469489595,106.8569171011719,106.892251278636].forEach((expected,k)=>assert.ok(Math.abs(cieXYZ.reduce((sum,row)=>sum+row[k],0)-expected)<1e-9));
});
test('measured spectra validate ordering, coverage and interpolate linearly', () => {
  const p=validateSpectrum([[360,1.6],[830,1.4]],{ior:true});assert.equal(sampleSpectrum(p,595),1.5);assert.equal(sampleSpectrum(p,350),0);
  assert.throws(()=>validateSpectrum([[400,1.5],[700,1.4]],{ior:true}),/cover/);
  assert.throws(()=>validateSpectrum([[400,1],[400,2]]),/unordered/);
  assert.throws(()=>validateSpectrum([[400,1],[700,-1]]),/Invalid/);
});
test('image packing linearizes before mip filtering and preserves alpha', () => {
  const packed = packImages([{ width: 3, height: 1, colorspace: 'srgb_texture', data: [0,0,0,.2, .5,.5,.5,.4, 1,1,1,.6] }]);
  assert.deepEqual(packed.descriptors, [{ offset: 0, width: 3, height: 1, levels: 2 }]);
  assert.ok(Math.abs(packed.data[4] - .21404114) < 1e-6);
  assert.ok(Math.abs(packed.data[12] - (1 + .21404114) / 3) < 1e-6);
  assert.ok(Math.abs(packed.data[15] - .4) < 1e-6);
});
test('image budgets, finite float32, dimensions and color interpretation are validated', () => {
  const image = { width: 1, height: 1, data: [0,0,0,1] };
  assert.throws(() => packImages([image], { maxBytes: 15 }), /budget/);
  assert.throws(() => packImages([{ ...image, width: .5 }]), /dimensions/);
  assert.throws(() => packImages([{ ...image, data: [1e100,0,0,1] }]), /non-finite/);
  assert.throws(() => packImages([{ ...image, colorspace: 'unknown' }]), /colorspace/);
});
test('image graph resource resolution and unsupported filtering diagnostics', () => {
  const doc = syntheticScene().materials[1];
  const image = { name: 'image', category: 'image', type: 'color3', inputs: { file: { type: 'filename', value: 'test' } } };
  doc.nodes[0].inputs.base_color = { nodename: 'image' }; doc.nodes.unshift(image);
  assert.throws(() => shaderSource([doc]), /missing decoded image/);
  doc.images = { test: { width: 1, height: 1, data: [1,0,0,1] } };
  const resources = {}; assert.match(shaderSource([doc], resources), /imageSample\(0u/); assert.equal(resources.imageData.length, 4);
  image.inputs.filtertype = { value: 'cubic' }; assert.throws(() => shaderSource([doc]), /closest\/linear/);
  image.inputs.file.value = ''; assert.match(shaderSource([doc]), /vec3f\(0.0,0.0,0.0\)/);
});
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
  assert.match(compileGraph(doc, { material: true }).body, /Material/);
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

test('spectral EXR declares XYZ channels and preserves component order',()=>{
  const bytes=encodeEXR(1,1,new Float32Array([.25,2,32,1]),{channels:['X','Y','Z']}),view=new DataView(bytes.buffer);let offset=8;
  const z=()=>{const start=offset;while(bytes[offset]!==0)offset++;return new TextDecoder().decode(bytes.subarray(start,offset++));};
  let channels=[];
  while(bytes[offset]!==0){const name=z();z();const size=view.getUint32(offset,true);offset+=4;const end=offset+size;if(name==='channels'){while(bytes[offset]!==0){channels.push(z());assert.equal(view.getUint32(offset,true),2);offset+=16;}}offset=end;}
  offset++;assert.deepEqual(channels,['X','Y','Z']);const row=Number(view.getBigUint64(offset,true));assert.equal(view.getUint32(row+4,true),12);
  assert.deepEqual([0,1,2].map(i=>view.getFloat32(row+8+i*4,true)),[.25,2,32]);
});
