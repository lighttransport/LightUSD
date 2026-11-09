#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-strutil.h"
#include "str-util.hh"
#include "tiny-string.hh"
#include "token-type.hh"
#include "value-types.hh"
#include <cmath>
#include <limits>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

void strutil_test(void) {
  {
    std::string s = "HelloA WorlZ";
    std::string ls = lightusd::legacy::to_lower(s);
    TEST_CHECK(ls.compare("helloa worlz") == 0);
  }

  {
    std::string s = "_aaa";
    TEST_CHECK(is_valid_utf8_identifier(s));

    // TODO: Do not allow underscore-only identifier?
    s = "___";
    TEST_CHECK(is_valid_utf8_identifier(s));

    s = "customLayerData";
    TEST_CHECK(isValidIdentifier(s));

    // Now LightUSD allow UTF-8 string as identifier by default.
    s = u8"響";
    TEST_CHECK(isValidIdentifier(s));

    // Emoji in identifier is not allowed.
    s = u8"_hello😩";
    TEST_CHECK(!isValidIdentifier(s));

  }

  auto check_usda_string_roundtrip = [](const std::string &src) {
    const std::string encoded = buildEscapedAndQuotedStringForUSDA(src);
    std::string body;
    if (encoded.compare(0, 3, "\"\"\"") == 0) {
      body = unwrap(encoded, "\"\"\"");
    } else if (encoded.compare(0, 3, "'''") == 0) {
      body = unwrap(encoded, "'''");
    } else {
      body = unwrap(encoded, encoded.substr(0, 1));
    }
    TEST_CHECK(unescapeControlSequence(body) == src);
  };
  check_usda_string_roundtrip(
      R"usd(both'quotes" and literal \ before \"quote)usd");
  check_usda_string_roundtrip("line one\nline two \\ \" ''' and \"\"\"");
  {
    std::string controls;
    controls.push_back('\0');
    controls.push_back('\x01');
    controls.push_back('\x1f');
    controls.push_back('\x7f');
    controls += "\t\r\n";
    check_usda_string_roundtrip(controls);

    const std::string encoded = buildEscapedAndQuotedStringForUSDA(controls);
    TEST_CHECK(encoded.find("\\x00") != std::string::npos);
    TEST_CHECK(encoded.find("\\x01") != std::string::npos);
    TEST_CHECK(encoded.find("\\x1f") != std::string::npos);
    TEST_CHECK(encoded.find("\\x7f") != std::string::npos);
    TEST_CHECK(encoded.find('\0') == std::string::npos);
    TEST_CHECK(encoded.find('\x01') == std::string::npos);
    TEST_CHECK(encoded.find('\x1f') == std::string::npos);
    TEST_CHECK(encoded.find('\x7f') == std::string::npos);

    const std::string escaped = escapeControlSequence(controls);
    TEST_CHECK(escaped.find("\\n") != std::string::npos);
    TEST_CHECK(escaped.find("\\r") != std::string::npos);
    TEST_CHECK(unescapeControlSequence(escaped) == controls);
  }

  Token a("alpha");
  Token b("beta");
  const char *alpha = a.c_str();
  TEST_CHECK(std::string(b.c_str()) == "beta");
  TEST_CHECK(alpha && std::string(alpha) == "alpha");
}

void tinystring_test(void) {

  tstring s("hello");
  tstring s2("bora");
  tstring s3("ll");
  tstring s4("hellobora");
  tstring_view v0(s);
  tstring_view v1(s);
  tstring_view v2(s2);
  tstring_view v3(s3);
  tstring_view v4(s4);

  TEST_CHECK(v0 == v1);
  TEST_CHECK(v0 != v2);

  TEST_CHECK(v0.contains(v0));
  TEST_CHECK(v0.contains(v3));
  TEST_CHECK(!v0.contains(v2));

  TEST_CHECK(!v4.starts_with(v2));
  TEST_CHECK(v4.starts_with(v0));

  TEST_CHECK(!v4.ends_with(v0));
  TEST_CHECK(v4.ends_with(v2));

}

void parse_int_test(void) {
  using namespace lightusd::legacy::str;

  int32_t result;

  // Basic positive numbers
  {
    tstring_view sv("123");
    TEST_CHECK(parse_int(sv, &result));
    TEST_CHECK(result == 123);
  }

  // Basic negative numbers
  {
    tstring_view sv("-456");
    TEST_CHECK(parse_int(sv, &result));
    TEST_CHECK(result == -456);
  }

  // Zero
  {
    tstring_view sv("0");
    TEST_CHECK(parse_int(sv, &result));
    TEST_CHECK(result == 0);
  }

  // Positive sign
  {
    tstring_view sv("+789");
    TEST_CHECK(parse_int(sv, &result));
    TEST_CHECK(result == 789);
  }

  // Maximum int32_t value
  {
    tstring_view sv("2147483647");
    TEST_CHECK(parse_int(sv, &result));
    TEST_CHECK(result == 2147483647);
  }

  // Minimum int32_t value
  {
    tstring_view sv("-2147483648");
    TEST_CHECK(parse_int(sv, &result));
    TEST_CHECK(result == -2147483648);
  }

  // Empty string
  {
    tstring_view sv("");
    TEST_CHECK(!parse_int(sv, &result));
  }

  // Just a sign
  {
    tstring_view sv("-");
    TEST_CHECK(!parse_int(sv, &result));
  }

  {
    tstring_view sv("+");
    TEST_CHECK(!parse_int(sv, &result));
  }

  // Non-numeric characters
  {
    tstring_view sv("123a");
    TEST_CHECK(!parse_int(sv, &result));
  }

  {
    tstring_view sv("a123");
    TEST_CHECK(!parse_int(sv, &result));
  }

  {
    tstring_view sv("12.3");
    TEST_CHECK(!parse_int(sv, &result));
  }

  // Overflow cases
  {
    tstring_view sv("2147483648");  // INT32_MAX + 1
    TEST_CHECK(!parse_int(sv, &result));
  }

  {
    tstring_view sv("-2147483649");  // INT32_MIN - 1
    TEST_CHECK(!parse_int(sv, &result));
  }

  // Very large numbers
  {
    tstring_view sv("999999999999999999");
    TEST_CHECK(!parse_int(sv, &result));
  }

  {
    tstring_view sv("-999999999999999999");
    TEST_CHECK(!parse_int(sv, &result));
  }

  // Leading/trailing spaces (should fail since parse_int doesn't handle whitespace)
  {
    tstring_view sv(" 123");
    TEST_CHECK(!parse_int(sv, &result));
  }

  {
    tstring_view sv("123 ");
    TEST_CHECK(!parse_int(sv, &result));
  }

  // Multiple signs
  {
    tstring_view sv("++123");
    TEST_CHECK(!parse_int(sv, &result));
  }

  {
    tstring_view sv("--123");
    TEST_CHECK(!parse_int(sv, &result));
  }

  {
    tstring_view sv("+-123");
    TEST_CHECK(!parse_int(sv, &result));
  }

}

void strutil_parse_helpers_test(void) {
  {
    nonstd::optional<int> v = lightusd::legacy::atoi("2147483647");
    TEST_CHECK(v.has_value());
    TEST_CHECK(v.value() == 2147483647);
  }
  {
    nonstd::optional<int> v = lightusd::legacy::atoi("-2147483648");
    TEST_CHECK(v.has_value());
    TEST_CHECK(v.value() == (std::numeric_limits<int>::min)());
  }
  TEST_CHECK(!lightusd::legacy::atoi("2147483648").has_value());
  TEST_CHECK(!lightusd::legacy::atoi("-2147483649").has_value());
  TEST_CHECK(!lightusd::legacy::atoi("123abc").has_value());
  TEST_CHECK(!lightusd::legacy::atoi(static_cast<const char *>(nullptr)).has_value());

  {
    nonstd::optional<uint32_t> v = lightusd::legacy::atou("4294967295");
    TEST_CHECK(v.has_value());
    TEST_CHECK(v.value() == (std::numeric_limits<uint32_t>::max)());
  }
  TEST_CHECK(!lightusd::legacy::atou("4294967296").has_value());
  TEST_CHECK(!lightusd::legacy::atou("-1").has_value());
  TEST_CHECK(!lightusd::legacy::atou("1x").has_value());

  {
    nonstd::optional<int64_t> v = lightusd::legacy::atoll("9223372036854775807");
    TEST_CHECK(v.has_value());
    TEST_CHECK(v.value() == (std::numeric_limits<int64_t>::max)());
  }
  {
    nonstd::optional<int64_t> v = lightusd::legacy::atoll("-9223372036854775808");
    TEST_CHECK(v.has_value());
    TEST_CHECK(v.value() == (std::numeric_limits<int64_t>::min)());
  }
  TEST_CHECK(!lightusd::legacy::atoll("9223372036854775808").has_value());
  TEST_CHECK(!lightusd::legacy::atoll("-9223372036854775809").has_value());
  TEST_CHECK(!lightusd::legacy::atoll(static_cast<const char *>(nullptr)).has_value());

  {
    nonstd::optional<uint64_t> v = lightusd::legacy::atoull("18446744073709551615");
    TEST_CHECK(v.has_value());
    TEST_CHECK(v.value() == (std::numeric_limits<uint64_t>::max)());
  }
  TEST_CHECK(!lightusd::legacy::atoull("18446744073709551616").has_value());
  TEST_CHECK(!lightusd::legacy::atoull("-1").has_value());
  TEST_CHECK(!lightusd::legacy::atoull(static_cast<const char *>(nullptr)).has_value());

  {
    nonstd::optional<double> v = lightusd::legacy::atod(" 3.5 ");
    TEST_CHECK(v.has_value());
    TEST_CHECK(v.value() == 3.5);
  }
  TEST_CHECK(!lightusd::legacy::atod("3.5x").has_value());
  TEST_CHECK(!lightusd::legacy::atod("nan").has_value());
  TEST_CHECK(!lightusd::legacy::atod("inf").has_value());
  TEST_CHECK(!lightusd::legacy::atod(static_cast<const char *>(nullptr)).has_value());

  {
    nonstd::optional<float> v = lightusd::legacy::atof_float("-2.25");
    TEST_CHECK(v.has_value());
    TEST_CHECK(v.value() == -2.25f);
  }
  TEST_CHECK(!lightusd::legacy::atof_float("1.0x").has_value());
  TEST_CHECK(!lightusd::legacy::atof_float("nan").has_value());
  TEST_CHECK(!lightusd::legacy::atof_float("inf").has_value());
  TEST_CHECK(!lightusd::legacy::atof_float(static_cast<const char *>(nullptr)).has_value());
}

void dtoa_test(void) {
  // Test double to ASCII conversion with full precision

  {
    // Test pi with full double precision
    double pi = 3.141592653589793;
    char buf[384];
    char *end = dtoa(pi, buf);
    *end = '\0';
    std::string result(buf);

    // Should preserve full precision (at least 15 significant digits)
    TEST_CHECK(result.find("3.14159265358979") != std::string::npos);
    TEST_MSG("pi result: %s", result.c_str());
  }

  {
    // Test e with full double precision
    double e = 2.718281828459045;
    char buf[384];
    char *end = dtoa(e, buf);
    *end = '\0';
    std::string result(buf);

    // Should preserve full precision
    TEST_CHECK(result.find("2.71828182845904") != std::string::npos);
    TEST_MSG("e result: %s", result.c_str());
  }

  {
    // Test negative value
    double neg = -2.718281828459045;
    char buf[384];
    char *end = dtoa(neg, buf);
    *end = '\0';
    std::string result(buf);

    TEST_CHECK(result[0] == '-');
    TEST_CHECK(result.find("2.71828182845904") != std::string::npos);
    TEST_MSG("negative e result: %s", result.c_str());
  }

  {
    // Test zero
    double zero = 0.0;
    char buf[384];
    char *end = dtoa(zero, buf);
    *end = '\0';
    std::string result(buf);

    TEST_CHECK(result == "0.0");
    TEST_MSG("zero result: %s", result.c_str());
  }

  {
    // Test negative zero
    double neg_zero = -0.0;
    char buf[384];
    char *end = dtoa(neg_zero, buf);
    *end = '\0';
    std::string result(buf);

    // dtoa_milo should output "-0.0" for negative zero
    TEST_CHECK(result == "-0.0");
    TEST_MSG("negative zero result: %s", result.c_str());
  }

  {
    // Test small number
    double small_val = 0.000001234567890123456;
    char buf[384];
    char *end = dtoa(small_val, buf);
    *end = '\0';
    std::string result(buf);

    // Should use scientific notation for very small numbers
    TEST_MSG("small number result: %s", result.c_str());
    TEST_CHECK(result.length() > 0);
  }

  {
    // Test large number
    double large = 1234567890123456.0;
    char buf[384];
    char *end = dtoa(large, buf);
    *end = '\0';
    std::string result(buf);

    TEST_MSG("large number result: %s", result.c_str());
    TEST_CHECK(result.length() > 0);
  }

  {
    // Test float conversion
    float f = 3.14159f;
    char buf[384];
    char *end = dtoa(f, buf);
    *end = '\0';
    std::string result(buf);

    // Should output float with appropriate precision
    TEST_CHECK(result.find("3.14159") != std::string::npos);
    TEST_MSG("float result: %s", result.c_str());
  }

  {
    // Test one
    double one = 1.0;
    char buf[384];
    char *end = dtoa(one, buf);
    *end = '\0';
    std::string result(buf);

    TEST_CHECK(result == "1.0");
    TEST_MSG("one result: %s", result.c_str());
  }

  {
    // Test integer-like double
    double int_like = 42.0;
    char buf[384];
    char *end = dtoa(int_like, buf);
    *end = '\0';
    std::string result(buf);

    TEST_CHECK(result == "42.0");
    TEST_MSG("integer-like result: %s", result.c_str());
  }
}

void parse_array_test(void) {
  using namespace lightusd::legacy::str;
  using namespace lightusd::value;
  using namespace lightusd::legacy::value;  // NOLINT

  // ===== float2 =====
  {
    tstring_view sv("[(1, 2), (3.5, 4.5)]");
    std::vector<float2> result;
    TEST_CHECK(parse_float2_array(sv, &result));
    TEST_CHECK(result.size() == 2);
    TEST_CHECK(result[0][0] == 1.0f);
    TEST_CHECK(result[0][1] == 2.0f);
    TEST_CHECK(result[1][0] == 3.5f);
    TEST_CHECK(result[1][1] == 4.5f);
  }

  // ===== float3 =====
  {
    tstring_view sv("[(1, 2, 3), (4.5, 5.5, 6.5)]");
    std::vector<float3> result;
    TEST_CHECK(parse_float3_array(sv, &result));
    TEST_CHECK(result.size() == 2);
    TEST_CHECK(result[0][0] == 1.0f);
    TEST_CHECK(result[0][2] == 3.0f);
    TEST_CHECK(result[1][1] == 5.5f);
  }

  // ===== float4 =====
  {
    tstring_view sv("[(1, 2, 3, 4)]");
    std::vector<float4> result;
    TEST_CHECK(parse_float4_array(sv, &result));
    TEST_CHECK(result.size() == 1);
    TEST_CHECK(result[0][0] == 1.0f);
    TEST_CHECK(result[0][3] == 4.0f);
  }

  // ===== double2 =====
  {
    tstring_view sv("[(1.5, 2.5), (3.5, 4.5)]");
    std::vector<double2> result;
    TEST_CHECK(parse_double2_array(sv, &result));
    TEST_CHECK(result.size() == 2);
    TEST_CHECK(result[0][0] == 1.5);
    TEST_CHECK(result[1][1] == 4.5);
  }

  // ===== double3 =====
  {
    tstring_view sv("[(1, 2, 3)]");
    std::vector<double3> result;
    TEST_CHECK(parse_double3_array(sv, &result));
    TEST_CHECK(result.size() == 1);
    TEST_CHECK(result[0][0] == 1.0);
    TEST_CHECK(result[0][1] == 2.0);
    TEST_CHECK(result[0][2] == 3.0);
  }

  // ===== double4 =====
  {
    tstring_view sv("[(1, 2, 3, 4), (5, 6, 7, 8)]");
    std::vector<double4> result;
    TEST_CHECK(parse_double4_array(sv, &result));
    TEST_CHECK(result.size() == 2);
    TEST_CHECK(result[0][0] == 1.0);
    TEST_CHECK(result[1][3] == 8.0);
  }

  // ===== matrix2f =====
  {
    tstring_view sv("[((1, 0), (0, 1))]");
    std::vector<matrix2f> result;
    TEST_CHECK(parse_matrix2f_array(sv, &result));
    TEST_CHECK(result.size() == 1);
    TEST_CHECK(result[0].m[0][0] == 1.0f);
    TEST_CHECK(result[0].m[0][1] == 0.0f);
    TEST_CHECK(result[0].m[1][0] == 0.0f);
    TEST_CHECK(result[0].m[1][1] == 1.0f);
  }

  // ===== matrix3f =====
  {
    tstring_view sv("[((1, 0, 0), (0, 1, 0), (0, 0, 1))]");
    std::vector<matrix3f> result;
    TEST_CHECK(parse_matrix3f_array(sv, &result));
    TEST_CHECK(result.size() == 1);
    TEST_CHECK(result[0].m[0][0] == 1.0f);
    TEST_CHECK(result[0].m[1][1] == 1.0f);
    TEST_CHECK(result[0].m[2][2] == 1.0f);
    TEST_CHECK(result[0].m[0][1] == 0.0f);
  }

  // ===== matrix4f =====
  {
    tstring_view sv("[((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))]");
    std::vector<matrix4f> result;
    TEST_CHECK(parse_matrix4f_array(sv, &result));
    TEST_CHECK(result.size() == 1);
    TEST_CHECK(result[0].m[0][0] == 1.0f);
    TEST_CHECK(result[0].m[3][3] == 1.0f);
    TEST_CHECK(result[0].m[1][0] == 0.0f);
  }

  // ===== matrix2d =====
  {
    tstring_view sv("[((2, 0), (0, 2))]");
    std::vector<matrix2d> result;
    TEST_CHECK(parse_matrix2d_array(sv, &result));
    TEST_CHECK(result.size() == 1);
    TEST_CHECK(result[0].m[0][0] == 2.0);
    TEST_CHECK(result[0].m[1][1] == 2.0);
  }

  // ===== matrix3d =====
  {
    tstring_view sv("[((1, 0, 0), (0, 1, 0), (0, 0, 1))]");
    std::vector<matrix3d> result;
    TEST_CHECK(parse_matrix3d_array(sv, &result));
    TEST_CHECK(result.size() == 1);
    TEST_CHECK(result[0].m[0][0] == 1.0);
    TEST_CHECK(result[0].m[2][2] == 1.0);
  }

  // ===== matrix4d =====
  {
    tstring_view sv("[((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))]");
    std::vector<matrix4d> result;
    TEST_CHECK(parse_matrix4d_array(sv, &result));
    TEST_CHECK(result.size() == 1);
    TEST_CHECK(result[0].m[0][0] == 1.0);
    TEST_CHECK(result[0].m[3][3] == 1.0);
  }

  // ===== Empty arrays =====
  {
    tstring_view sv("[]");
    std::vector<float2> result;
    TEST_CHECK(parse_float2_array(sv, &result));
    TEST_CHECK(result.size() == 0);
  }
  {
    tstring_view sv("[]");
    std::vector<matrix4d> result;
    TEST_CHECK(parse_matrix4d_array(sv, &result));
    TEST_CHECK(result.size() == 0);
  }

  // ===== Null pointer =====
  {
    tstring_view sv("[(1, 2)]");
    TEST_CHECK(!parse_float2_array(sv, nullptr));
  }

  // ===== Empty string =====
  {
    tstring_view sv("");
    std::vector<float3> result;
    TEST_CHECK(!parse_float3_array(sv, &result));
  }

  // ===== Multi-element matrix array =====
  {
    tstring_view sv("[((1, 0), (0, 1)), ((0, 1), (1, 0))]");
    std::vector<matrix2f> result;
    TEST_CHECK(parse_matrix2f_array(sv, &result));
    TEST_CHECK(result.size() == 2);
    TEST_CHECK(result[0].m[0][0] == 1.0f);
    TEST_CHECK(result[1].m[0][0] == 0.0f);
    TEST_CHECK(result[1].m[0][1] == 1.0f);
  }

  // ===== matrix4d compact format (no spaces, as found in real USDA files) =====
  {
    tstring_view sv("[((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,2,1))]");
    std::vector<matrix4d> result;
    TEST_CHECK(parse_matrix4d_array(sv, &result));
    TEST_CHECK(result.size() == 2);
    TEST_CHECK(result[0].m[0][0] == 1.0);
    TEST_CHECK(result[0].m[3][3] == 1.0);
    TEST_CHECK(result[1].m[3][2] == 2.0);
    TEST_CHECK(result[1].m[3][3] == 1.0);
  }

  // ==========================================================================
  // Scalar array error cases (float, double, int)
  // ==========================================================================

  // --- float array ---
  {
    // Null pointer
    tstring_view sv("[1.0, 2.0]");
    TEST_CHECK(!parse_float_array(sv, (std::vector<float> *)nullptr));
  }
  {
    // Empty string
    tstring_view sv("");
    std::vector<float> r;
    TEST_CHECK(!parse_float_array(sv, &r));
  }
  {
    // Missing opening bracket
    tstring_view sv("1.0, 2.0]");
    std::vector<float> r;
    TEST_CHECK(!parse_float_array(sv, &r));
  }
  {
    // Non-numeric content
    tstring_view sv("[abc, def]");
    std::vector<float> r;
    TEST_CHECK(!parse_float_array(sv, &r));
  }
  {
    // Empty array
    tstring_view sv("[]");
    std::vector<float> r;
    TEST_CHECK(parse_float_array(sv, &r));
    TEST_CHECK(r.size() == 0);
  }
  {
    // Single element
    tstring_view sv("[3.14]");
    std::vector<float> r;
    TEST_CHECK(parse_float_array(sv, &r));
    TEST_CHECK(r.size() == 1);
    TEST_CHECK(r[0] == 3.14f);
  }
  {
    // Whitespace around values
    tstring_view sv("[  1.0 , 2.0 , 3.0  ]");
    std::vector<float> r;
    TEST_CHECK(parse_float_array(sv, &r));
    TEST_CHECK(r.size() == 3);
  }

  // --- double array ---
  {
    tstring_view sv("");
    std::vector<double> r;
    TEST_CHECK(!parse_double_array(sv, &r));
  }
  {
    tstring_view sv("[abc]");
    std::vector<double> r;
    TEST_CHECK(!parse_double_array(sv, &r));
  }
  {
    tstring_view sv("[]");
    std::vector<double> r;
    TEST_CHECK(parse_double_array(sv, &r));
    TEST_CHECK(r.size() == 0);
  }
  {
    tstring_view sv("[1.0]");
    std::vector<double> r;
    TEST_CHECK(parse_double_array(sv, &r));
    TEST_CHECK(r.size() == 1);
    TEST_CHECK(r[0] == 1.0);
  }

  // --- int array ---
  {
    tstring_view sv("");
    std::vector<int32_t> r;
    TEST_CHECK(!parse_int_array(sv, &r));
  }
  {
    tstring_view sv("[abc]");
    std::vector<int32_t> r;
    TEST_CHECK(!parse_int_array(sv, &r));
  }
  {
    tstring_view sv("[]");
    std::vector<int32_t> r;
    TEST_CHECK(parse_int_array(sv, &r));
    TEST_CHECK(r.size() == 0);
  }
  {
    tstring_view sv("[1, -2, 3]");
    std::vector<int32_t> r;
    TEST_CHECK(parse_int_array(sv, &r));
    TEST_CHECK(r.size() == 3);
    TEST_CHECK(r[0] == 1);
    TEST_CHECK(r[1] == -2);
    TEST_CHECK(r[2] == 3);
  }

  // ==========================================================================
  // Compound vector (float2/3/4, double2/3/4) error cases
  // ==========================================================================

  // --- Null pointer for each type ---
  {
    tstring_view sv("[(1, 2)]");
    TEST_CHECK(!parse_float2_array(sv, (std::vector<float2> *)nullptr));
    TEST_CHECK(!parse_float3_array(sv, (std::vector<float3> *)nullptr));
    TEST_CHECK(!parse_float4_array(sv, (std::vector<float4> *)nullptr));
    TEST_CHECK(!parse_double2_array(sv, (std::vector<double2> *)nullptr));
    TEST_CHECK(!parse_double3_array(sv, (std::vector<double3> *)nullptr));
    TEST_CHECK(!parse_double4_array(sv, (std::vector<double4> *)nullptr));
  }

  // --- Empty string for each type ---
  {
    tstring_view sv("");
    std::vector<float2> f2; std::vector<float3> f3; std::vector<float4> f4;
    std::vector<double2> d2; std::vector<double3> d3; std::vector<double4> d4;
    TEST_CHECK(!parse_float2_array(sv, &f2));
    TEST_CHECK(!parse_float3_array(sv, &f3));
    TEST_CHECK(!parse_float4_array(sv, &f4));
    TEST_CHECK(!parse_double2_array(sv, &d2));
    TEST_CHECK(!parse_double3_array(sv, &d3));
    TEST_CHECK(!parse_double4_array(sv, &d4));
  }

  // --- Empty arrays for each type ---
  {
    tstring_view sv("[]");
    std::vector<float2> f2; std::vector<float3> f3; std::vector<float4> f4;
    std::vector<double2> d2; std::vector<double3> d3; std::vector<double4> d4;
    TEST_CHECK(parse_float2_array(sv, &f2)); TEST_CHECK(f2.empty());
    TEST_CHECK(parse_float3_array(sv, &f3)); TEST_CHECK(f3.empty());
    TEST_CHECK(parse_float4_array(sv, &f4)); TEST_CHECK(f4.empty());
    TEST_CHECK(parse_double2_array(sv, &d2)); TEST_CHECK(d2.empty());
    TEST_CHECK(parse_double3_array(sv, &d3)); TEST_CHECK(d3.empty());
    TEST_CHECK(parse_double4_array(sv, &d4)); TEST_CHECK(d4.empty());
  }

  // --- Missing opening bracket ---
  {
    tstring_view sv("(1, 2)");
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("1, 2, 3");
    std::vector<float3> r;
    TEST_CHECK(!parse_float3_array(sv, &r));
  }

  // --- Missing opening paren for tuple ---
  {
    tstring_view sv("[1, 2]");  // no parens around tuple
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("[1, 2, 3]");
    std::vector<double3> r;
    TEST_CHECK(!parse_double3_array(sv, &r));
  }

  // --- Missing closing paren for tuple ---
  {
    tstring_view sv("[(1, 2]");  // missing ')'
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("[(1, 2, 3]");
    std::vector<float3> r;
    TEST_CHECK(!parse_float3_array(sv, &r));
  }
  {
    tstring_view sv("[(1, 2, 3, 4]");
    std::vector<double4> r;
    TEST_CHECK(!parse_double4_array(sv, &r));
  }

  // --- Missing comma between elements in tuple ---
  {
    tstring_view sv("[(1 2)]");  // missing comma
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("[(1 2 3)]");
    std::vector<float3> r;
    TEST_CHECK(!parse_float3_array(sv, &r));
  }
  {
    tstring_view sv("[(1 2 3 4)]");
    std::vector<double4> r;
    TEST_CHECK(!parse_double4_array(sv, &r));
  }

  // --- Non-numeric content in tuple ---
  {
    tstring_view sv("[(abc, 2)]");
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("[(1, abc)]");
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("[(1, abc, 3)]");
    std::vector<double3> r;
    TEST_CHECK(!parse_double3_array(sv, &r));
  }

  // --- Too few elements in tuple ---
  {
    tstring_view sv("[(1)]");  // float2 needs 2
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("[(1, 2)]");  // float3 needs 3
    std::vector<float3> r;
    TEST_CHECK(!parse_float3_array(sv, &r));
  }
  {
    tstring_view sv("[(1, 2, 3)]");  // float4 needs 4
    std::vector<float4> r;
    TEST_CHECK(!parse_float4_array(sv, &r));
  }

  // --- Truncated input (abrupt end) ---
  {
    tstring_view sv("[(1,");  // ends mid-tuple
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("[(1, 2");  // no closing paren or bracket
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("[");  // just opening bracket, no content — parser returns empty
    std::vector<float3> r;
    TEST_CHECK(parse_float3_array(sv, &r));
    TEST_CHECK(r.empty());
  }
  {
    tstring_view sv("[(");  // opening bracket and paren
    std::vector<double4> r;
    TEST_CHECK(!parse_double4_array(sv, &r));
  }

  // --- Negative and scientific notation values ---
  {
    tstring_view sv("[(-1.5, 2.5e3), (-3.14, 0)]");
    std::vector<float2> r;
    TEST_CHECK(parse_float2_array(sv, &r));
    TEST_CHECK(r.size() == 2);
    TEST_CHECK(r[0][0] == -1.5f);
    TEST_CHECK(r[0][1] == 2500.0f);
    TEST_CHECK(r[1][0] == -3.14f);
    TEST_CHECK(r[1][1] == 0.0f);
  }
  {
    tstring_view sv("[(-1e-5, 2.0e+10, -3)]");
    std::vector<double3> r;
    TEST_CHECK(parse_double3_array(sv, &r));
    TEST_CHECK(r.size() == 1);
    TEST_CHECK(r[0][0] == -1e-5);
    TEST_CHECK(r[0][1] == 2.0e+10);
    TEST_CHECK(r[0][2] == -3.0);
  }

  // --- Whitespace variations ---
  {
    tstring_view sv(" [ ( 1 , 2 ) , ( 3 , 4 ) ] ");
    std::vector<float2> r;
    TEST_CHECK(parse_float2_array(sv, &r));
    TEST_CHECK(r.size() == 2);
    TEST_CHECK(r[0][0] == 1.0f);
    TEST_CHECK(r[1][1] == 4.0f);
  }
  {
    // Newlines and tabs (multi-line format as in USDA files)
    tstring_view sv("[\n\t(1, 2, 3),\n\t(4, 5, 6)\n]");
    std::vector<float3> r;
    TEST_CHECK(parse_float3_array(sv, &r));
    TEST_CHECK(r.size() == 2);
    TEST_CHECK(r[0][0] == 1.0f);
    TEST_CHECK(r[1][2] == 6.0f);
  }

  // --- Trailing comma after last tuple (common in USDA) ---
  {
    tstring_view sv("[(1, 2),]");
    std::vector<float2> r;
    // Trailing comma before ']' — parser skips whitespace and sees ']'
    TEST_CHECK(parse_float2_array(sv, &r));
    TEST_CHECK(r.size() == 1);
  }
  {
    tstring_view sv("[(1, 2, 3),]");
    std::vector<double3> r;
    TEST_CHECK(parse_double3_array(sv, &r));
    TEST_CHECK(r.size() == 1);
  }

  // ==========================================================================
  // Matrix error cases
  // ==========================================================================

  // --- Null pointer for each matrix type ---
  {
    tstring_view sv("[((1, 0), (0, 1))]");
    TEST_CHECK(!parse_matrix2f_array(sv, (std::vector<matrix2f> *)nullptr));
    TEST_CHECK(!parse_matrix3f_array(sv, (std::vector<matrix3f> *)nullptr));
    TEST_CHECK(!parse_matrix4f_array(sv, (std::vector<matrix4f> *)nullptr));
    TEST_CHECK(!parse_matrix2d_array(sv, (std::vector<matrix2d> *)nullptr));
    TEST_CHECK(!parse_matrix3d_array(sv, (std::vector<matrix3d> *)nullptr));
    TEST_CHECK(!parse_matrix4d_array(sv, (std::vector<matrix4d> *)nullptr));
  }

  // --- Empty string for each matrix type ---
  {
    tstring_view sv("");
    std::vector<matrix2f> m2f; std::vector<matrix3f> m3f; std::vector<matrix4f> m4f;
    std::vector<matrix2d> m2d; std::vector<matrix3d> m3d; std::vector<matrix4d> m4d;
    TEST_CHECK(!parse_matrix2f_array(sv, &m2f));
    TEST_CHECK(!parse_matrix3f_array(sv, &m3f));
    TEST_CHECK(!parse_matrix4f_array(sv, &m4f));
    TEST_CHECK(!parse_matrix2d_array(sv, &m2d));
    TEST_CHECK(!parse_matrix3d_array(sv, &m3d));
    TEST_CHECK(!parse_matrix4d_array(sv, &m4d));
  }

  // --- Empty arrays for each matrix type ---
  {
    tstring_view sv("[]");
    std::vector<matrix2f> m2f; std::vector<matrix3f> m3f; std::vector<matrix4f> m4f;
    std::vector<matrix2d> m2d; std::vector<matrix3d> m3d; std::vector<matrix4d> m4d;
    TEST_CHECK(parse_matrix2f_array(sv, &m2f)); TEST_CHECK(m2f.empty());
    TEST_CHECK(parse_matrix3f_array(sv, &m3f)); TEST_CHECK(m3f.empty());
    TEST_CHECK(parse_matrix4f_array(sv, &m4f)); TEST_CHECK(m4f.empty());
    TEST_CHECK(parse_matrix2d_array(sv, &m2d)); TEST_CHECK(m2d.empty());
    TEST_CHECK(parse_matrix3d_array(sv, &m3d)); TEST_CHECK(m3d.empty());
    TEST_CHECK(parse_matrix4d_array(sv, &m4d)); TEST_CHECK(m4d.empty());
  }

  // --- Missing opening bracket ---
  {
    tstring_view sv("((1, 0), (0, 1))");
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }

  // --- Missing outer '(' for matrix element ---
  {
    tstring_view sv("[(1, 0), (0, 1)]");  // no outer parens wrapping rows
    std::vector<matrix2d> r;
    TEST_CHECK(!parse_matrix2d_array(sv, &r));
  }

  // --- Missing inner '(' for row ---
  {
    tstring_view sv("[(1, 0, 0, 1)]");  // flat tuple instead of nested rows
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }
  {
    tstring_view sv("[(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1)]");  // flat
    std::vector<matrix4d> r;
    TEST_CHECK(!parse_matrix4d_array(sv, &r));
  }

  // --- Missing inner ')' for row ---
  {
    tstring_view sv("[((1, 0, (0, 1))]");  // first row missing ')'
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }

  // --- Missing outer ')' for matrix element ---
  {
    tstring_view sv("[((1, 0), (0, 1)]");  // missing outer ')'
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }

  // --- Non-numeric content in matrix ---
  {
    tstring_view sv("[((abc, 0), (0, 1))]");
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }
  {
    tstring_view sv("[((1, xyz), (0, 1))]");
    std::vector<matrix2d> r;
    TEST_CHECK(!parse_matrix2d_array(sv, &r));
  }

  // --- Missing comma between row elements ---
  {
    tstring_view sv("[((1 0), (0, 1))]");  // missing comma in first row
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }

  // --- Truncated matrix input ---
  {
    tstring_view sv("[((1, 0), (0,");  // ends mid-row
    std::vector<matrix2d> r;
    TEST_CHECK(!parse_matrix2d_array(sv, &r));
  }
  {
    tstring_view sv("[((1, 0),");  // ends between rows
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }
  {
    tstring_view sv("[((1,");  // ends mid-first-row
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }
  {
    tstring_view sv("[((");  // just outer and inner opening parens
    std::vector<matrix4d> r;
    TEST_CHECK(!parse_matrix4d_array(sv, &r));
  }

  // --- Too few rows ---
  {
    tstring_view sv("[((1, 0))]");  // matrix2f needs 2 rows, only 1
    std::vector<matrix2f> r;
    TEST_CHECK(!parse_matrix2f_array(sv, &r));
  }

  // --- Matrix with whitespace/newlines (real USDA multi-line format) ---
  {
    tstring_view sv(
      "[\n"
      "  ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1)),\n"
      "  ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 2, 1))\n"
      "]"
    );
    std::vector<matrix4d> r;
    TEST_CHECK(parse_matrix4d_array(sv, &r));
    TEST_CHECK(r.size() == 2);
    TEST_CHECK(r[0].m[0][0] == 1.0);
    TEST_CHECK(r[0].m[3][3] == 1.0);
    TEST_CHECK(r[1].m[3][2] == 2.0);
  }

  // --- Trailing comma after last matrix (common in USDA) ---
  {
    tstring_view sv("[((1, 0), (0, 1)),]");
    std::vector<matrix2f> r;
    TEST_CHECK(parse_matrix2f_array(sv, &r));
    TEST_CHECK(r.size() == 1);
  }
  {
    tstring_view sv(
      "[\n"
      "  ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),\n"
      "  ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,4,1)),\n"
      "]"
    );
    std::vector<matrix4d> r;
    TEST_CHECK(parse_matrix4d_array(sv, &r));
    TEST_CHECK(r.size() == 2);
    TEST_CHECK(r[1].m[3][2] == 4.0);
  }

  // --- Matrix with negative and scientific values ---
  {
    tstring_view sv("[((-1.5, 2e3), (0, -0.5))]");
    std::vector<matrix2f> r;
    TEST_CHECK(parse_matrix2f_array(sv, &r));
    TEST_CHECK(r.size() == 1);
    TEST_CHECK(r[0].m[0][0] == -1.5f);
    TEST_CHECK(r[0].m[0][1] == 2000.0f);
    TEST_CHECK(r[0].m[1][1] == -0.5f);
  }

  // --- Three-element matrix4d array (real skeleton bindTransforms) ---
  {
    tstring_view sv(
      "[((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),"
      "((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,2,1)),"
      "((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,4,1))]"
    );
    std::vector<matrix4d> r;
    TEST_CHECK(parse_matrix4d_array(sv, &r));
    TEST_CHECK(r.size() == 3);
    TEST_CHECK(r[0].m[3][2] == 0.0);
    TEST_CHECK(r[1].m[3][2] == 2.0);
    TEST_CHECK(r[2].m[3][2] == 4.0);
  }

  // --- matrix4d with fractional values from real USDA ---
  {
    tstring_view sv(
      "["
      "((0.3778795897960663, 2.9802322387695312e-8, -0.9258614778518677, 0),"
      " (0.37193965911865234, 0.9152927398681641, 0.15180081129074097, 0),"
      " (0.8473281860351562, -0.40277791023254395, 0.34588557481765747, 0),"
      " (0, 0, 0, 1))"
      "]"
    );
    std::vector<matrix4d> r;
    TEST_CHECK(parse_matrix4d_array(sv, &r));
    TEST_CHECK(r.size() == 1);
    // Spot-check a few values
    TEST_CHECK(r[0].m[0][0] == 0.3778795897960663);
    TEST_CHECK(r[0].m[3][3] == 1.0);
    TEST_CHECK(r[0].m[0][3] == 0.0);
  }

  // --- Only whitespace (no array) ---
  {
    tstring_view sv("   ");
    std::vector<float2> r;
    TEST_CHECK(!parse_float2_array(sv, &r));
  }
  {
    tstring_view sv("  \n\t  ");
    std::vector<matrix4d> r;
    TEST_CHECK(!parse_matrix4d_array(sv, &r));
  }

  // --- Garbage after valid array (parser doesn't look past ']') ---
  {
    tstring_view sv("[(1, 2)]garbage");
    std::vector<float2> r;
    TEST_CHECK(parse_float2_array(sv, &r));
    TEST_CHECK(r.size() == 1);
    TEST_CHECK(r[0][0] == 1.0f);
    TEST_CHECK(r[0][1] == 2.0f);
  }

  // --- Single-element arrays ---
  {
    tstring_view sv("[(42.0, -1.0)]");
    std::vector<float2> r;
    TEST_CHECK(parse_float2_array(sv, &r));
    TEST_CHECK(r.size() == 1);
  }
  {
    tstring_view sv("[(1, 2, 3, 4)]");
    std::vector<double4> r;
    TEST_CHECK(parse_double4_array(sv, &r));
    TEST_CHECK(r.size() == 1);
    TEST_CHECK(r[0][0] == 1.0);
    TEST_CHECK(r[0][1] == 2.0);
    TEST_CHECK(r[0][2] == 3.0);
    TEST_CHECK(r[0][3] == 4.0);
  }
  {
    tstring_view sv("[((1, 0, 0), (0, 1, 0), (0, 0, 1))]");
    std::vector<matrix3d> r;
    TEST_CHECK(parse_matrix3d_array(sv, &r));
    TEST_CHECK(r.size() == 1);
  }

  // --- Empty array with whitespace ---
  {
    tstring_view sv("[  ]");
    std::vector<float3> r;
    TEST_CHECK(parse_float3_array(sv, &r));
    TEST_CHECK(r.empty());
  }
  {
    tstring_view sv("[ \n\t ]");
    std::vector<matrix4d> r;
    TEST_CHECK(parse_matrix4d_array(sv, &r));
    TEST_CHECK(r.empty());
  }
}

void fp_string_conversion_test(void) {
  auto nearly_equal = [](double a, double b, double eps) {
    return std::fabs(a - b) <= eps * (std::fabs(a) + std::fabs(b) + 1.0);
  };

  auto roundtrip_double = [&](double v, double eps) {
    std::string s = dtos(v);
    double parsed = lightusd::legacy::atof(s);
    TEST_CHECK_(nearly_equal(parsed, v, eps), "double roundtrip failed: v=%g, s=%s, parsed=%g", v, s.c_str(), parsed);

    char buf[DTOS_MAX_CHARS_DOUBLE + 2];
    buf[DTOS_MAX_CHARS_DOUBLE + 1] = '*';  // sentinel
    size_t len = dtos(v, buf);
    std::string buf_str(buf, len);
    TEST_CHECK_(buf_str == s, "double buffer string mismatch: expected %s, got %s", s.c_str(), buf_str.c_str());
    TEST_CHECK(buf[DTOS_MAX_CHARS_DOUBLE + 1] == '*');  // no overflow
  };

  auto roundtrip_float = [&](float v, float eps) {
    std::string s = dtos(v);
    double parsed = lightusd::legacy::atof(s);
    TEST_CHECK_(nearly_equal(parsed, static_cast<double>(v), eps), "float roundtrip failed: v=%g, s=%s, parsed=%g", v, s.c_str(), parsed);

    char buf[DTOS_MAX_CHARS_FLOAT + 2];
    buf[DTOS_MAX_CHARS_FLOAT + 1] = '*';  // sentinel
    size_t len = dtos(v, buf);
    std::string buf_str(buf, len);
    TEST_CHECK_(buf_str == s, "float buffer string mismatch: expected %s, got %s", s.c_str(), buf_str.c_str());
    TEST_CHECK(buf[DTOS_MAX_CHARS_FLOAT + 1] == '*');  // no overflow
  };

  // Fast-path values
  TEST_CHECK(dtos(1.0) == "1");
  TEST_CHECK(dtos(-1.0) == "-1");
  TEST_CHECK(dtos(1.0f) == "1");
  TEST_CHECK(dtos(-1.0f) == "-1");

  // Representative doubles
  roundtrip_double(0.0, 1e-15);
  roundtrip_double(-0.0, 1e-15);
  roundtrip_double(123456.789, 1e-12);
  roundtrip_double(-9876.54321, 1e-12);
  roundtrip_double(1e-6, 1e-12);
  roundtrip_double(1e20, 1e-12);
  roundtrip_double(-1e-8, 1e-12);

  // Representative floats
  roundtrip_float(0.0f, 1e-6f);
  roundtrip_float(-0.0f, 1e-6f);
  roundtrip_float(1234.5f, 1e-5f);
  roundtrip_float(-9876.5f, 1e-5f);
  roundtrip_float(1e-4f, 1e-5f);
  roundtrip_float(1e6f, 1e-5f);
}

// Test human-readable format ranges:
// - Double: [1e-4, 1e16) uses decimal format, outside uses scientific
// - Float: [1e-4, 1e7) uses decimal format, outside uses scientific
void fp_format_range_test(void) {
  // Helper: check if string uses scientific notation
  auto is_scientific = [](const std::string& s) -> bool {
    return s.find('e') != std::string::npos || s.find('E') != std::string::npos;
  };

  // Helper: count true significant digits (excludes leading zeros)
  auto count_sig_digits = [](const std::string& s) -> size_t {
    size_t count = 0;
    bool started = false;
    bool in_exponent = false;
    for (char c : s) {
      if (c == 'e' || c == 'E') in_exponent = true;
      else if (!in_exponent && c >= '0' && c <= '9') {
        if (c != '0' || started) { started = true; count++; }
      }
    }
    return count;
  };

  // Helper: verify roundtrip for double
  auto check_double_roundtrip = [](double v) {
    std::string s = dtos(v);
    double parsed = std::strtod(s.c_str(), nullptr);
    TEST_CHECK_(parsed == v || (std::isnan(parsed) && std::isnan(v)),
                "double roundtrip failed: v=%.17g, s='%s', parsed=%.17g", v, s.c_str(), parsed);
  };

  // Helper: verify roundtrip for float
  auto check_float_roundtrip = [](float v) {
    std::string s = dtos(v);
    float parsed = std::strtof(s.c_str(), nullptr);
    TEST_CHECK_(parsed == v || (std::isnan(parsed) && std::isnan(v)),
                "float roundtrip failed: v=%.9g, s='%s', parsed=%.9g", v, s.c_str(), parsed);
  };

  // =========================================================================
  // DOUBLE FORMAT RANGE TESTS [1e-4, 1e16)
  // =========================================================================

  // Below lower boundary (< 1e-4) -> SCIENTIFIC
  TEST_CHECK_(is_scientific(dtos(0.00001)), "0.00001 should be scientific: %s", dtos(0.00001).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-5)), "1e-5 should be scientific: %s", dtos(1e-5).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-6)), "1e-6 should be scientific: %s", dtos(1e-6).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-7)), "1e-7 should be scientific: %s", dtos(1e-7).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-8)), "1e-8 should be scientific: %s", dtos(1e-8).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-10)), "1e-10 should be scientific: %s", dtos(1e-10).c_str());
  TEST_CHECK_(is_scientific(dtos(9.9999e-5)), "9.9999e-5 should be scientific: %s", dtos(9.9999e-5).c_str());
  TEST_CHECK_(is_scientific(dtos(9.999999e-5)), "9.999999e-5 should be scientific: %s", dtos(9.999999e-5).c_str());
  TEST_CHECK_(is_scientific(dtos(0.000099)), "0.000099 should be scientific: %s", dtos(0.000099).c_str());
  TEST_CHECK_(is_scientific(dtos(0.0000999)), "0.0000999 should be scientific: %s", dtos(0.0000999).c_str());
  TEST_CHECK_(is_scientific(dtos(0.00009999)), "0.00009999 should be scientific: %s", dtos(0.00009999).c_str());

  // Negative values below boundary -> SCIENTIFIC
  TEST_CHECK_(is_scientific(dtos(-0.00001)), "-0.00001 should be scientific: %s", dtos(-0.00001).c_str());
  TEST_CHECK_(is_scientific(dtos(-1e-5)), "-1e-5 should be scientific: %s", dtos(-1e-5).c_str());
  TEST_CHECK_(is_scientific(dtos(-1e-6)), "-1e-6 should be scientific: %s", dtos(-1e-6).c_str());

  // At/above lower boundary (>= 1e-4) -> DECIMAL
  TEST_CHECK_(!is_scientific(dtos(1e-4)), "1e-4 should be decimal: %s", dtos(1e-4).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.0001)), "0.0001 should be decimal: %s", dtos(0.0001).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.00010001)), "0.00010001 should be decimal: %s", dtos(0.00010001).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.0002)), "0.0002 should be decimal: %s", dtos(0.0002).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.0005)), "0.0005 should be decimal: %s", dtos(0.0005).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.001)), "0.001 should be decimal: %s", dtos(0.001).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.01)), "0.01 should be decimal: %s", dtos(0.01).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.1)), "0.1 should be decimal: %s", dtos(0.1).c_str());
  TEST_CHECK_(!is_scientific(dtos(1.0)), "1.0 should be decimal: %s", dtos(1.0).c_str());
  TEST_CHECK_(!is_scientific(dtos(10.0)), "10.0 should be decimal: %s", dtos(10.0).c_str());
  TEST_CHECK_(!is_scientific(dtos(100.0)), "100.0 should be decimal: %s", dtos(100.0).c_str());
  TEST_CHECK_(!is_scientific(dtos(1000.0)), "1000.0 should be decimal: %s", dtos(1000.0).c_str());
  TEST_CHECK_(!is_scientific(dtos(1e10)), "1e10 should be decimal: %s", dtos(1e10).c_str());
  TEST_CHECK_(!is_scientific(dtos(1e15)), "1e15 should be decimal: %s", dtos(1e15).c_str());

  // Negative values in decimal range -> DECIMAL
  TEST_CHECK_(!is_scientific(dtos(-0.0001)), "-0.0001 should be decimal: %s", dtos(-0.0001).c_str());
  TEST_CHECK_(!is_scientific(dtos(-0.001)), "-0.001 should be decimal: %s", dtos(-0.001).c_str());
  TEST_CHECK_(!is_scientific(dtos(-1.0)), "-1.0 should be decimal: %s", dtos(-1.0).c_str());
  TEST_CHECK_(!is_scientific(dtos(-123.456)), "-123.456 should be decimal: %s", dtos(-123.456).c_str());

  // At/above upper boundary (>= 1e16) -> SCIENTIFIC
  TEST_CHECK_(is_scientific(dtos(1e16)), "1e16 should be scientific: %s", dtos(1e16).c_str());
  TEST_CHECK_(is_scientific(dtos(1e17)), "1e17 should be scientific: %s", dtos(1e17).c_str());
  TEST_CHECK_(is_scientific(dtos(1e20)), "1e20 should be scientific: %s", dtos(1e20).c_str());
  TEST_CHECK_(is_scientific(dtos(1e100)), "1e100 should be scientific: %s", dtos(1e100).c_str());
  TEST_CHECK_(is_scientific(dtos(-1e16)), "-1e16 should be scientific: %s", dtos(-1e16).c_str());

  // =========================================================================
  // FLOAT FORMAT RANGE TESTS [1e-4, 1e7)
  // =========================================================================

  // Below lower boundary (< 1e-4) -> SCIENTIFIC
  TEST_CHECK_(is_scientific(dtos(0.00001f)), "0.00001f should be scientific: %s", dtos(0.00001f).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-5f)), "1e-5f should be scientific: %s", dtos(1e-5f).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-6f)), "1e-6f should be scientific: %s", dtos(1e-6f).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-7f)), "1e-7f should be scientific: %s", dtos(1e-7f).c_str());
  TEST_CHECK_(is_scientific(dtos(9.999e-5f)), "9.999e-5f should be scientific: %s", dtos(9.999e-5f).c_str());
  TEST_CHECK_(is_scientific(dtos(-1e-5f)), "-1e-5f should be scientific: %s", dtos(-1e-5f).c_str());

  // At/above lower boundary (>= 1e-4) -> DECIMAL
  TEST_CHECK_(!is_scientific(dtos(0.0001f)), "0.0001f should be decimal: %s", dtos(0.0001f).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.001f)), "0.001f should be decimal: %s", dtos(0.001f).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.01f)), "0.01f should be decimal: %s", dtos(0.01f).c_str());
  TEST_CHECK_(!is_scientific(dtos(0.1f)), "0.1f should be decimal: %s", dtos(0.1f).c_str());
  TEST_CHECK_(!is_scientific(dtos(1.0f)), "1.0f should be decimal: %s", dtos(1.0f).c_str());
  TEST_CHECK_(!is_scientific(dtos(10.0f)), "10.0f should be decimal: %s", dtos(10.0f).c_str());
  TEST_CHECK_(!is_scientific(dtos(100.0f)), "100.0f should be decimal: %s", dtos(100.0f).c_str());
  TEST_CHECK_(!is_scientific(dtos(1000.0f)), "1000.0f should be decimal: %s", dtos(1000.0f).c_str());
  TEST_CHECK_(!is_scientific(dtos(1e6f)), "1e6f should be decimal: %s", dtos(1e6f).c_str());
  TEST_CHECK_(!is_scientific(dtos(9999999.0f)), "9999999.0f should be decimal: %s", dtos(9999999.0f).c_str());
  TEST_CHECK_(!is_scientific(dtos(-0.001f)), "-0.001f should be decimal: %s", dtos(-0.001f).c_str());
  TEST_CHECK_(!is_scientific(dtos(-123.456f)), "-123.456f should be decimal: %s", dtos(-123.456f).c_str());

  // At/above upper boundary (>= 1e7) -> SCIENTIFIC
  TEST_CHECK_(is_scientific(dtos(1e7f)), "1e7f should be scientific: %s", dtos(1e7f).c_str());
  TEST_CHECK_(is_scientific(dtos(1e8f)), "1e8f should be scientific: %s", dtos(1e8f).c_str());
  TEST_CHECK_(is_scientific(dtos(1e10f)), "1e10f should be scientific: %s", dtos(1e10f).c_str());
  TEST_CHECK_(is_scientific(dtos(1e20f)), "1e20f should be scientific: %s", dtos(1e20f).c_str());
  TEST_CHECK_(is_scientific(dtos(-1e7f)), "-1e7f should be scientific: %s", dtos(-1e7f).c_str());

  // =========================================================================
  // EXTREME VALUES -> SCIENTIFIC
  // =========================================================================
  TEST_CHECK_(is_scientific(dtos(1e-100)), "1e-100 should be scientific: %s", dtos(1e-100).c_str());
  TEST_CHECK_(is_scientific(dtos(1e-50)), "1e-50 should be scientific: %s", dtos(1e-50).c_str());
  TEST_CHECK_(is_scientific(dtos(1e50)), "1e50 should be scientific: %s", dtos(1e50).c_str());
  TEST_CHECK_(is_scientific(dtos(1e100)), "1e100 should be scientific: %s", dtos(1e100).c_str());
  TEST_CHECK_(is_scientific(dtos(1e200)), "1e200 should be scientific: %s", dtos(1e200).c_str());
  TEST_CHECK_(is_scientific(dtos(std::numeric_limits<double>::min())), "DBL_MIN should be scientific: %s", dtos(std::numeric_limits<double>::min()).c_str());
  TEST_CHECK_(is_scientific(dtos(std::numeric_limits<double>::max())), "DBL_MAX should be scientific: %s", dtos(std::numeric_limits<double>::max()).c_str());
  TEST_CHECK_(is_scientific(dtos(std::numeric_limits<float>::min())), "FLT_MIN should be scientific: %s", dtos(std::numeric_limits<float>::min()).c_str());
  TEST_CHECK_(is_scientific(dtos(std::numeric_limits<float>::max())), "FLT_MAX should be scientific: %s", dtos(std::numeric_limits<float>::max()).c_str());

  // =========================================================================
  // SHORTEST REPRESENTATION TESTS
  // =========================================================================

  // Verify common values produce short output
  {
    std::string s = dtos(0.1);
    TEST_CHECK_(s == "0.1", "0.1 should produce '0.1', got '%s'", s.c_str());
  }
  {
    std::string s = dtos(0.5);
    TEST_CHECK_(s == "0.5", "0.5 should produce '0.5', got '%s'", s.c_str());
  }
  {
    std::string s = dtos(0.25);
    TEST_CHECK_(s == "0.25", "0.25 should produce '0.25', got '%s'", s.c_str());
  }
  {
    std::string s = dtos(0.125);
    TEST_CHECK_(s == "0.125", "0.125 should produce '0.125', got '%s'", s.c_str());
  }

  // Float shortest representation
  {
    std::string s = dtos(0.707f);
    size_t digits = count_sig_digits(s);
    TEST_CHECK_(digits <= 9, "0.707f should have <= 9 sig digits, got %zu in '%s'", digits, s.c_str());
    check_float_roundtrip(0.707f);
  }
  {
    std::string s = dtos(0.1f);
    TEST_CHECK_(s == "0.1", "0.1f should produce '0.1', got '%s'", s.c_str());
  }

  // =========================================================================
  // ROUNDTRIP VERIFICATION FOR KEY VALUES
  // =========================================================================

  // Doubles at boundaries
  check_double_roundtrip(0.00001);
  check_double_roundtrip(0.0001);
  check_double_roundtrip(0.001);
  check_double_roundtrip(1e-5);
  check_double_roundtrip(1e-4);
  check_double_roundtrip(1e15);
  check_double_roundtrip(1e16);
  check_double_roundtrip(9.999999999999999e15);

  // Floats at boundaries
  check_float_roundtrip(0.00001f);
  check_float_roundtrip(0.0001f);
  check_float_roundtrip(1e-5f);
  check_float_roundtrip(1e-4f);
  check_float_roundtrip(1e6f);
  check_float_roundtrip(1e7f);
  check_float_roundtrip(9999999.0f);

  // Common problematic values
  check_double_roundtrip(0.1);
  check_double_roundtrip(0.2);
  check_double_roundtrip(0.3);
  check_double_roundtrip(1.0/3.0);
  check_double_roundtrip(2.0/3.0);
  check_float_roundtrip(0.1f);
  check_float_roundtrip(0.2f);
  check_float_roundtrip(0.3f);
  check_float_roundtrip(0.707f);
  check_float_roundtrip(3.14159f);
}
