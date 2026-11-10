// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Small value/math types for the LightUSD apps (lusdrender, lusdview). These keep the layout and arithmetic
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
#include <string>
#include <vector>

#include "external/linalg.h"

namespace lvmath {

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

struct quatd {
  double3 imag;
  double real;
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

inline value::matrix4d to_matrix(const value::quatf &q) {
  const linalg::mat<double, 3, 3> m33 = linalg::qmat<double>(
      {double(q.imag[0]), double(q.imag[1]), double(q.imag[2]), double(q.real)});
  value::matrix4d m = value::matrix4d::identity();
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) m.m[r][c] = m33[r][c];
  return m;
}

inline value::matrix4d inverse(const value::matrix4d &src) {
  linalg::mat<double, 4, 4> m;
  std::memcpy(&m[0][0], src.m, sizeof(double) * 16);
  const linalg::mat<double, 4, 4> r = linalg::inverse(m);
  value::matrix4d out;
  std::memcpy(out.m, &r[0][0], sizeof(double) * 16);
  return out;
}

inline double determinant(const value::matrix3d &src) {
  linalg::mat<double, 3, 3> m;
  std::memcpy(&m[0][0], src.m, sizeof(double) * 9);
  return linalg::determinant(m);
}

// Translation / rotation / scale of an affine row-vector matrix (reflection
// folded into scale.x).
inline bool decompose(const value::matrix4d &m, value::double3 *translation,
                      value::quatd *rotation, value::double3 *scale) {
  if (!translation || !rotation || !scale) return false;
  (*translation)[0] = m.m[3][0];
  (*translation)[1] = m.m[3][1];
  (*translation)[2] = m.m[3][2];
  const double sx = std::sqrt(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
  const double sy = std::sqrt(m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2]);
  const double sz = std::sqrt(m.m[2][0] * m.m[2][0] + m.m[2][1] * m.m[2][1] + m.m[2][2] * m.m[2][2]);
  (*scale)[0] = sx;
  (*scale)[1] = sy;
  (*scale)[2] = sz;
  constexpr double kEpsilon = 1e-10;
  if (sx < kEpsilon || sy < kEpsilon || sz < kEpsilon) {
    rotation->real = 1.0;
    rotation->imag = {0.0, 0.0, 0.0};
    return true;
  }
  value::matrix3d r;
  const double s[3] = {sx, sy, sz};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) r.m[i][j] = m.m[i][j] / s[i];
  if (determinant(r) < 0.0) {
    (*scale)[0] = -sx;
    for (int j = 0; j < 3; j++) r.m[0][j] = -r.m[0][j];
  }
  // Shepperd's method for the row-vector (transposed) rotation matrix.
  const double trace = r.m[0][0] + r.m[1][1] + r.m[2][2];
  if (trace > 0.0) {
    const double q = std::sqrt(trace + 1.0) * 2.0;
    rotation->real = 0.25 * q;
    rotation->imag[0] = (r.m[1][2] - r.m[2][1]) / q;
    rotation->imag[1] = (r.m[2][0] - r.m[0][2]) / q;
    rotation->imag[2] = (r.m[0][1] - r.m[1][0]) / q;
  } else if (r.m[0][0] > r.m[1][1] && r.m[0][0] > r.m[2][2]) {
    const double q = std::sqrt(1.0 + r.m[0][0] - r.m[1][1] - r.m[2][2]) * 2.0;
    rotation->real = (r.m[1][2] - r.m[2][1]) / q;
    rotation->imag[0] = 0.25 * q;
    rotation->imag[1] = (r.m[0][1] + r.m[1][0]) / q;
    rotation->imag[2] = (r.m[0][2] + r.m[2][0]) / q;
  } else if (r.m[1][1] > r.m[2][2]) {
    const double q = std::sqrt(1.0 + r.m[1][1] - r.m[0][0] - r.m[2][2]) * 2.0;
    rotation->real = (r.m[2][0] - r.m[0][2]) / q;
    rotation->imag[0] = (r.m[0][1] + r.m[1][0]) / q;
    rotation->imag[1] = 0.25 * q;
    rotation->imag[2] = (r.m[1][2] + r.m[2][1]) / q;
  } else {
    const double q = std::sqrt(1.0 + r.m[2][2] - r.m[0][0] - r.m[1][1]) * 2.0;
    rotation->real = (r.m[0][1] - r.m[1][0]) / q;
    rotation->imag[0] = (r.m[0][2] + r.m[2][0]) / q;
    rotation->imag[1] = (r.m[1][2] + r.m[2][1]) / q;
    rotation->imag[2] = 0.25 * q;
  }
  const double len = std::sqrt(rotation->real * rotation->real +
                               rotation->imag[0] * rotation->imag[0] +
                               rotation->imag[1] * rotation->imag[1] +
                               rotation->imag[2] * rotation->imag[2]);
  if (len > kEpsilon) {
    rotation->real /= len;
    for (int i = 0; i < 3; i++) rotation->imag[size_t(i)] /= len;
  } else {
    rotation->real = 1.0;
    rotation->imag = {0.0, 0.0, 0.0};
  }
  return true;
}

// World-space joint transforms from local transforms and parent indices
// (UsdSkelConcatJointTransforms; parents precede children).
inline bool ConcatJointTransforms(const std::vector<int> &topology,
                                  const std::vector<value::matrix4d> &local,
                                  std::vector<value::matrix4d> *world,
                                  const value::matrix4d *root = nullptr) {
  if (!world || local.size() != topology.size()) return false;
  const size_t n = topology.size();
  world->resize(n);
  for (size_t i = 0; i < n; i++) {
    const int parent = topology[i];
    if (parent < 0) {
      (*world)[i] = root ? local[i] * (*root) : local[i];
    } else if (size_t(parent) < n) {
      (*world)[i] = local[i] * (*world)[size_t(parent)];
    } else {
      (*world)[i] = local[i];
    }
  }
  return true;
}

// Linear blend skinning: rest points through geomBindTransform, the weighted
// joint transforms, then back through inverse(geomBindTransform).
inline bool SkinPointsLBS(const std::vector<value::point3f> &rest,
                          const value::matrix4d &geomBind,
                          const std::vector<value::matrix4d> &joints,
                          const std::vector<int> &jointIndices,
                          const std::vector<float> &jointWeights,
                          int influences, std::vector<value::point3f> *out,
                          std::string *err = nullptr) {
  if (!out) { if (err) *err = "skinnedPoints is null."; return false; }
  if (influences < 1) { if (err) *err = "numInfluencesPerPoint must be >= 1."; return false; }
  const size_t n = rest.size();
  const size_t expected = n * size_t(influences);
  if (jointIndices.size() != expected || jointWeights.size() != expected) {
    if (err) *err = "joint influence array size mismatch.";
    return false;
  }
  value::matrix4d invBind = value::matrix4d::identity();
  if (!inverse(geomBind, invBind)) {
    if (err) *err = "Failed to invert geomBindTransform.";
    return false;
  }
  const int numJoints = int(joints.size());
  out->resize(n);
  auto xform = [](const value::matrix4d &m, double x, double y, double z,
                  double *ox, double *oy, double *oz) {
    double rx = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
    double ry = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
    double rz = x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2];
    const double rw = x * m.m[0][3] + y * m.m[1][3] + z * m.m[2][3] + m.m[3][3];
    if (std::abs(rw) > 1e-10) { rx /= rw; ry /= rw; rz /= rw; }
    *ox = rx; *oy = ry; *oz = rz;
  };
  for (size_t pi = 0; pi < n; pi++) {
    double sx, sy, sz;
    xform(geomBind, double(rest[pi].x), double(rest[pi].y), double(rest[pi].z),
          &sx, &sy, &sz);
    double ox = 0.0, oy = 0.0, oz = 0.0;
    const size_t base = pi * size_t(influences);
    for (int ji = 0; ji < influences; ji++) {
      const int idx = jointIndices[base + size_t(ji)];
      const float w = jointWeights[base + size_t(ji)];
      if (w == 0.0f || idx < 0 || idx >= numJoints) continue;
      const value::matrix4d &jx = joints[size_t(idx)];
      ox += double(w) * (sx * jx.m[0][0] + sy * jx.m[1][0] + sz * jx.m[2][0] + jx.m[3][0]);
      oy += double(w) * (sx * jx.m[0][1] + sy * jx.m[1][1] + sz * jx.m[2][1] + jx.m[3][1]);
      oz += double(w) * (sx * jx.m[0][2] + sy * jx.m[1][2] + sz * jx.m[2][2] + jx.m[3][2]);
    }
    double gx, gy, gz;
    xform(invBind, ox, oy, oz, &gx, &gy, &gz);
    (*out)[pi].x = float(gx);
    (*out)[pi].y = float(gy);
    (*out)[pi].z = float(gz);
  }
  return true;
}

}  // namespace lvmath
