// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - SIMD scanning helpers (implementation)
//
// Two build paths, selected at compile time:
//   * SSE2  - the x86-64 baseline (and 32-bit x86 only when the compiler is
//             actually targeting SSE2). All x86 toolchains (GCC/Clang/MSVC).
//   * scalar - everything else: ARM/AArch64, WebAssembly (no-SIMD), other ISAs,
//             and 32-bit x86 without SSE2. Byte-for-byte identical results, just
//             without the bulk speedup.
// The scalar routine is the SSE2 path's <16-byte tail handler too, so it is
// always compiled.

#include "simd-scan.hh"

// Master switch. The SIMD backends (SSE2 / NEON) are compiled in by default;
// define LIGHTUSD_DISABLE_SIMD (e.g. -DLIGHTUSD_DISABLE_SIMD) to force the
// portable scalar path on every target. LIGHTUSD_ENABLE_SIMD may also be defined
// explicitly; disable wins if both are set.
#if defined(LIGHTUSD_DISABLE_SIMD)
#undef LIGHTUSD_ENABLE_SIMD
#elif !defined(LIGHTUSD_ENABLE_SIMD)
#define LIGHTUSD_ENABLE_SIMD
#endif

#if defined(LIGHTUSD_ENABLE_SIMD)
// Gate each backend on the intrinsics' *availability* (matching str-util.cc), not
// just the target arch. On a no-SIMD x86 build (e.g. -m32 without -msse2)
// __i386__ is still defined but the SSE2 intrinsics are unusable, so keying on the
// arch macros alone would pull in <emmintrin.h> and fail to compile. __SSE2__ is
// always set on x86-64 and on -msse2 i386; MSVC implies SSE2 for x64 / /arch:SSE2.
// NEON is mandatory (always available) on AArch64.
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86) && _M_IX86_FP >= 2)
// immintrin (not just emmintrin): the runtime-dispatched AVX2 kernel uses
// 256-bit intrinsics via the GCC/Clang `target` attribute; the TU itself is
// still compiled for the SSE2 baseline.
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>  // _BitScanForward (MSVC has no __builtin_ctz)
#endif
#define LIGHTUSD_SIMDSCAN_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>  // NEON (baseline on AArch64)
#if defined(_MSC_VER)
#include <intrin.h>  // _BitScanForward64 for MSVC ARM64
#endif
#define LIGHTUSD_SIMDSCAN_NEON 1
#endif
#endif  // LIGHTUSD_ENABLE_SIMD

namespace lightusd {
namespace next {
namespace simdscan {

namespace {

// Is `c` one of the array-structural bytes the capture loop must stop on?
inline bool IsStructural(char c) {
  return c == '[' || c == ']' || c == '"' || c == '\'' || c == '@' || c == '#';
}

// Is `c` one of the prim-block-structural bytes (brace/paren matching plus the
// skip-region starters)?
inline bool IsPrimStructural(char c) {
  return c == '{' || c == '}' || c == '(' || c == ')' || c == '[' ||
         c == ']' || c == '"' || c == '\'' || c == '@' || c == '#' ||
         c == '/';
}

// Scalar reference (also the SSE2/NEON path's <16-byte tail handler and the
// non-x86/non-AArch64 path, so it is always compiled).
const char* ScanScalar(const char* p, const char* end, size_t* newlines,
                       size_t* commas) {
  size_t nl = 0;
  size_t cm = 0;
  for (; p < end; ++p) {
    const char c = *p;
    if (IsStructural(c)) {
      *newlines += nl;
      *commas += cm;
      return p;
    }
    if (c == '\n') ++nl;
    if (c == ',') ++cm;
  }
  *newlines += nl;
  *commas += cm;
  return end;
}

const char* ScanPrimScalar(const char* p, const char* end, size_t* newlines) {
  size_t nl = 0;
  for (; p < end; ++p) {
    const char c = *p;
    if (IsPrimStructural(c)) {
      *newlines += nl;
      return p;
    }
    if (c == '\n') ++nl;
  }
  *newlines += nl;
  return end;
}

}  // namespace

#if defined(LIGHTUSD_SIMDSCAN_SSE2)

namespace {

// popcount of a <=16-bit mask, inlined SWAR. __builtin_popcount emits a libgcc
// call (__popcountdi2) under the SSE2 baseline (no POPCNT), which showed up hot;
// this stays a few register ops and is compiler-portable.
inline unsigned PopCount16(unsigned x) {
  x = x - ((x >> 1) & 0x5555u);
  x = (x & 0x3333u) + ((x >> 2) & 0x3333u);
  x = (x + (x >> 4)) & 0x0F0Fu;
  return (x + (x >> 8)) & 0x1Fu;
}

// Index of the lowest set bit. `x` is always non-zero at the call sites.
inline unsigned LowestSetBit(unsigned x) {
#if defined(_MSC_VER)
  unsigned long idx;
  _BitScanForward(&idx, x);
  return static_cast<unsigned>(idx);
#else
  return static_cast<unsigned>(__builtin_ctz(x));
#endif
}

}  // namespace

// AVX2 kernel (runtime-dispatched): same stop positions and tallies as the
// SSE2 loop, 32 bytes per iteration. Compiled via the GCC/Clang `target`
// attribute so the translation unit itself stays baseline-SSE2; selected once
// at runtime with __builtin_cpu_supports. popcnt is implied by the avx2
// target set, but list it explicitly for older GCC.
#if defined(__GNUC__) || defined(__clang__)
#define LIGHTUSD_SIMDSCAN_AVX2_DISPATCH 1

__attribute__((target("avx2,popcnt")))
static const char* ScanArrayStructuralAvx2(const char* p, const char* end,
                                           size_t* newlines, size_t* commas) {
  const __m256i lb = _mm256_set1_epi8('[');
  const __m256i rb = _mm256_set1_epi8(']');
  const __m256i dq = _mm256_set1_epi8('"');
  const __m256i sq = _mm256_set1_epi8('\'');
  const __m256i at = _mm256_set1_epi8('@');
  const __m256i hs = _mm256_set1_epi8('#');
  const __m256i nlv = _mm256_set1_epi8('\n');
  const __m256i cmv = _mm256_set1_epi8(',');

  size_t nl = 0;
  size_t cm = 0;
  while (end - p >= 32) {
    const __m256i v =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, lb),
                                _mm256_cmpeq_epi8(v, rb));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, dq));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, sq));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, at));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, hs));
    const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(m));
    const unsigned nlmask =
        static_cast<unsigned>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, nlv)));
    const unsigned cmmask =
        static_cast<unsigned>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, cmv)));
    if (mask) {
      const unsigned idx = static_cast<unsigned>(__builtin_ctz(mask));
      // newlines/commas strictly before the structural byte. idx <= 31, so the
      // (1u << idx) - 1 mask never shifts by 32.
      const unsigned before = (1u << idx) - 1u;
      nl += static_cast<unsigned>(__builtin_popcount(nlmask & before));
      cm += static_cast<unsigned>(__builtin_popcount(cmmask & before));
      *newlines += nl;
      *commas += cm;
      return p + idx;
    }
    nl += static_cast<unsigned>(__builtin_popcount(nlmask));
    cm += static_cast<unsigned>(__builtin_popcount(cmmask));
    p += 32;
  }
  *newlines += nl;
  *commas += cm;
  return ScanScalar(p, end, newlines, commas);  // <32-byte tail
}
#endif  // __GNUC__ || __clang__

const char* ScanArrayStructural(const char* p, const char* end,
                                size_t* newlines, size_t* commas) {
#if defined(LIGHTUSD_SIMDSCAN_AVX2_DISPATCH)
  static const bool use_avx2 = __builtin_cpu_supports("avx2");
  if (use_avx2) return ScanArrayStructuralAvx2(p, end, newlines, commas);
#endif
  const __m128i lb = _mm_set1_epi8('[');
  const __m128i rb = _mm_set1_epi8(']');
  const __m128i dq = _mm_set1_epi8('"');
  const __m128i sq = _mm_set1_epi8('\'');
  const __m128i at = _mm_set1_epi8('@');
  const __m128i hs = _mm_set1_epi8('#');
  const __m128i nlv = _mm_set1_epi8('\n');
  const __m128i cmv = _mm_set1_epi8(',');

  size_t nl = 0;
  size_t cm = 0;
  while (end - p >= 16) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, lb), _mm_cmpeq_epi8(v, rb));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, dq));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, sq));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, at));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, hs));
    const unsigned mask =
        static_cast<unsigned>(_mm_movemask_epi8(m)) & 0xFFFFu;
    const unsigned nlmask =
        static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, nlv))) &
        0xFFFFu;
    const unsigned cmmask =
        static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, cmv))) &
        0xFFFFu;
    if (mask) {
      const unsigned idx = LowestSetBit(mask);
      // newlines/commas strictly before the structural byte
      const unsigned before = (1u << idx) - 1u;
      nl += PopCount16(nlmask & before);
      cm += PopCount16(cmmask & before);
      *newlines += nl;
      *commas += cm;
      return p + idx;
    }
    if (nlmask) nl += PopCount16(nlmask);
    if (cmmask) cm += PopCount16(cmmask);
    p += 16;
  }
  *newlines += nl;
  *commas += cm;
  return ScanScalar(p, end, newlines, commas);  // <16-byte tail
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,popcnt")))
static const char* ScanPrimStructuralAvx2(const char* p, const char* end,
                                          size_t* newlines) {
  const __m256i ob = _mm256_set1_epi8('{');
  const __m256i cb = _mm256_set1_epi8('}');
  const __m256i op = _mm256_set1_epi8('(');
  const __m256i cp = _mm256_set1_epi8(')');
  const __m256i lb = _mm256_set1_epi8('[');
  const __m256i rb = _mm256_set1_epi8(']');
  const __m256i dq = _mm256_set1_epi8('"');
  const __m256i sq = _mm256_set1_epi8('\'');
  const __m256i at = _mm256_set1_epi8('@');
  const __m256i hs = _mm256_set1_epi8('#');
  const __m256i sl = _mm256_set1_epi8('/');
  const __m256i nlv = _mm256_set1_epi8('\n');

  size_t nl = 0;
  while (end - p >= 32) {
    const __m256i v =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, ob),
                                _mm256_cmpeq_epi8(v, cb));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, op));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, cp));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, lb));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, rb));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, dq));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, sq));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, at));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, hs));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, sl));
    const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(m));
    const unsigned nlmask =
        static_cast<unsigned>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, nlv)));
    if (mask) {
      const unsigned idx = static_cast<unsigned>(__builtin_ctz(mask));
      const unsigned before = (1u << idx) - 1u;
      nl += static_cast<unsigned>(__builtin_popcount(nlmask & before));
      *newlines += nl;
      return p + idx;
    }
    nl += static_cast<unsigned>(__builtin_popcount(nlmask));
    p += 32;
  }
  *newlines += nl;
  return ScanPrimScalar(p, end, newlines);  // <32-byte tail
}
#endif  // __GNUC__ || __clang__

const char* ScanPrimStructural(const char* p, const char* end,
                               size_t* newlines) {
#if defined(LIGHTUSD_SIMDSCAN_AVX2_DISPATCH)
  static const bool use_avx2 = __builtin_cpu_supports("avx2");
  if (use_avx2) return ScanPrimStructuralAvx2(p, end, newlines);
#endif
  const __m128i ob = _mm_set1_epi8('{');
  const __m128i cb = _mm_set1_epi8('}');
  const __m128i op = _mm_set1_epi8('(');
  const __m128i cp = _mm_set1_epi8(')');
  const __m128i lb = _mm_set1_epi8('[');
  const __m128i rb = _mm_set1_epi8(']');
  const __m128i dq = _mm_set1_epi8('"');
  const __m128i sq = _mm_set1_epi8('\'');
  const __m128i at = _mm_set1_epi8('@');
  const __m128i hs = _mm_set1_epi8('#');
  const __m128i sl = _mm_set1_epi8('/');
  const __m128i nlv = _mm_set1_epi8('\n');

  size_t nl = 0;
  while (end - p >= 16) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, ob), _mm_cmpeq_epi8(v, cb));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, op));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, cp));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, lb));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, rb));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, dq));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, sq));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, at));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, hs));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, sl));
    const unsigned mask =
        static_cast<unsigned>(_mm_movemask_epi8(m)) & 0xFFFFu;
    const unsigned nlmask =
        static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, nlv))) &
        0xFFFFu;
    if (mask) {
      const unsigned idx = LowestSetBit(mask);
      nl += PopCount16(nlmask & ((1u << idx) - 1u));
      *newlines += nl;
      return p + idx;
    }
    if (nlmask) nl += PopCount16(nlmask);
    p += 16;
  }
  *newlines += nl;
  return ScanPrimScalar(p, end, newlines);  // <16-byte tail
}

const char* Backend() { return "sse2"; }

#elif defined(LIGHTUSD_SIMDSCAN_NEON)

const char* ScanArrayStructural(const char* p, const char* end,
                                size_t* newlines, size_t* commas) {
  const uint8x16_t lb = vdupq_n_u8('[');
  const uint8x16_t rb = vdupq_n_u8(']');
  const uint8x16_t dq = vdupq_n_u8('"');
  const uint8x16_t sq = vdupq_n_u8('\'');
  const uint8x16_t at = vdupq_n_u8('@');
  const uint8x16_t hs = vdupq_n_u8('#');
  const uint8x16_t nlv = vdupq_n_u8('\n');
  const uint8x16_t cmv = vdupq_n_u8(',');
  const uint8x16_t one = vdupq_n_u8(1);

  size_t nl = 0;
  size_t cm = 0;
  while (end - p >= 16) {
    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    uint8x16_t m = vorrq_u8(vceqq_u8(v, lb), vceqq_u8(v, rb));
    m = vorrq_u8(m, vceqq_u8(v, dq));
    m = vorrq_u8(m, vceqq_u8(v, sq));
    m = vorrq_u8(m, vceqq_u8(v, at));
    m = vorrq_u8(m, vceqq_u8(v, hs));

    // NEON has no movemask; reduce the 16x {0x00,0xFF} compare result to a 64-bit
    // value holding 4 bits per input byte via the standard shrn-by-4 trick. The
    // value is nonzero iff some byte matched, and the first match's byte index is
    // (count-trailing-zeros / 4) on little-endian AArch64.
    const uint64_t mmask = vget_lane_u64(
        vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
    if (mmask) {
#if defined(_MSC_VER)
      unsigned long bidx;
      _BitScanForward64(&bidx, mmask);
      const int idx = static_cast<int>(bidx >> 2);
#else
      const int idx = static_cast<int>(__builtin_ctzll(mmask) >> 2);
#endif
      // newlines/commas strictly before the structural byte (scalar; idx < 16)
      for (int i = 0; i < idx; ++i) {
        if (p[i] == '\n') ++nl;
        if (p[i] == ',') ++cm;
      }
      *newlines += nl;
      *commas += cm;
      return p + idx;
    }
    // No structural byte in this block: add every newline/comma in it. vaddvq_u8
    // of the {0,1} per-byte mask is the byte popcount (<=16, fits in u8).
    nl += vaddvq_u8(vandq_u8(vceqq_u8(v, nlv), one));
    cm += vaddvq_u8(vandq_u8(vceqq_u8(v, cmv), one));
    p += 16;
  }
  *newlines += nl;
  *commas += cm;
  return ScanScalar(p, end, newlines, commas);  // <16-byte tail
}

const char* ScanPrimStructural(const char* p, const char* end,
                               size_t* newlines) {
  const uint8x16_t ob = vdupq_n_u8('{');
  const uint8x16_t cb = vdupq_n_u8('}');
  const uint8x16_t op = vdupq_n_u8('(');
  const uint8x16_t cp = vdupq_n_u8(')');
  const uint8x16_t lb = vdupq_n_u8('[');
  const uint8x16_t rb = vdupq_n_u8(']');
  const uint8x16_t dq = vdupq_n_u8('"');
  const uint8x16_t sq = vdupq_n_u8('\'');
  const uint8x16_t at = vdupq_n_u8('@');
  const uint8x16_t hs = vdupq_n_u8('#');
  const uint8x16_t sl = vdupq_n_u8('/');
  const uint8x16_t nlv = vdupq_n_u8('\n');
  const uint8x16_t one = vdupq_n_u8(1);

  size_t nl = 0;
  while (end - p >= 16) {
    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    uint8x16_t m = vorrq_u8(vceqq_u8(v, ob), vceqq_u8(v, cb));
    m = vorrq_u8(m, vceqq_u8(v, op));
    m = vorrq_u8(m, vceqq_u8(v, cp));
    m = vorrq_u8(m, vceqq_u8(v, lb));
    m = vorrq_u8(m, vceqq_u8(v, rb));
    m = vorrq_u8(m, vceqq_u8(v, dq));
    m = vorrq_u8(m, vceqq_u8(v, sq));
    m = vorrq_u8(m, vceqq_u8(v, at));
    m = vorrq_u8(m, vceqq_u8(v, hs));
    m = vorrq_u8(m, vceqq_u8(v, sl));
    const uint64_t mmask = vget_lane_u64(
        vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
    if (mmask) {
#if defined(_MSC_VER)
      unsigned long bidx;
      _BitScanForward64(&bidx, mmask);
      const int idx = static_cast<int>(bidx >> 2);
#else
      const int idx = static_cast<int>(__builtin_ctzll(mmask) >> 2);
#endif
      for (int i = 0; i < idx; ++i) {
        if (p[i] == '\n') ++nl;
      }
      *newlines += nl;
      return p + idx;
    }
    nl += vaddvq_u8(vandq_u8(vceqq_u8(v, nlv), one));
    p += 16;
  }
  *newlines += nl;
  return ScanPrimScalar(p, end, newlines);  // <16-byte tail
}

const char* Backend() { return "neon"; }

#else  // no SIMD backend: scalar (WASM / disabled / other archs; identical result)

const char* ScanArrayStructural(const char* p, const char* end,
                                size_t* newlines, size_t* commas) {
  return ScanScalar(p, end, newlines, commas);
}

const char* ScanPrimStructural(const char* p, const char* end,
                               size_t* newlines) {
  return ScanPrimScalar(p, end, newlines);
}

const char* Backend() { return "scalar"; }

#endif  // LIGHTUSD_SIMDSCAN_SSE2

}  // namespace simdscan
}  // namespace next
}  // namespace lightusd
