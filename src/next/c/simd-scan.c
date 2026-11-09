/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * simd-scan.c -- C11 SIMD byte scanning for the USDA parser.
 *
 * Faithful C11 port of src/next/parser/simd-scan.cc. Bulk byte scanning for the
 * array-literal hot path: skip runs of "boring" bytes (digits/commas/
 * whitespace) at SIMD speed and stop only at structurally-interesting bytes.
 * Pure scanning -- it changes only *how fast* the same byte positions are
 * found, never *which* (the C++ forwarder is byte-identical).
 *
 * Two build paths, selected at compile time:
 *   * SSE2  - the x86-64 baseline (and 32-bit x86 only when the compiler is
 *             actually targeting SSE2). All x86 toolchains.
 *   * NEON  - AArch64 (baseline).
 *   * scalar- everything else (WASM, other ISAs), byte-for-byte identical.
 * The scalar routine is also the SIMD path's <16/<32-byte tail handler.
 */

#include "lightusd-core-c.h"

#include <stdint.h>
#include <stddef.h>

/* Master switch: the SIMD backends are compiled in by default; define
 * LIGHTUSD_DISABLE_SIMD to force the portable scalar path on every target. */
#if defined(LIGHTUSD_DISABLE_SIMD)
#undef LIGHTUSD_ENABLE_SIMD
#elif !defined(LIGHTUSD_ENABLE_SIMD)
#define LIGHTUSD_ENABLE_SIMD
#endif

#if defined(LIGHTUSD_ENABLE_SIMD)
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86) && _M_IX86_FP >= 2)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h> /* _BitScanForward (MSVC has no __builtin_ctz) */
#endif
#define LIGHTUSD_SIMDSCAN_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#if defined(_MSC_VER)
#include <intrin.h> /* _BitScanForward64 for MSVC ARM64 */
#endif
#define LIGHTUSD_SIMDSCAN_NEON 1
#endif
#endif /* LIGHTUSD_ENABLE_SIMD */

/* Is `c` one of the array-structural bytes the capture loop must stop on? */
static int lu_simd_is_structural(char c) {
  return c == '[' || c == ']' || c == '"' || c == '\'' || c == '@' || c == '#';
}

/* Is `c` one of the prim-block-structural bytes? */
static int lu_simd_is_prim_structural(char c) {
  return c == '{' || c == '}' || c == '(' || c == ')' || c == '[' ||
         c == ']' || c == '"' || c == '\'' || c == '@' || c == '#' ||
         c == '/';
}

/* Scalar reference (also the SIMD path's tail handler, so always compiled). */
static const char *lu_simd_scan_scalar(const char *p, const char *end,
                                       size_t *newlines, size_t *commas) {
  size_t nl = 0;
  size_t cm = 0;
  for (; p < end; ++p) {
    const char c = *p;
    if (lu_simd_is_structural(c)) {
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

static const char *lu_simd_scan_prim_scalar(const char *p, const char *end,
                                            size_t *newlines) {
  size_t nl = 0;
  for (; p < end; ++p) {
    const char c = *p;
    if (lu_simd_is_prim_structural(c)) {
      *newlines += nl;
      return p;
    }
    if (c == '\n') ++nl;
  }
  *newlines += nl;
  return end;
}

#if defined(LIGHTUSD_SIMDSCAN_SSE2)

/* popcount of a <=16-bit mask, inlined SWAR (avoids the libgcc __popcountdi2
 * call hot under the SSE2 baseline). */
static unsigned lu_simd_popcount16(unsigned x) {
  x = x - ((x >> 1) & 0x5555u);
  x = (x & 0x3333u) + ((x >> 2) & 0x3333u);
  x = (x + (x >> 4)) & 0x0F0Fu;
  return (x + (x >> 8)) & 0x1Fu;
}

/* Index of the lowest set bit. `x` is always non-zero at the call sites. */
static unsigned lu_simd_lowest_set_bit(unsigned x) {
#if defined(_MSC_VER)
  unsigned long idx;
  _BitScanForward(&idx, x);
  return (unsigned)idx;
#else
  return (unsigned)__builtin_ctz(x);
#endif
}

/* AVX2 kernel (runtime-dispatched): same stop positions and tallies as the
 * SSE2 loop, 32 bytes per iteration. */
#if defined(__GNUC__) || defined(__clang__)
#define LIGHTUSD_SIMDSCAN_AVX2_DISPATCH 1

__attribute__((target("avx2,popcnt")))
static const char *lu_simd_scan_array_avx2(const char *p, const char *end,
                                           size_t *newlines, size_t *commas) {
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
    const __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)p);
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, lb),
                                _mm256_cmpeq_epi8(v, rb));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, dq));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, sq));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, at));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(v, hs));
    const unsigned mask = (unsigned)_mm256_movemask_epi8(m);
    const unsigned nlmask =
        (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, nlv));
    const unsigned cmmask =
        (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, cmv));
    if (mask) {
      const unsigned idx = (unsigned)__builtin_ctz(mask);
      const unsigned before = (1u << idx) - 1u;
      nl += (unsigned)__builtin_popcount(nlmask & before);
      cm += (unsigned)__builtin_popcount(cmmask & before);
      *newlines += nl;
      *commas += cm;
      return p + idx;
    }
    nl += (unsigned)__builtin_popcount(nlmask);
    cm += (unsigned)__builtin_popcount(cmmask);
    p += 32;
  }
  *newlines += nl;
  *commas += cm;
  return lu_simd_scan_scalar(p, end, newlines, commas); /* <32-byte tail */
}
#endif /* __GNUC__ || __clang__ */

const char *lu_simd_scan_array_structural(const char *p, const char *end,
                                          size_t *newlines, size_t *commas) {
#if defined(LIGHTUSD_SIMDSCAN_AVX2_DISPATCH)
  if (__builtin_cpu_supports("avx2"))
    return lu_simd_scan_array_avx2(p, end, newlines, commas);
#endif
  {
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
      const __m128i v = _mm_loadu_si128((const __m128i *)(const void *)p);
      __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, lb), _mm_cmpeq_epi8(v, rb));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(v, dq));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(v, sq));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(v, at));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(v, hs));
      const unsigned mask = (unsigned)_mm_movemask_epi8(m) & 0xFFFFu;
      const unsigned nlmask =
          (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, nlv)) & 0xFFFFu;
      const unsigned cmmask =
          (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, cmv)) & 0xFFFFu;
      if (mask) {
        const unsigned idx = lu_simd_lowest_set_bit(mask);
        const unsigned before = (1u << idx) - 1u;
        nl += lu_simd_popcount16(nlmask & before);
        cm += lu_simd_popcount16(cmmask & before);
        *newlines += nl;
        *commas += cm;
        return p + idx;
      }
      if (nlmask) nl += lu_simd_popcount16(nlmask);
      if (cmmask) cm += lu_simd_popcount16(cmmask);
      p += 16;
    }
    *newlines += nl;
    *commas += cm;
  }
  return lu_simd_scan_scalar(p, end, newlines, commas); /* <16-byte tail */
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,popcnt")))
static const char *lu_simd_scan_prim_avx2(const char *p, const char *end,
                                          size_t *newlines) {
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
    const __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)p);
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
    const unsigned mask = (unsigned)_mm256_movemask_epi8(m);
    const unsigned nlmask =
        (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, nlv));
    if (mask) {
      const unsigned idx = (unsigned)__builtin_ctz(mask);
      const unsigned before = (1u << idx) - 1u;
      nl += (unsigned)__builtin_popcount(nlmask & before);
      *newlines += nl;
      return p + idx;
    }
    nl += (unsigned)__builtin_popcount(nlmask);
    p += 32;
  }
  *newlines += nl;
  return lu_simd_scan_prim_scalar(p, end, newlines); /* <32-byte tail */
}
#endif /* __GNUC__ || __clang__ */

const char *lu_simd_scan_prim_structural(const char *p, const char *end,
                                         size_t *newlines) {
#if defined(LIGHTUSD_SIMDSCAN_AVX2_DISPATCH)
  if (__builtin_cpu_supports("avx2"))
    return lu_simd_scan_prim_avx2(p, end, newlines);
#endif
  {
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
      const __m128i v = _mm_loadu_si128((const __m128i *)(const void *)p);
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
      const unsigned mask = (unsigned)_mm_movemask_epi8(m) & 0xFFFFu;
      const unsigned nlmask =
          (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, nlv)) & 0xFFFFu;
      if (mask) {
        const unsigned idx = lu_simd_lowest_set_bit(mask);
        nl += lu_simd_popcount16(nlmask & ((1u << idx) - 1u));
        *newlines += nl;
        return p + idx;
      }
      if (nlmask) nl += lu_simd_popcount16(nlmask);
      p += 16;
    }
    *newlines += nl;
  }
  return lu_simd_scan_prim_scalar(p, end, newlines); /* <16-byte tail */
}

const char *lu_simd_scan_backend(void) { return "sse2"; }

#elif defined(LIGHTUSD_SIMDSCAN_NEON)

const char *lu_simd_scan_array_structural(const char *p, const char *end,
                                          size_t *newlines, size_t *commas) {
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
    const uint8x16_t v = vld1q_u8((const uint8_t *)(const void *)p);
    uint8x16_t m = vorrq_u8(vceqq_u8(v, lb), vceqq_u8(v, rb));
    m = vorrq_u8(m, vceqq_u8(v, dq));
    m = vorrq_u8(m, vceqq_u8(v, sq));
    m = vorrq_u8(m, vceqq_u8(v, at));
    m = vorrq_u8(m, vceqq_u8(v, hs));

    const uint64_t mmask = vget_lane_u64(
        vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
    if (mmask) {
#if defined(_MSC_VER)
      unsigned long bidx;
      _BitScanForward64(&bidx, mmask);
      const int idx = (int)(bidx >> 2);
#else
      const int idx = (int)(__builtin_ctzll(mmask) >> 2);
#endif
      for (int i = 0; i < idx; ++i) {
        if (p[i] == '\n') ++nl;
        if (p[i] == ',') ++cm;
      }
      *newlines += nl;
      *commas += cm;
      return p + idx;
    }
    nl += vaddvq_u8(vandq_u8(vceqq_u8(v, nlv), one));
    cm += vaddvq_u8(vandq_u8(vceqq_u8(v, cmv), one));
    p += 16;
  }
  *newlines += nl;
  *commas += cm;
  return lu_simd_scan_scalar(p, end, newlines, commas); /* <16-byte tail */
}

const char *lu_simd_scan_prim_structural(const char *p, const char *end,
                                         size_t *newlines) {
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
    const uint8x16_t v = vld1q_u8((const uint8_t *)(const void *)p);
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
      const int idx = (int)(bidx >> 2);
#else
      const int idx = (int)(__builtin_ctzll(mmask) >> 2);
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
  return lu_simd_scan_prim_scalar(p, end, newlines); /* <16-byte tail */
}

const char *lu_simd_scan_backend(void) { return "neon"; }

#else /* no SIMD backend: scalar (WASM / disabled / other archs; identical) */

const char *lu_simd_scan_array_structural(const char *p, const char *end,
                                          size_t *newlines, size_t *commas) {
  return lu_simd_scan_scalar(p, end, newlines, commas);
}

const char *lu_simd_scan_prim_structural(const char *p, const char *end,
                                         size_t *newlines) {
  return lu_simd_scan_prim_scalar(p, end, newlines);
}

const char *lu_simd_scan_backend(void) { return "scalar"; }

#endif /* LIGHTUSD_SIMDSCAN_SSE2 */
