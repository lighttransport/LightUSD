// Deterministic cosine-weighted solid-angle integration for LightRT's CPU BSDF.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsdf.h"

typedef struct {
  const char *name;
  float base;
  float metal;
  float subsurface;
  float coat;
  float sheen;
  float specular_weight;
  int colored;
} FurnaceCase;

static v3 integrate(const OpenPBRParams *p, float view_cosine, int lobe) {
  const int n_mu = 256;
  const int n_phi = 512;
  const float dmu = 1.0f / (float)n_mu;
  const float dphi = MTLX_TWO_PI / (float)n_phi;
  const float view_sine = sqrtf(fmaxf(0.0f, 1.0f - view_cosine * view_cosine));
  const v3 N = v3_make(0.0f, 0.0f, 1.0f);
  const v3 wo = v3_make(view_sine, 0.0f, view_cosine);
  v3 result = v3_splat(0.0f);

  for (int y = 0; y < n_mu; ++y) {
    const float mu = ((float)y + 0.5f) * dmu;
    const float radial = sqrtf(fmaxf(0.0f, 1.0f - mu * mu));
    for (int x = 0; x < n_phi; ++x) {
      const float phi = ((float)x + 0.5f) * dphi;
      const v3 wi = v3_make(radial * cosf(phi), radial * sinf(phi), mu);
      v3 diffuse, specular;
      float pdf = 0.0f;
      (void)bsdf_eval_lobes(p, N, wo, wi, &diffuse, &specular, &pdf);
      const v3 f = lobe == 0 ? v3_add(diffuse, specular)
                             : (lobe == 1 ? diffuse : specular);
      result = v3_add(result, v3_scale(f, mu * dmu * dphi));
    }
  }
  return result;
}

static void set_defaults(OpenPBRParams *p, const FurnaceCase *c) {
  memset(p, 0, sizeof(*p));
  p->base_weight = 1.0f;
  p->base_color = c->colored ? v3_make(0.97f, 0.55f, 0.18f)
                             : v3_splat(c->base);
  p->metalness = c->metal;
  p->specular_weight = c->specular_weight;
  p->specular_color = c->colored ? v3_make(0.3f, 0.7f, 1.0f)
                                 : v3_splat(1.0f);
  p->specular_roughness = 0.35f;
  p->specular_ior = 1.5f;
  p->transmission_color = v3_splat(1.0f);
  p->subsurface = c->subsurface;
  p->subsurface_color = v3_splat(c->base);
  p->subsurface_radius = v3_splat(1.0f);
  p->subsurface_scale = 1.0f;
  p->coat_weight = c->coat;
  p->coat_color = v3_splat(1.0f);
  p->coat_roughness = 0.2f;
  p->coat_ior = 1.5f;
  p->sheen_weight = c->sheen;
  p->sheen_color = v3_splat(1.0f);
  p->sheen_roughness = 0.5f;
  p->thin_film_ior = 1.5f;
  p->normal = v3_make(0.0f, 0.0f, 1.0f);
  p->opacity = 1.0f;
}

int main(void) {
  const FurnaceCase cases[] = {
      {"diffuse", 0.95f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0},
      {"no-specular", 0.95f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0},
      {"dielectric", 0.75f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0},
      {"metal", 0.85f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0},
      {"subsurface", 0.75f, 0.0f, 0.8f, 0.0f, 0.0f, 1.0f, 0},
      {"coat", 0.7f, 0.0f, 0.0f, 0.8f, 0.0f, 1.0f, 0},
      {"sheen", 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 0},
      {"colored", 0.7f, 0.35f, 0.2f, 0.25f, 0.1f, 0.85f, 1},
  };
  const float views[] = {1.0f, 0.2f};
  const float tolerance = 0.01f;

  for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
    OpenPBRParams p;
    set_defaults(&p, &cases[ci]);
    for (size_t vi = 0; vi < sizeof(views) / sizeof(views[0]); ++vi) {
      const v3 total = integrate(&p, views[vi], 0);
      const v3 diffuse = integrate(&p, views[vi], 1);
      const v3 specular = integrate(&p, views[vi], 2);
      const float energy[] = {total.x, total.y, total.z,
                              diffuse.x, diffuse.y, diffuse.z,
                              specular.x, specular.y, specular.z};
      for (size_t k = 0; k < sizeof(energy) / sizeof(energy[0]); ++k) {
        if (!isfinite(energy[k]) || energy[k] < -tolerance ||
            energy[k] > 1.0f + tolerance) {
          fprintf(stderr,
                  "FAIL: %s view_cos=%.2f channel=%zu energy=%.6f\n",
                  cases[ci].name, views[vi], k % 3, energy[k]);
          return 1;
        }
      }
      if (fabsf(total.x - diffuse.x - specular.x) > 2.0e-4f ||
          fabsf(total.y - diffuse.y - specular.y) > 2.0e-4f ||
          fabsf(total.z - diffuse.z - specular.z) > 2.0e-4f) {
        fprintf(stderr, "FAIL: %s split lobes do not sum to total\n",
                cases[ci].name);
        return 1;
      }
      printf("PASS: %s view_cos=%.2f total=(%.5f,%.5f,%.5f) "
             "diffuse=(%.5f,%.5f,%.5f) specular=(%.5f,%.5f,%.5f)\n",
             cases[ci].name, views[vi], total.x, total.y, total.z,
             diffuse.x, diffuse.y, diffuse.z,
             specular.x, specular.y, specular.z);
    }
  }
  return 0;
}
