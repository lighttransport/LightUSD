/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LU_VALUE_H
#define LU_VALUE_H
#include "storage.h"
#ifdef __cplusplus
extern "C" {
#endif
#define LU_VALUE_MODIFIED 1u
#define LU_VALUE_STRING_INLINE 2u

typedef const char* (*lu_string_at)(const void*, size_t, size_t*);
typedef struct lu_lazy_callbacks {
  int (*decode)(const void*, uint16_t, uint32_t, lu_value*);
  void (*destroy)(void*);
  void* (*clone)(const void*);
} lu_lazy_callbacks;

LU_CORE_API int lu_value_set_pod(lu_value*, uint16_t, const void*, size_t, lu_budget*);
LU_CORE_API int lu_value_set_string(lu_value*, uint16_t, const char*, size_t, lu_budget*);
LU_CORE_API int lu_value_set_array(lu_value*, uint16_t, const void*, size_t, lu_budget*);
LU_CORE_API int lu_value_set_array_buffer(lu_value*, uint16_t, lu_buffer*, size_t);
LU_CORE_API int lu_value_string_array_set(lu_value*, size_t, const char*, size_t);
LU_CORE_API int lu_value_array_create(lu_value*, uint16_t, size_t, lu_budget*);
LU_CORE_API int lu_value_array_resize(lu_value*, size_t);
LU_CORE_API const void* lu_value_data(const lu_value*);
LU_CORE_API void* lu_value_mutable_data(lu_value*);
LU_CORE_API size_t lu_value_bytes(const lu_value*);
LU_CORE_API size_t lu_value_array_scalar_count(const lu_value*);
LU_CORE_API uint16_t lu_value_array_component(uint16_t);
LU_CORE_API lu_buffer* lu_value_buffer(const lu_value*);
LU_CORE_API int lu_value_set_strings(lu_value*, uint16_t, size_t, lu_string_at, const void*, lu_budget*);
LU_CORE_API lu_view lu_value_string(const lu_value*);
LU_CORE_API lu_view lu_value_string_at(const lu_value*, size_t);
LU_CORE_API int lu_value_make_dict(lu_value*, lu_budget*);
LU_CORE_API size_t lu_value_dict_size(const lu_value*);
LU_CORE_API lu_view lu_value_dict_key(const lu_value*, size_t);
LU_CORE_API const lu_value* lu_value_dict_value(const lu_value*, size_t);
LU_CORE_API const lu_value* lu_value_dict_find(const lu_value*, const char*, size_t);
/* Detaches the parent. The borrowed child expires on structural mutation;
 * never assign its containing dictionary into it (which would form a cycle). */
LU_CORE_API lu_value* lu_value_dict_mutable_value(lu_value*, const char*, size_t);
LU_CORE_API int lu_value_dict_set(lu_value*, const char*, size_t, const lu_value*);
LU_CORE_API int lu_value_dict_typed(const lu_value*);
LU_CORE_API int lu_value_dict_set_typed(lu_value*, int);
LU_CORE_API int lu_value_set_lazy(lu_value*, uint16_t, uint32_t, void*, const lu_lazy_callbacks*, lu_budget*);
LU_CORE_API const void* lu_value_lazy_context(const lu_value*);
LU_CORE_API void* lu_value_mutable_lazy_context(lu_value*);
LU_CORE_API int lu_value_materialize(lu_value*);
LU_CORE_API int lu_value_equal(const lu_value*, const lu_value*);
LU_CORE_API uint64_t lu_value_hash(const lu_value*);
LU_CORE_API size_t lu_value_heap_bytes(const lu_value*);
#ifdef __cplusplus
}
#endif
#endif
