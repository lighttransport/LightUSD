// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-layer.hh"
#include "binding-next-scene.hh"
#include "c-stage-bridge.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/types/type-info.hh"
#include "next/stage/stage.hh"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
namespace tn = lightusd::next;

namespace lightusd {
namespace web_next {
int32_t NextLayerDocument::mhProfileJSONSize() {
  if (!stage_) {
    mh_profile_json_ = "[]";
    return 2;
  }
  const tn::Stage* stage = lightusd_internal::BorrowNativeStage(stage_);
  if (!stage) return fail_("No layer is loaded");
  constexpr size_t kMaxJSONBytes = size_t{1} << 29;
  size_t estimated_bytes = 2;
  size_t prim_count = 0;
  minijson::Value result = minijson::Value::array();
  bool exceeded = false;
  stage->Traverse([&](const tn::UsdPrim& prim) {
    if (++prim_count > 1000000) { exceeded = true; return false; }
    const std::string& type = prim.GetTypeName();
    if (type != "Skeleton" && type != "SkelAnimation" &&
        type != "Material" && type != "SkelRoot") return true;
    minijson::Value attrs = minijson::Value::object();
    minijson::Value rels = minijson::Value::object();
    std::vector<std::string> names = prim.GetPropertyNames();
    const std::vector<std::string> relationship_names = prim.GetRelationshipNames();
    names.insert(names.end(), relationship_names.begin(), relationship_names.end());
    for (const std::string& name : names) {
      if (name.rfind("mh:", 0) != 0) continue;
      if (!prim.HasAuthoredProperty(name) && !prim.GetRelationship(name)) continue;
      if (estimated_bytes > kMaxJSONBytes - std::min(name.size() * size_t{4} + 512,
                                                      kMaxJSONBytes)) {
        exceeded = true; return false;
      }
      estimated_bytes += name.size() * size_t{4} + 512;
      const auto* targets = prim.GetRelationship(name);
      if (targets) {
        minijson::Value encoded = minijson::Value::array();
        for (const tn::Path& path : *targets) {
          if (path.str().size() > (kMaxJSONBytes - estimated_bytes) / 4) {
            exceeded = true; return false;
          }
          estimated_bytes += path.str().size() * 4;
          encoded.push_back(path.str());
        }
        rels[name] = std::move(encoded);
        continue;
      }
      const tn::Value* value = prim.GetPropertyValue(name);
      const size_t elements = value ? value->array_size() : 0;
      if (value) {
        const size_t remaining = kMaxJSONBytes - estimated_bytes;
        const size_t strings = value->dynamic_string_memory_usage();
        if (strings > remaining || elements > (remaining - strings) / 256) {
          exceeded = true; return false;
        }
        estimated_bytes += strings + elements * 256;
      }
      if (prim.HasTimeSamples(name)) {
        minijson::Value samples = minijson::Value::array();
        const std::vector<double> times = prim.GetTimeSampleTimes(name);
        if (times.size() > (kMaxJSONBytes - estimated_bytes) / 512) {
          exceeded = true; return false;
        }
        estimated_bytes += times.size() * 512;
        for (double time : times) {
          const tn::Value* sample = prim.GetValueAtTime(name, time);
          if (!sample) continue;
          auto floats = sample->as_float_array();
          if (!floats) continue;
          if (floats->size() > (kMaxJSONBytes - estimated_bytes) / 32) {
            exceeded = true; return false;
          }
          estimated_bytes += floats->size() * 32;
          minijson::Value encoded = minijson::Value::object();
          encoded["t"] = time;
          encoded["v"] = NextValueJSON(*sample);
          samples.push_back(std::move(encoded));
        }
        minijson::Value sampled = minijson::Value::object();
        sampled["timeSamples"] = std::move(samples);
        attrs[name] = std::move(sampled);
      } else if (value) {
        attrs[name] = NextValueJSON(*value);
      } else {
        attrs[name] = minijson::Value();
      }
    }
    if (!attrs.empty() || !rels.empty()) {
      minijson::Value item = minijson::Value::object();
      item["path"] = prim.GetPath().str();
      item["type"] = type;
      item["attrs"] = std::move(attrs);
      if (!rels.empty()) item["rels"] = std::move(rels);
      result.push_back(std::move(item));
    }
    return true;
  });
  if (exceeded) return fail_("Metahuman profile exceeds 512 MiB output limit");
  mh_profile_json_ = result.dump();
  if (mh_profile_json_.size() > kMaxJSONBytes ||
      mh_profile_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    mh_profile_json_.clear();
    return fail_("Metahuman profile exceeds 512 MiB output limit");
  }
  error_.clear();
  return static_cast<int32_t>(mh_profile_json_.size());
}

int32_t NextLayerDocument::mhProfileJSONCopy(uint8_t* out,
                                             uint32_t cap) const {
  if (mh_profile_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) return -1;
  const int32_t required = static_cast<int32_t>(mh_profile_json_.size());
  if (!out || cap < mh_profile_json_.size()) return required;
  std::memcpy(out, mh_profile_json_.data(), mh_profile_json_.size());
  return required;
}

int32_t NextLayerDocument::shadingGraphJSONSize() {
  if (!stage_) {
    error_ = "Shading graph inspection requires a loaded Layer";
    shading_graph_json_.clear();
    return 0;
  }
  const tn::Stage* stage = lightusd_internal::BorrowNativeStage(stage_);
  if (!stage) return fail_("No layer is loaded");
  constexpr size_t kMaxJSONBytes = size_t{1} << 29;
  size_t estimated_bytes = 256;
  size_t prim_count = 0;
  bool exceeded = false;
  const auto charge = [&](size_t bytes) {
    if (bytes > kMaxJSONBytes - estimated_bytes) {
      exceeded = true;
      return false;
    }
    estimated_bytes += bytes;
    return true;
  };
  const auto charge_string = [&](size_t size, size_t overhead = 0) {
    if (size > (kMaxJSONBytes - estimated_bytes -
                std::min(overhead, kMaxJSONBytes - estimated_bytes)) / 4) {
      exceeded = true;
      return false;
    }
    return charge(size * 4 + overhead);
  };
  minijson::Value root = minijson::Value::object();
  root["version"] = 1;
  root["colorMetadataVersion"] = 1;
  root["colorSpaces"] = minijson::Value::object();
  root["assetPaths"] = minijson::Value::array();
  root["prims"] = minijson::Value::array();
  stage->Traverse([&](const tn::UsdPrim& prim) {
    if (++prim_count > 1000000 || !charge_string(prim.GetPath().str().size(), 128)) {
      exceeded = true;
      return false;
    }
    const std::string& type = prim.GetTypeName();
    const bool shading_prim = type == "Shader" || type == "Material" ||
                              type == "NodeGraph";
    minijson::Value properties = minijson::Value::object();
    const std::vector<std::string> property_names = prim.GetPropertyNames();
    for (const std::string& name : property_names) {
      if (!prim.HasAuthoredProperty(name)) continue;
      if (!charge_string(name.size(), 192)) return false;
      const tn::Value* value = prim.GetPropertyValue(name);
      const bool authored = true;
      if (value && authored) {
        const size_t remaining = kMaxJSONBytes - estimated_bytes;
        const size_t strings = value->dynamic_string_memory_usage();
        if (strings > remaining || value->array_size() > (remaining - strings) / 512) {
          exceeded = true;
          return false;
        }
        if (!charge(strings + value->array_size() * 512)) return false;
        if (auto asset = value->as_asset_path()) {
          minijson::Value record = minijson::Value::object();
          record["primPath"] = prim.GetPath().str();
          record["propertyPath"] = prim.GetPath().str() + "." + name;
          record["authored"] = *asset;
          root["assetPaths"].push_back(std::move(record));
        }
      }
      const std::vector<tn::Path>* connections =
          NextPropertyConnections(prim, name);
      if (shading_prim) {
        const tn::PrimSpec* spec = prim.GetPrimSpec();
        const std::string* type_name = spec ? spec->property_type_name(name) : nullptr;
        std::string property_type = type_name ? *type_name
            : value ? tn::GetTypeName(value->type_id()) : std::string();
        if (value && value->is_array() && property_type.find("[]") == std::string::npos)
          property_type += "[]";
        minijson::Value item = minijson::Value::object();
        item["type"] = property_type;
        item["connections"] = minijson::Value::array();
        item["timeSampled"] = prim.HasTimeSamples(name);
        if (const tn::PropMeta* meta = prim.GetPropertyMeta(name)) {
          if ((meta->authored & tn::PropMeta::kColorSpace) && !meta->colorSpace.empty())
            item["colorSpace"] = meta->colorSpace;
        }
        if (connections) {
          for (const tn::Path& connection : *connections) {
            if (!charge_string(connection.str().size(), 32)) return false;
            item["connections"].push_back(connection.str());
          }
        }
        if (value && authored) item["value"] = NextValueJSON(*value);
        properties[name] = std::move(item);
      }
    }
    if (shading_prim) {
      for (const std::string& name : prim.GetRelationshipNames()) {
        if (!charge_string(name.size(), 96)) return false;
        minijson::Value targets = minijson::Value::array();
        const std::vector<tn::Path>* values = prim.GetRelationship(name);
        if (values) {
          for (const tn::Path& target : *values) {
            if (!charge_string(target.str().size(), 16)) return false;
            targets.push_back(target.str());
          }
        }
        minijson::Value relationship = minijson::Value::object();
        relationship["targets"] = std::move(targets);
        properties[name] = std::move(relationship);
      }
      minijson::Value record = minijson::Value::object();
      record["path"] = prim.GetPath().str();
      record["type"] = type;
      record["properties"] = std::move(properties);
      root["prims"].push_back(std::move(record));
    }
    for (const std::string& schema : prim.GetMeta().apiSchemas()) {
      if (schema != "ColorSpaceAPI") continue;
      const tn::Value* value = prim.GetPropertyValue("colorSpace:name");
      if (!value || !prim.HasAuthoredProperty("colorSpace:name")) continue;
      minijson::Value color = minijson::Value::object();
      color["value"] = NextValueJSON(*value);
      color["timeSampled"] = prim.HasTimeSamples("colorSpace:name");
      root["colorSpaces"][prim.GetPath().str()] = std::move(color);
    }
    return true;
  });
  if (exceeded) return fail_("Shading graph traversal budget exceeded");
  shading_graph_json_ = root.dump();
  if (shading_graph_json_.size() > kMaxJSONBytes ||
      shading_graph_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    shading_graph_json_.clear();
    return fail_("Shading graph output exceeds 512 MiB limit");
  }
  error_.clear();
  return static_cast<int32_t>(shading_graph_json_.size());
}

int32_t NextLayerDocument::shadingGraphJSONCopy(uint8_t* out,
                                                uint32_t cap) const {
  if (shading_graph_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) return -1;
  const int32_t required = static_cast<int32_t>(shading_graph_json_.size());
  if (!out || cap < shading_graph_json_.size()) return required;
  std::memcpy(out, shading_graph_json_.data(), shading_graph_json_.size());
  return required;
}

}  // namespace web_next
}  // namespace lightusd
