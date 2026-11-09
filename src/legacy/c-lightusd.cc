// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment Inc.

#include "c-lightusd.h"

#include "lightusd.hh"
#include "tydra/scene-access.hh"
#include "usdLux.hh"
#include "usdPhysics.hh"
#include "usdSkel.hh"
#include "prim-pprint.hh"
#include "value-pprint.hh"
#include "common-macros.inc"
#include "str-util.hh"
#include "value-types.hh"

// - [ ] Implement our own `strlen`

CLightUSDValueType c_lightusd_value_type(const CLightUSDValue *value) {
  if (!value) {
    return C_LIGHTUSD_VALUE_UNKNOWN;
  }

  const lightusd::value::Value *pv = reinterpret_cast<const lightusd::value::Value *>(value);
  uint32_t tyid = pv->type_id();

  bool is_array = false;
  if (tyid & lightusd::value::TYPE_ID_1D_ARRAY_BIT) {
    is_array = true;
    // turn of array bit
    tyid = tyid & (~lightusd::value::TYPE_ID_1D_ARRAY_BIT);
  }

  using namespace lightusd::value;

  uint32_t basety = C_LIGHTUSD_VALUE_UNKNOWN;

  switch (tyid) {
    case TYPE_ID_BOOL: {
      basety = C_LIGHTUSD_VALUE_BOOL;
      break;
    }
    case TYPE_ID_INT32: {
      basety = C_LIGHTUSD_VALUE_INT;
      break;
    }
    case TYPE_ID_INT2: {
      basety = C_LIGHTUSD_VALUE_INT2;
      break;
    }
    case TYPE_ID_INT3: {
      basety = C_LIGHTUSD_VALUE_INT3;
      break;
    }
    case TYPE_ID_INT4: {
      basety = C_LIGHTUSD_VALUE_INT4;
      break;
    }
    case TYPE_ID_UINT32: {
      basety = C_LIGHTUSD_VALUE_UINT;
      break;
    }
    case TYPE_ID_UINT2: {
      basety = C_LIGHTUSD_VALUE_UINT2;
      break;
    }
    case TYPE_ID_UINT3: {
      basety = C_LIGHTUSD_VALUE_UINT3;
      break;
    }
    case TYPE_ID_UINT4: {
      basety = C_LIGHTUSD_VALUE_UINT4;
      break;
    }
    case TYPE_ID_CUSTOMDATA: {
      basety = C_LIGHTUSD_VALUE_DICTIONARY;
      break;
    }
    default: {
      break;
    }
  }

  if (is_array) {
    return static_cast<CLightUSDValueType>(basety | C_LIGHTUSD_VALUE_1D_BIT);
  } else {
    return static_cast<CLightUSDValueType>(basety);
  }
}

const char *c_lightusd_value_type_name(CLightUSDValueType value_type) {
  // 32 should be enough length to support all C_LIGHTUSD_VALUE_* type name +
  // '[]'
  static thread_local char buf[32];

  bool is_array = value_type & C_LIGHTUSD_VALUE_1D_BIT;

  // drop array bit.
  uint32_t basety = value_type & (~C_LIGHTUSD_VALUE_1D_BIT);

  const char *tyname = "[invalid]";

  switch (static_cast<CLightUSDValueType>(basety)) {
    case C_LIGHTUSD_VALUE_UNKNOWN: {
      break;
    }
    case C_LIGHTUSD_VALUE_BOOL: {
      tyname = "bool";
      break;
    }
    case C_LIGHTUSD_VALUE_TOKEN: {
      tyname = "token";
      break;
    }
    case C_LIGHTUSD_VALUE_TOKEN_VECTOR: {
      tyname = "token[]";
      is_array = false;
      break;
    }
    case C_LIGHTUSD_VALUE_STRING: {
      tyname = "string";
      break;
    }
    case C_LIGHTUSD_VALUE_STRING_VECTOR: {
      tyname = "string[]";
      is_array = false;
      break;
    }
    case C_LIGHTUSD_VALUE_HALF: {
      tyname = "half";
      break;
    }
    case C_LIGHTUSD_VALUE_HALF2: {
      tyname = "half2";
      break;
    }
    case C_LIGHTUSD_VALUE_HALF3: {
      tyname = "half3";
      break;
    }
    case C_LIGHTUSD_VALUE_HALF4: {
      tyname = "half4";
      break;
    }
    case C_LIGHTUSD_VALUE_INT: {
      tyname = "int";
      break;
    }
    case C_LIGHTUSD_VALUE_INT2: {
      tyname = "int2";
      break;
    }
    case C_LIGHTUSD_VALUE_INT3: {
      tyname = "int3";
      break;
    }
    case C_LIGHTUSD_VALUE_INT4: {
      tyname = "int4";
      break;
    }
    case C_LIGHTUSD_VALUE_UINT: {
      tyname = "uint";
      break;
    }
    case C_LIGHTUSD_VALUE_UINT2: {
      tyname = "uint2";
      break;
    }
    case C_LIGHTUSD_VALUE_UINT3: {
      tyname = "uint3";
      break;
    }
    case C_LIGHTUSD_VALUE_UINT4: {
      tyname = "uint4";
      break;
    }
    case C_LIGHTUSD_VALUE_INT64: {
      tyname = "int64";
      break;
    }
    case C_LIGHTUSD_VALUE_UINT64: {
      tyname = "uint64";
      break;
    }
    case C_LIGHTUSD_VALUE_FLOAT: {
      tyname = "float";
      break;
    }
    case C_LIGHTUSD_VALUE_FLOAT2: {
      tyname = "float2";
      break;
    }
    case C_LIGHTUSD_VALUE_FLOAT3: {
      tyname = "float3";
      break;
    }
    case C_LIGHTUSD_VALUE_FLOAT4: {
      tyname = "float4";
      break;
    }
    case C_LIGHTUSD_VALUE_DOUBLE: {
      tyname = "double";
      break;
    }
    case C_LIGHTUSD_VALUE_DOUBLE2: {
      tyname = "double2";
      break;
    }
    case C_LIGHTUSD_VALUE_DOUBLE3: {
      tyname = "double3";
      break;
    }
    case C_LIGHTUSD_VALUE_DOUBLE4: {
      tyname = "double4";
      break;
    }
    case C_LIGHTUSD_VALUE_QUATH: {
      tyname = "quath";
      break;
    }
    case C_LIGHTUSD_VALUE_QUATF: {
      tyname = "quatf";
      break;
    }
    case C_LIGHTUSD_VALUE_QUATD: {
      tyname = "quatd";
      break;
    }
    case C_LIGHTUSD_VALUE_NORMAL3H: {
      tyname = "normal3h";
      break;
    }
    case C_LIGHTUSD_VALUE_NORMAL3F: {
      tyname = "normal3f";
      break;
    }
    case C_LIGHTUSD_VALUE_NORMAL3D: {
      tyname = "normal3d";
      break;
    }
    case C_LIGHTUSD_VALUE_VECTOR3H: {
      tyname = "vector3h";
      break;
    }
    case C_LIGHTUSD_VALUE_VECTOR3F: {
      tyname = "vector3f";
      break;
    }
    case C_LIGHTUSD_VALUE_VECTOR3D: {
      tyname = "vector3d";
      break;
    }
    case C_LIGHTUSD_VALUE_POINT3H: {
      tyname = "point3h";
      break;
    }
    case C_LIGHTUSD_VALUE_POINT3F: {
      tyname = "point3f";
      break;
    }
    case C_LIGHTUSD_VALUE_POINT3D: {
      tyname = "point3d";
      break;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2H: {
      tyname = "texCoord2h";
      break;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2F: {
      tyname = "texCoord2f";
      break;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2D: {
      tyname = "texCoord2d";
      break;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3H: {
      tyname = "texCoord3h";
      break;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3F: {
      tyname = "texCoord3f";
      break;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3D: {
      tyname = "texCoord3d";
      break;
    }
    case C_LIGHTUSD_VALUE_COLOR3H: {
      tyname = "color3h";
      break;
    }
    case C_LIGHTUSD_VALUE_COLOR3F: {
      tyname = "color3f";
      break;
    }
    case C_LIGHTUSD_VALUE_COLOR3D: {
      tyname = "color3d";
      break;
    }
    case C_LIGHTUSD_VALUE_COLOR4H: {
      tyname = "color4h";
      break;
    }
    case C_LIGHTUSD_VALUE_COLOR4F: {
      tyname = "color4f";
      break;
    }
    case C_LIGHTUSD_VALUE_COLOR4D: {
      tyname = "color4d";
      break;
    }
    case C_LIGHTUSD_VALUE_MATRIX2D: {
      tyname = "matrix2d";
      break;
    }
    case C_LIGHTUSD_VALUE_MATRIX3D: {
      tyname = "matrix3d";
      break;
    }
    case C_LIGHTUSD_VALUE_MATRIX4D: {
      tyname = "matrix4d";
      break;
    }
    case C_LIGHTUSD_VALUE_FRAME4D: {
      tyname = "frame4d";
      break;
    }
    case C_LIGHTUSD_VALUE_DICTIONARY: {
      tyname = "dictionary";
      break;
    }
    case C_LIGHTUSD_VALUE_END: {
      tyname = "[invalid]";
      break;
    }  // invalid
       // default: { return 0; }
  }

  uint32_t sz = static_cast<uint32_t>(strlen(tyname));

  if (sz > 31) {
    // Just in case: this should not happen though.
    sz = 31;
  }

  memcpy(buf, tyname, sz);

  if (is_array) {
    if (sz > 29) {
      // Just in case: this should not happen though.
      sz = 29;
    }

    buf[sz] = '[';
    buf[sz + 1] = ']';
    buf[sz + 2] = '\0';
  } else {
    buf[sz] = '\0';
  }

  return buf;
}

uint32_t c_lightusd_value_type_components(CLightUSDValueType value_type) {
  // drop array bit.
  uint32_t basety = value_type & (~C_LIGHTUSD_VALUE_1D_BIT);

  switch (static_cast<CLightUSDValueType>(basety)) {
    case C_LIGHTUSD_VALUE_UNKNOWN: {
      return 0; // invalid
    }
    case C_LIGHTUSD_VALUE_BOOL: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TOKEN: {
      return 0;
    }  // invalid
    case C_LIGHTUSD_VALUE_TOKEN_VECTOR: {
      return 0;
    }  // invalid
    case C_LIGHTUSD_VALUE_STRING: {
      return 0;
    }  // invalid
    case C_LIGHTUSD_VALUE_STRING_VECTOR: {
      return 0;
    }  // invalid
    case C_LIGHTUSD_VALUE_HALF: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_HALF2: {
      return 2;
    }
    case C_LIGHTUSD_VALUE_HALF3: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_HALF4: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_INT: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_INT2: {
      return 2;
    }
    case C_LIGHTUSD_VALUE_INT3: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_INT4: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_UINT: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_UINT2: {
      return 2;
    }
    case C_LIGHTUSD_VALUE_UINT3: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_UINT4: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_INT64: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_UINT64: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_FLOAT: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_FLOAT2: {
      return 2;
    }
    case C_LIGHTUSD_VALUE_FLOAT3: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_FLOAT4: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_DOUBLE: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_DOUBLE2: {
      return 2;
    }
    case C_LIGHTUSD_VALUE_DOUBLE3: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_DOUBLE4: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_QUATH: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_QUATF: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_QUATD: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_NORMAL3H: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_NORMAL3F: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_NORMAL3D: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_VECTOR3H: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_VECTOR3F: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_VECTOR3D: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_POINT3H: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_POINT3F: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_POINT3D: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2H: {
      return 2;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2F: {
      return 2;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2D: {
      return 2;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3H: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3F: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3D: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_COLOR3H: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_COLOR3F: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_COLOR3D: {
      return 3;
    }
    case C_LIGHTUSD_VALUE_COLOR4H: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_COLOR4F: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_COLOR4D: {
      return 4;
    }
    case C_LIGHTUSD_VALUE_MATRIX2D: {
      return 2 * 2;
    }
    case C_LIGHTUSD_VALUE_MATRIX3D: {
      return 3 * 3;
    }
    case C_LIGHTUSD_VALUE_MATRIX4D: {
      return 4 * 4;
    }
    case C_LIGHTUSD_VALUE_FRAME4D: {
      return 4 * 4;
    }
    case C_LIGHTUSD_VALUE_DICTIONARY: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_END: {
      return 0;
    }  // invalid
       // default: { return 0; }
  }

  return 0;
}

uint32_t c_lightusd_value_type_is_numeric(CLightUSDValueType value_type) {
  // drop array bit.
  uint32_t basety = value_type & (~C_LIGHTUSD_VALUE_1D_BIT);

  switch (static_cast<CLightUSDValueType>(basety)) {
    case C_LIGHTUSD_VALUE_UNKNOWN: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_BOOL: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TOKEN: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_TOKEN_VECTOR: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_STRING: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_STRING_VECTOR: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_HALF: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_HALF2: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_HALF3: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_HALF4: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_INT: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_INT2: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_INT3: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_INT4: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_UINT: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_UINT2: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_UINT3: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_UINT4: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_INT64: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_UINT64: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_FLOAT: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_FLOAT2: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_FLOAT3: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_FLOAT4: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_DOUBLE: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_DOUBLE2: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_DOUBLE3: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_DOUBLE4: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_QUATH: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_QUATF: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_QUATD: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_NORMAL3H: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_NORMAL3F: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_NORMAL3D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_VECTOR3H: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_VECTOR3F: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_VECTOR3D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_POINT3H: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_POINT3F: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_POINT3D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2H: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2F: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3H: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3F: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_COLOR3H: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_COLOR3F: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_COLOR3D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_COLOR4H: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_COLOR4F: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_COLOR4D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_MATRIX2D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_MATRIX3D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_MATRIX4D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_FRAME4D: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_DICTIONARY: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_END: {
      return 0;
    }  // invalid
       // default: { return 0; }
  }

  return 0;
}

uint32_t c_lightusd_value_type_sizeof(CLightUSDValueType value_type) {
  // drop array bit.
  uint32_t basety = value_type & (~C_LIGHTUSD_VALUE_1D_BIT);

  switch (static_cast<CLightUSDValueType>(basety)) {
    case C_LIGHTUSD_VALUE_UNKNOWN: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_BOOL: {
      return 1;
    }
    case C_LIGHTUSD_VALUE_TOKEN: {
      return 0;
    }  // invalid
    case C_LIGHTUSD_VALUE_TOKEN_VECTOR: {
      return 0;
    }  // invalid
    case C_LIGHTUSD_VALUE_STRING: {
      return 0;
    }  // invalid
    case C_LIGHTUSD_VALUE_STRING_VECTOR: {
      return 0;
    }  // invalid
    case C_LIGHTUSD_VALUE_HALF: {
      return sizeof(uint16_t);
    }
    case C_LIGHTUSD_VALUE_HALF2: {
      return sizeof(uint16_t) * 2;
    }
    case C_LIGHTUSD_VALUE_HALF3: {
      return sizeof(uint16_t) * 3;
    }
    case C_LIGHTUSD_VALUE_HALF4: {
      return sizeof(uint16_t) * 4;
    }
    case C_LIGHTUSD_VALUE_INT: {
      return sizeof(int);
    }
    case C_LIGHTUSD_VALUE_INT2: {
      return sizeof(int) * 2;
    }
    case C_LIGHTUSD_VALUE_INT3: {
      return sizeof(int) * 3;
    }
    case C_LIGHTUSD_VALUE_INT4: {
      return sizeof(int) * 4;
    }
    case C_LIGHTUSD_VALUE_UINT: {
      return sizeof(uint32_t);
    }
    case C_LIGHTUSD_VALUE_UINT2: {
      return sizeof(uint32_t) * 2;
    }
    case C_LIGHTUSD_VALUE_UINT3: {
      return sizeof(uint32_t) * 3;
    }
    case C_LIGHTUSD_VALUE_UINT4: {
      return sizeof(uint32_t) * 4;
    }
    case C_LIGHTUSD_VALUE_INT64: {
      return sizeof(int64_t);
    }
    case C_LIGHTUSD_VALUE_UINT64: {
      return sizeof(uint64_t);
    }
    case C_LIGHTUSD_VALUE_FLOAT: {
      return sizeof(float);
    }
    case C_LIGHTUSD_VALUE_FLOAT2: {
      return sizeof(float) * 2;
    }
    case C_LIGHTUSD_VALUE_FLOAT3: {
      return sizeof(float) * 3;
    }
    case C_LIGHTUSD_VALUE_FLOAT4: {
      return sizeof(float) * 4;
    }
    case C_LIGHTUSD_VALUE_DOUBLE: {
      return sizeof(double);
    }
    case C_LIGHTUSD_VALUE_DOUBLE2: {
      return sizeof(double) * 2;
    }
    case C_LIGHTUSD_VALUE_DOUBLE3: {
      return sizeof(double) * 3;
    }
    case C_LIGHTUSD_VALUE_DOUBLE4: {
      return sizeof(double) * 4;
    }
    case C_LIGHTUSD_VALUE_QUATH: {
      return sizeof(uint16_t) * 4;
    }
    case C_LIGHTUSD_VALUE_QUATF: {
      return sizeof(float) * 4;
    }
    case C_LIGHTUSD_VALUE_QUATD: {
      return sizeof(double) * 4;
    }
    case C_LIGHTUSD_VALUE_NORMAL3H: {
      return sizeof(uint16_t) * 3;
    }
    case C_LIGHTUSD_VALUE_NORMAL3F: {
      return sizeof(float) * 3;
    }
    case C_LIGHTUSD_VALUE_NORMAL3D: {
      return sizeof(double) * 3;
    }
    case C_LIGHTUSD_VALUE_VECTOR3H: {
      return sizeof(uint16_t) * 3;
    }
    case C_LIGHTUSD_VALUE_VECTOR3F: {
      return sizeof(float) * 3;
    }
    case C_LIGHTUSD_VALUE_VECTOR3D: {
      return sizeof(double) * 3;
    }
    case C_LIGHTUSD_VALUE_POINT3H: {
      return sizeof(uint16_t) * 3;
    }
    case C_LIGHTUSD_VALUE_POINT3F: {
      return sizeof(float) * 3;
    }
    case C_LIGHTUSD_VALUE_POINT3D: {
      return sizeof(double) * 3;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2H: {
      return sizeof(uint16_t) * 2;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2F: {
      return sizeof(float) * 2;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD2D: {
      return sizeof(double) * 2;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3H: {
      return sizeof(uint16_t) * 3;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3F: {
      return sizeof(float) * 3;
    }
    case C_LIGHTUSD_VALUE_TEXCOORD3D: {
      return sizeof(double) * 3;
    }
    case C_LIGHTUSD_VALUE_COLOR3H: {
      return sizeof(uint16_t) * 3;
    }
    case C_LIGHTUSD_VALUE_COLOR3F: {
      return sizeof(float) * 3;
    }
    case C_LIGHTUSD_VALUE_COLOR3D: {
      return sizeof(double) * 3;
    }
    case C_LIGHTUSD_VALUE_COLOR4H: {
      return sizeof(uint16_t) * 4;
    }
    case C_LIGHTUSD_VALUE_COLOR4F: {
      return sizeof(float) * 4;
    }
    case C_LIGHTUSD_VALUE_COLOR4D: {
      return sizeof(double) * 4;
    }
    case C_LIGHTUSD_VALUE_MATRIX2D: {
      return sizeof(double) * 2 * 2;
    }
    case C_LIGHTUSD_VALUE_MATRIX3D: {
      return sizeof(double) * 3 * 3;
    }
    case C_LIGHTUSD_VALUE_MATRIX4D: {
      return sizeof(double) * 4 * 4;
    }
    case C_LIGHTUSD_VALUE_FRAME4D: {
      return sizeof(double) * 4 * 4;
    }
    case C_LIGHTUSD_VALUE_DICTIONARY: {
      return 0;
    }
    case C_LIGHTUSD_VALUE_END: {
      return 0;
    }  // invalid
       // default: { return 0; }
  }

  return 0;
}

CLightUSDFormat c_lightusd_detect_format(const char *filename) {
  if (lightusd::IsUSDA(filename)) {
    return C_LIGHTUSD_FORMAT_USDA;
  }

  if (lightusd::IsUSDC(filename)) {
    return C_LIGHTUSD_FORMAT_USDC;
  }

  if (lightusd::IsUSDZ(filename)) {
    return C_LIGHTUSD_FORMAT_USDZ;
  }

  return C_LIGHTUSD_FORMAT_UNKNOWN;
}

const char *c_lightusd_prim_type_name(CLightUSDPrimType prim_type) {
  // 32 should be enough length to support all C_LIGHTUSD_PRIM_*** type
  static thread_local char buf[32];

  const char *tyname = "";

  switch (prim_type) {
    case C_LIGHTUSD_PRIM_UNKNOWN: {
      return nullptr;
    }
    case C_LIGHTUSD_PRIM_MODEL: {
      // empty string for Model
      tyname = "";
      break;
    }
    case C_LIGHTUSD_PRIM_SCOPE: {
      tyname = "Scope";
      break;
    }  // empty string for Model
    case C_LIGHTUSD_PRIM_XFORM: {
      tyname = lightusd::kGeomXform;
      break;
    }
    case C_LIGHTUSD_PRIM_MESH: {
      tyname = lightusd::kGeomMesh;
      break;
    }
    case C_LIGHTUSD_PRIM_GEOMSUBSET: {
      tyname = lightusd::kGeomSubset;
      break;
    }
    case C_LIGHTUSD_PRIM_MATERIAL: {
      tyname = lightusd::kMaterial;
      break;
    }
    case C_LIGHTUSD_PRIM_SHADER: {
      tyname = lightusd::kShader;
      break;
    }
    case C_LIGHTUSD_PRIM_CAMERA: {
      tyname = lightusd::kGeomCamera;
      break;
    }
    case C_LIGHTUSD_PRIM_SPHERE_LIGHT: {
      tyname = lightusd::kSphereLight;
      break;
    }
    case C_LIGHTUSD_PRIM_DISTANT_LIGHT: {
      tyname = lightusd::kDistantLight;
      break;
    }
    case C_LIGHTUSD_PRIM_RECT_LIGHT: {
      tyname = lightusd::kRectLight;
      break;
    }
    case C_LIGHTUSD_PRIM_END: {
      return nullptr;
    }
  }

  size_t sz = strlen(tyname);
  if (sz > 31) {
    // Just in case: this should not happen though.
    sz = 31;
  }
  memcpy(buf, tyname, sz);
  buf[sz] = '\0';

  return buf;
}

CLightUSDPrimType c_lightusd_prim_type_from_string(const char *c_type_name) {
  std::string type_name(c_type_name);

  if (type_name == "Model") {
    return C_LIGHTUSD_PRIM_MODEL;
  } else if (type_name == "Scope") {
    return C_LIGHTUSD_PRIM_SCOPE;
  } else if (type_name == lightusd::kGeomXform) {
    return C_LIGHTUSD_PRIM_XFORM;
  } else if (type_name == lightusd::kGeomMesh) {
    return C_LIGHTUSD_PRIM_MESH;
  } else if (type_name == lightusd::kGeomSubset) {
    return C_LIGHTUSD_PRIM_GEOMSUBSET;
  } else if (type_name == lightusd::kGeomCamera) {
    return C_LIGHTUSD_PRIM_CAMERA;
  } else if (type_name == lightusd::kMaterial) {
    return C_LIGHTUSD_PRIM_MATERIAL;
  } else if (type_name == lightusd::kShader) {
    return C_LIGHTUSD_PRIM_SHADER;
  } else if (type_name == lightusd::kSphereLight) {
    return C_LIGHTUSD_PRIM_SPHERE_LIGHT;
  } else if (type_name == lightusd::kDistantLight) {
    return C_LIGHTUSD_PRIM_DISTANT_LIGHT;
  } else if (type_name == lightusd::kRectLight) {
    return C_LIGHTUSD_PRIM_RECT_LIGHT;
  } else {
    return C_LIGHTUSD_PRIM_UNKNOWN;
  }
}

const char *c_lightusd_prim_element_name(
    const CLightUSDPrim *prim) {

  if (!prim) {
    return nullptr;
  }

  const lightusd::Prim *p = reinterpret_cast<const lightusd::Prim *>(prim);
  return p->element_name().c_str();
}

int c_lightusd_prim_append_child(CLightUSDPrim *prim, CLightUSDPrim *child_prim) {
  DCOUT("DCOUT: Append child: " << prim << ", " << child_prim);

  if (!prim) {
    DCOUT("`prim` is nullptr.");
    return 0;
  }

  if (!child_prim) {
    DCOUT("`child_prim` is nullptr.");
    return 0;
  }

  lightusd::Prim *pprim = reinterpret_cast<lightusd::Prim *>(prim);
  lightusd::Prim *pchild = reinterpret_cast<lightusd::Prim *>(child_prim);

  pprim->children().emplace_back(*pchild);

  return 1;
}

int c_lightusd_prim_append_child_move(CLightUSDPrim *prim, CLightUSDPrim *child_prim) {
  if (!prim) {
    return 0;
  }

  if (!child_prim) {
    return 0;
  }

  lightusd::Prim *pprim = reinterpret_cast<lightusd::Prim *>(prim);
  lightusd::Prim *pchild = reinterpret_cast<lightusd::Prim *>(child_prim);

  pprim->children().emplace_back(std::move(*pchild));

  return 1;
}

uint64_t c_lightusd_prim_num_children(const CLightUSDPrim *prim) {
  if (!prim) {
    return 0;
  }

  const lightusd::Prim *pprim = reinterpret_cast<const lightusd::Prim *>(prim);
  return pprim->children().size();
}

const char *c_lightusd_prim_type(const CLightUSDPrim *prim) {
  if (!prim) {
    return nullptr;
  }

  const lightusd::Prim *pprim = reinterpret_cast<const lightusd::Prim *>(prim);

  // prim_type_name() is the user/parser-set string; for prims constructed
  // programmatically through c_lightusd_prim_new() it is empty. Fall back
  // to the typed schema type-name from the underlying value::Value.
  const std::string &n = pprim->prim_type_name();
  if (!n.empty()) return n.c_str();
  // type_name() returns by value; cache via a thread-local to keep the
  // returned char* stable until the next call on this thread.
  thread_local std::string tls_type;
  tls_type = pprim->type_name();
  return tls_type.c_str();
}


int c_lightusd_prim_get_child(const CLightUSDPrim *prim,
                                              uint64_t child_index,
                                              const CLightUSDPrim ** child_prim) {
  if (!prim) {
    return 0;
  }

  const lightusd::Prim *pprim = reinterpret_cast<const lightusd::Prim *>(prim);
  if (child_index >= pprim->children().size()) {
    return 0;
  }

  const lightusd::Prim *pchild = &pprim->children()[size_t(child_index)];

  (*child_prim) = reinterpret_cast<const CLightUSDPrim *>(pchild);

  return 1;
}

int c_lightusd_prim_del_child(CLightUSDPrim *prim, uint64_t child_idx) {
  if (!prim) {
    return 0;
  }

  lightusd::Prim *pprim = reinterpret_cast<lightusd::Prim *>(prim);
  if (child_idx >= pprim->children().size()) {
    return 0;
  }

  pprim->children().erase(pprim->children().begin() + ptrdiff_t(child_idx));

  return 1;
}

c_lightusd_token_t *c_lightusd_token_new(const char *str) {
  if (!str) {
    return nullptr;
  }

  auto *value = new lightusd::value::token(str);

  return reinterpret_cast<c_lightusd_token_t *>(value);
}

c_lightusd_token_t *c_lightusd_token_dup(const c_lightusd_token_t *_tok) {

  if (!_tok) {
    return nullptr;
  }

  auto *tok = reinterpret_cast<const lightusd::value::token *>(_tok);

  auto *value = new lightusd::value::token(tok->str());

  return reinterpret_cast<c_lightusd_token_t *>(value);
}

int c_lightusd_token_free(c_lightusd_token_t *tok) {
  if (!tok) {
    return 0;
  }

  auto *p = reinterpret_cast<lightusd::value::token *>(tok);
  delete p;

  return 1;  // ok
}

const char *c_lightusd_token_str(const c_lightusd_token_t *tok) {
  if (!tok) {
    return nullptr;
  }

  auto *p = reinterpret_cast<const lightusd::value::token *>(tok);
  return p->c_str();
}

size_t c_lightusd_token_size(const c_lightusd_token_t *tok) {
  if (!tok) {
    return 0;
  }

  auto *p = reinterpret_cast<const lightusd::value::token *>(tok);

  return p->str().size();
}

c_lightusd_token_vector_t *c_lightusd_token_vector_new_empty() {

  auto *value = new std::vector<lightusd::value::token>();
  return reinterpret_cast<c_lightusd_token_vector_t *>(value);
}

c_lightusd_token_vector_t *c_lightusd_token_vector_new(const size_t n, const char **strs) {

  if (strs) {
    for (size_t i = 0; i < n; i++) {
      if (!strs[i]) {
        return nullptr;
      }
    }

    auto *value = new std::vector<lightusd::value::token>(n);
    for (size_t i = 0; i < n; i++) {
      value->at(i) = lightusd::value::token(strs[i]);
    }
    return reinterpret_cast<c_lightusd_token_vector_t *>(value);
  } else {
    auto *value = new std::vector<lightusd::value::token>(n);
    return reinterpret_cast<c_lightusd_token_vector_t *>(value);
  }
}

size_t c_lightusd_token_vector_size(const c_lightusd_token_vector_t *sv) {
  if (!sv) {
    return 0;
  }

  auto *p = reinterpret_cast<const std::vector<lightusd::value::token> *>(sv);

  return p->size();
}

int c_lightusd_token_vector_clear(c_lightusd_token_vector_t *sv) {
  if (!sv) {
    return 0;
  }

  auto *p = reinterpret_cast<std::vector<lightusd::value::token> *>(sv);
  p->clear();

  return 1;
}

int c_lightusd_token_vector_resize(c_lightusd_token_vector_t *sv, const size_t n) {
  if (!sv) {
    return 0;
  }

  auto *p = reinterpret_cast<std::vector<lightusd::value::token> *>(sv);

  p->resize(n);

  return 1;
}

int c_lightusd_token_vector_replace(c_lightusd_token_vector_t *sv, const size_t idx, const char *str) {
  if (!sv) {
    return 0;
  }

  if (!str) {
    return 0;
  }

  std::vector<lightusd::value::token> *pv = reinterpret_cast<std::vector<lightusd::value::token> *>(sv);
  if (idx >= pv->size()) {
    return 0;
  }

  pv->at(idx) = lightusd::value::token(str);

  return 1;  // ok
}

int c_lightusd_token_vector_free(c_lightusd_token_vector_t *sv) {
  if (!sv) {
    return 0;
  }

  auto *p = reinterpret_cast<std::vector<lightusd::value::token> *>(sv);
  delete p;

  return 1;  // ok
}

const char *c_lightusd_token_vector_str(const c_lightusd_token_vector_t *sv, const size_t idx) {
  if (!sv) {
    return nullptr;
  }

  auto *p = reinterpret_cast<const std::vector<lightusd::value::token> *>(sv);
  if (idx >= p->size()) {
    return nullptr;
  }

  return p->at(idx).c_str();
}

c_lightusd_string_t *c_lightusd_string_new_empty() {

  auto *value = new std::string();

  return reinterpret_cast<c_lightusd_string_t *>(value);
}

c_lightusd_string_t *c_lightusd_string_new(const char *str) {

  if (str) {
    auto *value = new std::string(str);
    return reinterpret_cast<c_lightusd_string_t *>(value);
  } else {
    auto *value = new std::string();
    return reinterpret_cast<c_lightusd_string_t *>(value);
  }
}

size_t c_lightusd_string_size(const c_lightusd_string_t *s) {
  if (!s) {
    return 0;
  }

  auto *p = reinterpret_cast<const std::string *>(s);

  return p->size();
}

int c_lightusd_string_replace(c_lightusd_string_t *s, const char *str) {
  if (!s) {
    return 0;
  }

  if (!str) {
    return 0;
  }

  std::string *p = reinterpret_cast<std::string *>(s);
  (*p) = std::string(str);

  return 1;  // ok
}

int c_lightusd_string_free(c_lightusd_string_t *s) {
  if (!s) {
    return 0;
  }

  auto *p = reinterpret_cast<std::string *>(s);
  delete p;

  return 1;  // ok
}

const char *c_lightusd_string_str(const c_lightusd_string_t *s) {
  if (!s) {
    return nullptr;
  }

  auto *p = reinterpret_cast<const std::string *>(s);
  return p->c_str();
}

int c_lightusd_string_vector_new_empty(c_lightusd_string_vector *sv, const size_t n) {
  if (!sv) {
    return 0;
  }

  auto *value = new std::vector<std::string>(n);
  sv->data = reinterpret_cast<void *>(value);

  return 1;  // ok
}

int c_lightusd_string_vector_new(c_lightusd_string_vector *sv, const size_t n, const char **strs) {
  if (!sv) {
    return 0;
  }

  if (strs) {
    auto *value = new std::vector<std::string>(n);
    for (size_t i = 0; i < n; i++) {
      value->at(i) = std::string(strs[i]);
    }
    sv->data = reinterpret_cast<void *>(value);
  } else {
    auto *value = new std::vector<std::string>(n);
    sv->data = reinterpret_cast<void *>(value);
  }

  return 1;  // ok
}

size_t c_lightusd_string_vector_size(const c_lightusd_string_vector *sv) {
  if (!sv) {
    return 0;
  }

  if (!sv->data) {
    return 0;
  }

  auto *p = reinterpret_cast<const std::vector<std::string> *>(sv->data);

  return p->size();
}

int c_lightusd_string_vector_clear(c_lightusd_string_vector *sv) {
  if (!sv) {
    return 0;
  }

  if (!sv->data) {
    return 0;
  }

  auto *p = reinterpret_cast<std::vector<std::string> *>(sv->data);
  p->clear();

  return 1;
}

int c_lightusd_string_vector_resize(c_lightusd_string_vector *sv, const size_t n) {
  if (!sv) {
    return 0;
  }

  if (!sv->data) {
    return 0;
  }

  auto *p = reinterpret_cast<std::vector<std::string> *>(sv->data);

  p->resize(n);

  return 1;
}

int c_lightusd_string_vector_replace(c_lightusd_string_vector *sv, const size_t idx, const char *str) {
  if (!sv) {
    return 0;
  }

  if (!sv->data) {
    return 0;
  }

  if (!str) {
    return 0;
  }

  std::vector<std::string> *pv = reinterpret_cast<std::vector<std::string> *>(sv->data);
  if (idx >= pv->size()) {
    return 0;
  }

  pv->at(idx) = std::string(str);

  return 1;  // ok
}

int c_lightusd_string_vector_free(c_lightusd_string_vector *sv) {
  if (!sv) {
    return 0;
  }

  if (sv->data) {
    auto *p = reinterpret_cast<std::vector<std::string> *>(sv->data);
    delete p;
    sv->data = nullptr;
  }

  return 1;  // ok
}

const char *c_lightusd_string_vector_str(const c_lightusd_string_vector *sv, const size_t idx) {
  if (!sv) {
    return nullptr;
  }

  if (sv->data) {
    auto *p = reinterpret_cast<const std::vector<std::string> *>(sv->data);
    if (idx >= p->size()) {
      return nullptr;
    }

    return p->at(idx).c_str();
  }

  return nullptr;
}



int c_lightusd_is_usda_file(const char *filename) {
  if (lightusd::IsUSDA(filename)) {
    return 1;
  }
  return 0;
}

int c_lightusd_is_usdc_file(const char *filename) {
  if (lightusd::IsUSDC(filename)) {
    return 1;
  }
  return 0;
}

int c_lightusd_is_usdz_file(const char *filename) {
  if (lightusd::IsUSDZ(filename)) {
    return 1;
  }
  return 0;
}

int c_lightusd_is_usd_file(const char *filename) {
  if (lightusd::IsUSD(filename)) {
    return 1;
  }
  return 0;
}

int c_lightusd_load_usd_from_file(const char *filename, CLightUSDStage *stage,
                                 c_lightusd_string_t *warn,
                                 c_lightusd_string_t *err) {
  // lightusd::Stage *p = new lightusd::Stage();

  if (!stage) {
    if (err) {
      c_lightusd_string_replace(err, "`stage` argument is null.\n");
    }
    return 0;
  }

  if (!filename) {
    if (err) {
      c_lightusd_string_replace(err, "`filename` argument is null.\n");
    }
    return 0;
  }

  std::string _warn;
  std::string _err;

  bool ret = lightusd::LoadUSDFromFile(
      filename, reinterpret_cast<lightusd::Stage *>(stage), &_warn,
      &_err);

  if (_warn.size() && warn) {
    c_lightusd_string_replace(warn, _warn.c_str());
  }

  if (!ret) {
    if (err) {
      c_lightusd_string_replace(err, _err.c_str());
    }

    return 0;
  }

  return 1;
}

int c_lightusd_load_usda_from_file(const char *filename, CLightUSDStage *stage,
                                 c_lightusd_string_t *warn,
                                 c_lightusd_string_t *err) {
  // lightusd::Stage *p = new lightusd::Stage();

  if (!stage) {
    if (err) {
      c_lightusd_string_replace(err, "`stage` argument is null.\n");
    }
    return 0;
  }

  if (!filename) {
    if (err) {
      c_lightusd_string_replace(err, "`filename` argument is null.\n");
    }
    return 0;
  }

  std::string _warn;
  std::string _err;

  bool ret = lightusd::LoadUSDAFromFile(
      filename, reinterpret_cast<lightusd::Stage *>(stage), &_warn,
      &_err);

  if (_warn.size() && warn) {
    c_lightusd_string_replace(warn, _warn.c_str());
  }

  if (!ret) {
    if (err) {
      c_lightusd_string_replace(err, _err.c_str());
    }

    return 0;
  }

  return 1;
}

int c_lightusd_load_usdc_from_file(const char *filename, CLightUSDStage *stage,
                                 c_lightusd_string_t *warn,
                                 c_lightusd_string_t *err) {
  // lightusd::Stage *p = new lightusd::Stage();

  if (!stage) {
    if (err) {
      c_lightusd_string_replace(err, "`stage` argument is null.\n");
    }
    return 0;
  }

  std::string _warn;
  std::string _err;

  bool ret = lightusd::LoadUSDCFromFile(
      filename, reinterpret_cast<lightusd::Stage *>(stage), &_warn,
      &_err);

  if (_warn.size() && warn) {
    c_lightusd_string_replace(warn, _warn.c_str());
  }

  if (!ret) {
    if (err) {
      c_lightusd_string_replace(err, _err.c_str());
    }

    return 0;
  }

  return 1;
}

int c_lightusd_load_usdz_from_file(const char *filename, CLightUSDStage *stage,
                                 c_lightusd_string_t *warn,
                                 c_lightusd_string_t *err) {
  // lightusd::Stage *p = new lightusd::Stage();

  if (!stage) {
    if (err) {
      c_lightusd_string_replace(err, "`stage` argument is null.\n");
    }
    return 0;
  }

  if (!filename) {
    if (err) {
      c_lightusd_string_replace(err, "`filename` argument is null.\n");
    }
    return 0;
  }

  std::string _warn;
  std::string _err;

  bool ret = lightusd::LoadUSDZFromFile(
      filename, reinterpret_cast<lightusd::Stage *>(stage), &_warn,
      &_err);

  if (_warn.size() && warn) {
    c_lightusd_string_replace(warn, _warn.c_str());
  }

  if (!ret) {
    if (err) {
      c_lightusd_string_replace(err, _err.c_str());
    }

    return 0;
  }

  return 1;
}

namespace {

using namespace lightusd;

bool CVisitPrimFunction(const Path &abs_path, const Prim &prim,
                        const int32_t tree_depth, void *userdata,
                        std::string *err) {
  (void)tree_depth;

  if (!userdata) {
    if (err) {
      (*err) += "`userdata` is nullptr.\n";
    }
    return false;
  }

  CLightUSDPrim *pprim = reinterpret_cast<CLightUSDPrim *>(const_cast<Prim *>(&prim));

  CLightUSDPath *ppath = reinterpret_cast<CLightUSDPath *>(const_cast<Path *>(&abs_path));

  CLightUSDTraversalFunction callback_fun =
      reinterpret_cast<CLightUSDTraversalFunction>(userdata);

  int ret = callback_fun(pprim, ppath);

  if (ret) {
    return true;
  }

  return false;
}

}  // namespace

CLightUSDPrim *c_lightusd_prim_new(const char *_prim_type, c_lightusd_string_t *err) {

  if (!_prim_type) {
    if (err) {
      c_lightusd_string_replace(err, "prim_type is nullptr.");
    }
    return nullptr;
  }

  std::string prim_type_name = std::string(_prim_type);
  if (!lightusd::isValidIdentifier(prim_type_name)) {
    if (err) {
      c_lightusd_string_replace(err, "prim_type contains invalid character.");
    }
    return nullptr;
  }

  bool non_builtin_prim_type{false};

  CLightUSDPrimType prim_type = c_lightusd_prim_type_from_string(_prim_type);
  if (prim_type == C_LIGHTUSD_PRIM_UNKNOWN) {
    // Use `Model`
    prim_type = C_LIGHTUSD_PRIM_MODEL;
    non_builtin_prim_type = true;
  }

  Prim *p{nullptr};

  // Handle additional geom types by string before the enum check, since
  // CLightUSDPrimType doesn't enumerate Sphere/Cube/Cylinder/Cone/Capsule.
  if (prim_type_name == lightusd::kGeomSphere) {
    p = new Prim(GeomSphere{});
  } else if (prim_type_name == lightusd::kGeomCube) {
    p = new Prim(GeomCube{});
  } else if (prim_type_name == lightusd::kGeomCylinder) {
    p = new Prim(GeomCylinder{});
  } else if (prim_type_name == lightusd::kGeomCone) {
    p = new Prim(GeomCone{});
  } else if (prim_type_name == lightusd::kGeomCapsule) {
    p = new Prim(GeomCapsule{});
  } else if (prim_type_name == lightusd::kGeomPoints) {
    p = new Prim(GeomPoints{});
  } else if (prim_type_name == lightusd::kGeomCamera) {
    p = new Prim(GeomCamera{});
  } else if (prim_type_name == lightusd::kGeomSubset) {
    p = new Prim(GeomSubset{});
  } else if (prim_type_name == lightusd::kGeomBasisCurves) {
    p = new Prim(GeomBasisCurves{});
  } else if (prim_type_name == lightusd::kGeomNurbsCurves) {
    p = new Prim(GeomNurbsCurves{});
  } else if (prim_type_name == lightusd::kGeomHermiteCurves) {
    p = new Prim(GeomHermiteCurves{});
  } else if (prim_type_name == lightusd::kGeomPlane) {
    p = new Prim(GeomPlane{});
  } else if (prim_type_name == lightusd::kGeomCylinder_1) {
    p = new Prim(GeomCylinder_1{});
  } else if (prim_type_name == lightusd::kGeomCapsule_1) {
    p = new Prim(GeomCapsule_1{});
  } else if (prim_type_name == lightusd::kGeomTetMesh) {
    p = new Prim(GeomTetMesh{});
  } else if (prim_type_name == lightusd::kGeomNurbsPatch) {
    p = new Prim(GeomNurbsPatch{});
  } else if (prim_type_name == lightusd::kPointInstancer) {
    p = new Prim(GeomPointInstancer{});
  } else if (prim_type_name == lightusd::kSphereLight) {
    p = new Prim(SphereLight{});
  } else if (prim_type_name == lightusd::kRectLight) {
    p = new Prim(RectLight{});
  } else if (prim_type_name == lightusd::kDiskLight) {
    p = new Prim(DiskLight{});
  } else if (prim_type_name == lightusd::kDistantLight) {
    p = new Prim(DistantLight{});
  } else if (prim_type_name == lightusd::kCylinderLight) {
    p = new Prim(CylinderLight{});
  } else if (prim_type_name == lightusd::kDomeLight) {
    p = new Prim(DomeLight{});
  } else if (prim_type_name == lightusd::kDomeLight_1) {
    p = new Prim(DomeLight_1{});
  } else if (prim_type_name == lightusd::kGeometryLight) {
    p = new Prim(GeometryLight{});
  } else if (prim_type_name == lightusd::kPortalLight) {
    p = new Prim(PortalLight{});
  } else if (prim_type_name == lightusd::kPhysicsScene) {
    p = new Prim(PhysicsScene{});
  } else if (prim_type_name == lightusd::kPhysicsJoint) {
    p = new Prim(PhysicsJoint{});
  } else if (prim_type_name == lightusd::kPhysicsRevoluteJoint) {
    p = new Prim(PhysicsRevoluteJoint{});
  } else if (prim_type_name == lightusd::kPhysicsPrismaticJoint) {
    p = new Prim(PhysicsPrismaticJoint{});
  } else if (prim_type_name == lightusd::kPhysicsSphericalJoint) {
    p = new Prim(PhysicsSphericalJoint{});
  } else if (prim_type_name == lightusd::kPhysicsFixedJoint) {
    p = new Prim(PhysicsFixedJoint{});
  } else if (prim_type_name == lightusd::kPhysicsDistanceJoint) {
    p = new Prim(PhysicsDistanceJoint{});
  } else if (prim_type_name == lightusd::kPhysicsCollisionGroup) {
    p = new Prim(PhysicsCollisionGroup{});
  } else if (prim_type_name == lightusd::kNodeGraph) {
    p = new Prim(NodeGraph{});
  } else if (prim_type_name == lightusd::kSkelRoot) {
    p = new Prim(SkelRoot{});
  } else if (prim_type_name == lightusd::kSkeleton) {
    p = new Prim(Skeleton{});
  } else if (prim_type_name == lightusd::kSkelAnimation) {
    p = new Prim(SkelAnimation{});
  } else if (prim_type_name == lightusd::kBlendShape) {
    p = new Prim(BlendShape{});
  } else if (non_builtin_prim_type) {
    Model model;
    model.prim_type_name = std::string(_prim_type);
    p = new Prim(model);
  } else {

#define NEW_PRIM(__cty, __ty) \
    if (prim_type == __cty) {   \
      __ty content;             \
      p = new Prim(content);    \
    } else

    NEW_PRIM(C_LIGHTUSD_PRIM_XFORM, Xform)
    NEW_PRIM(C_LIGHTUSD_PRIM_SCOPE, Scope)
    NEW_PRIM(C_LIGHTUSD_PRIM_MESH, GeomMesh)
    NEW_PRIM(C_LIGHTUSD_PRIM_GEOMSUBSET, GeomSubset)
    NEW_PRIM(C_LIGHTUSD_PRIM_MATERIAL, Material)
    NEW_PRIM(C_LIGHTUSD_PRIM_SHADER, Shader)
    NEW_PRIM(C_LIGHTUSD_PRIM_CAMERA, GeomCamera)
    // TODO: More types.
    {
      if (err) {
        std::string msg = "Unknown or unsupported type: " + std::string(_prim_type) + "\n";
        c_lightusd_string_replace(err, msg.c_str());
      }

      // Unknown or unsupported type.
      DCOUT("Unknown or unsupported type: " << _prim_type);
      return nullptr;
    }
  }

#undef NEW_PRIM

  return reinterpret_cast<CLightUSDPrim*>(p);
}

CLightUSDPrim *c_lightusd_prim_new_builtin(CLightUSDPrimType prim_type) {

  const char *prim_type_name = c_lightusd_prim_type_name(prim_type);
  if (!prim_type_name) {
    return nullptr;
  }

  return c_lightusd_prim_new(prim_type_name, nullptr);
}

int c_lightusd_prim_free(CLightUSDPrim *prim) {
  if (!prim) {
    return 0;
  }

  Prim *p = reinterpret_cast<Prim *>(prim);
  delete p;

  return 1;
}

int c_lightusd_prim_to_string(const CLightUSDPrim *prim, c_lightusd_string_t *str) {
  if (!prim) {
    return 0;
  }

  if (!str) {
    return 0;
  }

  const Prim *p = reinterpret_cast<const Prim *>(prim);

  std::string s = lightusd::to_string(*p);

  if (!c_lightusd_string_replace(str, s.c_str())) {
    return 0;
  }

  return 1;
}

CLightUSDStage *c_lightusd_stage_new() {

  auto *buf = new lightusd::Stage();
  return reinterpret_cast<CLightUSDStage*>(buf);
}

int c_lightusd_stage_to_string(const CLightUSDStage *stage,
                              c_lightusd_string_t *str) {
  if (!stage) {
    return 0;
  }

  if (!str) {
    return 0;
  }

  const auto *p = reinterpret_cast<const lightusd::Stage *>(stage);
  std::string s = p->ExportToString();

  return c_lightusd_string_replace(str, s.c_str());
}

int c_lightusd_stage_free(CLightUSDStage *stage) {
  if (!stage) {
    return 0;
  }

  lightusd::Stage *ptr = reinterpret_cast<lightusd::Stage *>(stage);
  delete ptr;

  return 1;
}

int c_lightusd_stage_traverse(const CLightUSDStage *_stage,
                             CLightUSDTraversalFunction callback_fun,
                             c_lightusd_string_t *_err) {
  if (!_stage) {
    if (_err) {
      c_lightusd_string_replace(_err, "`stage` argument is null.\n");
    }
    return 0;
  }

  const lightusd::Stage *pstage =
      reinterpret_cast<const lightusd::Stage *>(_stage);

  DCOUT("visit prims\n");

  std::string err;
  if (!lightusd::tydra::VisitPrims(*pstage, CVisitPrimFunction,
                                   reinterpret_cast<void *>(callback_fun),
                                   &err)) {
    if (_err) {
      c_lightusd_string_replace(_err, err.c_str());
    }
    return 0;
  }

  return 1;
}

CLightUSDValue *c_lightusd_value_new_null() {

  auto *pv = new lightusd::value::Value(nullptr);

  return reinterpret_cast<CLightUSDValue *>(pv);
}

int c_lightusd_value_is_type(const CLightUSDValue *_value, CLightUSDValueType value_type) {

  if (!_value) {
    return 0;
  }

  const lightusd::value::Value *value = reinterpret_cast<const lightusd::value::Value *>(_value);
  const char *ctyname = c_lightusd_value_type_name(value_type);

  if (value->type_name() == std::string(ctyname)) {
    return 1;
  }

  return 0;
}

int c_lightusd_value_free(CLightUSDValue *aval) {
  if (!aval) {
    return 0;
  }

  lightusd::value::Value *vp = reinterpret_cast<lightusd::value::Value *>(aval);
  delete vp;

  return 1;
}

CLightUSDValue *c_lightusd_value_new_token(const c_lightusd_token_t *tok) {
  if (!tok) {
    return nullptr;
  }

  auto *pv = reinterpret_cast<const lightusd::value::token *>(tok);

  // copies.
  lightusd::value::Value *vp = new lightusd::value::Value(*pv);

  return reinterpret_cast<CLightUSDValue *>(vp);
}

CLightUSDValue *c_lightusd_value_new_string(const c_lightusd_string_t *str) {
  if (!str) {
    return nullptr;
  }

  auto *pv = reinterpret_cast<const std::string *>(str);

  // copies.
  lightusd::value::Value *vp = new lightusd::value::Value(*pv);

  return reinterpret_cast<CLightUSDValue *>(vp);
}

#define ATTRIB_VALUE_NEW_IMPL(__tyname, __cppty, __cty, __tyenum) \
CLightUSDValue *c_lightusd_value_new_##__tyname(__cty val) { \
  (void)__tyenum; \
  /* ensure C++ and C types has same size. */ \
  static_assert(sizeof(__cppty) == sizeof(__cty), ""); \
  __cppty cppval; \
  memcpy(&cppval, &val, sizeof(__cppty)); \
  lightusd::value::Value *vp = new lightusd::value::Value(cppval); \
  return reinterpret_cast<CLightUSDValue *>(vp); \
}

ATTRIB_VALUE_NEW_IMPL(int, int, int, C_LIGHTUSD_VALUE_INT)
ATTRIB_VALUE_NEW_IMPL(int2, value::int2, c_lightusd_int2_t, C_LIGHTUSD_VALUE_INT2)
ATTRIB_VALUE_NEW_IMPL(int3, value::int3, c_lightusd_int3_t, C_LIGHTUSD_VALUE_INT3)
ATTRIB_VALUE_NEW_IMPL(int4, value::int4, c_lightusd_int4_t, C_LIGHTUSD_VALUE_INT4)

ATTRIB_VALUE_NEW_IMPL(float, float, float, C_LIGHTUSD_VALUE_FLOAT)
ATTRIB_VALUE_NEW_IMPL(float2, value::float2, c_lightusd_float2_t, C_LIGHTUSD_VALUE_FLOAT2)
ATTRIB_VALUE_NEW_IMPL(float3, value::float3, c_lightusd_float3_t, C_LIGHTUSD_VALUE_FLOAT3)
ATTRIB_VALUE_NEW_IMPL(float4, value::float4, c_lightusd_float4_t, C_LIGHTUSD_VALUE_FLOAT4)

#undef ATTRIB_VALUE_NEW_IMPL

#define ATTRIB_VALUE_NEW_ARRAY_IMPL(__tyname, __cppty, __cty, __tyenum) \
CLightUSDValue *c_lightusd_value_new_array_##__tyname(uint64_t n, const __cty *vals) { \
  (void)__tyenum; \
  /* ensure C++ and C types has same size. */ \
  static_assert(sizeof(__cppty) == sizeof(__cty), ""); \
  std::vector<__cppty> cppvalarray; \
  cppvalarray.resize(size_t(n)); \
  if (n > 0 && vals) memcpy(cppvalarray.data(), vals, sizeof(__cppty) * size_t(n)); \
  lightusd::value::Value *vp = new lightusd::value::Value(std::move(cppvalarray)); \
  return reinterpret_cast<CLightUSDValue *>(vp); \
}

ATTRIB_VALUE_NEW_ARRAY_IMPL(int, int, int, C_LIGHTUSD_VALUE_INT)
ATTRIB_VALUE_NEW_ARRAY_IMPL(int2, value::int2, c_lightusd_int2_t, C_LIGHTUSD_VALUE_INT2)
ATTRIB_VALUE_NEW_ARRAY_IMPL(int3, value::int3, c_lightusd_int3_t, C_LIGHTUSD_VALUE_INT3)
ATTRIB_VALUE_NEW_ARRAY_IMPL(int4, value::int4, c_lightusd_int4_t, C_LIGHTUSD_VALUE_INT4)

ATTRIB_VALUE_NEW_ARRAY_IMPL(float, float, float, C_LIGHTUSD_VALUE_FLOAT)
ATTRIB_VALUE_NEW_ARRAY_IMPL(float2, value::float2, c_lightusd_float2_t, C_LIGHTUSD_VALUE_FLOAT2)
ATTRIB_VALUE_NEW_ARRAY_IMPL(float3, value::float3, c_lightusd_float3_t, C_LIGHTUSD_VALUE_FLOAT3)
ATTRIB_VALUE_NEW_ARRAY_IMPL(float4, value::float4, c_lightusd_float4_t, C_LIGHTUSD_VALUE_FLOAT4)

#undef ATTRIB_VALUE_NEW_ARRAY_IMPL

#define ATTRIB_VALUE_AS_IMPL(__tyname, __cppty, __cty) \
int c_lightusd_value_as_##__tyname(const CLightUSDValue *_value, __cty *val) { \
  /* ensure C++ and C types has same size. */ \
  static_assert(sizeof(__cppty) == sizeof(__cty), ""); \
  if (!_value) { return 0; } \
  const lightusd::value::Value *vp = reinterpret_cast<const lightusd::value::Value *>(_value); \
  if (auto pv = vp->as<__cppty>()) { \
    memcpy(val, pv, sizeof(__cppty)); \
    return 1; \
  } \
  return 0; \
}

ATTRIB_VALUE_AS_IMPL(int, int, int);
ATTRIB_VALUE_AS_IMPL(int2, value::int2, c_lightusd_int2_t);
ATTRIB_VALUE_AS_IMPL(int3, value::int3, c_lightusd_int3_t);
ATTRIB_VALUE_AS_IMPL(int4, value::int4, c_lightusd_int4_t);

ATTRIB_VALUE_AS_IMPL(float, float, float);
ATTRIB_VALUE_AS_IMPL(float2, value::float2, c_lightusd_float2_t);
ATTRIB_VALUE_AS_IMPL(float3, value::float3, c_lightusd_float3_t);
ATTRIB_VALUE_AS_IMPL(float4, value::float4, c_lightusd_float4_t);


int c_lightusd_value_to_string(const CLightUSDValue *aval, c_lightusd_string_t *str) {
  if (!aval) {
    return 0;
  }

  if (!str) {
    return 0;
  }

  const lightusd::value::Value *cp = reinterpret_cast<const lightusd::value::Value *>(aval);

  std::string s = lightusd::value::pprint_value(*cp, /* indent */0, /* closing_brace */false);

  if (!c_lightusd_string_replace(str, s.c_str())) {
    return 0;
  }

  return 1;
}

int c_lightusd_prim_get_property_names(const CLightUSDPrim *prim, c_lightusd_token_vector_t *prop_names_out) {
  if (!prim) {
    return 0;
  }

  if (!prop_names_out) {
    return 0;
  }

  const Prim *p = reinterpret_cast<const Prim *>(prim);
  std::vector<std::string> ps;
  std::string err;
  if (!tydra::GetPropertyNames(*p, &ps, &err)) {
    // err is suppressed.
    return 0;
  }

  if (!c_lightusd_token_vector_resize(prop_names_out, ps.size())) {
    return 0;
  }

  for (size_t i = 0; i < ps.size(); i++) {
    const std::string &s = ps[i];

    if (!c_lightusd_token_vector_replace(prop_names_out, i, s.c_str())) {
      return 0;
    }
  }

  return 1;
}

static_assert(sizeof(c_lightusd_int2_t) == sizeof(float) * 2, "");
static_assert(sizeof(c_lightusd_int3_t) == sizeof(float) * 3, "");
static_assert(sizeof(c_lightusd_int4_t) == sizeof(float) * 4, "");
static_assert(sizeof(c_lightusd_half2_t) == sizeof(uint16_t) * 2, "");
static_assert(sizeof(c_lightusd_half3_t) == sizeof(uint16_t) * 3, "");
static_assert(sizeof(c_lightusd_half4_t) == sizeof(uint16_t) * 4, "");
static_assert(sizeof(c_lightusd_quath_t) == sizeof(uint16_t) * 4, "");
static_assert(sizeof(c_lightusd_quatf_t) == sizeof(float) * 4, "");
static_assert(sizeof(c_lightusd_quatd_t) == sizeof(double) * 4, "");
