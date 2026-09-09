// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache
//
// Lazy, per-prim composition cache for the `next` module, mirroring the design
// of lightusd::pcp::Cache but built entirely on next types (standalone, C++14).
//
// Phase 1 scope: sublayers (L) + references (R), lazy ComputePrimIndex,
// parse-once LayerRegistry, dependency-aware Invalidate, and BuildStage
// materialization. Inherits/specializes/variants/payloads/instancing are later
// phases (see the plan).

#pragma once

#include "prim-index.hh"
#include "layer-registry.hh"
#include "load-rules.hh"
#include "../resolver/asset-resolver.hh"
#include "../stage/stage.hh"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nonstd/expected.hpp"

namespace lightusd {
namespace next {
namespace pcp {

class Cache {
 public:
  // Called once after namespace discovery, but before the (potentially
  // expensive) full opinion-fill pass. The compact Stage contains spatial
  // endpoints (bounds/cameras) and their transform ancestors. It is separate
  // and move-only, so retaining it cannot race authoritative BuildStage work.
  using PreviewCallback = std::function<bool(Stage&&)>;

  struct MemoryStats {
    size_t source_layer_bytes = 0;
    size_t transient_cache_bytes = 0;
    size_t layer_count = 0;
    size_t prim_index_count = 0;
    size_t composed_prim_count = 0;

    size_t total_bytes() const {
      return source_layer_bytes + transient_cache_bytes;
    }
  };

  Cache();
  ~Cache();
  Cache(Cache &&) noexcept;
  Cache &operator=(Cache &&) noexcept;
  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;

  /// Open a cache over a root layer. Runs the sublayer (L) phase once: builds
  /// the root layer stack [root, sublayers...]. `resolver` is borrowed and must
  /// outlive the cache. `root_identifier` is the root layer's resolved file path
  /// (used as the anchor for resolving its references/sublayers); may be empty.
  static nonstd::expected<Cache, std::string> Open(
      AssetResolver &resolver, std::shared_ptr<Layer> root_layer,
      const std::string &root_identifier = "",
      const CompositionOptions &options = {});

  // --- typed composition diagnostics (Phase 7 E4) -------------------------

  /// Typed composition diagnostic codes. Stable across message wording so
  /// tests and consumers can branch on the code rather than matching the
  /// free-form `err` text. New codes are appended (never renumbered).
  enum class ErrorCode : uint8_t {
    ArcCycle,                 ///< reference/payload/inherit/specialize cycle
    SublayerCycle,            ///< subLayers form a cycle
    MaxDepthExceeded,         ///< sublayer / arc / namespace recursion limit hit
    InvalidAssetPath,         ///< an arc or sublayer asset could not be resolved
    UnresolvedPrimPath,       ///< an arc targets a prim path that does not exist
    IndexCapacityExceeded,    ///< PrimIndex node count exceeds uint16 capacity
    InvalidVariantSelection,  ///< a variant selection names no known variant
    InvalidReferenceOffset,   ///< offset non-finite or scale not positive-finite
    ExpressionVariableError,  ///< asset-path expression failed evaluation
  };

  /// One typed composition diagnostic. `site` is the layer:prim (or prim path)
  /// where the problem was detected; `message` is the human-readable rendering
  /// that is also concatenated into the `err` out-param (so no API break).
  struct CompositionIssue {
    ErrorCode code;
    std::string site;
    std::string message;
  };

  /// All composition issues accumulated since Open (or the last
  /// ClearCompositionIssues / Invalidate / InvalidateLayer / SetLoadRules).
  /// Populated regardless of whether an `err` pointer was passed, so callers
  /// can inspect typed codes even with `err == nullptr`. Returned by value: a
  /// borrowed reference could dangle if a concurrent compose appended to the
  /// log under LIGHTUSD_NEXT_FINE_LOCKS.
  std::vector<CompositionIssue> GetCompositionIssues() const;

  /// Drop all accumulated composition issues.
  void ClearCompositionIssues();

  /// Lazily build (if needed) and cache the PrimIndex for `prim_path`. Returns a
  /// borrowed pointer owned by the cache (stable until Invalidate/destruction),
  /// or nullptr if the prim has no local spec in the root layer stack.
  const PrimIndex *ComputePrimIndex(const Path &prim_path, std::string *warn,
                                    std::string *err);

  /// Build (and cache) the PrimIndices for a batch of paths. With thread
  /// support enabled and `CompositionOptions::num_threads != 1`, independent
  /// prim indices are built in parallel using per-worker contexts and a
  /// deterministic merge. Payload-policy and custom resolver callbacks must be
  /// thread-safe in that mode. Best-effort: a path that fails to build is
  /// skipped.
  bool PrewarmPrimIndices(const std::vector<Path> &paths, std::string *warn,
                          std::string *err);

  /// Materialize the fully-composed scene into `stage` (a fresh root Layer).
  bool BuildStage(Stage *stage, std::string *warn, std::string *err,
                  const PreviewCallback& preview_callback = {});

  /// Resolved identifiers for every layer participating in the current
  /// composition, including the root. Sorted and deduplicated so callers can
  /// construct deterministic cache manifests without borrowing Layer objects.
  std::vector<std::string> GetLayerDependencies() const;

  /// Approximate logical residency owned by the composition cache. Source
  /// layers are counted once even when several layer stacks reference them.
  MemoryStats GetMemoryStats() const;

  /// Drop recomputable PrimIndex/source/opinion caches while retaining parsed
  /// layers, load rules, deferred-payload state and variant selections.
  void TrimTransientCaches();

  // --- lazy per-prim composition (Phase 10) -------------------------------

  /// Compose (and cache) just the prim at `prim_path` on first access, reusing
  /// the same source resolution + opinion merge as BuildStage but WITHOUT
  /// materializing the whole stage -- for read-only consumers that touch a few
  /// prims. Returns a borrowed pointer to the composed PrimSpec (type,
  /// specifier, properties, relationships, connections, declared type names,
  /// and active/hidden/doc/... metadata), stable until Invalidate/destruction,
  /// or nullptr if the prim authors no opinions. Children are not materialized
  /// here: query their names via ComposedChildNames() and compose each lazily.
  /// Instancing/relocates are stage-level concerns and are NOT applied to this
  /// per-prim view (use BuildStage / the instance query API for those).
  const PrimSpec *ComposePrim(const Path &prim_path, std::string *warn,
                              std::string *err);

  /// Composed child prim names of `prim_path` in BuildStage emit order
  /// (composes the prim first if needed). Empty if it has no children.
  std::vector<std::string> ComposedChildNames(const Path &prim_path,
                                              std::string *warn,
                                              std::string *err);

  // --- instancing ---------------------------------------------------------

  /// Whether `prim_path` is an instance (instanceable, and not the prototype of
  /// its instance group).
  bool IsInstance(const Path &prim_path) const;

  /// The prototype prim path for `prim_path` (itself if it is the prototype, or
  /// empty if not instanceable / not computed).
  Path GetPrototype(const Path &prim_path) const;

  /// All prototype prim paths discovered so far.
  std::vector<Path> GetPrototypePaths() const;

  /// All instances (including the prototype) sharing a prototype.
  std::vector<Path> GetInstancesForPrototype(const Path &prototype) const;

  /// Translate an instance-space path into prototype space: given a path that is
  /// an instance root or any descendant of one, rewrite the nearest enclosing
  /// instance prefix to its prototype root, recursing for nested instances
  /// (a prim inside a prototype that is itself an instance). Returns an empty
  /// Path when `path` lies under no instance (mirrors OpenUSD's
  /// `UsdPrim::GetPathInPrototypeForPath` returning an empty path).
  Path TranslatePathToPrototype(const Path &path) const;

  /// Inverse of the above for a single, caller-named instance: rewrite a
  /// prototype-space path (which must lie at/under `instance_root`'s prototype)
  /// onto `instance_root`. Returns an empty Path when `instance_root` is not an
  /// instance or `proto_path` is not within its prototype.
  Path TranslatePathFromPrototype(const Path &proto_path,
                                  const Path &instance_root) const;

  /// Number of distinct prototypes (instance groups).
  size_t PrototypeCount() const;

  /// Compute the structural instance key for `prim_path` (for diagnostics/tests).
  std::string ComputeInstanceKey(const Path &prim_path, std::string *warn,
                                 std::string *err);

  /// Payload load granularity (mirrors UsdLoadPolicy).
  enum class LoadPolicy {
    WithDescendants,     ///< load this prim's payload and all descendants'
    WithoutDescendants,  ///< load only this prim's payload
  };

  /// Load a deferred payload on `prim_path` and recompose the affected subtree.
  /// Defaults to loading descendant payloads too (LoadWithDescendants).
  bool LoadPayload(const Path &prim_path, std::string *warn, std::string *err);
  bool LoadPayload(const Path &prim_path, LoadPolicy policy, std::string *warn,
                   std::string *err);

  /// Apply several payload load rules at once and invalidate their common
  /// affected subtree once. Unlike SetLoadRules, parsed layers and unrelated
  /// composition caches are retained.
  bool LoadPayloads(const std::vector<Path> &prim_paths, LoadPolicy policy);

  /// Unload (defer) the payload on `prim_path` and its descendants, then
  /// recompose so HasDeferredPayload is immediately accurate.
  bool UnloadPayload(const Path &prim_path);

  /// Replace the full set of payload load rules and drop all cached indices.
  /// `rules` is a list of (prim path, rule) where rule is 0=All, 1=Only, 2=None.
  void SetLoadRules(const LoadRules &rules);

  /// A snapshot of the current payload load rules.
  LoadRules GetLoadRules() const;

  /// Replace path-scoped variant selections and invalidate only paths whose
  /// selections changed (plus dependent indices). Parsed layers stay cached.
  void SetVariantSelections(
      const CompositionOptions::VariantSelectionMap &selections);
  CompositionOptions::VariantSelectionMap GetVariantSelections() const;

  /// Whether `prim_path` has an unloaded (deferred) payload.
  bool HasDeferredPayload(const Path &prim_path) const;

  /// All prims that currently have a deferred payload.
  std::vector<Path> GetDeferredPayloadPaths() const;

  /// Drop the cached index for `prim_path` and every index that depended on a
  /// site at/under it.
  void Invalidate(const Path &prim_path);

  /// Drop every cached index that read from `resolved_layer_id`, then drop the
  /// layer from the registry.
  void InvalidateLayer(const std::string &resolved_layer_id);

  /// Re-read a resolved layer and invalidate all composition state that may
  /// retain opinions or pointers from the previous layer object.
  bool ReloadLayer(const std::string &resolved_layer_id, std::string *warn,
                   std::string *err);

  bool HasComputedPrimIndex(const Path &prim_path) const;
  size_t ComputedPrimIndexCount() const;
  const LayerRegistry &layer_registry() const;

  /// Pre-register an in-memory layer under `identifier`, so a reference/payload
  /// resolving to that id composes it without disk I/O (embedding helper).
  void PreloadLayer(const std::string &identifier, std::shared_ptr<Layer> layer);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// --- one-call composition helpers ------------------------------------------

/// Load a root layer from `filename`, compose it (sublayers + references +
/// inherits + variants + payloads + specializes + relocates + instancing), and
/// materialize the result into `out_stage`. References resolve relative to the
/// root file's directory.
bool ComposeStageFromFile(const std::string &filename, AssetResolver &resolver,
                          Stage *out_stage,
                          const CompositionOptions &options = {},
                          std::string *warn = nullptr, std::string *err = nullptr);

/// Compose an already-loaded in-memory root layer into `out_stage`.
bool ComposeStageFromLayer(std::shared_ptr<Layer> root_layer,
                           AssetResolver &resolver, Stage *out_stage,
                           const std::string &root_identifier = "",
                           const CompositionOptions &options = {},
                           std::string *warn = nullptr,
                           std::string *err = nullptr);

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
