// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Shader/Material/NodeGraph reconstruction specializations.
// Split from prim-reconstruct.cc
//
#include "prim-reconstruct.hh"

#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "tiny-format.hh"
#include "enum-handlers.hh"
#include "prim-property-tables.hh"

#include "usdShade.hh"
#include "usdMtlx.hh"

#include "common-macros.inc"
#include "value-types.hh"

// For PUSH_ERROR_AND_RETURN
#define PushError(s) \
  if (err) { \
    (*err) = (s) + (err->empty() ? std::string() : std::string("\n")) + (*err); \
  }
#define PushWarn(s) \
  if (warn) { \
    (*warn) = (s) + (warn->empty() ? std::string() : std::string("\n")) + (*warn); \
  }

// __VA_ARGS__ does not allow empty, thus # of args must be 2+
#define PUSH_WARN_F(s, ...) PUSH_WARN(fmt::format(s, __VA_ARGS__))

namespace lightusd {
namespace prim {

[[maybe_unused]] constexpr auto kInputsVarname = "inputs:varname";
[[maybe_unused]] constexpr auto kPurpose = "purpose";

// MaterialX Validation Helpers
// ==========================================================================

namespace mtlx_validation {

// Known MaterialX node categories (from MaterialX spec)
static const std::set<std::string> &GetKnownNodeCategories() {
  static const std::set<std::string> s = {
    // Math operations
    "add", "subtract", "multiply", "divide", "power", "min", "max",
    "absval", "floor", "ceil", "round", "sqrt", "sin", "cos", "tan",
    "asin", "acos", "atan", "atan2", "exp", "log", "ln", "sign",
    "clamp", "mix", "remap", "smoothstep", "modulo", "invert",
    // Channel operations
    "extract", "combine2", "combine3", "combine4", "separate2", "separate3", "separate4",
    "swizzle", "convert", "luminance",
    // Color operations
    "hsvadjust", "saturate", "contrast", "range",
    // Vector operations
    "normalize", "magnitude", "dotproduct", "crossproduct", "rotate3d",
    "transformpoint", "transformvector", "transformnormal",
    // Geometry
    "position", "normal", "tangent", "bitangent", "texcoord", "geomcolor",
    // Texture
    "image", "tiledimage", "constant", "noise2d", "noise3d",
    "cellnoise2d", "cellnoise3d", "fractal3d", "worleynoise2d", "worleynoise3d",
    // Procedural
    "ramp4", "splitlr", "splittb", "checkerboard",
    // Surface shaders
    "open_pbr_surface", "standard_surface", "UsdPreviewSurface"
  };
  return s;
}

// Known MaterialX types
static const std::set<std::string> &GetKnownTypes() {
  static const std::set<std::string> s = {
    "float", "color3", "color4", "vector2", "vector3", "vector4",
    "matrix33", "matrix44", "string", "filename", "boolean", "integer",
    "surfaceshader", "displacementshader", "volumeshader"
  };
  return s;
}

// Check if info:id follows MaterialX naming convention: ND_<category>_<type>
// Returns true if valid or if validation is disabled
static bool ValidateInfoId(const std::string &info_id,
                           const PrimReconstructOptions &options,
                           std::string *warn,
                           std::string *err) {
  if (!options.validate_mtlx_info_id && !options.strict_mtlx_check) {
    return true;
  }

  // Check for ND_ prefix (MaterialX node definition)
  if (info_id.rfind("ND_", 0) != 0) {
    // Not a MaterialX node definition - might be UsdPreviewSurface etc.
    return true;
  }

  // Parse ND_<category>_<type>
  std::string rest = info_id.substr(3);  // Remove "ND_"
  size_t lastUnderscore = rest.rfind('_');
  if (lastUnderscore == std::string::npos || lastUnderscore == 0) {
    if (err) {
      *err = fmt::format("Invalid MaterialX info:id format: '{}'. Expected ND_<category>_<type>", info_id);
    }
    return false;
  }

  std::string category = rest.substr(0, lastUnderscore);
  std::string type = rest.substr(lastUnderscore + 1);

  // Handle multi-part categories like "convert_color3_vector3"
  // These should be parsed as category="convert_color3" type="vector3"
  // Actually, format is ND_<category>_<outputtype>, where category may include input type
  // Let's be more lenient - just check if the base category is known

  // Extract base category (first part before any type specifiers)
  size_t firstUnderscore = category.find('_');
  std::string baseCategory = (firstUnderscore != std::string::npos)
                             ? category.substr(0, firstUnderscore)
                             : category;

  if (GetKnownNodeCategories().find(baseCategory) == GetKnownNodeCategories().end()) {
    // Check full category name as well
    if (GetKnownNodeCategories().find(category) == GetKnownNodeCategories().end()) {
      if (warn) {
        *warn += fmt::format("Unknown MaterialX node category '{}' in info:id '{}'\n",
                            category, info_id);
      }
      // Don't fail - just warn for unknown categories
    }
  }

  if (GetKnownTypes().find(type) == GetKnownTypes().end()) {
    if (warn) {
      *warn += fmt::format("Unknown MaterialX type '{}' in info:id '{}'\n", type, info_id);
    }
    // Don't fail - just warn for unknown types
  }

  return true;
}

// Validate extract/combine index bounds
[[maybe_unused]]
static bool ValidateIndexBounds(const std::string &info_id,
                                int index,
                                const PrimReconstructOptions &options,
                                std::string * /* warn */,
                                std::string *err) {
  if (!options.validate_mtlx_index_bounds && !options.strict_mtlx_check) {
    return true;
  }

  int maxIndex = 2;  // Default for color3/vector3

  // Determine max index based on type
  if (info_id.find("color4") != std::string::npos ||
      info_id.find("vector4") != std::string::npos) {
    maxIndex = 3;
  } else if (info_id.find("color2") != std::string::npos ||
             info_id.find("vector2") != std::string::npos ||
             info_id.find("float2") != std::string::npos) {
    maxIndex = 1;
  }

  if (index < 0) {
    if (err) {
      *err = fmt::format("Negative index {} for extract/combine in '{}'", index, info_id);
    }
    return false;
  }

  if (index > maxIndex) {
    if (err) {
      *err = fmt::format("Index {} out of bounds (max {}) for extract/combine in '{}'",
                        index, maxIndex, info_id);
    }
    return false;
  }

  return true;
}

// Get expected type for a connection based on property name/type
[[maybe_unused]]
static std::string GetExpectedConnectionType(const std::string &propName,
                                             const value::Value &propValue) {
  (void)propValue;
  // Map common property names to expected types
  if (propName.find("color") != std::string::npos) return "color3";
  if (propName.find("normal") != std::string::npos) return "vector3";
  if (propName.find("metalness") != std::string::npos) return "float";
  if (propName.find("roughness") != std::string::npos) return "float";
  return "";  // Unknown
}

}  // namespace mtlx_validation


template <typename T>
bool ReconstructShader(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    T *out,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options);

#include "prim-reconstruct-common.inc"

#include "prim-reconstruct-shader-decls.inc"
template <>
bool ReconstructPrim<Shader>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    Shader *shader,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options)
{
  (void)spec;
  (void)properties;
  (void)options;

  bool is_generic_shader{false};
  auto info_id_prop = properties.find("info:id");
  if (info_id_prop == properties.end()) {
    // Guess MatrialX shader. info:id will be resolved by importing referenced .mtlx.
    // Treat generic Shader at the moment.
    is_generic_shader = true;
    //PUSH_ERROR_AND_RETURN("`Shader` must contain `info:id` property.");
  }

  std::string shader_type;
  if (!is_generic_shader) {
    if (info_id_prop->second.is_attribute()) {
      const Attribute &attr = info_id_prop->second.get_attribute();
      if ((attr.type_name() == value::kToken)) {
        if (auto pv = attr.get_value<value::token>()) {
          shader_type = pv.value().str();
        } else if (options.strict_shader_check) {
          PUSH_ERROR_AND_RETURN("Internal errror. `info:id` has invalid type.");
        } else {
          // Declared without a value (e.g. `uniform token info:id` with
          // no `= "..."`). Common with Omniverse MDL shaders where the
          // real identity comes from `info:implementationSource` +
          // `info:mdl:sourceAsset`. Treat as a generic ShaderNode.
          PUSH_WARN("`info:id` declared without a value on Shader; "
                    "treating as generic ShaderNode. (set "
                    "PrimReconstructOptions::strict_shader_check=true "
                    "to make this an error.)");
          is_generic_shader = true;
        }
      } else {
        PUSH_ERROR_AND_RETURN("`info:id` attribute must be `token` type.");
      }

      if (!is_generic_shader) {
        // For some corrupted? USDZ file does not have `uniform` variability.
        if (attr.variability() != Variability::Uniform) {
          PUSH_WARN("`info:id` attribute must have `uniform` variability.");
        }
      }
    } else {
      PUSH_ERROR_AND_RETURN("Invalid type or value for `info:id` property in `Shader`.");
    }

    if (!is_generic_shader) {
      DCOUT("info:id = " << shader_type);

      // Validate MaterialX info:id if validation is enabled
      if (!mtlx_validation::ValidateInfoId(shader_type, options, warn, err)) {
        PUSH_ERROR_AND_RETURN("Invalid MaterialX info:id: " << shader_type);
      }
    }
  }


  if (shader_type.compare(kUsdPreviewSurface) == 0) {
    UsdPreviewSurface surface;
    if (!ReconstructShader<UsdPreviewSurface>(spec, properties, references,
                                              &surface, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `" << kUsdPreviewSurface << "`.");
    }
    shader->info_id = kUsdPreviewSurface;
    shader->value = std::move(surface);
    DCOUT("info_id = " << shader->info_id);
  } else if (shader_type.compare(kUsdUVTexture) == 0) {
    UsdUVTexture texture;
    if (!ReconstructShader<UsdUVTexture>(spec, properties, references,
                                         &texture, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `" << kUsdUVTexture << "`.");
    }
    shader->info_id = kUsdUVTexture;
    shader->value = std::move(texture);
  } else if (shader_type.compare(kUsdPrimvarReader_int) == 0) {
    UsdPrimvarReader_int preader;
    if (!ReconstructShader<UsdPrimvarReader_int>(spec, properties, references,
                                                 &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_int << "`.");
    }
    shader->info_id = kUsdPrimvarReader_int;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdPrimvarReader_float) == 0) {
    UsdPrimvarReader_float preader;
    if (!ReconstructShader<UsdPrimvarReader_float>(spec, properties, references,
                                                   &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_float << "`.");
    }
    shader->info_id = kUsdPrimvarReader_float;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdPrimvarReader_float2) == 0) {
    UsdPrimvarReader_float2 preader;
    if (!ReconstructShader<UsdPrimvarReader_float2>(spec, properties, references,
                                                    &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_float2 << "`.");
    }
    shader->info_id = kUsdPrimvarReader_float2;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdPrimvarReader_float3) == 0) {
    UsdPrimvarReader_float3 preader;
    if (!ReconstructShader<UsdPrimvarReader_float3>(spec,properties, references,
                                                    &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_float3 << "`.");
    }
    shader->info_id = kUsdPrimvarReader_float3;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdPrimvarReader_float4) == 0) {
    UsdPrimvarReader_float4 preader;
    if (!ReconstructShader<UsdPrimvarReader_float4>(spec,properties, references,
                                                    &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_float4 << "`.");
    }
    shader->info_id = kUsdPrimvarReader_float4;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdPrimvarReader_string) == 0) {
    UsdPrimvarReader_string preader;
    if (!ReconstructShader<UsdPrimvarReader_string>(spec,properties, references,
                                                    &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_string << "`.");
    }
    shader->info_id = kUsdPrimvarReader_string;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdPrimvarReader_vector) == 0) {
    UsdPrimvarReader_vector preader;
    if (!ReconstructShader<UsdPrimvarReader_vector>(spec,properties, references,
                                                    &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_vector << "`.");
    }
    shader->info_id = kUsdPrimvarReader_vector;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdPrimvarReader_normal) == 0) {
    UsdPrimvarReader_normal preader;
    if (!ReconstructShader<UsdPrimvarReader_normal>(spec,properties, references,
                                                    &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_normal << "`.");
    }
    shader->info_id = kUsdPrimvarReader_normal;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdPrimvarReader_point) == 0) {
    UsdPrimvarReader_point preader;
    if (!ReconstructShader<UsdPrimvarReader_point>(spec,properties, references,
                                                    &preader, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdPrimvarReader_point << "`.");
    }
    shader->info_id = kUsdPrimvarReader_point;
    shader->value = std::move(preader);
  } else if (shader_type.compare(kUsdTransform2d) == 0) {
    UsdTransform2d transform;
    if (!ReconstructShader<UsdTransform2d>(spec,properties, references,
                                                    &transform, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `"
                            << kUsdTransform2d << "`.");
    }
    shader->info_id = kUsdTransform2d;
    shader->value = std::move(transform);
  } else if (shader_type.compare(kOpenPBRSurface) == 0) {
    OpenPBRSurface surface;
    if (!ReconstructShader<OpenPBRSurface>(spec, properties, references,
                                           &surface, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `" << kOpenPBRSurface << "`.");
    }
    shader->info_id = kOpenPBRSurface;
    shader->value = std::move(surface);
  } else if (shader_type.compare(kMtlxAutodeskStandardSurface) == 0) {
    MtlxAutodeskStandardSurface surface;
    if (!ReconstructShader<MtlxAutodeskStandardSurface>(spec, properties, references,
                                                         &surface, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `" << kMtlxAutodeskStandardSurface << "`.");
    }
    shader->info_id = kMtlxAutodeskStandardSurface;
    shader->value = std::move(surface);
  } else if (shader_type.compare(kNdStandardSurfaceSurfaceshader) == 0) {
    // MaterialX Standard Surface via ND_standard_surface_surfaceshader info:id
    MtlxAutodeskStandardSurface surface;
    if (!ReconstructShader<MtlxAutodeskStandardSurface>(spec, properties, references,
                                                         &surface, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `" << kNdStandardSurfaceSurfaceshader << "`.");
    }
    shader->info_id = kNdStandardSurfaceSurfaceshader;
    shader->value = std::move(surface);
  } else if (shader_type.compare(kNdOpenPbrSurfaceSurfaceshader) == 0) {
    // Blender v4.5 MaterialX OpenPBR Surface export
    MtlxOpenPBRSurface surface;
    if (!ReconstructShader<MtlxOpenPBRSurface>(spec, properties, references,
                                                &surface, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `" << kNdOpenPbrSurfaceSurfaceshader << "`.");
    }
    shader->info_id = kNdOpenPbrSurfaceSurfaceshader;
    shader->value = std::move(surface);
  } else if (shader_type.compare(kNdUsdPreviewSurfaceSurfaceshader) == 0) {
    // MaterialX's UsdPreviewSurface node (e.g. usd-wg MaterialXTest/basic_flatten):
    // info:id `ND_UsdPreviewSurface_surfaceshader` with the same inputs as
    // UsdPreviewSurface. Reconstruct into a UsdPreviewSurface value so downstream
    // (tydra render-data) handles it as one; keep the ND_ info:id for round-trip.
    UsdPreviewSurface surface;
    if (!ReconstructShader<UsdPreviewSurface>(spec, properties, references,
                                              &surface, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `" << kNdUsdPreviewSurfaceSurfaceshader << "`.");
    }
    shader->info_id = kNdUsdPreviewSurfaceSurfaceshader;
    shader->value = std::move(surface);
  } else {
    // Reconstruct as generic ShaderNode
    ShaderNode surface;
    if (!ReconstructShader<ShaderNode>(spec,properties, references,
                                              &surface, warn, err, options)) {
      PUSH_ERROR_AND_RETURN("Failed to reconstruct shader `" << shader_type << "`.");
    }
    if (shader_type.size()) {
      shader->info_id = shader_type;
    }
    shader->value = std::move(surface);
  }

  DCOUT("Shader reconstructed.");

  return true;
}

template <>
bool ReconstructPrim<Material>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    Material *material,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options)
{
  (void)spec;
  (void)references;
  (void)options;
  std::set<std::string> table;

  // TODO: special treatment for properties with 'inputs' and 'outputs' namespace.

  // Check if MaterialXConfigAPI is applied
  bool hasMaterialXConfig = false;
  bool hasMaterialUEConfig = false;
  for (auto &prop : properties) {
    if (prop.first == "config:mtlx:version" ||
        prop.first == "config:mtlx:namespace" ||
        prop.first == "config:mtlx:colorspace" ||
      prop.first == "config:mtlx:sourceUri") {
      hasMaterialXConfig = true;
    }
    if (prop.first == "config:unreal:version" ||
        prop.first == "config:unreal:engineVersion" ||
        prop.first == "config:unreal:sourceAsset" ||
        prop.first == "config:unreal:graphAuthority" ||
        prop.first == "config:unreal:materialDomain" ||
        prop.first == "config:unreal:blendMode" ||
        prop.first == "config:unreal:shadingModels" ||
        prop.first == "config:unreal:twoSided" ||
        prop.first == "config:unreal:opacityMaskClipValue" ||
        prop.first == "config:unreal:propertyArchive") {
      hasMaterialUEConfig = true;
    }
  }

  // Initialize MaterialXConfigAPI if needed
  if (hasMaterialXConfig) {
    material->materialXConfig = MaterialXConfigAPI();
  }
  if (hasMaterialUEConfig) {
    material->materialUEConfig = MaterialUEConfigAPI();
  }

  // For `Material`, `outputs` are terminal attribute and treated as input attribute with connection(Should be "token output:surface.connect = </path/to/shader>").
  for (auto &prop : properties) {
    // Parse MaterialXConfigAPI properties
    if (hasMaterialXConfig) {
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:mtlx:version", Material,
                           material->materialXConfig->mtlx_version)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:mtlx:namespace", Material,
                           material->materialXConfig->mtlx_namespace)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:mtlx:colorspace", Material,
                           material->materialXConfig->mtlx_colorspace)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:mtlx:sourceUri", Material,
                           material->materialXConfig->mtlx_sourceUri)
    }

    if (hasMaterialUEConfig) {
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:version", Material,
                           material->materialUEConfig->version)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:engineVersion", Material,
                           material->materialUEConfig->engineVersion)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:sourceAsset", Material,
                           material->materialUEConfig->sourceAsset)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:graphAuthority", Material,
                           material->materialUEConfig->graphAuthority)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:materialDomain", Material,
                           material->materialUEConfig->materialDomain)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:blendMode", Material,
                           material->materialUEConfig->blendMode)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:shadingModels", Material,
                           material->materialUEConfig->shadingModels)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:twoSided", Material,
                           material->materialUEConfig->twoSided)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:opacityMaskClipValue", Material,
                           material->materialUEConfig->opacityMaskClipValue)
      PARSE_TYPED_ATTRIBUTE(table, prop, "config:unreal:propertyArchive", Material,
                           material->materialUEConfig->propertyArchive)
    }

    PARSE_SHADER_INPUT_CONNECTION_PROPERTY(table, prop, "outputs:surface",
                                  Material, material->surface)
    PARSE_SHADER_INPUT_CONNECTION_PROPERTY(table, prop, "outputs:mtlx:surface",
                                  Material, material->mtlxSurface)
    PARSE_SHADER_INPUT_CONNECTION_PROPERTY(table, prop, "outputs:displacement",
                                  Material, material->displacement)
    PARSE_SHADER_INPUT_CONNECTION_PROPERTY(table, prop, "outputs:mtlx:displacement",
                                  Material, material->displacement)
    PARSE_SHADER_INPUT_CONNECTION_PROPERTY(table, prop, "outputs:volume",
                                  Material, material->volume)
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, kPurpose, Purpose, PurposeEnumHandler, Material,
                       material->purpose, options.strict_allowedToken_check)
    ADD_PROPERTY(table, prop, Material, material->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
  return true;
}

template <>
bool ReconstructPrim<NodeGraph>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    NodeGraph *nodegraph,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options)
{
  (void)spec;
  (void)references;
  (void)warn;
  std::set<std::string> table;

  // NodeGraph can have arbitrary outputs (e.g., outputs:result, outputs:normal, etc.)
  // They are stored in the props map, so we just add all properties
  for (auto &prop : properties) {
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, kPurpose, Purpose, PurposeEnumHandler, NodeGraph,
                       nodegraph->purpose, options.strict_allowedToken_check)
    ADD_PROPERTY(table, prop, NodeGraph, nodegraph->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
  return true;
}

///
/// -- PrimSpec wrappers for Shader/Material/NodeGraph
///

#define RECONSTRUCT_PRIM_PRIMSPEC_IMPL(__prim_ty) \
template <> \
bool ReconstructPrim<__prim_ty>( \
    PrimSpec &primspec, \
    __prim_ty *prim, \
    std::string *warn, \
    std::string *err, \
    const PrimReconstructOptions &options) { \
 \
  ReferenceList references; /* dummy */ \
 \
  return ReconstructPrim<__prim_ty>(primspec.specifier(), primspec.props(), references, prim, warn, err, options); \
}

RECONSTRUCT_PRIM_PRIMSPEC_IMPL(Shader)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(Material)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(NodeGraph)

} // namespace prim
} // namespace lightusd
