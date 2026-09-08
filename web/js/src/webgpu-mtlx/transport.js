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
fn thinFilmFresnelLambda(c0:f32,baseIOR:f32,filmIOR:f32,thickness:f32,wavelength:f32)->f32 {
  let c=clamp(abs(c0),0.0,1.0);let phase=4.0*PI*filmIOR*max(0.0,thickness)*c/max(1.0,wavelength);
  let r01=(1.0-filmIOR)/(1.0+filmIOR);let r12=(filmIOR-baseIOR)/(filmIOR+baseIOR);
  return clamp(r01*r01+r12*r12+2.0*r01*r12*cos(phase),0.0,1.0);
}
fn thinFilmAt(c0:f32,baseIOR:f32,filmIOR:f32,thickness:f32,wavelength:f32)->vec3f {
  return select(thinFilmFresnel(c0,baseIOR,filmIOR,thickness),vec3f(thinFilmFresnelLambda(c0,baseIOR,filmIOR,thickness,wavelength)),wavelength>0.0);
}
fn transmissionAttenuation(m:Lobe)->vec3f { return exp(-max(vec3f(0),m.transmissionScatter)*max(0.0,m.transmissionDepth)); }
fn mxPow6(v:f32)->f32 { let v2=v*v; return v2*v2*v2; }
fn generalizedSchlickFresnel(m:Lobe,c:f32,wavelength:f32)->vec3f { let x=clamp(abs(c),0.0,1.0);let maxCos=1.0/7.0;let factor=1.0/(maxCos*pow(1.0-maxCos,6.0));let a=mix(m.base,m.schlickColor90,vec3f(pow(1.0-maxCos,m.schlickExponent)))*(vec3f(1)-m.schlickColor82)*factor;var f=mix(m.base,m.schlickColor90,vec3f(pow(1.0-x,m.schlickExponent)))-a*x*vec3f(mxPow6(1.0-x));if(m.thinFilmThickness>0.0){f*=thinFilmAt(x,m.ior,m.thinFilmIOR,m.thinFilmThickness,wavelength);}return f; }
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
fn transportEval(m: Lobe, wo: vec3f, wi: vec3f, eta: f32, wavelength:f32) -> vec4f {
  if(wo.z<=0.0 || wi.z==0.0) { return vec4f(0); }
  if(m.kind!=0u){return nativeEval(m,wo,wi,eta,wavelength);}
  let alpha=transportAlpha(m); let t=(1.0-m.metal)*m.transmission;
  let opaque=1.0-t; let specProbability=clamp(m.weight*mix(0.5,1.0,m.metal),0.0,1.0);
  var value=vec3f(0); var pdf=0.0;
  if(wi.z>0.0) {
    let h=normalize(wo+wi); let oh=max(1e-20,dot(wo,h));
    let fr=dielectricFresnel(oh,eta);
    let baseFresnel=select(mix(vec3f(fr),fresnel(oh,m.base),m.metal),fresnel(oh,m.specularColor),m.specularColorEnabled!=0u);
    let f=select(baseFresnel,thinFilmAt(oh,m.ior,m.thinFilmIOR,m.thinFilmThickness,wavelength),m.thinFilmThickness>0.0);
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
fn transportSample(m: Lobe, wo: vec3f, eta: f32, rng: ptr<function,u32>, wavelength:f32) -> Scatter {
  if(m.kind!=0u){return nativeSample(m,wo,eta,rng,wavelength);}
  var wi=vec3f(0); let t=(1.0-m.metal)*m.transmission;
  let glass=random(rng)<t;
  let alpha=transportAlpha(m);
  if(glass && (m.roughness<=0.0001 || eta==1.0)) {
    let f=select(dielectricFresnel(wo.z,eta),dot(thinFilmAt(wo.z,m.ior,m.thinFilmIOR,m.thinFilmThickness,wavelength),vec3f(1.0/3.0)),m.thinFilmThickness>0.0);
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
  let f=transportEval(m,wo,wi,eta,wavelength);
  return Scatter(wi,f.w,f.xyz*abs(wi.z)/max(1e-30,f.w),0u,select(eta,1.0,wi.z>0.0));
}
fn powerHeuristic(a: f32, b: f32) -> f32 { return a*a/max(1e-30,a*a+b*b); }
fn sheenZeltnerDirAlbedo(nv:f32,r:f32)->f32 {let s=r*(.0206607+1.58491*r)/(.0379424+r*(1.32227+r));let mm=r*(-.193854+r*(-1.14885+r*(1.7932-.95943*r*r)))/(.046391+r);let o=r*(.000654023+(-.0207818+.119681*r)*r)/(1.26264+r*(-1.92021+r));return clamp(exp(-.5*pow((nv-mm)/max(1e-5,s),2.0))/(max(1e-5,s)*sqrt(2.0*PI))+o,0.0,1.0);}
fn sheenZeltnerAInv(nv:f32,r:f32)->f32 {return (2.58126*nv+.813703*r)*r/(1.0+.310327*nv*nv+2.60994*nv*r);}
fn sheenZeltnerBInv(nv:f32,r:f32)->f32 {return sqrt(max(0.0,1.0-nv))*(r-1.0)*r*r*r/(.0000254053+1.71228*nv-1.71506*nv*r+1.34174*r*r);}
fn sheenZeltnerTransform(wo:vec3f,wi:vec3f,r:f32)->vec4f {let nv=clamp(wo.z,0.0,1.0);let tx=safeNormal(vec3f(wo.x,wo.y,0),vec3f(1,0,0));let ty=vec3f(-tx.y,tx.x,0);let l=vec3f(dot(wi,tx),dot(wi,ty),wi.z);let a=sheenZeltnerAInv(nv,r);let b=sheenZeltnerBInv(nv,r);let w=vec3f(a*l.x+b*l.z,a*l.y,l.z);let len2=max(1e-8,dot(w,w));return vec4f(w,len2);}
fn sheenZeltnerBRDF(wo:vec3f,wi:vec3f,r:f32)->f32 {let t=sheenZeltnerTransform(wo,wi,r);return max(0.0,t.z)/PI*pow(sheenZeltnerAInv(clamp(wo.z,0.0,1.0),r)/t.w,2.0);}
fn sheenZeltnerPDF(wo:vec3f,wi:vec3f,r:f32)->f32 {let t=sheenZeltnerTransform(wo,wi,r);let a=sheenZeltnerAInv(clamp(wo.z,0.0,1.0),r);let z=max(0.0,t.z/sqrt(t.w));return z/PI*pow(a*t.w,2.0);}
fn sheenZeltnerSample(wo:vec3f,r:f32,rng:ptr<function,u32>)->vec3f {let nv=clamp(wo.z,0.0,1.0);let rr=sqrt(random(rng));let phi=2.0*PI*random(rng);let local=vec3f(rr*cos(phi),rr*sin(phi),sqrt(max(0.0,1.0-rr*rr)));let a=sheenZeltnerAInv(nv,r);let b=sheenZeltnerBInv(nv,r);let raw=vec3f(local.x/a-local.z*b/a,local.y/a,local.z);let w=raw/sqrt(max(1e-8,dot(raw,raw)));let tx=safeNormal(vec3f(wo.x,wo.y,0),vec3f(1,0,0));let ty=vec3f(-tx.y,tx.x,0);return tx*w.x+ty*w.y+vec3f(0,0,w.z);}
fn nativeEval(m:Lobe,wo:vec3f,wi:vec3f,eta:f32,wavelength:f32)->vec4f {
  if(m.kind==5u) {
    if(wi.z<=0.0){return vec4f(0);}
    let h=normalize(wo+wi);let oh=max(1e-6,dot(wo,h));let alpha=max(vec2f(.0001),m.alpha);
    let f=generalizedSchlickFresnel(m,oh,wavelength);return vec4f(m.weight*f*microfacetD(h,alpha)*microfacetG(wo,wi,alpha)/(4.0*wo.z*wi.z),visibleNormalPDF(wo,h,alpha)/(4.0*oh));
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
    let sigma2=max(vec3f(.0004),m.subsurfaceRadius*m.subsurfaceRadius);let anis=clamp(m.anisotropy,-.9,.9);let radial=(wo-wi)*vec3f(1.0+anis,1.0-anis,1.0);let radial2=dot(radial,radial);
    let profile=vec3f(1.0)/(2.0*PI*sigma2*(vec3f(1.0)+radial2/sigma2)*(vec3f(1.0)+radial2/sigma2));
    let value=m.weight*m.base*(vec3f(.25/PI)+vec3f(.75)*profile);
    return vec4f(value,wi.z/PI);
  }
  if(m.kind==7u) {
    if(wi.z>=0.0){return vec4f(0);}
    return vec4f(m.weight*m.base*abs(wi.z)/PI,abs(wi.z)/PI);
  }
  if(m.kind==8u) {
    if(wi.z<=0.0){return vec4f(0);}
    let ndv=max(1e-6,wo.z);let ndl=max(1e-6,wi.z);let h=normalize(wo+wi);let ndh=max(1e-6,h.z);let r=clamp(m.alpha.x,.01,1.0);var value=0.0;var pdf=ndl/PI;if(m.scatterMode==0u){let invR=1.0/max(r,.005);let d=(2.0+invR)*pow(max(0.0,1.0-ndh*ndh),invR*.5)/(2.0*PI);value=d/(4.0*(ndl+ndv-ndl*ndv));}else{value=sheenZeltnerDirAlbedo(ndv,r)*sheenZeltnerBRDF(wo,wi,r);pdf=sheenZeltnerPDF(wo,wi,r);}return vec4f(m.weight*m.base*value,pdf);
  }
  if(m.kind==9u) {
    if(wi.z<=0.0){return vec4f(0);}
    let h=normalize(wo+wi);let rough=clamp(m.roughness,0.0,1.0);
    let fd90=0.5+2.0*rough*dot(wo,h)*dot(wo,h);
    let fi=1.0+(fd90-1.0)*pow(1.0-clamp(wi.z,0.0,1.0),5.0);
    let fo=1.0+(fd90-1.0)*pow(1.0-clamp(wo.z,0.0,1.0),5.0);
    return vec4f(m.weight*m.base*(fi*fo)/PI,wi.z/PI);
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
    var fres=conductorFresnel(oh,m.complexIOR,m.extinction);if(m.thinFilmThickness>0.0){fres*=thinFilmAt(oh,m.ior,m.thinFilmIOR,m.thinFilmThickness,wavelength);}
    return vec4f(m.weight*fres*microfacetD(h,alpha)*microfacetG(wo,wi,alpha)/(4.0*wo.z*wi.z),visibleNormalPDF(wo,h,alpha)/(4.0*oh));
  }
  if((wi.z>0.0&&m.scatterMode==2u)||(wi.z<0.0&&m.scatterMode==1u)){return vec4f(0);}
  let f=dielectricEval(wo,wi,alpha,eta);var value=f.x;var normalization=1.0;
  if(wi.z>0.0&&m.thinFilmThickness>0.0){let h=normalize(wo+wi);value=dot(thinFilmAt(dot(wo,h),m.ior,m.thinFilmIOR,m.thinFilmThickness,wavelength),vec3f(1.0/3.0));}
  if(m.scatterMode!=3u) {let sum=wo+wi*select(eta,1.0,wi.z>0.0);let h=normalize(sum);let fr=dielectricFresnel(abs(dot(wo,h)),eta);normalization=select(1.0-fr,fr,m.scatterMode==1u);}
  return vec4f(m.weight*m.transmissionColor*transmissionAttenuation(m)*value,f.y/max(1e-30,normalization));
}
fn nativeSample(m:Lobe,wo:vec3f,eta:f32,rng:ptr<function,u32>,wavelength:f32)->Scatter {
  var wi=vec3f(0);var delta=0u;
  if(m.kind==8u && m.scatterMode==1u) {wi=sheenZeltnerSample(wo,clamp(m.alpha.x,.01,1.0),rng);}
  else if(m.kind==3u || m.kind==4u || m.kind==6u || m.kind==8u || m.kind==9u) {let r=sqrt(random(rng));let phi=2.0*PI*random(rng);wi=vec3f(r*cos(phi),r*sin(phi),sqrt(max(0.0,1.0-r*r)));}
  else if(m.kind==7u) {let r=sqrt(random(rng));let phi=2.0*PI*random(rng);wi=vec3f(r*cos(phi),r*sin(phi),-sqrt(max(0.0,1.0-r*r)));}
  else {
    var h=vec3f(0,0,1);if(max(m.alpha.x,m.alpha.y)>0.0001 && (eta!=1.0||m.kind==2u)){h=visibleNormal(wo,max(vec2f(.0001),m.alpha),vec2f(random(rng),random(rng)));}else{delta=1u;}
    if(m.kind==2u){wi=reflect(-wo,h);if(delta!=0u){var fres=conductorFresnel(wo.z,m.complexIOR,m.extinction);if(m.thinFilmThickness>0.0){fres*=thinFilmAt(wo.z,m.ior,m.thinFilmIOR,m.thinFilmThickness,wavelength);}return Scatter(wi,1,m.weight*fres,1u,1);}}
    else if(m.kind==5u){wi=reflect(-wo,h);if(wi.z<=0.0){return Scatter(wi,0,vec3f(0),0u,1);}if(delta!=0u){return Scatter(wi,1,m.weight*generalizedSchlickFresnel(m,dot(wo,h),wavelength),1u,1);}}
    else {
      let fr0=dielectricFresnel(dot(wo,h),eta);let fr=select(fr0,dot(thinFilmAt(dot(wo,h),m.ior,m.thinFilmIOR,m.thinFilmThickness,wavelength),vec3f(1.0/3.0)),m.thinFilmThickness>0.0);let pr=select(fr,0.0,m.scatterMode==2u);let pt=select(1.0-fr,0.0,m.scatterMode==1u);let total=pr+pt;
      if(total<=0.0){return Scatter(vec3f(0),0,vec3f(0),0u,1);}
      if(random(rng)<pr/total){wi=reflect(-wo,h);if(wi.z<=0.0){return Scatter(wi,0,vec3f(0),0u,1);}if(delta!=0u){return Scatter(wi,pr/total,m.weight*m.transmissionColor*total,1u,1);}}
      else{wi=refract(-wo,h,1.0/eta);if(delta!=0u){return Scatter(wi,pt/total,m.weight*m.transmissionColor*transmissionAttenuation(m)*total/(eta*eta),1u,eta);}if(wi.z>=0.0){return Scatter(wi,0,vec3f(0),0u,1);}}
    }
  }
  if((m.kind!=1u&&m.kind!=7u&&wi.z<=0.0)||wo.z<=0.0){return Scatter(wi,0,vec3f(0),0u,1);}
  let f=nativeEval(m,wo,wi,eta,wavelength);return Scatter(wi,f.w,f.xyz*abs(wi.z)/max(1e-30,f.w),0u,select(eta,1.0,wi.z>0.0));
}
`;
