// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - pointer-sized containers for sparse PrimSpec fields.
//
// A composed stage has ~10M PrimSpecs whose arc lists and legacy variant
// selection are almost always empty; inline std::vector / std::string heads
// cost 24 / 32 bytes each regardless. These hold one pointer (null when
// empty) and keep the subset of the vector / string API the codebase uses,
// plus implicit conversion to the const container for every reader that
// takes a `const std::vector<std::string>&` / `const std::string&`.

#ifndef LIGHTUSD_NEXT_LAYER_LAZY_CONTAINERS_HH_
#define LIGHTUSD_NEXT_LAYER_LAZY_CONTAINERS_HH_

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace next {

/// A std::vector<std::string> that costs one pointer while empty.
class LazyStringList {
 public:
  using vector_type = std::vector<std::string>;
  using value_type = std::string;
  using size_type = vector_type::size_type;
  using iterator = vector_type::iterator;
  using const_iterator = vector_type::const_iterator;
  using reference = std::string &;
  using const_reference = const std::string &;

  LazyStringList() = default;
  LazyStringList(const LazyStringList &o) { assign_from(o.get()); }
  LazyStringList(LazyStringList &&o) noexcept = default;
  LazyStringList(const vector_type &v) { assign_from(v); }  // NOLINT
  LazyStringList(vector_type &&v) { assign_from(std::move(v)); }  // NOLINT
  LazyStringList(std::initializer_list<std::string> il) {
    if (il.size()) v_.reset(new vector_type(il));
  }
  LazyStringList &operator=(const LazyStringList &o) {
    if (this != &o) assign_from(o.get());
    return *this;
  }
  LazyStringList &operator=(LazyStringList &&o) noexcept = default;
  LazyStringList &operator=(const vector_type &v) {
    assign_from(v);
    return *this;
  }
  LazyStringList &operator=(vector_type &&v) {
    assign_from(std::move(v));
    return *this;
  }
  LazyStringList &operator=(std::initializer_list<std::string> il) {
    if (il.size()) mut() = il;
    else v_.reset();
    return *this;
  }

  /// The contents (a shared empty vector while unallocated).
  const vector_type &get() const { return v_ ? *v_ : Empty(); }
  operator const vector_type &() const { return get(); }  // NOLINT
  /// Mutable contents (allocates).
  vector_type &mut() {
    if (!v_) v_.reset(new vector_type());
    return *v_;
  }

  bool empty() const { return !v_ || v_->empty(); }
  size_type size() const { return v_ ? v_->size() : 0; }
  const_iterator begin() const { return get().begin(); }
  const_iterator end() const { return get().end(); }
  const_iterator cbegin() const { return get().begin(); }
  const_iterator cend() const { return get().end(); }
  // Mutable iteration over an unallocated list allocates nothing: both ends
  // are value-initialized and compare equal.
  iterator begin() { return v_ ? v_->begin() : iterator(); }
  iterator end() { return v_ ? v_->end() : iterator(); }
  const std::string &operator[](size_type i) const { return (*v_)[i]; }
  std::string &operator[](size_type i) { return (*v_)[i]; }
  const std::string &front() const { return v_->front(); }
  const std::string &back() const { return v_->back(); }
  std::string &front() { return v_->front(); }
  std::string &back() { return v_->back(); }

  void clear() { v_.reset(); }
  void reserve(size_type n) {
    if (n) mut().reserve(n);
  }
  void resize(size_type n) {
    if (n) mut().resize(n);
    else v_.reset();
  }
  void push_back(const std::string &s) { mut().push_back(s); }
  void push_back(std::string &&s) { mut().push_back(std::move(s)); }
  template <typename... Args>
  std::string &emplace_back(Args &&...args) {
    return mut().emplace_back(std::forward<Args>(args)...);
  }
  template <typename... Args>
  iterator insert(const_iterator pos, Args &&...args) {
    if (!v_) {
      v_.reset(new vector_type());
      return v_->insert(v_->begin(), std::forward<Args>(args)...);
    }
    return v_->insert(pos, std::forward<Args>(args)...);
  }
  iterator erase(const_iterator pos) { return v_->erase(pos); }
  iterator erase(const_iterator first, const_iterator last) {
    if (!v_) return iterator();
    return v_->erase(first, last);
  }
  template <typename It>
  void assign(It first, It last) {
    if (first == last) v_.reset();
    else mut().assign(first, last);
  }
  void swap(LazyStringList &o) noexcept { v_.swap(o.v_); }
  void swap(vector_type &o) {
    mut().swap(o);
    if (v_->empty()) v_.reset();
  }

  friend bool operator==(const LazyStringList &a, const LazyStringList &b) {
    return a.get() == b.get();
  }
  friend bool operator!=(const LazyStringList &a, const LazyStringList &b) {
    return !(a == b);
  }
  friend bool operator==(const LazyStringList &a, const vector_type &b) {
    return a.get() == b;
  }
  friend bool operator==(const vector_type &a, const LazyStringList &b) {
    return a == b.get();
  }
  friend bool operator!=(const LazyStringList &a, const vector_type &b) {
    return a.get() != b;
  }
  friend bool operator!=(const vector_type &a, const LazyStringList &b) {
    return a != b.get();
  }

 private:
  static const vector_type &Empty() {
    static const vector_type kEmpty;
    return kEmpty;
  }
  template <typename V>
  void assign_from(V &&v) {
    if (v.empty()) v_.reset();
    else if (v_) *v_ = std::forward<V>(v);
    else v_.reset(new vector_type(std::forward<V>(v)));
  }

  std::unique_ptr<vector_type> v_;
};

/// A std::string that costs one pointer while empty.
class LazyString {
 public:
  using size_type = std::string::size_type;
  static constexpr size_type npos = std::string::npos;

  LazyString() = default;
  LazyString(const LazyString &o) { assign_from(o.get()); }
  LazyString(LazyString &&o) noexcept = default;
  LazyString(const std::string &s) { assign_from(s); }  // NOLINT
  LazyString(std::string &&s) { assign_from(std::move(s)); }  // NOLINT
  LazyString(const char *s) { assign_from(std::string(s)); }  // NOLINT
  LazyString &operator=(const LazyString &o) {
    if (this != &o) assign_from(o.get());
    return *this;
  }
  LazyString &operator=(LazyString &&o) noexcept = default;
  LazyString &operator=(const std::string &s) {
    assign_from(s);
    return *this;
  }
  LazyString &operator=(std::string &&s) {
    assign_from(std::move(s));
    return *this;
  }
  LazyString &operator=(const char *s) {
    assign_from(std::string(s));
    return *this;
  }

  const std::string &get() const { return s_ ? *s_ : Empty(); }
  operator const std::string &() const { return get(); }  // NOLINT
  std::string &mut() {
    if (!s_) s_.reset(new std::string());
    return *s_;
  }

  bool empty() const { return !s_ || s_->empty(); }
  size_type size() const { return s_ ? s_->size() : 0; }
  size_type length() const { return size(); }
  const char *c_str() const { return get().c_str(); }
  const char *data() const { return get().data(); }
  void clear() { s_.reset(); }
  size_type find(char c, size_type pos = 0) const { return get().find(c, pos); }
  size_type find(const std::string &t, size_type pos = 0) const {
    return get().find(t, pos);
  }
  std::string substr(size_type pos = 0, size_type n = npos) const {
    return get().substr(pos, n);
  }
  int compare(const std::string &t) const { return get().compare(t); }

  friend bool operator==(const LazyString &a, const LazyString &b) {
    return a.get() == b.get();
  }
  friend bool operator!=(const LazyString &a, const LazyString &b) {
    return !(a == b);
  }
  friend bool operator==(const LazyString &a, const std::string &b) {
    return a.get() == b;
  }
  friend bool operator==(const std::string &a, const LazyString &b) {
    return a == b.get();
  }
  friend bool operator!=(const LazyString &a, const std::string &b) {
    return a.get() != b;
  }
  friend bool operator!=(const std::string &a, const LazyString &b) {
    return a != b.get();
  }
  friend bool operator==(const LazyString &a, const char *b) {
    return a.get() == b;
  }
  friend bool operator!=(const LazyString &a, const char *b) {
    return a.get() != b;
  }
  friend std::string operator+(const LazyString &a, const std::string &b) {
    return a.get() + b;
  }
  friend std::string operator+(const std::string &a, const LazyString &b) {
    return a + b.get();
  }
  friend std::string operator+(const char *a, const LazyString &b) {
    return a + b.get();
  }
  friend std::string operator+(const LazyString &a, const char *b) {
    return a.get() + b;
  }

 private:
  static const std::string &Empty() {
    static const std::string kEmpty;
    return kEmpty;
  }
  template <typename S>
  void assign_from(S &&s) {
    if (s.empty()) s_.reset();
    else if (s_) *s_ = std::forward<S>(s);
    else s_.reset(new std::string(std::forward<S>(s)));
  }

  std::unique_ptr<std::string> s_;
};

}  // namespace next
}  // namespace lightusd

#endif  // LIGHTUSD_NEXT_LAYER_LAZY_CONTAINERS_HH_
