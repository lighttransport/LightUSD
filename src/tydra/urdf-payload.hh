// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.

#pragma once

#include <string>

#include "core/minijson.hh"

namespace lightusd {
namespace tydra {
namespace detail {

using JSONValue = ::lightusd::minijson::Value;

// Schema-neutral, validated view of the JSON exchanged by the browser URDF
// and MJCF frontends. Both legacy and next-core authors consume this object so
// section defaults and source-format detection cannot drift.
struct URDFPayload {
  JSONValue root;
  const JSONValue* borrowed = nullptr;
  const JSONValue& Root() const { return borrowed ? *borrowed : root; }
  JSONValue empty_array = JSONValue::array();
  bool mjcf_source{false};

  const JSONValue& Array(const char* name) const {
    if (Root().contains(name) && Root().at(name).is_array()) {
      return Root().at(name);
    }
    return empty_array;
  }

  static bool Parse(const std::string& text, URDFPayload* out,
                    std::string* err) {
    if (!out) {
      if (err) *err = "URDF payload output is null";
      return false;
    }
    minijson::Error parse_error;
    if (!minijson::Parse(text, &out->root, &parse_error) ||
        !out->root.is_object()) {
      if (err) *err = "URDF export JSON parse failed: " + parse_error.message;
      return false;
    }
    const bool valid = Borrow(out->root, out, err);
    out->borrowed = nullptr;
    return valid;
  }

  static bool Borrow(const JSONValue& value, URDFPayload* out,
                     std::string* err) {
    if (!out || !value.is_object()) {
      if (err) *err = "Invalid URDF payload";
      return false;
    }
    out->borrowed = &value;
    auto string_value = [&](const char* key) -> std::string {
      if (!out->Root().contains(key) || !out->Root().at(key).is_string()) {
        return std::string();
      }
      return out->Root().at(key).get<std::string>();
    };
    const std::string source_format = string_value("sourceFormat");
    const std::string input_format = string_value("inputFormat");
    out->mjcf_source = source_format == "mjcf" || source_format == "MJCF" ||
                       input_format == "mjcf" || input_format == "MJCF";
    if (out->Array("links").empty()) {
      if (err) *err = "URDF export JSON has no links";
      return false;
    }
    for (const char* key : {"links", "joints", "materials", "sites", "actuators",
                             "mjcActuators", "tendons", "equalities", "keyframes",
                             "lights", "cameras", "sensors", "contactPairs", "plugins"}) {
      const auto& items = value[key];
      if (items.is_null()) continue;
      bool valid = items.is_array();
      if (valid)
        for (const auto& item : items)
          if (!item.is_object()) {
            valid = false;
            break;
          }
      if (!valid) {
        if (err) *err = std::string("Robot payload field '") + key +
                        "' must be an array of objects";
        return false;
      }
    }
    return true;
  }
};

}  // namespace detail
}  // namespace tydra
}  // namespace lightusd
