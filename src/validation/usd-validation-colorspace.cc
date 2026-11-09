// SPDX-License-Identifier: Apache-2.0
#include "usd-validation-internal.hh"
#include "core/color-transform.hh"

namespace lightusd {
namespace validation_detail {

// ---------------------------------------------------------------------------
// ColorSpace / CollectionAPI schema rules
// ---------------------------------------------------------------------------

bool IsCanonicalColorSpaceToken(const std::string &token) {
  static const std::unordered_set<std::string> kCanonicalTokens = {
      "lin_ap1_scene",     "lin_ap0_scene",      "lin_rec709_scene",
      "lin_p3d65_scene",   "lin_rec2020_scene",  "lin_adobergb_scene",
      "lin_ciexyzd65_scene", "srgb_rec709_scene", "g22_rec709_scene",
      "g18_rec709_scene",  "srgb_ap1_scene",     "g22_ap1_scene",
      "srgb_p3d65_scene",  "g22_adobergb_scene", "data",
      "unknown",           "raw",                "identity",
  };
  return kCanonicalTokens.count(token) > 0;
}

bool IsColorSpaceDefinitionPropertyName(const std::string &prop_name) {
  return prop_name == "name" || prop_name == "redChroma" ||
         prop_name == "greenChroma" || prop_name == "blueChroma" ||
         prop_name == "whitePoint" || prop_name == "gamma" ||
         prop_name == "linearBias" ||
         prop_name == "colorSpaceDefinition:name" ||
         prop_name == "colorSpaceDefinition:redChroma" ||
         prop_name == "colorSpaceDefinition:greenChroma" ||
         prop_name == "colorSpaceDefinition:blueChroma" ||
         prop_name == "colorSpaceDefinition:whitePoint" ||
         prop_name == "colorSpaceDefinition:gamma" ||
         prop_name == "colorSpaceDefinition:linearBias";
}

bool ParseColorSpaceDefinitionPropertyName(const std::string &prop_name,
                                           std::string *instance,
                                           std::string *field) {
  const std::vector<std::string> segments = SplitString(prop_name, ':');
  if (segments.size() == 3 && segments[0] == "colorSpaceDefinition" &&
      !segments[1].empty() && !segments[2].empty()) {
    if (instance) *instance = segments[1];
    if (field) *field = segments[2];
    return IsColorSpaceDefinitionPropertyName(segments[2]);
  }
  return false;
}

ColorSpaceSet CollectLocalColorSpaceDefinitions(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas) {
  ColorSpaceSet local_defs;
  if (!HasAppliedSchema(applied_schemas, "ColorSpaceDefinitionAPI")) {
    return local_defs;
  }
  for (const AppliedSchema &schema : applied_schemas) {
    if (schema.name != "ColorSpaceDefinitionAPI") continue;
    if (schema.instance_name.empty()) {
      // Compatibility with pre-multiple-apply AOUSD drafts.
      local_defs.insert("custom");
      for (const char *prop_name : {"name", "colorSpaceDefinition:name"}) {
        std::string value;
      if (GetTokenProperty(ps, prop_name, &value) && !value.empty()) {
        local_defs.insert(value);
        local_defs.insert(::lightusd::color::CanonicalizeToken(value));
        }
      }
      continue;
    }
    local_defs.insert(schema.instance_name);
    std::string value;
    const std::string name_prop =
        "colorSpaceDefinition:" + schema.instance_name + ":name";
    if (GetTokenProperty(ps, name_prop, &value) && !value.empty()) {
      local_defs.insert(value);
      local_defs.insert(::lightusd::color::CanonicalizeToken(value));
    }
  }
  return local_defs;
}

bool IsKnownColorSpaceToken(const std::string &token,
                            const ColorSpaceSet &visible_custom_spaces) {
  if (token.empty()) {
    return true;
  }
  const std::string canonical =
      ::lightusd::color::CanonicalizeToken(token);
  if (IsCanonicalColorSpaceToken(canonical)) {
    return true;
  }
  return visible_custom_spaces.count(token) > 0 ||
         visible_custom_spaces.count(canonical) > 0;
}

void ValidateLocalColorSpaceDefinitions(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &location, USDValidationResult *result) {
  std::map<std::string, std::string> names;
  for (const AppliedSchema &schema : applied_schemas) {
    if (schema.name != "ColorSpaceDefinitionAPI") continue;
    const bool legacy = schema.instance_name.empty();
    const std::string prefix = legacy
        ? std::string()
        : "colorSpaceDefinition:" + schema.instance_name + ":";
    std::string name = legacy ? "custom" : schema.instance_name;
    std::string authored_name;
    if (GetTokenProperty(ps, prefix + "name", &authored_name) &&
        !authored_name.empty()) {
      name = authored_name;
    }
    const std::string canonical =
        ::lightusd::color::CanonicalizeToken(name);
    auto inserted = names.emplace(canonical, schema.instance_name);
    if (!inserted.second) {
      AddError(result, "core.schema.ColorSpaceDefinitionAPI.duplicate",
               location, "multiple definitions resolve to color space `" +
                             canonical + "`");
    }

    float red[2] = {1.0f, 0.0f};
    float green[2] = {0.0f, 1.0f};
    float blue[2] = {0.0f, 0.0f};
    float white[2] = {1.0f / 3.0f, 1.0f / 3.0f};
    const auto read_float2 = [&](const std::string &property, float out[2]) {
      const Value *value = GetAttrValue(ps, property);
      if (!value) return;
      if (const float *lanes = value->as_float2()) {
        out[0] = lanes[0];
        out[1] = lanes[1];
      }
    };
    read_float2(prefix + "redChroma", red);
    read_float2(prefix + "greenChroma", green);
    read_float2(prefix + "blueChroma", blue);
    read_float2(prefix + "whitePoint", white);
    double gamma = 1.0;
    double bias = 0.0;
    (void)GetNumericScalarProperty(ps, prefix + "gamma", &gamma);
    (void)GetNumericScalarProperty(ps, prefix + "linearBias", &bias);
    ::lightusd::color::ColorSpaceDesc definition;
    if (!::lightusd::color::MakeColorSpaceFromChromaticities(
            canonical, red, green, blue, white, static_cast<float>(gamma),
            static_cast<float>(bias), &definition)) {
      AddError(result, "core.schema.ColorSpaceDefinitionAPI.invalid",
               location, "definition `" + canonical +
                             "` has invalid chromaticities or transfer curve");
    }
  }
}

void ValidateColorSpaceToken(const std::string &token,
                             const ColorSpaceSet &visible_custom_spaces,
                             const std::string &rule_id,
                             const std::string &location,
                             USDValidationResult *result) {
  if (IsKnownColorSpaceToken(token, visible_custom_spaces)) {
    return;
  }
  AddError(result, rule_id, location,
           "uses unknown color space token `" + token +
               "` with no visible ColorSpaceDefinitionAPI definition");
}

void ValidateColorSpaceDefinitionProperty(const PrimSpec &ps,
                                          const std::string &prop_name,
                                          const std::string &location,
                                          USDValidationResult *result) {
  std::string instance;
  std::string field;
  const bool current_multiple_apply =
      ParseColorSpaceDefinitionPropertyName(prop_name, &instance, &field);
  const std::string &effective_name = current_multiple_apply ? field : prop_name;
  if (effective_name == "name" ||
      effective_name == "colorSpaceDefinition:name") {
    ValidateTokenAttributeType(ps, prop_name,
                               "core.schema.ColorSpaceDefinitionAPI.name",
                               location, result);
    ValidateUniformAttribute(ps, prop_name,
                             "core.schema.ColorSpaceDefinitionAPI.name",
                             location, result);
    return;
  }

  if (effective_name == "redChroma" ||
      effective_name == "colorSpaceDefinition:redChroma" ||
      effective_name == "greenChroma" ||
      effective_name == "colorSpaceDefinition:greenChroma" ||
      effective_name == "blueChroma" ||
      effective_name == "colorSpaceDefinition:blueChroma" ||
      effective_name == "whitePoint" ||
      effective_name == "colorSpaceDefinition:whitePoint") {
    ValidateExactAttributeType(
        ps, prop_name, "float2",
        "core.schema.ColorSpaceDefinitionAPI.coordinates", location, result);
    ValidateVaryingAttribute(ps, prop_name,
                             "core.schema.ColorSpaceDefinitionAPI.coordinates",
                             location, result);
    return;
  }

  if (effective_name == "gamma" ||
      effective_name == "colorSpaceDefinition:gamma" ||
      effective_name == "linearBias" ||
      effective_name == "colorSpaceDefinition:linearBias") {
    ValidateExactAttributeType(ps, prop_name, "float",
                               "core.schema.ColorSpaceDefinitionAPI.curve",
                               location, result);
    ValidateVaryingAttribute(ps, prop_name,
                             "core.schema.ColorSpaceDefinitionAPI.curve",
                             location, result);
  }

}

}  // namespace validation_detail
}  // namespace lightusd
