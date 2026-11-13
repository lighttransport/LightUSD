/* Compare the CPU OpenPBR diffuse response with the MaterialX EON equations. */
#include "bsdf.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static double directional(double mu, double sigma) {
  const double pi = 3.14159265358979323846;
  double c1 = 0.5 - 2.0 / (3.0 * pi);
  double a = 1.0 / (1.0 + c1 * sigma);
  double theta = acos(mu), sn = sin(theta);
  double g = sn * (theta - sn * mu) +
      (2.0 / 3.0) * ((sn / mu) * (1.0 - sn * sn * sn) - sn);
  return a * (1.0 + sigma * g / pi);
}

static double reference(double rho, double sigma, v3 wo, v3 wi) {
  const double pi = 3.14159265358979323846;
  if (sigma == 0.0) return rho / pi;
  double c1 = 0.5 - 2.0 / (3.0 * pi), c2 = 2.0 / 3.0 - 28.0 / (15.0 * pi);
  double a = 1.0 / (1.0 + c1 * sigma), avg = a * (1.0 + c2 * sigma);
  double nv = wo.z, nl = wi.z;
  double dot = (double)wo.x * wi.x + (double)wo.y * wi.y + (double)wo.z * wi.z;
  double s = dot - nv * nl, st = s > 0.0 ? s / fmax(nv, nl) : s;
  double ev = directional(nv, sigma), el = directional(nl, sigma);
  double single = rho * a * (1.0 + sigma * st);
  double cm = rho * rho * avg / (1.0 - rho * fmax(0.0, 1.0 - avg));
  double multi = cm * fmax(1.0e-8, 1.0 - ev) * fmax(1.0e-8, 1.0 - el) /
                 fmax(1.0e-8, 1.0 - avg);
  return (single + multi) / pi;
}

int main(void) {
  const v3 n = {0, 0, 1};
  const v3 views[] = {{0,0,1}, {0.8f,0,0.6f}, {0.99995f,0,0.009999875f}};
  const v3 lights[] = {{0,0,1}, {0.6f,0,0.8f}, {-0.6f,0,0.8f}, {0,0.8f,0.6f}};
  const float sigmas[] = {0, 0.2f, 0.75f, 1.0f};
  const float weights[] = {0.25f, 1.0f};
  int count = 0;
  OpenPBRParams p; memset(&p, 0, sizeof(p));
  p.base_color = v3_make(0.2f, 0.6f, 0.9f); p.specular_ior = 1.5f;
  p.specular_roughness = 0.3f; p.coat_ior = 1.5f; p.normal = n;
  p.diffuse_energy_compensation = 1;
  for (size_t a=0;a<sizeof(sigmas)/sizeof(sigmas[0]);a++)
    for (size_t b=0;b<sizeof(weights)/sizeof(weights[0]);b++)
      for (size_t c=0;c<sizeof(views)/sizeof(views[0]);c++)
        for (size_t d=0;d<sizeof(lights)/sizeof(lights[0]);d++) {
          p.diffuse_roughness=sigmas[a]; p.base_weight=weights[b];
          v3 diff, spec; float pdf;
          bsdf_eval_lobes(&p,n,views[c],lights[d],&diff,&spec,&pdf);
          float actual[] = {diff.x,diff.y,diff.z}; float rho[] = {0.2f,0.6f,0.9f};
          for (int k=0;k<3;k++) {
            double expected=weights[b]*reference(rho[k],sigmas[a],views[c],lights[d]);
            if (!isfinite(actual[k]) || fabs(actual[k]-expected)>3e-5) {
              fprintf(stderr,"FAIL EON sigma=%g weight=%g view=%zu light=%zu channel=%d actual=%.9g expected=%.9g\n",sigmas[a],weights[b],c,d,k,actual[k],expected); return 1;
            }
          }
          count++;
        }
  printf("PASS: %d EON smooth/rough, weighted, grazing and azimuth response cases\n",count);
  return 0;
}
