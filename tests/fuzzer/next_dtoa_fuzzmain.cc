// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// libFuzzer harness: src/next float->string formatter (writer/dtoa), including
// the zmij usdcat SIMD fast path.
//
// Build (clang): cmake -S src/next -B build-fuzz -DLIGHTUSD_NEXT_BUILD_FUZZERS=ON
// No seed corpus needed -- the input bytes ARE the float/double bit patterns, so
// the fuzzer directly explores nan/inf/subnormal/huge/tiny/edge values.
//
// Invariants checked on every value (violations -> abort, i.e. a libFuzzer find):
//   * memory safety: dtos_to() into an EXACT kDtoaBufSize heap buffer, so ASAN's
//     redzone catches any SIMD 16-byte overshoot past the documented capacity;
//   * the three APIs (dtos / dtos_to / dtos_append) agree byte-for-byte;
//   * the fast path is byte-identical to the dragonbox-only reference
//     formatter (dtos_to_reference), i.e. USDA output is unchanged;
//   * round-trip: every finite value parses back to exactly itself (bit-equal),
//     which catches any wrong-digit bug in the fast path OR the fallback;
//   * bounded output length.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "next/writer/dtoa.hh"

namespace {

using lightusd::next::dtos;
using lightusd::next::dtos_append;
using lightusd::next::dtos_to;
using lightusd::next::dtos_to_reference;
using lightusd::next::format_g;
using lightusd::next::kDtoaBufSize;

inline void require(bool cond, const char *msg) {
  if (!cond) {
    std::fprintf(stderr, "[fuzz_next_dtoa] invariant violated: %s\n", msg);
    std::abort();
  }
}

template <typename T>
void exercise(T v, T (*parse)(const char *, char **)) {
  // Format into an EXACT-size heap buffer: ASAN's redzone then catches any write
  // past kDtoaBufSize (the zmij fast path stores 16-byte chunks past the logical
  // end -- this is the documented reason kDtoaBufSize > the longest string).
  char *buf = new char[kDtoaBufSize];
  std::size_t n = dtos_to(buf, v);
  require(n <= kDtoaBufSize, "dtos_to length > kDtoaBufSize");
  std::string s_to(buf, n);
  delete[] buf;

  // All three public entry points must produce identical bytes.
  std::string s_ret = dtos(v);
  std::string s_app;
  dtos_append(s_app, v);
  require(s_to == s_ret, "dtos_to != dtos");
  require(s_to == s_app, "dtos_to != dtos_append");

  // Fast path must match the dragonbox-only reference byte-for-byte.
  char ref[kDtoaBufSize];
  std::size_t rn = dtos_to_reference(ref, v);
  require(s_to == std::string(ref, rn), "dtos_to != dtos_to_reference");

  // No '+' sign and no null bytes in usdcat notation.
  require(s_ret.find('+') == std::string::npos, "unexpected '+' in output");
  require(s_ret.find('\0') == std::string::npos, "embedded NUL in output");

  // Round-trip: finite values must parse back to exactly the same bits.
  if (std::isfinite(v)) {
    T back = parse(s_ret.c_str(), nullptr);
    require(std::memcmp(&back, &v, sizeof(T)) == 0, "round-trip not bit-exact");
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
  std::size_t i = 0;

  // Consume 8-byte chunks as doubles.
  for (; i + 8 <= size; i += 8) {
    double d;
    std::memcpy(&d, data + i, sizeof(d));
    exercise<double>(d, std::strtod);

    // Also fuzz the %g formatter (layer-meta/time-sample doubles), with a
    // precision derived from the input. Its output is bounded and NUL-free.
    int precision = 1 + static_cast<int>(data[i] % 17u);
    std::string g = format_g(d, precision);
    require(g.size() < kDtoaBufSize, "format_g output too long");
  }

  // Consume the remaining 4-byte chunks as floats.
  for (; i + 4 <= size; i += 4) {
    float f;
    std::memcpy(&f, data + i, sizeof(f));
    exercise<float>(f, std::strtof);
  }

  return 0;
}
