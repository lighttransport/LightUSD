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

#include "render-data.hh"
#include "render-extract.hh"
#include "scene-access.hh"
#include "next/stage/stage.hh"
#include "next/execution.hh"
#if defined(LIGHTUSD_ENABLE_THREAD)
#include "tsa-mutex.hh"
#endif

namespace lightusd {
namespace next {
class AssetResolver;
}  // namespace next
}  // namespace lightusd

namespace lightusd {
namespace tydra {
namespace next {

#if defined(LIGHTUSD_ENABLE_THREAD)
using ConverterStateMutex = ::lightusd::Mutex;
using ConverterStateLock = ::lightusd::MutexLockGuard;
#else
// The converter is fully serial when threading is disabled. Keep its state
// lock ABI-free in that configuration so no mutex/thread implementation is
// pulled into single-threaded or WASM builds.
struct ConverterStateMutex {};
struct ConverterStateLock {
  explicit ConverterStateLock(ConverterStateMutex&) {}
};
#endif

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

  // Memory optimization
  bool use_chunked_arrays = true;

  // Keep the converted vertex/topology payload in RenderScene. Consumers
  // which source geometry elsewhere (for example the web RenderStream, which
  // lazily builds one output mesh from Stage at a time) can disable this to
  // retain only mesh metadata, material bindings, skinning and blend shapes.
  // Bulk arrays are released incrementally during conversion so the peak does
  // not include both the Stage and a second complete copy of every mesh.
  bool retain_geometry = true;

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
  // Enable animation extraction. Disable this for static-scene pipelines to
  // avoid per-prim time-sample scans when animations are not needed.
  bool enabled = true;
  using ClipStageLoader = std::function<bool(
      const std::string& asset_path, ::lightusd::next::Stage* stage,
      std::string* warn, std::string* err)>;
  ClipStageLoader clip_stage_loader;

  // Maximum number of samples generated for a clip set. The authored stage
  // timeCodesPerSecond is used as the sampling rate.
  uint32_t max_value_clip_samples = 10000;
};

struct ConverterConfig {
  MeshConfig mesh;
  MaterialConfig material;
  CurvesConfig curves;
  PointInstancerConfig point_instancer;
  AnimationConfig animation;

  // Defensive namespace limits for composed/programmatically-created stages.
  // Zero disables the corresponding limit; depth defaults to the core's
  // 256-level safety ceiling.
  size_t max_render_depth = 256;
  size_t max_render_records = 0;

  // Worker threads for the per-record conversion batches (mesh/points/
  // curves/cameras/skeletons/lights/point_instancers). 0 = auto
  // (std::thread::hardware_concurrency(), capped at 16). 1 forces fully
  // serial conversion -- useful for deterministic/single-threaded test
  // builds or to cap CPU usage in a server context; WASM/single-thread
  // targets should set this to 1 explicitly rather than relying on
  // hardware_concurrency() (which may over-report on some runtimes).
  size_t max_worker_threads = 0;

  // Unified execution policy. max_threads == -1 preserves
  // max_worker_threads above; otherwise 0=auto, 1=serial, >1=fixed.
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
  bool cancelled = false;
  std::string error;
  std::vector<std::string> warnings;
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
  bool ConvertLight(const UsdPrim& prim, RenderLight* out);
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
  std::string GetLastError() const {
    ConverterStateLock lk(state_mu_);
    return last_error_;
  }

 private:
  // Build scene hierarchy
  void BuildNodeHierarchy(const RenderExtractResult& extracted, RenderScene* scene);
  void ExtractPhysicsAnnotations(const ::lightusd::next::Stage& stage,
                                 RenderScene* scene);
  void AssignMaterialBindings(const ::lightusd::next::Stage& stage,
                              RenderScene* scene);
  void AssignMeshMaterialBinding(const ::lightusd::next::Stage& stage,
                                 const RenderScene& scene,
                                 RenderMesh* mesh);

  /// Lazily create the shared default material (MaterialConfig::
  /// assign_default_material); returns its id.
  int32_t GetOrCreateDefaultMaterial(RenderScene* scene);
  void AssignPointInstanceDrawMaterials(RenderScene* scene);
  void DuplicatePointInstanceMeshes(RenderScene* scene);

  // Extract mesh data directly into chunked arrays
  bool ConvertGeomPrimitive(const UsdPrim& prim, RenderMesh* out);
  bool ExtractMeshGeometry(const UsdPrim& prim, RenderMesh* mesh);
  bool ExtractMeshTopology(const UsdPrim& prim, RenderMesh* mesh);
  /// Drop faces with out-of-range (or negative) indices and truncate counts
  /// that overrun the index buffer; appends a warning when anything changed.
  void SanitizeMeshTopology(RenderMesh* mesh);
  bool ExtractMeshPrimvars(const UsdPrim& prim, RenderMesh* mesh);

  // Triangulation
  bool TriangulateFan(const uint32_t* face_vertex_counts, size_t face_count,
                      const uint32_t* indices, size_t index_count,
                      UInt32Chunked* out_indices);

  // Normal computation
  bool ComputeVertexNormals(RenderMesh* mesh);
  /// Tangent frame (xyzw, w=handedness) from triangulated topology, normals,
  /// and UVs. Emits vertex tangents for vertex-varying inputs and faceVarying
  /// tangents when seams/mirrors require per-corner data.
  bool ComputeVertexTangents(RenderMesh* mesh);

  /// Resolve an authored asset path against the directory of the LAYER THAT
  /// AUTHORED IT (`asset_anchor_id`, carried through composition -- see
  /// next/layer/asset-anchor.hh), falling back to `config_.asset_base_dir` when
  /// the prim has no anchor. Anchoring at the stage root instead would break any
  /// scene whose look layers reach their textures with `../..`.
  std::string ResolveAssetPath(const std::string& file,
                               uint32_t asset_anchor_id = 0) const;
  /// The anchor stamped on `prim`'s spec (0 when it has none).
  static uint32_t AssetAnchorOf(const ::lightusd::next::UsdPrim& prim);
  /// Find-or-create an image record for `file`; returns its id (-1 on empty).
  int32_t ResolveImageId(RenderScene* scene, const std::string& file,
                         ColorSpace color_space, uint32_t asset_anchor_id = 0);
  int32_t FindCachedImageId(RenderScene* scene, const std::string& resolved,
                            ColorSpace color_space);
  void RememberImageId(RenderScene* scene, const std::string& resolved,
                       ColorSpace color_space, int32_t id);
  void ResetImageIdCache();

  /// O(1) image dedup, replacing the linear scan over scene->images that both
  /// dedup sites used (O(n^2) with a long-path string compare per step).
  static std::string ImageKey(const std::string& resolved_path, ColorSpace cs);
  int32_t FindImageId(const RenderScene* scene,
                      const std::string& resolved_path, ColorSpace cs);
  void RememberImageId(const RenderScene* scene,
                       const std::string& resolved_path, ColorSpace cs,
                       int32_t id);
  std::unordered_map<std::string, int32_t> image_id_by_key_;
  /// Scene the rendering color config has already been resolved into; the
  /// result is stage/config-invariant, so it must not be recomputed per
  /// material.
  const RenderScene* color_config_scene_ = nullptr;

  /// Cumulative (RSS-based) memory guard for the expensive phases; the
  /// converter's other limits are all per-prim and cannot see a scene of many
  /// small meshes summing past the cap. Latches once tripped so the rest of
  /// the conversion degrades consistently rather than thrashing.
  bool BudgetWouldExceed(size_t estimate, const char* phase);
  void ResetOperationState();
  size_t budget_accounted_bytes_ = 0;
  size_t budget_pending_bytes_ = 0;
  size_t budget_check_counter_ = 0;
  bool budget_exceeded_ = false;

  // Material extraction
  bool ExtractPreviewSurface(const ::lightusd::next::Stage& stage,
                             const UsdPrim& shader_prim,
                             PreviewSurfaceShader* out,
                             RenderScene* scene);
  bool ExtractStandardSurfaceAsOpenPBR(const ::lightusd::next::Stage& stage,
                                       const ::lightusd::next::UsdPrim& shader_prim,
                                       OpenPBRSurfaceShader* out,
                                       RenderScene* scene);
  bool ExtractOpenPBRSurface(const ::lightusd::next::Stage& stage,
                             const UsdPrim& shader_prim,
                             OpenPBRSurfaceShader* out,
                             RenderScene* scene);
  bool ExtractShaderParam(const ::lightusd::next::Stage& stage,
                          const UsdPrim& shader_prim,
                          const std::string& param_name,
                          ShaderParam* out,
                          RenderScene* scene);

  ConverterConfig config_;
  std::string last_error_;
  std::vector<std::string> warnings_;
  // Guards last_error_, warnings_ and the BudgetWouldExceed bookkeeping
  // (budget_*_ below) for the parallel per-record conversion phases (meshes
  // today; see the mesh-conversion loop in Convert()). No-op cost on the
  // serial phases that still call SetLastError()/AddWarning()/
  // BudgetWouldExceed() from the main thread only.
  mutable ConverterStateMutex state_mu_;
  void AddWarning(std::string msg);
  void SetLastError(std::string msg);
  const RenderScene* image_cache_scene_ = nullptr;
  // Keep only a compact path hash in the transient dedup index.  The
  // resolved path is already retained by RenderScene::images; duplicating it
  // here would turn a CPU optimization into a sizeable memory tax for scenes
  // with many textures.  FindCachedImageId verifies candidates to make hash
  // collisions harmless.
  std::unordered_multimap<uint64_t, int32_t> image_id_cache_;

  // Set (RAII, see MaterialLocalScope in render-converter.cc) around a
  // parallel materials-batch worker's ConvertMaterial() call. That call is
  // given a per-worker LOCAL scratch RenderScene (not the shared result
  // scene), so FindCachedImageId/RememberImageId must not touch the shared
  // image_id_cache_/image_cache_scene_ above -- multiple workers would race
  // on them. While set, those two methods fall back to a plain scan of the
  // (small, freshly-empty-per-material) local scratch's own images list, and
  // the color-config memo block in ConvertMaterial is skipped entirely (the
  // caller pre-seeds the scratch scene's working_color_space from the
  // already-resolved result.scene value instead). A serial merge pass then
  // dedups/appends each worker's local images/textures into the shared scene
  // and remaps every ShaderParam::texture_id, so output is byte-identical to
  // the fully-serial conversion. thread_local, not a member: each worker
  // thread needs its own scope independent of which RenderSceneConverter
  // instance it's calling into.
  static thread_local bool tl_material_local_scope_;

  // RAII scope for tl_material_local_scope_ (nested so its ctor/dtor can
  // touch the private flag above; a class local to a .cc member function
  // body does NOT get implicit access to its enclosing class's private
  // members, so this can't just live next to its one call site).
  struct MaterialLocalScope {
    MaterialLocalScope() { tl_material_local_scope_ = true; }
    ~MaterialLocalScope() { tl_material_local_scope_ = false; }
  };
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
