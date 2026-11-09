/* SPDX-License-Identifier: Apache-2.0 */
#include "descriptor.h"
#include "chunks.h"
#include <math.h>
#include <string.h>

static const lu_component_info components[] = {
  {"void", 0, 0, 1}, {"uint8", 'B', 1, 1},
  {"int32", 'i', 4, _Alignof(int32_t)}, {"uint32", 'I', 4, _Alignof(uint32_t)},
  {"int64", 'q', 8, _Alignof(int64_t)}, {"uint64", 'Q', 8, _Alignof(uint64_t)},
  {"float16", 'e', 2, _Alignof(uint16_t)}, {"float32", 'f', 4, _Alignof(float)},
  {"float64", 'd', 8, _Alignof(double)}, {"uint16", 'H', 2, _Alignof(uint16_t)},
  {"int16", 'h', 2, _Alignof(int16_t)}
};
const lu_component_info* lu_component_get(uint8_t component) {
  return component < sizeof(components) / sizeof(components[0]) ? &components[component] : NULL;
}
int lu_type_describe(uint16_t type, int array, lu_type_descriptor* out) {
  uint16_t component;
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  if (!lu_type_info_at(type) || type == LU_TYPE_INVALID) return 0;
  out->type = type;
  out->is_array = array != 0;
  out->is_string = lu_value_uses_string_storage(type) != 0;
  out->components = type == LU_TYPE_EXTENT ? 6 : lu_type_component_count(type);
  component = array ? lu_value_array_component(type) : lu_scalar_component(type);
  if (type == LU_TYPE_EXTENT) component = LU_TYPE_FLOAT;
  switch (component) {
    case LU_TYPE_BOOL: case LU_TYPE_UCHAR: out->storage = LU_COMP_UINT8; break;
    case LU_TYPE_INT: out->storage = LU_COMP_INT32; break;
    case LU_TYPE_UINT: out->storage = LU_COMP_UINT32; break;
    case LU_TYPE_INT64: out->storage = LU_COMP_INT64; break;
    case LU_TYPE_UINT64: out->storage = LU_COMP_UINT64; break;
    case LU_TYPE_HALF: out->storage = LU_COMP_FLOAT16; break;
    case LU_TYPE_FLOAT: out->storage = LU_COMP_FLOAT32; break;
    case LU_TYPE_DOUBLE: case LU_TYPE_TIMECODE: out->storage = LU_COMP_FLOAT64; break;
    default: break;
  }
  out->stride = out->components * components[out->storage].size;
  out->alignment = components[out->storage].alignment;
  return 1;
}
lu_abi_info lu_abi_get(void) {
  const lu_abi_info info = {LU_CORE_C_VERSION_MAJOR, sizeof(void*) * 8,
    LU_CORE_C_LAYOUT_REVISION,
    sizeof(lu_value), _Alignof(lu_value), offsetof(lu_value, store),
    sizeof(lu_type_descriptor), sizeof(lu_chunks)};
  return info;
}

static int write_number(void* dest, uint8_t storage, const lu_number* n) {
  double f = n->kind == LU_NUMBER_FLOAT ? n->value.float_value :
    n->kind == LU_NUMBER_SIGNED ? (double)n->value.signed_value : (double)n->value.unsigned_value;
  uint64_t u = 0;
  int64_t s = 0;
  if (storage == LU_COMP_FLOAT64) { memcpy(dest, &f, sizeof(f)); return 1; }
  if (storage == LU_COMP_FLOAT32) { float v = (float)f; memcpy(dest, &v, sizeof(v)); return 1; }
  if (storage == LU_COMP_FLOAT16) { uint16_t v = lu_float_to_half((float)f); memcpy(dest, &v, sizeof(v)); return 1; }
  if (storage == LU_COMP_INT32 || storage == LU_COMP_INT64 || storage == LU_COMP_INT16) {
    if (n->kind == LU_NUMBER_UNSIGNED) {
      if (n->value.unsigned_value > INT64_MAX) return 0;
      s = (int64_t)n->value.unsigned_value;
    } else if (n->kind == LU_NUMBER_SIGNED) s = n->value.signed_value;
    else { if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0)) return 0; s = (int64_t)f; }
    if (storage == LU_COMP_INT64) { memcpy(dest, &s, 8); return 1; }
    if (storage == LU_COMP_INT32) { int32_t v; if (s < INT32_MIN || s > INT32_MAX) return 0; v = (int32_t)s; memcpy(dest, &v, 4); return 1; }
    { int16_t v; if (s < INT16_MIN || s > INT16_MAX) return 0; v = (int16_t)s; memcpy(dest, &v, 2); return 1; }
  }
  if (n->kind == LU_NUMBER_SIGNED) { if (n->value.signed_value < 0) return 0; u = (uint64_t)n->value.signed_value; }
  else if (n->kind == LU_NUMBER_UNSIGNED) u = n->value.unsigned_value;
  else { if (!(f >= 0 && f < 18446744073709551616.0)) return 0; u = (uint64_t)f; }
  if (storage == LU_COMP_UINT64) { memcpy(dest, &u, 8); return 1; }
  if (storage == LU_COMP_UINT32) { uint32_t v; if (u > UINT32_MAX) return 0; v = (uint32_t)u; memcpy(dest, &v, 4); return 1; }
  if (storage == LU_COMP_UINT16) { uint16_t v; if (u > UINT16_MAX) return 0; v = (uint16_t)u; memcpy(dest, &v, 2); return 1; }
  if (storage == LU_COMP_UINT8) { uint8_t v; if (u > UINT8_MAX) return 0; v = (uint8_t)u; memcpy(dest, &v, 1); return 1; }
  return 0;
}
int lu_value_set_numbers(lu_value* dst, uint16_t type, int array, size_t count,
                          lu_number_at read, void* context, lu_budget* budget) {
  lu_type_descriptor desc;
  lu_value v = {0};
  unsigned char zero[128] = {0};
  unsigned char* data;
  size_t i, lanes;
  if (!dst || !read || !lu_type_describe(type, array, &desc) || !desc.stride ||
      (!array && count != 1) || (array && count > UINT32_MAX) ||
      count > SIZE_MAX / desc.stride) return 0;
  if (array) { if (!lu_value_array_create(&v, type, count, budget)) return 0; }
  else if (!lu_value_set_pod(&v, type, zero, desc.stride, budget)) return 0;
  data = (unsigned char*)lu_value_mutable_data(&v);
  lanes = count * desc.components;
  for (i = 0; i < lanes; ++i) {
    lu_number n;
    if (!read(context, i, &n) || n.kind < LU_NUMBER_SIGNED || n.kind > LU_NUMBER_FLOAT) goto failed;
    if (type == LU_TYPE_UCHAR &&
        ((n.kind == LU_NUMBER_UNSIGNED && n.value.unsigned_value > UINT8_MAX) ||
         (n.kind == LU_NUMBER_SIGNED && (n.value.signed_value < 0 || n.value.signed_value > UINT8_MAX)) ||
         (n.kind == LU_NUMBER_FLOAT && !(n.value.float_value >= 0 && n.value.float_value <= UINT8_MAX)))) goto failed;
    if (type == LU_TYPE_BOOL) {
      data[i] = n.kind == LU_NUMBER_FLOAT ? n.value.float_value != 0 :
        n.kind == LU_NUMBER_SIGNED ? n.value.signed_value != 0 : n.value.unsigned_value != 0;
    } else if (!write_number(data + i * components[desc.storage].size, desc.storage, &n)) goto failed;
    if (array && lu_scalar_component(type) == LU_TYPE_HALF) {
      float f; memcpy(&f, data + i * sizeof(f), sizeof(f));
      f = lu_half_to_float(lu_float_to_half(f)); memcpy(data + i * sizeof(f), &f, sizeof(f));
    }
  }
  v.flags &= (uint8_t)~LU_VALUE_MODIFIED;
  lu_value_move(dst, &v); return 1;
failed:
  lu_value_destroy(&v); return 0;
}
