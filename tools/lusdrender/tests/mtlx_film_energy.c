// Thin film on OpenPBR glass must not create energy: reflection uses the film
// Fresnel, so transmission has to use 1 minus the same Fresnel (issues/lightrt/07).
// Full-sphere quadrature of R + T per channel with film weight 1 versus no film. The
// interface is lossless, so each channel's albedo must stay <= 1, up to the fitted
// MaterialX reflection compensation (reference mode; integrated-energy gate: 1.0036):
// we require film <= 1.005 + quadrature error. Built with -DBSDF_TEST_PRE_FILM_FIX the
// old transmission (plain Fresnel) is restored, to show the check detects it.
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
  const float views[] = {0.2f, 0.5f, 0.8f, 1.0f};
  const float thick[] = {250.0f, 400.0f, 600.0f};
  int fail = 0;
  double worst = 0.0;
  for (int side = 0; side < 2; ++side) {
    const v3 Ns = side ? v3_make(0, 0, -1) : v3_make(0, 0, 1);
    for (size_t t = 0; t < sizeof thick / sizeof *thick; ++t)
      for (size_t v = 0; v < sizeof views / sizeof *views; ++v) {
        OpenPBRParams p;
        glass(&p, 0.5f);
        const v3 plain = albedo_rgb(&p, Ns, views[v], 384);
        p.thin_film_weight = 1.0f;
        p.thin_film_thickness = thick[t];
        p.thin_film_ior = 1.3f;
        const v3 film = albedo_rgb(&p, Ns, views[v], 384);
        const v3 half = albedo_rgb(&p, Ns, views[v], 192);
        const float c[3] = {film.x - plain.x, film.y - plain.y, film.z - plain.z};
        const float q = fmaxf(fabsf(film.x - half.x), fmaxf(fabsf(film.y - half.y), fabsf(film.z - half.z)));
        const float fc[3] = {film.x, film.y, film.z};
        (void)c;
        for (int k = 0; k < 3; ++k) {
          worst = fmax(worst, fc[k]);
          if (fc[k] > 1.005f + q) fail = 1;
        }
        printf("%s t=%3.0f mu=%.1f plain=(%.4f %.4f %.4f) film=(%.4f %.4f %.4f)\n",
               side ? "exit " : "enter", thick[t], views[v], plain.x, plain.y, plain.z,
               film.x, film.y, film.z);
      }
  }
  printf("max film albedo %.5f: %s\n", worst, fail ? "FAIL" : "ok");
  return fail;
}
