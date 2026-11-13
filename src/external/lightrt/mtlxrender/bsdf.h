/*
 * bsdf.h - OpenPBR-style layered BSDF (metallic-roughness core + dielectric
 * specular + smooth glass transmission + subsurface-as-tinted-diffuse).
 *
 * All directions are world-space; wo points from the surface toward the viewer.
 * The shading normal N is oriented to the wo side by the caller helpers.
 */
#ifndef MTLXRENDER_BSDF_H_
#define MTLXRENDER_BSDF_H_

#include "mtlx_eval.h"
#include "vecmath.h"

typedef struct {
    v3    wi;          /* sampled incident direction (world) */
    v3    throughput;  /* f * |cos| / pdf */
    float pdf;
    int   specular;    /* delta lobe: skip NEE/MIS for this bounce */
    int   transmission;/* glass refraction lobe (ray enters/exits medium) */
    int   crossed;     /* transmission actually refracted across the interface
                          (vs reflected/TIR); toggles the caller's medium state */
    int   subsurface;  /* diffuse lobe flagged as subsurface entry */
} BsdfSample;

typedef struct {
    float diffuse;
    float specular;
    float coat;
    float transmission;
} BsdfLobeWeights;

/* MaterialX Zeltner directional-albedo fit (cosine and roughness in [0,1]). */
float bsdf_zeltner_sheen_albedo(float ndotv, float roughness);

/* Airy Fresnel for a positive-thickness conductor film (thickness in nm). */
v3 bsdf_materialx_conductor_film_fresnel(float cosine, v3 eta, v3 extinction,
                                         float thickness, float film_ior);

/* MaterialX genglsl isotropic conductor BRDF (unit weight, no thin film).
 * alpha is the authored microfacet roughness, not its square. Directions are
 * represented by positive cosine inputs; a nonpositive NdotV/L returns zero. */
v3 bsdf_materialx_conductor(v3 eta, v3 extinction, float alpha,
                           float ndotv, float ndotl, float ndoth, float vdoth);

/* Anisotropic version: H components are expressed in the orthonormal
 * tangent, bitangent, normal frame. Masking follows the reference's geometric
 * mean alpha approximation. */
v3 bsdf_materialx_conductor_anisotropic(v3 eta, v3 extinction, float alpha_x,
    float alpha_y, float ndotv, float ndotl, float hx, float hy, float hz, float vdoth);

/* Importance-sample the BSDF. Returns 0 if no valid sample. */
int bsdf_sample(const OpenPBRParams *p, v3 N, v3 wo, pcg32 *rng, BsdfSample *out);

/* Homogeneous interior medium of a transmissive material (OpenPBR semantics):
 *   sigma_t = -ln(transmission_color)/transmission_depth        (extinction)
 *   sigma_s = min(transmission_scatter/transmission_depth, sigma_t) (scattering)
 *   sigma_a = sigma_t - sigma_s                                  (absorption)
 * g is the Henyey-Greenstein anisotropy. All zero when transmission_depth <= 0. */
typedef struct {
    v3    sigma_t;   /* extinction coefficient (per channel) */
    v3    sigma_s;   /* scattering coefficient (per channel) */
    float g;         /* phase anisotropy */
} VolumeMedium;

VolumeMedium transmission_medium(const OpenPBRParams *p);

/* Evaluate the non-delta BSDF (diffuse + rough specular) for NEE/MIS.
 * Returns f (rgb); *pdf_out gets the mixture pdf for sampling wi. */
v3 bsdf_eval(const OpenPBRParams *p, v3 N, v3 wo, v3 wi, float *pdf_out);

/* Same, but reports the diffuse and specular halves separately (either out
 * pointer may be NULL; the return value is still their sum, and *pdf_out is the
 * same mixture pdf). A split-sum IBL weighs the diffuse lobe against the
 * irradiance map and the specular lobe against the prefiltered reflection map --
 * two different environments -- so it must not be handed the sum, or each lobe
 * gets counted against both. Sheen and coat are grouped with the specular half. */
v3 bsdf_eval_lobes(const OpenPBRParams *p, v3 N, v3 wo, v3 wi, v3 *out_diffuse,
                   v3 *out_specular, float *pdf_out);

/* Probabilities used to select diffuse, specular, coat and transmission
 * lobes during importance sampling. These are diagnostic weights, not
 * integrated energy fractions. Explicit conductor proposals are included in
 * the reported specular probability. */
BsdfLobeWeights bsdf_lobe_weights(const OpenPBRParams *p);

#endif /* MTLXRENDER_BSDF_H_ */
