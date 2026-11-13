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
  /* Uniform-solid-angle white furnace: unit radiance, cosine-weighted BRDF.
   * The reference's fitted multiple-scatter compensation is approximate. */
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
    if (!v3_is_finite(energy) || energy.x>0.42f || energy.y>0.42f || energy.z>0.42f ||
        energy.x<0.01f || energy.y<0.01f || energy.z<0.01f) {
      fprintf(stderr,"conductor furnace view=%d energy=(%g,%g,%g)\n",v,energy.x,energy.y,energy.z);return 1;
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
