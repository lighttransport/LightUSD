#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-minijson.h"

#include "minijson.hh"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using lightusd::minijson::Error;
using lightusd::minijson::Parse;
using lightusd::minijson::ParseOptions;
using lightusd::minijson::Serialize;
using lightusd::minijson::Value;

void minijson_shortest_double_roundtrip_test(void) {
  TEST_CHECK(Value(0.1).dump() == "0.1");
  TEST_CHECK(Value(-0.0).dump() == "-0");
  const double edges[] = {
      std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::min(),
      (std::numeric_limits<double>::max)(),
      std::nextafter(1.0, 2.0), std::nextafter(1.0, 0.0),
      1.2345678901234567, -1.2345678901234567};
  auto check = [](double input) {
    std::string text;
    Error error;
    TEST_CHECK(Serialize(Value(input), &text, &error));
    Value parsed;
    TEST_CHECK(Parse(text, &parsed, &error));
    double output = 0;
    TEST_CHECK(parsed.as_double(&output));
    TEST_CHECK(std::memcmp(&input, &output, sizeof(input)) == 0);
  };
  for (double input : edges) check(input);
  // Cover exponent ranges and rounding boundaries without relying on a
  // platform-specific random distribution or decimal reference formatter.
  uint64_t state = UINT64_C(0xa13628b5e04d79cf);
  for (int i = 0; i < 4096; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    double input;
    std::memcpy(&input, &state, sizeof(input));
    if (std::isfinite(input) && input != 0.0) check(input);
  }
}

void minijson_parse_basic_test(void) {
  const std::string src =
      R"({"name":"mesh","count":3,"ok":true,"items":[1,2.5,null]})";
  Value v;
  Error err;
  bool ok = Parse(src, &v, &err);
  TEST_CHECK(ok);
  TEST_CHECK(v.is_object());
  TEST_CHECK(v["name"].get<std::string>() == "mesh");
  size_t count = 0;
  TEST_CHECK(v["count"].as_size_t(&count));
  TEST_CHECK(count == 3);
  TEST_CHECK(v["ok"].get<bool>());
  TEST_CHECK(v["items"].is_array());
  TEST_CHECK(v["items"].size() == 3);
  TEST_CHECK(v["items"][1].get<double>() == 2.5);
  TEST_CHECK(v["items"][2].is_null());
}

void minijson_unicode_escape_test(void) {
  Value v;
  Error err;
  bool ok = Parse(R"({"s":"A\u3042\ud83d\ude00"})", &v, &err);
  TEST_CHECK(ok);
  TEST_CHECK(v["s"].get<std::string>() == std::string("A") + "\xe3\x81\x82" +
                                            "\xf0\x9f\x98\x80");

  std::string out;
  ok = Serialize(v, &out, &err);
  TEST_CHECK(ok);
  TEST_CHECK(out.find("\xe3\x81\x82") != std::string::npos);
}

void minijson_reject_invalid_utf8_test(void) {
  std::string src = "{\"s\":\"";
  src.push_back(static_cast<char>(0xc0));
  src.push_back(static_cast<char>(0x80));
  src += "\"}";

  Value v;
  Error err;
  bool ok = Parse(src, &v, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.message.find("UTF-8") != std::string::npos);
}

void minijson_reject_duplicate_key_test(void) {
  Value v;
  Error err;
  bool ok = Parse(R"({"a":1,"a":2})", &v, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.message.find("duplicate") != std::string::npos);
}

void minijson_reject_invalid_number_test(void) {
  const char *bad_numbers[] = {
      R"({"n":01})",
      R"({"n":1.})",
      R"({"n":1e})",
      R"({"n":+1})",
      R"({"n":NaN})",
  };

  for (const char *src : bad_numbers) {
    Value v;
    Error err;
    bool ok = Parse(src, &v, &err);
    TEST_CHECK(!ok);
  }
}

void minijson_reject_depth_limit_test(void) {
  ParseOptions options;
  options.max_depth = 8;

  std::string src;
  for (int i = 0; i < 32; i++) src.push_back('[');
  for (int i = 0; i < 32; i++) src.push_back(']');

  Value v;
  Error err;
  bool ok = Parse(src, &v, &err, options);
  TEST_CHECK(!ok);
  TEST_CHECK(err.message.find("depth") != std::string::npos);
}

void minijson_reject_nonfinite_serialize_test(void) {
  Value v;
  v["bad"] = (std::numeric_limits<double>::infinity)();

  std::string out;
  Error err;
  bool ok = Serialize(v, &out, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.message.find("non-finite") != std::string::npos);
}

void minijson_serialize_escapes_control_chars_test(void) {
  Value v("multi\nline\tdocument");

  std::string out;
  Error err;
  bool ok = Serialize(v, &out, &err);
  TEST_CHECK(ok);
  TEST_CHECK(out == "\"multi\\nline\\tdocument\"");
}
