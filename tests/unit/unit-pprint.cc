#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-pprint.h"
#include "core/prim.hh"
#include "core/prim-enums.hh"
#include "value-types.hh"
#include "value-pprint.hh"
#include "pprinter.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

void value_type_pprint_test(void) {

  {
    std::stringstream ss;
    lightusd::legacy::Interpolation interp = lightusd::legacy::Interpolation::Vertex;
    ss << interp;
    TEST_CHECK(ss.str() == "vertex");
  }

  {
    lightusd::legacy::value::normal3f v{1.0f, 2.0f, 3.f};
    std::string s = to_string(v);
    TEST_CHECK(s == "(1, 2, 3)");
  }
}

void column_wrap_disabled_test(void) {
  // Default: column wrap OFF — arrays stay on single line
  pprint::SetColumnLimit(0);

  std::vector<float> v = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  std::stringstream ss;
  ss << v;
  std::string result = ss.str();
  TEST_CHECK(result.find('\n') == std::string::npos);
  TEST_CHECK(result == "[1, 2, 3, 4, 5]");
}

void column_wrap_float3_array_test(void) {
  pprint::SetColumnLimit(80);

  // Build an array of float3 that will exceed 80 columns
  std::vector<lightusd::legacy::value::float3> v;
  for (int i = 0; i < 10; i++) {
    v.push_back({static_cast<float>(i), static_cast<float>(i + 1),
                 static_cast<float>(i + 2)});
  }

  // Simulate prefix: "    float3[] points = " (22 chars)
  {
    pprint::ScopedPrefixColumns spc(22);
    std::stringstream ss;
    ss << v;
    std::string result = ss.str();

    // Should contain newlines (wrapped)
    TEST_CHECK(result.find('\n') != std::string::npos);

    // Should start with '['
    TEST_CHECK(result[0] == '[');

    // Should end with ']'
    TEST_CHECK(result.back() == ']');

    // Continuation lines should be indented to column 23 (after '[')
    size_t nl_pos = result.find('\n');
    if (nl_pos != std::string::npos) {
      // Count leading spaces on next line
      size_t spaces = 0;
      for (size_t j = nl_pos + 1; j < result.size() && result[j] == ' '; j++) {
        spaces++;
      }
      TEST_CHECK(spaces == 23);
    }

    // No line (including prefix) should exceed 80 columns
    std::istringstream iss(result);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
      if (first) {
        // First line has the prefix too
        TEST_CHECK(22 + line.size() <= 80);
        first = false;
      } else {
        TEST_CHECK(line.size() <= 80);
      }
    }
  }

  pprint::SetColumnLimit(0);
}

void column_wrap_int_array_test(void) {
  pprint::SetColumnLimit(40);

  std::vector<int32_t> v;
  for (int i = 0; i < 20; i++) {
    v.push_back(i * 100);
  }

  // Simulate prefix of 10 chars
  {
    pprint::ScopedPrefixColumns spc(10);
    std::stringstream ss;
    ss << v;
    std::string result = ss.str();

    // Should wrap
    TEST_CHECK(result.find('\n') != std::string::npos);

    // Should start with '[' and end with ']'
    TEST_CHECK(result[0] == '[');
    TEST_CHECK(result.back() == ']');
  }

  pprint::SetColumnLimit(0);
}

void column_wrap_deep_indent_test(void) {
  pprint::SetColumnLimit(80);

  std::vector<float> v = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};

  // Simulate a very deep prefix (60 chars, > 80*0.6=48)
  {
    pprint::ScopedPrefixColumns spc(60);
    std::stringstream ss;
    ss << v;
    std::string result = ss.str();

    // Should start with newline (deep indent fallback)
    TEST_CHECK(result[0] == '\n');

    // The continuation should use single indent (4 spaces) + "["
    size_t bracket_pos = result.find('[');
    TEST_CHECK(bracket_pos != std::string::npos);
    // After newline, should have 4 spaces then '['
    TEST_CHECK(result.substr(1, 5) == "    [");
  }

  pprint::SetColumnLimit(0);
}

void column_wrap_string_no_wrap_test(void) {
  pprint::SetColumnLimit(40);

  std::vector<lightusd::legacy::value::token> v;
  v.push_back(lightusd::legacy::value::token("hello"));
  v.push_back(lightusd::legacy::value::token("world"));
  v.push_back(lightusd::legacy::value::token("this_is_a_long_token_name"));

  {
    pprint::ScopedPrefixColumns spc(10);
    std::stringstream ss;
    ss << v;
    std::string result = ss.str();

    // Tokens should NOT be wrapped (not a numeric type)
    TEST_CHECK(result.find('\n') == std::string::npos);
  }

  pprint::SetColumnLimit(0);
}

