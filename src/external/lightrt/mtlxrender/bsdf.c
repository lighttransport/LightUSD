#include "bsdf.h"

#include <math.h>

/* ---- microfacet helpers ----------------------------------------------- */

static v3 fresnel_schlick(float cos_t, v3 f0, float f90) {
    float m = clampf(1.0f - cos_t, 0.0f, 1.0f);
    float m5 = m * m * m * m * m;
    return v3_add(f0, v3_scale(v3_sub(v3_splat(f90), f0), m5));
}
static float fresnel_schlick_s(float cos_t, float f0) {
    float m = clampf(1.0f - cos_t, 0.0f, 1.0f);
    float m5 = m * m * m * m * m;
    return f0 + (1.0f - f0) * m5;
}
static float fresnel_dielectric(float cosi, float eta) {
    float sint2 = eta * eta * (1.0f - cosi * cosi);
    if (sint2 >= 1.0f) return 1.0f; /* TIR */
    float cost = sqrtf(1.0f - sint2);
    float rs = (eta * cosi - cost) / (eta * cosi + cost);
    float rp = (cosi - eta * cost) / (cosi + eta * cost);
    return 0.5f * (rs * rs + rp * rp);
}
static float ggx_D(float NdotH, float a) {
    if (NdotH <= 0.0f) return 0.0f;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (MTLX_PI * d * d);
}
/* Height-correlated Smith G2 for unit directions and isotropic GGX. */
static float ggx_G(float NdotV, float NdotL, float a) {
    if (NdotV <= 0.0f || NdotL <= 0.0f) return 0.0f;
    float a2 = a * a;
    float lv = sqrtf(a2 + (1.0f - a2) * NdotV * NdotV);
    float ll = sqrtf(a2 + (1.0f - a2) * NdotL * NdotL);
    return 2.0f * NdotV * NdotL / (NdotV * ll + NdotL * lv);
}

/* "Charlie" sheen NDF (Estevez & Kulla 2017) + Neubelt visibility. */
static float charlie_D(float NdotH, float r) {
    r = clampf(r, 0.05f, 1.0f);
    float inv = 1.0f / r;
    float sin2 = maxf(0.0f, 1.0f - NdotH * NdotH);
    return (2.0f + inv) * powf(sin2, inv * 0.5f) * (1.0f / (2.0f * MTLX_PI));
}
static float sheen_V(float NdotV, float NdotL) {
    return 1.0f / (4.0f * (NdotL + NdotV - NdotL * NdotV) + 1e-4f);
}

/* ---- thin-film iridescence (Belcour & Barla 2017) --------------------- */
/* Port of MaterialX's mx_fresnel_airy for the dielectric model: a thin film of
 * index tf_ior and thickness (nm) over a dielectric substrate of index
 * ior_substrate. Wave interference of the two interface reflections produces the
 * view-dependent rainbow sheen. For a dielectric substrate (extinction = 0) the
 * per-interface reflectances and phase shifts are scalar; the spectral content
 * comes only from the Gaussian sensitivity fit, so we accumulate in XYZ and
 * convert to RGB at the end. Returns the reflectance that replaces the plain
 * Fresnel term of the specular lobe. */

/* Polarized dielectric Fresnel reflectance (parallel, perpendicular) at a
 * relative index `ior`. */
static void fresnel_dielectric_pol(float cosTheta, float ior, float *Rp, float *Rs) {
    float c = clampf(cosTheta, 0.0f, 1.0f);
    float c2 = c * c, s2 = 1.0f - c2;
    float t0 = maxf(ior * ior - s2, 0.0f);
    float t1 = t0 + c2;
    float t2 = 2.0f * sqrtf(t0) * c;
    float rs = (t1 - t2) / (t1 + t2);
    float t3 = c2 * t0 + s2 * s2;
    float t4 = t2 * s2;
    *Rs = rs; *Rp = rs * (t3 - t4) / (t3 + t4);
}

/* Second-interface (film->substrate) polarized reflectance, dielectric (k=0):
 * the conductor form with zero extinction, which handles eta_sub<eta_film TIR. */
static void fresnel_substrate_pol(float cosTheta, float n, float *Rp, float *Rs) {
    float c = clampf(cosTheta, 0.0f, 1.0f);
    float c2 = c * c, s2 = 1.0f - c2;
    float t0 = n * n - s2;
    float a2b2 = fabsf(t0); /* sqrt(t0*t0), k=0 */
    float t1 = a2b2 + c2;
    float a = sqrtf(maxf(0.5f * (a2b2 + t0), 0.0f));
    float t2 = 2.0f * a * c;
    float rs = (t1 - t2) / (t1 + t2);
    float t3 = c2 * a2b2 + s2 * s2;
    float t4 = t2 * s2;
    *Rs = rs; *Rp = rs * (t3 - t4) / (t3 + t4);
}

/* Phase shift of the second-interface reflection (conductor phase, k=0). */
static void fresnel_substrate_phase(float cosTheta, float eta_film, float eta_sub,
                                    float *phiP, float *phiS) {
    float c = cosTheta, sin2 = 1.0f - c * c;
    float A = eta_sub * eta_sub - eta_film * eta_film * sin2;
    float B = fabsf(A);
    float U = sqrtf(maxf((A + B) * 0.5f, 0.0f));
    float V = maxf(0.0f, sqrtf(maxf((B - A) * 0.5f, 0.0f)));
    *phiS = atan2f(2.0f * eta_film * V * c, U * U + V * V - eta_film * eta_film * c * c);
    float es2 = eta_sub * eta_sub;
    *phiP = atan2f(-2.0f * eta_film * es2 * c * V,
                   es2 * es2 * c * c - eta_film * eta_film * (U * U + V * V));
}

/* Gaussian-fit spectral sensitivity (XYZ) of the human eye to an optical path
 * difference, with a constant phase shift. */
static v3 eval_sensitivity(float opd, float shift) {
    float phase = 2.0f * MTLX_PI * opd, p2 = phase * phase;
    const float valx = 5.4856e-13f, valy = 4.4201e-13f, valz = 5.2481e-13f;
    const float posx = 1.6810e+06f, posy = 1.7953e+06f, posz = 2.2084e+06f;
    const float varx = 4.3278e+09f, vary = 9.3046e+09f, varz = 6.6121e+09f;
    float x = valx * sqrtf(2.0f * MTLX_PI * varx) * cosf(posx * phase + shift) * expf(-varx * p2);
    float y = valy * sqrtf(2.0f * MTLX_PI * vary) * cosf(posy * phase + shift) * expf(-vary * p2);
    float z = valz * sqrtf(2.0f * MTLX_PI * varz) * cosf(posz * phase + shift) * expf(-varz * p2);
    x += 9.7470e-14f * sqrtf(2.0f * MTLX_PI * 4.5282e+09f) * cosf(2.2399e+06f * phase + shift) * expf(-4.5282e+09f * p2);
    return v3_scale(v3_make(x, y, z), 1.0f / 1.0685e-7f);
}

/* CIE-XYZ(E illuminant) -> linear RGB, the matrix MaterialX uses in the airy
 * model (column-major mat3 * vec). */
static v3 xyz_to_rgb_airy(v3 c) {
    return v3_make(2.3706743f * c.x - 0.9000405f * c.y - 0.4706338f * c.z,
                  -0.5138850f * c.x + 1.4253036f * c.y + 0.0885814f * c.z,
                   0.0052982f * c.x - 0.0146949f * c.y + 1.0093968f * c.z);
}

static v3 fresnel_airy(float cosTheta, float ior_substrate, float tf_thickness_nm, float tf_ior) {
    float eta1 = 1.0f, eta2 = maxf(tf_ior, eta1), eta3 = ior_substrate;
    cosTheta = clampf(cosTheta, 0.0f, 1.0f);
    float cosTt2 = 1.0f - (1.0f - cosTheta * cosTheta) * (eta1 / eta2) * (eta1 / eta2);
    float cosThetaT = cosTt2 > 0.0f ? sqrtf(cosTt2) : 0.0f;

    float R12p, R12s;
    fresnel_dielectric_pol(cosTheta, eta2 / eta1, &R12p, &R12s);
    if (cosThetaT <= 0.0f) { R12p = 1.0f; R12s = 1.0f; } /* TIR at first interface */
    float T121p = 1.0f - R12p, T121s = 1.0f - R12s;

    float R23p, R23s;
    fresnel_substrate_pol(cosThetaT, eta3 / eta2, &R23p, &R23s);

    float cosB = cosf(atanf(eta2 / eta1));
    float phi21p = (cosTheta < cosB) ? 0.0f : MTLX_PI, phi21s = MTLX_PI;
    float phi23p, phi23s;
    fresnel_substrate_phase(cosThetaT, eta2, eta3, &phi23p, &phi23s);

    float r123p = maxf(sqrtf(R12p * R23p), 0.0f);
    float r123s = maxf(sqrtf(R12s * R23s), 0.0f);
    float opd = 2.0f * eta2 * cosThetaT * (tf_thickness_nm * 1.0e-9f);

    v3 I = v3_splat(0.0f);
    /* parallel polarization */
    float Rsp = (T121p * T121p * R23p) / (1.0f - R12p * R23p);
    I = v3_add(I, v3_splat(R12p + Rsp));
    float Cm = Rsp - T121p;
    for (int m = 1; m <= 2; m++) {
        Cm *= r123p;
        I = v3_add(I, v3_scale(eval_sensitivity((float)m * opd, (float)m * (phi23p + phi21p)), 2.0f * Cm));
    }
    /* perpendicular polarization */
    float Rss = (T121s * T121s * R23s) / (1.0f - R12s * R23s);
    I = v3_add(I, v3_splat(R12s + Rss));
    Cm = Rss - T121s;
    for (int m = 1; m <= 2; m++) {
        Cm *= r123s;
        I = v3_add(I, v3_scale(eval_sensitivity((float)m * opd, (float)m * (phi23s + phi21s)), 2.0f * Cm));
    }
    I = v3_scale(I, 0.5f);
    v3 rgb = xyz_to_rgb_airy(I);
    return v3_make(clampf(rgb.x, 0.0f, 1.0f), clampf(rgb.y, 0.0f, 1.0f), clampf(rgb.z, 0.0f, 1.0f));
}

/* ---- layered material ------------------------------------------------- */
typedef struct {
    v3    diff_color;  /* effective diffuse albedo (incl. subsurface tint) */
    float subsurface_w; /* normalized-diffusion strength */
    v3    F0;          /* specular reflectance at normal incidence */
    float F90;         /* specular-weighted grazing reflectance */
    float alpha;       /* GGX roughness^2 */
    v3    sheen_color; float sheen_alpha, sheen_w;
    v3    coat_color; float coat_alpha, coat_w, coat_f0, coat_Ri;
    float tf_weight, tf_thickness, tf_ior; /* thin-film iridescence */
    float glass_w, ior;
    /* normalized lobe selection probabilities (glass is delta) */
    float pd, ps, pc, pg;
} Layers;

static Layers extract(const OpenPBRParams *p) {
    Layers L;
    float metal = p->metalness, trans = p->transmission;
    v3 base = v3_scale(p->base_color, p->base_weight);

    v3 diff = v3_scale(base, (1.0f - metal) * (1.0f - trans));
    if (p->subsurface > 0.0f)
        diff = v3_lerp(diff, v3_scale(p->subsurface_color, (1.0f - metal) * (1.0f - trans)), p->subsurface);
    L.diff_color = diff;
    {
        float mean_radius = (p->subsurface_radius.x + p->subsurface_radius.y +
                             p->subsurface_radius.z) * (1.0f / 3.0f);
        float radius_response = 1.0f - expf(-maxf(0.0f, mean_radius *
                                                   p->subsurface_scale));
        L.subsurface_w = clampf(p->subsurface * radius_response, 0.0f, 1.0f);
    }

    float f0d = (p->specular_ior - 1.0f) / (p->specular_ior + 1.0f);
    f0d = f0d * f0d * p->specular_weight;
    L.F0 = v3_lerp(v3_mul(v3_splat(f0d), p->specular_color), base, metal);
    L.F90 = clampf(p->specular_weight, 0.0f, 1.0f);
    L.ior = p->specular_ior;
    L.alpha = clampf(p->specular_roughness, 0.02f, 1.0f); L.alpha *= L.alpha;

    L.sheen_w = p->sheen_weight;
    L.sheen_color = p->sheen_color;
    L.sheen_alpha = clampf(p->sheen_roughness, 0.05f, 1.0f);

    L.coat_w = clampf(p->coat_weight, 0.0f, 1.0f);
    L.coat_color = p->coat_color;
    L.coat_alpha = clampf(p->coat_roughness, 0.02f, 1.0f); L.coat_alpha *= L.coat_alpha;
    float c = (p->coat_ior - 1.0f) / (p->coat_ior + 1.0f);
    L.coat_f0 = c * c;
    /* Internal diffuse Fresnel reflectance of the coat (the fraction of base-
     * reflected light the coat-air interface bounces back down). Drives coat
     * darkening: light trapped between coat and base re-illuminates it, which
     * saturates the apparent base color (a clear coat makes a tinted base look
     * deeper, not washed out). Egan/Hilgeman polynomial in the coat IOR. */
    { float n = p->coat_ior;
      L.coat_Ri = (n > 1.001f) ? (-1.440f / (n * n) + 0.710f / n + 0.668f + 0.0636f * n) : 0.0f; }

    L.tf_weight = clampf(p->thin_film_weight, 0.0f, 1.0f);
    L.tf_thickness = p->thin_film_thickness;
    L.tf_ior = p->thin_film_ior;

    L.glass_w = (1.0f - metal) * trans;

    float lumD = luminance(L.diff_color) + 0.5f * L.sheen_w * luminance(L.sheen_color);
    float lumS = luminance(L.F0) + 0.04f;
    float wc = L.coat_w * 0.5f;
    float tot = lumD + lumS + wc + L.glass_w + 1e-4f;
    L.pd = lumD / tot; L.ps = lumS / tot; L.pc = wc / tot; L.pg = L.glass_w / tot;
    return L;
}

static v3 face_forward(v3 N, v3 wo) { return v3_dot(N, wo) < 0.0f ? v3_neg(N) : N; }

/* Unified reflection-lobe evaluation (diffuse + spec + sheen + coat). Fills the
 * mixture pdf for the sampled wi. Returns f (rgb). Excludes the glass lobe. */
/* Evaluate the reflection lobes. `out_diff` / `out_spec` (either may be NULL)
 * receive the diffuse and the specular halves separately: a split-sum IBL has to
 * weigh each against a DIFFERENT prefiltered environment (irradiance vs the
 * reflection lobe), so handing it the sum makes it count both lobes twice. Sheen
 * is grouped with the specular half: it is a directional grazing lobe, not part
 * of the cosine-distributed diffuse response. */
static v3 eval_reflection_split(const Layers *L, v3 N, v3 wo, v3 wi, float *pdf,
                                v3 *out_diff, v3 *out_spec) {
    float NdotL = v3_dot(N, wi), NdotV = v3_dot(N, wo);
    if (NdotL <= 0.0f || NdotV <= 0.0f) {
        *pdf = 0.0f;
        if (out_diff) *out_diff = v3_splat(0.0f);
        if (out_spec) *out_spec = v3_splat(0.0f);
        return v3_splat(0.0f);
    }
    v3 H = v3_normalize(v3_add(wo, wi));
    float NdotH = v3_dot(N, H), VdotH = v3_dot(wo, H);

    /* The coat attenuates the layers beneath it by its Fresnel reflectance and
     * tints their throughput by coat_color (a clear-coat acts as a colored
     * filter on everything below; the tint scales with the coat weight). This
     * colored attenuation is what gives e.g. standard_surface copper its orange
     * cast -- the copper color lives in coat_color, not base_color. */
    float coat_fr = L->coat_w * fresnel_schlick_s(NdotV, L->coat_f0);
    v3 base_atten = v3_scale(v3_lerp(v3_splat(1.0f), L->coat_color, L->coat_w),
                             1.0f - coat_fr);

    /* Coat darkening: the multiple-scattering geometric series for light bounced
     * between the base (albedo rho) and the coat's internal interface boosts the
     * effective albedo to rho/(1 - rho*Ri), which raises saturation for tinted
     * bases (more for high-albedo channels). 1 - rho*Ri*coat_w stays positive
     * since rho <= 1 and Ri*coat_w < 1. */
    v3 diff_alb = L->diff_color;
    if (L->coat_w > 0.0f && L->coat_Ri > 0.0f) {
        float k = L->coat_Ri * L->coat_w;
        diff_alb = v3_make(diff_alb.x / (1.0f - diff_alb.x * k),
                           diff_alb.y / (1.0f - diff_alb.y * k),
                           diff_alb.z / (1.0f - diff_alb.z * k));
    }
    /* Disney/Burley normalized-diffusion approximation. Unlike a plain tint,
     * this broadens the diffuse response at grazing incidence and uses the
     * authored scattering radius/scale to control how strongly the material
     * behaves as a subsurface medium. The 1.25 normalization keeps the lobe's
     * integrated energy close to Lambert. */
    float diffuse_shape = 1.0f;
    if (L->subsurface_w > 0.0f) {
        float L5 = powf(1.0f - NdotL, 5.0f);
        float V5 = powf(1.0f - NdotV, 5.0f);
        float fss90 = L->alpha * NdotL * NdotL;
        float fss = (1.0f + (fss90 - 1.0f) * L5) *
                    (1.0f + (fss90 - 1.0f) * V5);
        float ss = 1.25f * (fss * (1.0f / (NdotL + NdotV + 1.0e-4f) - 0.5f) +
                            0.5f);
        diffuse_shape = (1.0f - L->subsurface_w) +
                        L->subsurface_w * maxf(0.0f, ss);
    }
    /* Preserve the energy left for the diffuse/subsurface response after the
     * dielectric interface reflects part of the incident and outgoing flux.
     * Use the same Schlick curve and authored specular weight as the specular
     * lobe; this also keeps a disabled specular lobe from spuriously reaching
     * unit reflectance at grazing angles. */
    const v3 one = v3_splat(1.0f);
    const v3 diffuse_fresnel_view = fresnel_schlick(NdotV, L->F0, L->F90);
    const v3 diffuse_fresnel_light = fresnel_schlick(NdotL, L->F0, L->F90);
    const v3 diffuse_fresnel_attenuation = v3_mul(
        v3_sub(one, diffuse_fresnel_view),
        v3_sub(one, diffuse_fresnel_light));
    v3 diff = v3_mul(diff_alb,
                     v3_scale(v3_mul(base_atten, diffuse_fresnel_attenuation),
                              MTLX_INV_PI * diffuse_shape));

    float D = ggx_D(NdotH, L->alpha), G = ggx_G(NdotV, NdotL, L->alpha);
    /* Thin film replaces the plain Fresnel with the iridescent airy reflectance
     * (blended by the film weight); the substrate index is the specular IOR. */
    v3 F = fresnel_schlick(VdotH, L->F0, L->F90);
    if (L->tf_weight > 0.0f && L->tf_thickness > 0.0f)
        F = v3_lerp(F, fresnel_airy(VdotH, L->ior, L->tf_thickness, L->tf_ior), L->tf_weight);
    v3 spec = v3_mul(v3_scale(F, D * G / (4.0f * NdotV * NdotL)), base_atten);

    v3 sheen = v3_splat(0.0f);
    if (L->sheen_w > 0.0f)
        sheen = v3_mul(v3_scale(L->sheen_color, L->sheen_w * charlie_D(NdotH, L->sheen_alpha) * sheen_V(NdotV, NdotL)), base_atten);

    v3 coat = v3_splat(0.0f);
    if (L->coat_w > 0.0f) {
        float Dc = ggx_D(NdotH, L->coat_alpha), Gc = ggx_G(NdotV, NdotL, L->coat_alpha);
        float Fc = L->coat_w * fresnel_schlick_s(VdotH, L->coat_f0);
        coat = v3_splat(Fc * Dc * Gc / (4.0f * NdotV * NdotL));
    }

    float pdf_diff = NdotL * MTLX_INV_PI;
    float pdf_spec = ggx_D(NdotH, L->alpha) * NdotH / (4.0f * VdotH + 1e-6f);
    float pdf_coat = ggx_D(NdotH, L->coat_alpha) * NdotH / (4.0f * VdotH + 1e-6f);
    *pdf = L->pd * pdf_diff + L->ps * pdf_spec + L->pc * pdf_coat;

    if (out_diff) *out_diff = diff;
    if (out_spec) *out_spec = v3_add(spec, v3_add(sheen, coat));
    return v3_add(v3_add(diff, spec), v3_add(sheen, coat));
}

static v3 eval_reflection(const Layers *L, v3 N, v3 wo, v3 wi, float *pdf) {
    return eval_reflection_split(L, N, wo, wi, pdf, (v3 *)0, (v3 *)0);
}

int bsdf_sample(const OpenPBRParams *p, v3 Ns, v3 wo, pcg32 *rng, BsdfSample *out) {
    v3 Nn = v3_normalize(Ns);
    /* Sign of the raw (outward) normal vs the view direction tells us which side
     * of the dielectric interface we are on: entering air->glass (wo on the
     * outward side) or exiting glass->air (wo on the inward side). This selects
     * the relative IOR for refraction/Fresnel below. */
    int entering = v3_dot(Nn, wo) > 0.0f;
    v3 N = face_forward(Nn, wo);
    Layers L = extract(p);
    v3 T, B; onb(N, &T, &B);
    float u = pcg32_f(rng), u1 = pcg32_f(rng), u2 = pcg32_f(rng);

    /* glass (dielectric; microfacet-roughened when alpha is significant) */
    if (u < L.pg) {
        /* sample a microfacet normal m (= N for smooth glass) and reflect/
         * refract about it, giving frosted glass for rough surfaces. */
        v3 m = (L.alpha > 1e-3f) ? v3_normalize(to_world(sample_ggx_h(L.alpha, u1, u2), T, B, N)) : N;
        if (v3_dot(m, wo) < 0.0f) m = v3_neg(m);
        /* relative IOR n_i/n_t: 1/ior entering air->glass, ior exiting glass->air
         * (the latter enables total internal reflection past the critical angle,
         * which the hardcoded 1/ior path silently dropped -- turning the lens
         * into a washed-out blur). */
        float eta = entering ? (1.0f / L.ior) : L.ior;
        float cosi = clampf(v3_dot(m, wo), 0.0f, 1.0f);
        float Fr = fresnel_dielectric(cosi, eta);
        v3 wi; int refracted;
        if (pcg32_f(rng) < Fr) {
            wi = v3_sub(v3_scale(m, 2.0f * v3_dot(m, wo)), wo);
            if (v3_dot(wi, N) <= 0.0f) return 0; /* reflected below surface: reject */
            refracted = 0;
        } else {
            float sint2 = eta * eta * (1.0f - cosi * cosi);
            float cost = sqrtf(maxf(0.0f, 1.0f - sint2));
            wi = v3_normalize(v3_sub(v3_scale(m, eta * cosi - cost), v3_scale(wo, eta)));
            refracted = 1;
        }
        /* When transmission_depth > 0 the tint lives in the volume (applied as
         * Beer-Lambert absorption by the path tracer over the internal path
         * length), so the interface itself is clear; otherwise transmission_color
         * is a thin per-interface tint. Reflections carry the specular color. */
        out->throughput = !refracted ? p->specular_color
                        : (p->transmission_depth > 0.0f ? v3_splat(1.0f)
                                                        : p->transmission_color);
        out->wi = wi; out->pdf = 1.0f;
        out->specular = 1; out->transmission = 1; out->crossed = refracted;
        out->subsurface = 0;
        return 1;
    }

    /* choose a reflection lobe to generate a direction */
    v3 wi;
    if (u < L.pg + L.pc) {              /* coat GGX */
        v3 H = v3_normalize(to_world(sample_ggx_h(L.coat_alpha, u1, u2), T, B, N));
        wi = v3_sub(v3_scale(H, 2.0f * v3_dot(wo, H)), wo);
    } else if (u < L.pg + L.pc + L.ps) { /* base spec GGX */
        v3 H = v3_normalize(to_world(sample_ggx_h(L.alpha, u1, u2), T, B, N));
        wi = v3_sub(v3_scale(H, 2.0f * v3_dot(wo, H)), wo);
    } else {                             /* diffuse / sheen (cosine) */
        wi = to_world(sample_cosine_hemisphere(u1, u2), T, B, N);
    }
    if (v3_dot(N, wi) <= 0.0f) return 0;

    float pdf;
    v3 f = eval_reflection(&L, N, wo, wi, &pdf);
    if (pdf <= 0.0f) return 0;
    out->wi = wi;
    out->throughput = v3_scale(f, v3_dot(N, wi) / pdf);
    out->pdf = pdf;
    out->specular = 0; out->transmission = 0; out->crossed = 0;
    out->subsurface = (p->subsurface > 0.5f) ? 1 : 0;
    return 1;
}

VolumeMedium transmission_medium(const OpenPBRParams *p) {
    VolumeMedium m;
    m.sigma_t = v3_splat(0.0f); m.sigma_s = v3_splat(0.0f); m.g = 0.0f;
    if (p->transmission_depth <= 0.0f) return m;
    float inv = 1.0f / p->transmission_depth;
    /* extinction = -ln(clamp(transmission_color))/depth (clamp the color away
     * from 0 = infinite extinction and 1 = none for stability). */
    v3 c = p->transmission_color;
    m.sigma_t = v3_make(-logf(clampf(c.x, 1e-3f, 1.0f)) * inv,
                        -logf(clampf(c.y, 1e-3f, 1.0f)) * inv,
                        -logf(clampf(c.z, 1e-3f, 1.0f)) * inv);
    /* scattering = transmission_scatter/depth, clamped to <= extinction (so the
     * single-scattering albedo sigma_s/sigma_t stays in [0,1]). */
    v3 ss = v3_scale(p->transmission_scatter, inv);
    m.sigma_s = v3_make(minf(ss.x, m.sigma_t.x), minf(ss.y, m.sigma_t.y), minf(ss.z, m.sigma_t.z));
    m.g = clampf(p->transmission_scatter_anisotropy, -0.95f, 0.95f);
    return m;
}

v3 bsdf_eval(const OpenPBRParams *p, v3 Ns, v3 wo, v3 wi, float *pdf_out) {
    v3 N = face_forward(v3_normalize(Ns), wo);
    Layers L = extract(p);
    return eval_reflection(&L, N, wo, wi, pdf_out);
}

v3 bsdf_eval_lobes(const OpenPBRParams *p, v3 Ns, v3 wo, v3 wi, v3 *out_diffuse,
                   v3 *out_specular, float *pdf_out) {
    v3 N = face_forward(v3_normalize(Ns), wo);
    Layers L = extract(p);
    return eval_reflection_split(&L, N, wo, wi, pdf_out, out_diffuse, out_specular);
}

BsdfLobeWeights bsdf_lobe_weights(const OpenPBRParams *p) {
    const Layers L = extract(p);
    BsdfLobeWeights weights = {L.pd, L.ps, L.pc, L.pg};
    return weights;
}
