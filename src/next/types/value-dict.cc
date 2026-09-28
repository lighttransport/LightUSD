// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "value.hh"
#include <limits>
#include <utility>

namespace lightusd {
namespace next {
namespace {
const char* DictKey(const void* context, size_t index, size_t* length) {
  const auto& entries_ = static_cast<const Dict*>(context)->entries();
  if (index >= entries_.size()) return nullptr;
  const std::string& key = entries_[index].first;
  *length = key.size();
  return key.data();
}
}  // namespace

Dict::Dict() = default;
Dict::~Dict() = default;
Dict::Dict(Dict&& other) noexcept = default;
Dict& Dict::operator=(Dict&& other) noexcept = default;
Dict::Dict(const Dict& other)
    : entries_(other.entries_), typed_json_wrapper_(other.typed_json_wrapper_) {
  index_.rebuild(entries_.size(), DictKey, this);
}
Dict& Dict::operator=(const Dict& other) {
  if (this != &other) {
    Dict copy(other);
    *this = std::move(copy);
  }
  return *this;
}

const Value* Dict::find(const std::string& key) const {
  const size_t index = index_.find(key.data(), key.size(), DictKey, this);
  if (index != detail::StringIndex::kMissing) return &entries_[index].second;
  if (index_.size() != entries_.size()) {
    for (const auto& entry : entries_) {
      if (entry.first == key) return &entry.second;
    }
  }
  return nullptr;
}

Value* Dict::find(const std::string& key) {
  return const_cast<Value*>(static_cast<const Dict*>(this)->find(key));
}

void Dict::set(std::string key, Value v) {
  const size_t index = index_.find(key.data(), key.size(), DictKey, this);
  if (index != detail::StringIndex::kMissing) {
    entries_[index].second = std::move(v);
    return;
  }
  const bool indexed = index_.size() == entries_.size();
  if (!indexed) {
    for (auto& entry : entries_) {
      if (entry.first == key) {
        entry.second = std::move(v);
        index_.rebuild(entries_.size(), DictKey, this);
        return;
      }
    }
  }
  entries_.emplace_back(std::move(key), std::move(v));
  if (indexed) index_.insert(entries_.size() - 1, DictKey, this);
  else index_.rebuild(entries_.size(), DictKey, this);
}

size_t Dict::dynamic_string_memory_usage() const {
  size_t bytes = entries_.capacity() &&
          sizeof(std::pair<std::string, Value>) >
              (std::numeric_limits<size_t>::max)() / entries_.capacity()
      ? (std::numeric_limits<size_t>::max)()
      : entries_.capacity() * sizeof(std::pair<std::string, Value>);
  const auto add = [&bytes](size_t n) {
    bytes = n > (std::numeric_limits<size_t>::max)() - bytes
        ? (std::numeric_limits<size_t>::max)() : bytes + n;
  };
  for (const auto& entry : entries_) {
    add(entry.first.capacity());
    add(entry.second.dynamic_string_memory_usage());
  }
  return bytes;
}

}  // namespace next
}  // namespace lightusd
