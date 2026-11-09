/* SPDX-License-Identifier: Apache-2.0 */
#include "string-index.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
static size_t Hash(const char* key, size_t length) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < length; ++i)
    hash = (hash ^ (uint8_t)key[i]) * 1099511628211ULL;
  return (size_t)hash;
}
static bool Equal(const char* key, size_t length, size_t index,
           lu_string_index_key keys, const void* context) {
  size_t candidate_length = 0;
  const char* candidate = keys(context, index, &candidate_length);
  return candidate && length == candidate_length &&
         (length == 0 || memcmp(key, candidate, length) == 0);
}

void lu_string_index_destroy(lu_string_index* s) {
  if (s) { free(s->slots); memset(s, 0, sizeof(*s)); }
}
size_t lu_string_index_find(const lu_string_index* s, const char* key, size_t length,
                           lu_string_index_key keys, const void* context) {
  if (!s || !key || !keys || !s->capacity) return SIZE_MAX;
  size_t slot = Hash(key, length) & (s->capacity - 1);
  for (size_t probe = 0; probe < s->capacity; ++probe) {
    if (!s->slots[slot]) return SIZE_MAX;
    size_t index = s->slots[slot] - 1;
    if (Equal(key, length, index, keys, context)) return index;
    slot = (slot + 1) & (s->capacity - 1);
  }
  return SIZE_MAX;
}
static void insert_reserved(lu_string_index* s, size_t index,
                            lu_string_index_key keys, const void* context) {
  size_t length = 0;
  const char* key = keys(context, index, &length);
  if (!key) return;
  size_t slot = Hash(key, length) & (s->capacity - 1);
  while (s->slots[slot]) {
    if (Equal(key, length, s->slots[slot] - 1, keys, context)) return;
    slot = (slot + 1) & (s->capacity - 1);
  }
  s->slots[slot] = index + 1;
  ++s->count;
}
static int reserve(lu_string_index* s, size_t count,
                   lu_string_index_key keys, const void* context) {
  if (count <= s->capacity / 2) return 1;
  size_t capacity = s->capacity ? s->capacity : 8;
  while (count > capacity / 2) {
    if (capacity > SIZE_MAX / 2) return 0;
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(size_t)) return 0;
  lu_string_index grown = {NULL, capacity, 0};
  grown.slots = (size_t*)calloc(capacity, sizeof(size_t));
  if (!grown.slots) return 0;
  for (size_t i = 0; i < s->capacity; ++i)
    if (s->slots[i]) insert_reserved(&grown, s->slots[i] - 1, keys, context);
  lu_string_index_destroy(s);
  *s = grown;
  return 1;
}
int lu_string_index_insert(lu_string_index* s, size_t index,
                           lu_string_index_key keys, const void* context) {
  size_t length = 0;
  if (!s || index == SIZE_MAX || !keys || !keys(context, index, &length)) return 0;
  if (s->count == SIZE_MAX || !reserve(s, s->count + 1, keys, context)) return 0;
  insert_reserved(s, index, keys, context);
  return 1;
}
int lu_string_index_rebuild(lu_string_index* s, size_t count,
                            lu_string_index_key keys, const void* context) {
  if (!s || !keys) return 0;
  lu_string_index rebuilt = {NULL, 0, 0};
  if (!reserve(&rebuilt, count, keys, context)) return 0;
  for (size_t i = 0; i < count; ++i) {
    size_t length = 0;
    if (!keys(context, i, &length)) { lu_string_index_destroy(&rebuilt); return 0; }
    insert_reserved(&rebuilt, i, keys, context);
  }
  lu_string_index_destroy(s);
  *s = rebuilt;
  return 1;
}
