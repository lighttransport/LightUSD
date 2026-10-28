/* SPDX-License-Identifier: Apache-2.0 */
#include "value.h"
#include "storage-internal.h"
#include <string.h>

typedef struct strings {
  lu_atomic_size references;
  lu_buffer* allocation;
  size_t count;
  lu_view* values;
} strings;
typedef struct dict_entry { lu_value key, value; } dict_entry;
typedef struct dict {
  lu_atomic_size references;
  lu_buffer* allocation;
  lu_buffer* entries;
  lu_buffer* index;
  size_t count;
  int typed;
  struct dict* pending_next;
} dict;
typedef struct lazy {
  lu_atomic_size references;
  lu_buffer* allocation;
  void* context;
  lu_lazy_callbacks callbacks;
} lazy;

static void* payload(const lu_value* v) {
  void* p = NULL;
  if (v) memcpy(&p, v->store.bytes, sizeof(p));
  return p;
}
static void put_payload(lu_value* v, void* p) { memcpy(v->store.bytes, &p, sizeof(p)); }
static dict_entry* entries(dict* d) { return (dict_entry*)lu_buffer_data(d->entries); }
static const dict_entry* centries(const dict* d) { return (const dict_entry*)lu_buffer_data(d->entries); }
static lu_budget* budget_of(const lu_value* v) {
  if (!v) return NULL;
  if (v->kind == LU_VALUE_DICT) return ((dict*)payload(v))->allocation->budget;
  if (v->kind == LU_VALUE_LAZY_ARRAY) return ((lazy*)payload(v))->allocation->budget;
  if (v->kind == LU_VALUE_ARRAY && lu_value_uses_string_storage(v->type))
    return ((strings*)payload(v))->allocation->budget;
  if (v->kind == LU_VALUE_BOX || v->kind == LU_VALUE_ARRAY ||
      (v->kind == LU_VALUE_STRING && !(v->flags & LU_VALUE_STRING_INLINE)))
    return ((lu_buffer*)payload(v))->budget;
  return NULL;
}

lu_value_kind lu_value_kind_of(uint16_t type) {
  if (type == LU_TYPE_DICTIONARY) return LU_VALUE_DICT;
  if (lu_value_uses_string_storage(type)) return LU_VALUE_STRING;
  if (!lu_type_size(type)) return LU_VALUE_EMPTY;
  return lu_type_size(type) <= LU_VALUE_INLINE_CAP ? LU_VALUE_POD : LU_VALUE_BOX;
}
void lu_value_init_empty(lu_value* v) { if (v) memset(v, 0, sizeof(*v)); }
void lu_value_init(lu_value* v, uint16_t type) {
  lu_value_init_empty(v);
  if (!v) return;
  if (lu_value_kind_of(type) == LU_VALUE_POD) { v->type = type; v->kind = LU_VALUE_POD; }
}
void lu_value_init_block(lu_value* v) {
  lu_value_init_empty(v);
  if (v) v->kind = LU_VALUE_BLOCK;
}

static void release_dict(dict* d) {
  if (!d || !lu_ref_release(&d->references)) return;
  d->pending_next = NULL;
  while (d) {
    size_t i;
    dict* next = d->pending_next;
    dict_entry* e = entries(d);
    for (i = 0; i < d->count; ++i) {
      lu_value_destroy(&e[i].key);
      if (e[i].value.kind == LU_VALUE_DICT) {
        dict* child = (dict*)payload(&e[i].value);
        if (child && lu_ref_release(&child->references)) {
          child->pending_next = next;
          next = child;
        }
      } else lu_value_destroy(&e[i].value);
    }
    lu_buffer_release(d->entries);
    lu_buffer_release(d->index);
    lu_buffer_release(d->allocation);
    d = next;
  }
}
void lu_value_destroy(lu_value* v) {
  void* p;
  if (!v) return;
  p = payload(v);
  switch (v->kind) {
    case LU_VALUE_DICT: release_dict((dict*)p); break;
    case LU_VALUE_LAZY_ARRAY:
      if (p && lu_ref_release(&((lazy*)p)->references)) {
        lazy* l = (lazy*)p;
        lu_buffer* allocation = l->allocation;
        l->callbacks.destroy(l->context);
        lu_buffer_release(allocation);
      }
      break;
    case LU_VALUE_ARRAY:
      if (lu_value_uses_string_storage(v->type)) {
        if (p && lu_ref_release(&((strings*)p)->references))
          lu_buffer_release(((strings*)p)->allocation);
      } else lu_buffer_release((lu_buffer*)p);
      break;
    case LU_VALUE_STRING:
      if (!(v->flags & LU_VALUE_STRING_INLINE)) lu_buffer_release((lu_buffer*)p);
      break;
    case LU_VALUE_BOX: lu_buffer_release((lu_buffer*)p); break;
    default: break;
  }
  lu_value_init_empty(v);
}
void lu_value_copy(lu_value* dst, const lu_value* src) {
  lu_value copy;
  void* p;
  if (!dst || !src || dst == src) return;
  copy = *src;
  p = payload(src);
  if (p) {
    if (src->kind == LU_VALUE_DICT) lu_ref_retain(&((dict*)p)->references);
    else if (src->kind == LU_VALUE_LAZY_ARRAY) lu_ref_retain(&((lazy*)p)->references);
    else if (src->kind == LU_VALUE_ARRAY && lu_value_uses_string_storage(src->type))
      lu_ref_retain(&((strings*)p)->references);
    else if (src->kind == LU_VALUE_ARRAY || src->kind == LU_VALUE_BOX ||
             (src->kind == LU_VALUE_STRING && !(src->flags & LU_VALUE_STRING_INLINE)))
      lu_buffer_retain((lu_buffer*)p);
  }
  lu_value_destroy(dst);
  *dst = copy;
}
void lu_value_move(lu_value* dst, lu_value* src) {
  if (!dst || !src || dst == src) return;
  lu_value_destroy(dst);
  *dst = *src;
  lu_value_init_empty(src);
}
static void publish(lu_value* dst, lu_value* v) { lu_value_move(dst, v); }
int lu_value_set_pod(lu_value* dst, uint16_t type, const void* data, size_t size,
                      lu_budget* budget) {
  lu_value v = {0};
  if (!dst || !data || !size || size != lu_type_size(type) ||
      type == LU_TYPE_DICTIONARY || lu_value_uses_string_storage(type)) return 0;
  v.type = type;
  if (size <= LU_VALUE_INLINE_CAP) { v.kind = LU_VALUE_POD; memcpy(v.store.bytes, data, size); }
  else {
    lu_buffer* b = lu_buffer_copy(data, size, lu_type_alignment(type), budget);
    if (!b) return 0;
    v.kind = LU_VALUE_BOX;
    put_payload(&v, b);
  }
  publish(dst, &v);
  return 1;
}
int lu_value_set_string(lu_value* dst, uint16_t type, const char* data, size_t size,
                         lu_budget* budget) {
  lu_value v = {0};
  if (!dst || (!data && size) || !lu_value_uses_string_storage(type) || size == SIZE_MAX) return 0;
  v.type = type;
  v.kind = LU_VALUE_STRING;
  if (size < LU_VALUE_INLINE_CAP) {
    v.flags = LU_VALUE_STRING_INLINE;
    v.size = (uint32_t)size;
    if (size) memcpy(v.store.bytes, data, size);
  } else {
    lu_buffer* b = lu_buffer_create(size + 1, 1, budget);
    if (!b) return 0;
    if (size) memcpy((void*)lu_buffer_data(b), data, size);
    put_payload(&v, b);
  }
  publish(dst, &v);
  return 1;
}
lu_view lu_value_string(const lu_value* v) {
  lu_view result = {NULL, 0};
  if (!v || v->kind != LU_VALUE_STRING) return result;
  if (v->flags & LU_VALUE_STRING_INLINE) {
    result.data = (const char*)v->store.bytes;
    result.len = v->size;
  } else {
    const lu_buffer* b = (const lu_buffer*)payload(v);
    result.data = (const char*)lu_buffer_data(b);
    result.len = lu_buffer_size(b) ? lu_buffer_size(b) - 1 : 0;
  }
  return result;
}
uint16_t lu_value_array_component(uint16_t type) {
  if (lu_value_is_float_backed_array(type)) return LU_TYPE_FLOAT;
  if (lu_value_is_double_backed_array(type)) return LU_TYPE_DOUBLE;
  if (lu_value_is_int_backed_array(type)) return LU_TYPE_INT;
  if (lu_value_is_uint_backed_array(type)) return LU_TYPE_UINT;
  if (type == LU_TYPE_INT64 || type == LU_TYPE_UINT64 || type == LU_TYPE_BOOL) return type;
  return LU_TYPE_INVALID;
}
static size_t array_bytes(uint16_t type, size_t count) {
  size_t stride = lu_type_size(lu_value_array_component(type));
  size_t components = lu_type_component_count(type);
  if (!stride || !components || count > UINT32_MAX ||
      components > SIZE_MAX / stride || count > SIZE_MAX / (stride * components)) return SIZE_MAX;
  return count * components * stride;
}
/* Retain an already-filled canonical array buffer without copying it. */
int lu_value_set_array_buffer(lu_value* dst, uint16_t type, lu_buffer* buffer, size_t count) {
  lu_value v = {0};
  size_t size = array_bytes(type, count), i;
  if (!dst || !buffer || size == SIZE_MAX || size != lu_buffer_size(buffer) ||
      lu_buffer_alignment(buffer) < lu_type_alignment(lu_value_array_component(type))) return 0;
  if (type == LU_TYPE_BOOL) {
    const unsigned char* p = (const unsigned char*)lu_buffer_data(buffer);
    for (i = 0; i < count; ++i) if (p[i] > 1) return 0;
  }
  lu_buffer_retain(buffer);
  v.type = type;
  v.kind = LU_VALUE_ARRAY;
  v.size = (uint32_t)count;
  put_payload(&v, buffer);
  publish(dst, &v);
  return 1;
}
int lu_value_array_create(lu_value* dst, uint16_t type, size_t count, lu_budget* budget) {
  size_t size = array_bytes(type, count);
  lu_value v = {0};
  lu_buffer* b;
  if (!dst || size == SIZE_MAX) return 0;
  b = lu_buffer_create(size, lu_type_alignment(lu_value_array_component(type)), budget);
  if (!b) return 0;
  v.type = type;
  v.kind = LU_VALUE_ARRAY;
  v.size = (uint32_t)count;
  put_payload(&v, b);
  publish(dst, &v);
  return 1;
}
int lu_value_set_array(lu_value* dst, uint16_t type, const void* data, size_t count,
                        lu_budget* budget) {
  lu_value v = {0};
  size_t size = array_bytes(type, count), i;
  if ((!data && size) || size == SIZE_MAX || !lu_value_array_create(&v, type, count, budget)) return 0;
  if (size) memcpy((void*)lu_buffer_data((lu_buffer*)payload(&v)), data, size);
  if (type == LU_TYPE_BOOL) {
    unsigned char* bytes = (unsigned char*)lu_buffer_data((lu_buffer*)payload(&v));
    for (i = 0; i < count; ++i) bytes[i] = (unsigned char)(bytes[i] != 0);
  }
  publish(dst, &v);
  return 1;
}
lu_buffer* lu_value_buffer(const lu_value* v) {
  if (!v) return NULL;
  if (v->kind == LU_VALUE_BOX ||
      (v->kind == LU_VALUE_ARRAY && !lu_value_uses_string_storage(v->type)) ||
      (v->kind == LU_VALUE_STRING && !(v->flags & LU_VALUE_STRING_INLINE)))
    return (lu_buffer*)payload(v);
  return NULL;
}
const void* lu_value_data(const lu_value* v) {
  if (!v) return NULL;
  if (v->kind == LU_VALUE_POD) return v->store.bytes;
  if (v->kind == LU_VALUE_STRING) return lu_value_string(v).data;
  return lu_buffer_data(lu_value_buffer(v));
}
size_t lu_value_bytes(const lu_value* v) {
  if (!v) return 0;
  if (v->kind == LU_VALUE_POD) return lu_type_size(v->type);
  if (v->kind == LU_VALUE_STRING) return lu_value_string(v).len;
  return lu_buffer_size(lu_value_buffer(v));
}
void* lu_value_mutable_data(lu_value* v) {
  lu_buffer* b;
  if (!v) return NULL;
  if (v->kind == LU_VALUE_POD || (v->kind == LU_VALUE_STRING && (v->flags & LU_VALUE_STRING_INLINE)))
    return v->store.bytes;
  b = lu_value_buffer(v);
  if (!b || !lu_buffer_unique(&b)) return NULL;
  put_payload(v, b);
  v->flags |= LU_VALUE_MODIFIED;
  return (void*)lu_buffer_data(b);
}
size_t lu_value_array_scalar_count(const lu_value* v) {
  return v && v->kind == LU_VALUE_ARRAY ? (size_t)v->size * lu_type_component_count(v->type) : 0;
}
int lu_value_array_resize(lu_value* v, size_t count) {
  size_t size;
  lu_buffer* b;
  if (!v || v->kind != LU_VALUE_ARRAY || !(b = lu_value_buffer(v))) return 0;
  size = array_bytes(v->type, count);
  if (size == SIZE_MAX || !lu_buffer_resize(&b, size)) return 0;
  put_payload(v, b);
  v->size = (uint32_t)count;
  v->flags |= LU_VALUE_MODIFIED;
  return 1;
}
int lu_value_set_strings(lu_value* dst, uint16_t type, size_t count,
                          lu_string_at at, const void* context, lu_budget* budget) {
  size_t i, bytes = 0, header;
  strings* s;
  lu_buffer* allocation;
  char* output;
  lu_value v = {0};
  if (!dst || !at || count > UINT32_MAX || !lu_value_uses_string_storage(type) ||
      count > (SIZE_MAX - sizeof(strings)) / sizeof(lu_view)) return 0;
  header = sizeof(strings) + count * sizeof(lu_view);
  for (i = 0; i < count; ++i) {
    size_t size = 0;
    const char* text = at(context, i, &size);
    if ((!text && size) || size == SIZE_MAX || bytes > SIZE_MAX - size - 1) return 0;
    bytes += size + 1;
  }
  if (bytes > SIZE_MAX - header) return 0;
  allocation = lu_buffer_create(header + bytes, _Alignof(lu_max_align_t), budget);
  if (!allocation) return 0;
  s = (strings*)lu_buffer_data(allocation);
  lu_atomic_init(&s->references, 1);
  s->allocation = allocation;
  s->count = count;
  s->values = (lu_view*)(s + 1);
  output = (char*)s + header;
  for (i = 0; i < count; ++i) {
    size_t size = 0;
    const char* text = at(context, i, &size);
    size_t remaining = header + bytes - (size_t)(output - (char*)s);
    if ((!text && size) || size >= remaining) { lu_buffer_release(allocation); return 0; }
    s->values[i].data = output;
    s->values[i].len = size;
    if (size) memcpy(output, text, size);
    output[size] = '\0';
    output += size + 1;
  }
  v.type = type;
  v.kind = LU_VALUE_ARRAY;
  v.size = (uint32_t)count;
  put_payload(&v, s);
  publish(dst, &v);
  return 1;
}
lu_view lu_value_string_at(const lu_value* v, size_t i) {
  lu_view result = {NULL, 0};
  if (v && v->kind == LU_VALUE_ARRAY && lu_value_uses_string_storage(v->type)) {
    const strings* s = (const strings*)payload(v);
    if (s && i < s->count) return s->values[i];
  }
  return result;
}

typedef struct string_replacement {
  const lu_value* value;
  size_t index;
  lu_view replacement;
} string_replacement;
static const char* replacement_at(const void* context, size_t index, size_t* length) {
  const string_replacement* r = (const string_replacement*)context;
  const lu_view s = index == r->index ? r->replacement : lu_value_string_at(r->value, index);
  *length = s.len;
  return s.data;
}
int lu_value_string_array_set(lu_value* v, size_t index, const char* text, size_t size) {
  string_replacement replacement;
  if (!v || v->kind != LU_VALUE_ARRAY || !lu_value_uses_string_storage(v->type) ||
      index >= v->size || (!text && size)) return 0;
  replacement.value = v;
  replacement.index = index;
  replacement.replacement.data = text;
  replacement.replacement.len = size;
  if (!lu_value_set_strings(v, v->type, v->size, replacement_at, &replacement, budget_of(v))) return 0;
  v->flags |= LU_VALUE_MODIFIED;
  return 1;
}

int lu_value_make_dict(lu_value* dst, lu_budget* budget) {
  lu_value v = {0};
  lu_buffer* allocation;
  dict* d;
  if (!dst) return 0;
  allocation = lu_buffer_create(sizeof(dict), _Alignof(dict), budget);
  if (!allocation) return 0;
  d = (dict*)lu_buffer_data(allocation);
  lu_atomic_init(&d->references, 1);
  d->allocation = allocation;
  v.type = LU_TYPE_DICTIONARY;
  v.kind = LU_VALUE_DICT;
  put_payload(&v, d);
  publish(dst, &v);
  return 1;
}
size_t lu_value_dict_size(const lu_value* v) {
  return v && v->kind == LU_VALUE_DICT ? ((dict*)payload(v))->count : 0;
}
lu_view lu_value_dict_key(const lu_value* v, size_t i) {
  lu_view none = {NULL, 0};
  if (i >= lu_value_dict_size(v)) return none;
  return lu_value_string(&centries((const dict*)payload(v))[i].key);
}
const lu_value* lu_value_dict_value(const lu_value* v, size_t i) {
  if (i >= lu_value_dict_size(v)) return NULL;
  return &centries((const dict*)payload(v))[i].value;
}
static uint64_t bytes_hash(const void* data, size_t size) {
  const unsigned char* p = (const unsigned char*)data;
  uint64_t h = UINT64_C(14695981039346656037);
  size_t i;
  for (i = 0; i < size; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
  return h;
}
static size_t dict_find(const dict* d, const char* key, size_t size) {
  size_t i;
  if (d->index) {
    const size_t* slots = (const size_t*)lu_buffer_data(d->index);
    size_t mask = lu_buffer_size(d->index) / sizeof(size_t) - 1;
    for (i = (size_t)bytes_hash(key, size) & mask; slots[i]; i = (i + 1) & mask) {
      size_t n = slots[i] - 1;
      lu_view text = lu_value_string(&centries(d)[n].key);
      if (text.len == size && (!size || memcmp(text.data, key, size) == 0)) return n;
    }
  } else for (i = 0; i < d->count; ++i) {
    lu_view text = lu_value_string(&centries(d)[i].key);
    if (text.len == size && (!size || memcmp(text.data, key, size) == 0)) return i;
  }
  return SIZE_MAX;
}
/* Equality may compare a dictionary that has no cached index (for example when
 * an index allocation failed under the payload budget). Build a temporary one
 * per comparison instead of degrading to O(n^2) key scans. Returns NULL on
 * allocation failure; callers then retain the linear fallback. */
static size_t* dict_build_slots(const dict* d, size_t* out_capacity) {
  size_t capacity = 16, i;
  size_t* slots;
  if (!d || !out_capacity || d->count == 0) return NULL;
  while (capacity / 2 < d->count) {
    if (capacity > SIZE_MAX / 2) return NULL;
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(size_t)) return NULL;
  slots = (size_t*)calloc(capacity, sizeof(size_t));
  if (!slots) return NULL;
  for (i = 0; i < d->count; ++i) {
    const lu_view key = lu_value_string(&centries(d)[i].key);
    size_t slot = (size_t)bytes_hash(key.data, key.len) & (capacity - 1);
    while (slots[slot]) slot = (slot + 1) & (capacity - 1);
    slots[slot] = i + 1;
  }
  *out_capacity = capacity;
  return slots;
}
static size_t dict_find_slots(const dict* d, const size_t* slots,
                              size_t capacity, const char* key, size_t size) {
  const size_t mask = capacity - 1;
  size_t i;
  for (i = (size_t)bytes_hash(key, size) & mask; slots[i];
       i = (i + 1) & mask) {
    const size_t n = slots[i] - 1;
    const lu_view text = lu_value_string(&centries(d)[n].key);
    if (text.len == size && (!size || memcmp(text.data, key, size) == 0)) {
      return n;
    }
  }
  return SIZE_MAX;
}
const lu_value* lu_value_dict_find(const lu_value* v, const char* key, size_t size) {
  size_t i;
  const dict* d;
  if (!v || v->kind != LU_VALUE_DICT || (!key && size)) return NULL;
  d = (const dict*)payload(v);
  i = dict_find(d, key, size);
  return i == SIZE_MAX ? NULL : &centries(d)[i].value;
}
static void rebuild_index(dict* d) {
  size_t capacity = 16, i;
  lu_buffer* b;
  size_t* slots;
  while (capacity / 2 < d->count) {
    if (capacity > SIZE_MAX / 2) return;
    capacity *= 2;
  }
  lu_buffer_release(d->index);
  d->index = NULL;
  if (capacity > SIZE_MAX / sizeof(size_t)) return;
  b = lu_buffer_create(capacity * sizeof(size_t), _Alignof(size_t), d->allocation->budget);
  if (!b) return; /* Correct linear lookup remains available under a tight budget. */
  slots = (size_t*)lu_buffer_data(b);
  for (i = 0; i < d->count; ++i) {
    lu_view key = lu_value_string(&centries(d)[i].key);
    size_t slot = (size_t)bytes_hash(key.data, key.len) & (capacity - 1);
    while (slots[slot]) slot = (slot + 1) & (capacity - 1);
    slots[slot] = i + 1;
  }
  d->index = b;
}
static int detach_dict(lu_value* v) {
  dict* old = (dict*)payload(v);
  lu_value copy = {0};
  dict* d;
  size_t i;
  if (!old || lu_atomic_load(&old->references) == 1) return old != NULL;
  if (!lu_value_make_dict(&copy, old->allocation->budget)) return 0;
  d = (dict*)payload(&copy);
  if (old->count) {
    d->entries = lu_buffer_create(old->count * sizeof(dict_entry), _Alignof(dict_entry), old->allocation->budget);
    if (!d->entries) { lu_value_destroy(&copy); return 0; }
    for (i = 0; i < old->count; ++i) {
      lu_value_copy(&entries(d)[i].key, &centries(old)[i].key);
      lu_value_copy(&entries(d)[i].value, &centries(old)[i].value);
    }
  }
  d->count = old->count;
  d->typed = old->typed;
  lu_buffer_retain(old->index);
  d->index = old->index;
  publish(v, &copy);
  return 1;
}
lu_value* lu_value_dict_mutable_value(lu_value* v, const char* key, size_t size) {
  size_t i;
  if (!v || v->kind != LU_VALUE_DICT || (!key && size)) return NULL;
  i = dict_find((const dict*)payload(v), key, size);
  if (i == SIZE_MAX || !detach_dict(v)) return NULL;
  return &entries((dict*)payload(v))[i].value;
}
int lu_value_dict_set(lu_value* v, const char* key, size_t size, const lu_value* child) {
  lu_value stored = {0}, owned_key = {0};
  dict* d;
  size_t i, old_count;
  if (!v || v->kind != LU_VALUE_DICT || !child || (!key && size)) return 0;
  if (child == v || lu_atomic_load(&((dict*)payload(v))->references) > 1) {
    lu_value candidate = {0};
    int ok;
    lu_value_copy(&candidate, v);
    if (!detach_dict(&candidate)) { lu_value_destroy(&candidate); return 0; }
    ok = lu_value_dict_set(&candidate, key, size, child);
    if (ok) publish(v, &candidate);
    lu_value_destroy(&candidate);
    return ok;
  }
  /* Retain before detaching: child and key may point into this dictionary. */
  lu_value_copy(&stored, child);
  if (!lu_value_set_string(&owned_key, LU_TYPE_STRING, key, size, budget_of(v))) {
    lu_value_destroy(&stored); return 0;
  }
  if (!detach_dict(v)) { lu_value_destroy(&stored); lu_value_destroy(&owned_key); return 0; }
  d = (dict*)payload(v);
  key = lu_value_string(&owned_key).data;
  i = dict_find(d, key, size);
  if (i != SIZE_MAX) {
    lu_value_move(&entries(d)[i].value, &stored);
    lu_value_destroy(&owned_key);
    return 1;
  }
  old_count = d->count;
  if (old_count >= SIZE_MAX / sizeof(dict_entry)) goto failed;
  if (!d->entries) d->entries = lu_buffer_create(sizeof(dict_entry), _Alignof(dict_entry), d->allocation->budget);
  else if (!lu_buffer_resize(&d->entries, (old_count + 1) * sizeof(dict_entry))) goto failed;
  if (!d->entries) goto failed;
  lu_value_move(&entries(d)[old_count].key, &owned_key);
  lu_value_move(&entries(d)[old_count].value, &stored);
  d->count++;
  if (!d->index || d->count > lu_buffer_size(d->index) / sizeof(size_t) / 2) rebuild_index(d);
  else {
    size_t* slots;
    size_t mask, slot;
    if (!lu_buffer_unique(&d->index)) { lu_buffer_release(d->index); d->index = NULL; return 1; }
    slots = (size_t*)lu_buffer_data(d->index);
    mask = lu_buffer_size(d->index) / sizeof(size_t) - 1;
    key = lu_value_string(&entries(d)[old_count].key).data;
    slot = (size_t)bytes_hash(key, size) & mask;
    while (slots[slot]) slot = (slot + 1) & mask;
    slots[slot] = old_count + 1;
  }
  return 1;
failed:
  lu_value_destroy(&stored);
  lu_value_destroy(&owned_key);
  return 0;
}
int lu_value_dict_typed(const lu_value* v) {
  return v && v->kind == LU_VALUE_DICT && ((dict*)payload(v))->typed;
}
int lu_value_dict_set_typed(lu_value* v, int typed) {
  if (!v || v->kind != LU_VALUE_DICT || !detach_dict(v)) return 0;
  ((dict*)payload(v))->typed = typed != 0;
  return 1;
}
int lu_value_set_lazy(lu_value* dst, uint16_t type, uint32_t count, void* context,
                       const lu_lazy_callbacks* callbacks, lu_budget* budget) {
  lu_buffer* allocation;
  lazy* l;
  lu_value v = {0};
  if (!dst || !context || !callbacks || !callbacks->decode || !callbacks->destroy || !lu_value_is_array_type(type)) return 0;
  allocation = lu_buffer_create(sizeof(lazy), _Alignof(lazy), budget);
  if (!allocation) return 0;
  l = (lazy*)lu_buffer_data(allocation);
  lu_atomic_init(&l->references, 1);
  l->allocation = allocation;
  l->context = context;
  l->callbacks = *callbacks;
  v.type = type;
  v.kind = LU_VALUE_LAZY_ARRAY;
  v.size = count;
  put_payload(&v, l);
  publish(dst, &v);
  return 1;
}
const void* lu_value_lazy_context(const lu_value* v) {
  return v && v->kind == LU_VALUE_LAZY_ARRAY ? ((lazy*)payload(v))->context : NULL;
}
void* lu_value_mutable_lazy_context(lu_value* v) {
  lazy* l;
  lu_value copy = {0};
  void* context;
  if (!v || v->kind != LU_VALUE_LAZY_ARRAY) return NULL;
  l = (lazy*)payload(v);
  if (lu_atomic_load(&l->references) == 1) return l->context;
  if (!l->callbacks.clone || !(context = l->callbacks.clone(l->context))) return NULL;
  if (!lu_value_set_lazy(&copy, v->type, v->size, context, &l->callbacks, l->allocation->budget)) {
    l->callbacks.destroy(context); return NULL;
  }
  publish(v, &copy);
  return ((lazy*)payload(v))->context;
}
int lu_value_materialize(lu_value* v) {
  lu_value decoded = {0};
  lazy* l;
  if (!v) return 0;
  if (v->kind != LU_VALUE_LAZY_ARRAY) return 1;
  l = (lazy*)payload(v);
  if (!l->callbacks.decode(l->context, v->type, v->size, &decoded) ||
      decoded.kind != LU_VALUE_ARRAY || decoded.type != v->type || decoded.size != v->size) {
    lu_value_destroy(&decoded);
    return 0;
  }
  publish(v, &decoded);
  return 1;
}

typedef struct pair { const void* a; const void* b; uint64_t hash; int state; } pair;
typedef struct memo { pair* rows; size_t capacity, count; } memo;
static size_t pointer_hash(const void* a, const void* b) {
  uintptr_t x = (uintptr_t)a >> 3;
  x ^= ((uintptr_t)b >> 3) + (uintptr_t)0x9e3779b9u + (x << 6) + (x >> 2);
  x ^= x >> (sizeof(x) * 4);
  x *= (uintptr_t)0x85ebca6bu;
  return (size_t)x;
}
static pair* memo_slot(memo* m, const void* a, const void* b) {
  size_t i = pointer_hash(a, b) & (m->capacity - 1);
  while (m->rows[i].a && (m->rows[i].a != a || m->rows[i].b != b)) i = (i + 1) & (m->capacity - 1);
  return &m->rows[i];
}
static pair* memo_get(memo* m, const void* a, const void* b) {
  pair* p;
  if (!m->capacity || (m->count + 1) > m->capacity / 2) {
    memo grown;
    size_t i;
    grown.capacity = m->capacity ? m->capacity * 2 : 32;
    if (grown.capacity < m->capacity || grown.capacity > SIZE_MAX / sizeof(pair)) return NULL;
    grown.rows = (pair*)calloc(grown.capacity, sizeof(pair));
    if (!grown.rows) return NULL;
    grown.count = m->count;
    for (i = 0; i < m->capacity; ++i) if (m->rows[i].a) *memo_slot(&grown, m->rows[i].a, m->rows[i].b) = m->rows[i];
    free(m->rows);
    *m = grown;
  }
  p = memo_slot(m, a, b);
  if (!p->a) { p->a = a; p->b = b; m->count++; }
  return p;
}
static int array_equal(const lu_value* a, const lu_value* b) {
  size_t size = lu_value_bytes(a), i;
  uint16_t component = lu_value_array_component(a->type);
  const void* ad = lu_value_data(a);
  const void* bd = lu_value_data(b);
  if (lu_value_uses_string_storage(a->type)) {
    for (i = 0; i < a->size; ++i) {
      lu_view x = lu_value_string_at(a, i), y = lu_value_string_at(b, i);
      if (x.len != y.len || (x.len && memcmp(x.data, y.data, x.len))) return 0;
    }
    return 1;
  }
  if (size != lu_value_bytes(b)) return 0;
  if (component == LU_TYPE_FLOAT) {
    const float* x = (const float*)ad;
    const float* y = (const float*)bd;
    for (i = 0; i < size / sizeof(float); ++i) if (x[i] != y[i]) return 0;
    return 1;
  }
  if (component == LU_TYPE_DOUBLE) {
    const double* x = (const double*)ad;
    const double* y = (const double*)bd;
    for (i = 0; i < size / sizeof(double); ++i) if (x[i] != y[i]) return 0;
    return 1;
  }
  return !size || memcmp(ad, bd, size) == 0;
}
int lu_value_equal(const lu_value* left, const lu_value* right) {
  typedef struct work { const lu_value* a; const lu_value* b; } work;
  work local[32], *stack = local;
  size_t count = 1, capacity = 32;
  memo visited = {NULL, 0, 0};
  int result = 0;
  if (!left || !right) return left == right;
  stack[0].a = left;
  stack[0].b = right;
  while (count) {
    work current = stack[--count];
    const lu_value *a = current.a, *b = current.b;
    if (!lu_value_materialize((lu_value*)a) || !lu_value_materialize((lu_value*)b)) goto done;
    if (a->type != b->type || a->kind != b->kind) goto done;
    if (a->kind == LU_VALUE_EMPTY || a->kind == LU_VALUE_BLOCK) continue;
    if (a->kind == LU_VALUE_ARRAY) {
      if (a->size != b->size || !array_equal(a, b)) goto done;
    } else if (a->kind == LU_VALUE_DICT) {
      const dict* x = (const dict*)payload(a);
      const dict* y = (const dict*)payload(b);
      pair* seen;
      size_t i;
      if (x == y) continue;
      if (x->count != y->count || !(seen = memo_get(&visited, x, y))) goto done;
      if (seen->state) continue;
      seen->state = 1;
      if (x->count > SIZE_MAX - count) goto done;
      if (x->count + count > capacity) {
        size_t n = capacity;
        work* grown;
        while (n < x->count + count) { if (n > SIZE_MAX / 2) goto done; n *= 2; }
        if (n > SIZE_MAX / sizeof(work)) goto done;
        grown = (work*)malloc(n * sizeof(work));
        if (!grown) goto done;
        memcpy(grown, stack, count * sizeof(work));
        if (stack != local) free(stack);
        stack = grown;
        capacity = n;
      }
      size_t* local_slots = NULL;
      size_t local_capacity = 0;
      if (y->index == NULL && y->count > 32) {
        local_slots = dict_build_slots(y, &local_capacity);
      }
      for (i = 0; i < x->count; ++i) {
        const dict_entry* entry = &centries(x)[i];
        const lu_view key = lu_value_string(&entry->key);
        /* USD dictionaries are unordered maps: match by key, not by the
         * insertion slot. This also makes equality independent of authored /
         * read order. */
        const size_t match =
            local_slots ? dict_find_slots(y, local_slots, local_capacity,
                                          key.data, key.len)
                        : dict_find(y, key.data, key.len);
        if (match == SIZE_MAX) {
          free(local_slots);
          goto done;
        }
        stack[count].a = &entry->value;
        stack[count++].b = &centries(y)[match].value;
      }
      free(local_slots);
    } else {
      size_t size = lu_value_bytes(a);
      if (size != lu_value_bytes(b) || (size && memcmp(lu_value_data(a), lu_value_data(b), size))) goto done;
    }
  }
  result = 1;
done:
  if (stack != local) free(stack);
  free(visited.rows);
  return result;
}
static uint64_t leaf_hash(const lu_value* v) {
  uint64_t h = v->type;
  size_t i, size;
  const void* data;
  if (!v->type) return 0;
  if (v->kind == LU_VALUE_ARRAY) {
    h |= 0x100u;
    if (lu_value_uses_string_storage(v->type)) {
      for (i = 0; i < v->size; ++i) {
        lu_view s = lu_value_string_at(v, i);
        h ^= bytes_hash(s.data, s.len);
        h *= UINT64_C(1099511628211);
      }
      return h;
    }
  }
  size = lu_value_bytes(v);
  data = lu_value_data(v);
  return size ? h ^ bytes_hash(data, size) : h;
}
static uint64_t hash_mix64(uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}
static uint64_t hash_entry(uint64_t key_hash, uint64_t value_hash) {
  return hash_mix64(key_hash ^ (value_hash + UINT64_C(0x9e3779b97f4a7c15) +
                                (key_hash << 6) + (key_hash >> 2)));
}
uint64_t lu_value_hash(const lu_value* value) {
  /* Dictionary hashing is order-independent: each entry contributes a mixed
   * (key, value) hash to a commutative sum. Ordinary leaf/array values keep
   * their order-sensitive hash. Cycles are memoized exactly like equality. */
  typedef struct frame {
    const dict* d;
    size_t next;
    uint64_t sum;
    uint64_t key_hash;
  } frame;
  frame local[32], *stack = local;
  size_t count = 0, capacity = 32;
  memo seen = {NULL, 0, 0};
  uint64_t result = 0;
  pair* record;
  if (!value || !lu_value_materialize((lu_value*)value)) return 0;
  if (value->kind != LU_VALUE_DICT) return leaf_hash(value);
  local[0].d = (const dict*)payload(value);
  local[0].next = 0;
  local[0].sum = 0;
  local[0].key_hash = 0;
  count = 1;
  record = memo_get(&seen, local[0].d, NULL);
  if (!record) goto done;
  record->state = 1;
  while (count) {
    frame* f = &stack[count - 1];
    const dict_entry* e;
    const lu_value* child;
    lu_view key;
    if (f->next == f->d->count) {
      const uint64_t completed_key = f->key_hash;
      const uint64_t h = hash_mix64(LU_TYPE_DICTIONARY + f->sum);
      record = memo_slot(&seen, f->d, NULL);
      record->hash = h;
      record->state = 2;
      count--;
      if (!count) { result = h; break; }
      stack[count - 1].sum += hash_entry(completed_key, h);
      continue;
    }
    e = &centries(f->d)[f->next++];
    child = &e->value;
    key = lu_value_string(&e->key);
    const uint64_t key_hash = bytes_hash(key.data, key.len);
    if (!lu_value_materialize((lu_value*)child)) goto done;
    if (child->kind != LU_VALUE_DICT) {
      f->sum += hash_entry(key_hash, leaf_hash(child));
      continue;
    }
    record = memo_get(&seen, payload(child), NULL);
    if (!record) goto done;
    if (record->state) {
      f->sum += hash_entry(key_hash, record->state == 2 ? record->hash
                                                        : LU_TYPE_DICTIONARY);
    } else {
      record->state = 1;
      if (count == capacity) {
        frame* grown;
        if (capacity > SIZE_MAX / 2 / sizeof(frame)) goto done;
        grown = (frame*)malloc(capacity * 2 * sizeof(frame));
        if (!grown) goto done;
        memcpy(grown, stack, count * sizeof(frame));
        if (stack != local) free(stack);
        stack = grown;
        capacity *= 2;
      }
      stack[count].d = (const dict*)payload(child);
      stack[count].next = 0;
      stack[count].sum = 0;
      stack[count].key_hash = key_hash;
      count++;
    }
  }
done:
  if (stack != local) free(stack);
  free(seen.rows);
  return result;
}
static size_t add_size(size_t a, size_t b) { return a > SIZE_MAX - b ? SIZE_MAX : a + b; }
size_t lu_value_heap_bytes(const lu_value* value) {
  const lu_value* local[32];
  const lu_value** stack = local;
  size_t count = 1, capacity = 32, bytes = 0;
  memo seen = {NULL, 0, 0};
  if (!value) return 0;
  stack[0] = value;
  while (count) {
    const lu_value* v = stack[--count];
    lu_buffer* b = lu_value_buffer(v);
    if (b) bytes = add_size(bytes, b->charged);
    else if (v->kind == LU_VALUE_LAZY_ARRAY) bytes = add_size(bytes, ((lazy*)payload(v))->allocation->charged);
    else if (v->kind == LU_VALUE_ARRAY && lu_value_uses_string_storage(v->type))
      bytes = add_size(bytes, ((strings*)payload(v))->allocation->charged);
    else if (v->kind == LU_VALUE_DICT) {
      const dict* d = (const dict*)payload(v);
      pair* record = memo_get(&seen, d, NULL);
      size_t i;
      if (!record) { bytes = SIZE_MAX; break; }
      if (record->state) continue;
      record->state = 1;
      bytes = add_size(bytes, d->allocation->charged);
      if (d->entries) bytes = add_size(bytes, d->entries->charged);
      if (d->index) bytes = add_size(bytes, d->index->charged);
      if (d->count > (SIZE_MAX - count) / 2) { bytes = SIZE_MAX; break; }
      if (count + d->count * 2 > capacity) {
        size_t n = capacity;
        const lu_value** grown;
        while (n < count + d->count * 2) {
          if (n > SIZE_MAX / 2) { bytes = SIZE_MAX; goto done; }
          n *= 2;
        }
        if (n > SIZE_MAX / sizeof(*grown)) { bytes = SIZE_MAX; break; }
        grown = (const lu_value**)malloc(n * sizeof(*grown));
        if (!grown) { bytes = SIZE_MAX; break; }
        memcpy(grown, stack, count * sizeof(*grown));
        if (stack != local) free(stack);
        stack = grown;
        capacity = n;
      }
      for (i = 0; i < d->count; ++i) {
        stack[count++] = &centries(d)[i].key;
        stack[count++] = &centries(d)[i].value;
      }
    }
  }
done:
  if (stack != local) free(stack);
  free(seen.rows);
  return bytes;
}
