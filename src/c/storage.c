/* SPDX-License-Identifier: Apache-2.0 */
#include "storage-internal.h"
#include <string.h>

static lu_atomic_size materialization_epoch = 1;
size_t lu_storage_materialization_epoch(void) {
  const size_t epoch = lu_atomic_load(&materialization_epoch);
  return epoch == SIZE_MAX ? 0 : epoch;
}
void lu_storage_note_materialization(void) {
  size_t epoch = lu_atomic_load(&materialization_epoch);
  while (epoch != SIZE_MAX &&
         !lu_atomic_cas(&materialization_epoch, &epoch, epoch + 1)) {}
}

struct lu_budget {
  lu_atomic_size references, current, peak;
  size_t limit;
};

lu_budget* lu_budget_create(size_t limit) {
  lu_budget* b = (lu_budget*)malloc(sizeof(*b));
  if (!b) return NULL;
  lu_atomic_init(&b->references, 1);
  lu_atomic_init(&b->current, 0);
  lu_atomic_init(&b->peak, 0);
  b->limit = limit;
  return b;
}
void lu_budget_retain(lu_budget* b) { if (b) lu_ref_retain(&b->references); }
void lu_budget_release(lu_budget* b) {
  if (b && lu_ref_release(&b->references)) {
    if (lu_atomic_load(&b->current)) abort();
    free(b);
  }
}
int lu_budget_add(lu_budget* b, size_t bytes) {
  size_t n, peak;
  if (!b) return 1;
  n = lu_atomic_load(&b->current);
  do {
    if (bytes > b->limit || n > b->limit - bytes) return 0;
  } while (!lu_atomic_cas(&b->current, &n, n + bytes));
  n += bytes;
  peak = lu_atomic_load(&b->peak);
  while (peak < n && !lu_atomic_cas(&b->peak, &peak, n)) {}
  return 1;
}
void lu_budget_sub(lu_budget* b, size_t bytes) {
  size_t n;
  if (!b) return;
  n = lu_atomic_load(&b->current);
  do { if (n < bytes) abort(); } while (!lu_atomic_cas(&b->current, &n, n - bytes));
}
size_t lu_budget_limit(const lu_budget* b) { return b ? b->limit : SIZE_MAX; }
size_t lu_budget_current(const lu_budget* b) { return b ? lu_atomic_load(&b->current) : 0; }
size_t lu_budget_peak(const lu_budget* b) { return b ? lu_atomic_load(&b->peak) : 0; }

static lu_buffer* create_buffer(size_t size, size_t capacity, size_t alignment,
                                lu_budget* budget) {
  size_t bytes;
  uintptr_t address;
  lu_buffer* b;
  if (!alignment || (alignment & (alignment - 1)) || size > capacity ||
      capacity > (size_t)PTRDIFF_MAX || alignment > (size_t)PTRDIFF_MAX ||
      alignment - 1 > SIZE_MAX - sizeof(*b) ||
      capacity > SIZE_MAX - sizeof(*b) - (alignment - 1)) return NULL;
  bytes = sizeof(*b) + alignment - 1 + capacity;
  if (!lu_budget_add(budget, bytes)) return NULL;
  b = (lu_buffer*)malloc(bytes);
  if (!b) { lu_budget_sub(budget, bytes); return NULL; }
  lu_atomic_init(&b->references, 1);
  b->size = size;
  b->capacity = capacity;
  b->alignment = alignment;
  b->charged = bytes;
  b->budget = budget;
  b->backing = NULL;
  lu_budget_retain(budget);
  address = (uintptr_t)(b + 1);
  address = (address + alignment - 1) & ~(uintptr_t)(alignment - 1);
  b->data = (void*)address;
  if (size) memset(b->data, 0, size);
  return b;
}
lu_buffer* lu_buffer_create(size_t size, size_t alignment, lu_budget* budget) {
  return create_buffer(size, size, alignment, budget);
}
lu_buffer* lu_buffer_copy(const void* data, size_t size, size_t alignment,
                          lu_budget* budget) {
  lu_buffer* b;
  if (!data && size) return NULL;
  b = lu_buffer_create(size, alignment, budget);
  if (b && size) memcpy(b->data, data, size);
  return b;
}
void lu_buffer_retain(lu_buffer* b) { if (b) lu_ref_retain(&b->references); }
void lu_buffer_release(lu_buffer* b) {
  if (b && lu_ref_release(&b->references)) {
    lu_budget* budget = b->budget;
    lu_buffer_release(b->backing);
    lu_budget_sub(budget, b->charged);
    free(b);
    lu_budget_release(budget);
  }
}
const void* lu_buffer_data(const lu_buffer* b) { return b ? b->data : NULL; }
size_t lu_buffer_size(const lu_buffer* b) { return b ? b->size : 0; }
size_t lu_buffer_capacity(const lu_buffer* b) { return b ? b->capacity : 0; }
size_t lu_buffer_alignment(const lu_buffer* b) { return b ? b->alignment : 1; }
int lu_buffer_shared(const lu_buffer* b) {
  return b && (b->backing || lu_atomic_load(&b->references) > 1);
}

static int replace_buffer(lu_buffer** slot, size_t capacity, size_t size) {
  lu_buffer* old;
  lu_buffer* b;
  if (!slot || !(old = *slot)) return 0;
  b = create_buffer(size, capacity, old->alignment, old->budget);
  if (!b) return 0;
  if (old->size && size) memcpy(b->data, old->data, old->size < size ? old->size : size);
  *slot = b;
  lu_buffer_release(old);
  return 1;
}
int lu_buffer_unique(lu_buffer** slot) {
  if (!slot || !*slot) return 0;
  return !lu_buffer_shared(*slot) || replace_buffer(slot, (*slot)->capacity, (*slot)->size);
}
int lu_buffer_reserve(lu_buffer** slot, size_t capacity) {
  if (!slot || !*slot) return 0;
  if (capacity < (*slot)->capacity) capacity = (*slot)->capacity;
  if (capacity == (*slot)->capacity && !lu_buffer_shared(*slot)) return 1;
  return replace_buffer(slot, capacity, (*slot)->size);
}
int lu_buffer_resize(lu_buffer** slot, size_t size) {
  size_t old, capacity;
  if (!slot || !*slot) return 0;
  old = (*slot)->size;
  capacity = (*slot)->capacity;
  if (size > capacity) {
    size_t extra = capacity / 2;
    capacity = capacity <= SIZE_MAX - extra ? capacity + extra : size;
    if (capacity < size) capacity = size;
  }
  if ((size > (*slot)->capacity || lu_buffer_shared(*slot)) &&
      !replace_buffer(slot, capacity, size)) return 0;
  if (size > old) memset((unsigned char*)(*slot)->data + old, 0, size - old);
  (*slot)->size = size;
  return 1;
}
void* lu_buffer_mutable(lu_buffer** slot) {
  return lu_buffer_unique(slot) ? (*slot)->data : NULL;
}
int lu_buffer_append(lu_buffer** slot, const void* data, size_t size) {
  size_t old, offset = 0;
  int alias = 0;
  uintptr_t address, base;
  if (!slot || !*slot || (!data && size)) return 0;
  old = (*slot)->size;
  if (size > SIZE_MAX - old) return 0;
  address = (uintptr_t)data;
  base = (uintptr_t)(*slot)->data;
  if (data && address >= base && address - base <= old) {
    offset = (size_t)(address - base);
    if (size > old - offset) return 0;
    alias = 1;
  }
  if (!lu_buffer_resize(slot, old + size)) return 0;
  if (alias) data = (const unsigned char*)(*slot)->data + offset;
  if (size) memmove((unsigned char*)(*slot)->data + old, data, size);
  return 1;
}

int lu_buffer_publish_deferred(lu_buffer* target, lu_buffer* source) {
  if (!target || !source || target == source || target->size || target->backing ||
      source->alignment != target->alignment) return 0;
  target->backing = source->backing ? source->backing : source;
  lu_buffer_retain(target->backing);
  target->data = source->data;
  target->size = source->size;
  target->capacity = source->capacity;
  return 1;
}
