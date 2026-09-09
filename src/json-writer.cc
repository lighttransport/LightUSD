#include "json-writer.hh"
#include "layer.hh"
#include "minijson.hh"
#include "usd-to-json.hh"
#include "str-util.hh"
#include <string>

// NOTE: dtos() from str-util.hh uses dragonbox algorithm for
// shortest float-to-string conversion

#include "common-macros.inc"

namespace lightusd {
namespace json {


namespace detail {

// NOTE: Use lightusd::dtos() from str-util.hh instead



} // namespace detal

bool JsonWriter::to_json(const lightusd::Layer &layer, std::string *out_json) {
  if (!out_json) {
    return false;
  }

  // Keep this facade on the minijson path.  Besides avoiding a second USD
  // serializer, this preserves the canonical key ordering and bounded value
  // handling implemented by usd-to-json.cc.
  minijson::Value value = lightusd::ToJSONValue(layer);
  minijson::SerializeOptions options;
  options.indent = static_cast<int>(indent_);
  options.sort_keys = true;
  minijson::Error error;
  return minijson::Serialize(value, out_json, &error, options);
}

bool JsonWriter::to_json(const lightusd::Stage &stage, std::string *out_json) {
  if (!out_json) {
    return false;
  }

  USDToJSONContext context;
  minijson::Value value = lightusd::ToJSONValue(stage, &context);
  minijson::SerializeOptions options;
  options.indent = static_cast<int>(indent_);
  options.sort_keys = true;
  minijson::Error error;
  return minijson::Serialize(value, out_json, &error, options);
}

} // namespace json
}  // namespace lightusd
