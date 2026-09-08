// SPDX-License-Identifier: Apache-2.0
import { compileGraph, contextWGSL, literal } from './graph.js';
import { packImages, imageWGSL, imageWGSLCompact } from './textures.js';
import { transportWGSL } from './transport.js';
import { pathStateWGSL } from './path-state.js';
import { spectrumWGSL } from './spectrum.js';
import { volumeWGSL } from './volume.js';
import { closureTransportWGSL } from './closures.js';

export function measuredProfileWGSL(materials) {
  const profiles = new Map(); let next = 1;
  for (const doc of materials) for (const [key, profile] of Object.entries(doc.measuredProfiles || {})) if (!profiles.has(key)) profiles.set(key, { id: next++, profile });
  const cases = [...profiles.values()].map(({ id, profile }) => {
    const vertical = profile.verticalAngles || profile.samples?.map(sample => sample[0]), horizontal = profile.horizontalAngles || [0], values = profile.values || profile.samples?.map(sample => sample[1]);
    if (!Array.isArray(vertical) || !Array.isArray(horizontal) || !Array.isArray(values) || vertical.length < 2 || vertical.length > 181 || horizontal.length < 1 || horizontal.length > 73 || values.length !== vertical.length * horizontal.length) throw new Error('Invalid measured EDF profile');
    if (vertical.some((v, i) => !Number.isFinite(v) || v < 0 || v > 180 || i && v <= vertical[i - 1]) || horizontal.some((v, i) => !Number.isFinite(v) || v < 0 || v > 360 || i && v <= horizontal[i - 1]) || values.some(v => !Number.isFinite(v) || v < 0)) throw new Error('Invalid measured EDF grid');
    const rows = horizontal.map((_, h) => {
      const row = values.slice(h * vertical.length, (h + 1) * vertical.length);
      const lines = [`fn measuredProfileV${id}_${h}(theta:f32)->f32{`];
      for (let v = 1; v < vertical.length; v++) { const a=vertical[v-1], b=vertical[v], va=row[v-1], vb=row[v]; lines.push(`if(theta<=${b}f){return mix(${va}f,${vb}f,(theta-${a}f)/max(1e-6,${b-a}f));}`); }
      lines.push(`return ${row.at(-1)}f;}`); return lines.join('');
    }).join('');
    const lines = [`case ${id}u:{let theta=degrees(acos(clamp(c,-1.0,1.0)));let az=degrees(atan2(py,px));if(az<0.0){az+=360.0;}`];
    if (horizontal.length === 1) lines.push(`return measuredProfileV${id}_0(theta);`);
    else {
      for (let h = 1; h < horizontal.length; h++) { const a=horizontal[h-1], b=horizontal[h]; lines.push(`if(az<=${b}f){return mix(measuredProfileV${id}_${h-1}(theta),measuredProfileV${id}_${h}(theta),(az-${a}f)/max(1e-6,${b-a}f));}`); }
      lines.push(`return measuredProfileV${id}_${horizontal.length-1}(theta);`);
    }
    return `${rows}${lines.join('')}}`;
  });
  // px/py are the azimuth components in a stable tangent frame around the
  // authored emission axis. Rotational profiles simply ignore them.
  return `fn measuredProfile(id:u32,c:f32,px:f32,py:f32)->f32{switch id{${cases.join('')}default:{return 1.0;}}}`;
}

export function shaderSource(materials, resources = {}, lighting = {}, textureOptions = {}) {
  for(const doc of materials)if(doc.twoSidedEmission!==undefined&&typeof doc.twoSidedEmission!=='boolean')throw new Error('twoSidedEmission must be boolean');
  const authoredDirectionalLights=lighting.directionalLights||[lighting.directional||{direction:[-.5,.8,.4],radiance:[3.5,3.2,2.8]}];
  if(!Array.isArray(authoredDirectionalLights)||authoredDirectionalLights.length<1||authoredDirectionalLights.length>256)throw new Error('Invalid authored directional-light list');
  const directionalLights=authoredDirectionalLights.map(light=>({direction:light.direction||[-.5,.8,.4],radiance:light.radiance||[3.5,3.2,2.8]}));
  for(const light of directionalLights)if(!Array.isArray(light.direction)||light.direction.length!==3||!light.direction.every(Number.isFinite)||Math.hypot(...light.direction)<1e-8||!Array.isArray(light.radiance)||light.radiance.length!==3||!light.radiance.every(v=>Number.isFinite(v)&&v>=0))throw new Error('Invalid authored directional light');
  for(const color of [lighting.environment])if(color && (!Array.isArray(color)||color.length!==3||color.some(v=>!Number.isFinite(v)||v<0)))throw new Error('Invalid light radiance');
  const pointLights=lighting.pointLights||[];
  if(!Array.isArray(pointLights)||pointLights.length>256)throw new Error('Invalid authored point-light list');
  for(const light of pointLights)if(!Array.isArray(light.position)||light.position.length!==3||!light.position.every(Number.isFinite)||!Array.isArray(light.radiance)||light.radiance.length!==3||!light.radiance.every(v=>Number.isFinite(v)&&v>=0)||!Number.isFinite(light.worldArea)||light.worldArea<=0||light.coneDirection&&(!Array.isArray(light.coneDirection)||light.coneDirection.length!==3||!light.coneDirection.every(Number.isFinite)||!Number.isFinite(light.coneInnerCos)||!Number.isFinite(light.coneOuterCos)))throw new Error('Invalid authored point light');
  const areaLights=lighting.areaLights||[];
  if(!Array.isArray(areaLights)||areaLights.length>256)throw new Error('Invalid authored area-light list');
  for(const light of areaLights)if(!Array.isArray(light.position)||light.position.length!==3||!light.position.every(Number.isFinite)||!Array.isArray(light.normal)||light.normal.length!==3||!light.normal.every(Number.isFinite)||!Array.isArray(light.radiance)||light.radiance.length!==3||!light.radiance.every(v=>Number.isFinite(v)&&v>=0)||!Number.isFinite(light.worldArea)||light.worldArea<=0||typeof light.twoSided!=='boolean')throw new Error('Invalid authored area light');
  resources.requiresPhysical = materials.some(doc => doc.mediumOutput || doc.nodes.some(n => ['transmission', 'transmission_weight'].some(k => n.inputs?.[k] && (n.inputs[k].value === undefined || Number(n.inputs[k].value) !== 0))));
  const images = materials.flatMap(doc => Object.values(doc.images || {}));
  const measuredProfileIds = Object.fromEntries([...new Set(materials.flatMap(doc => Object.keys(doc.measuredProfiles || {})))].map((key, i) => [key, i + 1]));
  const environmentImageIndex = lighting.environmentTexture ? images.length : -1;
  if (lighting.environmentTexture) images.push(lighting.environmentTexture);
  const packed = packImages(images, textureOptions); resources.imageData = packed.data;
  let imageIndex = 0;
  const functions = materials.map((doc, i) => {
    const imageDescriptors = Object.fromEntries(Object.entries(doc.images || {}).map(([name, image]) => [name, { ...packed.descriptors[imageIndex++], colorspace: image.colorspace || 'lin_rec709' }]));
    const uvIndex = Number.isInteger(doc.uvIndex) && doc.uvIndex >= 0 ? doc.uvIndex : 0;
    const c = compileGraph(doc, { material: true, imageDescriptors, output: doc.output, uvIndex, measuredProfileIds, geompropNames: doc.geompropNames || (doc.geompropName ? [doc.geompropName] : []) });
    if(c.categories.some(c=>['dielectric_bsdf','conductor_bsdf','oren_nayar_diffuse_bsdf','burley_diffuse_bsdf'].includes(c)))resources.requiresPhysical=true;
    if (!['surfaceshader', 'material'].includes(c.type)) throw new Error('Material graph must produce a surface');
    const medium = doc.mediumOutput ? compileGraph(doc, { output: doc.mediumOutput, imageDescriptors, uvIndex, geompropNames: doc.geompropNames || (doc.geompropName ? [doc.geompropName] : []) }) : null;
    if(medium && !['VDF', 'volumeshader'].includes(medium.type)) throw new Error('mediumOutput must produce VDF or volumeshader');
    if(medium?.volumeEmission && medium.categories.some(c=>['position','normal','tangent','bitangent','texcoord','image'].includes(c))) throw new Error('Spatially varying volume emission is not implemented');
    if(doc.mediumMajorant!==undefined && (!Number.isFinite(doc.mediumMajorant)||doc.mediumMajorant<=0))throw new Error('Medium majorant must be finite and positive');
    if(medium && medium.categories.some(c=>['position','normal','tangent','bitangent','texcoord','image'].includes(c)) && !doc.mediumMajorant) throw new Error('Spatially varying media require a conservative mediumMajorant');
    if(!medium&&c.interiorCategories.some(c=>['position','normal','tangent','bitangent','texcoord','image'].includes(c))&&!doc.mediumMajorant)throw new Error('Spatially varying layered media require a conservative mediumMajorant');
    return `fn material${i}(ctx: ShadingContext) -> Material { ${c.body}\nreturn ${c.expression}; }\nfn medium${i}(ctx:ShadingContext)->Medium { ${medium ? `${medium.body}\nreturn ${medium.expression};` : `return material${i}(ctx).bsdf.interior;`} }`;
  }).join('\n');
  const environmentImage = environmentImageIndex >= 0 ? packed.descriptors[environmentImageIndex] : null;
  if (environmentImage?.udim && (!Number.isInteger(environmentImage.udim.columns) || !Number.isInteger(environmentImage.udim.rows) || environmentImage.udim.columns < 1 || environmentImage.udim.rows < 1)) throw new Error('Invalid environment UDIM atlas descriptor');
  const environmentSample = environmentImage?.udim
    ? `imageSampleUDIM(${environmentImage.offset}u,vec2u(${environmentImage.width}u,${environmentImage.height}u),${environmentImage.levels}u,uv,vec2u(${environmentImage.udim.columns}u,${environmentImage.udim.rows}u),0.0,true,vec4f(0.0))`
    : environmentImage ? `imageSample(${environmentImage.offset}u,vec2u(${environmentImage.width}u,${environmentImage.height}u),${environmentImage.levels}u,uv,0.0,vec2u(2u,1u),true,vec4f(0.0))` : '';
  const directionalFns = directionalLights.map((light,i)=>`case ${i}u:{return normalize(${literal('vector3',light.direction)});}`).join('');
  const directionalRadianceFns = directionalLights.map((light,i)=>`case ${i}u:{return ${literal('color3',light.radiance)};}`).join('');
  const authoredPointDirect = pointLights.map(light => {
    const position=literal('vector3',light.position), radiance=literal('color3',light.radiance.map(v=>v*light.worldArea*.5));
    const cone=light.coneDirection ? `let coneCos=dot(${literal('vector3',light.coneDirection)},-wi);let coneWeight=smoothstep(${Number(light.coneOuterCos)},${Number(light.coneInnerCos)},coneCos);` : 'let coneWeight=1.0;';
    return `{let to=${position}-p;let d2=max(1e-8,dot(to,to));let dist=sqrt(d2);let wi=to/dist;${cone}if(dot(n,wi)>0.0&&coneWeight>0.0&&intersect(p+n*max(1e-4,length(p)*1e-5),wi).id==0xffffffffu){let f=bsdf(m,n,wo,wi);value+=f.xyz*max(0.0,dot(n,wi))*${radiance}*coneWeight/d2;}}`;
  }).join('');
  const authoredAreaDirect = areaLights.map(light => {
    const position=literal('vector3',light.position), normal=literal('vector3',light.normal), radiance=literal('color3',light.radiance.map(v=>v*light.worldArea));
    const facing=light.twoSided?'1.0':'max(0.0,dot('+normal+',-wi))';
    return `{let to=${position}-p;let d2=max(1e-8,dot(to,to));let dist=sqrt(d2);let wi=to/dist;let lightCos=${facing};if(dot(n,wi)>0.0&&lightCos>0.0&&intersect(p+n*max(1e-4,length(p)*1e-5),wi).id==0xffffffffu){let f=bsdf(m,n,wo,wi);value+=f.xyz*max(0.0,dot(n,wi))*${radiance}*lightCos/d2;}}`;
  }).join('');
  return /* wgsl */`
${contextWGSL}
${spectrumWGSL(materials, resources)}
${textureOptions.compact ? imageWGSLCompact : imageWGSL}
${functions}
${measuredProfileWGSL(materials)}
fn emissionSidedness(id:u32,normal:vec3f,direction:vec3f)->f32 {
  switch id { ${materials.map((doc,i)=>doc.twoSidedEmission?`case ${i}u:{return 1.0;}`:'').join('\n')} default:{} }
  return select(0.0,1.0,dot(normal,direction)<0.0);
}
struct Settings { origin: vec4f, forward: vec4f, right: vec4f, up: vec4f, dimensions: vec4u, display: vec4f, sampling:vec4u, animation:vec4f }
struct Node { lo: vec4f, hi: vec4f, link: vec4f }
struct Vertex { p: vec4f, n: vec4f, uv: vec4f, color: vec4f, tangent: vec4f, geomprop: vec4f, geomprop1: vec4f, geomprop2: vec4f, geomprop3: vec4f, geomprop4: vec4f, geomprop5: vec4f, geomprop6: vec4f, geomprop7: vec4f }
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
fn context(h: Hit, o: vec3f, d: vec3f, rayCone:f32) -> ShadingContext {
  let tri = triangles[h.id]; let w = 1.0-h.u-h.v;
  let n = normalize(tri.a.n.xyz*w+tri.b.n.xyz*h.u+tri.c.n.xyz*h.v);
  let p1=tri.b.p.xyz-tri.a.p.xyz; let p2=tri.c.p.xyz-tri.a.p.xyz;
  let uv1=tri.b.uv.xy-tri.a.uv.xy; let uv2=tri.c.uv.xy-tri.a.uv.xy;
  let frame=mxSurfaceFrame(n,p1,p2,uv1,uv2);
  let derivatives=mxSurfaceDerivatives(n,p1,p2,uv1,uv2);
  let uvScale=0.5*(length(uv1)/max(length(p1),1e-6)+length(uv2)/max(length(p2),1e-6));
  let pixelWorld=max(2.0*cfg.right.w/f32(cfg.dimensions.x),2.0*cfg.up.w/f32(cfg.dimensions.y))*max(h.t,1e-4);
  let footprint=max(1e-7,max(pixelWorld,rayCone*max(h.t,1e-4))*uvScale);
  let authoredT=tri.a.tangent*w+tri.b.tangent*h.u+tri.c.tangent*h.v;
  let hasT=length(authoredT.xyz)>1e-5;
  let t=normalize(authoredT.xyz-n*dot(n,authoredT.xyz));
  let tangent=select(frame[0],safeNormal(t,frame[0]),hasT);
  let handed=select(1.0,select(-1.0,1.0,authoredT.w>=0.0),hasT);
  let bitangent=select(frame[1],normalize(cross(n,tangent))*handed,hasT);
  return ShadingContext(o+d*h.t,n,tangent,bitangent,tri.a.uv.xy*w+tri.b.uv.xy*h.u+tri.c.uv.xy*h.v,cfg.animation.x,cfg.animation.y,vec2f(footprint,0),vec2f(0,footprint),derivatives[0],derivatives[1],-d,tri.a.color*w+tri.b.color*h.u+tri.c.color*h.v,tri.a.geomprop*w+tri.b.geomprop*h.u+tri.c.geomprop*h.v,tri.a.geomprop1*w+tri.b.geomprop1*h.u+tri.c.geomprop1*h.v,tri.a.geomprop2*w+tri.b.geomprop2*h.u+tri.c.geomprop2*h.v,tri.a.geomprop3*w+tri.b.geomprop3*h.u+tri.c.geomprop3*h.v,tri.a.geomprop4*w+tri.b.geomprop4*h.u+tri.c.geomprop4*h.v,tri.a.geomprop5*w+tri.b.geomprop5*h.u+tri.c.geomprop5*h.v,tri.a.geomprop6*w+tri.b.geomprop6*h.u+tri.c.geomprop6*h.v,tri.a.geomprop7*w+tri.b.geomprop7*h.u+tri.c.geomprop7*h.v);
}
fn getSurface(id: u32, ctx: ShadingContext) -> Material {
  switch id { ${materials.map((_, i) => `case ${i}u: { return material${i}(ctx); }`).join('\n')} default: { return material0(ctx); } }
}
fn getMaterial(id:u32,ctx:ShadingContext)->Lobe {return primaryLobe(getSurface(id,ctx));}
fn getMedium(id:u32,ctx:ShadingContext)->Medium {
  switch id { ${materials.map((_,i)=>`case ${i}u: {return medium${i}(ctx);}`).join('\n')} default:{return Medium(vec3f(0),vec3f(0),0,vec3f(0));} }
}
fn mediumMajorant(id:u32)->f32 {switch id {${materials.map((doc,i)=>`case ${i}u:{return ${literal('float',doc.mediumMajorant||0)};}`).join('\n')}default:{return 0.0;}}}
fn environment(d: vec3f) -> vec3f {
  ${environmentImage ? `let dir=safeNormal(d,vec3f(0.0,1.0,0.0));let uv=vec2f(fract(atan2(dir.z,dir.x)/(2.0*PI)+0.5),acos(clamp(dir.y,-1.0,1.0))/PI);return ${environmentSample}.rgb*${literal('color3',lighting.environmentTexture.scale||[1,1,1])}+${literal('color3',lighting.environment||[0,0,0])};` : ''}
  ${lighting.environment ? `return ${literal('color3',lighting.environment)};` : ''}
  let sky = mix(vec3f(0.12,0.15,0.2),vec3f(0.55,0.66,0.85),smoothstep(-0.1,0.9,d.y));
  return sky;
}
fn directionalCount()->u32{return ${directionalLights.length}u;}
fn directionalDirectionAt(i:u32)->vec3f{switch i{${directionalFns}default:{return vec3f(0,1,0);}}}
fn directionalRadianceAt(i:u32)->vec3f{switch i{${directionalRadianceFns}default:{return vec3f(0);}}}
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
  let f0 = select(mix(vec3f(pow((m.ior-1.0)/(m.ior+1.0),2.0)),m.base,m.metal),m.specularColor,m.specularColorEnabled!=0u);
  let f = fresnel(vh,f0); let D = distribution(nh,a2);
  let spec = f*D*masking(nv,a2)*masking(nl,a2)/(4.0*nl*nv);
  let diff = (1.0-f)*m.base*(1.0-m.metal)/PI;
  let prob = mix(0.25,0.9,m.metal); let pdf = prob*D*nh/(4.0*vh)+(1.0-prob)*nl/PI;
  return vec4f(spec+diff,pdf);
}
fn authoredPointDirect(m:Lobe,n:vec3f,wo:vec3f,p:vec3f)->vec3f {var value=vec3f(0);${authoredPointDirect}return value;}
fn authoredAreaDirect(m:Lobe,n:vec3f,wo:vec3f,p:vec3f)->vec3f {var value=vec3f(0);${authoredAreaDirect}return value;}
fn sampleDirection(m: Lobe, n: vec3f, wo: vec3f, rng: ptr<function,u32>) -> vec3f {
  if (random(rng) >= mix(0.25,0.9,m.metal)) { return cosine(n,rng); }
  let a = max(0.001,m.roughness*m.roughness); let u = random(rng); let phi = 2.0*PI*random(rng);
  let ct = sqrt((1.0-u)/(1.0+(a*a-1.0)*u)); let st = sqrt(max(0.0,1.0-ct*ct));
  return reflect(-wo,basis(n,vec3f(st*cos(phi),st*sin(phi),ct)));
}
fn preview(o0: vec3f, d0: vec3f, rng: ptr<function,u32>, realtime: bool) -> vec3f {
  var o = o0; var d = d0; var beta = vec3f(1); var radiance = vec3f(0);
  for (var bounce = 0u; bounce < 12u; bounce++) {
    let h = intersect(o,d);
    if (h.id == 0xffffffffu) { radiance += beta*environment(d); break; }
    var ctx = context(h,o,d,0.0); if (dot(ctx.normal,d)>0.0) { ctx.normal = -ctx.normal; }
    let surface = getSurface(u32(triangles[h.id].a.uv.z),ctx); let opacity=clamp(surface.opacity,0.0,1.0); if(opacity<=0.001){if(realtime){break;}o=ctx.position+d*max(1e-4,length(ctx.position)*1e-5);continue;} if(!realtime && opacity<1.0 && random(rng)>opacity){o=ctx.position+d*max(1e-4,length(ctx.position)*1e-5);continue;} let contributionOpacity=select(1.0,opacity,realtime); let geometricNormal=ctx.normal; ctx.normal=safeNormal(surface.normal,geometricNormal); if(dot(ctx.normal,geometricNormal)<0.0){ctx.normal=-ctx.normal;} if(dot(ctx.normal,d)>0.0){ctx.normal=-ctx.normal;} let m = primaryLobe(surface);
    let eps = max(1e-4,length(ctx.position)*1e-5);
    let emittingTriangle=triangles[h.id];let emittingNormal=normalize(cross(emittingTriangle.b.p.xyz-emittingTriangle.a.p.xyz,emittingTriangle.c.p.xyz-emittingTriangle.a.p.xyz));
    radiance += beta*m.emission*m.emissionWeight*emissionFactor(surface,-d)*emissionSidedness(u32(triangles[h.id].a.uv.z),emittingNormal,d)*contributionOpacity;
    var direct=vec3f(0);
    for(var directionalIndex=0u;directionalIndex<directionalCount();directionalIndex++){let light=directionalDirectionAt(directionalIndex);if(intersect(ctx.position+ctx.normal*eps,light).id==0xffffffffu){direct+=bsdf(m,ctx.normal,-d,light).xyz*max(0.0,dot(ctx.normal,light))*directionalRadianceAt(directionalIndex)*contributionOpacity;}}
    if (realtime) { direct += (authoredPointDirect(m,ctx.normal,-d,ctx.position)+authoredAreaDirect(m,ctx.normal,-d,ctx.position))*contributionOpacity; }
    radiance += beta*direct;
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
struct RasterVertex { @builtin(position) clip: vec4f, @location(0) position: vec3f, @location(1) normal: vec3f, @location(2) uv: vec2f, @location(3) color: vec4f, @location(4) tangent: vec4f, @location(5) geomprop: vec4f, @location(6) geomprop1: vec4f, @location(7) geomprop2: vec4f, @location(8) geomprop3: vec4f, @location(9) geomprop4: vec4f, @location(10) geomprop5: vec4f, @location(11) geomprop6: vec4f, @location(12) geomprop7: vec4f, @location(13) @interpolate(flat) material: u32 }
@vertex fn rasterVertex(@builtin(vertex_index) id: u32) -> RasterVertex {
  let tri = triangles[id/3u]; var v = tri.a;
  if (id%3u==1u) { v=tri.b; } else if (id%3u==2u) { v=tri.c; }
  let d = v.p.xyz-cfg.origin.xyz; let z = dot(d,cfg.forward.xyz);
  return RasterVertex(vec4f(dot(d,cfg.right.xyz)/cfg.right.w,dot(d,cfg.up.xyz)/cfg.up.w,1.00001*z-0.0100001,z),v.p.xyz,v.n.xyz,v.uv.xy,v.color,v.tangent,v.geomprop,v.geomprop1,v.geomprop2,v.geomprop3,v.geomprop4,v.geomprop5,v.geomprop6,v.geomprop7,u32(v.uv.z));
}
@fragment fn rasterFragment(v: RasterVertex, @builtin(front_facing) front: bool) -> @location(0) vec4f {
  let geomN = normalize(select(-v.normal,v.normal,front));
  let frame=mxSurfaceFrame(geomN,dpdx(v.position),dpdy(v.position),dpdx(v.uv),dpdy(v.uv));
  let derivatives=mxSurfaceDerivatives(geomN,dpdx(v.position),dpdy(v.position),dpdx(v.uv),dpdy(v.uv));
  let hasT=length(v.tangent.xyz)>1e-5;let t=safeNormal(v.tangent.xyz-geomN*dot(geomN,v.tangent.xyz),frame[0]);let handed=select(1.0,select(-1.0,1.0,v.tangent.w>=0.0),hasT);let bt=select(frame[1],normalize(cross(geomN,t))*handed,hasT);
  var ctx = ShadingContext(v.position,geomN,select(frame[0],t,hasT),bt,v.uv,cfg.animation.x,cfg.animation.y,dpdx(v.uv),dpdy(v.uv),derivatives[0],derivatives[1],normalize(cfg.origin.xyz-v.position),v.color,v.geomprop,v.geomprop1,v.geomprop2,v.geomprop3,v.geomprop4,v.geomprop5,v.geomprop6,v.geomprop7);
  let surface=getSurface(v.material,ctx); if(surface.opacity<=0.001){discard;} var n=safeNormal(surface.normal,geomN); if(dot(n,geomN)<0.0){n=-n;} ctx.normal=n; let m = primaryLobe(surface); let wo = normalize(cfg.origin.xyz-v.position);
  var color = m.emission*m.emissionWeight*emissionFactor(surface,wo)*emissionSidedness(v.material,geomN,-wo)+m.base*(1.0-m.metal)*0.22+fresnel(max(0.0,dot(n,wo)),mix(vec3f(0.04),m.base,m.metal))*environment(reflect(-wo,n));
  let transmission=clamp((1.0-m.metal)*m.transmission,0.0,1.0);
  let refracted=refract(-wo,n,1.0/max(1.0001,m.ior));
  color=mix(color,m.transmissionColor*environment(refracted),transmission)*clamp(surface.opacity,0.0,1.0);
  color += authoredPointDirect(m,n,wo,v.position)*clamp(surface.opacity,0.0,1.0);
  color += authoredAreaDirect(m,n,wo,v.position)*clamp(surface.opacity,0.0,1.0);
  for(var directionalIndex=0u;directionalIndex<directionalCount();directionalIndex++){let light=directionalDirectionAt(directionalIndex);if(intersect(v.position+geomN*max(1e-4,length(v.position)*1e-5),light).id==0xffffffffu){color+=bsdf(m,n,wo,light).xyz*max(0.0,dot(n,light))*directionalRadianceAt(directionalIndex)*clamp(surface.opacity,0.0,1.0);}}
  let linear = max(vec3f(0),color*exp2(cfg.display.x)); let mapped=linear/(1.0+linear);
  return vec4f(select(12.92*mapped,1.055*pow(mapped,vec3f(1.0/2.4))-0.055,mapped>vec3f(0.0031308)),1);
}
`;
}

export const displayShader = /* wgsl */`
struct Settings { origin: vec4f, forward: vec4f, right: vec4f, up: vec4f, dimensions: vec4u, display: vec4f, sampling:vec4u, animation:vec4f }
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
