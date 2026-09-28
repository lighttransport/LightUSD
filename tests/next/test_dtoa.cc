// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA float formatter (writer/dtoa) equivalence test.
//
// dtos_to() takes a zmij fast path (src/external/zmij) for the common
// fixed-notation window and falls back to the dragonbox renderer otherwise. The
// USDA writer's output must stay byte-identical, so this test checks, via the
// public API:
//   1. Notation contract -- curated value -> exact usdcat string.
//   2. Equivalence       -- dtos_to() == dtos_to_reference() (the dragonbox-only
//                           formatter the writer used before the fast path) for
//                           a large deterministic sample of floats and doubles:
//                           every binary exponent with random mantissas, all
//                           subnormal/special/boundary patterns, integers,
//                           short decimals (k / 10^n), realistic coordinates and
//                           random bit patterns.
//   3. Round-trip        -- the formatted string parses back to the same bits.
//   4. API agreement     -- dtos(), dtos_to(), dtos_append() agree.
//   5. Buffer safety     -- the fast path never writes past kDtoaBufSize.
//
// `test_dtoa exhaustive` additionally sweeps all 2^32 float bit patterns
// (registered as ctest next_test_dtoa_exhaustive when
// LIGHTUSD_NEXT_DTOA_EXHAUSTIVE_TEST=ON; multithreaded when built with
// LIGHTUSD_ENABLE_THREAD).

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif

#include "next/writer/dtoa.hh"

using lightusd::next::dtos;
using lightusd::next::dtos_append;
using lightusd::next::dtos_to;
using lightusd::next::dtos_to_reference;
using lightusd::next::kDtoaBufSize;

namespace {

std::atomic<uint64_t> g_failures{0};
std::atomic<uint64_t> g_checked{0};  // flushed from t_checked per thread
thread_local uint64_t t_checked = 0;
constexpr uint64_t kMaxReported = 20;

void report_failure() { g_failures.fetch_add(1, std::memory_order_relaxed); }
bool should_print() {
  return g_failures.load(std::memory_order_relaxed) < kMaxReported;
}

#define CHECK_EQ_STR(got, want)                                             \
  do {                                                                      \
    std::string g_ = (got), w_ = (want);                                    \
    if (g_ != w_) {                                                         \
      std::printf("  FAIL %s:%d  got='%s' want='%s'\n", __FILE__, __LINE__, \
                  g_.c_str(), w_.c_str());                                  \
      report_failure();                                                     \
    }                                                                       \
  } while (0)

inline uint64_t splitmix64(uint64_t& s) {
  uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

inline float f_from_bits(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
inline double d_from_bits(uint64_t b) {
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
inline uint32_t bits_of(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
inline uint64_t bits_of(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}

// Core per-value check. `full` adds the (slower) round-trip / API / canary
// checks; the exhaustive sweep only needs the equivalence comparison.
template <typename T>
void check_value(T v, bool full = true) {
  ++t_checked;

  // Canary after an exact kDtoaBufSize buffer: the fast path's 8/16-byte
  // stores must stay inside it.
  struct {
    char buf[kDtoaBufSize];
    unsigned char canary[16];
  } g;
  std::memset(g.canary, 0xAB, sizeof(g.canary));
  const size_t n = dtos_to(g.buf, v);
  char rbuf[kDtoaBufSize];
  const size_t rn = dtos_to_reference(rbuf, v);

  if (n != rn || std::memcmp(g.buf, rbuf, n) != 0) {
    if (should_print()) {
      std::printf("  FAIL equivalence (%s bits 0x%llx): fast='%.*s' ref='%.*s'\n",
                  sizeof(T) == 4 ? "float" : "double",
                  (unsigned long long)bits_of(v), (int)n, g.buf, (int)rn, rbuf);
    }
    report_failure();
    return;
  }
  if (!full) return;

  for (unsigned char c : g.canary) {
    if (c != 0xAB) {
      if (should_print())
        std::printf("  FAIL buffer overshoot past kDtoaBufSize (bits 0x%llx)\n",
                    (unsigned long long)bits_of(v));
      report_failure();
      break;
    }
  }

  const std::string s_to(g.buf, n);
  std::string s_app;
  dtos_append(s_app, v);
  if (dtos(v) != s_to || s_app != s_to) {
    if (should_print())
      std::printf("  FAIL api-agree: to='%s' ret='%s' app='%s'\n", s_to.c_str(),
                  dtos(v).c_str(), s_app.c_str());
    report_failure();
  }

  if (std::isfinite(v)) {
    const T back = sizeof(T) == 4 ? T(std::strtof(s_to.c_str(), nullptr))
                                  : T(std::strtod(s_to.c_str(), nullptr));
    if (bits_of(back) != bits_of(v)) {
      if (should_print())
        std::printf("  FAIL roundtrip '%s' (bits 0x%llx)\n", s_to.c_str(),
                    (unsigned long long)bits_of(v));
      report_failure();
    }
  }
}

// Both signs of a value.
template <typename T>
void check_pm(T v) {
  check_value(v);
  check_value(-v);
}

// ---- 1. notation contract ----
void test_notation() {
  CHECK_EQ_STR(dtos(0.0), "0");
  CHECK_EQ_STR(dtos(-0.0), "-0");
  CHECK_EQ_STR(dtos(1.0), "1");
  CHECK_EQ_STR(dtos(-1.0), "-1");
  CHECK_EQ_STR(dtos(100.0), "100");
  CHECK_EQ_STR(dtos(0.3), "0.3");
  CHECK_EQ_STR(dtos(1.5), "1.5");
  CHECK_EQ_STR(dtos(13.944), "13.944");
  CHECK_EQ_STR(dtos(3.14159), "3.14159");
  CHECK_EQ_STR(dtos(0.0001), "0.0001");
  CHECK_EQ_STR(dtos(1e-6), "0.000001");  // fixed (boundary)
  CHECK_EQ_STR(dtos(1e-7), "1e-7");      // scientific, unpadded, no '+'
  CHECK_EQ_STR(dtos(1e14), "100000000000000");
  CHECK_EQ_STR(dtos(1e15), "1e15");  // scientific (>= 15)
  CHECK_EQ_STR(dtos(1e30), "1e30");
  CHECK_EQ_STR(dtos(-1e-30), "-1e-30");
  CHECK_EQ_STR(dtos(std::nan("")), "nan");
  CHECK_EQ_STR(dtos(std::numeric_limits<double>::infinity()), "inf");
  CHECK_EQ_STR(dtos(-std::numeric_limits<double>::infinity()), "-inf");
  CHECK_EQ_STR(dtos(0.0f), "0");
  CHECK_EQ_STR(dtos(-0.0f), "-0");
  CHECK_EQ_STR(dtos(1.0f), "1");
  CHECK_EQ_STR(dtos(-1.0f), "-1");
  CHECK_EQ_STR(dtos(100.0f), "100");
  CHECK_EQ_STR(dtos(0.3f), "0.3");
  CHECK_EQ_STR(dtos(1.5f), "1.5");
  CHECK_EQ_STR(dtos(0.1f), "0.1");
  CHECK_EQ_STR(dtos(1e-7f), "1e-7");
  CHECK_EQ_STR(dtos(1e15f), "1e15");
  CHECK_EQ_STR(dtos(std::nanf("")), "nan");
  CHECK_EQ_STR(dtos(-std::numeric_limits<float>::infinity()), "-inf");
}

// ---- 2. equivalence over a deterministic sample ----
void test_special_values() {
  const float fspecial[] = {
      0.0f, 1.0f, 2.0f, 0.5f, 10.0f, 0.1f, 0.2f, 0.3f, 1e-4f, 1e-5f, 1e-6f,
      1e-7f, 9.999999e-5f, 9.999999e-7f, 1e14f, 1e15f, 9.99999e14f, 1e16f,
      16777216.0f, 16777217.0f, 3.4028235e38f, 1.17549435e-38f, 1.4e-45f,
      std::numeric_limits<float>::max(), std::numeric_limits<float>::min(),
      std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::epsilon(),
      std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::signaling_NaN(), 3.14159265f, 2.71828183f,
      0.33333334f, 0.6666667f, 123456.79f, 1234567.9f, 12345678.0f};
  for (float v : fspecial) check_pm(v);
  const double dspecial[] = {
      0.0, 1.0, 2.0, 0.5, 10.0, 0.1, 0.2, 0.3, 1e-4, 1e-5, 1e-6, 1e-7,
      9.999999999999999e-5, 9.999999999999999e-7, 1e14, 1e15, 9.99999999999999e14,
      1e16, 1e17, 9007199254740992.0, 9007199254740993.0, 5e-324, 1e308,
      std::numeric_limits<double>::max(), std::numeric_limits<double>::min(),
      std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::epsilon(),
      std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::signaling_NaN(), 3.141592653589793,
      2.718281828459045, 1.0 / 3.0, 2.0 / 3.0, 13.944, 24.0, 23.976023976023978,
      0.041666666666666664, 1234567890123456.0, 123456789012345.67};
  for (double v : dspecial) check_pm(v);
  // NaN payloads.
  check_value(f_from_bits(0x7fc00001u));
  check_value(f_from_bits(0xffc12345u));
  check_value(d_from_bits(0x7ff8000000000001ULL));
  check_value(d_from_bits(0xfff0000000000001ULL));
}

void test_all_exponents(uint64_t per_exp) {
  uint64_t s = 0x5eed0001ULL;
  // float: every biased exponent (incl. 0 = subnormal and 255 = inf/nan).
  for (uint32_t e = 0; e < 256; ++e) {
    const uint32_t base = e << 23;
    // Mantissa boundaries.
    const uint32_t edges[] = {0u, 1u, 2u, 0x400000u, 0x7ffffeu, 0x7fffffu};
    for (uint32_t m : edges) check_pm(f_from_bits(base | m));
    for (uint64_t i = 0; i < per_exp; ++i) {
      const uint32_t m = uint32_t(splitmix64(s)) & 0x7fffffu;
      check_pm(f_from_bits(base | m));
    }
  }
  // double: every biased exponent (0..2047).
  for (uint64_t e = 0; e < 2048; ++e) {
    const uint64_t base = e << 52;
    const uint64_t edges[] = {0ULL, 1ULL, 2ULL, 1ULL << 51, (1ULL << 52) - 2,
                              (1ULL << 52) - 1};
    for (uint64_t m : edges) check_pm(d_from_bits(base | m));
    for (uint64_t i = 0; i < per_exp; ++i) {
      const uint64_t m = splitmix64(s) & ((1ULL << 52) - 1);
      check_pm(d_from_bits(base | m));
    }
  }
}

void test_subnormals(uint64_t n) {
  // Low float subnormal patterns exhaustively, plus random ones.
  for (uint32_t m = 1; m < 70000; ++m) check_value(f_from_bits(m));
  uint64_t s = 0x5eed0002ULL;
  for (uint64_t i = 0; i < n; ++i) {
    check_pm(f_from_bits(uint32_t(splitmix64(s)) & 0x7fffffu));
    check_pm(d_from_bits(splitmix64(s) & ((1ULL << 52) - 1)));
  }
}

void test_integers() {
  for (int i = 0; i <= 1000000; ++i) {
    check_value(float(i), i < 20000);
    check_value(double(i), i < 20000);
  }
  // Around the exact-integer limits and powers of ten / two.
  for (int k = -64; k <= 64; ++k) {
    check_pm(float(16777216 + k));
    check_pm(double(9007199254740992LL + k));
  }
  double p10 = 1.0;
  for (int k = 0; k <= 22; ++k, p10 *= 10.0) {
    check_pm(p10);
    check_pm(float(p10));
    check_pm(std::nextafter(p10, 0.0));
    check_pm(std::nextafter(p10, 1e300));
    check_pm(std::nextafter(float(p10), 0.0f));
    check_pm(std::nextafter(float(p10), 1e30f));
  }
  for (int k = -1074; k <= 1023; ++k) check_pm(std::ldexp(1.0, k));
  for (int k = -149; k <= 127; ++k) check_pm(std::ldexp(1.0f, k));
}

void test_short_decimals() {
  // k / 10^n: the values authors actually type (0.25, 1.5, 13.944, 1e-5 ...).
  double p10 = 1.0;
  for (int n = 0; n <= 16; ++n, p10 *= 10.0) {
    const int kmax = n <= 4 ? 20000 : 2000;
    for (int k = 1; k <= kmax; ++k) {
      const double d = double(k) / p10;
      check_value(d, (k % 7) == 0);
      check_value(float(d), (k % 7) == 0);
      check_value(double(float(d)), false);  // widened float (float attr -> double)
    }
  }
}

void test_random(uint64_t n) {
  uint64_t s = 0xDEADBEEFCAFEULL;
  for (uint64_t i = 0; i < n; ++i) {
    const uint64_t bits = splitmix64(s);
    check_value(d_from_bits(bits));
    check_value(f_from_bits(uint32_t(bits >> 32)));
    // Realistic ranges (the fast-path window): coords, normals, uvs.
    const double u = double(bits >> 11) * (1.0 / 9007199254740992.0);
    check_value(u * 2000.0 - 1000.0);
    check_value(float(u * 2.0 - 1.0));
    check_value(float(u * 20000.0 - 10000.0));
    check_value(float(u));
    check_value(float(u * 1e-3));
    check_value(u * 1e6);
  }
}

// ---- exhaustive float sweep (opt-in) ----
void sweep_float_range(uint64_t lo, uint64_t hi) {
  for (uint64_t i = lo; i < hi; ++i) check_value(f_from_bits(uint32_t(i)), false);
  g_checked.fetch_add(t_checked);
  t_checked = 0;
}

void test_float_exhaustive() {
  const uint64_t total = 1ULL << 32;
#if defined(LIGHTUSD_ENABLE_THREAD)
  unsigned nt = std::thread::hardware_concurrency();
  if (nt == 0) nt = 4;
  if (nt > 16) nt = 16;
  std::vector<std::thread> threads;
  const uint64_t step = total / nt;
  for (unsigned t = 0; t < nt; ++t) {
    const uint64_t lo = t * step;
    const uint64_t hi = (t + 1 == nt) ? total : lo + step;
    threads.emplace_back([lo, hi] { sweep_float_range(lo, hi); });
  }
  for (auto& th : threads) th.join();
#else
  const uint64_t chunk = total / 16;
  for (uint64_t c = 0; c < 16; ++c) {
    sweep_float_range(c * chunk, (c + 1) * chunk);
    std::printf("  ... %llu/16\n", (unsigned long long)(c + 1));
  }
#endif
}

}  // namespace

int main(int argc, char** argv) {
  const bool exhaustive = argc > 1 && std::string(argv[1]) == "exhaustive";

  std::printf("[test_dtoa] notation contract...\n");
  test_notation();
  std::printf("[test_dtoa] special values...\n");
  test_special_values();
  std::printf("[test_dtoa] every binary exponent x random mantissas...\n");
  test_all_exponents(/*per_exp=*/256);
  std::printf("[test_dtoa] subnormals...\n");
  test_subnormals(200000);
  std::printf("[test_dtoa] integers / powers...\n");
  test_integers();
  std::printf("[test_dtoa] short decimals...\n");
  test_short_decimals();
  std::printf("[test_dtoa] random bit patterns + realistic ranges...\n");
  test_random(500000);

  if (exhaustive) {
    std::printf("[test_dtoa] exhaustive all-2^32-float equivalence...\n");
    test_float_exhaustive();
  }

  g_checked.fetch_add(t_checked);
  const uint64_t fails = g_failures.load();
  std::printf("[test_dtoa] %llu values checked\n",
              (unsigned long long)g_checked.load());
  if (fails == 0) {
    std::printf("[test_dtoa] PASS\n");
    return 0;
  }
  std::printf("[test_dtoa] FAIL (%llu failures)\n", (unsigned long long)fails);
  return 1;
}
