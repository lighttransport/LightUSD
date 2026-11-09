// SPDX-License-Identifier: Apache-2.0
// Variant application and external-layer composition cache member
// functions of Compositor, extracted from composition.cc for compile-time
// parallelism.

#include "composition.hh"
#include "composition-internal.hh"
#include "../layer/array-edit.hh"
#include "../layer/arc-reference.hh"
#include "../layer/listop-field-table.hh"
#include "../../external/fast_float/include/fast_float/fast_float.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

// Apply one SELECTED variant option's opinions onto `prim` (fill-absent):
// inline properties/relationships/doc/state, the content sub-layer (its
// "__self__" root merges onto the prim; descendants graft as children),
// composition arcs authored on the option, and nested variant sets
// (recursive, selection from the nested set's own `selected`).
void Compositor::ApplyOneVariant(PrimSpec& prim, const Layer& layer,
                                 const std::string& anchor_path, int depth,
                                 const PrimVariantData& variant) {
  if (depth > options_.max_depth) return;

  for (const auto& vp : variant.properties) {
    if (!prim.property_value(vp.name)) {
      prim.add_property(vp.name, vp.value, vp.flags);
    }
  }
  for (const auto& [rel_name, targets] : variant.relationships) {
    if (!prim.relationship(rel_name)) {
      for (const auto& target : targets) prim.add_relationship(rel_name, target);
      auto fit = variant.relationshipFlags.find(rel_name);
      if (fit != variant.relationshipFlags.end()) {
        prim.set_relationship_flags(rel_name, fit->second);
      }
    }
  }
  const PrimMetadata& prim_meta = prim.meta();
  if (!variant.doc.empty() && prim_meta.doc().empty()) {
    prim.meta().doc() = variant.doc;
  }
  if (!variant.kind.empty() && !prim.meta().kindAuthored() &&
      prim_meta.kind().empty()) {
    prim.meta().kind() = variant.kind;
    prim.meta().setKindAuthored();
  }
  if (!variant.active) {
    prim.meta().active = false;
    prim.meta().active_authored = true;
  }
  if (variant.hidden) {
    prim.meta().hidden = true;
    prim.meta().hidden_authored = true;
  }

  // Content sub-layer (USDA representation): "__self__" opinions merge onto
  // the prim; its descendants become the prim's children.
  if (variant.content) {
    const Layer& content = *variant.content;
    if (const PrimSpec* self_spec = content.prim_at_path("/__self__")) {
      CopyLocalOpinions(prim, *self_spec);
    }
    GraftSubtree(content, anchor_path, "/__self__", prim.path().str());
  }

  // Composition arcs authored on the option (XGen-style geometry payloads).
  // Resolve at the variant's strength, anchored like the host's own arcs.
  for (const auto& r : variant.references) {
    ResolveRefArc(const_cast<Layer&>(layer), prim, ParseReference(r),
                  anchor_path, depth + 1);
  }
  if (options_.load_payloads) {
    for (const auto& pl : variant.payloads) {
      ResolveRefArc(const_cast<Layer&>(layer), prim, ParsePayload(pl),
                    anchor_path, depth + 1);
    }
  }

  // Nested variant sets on this option. Caller overrides must remain stronger
  // than the selection authored on the outer option, just as they are for
  // top-level variant sets in ApplyVariants().
  for (const auto& nvs : variant.variantSets) {
    std::string chosen = nvs.selected;
    auto override_it = options_.variant_overrides.find(
        prim.path().str() + "{" + nvs.name + "}");
    if (override_it == options_.variant_overrides.end()) {
      override_it = options_.variant_overrides.find(nvs.name);
    }
    if (override_it != options_.variant_overrides.end()) {
      chosen = override_it->second;
    }
    if (chosen.empty()) {
      for (const auto& sel : variant.variantSelections) {
        if (sel.first == nvs.name) {
          chosen = sel.second;
          break;
        }
      }
    }
    if (options_.variant_observer)
      options_.variant_observer(prim.path().str(), nvs, chosen);
    if (chosen.empty()) continue;
    for (const auto& nested : nvs.variants) {
      if (nested.name == chosen) {
        ApplyOneVariant(prim, layer, anchor_path, depth + 1, nested);
        break;
      }
    }
  }
}

bool Compositor::ApplyVariants(PrimSpec& prim, const Layer& layer,
                               const std::string& anchor_path, int depth,
                               size_t pending_graft_begin) {
  if (!options_.resolve_variants) return true;
  const PrimMetadata& prim_meta = prim.meta();

  // Apply EACH variant set's selected variant (a prim may select several sets).
  // Variant opinions are weaker than local opinions already on the prim, so use
  // the dedup-skip-existing copy. The per-set selection is `vs.selected`,
  // falling back to the plural variantSelections() list (the USDA parser
  // stores ALL selections there and only the FIRST in the legacy single
  // string — consulting just the legacy string dropped every set after the
  // first when flattening multi-set USDA), then the legacy single string.
  VariantSelection legacy = ParseVariantSelection(prim.meta().variantSelection);

  // Iterate a SNAPSHOT of the set names, never `prim.meta().variantSets()`
  // directly: the body calls CopyLocalOpinions(prim, ...) / ResolveRefArc(),
  // both of which push_back onto that very vector when the variant content
  // contributes a set the prim does not have yet. A range-for over it is a
  // use-after-free the moment the vector reallocates. Sets that appear DURING
  // the pass are deliberately not applied here — the caller runs a second
  // ApplyVariants pass for arcs merged by references.
  std::vector<std::string> set_names;
  set_names.reserve(prim_meta.variantSets().size());
  for (const auto& vs : prim_meta.variantSets()) set_names.push_back(vs.name);

  for (const std::string& set_name : set_names) {
    // Re-look up the set each iteration; a prior iteration may have
    // reallocated the vector.
    auto find_set = [&prim_meta](const std::string& n) -> const PrimVariantSetData* {
      for (const auto& s : prim_meta.variantSets()) {
        if (s.name == n) return &s;
      }
      return nullptr;
    };
    const PrimVariantSetData* vs_p = find_set(set_name);
    if (!vs_p) continue;

    std::string chosen = vs_p->selected;
    // Prim-scoped override ("<primPath>{<set>}") wins over the bare-set key.
    auto override_it = options_.variant_overrides.find(
        prim.path().str() + "{" + set_name + "}");
    if (override_it == options_.variant_overrides.end()) {
      override_it = options_.variant_overrides.find(set_name);
    }
    if (override_it != options_.variant_overrides.end()) {
      chosen = override_it->second;
    }
    if (chosen.empty()) {
      for (const auto& sel : prim.meta().variantSelections()) {
        if (sel.first == set_name) {
          chosen = sel.second;
          break;
        }
      }
    }
    if (chosen.empty() && set_name == legacy.variant_set) {
      chosen = legacy.variant_name;
    }
    if (options_.variant_observer)
      options_.variant_observer(prim.path().str(), *vs_p, chosen);
    if (chosen.empty()) continue;

    // Copy the selected option out before applying it: ApplyOneVariant mutates
    // `prim`, which owns the vector `variant` would otherwise point into (and
    // the whole recursion below reads through that reference). PrimVariantData
    // copies are shallow in practice — Values are COW and `content` is a
    // shared_ptr<Layer>.
    bool have_variant = false;
    PrimVariantData selected_variant;
    for (const auto& variant : vs_p->variants) {
      if (variant.name != chosen) continue;
      selected_variant = variant;
      have_variant = true;
      break;
    }
    vs_p = nullptr;  // must not be used across the mutating calls below
    if (have_variant) {
      ApplyOneVariant(prim, layer, anchor_path, depth, selected_variant);
    }

    // Read the selected variant's holder prim ("<prim>/{vset=sel}"): copy its
    // own opinions (the variant's properties on the owning prim, as authored in
    // the layer) and graft its descendant subtree (variant CHILD prims) under
    // <prim>. (The vs.variants loop above covers in-memory model variants; this
    // covers reader-produced variants whose content lives in the layer.)
    const std::string dst = prim.path().str();
    const std::string holder =
        dst + "/{" + set_name + "=" + chosen + "}";
    const std::string holder_legacy =
        dst + "/" + prim.name() + "{" + set_name + "=" + chosen + "}";
    std::vector<std::string> holders{holder};
    if (holder_legacy != holder) holders.push_back(holder_legacy);
    for (const std::string& hp : holders) {
      if (const PrimSpec* h = layer.prim_at_path(hp)) {
        CopyLocalOpinions(prim, *h);
      }
      GraftSubtree(layer, anchor_path, hp, dst);
    }

    // The holder (and its children) may have JUST been grafted from a
    // referenced layer in this same pass and still sit in the pending buffer
    // (e.g. a UE mesh asset whose LOD variant content arrives via an external
    // reference). Apply from the pending entries: copy the holder's own
    // opinions and re-root its descendants under the prim, dropping the
    // "{vset=sel}" path segment. Unselected variant content keeps its brace
    // path and is filtered out at append time.
    const std::string holder_alt =
        dst + "{" + set_name + "=" + chosen + "}";
    // Only arcs expanded for this prim can contribute matching variant-holder
    // paths. Older pending grafts belong to previously resolved prims; walking
    // them for every instance made large LOD-heavy scenes O(instances*grafts).
    pending_graft_begin = std::min(pending_graft_begin, pending_graft_.size());
    for (size_t pending_i = pending_graft_begin;
         pending_i < pending_graft_.size(); ++pending_i) {
      auto& pg = pending_graft_[pending_i];
      const std::string& pp = pg.prim.path().str();
      for (const std::string& hp : holders) {
        const std::string hprefix = hp + "/";
        if (pp == hp) {
          pg.prim.remap_target_prefix(hp, dst);
          pg.prim.remap_target_prefix(holder_alt, dst);
          CopyLocalOpinions(prim, pg.prim);
          break;
        }
        if (pp.size() > hprefix.size() &&
            pp.compare(0, hprefix.size(), hprefix) == 0) {
          std::string new_path = dst + "/" + pp.substr(hprefix.size());
          graft_paths_.insert(new_path);
          pg.prim.remap_target_prefix(hp, dst);
          pg.prim.remap_target_prefix(holder_alt, dst);
          pg.prim.set_path(Path(new_path));
          break;
        }
      }
    }
  }

  return true;
}

const Layer* Compositor::GetCachedLayer(const std::string& path) {
  auto it = layer_cache_.find(path);
  if (it != layer_cache_.end()) {
    return it->second.get();
  }

  if (!layer_loader_) return nullptr;

  std::string error;
  auto layer = layer_loader_(path, &error);
  if (!layer) {
    AddError("Failed to load layer: " + error, "", path, ArcType::Reference);
    return nullptr;
  }

  Layer* result = layer.get();
  layer_cache_[path] = std::move(layer);
  return result;
}

const Compositor::LayerArcIndex& Compositor::GetLayerArcIndex(
    const Layer& raw, const std::string& resolved_path) {
  if (!layer_arc_index_) {
    layer_arc_index_ = std::make_shared<std::map<std::string, LayerArcIndex>>();
  }
  auto it = layer_arc_index_->find(resolved_path);
  if (it != layer_arc_index_->end()) return it->second;

  // Built once per layer: ONE pass over the prims, and every arc string is
  // parsed once rather than once per referencing arc.
  LayerArcIndex idx;
  idx.has_sublayers = !raw.meta().subLayers.empty();
  for (size_t i = 0; i < raw.prim_count(); ++i) {
    const PrimSpec* p = raw.prim(static_cast<uint32_t>(i));
    if (!p || !PrimHasComposableArcs(*p)) continue;
    const auto& m = p->meta();
    ArcPrimEntry e;
    e.path = p->path().str();
    e.has_class_arc = !m.inherits.empty() || !m.specializes.empty();
    for (const auto& r : m.references) {
      const CompositionArc a = Compositor::ParseReference(r);
      if (a.is_internal) e.internal_targets.push_back(a.prim_path);
    }
    for (const auto& pl : m.payloads) {
      const CompositionArc a = Compositor::ParsePayload(pl);
      if (a.is_internal) e.internal_targets.push_back(a.prim_path);
    }
    idx.arc_prims.push_back(std::move(e));
  }
  std::sort(idx.arc_prims.begin(), idx.arc_prims.end(),
            [](const ArcPrimEntry& a, const ArcPrimEntry& b) {
              return a.path < b.path;
            });

  idx.paths_sorted.reserve(raw.prim_count());
  for (size_t i = 0; i < raw.prim_count(); ++i) {
    if (raw.prim(static_cast<uint32_t>(i))) {
      idx.paths_sorted.push_back(static_cast<uint32_t>(i));
    }
  }
  std::sort(idx.paths_sorted.begin(), idx.paths_sorted.end(),
            [&raw](uint32_t a, uint32_t b) {
              return raw.prim(a)->path().str() < raw.prim(b)->path().str();
            });

  return (*layer_arc_index_)[resolved_path] = std::move(idx);
}

std::unique_ptr<Layer> Compositor::ExtractSubtreeIndexed(
    const Layer& src, const std::string& resolved_path,
    const std::string& root_path) {
  const LayerArcIndex& idx = GetLayerArcIndex(src, resolved_path);
  const std::vector<uint32_t>& sorted = idx.paths_sorted;
  auto path_of = [&src](uint32_t i) -> const std::string& {
    return src.prim(i)->path().str();
  };
  auto lower = [&](const std::string& key) {
    return std::lower_bound(sorted.begin(), sorted.end(), key,
                            [&](uint32_t i, const std::string& v) {
                              return path_of(i) < v;
                            });
  };

  auto out = std::make_unique<Layer>();
  // The root itself, then its descendants: paths with the prefix "root/"
  // occupy ["root/", "root0") since '/' is 0x2F (see MeshPathIndex in
  // tydra-next for the same construction).
  auto it = lower(root_path);
  if (it != sorted.end() && path_of(*it) == root_path) {
    const uint32_t idx_out = out->add_prim(src.prim(*it)->Clone());
    out->add_root(idx_out);
  }
  const std::string lo_key = root_path + "/";
  std::string hi_key = root_path;
  hi_key += static_cast<char>('/' + 1);
  for (auto d = lower(lo_key), e = lower(hi_key); d != e; ++d) {
    out->add_prim(src.prim(*d)->Clone());
  }
  out->build_path_index();
  return out;
}

Compositor::SubtreeArcInfo Compositor::GetSubtreeArcInfo(
    const Layer& raw, const std::string& resolved_path,
    const std::string& subtree_root) {
  if (!subtree_arc_cache_) {
    subtree_arc_cache_ = std::make_shared<std::map<std::string, SubtreeArcInfo>>();
  }
  // '\x1f' = unit separator, never valid in a path or asset string.
  const std::string key = resolved_path + '\x1f' + subtree_root;
  auto it = subtree_arc_cache_->find(key);
  if (it != subtree_arc_cache_->end()) return it->second;

  // Answer from the per-layer index: the arc-bearing prims of the subtree are
  // the contiguous range [subtree_root, subtree_root + "/\xff") of the sorted
  // paths, plus possibly the root itself. This replaces a FULL scan of the
  // referenced layer (and a re-parse of every arc string) per reference arc,
  // which made a scene of N references into an M-prim library O(N*M) and
  // defeated composed_ext_cache_ entirely.
  const LayerArcIndex& idx = GetLayerArcIndex(raw, resolved_path);
  const std::string prefix = subtree_root + "/";

  auto lower = std::lower_bound(
      idx.arc_prims.begin(), idx.arc_prims.end(), subtree_root,
      [](const ArcPrimEntry& e, const std::string& v) { return e.path < v; });

  SubtreeArcInfo info;
  // Sublayers are composition arcs too.  An external payload/reference may
  // deliberately be an otherwise-empty layer whose target prim is supplied by
  // its sublayers (OpenChessSet's *_payload.usd layers use this pattern).
  // Treating such a layer as arc-free grafted the empty raw layer and made the
  // requested prim appear to be missing.
  info.has_arcs = idx.has_sublayers;
  info.self_contained = !idx.has_sublayers;
  for (auto e = lower; e != idx.arc_prims.end(); ++e) {
    if (!PathInSubtree(e->path, subtree_root, prefix)) {
      // Sorted order: the first path past the prefix range ends it. (An exact
      // match on subtree_root sorts first, so this only fires after it.)
      if (e->path > prefix) break;
      continue;
    }
    info.has_arcs = true;
    if (e->has_class_arc) {
      info.self_contained = false;
      break;
    }
    bool escapes = false;
    for (const std::string& t : e->internal_targets) {
      if (!PathInSubtree(t, subtree_root, prefix)) {
        escapes = true;
        break;
      }
    }
    if (escapes) {
      info.self_contained = false;
      break;
    }
  }
  if (!info.has_arcs) info.self_contained = false;
  (*subtree_arc_cache_)[key] = info;
  return info;
}

bool Compositor::GetLayerHasArcs(const Layer& raw,
                                 const std::string& resolved_path) {
  if (!layer_arc_cache_) {
    layer_arc_cache_ = std::make_shared<std::map<std::string, bool>>();
  }
  auto it = layer_arc_cache_->find(resolved_path);
  if (it != layer_arc_cache_->end()) return it->second;
  const LayerArcIndex& idx = GetLayerArcIndex(raw, resolved_path);
  const bool v = !idx.arc_prims.empty() || idx.has_sublayers;
  (*layer_arc_cache_)[resolved_path] = v;
  return v;
}

const Layer* Compositor::GetComposedExternalLayer(
    const std::string& resolved_path, const std::string& subtree_root) {
  if (!composed_ext_cache_) {
    composed_ext_cache_ =
        std::make_shared<std::map<std::string, std::shared_ptr<Layer>>>();
  }
  if (!composing_ext_) {
    composing_ext_ = std::make_shared<std::set<std::string>>();
  }

  // Cache/cycle key: whole-layer composes share one entry; a self-contained
  // subtree compose is keyed per (layer, subtree) since it materializes only
  // that subtree. ('\x1f' = unit separator, never a valid path/asset char.)
  const std::string key = subtree_root.empty()
                              ? resolved_path
                              : resolved_path + '\x1f' + subtree_root;

  auto cit = composed_ext_cache_->find(key);
  if (cit != composed_ext_cache_->end()) return cit->second.get();

  const Layer* raw = GetCachedLayer(resolved_path);
  if (!raw) return nullptr;

  // Nothing to expand → graft the raw layer directly (no clone/compose cost).
  // (Subtree callers already verified the subtree has arcs.)
  if (subtree_root.empty() && !GetLayerHasArcs(*raw, resolved_path)) return raw;

  // Cross-layer cycle guard: already composing this key → fall back to raw so
  // the recursion terminates (the in-progress layer's own arcs still resolve in
  // the outer composition that owns it).
  if (composing_ext_->count(key)) return raw;
  composing_ext_->insert(key);

  // Compose the external content in its OWN anchor context so its references /
  // payloads resolve relative to itself, but DEFER variant selection: the
  // referencing prim's selection is stronger and is applied by the host's
  // ApplyVariants pass after grafting. A fresh sub-Compositor is used because
  // Compose() resets per-run state (arc_resolved_/pending_graft_/...) that the
  // outer composition is still mid-pass on; the composed-layer cache and the
  // cycle guard are shared so the whole recursion sees them.
  //
  // When `subtree_root` is set the input is just that (self-contained) subtree,
  // so a large multi-prim library is not materialized to pull one prim from it.
  std::unique_ptr<Layer> extracted;
  const Layer* input = raw;
  if (!subtree_root.empty()) {
    extracted = ExtractSubtreeIndexed(*raw, resolved_path, subtree_root);
    input = extracted.get();
  }

  Compositor sub;
  sub.resolver_ = resolver_;
  sub.layer_loader_ = layer_loader_;
  CompositionOptions opts = options_;
  opts.resolve_variants = false;
  sub.options_ = opts;
  sub.composed_ext_cache_ = composed_ext_cache_;
  sub.composing_ext_ = composing_ext_;
  // Share the arc-verdict memos too, so a nested composition does not redo the
  // full-layer scans the outer one already paid for.
  if (!subtree_arc_cache_) {
    subtree_arc_cache_ = std::make_shared<std::map<std::string, SubtreeArcInfo>>();
  }
  if (!layer_arc_cache_) {
    layer_arc_cache_ = std::make_shared<std::map<std::string, bool>>();
  }
  if (!layer_arc_index_) {
    layer_arc_index_ = std::make_shared<std::map<std::string, LayerArcIndex>>();
  }
  sub.subtree_arc_cache_ = subtree_arc_cache_;
  sub.layer_arc_cache_ = layer_arc_cache_;
  sub.layer_arc_index_ = layer_arc_index_;

  std::unique_ptr<Layer> composed = sub.Compose(*input, resolved_path);
  for (const auto& e : sub.errors_) errors_.push_back(e);
  composing_ext_->erase(key);

  if (!composed) return raw;  // fall back to raw on failure
  std::shared_ptr<Layer> shared(std::move(composed));
  const Layer* ptr = shared.get();
  (*composed_ext_cache_)[key] = std::move(shared);
  return ptr;
}


}  // namespace next
}  // namespace lightusd
