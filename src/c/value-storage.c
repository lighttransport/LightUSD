/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * value-storage.c -- C11 value type-query predicates.
 *
 * The Value-free type predicates of src/next/types/value.cc and
 * src/next/types/value-view.cc: which backing storage an array element type
 * uses, scalar-component/byte-size mapping, and the array-size validity checks.
 * Everything is a pure function of lu_type_id; the Value object model stays C++.
 */

#include "lightusd-core-c.h"

int lu_value_uses_string_storage(uint16_t id) {
  return id == LU_TYPE_STRING || id == LU_TYPE_TOKEN ||
         id == LU_TYPE_ASSETPATH || id == LU_TYPE_PATHEXPRESSION;
}

int lu_value_is_float_backed_array(uint16_t id) {
  switch (id) {
    case LU_TYPE_FLOAT:
    case LU_TYPE_FLOAT2:
    case LU_TYPE_FLOAT3:
    case LU_TYPE_FLOAT4:
    case LU_TYPE_POINT3F:
    case LU_TYPE_VECTOR3F:
    case LU_TYPE_NORMAL3F:
    case LU_TYPE_COLOR3F:
    case LU_TYPE_COLOR4F:
    case LU_TYPE_TEXCOORD2F:
    case LU_TYPE_TEXCOORD3F:
    case LU_TYPE_QUATF:
    case LU_TYPE_MATRIX2F:
    case LU_TYPE_MATRIX3F:
    case LU_TYPE_MATRIX4F:
    /* Half element types materialize into a float buffer (no 16-bit storage). */
    case LU_TYPE_HALF:
    case LU_TYPE_HALF2:
    case LU_TYPE_HALF3:
    case LU_TYPE_HALF4:
    case LU_TYPE_QUATH:
    case LU_TYPE_POINT3H:
    case LU_TYPE_VECTOR3H:
    case LU_TYPE_NORMAL3H:
    case LU_TYPE_COLOR3H:
    case LU_TYPE_COLOR4H:
    case LU_TYPE_TEXCOORD2H:
    case LU_TYPE_TEXCOORD3H:
      return 1;
    default:
      return 0;
  }
}

int lu_value_is_int_backed_array(uint16_t id) {
  switch (id) {
    case LU_TYPE_INT:
    case LU_TYPE_INT2:
    case LU_TYPE_INT3:
    case LU_TYPE_INT4:
      return 1;
    default:
      return 0;
  }
}

int lu_value_is_uint_backed_array(uint16_t id) {
  switch (id) {
    case LU_TYPE_UINT:
    case LU_TYPE_UINT2:
    case LU_TYPE_UINT3:
    case LU_TYPE_UINT4:
    case LU_TYPE_UCHAR:
      return 1;
    default:
      return 0;
  }
}

int lu_value_is_double_backed_array(uint16_t id) {
  switch (id) {
    case LU_TYPE_DOUBLE:
    case LU_TYPE_TIMECODE:
    case LU_TYPE_DOUBLE2:
    case LU_TYPE_DOUBLE3:
    case LU_TYPE_DOUBLE4:
    case LU_TYPE_POINT3D:
    case LU_TYPE_VECTOR3D:
    case LU_TYPE_NORMAL3D:
    case LU_TYPE_COLOR3D:
    case LU_TYPE_COLOR4D:
    case LU_TYPE_TEXCOORD2D:
    case LU_TYPE_TEXCOORD3D:
    case LU_TYPE_FRAME4D:
    case LU_TYPE_QUATD:
    case LU_TYPE_MATRIX2D:
    case LU_TYPE_MATRIX3D:
    case LU_TYPE_MATRIX4D:
      return 1;
    default:
      return 0;
  }
}

int lu_value_fits_array_size(size_t count) {
  return count <= (size_t)0xFFFFFFFFu;
}

int lu_value_is_valid_component_array(size_t scalar_count, uint16_t elem_type,
                                      uint32_t comps_per_elem,
                                      int storage_matches) {
  if (!storage_matches || comps_per_elem == 0 ||
      (uint32_t)lu_type_component_count(elem_type) != comps_per_elem ||
      scalar_count % comps_per_elem != 0) {
    return 0;
  }
  return lu_value_fits_array_size(scalar_count / comps_per_elem);
}

size_t lu_value_component_array_size(size_t scalar_count, size_t components) {
  if (components == 0 || scalar_count % components != 0) return 0;
  return scalar_count / components;
}

int lu_value_is_array_type(uint16_t id) {
  return lu_value_is_float_backed_array(id) ||
         lu_value_is_double_backed_array(id) ||
         lu_value_is_int_backed_array(id) ||
         lu_value_is_uint_backed_array(id) || id == LU_TYPE_INT64 ||
         id == LU_TYPE_UINT64 || id == LU_TYPE_BOOL ||
         lu_value_uses_string_storage(id);
}

uint16_t lu_scalar_component(uint16_t id) {
  const uint16_t c = lu_type_component_type(id);
  return c == LU_TYPE_INVALID ? id : c;
}

int lu_requires_lazy_component_reorder(uint16_t id) {
  return id == LU_TYPE_QUATF || id == LU_TYPE_QUATD || id == LU_TYPE_QUATH;
}

size_t lu_scalar_byte_size(uint16_t comp) {
  switch (comp) {
    case LU_TYPE_FLOAT:
    case LU_TYPE_INT:
    case LU_TYPE_UINT:
      return 4;
    case LU_TYPE_DOUBLE:
    case LU_TYPE_INT64:
    case LU_TYPE_UINT64:
      return 8;
    default:
      return 0;
  }
}
