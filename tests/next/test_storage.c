/* SPDX-License-Identifier: Apache-2.0 */
#include "next/c/value.h"
#include "next/c/chunks.h"
#include "next/c/descriptor.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* string_at(const void* context, size_t i, size_t* size) {
  const char* const* values = (const char* const*)context;
  *size = strlen(values[i]);
  return values[i];
}
static int destroyed;
static void destroy_lazy(void* context) { destroyed++; free(context); }
static int decode_lazy(const void* context, uint16_t type, uint32_t count, lu_value* out) {
  return lu_value_set_array(out, type, context, count, NULL);
}
static int fail_lazy(const void* context, uint16_t type, uint32_t count, lu_value* out) {
  (void)context; (void)type; (void)count; (void)out;
  return 0;
}
static void buffers(void) {
  lu_buffer *a = lu_buffer_copy("abcdef", 6, 64, NULL), *b;
  lu_budget* budget = lu_budget_create(512);
  lu_buffer* limited = lu_buffer_create(32, 16, budget);
  size_t tracked = lu_budget_current(budget);
  assert(a && limited && tracked > 32);
  assert(((uintptr_t)lu_buffer_data(a) & 63u) == 0);
  b = a; lu_buffer_retain(b);
  assert(lu_buffer_append(&a, (const char*)lu_buffer_data(a) + 1, 3));
  assert(lu_buffer_size(a) == 9 && memcmp(lu_buffer_data(a), "abcdefbcd", 9) == 0);
  assert(lu_buffer_size(b) == 6 && memcmp(lu_buffer_data(b), "abcdef", 6) == 0);
  assert(!lu_buffer_resize(&limited, 1024));
  assert(lu_buffer_size(limited) == 32 && lu_budget_current(budget) == tracked);
  assert(!lu_buffer_append(&a, lu_buffer_data(a), SIZE_MAX));
  assert(!lu_buffer_create(SIZE_MAX, 16, NULL));
  assert(!lu_buffer_create(8, 3, NULL));
  lu_buffer_release(a); lu_buffer_release(b); lu_buffer_release(limited);
  assert(lu_budget_current(budget) == 0 && lu_budget_peak(budget) == tracked);
  lu_budget_release(budget);
}
static void values(void) {
  lu_value a = {0}, b = {0}, box = {0};
  double matrix[16] = {1, 2, 3};
  float data[] = {1, 2, 3, 4, 5, 6};
  const char* strings[] = {"", "a very long string which cannot fit into the inline payload", "token"};
  assert(lu_value_set_array(&a, LU_TYPE_FLOAT3, data, 2, NULL));
  assert(a.size == 2 && lu_value_array_scalar_count(&a) == 6);
  lu_value_copy(&b, &a);
  assert(lu_value_data(&a) == lu_value_data(&b));
  ((float*)lu_value_mutable_data(&b))[0] = 7;
  assert(((const float*)lu_value_data(&a))[0] == 1);
  assert(!lu_value_equal(&a, &b));
  assert(lu_value_array_resize(&b, 3));
  assert(((const float*)lu_value_data(&b))[8] == 0);
  assert(!lu_value_array_resize(&b, SIZE_MAX));
  assert(lu_value_set_pod(&box, LU_TYPE_MATRIX4D, matrix, sizeof(matrix), NULL));
  lu_value_copy(&b, &box);
  ((double*)lu_value_mutable_data(&b))[0] = 9;
  assert(((const double*)lu_value_data(&box))[0] == 1);
  assert(lu_value_set_string(&a, LU_TYPE_TOKEN, "hi\0x", 4, NULL));
  assert(lu_value_string(&a).len == 4);
  lu_value_copy(&b, &a);
  assert(lu_value_equal(&a, &b) && lu_value_hash(&a) == lu_value_hash(&b));
  assert(lu_value_set_strings(&a, LU_TYPE_TOKEN, 3, string_at, strings, NULL));
  lu_value_copy(&b, &a);
  assert(lu_value_equal(&a, &b));
  assert(lu_value_string_at(&a, 1).len == strlen(strings[1]));
  assert(lu_value_string_at(&a, 3).data == NULL);
  lu_value_destroy(&a); lu_value_destroy(&b); lu_value_destroy(&box);
}
static void dictionaries(void) {
  lu_value a = {0}, b = {0}, child = {0};
  int32_t n;
  size_t i;
  char key[2] = {'a', 0};
  assert(lu_value_make_dict(&a, NULL));
  for (i = 0; i < 24; ++i) {
    key[0] = (char)('a' + i); n = (int32_t)i;
    assert(lu_value_set_pod(&child, LU_TYPE_INT, &n, sizeof(n), NULL));
    assert(lu_value_dict_set(&a, key, 1, &child));
  }
  for (i = 0; i < 24; ++i) {
    const lu_value* found;
    key[0] = (char)('a' + i);
    found = lu_value_dict_find(&a, key, 1);
    assert(found && *(const int32_t*)lu_value_data(found) == (int32_t)i);
    assert(lu_value_dict_key(&a, i).data[0] == key[0]);
  }
  lu_value_copy(&b, &a);
  assert(lu_value_dict_set_typed(&b, 1));
  assert(lu_value_equal(&a, &b)); /* JSON wrapper flags do not alter USD equality. */
  assert(lu_value_hash(&a) == lu_value_hash(&b));
  assert(!lu_value_dict_typed(&a) && lu_value_dict_typed(&b));
  /* Both a replacement's key and child may alias the current dictionary. */
  assert(lu_value_dict_set(&b, lu_value_dict_key(&b, 0).data, 1, lu_value_dict_value(&b, 23)));
  assert(*(const int32_t*)lu_value_data(lu_value_dict_find(&a, "a", 1)) == 0);
  assert(*(const int32_t*)lu_value_data(lu_value_dict_find(&b, "a", 1)) == 23);
  lu_value_destroy(&a); lu_value_destroy(&b);
  /* Dictionaries are unordered: insertion order must not affect equality or
   * hashing, including for nested dictionaries. */
  {
    lu_value x = {0}, y = {0}, cx = {0}, cy = {0}, v = {0};
    int32_t one = 1, two = 2, three = 3;
    assert(lu_value_make_dict(&x, NULL) && lu_value_make_dict(&y, NULL));
    assert(lu_value_make_dict(&cx, NULL) && lu_value_make_dict(&cy, NULL));
    assert(lu_value_set_pod(&v, LU_TYPE_INT, &one, sizeof(one), NULL));
    assert(lu_value_dict_set(&cx, "a", 1, &v));
    assert(lu_value_set_pod(&v, LU_TYPE_INT, &two, sizeof(two), NULL));
    assert(lu_value_dict_set(&cx, "b", 1, &v));
    assert(lu_value_dict_set(&cy, "b", 1, &v));
    assert(lu_value_set_pod(&v, LU_TYPE_INT, &one, sizeof(one), NULL));
    assert(lu_value_dict_set(&cy, "a", 1, &v));
    assert(lu_value_dict_set(&x, "nested", 6, &cx));
    assert(lu_value_set_pod(&v, LU_TYPE_INT, &three, sizeof(three), NULL));
    assert(lu_value_dict_set(&x, "z", 1, &v));
    assert(lu_value_dict_set(&y, "z", 1, &v));
    assert(lu_value_dict_set(&y, "nested", 6, &cy));
    assert(lu_value_equal(&x, &y));
    assert(lu_value_hash(&x) == lu_value_hash(&y));
    lu_value_destroy(&x); lu_value_destroy(&y);
    lu_value_destroy(&cx); lu_value_destroy(&cy); lu_value_destroy(&v);
  }
  /* Large dictionaries must remain order-independent without relying on the
   * insertion order used to build either index. */
  {
    lu_value x = {0}, y = {0}, v = {0};
    int32_t n;
    char key[16];
    assert(lu_value_make_dict(&x, NULL) && lu_value_make_dict(&y, NULL));
    for (i = 0; i < 512; ++i) {
      n = (int32_t)i;
      snprintf(key, sizeof(key), "key_%zu", i);
      assert(lu_value_set_pod(&v, LU_TYPE_INT, &n, sizeof(n), NULL));
      assert(lu_value_dict_set(&x, key, strlen(key), &v));
    }
    for (i = 512; i > 0; --i) {
      n = (int32_t)(i - 1);
      snprintf(key, sizeof(key), "key_%zu", i - 1);
      assert(lu_value_set_pod(&v, LU_TYPE_INT, &n, sizeof(n), NULL));
      assert(lu_value_dict_set(&y, key, strlen(key), &v));
    }
    assert(lu_value_equal(&x, &y));
    assert(lu_value_hash(&x) == lu_value_hash(&y));
    lu_value_destroy(&x); lu_value_destroy(&y); lu_value_destroy(&v);
  }
  /* Destruction, equality and hashing must handle depth beyond the C stack. */
  for (i = 0; i < 20000; ++i) {
    assert(lu_value_make_dict(&a, NULL));
    assert(lu_value_dict_set(&a, "v", 1, &child));
    lu_value_move(&child, &a);
  }
  lu_value_copy(&b, &child);
  assert(lu_value_equal(&b, &child));
  assert(lu_value_hash(&child) == lu_value_hash(&b));
  assert(lu_value_heap_bytes(&child) > 20000);
  lu_value_destroy(&b); lu_value_destroy(&child);
}
static void lazy_values(void) {
  lu_value a = {0}, b = {0};
  lu_lazy_callbacks callbacks = {decode_lazy, destroy_lazy, NULL};
  float* data = (float*)malloc(3 * sizeof(float));
  data[0] = 3; data[1] = 4; data[2] = 5;
  assert(lu_value_set_lazy(&a, LU_TYPE_FLOAT, 3, data, &callbacks, NULL));
  lu_value_copy(&b, &a);
  const size_t before = lu_storage_materialization_epoch();
  assert(before != 0);
  assert(lu_value_materialize(&b));
  assert(lu_storage_materialization_epoch() == before + 1);
  assert(lu_value_materialize(&b));
  assert(lu_storage_materialization_epoch() == before + 1);
  assert(a.kind == LU_VALUE_LAZY_ARRAY && b.kind == LU_VALUE_ARRAY && destroyed == 0);
  assert(((const float*)lu_value_data(&b))[1] == 4);
  lu_value_destroy(&a); lu_value_destroy(&b);
  assert(destroyed == 1);
  callbacks.decode = fail_lazy;
  data = (float*)malloc(sizeof(float));
  assert(lu_value_set_lazy(&a, LU_TYPE_FLOAT, 1, data, &callbacks, NULL));
  const size_t before_failed = lu_storage_materialization_epoch();
  assert(!lu_value_materialize(&a) && a.kind == LU_VALUE_LAZY_ARRAY);
  assert(lu_storage_materialization_epoch() == before_failed);
  lu_value_destroy(&a);
  assert(destroyed == 2);
}
static int number_at(void* context, size_t index, lu_number* out) {
  *out = ((const lu_number*)context)[index];
  return 1;
}
static void numeric_ingestion(void) {
  lu_value value = {0}, saved = {0};
  lu_type_descriptor desc;
  lu_number numbers[3] = {
    {LU_NUMBER_UNSIGNED, {.unsigned_value = UINT64_MAX}},
    {LU_NUMBER_SIGNED, {.signed_value = -1}},
    {LU_NUMBER_FLOAT, {.float_value = 0.125}}
  };
  const lu_abi_info abi = lu_abi_get();
  assert(abi.major == 2 && abi.pointer_bits == sizeof(void*) * 8);
  /* Revision 1 described the old max_align_t-aligned carrier. */
  assert(abi.layout_revision == 2);
  assert(abi.layout_revision == LU_CORE_C_LAYOUT_REVISION);
  assert(abi.value_size == sizeof(lu_value) && abi.value_alignment == _Alignof(lu_value));
  assert(abi.value_store_offset == offsetof(lu_value, store));
  assert(abi.descriptor_size == sizeof(lu_type_descriptor));
  assert(abi.chunks_size == sizeof(lu_chunks));
  assert(lu_type_describe(LU_TYPE_HALF3, 1, &desc));
  assert(desc.storage == LU_COMP_FLOAT32 && desc.components == 3 && desc.stride == 12);
  assert(lu_type_describe(LU_TYPE_HALF, 0, &desc) && desc.storage == LU_COMP_FLOAT16);
  assert(lu_value_set_numbers(&value, LU_TYPE_UINT64, 0, 1, number_at, numbers, NULL));
  assert(*(const uint64_t*)lu_value_data(&value) == UINT64_MAX);
  lu_value_copy(&saved, &value);
  assert(!lu_value_set_numbers(&value, LU_TYPE_INT64, 0, 1, number_at, numbers, NULL));
  assert(lu_value_equal(&value, &saved));
  assert(!lu_value_set_numbers(&value, LU_TYPE_UCHAR, 1, 1, number_at, numbers, NULL));
  assert(lu_value_equal(&value, &saved));
  assert(!lu_value_set_numbers(&value, LU_TYPE_UINT, 0, 1, number_at, numbers + 1, NULL));
  assert(lu_value_equal(&value, &saved));
  assert(lu_value_set_numbers(&value, LU_TYPE_HALF, 1, 1, number_at, numbers + 2, NULL));
  assert(*(const float*)lu_value_data(&value) == 0.125f);
  assert(lu_value_set_numbers(&value, LU_TYPE_BOOL, 1, 3, number_at, numbers, NULL));
  assert(memcmp(lu_value_data(&value), "\1\1\1", 3) == 0);
  lu_value_destroy(&value); lu_value_destroy(&saved);
}
static void chunks(void) {
  lu_chunks a, b;
  const uint32_t values[] = {1, 2, 3};
  uint32_t copied[6];
  lu_budget* budget = lu_budget_create(4 * sizeof(uint32_t));
  assert(lu_chunks_init(&a, 4, 4, 4) && lu_chunks_init(&b, 4, 4, 4));
  assert(lu_chunks_append(&a, values, 3, budget));
  lu_chunks_share(&b, &a);
  assert(lu_chunks_shared(&a) && lu_chunks_shared(&b));
  assert(!lu_chunks_append(&b, values, 1, budget));
  assert(b.size == 3 && b.failed && lu_budget_current(budget) == 16);
  lu_chunks_destroy(&a);
  assert(lu_chunks_append(&b, values, 1, budget));
  assert(b.size == 4 && lu_chunks_copy(&b, copied, 6));
  assert(copied[0] == 1 && copied[2] == 3 && copied[3] == 1);
  lu_chunks_destroy(&b);
  assert(lu_budget_current(budget) == 0);
  lu_budget_release(budget);
  assert(lu_chunks_init(&a, 64, 64, 2));
  assert(lu_chunks_reserve(&a, 3, 1, NULL));
  assert(((uintptr_t)a.table->chunks[0] & 63u) == 0);
  assert(((uintptr_t)a.table->chunks[1] & 63u) == 0);
  lu_chunks_destroy(&a);
}
int main(void) {
  buffers(); values(); dictionaries(); lazy_values(); numeric_ingestion(); chunks();
  return 0;
}
