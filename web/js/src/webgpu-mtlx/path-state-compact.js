// SPDX-License-Identifier: Apache-2.0
// Diagnostic physical kernel. This deliberately performs one bounce so that
// shader compiler investigations have a small, physically meaningful target.
export const pathStateCompactWGSL = /* wgsl */`
struct PathState { origin: vec4f, direction: vec4f, beta: vec4f, radiance: vec4f, state: vec4u, previous: vec4f, iorsLo: vec4f, iorsHi: vec4f, mediaLo: vec4u, mediaHi: vec4u }
@group(0) @binding(5) var<storage,read_write> paths: array<PathState>;
@group(0) @binding(6) var<storage,read_write> pathCounters: array<atomic<u32>>;
@group(0) @binding(7) var<storage,read_write> moments: array<vec4f>;
@compute @workgroup_size(8,8) fn tracePhysical(@builtin(global_invocation_id) id: vec3u) {
  if(any(id.xy>=cfg.dimensions.xy)){return;}
  let index=id.y*cfg.dimensions.x+id.x;
  var p=paths[index];
  var rng=hash(index^hash(cfg.dimensions.z+0x1234u)^hash(cfg.sampling.x));
  let pixel=vec2f(id.xy)+vec2f(random(&rng),random(&rng));
  let uv=pixel/vec2f(cfg.dimensions.xy)*2.0-1.0;
  let d=normalize(cfg.forward.xyz+cfg.right.xyz*uv.x*cfg.right.w-cfg.up.xyz*uv.y*cfg.up.w);
  p=PathState(vec4f(cfg.origin.xyz,0),vec4f(d,0),vec4f(1),vec4f(0),vec4u(rng,0u,u32(cfg.display.w),0u),vec4f(0,0,f32(cfg.dimensions.z),0),vec4f(1),vec4f(1),vec4u(0),vec4u(0));
  let h=intersect(p.origin.xyz,p.direction.xyz);
  if(h.id==0xffffffffu){p.radiance+=vec4f(environment(p.direction.xyz),0);}
  else {
    let ctx=context(h,p.origin.xyz,p.direction.xyz,0.0);
    let surface=getSurface(u32(triangles[h.id].a.uv.z),ctx);
    let m=primaryLobe(surface);
    let frame=transportFrame(ctx.normal,surface.tangent,surface.bitangent);
    let wo=transpose(frame)*(-p.direction.xyz);
    p.radiance+=vec4f(m.emission*m.emissionWeight,0);
    let z=1.0-2.0*random(&rng);let phi=2.0*PI*random(&rng);let rr=sqrt(max(0.0,1.0-z*z));
    let wi=vec3f(rr*cos(phi),z,rr*sin(phi));let local=transpose(frame)*wi;
    let f=closureEval(surface.bsdf,wo,local,m.ior,0.0);let pdf=1.0/(4.0*PI);
    if(f.w>0.0&&intersect(ctx.position+ctx.normal*1e-5,wi).id==0xffffffffu){p.radiance+=vec4f(f.xyz*abs(local.z)*environment(wi)/pdf,0);}
    let sample=closureSample(surface.bsdf,wo,m.ior,&rng,0.0);
    if(sample.pdf>0.0){p.beta=vec4f(p.beta.xyz*sample.weight,p.beta.w*sample.eta*sample.eta);p.direction=vec4f(normalize(frame*sample.wi),sample.pdf);p.origin=vec4f(ctx.position,0);}
  }
  accumulation[index]=vec4f(p.radiance.xyz,1.0);
  p.state.x=rng;p.state.w=1u;
  paths[index].origin=p.origin;paths[index].direction=p.direction;paths[index].beta=p.beta;paths[index].radiance=p.radiance;paths[index].state=p.state;paths[index].previous=p.previous;paths[index].iorsLo=p.iorsLo;paths[index].iorsHi=p.iorsHi;paths[index].mediaLo=p.mediaLo;paths[index].mediaHi=p.mediaHi;
}`;
