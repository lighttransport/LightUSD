// Empirical size of the binary32 lobe-evaluation error behind conservative mode's 1e-3
// reserve (doc/validation/certified-albedo.md): the native rough transmission lobe
// (bsdf_eval_lobes, binary32) against a binary64 recomputation of the same formula on
// identical binary32 inputs. Requires max relative error <= 5e-4 (half the reserve).
// Evidence, not a proof.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bsdf.h"
static double fd(double c, double e){ double s=e*e*(1-c*c); if(s>=1) return 1; double t=sqrt(1-s);
  double rs=(e*c-t)/(e*c+t), rp=(c-e*t)/(c+e*t); return .5*(rs*rs+rp*rp); }
static double Dg(double nh,double a){ if(nh<=0) return 0; double a2=a*a,h2=fmin(nh*nh,1),d=(1-h2)+a2*h2; return a2/(M_PI*d*d); }
static double Gg(double v,double l,double a){ double a2=a*a, lv=sqrt(a2+(1-a2)*v*v), ll=sqrt(a2+(1-a2)*l*l); return 2*v*l/(v*ll+l*lv); }
int main(void){
  OpenPBRParams p; memset(&p,0,sizeof p); p.base_weight=1; p.base_color=v3_splat(1); p.specular_weight=1; p.specular_color=v3_splat(1);
  p.specular_ior=1.5f; p.transmission=1; p.transmission_color=v3_splat(1); p.coat_ior=1.5f; p.coat_color=v3_splat(1); p.thin_film_ior=1.5f; p.normal=v3_make(0,0,1); p.opacity=1; p.openpbr_coat_remap=1;
  pcg32 rng; pcg32_seed(&rng,3,4); double worst=0; long n=0;
  for (int r=0;r<3;++r){ float rough=(float[]){0.3f,0.6f,1.0f}[r]; p.specular_roughness=rough;
    float a=rough*rough; double eta=1.0/1.5;
    BsdfLobeWeights lw = bsdf_lobe_weights(&p);
    for (int i=0;i<200000;++i){
      float mu=0.02f+0.98f*pcg32_f(&rng), ml=0.02f+0.98f*pcg32_f(&rng), ph=6.2831853f*pcg32_f(&rng);
      v3 wo=v3_make(sqrtf(1-mu*mu),0,mu), wi=v3_make(sqrtf(1-ml*ml)*cosf(ph),sqrtf(1-ml*ml)*sinf(ph),-ml);
      v3 d,s; float pdf; v3 f=bsdf_eval_lobes(&p,v3_make(0,0,1),wo,wi,&d,&s,&pdf);
      if(!(f.x>1e-6f)) continue;
      double ox=wo.x,oz=wo.z,ix=wi.x,iy=wi.y,iz=wi.z;
      double hx=ox+ix/eta, hy=iy/eta, hz=oz+iz/eta, h2=hx*hx+hy*hy+hz*hz, hl=sqrt(h2); hx/=hl;hy/=hl;hz/=hl; if(hz<0){hx=-hx;hy=-hy;hz=-hz;}
      double nv=oz, nl=-iz, nh=hz, oh=ox*hx+oz*hz, ih=ix*hx+iy*hy+iz*hz;
      double F=fd(oh,eta), a2=(double)a*a, root=sqrt(a2+(1-a2)*nv*nv), g1=2*nv/(nv+root);
      double dens=Dg(nh,a)*g1*oh/nv, jac=-ih/(eta*eta*h2), fx=Gg(nv,nl,a)/g1*(1-F)*dens*jac/nl;
      (void)lw; double rel=fabs(f.x-fx)/fx; if(rel>worst) worst=rel; ++n; }
  }
  printf("binary32 rough transmission vs binary64 recomputation: %ld points, max relative error %.3e: %s\n",
         n, worst, worst <= 5e-4 ? "ok" : "FAIL");
  return worst <= 5e-4 ? 0 : 1;
}
