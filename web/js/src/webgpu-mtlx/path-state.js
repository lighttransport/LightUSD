// SPDX-License-Identifier: Apache-2.0
// A dispatch advances at most four vertices. Surviving paths are never truncated.
export const pathStateWGSL = /* wgsl */`
struct PathState { origin: vec4f, direction: vec4f, beta: vec4f, radiance: vec4f, state: vec4u, previous: vec4f, iors: vec4f, media: vec4u }
@group(0) @binding(5) var<storage,read_write> paths: array<PathState>;
@group(0) @binding(6) var<storage,read_write> pathCounters: array<atomic<u32>>;
@group(0) @binding(7) var<storage,read_write> moments: array<vec4f>;
fn triangleLightPDF(tri:Triangle,distance:f32,direction:vec3f)->f32 {
  let total=triangles[arrayLength(&triangles)-1u].b.p.w;
  let normal=normalize(cross(tri.b.p.xyz-tri.a.p.xyz,tri.c.p.xyz-tri.a.p.xyz));
  let cosine=abs(dot(normal,direction));
  if(total<=0.0||tri.c.p.w<=0.0||cosine<=1e-8){return 0.0;}
  return (tri.b.p.w-tri.a.p.w)/total*distance*distance/(tri.c.p.w*cosine);
}
fn finishPath(index: u32, p: ptr<function,PathState>) {
  var color=(*p).radiance.xyz;
  if(cfg.dimensions.w==2u) { color=cieAt((*p).previous.x)*(color.x*470.0/106.856915); }
  if(any(abs(color)>vec3f(3.0e37)) || any(color!=color)) { atomicAdd(&pathCounters[2],1u); (*p).state.w=1u; return; }
  let old=accumulation[index]; let count=old.w+1.0;
  let delta=color-old.xyz/max(1.0,old.w); let sum=old.xyz+color;
  moments[index]+=vec4f(delta*(color-sum/count),0);
  accumulation[index]=vec4f(sum,count); (*p).state.w=1u;
}
@compute @workgroup_size(8,8) fn tracePhysical(@builtin(global_invocation_id) id: vec3u) {
  if(any(id.xy>=cfg.dimensions.xy)) { return; }
  let index=id.y*cfg.dimensions.x+id.x;
  var p=paths[index];
  if(p.state.z!=u32(cfg.display.w) || p.previous.z!=f32(cfg.dimensions.z)) {
    if(cfg.dimensions.z==0u) { accumulation[index]=vec4f(0); moments[index]=vec4f(0); }
    var rng=hash(index^hash(cfg.dimensions.z+0x1234u)^hash(cfg.sampling.x));
    let pixel=vec2f(id.xy)+vec2f(random(&rng),random(&rng));
    let uv=pixel/vec2f(cfg.dimensions.xy)*2.0-1.0;
    let d=normalize(cfg.forward.xyz+cfg.right.xyz*uv.x*cfg.right.w-cfg.up.xyz*uv.y*cfg.up.w);
    p=PathState(vec4f(cfg.origin.xyz,0),vec4f(d,0),vec4f(1),vec4f(0),vec4u(rng,0u,u32(cfg.display.w),0u),vec4f(0,0,f32(cfg.dimensions.z),0),vec4f(1),vec4u(0));
    p.previous.x=360.0+470.0*random(&rng); p.state.x=rng;
  }
  if(p.state.w!=0u) { paths[index]=p; return; }
  var rng=p.state.x;
  for(var step=0u;step<4u;step++) {
    // Russian roulette normally bounds physical path depth after five events.
    // Keep a finite guard as a last resort for degenerate authored normals or
    // delta chains; this terminates a path, never a dispatch, and is reported
    // separately so reference runs can detect pathological materials.
    if(p.state.y>=64u){atomicAdd(&pathCounters[3],1u);finishPath(index,&p);break;}
    let h=intersect(p.origin.xyz,p.direction.xyz);
    let mediumDepth=u32(p.previous.w);
    var homogeneous=true;
    if(mediumDepth>0u && mediumMajorant(p.media[mediumDepth]-1u)>0.0) {
      homogeneous=false;
      if(cfg.dimensions.w!=2u){atomicAdd(&pathCounters[2],1u);p.state.w=1u;break;}
      let mediumID=p.media[mediumDepth]-1u;let majorant=mediumMajorant(mediumID);
      let distance=-log(1.0-random(&rng))/majorant;
      if(distance<h.t) {
        let point=p.origin.xyz+p.direction.xyz*distance;
        let ctx=ShadingContext(point,-p.direction.xyz,vec3f(1,0,0),vec3f(0,1,0),vec2f(0),0,0,vec2f(0),vec2f(0),vec3f(1,0,0),vec3f(0,1,0),-p.direction.xyz,vec4f(0,0,0,1));
        let medium=mediumAt(mediumID,ctx,p.previous.x,true);
        let sigmaA=medium.absorption.x;let sigmaS=medium.scattering.x;
        if(sigmaA<0.0||sigmaS<0.0||sigmaA+sigmaS>majorant||abs(medium.anisotropy)>=1.0){atomicAdd(&pathCounters[2],1u);p.state.w=1u;break;}
        let event=random(&rng)*majorant;
        p.origin=vec4f(point,0);
        if(event<sigmaA){finishPath(index,&p);break;}
        if(event<sigmaA+sigmaS){
          p.beta=vec4f(p.beta.xyz*(sigmaS/majorant),p.beta.w);
          let light=directionalDirection();
          let shadow=intersect(point+light*max(1e-5,length(point)*2e-6),light);
          if(shadow.id!=0xffffffffu && u32(triangles[shadow.id].a.uv.z)==mediumID){
            var lightColor=directionalRadiance();
            lightColor=vec3f(rgbSpectrum(lightColor,p.previous.x,true));
            let tr=exp(-vec3f(sigmaA+sigmaS)*shadow.t);
            p.radiance+=vec4f(p.beta.xyz*tr*hgPhase(dot(-p.direction.xyz,light),medium.anisotropy)*lightColor,0);
          }
          let ez=1.0-2.0*random(&rng); let ephi=2.0*PI*random(&rng); let er=sqrt(max(0.0,1.0-ez*ez));
          let envDirection=vec3f(er*cos(ephi),ez,er*sin(ephi));
          let envShadow=intersect(point+envDirection*max(1e-5,length(point)*2e-6),envDirection);
          if(envShadow.id!=0xffffffffu && u32(triangles[envShadow.id].a.uv.z)==mediumID){
            var envColor=environment(envDirection); envColor=vec3f(rgbSpectrum(envColor,p.previous.x,true));
            let tr=exp(-vec3f(sigmaA+sigmaS)*envShadow.t);
            p.radiance+=vec4f(p.beta.xyz*tr*hgPhase(dot(-p.direction.xyz,envDirection),medium.anisotropy)*envColor*(4.0*PI),0);
          }
          p.direction=vec4f(normalize(hgDirection(p.direction.xyz,medium.anisotropy,vec2f(random(&rng),random(&rng)))),0);p.state.y++;
          if(p.state.y>=5u){let survival=min(.95,max(.05,p.beta.x*p.beta.w));if(random(&rng)>=survival){finishPath(index,&p);break;}p.beta=vec4f(p.beta.xyz/survival,p.beta.w);}
        }
        continue;
      }
    }
    if(mediumDepth>0u && homogeneous) {
      let mediumContext=ShadingContext(p.origin.xyz,-p.direction.xyz,vec3f(1,0,0),vec3f(0,1,0),vec2f(0),0,0,vec2f(0),vec2f(0),vec3f(1,0,0),vec3f(0,1,0),-p.direction.xyz,vec4f(0,0,0,1));
      let medium=mediumAt(p.media[mediumDepth]-1u,mediumContext,p.previous.x,cfg.dimensions.w==2u);
      let sigmaT=medium.absorption+medium.scattering;
      if(any(medium.absorption<vec3f(0)) || any(medium.scattering<vec3f(0)) || abs(medium.anisotropy)>=1.0) { atomicAdd(&pathCounters[2],1u);p.state.w=1u;break; }
      let channel=min(2u,u32(random(&rng)*3.0));
      var distance=1e30; if(sigmaT[channel]>0.0) {distance=-log(1.0-random(&rng))/sigmaT[channel];}
      let traveled=min(distance,h.t);let tr=exp(-sigmaT*traveled);
      if(distance<h.t) {
        let pdf=dot(tr*sigmaT,vec3f(1.0/3.0));
        p.beta=vec4f(p.beta.xyz*tr*medium.scattering/max(1e-30,pdf),p.beta.w);
        if(all(p.beta.xyz<=vec3f(0))) {finishPath(index,&p);break;}
        p.origin=vec4f(p.origin.xyz+p.direction.xyz*distance,0);
        let light=directionalDirection();
        let shadow=intersect(p.origin.xyz+light*max(1e-5,length(p.origin.xyz)*2e-6),light);
        if(shadow.id!=0xffffffffu && u32(triangles[shadow.id].a.uv.z)==p.media[mediumDepth]-1u){
          var lightColor=directionalRadiance(); if(cfg.dimensions.w==2u){lightColor=vec3f(rgbSpectrum(lightColor,p.previous.x,true));}
          let tr=exp(-sigmaT*shadow.t);
          p.radiance+=vec4f(p.beta.xyz*tr*hgPhase(dot(-p.direction.xyz,light),medium.anisotropy)*lightColor,0);
        }
        let ez=1.0-2.0*random(&rng); let ephi=2.0*PI*random(&rng); let er=sqrt(max(0.0,1.0-ez*ez));
        let envDirection=vec3f(er*cos(ephi),ez,er*sin(ephi));
        let envShadow=intersect(p.origin.xyz+envDirection*max(1e-5,length(p.origin.xyz)*2e-6),envDirection);
        if(envShadow.id!=0xffffffffu && u32(triangles[envShadow.id].a.uv.z)==p.media[mediumDepth]-1u){
          var envColor=environment(envDirection); if(cfg.dimensions.w==2u){envColor=vec3f(rgbSpectrum(envColor,p.previous.x,true));}
          let tr=exp(-sigmaT*envShadow.t);
          p.radiance+=vec4f(p.beta.xyz*tr*hgPhase(dot(-p.direction.xyz,envDirection),medium.anisotropy)*envColor*(4.0*PI),0);
        }
        p.direction=vec4f(normalize(hgDirection(p.direction.xyz,medium.anisotropy,vec2f(random(&rng),random(&rng)))),0);
        p.state.y++;
        if(p.state.y>=5u) {
          let survival=min(.95,max(.05,max(p.beta.x,max(p.beta.y,p.beta.z))*p.beta.w));
          if(random(&rng)>=survival) {finishPath(index,&p);break;}p.beta=vec4f(p.beta.xyz/survival,p.beta.w);
        }
        continue;
      }
      p.beta=vec4f(p.beta.xyz*tr/max(1e-30,dot(tr,vec3f(1.0/3.0))),p.beta.w);
    }
    if(h.id==0xffffffffu) {
      var mis=1.0; if(p.state.y>0u && p.direction.w>0.0) { mis=powerHeuristic(p.direction.w,1.0/(4.0*PI)); }
      var sky=environment(p.direction.xyz); if(cfg.dimensions.w==2u) { sky=vec3f(rgbSpectrum(sky,p.previous.x,true)); }
      p.radiance+=vec4f(p.beta.xyz*sky*mis,0); finishPath(index,&p); break;
    }
    let tri=triangles[h.id]; let materialID=u32(tri.a.uv.z);
    var ctx=context(h,p.origin.xyz,p.direction.xyz);
    let outward=normalize(cross(tri.b.p.xyz-tri.a.p.xyz,tri.c.p.xyz-tri.a.p.xyz));
    let entering=dot(outward,p.direction.xyz)<0.0;
    let gn=select(-outward,outward,entering);
    // Geometric normals avoid shading-normal energy leaks at dielectric boundaries.
    ctx.normal=gn;
    let uvFrame=mxSurfaceFrame(gn,tri.b.p.xyz-tri.a.p.xyz,tri.c.p.xyz-tri.a.p.xyz,tri.b.uv.xy-tri.a.uv.xy,tri.c.uv.xy-tri.a.uv.xy);
    ctx.tangent=uvFrame[0];ctx.bitangent=uvFrame[1];
    var surface=getSurface(materialID,ctx);
    if(cfg.dimensions.w==2u) { surface=spectralSurface(surface,materialID,p.previous.x); }
    // MaterialX normal/normalmap outputs are evaluated after geometric
    // orientation and before transport. Keep the geometric normal for ray
    // offsets while using the authored normal for the local BSDF frame.
    ctx.normal=safeNormal(surface.normal,gn);
    if(dot(ctx.normal,gn)<0.0){ctx.normal=-ctx.normal;}
    if(dot(ctx.normal,p.direction.xyz)>0.0){ctx.normal=-ctx.normal;}
    // MaterialX opacity is a cutout/transmittance factor. Stochastic
    // continuation keeps fractional opacity unbiased without a second shading
    // event for the transparent branch.
    let surfaceOpacity=surface.opacity;
    if(!(surfaceOpacity>0.0)||surfaceOpacity<1.0){
      if(!(surfaceOpacity>0.0)||random(&rng)>=surfaceOpacity){p.origin=vec4f(ctx.position+p.direction.xyz*max(1e-5,length(ctx.position)*2e-6),0);continue;}
      // Coverage and branch probability are both opacity; their ratio is one.
      // Dividing beta by opacity here would over-brighten accepted surfaces.
    }
    let m=primaryLobe(surface);
    if(!validClosure(surface.bsdf)){atomicAdd(&pathCounters[2],1u);p.state.w=1u;break;}
    if(m.ior<=0.0 || m.roughness<0.0 || m.weight<0.0 || any(m.alpha<vec2f(0)) || any(m.complexIOR<vec3f(0)) || any(m.extinction<vec3f(0)) || m.metal<0.0 || m.metal>1.0 || m.transmission<0.0 || m.transmission>1.0 || any(m.base<vec3f(0)) || any(m.transmissionColor<vec3f(0))) {
      atomicAdd(&pathCounters[2],1u); p.state.w=1u; break;
    }
    let depth=u32(p.previous.w); var etaI=p.iors[depth]; var etaT=m.ior;
    if(!entering) {
      if(depth==0u) { etaI=m.ior; etaT=1.0; }
      else { etaT=p.iors[depth-1u]; }
    }
    let eta=select(etaT/etaI,m.ior,m.thinWalled!=0u); let frame=transportFrame(ctx.normal); let wo=transpose(frame)*(-p.direction.xyz);
    var emitterMIS=1.0;
    if(p.state.y>0u && p.direction.w>0.0){emitterMIS=powerHeuristic(p.direction.w,triangleLightPDF(tri,h.t,p.direction.xyz));}
    p.radiance+=vec4f(p.beta.xyz*m.emission*m.emissionWeight*emitterMIS*emissionSidedness(materialID,outward,p.direction.xyz),0);
    let eps=max(1e-5,length(ctx.position)*2e-6);
    let light=directionalDirection();
    let lightLocal=transpose(frame)*light;
    let direct=closureEval(surface.bsdf,wo,lightLocal,eta);
    let lightOrigin=ctx.position+gn*select(-eps,eps,lightLocal.z>0.0);
    var lightColor=directionalRadiance(); if(cfg.dimensions.w==2u) { lightColor=vec3f(rgbSpectrum(lightColor,p.previous.x,true)); }
    if(intersect(lightOrigin,light).id==0xffffffffu) { p.radiance+=vec4f(p.beta.xyz*direct.xyz*abs(lightLocal.z)*lightColor,0); }
    // Uniform environment sampling plus power-heuristic BSDF sampling.
    let z=1.0-2.0*random(&rng); let phi=2.0*PI*random(&rng); let rr=sqrt(max(0.0,1.0-z*z));
    let envDirection=vec3f(rr*cos(phi),z,rr*sin(phi)); let envLocal=transpose(frame)*envDirection;
    let f=closureEval(surface.bsdf,wo,envLocal,eta); let envPDF=1.0/(4.0*PI);
    let envOrigin=ctx.position+gn*select(-eps,eps,envLocal.z>0.0);
    if(f.w>0.0 && intersect(envOrigin,envDirection).id==0xffffffffu) {
      var sky=environment(envDirection); if(cfg.dimensions.w==2u) { sky=vec3f(rgbSpectrum(sky,p.previous.x,true)); }
      p.radiance+=vec4f(p.beta.xyz*f.xyz*abs(envLocal.z)*sky*(powerHeuristic(envPDF,f.w)/envPDF),0);
    }
    // Area-weighted triangle sampling includes all geometry, so graph-driven
    // emission cannot be accidentally omitted from the light distribution.
    let totalArea=triangles[arrayLength(&triangles)-1u].b.p.w;
    if(totalArea>0.0) {
      let threshold=random(&rng)*totalArea;var lo=0u;var hi=arrayLength(&triangles)-1u;
      loop{if(lo>=hi){break;}let mid=(lo+hi)/2u;if(triangles[mid].b.p.w<=threshold){lo=mid+1u;}else{hi=mid;}}
      let emitter=triangles[lo];let r=sqrt(random(&rng));let b1=r*(1.0-random(&rng));let b2=r-b1;
      let point=emitter.a.p.xyz*(1.0-r)+emitter.b.p.xyz*b1+emitter.c.p.xyz*b2;
      let delta=point-ctx.position;let distance=length(delta);
      if(distance>eps && lo!=h.id) {
        let direction=delta/distance;let local=transpose(frame)*direction;
        let pdf=triangleLightPDF(emitter,distance,direction);
        let ev=closureEval(surface.bsdf,wo,local,eta);
        let lightCtx=context(Hit(distance,b1,b2,lo),ctx.position,direction);
        var emission=getMaterial(u32(emitter.a.uv.z),lightCtx);
        if(cfg.dimensions.w==2u){emission=spectralMaterial(emission,u32(emitter.a.uv.z),p.previous.x);}
        let emitterNormal=normalize(cross(emitter.b.p.xyz-emitter.a.p.xyz,emitter.c.p.xyz-emitter.a.p.xyz));
        emission.emission*=emissionSidedness(u32(emitter.a.uv.z),emitterNormal,direction);
        if(pdf>0.0 && ev.w>0.0 && any(emission.emission*emission.emissionWeight>vec3f(0))) {
          let shadow=intersect(ctx.position+gn*select(-eps,eps,local.z>0.0),direction);
          if(shadow.id==lo||shadow.t>=distance-eps*2.0){p.radiance+=vec4f(p.beta.xyz*ev.xyz*abs(local.z)*emission.emission*emission.emissionWeight*(powerHeuristic(pdf,ev.w)/pdf),0);}
        }
      }
    }
    let sample=closureSample(surface.bsdf,wo,eta,&rng);
    if(sample.pdf<=0.0 || dot(sample.wi,sample.wi)<0.5 || all(sample.weight<=vec3f(0))) { finishPath(index,&p); break; }
    p.beta=vec4f(p.beta.xyz*sample.weight,p.beta.w*sample.eta*sample.eta);
    if(sample.wi.z<0.0 && m.thinWalled==0u) {
      if(entering) {
        if(depth>=3u) { atomicAdd(&pathCounters[1],1u); p.state.w=1u; break; }
        p.previous.w=f32(depth+1u); p.iors[depth+1u]=m.ior; p.media[depth+1u]=materialID+1u;
      } else if(depth>0u) {
        if(p.media[depth]!=materialID+1u) { atomicAdd(&pathCounters[1],1u); p.state.w=1u; break; }
        p.previous.w=f32(depth-1u);
      }
    }
    p.state.y++;
    if(p.state.y>=5u) {
      let rrBeta=p.beta.xyz*p.beta.w; let survival=min(0.95,max(0.05,max(rrBeta.x,max(rrBeta.y,rrBeta.z))));
      if(random(&rng)>=survival) { finishPath(index,&p); break; } p.beta=vec4f(p.beta.xyz/survival,p.beta.w);
    }
    p.origin=vec4f(ctx.position+gn*select(-eps,eps,sample.wi.z>0.0),0);
    p.direction=vec4f(normalize(frame*sample.wi),select(sample.pdf,0.0,sample.delta!=0u));
  }
  p.state.x=rng; paths[index]=p;
  if(p.state.w==0u) { atomicAdd(&pathCounters[0],1u); }
}
`;
