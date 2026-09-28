// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Layer
// Container for PrimSpecs with flat storage and path indexing

#pragma once

#include "prim-spec.hh"
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace lightusd {
namespace next {

/// Layer metadata
struct LayerMeta {
  std::string defaultPrim;
  bool defaultPrim_set = false;
  std::string upAxis = "Y";
  double metersPerUnit = 0.01;
  double timeCodesPerSecond = 24.0;
  double startTimeCode = 0.0;
  double endTimeCode = 0.0;
  // Authored flags for the core fields above: an opinion authored AT the
  // fallback value ("upAxis = \"Y\"", "startTimeCode = 0") must round-trip
  // as authored, and an unauthored field must not be written.
  bool upAxis_set = false;
  bool metersPerUnit_set = false;
  bool timeCodesPerSecond_set = false;
  bool startTimeCode_set = false;
  bool endTimeCode_set = false;

  // Optional stage metadata (parity with the mature reader). The *_set flags
  // distinguish "authored" from "default" so the writer re-emits only authored
  // opinions.
  double framesPerSecond = 24.0;
  bool framesPerSecond_set = false;
  bool hasOwnedSubLayers = false;
  bool hasOwnedSubLayers_set = false;
  double kilogramsPerUnit = 1.0;
  bool kilogramsPerUnit_set = false;
  std::string colorConfiguration;   // asset path
  std::string colorManagementSystem;  // token
  bool colorConfiguration_set = false;
  bool colorManagementSystem_set = false;
  std::string renderSettingsPrimPath;
  bool renderSettingsPrimPath_set = false;

  std::string doc;
  std::string comment;
  std::string owner;
  bool doc_set = false;
  bool comment_set = false;
  bool owner_set = false;
  bool autoPlay = true;
  bool autoPlay_set = false;
  std::string playbackMode;
  bool playbackMode_set = false;

  // Authored pseudo-root namespace order (`reorder rootPrims = [...]`).
  std::vector<std::string> rootPrimOrder;
  bool rootPrimOrder_set = false;

  // Dictionary-valued stage metadata (Dictionary Value; empty when unauthored).
  Value customLayerData;
  Value expressionVariables;
  bool customLayerData_set = false;
  bool expressionVariables_set = false;

  // Layer-level relocates (SdfRelocates, USD 24.11+): composed source path
  // -> new path. Applied by pcp during stage build (cross-arc prims only).
  std::vector<std::pair<std::string, std::string>> relocates;
  // Authored bit: distinguishes explicit-empty `relocates = {}` from
  // unauthored (matters for layer diffing and round-trip fidelity).
  bool relocates_set = false;

  // Sublayer paths for composition
  std::vector<std::string> subLayers;
  // Authored bit for subLayers (explicit-empty `subLayers = []` vs absent).
  bool subLayers_set = false;
  // Per-sublayer layer offsets (offset, scale), parallel to subLayers.
  // May be shorter than subLayers (older files / API construction): missing
  // entries are identity (0, 1).
  std::vector<std::pair<double, double>> subLayerOffsets;

  // Unknown (unmodeled) stage metadata preserved as raw source text in
  // authored order; the USDA writer re-emits it verbatim.
  std::vector<std::pair<std::string, std::string>> unknownMeta;
  // Decodable unregistered USDC fields retained by name and typed value.
  std::vector<TypedExtensionField> unknownFields;

  /// Fill stage-metadata fields this layer leaves unauthored from a WEAKER
  /// layer (a sublayer): stage metadata resolves through the whole root
  /// layer stack in pxr (upAxis/metersPerUnit/timeCodesPerSecond/...), so a
  /// flatten engine must gap-fill before dropping the subLayers list. Call
  /// with sublayers strongest-first.
  void FillAbsentStageMetaFrom(const LayerMeta& weaker);
};

/// Layer - owns all PrimSpecs for a USD file
/// Design:
/// - PrimSpecs stored in flat vector (cache-friendly)
/// - Path-to-index map for O(1) lookup
/// - Root prims stored by index
/// - Children stored as indices (no pointer chasing)
class Layer {
public:
  Layer();
  ~Layer();

  // Move only
  Layer(Layer&&) noexcept;
  Layer& operator=(Layer&&) noexcept;
  Layer(const Layer&) = delete;
  Layer& operator=(const Layer&) = delete;

  // ============================================================
  // Building
  // ============================================================

  /// Reserve space for expected prim count
  void reserve(size_t count);

  /// Add a new PrimSpec and return its index
  uint32_t add_prim(PrimSpec&& spec);

  /// Set parent-child relationship
  void set_parent(uint32_t child_index, uint32_t parent_index);

  /// Add a root prim index
  void add_root(uint32_t index);

  /// Finalize layer (build path index, sort properties)
  void finalize();

  /// (Re)build only the path->index map from current prim paths. Unlike
  /// finalize() this is not guarded and does not sort properties, so it can be
  /// called mid-mutation (e.g. composition pass 2 needs prim_at_path lookups
  /// before the layer is finalized, and after prims are added/renamed).
  void build_path_index();

  /// Sort prims so every ancestor precedes its descendants and subtrees are
  /// contiguous (lexicographic full-path order). The crate writer's
  /// compressed-paths encoding requires this; composition appends grafted
  /// subtrees out of order. Clears the path index (rebuild after).
  void sort_prims_by_path();

  /// Apply authored root/nameChildren ordering to hierarchy index vectors.
  void apply_namespace_ordering();

  /// Clone the layer. PrimSpecs are deep-cloned, while Value array payloads keep
  /// their copy-on-write/lazy shared backing, so this is cheap for crate-backed
  /// large arrays.
  Layer Clone() const;

  // ============================================================
  // Path-addressed authoring (post-load editing)
  // ============================================================

  /// Define (or fetch) a prim at an absolute path, creating missing ancestors
  /// as typeless `def`s (pxr DefinePrim semantics). If the prim already exists,
  /// a non-empty `type_name` overwrites its type and `specifier` is applied.
  /// Keeps the path index and root/child links up to date incrementally.
  /// Returns the prim's index, or UINT32_MAX for an invalid path (empty,
  /// relative, or containing empty components).
  uint32_t define_prim_at_path(const std::string& path,
                               const std::string& type_name = "",
                               PrimSpecifier specifier = PrimSpecifier::Def);

  /// Remove the prim at `path` (and its whole subtree) from the hierarchy:
  /// unlinks it from its parent / the root list and drops all subtree entries
  /// from the path index. The PrimSpec storage itself is append-only, so the
  /// removed specs stay allocated but unreachable (writers traverse from
  /// root_indices). Returns false if no prim exists at `path`.
  bool remove_prim_at_path(const std::string& path);

  // ============================================================
  // Access
  // ============================================================

  /// Get prim by index
  const PrimSpec* prim(uint32_t index) const;
  PrimSpec* prim(uint32_t index);

  /// Get prim by path (O(1) after finalize)
  const PrimSpec* prim_at_path(const Path& path) const;
  const PrimSpec* prim_at_path(const std::string& path) const;
  PrimSpec* prim_at_path_mutable(const std::string& path);

  /// Index of the prim at `path`, or UINT32_MAX if absent (O(1), from the path
  /// index). Lets a caller build a UsdPrim without re-scanning for the index.
  uint32_t index_at_path(const std::string& path) const;

  /// Get prim by index (mutable)
  PrimSpec* prim_mutable(uint32_t index);

  /// Get root prim indices
  const std::vector<uint32_t>& root_indices() const { return root_indices_; }

  /// Replace the root prim order (namespace reordering; e.g. the crate reader
  /// restoring authored order from the pseudo-root's primChildren).
  void set_root_indices(std::vector<uint32_t>&& idx) {
    root_indices_ = std::move(idx);
  }

  /// Get all prims (flat array)
  const std::vector<PrimSpec>& prims() const { return prims_; }

  /// Get prim count
  size_t prim_count() const { return prims_.size(); }

  /// Get children of a prim
  std::vector<const PrimSpec*> children(uint32_t prim_index) const;

  // ============================================================
  // Metadata
  // ============================================================

  const LayerMeta& meta() const { return meta_; }
  LayerMeta& meta() { return meta_; }

  /// Effective timeCodesPerSecond with pxr SdfLayer fallback order:
  /// authored timeCodesPerSecond, else authored framesPerSecond, else 24.
  /// Used for the automatic time scaling between layers authored at
  /// different rates (sublayer/reference arcs).
  double effective_timeCodesPerSecond() const {
    if (meta_.timeCodesPerSecond_set) return meta_.timeCodesPerSecond;
    if (meta_.framesPerSecond_set) return meta_.framesPerSecond;
    return 24.0;
  }

  // ============================================================
  // Memory
  // ============================================================

  /// Get total memory usage
  size_t memory_usage() const;

  /// Get statistics
  struct Stats {
    size_t prim_count;
    size_t root_count;
    size_t total_properties;
    size_t total_time_samples;
    size_t memory_bytes;
  };
  Stats stats() const;

private:
  std::vector<PrimSpec> prims_;
  std::vector<uint32_t> root_indices_;
  std::unordered_map<std::string, uint32_t> path_to_index_;
  LayerMeta meta_;
  bool finalized_ = false;
};

/// Layer builder - helper for constructing layers from parsed data
class LayerBuilder {
public:
  explicit LayerBuilder(Layer& layer);

  /// Start a new prim at the given path
  /// Returns the prim index
  uint32_t begin_prim(const std::string& name, const std::string& type_name,
                      PrimSpecifier specifier = PrimSpecifier::Def);

  /// Strict authoring entry point: validates the prim name (a plain
  /// identifier) before creating it, so the C++ API can't author a
  /// non-identifier prim name that won't round-trip (pxr rejects e.g.
  /// "0abc" / "bad name"). begin_prim is the unvalidated reconstruction
  /// primitive the parser/reader use (the crate reader passes variant
  /// last-components like "{geo=hi}" as the name). Prefer this for authoring;
  /// returns UINT32_MAX (and sets *err) on an invalid name, leaving the layer
  /// unchanged.
  uint32_t define_prim(const std::string& name, const std::string& type_name,
                       PrimSpecifier specifier = PrimSpecifier::Def,
                       std::string* err = nullptr);

  /// End current prim (validates and finalizes)
  void end_prim();

  /// Get current prim being built
  PrimSpec* current();

  /// Add property to current prim
  void add_property(const std::string& name, Value value, uint16_t flags = 0);

  /// Add time sample to current prim
  void add_time_sample(const std::string& prop_name, double time, Value value);

  /// Add relationship to current prim
  void add_relationship(const std::string& name, const Path& target);

  /// Add an attribute connection to current prim
  void add_connection(const std::string& name, const Path& target);

  /// Set metadata on current prim
  void set_active(bool active);
  void set_hidden(bool hidden);

  /// Finalize the layer
  void finalize();

private:
  /// Index of the prim already at `path_str`, or UINT32_MAX.
  ///
  /// begin_prim() must re-open an existing prim rather than append a second
  /// spec with the same path (two same-path specs corrupt a crate write). That
  /// check used to be a LINEAR SCAN of the parent's children -- or of the root
  /// prims -- on every single begin_prim(), making construction of a layer
  /// with N siblings under one parent O(N^2). Building a 40k-prim library took
  /// 34s, essentially all of it in this scan; it is the same shape a USDA/USDC
  /// reader or a URDF/MJCF importer hits on any wide layer.
  ///
  /// Same parent + same name is exactly the same path, so a path->index map is
  /// an equivalent test at O(1). It is rebuilt if the layer gains prims behind
  /// the builder's back, so it can never go stale.
  uint32_t FindExistingPrim(const std::string& path_str);

  Layer& layer_;
  std::vector<uint32_t> prim_stack_;  // Stack of parent indices
  uint32_t current_index_ = UINT32_MAX;
  std::unordered_map<std::string, uint32_t> path_index_;
  size_t indexed_prim_count_ = 0;
  bool path_index_built_ = false;
};

}  // namespace next
}  // namespace lightusd
