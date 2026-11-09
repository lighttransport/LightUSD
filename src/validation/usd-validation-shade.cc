// SPDX-License-Identifier: Apache-2.0
// UsdPreviewSurface / UsdUVTexture / UsdPrimvarReader / MaterialX shade
// validation rules, extracted from usd-validation.cc for compile-time
// parallelism. Part of the validation_detail namespace shared through
// usd-validation-internal.hh.

#include "usd-validation-internal.hh"

#include "core/color-transform.hh"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lightusd {
namespace validation_detail {

// ---------------------------------------------------------------------------
// shade rules
// ---------------------------------------------------------------------------

std::string GetShaderInfoId(const PrimSpec &ps) {
  std::string id;
  (void)GetTokenProperty(ps, "info:id", &id);
  return id;
}

void ValidateUsdPreviewSurface(const PrimSpec &ps,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  static const std::string kInputsPrefix = "inputs:";
  const auto &inputs = UsdPreviewSurfaceInputTypes();

  for (const PropSlot &slot : ps.properties().slots()) {
    if (slot.is_relationship()) {
      continue;
    }
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (!StartsWith(prop_name, kInputsPrefix)) {
      continue;
    }
    const std::string input_name = prop_name.substr(kInputsPrefix.size());
    const std::string prop_location =
        MakePropertyLocation(prim_location, prop_name);

    auto it = inputs.find(input_name);
    if (it == inputs.end()) {
      AddWarning(result, "shade.preview.unknownInput", prop_location,
                 "`" + prop_name + "` is not a UsdPreviewSurface input");
      continue;
    }

    const std::string authored = AttrTypeNameOf(ps, prop_name);
    if (!authored.empty() && !ValueTypesAgree(authored, it->second)) {
      AddWarning(result, "shade.preview.inputType", prop_location,
                 "UsdPreviewSurface input `" + input_name + "` should be `" +
                     it->second + "` but is `" + authored + "`");
    } else if (!authored.empty() && authored != it->second) {
      // Same storage, different declared spelling (float3 vs color3f).
      // OpenUSD's ShaderSdrCompliance flags this as MismatchedPropertyType:
      // the sdr registry type is exact, not role-tolerant.
      AddWarning(result, "shade.shader.typeMismatch", prop_location,
                 "UsdPreviewSurface input `" + input_name +
                     "` is declared `" + authored +
                     "` but the shader registry type is `" + it->second + "`");
    }
  }

  for (const char *prop_name :
       {"inputs:roughness", "inputs:metallic", "inputs:clearcoat",
        "inputs:clearcoatRoughness", "inputs:opacity", "inputs:occlusion"}) {
    double value = 0.0;
    if (GetNumericScalarProperty(ps, prop_name, &value) &&
        (value < 0.0 || value > 1.0)) {
      AddWarning(result, "shade.preview.inputRange",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) +
                     " should be in range [0, 1], but is " +
                     std::to_string(value));
    }
  }

  double ior = 0.0;
  if (GetNumericScalarProperty(ps, "inputs:ior", &ior) && !(ior > 0.0)) {
    AddWarning(result, "shade.preview.inputRange",
               MakePropertyLocation(prim_location, "inputs:ior"),
               "inputs:ior should be greater than 0, but is " +
                   std::to_string(ior));
  }

  std::string opacity_mode;
  if (GetTokenProperty(ps, "inputs:opacityMode", &opacity_mode) &&
      opacity_mode != "opacity" && opacity_mode != "transparent" &&
      opacity_mode != "presence") {
    AddWarning(result, "shade.preview.opacityMode",
               MakePropertyLocation(prim_location, "inputs:opacityMode"),
               "invalid UsdPreviewSurface opacityMode `" + opacity_mode + "`");
  }
}

bool IsUsdUVTextureWrapToken(const std::string &token) {
  return token == "useMetadata" || token == "black" || token == "clamp" ||
         token == "repeat" || token == "mirror";
}

bool IsUsdUVTextureColorSpaceToken(const std::string &token) {
  return token == "auto" || token == "raw" || token == "sRGB";
}

void ValidateUsdUVTexture(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result) {
  std::string file;
  if (!HasAttributeProp(ps, "inputs:file")) {
    AddWarning(result, "shade.uvTexture.file",
               MakePropertyLocation(prim_location, "inputs:file"),
               "UsdUVTexture should author inputs:file");
  } else if (!HasConnections(ps, "inputs:file") &&
             (!GetAssetPathProperty(ps, "inputs:file", &file) ||
              file.empty())) {
    AddWarning(result, "shade.uvTexture.file",
               MakePropertyLocation(prim_location, "inputs:file"),
               "UsdUVTexture inputs:file should be a non-empty asset path");
  }

  for (const char *prop_name : {"inputs:wrapS", "inputs:wrapT"}) {
    std::string token;
    if (GetTokenProperty(ps, prop_name, &token) &&
        !IsUsdUVTextureWrapToken(token)) {
      AddWarning(result, "shade.uvTexture.wrap",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) + " has invalid wrap token `" + token +
                     "`");
    }
  }

  std::string color_space;
  if (GetTokenProperty(ps, "inputs:sourceColorSpace", &color_space) &&
      !IsUsdUVTextureColorSpaceToken(color_space)) {
    AddWarning(result, "shade.uvTexture.sourceColorSpace",
               MakePropertyLocation(prim_location, "inputs:sourceColorSpace"),
               "UsdUVTexture sourceColorSpace should be auto, raw, or sRGB, "
               "but is `" +
                   color_space + "`");
  }
}

void ValidateUsdPrimvarReader(const PrimSpec &ps, const std::string &shader_id,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  if (!HasAttributeProp(ps, "inputs:varname")) {
    AddWarning(result, "shade.primvarReader.varname",
               MakePropertyLocation(prim_location, "inputs:varname"),
               "UsdPrimvarReader should author inputs:varname");
    return;
  }

  const std::string varname_type = AttrTypeNameOf(ps, "inputs:varname");
  if (varname_type == "token") {
    // Accepted for evaluation (the pre-21.02 spec used token), but the sdr
    // registry type is `string` -- OpenUSD's ShaderSdrCompliance flags the
    // token spelling as MismatchedPropertyType, connected or not.
    AddWarning(result, "shade.shader.typeMismatch",
               MakePropertyLocation(prim_location, "inputs:varname"),
               "inputs:varname is declared `token` but the shader registry "
               "type is `string`");
  }
  if (HasConnections(ps, "inputs:varname")) {
    // A connected varname's VALUE resolves at composition/evaluation time.
  } else if (varname_type != "token" && varname_type != "string") {
    AddWarning(result, "shade.primvarReader.varname",
               MakePropertyLocation(prim_location, "inputs:varname"),
               "UsdPrimvarReader inputs:varname should be token or string, "
               "but is `" +
                   varname_type + "`");
  } else {
    std::string varname;
    if (!GetStringLikeProperty(ps, "inputs:varname", &varname) ||
        varname.empty()) {
      AddWarning(result, "shade.primvarReader.varname",
                 MakePropertyLocation(prim_location, "inputs:varname"),
                 "UsdPrimvarReader inputs:varname should be non-empty");
    }
  }

  const std::string *expected_type = UsdPrimvarReaderResultType(shader_id);
  if (!expected_type) {
    AddWarning(result, "shade.primvarReader.result", prim_location,
               "unknown UsdPrimvarReader shader id `" + shader_id + "`");
    return;
  }

  const std::string location =
      MakePropertyLocation(prim_location, "outputs:result");
  if (!HasProperty(ps, "outputs:result")) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " should author outputs:result");
    return;
  }
  if (IsRelationshipProp(ps, "outputs:result")) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " outputs:result should be an attribute");
    return;
  }

  const std::string result_type = AttrTypeNameOf(ps, "outputs:result");
  if (!ValueTypesAgree(result_type, *expected_type)) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " outputs:result should be " + *expected_type +
                   ", but is `" + result_type + "`");
  }
}

bool HasAppliedSchema(const std::vector<AppliedSchema> &schemas,
                      const std::string &schema_name) {
  for (const auto &schema : schemas) {
    if (schema.name == schema_name) {
      return true;
    }
  }
  return false;
}

void ValidateMaterialXConfig(const PrimSpec &ps,
                             const std::vector<AppliedSchema> &applied_schemas,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  bool has_config_property = false;
  for (const PropSlot &slot : ps.properties().slots()) {
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (StartsWith(prop_name, "config:mtlx:")) {
      has_config_property = true;
      break;
    }
  }

  const bool has_config_api =
      HasAppliedSchema(applied_schemas, "MaterialXConfigAPI");
  const bool has_mtlx_terminal =
      HasProperty(ps, "outputs:mtlx:surface") ||
      HasProperty(ps, "outputs:mtlx:displacement") ||
      HasProperty(ps, "outputs:mtlx:volume");
  if ((has_config_property || has_mtlx_terminal) && !has_config_api) {
    AddWarning(result, "shade.materialX.configAPI", prim_location,
               "MaterialX properties are authored without applying "
               "MaterialXConfigAPI");
  }

  if (has_config_api && ps.type_name() != "Material") {
    AddWarning(result, "shade.materialX.configAPI", prim_location,
               "MaterialXConfigAPI is expected on Material prims");
  }

  std::string version;
  if (HasProperty(ps, "config:mtlx:version")) {
    const std::string location =
        MakePropertyLocation(prim_location, "config:mtlx:version");
    if (IsRelationshipProp(ps, "config:mtlx:version") ||
        AttrTypeNameOf(ps, "config:mtlx:version") != "string" ||
        !GetStringProperty(ps, "config:mtlx:version", &version)) {
      AddError(result, "shade.materialX.version", location,
               "config:mtlx:version must be a string attribute");
    } else if (version.empty() || !StartsWith(version, "1.")) {
      AddWarning(result, "shade.materialX.version", location,
                 "MaterialX version should be a 1.x version string, but is `" +
                     version + "`");
    }
  }

  std::string source_uri;
  if ((GetStringProperty(ps, "config:mtlx:sourceUri", &source_uri) ||
       GetAssetPathProperty(ps, "config:mtlx:sourceUri", &source_uri)) &&
      !source_uri.empty() && !IsMaterialXAssetPath(source_uri)) {
    AddWarning(result, "shade.materialX.sourceUri",
               MakePropertyLocation(prim_location, "config:mtlx:sourceUri"),
               "MaterialX sourceUri should usually reference a .mtlx asset");
  }
}

void ValidateMaterialXShader(const PrimSpec &ps, const std::string &shader_id,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  if (!IsMaterialXShaderId(shader_id)) {
    return;
  }

  bool has_output = false;
  for (const PropSlot &slot : ps.properties().slots()) {
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (StartsWith(prop_name, "outputs:")) {
      has_output = true;
      break;
    }
  }
  if (!has_output) {
    AddWarning(result, "shade.materialX.output", prim_location,
               "MaterialX shader `" + shader_id +
                   "` should author at least one outputs:* port");
  }
}

void ValidateMaterialXSynthesizedMaterial(
    const PrimSpec &ps, const std::string &prim_location,
    const PrimTypeByPath &prim_types, USDValidationResult *result) {
  if (ps.type_name() != "Material" ||
      !StartsWith(prim_location, "/MaterialX/Materials/")) {
    return;
  }

  bool has_shader = false;
  for (const char *rel_name : {"mtlx:surface:source",
                               "mtlx:displacement:source",
                               "mtlx:volume:source"}) {
    const std::string relationship_name(rel_name);
    if (!HasProperty(ps, relationship_name)) {
      continue;
    }
    has_shader = true;
    const std::string location =
        MakePropertyLocation(prim_location, relationship_name);
    if (!IsRelationshipProp(ps, relationship_name)) {
      AddError(result, "shade.materialX.nodeReference", location,
               relationship_name + " must be a relationship");
      continue;
    }
    ValidateRelationshipTargetPaths(ps, relationship_name,
                                    "shade.materialX.nodeReference", location,
                                    result);
    for (const Path &target : RelTargets(ps, relationship_name)) {
      std::string target_prim;
      SplitScenePathString(target.str(), &target_prim, nullptr);
      const std::string *target_type = FindPrimType(prim_types, target_prim);
      if (!target_type) {
        AddError(result, "shade.materialX.nodeReference", location,
                 "MaterialX shader target `" + target_prim +
                     "` does not exist");
      } else if (*target_type != "Shader" && *target_type != "NodeGraph") {
        AddError(result, "shade.materialX.nodeReference", location,
                 "MaterialX shader target `" + target_prim + "` has type `" +
                     *target_type + "`");
      }
    }
  }
  if (!has_shader) {
    AddError(result, "shade.materialX.nodeReference", prim_location,
             "MaterialX material has no surface, displacement, or volume "
             "shader reference");
  }
}

// ---------------------------------------------------------------------------
// lux rules
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Dictionary metadata helpers (::lightusd::Dict)
// ---------------------------------------------------------------------------

bool GetMetaDouble(DictView dict, const std::string &key, double *out) {
  auto v = dict.find(key);
  if (!v || !out) {
    return false;
  }
  return ValueToDouble(*v, out);
}

bool GetMetaAssetPath(DictView dict, const std::string &key,
                      std::string *out) {
  auto v = dict.find(key);
  if (!v || !out) {
    return false;
  }
  if (auto asset = v->as_asset_path()) {
    *out = *asset;
    return true;
  }
  if (auto str = v->as_string()) {
    *out = *str;
    return true;
  }
  return false;
}

bool GetMetaString(DictView dict, const std::string &key,
                   std::string *out) {
  auto v = dict.find(key);
  if (!v || !out) {
    return false;
  }
  if (auto str = v->as_string()) {
    *out = *str;
    return true;
  }
  if (auto tok = v->as_token()) {
    *out = *tok;
    return true;
  }
  return false;
}

bool GetMetaStringArray(DictView dict, const std::string &key,
                        std::vector<std::string> *out) {
  auto v = dict.find(key);
  if (!v || !out || !v->is_array()) {
    return false;
  }
  auto arr = v->as_token_array();
  if (!arr) {
    return false;
  }
  out->assign(arr->begin(), arr->end());
  return true;
}

bool GetMetaDouble2Array(DictView dict, const std::string &key,
                         std::vector<std::array<double, 2>> *out) {
  auto v = dict.find(key);
  if (!v || !out || !v->is_array() || v->type_id() != TypeId::Double2) {
    return false;
  }
  auto flat = v->as_double_array();
  if (!flat) {
    return false;
  }
  out->clear();
  for (size_t i = 0; i + 1 < flat->size(); i += 2) {
    out->push_back({{(*flat)[i], (*flat)[i + 1]}});
  }
  return true;
}



}  // namespace validation_detail
}  // namespace lightusd
