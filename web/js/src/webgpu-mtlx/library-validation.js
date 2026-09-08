// SPDX-License-Identifier: Apache-2.0
import { parseMaterialX, compileGraph, contextWGSL } from './graph.js';
import { fetchResource } from './resources.js';

/** Execute actual pinned graph implementations, rather than cataloguing names. */
export async function validateLibraryGraphs(device) {
  const library={definitions:Object.create(null),graphs:Object.create(null)};
  for(const file of ['stdlib/stdlib_defs.mtlx','stdlib/stdlib_ng.mtlx','cmlib/cmlib_defs.mtlx','cmlib/cmlib_ng.mtlx']) {
    const source=`/__mtlx/libraries/${file}`;
    const doc=parseMaterialX(new TextDecoder().decode(await fetchResource(source)),{source});
    Object.assign(library.definitions,doc.definitions);Object.assign(library.graphs,doc.graphs);
  }
  const cases=[
    {name:'ND_range_color3FA',category:'range',type:'color3',inputs:{in:{type:'color3',value:[-4,.25,9]},gamma:{type:'float',value:2}},expected:[-2,.5,3,1]},
    {name:'ND_acescg_to_lin_rec709_color3',category:'acescg_to_lin_rec709',type:'color3',inputs:{in:{type:'color3',value:[1,0,0]}},expected:[1.705050992658,-.130256417507,-.024003356805,1]},
    {name:'ND_convert_vector2_color4',category:'convert',type:'color4',inputs:{in:{type:'vector2',value:[2,3]}},expected:[2,3,0,1]},
    {name:'ND_normalmap_vector2',category:'normalmap',type:'vector3',inputs:{in:{type:'vector3',value:[1,.5,1]},scale:{type:'vector2',value:[2,1]}},expected:[2/Math.sqrt(5),0,1/Math.sqrt(5),1]},
  ];
  const bodies=cases.map((c,i)=>{
    const graph=compileGraph({nodes:[{name:'test',nodedef:c.name,...c}]},{library});
    return `{${graph.body}\nresult[${i}]=${c.type==='color4'?graph.expression:`vec4f(${graph.expression},1)`};}`;
  });
  const code=`${contextWGSL}\n@group(0) @binding(0) var<storage,read_write> result:array<vec4f>;
  @compute @workgroup_size(1) fn main(){let ctx=ShadingContext(vec3f(0),vec3f(0,0,1),vec3f(1,0,0),vec3f(0,1,0),vec2f(0),0,0,vec2f(0),vec2f(0),vec3f(1,0,0),vec3f(0,1,0));${bodies.join('\n')}}`;
  const module=device.createShaderModule({code});const info=await module.getCompilationInfo();
  if(info.messages.some(m=>m.type==='error'))throw new Error(info.messages.map(m=>m.message).join('\n'));
  const pipeline=await device.createComputePipelineAsync({layout:'auto',compute:{module,entryPoint:'main'}});
  const output=device.createBuffer({size:cases.length*16,usage:GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_SRC});
  const read=device.createBuffer({size:output.size,usage:GPUBufferUsage.MAP_READ|GPUBufferUsage.COPY_DST});
  try{
    const group=device.createBindGroup({layout:pipeline.getBindGroupLayout(0),entries:[{binding:0,resource:{buffer:output}}]});
    const encoder=device.createCommandEncoder(),pass=encoder.beginComputePass();pass.setPipeline(pipeline);pass.setBindGroup(0,group);pass.dispatchWorkgroups(1);pass.end();encoder.copyBufferToBuffer(output,0,read,0,output.size);device.queue.submit([encoder.finish()]);
    await read.mapAsync(GPUMapMode.READ);const values=new Float32Array(read.getMappedRange());
    return cases.map((c,i)=>{const actual=Array.from(values.slice(i*4,i*4+4));if(actual.some((v,k)=>!Number.isFinite(v)||Math.abs(v-c.expected[k])>1e-5+1e-4*Math.abs(c.expected[k])))throw new Error(`Pinned library mismatch ${c.name}: ${actual}`);return {name:c.name,actual,expected:c.expected};});
  }finally{output.destroy();read.destroy();}
}
