// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Composition Implementation
// Full support: references, payloads, inherits, specializes, variants, layer offsets

#include "../layer/int64-listop.hh"
#include "composition.hh"
#include "composition-internal.hh"
#include "../layer/array-edit.hh"
#include "../layer/arc-reference.hh"
#include "../layer/listop-field-table.hh"
#include "../../external/fast_float/include/fast_float/fast_float.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <system_error>
#include <unordered_set>

namespace lightusd {
namespace next {

bool PrimHasComposableArcs(const PrimSpec& p) {
  const auto& m = p.meta();
  return !m.references.empty() || !m.payloads.empty() || !m.inherits.empty() ||
         !m.specializes.empty();
}

bool PathInSubtree(const std::string& path, const std::string& root,
                   const std::string& root_slash) {
  return path == root || (path.size() > root_slash.size() &&
                          path.compare(0, root_slash.size(), root_slash) == 0);
}


Compositor::Compositor() = default;

Compositor::Compositor(AssetResolver* resolver) : resolver_(resolver) {}

Compositor::~Compositor() = default;

std::unique_ptr<Layer> Compositor::Compose(const Layer& root_layer,
                                            const std::string& anchor_path) {
  // Track re-entrancy (sublayer / external-layer composes share this
  // instance): array edits resolve only at depth 1 (see
  // ResolveArrayEditsInLayer).
  struct DepthGuard {
    int& d;
    explicit DepthGuard(int& x) : d(x) { ++d; }
    ~DepthGuard() { --d; }
  } depth_guard(compose_depth_);
  errors_.clear();
  composition_stack_.clear();
  arc_resolved_.clear();
  pending_graft_.clear();
  graft_paths_.clear();
  // Top-level entry only (Compose re-enters itself for sublayers): reset the
  // stronger-layer arc-delete map, then seed it from this layer so weaker
  // sublayers see the deletes while THEIR arcs resolve.
  const bool nested_sublayer_compose =
      composing_sublayers_ && !composing_sublayers_->empty();
  if (!nested_sublayer_compose) pending_arc_deletes_.clear();
  CollectArcDeletes(root_layer);

  auto result = std::make_unique<Layer>();
  result->meta() = root_layer.meta();

  // Pass 1 — merge local opinions. ComposePrim/CopyLocalOpinions use
  // fill-absent semantics (an opinion already on the target wins), so the
  // STRONGEST layer must be merged FIRST: the root layer, then sublayers in
  // authored order (earlier sublayers are stronger). Arc-free scenes are
  // fully composed by this pass alone.
  ComposeLayer(*result, root_layer, anchor_path, 0);
  if (!root_layer.meta().subLayers.empty()) {
    auto sublayer_base = ComposeSublayers(root_layer, anchor_path);
    if (sublayer_base) {
      ComposeLayer(*result, *sublayer_base, anchor_path, 0);
    }
  }

  // Pass 2 — expand composition arcs over the merged layer. Build the path
  // index first so internal-arc / inherit / specialize lookups resolve (the
  // layer is not finalized yet). Iterate by index up to the current count;
  // ResolveArcsForPrim only mutates existing prims (grafted subtrees are
  // buffered in pending_graft_ and appended afterward).
  // The recursive sublayer composition above shares this compositor's state
  // and marked its own prims in arc_resolved_ — those markers must not
  // suppress THIS layer's pass (deferred variants/arcs on merged prims would
  // silently never resolve).
  arc_resolved_.clear();
  result->build_path_index();
  size_t n = result->prim_count();
  for (size_t i = 0; i < n; ++i) {
    PrimSpec* p = result->prim_mutable(static_cast<uint32_t>(i));
    if (p) ResolveArcsForPrim(*result, *p, anchor_path, 0);
  }
  // Append grafted subtree prims brought in by references/payloads, then
  // resolve the grafted prims' OWN arcs (anchored to the layer they came
  // from) — referenced files commonly reference further files relative to
  // themselves (e.g. a mesh asset referencing ../../Materials/x.usd). Each
  // round may graft more subtrees; iterate to a fixed point (depth-capped).
  for (int round = 0; round < 64 && !pending_graft_.empty(); ++round) {
    std::vector<PendingGraft> batch = std::move(pending_graft_);
    pending_graft_.clear();
    std::vector<std::pair<size_t, std::string>> added;  // index in result, anchor
    for (auto& g : batch) {
      std::string gp = g.prim.path().str();
      // Variant-holder prims and unselected variant content keep a "{...}"
      // segment in their path. Drop them only for a final variant-resolving
      // flatten; when composing an external layer with variants deferred, the
      // caller still needs those descendants so its selected holder can be
      // remapped into the host namespace.
      if (options_.resolve_variants && gp.find('{') != std::string::npos) {
        continue;
      }
      if (PrimSpec* existing = result->prim_at_path_mutable(gp)) {
        // A local spec already exists at this path (e.g. the root layer
        // authors `over "LOD1"` material-binding overrides on a prim whose
        // definition arrives via a referenced file's variant). Local opinions
        // win; the graft fills in type/properties/children content.
        CopyLocalOpinions(*existing, g.prim);
        const uint32_t existing_index = result->index_at_path(gp);
        if (existing_index != UINT32_MAX) {
          arc_resolved_.erase(gp);
          added.emplace_back(existing_index, g.anchor);
        }
        continue;
      }
      added.emplace_back(result->prim_count(), g.anchor);
      result->add_prim(std::move(g.prim));
    }
    if (added.empty()) break;
    result->build_path_index();
    for (const auto& a : added) {
      PrimSpec* p = result->prim_mutable(static_cast<uint32_t>(a.first));
      if (p) ResolveArcsForPrim(*result, *p, a.second, 0);
    }
  }
  pending_graft_.clear();
  graft_paths_.clear();

  // The flattened output has all sublayer opinions baked in: keeping the
  // subLayers list would re-apply (now stale) layers on re-read. Stage
  // metadata comes from the ROOT layer only — pxr does not fall back to
  // sublayer-authored stage fields (verified against the 26.05 oracle) —
  // so the consumed list is dropped without any gap-fill.
  result->meta().subLayers.clear();

  // The crate writer's compressed-paths encoding requires ancestors before
  // descendants with contiguous subtrees; grafted prims were appended at the
  // end, so restore hierarchical order first.
  result->sort_prims_by_path();

  // Resolve remaining sparse array edits (over an empty base when no weaker
  // opinion supplied one), so the composed layer carries concrete arrays like
  // a pxr flattened stage.
  if (compose_depth_ == 1) {
    ResolveArrayEditsInLayer(*result);
  }

  // Finalize the composed layer
  result->finalize();

  // Prune the subtree of every deactivated prim (pxr composes NO children
  // under an inactive prim; usdcat --flatten drops the prim itself too — the
  // prim is kept here, with its authored active=false, so consumers can still
  // query it). Only in the FINAL variant-resolving flatten: intermediate
  // composes (nested sublayers / GetComposedExternalLayer, which run with
  // resolve_variants=false) may still be re-activated by a stronger layer.
  if (options_.resolve_variants) {
    std::vector<std::string> prune;
    const size_t pc = result->prim_count();
    for (size_t i = 0; i < pc; ++i) {
      const PrimSpec* p = result->prim(static_cast<uint32_t>(i));
      if (!p || !p->meta().active_authored || p->meta().active) continue;
      for (uint32_t ci : p->child_indices()) {
        if (const PrimSpec* c = result->prim(ci)) {
          prune.push_back(c->path().str());
        }
      }
    }
    // remove_prim_at_path drops the whole subtree from the hierarchy/index
    // (already-removed nested paths simply miss). The specs stay allocated
    // but unreachable — writers/consumers traverse from the roots.
    for (const std::string& cp : prune) result->remove_prim_at_path(cp);

    // Variant holders authored in the ROOT layer itself (Crate encodes variant
    // content as "<prim>{set=option}" specs) were copied into the result and
    // their selected content already grafted; drop them like grafted holders
    // so the flattened layer carries no variant structure (matching pxr and
    // the PCP flatten). Selections are consumed too: an empty `variants = {}`
    // left behind by the selections-authored flag is not part of the output.
    std::vector<std::string> holders;
    for (size_t i = 0; i < result->prim_count(); ++i) {
      PrimSpec* p = result->prim_mutable(static_cast<uint32_t>(i));
      if (!p) continue;
      const std::string path = p->path().str();
      if (path.find('{') != std::string::npos) {
        holders.push_back(path);
        continue;
      }
      if (p->meta().has_ext() && p->meta().variantSelectionsAuthored() &&
          p->meta().variantSelections().empty() &&
          p->meta().variantSelection.empty()) {
        p->meta().setVariantSelectionsAuthored(false);
      }
    }
    for (const std::string& hp : holders) result->remove_prim_at_path(hp);
  }

  return result;
}

bool Compositor::ResolveArrayEditsOnPrim(PrimSpec* p, std::string* err) {
  if (!p || p->array_edits().empty()) return true;
  bool ok = true;
  std::vector<uint32_t> ids;
  ids.reserve(p->array_edits().size());
  for (const auto& kv : p->array_edits()) ids.push_back(kv.first);
  for (uint32_t raw_id : ids) {
    PropNameId nid;
    nid.id = raw_id;
    const ArrayEditData* edit = p->array_edit(nid);
    const PropSlot* slot = p->property(nid);
    if (!edit || !slot) continue;
    Value resolved;
    std::string apply_err;
    if (!ApplyArrayEdit(*edit, p->property_value(nid),
                        static_cast<TypeId>(slot->value_type), &resolved,
                        &apply_err)) {
      if (ok && err) *err = apply_err;
      ok = false;
      continue;
    }
    if (!p->fill_property_value_if_absent(nid, resolved)) {
      p->set_property_value(nid, std::move(resolved));
    }
    p->clear_array_edit(nid);
    p->clear_raw_default_source(nid);
  }
  return ok;
}

void Compositor::ResolveArrayEditsInLayer(Layer& layer) {
  const size_t n = layer.prim_count();
  for (size_t i = 0; i < n; ++i) {
    PrimSpec* p = layer.prim_mutable(static_cast<uint32_t>(i));
    if (!p) continue;
    if (!static_cast<const PrimSpec*>(p)->meta().unknownFields().empty())
      ResolveInt64ListOpFields(&p->meta().unknownFields());
    std::string apply_err;
    if (!ResolveArrayEditsOnPrim(p, &apply_err)) {
      AddError(apply_err, p->path().str(), "", ArcType::Local);
    }
  }
}

std::unique_ptr<Layer> Compositor::ComposeSublayers(const Layer& root_layer,
                                                      const std::string& anchor_path) {
  const auto& sublayer_paths = root_layer.meta().subLayers;
  if (sublayer_paths.empty()) return nullptr;

  // Sublayer cycle guard: a.usda <-> b.usda used to recurse Compose ->
  // ComposeSublayers unbounded (stack-overflow SEGV). Track visiting resolved
  // paths across the recursion (shared like composing_ext_) and skip repeats
  // like pxr does (warn + break the cycle).
  if (!composing_sublayers_) {
    composing_sublayers_ = std::make_shared<std::set<std::string>>();
  }

  auto result = std::make_unique<Layer>();
  result->meta() = root_layer.meta();

  // Sublayers are ordered strongest-first in the USD spec, and the merge
  // below is fill-absent (first opinion wins) — so process in AUTHORED order
  // (strongest first). The previous weakest-first iteration inverted
  // sublayer strength.
  const auto& sublayer_offsets = root_layer.meta().subLayerOffsets;
  for (auto it = sublayer_paths.begin(); it != sublayer_paths.end(); ++it) {
    const size_t sub_index = static_cast<size_t>(it - sublayer_paths.begin());
    const std::string& sublayer_path = *it;

    std::string resolved_path = sublayer_path;
    if (resolver_) {
      resolved_path = resolver_->ResolvePath(
          sublayer_path, anchor_path, !options_.strict_aousd_conformance);
    }

    if (std::find(options_.muted_layers.begin(),
                  options_.muted_layers.end(),
                  resolved_path) != options_.muted_layers.end()) {
      continue;
    }

    if (composing_sublayers_->count(resolved_path)) {
      AddError("Sublayer cycle detected (skipped): " + resolved_path,
               "", sublayer_path, ArcType::Local);
      continue;
    }

    const Layer* sublayer = GetCachedLayer(resolved_path);
    if (!sublayer) {
      AddError("Failed to load sublayer: " + sublayer_path,
               "", sublayer_path, ArcType::Local);
      continue;
    }

    composing_sublayers_->insert(resolved_path);
    // Defer VARIANT resolution inside the sublayer: a variant set and its
    // selection may live in DIFFERENT layers of the stack (set in root,
    // selection in a sublayer or vice versa). Resolving per-sublayer consumed
    // the set before the cross-layer merge, so they never met. The merged
    // top-level pass applies variants once, over the full stack.
    const bool saved_rv = options_.resolve_variants;
    options_.resolve_variants = false;
    auto composed_sub = Compose(*sublayer, resolved_path);
    options_.resolve_variants = saved_rv;
    composing_sublayers_->erase(resolved_path);
    if (composed_sub) {
      // Apply the authored per-sublayer layer offset: every time sample the
      // sublayer contributes maps t -> t*scale + offset in the root's time
      // space. (Nested sublayer offsets were already baked by the recursive
      // Compose above, so this composes correctly through nesting.)
      if (sub_index < sublayer_offsets.size()) {
        double off = sublayer_offsets[sub_index].first;
        double scl = sublayer_offsets[sub_index].second;
        if (!std::isfinite(off) || !std::isfinite(scl) || !(scl > 0.0)) {
          AddError("Invalid sublayer offset; using identity mapping", "",
                   sublayer_path, ArcType::Local);
          off = 0.0;
          scl = 1.0;
        }
        if (off != 0.0 || scl != 1.0) {
          for (size_t pi = 0; pi < composed_sub->prim_count(); ++pi) {
            if (PrimSpec* p = composed_sub->prim_mutable(static_cast<uint32_t>(pi))) {
              p->remap_time_sample_times(off, scl);
            }
          }
        }
      }
      ComposeLayer(*result, *composed_sub, resolved_path, 0);
    }
  }

  return result;
}


void Compositor::CollectArcDeletes(const Layer& layer) {
  for (const PrimSpec& prim : layer.prims()) {
    const ArcListOps* e = prim.meta().arc_edits();
    if (!e) continue;
    auto take = [&](const ArcListOp& a) {
      if (!a.authored || a.deleted.empty()) return;
      auto& set = pending_arc_deletes_[prim.path().str()];
      set.insert(a.deleted.begin(), a.deleted.end());
    };
    take(e->references);
    take(e->payloads);
    take(e->inherits);
    take(e->specializes);
  }
}

bool Compositor::ArcDeletedByStronger(const std::string& prim_path,
                                      const std::string& arc) const {
  auto it = pending_arc_deletes_.find(prim_path);
  return it != pending_arc_deletes_.end() && it->second.count(arc) > 0;
}

void Compositor::ResolveArcsForPrim(Layer& layer, PrimSpec& prim,
                                    const std::string& anchor_path, int depth) {
  if (depth > options_.max_depth) return;
  const std::string self = prim.path().str();
  // Mark before recursing: prevents infinite recursion on cycles and avoids
  // recomposing a prim referenced by several others.
  if (!arc_resolved_.insert(self).second) return;

  // Opinion merging is fill-absent (an opinion already on the prim wins), so
  // arcs must be applied in LIVRPS strength order: local (already present) >
  // inherits > variants > references > payloads > specializes. References
  // were previously applied FIRST, making them stronger than inherits and
  // variants — the inverse of USD semantics.

  // Inherits — copy opinions from same-layer class prims (resolve them
  // first). Arc strings use the canonical "</path>" (or "@asset@</path>")
  // encoding; decode before the path lookup (raw strings never matched, so
  // every inherit failed with "class not found").
  if (options_.resolve_inherits) {
    const bool local_was_over = prim.specifier() == PrimSpecifier::Over;
    bool inherited_class = false;
    bool inherited_def = false;
    for (const auto& inh : prim.meta().inherits) {
      CompositionArc arc = ParseReference(inh);
      // Programmatically-built layers store a bare prim path; the USDA
      // parser stores the canonical "</path>" form. Accept both.
      if (arc.prim_path.empty() && arc.asset_path.empty() && !inh.empty() &&
          inh[0] == '/') {
        arc.prim_path = inh;
      }
      if (!arc.is_internal) {
        AddError("External inherits are not supported by the flatten "
                 "compositor: " + inh, self, inh, ArcType::Inherits);
        continue;
      }
      PrimSpec* cls = layer.prim_at_path_mutable(arc.prim_path);
      if (!cls) {
        AddError("Inherited class not found: " + arc.prim_path, self, inh,
                 ArcType::Inherits);
        continue;
      }
      ResolveArcsForPrim(layer, *cls, anchor_path, depth + 1);
      inherited_class = inherited_class ||
                        cls->specifier() == PrimSpecifier::Class;
      inherited_def = inherited_def || cls->specifier() == PrimSpecifier::Def;
      CopyLocalOpinions(prim, *cls);
      GraftSubtree(layer, anchor_path, arc.prim_path, self);
    }
    // Direct inherits collectively determine an otherwise-undefining prim.
    // A concrete inherited definition wins regardless of list order; a local
    // defining specifier remains stronger and is never changed here.
    if (local_was_over) {
      if (inherited_def) prim.set_specifier(PrimSpecifier::Def);
      else if (inherited_class) prim.set_specifier(PrimSpecifier::Class);
    }
  }

  // Variants (if the reader populated variant content).
  if (options_.resolve_variants) {
    // ApplyVariants only needs to inspect grafts produced while applying this
    // prim's variants. Scanning older grafts is both irrelevant (their paths
    // belong to previously resolved prims) and quadratic for scenes with many
    // referenced variant instances.
    const size_t pending_begin = pending_graft_.size();
    ApplyVariants(prim, layer, anchor_path, depth, pending_begin);
  }

  // References then payloads — both bring in a target prim's opinions plus
  // its descendant subtree. Arcs deleted by a STRONGER layer are skipped
  // (visible here via pending_arc_deletes_ during sublayer composition).
  const size_t arc_pending_begin = pending_graft_.size();
  for (const auto& ref_str : prim.meta().references) {
    if (ArcDeletedByStronger(self, ref_str)) continue;
    ResolveRefArc(layer, prim, ParseReference(ref_str), anchor_path, depth);
  }
  if (options_.load_payloads) {
    for (const auto& pl_str : prim.meta().payloads) {
      if (ArcDeletedByStronger(self, pl_str)) continue;
      ResolveRefArc(layer, prim, ParsePayload(pl_str), anchor_path, depth);
    }
  }

  // Variants again: references/payloads may have merged variant SETS defined
  // in the referenced asset (host selects a referenced set). Their opinions
  // compose at reference strength, which fill-absent gives us here; sets
  // already applied in the first pass are idempotent (fill-absent no-ops).
  if (options_.resolve_variants) {
    ApplyVariants(prim, layer, anchor_path, depth, arc_pending_begin);
  }

  // Specializes (weakest) — same-layer, fill only what is still missing.
  if (options_.resolve_specializes) {
    for (const auto& sp : prim.meta().specializes) {
      CompositionArc sarc = ParseReference(sp);
      if (sarc.prim_path.empty() && sarc.asset_path.empty() && !sp.empty() &&
          sp[0] == '/') {
        sarc.prim_path = sp;
      }
      PrimSpec* spec = sarc.is_internal
                           ? layer.prim_at_path_mutable(sarc.prim_path)
                           : nullptr;
      if (!spec) {
        AddError("Specialized prim not found: " + sarc.prim_path, self, sp,
                 ArcType::Specializes);
        continue;
      }
      ResolveArcsForPrim(layer, *spec, anchor_path, depth + 1);
      CopyLocalOpinions(prim, *spec);
      GraftSubtree(layer, anchor_path, sarc.prim_path, self);
    }
  }

  // Flatten: drop the now-resolved arcs so they are not re-emitted.
  prim.meta().references.clear();
  prim.meta().payloads.clear();
  prim.meta().inherits.clear();
  prim.meta().specializes.clear();
  // Also drop the list-op EDITS: the writers re-emit arcs from surviving
  // qualifiers, so a flattened layer would otherwise carry phantom
  // `prepend references = ...` metadata (pxr then errors on it and would
  // re-compose on top of the baked content).
  prim.meta().clear_arc_edits();
  // Variant sets/selection are consumed ONLY when variants were actually baked
  // (resolve_variants). When variant resolution is DEFERRED (a referenced layer
  // composed by GetComposedExternalLayer, whose variant selection belongs to the
  // referencing prim), the variant metadata must survive so it can be merged
  // into — and baked by — the host. variantSets lives in the lazily-allocated
  // ext; only touch it (the mutable accessor would otherwise allocate an empty
  // ext for every ext-free prim) when an ext already exists.
  if (options_.resolve_variants) {
    if (prim.meta().has_ext()) {
      prim.meta().variantSets().clear();
      prim.meta().variantSetNameEdits() = StringListOps();
      prim.meta().variantSelections().clear();
    }
    prim.meta().variantSelection.clear();
  }
}

void Compositor::ResolveRefArc(Layer& layer, PrimSpec& prim,
                               const CompositionArc& arc,
                               const std::string& anchor_path, int depth) {
  const std::string self = prim.path().str();

  if (arc.is_internal) {
    // Internal arc: target prim lives in this same composed layer.
    const std::string& tp = arc.prim_path;
    if (tp.empty()) return;
    PrimSpec* target = layer.prim_at_path_mutable(tp);
    if (!target) {
      AddError("Internal reference target not found: " + tp, self, tp,
               arc.type);
      return;
    }
    if (tp == self) return;  // self-reference
    ResolveArcsForPrim(layer, *target, anchor_path, depth + 1);
    // Retarget relationship/connection paths from the referenced prim's
    // namespace into the host's (targets outside the referenced subtree are
    // unmappable and dropped, matching pxr).
    auto remap = [&](const std::string& p) -> std::string {
      if (p == tp) return self;
      if (p.size() > tp.size() && p.compare(0, tp.size(), tp) == 0 &&
          (p[tp.size()] == '/' || p[tp.size()] == '.')) {
        return self + p.substr(tp.size());
      }
      return std::string();
    };
    CopyLocalOpinions(prim, *target, 0.0, 1.0, remap);
    GraftSubtree(layer, anchor_path, tp, self);
    return;
  }

  // External arc: load the referenced layer via the loader/cache.
  std::string resolved = arc.asset_path;
  if (resolver_) {
    resolved = resolver_->ResolvePath(
        arc.asset_path, anchor_path, !options_.strict_aousd_conformance);
  }
  if (CheckCycle(resolved)) {
    AddError("Circular reference detected", self, arc.asset_path, arc.type);
    return;
  }
  PushStack(resolved);
  const Layer* raw = GetCachedLayer(resolved);
  if (!raw) {
    AddError("Failed to load reference: " + arc.asset_path, self,
             arc.asset_path, arc.type);
    PopStack();
    return;
  }
  std::string tp = arc.prim_path;
  if (tp.empty()) {
    if (raw->meta().defaultPrim.empty()) {
      // pxr: "Unresolved reference prim path @...@<defaultPrim>" — the arc
      // names no prim and the layer authors no defaultPrim, so it contributes
      // nothing (composition continues; the prim stays empty). Never guess a
      // root prim silently.
      AddError("Unresolved reference prim path @" + arc.asset_path +
                   "@<defaultPrim>: layer has no defaultPrim and the arc "
                   "names no prim path",
               self, arc.asset_path, arc.type);
      PopStack();
      return;
    }
    tp = "/" + raw->meta().defaultPrim;
  }

  // Compose the external target's OWN arcs first (references / payloads /
  // inherits / specializes), variants deferred, so a referenced asset whose
  // content arrives through its own payload or nested references is fully
  // expanded before its opinions are merged here (mirrors the internal-arc path
  // above). Cost-scaled to what is actually needed:
  //   - subtree has NO arcs        → graft the raw layer (compose is identical);
  //   - subtree is self-contained  → compose ONLY that subtree (a big library
  //                                  layer is not materialized for one prim);
  //   - otherwise                  → compose the whole layer (always safe).
  const Layer* ext = raw;
  // Memoized: both verdicts are full linear scans of `raw`, and they run
  // BEFORE the composed_ext_cache_ lookup inside GetComposedExternalLayer, so
  // recomputing them per arc made that cache useless.
  const SubtreeArcInfo arc_info = GetSubtreeArcInfo(*raw, resolved, tp);
  if (arc_info.has_arcs) {
    ext = arc_info.self_contained
              ? GetComposedExternalLayer(resolved, tp)
              : GetComposedExternalLayer(resolved, std::string());
    if (!ext) ext = raw;  // fall back to raw on composition failure
  }
  const PrimSpec* target = ext->prim_at_path(tp);
  if (!target) {
    AddError("Referenced prim not found: " + tp, self, arc.asset_path, arc.type);
    PopStack();
    return;
  }
  // Layer offset: BAKE it into the copied/grafted time samples (nothing ever
  // consumed the previous meta().layer_offset stash, so offsets were a no-op).
  // Chains accumulate naturally: each hop remaps the already-remapped samples
  // of its own graft.
  double t_offset = 0.0, t_scale = 1.0;
  if (!arc.layer_offset.empty()) {
    ParseLayerOffset(arc.layer_offset, t_offset, t_scale);
    if (!std::isfinite(t_offset) || !std::isfinite(t_scale) ||
        !(t_scale > 0.0)) {
      AddError("Invalid reference/payload layer offset; using identity mapping",
               self, arc.asset_path, arc.type);
      t_offset = 0.0;
      t_scale = 1.0;
    }
  }
  // Retarget relationship/connection paths from the referenced namespace into
  // the host's; out-of-scope targets are dropped (pxr behavior).
  auto remap = [&](const std::string& p) -> std::string {
    if (p == tp) return self;
    if (p.size() > tp.size() && p.compare(0, tp.size(), tp) == 0 &&
        (p[tp.size()] == '/' || p[tp.size()] == '.')) {
      return self + p.substr(tp.size());
    }
    return std::string();
  };
  CopyLocalOpinions(prim, *target, t_offset, t_scale, remap);
  GraftSubtree(*ext, resolved, tp, self, t_offset, t_scale);
  PopStack();
}

void Compositor::GraftSubtree(const Layer& src, const std::string& src_anchor,
                              const std::string& src_root,
                              const std::string& dst_root, double t_offset,
                              double t_scale) {
  // Copy every descendant of src_root in `src` to the matching path under
  // dst_root, buffered in pending_graft_ (added after pass 2). Local overrides
  // already present in the result win (checked when appending).
  //
  // Prefer the child-index hierarchy when it proves valid for this subtree. Some
  // composed/intermediate layers can carry stale child indices after cloned or
  // renamed prims, so fall back to the path-prefix scan when validation fails.
  const std::string prefix = src_root + "/";
  auto graft_descendant = [&](const PrimSpec& d) {
    const std::string& dp = d.path().str();
    if (dp.size() <= prefix.size() ||
        dp.compare(0, prefix.size(), prefix) != 0) {
      return;  // not a descendant of src_root
    }
    std::string new_path = dst_root + dp.substr(src_root.size());
    if (!graft_paths_.insert(new_path).second) return;  // already grafted
    PrimSpec g = d.Clone();
    g.set_path(Path(new_path));
    // Retarget relationship/connection paths that point inside the grafted
    // subtree; bake the arc's layer offset into the clone's time samples.
    g.remap_target_prefix(src_root, dst_root);
    if (t_offset != 0.0 || t_scale != 1.0) {
      g.remap_time_sample_times(t_offset, t_scale);
    }
    pending_graft_.push_back(PendingGraft{std::move(g), src_anchor});
  };

  const PrimSpec* root = src.prim_at_path(src_root);
  if (root && root->child_count() > 0) {
    bool valid_child_links = true;
    std::vector<uint32_t> stack(root->child_indices().begin(),
                                root->child_indices().end());
    std::vector<uint32_t> order;
    order.reserve(stack.size());
    const size_t nprims = src.prim_count();
    std::vector<uint8_t> seen(nprims, uint8_t{0});
    while (!stack.empty()) {
      uint32_t idx = stack.back();
      stack.pop_back();
      if (idx >= src.prim_count() || seen[idx]) {
        valid_child_links = false;
        break;
      }
      seen[idx] = 1;
      const PrimSpec* d = src.prim(idx);
      if (!d) {
        valid_child_links = false;
        break;
      }
      const std::string& dp = d->path().str();
      if (dp.size() <= prefix.size() ||
          dp.compare(0, prefix.size(), prefix) != 0) {
        valid_child_links = false;
        break;
      }
      order.push_back(idx);
      const auto& children = d->child_indices();
      stack.insert(stack.end(), children.begin(), children.end());
    }
    if (valid_child_links) {
      for (uint32_t idx : order) {
        if (const PrimSpec* d = src.prim(idx)) {
          graft_descendant(*d);
        }
      }
      return;
    }
  }

  for (size_t i = 0; i < src.prim_count(); ++i) {
    const PrimSpec* d = src.prim(static_cast<uint32_t>(i));
    if (!d) continue;
    graft_descendant(*d);
  }
}

void Compositor::ClearCache() {
  layer_cache_.clear();
  if (composed_ext_cache_) composed_ext_cache_->clear();
}

void Compositor::AddError(const std::string& msg, const std::string& prim_path,
                           const std::string& arc_path, ArcType type) {
  CompositionError err;
  err.message = msg;
  err.prim_path = prim_path;
  err.arc_path = arc_path;
  err.arc_type = type;
  errors_.push_back(err);
}

bool Compositor::CheckCycle(const std::string& path) {
  return std::find(composition_stack_.begin(), composition_stack_.end(), path)
         != composition_stack_.end();
}

void Compositor::PushStack(const std::string& path) {
  composition_stack_.push_back(path);
}

void Compositor::PopStack() {
  if (!composition_stack_.empty()) {
    composition_stack_.pop_back();
  }
}

// ============================================================
// Static parsing methods
// ============================================================

CompositionArc Compositor::ParseReference(const std::string& ref_str) {
  CompositionArc arc;
  arc.type = ArcType::Reference;

  // Format: @asset_path@</prim/path> or </prim/path> for internal
  // Also supports: @asset_path@?layerOffset=offset:scale
  std::string str = ArcReferenceBody(ref_str);

  // Check for asset path
  if (!str.empty() && str[0] == '@') {
    size_t end = str.find('@', 1);
    if (end != std::string::npos) {
      arc.asset_path = str.substr(1, end - 1);
      str = str.substr(end + 1);
    }
  }

  // Check for prim path
  size_t prim_start = str.find('<');
  size_t prim_end = str.find('>');
  if (prim_start != std::string::npos && prim_end != std::string::npos) {
    arc.prim_path = str.substr(prim_start + 1, prim_end - prim_start - 1);
  }

  // Check for layer offset
  size_t offset_pos = str.find("layerOffset=");
  if (offset_pos != std::string::npos) {
    arc.layer_offset = str.substr(offset_pos + 12);
    // Trim trailing content
    size_t end_pos = arc.layer_offset.find_first_of(" \t\n\r)");
    if (end_pos != std::string::npos) {
      arc.layer_offset = arc.layer_offset.substr(0, end_pos);
    }
  }

  arc.is_internal = arc.asset_path.empty();

  return arc;
}

CompositionArc Compositor::ParsePayload(const std::string& payload_str) {
  CompositionArc arc = ParseReference(payload_str);
  arc.type = ArcType::Payload;
  return arc;
}

VariantSelection Compositor::ParseVariantSelection(const std::string& str) {
  VariantSelection sel;
  size_t eq = str.find('=');
  if (eq != std::string::npos) {
    sel.variant_set = str.substr(0, eq);
    sel.variant_name = str.substr(eq + 1);
  }
  return sel;
}

// Locale-independent double parse (replaces std::atof, which honors the C locale
// and could mis-parse under a comma-decimal locale). fast_float is correctly
// rounded and already the parser's value producer. Returns 0.0 on a malformed
// field, matching atof. Tolerates leading whitespace / a single '+' the way atof
// did, since these query fields are not pre-trimmed.
static double ParseOffsetField(const char* first, const char* last) {
  while (first < last && (*first == ' ' || *first == '\t')) ++first;
  const char* p = (first < last && *first == '+') ? first + 1 : first;
  double v = 0.0;
  auto r = fast_float::from_chars(p, last, v);
  return (r.ec == std::errc{}) ? v : 0.0;
}

void Compositor::ParseLayerOffset(const std::string& offset_str,
                                   double& offset, double& scale) {
  offset = 0.0;
  scale = 1.0;

  // Format: "offset:scale" or just "offset"
  const char* b = offset_str.data();
  const char* e = b + offset_str.size();
  size_t colon = offset_str.find(':');
  if (colon == std::string::npos) {
    offset = ParseOffsetField(b, e);
  } else {
    offset = ParseOffsetField(b, b + colon);
    scale = ParseOffsetField(b + colon + 1, e);
  }
}

// ============================================================
// Free utility functions
// ============================================================

void FlattenLayer(Layer& layer) {
  // Remove all composition arc metadata from prims
  for (size_t i = 0; i < layer.prim_count(); ++i) {
    PrimSpec* prim = layer.prim_mutable(static_cast<uint32_t>(i));
    if (!prim) continue;

    auto& meta = prim->meta();
    meta.references.clear();
    meta.payloads.clear();
    meta.inherits.clear();
    meta.specializes.clear();
    meta.clear_arc_edits();
    meta.variantSelection.clear();
    // See note above: avoid allocating an empty ext just to clear variantSets.
    if (meta.has_ext()) {
      meta.variantSets().clear();
      meta.variantSetNameEdits() = StringListOps();
      meta.variantSelections().clear();
    }
  }
}

std::vector<CompositionArc> GetExternalReferences(const Layer& layer) {
  std::vector<CompositionArc> result;

  for (size_t i = 0; i < layer.prim_count(); ++i) {
    const PrimSpec* prim = layer.prim(static_cast<uint32_t>(i));
    if (!prim) continue;

    for (const auto& ref_str : prim->meta().references) {
      CompositionArc arc = Compositor::ParseReference(ref_str);
      if (!arc.is_internal) {
        result.push_back(arc);
      }
    }
  }

  return result;
}

std::vector<CompositionArc> GetPayloads(const Layer& layer) {
  std::vector<CompositionArc> result;

  for (size_t i = 0; i < layer.prim_count(); ++i) {
    const PrimSpec* prim = layer.prim(static_cast<uint32_t>(i));
    if (!prim) continue;

    for (const auto& payload_str : prim->meta().payloads) {
      result.push_back(Compositor::ParsePayload(payload_str));
    }
  }

  return result;
}

bool HasCompositionArcs(const PrimSpec& prim) {
  const auto& meta = prim.meta();
  return !meta.references.empty() ||
         !meta.payloads.empty() ||
         !meta.inherits.empty() ||
         !meta.specializes.empty() ||
         !meta.variantSelection.empty() ||
         !meta.variantSets().empty();
}

bool LayerNeedsComposition(const Layer& layer) {
  if (!layer.meta().subLayers.empty() || !layer.meta().relocates.empty())
    return true;
  for (const PrimSpec& prim : layer.prims()) {
    const PrimMetadata& meta = prim.meta();
    if (HasCompositionArcs(prim) || prim.needs_property_composition() ||
        (meta.active_authored && !meta.active) || !meta.relocates().empty() ||
        meta.variantSetNameEdits().authored ||
        !meta.variantSelections().empty() ||
        (meta.apiSchemaEdits().authored && !meta.apiSchemaEdits().is_explicit))
      return true;
    const ArcListOps* edits = meta.arc_edits();
    if (edits && (edits->references.has_authored_opinion() ||
                  edits->payloads.has_authored_opinion() ||
                  edits->inherits.has_authored_opinion() ||
                  edits->specializes.has_authored_opinion()))
      return true;
  }
  return false;
}

}  // namespace next
}  // namespace lightusd
