// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Tydra Next - Preview-surface material extraction
#include "safe-arithmetic.hh"
#include "core/path-expression-eval.hh"
#include "next/schema/usd-vol.hh"
#include "next/schema/usd-geom-camera.hh"
#include "render-converter.hh"
#include "mem-budget.hh"
#include "next/schema/color-space.hh"
#include "next/eval/value-clip.hh"
#include "next/resolver/asset-resolver.hh"
#include "materialx.hh"
#include "next/layer/asset-anchor.hh"
#include "next/schema/usdPhysics.hh"
#include "next/schema/usd-shade.hh"
#include "next/schema/usd-skel.hh"
#include "next/types/type-info.hh"
#include "tydra/fast-mikktspace.hh"
#include "tydra/mikktspace-tangent.hh"
#include "tydra/shape-to-mesh.hh"
#include "tydra/tangent-quantize.hh"
#include "tsd/tinysubdiv.hh"
#include "external/mapbox/earcut/earcut.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <deque>
#include <memory>
#include <unordered_map>
#include <optional>
#include <set>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <unordered_set>
namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::Stage; using ::lightusd::next::UsdPrim; using ::lightusd::next::Value;
namespace {
constexpr float kAlphaEpsilon = 1.0e-6f;

constexpr int kMaxMtlxConstantDepth = 64;
std::string SourcePrimPathFromConnection(const std::string& connection_path) {
  size_t dot = connection_path.find(".outputs:");
  if (dot == std::string::npos) dot = connection_path.find(".inputs:");
  if (dot == std::string::npos) dot = connection_path.rfind('.');
  return dot == std::string::npos ? connection_path : connection_path.substr(0, dot);
}
bool SplitConnectionPath(const std::string& connection_path,
                         std::string* prim_path,
                         std::string* prop_name) {
  if (!prim_path || !prop_name) return false;
  size_t dot_pos = connection_path.find(".outputs:");
  if (dot_pos == std::string::npos) {
    dot_pos = connection_path.find(".inputs:");
  }
  if (dot_pos == std::string::npos) {
    dot_pos = connection_path.rfind('.');
  }
  if (dot_pos == std::string::npos) return false;

  *prim_path = connection_path.substr(0, dot_pos);
  *prop_name = connection_path.substr(dot_pos + 1);
  return !prim_path->empty() && !prop_name->empty();
}













void SetParamFloat(ShaderParam* out, float x) {
  out->texture_id = -1;
  out->value = Float4(x, 0.0f, 0.0f, 0.0f);
}

void SetParamFloat3(ShaderParam* out, float x, float y, float z) {
  out->texture_id = -1;
  out->value = Float4(x, y, z, 1.0f);
}

void SetParamFloat4(ShaderParam* out, float x, float y, float z, float w) {
  out->texture_id = -1;
  out->value = Float4(x, y, z, w);
}

bool ValueToShaderParam(const Value& value, ShaderParam* out) {
  if (!out || value.is_empty() || value.is_array()) return false;

  if (const float* v = value.as_float()) {
    SetParamFloat(out, *v);
    return true;
  }
  if (const double* v = value.as_double()) {
    SetParamFloat(out, static_cast<float>(*v));
    return true;
  }
  if (const int32_t* v = value.as_int()) {
    SetParamFloat(out, static_cast<float>(*v));
    return true;
  }
  if (const uint32_t* v = value.as_uint()) {
    SetParamFloat(out, static_cast<float>(*v));
    return true;
  }
  if (const bool* v = value.as_bool()) {
    SetParamFloat(out, *v ? 1.0f : 0.0f);
    return true;
  }
  if (const float* v = value.as_float2()) {
    SetParamFloat4(out, v[0], v[1], 0.0f, 1.0f);
    return true;
  }
  if (const float* v = value.as_float3()) {
    SetParamFloat3(out, v[0], v[1], v[2]);
    return true;
  }
  if (const float* v = value.as_float4()) {
    SetParamFloat4(out, v[0], v[1], v[2], v[3]);
    return true;
  }
  if (const double* v = value.as_double2()) {
    SetParamFloat4(out, static_cast<float>(v[0]), static_cast<float>(v[1]),
                   0.0f, 1.0f);
    return true;
  }
  if (const double* v = value.as_double3()) {
    SetParamFloat3(out, static_cast<float>(v[0]), static_cast<float>(v[1]),
                   static_cast<float>(v[2]));
    return true;
  }
  if (const double* v = value.as_double4()) {
    SetParamFloat4(out, static_cast<float>(v[0]), static_cast<float>(v[1]),
                   static_cast<float>(v[2]), static_cast<float>(v[3]));
    return true;
  }
  // Half-precision shader inputs (half/half2/half3/half4 + role types) store
  // raw half-bit lanes; widen through the converting reads.
  {
    float h[4];
    if (value.to_float(h)) {
      SetParamFloat(out, h[0]);
      return true;
    }
    if (value.to_float2(h)) {
      SetParamFloat4(out, h[0], h[1], 0.0f, 1.0f);
      return true;
    }
    if (value.to_float3(h)) {
      SetParamFloat3(out, h[0], h[1], h[2]);
      return true;
    }
    if (value.to_float4(h)) {
      SetParamFloat4(out, h[0], h[1], h[2], h[3]);
      return true;
    }
  }

  return false;
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c == '\\' || c == '"') out.push_back('\\');
    if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
  }
  return out;
}

std::string ConnectionNodeName(const std::string& path) {
  const size_t slash = path.rfind('/');
  const size_t dot = path.rfind('.');
  if (slash == std::string::npos || dot == std::string::npos || dot <= slash)
    return {};
  return path.substr(slash + 1, dot - slash - 1);
}

std::string ConnectionOutputName(const std::string& path) {
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) return {};
  std::string out = path.substr(dot + 1);
  if (out.compare(0, 8, "outputs:") == 0) out.erase(0, 8);
  return out;
}

std::string ConnectionPropertyName(const std::string& path) {
  const size_t dot = path.rfind('.');
  return dot == std::string::npos ? std::string() : path.substr(dot + 1);
}

void EmitNextGraphValue(std::ostream& os, const Value& value) {
  if (const std::string* v = value.as_asset_path()) {
    os << '"' << JsonEscape(*v) << '"';
    return;
  }
  if (const std::string* v = value.as_string()) {
    os << '"' << JsonEscape(*v) << '"';
    return;
  }
  if (const std::string* v = value.as_token()) {
    os << '"' << JsonEscape(*v) << '"';
    return;
  }
  ShaderParam p;
  if (!ValueToShaderParam(value, &p)) { os << "null"; return; }
  int lanes = 1;
  if (value.as_float2() || value.as_double2()) lanes = 2;
  else if (value.as_float3() || value.as_double3()) lanes = 3;
  else if (value.as_float4() || value.as_double4()) lanes = 4;
  const float v[4] = {p.value.x, p.value.y, p.value.z, p.value.w};
  if (lanes == 1) { os << v[0]; return; }
  os << '[';
  for (int i = 0; i < lanes; ++i) { if (i) os << ','; os << v[i]; }
  os << ']';
}

// Preserve the programmable MaterialX graph in the same compact JSON schema
// consumed by the shared lusdview graph compiler. The next converter already
// resolves simple constants and images; this record retains the full utility
// node topology for descriptor-backed renderers instead of silently baking it.
std::string BuildNextMaterialXGraphJson(const Stage& stage,
                                        const UsdPrim& shader,
                                        bool volume_graph = false) {
  const UsdPrim material = shader.GetParent();
  if (!material.IsValid()) return {};
  std::vector<UsdPrim> graphs;
  ::lightusd::next::AttributeEval shader_connections(&stage);
  for (const std::string& prop : shader.GetPropertyNames()) {
    if (prop.compare(0, 7, "inputs:") != 0 ||
        !shader_connections.HasConnection(shader, prop)) continue;
    const std::string connection =
        shader_connections.GetConnectionPath(shader, prop);
    UsdPrim candidate = stage.GetPrimAtPath(
        SourcePrimPathFromConnection(connection));
    if (::lightusd::next::IsNodeGraph(candidate) &&
        std::none_of(graphs.begin(), graphs.end(), [&](const UsdPrim& item) {
          return item.GetPath() == candidate.GetPath();
        })) {
      graphs.push_back(candidate);
    }
  }
  for (size_t i = 0; i < material.GetChildCount(); ++i) {
    if (!graphs.empty()) break;
    UsdPrim child = material.GetChildAt(i);
    if (::lightusd::next::IsNodeGraph(child)) { graphs.push_back(child); break; }
  }
  const bool direct_graph = graphs.empty();
  const bool graph_forest = graphs.size() > 1;
  const std::string graph_name = direct_graph
      ? material.GetName() + "_direct_graph"
      : (graph_forest ? material.GetName() + "_graphs"
                      : graphs.front().GetName());
  std::vector<UsdPrim> graph_nodes;
  std::function<void(const UsdPrim&)> collect_nodes = [&](const UsdPrim& parent) {
    for (size_t ci = 0; ci < parent.GetChildCount(); ++ci) {
      const UsdPrim child = parent.GetChildAt(ci);
      if (::lightusd::next::IsShader(child)) graph_nodes.push_back(child);
      else if (::lightusd::next::IsNodeGraph(child)) collect_nodes(child);
    }
  };
  if (direct_graph) {
    std::unordered_set<std::string> visited;
    std::function<void(const UsdPrim&)> collect_upstream =
        [&](const UsdPrim& node) {
          if (!node.IsValid() || !::lightusd::next::IsShader(node) ||
              node.GetPath() == shader.GetPath() ||
              !visited.insert(node.GetPath().str()).second) return;
          ::lightusd::next::AttributeEval eval(&stage);
          for (const std::string& prop : node.GetPropertyNames()) {
            if (prop.compare(0, 7, "inputs:") != 0 ||
                !eval.HasConnection(node, prop)) continue;
            collect_upstream(stage.GetPrimAtPath(SourcePrimPathFromConnection(
                eval.GetConnectionPath(node, prop))));
          }
          graph_nodes.push_back(node);
        };
    for (const std::string& prop : shader.GetPropertyNames()) {
      if (prop.compare(0, 7, "inputs:") != 0 ||
          !shader_connections.HasConnection(shader, prop)) continue;
      collect_upstream(stage.GetPrimAtPath(SourcePrimPathFromConnection(
          shader_connections.GetConnectionPath(shader, prop))));
    }
  } else {
    for (const UsdPrim& graph : graphs) collect_nodes(graph);
  }
  std::unordered_map<std::string, std::string> graph_node_names;
  if (direct_graph) {
    for (const UsdPrim& node : graph_nodes) {
      std::string relative = node.GetPath().str();
      const std::string material_path = material.GetPath().str();
      if (relative.compare(0, material_path.size(), material_path) == 0)
        relative.erase(0, material_path.size());
      while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);
      std::replace(relative.begin(), relative.end(), '/', '_');
      graph_node_names[node.GetPath().str()] =
          relative.empty() ? node.GetName() : relative;
    }
  } else for (const UsdPrim& graph : graphs) {
    const std::string graph_path = graph.GetPath().str();
    for (const UsdPrim& node : graph_nodes) {
      std::string relative = node.GetPath().str();
      if (relative.compare(0, graph_path.size(), graph_path) != 0) continue;
      relative.erase(0, graph_path.size());
      while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);
      std::replace(relative.begin(), relative.end(), '/', '_');
      if (graph_forest) relative = graph.GetName() + '_' + relative;
      graph_node_names[node.GetPath().str()] =
          relative.empty() ? node.GetName() : relative;
    }
  }
  // A connection may target an output on a nested NodeGraph. Chase those
  // forwarding outputs until the actual Shader output is reached; the packed
  // runtime has no graph-boundary node and should see the flattened topology.
  auto resolve_connection = [&](std::string connection) {
    for (int depth = 0; depth < 16; ++depth) {
      const UsdPrim source = stage.GetPrimAtPath(
          SourcePrimPathFromConnection(connection));
      if (!source.IsValid() || !::lightusd::next::IsNodeGraph(source)) break;
      const std::string property = ConnectionPropertyName(connection);
      if (property.empty()) break;
      ::lightusd::next::AttributeEval eval(&stage);
      if (!eval.HasConnection(source, property)) break;
      const std::string forwarded = eval.GetConnectionPath(source, property);
      if (forwarded.empty() || forwarded == connection) break;
      connection = forwarded;
    }
    return connection;
  };
  // Unconnected surface inputs are already preserved by the typed material
  // converter. Keep them out of the executable graph: synthesizing Constant
  // nodes for every direct value duplicated work at each hit and could replace
  // the authoritative typed block with graph-evaluator defaults. Only authored
  // connections belong in this runtime record.
  std::ostringstream os;
  os << "{\"version\":\"1.39\",\"nodegraph\":{\"name\":\""
     << JsonEscape(graph_name) << "\",\"inputs\":[],\"nodes\":[";
  bool first_node = true;
  for (const UsdPrim& node : graph_nodes) {
    std::string node_id;
    GetToken(node, "info:id", &node_id);
    std::string category = node_id;
    if (category.compare(0, 3, "ND_") == 0) {
      category.erase(0, 3);
      const size_t suffix = category.rfind('_');
      if (suffix != std::string::npos) category.erase(suffix);
    }
    if (!first_node) os << ',';
    first_node = false;
    os << "{\"name\":\"" << JsonEscape(graph_node_names[node.GetPath().str()])
       << "\",\"category\":\"" << JsonEscape(category)
       << "\",\"type\":\"" << JsonEscape(node_id)
       << "\",\"inputs\":[";
    bool first_input = true;
    ::lightusd::next::AttributeEval eval(&stage);
    for (const std::string& prop : node.GetPropertyNames()) {
      if (prop.compare(0, 7, "inputs:") != 0) continue;
      if (!first_input) os << ',';
      first_input = false;
      os << "{\"name\":\"" << JsonEscape(prop.substr(7)) << '"';
      if (eval.HasConnection(node, prop)) {
        const std::string connection = resolve_connection(
            eval.GetConnectionPath(node, prop));
        const std::string source_path = SourcePrimPathFromConnection(connection);
        const auto source_name = graph_node_names.find(source_path);
        os << ",\"nodename\":\""
           << JsonEscape(source_name == graph_node_names.end()
                             ? ConnectionNodeName(connection)
                             : source_name->second)
           << "\",\"output\":\"" << JsonEscape(ConnectionOutputName(connection)) << '"';
      } else if (const Value* value = node.GetPropertyValueOrEarliestTimeSample(prop)) {
        os << ",\"value\":"; EmitNextGraphValue(os, *value);
        if (const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec()) {
          if (const std::string* type = spec->property_type_name(prop))
            os << ",\"type\":\"" << JsonEscape(*type) << '"';
        }
      }
      os << '}';
    }
    os << "]}";
  }
  os << "],\"outputs\":[";
  bool first_output = true;
  ::lightusd::next::AttributeEval shader_eval(&stage);
  auto runtime_input_name = [&](const std::string& name) {
    if (!volume_graph) return name;
    if (name == "density") return std::string("volume_density");
    if (name == "scattering_color" || name == "scatter_color")
      return std::string("volume_albedo");
    if (name == "emission_color" || name == "emissionColor")
      return std::string("volume_emission_color");
    if (name == "emission" || name == "emission_intensity" ||
        name == "emissionIntensity") return std::string("volume_emission_scale");
    if (name == "anisotropy" || name == "scatter_anisotropy" ||
        name == "scattering_anisotropy") return std::string("volume_anisotropy");
    return name;
  };
  if (direct_graph) {
    for (const std::string& prop : shader.GetPropertyNames()) {
      if (prop.compare(0, 7, "inputs:") != 0 ||
          !shader_eval.HasConnection(shader, prop)) continue;
      const std::string connection = resolve_connection(
          shader_eval.GetConnectionPath(shader, prop));
      const std::string source_path = SourcePrimPathFromConnection(connection);
      const auto source_name = graph_node_names.find(source_path);
      if (source_name == graph_node_names.end()) continue;
      if (!first_output) os << ',';
      first_output = false;
      os << "{\"name\":\"" << JsonEscape(runtime_input_name(prop.substr(7)))
         << "\",\"nodename\":\"" << JsonEscape(source_name->second)
         << "\",\"output\":\"" << JsonEscape(ConnectionOutputName(connection))
         << "\"}";
    }
  } else for (const UsdPrim& graph : graphs) {
    ::lightusd::next::AttributeEval graph_eval(&stage);
    for (const std::string& prop : graph.GetPropertyNames()) {
      if (prop.compare(0, 8, "outputs:") != 0 ||
          !graph_eval.HasConnection(graph, prop)) continue;
      const std::string connection = resolve_connection(
          graph_eval.GetConnectionPath(graph, prop));
      const std::string source_path = SourcePrimPathFromConnection(connection);
      const auto source_name = graph_node_names.find(source_path);
      if (!first_output) os << ',';
      first_output = false;
      os << "{\"name\":\""
         << JsonEscape((graph_forest ? graph.GetName() + '_' : std::string()) +
                       prop.substr(8))
         << "\",\"nodename\":\""
         << JsonEscape(source_name == graph_node_names.end()
                           ? ConnectionNodeName(connection)
                           : source_name->second)
         << "\",\"output\":\"" << JsonEscape(ConnectionOutputName(connection)) << "\"}";
    }
  }
  os << "]},\"connections\":[";
  bool first_connection = true;
  for (const std::string& prop : shader.GetPropertyNames()) {
    if (prop.compare(0, 7, "inputs:") != 0 ||
        !shader_eval.HasConnection(shader, prop)) continue;
    const std::string connection = shader_eval.GetConnectionPath(shader, prop);
    const UsdPrim source = stage.GetPrimAtPath(
        SourcePrimPathFromConnection(connection));
    if (direct_graph) {
      if (!::lightusd::next::IsShader(source) ||
          graph_node_names.find(source.GetPath().str()) == graph_node_names.end())
        continue;
      if (!first_connection) os << ',';
      first_connection = false;
      os << "{\"input\":\"" << JsonEscape(runtime_input_name(prop.substr(7)))
         << "\",\"nodegraph\":\"" << JsonEscape(graph_name)
         << "\",\"output\":\"" << JsonEscape(prop.substr(7)) << "\"}";
      continue;
    }
    if (!::lightusd::next::IsNodeGraph(source)) continue;
    const auto graph_it = std::find_if(
        graphs.begin(), graphs.end(), [&](const UsdPrim& graph) {
          return graph.GetPath() == source.GetPath();
        });
    if (graph_it == graphs.end()) continue;
    if (!first_connection) os << ',';
    first_connection = false;
      os << "{\"input\":\"" << JsonEscape(runtime_input_name(prop.substr(7)))
       << "\",\"nodegraph\":\"" << JsonEscape(graph_name)
       << "\",\"output\":\""
       << JsonEscape((graph_forest ? source.GetName() + '_' : std::string()) +
                     ConnectionOutputName(connection)) << "\"}";
  }
  os << "]}";
  return first_node || first_output || first_connection ? std::string() : os.str();
}

struct MtlxConstantValue {
  std::array<float, 4> value{{0.0f, 0.0f, 0.0f, 0.0f}};
  int components = 0;
  bool color_managed = false;

  float component(int i) const {
    return value[static_cast<size_t>(i < components ? i : 0)];
  }
};

bool ValueToMtlxConstant(const Value& value, MtlxConstantValue* out) {
  if (!out) return false;
  ShaderParam param;
  if (!ValueToShaderParam(value, &param)) return false;
  out->value = {{param.value.x, param.value.y, param.value.z, param.value.w}};
  if (value.as_float3() || value.as_double3()) out->components = 3;
  else if (value.as_float4() || value.as_double4()) out->components = 4;
  else if (value.as_float2() || value.as_double2()) out->components = 2;
  else {
    float widened[4];
    if (value.to_float4(widened)) out->components = 4;
    else if (value.to_float3(widened)) out->components = 3;
    else if (value.to_float2(widened)) out->components = 2;
    else out->components = 1;
  }
  return true;
}

MtlxConstantValue MtlxBinary(const MtlxConstantValue& a,
                             const MtlxConstantValue& b,
                             const std::function<float(float, float)>& op) {
  MtlxConstantValue out;
  out.components = std::max(a.components, b.components);
  out.color_managed = a.color_managed || b.color_managed;
  for (int i = 0; i < out.components; ++i) {
    out.value[static_cast<size_t>(i)] = op(a.component(i), b.component(i));
  }
  return out;
}

bool EvalMtlxConstantConnection(const Stage& stage,
                                const std::string& connection,
                                double time_code,
                                const std::string& evaluation_space,
                                MtlxConstantValue* out,
                                std::set<std::string>* visiting,
                                int depth);

bool TransformMtlxColorInput(const UsdPrim& node,
                             const std::string& property,
                             const std::string& evaluation_space,
                             MtlxConstantValue* value) {
  if (!value) return false;
  if (value->components < 3 || evaluation_space.empty()) return true;
  const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec();
  const std::string* type = spec ? spec->property_type_name(property) : nullptr;
  if (!type || (type->rfind("color3", 0) != 0 &&
                type->rfind("color4", 0) != 0)) {
    return true;
  }

  std::string source;
  bool authored = false;
  if (!::lightusd::next::color_management::ComputeColorSpaceName(
          node, property, &source, &authored)) {
    return false;
  }
  if (!authored) source = evaluation_space;
  ::lightusd::color::ColorTransform transform;
  if (!::lightusd::next::color_management::BuildColorTransform(
          node, source, evaluation_space, &transform)) {
    return false;
  }
  float rgb[3] = {value->value[0], value->value[1], value->value[2]};
  ::lightusd::color::TransformRGB(transform, rgb);
  value->value[0] = rgb[0];
  value->value[1] = rgb[1];
  value->value[2] = rgb[2];
  value->color_managed =
      transform.source.kind != ::lightusd::color::ColorSpaceKind::Data;
  return true;
}

bool EvalMtlxInput(const Stage& stage, const UsdPrim& node,
                   const std::string& input, double time_code,
                   const std::string& evaluation_space,
                   MtlxConstantValue* out, std::set<std::string>* visiting,
                   int depth) {
  if (!out || depth > kMaxMtlxConstantDepth) return false;
  const std::string property = "inputs:" + input;
  const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec();
  const std::vector<::lightusd::next::Path>* connections =
      spec ? spec->connection(property) : nullptr;
  if (connections && !connections->empty()) {
    return EvalMtlxConstantConnection(stage, (*connections)[0].str(),
                                      time_code, evaluation_space, out,
                                      visiting, depth + 1);
  }
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  ::lightusd::next::EvalOptions options = eval.GetOptions();
  options.follow_connections = false;
  const ::lightusd::next::EvalResult result =
      eval.EvalWith(node, property, options);
  return result.success && ValueToMtlxConstant(result.value, out) &&
         TransformMtlxColorInput(node, property, evaluation_space, out);
}

bool EvalMtlxConstantNode(const Stage& stage, const UsdPrim& node,
                          double time_code,
                          const std::string& evaluation_space,
                          MtlxConstantValue* out,
                          std::set<std::string>* visiting, int depth) {
  if (!out || !node.IsValid() || depth > kMaxMtlxConstantDepth || !visiting) {
    return false;
  }
  const std::string key = node.GetPath().str();
  if (!visiting->insert(key).second) return false;
  struct VisitGuard {
    std::set<std::string>* set;
    std::string key;
    ~VisitGuard() { set->erase(key); }
  } guard{visiting, key};

  std::string id;
  GetToken(node, "info:id", &id);
  auto input = [&](const char* name, MtlxConstantValue* value) {
    return EvalMtlxInput(stage, node, name, time_code, evaluation_space, value,
                         visiting, depth + 1);
  };
  auto starts = [&id](const char* prefix) { return id.rfind(prefix, 0) == 0; };

  if (id == "ND_constant_float" || id == "ND_constant_color3" ||
      id == "ND_constant_vector3" || id == "ND_constant_color4") {
    return input("value", out);
  }

  if (starts("ND_add_") || starts("ND_subtract_") ||
      starts("ND_multiply_") || starts("ND_divide_") ||
      starts("ND_min_") || starts("ND_max_") || starts("ND_power_")) {
    MtlxConstantValue a, b;
    if (!input("in1", &a) || !input("in2", &b)) return false;
    if (starts("ND_add_")) *out = MtlxBinary(a, b, [](float x, float y) { return x + y; });
    else if (starts("ND_subtract_")) *out = MtlxBinary(a, b, [](float x, float y) { return x - y; });
    else if (starts("ND_multiply_")) *out = MtlxBinary(a, b, [](float x, float y) { return x * y; });
    else if (starts("ND_divide_")) *out = MtlxBinary(a, b, [](float x, float y) { return std::abs(y) > 1.0e-8f ? x / y : 0.0f; });
    else if (starts("ND_min_")) *out = MtlxBinary(a, b, [](float x, float y) { return std::min(x, y); });
    else if (starts("ND_max_")) *out = MtlxBinary(a, b, [](float x, float y) { return std::max(x, y); });
    else *out = MtlxBinary(a, b, [](float x, float y) { return std::pow(x, y); });
    return true;
  }

  if (starts("ND_mix_")) {
    MtlxConstantValue bg, fg, amount;
    if (!input("bg", &bg) || !input("fg", &fg) ||
        !input("mix", &amount)) return false;
    const float t = amount.component(0);
    *out = MtlxBinary(bg, fg, [t](float x, float y) {
      return x * (1.0f - t) + y * t;
    });
    return true;
  }

  if (starts("ND_clamp_")) {
    MtlxConstantValue value, low, high;
    if (!input("in", &value) || !input("low", &low) ||
        !input("high", &high)) return false;
    *out = value;
    out->color_managed = value.color_managed || low.color_managed ||
                         high.color_managed;
    for (int i = 0; i < out->components; ++i) {
      out->value[static_cast<size_t>(i)] = std::min(
          std::max(value.component(i), low.component(i)), high.component(i));
    }
    return true;
  }

  if (starts("ND_remap_")) {
    MtlxConstantValue value, in_low, in_high, out_low, out_high;
    if (!input("in", &value) || !input("inlow", &in_low) ||
        !input("inhigh", &in_high) || !input("outlow", &out_low) ||
        !input("outhigh", &out_high)) return false;
    *out = value;
    out->color_managed = value.color_managed || in_low.color_managed ||
                         in_high.color_managed || out_low.color_managed ||
                         out_high.color_managed;
    for (int i = 0; i < out->components; ++i) {
      const float denom = in_high.component(i) - in_low.component(i);
      const float t = std::abs(denom) > 1.0e-8f
                          ? (value.component(i) - in_low.component(i)) / denom
                          : 0.0f;
      out->value[static_cast<size_t>(i)] =
          out_low.component(i) + t * (out_high.component(i) - out_low.component(i));
    }
    return true;
  }

  if (starts("ND_combine3_")) {
    MtlxConstantValue a, b, c;
    if (!input("in1", &a) || !input("in2", &b) || !input("in3", &c)) {
      return false;
    }
    out->components = 3;
    out->value = {{a.component(0), b.component(0), c.component(0), 0.0f}};
    out->color_managed =
        a.color_managed || b.color_managed || c.color_managed;
    return true;
  }

  if (starts("ND_extract_")) {
    MtlxConstantValue value, index;
    if (!input("in", &value) || !input("index", &index)) return false;
    int component = static_cast<int>(index.component(0));
    if (component < 0 || component >= value.components) component = 0;
    out->components = 1;
    out->value[0] = value.value[static_cast<size_t>(component)];
    out->color_managed = value.color_managed;
    return true;
  }

  if (starts("ND_normalize_")) {
    MtlxConstantValue value;
    if (!input("in", &value)) return false;
    float length_squared = 0.0f;
    for (int i = 0; i < value.components; ++i) {
      const float component = value.component(i);
      length_squared += component * component;
    }
    const float length = std::sqrt(length_squared);
    *out = value;
    if (length > 1.0e-7f) {
      for (int i = 0; i < out->components; ++i) {
        out->value[static_cast<size_t>(i)] /= length;
      }
    }
    return true;
  }

  // Constant-fold the common MaterialX unary math family. These nodes occur
  // frequently between DCC-authored controls and surface inputs; treating
  // them as unsupported discarded an otherwise fully evaluable material.
  if (starts("ND_absval_") || starts("ND_floor_") ||
      starts("ND_ceil_") || starts("ND_round_") ||
      starts("ND_sqrt_") || starts("ND_exp_") ||
      starts("ND_ln_") || starts("ND_sin_") || starts("ND_cos_") ||
      starts("ND_tan_")) {
    MtlxConstantValue value;
    if (!input("in", &value)) return false;
    *out = value;
    for (int i = 0; i < out->components; ++i) {
      const float x = value.component(i);
      float y = x;
      if (starts("ND_absval_")) y = std::fabs(x);
      else if (starts("ND_floor_")) y = std::floor(x);
      else if (starts("ND_ceil_")) y = std::ceil(x);
      else if (starts("ND_round_")) y = std::round(x);
      else if (starts("ND_sqrt_")) y = std::sqrt(std::max(0.0f, x));
      else if (starts("ND_exp_")) y = std::exp(x);
      else if (starts("ND_ln_")) y = x > 0.0f ? std::log(x) : 0.0f;
      else if (starts("ND_sin_")) y = std::sin(x);
      else if (starts("ND_cos_")) y = std::cos(x);
      else if (starts("ND_tan_")) y = std::tan(x);
      out->value[static_cast<size_t>(i)] = y;
    }
    return true;
  }

  if (starts("ND_ifgreater_") || starts("ND_ifgreatereq_") ||
      starts("ND_ifequal_")) {
    MtlxConstantValue value1, value2, when_true, when_false;
    if (!input("value1", &value1) || !input("value2", &value2) ||
        !input("in1", &when_true) || !input("in2", &when_false)) {
      return false;
    }
    const float a = value1.component(0);
    const float b = value2.component(0);
    bool condition = false;
    if (starts("ND_ifgreatereq_")) {
      condition = a >= b;
    } else if (starts("ND_ifgreater_")) {
      condition = a > b;
    } else {
      const float scale = std::max({std::fabs(a), std::fabs(b), 1.0f});
      condition = std::fabs(a - b) <=
                  std::numeric_limits<float>::epsilon() * scale;
    }
    *out = condition ? when_true : when_false;
    return true;
  }

  if (starts("ND_convert_")) return input("in", out);

  if (id == "ND_hsv_adjust_color3" || id == "ND_hsvadjust_color3") {
    MtlxConstantValue color, hue, saturation, value, factor;
    // The standard MaterialX hsvadjust amount is a direct hue offset with
    // (0, 1, 1) as its identity. Blender's separate-input variant exposes a
    // UI control where 0.5 is neutral instead.
    const float hue_neutral =
        id == "ND_hsvadjust_color3" ? 0.0f : 0.5f;
    if (!input("in", &color)) return false;
    if (id == "ND_hsvadjust_color3") {
      MtlxConstantValue amount;
      if (!input("amount", &amount) || amount.components < 3) return false;
      hue.components = saturation.components = value.components =
          factor.components = 1;
      hue.value[0] = amount.component(0);
      saturation.value[0] = amount.component(1);
      value.value[0] = amount.component(2);
      factor.value[0] = 1.0f;
    } else if (!input("hue", &hue) ||
               !input("saturation", &saturation) ||
               !input("value", &value) || !input("fac", &factor)) {
      return false;
    }
    const float r = color.component(0), g = color.component(1), b = color.component(2);
    const float maximum = std::max({r, g, b});
    const float minimum = std::min({r, g, b});
    const float delta = maximum - minimum;
    float h = 0.0f;
    if (delta > 1.0e-7f) {
      if (r >= maximum) h = (g - b) / delta;
      else if (g >= maximum) h = 2.0f + (b - r) / delta;
      else h = 4.0f + (r - g) / delta;
      h /= 6.0f;
      if (h < 0.0f) h += 1.0f;
    }
    float s = maximum > 0.0f ? delta / maximum : 0.0f;
    float v = maximum;
    h = std::fmod(h + (hue.component(0) - hue_neutral) + 1.0f, 1.0f);
    s *= saturation.component(0);
    v *= value.component(0);
    float adjusted[3] = {v, v, v};
    if (s > 0.0f) {
      const float hh = h * 6.0f;
      const int sector = static_cast<int>(std::floor(hh)) % 6;
      const float fraction = hh - std::floor(hh);
      const float p = v * (1.0f - s);
      const float q = v * (1.0f - s * fraction);
      const float t = v * (1.0f - s * (1.0f - fraction));
      const float table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t},
                                 {p, q, v}, {t, p, v}, {v, p, q}};
      for (int i = 0; i < 3; ++i) adjusted[i] = table[sector][i];
    }
    const float mix = factor.component(0);
    out->components = 3;
    out->color_managed = color.color_managed;
    for (int i = 0; i < 3; ++i) {
      out->value[static_cast<size_t>(i)] =
          color.component(i) * (1.0f - mix) + adjusted[i] * mix;
    }
    return true;
  }
  return false;
}

bool EvalMtlxConstantConnection(const Stage& stage,
                                const std::string& connection,
                                double time_code,
                                const std::string& evaluation_space,
                                MtlxConstantValue* out,
                                std::set<std::string>* visiting,
                                int depth) {
  if (!out || depth > kMaxMtlxConstantDepth) return false;
  std::string prim_path, property;
  if (!SplitConnectionPath(connection, &prim_path, &property)) return false;
  const UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;
  const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
  const std::vector<::lightusd::next::Path>* forwarded =
      spec ? spec->connection(property) : nullptr;
  if (forwarded && !forwarded->empty()) {
    return EvalMtlxConstantConnection(stage, (*forwarded)[0].str(), time_code,
                                      evaluation_space, out, visiting,
                                      depth + 1);
  }
  if (::lightusd::next::IsShader(prim)) {
    return EvalMtlxConstantNode(stage, prim, time_code, evaluation_space, out,
                                visiting, depth + 1);
  }
  return false;
}


// Closed-form Euler-degrees -> quaternion (xyzw) for all six USD rotation
// orders (rotateXYZ means apply X first: Q = Qz * Qy * Qx). Ported from the
// legacy tydra converter so Rotation channels always carry quaternions.

// Extracts the axis order ("XYZ", "ZYX", ...) from an xformOp:rotate<ORDER>
// property name. Returns false for single-axis rotateX/Y/Z and non-rotate ops.
}  // namespace


bool IsStandardSurfaceShaderId(const std::string& id) { return id == "ND_standard_surface_surfaceshader" || id == "standard_surface" || id == "AutodeskStandardSurface" || id == "MtlxAutodeskStandardSurface"; }
bool IsOpenPBRShaderId(const std::string& id) { return id == "ND_open_pbr_surface_surfaceshader" || id == "open_pbr_surface" || id == "OpenPBRSurface" || id == "MtlxOpenPBRSurface"; }
bool IsSurfaceUnlitShaderId(const std::string& id) { return id == "ND_surface_unlit_surfaceshader" || id == "surface_unlit" || id == "SurfaceUnlit" || id == "MtlxSurfaceUnlit"; }

bool ReadStringLikeProperty(const UsdPrim& prim, const char* name,
                            std::string* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (const std::string* text = value->as_string()) { *out = *text; return true; }
  if (const std::string* token = value->as_token()) { *out = *token; return true; }
  if (const std::string* asset = value->as_asset_path()) { *out = *asset; return true; }
  return false;
}

void ExtractMaterialXConfig(const UsdPrim& prim,
                            RenderMaterial::MaterialXConfig* out) {
  if (!out || !prim.IsValid()) return;
  std::string v;
  if (ReadStringLikeProperty(prim, "config:mtlx:version", &v)) {
    out->version = v;
    out->authored = true;
  }
  if (ReadStringLikeProperty(prim, "config:mtlx:namespace", &v)) {
    out->name_space = v;
    out->authored = true;
  }
  if (ReadStringLikeProperty(prim, "config:mtlx:colorspace", &v)) {
    out->colorspace = v;
    out->authored = true;
  }
  if (ReadStringLikeProperty(prim, "config:mtlx:sourceUri", &v) ||
      ReadStringLikeProperty(prim, "config:mtlx:sourceAsset", &v) ||
      ReadStringLikeProperty(prim, "config:mtlx:file", &v)) {
    out->source_uri = v;
    out->authored = true;
  }
}

bool RenderSceneConverter::ConvertMaterial(const Stage& stage,
                                           const UsdPrim& prim,
                                           RenderMaterial* out) {
  return ConvertMaterial(stage, prim, out, nullptr);
}

bool RenderSceneConverter::ConvertMaterial(const Stage& stage,
                                           const UsdPrim& prim,
                                           RenderMaterial* out,
                                           RenderScene* scene) {
  if (!out || !::lightusd::next::IsMaterial(prim)) {
    SetLastError("Invalid material prim");
    return false;
  }

  // The rendering color config depends only on `stage` and
  // config_.material.render_settings_path -- both invariant across a
  // conversion -- yet this did a stage path lookup, token canonicalization,
  // color-space resolve and a 3x3 transform build once PER MATERIAL, writing
  // the identical result to `scene` every time.
  // In the parallel-worker local scope, this is skipped: the caller
  // pre-seeds the scratch scene's working_color_space (and the other fields
  // this block would compute) from the already-resolved result.scene value,
  // and color_config_scene_ is a shared member no worker thread may write.
  if (scene && scene != color_config_scene_ && !tl_material_local_scope_) {
    color_config_scene_ = scene;
    ::lightusd::next::color_management::RenderingColorConfig color_config;
    std::string color_warning;
    if (::lightusd::next::color_management::ResolveRenderingColorConfig(
            stage, config_.material.render_settings_path, &color_config,
            &color_warning)) {
      scene->render_settings_path = color_config.render_settings_path;
      scene->working_color_space = color_config.working_space;
      ::lightusd::color::ColorSpaceDesc display_linear;
      ::lightusd::color::ColorTransform display_transform;
      if (::lightusd::color::GetBuiltinColorSpace("lin_rec709_scene",
                                                   &display_linear) &&
          ::lightusd::color::BuildColorTransform(
              color_config.working_definition, display_linear,
              &display_transform)) {
        std::copy(display_transform.matrix, display_transform.matrix + 9,
                  scene->working_to_display_linear);
      }
      (void)color_warning;
    }
  }

  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();
  ExtractMaterialXConfig(prim, &out->mtlx_config);

  // Displacement/volume terminal metadata (legacy parity: recorded, not
  // converted into shader networks).
  {
    const std::string disp =
        ::lightusd::next::GetDisplacementShader(stage, prim);
    if (!disp.empty()) {
      out->has_displacement = true;
      out->displacement_shader_path = disp;
    }
    const std::string vol = ::lightusd::next::GetVolumeShader(stage, prim);
    if (!vol.empty()) {
      out->has_volume = true;
      out->volume_shader_path = vol;
      const UsdPrim volume_shader = stage.GetPrimAtPath(vol);
      if (volume_shader.IsValid()) {
        out->volume_nodegraph_json =
            BuildNextMaterialXGraphJson(stage, volume_shader, true);
      }
    }
  }

  // Find shader(s) in material. The material's `outputs:surface` connection
  // names the authoritative surface shader (child iteration order previously
  // decided ties, and shaders living OUTSIDE the material prim never resolved).
  bool found_shader = false;
  std::vector<UsdPrim> degraded_candidates;

  std::vector<UsdPrim> candidates;
  // Materials exported by Blender commonly author both a PreviewSurface
  // fallback on outputs:surface and the authoritative MaterialX graph on
  // outputs:mtlx:surface. Prefer the explicit MaterialX render context; using
  // the fallback made every graph-driven parameter look unauthored.
  if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
    const std::vector<::lightusd::next::Path>* mtlx_surface =
        spec->connection("outputs:mtlx:surface");
    if (mtlx_surface && !mtlx_surface->empty()) {
      UsdPrim sp = stage.GetPrimAtPath(
          SourcePrimPathFromConnection((*mtlx_surface)[0].str()));
      if (sp.IsValid()) candidates.push_back(sp);
    }
  }
  {
    // When both MaterialX and universal PreviewSurface terminals are authored,
    // prefer the MaterialX terminal. Blender commonly emits a textured OpenPBR
    // graph plus an untextured PreviewSurface fallback; selecting the fallback
    // here silently turns the material white.
    ::lightusd::next::AttributeEval eval(&stage);
    if (eval.HasConnection(prim, "outputs:mtlx:surface")) {
      const std::string mtlx_surface =
          eval.GetConnectionPath(prim, "outputs:mtlx:surface");
      UsdPrim sp = stage.GetPrimAtPath(
          SourcePrimPathFromConnection(mtlx_surface));
      if (sp.IsValid()) candidates.push_back(sp);
    } else if (const std::vector<::lightusd::next::Path>* targets =
                   prim.GetRelationship("outputs:mtlx:surface")) {
      if (!targets->empty()) {
        UsdPrim sp = stage.GetPrimAtPath(
            SourcePrimPathFromConnection((*targets)[0].str()));
        if (sp.IsValid()) candidates.push_back(sp);
      }
    }
    // External MaterialX references are synthesized by the next layer
    // registry with this relationship instead of outputs:mtlx:surface.
    if (const std::vector<::lightusd::next::Path>* sources =
            prim.GetRelationship("mtlx:surface:source")) {
      if (!sources->empty()) {
        UsdPrim sp = stage.GetPrimAtPath(sources->front().str());
        if (sp.IsValid() &&
            std::none_of(candidates.begin(), candidates.end(),
                         [&sp](const UsdPrim& p) {
                           return p.GetPath().str() == sp.GetPath().str();
                         })) {
          candidates.push_back(sp);
        }
      }
    }
    const std::string surf =
        ::lightusd::next::GetSurfaceShader(stage, prim);
    if (!surf.empty()) {
      UsdPrim sp = stage.GetPrimAtPath(surf);
      if (sp.IsValid() &&
          std::none_of(candidates.begin(), candidates.end(),
                       [&sp](const UsdPrim& p) {
                         return p.GetPath().str() == sp.GetPath().str();
                       })) {
        candidates.push_back(sp);
      }
    }
  }
  const size_t child_count = prim.GetChildCount();
  for (size_t i = 0; i < child_count; ++i) {
    const UsdPrim child = prim.GetChildAt(i);
    if (!child.IsValid()) continue;
    candidates.push_back(child);
  }

  // A common UsdShade authoring pattern puts the terminal shader behind one
  // or more NodeGraph output pass-throughs.  The graph is the material's
  // surface target, but the shader is the object that carries the usable
  // `info:id` and inputs.  Resolve these hops before classifying the material
  // as degraded.  Keep this local to material discovery so parameter
  // extraction still follows the original shader connections.
  {
    std::set<std::string> visited;
    std::function<void(const UsdPrim&, int)> append_graph_shader =
        [&](const UsdPrim& source, int depth) {
          if (!source.IsValid() || depth > 16 ||
              !visited.insert(source.GetPath().str()).second) {
            return;
          }
          if (!::lightusd::next::IsNodeGraph(source)) return;

          const ::lightusd::next::PrimSpec* spec = source.GetPrimSpec();
          if (!spec) return;
          static const char* kOutputs[] = {"outputs:surface", "outputs:out"};
          for (const char* output : kOutputs) {
            const std::vector<::lightusd::next::Path>* connections =
                spec->connection(output);
            if (!connections || connections->empty()) continue;
            UsdPrim target = stage.GetPrimAtPath(
                SourcePrimPathFromConnection((*connections)[0].str()));
            if (!target.IsValid()) continue;
            candidates.push_back(target);
            if (::lightusd::next::IsNodeGraph(target)) {
              append_graph_shader(target, depth + 1);
            }
            return;
          }
        };
    const size_t initial_count = candidates.size();
    for (size_t i = 0; i < initial_count; ++i) {
      append_graph_shader(candidates[i], 0);
    }
  }

  for (const auto& child : candidates) {
    if (found_shader) break;
    if (::lightusd::next::IsShader(child)) {
      std::string shader_id;
      GetToken(child, "info:id", &shader_id);

      if (shader_id == "UsdPreviewSurface" ||
          shader_id == "ND_UsdPreviewSurface_surfaceshader") {
        // MaterialX's UsdPreviewSurface node (`ND_UsdPreviewSurface_surfaceshader`)
        // has the same inputs as UsdPreviewSurface — treat it as one (matches the
        // legacy tydra path, e.g. usd-wg MaterialXTest/basic_flatten).
        out->shader_type = RenderMaterial::ShaderType::PreviewSurface;
        out->preview_surface = std::make_unique<PreviewSurfaceShader>();
        ExtractPreviewSurface(stage, child, out->preview_surface.get(), scene);
        if (out->preview_surface->opacity.is_texture() ||
            out->preview_surface->opacity.value.x < 1.0f - kAlphaEpsilon) {
          out->alpha_mode = RenderMaterial::AlphaMode::Blend;
        }
        if (out->preview_surface->opacity_threshold.value.x > kAlphaEpsilon) {
          out->alpha_mode = RenderMaterial::AlphaMode::Mask;
          out->alpha_cutoff = out->preview_surface->opacity_threshold.value.x;
        }
        found_shader = true;
      } else if (IsOpenPBRShaderId(shader_id)) {
        out->shader_type = RenderMaterial::ShaderType::OpenPBR;
        out->openpbr = std::make_unique<OpenPBRSurfaceShader>();
        ExtractOpenPBRSurface(stage, child, out->openpbr.get(), scene);
        out->openpbr->nodegraph_json =
            BuildNextMaterialXGraphJson(stage, child);
        if (out->openpbr->opacity.is_texture() ||
            out->openpbr->opacity.value.x < 1.0f - kAlphaEpsilon ||
            out->openpbr->transmission_weight.value.x > kAlphaEpsilon) {
          // Transmissive OpenPBR (glass) needs the blend path even at
          // opacity 1 (legacy marks these Translucent).
          out->alpha_mode = RenderMaterial::AlphaMode::Blend;
        }
        found_shader = true;
      } else if (IsSurfaceUnlitShaderId(shader_id)) {
        // surface_unlit is a first-class MaterialX surface shader. Reuse the
        // renderer's OpenPBR transport, but disable its reflective lobes and
        // retain the executable graph for emission/transmission/opacity.
        out->shader_type = RenderMaterial::ShaderType::OpenPBR;
        out->openpbr = std::make_unique<OpenPBRSurfaceShader>();
        SetParamFloat(&out->openpbr->base_weight, 0.0f);
        SetParamFloat(&out->openpbr->specular_weight, 0.0f);
        SetParamFloat(&out->openpbr->emission_luminance, 1.0f);
        ExtractShaderParam(stage, child, "emission",
                           &out->openpbr->emission_luminance, scene);
        ExtractShaderParam(stage, child, "emission_color",
                           &out->openpbr->emission_color, scene);
        ExtractShaderParam(stage, child, "transmission",
                           &out->openpbr->transmission_weight, scene);
        ExtractShaderParam(stage, child, "transmission_color",
                           &out->openpbr->transmission_color, scene);
        ExtractShaderParam(stage, child, "normal", &out->openpbr->normal,
                           scene);
        if (!ExtractShaderParam(stage, child, "opacity",
                                &out->openpbr->opacity, scene)) {
          ExtractShaderParam(stage, child, "geometry_opacity",
                             &out->openpbr->opacity, scene);
        }
        out->openpbr->nodegraph_json =
            BuildNextMaterialXGraphJson(stage, child);
        if (out->openpbr->opacity.is_texture() ||
            out->openpbr->opacity.value.x < 1.0f - kAlphaEpsilon ||
            out->openpbr->transmission_weight.value.x > kAlphaEpsilon) {
          out->alpha_mode = RenderMaterial::AlphaMode::Blend;
        }
        found_shader = true;
      } else if (IsStandardSurfaceShaderId(shader_id)) {
        // MaterialX standard_surface maps onto OpenPBR (legacy tydra's
        // ConvertMtlxStandardSurfaceToOpenPBRSurface table).
        out->shader_type = RenderMaterial::ShaderType::OpenPBR;
        out->openpbr = std::make_unique<OpenPBRSurfaceShader>();
        ExtractStandardSurfaceAsOpenPBR(stage, child, out->openpbr.get(), scene);
        out->openpbr->nodegraph_json =
            BuildNextMaterialXGraphJson(stage, child);
        if (out->openpbr->opacity.is_texture() ||
            out->openpbr->opacity.value.x < 1.0f - kAlphaEpsilon) {
          out->alpha_mode = RenderMaterial::AlphaMode::Blend;
        }
        found_shader = true;
      } else {
        // Keep the authoritative unsupported shader (and then any unsupported
        // child shaders) as degraded-material sources. Engine shaders and
        // newer MaterialX nodes often retain familiar PBR input names even
        // when their full implementation cannot be evaluated.
        degraded_candidates.push_back(child);
        MaterialDiagnostic diagnostic;
        diagnostic.kind = shader_id.rfind("ND_", 0) == 0
                              ? MaterialDiagnosticKind::UnsupportedMaterialXNode
                              : MaterialDiagnosticKind::UnsupportedShader;
        diagnostic.material_path = out->prim_path;
        diagnostic.node_path = child.GetPath().str();
        diagnostic.shader_id = shader_id;
        diagnostic.message = "unsupported surface shader";
        out->diagnostics.push_back(std::move(diagnostic));
      }
    }
  }

  if (!found_shader) {
    // MaterialX surface shaders (e.g. ND_standard_surface_surfaceshader):
    // convert through the MaterialX -> PreviewSurface mapping.
    MtlxConverter mtlx;
    RenderMaterial mtlx_out;
    // We are the fallback path: we already failed to find a shader we know, so
    // the converter must not hand this same material back to us.
    if (mtlx.ConvertUsdMtlxMaterial(stage, prim, &mtlx_out,
                                    /*allow_converter_delegation=*/false)) {
      if (mtlx_out.preview_surface && !mtlx_out.default_fallback) {
        out->shader_type = RenderMaterial::ShaderType::PreviewSurface;
        out->preview_surface = std::move(mtlx_out.preview_surface);
        out->alpha_mode = mtlx_out.alpha_mode;
        out->alpha_cutoff = mtlx_out.alpha_cutoff;
        if (!out->mtlx_config.authored) {
          out->mtlx_config = std::move(mtlx_out.mtlx_config);
        }
        found_shader = true;
      } else if (!out->mtlx_config.authored && mtlx_out.mtlx_config.authored) {
        // Retain the MaterialX document metadata even when its surface node is
        // unsupported and the PBR inputs below have to be salvaged by name.
        out->mtlx_config = std::move(mtlx_out.mtlx_config);
      }
    }
  }

  if (!found_shader) {
    // No convertible surface shader. This happens for materials that only
    // reference an engine source asset and author no UsdPreviewSurface — e.g.
    // Unreal Engine USD exports whose surface is `outputs:unreal:surface` ->
    // an `info:implementationSource = "sourceAsset"` shader
    // (`info:unreal:sourceAsset = @...uasset@`), as in MetaHuman face/body
    // materials — or whose surface connection doesn't resolve after
    // composition. Emit a per-material degraded PreviewSurface rather than
    // dropping it. Recover conventional PBR input aliases from the unsupported
    // surface shader first, then from Material interface inputs. This preserves
    // the useful constants around an unsupported node instead of replacing the
    // entire material with shared gray.
    out->shader_type = RenderMaterial::ShaderType::PreviewSurface;
    out->preview_surface = std::make_unique<PreviewSurfaceShader>();
    out->default_fallback = true;

    PreviewSurfaceShader* degraded = out->preview_surface.get();
    // White is the neutral multiplier for a mesh-authored displayColor. If no
    // recognizable base color can be recovered, this lets displayColor remain
    // visible instead of being darkened by the PreviewSurface schema fallback.
    SetParamFloat3(&degraded->diffuse_color, 1.0f, 1.0f, 1.0f);

    size_t recovered_count = 0;
    auto recover = [&](ShaderParam* dst,
                       std::initializer_list<const char*> aliases) {
      // An unsupported terminal appears first in candidates; deduplicate child
      // traversal so texture/image nodes cannot accidentally override it.
      std::set<std::string> visited;
      auto recover_from = [&](const UsdPrim& source) {
        if (!source.IsValid() ||
            !visited.insert(source.GetPath().str()).second) {
          return false;
        }
        for (const char* alias : aliases) {
          ShaderParam value = *dst;
          if (ExtractShaderParam(stage, source, alias, &value, scene)) {
            *dst = value;
            ++recovered_count;
            return true;
          }
        }
        return false;
      };
      for (const UsdPrim& source : degraded_candidates) {
        if (recover_from(source)) return;
      }
      // Material interface inputs are common in MaterialX exports and remain
      // meaningful even when the connected surface implementation is unknown.
      recover_from(prim);
    };

    // Preserve every evaluatable authored input from the unsupported terminal
    // and the material interface, not only the conventional aliases above.
    // This keeps advanced lobes and future shader inputs available without
    // making the degraded PreviewSurface pretend to evaluate them.
    std::set<std::string> retained_names;
    auto retain_inputs = [&](const UsdPrim& source, const std::string& shader) {
      if (!source.IsValid()) return;
      for (const std::string& property : source.GetPropertyNames()) {
        constexpr const char* kPrefix = "inputs:";
        if (property.rfind(kPrefix, 0) != 0) continue;
        const std::string name = property.substr(7);
        if (name.empty() || !retained_names.insert(name).second) continue;
        ShaderParam value;
        if (!ExtractShaderParam(stage, source, name, &value, scene)) continue;
        RetainedMaterialParam param;
        param.shader = shader;
        param.name = name;
        param.value = value;
        out->retained_params.push_back(std::move(param));
      }
    };
    for (const UsdPrim& source : degraded_candidates) {
      std::string shader;
      GetToken(source, "info:id", &shader);
      retain_inputs(source, shader);
    }
    retain_inputs(prim, "MaterialInterface");

    recover(&degraded->diffuse_color,
            {"diffuseColor", "baseColor", "base_color", "color"});
    recover(&degraded->emissive_color,
            {"emissiveColor", "emissionColor", "emission_color"});
    recover(&degraded->specular_color,
            {"specularColor", "specular_color"});
    recover(&degraded->metallic,
            {"metallic", "metalness", "base_metalness"});
    recover(&degraded->roughness,
            {"roughness", "base_roughness", "specular_roughness"});
    recover(&degraded->clearcoat,
            {"clearcoat", "coat", "coat_weight"});
    recover(&degraded->clearcoat_roughness,
            {"clearcoatRoughness", "coat_roughness"});
    recover(&degraded->opacity,
            {"opacity", "geometry_opacity", "alpha"});
    recover(&degraded->opacity_threshold,
            {"opacityThreshold", "alphaCutoff", "alpha_cutoff"});
    recover(&degraded->ior, {"ior", "specular_ior", "specular_IOR"});
    recover(&degraded->normal, {"normal", "geometry_normal"});
    recover(&degraded->displacement, {"displacement"});
    recover(&degraded->occlusion, {"occlusion"});

    ShaderParam use_spec;
    recover(&use_spec, {"useSpecularWorkflow", "use_specular_workflow"});
    degraded->use_specular_workflow = use_spec.value.x >= 0.5f;

    if (degraded->opacity.is_texture() ||
        degraded->opacity.value.x < 1.0f - kAlphaEpsilon) {
      out->alpha_mode = RenderMaterial::AlphaMode::Blend;
    }
    if (degraded->opacity_threshold.value.x > kAlphaEpsilon) {
      out->alpha_mode = RenderMaterial::AlphaMode::Mask;
      out->alpha_cutoff = degraded->opacity_threshold.value.x;
    }
    AddWarning(
        "Material '" + out->prim_path +
        "' has no fully convertible surface shader; using a degraded material "
        "with " + std::to_string(recovered_count) +
        " recovered input(s).");
    MaterialDiagnostic diagnostic;
    diagnostic.kind = MaterialDiagnosticKind::DegradedMaterial;
    diagnostic.material_path = out->prim_path;
    diagnostic.node_path = degraded_candidates.empty()
                               ? std::string()
                               : degraded_candidates.front().GetPath().str();
    if (!degraded_candidates.empty()) {
      GetToken(degraded_candidates.front(), "info:id", &diagnostic.shader_id);
    }
    diagnostic.message = "using degraded material with " +
                         std::to_string(recovered_count) +
                         " recovered input(s)";
    out->diagnostics.push_back(std::move(diagnostic));
    found_shader = true;
  }

  // Translate the production RenderMan Ptex displacement pattern used by the
  // Island asset into the renderer-neutral PreviewSurface displacement lane:
  //
  // PxrDisplace.dispScalar <- PxrDispTransform.dispScalar <- PxrPtexture
  // result = (texture - dispCenter) * dispAmount  (dispRemapMode == 2)
  //
  // Keep this compatibility adapter narrow and diagnosed. Unknown Pxr graphs
  // remain visible as unsupported displacement instead of being guessed.
  if (found_shader && out->preview_surface && out->has_displacement && scene) {
    const UsdPrim displacement =
        stage.GetPrimAtPath(out->displacement_shader_path);
    std::string displacementId;
    GetToken(displacement, "info:id", &displacementId);
    if (displacement.IsValid() && displacementId == "PxrDisplace") {
      ::lightusd::next::AttributeEval eval(&stage);
      eval.SetTime(config_.time_code);
      const std::string scalarConnection =
          eval.GetConnectionPath(displacement, "inputs:dispScalar");
      const UsdPrim transform = stage.GetPrimAtPath(
          SourcePrimPathFromConnection(scalarConnection));
      std::string transformId;
      GetToken(transform, "info:id", &transformId);
      const std::optional<int32_t> remap =
          transform.IsValid()
              ? eval.EvalInt(transform, "inputs:dispRemapMode")
              : std::optional<int32_t>();
      if (transform.IsValid() && transformId == "PxrDispTransform" &&
          (!remap || *remap == 2)) {
        ShaderParam scalar;
        ShaderParam amountParam;
        ShaderParam centerParam;
        const bool haveScalar = ExtractShaderParam(
            stage, displacement, "dispScalar", &scalar, scene);
        const bool haveAmount = ExtractShaderParam(
            stage, displacement, "dispAmount", &amountParam, scene);
        const bool haveCenter = ExtractShaderParam(
            stage, transform, "dispCenter", &centerParam, scene);
        const float amount = haveAmount ? amountParam.value.x : 1.0f;
        const float center = haveCenter ? centerParam.value.x : 0.0f;
        if (haveScalar) {
          if (scalar.texture_id >= 0 &&
              static_cast<size_t>(scalar.texture_id) < scene->textures.size()) {
            RenderTexture& texture =
                scene->textures[static_cast<size_t>(scalar.texture_id)];
            texture.scale_value.x *= amount;
            texture.bias.x = texture.bias.x * amount - center * amount;
            texture.source_color_space = "raw";
            if (texture.image_id >= 0 &&
                static_cast<size_t>(texture.image_id) < scene->images.size()) {
              scene->images[static_cast<size_t>(texture.image_id)].color_space =
                  ColorSpace::Raw;
            }
          } else {
            scalar.value.x = (scalar.value.x - center) * amount;
          }
          out->preview_surface->displacement = scalar;
        } else {
          AddWarning("Material '" + out->prim_path +
                              "' has an unreadable Pxr Ptex displacement graph");
        }
      } else {
        MaterialDiagnostic diagnostic;
        diagnostic.kind = MaterialDiagnosticKind::UnsupportedShader;
        diagnostic.material_path = out->prim_path;
        diagnostic.node_path = transform.IsValid()
                                   ? transform.GetPath().str()
                                   : out->displacement_shader_path;
        diagnostic.shader_id = transformId;
        diagnostic.message = "unsupported Pxr displacement transform/remap mode";
        out->diagnostics.push_back(std::move(diagnostic));
      }
    }
  }

  return found_shader;
}
}}}  // namespace lightusd::tydra::next
