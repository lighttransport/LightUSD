/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LU_STORAGE_INTERNAL_H
#define LU_STORAGE_INTERNAL_H
#include "storage.h"
#include <stdlib.h>
#if defined(_MSC_VER) && !defined(__clang__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef volatile LONG64 lu_atomic_size;
static inline size_t lu_atomic_load(const lu_atomic_size* p) {
  return (size_t)InterlockedCompareExchange64((volatile LONG64*)p, 0, 0);
}
static inline int lu_atomic_cas(lu_atomic_size* p, size_t* expected, size_t next) {
  size_t old = (size_t)InterlockedCompareExchange64(p, (LONG64)next,
                                                  (LONG64)*expected);
  if (old == *expected) return 1;
  *expected = old;
  return 0;
}
static inline void lu_atomic_init(lu_atomic_size* p, size_t n) { *p = (LONG64)n; }
#else
#include <stdatomic.h>
typedef _Atomic(size_t) lu_atomic_size;
static inline size_t lu_atomic_load(const lu_atomic_size* p) {
  return atomic_load_explicit(p, memory_order_acquire);
}
static inline int lu_atomic_cas(lu_atomic_size* p, size_t* expected, size_t next) {
  return atomic_compare_exchange_weak_explicit(p, expected, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire);
}
static inline void lu_atomic_init(lu_atomic_size* p, size_t n) { atomic_init(p, n); }
#endif

static inline void lu_ref_retain(lu_atomic_size* p) {
  size_t n = lu_atomic_load(p);
  do { if (!n || n == SIZE_MAX) abort(); } while (!lu_atomic_cas(p, &n, n + 1));
}
static inline int lu_ref_release(lu_atomic_size* p) {
  size_t n = lu_atomic_load(p);
  do { if (!n) abort(); } while (!lu_atomic_cas(p, &n, n - 1));
  return n == 1;
}

struct lu_buffer {
  lu_atomic_size references;
  size_t size, capacity, alignment, charged;
  void* data;
  lu_budget* budget;
  struct lu_buffer* backing;
};
#endif
