// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
// Matrix and frame arithmetic operators — moved from value-eval-util.hh to reduce header bloat

#include "value-eval-util.hh"

namespace lightusd {

value::matrix2f operator+(const value::matrix2f &a, const double b) {
  value::matrix2f dst;
  dst.m[0][0] = float(double(a.m[0][0]) + b);
  dst.m[0][1] = float(double(a.m[0][1]) + b);
  dst.m[1][0] = float(double(a.m[1][0]) + b);
  dst.m[1][1] = float(double(a.m[1][1]) + b);

  return dst;
}

value::matrix2f operator+(const double a, const value::matrix2f &b) {
  value::matrix2f dst;
  dst.m[0][0] = float(a + double(b.m[0][0]));
  dst.m[0][1] = float(a + double(b.m[0][1]));
  dst.m[1][0] = float(a + double(b.m[1][0]));
  dst.m[1][1] = float(a + double(b.m[1][1]));

  return dst;
}


value::matrix2f operator-(const value::matrix2f &a, const double b) {
  value::matrix2f dst;
  dst.m[0][0] = float(double(a.m[0][0]) - b);
  dst.m[0][1] = float(double(a.m[0][1]) - b);
  dst.m[1][0] = float(double(a.m[1][0]) - b);
  dst.m[1][1] = float(double(a.m[1][1]) - b);

  return dst;
}

value::matrix2f operator-(const double a, const value::matrix2f &b) {
  value::matrix2f dst;
  dst.m[0][0] = float(a - double(b.m[0][0]));
  dst.m[0][1] = float(a - double(b.m[0][1]));
  dst.m[1][0] = float(a - double(b.m[1][0]));
  dst.m[1][1] = float(a - double(b.m[1][1]));

  return dst;
}

value::matrix2f operator*(const value::matrix2f &a, const double b) {
  value::matrix2f dst;
  dst.m[0][0] = float(double(a.m[0][0]) * b);
  dst.m[0][1] = float(double(a.m[0][1]) * b);
  dst.m[1][0] = float(double(a.m[1][0]) * b);
  dst.m[1][1] = float(double(a.m[1][1]) * b);

  return dst;
}

value::matrix2f operator*(const double a, const value::matrix2f &b) {
  value::matrix2f dst;
  dst.m[0][0] = float(a * double(b.m[0][0]));
  dst.m[0][1] = float(a * double(b.m[0][1]));
  dst.m[1][0] = float(a * double(b.m[1][0]));
  dst.m[1][1] = float(a * double(b.m[1][1]));

  return dst;
}

value::matrix2f operator/(const value::matrix2f &a, const double b) {
  value::matrix2f dst;
  dst.m[0][0] = float(double(a.m[0][0]) / b);
  dst.m[0][1] = float(double(a.m[0][1]) / b);
  dst.m[1][0] = float(double(a.m[1][0]) / b);
  dst.m[1][1] = float(double(a.m[1][1]) / b);

  return dst;
}

value::matrix2f operator/(const double a, const value::matrix2f &b) {
  value::matrix2f dst;
  dst.m[0][0] = float(a / double(b.m[0][0]));
  dst.m[0][1] = float(a / double(b.m[0][1]));
  dst.m[1][0] = float(a / double(b.m[1][0]));
  dst.m[1][1] = float(a / double(b.m[1][1]));

  return dst;
}

value::matrix3f operator+(const value::matrix3f &a, const double b) {
  value::matrix3f dst;
  dst.m[0][0] = float(double(a.m[0][0]) + b);
  dst.m[0][1] = float(double(a.m[0][1]) + b);
  dst.m[0][2] = float(double(a.m[0][2]) + b);
  dst.m[1][0] = float(double(a.m[1][0]) + b);
  dst.m[1][1] = float(double(a.m[1][1]) + b);
  dst.m[1][2] = float(double(a.m[1][2]) + b);
  dst.m[2][0] = float(double(a.m[2][0]) + b);
  dst.m[2][1] = float(double(a.m[2][1]) + b);
  dst.m[2][2] = float(double(a.m[2][2]) + b);

  return dst;
}

value::matrix3f operator+(const double a, const value::matrix3f &b) {
  value::matrix3f dst;
  dst.m[0][0] = float(a + double(b.m[0][0]));
  dst.m[0][1] = float(a + double(b.m[0][1]));
  dst.m[0][2] = float(a + double(b.m[0][2]));
  dst.m[1][0] = float(a + double(b.m[1][0]));
  dst.m[1][1] = float(a + double(b.m[1][1]));
  dst.m[1][2] = float(a + double(b.m[1][2]));
  dst.m[2][0] = float(a + double(b.m[2][0]));
  dst.m[2][1] = float(a + double(b.m[2][1]));
  dst.m[2][2] = float(a + double(b.m[2][2]));

  return dst;
}


value::matrix3f operator-(const value::matrix3f &a, const double b) {
  value::matrix3f dst;
  dst.m[0][0] = float(double(a.m[0][0] )- b);
  dst.m[0][1] = float(double(a.m[0][1] )- b);
  dst.m[0][2] = float(double(a.m[0][2] )- b);
  dst.m[1][0] = float(double(a.m[1][0] )- b);
  dst.m[1][1] = float(double(a.m[1][1] )- b);
  dst.m[1][2] = float(double(a.m[1][2] )- b);
  dst.m[2][0] = float(double(a.m[2][0] )- b);
  dst.m[2][1] = float(double(a.m[2][1] )- b);
  dst.m[2][2] = float(double(a.m[2][2] )- b);

  return dst;
}

value::matrix3f operator-(const double a, const value::matrix3f &b) {
  value::matrix3f dst;
  dst.m[0][0] = float(a - double(b.m[0][0]));
  dst.m[0][1] = float(a - double(b.m[0][1]));
  dst.m[0][2] = float(a - double(b.m[0][2]));
  dst.m[1][0] = float(a - double(b.m[1][0]));
  dst.m[1][1] = float(a - double(b.m[1][1]));
  dst.m[1][2] = float(a - double(b.m[1][2]));
  dst.m[2][0] = float(a - double(b.m[2][0]));
  dst.m[2][1] = float(a - double(b.m[2][1]));
  dst.m[2][2] = float(a - double(b.m[2][2]));

  return dst;
}

value::matrix3f operator*(const value::matrix3f &a, const double b) {
  value::matrix3f dst;
  dst.m[0][0] = float(double(a.m[0][0]) * b);
  dst.m[0][1] = float(double(a.m[0][1]) * b);
  dst.m[0][2] = float(double(a.m[0][2]) * b);
  dst.m[1][0] = float(double(a.m[1][0]) * b);
  dst.m[1][1] = float(double(a.m[1][1]) * b);
  dst.m[1][2] = float(double(a.m[1][2]) * b);
  dst.m[2][0] = float(double(a.m[2][0]) * b);
  dst.m[2][1] = float(double(a.m[2][1]) * b);
  dst.m[2][2] = float(double(a.m[2][2]) * b);

  return dst;
}

value::matrix3f operator*(const double a, const value::matrix3f &b) {
  value::matrix3f dst;
  dst.m[0][0] = float(a * double(b.m[0][0]));
  dst.m[0][1] = float(a * double(b.m[0][1]));
  dst.m[0][2] = float(a * double(b.m[0][2]));
  dst.m[1][0] = float(a * double(b.m[1][0]));
  dst.m[1][1] = float(a * double(b.m[1][1]));
  dst.m[1][2] = float(a * double(b.m[1][2]));
  dst.m[2][0] = float(a * double(b.m[2][0]));
  dst.m[2][1] = float(a * double(b.m[2][1]));
  dst.m[2][2] = float(a * double(b.m[2][2]));

  return dst;
}

value::matrix3f operator/(const value::matrix3f &a, const double b) {
  value::matrix3f dst;
  dst.m[0][0] = float(double(a.m[0][0]) / b);
  dst.m[0][1] = float(double(a.m[0][1]) / b);
  dst.m[0][2] = float(double(a.m[0][2]) / b);
  dst.m[1][0] = float(double(a.m[1][0]) / b);
  dst.m[1][1] = float(double(a.m[1][1]) / b);
  dst.m[1][2] = float(double(a.m[1][2]) / b);
  dst.m[2][0] = float(double(a.m[2][0]) / b);
  dst.m[2][1] = float(double(a.m[2][1]) / b);
  dst.m[2][2] = float(double(a.m[2][2]) / b);

  return dst;
}

value::matrix3f operator/(const double a, const value::matrix3f &b) {
  value::matrix3f dst;
  dst.m[0][0] = float(a / double(b.m[0][0]));
  dst.m[0][1] = float(a / double(b.m[0][1]));
  dst.m[0][2] = float(a / double(b.m[0][2]));
  dst.m[1][0] = float(a / double(b.m[1][0]));
  dst.m[1][1] = float(a / double(b.m[1][1]));
  dst.m[1][2] = float(a / double(b.m[1][2]));
  dst.m[2][0] = float(a / double(b.m[2][0]));
  dst.m[2][1] = float(a / double(b.m[2][1]));
  dst.m[2][2] = float(a / double(b.m[2][2]));

  return dst;
}

value::matrix4f operator+(const value::matrix4f &a, const double b) {
  value::matrix4f dst;
  dst.m[0][0] = float(double(a.m[0][0]) + b);
  dst.m[0][1] = float(double(a.m[0][1]) + b);
  dst.m[0][2] = float(double(a.m[0][2]) + b);
  dst.m[0][3] = float(double(a.m[0][3]) + b);
  dst.m[1][0] = float(double(a.m[1][0]) + b);
  dst.m[1][1] = float(double(a.m[1][1]) + b);
  dst.m[1][2] = float(double(a.m[1][2]) + b);
  dst.m[1][3] = float(double(a.m[1][3]) + b);
  dst.m[2][0] = float(double(a.m[2][0]) + b);
  dst.m[2][1] = float(double(a.m[2][1]) + b);
  dst.m[2][2] = float(double(a.m[2][2]) + b);
  dst.m[2][3] = float(double(a.m[2][3]) + b);
  dst.m[3][0] = float(double(a.m[3][0]) + b);
  dst.m[3][1] = float(double(a.m[3][1]) + b);
  dst.m[3][2] = float(double(a.m[3][2]) + b);
  dst.m[3][3] = float(double(a.m[3][3]) + b);

  return dst;
}

value::matrix4f operator+(const double a, const value::matrix4f &b) {
  value::matrix4f dst;
  dst.m[0][0] = float(a + double(b.m[0][0]));
  dst.m[0][1] = float(a + double(b.m[0][1]));
  dst.m[0][2] = float(a + double(b.m[0][2]));
  dst.m[0][3] = float(a + double(b.m[0][3]));
  dst.m[1][0] = float(a + double(b.m[1][0]));
  dst.m[1][1] = float(a + double(b.m[1][1]));
  dst.m[1][2] = float(a + double(b.m[1][2]));
  dst.m[1][3] = float(a + double(b.m[1][3]));
  dst.m[2][0] = float(a + double(b.m[2][0]));
  dst.m[2][1] = float(a + double(b.m[2][1]));
  dst.m[2][2] = float(a + double(b.m[2][2]));
  dst.m[2][3] = float(a + double(b.m[2][3]));
  dst.m[3][0] = float(a + double(b.m[3][0]));
  dst.m[3][1] = float(a + double(b.m[3][1]));
  dst.m[3][2] = float(a + double(b.m[3][2]));
  dst.m[3][3] = float(a + double(b.m[3][3]));

  return dst;
}

value::matrix4f operator-(const value::matrix4f &a, const double b) {
  value::matrix4f dst;
  dst.m[0][0] = float(double(a.m[0][0]) - b);
  dst.m[0][1] = float(double(a.m[0][1]) - b);
  dst.m[0][2] = float(double(a.m[0][2]) - b);
  dst.m[0][3] = float(double(a.m[0][3]) - b);
  dst.m[1][0] = float(double(a.m[1][0]) - b);
  dst.m[1][1] = float(double(a.m[1][1]) - b);
  dst.m[1][2] = float(double(a.m[1][2]) - b);
  dst.m[1][3] = float(double(a.m[1][3]) - b);
  dst.m[2][0] = float(double(a.m[2][0]) - b);
  dst.m[2][1] = float(double(a.m[2][1]) - b);
  dst.m[2][2] = float(double(a.m[2][2]) - b);
  dst.m[2][3] = float(double(a.m[2][3]) - b);
  dst.m[3][0] = float(double(a.m[3][0]) - b);
  dst.m[3][1] = float(double(a.m[3][1]) - b);
  dst.m[3][2] = float(double(a.m[3][2]) - b);
  dst.m[3][3] = float(double(a.m[3][3]) - b);

  return dst;
}

value::matrix4f operator-(const double a, const value::matrix4f &b) {
  value::matrix4f dst;
  dst.m[0][0] = float(a - double(b.m[0][0]));
  dst.m[0][1] = float(a - double(b.m[0][1]));
  dst.m[0][2] = float(a - double(b.m[0][2]));
  dst.m[0][3] = float(a - double(b.m[0][3]));
  dst.m[1][0] = float(a - double(b.m[1][0]));
  dst.m[1][1] = float(a - double(b.m[1][1]));
  dst.m[1][2] = float(a - double(b.m[1][2]));
  dst.m[1][3] = float(a - double(b.m[1][3]));
  dst.m[2][0] = float(a - double(b.m[2][0]));
  dst.m[2][1] = float(a - double(b.m[2][1]));
  dst.m[2][2] = float(a - double(b.m[2][2]));
  dst.m[2][3] = float(a - double(b.m[2][3]));
  dst.m[3][0] = float(a - double(b.m[3][0]));
  dst.m[3][1] = float(a - double(b.m[3][1]));
  dst.m[3][2] = float(a - double(b.m[3][2]));
  dst.m[3][3] = float(a - double(b.m[3][3]));

  return dst;
}
value::matrix4f operator*(const value::matrix4f &a, const double b) {
  value::matrix4f dst;
  dst.m[0][0] = float(double(a.m[0][0]) * b);
  dst.m[0][1] = float(double(a.m[0][1]) * b);
  dst.m[0][2] = float(double(a.m[0][2]) * b);
  dst.m[0][3] = float(double(a.m[0][3]) * b);
  dst.m[1][0] = float(double(a.m[1][0]) * b);
  dst.m[1][1] = float(double(a.m[1][1]) * b);
  dst.m[1][2] = float(double(a.m[1][2]) * b);
  dst.m[1][3] = float(double(a.m[1][3]) * b);
  dst.m[2][0] = float(double(a.m[2][0]) * b);
  dst.m[2][1] = float(double(a.m[2][1]) * b);
  dst.m[2][2] = float(double(a.m[2][2]) * b);
  dst.m[2][3] = float(double(a.m[2][3]) * b);
  dst.m[3][0] = float(double(a.m[3][0]) * b);
  dst.m[3][1] = float(double(a.m[3][1]) * b);
  dst.m[3][2] = float(double(a.m[3][2]) * b);
  dst.m[3][3] = float(double(a.m[3][3]) * b);

  return dst;
}

value::matrix4f operator*(const double a, const value::matrix4f &b) {
  value::matrix4f dst;
  dst.m[0][0] = float(a * double(b.m[0][0]));
  dst.m[0][1] = float(a * double(b.m[0][1]));
  dst.m[0][2] = float(a * double(b.m[0][2]));
  dst.m[0][3] = float(a * double(b.m[0][3]));
  dst.m[1][0] = float(a * double(b.m[1][0]));
  dst.m[1][1] = float(a * double(b.m[1][1]));
  dst.m[1][2] = float(a * double(b.m[1][2]));
  dst.m[1][3] = float(a * double(b.m[1][3]));
  dst.m[2][0] = float(a * double(b.m[2][0]));
  dst.m[2][1] = float(a * double(b.m[2][1]));
  dst.m[2][2] = float(a * double(b.m[2][2]));
  dst.m[2][3] = float(a * double(b.m[2][3]));
  dst.m[3][0] = float(a * double(b.m[3][0]));
  dst.m[3][1] = float(a * double(b.m[3][1]));
  dst.m[3][2] = float(a * double(b.m[3][2]));
  dst.m[3][3] = float(a * double(b.m[3][3]));

  return dst;
}

value::matrix4f operator/(const value::matrix4f &a, const double b) {
  value::matrix4f dst;
  dst.m[0][0] = float(double(a.m[0][0]) / b);
  dst.m[0][1] = float(double(a.m[0][1]) / b);
  dst.m[0][2] = float(double(a.m[0][2]) / b);
  dst.m[0][3] = float(double(a.m[0][3]) / b);
  dst.m[1][0] = float(double(a.m[1][0]) / b);
  dst.m[1][1] = float(double(a.m[1][1]) / b);
  dst.m[1][2] = float(double(a.m[1][2]) / b);
  dst.m[1][3] = float(double(a.m[1][3]) / b);
  dst.m[2][0] = float(double(a.m[2][0]) / b);
  dst.m[2][1] = float(double(a.m[2][1]) / b);
  dst.m[2][2] = float(double(a.m[2][2]) / b);
  dst.m[2][3] = float(double(a.m[2][3]) / b);
  dst.m[3][0] = float(double(a.m[3][0]) / b);
  dst.m[3][1] = float(double(a.m[3][1]) / b);
  dst.m[3][2] = float(double(a.m[3][2]) / b);
  dst.m[3][3] = float(double(a.m[3][3]) / b);

  return dst;
}

value::matrix4f operator/(const double a, const value::matrix4f &b) {
  value::matrix4f dst;
  dst.m[0][0] = float(a / double(b.m[0][0]));
  dst.m[0][1] = float(a / double(b.m[0][1]));
  dst.m[0][2] = float(a / double(b.m[0][2]));
  dst.m[0][3] = float(a / double(b.m[0][3]));
  dst.m[1][0] = float(a / double(b.m[1][0]));
  dst.m[1][1] = float(a / double(b.m[1][1]));
  dst.m[1][2] = float(a / double(b.m[1][2]));
  dst.m[1][3] = float(a / double(b.m[1][3]));
  dst.m[2][0] = float(a / double(b.m[2][0]));
  dst.m[2][1] = float(a / double(b.m[2][1]));
  dst.m[2][2] = float(a / double(b.m[2][2]));
  dst.m[2][3] = float(a / double(b.m[2][3]));
  dst.m[3][0] = float(a / double(b.m[3][0]));
  dst.m[3][1] = float(a / double(b.m[3][1]));
  dst.m[3][2] = float(a / double(b.m[3][2]));
  dst.m[3][3] = float(a / double(b.m[3][3]));

  return dst;
}

value::matrix2d operator+(const value::matrix2d &a, const double b) {
  value::matrix2d dst;
  dst.m[0][0] = a.m[0][0] + b;
  dst.m[0][1] = a.m[0][1] + b;
  dst.m[1][0] = a.m[1][0] + b;
  dst.m[1][1] = a.m[1][1] + b;

  return dst;
}

value::matrix2d operator+(const double a, const value::matrix2d &b) {
  value::matrix2d dst;
  dst.m[0][0] = a + b.m[0][0];
  dst.m[0][1] = a + b.m[0][1];
  dst.m[1][0] = a + b.m[1][0];
  dst.m[1][1] = a + b.m[1][1];

  return dst;
}


value::matrix2d operator-(const value::matrix2d &a, const double b) {
  value::matrix2d dst;
  dst.m[0][0] = a.m[0][0] - b;
  dst.m[0][1] = a.m[0][1] - b;
  dst.m[1][0] = a.m[1][0] - b;
  dst.m[1][1] = a.m[1][1] - b;

  return dst;
}

value::matrix2d operator-(const double a, const value::matrix2d &b) {
  value::matrix2d dst;
  dst.m[0][0] = a - b.m[0][0];
  dst.m[0][1] = a - b.m[0][1];
  dst.m[1][0] = a - b.m[1][0];
  dst.m[1][1] = a - b.m[1][1];

  return dst;
}

value::matrix2d operator*(const value::matrix2d &a, const double b) {
  value::matrix2d dst;
  dst.m[0][0] = a.m[0][0] * b;
  dst.m[0][1] = a.m[0][1] * b;
  dst.m[1][0] = a.m[1][0] * b;
  dst.m[1][1] = a.m[1][1] * b;

  return dst;
}

value::matrix2d operator*(const double a, const value::matrix2d &b) {
  value::matrix2d dst;
  dst.m[0][0] = a * b.m[0][0];
  dst.m[0][1] = a * b.m[0][1];
  dst.m[1][0] = a * b.m[1][0];
  dst.m[1][1] = a * b.m[1][1];

  return dst;
}

value::matrix2d operator/(const value::matrix2d &a, const double b) {
  value::matrix2d dst;
  dst.m[0][0] = a.m[0][0] / b;
  dst.m[0][1] = a.m[0][1] / b;
  dst.m[1][0] = a.m[1][0] / b;
  dst.m[1][1] = a.m[1][1] / b;

  return dst;
}

value::matrix2d operator/(const double a, const value::matrix2d &b) {
  value::matrix2d dst;
  dst.m[0][0] = a / b.m[0][0];
  dst.m[0][1] = a / b.m[0][1];
  dst.m[1][0] = a / b.m[1][0];
  dst.m[1][1] = a / b.m[1][1];

  return dst;
}

value::matrix3d operator+(const value::matrix3d &a, const double b) {
  value::matrix3d dst;
  dst.m[0][0] = a.m[0][0] + b;
  dst.m[0][1] = a.m[0][1] + b;
  dst.m[0][2] = a.m[0][2] + b;
  dst.m[1][0] = a.m[1][0] + b;
  dst.m[1][1] = a.m[1][1] + b;
  dst.m[1][2] = a.m[1][2] + b;
  dst.m[2][0] = a.m[2][0] + b;
  dst.m[2][1] = a.m[2][1] + b;
  dst.m[2][2] = a.m[2][2] + b;

  return dst;
}

value::matrix3d operator+(const double a, const value::matrix3d &b) {
  value::matrix3d dst;
  dst.m[0][0] = a + b.m[0][0];
  dst.m[0][1] = a + b.m[0][1];
  dst.m[0][2] = a + b.m[0][2];
  dst.m[1][0] = a + b.m[1][0];
  dst.m[1][1] = a + b.m[1][1];
  dst.m[1][2] = a + b.m[1][2];
  dst.m[2][0] = a + b.m[2][0];
  dst.m[2][1] = a + b.m[2][1];
  dst.m[2][2] = a + b.m[2][2];

  return dst;
}


value::matrix3d operator-(const value::matrix3d &a, const double b) {
  value::matrix3d dst;
  dst.m[0][0] = a.m[0][0] - b;
  dst.m[0][1] = a.m[0][1] - b;
  dst.m[0][2] = a.m[0][2] - b;
  dst.m[1][0] = a.m[1][0] - b;
  dst.m[1][1] = a.m[1][1] - b;
  dst.m[1][2] = a.m[1][2] - b;
  dst.m[2][0] = a.m[2][0] - b;
  dst.m[2][1] = a.m[2][1] - b;
  dst.m[2][2] = a.m[2][2] - b;

  return dst;
}

value::matrix3d operator-(const double a, const value::matrix3d &b) {
  value::matrix3d dst;
  dst.m[0][0] = a - b.m[0][0];
  dst.m[0][1] = a - b.m[0][1];
  dst.m[0][2] = a - b.m[0][2];
  dst.m[1][0] = a - b.m[1][0];
  dst.m[1][1] = a - b.m[1][1];
  dst.m[1][2] = a - b.m[1][2];
  dst.m[2][0] = a - b.m[2][0];
  dst.m[2][1] = a - b.m[2][1];
  dst.m[2][2] = a - b.m[2][2];

  return dst;
}

value::matrix3d operator*(const value::matrix3d &a, const double b) {
  value::matrix3d dst;
  dst.m[0][0] = a.m[0][0] * b;
  dst.m[0][1] = a.m[0][1] * b;
  dst.m[0][2] = a.m[0][2] * b;
  dst.m[1][0] = a.m[1][0] * b;
  dst.m[1][1] = a.m[1][1] * b;
  dst.m[1][2] = a.m[1][2] * b;
  dst.m[2][0] = a.m[2][0] * b;
  dst.m[2][1] = a.m[2][1] * b;
  dst.m[2][2] = a.m[2][2] * b;

  return dst;
}

value::matrix3d operator*(const double a, const value::matrix3d &b) {
  value::matrix3d dst;
  dst.m[0][0] = a * b.m[0][0];
  dst.m[0][1] = a * b.m[0][1];
  dst.m[0][2] = a * b.m[0][2];
  dst.m[1][0] = a * b.m[1][0];
  dst.m[1][1] = a * b.m[1][1];
  dst.m[1][2] = a * b.m[1][2];
  dst.m[2][0] = a * b.m[2][0];
  dst.m[2][1] = a * b.m[2][1];
  dst.m[2][2] = a * b.m[2][2];

  return dst;
}

value::matrix3d operator/(const value::matrix3d &a, const double b) {
  value::matrix3d dst;
  dst.m[0][0] = a.m[0][0] / b;
  dst.m[0][1] = a.m[0][1] / b;
  dst.m[0][2] = a.m[0][2] / b;
  dst.m[1][0] = a.m[1][0] / b;
  dst.m[1][1] = a.m[1][1] / b;
  dst.m[1][2] = a.m[1][2] / b;
  dst.m[2][0] = a.m[2][0] / b;
  dst.m[2][1] = a.m[2][1] / b;
  dst.m[2][2] = a.m[2][2] / b;

  return dst;
}

value::matrix3d operator/(const double a, const value::matrix3d &b) {
  value::matrix3d dst;
  dst.m[0][0] = a / b.m[0][0];
  dst.m[0][1] = a / b.m[0][1];
  dst.m[0][2] = a / b.m[0][2];
  dst.m[1][0] = a / b.m[1][0];
  dst.m[1][1] = a / b.m[1][1];
  dst.m[1][2] = a / b.m[1][2];
  dst.m[2][0] = a / b.m[2][0];
  dst.m[2][1] = a / b.m[2][1];
  dst.m[2][2] = a / b.m[2][2];

  return dst;
}

value::matrix4d operator+(const value::matrix4d &a, const double b) {
  value::matrix4d dst;
  dst.m[0][0] = a.m[0][0] + b;
  dst.m[0][1] = a.m[0][1] + b;
  dst.m[0][2] = a.m[0][2] + b;
  dst.m[0][3] = a.m[0][3] + b;
  dst.m[1][0] = a.m[1][0] + b;
  dst.m[1][1] = a.m[1][1] + b;
  dst.m[1][2] = a.m[1][2] + b;
  dst.m[1][3] = a.m[1][3] + b;
  dst.m[2][0] = a.m[2][0] + b;
  dst.m[2][1] = a.m[2][1] + b;
  dst.m[2][2] = a.m[2][2] + b;
  dst.m[2][3] = a.m[2][3] + b;
  dst.m[3][0] = a.m[3][0] + b;
  dst.m[3][1] = a.m[3][1] + b;
  dst.m[3][2] = a.m[3][2] + b;
  dst.m[3][3] = a.m[3][3] + b;

  return dst;
}

value::matrix4d operator+(const double a, const value::matrix4d &b) {
  value::matrix4d dst;
  dst.m[0][0] = a + b.m[0][0];
  dst.m[0][1] = a + b.m[0][1];
  dst.m[0][2] = a + b.m[0][2];
  dst.m[0][3] = a + b.m[0][3];
  dst.m[1][0] = a + b.m[1][0];
  dst.m[1][1] = a + b.m[1][1];
  dst.m[1][2] = a + b.m[1][2];
  dst.m[1][3] = a + b.m[1][3];
  dst.m[2][0] = a + b.m[2][0];
  dst.m[2][1] = a + b.m[2][1];
  dst.m[2][2] = a + b.m[2][2];
  dst.m[2][3] = a + b.m[2][3];
  dst.m[3][0] = a + b.m[3][0];
  dst.m[3][1] = a + b.m[3][1];
  dst.m[3][2] = a + b.m[3][2];
  dst.m[3][3] = a + b.m[3][3];

  return dst;
}

value::matrix4d operator-(const value::matrix4d &a, const double b) {
  value::matrix4d dst;
  dst.m[0][0] = a.m[0][0] - b;
  dst.m[0][1] = a.m[0][1] - b;
  dst.m[0][2] = a.m[0][2] - b;
  dst.m[0][3] = a.m[0][3] - b;
  dst.m[1][0] = a.m[1][0] - b;
  dst.m[1][1] = a.m[1][1] - b;
  dst.m[1][2] = a.m[1][2] - b;
  dst.m[1][3] = a.m[1][3] - b;
  dst.m[2][0] = a.m[2][0] - b;
  dst.m[2][1] = a.m[2][1] - b;
  dst.m[2][2] = a.m[2][2] - b;
  dst.m[2][3] = a.m[2][3] - b;
  dst.m[3][0] = a.m[3][0] - b;
  dst.m[3][1] = a.m[3][1] - b;
  dst.m[3][2] = a.m[3][2] - b;
  dst.m[3][3] = a.m[3][3] - b;

  return dst;
}

value::matrix4d operator-(const double a, const value::matrix4d &b) {
  value::matrix4d dst;
  dst.m[0][0] = a - b.m[0][0];
  dst.m[0][1] = a - b.m[0][1];
  dst.m[0][2] = a - b.m[0][2];
  dst.m[0][3] = a - b.m[0][3];
  dst.m[1][0] = a - b.m[1][0];
  dst.m[1][1] = a - b.m[1][1];
  dst.m[1][2] = a - b.m[1][2];
  dst.m[1][3] = a - b.m[1][3];
  dst.m[2][0] = a - b.m[2][0];
  dst.m[2][1] = a - b.m[2][1];
  dst.m[2][2] = a - b.m[2][2];
  dst.m[2][3] = a - b.m[2][3];
  dst.m[3][0] = a - b.m[3][0];
  dst.m[3][1] = a - b.m[3][1];
  dst.m[3][2] = a - b.m[3][2];
  dst.m[3][3] = a - b.m[3][3];

  return dst;
}
value::matrix4d operator*(const value::matrix4d &a, const double b) {
  value::matrix4d dst;
  dst.m[0][0] = a.m[0][0] * b;
  dst.m[0][1] = a.m[0][1] * b;
  dst.m[0][2] = a.m[0][2] * b;
  dst.m[0][3] = a.m[0][3] * b;
  dst.m[1][0] = a.m[1][0] * b;
  dst.m[1][1] = a.m[1][1] * b;
  dst.m[1][2] = a.m[1][2] * b;
  dst.m[1][3] = a.m[1][3] * b;
  dst.m[2][0] = a.m[2][0] * b;
  dst.m[2][1] = a.m[2][1] * b;
  dst.m[2][2] = a.m[2][2] * b;
  dst.m[2][3] = a.m[2][3] * b;
  dst.m[3][0] = a.m[3][0] * b;
  dst.m[3][1] = a.m[3][1] * b;
  dst.m[3][2] = a.m[3][2] * b;
  dst.m[3][3] = a.m[3][3] * b;

  return dst;
}

value::matrix4d operator*(const double a, const value::matrix4d &b) {
  value::matrix4d dst;
  dst.m[0][0] = a * b.m[0][0];
  dst.m[0][1] = a * b.m[0][1];
  dst.m[0][2] = a * b.m[0][2];
  dst.m[0][3] = a * b.m[0][3];
  dst.m[1][0] = a * b.m[1][0];
  dst.m[1][1] = a * b.m[1][1];
  dst.m[1][2] = a * b.m[1][2];
  dst.m[1][3] = a * b.m[1][3];
  dst.m[2][0] = a * b.m[2][0];
  dst.m[2][1] = a * b.m[2][1];
  dst.m[2][2] = a * b.m[2][2];
  dst.m[2][3] = a * b.m[2][3];
  dst.m[3][0] = a * b.m[3][0];
  dst.m[3][1] = a * b.m[3][1];
  dst.m[3][2] = a * b.m[3][2];
  dst.m[3][3] = a * b.m[3][3];

  return dst;
}

value::matrix4d operator/(const value::matrix4d &a, const double b) {
  value::matrix4d dst;
  dst.m[0][0] = a.m[0][0] / b;
  dst.m[0][1] = a.m[0][1] / b;
  dst.m[0][2] = a.m[0][2] / b;
  dst.m[0][3] = a.m[0][3] / b;
  dst.m[1][0] = a.m[1][0] / b;
  dst.m[1][1] = a.m[1][1] / b;
  dst.m[1][2] = a.m[1][2] / b;
  dst.m[1][3] = a.m[1][3] / b;
  dst.m[2][0] = a.m[2][0] / b;
  dst.m[2][1] = a.m[2][1] / b;
  dst.m[2][2] = a.m[2][2] / b;
  dst.m[2][3] = a.m[2][3] / b;
  dst.m[3][0] = a.m[3][0] / b;
  dst.m[3][1] = a.m[3][1] / b;
  dst.m[3][2] = a.m[3][2] / b;
  dst.m[3][3] = a.m[3][3] / b;

  return dst;
}

value::matrix4d operator/(const double a, const value::matrix4d &b) {
  value::matrix4d dst;
  dst.m[0][0] = a / b.m[0][0];
  dst.m[0][1] = a / b.m[0][1];
  dst.m[0][2] = a / b.m[0][2];
  dst.m[0][3] = a / b.m[0][3];
  dst.m[1][0] = a / b.m[1][0];
  dst.m[1][1] = a / b.m[1][1];
  dst.m[1][2] = a / b.m[1][2];
  dst.m[1][3] = a / b.m[1][3];
  dst.m[2][0] = a / b.m[2][0];
  dst.m[2][1] = a / b.m[2][1];
  dst.m[2][2] = a / b.m[2][2];
  dst.m[2][3] = a / b.m[2][3];
  dst.m[3][0] = a / b.m[3][0];
  dst.m[3][1] = a / b.m[3][1];
  dst.m[3][2] = a / b.m[3][2];
  dst.m[3][3] = a / b.m[3][3];

  return dst;
}

value::frame4d operator+(const value::frame4d &a, const value::frame4d &b) {
  value::frame4d dst;
  dst.m[0][0] = a.m[0][0] + b.m[0][0];
  dst.m[0][1] = a.m[0][1] + b.m[0][1];
  dst.m[0][2] = a.m[0][2] + b.m[0][2];
  dst.m[0][3] = a.m[0][3] + b.m[0][3];
  dst.m[1][0] = a.m[1][0] + b.m[1][0];
  dst.m[1][1] = a.m[1][1] + b.m[1][1];
  dst.m[1][2] = a.m[1][2] + b.m[1][2];
  dst.m[1][3] = a.m[1][3] + b.m[1][3];
  dst.m[2][0] = a.m[2][0] + b.m[2][0];
  dst.m[2][1] = a.m[2][1] + b.m[2][1];
  dst.m[2][2] = a.m[2][2] + b.m[2][2];
  dst.m[2][3] = a.m[2][3] + b.m[2][3];
  dst.m[3][0] = a.m[3][0] + b.m[3][0];
  dst.m[3][1] = a.m[3][1] + b.m[3][1];
  dst.m[3][2] = a.m[3][2] + b.m[3][2];
  dst.m[3][3] = a.m[3][3] + b.m[3][3];

  return dst;
}

value::frame4d operator+(const value::frame4d &a, const double b) {
  value::frame4d dst;
  dst.m[0][0] = a.m[0][0] + b;
  dst.m[0][1] = a.m[0][1] + b;
  dst.m[0][2] = a.m[0][2] + b;
  dst.m[0][3] = a.m[0][3] + b;
  dst.m[1][0] = a.m[1][0] + b;
  dst.m[1][1] = a.m[1][1] + b;
  dst.m[1][2] = a.m[1][2] + b;
  dst.m[1][3] = a.m[1][3] + b;
  dst.m[2][0] = a.m[2][0] + b;
  dst.m[2][1] = a.m[2][1] + b;
  dst.m[2][2] = a.m[2][2] + b;
  dst.m[2][3] = a.m[2][3] + b;
  dst.m[3][0] = a.m[3][0] + b;
  dst.m[3][1] = a.m[3][1] + b;
  dst.m[3][2] = a.m[3][2] + b;
  dst.m[3][3] = a.m[3][3] + b;

  return dst;
}

value::frame4d operator+(const double a, const value::frame4d &b) {
  value::frame4d dst;
  dst.m[0][0] = a + b.m[0][0];
  dst.m[0][1] = a + b.m[0][1];
  dst.m[0][2] = a + b.m[0][2];
  dst.m[0][3] = a + b.m[0][3];
  dst.m[1][0] = a + b.m[1][0];
  dst.m[1][1] = a + b.m[1][1];
  dst.m[1][2] = a + b.m[1][2];
  dst.m[1][3] = a + b.m[1][3];
  dst.m[2][0] = a + b.m[2][0];
  dst.m[2][1] = a + b.m[2][1];
  dst.m[2][2] = a + b.m[2][2];
  dst.m[2][3] = a + b.m[2][3];
  dst.m[3][0] = a + b.m[3][0];
  dst.m[3][1] = a + b.m[3][1];
  dst.m[3][2] = a + b.m[3][2];
  dst.m[3][3] = a + b.m[3][3];

  return dst;
}

value::frame4d operator-(const value::frame4d &a, const double b) {
  value::frame4d dst;
  dst.m[0][0] = a.m[0][0] - b;
  dst.m[0][1] = a.m[0][1] - b;
  dst.m[0][2] = a.m[0][2] - b;
  dst.m[0][3] = a.m[0][3] - b;
  dst.m[1][0] = a.m[1][0] - b;
  dst.m[1][1] = a.m[1][1] - b;
  dst.m[1][2] = a.m[1][2] - b;
  dst.m[1][3] = a.m[1][3] - b;
  dst.m[2][0] = a.m[2][0] - b;
  dst.m[2][1] = a.m[2][1] - b;
  dst.m[2][2] = a.m[2][2] - b;
  dst.m[2][3] = a.m[2][3] - b;
  dst.m[3][0] = a.m[3][0] - b;
  dst.m[3][1] = a.m[3][1] - b;
  dst.m[3][2] = a.m[3][2] - b;
  dst.m[3][3] = a.m[3][3] - b;

  return dst;
}

value::frame4d operator-(const double a, const value::frame4d &b) {
  value::frame4d dst;
  dst.m[0][0] = a - b.m[0][0];
  dst.m[0][1] = a - b.m[0][1];
  dst.m[0][2] = a - b.m[0][2];
  dst.m[0][3] = a - b.m[0][3];
  dst.m[1][0] = a - b.m[1][0];
  dst.m[1][1] = a - b.m[1][1];
  dst.m[1][2] = a - b.m[1][2];
  dst.m[1][3] = a - b.m[1][3];
  dst.m[2][0] = a - b.m[2][0];
  dst.m[2][1] = a - b.m[2][1];
  dst.m[2][2] = a - b.m[2][2];
  dst.m[2][3] = a - b.m[2][3];
  dst.m[3][0] = a - b.m[3][0];
  dst.m[3][1] = a - b.m[3][1];
  dst.m[3][2] = a - b.m[3][2];
  dst.m[3][3] = a - b.m[3][3];

  return dst;
}
value::frame4d operator*(const value::frame4d &a, const double b) {
  value::frame4d dst;
  dst.m[0][0] = a.m[0][0] * b;
  dst.m[0][1] = a.m[0][1] * b;
  dst.m[0][2] = a.m[0][2] * b;
  dst.m[0][3] = a.m[0][3] * b;
  dst.m[1][0] = a.m[1][0] * b;
  dst.m[1][1] = a.m[1][1] * b;
  dst.m[1][2] = a.m[1][2] * b;
  dst.m[1][3] = a.m[1][3] * b;
  dst.m[2][0] = a.m[2][0] * b;
  dst.m[2][1] = a.m[2][1] * b;
  dst.m[2][2] = a.m[2][2] * b;
  dst.m[2][3] = a.m[2][3] * b;
  dst.m[3][0] = a.m[3][0] * b;
  dst.m[3][1] = a.m[3][1] * b;
  dst.m[3][2] = a.m[3][2] * b;
  dst.m[3][3] = a.m[3][3] * b;

  return dst;
}

value::frame4d operator*(const double a, const value::frame4d &b) {
  value::frame4d dst;
  dst.m[0][0] = a * b.m[0][0];
  dst.m[0][1] = a * b.m[0][1];
  dst.m[0][2] = a * b.m[0][2];
  dst.m[0][3] = a * b.m[0][3];
  dst.m[1][0] = a * b.m[1][0];
  dst.m[1][1] = a * b.m[1][1];
  dst.m[1][2] = a * b.m[1][2];
  dst.m[1][3] = a * b.m[1][3];
  dst.m[2][0] = a * b.m[2][0];
  dst.m[2][1] = a * b.m[2][1];
  dst.m[2][2] = a * b.m[2][2];
  dst.m[2][3] = a * b.m[2][3];
  dst.m[3][0] = a * b.m[3][0];
  dst.m[3][1] = a * b.m[3][1];
  dst.m[3][2] = a * b.m[3][2];
  dst.m[3][3] = a * b.m[3][3];

  return dst;
}

value::frame4d operator/(const value::frame4d &a, const double b) {
  value::frame4d dst;
  dst.m[0][0] = a.m[0][0] / b;
  dst.m[0][1] = a.m[0][1] / b;
  dst.m[0][2] = a.m[0][2] / b;
  dst.m[0][3] = a.m[0][3] / b;
  dst.m[1][0] = a.m[1][0] / b;
  dst.m[1][1] = a.m[1][1] / b;
  dst.m[1][2] = a.m[1][2] / b;
  dst.m[1][3] = a.m[1][3] / b;
  dst.m[2][0] = a.m[2][0] / b;
  dst.m[2][1] = a.m[2][1] / b;
  dst.m[2][2] = a.m[2][2] / b;
  dst.m[2][3] = a.m[2][3] / b;
  dst.m[3][0] = a.m[3][0] / b;
  dst.m[3][1] = a.m[3][1] / b;
  dst.m[3][2] = a.m[3][2] / b;
  dst.m[3][3] = a.m[3][3] / b;

  return dst;
}

value::frame4d operator/(const double a, const value::frame4d &b) {
  value::frame4d dst;
  dst.m[0][0] = a / b.m[0][0];
  dst.m[0][1] = a / b.m[0][1];
  dst.m[0][2] = a / b.m[0][2];
  dst.m[0][3] = a / b.m[0][3];
  dst.m[1][0] = a / b.m[1][0];
  dst.m[1][1] = a / b.m[1][1];
  dst.m[1][2] = a / b.m[1][2];
  dst.m[1][3] = a / b.m[1][3];
  dst.m[2][0] = a / b.m[2][0];
  dst.m[2][1] = a / b.m[2][1];
  dst.m[2][2] = a / b.m[2][2];
  dst.m[2][3] = a / b.m[2][3];
  dst.m[3][0] = a / b.m[3][0];
  dst.m[3][1] = a / b.m[3][1];
  dst.m[3][2] = a / b.m[3][2];
  dst.m[3][3] = a / b.m[3][3];

  return dst;
}

} // namespace lightusd
