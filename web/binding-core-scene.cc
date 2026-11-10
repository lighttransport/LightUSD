// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "layer/prim-spec.hh"
#include "binding-core-scene.hh"
#include "writer/layer-json-internal.hh"
#include "schema/color-space.hh"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <set>
#include "stage/stage.hh"
#include "schema/usd-shade.hh"
#include "schema/color-space.hh"
#include "writer/value-printer.hh"

namespace tn = lightusd;
namespace lightusd {
namespace web_core {
minijson::Value NextValueJSON(tn::ValueView value) {
  return ::lightusd::layer_json_detail::ValueJSON(value);
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
      if (auto token = value->as_token()) node_id = *token;
      else if (auto str = value->as_string()) node_id = *str;
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
  auto append_value = [&](std::string* out, tn::ValueView value) {
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
  const std::string& type = prim.GetTypeName();
  item["properties"] = lightusd::minijson::Value::object();
  item["relationships"] = lightusd::minijson::Value::object();
  for (const std::string& name : prim.GetPropertyNames()) {
    const tn::Value* value = prim.GetPropertyValue(name);
    if (!value) continue;
    // Physics snapshots describe mesh topology by counts. Expanding vertex,
    // normal, UV and skin arrays into per-number JSON objects can consume
    // gigabytes for a small compressed Crate, and those arrays were emitted
    // twice (properties and geometry). RenderStream owns the geometry API.
    if (value->is_array()) {
      bool render = name.rfind("primvars:", 0) == 0;
      const auto named = [&](std::initializer_list<const char*> fields) {
        for (const char* field : fields) if (name == field) return true;
        return false;
      };
      if (type == "Mesh")
        render = render || named({"points", "normals", "faceVertexCounts",
          "faceVertexIndices", "holeIndices", "cornerIndices", "cornerSharpnesses",
          "creaseIndices", "creaseLengths", "creaseSharpnesses", "velocities", "accelerations"});
      else if (type == "Points" || type == "BasisCurves" || type == "NurbsCurves")
        render = render || named({"points", "normals", "widths", "ids", "curveVertexCounts",
          "knots", "ranges", "order", "pointWeights", "velocities", "accelerations"});
      else if (type == "PointInstancer")
        render = render || named({"protoIndices", "positions", "orientations", "scales",
          "ids", "invisibleIds", "velocities", "angularVelocities", "accelerations"});
      else if (type == "Skeleton")
        render = render || named({"joints", "bindTransforms", "restTransforms"});
      else if (type == "SkelAnimation")
        render = render || named({"joints", "translations", "rotations", "scales",
          "blendShapes", "blendShapeWeights"});
      else if (type == "BlendShape")
        render = render || named({"offsets", "normalOffsets", "pointIndices"});
      else if (type == "GeomSubset") render = render || name == "indices";
      if (render) continue;
    }
    item["properties"][name] = NextValueJSON(*value);
  }
  for (const std::string& name : prim.GetRelationshipNames()) {
    const std::vector<tn::Path>* targets = prim.GetRelationship(name);
    if (!targets) continue;
    lightusd::minijson::Value paths = lightusd::minijson::Value::array();
    for (const tn::Path& path : *targets) paths.push_back(path.str());
    item["relationships"][name] = std::move(paths);
  }
  if (const tn::Value* purpose = prim.GetPropertyValue("purpose")) {
    if (auto token = purpose->as_token()) item["purpose"] = *token;
  }
  if (const tn::Value* matrix = prim.GetPropertyValue("xformOp:transform")) {
    item["matrix"] = NextValueJSON(*matrix);
  }
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
    if (type == "Mesh") {
      const auto* points = prim.GetPropertyValue("points");
      const auto* faces = prim.GetPropertyValue("faceVertexCounts");
      geometry["pointCount"] = uint64_t(points ? points->array_size() : 0);
      geometry["faceCount"] = uint64_t(faces ? faces->array_size() : 0);
    }
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


}  // namespace web_core
}  // namespace lightusd
