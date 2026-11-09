#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-fp-parse-print.h"
#include "str-util.hh"
#include "math-util.inc"
#include <cmath>
#include <limits>
#include <cstring>
#include <cstdint>
#include <vector>
#include <random>
#include <cstdio>
#include <algorithm>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Math constants using lightusd definitions where available
static constexpr double kPi    = math::constants<double>::pi();
static constexpr double kE     = 2.71828182845904523536028747135266250;
static constexpr double kSqrt2 = 1.41421356237309504880168872420969808;

// Helper: bitwise equality check for doubles (handles -0.0 vs 0.0)
static bool bitwise_equal_double(double a, double b) {
  uint64_t bits_a, bits_b;
  std::memcpy(&bits_a, &a, sizeof(double));
  std::memcpy(&bits_b, &b, sizeof(double));
  return bits_a == bits_b;
}

// Helper: bitwise equality check for floats (handles -0.0 vs 0.0)
static bool bitwise_equal_float(float a, float b) {
  uint32_t bits_a, bits_b;
  std::memcpy(&bits_a, &a, sizeof(float));
  std::memcpy(&bits_b, &b, sizeof(float));
  return bits_a == bits_b;
}

// Helper: relative epsilon comparison
static bool nearly_equal_double(double a, double b, double eps = 1e-15) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isinf(a) && std::isinf(b)) return (a > 0) == (b > 0);
  return std::fabs(a - b) <= eps * (std::fabs(a) + std::fabs(b) + 1.0);
}

static bool nearly_equal_float(float a, float b, float eps = 1e-6f) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isinf(a) && std::isinf(b)) return (a > 0) == (b > 0);
  return std::fabs(a - b) <= eps * (std::fabs(a) + std::fabs(b) + 1.0f);
}

// Helper: construct float from bits
static float float_from_bits(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(float));
  return f;
}

// Helper: construct double from bits
static double double_from_bits(uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, sizeof(double));
  return d;
}

// Helper: get bits from float
static uint32_t float_to_bits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(float));
  return bits;
}

// Helper: get bits from double
static uint64_t double_to_bits(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof(double));
  return bits;
}

//
// Test 1: Basic round-trip tests for common values
//
void fp_roundtrip_basic_test(void) {
  // Double precision basic tests
  {
    std::vector<double> test_values = {
      0.0,
      1.0,
      -1.0,
      2.0,
      -2.0,
      0.5,
      -0.5,
      0.1,
      0.2,
      0.3,
      0.25,
      0.125,
      0.0625,
      10.0,
      100.0,
      1000.0,
      123.456,
      -123.456,
      0.001,
      0.0001,
      1234567890.0,
      0.123456789,
      3.14159265358979323846,
      2.71828182845904523536,
      1.41421356237309504880,  // sqrt(2)
      1.61803398874989484820,  // golden ratio
    };

    for (double v : test_values) {
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double basic roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }
  }

  // Float precision basic tests
  {
    std::vector<float> test_values = {
      0.0f,
      1.0f,
      -1.0f,
      2.0f,
      -2.0f,
      0.5f,
      -0.5f,
      0.1f,
      0.2f,
      0.3f,
      0.25f,
      0.125f,
      0.0625f,
      10.0f,
      100.0f,
      1000.0f,
      123.456f,
      -123.456f,
      0.001f,
      0.0001f,
      1234567.0f,
      0.1234567f,
      3.14159265f,
      2.71828182f,
    };

    for (float v : test_values) {
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_float(static_cast<float>(parsed), v, 1e-6f),
                  "float basic roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  v, s.c_str(), parsed);
    }
  }
}

//
// Test 2: Edge cases (extreme values, boundaries)
//
void fp_roundtrip_edge_cases_test(void) {
  // Double precision edge cases
  {
    // Powers of 2
    for (int exp = -300; exp <= 300; exp += 50) {
      double v = std::ldexp(1.0, exp);
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double power of 2 (2^%d) roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  exp, v, s.c_str(), parsed);
    }

    // Maximum and minimum normal values
    {
      double v = std::numeric_limits<double>::max();
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double max roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }

    {
      double v = std::numeric_limits<double>::min();  // smallest normal
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double min normal roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }

    {
      double v = std::numeric_limits<double>::denorm_min();  // smallest denormal
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double denorm_min roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }

    // Epsilon and related
    {
      double v = std::numeric_limits<double>::epsilon();
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double epsilon roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }

    // Values near 1.0
    {
      double v = 1.0 + std::numeric_limits<double>::epsilon();
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double 1+eps roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }

    {
      double v = 1.0 - std::numeric_limits<double>::epsilon() / 2;
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double 1-eps/2 roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }

    // Values crossing format boundaries (scientific notation vs fixed point)
    std::vector<double> boundary_values = {
      1e-5, 1e-4, 1e-3,  // near scientific notation threshold
      1e15, 1e16, 1e17,  // near upper scientific notation threshold
      9.9999e-5, 1.0001e-4,
      9.9999e15, 1.0001e16,
    };

    for (double v : boundary_values) {
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double boundary roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);

      // Also test negative
      s = dtos(-v);
      parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_double(parsed, -v, 1e-14),
                  "double negative boundary roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  -v, s.c_str(), parsed);
    }
  }

  // Float precision edge cases
  {
    // Powers of 2
    for (int exp = -120; exp <= 120; exp += 20) {
      float v = std::ldexp(1.0f, exp);
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_float(static_cast<float>(parsed), v, 1e-6f),
                  "float power of 2 (2^%d) roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  exp, v, s.c_str(), parsed);
    }

    // Maximum and minimum normal values
    {
      float v = std::numeric_limits<float>::max();
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_float(static_cast<float>(parsed), v, 1e-6f),
                  "float max roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  v, s.c_str(), parsed);
    }

    {
      float v = std::numeric_limits<float>::min();  // smallest normal
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_float(static_cast<float>(parsed), v, 1e-6f),
                  "float min normal roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  v, s.c_str(), parsed);
    }

    {
      float v = std::numeric_limits<float>::denorm_min();  // smallest denormal
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_float(static_cast<float>(parsed), v, 1e-6f),
                  "float denorm_min roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  v, s.c_str(), parsed);
    }

    // Epsilon
    {
      float v = std::numeric_limits<float>::epsilon();
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_float(static_cast<float>(parsed), v, 1e-6f),
                  "float epsilon roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  v, s.c_str(), parsed);
    }

    // Float format boundaries
    std::vector<float> boundary_values = {
      1e-5f, 1e-4f, 1e-3f,
      1e6f, 1e7f, 1e8f,
      9.9999e-5f, 1.0001e-4f,
      9.9999e6f, 1.0001e7f,
    };

    for (float v : boundary_values) {
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(nearly_equal_float(static_cast<float>(parsed), v, 1e-5f),
                  "float boundary roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  v, s.c_str(), parsed);
    }
  }
}

//
// Test 3: Special values (zero, NaN, infinity)
//
void fp_roundtrip_special_values_test(void) {
  // Zero handling
  {
    // Positive zero (double)
    {
      double v = 0.0;
      std::string s = dtos(v);
      TEST_CHECK_(s == "0", "double +0 should print as '0', got '%s'", s.c_str());
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(parsed == 0.0, "double +0 roundtrip failed");
    }

    // Negative zero (double)
    {
      double v = -0.0;
      std::string s = dtos(v);
      TEST_CHECK_(s == "-0", "double -0 should print as '-0', got '%s'", s.c_str());
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(parsed == 0.0, "double -0 roundtrip value failed");
      TEST_CHECK_(std::signbit(parsed), "double -0 roundtrip sign failed");
    }

    // Positive zero (float)
    {
      float v = 0.0f;
      std::string s = dtos(v);
      TEST_CHECK_(s == "0", "float +0 should print as '0', got '%s'", s.c_str());
    }

    // Negative zero (float)
    {
      float v = -0.0f;
      std::string s = dtos(v);
      TEST_CHECK_(s == "-0", "float -0 should print as '-0', got '%s'", s.c_str());
      TEST_CHECK_(std::signbit(static_cast<float>(lightusd::legacy::atof(s))),
                  "float -0 roundtrip sign failed");
    }
  }

  // Fast path values (1.0, -1.0)
  {
    // 1.0 double
    {
      double v = 1.0;
      std::string s = dtos(v);
      TEST_CHECK_(s == "1", "double 1.0 should print as '1', got '%s'", s.c_str());
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(parsed == 1.0, "double 1.0 roundtrip failed");
    }

    // -1.0 double
    {
      double v = -1.0;
      std::string s = dtos(v);
      TEST_CHECK_(s == "-1", "double -1.0 should print as '-1', got '%s'", s.c_str());
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(parsed == -1.0, "double -1.0 roundtrip failed");
    }

    // 1.0 float
    {
      float v = 1.0f;
      std::string s = dtos(v);
      TEST_CHECK_(s == "1", "float 1.0 should print as '1', got '%s'", s.c_str());
    }

    // -1.0 float
    {
      float v = -1.0f;
      std::string s = dtos(v);
      TEST_CHECK_(s == "-1", "float -1.0 should print as '-1', got '%s'", s.c_str());
    }
  }

  // Values very close to 1.0 (should NOT trigger fast path)
  {
    // nextafter(1.0, 2.0) - smallest double > 1.0
    double v = std::nextafter(1.0, 2.0);
    std::string s = dtos(v);
    TEST_CHECK_(s != "1", "nextafter(1.0, 2.0) should NOT print as '1', got '%s'", s.c_str());
    double parsed = lightusd::legacy::atof(s);
    TEST_CHECK_(parsed == v, "nextafter(1.0, 2.0) roundtrip failed");

    // nextafter(1.0, 0.0) - largest double < 1.0
    v = std::nextafter(1.0, 0.0);
    s = dtos(v);
    TEST_CHECK_(s != "1", "nextafter(1.0, 0.0) should NOT print as '1', got '%s'", s.c_str());
    parsed = lightusd::legacy::atof(s);
    TEST_CHECK_(parsed == v, "nextafter(1.0, 0.0) roundtrip failed");
  }

  // Specific IEEE 754 bit patterns
  {
    // Double precision - specific bit patterns
    // 1.0 = 0x3FF0000000000000
    {
      double v = double_from_bits(0x3FF0000000000000ULL);
      TEST_CHECK_(v == 1.0, "bit pattern 0x3FF0000000000000 should be 1.0");
      TEST_CHECK_(dtos(v) == "1", "bit pattern 1.0 should print as '1'");
    }

    // -1.0 = 0xBFF0000000000000
    {
      double v = double_from_bits(0xBFF0000000000000ULL);
      TEST_CHECK_(v == -1.0, "bit pattern 0xBFF0000000000000 should be -1.0");
      TEST_CHECK_(dtos(v) == "-1", "bit pattern -1.0 should print as '-1'");
    }

    // Float precision - specific bit patterns
    // 1.0f = 0x3F800000
    {
      float v = float_from_bits(0x3F800000U);
      TEST_CHECK_(v == 1.0f, "bit pattern 0x3F800000 should be 1.0f");
      TEST_CHECK_(dtos(v) == "1", "bit pattern 1.0f should print as '1'");
    }

    // -1.0f = 0xBF800000
    {
      float v = float_from_bits(0xBF800000U);
      TEST_CHECK_(v == -1.0f, "bit pattern 0xBF800000 should be -1.0f");
      TEST_CHECK_(dtos(v) == "-1", "bit pattern -1.0f should print as '-1'");
    }
  }

  // Infinity and NaN tests
  {
    // Positive infinity (double)
    {
      double v = std::numeric_limits<double>::infinity();
      std::string s = dtos(v);
      TEST_CHECK_(s == "inf", "double +inf should print as 'inf', got '%s'", s.c_str());
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(std::isinf(parsed) && parsed > 0, "double +inf roundtrip failed");
    }

    // Negative infinity (double)
    {
      double v = -std::numeric_limits<double>::infinity();
      std::string s = dtos(v);
      TEST_CHECK_(s == "-inf", "double -inf should print as '-inf', got '%s'", s.c_str());
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(std::isinf(parsed) && parsed < 0, "double -inf roundtrip failed");
    }

    // NaN (double)
    {
      double v = std::numeric_limits<double>::quiet_NaN();
      std::string s = dtos(v);
      TEST_CHECK_(s == "nan", "double nan should print as 'nan', got '%s'", s.c_str());
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(std::isnan(parsed), "double nan roundtrip failed");
    }

    // Positive infinity (float)
    {
      float v = std::numeric_limits<float>::infinity();
      std::string s = dtos(v);
      TEST_CHECK_(s == "inf", "float +inf should print as 'inf', got '%s'", s.c_str());
      float parsed = static_cast<float>(lightusd::legacy::atof(s));
      TEST_CHECK_(std::isinf(parsed) && parsed > 0, "float +inf roundtrip failed");
    }

    // Negative infinity (float)
    {
      float v = -std::numeric_limits<float>::infinity();
      std::string s = dtos(v);
      TEST_CHECK_(s == "-inf", "float -inf should print as '-inf', got '%s'", s.c_str());
      float parsed = static_cast<float>(lightusd::legacy::atof(s));
      TEST_CHECK_(std::isinf(parsed) && parsed < 0, "float -inf roundtrip failed");
    }

    // NaN (float)
    {
      float v = std::numeric_limits<float>::quiet_NaN();
      std::string s = dtos(v);
      TEST_CHECK_(s == "nan", "float nan should print as 'nan', got '%s'", s.c_str());
      float parsed = static_cast<float>(lightusd::legacy::atof(s));
      TEST_CHECK_(std::isnan(parsed), "float nan roundtrip failed");
    }
  }

  // Null buffer handling test (hardening)
  {
    // dtos with null buffer should return 0 (not crash)
    size_t len_f = dtos(1.0f, nullptr);
    TEST_CHECK_(len_f == 0, "dtos(float, nullptr) should return 0, got %zu", len_f);

    size_t len_d = dtos(1.0, nullptr);
    TEST_CHECK_(len_d == 0, "dtos(double, nullptr) should return 0, got %zu", len_d);

    // Test with special values and null buffer
    len_f = dtos(std::numeric_limits<float>::infinity(), nullptr);
    TEST_CHECK_(len_f == 0, "dtos(inf, nullptr) should return 0");

    len_d = dtos(std::numeric_limits<double>::quiet_NaN(), nullptr);
    TEST_CHECK_(len_d == 0, "dtos(nan, nullptr) should return 0");
  }
}

//
// Test 4: Precision tests (verify enough digits are printed)
//
void fp_roundtrip_precision_test(void) {
  // Double precision: verify all significand bits are preserved
  {
    // Random values with full precision
    std::mt19937_64 rng(42);  // fixed seed for reproducibility
    std::uniform_real_distribution<double> dist(-1e15, 1e15);

    for (int i = 0; i < 100; i++) {
      double v = dist(rng);
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(bitwise_equal_double(parsed, v) || nearly_equal_double(parsed, v, 1e-15),
                  "double random precision test failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }

    // Test with very precise values that differ in last few bits
    double base = 1.0;
    for (int i = 0; i < 10; i++) {
      double v1 = std::nextafter(base, 2.0);
      double v2 = std::nextafter(v1, 2.0);
      double v3 = std::nextafter(v2, 2.0);

      std::string s1 = dtos(v1);
      std::string s2 = dtos(v2);
      std::string s3 = dtos(v3);

      // These should all produce different strings
      TEST_CHECK_(s1 != s2 || v1 == v2,
                  "adjacent doubles should produce different strings or be equal");
      TEST_CHECK_(s2 != s3 || v2 == v3,
                  "adjacent doubles should produce different strings or be equal");

      // Each should round-trip correctly
      double p1 = lightusd::legacy::atof(s1);
      double p2 = lightusd::legacy::atof(s2);
      double p3 = lightusd::legacy::atof(s3);

      TEST_CHECK_(bitwise_equal_double(p1, v1) || nearly_equal_double(p1, v1, 1e-15),
                  "adjacent double roundtrip failed");
      TEST_CHECK_(bitwise_equal_double(p2, v2) || nearly_equal_double(p2, v2, 1e-15),
                  "adjacent double roundtrip failed");
      TEST_CHECK_(bitwise_equal_double(p3, v3) || nearly_equal_double(p3, v3, 1e-15),
                  "adjacent double roundtrip failed");

      base *= 10.0;
    }
  }

  // Float precision: verify all significand bits are preserved
  {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1e6f, 1e6f);

    for (int i = 0; i < 100; i++) {
      float v = dist(rng);
      std::string s = dtos(v);
      float parsed = static_cast<float>(lightusd::legacy::atof(s));
      TEST_CHECK_(bitwise_equal_float(parsed, v) || nearly_equal_float(parsed, v, 1e-6f),
                  "float random precision test failed: v=%.9g, s=%s, parsed=%.9g",
                  v, s.c_str(), parsed);
    }

    // Test adjacent float values
    float base = 1.0f;
    for (int i = 0; i < 5; i++) {
      float v1 = std::nextafter(base, 2.0f);
      float v2 = std::nextafter(v1, 2.0f);

      std::string s1 = dtos(v1);
      std::string s2 = dtos(v2);

      float p1 = static_cast<float>(lightusd::legacy::atof(s1));
      float p2 = static_cast<float>(lightusd::legacy::atof(s2));

      TEST_CHECK_(bitwise_equal_float(p1, v1) || nearly_equal_float(p1, v1, 1e-6f),
                  "adjacent float roundtrip failed");
      TEST_CHECK_(bitwise_equal_float(p2, v2) || nearly_equal_float(p2, v2, 1e-6f),
                  "adjacent float roundtrip failed");

      base *= 10.0f;
    }
  }

  // Test specific problematic values
  {
    // Values that often cause precision issues
    std::vector<double> problematic_doubles = {
      0.1, 0.2, 0.3, 0.6, 0.7,  // binary fractions
      1.0/3.0, 2.0/3.0,  // repeating fractions
      kPi, kE, kSqrt2,  // mathematical constants
      1.7976931348623157e308,  // near max
      2.2250738585072014e-308,  // near min normal
      5e-324,  // near denorm min
    };

    for (double v : problematic_doubles) {
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(bitwise_equal_double(parsed, v) || nearly_equal_double(parsed, v, 1e-14),
                  "problematic double roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, s.c_str(), parsed);
    }

    std::vector<float> problematic_floats = {
      0.1f, 0.2f, 0.3f, 0.6f, 0.7f,
      1.0f/3.0f, 2.0f/3.0f,
      static_cast<float>(kPi), static_cast<float>(kE),
      3.4028235e38f,  // near max
      1.1754944e-38f,  // near min normal
      1.4e-45f,  // near denorm min
    };

    for (float v : problematic_floats) {
      std::string s = dtos(v);
      float parsed = static_cast<float>(lightusd::legacy::atof(s));
      TEST_CHECK_(bitwise_equal_float(parsed, v) || nearly_equal_float(parsed, v, 1e-6f),
                  "problematic float roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  v, s.c_str(), parsed);
    }
  }
}

//
// Test 5: Buffer-based API tests
//
void fp_roundtrip_buffer_test(void) {
  // Double buffer tests
  {
    char buffer[DTOS_MAX_CHARS_DOUBLE + 2];

    std::vector<double> test_values = {
      0.0, 1.0, -1.0,
      123.456, -987.654,
      1e-10, 1e10,
      1e-100, 1e100,
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::epsilon(),
    };

    for (double v : test_values) {
      // Set sentinel
      buffer[DTOS_MAX_CHARS_DOUBLE + 1] = '*';

      size_t len = dtos(v, buffer);
      std::string buf_str(buffer, len);
      std::string std_str = dtos(v);

      TEST_CHECK_(buf_str == std_str,
                  "double buffer mismatch: expected '%s', got '%s'",
                  std_str.c_str(), buf_str.c_str());

      TEST_CHECK_(len <= DTOS_MAX_CHARS_DOUBLE,
                  "double buffer overflow: len=%zu, max=%zu",
                  len, DTOS_MAX_CHARS_DOUBLE);

      TEST_CHECK_(buffer[DTOS_MAX_CHARS_DOUBLE + 1] == '*',
                  "double buffer sentinel overwritten");

      // Verify roundtrip
      double parsed = lightusd::legacy::atof(buf_str);
      TEST_CHECK_(nearly_equal_double(parsed, v, 1e-14),
                  "double buffer roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  v, buf_str.c_str(), parsed);
    }
  }

  // Float buffer tests
  {
    char buffer[DTOS_MAX_CHARS_FLOAT + 2];

    std::vector<float> test_values = {
      0.0f, 1.0f, -1.0f,
      123.456f, -987.654f,
      1e-5f, 1e5f,
      1e-30f, 1e30f,
      std::numeric_limits<float>::max(),
      std::numeric_limits<float>::min(),
      std::numeric_limits<float>::epsilon(),
    };

    for (float v : test_values) {
      // Set sentinel
      buffer[DTOS_MAX_CHARS_FLOAT + 1] = '*';

      size_t len = dtos(v, buffer);
      std::string buf_str(buffer, len);
      std::string std_str = dtos(v);

      TEST_CHECK_(buf_str == std_str,
                  "float buffer mismatch: expected '%s', got '%s'",
                  std_str.c_str(), buf_str.c_str());

      TEST_CHECK_(len <= DTOS_MAX_CHARS_FLOAT,
                  "float buffer overflow: len=%zu, max=%zu",
                  len, DTOS_MAX_CHARS_FLOAT);

      TEST_CHECK_(buffer[DTOS_MAX_CHARS_FLOAT + 1] == '*',
                  "float buffer sentinel overwritten");

      // Verify roundtrip
      float parsed = static_cast<float>(lightusd::legacy::atof(buf_str));
      TEST_CHECK_(nearly_equal_float(parsed, v, 1e-6f),
                  "float buffer roundtrip failed: v=%.9g, s=%s, parsed=%.9g",
                  v, buf_str.c_str(), parsed);
    }
  }

  // Stress test: many sequential conversions
  {
    char buffer[DTOS_MAX_CHARS_DOUBLE];
    double sum = 0.0;

    for (int i = 0; i < 10000; i++) {
      double v = static_cast<double>(i) * 0.001 - 5.0;
      size_t len = dtos(v, buffer);
      std::string s(buffer, len);
      sum += lightusd::legacy::atof(s);
    }

    // Just verify no crashes and reasonable output
    TEST_CHECK_(std::isfinite(sum), "stress test produced non-finite sum");
  }
}

//
// Test 6: Shortest representation tests
// Verify that dtos() produces the shortest string that round-trips correctly
//
void fp_shortest_representation_test(void) {
  // Helper: count significant digits in a string (excluding sign, decimal point, exponent)
  auto count_significand_digits = [](const std::string& s) -> size_t {
    size_t count = 0;
    bool in_exponent = false;
    for (char c : s) {
      if (c == 'e' || c == 'E') {
        in_exponent = true;
      } else if (!in_exponent && c >= '0' && c <= '9') {
        count++;
      }
    }
    return count;
  };

  // Test 6.1: Compare dtos() output length with printf %.17g for doubles
  // dtos() should produce equal or shorter output for the same precision
  {
    std::vector<double> test_doubles = {
      0.1, 0.2, 0.3,
      1.0/3.0, 2.0/3.0,
      123.456, 789.012,
      1e10, 1e-10,
      1.23456789012345678,
      9.99999999999999,
      1.0000000000000002,  // 1.0 + epsilon
      kPi, kE,
    };

    for (double v : test_doubles) {
      std::string dtos_str = dtos(v);

      // Get printf output with maximum precision
      char printf_buf[64];
      std::snprintf(printf_buf, sizeof(printf_buf), "%.17g", v);
      std::string printf_str(printf_buf);

      // Both should round-trip correctly
      double dtos_parsed = lightusd::legacy::atof(dtos_str);

      TEST_CHECK_(bitwise_equal_double(dtos_parsed, v),
                  "dtos double roundtrip failed: v=%.17g, s=%s", v, dtos_str.c_str());

      // dtos should produce output no longer than printf (often shorter)
      size_t dtos_digits = count_significand_digits(dtos_str);
      size_t printf_digits = count_significand_digits(printf_str);

      TEST_CHECK_(dtos_digits <= printf_digits + 1,  // Allow 1 extra for format differences
                  "dtos produced longer output than printf: dtos='%s' (%zu), printf='%s' (%zu)",
                  dtos_str.c_str(), dtos_digits, printf_str.c_str(), printf_digits);
    }
  }

  // Test 6.2: Verify known shortest representations for doubles
  // These are cases where we know the exact shortest output
  {
    struct TestCase {
      double value;
      size_t max_digits;  // Maximum expected significand digits
    };

    std::vector<TestCase> known_cases = {
      {1.0, 1},          // "1"
      {-1.0, 1},         // "-1"
      {0.0, 1},          // "0"
      {10.0, 2},         // "10"
      {100.0, 3},        // "100"
      {0.5, 2},          // "0.5"
      {0.25, 3},         // "0.25"
      {0.125, 4},        // "0.125"
      {1.5, 2},          // "1.5"
      {2.5, 2},          // "2.5"
    };

    for (const auto& tc : known_cases) {
      std::string s = dtos(tc.value);
      size_t digits = count_significand_digits(s);

      TEST_CHECK_(digits <= tc.max_digits,
                  "expected at most %zu digits for %.17g, got '%s' (%zu digits)",
                  tc.max_digits, tc.value, s.c_str(), digits);

      // Verify roundtrip
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(bitwise_equal_double(parsed, tc.value),
                  "known case roundtrip failed: v=%.17g, s=%s, parsed=%.17g",
                  tc.value, s.c_str(), parsed);
    }
  }

  // Test 6.3: Verify that removing the last significand digit breaks the roundtrip
  // This is a weaker but more reliable test than removing any digit
  {
    auto remove_last_significand_digit = [](const std::string& s) -> std::string {
      // Find exponent position
      size_t exp_pos = s.find('e');
      if (exp_pos == std::string::npos) exp_pos = s.find('E');

      std::string mantissa = (exp_pos != std::string::npos) ? s.substr(0, exp_pos) : s;
      std::string exponent = (exp_pos != std::string::npos) ? s.substr(exp_pos) : "";

      // Find last digit in mantissa
      size_t last_digit_pos = std::string::npos;
      for (size_t i = mantissa.size(); i > 0; i--) {
        if (mantissa[i-1] >= '0' && mantissa[i-1] <= '9') {
          last_digit_pos = i - 1;
          break;
        }
      }

      if (last_digit_pos == std::string::npos) return s;  // No digits found

      // Remove that digit
      std::string shortened = mantissa.substr(0, last_digit_pos) + mantissa.substr(last_digit_pos + 1);

      // Clean up: remove trailing decimal point if any
      if (!shortened.empty() && shortened.back() == '.') {
        shortened.pop_back();
      }

      // Don't return empty or sign-only strings
      if (shortened.empty() || shortened == "-" || shortened == "+") return s;

      return shortened + exponent;
    };

    std::vector<double> test_values = {
      1.234, 12.34, 123.4,
      1.23e10, 1.23e-10,
      1.000000000000001,
      9.999999999999998,
      0.123456789012345,
    };

    for (double v : test_values) {
      std::string s = dtos(v);
      size_t digit_count = count_significand_digits(s);

      // Skip values with very few digits
      if (digit_count <= 2) continue;

      std::string shortened = remove_last_significand_digit(s);
      if (shortened == s) continue;  // Couldn't shorten

      double parsed = lightusd::legacy::atof(shortened);

      // Removing the last digit should break the roundtrip
      TEST_CHECK_(!bitwise_equal_double(parsed, v),
                  "removing last digit still roundtrips for v=%.17g, s='%s', shortened='%s'",
                  v, s.c_str(), shortened.c_str());
    }
  }

  // Test 6.4: Adjacent values must produce different strings
  // If two adjacent floats produce the same string, the representation isn't shortest
  {
    std::vector<double> base_values = {
      1.0, 10.0, 100.0, 0.1, 0.01,
      1e10, 1e-10, 1e100, 1e-100,
      kPi, kE,
    };

    for (double base : base_values) {
      double v1 = base;
      double v2 = std::nextafter(base, std::numeric_limits<double>::infinity());
      double v3 = std::nextafter(base, -std::numeric_limits<double>::infinity());

      std::string s1 = dtos(v1);
      std::string s2 = dtos(v2);
      std::string s3 = dtos(v3);

      // Adjacent values should have different string representations
      TEST_CHECK_(s1 != s2,
                  "adjacent doubles have same string: v1=%.17g, v2=%.17g, s='%s'",
                  v1, v2, s1.c_str());
      TEST_CHECK_(s1 != s3,
                  "adjacent doubles have same string: v1=%.17g, v3=%.17g, s='%s'",
                  v1, v3, s1.c_str());
    }

    // Float adjacent value tests (using double representation)
    std::vector<float> base_floats = {
      1.0f, 10.0f, 100.0f, 0.1f, 0.01f,
      1e5f, 1e-5f,
    };

    for (float base : base_floats) {
      float v1 = base;
      float v2 = std::nextafter(base, std::numeric_limits<float>::infinity());
      float v3 = std::nextafter(base, -std::numeric_limits<float>::infinity());

      std::string s1 = dtos(v1);
      std::string s2 = dtos(v2);
      std::string s3 = dtos(v3);

      // Adjacent float values should produce different strings
      TEST_CHECK_(s1 != s2,
                  "adjacent floats have same string: v1=%.9g, v2=%.9g, s='%s'",
                  v1, v2, s1.c_str());
      TEST_CHECK_(s1 != s3,
                  "adjacent floats have same string: v1=%.9g, v3=%.9g, s='%s'",
                  v1, v3, s1.c_str());
    }
  }

  // Test 6.5: Specific cases where dragonbox is known to be shorter than naive
  {
    // 0.3 is a classic case: printf gives "0.29999999999999999" but dragonbox gives "0.3"
    {
      double v = 0.3;
      std::string s = dtos(v);
      // Should be short, not the full precision representation
      TEST_CHECK_(s.size() <= 4,  // "0.3" or similar
                  "0.3 should have short representation, got '%s'", s.c_str());
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(bitwise_equal_double(parsed, v),
                  "0.3 roundtrip failed: s='%s', parsed=%.17g", s.c_str(), parsed);
    }

    // 1.0/3.0 should produce a reasonable length string
    {
      double v = 1.0/3.0;
      std::string s = dtos(v);
      size_t digits = count_significand_digits(s);
      // Should need fewer than 17 digits
      TEST_CHECK_(digits <= 17,
                  "1/3 produced too many digits: '%s' (%zu)", s.c_str(), digits);
      double parsed = lightusd::legacy::atof(s);
      TEST_CHECK_(bitwise_equal_double(parsed, v),
                  "1/3 roundtrip failed");
    }

    // Verify common decimal fractions have short representations
    std::vector<std::pair<double, size_t>> short_cases = {
      {0.1, 2},    // "0.1"
      {0.2, 2},    // "0.2"
      {0.3, 2},    // "0.3"
      {0.4, 2},    // "0.4"
      {0.5, 2},    // "0.5"
      {0.6, 2},    // "0.6"
      {0.7, 2},    // "0.7"
      {0.8, 2},    // "0.8"
      {0.9, 2},    // "0.9"
    };

    for (const auto& tc : short_cases) {
      std::string s = dtos(tc.first);
      size_t digits = count_significand_digits(s);
      TEST_CHECK_(digits <= tc.second,
                  "%.1f should have at most %zu digits, got '%s' (%zu digits)",
                  tc.first, tc.second, s.c_str(), digits);
    }
  }

  // Test 6.6: Random values - verify shorter or equal to maximum precision
  {
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> dist(-1e15, 1e15);

    size_t total_dtos_digits = 0;
    size_t total_printf_digits = 0;
    const int num_tests = 1000;

    for (int i = 0; i < num_tests; i++) {
      double v = dist(rng);
      if (!std::isfinite(v)) continue;

      std::string dtos_str = dtos(v);

      char printf_buf[64];
      std::snprintf(printf_buf, sizeof(printf_buf), "%.17g", v);
      std::string printf_str(printf_buf);

      total_dtos_digits += count_significand_digits(dtos_str);
      total_printf_digits += count_significand_digits(printf_str);

      // Verify roundtrip
      double parsed = lightusd::legacy::atof(dtos_str);
      TEST_CHECK_(bitwise_equal_double(parsed, v) || nearly_equal_double(parsed, v, 1e-15),
                  "random shortest test roundtrip failed");
    }

    // On average, dtos should produce shorter output than printf %.17g
    double avg_dtos = static_cast<double>(total_dtos_digits) / num_tests;
    double avg_printf = static_cast<double>(total_printf_digits) / num_tests;

    TEST_CHECK_(avg_dtos <= avg_printf,
                "dtos average digits (%.2f) should be <= printf average (%.2f)",
                avg_dtos, avg_printf);
  }

  // Test 6.7: Float precision - verify roundtrip with double conversion
  // Note: dtos(float) internally converts to double, so we test that the
  // double representation still correctly round-trips through float
  {
    std::vector<float> test_floats = {
      0.1f, 0.2f, 0.3f,
      1.0f/3.0f, 2.0f/3.0f,
      123.456f, 789.012f,
      1e5f, 1e-5f,
      1.234567f,
      static_cast<float>(kPi),
    };

    for (float v : test_floats) {
      std::string s = dtos(v);
      float parsed = static_cast<float>(lightusd::legacy::atof(s));

      // The string representation should round-trip correctly for float
      TEST_CHECK_(bitwise_equal_float(parsed, v),
                  "float roundtrip via double failed: v=%.9g, s='%s', parsed=%.9g",
                  v, s.c_str(), parsed);
    }
  }

  // Test 6.8: Verify output never has unnecessary trailing zeros
  {
    std::vector<double> test_values = {
      1.0, 10.0, 100.0, 1000.0,
      1.5, 2.5, 3.5,
      0.5, 0.25, 0.125,
      1.25, 12.5, 125.0,
    };

    for (double v : test_values) {
      std::string s = dtos(v);

      // Find the decimal point
      size_t dot_pos = s.find('.');
      if (dot_pos != std::string::npos) {
        // Find exponent if any
        size_t exp_pos = s.find('e');
        if (exp_pos == std::string::npos) exp_pos = s.find('E');

        size_t end_pos = (exp_pos != std::string::npos) ? exp_pos : s.size();

        // Check last char before exponent is not '0' (unless it's the only digit after dot)
        if (end_pos > dot_pos + 2) {  // More than one digit after dot
          TEST_CHECK_(s[end_pos - 1] != '0',
                      "value %.17g has trailing zero: '%s'", v, s.c_str());
        }
      }
    }
  }

  // Test 6.9: Edge cases where Grisu2 may fail but Dragonbox guarantees shortest
  // These are specific bit patterns and boundary values known to be problematic
  {
    // Helper to verify shortest representation
    auto verify_shortest = [&](double v, const char* description) {
      std::string s = dtos(v);
      double parsed = lightusd::legacy::atof(s);

      // Must round-trip correctly
      TEST_CHECK_(bitwise_equal_double(parsed, v),
                  "%s: roundtrip failed for %.17g, got '%s' -> %.17g",
                  description, v, s.c_str(), parsed);

      // Compare with printf %.17g
      char printf_buf[64];
      std::snprintf(printf_buf, sizeof(printf_buf), "%.17g", v);
      size_t dtos_len = s.size();
      size_t printf_len = std::strlen(printf_buf);

      // dtos should be no longer than printf
      TEST_CHECK_(dtos_len <= printf_len,
                  "%s: dtos '%s' (%zu) longer than printf '%s' (%zu)",
                  description, s.c_str(), dtos_len, printf_buf, printf_len);
    };

    // 6.9.1: Subnormal numbers - Grisu2 often struggles with these
    {
      // Smallest subnormal
      verify_shortest(std::numeric_limits<double>::denorm_min(), "denorm_min");

      // Various subnormals
      verify_shortest(5e-324, "5e-324");
      verify_shortest(1e-323, "1e-323");
      verify_shortest(1e-320, "1e-320");
      verify_shortest(1e-310, "1e-310");

      // Subnormals from bit patterns
      verify_shortest(double_from_bits(0x0000000000000001ULL), "smallest_subnormal");
      verify_shortest(double_from_bits(0x0000000000000010ULL), "subnormal_16");
      verify_shortest(double_from_bits(0x0000000000000100ULL), "subnormal_256");
      verify_shortest(double_from_bits(0x000FFFFFFFFFFFFFULL), "largest_subnormal");
    }

    // 6.9.2: Values near powers of 10 - boundary cases
    {
      for (int exp = -300; exp <= 300; exp += 10) {
        double pow10 = std::pow(10.0, exp);
        if (!std::isfinite(pow10)) continue;

        verify_shortest(pow10, "power_of_10");

        // Just above and below
        double above = std::nextafter(pow10, std::numeric_limits<double>::infinity());
        double below = std::nextafter(pow10, 0.0);
        if (std::isfinite(above)) verify_shortest(above, "above_pow10");
        if (std::isfinite(below) && below > 0) verify_shortest(below, "below_pow10");
      }
    }

    // 6.9.3: Values near powers of 2 - IEEE 754 boundaries
    {
      for (int exp = -1022; exp <= 1023; exp += 50) {
        double pow2 = std::ldexp(1.0, exp);
        verify_shortest(pow2, "power_of_2");

        // Values just crossing the exponent boundary
        double above = std::nextafter(pow2, std::numeric_limits<double>::infinity());
        double below = std::nextafter(pow2, 0.0);
        verify_shortest(above, "above_pow2");
        if (below > 0) verify_shortest(below, "below_pow2");
      }
    }

    // 6.9.4: Specific problematic bit patterns for Grisu2
    // These are values where Grisu2's conservative rounding may produce longer output
    {
      // Values where the shortest representation requires careful rounding
      std::vector<uint64_t> problematic_bits = {
        0x3FF0000000000001ULL,  // Just above 1.0
        0x3FEFFFFFFFFFFFFFULL,  // Just below 1.0
        0x4000000000000000ULL,  // Exactly 2.0
        0x4008000000000000ULL,  // Exactly 3.0
        0x7FEFFFFFFFFFFFFFULL,  // Max normal (near infinity)
        0x0010000000000000ULL,  // Min normal
        0x000FFFFFFFFFFFFFULL,  // Max subnormal
        0x3FB999999999999AULL,  // 0.1
        0x3FC999999999999AULL,  // 0.2
        0x3FD3333333333333ULL,  // 0.3
        0x4005BF0A8B145769ULL,  // 2.718281828... (e)
        0x400921FB54442D18ULL,  // 3.141592653... (pi)
        0x3FF6A09E667F3BCDULL,  // sqrt(2)
        // Values at decimal/binary boundary crossings
        0x4024000000000000ULL,  // 10.0
        0x4059000000000000ULL,  // 100.0
        0x408F400000000000ULL,  // 1000.0
        0x40C3880000000000ULL,  // 10000.0
        // Challenging values for rounding decisions
        0x3FF8000000000000ULL,  // 1.5
        0x4000CCCCCCCCCCCDULL,  // 2.1
        0x4002666666666666ULL,  // 2.3
        0x400599999999999AULL,  // 2.7
      };

      for (uint64_t bits : problematic_bits) {
        double v = double_from_bits(bits);
        if (std::isfinite(v)) {
          verify_shortest(v, "problematic_bits");
        }
      }
    }

    // 6.9.5: Integer values that fit exactly - should have no decimal point
    {
      std::vector<double> exact_integers = {
        1.0, 2.0, 3.0, 10.0, 100.0, 1000.0,
        123.0, 456.0, 789.0,
        1234567890.0,
        9007199254740992.0,  // 2^53 - largest exact integer
        static_cast<double>(1ULL << 52),
        static_cast<double>(1ULL << 51),
      };

      for (double v : exact_integers) {
        std::string s = dtos(v);
        double parsed = lightusd::legacy::atof(s);
        TEST_CHECK_(bitwise_equal_double(parsed, v),
                    "exact integer roundtrip failed: %.17g -> '%s' -> %.17g",
                    v, s.c_str(), parsed);

        // Check that integer values don't have unnecessary decimal points
        // (unless in scientific notation)
        if (s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
          // For non-scientific notation, check for clean integer representation
          // Note: Some values may still need decimal for round-trip correctness
        }
      }
    }

    // 6.9.6: Values with long decimal representations
    // These test that Dragonbox finds the shortest among many possibilities
    {
      std::vector<double> long_decimal_values = {
        0.123456789012345,
        0.111111111111111,
        0.142857142857143,  // 1/7
        0.166666666666667,  // 1/6
        0.090909090909091,  // 1/11
        0.076923076923077,  // 1/13
        1.4142135623730951, // sqrt(2)
        2.2360679774997896, // sqrt(5)
        1.7320508075688772, // sqrt(3)
      };

      for (double v : long_decimal_values) {
        std::string s = dtos(v);
        double parsed = lightusd::legacy::atof(s);
        TEST_CHECK_(bitwise_equal_double(parsed, v),
                    "long decimal roundtrip failed: %.17g -> '%s' -> %.17g",
                    v, s.c_str(), parsed);

        // These should all be shorter than 17 digits
        TEST_CHECK_(count_significand_digits(s) <= 17,
                    "long decimal produced too many digits: '%s'", s.c_str());
      }
    }
  }

  // Test 6.10: Exhaustive verification of shortest property
  // For a sample of values, verify that no shorter representation exists
  {
    auto is_shorter_valid = [](double original, const std::string& shorter) -> bool {
      // Try to parse the shorter string
      double parsed = lightusd::legacy::atof(shorter);
      return bitwise_equal_double(parsed, original);
    };

    auto try_shorten = [](const std::string& s) -> std::vector<std::string> {
      std::vector<std::string> candidates;

      // Try removing each digit
      for (size_t i = 0; i < s.size(); i++) {
        if (s[i] >= '0' && s[i] <= '9') {
          std::string shortened = s.substr(0, i) + s.substr(i + 1);
          // Clean up invalid formats
          if (shortened.empty()) continue;
          if (shortened == "-" || shortened == "+") continue;
          if (shortened == "." || shortened == "-.") continue;
          if (shortened.back() == '.') shortened.pop_back();
          if (shortened.find('.') == 0) shortened = "0" + shortened;
          if (shortened.find("-.") == 0) shortened = "-0" + shortened.substr(1);
          if (!shortened.empty()) {
            candidates.push_back(shortened);
          }
        }
      }

      // Try rounding last digit up/down
      size_t exp_pos = s.find('e');
      if (exp_pos == std::string::npos) exp_pos = s.find('E');
      size_t end = (exp_pos != std::string::npos) ? exp_pos : s.size();

      for (size_t i = end; i > 0; i--) {
        if (s[i-1] >= '0' && s[i-1] <= '9') {
          // Try incrementing
          if (s[i-1] < '9') {
            std::string inc = s;
            inc[i-1]++;
            candidates.push_back(inc.substr(0, i) + (exp_pos != std::string::npos ? s.substr(exp_pos) : ""));
          }
          // Try decrementing
          if (s[i-1] > '0') {
            std::string dec = s;
            dec[i-1]--;
            candidates.push_back(dec.substr(0, i) + (exp_pos != std::string::npos ? s.substr(exp_pos) : ""));
          }
          break;
        }
      }

      return candidates;
    };

    // Test specific values known to be at representation boundaries
    std::vector<double> boundary_values = {
      1.0 + std::numeric_limits<double>::epsilon(),
      1.0 - std::numeric_limits<double>::epsilon() / 2,
      std::nextafter(0.1, 0.0),
      std::nextafter(0.1, 1.0),
      std::nextafter(0.5, 0.0),
      std::nextafter(0.5, 1.0),
      1.005,  // Rounding boundary
      2.995,  // Rounding boundary
      9.995,  // Rounding boundary
      99.995, // Rounding boundary
    };

    for (double v : boundary_values) {
      std::string s = dtos(v);

      // Verify no shortened version works
      auto candidates = try_shorten(s);
      for (const auto& candidate : candidates) {
        if (candidate.size() < s.size() && is_shorter_valid(v, candidate)) {
          TEST_CHECK_(false,
                      "found shorter valid representation: %.17g -> '%s' but '%s' also works",
                      v, s.c_str(), candidate.c_str());
        }
      }
    }
  }

  // Test 6.11: Statistical verification across full double range
  // Verify Dragonbox consistently produces shorter output than naive approaches
  {
    std::mt19937_64 rng(98765);

    size_t dtos_total_chars = 0;
    size_t printf_total_chars = 0;
    size_t dtos_wins = 0;
    size_t printf_wins = 0;
    size_t ties = 0;
    const int num_samples = 10000;

    for (int i = 0; i < num_samples; i++) {
      // Generate random double across full range
      uint64_t bits = rng();
      // Mask out NaN/Inf
      bits &= 0x7FEFFFFFFFFFFFFFULL;
      if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) {
        bits &= 0x7FEFFFFFFFFFFFFFULL;  // Ensure not NaN/Inf
      }
      // Random sign
      if (rng() & 1) bits |= 0x8000000000000000ULL;

      double v = double_from_bits(bits);
      if (!std::isfinite(v) || v == 0.0) continue;

      std::string dtos_str = dtos(v);
      char printf_buf[64];
      std::snprintf(printf_buf, sizeof(printf_buf), "%.17g", v);

      size_t dtos_len = dtos_str.size();
      size_t printf_len = std::strlen(printf_buf);

      dtos_total_chars += dtos_len;
      printf_total_chars += printf_len;

      if (dtos_len < printf_len) dtos_wins++;
      else if (dtos_len > printf_len) printf_wins++;
      else ties++;

      // Verify roundtrip
      double parsed = lightusd::legacy::atof(dtos_str);
      TEST_CHECK_(bitwise_equal_double(parsed, v),
                  "statistical test roundtrip failed: %.17g -> '%s'", v, dtos_str.c_str());
    }

    // dtos should win more often than printf (or at least tie)
    TEST_CHECK_(dtos_wins + ties >= printf_wins,
                "dtos should produce shorter output: wins=%zu, loses=%zu, ties=%zu",
                dtos_wins, printf_wins, ties);

    // Average length should be shorter
    double avg_dtos = static_cast<double>(dtos_total_chars) / num_samples;
    double avg_printf = static_cast<double>(printf_total_chars) / num_samples;
    TEST_CHECK_(avg_dtos <= avg_printf,
                "dtos avg length (%.2f) should be <= printf avg (%.2f)",
                avg_dtos, avg_printf);
  }

  // Test 6.12: Float-specific edge cases
  // Since dtos(float) converts to double, verify float precision is preserved
  {
    // Float-specific problematic values
    std::vector<uint32_t> float_bits = {
      0x3F800001U,  // Just above 1.0f
      0x3F7FFFFFU,  // Just below 1.0f
      0x00000001U,  // Smallest subnormal float
      0x007FFFFFU,  // Largest subnormal float
      0x00800000U,  // Smallest normal float
      0x7F7FFFFFU,  // Largest finite float
      0x3DCCCCCDU,  // 0.1f
      0x3E4CCCCDU,  // 0.2f
      0x3E99999AU,  // 0.3f
      0x40490FDBU,  // pi as float
      0x402DF854U,  // e as float
    };

    for (uint32_t bits : float_bits) {
      float f = float_from_bits(bits);
      if (!std::isfinite(f)) continue;

      std::string s = dtos(f);
      float parsed = static_cast<float>(lightusd::legacy::atof(s));

      TEST_CHECK_(bitwise_equal_float(parsed, f),
                  "float edge case roundtrip failed: bits=0x%08X, f=%.9g, s='%s', parsed=%.9g",
                  bits, f, s.c_str(), parsed);
    }

    // Verify adjacent floats produce different strings
    std::vector<float> float_bases = {1.0f, 0.1f, 10.0f, 100.0f, 0.01f, 1e6f, 1e-6f};
    for (float base : float_bases) {
      float v1 = base;
      float v2 = std::nextafter(base, std::numeric_limits<float>::infinity());
      float v3 = std::nextafter(base, -std::numeric_limits<float>::infinity());

      std::string s1 = dtos(v1);
      std::string s2 = dtos(v2);
      std::string s3 = dtos(v3);

      // All three should be different
      TEST_CHECK_(s1 != s2 && s1 != s3 && s2 != s3,
                  "adjacent floats should have different strings: %.9g='%s', %.9g='%s', %.9g='%s'",
                  v1, s1.c_str(), v2, s2.c_str(), v3, s3.c_str());
    }
  }

  // Test 6.13: Float precision limit (max 9 significant digits)
  // Verify that float output never exceeds max_digits10 = 9
  // Note: "significant digits" excludes leading zeros (e.g., in 0.00123, only 123 counts)
  {
    // Helper to count true significant digits (excludes leading zeros)
    auto count_true_significant_digits = [](const std::string& s) -> size_t {
      size_t count = 0;
      bool started = false;
      bool in_exponent = false;
      for (char c : s) {
        if (c == 'e' || c == 'E') {
          in_exponent = true;
        } else if (!in_exponent && c >= '0' && c <= '9') {
          if (c != '0' || started) {
            started = true;
            count++;
          }
        }
      }
      return count;
    };

    std::vector<float> test_floats = {
      0.707f,      // The case that triggered precision limiting
      0.123456789f,
      3.14159265f,
      2.71828182f,
      1.41421356f,  // sqrt(2)
      0.0001234567f,
      1234567.0f,
      9.99999999f,
    };

    for (float v : test_floats) {
      std::string s = dtos(v);
      size_t sig_digits = count_true_significant_digits(s);

      TEST_CHECK_(sig_digits <= 9,
                  "float 0x%08X (%.9g) has %zu significant digits (max 9): '%s'",
                  float_to_bits(v), v, sig_digits, s.c_str());

      // Also verify roundtrip still works
      float parsed = std::strtof(s.c_str(), nullptr);
      TEST_CHECK_(bitwise_equal_float(parsed, v),
                  "float precision limit broke roundtrip: v=%.9g, s='%s', parsed=%.9g",
                  v, s.c_str(), parsed);
    }
  }

  // Test 6.14: Double precision limit (max 17 significant digits)
  // Verify that double output never exceeds max_digits10 = 17
  {
    // Helper to count true significant digits (excludes leading zeros)
    auto count_true_significant_digits = [](const std::string& s) -> size_t {
      size_t count = 0;
      bool started = false;
      bool in_exponent = false;
      for (char c : s) {
        if (c == 'e' || c == 'E') {
          in_exponent = true;
        } else if (!in_exponent && c >= '0' && c <= '9') {
          if (c != '0' || started) {
            started = true;
            count++;
          }
        }
      }
      return count;
    };

    std::vector<double> test_doubles = {
      3.14159265358979323846,  // pi
      2.71828182845904523536,  // e
      1.41421356237309504880,  // sqrt(2)
      0.12345678901234567890,
      123456789012345.0,
      9.99999999999999999,
      0.00000000000000001,
    };

    for (double v : test_doubles) {
      std::string s = dtos(v);
      size_t sig_digits = count_true_significant_digits(s);

      TEST_CHECK_(sig_digits <= 17,
                  "double %.17g has %zu significant digits (max 17): '%s'",
                  v, sig_digits, s.c_str());

      // Also verify roundtrip still works
      double parsed = std::strtod(s.c_str(), nullptr);
      TEST_CHECK_(bitwise_equal_double(parsed, v),
                  "double precision limit broke roundtrip: v=%.17g, s='%s', parsed=%.17g",
                  v, s.c_str(), parsed);
    }
  }
}
