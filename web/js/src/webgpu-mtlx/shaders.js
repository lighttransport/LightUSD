// SPDX-License-Identifier: Apache-2.0
import { compileGraph, contextWGSL, literal } from './graph.js';
import { packImages, imageWGSL } from './textures.js';
import { transportWGSL } from './transport.js';
import { pathStateWGSL } from './path-state.js';
import { spectrumWGSL } from './spectrum.js';
import { volumeWGSL } from './volume.js';
import { closureTransportWGSL } from './closures.js';

export function shaderSource(materials, resources = {}, lighting = {}, textureOptions = {}) {
  for(const doc of materials)if(doc.twoSidedEmission!==undefined&&typeof doc.twoSidedEmission!=='boolean')throw new Error('twoSidedEmission must be boolean');
  const lightDirection=lighting.directional?.direction||[-.5,.8,.4];
  if(!Array.isArray(lightDirection)||lightDirection.length!==3||Math.hypot(...lightDirection)<1e-8)throw new Error('Invalid directional light direction');
  for(const color of [lighting.environment,lighting.directional?.radiance])if(color && (!Array.isArray(color)||color.length!==3||color.some(v=>!Number.isFinite(v)||v<0)))throw new Error('Invalid light radiance');
  resources.requiresPhysical = materials.some(doc => doc.mediumOutput || doc.nodes.some(n => ['transmission', 'transmission_weight'].some(k => n.inputs?.[k] && (n.inputs[k].value === undefined || Number(n.inputs[k].value) !== 0))));
  const images = materials.flatMap(doc => Object.values(doc.images || {}));
  const packed = packImages(images, textureOptions); resources.imageData = packed.data;
  let imageIndex = 0;
  const functions = materials.map((doc, i) => {
    const imageDescriptors = Object.fromEntries(Object.entries(doc.images || {}).map(([name, image]) => [name, { ...packed.descriptors[imageIndex++], colorspace: image.colorspace || 'lin_rec709' }]));
    const c = compileGraph(doc, { material: true, imageDescriptors, output: doc.output });
    if(c.categories.some(c=>['dielectric_bsdf','conductor_bsdf','oren_nayar_diffuse_bsdf'].includes(c)))resources.requiresPhysical=true;
    if (!['surfaceshader', 'material'].includes(c.type)) throw new Error('Material graph must produce a surface');
    const medium = doc.mediumOutput ? compileGraph(doc, { output: doc.mediumOutput, imageDescriptors }) : null;
    if(medium && medium.type!=='VDF') throw new Error('mediumOutput must produce VDF');
    if(doc.mediumMajorant!==undefined && (!Number.isFinite(doc.mediumMajorant)||doc.mediumMajorant<=0))throw new Error('Medium majorant must be finite and positive');
    if(medium && medium.categories.some(c=>['position','normal','tangent','bitangent','texcoord','image'].includes(c)) && !doc.mediumMajorant) throw new Error('Spatially varying media require a conservative mediumMajorant');
    if(!medium&&c.interiorCategories.some(c=>['position','normal','tangent','bitangent','texcoord','image'].includes(c))&&!doc.mediumMajorant)throw new Error('Spatially varying layered media require a conservative mediumMajorant');
    return `fn material${i}(ctx: ShadingContext) -> Material { ${c.body}\nreturn ${c.expression}; }\nfn medium${i}(ctx:ShadingContext)->Medium { ${medium ? `${medium.body}\nreturn ${medium.expression};` : `return material${i}(ctx).bsdf.interior;`} }`;
  }).join('\n');
  return /* wgsl */`
${contextWGSL}
${spectrumWGSL(materials, resources)}
${imageWGSL}
${functions}
fn emissionSidedness(id:u32,normal:vec3f,direction:vec3f)->f32 {
  switch id { ${materials.map((doc,i)=>doc.twoSidedEmission?`case ${i}u:{return 1.0;}`:'').join('\n')} default:{} }
  return select(0.0,1.0,dot(normal,direction)<0.0);
}
struct Settings { origin: vec4f, forward: vec4f, right: vec4f, up: vec4f, dimensions: vec4u, display: vec4f, sampling:vec4u }
struct Node { lo: vec4f, hi: vec4f, link: vec4f }
struct Vertex { p: vec4f, n: vec4f, uv: vec4f }
struct Triangle { a: Vertex, b: Vertex, c: Vertex }
struct Hit { t: f32, u: f32, v: f32, id: u32 }
@group(0) @binding(0) var<uniform> cfg: Settings;
@group(0) @binding(1) var<storage,read> nodes: array<Node>;
@group(0) @binding(2) var<storage,read> triangles: array<Triangle>;
@group(0) @binding(3) var<storage,read_write> accumulation: array<vec4f>;
const PI = 3.141592653589793;
${transportWGSL}
${closureTransportWGSL}
${volumeWGSL}
${pathStateWGSL}
fn hash(v0: u32) -> u32 { var v = v0; v = (v ^ (v >> 16u)) * 0x7feb352du; v = (v ^ (v >> 15u)) * 0x846ca68bu; return v ^ (v >> 16u); }
fn random(state: ptr<function,u32>) -> f32 { *state = hash(*state + 0x9e3779b9u); return min(0.9999999403953552,(f32(*state >> 8u) + 0.5) / 16777216.0); }
fn intersect(o: vec3f, d: vec3f) -> Hit {
  var h = Hit(1e30,0,0,0xffffffffu); var ni = 0u;
  // Ray-aligned shear coordinates use the same edge endpoints on adjacent
  // triangles, avoiding cancellation cracks from separate Moller-Trumbore solves.
  // Algorithm: https://www.pbr-book.org/4ed/Shapes/Triangle_Meshes
  // Float32 WGSL only: this does not claim PBRT's double-precision edge fallback.
  let ad=abs(d);let kz=select(select(0u,1u,ad.y>ad.x),2u,ad.z>max(ad.x,ad.y));
  let kx=(kz+1u)%3u;let ky=(kx+1u)%3u;
  let shear=vec3f(-d[kx]/d[kz],-d[ky]/d[kz],1.0/d[kz]);
  let safeD = select(select(vec3f(-1e-20),vec3f(1e-20),d >= vec3f(0)),d,abs(d) > vec3f(1e-20));
  let inv = 1.0 / safeD;
  loop {
    if (ni >= arrayLength(&nodes)) { break; }
    let node = nodes[ni]; let a = (node.lo.xyz-o)*inv; let b = (node.hi.xyz-o)*inv;
    let near = min(a,b); let far = max(a,b);
    if (max(max(near.x,near.y),max(near.z,0.0)) > min(min(far.x,far.y),min(far.z,h.t))) { ni = u32(node.link.x); continue; }
    for (var j = 0u; j < u32(node.hi.w); j++) {
      let ti = u32(node.lo.w)+j; let tri = triangles[ti];
      let a0=tri.a.p.xyz-o;let b0=tri.b.p.xyz-o;let c0=tri.c.p.xyz-o;
      let a=vec3f(a0[kx]+shear.x*a0[kz],a0[ky]+shear.y*a0[kz],a0[kz]*shear.z);
      let b=vec3f(b0[kx]+shear.x*b0[kz],b0[ky]+shear.y*b0[kz],b0[kz]*shear.z);
      let c=vec3f(c0[kx]+shear.x*c0[kz],c0[ky]+shear.y*c0[kz],c0[kz]*shear.z);
      let edges=vec3f(b.x*c.y-b.y*c.x,c.x*a.y-c.y*a.x,a.x*b.y-a.y*b.x);
      if(any(edges<vec3f(0))&&any(edges>vec3f(0))){continue;}
      let det=edges.x+edges.y+edges.z;if(det==0.0){continue;}
      let t=dot(edges,vec3f(a.z,b.z,c.z))/det;
      if(t>1e-5&&t<h.t){h=Hit(t,edges.y/det,edges.z/det,ti);}
    }
    ni++;
  }
  return h;
}
fn context(h: Hit, o: vec3f, d: vec3f) -> ShadingContext {
  let tri = triangles[h.id]; let w = 1.0-h.u-h.v;
  let n = normalize(tri.a.n.xyz*w+tri.b.n.xyz*h.u+tri.c.n.xyz*h.v);
  let tangent = normalize(cross(select(vec3f(0,1,0),vec3f(1,0,0),abs(n.y)>0.9),n));
  let p1=tri.b.p.xyz-tri.a.p.xyz; let p2=tri.c.p.xyz-tri.a.p.xyz;
  let uv1=tri.b.uv.xy-tri.a.uv.xy; let uv2=tri.c.uv.xy-tri.a.uv.xy;
  let uvScale=0.5*(length(uv1)/max(length(p1),1e-6)+length(uv2)/max(length(p2),1e-6));
  let pixelWorld=max(2.0*cfg.right.w/f32(cfg.dimensions.x),2.0*cfg.up.w/f32(cfg.dimensions.y))*max(h.t,1e-4);
  let footprint=max(1e-7,pixelWorld*uvScale);
  return ShadingContext(o+d*h.t,n,tangent,cross(n,tangent),tri.a.uv.xy*w+tri.b.uv.xy*h.u+tri.c.uv.xy*h.v,0,0,vec2f(footprint,0),vec2f(0,footprint));
}
fn getSurface(id: u32, ctx: ShadingContext) -> Material {
  switch id { ${materials.map((_, i) => `case ${i}u: { return material${i}(ctx); }`).join('\n')} default: { return material0(ctx); } }
}
fn getMaterial(id:u32,ctx:ShadingContext)->Lobe {return primaryLobe(getSurface(id,ctx));}
fn getMedium(id:u32,ctx:ShadingContext)->Medium {
  switch id { ${materials.map((_,i)=>`case ${i}u: {return medium${i}(ctx);}`).join('\n')} default:{return Medium(vec3f(0),vec3f(0),0);} }
}
fn mediumMajorant(id:u32)->f32 {switch id {${materials.map((doc,i)=>`case ${i}u:{return ${literal('float',doc.mediumMajorant||0)};}`).join('\n')}default:{return 0.0;}}}
fn environment(d: vec3f) -> vec3f {
  ${lighting.environment ? `return ${literal('color3',lighting.environment)};` : ''}
  let sky = mix(vec3f(0.12,0.15,0.2),vec3f(0.55,0.66,0.85),smoothstep(-0.1,0.9,d.y));
  return sky;
}
fn directionalDirection()->vec3f{return normalize(${literal('vector3',lightDirection)});}
fn directionalRadiance()->vec3f{return ${literal('color3',lighting.directional?.radiance||[3.5,3.2,2.8])};}
fn basis(n: vec3f, v: vec3f) -> vec3f {
  let t = normalize(cross(select(vec3f(0,1,0),vec3f(1,0,0),abs(n.y)>0.9),n));
  return t*v.x+cross(n,t)*v.y+n*v.z;
}
fn cosine(n: vec3f, rng: ptr<function,u32>) -> vec3f {
  let r = sqrt(random(rng)); let phi = 2.0*PI*random(rng);
  return basis(n,vec3f(r*cos(phi),r*sin(phi),sqrt(max(0.0,1.0-r*r))));
}
fn fresnel(c: f32, f0: vec3f) -> vec3f { return f0+(1.0-f0)*pow(1.0-clamp(c,0.0,1.0),5.0); }
fn distribution(nh: f32, a2: f32) -> f32 { let d = nh*nh*(a2-1.0)+1.0; return a2/(PI*d*d); }
fn masking(nv: f32, a2: f32) -> f32 { return 2.0*nv/(nv+sqrt(a2+(1.0-a2)*nv*nv)); }
fn bsdf(m: Lobe, n: vec3f, wo: vec3f, wi: vec3f) -> vec4f {
  let nv = dot(n,wo); let nl = dot(n,wi); if (nv <= 0.0 || nl <= 0.0) { return vec4f(0); }
  let h = normalize(wo+wi); let nh = max(0.0,dot(n,h)); let vh = max(1e-7,dot(wo,h));
  let a = max(0.001,m.roughness*m.roughness); let a2 = a*a;
  let f0 = mix(vec3f(pow((m.ior-1.0)/(m.ior+1.0),2.0)),m.base,m.metal);
  let f = fresnel(vh,f0); let D = distribution(nh,a2);
  let spec = f*D*masking(nv,a2)*masking(nl,a2)/(4.0*nl*nv);
  let diff = (1.0-f)*m.base*(1.0-m.metal)/PI;
  let prob = mix(0.25,0.9,m.metal); let pdf = prob*D*nh/(4.0*vh)+(1.0-prob)*nl/PI;
  return vec4f(spec+diff,pdf);
}
fn sampleDirection(m: Lobe, n: vec3f, wo: vec3f, rng: ptr<function,u32>) -> vec3f {
  if (random(rng) >= mix(0.25,0.9,m.metal)) { return cosine(n,rng); }
  let a = max(0.001,m.roughness*m.roughness); let u = random(rng); let phi = 2.0*PI*random(rng);
  let ct = sqrt((1.0-u)/(1.0+(a*a-1.0)*u)); let st = sqrt(max(0.0,1.0-ct*ct));
  return reflect(-wo,basis(n,vec3f(st*cos(phi),st*sin(phi),ct)));
}
fn preview(o0: vec3f, d0: vec3f, rng: ptr<function,u32>, realtime: bool) -> vec3f {
  var o = o0; var d = d0; var beta = vec3f(1); var radiance = vec3f(0);
  let light = directionalDirection();
  for (var bounce = 0u; bounce < 12u; bounce++) {
    let h = intersect(o,d);
    if (h.id == 0xffffffffu) { radiance += beta*environment(d); break; }
    var ctx = context(h,o,d); if (dot(ctx.normal,d)>0.0) { ctx.normal = -ctx.normal; }
    let surface = getSurface(u32(triangles[h.id].a.uv.z),ctx); let opacity=clamp(surface.opacity,0.0,1.0); if(opacity<=0.001){if(realtime){break;}o=ctx.position+d*max(1e-4,length(ctx.position)*1e-5);continue;} if(!realtime && opacity<1.0 && random(rng)>opacity){o=ctx.position+d*max(1e-4,length(ctx.position)*1e-5);continue;} let contributionOpacity=select(1.0,opacity,realtime); let geometricNormal=ctx.normal; ctx.normal=normalize(surface.normal); if(dot(ctx.normal,geometricNormal)<0.0){ctx.normal=-ctx.normal;} if(dot(ctx.normal,d)>0.0){ctx.normal=-ctx.normal;} let m = primaryLobe(surface);
    let eps = max(1e-4,length(ctx.position)*1e-5);
    let emittingTriangle=triangles[h.id];let emittingNormal=normalize(cross(emittingTriangle.b.p.xyz-emittingTriangle.a.p.xyz,emittingTriangle.c.p.xyz-emittingTriangle.a.p.xyz));
    radiance += beta*m.emission*m.emissionWeight*emissionSidedness(u32(triangles[h.id].a.uv.z),emittingNormal,d)*contributionOpacity;
    let direct = bsdf(m,ctx.normal,-d,light).xyz * max(0.0,dot(ctx.normal,light))*directionalRadiance()*contributionOpacity;
    if (intersect(ctx.position+ctx.normal*eps,light).id == 0xffffffffu) { radiance += beta*direct; }
    if (realtime) { radiance += beta*(m.base*(1.0-m.metal)*0.22+fresnel(max(0.0,dot(ctx.normal,-d)),mix(vec3f(0.04),m.base,m.metal))*environment(reflect(d,ctx.normal)))*contributionOpacity; break; }
    let wi = sampleDirection(m,ctx.normal,-d,rng); let f = bsdf(m,ctx.normal,-d,wi);
    if (f.w <= 0.0) { break; }
    beta *= f.xyz*max(0.0,dot(ctx.normal,wi))/f.w;
    if (bounce >= 4u) { let p = clamp(max(max(beta.x,beta.y),beta.z),0.05,0.95); if (random(rng)>p) { break; } beta /= p; }
    o = ctx.position+ctx.normal*eps; d = wi;
  }
  return radiance;
}
@compute @workgroup_size(8,8) fn trace(@builtin(global_invocation_id) id: vec3u) {
  if (id.x>=cfg.dimensions.x || id.y>=cfg.dimensions.y) { return; }
  let index = id.y*cfg.dimensions.x+id.x;
  var rng = hash(index ^ hash(cfg.dimensions.z + 0x1234u) ^ hash(cfg.sampling.x));
  let pixel = vec2f(id.xy)+select(vec2f(random(&rng),random(&rng)),vec2f(0.5),cfg.dimensions.w==1u);
  let uv = pixel/vec2f(cfg.dimensions.xy)*2.0-1.0;
  let d = normalize(cfg.forward.xyz+cfg.right.xyz*uv.x*cfg.right.w-cfg.up.xyz*uv.y*cfg.up.w);
  let color = preview(cfg.origin.xyz,d,&rng,cfg.dimensions.w==1u);
  if (cfg.dimensions.z==0u || cfg.dimensions.w==1u) { accumulation[index] = vec4f(color,1); }
  else { accumulation[index] += vec4f(color,1); }
}
struct RasterVertex { @builtin(position) clip: vec4f, @location(0) position: vec3f, @location(1) normal: vec3f, @location(2) uv: vec2f, @location(3) @interpolate(flat) material: u32 }
@vertex fn rasterVertex(@builtin(vertex_index) id: u32) -> RasterVertex {
  let tri = triangles[id/3u]; var v = tri.a;
  if (id%3u==1u) { v=tri.b; } else if (id%3u==2u) { v=tri.c; }
  let d = v.p.xyz-cfg.origin.xyz; let z = dot(d,cfg.forward.xyz);
  return RasterVertex(vec4f(dot(d,cfg.right.xyz)/cfg.right.w,dot(d,cfg.up.xyz)/cfg.up.w,1.00001*z-0.0100001,z),v.p.xyz,v.n.xyz,v.uv.xy,u32(v.uv.z));
}
@fragment fn rasterFragment(v: RasterVertex, @builtin(front_facing) front: bool) -> @location(0) vec4f {
  let geomN = normalize(select(-v.normal,v.normal,front)); let tangent = normalize(cross(select(vec3f(0,1,0),vec3f(1,0,0),abs(geomN.y)>0.9),geomN));
  var ctx = ShadingContext(v.position,geomN,tangent,cross(geomN,tangent),v.uv,0,0,dpdx(v.uv),dpdy(v.uv));
  let surface=getSurface(v.material,ctx); if(surface.opacity<=0.001){discard;} var n=normalize(surface.normal); if(dot(n,geomN)<0.0){n=-n;} ctx.normal=n; let m = primaryLobe(surface); let wo = normalize(cfg.origin.xyz-v.position); let light=directionalDirection();
  var color = m.emission*m.emissionWeight*emissionSidedness(v.material,geomN,-wo)+m.base*(1.0-m.metal)*0.22+fresnel(max(0.0,dot(n,wo)),mix(vec3f(0.04),m.base,m.metal))*environment(reflect(-wo,n));
  let transmission=clamp((1.0-m.metal)*m.transmission,0.0,1.0);
  let refracted=refract(-wo,n,1.0/max(1.0001,m.ior));
  color=mix(color,m.transmissionColor*environment(refracted),transmission)*clamp(surface.opacity,0.0,1.0);
  if (intersect(v.position+geomN*max(1e-4,length(v.position)*1e-5),light).id==0xffffffffu) { color += bsdf(m,n,wo,light).xyz*max(0.0,dot(n,light))*directionalRadiance()*clamp(surface.opacity,0.0,1.0); }
  let linear = max(vec3f(0),color*exp2(cfg.display.x)); let mapped=linear/(1.0+linear);
  return vec4f(select(12.92*mapped,1.055*pow(mapped,vec3f(1.0/2.4))-0.055,mapped>vec3f(0.0031308)),1);
}
`;
}

export const displayShader = /* wgsl */`
struct Settings { origin: vec4f, forward: vec4f, right: vec4f, up: vec4f, dimensions: vec4u, display: vec4f, sampling:vec4u }
@group(0) @binding(0) var<uniform> cfg: Settings;
@group(0) @binding(1) var<storage,read> accumulation: array<vec4f>;
@vertex fn vertex(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { return vec4f(f32((i<<1u)&2u)*2.0-1.0,f32(i&2u)*2.0-1.0,0,1); }
@fragment fn fragment(@builtin(position) p: vec4f) -> @location(0) vec4f {
  let xy = min(vec2u(p.xy * vec2f(cfg.dimensions.xy)/cfg.display.yz),cfg.dimensions.xy-vec2u(1));
  let sum = accumulation[xy.y*cfg.dimensions.x+xy.x];
  var color=sum.xyz/max(1.0,sum.w);
  if(cfg.dimensions.w==2u) { color=mat3x3f(3.2404542,-0.9692660,0.0556434,-1.5371385,1.8760108,-0.2040259,-0.4985314,0.0415560,1.0572252)*color; }
  let linear = max(vec3f(0),color*exp2(cfg.display.x));
  let mapped = linear/(1.0+linear);
  let srgb = select(12.92*mapped,1.055*pow(mapped,vec3f(1.0/2.4))-0.055,mapped>vec3f(0.0031308));
  return vec4f(srgb,1);
}`;

export const blitShader = /* wgsl */`
@group(0) @binding(0) var image: texture_2d<f32>;
@group(0) @binding(1) var filtering: sampler;
struct V { @builtin(position) p: vec4f, @location(0) uv: vec2f }
@vertex fn vertex(@builtin(vertex_index) i: u32) -> V { let p = vec2f(f32((i<<1u)&2u),f32(i&2u)); return V(vec4f(p*2.0-1.0,0,1),vec2f(p.x,1.0-p.y)); }
@fragment fn fragment(v: V) -> @location(0) vec4f { return textureSample(image,filtering,v.uv); }
`;
