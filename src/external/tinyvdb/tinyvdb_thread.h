#pragma once

/* Backend-independent synchronous task ranges. C11 is the default where its
 * runtime is available; CMake selects GCD on Apple platforms. Public headers
 * do not expose threads.h, pthreads, or Dispatch types. */
#include "tinyvdb_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tvdb_thread_pool tvdb_thread_pool_t;
typedef void (*tvdb_range_fn)(size_t begin, size_t end, void *user);

/* Zero selects the CPU count, capped at 32; explicit counts must be 1..256.
 * The caller participates in C11/pthread work. NONE always uses one worker.
 * Destroy only after all calls finish. */
tvdb_status_t tvdb_thread_pool_create(size_t threads, tvdb_thread_pool_t **out, tvdb_error_t *err);
void tvdb_thread_pool_destroy(tvdb_thread_pool_t *pool);
size_t tvdb_thread_pool_size(const tvdb_thread_pool_t *pool);
size_t tvdb_thread_default_count(void);
const char *tvdb_thread_backend_name(void);

/* Each index in [begin,end) is covered exactly once by disjoint ranges of at
 * most grain indices. Returns after all callbacks complete, establishing the
 * boundary between dependent processing passes. Callbacks must be thread-safe.
 * Reentrant calls on the same pool run serially. C11/pthread parallel submissions from
 * different callers are serialized. GCD submissions use independent jobs. */
tvdb_status_t tvdb_thread_pool_for(tvdb_thread_pool_t *pool, size_t begin, size_t end, size_t grain,
                                   tvdb_range_fn fn, void *user, tvdb_error_t *err);

/* Compatibility with the former header-only helper. Prefer a reusable pool
 * for repeated work. Falls back to serial execution if pool creation fails. */
void tvdb_parallel_for(int start, int end, void (*fn)(int, void *), void *user);

#ifdef __cplusplus
}
#endif
