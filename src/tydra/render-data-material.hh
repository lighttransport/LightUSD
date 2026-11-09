// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data-shaders.hh"
#include <memory>
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {

// Shader Parameter - value or texture reference

struct MaterialDiagnostic {
  MaterialDiagnosticKind kind = MaterialDiagnosticKind::UnsupportedShader;
  std::string material_path;
  std::string node_path;
  std::string shader_id;
  std::string message;
};

// Authored shader inputs retained when the surface terminal cannot be fully
// evaluated. These values are intentionally neutral to current real-time
// shading, but remain available to future evaluators and diagnostics.
struct RetainedMaterialParam {
  std::string shader;
  std::string name;
  ShaderParam value;
};

struct RenderMaterial {
  RenderMaterial();
  ~RenderMaterial();
  RenderMaterial(const RenderMaterial&);
  RenderMaterial& operator=(const RenderMaterial&);
  RenderMaterial(RenderMaterial&&) noexcept;
  RenderMaterial& operator=(RenderMaterial&&) noexcept;

  std::string name;
  std::string prim_path;

  struct MaterialXConfig {
    bool authored = false;
    std::string version;
    std::string name_space;
    std::string colorspace;
    std::string source_uri;
  };

  // Shader type
  enum class ShaderType : uint8_t {
    None = 0,
    PreviewSurface,
    OpenPBR
  };
  ShaderType shader_type = ShaderType::None;

  // The material had no fully convertible surface shader and carries a
  // degraded PreviewSurface instead of being dropped (see ConvertMaterial).
  // Recognizable authored constants and texture inputs are retained when
  // possible. Conversion still SUCCEEDS, so consumers that report load
  // degradation must look here rather than relying on the return value.
  bool default_fallback = false;
  std::vector<MaterialDiagnostic> diagnostics;
  std::vector<RetainedMaterialParam> retained_params;

  // Shader data (one of these based on shader_type)
  std::shared_ptr<PreviewSurfaceShader> preview_surface;
  std::shared_ptr<OpenPBRSurfaceShader> openpbr;
  // Authored utility graph feeding a PreviewSurface terminal. Kept as a
  // separate compatibility payload because the GPU-facing parameters above
  // intentionally contain only the resolved shader values.
  std::string preview_surface_nodegraph_json;

  // Double-sided
  bool double_sided = false;

  // Alpha mode
  enum class AlphaMode : uint8_t {
    Opaque = 0,
    Mask,
    Blend
  };
  AlphaMode alpha_mode = AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;

  // Material displacement/volume terminals (outputs:displacement /
  // outputs:volume connections). Terminal paths remain available for geometry
  // consumers; MaterialX volume topology is retained separately below.
  bool has_displacement = false;
  std::string displacement_shader_path;
  bool has_volume = false;
  std::string volume_shader_path;
  // Retained MaterialX graph for the volume terminal. Kept separate from the
  // surface graph because a material may author both terminals.
  std::string volume_nodegraph_json;

  MaterialXConfig mtlx_config;
};



}}} // namespace lightusd::tydra::next
