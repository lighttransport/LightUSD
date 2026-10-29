#pragma once
/* Operation-local allocation helpers. Custom callbacks must track their own
 * block sizes: free/realloc receive zero for an unknown previous size. The
 * allocator and its context must outlive all resulting owning objects. */
#include "tinyvdb_io.h"
#include <stdlib.h>
#include <string.h>
static inline void *tvdb_work_malloc(tvdb_allocator_t *a, size_t n) {
  return a ? a->malloc_fn(n, a->user_ctx) : malloc(n);
}
static inline void tvdb_work_free(tvdb_allocator_t *a, void *p) {
  if (p) { if (a) a->free_fn(p, 0, a->user_ctx); else free(p); }
}
static inline void *tvdb_work_calloc(tvdb_allocator_t *a, size_t n, size_t w) {
  if (w && n > SIZE_MAX / w) return NULL;
  size_t bytes = n * w;
  void *p = tvdb_work_malloc(a, bytes);
  if (p) memset(p, 0, bytes);
  return p;
}
static inline void *tvdb_work_realloc(tvdb_allocator_t *a, void *p, size_t n) {
  return a ? a->realloc_fn(p, 0, n, a->user_ctx) : realloc(p, n);
}
/* In-place heapsort: unlike libc qsort, it has no hidden heap workspace.
 * Callers provide complete tie-breaks, preserving the same sorted order. */
static inline void tvdb_work_swap(unsigned char *a, unsigned char *b, size_t w) {
  for (size_t k = 0; k < w; ++k) { unsigned char t = a[k]; a[k] = b[k]; b[k] = t; }
}
static inline void tvdb_work_sift(unsigned char *p, size_t root, size_t n,
                                 size_t w, int (*cmp)(const void *, const void *)) {
  while (root < n / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < n && cmp(p + child*w, p + (child+1)*w) < 0) ++child;
    if (cmp(p + root*w, p + child*w) >= 0) break;
    tvdb_work_swap(p + root*w, p + child*w, w); root = child;
  }
}
static inline void tvdb_work_sort(tvdb_allocator_t *a, void *data, size_t n,
                                 size_t w, int (*cmp)(const void *, const void *)) {
  if (!a) { qsort(data, n, w, cmp); return; }
  unsigned char *p = (unsigned char *)data;
  for (size_t k = n/2; k; --k) tvdb_work_sift(p, k-1, n, w, cmp);
  for (size_t k = n; k > 1; --k) {
    tvdb_work_swap(p, p + (k-1)*w, w); tvdb_work_sift(p, 0, k-1, w, cmp);
  }
}
