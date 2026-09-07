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
