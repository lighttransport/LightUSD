// Energy check of --energy-mode conservative for pure OpenPBR glass.
//
// Integrates reflection + transmission (power, f |cos| over the full sphere of incident
// directions) through bsdf_eval_lobes for entering and exiting views. In conservative
// mode the certified table scales both lobes by s = 1/(E_up + reserve), so the exact
// albedo is at most 1. The deterministic quadrature has its own error, estimated from a
// half-resolution run; the check is albedo <= 1 + quadrature-error estimate + 1e-3.
// The test also requires conservative mode to differ from reference mode only inside
// the certified domain (it may only lower the albedo there), and to recover energy inside it.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsdf.h"

static double albedo(const OpenPBRParams *p, v3 Ns, float view_cosine, int n) {
  const int n_mu = n, n_phi = 2 * n;
  const double dmu = 1.0 / n_mu, dphi = 2.0 * M_PI / n_phi;
  const float s = sqrtf(fmaxf(0.0f, 1.0f - view_cosine * view_cosine));
  const v3 wo = v3_make(s, 0.0f, view_cosine);
  double sum = 0.0;
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
        sum += (double)f.x * mu * w_mu * dphi;
      }
    }
  }
  return sum;
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
  const float rough[] = {0.4f, 0.6f, 0.8f, 1.0f};
  const float views[] = {0.15f, 0.35f, 0.6f, 0.85f, 1.0f};
  int fail = 0;
  double worst = 0.0, gain = 0.0;
  for (int side = 0; side < 2; ++side) {
    /* entering: outward normal faces the view; exiting: outward normal faces away */
    const v3 Ns = side ? v3_make(0, 0, -1) : v3_make(0, 0, 1);
    for (size_t r = 0; r < sizeof rough / sizeof *rough; ++r) {
      for (size_t v = 0; v < sizeof views / sizeof *views; ++v) {
        OpenPBRParams p;
        glass(&p, rough[r]);
        bsdf_set_energy_mode(0);
        const double ref = albedo(&p, Ns, views[v], 512);
        bsdf_set_energy_mode(1);
        const double cons = albedo(&p, Ns, views[v], 512);
        const double cons_half = albedo(&p, Ns, views[v], 256);
        const double qerr = fabs(cons - cons_half);
        const int ok = cons <= 1.0 + qerr + 1e-3;
        fail |= !ok;
        worst = fmax(worst, cons);
        gain = fmax(gain, cons - ref);
        printf("%s rough=%.2f mu=%.2f reference=%.5f conservative=%.5f (quad %.1e)%s\n",
               side ? "exit " : "enter", rough[r], views[v], ref, cons, qerr, ok ? "" : "  FAIL");
      }
    }
  }
  /* outside the certified domain (coat) */
  OpenPBRParams p;
  glass(&p, 0.6f);
  p.coat_weight = 0.5f;
  p.coat_roughness = 0.2f;
  bsdf_set_energy_mode(0);
  const double a = albedo(&p, v3_make(0, 0, 1), 0.6f, 128);
  bsdf_set_energy_mode(1);
  const double b = albedo(&p, v3_make(0, 0, 1), 0.6f, 128);
  /* Outside the certified glass domain conservative mode only drops the fitted MaterialX
   * compensation (issues/materialx/03), so it may lower but never raise the albedo. */
  if (!(b <= a + 1e-6 && b <= 1.005)) {
    printf("FAIL: conservative out-of-domain albedo %.5f (reference %.5f)\n", b, a); fail = 1; }
  if (!(gain > 0.0)) { printf("FAIL: conservative mode recovered no energy\n"); fail = 1; }
  printf("max conservative albedo %.5f, max gain over reference %.5f: %s\n", worst, gain,
         fail ? "FAIL" : "ok");
  return fail;
}
