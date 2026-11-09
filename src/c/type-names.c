/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * type-names.c -- built-in USD type metadata (C11 core).
 *
 * Crate/in-memory type layouts plus the USD<->TypeId name mapping. This was
 * a constexpr C++ array in src/next/types/type-info.cc; as C11 data it is
 * available to C consumers (the C ABI) and the layout is the single source
 * of truth. Metadata only: no per-type executable callbacks.
 */

#include "lightusd-core-c.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* enum lu_type_id and struct lu_type_info are declared in lightusd-core-c.h */

static const lu_type_info kLUTypes[] = {
  {LU_TYPE_INVALID, NULL, NULL, 0, 0, LU_TYPE_INVALID, 0, 0},
  {LU_TYPE_BOOL, "bool", "bool", sizeof(bool) * 1, _Alignof(bool), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_INT, "int", "int32_t", sizeof(int32_t) * 1, _Alignof(int32_t), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_UINT, "uint", "uint32_t", sizeof(uint32_t) * 1, _Alignof(uint32_t), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_INT64, "int64", "int64_t", sizeof(int64_t) * 1, _Alignof(int64_t), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_UINT64, "uint64", "uint64_t", sizeof(uint64_t) * 1, _Alignof(uint64_t), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_HALF, "half", "half", sizeof(uint16_t) * 1, _Alignof(uint16_t), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_FLOAT, "float", "float", sizeof(float) * 1, _Alignof(float), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_DOUBLE, "double", "double", sizeof(double) * 1, _Alignof(double), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_STRING, "string", "std::string", 0, 1, LU_TYPE_INVALID, 0, 0},
  {LU_TYPE_TOKEN, "token", "Token", 0, 1, LU_TYPE_INVALID, 0, 0},
  {LU_TYPE_ASSETPATH, "asset", "AssetPath", 0, 1, LU_TYPE_INVALID, 0, 0},
  {LU_TYPE_INT2, "int2", "int2", sizeof(int32_t) * 2, _Alignof(int32_t), LU_TYPE_INT, 2, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_INT3, "int3", "int3", sizeof(int32_t) * 3, _Alignof(int32_t), LU_TYPE_INT, 3, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_INT4, "int4", "int4", sizeof(int32_t) * 4, _Alignof(int32_t), LU_TYPE_INT, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_UINT2, "uint2", "uint2", sizeof(uint32_t) * 2, _Alignof(uint32_t), LU_TYPE_UINT, 2, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_UINT3, "uint3", "uint3", sizeof(uint32_t) * 3, _Alignof(uint32_t), LU_TYPE_UINT, 3, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_UINT4, "uint4", "uint4", sizeof(uint32_t) * 4, _Alignof(uint32_t), LU_TYPE_UINT, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_HALF2, "half2", "half2", sizeof(uint16_t) * 2, _Alignof(uint16_t), LU_TYPE_HALF, 2, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_HALF3, "half3", "half3", sizeof(uint16_t) * 3, _Alignof(uint16_t), LU_TYPE_HALF, 3, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_HALF4, "half4", "half4", sizeof(uint16_t) * 4, _Alignof(uint16_t), LU_TYPE_HALF, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_FLOAT2, "float2", "float2", sizeof(float) * 2, _Alignof(float), LU_TYPE_FLOAT, 2, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_FLOAT3, "float3", "float3", sizeof(float) * 3, _Alignof(float), LU_TYPE_FLOAT, 3, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_FLOAT4, "float4", "float4", sizeof(float) * 4, _Alignof(float), LU_TYPE_FLOAT, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_DOUBLE2, "double2", "double2", sizeof(double) * 2, _Alignof(double), LU_TYPE_DOUBLE, 2, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_DOUBLE3, "double3", "double3", sizeof(double) * 3, _Alignof(double), LU_TYPE_DOUBLE, 3, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_DOUBLE4, "double4", "double4", sizeof(double) * 4, _Alignof(double), LU_TYPE_DOUBLE, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_QUATH, "quath", "quath", sizeof(uint16_t) * 4, _Alignof(uint16_t), LU_TYPE_HALF, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_QUATF, "quatf", "quatf", sizeof(float) * 4, _Alignof(float), LU_TYPE_FLOAT, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_QUATD, "quatd", "quatd", sizeof(double) * 4, _Alignof(double), LU_TYPE_DOUBLE, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_POINT3H, "point3h", "point3h", sizeof(uint16_t) * 3, _Alignof(uint16_t), LU_TYPE_HALF, 3, 0},
  {LU_TYPE_POINT3F, "point3f", "point3f", sizeof(float) * 3, _Alignof(float), LU_TYPE_FLOAT, 3, 0},
  {LU_TYPE_POINT3D, "point3d", "point3d", sizeof(double) * 3, _Alignof(double), LU_TYPE_DOUBLE, 3, 0},
  {LU_TYPE_VECTOR3H, "vector3h", "vector3h", sizeof(uint16_t) * 3, _Alignof(uint16_t), LU_TYPE_HALF, 3, 0},
  {LU_TYPE_VECTOR3F, "vector3f", "vector3f", sizeof(float) * 3, _Alignof(float), LU_TYPE_FLOAT, 3, 0},
  {LU_TYPE_VECTOR3D, "vector3d", "vector3d", sizeof(double) * 3, _Alignof(double), LU_TYPE_DOUBLE, 3, 0},
  {LU_TYPE_NORMAL3H, "normal3h", "normal3h", sizeof(uint16_t) * 3, _Alignof(uint16_t), LU_TYPE_HALF, 3, 0},
  {LU_TYPE_NORMAL3F, "normal3f", "normal3f", sizeof(float) * 3, _Alignof(float), LU_TYPE_FLOAT, 3, 0},
  {LU_TYPE_NORMAL3D, "normal3d", "normal3d", sizeof(double) * 3, _Alignof(double), LU_TYPE_DOUBLE, 3, 0},
  {LU_TYPE_COLOR3H, "color3h", "color3h", sizeof(uint16_t) * 3, _Alignof(uint16_t), LU_TYPE_HALF, 3, 0},
  {LU_TYPE_COLOR3F, "color3f", "color3f", sizeof(float) * 3, _Alignof(float), LU_TYPE_FLOAT, 3, 0},
  {LU_TYPE_COLOR3D, "color3d", "color3d", sizeof(double) * 3, _Alignof(double), LU_TYPE_DOUBLE, 3, 0},
  {LU_TYPE_COLOR4H, "color4h", "color4h", sizeof(uint16_t) * 4, _Alignof(uint16_t), LU_TYPE_HALF, 4, 0},
  {LU_TYPE_COLOR4F, "color4f", "color4f", sizeof(float) * 4, _Alignof(float), LU_TYPE_FLOAT, 4, 0},
  {LU_TYPE_COLOR4D, "color4d", "color4d", sizeof(double) * 4, _Alignof(double), LU_TYPE_DOUBLE, 4, 0},
  {LU_TYPE_MATRIX2F, "matrix2f", "matrix2f", sizeof(float) * 4, _Alignof(float), LU_TYPE_FLOAT, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_MATRIX2D, "matrix2d", "matrix2d", sizeof(double) * 4, _Alignof(double), LU_TYPE_DOUBLE, 4, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_MATRIX3F, "matrix3f", "matrix3f", sizeof(float) * 9, _Alignof(float), LU_TYPE_FLOAT, 9, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_MATRIX3D, "matrix3d", "matrix3d", sizeof(double) * 9, _Alignof(double), LU_TYPE_DOUBLE, 9, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_MATRIX4F, "matrix4f", "matrix4f", sizeof(float) * 16, _Alignof(float), LU_TYPE_FLOAT, 16, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_MATRIX4D, "matrix4d", "matrix4d", sizeof(double) * 16, _Alignof(double), LU_TYPE_DOUBLE, 16, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_TEXCOORD2H, "texCoord2h", "texcoord2h", sizeof(uint16_t) * 2, _Alignof(uint16_t), LU_TYPE_HALF, 2, 0},
  {LU_TYPE_TEXCOORD2F, "texCoord2f", "texcoord2f", sizeof(float) * 2, _Alignof(float), LU_TYPE_FLOAT, 2, 0},
  {LU_TYPE_TEXCOORD2D, "texCoord2d", "texcoord2d", sizeof(double) * 2, _Alignof(double), LU_TYPE_DOUBLE, 2, 0},
  {LU_TYPE_TEXCOORD3H, "texCoord3h", "texcoord3h", sizeof(uint16_t) * 3, _Alignof(uint16_t), LU_TYPE_HALF, 3, 0},
  {LU_TYPE_TEXCOORD3F, "texCoord3f", "texcoord3f", sizeof(float) * 3, _Alignof(float), LU_TYPE_FLOAT, 3, 0},
  {LU_TYPE_TEXCOORD3D, "texCoord3d", "texcoord3d", sizeof(double) * 3, _Alignof(double), LU_TYPE_DOUBLE, 3, 0},
  {LU_TYPE_TIMECODE, "timecode", "TimeCode", sizeof(double) * 1, _Alignof(double), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR},
  {LU_TYPE_EXTENT, "float3[]", "extent", sizeof(float) * 6, _Alignof(float), LU_TYPE_INVALID, 0, 0},
  {LU_TYPE_DICTIONARY, "dictionary", "Dictionary", 0, 1, LU_TYPE_INVALID, 0, 0},
  {LU_TYPE_RELATIONSHIP, "rel", "Relationship", 0, 1, LU_TYPE_INVALID, 0, 0},
  {LU_TYPE_REFERENCE, "reference", "Reference", 0, 1, LU_TYPE_INVALID, 0, 0},
  {LU_TYPE_UCHAR, "uchar", "uint8_t", sizeof(uint8_t) * 1, _Alignof(uint8_t), LU_TYPE_INVALID, 1, LU_TYPE_FLAG_SCALAR|LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_FRAME4D, "frame4d", "frame4d", sizeof(double) * 16, _Alignof(double), LU_TYPE_DOUBLE, 16, LU_TYPE_FLAG_NUMERIC},
  {LU_TYPE_PATHEXPRESSION, "pathExpression", "PathExpression", 0, 1, LU_TYPE_INVALID, 0, 0},
};

static const size_t kLUTypesCount = sizeof(kLUTypes) / sizeof(kLUTypes[0]);

const lu_type_info *lu_type_info_at(uint16_t id) {
  return id < kLUTypesCount ? &kLUTypes[id] : NULL;
}

const char *lu_type_name(uint16_t id) {
  const lu_type_info *t = lu_type_info_at(id);
  return t ? t->name : NULL;
}

uint16_t lu_type_id_from_name(const char *name, size_t len) {
  if (name == NULL) return LU_TYPE_INVALID;
  /* The table has fewer than 80 rows; a bounded linear scan beats a generated
     hash and is called only at parse/resolve time. */
  for (size_t i = 1; i < kLUTypesCount; ++i) {
    const char *cand = kLUTypes[i].name;
    if (cand != NULL && strlen(cand) == len && memcmp(cand, name, len) == 0)
      return (uint16_t)i;
  }
  return LU_TYPE_INVALID;
}

size_t lu_type_size(uint16_t id) { const lu_type_info *t = lu_type_info_at(id); return t ? t->size : 0; }
size_t lu_type_alignment(uint16_t id) { const lu_type_info *t = lu_type_info_at(id); return t ? t->alignment : 0; }
int lu_type_is_scalar(uint16_t id) { const lu_type_info *t = lu_type_info_at(id); return t && (t->flags & LU_TYPE_FLAG_SCALAR); }
int lu_type_is_numeric(uint16_t id) { const lu_type_info *t = lu_type_info_at(id); return t && (t->flags & LU_TYPE_FLAG_NUMERIC); }
uint16_t lu_type_component_type(uint16_t id) { const lu_type_info *t = lu_type_info_at(id); return t ? t->component_type : LU_TYPE_INVALID; }
uint8_t lu_type_component_count(uint16_t id) { const lu_type_info *t = lu_type_info_at(id); return t ? t->component_count : 0; }
