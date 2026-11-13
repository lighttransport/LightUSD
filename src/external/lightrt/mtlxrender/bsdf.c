#include "bsdf.h"

#include <math.h>
#include <stddef.h>

/* MaterialX genglsl Zeltner LTC fit. The LTC density contains NdotL;
 * divide that factor out here because bsdf_eval returns a BRDF. */
float bsdf_zeltner_sheen_albedo(float ndotv, float roughness) {
    float x = clampf(ndotv, 0.0f, 1.0f);
    float y = clampf(roughness, 0.01f, 1.0f);
    float s = y * (0.0206607f + 1.58491f*y) / (0.0379424f + y*(1.32227f + y));
    float m = y * (-0.193854f + y*(-1.14885f + y*(1.7932f - 0.95943f*y*y))) / (0.046391f + y);
    float o = y * (0.000654023f + (-0.0207818f + 0.119681f*y)*y) / (1.26264f + y*(-1.92021f + y));
    float q = (x - m) / s;
    return expf(-0.5f*q*q) / (s*sqrtf(2.0f*MTLX_PI)) + o;
}

static float zeltner_sheen_brdf(v3 wo, v3 wi, float nv, float nl, float roughness) {
    float y = clampf(roughness, 0.01f, 1.0f);
    float a = (2.58126f*nv + 0.813703f*y)*y / (1.0f + 0.310327f*nv*nv + 2.60994f*nv*y);
    float b = sqrtf(maxf(0.0f, 1.0f - nv))*(y - 1.0f)*y*y*y /
              (0.0000254053f + 1.71228f*nv - 1.71506f*nv*y + 1.34174f*y*y);
    float tangent_len = sqrtf(maxf(0.0f, 1.0f - nv*nv));
    float wx = tangent_len > 1.0e-6f ? (v3_dot(wi, wo) - nv*nl) / tangent_len : 0.0f;
    float wy2 = maxf(0.0f, 1.0f - nl*nl - wx*wx);
    float tx = a*wx + b*nl;
    float len2 = tx*tx + a*a*wy2 + nl*nl;
    return bsdf_zeltner_sheen_albedo(nv, y) * MTLX_INV_PI * (a/len2)*(a/len2);
}

/* MaterialX genglsl conductor Fresnel. Double intermediates avoid loss of
 * significance for smooth lobes and preserve finite values at normal incidence. */
static double materialx_conductor_fresnel(double cosine, double n, double k) {
    double c = fmin(1.0, fmax(0.0, cosine));
    double c2 = c*c, sin2 = 1.0-c2, n2 = n*n, k2 = k*k;
    double t0 = n2-k2-sin2;
    double ab = sqrt(t0*t0+4.0*n2*k2);
    double aa = sqrt(fmax(0.5*(ab+t0),0.0));
    double t1 = ab+c2, t2 = 2.0*aa*c;
    double t3 = c2*ab+sin2*sin2, t4 = t2*sin2;
    if (t1+t2 <= 0.0 || t3+t4 <= 0.0) return 1.0;
    double rs = (t1-t2)/(t1+t2);
    double rp = rs*(t3-t4)/(t3+t4);
    return 0.5*(rp+rs);
}

static double materialx_ggx_directional_albedo(double x, double y) {
    double x2=x*x, y2=y*y;
    double r0=.1003-.6303*x+9.748*y-2.038*x*y+29.34*x2-8.245*y2
              -26.44*x2*y+19.99*x*y2-5.448*x2*y2;
    double r1=.9345-2.323*x+2.229*y-3.748*x*y+1.424*x2-.7684*y2
              +1.436*x2*y+.2913*x*y2+.6286*x2*y2;
    double r2=1.0-1.765*x+8.263*y+11.53*x*y+28.96*x2-7.507*y2
              -36.11*x2*y+15.86*x*y2+33.37*x2*y2;
    double r3=1.0+.2281*x+15.94*y-55.83*x*y+13.08*x2+41.26*y2
              +54.9*x2*y+300.2*x*y2-285.1*x2*y2;
    return fmin(1.0,fmax(0.0,r0/r2))+fmin(1.0,fmax(0.0,r1/r3));
}

static v3 conductor_airy(float cosine, v3 eta, v3 extinction, float thickness, float film_ior);

static v3 conductor_response(v3 eta, v3 extinction, float alpha_x,
    float alpha_y, float ndotv, float ndotl, float hx, float hy, float hz, float vdoth,
    float thickness, float film_ior) {
    if (!(ndotv > 0.0f && ndotl > 0.0f) || !v3_is_finite(eta) ||
        !v3_is_finite(extinction) || !isfinite(alpha_x) || !isfinite(alpha_y) ||
        !isfinite(ndotv) || !isfinite(ndotl) || !isfinite(hx) || !isfinite(hy) ||
        !isfinite(hz) || !isfinite(vdoth)) return v3_splat(0.0f);
    double v=fmin(1.0,ndotv), l=fmin(1.0,ndotl), h=fmin(1.0,fmax(0.0,hz));
    double ax=fmin(1.0,fmax(1.0e-8,(double)alpha_x));
    double ay=fmin(1.0,fmax(1.0e-8,(double)alpha_y));
    double a=sqrt(ax*ay), a2=a*a;
    double d=((double)hx/ax)*((double)hx/ax)+((double)hy/ay)*((double)hy/ay)+h*h;
    if (!(d > 0.0)) return v3_splat(0.0f);
    double D=1.0/(3.14159265358979323846*ax*ay*d*d);
    double lv=sqrt(a2+(1.0-a2)*v*v), ll=sqrt(a2+(1.0-a2)*l*l);
    double G=2.0*v*l/(v*ll+l*lv);
    double ess=materialx_ggx_directional_albedo(v,a);
    if (!(ess > 0.0)) return v3_splat(0.0f);
    double n[3]={eta.x,eta.y,eta.z}, k[3]={extinction.x,extinction.y,extinction.z};
    float result[3];
    v3 film0 = v3_splat(0.0f), film = v3_splat(0.0f);
    if (thickness > 0.0f && isfinite(thickness) && isfinite(film_ior)) {
        film0 = conductor_airy(1.0f, eta, extinction, thickness, film_ior);
        film = conductor_airy(vdoth, eta, extinction, thickness, film_ior);
    }
    const float film0c[3] = {film0.x, film0.y, film0.z};
    const float filmc[3] = {film.x, film.y, film.z};
    int use_film = thickness > 0.0f && isfinite(thickness) && isfinite(film_ior);
    for (int i=0;i<3;i++) {
        double f0=use_film ? film0c[i] : materialx_conductor_fresnel(1.0,n[i],k[i]);
        double f=use_film ? filmc[i] : materialx_conductor_fresnel(vdoth,n[i],k[i]);
        double favg=f0+(1.0-f0)/21.0;
        double compensation=1.0+favg*(1.0-ess)/ess;
        result[i]=(float)(D*G*f*compensation/(4.0*v*l));
    }
    return v3_make(result[0],result[1],result[2]);
}

v3 bsdf_materialx_conductor_anisotropic(v3 eta, v3 extinction, float alpha_x,
    float alpha_y, float ndotv, float ndotl, float hx, float hy, float hz, float vdoth) {
    return conductor_response(eta, extinction, alpha_x, alpha_y, ndotv, ndotl,
                              hx, hy, hz, vdoth, 0.0f, 1.0f);
}

v3 bsdf_materialx_conductor(v3 eta, v3 extinction, float alpha,
                           float ndotv, float ndotl, float ndoth, float vdoth) {
    if (!isfinite(ndoth)) return v3_splat(0.0f);
    float h=clampf(ndoth, 0.0f, 1.0f);
    return bsdf_materialx_conductor_anisotropic(eta, extinction, alpha, alpha,
        ndotv, ndotl, sqrtf(fmaxf(0.0f,1.0f-h*h)), 0.0f, h, vdoth);
}

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
    float h2 = clampf(NdotH * NdotH, 0.0f, 1.0f);
    /* Avoid cancellation at NdotH=1 for small alpha. */
    float d = (1.0f - h2) + a2 * h2;
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

/* Energy-compensated Oren-Nayar from the MaterialX PBRSpec. The algebraic
 * form of G avoids cancellation and division by zero as cosTheta approaches 0. */
static double eon_directional(double mu, double sigma, double a) {
    const double pi = 3.14159265358979323846;
    double c = fmin(1.0, fmax(0.0, mu));
    double sn = sqrt(fmax(0.0, 1.0 - c * c));
    double ratio = sn * c * (1.0 + sn + sn * sn) / (1.0 + sn);
    double g = sn * (acos(c) - sn * c) + (2.0 / 3.0) * (ratio - sn);
    return a * (1.0 + sigma * g / pi);
}

static v3 eon_diffuse(v3 rho, float roughness, float nv, float nl, float lv) {
    const double pi = 3.14159265358979323846;
    double sigma = fmin(1.0, fmax(0.0, (double)roughness));
    if (sigma == 0.0) return v3_scale(rho, MTLX_INV_PI);
    double c1 = 0.5 - 2.0 / (3.0 * pi);
    double c2 = 2.0 / 3.0 - 28.0 / (15.0 * pi);
    double a = 1.0 / (1.0 + c1 * sigma);
    double avg = a * (1.0 + c2 * sigma);
    double ev = eon_directional(nv, sigma, a);
    double el = eon_directional(nl, sigma, a);
    double s = (double)lv - (double)nv * nl;
    double st = s > 0.0 ? s / fmax((double)nv, (double)nl) : s;
    double single = a * (1.0 + sigma * st);
    double multi = fmax(1.0e-8, 1.0 - ev) * fmax(1.0e-8, 1.0 - el) /
                   fmax(1.0e-8, 1.0 - avg);
    double r = rho.x, g = rho.y, b = rho.z;
    return v3_make((float)((r * single + r * r * avg / (1.0 - r * fmax(0.0, 1.0 - avg)) * multi) / pi),
                   (float)((g * single + g * g * avg / (1.0 - g * fmax(0.0, 1.0 - avg)) * multi) / pi),
                   (float)((b * single + b * b * avg / (1.0 - b * fmax(0.0, 1.0 - avg)) * multi) / pi));
}

/* Reference qualitative Oren-Nayar and Burley diffuse factors. */
static float rough_diffuse_factor(int model, float roughness, float nv,
                                  float nl, float lv, float lh) {
    float r = clampf(roughness, 0.0f, 1.0f);
    if (model == MTLX_DIFFUSE_OREN_NAYAR) {
        float s = lv - nl * nv;
        float st = s > 0.0f ? s / maxf(nl, nv) : 0.0f;
        float r2 = r * r;
        float a = 1.0f - 0.5f * r2 / (r2 + 0.33f);
        float b = 0.45f * r2 / (r2 + 0.09f);
        return a + b * st;
    }
    if (model == MTLX_DIFFUSE_BURLEY) {
        float f90 = 0.5f + 2.0f * r * lh * lh;
        float ml = 1.0f - nl, mv = 1.0f - nv;
        float ml2 = ml * ml, mv2 = mv * mv;
        return (1.0f + (f90 - 1.0f) * ml2 * ml2 * ml) *
               (1.0f + (f90 - 1.0f) * mv2 * mv2 * mv);
    }
    return 1.0f;
}

/* "Charlie" sheen NDF (Estevez & Kulla 2017) + Neubelt visibility. */
static float charlie_D(float NdotH, float r) {
    r = clampf(r, 0.005f, 1.0f);
    float inv = 1.0f / r;
    float sin2 = maxf(0.0f, 1.0f - NdotH * NdotH);
    return (2.0f + inv) * powf(sin2, inv * 0.5f) * (1.0f / (2.0f * MTLX_PI));
}
static float sheen_V(float NdotV, float NdotL) {
    return 1.0f / (4.0f * (NdotL + NdotV - NdotL * NdotV));
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

/* MaterialX conductor Airy model: each XYZ channel carries the phase and
 * reflectance of the corresponding complex substrate index before conversion
 * to RGB, matching the reference's spectral sensitivity approximation. */
static v3 conductor_airy(float cosine, v3 eta, v3 extinction, float thickness, float film_ior) {
    float c = clampf(cosine, 1.0e-8f, 1.0f);
    float e2 = maxf(film_ior, 1.0f);
    float ct = sqrtf(maxf(0.0f, 1.0f - (1.0f-c*c)/(e2*e2)));
    float r12p, r12s;
    fresnel_dielectric_pol(c, e2, &r12p, &r12s);
    if (ct <= 0.0f) { r12p = 1.0f; r12s = 1.0f; }
    float phi1p = c < cosf(atanf(e2)) ? 0.0f : MTLX_PI;
    float opd = 2.0f * e2 * ct * thickness * 1.0e-9f;
    const float ns[3] = {eta.x,eta.y,eta.z}, ks[3] = {extinction.x,extinction.y,extinction.z};
    float xyz[3];
    for (int j = 0; j < 3; ++j) {
        double n = ns[j], k = ks[j], reln = n/e2, relk = k/e2;
        double sin2 = 1.0-(double)ct*ct, c2 = (double)ct*ct;
        double t0 = reln*reln-relk*relk-sin2;
        double ab = sqrt(t0*t0+4.0*reln*reln*relk*relk);
        double a = sqrt(fmax(0.0,(ab+t0)*0.5));
        double t1 = ab+c2, t2 = 2.0*a*ct;
        double rs = t1+t2 > 0.0 ? (t1-t2)/(t1+t2) : 1.0;
        double t3 = c2*ab+sin2*sin2, t4 = t2*sin2;
        double rp = t3+t4 > 0.0 ? rs*(t3-t4)/(t3+t4) : 1.0;
        /* Algebraic form avoids division by eta when eta is zero. */
        double A = n*n-k*k-e2*e2*sin2;
        double B = sqrt(A*A+4.0*n*n*k*k);
        double U = sqrt(fmax(0.0,(A+B)*0.5));
        double V = sqrt(fmax(0.0,(B-A)*0.5));
        double phiS = atan2(2.0*e2*V*ct, U*U+V*V-e2*e2*c2);
        double phiP = atan2(2.0*e2*ct*(2.0*n*k*U-(n*n-k*k)*V),
                            (n*n+k*k)*(n*n+k*k)*c2-e2*e2*(U*U+V*V));
        const double r12[2] = {r12p,r12s}, r23[2] = {rp,rs};
        const double phase[2] = {phiP+phi1p,phiS+MTLX_PI};
        double sum = 0.0;
        for (int pol=0; pol<2; ++pol) {
            double trans=1.0-r12[pol], den=1.0-r12[pol]*r23[pol];
            double dc=den > 1e-15 ? trans*trans*r23[pol]/den : 0.0;
            sum += r12[pol]+dc;
            double cm=dc-trans, r=sqrt(fmax(0.0,r12[pol]*r23[pol]));
            for (int m=1; m<=2; ++m) {
                cm *= r;
                v3 sensitivity=eval_sensitivity((float)m*opd,(float)(m*phase[pol]));
                float component = j==0 ? sensitivity.x : (j==1 ? sensitivity.y : sensitivity.z);
                sum += 2.0*cm*component;
            }
        }
        xyz[j]=(float)(0.5*sum);
    }
    v3 rgb=xyz_to_rgb_airy(v3_make(xyz[0],xyz[1],xyz[2]));
    return v3_make(clampf(rgb.x,0.0f,1.0f),clampf(rgb.y,0.0f,1.0f),clampf(rgb.z,0.0f,1.0f));
}

v3 bsdf_materialx_conductor_film_fresnel(float cosine, v3 eta, v3 extinction,
                                           float thickness, float film_ior) {
    if (!isfinite(cosine) || !v3_is_finite(eta) || !v3_is_finite(extinction) ||
        !isfinite(thickness) || !isfinite(film_ior) || thickness <= 0.0f)
        return v3_splat(0.0f);
    return conductor_airy(cosine, eta, extinction, thickness, film_ior);
}

/* ---- layered material ------------------------------------------------- */
typedef struct {
    v3    diff_color;  /* effective diffuse albedo (incl. subsurface tint) */
    v3    diffuse_base_color, diffuse_subsurface_color;
    float diffuse_base_scale, diffuse_subsurface_scale;
    float diffuse_roughness;
    int   diffuse_model;
    int   diffuse_lobe_count;
    const MtlxDiffuseLobe *diffuse_lobes;
    float diffuse_mix_scale;
    float subsurface_w; /* normalized-diffusion strength */
    v3    F0;          /* specular reflectance at normal incidence */
    float F90;         /* specular-weighted grazing reflectance */
    float alpha;       /* GGX roughness^2 */
    v3    sheen_color; float sheen_alpha, sheen_w;
    int   sheen_lobe_count;
    const MtlxSheenLobe *sheen_lobes;
    int conductor_lobe_count;
    const MtlxConductorLobe *conductor_lobes;
    v3    coat_color; float coat_alpha, coat_w, coat_f0, coat_Ri;
    float tf_weight, tf_thickness, tf_ior; /* thin-film iridescence */
    float glass_w, ior;
    /* normalized lobe selection probabilities (glass is delta) */
    float pd, ps, pc, pg, pm;
    float conductor_weight_sum;
} Layers;

static Layers extract(const OpenPBRParams *p) {
    Layers L;
    const int explicit_conductor = p->conductor_lobe_count > 0;
    float metal = explicit_conductor ? 0.0f : p->metalness, trans = p->transmission;
    v3 base = v3_scale(p->base_color, p->base_weight);
    if (explicit_conductor) {
        base = v3_splat(0.0f);
        for (int i = 0; i < p->diffuse_lobe_count && i < MTLX_MAX_DIFFUSE_LOBES; ++i) {
            const MtlxDiffuseLobe *d = &p->diffuse_lobes[i];
            base = v3_add(base, v3_scale(v3_mul(d->color, d->weight_color), d->weight));
        }
    }

    v3 diff = v3_scale(base, (1.0f - metal) * (1.0f - trans));
    if (p->subsurface > 0.0f)
        diff = v3_lerp(diff, v3_scale(p->subsurface_color, (1.0f - metal) * (1.0f - trans)), p->subsurface);
    L.diff_color = diff;
    L.diffuse_base_color = p->base_color;
    L.diffuse_subsurface_color = p->subsurface_color;
    L.diffuse_base_scale = (explicit_conductor ? 0.0f : p->base_weight) * (1.0f - metal) * (1.0f - trans) * (1.0f - p->subsurface);
    L.diffuse_subsurface_scale = (1.0f - metal) * (1.0f - trans) * p->subsurface;
    L.diffuse_roughness = p->diffuse_roughness;
    L.diffuse_model = p->diffuse_model;
    L.diffuse_lobe_count = p->diffuse_lobe_count < 0 ? 0 :
        (p->diffuse_lobe_count > MTLX_MAX_DIFFUSE_LOBES ? MTLX_MAX_DIFFUSE_LOBES : p->diffuse_lobe_count);
    L.diffuse_lobes = p->diffuse_lobes;
    L.diffuse_mix_scale = (1.0f - metal) * (1.0f - trans) * (1.0f - p->subsurface);
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
    float specular_roughness = isfinite(p->specular_roughness) ? p->specular_roughness : 1.0f;
    L.alpha = clampf(specular_roughness, 0.02f, 1.0f); L.alpha *= L.alpha;

    L.sheen_w = p->sheen_weight;
    L.sheen_color = p->sheen_color;
    L.sheen_alpha = clampf(p->sheen_roughness, 0.005f, 1.0f);
    L.sheen_lobe_count = p->sheen_lobe_count < 0 ? 0 :
        (p->sheen_lobe_count > MTLX_MAX_SHEEN_LOBES ? MTLX_MAX_SHEEN_LOBES : p->sheen_lobe_count);
    L.sheen_lobes = p->sheen_lobes;
    L.conductor_lobe_count = p->conductor_lobe_count < 0 ? 0 :
        (p->conductor_lobe_count > MTLX_MAX_CONDUCTOR_LOBES ? MTLX_MAX_CONDUCTOR_LOBES : p->conductor_lobe_count);
    L.conductor_lobes = p->conductor_lobes;

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

    float lumD = fmaxf(0.0f, luminance(L.diff_color) + 0.5f * L.sheen_w * luminance(L.sheen_color));
    float lumS = fmaxf(0.0f, luminance(L.F0)) + 0.04f;
    L.conductor_weight_sum = 0.0f;
    for (int i = 0; i < L.conductor_lobe_count; ++i)
        L.conductor_weight_sum += fmaxf(0.0f, L.conductor_lobes[i].weight) *
            fmaxf(0.0f, luminance(L.conductor_lobes[i].weight_color));
    float wc = L.coat_w * 0.5f;
    float tot = lumD + lumS + wc + L.glass_w + L.conductor_weight_sum;
    L.pd = lumD / tot; L.ps = lumS / tot; L.pc = wc / tot; L.pg = L.glass_w / tot;
    L.pm = L.conductor_weight_sum / tot;
    return L;
}

static v3 face_forward(v3 N, v3 wo) { return v3_dot(N, wo) < 0.0f ? v3_neg(N) : N; }

/* Unauthored or invalid normals use the surface shading normal. MaterialX
 * orients each closure's normal towards the view. */
static v3 conductor_normal(const MtlxConductorLobe *lobe, v3 fallback, v3 wo) {
    v3 n = lobe->normal;
    float length = v3_len(n);
    if (!v3_is_finite(n) || !isfinite(length) || length < 1.0e-6f) n = fallback;
    else n = v3_scale(n, 1.0f / length);
    return face_forward(n, wo);
}

/* Keep evaluation, proposal and PDF in the same tangent frame. */
static void conductor_frame(v3 N, v3 input, v3 *tangent, v3 *bitangent) {
    *tangent = v3_sub(input, v3_scale(N, v3_dot(input, N)));
    float length = v3_len(*tangent);
    if (!v3_is_finite(*tangent) || !isfinite(length) || length < 1.0e-6f)
        onb(N, tangent, bitangent);
    else {
        *tangent = v3_normalize(*tangent);
        *bitangent = v3_cross(N, *tangent);
    }
}

static float conductor_sampling_weight(const MtlxConductorLobe *lobe) {
    return fmaxf(0.0f, lobe->weight) * fmaxf(0.0f, luminance(lobe->weight_color));
}

static float conductor_proposal_alpha(float alpha) {
    return isfinite(alpha) ? clampf(alpha, 1.0e-4f, 1.0f) : 1.0f;
}

/* The 1e-4 proposal floor avoids float underflow for near-delta lobes. The
 * BRDF itself retains MaterialX's 1e-8 floor; this proposal remains positive. */
static float conductor_reflection_pdf(const MtlxConductorLobe *lobe, v3 N,
                                      v3 wo, v3 H, float ndoth, float vdoth) {
    float ndotv = v3_dot(N, wo);
    if (!(ndoth > 0.0f && vdoth > 0.0f && ndotv > 0.0f)) return 0.0f;
    v3 t, b; conductor_frame(N, lobe->tangent, &t, &b);
    double ax=conductor_proposal_alpha(lobe->roughness);
    double ay=conductor_proposal_alpha(lobe->roughness_y);
    double x=v3_dot(H,t)/ax, y=v3_dot(H,b)/ay, z=ndoth;
    double d=x*x+y*y+z*z;
    double vx=ax*v3_dot(wo,t), vy=ay*v3_dot(wo,b), v=ndotv;
    double root=sqrt(v*v+vx*vx+vy*vy);
    if (!(d > 0.0) || !isfinite(d) || !isfinite(root)) return 0.0f;
    /* VNDF reflection PDF: D * G1 / (4 NdotV).  The stretched-view
     * sampling distribution requires directional anisotropic G1. */
    return (float)(1.0/(2.0*(v+root)*3.14159265358979323846*ax*ay*d*d));
}

/* Spherical-cap GGX visible-normal sampling, following the MaterialX
 * genglsl construction in the lobe's orthonormal tangent frame. */
static v3 sample_conductor_vndf(v3 view, float ax, float ay, float u1, float u2) {
    v3 stretched=v3_normalize(v3_make(ax*view.x, ay*view.y, view.z));
    float phi=MTLX_TWO_PI*u1;
    float z=(1.0f-u2)*(1.0f+stretched.z)-stretched.z;
    float radial=sqrtf(fmaxf(0.0f,1.0f-z*z));
    v3 cap=v3_make(radial*cosf(phi),radial*sinf(phi),z);
    v3 h=v3_add(cap,stretched);
    return v3_normalize(v3_make(ax*h.x,ay*h.y,fmaxf(h.z,0.0f)));
}


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
    v3 diffuse_darken = v3_splat(1.0f);
    if (L->coat_w > 0.0f && L->coat_Ri > 0.0f) {
        float k = L->coat_Ri * L->coat_w;
        diffuse_darken = v3_make(1.0f / (1.0f - diff_alb.x * k),
                                 1.0f / (1.0f - diff_alb.y * k),
                                 1.0f / (1.0f - diff_alb.z * k));
        diff_alb = v3_mul(diff_alb, diffuse_darken);
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

    if (L->diffuse_lobe_count == 0 && L->diffuse_model == MTLX_DIFFUSE_EON &&
        L->diffuse_roughness > 0.0f) {
        float lv = v3_dot(wi, wo);
        v3 rough_diffuse = v3_splat(0.0f);
        if (L->diffuse_base_scale != 0.0f) {
            v3 base_eon = eon_diffuse(v3_mul(L->diffuse_base_color, diffuse_darken),
                                       L->diffuse_roughness, NdotV, NdotL, lv);
            rough_diffuse = v3_scale(base_eon, L->diffuse_base_scale);
        }
        if (L->diffuse_subsurface_scale != 0.0f) {
            v3 sub_eon = eon_diffuse(v3_mul(L->diffuse_subsurface_color, diffuse_darken),
                                      L->diffuse_roughness, NdotV, NdotL, lv);
            rough_diffuse = v3_add(rough_diffuse, v3_scale(sub_eon, L->diffuse_subsurface_scale));
        }
        diff = v3_mul(rough_diffuse,
                      v3_scale(v3_mul(base_atten, diffuse_fresnel_attenuation), diffuse_shape));
    }

    if (L->diffuse_lobe_count == 0 &&
        (L->diffuse_model == MTLX_DIFFUSE_OREN_NAYAR ||
         L->diffuse_model == MTLX_DIFFUSE_BURLEY)) {
        float factor = rough_diffuse_factor(L->diffuse_model, L->diffuse_roughness,
                                            NdotV, NdotL, v3_dot(wi, wo), v3_dot(wi, H));
        diff = v3_scale(diff, factor);
    }

    if (L->diffuse_lobe_count > 0) {
        v3 mixed = v3_splat(0.0f);
        float lv = v3_dot(wi, wo), lh = v3_dot(wi, H);
        for (int i = 0; i < L->diffuse_lobe_count; ++i) {
            const MtlxDiffuseLobe *lobe = &L->diffuse_lobes[i];
            v3 color = v3_mul(lobe->color, diffuse_darken);
            v3 response;
            if (lobe->model == MTLX_DIFFUSE_EON)
                response = eon_diffuse(color, lobe->roughness, NdotV, NdotL, lv);
            else
                response = v3_scale(color, MTLX_INV_PI *
                    rough_diffuse_factor(lobe->model, lobe->roughness, NdotV, NdotL, lv, lh));
            mixed = v3_add(mixed, v3_scale(v3_mul(response, lobe->weight_color), lobe->weight));
        }
        v3 sub = v3_scale(v3_mul(L->diffuse_subsurface_color, diffuse_darken),
                          L->diffuse_subsurface_scale * MTLX_INV_PI);
        diff = v3_mul(v3_add(v3_scale(mixed, L->diffuse_mix_scale), sub),
                      v3_scale(v3_mul(base_atten, diffuse_fresnel_attenuation), diffuse_shape));
    }

    float D = ggx_D(NdotH, L->alpha), G = ggx_G(NdotV, NdotL, L->alpha);
    /* Thin film replaces the plain Fresnel with the iridescent airy reflectance
     * (blended by the film weight); the substrate index is the specular IOR. */
    v3 F = fresnel_schlick(VdotH, L->F0, L->F90);
    if (L->tf_weight > 0.0f && L->tf_thickness > 0.0f)
        F = v3_lerp(F, fresnel_airy(VdotH, L->ior, L->tf_thickness, L->tf_ior), L->tf_weight);
    v3 spec = v3_mul(v3_scale(F, D * G / (4.0f * NdotV * NdotL)), base_atten);

    for (int i = 0; i < L->conductor_lobe_count; ++i) {
        const MtlxConductorLobe *lobe = &L->conductor_lobes[i];
        v3 ln = conductor_normal(lobe, N, wo), tangent, bitangent;
        float lv = v3_dot(ln, wo), ll = v3_dot(ln, wi);
        if (!(lv > 0.0f && ll > 0.0f)) continue;
        conductor_frame(ln, lobe->tangent, &tangent, &bitangent);
        v3 response = conductor_response(lobe->ior, lobe->extinction,
            lobe->roughness, lobe->roughness_y, lv, ll,
            v3_dot(H,tangent), v3_dot(H,bitangent), v3_dot(H,ln), VdotH,
            lobe->thinfilm_thickness, lobe->thinfilm_ior);
        /* The integrator multiplies by the surface cosine. Correct for the
         * closure's cosine so different lobe normals compose independently. */
        spec = v3_add(spec, v3_mul(base_atten,
            v3_scale(v3_mul(response, lobe->weight_color), lobe->weight * ll / NdotL)));
    }

    v3 sheen = v3_splat(0.0f);
    if (L->sheen_lobe_count > 0) {
        for (int i = 0; i < L->sheen_lobe_count; ++i) {
            const MtlxSheenLobe *lobe = &L->sheen_lobes[i];
            v3 color = v3_mul(lobe->color, lobe->weight_color);
            float response = lobe->weight * (lobe->model == MTLX_SHEEN_ZELTNER
                ? zeltner_sheen_brdf(wo, wi, NdotV, NdotL, lobe->roughness)
                : charlie_D(NdotH, lobe->roughness) * sheen_V(NdotV, NdotL));
            sheen = v3_add(sheen, v3_scale(color, response));
        }
        sheen = v3_mul(sheen, base_atten);
    } else if (L->sheen_w > 0.0f) {
        sheen = v3_mul(v3_scale(L->sheen_color, L->sheen_w * charlie_D(NdotH, L->sheen_alpha) * sheen_V(NdotV, NdotL)), base_atten);
    }

    v3 coat = v3_splat(0.0f);
    if (L->coat_w > 0.0f) {
        float Dc = ggx_D(NdotH, L->coat_alpha), Gc = ggx_G(NdotV, NdotL, L->coat_alpha);
        float Fc = L->coat_w * fresnel_schlick_s(VdotH, L->coat_f0);
        coat = v3_splat(Fc * Dc * Gc / (4.0f * NdotV * NdotL));
    }

    float pdf_diff = NdotL * MTLX_INV_PI;
    float inv_jacobian = VdotH > 0.0f ? 0.25f / VdotH : 0.0f;
    float pdf_spec = ggx_D(NdotH, L->alpha) * NdotH * inv_jacobian;
    float pdf_coat = ggx_D(NdotH, L->coat_alpha) * NdotH * inv_jacobian;
    float pdf_conductor = 0.0f;
    if (L->conductor_weight_sum > 0.0f) {
        for (int i = 0; i < L->conductor_lobe_count; ++i) {
            const MtlxConductorLobe *lobe = &L->conductor_lobes[i];
            v3 ln = conductor_normal(lobe, N, wo);
            pdf_conductor += (conductor_sampling_weight(lobe) / L->conductor_weight_sum) *
                conductor_reflection_pdf(lobe, ln, wo, H, v3_dot(ln,H), VdotH);
        }
    }
    *pdf = L->pd * pdf_diff + L->ps * pdf_spec + L->pc * pdf_coat + L->pm * pdf_conductor;

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
    } else if (u < L.pg + L.pc + L.ps + L.pm && L.conductor_weight_sum > 0.0f) {
        float target = ((u - L.pg - L.pc - L.ps) / L.pm) * L.conductor_weight_sum;
        const MtlxConductorLobe *chosen = NULL;
        for (int i = 0; i < L.conductor_lobe_count; ++i) {
            float weight = conductor_sampling_weight(&L.conductor_lobes[i]);
            if (weight <= 0.0f) continue;
            chosen = &L.conductor_lobes[i];
            if (target < weight) break;
            target -= weight;
        }
        if (!chosen) return 0;
        v3 ln = conductor_normal(chosen, N, wo), tangent, bitangent;
        if (!(v3_dot(wo, ln) > 0.0f)) return 0;
        conductor_frame(ln, chosen->tangent, &tangent, &bitangent);
        float ax = conductor_proposal_alpha(chosen->roughness);
        float ay = conductor_proposal_alpha(chosen->roughness_y);
        v3 local_view = v3_make(v3_dot(wo,tangent),v3_dot(wo,bitangent),v3_dot(wo,ln));
        v3 h = sample_conductor_vndf(local_view, ax, ay, u1, u2);
        v3 H = v3_normalize(to_world(h, tangent, bitangent, ln));
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
    BsdfLobeWeights weights = {L.pd, L.ps + L.pm, L.pc, L.pg};
    return weights;
}
