// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data-math.hh"
#include <cstdint>
#include <string>

namespace lightusd { namespace tydra { namespace next {

//
struct ShaderParam {
  // If texture_id >= 0, use texture; otherwise use value
  int32_t texture_id = -1;

  // Scalar/vector value (when texture_id < 0)
  Float4 value = {0, 0, 0, 1};

  bool is_texture() const { return texture_id >= 0; }
  bool is_value() const { return texture_id < 0; }

  float as_float() const { return value.x; }
  Float3 as_float3() const { return Float3(value.x, value.y, value.z); }
  Float4 as_float4() const { return value; }
};

//
// PreviewSurface shader parameters
//
struct PreviewSurfaceShader {
  ShaderParam diffuse_color = {-1, {0.18f, 0.18f, 0.18f, 1.0f}};
  ShaderParam emissive_color = {-1, {0, 0, 0, 1}};
  // UsdPreviewSurface spec fallback is (0,0,0) — only meaningful when
  // useSpecularWorkflow is on.
  ShaderParam specular_color = {-1, {0, 0, 0, 1}};

  ShaderParam metallic = {-1, {0, 0, 0, 0}};
  ShaderParam roughness = {-1, {0.5f, 0, 0, 0}};
  ShaderParam clearcoat = {-1, {0, 0, 0, 0}};
  ShaderParam clearcoat_roughness = {-1, {0.01f, 0, 0, 0}};

  ShaderParam opacity = {-1, {1, 0, 0, 0}};
  ShaderParam opacity_threshold = {-1, {0, 0, 0, 0}};
  ShaderParam ior = {-1, {1.5f, 0, 0, 0}};

  ShaderParam normal = {-1, {0, 0, 1, 0}};
  ShaderParam displacement = {-1, {0, 0, 0, 0}};
  ShaderParam occlusion = {-1, {1, 0, 0, 0}};

  bool use_specular_workflow = false;
};

//
// OpenPBR Surface shader (MaterialX)
//
struct OpenPBRSurfaceShader {
  // Base
  ShaderParam base_weight = {-1, {1, 0, 0, 0}};
  ShaderParam base_color = {-1, {0.8f, 0.8f, 0.8f, 1}};
  ShaderParam base_roughness = {-1, {0, 0, 0, 0}};
  ShaderParam base_diffuse_roughness = {-1, {0, 0, 0, 0}};
  ShaderParam base_metalness = {-1, {0, 0, 0, 0}};

  // Specular
  ShaderParam specular_weight = {-1, {1, 0, 0, 0}};
  ShaderParam specular_color = {-1, {1, 1, 1, 1}};
  ShaderParam specular_roughness = {-1, {0.3f, 0, 0, 0}};
  ShaderParam specular_ior = {-1, {1.5f, 0, 0, 0}};
  ShaderParam specular_anisotropy = {-1, {0, 0, 0, 0}};
  ShaderParam specular_roughness_anisotropy = {-1, {0, 0, 0, 0}};
  ShaderParam specular_rotation = {-1, {0, 0, 0, 0}};

  // Transmission
  ShaderParam transmission_weight = {-1, {0, 0, 0, 0}};
  ShaderParam transmission_color = {-1, {1, 1, 1, 1}};
  ShaderParam transmission_depth = {-1, {0, 0, 0, 0}};
  ShaderParam transmission_scatter = {-1, {0, 0, 0, 1}};
  ShaderParam transmission_scatter_anisotropy = {-1, {0, 0, 0, 0}};
  ShaderParam transmission_dispersion = {-1, {0, 0, 0, 0}};
  ShaderParam transmission_dispersion_scale = {-1, {0, 0, 0, 0}};

  // Subsurface
  ShaderParam subsurface_weight = {-1, {0, 0, 0, 0}};
  ShaderParam subsurface_color = {-1, {0.8f, 0.8f, 0.8f, 1}};
  ShaderParam subsurface_radius = {-1, {1, 1, 1, 0}};
  ShaderParam subsurface_scale = {-1, {1, 0, 0, 0}};
  ShaderParam subsurface_anisotropy = {-1, {0, 0, 0, 0}};

  // Coat
  ShaderParam coat_weight = {-1, {0, 0, 0, 0}};
  ShaderParam coat_color = {-1, {1, 1, 1, 1}};
  ShaderParam coat_roughness = {-1, {0, 0, 0, 0}};
  ShaderParam coat_rotation = {-1, {0, 0, 0, 0}};
  ShaderParam coat_ior = {-1, {1.5f, 0, 0, 0}};
  ShaderParam coat_affect_color = {-1, {0, 0, 0, 0}};
  ShaderParam coat_affect_roughness = {-1, {0, 0, 0, 0}};
  ShaderParam coat_anisotropy = {-1, {0, 0, 0, 0}};
  ShaderParam coat_roughness_anisotropy = {-1, {0, 0, 0, 0}};
  // OpenPBR's independently authored coat-layer normal. Keep this separate
  // from `normal`: a missing coat normal falls back to the surface normal in
  // consumers, but an authored map must retain its own image/UV descriptor.
  ShaderParam coat_normal = {-1, {0, 0, 1, 0}};

  // Sheen
  ShaderParam sheen_weight = {-1, {0, 0, 0, 0}};
  ShaderParam sheen_color = {-1, {1, 1, 1, 1}};
  ShaderParam sheen_roughness = {-1, {0.3f, 0, 0, 0}};
  ShaderParam fuzz_weight = {-1, {0, 0, 0, 0}};
  ShaderParam fuzz_color = {-1, {1, 1, 1, 1}};
  ShaderParam fuzz_roughness = {-1, {0.5f, 0, 0, 0}};
  bool fuzz_authored = false;

  // Thin-film / iridescence.
  ShaderParam thin_film_weight = {-1, {0, 0, 0, 0}};
  ShaderParam thin_film_thickness = {-1, {0, 0, 0, 0}};
  ShaderParam thin_film_ior = {-1, {1.5f, 0, 0, 0}};

  // Emission
  ShaderParam emission_luminance = {-1, {0, 0, 0, 0}};
  ShaderParam emission_color = {-1, {1, 1, 1, 1}};

  // Geometry
  ShaderParam opacity = {-1, {1, 0, 0, 0}};
  ShaderParam thin_walled = {-1, {0, 0, 0, 0}};
  ShaderParam normal = {-1, {0, 0, 1, 0}};
  ShaderParam tangent = {-1, {1, 0, 0, 0}};
  ShaderParam coat_tangent = {-1, {1, 0, 0, 0}};
  // Scalar metadata carried by common MaterialX utility nodes feeding the
  // geometry inputs. Renderers without a programmable MaterialX path can
  // still reproduce normal-map strength and retain tangent-frame intent.
  float normal_map_scale = 1.0f;
  float coat_normal_map_scale = 1.0f;
  float tangent_rotation = 0.0f;
  float coat_tangent_rotation = 0.0f;
  // Height/displacement output carried by MaterialX standard_surface graphs.
  // OpenPBR itself does not define surface displacement, but retaining it here
  // lets render consumers use the same geometry path as UsdPreviewSurface.
  ShaderParam displacement = {-1, {0, 0, 0, 0}};

  // MaterialX node graph as JSON (optional)
  std::string nodegraph_json;
};

//
// RenderMaterial
//
enum class MaterialDiagnosticKind : uint8_t {
  UnsupportedShader = 0,
  UnsupportedMaterialXNode,
  DegradedMaterial
};

}}} // namespace lightusd::tydra::next
