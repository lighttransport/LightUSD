// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - crate writer type mapping helpers.

#include "crate-writer-types.hh"

namespace lightusd {
namespace next {

CrateTypeId ToCrateTypeId(TypeId type_id) {
  switch (type_id) {
    case TypeId::Bool: return CrateTypeId::Bool;
    case TypeId::Int: return CrateTypeId::Int;
    case TypeId::UInt: return CrateTypeId::UInt;
    case TypeId::Int64: return CrateTypeId::Int64;
    case TypeId::UInt64: return CrateTypeId::UInt64;
    case TypeId::Half: return CrateTypeId::Half;
    case TypeId::Half2: return CrateTypeId::Vec2h;
    case TypeId::Half3: return CrateTypeId::Vec3h;
    case TypeId::Half4: return CrateTypeId::Vec4h;
    case TypeId::Quath: return CrateTypeId::Quath;
    case TypeId::UChar: return CrateTypeId::UChar;
    case TypeId::Float: return CrateTypeId::Float;
    case TypeId::Double: return CrateTypeId::Double;
    case TypeId::String: return CrateTypeId::String;
    case TypeId::Token: return CrateTypeId::Token;
    case TypeId::AssetPath: return CrateTypeId::AssetPath;
    case TypeId::Int2: return CrateTypeId::Vec2i;
    case TypeId::Int3: return CrateTypeId::Vec3i;
    case TypeId::Int4: return CrateTypeId::Vec4i;
    // pxr's Sdf has no unsigned vector types; encode as the signed twin
    // (same 32-bit lanes, bit-exact round-trip; the declared type name
    // "uintN" restores the USDA-facing type). Previously these mapped to
    // Invalid and the VALUE was silently dropped.
    case TypeId::UInt2: return CrateTypeId::Vec2i;
    case TypeId::UInt3: return CrateTypeId::Vec3i;
    case TypeId::UInt4: return CrateTypeId::Vec4i;
    case TypeId::Float2: return CrateTypeId::Vec2f;
    case TypeId::Float3:
    case TypeId::Point3f:
    case TypeId::Vector3f:
    case TypeId::Normal3f:
    case TypeId::Color3f:
      return CrateTypeId::Vec3f;
    case TypeId::Float4:
    case TypeId::Color4f:
      return CrateTypeId::Vec4f;
    case TypeId::Double2: return CrateTypeId::Vec2d;
    case TypeId::Double3:
    case TypeId::Point3d:
    case TypeId::Vector3d:
    case TypeId::Normal3d:
    case TypeId::Color3d:
      return CrateTypeId::Vec3d;
    case TypeId::Double4:
    case TypeId::Color4d:
      return CrateTypeId::Vec4d;
    case TypeId::Texcoord2f: return CrateTypeId::Vec2f;
    case TypeId::Texcoord2d: return CrateTypeId::Vec2d;
    case TypeId::Texcoord2h: return CrateTypeId::Vec2h;
    case TypeId::Texcoord3f: return CrateTypeId::Vec3f;
    case TypeId::Texcoord3d: return CrateTypeId::Vec3d;
    case TypeId::Texcoord3h: return CrateTypeId::Vec3h;
    case TypeId::Point3h:
    case TypeId::Vector3h:
    case TypeId::Normal3h:
    case TypeId::Color3h:
      return CrateTypeId::Vec3h;
    case TypeId::Color4h: return CrateTypeId::Vec4h;
    case TypeId::TimeCode: return CrateTypeId::TimeCode;
    case TypeId::Quatf: return CrateTypeId::Quatf;
    case TypeId::Quatd: return CrateTypeId::Quatd;
    case TypeId::Matrix2d: return CrateTypeId::Matrix2d;
    case TypeId::Matrix3d: return CrateTypeId::Matrix3d;
    case TypeId::Matrix4d: return CrateTypeId::Matrix4d;
    // frame4d shares matrix4d's crate encoding; the declared type name
    // restores the role on read (retag_role: Double x16 both sides).
    case TypeId::Frame4d: return CrateTypeId::Matrix4d;
    // pathExpression: crate 0.10 type 57 shares the Token payload form.
    case TypeId::PathExpression: return CrateTypeId::PathExpression;
    case TypeId::Matrix2f: return CrateTypeId::Matrix2d;
    case TypeId::Matrix3f: return CrateTypeId::Matrix3d;
    case TypeId::Matrix4f: return CrateTypeId::Matrix4d;
    default: return CrateTypeId::Invalid;
  }
}

uint32_t ArrayComps(TypeId type_id) {
  switch (type_id) {
    case TypeId::Half:
    case TypeId::TimeCode:
    case TypeId::UChar:
      return 1;
    case TypeId::Float2:
    case TypeId::Double2:
    case TypeId::Texcoord2f:
    case TypeId::Texcoord2d:
    case TypeId::Texcoord2h:
    case TypeId::Int2:
    case TypeId::UInt2:
    case TypeId::Half2:
      return 2;
    case TypeId::Float3:
    case TypeId::Double3:
    case TypeId::Point3f:
    case TypeId::Vector3f:
    case TypeId::Normal3f:
    case TypeId::Color3f:
    case TypeId::Color3h:
    case TypeId::Color3d:
    case TypeId::Point3h:
    case TypeId::Vector3h:
    case TypeId::Normal3h:
    case TypeId::Point3d:
    case TypeId::Vector3d:
    case TypeId::Normal3d:
    case TypeId::Texcoord3f:
    case TypeId::Texcoord3d:
    case TypeId::Texcoord3h:
    case TypeId::Int3:
    case TypeId::UInt3:
    case TypeId::Half3:
      return 3;
    case TypeId::Float4:
    case TypeId::Double4:
    case TypeId::Color4f:
    case TypeId::Color4h:
    case TypeId::Color4d:
    case TypeId::Quatf:
    case TypeId::Quatd:
    case TypeId::Matrix2f:
    case TypeId::Matrix2d:
    case TypeId::Int4:
    case TypeId::UInt4:
    case TypeId::Half4:
    case TypeId::Quath:
      return 4;
    case TypeId::Matrix3f:
    case TypeId::Matrix3d:
      return 9;
    case TypeId::Matrix4f:
    case TypeId::Matrix4d:
      return 16;
    default:
      return 0;
  }
}

size_t CrateValueSize(CrateTypeId type, bool is_array) {
  if (is_array) return 0;
  switch (type) {
    case CrateTypeId::Bool:
    case CrateTypeId::UChar: return 1;
    case CrateTypeId::Int:
    case CrateTypeId::UInt:
    case CrateTypeId::Float: return 4;
    case CrateTypeId::Half: return 2;
    case CrateTypeId::Vec2h: return 4;
    case CrateTypeId::Vec3h: return 6;
    case CrateTypeId::Int64:
    case CrateTypeId::UInt64:
    case CrateTypeId::Double:
    case CrateTypeId::TimeCode:
    case CrateTypeId::Vec4h:
    case CrateTypeId::Quath: return 8;
    case CrateTypeId::Vec2f:
    case CrateTypeId::Vec2i: return 8;
    case CrateTypeId::Vec3f:
    case CrateTypeId::Vec3i: return 12;
    case CrateTypeId::Vec2d:
    case CrateTypeId::Vec4f:
    case CrateTypeId::Vec4i:
    case CrateTypeId::Quatf: return 16;
    case CrateTypeId::Vec3d: return 24;
    case CrateTypeId::Vec4d:
    case CrateTypeId::Quatd: return 32;
    case CrateTypeId::Matrix2d: return 32;
    case CrateTypeId::Matrix3d: return 72;
    case CrateTypeId::Matrix4d: return 128;
    default: return 0;
  }
}

}  // namespace next
}  // namespace lightusd
