// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <sstream>
#include <cmath>
#include <string>
#include <utility>
#include "json-assets.hh"
#include "core/minijson.hh"
#include "layer/layer.hh"
#include "types/type-info.hh"
#include "writer/layer-json.hh"

namespace lightusd_examples {
using JSON = lightusd::minijson::Value;
inline JSON RuntimeArray(const lightusd::PrimSpec& prim, const char* name,
                         const char* type, const JSON& encoded) {
  JSON out = JSON::object();
  const auto* value = prim.property_value(name);
  if (!value || !value->is_array() || value->array_size() == 0) return out;
  size_t size = 0;
  const uint8_t* data = value->raw_bytes(&size);
  if (!data || !size) return out;
  std::ostringstream bytes;
  Base64(bytes, data, size);
  out["data"] = bytes.str();
  out["count"] = static_cast<uint64_t>(value->array_size());
  out["type"] = type;
  const JSON& metadata = encoded["properties"][name]["attribute"]["metadata"];
  if (metadata.is_object() && !metadata.empty()) out["metadata"] = metadata;
  return out;
}
inline JSON RuntimePrim(const lightusd::Layer& layer, const std::string& path,
                        const JSON& encoded) {
  JSON out = JSON::object();
  for (const auto& member : *encoded.object_items())
    if (member.key != "children") out[member.key] = member.value();
  const auto* prim = layer.prim_at_path(path);
  if (!prim) return out;
  const std::string type = prim->type_name();
  // Keep the runtime schema aliases and geometry fields consumed by existing
  // scene JSON clients, alongside the complete authored property records.
  if (type == "Mesh" || type == "BasisCurves" || type == "Sphere" ||
      type == "Cube" || type == "Cone" || type == "Cylinder" ||
      type == "Capsule" || type == "Plane") {
    out["usdTypeName"] = type;
    out["typeName"] = "Geom" + type;
  }
  const auto array = [&](const char* name, const char* array_type) {
    if (prim->property_value(name)) out[name] = RuntimeArray(*prim, name, array_type, encoded);
  };
  if (type == "Mesh" || type == "BasisCurves") {
    array("points", "point3f[]");
    array("normals", "normal3f[]");
    if (type == "Mesh") {
      array("faceVertexCounts", "SCALAR[]");
      array("faceVertexIndices", "SCALAR[]");
    } else {
      array("curveVertexCounts", "SCALAR[]");
      array("widths", "SCALAR[]");
    }
  }
  const auto scalar = [&](const char* name, double fallback) {
    const auto* value = prim->property_value(name);
    const auto* slot = prim->property(name);
    if (!value && slot && slot->is_time_sampled()) return;
    if (value && value->is_block()) return;
    double number = fallback;
    if (value) {
      if (const auto* d = value->as_double()) number = *d;
      else if (const auto* f = value->as_float()) number = *f;
    }
    out[name] = std::isfinite(number) ? JSON(number) : JSON();
  };
  if (type == "Sphere") scalar("radius", 1);
  if (type == "Cube") scalar("size", 2);
  if (type == "Cone" || type == "Cylinder" || type == "Capsule") {
    scalar("height", 2);
    scalar("radius", type == "Capsule" ? 0.5 : 1);
  }
  if (type == "Plane") { scalar("width", 2); scalar("length", 2); }
  if (type == "Cone" || type == "Cylinder" || type == "Capsule" || type == "Plane") {
    const auto* axis = prim->property_value("axis");
    const auto token = axis ? axis->as_token() : lightusd::StringView{};
    out["axis"] = token ? std::string(*token) : "Z";
  }
  if (type == "Xform") {
    const auto* order = prim->property_value("xformOpOrder");
    const auto tokens = order ? order->as_token_array() : lightusd::StringArrayView{};
    if (tokens && !tokens->empty()) {
      JSON suffixes = JSON::array();
      for (const auto& token : *tokens) {
        const std::string name(token);
        if (name == "!resetXformStack!") continue;
        const size_t start = name.find("xformOp:");
        const size_t colon = start == std::string::npos ? start : name.find(':', start + 8);
        suffixes.push_back(colon == std::string::npos ? "" : name.substr(colon + 1));
      }
      if (!suffixes.empty()) out["xformOpOrder"] = std::move(suffixes);
    }
  }
  if (encoded["children"].is_object()) {
    JSON children = JSON::object();
    for (const auto& child : *encoded["children"].object_items())
      children[child.key] = RuntimePrim(layer, path + "/" + child.key, child.value());
    out["primChildren"] = std::move(children);
  }
  return out;
}
inline nonstd::expected<std::string, std::string> RuntimeSceneJSON(
    const lightusd::Layer& layer, const std::string& authored) {
  JSON encoded;
  lightusd::minijson::Error error;
  lightusd::minijson::ParseOptions options;
  options.max_input_bytes = size_t{1} << 29;
  options.max_string_bytes = options.max_input_bytes;
  if (!lightusd::minijson::Parse(authored, &encoded, &error, options))
    return nonstd::make_unexpected(error.message);
  JSON root = JSON::object();
  root["version"] = 1.0;
  if (encoded["metas"].is_object()) root["properties"] = encoded["metas"];
  JSON children = JSON::object();
  if (encoded["primSpecs"].is_object())
    for (const auto& child : *encoded["primSpecs"].object_items())
      children[child.key] = RuntimePrim(layer, "/" + child.key, child.value());
  root["primChildren"] = std::move(children);
  std::string output = root.dump(2);
  if (output.empty() || output.size() > (size_t{1} << 29))
    return nonstd::make_unexpected<std::string>("Runtime JSON output exceeds serialization limits");
  return output;
}
}  // namespace lightusd_examples
