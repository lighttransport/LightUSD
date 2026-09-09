// SPDX-License-Identifier: Apache-2.0
// libFuzzer harness for the bounded native JSON parser/serializer.

#include <cstddef>
#include <cstdint>
#include <string>

#include "minijson.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  lightusd::minijson::ParseOptions parse_options;
  parse_options.max_input_bytes = 16u << 20;
  parse_options.max_depth = 128;
  parse_options.max_string_bytes = 4u << 20;
  parse_options.max_array_elements = 1u << 20;
  parse_options.max_object_members = 1u << 20;

  lightusd::minijson::Value value;
  lightusd::minijson::Error error;
  if (!lightusd::minijson::Parse(
          reinterpret_cast<const char *>(data), size, &value, &error,
          parse_options)) {
    return 0;
  }

  // Exercise the serializer on every successfully parsed value. This also
  // covers the parser/serializer boundary for deeply nested and large data.
  std::string encoded;
  lightusd::minijson::SerializeOptions serialize_options;
  serialize_options.max_depth = 128;
  (void)lightusd::minijson::Serialize(value, &encoded, &error,
                                      serialize_options);
  return 0;
}
