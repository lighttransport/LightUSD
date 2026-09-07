// SPDX-License-Identifier: Apache-2.0
import { compileGraph, contextWGSL } from './graph.js';
import { packImages, imageWGSL } from './textures.js';
import { cross, sub, normalize } from './scene.js';

/** Linear triangle refinement, not Catmull-Clark subdivision. Material seams split. */
export function refineDisplacementScene(scene, levels = 0, maxTriangles = 500_000) {
  if(!Number.isInteger(levels)||levels<0||levels>5)throw new Error('Displacement refinement must be 0..5');
  if(!scene.indices?.length || scene.indices.length%3 || scene.indices.length/3*4**levels>maxTriangles)throw new Error('Displacement triangle budget exceeded');
  const result={...scene,positions:[],normals:[],uvs:[],indices:[],materialIds:[]};
  function vertex(i) {
    if(!Number.isInteger(i)||i<0||i*3+2>=scene.positions.length)throw new Error('Invalid displacement index');
    const p=Array.from(scene.positions.slice(i*3,i*3+3)),n=scene.normals?Array.from(scene.normals.slice(i*3,i*3+3)):null,uv=scene.uvs?Array.from(scene.uvs.slice(i*2,i*2+2)):[0,0];
    if(![...p,...(n||[]),...uv].every(v=>Number.isFinite(v)&&Number.isFinite(Math.fround(v))))throw new Error('Invalid displacement geometry');
    return {p,n,uv};
  }
  const midpoint=(a,b)=>({p:a.p.map((v,k)=>(v+b.p[k])*.5),n:normalize(a.n.map((v,k)=>(v+b.n[k])*.5)),uv:a.uv.map((v,k)=>(v+b.uv[k])*.5)});
  function triangle(a,b,c,mat,depth) {
    if(depth) {const ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);triangle(a,ab,ca,mat,depth-1);triangle(ab,b,bc,mat,depth-1);triangle(ca,bc,c,mat,depth-1);triangle(ab,bc,ca,mat,depth-1);return;}
    for(const v of [a,b,c]){result.indices.push(result.positions.length/3);result.positions.push(...v.p);result.normals.push(...v.n);result.uvs.push(...v.uv);}result.materialIds.push(mat);
  }
  for(let t=0;t<scene.indices.length/3;t++) {
    const mat=scene.materialIds?.[t]??0;if(!Number.isInteger(mat)||!scene.materials[mat])throw new Error('Invalid displacement material');
    const [a,b,c]=Array.from(scene.indices.slice(t*3,t*3+3),vertex);const n=normalize(cross(sub(b.p,a.p),sub(c.p,a.p)));for(const v of [a,b,c])v.n=v.n?normalize(v.n):n;
    triangle(a,b,c,mat,levels);
  }
  return result;
}

export async function bakeDisplacement(scene, device) {
  if(!scene.materials.some(m=>m.displacementOutput))return scene;
  const refined=refineDisplacementScene(scene,scene.displacementRefinement??0);
  const packed=packImages(scene.materials.flatMap(m=>Object.values(m.images||{})));let imageIndex=0,usedImages=false;
  const functions=scene.materials.map((doc,i)=>{
    const imageDescriptors=Object.fromEntries(Object.entries(doc.images||{}).map(([name,image])=>[name,{...packed.descriptors[imageIndex++],colorspace:image.colorspace||'lin_rec709'}]));
    if(!doc.displacementOutput)return `fn displacement${i}(ctx:ShadingContext)->vec3f{return vec3f(0);}`;
    const c=compileGraph(doc,{output:doc.displacementOutput,imageDescriptors});
    usedImages ||= c.categories.includes('image');
    if(!['float','vector3'].includes(c.type))throw new Error('Displacement output must be float height or world-space vector3');
    return `fn displacement${i}(ctx:ShadingContext)->vec3f{${c.body}\nreturn ${c.type==='float'?`ctx.normal*${c.expression}`:c.expression};}`;
  }).join('\n');
  const count=refined.positions.length/3,data=new Float32Array(count*12);
  for(let i=0;i<count;i++)data.set([...refined.positions.slice(i*3,i*3+3),0,...refined.normals.slice(i*3,i*3+3),0,...refined.uvs.slice(i*2,i*2+2),refined.materialIds[Math.floor(i/3)],0],i*12);
  if(data.byteLength>device.limits.maxStorageBufferBindingSize)throw new Error('Displacement vertices exceed WebGPU buffer limit');
  const module=device.createShaderModule({code:`${contextWGSL}\n${imageWGSL}\n${functions}
    struct BakeVertex {p:vec4f,n:vec4f,uv:vec4f}
    @group(0) @binding(0) var<storage,read> source:array<BakeVertex>;
    @group(0) @binding(1) var<storage,read_write> result:array<vec4f>;
    @compute @workgroup_size(64) fn bake(@builtin(global_invocation_id) id:vec3u){
      if(id.x>=arrayLength(&source)){return;}let v=source[id.x];let n=normalize(v.n.xyz);
      let t=normalize(cross(select(vec3f(0,1,0),vec3f(1,0,0),abs(n.y)>.9),n));
      let ctx=ShadingContext(v.p.xyz,n,t,cross(n,t),v.uv.xy,0,0,vec2f(0),vec2f(0));var d=vec3f(0);
      switch u32(v.uv.z){${scene.materials.map((_,i)=>`case ${i}u:{d=displacement${i}(ctx);}`).join('')}default:{}}
      result[id.x]=vec4f(v.p.xyz+d,0);
    }`});
  const info=await module.getCompilationInfo();if(info.messages.some(m=>m.type==='error'))throw new Error(info.messages.map(m=>m.message).join('\n'));
  const pipeline=await device.createComputePipelineAsync({layout:'auto',compute:{module,entryPoint:'bake'}});
  const resources=[];const buffer=(size,usage)=>{const b=device.createBuffer({size,usage});resources.push(b);return b;};
  try {
    const source=buffer(data.byteLength,GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_DST),result=buffer(count*16,GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_SRC),images=buffer(packed.data.byteLength,GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_DST),read=buffer(count*16,GPUBufferUsage.MAP_READ|GPUBufferUsage.COPY_DST);
    device.queue.writeBuffer(source,0,data);device.queue.writeBuffer(images,0,packed.data);
    // Auto-layout excludes unused image resources for non-textured displacement.
    const entries=[{binding:0,resource:{buffer:source}},{binding:1,resource:{buffer:result}}];if(usedImages)entries.push({binding:4,resource:{buffer:images}});
    const group=device.createBindGroup({layout:pipeline.getBindGroupLayout(0),entries});const encoder=device.createCommandEncoder(),pass=encoder.beginComputePass();pass.setPipeline(pipeline);pass.setBindGroup(0,group);pass.dispatchWorkgroups(Math.ceil(count/64));pass.end();encoder.copyBufferToBuffer(result,0,read,0,read.size);device.queue.submit([encoder.finish()]);await read.mapAsync(GPUMapMode.READ);
    const output=new Float32Array(read.getMappedRange());for(let i=0;i<count;i++)for(let k=0;k<3;k++){const value=output[i*4+k];if(!Number.isFinite(value))throw new Error('Non-finite displacement');refined.positions[i*3+k]=value;}
    const sums=new Map();
    for(let i=0;i<count;i+=3){const p=[0,1,2].map(k=>refined.positions.slice((i+k)*3,(i+k)*3+3));const n=normalize(cross(sub(p[1],p[0]),sub(p[2],p[0])));const mat=refined.materialIds[Math.floor(i/3)];for(let v=0;v<3;v++){const uv=refined.uvs.slice((i+v)*2,(i+v)*2+2);const key=`${mat}:${p[v].join(',')}:${uv.join(',')}`;const sum=sums.get(key)||[0,0,0];sum[0]+=n[0];sum[1]+=n[1];sum[2]+=n[2];sums.set(key,sum);}}
    for(let i=0;i<count;i++){const p=refined.positions.slice(i*3,i*3+3),uv=refined.uvs.slice(i*2,i*2+2),mat=refined.materialIds[Math.floor(i/3)],key=`${mat}:${p.join(',')}:${uv.join(',')}`,n=normalize(sums.get(key)||[0,1,0]);for(let k=0;k<3;k++)refined.normals[i*3+k]=n[k];}
    refined.provenance={...scene.provenance,displacement:{refinement:scene.displacementRefinement??0,scheme:'linear triangles',bakedBeforeBVH:true}};return refined;
  } finally {resources.forEach(b=>b.destroy());}
}
