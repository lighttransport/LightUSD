// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Unified Header
//
// This header provides the main entry point for the "next" architecture.
// Use -DLIGHTUSD_USE_NEXT=ON in CMake to enable this architecture.
//
// Key differences from the original architecture:
// - Minimal template usage for faster compilation
// - Runtime type dispatch via TypeId enum
// - Small Buffer Optimization (SBO) for Value class
// - Unified PrimSpec (no separate Prim/PrimSpec trees)
// - Time sample value deduplication
// - O(1) property lookup via interned name IDs

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "../security-policy.hh"
#include "execution.hh"
#include "operation-status.hh"
#include "resource-limits.hh"

// Core types
#include "types/type-id.hh"
#include "types/type-info.hh"
#include "types/value.hh"
#include "types/interpolation.hh"

// Persistent PCP composition/cache APIs.
#include "pcp/cache.hh"

// Prim types
#include "prim/path.hh"
#include "prim/attribute.hh"
#include "prim/prim.hh"

// Layer and Stage
#include "layer/property-index.hh"
#include "layer/prim-spec.hh"
#include "layer/layer.hh"
#include "stage/stage.hh"
#include "stage/change-set.hh"

// Parsers
#include "parser/lexer.hh"
#include "parser/value-parser.hh"
#include "parser/ascii-parser.hh"

// Readers
#include "reader/usda-reader.hh"
#include "reader/usdc-reader.hh"
#include "reader/usdz-reader.hh"

// Writers
#include "writer/usda-writer.hh"
#include "writer/usdc-writer.hh"

// Evaluation
#include "eval/attribute-eval.hh"
#include "eval/value-clip.hh"

// Asset resolution
#include "resolver/asset-resolver.hh"

// Composition
#include "composition/composition.hh"

// Schema APIs
#include "schema/geom-mesh.hh"
#include "schema/schema-registry.hh"
#include "schema/geom-point-instancer.hh"
#include "schema/geom-xform.hh"
#include "schema/usd-lux.hh"
#include "schema/usd-geom-camera.hh"
#include "schema/usd-shade.hh"
#include "schema/usd-skel.hh"
#include "schema/usdPhysics.hh"
#include "schema/usd-ar.hh"
#include "schema/usd-media.hh"
#include "schema/usd-mtlx.hh"
#include "schema/usd-render.hh"
#include "schema/usd-semantics.hh"
#include "schema/usd-vol.hh"

namespace lightusd {
namespace next {

// Version info — keep in sync with src/lightusd.hh and web/{npm,js}/package.json
constexpr int version_major = 1;
constexpr int version_minor = 0;
constexpr int version_micro = 0;
constexpr const char* version_string = "1.0.0-rc4";

// ============================================================
// Convenience loading functions (wrap reader APIs)
// ============================================================

/// Options for high-level USD loading.
struct LoadUSDOptions {
  /// Untrusted is the fail-closed default. Trusted restores compatibility
  /// parsing and permits mmap/broader filesystem resolution when requested by
  /// the containing StageSession.
  InputPolicy input_policy = InputPolicy::Untrusted;

  /// Common finite limits. Zero-valued members are invalid; use
  /// ResourceLimits::Unlimited() for an intentional trusted opt-out.
  ResourceLimits limits;

  /// Format-specific USDA options.
  LoadOptions usda_options;

  /// Format-specific USDC options.
  USDCLoadOptions usdc_options;

  /// Format-specific USDZ options.
  USDZReadOptions usdz_options;
};

enum class DiagnosticSeverity : uint8_t { Info, Warning, Error };
enum class DiagnosticDomain : uint8_t { Load, Resolve, Compose, Convert };

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Info;
  DiagnosticDomain domain = DiagnosticDomain::Load;
  std::string code;
  std::string message;
  std::string path;
  std::string asset_path;
};

enum class ProgressPhase : uint8_t {
  RootLoad,
  Compose,
  Recompose,
  PreviewCompose,
};

struct ProgressEvent {
  ProgressPhase phase = ProgressPhase::RootLoad;
  float progress = 0.0f;
  std::string message;
  size_t estimated_resident_bytes = 0;
};

enum class CacheRetention : uint8_t { Full, LayersOnly };

struct StageSessionMemoryStats {
  size_t source_layer_bytes = 0;
  size_t transient_cache_bytes = 0;
  size_t composed_stage_bytes = 0;
  size_t estimated_total_bytes = 0;
  size_t peak_estimated_total_bytes = 0;
  size_t layer_count = 0;
  size_t prim_index_count = 0;
  size_t composed_prim_count = 0;
};

struct StagePreview {
  StageSnapshot snapshot;
  // The snapshot is a compact spatial subset: bound/camera prims and their
  // transform ancestors. Full namespace, geometry and shading are absent.
  bool namespace_complete = false;
  bool spatial_subset = true;
  bool authoritative = false;
};

struct StageSessionOptions {
  LoadUSDOptions load;
  pcp::CompositionOptions composition;
  ResolverConfig resolver;
  bool compose = true;
  CacheRetention cache_retention = CacheRetention::Full;
  // Unified execution policy. 0=bounded auto, 1=serial, >1=fixed.
  ExecutionOptions execution;
  using ProgressCallback = std::function<bool(const ProgressEvent&)>;
  ProgressCallback progress_callback;
  using PreviewCallback = std::function<bool(const StagePreview&)>;
  // Invoked after the root layer is parsed but before PCP composition. The
  // stage is the authored root layer only, so consumers must treat it as a
  // latency-only preview and wait for the authoritative callback below.
  PreviewCallback early_preview_callback;
  // Invoked synchronously on the loading thread during initial composition.
  // The snapshot owns a separate Stage and may safely be retained.
  PreviewCallback preview_callback;
};

/// Convenience for a caller-selected untrusted cap. New code can configure
/// `load.limits` directly; ordinary defaults are already fail closed.
StageSessionOptions MakeHardenedStageSessionOptions(size_t max_memory);

struct StageOperationResult {
  bool success = false;
  OperationStatus status = OperationStatus::InvalidData;
  StageSnapshot snapshot;
  StageChangeSet changes;
  std::vector<Diagnostic> diagnostics;
  std::string warning;
  std::string error;

  // Implicit for source compatibility with the former bool edit API.
  operator bool() const { return success; }
};

using StageEditResult = StageOperationResult;

/// Persistent next-core document. It keeps the resolver and PCP cache alive so
/// payload and variant edits reuse parsed dependency layers.
class StageSession {
 public:
  StageSession();
  ~StageSession();
  StageSession(StageSession&&) noexcept;
  StageSession& operator=(StageSession&&) noexcept;
  StageSession(const StageSession&) = delete;
  StageSession& operator=(const StageSession&) = delete;

  StageOperationResult OpenFile(const std::string& filename,
                                const StageSessionOptions& options = {});

  StageSnapshot GetSnapshot() const;
  // Transfer the composed Stage out of a one-shot session. This fails with
  // Busy when an external snapshot still owns the Stage instead of silently
  // cloning a potentially huge scene.
  nonstd::expected<Stage, OperationStatus> CloseAndTakeStage();
  StageSessionOptions GetOptions() const;
  std::string GetRootIdentifier() const;
  bool IsOpen() const;
  bool IsComposed() const;

  StageEditResult Rebuild();
  StageEditResult LoadPayload(const Path& prim_path,
                              pcp::Cache::LoadPolicy policy =
                                  pcp::Cache::LoadPolicy::WithDescendants);
  StageEditResult UnloadPayload(const Path& prim_path);
  StageEditResult LoadPayloads(
      const std::vector<Path>& prim_paths,
      pcp::Cache::LoadPolicy policy =
          pcp::Cache::LoadPolicy::WithDescendants);
  StageEditResult SetVariantSelection(const Path& prim_path,
                                      const std::string& variant_set,
                                      const std::string& selection);
  StageEditResult ClearVariantSelection(const Path& prim_path,
                                        const std::string& variant_set);
  StageEditResult SetVariantSelections(
      const pcp::CompositionOptions::VariantSelectionMap& selections);
  /// Re-read a dependency layer and transactionally publish the recomposed
  /// stage. Passing the root identifier performs a full reopen.
  StageEditResult ReloadLayer(const std::string& resolved_layer_id);

  pcp::CompositionOptions::VariantSelectionMap GetVariantSelections() const;
  std::vector<Path> GetDeferredPayloadPaths() const;
  std::vector<pcp::Cache::CompositionIssue> GetCompositionIssues() const;
  std::vector<std::string> GetLayerDependencies() const;
  std::vector<Diagnostic> GetDiagnostics() const;
  StageSessionMemoryStats GetMemoryStats() const;
  void TrimCaches();
  // Drop parsed dependency layers and the PCP cache while keeping the composed
  // Stage available. In threaded builds destruction retires in the background;
  // the cache is reconstructed lazily on the next payload or variant edit.
  void ReleaseCompositionCache();
  // Drop large static geometry arrays from the composed stage after a renderer
  // has copied them. Hierarchy and property declarations remain queryable. The
  // next payload/variant rebuild restores full authored values from source.
  Stage::StaticGeometryReleaseStats ReleaseStaticGeometryArrays(
      size_t min_array_elements = 256);
  Stage::StaticGeometryReleaseStats ReleaseStaticGeometryArraysForPrim(
      const UsdPrim& prim, size_t min_array_elements = 256);
  std::string GetWarning() const;
  std::string GetError() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

/// Load a USD file (auto-detects format: USDA, USDC)
/// @param filename Path to the USD file
/// @param stage Output stage (populated on success)
/// @param warn Warning messages (optional)
/// @param err Error messages (optional)
/// @return true on success
bool LoadUSD(const std::string& filename, Stage* stage,
             std::string* warn = nullptr, std::string* err = nullptr);

bool LoadUSD(const std::string& filename, Stage* stage,
             const LoadUSDOptions& options, std::string* warn = nullptr,
             std::string* err = nullptr);

/// Load a USD file and resolve composition arcs (sublayers / references /
/// payloads / inherits / specializes / variants) in place, so the returned
/// stage exposes the fully composed scene. External USDC layers are loaded
/// lazily and anchored to `filename`'s directory. Self-contained / pre-flattened
/// inputs skip composition entirely (identical to LoadUSD). USDA/USDZ external
/// dependencies are supported through the next PCP layer registry, including
/// direct `.usdz` layers and explicit package paths such as
/// `asset.usdz[root.usdc]`.
/// @param comp_opts Optional composition options (populated with defaults if
///                  null). variant_overrides in comp_opts are applied after
///                  authored variant selections.
bool LoadUSDComposed(const std::string& filename, Stage* stage,
                     std::string* warn = nullptr, std::string* err = nullptr,
                     const pcp::CompositionOptions* comp_opts = nullptr);

bool LoadUSDComposed(const std::string& filename, Stage* stage,
                     const LoadUSDOptions& options,
                     std::string* warn = nullptr, std::string* err = nullptr,
                     const pcp::CompositionOptions* comp_opts = nullptr);

/// True when the stage's root layer authors composition arcs (sublayers,
/// references, payloads, inherits, specializes, or variants) that a plain
/// single-layer load leaves unresolved.
bool StageNeedsComposition(const Stage& stage);

/// Compose an already-loaded stage in place through the PCP engine (variants,
/// internal references/inherits/specializes; external arcs resolve through
/// `resolver`). No-op for self-contained stages. `anchor_label` names the root
/// layer in diagnostics and anchors arcs authored in it.
bool ComposeLoadedStage(Stage* stage, AssetResolver& resolver,
                        const std::string& anchor_label,
                        const LoadUSDOptions& load_options,
                        std::string* warn, std::string* err,
                        const pcp::CompositionOptions* comp_opts = nullptr);

/// Convenience overload for memory-rooted stages (wasm): no anchor directory;
/// external arcs resolve only through resolver custom callbacks (none by
/// default). Primary use: applying variant selections / internal arcs after
/// LoadUSDFromMemory[Owned].
bool ComposeLoadedStage(Stage* stage, std::string* warn, std::string* err,
                        const pcp::CompositionOptions* comp_opts = nullptr,
                        const std::string& anchor_label = "");

/// Load USD from an in-memory buffer (auto-detects USDA / USDC / USDZ from the
/// content). Single-layer load only: composition arcs are not resolved (there
/// is no anchor directory for external assets).
bool LoadUSDFromMemory(const uint8_t* data, size_t size, Stage* stage,
                       std::string* warn = nullptr, std::string* err = nullptr);

bool LoadUSDFromMemory(const uint8_t* data, size_t size, Stage* stage,
                       const LoadUSDOptions& options,
                       std::string* warn = nullptr, std::string* err = nullptr);

/// Load USD from an owned in-memory buffer adopted by move. USDA lazy arrays and
/// USDC lazy arrays retain this buffer directly, avoiding an extra heap copy in
/// WASM/browser bindings that already copied JS bytes into a C++ string.
bool LoadUSDFromMemoryOwned(std::string&& data, Stage* stage,
                            const LoadUSDOptions& options = {},
                            std::string* warn = nullptr,
                            std::string* err = nullptr);

/// Load USDA (ASCII) file
bool LoadUSDA(const std::string& filename, Stage* stage,
              std::string* warn = nullptr, std::string* err = nullptr);

bool LoadUSDA(const std::string& filename, Stage* stage,
              const LoadOptions& options, std::string* warn = nullptr,
              std::string* err = nullptr);

/// Load USDC (binary/Crate) file
bool LoadUSDC(const std::string& filename, Stage* stage,
              std::string* warn = nullptr, std::string* err = nullptr);

bool LoadUSDC(const std::string& filename, Stage* stage,
              const USDCLoadOptions& options, std::string* warn = nullptr,
              std::string* err = nullptr);

// ============================================================
// Convenience writing functions (wrap writer APIs)
// ============================================================

/// Write stage to USDA file
bool WriteUSDA(const Stage& stage, const std::string& filename,
               std::string* err = nullptr);

/// Write stage to USDC file
bool WriteUSDC(const Stage& stage, const std::string& filename,
               std::string* err = nullptr);

}  // namespace next
}  // namespace lightusd
