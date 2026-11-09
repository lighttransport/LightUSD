#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-ascii-parse.h"
#include "lightusd.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Helper function to parse a USD string
static bool parseUSDString(const std::string &usd_content, std::string *err) {
  Stage stage;
  std::string warn;

  bool ret = LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd_content.data()),
      usd_content.size(),
      "memory.usda",
      &stage,
      &warn,
      err);

  return ret;
}

//
// int64_t digit length guard tests
// These tests verify that excessively long integer literals are rejected
// to prevent denial-of-service attacks via parser resource exhaustion
//

void ascii_parse_int64_valid_test(void) {
  std::string err;

  // Test value under the digit limit (should succeed)
  {
    std::string usd = R"(#usda 1.0
def Xform "Test" {
    int64 testValue = 9223372036854775807
}
)";
    bool ret = parseUSDString(usd, &err);
    TEST_CHECK(ret == true);
  }
}

void ascii_parse_int64_excessive_digits_test(void) {
  std::string err;

  // Test 22 digits (over the 21 digit limit) - should fail
  {
    std::string usd = R"(#usda 1.0
def Xform "Test" {
    int64 testExcessive = 1234567890123456789012
}
)";
    bool ret = parseUSDString(usd, &err);
    TEST_CHECK(ret == false);
  }

  // Test 30 digits - should fail
  {
    std::string usd = R"(#usda 1.0
def Xform "Test" {
    int64 testHuge = 123456789012345678901234567890
}
)";
    bool ret = parseUSDString(usd, &err);
    TEST_CHECK(ret == false);
  }
}

//
// uint64_t digit length guard tests
//

void ascii_parse_uint64_valid_test(void) {
  std::string err;

  // Test value under the digit limit (should succeed)
  {
    std::string usd = R"(#usda 1.0
def Xform "Test" {
    uint64 testValue = 18446744073709551615
}
)";
    bool ret = parseUSDString(usd, &err);
    TEST_CHECK(ret == true);
  }
}

void ascii_parse_uint64_excessive_digits_test(void) {
  std::string err;

  // Test 23 digits (over the 22 digit limit) - should fail
  {
    std::string usd = R"(#usda 1.0
def Xform "Test" {
    uint64 testExcessive = 12345678901234567890123
}
)";
    bool ret = parseUSDString(usd, &err);
    TEST_CHECK(ret == false);
  }

  // Test 30 digits - should fail
  {
    std::string usd = R"(#usda 1.0
def Xform "Test" {
    uint64 testHuge = 123456789012345678901234567890
}
)";
    bool ret = parseUSDString(usd, &err);
    TEST_CHECK(ret == false);
  }
}

//
// Regression tests: '#' comments inside arrays
// The optimized tiny-string parsers (float[], double[], int[]) previously
// failed when arrays contained '#' comments between elements.
//

void ascii_parse_array_comments_int_test(void) {
  std::string err;
  Stage stage;
  std::string warn;

  std::string usd = R"(#usda 1.0
def Scope "Test" {
    custom int[] indices = [
        # first group
        0, 1, 2, 3,
        # second group
        4, 5, 6, 7
    ]
}
)";

  bool ret = LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd.data()),
      usd.size(), "memory.usda", &stage, &warn, &err);

  TEST_CHECK(ret == true);
  if (!ret) {
    TEST_MSG("Failed to parse int[] with comments: %s", err.c_str());
    return;
  }

  std::string exported = stage.ExportToString();
  // Verify all 8 values are present
  TEST_CHECK(exported.find("int[] indices") != std::string::npos);
  TEST_CHECK(exported.find("0, 1, 2, 3, 4, 5, 6, 7") != std::string::npos);
}

void ascii_parse_array_comments_float_test(void) {
  std::string err;
  Stage stage;
  std::string warn;

  std::string usd = R"(#usda 1.0
def Scope "Test" {
    custom float[] weights = [
        # root weights
        1.0, 0.0, 1.0, 0.0,
        # blended weights
        0.6, 0.4, 0.8, 0.2
        # trailing comment before bracket
    ]
}
)";

  bool ret = LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd.data()),
      usd.size(), "memory.usda", &stage, &warn, &err);

  TEST_CHECK(ret == true);
  if (!ret) {
    TEST_MSG("Failed to parse float[] with comments: %s", err.c_str());
    return;
  }

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("float[] weights") != std::string::npos);
  TEST_CHECK(exported.find("0.6") != std::string::npos);
  TEST_CHECK(exported.find("0.8") != std::string::npos);
}

void ascii_parse_array_comments_double_test(void) {
  std::string err;
  Stage stage;
  std::string warn;

  std::string usd = R"(#usda 1.0
def Scope "Test" {
    custom double[] values = [
        # positive values
        1.0, 2.5, 3.14159,
        # negative values
        -1.0, -2.5, -0.001
    ]
}
)";

  bool ret = LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd.data()),
      usd.size(), "memory.usda", &stage, &warn, &err);

  TEST_CHECK(ret == true);
  if (!ret) {
    TEST_MSG("Failed to parse double[] with comments: %s", err.c_str());
    return;
  }

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("double[] values") != std::string::npos);
  TEST_CHECK(exported.find("3.14159") != std::string::npos);
  TEST_CHECK(exported.find("-2.5") != std::string::npos);
}

void ascii_parse_array_comments_matrix4d_test(void) {
  std::string err;
  Stage stage;
  std::string warn;

  std::string usd = R"(#usda 1.0
def Scope "Test" {
    custom matrix4d[] xforms = [
        # identity matrix
        ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) ),
        # translation (0, 1, 0)
        ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 0, 1) )
    ]
}
)";

  bool ret = LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd.data()),
      usd.size(), "memory.usda", &stage, &warn, &err);

  TEST_CHECK(ret == true);
  if (!ret) {
    TEST_MSG("Failed to parse matrix4d[] with comments: %s", err.c_str());
    return;
  }

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("matrix4d[] xforms") != std::string::npos);
}

void ascii_parse_array_comments_token_test(void) {
  std::string err;
  Stage stage;
  std::string warn;

  std::string usd = R"(#usda 1.0
def Scope "Test" {
    custom token[] joints = [
        # root joint
        "Root",
        # spine chain
        "Root/Spine",
        "Root/Spine/Head"
    ]
}
)";

  bool ret = LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd.data()),
      usd.size(), "memory.usda", &stage, &warn, &err);

  TEST_CHECK(ret == true);
  if (!ret) {
    TEST_MSG("Failed to parse token[] with comments: %s", err.c_str());
    return;
  }

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("Root/Spine/Head") != std::string::npos);
}

void ascii_parse_array_comments_quatf_test(void) {
  std::string err;
  Stage stage;
  std::string warn;

  std::string usd = R"(#usda 1.0
def Scope "Test" {
    custom quatf[] rotations = [
        # identity
        (1, 0, 0, 0),
        # 30 deg around Y
        (0.9659, 0, 0.2588, 0),
        # back to identity
        (1, 0, 0, 0)
    ]
}
)";

  bool ret = LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd.data()),
      usd.size(), "memory.usda", &stage, &warn, &err);

  TEST_CHECK(ret == true);
  if (!ret) {
    TEST_MSG("Failed to parse quatf[] with comments: %s", err.c_str());
    return;
  }

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("quatf[] rotations") != std::string::npos);
  TEST_CHECK(exported.find("0.2588") != std::string::npos);
}

// Verify string[] values are parsed and exported with proper USD quoting
void ascii_parse_string_array_test(void) {
  std::string err;
  std::string warn;
  Stage stage;

  std::string usd = R"(#usda 1.0
def Xform "StrArray" {
    string[] labels = ["alpha", "beta", "g\"amma"]
}
)";

  bool ret = LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usd.data()),
      usd.size(),
      "memory.usda",
      &stage,
      &warn,
      &err);

  TEST_CHECK(ret == true);
  if (!ret) {
    TEST_MSG("Failed to parse string array USD: %s", err.c_str());
    return;
  }

  std::string exported = stage.ExportToString();

  TEST_CHECK(exported.find("string[] labels") != std::string::npos);
  TEST_CHECK(exported.find("alpha") != std::string::npos);
  TEST_CHECK(exported.find("beta") != std::string::npos);
  bool found_gamma = (exported.find("g\\\"amma") != std::string::npos) ||
                     (exported.find("g\"amma") != std::string::npos);
  TEST_CHECK(found_gamma);
}
