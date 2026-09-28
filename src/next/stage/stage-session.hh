// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Focused next-core loading and persistent document-session API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../execution.hh"
#include "../operation-status.hh"
#include "../pcp/cache.hh"
#include "../reader/usda-reader.hh"
#include "../reader/usdc-reader.hh"
#include "../reader/usdz-reader.hh"
#include "../resolver/asset-resolver.hh"
#include "../resource-limits.hh"
#include "../../security-policy.hh"
#include "change-set.hh"
#include "stage.hh"

namespace lightusd {
namespace next {

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
  /// Change record that produced the currently published snapshot. The value
  /// is copied under the publication lock and remains valid after later edits.
  StageChangeSet GetLastChangeSet() const;
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
  size_t GetCompositionIssueCount() const;
  bool GetCompositionIssue(size_t index,
                           pcp::Cache::CompositionIssue* out) const;
  std::vector<std::string> GetLayerDependencies() const;
  std::vector<Diagnostic> GetDiagnostics() const;
  StageSessionMemoryStats GetMemoryStats() const;
  OperationStatus TrimCaches();
  // Drop parsed dependency layers and the PCP cache while keeping the composed
  // Stage available. In threaded builds destruction retires in the background;
  // the cache is reconstructed lazily on the next payload or variant edit.
  OperationStatus ReleaseCompositionCache();
  // Drop large static geometry arrays from the composed stage after a renderer
  // has copied them. Hierarchy and property declarations remain queryable. The
  // next payload/variant rebuild restores full authored values from source.
  Stage::StaticGeometryReleaseStats ReleaseStaticGeometryArrays(
      size_t min_array_elements = 256);
  Stage::StaticGeometryReleaseStats ReleaseStaticGeometryArraysForPrim(
      const UsdPrim& prim, size_t min_array_elements = 256);
  // NULL path releases all eligible arrays. A prim path is resolved after COW;
  // retained snapshots keep their geometry. Per-prim calls omit stage byte totals.
  // Failure leaves out unchanged. Non-composed sessions are unsupported.
  OperationStatus ReleaseStaticGeometryArraysByPath(
      const Path* prim_path, size_t min_array_elements,
      Stage::StaticGeometryReleaseStats* out);
  std::string GetWarning() const;
  std::string GetError() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace next
}  // namespace lightusd
