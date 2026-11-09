// SPDX-License-Identifier: Apache-2.0
#include "string-index.hh"
#include <utility>
namespace lightusd { namespace next { namespace detail {
StringIndex::~StringIndex() { lu_string_index_destroy(&index_); }
StringIndex::StringIndex(StringIndex&& other) noexcept { std::swap(index_, other.index_); }
StringIndex& StringIndex::operator=(StringIndex&& other) noexcept {
  if (this != &other) { lu_string_index_destroy(&index_); std::swap(index_, other.index_); }
  return *this;
}
size_t StringIndex::find(const char* key, size_t length, Key keys, const void* context) const {
  return lu_string_index_find(&index_, key, length, keys, context);
}
bool StringIndex::insert(size_t index, Key keys, const void* context) {
  return lu_string_index_insert(&index_, index, keys, context) != 0;
}
bool StringIndex::rebuild(size_t count, Key keys, const void* context) {
  return lu_string_index_rebuild(&index_, count, keys, context) != 0;
}
} } }
