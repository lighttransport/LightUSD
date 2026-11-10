// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// JSON/XML serialization of render materials and lights for the WASM binding
// (LightUSDLoader's material JSON). The binding fills this small, self-
// contained model from the core tydra RenderScene; field names, types and
// defaults are those of the pre-1.1 serializer so the output is unchanged.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "nonstd/expected.hpp"
#include "nonstd/optional.hpp"

namespace lightusd {
namespace web_core {
namespace matser {

using vec3 = std::array<float, 3>;

template <typename T>
struct ShaderParam {
  ShaderParam() = default;
  ShaderParam(const T &t) : value(t) {}
  bool is_texture() const { return texture_id >= 0; }
  T value{};
  int32_t texture_id{-1};  // negative = invalid
};

// UsdPreviewSurface
struct PreviewSurfaceShader {
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
  ShaderParam<float> occlusion{1.0f};
};

// MaterialX OpenPBR surface
struct OpenPBRSurfaceShader {
  ShaderParam<float> base_weight{1.0f};
  ShaderParam<vec3> base_color{{0.8f, 0.8f, 0.8f}};
  ShaderParam<float> base_roughness{0.0f};
  ShaderParam<float> base_metalness{0.0f};
  ShaderParam<float> base_diffuse_roughness{0.0f};

  ShaderParam<float> specular_weight{1.0f};
  ShaderParam<vec3> specular_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> specular_roughness{0.3f};
  ShaderParam<float> specular_ior{1.5f};
  ShaderParam<float> specular_ior_level{0.5f};
  ShaderParam<float> specular_anisotropy{0.0f};
  ShaderParam<float> specular_rotation{0.0f};
  ShaderParam<float> specular_roughness_anisotropy{0.0f};

  ShaderParam<float> transmission_weight{0.0f};
  ShaderParam<vec3> transmission_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> transmission_depth{0.0f};
  ShaderParam<vec3> transmission_scatter{{0.0f, 0.0f, 0.0f}};
  ShaderParam<float> transmission_scatter_anisotropy{0.0f};
  ShaderParam<float> transmission_dispersion{0.0f};
  ShaderParam<float> transmission_dispersion_abbe_number{0.0f};
  ShaderParam<float> transmission_dispersion_scale{0.0f};

  ShaderParam<float> subsurface_weight{0.0f};
  ShaderParam<vec3> subsurface_color{{0.8f, 0.8f, 0.8f}};
  ShaderParam<float> subsurface_radius{1.0f};
  ShaderParam<vec3> subsurface_radius_scale{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> subsurface_scale{1.0f};
  ShaderParam<float> subsurface_anisotropy{0.0f};
  ShaderParam<float> subsurface_scatter_anisotropy{0.0f};

  ShaderParam<float> sheen_weight{0.0f};
  ShaderParam<vec3> sheen_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> sheen_roughness{0.3f};

  ShaderParam<float> fuzz_weight{0.0f};
  ShaderParam<vec3> fuzz_color{{1.0f, 1.0f, 1.0f}};
  ShaderParam<float> fuzz_roughness{0.5f};

  ShaderParam<float> thin_film_weight{0.0f};
  ShaderParam<float> thin_film_thickness{500.0f};
  ShaderParam<float> thin_film_ior{1.5f};

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

  ShaderParam<float> emission_luminance{0.0f};
  ShaderParam<vec3> emission_color{{1.0f, 1.0f, 1.0f}};

  ShaderParam<float> opacity{1.0f};
  ShaderParam<vec3> normal{{0.0f, 0.0f, 1.0f}};
  ShaderParam<vec3> tangent{{1.0f, 0.0f, 0.0f}};
  float tangent_rotation{0.0f};
  float normal_map_scale{1.0f};

  ShaderParam<vec3> coat_normal{{0.0f, 0.0f, 1.0f}};
  ShaderParam<vec3> coat_tangent{{1.0f, 0.0f, 0.0f}};
  ShaderParam<float> displacement{0.0f};
  float coat_tangent_rotation{0.0f};
  float coat_normal_map_scale{1.0f};

  std::string nodeGraphJson;  // MaterialX node graph as JSON; may be empty
};

struct RenderMaterialXConfig {
  bool authored{false};
  std::string version{"1.38"};
  std::string name_space;
  std::string colorspace{"lin_rec709"};
  std::string source_uri;
};

struct RenderMaterial {
  std::string name;
  std::string abs_path;
  std::string display_name;
  nonstd::optional<PreviewSurfaceShader> surfaceShader;
  nonstd::optional<OpenPBRSurfaceShader> openPBRShader;
  RenderMaterialXConfig materialXConfig;

  bool hasUsdPreviewSurface() const { return surfaceShader.has_value(); }
  bool hasOpenPBR() const { return openPBRShader.has_value(); }
};

enum class ColorSpace {
  sRGB,
  Lin_sRGB,
  Rec709,
  Lin_Rec709,
  g22_Rec709,
  g18_Rec709,
  sRGB_Texture,
  Raw,
  Lin_ACEScg,
  ACES2065_1,
  Lin_Rec2020,
  OCIO,
  Lin_DisplayP3,
  sRGB_DisplayP3,
  Custom,
  Unknown,
};

struct TextureImage {
  std::string asset_identifier;
  ColorSpace colorSpace{ColorSpace::sRGB};
  ColorSpace usdColorSpace{ColorSpace::sRGB};
  std::string sourceColorSpaceName{"auto"};
  int32_t width{-1};
  int32_t height{-1};
  int32_t channels{-1};
  bool decoded{false};
};

struct UVTexture {
  int64_t texture_image_id{-1};
};

struct RenderMesh {
  std::string prim_name;
  std::string abs_path;
};

struct RenderLight {
  std::string name;
  std::string abs_path;
  std::string display_name;

  enum class Type { Point, Sphere, Disk, Rect, Cylinder, Distant, Dome, Geometry, Portal };
  enum class DomeTextureFormat { Automatic, Latlong, MirroredBall, Angular };

  Type type{Type::Point};
  vec3 color{1.0f, 1.0f, 1.0f};
  float intensity{1.0f};
  float exposure{0.0f};
  float diffuse{1.0f};
  float specular{1.0f};
  bool normalize{false};
  bool enableColorTemperature{false};
  float colorTemperature{6500.0f};
  vec3 position{0.0f, 0.0f, 0.0f};
  vec3 direction{0.0f, -1.0f, 0.0f};
  float radius{0.5f};
  float width{1.0f};
  float height{1.0f};
  float length{1.0f};
  float angle{0.53f};
  std::string textureFile;
  float shapingConeAngle{90.0f};
  float shapingConeSoftness{0.0f};
  float shapingFocus{0.0f};
  vec3 shapingFocusTint{0.0f, 0.0f, 0.0f};
  std::string shapingIesFile;
  float shapingIesAngleScale{0.0f};
  bool shapingIesNormalize{false};
  bool shadowEnable{true};
  vec3 shadowColor{0.0f, 0.0f, 0.0f};
  float shadowDistance{-1.0f};
  float shadowFalloff{-1.0f};
  float shadowFalloffGamma{1.0f};
  DomeTextureFormat domeTextureFormat{DomeTextureFormat::Automatic};
  float guideRadius{1.0e5f};
  int32_t envmap_texture_id{-1};
  int32_t geometry_mesh_id{-1};
  std::string geometry_target_path;
  std::string material_sync_mode;
};

// Texture/image/mesh tables referenced by texture ids and geometry lights.
struct RenderScene {
  std::vector<UVTexture> textures;
  std::vector<TextureImage> images;
  std::vector<RenderMesh> meshes;
};

enum class SerializationFormat { JSON, XML };

nonstd::expected<std::string, std::string> serializeMaterial(
    const RenderMaterial &material, SerializationFormat format,
    const RenderScene *renderScene = nullptr);

nonstd::expected<std::string, std::string> serializeLight(
    const RenderLight &light, SerializationFormat format,
    const RenderScene *renderScene = nullptr);

}  // namespace matser
}  // namespace web_core
}  // namespace lightusd
