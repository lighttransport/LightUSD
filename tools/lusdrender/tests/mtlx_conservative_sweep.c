// --energy-mode conservative must not create energy for any material: sampler-based
// (bsdf_sample) directional albedo, per channel, for 16 OpenPBR configurations at roughness
// 0.02/0.1/0.4, four views, entering and exiting. Requires albedo - 3 sigma <= 1.003.
// Reference mode is allowed to exceed 1 (MaterialX's fitted compensation,
// issues/materialx/03); conservative mode removes that factor and attenuates layers by
// at least the sampled specular albedo.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bsdf.h"
static void mc_albedo(const OpenPBRParams *p, v3 Ns, float mu, long n, double *mean, double *se) {
  /* per-channel means; report the largest channel mean and its standard error */
  const v3 wo = v3_make(sqrtf(fmaxf(0.0f, 1.0f - mu * mu)), 0.0f, mu);
  pcg32 rng; pcg32_seed(&rng, 12345, 77);
  double s1[3] = {0, 0, 0}, s2[3] = {0, 0, 0};
  for (long i = 0; i < n; ++i) {
    BsdfSample bs; double t[3] = {0, 0, 0};
    if (bsdf_sample(p, Ns, wo, &rng, &bs) && bs.pdf > 0.0f && v3_is_finite(bs.throughput)) {
      t[0] = bs.throughput.x; t[1] = bs.throughput.y; t[2] = bs.throughput.z; }
    for (int c = 0; c < 3; ++c) { s1[c] += t[c]; s2[c] += t[c] * t[c]; }
  }
  int k = 0;
  for (int c = 1; c < 3; ++c) if (s1[c] > s1[k]) k = c;
  *mean = s1[k] / n; *se = sqrt(fmax(0, s2[k] / n - (*mean) * (*mean)) / n);
}
static void glass(OpenPBRParams *p, float roughness) {
  memset(p, 0, sizeof(*p));
  p->base_weight = 1.0f;
  p->base_color = v3_splat(1.0f);
  p->specular_weight = 1.0f;
  p->specular_color = v3_splat(1.0f);
  p->specular_roughness = roughness;
  p->specular_ior = 1.5f;
  p->transmission = 1.0f;
  p->transmission_color = v3_splat(1.0f);
  p->coat_ior = 1.5f;
  p->coat_color = v3_splat(1.0f);
  p->thin_film_ior = 1.5f;
  p->normal = v3_make(0.0f, 0.0f, 1.0f);
  p->opacity = 1.0f;
  p->openpbr_coat_remap = 1;
}


typedef struct { const char *name; void (*set)(OpenPBRParams *); } Case;
static void c_glass(OpenPBRParams *p) { (void)p; }
static void c_coat(OpenPBRParams *p) { p->coat_weight = 1.0f; p->coat_roughness = 0.1f; p->coat_ior = 1.6f; p->coat_darkening = 1.0f; }
static void c_coat_half(OpenPBRParams *p) { p->coat_weight = 0.5f; p->coat_roughness = 0.4f; p->coat_ior = 1.5f; p->coat_darkening = 1.0f; }
static void c_sheen(OpenPBRParams *p) { p->sheen_weight = 1.0f; p->sheen_color = v3_splat(1.0f); p->sheen_roughness = 0.5f; }
static void c_specw(OpenPBRParams *p) { p->specular_weight = 0.5f; }
static void c_specw2(OpenPBRParams *p) { p->specular_weight = 1.0f; p->specular_ior = 2.4f; }
static void c_trans_half(OpenPBRParams *p) { p->transmission = 0.5f; p->base_color = v3_splat(1.0f); }
static void c_metal(OpenPBRParams *p) { p->transmission = 0.0f; p->metalness = 1.0f; p->base_color = v3_splat(1.0f); }
static void c_metal_film(OpenPBRParams *p) { c_metal(p); p->thin_film_weight = 1.0f; p->thin_film_thickness = 400.0f; p->thin_film_ior = 1.4f; }
static void c_opaque_film(OpenPBRParams *p) { p->transmission = 0.0f; p->base_color = v3_splat(1.0f); p->thin_film_weight = 1.0f; p->thin_film_thickness = 400.0f; p->thin_film_ior = 1.4f; }
static void c_glass_film(OpenPBRParams *p) { p->thin_film_weight = 1.0f; p->thin_film_thickness = 400.0f; p->thin_film_ior = 1.4f; }
static void c_coat_glass_film(OpenPBRParams *p) { c_coat(p); c_glass_film(p); }
static void c_coat_metal(OpenPBRParams *p) { c_metal(p); c_coat(p); }
static void c_sheen_metal(OpenPBRParams *p) { c_metal(p); c_sheen(p); }
static void c_diffuse(OpenPBRParams *p) { p->transmission = 0.0f; p->base_color = v3_splat(1.0f); }
static void c_diffuse_coat(OpenPBRParams *p) { c_diffuse(p); c_coat(p); }
static void c_diffuse_sheen(OpenPBRParams *p) { c_diffuse(p); c_sheen(p); }
static void c_sss(OpenPBRParams *p) { c_diffuse(p); p->subsurface = 1.0f; p->subsurface_color = v3_splat(1.0f);
  p->subsurface_radius = v3_splat(1.0f); p->subsurface_scale = 1.0f; }
int main(void) {
  bsdf_set_energy_mode(1);
  Case cs[] = {{"glass",c_glass},{"coat1",c_coat},{"coat.5",c_coat_half},{"sheen",c_sheen},{"specw.5",c_specw},{"ior2.4",c_specw2},{"trans.5-white",c_trans_half},{"metal-white",c_metal},{"opaque-white-film",c_opaque_film},{"glass-film",c_glass_film},
    {"coat-metal",c_coat_metal},{"sheen-metal",c_sheen_metal},{"diffuse-white",c_diffuse},
    {"diffuse+coat",c_diffuse_coat},{"diffuse+sheen",c_diffuse_sheen},{"subsurface-white",c_sss}};
  const float rough[] = {0.02f, 0.1f, 0.4f};
  const float views[] = {0.1f, 0.4f, 0.8f, 1.0f};
  int fail = 0;
  for (size_t k = 0; k < sizeof cs / sizeof *cs; ++k) {
    double worst = 0, wse = 0;
    for (int side = 0; side < 2; ++side) for (int r = 0; r < 3; ++r) for (int v = 0; v < 4; ++v) {
      OpenPBRParams p; glass(&p, rough[r]); cs[k].set(&p);
      if (side && p.transmission <= 0.0f) continue;
      double m, se; mc_albedo(&p, side ? v3_make(0,0,-1) : v3_make(0,0,1), views[v], 400000, &m, &se);
      if (m - 3 * se > worst - 3 * wse) { worst = m; wse = se; }
    }
    const int bad = worst - 3 * wse > 1.003;
    fail |= bad;
    printf("%-18s max %.4f +- %.4f%s\n", cs[k].name, worst, wse, bad ? "  FAIL" : "");
  }
  printf("conservative sweep: %s\n", fail ? "FAIL" : "ok");
  return fail;
}
