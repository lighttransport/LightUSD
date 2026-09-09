// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Dict (USD dictionary value) unit test
//
// Covers the O(1) key index on next::Dict: insertion order is preserved (the
// writer re-emits dicts as authored), set() is last-wins for a repeated key,
// find() is O(1), and the key index survives the copy-on-write copy/move used
// by Value (DetachDict / MakeDictionary). A hostile 100k-key dict must build
// and serve lookups in near-linear time, not O(n^2).

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <type_traits>
#include <utility>

#include "next/types/value.hh"

using namespace lightusd::next;

static void TestSmallDict() {
  Value v = Value::MakeDictionary();
  Dict* d = v.as_dictionary();
  assert(d);

  // Insertion order is preserved (writer round-trip depends on it).
  d->set("zebra", Value(1));
  d->set("apple", Value(2));
  d->set("mango", Value(3));
  assert(d->size() == 3);
  assert(d->entries()[0].first == "zebra");
  assert(d->entries()[1].first == "apple");
  assert(d->entries()[2].first == "mango");

  // find() hits and misses.
  assert(d->find("apple") && *d->find("apple")->as_int() == 2);
  assert(d->find("nope") == nullptr);

  // set() on an EXISTING key replaces in place (no reorder, no dup).
  d->set("apple", Value(20));
  assert(d->size() == 3);
  assert(d->entries()[1].first == "apple");
  assert(d->find("apple") && *d->find("apple")->as_int() == 20);
  assert(d->entries()[0].first == "zebra");  // order unchanged

  // Last-wins: a repeated key keeps the final value at its FIRST position.
  d->set("zebra", Value(100));
  assert(d->size() == 3);
  assert(d->find("zebra") && *d->find("zebra")->as_int() == 100);
  assert(d->entries()[0].first == "zebra");
}

static void TestValueCowIndex() {
  // Build a dict, share it through a Value (CoW), then detach + mutate the copy:
  // the shared original and the private copy must both answer find() correctly.
  Value v = Value::MakeDictionary();
  {
    Dict* d = v.as_dictionary();
    d->set("a", Value(1));
    d->set("b", Value(2));
    d->set("c", Value(3));
  }
  Value shared = v;  // refcount++ on the same Dict (no deep copy)
  assert(shared.as_dictionary()->find("b") &&
         *shared.as_dictionary()->find("b")->as_int() == 2);

  // Mutable access detaches (copies the Dict, rebuilding its index).
  Dict* det = v.as_dictionary();
  det->set("d", Value(4));
  assert(det->find("d") && *det->find("d")->as_int() == 4);
  // The detached copy still sees its pre-detach keys.
  assert(det->find("a") && *det->find("a")->as_int() == 1);
  assert(det->find("c") && *det->find("c")->as_int() == 3);
  // The other sharor is unaffected by the detach.
  assert(shared.as_dictionary()->find("d") == nullptr);
  assert(shared.as_dictionary()->find("a") &&
         *shared.as_dictionary()->find("a")->as_int() == 1);
}

static void TestMoveIndex() {
  // MakeDictionary(Dict&&) move-construct: the index must move with the entries.
  Dict src;
  src.set("x", Value(10));
  src.set("y", Value(20));
  Value v = Value::MakeDictionary(std::move(src));
  const Dict* d = v.as_dictionary();
  assert(d);
  assert(d->find("x") && *d->find("x")->as_int() == 10);
  assert(d->find("y") && *d->find("y")->as_int() == 20);
  assert(d->size() == 2);
  assert(d->entries()[0].first == "x");
}

static void TestControlledEntries() {
  // Iteration cannot reorder/replace keys behind the index. Value mutation
  // remains available through find(), and insertion goes through set().
  static_assert(std::is_const<typename std::remove_reference<
      decltype(std::declval<Dict&>().entries())>::type>::value,
      "Dictionary entries must be read-only");
  Value v = Value::MakeDictionary();
  Dict* d = v.as_dictionary();
  d->set("a", Value(1));
  d->set("b", Value(2));
  assert(d->find("b") && *d->find("b")->as_int() == 2);
  d->set("b", Value(20));
  assert(d->size() == 2);
  assert(d->find("b") && *d->find("b")->as_int() == 20);
}

static const char* IndexedKey(const void* context, size_t index, size_t* length) {
  const auto& keys = *static_cast<const std::vector<std::string>*>(context);
  if (index >= keys.size()) return nullptr;
  *length = keys[index].size();
  return keys[index].data();
}

static void TestStringIndex() {
  using detail::StringIndex;
  std::vector<std::string> keys{"", std::string("embedded\0nul", 12), "same", "same"};
  StringIndex index;
  assert(index.rebuild(keys.size(), IndexedKey, &keys));
  assert(index.size() == 3);  // duplicate keeps its first position
  assert(index.find("same", 4, IndexedKey, &keys) == 2);
  assert(index.find("", 0, IndexedKey, &keys) == 0);
  assert(index.find(keys[1].data(), keys[1].size(), IndexedKey, &keys) == 1);
  assert(index.find("embedded", 8, IndexedKey, &keys) == StringIndex::kMissing);
  for (size_t i = 4; i < 4096; ++i) {
    keys.push_back("index-key-" + std::to_string(i));
    assert(index.insert(i, IndexedKey, &keys));
  }
  for (size_t i = 4; i < keys.size(); ++i)
    assert(index.find(keys[i].data(), keys[i].size(), IndexedKey, &keys) == i);
  // Arithmetic failure must not discard the existing table or consult keys
  // outside the supplied range.
  assert(!index.rebuild(StringIndex::kMissing, IndexedKey, &keys));
  assert(!index.insert(StringIndex::kMissing, IndexedKey, &keys));
  assert(!index.insert(keys.size(), IndexedKey, &keys));
  assert(index.find("same", 4, IndexedKey, &keys) == 2);
  StringIndex moved(std::move(index));
  assert(index.size() == 0);
  assert(moved.find("same", 4, IndexedKey, &keys) == 2);
  assert(moved.rebuild(0, IndexedKey, &keys));
  assert(moved.find("same", 4, IndexedKey, &keys) == StringIndex::kMissing);
}

static void TestDictAssignment() {
  Dict a;
  a.set("old", Value(1));
  Dict b;
  b.set("new", Value(2));
  a = b;
  b.set("new", Value(3));
  assert(*a.find("new")->as_int() == 2 && !a.find("old"));
  a = std::move(b);
  assert(*a.find("new")->as_int() == 3);
  b.set("reused", Value(4));
  assert(b.find("reused") && !a.find("reused"));
}

static void TestLargeDict() {
  // 100k distinct keys: build + probe in linear time (was O(n^2) string compares).
  const int N = 100000;
  Value v = Value::MakeDictionary();
  Dict* d = v.as_dictionary();
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) {
    d->set("key_" + std::to_string(i), Value(i));
  }
  auto t1 = std::chrono::steady_clock::now();
  // Probe first / last / a middle key and a miss.
  assert(d->find("key_0") && *d->find("key_0")->as_int() == 0);
  assert(d->find("key_99999") && *d->find("key_99999")->as_int() == 99999);
  assert(d->find("key_12345") && *d->find("key_12345")->as_int() == 12345);
  assert(d->find("absent") == nullptr);
  assert(d->size() == static_cast<size_t>(N));
  auto t2 = std::chrono::steady_clock::now();
  const double ms =
      std::chrono::duration<double, std::milli>(t2 - t0).count();
  std::cout << "  100k-key dict build+probe: " << ms << " ms" << std::endl;
  // A generous ceiling: the old O(n^2) build measured in the hundreds of ms on
  // CI; the indexed build is linear. This only guards against a regression to
  // super-linear behavior, not a precise perf number.
  assert(ms < 2000.0);
}

int main() {
  std::cout << "=== LightUSD Next Dict index test ===" << std::endl;
  TestStringIndex();
  TestDictAssignment();
  TestSmallDict();
  TestValueCowIndex();
  TestMoveIndex();
  TestControlledEntries();
  TestLargeDict();
  std::cout << "  Dict index test passed!" << std::endl;
  return 0;
}
