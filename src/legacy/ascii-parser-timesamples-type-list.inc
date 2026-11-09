// SPDX-License-Identifier: Apache 2.0
// Shared type list for TimeSamples scalar and array parsing.
// Used by ascii-parser-timesamples.cc and ascii-parser-timesamples-array.cc
// with different PARSE_TYPE macro definitions.
//
// Usage: define PARSE_TYPE(__tyid, __type) before including, #undef after.

  // NOTE: `string` does not support multi-line string.
  PARSE_TYPE(type_id, value::AssetPath)
  PARSE_TYPE(type_id, value::token)
  PARSE_TYPE(type_id, std::string)
  // Boolean
  PARSE_TYPE(type_id, bool)
  // Int types
  PARSE_TYPE(type_id, int32_t)
  PARSE_TYPE(type_id, value::int2)
  PARSE_TYPE(type_id, value::int3)
  PARSE_TYPE(type_id, value::int4)
  // Unsigned int types
  PARSE_TYPE(type_id, uint32_t)
  PARSE_TYPE(type_id, value::uint2)
  PARSE_TYPE(type_id, value::uint3)
  PARSE_TYPE(type_id, value::uint4)
  // Char types (int8_t)
  PARSE_TYPE(type_id, char)
  PARSE_TYPE(type_id, value::char2)
  PARSE_TYPE(type_id, value::char3)
  PARSE_TYPE(type_id, value::char4)
  // Uchar types (uint8_t)
  PARSE_TYPE(type_id, uint8_t)
  PARSE_TYPE(type_id, value::uchar2)
  PARSE_TYPE(type_id, value::uchar3)
  PARSE_TYPE(type_id, value::uchar4)
  // Short types (int16_t)
  PARSE_TYPE(type_id, int16_t)
  PARSE_TYPE(type_id, value::short2)
  PARSE_TYPE(type_id, value::short3)
  PARSE_TYPE(type_id, value::short4)
  // Ushort types (uint16_t)
  PARSE_TYPE(type_id, uint16_t)
  PARSE_TYPE(type_id, value::ushort2)
  PARSE_TYPE(type_id, value::ushort3)
  PARSE_TYPE(type_id, value::ushort4)
  // 64-bit integer types
  PARSE_TYPE(type_id, int64_t)
  PARSE_TYPE(type_id, uint64_t)
  // Half precision types
  PARSE_TYPE(type_id, value::half)
  PARSE_TYPE(type_id, value::half2)
  PARSE_TYPE(type_id, value::half3)
  PARSE_TYPE(type_id, value::half4)
  // Float types
  PARSE_TYPE(type_id, float)
  PARSE_TYPE(type_id, value::float2)
  PARSE_TYPE(type_id, value::float3)
  PARSE_TYPE(type_id, value::float4)
  // Double types
  PARSE_TYPE(type_id, double)
  PARSE_TYPE(type_id, value::double2)
  PARSE_TYPE(type_id, value::double3)
  PARSE_TYPE(type_id, value::double4)
  // Quaternion types
  PARSE_TYPE(type_id, value::quath)
  PARSE_TYPE(type_id, value::quatf)
  PARSE_TYPE(type_id, value::quatd)
  // Color types (half precision)
  PARSE_TYPE(type_id, value::color3h)
  PARSE_TYPE(type_id, value::color4h)
  // Color types (float precision)
  PARSE_TYPE(type_id, value::color3f)
  PARSE_TYPE(type_id, value::color4f)
  // Color types (double precision)
  PARSE_TYPE(type_id, value::color3d)
  PARSE_TYPE(type_id, value::color4d)
  // Vector types
  PARSE_TYPE(type_id, value::vector3h)
  PARSE_TYPE(type_id, value::vector3f)
  PARSE_TYPE(type_id, value::vector3d)
  // Normal types
  PARSE_TYPE(type_id, value::normal3h)
  PARSE_TYPE(type_id, value::normal3f)
  PARSE_TYPE(type_id, value::normal3d)
  // Point types
  PARSE_TYPE(type_id, value::point3h)
  PARSE_TYPE(type_id, value::point3f)
  PARSE_TYPE(type_id, value::point3d)
  // Texcoord types
  PARSE_TYPE(type_id, value::texcoord2h)
  PARSE_TYPE(type_id, value::texcoord2f)
  PARSE_TYPE(type_id, value::texcoord2d)
  PARSE_TYPE(type_id, value::texcoord3h)
  PARSE_TYPE(type_id, value::texcoord3f)
  PARSE_TYPE(type_id, value::texcoord3d)
  // Matrix types (float)
  PARSE_TYPE(type_id, value::matrix2f)
  PARSE_TYPE(type_id, value::matrix3f)
  PARSE_TYPE(type_id, value::matrix4f)
  // Matrix types (double)
  PARSE_TYPE(type_id, value::matrix2d)
  PARSE_TYPE(type_id, value::matrix3d)
  PARSE_TYPE(type_id, value::matrix4d)
  // Frame type (same as matrix4d)
  PARSE_TYPE(type_id, value::frame4d)
