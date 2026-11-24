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
#include <cstdint>
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

static uint32_t MtlxRotl32(uint32_t x, int k) {
  return (x << k) | (x >> (32 - k));
}

static uint32_t MtlxHashFinalize(uint32_t a, uint32_t b, uint32_t c) {
  c ^= b; c -= MtlxRotl32(b, 14);
  a ^= c; a -= MtlxRotl32(c, 11);
  b ^= a; b -= MtlxRotl32(a, 25);
  c ^= b; c -= MtlxRotl32(b, 16);
  a ^= c; a -= MtlxRotl32(c, 4);
  b ^= a; b -= MtlxRotl32(a, 14);
  c ^= b; c -= MtlxRotl32(b, 24);
  return c;
}

static void MtlxHashMix(uint32_t* a, uint32_t* b, uint32_t* c) {
  *a -= *c; *a ^= MtlxRotl32(*c, 4); *c += *b;
  *b -= *a; *b ^= MtlxRotl32(*a, 6); *a += *c;
  *c -= *b; *c ^= MtlxRotl32(*b, 8); *b += *a;
  *a -= *c; *a ^= MtlxRotl32(*c, 16); *c += *b;
  *b -= *a; *b ^= MtlxRotl32(*a, 19); *a += *c;
  *c -= *b; *c ^= MtlxRotl32(*b, 4); *b += *a;
}

static uint32_t MtlxHashInt3(int x, int y, int z) {
  uint32_t a = 0xdeadbeefu + (3u << 2u) + 13u;
  uint32_t b = a, c = a;
  a += static_cast<uint32_t>(x);
  b += static_cast<uint32_t>(y);
  c += static_cast<uint32_t>(z);
  return MtlxHashFinalize(a, b, c);
}

static uint32_t MtlxHashInt2(int x, int y) {
  uint32_t a = 0xdeadbeefu + (2u << 2u) + 13u;
  uint32_t b = a, c = a;
  a += static_cast<uint32_t>(x);
  b += static_cast<uint32_t>(y);
  return MtlxHashFinalize(a, b, c);
}

static uint32_t MtlxCellHash(int x, int y, int z, bool is_3d, int channel) {
  if (channel >= 0 && is_3d) {
    uint32_t a = 0xdeadbeefu + (4u << 2u) + 13u;
    uint32_t b = a, c = a;
    a += static_cast<uint32_t>(x);
    b += static_cast<uint32_t>(y);
    c += static_cast<uint32_t>(z);
    MtlxHashMix(&a, &b, &c);
    return MtlxHashFinalize(a + static_cast<uint32_t>(channel), b, c);
  }
  if (channel >= 0 && !is_3d) {
    return MtlxHashInt3(x, y, channel);
  }
  return is_3d ? MtlxHashInt3(x, y, z) : MtlxHashInt2(x, y);
}

static float MtlxCellChannel(int x, int y, int z, bool is_3d,
                             int channel) {
  return static_cast<float>(MtlxCellHash(x, y, z, is_3d, channel)) /
         static_cast<float>(0xffffffffu);
}

static float MtlxNoiseGradient(uint32_t hash, float x, float y, float z) {
  const uint32_t h = hash & 15u;
  const float u = h < 8u ? x : y;
  const float v = h < 4u ? y : (((h == 12u) || (h == 14u)) ? x : z);
  return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
}

static float MtlxNoiseFade(float t) {
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float MtlxPerlin3(const float p[3]) {
  float f[3];
  const int cell[3] = {static_cast<int>(std::floor(p[0])),
                       static_cast<int>(std::floor(p[1])),
                       static_cast<int>(std::floor(p[2]))};
  for (int i = 0; i < 3; ++i) f[i] = p[i] - static_cast<float>(cell[i]);
  const float u = MtlxNoiseFade(f[0]);
  const float v = MtlxNoiseFade(f[1]);
  const float w = MtlxNoiseFade(f[2]);
  float corners[2][2][2];
  for (int z = 0; z < 2; ++z) {
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        const uint32_t hash = MtlxHashInt3(cell[0] + x, cell[1] + y,
                                           cell[2] + z);
        corners[z][y][x] = MtlxNoiseGradient(
            hash, f[0] - static_cast<float>(x),
            f[1] - static_cast<float>(y), f[2] - static_cast<float>(z));
      }
    }
  }
  const float x00 = corners[0][0][0] + (corners[0][0][1] - corners[0][0][0]) * u;
  const float x10 = corners[0][1][0] + (corners[0][1][1] - corners[0][1][0]) * u;
  const float x01 = corners[1][0][0] + (corners[1][0][1] - corners[1][0][0]) * u;
  const float x11 = corners[1][1][0] + (corners[1][1][1] - corners[1][1][0]) * u;
  const float y0 = x00 + (x10 - x00) * v;
  const float y1 = x01 + (x11 - x01) * v;
  return 0.9820f * (y0 + (y1 - y0) * w);
}

static float MtlxPerlin2(float x, float y) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(ix);
  const float fy = y - static_cast<float>(iy);
  const float u = MtlxNoiseFade(fx);
  const float v = MtlxNoiseFade(fy);
  auto gradient = [](uint32_t hash, float dx, float dy) {
    const uint32_t h = hash & 7u;
    const float a = h < 4u ? dx : dy;
    const float b = 2.0f * (h < 4u ? dy : dx);
    return ((h & 1u) ? -a : a) + ((h & 2u) ? -b : b);
  };
  const float a = gradient(MtlxHashInt2(ix, iy), fx, fy);
  const float b = gradient(MtlxHashInt2(ix + 1, iy), fx - 1.0f, fy);
  const float c = gradient(MtlxHashInt2(ix, iy + 1), fx, fy - 1.0f);
  const float d = gradient(MtlxHashInt2(ix + 1, iy + 1),
                           fx - 1.0f, fy - 1.0f);
  const float low = a + (b - a) * u;
  const float high = c + (d - c) * u;
  return 0.6616f * (low + (high - low) * v);
}

static float MtlxFractal3(float p[3], int octaves, float lacunarity,
                          float diminish) {
  if (octaves < 0 || octaves > 64) return 0.0f;
  float result = 0.0f;
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * MtlxPerlin3(p);
    amplitude *= diminish;
    for (int axis = 0; axis < 3; ++axis) p[axis] *= lacunarity;
  }
  return result;
}

static bool MtlxWorley(const MtlxConstantValue& coordinate, bool is_3d,
                      float jitter, int style, int components,
                      MtlxConstantValue* out) {
  if (!out || coordinate.components < (is_3d ? 3 : 2) ||
      components < 1 || components > 3) return false;
  const float p[3] = {coordinate.component(0), coordinate.component(1),
                      is_3d ? coordinate.component(2) : 0.0f};
  const int cell[3] = {static_cast<int>(std::floor(p[0])),
                       static_cast<int>(std::floor(p[1])),
                       is_3d ? static_cast<int>(std::floor(p[2])) : 0};
  const float local[3] = {p[0] - cell[0], p[1] - cell[1],
                          is_3d ? p[2] - cell[2] : 0.0f};
  float nearest[3] = {1.0e6f, 1.0e6f, 1.0e6f};
  float feature[3] = {0.0f, 0.0f, 0.0f};
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      for (int z = is_3d ? -1 : 0; z <= (is_3d ? 1 : 0); ++z) {
        const int cx = cell[0] + x, cy = cell[1] + y, cz = cell[2] + z;
        const float q[3] = {
            x + (MtlxCellChannel(cx, cy, cz, is_3d, 0) - 0.5f) * jitter + 0.5f,
            y + (MtlxCellChannel(cx, cy, cz, is_3d, 1) - 0.5f) * jitter + 0.5f,
            is_3d ? z + (MtlxCellChannel(cx, cy, cz, true, 2) - 0.5f) * jitter + 0.5f : 0.0f};
        const float dx = q[0] - local[0], dy = q[1] - local[1];
        const float dz = is_3d ? q[2] - local[2] : 0.0f;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (distance_squared < nearest[0]) {
          nearest[2] = nearest[1]; nearest[1] = nearest[0];
          nearest[0] = distance_squared;
          std::copy(q, q + 3, feature);
        } else if (distance_squared < nearest[1]) {
          nearest[2] = nearest[1]; nearest[1] = distance_squared;
        } else if (distance_squared < nearest[2]) {
          nearest[2] = distance_squared;
        }
      }
    }
  }
  out->components = components;
  out->color_managed = coordinate.color_managed;
  for (int i = 0; i < components; ++i) {
    if (style == 1) {
      const int hx = static_cast<int>(std::floor(feature[0] - local[0] + p[0]));
      const int hy = static_cast<int>(std::floor(feature[1] - local[1] + p[1]));
      const int hz = is_3d ? static_cast<int>(std::floor(feature[2] - local[2] + p[2])) : 0;
      out->value[static_cast<size_t>(i)] = MtlxCellChannel(
          hx, hy, hz, is_3d, components == 1 ? -1 : i);
    } else {
      out->value[static_cast<size_t>(i)] = std::sqrt(nearest[i]);
    }
  }
  return true;
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

static bool MtlxHexagon(float u, float v, float radius) {
  constexpr float kx = -0.866025f, ky = 0.5f, kz = 0.57735f;
  const float px = std::fabs(v), py = std::fabs(u);
  const float projection = std::min(kx * px + ky * py, 0.0f);
  const float p1x = px - 2.0f * projection * kx;
  const float p1y = py - 2.0f * projection * ky;
  const float reflected_x = -kx;
  const float reflected_projection = std::min(reflected_x * p1x + ky * p1y, 0.0f);
  const float p2x = p1x - 2.0f * reflected_projection * reflected_x;
  const float p2y = p1y - 2.0f * reflected_projection * ky;
  const float high = kz * radius;
  const float clamped_x = std::max(-high, std::min(p2x, high));
  const float p3x = p2x - clamped_x, p3y = p2y - radius;
  return p3x + p3y <= 0.0f;
}

static bool MtlxCloverleaf(float x, float y, float radius) {
  const float sx = 2.0f * x, sy = 2.0f * y;
  const float r2 = radius * radius;
  const auto inside = [r2](float a, float b) { return a * a + b * b <= r2; };
  return inside(sx + radius, sy) || inside(sx - radius, sy) ||
         inside(sx, sy - radius) || inside(sx, sy + radius);
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

  if (starts("ND_unifiednoise2d_") || starts("ND_unifiednoise3d_")) {
    const bool is_3d = starts("ND_unifiednoise3d_");
    MtlxConstantValue coordinate, frequency, offset, jitter, kind, style;
    if (!input(is_3d ? "position" : "texcoord", &coordinate)) return false;
    const int dimensions = is_3d ? 3 : 2;
    if (coordinate.components < dimensions) return false;
    frequency.components = dimensions;
    offset.components = dimensions;
    for (int i = 0; i < dimensions; ++i) frequency.value[static_cast<size_t>(i)] = 1.0f;
    if (!input("freq", &frequency)) {
      frequency.components = dimensions;
      for (int i = 0; i < dimensions; ++i) frequency.value[static_cast<size_t>(i)] = 1.0f;
    }
    (void)input("offset", &offset);
    const float jitter_value = input("jitter", &jitter)
                                   ? jitter.component(0) : 1.0f;
    const int noise_type = input("type", &kind)
                               ? static_cast<int>(kind.component(0)) : 0;
    const int style_value = input("style", &style)
                                ? static_cast<int>(style.component(0)) : 0;

    float p[3] = {coordinate.component(0) * frequency.component(0) + offset.component(0),
                  coordinate.component(1) * frequency.component(1) + offset.component(1),
                  is_3d ? coordinate.component(2) * frequency.component(2) + offset.component(2) : 0.0f};
    const float angle_degrees = (jitter_value - 1.0f) * 90000.0f;
    const float radians = angle_degrees * 3.14159265358979323846f / 180.0f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    float rotated[3] = {p[0], p[1], p[2]};
    if (is_3d) {
      const float axis_length = std::sqrt(1.01f);
      const float axis[3] = {0.1f / axis_length, 1.0f / axis_length, 0.0f};
      const float dot = axis[0] * p[0] + axis[1] * p[1];
      const float cross[3] = {axis[1] * p[2], -axis[0] * p[2],
                              axis[0] * p[1] - axis[1] * p[0]};
      for (int i = 0; i < 3; ++i) {
        rotated[i] = p[i] * cosine + cross[i] * sine +
                     axis[i] * dot * (1.0f - cosine);
      }
    } else {
      rotated[0] = p[0] * cosine - p[1] * sine;
      rotated[1] = p[0] * sine + p[1] * cosine;
    }

    float noise = 0.0f;
    if (noise_type == 0) {
      noise = 0.5f * (is_3d ? MtlxPerlin3(rotated)
                              : MtlxPerlin2(rotated[0], rotated[1])) + 0.5f;
    } else if (noise_type == 1) {
      const int x = static_cast<int>(std::floor(rotated[0]));
      const int y = static_cast<int>(std::floor(rotated[1]));
      const int z = is_3d ? static_cast<int>(std::floor(rotated[2])) : 0;
      noise = is_3d
          ? MtlxCellChannel(x, y, z, true, -1)
          : static_cast<float>(MtlxHashInt2(x, y)) /
                static_cast<float>(0xffffffffu);
    } else if (noise_type == 2) {
      MtlxConstantValue noise_value;
      MtlxConstantValue worley_point;
      worley_point.components = dimensions;
      worley_point.value = {{p[0], p[1], p[2], 0.0f}};
      if (!MtlxWorley(worley_point, is_3d, jitter_value, style_value, 1,
                      &noise_value)) return false;
      noise = noise_value.component(0);
    } else if (noise_type == 3) {
      MtlxConstantValue octaves, lacunarity, diminish;
      const int octave_count = input("octaves", &octaves)
                                   ? static_cast<int>(octaves.component(0)) : 3;
      const float lacunarity_value = input("lacunarity", &lacunarity)
                                         ? lacunarity.component(0) : 2.0f;
      const float diminish_value = input("diminish", &diminish)
                                       ? diminish.component(0) : 0.5f;
      float fractal_point[3] = {is_3d ? rotated[0] : p[0],
                                is_3d ? rotated[1] : p[1],
                                is_3d ? rotated[2] : angle_degrees};
      noise = MtlxFractal3(fractal_point, octave_count, lacunarity_value,
                           diminish_value);
    } else {
      return false;
    }

    MtlxConstantValue out_min, out_max, clamp_output;
    const float minimum = input("outmin", &out_min)
                              ? out_min.component(0) : 0.0f;
    const float maximum = input("outmax", &out_max)
                              ? out_max.component(0) : 1.0f;
    const bool clamp = !input("clampoutput", &clamp_output) ||
                       clamp_output.component(0) != 0.0f;
    noise = minimum + noise * (maximum - minimum);
    if (clamp) noise = std::max(std::min(minimum, maximum),
                                std::min(std::max(minimum, maximum), noise));
    out->components = 1;
    out->color_managed = false;
    out->value[0] = noise;
    return true;
  }

  if (starts("ND_worleynoise2d_") || starts("ND_worleynoise3d_")) {
    const bool is_3d = starts("ND_worleynoise3d_");
    MtlxConstantValue coordinate, jitter, style;
    if (!input(is_3d ? "position" : "texcoord", &coordinate)) return false;
    const float jitter_value = input("jitter", &jitter)
                                   ? jitter.component(0) : 1.0f;
    const int style_value = input("style", &style)
                                ? static_cast<int>(style.component(0)) : 0;
    const int components = id.find("_vector3") != std::string::npos ? 3
        : (id.find("_vector2") != std::string::npos ? 2 : 1);
    return MtlxWorley(coordinate, is_3d, jitter_value, style_value,
                      components, out);
  }

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
    MtlxConstantValue value, in_low, in_high, out_low, out_high, gamma, do_clamp;
    if (!input("in", &value) || !input("inlow", &in_low) ||
        !input("inhigh", &in_high) || !input("outlow", &out_low) ||
        !input("outhigh", &out_high) || !input("gamma", &gamma)) return false;
    if (!input("doclamp", &do_clamp)) {
      const ::lightusd::PrimSpec* spec = node.GetPrimSpec();
      if (spec && (spec->property("inputs:doclamp") ||
                   spec->connection("inputs:doclamp"))) return false;
      // An omitted doclamp uses the MaterialX default, false.
    }
    *out = value;
    out->color_managed = value.color_managed || in_low.color_managed ||
        in_high.color_managed || out_low.color_managed ||
        out_high.color_managed || gamma.color_managed || do_clamp.color_managed;
    for (int i = 0; i < value.components; ++i) {
      const float lo = in_low.component(i), hi = in_high.component(i);
      const float outlo = out_low.component(i), outhi = out_high.component(i);
      const float t = (value.component(i) - lo) / (hi - lo);
      const float sign = t > 0.0f ? 1.0f : (t < 0.0f ? -1.0f : 0.0f);
      const float mapped = outlo + sign * std::pow(std::fabs(t), 1.0f / gamma.component(i)) *
                                      (outhi - outlo);
      out->value[static_cast<size_t>(i)] = do_clamp.component(0) != 0.0f
          ? std::clamp(mapped, std::min(outlo, outhi), std::max(outlo, outhi))
          : mapped;
    }
    return true;
  }

  if (starts("ND_ramplr_")) {
    MtlxConstantValue left, right, texcoord;
    if (!input("valuel", &left) || !input("valuer", &right) ||
        !input("texcoord", &texcoord) || texcoord.components < 2) {
      return false;
    }
    const float u = std::clamp(texcoord.component(0), 0.0f, 1.0f);
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
    const float v = std::clamp(texcoord.component(1), 0.0f, 1.0f);
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
    const float u = std::clamp(texcoord.component(0), 0.0f, 1.0f);
    const float v = std::clamp(texcoord.component(1), 0.0f, 1.0f);
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

  if (starts("ND_checkerboard_")) {
    MtlxConstantValue color1, color2, texcoord, tiling, offset;
    if (!input("color1", &color1) || !input("color2", &color2) ||
        !input("texcoord", &texcoord) || !input("uvtiling", &tiling) ||
        !input("uvoffset", &offset) || color1.components < 3 ||
        color2.components < 3 || texcoord.components < 2 ||
        tiling.components < 2 || offset.components < 2) {
      return false;
    }
    const float ix = std::floor(texcoord.component(0) * tiling.component(0) -
                                offset.component(0));
    const float iy = std::floor(texcoord.component(1) * tiling.component(1) -
                                offset.component(1));
    const float total = ix + iy;
    const float mix = total - 2.0f * std::floor(total / 2.0f);
    out->components = 3;
    for (int i = 0; i < out->components; ++i) {
      out->value[i] = color1.component(i) * mix +
                      color2.component(i) * (1.0f - mix);
    }
    out->color_managed = color1.color_managed || color2.color_managed ||
                         texcoord.color_managed || tiling.color_managed ||
                         offset.color_managed;
    return true;
  }

  if (starts("ND_grid_")) {
    MtlxConstantValue texcoord, tiling, offset, thickness, staggered;
    if (!input("texcoord", &texcoord) || !input("uvtiling", &tiling) ||
        !input("uvoffset", &offset) || !input("thickness", &thickness) ||
        !input("staggered", &staggered) || texcoord.components < 2 ||
        tiling.components < 2 || offset.components < 2) {
      return false;
    }
    const float x = texcoord.component(0) * tiling.component(0) -
                    offset.component(0);
    const float y = texcoord.component(1) * tiling.component(1) -
                    offset.component(1);
    const float row = y - 2.0f * std::floor(y / 2.0f);
    const float shift = staggered.component(0) != 0.0f && row > 1.0f
                            ? 0.5f
                            : 0.0f;
    const float shifted_x = x + shift;
    const float mx = shifted_x - std::floor(shifted_x);
    const float my = y - std::floor(y);
    const float threshold = 1.0f - thickness.component(0);
    const bool on_grid = std::fabs(2.0f * mx - 1.0f) > threshold ||
                         std::fabs(2.0f * my - 1.0f) > threshold;
    out->components = 3;
    out->value[0] = out->value[1] = out->value[2] = on_grid ? 1.0f : 0.0f;
    out->color_managed = texcoord.color_managed || tiling.color_managed ||
                         offset.color_managed || thickness.color_managed ||
                         staggered.color_managed;
    return true;
  }

  if (starts("ND_grid_")) {
    MtlxConstantValue texcoord, tiling, offset, thickness, staggered;
    if (!input("texcoord", &texcoord) || !input("uvtiling", &tiling) ||
        !input("uvoffset", &offset) || !input("thickness", &thickness) ||
        !input("staggered", &staggered) || texcoord.components < 2 ||
        tiling.components < 2 || offset.components < 2) {
      return false;
    }
    const float x = texcoord.component(0) * tiling.component(0) -
                    offset.component(0);
    const float y = texcoord.component(1) * tiling.component(1) -
                    offset.component(1);
    const float row = y - 2.0f * std::floor(y / 2.0f);
    const float shift = staggered.component(0) != 0.0f && row > 1.0f
                            ? 0.5f
                            : 0.0f;
    const float shifted_x = x + shift;
    const float mx = shifted_x - std::floor(shifted_x);
    const float my = y - std::floor(y);
    const float threshold = 1.0f - thickness.component(0);
    const bool on_grid = std::fabs(2.0f * mx - 1.0f) > threshold ||
                         std::fabs(2.0f * my - 1.0f) > threshold;
    out->components = 3;
    out->value[0] = out->value[1] = out->value[2] = on_grid ? 1.0f : 0.0f;
    out->color_managed = texcoord.color_managed || tiling.color_managed ||
                         offset.color_managed || thickness.color_managed ||
                         staggered.color_managed;
    return true;
  }

  if (starts("ND_tiledcircles_") || starts("ND_tiledcloverleafs_") ||
      starts("ND_tiledhexagons_")) {
    const bool cloverleaf = starts("ND_tiledcloverleafs_");
    const bool hexagon = starts("ND_tiledhexagons_");
    MtlxConstantValue texcoord, tiling, offset, size, staggered;
    if (!input("texcoord", &texcoord) || !input("uvtiling", &tiling) ||
        !input("uvoffset", &offset) || !input("size", &size) ||
        !input("staggered", &staggered) || texcoord.components < 2 ||
        tiling.components < 2 || offset.components < 2) return false;
    const float x = texcoord.component(0) * tiling.component(0) - offset.component(0);
    const float y = texcoord.component(1) * tiling.component(1) - offset.component(1);
    auto mod = [](float a, float b) { return a - b * std::floor(a / b); };
    auto inside = [cloverleaf, hexagon](float a, float b, float radius) {
      if (cloverleaf) return MtlxCloverleaf(a, b, radius);
      if (hexagon) return MtlxHexagon(a, b, radius);
      return a * a + b * b <= radius * radius;
    };
    bool hit;
    if (staggered.component(0) == 0.0f) {
      hit = inside(2.0f * mod(x, 1.0f) - 1.0f,
                   2.0f * mod(y, 1.0f) - 1.0f, size.component(0));
    } else {
      const float half = cloverleaf ? 0.5f : 0.866025f;
      const float shift = mod(y, 2.0f * half) > half ? 0.5f : 0.0f;
      const float mx = mod(x + shift, 1.0f), my = mod(y, half);
      const float radius = size.component(0) * 0.5f;
      hit = inside(mx, my, radius) || inside(1.0f - mx, my, radius) ||
            inside(mx - 0.5f, half - my, radius);
    }
    out->components = 3;
    out->value[0] = out->value[1] = out->value[2] = hit ? 1.0f : 0.0f;
    out->color_managed = texcoord.color_managed || tiling.color_managed ||
                         offset.color_managed || size.color_managed || staggered.color_managed;
    return true;
  }

  if (starts("ND_crosshatch_")) {
    MtlxConstantValue texcoord, tiling, offset, thickness, staggered;
    if (!input("texcoord", &texcoord) || !input("uvtiling", &tiling) ||
        !input("uvoffset", &offset) || !input("thickness", &thickness) ||
        !input("staggered", &staggered) || texcoord.components < 2 ||
        tiling.components < 2 || offset.components < 2) {
      return false;
    }
    const float x = texcoord.component(0) * tiling.component(0) -
                    offset.component(0);
    const float y = texcoord.component(1) * tiling.component(1) -
                    offset.component(1);
    const float row = y - 2.0f * std::floor(y / 2.0f);
    const float shift = staggered.component(0) != 0.0f && row > 1.0f
                            ? 0.5f
                            : 0.0f;
    const float shifted_x = x + shift;
    const float sx = 2.0f * (shifted_x - std::floor(shifted_x)) - 1.0f;
    const float sy = 2.0f * (y - std::floor(y)) - 1.0f;
    const float radius_squared = 2.0f * thickness.component(0) *
                                 thickness.component(0);
    const float diag1 = sx - sy;
    const float diag2 = sx + sy;
    const bool on_diagonal = thickness.component(0) >= 0.0f &&
                             (diag1 * diag1 <= radius_squared ||
                              diag2 * diag2 <= radius_squared);
    out->components = 3;
    out->value[0] = out->value[1] = out->value[2] =
        on_diagonal ? 1.0f : 0.0f;
    out->color_managed = texcoord.color_managed || tiling.color_managed ||
                         offset.color_managed || thickness.color_managed ||
                         staggered.color_managed;
    return true;
  }

  if (starts("ND_hexagon_")) {
    MtlxConstantValue texcoord, center, radius;
    if (!input("texcoord", &texcoord) || !input("center", &center) ||
        !input("radius", &radius) || texcoord.components < 2 ||
        center.components < 2) {
      return false;
    }
    out->components = 1;
    out->value[0] = MtlxHexagon(texcoord.component(0) - center.component(0),
                                texcoord.component(1) - center.component(1),
                                radius.component(0)) ? 1.0f : 0.0f;
    out->color_managed = texcoord.color_managed || center.color_managed ||
                         radius.color_managed;
    return true;
  }

  if (starts("ND_cloverleaf_")) {
    MtlxConstantValue texcoord, center, radius;
    if (!input("texcoord", &texcoord) || !input("center", &center) ||
        !input("radius", &radius) || texcoord.components < 2 || center.components < 2) return false;
    out->components = 1;
    out->value[0] = MtlxCloverleaf(texcoord.component(0) - center.component(0),
                                   texcoord.component(1) - center.component(1),
                                   radius.component(0)) ? 1.0f : 0.0f;
    out->color_managed = texcoord.color_managed || center.color_managed || radius.color_managed;
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

  if (starts("ND_line_")) {
    MtlxConstantValue texcoord, center, radius, point1, point2;
    if (!input("texcoord", &texcoord) || !input("center", &center) ||
        !input("radius", &radius) || !input("point1", &point1) ||
        !input("point2", &point2) || texcoord.components < 2 ||
        center.components < 2 || point1.components < 2 ||
        point2.components < 2) {
      return false;
    }
    const float px = texcoord.component(0) - center.component(0) -
                     point1.component(0);
    const float py = texcoord.component(1) - center.component(1) -
                     point1.component(1);
    const float bx = point2.component(0) - point1.component(0);
    const float by = point2.component(1) - point1.component(1);
    const float length_squared = bx * bx + by * by;
    float projection = 0.0f;
    if (length_squared > 1.0e-12f) {
      projection = (px * bx + py * by) / length_squared;
      projection = std::max(0.0f, std::min(1.0f, projection));
    }
    const float dx = px - bx * projection;
    const float dy = py - by * projection;
    out->components = 1;
    out->value[0] = std::sqrt(dx * dx + dy * dy) > radius.component(0)
                        ? 0.0f
                        : 1.0f;
    out->color_managed = texcoord.color_managed || center.color_managed ||
                         radius.color_managed || point1.color_managed ||
                         point2.color_managed;
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
    MtlxConstantValue texcoord, pivot, scale, offset, rotation, order;
    if (!input("texcoord", &texcoord) || !input("pivot", &pivot) ||
        !input("scale", &scale) || !input("offset", &offset) ||
        !input("rotate", &rotation) || texcoord.components < 2) return false;
    if (!input("operationorder", &order)) {
      const ::lightusd::PrimSpec* spec = node.GetPrimSpec();
      if (spec && (spec->property("inputs:operationorder") ||
                   spec->connection("inputs:operationorder"))) return false;
      // The MaterialX default operation order is SRT (0).
    }
    float x = texcoord.component(0) - pivot.component(0);
    float y = texcoord.component(1) - pivot.component(1);
    const float radians = rotation.component(0) * (3.14159265358979323846f / 180.0f);
    const float cs = std::cos(radians), sn = std::sin(radians);
    const float sx = scale.component(0) != 0.0f ? scale.component(0) : 1.0f;
    const float sy = scale.component(1) != 0.0f ? scale.component(1) : 1.0f;
    out->components = 2;
    if (order.component(0) < 1.0f) {
      x /= sx; y /= sy;
      out->value[0] = x * cs + y * sn - offset.component(0) + pivot.component(0);
      out->value[1] = -x * sn + y * cs - offset.component(1) + pivot.component(1);
    } else if (order.component(0) < 2.0f) {
      x -= offset.component(0); y -= offset.component(1);
      out->value[0] = (x * cs + y * sn) / sx + pivot.component(0);
      out->value[1] = (-x * sn + y * cs) / sy + pivot.component(1);
    } else {
      out->value[0] = 0.0f;
      out->value[1] = 0.0f;
    }
    out->color_managed = texcoord.color_managed || pivot.color_managed ||
                         scale.color_managed || offset.color_managed ||
                         rotation.color_managed || order.color_managed;
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

  if (starts("ND_colorcorrect_")) {
    MtlxConstantValue value, hue, saturation, gamma, lift, gain, contrast, pivot, exposure;
    if (!input("in", &value)) {
      const ::lightusd::PrimSpec* spec = node.GetPrimSpec();
      if (spec && (spec->property("inputs:in") || spec->connection("inputs:in"))) return false;
      value.components = starts("ND_colorcorrect_color4") ? 4 : 3;
      value.value = {{1.0f, 1.0f, 1.0f, 0.0f}};
    }
    if (value.components != 3 && value.components != 4) return false;
    auto parameter = [&](const char* name, float fallback, MtlxConstantValue* result) {
      if (input(name, result)) return true;
      const ::lightusd::PrimSpec* spec = node.GetPrimSpec();
      const std::string property = std::string("inputs:") + name;
      if (spec && (spec->property(property) || spec->connection(property))) return false;
      result->components = 1;
      result->value[0] = fallback;
      return true;
    };
    if (!parameter("hue", 0.0f, &hue) || !parameter("saturation", 1.0f, &saturation) ||
        !parameter("gamma", 1.0f, &gamma) || !parameter("lift", 0.0f, &lift) ||
        !parameter("gain", 1.0f, &gain) || !parameter("contrast", 1.0f, &contrast) ||
        !parameter("contrastpivot", 0.5f, &pivot) || !parameter("exposure", 0.0f, &exposure)) return false;
    float hsv[3], rgb[3];
    MtlxRgbToHsv(value.value.data(), hsv);
    hsv[0] += hue.component(0);
    MtlxHsvToRgb(hsv, rgb);
    const float gray = 0.2722287f * rgb[0] + 0.6740818f * rgb[1] + 0.0536895f * rgb[2];
    const float factor = std::pow(2.0f, exposure.component(0));
    const float reciprocal_gamma = 1.0f / gamma.component(0);
    *out = value;
    for (int i = 0; i < 3; ++i) {
      float x = gray + saturation.component(0) * (rgb[i] - gray);
      const float sign = x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
      x = sign * std::pow(std::fabs(x), reciprocal_gamma);
      x = (x * (1.0f - lift.component(0)) + lift.component(0)) * gain.component(0);
      out->value[static_cast<size_t>(i)] =
          ((x - pivot.component(0)) * contrast.component(0) + pivot.component(0)) * factor;
    }
    out->color_managed = value.color_managed || hue.color_managed || saturation.color_managed ||
        gamma.color_managed || lift.color_managed || gain.color_managed ||
        contrast.color_managed || pivot.color_managed || exposure.color_managed;
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
    MtlxConstantValue value1, value2;
    if (!input("value1", &value1) || !input("value2", &value2)) return false;
    const float a = value1.component(0);
    const float b = value2.component(0);
    bool condition = false;
    if (starts("ND_ifgreatereq_")) {
      condition = a >= b;
    } else if (starts("ND_ifgreater_")) {
      condition = a > b;
    } else {
      condition = a == b;
    }
    if (starts("ND_ifgreater_boolean") || starts("ND_ifgreatereq_boolean") ||
        starts("ND_ifequal_boolean")) {
      out->components = 1;
      out->value[0] = condition ? 1.0f : 0.0f;
      out->color_managed = false;
      return true;
    }
    MtlxConstantValue when_true, when_false;
    if (!input("in1", &when_true) || !input("in2", &when_false)) return false;
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
