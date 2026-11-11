// SPDX-License-Identifier: Apache-2.0
// Shared MaterialX/shader constant-eval + ShaderParam helper definitions.
#include "render-converter-mtlx-eval.hh"
#include "core/safe-arithmetic.hh"
#include "layer/prim-spec.hh"
#include "../core/path-expression-eval.hh"
#include "schema/usd-vol.hh"
#include "schema/usd-geom-camera.hh"
#include "render-converter-internal.hh"
#include "mem-budget.hh"
#include "schema/color-space.hh"
#include "eval/value-clip.hh"
#include "resolver/asset-resolver.hh"
#include "materialx.hh"
#include "layer/asset-anchor.hh"
#include "schema/usdPhysics.hh"
#include "schema/usd-shade.hh"
#include "schema/usd-skel.hh"
#include "types/type-info.hh"
#include "fast-mikktspace.hh"
#include "mikktspace-tangent.hh"
#include "shape-to-mesh.hh"
#include "tangent-quantize.hh"
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

namespace lightusd { namespace tydra {
using ::lightusd::Stage; using ::lightusd::UsdPrim; using ::lightusd::Value;

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

static void MtlxRgbToHsv(const float* rgb, float* hsv) {
  const float maximum = std::max({rgb[0], rgb[1], rgb[2]});
  const float minimum = std::min({rgb[0], rgb[1], rgb[2]});
  const float delta = maximum - minimum;
  float hue = 0.0f;
  if (delta > 0.0f) {
    if (rgb[0] == maximum) hue = (rgb[1] - rgb[2]) / delta;
    else if (rgb[1] == maximum) hue = 2.0f + (rgb[2] - rgb[0]) / delta;
    else hue = 4.0f + (rgb[0] - rgb[1]) / delta;
    hue /= 6.0f;
    if (hue < 0.0f) hue += 1.0f;
  }
  hsv[0] = hue;
  hsv[1] = maximum != 0.0f ? delta / maximum : 0.0f;
  hsv[2] = maximum;
}

static void MtlxHsvToRgb(const float* hsv, float* rgb) {
  if (hsv[1] == 0.0f) {
    rgb[0] = rgb[1] = rgb[2] = hsv[2];
    return;
  }
  float hue = std::fmod(hsv[0], 1.0f);
  if (hue < 0.0f) hue += 1.0f;
  const float sector_position = hue * 6.0f;
  const int sector = static_cast<int>(std::floor(sector_position)) % 6;
  const float fraction = sector_position - std::floor(sector_position);
  const float p = hsv[2] * (1.0f - hsv[1]);
  const float q = hsv[2] * (1.0f - hsv[1] * fraction);
  const float t = hsv[2] * (1.0f - hsv[1] * (1.0f - fraction));
  const float table[6][3] = {{hsv[2], t, p}, {q, hsv[2], p},
                             {p, hsv[2], t}, {p, q, hsv[2]},
                             {t, p, hsv[2]}, {hsv[2], p, q}};
  for (int i = 0; i < 3; ++i) rgb[i] = table[sector][i];
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
  const ::lightusd::PrimSpec* spec = node.GetPrimSpec();
  const std::string* type = spec ? spec->property_type_name(property) : nullptr;
  if (!type || (type->rfind("color3", 0) != 0 &&
                type->rfind("color4", 0) != 0)) {
    return true;
  }

  std::string source;
  bool authored = false;
  if (!::lightusd::color_management::ComputeColorSpaceName(
          node, property, &source, &authored)) {
    return false;
  }
  if (!authored) source = evaluation_space;
  ::lightusd::color::ColorTransform transform;
  if (!::lightusd::color_management::BuildColorTransform(
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
  const ::lightusd::PrimSpec* spec = node.GetPrimSpec();
  const std::vector<::lightusd::Path>* connections =
      spec ? spec->connection(property) : nullptr;
  if (connections && !connections->empty()) {
    return EvalMtlxConstantConnection(stage, (*connections)[0].str(),
                                      time_code, evaluation_space, out,
                                      visiting, depth + 1);
  }
  ::lightusd::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  ::lightusd::EvalOptions options = eval.GetOptions();
  options.follow_connections = false;
  const ::lightusd::EvalResult result =
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

  if (starts("ND_safepower_")) {
    MtlxConstantValue base, exponent;
    if (!input("in1", &base) || !input("in2", &exponent)) return false;
    *out = MtlxBinary(base, exponent, [](float x, float y) {
      return x == 0.0f ? 0.0f : std::copysign(std::pow(std::fabs(x), y), x);
    });
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

  if (starts("ND_magnitude_")) {
    MtlxConstantValue value;
    if (!input("in", &value)) return false;
    float length_squared = 0.0f;
    for (int i = 0; i < value.components; ++i) {
      const float component = value.component(i);
      length_squared += component * component;
    }
    out->components = 1;
    out->value[0] = std::sqrt(length_squared);
    out->color_managed = value.color_managed;
    return true;
  }

  if (starts("ND_distance_")) {
    MtlxConstantValue a, b;
    if (!input("in1", &a) || !input("in2", &b)) return false;
    float length_squared = 0.0f;
    const int components = std::max(a.components, b.components);
    for (int i = 0; i < components; ++i) {
      const float delta = a.component(i) - b.component(i);
      length_squared += delta * delta;
    }
    out->components = 1;
    out->value[0] = std::sqrt(length_squared);
    out->color_managed = a.color_managed || b.color_managed;
    return true;
  }

  if (starts("ND_range_")) {
    MtlxConstantValue value, in_low, in_high, out_low, out_high, gamma;
    if (!input("in", &value) || !input("inlow", &in_low) ||
        !input("inhigh", &in_high) || !input("outlow", &out_low) ||
        !input("outhigh", &out_high) || !input("gamma", &gamma)) return false;
    *out = value;
    out->color_managed = value.color_managed || in_low.color_managed ||
        in_high.color_managed || out_low.color_managed ||
        out_high.color_managed || gamma.color_managed;
    for (int i = 0; i < value.components; ++i) {
      const float lo = in_low.component(i), hi = in_high.component(i);
      const float denom = hi - lo;
      const float t = denom != 0.0f
          ? std::clamp((value.component(i) - lo) / denom, 0.0f, 1.0f)
          : 0.0f;
      out->value[static_cast<size_t>(i)] = out_low.component(i) +
          std::pow(t, gamma.component(i)) * (out_high.component(i) - out_low.component(i));
    }
    return true;
  }

  if (starts("ND_ramplr_")) {
    MtlxConstantValue left, right, texcoord;
    if (!input("valuel", &left) || !input("valuer", &right) ||
        !input("texcoord", &texcoord) || texcoord.components < 2) {
      return false;
    }
    const float u = texcoord.component(0);
    *out = MtlxBinary(left, right, [u](float a, float b) {
      return a + (b - a) * u;
    });
    out->color_managed = left.color_managed || right.color_managed ||
                         texcoord.color_managed;
    return true;
  }

  if (starts("ND_ramptb_")) {
    MtlxConstantValue top, bottom, texcoord;
    if (!input("valuet", &top) || !input("valueb", &bottom) ||
        !input("texcoord", &texcoord) || texcoord.components < 2) {
      return false;
    }
    const float v = texcoord.component(1);
    *out = MtlxBinary(bottom, top, [v](float b, float t) {
      return b + (t - b) * v;
    });
    out->color_managed = top.color_managed || bottom.color_managed ||
                         texcoord.color_managed;
    return true;
  }

  if (starts("ND_ramp4_")) {
    MtlxConstantValue top_left, top_right, bottom_left, bottom_right,
        texcoord;
    if (!input("valuetl", &top_left) || !input("valuetr", &top_right) ||
        !input("valuebl", &bottom_left) ||
        !input("valuebr", &bottom_right) || !input("texcoord", &texcoord) ||
        texcoord.components < 2) {
      return false;
    }
    const float u = texcoord.component(0);
    const float v = texcoord.component(1);
    const MtlxConstantValue bottom = MtlxBinary(
        bottom_left, bottom_right,
        [u](float a, float b) { return a + (b - a) * u; });
    const MtlxConstantValue top = MtlxBinary(
        top_left, top_right,
        [u](float a, float b) { return a + (b - a) * u; });
    *out = MtlxBinary(bottom, top,
                      [v](float b, float t) { return b + (t - b) * v; });
    out->color_managed = top_left.color_managed || top_right.color_managed ||
                         bottom_left.color_managed ||
                         bottom_right.color_managed ||
                         texcoord.color_managed;
    return true;
  }

  if (starts("ND_splitlr_")) {
    MtlxConstantValue left, right, center, texcoord;
    if (!input("valuel", &left) || !input("valuer", &right) ||
        !input("center", &center) || !input("texcoord", &texcoord) ||
        texcoord.components < 2) {
      return false;
    }
    *out = texcoord.component(0) < center.component(0) ? left : right;
    out->color_managed = out->color_managed || center.color_managed ||
                         texcoord.color_managed;
    return true;
  }

  if (starts("ND_splittb_")) {
    MtlxConstantValue top, bottom, center, texcoord;
    if (!input("valuet", &top) || !input("valueb", &bottom) ||
        !input("center", &center) || !input("texcoord", &texcoord) ||
        texcoord.components < 2) {
      return false;
    }
    *out = texcoord.component(1) < center.component(0) ? bottom : top;
    out->color_managed = out->color_managed || center.color_managed ||
                         texcoord.color_managed;
    return true;
  }

  if (starts("ND_circle_")) {
    MtlxConstantValue texcoord, center, radius;
    if (!input("texcoord", &texcoord) || !input("center", &center) ||
        !input("radius", &radius) || texcoord.components < 2 ||
        center.components < 2) {
      return false;
    }
    const float dx = texcoord.component(0) - center.component(0);
    const float dy = texcoord.component(1) - center.component(1);
    out->components = 1;
    out->value[0] = dx * dx + dy * dy <=
                            radius.component(0) * radius.component(0)
                        ? 1.0f
                        : 0.0f;
    out->color_managed = texcoord.color_managed || center.color_managed ||
                         radius.color_managed;
    return true;
  }

  if (starts("ND_trianglewave_")) {
    MtlxConstantValue value;
    if (!input("in", &value)) return false;
    out->components = 1;
    const float q = std::fmod(std::fabs(value.component(0)), 1.0f);
    out->value[0] = 0.5f - std::fabs(q - 0.5f);
    out->color_managed = value.color_managed;
    return true;
  }

  if (starts("ND_place2d_")) {
    MtlxConstantValue texcoord, pivot, scale, offset, rotation;
    if (!input("texcoord", &texcoord) || !input("pivot", &pivot) ||
        !input("scale", &scale) || !input("offset", &offset) ||
        !input("rotate", &rotation) || texcoord.components < 2) return false;
    const float x = texcoord.component(0) - pivot.component(0);
    const float y = texcoord.component(1) - pivot.component(1);
    const float radians = rotation.component(0) * (3.14159265358979323846f / 180.0f);
    const float rx = x * std::cos(radians) - y * std::sin(radians);
    const float ry = x * std::sin(radians) + y * std::cos(radians);
    out->components = 2;
    out->value[0] = rx / (scale.component(0) != 0.0f ? scale.component(0) : 1.0f) +
                    pivot.component(0) + offset.component(0);
    out->value[1] = ry / (scale.component(1) != 0.0f ? scale.component(1) : 1.0f) +
                    pivot.component(1) + offset.component(1);
    out->color_managed = texcoord.color_managed || pivot.color_managed ||
                         scale.color_managed || offset.color_managed;
    return true;
  }

  if (starts("ND_reflect_")) {
    MtlxConstantValue value, normal;
    if (!input("in", &value) || !input("normal", &normal) ||
        value.components < 3 || normal.components < 3) return false;
    float dot = 0.0f;
    for (int i = 0; i < 3; ++i) dot += value.value[static_cast<size_t>(i)] * normal.value[static_cast<size_t>(i)];
    *out = value;
    for (int i = 0; i < 3; ++i) out->value[static_cast<size_t>(i)] -= 2.0f * dot * normal.value[static_cast<size_t>(i)];
    out->color_managed = value.color_managed || normal.color_managed;
    return true;
  }

  if (starts("ND_refract_")) {
    MtlxConstantValue value, normal, ior;
    if (!input("in", &value) || !input("normal", &normal) ||
        !input("ior", &ior) || value.components < 3 || normal.components < 3) return false;
    float dot = 0.0f;
    for (int i = 0; i < 3; ++i) dot += value.value[static_cast<size_t>(i)] * normal.value[static_cast<size_t>(i)];
    const float eta = ior.component(0);
    const float k = 1.0f - eta * eta * (1.0f - dot * dot);
    out->components = 3;
    out->color_managed = value.color_managed || normal.color_managed || ior.color_managed;
    for (int i = 0; i < 3; ++i) {
      out->value[static_cast<size_t>(i)] = k < 0.0f ? 0.0f :
          eta * value.value[static_cast<size_t>(i)] -
          (eta * dot + std::sqrt(k)) * normal.value[static_cast<size_t>(i)];
    }
    return true;
  }

  if (starts("ND_atan2_")) {
    MtlxConstantValue y, x;
    if (!input("iny", &y) || !input("inx", &x)) return false;
    *out = MtlxBinary(y, x, [](float yy, float xx) {
      return std::atan2(yy, xx);
    });
    return true;
  }

  if (starts("ND_rotate2d_")) {
    MtlxConstantValue value, amount;
    if (!input("in", &value) || !input("amount", &amount) ||
        value.components < 2) return false;
    const float radians = amount.component(0) * (3.14159265358979323846f / 180.0f);
    const float x = value.component(0), y = value.component(1);
    *out = value;
    out->value[0] = x * std::cos(radians) - y * std::sin(radians);
    out->value[1] = x * std::sin(radians) + y * std::cos(radians);
    return true;
  }

  if (starts("ND_rgbtohsv_") || starts("ND_hsvtorgb_")) {
    MtlxConstantValue value;
    if (!input("in", &value) || value.components < 3) return false;
    *out = value;
    float converted[3];
    if (starts("ND_rgbtohsv_")) MtlxRgbToHsv(value.value.data(), converted);
    else MtlxHsvToRgb(value.value.data(), converted);
    for (int i = 0; i < 3; ++i) out->value[static_cast<size_t>(i)] = converted[i];
    return true;
  }

  if (starts("ND_hsvadjust_")) {
    MtlxConstantValue value, amount;
    if (!input("in", &value) || !input("amount", &amount) ||
        value.components < 3 || amount.components < 3) return false;
    float hsv[3], rgb[3];
    MtlxRgbToHsv(value.value.data(), hsv);
    hsv[0] += amount.component(0);
    hsv[1] *= amount.component(1);
    hsv[2] *= amount.component(2);
    MtlxHsvToRgb(hsv, rgb);
    *out = value;
    for (int i = 0; i < 3; ++i) out->value[static_cast<size_t>(i)] = rgb[i];
    out->color_managed = value.color_managed || amount.color_managed;
    return true;
  }

  // Constant-fold the common MaterialX unary math family. These nodes occur
  // frequently between DCC-authored controls and surface inputs; treating
  // them as unsupported discarded an otherwise fully evaluable material.
  if (starts("ND_absval_") || starts("ND_floor_") ||
      starts("ND_ceil_") || starts("ND_round_") ||
      starts("ND_sqrt_") || starts("ND_exp_") ||
      starts("ND_ln_") || starts("ND_asin_") || starts("ND_acos_") ||
      starts("ND_sin_") || starts("ND_cos_") ||
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
      else if (starts("ND_asin_")) y = std::asin(x);
      else if (starts("ND_acos_")) y = std::acos(x);
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
  const ::lightusd::PrimSpec* spec = prim.GetPrimSpec();
  const std::vector<::lightusd::Path>* forwarded =
      spec ? spec->connection(property) : nullptr;
  if (forwarded && !forwarded->empty()) {
    return EvalMtlxConstantConnection(stage, (*forwarded)[0].str(), time_code,
                                      evaluation_space, out, visiting,
                                      depth + 1);
  }
  if (::lightusd::IsShader(prim)) {
    return EvalMtlxConstantNode(stage, prim, time_code, evaluation_space, out,
                                visiting, depth + 1);
  }
  return false;
}

}}
