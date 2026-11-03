// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Safe arithmetic operations with overflow checking
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace lightusd {
namespace safe {

template <typename T>
inline bool to_size(T value, size_t* out) {
  static_assert(std::is_integral<T>::value, "to_size requires an integer");
  if (!out) return false;
  using RawT = typename std::remove_cv<T>::type;
  if constexpr (std::is_same<RawT, bool>::value) {
    *out = value ? size_t{1} : size_t{0};
    return true;
  } else {
    if constexpr (std::is_signed<T>::value) {
      if (value < 0) return false;
    }
    using UnsignedT = typename std::make_unsigned<RawT>::type;
    const UnsignedT unsigned_value = static_cast<UnsignedT>(value);
    if constexpr (sizeof(UnsignedT) > sizeof(size_t)) {
      if (unsigned_value > static_cast<UnsignedT>(
                               (std::numeric_limits<size_t>::max)())) {
        return false;
      }
    }
    *out = static_cast<size_t>(unsigned_value);
    return true;
  }
}

///
/// Safe multiplication: a * b -> result (as size_t)
/// Works with any integer types that can be converted to size_t.
/// Returns true and sets *out on success, false on overflow.
///
template <typename A, typename B>
inline bool mul(A a, B b, size_t* out) {
  static_assert(std::is_integral<A>::value && std::is_integral<B>::value,
                "mul requires integral types");
  if (!out) return false;
  size_t sa = 0;
  size_t sb = 0;
  if (!to_size(a, &sa) || !to_size(b, &sb)) return false;
  if ((sb != 0) && (sa > (std::numeric_limits<size_t>::max)() / sb)) {
    return false;  // overflow would occur
  }
  *out = sa * sb;
  return true;
}

///
/// Safe multiplication with three operands: a * b * c -> result (as size_t)
/// Returns true and sets *out on success, false on overflow.
///
template <typename A, typename B, typename C>
inline bool mul3(A a, B b, C c, size_t* out) {
  static_assert(std::is_integral<A>::value && std::is_integral<B>::value && std::is_integral<C>::value,
                "mul3 requires integral types");
  size_t tmp;
  if (!mul(a, b, &tmp)) return false;
  return mul(tmp, c, out);
}

///
/// Safe addition: a + b -> result (as size_t)
/// Returns true and sets *out on success, false on overflow.
///
template <typename A, typename B>
inline bool add(A a, B b, size_t* out) {
  static_assert(std::is_integral<A>::value && std::is_integral<B>::value,
                "add requires integral types");
  if (!out) return false;
  size_t sa = 0;
  size_t sb = 0;
  if (!to_size(a, &sa) || !to_size(b, &sb)) return false;
  if (sa > (std::numeric_limits<size_t>::max)() - sb) {
    return false;  // overflow would occur
  }
  *out = sa + sb;
  return true;
}

template <typename A, typename B>
inline size_t saturating_add(A a, B b) {
  size_t result = 0;
  return add(a, b, &result) ? result
                            : (std::numeric_limits<size_t>::max)();
}

template <typename A, typename B>
inline size_t saturating_mul(A a, B b) {
  size_t result = 0;
  return mul(a, b, &result) ? result
                            : (std::numeric_limits<size_t>::max)();
}

///
/// Safe uint64_t n -> size_t with sizeof(T) multiplication
/// For patterns like: size_t byte_count = sizeof(T) * arr.size();
/// Returns true and sets *out on success, false on overflow.
///
template <typename T>
inline bool n_to_size(uint64_t n, size_t* out) {
  // On 32-bit platforms `n` may not fit in size_t. On 64-bit they are
  // the same width, so the runtime check would be tautological — clang's
  // -Wtautological-type-limit-compare flags it. `if constexpr` does not
  // discard the body at template-definition time, so use the preprocessor.
#if SIZE_MAX < UINT64_MAX
  if (n > (std::numeric_limits<size_t>::max)()) {
    return false;
  }
#endif
  size_t count = static_cast<size_t>(n);
  return mul(count, sizeof(T), out);
}

}  // namespace safe
}  // namespace lightusd
