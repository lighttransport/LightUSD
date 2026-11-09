#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lightusd {

class Layer;
class Stage;

namespace json {

class JsonContext {
};

class JsonWriter {

 public:


  JsonWriter() = default;
  ~JsonWriter() = default;
  JsonWriter(const JsonWriter &) = delete;
  JsonWriter &operator=(const JsonWriter &) = delete;
  JsonWriter(JsonWriter &&) = default;
  JsonWriter &operator=(JsonWriter &&) = default;

  void set_indent(uint32_t indent) { indent_ = indent; }

  // Serialize through the canonical minijson USD representation. Returns
  // false for a null output pointer or a serialization failure.
  bool to_json(const lightusd::Layer &layer, std::string *out_json);
  bool to_json(const lightusd::Stage &stage, std::string *out_json);

 private:
  uint32_t indent_ = 2;


};

} // namespace json
} // namespace lightusd
