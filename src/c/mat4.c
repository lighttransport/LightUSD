/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * mat4.c -- C11 double 4x4 transform math and reduced-argument pi trig.
 *
 * The leaf routines of src/next/eval/xform-eval.cc. They must stay
 * bit-identical to the legacy evaluator (lightusd::Xformable), so the
 * reduced-argument sin_pi/cos_pi (NOT sin/cos of deg*pi/180) and the
 * row-vector multiply order are reproduced verbatim. Pure double[16] math: no
 * allocation, no C++ types, reusable from C.
 */

#include "lightusd-core-c.h"

#include <math.h>
#include <string.h>

static const double kPi = 3.141592653589793238462643383279502884e+00;

static int is_close_d(double a, double b, double eps) {
  const double d = a - b;
  if (fabs(d) <= eps) return 1;
  return fabs(d) <= (eps * fmax(fabs(a), fabs(b)));
}

void lu_mat4_identity(double *m) {
  memset(m, 0, 16 * sizeof(double));
  m[0] = m[5] = m[10] = m[15] = 1.0;
}

void lu_mat4_mul(double *result, const double *a, const double *b) {
  double temp[16];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      temp[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] +
                        a[i * 4 + 1] * b[1 * 4 + j] +
                        a[i * 4 + 2] * b[2 * 4 + j] +
                        a[i * 4 + 3] * b[3 * 4 + j];
    }
  }
  memcpy(result, temp, 16 * sizeof(double));
}

/* cos(pi*x) -- verbatim port of lightusd::math::cos_pi_imp<double>. */
double lu_cos_pi(double x) {
  int invert = 0;
  if (fabs(x) < 0.25) return cos(kPi * x);
  if (x < 0) x = -x;
  double rem = floor(x);
  {
    const double r = trunc(rem);
    const int ival = (int)r;
    if (ival & 1) invert = !invert;
  }
  rem = x - rem;
  if (rem > 0.5) {
    rem = 1 - rem;
    invert = !invert;
  }
  if (is_close_d(rem, 0.5, 0.0)) return 0.0;
  if (rem > 0.25) {
    rem = 0.5 - rem;
    rem = sin(kPi * rem);
  } else {
    rem = cos(kPi * rem);
  }
  return invert ? -rem : rem;
}

/* sin(pi*x) -- verbatim port of lightusd::math::sin_pi_imp<double>. */
double lu_sin_pi(double x) {
  if (x < 0) return -lu_sin_pi(-x);
  int invert = 0;
  if (x < 0.5) {
    if (is_close_d(x, 0.25, 0.0)) return cos(kPi * x);
    return sin(kPi * x);
  }
  if (x < 1) {
    invert = 1;
    x = -x;
  } else {
    invert = 0;
  }
  double rem = floor(x);
  {
    const double r = trunc(rem);
    const int ival = (int)r;
    if (ival & 1) invert = !invert;
  }
  rem = x - rem;
  if (rem > 0.5) rem = 1 - rem;
  if (is_close_d(rem, 0.5, 0.0)) return invert ? -1.0 : 1.0;
  if (is_close_d(rem, 0.25, 0.0)) {
    rem = cos(kPi * rem);
  } else {
    rem = sin(kPi * rem);
  }
  return invert ? -rem : rem;
}

void lu_mat4_translation(double *m, double x, double y, double z) {
  lu_mat4_identity(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
}

void lu_mat4_scale(double *m, double x, double y, double z) {
  lu_mat4_identity(m);
  m[0] = x;
  m[5] = y;
  m[10] = z;
}

void lu_mat4_rot_x(double *m, double deg) {
  lu_mat4_identity(m);
  const double c = lu_cos_pi(deg / 180.0), s = lu_sin_pi(deg / 180.0);
  m[5] = c;
  m[6] = s;
  m[9] = -s;
  m[10] = c;
}

void lu_mat4_rot_y(double *m, double deg) {
  lu_mat4_identity(m);
  const double c = lu_cos_pi(deg / 180.0), s = lu_sin_pi(deg / 180.0);
  m[0] = c;
  m[2] = -s;
  m[8] = s;
  m[10] = c;
}

void lu_mat4_rot_z(double *m, double deg) {
  lu_mat4_identity(m);
  const double c = lu_cos_pi(deg / 180.0), s = lu_sin_pi(deg / 180.0);
  m[0] = c;
  m[1] = s;
  m[4] = -s;
  m[5] = c;
}

void lu_mat4_rot_axis(double *m, char axis, double deg) {
  switch (axis) {
    case 'X': lu_mat4_rot_x(m, deg); break;
    case 'Y': lu_mat4_rot_y(m, deg); break;
    case 'Z': lu_mat4_rot_z(m, deg); break;
    default: lu_mat4_identity(m); break;
  }
}

void lu_mat4_orient(double *m, double w, double x, double y, double z) {
  lu_mat4_identity(m);
  m[0] = 1.0 - 2.0 * (y * y + z * z);
  m[1] = 2.0 * (x * y + z * w);
  m[2] = 2.0 * (x * z - y * w);
  m[4] = 2.0 * (x * y - z * w);
  m[5] = 1.0 - 2.0 * (x * x + z * z);
  m[6] = 2.0 * (y * z + x * w);
  m[8] = 2.0 * (x * z + y * w);
  m[9] = 2.0 * (y * z - x * w);
  m[10] = 1.0 - 2.0 * (x * x + y * y);
}

void lu_mat4_invert(const double *a, double *out) {
  double inv[16];
  inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
  inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
  inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
  inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
  inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
  inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
  inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
  inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
  inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
  inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
  inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
  inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
  inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
  inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
  inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
  inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
  const double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
  if (det == 0.0) {
    lu_mat4_identity(out);
    return;
  }
  const double idet = 1.0 / det;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] * idet;
}
