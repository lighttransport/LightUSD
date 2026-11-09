// SPDX-License-Identifier: Apache-2.0
// ARKit / RealityKit USDZ delivery-profile validation rules, extracted from
// usd-validation.cc for compile-time parallelism. Part of the
// validation_detail namespace shared through usd-validation-internal.hh.

#include "usd-validation-internal.hh"

#include "../composition/composition.hh"


#include <algorithm>
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
namespace next {
namespace validation_detail {

// arkit rules -- the ARKit / RealityKit USDZ delivery profile, modeled on
// OpenUSD's `usdchecker --arkit` (pxr/usd/usdUtils/complianceChecker.py; see
// doc/openusd-usdz.md for the rule inventory this mirrors).
//
// NOTE (structural approximations, in the spirit of the rest of this file):
//  - OpenUSD checks a *composed* stage. These rules run on the uncomposed
//    Layer, so cross-layer targets (a Material or normal-map texture brought
//    in by a reference) cannot be resolved. Target-resolution rules therefore
//    only hard-fail when the layer is self-contained (no sublayers and no
//    reference/payload asset arcs) -- which is what an ARKit-ready `.usdz`
//    root layer is required to be anyway.
//  - OpenUSD's NormalMapTextureChecker inspects the actual image bit depth.
//    We do not decode images: the 8-bit determination is made from the file
//    extension (png/jpg/jpeg/bmp/tga are treated as 8-bit; exr/hdr as float).
//  - The `package` container rules (zip layout, allowed entry extensions,
//    `.usdc` root layer) need the original bytes and are run by callers such
//    as lusdchecker under the `arkit.package.*` rule ids.
// ---------------------------------------------------------------------------

// complianceChecker.py:792 `_allowedPrimTypeNames`.
bool IsArkitAllowedPrimTypeName(const std::string &type_name) {
  if (type_name.empty()) {
    return true;  // untyped prims ("over", pure namespace parents) are allowed
  }
  if (StartsWith(type_name, "RealityKit")) {
    return true;
  }
  static const std::unordered_set<std::string> kAllowed = {
      "Scope",        "Xform",       "Camera",
      "Shader",       "Material",    "Mesh",
      "Sphere",       "Cube",        "Cylinder",
      "Cone",         "Capsule",     "GeomSubset",
      "Points",       "SkelRoot",    "Skeleton",
      "SkelAnimation", "BlendShape", "SpatialAudio",
      "PhysicsScene", "Preliminary_ReferenceImage",
      "Preliminary_Text", "Preliminary_Trigger"};
  return kAllowed.count(type_name) > 0;
}

// Lowercase file extension of an asset path (without the dot). UDIM-style
// paths ("tex.<UDIM>.png") still yield the trailing extension.
std::string LowerAssetExtension(const std::string &asset_path) {
  const size_t slash = asset_path.find_last_of("/\\");
  const size_t dot = asset_path.find_last_of('.');
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return std::string();
  }
  std::string ext = asset_path.substr(dot + 1);
  // Drop any package-relative suffix ("scene.usdz[tex.png]" is handled by the
  // caller; a trailing "]" would otherwise leak into the extension).
  const size_t bad = ext.find_first_of("[]?#");
  if (bad != std::string::npos) {
    ext = ext.substr(0, bad);
  }
  for (char &c : ext) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return ext;
}

// complianceChecker.py:232 `_basicUSDZImageFormats`.
bool IsArkitTextureExtension(const std::string &ext) {
  return ext == "exr" || ext == "jpg" || ext == "jpeg" || ext == "png";
}

// complianceChecker.py `_extsForFilesThatCanBeDecoded`: readable by the USD
// image plugins, but not portable to ARKit/RealityKit.
bool IsDecodableButNonPortableTextureExtension(const std::string &ext) {
  return ext == "bmp" || ext == "tga" || ext == "hdr" || ext == "tif" ||
         ext == "tiff" || ext == "tx" || ext == "zfile";
}

// Extension-based 8-bit determination (see NOTE above).
bool IsEightBitTextureExtension(const std::string &ext) {
  return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" ||
         ext == "tga";
}

bool IsArkitLayerExtension(const std::string &ext) {
  return ext == "usd" || ext == "usda" || ext == "usdc" || ext == "usdz";
}

// ARKitShaderChecker: `id`-implementation shaders drawn from the UsdPreview /
// MaterialX node families.
bool IsArkitShaderId(const std::string &shader_id) {
  return shader_id == "UsdPreviewSurface" || shader_id == "UsdUVTexture" ||
         shader_id == "UsdTransform2d" || IsUsdPrimvarReaderId(shader_id) ||
         StartsWith(shader_id, "UsdPrimvarReader") ||
         StartsWith(shader_id, "ND_");
}

bool ValueToFloat4(const Value &v, std::array<double, 4> *out) {
  if (!out || v.is_array()) {
    return false;
  }
  if (v.type_id() == TypeId::Float4 || v.type_id() == TypeId::Color4f) {
    const float *p = static_cast<const float *>(v.raw_data());
    if (!p) return false;
    for (size_t i = 0; i < 4; i++) (*out)[i] = static_cast<double>(p[i]);
    return true;
  }
  if (v.type_id() == TypeId::Double4) {
    const double *p = static_cast<const double *>(v.raw_data());
    if (!p) return false;
    for (size_t i = 0; i < 4; i++) (*out)[i] = p[i];
    return true;
  }
  return false;
}

bool GetFloat4Property(const PrimSpec &ps, const std::string &name,
                       std::array<double, 4> *out) {
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  return ValueToFloat4(*v, out);
}

bool Float4Equals(const std::array<double, 4> &v, double x, double y, double z,
                  double w) {
  const double kEps = 1.0e-4;
  return std::fabs(v[0] - x) <= kEps && std::fabs(v[1] - y) <= kEps &&
         std::fabs(v[2] - z) <= kEps && std::fabs(v[3] - w) <= kEps;
}

std::string FormatFloat4(const std::array<double, 4> &v) {
  std::ostringstream ss;
  ss << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
  return ss.str();
}

// arkit.texture.format: every asset-path-valued shader input.
void ValidateArkitTextureFormats(const PrimSpec &ps,
                                 const std::string &prim_location,
                                 USDValidationResult *result) {
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    if (!IsShaderInputName(prop_name) || IsRelationshipProp(ps, prop_name)) {
      continue;
    }
    std::string asset_path;
    if (!GetAssetPathProperty(ps, prop_name, &asset_path) ||
        asset_path.empty()) {
      continue;
    }
    const std::string location = MakePropertyLocation(prim_location, prop_name);
    const std::string ext = LowerAssetExtension(asset_path);
    if (IsArkitTextureExtension(ext)) {
      continue;
    }
    if (IsDecodableButNonPortableTextureExtension(ext)) {
      AddError(result, "arkit.texture.format", location,
               "texture `" + asset_path + "` has extension `" + ext +
                   "`, which is decodable by USD but not portable to ARKit; "
                   "convert it to exr, jpg, jpeg, or png");
    } else {
      AddError(result, "arkit.texture.format", location,
               "texture `" + asset_path + "` has unsupported extension `" +
                   ext + "`; ARKit allows only exr, jpg, jpeg, and png");
    }
  }
}

// An 8-bit UsdUVTexture feeding a UsdPreviewSurface `inputs:normal` must
// remap [0,1] to [-1,1] and stay raw. Shared by the always-on shade rule
// (`shade.normalMap.scaleBias`, warning -- OpenUSD's NormalMapTextureValidator)
// and the ARKit profile (`arkit.normalMap.scaleBias`, error).
void ValidateNormalMapTextureImpl(const Layer &layer, const PrimSpec &surface,
                                  const std::string &surface_location,
                                  const char *rule_id,
                                  USDValidationSeverity severity,
                                  USDValidationResult *result) {
  const std::vector<Path> *conns = surface.connection("inputs:normal");
  if (!conns) {
    return;
  }
  for (const Path &conn : *conns) {
    std::string prim_part;
    SplitScenePathString(conn.str(), &prim_part, nullptr);
    const PrimSpec *tex = layer.prim_at_path(prim_part);
    if (!tex || tex->type_name() != "Shader" ||
        GetShaderInfoId(*tex) != "UsdUVTexture") {
      if (tex && tex->type_name() != "Shader" && tex->type_name() != "Material" && tex->type_name() != "NodeGraph")
        AddIssue(result, severity, "shade.normalMap.connection", surface_location,
                 "Normal input is connected to a non-Shader prim");
      continue;
    }
    std::string file;
    if (!GetAssetPathProperty(*tex, "inputs:file", &file) || file.empty()) {
      AddIssue(result, severity, "shade.normalMap.file", prim_part + ".inputs:file",
               "Normal texture needs a nonempty inputs:file asset");
      continue;
    }
    if (!IsEightBitTextureExtension(LowerAssetExtension(file))) {
      continue;
    }

    const std::string scale_location =
        MakePropertyLocation(prim_part, "inputs:scale");
    std::array<double, 4> scale{};
    if (!GetFloat4Property(*tex, "inputs:scale", &scale)) {
      AddIssue(result, severity, rule_id, scale_location,
               "8-bit normal map read by `" + surface_location +
                   ".inputs:normal` must author inputs:scale = (2, 2, 2, 1)");
    } else if (!Float4Equals(scale, 2.0, 2.0, 2.0, 1.0)) {
      AddIssue(result, severity, rule_id, scale_location,
               "8-bit normal map inputs:scale must be (2, 2, 2, 1), but is " +
                   FormatFloat4(scale));
    }

    const std::string bias_location =
        MakePropertyLocation(prim_part, "inputs:bias");
    std::array<double, 4> bias{};
    if (!GetFloat4Property(*tex, "inputs:bias", &bias)) {
      AddIssue(result, severity, rule_id, bias_location,
               "8-bit normal map read by `" + surface_location +
                   ".inputs:normal` must author inputs:bias = (-1, -1, -1, 0)");
    } else if (!Float4Equals(bias, -1.0, -1.0, -1.0, 0.0)) {
      AddIssue(result, severity, rule_id, bias_location,
               "8-bit normal map inputs:bias must be (-1, -1, -1, 0), but is " +
                   FormatFloat4(bias));
    }

    std::string color_space;
    const std::string cs_location =
        MakePropertyLocation(prim_part, "inputs:sourceColorSpace");
    if (!GetTokenProperty(*tex, "inputs:sourceColorSpace", &color_space)) {
      AddIssue(result, severity, rule_id, cs_location,
               "normal map read by `" + surface_location +
                   ".inputs:normal` must author "
                   "inputs:sourceColorSpace = \"raw\"");
    } else if (color_space != "raw") {
      AddIssue(result, severity, rule_id, cs_location,
               "normal map inputs:sourceColorSpace must be `raw`, but is `" +
                   color_space + "`");
    }
  }
}

// arkit.shader.*
void ValidateArkitShader(const Layer &layer, const PrimSpec &ps,
                         const std::string &prim_location,
                         USDValidationResult *result) {
  std::string impl_source;
  if (GetTokenProperty(ps, "info:implementationSource", &impl_source) &&
      impl_source != "id") {
    AddError(result, "arkit.shader.implementationSource",
             MakePropertyLocation(prim_location, "info:implementationSource"),
             "ARKit shaders must use the `id` implementation source, but `" +
                 impl_source + "` is authored");
  }

  const std::string shader_id = GetShaderInfoId(ps);
  if (shader_id.empty()) {
    AddError(result, "arkit.shader.id",
             MakePropertyLocation(prim_location, "info:id"),
             "Shader must author an `info:id` token");
  } else if (!IsArkitShaderId(shader_id)) {
    AddError(result, "arkit.shader.id",
             MakePropertyLocation(prim_location, "info:id"),
             "shader id `" + shader_id +
                 "` is not supported by ARKit; allowed ids are "
                 "UsdPreviewSurface, UsdUVTexture, UsdTransform2d, "
                 "UsdPrimvarReader*, and ND_*");
  }

  // arkit.shader.connection: at most one connection source per input.
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    if (!IsShaderInputName(prop_name)) {
      continue;
    }
    const std::vector<Path> *conns = ps.connection(prop_name);
    if (conns && conns->size() > 1) {
      AddError(result, "arkit.shader.connection",
               MakePropertyLocation(prim_location, prop_name),
               "ARKit allows at most one connection per shader input, but `" +
                   prop_name + "` has " + std::to_string(conns->size()));
    }
  }

  ValidateArkitTextureFormats(ps, prim_location, result);

  if (shader_id == "UsdPreviewSurface") {
    ValidateNormalMapTextureImpl(layer, ps, prim_location,
                                 "arkit.normalMap.scaleBias",
                                 USDValidationSeverity::Error, result);
  }
}

// arkit.material.binding: direct and collection-based bindings must resolve to
// an existing Material prim.
void ValidateArkitMaterialBinding(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  const PrimTypeByPath &prim_types,
                                  bool layer_self_contained,
                                  USDValidationResult *result) {
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    if (prop_name != "material:binding" &&
        !StartsWith(prop_name, "material:binding:")) {
      continue;
    }
    if (!IsRelationshipProp(ps, prop_name)) {
      // Reported as an error by shade.material.binding.
      continue;
    }
    const std::string location = MakePropertyLocation(prim_location, prop_name);
    const bool is_collection =
        StartsWith(prop_name, "material:binding:collection:");
    const std::vector<Path> &targets = RelTargets(ps, prop_name);
    if (targets.empty()) {
      AddError(result, "arkit.material.binding", location,
               "material binding relationship has no target");
      continue;
    }
    bool found_material = false;
    for (const Path &target : targets) {
      std::string prim_part;
      std::string prop_part;
      SplitScenePathString(target.str(), &prim_part, &prop_part);
      if (!prop_part.empty()) {
        // The collection property target of a collection-based binding.
        if (!is_collection) {
          AddError(result, "arkit.material.binding", location,
                   "direct material binding target `" + target.str() +
                       "` must be a Material prim path, not a property path");
        }
        continue;
      }
      const std::string *target_type = FindPrimType(prim_types, prim_part);
      if (!target_type) {
        if (layer_self_contained) {
          AddError(result, "arkit.material.binding", location,
                   "material binding target `" + prim_part +
                       "` does not resolve to a prim in this layer");
        }
        continue;
      }
      if (*target_type != "Material") {
        AddError(result, "arkit.material.binding", location,
                 "material binding target `" + prim_part + "` is a `" +
                     *target_type + "`, not a Material");
        continue;
      }
      found_material = true;
    }
    if (is_collection && !found_material && layer_self_contained) {
      AddError(result, "arkit.material.binding", location,
               "collection-based material binding does not target a Material "
               "prim");
    }
  }
}

// arkit.layer.extension: reference / payload asset paths (sublayers are
// checked at layer scope).
void ValidateArkitCompositionAssetPaths(const PrimSpec &ps,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  const PrimMetadata &m = ps.meta();
  const ArcListOps *edits = m.arc_edits();
  const auto check = [&](const std::vector<std::string> &arcs,
                         const char *what) {
    for (const std::string &encoded : arcs) {
      const CompositionArc arc = Compositor::ParseReference(encoded);
      if (arc.asset_path.empty()) {
        continue;
      }
      const std::string ext = LowerAssetExtension(arc.asset_path);
      if (!IsArkitLayerExtension(ext)) {
        AddError(result, "arkit.layer.extension", prim_location,
                 std::string(what) + " asset `" + arc.asset_path +
                     "` must be a usd, usda, usdc, or usdz layer");
      }
    }
  };
  check(GatherArcStrings(m.references, edits ? &edits->references : nullptr),
        "reference");
  check(GatherArcStrings(m.payloads, edits ? &edits->payloads : nullptr),
        "payload");
}

void ValidateArkitPrim(const Layer &layer, const PrimSpec &ps,
                       const std::string &prim_location,
                       const PrimTypeByPath &prim_types,
                       bool layer_self_contained,
                       USDValidationResult *result) {
  const std::string &type_name = ps.type_name();
  if (!IsArkitAllowedPrimTypeName(type_name)) {
    AddError(result, "arkit.prim.type", prim_location,
             "prim type `" + type_name +
                 "` is not in the ARKit-supported prim type set");
  }

  if (type_name == "Shader") {
    ValidateArkitShader(layer, ps, prim_location, result);
  }

  ValidateArkitMaterialBinding(ps, prim_location, prim_types,
                               layer_self_contained, result);
  ValidateArkitCompositionAssetPaths(ps, prim_location, result);
}

// arkit.stage.* / arkit.layer.extension at layer scope.
void ValidateArkitLayerMetas(const Layer &layer, USDValidationResult *result) {
  const LayerMeta &metas = layer.meta();

  if (!metas.upAxis_set) {
    AddError(result, "arkit.stage.upAxis", kLayerLocation,
             "ARKit assets must author upAxis = \"Y\"");
  } else if (metas.upAxis != "Y") {
    AddError(result, "arkit.stage.upAxis", kLayerLocation,
             "ARKit assets must be Y-up, but upAxis is `" + metas.upAxis + "`");
  }

  if (!metas.metersPerUnit_set) {
    AddError(result, "arkit.stage.metersPerUnit", kLayerLocation,
             "ARKit assets must author metersPerUnit");
  }

  if (metas.defaultPrim.empty()) {
    AddError(result, "arkit.stage.defaultPrim", kLayerLocation,
             "ARKit assets must author a defaultPrim");
  }

  for (size_t i = 0; i < metas.subLayers.size(); i++) {
    const std::string &sublayer = metas.subLayers[i];
    if (sublayer.empty()) {
      continue;  // reported by core.layer.subLayers
    }
    const std::string ext = LowerAssetExtension(sublayer);
    if (!IsArkitLayerExtension(ext)) {
      AddError(result, "arkit.layer.extension",
               std::string(kLayerLocation) + ".subLayers[" +
                   std::to_string(i) + "]",
               "subLayer asset `" + sublayer +
                   "` must be a usd, usda, usdc, or usdz layer");
    }
  }
}

}  // namespace validation_detail
}  // namespace next
}  // namespace lightusd
