// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// value::Value per-type switch operations — split out of value-types.cc.
// Holds the wide switch-on-type_id functions (one arm per ~90 value types):
// Value::array_size, RoleTypeCast, UpcastType, GetTypeSize, and
// Value::estimate_{memory,actual}_usage. These dominate value-types.cc codegen;
// isolating them shortens the build critical path. All public entry points are
// declared in value-types.hh; GetTypeSize is a file-local helper used by the
// estimate_* methods and travels with them.
#include "value-types.hh"
#include "timesamples.hh"  // TimeSamples::has_sample_at (moved with this block)

#include "str-util.hh"     // endsWith (UpcastType)
#include "common-macros.inc"
#include "math-util.inc"

namespace lightusd {
namespace value {

size_t Value::array_size() const {
  if (!is_array()) {
    return 0;
  }

  // primvar types only.

#define APPLY_FUNC_TO_TYPES(__FUNC) \
  __FUNC(bool)                 \
  __FUNC(value::token)                 \
  __FUNC(std::string)                 \
  __FUNC(StringData)                 \
  __FUNC(half)                 \
  __FUNC(half2)                \
  __FUNC(half3)                \
  __FUNC(half4)                \
  __FUNC(int32_t)              \
  __FUNC(uint32_t)             \
  __FUNC(int2)                 \
  __FUNC(int3)                 \
  __FUNC(int4)                 \
  __FUNC(uint2)                \
  __FUNC(uint3)                \
  __FUNC(uint4)                \
  __FUNC(int64_t)              \
  __FUNC(uint64_t)             \
  __FUNC(float)                \
  __FUNC(float2)               \
  __FUNC(float3)               \
  __FUNC(float4)               \
  __FUNC(double)               \
  __FUNC(double2)              \
  __FUNC(double3)              \
  __FUNC(double4)              \
  __FUNC(quath)                \
  __FUNC(quatf)                \
  __FUNC(quatd)                \
  __FUNC(normal3h)             \
  __FUNC(normal3f)             \
  __FUNC(normal3d)             \
  __FUNC(vector3h)             \
  __FUNC(vector3f)             \
  __FUNC(vector3d)             \
  __FUNC(point3h)              \
  __FUNC(point3f)              \
  __FUNC(point3d)              \
  __FUNC(color3f)              \
  __FUNC(color3d)              \
  __FUNC(color4h)              \
  __FUNC(color4f)              \
  __FUNC(color4d)              \
  __FUNC(texcoord2h)           \
  __FUNC(texcoord2f)           \
  __FUNC(texcoord2d)           \
  __FUNC(texcoord3h)           \
  __FUNC(texcoord3f)           \
  __FUNC(texcoord3d) \
  __FUNC(matrix2d) \
  __FUNC(matrix3d) \
  __FUNC(matrix4d) \
  __FUNC(frame4d) \
  __FUNC(AssetPath)

#define ARRAY_SIZE_GET(__ty) case value::TypeTraits<__ty>::type_id() | value::TYPE_ID_1D_ARRAY_BIT: { \
    if (auto pv = v_.cast<std::vector<__ty>>()) { \
      return pv->size(); \
    } \
    return 0; \
  }


  switch (v_.type_id()) {
    APPLY_FUNC_TO_TYPES(ARRAY_SIZE_GET)
    default:
      return 0;
  }

#undef ARRAY_SIZE_GET
#undef APPLY_FUNC_TO_TYPES

}

//
// Compile-time validation for safe role type casting.
// These static_asserts ensure that the zero-copy cast is safe:
//   1. Both types must have the same size
//   2. Both types must have the same alignment
//   3. Both types must be trivially copyable (standard layout)
//
#define VALIDATE_ROLE_TYPE_CAST(__roleTy, __srcBaseTy)                         \
  static_assert(sizeof(__roleTy) == sizeof(__srcBaseTy),                       \
                "Role type and base type must have same size");                \
  static_assert(alignof(__roleTy) == alignof(__srcBaseTy),                     \
                "Role type and base type must have same alignment");           \
  static_assert(std::is_trivially_copyable<__roleTy>::value,                   \
                "Role type must be trivially copyable");                       \
  static_assert(std::is_trivially_copyable<__srcBaseTy>::value,                \
                "Base type must be trivially copyable");

// Validate all supported role type cast combinations at compile time
// texcoord types
VALIDATE_ROLE_TYPE_CAST(value::texcoord2h, value::half2)
VALIDATE_ROLE_TYPE_CAST(value::texcoord2f, value::float2)
VALIDATE_ROLE_TYPE_CAST(value::texcoord2d, value::double2)
VALIDATE_ROLE_TYPE_CAST(value::texcoord3h, value::half3)
VALIDATE_ROLE_TYPE_CAST(value::texcoord3f, value::float3)
VALIDATE_ROLE_TYPE_CAST(value::texcoord3d, value::double3)

// normal types
VALIDATE_ROLE_TYPE_CAST(value::normal3h, value::half3)
VALIDATE_ROLE_TYPE_CAST(value::normal3f, value::float3)
VALIDATE_ROLE_TYPE_CAST(value::normal3d, value::double3)

// vector types
VALIDATE_ROLE_TYPE_CAST(value::vector3h, value::half3)
VALIDATE_ROLE_TYPE_CAST(value::vector3f, value::float3)
VALIDATE_ROLE_TYPE_CAST(value::vector3d, value::double3)

// point types
VALIDATE_ROLE_TYPE_CAST(value::point3h, value::half3)
VALIDATE_ROLE_TYPE_CAST(value::point3f, value::float3)
VALIDATE_ROLE_TYPE_CAST(value::point3d, value::double3)

// color types
VALIDATE_ROLE_TYPE_CAST(value::color3h, value::half3)
VALIDATE_ROLE_TYPE_CAST(value::color3f, value::float3)
VALIDATE_ROLE_TYPE_CAST(value::color3d, value::double3)
VALIDATE_ROLE_TYPE_CAST(value::color4h, value::half4)
VALIDATE_ROLE_TYPE_CAST(value::color4f, value::float4)
VALIDATE_ROLE_TYPE_CAST(value::color4d, value::double4)

// frame type
VALIDATE_ROLE_TYPE_CAST(value::frame4d, value::matrix4d)

#undef VALIDATE_ROLE_TYPE_CAST

bool RoleTypeCast(const uint32_t roleTyId, value::Value &inout) {
  const uint32_t srcUnderlyingTyId = inout.underlying_type_id();

  DCOUT("input type = " << inout.type_name());

  // Zero-copy role type cast: just change the type_id.
  // This works because role types have identical memory layout to their base types.
  // The compile-time validation above ensures this is always safe.
#define ROLE_TYPE_CAST(__roleTy, __srcBaseTy)                                  \
  {                                                                            \
    static_assert(value::TypeTraits<__roleTy>::size() ==                       \
                      value::TypeTraits<__srcBaseTy>::size(),                  \
                  "Role type and base type must have same size");              \
    if (srcUnderlyingTyId == value::TypeTraits<__srcBaseTy>::type_id()) {      \
      if (roleTyId == value::TypeTraits<__roleTy>::type_id()) {                \
        inout.get_raw_mutable().unsafe_reinterpret_as<__roleTy>();             \
        return true;                                                           \
      }                                                                        \
    } else if (srcUnderlyingTyId ==                                            \
               (value::TypeTraits<__srcBaseTy>::type_id() |                    \
                value::TYPE_ID_1D_ARRAY_BIT)) {                                \
      if (roleTyId == value::TypeTraits<std::vector<__roleTy>>::type_id()) {   \
        inout.get_raw_mutable().unsafe_reinterpret_as<std::vector<__roleTy>>();\
        return true;                                                           \
      }                                                                        \
    }                                                                          \
  }

  ROLE_TYPE_CAST(value::texcoord2h, value::half2)
  ROLE_TYPE_CAST(value::texcoord2f, value::float2)
  ROLE_TYPE_CAST(value::texcoord2d, value::double2)

  ROLE_TYPE_CAST(value::texcoord3h, value::half3)
  ROLE_TYPE_CAST(value::texcoord3f, value::float3)
  ROLE_TYPE_CAST(value::texcoord3d, value::double3)

  ROLE_TYPE_CAST(value::normal3h, value::half3)
  ROLE_TYPE_CAST(value::normal3f, value::float3)
  ROLE_TYPE_CAST(value::normal3d, value::double3)

  ROLE_TYPE_CAST(value::vector3h, value::half3)
  ROLE_TYPE_CAST(value::vector3f, value::float3)
  ROLE_TYPE_CAST(value::vector3d, value::double3)

  ROLE_TYPE_CAST(value::point3h, value::half3)
  ROLE_TYPE_CAST(value::point3f, value::float3)
  ROLE_TYPE_CAST(value::point3d, value::double3)

  ROLE_TYPE_CAST(value::color3h, value::half3)
  ROLE_TYPE_CAST(value::color3f, value::float3)
  ROLE_TYPE_CAST(value::color3d, value::double3)

  ROLE_TYPE_CAST(value::color4h, value::half4)
  ROLE_TYPE_CAST(value::color4f, value::float4)
  ROLE_TYPE_CAST(value::color4d, value::double4)

  ROLE_TYPE_CAST(value::frame4d, value::matrix4d)

#undef ROLE_TYPE_CAST

  return false;
}

// TODO: Use template
bool UpcastType(const std::string &reqType, value::Value &inout) {
  // `reqType` may be Role type. Get underlying type
  uint32_t tyid;
  if (auto pv = value::TryGetUnderlyingTypeId(reqType)) {
    tyid = pv.value();
  } else {
    // Invalid reqType.
    return false;
  }

  bool reqTypeArray = false;
  //uint32_t baseReqTyId;
  DCOUT("UpcastType trial: reqTy : " << reqType
                                     << ", valtype = " << inout.type_name());

  if (endsWith(reqType, "[]")) {
    reqTypeArray = true;
    //baseReqTyId = value::GetTypeId(removeSuffix(reqType, "[]"));
  } else {
    //baseReqTyId = value::GetTypeId(reqType);
  }
  DCOUT("is array: " << reqTypeArray);

  // For array
  if (reqTypeArray) {
  } else {
    if (tyid == value::TYPE_ID_FLOAT) {
      float dst;
      if (auto pv = inout.get_value<value::half>()) {
        dst = half_to_float(pv.value());
        inout = dst;
        return true;
      }
    } else if (tyid == value::TYPE_ID_FLOAT2) {
      if (auto pv = inout.get_value<value::half2>()) {
        value::float2 dst;
        value::half2 v = pv.value();
        dst[0] = half_to_float(v[0]);
        dst[1] = half_to_float(v[1]);
        inout = dst;
        return true;
      }

    } else if (tyid == value::TYPE_ID_FLOAT3) {
      value::float3 dst;
      if (auto pv = inout.get_value<value::half3>()) {
        value::half3 v = pv.value();
        dst[0] = half_to_float(v[0]);
        dst[1] = half_to_float(v[1]);
        dst[2] = half_to_float(v[2]);
        inout = dst;
        return true;
      }
    } else if (tyid == value::TYPE_ID_FLOAT4) {
      value::float4 dst;
      if (auto pv = inout.get_value<value::half4>()) {
        value::half4 v = pv.value();
        dst[0] = half_to_float(v[0]);
        dst[1] = half_to_float(v[1]);
        dst[2] = half_to_float(v[2]);
        dst[3] = half_to_float(v[3]);
        inout = dst;
        return true;
      }
    } else if (tyid == value::TYPE_ID_DOUBLE) {
      double dst;
      if (auto pv = inout.get_value<value::half>()) {
        dst = double(half_to_float(pv.value()));
        inout = dst;
        return true;
      }
    } else if (tyid == value::TYPE_ID_DOUBLE2) {
      value::double2 dst;
      if (auto pv = inout.get_value<value::half2>()) {
        value::half2 v = pv.value();
        dst[0] = double(half_to_float(v[0]));
        dst[1] = double(half_to_float(v[1]));
        inout = dst;
        return true;
      }
    } else if (tyid == value::TYPE_ID_DOUBLE3) {
      value::double3 dst;
      if (auto pv = inout.get_value<value::half3>()) {
        value::half3 v = pv.value();
        dst[0] = double(half_to_float(v[0]));
        dst[1] = double(half_to_float(v[1]));
        dst[2] = double(half_to_float(v[2]));
        inout = dst;
        return true;
      }
    } else if (tyid == value::TYPE_ID_DOUBLE4) {
      value::double4 dst;
      if (auto pv = inout.get_value<value::half4>()) {
        value::half4 v = pv.value();
        dst[0] = double(half_to_float(v[0]));
        dst[1] = double(half_to_float(v[1]));
        dst[2] = double(half_to_float(v[2]));
        dst[3] = double(half_to_float(v[3]));
        inout = dst;
        return true;
      }
    }
  }

  return false;
}


// Get byte size for a given type_id
static size_t GetTypeSize(uint32_t type_id) {
  // Remove array bit if present
  uint32_t base_type_id = type_id & (~TYPE_ID_1D_ARRAY_BIT);

  // Create a compile-time lookup table using switch
  switch (base_type_id) {
    // Primitives
    case TYPE_ID_BOOL: return sizeof(bool);
    case TYPE_ID_CHAR: return sizeof(char);
    case TYPE_ID_CHAR2: return sizeof(char) * 2;
    case TYPE_ID_CHAR3: return sizeof(char) * 3;
    case TYPE_ID_CHAR4: return sizeof(char) * 4;

    // Half precision
    case TYPE_ID_HALF: return sizeof(half);
    case TYPE_ID_HALF2: return sizeof(half) * 2;
    case TYPE_ID_HALF3: return sizeof(half) * 3;
    case TYPE_ID_HALF4: return sizeof(half) * 4;

    // Integers
    case TYPE_ID_INT32: return sizeof(int32_t);
    case TYPE_ID_INT2: return sizeof(int32_t) * 2;
    case TYPE_ID_INT3: return sizeof(int32_t) * 3;
    case TYPE_ID_INT4: return sizeof(int32_t) * 4;
    case TYPE_ID_INT64: return sizeof(int64_t);

    // Unsigned integers
    case TYPE_ID_UCHAR: return sizeof(uint8_t);
    case TYPE_ID_UCHAR2: return sizeof(uint8_t) * 2;
    case TYPE_ID_UCHAR3: return sizeof(uint8_t) * 3;
    case TYPE_ID_UCHAR4: return sizeof(uint8_t) * 4;
    case TYPE_ID_UINT32: return sizeof(uint32_t);
    case TYPE_ID_UINT2: return sizeof(uint32_t) * 2;
    case TYPE_ID_UINT3: return sizeof(uint32_t) * 3;
    case TYPE_ID_UINT4: return sizeof(uint32_t) * 4;
    case TYPE_ID_UINT64: return sizeof(uint64_t);

    // Short integers
    case TYPE_ID_SHORT: return sizeof(int16_t);
    case TYPE_ID_SHORT2: return sizeof(int16_t) * 2;
    case TYPE_ID_SHORT3: return sizeof(int16_t) * 3;
    case TYPE_ID_SHORT4: return sizeof(int16_t) * 4;
    case TYPE_ID_USHORT: return sizeof(uint16_t);
    case TYPE_ID_USHORT2: return sizeof(uint16_t) * 2;
    case TYPE_ID_USHORT3: return sizeof(uint16_t) * 3;
    case TYPE_ID_USHORT4: return sizeof(uint16_t) * 4;

    // Floats
    case TYPE_ID_FLOAT: return sizeof(float);
    case TYPE_ID_FLOAT2: return sizeof(float) * 2;
    case TYPE_ID_FLOAT3: return sizeof(float) * 3;
    case TYPE_ID_FLOAT4: return sizeof(float) * 4;

    // Doubles
    case TYPE_ID_DOUBLE: return sizeof(double);
    case TYPE_ID_DOUBLE2: return sizeof(double) * 2;
    case TYPE_ID_DOUBLE3: return sizeof(double) * 3;
    case TYPE_ID_DOUBLE4: return sizeof(double) * 4;

    // Quaternions
    case TYPE_ID_QUATH: return sizeof(half) * 4;
    case TYPE_ID_QUATF: return sizeof(float) * 4;
    case TYPE_ID_QUATD: return sizeof(double) * 4;

    // Matrices
    case TYPE_ID_MATRIX2F: return sizeof(float) * 4;   // 2x2
    case TYPE_ID_MATRIX3F: return sizeof(float) * 9;   // 3x3
    case TYPE_ID_MATRIX4F: return sizeof(float) * 16;  // 4x4
    case TYPE_ID_MATRIX2D: return sizeof(double) * 4;  // 2x2
    case TYPE_ID_MATRIX3D: return sizeof(double) * 9;  // 3x3
    case TYPE_ID_MATRIX4D: return sizeof(double) * 16; // 4x4

    // Colors (role types - same memory as their underlying types)
    case TYPE_ID_COLOR3H: return sizeof(half) * 3;
    case TYPE_ID_COLOR3F: return sizeof(float) * 3;
    case TYPE_ID_COLOR3D: return sizeof(double) * 3;
    case TYPE_ID_COLOR4H: return sizeof(half) * 4;
    case TYPE_ID_COLOR4F: return sizeof(float) * 4;
    case TYPE_ID_COLOR4D: return sizeof(double) * 4;

    // Points (role types)
    case TYPE_ID_POINT3H: return sizeof(half) * 3;
    case TYPE_ID_POINT3F: return sizeof(float) * 3;
    case TYPE_ID_POINT3D: return sizeof(double) * 3;

    // Normals (role types)
    case TYPE_ID_NORMAL3H: return sizeof(half) * 3;
    case TYPE_ID_NORMAL3F: return sizeof(float) * 3;
    case TYPE_ID_NORMAL3D: return sizeof(double) * 3;

    // Vectors (role types)
    case TYPE_ID_VECTOR3H: return sizeof(half) * 3;
    case TYPE_ID_VECTOR3F: return sizeof(float) * 3;
    case TYPE_ID_VECTOR3D: return sizeof(double) * 3;

    // Texture coordinates (role types)
    case TYPE_ID_TEXCOORD2H: return sizeof(half) * 2;
    case TYPE_ID_TEXCOORD2F: return sizeof(float) * 2;
    case TYPE_ID_TEXCOORD2D: return sizeof(double) * 2;
    case TYPE_ID_TEXCOORD3H: return sizeof(half) * 3;
    case TYPE_ID_TEXCOORD3F: return sizeof(float) * 3;
    case TYPE_ID_TEXCOORD3D: return sizeof(double) * 3;

    // Special types
    case TYPE_ID_FRAME4D: return sizeof(double) * 16; // 4x4 matrix
    case TYPE_ID_EXTENT: return sizeof(float) * 6;    // float3[2]
    case TYPE_ID_TIMECODE: return sizeof(double);

    // String/token types - estimate with typical sizes
    case TYPE_ID_TOKEN: return 32;  // Estimate for typical token string
    case TYPE_ID_STRING: return 64; // Estimate for typical string
    case TYPE_ID_STRING_DATA: return 64; // Estimate for string data
    case TYPE_ID_ASSET_PATH: return 128; // Estimate for asset paths

    // Special values
    case TYPE_ID_VOID: return 0;
    case TYPE_ID_NULL: return 0;
    case TYPE_ID_MONOSTATE: return 0;
    case TYPE_ID_VALUEBLOCK: return 0;

    // Complex types - return base struct size
    case TYPE_ID_DICT: return sizeof(void*) * 2; // Rough estimate for map overhead
    case TYPE_ID_CUSTOMDATA: return sizeof(void*) * 2;

    // Default for unknown types
    default: return sizeof(void*); // Pointer size as fallback
  }
}

size_t Value::estimate_memory_usage() const {
  size_t total_size = sizeof(Value); // Base object size

  if (is_empty() || is_none()) {
    return total_size;
  }

  uint32_t tid = type_id();

  // Check if it's an array type
  if (tid & TYPE_ID_1D_ARRAY_BIT) {
    // For arrays, compute element size * array count
    size_t element_size = GetTypeSize(tid);
    size_t element_count = array_size();

    // Add array storage overhead (vector typically has 3 pointers)
    total_size += sizeof(void*) * 3;

    // Add actual data size
    total_size += element_size * element_count;

    // Handle special cases for string arrays
    uint32_t base_type = tid & (~TYPE_ID_1D_ARRAY_BIT);
    if (base_type == TYPE_ID_STRING || base_type == TYPE_ID_TOKEN ||
        base_type == TYPE_ID_STRING_DATA || base_type == TYPE_ID_ASSET_PATH) {
      // For string arrays, add estimated string sizes
      if (auto* vec = as<std::vector<std::string>>()) {
        for (const auto& str : *vec) {
          total_size += str.capacity();
        }
      } else if (auto* tokVec = as<std::vector<value::token>>()) {
        for (const auto& tok : *tokVec) {
          total_size += tok.str().capacity();
        }
      }
    }
  } else {
    // For scalar types
    size_t type_size = GetTypeSize(tid);

    // For MODEL types (concrete Prim types like GeomMesh, Xform, etc.),
    // GetTypeSize returns sizeof(void*) which is wrong.
    // Use sizeof_stored() which records the actual sizeof(T) at construction time.
    if (tid >= TYPE_ID_MODEL_BEGIN && tid < TYPE_ID_MODEL_END) {
      size_t stored_size = sizeof_stored();
      if (stored_size > 0) {
        type_size = stored_size;
      }
    }

    total_size += type_size;

    // Handle dynamic string types specially
    if (tid == TYPE_ID_STRING || tid == TYPE_ID_STRING_DATA) {
      if (auto* str = as<std::string>()) {
        total_size += str->capacity();
      }
    } else if (tid == TYPE_ID_TOKEN) {
      if (auto* tok = as<value::token>()) {
        total_size += tok->str().capacity();
      }
    } else if (tid == TYPE_ID_ASSET_PATH) {
      if (auto* path = as<value::AssetPath>()) {
        total_size += path->GetAssetPath().length();
        total_size += path->GetResolvedPath().length();
      }
    } else if (tid == TYPE_ID_DICT || tid == TYPE_ID_CUSTOMDATA) {
      // For dictionary types, estimate based on typical usage
      if (auto* dict = as<value::dict>()) {
        // Map overhead + estimated key/value sizes
        total_size += dict->size() * (32 + sizeof(void*) * 4);
        // Recursively compute values (simplified - just add base estimates)
        for (const auto& kv : *dict) {
          total_size += kv.first.capacity();
          // For values, use a rough estimate
          total_size += 64; // Average value size estimate
        }
      }
    }
  }

  return total_size;
}

size_t Value::estimate_actual_usage() const {
  size_t total_size = sizeof(Value);

  if (is_empty() || is_none()) {
    return total_size;
  }

  uint32_t tid = type_id();

  if (tid & TYPE_ID_1D_ARRAY_BIT) {
    size_t element_size = GetTypeSize(tid);
    size_t element_count = array_size();

    total_size += sizeof(void*) * 3;  // vector overhead
    total_size += element_size * element_count;

    // For string arrays, use size() instead of capacity()
    uint32_t base_type = tid & (~TYPE_ID_1D_ARRAY_BIT);
    if (base_type == TYPE_ID_STRING || base_type == TYPE_ID_TOKEN ||
        base_type == TYPE_ID_STRING_DATA || base_type == TYPE_ID_ASSET_PATH) {
      if (auto* vec = as<std::vector<std::string>>()) {
        for (const auto& str : *vec) {
          total_size += str.size();
        }
      } else if (auto* tokVec = as<std::vector<value::token>>()) {
        for (const auto& tok : *tokVec) {
          total_size += tok.str().size();
        }
      }
    }
  } else {
    size_t type_size = GetTypeSize(tid);
    if (tid >= TYPE_ID_MODEL_BEGIN && tid < TYPE_ID_MODEL_END) {
      size_t stored_size = sizeof_stored();
      if (stored_size > 0) {
        type_size = stored_size;
      }
    }
    total_size += type_size;

    if (tid == TYPE_ID_STRING || tid == TYPE_ID_STRING_DATA) {
      if (auto* str = as<std::string>()) {
        total_size += str->size();
      }
    } else if (tid == TYPE_ID_TOKEN) {
      if (auto* tok = as<value::token>()) {
        total_size += tok->str().size();
      }
    } else if (tid == TYPE_ID_ASSET_PATH) {
      if (auto* path = as<value::AssetPath>()) {
        total_size += path->GetAssetPath().length();
        total_size += path->GetResolvedPath().length();
      }
    } else if (tid == TYPE_ID_DICT || tid == TYPE_ID_CUSTOMDATA) {
      if (auto* dict = as<value::dict>()) {
        total_size += dict->size() * (32 + sizeof(void*) * 4);
        for (const auto& kv : *dict) {
          total_size += kv.first.size();
          total_size += 64;
        }
      }
    }
  }

  return total_size;
}

bool TimeSamples::has_sample_at(const double t) const {
  const auto &samples = get_samples();

  const auto it = std::find_if(samples.begin(), samples.end(), [&t](const Sample &s) {
    return math::is_close(t, s.t);
  });

  return (it != samples.end());
}

// Floating-point aware equality operators for matrix types
// Use epsilon-based comparison suitable for deduplication
bool operator==(const matrix2f &a, const matrix2f &b) {
  return math::is_close(a.m[0][0], b.m[0][0]) &&
         math::is_close(a.m[0][1], b.m[0][1]) &&
         math::is_close(a.m[1][0], b.m[1][0]) &&
         math::is_close(a.m[1][1], b.m[1][1]);
}

} // namespace value
} // namespace lightusd
