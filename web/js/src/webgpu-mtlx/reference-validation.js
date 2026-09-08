// SPDX-License-Identifier: Apache-2.0
// Independent analytic transport scenes, not self-generated image goldens.
function planeScene(materials, planes, environment) {
  const scene={positions:[],normals:[],uvs:[],indices:[],materialIds:[],materials,camera:{origin:[0,0,1],target:[0,0,0],fov:.01},lighting:{environment,directional:{radiance:[0,0,0]}}};
  for(const [z,flip,material]of planes){const offset=scene.positions.length/3;scene.positions.push(-100,-100,z,100,-100,z,100,100,z,-100,100,z);for(let i=0;i<4;i++)scene.normals.push(0,0,flip?-1:1);scene.uvs.push(0,0,1,0,1,1,0,1);scene.indices.push(...(flip?[0,2,1,0,3,2]:[0,1,2,0,2,3]).map(i=>i+offset));scene.materialIds.push(material,material);}
  return scene;
}
export async function validateReferenceScenes(renderer) {
  const diffuse={nodes:[{name:'bsdf',category:'oren_nayar_diffuse_bsdf',type:'BSDF',inputs:{color:{type:'color3',value:[.2,.4,.6]}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{bsdf:{nodename:'bsdf'}}}]};
  const glass={nodes:[{name:'bsdf',category:'dielectric_bsdf',type:'BSDF',inputs:{ior:{type:'float',value:1.5},roughness:{type:'vector2',value:[0,0]},scatter_mode:{type:'string',value:'RT'}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{bsdf:{nodename:'bsdf'}}}]};
  const emitter={nodes:[{name:'edf',category:'uniform_edf',type:'EDF'},{name:'surface',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'edf'}}}]};
  // Two parallel lossless interfaces: sum all internal reflection orders,
  // T=(1-F)^2/(1-F^2)=(1-F)/(1+F), with F=((1.5-1)/(1.5+1))^2.
  const fresnel=.04,transmission=(1-fresnel)/(1+fresnel);
  const fixtures=[{name:'Lambert furnace',scene:planeScene([diffuse],[[0,false,0]],[1,1,1]),expected:[.2,.4,.6]},
    {name:'lossless dielectric slab',scene:planeScene([glass,emitter],[[0,false,0],[-1,true,0],[-2,false,1]],[0,0,0]),expected:[transmission,transmission,transmission]}];
  const results=[];const width=renderer.canvas.width,height=renderer.canvas.height,seed=renderer.options.seed;
  try{
    renderer.canvas.width=32;renderer.canvas.height=32;renderer.setOptions({resolutionScale:1});
    for(const fixture of fixtures){await renderer.loadScene(fixture.scene);renderer.setMode('path-physical');
      for(const testSeed of [1,17,31337]){renderer.setOptions({seed:testSeed});
        for(const spp of [8,32]){
          let dispatches=0;while(renderer.samples<spp&&dispatches++<2048)await renderer.renderStep();
          if(renderer.samples<spp)throw new Error('Analytic reference scene did not converge to requested sample count');
          const capture=await renderer.capture({format:'float32'}),count=capture.sampleCounts.length,mean=[0,0,0],variance=[0,0,0];
          for(let i=0;i<count;i++)for(let k=0;k<3;k++){mean[k]+=capture.pixels[i*4+k]/count;variance[k]+=capture.variance[i*4+k]/(count*count);}
          const standardError=variance.map(Math.sqrt);
          for(let k=0;k<3;k++)if(!Number.isFinite(mean[k])||!Number.isFinite(standardError[k])||Math.abs(mean[k]-fixture.expected[k])>4*standardError[k]+1e-4)throw new Error(`${fixture.name} seed ${testSeed}, ${spp} spp: ${mean} expected ${fixture.expected}, stderr ${standardError}`);
          results.push({name:fixture.name,seed:testSeed,spp,mean,standardError,expected:fixture.expected});
        }
      }
    }
    return results;
  }finally{renderer.canvas.width=width;renderer.canvas.height=height;renderer.setOptions({seed});}
}

/** Coverage of an emissive cutout has an analytic Bernoulli expectation. */
export async function validateOpacityScenes(renderer) {
  const emission=[2,3,4], background=[.1,.2,.3];
  const emitter=opacity=>({nodes:[
    {name:'e',category:'uniform_edf',type:'EDF',inputs:{color:{type:'color3',value:emission}}},
    {name:'s',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'e'},opacity:{type:'float',value:opacity}}},
  ]});
  const fixtures=[0,.25,.5,1].map(opacity=>({name:`emissive cutout ${opacity}`,opacity,
    scene:planeScene([emitter(opacity)],[[0,false,0]],background),
    expected:emission.map((v,k)=>opacity*v+(1-opacity)*background[k])}));
  fixtures.push({name:'six transparent planes across dispatches',opacity:1,
    scene:planeScene([emitter(0),emitter(1)],Array.from({length:7},(_,i)=>[-i,false,i===6?1:0]),background),expected:emission});
  const results=[];
  renderer.canvas.width=16;renderer.canvas.height=16;renderer.setOptions({resolutionScale:1});
  for(const fixture of fixtures){
    await renderer.loadScene(fixture.scene);renderer.setMode('path-physical');renderer.setOptions({seed:173});
    let dispatches=0;
    while(renderer.samples<32&&dispatches++<128)await renderer.renderStep();
    if(renderer.samples!==32)throw new Error(`${fixture.name}: cutout paths did not finish`);
    const capture=await renderer.capture({format:'float32'}),count=capture.sampleCounts.length,mean=[0,0,0];
    for(let i=0;i<count;i++)for(let k=0;k<3;k++)mean[k]+=capture.pixels[i*4+k]/count;
    for(let k=0;k<3;k++){
      // Known Bernoulli variance, independent of the renderer's variance buffer.
      const error=Math.abs(emission[k]-background[k])*Math.sqrt(fixture.opacity*(1-fixture.opacity)/(count*32));
      if(!Number.isFinite(mean[k])||Math.abs(mean[k]-fixture.expected[k])>5*error+1e-5)throw new Error(`${fixture.name}: ${mean} expected ${fixture.expected}`);
    }
    results.push({name:fixture.name,mean,expected:fixture.expected,dispatches,samples:renderer.samples});
  }
  return results;
}

/** Encode a tangent-space normal as emission to isolate the geometric frame. */
export async function validateSurfaceFrameScenes(renderer) {
  const material={nodes:[
    {name:'n',category:'normalmap',type:'vector3',inputs:{in:{type:'vector3',value:[1,.5,1]}}},
    {name:'c',category:'convert',type:'color3',inputs:{in:{nodename:'n'}}},
    {name:'half',category:'multiply',type:'color3',inputs:{in1:{nodename:'c'},in2:{type:'float',value:.5}}},
    {name:'offset',category:'add',type:'color3',inputs:{in1:{nodename:'half'},in2:{type:'float',value:.5}}},
    {name:'e',category:'uniform_edf',type:'EDF',inputs:{color:{nodename:'offset'}}},
    {name:'s',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'e'}}},
  ]};
  const q=Math.SQRT1_2*.5, results=[];
  for(const [name,uvs,expected] of [
    ['standard',[0,0,1,0,1,1,0,1],[.5+q,.5,.5+q]],
    ['rotated/mirrored',[0,0,0,1,1,1,1,0],[.5,.5+q,.5+q]],
    ['mirrored U',[0,0,-1,0,-1,1,0,1],[.5-q,.5,.5+q]],
  ]){
    const scene=planeScene([material],[[0,false,0]],[0,0,0]);scene.uvs=uvs;
    // Raster derivatives must resolve at float32 precision across the plane.
    scene.camera.fov=45;scene.positions=scene.positions.map(v=>v/100);
    renderer.canvas.width=16;renderer.canvas.height=16;renderer.setOptions({resolutionScale:1,exposure:0});
    await renderer.loadScene(scene);renderer.setMode('path-physical');
    await renderer.renderStep();
    const capture=await renderer.capture({format:'float32'});
    for(let i=0;i<capture.pixels.length;i+=4)for(let k=0;k<3;k++){
      if(!Number.isFinite(capture.pixels[i+k])||Math.abs(capture.pixels[i+k]-expected[k])>1e-5)throw new Error(`${name}: wrong path normal emission`);
    }
    renderer.setMode('realtime');await renderer.renderStep();
    const png=await renderer.capture({format:'png'});
    const bitmap=await createImageBitmap(new Blob([png.bytes],{type:'image/png'}));
    const canvas=new OffscreenCanvas(16,16),ctx=canvas.getContext('2d');ctx.drawImage(bitmap,0,0);bitmap.close();
    const pixel=Array.from(ctx.getImageData(8,8,1,1).data);
    const display=expected.map(v=>{const m=v/(1+v);return Math.round(255*(m<=.0031308?12.92*m:1.055*m**(1/2.4)-.055));});
    if(display.some((v,k)=>Math.abs(v-pixel[k])>2))throw new Error(`${name}: raster ${pixel} expected ${display}`);
    results.push({name,expected,path:Array.from(capture.pixels.slice(0,3)),raster:pixel});
  }
  return results;
}
