// A double-to-string conversion algorithm based on Schubfach.
// Copyright (c) 2025 - present, Victor Zverovich
// Distributed under the MIT license (see LICENSE) or alternatively
// the Boost Software License, Version 1.0.

#ifndef ZMIJ_H_
#define ZMIJ_H_

#include <stddef.h>  // size_t
#include <string.h>  // memcpy

namespace zmij {
namespace detail {
template <typename Float>
auto write(Float value, char* buffer) noexcept -> char*;
// LOCAL ADDITION (lightusd): usdcat fixed-notation fast path;
// returns nullptr when the caller must use a scalar usdcat fallback.
template <typename Float>
auto write_usd_fast(Float value, char* buffer) noexcept -> char*;
}  // namespace detail

/// LOCAL ADDITION (lightusd): write `value` in OpenUSD/usdcat
/// notation via zmij's fixed-notation SIMD block. Returns a pointer past the
/// last char on success, or nullptr if `value` needs the caller's scalar
/// usdcat fallback (special/subnormal, or an out-of-fixed-window exponent).
/// `out` must have >= 40 bytes (the SIMD block writes 16-byte chunks).
inline auto write_usd_fast(char* out, float value) noexcept -> char* {
  return detail::write_usd_fast(value, out);
}
inline auto write_usd_fast(char* out, double value) noexcept -> char* {
  return detail::write_usd_fast(value, out);
}

enum {
  non_finite_exp = int(~0u >> 1),
};

// A decimal floating-point number sig * pow(10, exp).
// If exp is non_finite_exp then the number is a NaN or an infinity.
struct dec_fp {
  long long sig;  // significand
  int exp;        // exponent
  bool negative;
};

/// Converts `value` into the shortest correctly rounded decimal representation.
/// Usage:
///   auto [sig, exp, negative] = to_decimal(6.62607015e-34);
auto to_decimal(double value) noexcept -> dec_fp;

/// LOCAL ADDITION (lightusd): float overload — not upstream zmij.
auto to_decimal(float value) noexcept -> dec_fp;

enum {
  float_buffer_size = 17,
  double_buffer_size = 34,
};

/// Writes the shortest correctly rounded decimal representation of `value` to
/// `out` without a null terminator. Returns a pointer past the last character
/// written; if the representation exceeds `n` characters, only the first `n`
/// are written.
inline auto write(char* out, size_t n, float value) noexcept -> char* {
  if (n >= float_buffer_size) return detail::write(value, out);
  char buffer[float_buffer_size];
  size_t size = detail::write(value, buffer) - buffer;
  if (size > n) size = n;
  memcpy(out, buffer, size);
  return out + size;
}

/// Writes the shortest correctly rounded decimal representation of `value` to
/// `out` without a null terminator. Returns a pointer past the last character
/// written; if the representation exceeds `n` characters, only the first `n`
/// are written.
inline auto write(char* out, size_t n, double value) noexcept -> char* {
  if (n >= double_buffer_size) return detail::write(value, out);
  char buffer[double_buffer_size];
  size_t size = detail::write(value, buffer) - buffer;
  if (size > n) size = n;
  memcpy(out, buffer, size);
  return out + size;
}

}  // namespace zmij

#endif  // ZMIJ_H_
