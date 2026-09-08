// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-scene.hh"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include "next/stage/stage.hh"
#include "next/schema/usd-shade.hh"
#include "next/schema/color-space.hh"
#include "next/writer/value-printer.hh"

namespace tn = lightusd::next;
namespace lightusd {
namespace web_next {
namespace {
minijson::Value JSONNumber(double value) {
  // nlohmann encoded non-finite USD numeric values as JSON null. Keep that
  // web contract while the shared MiniJSON serializer remains strict.
  return std::isfinite(value) ? minijson::Value(value) : minijson::Value();
}
}
lightusd::minijson::Value NextValueJSON(const tn::Value& value) {
  if (const bool* v = value.as_bool()) return *v;
  if (const int32_t* v = value.as_int()) return *v;
  if (const uint32_t* v = value.as_uint()) return *v;
  if (const int64_t* v = value.as_int64()) return *v;
  if (const uint64_t* v = value.as_uint64()) return *v;
  if (const uint8_t* v = value.as_uchar()) return *v;
  if (const float* v = value.as_float()) return JSONNumber(*v);
  if (const double* v = value.as_double()) return JSONNumber(*v);
  if (const std::string* v = value.as_string()) return *v;
  if (const std::string* v = value.as_token()) return *v;
  if (const std::string* v = value.as_asset_path()) return *v;
  if (const std::vector<float>* v = value.as_float_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(JSONNumber(element));
    return out;
  }
  if (const std::vector<double>* v = value.as_double_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(JSONNumber(element));
    return out;
  }
  if (const std::vector<int32_t>* v = value.as_int_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }
  if (const std::vector<int64_t>* v = value.as_int64_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }
  if (const std::vector<uint32_t>* v = value.as_uint_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }
  if (const std::vector<uint64_t>* v = value.as_uint64_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }
  if (const std::vector<std::string>* v = value.as_token_array()) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    out.reserve(v->size());
    for (const auto& element : *v) out.push_back(element);
    return out;
  }

  auto float_array = [](const float* ptr, size_t count) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    for (size_t i = 0; i < count; ++i) out.push_back(JSONNumber(ptr[i]));
    return out;
  };
  auto double_array = [](const double* ptr, size_t count) {
    lightusd::minijson::Value out = lightusd::minijson::Value::array();
    for (size_t i = 0; i < count; ++i) out.push_back(JSONNumber(ptr[i]));
    return out;
  };
  switch (value.type_id()) {
    case tn::TypeId::Float2:
    case tn::TypeId::Texcoord2f:
      return float_array(static_cast<const float*>(value.raw_data()), 2);
    case tn::TypeId::Float3:
    case tn::TypeId::Point3f:
    case tn::TypeId::Vector3f:
    case tn::TypeId::Normal3f:
    case tn::TypeId::Color3f:
      return float_array(static_cast<const float*>(value.raw_data()), 3);
    case tn::TypeId::Float4:
    case tn::TypeId::Color4f:
    case tn::TypeId::Quatf:
      return float_array(static_cast<const float*>(value.raw_data()), 4);
    case tn::TypeId::Double2:
    case tn::TypeId::Texcoord2d:
      return double_array(static_cast<const double*>(value.raw_data()), 2);
    case tn::TypeId::Double3:
    case tn::TypeId::Point3d:
    case tn::TypeId::Vector3d:
    case tn::TypeId::Normal3d:
    case tn::TypeId::Color3d:
      return double_array(static_cast<const double*>(value.raw_data()), 3);
    case tn::TypeId::Double4:
    case tn::TypeId::Color4d:
    case tn::TypeId::Quatd:
      return double_array(static_cast<const double*>(value.raw_data()), 4);
    case tn::TypeId::Matrix4f:
      return float_array(value.as_matrix4f(), 16);
    case tn::TypeId::Matrix4d:
    case tn::TypeId::Frame4d:  // matrix4d role
      return double_array(value.as_matrix4d(), 16);
    default:
      break;
  }
  return nullptr;
}

const std::vector<tn::Path>* NextPropertyConnections(
    const tn::UsdPrim& prim, const std::string& property_name) {
  const tn::PrimSpec* spec = prim.GetPrimSpec();
  return spec ? spec->connection(property_name) : nullptr;
}

std::string NextConnectionPrimPath(const std::string& connection) {
  size_t dot = connection.find(".outputs:");
  if (dot == std::string::npos) dot = connection.find(".inputs:");
  if (dot == std::string::npos) dot = connection.rfind('.');
  return dot == std::string::npos ? connection : connection.substr(0, dot);
}

static std::string NextConnectionOutputName(const std::string& connection) {
  const size_t marker = connection.find(".outputs:");
  if (marker != std::string::npos) return connection.substr(marker + 9);
  const size_t dot = connection.rfind('.');
  return dot == std::string::npos ? std::string() : connection.substr(dot + 1);
}

static std::string NextConnectionNodeName(const std::string& connection) {
  const std::string path = NextConnectionPrimPath(connection);
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string NextMtlxCategory(const std::string& node_id) {
  if (node_id.rfind("ND_", 0) != 0) return node_id;
  const std::string body = node_id.substr(3);
  const size_t suffix = body.rfind('_');
  return suffix == std::string::npos ? body : body.substr(0, suffix);
}

static void CollectNextNodeGraphs(const tn::UsdPrim& prim,
                           std::vector<tn::UsdPrim>* graphs) {
  if (!graphs) return;
  for (const tn::UsdPrim& child : prim.GetChildren()) {
    if (child.GetTypeName() == "NodeGraph") graphs->push_back(child);
    CollectNextNodeGraphs(child, graphs);
  }
}

// Reconstruct the compact MaterialX graph payload expected by the web graph
// editor. The next render converter deliberately stores GPU-facing shader
// parameters; the authored node network remains in the Stage and must be
// serialized before RenderStream releases it.
std::string BuildNextNodeGraphJson(const tn::UsdPrim& material,
                                   const tn::UsdPrim& shader,
                                   const std::string& version) {
  if (!material.IsValid() || !shader.IsValid()) return {};
  std::vector<tn::UsdPrim> graphs;
  CollectNextNodeGraphs(material, &graphs);
  if (graphs.empty()) return {};

  tn::UsdPrim graph = graphs.front();
  for (const std::string& property : shader.GetPropertyNames()) {
    if (property.rfind("inputs:", 0) != 0) continue;
    const std::vector<tn::Path>* connections =
        NextPropertyConnections(shader, property);
    if (!connections || connections->empty()) continue;
    const std::string source = NextConnectionPrimPath((*connections)[0].str());
    for (const tn::UsdPrim& candidate : graphs) {
      const std::string candidate_path = candidate.GetPath().str();
      if (source == candidate_path ||
          source.rfind(candidate_path + "/", 0) == 0) {
        graph = candidate;
        break;
      }
    }
  }

  lightusd::minijson::Value root;
  root["version"] = version.empty() ? "1.39" : version;
  lightusd::minijson::Value graph_json;
  graph_json["name"] = graph.GetName();
  graph_json["nodes"] = lightusd::minijson::Value::array();
  graph_json["outputs"] = lightusd::minijson::Value::array();

  for (const tn::UsdPrim& node : graph.GetChildren()) {
    if (!tn::IsShader(node)) continue;
    std::string node_id;
    if (const tn::Value* value = node.GetPropertyValue("info:id")) {
      if (const std::string* token = value->as_token()) node_id = *token;
      else if (const std::string* str = value->as_string()) node_id = *str;
    }
    lightusd::minijson::Value node_json;
    node_json["name"] = node.GetName();
    node_json["category"] = NextMtlxCategory(node_id);
    node_json["type"] = node_id;
    node_json["inputs"] = lightusd::minijson::Value::array();
    for (const std::string& property : node.GetPropertyNames()) {
      if (property.rfind("inputs:", 0) != 0) continue;
      lightusd::minijson::Value input;
      input["name"] = property.substr(7);
      const std::vector<tn::Path>* connections =
          NextPropertyConnections(node, property);
      if (connections && !connections->empty()) {
        const std::string source = (*connections)[0].str();
        input["nodename"] = NextConnectionNodeName(source);
        input["output"] = NextConnectionOutputName(source);
      } else if (const tn::Value* value = node.GetPropertyValue(property)) {
        lightusd::minijson::Value encoded = NextValueJSON(*value);
        if (!encoded.is_null()) input["value"] = std::move(encoded);
      }
      std::string color_space;
      bool color_space_authored = false;
      if (tn::color_management::ComputeColorSpaceName(
              node, property, &color_space, &color_space_authored) &&
          color_space_authored) {
        input["colorspace"] = color_space;
      }
      node_json["inputs"].push_back(std::move(input));
    }
    graph_json["nodes"].push_back(std::move(node_json));
  }

  for (const std::string& property : graph.GetPropertyNames()) {
    if (property.rfind("outputs:", 0) != 0) continue;
    lightusd::minijson::Value output;
    output["name"] = property.substr(8);
    const std::vector<tn::Path>* connections =
        NextPropertyConnections(graph, property);
    if (connections && !connections->empty()) {
      const std::string source = (*connections)[0].str();
      output["nodename"] = NextConnectionNodeName(source);
      output["output"] = NextConnectionOutputName(source);
    }
    graph_json["outputs"].push_back(std::move(output));
  }

  root["nodegraph"] = std::move(graph_json);
  root["connections"] = lightusd::minijson::Value::array();
  const std::string graph_path = graph.GetPath().str();
  for (const std::string& property : shader.GetPropertyNames()) {
    if (property.rfind("inputs:", 0) != 0) continue;
    const std::vector<tn::Path>* connections =
        NextPropertyConnections(shader, property);
    if (!connections || connections->empty()) continue;
    const std::string source = (*connections)[0].str();
    const std::string source_prim = NextConnectionPrimPath(source);
    if (source_prim != graph_path &&
        source_prim.rfind(graph_path + "/", 0) != 0) continue;
    lightusd::minijson::Value connection;
    connection["input"] = property.substr(7);
    connection["nodegraph"] = graph.GetName();
    connection["output"] = NextConnectionOutputName(source);
    root["connections"].push_back(std::move(connection));
  }
  return root.dump();
}

std::string CanonicalMaterialGraph(const tn::Stage& stage,
                                   const tn::UsdPrim& material) {
  if (!material.IsValid()) return {};
  const std::string root_path = material.GetPath().str();
  bool valid = true;
  std::set<std::string> visiting;
  auto append = [](std::string* out, const std::string& value) {
    const uint64_t size = static_cast<uint64_t>(value.size());
    out->append(reinterpret_cast<const char*>(&size), sizeof(size));
    out->append(value);
  };
  auto append_value = [&](std::string* out, const tn::Value& value) {
    const uint16_t type = static_cast<uint16_t>(value.type_id());
    out->append(reinterpret_cast<const char*>(&type), sizeof(type));
    append(out, tn::PrintValue(value));
  };
  std::function<void(const std::string&, std::string*)> encode_connection;
  encode_connection = [&](const std::string& connection, std::string* out) {
    std::string prim_path = NextConnectionPrimPath(connection);
    std::string output = NextConnectionOutputName(connection);
    if (prim_path.empty()) prim_path = connection;
    const std::string visit_key = prim_path + "." + output;
    if (!visiting.insert(visit_key).second) {
      valid = false;
      return;
    }
    struct Guard {
      std::set<std::string>* visiting;
      std::string key;
      ~Guard() { visiting->erase(key); }
    } guard{&visiting, visit_key};

    // External graphs are not safe to alias by local structure alone. Keep
    // their exact target so only the same composed source can hit the cache.
    if (prim_path != root_path && prim_path.rfind(root_path + "/", 0) != 0) {
      append(out, "external");
      append(out, connection);
      return;
    }
    const tn::UsdPrim node = stage.GetPrimAtPath(prim_path);
    if (!node.IsValid()) {
      valid = false;
      return;
    }
    const tn::PrimSpec* spec = node.GetPrimSpec();
    const std::string output_property = output.empty()
                                            ? std::string()
                                            : "outputs:" + output;
    if (spec && !output_property.empty()) {
      if (const std::vector<tn::Path>* passthrough =
              spec->connection(output_property)) {
        append(out, "passthrough");
        append(out, output);
        const uint64_t target_count = static_cast<uint64_t>(passthrough->size());
        out->append(reinterpret_cast<const char*>(&target_count),
                    sizeof(target_count));
        for (const tn::Path& target : *passthrough) {
          encode_connection(target.str(), out);
        }
        return;
      }
    }

    append(out, "node");
    append(out, node.GetTypeName());
    append(out, output);
    if (const tn::Value* id = node.GetPropertyValue("info:id")) {
      out->push_back('\1');
      append_value(out, *id);
    } else {
      out->push_back('\0');
    }
    std::vector<std::string> properties = node.GetPropertyNames();
    std::sort(properties.begin(), properties.end());
    properties.erase(std::unique(properties.begin(), properties.end()),
                     properties.end());
    size_t input_count = 0;
    for (const std::string& property_name : properties) {
      if (property_name.rfind("inputs:", 0) == 0) ++input_count;
    }
    const uint64_t encoded_input_count = static_cast<uint64_t>(input_count);
    out->append(reinterpret_cast<const char*>(&encoded_input_count),
                sizeof(encoded_input_count));
    for (const std::string& property_name : properties) {
      if (property_name.rfind("inputs:", 0) != 0) continue;
      // The mesh-only material conversion is evaluated at a selected time.
      // A static key cannot safely alias independently animated parameters.
      if (node.HasTimeSamples(property_name)) {
        valid = false;
        return;
      }
      append(out, property_name.substr(7));
      if (spec) {
        if (const std::string* type =
                spec->property_type_name(property_name)) {
          out->push_back('\1');
          append(out, *type);
        } else {
          out->push_back('\0');
        }
        if (const tn::PropMeta* meta = spec->property_meta(property_name)) {
          out->push_back('\1');
          out->push_back(meta->authored ? '\1' : '\0');
          append(out, meta->colorSpace);
          append(out, meta->renderType);
        } else {
          out->push_back('\0');
        }
        if (const std::vector<tn::Path>* connections =
                spec->connection(property_name)) {
          out->push_back('\1');
          const uint64_t target_count =
              static_cast<uint64_t>(connections->size());
          out->append(reinterpret_cast<const char*>(&target_count),
                      sizeof(target_count));
          for (const tn::Path& target : *connections) {
            encode_connection(target.str(), out);
          }
        } else {
          out->push_back('\0');
        }
      } else {
        out->append(3, '\0');
      }
      if (const tn::Value* value = node.GetPropertyValue(property_name)) {
        out->push_back('\1');
        append_value(out, *value);
      } else {
        out->push_back('\0');
      }
    }
  };

  std::string root;
  root.reserve(2048);
  auto encode_terminal = [&](const char* name) -> bool {
    const std::string property = name;
    const tn::PrimSpec* spec = material.GetPrimSpec();
    const std::vector<tn::Path>* targets =
        spec ? spec->connection(property) : nullptr;
    if (!targets) targets = material.GetRelationship(property);
    if (!targets || targets->empty()) return false;
    append(&root, property);
    const uint64_t target_count = static_cast<uint64_t>(targets->size());
    root.append(reinterpret_cast<const char*>(&target_count),
                sizeof(target_count));
    for (const tn::Path& target : *targets) {
      encode_connection(target.str(), &root);
    }
    return true;
  };
  bool has_terminal = encode_terminal("outputs:mtlx:surface");
  has_terminal = encode_terminal("outputs:surface") || has_terminal;
  if (const std::vector<tn::Path>* sources =
          material.GetRelationship("mtlx:surface:source")) {
    append(&root, "mtlx:surface:source");
    const uint64_t source_count = static_cast<uint64_t>(sources->size());
    root.append(reinterpret_cast<const char*>(&source_count),
                sizeof(source_count));
    for (const tn::Path& target : *sources) {
      encode_connection(target.str(), &root);
    }
    has_terminal = has_terminal || !sources->empty();
  }
  for (const char* config : {"config:mtlx:version", "config:mtlx:namespace",
                             "config:mtlx:colorspace",
                             "config:mtlx:sourceUri"}) {
    if (material.HasTimeSamples(config)) valid = false;
    if (const tn::Value* value = material.GetPropertyValue(config)) {
      append(&root, config);
      append_value(&root, *value);
    }
  }
  return valid && has_terminal ? root : std::string();
}

void AppendNextPhysicsPrimJSON(const tn::UsdPrim& prim, lightusd::minijson::Value* out) {
  if (!out || !prim.IsValid()) return;
  lightusd::minijson::Value item;
  item["name"] = prim.GetName();
  item["path"] = prim.GetPath().str();
  item["type"] = prim.GetTypeName();
  item["apiSchemas"] = lightusd::minijson::Value::array();
  for (const std::string& schema : prim.GetMeta().apiSchemas()) {
    item["apiSchemas"].push_back(schema);
  }
  item["properties"] = lightusd::minijson::Value::object();
  item["relationships"] = lightusd::minijson::Value::object();
  for (const std::string& name : prim.GetPropertyNames()) {
    const tn::Value* value = prim.GetPropertyValue(name);
    if (value) item["properties"][name] = NextValueJSON(*value);
  }
  for (const std::string& name : prim.GetRelationshipNames()) {
    const std::vector<tn::Path>* targets = prim.GetRelationship(name);
    if (!targets) continue;
    lightusd::minijson::Value paths = lightusd::minijson::Value::array();
    for (const tn::Path& path : *targets) paths.push_back(path.str());
    item["relationships"][name] = std::move(paths);
  }
  if (const tn::Value* purpose = prim.GetPropertyValue("purpose")) {
    if (const std::string* token = purpose->as_token()) item["purpose"] = *token;
  }
  if (const tn::Value* matrix = prim.GetPropertyValue("xformOp:transform")) {
    item["matrix"] = NextValueJSON(*matrix);
  }
  const std::string& type = prim.GetTypeName();
  if (type == "Mesh" || type == "Cube" || type == "Sphere" ||
      type == "Cylinder" || type == "Capsule" || type == "Plane") {
    lightusd::minijson::Value geometry;
    geometry["type"] = type == "Mesh" ? "mesh" :
                         type == "Cube" ? "box" :
                         std::string(1, static_cast<char>(std::tolower(type[0]))) +
                             type.substr(1);
    auto copy_property = [&](const char* property, const char* key) {
      if (const tn::Value* value = prim.GetPropertyValue(property)) {
        geometry[key] = NextValueJSON(*value);
      }
    };
    copy_property("points", "positions");
    copy_property("faceVertexIndices", "indices");
    copy_property("normals", "normals");
    copy_property("primvars:st", "uvs");
    copy_property("size", "size");
    copy_property("radius", "radius");
    copy_property("height", "height");
    copy_property("width", "width");
    copy_property("length", "length");
    copy_property("axis", "axis");
    item["geometry"] = std::move(geometry);
  }
  out->push_back(std::move(item));
  for (const tn::UsdPrim& child : prim.GetChildren()) {
    AppendNextPhysicsPrimJSON(child, out);
  }
}


}  // namespace web_next
}  // namespace lightusd
