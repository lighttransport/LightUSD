// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Built-in USD layouts. Metadata only: no per-type executable callbacks.
#include "type-info.hh"

#include <cstring>

namespace lightusd {
namespace next {
namespace {
constexpr uint8_t kScalar = 1;
constexpr uint8_t kNumeric = 2;
constexpr TypeInfo kTypes[] = {
  {TypeId::Invalid, nullptr, nullptr, 0, 0,
   TypeId::Invalid, 0, 0},
  {TypeId::Bool, "bool", "bool", sizeof(bool) * 1, alignof(bool),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::Int, "int", "int32_t", sizeof(int32_t) * 1, alignof(int32_t),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::UInt, "uint", "uint32_t", sizeof(uint32_t) * 1, alignof(uint32_t),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::Int64, "int64", "int64_t", sizeof(int64_t) * 1, alignof(int64_t),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::UInt64, "uint64", "uint64_t", sizeof(uint64_t) * 1, alignof(uint64_t),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::Half, "half", "half", sizeof(uint16_t) * 1, alignof(uint16_t),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::Float, "float", "float", sizeof(float) * 1, alignof(float),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::Double, "double", "double", sizeof(double) * 1, alignof(double),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::String, "string", "std::string", 0, 1,
   TypeId::Invalid, 0, 0},
  {TypeId::Token, "token", "Token", 0, 1,
   TypeId::Invalid, 0, 0},
  {TypeId::AssetPath, "asset", "AssetPath", 0, 1,
   TypeId::Invalid, 0, 0},
  {TypeId::Int2, "int2", "int2", sizeof(int32_t) * 2, alignof(int32_t),
   TypeId::Int, 2, kNumeric},
  {TypeId::Int3, "int3", "int3", sizeof(int32_t) * 3, alignof(int32_t),
   TypeId::Int, 3, kNumeric},
  {TypeId::Int4, "int4", "int4", sizeof(int32_t) * 4, alignof(int32_t),
   TypeId::Int, 4, kNumeric},
  {TypeId::UInt2, "uint2", "uint2", sizeof(uint32_t) * 2, alignof(uint32_t),
   TypeId::UInt, 2, kNumeric},
  {TypeId::UInt3, "uint3", "uint3", sizeof(uint32_t) * 3, alignof(uint32_t),
   TypeId::UInt, 3, kNumeric},
  {TypeId::UInt4, "uint4", "uint4", sizeof(uint32_t) * 4, alignof(uint32_t),
   TypeId::UInt, 4, kNumeric},
  {TypeId::Half2, "half2", "half2", sizeof(uint16_t) * 2, alignof(uint16_t),
   TypeId::Half, 2, kNumeric},
  {TypeId::Half3, "half3", "half3", sizeof(uint16_t) * 3, alignof(uint16_t),
   TypeId::Half, 3, kNumeric},
  {TypeId::Half4, "half4", "half4", sizeof(uint16_t) * 4, alignof(uint16_t),
   TypeId::Half, 4, kNumeric},
  {TypeId::Float2, "float2", "float2", sizeof(float) * 2, alignof(float),
   TypeId::Float, 2, kNumeric},
  {TypeId::Float3, "float3", "float3", sizeof(float) * 3, alignof(float),
   TypeId::Float, 3, kNumeric},
  {TypeId::Float4, "float4", "float4", sizeof(float) * 4, alignof(float),
   TypeId::Float, 4, kNumeric},
  {TypeId::Double2, "double2", "double2", sizeof(double) * 2, alignof(double),
   TypeId::Double, 2, kNumeric},
  {TypeId::Double3, "double3", "double3", sizeof(double) * 3, alignof(double),
   TypeId::Double, 3, kNumeric},
  {TypeId::Double4, "double4", "double4", sizeof(double) * 4, alignof(double),
   TypeId::Double, 4, kNumeric},
  {TypeId::Quath, "quath", "quath", sizeof(uint16_t) * 4, alignof(uint16_t),
   TypeId::Half, 4, kNumeric},
  {TypeId::Quatf, "quatf", "quatf", sizeof(float) * 4, alignof(float),
   TypeId::Float, 4, kNumeric},
  {TypeId::Quatd, "quatd", "quatd", sizeof(double) * 4, alignof(double),
   TypeId::Double, 4, kNumeric},
  {TypeId::Point3h, "point3h", "point3h", sizeof(uint16_t) * 3, alignof(uint16_t),
   TypeId::Half, 3, 0},
  {TypeId::Point3f, "point3f", "point3f", sizeof(float) * 3, alignof(float),
   TypeId::Float, 3, 0},
  {TypeId::Point3d, "point3d", "point3d", sizeof(double) * 3, alignof(double),
   TypeId::Double, 3, 0},
  {TypeId::Vector3h, "vector3h", "vector3h", sizeof(uint16_t) * 3, alignof(uint16_t),
   TypeId::Half, 3, 0},
  {TypeId::Vector3f, "vector3f", "vector3f", sizeof(float) * 3, alignof(float),
   TypeId::Float, 3, 0},
  {TypeId::Vector3d, "vector3d", "vector3d", sizeof(double) * 3, alignof(double),
   TypeId::Double, 3, 0},
  {TypeId::Normal3h, "normal3h", "normal3h", sizeof(uint16_t) * 3, alignof(uint16_t),
   TypeId::Half, 3, 0},
  {TypeId::Normal3f, "normal3f", "normal3f", sizeof(float) * 3, alignof(float),
   TypeId::Float, 3, 0},
  {TypeId::Normal3d, "normal3d", "normal3d", sizeof(double) * 3, alignof(double),
   TypeId::Double, 3, 0},
  {TypeId::Color3h, "color3h", "color3h", sizeof(uint16_t) * 3, alignof(uint16_t),
   TypeId::Half, 3, 0},
  {TypeId::Color3f, "color3f", "color3f", sizeof(float) * 3, alignof(float),
   TypeId::Float, 3, 0},
  {TypeId::Color3d, "color3d", "color3d", sizeof(double) * 3, alignof(double),
   TypeId::Double, 3, 0},
  {TypeId::Color4h, "color4h", "color4h", sizeof(uint16_t) * 4, alignof(uint16_t),
   TypeId::Half, 4, 0},
  {TypeId::Color4f, "color4f", "color4f", sizeof(float) * 4, alignof(float),
   TypeId::Float, 4, 0},
  {TypeId::Color4d, "color4d", "color4d", sizeof(double) * 4, alignof(double),
   TypeId::Double, 4, 0},
  {TypeId::Matrix2f, "matrix2f", "matrix2f", sizeof(float) * 4, alignof(float),
   TypeId::Float, 4, kNumeric},
  {TypeId::Matrix2d, "matrix2d", "matrix2d", sizeof(double) * 4, alignof(double),
   TypeId::Double, 4, kNumeric},
  {TypeId::Matrix3f, "matrix3f", "matrix3f", sizeof(float) * 9, alignof(float),
   TypeId::Float, 9, kNumeric},
  {TypeId::Matrix3d, "matrix3d", "matrix3d", sizeof(double) * 9, alignof(double),
   TypeId::Double, 9, kNumeric},
  {TypeId::Matrix4f, "matrix4f", "matrix4f", sizeof(float) * 16, alignof(float),
   TypeId::Float, 16, kNumeric},
  {TypeId::Matrix4d, "matrix4d", "matrix4d", sizeof(double) * 16, alignof(double),
   TypeId::Double, 16, kNumeric},
  {TypeId::Texcoord2h, "texCoord2h", "texcoord2h", sizeof(uint16_t) * 2, alignof(uint16_t),
   TypeId::Half, 2, 0},
  {TypeId::Texcoord2f, "texCoord2f", "texcoord2f", sizeof(float) * 2, alignof(float),
   TypeId::Float, 2, 0},
  {TypeId::Texcoord2d, "texCoord2d", "texcoord2d", sizeof(double) * 2, alignof(double),
   TypeId::Double, 2, 0},
  {TypeId::Texcoord3h, "texCoord3h", "texcoord3h", sizeof(uint16_t) * 3, alignof(uint16_t),
   TypeId::Half, 3, 0},
  {TypeId::Texcoord3f, "texCoord3f", "texcoord3f", sizeof(float) * 3, alignof(float),
   TypeId::Float, 3, 0},
  {TypeId::Texcoord3d, "texCoord3d", "texcoord3d", sizeof(double) * 3, alignof(double),
   TypeId::Double, 3, 0},
  {TypeId::TimeCode, "timecode", "TimeCode", sizeof(double) * 1, alignof(double),
   TypeId::Invalid, 1, kScalar},
  {TypeId::Extent, "float3[]", "extent", sizeof(float) * 6, alignof(float),
   TypeId::Invalid, 0, 0},
  {TypeId::Dictionary, "dictionary", "Dictionary", 0, 1,
   TypeId::Invalid, 0, 0},
  {TypeId::Relationship, "rel", "Relationship", 0, 1,
   TypeId::Invalid, 0, 0},
  {TypeId::Reference, "reference", "Reference", 0, 1,
   TypeId::Invalid, 0, 0},
  {TypeId::UChar, "uchar", "uint8_t", sizeof(uint8_t) * 1, alignof(uint8_t),
   TypeId::Invalid, 1, kScalar | kNumeric},
  {TypeId::Frame4d, "frame4d", "frame4d", sizeof(double) * 16, alignof(double),
   TypeId::Double, 16, kNumeric},
  {TypeId::PathExpression, "pathExpression", "PathExpression", 0, 1,
   TypeId::Invalid, 0, 0},

};
constexpr size_t kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);
static_assert(kTypeCount == static_cast<size_t>(TypeId::Count), "Missing type metadata");
constexpr bool TypesInOrder() {
  for (size_t i = 0; i < kTypeCount; ++i) {
    if (static_cast<size_t>(kTypes[i].id) != i) return false;
  }
  return true;
}
static_assert(TypesInOrder(), "Type metadata must follow TypeId order");

// Construct a fixed name index at compile time. No dynamic initialization,
// allocation, locks, or templated hash containers on the parsing path.
constexpr uint32_t NameHash(const char* name, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ static_cast<uint8_t>(name[i])) * 16777619u;
  }
  return hash;
}
constexpr size_t NameLength(const char* name) {
  size_t n = 0;
  while (name[n]) ++n;
  return n;
}
constexpr size_t kNameSlots = 256;
static_assert(kTypeCount < kNameSlots / 2, "Increase name table capacity");
struct NameIndex { uint16_t slots[kNameSlots] = {}; };
constexpr NameIndex MakeNameIndex() {
  NameIndex index{};
  for (size_t i = 1; i < kTypeCount; ++i) {
    const char* name = kTypes[i].name;
    if (!name) continue;
    size_t slot = NameHash(name, NameLength(name)) % kNameSlots;
    while (index.slots[slot]) slot = (slot + 1) % kNameSlots;
    index.slots[slot] = static_cast<uint16_t>(i);
  }
  return index;
}
constexpr NameIndex kNames = MakeNameIndex();
}  // namespace

const TypeInfo* GetTypeInfo(TypeId id) {
  const size_t index = static_cast<size_t>(id);
  return index < kTypeCount ? &kTypes[index] : nullptr;
}

void InitTypeRegistry() {}  // Built-in metadata is constant-initialized.

const char* GetTypeName(TypeId id) {
  const TypeInfo* info = GetTypeInfo(id);
  return info ? info->name : nullptr;
}

TypeId GetTypeIdFromName(const char* name, size_t size) {
  if (!name || !size) return TypeId::Invalid;
  size_t slot = NameHash(name, size) % kNameSlots;
  for (size_t probe = 0; probe < kNameSlots; ++probe) {
    const uint16_t index = kNames.slots[slot];
    if (!index) return TypeId::Invalid;
    const char* candidate = kTypes[index].name;
    if (std::strlen(candidate) == size && std::memcmp(candidate, name, size) == 0)
      return kTypes[index].id;
    slot = (slot + 1) % kNameSlots;
  }
  return TypeId::Invalid;
}

TypeId GetTypeIdFromName(const char* name) {
  return name ? GetTypeIdFromName(name, std::strlen(name)) : TypeId::Invalid;
}

size_t GetTypeSize(TypeId id) {
  const TypeInfo* info = GetTypeInfo(id);
  return info ? info->size : 0;
}
size_t GetTypeAlignment(TypeId id) {
  const TypeInfo* info = GetTypeInfo(id);
  return info ? info->alignment : 0;
}
bool IsScalarType(TypeId id) {
  const TypeInfo* info = GetTypeInfo(id);
  return info && (info->flags & kScalar) != 0;
}
bool IsNumericType(TypeId id) {
  const TypeInfo* info = GetTypeInfo(id);
  return info && (info->flags & kNumeric) != 0;
}
TypeId GetComponentType(TypeId id) {
  const TypeInfo* info = GetTypeInfo(id);
  return info ? info->component_type : TypeId::Invalid;
}
size_t GetComponentCount(TypeId id) {
  const TypeInfo* info = GetTypeInfo(id);
  return info ? info->component_count : 0;
}

}  // namespace next
}  // namespace lightusd
