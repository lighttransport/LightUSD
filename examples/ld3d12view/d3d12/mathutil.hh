// ld3d12view - small column-major 4x4 helpers shared by lighting and picking.
#pragma once

#include <cmath>
#include <utility>

namespace ld3d12 {

// Gauss-Jordan inverse of a general column-major 4x4 matrix. Returns false for
// a singular matrix.
inline bool Invert4x4(const float m[16], float out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) a[r][c] = m[c * 4 + r];
    for (int c = 0; c < 4; ++c) a[r][4 + c] = r == c ? 1.0 : 0.0;
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r)
      if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
    if (std::fabs(a[pivot][col]) < 1e-30) return false;
    if (pivot != col)
      for (int c = 0; c < 8; ++c) std::swap(a[pivot][c], a[col][c]);
    const double inv = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      if (f == 0.0) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[c * 4 + r] = static_cast<float>(a[r][4 + c]);
  return true;
}

// out = M * (p, w), with the homogeneous divide applied for w == 1 points.
inline void TransformPoint(const float m[16], const float p[3], float out[3]) {
  float r[4];
  for (int i = 0; i < 4; ++i)
    r[i] = m[0 * 4 + i] * p[0] + m[1 * 4 + i] * p[1] + m[2 * 4 + i] * p[2] +
           m[3 * 4 + i];
  const float w = std::fabs(r[3]) > 1e-20f ? r[3] : 1e-20f;
  for (int i = 0; i < 3; ++i) out[i] = r[i] / w;
}

inline void TransformDir(const float m[16], const float d[3], float out[3]) {
  for (int i = 0; i < 3; ++i)
    out[i] = m[0 * 4 + i] * d[0] + m[1 * 4 + i] * d[1] + m[2 * 4 + i] * d[2];
}

}  // namespace ld3d12
