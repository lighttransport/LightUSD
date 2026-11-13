/* MaterialX closure composition must retain each diffuse lobe and weight. */
#include "mtlx_doc.h"
#include "mtlx_eval.h"
#include "bsdf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int evaluate_view(const char *xml, OpenPBRParams *out, v3 view) {
  MtlxDoc *doc = mtlx_load_string(xml);
  if (!doc) return 0;
  ShadeContext ctx; memset(&ctx, 0, sizeof(ctx));
  ctx.doc = doc; ctx.Ns = v3_make(0,0,1); ctx.Ng = ctx.Ns; ctx.V = view;
  ctx.dpdu = v3_make(1,0,0); ctx.dpdv = v3_make(0,1,0);
  ctx.memo = calloc((size_t)(doc->nnode ? doc->nnode : 1), sizeof(MtlxValue));
  ctx.memo_done = calloc((size_t)(doc->nnode ? doc->nnode : 1), 1);
  int id = mtlx_find_node(doc, -1, "s");
  int ok = ctx.memo && ctx.memo_done && !mtlx_eval_surface(&ctx, id, out);
  free(ctx.memo); free(ctx.memo_done); mtlx_free(doc);
  return ok;
}

static int evaluate(const char *xml, OpenPBRParams *out) {
  return evaluate_view(xml, out, v3_make(0,0,1));
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

static int specular_overflow_check(int count, int expected) {
  char xml[32768]; size_t used=0; int ids[64],next=count;
  used+=(size_t)snprintf(xml+used,sizeof(xml)-used,"<materialx>");
  for(int i=0;i<count;i++) {
    ids[i]=i;
    used+=(size_t)snprintf(xml+used,sizeof(xml)-used,
      "<dielectric_bsdf name=\"n%d\" type=\"BSDF\"/>",i);
  }
  while(count>1) {
    int k=0;
    for(int i=0;i<count;i+=2) {
      if(i+1==count) { ids[k++]=ids[i]; continue; }
      used+=(size_t)snprintf(xml+used,sizeof(xml)-used,
        "<add name=\"n%d\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"n%d\"/><input name=\"in2\" type=\"BSDF\" nodename=\"n%d\"/></add>",next,ids[i],ids[i+1]);
      ids[k++]=next++;
    }
    count=k;
  }
  snprintf(xml+used,sizeof(xml)-used,"<surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"n%d\"/></surface></materialx>",ids[0]);
  OpenPBRParams p; int ok=evaluate(xml,&p);
  if(ok!=expected || (ok && p.specular_lobe_count!=MTLX_MAX_SPECULAR_LOBES)) {
    fprintf(stderr,"specular lobe overflow accepted=%d expected=%d\n",ok,expected);return 0;
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

/* MaterialX genglsl directional albedo and normal-incidence Airy F0;
 * expected values are checked by an independent reference script. */
static const float layer_specular_reference[4][2] = {
  {0.968380698f,0.964583354f}, {0.968501665f,0.964747628f},
  {0.754071581f,0.755918826f}, {0.763761936f,0.767310678f}
};
static int specular_layer_checks(void) {
  const char *parts=
    "<diffuse_bsdf name=\"base\" type=\"BSDF\"><input name=\"color\" type=\"color3\" value=\"0.4,0.5,0.6\"/></diffuse_bsdf>"
    "<dielectric_bsdf name=\"d\" type=\"BSDF\"><input name=\"weight\" type=\"float\" value=\"0.7\"/><input name=\"ior\" type=\"float\" value=\"1.6\"/><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/></dielectric_bsdf>"
    "<dielectric_bsdf name=\"df\" type=\"BSDF\"><input name=\"weight\" type=\"float\" value=\"0.7\"/><input name=\"ior\" type=\"float\" value=\"1.6\"/><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/><input name=\"thinfilm_thickness\" type=\"float\" value=\"450\"/><input name=\"thinfilm_ior\" type=\"float\" value=\"1.3\"/></dielectric_bsdf>"
    "<generalized_schlick_bsdf name=\"sc\" type=\"BSDF\"><input name=\"weight\" type=\"float\" value=\"0.7\"/><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/><input name=\"color0\" type=\"color3\" value=\"0.1,0.35,0.7\"/><input name=\"color90\" type=\"color3\" value=\"0.9,0.8,0.6\"/></generalized_schlick_bsdf>"
    "<generalized_schlick_bsdf name=\"sf\" type=\"BSDF\"><input name=\"weight\" type=\"float\" value=\"0.7\"/><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/><input name=\"color0\" type=\"color3\" value=\"0.1,0.35,0.7\"/><input name=\"color90\" type=\"color3\" value=\"0.9,0.8,0.6\"/><input name=\"thinfilm_thickness\" type=\"float\" value=\"500\"/><input name=\"thinfilm_ior\" type=\"float\" value=\"1.4\"/></generalized_schlick_bsdf>"
    "<dielectric_bsdf name=\"t\" type=\"BSDF\"><input name=\"scatter_mode\" type=\"string\" value=\"T\"/></dielectric_bsdf>"
    "<mix name=\"mixed\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"df\"/><input name=\"fg\" type=\"BSDF\" nodename=\"sf\"/><input name=\"mix\" type=\"float\" value=\"0.3\"/></mix>"
    "<layer name=\"nested\" type=\"BSDF\"><input name=\"top\" type=\"BSDF\" nodename=\"df\"/><input name=\"base\" type=\"BSDF\" nodename=\"sf\"/></layer>"
    "<multiply name=\"scaledtop\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"df\"/><input name=\"in2\" type=\"float\" value=\"0.2\"/></multiply>";
  const char *tops[]={"d","df","sc","sf","t","mixed","nested","scaledtop"};
  const v3 views[]={{0,0,1},{.8f,0,.6f}};
  char xml[8192];
  for(int v=0;v<2;v++) for(int i=0;i<8;i++) {
    OpenPBRParams p;
    snprintf(xml,sizeof(xml),"<materialx>%s<layer name=\"stack\" type=\"BSDF\"><input name=\"top\" type=\"BSDF\" nodename=\"%s\"/><input name=\"base\" type=\"BSDF\" nodename=\"base\"/></layer><surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"stack\"/></surface></materialx>",parts,tops[i]);
    if(!evaluate_view(xml,&p,views[v]) || p.diffuse_lobe_count!=1) {
      fprintf(stderr,"specular layer graph failed %d\n",i); return 0;
    }
    float expected=i<4 ? layer_specular_reference[i][v] :
      i==4 ? 1.0f : i==5 ? .7f*layer_specular_reference[1][v]+.3f*layer_specular_reference[3][v] :
      i==6 ? layer_specular_reference[1][v]*layer_specular_reference[3][v] : layer_specular_reference[1][v];
    if(fabsf(p.diffuse_lobes[0].weight-expected)>2e-6f) {
      fprintf(stderr,"specular layer %d view %d got %.9g expected %.9g\n",i,v,p.diffuse_lobes[0].weight,expected);return 0;
    }
  }
  return 1;
}

/* Independent MaterialX genglsl reflection reference vectors, in order:
 * dielectric, dielectric film, generalized Schlick, generalized Schlick film. */
static const float specular_response_reference[4][2][3] = {
 {{.0156327250f,.0250123600f,.0312654500f},{.0007798742f,.0012477988f,.0015597485f}},
 {{.0146400639f,.0197808128f,.0047630506f},{.0004317322f,.0010823248f,.0003889846f}},
 {{.0590012755f,.2172329386f,.4642646361f},{.0028684316f,.0106457343f,.0230269579f}},
 {{.0000000000f,.1216358210f,.2777817198f},{.0000000000f,.0064307468f,.0131264403f}}
};
/* The OpenPBR dielectric base and coat should route to the independently
 * tested MaterialX dielectric closure with matching IOR, tint and GGX alpha. */
static int openpbr_dielectric_response_checks(void) {
  const char *base = "<materialx><open_pbr_surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"base_weight\" type=\"float\" value=\"0\"/>"
    "<input name=\"specular_ior\" type=\"float\" value=\"1.6\"/>"
    "<input name=\"specular_roughness\" type=\"float\" value=\"0.5\"/>"
    "<input name=\"specular_color\" type=\"color3\" value=\"0.5,0.8,1\"/>"
    "</open_pbr_surface></materialx>";
  const char *coat = "<materialx><open_pbr_surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"base_weight\" type=\"float\" value=\"0\"/>"
    "<input name=\"specular_weight\" type=\"float\" value=\"0\"/>"
    "<input name=\"coat_weight\" type=\"float\" value=\"1\"/>"
    "<input name=\"coat_ior\" type=\"float\" value=\"1.6\"/>"
    "<input name=\"coat_roughness\" type=\"float\" value=\"0.5\"/>"
    "</open_pbr_surface></materialx>";
  const char *reference_tint = "<materialx><dielectric_bsdf name=\"d\" type=\"BSDF\">"
    "<input name=\"ior\" type=\"float\" value=\"1.6\"/>"
    "<input name=\"tint\" type=\"color3\" value=\"0.5,0.8,1\"/>"
    "<input name=\"roughness\" type=\"vector2\" value=\"0.25,0.25\"/>"
    "</dielectric_bsdf><surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"bsdf\" type=\"BSDF\" nodename=\"d\"/></surface></materialx>";
  const char *reference_white = "<materialx><dielectric_bsdf name=\"d\" type=\"BSDF\">"
    "<input name=\"ior\" type=\"float\" value=\"1.6\"/>"
    "<input name=\"roughness\" type=\"vector2\" value=\"0.25,0.25\"/>"
    "</dielectric_bsdf><surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"bsdf\" type=\"BSDF\" nodename=\"d\"/></surface></materialx>";
  OpenPBRParams p[4];
  if (!evaluate(base, &p[0]) || !evaluate(reference_tint, &p[1]) ||
      !evaluate(coat, &p[2]) || !evaluate(reference_white, &p[3])) return 0;
  const v3 n = {0,0,1}, views[] = {{0,0,1},{0.8f,0,0.6f}};
  const v3 wi = {0.6f,0,0.8f};
  float old_difference = 0.0f;
  for (int v = 0; v < 2; ++v) {
    float pdf; v3 values[4];
    for (int i = 0; i < 4; ++i) values[i] = bsdf_eval(&p[i], n, views[v], wi, &pdf);
    for (int i = 0; i < 4; i += 2) {
      if (fabsf(values[i].x-values[i+1].x)>2e-6f ||
          fabsf(values[i].y-values[i+1].y)>2e-6f ||
          fabsf(values[i].z-values[i+1].z)>2e-6f) {
        fprintf(stderr,"FAIL: OpenPBR dielectric response case=%d view=%d\n",i,v); return 0;
      }
    }
    OpenPBRParams legacy = p[0]; legacy.openpbr_coat_remap = 0;
    v3 old = bsdf_eval(&legacy, n, views[v], wi, &pdf);
    old_difference = fmaxf(old_difference, fabsf(old.z-values[1].z));
  }
  if (old_difference < 1e-4f) {
    fprintf(stderr,"FAIL: dielectric response regression did not distinguish legacy model\n"); return 0;
  }
  /* The graph scales F0 by specular weight and preserves the IOR branch. */
  const float ior[][2] = {{1.6f, 1.260869565f}, {0.8f, 0.894736842f}};
  for (int i = 0; i < 2; ++i) {
    OpenPBRParams actual = p[0], expected = p[1];
    actual.specular_ior = ior[i][0]; actual.specular_weight = 0.25f;
    expected.specular_lobes[0].frame.ior = v3_splat(ior[i][1]);
    for (int v = 0; v < 2; ++v) {
      float pdf; v3 a = bsdf_eval(&actual, n, views[v], wi, &pdf);
      v3 b = bsdf_eval(&expected, n, views[v], wi, &pdf);
      if (fabsf(a.x-b.x)>3e-6f || fabsf(a.y-b.y)>3e-6f || fabsf(a.z-b.z)>3e-6f) {
        fprintf(stderr,"FAIL: OpenPBR weighted IOR response case=%d view=%d\n",i,v); return 0;
      }
    }
  }
  {
    OpenPBRParams disabled = p[0]; disabled.specular_weight = 0.0f;
    float pdf; v3 a = bsdf_eval(&disabled, n, views[1], wi, &pdf);
    if (fabsf(a.x)>1e-6f || fabsf(a.y)>1e-6f || fabsf(a.z)>1e-6f) {
      fprintf(stderr,"FAIL: zero-weight OpenPBR specular response\n"); return 0;
    }
  }
  return 1;
}

/* The OpenPBR graph mixes a weighted generalized-Schlick metal closure with
 * its dielectric base; compare each mixture against independent closures. */
static int openpbr_metal_response_checks(void) {
  const char *base = "<materialx><open_pbr_surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"base_weight\" type=\"float\" value=\"0.75\"/>"
    "<input name=\"base_color\" type=\"color3\" value=\"0.2,0.4,0.6\"/>"
    "<input name=\"specular_weight\" type=\"float\" value=\"0.7\"/>"
    "<input name=\"specular_ior\" type=\"float\" value=\"1.6\"/>"
    "<input name=\"specular_roughness\" type=\"float\" value=\"0.5\"/>"
    "<input name=\"specular_color\" type=\"color3\" value=\"0.4,0.7,1\"/>"
    "</open_pbr_surface></materialx>";
  const char *dielectric = "<materialx><dielectric_bsdf name=\"d\" type=\"BSDF\">"
    "<input name=\"ior\" type=\"float\" value=\"1.478546294\"/>"
    "<input name=\"tint\" type=\"color3\" value=\"0.4,0.7,1\"/>"
    "<input name=\"roughness\" type=\"vector2\" value=\"0.25,0.25\"/>"
    "</dielectric_bsdf><surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"bsdf\" type=\"BSDF\" nodename=\"d\"/></surface></materialx>";
  const char *metal = "<materialx><generalized_schlick_bsdf name=\"d\" type=\"BSDF\">"
    "<input name=\"weight\" type=\"float\" value=\"0.7\"/>"
    "<input name=\"color0\" type=\"color3\" value=\"0.15,0.3,0.45\"/>"
    "<input name=\"color82\" type=\"color3\" value=\"0.4,0.7,1\"/>"
    "<input name=\"roughness\" type=\"vector2\" value=\"0.25,0.25\"/>"
    "</generalized_schlick_bsdf><surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"bsdf\" type=\"BSDF\" nodename=\"d\"/></surface></materialx>";
  OpenPBRParams p, d, m;
  if (!evaluate(base, &p) || !evaluate(dielectric, &d) || !evaluate(metal, &m)) return 0;
  const v3 n = {0,0,1}, views[] = {{0,0,1},{0.8f,0,0.6f}};
  const v3 lights[] = {{0.6f,0,0.8f},{0,0.8f,0.6f}};
  const float amounts[] = {0.0f,0.25f,0.5f,1.0f};
  for (int v = 0; v < 2; ++v) for (int l = 0; l < 2; ++l) {
    float pdf; v3 dref = bsdf_eval(&d,n,views[v],lights[l],&pdf);
    v3 mref = bsdf_eval(&m,n,views[v],lights[l],&pdf);
    p.metalness = 0.0f; v3 full_dielectric = bsdf_eval(&p,n,views[v],lights[l],&pdf);
    p.metalness = 1.0f; v3 full_metal = bsdf_eval(&p,n,views[v],lights[l],&pdf);
    for (int i = 0; i < 4; ++i) {
      p.metalness = amounts[i]; v3 diff, actual;
      bsdf_eval_lobes(&p,n,views[v],lights[l],&diff,&actual,&pdf);
      v3 expected = v3_lerp(dref,mref,amounts[i]);
      v3 full = bsdf_eval(&p,n,views[v],lights[l],&pdf);
      v3 full_expected = v3_lerp(full_dielectric,full_metal,amounts[i]);
      if (fabsf(full.x-full_expected.x)>3e-6f || fabsf(full.y-full_expected.y)>3e-6f ||
          fabsf(full.z-full_expected.z)>3e-6f) {
        fprintf(stderr,"FAIL: OpenPBR full metal mixture amount=%g view=%d light=%d\n",amounts[i],v,l);
        return 0;
      }
      if (fabsf(actual.x-expected.x)>3e-6f || fabsf(actual.y-expected.y)>3e-6f ||
          fabsf(actual.z-expected.z)>3e-6f) {
        fprintf(stderr,"FAIL: OpenPBR metal mixture amount=%g view=%d light=%d\n",amounts[i],v,l);
        return 0;
      }
    }
  }
  /* Compare film-coated OpenPBR endpoints to the retained MaterialX lobes,
   * and verify that the film weight mixes complete responses. */
  {
    OpenPBRParams film = p, df = d, mf = m;
    film.thin_film_thickness = 450.0f; film.thin_film_ior = 1.3f;
    df.specular_lobes[0].frame.thinfilm_thickness = 450.0f;
    df.specular_lobes[0].frame.thinfilm_ior = 1.3f;
    mf.specular_lobes[0].frame.thinfilm_thickness = 450.0f;
    mf.specular_lobes[0].frame.thinfilm_ior = 1.3f;
    for (int v=0; v<2; ++v) for (int i=0; i<4; ++i) {
      float pdf; v3 diffuse, actual;
      film.metalness=amounts[i]; film.thin_film_weight=1.0f;
      bsdf_eval_lobes(&film,n,views[v],lights[0],&diffuse,&actual,&pdf);
      v3 dr=bsdf_eval(&df,n,views[v],lights[0],&pdf);
      v3 mr=bsdf_eval(&mf,n,views[v],lights[0],&pdf);
      v3 expected=v3_lerp(dr,mr,amounts[i]);
      if (fabsf(actual.x-expected.x)>3e-6f || fabsf(actual.y-expected.y)>3e-6f || fabsf(actual.z-expected.z)>3e-6f) {
        fprintf(stderr,"FAIL: OpenPBR film closure mixture\n"); return 0;
      }
      v3 full=bsdf_eval(&film,n,views[v],lights[0],&pdf);
      film.thin_film_weight=0.0f;
      v3 plain=bsdf_eval(&film,n,views[v],lights[0],&pdf);
      film.thin_film_weight=0.5f;
      v3 half=bsdf_eval(&film,n,views[v],lights[0],&pdf);
      expected=v3_lerp(plain,full,0.5f);
      if (fabsf(half.x-expected.x)>3e-6f || fabsf(half.y-expected.y)>3e-6f || fabsf(half.z-expected.z)>3e-6f) {
        fprintf(stderr,"FAIL: OpenPBR film weight mixture\n"); return 0;
      }
    }
  }
  {
    const char *film_doc = "<materialx><open_pbr_surface name=\"s\" type=\"surfaceshader\"><input name=\"thin_film_thickness\" type=\"float\" value=\"0.45\"/></open_pbr_surface></materialx>";
    OpenPBRParams units;
    if (!evaluate(film_doc,&units) || fabsf(units.thin_film_thickness-450.0f)>1e-4f) {
      fprintf(stderr,"FAIL: OpenPBR film thickness units\n"); return 0;
    }
  }
  /* A zero-thickness film must not change either component of the mix. */
  {
    OpenPBRParams nofilm = p, zero = p;
    nofilm.metalness = zero.metalness = 0.5f;
    nofilm.thin_film_weight = 0.0f; zero.thin_film_weight = 1.0f;
    nofilm.thin_film_thickness = zero.thin_film_thickness = 0.0f;
    float pdf; v3 a = bsdf_eval(&nofilm,n,views[1],lights[0],&pdf);
    v3 b = bsdf_eval(&zero,n,views[1],lights[0],&pdf);
    if (fabsf(a.x-b.x)>3e-6f || fabsf(a.y-b.y)>3e-6f || fabsf(a.z-b.z)>3e-6f) {
      fprintf(stderr,"FAIL: zero-thickness OpenPBR film changed the response\n"); return 0;
    }
  }
  /* OpenPBR's specular weight has a soft UI maximum, not a hard clamp. */
  p.metalness = 1.0f; p.specular_weight = 1.5f;
  m.specular_lobes[0].frame.weight = 1.5f;
  {
    float pdf; v3 diffuse, actual;
    bsdf_eval_lobes(&p,n,views[1],lights[0],&diffuse,&actual,&pdf);
    const v3 expected = bsdf_eval(&m,n,views[1],lights[0],&pdf);
    if (fabsf(actual.x-expected.x)>3e-6f || fabsf(actual.y-expected.y)>3e-6f ||
        fabsf(actual.z-expected.z)>3e-6f) {
      fprintf(stderr,"FAIL: OpenPBR metal specular weight above one\n"); return 0;
    }
  }
  return 1;
}

static int specular_response_checks(void) {
  const char *parts=
    "<dielectric_bsdf name=\"d\" type=\"BSDF\"><input name=\"ior\" type=\"float\" value=\"1.6\"/><input name=\"tint\" type=\"color3\" value=\"0.5,0.8,1\"/><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/></dielectric_bsdf>"
    "<dielectric_bsdf name=\"df\" type=\"BSDF\"><input name=\"ior\" type=\"float\" value=\"1.6\"/><input name=\"tint\" type=\"color3\" value=\"0.5,0.8,1\"/><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/><input name=\"thinfilm_thickness\" type=\"float\" value=\"450\"/><input name=\"thinfilm_ior\" type=\"float\" value=\"1.3\"/></dielectric_bsdf>"
    "<generalized_schlick_bsdf name=\"sc\" type=\"BSDF\"><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/><input name=\"color0\" type=\"color3\" value=\"0.1,0.35,0.7\"/><input name=\"color82\" type=\"color3\" value=\"0.7,0.9,0.3\"/><input name=\"color90\" type=\"color3\" value=\"0.9,0.8,0.6\"/><input name=\"exponent\" type=\"float\" value=\"4\"/></generalized_schlick_bsdf>"
    "<generalized_schlick_bsdf name=\"sf\" type=\"BSDF\"><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/><input name=\"color0\" type=\"color3\" value=\"0.1,0.35,0.7\"/><input name=\"color82\" type=\"color3\" value=\"0.7,0.9,0.3\"/><input name=\"color90\" type=\"color3\" value=\"0.9,0.8,0.6\"/><input name=\"exponent\" type=\"float\" value=\"4\"/><input name=\"thinfilm_thickness\" type=\"float\" value=\"500\"/><input name=\"thinfilm_ior\" type=\"float\" value=\"1.4\"/></generalized_schlick_bsdf>"
    "<mix name=\"mix\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"df\"/><input name=\"fg\" type=\"BSDF\" nodename=\"sf\"/><input name=\"mix\" type=\"float\" value=\"0.3\"/></mix>"
    "<multiply name=\"tinted\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"mix\"/><input name=\"in2\" type=\"color3\" value=\"0.2,0.5,0.9\"/></multiply>"
    "<constant name=\"factor\" type=\"color3\"><input name=\"value\" type=\"color3\" value=\"0.2,0.5,0.9\"/></constant>"
    "<multiply name=\"tintconn\" type=\"BSDF\"><input name=\"in1\" type=\"BSDF\" nodename=\"mix\"/><input name=\"in2\" type=\"color3\" nodename=\"factor\"/></multiply>"
    "<multiply name=\"tintreverse\" type=\"BSDF\"><input name=\"in1\" type=\"color3\" nodename=\"factor\"/><input name=\"in2\" type=\"BSDF\" nodename=\"mix\"/></multiply>"
    "<conductor_bsdf name=\"metal\" type=\"BSDF\"><input name=\"roughness\" type=\"vector2\" value=\"0.2,0.7\"/></conductor_bsdf>"
    "<mix name=\"cross\" type=\"BSDF\"><input name=\"bg\" type=\"BSDF\" nodename=\"d\"/><input name=\"fg\" type=\"BSDF\" nodename=\"metal\"/><input name=\"mix\" type=\"float\" value=\"0.4\"/></mix>";
  const char *roots[]={"d","df","sc","sf","mix","tinted","cross","tintconn","tintreverse"};
  const v3 views[]={{0,0,1},{.8f,0,.6f}}, light={0,.6f,.8f}, n={0,0,1};
  char xml[8192]; OpenPBRParams p[9];
  for(int i=0;i<9;i++) {
    snprintf(xml,sizeof(xml),"<materialx>%s<surface name=\"s\" type=\"surfaceshader\"><input name=\"bsdf\" type=\"BSDF\" nodename=\"%s\"/></surface></materialx>",parts,roots[i]);
    if(!evaluate(xml,&p[i]) || p[i].specular_lobe_count!=(i<4 || i==6 ? 1 : 2)) {
      fprintf(stderr,"specular response graph failed %d\n",i);return 0;
    }
  }
  for(int v=0;v<2;v++) {
    v3 output[9],diff;float pdf;
    for(int i=0;i<9;i++) bsdf_eval_lobes(&p[i],n,views[v],light,&diff,&output[i],&pdf);
    for(int i=0;i<4;i++) {
      const float *r=specular_response_reference[i][v];
      if(fabsf(output[i].x-r[0])>3e-5f || fabsf(output[i].y-r[1])>3e-5f || fabsf(output[i].z-r[2])>3e-5f) {
        fprintf(stderr,"specular response %d/%d got %g %g %g expected %g %g %g\n",i,v,output[i].x,output[i].y,output[i].z,r[0],r[1],r[2]);return 0;
      }
    }
    v3 half=v3_normalize(v3_add(views[v],light));
    v3 metal=bsdf_materialx_conductor_anisotropic(v3_make(.183f,.421f,1.373f),
        v3_make(3.424f,2.346f,1.770f),.2f,.7f,views[v].z,light.z,half.x,half.y,half.z,v3_dot(views[v],half));
    v3 cross=v3_add(v3_scale(output[0],.6f),v3_scale(metal,.4f));
    if(fabsf(output[6].x-cross.x)>2e-5f || fabsf(output[6].y-cross.y)>2e-5f || fabsf(output[6].z-cross.z)>2e-5f) {
      fprintf(stderr,"dielectric/conductor mixture failed view %d\n",v);return 0;
    }
    v3 expect=v3_add(v3_scale(output[1],.7f),v3_scale(output[3],.3f));
    v3 tint=v3_mul(expect,v3_make(.2f,.5f,.9f));
    if(fabsf(output[4].x-expect.x)>1e-5f || fabsf(output[4].y-expect.y)>1e-5f || fabsf(output[4].z-expect.z)>1e-5f ||
       fabsf(output[5].x-tint.x)>1e-5f || fabsf(output[5].y-tint.y)>1e-5f || fabsf(output[5].z-tint.z)>1e-5f ||
       fabsf(output[7].x-tint.x)>1e-5f || fabsf(output[7].y-tint.y)>1e-5f || fabsf(output[7].z-tint.z)>1e-5f ||
       fabsf(output[8].x-tint.x)>1e-5f || fabsf(output[8].y-tint.y)>1e-5f || fabsf(output[8].z-tint.z)>1e-5f) {
      fprintf(stderr,"specular closure mixture failed view %d\n",v);return 0;
    }
  }
  /* Rotating a lobe frame preserves its response after accounting for the
   * common surface cosine; retroreflection reflects the local view first. */
  {
    OpenPBRParams tilted=p[2];
    tilted.specular_lobes[0].frame.normal=v3_make(.6f,0,.8f);
    tilted.specular_lobes[0].frame.tangent=v3_make(0,1,0);
    tilted.specular_lobes[0].frame.retroreflective=1;
    v3 world_view=v3_make(.8f,0,.6f), world_light=v3_make(0,.6f,.8f);
    v3 local_view=v3_make(0,.28f,.96f), local_light=v3_make(.6f,.48f,.64f);
    v3 d,actual,expected;float pdf;
    bsdf_eval_lobes(&tilted,n,world_view,world_light,&d,&actual,&pdf);
    bsdf_eval_lobes(&p[2],n,local_view,local_light,&d,&expected,&pdf);
    expected=v3_scale(expected,.64f/.8f);
    if(fabsf(actual.x-expected.x)>2e-5f || fabsf(actual.y-expected.y)>2e-5f ||
       fabsf(actual.z-expected.z)>2e-5f) {
      fprintf(stderr,"tilted retroreflective specular frame failed\n");return 0;
    }
  }
  /* Proposal and PDF include each independent closure, including retro mode. */
  p[4].specular_lobes[1].frame.retroreflective=1;
  p[4].specular_roughness=1.0f;
  v3 view=views[1];double bins[4]={0},energy=0;int observed[4]={0};
  const int nm=192,np=384,count=120000;
  for(int i=0;i<nm;i++) for(int j=0;j<np;j++) {
    float mu=(i+.5f)/nm, phi=2.0f*MTLX_PI*(j+.5f)/np,r=sqrtf(1-mu*mu);
    v3 wi=v3_make(r*cosf(phi),r*sinf(phi),mu);float pdf;
    v3 value=bsdf_eval(&p[4],n,view,wi,&pdf);
    int bin=(mu>=.5f?2:0)+(wi.x>=0?1:0);
    bins[bin]+=pdf*(2.0*3.14159265358979323846/(nm*np));
    energy+=value.y*mu*(2.0*3.14159265358979323846/(nm*np));
  }
  pcg32 gen;pcg32_seed(&gen,628,413);double estimate=0;
  for(int i=0;i<count;i++) {
    BsdfSample sample;if(!bsdf_sample(&p[4],n,view,&gen,&sample))continue;
    int bin=(sample.wi.z>=.5f?2:0)+(sample.wi.x>=0?1:0);
    observed[bin]++;estimate+=sample.throughput.y;
  }
  for(int i=0;i<4;i++) if(fabs((double)observed[i]/count-bins[i])>.01) {
    fprintf(stderr,"specular proposal histogram %d: %g vs %g\n",i,(double)observed[i]/count,bins[i]);return 0;
  }
  if(fabs(estimate/count-energy)>.025) {
    fprintf(stderr,"specular sampling integral %g vs %g\n",estimate/count,energy);return 0;
  }
  {
    OpenPBRParams invalid=p[0];
    invalid.specular_lobes[0].frame.thinfilm_thickness=500.0f;
    invalid.specular_lobes[0].frame.thinfilm_ior=1.0e35f;
    float pdf; v3 value=bsdf_eval(&invalid,n,views[1],light,&pdf);
    if(!v3_is_finite(value) || !isfinite(pdf)) {
      fprintf(stderr,"nonfinite specular film response\n");return 0;
    }
  }
  /* OpenPBR defaults and connected inputs must reach the native BSDF. */
  {
    const char *defaults = "<materialx><open_pbr_surface name=\"s\" type=\"surfaceshader\"/></materialx>";
    const char *authored = "<materialx><constant name=\"v\" type=\"float\">"
      "<input name=\"value\" type=\"float\" value=\"0.3\"/></constant>"
      "<open_pbr_surface name=\"s\" type=\"surfaceshader\">"
      "<input name=\"coat_darkening\" type=\"float\" nodename=\"v\"/>"
      "</open_pbr_surface></materialx>";
    const char *standard = "<materialx><standard_surface name=\"s\" type=\"surfaceshader\"/></materialx>";
    OpenPBRParams a, b, c;
    if (!evaluate(defaults, &a) || !evaluate(authored, &b) || !evaluate(standard, &c) ||
        fabsf(a.coat_darkening - 1.0f) > 1.0e-6f ||
        fabsf(b.coat_darkening - 0.3f) > 1.0e-6f ||
        !a.openpbr_coat_remap || !b.openpbr_coat_remap || c.openpbr_coat_remap) {
      fprintf(stderr, "FAIL: OpenPBR coat darkening input routing\n"); return 0;
    }
  }
  /* The legacy glass branch must account for its mixture selection chance. */
  {
    OpenPBRParams legacy;openpbr_defaults(&legacy);
    legacy.base_weight=0.0f;legacy.specular_weight=0.0f;
    legacy.transmission=1.0f;legacy.specular_ior=1.5f;
    legacy.specular_roughness=.02f;legacy.transmission_color=v3_splat(1.0f);
    pcg32 gen;pcg32_seed(&gen,218,617);double sum=0;const int count=60000;
    for(int i=0;i<count;i++) {
      BsdfSample sample;
      if(bsdf_sample(&legacy,n,n,&gen,&sample) && sample.transmission)
        sum+=sample.throughput.x;
    }
    if(fabs(sum/count-1.0)>.01) {
      fprintf(stderr,"legacy glass mixture weight %g expected 1\n",sum/count);return 0;
    }
  }
  /* An RT closure must not emit a second reflection from the glass branch. */
  const char *rt_graph="<materialx><dielectric_bsdf name=\"glass\" type=\"BSDF\">"
    "<input name=\"ior\" type=\"float\" value=\"1.5\"/>"
    "<input name=\"roughness\" type=\"vector2\" value=\"0.02,0.02\"/>"
    "<input name=\"scatter_mode\" type=\"string\" value=\"RT\"/>"
    "</dielectric_bsdf><surface name=\"s\" type=\"surfaceshader\">"
    "<input name=\"bsdf\" type=\"BSDF\" nodename=\"glass\"/>"
    "</surface></materialx>";
  OpenPBRParams glass;
  if(!evaluate(rt_graph,&glass) || glass.specular_lobe_count!=1 || glass.transmission<.99f) return 0;
  pcg32 rng;pcg32_seed(&rng,957,341);double transmitted=0;const int nrt=30000;
  for(int i=0;i<nrt;i++) {
    BsdfSample sample;if(!bsdf_sample(&glass,n,n,&rng,&sample))continue;
    if(sample.transmission) {
      if(!sample.crossed || !v3_is_finite(sample.throughput)) {
        fprintf(stderr,"RT glass branch duplicated reflection\n");return 0;
      }
      transmitted+=sample.throughput.x;
    }
  }
  if(fabs(transmitted/nrt-.96)>.02) {
    fprintf(stderr,"RT transmitted weight %g expected about .96\n",transmitted/nrt);return 0;
  }
  return 1;
}

/* OpenPBR routes its modulated eta to the transmission closure. Compare the
 * smooth-interface refracted direction to Snell's law on both sides. */
static int openpbr_transmission_ior_checks(void) {
  const struct { float ior, weight, effective; int openpbr; } cases[] = {
    {1.6f, 0.0f, 1.0f, 1}, {1.6f, 0.25f, 1.260869565f, 1},
    {1.6f, 1.0f, 1.6f, 1}, {0.8f, 0.25f, 0.894736842f, 1},
    {1.6f, 0.25f, 1.6f, 0}
  };
  const v3 n = {0,0,1};
  for (size_t i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
    OpenPBRParams p; openpbr_defaults(&p);
    p.base_weight=0; p.transmission=1; p.specular_ior=cases[i].ior;
    p.specular_weight=cases[i].weight; p.specular_roughness=0.02f;
    p.openpbr_coat_remap=cases[i].openpbr; p.transmission_color=v3_splat(1);
    for (int side=0; side<2; ++side) {
      const float x=side ? 0.3f : 0.6f;
      v3 wo=v3_make(x,0,(side ? -1.0f : 1.0f)*sqrtf(1-x*x));
      const float eta=side ? cases[i].effective : 1.0f/cases[i].effective;
      pcg32 rng; pcg32_seed(&rng, 425+i, 987+side);
      int found=0;
      for (int j=0;j<2000 && !found;++j) {
        BsdfSample sample;
        if (!bsdf_sample(&p,n,wo,&rng,&sample) || !sample.transmission || !sample.crossed) continue;
        if (fabsf(sample.wi.x + eta*x)>2e-5f || !v3_is_finite(sample.wi)) {
          fprintf(stderr,"FAIL: OpenPBR transmission IOR case=%zu side=%d got=%g expected=%g\n",i,side,sample.wi.x,-eta*x); return 0;
        }
        found=1;
      }
      if (!found) { fprintf(stderr,"FAIL: no transmission IOR sample case=%zu side=%d\n",i,side); return 0; }
    }
  }
  /* At an exiting angle beyond the critical angle, full-weight glass must
   * reflect, while zero specular weight gives IOR 1 and must cross. */
  for (int weighted=0;weighted<2;++weighted) {
    OpenPBRParams p; openpbr_defaults(&p); p.base_weight=0; p.transmission=1;
    p.specular_ior=1.5f; p.specular_weight=weighted ? 1.0f : 0.0f;
    p.specular_roughness=0.02f; p.openpbr_coat_remap=1;
    v3 wo=v3_make(0.9f,0,-sqrtf(0.19f)); pcg32 rng;pcg32_seed(&rng,71,weighted+9);
    int found=0;
    for(int j=0;j<2000 && !found;++j) {
      BsdfSample sample;
      if(!bsdf_sample(&p,n,wo,&rng,&sample) || !sample.transmission) continue;
      if(sample.crossed == weighted) {
        fprintf(stderr,"FAIL: weighted OpenPBR critical-angle routing\n");return 0;
      }
      found=1;
    }
    if(!found) { fprintf(stderr,"FAIL: no critical-angle glass sample\n");return 0; }
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
  if (!openpbr_transmission_ior_checks() || !layer_checks() || !specular_layer_checks() || !specular_response_checks() || !openpbr_dielectric_response_checks() || !openpbr_metal_response_checks() ||
      !specular_overflow_check(MTLX_MAX_SPECULAR_LOBES,1) ||
      !specular_overflow_check(MTLX_MAX_SPECULAR_LOBES+1,0)) return 1;
  if (!overflow_check(MTLX_MAX_DIFFUSE_LOBES, 1) ||
      !overflow_check(MTLX_MAX_DIFFUSE_LOBES+1, 0)) return 1;
  puts("PASS: mixed diffuse closure responses and bounded overflow");
  return 0;
}
