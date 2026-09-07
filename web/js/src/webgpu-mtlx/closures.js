// SPDX-License-Identifier: Apache-2.0
// A bounded closure program. The compiler proves the lobe bound before upload.
export const MAX_CLOSURE_LOBES = 16;
export const closureTypesWGSL = /* wgsl */`
struct Closure { lobes:array<Lobe,16>, scales:array<vec3f,16>, count:u32, interior:Medium, hasInterior:u32 }
struct Material { bsdf:Closure, emission:vec3f, opacity:f32, normal:vec3f }
fn closureImportance(c:Closure,index:u32)->f32 {
  let scale=c.scales[index];let weight=c.lobes[index].weight;
  return max(0.0,max(scale.x,max(scale.y,scale.z))*weight);
}
fn emptyClosure()->Closure {var c:Closure;return c;}
fn closureLeaf(lobe:Lobe)->Closure {var c:Closure;c.lobes[0]=lobe;c.scales[0]=vec3f(1);c.count=1u;return c;}
fn closureScale(input:Closure,scale:vec3f)->Closure {var c=input;for(var i=0u;i<c.count;i++){c.scales[i]*=scale;}return c;}
fn closureAdd(a:Closure,b:Closure)->Closure {var c=a;for(var i=0u;i<b.count;i++){c.lobes[c.count]=b.lobes[i];c.scales[c.count]=b.scales[i];c.count++;}return c;}
fn closureAddPreservingInterior(a:Closure,b:Closure)->Closure {var c=closureAdd(a,b);if(a.hasInterior!=0u){c.interior=a.interior;c.hasInterior=1u;}else if(b.hasInterior!=0u){c.interior=b.interior;c.hasInterior=1u;}return c;}
fn closureMix(bg:Closure,fg:Closure,weight:f32)->Closure {return closureAdd(closureScale(bg,vec3f(1.0-weight)),closureScale(fg,vec3f(weight)));}
fn closureMixPreservingInterior(bg:Closure,fg:Closure,weight:f32)->Closure {return closureAddPreservingInterior(closureScale(bg,vec3f(1.0-weight)),closureScale(fg,vec3f(weight)));}
fn closureInterior(top:Closure,base:Medium)->Closure {var c=top;c.interior=base;c.hasInterior=1u;return c;}
fn surfaceEmission(bsdf:Closure,edf:vec3f,opacity:f32,thinWalled:u32,normal:vec3f)->Material {var c=bsdf;for(var i=0u;i<c.count;i++){c.lobes[i].thinWalled=thinWalled;}return Material(c,edf,opacity,normal);}
fn materialFromLobe(lobe:Lobe,opacity:f32,normal:vec3f)->Material {return Material(closureLeaf(lobe),lobe.emission*lobe.emissionWeight,opacity,normal);}
fn materialFromClosure(bsdf:Closure,emission:vec3f,opacity:f32,normal:vec3f)->Material {return Material(bsdf,emission,opacity,normal);}
fn primaryLobe(surface:Material)->Lobe {
  var m=nativeDiffuse(vec3f(0),0,0);
  for(var i=0u;i<surface.bsdf.count;i++){if(closureImportance(surface.bsdf,i)>0.0){m=surface.bsdf.lobes[i];break;}}
  for(var i=0u;i<surface.bsdf.count;i++){if(surface.bsdf.lobes[i].transmission>0.0&&closureImportance(surface.bsdf,i)>0.0){m=surface.bsdf.lobes[i];break;}}
  m.emission=surface.emission;m.emissionWeight=1.0;return m;
}
`;

export const closureTransportWGSL = /* wgsl */`
fn closureTotal(c:Closure)->f32 {var total=0.0;for(var i=0u;i<c.count;i++){total+=closureImportance(c,i);}return total;}
fn closureEval(c:Closure,wo:vec3f,wi:vec3f,eta:f32)->vec4f {
  let total=closureTotal(c);var f=vec3f(0);var pdf=0.0;
  if(total<=0.0){return vec4f(0);}
  for(var i=0u;i<c.count;i++) {let l=transportEval(c.lobes[i],wo,wi,eta);f+=c.scales[i]*l.xyz;pdf+=closureImportance(c,i)/total*l.w;}
  return vec4f(f,pdf);
}
fn closureSample(c:Closure,wo:vec3f,eta:f32,rng:ptr<function,u32>)->Scatter {
  let total=closureTotal(c);if(total<=0.0){return Scatter(vec3f(0),0,vec3f(0),0u,1);}
  let threshold=random(rng)*total;var cumulative=0.0;var selected=0u;
  for(var i=0u;i<c.count;i++){cumulative+=closureImportance(c,i);if(threshold<cumulative){selected=i;break;}}
  let probability=closureImportance(c,selected)/total;
  var s=transportSample(c.lobes[selected],wo,eta,rng);
  if(s.pdf<=0.0){return s;}
  if(s.delta!=0u){s.weight*=c.scales[selected]/probability;s.pdf*=probability;return s;}
  let f=closureEval(c,wo,s.wi,eta);s.pdf=f.w;s.weight=f.xyz*abs(s.wi.z)/max(1e-30,f.w);return s;
}
fn validClosure(c:Closure)->bool {
  if(c.count>16u){return false;}
  var interfaceIOR=0.0;
  for(var i=0u;i<c.count;i++){
    let l=c.lobes[i];
    if(!all(c.scales[i]>=vec3f(0))||!all(c.scales[i]<=vec3f(3e37))||!(l.weight>=0.0&&l.weight<=3e37&&l.ior>0.0&&l.ior<=3e37&&l.roughness>=0.0&&l.roughness<=3e37)||!all(l.alpha>=vec2f(0))||!all(l.alpha<=vec2f(3e37))){return false;}
    if(!all(l.base>=vec3f(0))||!all(l.base<=vec3f(3e37))||!all(l.transmissionColor>=vec3f(0))||!all(l.transmissionColor<=vec3f(3e37))||!all(l.complexIOR>=vec3f(0))||!all(l.complexIOR<=vec3f(3e37))||!all(l.extinction>=vec3f(0))||!all(l.extinction<=vec3f(3e37))||!(l.metal>=0.0&&l.metal<=1.0&&l.transmission>=0.0&&l.transmission<=1.0)){return false;}
    if(l.transmission>0.0&&closureImportance(c,i)>0.0){if(interfaceIOR>0.0&&abs(interfaceIOR-l.ior)>1e-5){return false;}interfaceIOR=l.ior;}
  }
  return true;
}
`;
