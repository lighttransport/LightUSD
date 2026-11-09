// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
#if defined(__wasi__)
#else
#include <thread>
#endif

#include "common-macros.inc"
#include "crate-format.hh"
#include "external/mapbox/eternal/include/mapbox/eternal.hpp"
#include "value-types.hh"

namespace lightusd {
namespace crate {

// Section constructor implementation
Section::Section(char const *name_, int64_t start_, int64_t size_)
  : start(start_), size(size_) {
  memset(this->name, 0, sizeof(this->name));
  if (name_) {
    strncpy(this->name, name_, kSectionNameMaxLength);
  }
}


nonstd::expected<CrateDataType, std::string> GetCrateDataType(int32_t type_id) {
  // See <pxrUSD>/pxr/usd/usd/crateDataTypes.h

  // TODO: Use type name in value-types.hh and prim-types.hh?
  MAPBOX_ETERNAL_CONSTEXPR const auto tymap =
      mapbox::eternal::map<CrateDataTypeId, mapbox::eternal::string>({
          {CrateDataTypeId::CRATE_DATA_TYPE_INVALID, "Invalid"},  // 0
          {CrateDataTypeId::CRATE_DATA_TYPE_BOOL, "Bool"},
          {CrateDataTypeId::CRATE_DATA_TYPE_UCHAR, "UChar"},
          {CrateDataTypeId::CRATE_DATA_TYPE_INT, "Int"},
          {CrateDataTypeId::CRATE_DATA_TYPE_UINT, "UInt"},
          {CrateDataTypeId::CRATE_DATA_TYPE_INT64, "Int64"},
          {CrateDataTypeId::CRATE_DATA_TYPE_UINT64, "UInt64"},

          {CrateDataTypeId::CRATE_DATA_TYPE_HALF, "Half"},
          {CrateDataTypeId::CRATE_DATA_TYPE_FLOAT, "Float"},
          {CrateDataTypeId::CRATE_DATA_TYPE_DOUBLE, "Double"},

          {CrateDataTypeId::CRATE_DATA_TYPE_STRING, "String"},
          {CrateDataTypeId::CRATE_DATA_TYPE_TOKEN, "Token"},
          {CrateDataTypeId::CRATE_DATA_TYPE_ASSET_PATH, "AssetPath"},

          {CrateDataTypeId::CRATE_DATA_TYPE_MATRIX2D, "Matrix2d"},
          {CrateDataTypeId::CRATE_DATA_TYPE_MATRIX3D, "Matrix3d"},
          {CrateDataTypeId::CRATE_DATA_TYPE_MATRIX4D, "Matrix4d"},

          {CrateDataTypeId::CRATE_DATA_TYPE_QUATD, "Quatd"},
          {CrateDataTypeId::CRATE_DATA_TYPE_QUATF, "Quatf"},
          {CrateDataTypeId::CRATE_DATA_TYPE_QUATH, "Quath"},

          {CrateDataTypeId::CRATE_DATA_TYPE_VEC2D, "Vec2d"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC2F, "Vec2f"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC2H, "Vec2h"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC2I, "Vec2i"},

          {CrateDataTypeId::CRATE_DATA_TYPE_VEC3D, "Vec3d"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC3F, "Vec3f"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC3H, "Vec3h"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC3I, "Vec3i"},

          {CrateDataTypeId::CRATE_DATA_TYPE_VEC4D, "Vec4d"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC4F, "Vec4f"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC4H, "Vec4h"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC4I, "Vec4i"},

          // Non-array types.
          {CrateDataTypeId::CRATE_DATA_TYPE_DICTIONARY, "Dictionary"},
          {CrateDataTypeId::CRATE_DATA_TYPE_TOKEN_LIST_OP, "TokenListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_STRING_LIST_OP, "StringListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_PATH_LIST_OP, "PathListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_REFERENCE_LIST_OP,
           "ReferenceListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_INT_LIST_OP, "IntListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_INT64_LIST_OP, "Int64ListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_UINT_LIST_OP, "UIntListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_UINT64_LIST_OP, "UInt64ListOp"},

          {CrateDataTypeId::CRATE_DATA_TYPE_PATH_VECTOR, "PathVector"},
          {CrateDataTypeId::CRATE_DATA_TYPE_TOKEN_VECTOR, "TokenVector"},
          {CrateDataTypeId::CRATE_DATA_TYPE_SPECIFIER, "Specifier"},
          {CrateDataTypeId::CRATE_DATA_TYPE_PERMISSION, "Permission"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VARIABILITY, "Variability"},

          {CrateDataTypeId::CRATE_DATA_TYPE_VARIANT_SELECTION_MAP,
           "VariantSelectionMap"},
          {CrateDataTypeId::CRATE_DATA_TYPE_TIME_SAMPLES, "TimeSamples"},
          {CrateDataTypeId::CRATE_DATA_TYPE_PAYLOAD, "Payload"},
          {CrateDataTypeId::CRATE_DATA_TYPE_DOUBLE_VECTOR, "DoubleVector"},
          {CrateDataTypeId::CRATE_DATA_TYPE_LAYER_OFFSET_VECTOR,
           "LayerOffsetVector"},
          {CrateDataTypeId::CRATE_DATA_TYPE_STRING_VECTOR, "StringVector"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VALUE_BLOCK, "ValueBlock"},
          {CrateDataTypeId::CRATE_DATA_TYPE_VALUE, "Value"},
          {CrateDataTypeId::CRATE_DATA_TYPE_UNREGISTERED_VALUE,
           "UnregisteredValue"},
          {CrateDataTypeId::CRATE_DATA_TYPE_UNREGISTERED_VALUE_LIST_OP,
           "UnregisteredValueListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_PAYLOAD_LIST_OP, "PayloadListOp"},
          {CrateDataTypeId::CRATE_DATA_TYPE_TIME_CODE, "TimeCode"},
          {CrateDataTypeId::CRATE_DATA_TYPE_PATH_EXPRESSION, "PathExpression"},
          {CrateDataTypeId::CRATE_DATA_TYPE_RELOCATES, "Relocates"},
          {CrateDataTypeId::CRATE_DATA_TYPE_SPLINE, "Spline"},
          {CrateDataTypeId::CRATE_DATA_TYPE_ANIMATION_BLOCK, "AnimationBlock"},
      });

  // List up `supports array` type.
  // TODO: Use compile-time `set`
  MAPBOX_ETERNAL_CONSTEXPR const auto arrmap =
      mapbox::eternal::map<CrateDataTypeId, bool>({
          {CrateDataTypeId::CRATE_DATA_TYPE_BOOL, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_UCHAR, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_INT, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_UINT, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_INT64, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_UINT64, true},

          {CrateDataTypeId::CRATE_DATA_TYPE_HALF, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_FLOAT, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_DOUBLE, true},

          {CrateDataTypeId::CRATE_DATA_TYPE_STRING, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_TOKEN, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_ASSET_PATH, true},

          {CrateDataTypeId::CRATE_DATA_TYPE_MATRIX2D, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_MATRIX3D, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_MATRIX4D, true},

          {CrateDataTypeId::CRATE_DATA_TYPE_QUATD, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_QUATF, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_QUATH, true},

          {CrateDataTypeId::CRATE_DATA_TYPE_VEC2D, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC2F, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC2H, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC2I, true},

          {CrateDataTypeId::CRATE_DATA_TYPE_VEC3D, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC3F, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC3H, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC3I, true},

          {CrateDataTypeId::CRATE_DATA_TYPE_VEC4D, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC4F, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC4H, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_VEC4I, true},
          {CrateDataTypeId::CRATE_DATA_TYPE_TIME_CODE, true},
          // SdfPathExpression supports arrays; TsSpline (59) does not.
          {CrateDataTypeId::CRATE_DATA_TYPE_PATH_EXPRESSION, true},
      });

  if (type_id < 0) {
    return nonstd::make_unexpected("Unknown type id: " +
                                   std::to_string(type_id));
  }

  auto tyret = tymap.find(static_cast<CrateDataTypeId>(type_id));

  if (tyret == tymap.end()) {
    // Invalid or unsupported.
    return nonstd::make_unexpected("Unknown or unspported type id: " +
                                   std::to_string(type_id));
  }

  bool supports_array = arrmap.count(static_cast<CrateDataTypeId>(type_id));

  CrateDataType dst(tyret->second.data(), static_cast<CrateDataTypeId>(type_id),
                    supports_array);

  return std::move(dst);
}

std::string GetCrateDataTypeRepr(CrateDataType dty) {
  auto tyRet = GetCrateDataType(static_cast<int32_t>(dty.dtype_id));
  if (!tyRet) {
    return "[Invalid]";
  }

  const CrateDataType ty = tyRet.value();

  std::stringstream ss;
  ss << "CrateDataType: " << ty.name << "("
     << static_cast<uint32_t>(ty.dtype_id)
     << "), supports_array = " << ty.supports_array;
  return ss.str();
}

std::string GetCrateDataTypeName(int32_t type_id) {
  auto tyRet = GetCrateDataType(type_id);
  if (!tyRet) {
    return "[Invalid]";
  }

  const CrateDataType dty = tyRet.value();
  return dty.name;
}

std::string GetCrateDataTypeName(CrateDataTypeId did) {
  return GetCrateDataTypeName(static_cast<int32_t>(did));
}

// std::string CrateValue::GetTypeName() const { return value_.type_name(); }
// uint32_t CrateValue::GetTypeId() const { return value_.type_id(); }

}  // namespace crate
}  // namespace lightusd
