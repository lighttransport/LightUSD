// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Render Scene Converter
//
// Converts next::Stage to RenderScene with minimal intermediate copies

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#include "render-data.hh"
#include "next/execution.hh"
#include "next/operation-status.hh"
#include "next/resource-limits.hh"

namespace lightusd {
namespace next {
class AssetResolver;
class Stage;
class UsdPrim;
}  // namespace next
}  // namespace lightusd

namespace lightusd {
namespace tydra {
namespace next {

using ::lightusd::next::Stage;
using ::lightusd::next::UsdPrim;

//
// Conversion configuration
//

struct MeshConfig {
  // Uniform pre-tessellation level for authored subdivision surfaces.
  // Zero disables refinement. Per-prim entries override the scene level.
  int subdivision_level = 0;
  std::unordered_map<std::string, int> subdivision_prim_levels;

  // Subdivision level for generated analytic spheres. Match the legacy
  // converter default so backend switches do not expose faceted silhouettes.
  // Clamped to [0, 6] by the converter.
  int sphere_subdivisions = 4;

  // Triangulation
  bool triangulate = true;
  enum class TriangulationMethod { Earcut, Fan } triangulation_method = TriangulationMethod::Earcut;

  // Normals
  bool compute_normals = true;
  bool compute_tangents = false;
  enum class TangentComputationMethod {
    Lengyel,
    MikkTSpace,
    FastMikkTSpace,
    Hybrid
  };
  TangentComputationMethod tangent_method = TangentComputationMethod::Hybrid;

  // UV handling. The primary UV set is the first of these names the mesh
  // actually authors; the secondary set is that name + "1". USD does not
  // mandate a name and exporters disagree (Maya/USD "st", Blender "UVMap"), so
  // a bare "st" lookup silently loses the UVs of a lot of real content. The
  // name that won is reported back on RenderMesh::texcoords_0_name, so a
  // consumer can match it against a UsdPrimvarReader varname
  // (RenderTexture::uv_primvar).
  // Production assets commonly carry a dense render UV (`perfuv`) alongside
  // simulation/effects sets and a generic `st`. Preview materials explicitly
  // target perfuv; selecting st first can leave perfuv outside the two retained
  // slots and silently sample the wrong atlas after subdivision.
  std::vector<std::string> uv_primvar_names = {"perfuv", "st", "UVMap", "uv",
                                               "st0", "map1", "fxuv"};

  // Index optimization
  bool build_vertex_indices = true;
  float dedup_epsilon = 1e-6f;

  // Optional real-time skinning reduction. Keeps the strongest influences per
  // point and renormalizes them, matching the legacy converter contract.
  bool enable_bone_reduction = false;
  uint32_t target_bone_count = 4;
  bool round_bone_count = false;

  // Memory optimization
  bool use_chunked_arrays = true;

  // Keep the converted vertex/topology payload in RenderScene. Consumers
  // which source geometry elsewhere (for example the web RenderStream, which
  // lazily builds one output mesh from Stage at a time) can disable this to
  // retain only mesh metadata, material bindings, skinning and blend shapes.
  // Bulk arrays are released incrementally during conversion so the peak does
  // not include both the Stage and a second complete copy of every mesh.
  bool retain_geometry = true;

  // Retain only authored custom primvars when geometry is rebuilt lazily by
  // the consumer. Builtin UV/color/normal arrays remain excluded.
  bool retain_custom_primvars = false;

  // Metadata-only consumers may still need the converter's robust polygon
  // triangulation without retaining authored points and vertex attributes.
  bool retain_triangulation = false;

  // Generated geometric primitives have no authored Mesh arrays for a lazy
  // consumer to rebuild from. Keep their generated payload when requested.
  bool retain_analytic_geometry = false;
};

struct MaterialConfig {
  // Preferred UsdShade material binding purpose. Supported real-time values
  // are "preview" and "full"; empty retains the preview fallback order.
  std::string binding_purpose{"preview"};
  // Texture loading
  bool load_textures = true;
  bool allow_missing_textures = true;
  // Pack decoded UDIM tiles into a single atlas instead of retaining sparse
  // per-tile records. Disabled by default for the editor-oriented next path.
  bool combine_udim_tiles = false;

  // Assign a generated default PreviewSurface material to meshes/curves that
  // have no authored material binding (legacy assign_default_material parity).
  // Disabled by default so callers can distinguish unbound geometry via
  // material_id == -1.
  bool assign_default_material = false;
  std::string default_material_name = "defaultMaterial";

  // Color space
  ColorSpace target_color_space = ColorSpace::Linear;

  // Optional RenderSettings prim override. Empty selects the stage-level
  // renderSettingsPrimPath, then the renderer's linear Rec.709 fallback.
  std::string render_settings_path;

  // Texture loader callback (optional custom loader)
  using TextureLoader = std::function<bool(const std::string& path, TextureImage* out)>;
  TextureLoader custom_texture_loader;

};

struct CurvesConfig {
  // Polyline samples per cubic/NURBS span when tessellating BasisCurves /
  // NurbsCurves (linear curves pass through unchanged). Clamped to >= 1.
  uint32_t tessellation_segments = 8;
  // Keep the authored control-point stream in RenderCurves. Consumers that
  // only need render-ready tessellated polylines can disable this to avoid a
  // second full control-point allocation for large curve fields.
  bool retain_control_points = true;
};

struct PointInstancerConfig {
  // Keep the default as lightweight draw references. Enable this only for
  // consumers that require ordinary mesh payloads for each visible instance.
  bool duplicate_meshes = false;

  // Large-scene mode can retain one GPU-oriented 32-byte record per instance
  // and omit the much larger parallel CPU arrays/matrices/draw expansion.
  bool compact_instances = false;
  bool retain_source_arrays = true;
  bool build_instance_transforms = true;
  bool build_instance_draws = true;
};

struct AnimationConfig {
  // Bake value clips into ordinary AnimationClip channels. Clip layers are
  // supplied by the application so the converter remains filesystem- and
  // archive-agnostic.
  bool bake_value_clips = true;
  float value_clip_sample_rate = 0.0f;
  bool value_clip_use_time_range = false;
  double value_clip_start_time = 0.0;
  double value_clip_end_time = 0.0;
  // Enable animation extraction. Disable this for static-scene pipelines to
  // avoid per-prim time-sample scans when animations are not needed.
  bool enabled = true;
  using ClipStageLoader = std::function<bool(
      const std::string& asset_path, ::lightusd::next::Stage* stage,
      std::string* warn, std::string* err)>;
  ClipStageLoader clip_stage_loader;

};

struct ConverterConfig {
  MeshConfig mesh;
  MaterialConfig material;
  CurvesConfig curves;
  PointInstancerConfig point_instancer;
  AnimationConfig animation;

  // Shared finite resource ceilings. Zero-valued fields are invalid; use
  // ResourceLimits::Unlimited() only for an intentional trusted opt-out.
  ::lightusd::next::ResourceLimits limits;

  // Unified execution policy: 0=bounded auto, 1=serial, >1=fixed.
  ::lightusd::next::ExecutionOptions execution;

  // Time code for evaluation
  double time_code = 0.0;

  // Directory of the source USD file: relative texture asset paths resolve
  // against it into TextureImage::resolved_path. Empty = leave paths as
  // authored.
  std::string asset_base_dir;

  // Optional next-core AssetResolver (non-owning). When set it takes over
  // texture asset-path resolution (anchor/working-dir/search paths +
  // suffix fallback) instead of the plain asset_base_dir prefix above.
  const ::lightusd::next::AssetResolver* asset_resolver = nullptr;

  // Progress callback
  using ProgressCallback = std::function<void(float progress, const std::string& message)>;
  ProgressCallback progress_callback;

  // Checked between catalog and geometry conversion steps. Returning true
  // stops conversion before the next large attribute is materialized.
  using CancelCallback = std::function<bool()>;
  CancelCallback cancel_callback;

  // Require warning-free conversion. This rejects unsupported renderables,
  // fallback materials, approximation warnings and failed conversions. It is
  // intentionally conservative and also rejects resolver/color warnings.
  bool strict_conversion = false;
};

/// Fail-closed render-conversion preset. A zero memory limit clamps to one byte
/// instead of selecting the legacy unlimited convention. Texture callbacks
/// remain serialized unless explicitly opted into concurrent invocation.
ConverterConfig MakeHardenedConverterConfig(size_t max_memory);

//
// Conversion result
//

struct ConvertResult {
  bool success = false;
  ::lightusd::next::OperationStatus status =
      ::lightusd::next::OperationStatus::InvalidData;
  std::string error;
  std::vector<std::string> warnings;

  RenderScene scene;
};

enum class ConversionProfile : uint8_t { Streaming, Retained };

enum class GeometryKind : uint8_t { Mesh, Points, Curves };
enum class GeometryDisposition : uint8_t { Full, Proxy, Skip, Cancel };

struct GeometryInfo {
  GeometryKind kind = GeometryKind::Mesh;
  int32_t id = -1;
  std::string prim_path;
  std::string type_name;
  size_t point_count = 0;
  size_t index_count = 0;
  size_t estimated_resident_bytes = 0;
  bool has_authored_extent = false;
};

struct StreamConvertResult {
  bool success = false;
  ::lightusd::next::OperationStatus status =
      ::lightusd::next::OperationStatus::InvalidData;
  bool cancelled = false;
  std::string error;
  std::vector<std::string> warnings;
  std::vector<ConversionDiagnostic> conversion_diagnostics;
  size_t mesh_count = 0;
  size_t point_count = 0;
  size_t curve_count = 0;
};

/// Receives a lightweight scene catalog followed by one converted geometry
/// prim at a time. The catalog contains stable node/material/prototype IDs and
/// placeholder geometry entries, but no large geometry arrays.
class SceneSink {
 public:
  virtual ~SceneSink() = default;
  virtual bool BeginScene(RenderScene&& catalog) = 0;
  virtual GeometryDisposition SelectGeometry(const GeometryInfo&) {
    return GeometryDisposition::Full;
  }
  virtual bool AddMesh(int32_t id, RenderMesh&& mesh) = 0;
  virtual bool AddPoints(int32_t id, RenderPoints&& points) = 0;
  virtual bool AddCurves(int32_t id, RenderCurves&& curves) = 0;
  virtual bool EndScene() = 0;
  virtual void AbortScene() {}
};

//
// Main converter class
//

class RenderSceneConverter {
 public:
  explicit RenderSceneConverter(const ConverterConfig& config = {});
  ~RenderSceneConverter();

  // Non-copyable
  RenderSceneConverter(const RenderSceneConverter&) = delete;
  RenderSceneConverter& operator=(const RenderSceneConverter&) = delete;

  // Main conversion entry point
  /// Read-only retained conversion. This never evicts values from `stage`, so
  /// it is safe to use with an immutable StageSnapshot.
  ConvertResult Convert(const ::lightusd::next::Stage& stage);

  // Low-memory, destructive conversion entry point. Only one converted
  // geometry prim is live outside the destination sink at a time. Static
  // source arrays may be evicted as their last consumer completes, hence the
  // deliberately mutable Stage reference.
  StreamConvertResult ConvertToSink(::lightusd::next::Stage& stage,
                                    SceneSink* sink);

  // Cheap preflight and extent-only conversion for application-managed
  // streaming pipelines. Neither method decodes the authored mesh payload.
  GeometryInfo GetGeometryInfo(const UsdPrim& prim, GeometryKind kind,
                               int32_t id = -1) const;
  bool ConvertExtentProxy(const UsdPrim& prim, RenderMesh* out);
  bool ConvertBoundsProxy(const UsdPrim& prim, const Float3& minimum,
                          const Float3& maximum, RenderMesh* out);

  // Individual conversion methods (for custom pipelines)
  bool ConvertRenderableMesh(const Stage& stage, const UsdPrim& prim,
                             RenderMesh* out);
  bool ConvertMesh(const Stage& stage, const UsdPrim& prim, RenderMesh* out);
  bool ConvertPoints(const Stage& stage, const UsdPrim& prim,
                     RenderPoints* out);
  bool ConvertCurves(const UsdPrim& prim, RenderCurves* out);
  bool ConvertPointInstancer(const UsdPrim& prim, RenderPointInstancer* out);
  bool ConvertMaterial(const ::lightusd::next::Stage& stage, const UsdPrim& prim, RenderMaterial* out);
  bool ConvertMaterial(const ::lightusd::next::Stage& stage,
                       const UsdPrim& prim, RenderMaterial* out,
                       RenderScene* scene);
  bool ConvertLight(const ::lightusd::next::Stage& stage,
                    const UsdPrim& prim, RenderLight* out);
  bool ConvertCamera(const ::lightusd::next::Stage& stage,
                     const UsdPrim& prim, RenderCamera* out);
  bool ConvertSkeleton(const UsdPrim& prim, Skeleton* out);
  bool ConvertAnimation(const ::lightusd::next::Stage& stage,
                        const UsdPrim& prim, AnimationClip* out);

  // Robustly triangulate an already-populated RenderMesh. Custom/lazy
  // pipelines can share the converter's earcut, quad-diagonal, winding and
  // hole handling without constructing a complete RenderScene.
  bool TriangulateMesh(RenderMesh* mesh);

  // Texture loading
  bool LoadTexture(const std::string& asset_path, TextureImage* out);

  // Get last error
  std::string GetLastError() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

//
// Utility functions
//

// (Utility triangulation/normal/tangent free functions were declared here but
// never defined; the declarations were removed — a caller would only get a
// link error. Fan triangulation and vertex-normal generation live on
// RenderSceneConverter.)

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
