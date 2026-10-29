// SPDX-License-Identifier: Apache 2.0
// StringPool unit tests

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-string-pool.h"

#include <string>
#include <thread>
#include <vector>

#include "string-pool.hh"

using namespace lightusd;

void string_pool_intern_test(void) {
  StringPool &pool = StringPool::instance();

  // Intern a string and verify same pointer returned
  const char *s1 = pool.intern("hello");
  TEST_CHECK(s1 != nullptr);
  TEST_CHECK(std::string(s1) == "hello");

  const char *s2 = pool.intern("hello");
  TEST_CHECK(s1 == s2);  // Same pointer = deduplicated
  TEST_MSG("Interning same string should return same pointer");
}

void string_pool_intern_different_test(void) {
  StringPool &pool = StringPool::instance();

  const char *s1 = pool.intern("alpha");
  const char *s2 = pool.intern("beta");
  TEST_CHECK(s1 != nullptr);
  TEST_CHECK(s2 != nullptr);
  TEST_CHECK(s1 != s2);
  TEST_CHECK(std::string(s1) == "alpha");
  TEST_CHECK(std::string(s2) == "beta");
  TEST_MSG("Different strings should have different pointers");
}

void string_pool_lookup_test(void) {
  StringPool &pool = StringPool::instance();

  // Lookup existing string
  pool.intern("findme");
  const char *found = pool.lookup("findme");
  TEST_CHECK(found != nullptr);
  TEST_CHECK(std::string(found) == "findme");

  // Lookup non-existent string
  const char *not_found = pool.lookup("nothere");
  TEST_CHECK(not_found == nullptr);
  TEST_MSG("Lookup should return nullptr for unknown strings");
}

void string_pool_pre_intern_test(void) {
  StringPool &pool = StringPool::instance();

  pool.pre_intern({"one", "two", "three"});

  TEST_CHECK(pool.lookup("one") != nullptr);
  TEST_CHECK(pool.lookup("two") != nullptr);
  TEST_CHECK(pool.lookup("three") != nullptr);
  TEST_CHECK(pool.lookup("four") == nullptr);
  TEST_MSG("pre_intern should batch-intern strings");
}

void string_pool_size_test(void) {
  // Use the singleton; just verify size increases with unique strings
  StringPool &pool = StringPool::instance();
  size_t before = pool.size();
  pool.intern("size_test_unique_abcxyz");
  pool.intern("size_test_unique_abcxyz");  // duplicate
  pool.intern("size_test_unique_123456");
  TEST_CHECK(pool.size() >= before + 2);
  TEST_MSG("size() should count unique strings");
}

void string_pool_charptr_test(void) {
  StringPool &pool = StringPool::instance();

  // Verify both overloads work and return valid pointers
  const char *s1 = pool.intern("charptr_test_a");
  const char *s2 = pool.intern(std::string("charptr_test_b"));
  TEST_CHECK(s1 != nullptr);
  TEST_CHECK(s2 != nullptr);
  TEST_CHECK(s1 != s2);
  TEST_CHECK(std::string(s1) == "charptr_test_a");
  TEST_CHECK(std::string(s2) == "charptr_test_b");
  TEST_MSG("const char* and std::string overloads should work");
}

void string_pool_pre_intern_common_test(void) {
  // Test that PreInternCommonStrings doesn't crash
  PreInternCommonStrings();

  // Verify some common strings are interned
  StringPool &pool = StringPool::instance();
  TEST_CHECK(pool.lookup("Xform") != nullptr);
  TEST_CHECK(pool.lookup("Mesh") != nullptr);
  TEST_CHECK(pool.lookup("Material") != nullptr);
  TEST_CHECK(pool.lookup("material:binding") != nullptr);
  TEST_CHECK(pool.lookup("points") != nullptr);
  TEST_CHECK(pool.lookup("UsdPreviewSurface") != nullptr);
  TEST_MSG("PreInternCommonStrings should intern common USD strings");
}

void string_pool_concurrent_test(void) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  StringPool &pool = StringPool::instance();

  static constexpr int kThreads = 8;
  // static: MSVC requires a capture for a local const int used in the lambda.
  static constexpr int kPerThread = 2000;

  std::vector<std::thread> threads;
  std::vector<std::vector<const char *>> shared_ptrs(kThreads);
  std::vector<char> uniq_ok(kThreads, 1);

  for (int t = 0; t < kThreads; t++) {
    shared_ptrs[t].reserve(kPerThread);
    threads.emplace_back([&pool, t, &shared_ptrs, &uniq_ok]() {
      for (int i = 0; i < kPerThread; i++) {
        const char *shared = pool.intern("concurrent_shared_key");
        shared_ptrs[t].push_back(shared);

        std::string uniq =
            "concurrent_t" + std::to_string(t) + "_i" + std::to_string(i);
        const char *p = pool.intern(uniq);
        if (p == nullptr) uniq_ok[t] = 0;

        (void)pool.lookup("concurrent_shared_key");
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  const char *expected = pool.lookup("concurrent_shared_key");
  TEST_CHECK(expected != nullptr);
  for (int t = 0; t < kThreads; t++) {
    TEST_CHECK(uniq_ok[t] != 0);
    for (const char *p : shared_ptrs[t]) {
      TEST_CHECK(p == expected);
    }
  }
  TEST_MSG("Concurrent intern/lookup should be race-free and deduplicated");
#else
  TEST_MSG("Skipped: built without LIGHTUSD_ENABLE_THREAD");
#endif
}

// Edge case: intern nullptr via const char* overload should not crash.
void string_pool_nullptr_test(void) {
  StringPool &pool = StringPool::instance();

  const char *result = pool.intern(static_cast<const char *>(nullptr));
  TEST_CHECK(result == nullptr);

  const char *found = pool.lookup(static_cast<const char *>(nullptr));
  TEST_CHECK(found == nullptr);
}

// Edge case: intern empty string.
void string_pool_empty_string_test(void) {
  StringPool &pool = StringPool::instance();

  const char *s = pool.intern("");
  TEST_CHECK(s != nullptr);
  TEST_CHECK(std::string(s).empty());

  const char *s2 = pool.intern("");
  TEST_CHECK(s == s2);
}

// Edge case: interning a long string (not pathological, just > 1KB).
void string_pool_long_string_test(void) {
  StringPool &pool = StringPool::instance();

  std::string long_str(4096, 'X');
  const char *s = pool.intern(long_str);
  TEST_CHECK(s != nullptr);
  TEST_CHECK(std::string(s) == long_str);

  const char *s2 = pool.intern(long_str);
  TEST_CHECK(s == s2);
}

// Edge case: many unique strings to exercise pool growth.
void string_pool_many_strings_test(void) {
  StringPool &pool = StringPool::instance();
  size_t before = pool.size();

  const int N = 1000;
  std::vector<const char *> ptrs;
  ptrs.reserve(N);
  for (int i = 0; i < N; i++) {
    std::string key = "pool_growth_test_" + std::to_string(i);
    ptrs.push_back(pool.intern(key));
  }

  for (int i = 0; i < N; i++) {
    TEST_CHECK(ptrs[i] != nullptr);
    std::string key = "pool_growth_test_" + std::to_string(i);
    TEST_CHECK(std::string(ptrs[i]) == key);
  }
  for (int i = 0; i < N; i++) {
    for (int j = i + 1; j < N; j++) {
      TEST_CHECK(ptrs[i] != ptrs[j]);
    }
  }
  TEST_CHECK(pool.size() >= before + N);
}

// Edge case: PreInternCommonStrings idempotency.
void string_pool_pre_intern_idempotent_test(void) {
  PreInternCommonStrings();
  PreInternCommonStrings();
  PreInternCommonStrings();

  StringPool &pool = StringPool::instance();
  TEST_CHECK(pool.lookup("Mesh") != nullptr);
  TEST_CHECK(pool.lookup("Material") != nullptr);
}
