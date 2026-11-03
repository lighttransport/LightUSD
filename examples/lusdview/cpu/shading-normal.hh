// SPDX-License-Identifier: Apache-2.0
// lusdview - CPU tangent-space normal mapping math.
//
// The Vulkan RT shader rebuilds a hit tangent frame from the triangle's
// positions and authored UVs, then transforms the authored tangent-space normal
// into world space. The CPU reference path needs the same transform to shade
// normal maps; keeping it header-only makes the math independently testable.

#pragma once

#include <cmath>

namespace lusdview {

struct ShadingTangentFrame {
  float tangent[3] = {1.0f, 0.0f, 0.0f};
  float bitangent[3] = {0.0f, 0.0f, -1.0f};
  float normal[3] = {0.0f, 1.0f, 0.0f};
};

inline void ShadingNormalize(float v[3]) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len > 1.0e-12f) {
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
  } else {
    v[0] = 0.0f;
    v[1] = 0.0f;
    v[2] = 0.0f;
  }
}

inline void ShadingCross(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

// Build an orthonormal shading frame from one triangle. `geometric_normal` is
// used when the UV parametrization is degenerate (zero determinant), matching
// the Vulkan RT shader's fallback tangent. `n` always equals the normalized
// input geometric normal.
inline void BuildShadingTangentFrame(const float p0[3], const float p1[3],
                                     const float p2[3], const float uv0[2],
                                     const float uv1[2], const float uv2[2],
                                     const float geometric_normal[3],
                                     ShadingTangentFrame* frame) {
  if (!frame) return;
  float n[3] = {geometric_normal[0], geometric_normal[1], geometric_normal[2]};
  ShadingNormalize(n);

  float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
  float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
  const float d1[2] = {uv1[0] - uv0[0], uv1[1] - uv0[1]};
  const float d2[2] = {uv2[0] - uv0[0], uv2[1] - uv0[1]};
  const float det = d1[0] * d2[1] - d1[1] * d2[0];

  float t[3];
  if (std::fabs(det) > 1.0e-12f) {
    t[0] = (e1[0] * d2[1] - e2[0] * d1[1]) / det;
    t[1] = (e1[1] * d2[1] - e2[1] * d1[1]) / det;
    t[2] = (e1[2] * d2[1] - e2[2] * d1[1]) / det;
  } else {
    // Fallback axis: +Z unless the normal is nearly parallel to it, matching
    // the Vulkan RT shader's `abs(N.z) < 0.999 ? (0,0,1) : (0,1,0)`.
    const float axis[3] = {0.0f,
                           std::fabs(n[2]) < 0.999f ? 0.0f : 1.0f,
                           std::fabs(n[2]) < 0.999f ? 1.0f : 0.0f};
    ShadingCross(axis, n, t);
  }
  ShadingNormalize(t);

  // Gram-Schmidt: the authored UVs are not guaranteed orthogonal in 3D.
  const float dot_tn = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
  t[0] -= dot_tn * n[0];
  t[1] -= dot_tn * n[1];
  t[2] -= dot_tn * n[2];
  ShadingNormalize(t);

  float b[3];
  ShadingCross(n, t, b);
  ShadingNormalize(b);
  // Mirrored UVs (negative determinant) flip the bitangent, matching the
  // Vulkan RT/raster and GL shader frame construction.
  if (det < 0.0f) {
    b[0] = -b[0];
    b[1] = -b[1];
    b[2] = -b[2];
  }

  frame->tangent[0] = t[0];
  frame->tangent[1] = t[1];
  frame->tangent[2] = t[2];
  frame->bitangent[0] = b[0];
  frame->bitangent[1] = b[1];
  frame->bitangent[2] = b[2];
  frame->normal[0] = n[0];
  frame->normal[1] = n[1];
  frame->normal[2] = n[2];
}

// Transform an authored tangent-space normal (already decoded to [-1, 1]) into
// world space and normalize it. A degenerate result falls back to the
// geometric normal.
inline void ApplyShadingTangentNormal(const ShadingTangentFrame& frame,
                                      const float tangent_normal[3],
                                      float out[3]) {
  out[0] = frame.tangent[0] * tangent_normal[0] +
           frame.bitangent[0] * tangent_normal[1] +
           frame.normal[0] * tangent_normal[2];
  out[1] = frame.tangent[1] * tangent_normal[0] +
           frame.bitangent[1] * tangent_normal[1] +
           frame.normal[1] * tangent_normal[2];
  out[2] = frame.tangent[2] * tangent_normal[0] +
           frame.bitangent[2] * tangent_normal[1] +
           frame.normal[2] * tangent_normal[2];
  const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  if (len <= 1.0e-12f) {
    out[0] = frame.normal[0];
    out[1] = frame.normal[1];
    out[2] = frame.normal[2];
    return;
  }
  out[0] /= len;
  out[1] /= len;
  out[2] /= len;
}

}  // namespace lusdview
