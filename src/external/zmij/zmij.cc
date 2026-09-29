// A double-to-string conversion algorithm based on Schubfach and xjb.
// Copyright (c) 2025 - present, Victor Zverovich
// Distributed under the MIT license (see LICENSE) or alternatively
// the Boost Software License, Version 1.0.
// https://github.com/vitaut/zmij/

#if __has_include("zmij.h")
#  include "zmij.h"
#else
namespace zmij {
struct dec_fp {
  long long sig;
  int exp;
  bool negative;
};
}  // namespace zmij
#endif

#include <assert.h>  // assert
#include <stddef.h>  // size_t
#include <stdint.h>  // uint64_t
#include <string.h>  // memcpy

#include <limits>       // std::numeric_limits
#include <type_traits>  // std::conditional_t

#ifndef ZMIJ_USE_SIMD
#  define ZMIJ_USE_SIMD 1
#endif

#ifdef ZMIJ_USE_NEON
// Use the provided definition.
#elif defined(__ARM_NEON) || defined(_M_ARM64)
#  define ZMIJ_USE_NEON ZMIJ_USE_SIMD
#else
#  define ZMIJ_USE_NEON 0
#endif
#if ZMIJ_USE_NEON
#  include <arm_neon.h>
#endif

#ifdef ZMIJ_USE_SSE
// Use the provided definition.
#elif defined(__SSE2__)
#  define ZMIJ_USE_SSE ZMIJ_USE_SIMD
#elif defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP == 2)
#  define ZMIJ_USE_SSE ZMIJ_USE_SIMD
#else
#  define ZMIJ_USE_SSE 0
#endif
#if ZMIJ_USE_SSE
#  include <immintrin.h>
#endif

#ifdef ZMIJ_USE_SSE4_1
// Use the provided definition.
static_assert(!ZMIJ_USE_SSE4_1 || ZMIJ_USE_SSE);
#elif defined(__SSE4_1__) || defined(__AVX__)
// On MSVC there's no way to check for SSE4.1 specifically so check __AVX__.
#  define ZMIJ_USE_SSE4_1 ZMIJ_USE_SSE
#else
#  define ZMIJ_USE_SSE4_1 0
#endif

#define ZMIJ_USE_SIMD_SHUFFLE \
  ((ZMIJ_USE_NEON || ZMIJ_USE_SSE4_1) && !ZMIJ_OPTIMIZE_SIZE)

#ifdef __aarch64__
#  define ZMIJ_AARCH64 1
#else
#  define ZMIJ_AARCH64 0
#endif

#ifdef __x86_64__
#  define ZMIJ_X86_64 1
#else
#  define ZMIJ_X86_64 0
#endif

#ifdef __clang__
#  define ZMIJ_CLANG 1
#else
#  define ZMIJ_CLANG 0
#endif

#ifdef _MSC_VER
#  define ZMIJ_MSC_VER _MSC_VER
#  include <intrin.h>  // __lzcnt64/_umul128/__umulh
#else
#  define ZMIJ_MSC_VER 0
#endif

#if defined(__has_builtin) && !defined(ZMIJ_NO_BUILTINS)
#  define ZMIJ_HAS_BUILTIN(x) __has_builtin(x)
#else
#  define ZMIJ_HAS_BUILTIN(x) 0
#endif
#ifdef __has_attribute
#  define ZMIJ_HAS_ATTRIBUTE(x) __has_attribute(x)
#else
#  define ZMIJ_HAS_ATTRIBUTE(x) 0
#endif
#ifdef __has_cpp_attribute
#  define ZMIJ_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
#  define ZMIJ_HAS_CPP_ATTRIBUTE(x) 0
#endif

#if ZMIJ_HAS_CPP_ATTRIBUTE(likely) && ZMIJ_HAS_CPP_ATTRIBUTE(unlikely)
#  define ZMIJ_LIKELY likely
#  define ZMIJ_UNLIKELY unlikely
#else
#  define ZMIJ_LIKELY
#  define ZMIJ_UNLIKELY
#endif

#if ZMIJ_HAS_CPP_ATTRIBUTE(maybe_unused)
#  define ZMIJ_MAYBE_UNUSED maybe_unused
#else
#  define ZMIJ_MAYBE_UNUSED
#endif

#ifdef ZMIJ_OPTIMIZE_SIZE
// Use the provided definition.
#elif defined(__OPTIMIZE_SIZE__)
#  define ZMIJ_OPTIMIZE_SIZE 1
#else
#  define ZMIJ_OPTIMIZE_SIZE 0
#endif
#ifndef ZMIJ_USE_EXP_STRING_TABLE
#  define ZMIJ_USE_EXP_STRING_TABLE ZMIJ_OPTIMIZE_SIZE == 0
#endif

#if ZMIJ_HAS_ATTRIBUTE(always_inline) && !ZMIJ_OPTIMIZE_SIZE
#  define ZMIJ_INLINE __attribute__((always_inline)) inline
#elif ZMIJ_MSC_VER
#  define ZMIJ_INLINE __forceinline
#else
#  define ZMIJ_INLINE inline
#endif

#ifdef __GNUC__
#  define ZMIJ_ASM(x) asm x
#else
#  define ZMIJ_ASM(x)
#endif

// Declares struct members that must live in memory on ARM64 but are encoded as
// immediates in the x64 assembly.
#ifdef ZMIJ_CONST_DECL
// Use the provided definition.
#elif ZMIJ_AARCH64
#  define ZMIJ_CONST_DECL
#else
#  define ZMIJ_CONST_DECL static constexpr
#endif

namespace {

#ifdef __cpp_lib_is_constant_evaluated
using std::is_constant_evaluated;
#  define ZMIJ_CONSTEXPR constexpr
#else
constexpr auto is_constant_evaluated() -> bool { return false; }
#  define ZMIJ_CONSTEXPR
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
constexpr bool is_big_endian = true;
#else
constexpr bool is_big_endian = false;
#endif

inline auto bswap64(uint64_t x) noexcept -> uint64_t {
#if ZMIJ_HAS_BUILTIN(__builtin_bswap64)
  return __builtin_bswap64(x);
#elif ZMIJ_MSC_VER
  return _byteswap_uint64(x);
#else
  return ((x & 0xff00000000000000) >> 56) | ((x & 0x00ff000000000000) >> 40) |
         ((x & 0x0000ff0000000000) >> 24) | ((x & 0x000000ff00000000) >> +8) |
         ((x & 0x00000000ff000000) << +8) | ((x & 0x0000000000ff0000) << 24) |
         ((x & 0x000000000000ff00) << 40) | ((x & 0x00000000000000ff) << 56);
#endif
}

inline auto clz(uint64_t x) noexcept -> int {
  assert(x != 0);
#if ZMIJ_HAS_BUILTIN(__builtin_clzll)
  return __builtin_clzll(x);
#elif defined(_M_AMD64) && defined(__AVX2__)
  // Use lzcnt only on AVX2-capable CPUs that have this BMI instruction.
  return __lzcnt64(x);
#elif defined(_M_AMD64) || defined(_M_ARM64)
  unsigned long idx;
  _BitScanReverse64(&idx, x);  // Fallback to the BSR instruction.
  return 63 - idx;
#elif ZMIJ_MSC_VER
  // Fallback to the 32-bit BSR instruction.
  unsigned long idx;
  if (_BitScanReverse(&idx, uint32_t(x >> 32))) return 31 - idx;
  _BitScanReverse(&idx, uint32_t(x));
  return 63 - idx;
#else
  int n = 64;
  for (; x > 0; x >>= 1) --n;
  return n;
#endif
}

inline auto ctz(uint64_t x) noexcept -> int {
  assert(x != 0);
#if ZMIJ_HAS_BUILTIN(__builtin_ctzll)
  return __builtin_ctzll(x);
#elif defined(_M_AMD64) || defined(_M_ARM64)
  unsigned long idx;
  _BitScanForward64(&idx, x);
  return idx;
#elif ZMIJ_MSC_VER
  unsigned long idx;
  if (_BitScanForward(&idx, uint32_t(x))) return idx;
  _BitScanForward(&idx, uint32_t(x >> 32));
  return idx + 32;
#else
  int n = 0;
  for (; (x & 1) == 0; x >>= 1) ++n;
  return n;
#endif
}

// Returns true_value if condition != 0, else false_value, without branching.
ZMIJ_INLINE auto select(uint64_t condition, int64_t true_value,
                        int64_t false_value) -> int64_t {
  // Clang can figure it out on its own.
  if (!ZMIJ_X86_64 || ZMIJ_CLANG) return condition ? true_value : false_value;
  ZMIJ_ASM(
      volatile("test %2, %2\n\t"
               "cmovne %1, %0\n\t" :  //
               "+r"(false_value) : "r"(true_value),
               "r"(condition) : "cc"));
  return false_value;
}

struct uint128 {
  uint64_t hi;
  uint64_t lo;

  [[ZMIJ_MAYBE_UNUSED]] explicit constexpr operator uint64_t() const noexcept {
    return lo;
  }

  [[ZMIJ_MAYBE_UNUSED]] constexpr auto operator>>(int shift) const noexcept
      -> uint128 {
    if (shift == 32) return {hi >> 32, (hi << 32) | (lo >> 32)};
    assert(shift >= 64 && shift < 128);
    return {0, hi >> (shift - 64)};
  }
};

#ifdef ZMIJ_USE_INT128
// Use the provided definition.
#elif defined(__SIZEOF_INT128__)
#  define ZMIJ_USE_INT128 1
#else
#  define ZMIJ_USE_INT128 0
#endif

#if ZMIJ_USE_INT128
using uint128_t = unsigned __int128;
#else
using uint128_t = uint128;
#endif  // ZMIJ_USE_INT128

#if ZMIJ_USE_INT128 && defined(__APPLE__)
constexpr bool use_umul128_hi64 = true;  // Use umul128_hi64 for division.
#else
constexpr bool use_umul128_hi64 = false;
#endif

// Computes 128-bit result of multiplication of two 64-bit unsigned integers.
constexpr auto umul128(uint64_t x, uint64_t y) noexcept -> uint128_t {
#if ZMIJ_USE_INT128
  return uint128_t(x) * y;
#else
  if (!is_constant_evaluated()) {
#  if defined(_M_AMD64) && defined(__cpp_lib_is_constant_evaluated)
    uint64_t hi = 0;
    uint64_t lo = _umul128(x, y, &hi);
    return {hi, lo};
#  elif defined(_M_ARM64) && defined(__cpp_lib_is_constant_evaluated)
    return {__umulh(x, y), x * y};
#  endif
  }
  uint64_t a = x >> 32;
  uint64_t b = uint32_t(x);
  uint64_t c = y >> 32;
  uint64_t d = uint32_t(y);

  uint64_t ac = a * c;
  uint64_t bc = b * c;
  uint64_t ad = a * d;
  uint64_t bd = b * d;

  uint64_t cs = (bd >> 32) + uint32_t(ad) + uint32_t(bc);  // cross sum
  return {ac + (ad >> 32) + (bc >> 32) + (cs >> 32), (cs << 32) + uint32_t(bd)};
#endif  // ZMIJ_USE_INT128
}

constexpr auto umul128_hi64(uint64_t x, uint64_t y) noexcept -> uint64_t {
  return uint64_t(umul128(x, y) >> 64);
}

// Returns (x * y + c) >> 64.
inline auto umul128_add_hi64(uint64_t x, uint64_t y, uint64_t c) noexcept
    -> uint64_t {
#if ZMIJ_USE_INT128
  return uint64_t((uint128_t(x) * y + c) >> 64);
#else
  auto p = umul128(x, y);
  return p.hi + (p.lo + c < p.lo);
#endif
}

inline auto umul192_hi128(uint64_t x_hi, uint64_t x_lo, uint64_t y) noexcept
    -> uint128 {
  uint128_t p = umul128(x_hi, y);
  uint64_t lo = uint64_t(p) + uint64_t(umul128(x_lo, y) >> 64);
  return {uint64_t(p >> 64) + (lo < uint64_t(p)), lo};
}

// Returns x / 10 for x <= 2**62.
ZMIJ_INLINE auto div10(uint64_t x) noexcept -> uint64_t {
  assert(x <= (1ull << 62));
  // ceil(2**64 / 10) computed as (1 << 63) / 5 + 1 to avoid int128.
  constexpr uint64_t div10_sig64 = (1ull << 63) / 5 + 1;
  return ZMIJ_USE_INT128 ? umul128_hi64(x, div10_sig64) : x / 10;
}

// Computes the decimal exponent as floor(log10(2**bin_exp)) if regular or
// floor(log10(3/4 * 2**bin_exp)) otherwise, without branching.
constexpr auto compute_dec_exp(int bin_exp, bool regular = true) noexcept
    -> int {
  assert(bin_exp >= -1334 && bin_exp <= 2620);
  // log10_3_over_4_sig = -log10(3/4) * 2**log10_2_exp rounded to a power of 2
  constexpr int log10_3_over_4_sig = 131'072;
  // log10_2_sig = round(log10(2) * 2**log10_2_exp)
  constexpr int log10_2_sig = 315'653, log10_2_exp = 20;
  return (bin_exp * log10_2_sig - !regular * log10_3_over_4_sig) >> log10_2_exp;
}

template <typename Float> struct float_traits : std::numeric_limits<Float> {
  static_assert(float_traits::is_iec559, "IEEE 754 required");

  static constexpr int num_bits = float_traits::digits == 53 ? 64 : 32;
  static constexpr int num_sig_bits = float_traits::digits - 1;
  static constexpr int num_exp_bits = num_bits - num_sig_bits - 1;
  static constexpr int exp_mask = (1 << num_exp_bits) - 1;
  static constexpr int exp_bias = (1 << (num_exp_bits - 1)) - 1;
  static constexpr int exp_offset = exp_bias + num_sig_bits;
  static constexpr int min_fixed_dec_exp = -4;
  static constexpr int max_fixed_dec_exp =
      compute_dec_exp(float_traits::digits + 1) - 1;

  using sig_type = std::conditional_t<num_bits == 64, uint64_t, uint32_t>;
  static constexpr sig_type implicit_bit = sig_type(1) << num_sig_bits;

  static auto to_bits(Float value) noexcept -> sig_type {
    sig_type bits;
    memcpy(&bits, &value, sizeof(value));
    return bits;
  }

  static auto is_negative(sig_type bits) noexcept -> bool {
    return bits >> (num_bits - 1);
  }
  static auto get_sig(sig_type bits) noexcept -> sig_type {
    return bits & (implicit_bit - 1);
  }
  static auto get_exp(sig_type bits) noexcept -> int64_t {
    return int64_t((bits << 1) >> (num_sig_bits + 1));
  }
};

constexpr uint64_t pow10_minor[] = {
    0x8000000000000000, 0xa000000000000000, 0xc800000000000000,
    0xfa00000000000000, 0x9c40000000000000, 0xc350000000000000,
    0xf424000000000000, 0x9896800000000000, 0xbebc200000000000,
    0xee6b280000000000, 0x9502f90000000000, 0xba43b74000000000,
    0xe8d4a51000000000, 0x9184e72a00000000, 0xb5e620f480000000,
    0xe35fa931a0000000, 0x8e1bc9bf04000000, 0xb1a2bc2ec5000000,
    0xde0b6b3a76400000, 0x8ac7230489e80000, 0xad78ebc5ac620000,
    0xd8d726b7177a8000, 0x878678326eac9000, 0xa968163f0a57b400,
    0xd3c21bcecceda100, 0x84595161401484a0, 0xa56fa5b99019a5c8,
    0xcecb8f27f4200f3a,
};
constexpr uint128 pow10_major[] = {
    {0xaf8e5410288e1b6f, 0x07ecf0ae5ee44dda},  // -303
    {0xb1442798f49ffb4a, 0x99cd11cfdf41779d},  // -275
    {0xb2fe3f0b8599ef07, 0x861fa7e6dcb4aa15},  // -247
    {0xb4bca50b065abe63, 0x0fed077a756b53aa},  // -219
    {0xb67f6455292cbf08, 0x1a3bc84c17b1d543},  // -191
    {0xb84687c269ef3bfb, 0x3d5d514f40eea742},  // -163
    {0xba121a4650e4ddeb, 0x92f34d62616ce413},  // -135
    {0xbbe226efb628afea, 0x890489f70a55368c},  // -107
    {0xbdb6b8e905cb600f, 0x5400e987bbc1c921},  //  -79
    {0xbf8fdb78849a5f96, 0xde98520472bdd034},  //  -51
    {0xc16d9a0095928a27, 0x75b7053c0f178294},  //  -23
    {0xc350000000000000, 0x0000000000000000},  //    5
    {0xc5371912364ce305, 0x6c28000000000000},  //   33
    {0xc722f0ef9d80aad6, 0x424d3ad2b7b97ef6},  //   61
    {0xc913936dd571c84c, 0x03bc3a19cd1e38ea},  //   89
    {0xcb090c8001ab551c, 0x5cadf5bfd3072cc6},  //  117
    {0xcd036837130890a1, 0x36dba887c37a8c10},  //  145
    {0xcf02b2c21207ef2e, 0x94f967e45e03f4bc},  //  173
    {0xd106f86e69d785c7, 0xe13336d701beba52},  //  201
    {0xd31045a8341ca07c, 0x1ede48111209a051},  //  229
    {0xd51ea6fa85785631, 0x552a74227f3ea566},  //  257
    {0xd732290fbacaf133, 0xa97c177947ad4096},  //  285
    {0xd94ad8b1c7380874, 0x18375281ae7822bc},  //  313
};
constexpr uint32_t pow10_fixups[] = {
    0x0a4e363f, 0x00001840, 0x00006400, 0x24200040, 0x00000000,
    0x0c000000, 0x82c81380, 0x5e4ce01f, 0xd730f60f, 0x0000001b,
    0x00000000, 0xcdf7fffc, 0x6e8201d8, 0x40cd3fd1, 0xdb642501,
    0x00000d0d, 0x14042400, 0x53713840, 0x11781db4, 0x00000000};

// 128-bit significands of powers of 10 rounded down.
struct pow10_significand_table {
  static constexpr bool compress = ZMIJ_OPTIMIZE_SIZE != 0;
  static constexpr bool split_tables = !compress && ZMIJ_AARCH64 != 0;
  static constexpr int num_pow10s = 618;
  uint64_t data[compress ? 1 : num_pow10s * 2] = {};

  // Computes the 128-bit significand of 10**i using method by Dougall Johnson.
  static constexpr auto compute(unsigned i) noexcept -> uint128 {
    constexpr int stride = sizeof(pow10_minor) / sizeof(*pow10_minor);
    auto m = pow10_minor[(i + 10) % stride];
    auto h = pow10_major[(i + 10) / stride];

    uint64_t h1 = umul128_hi64(h.lo, m);

    uint64_t c0 = h.lo * m;
    uint64_t c1 = h1 + h.hi * m;
    uint64_t c2 = (c1 < h1) + umul128_hi64(h.hi, m);

    uint128 result = (c2 >> 63) != 0
                         ? uint128{c2, c1}
                         : uint128{c2 << 1 | c1 >> 63, c1 << 1 | c0 >> 63};
    result.lo -= (pow10_fixups[i >> 5] >> (i & 31)) & 1;
    return result;
  }

  constexpr pow10_significand_table() {
    for (int i = 0; i < num_pow10s && !compress; ++i) {
      uint128 result = compute(i);
      if (split_tables) {
        data[num_pow10s - i - 1] = result.hi;
        data[num_pow10s * 2 - i - 1] = result.lo;
      } else {
        data[i * 2] = result.hi;
        data[i * 2 + 1] = result.lo;
      }
    }
  }

  ZMIJ_CONSTEXPR auto operator[](int dec_exp) const noexcept -> uint128 {
    constexpr int dec_exp_min = -293;
    int i = dec_exp - dec_exp_min;
    if (compress) return compute(i);
    if (!split_tables) {
      const uint64_t* p = data + i * 2;
      return {p[0], p[1]};
    }
    // The caller passes -e - 1 as dec_exp, so ~dec_exp recovers e. Picking the
    // base so that e itself is the index lets both loads share sxtw addressing.
    const uint64_t* p = data + num_pow10s + dec_exp_min;
    if (!is_constant_evaluated()) ZMIJ_ASM(("" : "+r"(p)));
    return {p[~dec_exp], p[~dec_exp + num_pow10s]};
  }
};

// Computes a shift so that, after scaling by a power of 10, the intermediate
// result always has a fixed 128-bit fractional part (for double).
//
// Different binary exponents can map to the same decimal exponent, but place
// the decimal point at different bit positions. The shift compensates for this.
//
// For example, both 3 * 2**59 and 3 * 2**60 have dec_exp = 2, but dividing by
// 10^dec_exp puts the decimal point in different bit positions:
//   3 * 2**59 / 100 = 1.72...e+16  (needs shift = 1 + 1)
//   3 * 2**60 / 100 = 3.45...e+16  (needs shift = 2 + 1)
constexpr ZMIJ_INLINE auto compute_exp_shift(int bin_exp, int dec_exp) noexcept
    -> unsigned char {
  assert(dec_exp >= -350 && dec_exp <= 350);
  // log2_pow10_sig = round(log2(10) * 2**log2_pow10_exp) + 1
  constexpr int log2_pow10_sig = 217'707, log2_pow10_exp = 16;
  // pow10_bin_exp = floor(log2(10**-dec_exp))
  int pow10_bin_exp = -dec_exp * log2_pow10_sig >> log2_pow10_exp;
  // pow10 = ((pow10_hi << 64) | pow10_lo) * 2**(pow10_bin_exp - 127)
  return bin_exp + pow10_bin_exp + 1;
}

struct exp_shift_table {
  static constexpr bool enable = ZMIJ_OPTIMIZE_SIZE == 0;
  // extra_shift must be >= 3 to keep shift non-negative and <= 11 to
  // fit the significand into 64 bits after the shift.
  static constexpr int extra_shift = 6;
  unsigned char data[enable ? float_traits<double>::exp_mask + 1 : 1] = {};

  constexpr exp_shift_table() {
    for (int raw_exp = 0; raw_exp < sizeof(data) && enable; ++raw_exp) {
      int bin_exp = raw_exp - float_traits<double>::exp_offset;
      if (raw_exp == 0) ++bin_exp;
      int dec_exp = compute_dec_exp(bin_exp);
      data[raw_exp] = compute_exp_shift(bin_exp, dec_exp + 1) + extra_shift;
    }
  }
};

// An optional table of precomputed exponent strings for exponential notation.
// Each entry packs "e+dd" or "e+ddd" into a uint64_t with the length in byte 7.
struct exp_string_table {
  static constexpr bool enable = ZMIJ_USE_EXP_STRING_TABLE;
  using traits = float_traits<double>;
  static constexpr int min_dec_exp =
      traits::min_exponent10 - traits::max_digits10;
  static constexpr int offset = -min_dec_exp;
  uint64_t data[enable ? traits::max_exponent10 - min_dec_exp + 1 : 1] = {};

  constexpr exp_string_table() {
    for (int e = min_dec_exp; e <= traits::max_exponent10 && enable; ++e) {
      uint64_t abs_e = e >= 0 ? e : -e;
      uint64_t bc = abs_e % 100;
      uint64_t val = ((bc % 10 + '0') << 8) | (bc / 10 + '0');
      if (uint64_t a = abs_e / 100) val = (val << 8) | (a + '0');
      uint64_t len = 4 + (abs_e >= 100);
      data[e + offset] =
          (len << 48) | (val << 16) | (uint64_t(e >= 0 ? '+' : '-') << 8) | 'e';
    }
  }
};

// Shuffle vectors to build strings for exponential notation.
//
// Byte positions in the source register assembled by write_exp_float_simd:
//   bytes [0, exp_pos):              BCD ASCII digits (reversed)
//   bytes [exp_pos, exp_pos + 4):    exponent string "e±NN"
//   byte  last_digit_pos:            rounded last digit
//   byte  point_pos:                 '.'
//
// The shuffle length (max 14) is stored in byte 15; the corresponding output
// byte is past the string and ignored by the caller.
struct exp_float_shuffle_table {
  static constexpr bool enable =
      (ZMIJ_USE_SSE4_1 || ZMIJ_USE_NEON) && exp_string_table::enable;
  static constexpr unsigned char exp_pos = 8;
  static constexpr unsigned char last_digit_pos = 12;
  static constexpr unsigned char point_pos = 13;
  alignas(16) unsigned char data[enable ? 32 * 16 : 1] = {};

  struct entry {
    const unsigned char* shuffle;
    unsigned char length;
  };

  constexpr auto get_entry(int num_digits, bool has_last_digit,
                           bool has_extra_digit) const noexcept {
    int idx = (num_digits - 1) * 4 + has_last_digit * 2 + has_extra_digit;
    return entry{&data[idx * 16], data[idx * 16 + 15]};
  }

  constexpr exp_float_shuffle_table() {
    for (int idx = 0; idx < 32 && enable; ++idx) {
      int num_digits = (idx >> 2) + 1;
      bool has_last_digit = ((idx >> 1) & 1) != 0;
      bool has_extra_digit = (idx & 1) != 0;

      unsigned char* out = &data[idx * 16];
      for (int i = 0; i < 16; ++i) out[i] = 0x80;  // shuffle high bit: output 0
      unsigned char leading_digit_pos = has_extra_digit ? 7 : 6;
      unsigned char length = 0;
      if (has_last_digit) {
        // Always 8 BCD chars in the significand plus a last-digit char;
        // for !has_extra_digit the leading '0' of the 8-digit padded BCD is
        // shown.
        out[length++] = leading_digit_pos;
        out[length++] = point_pos;
        for (int i = leading_digit_pos - 1; i >= 0; --i) out[length++] = i;
        out[length++] = last_digit_pos;
      } else {
        length = num_digits + has_extra_digit;
        // Drop the '.' for single-digit output: "5e+02", not "5.0e+02".
        if (length == 2) length = 1;
        out[0] = leading_digit_pos;
        out[1] = point_pos;
        for (int i = 2; i < length; ++i) out[i] = leading_digit_pos + 1 - i;
      }
      for (unsigned char i = 0; i < 4; ++i) out[length++] = exp_pos + i;
      out[15] = length;
    }
  }
};

// Per-decimal-exponent buffer layout for branchless fixed-notation output.
// Each entry holds the byte positions of the leading zeros, decimal point,
// and end of output, indexed by the decimal exponent (dec_exp).
struct fixed_layout_table {
  using traits = float_traits<double>;
  static constexpr int num_entries =
      traits::max_fixed_dec_exp - traits::min_fixed_dec_exp + 1;

  // On AArch64, align entry to 32 bytes so indexing uses `lsl #5` not `umaddl`.
  // On x86 with SSE4.1, align entry to 64 bytes (one cache line) and put the
  // 16-byte-aligned shuffle table at the back: the scalar fields and the
  // shuffle for any single dec_exp then share one cache-line fill.
  struct alignas(ZMIJ_AARCH64 && !ZMIJ_OPTIMIZE_SIZE ? 32
                 : ZMIJ_USE_SSE4_1
                    ? (ZMIJ_OPTIMIZE_SIZE ? 16 : 64) : 1) entry {
#if ZMIJ_USE_SSE4_1
    // pshufb shuffle table that places BCD bytes in their final output slots,
    // with the byte at the decimal-point position (if any) set to a pshufb
    // "zero" marker (high bit set).  Indexed by extra_digit.
    //  Needs 16 byte alignment because of aligned load below.
    alignas(16) unsigned char shuffle[2][16];
#endif

// Byte offset past leading "0.00..." before first significant digit.
    unsigned char start_pos;
    unsigned char point_pos;
    // Start position for shifting digits right by one to insert the point.
    unsigned char shift_pos;
#if ZMIJ_USE_SSE4_1
    // Buffer-relative position of the last_digit byte, indexed by has_extra_digit.
    // Only used for bcd_size == 16 (doubles).
    unsigned char last_digit_pos[2];
#endif
    // Offset past the end of fixed-notation output, indexed by sig length - 1.
    unsigned char end_pos[traits::max_digits10];
  };
  entry data[num_entries] = {};

  constexpr fixed_layout_table() {
    for (int dec_exp = traits::min_fixed_dec_exp;
         dec_exp <= traits::max_fixed_dec_exp; ++dec_exp) {
      auto& e = data[dec_exp - traits::min_fixed_dec_exp];

      e.start_pos = dec_exp < -0 ? 1 - dec_exp : 0;
      e.point_pos = dec_exp >= 0 ? 1 + dec_exp : 1;
      e.shift_pos = e.point_pos + (dec_exp >= 0);

#if ZMIJ_USE_SSE4_1
      constexpr int bcd_size = 16;
      for (int extra = 0; extra < 2; ++extra) {
        int full_size = bcd_size + extra - 1;
        e.last_digit_pos[extra] =
            full_size + ((0 <= dec_exp) && (dec_exp < full_size));
      }

      // Build the shuffle tables.  to_digits returns natural-order BCD bytes
      // (BCD[k] at register byte k).  For extra_digit == 1 the integer part
      // is BCD[0..dec_exp]; for extra_digit == 0 the leading zero is dropped,
      // so the integer part is BCD[1..dec_exp+1].  In both cases the decimal
      // point sits at output byte 1 + dec_exp (when in [0, 14]) and is
      // encoded as a pshufb zero-marker (high bit set).  For dec_exp == 15
      // or dec_exp < 0 no point lives inside the vector and the entry is
      // just identity with the extra_digit offset folded in.
      int hole = (dec_exp >= 0 && dec_exp <= 14) ? 1 + dec_exp : 128;
      for (int extra = 0; extra < 2; ++extra) {
        int bcd_idx = !extra;
        for (int i = 0; i < bcd_size; ++i) {
          if (i == hole) {
            e.shuffle[extra][i] = 0xFF;
          } else {
            e.shuffle[extra][i] = (unsigned char)bcd_idx++;
          }
        }
      }
#endif

      for (int n = 1; n <= traits::max_digits10; ++n) {
        int end_pos = dec_exp < 0 ? n : (n > dec_exp + 1 ? n + 1 : dec_exp + 1);
        e.end_pos[n - 1] = end_pos;
      }
    }
  }

  constexpr auto get(int dec_exp) const noexcept -> const entry& {
    constexpr auto min = traits::min_fixed_dec_exp;
    assert(dec_exp >= min && dec_exp <= traits::max_fixed_dec_exp);
    return data[unsigned(dec_exp - min)];
  }
};

inline auto count_trailing_nonzeros(uint64_t x) noexcept -> int {
  // We count the number of bytes until there are only zeros left.
  // The code is equivalent to
  //   return 8 - clz(x) / 8
  // but if the BSR instruction is emitted (as gcc on x64 does with
  // default settings), subtracting the constant before dividing allows
  // the compiler to combine it with the subtraction which it inserts
  // due to BSR counting in the opposite direction.
  //
  // Additionally, the BSR instruction requires a zero check.  Since the
  // high bit is unused we can avoid the zero check by shifting the
  // datum left by one and inserting a sentinel bit at the end. This can
  // be faster than the automatically inserted range check.
  if (is_big_endian) x = bswap64(x);
  return (size_t(70) - clz((x << 1) | 1)) / 8;  // size_t for native arithmetic
}

// Converts value in the range [0, 100) to a string. GCC generates a bit better
// code when value is pointer-size (https://www.godbolt.org/z/5fEPMT1cc).
inline auto digits2(size_t value) noexcept -> const char* {
  // Align data since unaligned access may be slower when crossing a
  // hardware-specific boundary.
  alignas(2) static const char data[] =
      "0001020304050607080910111213141516171819"
      "2021222324252627282930313233343536373839"
      "4041424344454647484950515253545556575859"
      "6061626364656667686970717273747576777879"
      "8081828384858687888990919293949596979899";
  return &data[value * 2];
}

constexpr int div10k_exp = 40;
constexpr uint32_t div10k_sig = uint32_t((1ull << div10k_exp) / 10000 + 1);
constexpr uint32_t neg10k = uint32_t((1ull << 32) - 10000);

constexpr int div100_exp = 19;
constexpr uint32_t div100_sig = (1 << div100_exp) / 100 + 1;
constexpr uint32_t neg100 = (1 << 16) - 100;

constexpr int div10_exp = 10;
constexpr uint32_t div10_sig = (1 << div10_exp) / 10 + 1;
constexpr uint32_t neg10 = (1 << 8) - 10;

constexpr uint64_t zeros = 0x0101010101010101u * '0';

inline auto write_if(char* buffer, uint32_t digit, bool condition) noexcept
    -> char* {
  *buffer = char('0' + digit);
  return buffer + condition;
}

struct data {
  static constexpr auto splat64(uint64_t x) -> uint128 { return {x, x}; }
  static constexpr auto splat32(uint32_t x) -> uint128 {
    return splat64(uint64_t(x) << 32 | x);
  }
  static constexpr auto splat16(uint16_t x) -> uint128 {
    return splat32(uint32_t(x) << 16 | x);
  }
  static constexpr auto pack8(uint8_t a, uint8_t b, uint8_t c, uint8_t d,  //
                              uint8_t e, uint8_t f, uint8_t g, uint8_t h)
      -> uint64_t {
    using u64 = uint64_t;
    return u64(h) << 56 | u64(g) << 48 | u64(f) << 40 | u64(e) << 32 |
           u64(d) << 24 | u64(c) << 16 | u64(b) << +8 | u64(a);
  }

  ZMIJ_CONST_DECL uint64_t threshold = 1e15;
  // +6 is needed for boundary cases found by verify.py.
  ZMIJ_CONST_DECL uint64_t biased_half = (uint64_t(1) << 63) + 6;

#if ZMIJ_USE_NEON
  static constexpr int32_t neg10k = 0x10000 - 10000;

  using int32x4 = std::conditional_t<ZMIJ_MSC_VER != 0, int32_t[4], int32x4_t>;
  using int16x8 = std::conditional_t<ZMIJ_MSC_VER != 0, int16_t[8], int16x8_t>;

  uint64_t mul_const = 0xabcc77118461cefd;
  uint64_t hundred_million = 100000000;
  int32x4 multipliers32 = {div10k_sig, neg10k, div100_sig << 12, neg100};
  int16x8 multipliers16 = {0xce0, neg10};
#elif ZMIJ_USE_SSE
  // Ordered so that the values used to format floats fit in a single cache
  // line.
  uint128 div100 = splat32(div100_sig);
  uint128 div10 = splat16((1 << 16) / 10 + 1);
#  if ZMIJ_USE_SSE4_1
  uint128 neg100 = splat32(::neg100);
  uint128 neg10 = splat16((1 << 8) - 10);
  uint128 bswap = uint128{pack8(15, 14, 13, 12, 11, 10, 9, 8),
                          pack8(7, 6, 5, 4, 3, 2, 1, 0)};
#  else
  uint128 hundred = splat32(100);
  uint128 moddiv10 = splat16(10 * (1 << 8) - 1);
#  endif  // ZMIJ_USE_SSE4_1
  uint128 div10k = splat64(div10k_sig);
  uint128 neg10k = splat64(::neg10k);
  uint128 zeros = splat64(::zeros);
#endif    // ZMIJ_USE_SSE

  exp_shift_table exp_shifts;
  exp_string_table exp_strings;
  alignas(64) pow10_significand_table pow10_significands;
  fixed_layout_table fixed_layouts;
  exp_float_shuffle_table exp_float_shuffles;

  // Shuffle indices for SIMD digit shift. Offset 0 = identity, offset 1 =
  // shift left by 1 (drops the leading '0' of a 16-digit significand).
  unsigned char shift_shuffle[17] = {0, 1,  2,  3,  4,  5,  6,  7, 8,
                                     9, 10, 11, 12, 13, 14, 15, 0};
};
alignas(64) constexpr data static_data;

#if ZMIJ_USE_NEON  // An optimized version for NEON by Dougall Johnson.

// Converts four numbers < 10000, one in each 32-bit lane, to BCD digits.
ZMIJ_INLINE auto to_bcd_4x4(int32x4_t efgh_abcd_mnop_ijkl,
                            const data& d) noexcept -> uint8x16_t {
  // Compiler barrier, or clang breaks the subsequent MLA into UADDW + MUL.
  ZMIJ_ASM(("" : "+w"(efgh_abcd_mnop_ijkl)));

  int32x4_t ef_ab_mn_ij =
      vqdmulhq_n_s32(efgh_abcd_mnop_ijkl, d.multipliers32[2]);
  int16x8_t gh_ef_cd_ab_op_mn_kl_ij = vreinterpretq_s16_s32(
      vmlaq_n_s32(efgh_abcd_mnop_ijkl, ef_ab_mn_ij, d.multipliers32[3]));
  int16x8_t high_10s =
      vqdmulhq_n_s16(gh_ef_cd_ab_op_mn_kl_ij, d.multipliers16[0]);
  return vreinterpretq_u8_s16(
      vmlaq_n_s16(gh_ef_cd_ab_op_mn_kl_ij, high_10s, d.multipliers16[1]));
}

ZMIJ_INLINE auto to_unshuffled_digits(uint64_t value, const data& d)
    -> uint8x16_t {
  uint64_t hundred_million = d.hundred_million;

  // Compiler barrier, or clang narrows the load to 32-bit and unpairs it.
  ZMIJ_ASM(("" : "+r"(hundred_million)));

  // abcdefgh = value / 100000000, ijklmnop = value % 100000000.
  uint64_t abcdefgh = uint64_t(umul128(value, d.mul_const) >> 90);
  uint64_t ijklmnop = value - abcdefgh * hundred_million;

  uint64x1_t ijklmnop_abcdefgh_64 = {ijklmnop << 32 | abcdefgh};
  int32x2_t abcdefgh_ijklmnop = vreinterpret_s32_u64(ijklmnop_abcdefgh_64);

  int32x2_t abcd_ijkl = vreinterpret_s32_u32(
      vshr_n_u32(vreinterpret_u32_s32(
                     vqdmulh_n_s32(abcdefgh_ijklmnop, d.multipliers32[0])),
                 9));
  int32x2_t efgh_abcd_mnop_ijkl_32 =
      vmla_n_s32(abcdefgh_ijklmnop, abcd_ijkl, d.multipliers32[1]);

  int32x4_t efgh_abcd_mnop_ijkl = vreinterpretq_s32_u32(
      vshll_n_u16(vreinterpret_u16_s32(efgh_abcd_mnop_ijkl_32), 0));
  return to_bcd_4x4(efgh_abcd_mnop_ijkl, d);
}

#elif ZMIJ_USE_SSE

using m128ptr = const __m128i*;

// Converts four numbers < 10000, one in each 32-bit lane, to BCD digits.
// Digits in each 32-bit lane will be in order for SSE2, reversed for SSE4.1.
ZMIJ_INLINE auto to_bcd_4x4(__m128i y, const data& d) noexcept -> __m128i {
  const __m128i div100 = _mm_load_si128(m128ptr(&d.div100));
  const __m128i div10 = _mm_load_si128(m128ptr(&d.div10));
#  if ZMIJ_USE_SSE4_1
  const __m128i neg100 = _mm_load_si128(m128ptr(&d.neg100));
  const __m128i neg10 = _mm_load_si128(m128ptr(&d.neg10));

  // _mm_mullo_epi32 is SSE 4.1
  __m128i z = _mm_add_epi64(
      y,
      _mm_mullo_epi32(neg100, _mm_srli_epi32(_mm_mulhi_epu16(y, div100), 3)));
  return _mm_add_epi16(z, _mm_mullo_epi16(neg10, _mm_mulhi_epu16(z, div10)));
#  else
  const __m128i hundred = _mm_load_si128(m128ptr(&d.hundred));
  const __m128i moddiv10 = _mm_load_si128(m128ptr(&d.moddiv10));

  __m128i y_div_100 = _mm_srli_epi16(_mm_mulhi_epu16(y, div100), 3);
  __m128i y_mod_100 = _mm_sub_epi16(y, _mm_mullo_epi16(y_div_100, hundred));
  __m128i z = _mm_or_si128(_mm_slli_epi32(y_mod_100, 16), y_div_100);
  return _mm_sub_epi16(_mm_slli_epi16(z, 8),
                       _mm_mullo_epi16(moddiv10, _mm_mulhi_epu16(z, div10)));
#  endif  // ZMIJ_USE_SSE4_1
}

#endif  // ZMIJ_USE_SSE

struct bcd_result {
  uint64_t bcd;
  int len;
};

auto to_bcd8(uint64_t abcdefgh) noexcept -> bcd_result {
  if (!ZMIJ_USE_SSE && !ZMIJ_USE_NEON) {
    // An optimization from Xiang JunBo.
    // Three steps BCD. Base 10000 -> base 100 -> base 10.
    // div and mod are evaluated simultaneously as, e.g.
    //   (abcdefgh / 10000) << 32 + (abcdefgh % 10000)
    //      == abcdefgh + (2**32 - 10000) * (abcdefgh / 10000)))
    // where the division on the RHS is implemented by the multiply + shift
    // trick and the fractional bits are masked away.
    uint64_t abcd_efgh =
        abcdefgh + neg10k * ((abcdefgh * div10k_sig) >> div10k_exp);
    uint64_t ab_cd_ef_gh =
        abcd_efgh +
        neg100 * (((abcd_efgh * div100_sig) >> div100_exp) & 0x7f0000007f);
    uint64_t a_b_c_d_e_f_g_h =
        ab_cd_ef_gh +
        neg10 * (((ab_cd_ef_gh * div10_sig) >> div10_exp) & 0xf000f000f000f);
    uint64_t bcd = is_big_endian ? a_b_c_d_e_f_g_h : bswap64(a_b_c_d_e_f_g_h);
    return {bcd, count_trailing_nonzeros(bcd)};
  }

  const auto* d = &static_data;
  ZMIJ_ASM(("" : "+r"(d)));  // Load constants from memory.

#if ZMIJ_USE_NEON
  uint64_t abcd_efgh_64 =
      abcdefgh + neg10k * ((abcdefgh * div10k_sig) >> div10k_exp);
  int32x4_t abcd_efgh = vcombine_s32(
      vreinterpret_s32_u64(vcreate_u64(abcd_efgh_64)), vdup_n_s32(0));
  uint8x16_t digits_128 = to_bcd_4x4(abcd_efgh, *d);
  uint8x8_t digits = vget_low_u8(digits_128);
  uint64_t bcd = vget_lane_u64(vreinterpret_u64_u8(vrev64_u8(digits)), 0);
  return {bcd, count_trailing_nonzeros(bcd)};
#elif ZMIJ_USE_SSE4_1
  uint64_t abcd_efgh =
      abcdefgh + neg10k * ((abcdefgh * div10k_sig) >> div10k_exp);
  uint64_t unshuffled_bcd =
      _mm_cvtsi128_si64(to_bcd_4x4(_mm_set_epi64x(0, abcd_efgh), *d));
  int len = unshuffled_bcd ? 8 - ctz(unshuffled_bcd) / 8 : 0;
  return {bswap64(unshuffled_bcd), len};
#elif ZMIJ_USE_SSE
  // Evaluate the 4-digit limbs and arrange them such that we get a result which
  // is in the correct order.
  uint64_t abcd_efgh =
      (abcdefgh << 32) -
      uint64_t((10000ull << 32) - 1) * ((abcdefgh * div10k_sig) >> div10k_exp);
  __m128i v = to_bcd_4x4(_mm_set_epi64x(0, abcd_efgh), *d);
#  if defined(__x86_64__) || defined(_M_X64)
  uint64_t bcd = _mm_cvtsi128_si64(v);
#  else
  uint64_t bcd = uint64_t(_mm_cvtsi128_si32(_mm_srli_si128(v, 4))) << 32 |
                 uint32_t(_mm_cvtsi128_si32(v));
#  endif
  return {bcd, count_trailing_nonzeros(bcd)};
#endif  // ZMIJ_USE_SSE
}

template <int num_bits> struct dec_digits {
  uint64_t digits;
  // `unshuffled` is the byte-reversed BCD vector used by write_exp_float_simd.
#if ZMIJ_USE_NEON
  uint8x16_t unshuffled;
#elif ZMIJ_USE_SSE4_1
  __m128i unshuffled;
#endif
  int num_digits;
};

template <> struct dec_digits<64> {
#if ZMIJ_USE_NEON
  using digits_type = uint16x8_t;
#elif ZMIJ_USE_SSE
  using digits_type = __m128i;
#else
  using digits_type = uint128;
#endif
  digits_type digits;
  int num_digits;
};

// Converts a significand to decimal digits, removing trailing zeros. value has
// up to 17 decimal digits (16-17 for normals) for double (num_bits == 64) and
// up to 9 digits (8-9 for normals) for float.
template <int num_bits>
ZMIJ_INLINE auto to_digits(uint64_t value, const data& d) noexcept
    -> dec_digits<num_bits> {
#if !ZMIJ_USE_NEON && !ZMIJ_USE_SSE
  uint32_t hi = uint32_t(value / 100'000'000);
  uint32_t lo = uint32_t(value % 100'000'000);
  auto hi_bcd = to_bcd8(hi);
  if (lo == 0) return {{hi_bcd.bcd + zeros, zeros}, hi_bcd.len};
  auto lo_bcd = to_bcd8(lo);
  return {{hi_bcd.bcd + zeros, lo_bcd.bcd + zeros}, 8 + lo_bcd.len};
#elif ZMIJ_USE_NEON
  auto unshuffled_digits = to_unshuffled_digits(value, d);
  uint8x16_t digits = vrev64q_u8(unshuffled_digits);
  uint16x8_t str = vaddq_u16(vreinterpretq_u16_u8(digits),
                             vreinterpretq_u16_s8(vdupq_n_s8('0')));
  uint16x8_t is_not_zero =
      vreinterpretq_u16_u8(vcgtzq_s8(vreinterpretq_s8_u8(digits)));
  uint64_t nonzero_mask =
      vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(is_not_zero, 4)), 0);
  return {str, 16 - (clz(nonzero_mask) >> 2)};
#else  // ZMIJ_USE_SSE
  uint32_t hi = uint32_t(value / 100'000'000);
  uint32_t lo = uint32_t(value % 100'000'000);

  const __m128i div10k = _mm_load_si128(m128ptr(&d.div10k));
  const __m128i neg10k = _mm_load_si128(m128ptr(&d.neg10k));
  __m128i x = _mm_set_epi64x(hi, lo);
  __m128i y = _mm_add_epi64(
      x, _mm_mul_epu32(neg10k,
                       _mm_srli_epi64(_mm_mul_epu32(x, div10k), div10k_exp)));

  // Shuffle to ensure correctly ordered result from SSE2 path.
  if (!ZMIJ_USE_SSE4_1) y = _mm_shuffle_epi32(y, _MM_SHUFFLE(0, 1, 2, 3));

  __m128i bcd = to_bcd_4x4(y, d);
  const __m128i zeros = _mm_load_si128(m128ptr(&d.zeros));

  // Computed against current bcd (rather than the post-bswap bcd) so the mask
  // is derived in parallel with the shuffle on the SSE4.1 path.
  uint64_t mask = _mm_movemask_epi8(_mm_cmpgt_epi8(bcd, _mm_setzero_si128()));
  // Trailing zeros are in the low bits for SSE4.1, the high bits for SSE2.
  int len = ZMIJ_USE_SSE4_1 ? 16 - ctz(mask) : 64 - clz(mask);
#  if ZMIJ_USE_SSE4_1
  bcd = _mm_shuffle_epi8(bcd, _mm_load_si128(m128ptr(&d.bswap)));  // SSSE3
#  endif
  return {_mm_or_si128(bcd, zeros), len};
#endif  // ZMIJ_USE_SSE
}

template <>
ZMIJ_INLINE auto to_digits<32>(uint64_t value,
                               [[ZMIJ_MAYBE_UNUSED]] const data& d) noexcept
    -> dec_digits<32> {
#if ZMIJ_USE_SSE4_1
  // Inline to_bcd8's SSE4.1 body so we can return the unshuffled xmm too;
  // the exponential-notation path uses it to skip the bswap-via-gpr.
  uint64_t abcd_efgh = value + neg10k * ((value * div10k_sig) >> div10k_exp);
  __m128i bcd_xmm = to_bcd_4x4(_mm_set_epi64x(0, abcd_efgh), d);
  uint64_t unshuffled_bcd = _mm_cvtsi128_si64(bcd_xmm);
  int len = unshuffled_bcd ? 8 - ctz(unshuffled_bcd) / 8 : 0;
  return {bswap64(unshuffled_bcd) + zeros, bcd_xmm, len};
#elif ZMIJ_USE_NEON
  // Inline to_bcd8's NEON body so we can return the unshuffled vector too;
  // the exponential-notation path uses it to skip the simd->gpr->bswap->simd
  // roundtrip needed to materialize `digits`.
  uint64_t abcd_efgh = value + neg10k * ((value * div10k_sig) >> div10k_exp);
  int32x4_t input =
      vcombine_s32(vreinterpret_s32_u64(vcreate_u64(abcd_efgh)), vdup_n_s32(0));
  uint8x16_t unshuffled = to_bcd_4x4(input, d);
  uint64_t unshuffled_bcd =
      vget_lane_u64(vreinterpret_u64_u8(vget_low_u8(unshuffled)), 0);
  int len = unshuffled_bcd ? 8 - ctz(unshuffled_bcd) / 8 : 0;
  return {bswap64(unshuffled_bcd) + zeros, unshuffled, len};
#else
  auto result = to_bcd8(value);
  return {result.bcd + zeros, result.len};
#endif
}

// Writes `digits` to `buffer`, dropping the leading '0' when drop_leading_zero
// is set. On SIMD, folds the shift into the digit shuffle to avoid a
// dependent 16-byte memmove.
ZMIJ_INLINE void write_digits(char* buffer, dec_digits<64>::digits_type digits,
                              bool drop_leading_zero, const data& d) noexcept {
  if (!ZMIJ_USE_NEON && !ZMIJ_USE_SSE4_1) {
    memcpy(buffer, &digits, sizeof(digits));
    memmove(buffer, buffer + drop_leading_zero, sizeof(digits));
    return;
  }
#if ZMIJ_USE_NEON
  uint8x16_t shuffle = vld1q_u8(d.shift_shuffle + drop_leading_zero);
  uint8x16_t shifted = vqtbl1q_u8(vreinterpretq_u8_u16(digits), shuffle);
  vst1q_u8(reinterpret_cast<uint8_t*>(buffer), shifted);
#elif ZMIJ_USE_SSE4_1
  __m128i shuffle = _mm_loadu_si128(
      reinterpret_cast<const __m128i*>(d.shift_shuffle + drop_leading_zero));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(buffer),
                   _mm_shuffle_epi8(digits, shuffle));
#endif
}

ZMIJ_INLINE void write_digits(char* buffer, uint64_t digits,
                              bool drop_leading_zero, const data&) noexcept {
  memcpy(buffer, &digits, sizeof(digits));
  memmove(buffer, buffer + drop_leading_zero, sizeof(digits));
}

ZMIJ_INLINE auto write_exp_float_simd(char* buffer, const dec_digits<32>& dig,
                                      int last_digit, bool has_last_digit,
                                      bool has_extra_digit, uint64_t exp_data,
                                      const data& d) noexcept -> char* {
  // Packed for insertion into lane 1: byte 0 of `tail` lands at register
  // byte exp_pos (8), so the exp string fills exp_pos..exp_pos+3; the prefix
  // shifts place '0'+last_digit at last_digit_pos (12) and '.' at point_pos
  // (13).
  uint32_t prefix = (uint32_t('.') << 8) + uint32_t('0') + last_digit;
  uint64_t tail = exp_data | (uint64_t(prefix) << 32);
  auto entry = d.exp_float_shuffles.get_entry(dig.num_digits, has_last_digit,
                                              has_extra_digit);
#if ZMIJ_USE_SSE4_1
  __m128i ascii =
      _mm_or_si128(dig.unshuffled, _mm_load_si128(m128ptr(&d.zeros)));
  __m128i src = _mm_insert_epi64(ascii, int64_t(tail), 1);
  __m128i shuffle = _mm_load_si128(m128ptr(entry.shuffle));
  __m128i out = _mm_shuffle_epi8(src, shuffle);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(buffer), out);
#elif ZMIJ_USE_NEON
  uint8x16_t ascii = vorrq_u8(dig.unshuffled, vdupq_n_u8('0'));
  uint8x16_t src = vreinterpretq_u8_u64(
      vsetq_lane_u64(tail, vreinterpretq_u64_u8(ascii), 1));
  uint8x16_t shuffle = vld1q_u8(entry.shuffle);
  uint8x16_t out = vqtbl1q_u8(src, shuffle);
  vst1q_u8(reinterpret_cast<uint8_t*>(buffer), out);
#endif
  return buffer + entry.length;
}

ZMIJ_INLINE auto write_exp_float_simd(char*, const dec_digits<64>&, int, bool,
                                      bool, uint64_t, const data&) noexcept
    -> char* {
  return nullptr;
}

struct to_decimal_result {
  long long sig;
  int exp;
  int last_digit = 0;
  bool has_last_digit = false;
};

// Here be 🐉s.
// Converts a binary FP number bin_sig * 2**bin_exp to the shortest decimal
// representation, where bin_exp = raw_exp - exp_offset.
template <typename Float, typename UInt>
ZMIJ_INLINE auto to_decimal(UInt bin_sig, int64_t raw_exp, bool regular,
                            const data& d) noexcept -> to_decimal_result {
  using traits = float_traits<Float>;
  int64_t bin_exp = raw_exp - traits::exp_offset;
  constexpr int num_bits = std::numeric_limits<UInt>::digits;
  constexpr int extra_shift = exp_shift_table::extra_shift;

  if (!regular) [[ZMIJ_UNLIKELY]] {
    int dec_exp = compute_dec_exp(bin_exp, false);
    unsigned char shift = compute_exp_shift(bin_exp, dec_exp + 1) + extra_shift;
    uint128 pow10 = d.pow10_significands[-dec_exp - 1];
    uint128 p = umul192_hi128(pow10.hi, pow10.lo, bin_sig << shift);

    long long integral = p.hi >> extra_shift;
    uint64_t fractional = p.hi << (64 - extra_shift) | p.lo >> extra_shift;

    uint64_t half_ulp = pow10.hi >> (extra_shift + 1 - shift);
    bool round_up = half_ulp > ~uint64_t(0) - fractional;
    bool round_down = (half_ulp >> 1) > fractional;
    integral += round_up;

    int digit = int(umul128_add_hi64(fractional, 10, (uint64_t(1) << 63) - 1));
    int lo =
        int(umul128_add_hi64(fractional - (half_ulp >> 1), 10, ~uint64_t(0)));
    if (digit < lo) digit = lo;
    return {integral, dec_exp, digit, (round_up + round_down) == 0};
  }

  constexpr uint64_t log10_2_sig = 78'913;
  constexpr int log10_2_exp = 18;
  int dec_exp = use_umul128_hi64
                    ? umul128_hi64(bin_exp, log10_2_sig << (64 - log10_2_exp))
                    : compute_dec_exp(bin_exp);
  ZMIJ_ASM(("" : "+r"(dec_exp)));  // Force 32-bit reg for sxtw addressing.
  unsigned char shift =
      exp_shift_table::enable
          ? d.exp_shifts.data[bin_exp + float_traits<double>::exp_offset]
          : compute_exp_shift(bin_exp, dec_exp + 1) + extra_shift;
  uint64_t even = 1 - (bin_sig & 1);

  if (num_bits == 32) {
    constexpr int extra_shift = 34;
    shift += extra_shift - exp_shift_table::extra_shift;
    uint64_t pow10_hi = d.pow10_significands[-dec_exp - 1].hi;
    uint64_t p = umul128_hi64(pow10_hi + 1, uint64_t(bin_sig) << shift);

    long long integral = p >> extra_shift;
    uint64_t fractional = p & ((1ull << extra_shift) - 1);

    uint64_t half_ulp = (pow10_hi >> (65 - shift)) + even;
    bool round_up = (fractional + half_ulp) >> extra_shift;
    bool round_down = half_ulp > fractional;
    integral += round_up;

    int digit = int((fractional * 10 + (uint64_t(1) << (extra_shift - 1))) >>
                    extra_shift);
    if (fractional == (uint64_t(1) << (extra_shift - 2))) [[ZMIJ_UNLIKELY]]
      digit = 2;  // Round 2.5 to 2.
    return {integral, dec_exp, digit, (round_up + round_down) == 0};
  }

  // An optimization by Xiang JunBo:
  // Scale by 10**(-dec_exp-1) to directly produce the shorter candidate
  // (15-16 digits), deriving the extra digit from the fractional part.
  // This eliminates div10 from the critical path.
  //
  // value = 5.0507837461e-27
  // next  = 5.0507837461000010e-27
  //
  // c = integral.fractional' = 5050783746100000.3153987... (value)
  //                            5050783746100001.0328635... (next)
  //                 half_ulp =                0.3587324...
  //
  // fractional = fractional' * 2**64 = 5818079786399166407
  //
  //    5050783746100000.0       c               upper    5050783746100001.0
  //             s              l|   L             |               S
  // ──┬────┬────┼────┬────┬────┼*───┼────┬────┬───*┬────┬────┬────┼─*──┬───
  //  .8   .9   .0   .1   .2   .3   .4   .5   .6   .7   .8   .9   .0 | .1
  //           └─────────────────┼─────────────────┘                next
  //                            1ulp
  //
  // s - shorter underestimate, S - shorter overestimate
  // l - longer underestimate,  L - longer overestimate
  uint128 pow10 = d.pow10_significands[-dec_exp - 1];
  uint128 p = umul192_hi128(pow10.hi, pow10.lo, bin_sig << shift);

  long long integral = p.hi >> extra_shift;
  uint64_t fractional = p.hi << (64 - extra_shift) | p.lo >> extra_shift;

  uint64_t half_ulp = (pow10.hi >> (extra_shift + 1 - shift)) + even;
  bool round_up = fractional + half_ulp < fractional;
  bool round_down = half_ulp > fractional;
  integral += round_up;  // Compute integral before digit.

  // Derive the extra digit from the fractional part (parallel with rounding).
  int digit = int(umul128_add_hi64(fractional, 10, d.biased_half));
  if (fractional == (1ull << 62)) [[ZMIJ_UNLIKELY]]
    digit = 2;  // Round 2.5 to 2.
  return {integral, dec_exp, digit, (round_up + round_down) == 0};
}

}  // namespace

namespace zmij {

// NOTE (lightusd): `inline` dropped so this links as a real symbol
// from a separately-compiled zmij.cc (upstream leaves it inline, unemitted).
auto to_decimal(double value) noexcept -> dec_fp {
  using traits = float_traits<double>;
  auto bits = traits::to_bits(value);
  auto bin_exp = traits::get_exp(bits);  // binary exponent
  auto bin_sig = traits::get_sig(bits);  // binary significand
  auto negative = traits::is_negative(bits);
  if (bin_exp == 0 || bin_exp == traits::exp_mask) [[ZMIJ_UNLIKELY]] {
    if (bin_exp != 0) return {int64_t(bin_sig), int(~0u >> 1), negative};
    if (bin_sig == 0) return {0, 0, negative};
    bin_exp = 1;
    bin_sig |= traits::implicit_bit;
  }
  auto dec = ::to_decimal<double>(bin_sig ^ traits::implicit_bit, bin_exp,
                                  bin_sig != 0, static_data);
  auto last_digit = -dec.has_last_digit & dec.last_digit;
  return {dec.sig * 10 + last_digit, dec.exp, negative};
}

// LOCAL ADDITION (lightusd): a float overload of to_decimal,
// mirroring to_decimal(double) above with float_traits<float>. Upstream zmij
// only exposes to_decimal(double); USD writes mostly float3 (points/normals/uv)
// so the shared-renderer benchmark needs float shortest digits too. Not part of
// upstream zmij. See doc/dtoa.md on the next-refactor branch.
auto to_decimal(float value) noexcept -> dec_fp {
  using traits = float_traits<float>;
  auto bits = traits::to_bits(value);
  auto bin_exp = traits::get_exp(bits);  // binary exponent
  auto bin_sig = traits::get_sig(bits);  // binary significand
  auto negative = traits::is_negative(bits);
  if (bin_exp == 0 || bin_exp == traits::exp_mask) [[ZMIJ_UNLIKELY]] {
    if (bin_exp != 0) return {int64_t(bin_sig), int(~0u >> 1), negative};
    if (bin_sig == 0) return {0, 0, negative};
    bin_exp = 1;
    bin_sig |= traits::implicit_bit;
  }
  auto dec = ::to_decimal<float>(bin_sig ^ traits::implicit_bit, bin_exp,
                                 bin_sig != 0, static_data);
  auto last_digit = -dec.has_last_digit & dec.last_digit;
  return {dec.sig * 10 + last_digit, dec.exp, negative};
}

namespace detail {

// It is slightly faster to return a pointer to the end than the size.
template <typename Float>
auto write(Float value, char* buffer) noexcept -> char* {
  using traits = float_traits<Float>;
  auto bits = traits::to_bits(value);
  // It is beneficial to extract exponent and significand early.
  auto bin_exp = traits::get_exp(bits);  // binary exponent
  auto bin_sig = traits::get_sig(bits);  // binary significand

  *buffer = '-';
  buffer += traits::is_negative(bits);

  const auto* d = &static_data;
  ZMIJ_ASM(("" : "+r"(d)));  // Load constants from memory.
  uint64_t threshold = traits::num_bits == 64 ? d->threshold : uint64_t(1e7);

  to_decimal_result dec;
  bool is_normal = unsigned(bin_exp - 1) < unsigned(traits::exp_mask - 1);
  if (!is_normal) [[ZMIJ_UNLIKELY]] {
    if (bin_exp != 0) {
      memcpy(buffer, bin_sig == 0 ? "inf" : "nan", 4);
      return buffer + 3;
    }
    if (bin_sig == 0) {
      memcpy(buffer, "0", 2);
      return buffer + 1;
    }
    dec = ::to_decimal<Float>(bin_sig, 1, true, *d);
    long long dec_sig = dec.sig * 10 + (-dec.has_last_digit & dec.last_digit);
    int dec_exp = dec.exp;
    while (dec_sig < threshold) {
      dec_sig *= 10;
      --dec_exp;
    }
    long long q = ::div10(dec_sig);
    int last_digit = dec_sig - q * 10;
    dec = {q, dec_exp, last_digit, last_digit != 0};
  } else {
    dec = ::to_decimal<Float>(bin_sig | traits::implicit_bit, bin_exp,
                              bin_sig != 0, *d);
  }
  bool has_last_digit = dec.has_last_digit;
  bool has_extra_digit = dec.sig >= threshold;
  int dec_exp = dec.exp + traits::max_digits10 - 2 + has_extra_digit;
  if (traits::num_bits == 32 && dec.sig < uint32_t(1e6)) [[ZMIJ_UNLIKELY]] {
    dec.sig = 10 * dec.sig + (-has_last_digit & dec.last_digit);
    has_last_digit = false;
    --dec_exp;
  }

  // Write significand/fixed.
  char* start = buffer;
  auto dig = to_digits<traits::num_bits>(dec.sig, *d);
  constexpr int bcd_size = traits::num_bits == 64 ? 16 : 8;
  if (dec_exp >= traits::min_fixed_dec_exp &&
      dec_exp <= traits::max_fixed_dec_exp) {
    memcpy(start, &zeros, 8);  // For dec_exp < 0.
    char last_digit = '0' + (-has_last_digit & dec.last_digit);
    int num_digits = select(has_last_digit, bcd_size, dig.num_digits - 1);

    // Materialize the base early so the entry address is `base + idx*32`;
    // otherwise Clang folds the offset in and adds a cycle to the idx chain.
    const auto* fixed_layouts = &d->fixed_layouts;
    if (ZMIJ_AARCH64) ZMIJ_ASM(("" : "+r"(fixed_layouts)));

    const auto& layout = fixed_layouts->get(dec_exp);
    buffer += layout.start_pos;
#if ZMIJ_USE_SSE4_1
    if (bcd_size == 16) {
      // dig.digits is uint64_t for float, alias as __m128i to avoid a compiler error.
      auto& digits = reinterpret_cast<const __m128i&>(dig.digits);
      // Assemble the digit string (minus the last_digit and, for has_extra_digit
      // == 1, BCD[15]) in a single SIMD register and store it in one go.
      __m128i tbl = _mm_load_si128(m128ptr(&layout.shuffle[has_extra_digit]));
      __m128i out = _mm_shuffle_epi8(digits, tbl);
      memcpy(buffer, &out, bcd_size);
      // For has_extra_digit == 1 with 0 <= dec_exp <= 14 the BCD[15] byte falls
      // outside the vector at buffer[16]; write it unconditionally.  In the
      // other cases (dec_exp == 15, dec_exp < 0, or has_extra_digit == 0) it's
      // either already in the vector or overwritten below by '.' / last_digit.
      buffer[bcd_size] = (char)_mm_extract_epi8(digits, 15);
      start[layout.point_pos] = '.';
      buffer[layout.last_digit_pos[has_extra_digit]] = last_digit;
      return buffer + layout.end_pos[num_digits + has_extra_digit - 1];
    }
#endif
    write_digits(buffer, dig.digits, !has_extra_digit, *d);
    buffer[bcd_size + has_extra_digit - 1] = last_digit;
    unsigned point_pos = layout.point_pos;
    memmove(start + layout.shift_pos, start + point_pos, bcd_size);
    start[point_pos] = '.';
    return buffer + layout.end_pos[num_digits + has_extra_digit - 1];
  }
  if (traits::num_bits == 32 && exp_float_shuffle_table::enable) {
    uint64_t exp_data = d->exp_strings.data[dec_exp + exp_string_table::offset];
    return write_exp_float_simd(buffer, dig, dec.last_digit, has_last_digit,
                                has_extra_digit, exp_data, *d);
  }

  buffer += has_extra_digit;
  memcpy(buffer, &dig.digits, bcd_size);
  buffer[bcd_size] = '0' + dec.last_digit;
  buffer += select(has_last_digit, bcd_size + 1, dig.num_digits);
  start[0] = start[1];
  start[1] = '.';
  buffer -= (buffer - 1 == start + 1);  // Remove trailing point.

  // Write exponent.
  if (exp_string_table::enable) {
    uint64_t exp_data = d->exp_strings.data[dec_exp + exp_string_table::offset];
    int len = int(exp_data >> 48);
    if (is_big_endian) exp_data = bswap64(exp_data);
    memcpy(buffer, &exp_data, traits::max_exponent10 >= 100 ? 8 : 4);
    return buffer + len;
  }
  uint16_t e_sign = dec_exp >= 0 ? ('+' << 8 | 'e') : ('-' << 8 | 'e');
  if (is_big_endian) e_sign = e_sign << 8 | e_sign >> 8;
  memcpy(buffer, &e_sign, 2);
  buffer += 2;
  dec_exp = dec_exp >= 0 ? dec_exp : -dec_exp;
  if (traits::max_exponent10 >= 100) {
    // digit = dec_exp / 100
    uint32_t digit = use_umul128_hi64
                         ? umul128_hi64(dec_exp, 0x290000000000000)
                         : (uint32_t(dec_exp) * div100_sig) >> div100_exp;
    *buffer = '0' + digit;
    buffer += dec_exp >= 100;
    dec_exp -= digit * 100;
  }
  memcpy(buffer, digits2(dec_exp), 2);
  return buffer + 2;
}

template auto write(float value, char* buffer) noexcept -> char*;
template auto write(double value, char* buffer) noexcept -> char*;

// LOCAL ADDITION (lightusd): a usdcat-notation FAST PATH.
//
// Reuses zmij's fixed-notation SIMD block verbatim (digit BCD + single-shuffle
// decimal-point placement) for the leading-decimal-exponent range where zmij's
// fixed notation is byte-identical to OpenUSD/`usdcat` fixed notation:
//   dec_exp in [-4, min(14, max_fixed_dec_exp)]
// (zmij's fixed low bound is -4; usdcat's high bound is <15; both write plain
// fixed digits with no trailing ".0" here). Returns nullptr for anything else
// (special values, subnormals, and every exponent outside that window — i.e.
// values usdcat renders in scientific notation, or the [-6,-5] fixed bucket zmij
// renders scientific) so the caller falls back to a scalar usdcat renderer.
// Realistic USD floats (coords/normals/uvs) live almost entirely in this window.
// Byte-identity is enforced by sandbox/dtoa's exhaustive conformance gate.
template <typename Float>
auto write_usd_fast(Float value, char* buffer) noexcept -> char* {
  using traits = float_traits<Float>;
  auto bits = traits::to_bits(value);
  auto bin_exp = traits::get_exp(bits);  // binary exponent
  auto bin_sig = traits::get_sig(bits);  // binary significand

  bool is_normal = unsigned(bin_exp - 1) < unsigned(traits::exp_mask - 1);
  if (!is_normal) return nullptr;  // nan/inf/zero/subnormal -> scalar fallback

  *buffer = '-';
  buffer += traits::is_negative(bits);

  const auto* d = &static_data;
  ZMIJ_ASM(("" : "+r"(d)));  // Load constants from memory.
  uint64_t threshold = traits::num_bits == 64 ? d->threshold : uint64_t(1e7);

  to_decimal_result dec = ::to_decimal<Float>(bin_sig | traits::implicit_bit,
                                              bin_exp, bin_sig != 0, *d);
  bool has_last_digit = dec.has_last_digit;
  bool has_extra_digit = dec.sig >= threshold;
  int dec_exp = dec.exp + traits::max_digits10 - 2 + has_extra_digit;
  if (traits::num_bits == 32 && dec.sig < uint32_t(1e6)) [[ZMIJ_UNLIKELY]] {
    dec.sig = 10 * dec.sig + (-has_last_digit & dec.last_digit);
    has_last_digit = false;
    --dec_exp;
  }

  // Only the range where zmij-fixed == usdcat-fixed. Everything else -> fallback.
  constexpr int usd_hi =
      traits::max_fixed_dec_exp < 14 ? traits::max_fixed_dec_exp : 14;
  if (dec_exp < -4 || dec_exp > usd_hi) return nullptr;

  // ---- zmij's fixed-notation block, verbatim from detail::write ----
  char* start = buffer;
  auto dig = to_digits<traits::num_bits>(dec.sig, *d);
  constexpr int bcd_size = traits::num_bits == 64 ? 16 : 8;
  memcpy(start, &zeros, 8);  // For dec_exp < 0.
  char last_digit = '0' + (-has_last_digit & dec.last_digit);
  int num_digits = select(has_last_digit, bcd_size, dig.num_digits - 1);

  const auto* fixed_layouts = &d->fixed_layouts;
  if (ZMIJ_AARCH64) ZMIJ_ASM(("" : "+r"(fixed_layouts)));

  const auto& layout = fixed_layouts->get(dec_exp);
  buffer += layout.start_pos;
#if ZMIJ_USE_SSE4_1
  if (bcd_size == 16) {
    auto& digits = reinterpret_cast<const __m128i&>(dig.digits);
    __m128i tbl = _mm_load_si128(m128ptr(&layout.shuffle[has_extra_digit]));
    __m128i out = _mm_shuffle_epi8(digits, tbl);
    memcpy(buffer, &out, bcd_size);
    buffer[bcd_size] = (char)_mm_extract_epi8(digits, 15);
    start[layout.point_pos] = '.';
    buffer[layout.last_digit_pos[has_extra_digit]] = last_digit;
    return buffer + layout.end_pos[num_digits + has_extra_digit - 1];
  }
#endif
  write_digits(buffer, dig.digits, !has_extra_digit, *d);
  buffer[bcd_size + has_extra_digit - 1] = last_digit;
  unsigned point_pos = layout.point_pos;
  memmove(start + layout.shift_pos, start + point_pos, bcd_size);
  start[point_pos] = '.';
  return buffer + layout.end_pos[num_digits + has_extra_digit - 1];
}

template auto write_usd_fast(float value, char* buffer) noexcept -> char*;
template auto write_usd_fast(double value, char* buffer) noexcept -> char*;

}  // namespace detail
}  // namespace zmij
