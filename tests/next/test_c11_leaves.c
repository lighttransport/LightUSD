/* SPDX-License-Identifier: Apache-2.0 */
#include "next/c/string-index.h"
#include "next/c/usda-string.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const char* key(const void* context, size_t i, size_t* length) {
  const char* const* keys = (const char* const*)context;
  if (!keys[i]) return NULL;
  *length = strlen(keys[i]); return keys[i];
}
int main(void) {
  lu_string_index index = {0};
  char storage[512][20]; const char* keys[514];
  for (size_t i = 0; i < 512; ++i) {
    snprintf(storage[i], sizeof(storage[i]), "key-%zu", i); keys[i] = storage[i];
    assert(lu_string_index_insert(&index, i, key, keys));
  }
  keys[512] = keys[3]; keys[513] = NULL;
  assert(lu_string_index_insert(&index, 512, key, keys));
  assert(index.count == 512);
  for (size_t i = 0; i < 512; ++i)
    assert(lu_string_index_find(&index, keys[i], strlen(keys[i]), key, keys) == i);
  assert(!lu_string_index_rebuild(&index, 514, key, keys));
  assert(!lu_string_index_rebuild(&index, SIZE_MAX, key, keys));
  assert(index.count == 512);
  assert(lu_string_index_find(&index, "key-3", 5, key, keys) == 3);
  assert(lu_string_index_find(&index, "missing", 7, key, keys) == SIZE_MAX);
  assert(lu_string_index_rebuild(&index, 513, key, keys));
  assert(index.count == 512);
  lu_string_index_destroy(&index); lu_string_index_destroy(&index);
  assert(lu_string_index_rebuild(&index, 0, key, keys));
  lu_string_index_destroy(&index);
  const char source[] = {'"', '\\', '\n', '\r', '\t', 0, 0x1f, 0x7f, (char)0xc3, (char)0xa9};
  const char expected[] = "\"\\\"\\\\\\n\\r\\t\\x00\\x1f\\x7f\xc3\xa9\"";
  char out[64]; memset(out, '#', sizeof(out));
  size_t n = lu_usda_quote(source, sizeof(source), NULL, 0);
  assert(n == sizeof(expected)-1);
  assert(lu_usda_quote(source, sizeof(source), out, n-1) == n);
  assert(out[0] == '#');
  assert(lu_usda_quote(source, sizeof(source), out, n) == n);
  assert(memcmp(out, expected, n) == 0 && out[n] == '#');
  assert(lu_usda_quote(NULL, 1, out, sizeof(out)) == SIZE_MAX);
  assert(lu_usda_quote(NULL, 0, out, sizeof(out)) == 2);
  assert(out[0] == '"' && out[1] == '"');
  return 0;
}
