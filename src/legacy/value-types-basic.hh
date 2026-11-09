// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Basic USD value type definitions (lightweight header)
//
// This header provides only the basic type definitions without templates,
// type traits, or utility functions. Include this instead of value-types.hh
// when you only need type declarations.
//
// For full functionality (TypeTraits, Value class, etc.), include value-types.hh
//
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace lightusd {

// Forward declaration
class Token;

namespace value {

// ============================================================================
// Type name constants
// ============================================================================

constexpr auto kToken = "token";
constexpr auto kString = "string";
constexpr auto kPath = "Path";
constexpr auto kAssetPath = "asset";
constexpr auto kDictionary = "dictionary";
constexpr auto kTimeCode = "timecode";

constexpr auto kBool = "bool";
constexpr auto kChar = "char";
constexpr auto kUChar = "uchar";
constexpr auto kHalf = "half";
constexpr auto kInt = "int";
constexpr auto kUInt = "uint";
constexpr auto kInt64 = "int64";
constexpr auto kUInt64 = "uint64";
constexpr auto kFloat = "float";
constexpr auto kDouble = "double";

constexpr auto kInt2 = "int2";
constexpr auto kInt3 = "int3";
constexpr auto kInt4 = "int4";
constexpr auto kUInt2 = "uint2";
constexpr auto kUInt3 = "uint3";
constexpr auto kUInt4 = "uint4";

constexpr auto kHalf2 = "half2";
constexpr auto kHalf3 = "half3";
constexpr auto kHalf4 = "half4";
constexpr auto kFloat2 = "float2";
constexpr auto kFloat3 = "float3";
constexpr auto kFloat4 = "float4";
constexpr auto kDouble2 = "double2";
constexpr auto kDouble3 = "double3";
constexpr auto kDouble4 = "double4";

constexpr auto kQuath = "quath";
constexpr auto kQuatf = "quatf";
constexpr auto kQuatd = "quatd";

constexpr auto kNormal3f = "normal3f";
constexpr auto kNormal3d = "normal3d";
constexpr auto kNormal3h = "normal3h";
constexpr auto kPoint3f = "point3f";
constexpr auto kPoint3d = "point3d";
constexpr auto kPoint3h = "point3h";
constexpr auto kVector3f = "vector3f";
constexpr auto kVector3d = "vector3d";
constexpr auto kVector3h = "vector3h";
constexpr auto kVector4f = "vector4f";
constexpr auto kVector4d = "vector4d";
constexpr auto kVector4h = "vector4h";

constexpr auto kColor3f = "color3f";
constexpr auto kColor3d = "color3d";
constexpr auto kColor3h = "color3h";
constexpr auto kColor4f = "color4f";
constexpr auto kColor4d = "color4d";
constexpr auto kColor4h = "color4h";

constexpr auto kTexCoord2f = "texCoord2f";
constexpr auto kTexCoord2d = "texCoord2d";
constexpr auto kTexCoord2h = "texCoord2h";
constexpr auto kTexCoord3f = "texCoord3f";
constexpr auto kTexCoord3d = "texCoord3d";
constexpr auto kTexCoord3h = "texCoord3h";

constexpr auto kMatrix2f = "matrix2f";
constexpr auto kMatrix3f = "matrix3f";
constexpr auto kMatrix4f = "matrix4f";
constexpr auto kMatrix2d = "matrix2d";
constexpr auto kMatrix3d = "matrix3d";
constexpr auto kMatrix4d = "matrix4d";

constexpr auto kFrame4d = "frame4d";
constexpr auto kRelationship = "rel";

// ============================================================================
// Half-precision float type
// ============================================================================

struct half {
  uint16_t _h;

  half() : _h(0) {}
  explicit half(uint16_t h) : _h(h) {}
  half(float f);  // Implemented in value-types.cc

  operator float() const;  // Implemented in value-types.cc
};

// ============================================================================
// Basic vector types (using std::array for POD compatibility)
// ============================================================================

using half2 = std::array<half, 2>;
using half3 = std::array<half, 3>;
using half4 = std::array<half, 4>;

using char2 = std::array<char, 2>;
using char3 = std::array<char, 3>;
using char4 = std::array<char, 4>;

using uchar2 = std::array<uint8_t, 2>;
using uchar3 = std::array<uint8_t, 3>;
using uchar4 = std::array<uint8_t, 4>;

using short2 = std::array<int16_t, 2>;
using short3 = std::array<int16_t, 3>;
using short4 = std::array<int16_t, 4>;

using ushort2 = std::array<uint16_t, 2>;
using ushort3 = std::array<uint16_t, 3>;
using ushort4 = std::array<uint16_t, 4>;

using int2 = std::array<int32_t, 2>;
using int3 = std::array<int32_t, 3>;
using int4 = std::array<int32_t, 4>;

using uint2 = std::array<uint32_t, 2>;
using uint3 = std::array<uint32_t, 3>;
using uint4 = std::array<uint32_t, 4>;

using float2 = std::array<float, 2>;
using float3 = std::array<float, 3>;
using float4 = std::array<float, 4>;

using double2 = std::array<double, 2>;
using double3 = std::array<double, 3>;
using double4 = std::array<double, 4>;

// ============================================================================
// Matrix types (forward declarations - full definitions in value-types.hh)
// ============================================================================

struct matrix2f;
struct matrix3f;
struct matrix4f;
struct matrix2d;
struct matrix3d;
struct matrix4d;
struct frame4d;

// ============================================================================
// Quaternion types (forward declarations - full definitions in value-types.hh)
// ============================================================================

struct quath;
struct quatf;
struct quatd;

// ============================================================================
// Role types (forward declarations - full definitions in value-types.hh)
// These are semantically-typed wrappers around basic vector types
// ============================================================================

struct point3h;
struct point3f;
struct point3d;
struct normal3h;
struct normal3f;
struct normal3d;
struct vector3h;
struct vector3f;
struct vector3d;
struct vector4h;
struct vector4f;
struct vector4d;
struct color3h;
struct color3f;
struct color3d;
struct color4h;
struct color4f;
struct color4d;
struct texcoord2h;
struct texcoord2f;
struct texcoord2d;
struct texcoord3h;
struct texcoord3f;
struct texcoord3d;

// ============================================================================
// Other types (forward declarations)
// ============================================================================

class Value;
class AssetPath;
class TimeCode;
struct TimeSamples;
struct StringData;
using token = lightusd::Token;
struct dict;
struct timecode;

}  // namespace value
}  // namespace lightusd
