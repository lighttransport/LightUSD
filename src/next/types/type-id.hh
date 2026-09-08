// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Type ID definitions
// Standalone header with no dependencies beyond C++14 standard library

#pragma once

#include <cstdint>
#include <cstddef>

namespace lightusd {
namespace next {

/// Core type identifier enum
/// All USD value types are represented by a single enum value
/// This replaces the template-based DEFINE_TYPE_TRAIT system
enum class TypeId : uint16_t {
  Invalid = 0,

  // ============================================================
  // Scalar types
  // ============================================================
  Bool,
  Int,       // int32_t
  UInt,      // uint32_t
  Int64,
  UInt64,
  Half,      // 16-bit float
  Float,     // 32-bit float
  Double,    // 64-bit float

  // String types
  String,
  Token,
  AssetPath,

  // ============================================================
  // Integer vectors
  // ============================================================
  Int2,
  Int3,
  Int4,
  UInt2,
  UInt3,
  UInt4,

  // ============================================================
  // Half-precision vectors
  // ============================================================
  Half2,
  Half3,
  Half4,

  // ============================================================
  // Single-precision vectors
  // ============================================================
  Float2,
  Float3,
  Float4,

  // ============================================================
  // Double-precision vectors
  // ============================================================
  Double2,
  Double3,
  Double4,

  // ============================================================
  // Quaternions
  // ============================================================
  Quath,
  Quatf,
  Quatd,

  // ============================================================
  // Geometric point types (semantic distinction from vectors)
  // ============================================================
  Point3h,
  Point3f,
  Point3d,

  // ============================================================
  // Geometric vector types
  // ============================================================
  Vector3h,
  Vector3f,
  Vector3d,

  // ============================================================
  // Normal types
  // ============================================================
  Normal3h,
  Normal3f,
  Normal3d,

  // ============================================================
  // Color types
  // ============================================================
  Color3h,
  Color3f,
  Color3d,
  Color4h,
  Color4f,
  Color4d,

  // ============================================================
  // Matrix types
  // ============================================================
  Matrix2f,
  Matrix2d,
  Matrix3f,
  Matrix3d,
  Matrix4f,
  Matrix4d,

  // ============================================================
  // Texture coordinate types
  // ============================================================
  Texcoord2h,
  Texcoord2f,
  Texcoord2d,
  Texcoord3h,
  Texcoord3f,
  Texcoord3d,

  // ============================================================
  // Special USD types
  // ============================================================
  TimeCode,
  Extent,      // float3[2] bounding box
  Dictionary,

  // ============================================================
  // Relationship/Reference types
  // ============================================================
  Relationship,
  Reference,

  // uchar / uchar[] (pxr's UChar). Appended at the END so existing numeric
  // TypeId values stay stable. Scalar storage is one byte in the SBO;
  // arrays are widened into the UInt array storage (bit-exact lanes).
  UChar,

  // frame4d: a matrix4d role type (same double[16] storage).
  Frame4d,
  // pathExpression: SdfPathExpression, string-backed.
  PathExpression,

  // Sentinel for array sizing
  Count
};

/// Get the USD type name string for a TypeId
/// Returns nullptr for Invalid or unknown types
const char* GetTypeName(TypeId id);

/// Get TypeId from a USD type name string
/// Returns TypeId::Invalid if the name is not recognized
TypeId GetTypeIdFromName(const char* name);
/// Look up a bounded name without allocating or requiring a NUL terminator.
TypeId GetTypeIdFromName(const char* name, size_t size);

/// Get the size in bytes of the type
/// Returns 0 for variable-size types (String, Dictionary) or Invalid
size_t GetTypeSize(TypeId id);

/// Get the alignment requirement in bytes
size_t GetTypeAlignment(TypeId id);

/// Check if a type is a scalar (single value, not a vector/matrix)
bool IsScalarType(TypeId id);

/// Check if a type is a numeric type (can be used in math operations)
bool IsNumericType(TypeId id);

/// Get the component type of a vector/matrix type
/// Returns the scalar element type, or Invalid for non-compound types
TypeId GetComponentType(TypeId id);

/// Get the number of components in a vector/matrix type
/// Returns 1 for scalars, 0 for non-numeric types
size_t GetComponentCount(TypeId id);

}  // namespace next
}  // namespace lightusd
