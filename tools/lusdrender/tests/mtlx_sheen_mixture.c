/* MaterialX closure composition must retain each sheen lobe and weight. */
#include "mtlx_doc.h"
#include "mtlx_eval.h"
#include "bsdf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int evaluate(const char *xml, OpenPBRParams *out) {
  MtlxDoc *doc = mtlx_load_string(xml);
  if (!doc) return 0;
  ShadeContext ctx; memset(&ctx, 0, sizeof(ctx));
  ctx.doc = doc; ctx.Ns = v3_make(0,0,1); ctx.Ng = ctx.Ns;
  ctx.dpdu = v3_make(1,0,0); ctx.dpdv = v3_make(0,1,0);
  ctx.memo = calloc((size_t)(doc->nnode ? doc->nnode : 1), sizeof(MtlxValue));
  ctx.memo_done = calloc((size_t)(doc->nnode ? doc->nnode : 1), 1);
  int id = mtlx_find_node(doc, -1, "s");
  int ok = ctx.memo && ctx.memo_done && !mtlx_eval_surface(&ctx, id, out);
  free(ctx.memo); free(ctx.memo_done); mtlx_free(doc);
  return ok;
}

static int overflow_check(int count, int expected) {
  char xml[32768]; size_t used = 0;
  used += (size_t)snprintf(xml + used, sizeof(xml) - used, "<materialx>");
  int ids[64], next = count;
  for (int i = 0; i < count; ++i) {
    ids[i] = i;
    used += (size_t)snprintf(xml + used, sizeof(xml) - used,
      "<sheen_bsdf name=\"n%d\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.5,0.5,0.5\"/></sheen_bsdf>", i);
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
  if (ok != expected || (ok && result.sheen_lobe_count != MTLX_MAX_SHEEN_LOBES)) {
    fprintf(stderr, "FAIL sheen lobe limit: accepted=%d expected=%d\n", ok, expected);
    return 0;
  }
  return 1;
}

int main(void) {
  const char *parts =
    "<sheen_bsdf name=\"a\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.2,0.5,0.8\"/><input name=\"roughness\" type=\"float\" value=\"0.02\"/></sheen_bsdf>"
    "<sheen_bsdf name=\"b\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.9,0.3,0.1\"/><input name=\"roughness\" type=\"float\" value=\"0.4\"/></sheen_bsdf>"
    "<sheen_bsdf name=\"c\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.4,0.7,0.3\"/><input name=\"roughness\" type=\"float\" value=\"0.6\"/></sheen_bsdf>"
    "<sheen_bsdf name=\"z\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.7,0.4,0.2\"/><input name=\"roughness\" type=\"float\" value=\"0.5\"/><input name=\"mode\" type=\"string\" value=\"zeltner\"/></sheen_bsdf>"
    "<mix name=\"mixab\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"a\"/><input name=\"fg\" type=\"BSDF\" nodename=\"b\"/><input name=\"mix\" type=\"float\" value=\"0.25\"/></mix>"
    "<add name=\"sum\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"mixab\"/><input name=\"in2\" type=\"BSDF\" nodename=\"c\"/></add>"
    "<multiply name=\"scaled\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"sum\"/><input name=\"in2\" type=\"float\" value=\"0.4\"/></multiply>"
    "<constant name=\"factor\" type=\"color3\"><input name=\"value\" type=\"color3\" value=\"0.2,0.5,0.9\"/></constant>"
    "<multiply name=\"tint\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"sum\"/><input name=\"in2\" type=\"color3\" nodename=\"factor\"/></multiply>"
    "<multiply name=\"tint_reverse\" type=\"BSDF\"><input name=\"in1\" type=\"color3\" nodename=\"factor\"/><input name=\"in2\" type=\"BSDF\" nodename=\"sum\"/></multiply>"
    "<multiply name=\"tint_literal\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"sum\"/><input name=\"in2\" type=\"color3\" value=\"0.2,0.5,0.9\"/></multiply>";
  const char *roots[] = {"scaled", "a", "b", "c", "sum", "tint", "tint_reverse", "tint_literal", "z"};
  OpenPBRParams params[9]; char xml[8192];
  for (int i=0;i<9;i++) {
    snprintf(xml,sizeof(xml),"<materialx>%s<surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"%s\"/></surface></materialx>",parts,roots[i]);
    if (!evaluate(xml,&params[i])) { fprintf(stderr,"FAIL closure parse/evaluation %d\n",i); return 1; }
  }
  if (params[0].sheen_lobe_count != 3 ||
      fabsf(params[0].sheen_lobes[0].weight-0.3f)>1e-6f ||
      fabsf(params[0].sheen_lobes[1].weight-0.1f)>1e-6f ||
      fabsf(params[0].sheen_lobes[2].weight-0.4f)>1e-6f) {
    fprintf(stderr,"FAIL sheen lobe data was lost\n"); return 1;
  }
  if (params[8].sheen_lobes[0].model != MTLX_SHEEN_ZELTNER ||
      params[1].sheen_lobes[0].model != MTLX_SHEEN_CONTY_KULLA) {
    fprintf(stderr,"FAIL sheen mode parsing\n"); return 1;
  }
  const v3 normal={0,0,1}, views[]={{0,0,1},{0.8f,0,0.6f},{0.99995f,0,0.009999875f}},
           lights[]={{0.6f,0,0.8f},{-0.6f,0,0.8f},{0,0.8f,0.6f},{0.99995f,0,0.009999875f}};
  for (int v=0;v<3;v++) for (int l=0;l<4;l++) {
    v3 d[9], diffuse; float pdf;
    for (int j=0;j<9;j++) bsdf_eval_lobes(&params[j],normal,views[v],lights[l],&diffuse,&d[j],&pdf);
    /* Independent Conty-Kulla BRDF: D / (4 * (Nv + Nl - Nv*Nl)). */
    const float roughness[] = {0.02f, 0.4f, 0.6f};
    const v3 color[] = {{0.2f,0.5f,0.8f}, {0.9f,0.3f,0.1f}, {0.4f,0.7f,0.3f}};
    v3 half = v3_normalize(v3_add(views[v], lights[l]));
    float nv = views[v].z, nl = lights[l].z;
    for (int j=0;j<3;j++) {
      float inv = 1.0f / fmaxf(roughness[j], 0.005f);
      float dref = (2.0f + inv) * powf(fmaxf(0.0f, 1.0f-half.z*half.z), inv*0.5f) /
                   (2.0f * MTLX_PI * 4.0f * (nv+nl-nv*nl));
      float tol = 3e-5f * fmaxf(1.0f, dref);
      if (fabsf(d[j+1].x - color[j].x*dref)>tol ||
          fabsf(d[j+1].y - color[j].y*dref)>tol ||
          fabsf(d[j+1].z - color[j].z*dref)>tol) {
        fprintf(stderr,"FAIL sheen reference lobe=%d view=%d light=%d\n",j,v,l); return 1;
      }
    }
    /* Independently evaluate the upstream LTC fit in double precision. */
    {
      double x=views[v].z, y=0.5, nl=lights[l].z;
      double sig=y*(0.0206607+1.58491*y)/(0.0379424+y*(1.32227+y));
      double mu=y*(-0.193854+y*(-1.14885+y*(1.7932-0.95943*y*y)))/(0.046391+y);
      double off=y*(0.000654023+(-0.0207818+0.119681*y)*y)/(1.26264+y*(-1.92021+y));
      double e=exp(-0.5*pow((x-mu)/sig,2))/(sig*sqrt(2.0*3.14159265358979323846))+off;
      double a=(2.58126*x+0.813703*y)*y/(1.0+0.310327*x*x+2.60994*x*y);
      double b=sqrt(1.0-x)*(y-1.0)*y*y*y/(0.0000254053+1.71228*x-1.71506*x*y+1.34174*y*y);
      double tangent=sqrt(fmax(0.0,1.0-x*x));
      double wx=tangent>1e-6 ? (v3_dot(views[v],lights[l])-x*nl)/tangent : 0.0;
      double wy2=fmax(0.0,1.0-nl*nl-wx*wx), tx=a*wx+b*nl;
      double len2=tx*tx+a*a*wy2+nl*nl;
      double ref=e/3.14159265358979323846*pow(a/len2,2);
      double tol=5e-4*fmax(1.0,ref);
      if (fabs(d[8].x-0.7*ref)>tol || fabs(d[8].y-0.4*ref)>tol || fabs(d[8].z-0.2*ref)>tol) {
        fprintf(stderr,"FAIL Zeltner reference view=%d light=%d got=%g expected=%g\n",v,l,d[8].x,0.7*ref); return 1;
      }
    }
    v3 expected=v3_add(v3_add(v3_scale(d[1],0.3f),v3_scale(d[2],0.1f)),v3_scale(d[3],0.4f));
    float mix_tol = 3e-5f * fmaxf(1.0f, v3_maxc(expected));
    if (fabsf(d[0].x-expected.x)>mix_tol || fabsf(d[0].y-expected.y)>mix_tol || fabsf(d[0].z-expected.z)>mix_tol) {
      fprintf(stderr,"FAIL mixture response view=%d light=%d\n",v,l); return 1;
    }
    v3 tinted = v3_mul(d[4], v3_make(0.2f, 0.5f, 0.9f));
    for (int j=5;j<8;j++) {
      float tint_tol = 3e-5f * fmaxf(1.0f, v3_maxc(tinted));
      if (fabsf(d[j].x-tinted.x)>tint_tol || fabsf(d[j].y-tinted.y)>tint_tol ||
          fabsf(d[j].z-tinted.z)>tint_tol) {
        fprintf(stderr,"FAIL RGB closure multiply order=%d view=%d light=%d\n",j,v,l); return 1;
      }
    }
  }
  if (!overflow_check(MTLX_MAX_SHEEN_LOBES, 1) ||
      !overflow_check(MTLX_MAX_SHEEN_LOBES+1, 0)) return 1;
  puts("PASS: mixed sheen closure responses and bounded overflow");
  return 0;
}
