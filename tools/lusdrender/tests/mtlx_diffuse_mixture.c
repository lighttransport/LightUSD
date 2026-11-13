/* MaterialX closure composition must retain each diffuse lobe and weight. */
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
      "<diffuse_bsdf name=\"n%d\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.5,0.5,0.5\"/></diffuse_bsdf>", i);
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
  if (ok != expected || (ok && result.diffuse_lobe_count != MTLX_MAX_DIFFUSE_LOBES)) {
    fprintf(stderr, "FAIL diffuse lobe limit: accepted=%d expected=%d\n", ok, expected);
    return 0;
  }
  return 1;
}

static int layer_checks(void) {
  const char *parts =
    "<oren_nayar_diffuse_bsdf name=\"a\" type=\"BSDF\"><input name=\"weight\" type=\"float\" value=\"0.25\"/><input name=\"color\" type=\"color3\" value=\"1,0,0\"/></oren_nayar_diffuse_bsdf>"
    "<burley_diffuse_bsdf name=\"b\" type=\"BSDF\"><input name=\"weight\" type=\"float\" value=\"0.5\"/><input name=\"color\" type=\"color3\" value=\"0,1,0\"/></burley_diffuse_bsdf>"
    "<oren_nayar_diffuse_bsdf name=\"c\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0,0,1\"/></oren_nayar_diffuse_bsdf>"
    "<mix name=\"mix\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"a\"/><input name=\"fg\" type=\"BSDF\" nodename=\"b\"/><input name=\"mix\" type=\"float\" value=\"0.4\"/></mix>"
    "<add name=\"add\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"a\"/><input name=\"in2\" type=\"BSDF\" nodename=\"b\"/></add>"
    "<multiply name=\"mult\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"a\"/><input name=\"in2\" type=\"float\" value=\"0.2\"/></multiply>"
    "<layer name=\"inner\" type=\"BSDF\"><input name=\"top\" type=\"BSDF\" nodename=\"a\"/><input name=\"base\" type=\"BSDF\" nodename=\"b\"/></layer>";
  const char *tops[] = {"a", "mix", "add", "mult", "inner", "c"};
  /* Expected coefficients of independently evaluated red, green, blue lobes. */
  const float expected[][3] = {{.25f,0,.75f},{.15f,.20f,.65f},
      {.25f,.50f,.25f},{.05f,0,.75f},{.25f,.375f,.375f},{0,0,1}};
  char xml[8192];
  for (int i = 0; i < 6; ++i) {
    OpenPBRParams p;
    snprintf(xml,sizeof(xml),"<materialx>%s<layer name=\"stack\" type=\"BSDF\"><input name=\"top\" type=\"BSDF\" nodename=\"%s\"/><input name=\"base\" type=\"BSDF\" nodename=\"c\"/></layer><surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"stack\"/></surface></materialx>",parts,tops[i]);
    if (!evaluate(xml,&p)) { fprintf(stderr,"FAIL layer graph %d\n",i); return 0; }
    float coefficients[3]={0,0,0};
    for (int j=0;j<p.diffuse_lobe_count;j++) {
      MtlxDiffuseLobe *l=&p.diffuse_lobes[j];
      coefficients[0]+=l->weight*l->color.x;
      coefficients[1]+=l->weight*l->color.y;
      coefficients[2]+=l->weight*l->color.z;
    }
    for (int j=0;j<3;j++) if (fabsf(coefficients[j]-expected[i][j])>1e-6f) {
      fprintf(stderr,"FAIL layer case=%d channel=%d got=%f expected=%f\n",i,j,coefficients[j],expected[i][j]); return 0;
    }
  }
  return 1;
}

int main(void) {
  const char *parts =
    "<oren_nayar_diffuse_bsdf name=\"a\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.2,0.5,0.8\"/><input name=\"roughness\" type=\"float\" value=\"0.9\"/></oren_nayar_diffuse_bsdf>"
    "<burley_diffuse_bsdf name=\"b\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.9,0.3,0.1\"/><input name=\"roughness\" type=\"float\" value=\"0.4\"/></burley_diffuse_bsdf>"
    "<oren_nayar_diffuse_bsdf name=\"c\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.4,0.7,0.3\"/><input name=\"roughness\" type=\"float\" value=\"0.6\"/><input name=\"energy_compensation\" type=\"boolean\" value=\"true\"/></oren_nayar_diffuse_bsdf>"
    "<mix name=\"mixab\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"a\"/><input name=\"fg\" type=\"BSDF\" nodename=\"b\"/><input name=\"mix\" type=\"float\" value=\"0.25\"/></mix>"
    "<add name=\"sum\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"mixab\"/><input name=\"in2\" type=\"BSDF\" nodename=\"c\"/></add>"
    "<multiply name=\"scaled\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"sum\"/><input name=\"in2\" type=\"float\" value=\"0.4\"/></multiply>"
    "<constant name=\"factor\" type=\"color3\"><input name=\"value\" type=\"color3\" value=\"0.2,0.5,0.9\"/></constant>"
    "<multiply name=\"tint\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"sum\"/><input name=\"in2\" type=\"color3\" nodename=\"factor\"/></multiply>"
    "<multiply name=\"tint_reverse\" type=\"BSDF\"><input name=\"in1\" type=\"color3\" nodename=\"factor\"/><input name=\"in2\" type=\"BSDF\" nodename=\"sum\"/></multiply>"
    "<multiply name=\"tint_literal\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"sum\"/><input name=\"in2\" type=\"color3\" value=\"0.2,0.5,0.9\"/></multiply>";
  const char *roots[] = {"scaled", "a", "b", "c", "sum", "tint", "tint_reverse", "tint_literal"};
  OpenPBRParams params[8]; char xml[8192];
  for (int i=0;i<8;i++) {
    snprintf(xml,sizeof(xml),"<materialx>%s<surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"%s\"/></surface></materialx>",parts,roots[i]);
    if (!evaluate(xml,&params[i])) { fprintf(stderr,"FAIL closure parse/evaluation %d\n",i); return 1; }
  }
  if (params[0].diffuse_lobe_count != 3 ||
      params[0].diffuse_lobes[0].model != MTLX_DIFFUSE_OREN_NAYAR ||
      params[0].diffuse_lobes[1].model != MTLX_DIFFUSE_BURLEY ||
      params[0].diffuse_lobes[2].model != MTLX_DIFFUSE_EON ||
      fabsf(params[0].diffuse_lobes[0].weight-0.3f)>1e-6f ||
      fabsf(params[0].diffuse_lobes[1].weight-0.1f)>1e-6f ||
      fabsf(params[0].diffuse_lobes[2].weight-0.4f)>1e-6f) {
    fprintf(stderr,"FAIL diffuse lobe data was lost\n"); return 1;
  }
  const v3 normal={0,0,1}, views[]={{0,0,1},{0.8f,0,0.6f}},
           lights[]={{0.6f,0,0.8f},{-0.6f,0,0.8f},{0,0.8f,0.6f}};
  for (int v=0;v<2;v++) for (int l=0;l<3;l++) {
    v3 d[8], spec; float pdf;
    for (int j=0;j<8;j++) bsdf_eval_lobes(&params[j],normal,views[v],lights[l],&d[j],&spec,&pdf);
    v3 expected=v3_add(v3_add(v3_scale(d[1],0.3f),v3_scale(d[2],0.1f)),v3_scale(d[3],0.4f));
    if (fabsf(d[0].x-expected.x)>1e-5f || fabsf(d[0].y-expected.y)>1e-5f || fabsf(d[0].z-expected.z)>1e-5f) {
      fprintf(stderr,"FAIL mixture response view=%d light=%d\n",v,l); return 1;
    }
    v3 tinted = v3_mul(d[4], v3_make(0.2f, 0.5f, 0.9f));
    for (int j=5;j<8;j++) {
      if (fabsf(d[j].x-tinted.x)>1e-5f || fabsf(d[j].y-tinted.y)>1e-5f ||
          fabsf(d[j].z-tinted.z)>1e-5f) {
        fprintf(stderr,"FAIL RGB closure multiply order=%d view=%d light=%d\n",j,v,l); return 1;
      }
    }
  }
  if (!layer_checks()) return 1;
  if (!overflow_check(MTLX_MAX_DIFFUSE_LOBES, 1) ||
      !overflow_check(MTLX_MAX_DIFFUSE_LOBES+1, 0)) return 1;
  puts("PASS: mixed diffuse closure responses and bounded overflow");
  return 0;
}
