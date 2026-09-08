// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment, Inc.
//
// Predefined MaterialX shadingmodel & Built-in MaterialX XML import plugIn.
// Supports both import and export.
//
// example usage
//
// def Shader "mesh" (
//   prepend references = @myshader.mtlx@
// )
// {
//    ...
// }
//
// Based on MaterialX spec v1.38

#pragma once

#include <map>
#include <string>
#include <vector>

#include "asset-resolution.hh"
#include "usdShade.hh"


namespace lightusd {

constexpr auto kMtlxUsdPreviewSurface = "MtlxUsdPreviewSurface";
constexpr auto kMtlxAutodeskStandardSurface = "MtlxAutodeskStandardSurface";
constexpr auto kMtlxOpenPBRSurface = "MtlxOpenPBRSurface";

// MaterialX node definition IDs (as used in info:id attribute)
constexpr auto kNdOpenPbrSurfaceSurfaceshader = "ND_open_pbr_surface_surfaceshader";
constexpr auto kNdStandardSurfaceSurfaceshader = "ND_standard_surface_surfaceshader";
// MaterialX's UsdPreviewSurface node definition — same inputs as the native
// UsdPreviewSurface, so it is reconstructed into a UsdPreviewSurface value.
constexpr auto kNdUsdPreviewSurfaceSurfaceshader = "ND_UsdPreviewSurface_surfaceshader";

// MaterialX Light Shader Nodes
constexpr auto kMtlxUniformEdf = "uniform_edf";
constexpr auto kMtlxConicalEdf = "conical_edf";
constexpr auto kMtlxMeasuredEdf = "measured_edf";
constexpr auto kMtlxLight = "light";

namespace mtlx {

enum class ColorSpace {
  Lin_rec709, // lin_rec709
  Unknown
};

} // namespace mtlx

///
/// Configuration for MaterialX parsing.
/// Similar to OpenUSD's USDMTLX_PRIMARY_UV_NAME environment variable.
///
struct MtlxConfig {
  /// Primary UV set name for ND_texcoord_vector2 nodes.
  /// Empty string means use default "st".
  /// Similar to OpenUSD's USDMTLX_PRIMARY_UV_NAME environment variable.
  std::string primary_uv_name{"st"};

  /// Secondary UV set name pattern for ND_texcoord_vector2 with index > 0.
  /// The index will be appended (e.g., "st1", "st2").
  /// Empty string means use default "st".
  std::string secondary_uv_name_prefix{"st"};
};

// MaterialX shader input connection information
struct MtlxShaderConnection {
  std::string input_name;      // e.g., "base_color"
  std::string type;            // Original MaterialX input type.
  std::string nodegraph;        // Reference to nodegraph name (if using nodegraph output)
  std::string output;           // Output name from nodegraph (e.g., "out_color")
  std::string nodename;         // Direct node reference (alternative to nodegraph)
};

// Renderer-specific direct inputs that are not part of the built-in carrier
// structs. They are retained verbatim so importing and exporting a MaterialX
// shader does not silently discard extension parameters.
struct MtlxCustomShaderInput {
  std::string name;
  std::string type;
  std::string value;
};

// <surfacematerial>
struct MtlxMaterial {
  std::string name;
  std::string typeName;
  std::string nodename;
};

// A MaterialX <look> child (materialassign/propertyassign/visibility, or an
// extension element). Attributes are retained generically so importing a look
// does not discard renderer-specific assignment metadata.
struct MtlxLookElement {
  std::string name;
  std::map<std::string, std::string> attributes;
  std::vector<MtlxLookElement> children;
};

struct MtlxLook {
  std::string name;
  std::vector<MtlxLookElement> elements;
};

struct MtlxModel {
  std::string asset_name;

  std::string version;
  std::string cms;
  std::string cmsconfig; // filename
  std::string color_space; // colorspace
  std::string name_space; // namespace

  //mtlx::ColorSpace colorspace{Lin_rec709};

  std::string shader_name;

  // Content of shader.
  // MtlxUsdPreviewSurface or MtlxAutodeskStandardSurface
  value::Value shader;

  std::map<std::string, MtlxMaterial> surface_materials;
  std::map<std::string, MtlxLook> looks;
  std::map<std::string, value::Value> shaders; // MtlxUsdPreviewSurface, MtlxAutodeskStandardSurface, or OpenPBRSurface
  std::map<std::string, value::Value> light_shaders; // Light shaders (EDF nodes)
  std::map<std::string, PrimSpec> nodegraphs; // NodeGraph PrimSpecs
  std::map<std::string, std::vector<MtlxShaderConnection>> shader_connections; // Shader name -> list of connections
  std::map<std::string, std::vector<MtlxCustomShaderInput>> custom_shader_inputs;
};

struct MtlxUsdPreviewSurface : UsdPreviewSurface {
  // MaterialX currently has no additional UsdPreviewSurface fields in this
  // adapter. The wrapper remains useful at the type-erased boundary.
};

// OpenPBR Surface Shader
// https://github.com/AcademySoftwareFoundation/OpenPBR
// MaterialX implementation of OpenPBR specification
struct MtlxOpenPBRSurface : ShaderNode {
  // Base properties
  TypedAttributeWithFallback<Animatable<float>> base_weight{1.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> base_color{
      value::color3f{0.8f, 0.8f, 0.8f}};
  TypedAttributeWithFallback<Animatable<float>> base_roughness{0.0f};
  TypedAttributeWithFallback<Animatable<float>> base_metalness{0.0f};
  TypedAttributeWithFallback<Animatable<float>> base_diffuse_roughness{0.0f};

  // Specular properties
  TypedAttributeWithFallback<Animatable<float>> specular_weight{1.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> specular_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> specular_roughness{0.3f};
  TypedAttributeWithFallback<Animatable<float>> specular_ior{1.5f};
  TypedAttributeWithFallback<Animatable<float>> specular_ior_level{0.5f};
  TypedAttributeWithFallback<Animatable<float>> specular_anisotropy{0.0f};
  TypedAttributeWithFallback<Animatable<float>> specular_rotation{0.0f};
  TypedAttributeWithFallback<Animatable<float>> specular_roughness_anisotropy{0.0f};

  // Transmission properties
  TypedAttributeWithFallback<Animatable<float>> transmission_weight{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> transmission_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> transmission_depth{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> transmission_scatter{
      value::color3f{0.0f, 0.0f, 0.0f}};
  TypedAttributeWithFallback<Animatable<float>> transmission_scatter_anisotropy{0.0f};
  TypedAttributeWithFallback<Animatable<float>> transmission_dispersion{0.0f};
  TypedAttributeWithFallback<Animatable<float>> transmission_dispersion_abbe_number{0.0f};
  TypedAttributeWithFallback<Animatable<float>> transmission_dispersion_scale{0.0f};

  // Subsurface properties
  TypedAttributeWithFallback<Animatable<float>> subsurface_weight{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> subsurface_color{
      value::color3f{0.8f, 0.8f, 0.8f}};
  TypedAttributeWithFallback<Animatable<float>> subsurface_radius{0.05f};  // Blender uses float, not color3f
  TypedAttributeWithFallback<Animatable<value::color3f>> subsurface_radius_scale{
      value::color3f{1.0f, 0.2f, 0.1f}};
  TypedAttributeWithFallback<Animatable<float>> subsurface_scale{1.0f};
  TypedAttributeWithFallback<Animatable<float>> subsurface_anisotropy{0.0f};
  TypedAttributeWithFallback<Animatable<float>> subsurface_scatter_anisotropy{0.0f};

  // Sheen properties
  TypedAttributeWithFallback<Animatable<float>> sheen_weight{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> sheen_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> sheen_roughness{0.3f};

  // Coat properties
  TypedAttributeWithFallback<Animatable<float>> coat_weight{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> coat_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> coat_roughness{0.1f};
  TypedAttributeWithFallback<Animatable<float>> coat_anisotropy{0.0f};
  TypedAttributeWithFallback<Animatable<float>> coat_rotation{0.0f};
  TypedAttributeWithFallback<Animatable<float>> coat_roughness_anisotropy{0.0f};
  TypedAttributeWithFallback<Animatable<float>> coat_ior{1.6f};
  TypedAttributeWithFallback<Animatable<float>> coat_darkening{0.0f};
  TypedAttributeWithFallback<Animatable<float>> coat_affect_color{0.0f};
  TypedAttributeWithFallback<Animatable<float>> coat_affect_roughness{0.0f};

  // Fuzz properties (fabric/cloth layer)
  TypedAttributeWithFallback<Animatable<float>> fuzz_weight{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> fuzz_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> fuzz_roughness{0.5f};

  // Thin film properties
  TypedAttributeWithFallback<Animatable<float>> thin_film_thickness{0.0f};
  TypedAttributeWithFallback<Animatable<float>> thin_film_ior{1.5f};
  TypedAttributeWithFallback<Animatable<float>> thin_film_weight{0.0f};

  // Emission properties
  TypedAttributeWithFallback<Animatable<float>> emission_luminance{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> emission_color{
      value::color3f{1.0f, 1.0f, 1.0f}};

  // Geometry properties
  TypedAttributeWithFallback<Animatable<float>> geometry_opacity{1.0f};
  TypedAttributeWithFallback<Animatable<bool>> geometry_thin_walled{false};

  // Normal and tangent
  TypedAttribute<Animatable<value::normal3f>> geometry_normal;
  TypedAttribute<Animatable<value::vector3f>> geometry_tangent;
  TypedAttribute<Animatable<value::normal3f>> geometry_coat_normal;
  TypedAttribute<Animatable<value::vector3f>> geometry_coat_tangent;

  // Output
  TypedTerminalAttribute<value::token> surface;  // 'outputs:surface'
};

// https://github.com/Autodesk/standard-surface/blob/master/reference/standard_surface.mtlx
// We only support v1.0.1
struct MtlxAutodeskStandardSurface : ShaderNode {
  // Base properties
  TypedAttributeWithFallback<Animatable<float>> base{1.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> base_color{
      value::color3f{0.8f, 0.8f, 0.8f}};  // color3
  TypedAttributeWithFallback<Animatable<float>> diffuse_roughness{0.0f};
  TypedAttributeWithFallback<Animatable<float>> metalness{0.0f};

  // Specular properties
  TypedAttributeWithFallback<Animatable<float>> specular{1.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> specular_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> specular_roughness{0.2f};
  TypedAttributeWithFallback<Animatable<float>> specular_IOR{1.5f};
  TypedAttributeWithFallback<Animatable<float>> specular_anisotropy{0.0f};
  TypedAttributeWithFallback<Animatable<float>> specular_rotation{0.0f};

  // Transmission properties
  TypedAttributeWithFallback<Animatable<float>> transmission{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> transmission_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> transmission_depth{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> transmission_scatter{
      value::color3f{0.0f, 0.0f, 0.0f}};
  TypedAttributeWithFallback<Animatable<float>> transmission_scatter_anisotropy{0.0f};
  TypedAttributeWithFallback<Animatable<float>> transmission_dispersion{0.0f};
  TypedAttributeWithFallback<Animatable<float>> transmission_extra_roughness{0.0f};

  // Subsurface properties
  TypedAttributeWithFallback<Animatable<float>> subsurface{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> subsurface_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  // Autodesk standard_surface defines subsurface_radius as color3 (per-channel
  // mean free path), so accept color3f — authoring it as such must not fail the
  // whole shader reconstruction.
  TypedAttributeWithFallback<Animatable<value::color3f>> subsurface_radius{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> subsurface_scale{1.0f};
  TypedAttributeWithFallback<Animatable<float>> subsurface_anisotropy{0.0f};

  // Sheen properties
  TypedAttributeWithFallback<Animatable<float>> sheen{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> sheen_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> sheen_roughness{0.3f};

  // Coat properties
  TypedAttributeWithFallback<Animatable<float>> coat{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> coat_color{
      value::color3f{1.0f, 1.0f, 1.0f}};
  TypedAttributeWithFallback<Animatable<float>> coat_roughness{0.1f};
  TypedAttributeWithFallback<Animatable<float>> coat_anisotropy{0.0f};
  TypedAttributeWithFallback<Animatable<float>> coat_rotation{0.0f};
  TypedAttributeWithFallback<Animatable<float>> coat_IOR{1.5f};
  TypedAttributeWithFallback<Animatable<float>> coat_affect_color{0.0f};
  TypedAttributeWithFallback<Animatable<float>> coat_affect_roughness{0.0f};

  // Thin film properties
  TypedAttributeWithFallback<Animatable<float>> thin_film_thickness{0.0f};
  TypedAttributeWithFallback<Animatable<float>> thin_film_IOR{1.5f};

  // Emission properties
  TypedAttributeWithFallback<Animatable<float>> emission{0.0f};
  TypedAttributeWithFallback<Animatable<value::color3f>> emission_color{
      value::color3f{1.0f, 1.0f, 1.0f}};

  // Opacity
  TypedAttributeWithFallback<Animatable<value::color3f>> opacity{
      value::color3f{1.0f, 1.0f, 1.0f}};

  // Thin walled
  TypedAttributeWithFallback<Animatable<bool>> thin_walled{false};

  // Normal and tangent
  TypedAttribute<Animatable<value::normal3f>> normal;
  TypedAttribute<Animatable<value::vector3f>> tangent;
  TypedAttribute<Animatable<value::normal3f>> coat_normal;
  // Height input retained for renderer parity with the next MaterialX carrier.
  // It is consumed as a real-time geometry modifier, not as an OpenPBR lobe.
  TypedAttributeWithFallback<Animatable<float>> displacement{0.0f};

  // Output
  TypedTerminalAttribute<value::token> out;  // 'out'
};

//
// MaterialX Light Shader Nodes (EDF - Emission Distribution Functions)
//

// uniform_edf: Constructs an EDF emitting light uniformly in all directions
struct MtlxUniformEdf : ShaderNode {
  TypedAttributeWithFallback<Animatable<value::color3f>> color{
      value::color3f{1.0f, 1.0f, 1.0f}};  // color3 - Radiant emittance

  // Output
  TypedTerminalAttribute<value::token> out;  // 'out' (EDF type)
};

// conical_edf: Constructs an EDF emitting light inside a cone around the normal direction
struct MtlxConicalEdf : ShaderNode {
  TypedAttributeWithFallback<Animatable<value::color3f>> color{
      value::color3f{1.0f, 1.0f, 1.0f}};  // color3 - Radiant emittance
  TypedAttribute<Animatable<value::normal3f>> normal;  // vector3 - Surface normal (default: world space normal)
  TypedAttributeWithFallback<Animatable<float>> inner_angle{60.0f};  // float - Inner cone angle in degrees
  TypedAttribute<Animatable<float>> outer_angle;  // float - Outer cone angle for intensity falloff

  // Output
  TypedTerminalAttribute<value::token> out;  // 'out' (EDF type)
};

// measured_edf: Constructs an EDF emitting light according to a measured IES light profile
struct MtlxMeasuredEdf : ShaderNode {
  TypedAttributeWithFallback<Animatable<value::color3f>> color{
      value::color3f{1.0f, 1.0f, 1.0f}};  // color3 - Radiant emittance
  TypedAttribute<Animatable<value::AssetPath>> file;  // filename - Path to IES light profile data

  // Output
  TypedTerminalAttribute<value::token> out;  // 'out' (EDF type)
};

// light: Constructs a light shader from an emission distribution function (EDF)
struct MtlxLight : ShaderNode {
  TypedAttribute<value::token> edf;  // EDF - Emission distribution function (connection to EDF node)
  TypedAttributeWithFallback<Animatable<value::color3f>> intensity{
      value::color3f{1.0f, 1.0f, 1.0f}};  // color3 - Intensity multiplier for EDF emittance

  // Optional: exposure (EV) - some renderers support this
  TypedAttribute<Animatable<float>> exposure;  // float - Exposure value

  // Output
  TypedTerminalAttribute<value::token> out;  // 'out' (lightshader type)
};

//
// IO
//

///
/// Load MaterialX XML from a string.
///
/// @param[in] str String representation of XML data.
/// @param[in] asset_name Corresponding asset name. Can be empty.
/// @param[out] mtlx Output
/// @param[out] warn Warning message
/// @param[out] err Error message
/// @param[in] config MaterialX configuration (primary_uv_name, etc.)
///
/// @return true upon success.
bool ReadMaterialXFromString(const std::string &str, const std::string &asset_name, MtlxModel *mtlx,
                             std::string *warn, std::string *err,
                             const MtlxConfig &config = MtlxConfig{});

///
/// Load MaterialX XML from a file.
///
/// @param[in] resolver Asset resolution resolver.
/// @param[in] asset_path Asset path.
/// @param[out] mtlx Output
/// @param[out] warn Warning message
/// @param[out] err Error message
/// @param[in] config MaterialX configuration (primary_uv_name, etc.)
///
/// @return true upon success.
///
bool ReadMaterialXFromFile(const AssetResolutionResolver &resolver,
                            const std::string &asset_path, MtlxModel *mtlx,
                            std::string *warn, std::string *err,
                            const MtlxConfig &config = MtlxConfig{});

bool WriteMaterialXToString(const MtlxModel &mtlx, std::string &xml_str,
                             std::string *warn, std::string *err);

bool ToPrimSpec(const MtlxModel &model, PrimSpec &ps, std::string *err);

///
/// Load MaterialX from Asset and construct USD PrimSpec
///
bool LoadMaterialXFromAsset(const Asset &asset,
                            const std::string &asset_path, PrimSpec &ps /* inout */,
                            std::string *warn, std::string *err);

///
/// Convert MaterialX Light shader to UsdLux light
/// This helps map MaterialX light shaders to corresponding USD light types
///
bool ConvertMtlxLightToUsdLux(const MtlxLight &mtlx_light,
                               const std::map<std::string, value::Value> &light_shaders,
                               value::Value *usd_light,
                               std::string *warn, std::string *err);

// import DEFINE_TYPE_TRAIT and DEFINE_ROLE_TYPE_TRAIT
#include "define-type-trait.inc"

namespace value {

// ShaderNodes
DEFINE_TYPE_TRAIT(MtlxUsdPreviewSurface, kMtlxUsdPreviewSurface,
                  TYPE_ID_IMAGING_MTLX_PREVIEWSURFACE, 1);
DEFINE_TYPE_TRAIT(MtlxAutodeskStandardSurface, kMtlxAutodeskStandardSurface,
                  TYPE_ID_IMAGING_MTLX_STANDARDSURFACE, 1);
DEFINE_TYPE_TRAIT(MtlxOpenPBRSurface, kMtlxOpenPBRSurface,
                  TYPE_ID_IMAGING_MTLX_OPENPBRSURFACE, 1);

// Light ShaderNodes (EDF and Light)
DEFINE_TYPE_TRAIT(MtlxUniformEdf, kMtlxUniformEdf,
                  TYPE_ID_IMAGING_MTLX_UNIFORMEDF, 1);
DEFINE_TYPE_TRAIT(MtlxConicalEdf, kMtlxConicalEdf,
                  TYPE_ID_IMAGING_MTLX_CONICALEDF, 1);
DEFINE_TYPE_TRAIT(MtlxMeasuredEdf, kMtlxMeasuredEdf,
                  TYPE_ID_IMAGING_MTLX_MEASUREDEDF, 1);
DEFINE_TYPE_TRAIT(MtlxLight, kMtlxLight,
                  TYPE_ID_IMAGING_MTLX_LIGHT, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
