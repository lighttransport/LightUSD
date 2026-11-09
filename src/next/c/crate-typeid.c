/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * crate-typeid.c -- C11 crate value-type metadata (name + fixed size).
 *
 * The `CrateTypeId` enum's diagnostic name and fixed element size, keyed by the
 * wire value. Values and the size table mirror src/next/crate/crate-format.hh;
 * the C++ accessors forward so the table lives once and C consumers (the C ABI)
 * can reuse it.
 */

#include "lightusd-core-c.h"

const char *lu_crate_type_name(uint8_t id) {
  switch (id) {
    case LU_CRATE_TYPE_INVALID: return "Invalid";
    case LU_CRATE_TYPE_BOOL: return "Bool";
    case LU_CRATE_TYPE_UCHAR: return "UChar";
    case LU_CRATE_TYPE_INT: return "Int";
    case LU_CRATE_TYPE_UINT: return "UInt";
    case LU_CRATE_TYPE_INT64: return "Int64";
    case LU_CRATE_TYPE_UINT64: return "UInt64";
    case LU_CRATE_TYPE_HALF: return "Half";
    case LU_CRATE_TYPE_FLOAT: return "Float";
    case LU_CRATE_TYPE_DOUBLE: return "Double";
    case LU_CRATE_TYPE_STRING: return "String";
    case LU_CRATE_TYPE_TOKEN: return "Token";
    case LU_CRATE_TYPE_ASSETPATH: return "AssetPath";
    case LU_CRATE_TYPE_MATRIX2D: return "Matrix2d";
    case LU_CRATE_TYPE_MATRIX3D: return "Matrix3d";
    case LU_CRATE_TYPE_MATRIX4D: return "Matrix4d";
    case LU_CRATE_TYPE_QUATD: return "Quatd";
    case LU_CRATE_TYPE_QUATF: return "Quatf";
    case LU_CRATE_TYPE_QUATH: return "Quath";
    case LU_CRATE_TYPE_VEC2D: return "Vec2d";
    case LU_CRATE_TYPE_VEC2F: return "Vec2f";
    case LU_CRATE_TYPE_VEC2H: return "Vec2h";
    case LU_CRATE_TYPE_VEC2I: return "Vec2i";
    case LU_CRATE_TYPE_VEC3D: return "Vec3d";
    case LU_CRATE_TYPE_VEC3F: return "Vec3f";
    case LU_CRATE_TYPE_VEC3H: return "Vec3h";
    case LU_CRATE_TYPE_VEC3I: return "Vec3i";
    case LU_CRATE_TYPE_VEC4D: return "Vec4d";
    case LU_CRATE_TYPE_VEC4F: return "Vec4f";
    case LU_CRATE_TYPE_VEC4H: return "Vec4h";
    case LU_CRATE_TYPE_VEC4I: return "Vec4i";
    case LU_CRATE_TYPE_DICTIONARY: return "Dictionary";
    case LU_CRATE_TYPE_TOKENLISTOP: return "TokenListOp";
    case LU_CRATE_TYPE_STRINGLISTOP: return "StringListOp";
    case LU_CRATE_TYPE_PATHLISTOP: return "PathListOp";
    case LU_CRATE_TYPE_REFERENCELISTOP: return "ReferenceListOp";
    case LU_CRATE_TYPE_INTLISTOP: return "IntListOp";
    case LU_CRATE_TYPE_INT64LISTOP: return "Int64ListOp";
    case LU_CRATE_TYPE_UINTLISTOP: return "UIntListOp";
    case LU_CRATE_TYPE_UINT64LISTOP: return "UInt64ListOp";
    case LU_CRATE_TYPE_PATHVECTOR: return "PathVector";
    case LU_CRATE_TYPE_TOKENVECTOR: return "TokenVector";
    case LU_CRATE_TYPE_SPECIFIER: return "Specifier";
    case LU_CRATE_TYPE_PERMISSION: return "Permission";
    case LU_CRATE_TYPE_VARIABILITY: return "Variability";
    case LU_CRATE_TYPE_VARIANTSELECTIONMAP: return "VariantSelectionMap";
    case LU_CRATE_TYPE_TIMESAMPLES: return "TimeSamples";
    case LU_CRATE_TYPE_PAYLOAD: return "Payload";
    case LU_CRATE_TYPE_DOUBLEVECTOR: return "DoubleVector";
    case LU_CRATE_TYPE_LAYEROFFSETVECTOR: return "LayerOffsetVector";
    case LU_CRATE_TYPE_STRINGVECTOR: return "StringVector";
    case LU_CRATE_TYPE_VALUEBLOCK: return "ValueBlock";
    case LU_CRATE_TYPE_VALUE: return "Value";
    case LU_CRATE_TYPE_UNREGISTEREDVALUE: return "UnregisteredValue";
    case LU_CRATE_TYPE_UNREGISTEREDVALUELISTOP: return "UnregisteredValueListOp";
    case LU_CRATE_TYPE_PAYLOADLISTOP: return "PayloadListOp";
    case LU_CRATE_TYPE_TIMECODE: return "TimeCode";
    case LU_CRATE_TYPE_PATHEXPRESSION: return "PathExpression";
    case LU_CRATE_TYPE_SPLINE: return "Spline";
    case LU_CRATE_TYPE_RELOCATES: return "Relocates";
    default: return "Unknown";
  }
}

size_t lu_crate_type_size(uint8_t id) {
  switch (id) {
    case LU_CRATE_TYPE_BOOL: return 1;
    case LU_CRATE_TYPE_UCHAR: return 1;
    case LU_CRATE_TYPE_INT: return 4;
    case LU_CRATE_TYPE_UINT: return 4;
    case LU_CRATE_TYPE_INT64: return 8;
    case LU_CRATE_TYPE_UINT64: return 8;
    case LU_CRATE_TYPE_HALF: return 2;
    case LU_CRATE_TYPE_FLOAT: return 4;
    case LU_CRATE_TYPE_DOUBLE: return 8;
    case LU_CRATE_TYPE_MATRIX2D: return 4 * 8;
    case LU_CRATE_TYPE_MATRIX3D: return 9 * 8;
    case LU_CRATE_TYPE_MATRIX4D: return 16 * 8;
    case LU_CRATE_TYPE_QUATD: return 4 * 8;
    case LU_CRATE_TYPE_QUATF: return 4 * 4;
    case LU_CRATE_TYPE_QUATH: return 4 * 2;
    case LU_CRATE_TYPE_VEC2D: return 2 * 8;
    case LU_CRATE_TYPE_VEC2F: return 2 * 4;
    case LU_CRATE_TYPE_VEC2H: return 2 * 2;
    case LU_CRATE_TYPE_VEC2I: return 2 * 4;
    case LU_CRATE_TYPE_VEC3D: return 3 * 8;
    case LU_CRATE_TYPE_VEC3F: return 3 * 4;
    case LU_CRATE_TYPE_VEC3H: return 3 * 2;
    case LU_CRATE_TYPE_VEC3I: return 3 * 4;
    case LU_CRATE_TYPE_VEC4D: return 4 * 8;
    case LU_CRATE_TYPE_VEC4F: return 4 * 4;
    case LU_CRATE_TYPE_VEC4H: return 4 * 2;
    case LU_CRATE_TYPE_VEC4I: return 4 * 4;
    case LU_CRATE_TYPE_TIMECODE: return 8;
    default:
      return 0; /* variable-size or unknown */
  }
}
