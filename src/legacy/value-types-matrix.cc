// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// matrix{2,3,4}{f,d} member/free functions — split out of value-types.cc.
// set_row/set_scale/set_translation, cross-precision ctors/operator=, operator==
// (math::is_close per element), and MatAdd/MatSub (+ MatAddImpl/MatSubImpl). These
// are independent of the lerp + half-conversion code that stays in value-types.cc,
// so isolating them shortens that TU (faster single-TU rebuilds). All matrix
// members/free functions are declared in value-types.hh.
#include "value-types.hh"

#include "common-macros.inc"
#include "math-util.inc"

namespace lightusd {
namespace value {

// matrix set_row, set_scale, set_translation methods

void matrix2f::set_row(uint32_t row, float x, float y) {
  if (row < 2) {
    m[row][0] = x;
    m[row][1] = y;
  }
}

void matrix2f::set_scale(float sx, float sy) {
  m[0][0] = sx;
  m[0][1] = 0.0f;

  m[1][0] = 0.0f;
  m[1][1] = sy;
}

void matrix3f::set_row(uint32_t row, float x, float y, float z) {
  if (row < 3) {
    m[row][0] = x;
    m[row][1] = y;
    m[row][2] = z;
  }
}

void matrix3f::set_scale(float sx, float sy, float sz) {
  m[0][0] = sx;
  m[0][1] = 0.0f;
  m[0][2] = 0.0f;

  m[1][0] = 0.0f;
  m[1][1] = sy;
  m[1][2] = 0.0f;

  m[2][0] = 0.0f;
  m[2][1] = 0.0f;
  m[2][2] = sz;
}

void matrix3f::set_translation(float tx, float ty, float tz) {
  m[2][0] = tx;
  m[2][1] = ty;
  m[2][2] = tz;
}

void matrix4f::set_row(uint32_t row, float x, float y, float z, float w) {
  if (row < 4) {
    m[row][0] = x;
    m[row][1] = y;
    m[row][2] = z;
    m[row][3] = w;
  }
}

void matrix4f::set_scale(float sx, float sy, float sz) {
  m[0][0] = sx;
  m[0][1] = 0.0f;
  m[0][2] = 0.0f;
  m[0][3] = 0.0f;

  m[1][0] = 0.0f;
  m[1][1] = sy;
  m[1][2] = 0.0f;
  m[1][3] = 0.0f;

  m[2][0] = 0.0f;
  m[2][1] = 0.0f;
  m[2][2] = sz;
  m[2][3] = 0.0f;

  m[3][0] = 0.0f;
  m[3][1] = 0.0f;
  m[3][2] = 0.0f;
  m[3][3] = 1.0f;
}

void matrix4f::set_translation(float tx, float ty, float tz) {
  m[3][0] = tx;
  m[3][1] = ty;
  m[3][2] = tz;
}

void matrix2d::set_row(uint32_t row, double x, double y) {
  if (row < 2) {
    m[row][0] = x;
    m[row][1] = y;
  }
}

void matrix2d::set_scale(double sx, double sy) {
  m[0][0] = sx;
  m[0][1] = 0.0;

  m[1][0] = 0.0;
  m[1][1] = sy;
}

void matrix3d::set_row(uint32_t row, double x, double y, double z) {
  if (row < 3) {
    m[row][0] = x;
    m[row][1] = y;
    m[row][2] = z;
  }
}

void matrix3d::set_scale(double sx, double sy, double sz) {
  m[0][0] = sx;
  m[0][1] = 0.0;
  m[0][2] = 0.0;

  m[1][0] = 0.0;
  m[1][1] = sy;
  m[1][2] = 0.0;

  m[2][0] = 0.0;
  m[2][1] = 0.0;
  m[2][2] = sz;
}

void matrix4d::set_row(uint32_t row, double x, double y, double z, double w) {
  if (row < 4) {
    m[row][0] = x;
    m[row][1] = y;
    m[row][2] = z;
    m[row][3] = w;
  }
}

void matrix4d::set_scale(double sx, double sy, double sz) {
  m[0][0] = sx;
  m[0][1] = 0.0;
  m[0][2] = 0.0;
  m[0][3] = 0.0;

  m[1][0] = 0.0;
  m[1][1] = sy;
  m[1][2] = 0.0;
  m[1][3] = 0.0;

  m[2][0] = 0.0;
  m[2][1] = 0.0;
  m[2][2] = sz;
  m[2][3] = 0.0;

  m[3][0] = 0.0;
  m[3][1] = 0.0;
  m[3][2] = 0.0;
  m[3][3] = 1.0;
}

matrix2f::matrix2f(const matrix2d &src) {
  (*this) = src;
}

matrix2f &matrix2f::operator=(const matrix2d &src) {

  for (size_t j = 0; j < 2; j++) {
    for (size_t i = 0; i < 2; i++) {
      m[j][i] = float(src.m[j][i]);
    }
  }

  return *this;
}

matrix3f::matrix3f(const matrix3d &src) {
  (*this) = src;
}

matrix3f &matrix3f::operator=(const matrix3d &src) {

  for (size_t j = 0; j < 3; j++) {
    for (size_t i = 0; i < 3; i++) {
      m[j][i] = float(src.m[j][i]);
    }
  }

  return *this;
}

matrix4f::matrix4f(const matrix4d &src) {
  (*this) = src;
}

matrix4f &matrix4f::operator=(const matrix4d &src) {

  for (size_t j = 0; j < 4; j++) {
    for (size_t i = 0; i < 4; i++) {
      m[j][i] = float(src.m[j][i]);
    }
  }

  return *this;
}

matrix2d &matrix2d::operator=(const matrix2f &src) {

  for (size_t j = 0; j < 2; j++) {
    for (size_t i = 0; i < 2; i++) {
      m[j][i] = double(src.m[j][i]);
    }
  }

  return *this;
}

matrix3d &matrix3d::operator=(const matrix3f &src) {

  for (size_t j = 0; j < 3; j++) {
    for (size_t i = 0; i < 3; i++) {
      m[j][i] = double(src.m[j][i]);
    }
  }

  return *this;
}

matrix4d &matrix4d::operator=(const matrix4f &src) {

  for (size_t j = 0; j < 4; j++) {
    for (size_t i = 0; i < 4; i++) {
      m[j][i] = double(src.m[j][i]);
    }
  }

  return *this;
}



bool operator==(const matrix3f &a, const matrix3f &b) {
  return math::is_close(a.m[0][0], b.m[0][0]) &&
         math::is_close(a.m[0][1], b.m[0][1]) &&
         math::is_close(a.m[0][2], b.m[0][2]) &&
         math::is_close(a.m[1][0], b.m[1][0]) &&
         math::is_close(a.m[1][1], b.m[1][1]) &&
         math::is_close(a.m[1][2], b.m[1][2]) &&
         math::is_close(a.m[2][0], b.m[2][0]) &&
         math::is_close(a.m[2][1], b.m[2][1]) &&
         math::is_close(a.m[2][2], b.m[2][2]);
}

bool operator==(const matrix4f &a, const matrix4f &b) {
  return math::is_close(a.m[0][0], b.m[0][0]) &&
         math::is_close(a.m[0][1], b.m[0][1]) &&
         math::is_close(a.m[0][2], b.m[0][2]) &&
         math::is_close(a.m[0][3], b.m[0][3]) &&
         math::is_close(a.m[1][0], b.m[1][0]) &&
         math::is_close(a.m[1][1], b.m[1][1]) &&
         math::is_close(a.m[1][2], b.m[1][2]) &&
         math::is_close(a.m[1][3], b.m[1][3]) &&
         math::is_close(a.m[2][0], b.m[2][0]) &&
         math::is_close(a.m[2][1], b.m[2][1]) &&
         math::is_close(a.m[2][2], b.m[2][2]) &&
         math::is_close(a.m[2][3], b.m[2][3]) &&
         math::is_close(a.m[3][0], b.m[3][0]) &&
         math::is_close(a.m[3][1], b.m[3][1]) &&
         math::is_close(a.m[3][2], b.m[3][2]) &&
         math::is_close(a.m[3][3], b.m[3][3]);
}

bool operator==(const matrix2d &a, const matrix2d &b) {
  return math::is_close(a.m[0][0], b.m[0][0]) &&
         math::is_close(a.m[0][1], b.m[0][1]) &&
         math::is_close(a.m[1][0], b.m[1][0]) &&
         math::is_close(a.m[1][1], b.m[1][1]);
}

bool operator==(const matrix3d &a, const matrix3d &b) {
  return math::is_close(a.m[0][0], b.m[0][0]) &&
         math::is_close(a.m[0][1], b.m[0][1]) &&
         math::is_close(a.m[0][2], b.m[0][2]) &&
         math::is_close(a.m[1][0], b.m[1][0]) &&
         math::is_close(a.m[1][1], b.m[1][1]) &&
         math::is_close(a.m[1][2], b.m[1][2]) &&
         math::is_close(a.m[2][0], b.m[2][0]) &&
         math::is_close(a.m[2][1], b.m[2][1]) &&
         math::is_close(a.m[2][2], b.m[2][2]);
}

bool operator==(const matrix4d &a, const matrix4d &b) {
  return math::is_close(a.m[0][0], b.m[0][0]) &&
         math::is_close(a.m[0][1], b.m[0][1]) &&
         math::is_close(a.m[0][2], b.m[0][2]) &&
         math::is_close(a.m[0][3], b.m[0][3]) &&
         math::is_close(a.m[1][0], b.m[1][0]) &&
         math::is_close(a.m[1][1], b.m[1][1]) &&
         math::is_close(a.m[1][2], b.m[1][2]) &&
         math::is_close(a.m[1][3], b.m[1][3]) &&
         math::is_close(a.m[2][0], b.m[2][0]) &&
         math::is_close(a.m[2][1], b.m[2][1]) &&
         math::is_close(a.m[2][2], b.m[2][2]) &&
         math::is_close(a.m[2][3], b.m[2][3]) &&
         math::is_close(a.m[3][0], b.m[3][0]) &&
         math::is_close(a.m[3][1], b.m[3][1]) &&
         math::is_close(a.m[3][2], b.m[3][2]) &&
         math::is_close(a.m[3][3], b.m[3][3]);
}

// ---------------------------------------------------------------
// Concrete matrix operation implementations (moved from header)
// ---------------------------------------------------------------

namespace {

template <typename MTy, typename STy, size_t N>
MTy MultImpl(const MTy &m, const MTy &n) {
  MTy ret;
  for (size_t j = 0; j < N; j++) {
    for (size_t i = 0; i < N; i++) {
      STy value = static_cast<STy>(0);
      for (size_t k = 0; k < N; k++) {
        value += m.m[j][k] * n.m[k][i];
      }
      ret.m[j][i] = value;
    }
  }
  return ret;
}

template <typename MTy, size_t N>
MTy MatAddImpl(const MTy &m, const MTy &n) {
  MTy ret;
  for (size_t j = 0; j < N; j++) {
    for (size_t i = 0; i < N; i++) {
      ret.m[j][i] = m.m[j][i] + n.m[j][i];
    }
  }
  return ret;
}

template <typename MTy, size_t N>
MTy MatSubImpl(const MTy &m, const MTy &n) {
  MTy ret;
  for (size_t j = 0; j < N; j++) {
    for (size_t i = 0; i < N; i++) {
      ret.m[j][i] = m.m[j][i] - n.m[j][i];
    }
  }
  return ret;
}

}  // namespace

matrix2f Mult(const matrix2f &m, const matrix2f &n) { return MultImpl<matrix2f, float, 2>(m, n); }
matrix3f Mult(const matrix3f &m, const matrix3f &n) { return MultImpl<matrix3f, float, 3>(m, n); }
matrix4f Mult(const matrix4f &m, const matrix4f &n) { return MultImpl<matrix4f, float, 4>(m, n); }
matrix2d Mult(const matrix2d &m, const matrix2d &n) { return MultImpl<matrix2d, double, 2>(m, n); }
matrix3d Mult(const matrix3d &m, const matrix3d &n) { return MultImpl<matrix3d, double, 3>(m, n); }
matrix4d Mult(const matrix4d &m, const matrix4d &n) { return MultImpl<matrix4d, double, 4>(m, n); }

matrix2f MatAdd(const matrix2f &a, const matrix2f &b) { return MatAddImpl<matrix2f, 2>(a, b); }
matrix3f MatAdd(const matrix3f &a, const matrix3f &b) { return MatAddImpl<matrix3f, 3>(a, b); }
matrix4f MatAdd(const matrix4f &a, const matrix4f &b) { return MatAddImpl<matrix4f, 4>(a, b); }
matrix2d MatAdd(const matrix2d &a, const matrix2d &b) { return MatAddImpl<matrix2d, 2>(a, b); }
matrix3d MatAdd(const matrix3d &a, const matrix3d &b) { return MatAddImpl<matrix3d, 3>(a, b); }
matrix4d MatAdd(const matrix4d &a, const matrix4d &b) { return MatAddImpl<matrix4d, 4>(a, b); }

matrix2f MatSub(const matrix2f &a, const matrix2f &b) { return MatSubImpl<matrix2f, 2>(a, b); }
matrix3f MatSub(const matrix3f &a, const matrix3f &b) { return MatSubImpl<matrix3f, 3>(a, b); }
matrix4f MatSub(const matrix4f &a, const matrix4f &b) { return MatSubImpl<matrix4f, 4>(a, b); }
matrix2d MatSub(const matrix2d &a, const matrix2d &b) { return MatSubImpl<matrix2d, 2>(a, b); }
matrix3d MatSub(const matrix3d &a, const matrix3d &b) { return MatSubImpl<matrix3d, 3>(a, b); }
matrix4d MatSub(const matrix4d &a, const matrix4d &b) { return MatSubImpl<matrix4d, 4>(a, b); }

}  // namespace value
}  // namespace lightusd
