// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iosfwd>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include "../c/value.h"

namespace lightusd { namespace next {
// A missing view differs from a present, empty string.
class StringView : public std::string_view {
 public:
  StringView() = default;
  StringView(const char* p, size_t n) : std::string_view(p, n) {}
  StringView(const char* p) : StringView(p, p ? std::char_traits<char>::length(p) : 0) {}
  StringView(const std::string& s) : StringView(s.data(), s.size()) {}
  StringView(std::string_view s) : StringView(s.data(), s.size()) {}
  explicit operator bool() const { return data() != nullptr; }
  operator std::string() const;
  const StringView* operator->() const { return this; }
  const StringView& operator*() const { return *this; }
  const char* c_str() const { return data(); }
  size_t capacity() const { return size(); }
  friend bool operator==(StringView a, std::nullptr_t) { return a.data() == nullptr; }
  friend bool operator==(std::nullptr_t, StringView a) { return a.data() == nullptr; }
  friend bool operator!=(StringView a, std::nullptr_t) { return a.data() != nullptr; }
  friend bool operator!=(std::nullptr_t, StringView a) { return a.data() != nullptr; }
  friend bool operator==(StringView a, StringView b) { return std::string_view(a) == std::string_view(b); }
  friend bool operator!=(StringView a, StringView b) { return !(a == b); }
  friend bool operator==(StringView a, std::string_view b) { return std::string_view(a) == std::string_view(b); }
  friend bool operator==(std::string_view a, StringView b) { return b == a; }
  friend bool operator!=(StringView a, std::string_view b) { return !(a == b); }
  friend bool operator!=(std::string_view a, StringView b) { return !(b == a); }
  friend bool operator==(StringView a, const char* b) { return std::string_view(a) == std::string_view(b); }
  friend bool operator==(const char* a, StringView b) { return b == a; }
  friend bool operator!=(StringView a, const char* b) { return !(a == b); }
  friend bool operator!=(const char* a, StringView b) { return !(b == a); }
  friend bool operator==(StringView a, const std::string& b) { return std::string_view(a) == std::string_view(b); }
  friend bool operator==(const std::string& a, StringView b) { return b == a; }
  friend bool operator!=(StringView a, const std::string& b) { return !(a == b); }
  friend bool operator!=(const std::string& a, StringView b) { return !(b == a); }
  friend std::string operator+(const std::string& a, StringView b) { std::string out(a); out.append(b.data(), b.size()); return out; }
  friend std::string operator+(const char* a, StringView b) { return std::string(a) + b; }
  friend std::string operator+(StringView a, const char* b) { return std::string(a) + b; }
  friend std::string operator+(StringView a, const std::string& b) { return std::string(a) + b; }
};
std::ostream& operator<<(std::ostream&, StringView);

template<class T> class ValueArrayView {
 public:
  ValueArrayView() = default;
  ValueArrayView(std::nullptr_t) {}
  ValueArrayView(T* p, size_t n, bool present = true) : data_(p), size_(present ? n : SIZE_MAX) {}
  template<class R, std::enable_if_t<std::is_convertible<decltype(std::declval<const R&>().data()), T*>::value, int> = 0>
  ValueArrayView(const R& r) : ValueArrayView(r.data(), r.size()) {}
  T* data() const { return data_; }
  template<class R> auto operator==(const R& r) const -> decltype(r.size(), r.begin(), bool()) {
    return size() == r.size() && std::equal(begin(), end(), r.begin());
  }
  template<class R> auto operator!=(const R& r) const -> decltype(r.size(), r.begin(), bool()) { return !(*this == r); }
  bool operator==(std::nullptr_t) const { return size_ == SIZE_MAX; }
  bool operator!=(std::nullptr_t) const { return size_ != SIZE_MAX; }
  size_t size() const { return size_ == SIZE_MAX ? 0 : size_; }
  size_t capacity() const { return size(); }
  bool empty() const { return size() == 0; }
  T* begin() const { return data_; }
  T* end() const { return size() ? data_ + size_ : data_; }
  T& operator[](size_t n) const { return data_[n]; }
  T& front() const { return data_[0]; }
  T& back() const { return data_[size_ - 1]; }
  explicit operator bool() const { return size_ != SIZE_MAX; }
  const ValueArrayView* operator->() const { return this; }
  const ValueArrayView& operator*() const { return *this; }
 private:
  T* data_ = nullptr;
  // No allocation can contain SIZE_MAX elements. Reserve that count for a
  // missing view while keeping present empty spans and two-word returns.
  size_t size_ = SIZE_MAX;
};
class StringArrayView {
 public:
  StringArrayView() = default;
  StringArrayView(std::nullptr_t) {}
  explicit StringArrayView(const lu_value* v) : value_(v) {}
  explicit operator bool() const { return value_ != nullptr; }
  size_t size() const { return value_ ? value_->size : 0; }
  size_t capacity() const { return size(); }
  bool empty() const { return !size(); }
  StringView operator[](size_t i) const { const lu_view s = lu_value_string_at(value_, i); return {s.data, s.len}; }
  const StringArrayView* operator->() const { return this; }
  const StringArrayView& operator*() const { return *this; }
  struct Iterator {
    using iterator_category = std::input_iterator_tag;
    using value_type = StringView;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = StringView;
    const lu_value* value; size_t index;
    StringView operator*() const { const lu_view s = lu_value_string_at(value, index); return {s.data, s.len}; }
    Iterator& operator++() { ++index; return *this; }
    Iterator operator++(int) { Iterator old = *this; ++*this; return old; }
    bool operator==(const Iterator& b) const { return value == b.value && index == b.index; }
    bool operator!=(const Iterator& b) const { return !(*this == b); }
  };
  bool operator==(const StringArrayView& b) const {
    return size() == b.size() && std::equal(begin(), end(), b.begin());
  }
  bool operator!=(const StringArrayView& b) const { return !(*this == b); }
  bool operator==(std::nullptr_t) const { return !value_; }
  bool operator!=(std::nullptr_t) const { return value_ != nullptr; }
  Iterator begin() const { return {value_, 0}; }
  Iterator end() const { return {value_, size()}; }
 private:
  const lu_value* value_ = nullptr;
};
}}  // namespace lightusd::next
