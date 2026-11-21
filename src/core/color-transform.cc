// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "core/color-transform.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lightusd {
namespace color {
namespace {

struct ChromaDef {
  const char *name;
  float r[2];
  float g[2];
  float b[2];
  float w[2];
  float gamma;
  float bias;
  ColorSpaceKind kind;
};

constexpr float kD65[2] = {0.3127f, 0.3290f};

// ACES primaries are pre-adapted to D65 exactly as in OpenUSD NanoColor.
const ChromaDef kBuiltins[] = {
    {"lin_ap1_scene", {0.71319588766205f, 0.29268891446333f},
     {0.15950855654178f, 0.83878851615096f},
     {0.128672995285350f, 0.043895571160528f}, {kD65[0], kD65[1]},
     1.0f, 0.0f, ColorSpaceKind::Color},
    {"lin_ap0_scene", {0.73485524337371f, 0.26422532524554f},
     {-0.0061709124786224f, 1.0113149590212864f},
     {0.015967559255041f, -0.064235503128551f}, {kD65[0], kD65[1]},
     1.0f, 0.0f, ColorSpaceKind::Color},
    {"lin_rec709_scene", {0.640f, 0.330f}, {0.300f, 0.600f},
     {0.150f, 0.060f}, {kD65[0], kD65[1]}, 1.0f, 0.0f,
     ColorSpaceKind::Color},
    {"lin_p3d65_scene", {0.6800f, 0.3200f}, {0.2650f, 0.6900f},
     {0.1500f, 0.0600f}, {kD65[0], kD65[1]}, 1.0f, 0.0f,
     ColorSpaceKind::Color},
    {"lin_rec2020_scene", {0.708f, 0.292f}, {0.170f, 0.797f},
     {0.131f, 0.046f}, {kD65[0], kD65[1]}, 1.0f, 0.0f,
     ColorSpaceKind::Color},
    {"lin_adobergb_scene", {0.640f, 0.330f}, {0.210f, 0.710f},
     {0.150f, 0.060f}, {kD65[0], kD65[1]}, 1.0f, 0.0f,
     ColorSpaceKind::Color},
    {"srgb_rec709_scene", {0.640f, 0.330f}, {0.300f, 0.600f},
     {0.150f, 0.060f}, {kD65[0], kD65[1]}, 2.4f, 0.055f,
     ColorSpaceKind::Color},
    {"g22_rec709_scene", {0.640f, 0.330f}, {0.300f, 0.600f},
     {0.150f, 0.060f}, {kD65[0], kD65[1]}, 2.2f, 0.0f,
     ColorSpaceKind::Color},
    {"g18_rec709_scene", {0.640f, 0.330f}, {0.300f, 0.600f},
     {0.150f, 0.060f}, {kD65[0], kD65[1]}, 1.8f, 0.0f,
     ColorSpaceKind::Color},
    {"srgb_ap1_scene", {0.71319588766205f, 0.29268891446333f},
     {0.15950855654178f, 0.83878851615096f},
     {0.128672995285350f, 0.043895571160528f}, {kD65[0], kD65[1]},
     2.4f, 0.055f, ColorSpaceKind::Color},
    {"g22_ap1_scene", {0.71319588766205f, 0.29268891446333f},
     {0.15950855654178f, 0.83878851615096f},
     {0.128672995285350f, 0.043895571160528f}, {kD65[0], kD65[1]},
     2.2f, 0.0f, ColorSpaceKind::Color},
    {"srgb_p3d65_scene", {0.6800f, 0.3200f}, {0.2650f, 0.6900f},
     {0.1500f, 0.0600f}, {kD65[0], kD65[1]}, 2.4f, 0.055f,
     ColorSpaceKind::Color},
    {"g22_adobergb_scene", {0.640f, 0.330f}, {0.210f, 0.710f},
     {0.150f, 0.060f}, {kD65[0], kD65[1]}, 2.2f, 0.0f,
     ColorSpaceKind::Color},
};

bool Invert3x3(const float m[9], float out[9]) {
  const float det =
      m[0] * (m[4] * m[8] - m[5] * m[7]) -
      m[1] * (m[3] * m[8] - m[5] * m[6]) +
      m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (!std::isfinite(det) || std::fabs(det) < 1.0e-12f) return false;
  const float d = 1.0f / det;
  out[0] = (m[4] * m[8] - m[5] * m[7]) * d;
  out[1] = (m[2] * m[7] - m[1] * m[8]) * d;
  out[2] = (m[1] * m[5] - m[2] * m[4]) * d;
  out[3] = (m[5] * m[6] - m[3] * m[8]) * d;
  out[4] = (m[0] * m[8] - m[2] * m[6]) * d;
  out[5] = (m[2] * m[3] - m[0] * m[5]) * d;
  out[6] = (m[3] * m[7] - m[4] * m[6]) * d;
  out[7] = (m[1] * m[6] - m[0] * m[7]) * d;
  out[8] = (m[0] * m[4] - m[1] * m[3]) * d;
  return true;
}

void Mul3x3(const float a[9], const float b[9], float out[9]) {
  float t[9];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      t[r * 3 + c] = a[r * 3] * b[c] +
                     a[r * 3 + 1] * b[3 + c] +
                     a[r * 3 + 2] * b[6 + c];
    }
  }
  std::memcpy(out, t, sizeof(t));
}

void MulVec(const float m[9], const float in[3], float out[3]) {
  const float x = in[0], y = in[1], z = in[2];
  out[0] = m[0] * x + m[1] * y + m[2] * z;
  out[1] = m[3] * x + m[4] * y + m[5] * z;
  out[2] = m[6] * x + m[7] * y + m[8] * z;
}

void TransferParams(const ColorSpaceDesc &space, float *k0, float *phi) {
  if (space.gamma == 1.0f) {
    *k0 = 1.0e9f;
    *phi = 1.0f;
  } else if (space.linear_bias <= 0.0f) {
    *k0 = 0.0f;
    *phi = 1.0f;
  } else {
    const float a = space.linear_bias;
    const float g = space.gamma;
    *k0 = a / (g - 1.0f);
    *phi = (a / std::exp(std::log(g * a / (g + g * a - 1.0f - a)) * g)) /
           (g - 1.0f);
  }
}

float ToLinear(const ColorSpaceDesc &space, float value) {
  const float sign = value < 0.0f ? -1.0f : 1.0f;
  const float v = std::fabs(value);
  float k0 = 0.0f, phi = 1.0f;
  TransferParams(space, &k0, &phi);
  if (v < k0) return sign * (v / phi);
  const float a = space.linear_bias;
  return sign * std::pow((v + a) / (1.0f + a), space.gamma);
}

float FromLinear(const ColorSpaceDesc &space, float value) {
  const float sign = value < 0.0f ? -1.0f : 1.0f;
  const float v = std::fabs(value);
  float k0 = 0.0f, phi = 1.0f;
  TransferParams(space, &k0, &phi);
  if (v < k0 / phi) return sign * (v * phi);
  const float a = space.linear_bias;
  return sign * ((1.0f + a) * std::pow(v, 1.0f / space.gamma) - a);
}

}  // namespace

std::string CanonicalizeToken(const std::string &token) {
  struct Alias { const char *alias; const char *canonical; };
  static const Alias aliases[] = {
      {"acescg", "lin_ap1_scene"}, {"lin_ap1", "lin_ap1_scene"},
      {"aces2065-1", "lin_ap0_scene"}, {"lin_ap0", "lin_ap0_scene"},
      {"lin_rec709", "lin_rec709_scene"}, {"lin_srgb", "lin_rec709_scene"},
      {"srgb_texture", "srgb_rec709_scene"}, {"sRGB", "srgb_rec709_scene"},
      {"srgb", "srgb_rec709_scene"}, {"g22_rec709", "g22_rec709_scene"},
      {"g18_rec709", "g18_rec709_scene"}, {"g22_ap1", "g22_ap1_scene"},
      {"lin_displayp3", "lin_p3d65_scene"},
      {"srgb_displayp3", "srgb_p3d65_scene"},
      {"lin_rec2020", "lin_rec2020_scene"},
      {"lin_adobergb", "lin_adobergb_scene"},
      {"adobergb", "g22_adobergb_scene"},
  };
  for (const Alias &a : aliases) {
    if (token == a.alias) return a.canonical;
  }
  return token;
}

bool MakeColorSpaceFromChromaticities(
    const std::string &name, const float red[2], const float green[2],
    const float blue[2], const float white[2], float gamma,
    float linear_bias, ColorSpaceDesc *out) {
  if (!out || !red || !green || !blue || !white || !(gamma > 0.0f) ||
      !std::isfinite(gamma) || !std::isfinite(linear_bias) ||
      white[1] == 0.0f) return false;
  float p[9] = {
      red[0], green[0], blue[0],
      red[1], green[1], blue[1],
      1.0f - red[0] - red[1],
      1.0f - green[0] - green[1],
      1.0f - blue[0] - blue[1],
  };
  float inv[9];
  if (!Invert3x3(p, inv)) return false;
  const float w[3] = {white[0] / white[1], 1.0f,
                      (1.0f - white[0] - white[1]) / white[1]};
  float c[3];
  MulVec(inv, w, c);
  for (int r = 0; r < 3; ++r) {
    p[r * 3] *= c[0];
    p[r * 3 + 1] *= c[1];
    p[r * 3 + 2] *= c[2];
  }
  out->name = name;
  std::memcpy(out->rgb_to_xyz, p, sizeof(p));
  out->gamma = gamma;
  out->linear_bias = linear_bias;
  out->kind = ColorSpaceKind::Color;
  return true;
}

bool GetBuiltinColorSpace(const std::string &token, ColorSpaceDesc *out) {
  if (!out) return false;
  const std::string name = CanonicalizeToken(token);
  if (name == "lin_ciexyzd65_scene") {
    *out = ColorSpaceDesc{};
    out->name = name;
    out->kind = ColorSpaceKind::Color;
    return true;
  }
  if (name == "data" || name == "raw" || name == "unknown" ||
      name == "identity") {
    *out = ColorSpaceDesc{};
    out->name = name;
    out->kind = (name == "data" || name == "raw")
                    ? ColorSpaceKind::Data : ColorSpaceKind::Unknown;
    return true;
  }
  for (const ChromaDef &def : kBuiltins) {
    if (name == def.name) {
      if (!MakeColorSpaceFromChromaticities(
              name, def.r, def.g, def.b, def.w, def.gamma, def.bias, out)) {
        return false;
      }
      out->kind = def.kind;
      return true;
    }
  }
  return false;
}

bool BuildColorTransform(const ColorSpaceDesc &source,
                         const ColorSpaceDesc &destination,
                         ColorTransform *out) {
  if (!out) return false;
  out->source = source;
  out->destination = destination;
  out->bypass = (!source.name.empty() && source.name == destination.name) ||
                source.kind != ColorSpaceKind::Color ||
                destination.kind != ColorSpaceKind::Color;
  const float identity[9] = {1,0,0, 0,1,0, 0,0,1};
  std::memcpy(out->matrix, identity, sizeof(identity));
  if (out->bypass) return true;
  float xyz_to_dst[9];
  if (!Invert3x3(destination.rgb_to_xyz, xyz_to_dst)) return false;
  Mul3x3(xyz_to_dst, source.rgb_to_xyz, out->matrix);
  out->bypass = false;
  return true;
}

void TransformRGB(const ColorTransform &transform, float rgb[3]) {
  if (!rgb || transform.bypass) return;
  float linear[3] = {ToLinear(transform.source, rgb[0]),
                     ToLinear(transform.source, rgb[1]),
                     ToLinear(transform.source, rgb[2])};
  float converted[3];
  MulVec(transform.matrix, linear, converted);
  rgb[0] = FromLinear(transform.destination, converted[0]);
  rgb[1] = FromLinear(transform.destination, converted[1]);
  rgb[2] = FromLinear(transform.destination, converted[2]);
}

void TransformRGBSpan(const ColorTransform &transform, float *rgb,
                      size_t rgb_count) {
  if (!rgb) return;
  for (size_t i = 0; i < rgb_count; ++i) TransformRGB(transform, rgb + i * 3);
}

void TransformRGBASpan(const ColorTransform &transform, float *rgba,
                       size_t rgba_count) {
  if (!rgba) return;
  for (size_t i = 0; i < rgba_count; ++i) TransformRGB(transform, rgba + i * 4);
}

bool IsLinear(const ColorSpaceDesc &space) {
  return space.kind == ColorSpaceKind::Color && space.gamma == 1.0f &&
         space.linear_bias == 0.0f;
}

bool IsData(const ColorSpaceDesc &space) {
  return space.kind == ColorSpaceKind::Data;
}

}  // namespace color
}  // namespace lightusd
