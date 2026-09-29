// SPDX-License-Identifier: Apache-2.0
// lusdview - load a USD file into a Stage and convert it to a Tydra RenderScene.
#pragma once

#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu_scene.hh"
#include "lightusd-render-c.h"
#include "preview_cache.hh"
#include "io-util.hh"  // lightusd::io::MMapFileHandle
#include "layer.hh"
#include "load_control.hh"
#include "stage.hh"
#include "tydra/render-data.hh"

namespace lightusd { struct USDZAsset; }

namespace lusdview {

// How `payload` composition arcs are handled at load time.
enum class PayloadPolicy {
  LoadAll,    // compose all payloads eagerly
  DeferAll,   // skip all payloads; record them for on-demand loading
  Whitelist,  // load only payloads whose prim path is in `payloadWhitelist`
};

struct LoadOptions {
  // UsdShade material binding purpose used by real-time conversion. Empty or
  // "preview" preserves the historical preview -> all-purpose -> full order;
  // "full" selects full -> all-purpose -> preview for production assets.
  std::string materialPurpose{"preview"};
  // Aggregate host/GPU limits for the next large-scene path. Zero preserves
  // legacy behavior. The loader applies these before geometry materialization.
  size_t maxMemoryBytes{0};
  size_t gpuGeometryBudgetBytes{0};
  // Total CPU/GPU density budget for UsdVol grids. Zero derives a conservative
  // share from gpuGeometryBudgetBytes (or uses the built-in preview default).
  size_t volumeMemoryBudgetBytes{0};
  size_t uploadStagingBytes{0};
  // Maximum CPU geometry held between the next-loader producer and the GPU
  // context thread. Zero selects 64 MiB for interactive streaming.
  size_t streamBufferBytes{0};
  // Explicit performance controls. Zero selects the hardware-derived default.
  unsigned compositionThreads{0};
  unsigned conversionThreads{0};
  // Runtime scheduling controls. Zero selects generic hardware/memory-derived
  // defaults; presets and users may override every value.
  size_t compositionOpinionBatch{0};
  size_t instanceChunkSamples{0};
  size_t meshConversionChunkPrims{0};
  size_t meshConversionChunkBytes{0};
  size_t curveParallelMinPrims{0};
  // Number of Ptex faces to materialize in the startup fallback atlas. Zero
  // preserves eager construction; remaining faces are populated on demand.
  uint32_t ptexInitialFaces{0};
  // Fixed physical page-cache bytes reserved per Ptex texture. Zero selects
  // the ordinary 32 MiB default; large-scene profiles can choose a smaller
  // first-preview residency without changing generic fidelity.
  size_t ptexPhysicalCacheBytes{0};
  // Samples per cubic/NURBS curve span. Linear curves are unchanged. Large
  // scene preview profiles may lower this while ordinary loads keep 8.
  uint32_t curveTessellationSegments{8};
  // Interactive procedural preview limits. Zero keeps every curves prim/strand.
  // Sampling retains complete polylines so hair topology remains valid.
  size_t maxCurvePrims{0};
  size_t maxCurveStrands{0};
  // Gaussian/Points carriers are emitted in bounded records so a large point
  // field never requires one giant DrawPointsCPU allocation. Zero selects 64K.
  size_t pointChunkSamples{0};
  // Register ordinary texture slots during material conversion, then decode,
  // resize and compress them after geometry publication on bounded workers.
  bool asyncTextureDecode{false};
  // Force ordinary textures through the synchronous decode/compression path.
  // This is useful for bounded conversion benchmarks and validation runs where
  // the result must include texture processing rather than merely registering
  // deferred placeholders for the interactive residency manager.
  bool forceTextureDecode{false};
  // Interactive fast path: when decoded textures fit comfortably in the
  // device texture budget, avoid CPU block compression/mip generation.
  bool optimizeTextureUpload{false};
  size_t textureGpuBudgetBytes{0};
  bool textureCompressionExplicit{false};
  bool textureMipsExplicit{false};
  bool textureBudgetExplicit{false};
  // --texture-fit: resident-byte threshold (geometry + decoded textures) under
  // which textures are left unshrunk and uncompressed. 0 == "always process"
  // (the pre-policy behaviour); UINT64 max == "never process".
  lightusd_texture_fit textureFit{0, LIGHTUSD_TEXTURE_FIT_DEFAULT, 0};
  size_t textureFitThresholdBytes{0};
  // 25% of resident VRAM, INDEPENDENT of --texture-fit. Gates mip generation
  // only; see the comment where it is consumed in next_scene_loader.cc.
  size_t textureComfortBytes{0};
  // Optional source-mesh conversion cap for large-scene preview profiles.
  // Remaining prims stay unmaterialized instead of paying CPU conversion cost.
  size_t maxMeshConversions{0};
  // Publish a bounds/camera proxy scene from next-core's namespace checkpoint
  // while authoritative opinion composition continues.
  bool progressivePreview{false};
  // Maximum marker boxes emitted for the asynchronous composition preview.
  // Zero selects the generic interactive default.
  size_t previewMaxBoxes{0};
  // Optional authored camera used to prioritize mesh conversion/admission.
  // Geometry with large projected coverage in front of this camera streams
  // before off-camera detail; empty preserves deterministic stage order.
  std::string viewCamera;
  PreviewCacheOptions previewCache;
  bool timing{false};

  // Compose USD composition arcs (subLayers/references/payload/inherits/
  // variants) on load. When false, only the root layer is loaded (legacy
  // behavior, keeps the mmap zero-copy fast path).
  bool composition{true};
  PayloadPolicy payloadPolicy{PayloadPolicy::DeferAll};
  // Also defer `references` arcs (--defer-references). Off by default: unlike
  // payload, USD semantics assume references always resolve, so most content
  // arrives through them and a deferred scene looks much emptier. Deferred
  // references share the payload whitelist/on-demand machinery (a prim path in
  // the whitelist loads both its arcs).
  bool deferReferences{false};
  // Prim paths (composed-layer full paths) whose deferred arcs to load when
  // payloadPolicy == Whitelist.
  std::set<std::string> payloadWhitelist;
  // USD time code to evaluate the scene at (animated transforms / points /
  // skinning / value clips are sampled here). NaN (TimeCode::Default) = static
  // default values. Used for the initial load (e.g. --time for a headless
  // screenshot at a specific frame); interactive playback re-evaluates via
  // RenderSceneAtTime().
  double timecode{std::numeric_limits<double>::quiet_NaN()};
  // Conversion-time subdivision surface refinement. subdivisionLevel is the
  // scene-wide fallback; subdivisionPrimLevels overrides individual mesh prims.
  // subdivisionAuto is handled by lusdview before conversion by filling
  // subdivisionPrimLevels from projected mesh screen coverage.
  int subdivisionLevel{0};
  bool subdivisionAuto{false};
  int subdivisionAutoMaxLevel{3};
  std::map<std::string, int> subdivisionPrimLevels;
  // Variant selection overrides: key = prim full path, value = map of
  // variantSet name -> variant name. Applied before composition so variant
  // arcs resolve with the user's choices instead of the layer defaults.
  std::map<std::string, std::map<std::string, std::string>> variantOverrides;
  // Allow parent-directory ('..') segments in composition asset paths
  // (--allow-parent-paths). Off by default (lightusd rejects '..' traversal as
  // unsafe). Some production scenes have lighting overrides
  // referenced through parent-directory paths and need it; resolution of the
  // surviving '..' is delegated to the asset resolver, anchored at searchPaths.
  bool allowParentRelativePaths{false};
  // Emit per-vertex GPU skinning attributes (joint indices/weights + a bone
  // matrix layout) instead of baking a static skinned pose into the geometry at
  // load. Only the `next` loader reads this: the Tydra path always emits the
  // attributes and decides afterwards. Off = load-time CPU bake (the pose at
  // `timecode`), which is what the CPU-skinning and CPU-tracer paths need.
  bool gpuSkinning{false};
  TextureRuntimeOptions textureOptions;
};

// Move-only messages produced by the next loader and consumed by App on the
// render/context thread. A complete event contains scene-wide metadata and
// resources; streamed meshes have already been removed from its DrawScene.
struct ProgressiveSceneEvent {
  enum class Type {
    PreviewScene, Reset, Resources, Mesh, Points, Curves, Volume, Texture,
    Complete, Failed
  };
  Type type{Type::Failed};
  std::vector<DrawMaterialCPU> materials;
  int textureCount{0};
  std::string upAxis{"Y"};
  DrawMeshCPU mesh;
  DrawPointsCPU points;
  DrawCurvesCPU curves;
  DrawVolumeCPU volume;
  DrawTextureCPU texture;
  int textureSlot{-1};
  DrawScene scene;
  std::string error;
};

// Bounded producer/consumer handoff. Byte accounting applies to mesh payloads;
// resource and terminal messages are small/one-shot and never block completion.
class ProgressiveSceneStream {
 public:
  explicit ProgressiveSceneStream(size_t maxBytes);
  ~ProgressiveSceneStream();

  ProgressiveSceneStream(const ProgressiveSceneStream&) = delete;
  ProgressiveSceneStream& operator=(const ProgressiveSceneStream&) = delete;

  bool pushPreview(DrawScene&& scene);
  bool pushReset();
  bool pushResources(const std::vector<DrawMaterialCPU>& materials,
                     int textureCount, const std::string& upAxis);
  bool pushMesh(DrawMeshCPU&& mesh, const std::atomic<bool>* cancelled = nullptr);
  bool pushPoints(DrawPointsCPU&& points,
                  const std::atomic<bool>* cancelled = nullptr);
  bool pushCurves(DrawCurvesCPU&& curves,
                  const std::atomic<bool>* cancelled = nullptr);
  bool pushVolume(DrawVolumeCPU&& volume,
                  const std::atomic<bool>* cancelled = nullptr);
  bool pushTexture(int slot, DrawTextureCPU&& texture,
                   const std::atomic<bool>* cancelled = nullptr);
  void pushComplete(DrawScene&& scene);
  void pushFailed(std::string error);
  bool tryPop(ProgressiveSceneEvent* event);
  void cancel();
  bool cancelled() const;
  size_t queuedBytes() const;

 private:
  struct QueuedEvent {
    ProgressiveSceneEvent event;
    size_t bytes{0};
  };
  size_t maxBytes_{0};
  size_t queuedBytes_{0};
  bool cancelled_{false};
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable space_;
  std::deque<QueuedEvent> queue_;
};

// A payload/reference arc that was skipped during composition.
struct DeferredArc {
  std::string primPath;   // prim carrying the arc (composed-layer path)
  std::string assetPath;  // target asset (may be empty for internal arcs)
  const char* arc;        // "payload" | "reference"
};

// Composition state retained for on-demand payload loading. `rootLayer` is the
// post-sublayer snapshot of the opened file; recomposition restarts from it
// (CompositePayload strips payload metadata even for deferred arcs, so the
// composed Stage alone is not enough to load payloads later).
struct CompositionInfo {
  bool composed{false};
  std::shared_ptr<const lightusd::Layer> rootLayer;
  std::vector<std::string> searchPaths;
  // Retained package backing for composition and later payload recomposition.
  std::shared_ptr<lightusd::USDZAsset> usdzAsset;
  // Mirror of SceneLoaderOptions::allowParentRelativePaths, retained so the
  // RenderScene conversion applies the same '..' policy to texture/light asset
  // resolution that composition used (the tydra asset resolver defaults to
  // rejecting parent-relative paths).
  bool allowParentRelativePaths{false};
  std::vector<DeferredArc> deferred;         // still-unloaded payload/reference arcs
  std::set<std::string> loadedPayloads;      // whitelist accumulated so far
};

// Holds both the parsed Stage (for the hierarchy browser / property inspector)
// and the converted RenderScene (for rendering). Both must stay alive together.
struct LoadedScene {
  lightusd::Stage stage;
  lightusd::tydra::RenderScene render;
  std::string filepath;
  std::string warn;
  std::string err;
  bool ok{false};
  int subdivisionLevel{0};
  std::map<std::string, int> subdivisionPrimLevels;
  // Memory-mapped file handle kept alive for the Stage's lifetime (zero-copy
  // USDC arrays reference this mapping). Unmapped when this LoadedScene dies.
  // Null on the composition path (composition copies specs, so zero-copy
  // offsets would dangle).
  std::shared_ptr<lightusd::io::MMapFileHandle> mmap;
  CompositionInfo comp;
};

// Load `path` (usd/usda/usdc/usdz), convert to a RenderScene configured for
// single-index OpenGL/Vulkan rendering, and build the backend-neutral
// `DrawScene` (`draw`) in the same streaming pass (Tydra
// ConvertToRenderSceneStreaming): mesh geometry is interleaved as each mesh
// converts, world placement applied when the node hierarchy is built, and
// textures/materials decoded on completion. Returns false with `out->err`
// filled on failure (out->stage may still be partially populated for inspection).
//
// `opts` controls composition: by default the file's composition arcs are
// resolved (LIVRPS) with payloads deferred; deferred payloads are listed in
// `out->comp.deferred` and can be loaded later via RecomposeWithPayloads().
// USDZ composition arcs resolve against the retained archive backing, including
// deferred package-internal payloads loaded by RecomposeWithPayloads().
//
// `ctrl` (optional) enables cancellation, progress reporting, a conversion time
// budget and a draw-side triangle/vertex budget. Safe to call on a worker
// thread (no GPU access).
//
// `rtPath` selects ray-tracer-friendly conversion: it disables the
// rasterization-only single-index dedup (build_vertex_indices=false), yielding a
// triangle-soup vertex/index layout that a BLAS consumes directly (still
// renderable by the raster path too).
bool LoadUSD(const std::string& path, const LoadOptions& opts, LoadedScene* out,
             DrawScene* draw, bool rtPath = false, LoadControl* ctrl = nullptr);

// Recompose `prev`'s retained root layer with `opts` (typically
// PayloadPolicy::Whitelist with an enlarged whitelist) and rebuild the
// Stage/RenderScene/DrawScene. `path` is the original file path (used for
// asset resolution and labeling). Worker-thread safe: `prev.rootLayer` is
// only read.
bool RecomposeWithPayloads(const std::string& path, const CompositionInfo& prev,
                           const LoadOptions& opts, LoadedScene* out,
                           DrawScene* draw, bool rtPath = false,
                           LoadControl* ctrl = nullptr);

// Re-evaluate an already-loaded scene at `timecode` and build geometry into
// `draw` (animation playback / scrubbing). Reads `src.stage` only (const) — safe
// to call on a worker thread while the main thread keeps `src` alive and does
// not reload it. Texture image loading is skipped (load_texture_assets=false):
// materials/textures don't animate, so only mesh geometry/transforms/skinning
// are rebuilt; callers keep the textures from the initial load. `draw->materials`
// and `draw->textures` are therefore left empty — only `draw->meshes` (+ bounds)
// are produced. Returns false with `*err` set on failure.
// `blendOverride` (optional, by BlendShape name) applies manual blendshape
// weights (the blend editor) when deforming -- the ray-traced / CPU-skinned
// equivalent of the GPU path's override; honors in-between shapes.
//
// Optional `restCache`: the un-deformed Tydra scene from the last conversion,
// keyed by timecode. When the requested `timecode` matches the cache, the stage
// re-conversion (the heavy `ConvertStageToSceneImpl`) is skipped and the cached
// rest scene is reused -- only the CPU deform + pack re-run. This lets interactive
// blendshape edits (same timecode, only weights changing) on the RT/CPU path avoid
// a full re-conversion. The caller owns the cache and must clear it on reload (the
// geometry belongs to the old scene). NOT thread-safe: only one RenderSceneAtTime
// may touch a given cache at a time (the app gates reconverts to one in flight).
struct RestSceneCache {
  lightusd::tydra::RenderScene scene;
  double timecode = 0.0;
  bool valid = false;
};

bool RenderSceneAtTime(const LoadedScene& src, double timecode, bool rtPath,
                       DrawScene* draw, std::string* warn, std::string* err,
                       LoadControl* ctrl = nullptr,
                       const std::unordered_map<std::string, float>* blendOverride =
                           nullptr,
                       RestSceneCache* restCache = nullptr);

}  // namespace lusdview
