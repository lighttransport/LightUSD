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
  double xsum = 0.0, ysum = 0.0, zsum = 0.0;

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
      xsum += (double)f.x * mu * dmu * dphi;
      ysum += (double)f.y * mu * dmu * dphi;
      zsum += (double)f.z * mu * dmu * dphi;
    }
  }
  return v3_make((float)xsum, (float)ysum, (float)zsum);
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
  p->coat_darkening = 1.0f;
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

  // The OpenPBR graph attenuates the whole base by a bounded coat-darkening
  // ratio. Check its per-channel values and authored interpolation amount.
  {
    FurnaceCase c = {"coat-formula", 0.6f, 0.25f, 0.3f, 0.8f, 0.0f, 0.7f, 0};
    OpenPBRParams p; set_defaults(&p, &c);
    p.base_color = v3_make(0.2f, 0.4f, 0.6f);
    p.subsurface_color = v3_make(0.4f, 0.5f, 0.7f);
    const v3 n = v3_make(0.0f, 0.0f, 1.0f);
    v3 baseline, spec; float pdf;
    p.coat_darkening = 0.0f;
    bsdf_eval_lobes(&p, n, n, n, &baseline, &spec, &pdf);
    const float base[] = {0.2f, 0.4f, 0.6f};
    const float subsurface[] = {0.4f, 0.5f, 0.7f};
    const float f0 = (1.5f - 1.0f) / (1.5f + 1.0f);
    const float k = 1.0f - (1.0f - f0 * f0) / (1.5f * 1.5f);
    for (int step = 1; step <= 2; ++step) {
      p.coat_darkening = 0.5f * (float)step;
      v3 diff;
      bsdf_eval_lobes(&p, n, n, n, &diff, &spec, &pdf);
      const float b[] = {baseline.x, baseline.y, baseline.z};
      const float d[] = {diff.x, diff.y, diff.z};
      for (int ch = 0; ch < 3; ++ch) {
        const float dielectric = (1.0f - c.subsurface) * base[ch] + c.subsurface * subsurface[ch];
        const float albedo = (1.0f - c.metal) * dielectric + c.metal * base[ch] * c.specular_weight;
        const float factor = (1.0f - k) / (1.0f - albedo * k);
        const float expected = b[ch] * (1.0f + c.coat * p.coat_darkening * (factor - 1.0f));
        if (fabsf(d[ch] - expected) > 1.0e-6f) {
          fprintf(stderr, "FAIL: coat darkening channel=%d got=%g expected=%g\n", ch, d[ch], expected);
          return 1;
        }
      }
    }
    printf("PASS: OpenPBR coat darkening and authored interpolation\n");
  }

  // Reference graph values for the coat-relative IOR and coat-affected
  // specular roughness, covering both sides of the IOR ratio and saturation.
  {
    const float cases[][6] = {{1.5f, 1.6f, 0.8f, 0.3f, 0.2f, 1.153333333f},
                              {1.8f, 1.2f, 0.4f, 0.7f, 0.4f, 1.68f},
                              {1.5f, 1.5f, 1.0f, 0.2f, 0.9f, 1.0f}};
    const float roughness[] = {0.320831235f, 0.713863138f, 1.0f};
    const v3 n = v3_make(0, 0, 1), view = v3_make(0.6f, 0, 0.8f);
    const v3 light = v3_make(0, 0.6f, 0.8f);
    for (size_t i = 0; i < sizeof(roughness)/sizeof(roughness[0]); ++i) {
      FurnaceCase c = {"coat-remap", 0.6f, 0.0f, 0.0f, cases[i][2], 0.0f, 1.0f, 0};
      OpenPBRParams actual; set_defaults(&actual, &c);
      actual.openpbr_coat_remap = 1;
      actual.specular_ior = cases[i][0]; actual.coat_ior = cases[i][1];
      actual.specular_roughness = cases[i][3]; actual.coat_roughness = cases[i][4];
      OpenPBRParams expected = actual;
      expected.openpbr_coat_remap = 0;
      expected.specular_ior = cases[i][5]; expected.specular_roughness = roughness[i];
      float actual_pdf, expected_pdf;
      const v3 a = bsdf_eval(&actual, n, view, light, &actual_pdf);
      const v3 b = bsdf_eval(&expected, n, view, light, &expected_pdf);
      if (fabsf(a.x-b.x) > 1.0e-5f || fabsf(a.y-b.y) > 1.0e-5f ||
          fabsf(a.z-b.z) > 1.0e-5f || fabsf(actual_pdf-expected_pdf) > 1.0e-5f) {
        fprintf(stderr, "FAIL: OpenPBR coat IOR/roughness remap case=%zu\n", i);
        return 1;
      }
    }
    printf("PASS: OpenPBR coat IOR and roughness remapping\n");
  }

  // Invalid coat roughness must not poison an uncoated BSDF through 0 * NaN,
  // and the active-coat path uses a finite roughness fallback as well.
  {
    FurnaceCase c = {"coat-nonfinite", 0.6f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0};
    OpenPBRParams p; set_defaults(&p, &c);
    p.openpbr_coat_remap = 1;
    p.coat_roughness = NAN;
    const v3 n = v3_make(0, 0, 1), view = v3_make(0.6f, 0, 0.8f);
    for (int i = 0; i < 2; ++i) {
      p.coat_weight = i ? 0.8f : 0.0f;
      float pdf;
      v3 result = bsdf_eval(&p, n, view, n, &pdf);
      if (!v3_is_finite(result) || !isfinite(pdf)) {
        fprintf(stderr, "FAIL: nonfinite coat roughness at weight %.1f\n", p.coat_weight);
        return 1;
      }
    }
    printf("PASS: nonfinite coat roughness fallback\n");
  }

  // Sweep the principal base/specular controls so the furnace bound also
  // covers combinations between the named material fixtures above.
  const v3 sweep_colors[] = {
      {0.95f, 0.75f, 0.55f},
      {0.35f, 0.70f, 0.90f},
  };
  const float sweep_metals[] = {0.0f, 0.5f, 1.0f};
  const float sweep_specular[] = {0.0f, 0.5f, 1.0f};
  const float sweep_roughness[] = {0.05f, 0.35f, 1.0f};
  size_t sweep_count = 0;
  for (size_t ci = 0; ci < sizeof(sweep_colors) / sizeof(sweep_colors[0]); ++ci) {
    for (size_t mi = 0; mi < sizeof(sweep_metals) / sizeof(sweep_metals[0]); ++mi) {
      for (size_t si = 0; si < sizeof(sweep_specular) / sizeof(sweep_specular[0]); ++si) {
        for (size_t ri = 0; ri < sizeof(sweep_roughness) / sizeof(sweep_roughness[0]); ++ri) {
          OpenPBRParams p;
          set_defaults(&p, &cases[0]);
          p.base_color = sweep_colors[ci];
          p.metalness = sweep_metals[mi];
          p.specular_weight = sweep_specular[si];
          p.specular_roughness = sweep_roughness[ri];
          for (size_t vi = 0; vi < sizeof(views) / sizeof(views[0]); ++vi) {
            const v3 total = integrate(&p, views[vi], 0);
            const float energy[] = {total.x, total.y, total.z};
            for (size_t channel = 0; channel < 3; ++channel) {
              if (!isfinite(energy[channel]) || energy[channel] < -tolerance ||
                  energy[channel] > 1.0f + tolerance) {
                fprintf(stderr,
                        "FAIL: parameter sweep color=%zu metal=%.2f "
                        "specular=%.2f roughness=%.2f view_cos=%.2f "
                        "channel=%zu energy=%.6f\n",
                        ci, sweep_metals[mi], sweep_specular[si],
                        sweep_roughness[ri], views[vi], channel,
                        energy[channel]);
                return 1;
              }
            }
            ++sweep_count;
          }
        }
      }
    }
  }
  printf("PASS: parameter sweep %zu material/view combinations\n", sweep_count);

  // A zero-specular, zero-subsurface, zero-metal OpenPBR base is Lambert:
  // integrating f_r * cos(theta) over a uniform white hemisphere must return
  // its base color at every view angle. This gives the validation renderer an
  // analytic reference before display encoding.
  OpenPBRParams lambert;
  memset(&lambert, 0, sizeof(lambert));
  lambert.base_weight = 1.0f;
  lambert.base_color = v3_make(0.2f, 0.4f, 0.6f);
  lambert.specular_weight = 0.0f;
  lambert.specular_color = v3_splat(1.0f);
  lambert.specular_roughness = 1.0f;
  lambert.specular_ior = 1.5f;
  lambert.transmission_color = v3_splat(1.0f);
  lambert.subsurface_color = v3_splat(1.0f);
  lambert.subsurface_radius = v3_splat(1.0f);
  lambert.coat_color = v3_splat(1.0f);
  lambert.coat_ior = 1.5f;
  lambert.sheen_color = v3_splat(1.0f);
  lambert.sheen_roughness = 0.5f;
  lambert.normal = v3_make(0.0f, 0.0f, 1.0f);
  lambert.opacity = 1.0f;
  const v3 expected = lambert.base_color;
  const float lambert_views[] = {1.0f, 0.2f};
  for (size_t vi = 0;
       vi < sizeof(lambert_views) / sizeof(lambert_views[0]); ++vi) {
    const v3 actual = integrate(&lambert, lambert_views[vi], 0);
    if (fabsf(actual.x - expected.x) > 0.002f ||
        fabsf(actual.y - expected.y) > 0.002f ||
        fabsf(actual.z - expected.z) > 0.002f) {
      fprintf(stderr,
              "FAIL: Lambert white-furnace view_cos=%.2f "
              "expected=(%.5f,%.5f,%.5f) actual=(%.5f,%.5f,%.5f)\n",
              lambert_views[vi], expected.x, expected.y, expected.z,
              actual.x, actual.y, actual.z);
      return 1;
    }
    printf("PASS: Lambert white-furnace view_cos=%.2f "
           "albedo=(%.3f,%.3f,%.3f)\n",
           lambert_views[vi], actual.x, actual.y, actual.z);
  }
  return 0;
}
