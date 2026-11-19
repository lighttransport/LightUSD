// SPDX-License-Identifier: Apache-2.0
//
// Header-only 3D Gaussian splat ray kernel shared by the LightUSD renderers.
//
// Ported from Blender 5.3 Cycles (Apache-2.0, Copyright 2026 Blender
// Foundation): intern/cycles/kernel/geom/gsplat_intersect.h, gsplat.h,
// scene/pointcloud.cpp (update_gsplat_radii) and
// util/math_spherical_harmonics.h. See LICENSE.3rdparty.
//
// Algorithm:
//  * Bounding sphere radius per [3dgrt2024] Eq. (7):
//      r = sqrt(2 ln(opacity / kAlphaMin)) * max(scale)
//  * Peak response along the ray, [3dgrt2024] Eq. (8): transform the ray into
//    the unit Gaussian frame (S^-1 R^T), t* = -dot(o, d) / dot(d, d).
//  * Response alpha = opacity * exp(-0.5 * |S^-1 R^T (x - mu)|^2).
//  * Stochastic opaque hit per [sun2025]: accept when hash(x) <= alpha, so
//    the existing opaque-hit BVH traversal integrates transmittance by Monte
//    Carlo. The deterministic GSplatAlpha() is provided for alpha-blending
//    paths and tests.
//  * Radiance: degree <= 3 real SH in the splat's object space (the same
//    basis/sign convention as the reference 3DGS code), + 0.5 offset. USD
//    stores linear radiance, so sRGB decoding is opt-in (Blender treats
//    trained splats as sRGB; see GSplatColorSpace).
//
// The hash differs from Cycles' Jenkins lookup3 (any stateless uniform hash
// is valid); all other math matches Cycles.
#pragma once

#include <cmath>
#include <cstdint>

namespace lightusd {
namespace gsplat {

constexpr float kAlphaMin = 0.01f;  // Minimum captured response (Cycles/3DGRT).

enum class GSplatColorSpace { Linear, SRGB };

struct Vec3 { float x, y, z; };
// Quaternion stored as USD quatf: real part w, imaginary (x, y, z).
struct Quat { float w, x, y, z; };

inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Rotation matrix rows (normalizes q like Cycles' quaternion_to_rotation).
inline void QuatToRows(Quat q, Vec3 r[3]) {
  const float n = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
  const float s = n > 0.f ? 2.f / n : 0.f;
  const float xx = q.x * q.x * s, yy = q.y * q.y * s, zz = q.z * q.z * s;
  const float xy = q.x * q.y * s, xz = q.x * q.z * s, yz = q.y * q.z * s;
  const float wx = q.w * q.x * s, wy = q.w * q.y * s, wz = q.w * q.z * s;
  r[0] = {1.f - (yy + zz), xy - wz, xz + wy};
  r[1] = {xy + wz, 1.f - (xx + zz), yz - wx};
  r[2] = {xz - wy, yz + wx, 1.f - (xx + yy)};
}

// Returns 0 when the splat can be culled (opacity < kAlphaMin).
inline float GSplatBoundingRadius(float opacity, Vec3 scale) {
  if (!(opacity >= kAlphaMin)) return 0.f;
  const float f = std::sqrt(2.f * std::log(opacity / kAlphaMin));
  float m = std::fabs(scale.x);
  if (std::fabs(scale.y) > m) m = std::fabs(scale.y);
  if (std::fabs(scale.z) > m) m = std::fabs(scale.z);
  return f * m;
}

struct GSplat {
  Vec3 mu;
  Quat rotation;
  Vec3 scale;     // linear standard deviations
  float opacity;  // linear [0, 1]
};

// S^-1 R^T v
inline Vec3 ToUnitFrame(const Vec3 rows[3], Vec3 inv_s, Vec3 v) {
  // R^T v = sum_i column_i... rows[] are rows of R, so (R^T v)_j = sum_i R_ij v_i
  const Vec3 rt = {rows[0].x * v.x + rows[1].x * v.y + rows[2].x * v.z,
                   rows[0].y * v.x + rows[1].y * v.y + rows[2].y * v.z,
                   rows[0].z * v.x + rows[1].z * v.y + rows[2].z * v.z};
  return {rt.x * inv_s.x, rt.y * inv_s.y, rt.z * inv_s.z};
}

struct GSplatPeak {
  float t;      // ray parameter of maximum response
  float alpha;  // response at t
  Vec3 x;       // world position at t
};

// Deterministic peak response; returns false when outside [tmin, tmax] or
// degenerate (zero scale -> NaN, rejected like Cycles).
inline bool GSplatPeakResponse(const GSplat& g, Vec3 ro, Vec3 rd, float tmin,
                               float tmax, GSplatPeak* out) {
  Vec3 rows[3];
  QuatToRows(g.rotation, rows);
  const Vec3 inv_s = {1.f / g.scale.x, 1.f / g.scale.y, 1.f / g.scale.z};
  const Vec3 og = ToUnitFrame(rows, inv_s, Sub(ro, g.mu));
  const Vec3 dg = ToUnitFrame(rows, inv_s, rd);
  const float t = -Dot(og, dg) / Dot(dg, dg);
  if (!(t >= tmin && t <= tmax)) return false;
  const Vec3 x = Add(ro, Mul(rd, t));
  const Vec3 e = ToUnitFrame(rows, inv_s, Sub(x, g.mu));
  out->t = t;
  out->alpha = g.opacity * std::exp(-0.5f * Dot(e, e));
  out->x = x;
  return true;
}

// Stateless hash of a position to [0, 1) (PCG-style integer mix). Cycles
// relies on per-sample ray jitter to decorrelate hash(x); renderers also mix
// the sample index into `seed` so samples stay independent even when primary
// rays coincide.
inline float HashPositionToUnit(Vec3 p, uint32_t seed = 0) {
  uint32_t h = 0x9e3779b9u ^ (seed * 0x85ebca6bu);
  const float v[3] = {p.x, p.y, p.z};
  for (float f : v) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    h ^= b + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h = h * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
  }
  return float(h >> 8) * (1.f / 16777216.f);
}

// Cycles gsplat_intersect_test: stochastic opaque hit.
inline bool GSplatIntersectStochastic(const GSplat& g, Vec3 ro, Vec3 rd,
                                      float tmin, float tmax, float* t_hit) {
  GSplatPeak p;
  if (!GSplatPeakResponse(g, ro, rd, tmin, tmax, &p)) return false;
  if (HashPositionToUnit(p.x) > p.alpha) return false;
  *t_hit = p.t;
  return true;
}

// ---------------------------------------------------------------------------
// World-space (affine) form used by renderers. For a splat in an object with
// linear transform L (column-vector convention), the unit-frame map is
// A = S^-1 R^T L^-1, which is exact for any non-singular affine placement
// (including shear / non-uniform scale, where a quat+scale decomposition of
// the world covariance would not be).
struct GSplatWorld {
  Vec3 mu;        // world center
  float a[9];     // row-major A
  float opacity;
  float radius;   // world bounding-sphere radius (0 = culled)
};

inline Vec3 MulA(const float* a, Vec3 v) {
  return {a[0] * v.x + a[1] * v.y + a[2] * v.z,
          a[3] * v.x + a[4] * v.y + a[5] * v.z,
          a[6] * v.x + a[7] * v.y + a[8] * v.z};
}

// Builds A from local rotation/scale and the inverse linear part of the
// object transform (row-major, column-vector convention). Returns false for
// degenerate scales (Cycles rejects those per ray via NaN; dropping them up
// front is equivalent and cheaper).
inline bool MakeGSplatWorld(Vec3 mu_world, Quat q, Vec3 scale, float opacity,
                            const float linv[9], float l_frobenius,
                            GSplatWorld* out) {
  if (!(scale.x > 0.f && scale.y > 0.f && scale.z > 0.f)) return false;
  const float r = GSplatBoundingRadius(opacity, scale);
  if (!(r > 0.f)) return false;
  Vec3 rows[3];
  QuatToRows(q, rows);
  const float inv_s[3] = {1.f / scale.x, 1.f / scale.y, 1.f / scale.z};
  // (R^T)[i][k] = R[k][i]; A[i][j] = inv_s[i] * sum_k R[k][i] * linv[k][j]
  const float* R = &rows[0].x;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      float s = 0.f;
      for (int k = 0; k < 3; ++k) s += R[k * 3 + i] * linv[k * 3 + j];
      out->a[i * 3 + j] = inv_s[i] * s;
    }
  out->mu = mu_world;
  out->opacity = opacity;
  // |L x| <= ||L||_F |x|: a safe world bound for the local sphere.
  out->radius = r * l_frobenius;
  return true;
}

inline bool GSplatWorldPeak(const GSplatWorld& g, Vec3 ro, Vec3 rd, float tmin,
                            float tmax, GSplatPeak* out) {
  const Vec3 og = MulA(g.a, Sub(ro, g.mu));
  const Vec3 dg = MulA(g.a, rd);
  const float t = -Dot(og, dg) / Dot(dg, dg);
  if (!(t >= tmin && t <= tmax)) return false;
  const Vec3 e = Add(og, Mul(dg, t));
  out->t = t;
  out->alpha = g.opacity * std::exp(-0.5f * Dot(e, e));
  out->x = Add(ro, Mul(rd, t));
  return true;
}

// Real SH (degree <= 3) for a normalized direction; coeffs holds
// (degree+1)^2 RGB triples, band-major, m = -l..l.
inline Vec3 EvalSH(int degree, const float* c, Vec3 v) {
  const float x = v.x, y = v.y, z = v.z;
  float b[16];
  b[0] = 0.282094792f;
  int n = 1;
  if (degree >= 1) {
    b[1] = -0.488602512f * y;
    b[2] = 0.488602512f * z;
    b[3] = -0.488602512f * x;
    n = 4;
  }
  if (degree >= 2) {
    b[4] = 1.092548431f * x * y;
    b[5] = -1.092548431f * y * z;
    b[6] = 0.315391565f * (3.f * z * z - 1.f);
    b[7] = -1.092548431f * x * z;
    b[8] = 0.546274215f * (x * x - y * y);
    n = 9;
  }
  if (degree >= 3) {
    b[9] = -0.590043589f * y * (3.f * x * x - y * y);
    b[10] = 2.890611442f * x * y * z;
    b[11] = -0.457045799f * y * (5.f * z * z - 1.f);
    b[12] = 0.373176332f * z * (5.f * z * z - 3.f);
    b[13] = -0.457045799f * x * (5.f * z * z - 1.f);
    b[14] = 1.445305721f * z * (x * x - y * y);
    b[15] = -0.590043590f * x * (x * x - 3.f * y * y);
    n = 16;
  }
  Vec3 r = {0, 0, 0};
  for (int i = 0; i < n; ++i)
    r = Add(r, Mul({c[i * 3], c[i * 3 + 1], c[i * 3 + 2]}, b[i]));
  return r;
}

inline float SrgbToLinear(float c) {
  if (c < 0.04045f) return c < 0.f ? 0.f : c * (1.f / 12.92f);
  return std::pow((c + 0.055f) * (1.f / 1.055f), 2.4f);
}

// Emission for a splat viewed along view_dir_object (object-space direction
// from the camera toward the splat; Cycles uses -wi).
inline Vec3 GSplatRadiance(int degree, const float* coeffs,
                           Vec3 view_dir_object, GSplatColorSpace cs) {
  const float l = std::sqrt(Dot(view_dir_object, view_dir_object));
  const Vec3 d = l > 0.f ? Mul(view_dir_object, 1.f / l) : Vec3{0, 0, 1};
  Vec3 c = EvalSH(degree, coeffs, d);
  c = {c.x + .5f, c.y + .5f, c.z + .5f};
  if (cs == GSplatColorSpace::SRGB)
    return {SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z)};
  return {c.x < 0.f ? 0.f : c.x, c.y < 0.f ? 0.f : c.y, c.z < 0.f ? 0.f : c.z};
}

}  // namespace gsplat
}  // namespace lightusd
