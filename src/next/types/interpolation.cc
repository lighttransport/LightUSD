// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Time Sample Interpolation Implementation

#include "interpolation.hh"
#include "../c/lightusd-core-c.h"
#include "../crate/crate-format.hh"
#include "type-id.hh"
#include <cmath>
#include <cstring>

namespace lightusd {
namespace next {

bool TimeInterpolator::IsLinearInterpolatable(TypeId type) {
  return lu_type_is_linear_interpolatable(static_cast<uint16_t>(type)) != 0;
}

// ============================================================
// Scalar interpolation
// ============================================================

float TimeInterpolator::LerpFloat(float a, float b, float t) {
  return lu_lerp_f(a, b, t);
}

double TimeInterpolator::LerpDouble(double a, double b, double t) {
  return lu_lerp_d(a, b, t);
}

// ============================================================
// Vector interpolation
// ============================================================

void TimeInterpolator::LerpFloatN(float* out, const float* a, const float* b, float t, size_t n) {
  lu_lerp_fn(out, a, b, t, n);
}

void TimeInterpolator::LerpDoubleN(double* out, const double* a, const double* b, double t, size_t n) {
  lu_lerp_dn(out, a, b, t, n);
}

// ============================================================
// Quaternion interpolation (Spherical Linear Interpolation)
// ============================================================

void TimeInterpolator::SlerpQuatf(float* out, const float* a, const float* b, float t) {
  lu_slerp_quatf(out, a, b, t);
}

void TimeInterpolator::SlerpQuatd(double* out, const double* a, const double* b, double t) {
  lu_slerp_quatd(out, a, b, t);
}

// ============================================================
// Value interpolation
// ============================================================

Value TimeInterpolator::InterpolateValues(const Value& a, const Value& b, double t) {
  // Types must match
  if (a.type_id() != b.type_id()) {
    return Value();
  }

  // Both must be same array status
  if (a.is_array() != b.is_array()) {
    return Value();
  }

  TypeId type = a.type_id();
  float tf = static_cast<float>(t);

  // Handle array interpolation
  if (a.is_array()) {
    // Size-mismatched arrays cannot lerp; HOLD the earlier sample (pxr
    // behavior — changing topology must not drop the value entirely).
    if (a.array_size() != b.array_size()) {
      return a;
    }

    const size_t comps = GetComponentCount(type);

    // Quaternion arrays: per-element slerp. Elementwise lerp is wrong for
    // rotations between keys (SkelAnimation `rotations` is quatf[]); quath
    // arrays materialize into the float buffer like the other half types.
    if (type == TypeId::Quatf || type == TypeId::Quath) {
      auto arr_a = a.as_float_array();
      auto arr_b = b.as_float_array();
      if (arr_a && arr_b && arr_a->size() == arr_b->size() &&
          (arr_a->size() % 4) == 0) {
        std::vector<float> result(arr_a->size());
        for (size_t i = 0; i < result.size(); i += 4) {
          SlerpQuatf(result.data() + i, arr_a->data() + i, arr_b->data() + i,
                     tf);
        }
        return Value::MakeFloatCompArray(std::move(result), type, 4);
      }
      return Value();
    }
    if (type == TypeId::Quatd) {
      auto arr_a = a.as_double_array();
      auto arr_b = b.as_double_array();
      if (arr_a && arr_b && arr_a->size() == arr_b->size() &&
          (arr_a->size() % 4) == 0) {
        std::vector<double> result(arr_a->size());
        for (size_t i = 0; i < result.size(); i += 4) {
          SlerpQuatd(result.data() + i, arr_a->data() + i, arr_b->data() + i,
                     t);
        }
        return Value::MakeDoubleCompArray(std::move(result), type, 4);
      }
      return Value();
    }

    // Any float-backed array (float/half scalars, vectors, matrices — half
    // element types materialize into the float buffer): lerp all lanes.
    if (auto arr_a = a.as_float_array()) {
      auto arr_b = b.as_float_array();
      if (arr_b && arr_a->size() == arr_b->size() && comps > 0) {
        std::vector<float> result(arr_a->size());
        LerpFloatN(result.data(), arr_a->data(), arr_b->data(), tf,
                   arr_a->size());
        if (type == TypeId::Float) {
          return Value::MakeFloatArray(std::move(result));
        }
        return Value::MakeFloatCompArray(std::move(result), type,
                                         static_cast<uint32_t>(comps));
      }
      return Value();
    }

    // Double-backed arrays.
    if (auto arr_a = a.as_double_array()) {
      auto arr_b = b.as_double_array();
      if (arr_b && arr_a->size() == arr_b->size() && comps > 0) {
        std::vector<double> result(arr_a->size());
        LerpDoubleN(result.data(), arr_a->data(), arr_b->data(), t,
                    arr_a->size());
        if (type == TypeId::Double) {
          return Value::MakeDoubleArray(std::move(result));
        }
        return Value::MakeDoubleCompArray(std::move(result), type,
                                          static_cast<uint32_t>(comps));
      }
      return Value();
    }

    // Non-interpolatable arrays (int/uint/bool/string-like): held.
    return Value(a);
  }

  // Half-backed scalars and semantic aliases use raw uint16 lanes in Value's
  // SBO. Widen, interpolate (or slerp for quath), then quantize once at the
  // result while preserving the declared role type.
  const TypeId component =
      (type == TypeId::Half) ? TypeId::Half : GetComponentType(type);
  const size_t components = GetComponentCount(type);
  if (component == TypeId::Half && components >= 1 && components <= 4) {
    float va[4] = {}, vb[4] = {}, result[4] = {};
    bool ok = false;
    if (components == 1) ok = a.to_float(va) && b.to_float(vb);
    else if (components == 2) ok = a.to_float2(va) && b.to_float2(vb);
    else if (components == 3) ok = a.to_float3(va) && b.to_float3(vb);
    else ok = a.to_float4(va) && b.to_float4(vb);
    if (!ok) return Value(a);
    if (type == TypeId::Quath) {
      SlerpQuatf(result, va, vb, tf);
    } else {
      LerpFloatN(result, va, vb, tf, components);
    }
    uint16_t bits[4] = {};
    for (size_t i = 0; i < components; ++i) bits[i] = FloatToHalf(result[i]);
    return Value::MakeFromRaw(type, bits);
  }

  // Handle scalar/vector types
  switch (type) {
    // Scalar float types
    case TypeId::Float: {
      const float* va = a.as_float();
      const float* vb = b.as_float();
      if (va && vb) {
        return Value(LerpFloat(*va, *vb, tf));
      }
      break;
    }

    case TypeId::Double: {
      const double* va = a.as_double();
      const double* vb = b.as_double();
      if (va && vb) {
        return Value(LerpDouble(*va, *vb, t));
      }
      break;
    }

    case TypeId::TimeCode: {  // same 8-byte double storage; keep the type
      const double* va = a.as_double();
      const double* vb = b.as_double();
      if (va && vb) {
        const double r = LerpDouble(*va, *vb, t);
        return Value::MakeFromRaw(TypeId::TimeCode, &r);
      }
      break;
    }

    // Integer types - use held (can't interpolate meaningfully)
    case TypeId::Int:
    case TypeId::UInt:
    case TypeId::Int64:
    case TypeId::UInt64:
    case TypeId::Bool:
      return Value(a);  // Return copy of 'a' (held)

    // Float2
    case TypeId::Float2:
    case TypeId::Texcoord2f: {
      const float* va = a.as_float2();
      const float* vb = b.as_float2();
      if (va && vb) {
        float result[2];
        LerpFloatN(result, va, vb, tf, 2);
        return Value::MakeFromRaw(type, result);
      }
      break;
    }

    // Float3
    case TypeId::Float3:
    case TypeId::Point3f:
    case TypeId::Vector3f:
    case TypeId::Normal3f:
    case TypeId::Color3f:
    case TypeId::Texcoord3f: {
      const float* va = a.as_float3();
      const float* vb = b.as_float3();
      if (va && vb) {
        float result[3];
        LerpFloatN(result, va, vb, tf, 3);
        return Value::MakeFromRaw(type, result);
      }
      break;
    }

    // Float4
    case TypeId::Float4:
    case TypeId::Color4f: {
      const float* va = a.as_float4();
      const float* vb = b.as_float4();
      if (va && vb) {
        float result[4];
        LerpFloatN(result, va, vb, tf, 4);
        return Value::MakeFromRaw(type, result);
      }
      break;
    }

    // Double2
    case TypeId::Double2:
    case TypeId::Texcoord2d: {
      const double* va = a.as_double2();
      const double* vb = b.as_double2();
      if (va && vb) {
        double result[2];
        LerpDoubleN(result, va, vb, t, 2);
        return Value::MakeFromRaw(type, result);
      }
      break;
    }

    // Double3
    case TypeId::Double3:
    case TypeId::Point3d:
    case TypeId::Vector3d:
    case TypeId::Normal3d:
    case TypeId::Color3d:
    case TypeId::Texcoord3d: {
      const double* va = a.as_double3();
      const double* vb = b.as_double3();
      if (va && vb) {
        double result[3];
        LerpDoubleN(result, va, vb, t, 3);
        return Value::MakeFromRaw(type, result);
      }
      break;
    }

    // Double4
    case TypeId::Double4:
    case TypeId::Color4d: {
      const double* va = a.as_double4();
      const double* vb = b.as_double4();
      if (va && vb) {
        double result[4];
        LerpDoubleN(result, va, vb, t, 4);
        return Value::MakeFromRaw(type, result);
      }
      break;
    }

    // Quaternions - use slerp
    case TypeId::Quatf: {
      const float* va = a.as_float4();  // Quat stored as 4 floats
      const float* vb = b.as_float4();
      if (va && vb) {
        float result[4];
        SlerpQuatf(result, va, vb, tf);
        return Value::MakeQuatf(result[0], result[1], result[2], result[3]);
      }
      break;
    }

    case TypeId::Quatd: {
      const double* va = a.as_double4();  // Quat stored as 4 doubles
      const double* vb = b.as_double4();
      if (va && vb) {
        double result[4];
        SlerpQuatd(result, va, vb, t);
        return Value::MakeQuatd(result[0], result[1], result[2], result[3]);
      }
      break;
    }

    // Matrix4f - linear interpolation of elements
    // Note: For proper rotation interpolation, should decompose to TRS
    case TypeId::Matrix4f: {
      const float* va = a.as_matrix4f();
      const float* vb = b.as_matrix4f();
      if (va && vb) {
        float result[16];
        LerpFloatN(result, va, vb, tf, 16);
        return Value::MakeMatrix4f(result);
      }
      break;
    }

    // Matrix4d
    case TypeId::Matrix4d:
    case TypeId::Frame4d: {
      const double* va = a.as_matrix4d();
      const double* vb = b.as_matrix4d();
      if (va && vb) {
        double result[16];
        LerpDoubleN(result, va, vb, t, 16);
        return Value::MakeFromRaw(type, result);
      }
      break;
    }

    // Matrix2f
    case TypeId::Matrix2f: {
      const float* va = a.as_matrix2f();
      const float* vb = b.as_matrix2f();
      if (va && vb) {
        float result[4];
        LerpFloatN(result, va, vb, tf, 4);
        return Value::MakeMatrix2f(result);
      }
      break;
    }

    case TypeId::Matrix2d: {
      const double* va = a.as_matrix2d();
      const double* vb = b.as_matrix2d();
      if (va && vb) {
        double result[4];
        LerpDoubleN(result, va, vb, t, 4);
        return Value::MakeMatrix2d(result);
      }
      break;
    }

    // Matrix3f
    case TypeId::Matrix3f: {
      const float* va = a.as_matrix3f();
      const float* vb = b.as_matrix3f();
      if (va && vb) {
        float result[9];
        LerpFloatN(result, va, vb, tf, 9);
        return Value::MakeMatrix3f(result);
      }
      break;
    }

    // Matrix3d
    case TypeId::Matrix3d: {
      const double* va = a.as_matrix3d();
      const double* vb = b.as_matrix3d();
      if (va && vb) {
        double result[9];
        LerpDoubleN(result, va, vb, t, 9);
        return Value::MakeMatrix3d(result);
      }
      break;
    }

    // String types - use held (can't interpolate)
    case TypeId::String:
    case TypeId::Token:
    case TypeId::AssetPath:
      return Value(a);

    default:
      break;
  }

  // Linear mode normatively falls back to held for all other types.
  return Value(a);
}

// ============================================================
// Bracket finding
// ============================================================

std::tuple<size_t, size_t, bool> TimeInterpolator::FindBracket(
    const std::vector<std::pair<double, uint32_t>>& time_offsets,
    double time) {

  if (time_offsets.empty()) {
    return {0, 0, false};
  }

  // Check if before first sample
  if (time <= time_offsets.front().first) {
    bool exact = (time == time_offsets.front().first);
    return {0, 0, exact};
  }

  // Check if after last sample
  if (time >= time_offsets.back().first) {
    size_t last = time_offsets.size() - 1;
    bool exact = (time == time_offsets.back().first);
    return {last, last, exact};
  }

  // Binary search for bracket
  size_t low = 0;
  size_t high = time_offsets.size() - 1;

  while (low < high - 1) {
    size_t mid = (low + high) / 2;
    double mid_time = time_offsets[mid].first;

    if (time == mid_time) {
      return {mid, mid, true};
    } else if (time < mid_time) {
      high = mid;
    } else {
      low = mid;
    }
  }

  // Check for exact match at boundaries
  if (time == time_offsets[low].first) {
    return {low, low, true};
  }
  if (time == time_offsets[high].first) {
    return {high, high, true};
  }

  return {low, high, false};
}

// ============================================================
// High-level interpolation with direct Value vector
// ============================================================

SampleResult TimeInterpolator::Interpolate(
    const std::vector<std::pair<double, Value>>& samples,
    double time,
    TimeInterpolation mode) {

  SampleResult result;

  if (samples.empty()) {
    return result;
  }

  // Single sample
  if (samples.size() == 1) {
    if (samples[0].second.is_block()) {
      result.blocked = true;
      return result;
    }
    result.value = samples[0].second;
    result.success = true;
    result.interpolated = false;
    return result;
  }

  // Find bracket
  size_t low = 0;
  size_t high = samples.size() - 1;

  // Before first
  if (time <= samples.front().first) {
    if (samples.front().second.is_block()) {
      result.blocked = true;
      return result;
    }
    result.value = samples.front().second;
    result.success = true;
    result.interpolated = false;
    return result;
  }

  // After last
  if (time >= samples.back().first) {
    if (samples.back().second.is_block()) {
      result.blocked = true;
      return result;
    }
    result.value = samples.back().second;
    result.success = true;
    result.interpolated = false;
    return result;
  }

  // Binary search
  while (low < high - 1) {
    size_t mid = (low + high) / 2;
    if (time == samples[mid].first) {
      if (samples[mid].second.is_block()) {
        result.blocked = true;
        return result;
      }
      result.value = samples[mid].second;
      result.success = true;
      result.interpolated = false;
      return result;
    } else if (time < samples[mid].first) {
      high = mid;
    } else {
      low = mid;
    }
  }

  // Held interpolation
  if (mode == TimeInterpolation::Held) {
    if (samples[low].second.is_block()) {
      result.blocked = true;
      return result;
    }
    result.value = samples[low].second;
    result.success = true;
    result.interpolated = false;
    return result;
  }

  // Linear interpolation
  if (samples[low].second.is_block() || samples[high].second.is_block()) {
    result.blocked = true;
    return result;
  }
  double t_low = samples[low].first;
  double t_high = samples[high].first;
  double t = (time - t_low) / (t_high - t_low);

  result.value = InterpolateValues(samples[low].second, samples[high].second, t);
  result.success = !result.value.is_empty();
  result.interpolated =
      result.success && IsLinearInterpolatable(samples[low].second.type_id());

  return result;
}

}  // namespace next
}  // namespace lightusd
