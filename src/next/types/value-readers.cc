// SPDX-License-Identifier: Apache-2.0
#include "value.hh"
#include "type-info.hh"
#include <cstring>
namespace lightusd { namespace next {
static const void* PodData(const lu_value& v) { return lu_value_data(&v); }
static bool Boxed(const lu_value& v) { return v.kind == LU_VALUE_BOX; }
const bool* ValueView::as_bool() const {
  if (!core_) return nullptr; 
  if (static_cast<TypeId>(core_->type) != TypeId::Bool || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const bool* >(core_->store.bytes);
}

const int32_t* ValueView::as_int() const {
  if (!core_) return nullptr; 
  if (static_cast<TypeId>(core_->type) != TypeId::Int || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const int32_t* >(core_->store.bytes);
}

const uint32_t* ValueView::as_uint() const {
  if (!core_) return nullptr; 
  if (static_cast<TypeId>(core_->type) != TypeId::UInt || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const uint32_t* >(core_->store.bytes);
}

const int64_t* ValueView::as_int64() const {
  if (!core_) return nullptr; 
  if (static_cast<TypeId>(core_->type) != TypeId::Int64 || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const int64_t* >(core_->store.bytes);
}

const uint64_t* ValueView::as_uint64() const {
  if (!core_) return nullptr; 
  if (static_cast<TypeId>(core_->type) != TypeId::UInt64 || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const uint64_t* >(core_->store.bytes);
}

const float* ValueView::as_float() const {
  if (!core_) return nullptr; 
  if (static_cast<TypeId>(core_->type) != TypeId::Float || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const float* >(core_->store.bytes);
}

const uint8_t* ValueView::as_uchar() const {
  if (!core_) return nullptr; 
  if (static_cast<TypeId>(core_->type) != TypeId::UChar || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const uint8_t* >(core_->store.bytes);
}

const double* ValueView::as_double() const {
  if (!core_) return nullptr;
  if ((static_cast<TypeId>(core_->type) != TypeId::Double && static_cast<TypeId>(core_->type) != TypeId::TimeCode) ||
      (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) {
    return nullptr;
  }
  return reinterpret_cast<const double*>(PodData(*core_));
}

const int32_t* ValueView::as_int2() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Int2 || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const int32_t*>(PodData(*core_));
}

const int32_t* ValueView::as_int3() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Int3 || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const int32_t*>(PodData(*core_));
}

const int32_t* ValueView::as_int4() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Int4 || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const int32_t*>(PodData(*core_));
}

const float* ValueView::as_float2() const {
  if (!core_) return nullptr;
  if ((static_cast<TypeId>(core_->type) != TypeId::Float2 && static_cast<TypeId>(core_->type) != TypeId::Texcoord2f) ||
      (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const float*>(PodData(*core_));
}

const float* ValueView::as_float3() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Float3 && static_cast<TypeId>(core_->type) != TypeId::Point3f &&
      static_cast<TypeId>(core_->type) != TypeId::Vector3f && static_cast<TypeId>(core_->type) != TypeId::Normal3f &&
      static_cast<TypeId>(core_->type) != TypeId::Color3f && static_cast<TypeId>(core_->type) != TypeId::Texcoord3f)
    return nullptr;
  if ((core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const float*>(PodData(*core_));
}

const float* ValueView::as_float4() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Float4 && static_cast<TypeId>(core_->type) != TypeId::Quatf &&
      static_cast<TypeId>(core_->type) != TypeId::Color4f) return nullptr;
  if ((core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const float*>(PodData(*core_));
}

const double* ValueView::as_double2() const {
  if (!core_) return nullptr;
  if ((static_cast<TypeId>(core_->type) != TypeId::Double2 && static_cast<TypeId>(core_->type) != TypeId::Texcoord2d) ||
      (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const double*>(PodData(*core_));
}

const double* ValueView::as_double3() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Double3 && static_cast<TypeId>(core_->type) != TypeId::Point3d &&
      static_cast<TypeId>(core_->type) != TypeId::Vector3d && static_cast<TypeId>(core_->type) != TypeId::Normal3d &&
      static_cast<TypeId>(core_->type) != TypeId::Color3d && static_cast<TypeId>(core_->type) != TypeId::Texcoord3d)
    return nullptr;
  if ((core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const double*>(PodData(*core_));
}

const double* ValueView::as_double4() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Double4 && static_cast<TypeId>(core_->type) != TypeId::Quatd &&
      static_cast<TypeId>(core_->type) != TypeId::Color4d) return nullptr;
  if ((core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const double*>(PodData(*core_));
}

const float* ValueView::as_matrix2f() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Matrix2f || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const float*>(PodData(*core_));
}

const float* ValueView::as_matrix3f() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Matrix3f || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const float*>(PodData(*core_));
}

const float* ValueView::as_matrix4f() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Matrix4f || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  if (Boxed(*core_)) return reinterpret_cast<const float*>(  PodData(*core_));
  return reinterpret_cast<const float*>(PodData(*core_));
}

const double* ValueView::as_matrix2d() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Matrix2d || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const double*>(PodData(*core_));
}

const double* ValueView::as_matrix3d() const {
  if (!core_) return nullptr;
  if (static_cast<TypeId>(core_->type) != TypeId::Matrix3d || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return nullptr;
  return reinterpret_cast<const double*>(PodData(*core_));
}

const double* ValueView::as_matrix4d() const {
  if (!core_) return nullptr;
  // frame4d is a matrix4d role type (same double[16] storage).
  if ((static_cast<TypeId>(core_->type) != TypeId::Matrix4d && static_cast<TypeId>(core_->type) != TypeId::Frame4d) ||
      (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) {
    return nullptr;
  }
  if (Boxed(*core_)) return reinterpret_cast<const double*>(  PodData(*core_));
  return reinterpret_cast<const double*>(PodData(*core_));
}

bool ValueView::ToFloatLanes(int lanes, float* out) const {
  if (!core_) return false;
  if (!out || (core_->kind == LU_VALUE_ARRAY || core_->kind == LU_VALUE_LAZY_ARRAY)) return false;
  if (lu_scalar_component(core_->type) == LU_TYPE_HALF && static_cast<int>(GetComponentCount(type_id())) == lanes) {
    const uint16_t* bits = reinterpret_cast<const uint16_t*>(PodData(*core_));
    for (int i = 0; i < lanes; ++i) out[i] = lu_half_to_float(bits[i]);
    return true;
  }
  switch (lanes) {
    case 1:
      if (const float* f = as_float()) { out[0] = *f; return true; }
      if (const double* d = as_double()) { out[0] = static_cast<float>(*d); return true; }
      return false;
    case 2:
      if (const float* f = as_float2()) { out[0] = f[0]; out[1] = f[1]; return true; }
      if (const double* d = as_double2()) {
        for (int i = 0; i < 2; ++i) out[i] = static_cast<float>(d[i]);
        return true;
      }
      return false;
    case 3:
      if (const float* f = as_float3()) {
        for (int i = 0; i < 3; ++i) out[i] = f[i];
        return true;
      }
      if (const double* d = as_double3()) {
        for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(d[i]);
        return true;
      }
      return false;
    case 4:
      if (const float* f = as_float4()) {
        for (int i = 0; i < 4; ++i) out[i] = f[i];
        return true;
      }
      if (const double* d = as_double4()) {
        for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(d[i]);
        return true;
      }
      return false;
    default:
      return false;
  }
}

bool ValueView::to_float(float* out) const { return ToFloatLanes(1, out); }

bool ValueView::to_float2(float* out) const { return ToFloatLanes(2, out); }

bool ValueView::to_float3(float* out) const { return ToFloatLanes(3, out); }

bool ValueView::to_float4(float* out) const { return ToFloatLanes(4, out); }
}}
