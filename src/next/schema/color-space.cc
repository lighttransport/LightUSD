// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "color-space.hh"
#include "../layer/prim-spec.hh"

#include <cmath>
#include <vector>

namespace lightusd {
namespace next {
namespace color_management {
namespace {

void Append(std::string *dst, const std::string &message) {
  if (!dst) return;
  if (!dst->empty() && dst->back() != '\n') *dst += '\n';
  *dst += message;
}

bool HasApplied(const UsdPrim &prim, const std::string &schema) {
  for (const std::string &entry : prim.GetMeta().apiSchemas()) {
    if (entry == schema) return true;
  }
  return false;
}

bool GetStringLike(const UsdPrim &prim, const std::string &property,
                   std::string *out) {
  if (!out) return false;
  const Value *v = prim.GetPropertyValue(property);
  if (!v) return false;
  if (const std::string *s = v->as_token()) {
    *out = *s;
    return true;
  }
  if (const std::string *s = v->as_string()) {
    *out = *s;
    return true;
  }
  return false;
}

bool GetFloat(const UsdPrim &prim, const std::string &property,
              float fallback, float *out) {
  if (!out) return false;
  const Value *v = prim.GetPropertyValue(property);
  if (!v) {
    *out = fallback;
    return true;
  }
  if (const float *f = v->as_float()) {
    *out = *f;
    return true;
  }
  if (const double *d = v->as_double()) {
    *out = static_cast<float>(*d);
    return true;
  }
  return false;
}

bool GetFloat2(const UsdPrim &prim, const std::string &property,
               const float fallback[2], float out[2]) {
  if (!out) return false;
  const Value *v = prim.GetPropertyValue(property);
  if (!v) {
    out[0] = fallback[0]; out[1] = fallback[1];
    return true;
  }
  if (const float *f = v->as_float2()) {
    out[0] = f[0]; out[1] = f[1];
    return true;
  }
  return false;
}

struct DefinitionInstance {
  std::string instance;
  bool legacy = false;
};

std::vector<DefinitionInstance> DefinitionInstances(const UsdPrim &prim) {
  std::vector<DefinitionInstance> result;
  const std::string prefix = "ColorSpaceDefinitionAPI:";
  for (const std::string &entry : prim.GetMeta().apiSchemas()) {
    if (entry.compare(0, prefix.size(), prefix) == 0 &&
        entry.size() > prefix.size()) {
      result.push_back({entry.substr(prefix.size()), false});
    } else if (entry == "ColorSpaceDefinitionAPI") {
      result.push_back({std::string(), true});
    }
  }
  return result;
}

std::string DefProp(const DefinitionInstance &instance, const char *name) {
  if (instance.legacy) return name;
  return "colorSpaceDefinition:" + instance.instance + ":" + name;
}

bool ReadDefinition(const UsdPrim &prim, const DefinitionInstance &instance,
                    const std::string &requested, ColorSpaceDesc *out,
                    bool *matches, std::string *error) {
  *matches = false;
  std::string name = instance.instance.empty() ? "custom" : instance.instance;
  std::string authored_name;
  if (GetStringLike(prim, DefProp(instance, "name"), &authored_name) &&
      !authored_name.empty()) name = authored_name;
  if (name != requested) return true;
  *matches = true;

  const float r_default[2] = {1.0f, 0.0f};
  const float g_default[2] = {0.0f, 1.0f};
  const float b_default[2] = {0.0f, 0.0f};
  const float w_default[2] = {1.0f / 3.0f, 1.0f / 3.0f};
  float r[2], g[2], b[2], w[2], gamma = 1.0f, bias = 0.0f;
  if (!GetFloat2(prim, DefProp(instance, "redChroma"), r_default, r) ||
      !GetFloat2(prim, DefProp(instance, "greenChroma"), g_default, g) ||
      !GetFloat2(prim, DefProp(instance, "blueChroma"), b_default, b) ||
      !GetFloat2(prim, DefProp(instance, "whitePoint"), w_default, w) ||
      !GetFloat(prim, DefProp(instance, "gamma"), 1.0f, &gamma) ||
      !GetFloat(prim, DefProp(instance, "linearBias"), 0.0f, &bias) ||
      !::lightusd::color::MakeColorSpaceFromChromaticities(
          requested, r, g, b, w, gamma, bias, out)) {
    Append(error, "Invalid ColorSpaceDefinitionAPI for `" + requested +
                      "` at " + prim.GetPath().str());
    return false;
  }
  return true;
}

}  // namespace

bool ComputeColorSpaceName(const UsdPrim &prim, const std::string &property,
                           std::string *name, bool *authored) {
  if (!name || !prim.IsValid()) return false;
  if (authored) *authored = false;
  if (!property.empty()) {
    if (const PropMeta *meta = prim.GetPropertyMeta(property)) {
      if ((meta->authored & PropMeta::kColorSpace) && !meta->colorSpace.empty()) {
        *name = ::lightusd::color::CanonicalizeToken(meta->colorSpace);
        if (authored) *authored = true;
        return true;
      }
    }
  }
  for (UsdPrim current = prim; current.IsValid(); current = current.GetParent()) {
    if (!HasApplied(current, "ColorSpaceAPI")) continue;
    std::string token;
    if (GetStringLike(current, "colorSpace:name", &token) && !token.empty()) {
      *name = ::lightusd::color::CanonicalizeToken(token);
      if (authored) *authored = true;
      return true;
    }
  }
  *name = "lin_rec709_scene";
  return true;
}

bool ResolveColorSpaceDefinition(const UsdPrim &context,
                                 const std::string &name,
                                 ColorSpaceDesc *definition,
                                 std::string *error) {
  if (!definition) return false;
  const std::string canonical = ::lightusd::color::CanonicalizeToken(name);
  if (::lightusd::color::GetBuiltinColorSpace(canonical, definition)) return true;
  for (UsdPrim current = context; current.IsValid(); current = current.GetParent()) {
    int matches = 0;
    ColorSpaceDesc candidate;
    for (const DefinitionInstance &instance : DefinitionInstances(current)) {
      bool match = false;
      if (!ReadDefinition(current, instance, canonical, &candidate, &match,
                          error)) return false;
      if (match) ++matches;
    }
    if (matches > 1) {
      Append(error, "Ambiguous ColorSpaceDefinitionAPI `" + canonical +
                        "` at " + current.GetPath().str());
      return false;
    }
    if (matches == 1) {
      *definition = candidate;
      return true;
    }
  }
  Append(error, "Unknown color space `" + canonical + "`");
  return false;
}

bool BuildColorTransform(const UsdPrim &context,
                         const std::string &source,
                         const std::string &destination,
                         ColorTransform *transform,
                         std::string *error) {
  ColorSpaceDesc src, dst;
  if (!ResolveColorSpaceDefinition(context, source, &src, error) ||
      !ResolveColorSpaceDefinition(context, destination, &dst, error)) {
    return false;
  }
  if (!::lightusd::color::BuildColorTransform(src, dst, transform)) {
    Append(error, "Cannot build color transform from `" + source + "` to `" +
                      destination + "`");
    return false;
  }
  return true;
}

bool ResolveRenderingColorConfig(const Stage &stage,
                                 const std::string &override_path,
                                 RenderingColorConfig *config,
                                 std::string *warning) {
  if (!config) return false;
  *config = RenderingColorConfig{};
  std::string path = override_path;
  config->used_override = !override_path.empty();
  if (path.empty() && stage.GetMeta().renderSettingsPrimPath_set) {
    path = stage.GetMeta().renderSettingsPrimPath;
  }
  if (path.empty()) {
    ::lightusd::color::GetBuiltinColorSpace(config->working_space,
                                            &config->working_definition);
    return true;
  }

  const UsdPrim settings = stage.GetPrimAtPath(path);
  if (!settings.IsValid() || settings.GetTypeName() != "RenderSettings") {
    Append(warning, "Invalid RenderSettings path `" + path +
                        "`; using lin_rec709_scene");
    ::lightusd::color::GetBuiltinColorSpace(config->working_space,
                                            &config->working_definition);
    return true;
  }
  std::string token;
  if (!GetStringLike(settings, "renderingColorSpace", &token) || token.empty()) {
    token = "lin_rec709_scene";
  }
  token = ::lightusd::color::CanonicalizeToken(token);
  ColorSpaceDesc definition;
  std::string resolve_error;
  if (!ResolveColorSpaceDefinition(settings, token, &definition, &resolve_error) ||
      !::lightusd::color::IsLinear(definition)) {
    Append(warning, "RenderSettings `" + path + "` has unsupported nonlinear "
                    "or unknown working space `" + token +
                    "`; using lin_rec709_scene");
    ::lightusd::color::GetBuiltinColorSpace(config->working_space,
                                            &config->working_definition);
    return true;
  }
  config->render_settings_path = path;
  config->working_space = token;
  config->working_definition = definition;
  config->used_fallback = false;
  return true;
}

}  // namespace color_management
}  // namespace next
}  // namespace lightusd
