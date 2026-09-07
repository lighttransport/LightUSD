// SPDX-License-Identifier: Apache-2.0
import { shaderSource } from './shaders.js';
import { surfaceDocument } from './scene.js';

export async function validateTransportKernels(device) {
  const analytic = [
    ['normal Fresnel', 'dielectricFresnel(1.0,1.5)', .04],
    ['normal exit Fresnel', 'dielectricFresnel(1.0,1.0/1.5)', .04],
    ['total internal reflection', 'dielectricFresnel(0.5,1.0/1.5)', 1],
    ['matched index', 'dielectricFresnel(0.7,1.0)', 0],
    ['matched index grazing', 'dielectricFresnel(0.0,1.0)', 0],
    ['GGX peak', 'microfacetD(vec3f(0,0,1),vec2f(0.5))', 4 / Math.PI],
    ['GGX normal masking', 'microfacetG1(vec3f(0,0,1),vec2f(0.2,0.5))', 1],
    ['conductor normal Fresnel', 'conductorFresnel(1.0,vec3f(0.2),vec3f(3)).x', ((.2-1)**2+9)/((.2+1)**2+9)],
  ];
  const count = analytic.length + 4;
  const module = device.createShaderModule({ code: shaderSource([surfaceDocument()]) + `
    @group(0) @binding(9) var<storage,read_write> checks: array<vec4f>;
    @compute @workgroup_size(1) fn validateTransport() {
      ${analytic.map(([,expr], i) => `checks[${i}]=vec4f(${expr},0,0,0);`).join('\n')}
      let wo=normalize(vec3f(.2,.3,1)); let wi=normalize(vec3f(-.4,.1,1));
      let a=dielectricEval(wo,wi,vec2f(.2,.4),1.5); let b=dielectricEval(wi,wo,vec2f(.2,.4),1.5);
      checks[${analytic.length}]=vec4f(a.x,b.x,a.y,b.y);
      let wt=normalize(vec3f(-.1,-.2,-1));
      let ft=dielectricEval(wo,wt,vec2f(.2,.4),1.5); let rt=dielectricEval(-wt,-wo,vec2f(.2,.4),1.0/1.5);
      checks[${analytic.length+1}]=vec4f(ft.x*2.25,rt.x,ft.y,rt.y);
      var rng=123456u; var total=0.0; var error=0.0; var transmitted=0.0;
      let m=makeMaterial(vec3f(1),0,.3,1.5,1,vec3f(0),0,0,vec3f(1));
      for(var i=0u;i<32768u;i++) {
        let s=transportSample(m,wo,1.5,&rng);
        if(s.pdf>0.0) {
          let f=transportEval(m,wo,s.wi,1.5);
          error=max(error,abs(s.pdf-f.w));
          total+=s.weight.x*select(1.0,2.25,s.wi.z<0.0);
          if(s.wi.z<0.0) { transmitted+=1.0; }
        }
      }
      checks[${analytic.length+2}]=vec4f(total/32768.0,error,transmitted/32768.0,0);
      var beer=0.0;var cosine=0.0;
      for(var i=0u;i<32768u;i++) {
        if(-log(1.0-random(&rng))>1.0) {beer+=1.0;}
        let d=hgDirection(vec3f(0,0,1),0.4,vec2f(random(&rng),random(&rng)));cosine+=d.z;
      }
      checks[${analytic.length+3}]=vec4f(beer/32768.0,cosine/32768.0,hgPhase(.7,0.0),0);
    }` });
  const info=await module.getCompilationInfo(); if(info.messages.some(m=>m.type==='error')) throw new Error(info.messages.map(m=>m.message).join('\n'));
  const pipeline=await device.createComputePipelineAsync({layout:'auto',compute:{module,entryPoint:'validateTransport'}});
  const output=device.createBuffer({size:count*16,usage:GPUBufferUsage.STORAGE|GPUBufferUsage.COPY_SRC});
  const read=device.createBuffer({size:count*16,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});
  try {
    const group=device.createBindGroup({layout:pipeline.getBindGroupLayout(0),entries:[{binding:9,resource:{buffer:output}}]});
    const encoder=device.createCommandEncoder(),pass=encoder.beginComputePass(); pass.setPipeline(pipeline);pass.setBindGroup(0,group);pass.dispatchWorkgroups(1);pass.end();encoder.copyBufferToBuffer(output,0,read,0,read.size);device.queue.submit([encoder.finish()]);
    await read.mapAsync(GPUMapMode.READ);const values=new Float32Array(read.getMappedRange()).slice();
    for(let i=0;i<analytic.length;i++) if(Math.abs(values[i*4]-analytic[i][2])>1e-5 || !Number.isFinite(values[i*4])) throw new Error(`${analytic[i][0]}: ${values[i*4]} != ${analytic[i][2]}`);
    for(let i=analytic.length;i<analytic.length+2;i++) if(Math.abs(values[i*4]-values[i*4+1])>1e-5*Math.max(1,values[i*4]) || values[i*4]<=0) throw new Error(`Reciprocity failed: ${values.slice(i*4,i*4+4)}`);
    const energy=values[(count-2)*4],pdfError=values[(count-2)*4+1],transmitted=values[(count-2)*4+2];
    if(!(energy>.95 && energy<1.005 && pdfError<1e-5 && transmitted>.9)) throw new Error(`Dielectric sampling energy=${energy}, pdf error=${pdfError}, transmitted=${transmitted}`);
    const beer=values[(count-1)*4],phaseMean=values[(count-1)*4+1],isotropic=values[(count-1)*4+2];
    if(Math.abs(beer-Math.exp(-1))>.01 || Math.abs(phaseMean-.4)>.01 || Math.abs(isotropic-1/(4*Math.PI))>1e-6)throw new Error(`Volume sampling: Beer=${beer}, HG mean=${phaseMean}, isotropic=${isotropic}`);
    return { analytic:analytic.length,reciprocity:2,samples:32768,energy,pdfError,transmitted,beer,phaseMean };
  } finally {output.destroy();read.destroy();}
}
