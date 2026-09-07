// SPDX-License-Identifier: Apache-2.0
import { compileGraph, contextWGSL, parseMaterialX } from './graph.js';
export async function validateValueKernels(device) {
  const value = v => ({ type: 'float', value: v });
  const binary = (category, a, b, expected) => ({ category, inputs: { in1: value(a), in2: value(b) }, expected });
  const unary = (category, x, expected) => ({ category, inputs: { in: value(x) }, expected });
  const cases = [binary('add', 2, 3, 5), binary('subtract', 2, 3, -1), binary('multiply', 2, 3, 6), binary('divide', 2, 4, 0.5), binary('modulo', -1.5, 1, 0.5), binary('power', 2, 3, 8), binary('min', -1, 2, -1), binary('max', -1, 2, 2),
    unary('absval', -2, 2), unary('sign', -2, -1), unary('floor', -1.2, -2), unary('ceil', -1.2, -1), unary('sqrt', 4, 2), unary('ln', 1, 0), unary('exp', 0, 1), unary('sin', 0.5, Math.sin(0.5)), unary('cos', 0.5, Math.cos(0.5)), unary('tan', 0.5, Math.tan(0.5)), unary('asin', 0.5, Math.asin(0.5)), unary('acos', 0.5, Math.acos(0.5)),
    { category: 'mix', inputs: { fg: value(10), bg: value(2), mix: value(0.25) }, expected: 4 },
    { category: 'smoothstep', inputs: { in: value(0.25), low: value(0), high: value(1) }, expected: 0.15625 },
    { category: 'clamp', inputs: { in: value(5), low: value(1), high: value(3) }, expected: 3 },
    { category: 'remap', inputs: { in: value(0.5), inlow: value(0), inhigh: value(1), outlow: value(10), outhigh: value(20) }, expected: 15 },
    { category: 'dotproduct', inputs: { in1: { type: 'vector3', value: [1, 2, 3] }, in2: { type: 'vector3', value: [3, 2, 1] } }, expected: 10 },
    { category: 'magnitude', inputs: { in: { type: 'vector3', value: [3, 4, 0] } }, expected: 5 },
    { category: 'ifequal', inputs: { value1: value(1), value2: value(1), in1: value(7), in2: value(9) }, expected: 7 },
    {category:'range',inputs:{in:value(-4),gamma:value(2)},expected:-2},
    {category:'range',inputs:{in:value(-4),gamma:value(2),doclamp:{type:'boolean',value:true}},expected:0},
    {category:'range',type:'color3',component:2,inputs:{in:{type:'color3',value:[1,4,9]},gamma:value(2)},expected:3},
    {category:'add',type:'vector3',component:1,inputs:{in1:{type:'vector3',value:[1,2,3]},in2:value(4)},expected:6},
    {category:'combine2',type:'color4',component:3,inputs:{in1:{type:'color3',value:[1,2,3]},in2:value(.7)},expected:.7},
    {category:'convert',type:'color4',component:3,inputs:{in:{type:'vector2',value:[.2,.4]}},expected:1},
    {category:'convert',type:'vector2',component:1,inputs:{in:{type:'color4',value:[.2,.4,.6,.8]}},expected:.4},
    {category:'transformmatrix',type:'vector2',component:0,inputs:{in:{type:'vector2',value:[2,3]},mat:{type:'matrix33',value:[1,0,0,0,1,0,5,6,1]}},expected:7},
    {category:'normalmap',type:'vector3',component:2,inputs:{in:{type:'vector3',value:[.5,.5,1]}},expected:1},
    {category:'normalmap',type:'vector3',component:0,inputs:{in:{type:'vector3',value:[1,.5,1]},scale:{type:'vector2',value:[2,1]}},expected:2/Math.sqrt(5)},
  ];
  const bodies = cases.map((c, i) => {
    const g = compileGraph({ nodes: [{ name: 'test', type: c.type||'float', category: c.category, inputs: c.inputs }] });
    return `{ ${g.body}\nresult[${i}] = ${g.expression}${c.component===undefined?'':`[${c.component}]`}; }`;
  });
  const module = device.createShaderModule({ code: `${contextWGSL}\n@group(0) @binding(0) var<storage,read_write> result: array<f32>; @compute @workgroup_size(1) fn main() { let ctx=ShadingContext(vec3f(0),vec3f(0,0,1),vec3f(1,0,0),vec3f(0,1,0),vec2f(0),0,0,vec2f(0),vec2f(0)); ${bodies.join('\n')} }` });
  const info = await module.getCompilationInfo();
  if (info.messages.some(m => m.type === 'error')) throw new Error(info.messages.map(m => m.message).join('\n'));
  const pipeline = await device.createComputePipelineAsync({ layout: 'auto', compute: { module, entryPoint: 'main' } });
  const output = device.createBuffer({ size: cases.length * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
  const readback = device.createBuffer({ size: cases.length * 4, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  try {
    const group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: output } }] });
    const encoder = device.createCommandEncoder(); const pass = encoder.beginComputePass(); pass.setPipeline(pipeline); pass.setBindGroup(0, group); pass.dispatchWorkgroups(1); pass.end(); encoder.copyBufferToBuffer(output, 0, readback, 0, output.size); device.queue.submit([encoder.finish()]);
    await readback.mapAsync(GPUMapMode.READ); const actual = new Float32Array(readback.getMappedRange());
    const results = cases.map((c, i) => ({ category: c.category, expected: c.expected, actual: actual[i], pass: Number.isFinite(actual[i]) && Math.abs(actual[i] - c.expected) <= 1e-5 + 1e-4 * Math.abs(c.expected) }));
    if (results.some(r => !r.pass)) throw new Error(`GPU numeric mismatch: ${JSON.stringify(results.filter(r => !r.pass))}`);
    // Exercise actual XML parsing, a connected graph and entity rejection in Chrome.
    const doc = parseMaterialX('<materialx version="1.39"><constant name="a" type="float"><input name="value" type="float" value="2"/></constant><add name="b" type="float"><input name="in1" type="float" nodename="a"/><input name="in2" type="float" value="3"/></add></materialx>');
    compileGraph(doc);
    let rejected = false; try { parseMaterialX('<!DOCTYPE materialx><materialx/>'); } catch { rejected = true; }
    if (!rejected) throw new Error('DTD was not rejected');
    return results;
  } finally { output.destroy(); readback.destroy(); }
}
