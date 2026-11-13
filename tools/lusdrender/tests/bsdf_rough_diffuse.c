/* Compare isolated rough diffuse lobes against the MaterialX reference formulas. */
#include "bsdf.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static double reference(int model, double rho, double rough, v3 wo, v3 wi) {
  const double pi = 3.14159265358979323846;
  double nv = wo.z, nl = wi.z;
  double lv = (double)wo.x * wi.x + (double)wo.y * wi.y + (double)wo.z * wi.z;
  if (model == MTLX_DIFFUSE_OREN_NAYAR) {
    double r2 = rough * rough, s = lv - nl * nv;
    double a = 1.0 - 0.5 * r2 / (r2 + 0.33);
    double b = 0.45 * r2 / (r2 + 0.09);
    return rho / pi * (a + b * (s > 0.0 ? s / fmax(nl, nv) : 0.0));
  }
  double hx = wo.x + wi.x, hy = wo.y + wi.y, hz = wo.z + wi.z;
  double length = sqrt(hx * hx + hy * hy + hz * hz);
  double lh = ((double)wi.x * hx + (double)wi.y * hy + (double)wi.z * hz) / length;
  double f90 = 0.5 + 2.0 * rough * lh * lh;
  return rho / pi * (1.0 + (f90 - 1.0) * pow(1.0 - nv, 5.0)) *
                    (1.0 + (f90 - 1.0) * pow(1.0 - nl, 5.0));
}

int main(void) {
  const v3 n = {0,0,1};
  const v3 views[] = {{0,0,1}, {0.8f,0,0.6f}, {0.96f,0,0.28f}};
  const v3 lights[] = {{0,0,1}, {0.6f,0,0.8f}, {-0.6f,0,0.8f}, {0,0.8f,0.6f}};
  const float roughness[] = {0, 0.2f, 0.75f, 1.0f};
  const int models[] = {MTLX_DIFFUSE_OREN_NAYAR, MTLX_DIFFUSE_BURLEY};
  OpenPBRParams p; memset(&p, 0, sizeof(p));
  p.base_weight = 0.6f; p.base_color = v3_make(0.2f,0.6f,0.9f);
  p.specular_ior = 1.5f; p.specular_roughness = 0.3f; p.coat_ior = 1.5f; p.normal = n;
  int count = 0;
  for (size_t m=0;m<2;m++) for (size_t r=0;r<4;r++)
    for (size_t v=0;v<3;v++) for (size_t l=0;l<4;l++) {
      p.diffuse_model=models[m]; p.diffuse_roughness=roughness[r];
      v3 diffuse, specular; float pdf;
      bsdf_eval_lobes(&p,n,views[v],lights[l],&diffuse,&specular,&pdf);
      float got[] = {diffuse.x,diffuse.y,diffuse.z}, rho[] = {0.2f,0.6f,0.9f};
      for (int c=0;c<3;c++) {
        double expected = 0.6 * reference(models[m],rho[c],roughness[r],views[v],lights[l]);
        if (!isfinite(got[c]) || fabs(got[c]-expected)>3e-5) {
          fprintf(stderr,"FAIL model=%d rough=%g view=%zu light=%zu channel=%d actual=%.9g expected=%.9g\n",models[m],roughness[r],v,l,c,got[c],expected);
          return 1;
        }
      }
      count++;
    }
  printf("PASS: %d Oren-Nayar and Burley point-response cases\n",count);
  return 0;
}
