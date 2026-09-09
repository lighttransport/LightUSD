#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-minijson.h"

#include "minijson.hh"
#include "tydra/value-to-json.hh"

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
  TEST_CHECK(v["name"].get_string() == "mesh");
  size_t count = 0;
  TEST_CHECK(v["count"].as_size_t(&count));
  TEST_CHECK(count == 3);
  TEST_CHECK(v["ok"].get_bool());
  TEST_CHECK(v["items"].is_array());
  TEST_CHECK(v["items"].size() == 3);
  TEST_CHECK(v["items"][1].get_double() == 2.5);
  TEST_CHECK(v["items"][2].is_null());
  TEST_CHECK(v.at("count").get<int>() == 3);
  TEST_CHECK(v.value("missing", std::string("fallback")) == "fallback");
}

void minijson_unicode_escape_test(void) {
  Value v;
  Error err;
  bool ok = Parse(R"({"s":"A\u3042\ud83d\ude00"})", &v, &err);
  TEST_CHECK(ok);
  TEST_CHECK(v["s"].get_string() == std::string("A") + "\xe3\x81\x82" +
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

void minijson_reject_input_size_limit_test(void) {
  ParseOptions options;
  options.max_input_bytes = 8;
  Value v;
  Error err;
  TEST_CHECK(!Parse("{\"value\":1}", &v, &err, options));
  TEST_CHECK(err.message.find("maximum size") != std::string::npos);
  TEST_CHECK(err.offset == 0);
}

void minijson_reject_string_size_limit_test(void) {
  ParseOptions options;
  options.max_string_bytes = 3;
  Value v;
  Error err;
  TEST_CHECK(!Parse(R"({"value":"abcd"})", &v, &err, options));
  TEST_CHECK(err.message.find("string exceeds") != std::string::npos);
}

void minijson_reject_array_size_limit_test(void) {
  ParseOptions options;
  options.max_array_elements = 2;
  Value v;
  Error err;
  TEST_CHECK(!Parse("[1,2,3]", &v, &err, options));
  TEST_CHECK(err.message.find("array exceeds") != std::string::npos);
}

void minijson_reject_object_size_limit_test(void) {
  ParseOptions options;
  options.max_object_members = 1;
  Value v;
  Error err;
  TEST_CHECK(!Parse(R"({"a":1,"b":2})", &v, &err, options));
  TEST_CHECK(err.message.find("object exceeds") != std::string::npos);
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

void minijson_reject_serialize_depth_limit_test(void) {
  Value root = Value::array();
  Value *current = &root;
  for (int i = 0; i < 32; ++i) {
    current->push_back(Value::array());
    current = &(*current)[0];
  }
  lightusd::minijson::SerializeOptions options;
  options.max_depth = 8;
  std::string out;
  Error err;
  TEST_CHECK(!Serialize(root, &out, &err, options));
  TEST_CHECK(err.message.find("serialization") != std::string::npos);
}

void minijson_serialize_escapes_control_chars_test(void) {
  Value v("multi\nline\tdocument");

  std::string out;
  Error err;
  bool ok = Serialize(v, &out, &err);
  TEST_CHECK(ok);
  TEST_CHECK(out == "\"multi\\nline\\tdocument\"");
}

void minijson_tydra_value_bridge_test(void) {
  lightusd::value::Value source(1.25f);
  lightusd::minijson::Value encoded = lightusd::tydra::ValueToMiniJSON(source);
  TEST_CHECK(encoded.is_object());
  TEST_CHECK(encoded["type"].get_string() == "float");
  std::string err;
  auto decoded = lightusd::tydra::MiniJSONToValue(encoded, &err);
  TEST_CHECK(decoded.has_value());
  if (decoded) {
    auto value = decoded->get_value<float>();
    TEST_CHECK(value.has_value());
    if (value) TEST_CHECK(std::fabs(*value - 1.25f) < 1.0e-6f);
  }

  lightusd::value::Value array_source(
      std::vector<float>{1.0f, 2.5f, 3.0f});
  lightusd::minijson::Value array_encoded =
      lightusd::tydra::ValueToMiniJSON(array_source);
  TEST_CHECK(array_encoded["type"].get_string() == "float[]");
  TEST_CHECK(array_encoded["value"].is_array());
  TEST_CHECK(array_encoded["value"].size() == 3);
  TEST_CHECK(std::fabs(array_encoded["value"][1].get_double() - 2.5) <
             1.0e-6);
  auto array_decoded = lightusd::tydra::MiniJSONToValue(array_encoded, &err);
  TEST_CHECK(array_decoded.has_value());
  if (array_decoded) {
    auto values = array_decoded->get_value<std::vector<float>>();
    TEST_CHECK(values.has_value());
    if (values) TEST_CHECK(values->size() == 3 && (*values)[1] == 2.5f);
  }

  lightusd::value::Value compound_source(lightusd::value::float3{1.0f, 2.0f,
                                                                  3.0f});
  lightusd::minijson::Value compound_encoded =
      lightusd::tydra::ValueToMiniJSON(compound_source);
  TEST_CHECK(compound_encoded["type"].get_string() == "float3");
  TEST_CHECK(compound_encoded["value"].is_array());
  TEST_CHECK(compound_encoded["value"].size() == 3);
  auto compound_decoded =
      lightusd::tydra::MiniJSONToValue(compound_encoded, &err);
  TEST_CHECK(compound_decoded.has_value());
  if (compound_decoded) {
    auto vector = compound_decoded->get_value<lightusd::value::float3>();
    TEST_CHECK(vector.has_value());
    if (vector) TEST_CHECK((*vector)[2] == 3.0f);
  }

  lightusd::value::matrix4d matrix = lightusd::value::matrix4d::identity();
  matrix.m[0][3] = 4.0;
  lightusd::value::Value matrix_source(matrix);
  lightusd::minijson::Value matrix_encoded =
      lightusd::tydra::ValueToMiniJSON(matrix_source);
  TEST_CHECK(matrix_encoded["type"].get_string() == "matrix4d");
  TEST_CHECK(matrix_encoded["value"].is_array());
  TEST_CHECK(matrix_encoded["value"].size() == 4);
  TEST_CHECK(matrix_encoded["value"][0][3].get_double() == 4.0);
  auto matrix_decoded = lightusd::tydra::MiniJSONToValue(matrix_encoded, &err);
  TEST_CHECK(matrix_decoded.has_value());
  if (matrix_decoded) {
    auto decoded_matrix =
        matrix_decoded->get_value<lightusd::value::matrix4d>();
    TEST_CHECK(decoded_matrix.has_value());
    if (decoded_matrix) TEST_CHECK(decoded_matrix->m[0][3] == 4.0);
  }

  lightusd::value::dict dictionary;
  dictionary["weight"] = 0.75f;
  lightusd::value::dict nested;
  nested["enabled"] = true;
  dictionary["nested"] = nested;
  lightusd::value::Value dictionary_source(dictionary);
  lightusd::minijson::Value dictionary_encoded =
      lightusd::tydra::ValueToMiniJSON(dictionary_source);
  TEST_CHECK(dictionary_encoded["type"].get_string() == "dictionary");
  TEST_CHECK(dictionary_encoded["value"]["weight"]["type"].get_string() ==
             "float");
  TEST_CHECK(dictionary_encoded["value"]["nested"]["value"]["enabled"]
                 ["value"]
                 .get_bool());
  auto dictionary_decoded =
      lightusd::tydra::MiniJSONToValue(dictionary_encoded, &err);
  TEST_CHECK(dictionary_decoded.has_value());
  if (dictionary_decoded) {
    auto decoded_dictionary =
        dictionary_decoded->get_value<lightusd::value::dict>();
    TEST_CHECK(decoded_dictionary.has_value());
    if (decoded_dictionary) {
      auto weight = decoded_dictionary->at("weight").cast<float>();
      TEST_CHECK(weight && *weight == 0.75f);
      auto nested_value = decoded_dictionary->at("nested").cast<lightusd::value::dict>();
      TEST_CHECK(nested_value != nullptr);
    }
  }

  lightusd::value::Value asset_source(
      lightusd::value::AssetPath("textures/albedo.png", "/tmp/albedo.png"));
  lightusd::minijson::Value asset_encoded =
      lightusd::tydra::ValueToMiniJSON(asset_source);
  auto asset_decoded = lightusd::tydra::MiniJSONToValue(asset_encoded, &err);
  TEST_CHECK(asset_decoded.has_value());
  if (asset_decoded) {
    auto asset = asset_decoded->get_value<lightusd::value::AssetPath>();
    TEST_CHECK(asset.has_value());
    if (asset) {
      TEST_CHECK(asset->GetAssetPath() == "textures/albedo.png");
      TEST_CHECK(asset->GetResolvedPath() == "/tmp/albedo.png");
    }
  }

  lightusd::value::color3f color{0.1f, 0.2f, 0.3f};
  lightusd::value::Value color_source(color);
  lightusd::minijson::Value color_encoded =
      lightusd::tydra::ValueToMiniJSON(color_source);
  TEST_CHECK(color_encoded["type"].get_string() == "color3f");
  TEST_CHECK(color_encoded["value"].size() == 3);
  auto color_decoded = lightusd::tydra::MiniJSONToValue(color_encoded, &err);
  TEST_CHECK(color_decoded.has_value());
  if (color_decoded) {
    auto decoded_color = color_decoded->get_value<lightusd::value::color3f>();
    TEST_CHECK(decoded_color.has_value());
    if (decoded_color) TEST_CHECK(std::fabs(decoded_color->b - 0.3f) < 1.0e-6f);
  }

  lightusd::value::Value point_array_source(
      std::vector<lightusd::value::float3>{{0.0f, 1.0f, 2.0f},
                                           {3.0f, 4.0f, 5.0f}});
  lightusd::minijson::Value point_array_encoded =
      lightusd::tydra::ValueToMiniJSON(point_array_source);
  TEST_CHECK(point_array_encoded["type"].get_string() == "float3[]");
  TEST_CHECK(point_array_encoded["value"].size() == 2);
  TEST_CHECK(point_array_encoded["value"][1][2].get_double() == 5.0);
  auto point_array_decoded =
      lightusd::tydra::MiniJSONToValue(point_array_encoded, &err);
  TEST_CHECK(point_array_decoded.has_value());
  if (point_array_decoded) {
    auto points = point_array_decoded->get_value<
        std::vector<lightusd::value::float3>>();
    TEST_CHECK(points.has_value());
  if (points) TEST_CHECK(points->size() == 2 && points->at(1)[2] == 5.0f);

  lightusd::value::point3f point{6.0f, 7.0f, 8.0f};
  lightusd::value::Value point_source(point);
  auto point_encoded = lightusd::tydra::ValueToMiniJSON(point_source);
  TEST_CHECK(point_encoded["type"].get_string() == "point3f");
  auto point_decoded = lightusd::tydra::MiniJSONToValue(point_encoded, &err);
  TEST_CHECK(point_decoded.has_value());
  if (point_decoded) {
    auto decoded_point = point_decoded->get_value<lightusd::value::point3f>();
    TEST_CHECK(decoded_point.has_value());
    if (decoded_point) TEST_CHECK(decoded_point->z == 8.0f);
  }

  auto plain = lightusd::tydra::ValueToPlainMiniJSON(point_source);
  TEST_CHECK(plain.is_array());
  TEST_CHECK(plain.size() == 3 && plain[2].get_double() == 8.0);

  lightusd::value::point3h half_point{
      lightusd::value::float_to_half_full(1.0f),
      lightusd::value::float_to_half_full(2.0f),
      lightusd::value::float_to_half_full(3.0f)};
  auto half_encoded = lightusd::tydra::ValueToMiniJSON(
      lightusd::value::Value(half_point));
  TEST_CHECK(half_encoded["type"].get_string() == "point3h");
  TEST_CHECK(std::fabs(half_encoded["value"][1].get_double() - 2.0) <
             1.0e-3);

  auto schema = lightusd::tydra::ValueTypeToMiniJSONSchema("float3[]");
  TEST_CHECK(schema["type"].get_string() == "object");
  TEST_CHECK(schema["properties"]["value"]["type"].get_string() ==
             "array");
  TEST_CHECK(schema["properties"]["value"]["items"]["type"].get_string() ==
             "array");

  lightusd::value::Value token_array_source(std::vector<lightusd::value::token>{
      lightusd::value::token("st"), lightusd::value::token("st1")});
  auto token_array = lightusd::tydra::ValueToMiniJSON(token_array_source);
  TEST_CHECK(token_array["type"].get_string() == "token[]");
  TEST_CHECK(token_array["value"][0].get_string() == "st");
  auto decoded_tokens = lightusd::tydra::MiniJSONToValue(token_array, &err);
  TEST_CHECK(decoded_tokens.has_value());
  if (decoded_tokens) {
    auto tokens = decoded_tokens->get_value<std::vector<lightusd::value::token>>();
    TEST_CHECK(tokens.has_value() && tokens->size() == 2);
    if (tokens) TEST_CHECK(tokens->at(1).str() == "st1");
  }
}
}
