#pragma once

#include "value-types.hh"
#include "linear-algebra.hh"

namespace lightusd {

#define FOUR_ARITH_OP_2(__ty, __basety) \
inline __ty operator+(const __ty &a, const __ty &b) { \
  return {a[0] + b[0], a[1] + b[1]}; \
} \
\
inline __ty operator+(const __basety a, const __ty &b) { \
  return {a + b[0], a + b[1]}; \
}\
inline __ty operator+(const __ty &a, const __basety b) { \
  return {a[0] + b, a[1] + b}; \
} \
\
inline __ty operator-(const __ty &a, const __ty &b) { \
  return {a[0] - b[0], a[1] - b[1]}; \
} \
\
inline __ty operator-(const __basety a, const __ty &b) { \
  return {a - b[0], a - b[1]}; \
}\
inline __ty operator-(const __ty &a, const __basety b) { \
  return {a[0] - b, a[1] - b}; \
}\
\
inline __ty operator*(const __ty &a, const __ty &b) { \
  return {a[0] * b[0], a[1] * b[1]}; \
} \
\
inline __ty operator*(const __basety a, const __ty &b) { \
  return {a * b[0], a * b[1]}; \
}\
inline __ty operator*(const __ty &a, const __basety b) { \
  return {a[0] * b, a[1] * b}; \
} \
\
inline __ty operator/(const __ty &a, const __ty &b) { \
  return {a[0] / b[0], a[1] / b[1]}; \
} \
\
inline __ty operator/(const __basety a, const __ty &b) { \
  return {a / b[0], a / b[1]}; \
}\
inline __ty operator/(const __ty &a, const __basety b) { \
  return {a[0] / b, a[1] / b}; \
}

#define FOUR_ARITH_OP_3(__ty, __basety) \
inline __ty operator+(const __ty &a, const __ty &b) { \
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; \
} \
\
inline __ty operator+(const __basety a, const __ty &b) { \
  return {a + b[0], a + b[1], a + b[2]}; \
}\
inline __ty operator+(const __ty &a, const __basety b) { \
  return {a[0] + b, a[1] + b, a[2] + b}; \
} \
\
inline __ty operator-(const __ty &a, const __ty &b) { \
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; \
} \
\
inline __ty operator-(const __basety a, const __ty &b) { \
  return {a - b[0], a - b[1], a - b[2]}; \
}\
inline __ty operator-(const __ty &a, const __basety b) { \
  return {a[0] - b, a[1] - b, a[2] - b}; \
}\
\
inline __ty operator*(const __ty &a, const __ty &b) { \
  return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; \
} \
\
inline __ty operator*(const __basety a, const __ty &b) { \
  return {a * b[0], a * b[1], a * b[2]}; \
}\
inline __ty operator*(const __ty &a, const __basety b) { \
  return {a[0] * b, a[1] * b, a[2] * b}; \
} \
\
inline __ty operator/(const __ty &a, const __ty &b) { \
  return {a[0] / b[0], a[1] / b[1], a[2] / b[2]}; \
} \
\
inline __ty operator/(const __basety a, const __ty &b) { \
  return {a / b[0], a / b[1], a / b[2]}; \
}\
inline __ty operator/(const __ty &a, const __basety b) { \
  return {a[0] / b, a[1] / b, a[2] / b}; \
}

#define FOUR_ARITH_OP_4(__ty, __basety) \
inline __ty operator+(const __ty &a, const __ty &b) { \
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]}; \
} \
\
inline __ty operator+(const __basety a, const __ty &b) { \
  return {a + b[0], a + b[1], a + b[2], a + b[3]}; \
}\
inline __ty operator+(const __ty &a, const __basety b) { \
  return {a[0] + b, a[1] + b, a[2] + b, a[3] + b}; \
} \
\
inline __ty operator-(const __ty &a, const __ty &b) { \
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]}; \
} \
\
inline __ty operator-(const __basety a, const __ty &b) { \
  return {a - b[0], a - b[1], a - b[2], a - b[3]}; \
}\
inline __ty operator-(const __ty &a, const __basety b) { \
  return {a[0] - b, a[1] - b, a[2] - b, a[3] - b}; \
}\
\
inline __ty operator*(const __ty &a, const __ty &b) { \
  return {a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]}; \
} \
\
inline __ty operator*(const __basety a, const __ty &b) { \
  return {a * b[0], a * b[1], a * b[2], a * b[3]}; \
}\
inline __ty operator*(const __ty &a, const __basety b) { \
  return {a[0] * b, a[1] * b, a[2] * b, a[3] * b}; \
} \
\
inline __ty operator/(const __ty &a, const __ty &b) { \
  return {a[0] / b[0], a[1] / b[1], a[2] / b[2], a[3] / b[3]}; \
} \
\
inline __ty operator/(const __basety a, const __ty &b) { \
  return {a / b[0], a / b[1], a / b[2], a / b[3]}; \
}\
inline __ty operator/(const __ty &a, const __basety b) { \
  return {a[0] / b, a[1] / b, a[2] / b, a[3] / b}; \
}

#define ARITH_ASSIGN_OP_2(__ty, __basety) \
inline __ty &operator+=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] += b[0]; a[1] += b[1]; \
  return a; \
} \
inline __ty &operator-=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] -= b[0]; a[1] -= b[1]; \
  return a; \
} \
inline __ty &operator*=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] *= b[0]; a[1] *= b[1]; \
  return a; \
} \
inline __ty &operator/=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] /= b[0]; a[1] /= b[1]; \
  return a; \
} \

#define ARITH_ASSIGN_OP_3(__ty, __basety) \
inline __ty &operator+=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] += b[0]; a[1] += b[1]; a[2] += b[2]; \
  return a; \
} \
inline __ty &operator-=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] -= b[0]; a[1] -= b[1]; a[2] -= b[2]; \
  return a; \
} \
inline __ty &operator*=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] *= b[0]; a[1] *= b[1]; a[2] *= b[2]; \
  return a; \
} \
inline __ty &operator/=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] /= b[0]; a[1] /= b[1]; a[2] /= b[2]; \
  return a; \
} \

#define ARITH_ASSIGN_OP_4(__ty, __basety) \
inline __ty &operator+=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] += b[0]; a[1] += b[1]; a[2] += b[2]; a[3] += b[3]; \
  return a; \
} \
inline __ty &operator-=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] -= b[0]; a[1] -= b[1]; a[2] -= b[2]; a[3] -= b[3]; \
  return a; \
} \
inline __ty &operator*=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] *= b[0]; a[1] *= b[1]; a[2] *= b[2]; a[3] *= b[3]; \
  return a; \
} \
inline __ty &operator/=(__ty &a LIGHTUSD_LIFETIMEBOUND, const __ty &b) { \
  a[0] /= b[0]; a[1] /= b[1]; a[2] /= b[2]; a[3] /= b[3]; \
  return a; \
} \

// TODO: half op scalar_half
FOUR_ARITH_OP_2(value::half2, float)
FOUR_ARITH_OP_3(value::half3, float)
FOUR_ARITH_OP_4(value::half4, float)
ARITH_ASSIGN_OP_2(value::half2, float)
ARITH_ASSIGN_OP_3(value::half3, float)
ARITH_ASSIGN_OP_4(value::half4, float)

FOUR_ARITH_OP_2(value::int2, int)
FOUR_ARITH_OP_3(value::int3, int)
FOUR_ARITH_OP_4(value::int4, int)
ARITH_ASSIGN_OP_2(value::int2, int)
ARITH_ASSIGN_OP_3(value::int3, int)
ARITH_ASSIGN_OP_4(value::int4, int)

FOUR_ARITH_OP_2(value::uint2, uint32_t)
FOUR_ARITH_OP_3(value::uint3, uint32_t)
FOUR_ARITH_OP_4(value::uint4, uint32_t)
ARITH_ASSIGN_OP_2(value::uint2, uint32_t)
ARITH_ASSIGN_OP_3(value::uint3, uint32_t)
ARITH_ASSIGN_OP_4(value::uint4, uint32_t)

FOUR_ARITH_OP_2(value::float2, float)
FOUR_ARITH_OP_3(value::float3, float)
FOUR_ARITH_OP_4(value::float4, float)
ARITH_ASSIGN_OP_2(value::float2, float)
ARITH_ASSIGN_OP_3(value::float3, float)
ARITH_ASSIGN_OP_4(value::float4, float)

FOUR_ARITH_OP_2(value::double2, double)
FOUR_ARITH_OP_3(value::double3, double)
FOUR_ARITH_OP_4(value::double4, double)
ARITH_ASSIGN_OP_2(value::double2, double)
ARITH_ASSIGN_OP_3(value::double3, double)
ARITH_ASSIGN_OP_4(value::double4, double)

FOUR_ARITH_OP_3(value::normal3h, float)
FOUR_ARITH_OP_3(value::normal3f, float)
FOUR_ARITH_OP_3(value::normal3d, double)
ARITH_ASSIGN_OP_3(value::normal3h, float)
ARITH_ASSIGN_OP_3(value::normal3f, float)
ARITH_ASSIGN_OP_3(value::normal3d, double)

FOUR_ARITH_OP_3(value::vector3h, float)
FOUR_ARITH_OP_3(value::vector3f, float)
FOUR_ARITH_OP_3(value::vector3d, double)
ARITH_ASSIGN_OP_3(value::vector3h, float)
ARITH_ASSIGN_OP_3(value::vector3f, float)
ARITH_ASSIGN_OP_3(value::vector3d, double)

FOUR_ARITH_OP_3(value::point3h, float)
FOUR_ARITH_OP_3(value::point3f, float)
FOUR_ARITH_OP_3(value::point3d, double)
ARITH_ASSIGN_OP_3(value::point3h, float)
ARITH_ASSIGN_OP_3(value::point3f, float)
ARITH_ASSIGN_OP_3(value::point3d, double)

FOUR_ARITH_OP_3(value::color3h, float)
FOUR_ARITH_OP_3(value::color3f, float)
FOUR_ARITH_OP_3(value::color3d, double)
ARITH_ASSIGN_OP_3(value::color3h, float)
ARITH_ASSIGN_OP_3(value::color3f, float)
ARITH_ASSIGN_OP_3(value::color3d, double)

FOUR_ARITH_OP_4(value::color4h, float)
FOUR_ARITH_OP_4(value::color4f, float)
FOUR_ARITH_OP_4(value::color4d, double)
ARITH_ASSIGN_OP_4(value::color4h, float)
ARITH_ASSIGN_OP_4(value::color4f, float)
ARITH_ASSIGN_OP_4(value::color4d, double)

FOUR_ARITH_OP_2(value::texcoord2h, float)
FOUR_ARITH_OP_2(value::texcoord2f, float)
FOUR_ARITH_OP_2(value::texcoord2d, double)
ARITH_ASSIGN_OP_2(value::texcoord2h, float)
ARITH_ASSIGN_OP_2(value::texcoord2f, float)
ARITH_ASSIGN_OP_2(value::texcoord2d, double)

FOUR_ARITH_OP_3(value::texcoord3h, float)
FOUR_ARITH_OP_3(value::texcoord3f, float)
FOUR_ARITH_OP_3(value::texcoord3d, double)
ARITH_ASSIGN_OP_3(value::texcoord3h, float)
ARITH_ASSIGN_OP_3(value::texcoord3f, float)
ARITH_ASSIGN_OP_3(value::texcoord3d, double)

#undef FOUR_ARITH_OP_2
#undef FOUR_ARITH_OP_3
#undef FOUR_ARITH_OP_4

// Matrix/frame operators — defined in value-eval-util.cc
value::matrix2f operator+(const value::matrix2f &a, const double b);
value::matrix2f operator+(const double a, const value::matrix2f &b);
value::matrix2f operator-(const value::matrix2f &a, const double b);
value::matrix2f operator-(const double a, const value::matrix2f &b);
value::matrix2f operator*(const value::matrix2f &a, const double b);
value::matrix2f operator*(const double a, const value::matrix2f &b);
value::matrix2f operator/(const value::matrix2f &a, const double b);
value::matrix2f operator/(const double a, const value::matrix2f &b);
value::matrix3f operator+(const value::matrix3f &a, const double b);
value::matrix3f operator+(const double a, const value::matrix3f &b);
value::matrix3f operator-(const value::matrix3f &a, const double b);
value::matrix3f operator-(const double a, const value::matrix3f &b);
value::matrix3f operator*(const value::matrix3f &a, const double b);
value::matrix3f operator*(const double a, const value::matrix3f &b);
value::matrix3f operator/(const value::matrix3f &a, const double b);
value::matrix3f operator/(const double a, const value::matrix3f &b);
value::matrix4f operator+(const value::matrix4f &a, const double b);
value::matrix4f operator+(const double a, const value::matrix4f &b);
value::matrix4f operator-(const value::matrix4f &a, const double b);
value::matrix4f operator-(const double a, const value::matrix4f &b);
value::matrix4f operator*(const value::matrix4f &a, const double b);
value::matrix4f operator*(const double a, const value::matrix4f &b);
value::matrix4f operator/(const value::matrix4f &a, const double b);
value::matrix4f operator/(const double a, const value::matrix4f &b);
value::matrix2d operator+(const value::matrix2d &a, const double b);
value::matrix2d operator+(const double a, const value::matrix2d &b);
value::matrix2d operator-(const value::matrix2d &a, const double b);
value::matrix2d operator-(const double a, const value::matrix2d &b);
value::matrix2d operator*(const value::matrix2d &a, const double b);
value::matrix2d operator*(const double a, const value::matrix2d &b);
value::matrix2d operator/(const value::matrix2d &a, const double b);
value::matrix2d operator/(const double a, const value::matrix2d &b);
value::matrix3d operator+(const value::matrix3d &a, const double b);
value::matrix3d operator+(const double a, const value::matrix3d &b);
value::matrix3d operator-(const value::matrix3d &a, const double b);
value::matrix3d operator-(const double a, const value::matrix3d &b);
value::matrix3d operator*(const value::matrix3d &a, const double b);
value::matrix3d operator*(const double a, const value::matrix3d &b);
value::matrix3d operator/(const value::matrix3d &a, const double b);
value::matrix3d operator/(const double a, const value::matrix3d &b);
value::matrix4d operator+(const value::matrix4d &a, const double b);
value::matrix4d operator+(const double a, const value::matrix4d &b);
value::matrix4d operator-(const value::matrix4d &a, const double b);
value::matrix4d operator-(const double a, const value::matrix4d &b);
value::matrix4d operator*(const value::matrix4d &a, const double b);
value::matrix4d operator*(const double a, const value::matrix4d &b);
value::matrix4d operator/(const value::matrix4d &a, const double b);
value::matrix4d operator/(const double a, const value::matrix4d &b);
value::frame4d operator+(const value::frame4d &a, const value::frame4d &b);
value::frame4d operator+(const value::frame4d &a, const double b);
value::frame4d operator+(const double a, const value::frame4d &b);
value::frame4d operator-(const value::frame4d &a, const double b);
value::frame4d operator-(const double a, const value::frame4d &b);
value::frame4d operator*(const value::frame4d &a, const double b);
value::frame4d operator*(const double a, const value::frame4d &b);
value::frame4d operator/(const value::frame4d &a, const double b);
value::frame4d operator/(const double a, const value::frame4d &b);

// half


//
// Inlined lerp
// (Use prim-types.hh::Lerp if you do not need the performance)

// no lerp by default
template <typename T>
inline T lerp(const T &a, const T &b, const double t) {
  (void)b;
  (void)t;
  return a;
}

#define LIGHTUSD_INLINED_LERP(__ty, __interp_ty) \
template <> \
inline __ty lerp(const __ty &a, const __ty &b, const double t) { \
  return __interp_ty(1.0 - t) * a + __interp_ty(t) * b; \
}

LIGHTUSD_INLINED_LERP(value::half, float)
LIGHTUSD_INLINED_LERP(value::half2, float)
LIGHTUSD_INLINED_LERP(value::half3, float)
LIGHTUSD_INLINED_LERP(value::half4, float)
LIGHTUSD_INLINED_LERP(float, float)
LIGHTUSD_INLINED_LERP(value::float2, float)
LIGHTUSD_INLINED_LERP(value::float3, float)
LIGHTUSD_INLINED_LERP(value::float4, float)
LIGHTUSD_INLINED_LERP(double, double)
LIGHTUSD_INLINED_LERP(value::double2, double)
LIGHTUSD_INLINED_LERP(value::double3, double)
LIGHTUSD_INLINED_LERP(value::double4, double)
LIGHTUSD_INLINED_LERP(value::normal3h, float)
LIGHTUSD_INLINED_LERP(value::normal3f, float)
LIGHTUSD_INLINED_LERP(value::normal3d, double)
LIGHTUSD_INLINED_LERP(value::vector3h, float)
LIGHTUSD_INLINED_LERP(value::vector3f, float)
LIGHTUSD_INLINED_LERP(value::vector3d, double)
LIGHTUSD_INLINED_LERP(value::point3h, float)
LIGHTUSD_INLINED_LERP(value::point3f, float)
LIGHTUSD_INLINED_LERP(value::point3d, double)
LIGHTUSD_INLINED_LERP(value::color3h, float)
LIGHTUSD_INLINED_LERP(value::color3f, float)
LIGHTUSD_INLINED_LERP(value::color3d, double)
LIGHTUSD_INLINED_LERP(value::color4h, float)
LIGHTUSD_INLINED_LERP(value::color4f, float)
LIGHTUSD_INLINED_LERP(value::color4d, double)
LIGHTUSD_INLINED_LERP(value::texcoord2h, float)
LIGHTUSD_INLINED_LERP(value::texcoord2f, float)
LIGHTUSD_INLINED_LERP(value::texcoord2d, double)
LIGHTUSD_INLINED_LERP(value::texcoord3h, float)
LIGHTUSD_INLINED_LERP(value::texcoord3f, float)
LIGHTUSD_INLINED_LERP(value::texcoord3d, double)

// TODO: robust arithmetic for matrix add/sub/mul/div
LIGHTUSD_INLINED_LERP(value::matrix2f, double)
LIGHTUSD_INLINED_LERP(value::matrix3f, double)
LIGHTUSD_INLINED_LERP(value::matrix4f, double)
LIGHTUSD_INLINED_LERP(value::matrix2d, double)
LIGHTUSD_INLINED_LERP(value::matrix3d, double)
LIGHTUSD_INLINED_LERP(value::matrix4d, double)
LIGHTUSD_INLINED_LERP(value::frame4d, double)
//LIGHTUSD_INLINED_LERP(value::timecode, double)

#undef LIGHTUSD_INLINED_LERP



// for generic vector data.
template <typename T>
inline std::vector<T> lerp(const std::vector<T> &a, const std::vector<T> &b,
                           const double t) {
  std::vector<T> dst;

  // Choose shorter one
  size_t n = std::min(a.size(), b.size());
  if (n == 0) {
    return dst;
  }

  if (a.size() != b.size()) {
    // TODO: report error
    //std::cout << "vector size mismatch, returning v0" << std::endl;
    return dst;
  }

  dst.resize(n);
  for (size_t i = 0; i < n; i++) {
    dst[i] = lerp(a[i], b[i], t);
  }

  return dst;
}

template <>
inline value::quath lerp(const value::quath &a, const value::quath &b, const double t) {
  // to float.
  value::quatf af;
  value::quatf bf;
  af.real = half_to_float(a.real);
  af.imag[0] = half_to_float(a.imag[0]);
  af.imag[1] = half_to_float(a.imag[1]);
  af.imag[2] = half_to_float(a.imag[2]);

  bf.real = half_to_float(b.real);
  bf.imag[0] = half_to_float(b.imag[0]);
  bf.imag[1] = half_to_float(b.imag[1]);
  bf.imag[2] = half_to_float(b.imag[2]);

  value::quatf ret =  slerp(af, bf, float(t));
  value::quath h;
  h.real = value::float_to_half_full(ret.real);
  h.imag[0] = value::float_to_half_full(ret.imag[0]);
  h.imag[1] = value::float_to_half_full(ret.imag[1]);
  h.imag[2] = value::float_to_half_full(ret.imag[2]);

  return h;
}

template <>
inline value::quatf lerp(const value::quatf &a, const value::quatf &b, const double t) {
  // slerp
  return slerp(a, b, float(t));
}

template <>
inline value::quatd lerp(const value::quatd &a, const value::quatd &b, const double t) {
  // slerp
  return slerp(a, b, t);
}



} // namespace lightusd
