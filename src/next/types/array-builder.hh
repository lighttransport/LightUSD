// SPDX-License-Identifier: Apache-2.0
#pragma once
// Private parser/decoder builder. Allocation and publication use C11 storage;
// the shared adapter keeps growth and lifetime code out of typed callers.
#include "value.hh"
#include <vector>
namespace lightusd { namespace next { namespace detail {
class ArrayBuilderStorage {
 public:
  ArrayBuilderStorage(size_t width, size_t alignment);
  ~ArrayBuilderStorage();
  ArrayBuilderStorage(const ArrayBuilderStorage&) = delete;
  ArrayBuilderStorage& operator=(const ArrayBuilderStorage&) = delete;
  ArrayBuilderStorage(ArrayBuilderStorage&&) noexcept;
  ArrayBuilderStorage& operator=(ArrayBuilderStorage&&) noexcept;
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  void clear() { size_ = 0; failed_ = false; }
  bool failed() const { return failed_; }
  bool reserve(size_t n);
  bool resize(size_t n);
  Value take(TypeId type);
 protected:
  void append(const void* data, size_t n);
  lu_buffer* buffer_ = nullptr;
  void* data_ = nullptr;
  size_t size_ = 0, capacity_ = 0;
  bool failed_ = false;
 private:
  size_t width_, alignment_;
};
template<class T> class ArrayBuilder : public ArrayBuilderStorage {
 public:
  ArrayBuilder() : ArrayBuilderStorage(sizeof(T), alignof(T)) {}
  explicit ArrayBuilder(size_t n) : ArrayBuilder() { resize(n); }
  T* data() { return static_cast<T*>(data_); }
  const T* data() const { return static_cast<const T*>(data_); }
  T* begin() { return data(); }
  T* end() { return size_ ? data() + size_ : data(); }
  const T* begin() const { return data(); }
  const T* end() const { return size_ ? data() + size_ : data(); }
  T& operator[](size_t i) { return data()[i]; }
  const T& operator[](size_t i) const { return data()[i]; }
  void push_back(T value) {
    if (failed_ || (size_ == capacity_ && !reserve(capacity_ ? capacity_ + capacity_ / 2 + 1 : 8))) return;
    data()[size_++] = value;
  }
  void insert(T*, const T* first, const T* last) {
    append(first, first == last ? 0 : static_cast<size_t>(last - first));
  }
};
template<> class ArrayBuilder<std::string> : public std::vector<std::string> {};
}}} // namespace lightusd::next::detail
