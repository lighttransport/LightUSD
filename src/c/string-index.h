/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LU_STRING_INDEX_H
#define LU_STRING_INDEX_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef const char* (*lu_string_index_key)(const void*, size_t, size_t*);
typedef struct lu_string_index { size_t* slots; size_t capacity, count; } lu_string_index;
/* Zero-initialize before use. Keys remain caller-owned. Failed mutations leave
 * the index unchanged. Duplicate keys retain their first position. */
void lu_string_index_destroy(lu_string_index* index);
size_t lu_string_index_find(const lu_string_index*, const char*, size_t, lu_string_index_key, const void*);
int lu_string_index_insert(lu_string_index*, size_t, lu_string_index_key, const void*);
int lu_string_index_rebuild(lu_string_index*, size_t, lu_string_index_key, const void*);
#ifdef __cplusplus
}
#endif
#endif
