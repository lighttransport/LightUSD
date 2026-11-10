// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Small value/math types for lusdrender. These keep the layout and arithmetic
// of the vocabulary the renderer was written against (row-major double
// matrix4d with translation in m[3][*], quaternion = {imag, real}) so that
// renders stay bit-identical, without depending on the legacy library.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "external/linalg.h"

namespace lusdr {

// UsdGeomImageable purpose.
enum class Purpose { Default, Render, Proxy, Guide };

// Stage up axis.
enum class Axis { X, Y, Z, Invalid };

namespace value {

using float2 = std::array<float, 2>;
using float3 = std::array<float, 3>;
using float4 = std::array<float, 4>;
using double3 = std::array<double, 3>;

struct point3f {
  float x, y, z;
  float operator[](size_t i) const { return *(&x + i); }
  float &operator[](size_t i) { return *(&x + i); }
};

struct vector3f {
  float x, y, z;
  float operator[](size_t i) const { return *(&x + i); }
  float &operator[](size_t i) { return *(&x + i); }
};

struct color3f {
  float r, g, b;
  float operator[](size_t i) const { return *(&r + i); }
  float &operator[](size_t i) { return *(&r + i); }
};

struct quatf {
  float3 imag;
  float real;
};

struct matrix3d {
  double m[3][3];
  static matrix3d identity() {
    matrix3d r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
    return r;
  }
};

struct matrix4d {
  double m[4][4];
  static matrix4d identity() {
    matrix4d r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0;
    return r;
  }
};

inline matrix4d operator*(const matrix4d &a, const matrix4d &b) {
  matrix4d r;
  for (size_t j = 0; j < 4; j++)
    for (size_t i = 0; i < 4; i++) {
      double v = 0.0;
      for (size_t k = 0; k < 4; k++) v += a.m[j][k] * b.m[k][i];
      r.m[j][i] = v;
    }
  return r;
}

struct TimeCode {
  static constexpr double Default() {
    return std::numeric_limits<double>::quiet_NaN();
  }
};

}  // namespace value

inline value::matrix3d to_matrix3x3(const value::quatf &q) {
  const linalg::mat<double, 3, 3> m33 = linalg::qmat<double>(
      {double(q.imag[0]), double(q.imag[1]), double(q.imag[2]), double(q.real)});
  value::matrix3d m = value::matrix3d::identity();
  std::memcpy(m.m, &m33[0][0], sizeof(double) * 9);
  return m;
}

inline value::matrix4d to_matrix(const value::matrix3d &m33,
                                 const value::double3 &tx) {
  value::matrix4d m = value::matrix4d::identity();
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) m.m[r][c] = m33.m[r][c];
  m.m[3][0] = tx[0];
  m.m[3][1] = tx[1];
  m.m[3][2] = tx[2];
  return m;
}

inline bool inverse(const value::matrix4d &src, value::matrix4d &dst,
                    double eps = 0.0) {
  linalg::mat<double, 4, 4> m;
  std::memcpy(&m[0][0], src.m, sizeof(double) * 16);
  const double det = linalg::determinant(m);
  const double a = std::fabs(det);
  if (a <= eps || a <= eps * a) return false;
  const linalg::mat<double, 4, 4> r = linalg::adjugate(m) / det;
  std::memcpy(dst.m, &r[0][0], sizeof(double) * 16);
  return true;
}

}  // namespace lusdr
