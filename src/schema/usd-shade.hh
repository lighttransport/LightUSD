// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - UsdShade Schema APIs
// Convenience accessors for USD materials and shaders

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"
#include <map>

namespace lightusd {

/// Shader input/output info
struct ShaderPort {
  std::string name;
  TypeId type_id = TypeId::Invalid;
  Value value;
  std::string connection_path;  // If connected, path to source
  bool is_connected = false;
};

/// Material surface output binding
struct MaterialBinding {
  std::string surface_shader_path;
  std::string displacement_shader_path;
  std::string volume_shader_path;
};

/// Check if prim is a Material
bool IsMaterial(const UsdPrim& prim);

/// Check if prim is a Shader
bool IsShader(const UsdPrim& prim);

/// Check if prim is a NodeGraph
bool IsNodeGraph(const UsdPrim& prim);

/// Resolve an authored shader port through Material/NodeGraph connections.
/// Besides ordinary connected attributes, recognizes constant shader nodes
/// whose output forwards `inputs:value`. Cycles/deep graphs fail safely.
bool ResolveShaderPortValue(const Stage& stage, const UsdPrim& prim,
                            const std::string& port_name, Value* out,
                            double time = 0.0, int max_depth = 16);

// ============================================================
// Material API
// ============================================================

/// Get material surface shader path
std::string GetSurfaceShader(const Stage& stage, const UsdPrim& material);

/// Renderer-only variant: like GetSurfaceShader, but falls back to the
/// RenderMan `outputs:ri:surface` terminal when the universal one is absent.
/// Public/export paths must keep using GetSurfaceShader.
std::string GetSurfaceShaderWithRenderContextFallback(const Stage& stage,
                                                      const UsdPrim& material);

/// Get material displacement shader path
std::string GetDisplacementShader(const Stage& stage, const UsdPrim& material);

/// Get material volume shader path
std::string GetVolumeShader(const Stage& stage, const UsdPrim& material);

/// Get all material bindings
bool GetMaterialBinding(const Stage& stage, const UsdPrim& material,
                        MaterialBinding* out);

/// Find the first purpose-preferred binding on a prim whose target resolves to
/// a Material. Dangling purpose-specific targets fall through to weaker-purpose
/// bindings on the same prim.
UsdPrim GetBoundMaterial(const Stage& stage, const UsdPrim& prim);

/// Get bound material path
std::string GetBoundMaterialPath(const UsdPrim& prim);

/// Resolve the material bound to `prim_path`, honoring UsdShade binding
/// INHERITANCE (a binding on an ancestor applies to its descendants) on top of
/// the purpose fallback chain in `GetBoundMaterialPath`. Dangling/non-Material
/// targets do not shadow valid weaker-purpose or ancestor bindings. Returns ""
/// if nothing in the ancestor chain binds a material.
std::string GetInheritedBoundMaterialPath(const Stage& stage,
                                          const std::string& prim_path);

/// Resolve an exact purpose-specific material binding (for example `back`) on
/// `prim_path` or its ancestors. Unlike the preview/all/full fallback above,
/// this only considers `material:binding:<purpose>`; callers can fall back to
/// the ordinary front-face binding when it returns empty. Invalid targets are
/// skipped and bindMaterialAs strength is honored across the ancestor chain.
std::string GetInheritedBoundMaterialPathForPurpose(
    const Stage& stage, const std::string& prim_path,
    const std::string& purpose);

/// Does this prim's winning binding declare `bindMaterialAs =
/// "strongerThanDescendants"`? (The default is "weakerThanDescendants", i.e. a
/// descendant binding overrides an ancestor's.) Callers doing their own
/// inheritance walk -- e.g. one that also skips dangling targets -- need this.
bool BindingIsStrongerThanDescendants(const UsdPrim& prim);

// ============================================================
// Shader API
// ============================================================

/// Get shader ID (info:id attribute)
std::string GetShaderId(const UsdPrim& shader);

/// Get shader implementation source (info:implementationSource)
std::string GetShaderImplementationSource(const UsdPrim& shader);

/// Get all shader inputs
std::vector<ShaderPort> GetShaderInputs(const Stage& stage, const UsdPrim& shader,
                                         double time = 0.0);

/// Get all shader outputs
std::vector<ShaderPort> GetShaderOutputs(const UsdPrim& shader);

/// Get a specific shader input by name
bool GetShaderInput(const Stage& stage, const UsdPrim& shader,
                    const std::string& input_name, ShaderPort* out,
                    double time = 0.0);

/// Get shader input value with connection following
EvalResult EvalShaderInput(const Stage& stage, const UsdPrim& shader,
                           const std::string& input_name, double time = 0.0);

// ============================================================
// UsdPreviewSurface helpers
// ============================================================

/// UsdPreviewSurface material data
struct PreviewSurfaceData {
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

  // Texture connections (empty if not connected)
  std::string diffuse_texture;
  std::string normal_texture;
  std::string metallic_texture;
  std::string roughness_texture;
  std::string emissive_texture;
  std::string occlusion_texture;
  std::string opacity_texture;
};

/// Check if shader is UsdPreviewSurface
bool IsPreviewSurface(const UsdPrim& shader);

/// Get UsdPreviewSurface data
bool GetPreviewSurfaceData(const Stage& stage, const UsdPrim& shader,
                           PreviewSurfaceData* out, double time = 0.0);

// ============================================================
// UsdUVTexture helpers
// ============================================================

/// UsdUVTexture data
struct UVTextureData {
  std::string file;
  float st[2] = {0.0f, 0.0f};
  std::string wrap_s = "useMetadata";
  std::string wrap_t = "useMetadata";
  float fallback[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  std::string source_color_space = "auto";
};

/// Check if shader is UsdUVTexture
bool IsUVTexture(const UsdPrim& shader);

/// Get UsdUVTexture data
bool GetUVTextureData(const Stage& stage, const UsdPrim& shader,
                      UVTextureData* out, double time = 0.0);

// ============================================================
// UsdPrimvarReader helpers
// ============================================================

/// Check if shader is a UsdPrimvarReader
bool IsPrimvarReader(const UsdPrim& shader);

/// Get primvar name being read
std::string GetPrimvarReaderVarname(const UsdPrim& shader);

/// Stage-aware variant: additionally follows inputs:varname connections
/// (e.g. to a Material's inputs:frame:stPrimvarName interface attribute).
std::string GetPrimvarReaderVarname(const Stage& stage, const UsdPrim& shader);

}  // namespace lightusd
