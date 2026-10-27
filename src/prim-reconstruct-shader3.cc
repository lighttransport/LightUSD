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

template <>
bool ReconstructShader<ShaderNode>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    ShaderNode *node,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options)
{
  (void)spec;
  (void)options;
  (void)err;

  if (!node) {
    return false;
  }

  // TODO: references
  (void)references;

  ParsePropertyTable table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>

  // Add everything to props.
  for (auto &prop : properties) {
    ADD_PROPERTY(table, prop, ShaderNode, node->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  DCOUT("ShaderNode reconstructed.");
  return true;
}

template <>
bool ReconstructShader<UsdPreviewSurface>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    UsdPreviewSurface *surface,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec;
  (void)references;
  (void)options;

  // Use centralized enum handler
  auto OpacityModeHandler = enum_handler::OpacityMode;

  ParsePropertyTable table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>
  for (auto &prop : properties) {
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:diffuseColor", UsdPreviewSurface,
                         surface->diffuseColor)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:emissiveColor", UsdPreviewSurface,
                         surface->emissiveColor)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:roughness", UsdPreviewSurface,
                         surface->roughness)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:specularColor", UsdPreviewSurface,
                         surface->specularColor)  // specular workflow
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:metallic", UsdPreviewSurface,
                         surface->metallic)  // non specular workflow
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:clearcoat", UsdPreviewSurface,
                         surface->clearcoat)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:clearcoatRoughness",
                         UsdPreviewSurface, surface->clearcoatRoughness)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:opacity", UsdPreviewSurface,
                         surface->opacity)
    // From 2.6
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, "inputs:opacityMode",
                       UsdPreviewSurface::OpacityMode, OpacityModeHandler, UsdPreviewSurface,
                       surface->opacityMode, options.strict_allowedToken_check)

    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:opacityThreshold",
                         UsdPreviewSurface, surface->opacityThreshold)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:ior", UsdPreviewSurface,
                         surface->ior)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:normal", UsdPreviewSurface,
                         surface->normal)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:displacement", UsdPreviewSurface,
                         surface->displacement)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:occlusion", UsdPreviewSurface,
                         surface->occlusion)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:useSpecularWorkflow",
                         UsdPreviewSurface, surface->useSpecularWorkflow)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:surface", UsdPreviewSurface,
                   surface->outputsSurface)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:displacement", UsdPreviewSurface,
                   surface->outputsDisplacement)
    ADD_PROPERTY(table, prop, UsdPreviewSurface, surface->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructShader<UsdUVTexture>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    UsdUVTexture *texture,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options)
{
  (void)spec;
  (void)references;
  (void)options;

  // Use centralized enum handlers
  auto SourceColorSpaceHandler = enum_handler::SourceColorSpace;
  auto WrapHandler = enum_handler::TextureWrap;

  ParsePropertyTable table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>

  for (auto &prop : properties) {
    DCOUT("prop.name = " << prop.first);
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:file", UsdUVTexture, texture->file)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:st", UsdUVTexture,
                          texture->st)
    if (prop.first == "inputs:sourceColorSpace") {
      if (table.count("inputs:sourceColorSpace")) {
        continue;
      }
      const Attribute &attr = prop.second.get_attribute();
      std::function<nonstd::expected<UsdUVTexture::SourceColorSpace, std::string>(
          const std::string &)> fun = SourceColorSpaceHandler;
      if (!ParseTimeSampledEnumProperty(
              "inputs:sourceColorSpace", options.strict_allowedToken_check, fun,
              attr, &texture->sourceColorSpace, warn, err, options)) {
        return false;
      }
      texture->sourceColorSpace.metas() =
          std::move(prop.second.attribute().metas());
      table.insert("inputs:sourceColorSpace");
      continue;
    }
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, "inputs:wrapS",
                       UsdUVTexture::Wrap, WrapHandler, UsdUVTexture,
                       texture->wrapS, options.strict_allowedToken_check)
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, "inputs:wrapT",
                       UsdUVTexture::Wrap, WrapHandler, UsdUVTexture,
                       texture->wrapT, options.strict_allowedToken_check)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:fallback", UsdUVTexture,
                          texture->fallback)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:scale", UsdUVTexture,
                          texture->scale)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:bias", UsdUVTexture,
                          texture->bias)
    // lightusd extensions: UV set selection (index / name). Mirrored by the
    // USDA printer (pprint-shader.cc) and USDC writer (sconv-shader.cc).
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:uv_set", UsdUVTexture,
                          texture->uv_set)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:uv_set_name", UsdUVTexture,
                          texture->uv_set_name)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:r", UsdUVTexture,
                                  texture->outputsR)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:g", UsdUVTexture,
                                  texture->outputsG)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:b", UsdUVTexture,
                                  texture->outputsB)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:a", UsdUVTexture,
                                  texture->outputsA)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:rgb", UsdUVTexture,
                                  texture->outputsRGB)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:rgba", UsdUVTexture,
                                  texture->outputsRGBA)
    ADD_PROPERTY(table, prop, UsdUVTexture, texture->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  DCOUT("UsdUVTexture reconstructed.");
  return true;
}


template <>
bool ReconstructShader<UsdTransform2d>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    UsdTransform2d *transform,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options)
{
  (void)spec;
  (void)references;
  (void)options;
  ParsePropertyTable table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>
  for (auto &prop : properties) {
    DCOUT("prop = " << prop.first);
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:in", UsdTransform2d,
                   transform->in)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:rotation", UsdTransform2d,
                   transform->rotation)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:scale", UsdTransform2d,
                   transform->scale)
    PARSE_SHADER_INPUT_ATTRIBUTE(table, prop, "inputs:translation", UsdTransform2d,
                   transform->translation)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:result",
                                  UsdTransform2d, transform->result)
    ADD_PROPERTY(table, prop, UsdPrimvarReader_float2, transform->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}


}  // namespace prim
}  // namespace lightusd
