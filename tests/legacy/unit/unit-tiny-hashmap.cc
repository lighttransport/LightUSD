// SPDX-License-Identifier: MIT
// Copyright 2026 - Present, Light Transport Entertainment, Inc.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-tiny-hashmap.h"
#include "tiny-hashmap.hh"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

using lightusd::legacy::HashMap;

void tiny_hashmap_basic_insert_find_test(void) {
  HashMap<std::string, int> m;
  TEST_CHECK(m.empty());
  TEST_CHECK(m.size() == 0);

  for (int i = 0; i < 1000; ++i) {
    auto r = m.emplace("k" + std::to_string(i), i);
    TEST_CHECK(r.second == true);
  }
  TEST_CHECK(m.size() == 1000);

  for (int i = 0; i < 1000; ++i) {
    auto it = m.find("k" + std::to_string(i));
    TEST_CHECK(it != m.end());
    TEST_CHECK((*it).second == i);
    TEST_CHECK(m.count("k" + std::to_string(i)) == 1);
    TEST_CHECK(m.contains("k" + std::to_string(i)));
  }

  TEST_CHECK(m.find("missing") == m.end());
  TEST_CHECK(m.count("missing") == 0);
  TEST_CHECK(!m.contains("missing"));
}

void tiny_hashmap_operator_subscript_test(void) {
  HashMap<std::string, int> m;
  m["a"];
  TEST_CHECK(m.size() == 1);
  TEST_CHECK(m["a"] == 0);

  m["a"] = 42;
  TEST_CHECK(m.size() == 1);
  TEST_CHECK(m["a"] == 42);

  m["b"] = 7;
  TEST_CHECK(m.size() == 2);
  TEST_CHECK(m.at("b") == 7);
  TEST_CHECK(m.at("a") == 42);
}

void tiny_hashmap_erase_test(void) {
  HashMap<std::string, int> m;
  for (int i = 0; i < 64; ++i) {
    m.emplace("k" + std::to_string(i), i);
  }
  TEST_CHECK(m.size() == 64);

  // erase by key
  TEST_CHECK(m.erase("k10") == 1);
  TEST_CHECK(m.erase("k10") == 0);
  TEST_CHECK(m.size() == 63);
  TEST_CHECK(!m.contains("k10"));
  TEST_CHECK(m.contains("k11"));

  // erase by iterator
  auto it = m.find("k20");
  TEST_CHECK(it != m.end());
  m.erase(it);
  TEST_CHECK(!m.contains("k20"));
  TEST_CHECK(m.size() == 62);

  // remaining items are still findable
  for (int i = 0; i < 64; ++i) {
    if (i == 10 || i == 20) continue;
    TEST_CHECK(m.contains("k" + std::to_string(i)));
  }

  // erase everything
  for (int i = 0; i < 64; ++i) {
    m.erase("k" + std::to_string(i));
  }
  TEST_CHECK(m.empty());
}

void tiny_hashmap_rehash_growth_test(void) {
  HashMap<int, int> m;
  for (int i = 0; i < 5000; ++i) {
    m.emplace(i, i * 2);
  }
  TEST_CHECK(m.size() == 5000);
  for (int i = 0; i < 5000; ++i) {
    TEST_CHECK(m.contains(i));
    TEST_CHECK(m.at(i) == i * 2);
  }

  // reserve avoids rehash
  HashMap<int, int> m2;
  m2.reserve(1024);
  size_t cap_before = m2.bucket_count();
  for (int i = 0; i < 500; ++i) {
    m2.emplace(i, i);
  }
  TEST_CHECK(m2.bucket_count() == cap_before);
  TEST_CHECK(m2.size() == 500);
}

namespace {
struct AlwaysZeroHash {
  size_t operator()(int) const { return 0; }
  size_t operator()(const std::string &) const { return 0; }
};
}  // namespace

void tiny_hashmap_collision_stress_test(void) {
  HashMap<int, int, AlwaysZeroHash> m;
  m.reserve(512);
  for (int i = 0; i < 256; ++i) {
    auto r = m.emplace(i, i + 1000);
    TEST_CHECK(r.second);
  }
  TEST_CHECK(m.size() == 256);
  for (int i = 0; i < 256; ++i) {
    auto it = m.find(i);
    TEST_CHECK(it != m.end());
    TEST_CHECK((*it).second == i + 1000);
  }
  // erase half, verify the rest still findable
  for (int i = 0; i < 256; i += 2) {
    TEST_CHECK(m.erase(i) == 1);
  }
  TEST_CHECK(m.size() == 128);
  for (int i = 1; i < 256; i += 2) {
    TEST_CHECK(m.contains(i));
  }
  for (int i = 0; i < 256; i += 2) {
    TEST_CHECK(!m.contains(i));
  }
}

void tiny_hashmap_move_only_value_test(void) {
  HashMap<std::string, std::unique_ptr<int>> m;
  m.emplace("one", std::unique_ptr<int>(new int(1)));
  m.emplace("two", std::unique_ptr<int>(new int(2)));
  m.emplace("three", std::unique_ptr<int>(new int(3)));
  TEST_CHECK(m.size() == 3);

  auto it = m.find("two");
  TEST_CHECK(it != m.end());
  TEST_CHECK(*((*it).second) == 2);

  TEST_CHECK(m.erase("one") == 1);
  TEST_CHECK(m.size() == 2);
  TEST_CHECK(!m.contains("one"));
  TEST_CHECK(m.contains("three"));
}

void tiny_hashmap_iteration_coverage_test(void) {
  HashMap<int, int> m;
  std::set<int> inserted;
  for (int i = 0; i < 200; ++i) {
    m.emplace(i, i);
    inserted.insert(i);
  }
  std::set<int> seen;
  for (auto it = m.begin(); it != m.end(); ++it) {
    seen.insert((*it).first);
  }
  TEST_CHECK(seen == inserted);

  // const iteration
  const HashMap<int, int> &cm = m;
  std::set<int> seen2;
  for (auto it = cm.cbegin(); it != cm.cend(); ++it) {
    seen2.insert((*it).first);
  }
  TEST_CHECK(seen2 == inserted);
}

void tiny_hashmap_copy_move_test(void) {
  HashMap<std::string, int> m;
  for (int i = 0; i < 50; ++i) {
    m.emplace("k" + std::to_string(i), i);
  }

  HashMap<std::string, int> copy = m;
  TEST_CHECK(copy.size() == 50);
  for (int i = 0; i < 50; ++i) {
    TEST_CHECK(copy.at("k" + std::to_string(i)) == i);
  }
  // independent storage
  copy["k0"] = 999;
  TEST_CHECK(m.at("k0") == 0);
  TEST_CHECK(copy.at("k0") == 999);

  HashMap<std::string, int> moved = std::move(copy);
  TEST_CHECK(moved.size() == 50);
  TEST_CHECK(moved.at("k0") == 999);

  HashMap<std::string, int> assigned;
  assigned = m;
  TEST_CHECK(assigned.size() == 50);

  HashMap<std::string, int> move_assigned;
  move_assigned = std::move(moved);
  TEST_CHECK(move_assigned.size() == 50);
  TEST_CHECK(move_assigned.at("k0") == 999);
}

void tiny_hashmap_clear_reuse_test(void) {
  HashMap<int, int> m;
  for (int i = 0; i < 100; ++i) m.emplace(i, i);
  size_t cap = m.bucket_count();
  m.clear();
  TEST_CHECK(m.empty());
  TEST_CHECK(m.size() == 0);
  TEST_CHECK(m.bucket_count() == cap);
  for (int i = 0; i < 50; ++i) m.emplace(i, i + 1);
  TEST_CHECK(m.size() == 50);
  for (int i = 0; i < 50; ++i) {
    TEST_CHECK(m.at(i) == i + 1);
  }
}

void tiny_hashmap_load_factor_test(void) {
  HashMap<int, int> m;
  m.max_load_factor(0.25f);
  TEST_CHECK(m.max_load_factor() == 0.25f);
  for (int i = 0; i < 100; ++i) m.emplace(i, i);
  // With 0.25 max load, bucket_count must be > 4 * size at the moment of last
  // rehash. After inserting 100, capacity should be at least 512.
  TEST_CHECK(m.bucket_count() >= 256);
  TEST_CHECK(m.size() == 100);
  for (int i = 0; i < 100; ++i) {
    TEST_CHECK(m.at(i) == i);
  }
}

void tiny_hashmap_insert_or_assign_test(void) {
  HashMap<std::string, int> m;
  auto r1 = m.insert_or_assign("a", 1);
  TEST_CHECK(r1.second == true);
  TEST_CHECK(m["a"] == 1);

  auto r2 = m.insert_or_assign("a", 42);
  TEST_CHECK(r2.second == false);
  TEST_CHECK(m["a"] == 42);
  TEST_CHECK(m.size() == 1);

  m.insert_or_assign("b", 7);
  TEST_CHECK(m.size() == 2);
  TEST_CHECK(m.at("b") == 7);
}

void tiny_hashmap_at_miss_test(void) {
  HashMap<std::string, int> m;
  m["present"] = 9;
  size_t before = m.size();

  int &missing = m.at("missing");
  TEST_CHECK(m.size() == before);
  TEST_CHECK(!m.contains("missing"));
  TEST_CHECK(missing == 0);

  missing = 123;
  TEST_CHECK(m.size() == before);
  TEST_CHECK(!m.contains("missing"));

  const HashMap<std::string, int> &cm = m;
  const int &const_missing = cm.at("missing");
  TEST_CHECK(const_missing == 0);
}

void tiny_hashmap_int_keys_test(void) {
  HashMap<int, int> m;
  std::unordered_map<int, int> ref;
  m.reserve(200000);
  for (int i = 0; i < 100000; ++i) {
    int k = i * 17 + 3;
    m.emplace(k, i);
    ref.emplace(k, i);
  }
  TEST_CHECK(m.size() == ref.size());
  for (auto &kv : ref) {
    auto it = m.find(kv.first);
    TEST_CHECK(it != m.end());
    TEST_CHECK((*it).second == kv.second);
  }
  // erase a third
  for (int i = 0; i < 100000; i += 3) {
    int k = i * 17 + 3;
    TEST_CHECK(m.erase(k) == 1);
  }
  TEST_CHECK(m.size() == ref.size() - ((100000 + 2) / 3));
}
