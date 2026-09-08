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
  // Fixed double-precision values from the pinned GLSL xy -> XYZ -> Rec.709
  // equations; include all polynomial branches, HDR channels and clamp edges.
  const blackbodyCases = [
    [0,[1.496339391,.904075611,.489343155]],
    [1000,[2.867483188,.548988133,0]],
    [2200,[2.312416446,.704255010,.065786727]],
    [3000,[1.766762683,.845570184,.272628244]],
    [4000,[1.414415168,.923947437,.533722653]],
    [5000,[1.212318392,.960889191,.762853288]],
    [6500,[1.042633029,.983863471,1.034904717]],
    [40000,[.727261487,.987542194,1.927041689]],
  ];
  for (const [v,expected] of [[0,0],[.25,.25],[.5,.5],[1,0],[-1.25,.25]]) cases.push(unary('trianglewave',v,expected));
  cases.push({category:'plus',inputs:{fg:value(2),bg:value(3),mix:value(.25)},expected:3.5});
  cases.push({category:'minus',inputs:{fg:value(2),bg:value(3),mix:value(.25)},expected:2.5});
  for (let component=0;component<4;component++) {
    cases.push({category:'colorcorrect',type:'color4',component,
      inputs:{in:{type:'color4',value:[.25,.25,.25,.37]},gamma:value(2),lift:value(.2),gain:value(2),contrast:value(.5),exposure:value(1)},
      expected:component===3?.37:1.7});
  }
  for (const [temperature, rgb] of blackbodyCases) for (let component=0;component<3;component++) {
    cases.push({category:'blackbody',type:'color3',component,inputs:{temperature:value(temperature)},expected:rgb[component]});
  }
  for(const [label,uv1,uv2,expected] of [
    ['standard',[1,0],[0,1],[1,0,0,0,1,0]],
    ['rotated/mirrored',[0,1],[1,0],[0,1,0,1,0,0]],
    ['mirrored U',[-1,0],[0,1],[-1,0,0,0,1,0]],
    ['degenerate',[0,0],[0,0],[1,0,0,0,1,0]],
    ['small UVs',[.00001,0],[0,.00001],[1,0,0,0,1,0]],
  ]) for(let i=0;i<6;i++)cases.push({category:`UV frame ${label}`,expected:expected[i],
    expression:`mxSurfaceFrame(vec3f(0,0,1),vec3f(2,0,0),vec3f(0,3,0),vec2f(${uv1}),vec2f(${uv2}))[${Math.floor(i/3)}][${i%3}]`});
  for(let component=0;component<3;component++){
    cases.push({category:'heighttonormal constant',component,type:'vector3',expected:[.5,.5,1][component],nodes:[
      {name:'h',category:'heighttonormal',type:'vector3',inputs:{in:value(.7)}}]});
    for(const [label,uvScale,scale,normal] of [
      ['default',[1,1],1,[-1/Math.sqrt(257),0,16/Math.sqrt(257)]],
      ['scaled',[2,3],16,[-1/Math.sqrt(5),0,2/Math.sqrt(5)]],
      ['mirrored',[-1,1],16,[Math.SQRT1_2,0,Math.SQRT1_2]],
      ['degenerate',[0,0],16,[0,0,1]],
    ])cases.push({category:`heighttonormal ${label}`,component,type:'vector3',expected:.5+.5*normal[component],nodes:[
      {name:'uv',category:'texcoord',type:'vector2'},
      {name:'height',category:'extract',type:'float',inputs:{in:{nodename:'uv'},index:{type:'integer',value:0}}},
      {name:'mapped',category:'multiply',type:'vector2',inputs:{in1:{nodename:'uv'},in2:{type:'vector2',value:uvScale}}},
      {name:'h',category:'heighttonormal',type:'vector3',inputs:{in:{nodename:'height'},scale:value(scale),texcoord:{nodename:'mapped'}}},
    ]});
    cases.push({category:'bump constant',component,type:'vector3',expected:[0,0,1][component],nodes:[
      {name:'b',category:'bump',type:'vector3',inputs:{height:value(.7)}}]});
    cases.push({category:'bump procedural',component,type:'vector3',expected:[-1/Math.sqrt(257),0,16/Math.sqrt(257)][component],nodes:[
      {name:'uv',category:'texcoord',type:'vector2'},
      {name:'height',category:'extract',type:'float',inputs:{in:{nodename:'uv'},index:{type:'integer',value:0}}},
      {name:'b',category:'bump',type:'vector3',inputs:{height:{nodename:'height'}}}]});
  }
  for(const [roughness,anisotropy,expected] of [
    [0,0,[1e-8,1e-8]],[-.5,0,[.25,.25]],[2,0,[1,1]],
    [.5,-1,[.25,.25]],[.5,.75,[.5,.125]],[.5,1,[1,.25*Math.sqrt(.02)]],
  ])for(let component=0;component<2;component++){
    cases.push({category:'roughness_anisotropy',type:'vector2',component,inputs:{roughness:value(roughness),anisotropy:value(anisotropy)},expected:expected[component],absoluteTolerance:1e-10});
    cases.push({category:'glossiness_anisotropy',type:'vector2',component,inputs:{glossiness:value(1-roughness),anisotropy:value(anisotropy)},expected:expected[component],absoluteTolerance:1e-10});
  }
  for(const [edge,ior,extinction] of [[0,3,0],[.5,1.8,Math.sqrt(1.76)],[1,.6,.8],[2,-1.8,0],[-1,5.4,0]]){
    for(const [output,expected] of [['ior',ior],['extinction',extinction]])cases.push({category:`artistic_ior ${output} edge ${edge}`,type:'color3',component:0,expected,
      nodes:[{name:'metal',category:'artistic_ior',type:'multioutput',outputs:{ior:{type:'color3'},extinction:{type:'color3'}},inputs:{reflectivity:{type:'color3',value:[.25,.25,.25]},edge_color:{type:'color3',value:[edge,edge,edge]}}}],output:{nodename:'metal',output}});
  }
  for(const input of [[0,0,0],[2,-3,4]])for(let component=0;component<3;component++)cases.push({category:'transformnormal',type:'vector3',component,inputs:{in:{type:'vector3',value:input}},expected:input[component]});
  cases.push({category:'transformnormal',type:'vector3',component:2,inputs:{},expected:1});
  const bodies = cases.map((c, i) => {
    // Graphs below exercise re-evaluation at shifted contexts, not only the
    // normal reconstruction helper.
    if(c.expression)return `result[${i}] = ${c.expression};`;
    const g = compileGraph({ nodes: c.nodes || [{ name: 'test', type: c.type||'float', category: c.category, inputs: c.inputs }] },{output:c.output});
    return `{ ${g.body}\nresult[${i}] = ${g.expression}${c.component===undefined?'':`[${c.component}]`}; }`;
  });
  const module = device.createShaderModule({ code: `${contextWGSL}\n@group(0) @binding(0) var<storage,read_write> result: array<f32>; @compute @workgroup_size(1) fn main() { let ctx=ShadingContext(vec3f(0),vec3f(0,0,1),vec3f(1,0,0),vec3f(0,1,0),vec2f(0),0,0,vec2f(0),vec2f(0),vec3f(1,0,0),vec3f(0,1,0),vec3f(0,0,1),vec4f(0,0,0,1),vec4f(0),vec4f(0),vec4f(0),vec4f(0),vec4f(0),vec4f(0),vec4f(0),vec4f(0)); ${bodies.join('\n')} }` });
  const info = await module.getCompilationInfo();
  if (info.messages.some(m => m.type === 'error')) throw new Error(info.messages.map(m => m.message).join('\n'));
  const pipeline = await device.createComputePipelineAsync({ layout: 'auto', compute: { module, entryPoint: 'main' } });
  const output = device.createBuffer({ size: cases.length * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
  const readback = device.createBuffer({ size: cases.length * 4, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  try {
    const group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: output } }] });
    const encoder = device.createCommandEncoder(); const pass = encoder.beginComputePass(); pass.setPipeline(pipeline); pass.setBindGroup(0, group); pass.dispatchWorkgroups(1); pass.end(); encoder.copyBufferToBuffer(output, 0, readback, 0, output.size); device.queue.submit([encoder.finish()]);
    await readback.mapAsync(GPUMapMode.READ); const actual = new Float32Array(readback.getMappedRange());
    const results = cases.map((c, i) => ({ category: c.category, expected: c.expected, actual: actual[i], pass: Number.isFinite(actual[i]) && Math.abs(actual[i] - c.expected) <= (c.absoluteTolerance??1e-5) + 1e-4 * Math.abs(c.expected) }));
    if (results.some(r => !r.pass)) throw new Error(`GPU numeric mismatch: ${JSON.stringify(results.filter(r => !r.pass))}`);
    // Exercise actual XML parsing, a connected graph and entity rejection in Chrome.
    const doc = parseMaterialX('<materialx version="1.39"><constant name="a" type="float"><input name="value" type="float" value="2"/></constant><add name="b" type="float"><input name="in1" type="float" nodename="a"/><input name="in2" type="float" value="3"/></add></materialx>');
    compileGraph(doc);
    let rejected = false; try { parseMaterialX('<!DOCTYPE materialx><materialx/>'); } catch { rejected = true; }
    if (!rejected) throw new Error('DTD was not rejected');
    return results;
  } finally { output.destroy(); readback.destroy(); }
}
