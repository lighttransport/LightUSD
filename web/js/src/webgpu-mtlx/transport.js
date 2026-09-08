// SPDX-License-Identifier: Apache-2.0
// Microfacet transport equations: PBRT 4e, sections 9.4 and 9.7.
// https://www.pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF
// Fresh WGSL implementation; visible-normal sampling in stretched space.
export const transportWGSL = /* wgsl */`
struct Scatter { wi: vec3f, pdf: f32, weight: vec3f, delta: u32, eta: f32 }
fn dielectricFresnel(c0: f32, eta: f32) -> f32 {
  if(eta==1.0){return 0.0;}
  let c=clamp(abs(c0),0.0,1.0);
  let st2=(1.0-c*c)/(eta*eta);
  if(st2>=1.0) { return 1.0; }
  let ct=sqrt(max(0.0,1.0-st2));
  let rp=(eta*c-ct)/(eta*c+ct); let rs=(c-eta*ct)/(c+eta*ct);
  return 0.5*(rp*rp+rs*rs);
}
fn conductorFresnel(c0: f32, eta: vec3f, k: vec3f) -> vec3f {
  let c=clamp(abs(c0),0.0,1.0); let c2=c*c; let s2=1.0-c2;
  let e2=eta*eta; let k2=k*k; let t0=e2-k2-vec3f(s2);
  let a2b2=sqrt(t0*t0+4.0*e2*k2); let a=sqrt(max(vec3f(0),0.5*(a2b2+t0)));
  let rs=(a2b2+vec3f(c2)-2.0*c*a)/(a2b2+vec3f(c2)+2.0*c*a);
  let rp=rs*(c2*a2b2+vec3f(s2*s2)-2.0*c*a*s2)/(c2*a2b2+vec3f(s2*s2)+2.0*c*a*s2);
  return 0.5*(rp+rs);
}
fn thinFilmFresnel(c0:f32,baseIOR:f32,filmIOR:f32,thickness:f32)->vec3f {
  let c=clamp(abs(c0),0.0,1.0);let phase=4.0*PI*filmIOR*max(0.0,thickness)*c;
  let r01=(1.0-filmIOR)/(1.0+filmIOR);let r12=(filmIOR-baseIOR)/(filmIOR+baseIOR);
  let wavelengths=vec3f(650.0,510.0,475.0);let interference=2.0*r01*r12*cos(phase*1e-3*wavelengths);
  return clamp(vec3f(r01*r01)+vec3f(r12*r12)+vec3f(interference),vec3f(0),vec3f(1));
}
fn transmissionAttenuation(m:Lobe)->vec3f { return exp(-max(vec3f(0),m.transmissionScatter)*max(0.0,m.transmissionDepth)); }
fn mxPow6(v:f32)->f32 { let v2=v*v; return v2*v2*v2; }
fn generalizedSchlickFresnel(m:Lobe,c:f32)->vec3f { let x=clamp(abs(c),0.0,1.0);let maxCos=1.0/7.0;let factor=1.0/(maxCos*pow(1.0-maxCos,6.0));let a=mix(m.base,m.schlickColor90,vec3f(pow(1.0-maxCos,m.schlickExponent)))*(vec3f(1)-m.schlickColor82)*factor;return mix(m.base,m.schlickColor90,vec3f(pow(1.0-x,m.schlickExponent)))-a*x*vec3f(mxPow6(1.0-x)); }
fn microfacetD(h: vec3f, alpha: vec2f) -> f32 {
  if(h.z<=0.0) { return 0.0; }
  let q=dot(h.xy/alpha,h.xy/alpha)+h.z*h.z;
  return 1.0/(PI*alpha.x*alpha.y*q*q);
}
fn microfacetLambda(w: vec3f, alpha: vec2f) -> f32 {
  let a=dot(w.xy*alpha,w.xy*alpha)/max(1e-30,w.z*w.z);
  return 0.5*(sqrt(1.0+a)-1.0);
}
fn microfacetG1(w: vec3f, alpha: vec2f) -> f32 { return 1.0/(1.0+microfacetLambda(w,alpha)); }
fn microfacetG(wo: vec3f, wi: vec3f, alpha: vec2f) -> f32 { return 1.0/(1.0+microfacetLambda(wo,alpha)+microfacetLambda(wi,alpha)); }
fn visibleNormalPDF(wo: vec3f, h: vec3f, alpha: vec2f) -> f32 { return microfacetD(h,alpha)*microfacetG1(wo,alpha)*max(0.0,dot(wo,h))/max(1e-30,abs(wo.z)); }
fn visibleNormal(wo: vec3f, alpha: vec2f, u: vec2f) -> vec3f {
  let v=normalize(vec3f(alpha*wo.xy,wo.z));
  var t1=vec3f(1,0,0); if(v.z<0.99999) { t1=normalize(cross(vec3f(0,0,1),v)); }
  let t2=cross(v,t1); let r=sqrt(u.x); let phi=2.0*PI*u.y;
  let x=r*cos(phi); var y=r*sin(phi); let s=0.5*(1.0+v.z);
  y=mix(sqrt(max(0.0,1.0-x*x)),y,s);
  let nh=x*t1+y*t2+sqrt(max(0.0,1.0-x*x-y*y))*v;
  return normalize(vec3f(alpha*nh.xy,max(1e-8,nh.z)));
}
fn dielectricEval(wo: vec3f, wi: vec3f, alpha: vec2f, eta: f32) -> vec2f {
  if(wo.z<=0.0 || wi.z==0.0 || eta==1.0) { return vec2f(0); }
  let reflection=wi.z>0.0;
  let sum=wo+wi*select(eta,1.0,reflection);
  if(dot(sum,sum)<1e-20) { return vec2f(0); }
  var h=normalize(sum); if(h.z<0.0) { h=-h; }
  let oh=dot(wo,h); let ih=dot(wi,h);
  if(oh<=0.0 || ih*wi.z<=0.0) { return vec2f(0); }
  let f=dielectricFresnel(oh,eta); let d=microfacetD(h,alpha); let g=microfacetG(wo,wi,alpha);
  let p=visibleNormalPDF(wo,h,alpha);
  if(reflection) { return vec2f(f*d*g/(4.0*wo.z*wi.z),p*f/(4.0*oh)); }
  let den=ih+oh/eta; let den2=den*den;
  if(den2<1e-30) { return vec2f(0); }
  return vec2f((1.0-f)*d*g*abs(ih*oh/(wo.z*wi.z*den2))/(eta*eta),p*(1.0-f)*abs(ih)/den2);
}
fn transportFrame(n: vec3f) -> mat3x3f {
  let t=normalize(cross(select(vec3f(0,1,0),vec3f(1,0,0),abs(n.y)>0.9),n));
  return mat3x3f(t,cross(n,t),n);
}
fn transportAlpha(m: Lobe) -> vec2f {
  let a=max(0.001,m.roughness*m.roughness);
  let aspect=sqrt(1.0-0.9*clamp(m.anisotropy,0.0,1.0));
  return vec2f(a/aspect,a*aspect);
}
// A convex mixture of opaque metal/dielectric and transmissive dielectric.
// Standard Surface layering and multiple-scattering compensation are separate.
fn transportEval(m: Lobe, wo: vec3f, wi: vec3f, eta: f32) -> vec4f {
  if(wo.z<=0.0 || wi.z==0.0) { return vec4f(0); }
  if(m.kind!=0u){return nativeEval(m,wo,wi,eta);}
  let alpha=transportAlpha(m); let t=(1.0-m.metal)*m.transmission;
  let opaque=1.0-t; let specProbability=clamp(m.weight*mix(0.5,1.0,m.metal),0.0,1.0);
  var value=vec3f(0); var pdf=0.0;
  if(wi.z>0.0) {
    let h=normalize(wo+wi); let oh=max(1e-20,dot(wo,h));
    let fr=dielectricFresnel(oh,eta);
    let baseFresnel=mix(vec3f(fr),fresnel(oh,m.base),m.metal);
    let f=select(baseFresnel,thinFilmFresnel(oh,m.ior,m.thinFilmIOR,m.thinFilmThickness),m.thinFilmThickness>0.0);
    let spec=f*microfacetD(h,alpha)*microfacetG(wo,wi,alpha)/(4.0*wo.z*wi.z);
    let diff=(1.0-m.metal)*(1.0-fr)*m.base/PI;
    value=opaque*(m.weight*m.schlickColor90*spec+diff);
    pdf=opaque*(specProbability*visibleNormalPDF(wo,h,alpha)/(4.0*oh)+(1.0-specProbability)*wi.z/PI);
  }
  if(t>0.0 && m.roughness>0.0001 && eta!=1.0) {
    let glass=dielectricEval(wo,wi,alpha,eta);
    value+=t*glass.x*select(m.transmissionColor*transmissionAttenuation(m),vec3f(1),wi.z>0.0); pdf+=t*glass.y;
  }
  return vec4f(value,pdf);
}
fn transportSample(m: Lobe, wo: vec3f, eta: f32, rng: ptr<function,u32>) -> Scatter {
  if(m.kind!=0u){return nativeSample(m,wo,eta,rng);}
  var wi=vec3f(0); let t=(1.0-m.metal)*m.transmission;
  let glass=random(rng)<t;
  let alpha=transportAlpha(m);
  if(glass && (m.roughness<=0.0001 || eta==1.0)) {
    let f=dielectricFresnel(wo.z,eta);
    if(random(rng)<f) { return Scatter(vec3f(-wo.xy,wo.z),t*f,vec3f(1),1u,1.0); }
    wi=refract(-wo,vec3f(0,0,1),1.0/eta);
    return Scatter(wi,t*(1.0-f),m.transmissionColor*transmissionAttenuation(m)/(eta*eta),1u,eta);
  }
  if(glass || random(rng)<clamp(m.weight*mix(0.5,1.0,m.metal),0.0,1.0)) {
    let h=visibleNormal(wo,alpha,vec2f(random(rng),random(rng)));
    if(glass && random(rng)>=dielectricFresnel(dot(wo,h),eta)) { wi=refract(-wo,h,1.0/eta); if(wi.z>=0.0) { return Scatter(wi,0,vec3f(0),0u,1); } }
    else { wi=reflect(-wo,h); if(wi.z<=0.0) { return Scatter(wi,0,vec3f(0),0u,1); } }
  } else {
    let u=vec2f(random(rng),random(rng)); let r=sqrt(u.x); let phi=2.0*PI*u.y;
    wi=vec3f(r*cos(phi),r*sin(phi),sqrt(1.0-u.x));
  }
  let f=transportEval(m,wo,wi,eta);
  return Scatter(wi,f.w,f.xyz*abs(wi.z)/max(1e-30,f.w),0u,select(eta,1.0,wi.z>0.0));
}
fn powerHeuristic(a: f32, b: f32) -> f32 { return a*a/max(1e-30,a*a+b*b); }
fn nativeEval(m:Lobe,wo:vec3f,wi:vec3f,eta:f32)->vec4f {
  if(m.kind==5u) {
    if(wi.z<=0.0){return vec4f(0);}
    let h=normalize(wo+wi);let oh=max(1e-6,dot(wo,h));let alpha=max(vec2f(.0001),m.alpha);
    let f=generalizedSchlickFresnel(m,oh);return vec4f(m.weight*f*microfacetD(h,alpha)*microfacetG(wo,wi,alpha)/(4.0*wo.z*wi.z),visibleNormalPDF(wo,h,alpha)/(4.0*oh));
  }
  if(m.kind==4u) {
    if(wi.z<=0.0){return vec4f(0);}
    let sigma=max(.02,m.alpha.x);let az=max(.02,m.alpha.y);
    let forward=max(0.0,dot(wo,wi));
    let longitudinal=exp(-(1.0-forward)/max(1e-4,sigma*sigma))/(2.0*PI*sigma*sigma);
    let azimuth=pow(max(0.0,1.0-abs(wo.z-wi.z)),1.0/max(.02,az));
    let value=m.weight*m.base*(.2/PI+.8*longitudinal*azimuth);
    return vec4f(value,wi.z/PI);
  }
  if(m.kind==6u) {
    if(wi.z<=0.0){return vec4f(0);}
    let sigma2=max(.0004,m.alpha.x*m.alpha.x);let radial2=dot(wo-wi,wo-wi);
    let profile=1.0/(2.0*PI*sigma2*(1.0+radial2/sigma2)*(1.0+radial2/sigma2));
    let value=m.weight*m.base*(.25/PI+.75*profile);
    return vec4f(value,wi.z/PI);
  }
  if(m.kind==7u) {
    if(wi.z>=0.0){return vec4f(0);}
    return vec4f(m.weight*m.base*abs(wi.z)/PI,abs(wi.z)/PI);
  }
  if(m.kind==3u) {
    if(wi.z<=0.0){return vec4f(0);}
    let s=dot(wo,wi)-wo.z*wi.z;let sigma=m.roughness*m.roughness;
    let a=1.0-.5*sigma/(sigma+.33);let b=.45*sigma/(sigma+.09);
    return vec4f(m.weight*m.base*(a+b*max(0.0,s)/max(wo.z,wi.z))/PI,wi.z/PI);
  }
  if(max(m.alpha.x,m.alpha.y)<=0.0001){return vec4f(0);}
  let alpha=max(vec2f(.0001),m.alpha);
  if(m.kind==2u) {
    if(wi.z<=0.0){return vec4f(0);}
    let h=normalize(wo+wi);let oh=dot(wo,h);
    var fres=conductorFresnel(oh,m.complexIOR,m.extinction);if(m.thinFilmThickness>0.0){fres*=thinFilmFresnel(oh,m.ior,m.thinFilmIOR,m.thinFilmThickness);}
    return vec4f(m.weight*fres*microfacetD(h,alpha)*microfacetG(wo,wi,alpha)/(4.0*wo.z*wi.z),visibleNormalPDF(wo,h,alpha)/(4.0*oh));
  }
  if((wi.z>0.0&&m.scatterMode==2u)||(wi.z<0.0&&m.scatterMode==1u)){return vec4f(0);}
  let f=dielectricEval(wo,wi,alpha,eta);var value=f.x;var normalization=1.0;
  if(wi.z>0.0&&m.thinFilmThickness>0.0){let h=normalize(wo+wi);value=dot(thinFilmFresnel(dot(wo,h),m.ior,m.thinFilmIOR,m.thinFilmThickness),vec3f(1.0/3.0));}
  if(m.scatterMode!=3u) {let sum=wo+wi*select(eta,1.0,wi.z>0.0);let h=normalize(sum);let fr=dielectricFresnel(abs(dot(wo,h)),eta);normalization=select(1.0-fr,fr,m.scatterMode==1u);}
  return vec4f(m.weight*m.transmissionColor*transmissionAttenuation(m)*value,f.y/max(1e-30,normalization));
}
fn nativeSample(m:Lobe,wo:vec3f,eta:f32,rng:ptr<function,u32>)->Scatter {
  var wi=vec3f(0);var delta=0u;
  if(m.kind==3u || m.kind==4u || m.kind==6u) {let r=sqrt(random(rng));let phi=2.0*PI*random(rng);wi=vec3f(r*cos(phi),r*sin(phi),sqrt(max(0.0,1.0-r*r)));}
  else if(m.kind==7u) {let r=sqrt(random(rng));let phi=2.0*PI*random(rng);wi=vec3f(r*cos(phi),r*sin(phi),-sqrt(max(0.0,1.0-r*r)));}
  else {
    var h=vec3f(0,0,1);if(max(m.alpha.x,m.alpha.y)>0.0001 && (eta!=1.0||m.kind==2u)){h=visibleNormal(wo,max(vec2f(.0001),m.alpha),vec2f(random(rng),random(rng)));}else{delta=1u;}
    if(m.kind==2u){wi=reflect(-wo,h);if(delta!=0u){var fres=conductorFresnel(wo.z,m.complexIOR,m.extinction);if(m.thinFilmThickness>0.0){fres*=thinFilmFresnel(wo.z,m.ior,m.thinFilmIOR,m.thinFilmThickness);}return Scatter(wi,1,m.weight*fres,1u,1);}}
    else if(m.kind==5u){wi=reflect(-wo,h);if(wi.z<=0.0){return Scatter(wi,0,vec3f(0),0u,1);}if(delta!=0u){return Scatter(wi,1,m.weight*generalizedSchlickFresnel(m,dot(wo,h)),1u,1);}}
    else {
      let fr0=dielectricFresnel(dot(wo,h),eta);let fr=select(fr0,dot(thinFilmFresnel(dot(wo,h),m.ior,m.thinFilmIOR,m.thinFilmThickness),vec3f(1.0/3.0)),m.thinFilmThickness>0.0);let pr=select(fr,0.0,m.scatterMode==2u);let pt=select(1.0-fr,0.0,m.scatterMode==1u);let total=pr+pt;
      if(total<=0.0){return Scatter(vec3f(0),0,vec3f(0),0u,1);}
      if(random(rng)<pr/total){wi=reflect(-wo,h);if(wi.z<=0.0){return Scatter(wi,0,vec3f(0),0u,1);}if(delta!=0u){return Scatter(wi,pr/total,m.weight*m.transmissionColor*total,1u,1);}}
      else{wi=refract(-wo,h,1.0/eta);if(delta!=0u){return Scatter(wi,pt/total,m.weight*m.transmissionColor*transmissionAttenuation(m)*total/(eta*eta),1u,eta);}if(wi.z>=0.0){return Scatter(wi,0,vec3f(0),0u,1);}}
    }
  }
  if((m.kind!=1u&&m.kind!=7u&&wi.z<=0.0)||wo.z<=0.0){return Scatter(wi,0,vec3f(0),0u,1);}
  let f=nativeEval(m,wo,wi,eta);return Scatter(wi,f.w,f.xyz*abs(wi.z)/max(1e-30,f.w),0u,select(eta,1.0,wi.z>0.0));
}
`;
