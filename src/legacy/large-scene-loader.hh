// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// large-scene-loader.hh - Memory-bounded loading of very large USD scenes.
//
// Two backends, selected at compile time by LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE:
//
//   Default (no define):      CompositionGraph (old, stable).
//   LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE:
//                             next::pcp::Cache (new, incremental payload
//                             load/unload without full stage rebuild).
//
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "stage.hh"

#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#else
#include "composition-graph.hh"
#include "pcp/layer-registry.hh"
#endif

namespace lightusd {

struct LargeSceneLoadOptions {
  // How payload arcs are handled during the initial composition.
  enum class PayloadMode {
    LoadAll,   // Eagerly load every payload (may exceed RAM on huge scenes).
    LoadNone,  // Defer every payload (lightest; geometry streamed later).
    Budget,    // Load payloads until `payload_budget_mb` of payload asset bytes
               // have been loaded, then defer the rest.
  };
  PayloadMode payload_mode{PayloadMode::LoadNone};

  // Byte budget (MB) of payload asset file sizes for PayloadMode::Budget.
  size_t payload_budget_mb{12288};

  // Optional authored-extent coverage cap for PayloadMode::Budget. Zero keeps
  // the legacy byte-only policy. A positive value caps the cumulative
  // view-independent projected AABB area derived from each owner's
  // `extentsHint` (max of XY/XZ/YZ, in squared stage units). Payloads without a
  // valid hint retain byte-only behavior. Both limits must admit a payload.
  double payload_extent_budget{0.0};

  // Keep uncompressed USDC arrays mmap-backed through composition. Fully
  // supported by the next::pcp backend, whose LazyArrayRef owns a shared
  // per-layer CrateDataSource. The legacy backend uses copy mode because its
  // Stage mmap table represents only one source file.
  bool mmap_zero_copy{true};

  // Allow '..'-relative reference/payload asset paths (required for scenes such
  // as Caldera). Resolution of the surviving '..' is delegated to the resolver.
  bool allow_parent_relative_paths{true};

  // Parse each referenced/payload file exactly once and share it (the single
  // biggest win for scenes with tens of thousands of files). Backed by
  // pcp::LayerRegistry in the old path; the new next::pcp::Cache always dedups.
  bool dedup_layers{true};

  // Detect scenegraph instances during composition.
  bool detect_instances{true};

  // Extra resolver search roots (the scene's base directory is always added).
  std::vector<std::string> search_paths;

  // Upper bound on a single asset read (MB). Large crate payloads need this
  // raised above the default.
  size_t max_asset_bytes_mb{8192};

  // Maximum number of concurrently open file descriptors/asset handles used by
  // the resolver during sublayer/reference/payload loading.
  uint32_t max_file_descriptors{1024};

  // Maximum composition recursion depth.
  uint32_t max_composition_depth{1024};
};

// Owns the Stage plus the composition backend, resolver and ancillary state
// so deferred payloads can be loaded/unloaded after Load() returns.
class LargeSceneLoader {
 public:
  LargeSceneLoader();
  ~LargeSceneLoader();
  LargeSceneLoader(const LargeSceneLoader &) = delete;
  LargeSceneLoader &operator=(const LargeSceneLoader &) = delete;
  LargeSceneLoader(LargeSceneLoader &&) = delete;
  LargeSceneLoader &operator=(LargeSceneLoader &&) = delete;

  // Load + compose `filename` (USDA/USDC) under `options`. On success the Stage
  // is populated and (for LoadNone/Budget) some payloads are recorded as
  // deferred. Returns false with *err on failure.
  bool Load(const std::string &filename, const LargeSceneLoadOptions &options,
            std::string *warn, std::string *err);

#if !defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  Stage &stage() { return *_stage; }
  const Stage &stage() const { return *_stage; }

  // Thread-safe immutable snapshot. A background load/unload builds a new
  // Stage and atomically publishes it under the loader mutex; existing
  // snapshots remain valid until their last reader releases them. Prefer this
  // over stage() when payload streaming can run concurrently.
  std::shared_ptr<const Stage> stage_snapshot() const;
#else
  next::Stage &stage() { return *_stage; }
  const next::Stage &stage() const { return *_stage; }
  std::shared_ptr<const next::Stage> stage_snapshot() const;
#endif

  // -- Deferred (unloaded) payload streaming --

  // Prim paths whose payloads were deferred during the initial composition.
  std::vector<Path> deferred_payload_paths() const;
  size_t deferred_count() const;

  // Load / unload a single deferred payload and publish a rebuilt Stage. These
  // operations are serialized and may safely be called from a worker thread.
  // Concurrent readers should hold stage_snapshot(), not a bare stage()
  // reference. Explicit rebuild_stage() remains available but is not required
  // after these calls.
  bool load_payload(const Path &prim_path, std::string *warn, std::string *err);
  bool unload_payload(const Path &prim_path, std::string *warn,
                      std::string *err);

  // Rebuild stage() from the (possibly mutated) composition graph.
  bool rebuild_stage(std::string *warn, std::string *err);

  // -- Diagnostics --

  // Estimated Stage memory in bytes (composition structure; deferred geometry
  // is NOT counted because it is not in the Stage).
  size_t estimate_stage_memory_bytes() const;

  // Number of unique files actually parsed (cache misses) by the layer
  // registry.
  size_t layer_parse_count() const;

 private:
#if !defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  std::shared_ptr<Stage> _stage{std::make_shared<Stage>()};
#else
  std::shared_ptr<next::Stage> _stage{std::make_shared<next::Stage>()};
#endif
  bool _loaded{false};
  mutable std::mutex _mutex;

  // Caller must hold _mutex.
  bool RebuildStageLocked(std::string *warn, std::string *err);

#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  std::unique_ptr<next::AssetResolver> _resolver;
  std::unique_ptr<next::pcp::Cache> _cache;
#else
  static const Layer *LoadLayerThunk(void *userdata,
                                     const std::string &asset_path,
                                     const std::string &cwp, std::string *warn,
                                     std::string *err);

  std::unique_ptr<AssetResolutionResolver> _resolver;
  std::unique_ptr<pcp::LayerRegistry> _registry;
  std::unique_ptr<Layer> _flattened;
  std::unique_ptr<CompositionGraph> _graph;
#endif
};

// One-shot convenience: load + compose into `stage`. Does NOT keep a handle for
// on-demand payload streaming (use LargeSceneLoader for that).
// Available only in the old backend path (CompositionGraph).
// In the new backend path use LargeSceneLoader or next::pcp::ComposeStageFromFile.
#if !defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
bool LoadStageFromFile(const std::string &filename, Stage *stage,
                       std::string *warn, std::string *err,
                       const LargeSceneLoadOptions &options = {});
#endif

}  // namespace lightusd
