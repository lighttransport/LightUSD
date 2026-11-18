/*
 * vecmath.h - minimal C11 vec3 / mat4 / sampling helpers (header-only).
 *
 * Everything is `static inline`; include freely from any TU. Vectors are plain
 * float[3]; we keep a tiny tagged `v3` struct for readable return values.
 */
#ifndef MTLXRENDER_VECMATH_H_
#define MTLXRENDER_VECMATH_H_

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MTLX_PI 3.14159265358979323846f
#define MTLX_INV_PI 0.31830988618379067154f
#define MTLX_TWO_PI 6.28318530717958647692f

typedef struct v3 { float x, y, z; } v3;

static inline v3 v3_make(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static inline v3 v3_splat(float s) { v3 r = {s, s, s}; return r; }
static inline v3 v3_add(v3 a, v3 b) { return v3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 v3_sub(v3 a, v3 b) { return v3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 v3_mul(v3 a, v3 b) { return v3_make(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline v3 v3_scale(v3 a, float s) { return v3_make(a.x * s, a.y * s, a.z * s); }
static inline v3 v3_neg(v3 a) { return v3_make(-a.x, -a.y, -a.z); }
static inline float v3_dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 v3_cross(v3 a, v3 b) {
    return v3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3_len(v3 a) { return sqrtf(v3_dot(a, a)); }
static inline float v3_maxc(v3 a) { return a.x > a.y ? (a.x > a.z ? a.x : a.z) : (a.y > a.z ? a.y : a.z); }
static inline float v3_avg(v3 a) { return (a.x + a.y + a.z) * (1.0f / 3.0f); }
static inline v3 v3_normalize(v3 a) {
    float l = v3_len(a);
    return l > 0.0f ? v3_scale(a, 1.0f / l) : a;
}
static inline v3 v3_lerp(v3 a, v3 b, float t) { return v3_add(v3_scale(a, 1.0f - t), v3_scale(b, t)); }
static inline v3 v3_min(v3 a, v3 b) {
    return v3_make(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z);
}
static inline v3 v3_max(v3 a, v3 b) {
    return v3_make(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z);
}
static inline int v3_is_finite(v3 a) {
    return isfinite(a.x) && isfinite(a.y) && isfinite(a.z);
}

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float minf(float a, float b) { return a < b ? a : b; }

/* Build an orthonormal basis (t,b) around a unit normal n (Duff et al. 2017). */
static inline void onb(v3 n, v3 *t, v3 *b) {
    float sign = copysignf(1.0f, n.z);
    float a = -1.0f / (sign + n.z);
    float bb = n.x * n.y * a;
    *t = v3_make(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    *b = v3_make(bb, sign + n.y * n.y * a, -n.y);
}

/* Transform a tangent-space direction (x*t + y*b + z*n). */
static inline v3 to_world(v3 d, v3 t, v3 b, v3 n) {
    return v3_add(v3_add(v3_scale(t, d.x), v3_scale(b, d.y)), v3_scale(n, d.z));
}

/* ---- RNG: PCG32 -------------------------------------------------------- */
typedef struct { uint64_t state, inc; } pcg32;

static inline uint32_t pcg32_next(pcg32 *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}
static inline void pcg32_seed(pcg32 *r, uint64_t seed, uint64_t seq) {
    r->state = 0u;
    r->inc = (seq << 1u) | 1u;
    pcg32_next(r);
    r->state += seed;
    pcg32_next(r);
}
static inline float pcg32_f(pcg32 *r) {
    return (float)(pcg32_next(r) >> 8) * (1.0f / 16777216.0f); /* [0,1) */
}

/* ---- Sampling ---------------------------------------------------------- */
static inline v3 sample_cosine_hemisphere(float u1, float u2) {
    float r = sqrtf(u1);
    float phi = MTLX_TWO_PI * u2;
    return v3_make(r * cosf(phi), r * sinf(phi), sqrtf(maxf(0.0f, 1.0f - u1)));
}

/* GGX / Trowbridge-Reitz visible-normal-ish: sample a half vector in tangent
 * space given roughness alpha (isotropic). */
static inline v3 sample_ggx_h(float alpha, float u1, float u2) {
    float phi = MTLX_TWO_PI * u1;
    float cos_t = sqrtf((1.0f - u2) / (1.0f + (alpha * alpha - 1.0f) * u2));
    float sin_t = sqrtf(maxf(0.0f, 1.0f - cos_t * cos_t));
    return v3_make(sin_t * cosf(phi), sin_t * sinf(phi), cos_t);
}

static inline v3 sample_uniform_sphere(float u1, float u2) {
    float z = 1.0f - 2.0f * u1;
    float r = sqrtf(maxf(0.0f, 1.0f - z * z));
    float phi = MTLX_TWO_PI * u2;
    return v3_make(r * cosf(phi), r * sinf(phi), z);
}

static inline float luminance(v3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

#endif /* MTLXRENDER_VECMATH_H_ */
