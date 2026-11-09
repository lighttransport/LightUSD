// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Built-in USD layouts. Metadata only: no per-type executable callbacks.
//
// The table and the USD <-> TypeId name mapping live in the C11 core
// (src/next/c/type-names.c), so C consumers (the C ABI) share one copy of the
// type system. This file is the C++ facade.

#include "type-info.hh"

#include <array>
#include <cstring>

#include "../c/lightusd-core-c.h"

namespace lightusd {
namespace next {
namespace {

// C/C++ enum values are an ABI contract.
static_assert(static_cast<unsigned>(TypeId::Invalid) == LU_TYPE_INVALID,
              "TypeId::Invalid ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Bool) == LU_TYPE_BOOL,
              "TypeId::Bool ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Int) == LU_TYPE_INT,
              "TypeId::Int ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::UInt) == LU_TYPE_UINT,
              "TypeId::UInt ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Int64) == LU_TYPE_INT64,
              "TypeId::Int64 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::UInt64) == LU_TYPE_UINT64,
              "TypeId::UInt64 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Half) == LU_TYPE_HALF,
              "TypeId::Half ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Float) == LU_TYPE_FLOAT,
              "TypeId::Float ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Double) == LU_TYPE_DOUBLE,
              "TypeId::Double ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::String) == LU_TYPE_STRING,
              "TypeId::String ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Token) == LU_TYPE_TOKEN,
              "TypeId::Token ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::AssetPath) == LU_TYPE_ASSETPATH,
              "TypeId::AssetPath ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Int2) == LU_TYPE_INT2,
              "TypeId::Int2 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Int3) == LU_TYPE_INT3,
              "TypeId::Int3 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Int4) == LU_TYPE_INT4,
              "TypeId::Int4 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::UInt2) == LU_TYPE_UINT2,
              "TypeId::UInt2 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::UInt3) == LU_TYPE_UINT3,
              "TypeId::UInt3 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::UInt4) == LU_TYPE_UINT4,
              "TypeId::UInt4 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Half2) == LU_TYPE_HALF2,
              "TypeId::Half2 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Half3) == LU_TYPE_HALF3,
              "TypeId::Half3 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Half4) == LU_TYPE_HALF4,
              "TypeId::Half4 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Float2) == LU_TYPE_FLOAT2,
              "TypeId::Float2 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Float3) == LU_TYPE_FLOAT3,
              "TypeId::Float3 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Float4) == LU_TYPE_FLOAT4,
              "TypeId::Float4 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Double2) == LU_TYPE_DOUBLE2,
              "TypeId::Double2 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Double3) == LU_TYPE_DOUBLE3,
              "TypeId::Double3 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Double4) == LU_TYPE_DOUBLE4,
              "TypeId::Double4 ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Quath) == LU_TYPE_QUATH,
              "TypeId::Quath ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Quatf) == LU_TYPE_QUATF,
              "TypeId::Quatf ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Quatd) == LU_TYPE_QUATD,
              "TypeId::Quatd ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Point3h) == LU_TYPE_POINT3H,
              "TypeId::Point3h ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Point3f) == LU_TYPE_POINT3F,
              "TypeId::Point3f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Point3d) == LU_TYPE_POINT3D,
              "TypeId::Point3d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Vector3h) == LU_TYPE_VECTOR3H,
              "TypeId::Vector3h ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Vector3f) == LU_TYPE_VECTOR3F,
              "TypeId::Vector3f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Vector3d) == LU_TYPE_VECTOR3D,
              "TypeId::Vector3d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Normal3h) == LU_TYPE_NORMAL3H,
              "TypeId::Normal3h ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Normal3f) == LU_TYPE_NORMAL3F,
              "TypeId::Normal3f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Normal3d) == LU_TYPE_NORMAL3D,
              "TypeId::Normal3d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Color3h) == LU_TYPE_COLOR3H,
              "TypeId::Color3h ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Color3f) == LU_TYPE_COLOR3F,
              "TypeId::Color3f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Color3d) == LU_TYPE_COLOR3D,
              "TypeId::Color3d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Color4h) == LU_TYPE_COLOR4H,
              "TypeId::Color4h ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Color4f) == LU_TYPE_COLOR4F,
              "TypeId::Color4f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Color4d) == LU_TYPE_COLOR4D,
              "TypeId::Color4d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Matrix2f) == LU_TYPE_MATRIX2F,
              "TypeId::Matrix2f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Matrix2d) == LU_TYPE_MATRIX2D,
              "TypeId::Matrix2d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Matrix3f) == LU_TYPE_MATRIX3F,
              "TypeId::Matrix3f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Matrix3d) == LU_TYPE_MATRIX3D,
              "TypeId::Matrix3d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Matrix4f) == LU_TYPE_MATRIX4F,
              "TypeId::Matrix4f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Matrix4d) == LU_TYPE_MATRIX4D,
              "TypeId::Matrix4d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Texcoord2h) == LU_TYPE_TEXCOORD2H,
              "TypeId::Texcoord2h ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Texcoord2f) == LU_TYPE_TEXCOORD2F,
              "TypeId::Texcoord2f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Texcoord2d) == LU_TYPE_TEXCOORD2D,
              "TypeId::Texcoord2d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Texcoord3h) == LU_TYPE_TEXCOORD3H,
              "TypeId::Texcoord3h ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Texcoord3f) == LU_TYPE_TEXCOORD3F,
              "TypeId::Texcoord3f ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Texcoord3d) == LU_TYPE_TEXCOORD3D,
              "TypeId::Texcoord3d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::TimeCode) == LU_TYPE_TIMECODE,
              "TypeId::TimeCode ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Extent) == LU_TYPE_EXTENT,
              "TypeId::Extent ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Dictionary) == LU_TYPE_DICTIONARY,
              "TypeId::Dictionary ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Relationship) == LU_TYPE_RELATIONSHIP,
              "TypeId::Relationship ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Reference) == LU_TYPE_REFERENCE,
              "TypeId::Reference ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::UChar) == LU_TYPE_UCHAR,
              "TypeId::UChar ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Frame4d) == LU_TYPE_FRAME4D,
              "TypeId::Frame4d ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::PathExpression) == LU_TYPE_PATHEXPRESSION,
              "TypeId::PathExpression ABI mismatch");
static_assert(static_cast<unsigned>(TypeId::Count) == LU_TYPE_COUNT,
              "TypeId::Count ABI mismatch");


inline constexpr uint16_t T(TypeId id) { return static_cast<uint16_t>(id); }

}  // namespace

const TypeInfo* GetTypeInfo(TypeId id) {
  if (T(id) >= T(TypeId::Count)) return nullptr;
  // C records and C++ records have distinct effective types. Construct actual
  // TypeInfo objects once; their addresses remain stable without heap allocation.
  static const auto rows = [] {
    std::array<TypeInfo, static_cast<size_t>(TypeId::Count)> result{};
    for (size_t i = 0; i < result.size(); ++i) {
      const lu_type_info& row = *lu_type_info_at(static_cast<uint16_t>(i));
      result[i] = {static_cast<TypeId>(row.id), row.name, row.cpp_name,
                   row.size, row.alignment,
                   static_cast<TypeId>(row.component_type),
                   row.component_count, row.flags};
    }
    return result;
  }();
  return &rows[T(id)];
}

void InitTypeRegistry() {}  // Built-in metadata is constant-initialized.

const char* GetTypeName(TypeId id) { return lu_type_name(T(id)); }

TypeId GetTypeIdFromName(const char* name, size_t size) {
  return static_cast<TypeId>(lu_type_id_from_name(name, size));
}

TypeId GetTypeIdFromName(const char* name) {
  return name ? GetTypeIdFromName(name, std::strlen(name)) : TypeId::Invalid;
}

size_t GetTypeSize(TypeId id) { return lu_type_size(T(id)); }

size_t GetTypeAlignment(TypeId id) { return lu_type_alignment(T(id)); }

bool IsScalarType(TypeId id) { return lu_type_is_scalar(T(id)) != 0; }


TypeId GetComponentType(TypeId id) {
  return static_cast<TypeId>(lu_type_component_type(T(id)));
}

size_t GetComponentCount(TypeId id) { return lu_type_component_count(T(id)); }

}  // namespace next
}  // namespace lightusd
