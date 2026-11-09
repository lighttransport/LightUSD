// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-layer.hh"
#include "binding-next-layer-json-internal.hh"
#include "binding-next-scene.hh"
#include "next/types/type-info.hh"
#include "next/types/value.hh"
#include <cmath>
#include <utility>
namespace tn = lightusd::next;
#include "next/parser/lexer.hh"
#include "next/parser/value-parser.hh"
#include "next/writer/value-printer.hh"

namespace lightusd {
namespace web_next {
namespace layer_json_detail {
bool ParseLayerJSONValue(const minijson::Value& encoded,
                         const std::string& type_name, tn::Value* out,
                         bool* is_array, std::string* error) {
  if (!encoded.is_string() || !out || !is_array) {
    if (error) *error = "Layer JSON attribute value must be canonical USD text";
    return false;
  }
  bool array = false;
  const tn::TypeId type = tn::ParseTypeName(type_name, array);
  if (type == tn::TypeId::Invalid) {
    if (error) *error = "Unsupported Layer JSON attribute type: " + type_name;
    return false;
  }
  const std::string text = encoded.get_string();
  tn::Lexer lexer(text.data(), text.size());
  tn::ParseResult parsed = array ? tn::ParseArrayValue(lexer, type)
                                 : tn::ParseValue(lexer, type);
  if (!parsed.success || lexer.peek().type != tn::TokenType::Eof) {
    if (error) *error = parsed.error.empty()
        ? "Trailing tokens in Layer JSON attribute value" : parsed.error;
    return false;
  }
  *out = std::move(parsed.value);
  *is_array = array;
  return true;
}

bool FormatJSONMetadataUSDValue(const minijson::Value& encoded, bool tuple,
                                std::string* out) {
  if (!out) return false;
  if (encoded.is_array()) {
    out->push_back(tuple ? '(' : '[');
    bool first = true;
    for (const minijson::Value& item : encoded) {
      if (!first) out->append(", ");
      first = false;
      if (!FormatJSONMetadataUSDValue(item, tuple, out)) return false;
    }
    out->push_back(tuple ? ')' : ']');
    return true;
  }
  if (encoded.is_object()) return false;
  out->append(encoded.dump());
  return true;
}

bool ParseLayerJSONMetadataValue(const minijson::Value& encoded,
                                tn::Value* out, size_t depth,
                                size_t* value_count) {
  if (!out || !value_count || depth > 64 || ++*value_count > 1000000)
    return false;
  if (encoded.is_boolean()) {
    *out = tn::Value(encoded.get_bool());
    return true;
  }
  if (encoded.is_number_integer()) {
    if (encoded.is_number_unsigned())
      *out = tn::Value(encoded.get_uint64());
    else
      *out = tn::Value(encoded.get_int64());
    return true;
  }
  if (encoded.is_number()) {
    *out = tn::Value(encoded.get_double());
    return true;
  }
  if (encoded.is_string()) {
    *out = tn::Value(encoded.get_string());
    return true;
  }
  if (encoded.is_object() && encoded["type"].is_string() &&
      encoded.contains("value")) {
    const std::string type_name = encoded["type"].get_string();
    bool is_array = false;
    const tn::TypeId type = tn::ParseTypeName(type_name, is_array);
    if (type == tn::TypeId::Invalid) return false;
    // Legacy typed asset metadata uses an object payload. The next Value
    // stores the authored path; resolvedPath is a transient resolver hint.
    if (type == tn::TypeId::AssetPath && !is_array) {
      const minijson::Value& asset = encoded["value"];
      if (!asset.is_object() ||
          (asset.contains("assetPath") && !asset["assetPath"].is_string()) ||
          (asset.contains("resolvedPath") && !asset["resolvedPath"].is_string()))
        return false;
      *out = tn::Value::MakeAssetPath(asset["assetPath"].is_string()
          ? asset["assetPath"].get_string() : std::string());
      return true;
    }
    if (type == tn::TypeId::Dictionary && !is_array) {
      const minijson::Value& members = encoded["value"];
      if (!members.is_object()) return false;
      tn::Dict dict;
      const auto* items = members.object_items();
      if (!items) return false;
      for (const auto& member : *items) {
        if (!member.value().is_object() ||
            !member.value()["type"].is_string() ||
            !member.value().contains("value")) return false;
        tn::Value child;
        if (!ParseLayerJSONMetadataValue(member.value(), &child, depth + 1,
                                         value_count)) return false;
        dict.set(member.key, std::move(child));
      }
      dict.set_typed_json_wrapper(true);
      *out = tn::Value::MakeDictionary(std::move(dict));
      return true;
    }
    std::string text;
    const bool compound = tn::GetComponentCount(type) > 1;
    if (is_array) {
      const minijson::Value& items = encoded["value"];
      if (!items.is_array()) return false;
      text.push_back('[');
      bool first = true;
      for (const minijson::Value& item : items) {
        if (!first) text.append(", ");
        first = false;
        if (!FormatJSONMetadataUSDValue(item, compound, &text)) return false;
      }
      text.push_back(']');
    } else if (!FormatJSONMetadataUSDValue(encoded["value"], compound, &text)) {
      return false;
    }
    tn::Lexer lexer(text.data(), text.size());
    tn::ParseResult parsed = is_array ? tn::ParseArrayValue(lexer, type)
                                      : tn::ParseValue(lexer, type);
    if (!parsed.success || lexer.peek().type != tn::TokenType::Eof) return false;
    *out = std::move(parsed.value);
    return true;
  }
  if (!encoded.is_object()) return false;
  tn::Dict dict;
  const auto* items = encoded.object_items();
  if (!items) return false;
  for (const auto& item : *items) {
    tn::Value value;
    if (!ParseLayerJSONMetadataValue(item.value(), &value, depth + 1,
                                     value_count)) return false;
    dict.set(item.key, std::move(value));
  }
  *out = tn::Value::MakeDictionary(std::move(dict));
  return true;
}

minijson::Value LayerMetadataPayloadJSON(tn::ValueView value) {
  minijson::Value flat = NextValueJSON(value);
  if (flat.is_null() && !value.is_array()) {
    if (value.type_id() == tn::TypeId::Half) {
      float number = 0.0f;
      if (value.to_float(&number))
        return std::isfinite(number) ? minijson::Value(number)
                                     : minijson::Value();
    }
    const float* floats = nullptr;
    const double* doubles = nullptr;
    const int32_t* ints = nullptr;
    float half_lanes[4] = {};
    switch (value.type_id()) {
      case tn::TypeId::Int2: ints = value.as_int2(); break;
      case tn::TypeId::Int3: ints = value.as_int3(); break;
      case tn::TypeId::Int4: ints = value.as_int4(); break;
      case tn::TypeId::Half2:
      case tn::TypeId::Texcoord2h:
        if (value.to_float2(half_lanes)) floats = half_lanes;
        break;
      case tn::TypeId::Half3:
      case tn::TypeId::Point3h:
      case tn::TypeId::Vector3h:
      case tn::TypeId::Normal3h:
      case tn::TypeId::Color3h:
      case tn::TypeId::Texcoord3h:
        if (value.to_float3(half_lanes)) floats = half_lanes;
        break;
      case tn::TypeId::Half4:
      case tn::TypeId::Color4h:
      case tn::TypeId::Quath:
        if (value.to_float4(half_lanes)) floats = half_lanes;
        break;
      case tn::TypeId::Matrix2f: floats = value.as_matrix2f(); break;
      case tn::TypeId::Matrix3f: floats = value.as_matrix3f(); break;
      case tn::TypeId::Matrix2d: doubles = value.as_matrix2d(); break;
      case tn::TypeId::Matrix3d: doubles = value.as_matrix3d(); break;
      default: break;
    }
    if (floats || doubles || ints) {
      flat = minijson::Value::array();
      for (size_t i = 0; i < tn::GetComponentCount(value.type_id()); ++i) {
        if (ints) {
          flat.push_back(ints[i]);
        } else {
          const double number = floats ? floats[i] : doubles[i];
          flat.push_back(std::isfinite(number)
              ? minijson::Value(number) : minijson::Value());
        }
      }
    }
  }
  if (!flat.is_array()) return flat;
  const size_t components = tn::GetComponentCount(value.type_id());
  if (components <= 1) return flat;
  const size_t count = value.is_array() ? value.array_size() : 1;
  if (count > flat.size() / components ||
      count * components != flat.size()) return flat;
  size_t matrix_width = 0;
  switch (value.type_id()) {
    case tn::TypeId::Matrix2f: case tn::TypeId::Matrix2d:
      matrix_width = 2; break;
    case tn::TypeId::Matrix3f: case tn::TypeId::Matrix3d:
      matrix_width = 3; break;
    case tn::TypeId::Matrix4f: case tn::TypeId::Matrix4d:
    case tn::TypeId::Frame4d:
      matrix_width = 4; break;
    default: break;
  }
  if (!value.is_array() && !matrix_width) return flat;
  minijson::Value grouped = minijson::Value::array();
  for (size_t item = 0; item < count; ++item) {
    minijson::Value element = minijson::Value::array();
    if (matrix_width) {
      for (size_t row = 0; row < matrix_width; ++row) {
        minijson::Value cells = minijson::Value::array();
        for (size_t column = 0; column < matrix_width; ++column)
          cells.push_back(flat[item * components + row * matrix_width + column]);
        element.push_back(std::move(cells));
      }
    } else {
      for (size_t component = 0; component < components; ++component)
        element.push_back(flat[item * components + component]);
    }
    grouped.push_back(std::move(element));
  }
  return value.is_array() ? grouped : grouped[0];
}

minijson::Value LayerTypedMetadataValueJSON(tn::ValueView value) {
  minijson::Value out = minijson::Value::object();
  out["type"] = tn::PrintTypeName(value.type_id(), value.is_array());
  if (auto dict = value.as_dictionary()) {
    minijson::Value members = minijson::Value::object();
    for (const auto& entry : dict->entries())
      members[entry.first] = LayerTypedMetadataValueJSON(entry.second);
    out["value"] = std::move(members);
  } else if (auto asset = value.as_asset_path()) {
    minijson::Value path = minijson::Value::object();
    path["assetPath"] = *asset;
    path["resolvedPath"] = "";
    out["value"] = std::move(path);
  } else {
    out["value"] = LayerMetadataPayloadJSON(value);
  }
  return out;
}

minijson::Value LayerMetadataValueJSON(tn::ValueView value) {
  if (auto dictionary = value.as_dictionary()) {
    if (dictionary->typed_json_wrapper())
      return LayerTypedMetadataValueJSON(value);
    minijson::Value out = minijson::Value::object();
    for (const auto& entry : dictionary->entries())
      out[entry.first] = LayerMetadataValueJSON(entry.second);
    return out;
  }
  if (value.is_array() || tn::GetComponentCount(value.type_id()) > 1 ||
      value.type_id() == tn::TypeId::TimeCode ||
      value.type_id() == tn::TypeId::Half) {
    minijson::Value out = minijson::Value::object();
    out["type"] = tn::PrintTypeName(value.type_id(), value.is_array());
    out["value"] = LayerMetadataPayloadJSON(value);
    return out;
  }
  return NextValueJSON(value);
}

size_t MetadataValueEntryCount(tn::ValueView value) {
  size_t count = 1;
  std::vector<tn::ValueView> pending{value};
  while (!pending.empty()) {
    const tn::ValueView current = pending.back();
    pending.pop_back();
    if (auto dict = current->as_dictionary()) {
      if (dict->size() > 1000000 - count) return SIZE_MAX;
      count += dict->size();
      for (const auto& item : dict->entries()) pending.push_back(item.second);
    } else if (current->is_array()) {
      if (current->array_size() > 1000000 - count) return SIZE_MAX;
      count += current->array_size();
    }
  }
  return count;
}

}  // namespace layer_json_detail

}  // namespace web_next
}  // namespace lightusd
