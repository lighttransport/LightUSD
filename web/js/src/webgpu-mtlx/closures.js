// SPDX-License-Identifier: Apache-2.0
// A bounded closure program. The compiler proves the lobe bound before upload.
export const MAX_CLOSURE_LOBES = 16;
export const MAX_CLOSURE_NODES = 32;
export const closureTypesWGSL = /* wgsl */`
struct ClosureNode { kind:u32, aKind:u32, a:u32, aStart:u32, aCount:u32, bKind:u32, b:u32, bStart:u32, bCount:u32, mix:f32 }
struct Closure { lobes:array<Lobe,16>, scales:array<vec3f,16>, count:u32, nodes:array<ClosureNode,32>, nodeCount:u32, rootKind:u32, root:u32, interior:Medium, hasInterior:u32 }
struct Material { bsdf:Closure, emission:vec3f, opacity:f32, normal:vec3f, tangent:vec3f, bitangent:vec3f, emissionDirection:vec3f, emissionInnerCos:f32, emissionOuterCos:f32, emissionColor0:vec3f, emissionColor90:vec3f, emissionExponent:f32, emissionCone:u32, emissionSchlick:u32, emissionProfile:u32 }
fn closureImportance(c:Closure,index:u32)->f32 {
  let scale=c.scales[index];let weight=c.lobes[index].weight;
  return max(0.0,max(scale.x,max(scale.y,scale.z))*weight);
}
fn emptyClosure()->Closure {var c:Closure;c.rootKind=0u;c.root=0u;return c;}
fn closureLeaf(lobe:Lobe)->Closure {var c:Closure;c.lobes[0]=lobe;c.scales[0]=vec3f(1);c.count=1u;c.rootKind=0u;c.root=0u;return c;}
fn closureScale(input:Closure,scale:vec3f)->Closure {var c=input;for(var i=0u;i<c.count;i++){c.scales[i]*=scale;}return c;}
fn closureSelect(falseValue:Closure,trueValue:Closure,condition:bool)->Closure {if(condition){return trueValue;}return falseValue;}
fn closureCopyNodes(src:Closure,offset:u32,lobeOffset:u32,layerOffset:u32,dst:ptr<function,Closure>){for(var i=0u;i<src.nodeCount;i++){var n=src.nodes[i];if(n.aKind==0u){n.a+=lobeOffset;}else{n.a+=layerOffset;}n.aStart+=lobeOffset;if(n.bKind==0u){n.b+=lobeOffset;}else{n.b+=layerOffset;}n.bStart+=lobeOffset;(*dst).nodes[offset+i]=n;}}
fn closureRootKind(c:Closure)->u32 {return c.rootKind;}
fn closureRootIndex(c:Closure)->u32 {return c.root;}
fn closureRootCount(c:Closure)->u32 {return select(c.count,0u,c.rootKind!=0u);}
fn closureCombine(a:Closure,b:Closure,kind:u32,mix:f32)->Closure {var c=a;let nodeOffset=c.nodeCount;let lobeOffset=c.count;for(var i=0u;i<b.count;i++){c.lobes[c.count+i]=b.lobes[i];c.scales[c.count+i]=b.scales[i];}c.count+=b.count;closureCopyNodes(b,nodeOffset,lobeOffset,nodeOffset,&c);c.nodeCount+=b.nodeCount;let node=c.nodeCount;c.nodes[node]=ClosureNode(kind,a.rootKind,a.root,a.rootKind*0u,a.count,b.rootKind,b.root+b.rootKind*nodeOffset,lobeOffset,b.count,mix);if(b.rootKind==0u){c.nodes[node].b=b.root+lobeOffset;}c.nodeCount++;c.rootKind=1u;c.root=node;return c;}
fn closureAdd(a:Closure,b:Closure)->Closure {return closureCombine(a,b,1u,0.0);}
fn closureAddPreservingInterior(a:Closure,b:Closure)->Closure {var c=closureAdd(a,b);if(a.hasInterior!=0u){c.interior=a.interior;c.hasInterior=1u;}else if(b.hasInterior!=0u){c.interior=b.interior;c.hasInterior=1u;}return c;}
fn closureMix(bg:Closure,fg:Closure,weight:f32)->Closure {return closureCombine(closureScale(bg,vec3f(1.0-weight)),closureScale(fg,vec3f(weight)),2u,clamp(weight,0.0,1.0));}
fn closureMixPreservingInterior(bg:Closure,fg:Closure,weight:f32)->Closure {let b=closureScale(bg,vec3f(1.0-weight));let f=closureScale(fg,vec3f(weight));var c=closureCombine(b,f,2u,clamp(weight,0.0,1.0));if(bg.hasInterior!=0u){c.interior=bg.interior;c.hasInterior=1u;}else if(fg.hasInterior!=0u){c.interior=fg.interior;c.hasInterior=1u;}return c;}
fn closureLayer(top:Closure,base:Closure)->Closure {var c=closureCombine(top,base,3u,0.0);if(top.hasInterior!=0u){c.interior=top.interior;c.hasInterior=1u;}else if(base.hasInterior!=0u){c.interior=base.interior;c.hasInterior=1u;}return c;}
fn closureInterior(top:Closure,base:Medium)->Closure {var c=top;c.interior=base;c.hasInterior=1u;return c;}
fn surfaceEmission(bsdf:Closure,edf:vec3f,opacity:f32,thinWalled:u32,normal:vec3f,tangent:vec3f,bitangent:vec3f,emissionDirection:vec3f,emissionInnerCos:f32,emissionOuterCos:f32,emissionColor0:vec3f,emissionColor90:vec3f,emissionExponent:f32,emissionCone:u32,emissionSchlick:u32,emissionProfile:u32)->Material {var c=bsdf;for(var i=0u;i<c.count;i++){c.lobes[i].thinWalled=thinWalled;}return Material(c,edf,opacity,normal,tangent,bitangent,emissionDirection,emissionInnerCos,emissionOuterCos,emissionColor0,emissionColor90,emissionExponent,emissionCone,emissionSchlick,emissionProfile);}
fn materialFromLobe(lobe:Lobe,opacity:f32,normal:vec3f)->Material {return Material(closureLeaf(lobe),lobe.emission*lobe.emissionWeight,opacity,normal,vec3f(0),vec3f(0),normal,-1.0,-1.0,vec3f(1),vec3f(1),5.0,0u,0u,0u);}
fn materialFromClosure(bsdf:Closure,emission:vec3f,opacity:f32,normal:vec3f,tangent:vec3f,bitangent:vec3f)->Material {return Material(bsdf,emission,opacity,normal,tangent,bitangent,normal,-1.0,-1.0,vec3f(1),vec3f(1),5.0,0u,0u,0u);}
fn emissionFactor(m:Material,direction:vec3f)->vec3f {var factor=vec3f(1);if(m.emissionCone!=0u){let c=dot(normalize(m.emissionDirection),normalize(direction));if(m.emissionOuterCos>=m.emissionInnerCos){factor*=vec3f(select(0.0,1.0,c>=m.emissionInnerCos));}else{factor*=vec3f(smoothstep(m.emissionOuterCos,m.emissionInnerCos,c));}}if(m.emissionSchlick!=0u){let c=clamp(dot(normalize(m.emissionDirection),normalize(direction)),0.0,1.0);factor*=mix(m.emissionColor0,m.emissionColor90,vec3f(pow(1.0-c,max(.01,m.emissionExponent))));}if(m.emissionProfile!=0u){let axis=normalize(m.emissionDirection);let tangent=safeNormal(cross(axis,vec3f(0.0,0.0,1.0)),vec3f(1.0,0.0,0.0));let bitangent=normalize(cross(axis,tangent));let d=normalize(direction);let c=dot(axis,d);factor*=vec3f(measuredProfile(m.emissionProfile,c,dot(tangent,d),dot(bitangent,d)));}return factor;}
fn primaryLobe(surface:Material)->Lobe {
  var m=nativeDiffuse(vec3f(0),0,0);
  for(var i=0u;i<surface.bsdf.count;i++){if(closureImportance(surface.bsdf,i)>0.0){m=surface.bsdf.lobes[i];break;}}
  for(var i=0u;i<surface.bsdf.count;i++){if(surface.bsdf.lobes[i].transmission>0.0&&closureImportance(surface.bsdf,i)>0.0){m=surface.bsdf.lobes[i];break;}}
  m.emission=surface.emission;m.emissionWeight=1.0;return m;
}
`;

export const closureTransportWGSL = /* wgsl */`
fn closureTotal(c:Closure)->f32 {var total=0.0;for(var i=0u;i<c.count;i++){total+=closureImportance(c,i);}return total;}
fn closureRangeEval(c:Closure,start:u32,count:u32,wo:vec3f,wi:vec3f,eta:f32,wavelength:f32)->vec4f {var f=vec3f(0);var pdf=0.0;let end=min(c.count,start+count);for(var i=start;i<end;i++){let l=transportEval(c.lobes[i],wo,wi,eta,wavelength);f+=c.scales[i]*l.xyz;pdf+=closureImportance(c,i)/max(1e-30,closureTotal(c))*l.w;}return vec4f(f,pdf);}
fn closureRangeTransmission(c:Closure,start:u32,count:u32,wo:vec3f,eta:f32,wavelength:f32)->vec3f {var t=vec3f(0);let end=min(c.count,start+count);for(var i=start;i<end;i++){let m=c.lobes[i];var q=vec3f(0);if(m.kind==7u){q=m.weight*vec3f(1);}else if(m.kind==0u){q=(1.0-m.metal)*m.transmission*m.transmissionColor*transmissionAttenuation(m)*(1.0-dielectricFresnel(wo.z,m.ior));}else if(m.kind==1u&&m.scatterMode!=1u){q=m.weight*m.transmissionColor*transmissionAttenuation(m)*(1.0-dielectricFresnel(wo.z,m.ior));}t+=c.scales[i]*q;}return clamp(t,vec3f(0),vec3f(1));}
fn closureLayerAttenuation(c:Closure,selected:u32,wo:vec3f,eta:f32,wavelength:f32)->vec3f {var nodeTransmission:array<vec3f,32>;var attenuation=vec3f(1);for(var i=0u;i<c.nodeCount;i++){let n=c.nodes[i];var at=vec3f(0);var bt=vec3f(0);if(n.aKind==0u){at=closureRangeTransmission(c,n.aStart,n.aCount,wo,eta,wavelength);}else{at=nodeTransmission[n.a];}if(n.bKind==0u){bt=closureRangeTransmission(c,n.bStart,n.bCount,wo,eta,wavelength);}else{bt=nodeTransmission[n.b];}if(n.kind==3u){if(selected>=n.bStart&&selected<n.bStart+n.bCount){attenuation*=at;}nodeTransmission[i]=clamp(at*bt,vec3f(0),vec3f(1));}else{nodeTransmission[i]=clamp(at+bt,vec3f(0),vec3f(1));}}return attenuation;}
fn closureEval(c:Closure,wo:vec3f,wi:vec3f,eta:f32,wavelength:f32)->vec4f {
  var values:array<vec4f,32>;var transmission:array<vec3f,32>;
  for(var i=0u;i<c.nodeCount;i++) {let n=c.nodes[i];var a=vec4f(0);var b=vec4f(0);var at=vec3f(0);var bt=vec3f(0);
    if(n.aKind==0u){a=closureRangeEval(c,n.a,n.aCount,wo,wi,eta,wavelength);at=closureRangeTransmission(c,n.a,n.aCount,wo,eta,wavelength);}else{a=values[n.a];at=transmission[n.a];}
    if(n.bKind==0u){b=closureRangeEval(c,n.b,n.bCount,wo,wi,eta,wavelength);bt=closureRangeTransmission(c,n.b,n.bCount,wo,eta,wavelength);}else{b=values[n.b];bt=transmission[n.b];}
    if(n.kind==3u){let baseTransmission=select(vec3f(0),at,wi.z>0.0);values[i]=vec4f(a.xyz+b.xyz*baseTransmission,a.w+b.w);transmission[i]=clamp(at*bt,vec3f(0),vec3f(1));}
    else{values[i]=a+b;transmission[i]=clamp(at+bt,vec3f(0),vec3f(1));}
  }
  if(c.rootKind==0u){return closureRangeEval(c,c.root,c.count,wo,wi,eta,wavelength);}return values[c.root];
}
fn closureSample(c:Closure,wo:vec3f,eta:f32,rng:ptr<function,u32>,wavelength:f32)->Scatter {
  let total=closureTotal(c);if(total<=0.0){return Scatter(vec3f(0),0,vec3f(0),0u,1);}
  let threshold=random(rng)*total;var cumulative=0.0;var selected=0u;
  for(var i=0u;i<c.count;i++){cumulative+=closureImportance(c,i);if(threshold<cumulative){selected=i;break;}}
  let probability=closureImportance(c,selected)/total;
  var s=transportSample(c.lobes[selected],wo,eta,rng,wavelength);
  if(s.pdf<=0.0){return s;}
  if(s.delta!=0u){s.weight*=c.scales[selected]*select(vec3f(1),closureLayerAttenuation(c,selected,wo,eta,wavelength),c.rootKind!=0u)/probability;s.pdf*=probability;return s;}
  let f=closureEval(c,wo,s.wi,eta,wavelength);s.pdf=f.w;s.weight=f.xyz*abs(s.wi.z)/max(1e-30,f.w);return s;
}
fn validClosure(c:Closure)->bool {
  if(c.count>16u||c.nodeCount>32u){return false;}
  var interfaceIOR=0.0;
  for(var i=0u;i<c.count;i++){
    let l=c.lobes[i];
    if(!all(c.scales[i]>=vec3f(0))||!all(c.scales[i]<=vec3f(3e37))||!(l.weight>=0.0&&l.weight<=3e37&&l.ior>0.0&&l.ior<=3e37&&l.roughness>=0.0&&l.roughness<=3e37&&l.transmissionDepth>=0.0&&l.transmissionDepth<=3e37)||!all(l.alpha>=vec2f(0))||!all(l.alpha<=vec2f(3e37))){return false;}
    if(!all(l.base>=vec3f(0))||!all(l.base<=vec3f(3e37))||!all(l.specularColor>=vec3f(0))||!all(l.specularColor<=vec3f(3e37))||!all(l.transmissionColor>=vec3f(0))||!all(l.transmissionColor<=vec3f(3e37))||!all(l.transmissionScatter>=vec3f(0))||!all(l.transmissionScatter<=vec3f(3e37))||!all(l.schlickColor82>=vec3f(0))||!all(l.schlickColor82<=vec3f(3e37))||!all(l.schlickColor90>=vec3f(0))||!all(l.schlickColor90<=vec3f(3e37))||!(l.schlickExponent>=0.0&&l.schlickExponent<=3e37)||!all(l.complexIOR>=vec3f(0))||!all(l.complexIOR<=vec3f(3e37))||!all(l.extinction>=vec3f(0))||!all(l.extinction<=vec3f(3e37))||!(l.metal>=0.0&&l.metal<=1.0&&l.transmission>=0.0&&l.transmission<=1.0&&l.specularColorEnabled<=1u)){return false;}
    if(l.transmission>0.0&&closureImportance(c,i)>0.0){if(interfaceIOR>0.0&&abs(interfaceIOR-l.ior)>1e-5){return false;}interfaceIOR=l.ior;}
  }
  return true;
}
`;
