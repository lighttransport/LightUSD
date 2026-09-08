// SPDX-License-Identifier: Apache-2.0
import { shaderSource } from './shaders.js';
import { surfaceDocument } from './scene.js';

export async function validateTransportKernels(device) {
  const film = wavelength => {
    const c = .8, base = 1.5, ior = 1.4, thickness = 180;
    const phase = 4 * Math.PI * ior * thickness * c / wavelength;
    const r01 = (1 - ior) / (1 + ior), r12 = (ior - base) / (ior + base);
    return Math.max(0, Math.min(1, r01 * r01 + r12 * r12 + 2 * r01 * r12 * Math.cos(phase)));
  };
  const analytic = [
    ['normal Fresnel', 'dielectricFresnel(1.0,1.5)', .04],
    ['normal exit Fresnel', 'dielectricFresnel(1.0,1.0/1.5)', .04],
    ['total internal reflection', 'dielectricFresnel(0.5,1.0/1.5)', 1],
    ['matched index', 'dielectricFresnel(0.7,1.0)', 0],
    ['matched index grazing', 'dielectricFresnel(0.0,1.0)', 0],
    ['GGX peak', 'microfacetD(vec3f(0,0,1),vec2f(0.5))', 4 / Math.PI],
    ['GGX normal masking', 'microfacetG1(vec3f(0,0,1),vec2f(0.2,0.5))', 1],
    ['conductor normal Fresnel', 'conductorFresnel(1.0,vec3f(0.2),vec3f(3)).x', ((.2-1)**2+9)/((.2+1)**2+9)],
    ['thin film 450nm', 'thinFilmFresnelLambda(.8,1.5,1.4,180.0,450.0)', film(450)],
    ['thin film 650nm', 'thinFilmFresnelLambda(.8,1.5,1.4,180.0,650.0)', film(650)],
    ['closure weighted evaluation', 'closureEval(closureMix(closureLeaf(nativeDiffuse(vec3f(.2),1,0)),closureLeaf(nativeDiffuse(vec3f(.8),1,0)),.25),vec3f(0,0,1),vec3f(0,0,1),1.5,0.0).x', .35/Math.PI],
    ['closure mixture PDF', 'closureEval(closureMix(closureLeaf(nativeDiffuse(vec3f(.2),1,0)),closureLeaf(nativeDiffuse(vec3f(.8),1,0)),.25),vec3f(0,0,1),vec3f(0,0,1),1.5,0.0).w', 1/Math.PI],
    ['scaled subsurface profile', 'nativeEval(nativeSubsurface(vec3f(.7,.2,.1),1,vec3f(.5,.125,.0625),.35),vec3f(0,0,1),vec3f(0,0,1),1.5,0.0).x', .7*(.25/Math.PI+.75/(2*Math.PI*.25))],
    ['weighted layered base transmission', 'closureEval(closureLayer(closureLeaf(nativeTranslucent(vec3f(1),.25)),closureLeaf(nativeDiffuse(vec3f(.5),1,0))),vec3f(0,0,1),vec3f(0,0,1),1.5,0.0).x', .125/Math.PI],
    ['inactive transmission interface', 'primaryLobe(surfaceEmission(closureMix(closureLeaf(nativeDielectric(vec3f(1),1.8,vec2f(.1),1,3u)),closureLeaf(nativeDiffuse(vec3f(.5),1,0)),1.0),vec3f(0),1.0,0u,vec3f(0,0,1),vec3f(1,0,0),vec3f(0,1,0),vec3f(0,0,1),-1.0,-1.0,vec3f(1),vec3f(1),5.0,0u,0u,0u)).transmission', 0],
    ['secondary invalid lobe', 'select(0.0,1.0,validClosure(closureAdd(closureLeaf(nativeDiffuse(vec3f(1),1,0)),closureLeaf(nativeDiffuse(vec3f(-1),1,0)))))',0],
  ];
  const count = analytic.length + 4;
  const nestedLayer = { nodes: [
    { name: 'top', category: 'oren_nayar_diffuse_bsdf', type: 'BSDF', inputs: { color: { type: 'color3', value: [.8, .2, .1] } } },
    { name: 'base', category: 'sheen_bsdf', type: 'BSDF', inputs: { color: { type: 'color3', value: [.1, .3, .8] } } },
    { name: 'inner', category: 'layer', type: 'BSDF', inputs: { top: { nodename: 'top' }, base: { nodename: 'base' } } },
    { name: 'outerBase', category: 'burley_diffuse_bsdf', type: 'BSDF', inputs: { color: { type: 'color3', value: [.2, .2, .2] } } },
    { name: 'outer', category: 'layer', type: 'BSDF', inputs: { top: { nodename: 'inner' }, base: { nodename: 'outerBase' } } },
    { name: 'surface', category: 'surface', type: 'surfaceshader', inputs: { bsdf: { nodename: 'outer' } } },
  ] };
  const volumeComposition = { mediumMajorant: 4, nodes: [
    { name: 'baseMedium', category: 'absorption_vdf', type: 'VDF', inputs: { absorption: { type: 'color3', value: [.1, .2, .3] } } },
    { name: 'scatterMedium', category: 'anisotropic_vdf', type: 'VDF', inputs: { scattering: { type: 'color3', value: [.2, .3, .4] }, anisotropy: { type: 'float', value: .25 } } },
    { name: 'sumMedium', category: 'add', type: 'VDF', inputs: { in1: { nodename: 'baseMedium' }, in2: { nodename: 'scatterMedium' } } },
    { name: 'mixMedium', category: 'mix', type: 'VDF', inputs: { bg: { nodename: 'baseMedium' }, fg: { nodename: 'sumMedium' }, mix: { type: 'float', value: .25 } } },
    { name: 'scaledMedium', category: 'multiply', type: 'VDF', inputs: { in1: { nodename: 'mixMedium' }, in2: { type: 'color3', value: [.8, .9, 1] } } },
    { name: 'surface', category: 'surface', type: 'surfaceshader', inputs: { bsdf: { nodename: 'top' } } },
    { name: 'top', category: 'oren_nayar_diffuse_bsdf', type: 'BSDF', inputs: { color: { type: 'color3', value: [.4, .4, .4] } } },
  ], mediumOutput: { nodename: 'scaledMedium' }, output: { nodename: 'surface' } };
  const module = device.createShaderModule({ code: shaderSource([surfaceDocument(), nestedLayer, volumeComposition]) + `
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
      let m=makeMaterial(vec3f(1),0,.3,1.5,1,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);
      for(var i=0u;i<32768u;i++) {
        let s=transportSample(m,wo,1.5,&rng,0.0);
        if(s.pdf>0.0) {
          let f=transportEval(m,wo,s.wi,1.5,0.0);
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
