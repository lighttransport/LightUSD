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
constexpr int kMaxMtlxConstantDepth = 64;
const ::lightusd::next::PropNameId& kIdOutputsOut() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("outputs:out");
  return id;
}
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

bool IsTextureEndpoint(const Stage& stage, const UsdPrim& prim,
                       double time_code) {
  if (!prim.IsValid()) return false;
  std::string id;
  GetToken(prim, "info:id", &id);
  if (id == "UsdUVTexture" || id == "HwPtexTexture" ||
      id.rfind("HwPtexTexture", 0) == 0 || id == "PxrPtexture" ||
      id == "image" || id == "tiledimage" ||
      id.rfind("ND_image_", 0) == 0 ||
      id.rfind("ND_tiledimage_", 0) == 0) {
    return true;
  }
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  return eval.EvalAssetPath(prim, "inputs:file").has_value() ||
         eval.EvalString(prim, "inputs:file").has_value() ||
         eval.EvalAssetPath(prim, "inputs:filename").has_value() ||
         eval.EvalString(prim, "inputs:filename").has_value();
}

const std::vector<::lightusd::next::Path>* PrimaryDataInputConnection(
    const UsdPrim& prim) {
  const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) return nullptr;

  static const char* kPreferredInputs[] = {
      "inputs:in", "inputs:in1", "inputs:dispScalar", "inputs:inputRGB",
      "inputs:fg", "inputs:bg"};
  for (const char* preferred : kPreferredInputs) {
    const std::vector<::lightusd::next::Path>* connections =
        spec->connection(preferred);
    if (connections && !connections->empty()) return connections;
  }

  auto is_factor_input = [](const std::string& name) {
    return name == "inputs:mix" || name == "inputs:amount" ||
           name == "inputs:weight" || name == "inputs:factor" ||
           name == "inputs:alpha" || name == "inputs:mask";
  };
  for (const std::string& property : prim.GetPropertyNames()) {
    if (property.rfind("inputs:", 0) != 0 || is_factor_input(property)) {
      continue;
    }
    const std::vector<::lightusd::next::Path>* connections =
        spec->connection(property);
    if (connections && !connections->empty()) return connections;
  }
  return nullptr;
}

bool ResolveConnectedEndpoint(const Stage& stage,
                              const std::string& connection_path,
                              double time_code,
                              std::string* endpoint_path) {
  if (!endpoint_path) return false;
  std::string current = connection_path;
  std::set<std::string> visited;
  for (int depth = 0; depth <= kMaxMtlxConstantDepth; ++depth) {
    if (!visited.insert(current).second) return false;

    std::string prim_path;
    std::string prop_name;
    if (!SplitConnectionPath(current, &prim_path, &prop_name)) return false;
    UsdPrim prim = stage.GetPrimAtPath(prim_path);
    if (!prim.IsValid()) return false;
    if (IsTextureEndpoint(stage, prim, time_code)) {
      *endpoint_path = current;
      return true;
    }

    const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
    const std::vector<::lightusd::next::Path>* connections =
        spec ? spec->connection(prop_name) : nullptr;
    if (!connections || connections->empty()) {
      connections = PrimaryDataInputConnection(prim);
    }
    if (!connections || connections->empty()) {
      *endpoint_path = current;
      return true;
    }
    current = (*connections)[0].str();
  }
  return false;
}

bool FindConnectedUtilityScalar(const Stage& stage, const UsdPrim& shader,
                                const std::string& shader_input,
                                const std::string& node_id_prefix,
                                const std::string& node_input,
                                double time_code, float* out) {
  if (!out || !shader.IsValid()) return false;
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  const std::string property = "inputs:" + shader_input;
  if (!eval.HasConnection(shader, property)) return false;
  std::string current = eval.GetConnectionPath(shader, property);
  std::set<std::string> visited;
  for (int depth = 0; depth <= kMaxMtlxConstantDepth; ++depth) {
    if (!visited.insert(current).second) return false;
    std::string prim_path;
    std::string prop_name;
    if (!SplitConnectionPath(current, &prim_path, &prop_name)) return false;
    const UsdPrim node = stage.GetPrimAtPath(prim_path);
    if (!node.IsValid()) return false;
    std::string id;
    GetToken(node, "info:id", &id);
    if (id.rfind(node_id_prefix, 0) == 0) {
      if (std::optional<float> value =
              eval.EvalFloat(node, "inputs:" + node_input)) {
        *out = *value;
        return true;
      }
      return false;
    }
    const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec();
    const std::vector<::lightusd::next::Path>* connections =
        spec ? spec->connection(prop_name) : nullptr;
    if (!connections || connections->empty()) {
      connections = PrimaryDataInputConnection(node);
    }
    if (!connections || connections->empty()) return false;
    current = (*connections)[0].str();
  }
  return false;
}

bool ResolveConnectedValue(const Stage& stage,
                           const std::string& connection_path,
                           double time_code,
                           Value* out) {
  if (!out) return false;

  std::string endpoint;
  if (!ResolveConnectedEndpoint(stage, connection_path, time_code, &endpoint)) {
    return false;
  }
  std::string prim_path;
  std::string prop_name;
  if (!SplitConnectionPath(endpoint, &prim_path, &prop_name)) return false;

  UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;

  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  ::lightusd::next::EvalOptions opts = eval.GetOptions();
  opts.follow_connections = false;
  ::lightusd::next::EvalResult result = eval.EvalWith(prim, prop_name, opts);
  if (result.success) {
    *out = std::move(result.value);
    return true;
  }

  return false;
}

RenderTexture::Channel ChannelFromConnection(
    const std::string& connection_path, const UsdPrim& texture_prim) {
  size_t pos = connection_path.find(".outputs:");
  if (pos == std::string::npos) {
    return RenderTexture::Channel::RGBA;
  }

  const std::string channel = connection_path.substr(pos + 9);
  if (channel == "r" || channel == "x") return RenderTexture::Channel::R;
  if (channel == "g" || channel == "y") return RenderTexture::Channel::G;
  if (channel == "b" || channel == "z") return RenderTexture::Channel::B;
  if (channel == "a" || channel == "w") return RenderTexture::Channel::A;
  if (channel == "rgb" || channel == "xyz") return RenderTexture::Channel::RGB;

  // MaterialX image nodes conventionally expose a generic `outputs:out`.
  // Recover its scalar/vector shape from the synthesized output value/type so
  // roughness and metallic maps sample R while color/normal maps sample RGB.
  if (channel == "out" && texture_prim.IsValid()) {
    std::string type;
    if (const Value* value = texture_prim.GetPropertyValue(kIdOutputsOut())) {
      if (const std::string* token = value->as_token()) type = *token;
      else if (const std::string* str = value->as_string()) type = *str;
    }
    if (type.empty()) {
      if (const ::lightusd::next::PrimSpec* spec =
              texture_prim.GetPrimSpec()) {
        if (const std::string* declared =
                spec->property_type_name("outputs:out")) {
          type = *declared;
        }
      }
    }
    if (type == "float" || type == "integer" || type == "boolean") {
      return RenderTexture::Channel::R;
    }
    if (type == "color3" || type == "color3f" || type == "vector3" ||
        type == "vector3f") {
      return RenderTexture::Channel::RGB;
    }
  }
  return RenderTexture::Channel::RGBA;
}

WrapMode ParseWrapMode(const std::string& token) {
  // UsdUVTexture uses repeat/clamp/mirror/black, while MaterialX image nodes
  // call the equivalent modes periodic/clamp/mirror/constant. Keep the
  // translation at the RenderTexture boundary so every backend sees one
  // canonical enum.
  if (token == "repeat" || token == "periodic") return WrapMode::Repeat;
  if (token == "clamp") return WrapMode::Clamp;
  if (token == "mirror") return WrapMode::Mirror;
  if (token == "black" || token == "constant") return WrapMode::Black;
  // UsdUVTexture's wrapS/wrapT fallback is "useMetadata"; with no texture
  // metadata the effective mode is clamp-to-edge (legacy tydra behavior) —
  // NOT repeat, which visibly tiles textures authored to clamp.
  return WrapMode::Clamp;
}

ColorSpace ParseColorSpace(const std::string& token) {
  if (token == "raw") return ColorSpace::Raw;
  if (token == "linear" || token == "Linear" || token == "lin_srgb" ||
      token == "lin_rec709" || token == "scene-linear Rec.709-sRGB") {
    return ColorSpace::Linear;
  }
  if (token == "sRGB" || token == "srgb" || token == "srgb_texture") {
    return ColorSpace::sRGB;
  }
  if (token == "acescg" || token == "ACEScg") return ColorSpace::ACEScg;
  if (token == "rec709" || token == "Rec709") return ColorSpace::Rec709;
  if (token == "rec2020" || token == "Rec2020" ||
      token == "lin_rec2020") return ColorSpace::Rec2020;
  if (token == "displayP3" || token == "DisplayP3" ||
      token == "lin_displayp3" || token == "srgb_displayp3") {
    return ColorSpace::DisplayP3;
  }
  return ColorSpace::Unknown;
}

bool IsColorShaderInput(const UsdPrim& prim, const std::string& attr_name,
                        const std::string& param_name) {
  if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
    if (const std::string* type = spec->property_type_name(attr_name)) {
      if (type->rfind("color3", 0) == 0 || type->rfind("color4", 0) == 0) {
        return true;
      }
    }
  }
  static const std::set<std::string> kColorInputs = {
      "diffuseColor", "emissiveColor", "specularColor", "base_color",
      "baseColor", "specular_color", "transmission_color",
      "subsurface_color", "sheen_color", "coat_color", "emission_color"};
  return kColorInputs.count(param_name) != 0;
}

bool MaterialXConfiguredColorSpace(const UsdPrim& prim, std::string* out) {
  if (!out || !prim.IsValid()) return false;
  std::string id;
  if (!GetToken(prim, "info:id", &id) ||
      (id.rfind("ND_", 0) != 0 && id != "image" &&
       id != "tiledimage" && id != "open_pbr_surface" &&
       id != "standard_surface")) {
    return false;
  }
  for (UsdPrim current = prim; current.IsValid(); current = current.GetParent()) {
    const Value* value =
        current.GetPropertyValue("config:mtlx:colorspace");
    if (!value) continue;
    const std::string* token = value->as_token();
    if (!token) token = value->as_string();
    if (token && !token->empty()) {
      *out = ::lightusd::color::CanonicalizeToken(*token);
      return true;
    }
  }
  return false;
}

bool ResolveConnectedColorSource(const Stage& stage,
                                 const std::string& connection_path,
                                 double time_code, UsdPrim* source_prim,
                                 std::string* source_property) {
  if (!source_prim || !source_property) return false;
  std::string endpoint;
  if (!ResolveConnectedEndpoint(stage, connection_path, time_code, &endpoint)) {
    return false;
  }
  std::string prim_path;
  std::string property;
  if (!SplitConnectionPath(endpoint, &prim_path, &property)) return false;
  UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;

  // Prefer metadata on the resolved output itself. MaterialX constant and
  // utility nodes usually put it on their value/data input instead, so scan
  // those inputs before falling back to the terminal shader attribute.
  if (const ::lightusd::next::PropMeta* meta =
          prim.GetPropertyMeta(property)) {
    if ((meta->authored & ::lightusd::next::PropMeta::kColorSpace) != 0u) {
      *source_prim = prim;
      *source_property = property;
      return true;
    }
  }
  std::string value_input;
  for (const std::string& candidate : prim.GetPropertyNames()) {
    if (candidate.rfind("inputs:", 0) != 0) continue;
    if (value_input.empty() || candidate == "inputs:value" ||
        candidate == "inputs:in") {
      value_input = candidate;
    }
    if (const ::lightusd::next::PropMeta* meta =
            prim.GetPropertyMeta(candidate)) {
      if ((meta->authored & ::lightusd::next::PropMeta::kColorSpace) != 0u) {
        *source_prim = prim;
        *source_property = candidate;
        return true;
      }
    }
  }
  if (!value_input.empty()) {
    *source_prim = prim;
    *source_property = value_input;
    return true;
  }
  *source_prim = prim;
  *source_property = property;
  return true;
}

void ConvertShaderColorToWorking(const UsdPrim& prim,
                                 const std::string& attr_name,
                                 const std::string& param_name,
                                 const RenderScene* scene,
                                 ShaderParam* param) {
  if (!scene || !param || param->is_texture() ||
      !IsColorShaderInput(prim, attr_name, param_name)) {
    return;
  }
  std::string source;
  bool authored = false;
  if (!::lightusd::next::color_management::ComputeColorSpaceName(
          prim, attr_name, &source, &authored)) {
    return;
  }
  if (!authored) {
    (void)MaterialXConfiguredColorSpace(prim, &source);
  }
  ::lightusd::color::ColorTransform transform;
  if (!::lightusd::next::color_management::BuildColorTransform(
          prim, source, scene->working_color_space, &transform)) {
    return;
  }
  float rgb[3] = {param->value.x, param->value.y, param->value.z};
  ::lightusd::color::TransformRGB(transform, rgb);
  param->value.x = rgb[0];
  param->value.y = rgb[1];
  param->value.z = rgb[2];
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

bool ValueToFloat4(const Value& value, Float4* out) {
  if (!out || value.is_empty() || value.is_array()) return false;

  if (const float* v = value.as_float()) {
    *out = Float4(*v, 0.0f, 0.0f, 0.0f);
    return true;
  }
  if (const double* v = value.as_double()) {
    *out = Float4(static_cast<float>(*v), 0.0f, 0.0f, 0.0f);
    return true;
  }
  if (const float* v = value.as_float3()) {
    *out = Float4(v[0], v[1], v[2], 0.0f);
    return true;
  }
  if (const double* v = value.as_double3()) {
    *out = Float4(static_cast<float>(v[0]), static_cast<float>(v[1]),
                  static_cast<float>(v[2]), 0.0f);
    return true;
  }
  if (const float* v = value.as_float4()) {
    *out = Float4(v[0], v[1], v[2], v[3]);
    return true;
  }
  if (const double* v = value.as_double4()) {
    *out = Float4(static_cast<float>(v[0]), static_cast<float>(v[1]),
                  static_cast<float>(v[2]), static_cast<float>(v[3]));
    return true;
  }
  // Authored half-precision scalars (half3 rotate/scale, quath orient, ...)
  // store raw half-bit lanes; the converting reads widen them.
  float h[4];
  if (value.to_float3(h)) {
    *out = Float4(h[0], h[1], h[2], 0.0f);
    return true;
  }
  if (value.to_float4(h)) {
    *out = Float4(h[0], h[1], h[2], h[3]);
    return true;
  }
  if (value.to_float(h)) {
    *out = Float4(h[0], 0.0f, 0.0f, 0.0f);
    return true;
  }
  return false;
}

// Closed-form Euler-degrees -> quaternion (xyzw) for all six USD rotation
// orders (rotateXYZ means apply X first: Q = Qz * Qy * Qx). Ported from the
// legacy tydra converter so Rotation channels always carry quaternions.
Float4 EulerDegreesToQuatXYZW(float xdeg, float ydeg, float zdeg,
                              const std::string& order) {
  const double kHalfDegToRad = 3.14159265358979323846 / 360.0;
  const float sx = static_cast<float>(std::sin(double(xdeg) * kHalfDegToRad));
  const float cx = static_cast<float>(std::cos(double(xdeg) * kHalfDegToRad));
  const float sy = static_cast<float>(std::sin(double(ydeg) * kHalfDegToRad));
  const float cy = static_cast<float>(std::cos(double(ydeg) * kHalfDegToRad));
  const float sz = static_cast<float>(std::sin(double(zdeg) * kHalfDegToRad));
  const float cz = static_cast<float>(std::cos(double(zdeg) * kHalfDegToRad));

  if (order == "XZY") {  // Q = Qy * Qz * Qx
    return Float4(cy*cz*sx + sy*sz*cx, cy*sz*sx + sy*cz*cx,
                  cy*sz*cx - sy*cz*sx, cy*cz*cx - sy*sz*sx);
  }
  if (order == "YXZ") {  // Q = Qz * Qx * Qy
    return Float4(cz*sx*cy - sz*cx*sy, cz*cx*sy + sz*sx*cy,
                  cz*sx*sy + sz*cx*cy, cz*cx*cy - sz*sx*sy);
  }
  if (order == "YZX") {  // Q = Qx * Qz * Qy
    return Float4(sx*cz*cy - cx*sz*sy, cx*cz*sy - sx*sz*cy,
                  cx*sz*cy + sx*cz*sy, cx*cz*cy + sx*sz*sy);
  }
  if (order == "ZXY") {  // Q = Qy * Qx * Qz
    return Float4(cy*sx*cz + sy*cx*sz, sy*cx*cz - cy*sx*sz,
                  cy*cx*sz - sy*sx*cz, cy*cx*cz + sy*sx*sz);
  }
  if (order == "ZYX") {  // Q = Qx * Qy * Qz
    return Float4(cx*sy*sz + sx*cy*cz, cx*sy*cz - sx*cy*sz,
                  cx*cy*sz + sx*sy*cz, cx*cy*cz - sx*sy*sz);
  }
  // XYZ (and fallback): Q = Qz * Qy * Qx
  return Float4(cz*cy*sx - sz*sy*cx, cz*sy*cx + sz*cy*sx,
                sz*cy*cx - cz*sy*sx, cz*cy*cx + sz*sy*sx);
}

// Extracts the axis order ("XYZ", "ZYX", ...) from an xformOp:rotate<ORDER>
// property name. Returns false for single-axis rotateX/Y/Z and non-rotate ops.
}  // namespace

bool RenderSceneConverter::ExtractPreviewSurface(const Stage& stage,
                                                 const UsdPrim& shader_prim,
                                                 PreviewSurfaceShader* out,
                                                 RenderScene* scene) {
  if (!out || !::lightusd::next::IsShader(shader_prim)) return false;

  // AttributeEval returns a successful zero-valued result for an unauthored
  // shader input.  Those zeros are not the UsdPreviewSurface schema defaults
  // (most importantly, unauthored opacity is 1).  Preserve the initialized
  // PreviewSurfaceShader defaults unless the input is actually authored or
  // connected.  This also keeps the 0.5 roughness, 1.5 IOR and unit occlusion
  // fallbacks instead of silently replacing them with zero.
  ::lightusd::next::AttributeEval authored_eval(&stage);
  authored_eval.SetTime(config_.time_code);
  auto extract_authored = [&](const char* name, ShaderParam* param) {
    const std::string attr_name = std::string("inputs:") + name;
    if (GetAttribute(shader_prim, attr_name) == nullptr &&
        !authored_eval.HasConnection(shader_prim, attr_name)) {
      return;
    }
    (void)ExtractShaderParam(stage, shader_prim, name, param, scene);
  };

  extract_authored("diffuseColor", &out->diffuse_color);
  extract_authored("emissiveColor", &out->emissive_color);
  extract_authored("specularColor", &out->specular_color);
  extract_authored("metallic", &out->metallic);
  extract_authored("roughness", &out->roughness);
  extract_authored("clearcoat", &out->clearcoat);
  extract_authored("clearcoatRoughness", &out->clearcoat_roughness);
  extract_authored("opacity", &out->opacity);
  extract_authored("opacityThreshold", &out->opacity_threshold);
  extract_authored("ior", &out->ior);
  extract_authored("normal", &out->normal);
  extract_authored("displacement", &out->displacement);
  extract_authored("occlusion", &out->occlusion);

  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(config_.time_code);
  if (std::optional<int32_t> use_spec =
          eval.EvalInt(shader_prim, "inputs:useSpecularWorkflow")) {
    out->use_specular_workflow = (*use_spec != 0);
  }

  return true;
}

// MaterialX standard_surface -> OpenPBR field mapping (mirrors legacy
// ConvertMtlxStandardSurfaceToOpenPBRSurface). ExtractShaderParam follows
// connections, so textured inputs (ND_image chains that resolve to a file)
// come through as textures.


namespace {
struct TextureNodeData {
  std::string file;
  // `inputs:file` customData { asset ktx2 = @...@ } — the legacy-safe compressed
  // companion hint (doc/texcomp.md). Empty when unauthored.
  std::string ktx2_hint;
  std::string wrap_s = "useMetadata";
  std::string wrap_t = "useMetadata";
  float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  std::string source_color_space = "auto";
  // From the inputs:st chain (UsdTransform2d -> UsdPrimvarReader_float2):
  std::string uv_primvar;               // varname of the primvar reader
  float uv_translation[2] = {0.0f, 0.0f};
  float uv_rotation = 0.0f;             // degrees (UsdTransform2d convention)
  float uv_scale[2] = {1.0f, 1.0f};
};

// Trace a UsdUVTexture's inputs:st connection chain: UsdTransform2d nodes
// accumulate the UV transform (chained via their inputs:in); a
// UsdPrimvarReader_* terminates the chain and names the UV set.
static bool GetFloat2Local(const UsdPrim& prim, const std::string& name,
                           float* out2) {
  const ::lightusd::next::Value* v = GetAttribute(prim, name);
  if (!v) return false;
  const float* f = v->as_float2();
  if (!f) return false;
  out2[0] = f[0];
  out2[1] = f[1];
  return true;
}

void TraceTextureStChain(const Stage& stage, const UsdPrim& texture_prim,
                         TextureNodeData* out) {
  UsdPrim cur = texture_prim;
  std::string prop = "inputs:st";

  // Chained UsdTransform2d nodes COMPOSE (each node applies
  // uv' = R(rotation) * (scale * uv) + translation to its input). Accumulate
  // the affine composition walking from the texture (outermost) towards the
  // primvar reader (innermost): F_total(uv) = F_outer(F_inner(uv)).
  // A single node keeps its authored values verbatim (no decomposition).
  // Note: animated or connected translation/rotation/scale inputs are NOT
  // evaluated here (only directly-authored values are read).
  int transform_nodes = 0;
  double A[2][2] = {{1.0, 0.0}, {0.0, 1.0}};  // accumulated linear part
  double T[2] = {0.0, 0.0};                   // accumulated offset

  for (int hop = 0; hop < 4 && cur.IsValid(); ++hop) {
    const ::lightusd::next::PrimSpec* spec = cur.GetPrimSpec();
    const std::vector<::lightusd::next::Path>* conns =
        spec ? spec->connection(prop) : nullptr;
    if (!conns || conns->empty()) break;
    const std::string next_path = SourcePrimPathFromConnection((*conns)[0].str());
    UsdPrim np = stage.GetPrimAtPath(next_path);
    if (!np.IsValid()) break;
    std::string id;
    GetToken(np, "info:id", &id);
    if (id == "UsdTransform2d") {
      float tr[2] = {0.0f, 0.0f};
      GetFloat2Local(np, "inputs:translation", tr);
      float rot = 0.0f;
      GetFloat(np, "inputs:rotation", &rot);
      float sc[2] = {1.0f, 1.0f};
      GetFloat2Local(np, "inputs:scale", sc);

      if (transform_nodes == 0) {
        // First (outermost) node: keep the raw authored values so a single
        // Transform2d round-trips exactly.
        out->uv_translation[0] = tr[0];
        out->uv_translation[1] = tr[1];
        out->uv_rotation = rot;
        out->uv_scale[0] = sc[0];
        out->uv_scale[1] = sc[1];
      }

      // Local affine L(uv) = R * diag(sc) * uv + tr.
      const double rad = double(rot) * 3.14159265358979323846 / 180.0;
      const double c = std::cos(rad), s = std::sin(rad);
      const double L[2][2] = {{c * sc[0], -s * sc[1]},
                              {s * sc[0], c * sc[1]}};
      // Accumulated-so-far F is applied AFTER this (deeper) node:
      // F_new(uv) = F(L(uv)) => A_new = A*L, T_new = A*t_L + T.
      const double A00 = A[0][0] * L[0][0] + A[0][1] * L[1][0];
      const double A01 = A[0][0] * L[0][1] + A[0][1] * L[1][1];
      const double A10 = A[1][0] * L[0][0] + A[1][1] * L[1][0];
      const double A11 = A[1][0] * L[0][1] + A[1][1] * L[1][1];
      T[0] += A[0][0] * tr[0] + A[0][1] * tr[1];
      T[1] += A[1][0] * tr[0] + A[1][1] * tr[1];
      A[0][0] = A00; A[0][1] = A01; A[1][0] = A10; A[1][1] = A11;

      ++transform_nodes;
      cur = np;
      prop = "inputs:in";
      continue;
    }
    if (id.rfind("UsdPrimvarReader", 0) == 0) {
      out->uv_primvar = ::lightusd::next::GetPrimvarReaderVarname(stage, np);
      break;
    }
    break;
  }

  if (transform_nodes > 1) {
    // Decompose the composite affine back into the translate/rotate/scale
    // model RenderTexture carries (uv' = R * diag(scale) * uv + offset).
    // Composition of non-uniform scales and rotations can introduce shear,
    // which this model cannot represent; the QR-style decomposition below
    // drops it (best effort).
    out->uv_translation[0] = static_cast<float>(T[0]);
    out->uv_translation[1] = static_cast<float>(T[1]);
    const double sx = std::sqrt(A[0][0] * A[0][0] + A[1][0] * A[1][0]);
    if (sx > 1.0e-12) {
      const double c = A[0][0] / sx;
      const double s = A[1][0] / sx;
      const double sy = -s * A[0][1] + c * A[1][1];
      out->uv_rotation = static_cast<float>(
          std::atan2(s, c) * 180.0 / 3.14159265358979323846);
      out->uv_scale[0] = static_cast<float>(sx);
      out->uv_scale[1] = static_cast<float>(sy);
    } else {
      out->uv_rotation = 0.0f;
      out->uv_scale[0] = static_cast<float>(A[0][0]);
      out->uv_scale[1] = static_cast<float>(A[1][1]);
    }
  }
}

bool ExtractTextureNodeData(const Stage& stage,
                            const UsdPrim& texture_prim,
                            double time_code,
                            TextureNodeData* out) {
  if (!out || !texture_prim.IsValid()) return false;

  // Apply metadata from the property that actually authors the asset. Ptex
  // networks frequently forward a shader input through a material interface,
  // e.g. `inputs:file.connect = </Mat.inputs:surfaceMap>`, so that property is
  // not necessarily on the texture shader itself.
  auto apply_file_metadata = [out](const UsdPrim& owner,
                                   const std::string& property) {
    const ::lightusd::next::PropMeta* pm = owner.GetPropertyMeta(property);
    if (!pm) return;
    if (!pm->colorSpace.empty()) out->source_color_space = pm->colorSpace;
    if (const ::lightusd::next::Dict* cd = pm->customData.as_dictionary()) {
      if (const ::lightusd::next::Value* v = cd->find("ktx2")) {
        if (const std::string* s = v->as_asset_path()) {
          out->ktx2_hint = *s;
        } else if (const std::string* t = v->as_string()) {
          out->ktx2_hint = *t;
        }
      }
    }
  };

  ::lightusd::next::UVTextureData uv;
  if (::lightusd::next::GetUVTextureData(stage, texture_prim, &uv, time_code)) {
    out->file = uv.file;
    out->wrap_s = uv.wrap_s;
    out->wrap_t = uv.wrap_t;
    out->source_color_space = uv.source_color_space;
    std::memcpy(out->scale, uv.scale, sizeof(out->scale));
    std::memcpy(out->bias, uv.bias, sizeof(out->bias));
    apply_file_metadata(texture_prim, "inputs:file");
    TraceTextureStChain(stage, texture_prim, out);
    return !out->file.empty();
  }

  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);

  std::string shader_id;
  GetToken(texture_prim, "info:id", &shader_id);
  const bool materialx_image =
      shader_id == "image" || shader_id == "tiledimage" ||
      shader_id.rfind("ND_image_", 0) == 0 ||
      shader_id.rfind("ND_tiledimage_", 0) == 0;
  if (materialx_image) {
    // MaterialX image/tiledimage defaults are periodic in both directions.
    // Do not inherit UsdUVTexture's useMetadata -> clamp fallback: production
    // MaterialX assets commonly use coordinates outside [0,1] and rely on the
    // nodedef default even when no address-mode input is authored.
    out->wrap_s = "periodic";
    out->wrap_t = "periodic";
  }

  UsdPrim fileOwner;
  std::string fileAttribute;
  auto resolve_asset_input = [&](const std::string& initial,
                                 UsdPrim* resolved_owner,
                                 std::string* resolved_property)
      -> std::optional<std::string> {
    UsdPrim owner = texture_prim;
    std::string property = initial;
    std::set<std::string> visited;
    for (int depth = 0; depth <= kMaxMtlxConstantDepth && owner.IsValid();
         ++depth) {
      const std::string visit_key = owner.GetPath().str() + "." + property;
      if (!visited.insert(visit_key).second) break;

      std::optional<std::string> value = eval.EvalAssetPath(owner, property);
      if (!value) value = eval.EvalString(owner, property);
      if (value && !value->empty()) {
        *resolved_owner = owner;
        *resolved_property = property;
        return value;
      }
      if (!eval.HasConnection(owner, property)) break;

      std::string prim_path;
      std::string next_property;
      if (!SplitConnectionPath(eval.GetConnectionPath(owner, property),
                               &prim_path, &next_property)) {
        break;
      }
      owner = stage.GetPrimAtPath(prim_path);
      property = std::move(next_property);
    }
    return std::nullopt;
  };

  std::optional<std::string> file =
      resolve_asset_input("inputs:file", &fileOwner, &fileAttribute);
  if (!file) {
    file = resolve_asset_input("inputs:filename", &fileOwner, &fileAttribute);
  }
  if (!file || file->empty()) return false;
  out->file = *file;

  if (std::optional<std::string> wrap_s = eval.EvalToken(texture_prim, "inputs:wrapS")) {
    out->wrap_s = *wrap_s;
  }
  if (std::optional<std::string> wrap_t = eval.EvalToken(texture_prim, "inputs:wrapT")) {
    out->wrap_t = *wrap_t;
  }
  if (materialx_image) {
    auto address_mode = [&](const char* name) -> std::optional<std::string> {
      if (std::optional<std::string> value =
              eval.EvalToken(texture_prim, name)) {
        return value;
      }
      return eval.EvalString(texture_prim, name);
    };
    if (std::optional<std::string> mode =
            address_mode("inputs:uaddressmode")) {
      out->wrap_s = *mode;
    }
    if (std::optional<std::string> mode =
            address_mode("inputs:vaddressmode")) {
      out->wrap_t = *mode;
    }
  }
  float scale[4];
  if (eval.EvalFloat4(texture_prim, "inputs:scale", scale)) {
    std::memcpy(out->scale, scale, sizeof(out->scale));
  }
  float bias[4];
  if (eval.EvalFloat4(texture_prim, "inputs:bias", bias)) {
    std::memcpy(out->bias, bias, sizeof(out->bias));
  }
  if (std::optional<std::string> cs = eval.EvalToken(texture_prim, "inputs:sourceColorSpace")) {
    out->source_color_space = *cs;
  }
  apply_file_metadata(fileOwner, fileAttribute);
  TraceTextureStChain(stage, texture_prim, out);

  return true;
}

// MaterialX Autodesk standard_surface (usdMtlx flatten pattern).
}  // namespace

bool RenderSceneConverter::ExtractStandardSurfaceAsOpenPBR(
    const Stage& stage, const UsdPrim& shader_prim, OpenPBRSurfaceShader* out,
    RenderScene* scene) {
  if (!out || !::lightusd::next::IsShader(shader_prim)) return false;

  // Base layer
  // AttributeEval can return a successful zero-valued result for an
  // unauthored MaterialX input.  That is not the Standard Surface default:
  // `base` defaults to 1. Preserve the initialized schema default unless the
  // input is explicitly authored or connected.
  const bool base_authored =
      GetAttribute(shader_prim, "inputs:base") != nullptr ||
      ::lightusd::next::AttributeEval(&stage).HasConnection(
          shader_prim, "inputs:base");
  if (base_authored) {
    ExtractShaderParam(stage, shader_prim, "base", &out->base_weight, scene);
  }
  ExtractShaderParam(stage, shader_prim, "base_color", &out->base_color, scene);
  ExtractShaderParam(stage, shader_prim, "diffuse_roughness",
                     &out->base_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "metalness", &out->base_metalness,
                     scene);

  // Specular layer
  ExtractShaderParam(stage, shader_prim, "specular", &out->specular_weight,
                     scene);
  ExtractShaderParam(stage, shader_prim, "specular_color",
                     &out->specular_color, scene);
  ExtractShaderParam(stage, shader_prim, "specular_roughness",
                     &out->specular_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "specular_IOR", &out->specular_ior,
                     scene);
  ExtractShaderParam(stage, shader_prim, "specular_anisotropy",
                     &out->specular_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "specular_roughness_anisotropy",
                     &out->specular_roughness_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "specular_rotation",
                     &out->specular_rotation, scene);

  // Transmission
  ExtractShaderParam(stage, shader_prim, "transmission",
                     &out->transmission_weight, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_color",
                     &out->transmission_color, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_depth",
                     &out->transmission_depth, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_dispersion",
                     &out->transmission_dispersion, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_dispersion_scale",
                     &out->transmission_dispersion_scale, scene);

  // Subsurface
  ExtractShaderParam(stage, shader_prim, "subsurface",
                     &out->subsurface_weight, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface_color",
                     &out->subsurface_color, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface_radius",
                     &out->subsurface_radius, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface_scale",
                     &out->subsurface_scale, scene);

  // Sheen
  ExtractShaderParam(stage, shader_prim, "sheen", &out->sheen_weight, scene);
  ExtractShaderParam(stage, shader_prim, "sheen_color", &out->sheen_color,
                     scene);
  ExtractShaderParam(stage, shader_prim, "sheen_roughness",
                     &out->sheen_roughness, scene);

  // Coat
  ExtractShaderParam(stage, shader_prim, "coat", &out->coat_weight, scene);
  ExtractShaderParam(stage, shader_prim, "coat_color", &out->coat_color,
                     scene);
  ExtractShaderParam(stage, shader_prim, "coat_roughness",
                     &out->coat_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "coat_IOR", &out->coat_ior, scene);
  ExtractShaderParam(stage, shader_prim, "coat_normal", &out->coat_normal,
                     scene);

  // Emission
  ExtractShaderParam(stage, shader_prim, "emission", &out->emission_luminance,
                     scene);
  ExtractShaderParam(stage, shader_prim, "emission_color",
                     &out->emission_color, scene);

  // Geometry
  ExtractShaderParam(stage, shader_prim, "normal", &out->normal, scene);
  ExtractShaderParam(stage, shader_prim, "opacity", &out->opacity, scene);
  ExtractShaderParam(stage, shader_prim, "displacement", &out->displacement,
                     scene);

  return true;
}

bool RenderSceneConverter::ExtractOpenPBRSurface(const Stage& stage,
                                                 const UsdPrim& shader_prim,
                                                 OpenPBRSurfaceShader* out,
                                                 RenderScene* scene) {
  if (!out || !::lightusd::next::IsShader(shader_prim)) return false;

  ExtractShaderParam(stage, shader_prim, "base_weight", &out->base_weight, scene);
  ExtractShaderParam(stage, shader_prim, "base_color", &out->base_color, scene);
  ExtractShaderParam(stage, shader_prim, "baseColor", &out->base_color, scene);
  ExtractShaderParam(stage, shader_prim, "base_roughness", &out->base_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "base_diffuse_roughness",
                     &out->base_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "roughness", &out->base_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "base_metalness", &out->base_metalness, scene);
  ExtractShaderParam(stage, shader_prim, "metalness", &out->base_metalness, scene);

  ExtractShaderParam(stage, shader_prim, "specular_weight", &out->specular_weight, scene);
  ExtractShaderParam(stage, shader_prim, "specular_color", &out->specular_color, scene);
  ExtractShaderParam(stage, shader_prim, "specular_roughness",
                     &out->specular_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "specular_ior", &out->specular_ior, scene);
  ExtractShaderParam(stage, shader_prim, "specular_anisotropy",
                     &out->specular_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "specular_roughness_anisotropy",
                     &out->specular_roughness_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "specular_rotation",
                     &out->specular_rotation, scene);

  ExtractShaderParam(stage, shader_prim, "transmission_weight",
                     &out->transmission_weight, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_color",
                     &out->transmission_color, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_depth",
                     &out->transmission_depth, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_dispersion",
                     &out->transmission_dispersion, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_dispersion_scale",
                     &out->transmission_dispersion_scale, scene);

  ExtractShaderParam(stage, shader_prim, "subsurface_weight",
                     &out->subsurface_weight, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface_color",
                     &out->subsurface_color, scene);
  // OpenPBR represents the diffusion distance as a scalar radius multiplied
  // by a per-channel radius-scale color. Keep those inputs independent: all
  // transport backends already multiply the RGB radius by the scalar scale,
  // and retaining both lanes also preserves graph-driven radius textures.
  ShaderParam subsurfaceRadius;
  SetParamFloat(&subsurfaceRadius, 1.0f);
  ShaderParam subsurfaceRadiusScale;
  SetParamFloat3(&subsurfaceRadiusScale, 1.0f, 1.0f, 1.0f);
  const bool hasSubsurfaceRadius = ExtractShaderParam(
      stage, shader_prim, "subsurface_radius", &subsurfaceRadius, scene);
  const bool hasSubsurfaceRadiusScale = ExtractShaderParam(
      stage, shader_prim, "subsurface_radius_scale",
      &subsurfaceRadiusScale, scene);
  if (hasSubsurfaceRadius || hasSubsurfaceRadiusScale) {
    out->subsurface_radius = subsurfaceRadiusScale;
    out->subsurface_scale = subsurfaceRadius;
  }

  ExtractShaderParam(stage, shader_prim, "coat_weight", &out->coat_weight, scene);
  ExtractShaderParam(stage, shader_prim, "coat_color", &out->coat_color, scene);
  ExtractShaderParam(stage, shader_prim, "coat_roughness", &out->coat_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "coat_ior", &out->coat_ior, scene);
  ExtractShaderParam(stage, shader_prim, "coat_anisotropy",
                     &out->coat_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "coat_roughness_anisotropy",
                     &out->coat_roughness_anisotropy, scene);
  if (!ExtractShaderParam(stage, shader_prim, "geometry_coat_normal",
                          &out->coat_normal, scene)) {
    ExtractShaderParam(stage, shader_prim, "coat_normal", &out->coat_normal,
                       scene);
  }

  // OpenPBR renamed the grazing cloth lobe to fuzz. Retain the older sheen
  // spellings for Standard Surface and early OpenPBR files, but prefer the
  // current names when both are authored.
  if (!ExtractShaderParam(stage, shader_prim, "fuzz_weight",
                          &out->sheen_weight, scene)) {
    ExtractShaderParam(stage, shader_prim, "sheen_weight",
                       &out->sheen_weight, scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "fuzz_color",
                          &out->sheen_color, scene)) {
    ExtractShaderParam(stage, shader_prim, "sheen_color",
                       &out->sheen_color, scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "fuzz_roughness",
                          &out->sheen_roughness, scene)) {
    ExtractShaderParam(stage, shader_prim, "sheen_roughness",
                       &out->sheen_roughness, scene);
  }
  ExtractShaderParam(stage, shader_prim, "thin_film_weight",
                     &out->thin_film_weight, scene);
  ExtractShaderParam(stage, shader_prim, "thin_film_thickness",
                     &out->thin_film_thickness, scene);
  ExtractShaderParam(stage, shader_prim, "thin_film_ior",
                     &out->thin_film_ior, scene);

  ExtractShaderParam(stage, shader_prim, "emission_luminance",
                     &out->emission_luminance, scene);
  ExtractShaderParam(stage, shader_prim, "emission_color", &out->emission_color, scene);

  // OpenPBR prefixes geometry inputs. Accept the older short aliases as a
  // fallback for exporters that authored pre-1.39 spellings.
  if (!ExtractShaderParam(stage, shader_prim, "geometry_opacity",
                          &out->opacity, scene)) {
    ExtractShaderParam(stage, shader_prim, "opacity", &out->opacity, scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "geometry_thin_walled",
                          &out->thin_walled, scene)) {
    ExtractShaderParam(stage, shader_prim, "thin_walled",
                       &out->thin_walled, scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "geometry_normal",
                          &out->normal, scene)) {
    ExtractShaderParam(stage, shader_prim, "normal", &out->normal, scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "geometry_tangent",
                          &out->tangent, scene)) {
    ExtractShaderParam(stage, shader_prim, "tangent", &out->tangent, scene);
  }
  (void)FindConnectedUtilityScalar(
      stage, shader_prim, "geometry_normal", "ND_normalmap_", "scale",
      config_.time_code, &out->normal_map_scale);
  (void)FindConnectedUtilityScalar(
      stage, shader_prim, "geometry_tangent", "ND_rotate3d_", "amount",
      config_.time_code, &out->tangent_rotation);

  return true;
}

bool RenderSceneConverter::ExtractShaderParam(const Stage& stage,
                                              const UsdPrim& shader_prim,
                                              const std::string& param_name,
                                              ShaderParam* out,
                                              RenderScene* scene) {
  // Material interface inputs use the same `inputs:*` namespace and value /
  // connection semantics as Shader inputs. Accept either here so degraded
  // material recovery can preserve constants authored on the Material prim.
  if (!out || !shader_prim.IsValid() ||
      (!::lightusd::next::IsShader(shader_prim) &&
       !::lightusd::next::IsMaterial(shader_prim))) {
    return false;
  }

  const std::string attr_name = "inputs:" + param_name;
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(config_.time_code);

  if (eval.HasConnection(shader_prim, attr_name)) {
    std::string connection_path = eval.GetConnectionPath(shader_prim, attr_name);
    std::string evaluation_space = "lin_rec709_scene";
    UsdPrim evaluation_context = shader_prim;
    std::string evaluation_endpoint;
    if (ResolveConnectedEndpoint(stage, connection_path, config_.time_code,
                                 &evaluation_endpoint)) {
      std::string evaluation_prim_path;
      std::string evaluation_property;
      if (SplitConnectionPath(evaluation_endpoint, &evaluation_prim_path,
                              &evaluation_property)) {
        UsdPrim candidate = stage.GetPrimAtPath(evaluation_prim_path);
        if (candidate.IsValid()) evaluation_context = candidate;
      }
    }
    (void)MaterialXConfiguredColorSpace(evaluation_context,
                                        &evaluation_space);
    MtlxConstantValue evaluated;
    std::set<std::string> visiting;
    if (EvalMtlxConstantConnection(stage, connection_path, config_.time_code,
                                   evaluation_space, &evaluated, &visiting,
                                   0)) {
      if (evaluated.components >= 3) {
        SetParamFloat3(out, evaluated.value[0], evaluated.value[1],
                       evaluated.value[2]);
      } else if (evaluated.components == 2) {
        SetParamFloat4(out, evaluated.value[0], evaluated.value[1], 0.0f,
                       1.0f);
      } else {
        SetParamFloat(out, evaluated.value[0]);
      }
      if (scene && evaluated.components >= 3 && evaluated.color_managed &&
          IsColorShaderInput(shader_prim, attr_name, param_name)) {
        ::lightusd::color::ColorTransform transform;
        if (::lightusd::next::color_management::BuildColorTransform(
                evaluation_context, evaluation_space,
                scene->working_color_space, &transform)) {
          float rgb[3] = {out->value.x, out->value.y, out->value.z};
          ::lightusd::color::TransformRGB(transform, rgb);
          out->value.x = rgb[0];
          out->value.y = rgb[1];
          out->value.z = rgb[2];
        }
      }
      return true;
    }
    // Follow localized NodeGraph outputs and primary utility-node inputs to
    // the terminal image/value. The shared walker is cycle-safe and is also
    // used by ResolveConnectedValue below.
    std::string endpoint;
    if (ResolveConnectedEndpoint(stage, connection_path, config_.time_code,
                                 &endpoint)) {
      connection_path = std::move(endpoint);
    }
    const std::string texture_prim_path = SourcePrimPathFromConnection(connection_path);
    UsdPrim texture_prim = stage.GetPrimAtPath(texture_prim_path);

    TextureNodeData tex_data;
    if (scene && ExtractTextureNodeData(stage, texture_prim, config_.time_code, &tex_data)) {
      if (tex_data.source_color_space.empty() ||
          tex_data.source_color_space == "auto") {
        bool authored = false;
        std::string inherited;
        (void)::lightusd::next::color_management::ComputeColorSpaceName(
            texture_prim, "inputs:file", &inherited, &authored);
        if (authored) {
          tex_data.source_color_space = inherited;
        } else {
          (void)MaterialXConfiguredColorSpace(
              texture_prim, &tex_data.source_color_space);
        }
      }
      const ColorSpace cs = ParseColorSpace(tex_data.source_color_space);
      const ColorSpace image_color_space =
          cs == ColorSpace::Unknown ? ColorSpace::sRGB : cs;
      const std::string resolved =
          ResolveAssetPath(tex_data.file, AssetAnchorOf(texture_prim));
      int32_t image_id =
          FindCachedImageId(scene, resolved, image_color_space);
      if (image_id < 0) {
        TextureImage image;
        image.name = texture_prim.IsValid() ? texture_prim.GetName() : tex_data.file;
        image.resolved_path = resolved;
        image.color_space = image_color_space;
        if (config_.material.load_textures) {
          TextureImage loaded;
          if (LoadTexture(resolved, &loaded)) {
            if (loaded.name.empty()) loaded.name = image.name;
            if (loaded.resolved_path.empty()) loaded.resolved_path = resolved;
            if (!config_.material.custom_texture_loader ||
                loaded.color_space == ColorSpace::Unknown) {
              loaded.color_space = image.color_space;
            }
            image = std::move(loaded);
          } else if (!config_.material.allow_missing_textures) {
            AddWarning("Failed to load texture: " + tex_data.file);
            return false;
          }
        }
        image_id = static_cast<int32_t>(scene->images.size());
        // Key on the RESOLVED path/colorspace we looked up with -- a loaded
        // image may carry different values, and the next lookup uses these.
        scene->images.push_back(std::move(image));
        RememberImageId(scene, resolved, image_color_space, image_id);
      }

      RenderTexture texture;
      texture.name = texture_prim.IsValid() ? texture_prim.GetName() : param_name;
      texture.prim_path = texture_prim_path;
      texture.asset_path = tex_data.file;
      texture.ktx2_hint = tex_data.ktx2_hint;
      texture.wrap_s = ParseWrapMode(tex_data.wrap_s);
      texture.wrap_t = ParseWrapMode(tex_data.wrap_t);
      texture.scale_value = Float4(tex_data.scale[0], tex_data.scale[1],
                                   tex_data.scale[2], tex_data.scale[3]);
      texture.bias = Float4(tex_data.bias[0], tex_data.bias[1],
                            tex_data.bias[2], tex_data.bias[3]);
      texture.image_id = image_id;
      texture.source_color_space = tex_data.source_color_space;
      texture.target_color_space = scene->working_color_space;
      {
        std::string source_space = tex_data.source_color_space;
        if (source_space.empty() || source_space == "auto") {
          source_space = "srgb_rec709_scene";
        }
        ::lightusd::color::ColorTransform source_to_display;
        if (::lightusd::next::color_management::BuildColorTransform(
                texture_prim, source_space, "lin_rec709_scene",
                &source_to_display)) {
          texture.color_transform_valid = true;
          texture.color_transform_bypass = source_to_display.bypass;
          texture.source_color_is_data =
              source_to_display.source.kind ==
              ::lightusd::color::ColorSpaceKind::Data;
          texture.source_gamma = source_to_display.source.gamma;
          texture.source_linear_bias = source_to_display.source.linear_bias;
          std::copy(source_to_display.matrix, source_to_display.matrix + 9,
                    texture.source_to_display_linear);
        }
      }
      texture.output_channel =
          ChannelFromConnection(connection_path, texture_prim);
      // UsdTransform2d on the st chain (rotation is authored in degrees;
      // RenderTexture stores radians).
      texture.offset = Float2(tex_data.uv_translation[0],
                              tex_data.uv_translation[1]);
      texture.scale = Float2(tex_data.uv_scale[0], tex_data.uv_scale[1]);
      texture.rotation = tex_data.uv_rotation * 3.14159265358979323846f / 180.0f;
      texture.uv_primvar = tex_data.uv_primvar;

      out->texture_id = static_cast<int32_t>(scene->textures.size());
      scene->textures.push_back(std::move(texture));
      return true;
    }

    Value connected_value;
    if (ResolveConnectedValue(stage, connection_path, config_.time_code,
                              &connected_value) &&
        ValueToShaderParam(connected_value, out)) {
      UsdPrim color_prim = shader_prim;
      std::string color_property = attr_name;
      (void)ResolveConnectedColorSource(stage, connection_path,
                                        config_.time_code, &color_prim,
                                        &color_property);
      ConvertShaderColorToWorking(color_prim, color_property, param_name,
                                  scene, out);
      return true;
    }
  }

  ::lightusd::next::EvalOptions direct_opts = eval.GetOptions();
  direct_opts.follow_connections = false;
  ::lightusd::next::EvalResult direct =
      eval.EvalWith(shader_prim, attr_name, direct_opts);
  if (direct.success && ValueToShaderParam(direct.value, out)) {
    ConvertShaderColorToWorking(shader_prim, attr_name, param_name, scene,
                                out);
    return true;
  }

  ::lightusd::next::EvalResult followed = eval.Eval(shader_prim, attr_name);
  if (followed.success && ValueToShaderParam(followed.value, out)) {
    ConvertShaderColorToWorking(shader_prim, attr_name, param_name, scene,
                                out);
    return true;
  }

  return false;
}

//
// Light conversion
//



} } }  // namespace lightusd::tydra::next
