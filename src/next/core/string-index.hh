// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include <cstddef>

namespace lightusd {
namespace next {
namespace detail {

// An index into caller-owned strings. Stores only integer positions, never
// duplicates keys. Key callbacks must return nullptr for an invalid position.
// Insertion does not invalidate keys; callers rebuild after moving/removing keys.
class StringIndex {
 public:
  using Key = const char* (*)(const void* context, size_t index, size_t* length);
  static constexpr size_t kMissing = static_cast<size_t>(-1);
  StringIndex() = default;
  ~StringIndex();
  StringIndex(const StringIndex&) = delete;
  StringIndex& operator=(const StringIndex&) = delete;
  StringIndex(StringIndex&& other) noexcept;
  StringIndex& operator=(StringIndex&& other) noexcept;

  size_t size() const { return count_; }
  size_t find(const char* key, size_t length, Key keys, const void* context) const;
  // Failure leaves the index unchanged. The caller owns error propagation or
  // a correct unindexed fallback. Duplicate keys keep their first position.
  bool insert(size_t index, Key keys, const void* context);
  bool rebuild(size_t count, Key keys, const void* context);

 private:
  size_t* slots_ = nullptr;  // index + 1; zero is empty
  size_t capacity_ = 0;
  size_t count_ = 0;
  bool reserve(size_t count, Key keys, const void* context);
  void insert_reserved(size_t index, Key keys, const void* context);
};

}  // namespace detail
}  // namespace next
}  // namespace lightusd
