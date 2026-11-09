// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file threejs-exporter.hh
/// @brief Three.js material and scene exporter for LightUSD/Tydra
///
/// Converts Tydra RenderScene and materials to Three.js-compatible formats,
/// supporting both WebGPU (MaterialX nodes) and WebGL (standard materials).
///

#pragma once

#include <string>
#include <vector>
#include <map>
#include "render-data.hh"

// Suppress warnings from nlohmann/json.hpp
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace tydra {

using json = nlohmann::json;

///
/// Three.js Material Exporter
///
/// Exports Tydra materials to Three.js-compatible JSON format
/// Supports dual output: WebGPU (node-based) and WebGL (standard materials)
///
class ThreeJSMaterialExporter {
public:
  struct ExportOptions {
    bool use_webgpu = true;        ///< Target WebGPU renderer with MaterialX nodes
    bool generate_fallback = true;  ///< Generate WebGL fallback materials
    bool embed_textures = false;    ///< Embed texture data as base64 in JSON
    std::string texture_path = "";  ///< External texture directory path
    bool export_mtlx = false;       ///< Export as MaterialX document
    std::string color_space = "sRGB"; ///< Target color space for textures
    bool use_grouped_parameters = false; ///< Use grouped parameters (e.g., base.color) instead of flattened (e.g., base_color)
  };

  /// Export entire RenderScene to Three.js format
  bool ExportScene(const RenderScene& scene,
                   const ExportOptions& options,
                   json& output);

  /// Export single material to Three.js format
  bool ExportMaterial(const RenderMaterial& material,
                     const ExportOptions& options,
                     json& output);

  /// Export material as MaterialX document
  bool ExportMaterialX(const RenderMaterial& material,
                      std::string& mtlx_output);

  /// Get last error message
  const std::string& GetError() const { return _err; }

  /// Get warning messages
  const std::string& GetWarning() const { return _warn; }

private:
  /// Convert OpenPBR shader to Three.js node material
  json ConvertOpenPBRToNodeMaterial(const OpenPBRSurfaceShader& shader, const ExportOptions& options);

  /// Convert OpenPBR shader to Three.js MeshPhysicalMaterial (WebGL fallback)
  json ConvertOpenPBRToPhysicalMaterial(const OpenPBRSurfaceShader& shader, const ExportOptions& options);

  /// Convert UsdPreviewSurface to Three.js materials
  json ConvertPreviewSurfaceToNodeMaterial(const PreviewSurfaceShader& shader);
  json ConvertPreviewSurfaceToStandardMaterial(const PreviewSurfaceShader& shader);

  /// Convert shader parameter to Three.js format
  template<typename T>
  json ConvertShaderParam(const ShaderParam<T>& param, const std::string& name);

  /// Handle texture references and UV transformations
  json ConvertTextureReference(uint64_t texture_id, const ExportOptions& options);

  /// Map Tydra parameter names to Three.js equivalents
  std::string MapParameterName(const std::string& tydra_name);

  /// Generate MaterialX node graph for OpenPBR
  json GenerateMaterialXNodes(const OpenPBRSurfaceShader& shader);

  /// Utility to convert color values with color space management
  json ConvertColor(const vec3& color, const std::string& space = "linear");

  /// Error and warning strings
  std::string _err;
  std::string _warn;

  /// Cache for texture conversions
  std::map<uint64_t, json> _texture_cache;
};

///
/// Three.js Scene Exporter
///
/// Exports complete Tydra RenderScene to Three.js Object3D hierarchy
///
class ThreeJSSceneExporter {
public:
  struct SceneExportOptions {
    ThreeJSMaterialExporter::ExportOptions material_options;
    bool export_animations = true;
    bool export_cameras = true;
    bool export_lights = true;
    bool optimize_geometry = false;  ///< Merge duplicate vertices
    bool generate_lods = false;      ///< Generate LOD levels
    std::string output_format = "gltf"; ///< "gltf", "json", or "draco"
  };

  /// Export complete scene
  bool ExportScene(const RenderScene& scene,
                  const SceneExportOptions& options,
                  json& output);

  /// Export as glTF 2.0 with MaterialX extensions
  bool ExportGLTF(const RenderScene& scene,
                 const SceneExportOptions& options,
                 json& gltf_output);

  const std::string& GetError() const { return _err; }
  const std::string& GetWarning() const { return _warn; }

private:
  /// Convert node hierarchy. `depth` is the internal recursion depth (callers
  /// pass 0); it guards against stack overflow on deeply nested node trees.
  json ConvertNode(const Node& node, const RenderScene& scene, uint32_t depth = 0);

  /// Convert mesh with optimizations
  json ConvertMesh(const RenderMesh& mesh, bool optimize);

  /// Convert animation data
  json ConvertAnimation(const AnimationClip& anim);

  /// Convert camera
  json ConvertCamera(const RenderCamera& camera);

  /// Convert lights
  json ConvertLight(const RenderLight& light);

  std::string _err;
  std::string _warn;

  ThreeJSMaterialExporter _material_exporter;
};

///
/// Material parameter mapping tables
///
struct MaterialParameterMapping {
  // OpenPBR to Three.js MeshPhysicalMaterial
  static const std::map<std::string, std::string>& openpbr_to_physical();

  // OpenPBR to Three.js node names
  static const std::map<std::string, std::string>& openpbr_to_nodes();

  // UsdPreviewSurface to Three.js
  static const std::map<std::string, std::string>& preview_to_physical();

  // Color space conversions
  static const std::map<std::string, std::string>& colorspace_map();
};

} // namespace tydra
} // namespace lightusd
