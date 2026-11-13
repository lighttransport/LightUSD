/* Graph-level independent conductor lobe, mixture and opaque-layer regression. */
#include "mtlx_doc.h"
#include "mtlx_eval.h"
#include "bsdf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int evaluate(const char *xml, OpenPBRParams *out) {
  MtlxDoc *doc=mtlx_load_string(xml); if (!doc) return 0;
  ShadeContext c; memset(&c,0,sizeof(c)); c.doc=doc; c.Ns=v3_make(0,0,1); c.Ng=c.Ns; c.V=c.Ns;
  c.dpdu=v3_make(1,0,0); c.dpdv=v3_make(0,1,0);
  c.memo=calloc((size_t)doc->nnode,sizeof(*c.memo)); c.memo_done=calloc((size_t)doc->nnode,1);
  int id=mtlx_find_node(doc,-1,"s");
  int ok=c.memo && c.memo_done && !mtlx_eval_surface(&c,id,out);
  free(c.memo); free(c.memo_done); mtlx_free(doc); return ok;
}
static int near3(v3 a,v3 b) {
  float t=3e-5f*fmaxf(1.0f,v3_maxc(b));
  return fabsf(a.x-b.x)<t && fabsf(a.y-b.y)<t && fabsf(a.z-b.z)<t;
}
static int overflow_check(int count, int expected) {
  char xml[32768]; size_t used = 0;
  used += (size_t)snprintf(xml + used, sizeof(xml) - used, "<materialx>");
  int ids[64], next = count;
  for (int i = 0; i < count; ++i) {
    ids[i] = i;
    used += (size_t)snprintf(xml + used, sizeof(xml) - used,
      "<conductor_bsdf name=\"n%d\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.5,0.5,0.5\"/></conductor_bsdf>", i);
  }
  while (count > 1) {
    int k = 0;
    for (int i = 0; i < count; i += 2) {
      if (i + 1 == count) { ids[k++] = ids[i]; continue; }
      used += (size_t)snprintf(xml + used, sizeof(xml) - used,
        "<add name=\"n%d\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"n%d\"/><input name=\"in2\" type=\"BSDF\" nodename=\"n%d\"/></add>", next, ids[i], ids[i+1]);
      ids[k++] = next++;
    }
    count = k;
  }
  snprintf(xml + used, sizeof(xml) - used,
    "<surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"n%d\"/></surface></materialx>", ids[0]);
  OpenPBRParams result;
  int ok = evaluate(xml, &result);
  if (ok != expected || (ok && result.conductor_lobe_count != MTLX_MAX_CONDUCTOR_LOBES)) {
    fprintf(stderr, "FAIL conductor lobe limit: accepted=%d expected=%d\n", ok, expected);
    return 0;
  }
  return 1;
}

/* Independent anisotropic visible-normal reflection density in the lobe frame. */
static double reference_conductor_pdf(const MtlxConductorLobe *l, v3 h, v3 view) {
  v3 t=v3_normalize(v3_make(l->tangent.x,l->tangent.y,0));
  v3 b=v3_cross(v3_make(0,0,1),t);
  double ax=fmax(1e-4,fmin(1.0,l->roughness));
  double ay=fmax(1e-4,fmin(1.0,l->roughness_y));
  double x=v3_dot(h,t)/ax, y=v3_dot(h,b)/ay, z=h.z;
  double d=x*x+y*y+z*z;
  double nv=view.z, vx=ax*v3_dot(view,t), vy=ay*v3_dot(view,b);
  double g1=2.0*nv/(nv+sqrt(nv*nv+vx*vx+vy*vy));
  return g1/(4.0*nv*3.14159265358979323846*ax*ay*d*d);
}

int main(void) {
  const char *nodes=
    "<oren_nayar_diffuse_bsdf name=\"d\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.2,0.4,0.6\"/></oren_nayar_diffuse_bsdf>"
    "<conductor_bsdf name=\"a\" type=\"BSDF\"><input name=\"weight\" type=\"float\" value=\"0.4\"/><input name=\"roughness\" type=\"vector2\" value=\"0.3,0.3\"/></conductor_bsdf>"
    "<conductor_bsdf name=\"b\" type=\"BSDF\"><input name=\"ior\" type=\"color3\" value=\"1.1,1.2,1.3\"/><input name=\"extinction\" type=\"color3\" value=\"2,1,3\"/><input name=\"roughness\" type=\"vector2\" value=\"0.7,0.7\"/></conductor_bsdf>"
    "<conductor_bsdf name=\"ax\" type=\"BSDF\"><input name=\"roughness\" type=\"vector2\" value=\"0.1,0.6\"/><input name=\"tangent\" type=\"vector3\" value=\"1,0,0\"/></conductor_bsdf>"
    "<conductor_bsdf name=\"ay\" type=\"BSDF\"><input name=\"roughness\" type=\"vector2\" value=\"0.1,0.6\"/><input name=\"tangent\" type=\"vector3\" value=\"0,1,0\"/></conductor_bsdf>"
    "<conductor_bsdf name=\"zero\" type=\"BSDF\"><input name=\"weight\" type=\"float\" value=\"0\"/></conductor_bsdf>"
    "<layer name=\"zero_layer\" type=\"BSDF\"><input name=\"top\" type=\"BSDF\" nodename=\"zero\"/><input name=\"base\" type=\"BSDF\" nodename=\"d\"/></layer>"
    "<mix name=\"mix\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"d\"/><input name=\"fg\" type=\"BSDF\" nodename=\"a\"/><input name=\"mix\" type=\"float\" value=\"0.25\"/></mix>"
    "<layer name=\"layer\" type=\"BSDF\"><input name=\"top\" type=\"BSDF\" nodename=\"a\"/><input name=\"base\" type=\"BSDF\" nodename=\"d\"/></layer>"
    "<mix name=\"two\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"a\"/><input name=\"fg\" type=\"BSDF\" nodename=\"b\"/><input name=\"mix\" type=\"float\" value=\"0.3\"/></mix>"
    "<multiply name=\"tint\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"mix\"/><input name=\"in2\" type=\"color3\" value=\"0.2,0.5,0.9\"/></multiply>";
  const char *roots[]={"d","a","b","mix","layer","two","tint","zero_layer","ax","ay"};
  OpenPBRParams p[10]; char xml[8192];
  for (int i=0;i<10;i++) {
    snprintf(xml,sizeof(xml),"<materialx>%s<surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"%s\"/></surface></materialx>",nodes,roots[i]);
    if (!evaluate(xml,&p[i])) {fprintf(stderr,"graph evaluation failed %s\n",roots[i]);return 1;}
  }
  /* Expected RGB responses independently computed with complex Fresnel
   * amplitudes and the reference's spectral sensitivity / GGX equations. */
  const char *film_graph = "<materialx>"
    "<conductor_bsdf name=\"film\" type=\"BSDF\">"
    "<input name=\"roughness\" type=\"vector2\" value=\"0.3,0.3\"/>"
    "<input name=\"thinfilm_thickness\" type=\"float\" value=\"400\"/>"
    "<input name=\"thinfilm_ior\" type=\"float\" value=\"1.5\"/>"
    "</conductor_bsdf><surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"bsdf\" type=\"BSDF\" nodename=\"film\"/>"
    "</surface></materialx>";
  OpenPBRParams film;
  if (!evaluate(film_graph, &film) || film.conductor_lobe_count != 1 ||
      film.conductor_lobes[0].thinfilm_thickness != 400.0f ||
      film.conductor_lobes[0].thinfilm_ior != 1.5f) {
    fprintf(stderr, "conductor film parameters lost\n"); return 1;
  }
  const v3 film_views[]={{0,0,1},{.8f,0,.6f}}, film_light={0,.6f,.8f};
  const v3 film_reference[]={{.30699481f,.16552106f,.08735680f},
                             {.10751124f,.06037760f,.02345290f}};
  for (int i=0;i<2;i++) {
    v3 diffuse,specular; float density;
    bsdf_eval_lobes(&film,v3_make(0,0,1),film_views[i],film_light,&diffuse,&specular,&density);
    if (!near3(specular,film_reference[i])) {
      fprintf(stderr,"conductor film reference %d: %g %g %g\n",i,specular.x,specular.y,specular.z); return 1;
    }
  }
  /* Separate films must survive a closure mix; compare with isolated lobes. */
  const char *film_mix_graph = "<materialx>"
    "<conductor_bsdf name=\"a\" type=\"BSDF\"><input name=\"roughness\" type=\"vector2\" value=\"0.3,0.3\"/><input name=\"thinfilm_thickness\" type=\"float\" value=\"400\"/><input name=\"thinfilm_ior\" type=\"float\" value=\"1.5\"/></conductor_bsdf>"
    "<conductor_bsdf name=\"b\" type=\"BSDF\"><input name=\"roughness\" type=\"vector2\" value=\"0.3,0.3\"/><input name=\"thinfilm_thickness\" type=\"float\" value=\"800\"/><input name=\"thinfilm_ior\" type=\"float\" value=\"1.25\"/></conductor_bsdf>"
    "<mix name=\"m\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"a\"/><input name=\"fg\" type=\"BSDF\" nodename=\"b\"/><input name=\"mix\" type=\"float\" value=\"0.3\"/></mix>"
    "<surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"m\"/></surface></materialx>";
  OpenPBRParams mixed;
  if (!evaluate(film_mix_graph,&mixed) || mixed.conductor_lobe_count!=2 ||
      mixed.conductor_lobes[0].thinfilm_thickness!=400.0f ||
      mixed.conductor_lobes[1].thinfilm_thickness!=800.0f ||
      mixed.conductor_lobes[1].thinfilm_ior!=1.25f) {
    fprintf(stderr,"mixed conductor film parameters lost\n"); return 1;
  }
  OpenPBRParams second=film;
  second.conductor_lobes[0].thinfilm_thickness=800.0f;
  second.conductor_lobes[0].thinfilm_ior=1.25f;
  for (int i=0;i<2;i++) {
    v3 d,a,b,m; float pdf;
    bsdf_eval_lobes(&film,v3_make(0,0,1),film_views[i],film_light,&d,&a,&pdf);
    bsdf_eval_lobes(&second,v3_make(0,0,1),film_views[i],film_light,&d,&b,&pdf);
    bsdf_eval_lobes(&mixed,v3_make(0,0,1),film_views[i],film_light,&d,&m,&pdf);
    if (!near3(m,v3_add(v3_scale(a,.7f),v3_scale(b,.3f))) || near3(a,b)) {
      fprintf(stderr,"mixed conductor films failed %d\n",i); return 1;
    }
  }
  if(p[3].conductor_lobe_count!=1 || p[3].diffuse_lobe_count!=1 || p[5].conductor_lobe_count!=2 ||
     fabsf(p[4].diffuse_lobes[0].weight-.6f)>1e-6f) {
    fprintf(stderr,"conductor lobe or layer data lost\n");return 1;
  }
  const v3 n={0,0,1}, views[]={{0,0,1},{.8f,0,.6f}}, light={0,.6f,.8f};
  for(int i=0;i<2;i++) {
    v3 d[10],s[10];float pdf;
    for(int j=0;j<10;j++) bsdf_eval_lobes(&p[j],n,views[i],light,&d[j],&s[j],&pdf);
    v3 h=v3_normalize(v3_add(views[i],light));
    v3 ref=bsdf_materialx_conductor(v3_make(.183f,.421f,1.373f),v3_make(3.424f,2.346f,1.770f),.3f,views[i].z,light.z,h.z,v3_dot(views[i],h));
    v3 eta=v3_make(.183f,.421f,1.373f), extinction=v3_make(3.424f,2.346f,1.770f);
    v3 rx=bsdf_materialx_conductor_anisotropic(eta,extinction,.1f,.6f,views[i].z,light.z,h.x,h.y,h.z,v3_dot(views[i],h));
    v3 ry=bsdf_materialx_conductor_anisotropic(eta,extinction,.1f,.6f,views[i].z,light.z,h.y,-h.x,h.z,v3_dot(views[i],h));
    if (!near3(s[8],rx) || !near3(s[9],ry) || fabsf(s[8].x-s[9].x)<1e-4f ||
        fabsf(p[8].conductor_lobes[0].roughness_y-.6f)>1e-6f) {
      fprintf(stderr,"anisotropic conductor/tangent failed view=%d\n",i); return 1;
    }
    if(!near3(s[1],v3_scale(ref,.4f)) || !near3(d[3],v3_scale(d[0],.75f)) ||
       !near3(s[3],v3_scale(s[1],.25f)) || !near3(d[4],v3_scale(d[0],.6f)) ||
       !near3(s[4],s[1]) || !near3(s[5],v3_add(v3_scale(s[1],.7f),v3_scale(s[2],.3f))) ||
       !near3(d[6],v3_mul(d[3],v3_make(.2f,.5f,.9f))) ||
       !near3(s[6],v3_mul(s[3],v3_make(.2f,.5f,.9f))) ||
       !near3(d[7],d[0]) || !near3(s[7],s[0])) {
      fprintf(stderr,"conductor mixture/layer response failed view=%d\n",i);return 1;
    }
  }
  /* Check the whole mixture PDF, including the small generic GGX proposal. */
  const int pdf_cases[]={5,8,9};
  for (int k=0;k<3;k++) {
    int index=pdf_cases[k];
    v3 wo=views[1], wi=light, half=v3_normalize(v3_add(wo,wi));
    double vh=v3_dot(wo,half), nh=half.z;
    double alpha=fmax(.02,p[index].specular_roughness); alpha*=alpha;
    double d=(1.0-nh*nh)+alpha*alpha*nh*nh;
    double generic=alpha*alpha/(3.14159265358979323846*d*d)*nh/(4.0*vh);
    double total=.04, expected=.04*generic;
    for(int j=0;j<p[index].conductor_lobe_count;j++) {
      const MtlxConductorLobe *l=&p[index].conductor_lobes[j];
      double w=l->weight*(.2126*l->weight_color.x+.7152*l->weight_color.y+.0722*l->weight_color.z);
      total+=w; expected+=w*reference_conductor_pdf(l,half,wo);
    }
    float pdf; (void)bsdf_eval(&p[index],n,wo,wi,&pdf);
    if(fabs(pdf-expected/total)>2e-5*fmax(1.0,expected/total)) {
      fprintf(stderr,"conductor proposal PDF mismatch %d: %g vs %g\n",index,pdf,expected/total); return 1;
    }
  }
  {
    /* At the smooth normal-incidence peak, avoid float cancellation in D. */
    OpenPBRParams smooth=p[8]; smooth.specular_roughness=0.0f;
    float pdf; (void)bsdf_eval(&smooth,n,n,n,&pdf);
    const double proposal_alpha=.02*.02;
    const double generic=1.0/(3.14159265358979323846*proposal_alpha*proposal_alpha*4.0);
    const double conductor=1.0/(3.14159265358979323846*.1*.6*4.0);
    const double expected=(.04*generic+conductor)/1.04;
    if (fabs(pdf-expected)>1e-4*expected) {
      fprintf(stderr,"smooth GGX proposal PDF got %g expected %g\n",pdf,expected);return 1;
    }
  }
  /* The anisotropic proposal follows the authored tangent frame. */
  for(int index=8;index<=9;index++) {
    pcg32 gen; pcg32_seed(&gen,734,913); double sx=0,sy=0; int valid=0;
    for(int k=0;k<30000;k++) {
      BsdfSample sample;
      if(!bsdf_sample(&p[index],n,n,&gen,&sample)) continue;
      sx+=sample.wi.x*sample.wi.x; sy+=sample.wi.y*sample.wi.y; valid++;
    }
    if(valid<1000 || (index==8 && !(sy>2.0*sx)) || (index==9 && !(sx>2.0*sy))) {
      fprintf(stderr,"anisotropic sampling orientation failed %d: %g %g\n",index,sx,sy);return 1;
    }
  }
  /* At a grazing view, compare sampled bin frequencies with independent
   * hemisphere integration of the reported mixture PDF, including rejects. */
  for(int index=8;index<=9;index++) {
    OpenPBRParams material=p[index]; material.specular_roughness=1.0f;
    v3 view=v3_make(sqrtf(1.0f-.1f*.1f),0,.1f);
    double expected[4]={0,0,0,0}; int observed[4]={0,0,0,0};
    const int nm=192, np=384, samples=120000;
    for(int i=0;i<nm;i++) for(int j=0;j<np;j++) {
      float mu=(i+.5f)/(float)nm, phi=2.0f*MTLX_PI*(j+.5f)/(float)np;
      float radial=sqrtf(1.0f-mu*mu);
      v3 wi=v3_make(radial*cosf(phi),radial*sinf(phi),mu); float pdf;
      (void)bsdf_eval(&material,n,view,wi,&pdf);
      int bin=(mu>=.5f ? 2 : 0)+(wi.x>=0 ? 1 : 0);
      expected[bin]+=pdf*(2.0*3.14159265358979323846/(nm*np));
    }
    pcg32 gen; pcg32_seed(&gen,345,987);
    for(int i=0;i<samples;i++) {
      BsdfSample sample; if(!bsdf_sample(&material,n,view,&gen,&sample)) continue;
      int bin=(sample.wi.z>=.5f ? 2 : 0)+(sample.wi.x>=0 ? 1 : 0);
      observed[bin]++;
    }
    for(int bin=0;bin<4;bin++) if(fabs((double)observed[bin]/samples-expected[bin])>.01) {
      fprintf(stderr,"VNDF histogram %d bin=%d got=%g expected=%g\n",index,bin,
        (double)observed[bin]/samples,expected[bin]);return 1;
    }
  }
  for(int index=0;index<10;index++) {
    BsdfLobeWeights w=bsdf_lobe_weights(&p[index]);
    if(fabsf(w.diffuse+w.specular+w.coat+w.transmission-1.0f)>1e-6f) {
      fprintf(stderr,"sampling probabilities do not sum to one %d\n",index);return 1;
    }
  }
  /* Compare anisotropic sampling against independent hemisphere quadrature. */
  for(int index=8;index<=9;index++) {
    v3 reference=v3_splat(0.0f); const int nm=96, np=192;
    for(int i=0;i<nm;i++) for(int j=0;j<np;j++) {
      float mu=(i+.5f)/(float)nm, phi=2.0f*MTLX_PI*(j+.5f)/(float)np;
      float radial=sqrtf(1.0f-mu*mu);
      v3 wi=v3_make(radial*cosf(phi),radial*sinf(phi),mu), diff, spec;
      float pdf; bsdf_eval_lobes(&p[index],n,views[1],wi,&diff,&spec,&pdf);
      reference=v3_add(reference,v3_scale(spec,mu*(2.0f*MTLX_PI/(nm*np))));
    }
    pcg32 gen; pcg32_seed(&gen,2345,6789); v3 sum=v3_splat(0.0f);
    const int samples=80000;
    for(int i=0;i<samples;i++) {
      BsdfSample sample;
      if(bsdf_sample(&p[index],n,views[1],&gen,&sample)) sum=v3_add(sum,sample.throughput);
    }
    v3 estimate=v3_scale(sum,1.0f/samples);
    if(fabsf(estimate.x-reference.x)>.025f || fabsf(estimate.y-reference.y)>.025f ||
       fabsf(estimate.z-reference.z)>.025f) {
      fprintf(stderr,"anisotropic sampling energy %d got=(%g,%g,%g) ref=(%g,%g,%g)\n",
        index,estimate.x,estimate.y,estimate.z,reference.x,reference.y,reference.z);return 1;
    }
  }
  /* Uniform-solid-angle white furnace: unit radiance, cosine-weighted BRDF.
   * The reference's fitted multiple-scatter compensation is approximate. */
  v3 furnace[2];
  for (int v=0;v<2;v++) {
    v3 energy=v3_splat(0.0f);
    const int nm=48, np=96;
    for (int i=0;i<nm;i++) for (int j=0;j<np;j++) {
      float mu=(i+.5f)/(float)nm, phi=2.0f*MTLX_PI*(j+.5f)/(float)np;
      float radial=sqrtf(1.0f-mu*mu);
      v3 wi=v3_make(radial*cosf(phi),radial*sinf(phi),mu), diffuse, specular;
      float pdf;
      bsdf_eval_lobes(&p[1],n,views[v],wi,&diffuse,&specular,&pdf);
      energy=v3_add(energy,v3_scale(specular,mu*(2.0f*MTLX_PI/(nm*np))));
    }
    furnace[v]=energy;
    if (!v3_is_finite(energy) || energy.x>0.42f || energy.y>0.42f || energy.z>0.42f ||
        energy.x<0.01f || energy.y<0.01f || energy.z<0.01f) {
      fprintf(stderr,"conductor furnace view=%d energy=(%g,%g,%g)\n",v,energy.x,energy.y,energy.z);return 1;
    }
  }
  /* Unconditional Monte Carlo averages include rejected below-horizon samples. */
  for(int v=0;v<2;v++) {
    pcg32 gen; pcg32_seed(&gen,867,5309); v3 sum=v3_splat(0.0f);
    const int samples=60000;
    for(int i=0;i<samples;i++) {
      BsdfSample sample;
      if(bsdf_sample(&p[1],n,views[v],&gen,&sample)) sum=v3_add(sum,sample.throughput);
    }
    v3 estimate=v3_scale(sum,1.0f/samples);
    if(fabsf(estimate.x-furnace[v].x)>.015f || fabsf(estimate.y-furnace[v].y)>.015f ||
       fabsf(estimate.z-furnace[v].z)>.015f) {
      fprintf(stderr,"conductor sampling energy view=%d got=(%g,%g,%g) ref=(%g,%g,%g)\n",
        v,estimate.x,estimate.y,estimate.z,furnace[v].x,furnace[v].y,furnace[v].z);return 1;
    }
  }
  {
    OpenPBRParams invalid=p[8]; invalid.conductor_lobes[0].roughness=NAN;
    invalid.conductor_lobes[0].roughness_y=NAN;
    invalid.specular_roughness=NAN;
    pcg32 gen; pcg32_seed(&gen,17,31);
    for(int i=0;i<256;i++) {
      BsdfSample sample;
      if(bsdf_sample(&invalid,n,n,&gen,&sample) &&
         (!v3_is_finite(sample.wi) || !v3_is_finite(sample.throughput) || !isfinite(sample.pdf))) {
        fprintf(stderr,"nonfinite conductor proposal\n");return 1;
      }
    }
  }
  pcg32 rng; pcg32_seed(&rng, 1234, 5678);
  int accepted = 0;
  for (int i=0;i<1024;i++) {
    BsdfSample sample;
    if (!bsdf_sample(&p[3], n, views[1], &rng, &sample)) continue;
    float pdf = 0.0f;
    v3 f = bsdf_eval(&p[3], n, views[1], sample.wi, &pdf);
    if (!v3_is_finite(sample.wi) || !v3_is_finite(sample.throughput) ||
        !v3_is_finite(f) || !(pdf > 0.0f) ||
        fabsf(pdf - sample.pdf) > 1e-5f*fmaxf(1.0f,pdf)) {
      fprintf(stderr,"invalid mixed conductor sample\n");return 1;
    }
    accepted++;
  }
  if (accepted < 100) { fprintf(stderr,"too few conductor samples\n"); return 1; }
  if (!overflow_check(MTLX_MAX_CONDUCTOR_LOBES,1) ||
      !overflow_check(MTLX_MAX_CONDUCTOR_LOBES+1,0)) return 1;
  puts("PASS: independent conductor closures, mixtures and opaque layers");return 0;
}
