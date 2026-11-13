/* Independent high-precision values of the MaterialX genglsl isotropic
 * conductor BRDF, without thin film, generated from the published Fresnel,
 * GGX G2/NDF and directional-albedo fit equations. */
#include "bsdf.h"
#include <math.h>
#include <stdio.h>

typedef struct { float input[5]; float rgb[3]; } Case;
static const Case cases[] = {
  {{0.1f, 1.0f, 1.0f, 1.0f, 1.0f}, {7.501417365f, 6.167912038f, 2.968908431f}},
  {{0.5f, 0.6f, 0.8f, 0.94f, 0.8f}, {0.4317698469f, 0.3370515567f, 0.1430782443f}},
  {{1.0f, 0.2f, 0.4f, 0.75f, 0.55f}, {0.3818812079f, 0.2975940544f, 0.1309786071f}},
  {{0.025f, 0.02f, 0.05f, 0.9f, 0.7f}, {0.9948293783f, 0.8125264807f, 0.394488943f}},
  {{0.001f, 1.0f, 1.0f, 1.0f, 1.0f}, {74963.24258f, 61644.33213f, 29680.50927f}},
  {{0.7f, 0.8f, 0.8f, 0.8f, 1.0f}, {0.2079236958f, 0.1576202597f, 0.0604937605f}},
};
/* Independent 60-digit Decimal reference, in tangent coordinates. */
static const Case anisotropic_cases[] = {
  {{0.1f, 0.6f, 0.48f, 0.64f, 0.6f}, {0.004748362630f, 0.003825566925f, 0.001769913407f}},
  {{0.1f, 0.6f, 0.64f, 0.48f, 0.6f}, {0.001623840532f, 0.001308263735f, 0.0006052733021f}},
  {{0.6f, 0.1f, 0.48f, 0.64f, 0.6f}, {0.001623840532f, 0.001308263735f, 0.0006052733021f}},
  {{0.025f, 0.8f, 0.6f, 0.0f, 0.8f}, {0.00002458192447f, 0.00002002424994f, 0.000009525549355f}},
};
int main(void) {
  v3 eta = {0.183f, 0.421f, 1.373f}, k = {3.424f, 2.346f, 1.770f};
  for (size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
    const float *q=cases[i].input;
    v3 value=bsdf_materialx_conductor(eta,k,q[0],q[1],q[2],q[3],q[4]);
    const float actual[]={value.x,value.y,value.z};
    for (int c=0;c<3;c++) {
      const float ref=cases[i].rgb[c], tolerance=2e-5f*fmaxf(1.0f,fabsf(ref));
      if (!isfinite(actual[c]) || fabsf(actual[c]-ref)>tolerance) {
        fprintf(stderr,"conductor case %zu channel %d: got %g expected %g\n",i,c,actual[c],ref); return 1;
      }
    }
  }
  for (size_t i=0;i<sizeof(anisotropic_cases)/sizeof(*anisotropic_cases);++i) {
    const float *q=anisotropic_cases[i].input;
    v3 value=bsdf_materialx_conductor_anisotropic(eta,k,q[0],q[1],.6f,.8f,q[2],q[3],q[4],.8f);
    const float actual[]={value.x,value.y,value.z};
    for (int c=0;c<3;c++) {
      float ref=anisotropic_cases[i].rgb[c], tolerance=2e-5f*fmaxf(1e-4f,fabsf(ref));
      if (!isfinite(actual[c]) || fabsf(actual[c]-ref)>tolerance) {
        fprintf(stderr,"anisotropic conductor case %zu channel %d: got %g expected %g\n",i,c,actual[c],ref); return 1;
      }
    }
  }
  v3 boundary=bsdf_materialx_conductor(eta,k,0,1,1,1,1);
  if (!v3_is_finite(boundary) || boundary.x <= 0) {
    fprintf(stderr,"smooth-conductor limit must remain finite\n"); return 1;
  }
  boundary=bsdf_materialx_conductor(eta,k,.5f,0,1,1,1);
  if (boundary.x != 0 || boundary.y != 0 || boundary.z != 0) {
    fprintf(stderr,"conductor response must vanish outside the hemisphere\n"); return 1;
  }
  puts("PASS: MaterialX isotropic and anisotropic conductor reference responses");
  return 0;
}
