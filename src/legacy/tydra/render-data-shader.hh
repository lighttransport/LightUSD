// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file render-data-shader.hh
/// @brief Shader and material types extracted from render-data.hh
///
/// Contains ShaderParam, PreviewSurfaceShader, OpenPBRSurfaceShader,
/// RenderMaterial, and supporting spectral data structures.
///

#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "nonstd/optional.hpp"
#include "value-types.hh"

namespace lightusd {
namespace tydra {

// GLSL like data types
using vec2 = value::float2;
using vec3 = value::float3;
using vec4 = value::float4;
using quat = value::float4; // (x, y, z, w)
using mat2 = value::matrix2f;
using mat3 = value::matrix3f;
using mat4 = value::matrix4f;
using dmat4 = value::matrix4d;

// ============================================================================
// LTE SpectralAPI Support
// Spectral data structures for wavelength-dependent material properties
// See doc/lte_spectral_api.md for specification
// ============================================================================

///
/// Interpolation method for spectral data
///
enum class SpectralInterpolation {
  Linear,    ///< Piecewise linear interpolation (default)
  Held,      ///< USD Held interpolation (step function)
  Cubic,     ///< Piecewise cubic interpolation (smooth)
  Sellmeier, ///< Sellmeier equation (for IOR data only)
};

///
/// Standard illuminant presets for wavelength:emission
///
enum class IlluminantPreset {
  None,  ///< No preset, use explicit SPD values
  A,     ///< CIE Standard Illuminant A (incandescent/tungsten, 2856K)
  D50,   ///< CIE Standard Illuminant D50 (horizon daylight, 5003K)
  D65,   ///< CIE Standard Illuminant D65 (noon daylight, 6504K)
  E,     ///< CIE Standard Illuminant E (equal energy)
  F1,    ///< CIE Fluorescent Illuminant F1 (daylight fluorescent)
  F2,    ///< CIE Fluorescent Illuminant F2 (cool white fluorescent)
  F7,    ///< CIE Fluorescent Illuminant F7 (D65 simulator)
  F11,   ///< CIE Fluorescent Illuminant F11 (narrow-band cool white)
};

///
/// Wavelength unit for spectral data
///
enum class WavelengthUnit {
  Nanometers,   ///< nanometers (nm), default, range [380, 780]
  Micrometers,  ///< micrometers (um), range [0.38, 0.78]
};

///
/// Spectral data container
/// Stores (wavelength, value) pairs for wavelength-dependent properties
///
struct SpectralData {
  std::vector<vec2> samples;  ///< (wavelength, value) pairs
  SpectralInterpolation interpolation{SpectralInterpolation::Linear};
  WavelengthUnit unit{WavelengthUnit::Nanometers};

  /// Check if spectral data is present
  bool has_data() const { return !samples.empty(); }

  /// Get number of samples
  size_t size() const { return samples.size(); }

  /// Evaluate spectral value at given wavelength using interpolation
  float evaluate(float wavelength) const;

  /// Convert wavelength to nanometers (for internal processing)
  float to_nanometers(float wavelength) const {
    if (unit == WavelengthUnit::Micrometers) {
      return wavelength * 1000.0f;
    }
    return wavelength;
  }
};

///
/// Spectral IOR data with Sellmeier coefficient support
///
struct SpectralIOR {
  std::vector<vec2> samples;  ///< (wavelength, IOR) pairs or Sellmeier coefficients
  SpectralInterpolation interpolation{SpectralInterpolation::Linear};
  WavelengthUnit unit{WavelengthUnit::Nanometers};

  /// Sellmeier coefficients (B1, B2, B3, C1, C2, C3)
  /// Used when interpolation == Sellmeier
  /// Note: C1, C2, C3 are in [um^2]
  float sellmeier_B1{0.0f}, sellmeier_B2{0.0f}, sellmeier_B3{0.0f};
  float sellmeier_C1{0.0f}, sellmeier_C2{0.0f}, sellmeier_C3{0.0f};

  bool has_data() const {
    return !samples.empty() || interpolation == SpectralInterpolation::Sellmeier;
  }

  /// Evaluate IOR at given wavelength
  float evaluate(float wavelength_nm) const;
};

///
/// Spectral emission data for light sources
///
struct SpectralEmission {
  std::vector<vec2> samples;  ///< (wavelength, irradiance) pairs
  SpectralInterpolation interpolation{SpectralInterpolation::Linear};
  WavelengthUnit unit{WavelengthUnit::Nanometers};
  IlluminantPreset preset{IlluminantPreset::None};

  bool has_data() const {
    return !samples.empty() || preset != IlluminantPreset::None;
  }

  /// Evaluate emission at given wavelength
  /// Returns irradiance in W m^-2 nm^-1 (normalized to nanometers)
  float evaluate(float wavelength_nm) const;
};

// String conversion functions for spectral types
std::string to_string(SpectralInterpolation interp);
std::string to_string(IlluminantPreset preset);
std::string to_string(WavelengthUnit unit);

// ============================================================================
// End of LTE SpectralAPI Support
// ============================================================================

// workaround for GCC
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

// T or TextureId
template <typename T>
class ShaderParam {
 public:
  ShaderParam() = default;
  ShaderParam(const T &t) : value(t) { }

  bool is_texture() const { return texture_id >= 0; }

  template <typename STy>
  void set_value(const STy &val) {
    // Currently we assume T == Sty.
    // TODO: support more type variant
    static_assert(value::TypeTraits<T>::underlying_type_id() ==
                      value::TypeTraits<STy>::underlying_type_id(),
                  "");
    static_assert(sizeof(T) >= sizeof(STy), "");
    memcpy(&value, &val, sizeof(T));
  }

 //private:
  T value{};
  int32_t texture_id{-1};  // negative = invalid
};

// UsdPreviewSurface
class PreviewSurfaceShader {
 public:
  bool useSpecularWorkflow{false};

  ShaderParam<vec3> diffuseColor{{0.18f, 0.18f, 0.18f}};
  ShaderParam<vec3> emissiveColor{{0.0f, 0.0f, 0.0f}};
  ShaderParam<vec3> specularColor{{0.0f, 0.0f, 0.0f}};
  ShaderParam<float> metallic{0.0f};
  ShaderParam<float> roughness{0.5f};
  ShaderParam<float> clearcoat{0.0f};
  ShaderParam<float> clearcoatRoughness{0.01f};
  ShaderParam<float> opacity{1.0f};
  ShaderParam<float> opacityThreshold{0.0f};
  ShaderParam<float> ior{1.5f};
  ShaderParam<vec3> normal{{0.0f, 0.0f, 1.0f}};
  ShaderParam<float> displacement{0.0f};
  // UsdPreviewSurface spec: `occlusion` fallback is 1.0 (fully unoccluded).
  ShaderParam<float> occlusion{1.0f};

  // LTE SpectralAPI: Optional spectral properties
  // Only exported if has_data() returns true
  nonstd::optional<SpectralData> spd_reflectance;  ///< wavelength:reflectance
  nonstd::optional<SpectralIOR> spd_ior;           ///< wavelength:ior

  uint64_t handle{0};  // Handle ID for Graphics API. 0 = invalid

  /// Check if material has spectral reflectance data
  bool hasSpectralReflectance() const {
    return spd_reflectance.has_value() && spd_reflectance->has_data();
  }

  /// Check if material has spectral IOR data
  bool hasSpectralIOR() const {
    return spd_ior.has_value() && spd_ior->has_data();
  }
};

// MaterialX OpenPBR Surface shader optimized for WebGL/Vulkan rendering
class OpenPBRSurfaceShader {
 public:
  // Base layer - fundamental surface properties
  ShaderParam<float> base_weight{1.0f};
  ShaderParam<vec3> base_color{{0.8f, 0.8f, 0.8f}};
  ShaderParam<float> base_roughness{0.0f};
  ShaderParam<float> base_metalness{0.0f};
  ShaderParam<float> base_diffuse_roughness{0.0f};  // Oren-Nayar diffuse roughness

  // Specular layer - dielectric reflection
  ShaderParam<float> specular_weight{1.0f};
  ShaderParam<vec3> specular_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> specular_roughness{0.3f};
  ShaderParam<float> specular_ior{1.5f};
  ShaderParam<float> specular_ior_level{0.5f};
  ShaderParam<float> specular_anisotropy{0.0f};
  ShaderParam<float> specular_rotation{0.0f};
  ShaderParam<float> specular_roughness_anisotropy{0.0f};

  // Transmission - transparency and refraction
  ShaderParam<float> transmission_weight{0.0f};
  ShaderParam<vec3> transmission_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> transmission_depth{0.0f};
  ShaderParam<vec3> transmission_scatter{{0.0f, 0.0f, 0.0f}};
  ShaderParam<float> transmission_scatter_anisotropy{0.0f};
  ShaderParam<float> transmission_dispersion{0.0f};
  ShaderParam<float> transmission_dispersion_abbe_number{0.0f};
  ShaderParam<float> transmission_dispersion_scale{0.0f};

  // Subsurface scattering
  ShaderParam<float> subsurface_weight{0.0f};
  ShaderParam<vec3> subsurface_color{{0.8f, 0.8f, 0.8f}};
  ShaderParam<float> subsurface_radius{1.0f};
  ShaderParam<vec3> subsurface_radius_scale{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> subsurface_scale{1.0f};
  ShaderParam<float> subsurface_anisotropy{0.0f};
  ShaderParam<float> subsurface_scatter_anisotropy{0.0f};

  // Sheen - fabric-like reflection
  ShaderParam<float> sheen_weight{0.0f};
  ShaderParam<vec3> sheen_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> sheen_roughness{0.3f};

  // Fuzz - velvet/fabric-like appearance
  ShaderParam<float> fuzz_weight{0.0f};
  ShaderParam<vec3> fuzz_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> fuzz_roughness{0.5f};

  // Thin film - iridescence from thin film interference
  ShaderParam<float> thin_film_weight{0.0f};
  ShaderParam<float> thin_film_thickness{500.0f};  // in nanometers
  ShaderParam<float> thin_film_ior{1.5f};

  // Coat layer - clear coat over surface
  ShaderParam<float> coat_weight{0.0f};
  ShaderParam<vec3> coat_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> coat_roughness{0.0f};
  ShaderParam<float> coat_anisotropy{0.0f};
  ShaderParam<float> coat_rotation{0.0f};
  ShaderParam<float> coat_ior{1.5f};
  ShaderParam<float> coat_affect_color{0.0f};
  ShaderParam<float> coat_affect_roughness{0.0f};
  ShaderParam<float> coat_roughness_anisotropy{0.0f};
  ShaderParam<float> coat_darkening{0.0f};

  // Emission - light emission
  ShaderParam<float> emission_luminance{0.0f};
  ShaderParam<vec3> emission_color{{1.0f, 1.0f, 1.0f}};

  // Geometry modifiers
  ShaderParam<float> opacity{1.0f};  // "opacity" or "geometry_opacity" (maps to alpha in Three.js)
  ShaderParam<vec3> normal{{0.0f, 0.0f, 1.0f}};
  ShaderParam<vec3> tangent{{1.0f, 0.0f, 0.0f}};

  // Tangent rotation for anisotropic materials (in degrees)
  // Blender exports tangent rotation via ND_rotate3d_vector3 node with -90 degrees
  // This value is extracted from the MaterialX NodeGraph during conversion
  // 0.0 = no rotation, -90.0 = typical Blender anisotropic rotation
  float tangent_rotation{0.0f};

  // Normal map scale factor (from ND_normalmap_float node's scale input)
  // 1.0 = default, used for bump strength adjustment
  float normal_map_scale{1.0f};

  // Coat normal and tangent for separate coat layer normal mapping
  ShaderParam<vec3> coat_normal{{0.0f, 0.0f, 1.0f}};
  ShaderParam<vec3> coat_tangent{{1.0f, 0.0f, 0.0f}};
  // MaterialX standard_surface height input. OpenPBR has no corresponding
  // lobe, but render consumers share this geometry path with PreviewSurface.
  ShaderParam<float> displacement{0.0f};
  float coat_tangent_rotation{0.0f};
  float coat_normal_map_scale{1.0f};

  // LTE SpectralAPI: Optional spectral properties
  // Only exported to JSON if has_data() returns true
  // MaterialX property names use "spd_" prefix (e.g., "spd_reflectance", "spd_ior")
  nonstd::optional<SpectralData> spd_reflectance;  ///< wavelength:reflectance -> spd_reflectance
  nonstd::optional<SpectralIOR> spd_ior;           ///< wavelength:ior -> spd_ior
  nonstd::optional<SpectralEmission> spd_emission; ///< wavelength:emission -> spd_emission

  uint64_t handle{0};  // Handle ID for Graphics API. 0 = invalid

  // MaterialX Node Graph representation as JSON
  // Stores the complete node-based shader graph for reconstruction in JS/WASM
  // Schema follows MaterialX XML structure for compatibility
  // Empty string if no node graph exists (direct parameter values only)
  std::string nodeGraphJson;

  /// Check if material has spectral reflectance data
  bool hasSpectralReflectance() const {
    return spd_reflectance.has_value() && spd_reflectance->has_data();
  }

  /// Check if material has spectral IOR data
  bool hasSpectralIOR() const {
    return spd_ior.has_value() && spd_ior->has_data();
  }

  /// Check if material has spectral emission data
  bool hasSpectralEmission() const {
    return spd_emission.has_value() && spd_emission->has_data();
  }

  /// Check if material has any spectral data
  bool hasAnySpectralData() const {
    return hasSpectralReflectance() || hasSpectralIOR() || hasSpectralEmission();
  }
};

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Material tag for render pass classification.
// Matches OpenUSD hdSt's material tag computation logic.
enum class MaterialTag {
  Opaque,       // Default opaque rendering
  Translucent,  // Alpha blending / transmission (sorted back-to-front)
  Masked,       // Alpha cutout (opacityThreshold > 0, UsdPreviewSurface only)
};

struct RenderMaterialXConfig {
  bool authored{false};
  std::string version{"1.38"};
  std::string name_space;
  std::string colorspace{"lin_rec709"};
  std::string source_uri;
};

// Material + Shader
// Supports dual material representation: UsdPreviewSurface and/or MaterialX OpenPBR
struct RenderMaterial {
  std::string name;  // elementName in USD (e.g. "pbrMat")
  std::string
      abs_path;  // abosolute Prim path in USD (e.g. "/_material/scope/pbrMat")
  std::string display_name;

  // Material can have UsdPreviewSurface, OpenPBR, or both
  // Use nonstd::optional to allow either/both/none
  nonstd::optional<PreviewSurfaceShader> surfaceShader;  // UsdPreviewSurface
  nonstd::optional<OpenPBRSurfaceShader> openPBRShader;  // MaterialX OpenPBR
  RenderMaterialXConfig materialXConfig;

  // Displacement shader output.
  // For UsdPreviewSurface, displacement is part of surfaceShader.
  // For MaterialX, a separate displacement shader may be connected.
  bool has_displacement{false};
  std::string displacement_shader_path;  // Prim path of displacement shader

  // Volume shader output.
  bool has_volume{false};
  std::string volume_shader_path;  // Prim path of volume shader
  // Homogeneous fallback constants retained for renderers that bind a volume
  // output directly to a surface material (rather than a UsdVol field).
  float volume_density{0.0f};
  float volume_albedo[3] = {0.5f, 0.5f, 0.5f};
  float volume_emission_color[3] = {0.0f, 0.0f, 0.0f};
  float volume_emission_scale{0.0f};

  // Material tag for render pass sorting (opaque vs transparent).
  // Computed by computeMaterialTag() after shader conversion.
  MaterialTag materialTag{MaterialTag::Opaque};

  uint64_t handle{0};  // Handle ID for Graphics API. 0 = invalid

  // Helper methods to check which materials are available
  bool hasUsdPreviewSurface() const { return surfaceShader.has_value(); }
  bool hasOpenPBR() const { return openPBRShader.has_value(); }
  bool hasBothMaterials() const { return hasUsdPreviewSurface() && hasOpenPBR(); }

  // Compute material tag from shader parameters.
  // Follows OpenUSD hdSt logic:
  //   UsdPreviewSurface: opacityThreshold > 0 → Masked, opacity < 1 → Translucent
  //   OpenPBR: transmission_weight != 0 or opacity != 1 → Translucent
  void computeMaterialTag() {
    // Prefer OpenPBR if available (MaterialX path)
    if (openPBRShader.has_value()) {
      const auto &s = *openPBRShader;
      if (s.transmission_weight.value > 0.0f || s.opacity.value < 1.0f ||
          s.opacity.is_texture()) {
        materialTag = MaterialTag::Translucent;
      } else {
        materialTag = MaterialTag::Opaque;
      }
      return;
    }
    // UsdPreviewSurface
    if (surfaceShader.has_value()) {
      const auto &s = *surfaceShader;
      if (s.opacityThreshold.value > 0.0f) {
        materialTag = MaterialTag::Masked;
      } else if (s.opacity.value < 1.0f || s.opacity.is_texture()) {
        materialTag = MaterialTag::Translucent;
      } else {
        materialTag = MaterialTag::Opaque;
      }
      return;
    }
  }
};

}  // namespace tydra
}  // namespace lightusd
