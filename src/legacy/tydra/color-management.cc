// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "color-management.hh"

#include "core/model-scope.hh"
#include "pprint-enum.hh"
#include "scene-access.hh"
#include "stage.hh"

#include <set>
#include <vector>

namespace lightusd {
namespace tydra {
namespace color_management {
namespace {

void Append(std::string *dst, const std::string &message) {
  if (!dst) return;
  if (!dst->empty() && dst->back() != '\n') *dst += '\n';
  *dst += message;
}

bool GetPrimAtPath(const Stage &stage, const Path &path, const Prim **out) {
  if (!out || !path.is_valid() || path.prim_part().empty() ||
      path.prim_part() == "/") return false;
  auto result = stage.GetPrimAtPath(Path(std::string(path.prim_part()), ""));
  if (!result || !result.value()) return false;
  *out = result.value();
  return true;
}

bool GetPropertyOptional(const Prim &prim, const std::string &name,
                         Property *out) {
  if (!out) return false;
#define GET_PLACEHOLDER_PROPERTY(type)                                    \
  if (const type *p = prim.as<type>()) {                                  \
    const auto it = p->props.find(name);                                  \
    if (it == p->props.end()) return false;                               \
    *out = it->second;                                                    \
    return true;                                                          \
  }
  GET_PLACEHOLDER_PROPERTY(RenderSettings)
  GET_PLACEHOLDER_PROPERTY(RenderProduct)
  GET_PLACEHOLDER_PROPERTY(RenderVar)
  GET_PLACEHOLDER_PROPERTY(GenerativeProcedural)
#undef GET_PLACEHOLDER_PROPERTY
  std::string ignored;
  return GetProperty(prim, name, out, &ignored);
}

bool GetAttribute(const Prim &prim, const std::string &name,
                  Attribute *out) {
  Property prop;
  if (!out || !GetPropertyOptional(prim, name, &prop) ||
      !prop.is_attribute()) return false;
  *out = prop.get_attribute();
  return true;
}

bool GetStringLike(const Prim &prim, const std::string &name,
                   std::string *out) {
  Attribute attr;
  if (!out || !GetAttribute(prim, name, &attr) || !attr.has_value()) {
    return false;
  }
  if (auto v = attr.get_value<value::token>()) {
    *out = v->str();
    return true;
  }
  if (auto v = attr.get_value<std::string>()) {
    *out = *v;
    return true;
  }
  if (auto v = attr.get_value<value::StringData>()) {
    *out = v->value;
    return true;
  }
  return false;
}

bool GetFloat(const Prim &prim, const std::string &name, float fallback,
              float *out) {
  if (!out) return false;
  Attribute attr;
  if (!GetAttribute(prim, name, &attr) || !attr.has_value()) {
    *out = fallback;
    return true;
  }
  if (auto v = attr.get_value<float>()) {
    *out = *v;
    return true;
  }
  if (auto v = attr.get_value<double>()) {
    *out = static_cast<float>(*v);
    return true;
  }
  return false;
}

bool GetFloat2(const Prim &prim, const std::string &name,
               const float fallback[2], float out[2]) {
  if (!out) return false;
  Attribute attr;
  if (!GetAttribute(prim, name, &attr) || !attr.has_value()) {
    out[0] = fallback[0];
    out[1] = fallback[1];
    return true;
  }
  if (auto v = attr.get_value<value::float2>()) {
    out[0] = (*v)[0];
    out[1] = (*v)[1];
    return true;
  }
  return false;
}

std::vector<std::string> AppliedSchemas(const Prim &prim) {
  const APISchemas schemas = prim.metas().get_apiSchemas();
  std::vector<std::string> result;
  std::set<std::string> seen;
  const auto add = [&](const std::string &name) {
    if (!name.empty() && seen.insert(name).second) result.push_back(name);
  };
  for (const auto &entry : schemas.names) {
    std::string name = to_string(entry.first);
    if (!entry.second.empty()) name += ":" + entry.second;
    add(name);
  }
  for (const auto &entry : schemas.unknownSchemas) {
    std::string name = entry.first;
    if (!entry.second.empty()) name += ":" + entry.second;
    add(name);
  }
  for (const auto &op : schemas.authoredOps) {
    if (op.first == ListEditQual::Delete) continue;
    for (const auto &entry : op.second) add(entry.first);
  }
  return result;
}

bool HasApplied(const Prim &prim, const std::string &schema) {
  const auto applied = AppliedSchemas(prim);
  for (const std::string &entry : applied) {
    if (entry == schema) return true;
  }
  return false;
}

struct DefinitionInstance {
  std::string instance;
  bool legacy{false};
};

std::vector<DefinitionInstance> DefinitionInstances(const Prim &prim) {
  std::vector<DefinitionInstance> result;
  const std::string prefix = "ColorSpaceDefinitionAPI:";
  for (const std::string &entry : AppliedSchemas(prim)) {
    if (entry.compare(0, prefix.size(), prefix) == 0 &&
        entry.size() > prefix.size()) {
      result.push_back({entry.substr(prefix.size()), false});
    } else if (entry == "ColorSpaceDefinitionAPI") {
      result.push_back({std::string(), true});
    }
  }
  return result;
}

std::string DefinitionProperty(const DefinitionInstance &instance,
                               const char *name) {
  if (instance.legacy) return name;
  return "colorSpaceDefinition:" + instance.instance + ":" + name;
}

bool ReadDefinition(const Prim &prim, const DefinitionInstance &instance,
                    const std::string &requested,
                    color::ColorSpaceDesc *out, bool *matches,
                    std::string *error) {
  if (!out || !matches) return false;
  *matches = false;
  std::string name = instance.instance.empty() ? "custom" : instance.instance;
  std::string authored_name;
  if (GetStringLike(prim, DefinitionProperty(instance, "name"),
                    &authored_name) && !authored_name.empty()) {
    name = color::CanonicalizeToken(authored_name);
  }
  if (name != requested) return true;
  *matches = true;

  const float r_default[2] = {1.0f, 0.0f};
  const float g_default[2] = {0.0f, 1.0f};
  const float b_default[2] = {0.0f, 0.0f};
  const float w_default[2] = {1.0f / 3.0f, 1.0f / 3.0f};
  float r[2], g[2], b[2], w[2], gamma = 1.0f, bias = 0.0f;
  if (!GetFloat2(prim, DefinitionProperty(instance, "redChroma"), r_default,
                 r) ||
      !GetFloat2(prim, DefinitionProperty(instance, "greenChroma"), g_default,
                 g) ||
      !GetFloat2(prim, DefinitionProperty(instance, "blueChroma"), b_default,
                 b) ||
      !GetFloat2(prim, DefinitionProperty(instance, "whitePoint"), w_default,
                 w) ||
      !GetFloat(prim, DefinitionProperty(instance, "gamma"), 1.0f, &gamma) ||
      !GetFloat(prim, DefinitionProperty(instance, "linearBias"), 0.0f,
                &bias) ||
      !color::MakeColorSpaceFromChromaticities(requested, r, g, b, w, gamma,
                                               bias, out)) {
    Append(error, "Invalid ColorSpaceDefinitionAPI for `" + requested +
                      "` at " + prim.element_path().full_path_name());
    return false;
  }
  return true;
}

template <typename Callback>
bool VisitAncestors(const Stage &stage, Path path, Callback callback) {
  if (!path.is_valid()) return false;
  path = Path(std::string(path.prim_part()), "");
  while (path.is_valid() && path.prim_part() != "/" &&
         !path.prim_part().empty()) {
    const Prim *prim = nullptr;
    if (GetPrimAtPath(stage, path, &prim) && prim && callback(*prim)) {
      return true;
    }
    const Path parent = path.get_parent_prim_path();
    if (!parent.is_valid() || parent.prim_part() == path.prim_part()) break;
    path = parent;
  }
  return false;
}

}  // namespace

bool ComputeColorSpaceName(const Stage &stage, const Path &prim_path,
                           const AttrMetas *attribute_metadata,
                           std::string *name, bool *authored) {
  if (!name) return false;
  if (authored) *authored = false;
  if (attribute_metadata && attribute_metadata->has_colorSpace()) {
    *name = color::CanonicalizeToken(
        attribute_metadata->get_colorSpace().str());
    if (authored) *authored = true;
    return true;
  }
  bool found = VisitAncestors(stage, prim_path, [&](const Prim &prim) {
    if (!HasApplied(prim, "ColorSpaceAPI")) return false;
    std::string token;
    if (!GetStringLike(prim, "colorSpace:name", &token) || token.empty()) {
      return false;
    }
    *name = color::CanonicalizeToken(token);
    return true;
  });
  if (found) {
    if (authored) *authored = true;
    return true;
  }
  *name = "lin_rec709_scene";
  return true;
}

bool ResolveColorSpaceDefinition(const Stage &stage, const Path &context_path,
                                 const std::string &name,
                                 color::ColorSpaceDesc *definition,
                                 std::string *error) {
  if (!definition) return false;
  const std::string canonical = color::CanonicalizeToken(name);
  if (color::GetBuiltinColorSpace(canonical, definition)) return true;
  bool resolved = false;
  bool failed = false;
  VisitAncestors(stage, context_path, [&](const Prim &prim) {
    int matches = 0;
    color::ColorSpaceDesc candidate;
    for (const DefinitionInstance &instance : DefinitionInstances(prim)) {
      bool match = false;
      if (!ReadDefinition(prim, instance, canonical, &candidate, &match,
                          error)) {
        failed = true;
        return true;
      }
      if (match) ++matches;
    }
    if (matches > 1) {
      Append(error, "Ambiguous ColorSpaceDefinitionAPI `" + canonical +
                        "` at " + prim.element_path().full_path_name());
      failed = true;
      return true;
    }
    if (matches == 1) {
      *definition = candidate;
      resolved = true;
      return true;
    }
    return false;
  });
  if (failed) return false;
  if (resolved) return true;
  Append(error, "Unknown color space `" + canonical + "`");
  return false;
}

bool BuildColorTransform(const Stage &stage, const Path &context_path,
                         const std::string &source,
                         const std::string &destination,
                         color::ColorTransform *transform,
                         std::string *error) {
  color::ColorSpaceDesc src, dst;
  if (!ResolveColorSpaceDefinition(stage, context_path, source, &src, error) ||
      !ResolveColorSpaceDefinition(stage, context_path, destination, &dst,
                                   error)) return false;
  if (!color::BuildColorTransform(src, dst, transform)) {
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
  config->used_override = !override_path.empty();
  std::string path = override_path;
  if (path.empty() && stage.metas().renderSettingsPrimPath) {
    path = stage.metas().renderSettingsPrimPath.value();
  }
  if (path.empty()) {
    color::GetBuiltinColorSpace(config->working_space,
                                &config->working_definition);
    return true;
  }

  const Prim *settings = nullptr;
  const Path settings_path(path, "");
  if (!GetPrimAtPath(stage, settings_path, &settings) || !settings ||
      !settings->is<RenderSettings>()) {
    Append(warning, "Invalid RenderSettings path `" + path +
                        "`; using lin_rec709_scene");
    color::GetBuiltinColorSpace(config->working_space,
                                &config->working_definition);
    return true;
  }
  std::string token;
  if (!GetStringLike(*settings, "renderingColorSpace", &token) ||
      token.empty()) token = "lin_rec709_scene";
  token = color::CanonicalizeToken(token);
  color::ColorSpaceDesc definition;
  std::string resolve_error;
  if (!ResolveColorSpaceDefinition(stage, settings_path, token, &definition,
                                   &resolve_error) ||
      !color::IsLinear(definition)) {
    Append(warning, "RenderSettings `" + path +
                        "` has unsupported nonlinear or unknown working space `" +
                        token + "`; using lin_rec709_scene");
    color::GetBuiltinColorSpace(config->working_space,
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
}  // namespace tydra
}  // namespace lightusd
