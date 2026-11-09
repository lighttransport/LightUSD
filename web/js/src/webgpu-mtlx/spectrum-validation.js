// SPDX-License-Identifier: Apache-2.0
import { shaderSource } from './shaders.js';
import { surfaceDocument } from './scene.js';
import { cieXYZ } from './cie-data.js';
import { CIE_Y_INTEGRAL } from './spectrum.js';

export async function validateSpectrumKernels(device) {
  const resources = {}, doc = surfaceDocument();
  doc.spectra = { ior: [[360,1.6],[830,1.4]], base_color: [[360,.2],[500,.8],[830,.4]] };
  const module = device.createShaderModule({ code: shaderSource([doc], resources) + `
    @group(0) @binding(9) var<storage,read_write> checks: array<vec4f>;
    @compute @workgroup_size(1) fn validateSpectrum() {
      var xyz=vec3f(0);var white=vec3f(0);
      for(var i=0u;i<470u;i++) { let l=360.5+f32(i);xyz+=cieAt(l);white+=cieAt(l)*rgbSpectrum(vec3f(1),l,true); }
      checks[0]=vec4f(xyz/106.856915,0);checks[1]=vec4f(xyzToRGB(white/106.856915),0);
      checks[2]=vec4f(cieAt(555.0),0);
      checks[3]=vec4f(measured_0_ior_at(360.0),measured_0_ior_at(595.0),measured_0_ior_at(830.0),measured_0_base_at(500.0));
    }` });
  const info = await module.getCompilationInfo(); if(info.messages.some(m=>m.type==='error')) throw new Error(info.messages.map(m=>m.message).join('\n'));
  const pipeline=await device.createComputePipelineAsync({layout:'auto',compute:{module,entryPoint:'validateSpectrum'}});
  const table=device.createBuffer({size:resources.spectralData.byteLength,usage:GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_DST});
  const output=device.createBuffer({size:64,usage:GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_SRC});
  const read=device.createBuffer({size:64,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});
  try {
    device.queue.writeBuffer(table,0,resources.spectralData);
    const group=device.createBindGroup({layout:pipeline.getBindGroupLayout(0),entries:[{binding:8,resource:{buffer:table}},{binding:9,resource:{buffer:output}}]});
    const encoder=device.createCommandEncoder(),pass=encoder.beginComputePass();pass.setPipeline(pipeline);pass.setBindGroup(0,group);pass.dispatchWorkgroups(1);pass.end();encoder.copyBufferToBuffer(output,0,read,0,64);device.queue.submit([encoder.finish()]);await read.mapAsync(GPUMapMode.READ);
    const v=new Float32Array(read.getMappedRange()).slice();
    const expected=[0,1,2].map(k=>cieXYZ.reduce((sum,row,i)=>sum+row[k]*(i===0||i===470?.5:1),0)/CIE_Y_INTEGRAL);
    const close=(a,b,name,tolerance=1e-5)=>{if(!Number.isFinite(a)||Math.abs(a-b)>tolerance)throw new Error(`${name}: ${a} != ${b}`);};
    expected.forEach((n,k)=>close(v[k],n,'CIE integral'));cieXYZ[195].forEach((n,k)=>close(v[8+k],n,'CIE 555 nm'));
    [1.6,1.5,1.4,.8].forEach((n,k)=>close(v[12+k],n,'measured spectrum'));
    for(let k=0;k<3;k++)close(v[4+k],1,'illuminant white reconstruction',.035);
    return {equalEnergyXYZ:Array.from(v.slice(0,3)),upliftWhiteRGB:Array.from(v.slice(4,7)),measured:Array.from(v.slice(12,16))};
  } finally {table.destroy();output.destroy();read.destroy();}
}
