/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LU_CHUNKS_H
#define LU_CHUNKS_H
#include "storage.h"
#ifdef __cplusplus
extern "C" {
#endif
/* POD chunk table exposed for inlined typed indexing. Ownership is private.
 * Distinct shared instances may be read concurrently; synchronize mutation. */
typedef struct lu_chunk_table { void** chunks; size_t count, slots, tail; } lu_chunk_table;
typedef struct lu_chunks {
  lu_chunk_table* table;
  size_t size, chunk_elements;
  uint32_t element_size, alignment;
  unsigned char failed, maybe_shared;
} lu_chunks;
LU_CORE_API int lu_chunks_init(lu_chunks*, size_t element_size, size_t alignment, size_t chunk_elements);
LU_CORE_API void lu_chunks_destroy(lu_chunks*);
LU_CORE_API void lu_chunks_move(lu_chunks*, lu_chunks*);
LU_CORE_API void lu_chunks_share(lu_chunks*, lu_chunks*);
LU_CORE_API int lu_chunks_shared(const lu_chunks*);
LU_CORE_API int lu_chunks_unique(lu_chunks*);
LU_CORE_API int lu_chunks_reserve(lu_chunks*, size_t count, int exact, lu_budget*);
LU_CORE_API int lu_chunks_append(lu_chunks*, const void*, size_t count, lu_budget*);
LU_CORE_API void lu_chunks_compact(lu_chunks*);
LU_CORE_API int lu_chunks_copy(const lu_chunks*, void*, size_t count);
LU_CORE_API size_t lu_chunks_capacity(const lu_chunks*);
LU_CORE_API size_t lu_chunks_chunk_size(const lu_chunks*, size_t index);
#ifdef __cplusplus
}
#endif
#endif
