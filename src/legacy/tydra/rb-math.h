// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// rb-math.h -- Pure C header with shared physics math types and functions.
//

#ifndef LIGHTUSD_TYDRA_RB_MATH_H_
#define LIGHTUSD_TYDRA_RB_MATH_H_

#include <math.h>
#include <string.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

#define TP_PI        3.14159265358979323846f
#define TP_EPSILON   1e-7f
#define TP_UNLIMITED 1e18f

// ============================================================================
// Types
// ============================================================================

typedef struct TydraPhysVec3 {
  float x, y, z;
} TydraPhysVec3;

typedef struct TydraPhysQuat {
  float x, y, z, w; /* w = real (Hamilton) */
} TydraPhysQuat;

typedef struct TydraPhysMat4 {
  float m[16]; /* row-major; translation at m[3], m[7], m[11] */
} TydraPhysMat4;

typedef struct TydraPhysMat3 {
  float m[9]; /* row-major 3x3 (e.g. inertia tensors) */
} TydraPhysMat3;

typedef struct TydraPhysTransform {
  TydraPhysVec3 position;
  TydraPhysQuat rotation;
} TydraPhysTransform;

// ============================================================================
// Utility
// ============================================================================

static inline float tp_clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline float tp_maxf(float a, float b) { return a > b ? a : b; }
static inline float tp_minf(float a, float b) { return a < b ? a : b; }
static inline float tp_absf(float v) { return v < 0.0f ? -v : v; }

// ============================================================================
// Vec3
// ============================================================================

static inline TydraPhysVec3 tp_v3(float x, float y, float z) {
  TydraPhysVec3 v; v.x = x; v.y = y; v.z = z; return v;
}

static inline TydraPhysVec3 tp_v3_add(TydraPhysVec3 a, TydraPhysVec3 b) {
  return tp_v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static inline TydraPhysVec3 tp_v3_sub(TydraPhysVec3 a, TydraPhysVec3 b) {
  return tp_v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static inline TydraPhysVec3 tp_v3_scale(TydraPhysVec3 v, float s) {
  return tp_v3(v.x * s, v.y * s, v.z * s);
}

static inline float tp_v3_dot(TydraPhysVec3 a, TydraPhysVec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline TydraPhysVec3 tp_v3_cross(TydraPhysVec3 a, TydraPhysVec3 b) {
  return tp_v3(a.y * b.z - a.z * b.y,
               a.z * b.x - a.x * b.z,
               a.x * b.y - a.y * b.x);
}

static inline float tp_v3_length_sq(TydraPhysVec3 v) {
  return tp_v3_dot(v, v);
}

static inline float tp_v3_length(TydraPhysVec3 v) {
  return sqrtf(tp_v3_length_sq(v));
}

static inline TydraPhysVec3 tp_v3_normalize(TydraPhysVec3 v) {
  float len = tp_v3_length(v);
  if (len < TP_EPSILON) return tp_v3(0, 0, 0);
  float inv = 1.0f / len;
  return tp_v3(v.x * inv, v.y * inv, v.z * inv);
}

static inline TydraPhysVec3 tp_v3_negate(TydraPhysVec3 v) {
  return tp_v3(-v.x, -v.y, -v.z);
}

static inline TydraPhysVec3 tp_v3_lerp(TydraPhysVec3 a, TydraPhysVec3 b,
                                        float t) {
  return tp_v3(a.x + (b.x - a.x) * t,
               a.y + (b.y - a.y) * t,
               a.z + (b.z - a.z) * t);
}

static inline TydraPhysVec3 tp_v3_min(TydraPhysVec3 a, TydraPhysVec3 b) {
  return tp_v3(tp_minf(a.x, b.x), tp_minf(a.y, b.y), tp_minf(a.z, b.z));
}

static inline TydraPhysVec3 tp_v3_max(TydraPhysVec3 a, TydraPhysVec3 b) {
  return tp_v3(tp_maxf(a.x, b.x), tp_maxf(a.y, b.y), tp_maxf(a.z, b.z));
}

static inline TydraPhysVec3 tp_v3_abs(TydraPhysVec3 v) {
  return tp_v3(tp_absf(v.x), tp_absf(v.y), tp_absf(v.z));
}

// ============================================================================
// Quat
// ============================================================================

static inline TydraPhysQuat tp_q(float x, float y, float z, float w) {
  TydraPhysQuat q; q.x = x; q.y = y; q.z = z; q.w = w; return q;
}

static inline TydraPhysQuat tp_q_identity(void) {
  return tp_q(0, 0, 0, 1);
}

static inline TydraPhysQuat tp_q_mul(TydraPhysQuat a, TydraPhysQuat b) {
  return tp_q(
    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

static inline TydraPhysQuat tp_q_conjugate(TydraPhysQuat q) {
  return tp_q(-q.x, -q.y, -q.z, q.w);
}

static inline TydraPhysQuat tp_q_normalize(TydraPhysQuat q) {
  float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len < TP_EPSILON) return tp_q_identity();
  float inv = 1.0f / len;
  return tp_q(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
}

static inline TydraPhysQuat tp_q_from_axis_angle(TydraPhysVec3 axis,
                                                   float angle) {
  float half = angle * 0.5f;
  float s = sinf(half);
  return tp_q(axis.x * s, axis.y * s, axis.z * s, cosf(half));
}

/* Extract axis-angle from quaternion. Returns angle in radians. */
static inline float tp_q_to_axis_angle(TydraPhysQuat q,
                                        TydraPhysVec3 *axis) {
  q = tp_q_normalize(q);
  if (q.w < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
  float sin_half = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
  if (sin_half < TP_EPSILON) {
    *axis = tp_v3(1, 0, 0);
    return 0.0f;
  }
  float inv = 1.0f / sin_half;
  *axis = tp_v3(q.x * inv, q.y * inv, q.z * inv);
  return 2.0f * atan2f(sin_half, q.w);
}

static inline TydraPhysVec3 tp_q_rotate(TydraPhysQuat q, TydraPhysVec3 v) {
  /* q * (0,v) * q^-1 */
  TydraPhysQuat qv = tp_q(v.x, v.y, v.z, 0);
  TydraPhysQuat r = tp_q_mul(tp_q_mul(q, qv), tp_q_conjugate(q));
  return tp_v3(r.x, r.y, r.z);
}

/* Standard quaternion slerp with shortest-path check. */
static inline TydraPhysQuat tp_q_slerp(TydraPhysQuat a, TydraPhysQuat b,
                                         float t) {
  float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  /* Ensure shortest path */
  if (dot < 0.0f) {
    b = tp_q(-b.x, -b.y, -b.z, -b.w);
    dot = -dot;
  }
  if (dot > 1.0f - TP_EPSILON) {
    /* Very close -- linear interpolation to avoid division by zero */
    TydraPhysQuat r = tp_q(a.x + (b.x - a.x) * t,
                           a.y + (b.y - a.y) * t,
                           a.z + (b.z - a.z) * t,
                           a.w + (b.w - a.w) * t);
    return tp_q_normalize(r);
  }
  float theta = acosf(dot);
  float sin_theta = sinf(theta);
  float wa = sinf((1.0f - t) * theta) / sin_theta;
  float wb = sinf(t * theta) / sin_theta;
  return tp_q(wa * a.x + wb * b.x,
              wa * a.y + wb * b.y,
              wa * a.z + wb * b.z,
              wa * a.w + wb * b.w);
}

// ============================================================================
// Mat4 (row-major) -- m[row*4 + col]
// ============================================================================

static inline void tp_m4_identity(TydraPhysMat4 *m) {
  memset(m->m, 0, sizeof(m->m));
  m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1.0f;
}

static inline void tp_m4_mul(const TydraPhysMat4 *a, const TydraPhysMat4 *b,
                              TydraPhysMat4 *out) {
  TydraPhysMat4 r;
  int i, j, k;
  for (i = 0; i < 4; i++) {
    for (j = 0; j < 4; j++) {
      float sum = 0;
      for (k = 0; k < 4; k++) {
        sum += a->m[i * 4 + k] * b->m[k * 4 + j];
      }
      r.m[i * 4 + j] = sum;
    }
  }
  *out = r;
}

static inline TydraPhysVec3 tp_m4_get_translation(const TydraPhysMat4 *m) {
  return tp_v3(m->m[3], m->m[7], m->m[11]);
}

static inline void tp_m4_set_translation(TydraPhysMat4 *m, TydraPhysVec3 t) {
  m->m[3] = t.x; m->m[7] = t.y; m->m[11] = t.z;
}

static inline TydraPhysQuat tp_m4_to_quat(const TydraPhysMat4 *m) {
  /* Shepperd's method for rotation matrix -> quaternion */
  float m00 = m->m[0], m01 = m->m[1], m02 = m->m[2];
  float m10 = m->m[4], m11 = m->m[5], m12 = m->m[6];
  float m20 = m->m[8], m21 = m->m[9], m22 = m->m[10];
  float trace = m00 + m11 + m22;
  TydraPhysQuat q;
  if (trace > 0) {
    float s = sqrtf(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m21 - m12) / s;
    q.y = (m02 - m20) / s;
    q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
    q.w = (m21 - m12) / s;
    q.x = 0.25f * s;
    q.y = (m01 + m10) / s;
    q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
    q.w = (m02 - m20) / s;
    q.x = (m01 + m10) / s;
    q.y = 0.25f * s;
    q.z = (m12 + m21) / s;
  } else {
    float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
    q.w = (m10 - m01) / s;
    q.x = (m02 + m20) / s;
    q.y = (m12 + m21) / s;
    q.z = 0.25f * s;
  }
  return tp_q_normalize(q);
}

static inline void tp_q_to_m4(TydraPhysQuat q, TydraPhysMat4 *m) {
  q = tp_q_normalize(q);
  float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  tp_m4_identity(m);
  m->m[0]  = 1 - 2 * (yy + zz); m->m[1]  = 2 * (xy - wz);     m->m[2]  = 2 * (xz + wy);
  m->m[4]  = 2 * (xy + wz);     m->m[5]  = 1 - 2 * (xx + zz); m->m[6]  = 2 * (yz - wx);
  m->m[8]  = 2 * (xz - wy);     m->m[9]  = 2 * (yz + wx);     m->m[10] = 1 - 2 * (xx + yy);
}

static inline void tp_make_local_transform(TydraPhysQuat rot,
                                             TydraPhysVec3 trans,
                                             TydraPhysMat4 *out) {
  tp_q_to_m4(rot, out);
  tp_m4_set_translation(out, trans);
}

static inline void tp_m4_inverse_rigid(const TydraPhysMat4 *m,
                                        TydraPhysMat4 *out) {
  /* Transpose the 3x3 rotation part */
  TydraPhysMat4 r;
  tp_m4_identity(&r);
  r.m[0] = m->m[0]; r.m[1] = m->m[4]; r.m[2]  = m->m[8];
  r.m[4] = m->m[1]; r.m[5] = m->m[5]; r.m[6]  = m->m[9];
  r.m[8] = m->m[2]; r.m[9] = m->m[6]; r.m[10] = m->m[10];
  /* -R^T * t */
  TydraPhysVec3 t = tp_m4_get_translation(m);
  r.m[3]  = -(r.m[0] * t.x + r.m[1] * t.y + r.m[2]  * t.z);
  r.m[7]  = -(r.m[4] * t.x + r.m[5] * t.y + r.m[6]  * t.z);
  r.m[11] = -(r.m[8] * t.x + r.m[9] * t.y + r.m[10] * t.z);
  *out = r;
}

/* Map axis enum (0=X, 1=Y, 2=Z) to unit vector. */
static inline TydraPhysVec3 tp_axis_vector(int axis) {
  switch (axis) {
    case 0: return tp_v3(1, 0, 0);
    case 1: return tp_v3(0, 1, 0);
    case 2: return tp_v3(0, 0, 1);
  }
  return tp_v3(0, 1, 0);
}

// ============================================================================
// Mat3 (row-major 3x3)
// ============================================================================

static inline TydraPhysMat3 tp_m3_identity(void) {
  TydraPhysMat3 m;
  memset(m.m, 0, sizeof(m.m));
  m.m[0] = m.m[4] = m.m[8] = 1.0f;
  return m;
}

static inline TydraPhysVec3 tp_m3_mul_vec(const TydraPhysMat3 *m,
                                           TydraPhysVec3 v) {
  return tp_v3(m->m[0] * v.x + m->m[1] * v.y + m->m[2] * v.z,
               m->m[3] * v.x + m->m[4] * v.y + m->m[5] * v.z,
               m->m[6] * v.x + m->m[7] * v.y + m->m[8] * v.z);
}

static inline TydraPhysMat3 tp_m3_transpose(TydraPhysMat3 m) {
  TydraPhysMat3 r;
  r.m[0] = m.m[0]; r.m[1] = m.m[3]; r.m[2] = m.m[6];
  r.m[3] = m.m[1]; r.m[4] = m.m[4]; r.m[5] = m.m[7];
  r.m[6] = m.m[2]; r.m[7] = m.m[5]; r.m[8] = m.m[8];
  return r;
}

static inline TydraPhysMat3 tp_m3_mul(const TydraPhysMat3 *a,
                                       const TydraPhysMat3 *b) {
  TydraPhysMat3 r;
  int i, j, k;
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      float sum = 0;
      for (k = 0; k < 3; k++) {
        sum += a->m[i * 3 + k] * b->m[k * 3 + j];
      }
      r.m[i * 3 + j] = sum;
    }
  }
  return r;
}

/* Build 3x3 rotation matrix from quaternion (same math as tp_q_to_m4). */
static inline TydraPhysMat3 tp_m3_from_quat(TydraPhysQuat q) {
  q = tp_q_normalize(q);
  float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  TydraPhysMat3 m;
  m.m[0] = 1 - 2 * (yy + zz); m.m[1] = 2 * (xy - wz);     m.m[2] = 2 * (xz + wy);
  m.m[3] = 2 * (xy + wz);     m.m[4] = 1 - 2 * (xx + zz); m.m[5] = 2 * (yz - wx);
  m.m[6] = 2 * (xz - wy);     m.m[7] = 2 * (yz + wx);     m.m[8] = 1 - 2 * (xx + yy);
  return m;
}

/* Scalar multiply: every element *= s. */
static inline TydraPhysMat3 tp_m3_scale(TydraPhysMat3 m, float s) {
  int i;
  for (i = 0; i < 9; i++) m.m[i] *= s;
  return m;
}

// ============================================================================
// Transform (position + rotation)
// ============================================================================

static inline TydraPhysTransform tp_xform_identity(void) {
  TydraPhysTransform xf;
  xf.position = tp_v3(0, 0, 0);
  xf.rotation = tp_q_identity();
  return xf;
}

/* Rotate and translate a point: q * p + t. */
static inline TydraPhysVec3 tp_xform_point(TydraPhysTransform xf,
                                             TydraPhysVec3 p) {
  return tp_v3_add(tp_q_rotate(xf.rotation, p), xf.position);
}

/* Rotate a direction vector (no translation). */
static inline TydraPhysVec3 tp_xform_vector(TydraPhysTransform xf,
                                              TydraPhysVec3 v) {
  return tp_q_rotate(xf.rotation, v);
}

/* Inverse of a rigid transform. */
static inline TydraPhysTransform tp_xform_inverse(TydraPhysTransform xf) {
  TydraPhysTransform inv;
  inv.rotation = tp_q_conjugate(xf.rotation);
  inv.position = tp_q_rotate(inv.rotation, tp_v3_negate(xf.position));
  return inv;
}

/* Compose two transforms: result = a then b  (b applied in a's frame). */
static inline TydraPhysTransform tp_xform_mul(TydraPhysTransform a,
                                                TydraPhysTransform b) {
  TydraPhysTransform r;
  r.rotation = tp_q_mul(a.rotation, b.rotation);
  r.position = tp_v3_add(tp_q_rotate(a.rotation, b.position), a.position);
  return r;
}

/* Convert transform to 4x4 matrix. */
static inline TydraPhysMat4 tp_xform_to_m4(TydraPhysTransform xf) {
  TydraPhysMat4 m;
  tp_make_local_transform(xf.rotation, xf.position, &m);
  return m;
}

/* Extract transform from 4x4 matrix (assumes rigid -- no scale). */
static inline TydraPhysTransform tp_m4_to_xform(const TydraPhysMat4 *m) {
  TydraPhysTransform xf;
  xf.position = tp_m4_get_translation(m);
  xf.rotation = tp_m4_to_quat(m);
  return xf;
}

// ============================================================================
// Inertia helpers
// ============================================================================

/*
 * Compute world-space 3x3 inertia tensor from a diagonal local-space
 * inertia vector and a rotation quaternion:  I_world = R * diag(d) * R^T.
 */
static inline TydraPhysMat3 tp_inertia_world(TydraPhysVec3 diag,
                                               TydraPhysQuat q) {
  TydraPhysMat3 rot = tp_m3_from_quat(q);
  TydraPhysMat3 rot_t = tp_m3_transpose(rot);
  /* Build diagonal matrix */
  TydraPhysMat3 d;
  memset(d.m, 0, sizeof(d.m));
  d.m[0] = diag.x; d.m[4] = diag.y; d.m[8] = diag.z;
  /* R * D */
  TydraPhysMat3 rd = tp_m3_mul(&rot, &d);
  /* (R * D) * R^T */
  return tp_m3_mul(&rd, &rot_t);
}

/* Invert a diagonal inertia vector (component-wise reciprocal). */
static inline TydraPhysVec3 tp_inertia_inverse_diagonal(TydraPhysVec3 diag) {
  return tp_v3(tp_absf(diag.x) > TP_EPSILON ? 1.0f / diag.x : 0.0f,
               tp_absf(diag.y) > TP_EPSILON ? 1.0f / diag.y : 0.0f,
               tp_absf(diag.z) > TP_EPSILON ? 1.0f / diag.z : 0.0f);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUSD_TYDRA_RB_MATH_H_ */
