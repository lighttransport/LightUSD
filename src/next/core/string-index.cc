// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "string-index.hh"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace lightusd {
namespace next {
namespace detail {
namespace {
size_t Hash(const char* key, size_t length) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < length; ++i)
    hash = (hash ^ static_cast<uint8_t>(key[i])) * 1099511628211ULL;
  return static_cast<size_t>(hash);
}
bool Equal(const char* key, size_t length, size_t index,
           StringIndex::Key keys, const void* context) {
  size_t candidate_length = 0;
  const char* candidate = keys(context, index, &candidate_length);
  return candidate && length == candidate_length &&
         (length == 0 || std::memcmp(key, candidate, length) == 0);
}
}  // namespace

StringIndex::~StringIndex() { std::free(slots_); }
StringIndex::StringIndex(StringIndex&& other) noexcept { *this = std::move(other); }
StringIndex& StringIndex::operator=(StringIndex&& other) noexcept {
  if (this != &other) {
    std::free(slots_);
    slots_ = other.slots_;
    capacity_ = other.capacity_;
    count_ = other.count_;
    other.slots_ = nullptr;
    other.capacity_ = other.count_ = 0;
  }
  return *this;
}

size_t StringIndex::find(const char* key, size_t length, Key keys,
                         const void* context) const {
  if (!key || !keys || !capacity_) return kMissing;
  size_t slot = Hash(key, length) & (capacity_ - 1);
  for (size_t probe = 0; probe < capacity_; ++probe) {
    if (!slots_[slot]) return kMissing;
    const size_t index = slots_[slot] - 1;
    if (Equal(key, length, index, keys, context)) return index;
    slot = (slot + 1) & (capacity_ - 1);
  }
  return kMissing;
}

void StringIndex::insert_reserved(size_t index, Key keys, const void* context) {
  size_t length = 0;
  const char* key = keys(context, index, &length);
  if (!key) return;
  size_t slot = Hash(key, length) & (capacity_ - 1);
  while (slots_[slot]) {
    if (Equal(key, length, slots_[slot] - 1, keys, context)) return;
    slot = (slot + 1) & (capacity_ - 1);
  }
  slots_[slot] = index + 1;
  ++count_;
}

bool StringIndex::reserve(size_t count, Key keys, const void* context) {
  if (count <= capacity_ / 2) return true;
  size_t capacity = capacity_ ? capacity_ : 8;
  while (count > capacity / 2) {
    if (capacity > kMissing / 2) return false;
    capacity *= 2;
  }
  if (capacity > kMissing / sizeof(size_t)) return false;
  StringIndex grown;
  grown.slots_ = static_cast<size_t*>(std::calloc(capacity, sizeof(size_t)));
  if (!grown.slots_) return false;
  grown.capacity_ = capacity;
  for (size_t i = 0; i < capacity_; ++i) {
    if (slots_[i]) grown.insert_reserved(slots_[i] - 1, keys, context);
  }
  *this = std::move(grown);
  return true;
}

bool StringIndex::insert(size_t index, Key keys, const void* context) {
  if (index == kMissing || !keys) return false;
  size_t length = 0;
  if (!keys(context, index, &length)) return false;
  if (count_ == kMissing || !reserve(count_ + 1, keys, context)) return false;
  insert_reserved(index, keys, context);
  return true;
}

bool StringIndex::rebuild(size_t count, Key keys, const void* context) {
  if (!keys) return false;
  StringIndex rebuilt;
  if (!rebuilt.reserve(count, keys, context)) return false;
  for (size_t i = 0; i < count; ++i) {
    size_t length = 0;
    if (!keys(context, i, &length)) return false;
    rebuilt.insert_reserved(i, keys, context);
  }
  *this = std::move(rebuilt);
  return true;
}

}  // namespace detail
}  // namespace next
}  // namespace lightusd
