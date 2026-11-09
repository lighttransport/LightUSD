// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// value::type-name <-> type-id registry — split out of value-types.cc.
// Holds TryGetTypeName/TryGetTypeId/TryGetUnderlyingType{Name,Id} + their Get*
// wrappers + IsRoleType. Each is backed by a large compile-time mapbox::eternal
// map (~190 entries); isolating them lets value-types.cc drop the heavy
// mapbox/eternal include and shortens the build critical path. All functions are
// declared in value-types.hh.
#include "value-types.hh"

#include "str-util.hh"

#include "external/mapbox/eternal/include/mapbox/eternal.hpp"

namespace lightusd {
namespace value {

nonstd::optional<std::string> TryGetTypeName(uint32_t tyid) {
  MAPBOX_ETERNAL_CONSTEXPR const auto tynamemap =
      mapbox::eternal::map<uint32_t, mapbox::eternal::string>({
          {TYPE_ID_TOKEN, kToken},
          {TYPE_ID_STRING, kString},
          {TYPE_ID_STRING, kPath},
          {TYPE_ID_ASSET_PATH, kAssetPath},
          {TYPE_ID_PATH_EXPRESSION, kPathExpression},
          {TYPE_ID_DICT, kDictionary},
          {TYPE_ID_TIMECODE, kTimeCode},
          {TYPE_ID_BOOL, kBool},
          {TYPE_ID_CHAR, kChar},
          {TYPE_ID_CHAR2, kChar2},
          {TYPE_ID_CHAR3, kChar3},
          {TYPE_ID_CHAR4, kChar4},
          {TYPE_ID_UCHAR, kUChar},
          {TYPE_ID_UCHAR2, kUChar2},
          {TYPE_ID_UCHAR3, kUChar3},
          {TYPE_ID_UCHAR4, kUChar4},
          {TYPE_ID_HALF, kHalf},
          {TYPE_ID_SHORT, kShort},
          {TYPE_ID_SHORT2, kShort2},
          {TYPE_ID_SHORT3, kShort3},
          {TYPE_ID_SHORT4, kShort4},
          {TYPE_ID_USHORT, kUShort},
          {TYPE_ID_USHORT2, kUShort2},
          {TYPE_ID_USHORT3, kUShort3},
          {TYPE_ID_USHORT4, kUShort4},
          {TYPE_ID_INT32, kInt},
          {TYPE_ID_UINT32, kUInt},
          {TYPE_ID_INT64, kInt64},
          {TYPE_ID_UINT64, kUInt64},
          {TYPE_ID_INT2, kInt2},
          {TYPE_ID_INT3, kInt3},
          {TYPE_ID_INT4, kInt4},
          {TYPE_ID_UINT2, kUInt2},
          {TYPE_ID_UINT3, kUInt3},
          {TYPE_ID_UINT4, kUInt4},
          {TYPE_ID_HALF2, kHalf2},
          {TYPE_ID_HALF3, kHalf3},
          {TYPE_ID_HALF4, kHalf4},
          {TYPE_ID_MATRIX2F, kMatrix2f},
          {TYPE_ID_MATRIX3F, kMatrix3f},
          {TYPE_ID_MATRIX4F, kMatrix4f},
          {TYPE_ID_MATRIX2D, kMatrix2d},
          {TYPE_ID_MATRIX3D, kMatrix3d},
          {TYPE_ID_MATRIX4D, kMatrix4d},
          {TYPE_ID_FLOAT, kFloat},
          {TYPE_ID_FLOAT2, kFloat2},
          {TYPE_ID_FLOAT3, kFloat3},
          {TYPE_ID_FLOAT4, kFloat4},
          {TYPE_ID_DOUBLE, kDouble},
          {TYPE_ID_DOUBLE2, kDouble2},
          {TYPE_ID_DOUBLE3, kDouble3},
          {TYPE_ID_DOUBLE4, kDouble4},
          {TYPE_ID_QUATH, kQuath},
          {TYPE_ID_QUATF, kQuatf},
          {TYPE_ID_QUATD, kQuatd},
          {TYPE_ID_VECTOR3H, kVector3h},
          {TYPE_ID_VECTOR3F, kVector3f},
          {TYPE_ID_VECTOR3D, kVector3d},
          {TYPE_ID_POINT3H, kPoint3h},
          {TYPE_ID_POINT3F, kPoint3f},
          {TYPE_ID_POINT3D, kPoint3d},
          {TYPE_ID_NORMAL3H, kNormal3h},
          {TYPE_ID_NORMAL3F, kNormal3f},
          {TYPE_ID_NORMAL3D, kNormal3d},
          {TYPE_ID_COLOR3H, kColor3h},
          {TYPE_ID_COLOR3F, kColor3f},
          {TYPE_ID_COLOR3D, kColor3d},
          {TYPE_ID_COLOR4H, kColor4h},
          {TYPE_ID_COLOR4F, kColor4f},
          {TYPE_ID_COLOR4D, kColor4d},
          {TYPE_ID_FRAME4D, kFrame4d},
          {TYPE_ID_TEXCOORD2H, kTexCoord2h},
          {TYPE_ID_TEXCOORD2F, kTexCoord2f},
          {TYPE_ID_TEXCOORD2D, kTexCoord2d},
          {TYPE_ID_TEXCOORD3H, kTexCoord3h},
          {TYPE_ID_TEXCOORD3F, kTexCoord3f},
          {TYPE_ID_TEXCOORD3D, kTexCoord3d},
          {TYPE_ID_RELATIONSHIP, kRelationship},
      });

  bool array_bit = (TYPE_ID_1D_ARRAY_BIT & tyid);
  uint32_t scalar_tid = tyid & (~TYPE_ID_1D_ARRAY_BIT);

  auto ret = tynamemap.find(scalar_tid);
  if (ret != tynamemap.end()) {
    std::string s = ret->second.c_str();
    if (array_bit) {
      s += "[]";
    }
    return std::move(s);
  }

  return nonstd::nullopt;
}

std::string GetTypeName(uint32_t tyid) {
  auto ret = TryGetTypeName(tyid);

  if (!ret) {
    return "(GetTypeName) [[Unknown or unimplemented/unsupported type_id: " +
           std::to_string(tyid) + "]]";
  }

  return ret.value();
}

nonstd::optional<uint32_t> TryGetTypeId(const std::string &tyname) {
  MAPBOX_ETERNAL_CONSTEXPR const auto tyidmap =
      mapbox::eternal::hash_map<mapbox::eternal::string, uint32_t>({
          {kToken, TYPE_ID_TOKEN},
          {kString, TYPE_ID_STRING},
          {kPath, TYPE_ID_STRING},
          {kAssetPath, TYPE_ID_ASSET_PATH},
          {kPathExpression, TYPE_ID_PATH_EXPRESSION},
          {kDictionary, TYPE_ID_DICT},
          {kTimeCode, TYPE_ID_TIMECODE},
          {kBool, TYPE_ID_BOOL},
          {kChar, TYPE_ID_CHAR},
          {kChar2, TYPE_ID_CHAR2},
          {kChar3, TYPE_ID_CHAR3},
          {kChar4, TYPE_ID_CHAR4},
          {kUChar, TYPE_ID_UCHAR},
          {kUChar2, TYPE_ID_UCHAR2},
          {kUChar3, TYPE_ID_UCHAR3},
          {kUChar4, TYPE_ID_UCHAR4},
          {kHalf, TYPE_ID_HALF},
          {kShort, TYPE_ID_SHORT},
          {kShort2, TYPE_ID_SHORT2},
          {kShort3, TYPE_ID_SHORT3},
          {kShort4, TYPE_ID_SHORT4},
          {kUShort, TYPE_ID_USHORT},
          {kUShort2, TYPE_ID_USHORT2},
          {kUShort3, TYPE_ID_USHORT3},
          {kUShort4, TYPE_ID_USHORT4},
          {kInt, TYPE_ID_INT32},
          {kUInt, TYPE_ID_UINT32},
          {kInt64, TYPE_ID_INT64},
          {kUInt64, TYPE_ID_UINT64},
          {kInt2, TYPE_ID_INT2},
          {kInt3, TYPE_ID_INT3},
          {kInt4, TYPE_ID_INT4},
          {kUInt2, TYPE_ID_UINT2},
          {kUInt3, TYPE_ID_UINT3},
          {kUInt4, TYPE_ID_UINT4},
          {kHalf2, TYPE_ID_HALF2},
          {kHalf3, TYPE_ID_HALF3},
          {kHalf4, TYPE_ID_HALF4},
          {kMatrix2f, TYPE_ID_MATRIX2F},
          {kMatrix3f, TYPE_ID_MATRIX3F},
          {kMatrix4f, TYPE_ID_MATRIX4F},
          {kMatrix2d, TYPE_ID_MATRIX2D},
          {kMatrix3d, TYPE_ID_MATRIX3D},
          {kMatrix4d, TYPE_ID_MATRIX4D},
          {kFloat, TYPE_ID_FLOAT},
          {kFloat2, TYPE_ID_FLOAT2},
          {kFloat3, TYPE_ID_FLOAT3},
          {kFloat4, TYPE_ID_FLOAT4},
          {kDouble, TYPE_ID_DOUBLE},
          {kDouble2, TYPE_ID_DOUBLE2},
          {kDouble3, TYPE_ID_DOUBLE3},
          {kDouble4, TYPE_ID_DOUBLE4},
          {kQuath, TYPE_ID_QUATH},
          {kQuatf, TYPE_ID_QUATF},
          {kQuatd, TYPE_ID_QUATD},
          {kVector3h, TYPE_ID_VECTOR3H},
          {kVector3f, TYPE_ID_VECTOR3F},
          {kVector3d, TYPE_ID_VECTOR3D},
          {kPoint3h, TYPE_ID_POINT3H},
          {kPoint3f, TYPE_ID_POINT3F},
          {kPoint3d, TYPE_ID_POINT3D},
          {kNormal3h, TYPE_ID_NORMAL3H},
          {kNormal3f, TYPE_ID_NORMAL3F},
          {kNormal3d, TYPE_ID_NORMAL3D},
          {kColor3h, TYPE_ID_COLOR3H},
          {kColor3f, TYPE_ID_COLOR3F},
          {kColor3d, TYPE_ID_COLOR3D},
          {kColor4h, TYPE_ID_COLOR4H},
          {kColor4f, TYPE_ID_COLOR4F},
          {kColor4d, TYPE_ID_COLOR4D},
          {kFrame4d, TYPE_ID_FRAME4D},
          {kTexCoord2h, TYPE_ID_TEXCOORD2H},
          {kTexCoord2f, TYPE_ID_TEXCOORD2F},
          {kTexCoord2d, TYPE_ID_TEXCOORD2D},
          {kTexCoord3h, TYPE_ID_TEXCOORD3H},
          {kTexCoord3f, TYPE_ID_TEXCOORD3F},
          {kTexCoord3d, TYPE_ID_TEXCOORD3D},
          {kRelationship, TYPE_ID_RELATIONSHIP},
      });

  std::string s = tyname;
  uint32_t array_bit = 0;
  if (endsWith(tyname, "[]")) {
    s = removeSuffix(s, "[]");
    array_bit |= TYPE_ID_1D_ARRAY_BIT;
  }

  // It looks USD does not support 2D array type, so no further `[]` check

  auto ret = tyidmap.find(s.c_str());
  if (ret != tyidmap.end()) {
    return ret->second | array_bit;
  }

  return nonstd::nullopt;
}

uint32_t GetTypeId(const std::string &tyname) {
  auto ret = TryGetTypeId(tyname);

  if (!ret) {
    return TYPE_ID_INVALID;
  }

  return ret.value();
}

nonstd::optional<uint32_t> TryGetUnderlyingTypeId(const std::string &tyname) {
  MAPBOX_ETERNAL_CONSTEXPR const auto utyidmap =
      mapbox::eternal::hash_map<mapbox::eternal::string, uint32_t>({
        {kPoint3h, TYPE_ID_HALF3},
        {kPoint3f, TYPE_ID_FLOAT3},
        {kPoint3d, TYPE_ID_DOUBLE3},
        {kNormal3h, TYPE_ID_HALF3},
        {kNormal3f, TYPE_ID_FLOAT3},
        {kNormal3d, TYPE_ID_DOUBLE3},
        {kVector3h, TYPE_ID_HALF3},
        {kVector3f, TYPE_ID_FLOAT3},
        {kVector3d, TYPE_ID_DOUBLE3},
        {kColor3h, TYPE_ID_HALF3},
        {kColor3f, TYPE_ID_FLOAT3},
        {kColor3d, TYPE_ID_DOUBLE3},
        {kColor4h, TYPE_ID_HALF4},
        {kColor4f, TYPE_ID_FLOAT4},
        {kColor4d, TYPE_ID_DOUBLE4},
        {kTexCoord2h, TYPE_ID_HALF2},
        {kTexCoord2f, TYPE_ID_FLOAT2},
        {kTexCoord2d, TYPE_ID_DOUBLE3},
        {kTexCoord3h, TYPE_ID_HALF3},
        {kTexCoord3f, TYPE_ID_FLOAT3},
        {kTexCoord3d, TYPE_ID_DOUBLE4},
        {kFrame4d, TYPE_ID_MATRIX4D},
  });

  {
    std::string s = tyname;
    uint32_t array_bit = 0;
    if (endsWith(tyname, "[]")) {
      s = removeSuffix(s, "[]");
      array_bit |= TYPE_ID_1D_ARRAY_BIT;
    }

    auto ret = utyidmap.find(s.c_str());
    if (ret != utyidmap.end()) {
      return ret->second | array_bit;
    }
  }

  // Fallback
  return TryGetTypeId(tyname);
}

uint32_t GetUnderlyingTypeId(const std::string &tyname) {
  auto ret = TryGetUnderlyingTypeId(tyname);

  if (!ret) {
    return TYPE_ID_INVALID;
  }

  return ret.value();
}

nonstd::optional<std::string> TryGetUnderlyingTypeName(const uint32_t tyid) {
  MAPBOX_ETERNAL_CONSTEXPR const auto utynamemap =
      mapbox::eternal::map<uint32_t, mapbox::eternal::string>({
        {TYPE_ID_POINT3H, kHalf3},
        {TYPE_ID_POINT3F, kFloat3},
        {TYPE_ID_POINT3D, kDouble3},
        {TYPE_ID_NORMAL3H, kHalf3},
        {TYPE_ID_NORMAL3F, kFloat3},
        {TYPE_ID_NORMAL3D, kDouble3},
        {TYPE_ID_VECTOR3H, kHalf3},
        {TYPE_ID_VECTOR3F, kFloat3},
        {TYPE_ID_VECTOR3D, kDouble3},
        {TYPE_ID_COLOR3H, kHalf3},
        {TYPE_ID_COLOR3F, kFloat3},
        {TYPE_ID_COLOR3D, kDouble3},
        {TYPE_ID_COLOR4H, kHalf4},
        {TYPE_ID_COLOR4F, kFloat4},
        {TYPE_ID_COLOR4D, kDouble4},
        {TYPE_ID_TEXCOORD2H, kHalf2},
        {TYPE_ID_TEXCOORD2F, kFloat2},
        {TYPE_ID_TEXCOORD2D, kDouble2},
        {TYPE_ID_TEXCOORD3H, kHalf3},
        {TYPE_ID_TEXCOORD3F, kFloat3},
        {TYPE_ID_TEXCOORD3D, kDouble3},
        {TYPE_ID_FRAME4D, kMatrix4d},
  });

  {
  bool array_bit = (TYPE_ID_1D_ARRAY_BIT & tyid);
  uint32_t scalar_tid = tyid & (~TYPE_ID_1D_ARRAY_BIT);

  auto ret = utynamemap.find(scalar_tid);
  if (ret != utynamemap.end()) {
    std::string s = ret->second.c_str();
    if (array_bit) {
      s += "[]";
    }
    return std::move(s);
  }
  }

  return TryGetTypeName(tyid);

}

std::string GetUnderlyingTypeName(uint32_t tyid) {
  auto ret = TryGetUnderlyingTypeName(tyid);

  if (!ret) {
    return "(GetUnderlyingTypeName) [[Unknown or unimplemented/unsupported type_id: " +
           std::to_string(tyid) + "]]";
  }

  return ret.value();
}

bool IsRoleType(const std::string &tyname) {
  return GetUnderlyingTypeId(tyname) != value::TYPE_ID_INVALID;
}

bool IsRoleType(const uint32_t tyid) {
  return GetUnderlyingTypeId(GetTypeName(tyid)) != value::TYPE_ID_INVALID;
}

} // namespace value
} // namespace lightusd
