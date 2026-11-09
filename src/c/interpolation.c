/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * interpolation.c -- C11 time-sample interpolation primitives.
 *
 * The Value-free numeric core of src/next/types/interpolation.cc: the
 * "is this type linearly interpolatable" predicate (over lu_type_id, which
 * mirrors next::TypeId) and the scalar/vector lerp and quaternion slerp
 * kernels. The Value-typed InterpolateValues / bracket search stay C++.
 */

#include "lightusd-core-c.h"

#include <math.h>

int lu_type_is_linear_interpolatable(uint16_t id) {
  switch (id) {
    case LU_TYPE_HALF:
    case LU_TYPE_FLOAT:
    case LU_TYPE_DOUBLE:
    case LU_TYPE_TIMECODE:
    case LU_TYPE_MATRIX2F:
    case LU_TYPE_MATRIX3F:
    case LU_TYPE_MATRIX4F:
    case LU_TYPE_MATRIX2D:
    case LU_TYPE_MATRIX3D:
    case LU_TYPE_MATRIX4D:
    case LU_TYPE_FRAME4D:
    case LU_TYPE_HALF2:
    case LU_TYPE_FLOAT2:
    case LU_TYPE_DOUBLE2:
    case LU_TYPE_HALF3:
    case LU_TYPE_FLOAT3:
    case LU_TYPE_DOUBLE3:
    case LU_TYPE_HALF4:
    case LU_TYPE_FLOAT4:
    case LU_TYPE_DOUBLE4:
    case LU_TYPE_QUATH:
    case LU_TYPE_QUATF:
    case LU_TYPE_QUATD:
    case LU_TYPE_POINT3H:
    case LU_TYPE_POINT3F:
    case LU_TYPE_POINT3D:
    case LU_TYPE_VECTOR3H:
    case LU_TYPE_VECTOR3F:
    case LU_TYPE_VECTOR3D:
    case LU_TYPE_NORMAL3H:
    case LU_TYPE_NORMAL3F:
    case LU_TYPE_NORMAL3D:
    case LU_TYPE_COLOR3H:
    case LU_TYPE_COLOR3F:
    case LU_TYPE_COLOR3D:
    case LU_TYPE_COLOR4H:
    case LU_TYPE_COLOR4F:
    case LU_TYPE_COLOR4D:
    case LU_TYPE_TEXCOORD2H:
    case LU_TYPE_TEXCOORD2F:
    case LU_TYPE_TEXCOORD2D:
    case LU_TYPE_TEXCOORD3H:
    case LU_TYPE_TEXCOORD3F:
    case LU_TYPE_TEXCOORD3D:
      return 1;
    default:
      return 0;
  }
}

float lu_lerp_f(float a, float b, float t) { return a + t * (b - a); }

double lu_lerp_d(double a, double b, double t) { return a + t * (b - a); }

void lu_lerp_fn(float *out, const float *a, const float *b, float t, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = a[i] + t * (b[i] - a[i]);
}

void lu_lerp_dn(double *out, const double *a, const double *b, double t,
                size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = a[i] + t * (b[i] - a[i]);
}

/* Quaternion format: x, y, z, w. */
void lu_slerp_quatf(float *out, const float *a, const float *b, float t) {
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  float b_sign = 1.0f;
  if (dot < 0.0f) {
    dot = -dot;
    b_sign = -1.0f;
  }
  float s0, s1;
  if (dot > 0.9995f) {
    s0 = 1.0f - t;
    s1 = t * b_sign;
  } else {
    const float theta = acosf(dot);
    const float sin_theta = sinf(theta);
    s0 = sinf((1.0f - t) * theta) / sin_theta;
    s1 = sinf(t * theta) / sin_theta * b_sign;
  }
  out[0] = s0 * a[0] + s1 * b[0];
  out[1] = s0 * a[1] + s1 * b[1];
  out[2] = s0 * a[2] + s1 * b[2];
  out[3] = s0 * a[3] + s1 * b[3];
  const float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] +
                          out[3] * out[3]);
  if (len > 0.0f) {
    const float inv_len = 1.0f / len;
    out[0] *= inv_len;
    out[1] *= inv_len;
    out[2] *= inv_len;
    out[3] *= inv_len;
  }
}

void lu_slerp_quatd(double *out, const double *a, const double *b, double t) {
  double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  double b_sign = 1.0;
  if (dot < 0.0) {
    dot = -dot;
    b_sign = -1.0;
  }
  double s0, s1;
  if (dot > 0.9995) {
    s0 = 1.0 - t;
    s1 = t * b_sign;
  } else {
    const double theta = acos(dot);
    const double sin_theta = sin(theta);
    s0 = sin((1.0 - t) * theta) / sin_theta;
    s1 = sin(t * theta) / sin_theta * b_sign;
  }
  out[0] = s0 * a[0] + s1 * b[0];
  out[1] = s0 * a[1] + s1 * b[1];
  out[2] = s0 * a[2] + s1 * b[2];
  out[3] = s0 * a[3] + s1 * b[3];
  const double len = sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] +
                          out[3] * out[3]);
  if (len > 0.0) {
    const double inv_len = 1.0 / len;
    out[0] *= inv_len;
    out[1] *= inv_len;
    out[2] *= inv_len;
    out[3] *= inv_len;
  }
}
