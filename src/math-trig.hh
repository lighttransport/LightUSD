// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Light Transport Entertainment Inc.
#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

namespace lightusd {
namespace math {
template <class T>
struct constants {
  static constexpr T pi() {
    static_assert(std::is_floating_point<T>::value, "Floating point required");
    return T(3.141592653589793238462643383279502884L);
  }
};

// Reduce before multiplying by pi. Exact quarter turns share the same
// cosine evaluation, keeping sine and cosine bit-identical at 45 degrees.
// Floating-point remainder also avoids integer conversion overflow for large
// finite angles. Non-finite inputs return NaN without invoking remainder.
template <class T>
inline T sin_pi(T x) {
  static_assert(std::is_floating_point<T>::value, "Floating point required");
  if (!std::isfinite(x)) return std::numeric_limits<T>::quiet_NaN();
  T r = std::remainder(x, T(2));
  const bool negative = std::signbit(r);
  r = std::fabs(r);
  if (r > T(0.5)) r = T(1) - r;
  const T value = r == T(0.5)    ? T(1)
                  : r == T(0.25) ? std::cos(constants<T>::pi() * r)
                                 : std::sin(constants<T>::pi() * r);
  return negative ? -value : value;
}
template <class T>
inline T cos_pi(T x) {
  static_assert(std::is_floating_point<T>::value, "Floating point required");
  if (!std::isfinite(x)) return std::numeric_limits<T>::quiet_NaN();
  T r = std::fabs(std::remainder(x, T(2)));
  const bool negative = r > T(0.5);
  if (negative) r = T(1) - r;
  if (r == T(0.5)) return T(0);
  const T value = r > T(0.25) ? std::sin(constants<T>::pi() * (T(0.5) - r))
                              : std::cos(constants<T>::pi() * r);
  return negative ? -value : value;
}
}  // namespace math
}  // namespace lightusd
