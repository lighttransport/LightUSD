// --layering graph (diagnostic): the OpenPBR graph's layer throughput on transmission.
//
// Checks through bsdf_eval_lobes, for OpenPBR glass:
//  * physical (default) is bit-identical before and after toggling graph mode;
//  * graph mode leaves reflection untouched and lowers transmission by a factor in
//    (0.80, 0.995) at IOR 1.6 (the graph's second Fresnel, issues/materialx/02);
//  * with specular_weight 0 there is no R top layer, so graph == physical exactly.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsdf.h"

/* hemisphere power: hemi 0 = reflection side, 1 = transmission side */
static double power(const OpenPBRParams *p, float view_cosine, int hemi, int n) {
  const int n_mu = n, n_phi = 2 * n;
  const double dmu = 1.0 / n_mu, dphi = 2.0 * M_PI / n_phi;
  const float s = sqrtf(fmaxf(0.0f, 1.0f - view_cosine * view_cosine));
  const v3 wo = v3_make(s, 0.0f, view_cosine), Ns = v3_make(0, 0, 1);
  double sum = 0.0;
  for (int y = 0; y < n_mu; ++y) {
    const double t = (y + 0.5) * dmu, mu = 1.0 - t * t, w_mu = 2.0 * t * dmu;
    const float radial = (float)sqrt(fmax(0.0, 1.0 - mu * mu));
    for (int x = 0; x < n_phi; ++x) {
      const double phi = (x + 0.5) * dphi;
      const float z = hemi ? -(float)mu : (float)mu;
      const v3 wi = v3_make(radial * (float)cos(phi), radial * (float)sin(phi), z);
      v3 d, sp;
      float pdf = 0.0f;
      v3 f = bsdf_eval_lobes(p, Ns, wo, wi, &d, &sp, &pdf);
      sum += (double)f.x * mu * w_mu * dphi;
    }
  }
  return sum;
}

static void glass(OpenPBRParams *p, float roughness, float specular_weight) {
  memset(p, 0, sizeof(*p));
  p->base_weight = 1.0f;
  p->base_color = v3_splat(1.0f);
  p->specular_weight = specular_weight;
  p->specular_color = v3_splat(1.0f);
  p->specular_roughness = roughness;
  p->specular_ior = 1.6f;
  p->transmission = 1.0f;
  p->transmission_color = v3_splat(1.0f);
  p->coat_ior = 1.5f;
  p->coat_color = v3_splat(1.0f);
  p->thin_film_ior = 1.5f;
  p->normal = v3_make(0.0f, 0.0f, 1.0f);
  p->opacity = 1.0f;
  p->openpbr_coat_remap = 1;
}

int main(void) {
  int fail = 0;
  const float views[] = {0.3f, 0.6f, 1.0f};
  for (size_t v = 0; v < sizeof views / sizeof *views; ++v) {
    OpenPBRParams p;
    glass(&p, 0.7f, 1.0f);
    bsdf_set_layering_graph(0);
    const double r0 = power(&p, views[v], 0, 128), t0 = power(&p, views[v], 1, 128);
    bsdf_set_layering_graph(1);
    const double r1 = power(&p, views[v], 0, 128), t1 = power(&p, views[v], 1, 128);
    bsdf_set_layering_graph(0);
    const double t0b = power(&p, views[v], 1, 128);
    const double ratio = t1 / t0;
    const int ok = t0b == t0 && r1 == r0 && ratio > 0.80 && ratio < 0.995;
    fail |= !ok;
    printf("mu=%.2f reflection %.5f/%.5f transmission physical %.5f graph %.5f ratio %.4f%s\n",
           views[v], r0, r1, t0, t1, ratio, ok ? "" : "  FAIL");
    glass(&p, 0.7f, 0.0f);
    bsdf_set_layering_graph(0);
    const double w0 = power(&p, views[v], 1, 64);
    bsdf_set_layering_graph(1);
    const double w1 = power(&p, views[v], 1, 64);
    bsdf_set_layering_graph(0);
    if (w0 != w1) { printf("FAIL: specular_weight 0 differs %.6f vs %.6f\n", w0, w1); fail = 1; }
  }
  printf("%s\n", fail ? "FAIL" : "PASS: --layering graph scales only transmission; physical unchanged");
  return fail;
}
