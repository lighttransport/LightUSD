// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - minimal expected<T, E>.
//
// The next product needs only a narrow slice of std::expected: construct from a
// value or from make_unexpected(e), test with operator bool, dereference with
// operator*, and read value()/error(). The vendored nonstd/expected.hpp is
// 3,494 lines and is pulled into 14 translation units through the pcp/validation
// headers; this 180-line subset replaces it there so those units stop paying for
// the full implementation.
//
// Move-only value types (Cache, Stage) are supported; no allocation, no
// exceptions, and member functions are only instantiated when used, so the
// copy constructor does not require a copyable T unless it is actually copied.
//
#pragma once

#include <new>
#include <utility>

namespace lightusd {
namespace next {

template <class E>
class unexpected {
 public:
  unexpected() = delete;
  explicit unexpected(const E& e) : err_(e) {}
  explicit unexpected(E&& e) : err_(std::move(e)) {}

  const E& value() const& { return err_; }
  E& value() & { return err_; }
  E&& value() && { return std::move(err_); }

 private:
  E err_;
};

template <class E>
unexpected<E> make_unexpected(E e) {
  return unexpected<E>(std::move(e));
}

template <class T, class E>
class expected {
 public:
  expected(const T& v) : has_value_(true) { new (&storage_.value) T(v); }
  expected(T&& v) : has_value_(true) { new (&storage_.value) T(std::move(v)); }
  expected(const unexpected<E>& u) : has_value_(false) {
    new (&storage_.error) E(u.value());
  }
  expected(unexpected<E>&& u) : has_value_(false) {
    new (&storage_.error) E(std::move(u).value());
  }

  expected(const expected& other) : has_value_(other.has_value_) {
    if (has_value_) {
      new (&storage_.value) T(other.storage_.value);
    } else {
      new (&storage_.error) E(other.storage_.error);
    }
  }
  expected(expected&& other) noexcept : has_value_(other.has_value_) {
    if (has_value_) {
      new (&storage_.value) T(std::move(other.storage_.value));
    } else {
      new (&storage_.error) E(std::move(other.storage_.error));
    }
  }

  expected& operator=(const expected& other) {
    if (this != &other) {
      destroy();
      has_value_ = other.has_value_;
      if (has_value_) {
        new (&storage_.value) T(other.storage_.value);
      } else {
        new (&storage_.error) E(other.storage_.error);
      }
    }
    return *this;
  }
  expected& operator=(expected&& other) noexcept {
    if (this != &other) {
      destroy();
      has_value_ = other.has_value_;
      if (has_value_) {
        new (&storage_.value) T(std::move(other.storage_.value));
      } else {
        new (&storage_.error) E(std::move(other.storage_.error));
      }
    }
    return *this;
  }

  ~expected() { destroy(); }

  explicit operator bool() const noexcept { return has_value_; }
  bool has_value() const noexcept { return has_value_; }

  T& value() & {
    return storage_.value;
  }
  const T& value() const& { return storage_.value; }
  T&& value() && { return std::move(storage_.value); }

  E& error() & { return storage_.error; }
  const E& error() const& { return storage_.error; }

  T& operator*() & { return storage_.value; }
  const T& operator*() const& { return storage_.value; }
  T&& operator*() && { return std::move(storage_.value); }

  T* operator->() { return &storage_.value; }
  const T* operator->() const { return &storage_.value; }

  template <class U>
  T value_or(U&& fallback) const {
    return has_value_ ? storage_.value : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  void destroy() {
    if (has_value_) {
      storage_.value.~T();
    } else {
      storage_.error.~E();
    }
  }

  union Storage {
    Storage() {}
    ~Storage() {}
    T value;
    E error;
  } storage_;
  bool has_value_;
};

}  // namespace next
}  // namespace lightusd
