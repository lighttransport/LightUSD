// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP PrimIndex
//
// Per-prim composition graph (DAG of CompNodes in strength order). Mirrors the
// design of composition_graph::PrimIndex but on next types. Standalone, C++14.
//
// NOTE (memory): Phase 1 stores the site path as a std::string and children as
// a small vector per node for clarity. Interning the path table and packing
// CompNode into a fixed <=40B struct with uint16 indices is the next
// optimization (tracked in the plan); the public shape here is stable.

#pragma once

#include "../crate/crate-limits.hh"
#include "../parser/ascii-parser.hh"
#include "arc-types.hh"
#include "../composition/expression-variables.hh"
#include "namespace-mapping.hh"
#include "../layer/layer.hh"
#include "../prim/path.hh"
#include "../strfmt.hh"
#include "../../security-policy.hh"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace lightusd {
namespace next {
namespace pcp {

/// The namespace edits a layer stack authors (`relocates`), resolved in THAT
/// stack's own namespace -- pxr's PcpLayerStack relocates tables. Every stack
/// has one (root, referenced, payloaded and variant-content stacks alike): a
/// relocate authored in a referenced layer renames prims inside the referenced
/// namespace, and the rename then maps through the arc into root space.
///
/// Tables are keyed for their consumer (BuildStackRelocates splits by keying):
///   * `src_to_dst` uses the RAW (pre-ancestral-relocation) source, so
///     WithStackRelocates' single-pass relationship/connection target remap
///     pre-chains ancestral relocates (raw target -> FINAL dst).
///   * `departed` / `arrivals` use the COMPOSED (as-authored) path, so
///     SourcesForRelocateSource resolves a source through the composed
///     namespace (its ancestors exist there as arrivals, carrying relocated
///     content AND post-relocation `over` opinions) and ComposeChildNames
///     removes a composed prim's departed children by their composed parent.
struct StackRelocates {
  /// RAW source path -> authored destination path (WithStackRelocates remap).
  std::map<std::string, std::string> src_to_dst;
  /// COMPOSED parent path -> child names that moved away ("prohibited names").
  std::map<std::string, std::set<std::string>> departed;
  /// One prim arriving under a parent through a relocate.
  struct Arrival {
    std::string name;        // composed child name (last component of `dst`)
    std::string src_site;    // COMPOSED source path the content comes from
    std::string dst_site;    // authored destination path (may carry opinions)
  };
  /// COMPOSED destination-parent path -> prims relocated into it.
  std::map<std::string, std::vector<Arrival>> arrivals;

  bool empty() const { return src_to_dst.empty(); }

  const std::set<std::string> *DepartedAt(const std::string &parent) const {
    auto it = departed.find(parent);
    return it == departed.end() ? nullptr : &it->second;
  }
  const std::vector<Arrival> *ArrivalsAt(const std::string &parent) const {
    auto it = arrivals.find(parent);
    return it == arrivals.end() ? nullptr : &it->second;
  }
  const Arrival *ArrivalOf(const std::string &parent,
                           const std::string &name) const {
    const std::vector<Arrival> *list = ArrivalsAt(parent);
    if (!list) return nullptr;
    for (const Arrival &a : *list) {
      if (a.name == name) return &a;
    }
    return nullptr;
  }
  bool IsDeparted(const std::string &parent, const std::string &name) const {
    const std::set<std::string> *names = DepartedAt(parent);
    return names && names->count(name) != 0;
  }
};

/// An ordered stack of layers (strong -> weak): a root/referenced layer plus
/// its composed sublayers. Layers are shared_ptr so a parsed file exists once
/// and is refcount-shared across every node/index that uses it (parse-once).
struct LayerStack {
  std::vector<std::shared_ptr<Layer>> layers;  // strongest first
  std::vector<std::string> layer_identifiers;  // resolved id for each layer
  // Per-layer time offset RELATIVE TO THE STACK ROOT, accumulated through
  // nested sublayer `(offset = ..; scale = ..)` annotations. Parallel to
  // `layers`; entry 0 (the root) is identity.
  std::vector<LayerOffset> layer_offsets;
  std::string identifier;                       // resolved asset path (registry key)
  // Stack-table dedup key: identifier plus a fingerprint of the expression
  // variables inherited from the referencing site (pxr keys layer stacks by
  // (identifier, expression variables) — the same asset referenced under
  // different variable contexts is a DIFFERENT stack when its sublayer
  // expressions can resolve differently). Equals `identifier` when no
  // variables were inherited.
  std::string cache_key;
  LayerOffset offset;                           // cumulative time offset to root
  // Variables authored by this stack, composed weakest-to-strongest.
  Value expression_variables;
  // Namespace edits authored by this stack, in this stack's own namespace.
  StackRelocates relocates;
};

/// One composition source for a prim. The site prim path is interned into a
/// shared path table (owned by the Cache; borrowed by PrimIndex) and referenced
/// by a small index, so per-node storage stays compact and paths are deduped
/// across every cached index.
struct CompNode {
  ArcType arc_type = ArcType::Root;
  NodeFlags flags = NodeFlags::None;
  uint16_t parent = 0xFFFF;          // 0xFFFF == none (root)
  uint32_t layer_stack_idx = 0;      // index into Cache's layer-stack table
  uint32_t site_path_idx = 0;        // index into Cache's interned path table
  LayerOffset offset;                // cumulative time offset for this node
  NamespaceMapping map_to_root;      // remap this node's namespace -> root namespace
  std::vector<uint16_t> children;    // child node indices (build order)

  bool has_specs() const { return HasFlag(flags, NodeFlags::HasSpecs); }
  bool is_inert() const { return HasFlag(flags, NodeFlags::Inert); }
  bool is_culled() const { return HasFlag(flags, NodeFlags::Culled); }
};

/// How `flatten_instances` rewrites native instancing into a self-contained
/// layer.
///   Native  -- keep native instancing (instances carry instance_prototype meta
///              + emit empty; the prototype member holds the subtree).
///   Holder  -- the prototype member stays in place as a non-instanceable
///              content HOLDER; other members `references = </holder-path>`.
///   ExtractedPrototypes -- usdcat-style: each group's content moves to a root
///              `over "/Flattened_Prototype_N"` and EVERY member (holder
///              included) `references = </Flattened_Prototype_N>`.
enum class InstanceFlattenMode { Native, Holder, ExtractedPrototypes };

/// `/Flattened_Prototype_N` numbering scheme for ExtractedPrototypes mode.
///   Deterministic   -- next's own stable rule (groups sorted by prototype
///                      path). Reproducible run-to-run; the default.
///   UsdcatCompatible -- reproduce pxr's two-stage scheme (group order by first
///                      instance path -> `__Prototype_k` labels -> lexicographic
///                      re-sort -> `/Flattened_Prototype_j`) so output aligns
///                      with a deterministically-numbered usdcat. No env var.
enum class PrototypeNumbering { Deterministic, UsdcatCompatible };

/// Composition options.
struct CompositionOptions {
  using VariantSelectionMap =
      std::map<std::string, std::map<std::string, std::string>>;

  /// Opt into fail-closed AOUSD parsing/resolution policy. Compatibility mode
  /// remains the default for legacy assets.
  bool strict_aousd_conformance = false;
  ExpressionVariablePolicy expression_variable_policy =
      ExpressionVariablePolicy::Evaluate;
  bool load_payloads = true;       // default policy when payload_policy is null.
  uint32_t max_depth = 256;        // arc recursion limit / cycle backstop.
  uint32_t max_namespace_depth = 1024;  // composed prim-tree depth backstop
                                        // (BuildStage); errors instead of
                                        // exhausting the C++ stack.
  bool error_when_asset_not_found = false;
  bool detect_instances = true;    // group instanceable prims into prototypes.

  // Flatten instances into a self-contained layer: after BuildStage, each
  // prototype group's prototype member becomes the shared content HOLDER (kept
  // in place, made non-instanceable), and every OTHER instance is emptied +
  // `references = </prototype-member-path>`. So an instance's content is
  // reachable via an internal reference rather than via native instancing.
  // (usdcat instead extracts a separate `/Flattened_Prototype_N` over per group;
  // this is the equivalent de-duplicated flatten using the prototype member as
  // the holder — its pxr-internal prototype numbering is not reproducible.)
  // OFF by default (BuildStage keeps native instancing: prototype member holds
  // the subtree, instances carry instance_prototype meta + emit empty). Enable
  // for a complete flattened layer (next_usdcat -f).
  // Back-compat: a lone `flatten_instances = true` (with instance_flatten_mode
  // left Native) is normalized to InstanceFlattenMode::Holder in Cache::Open.
  bool flatten_instances = false;

  // Instance-flatten representation + (for ExtractedPrototypes) the prototype
  // numbering scheme. Default Native = no rewrite (BuildStage keeps native
  // instancing). See the enums above.
  InstanceFlattenMode instance_flatten_mode = InstanceFlattenMode::Native;
  PrototypeNumbering prototype_numbering = PrototypeNumbering::Deterministic;

  // Phase 7 (S5): apply cross-layer list-op merging when gathering a site's
  // composition arcs. Default ON (AOUSD-conformant): each arc field is composed
  // once across the site's specs weakest->strongest per SdfListOp rules
  // (explicit-replace / prepend / append / delete / dedup) -- validated to match
  // pxrUSD's flatten on cross-layer delete/explicit cases. Set false for the
  // legacy behavior (each spec's arcs expanded independently, i.e. strong-first
  // concatenation; an identically-authored arc in two sublayers expands twice).
  bool apply_list_ops = true;
  int num_threads = 1;             // PrewarmPrimIndices worker hint (see note).
                                   // -1 => auto (hardware_concurrency when threads exist)
  // Number of independent prim-opinion records claimed per parallel fill job.
  // Zero selects the library default. Smaller values improve load balance;
  // larger values reduce scheduler/atomic overhead.
  size_t opinion_batch_size = 0;
  // When true, BuildStage frees each prim's cached composition sources as
  // soon as its opinions are filled and drops the sources cache afterwards,
  // so the transient compose state does not coexist in full with the finished
  // stage (Island: ~400 MB lower peak RSS). Composed output is unaffected; a
  // later per-prim query (GetPropertyStack, ComputePrimIndex, ...) re-resolves
  // the sources it needs instead of hitting the cache. Off by default so a
  // long-lived Cache (e.g. a StageSession with full cache retention) stays
  // warm; the one-shot ComposeStageFromLayer/File always enable it.
  bool release_build_sources = false;

  // Per-layer file/input memory cap for layers loaded by the compositor
  // (sublayers, references, payloads). 0 = no limit.
  size_t max_layer_memory = 0;
  size_t max_array_elements = size_t(16) * 1024 * 1024;
  size_t max_archive_entries = security_policy::kDefaultArchiveEntryCount;
  // USDC structural table-count limits forwarded to every crate layer load.
  CrateLimits usdc_limits = {};

  // USDC backing policy for every file-backed layer loaded by PCP. With both
  // enabled, lazy array Values retain shared mmap-backed CrateDataSources as
  // they are copied into composed PrimSpecs and the rebuilt Stage.
  bool usdc_lazy_arrays = true;
  bool usdc_use_mmap = true;

  // USDA parser options for external USDA layers loaded by this compose path.
  // Keep defaults aligned with next's parser defaults (non-lazy unless the
  // caller enables it in LoadUSDOptions).
  ParseOptions usda_parse_options = {};

  // Emit per-phase timing diagnostics to stderr ([next_compose]/[next_build]/
  // [next_warm]). Off by default. Replaces the former LIGHTUSD_NEXT_TIMING env
  // read so the composition core takes no implicit process-environment input;
  // the CLI sets this from its own flag/env.
  bool enable_timing = false;

  /// Per-payload load policy. Invoked with (prim path that authors the payload,
  /// payload asset path); return true to load, false to defer. When null, the
  /// `load_payloads` flag is used. (Per-prim Load/UnloadPayload overrides this.)
  /// Must be thread-safe when PrewarmPrimIndices runs with num_threads != 1.
  std::function<bool(const Path &, const std::string &)> payload_policy;

  /// Extended payload policy with the authoring PrimSpec. Takes precedence
  /// over `payload_policy` when set; the two-argument form remains for source
  /// compatibility and simple path/asset filters.
  std::function<bool(const Path &, const std::string &, const PrimSpec &)>
      payload_policy_with_prim;
  /// Called when composition elects to resolve a payload arc. Used for live
  /// progress only; must be thread-safe when parallel composition is enabled.
  std::function<void(const Path &)> payload_load_callback;

  /// Fallback variant selections, consulted ONLY when no selection is authored
  /// anywhere for that set (pxr's PcpVariantFallbackMap / UsdStage global
  /// variant fallbacks). Candidates are tried in order; the first one the set
  /// actually defines is selected. EMPTY by default: stock OpenUSD registers
  /// no fallbacks (the classic `standin -> render` map is a Presto plugin
  /// registration) — pass {"standin", {"render", "proxy"}} to opt in.
  std::map<std::string, std::vector<std::string>> variant_fallbacks;

  /// Variant selection overrides: map of variantSet -> variantName. Overrides
  /// any authored variantSelection on the same set (stronger than authored).
  /// Empty by default (use authored selections as-is). Example:
  ///   {{"districtLod", "full"}} selects the "full" variant on every prim that
  ///   defines a "districtLod" variantSet.
  std::map<std::string, std::string> variant_overrides;

  /// Path-scoped variant overrides. The outer key is an absolute prim path;
  /// the inner map is variantSet -> selection. An exact path-scoped override
  /// wins over the global variant_overrides entry for the same set.
  VariantSelectionMap variant_overrides_by_path;
};

/// The composed graph for a single prim. Borrows its layer-stack table from the
/// owning Cache (the table outlives every PrimIndex it backs).
class PrimIndex {
 public:
  static constexpr uint16_t kInvalidNode = 0xFFFF;
  static constexpr size_t kMaxNodeCount = kInvalidNode;

  PrimIndex() = default;

  const Path &GetPath() const { return prim_path_; }
  const CompNode &GetRootNode() const { return nodes_[0]; }
  const CompNode &GetNode(uint16_t i) const { return nodes_[i]; }
  uint16_t GetNodeCount() const { return static_cast<uint16_t>(nodes_.size()); }
  const std::vector<CompNode> &GetNodes() const { return nodes_; }
  const std::vector<uint16_t> &GetStrengthOrder() const { return strength_order_; }
  const std::deque<LayerStack> *GetLayerStacks() const { return layer_stacks_; }

  /// Resolve a node's interned site prim path (empty if no path table bound).
  ///
  /// The path/layer tables are std::deque (Phase 9 F3): appends never move
  /// existing elements, so a PrimIndex handed to another thread can resolve its
  /// nodes' sites while a concurrent build appends new entries -- no dangling
  /// reference. The bounds check uses `path_table_size_`, a snapshot captured
  /// (under the build lock) when this index was bound to the table, instead of
  /// the live `deque::size()`, so it is also race-free against those appends.
  const std::string &SitePath(const CompNode &n) const {
    static const std::string kEmpty;
    if (!path_table_ || n.site_path_idx >= path_table_size_) return kEmpty;
    return (*path_table_)[n.site_path_idx];
  }

  bool HasAnySpecs() const {
    for (const auto &n : nodes_) if (n.has_specs()) return true;
    return false;
  }

  std::string DumpToString() const {
    std::string s = "PrimIndex<" + prim_path_.str() + "> nodes=" +
                    UIntToStr(nodes_.size()) + "\n";
    for (uint16_t oi : strength_order_) {
      const CompNode &n = nodes_[oi];
      s += "  [" + UIntToStr(oi) + "] " + ArcTypeName(n.arc_type) +
           " stack=" + UIntToStr(n.layer_stack_idx) + " site=" +
           SitePath(n) + (n.has_specs() ? " (specs)" : "") + "\n";
    }
    return s;
  }

  // Build API (used by Cache::Impl).
  void SetPath(const Path &p) { prim_path_ = p; }
  void SetLayerStacks(const std::deque<LayerStack> *t) { layer_stacks_ = t; }
  // `published_size` is the table size at bind time; SitePath bounds-checks
  // against it (race-free) rather than the live deque size (see SitePath).
  void SetPathTable(const std::deque<std::string> *t, size_t published_size) {
    path_table_ = t;
    path_table_size_ = published_size;
  }
  uint16_t AddNode(CompNode &&n) {
    if (nodes_.size() >= kMaxNodeCount) return kInvalidNode;
    nodes_.push_back(std::move(n));
    return static_cast<uint16_t>(nodes_.size() - 1);
  }
  CompNode &MutableNode(uint16_t i) { return nodes_[i]; }
  void SetStrengthOrder(std::vector<uint16_t> &&o) { strength_order_ = std::move(o); }

 private:
  Path prim_path_;
  std::vector<CompNode> nodes_;            // node 0 == root
  std::vector<uint16_t> strength_order_;   // strongest first
  // Borrowed, stable-address tables owned by the Cache (std::deque so concurrent
  // appends never invalidate elements a held PrimIndex resolves; F3).
  const std::deque<LayerStack> *layer_stacks_ = nullptr;
  const std::deque<std::string> *path_table_ = nullptr;   // interned site paths
  size_t path_table_size_ = 0;  // snapshot of path_table size at bind time
};

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
