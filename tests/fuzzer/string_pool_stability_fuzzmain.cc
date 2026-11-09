// SPDX-License-Identifier: MIT
// Fuzzer for StringPool null handling and interned pointer stability.

#include <cstddef>
#include <cstdint>

#include "string-pool.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0 || size > 4096) return 0;

  static const char *const kKeys[] = {
      "string_pool_fuzz_00", "string_pool_fuzz_01", "string_pool_fuzz_02",
      "string_pool_fuzz_03", "string_pool_fuzz_04", "string_pool_fuzz_05",
      "string_pool_fuzz_06", "string_pool_fuzz_07", "string_pool_fuzz_08",
      "string_pool_fuzz_09", "string_pool_fuzz_10", "string_pool_fuzz_11",
      "string_pool_fuzz_12", "string_pool_fuzz_13", "string_pool_fuzz_14",
      "string_pool_fuzz_15"};

  lightusd::legacy::StringPool &pool = lightusd::legacy::StringPool::instance();
  const char *ptrs[sizeof(kKeys) / sizeof(kKeys[0])] = {};
  const size_t nkeys = sizeof(kKeys) / sizeof(kKeys[0]);

  (void)pool.intern(static_cast<const char *>(nullptr));
  (void)pool.lookup(static_cast<const char *>(nullptr));

  for (size_t i = 0; i < size; i++) {
    const size_t idx = size_t(data[i]) % nkeys;
    const char *p = pool.intern(kKeys[idx]);
    if (ptrs[idx] == nullptr) {
      ptrs[idx] = p;
    } else if (ptrs[idx] != p) {
      __builtin_trap();
    }

    const char *found = pool.lookup(kKeys[idx]);
    if (found != p) {
      __builtin_trap();
    }
  }

  return 0;
}
