// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Tydra - MaterialX Support
//
// Converts MaterialX materials to render-ready format

#pragma once

#include "render-data.hh"
#include "stage/stage.hh"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace lightusd {
namespace tydra {

// ============================================================
// MaterialX Node Types (common shader patterns)
// ============================================================

/// Standard Surface node inputs (for standard_surface category)
struct StandardSurfaceData {
  // Base
  float base = 1.0f;
  float base_color[3] = {0.8f, 0.8f, 0.8f};
  std::string base_color_texture;

  // Specular
  float specular = 1.0f;
  float specular_color[3] = {1.0f, 1.0f, 1.0f};
  float specular_roughness = 0.2f;
  float specular_IOR = 1.5f;
  float specular_anisotropy = 0.0f;
  float specular_rotation = 0.0f;
  std::string specular_roughness_texture;

  // Metallic
  float metalness = 0.0f;
  std::string metalness_texture;

  // Transmission
  float transmission = 0.0f;
  float transmission_color[3] = {1.0f, 1.0f, 1.0f};
  float transmission_depth = 0.0f;
  float transmission_scatter[3] = {0.0f, 0.0f, 0.0f};
  float transmission_scatter_anisotropy = 0.0f;
  float transmission_dispersion = 0.0f;
  float transmission_extra_roughness = 0.0f;

  // Subsurface
  float subsurface = 0.0f;
  float subsurface_color[3] = {1.0f, 1.0f, 1.0f};
  float subsurface_radius[3] = {1.0f, 1.0f, 1.0f};
  float subsurface_scale = 1.0f;
  float subsurface_anisotropy = 0.0f;

  // Sheen
  float sheen = 0.0f;
  float sheen_color[3] = {1.0f, 1.0f, 1.0f};
  float sheen_roughness = 0.3f;

  // Coat
  float coat = 0.0f;
  float coat_color[3] = {1.0f, 1.0f, 1.0f};
  float coat_roughness = 0.1f;
  float coat_anisotropy = 0.0f;
  float coat_rotation = 0.0f;
  float coat_IOR = 1.5f;
  float coat_affect_color = 0.0f;
  float coat_affect_roughness = 0.0f;
  std::string coat_normal_texture;

  // Thin Film
  float thin_film_thickness = 0.0f;
  float thin_film_IOR = 1.5f;

  // Emission
  float emission = 0.0f;
  float emission_color[3] = {1.0f, 1.0f, 1.0f};
  std::string emission_texture;

  // Geometry
  float opacity[3] = {1.0f, 1.0f, 1.0f};
  std::string opacity_texture;
  std::string normal_texture;
  std::string tangent_texture;
};

/// UsdPreviewSurface compatible data
struct MtlxPreviewSurfaceData {
  float diffuse_color[3] = {0.18f, 0.18f, 0.18f};
  float emissive_color[3] = {0.0f, 0.0f, 0.0f};
  float specular_color[3] = {1.0f, 1.0f, 1.0f};

  float metallic = 0.0f;
  float roughness = 0.5f;
  float clearcoat = 0.0f;
  float clearcoat_roughness = 0.01f;
  float opacity = 1.0f;
  float opacity_threshold = 0.0f;
  float ior = 1.5f;

  float normal[3] = {0.0f, 0.0f, 1.0f};
  float displacement = 0.0f;
  float occlusion = 1.0f;

  bool use_specular_workflow = false;

  // Texture connections
  std::string diffuse_texture;
  std::string normal_texture;
  std::string metallic_texture;
  std::string roughness_texture;
  std::string emissive_texture;
  std::string occlusion_texture;
};

/// MaterialX texture node data
struct MtlxTextureData {
  std::string file;                         // File path/URI
  std::string uaddressmode = "periodic";    // periodic, clamp, mirror
  std::string vaddressmode = "periodic";
  std::string filtertype = "linear";        // closest, linear, cubic
  float default_value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  std::string colorspace = "srgb_texture";
};

/// MaterialX node graph representation
struct MtlxNodeInfo {
  std::string name;
  std::string category;  // e.g., "standard_surface", "image", "multiply"
  std::string type;      // e.g., "surfaceshader", "color3", "float"
  std::string colorspace;

  // Input connections/values
  std::map<std::string, std::string> input_connections;  // input_name -> node.output
  std::map<std::string, float> input_floats;
  std::map<std::string, std::vector<float>> input_vectors;
  std::map<std::string, std::string> input_strings;
};

/// Parsed MaterialX material
struct MtlxMaterialInfo {
  std::string name;
  std::string surface_shader;     // Node name
  std::string displacement_shader;
  std::string volume_shader;

  // The node list is a property of the DOCUMENT, not of any one material, so
  // every material in a .mtlx shares it. Held by shared_ptr: rebuilding it per
  // material made ParseMtlx O(materials * nodes) in both time and memory (each
  // MtlxNodeInfo carries four std::maps, so 50 materials x 2000 nodes meant
  // 100k node objects and ~400k red-black-tree allocations where 2000 would
  // do). Never null after a successful ParseMtlx; use nodes() to read it.
  std::shared_ptr<const std::vector<MtlxNodeInfo>> shared_nodes;

  static const std::vector<MtlxNodeInfo>& empty_nodes() {
    static const std::vector<MtlxNodeInfo> kEmpty;
    return kEmpty;
  }
  const std::vector<MtlxNodeInfo>& nodes() const {
    return shared_nodes ? *shared_nodes : empty_nodes();
  }

  std::map<std::string, MtlxTextureData> textures;
};

// ============================================================
// MaterialX Converter
// ============================================================

/// Options for MaterialX conversion
struct MtlxConvertOptions {
  bool convert_to_preview_surface = true;  // Convert standard_surface to UsdPreviewSurface
  bool resolve_nodegraphs = true;          // Flatten nodegraph references
  std::string default_colorspace = "srgb_texture";
};

/// MaterialX to render data converter
class MtlxConverter {
public:
  MtlxConverter();
  ~MtlxConverter();

  /// Set conversion options
  void SetOptions(const MtlxConvertOptions& options) { options_ = options; }

  /// Convert MaterialX material to RenderMaterial
  /// @param mtlx_content MaterialX XML content (string)
  /// @param material_name Name of material to convert (empty = first material)
  /// @param out Output render material
  /// @return true on success
  bool ConvertToRenderMaterial(const std::string& mtlx_content,
                                const std::string& material_name,
                                RenderMaterial* out,
                                const std::string& anchor_path = "");

  /// Convert MaterialX file to RenderMaterial
  bool ConvertFileToRenderMaterial(const std::string& filename,
                                    const std::string& material_name,
                                    RenderMaterial* out);

  /// Convert UsdShade material with MaterialX binding
  /// @param stage USD stage
  /// @param material_prim Material prim with MaterialX shader binding
  /// @param out Output render material
  /// @param allow_converter_delegation When true (direct callers), a
  ///        material-local shader may be routed through the shared
  ///        RenderSceneConverter. MUST be false when called FROM
  ///        RenderSceneConverter::ConvertMaterial — the two functions
  ///        otherwise recurse into each other until stack overflow on
  ///        materials whose shader neither side converts directly (e.g.
  ///        ND_standard_surface_surfaceshader).
  /// @return true on success
  bool ConvertUsdMtlxMaterial(const lightusd::Stage& stage,
                               const lightusd::UsdPrim& material_prim,
                               RenderMaterial* out,
                               bool allow_converter_delegation = true);

  /// Parse MaterialX content to intermediate representation
  bool ParseMtlx(const std::string& mtlx_content,
                 std::vector<MtlxMaterialInfo>* materials);

  /// Convert standard_surface to UsdPreviewSurface compatible data
  bool ConvertStandardSurface(const StandardSurfaceData& ss,
                               MtlxPreviewSurfaceData* out);

  /// Get error message
  const std::string& GetError() const { return error_; }

  /// Get warning message
  const std::string& GetWarning() const { return warning_; }

private:
  MtlxConvertOptions options_;
  std::string error_;
  std::string warning_;

  // Internal conversion helpers
  bool ParseStandardSurface(const MtlxNodeInfo& node, StandardSurfaceData* out);
  bool ParseTextureNode(const MtlxNodeInfo& node, MtlxTextureData* out);
  void PopulateRenderMaterial(const MtlxPreviewSurfaceData& data,
                               const std::map<std::string, MtlxTextureData>& textures,
                               RenderMaterial* out);
};

// ============================================================
// Utility functions
// ============================================================

/// Check if prim has MaterialX binding
bool HasMtlxBinding(const lightusd::UsdPrim& prim);

/// Get MaterialX file path from material binding
std::string GetMtlxFilePath(const lightusd::UsdPrim& material_prim);

/// Parse MaterialX colorspace string to ColorSpace enum
ColorSpace ParseMtlxColorSpace(const std::string& cs);

}  // namespace tydra
}  // namespace lightusd
