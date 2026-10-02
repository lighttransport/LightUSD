// SPDX-License-Identifier: Apache-2.0
// Private conversion algorithms and operation state. Never include from clients.
#pragma once
#include "render-converter.hh"
#include "render-extract.hh"
#include "scene-access.hh"
#if defined(LIGHTUSD_ENABLE_THREAD)
#include "tsa-mutex.hh"
#endif

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

class RenderSceneConverter::Impl {
 public:
  explicit Impl(const ConverterConfig& config = {});
  ~Impl();

  // Non-copyable
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

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
  bool ExtractMeshPrimvars(const UsdPrim& prim, RenderMesh* mesh,
                           bool custom_only = false);

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

  /// Cumulative per-operation memory guard for expensive phases. Chunked
  /// geometry has an additional exact per-conversion allocation budget.
  bool BudgetWouldExceed(size_t estimate, const char* phase);
  void ResetOperationState();
  size_t budget_accounted_bytes_ = 0;
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
  std::vector<ConversionDiagnostic> conversion_diagnostics_;
  // Guards last_error_, warnings_ and the BudgetWouldExceed bookkeeping
  // (budget_*_ below) for the parallel per-record conversion phases (meshes
  // today; see the mesh-conversion loop in Convert()). No-op cost on the
  // serial phases that still call SetLastError()/AddWarning()/
  // BudgetWouldExceed() from the main thread only.
  mutable ConverterStateMutex state_mu_;
  void AddWarning(std::string msg);
  void AddConversionWarning(const std::string& path, std::string msg,
                            ConversionDisposition disposition =
                                ConversionDisposition::Approximated);
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

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
