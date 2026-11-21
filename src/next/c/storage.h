/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LU_STORAGE_H
#define LU_STORAGE_H

#include "lightusd-core-c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Process-wide generation of completed read-side storage materializations
 * (lazy values and deferred indexes). Zero disables reuse after saturation.
 * Not a general mutation version: explicit edits need separate invalidation.
 * A read-side cache that changes an owner's footprint notifies after publication. */
LU_CORE_API size_t lu_storage_materialization_epoch(void);
LU_CORE_API void lu_storage_note_materialization(void);

typedef struct lu_budget lu_budget;
typedef struct lu_buffer lu_buffer;

/* A retained budget follows its allocations, including across worker threads.
 * Zero is a real zero-byte limit; SIZE_MAX is unlimited. */
LU_CORE_API lu_budget* lu_budget_create(size_t limit);
LU_CORE_API void lu_budget_retain(lu_budget* budget);
LU_CORE_API void lu_budget_release(lu_budget* budget);
LU_CORE_API int lu_budget_add(lu_budget* budget, size_t bytes);
LU_CORE_API void lu_budget_sub(lu_budget* budget, size_t bytes);
LU_CORE_API size_t lu_budget_limit(const lu_budget* budget);
LU_CORE_API size_t lu_budget_current(const lu_budget* budget);
LU_CORE_API size_t lu_budget_peak(const lu_budget* budget);

/* Buffers are aligned, zero-initialized, and copy-on-write. All fallible
 * operations return zero without changing the original allocation. */
LU_CORE_API lu_buffer* lu_buffer_create(size_t size, size_t alignment, lu_budget* budget);
LU_CORE_API lu_buffer* lu_buffer_copy(const void* data, size_t size, size_t alignment,
                          lu_budget* budget);
LU_CORE_API void lu_buffer_retain(lu_buffer* buffer);
LU_CORE_API void lu_buffer_release(lu_buffer* buffer);
LU_CORE_API const void* lu_buffer_data(const lu_buffer* buffer);
LU_CORE_API size_t lu_buffer_size(const lu_buffer* buffer);
LU_CORE_API size_t lu_buffer_capacity(const lu_buffer* buffer);
LU_CORE_API size_t lu_buffer_alignment(const lu_buffer* buffer);
LU_CORE_API int lu_buffer_shared(const lu_buffer* buffer);
LU_CORE_API int lu_buffer_unique(lu_buffer** buffer);
LU_CORE_API int lu_buffer_resize(lu_buffer** buffer, size_t size);
LU_CORE_API int lu_buffer_reserve(lu_buffer** buffer, size_t capacity);
LU_CORE_API int lu_buffer_append(lu_buffer** buffer, const void* data, size_t size);
LU_CORE_API void* lu_buffer_mutable(lu_buffer** buffer);
/* Parser drain-barrier publication: bind an empty shared placeholder once.
 * All readers must wait for publication before observing its payload. */
LU_CORE_API int lu_buffer_publish_deferred(lu_buffer* placeholder, lu_buffer* source);

#ifdef __cplusplus
}
#endif
#endif
