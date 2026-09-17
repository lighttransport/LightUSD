// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file usdShade.hh
/// @brief USD Shading schema definitions
///
/// Implements material and shader primitives following USD's UsdShade schema.
/// Includes Material, Shader, and supporting classes for building shading
/// networks. LightUSD also implements some UsdImaging utilities here.
///
/// Key classes:
/// - Material: Material binding and organization
/// - Shader: Individual shader nodes (e.g., UsdPreviewSurface, UsdUVTexture)
/// - UsdShadePrim: Base class for shading primitives
/// - Various shader input/output types
///
/// Supported shader types:
/// - UsdPreviewSurface: Physically-based material model
/// - UsdUVTexture: 2D texture sampling
/// - UsdTransform2d: 2D transformations
/// - UsdPrimvarReader_*: Primitive variable readers
///
/// Notes:
/// - `interfaceOnly` connections are partially supported (wrapS/wrapT etc.)
/// - MaterialX support is in usdMtlx.{hh,cc}
/// - NodeGraph is defined below with child shader management via prim hierarchy
///
#pragma once

// Core includes (replaces monolithic prim-types.hh)
#include "value-types.hh"
#include "nonstd/optional.hpp"
#include "core/prim-enums.hh"        // Specifier, Purpose
#include "core/composition-types.hh" // Reference, Payload, ListEditQual
#include "core/prim-metas.hh"       // PrimMeta
#include "core/animatable.hh"       // Animatable
#include "core/typed-attribute.hh"  // TypedAttribute, TypedAttributeWithFallback, TypedTerminalAttribute
#include "core/relationship.hh"     // TypedConnection
#include "core/property.hh"         // Property
#include "core/material-binding.hh" // MaterialBinding (for DEFINE_TYPE_TRAIT)
#include "core/variant-types.hh"    // VariantSet

namespace lightusd {

constexpr auto kMaterial = "Material";
constexpr auto kShader = "Shader";
constexpr auto kNodeGraph = "NodeGraph";
constexpr auto kShaderNode = "ShaderNode";
constexpr auto kMaterialXConfigAPI = "MaterialXConfigAPI";
constexpr auto kMaterialUEConfigAPI = "MaterialUEConfigAPI";

constexpr auto kShaderInfoId = "info:id";

constexpr auto kUsdPreviewSurface = "UsdPreviewSurface";
constexpr auto kUsdUVTexture = "UsdUVTexture";
constexpr auto kUsdTransform2d = "UsdTransform2d";
constexpr auto kUsdPrimvarReader_int = "UsdPrimvarReader_int";
constexpr auto kUsdPrimvarReader_float = "UsdPrimvarReader_float";
constexpr auto kUsdPrimvarReader_float2 = "UsdPrimvarReader_float2";
constexpr auto kUsdPrimvarReader_float3 = "UsdPrimvarReader_float3";
constexpr auto kUsdPrimvarReader_float4 = "UsdPrimvarReader_float4";
constexpr auto kUsdPrimvarReader_string = "UsdPrimvarReader_string";
constexpr auto kUsdPrimvarReader_normal = "UsdPrimvarReader_normal";
constexpr auto kUsdPrimvarReader_point = "UsdPrimvarReader_point";
constexpr auto kUsdPrimvarReader_vector = "UsdPrimvarReader_vector";
constexpr auto kUsdPrimvarReader_matrix = "UsdPrimvarReader_matrix";

constexpr auto kOpenPBRSurface = "OpenPBRSurface";

// In OpenUSD, UsdShadeShader/Material/NodeGraph inherit from UsdTyped (not Prim).
// LightUSD uses a separate UsdShadePrim base to hold common fields (name, meta, props).
struct UsdShadePrim {
  std::string name;
  Specifier spec{Specifier::Def};

  int64_t parent_id{-1};

  PrimMeta meta;

  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }

  // Check if `key` exists in `sdrMetadata` metadatum.
  // Return false when `key` is not found in `sdrMetadata`, or corrensponding item is not a string type.
  bool has_sdr_metadata(const std::string &key);

  // Get value from `sdrMetadata` metadatum.
  // Return empty string when `key` is not found in `sdrMetadata`, or corrensponding item is not a string type.
  const std::string get_sdr_metadata(const std::string &key);

  // Set value to `sdrMetadata` metadatum.
  // Return false when error(e.g. `key` contains invalid character for USD dictionary)
  bool set_sdr_metadata(const std::string &key, const std::string &value);

  TypedAttributeWithFallback<Purpose> purpose{
      Purpose::Default};  // "uniform token purpose"

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  // Custom properties
  std::map<std::string, Property> props;

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

//
// Similar to Maya's ShadingGroup
//
// MaterialXConfigAPI is an API schema that provides an interface for
// storing information about the MaterialX environment.
struct MaterialXConfigAPI {
  // MaterialX library version that the data has been authored against.
  // Defaults to 1.38 to allow correct versioning of old files.
  TypedAttributeWithFallback<std::string> mtlx_version{"1.38"}; // "string config:mtlx:version"

  // MaterialX namespace for node definitions
  TypedAttributeWithFallback<std::string> mtlx_namespace{""}; // "string config:mtlx:namespace"

  // Default colorspace for MaterialX documents
  TypedAttributeWithFallback<std::string> mtlx_colorspace{"lin_rec709"}; // "string config:mtlx:colorspace"

  // Source URI for MaterialX document references
  TypedAttributeWithFallback<std::string> mtlx_sourceUri{""}; // "string config:mtlx:sourceUri"
};

// LightUSD's Unreal Engine material bridge configuration.  The canonical
// shading network remains ordinary UsdShade/MaterialX; these fields identify
// the optional UE-preservation layer and its versioned graph payload.
struct MaterialUEConfigAPI {
  TypedAttributeWithFallback<std::string> version{"1"};
  TypedAttributeWithFallback<std::string> engineVersion{""};
  TypedAttributeWithFallback<std::string> sourceAsset{""};
  TypedAttributeWithFallback<std::string> graphAuthority{"materialx+ue"};
  TypedAttributeWithFallback<std::string> materialDomain{"Surface"};
  TypedAttributeWithFallback<std::string> blendMode{"Opaque"};
  TypedAttributeWithFallback<std::string> shadingModels{""};
  TypedAttributeWithFallback<bool> twoSided{false};
  TypedAttributeWithFallback<float> opacityMaskClipValue{0.333333f};
  TypedAttributeWithFallback<std::string> propertyArchive{""};
};

struct Material : UsdShadePrim {

  ///
  /// NOTE: Material's outputs must be a connection.
  /// (Whereas Shader's outputs is not)
  ///
  TypedConnection<value::token> surface; // "token outputs:surface.connect"
  TypedConnection<value::token> mtlxSurface; // "token outputs:mtlx:surface.connect"
  TypedConnection<value::token> displacement; // "token outputs:displacement.connect"
  TypedConnection<value::token> volume; // "token outputs:volume.connect"

  // Optional MaterialXConfigAPI
  nonstd::optional<MaterialXConfigAPI> materialXConfig;

  // Optional LightUSD Unreal material preservation layer.
  nonstd::optional<MaterialUEConfigAPI> materialUEConfig;

};

///
/// NodeGraph (Shader Network Container)
///
/// A NodeGraph is a container for shading nodes that can expose arbitrary outputs.
/// Container for organizing shader nodes and connections in a network.
/// Can contain multiple shader nodes as children and provide interface inputs/outputs.
/// Unlike Material which has fixed outputs (surface, displacement, volume),
/// NodeGraph outputs are stored in the props map with the "outputs:" prefix.
///
/// Example:
///   def NodeGraph "MyNodeGraph" {
///     float3 outputs:result.connect = </path/to/shader.outputs:out>
///   }
///
struct NodeGraph : UsdShadePrim {
  // NodeGraph can have arbitrary inputs and outputs (e.g., outputs:result, outputs:normal, etc.)
  // These are stored in the inherited props map from UsdShadePrim
  // Child nodes are stored as children in the USD hierarchy, not directly here

  // Optional MaterialX-specific attributes
  TypedAttribute<std::string> nodedef;  // Reference to a nodedef
  TypedAttribute<std::string> nodegraph_type;  // Type of the nodegraph
};

//
// Base class of ShaderNode. Maybe similar to SdrShaderNode in pxrUSD
//
struct ShaderNode : UsdShadePrim {

};

template <typename T>
struct UsdPrimvarReader : ShaderNode {

  TypedAttribute<Animatable<T>> fallback;  // "inputs:fallback"

  TypedAttribute<Animatable<std::string>> varname;  // "string inputs:varname". Name of the primvar to be fetched from the geometry("primvar" namespace is omitted). NOTE: older spec uses `token` type: https://openusd.org/release/spec_usdpreviewsurface.html#version-2-3


  ///
  /// Outputs
  ///
  TypedTerminalAttribute<T> result; // Terminal attr. "T outputs:result"

};

using UsdPrimvarReader_float = UsdPrimvarReader<float>;
using UsdPrimvarReader_float2 = UsdPrimvarReader<value::float2>;
using UsdPrimvarReader_float3 = UsdPrimvarReader<value::float3>;
using UsdPrimvarReader_float4 = UsdPrimvarReader<value::float4>;
using UsdPrimvarReader_int = UsdPrimvarReader<int>;
using UsdPrimvarReader_string = UsdPrimvarReader<std::string>;

// The underlying type is float precision for `normal`, `vector` and `point`
using UsdPrimvarReader_normal = UsdPrimvarReader<value::normal3f>;
using UsdPrimvarReader_vector = UsdPrimvarReader<value::vector3f>;
using UsdPrimvarReader_point  = UsdPrimvarReader<value::point3f>;

// The underlying type is matrix4d
using UsdPrimvarReader_matrix  = UsdPrimvarReader<value::matrix4d>;

// UV Set specification for multiple UV coordinate support
struct UVSetInfo {
  std::string name;  // UV set name (e.g., "st", "st0", "st1", "uv0", "uv1")
  int index{0};      // UV set index (0, 1, 2, etc.)

  UVSetInfo() = default;
  UVSetInfo(const std::string& n, int idx = 0) : name(n), index(idx) {}
};

struct UsdUVTexture : ShaderNode {

  // NOTE: transparent black(0, 0, 0, 0) for "black"
  //       https://github.com/PixarAnimationStudios/OpenUSD/commit/2cf6612b2b1d5a1a1031bc153867116c5963e605
  enum class Wrap {
    UseMetadata, // "useMetadata" (default)
    Black, // "black"
    Clamp, // "clamp"
    Repeat, // "repeat"
    Mirror, // "mirror"
  };

  enum class SourceColorSpace {
    Auto, // "auto"(default)
    Raw, // "raw"
    SRGB, // "sRGB
  };

  TypedAttribute<Animatable<value::AssetPath>> file; // "asset inputs:file" interfaceOnly

  TypedAttributeWithFallback<Animatable<value::texcoord2f>> st{value::texcoord2f{0.0f, 0.0f}}; // "inputs:st"

  // UV set selection - which UV coordinate set to use
  // Default is 0 (primary UV set)
  // MaterialX uses "texcoord" input, USD typically uses "st", "st0", "st1", etc.
  TypedAttributeWithFallback<int> uv_set{0}; // "int inputs:uv_set" - UV set index
  TypedAttribute<value::token> uv_set_name; // "token inputs:uv_set_name" - UV set name (e.g., "st0", "st1")

  TypedAttributeWithFallback<Animatable<Wrap>> wrapS{Wrap::UseMetadata}; // "token inputs:wrapS" interfaceOnly
  TypedAttributeWithFallback<Animatable<Wrap>> wrapT{Wrap::UseMetadata}; // "token inputs:wrapT" interfaceOnly

  TypedAttributeWithFallback<Animatable<value::float4>> fallback{
      Animatable<value::float4>(value::float4{0.0f, 0.0f, 0.0f, 1.0f})}; // "inputs:fallback" Fallback value when no texture is connected

  TypedAttributeWithFallback<Animatable<SourceColorSpace>> sourceColorSpace{SourceColorSpace::Auto}; // "token inputs:sourceColorSpace" interfaceOnly

  TypedAttributeWithFallback<value::float4> scale{{1.0f, 1.0f, 1.0f, 1.0f}}; // "inputs:scale" interfaceOnly
  TypedAttributeWithFallback<value::float4> bias{{0.0f, 0.0f, 0.0f, 0.0f}}; // "inputs:bias" interfaceOnly

  ///
  /// Outputs
  ///
  /// Terminal attribute.
  ///
  TypedTerminalAttribute<float> outputsR; // "float outputs:r"
  TypedTerminalAttribute<float> outputsG; // "float outputs:g"
  TypedTerminalAttribute<float> outputsB; // "float outputs:b"
  TypedTerminalAttribute<float> outputsA; // "float outputs:a"
  TypedTerminalAttribute<value::float3> outputsRGB; // "color3f outputs:rgb" (USD spec type is color3f/float3, not a single float). Allow color3f as well(please use TypedTerminalAttribute::get_actual_type_name() to get a actual type name in USDA/USDC).
  TypedTerminalAttribute<value::float4> outputsRGBA; // "float4 outputs:rgba"

  // Note: Texture coordinate orientation follows USD convention (origin at bottom-left).
  // See: https://openusd.org/release/spec_usdpreviewsurface.html
};

// UsdPreviewSurface
// USD's default? PBR shader
// https://graphics.pixar.com/usd/docs/UsdPreviewSurface-Proposal.html
// $USD/pxr/usdImaging/plugin/usdShaders/shaders/shaderDefs.usda

struct UsdPreviewSurface : ShaderNode {

  // From 2.6
  // NOTE: When opacityThreshold is non-zero, opacityMode is ignored.
  enum class OpacityMode {
    Opacity, // "opacity" : treat opacity as standard alpha (legacy name)
    Transparent, // "transparent" : the material will still receive a lighting response
    Presence, // "presence" : no lighting response
  };

  TypedAttributeWithFallback<Animatable<value::color3f>> diffuseColor{value::color3f{0.18f, 0.18f, 0.18f}};  // "inputs:diffuseColor"
  TypedAttributeWithFallback<Animatable<value::color3f>> emissiveColor{value::color3f{0.0f, 0.0f, 0.0f}};  // "inputs:emissiveColor"

  TypedAttributeWithFallback<Animatable<int>> useSpecularWorkflow{0}; // "inputs:useSpecularWorkflow"

  // specular workflow
  TypedAttributeWithFallback<Animatable<value::color3f>> specularColor{value::color3f{0.0f, 0.0f, 0.0f}};

  // metalness workflow
  //TypedAttributeWithFallback<float> metallic{0.0f};  // "inputs:metallic"
  TypedAttributeWithFallback<Animatable<float>> metallic{0.0f};  // "inputs:metallic"

  //
  TypedAttributeWithFallback<Animatable<float>> clearcoat{0.0f};  // "inputs:clearcoat"
  TypedAttributeWithFallback<Animatable<float>> clearcoatRoughness{0.01f};  // "inputs:clearcoatRoughness"
  TypedAttributeWithFallback<Animatable<float>> roughness{0.5f};  // "inputs:roughness"
  TypedAttributeWithFallback<Animatable<float>> opacity{1.0f};  // "inputs:opacity"

  TypedAttributeWithFallback<Animatable<OpacityMode>> opacityMode{OpacityMode::Opacity};  // "inputs:opacityMode"

  TypedAttributeWithFallback<Animatable<float>> opacityThreshold{0.0f};  // "inputs:opacityThreshold"
  TypedAttributeWithFallback<Animatable<float>> ior{1.5f};  // "inputs:ior"

  TypedAttributeWithFallback<Animatable<value::normal3f>> normal{value::normal3f{0.0f, 0.0f, 1.0f}}; // "inputs:normal"
  TypedAttributeWithFallback<Animatable<float>> displacement{0.0f}; // "inputs:displacement"
  TypedAttributeWithFallback<Animatable<float>> occlusion{1.0f}; // "inputs:occlusion"

  ///
  /// Outputs
  ///
  /// No value assigned.
  ///
  TypedTerminalAttribute<value::token> outputsSurface; // "token outputs:surface"
  TypedTerminalAttribute<value::token> outputsDisplacement; // "token outputs:displacement"

};

// Transform texture coordinates.
struct UsdTransform2d : ShaderNode {

  TypedAttributeWithFallback<Animatable<value::float2>> in{value::float2{0.0f, 0.0f}};  // "inputs:in" Usually connected to UsdPrimvarReader_float2

  // Transform is TRS order:
  //
  // result = in * scale * rotate * translation (in USD's notation(row-major, pre-multiply matrix mul))
  // result = translation * rotate * scale * in (in OpenGL's notation(column-major, post-multiply matrix mul))

  TypedAttributeWithFallback<Animatable<float>> rotation{0.0f};  // "inputs:rotation" CCW, in degree.
  TypedAttributeWithFallback<Animatable<value::float2>> scale{value::float2{1.0f, 1.0f}};  // "inputs:scale"
  TypedAttributeWithFallback<Animatable<value::float2>> translation{value::float2{0.0f, 0.0f}};  // "inputs:translation"


  ///
  /// Outputs
  ///
  TypedTerminalAttribute<value::float2> result; // "float2 outputs:result"

};

// OpenPBR Surface shader
// OpenPBR is a physically-based shading model developed by the Academy Software Foundation
// https://github.com/AcademySoftwareFoundation/OpenPBR
struct OpenPBRSurface : ShaderNode {

  // Base layer properties
  TypedAttributeWithFallback<Animatable<float>> base_weight{1.0f}; // "inputs:base_weight"
  TypedAttributeWithFallback<Animatable<value::color3f>> base_color{value::color3f{0.8f, 0.8f, 0.8f}}; // "inputs:base_color"
  TypedAttributeWithFallback<Animatable<float>> base_roughness{0.0f}; // "inputs:base_roughness"
  TypedAttributeWithFallback<Animatable<float>> base_metalness{0.0f}; // "inputs:base_metalness"
  TypedAttributeWithFallback<Animatable<float>> base_diffuse_roughness{0.0f}; // "inputs:base_diffuse_roughness"

  // Specular properties
  TypedAttributeWithFallback<Animatable<float>> specular_weight{1.0f}; // "inputs:specular_weight"
  TypedAttributeWithFallback<Animatable<value::color3f>> specular_color{value::color3f{1.0f, 1.0f, 1.0f}}; // "inputs:specular_color"
  TypedAttributeWithFallback<Animatable<float>> specular_roughness{0.3f}; // "inputs:specular_roughness"
  TypedAttributeWithFallback<Animatable<float>> specular_ior{1.5f}; // "inputs:specular_ior"
  TypedAttributeWithFallback<Animatable<float>> specular_ior_level{0.5f}; // "inputs:specular_ior_level"
  TypedAttributeWithFallback<Animatable<float>> specular_anisotropy{0.0f}; // "inputs:specular_anisotropy"
  TypedAttributeWithFallback<Animatable<float>> specular_rotation{0.0f}; // "inputs:specular_rotation"
  TypedAttributeWithFallback<Animatable<float>> specular_roughness_anisotropy{0.0f}; // "inputs:specular_roughness_anisotropy"

  // Transmission properties
  TypedAttributeWithFallback<Animatable<float>> transmission_weight{0.0f}; // "inputs:transmission_weight"
  TypedAttributeWithFallback<Animatable<value::color3f>> transmission_color{value::color3f{1.0f, 1.0f, 1.0f}}; // "inputs:transmission_color"
  TypedAttributeWithFallback<Animatable<float>> transmission_depth{0.0f}; // "inputs:transmission_depth"
  TypedAttributeWithFallback<Animatable<value::color3f>> transmission_scatter{value::color3f{0.0f, 0.0f, 0.0f}}; // "inputs:transmission_scatter"
  TypedAttributeWithFallback<Animatable<float>> transmission_scatter_anisotropy{0.0f}; // "inputs:transmission_scatter_anisotropy"
  TypedAttributeWithFallback<Animatable<float>> transmission_dispersion{0.0f}; // "inputs:transmission_dispersion"
  TypedAttributeWithFallback<Animatable<float>> transmission_dispersion_abbe_number{0.0f}; // "inputs:transmission_dispersion_abbe_number"
  TypedAttributeWithFallback<Animatable<float>> transmission_dispersion_scale{0.0f}; // "inputs:transmission_dispersion_scale"

  // Subsurface properties
  TypedAttributeWithFallback<Animatable<float>> subsurface_weight{0.0f}; // "inputs:subsurface_weight"
  TypedAttributeWithFallback<Animatable<value::color3f>> subsurface_color{value::color3f{0.8f, 0.8f, 0.8f}}; // "inputs:subsurface_color"
  TypedAttributeWithFallback<Animatable<float>> subsurface_radius{1.0f}; // "inputs:subsurface_radius"
  TypedAttributeWithFallback<Animatable<value::color3f>> subsurface_radius_scale{value::color3f{1.0f, 1.0f, 1.0f}}; // "inputs:subsurface_radius_scale"
  TypedAttributeWithFallback<Animatable<float>> subsurface_scale{1.0f}; // "inputs:subsurface_scale"
  TypedAttributeWithFallback<Animatable<float>> subsurface_anisotropy{0.0f}; // "inputs:subsurface_anisotropy"
  TypedAttributeWithFallback<Animatable<float>> subsurface_scatter_anisotropy{0.0f}; // "inputs:subsurface_scatter_anisotropy"

  // Sheen properties
  TypedAttributeWithFallback<Animatable<float>> sheen_weight{0.0f}; // "inputs:sheen_weight"
  TypedAttributeWithFallback<Animatable<value::color3f>> sheen_color{value::color3f{1.0f, 1.0f, 1.0f}}; // "inputs:sheen_color"
  TypedAttributeWithFallback<Animatable<float>> sheen_roughness{0.3f}; // "inputs:sheen_roughness"

  // Fuzz properties - velvet/fabric-like appearance
  TypedAttributeWithFallback<Animatable<float>> fuzz_weight{0.0f}; // "inputs:fuzz_weight"
  TypedAttributeWithFallback<Animatable<value::color3f>> fuzz_color{value::color3f{1.0f, 1.0f, 1.0f}}; // "inputs:fuzz_color"
  TypedAttributeWithFallback<Animatable<float>> fuzz_roughness{0.5f}; // "inputs:fuzz_roughness"

  // Thin film properties - iridescence from thin film interference
  TypedAttributeWithFallback<Animatable<float>> thin_film_weight{0.0f}; // "inputs:thin_film_weight"
  TypedAttributeWithFallback<Animatable<float>> thin_film_thickness{500.0f}; // "inputs:thin_film_thickness" (nanometers)
  TypedAttributeWithFallback<Animatable<float>> thin_film_ior{1.5f}; // "inputs:thin_film_ior"

  // Coat properties
  TypedAttributeWithFallback<Animatable<float>> coat_weight{0.0f}; // "inputs:coat_weight"
  TypedAttributeWithFallback<Animatable<value::color3f>> coat_color{value::color3f{1.0f, 1.0f, 1.0f}}; // "inputs:coat_color"
  TypedAttributeWithFallback<Animatable<float>> coat_roughness{0.0f}; // "inputs:coat_roughness"
  TypedAttributeWithFallback<Animatable<float>> coat_anisotropy{0.0f}; // "inputs:coat_anisotropy"
  TypedAttributeWithFallback<Animatable<float>> coat_rotation{0.0f}; // "inputs:coat_rotation"
  TypedAttributeWithFallback<Animatable<float>> coat_ior{1.5f}; // "inputs:coat_ior"
  TypedAttributeWithFallback<Animatable<float>> coat_affect_color{0.0f}; // "inputs:coat_affect_color"
  TypedAttributeWithFallback<Animatable<float>> coat_affect_roughness{0.0f}; // "inputs:coat_affect_roughness"
  TypedAttributeWithFallback<Animatable<float>> coat_roughness_anisotropy{0.0f}; // "inputs:coat_roughness_anisotropy"
  TypedAttributeWithFallback<Animatable<float>> coat_darkening{0.0f}; // "inputs:coat_darkening"

  // Emission properties
  TypedAttributeWithFallback<Animatable<float>> emission_luminance{0.0f}; // "inputs:emission_luminance"
  TypedAttributeWithFallback<Animatable<value::color3f>> emission_color{value::color3f{1.0f, 1.0f, 1.0f}}; // "inputs:emission_color"

  // Geometry properties
  TypedAttributeWithFallback<Animatable<float>> opacity{1.0f}; // "inputs:opacity" or "inputs:geometry_opacity" (maps to alpha in Three.js)
  TypedAttributeWithFallback<Animatable<value::normal3f>> normal{value::normal3f{0.0f, 0.0f, 1.0f}}; // "inputs:normal"
  TypedAttributeWithFallback<Animatable<value::vector3f>> tangent{value::vector3f{1.0f, 0.0f, 0.0f}}; // "inputs:tangent"

  ///
  /// Outputs
  ///
  TypedTerminalAttribute<value::token> surface; // "token outputs:surface"

};

// Shader Prim
struct Shader : UsdShadePrim {

  std::string info_id;  // ShaderNode type.

  // Holds the concrete ShaderNode (UsdPreviewSurface, UsdUVTexture,
  // UsdPrimvarReader_*, OpenPBRSurface, MaterialX nodes, etc.)
  // stored as value::Value for type-erased polymorphism.
  value::Value value;

};

// import DEFINE_TYPE_TRAIT and DEFINE_ROLE_TYPE_TRAIT
#include "define-type-trait.inc"

namespace value {

// Mateiral Prim
DEFINE_TYPE_TRAIT(Material, kMaterial,
                  TYPE_ID_MATERIAL, 1);

// Shader Prim
DEFINE_TYPE_TRAIT(Shader, kShader,
                  TYPE_ID_SHADER, 1);

// NodeGraph Prim
DEFINE_TYPE_TRAIT(NodeGraph, kNodeGraph,
                  TYPE_ID_NODEGRAPH, 1);


// ShaderNodes
DEFINE_TYPE_TRAIT(ShaderNode, kShaderNode,
                  TYPE_ID_IMAGING_SHADER_NODE, 1);
DEFINE_TYPE_TRAIT(UsdPreviewSurface, kUsdPreviewSurface,
                  TYPE_ID_IMAGING_PREVIEWSURFACE, 1);
DEFINE_TYPE_TRAIT(UsdUVTexture, kUsdUVTexture, TYPE_ID_IMAGING_UVTEXTURE, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_float, kUsdPrimvarReader_float,
                  TYPE_ID_IMAGING_PRIMVAR_READER_FLOAT, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_float2, kUsdPrimvarReader_float2,
                  TYPE_ID_IMAGING_PRIMVAR_READER_FLOAT2, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_float3, kUsdPrimvarReader_float3,
                  TYPE_ID_IMAGING_PRIMVAR_READER_FLOAT3, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_float4, kUsdPrimvarReader_float4,
                  TYPE_ID_IMAGING_PRIMVAR_READER_FLOAT4, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_int, kUsdPrimvarReader_int,
                  TYPE_ID_IMAGING_PRIMVAR_READER_INT, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_string, kUsdPrimvarReader_string,
                  TYPE_ID_IMAGING_PRIMVAR_READER_STRING, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_vector, kUsdPrimvarReader_vector,
                  TYPE_ID_IMAGING_PRIMVAR_READER_VECTOR, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_normal, kUsdPrimvarReader_normal,
                  TYPE_ID_IMAGING_PRIMVAR_READER_NORMAL, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_point, kUsdPrimvarReader_point,
                  TYPE_ID_IMAGING_PRIMVAR_READER_POINT, 1);
DEFINE_TYPE_TRAIT(UsdPrimvarReader_matrix, kUsdPrimvarReader_matrix,
                  TYPE_ID_IMAGING_PRIMVAR_READER_MATRIX, 1);
DEFINE_TYPE_TRAIT(UsdTransform2d, kUsdTransform2d,
                  TYPE_ID_IMAGING_TRANSFORM_2D, 1);
DEFINE_TYPE_TRAIT(OpenPBRSurface, kOpenPBRSurface,
                  TYPE_ID_IMAGING_OPENPBR_SURFACE, 1);

DEFINE_TYPE_TRAIT(MaterialBinding, "MaterialBindingAPI",
                  TYPE_ID_MATERIAL_BINDING, 1);

DEFINE_TYPE_TRAIT(MaterialXConfigAPI, kMaterialXConfigAPI,
                  TYPE_ID_MATERIALX_CONFIG_API, 1);

DEFINE_TYPE_TRAIT(MaterialUEConfigAPI, kMaterialUEConfigAPI,
                  TYPE_ID_MATERIALX_CONFIG_API + 1, 1);

// TypeTraits for SourceColorSpace enum
template <>
struct TypeTraits<UsdUVTexture::SourceColorSpace> {
  static constexpr uint32_t type_id() {
    return TYPE_ID_IMAGING_SOURCE_COLORSPACE;
  }
  static constexpr bool is_a_pod_type() { return true; }
  static constexpr bool is_a_container() { return false; }
  static constexpr size_t ndim() { return 0; }
  static constexpr const char* type_name() { return "UsdUVTexture::SourceColorSpace"; }
  // Use the same underlying type ID as int since enum is basically an int
  static constexpr uint32_t underlying_type_id() { return type_id(); }
};

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
