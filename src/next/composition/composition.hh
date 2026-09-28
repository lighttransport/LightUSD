// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Composition Arcs
//
// USD composition: references, payloads, inherits, specializes, variants

#pragma once

#include "../crate/crate-limits.hh"
#include "../layer/layer.hh"
#include "expression-variables.hh"
#include "../parser/ascii-parser.hh"
#include "../resolver/asset-resolver.hh"
#include "../../security-policy.hh"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <functional>

namespace lightusd {
namespace next {

/// Composition arc types (in LIVRPS strength order)
enum class ArcType : uint8_t {
  Local = 0,      // Direct opinions in the layer
  Inherits,       // Class inheritance
  VariantSet,     // Variant selection
  Reference,      // Reference to another prim
  Payload,        // Lazy-loaded reference
  Specializes     // Specialization (weaker than local)
};

/// Reference/Payload target
struct CompositionArc {
  ArcType type = ArcType::Reference;
  std::string asset_path;     // External asset (empty for internal)
  std::string prim_path;      // Target prim path (default prim if empty)
  std::string layer_offset;   // Optional layer offset expression
  bool is_internal = false;   // Internal reference (same layer)

  bool operator==(const CompositionArc& other) const {
    return type == other.type &&
           asset_path == other.asset_path &&
           prim_path == other.prim_path;
  }
};

/// Variant selection
struct VariantSelection {
  std::string variant_set;
  std::string variant_name;
};

/// Composition options
struct CompositionOptions {
  bool strict_aousd_conformance = false;
  ExpressionVariablePolicy expression_variable_policy =
      ExpressionVariablePolicy::Evaluate;
  bool load_payloads = true;              // Load payloads (false = unloaded)
  bool resolve_inherits = true;           // Resolve inherits
  bool resolve_specializes = true;        // Resolve specializes
  bool resolve_variants = true;           // Apply variant selections
  int max_depth = 100;                    // Max composition recursion depth
  std::vector<std::string> muted_layers;  // Layers to skip
  // Flatten pipeline memory/parse policy for external USDA layers loaded by the
  // low-memory compositing path.
  size_t max_layer_memory = 0;
  size_t max_array_elements = size_t(16) * 1024 * 1024;
  size_t max_archive_entries = security_policy::kDefaultArchiveEntryCount;
  // USDC structural table-count limits for every crate layer loaded.
  CrateLimits usdc_limits = {};
  ParseOptions usda_parse_options = {};

  // Strongest variant selections for flattening. Keys are either a bare
  // variant-set name ("shape" — applies to every prim carrying that set) or
  // prim-scoped "<primPath>{<set>}" ("/World/B{shape}" — applies to that
  // prim only and wins over the bare-set key; pxr keys selections per prim).
  // Empty keeps authored selections.
  std::map<std::string, std::string> variant_overrides;
};

/// Composition error
struct CompositionError {
  std::string message;
  std::string prim_path;
  std::string arc_path;
  ArcType arc_type;
};

/// Layer loader callback - used to load external layers
using LayerLoader = std::function<std::unique_ptr<Layer>(
    const std::string& resolved_path, std::string* error)>;

/// Compositor - composes layers into a flattened view
class Compositor {
public:
  Compositor();
  explicit Compositor(AssetResolver* resolver);
  ~Compositor();

  /// Set the asset resolver
  void SetResolver(AssetResolver* resolver) { resolver_ = resolver; }

  /// Set layer loader callback
  void SetLayerLoader(LayerLoader loader) { layer_loader_ = std::move(loader); }

  /// Set composition options
  void SetOptions(const CompositionOptions& options) {
    options_ = options;
    if (options_.strict_aousd_conformance) {
      options_.usda_parse_options.strict_aousd_conformance = true;
    }
  }
  const CompositionOptions& GetOptions() const { return options_; }

  // ============================================================
  // Main composition API
  // ============================================================

  /// Compose a root layer into a flattened layer
  /// @param root_layer The root layer to compose
  /// @param anchor_path Path of the root layer (for resolving references)
  /// @return Composed layer, or nullptr on error
  std::unique_ptr<Layer> Compose(const Layer& root_layer,
                                  const std::string& anchor_path = "");

  /// Compose sublayers into a single layer
  std::unique_ptr<Layer> ComposeSublayers(const Layer& root_layer,
                                           const std::string& anchor_path = "");

  /// Get errors from last composition
  const std::vector<CompositionError>& GetErrors() const { return errors_; }

  /// Clear errors
  void ClearErrors() { errors_.clear(); }

  // ============================================================
  // Arc resolution
  // ============================================================

  /// Parse a reference string into a CompositionArc
  static CompositionArc ParseReference(const std::string& ref_str);

  /// Parse a payload string
  static CompositionArc ParsePayload(const std::string& payload_str);

  /// Parse a layer-offset expression "offset:scale" (or "offset").
  static void ParseLayerOffset(const std::string& offset_str, double& offset,
                               double& scale);

  /// Parse variant selection from string "variantSet=selection"
  static VariantSelection ParseVariantSelection(const std::string& str);

  /// Merge local opinions from `source` into `target` (strongest-wins,
  /// fill-absent): type name, properties (incl. connection targets),
  /// relationships, time samples, and metadata. Public so the pcp
  /// value-resolution path can reuse it.
  /// Time-sample times are remapped by the layer offset
  /// `t -> time_offset + time_scale*t` (identity by default), baking a
  /// composition arc's layer offset into the flattened result.
  /// `remap_path`, if set, rewrites relationship/connection TARGET paths from
  /// the source (arc-local) namespace into the composed namespace — pass the
  /// arc's NamespaceMapping::Apply so a referenced asset's internal targets
  /// resolve to their flattened paths. Default (empty) leaves targets verbatim.
  /// `dict_conflicts`, if set, collects dotted key paths of dictionary TYPE
  /// CONFLICTS met while merging weaker dictionary-valued metadata (a key
  /// that is a dictionary on one side and a scalar on the other; the
  /// stronger opinion wins but the weaker subtree is silently shadowed).
  static void CopyLocalOpinions(
      PrimSpec& target, const PrimSpec& source, double time_offset = 0.0,
      double time_scale = 1.0,
      const std::function<std::string(const std::string&)>& remap_path = {},
      std::vector<std::string>* dict_conflicts = nullptr);

  // ============================================================
  // Layer cache
  // ============================================================

  /// Get a cached layer (loads if not cached)
  const Layer* GetCachedLayer(const std::string& path);

  /// Get an external layer with its OWN composition arcs (references / payloads
  /// / inherits / specializes) already expanded, variant selection deferred to
  /// the referencing prim. Cached; falls back to the raw layer when it has no
  /// such arcs or a composition cycle is detected. When `subtree_root` is given,
  /// only that (self-contained) subtree is composed/materialized rather than the
  /// whole layer.
  const Layer* GetComposedExternalLayer(const std::string& path,
                                        const std::string& subtree_root = "");

  /// Verdicts about a referenced subtree, memoized per (layer, subtree root).
  /// Recomputing them per arc is a full linear scan of the referenced layer
  /// and defeats GetComposedExternalLayer's own cache -- see
  /// subtree_arc_cache_.
  struct SubtreeArcInfo {
    bool has_arcs = false;
    bool self_contained = false;  // only meaningful when has_arcs
  };
  SubtreeArcInfo GetSubtreeArcInfo(const Layer& raw,
                                   const std::string& resolved_path,
                                   const std::string& subtree_root);
  bool GetLayerHasArcs(const Layer& raw, const std::string& resolved_path);
  /// Clone the `root_path` subtree of `src` using the cached sorted path
  /// range. Equivalent to the former linear-scan ExtractSubtree.
  std::unique_ptr<Layer> ExtractSubtreeIndexed(const Layer& src,
                                               const std::string& resolved_path,
                                               const std::string& root_path);

  /// One arc-bearing prim of a referenced layer, as recorded in LayerArcIndex.
  struct ArcPrimEntry {
    std::string path;
    bool has_class_arc = false;  // inherits/specializes: never subtree-local
    std::vector<std::string> internal_targets;  // internal ref/payload targets
  };
  /// Per-layer index of the arc-bearing prims, sorted by path, built ONCE per
  /// layer. Answers both subtree verdicts by binary-searching the path-prefix
  /// range instead of rescanning every prim (and re-parsing every arc string)
  /// per reference arc.
  struct LayerArcIndex {
    std::vector<ArcPrimEntry> arc_prims;  // sorted by `path`
    bool has_sublayers = false;
    /// ALL prim indices of the layer, sorted by path. Lets a subtree be
    /// extracted by binary-searching its contiguous path range instead of
    /// rescanning every prim: ExtractSubtree ran once per distinct referenced
    /// prim, so a scene pulling K prims out of an L-prim library was O(K*L)
    /// (3200 refs into 40k prims spent ~8.6s of a 12s compose there).
    /// Indices, not string pointers -- a Layer's prim vector can reallocate.
    std::vector<uint32_t> paths_sorted;
  };
  const LayerArcIndex& GetLayerArcIndex(const Layer& raw,
                                        const std::string& resolved_path);

  /// Clear the layer cache
  void ClearCache();

  /// Get cache size
  size_t GetCacheSize() const { return layer_cache_.size(); }

private:
  AssetResolver* resolver_ = nullptr;
  LayerLoader layer_loader_;
  CompositionOptions options_;
  std::vector<CompositionError> errors_;

  // Layer cache: resolved path -> layer
  std::map<std::string, std::unique_ptr<Layer>> layer_cache_;

  // Cache of external layers with their OWN arcs already expanded (variants
  // deferred), and the set of paths currently being so composed (cross-layer
  // cycle guard). Shared (shared_ptr) with the sub-Compositors that compose
  // referenced layers, so the cache and cycle guard span the whole recursion.
  // Lazily created on first external reference.
  std::shared_ptr<std::map<std::string, std::shared_ptr<Layer>>>
      composed_ext_cache_;
  std::shared_ptr<std::set<std::string>> composing_ext_;

  // Memo for the two subtree verdicts computed before the composed_ext_cache_
  // lookup. Each is a FULL linear scan of the referenced layer's prims (and
  // SubtreeIsSelfContained additionally string-parses every arc), so running
  // them per reference arc defeated composed_ext_cache_ entirely: a scene with
  // N references into an M-prim library layer cost O(N*M). Keyed
  // "<resolved layer path>\n<subtree root path>"; shared with sub-Compositors
  // like the caches above. Verdicts depend only on the layer's immutable
  // content, so they are stable for a layer's lifetime in layer_cache_.
  std::shared_ptr<std::map<std::string, SubtreeArcInfo>> subtree_arc_cache_;
  std::shared_ptr<std::map<std::string, LayerArcIndex>> layer_arc_index_;
  // Same idea for the whole-layer query (LayerHasComposableArcs), keyed by
  // resolved layer path.
  std::shared_ptr<std::map<std::string, bool>> layer_arc_cache_;
  // Resolved sublayer paths currently being composed (cycle guard; shared
  // across the recursion like composing_ext_).
  std::shared_ptr<std::set<std::string>> composing_sublayers_;
  // Arc strings deleted by STRONGER layers, per prim path. Consulted by
  // ResolveArcsForPrim during the nested sublayer composition, where a weaker
  // layer's reference would otherwise be resolved (baked) before the
  // stronger layer's `delete references = ...` can remove it.
  std::map<std::string, std::set<std::string>> pending_arc_deletes_;
  void CollectArcDeletes(const Layer& layer);
  bool ArcDeletedByStronger(const std::string& prim_path,
                            const std::string& arc) const;

  // Composition state (for cycle detection)
  std::vector<std::string> composition_stack_;

  // Pass-2 arc resolution state: prims whose arcs are resolved/in-progress
  // (prevents infinite recursion + redundant recomposition), and grafted
  // subtree prims to append to the result after the pass (cannot add during
  // iteration). Cleared at the start of each Compose().
  std::set<std::string> arc_resolved_;
  // Grafted subtrees buffered during pass 2 (appended afterwards). The anchor
  // (resolved path of the layer the subtree came from) rides along so a later
  // pass can resolve the grafted prims' OWN arcs — e.g. a referenced mesh file
  // whose material prims reference `../../Materials/x.usd` relative to itself.
  struct PendingGraft {
    PrimSpec prim;
    std::string anchor;
  };
  std::vector<PendingGraft> pending_graft_;
  std::set<std::string> graft_paths_;

  // Internal composition methods
  bool ComposeLayer(Layer& target, const Layer& source_layer,
                    const std::string& anchor_path, int depth);
  bool ComposePrim(PrimSpec& target, const Layer& source_layer,
                   const PrimSpec& source, const std::string& anchor_path, int depth);

  // Arc resolution (pass 2). Operates on the already-merged `layer`, expanding
  // each prim's references/payloads/inherits/specializes/variants in LIVRPS
  // strength order (local opinions kept strongest), grafting referenced
  // subtrees, then clearing the resolved arcs. Recursive (targets resolved
  // before being copied) with cycle/recompute guards via arc_resolved_.
  void ResolveArcsForPrim(Layer& layer, PrimSpec& prim,
                          const std::string& anchor_path, int depth);
  // Resolve one reference/payload arc (internal or external) onto `prim`.
  void ResolveRefArc(Layer& layer, PrimSpec& prim, const CompositionArc& arc,
                     const std::string& anchor_path, int depth);
  // Graft the descendant subtree of `src_root` in `src` under `dst_root`.
  void GraftSubtree(const Layer& src, const std::string& src_anchor,
                    const std::string& src_root, const std::string& dst_root,
                    double t_offset = 0.0, double t_scale = 1.0);
  bool ApplyVariants(PrimSpec& prim, const Layer& layer,
                     const std::string& anchor_path, int depth,
                     size_t pending_graft_begin);
  void ApplyOneVariant(PrimSpec& prim, const Layer& layer,
                       const std::string& anchor_path, int depth,
                       const VariantData& variant);

  // Resolve every remaining sparse array edit in the fully composed layer
  // against its (absent -> empty) base array, like pxr's flatten. Runs only
  // at the TOP-LEVEL Compose (compose_depth_ == 1): a nested compose result
  // (sublayer / referenced layer) is one opinion among many, and its edits
  // must survive to stack against the arcs still to come.
  void ResolveArrayEditsInLayer(Layer& layer);
  int compose_depth_ = 0;

 public:
  // Per-prim terminal resolution of stacked sparse array edits: apply each
  // remaining edit over the prim's own base value (or an empty array when no
  // weaker opinion supplied one) and clear the edit. Static so the pcp cache
  // compose -- whose ComposeOpinions sees every source of a prim exactly once
  // -- can resolve at the same "nothing weaker can arrive" point the serial
  // compositor's depth-1 hook represents. Returns false (first message in
  // *err) if any edit failed to apply; failed edits stay authored-as-text.
  static bool ResolveArrayEditsOnPrim(PrimSpec* p, std::string* err);

 private:

  // Helper methods
  void AddError(const std::string& msg, const std::string& prim_path,
                const std::string& arc_path, ArcType type);
  bool CheckCycle(const std::string& path);
  void PushStack(const std::string& path);
  void PopStack();
};

// ============================================================
// Utility functions
// ============================================================

/// Flatten a composed layer (remove composition arcs, keep only final opinions)
void FlattenLayer(Layer& layer);

/// Get all external references from a layer
std::vector<CompositionArc> GetExternalReferences(const Layer& layer);

/// Get all payloads from a layer
std::vector<CompositionArc> GetPayloads(const Layer& layer);

/// Check if a prim has any composition arcs
bool HasCompositionArcs(const PrimSpec& prim);

}  // namespace next
}  // namespace lightusd
