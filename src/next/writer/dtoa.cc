// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - dragonbox-based float-to-string (dtoa)
//
// Ported from src/str-util.cc `dtos` (Dragonbox algorithm by Junekey Jeon),
// hardcoded to OpenUSD/usdcat notation. Only `jkj::dragonbox::to_decimal` is
// used (header-only in dragonbox.h); the digit layout / exponent formatting is
// ours, mirroring pxr_double_conversion ToShortest/ToShortestSingle.
//
// Hot path: a vendored zmij (src/external/zmij) `write_usd_fast` renders the
// common fixed-notation window directly (SSE4.1/NEON shuffle when available,
// portable scalar code otherwise, e.g. WASM); everything else (special values,
// subnormals, scientific / out-of-window exponents) falls back to the dragonbox
// renderer below. The two are byte-identical for every input (see
// tests/next/test_dtoa.cc). Define LIGHTUSD_NEXT_NO_ZMIJ_DTOA to build the
// dragonbox-only formatter.

#include "dtoa.hh"
#include "../crate/crate-format.hh"
#include "../../external/fast_float/include/fast_float/fast_float.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#endif

#include "../../external/dragonbox/dragonbox.h"
#if !defined(LIGHTUSD_NEXT_NO_ZMIJ_DTOA)
#include "../../external/zmij/zmij.h"
#endif

// GCC's optimizer mis-analyses the inlined two-digit writes below and reports a
// bogus out-of-bounds (offset ~2^32 into a 32-byte buffer). The buffer is always
// large enough for a shortest float/double representation. Suppress on GCC only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow="
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

namespace lightusd {
namespace next {

namespace {

template <typename T>
inline int count_digits(T n) {
  int count = 1;
  for (;;) {
    if (n < 10) return count;
    if (n < 100) return count + 1;
    if (n < 1000) return count + 2;
    if (n < 10000) return count + 3;
    n /= 10000u;
    count += 4;
  }
}

inline void write2digits(char* out, size_t value) {
  *out++ = static_cast<char>('0' + value / 10);
  *out = static_cast<char>('0' + value % 10);
}

// OpenUSD exponent: leading '-' only (no '+'), minimal digits, no zero padding.
char* write_exponent(int exp, char* out) {
  if (exp < 0) {
    *out++ = '-';
    exp = -exp;
  }
  auto uexp = static_cast<uint32_t>(exp);
  char tmp[8];
  int n = 0;
  if (uexp == 0) tmp[n++] = '0';
  while (uexp) {
    tmp[n++] = static_cast<char>('0' + (uexp % 10u));
    uexp /= 10u;
  }
  for (int i = n - 1; i >= 0; --i) *out++ = tmp[i];
  return out;
}

inline char* fill_n(char* p, int n, char c) {
  for (int i = 0; i < n; i++, p++) *p = c;
  return p;
}

inline void format_decimal_impl(char* out, uint64_t value, uint32_t size) {
  unsigned n = size;
  while (value >= 100) {
    n -= 2;
    write2digits(out + n, static_cast<unsigned>(value % 100));
    value /= 100;
  }
  if (value >= 10) {
    n -= 2;
    write2digits(out + n, static_cast<unsigned>(value));
  } else {
    out[--n] = static_cast<char>('0' + value);
  }
}

inline char* format_decimal(char* out, uint64_t value, uint32_t num_digits) {
  format_decimal_impl(out, value, num_digits);
  return out + num_digits;
}

inline char* write_significand_e(char* out, uint64_t significand,
                                 int significand_size, int exponent) {
  out = format_decimal(out, significand, static_cast<uint32_t>(significand_size));
  return fill_n(out, exponent, '0');
}

inline char* write_significand(char* out, uint64_t significand,
                               int significand_size, int integral_size,
                               char decimal_point) {
  if (!decimal_point)
    return format_decimal(out, significand, static_cast<uint32_t>(significand_size));
  out += significand_size + 1;
  char* end = out;
  int floating_size = significand_size - integral_size;
  for (int i = floating_size / 2; i > 0; --i) {
    out -= 2;
    write2digits(out, static_cast<std::size_t>(significand % 100));
    significand /= 100;
  }
  if (floating_size % 2 != 0) {
    *--out = static_cast<char>('0' + significand % 10);
    significand /= 10;
  }
  *--out = decimal_point;
  format_decimal(out - integral_size, significand,
                 static_cast<uint32_t>(integral_size));
  return end;
}

// max_digits: 9 (float) / 17 (double). OpenUSD notation: fixed for decimal
// exponents in [-6, 15), scientific outside; `-0` preserved.
template <typename Float>
char* dtoa_impl_t(const Float f, char* buf, int max_digits) {
  if (std::isnan(f)) {
    std::memcpy(buf, "nan", 3);
    return buf + 3;
  }
  if (std::isinf(f)) {
    if (std::signbit(f)) {
      std::memcpy(buf, "-inf", 4);
      return buf + 4;
    }
    std::memcpy(buf, "inf", 3);
    return buf + 3;
  }

  bool is_negative = std::signbit(f);

  if (std::fpclassify(f) == FP_ZERO) {
    if (is_negative) *buf++ = '-';  // OpenUSD preserves `-0`.
    *buf++ = '0';
    return buf;
  }

  auto ret = jkj::dragonbox::to_decimal(f);

  const int exp_lower = -6;
  const int exp_upper = 15;

  auto significand = ret.significand;
  int significand_size = count_digits(significand);
  int exponent = ret.exponent;

  if (significand_size > max_digits) {
    int digits_to_remove = significand_size - max_digits;
    uint64_t divisor = 1;
    for (int i = 0; i < digits_to_remove; i++) divisor *= 10;
    uint64_t remainder = significand % divisor;
    significand /= divisor;
    exponent += digits_to_remove;
    uint64_t half = divisor / 2;
    if (remainder > half || (remainder == half && (significand & 1))) {
      significand++;
      if (count_digits(significand) > max_digits) {
        significand /= 10;
        exponent++;
      }
    }
    significand_size = count_digits(significand);
  }

  int output_exp = exponent + significand_size - 1;
  bool use_exp_format = (output_exp < exp_lower) || (output_exp >= exp_upper);

  char decimal_point = '.';
  if (use_exp_format) {
    if (significand_size == 1) decimal_point = '\0';
    if (is_negative) *buf++ = '-';
    buf = write_significand(buf, significand, significand_size, 1, decimal_point);
    *buf++ = 'e';
    return write_exponent(output_exp, buf);
  }

  int exp = exponent + significand_size;
  if (exponent >= 0) {
    if (is_negative) *buf++ = '-';
    return write_significand_e(buf, significand, significand_size, exponent);
  } else if (exp > 0) {
    if (is_negative) *buf++ = '-';
    return write_significand(buf, significand, significand_size, exp,
                             decimal_point);
  }

  int num_zeros = -exp;
  bool pointy = num_zeros != 0 || significand_size != 0;
  if (is_negative) *buf++ = '-';
  *buf++ = '0';
  if (!pointy) return buf;
  *buf++ = decimal_point;
  buf = fill_n(buf, num_zeros, '0');
  return format_decimal(buf, significand, static_cast<uint32_t>(significand_size));
}

// Dragonbox-only renderer (the reference; also the fallback of dtoa_impl).
char* dtoa_ref_impl(const double f, char* buf) {
  uint64_t bits;
  std::memcpy(&bits, &f, sizeof(double));
  if (bits == 0x3FF0000000000000ULL) { *buf++ = '1'; return buf; }
  if (bits == 0xBFF0000000000000ULL) { *buf++ = '-'; *buf++ = '1'; return buf; }
  return dtoa_impl_t(f, buf, /*max_digits=*/17);
}

// printf %g-style exponent: 'e', a sign always, at least two digits.
char* write_exponent_g(int exp, char* out) {
  *out++ = (exp < 0) ? '-' : '+';
  if (exp < 0) exp = -exp;
  auto uexp = static_cast<uint32_t>(exp);
  char tmp[8];
  int n = 0;
  while (uexp) { tmp[n++] = static_cast<char>('0' + (uexp % 10u)); uexp /= 10u; }
  while (n < 2) tmp[n++] = '0';
  for (int i = n - 1; i >= 0; --i) *out++ = tmp[i];
  return out;
}

// Freestanding C `printf("%.*g", precision, f)` formatter: round to `precision`
// significant digits, choose fixed vs scientific by the %g rule (fixed iff
// -4 <= decimal-exponent < precision), strip trailing zeros, scientific uses
// e±dd. Built on the same dragonbox shortest digits + round-to-N-digits the
// shortest dtoa uses. No libc / no locale.
char* dtoa_g_impl(double f, char* buf, int precision) {
  if (precision < 1) precision = 1;
  // A double carries at most 17 significant decimal digits; clamp so neither the
  // round-to-N loop nor the caller's fixed-size buffer can be driven past that
  // (a large `precision` in fixed notation would otherwise print precision-plus
  // digits and overflow the buffer).
  if (precision > 17) precision = 17;
  if (std::isnan(f)) { std::memcpy(buf, "nan", 3); return buf + 3; }
  if (std::isinf(f)) {
    if (std::signbit(f)) { std::memcpy(buf, "-inf", 4); return buf + 4; }
    std::memcpy(buf, "inf", 3); return buf + 3;
  }
  const bool is_negative = std::signbit(f);
  if (std::fpclassify(f) == FP_ZERO) {
    if (is_negative) *buf++ = '-';
    *buf++ = '0';
    return buf;
  }

  auto ret = jkj::dragonbox::to_decimal(f);
  uint64_t significand = ret.significand;
  int significand_size = count_digits(significand);
  int exponent = ret.exponent;

  // Round the shortest significand to `precision` significant digits.
  if (significand_size > precision) {
    int rm = significand_size - precision;
    uint64_t divisor = 1;
    for (int i = 0; i < rm; i++) divisor *= 10;
    uint64_t remainder = significand % divisor;
    significand /= divisor;
    exponent += rm;
    uint64_t half = divisor / 2;
    if (remainder > half || (remainder == half && (significand & 1))) {
      significand++;
      if (count_digits(significand) > precision) { significand /= 10; exponent++; }
    }
    significand_size = count_digits(significand);
  }
  // %g strips trailing zeros (rounding up can introduce them, e.g. 999..->1000..).
  while (significand_size > 1 && (significand % 10u) == 0) {
    significand /= 10u; ++exponent; --significand_size;
  }

  const int output_exp = exponent + significand_size - 1;
  const bool use_exp = (output_exp < -4) || (output_exp >= precision);

  char decimal_point = '.';
  if (use_exp) {
    if (significand_size == 1) decimal_point = '\0';
    if (is_negative) *buf++ = '-';
    buf = write_significand(buf, significand, significand_size, 1, decimal_point);
    *buf++ = 'e';
    return write_exponent_g(output_exp, buf);
  }
  const int exp = exponent + significand_size;
  if (exponent >= 0) {
    if (is_negative) *buf++ = '-';
    return write_significand_e(buf, significand, significand_size, exponent);
  } else if (exp > 0) {
    if (is_negative) *buf++ = '-';
    return write_significand(buf, significand, significand_size, exp, decimal_point);
  }
  const int num_zeros = -exp;
  if (is_negative) *buf++ = '-';
  *buf++ = '0';
  *buf++ = decimal_point;
  buf = fill_n(buf, num_zeros, '0');
  return format_decimal(buf, significand, static_cast<uint32_t>(significand_size));
}

char* dtoa_ref_impl(const float f, char* buf) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(float));
  if (bits == 0x3F800000U) { *buf++ = '1'; return buf; }
  if (bits == 0xBF800000U) { *buf++ = '-'; *buf++ = '1'; return buf; }
  return dtoa_impl_t(f, buf, /*max_digits=*/9);
}

// Fast path: zmij's fixed-notation block emits usdcat notation directly for the
// common leading-exponent window (~all authored coords/normals/uvs); it returns
// nullptr for special / subnormal / scientific / out-of-window values, which go
// to the dragonbox renderer. REQUIRES buf capacity >= kDtoaBufSize.
char* dtoa_impl(const double f, char* buf) {
#if !defined(LIGHTUSD_NEXT_NO_ZMIJ_DTOA)
  if (char* e = zmij::write_usd_fast(buf, f)) return e;
#endif
  return dtoa_ref_impl(f, buf);
}

char* dtoa_impl(const float f, char* buf) {
#if !defined(LIGHTUSD_NEXT_NO_ZMIJ_DTOA)
  if (char* e = zmij::write_usd_fast(buf, f)) return e;
#endif
  return dtoa_ref_impl(f, buf);
}

}  // namespace

std::string dtos(float v) {
  char buffer[kDtoaBufSize];
  char* end = dtoa_impl(v, buffer);
  return std::string(buffer, end);
}

std::string dtos(double v) {
  char buffer[kDtoaBufSize];
  char* end = dtoa_impl(v, buffer);
  return std::string(buffer, end);
}

// Format straight into a caller-provided buffer, returning the byte count.
// `dst` must have capacity >= kDtoaBufSize. Reuses the same
// dtoa_impl as dtos()/dtos_append (byte-for-byte identical result), so the value
// printer's hot path can format a scalar into a stack buffer and append it to the
// chunk buffer in a single copy (no intermediate std::string).
size_t dtos_to(char* dst, float v) {
  char* end = dtoa_impl(v, dst);
  return static_cast<size_t>(end - dst);
}

size_t dtos_to(char* dst, double v) {
  char* end = dtoa_impl(v, dst);
  return static_cast<size_t>(end - dst);
}

size_t dtos_to_reference(char* dst, float v) {
  return static_cast<size_t>(dtoa_ref_impl(v, dst) - dst);
}

size_t dtos_to_reference(char* dst, double v) {
  return static_cast<size_t>(dtoa_ref_impl(v, dst) - dst);
}

// Append variants: format straight into `out` with no intermediate std::string,
// reusing the exact same dtoa_impl as dtos() (byte-for-byte identical result).
void dtos_append(std::string& out, float v) {
  char buffer[kDtoaBufSize];
  out.append(buffer, dtos_to(buffer, v));
}

void dtos_append(std::string& out, double v) {
  char buffer[kDtoaBufSize];
  out.append(buffer, dtos_to(buffer, v));
}

std::string format_g(double v, int precision) {
  char buffer[48];
  char* end = dtoa_g_impl(v, buffer, precision);
  return std::string(buffer, end);
}

void format_g_append(std::string& out, double v, int precision) {
  char buffer[48];
  char* end = dtoa_g_impl(v, buffer, precision);
  out.append(buffer, static_cast<size_t>(end - buffer));
}

std::string htos(uint16_t bits) {
  char buffer[48];
  return std::string(buffer, htos_to(buffer, bits));
}

namespace {

size_t HalfToShortestUncached(char* dst, uint16_t bits);

// binary16 has only 65536 values, and the shortest-spelling search below
// costs up to five dtoa + parse round trips per value, which made half-backed
// arrays (quath orientations, half3 ...) several times slower to print than
// floats. Memoize the spelling per value, filled lazily in blocks of 256 so a
// small write only pays for the blocks it touches.
struct HalfSpellingCache {
  static constexpr size_t kBlockBits = 8;
  static constexpr size_t kBlocks = size_t(1) << (16 - kBlockBits);
  static constexpr size_t kMaxLen = 15;
  struct Entry {
    uint8_t len;  // 0 = not cacheable (spelling longer than kMaxLen)
    char s[kMaxLen];
  };
  struct Block {
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::once_flag once;
#else
    bool filled = false;
#endif
    Entry e[size_t(1) << kBlockBits];
  };
  Block blocks[kBlocks];

  const Entry& get(uint16_t bits) {
    Block& b = blocks[bits >> kBlockBits];
    auto fill = [&]() {
      const uint16_t base = uint16_t(bits & ~((1u << kBlockBits) - 1u));
      for (size_t i = 0; i < (size_t(1) << kBlockBits); ++i) {
        char buf[kDtoaBufSize + 48];
        const size_t n = HalfToShortestUncached(buf, uint16_t(base + i));
        Entry& e = b.e[i];
        if (n <= kMaxLen) {
          e.len = uint8_t(n);
          std::memcpy(e.s, buf, n);
        } else {
          e.len = 0;
        }
      }
    };
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::call_once(b.once, fill);
#else
    if (!b.filled) {
      fill();
      b.filled = true;
    }
#endif
    return b.e[bits & ((1u << kBlockBits) - 1u)];
  }
};

HalfSpellingCache& GetHalfSpellingCache() {
  static HalfSpellingCache* cache = new HalfSpellingCache();  // ~1 MiB, never freed
  return *cache;
}

}  // namespace

size_t htos_to(char* dst, uint16_t bits) {
  const HalfSpellingCache::Entry& e = GetHalfSpellingCache().get(bits);
  if (e.len != 0) {
    std::memcpy(dst, e.s, e.len);
    return e.len;
  }
  return HalfToShortestUncached(dst, bits);
}

namespace {

size_t HalfToShortestUncached(char* dst, uint16_t bits) {
  const float value = HalfToFloat(bits);
  if (!std::isfinite(value) || value == 0.0f) return dtos_to(dst, value);

  // binary16 needs at most five significant decimal digits for round-trip.
  // Try fewer first so values such as 0x359a (~0.35009765625) retain the
  // shortest half spelling, "0.35", instead of the widened float spelling.
  for (int precision = 1; precision <= 5; ++precision) {
    char candidate[48];
    char* end = dtoa_g_impl(double(value), candidate, precision);
    float parsed = 0.0f;
    const auto result = fast_float::from_chars(
        candidate, end, parsed);
    if (result.ec == std::errc{} &&
        result.ptr == end &&
        FloatToHalf(parsed) == bits) {
      const size_t size = static_cast<size_t>(end - candidate);
      std::memcpy(dst, candidate, size);
      return size;
    }
  }
  return dtos_to(dst, value);
}

}  // namespace

void htos_append(std::string& out, uint16_t bits) {
  char buffer[48];
  out.append(buffer, htos_to(buffer, bits));
}

}  // namespace next
}  // namespace lightusd

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
