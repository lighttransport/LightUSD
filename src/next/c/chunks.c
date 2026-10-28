/* SPDX-License-Identifier: Apache-2.0 */
#include "chunks.h"
#include "storage-internal.h"
#include <string.h>

typedef struct control {
  lu_chunk_table table;
  lu_atomic_size references;
  lu_budget* budget;
} control;
static control* owner(const lu_chunks* s) { return (control*)s->table; }
static int fail(lu_chunks* s) { s->failed = 1; return 0; }
int lu_chunks_init(lu_chunks* s, size_t element, size_t alignment, size_t chunk) {
  if (!s || !element || element > UINT32_MAX || !alignment || alignment > UINT32_MAX ||
      (alignment & (alignment - 1)) || element % alignment || !chunk ||
      chunk > (size_t)PTRDIFF_MAX / element) return 0;
  memset(s, 0, sizeof(*s));
  s->element_size = (uint32_t)element; s->alignment = (uint32_t)alignment;
  s->chunk_elements = chunk; return 1;
}
/* Budget semantics: retained payload bytes. Table/control overhead is bounded
 * by the chunk count and is not charged twice while copies share the table. */
static void* allocate_chunk(const lu_chunks* s, size_t count) {
  size_t bytes = count * s->element_size;
  void* raw;
  void* data;
  uintptr_t address;
  if (!lu_budget_add(owner(s)->budget, bytes)) return NULL;
  if (s->alignment <= _Alignof(lu_max_align_t)) data = malloc(bytes);
  else {
    if (bytes > SIZE_MAX - sizeof(void*) - (s->alignment - 1)) {
      lu_budget_sub(owner(s)->budget, bytes); return NULL;
    }
    raw = malloc(bytes + sizeof(void*) + s->alignment - 1);
    address = ((uintptr_t)raw + sizeof(void*) + s->alignment - 1) & ~(uintptr_t)(s->alignment - 1);
    data = raw ? (void*)address : NULL;
    if (data) memcpy((unsigned char*)data - sizeof(raw), &raw, sizeof(raw));
  }
  if (!data) lu_budget_sub(owner(s)->budget, bytes);
  return data;
}
static void free_chunk(const lu_chunks* s, void* data, size_t count) {
  void* raw = data;
  if (s->alignment > _Alignof(lu_max_align_t)) memcpy(&raw, (unsigned char*)data - sizeof(raw), sizeof(raw));
  free(raw);
  lu_budget_sub(owner(s)->budget, count * s->element_size);
}
static void release(lu_chunks* s) {
  control* c = owner(s);
  size_t i;
  if (!c || !lu_ref_release(&c->references)) return;
  for (i = 0; i < c->table.count; ++i)
    free_chunk(s, c->table.chunks[i], i + 1 == c->table.count ? c->table.tail : s->chunk_elements);
  free(c->table.chunks); lu_budget_release(c->budget); free(c);
}
void lu_chunks_destroy(lu_chunks* s) {
  if (!s) return;
  release(s); s->table = NULL; s->size = 0; s->failed = s->maybe_shared = 0;
}
void lu_chunks_move(lu_chunks* dst, lu_chunks* src) {
  if (!dst || !src || dst == src) return;
  lu_chunks_destroy(dst); *dst = *src;
  src->table = NULL; src->size = 0; src->failed = src->maybe_shared = 0;
}
void lu_chunks_share(lu_chunks* dst, lu_chunks* src) {
  if (dst == src) return;
  if (!dst || !src || dst->element_size != src->element_size ||
      dst->alignment != src->alignment || dst->chunk_elements != src->chunk_elements) abort();
  if (src->table) lu_ref_retain(&owner(src)->references);
  release(dst); dst->table = src->table; dst->size = src->size; dst->failed = src->failed;
  dst->maybe_shared = src->maybe_shared = src->table != NULL;
}
int lu_chunks_shared(const lu_chunks* s) {
  return s && s->maybe_shared && s->table && lu_atomic_load(&owner(s)->references) > 1;
}
static int create_control(lu_chunks* s, lu_budget* budget) {
  control* c = (control*)calloc(1, sizeof(*c));
  if (!c) return fail(s);
  lu_atomic_init(&c->references, 1); c->budget = budget; lu_budget_retain(budget);
  s->table = &c->table; return 1;
}
int lu_chunks_unique(lu_chunks* s) {
  lu_chunks copy;
  size_t count, i;
  if (!lu_chunks_shared(s)) { s->maybe_shared = 0; return 1; }
  lu_chunks_init(&copy, s->element_size, s->alignment, s->chunk_elements);
  if (!create_control(&copy, owner(s)->budget)) return fail(s);
  count = s->table->count;
  if (count) {
    copy.table->chunks = (void**)malloc(count * sizeof(void*));
    if (!copy.table->chunks) { lu_chunks_destroy(&copy); return fail(s); }
  }
  copy.table->slots = count;
  for (i = 0; i < count; ++i) {
    size_t n = i + 1 == count ? s->table->tail : s->chunk_elements;
    void* p = allocate_chunk(&copy, n);
    if (!p) { lu_chunks_destroy(&copy); return fail(s); }
    memcpy(p, s->table->chunks[i], n * s->element_size);
    copy.table->chunks[copy.table->count++] = p; copy.table->tail = n;
  }
  copy.size = s->size; copy.failed = s->failed;
  lu_chunks_move(s, &copy); return 1;
}
size_t lu_chunks_capacity(const lu_chunks* s) {
  return s && s->table && s->table->count ?
    (s->table->count - 1) * s->chunk_elements + s->table->tail : 0;
}
int lu_chunks_reserve(lu_chunks* s, size_t count, int exact, lu_budget* budget) {
  size_t max_elements, needed;
  lu_chunk_table* t;
  if (!s || !s->element_size) return 0;
  if (!lu_chunks_unique(s)) return 0;
  if (count <= lu_chunks_capacity(s)) return 1;
  max_elements = (size_t)PTRDIFF_MAX / s->element_size;
  if (count >= max_elements) return fail(s);
  needed = count / s->chunk_elements + (count % s->chunk_elements != 0);
  if (needed > max_elements / s->chunk_elements || needed > (size_t)PTRDIFF_MAX / sizeof(void*)) return fail(s);
  if (!s->table && !create_control(s, budget)) return 0;
  t = s->table;
  if (needed > t->slots) {
    void* table = realloc(t->chunks, needed * sizeof(void*));
    if (!table) return fail(s);
    t->chunks = (void**)table; t->slots = needed;
  }
  if (t->count && t->tail < s->chunk_elements) {
    void* p = allocate_chunk(s, s->chunk_elements);
    size_t last = t->count - 1;
    if (!p) return fail(s);
    memcpy(p, t->chunks[last], t->tail * s->element_size);
    free_chunk(s, t->chunks[last], t->tail);
    t->chunks[last] = p; t->tail = s->chunk_elements;
  }
  while (t->count < needed) {
    int partial = exact && t->count + 1 == needed && count % s->chunk_elements != 0;
    size_t n = partial ? count % s->chunk_elements : s->chunk_elements;
    void* p = allocate_chunk(s, n);
    if (!p) return fail(s);
    t->chunks[t->count++] = p; t->tail = n;
  }
  return 1;
}
size_t lu_chunks_chunk_size(const lu_chunks* s, size_t index) {
  size_t start, remaining;
  if (!s || !s->table || index >= s->table->count) return 0;
  start = index * s->chunk_elements;
  if (start >= s->size) return 0;
  remaining = s->size - start;
  return remaining < s->chunk_elements ? remaining : s->chunk_elements;
}
int lu_chunks_append(lu_chunks* s, const void* data, size_t count, lu_budget* budget) {
  void* saved = NULL;
  size_t first, chunks, i;
  uintptr_t address;
  const unsigned char* src;
  if (!count) return 1;
  if (!s || !data) return 0;
  if (count > (size_t)PTRDIFF_MAX / s->element_size || count > SIZE_MAX - s->size) return fail(s);
  address = (uintptr_t)data;
  chunks = s->table ? s->table->count : 0; first = chunks;
  if (lu_chunks_shared(s)) first = 0;
  else if (chunks && s->table->tail < s->chunk_elements && s->size + count > lu_chunks_capacity(s)) first = chunks - 1;
  for (i = first; i < chunks; ++i) {
    uintptr_t start = (uintptr_t)s->table->chunks[i];
    size_t bytes = lu_chunks_chunk_size(s, i) * s->element_size;
    if (address >= start && address - start < bytes) {
      if (count * s->element_size > bytes - (address - start)) return fail(s);
      saved = malloc(count * s->element_size);
      if (!saved) return fail(s);
      memcpy(saved, data, count * s->element_size); data = saved; break;
    }
  }
  if (!lu_chunks_reserve(s, s->size + count, 0, budget)) { free(saved); return 0; }
  src = (const unsigned char*)data;
  while (count) {
    size_t offset = s->size % s->chunk_elements;
    size_t space = s->chunk_elements - offset;
    size_t n = count < space ? count : space;
    unsigned char* dest = (unsigned char*)s->table->chunks[s->size / s->chunk_elements];
    memmove(dest + offset * s->element_size, src, n * s->element_size);
    s->size += n; src += n * s->element_size; count -= n;
  }
  free(saved); return 1;
}
int lu_chunks_copy(const lu_chunks* s, void* dest, size_t count) {
  size_t i;
  unsigned char* out = (unsigned char*)dest;
  if (!s || count < s->size || (s->size && !dest)) return 0;
  for (i = 0; s->table && i < s->table->count; ++i) {
    size_t bytes = lu_chunks_chunk_size(s, i) * s->element_size;
    if (!bytes) break;
    memcpy(out, s->table->chunks[i], bytes); out += bytes;
  }
  return 1;
}
void lu_chunks_compact(lu_chunks* s) {
  size_t needed, used;
  void* p;
  lu_chunk_table* t;
  if (!s || !s->table || !lu_chunks_unique(s)) return;
  t = s->table;
  needed = s->size / s->chunk_elements + (s->size % s->chunk_elements != 0);
  while (t->count > needed) {
    free_chunk(s, t->chunks[t->count - 1], t->tail);
    --t->count; t->tail = t->count ? s->chunk_elements : 0;
  }
  if (!needed) return;
  used = s->size - (needed - 1) * s->chunk_elements;
  if (used >= t->tail) return;
  p = allocate_chunk(s, used);
  if (!p) return;
  memcpy(p, t->chunks[needed - 1], used * s->element_size);
  free_chunk(s, t->chunks[needed - 1], t->tail);
  t->chunks[needed - 1] = p; t->tail = used;
}
