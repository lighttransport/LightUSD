// Partially transmissive OpenPBR (white base, transmission 0.5) shaded from inside the
// glass must not create energy: past the critical angle the reflection lobe reflects
// nearly everything, so the diffuse base underneath must be attenuated accordingly
// (issues/lightrt/09; previously the full-sphere albedo reached 1.403).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsdf.h"

static v3 albedo_rgb(const OpenPBRParams *p, v3 Ns, float view_cosine, int n) {
  const int n_mu = n, n_phi = 2 * n;
  const double dmu = 1.0 / n_mu, dphi = 2.0 * M_PI / n_phi;
  const float s = sqrtf(fmaxf(0.0f, 1.0f - view_cosine * view_cosine));
  const v3 wo = v3_make(s, 0.0f, view_cosine);
  double sx = 0, sy = 0, sz = 0;
  for (int hemi = 0; hemi < 2; ++hemi) {
    for (int y = 0; y < n_mu; ++y) {
      /* mu = 1 - t^2 concentrates nodes near the poles (the lobes' peaks), weight 2t */
      const double t = (y + 0.5) * dmu, mu = 1.0 - t * t, w_mu = 2.0 * t * dmu;
      const float radial = (float)sqrt(fmax(0.0, 1.0 - mu * mu));
      for (int x = 0; x < n_phi; ++x) {
        const double phi = (x + 0.5) * dphi;
        const float z = hemi ? -(float)mu : (float)mu;
        const v3 wi = v3_make(radial * (float)cos(phi), radial * (float)sin(phi), z);
        v3 d, sp;
        float pdf = 0.0f;
        v3 f = bsdf_eval_lobes(p, Ns, wo, wi, &d, &sp, &pdf);
        sx += (double)f.x * mu * w_mu * dphi; sy += (double)f.y * mu * w_mu * dphi; sz += (double)f.z * mu * w_mu * dphi;
      }
    }
  }
  return v3_make((float)sx, (float)sy, (float)sz);
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

int main(void) {
  const float rough[] = {0.3f, 0.7f};
  const float views[] = {0.1f, 0.3f, 0.5f, 0.8f, 1.0f};
  int fail = 0;
  double worst = 0.0;
  for (size_t r = 0; r < 2; ++r)
    for (size_t v = 0; v < sizeof views / sizeof *views; ++v) {
      OpenPBRParams p;
      glass(&p, rough[r]);
      p.transmission = 0.5f;
      const v3 a = albedo_rgb(&p, v3_make(0, 0, -1), views[v], 512);
      const v3 h = albedo_rgb(&p, v3_make(0, 0, -1), views[v], 256);
      const double q = fabs(a.x - h.x);
      worst = fmax(worst, a.x);
      if (a.x > 1.005 + q) fail = 1;
      printf("exit rough=%.1f mu=%.1f albedo=%.4f (quad %.1e)\n", rough[r], views[v], a.x, q);
    }
  printf("max interior albedo %.5f: %s\n", worst, fail ? "FAIL" : "ok");
  return fail;
}
