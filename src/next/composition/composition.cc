// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Composition Implementation
// Full support: references, payloads, inherits, specializes, variants, layer offsets

#include "composition.hh"
#include "../layer/array-edit.hh"
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

namespace {

// Does the prim author a composition arc that must be expanded (references /
// payloads / inherits / specializes)? Variants are intentionally excluded: their
// selection is deferred to the referencing prim, so a variants-only prim needs
// no pre-composition.
bool PrimHasComposableArcs(const PrimSpec& p) {
  const auto& m = p.meta();
  return !m.references.empty() || !m.payloads.empty() || !m.inherits.empty() ||
         !m.specializes.empty();
}

// NOTE: LayerHasComposableArcs() is gone too -- Compositor::GetLayerHasArcs()
// answers it from the cached per-layer arc index instead of rescanning.

// Is `path` equal to `root` or a descendant of it?
bool PathInSubtree(const std::string& path, const std::string& root,
                   const std::string& root_slash) {
  return path == root || (path.size() > root_slash.size() &&
                          path.compare(0, root_slash.size(), root_slash) == 0);
}

// NOTE: the former SubtreeHasComposableArcs()/SubtreeIsSelfContained() helpers
// are gone. Both were full linear scans of the referenced layer run per
// reference arc; Compositor::GetSubtreeArcInfo() answers the same two
// questions from the per-layer arc index (built once, queried by binary
// search over the sorted path range).

// AOUSD §6.6.2 / §12.2: dictionary opinions combine recursively. Keys from
// the stronger value keep their values; missing keys (including nested keys)
// are filled from the weaker value.
void MergeWeakerDictionary(Value* stronger, const Value& weaker,
                           std::vector<std::string>* conflicts = nullptr,
                           const std::string& key_prefix = std::string()) {
  MergeWeakerDictionaryValue(stronger, weaker, conflicts, key_prefix);
}

void MergeWeakerPropMeta(PropMeta* stronger, const PropMeta& weaker) {
  if (!stronger) return;
  auto fill = [&](uint32_t bit, auto* destination, const auto& source) {
    if ((weaker.authored & bit) && !(stronger->authored & bit)) {
      *destination = source;
      stronger->authored |= bit;
    }
  };
  fill(PropMeta::kInterpolation, &stronger->interpolation,
       weaker.interpolation);
  fill(PropMeta::kElementSize, &stronger->elementSize, weaker.elementSize);
  fill(PropMeta::kColorSpace, &stronger->colorSpace, weaker.colorSpace);
  fill(PropMeta::kDisplayName, &stronger->displayName, weaker.displayName);
  fill(PropMeta::kDisplayGroup, &stronger->displayGroup, weaker.displayGroup);
  fill(PropMeta::kDoc, &stronger->doc, weaker.doc);
  fill(PropMeta::kComment, &stronger->comment, weaker.comment);
  fill(PropMeta::kHidden, &stronger->hidden, weaker.hidden);
  fill(PropMeta::kRenderType, &stronger->renderType, weaker.renderType);
  fill(PropMeta::kConnectability, &stronger->connectability,
       weaker.connectability);
  fill(PropMeta::kOutputName, &stronger->outputName, weaker.outputName);
  fill(PropMeta::kBindMaterialAs, &stronger->bindMaterialAs,
       weaker.bindMaterialAs);
  fill(PropMeta::kKind, &stronger->kind, weaker.kind);
  fill(PropMeta::kPermission, &stronger->permission, weaker.permission);
  fill(PropMeta::kWeight, &stronger->weight, weaker.weight);
  fill(PropMeta::kUnauthoredIdx, &stronger->unauthoredValuesIndex,
       weaker.unauthoredValuesIndex);
  fill(PropMeta::kAllowedTokens, &stronger->allowedTokens,
       weaker.allowedTokens);

  auto merge_dictionary = [&](uint32_t bit, Value* destination,
                              const Value& source) {
    if (!(weaker.authored & bit)) return;
    if (!(stronger->authored & bit)) {
      *destination = source;
      stronger->authored |= bit;
    } else {
      MergeWeakerDictionary(destination, source);
    }
  };
  merge_dictionary(PropMeta::kCustomData, &stronger->customData,
                   weaker.customData);
  merge_dictionary(PropMeta::kAssetInfo, &stronger->assetInfo,
                   weaker.assetInfo);
  merge_dictionary(PropMeta::kSdrMetadata, &stronger->sdrMetadata,
                   weaker.sdrMetadata);

  if (weaker.authored & PropMeta::kUnknownMeta) {
    for (const auto& field : weaker.unknownMeta) {
      const bool present = std::find_if(
          stronger->unknownMeta.begin(), stronger->unknownMeta.end(),
          [&](const auto& own) { return own.first == field.first; }) !=
          stronger->unknownMeta.end();
      if (!present) stronger->unknownMeta.push_back(field);
    }
    stronger->authored |= PropMeta::kUnknownMeta;
  }
  MergeWeakerExtensionFields(&stronger->unknownFields, weaker.unknownFields);
}

std::string ResolveWeakerPathExpressionText(const std::string& stronger,
                                            const std::string& weaker) {
  if (stronger.find("%_") == std::string::npos) return stronger;
  const std::string replacement = weaker.empty()
      ? "(/ & ~/)" : ("(" + weaker + ")");
  std::string out = stronger;
  size_t pos = 0;
  while ((pos = out.find("%_", pos)) != std::string::npos) {
    out.replace(pos, 2, replacement);
    pos += replacement.size();
  }
  return out;
}

void ApplyRelationshipEdit(std::vector<Path>* values, const ArcEdit& edit) {
  if (!values) return;
  auto as_path = [](const std::string& s) { return Path(s); };
  auto erase_items = [&](const std::vector<std::string>& items) {
    values->erase(std::remove_if(values->begin(), values->end(),
                                 [&](const Path& p) {
                                   return std::find(items.begin(), items.end(),
                                                    p.str()) != items.end();
                                 }),
                  values->end());
  };
  auto add_unique = [&](const Path& p, bool front) {
    values->erase(std::remove(values->begin(), values->end(), p),
                  values->end());
    if (front) values->insert(values->begin(), p);
    else values->push_back(p);
  };
  erase_items(edit.deleted);
  // Insert prepended items in reverse so their authored order is retained.
  for (auto it = edit.prepended.rbegin(); it != edit.prepended.rend(); ++it) {
    add_unique(as_path(*it), true);
  }
  for (const std::string& item : edit.added) {
    const Path path = as_path(item);
    if (std::find(values->begin(), values->end(), path) == values->end()) {
      values->push_back(path);
    }
  }
  for (const std::string& item : edit.appended) {
    add_unique(as_path(item), false);
  }
  if (!edit.ordered.empty()) {
    std::vector<Path> ordered;
    ordered.reserve(values->size());
    for (const std::string& item : edit.ordered) {
      Path p(item);
      auto it = std::find(values->begin(), values->end(), p);
      if (it != values->end() &&
          std::find(ordered.begin(), ordered.end(), p) == ordered.end()) {
        ordered.push_back(p);
      }
    }
    for (const Path& p : *values) {
      if (std::find(ordered.begin(), ordered.end(), p) == ordered.end()) {
        ordered.push_back(p);
      }
    }
    *values = std::move(ordered);
  }
}

// Clone the prim at `root_path` and its descendants into a fresh layer (paths
// kept; `root_path` prim is the layer root). Used to compose a self-contained
// referenced subtree alone. child_indices on the clones reference the source
// layer and are stale, but composition (flat iteration + path lookups, with
// GraftSubtree validating / falling back to a path scan) does not rely on them
// — the same way Compose()'s own output carries stale child_indices.
// NOTE: the former linear-scan ExtractSubtree() is gone; the subtree is now
// cloned from the per-layer sorted path range (Compositor::ExtractSubtreeIndexed),
// so pulling K prims out of an L-prim library is no longer O(K*L).

}  // namespace

// ============================================================
// Compositor
// ============================================================

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

bool Compositor::ComposeLayer(Layer& target, const Layer& source_layer,
                               const std::string& anchor_path, int depth) {
  if (depth > options_.max_depth) {
    AddError("Max composition depth exceeded", "", anchor_path, ArcType::Reference);
    return false;
  }

  // The dedup lookup below needs a current path index. Without it every
  // source prim was cloned AGAIN into the target (one duplicate spec per
  // contributing layer), and the crate round-trip of a sublayered flatten
  // produced multiple same-path specs whose values collapsed into husks.
  target.build_path_index();

  // Compose each prim from source_layer into target.
  std::vector<uint32_t> added;  // indices of freshly cloned prims
  for (size_t i = 0; i < source_layer.prim_count(); ++i) {
    const PrimSpec* src_prim = source_layer.prim(static_cast<uint32_t>(i));
    if (!src_prim) continue;

    std::string prim_path = src_prim->path().str();
    PrimSpec* target_prim = target.prim_at_path_mutable(prim_path);

    if (!target_prim) {
      // Create new prim in target layer via Clone(). The clone's
      // child_indices reference the SOURCE layer and are meaningless here;
      // links are re-established from paths below.
      PrimSpec new_prim = src_prim->Clone();
      new_prim.clear_child_indices();
      uint32_t idx = target.add_prim(std::move(new_prim));
      added.push_back(idx);
    } else {
      // Existing prim - compose source onto it
      ComposePrim(*target_prim, source_layer, *src_prim, anchor_path, depth);
    }
  }

  // Link freshly added prims into the hierarchy: child of an existing (or
  // just-added) parent, or a root prim. (Previously every clone was added as
  // a ROOT, so sublayer children leaked to the stage root.) One index rebuild
  // covers all adds; a well-formed source has no duplicate paths, so the
  // in-loop lookups never needed the additions.
  if (!added.empty()) target.build_path_index();
  for (uint32_t idx : added) {
    const std::string path = target.prim(idx)->path().str();
    size_t slash = path.find_last_of('/');
    if (slash == 0 || slash == std::string::npos) {
      target.add_root(idx);
      continue;
    }
    uint32_t pidx = target.index_at_path(path.substr(0, slash));
    if (pidx != UINT32_MAX) {
      target.set_parent(idx, pidx);
    } else {
      target.add_root(idx);  // orphan without a composed parent
    }
  }

  return true;
}

bool Compositor::ComposePrim(PrimSpec& target, const Layer& source_layer,
                              const PrimSpec& source,
                              const std::string& anchor_path, int depth) {
  (void)source_layer;
  (void)anchor_path;
  if (depth > options_.max_depth) return false;
  // Pass 1 only merges local opinions across layers (root strongest first,
  // then sublayers). CopyLocalOpinions fills in only what `target` lacks.
  // Composition arcs are expanded later in pass 2 (ResolveArcsForPrim), once
  // the full local-opinion layer exists — so the ARCS themselves must merge
  // here (append weaker after stronger; stronger arcs resolve first and win
  // under fill-absent), or a sublayer-defined prim's references vanish when
  // a stronger layer also has a spec for it.
  std::vector<std::string> dict_conflicts;
  CopyLocalOpinions(target, source, 0.0, 1.0, {}, &dict_conflicts);
  for (const std::string& key : dict_conflicts) {
    AddError("Dictionary type conflict at `" + key +
                 "`: the weaker layer's opinion (dictionary vs scalar) is "
                 "shadowed by the stronger layer",
             target.path().str(), "", ArcType::Local);
  }
  // A STRONGER layer's `delete <arc>` list-edit removes matching arcs coming
  // from weaker layers (previously the union ignored ArcEdits entirely, so
  // `delete references = @a@` in the root still composed @a@ from a sublayer).
  auto deleted_by = [](const ArcEdit* e, const std::string& a) {
    return e && e->authored &&
           std::find(e->deleted.begin(), e->deleted.end(), a) != e->deleted.end();
  };
  auto merge_arcs = [&](std::vector<std::string>& dst,
                        const std::vector<std::string>& src,
                        const ArcEdit* tgt_edit) {
    for (const std::string& a : src) {
      if (deleted_by(tgt_edit, a)) continue;
      if (std::find(dst.begin(), dst.end(), a) == dst.end()) {
        dst.push_back(a);
      }
    }
  };
  const ArcListOpEdits* te = target.meta().arc_edits();
  merge_arcs(target.meta().references, source.meta().references,
             te ? &te->references : nullptr);
  merge_arcs(target.meta().payloads, source.meta().payloads,
             te ? &te->payloads : nullptr);
  merge_arcs(target.meta().inherits, source.meta().inherits,
             te ? &te->inherits : nullptr);
  merge_arcs(target.meta().specializes, source.meta().specializes,
             te ? &te->specializes : nullptr);
  // Propagate the source's own delete edits down the stack (they must also
  // remove arcs authored in still-weaker layers merged later).
  if (const ArcListOpEdits* se = source.meta().arc_edits()) {
    auto merge_deleted = [&](const ArcEdit& from, ArcEdit& into) {
      if (!from.authored || from.deleted.empty()) return;
      into.authored = true;
      if (into.deleted.empty() && into.prepended.empty() &&
          into.appended.empty() && into.ordered.empty()) {
        into.is_explicit = false;
      }
      for (const std::string& d : from.deleted) {
        if (std::find(into.deleted.begin(), into.deleted.end(), d) ==
            into.deleted.end()) {
          into.deleted.push_back(d);
        }
      }
    };
    ArcListOpEdits& tedits = target.meta().ensure_arc_edits();
    merge_deleted(se->references, tedits.references);
    merge_deleted(se->payloads, tedits.payloads);
    merge_deleted(se->inherits, tedits.inherits);
    merge_deleted(se->specializes, tedits.specializes);
  }
  // Variant sets/selections: fill-absent by set name. (variantSetNames
  // list-op edits are merged inside CopyLocalOpinions' registered string
  // list-op field loop.)
  if (!source.meta().variantSets().empty()) {
    for (const VariantSetData& svs : source.meta().variantSets()) {
      bool have = false;
      for (const VariantSetData& tvs : target.meta().variantSets()) {
        if (tvs.name == svs.name) {
          have = true;
          break;
        }
      }
      if (!have) target.meta().variantSets().push_back(svs);
    }
  }
  if (!source.meta().variantSelections().empty()) {
    for (const auto& sel : source.meta().variantSelections()) {
      bool have = false;
      for (const auto& t : target.meta().variantSelections()) {
        if (t.first == sel.first) {
          have = true;
          break;
        }
      }
      if (!have) target.meta().variantSelections().push_back(sel);
    }
  }
  if (target.meta().variantSelection.empty() &&
      !source.meta().variantSelection.empty()) {
    target.meta().variantSelection = source.meta().variantSelection;
  }
  return true;
}

void Compositor::CopyLocalOpinions(
    PrimSpec& target, const PrimSpec& source, double time_offset,
    double time_scale,
    const std::function<std::string(const std::string&)>& remap_path,
    std::vector<std::string>* dict_conflicts) {
  // Remap a relationship/connection target. An empty remap result means the
  // target is not expressible in the composed namespace (outside the arc's
  // scope) and must be DROPPED, not copied verbatim.
  auto map_target = [&](const Path& p) -> Path {
    return remap_path ? Path(remap_path(p.str())) : p;
  };
  auto target_mappable = [&](const Path& p) -> bool {
    if (!remap_path) return true;
    return p.str().empty() || !remap_path(p.str()).empty();
  };
  auto compose_connections = [&](const std::string& prop_name) {
    if (!source.connection(prop_name)) return;
    std::vector<PrimSpec::RelationshipOpinion> opinions;
    if (const auto* stack = target.connection_opinion_stack(prop_name)) {
      opinions = *stack;
    } else if (const std::vector<Path>* items = target.connection(prop_name)) {
      PrimSpec::RelationshipOpinion opinion;
      opinion.items = *items;
      if (const ArcEdit* edit = target.connection_edit(prop_name)) {
        opinion.edit = *edit;
        opinion.qualified = edit->authored && !edit->is_explicit;
      }
      opinions.push_back(std::move(opinion));
    }

    auto remap_opinion = [&](PrimSpec::RelationshipOpinion opinion) {
      std::vector<Path> mapped;
      for (const Path& item : opinion.items) {
        if (target_mappable(item)) mapped.push_back(map_target(item));
      }
      opinion.items = std::move(mapped);
      auto remap_items = [&](std::vector<std::string>* items) {
        if (!remap_path) return;
        std::vector<std::string> remapped;
        for (const std::string& item : *items) {
          const std::string value = remap_path(item);
          if (!value.empty()) remapped.push_back(value);
        }
        *items = std::move(remapped);
      };
      remap_items(&opinion.edit.added);
      remap_items(&opinion.edit.prepended);
      remap_items(&opinion.edit.appended);
      remap_items(&opinion.edit.deleted);
      remap_items(&opinion.edit.ordered);
      opinions.push_back(std::move(opinion));
    };
    if (const auto* stack = source.connection_opinion_stack(prop_name)) {
      for (const auto& opinion : *stack) remap_opinion(opinion);
    } else if (const std::vector<Path>* items = source.connection(prop_name)) {
      PrimSpec::RelationshipOpinion opinion;
      opinion.items = *items;
      if (const ArcEdit* edit = source.connection_edit(prop_name)) {
        opinion.edit = *edit;
        opinion.qualified = edit->authored && !edit->is_explicit;
      }
      remap_opinion(std::move(opinion));
    }

    std::vector<Path> effective;
    for (auto it = opinions.rbegin(); it != opinions.rend(); ++it) {
      if (!it->qualified || it->edit.is_explicit) effective = it->items;
      else ApplyRelationshipEdit(&effective, it->edit);
    }
    target.set_connection_targets(prop_name, std::move(effective));
    target.set_connection_opinion_stack(prop_name, std::move(opinions));
    ArcEdit& resolved = target.ensure_connection_edit(prop_name);
    resolved = ArcEdit();
    resolved.authored = true;
    resolved.is_explicit = true;
  };
  // Copy type name if target doesn't have one
  if (target.type_name().empty() && !source.type_name().empty()) {
    target.set_type_name(source.type_name());
  }

  // Specifier: composed prim existence is the union of opinions — an `over`
  // backed by a `def` from any arc (reference / variant content) flattens to
  // `def`, matching pxr (an over alone defines nothing; the def makes the
  // prim real).
  if (target.specifier() == PrimSpecifier::Over &&
      source.specifier() != PrimSpecifier::Over) {
    // An `over` is undefining. The strongest weaker defining opinion supplies
    // the resolved defining kind: `def` stays concrete, while `class` remains
    // abstract. The old def-only promotion left over+class incorrectly
    // undefining in populated stages.
    target.set_specifier(source.specifier());
  }

  // Copy properties (source overrides target for time-sampled props).
  // Preserves valueless slots (connection-only / declared-only attributes),
  // their declared type names, and connection targets for USDC fidelity.
  // Property ids whose DEFAULT value on the target was filled from THIS
  // source (below): the source's time samples belong with that default and
  // may ride along; any other authored target default came from a STRONGER
  // spec and blocks the source's samples (see the time-sample loop).
  // This is normally empty or only a handful of entries. A tree here caused
  // node allocations during every prim/source merge on large scenes; keep a
  // compact list and use a linear lookup for the rare time-sample check.
  // Most source/spec merges fill no more than a few defaults. Avoid a vector
  // allocation on every merge; spill only unusually property-heavy specs.
  std::array<PropNameId, 8> inline_default_filled{};
  size_t inline_default_filled_count = 0;
  std::vector<PropNameId> overflow_default_filled;
  auto default_was_filled_from_source = [&](PropNameId id) {
    if (std::find(inline_default_filled.begin(),
                  inline_default_filled.begin() +
                      static_cast<ptrdiff_t>(inline_default_filled_count),
                  id) != inline_default_filled.begin() +
                              static_cast<ptrdiff_t>(inline_default_filled_count)) {
      return true;
    }
    return std::find(overflow_default_filled.begin(),
                     overflow_default_filled.end(), id) !=
           overflow_default_filled.end();
  };
  auto record_default_filled = [&](PropNameId id) {
    if (default_was_filled_from_source(id)) return;
    if (inline_default_filled_count < inline_default_filled.size()) {
      inline_default_filled[inline_default_filled_count++] = id;
    } else {
      overflow_default_filled.push_back(id);
    }
  };
  PropNameTable& name_table = GetPropNameTable();
  for (const auto& slot : source.properties().slots()) {
    const std::string pname(name_table.get(slot.name_id));
    const PropSlot* tgt_slot = target.property(slot.name_id);
    if (tgt_slot) {
      const Value* target_value = target.property_value(slot.name_id);
      const Value* source_value = source.property_value(slot.name_id);
      if (target_value && source_value &&
          target_value->type_id() == TypeId::PathExpression &&
          source_value->type_id() == TypeId::PathExpression) {
        const std::string* stronger = target_value->as_string();
        const std::string* weaker = source_value->as_string();
        if (stronger && weaker) {
          const std::string resolved = ResolveWeakerPathExpressionText(
              *stronger, *weaker);
          if (resolved != *stronger) {
            target.set_property_value(slot.name_id,
                Value::MakeStringLike(resolved, TypeId::PathExpression));
          }
        }
      }
      // Sparse array edits (VtArrayEdit) compose BEFORE the fill-absent
      // value logic below: a stronger edit is not an opinion that blocks the
      // weaker default -- it TRANSFORMS it (pxr VtArrayEdit::ComposeOver).
      const ArrayEditData* target_edit = target.array_edit(slot.name_id);
      const ArrayEditData* source_edit = source.array_edit(slot.name_id);
      if (target_edit) {
        if (source_edit) {
          // Edit over edit: concatenate op lists, weaker first. Stays an
          // edit (a still-weaker arc may yet supply the base array).
          ArrayEditData stacked = *source_edit;
          stacked.ops.insert(stacked.ops.end(), target_edit->ops.begin(),
                             target_edit->ops.end());
          target.set_raw_default_source(pname, BuildArrayEditText(stacked));
          target.set_array_edit(pname, std::move(stacked));
        } else if (const Value* weaker_value =
                       source.property_value(slot.name_id)) {
          // Weaker plain array: resolve the edit against it now.
          Value resolved;
          std::string apply_err;
          if (ApplyArrayEdit(*target_edit, weaker_value,
                             static_cast<TypeId>(tgt_slot->value_type),
                             &resolved, &apply_err)) {
            target.clear_array_edit(slot.name_id);
            target.clear_raw_default_source(slot.name_id);
            if (!target.fill_property_value_if_absent(slot.name_id,
                                                      resolved)) {
              target.set_property_value(slot.name_id, std::move(resolved));
            }
            record_default_filled(slot.name_id);
          }
        }
        // Weaker has neither: the edit rides along and resolves later.
      } else if (source_edit && tgt_slot->value_offset == UINT32_MAX &&
                 !target.raw_default_source(slot.name_id)) {
        // Target slot carries no default opinion at all (declared-only /
        // connection-only): the weaker edit is the strongest default seen.
        target.set_array_edit(pname, *source_edit);
        if (const std::string* sraw =
                source.raw_default_source(slot.name_id)) {
          target.set_raw_default_source(pname, *sraw);
        } else {
          target.set_raw_default_source(pname,
                                        BuildArrayEditText(*source_edit));
        }
      }
      // Field-level fill-absent: pxr composes a property's default VALUE and its
      // CONNECTIONS as INDEPENDENT fields. A stronger source may author only one
      // of them; fill the other from this weaker source rather than dropping it.
      // (e.g. a value-only override `metallic = 0` over a base
      // `metallic = 0 (+ .connect)` must keep the connection; and the mirror.)
      if (tgt_slot->value_offset == UINT32_MAX) {
        if (const Value* sv = source.property_value(slot.name_id)) {
          target.fill_property_value_if_absent(slot.name_id, *sv);
        record_default_filled(slot.name_id);
        }
      }
      compose_connections(pname);
      // Property METADATA is likewise per-field: a stronger value-only
      // override must not drop the weaker layer's interpolation / elementSize
      // / customData (render-breaking for primvars). Fill absent fields.
      if (const PropMeta* spm = source.property_meta(slot.name_id)) {
        const PropMeta* tpm = target.property_meta(slot.name_id);
        if (!tpm) {
          target.ensure_property_meta(pname) = *spm;
        } else {
          MergeWeakerPropMeta(&target.ensure_property_meta(pname), *spm);
        }
      }
      // Variability (uniform) fills from the weaker source too.
      if ((slot.flags & PropSlot::kFlagUniform) &&
          !(tgt_slot->flags & PropSlot::kFlagUniform)) {
        if (PropSlot* ms = target.property_mutable(slot.name_id)) {
          ms->flags |= PropSlot::kFlagUniform;
        }
      }
      // AOUSD §12.2: custom resolves true if any contributing opinion is true.
      if ((slot.flags & PropSlot::kFlagCustom) &&
          !(tgt_slot->flags & PropSlot::kFlagCustom)) {
        if (PropSlot* ms = target.property_mutable(slot.name_id)) {
          ms->flags |= PropSlot::kFlagCustom;
        }
      }
      if (!target.spline_source(slot.name_id)) {
        if (const std::string* spline = source.spline_source(slot.name_id)) {
          target.set_spline_source(pname, *spline);
        }
      }
      if (!target.raw_default_source(slot.name_id) && !source_edit) {
        if (const std::string* raw = source.raw_default_source(slot.name_id)) {
          target.set_raw_default_source(pname, *raw);
        }
      }
      continue;  // target opinion otherwise wins (incl. time-sampled merge)
    }
    // Mirror of the rel-vs-attr form conflict below: a weaker ATTRIBUTE under
    // an existing stronger relationship of the same name is ignored (pxr
    // keeps the defining spec's form and drops the conflicting spec).
    if (!slot.is_relationship() &&
        (target.relationship(pname) ||
         target.relationship_opinion_stack(pname))) {
      continue;
    }
    const Value* src_val = source.property_value(slot.name_id);
    if (src_val) {
      target.add_property(slot.name_id, *src_val, slot.flags);
      record_default_filled(slot.name_id);
    } else {
      // No authored default: carry the typed slot across (connection-only /
      // declared-only attribute).
      target.add_property_slot(slot.name_id,
                               static_cast<TypeId>(slot.value_type), slot.flags);
    }
    if (const std::string* tn = source.property_type_name(pname)) {
      target.set_property_type_name(pname, *tn);
    }
    compose_connections(pname);
    if (const PropMeta* pm = source.property_meta(slot.name_id)) {
      target.ensure_property_meta(pname) = *pm;
    }
    if (const std::string* spline = source.spline_source(slot.name_id)) {
      target.set_spline_source(pname, *spline);
    }
    if (const std::string* raw = source.raw_default_source(slot.name_id)) {
      target.set_raw_default_source(pname, *raw);
    }
    if (const ArrayEditData* new_edit = source.array_edit(slot.name_id)) {
      target.set_array_edit(pname, *new_edit);
    }
  }

  // Relationship target list ops compose over the weaker target list. A
  // stronger explicit list blocks weaker targets; a stronger qualified edit
  // is applied to the weaker effective list (the old skip-on-existing path
  // lost `</A>` from weak A + strong prepend B).
  for (const auto& rel_name : source.relationship_names()) {
    // pxr: property specs whose FORM conflicts with the defining (strongest)
    // spec are ignored. A weaker RELATIONSHIP under an existing attribute
    // slot of the same name contributes no targets — but a relationship
    // spec's intrinsic `uniform` variability is an authored field and still
    // fills the composed attribute (usdcat prints `uniform double x`).
    {
      const PropNameId aid = GetPropNameTable().find(rel_name);
      if (aid.is_valid()) {
        const PropSlot* aslot = target.property(aid);
        if (aslot && !aslot->is_relationship()) {
          if (PropSlot* ms = target.property_mutable(aid)) {
            ms->flags |= PropSlot::kFlagUniform;
          }
          continue;
        }
      }
    }
    std::vector<PrimSpec::RelationshipOpinion> opinions;
    if (const auto* existing = target.relationship_opinion_stack(rel_name)) {
      opinions = *existing;
    } else if (const std::vector<Path>* items = target.relationship(rel_name)) {
      PrimSpec::RelationshipOpinion opinion;
      opinion.items = *items;
      const auto& edits = target.relationship_edits();
      auto it = edits.find(rel_name);
      if (it != edits.end() && it->second.authored) {
        opinion.edit = it->second;
        opinion.qualified = !it->second.is_explicit;
      }
      // A DECLARED-ONLY relationship (no targets, no authored edit) carries
      // no target opinion — pushing an empty explicit one here BLOCKED
      // weaker list-edited targets (BasicListEditing).
      if (!opinion.items.empty() || opinion.edit.authored) {
        opinions.push_back(std::move(opinion));
      }
    }

    // Track opinions whose every target was DROPPED as unmappable across the
    // arc: pxr keeps the relationship SPEC but no target opinion (a bare
    // `rel name` on flatten), NOT an authored-explicit empty list (`= None`).
    size_t src_opinions = 0;
    size_t src_vacated = 0;
    const size_t prior_opinions = opinions.size();
    auto remap_opinion = [&](PrimSpec::RelationshipOpinion opinion) {
      ++src_opinions;
      const bool had_items = !opinion.items.empty() ||
                             opinion.edit.has_authored_opinion();
      std::vector<Path> mapped_items;
      for (const Path& item : opinion.items) {
        if (target_mappable(item)) mapped_items.push_back(map_target(item));
      }
      opinion.items = std::move(mapped_items);
      auto remap_edit_items = [&](std::vector<std::string>* items) {
        if (!remap_path) return;
        std::vector<std::string> mapped;
        for (const std::string& item : *items) {
          std::string value = remap_path(item);
          if (!value.empty()) mapped.push_back(std::move(value));
        }
        *items = std::move(mapped);
      };
      remap_edit_items(&opinion.edit.prepended);
      remap_edit_items(&opinion.edit.added);
      remap_edit_items(&opinion.edit.appended);
      remap_edit_items(&opinion.edit.deleted);
      remap_edit_items(&opinion.edit.ordered);
      if (had_items && opinion.items.empty() &&
          !opinion.edit.has_authored_opinion()) {
        ++src_vacated;
      }
      opinions.push_back(std::move(opinion));
    };

    if (const auto* source_stack =
            source.relationship_opinion_stack(rel_name)) {
      for (const auto& opinion : *source_stack) remap_opinion(opinion);
    } else if (const std::vector<Path>* items =
                   source.relationship(rel_name)) {
      PrimSpec::RelationshipOpinion opinion;
      opinion.items = *items;
      const auto& edits = source.relationship_edits();
      auto it = edits.find(rel_name);
      if (it != edits.end() && it->second.authored) {
        opinion.edit = it->second;
        opinion.qualified = !it->second.is_explicit;
      }
      // Declared-only source rel: no target opinion (see above).
      if (!opinion.items.empty() || opinion.edit.authored) {
        remap_opinion(std::move(opinion));
      }
    }

    std::vector<Path> effective;
    for (auto it = opinions.rbegin(); it != opinions.rend(); ++it) {
      if (!it->qualified || it->edit.is_explicit) {
        effective = it->items;
      } else {
        ApplyRelationshipEdit(&effective, it->edit);
      }
    }
    // Every contributed opinion lost all its targets to arc mapping and there
    // was no prior opinion: the composed relationship exists but carries NO
    // target opinion (bare `rel name`, pxr parity) — an authored-explicit
    // empty list here would wrongly serialize as `= None`.
    const bool vacated = prior_opinions == 0 && src_opinions > 0 &&
                         src_vacated == src_opinions && effective.empty();
    target.set_relationship_targets(rel_name, std::move(effective));
    target.set_relationship_opinion_stack(rel_name, std::move(opinions));
    ArcEdit& resolved = target.ensure_relationship_edit(rel_name);
    resolved = ArcEdit();
    resolved.authored = !vacated;
    resolved.is_explicit = !vacated;
    target.set_relationship_flags(
        rel_name, static_cast<uint16_t>(target.relationship_flags(rel_name) |
                                        source.relationship_flags(rel_name)));
    if (const PropMeta* pm = source.property_meta(rel_name)) {
      const PropMeta* current = target.property_meta(rel_name);
      if (!current) {
        target.ensure_property_meta(rel_name) = *pm;
      } else {
        MergeWeakerPropMeta(&target.ensure_property_meta(rel_name), *pm);
      }
    }
  }

  // Copy time-sampled properties
  for (auto ts_prop_id : source.time_sampled_properties()) {
    auto* samples = source.time_samples(ts_prop_id);
    if (!samples) continue;

    // Check if target already has time samples for this property. A stronger
    // authored DEFAULT also blocks weaker samples: pxr resolves an attribute
    // from the strongest spec with any value opinion, so samples from a weaker
    // layer must not ride along under a stronger default (they would win at
    // evaluation time — samples beat defaults only WITHIN one spec).
    bool target_has_ts = target.has_time_samples(ts_prop_id);
    if (!target_has_ts) {
      if (const PropSlot* tslot = target.property(ts_prop_id)) {
        // The target's authored default blocks this source's samples UNLESS
        // that default was filled from this very source (then they are one
        // spec's opinion and ride together). Authored-ness is what matters:
        // comparing VALUES let a weaker spec's samples through whenever its
        // default happened to EQUAL the stronger spec's (false positive).
        if (tslot->value_offset != UINT32_MAX &&
            !default_was_filled_from_source(ts_prop_id)) {
          continue;
        }
      }
      // Copy time samples from source to target, remapping the sample time by
      // the layer offset (t -> time_offset + time_scale*t).
      //
      // A DEGENERATE offset (scale == 0) maps every sample onto `time_offset`.
      // It is reachable: the tcps auto-scale divides parent/sublayer rates, so
      // a root authoring `timeCodesPerSecond = 0` scales its sublayers by 0.
      // add_time_sample overwrites an equal key ("last opinion wins"), so
      // copying them all would freeze the layer at its LAST sample. pxr freezes
      // it at the FIRST instead (its inverse offset is non-finite, so the
      // layer-time lookup lands on the earliest sample), verified against the
      // 26.05 oracle. Samples are stored ascending, so stop after the first.
      const bool collapsed = (time_scale == 0.0);
      for (const auto& [time, val_offset] : *samples) {
        const Value* val = source.time_sample_value(val_offset);
        if (val) {
          target.add_time_sample(ts_prop_id, time_offset + time_scale * time,
                                 *val);
          if (collapsed) break;
        }
      }
    }
  }

  // Copy metadata fields (fill-absent: a stronger/earlier opinion already on the
  // target wins; weaker opinions only fill gaps).
  if ((source.meta().doc_authored() || !source.meta().doc().empty()) &&
      !target.meta().doc_authored() && target.meta().doc().empty()) {
    target.meta().doc() = source.meta().doc();
    target.meta().set_doc_authored();
  }
  // active/hidden: fill-absent on AUTHORED opinions only. The previous
  // "copy on difference" let a weaker spec's DEFAULT (active = true) clobber
  // a stronger authored `active = false` — the inverse of LIVRPS (pxr prunes
  // such prims; we un-deactivated them).
  if (source.meta().active_authored && !target.meta().active_authored) {
    target.meta().active = source.meta().active;
    target.meta().active_authored = true;
  }
  if (source.meta().hidden_authored && !target.meta().hidden_authored) {
    target.meta().hidden = source.meta().hidden;
    target.meta().hidden_authored = true;
  }
  if ((source.meta().instanceable || source.meta().instanceable_authored) &&
      !target.meta().instanceable_authored && !target.meta().instanceable) {
    target.meta().instanceable = source.meta().instanceable;
    target.meta().instanceable_authored =
        source.meta().instanceable_authored || source.meta().instanceable;
  }
  if ((source.meta().comment_authored() ||
       !source.meta().comment().empty()) &&
      !target.meta().comment_authored() && target.meta().comment().empty()) {
    target.meta().comment() = source.meta().comment();
    target.meta().set_comment_authored();
  }
  if ((source.meta().kindAuthored() || !source.meta().kind().empty()) &&
      !target.meta().kindAuthored() && target.meta().kind().empty()) {
    target.meta().kind() = source.meta().kind();
    target.meta().setKindAuthored();
  }
  if (!source.meta().permission().empty() &&
      target.meta().permission().empty()) {
    target.meta().permission() = source.meta().permission();
  }
  if ((source.meta().displayNameAuthored() ||
       !source.meta().displayName().empty()) &&
      !target.meta().displayNameAuthored() &&
      target.meta().displayName().empty()) {
    target.meta().displayName() = source.meta().displayName();
    target.meta().setDisplayNameAuthored();
  }
  if (!source.meta().unknownMeta().empty()) {
    MergeWeakerRawFields(&target.meta().unknownMeta(),
                         source.meta().unknownMeta());
  }
  if (!target.meta().primOrderAuthored() &&
      (source.meta().primOrderAuthored() ||
       !source.meta().primOrder().empty())) {
    target.meta().editPrimOrder() = source.meta().primOrder();
    target.meta().setPrimOrderAuthored();
  }
  if (!target.meta().propertyOrderAuthored() &&
      (source.meta().propertyOrderAuthored() ||
       !source.meta().propertyOrder().empty())) {
    target.meta().editPropertyOrder() = source.meta().propertyOrder();
    target.meta().setPropertyOrderAuthored();
  }
  if (!target.meta().displayGroupOrderAuthored() &&
      (source.meta().displayGroupOrderAuthored() ||
       !source.meta().displayGroupOrder().empty())) {
    target.meta().editDisplayGroupOrder() = source.meta().displayGroupOrder();
    target.meta().setDisplayGroupOrderAuthored();
  }
  // Registered string list-op metadata fields (apiSchemas / variantSetNames /
  // clipSets): one shared merge — a stronger authored op resolves over the
  // weaker effective name list and is stored explicitly (flatten semantics);
  // an absent stronger opinion fill-copies the weaker authored op. Per-field
  // storage nuances (apiSchemas' applied vector + legacy qualifier) live in
  // the table's normalization/post hooks (listop-field-table.hh).
  {
    size_t field_count = 0;
    const StringListOpFieldDef* fields = GetStringListOpFieldTable(&field_count);
    for (size_t i = 0; i < field_count; ++i) {
      MergeWeakerStringListOpField(fields[i], source.meta(), target.meta());
    }
  }
  // Dictionary-valued metadata (fill-absent). `ct` binds a const view so the
  // gap check never allocates the target's metadata ext.
  {
    const PrimSpecMeta& cs = source.meta();
    const PrimSpecMeta& ct = target.meta();
    if (cs.customData().is_dictionary()) {
      if (!ct.customData().is_dictionary())
        target.meta().customData() = cs.customData();
      else MergeWeakerDictionary(&target.meta().customData(), cs.customData(),
                                 dict_conflicts, "customData.");
      if (cs.customDataAuthored()) target.meta().setCustomDataAuthored();
    }
    if (cs.assetInfo().is_dictionary()) {
      if (!ct.assetInfo().is_dictionary())
        target.meta().assetInfo() = cs.assetInfo();
      else MergeWeakerDictionary(&target.meta().assetInfo(), cs.assetInfo(),
                                 dict_conflicts, "assetInfo.");
      if (cs.assetInfoAuthored()) target.meta().setAssetInfoAuthored();
    }
    if (cs.sdrMetadata().is_dictionary()) {
      if (!ct.sdrMetadata().is_dictionary())
        target.meta().sdrMetadata() = cs.sdrMetadata();
      else MergeWeakerDictionary(&target.meta().sdrMetadata(), cs.sdrMetadata(),
                                 dict_conflicts, "sdrMetadata.");
      if (cs.sdrMetadataAuthored()) target.meta().setSdrMetadataAuthored();
    }
    if (cs.clips().is_dictionary()) {
      if (!ct.clips().is_dictionary())
        target.meta().clips() = cs.clips();
      else MergeWeakerDictionary(&target.meta().clips(), cs.clips(),
                                 dict_conflicts, "clips.");
      if (cs.clipsAuthored()) target.meta().setClipsAuthored();
    }
    if (!cs.unknownFields().empty()) {
      MergeWeakerExtensionFields(&target.meta().unknownFields(),
                                 cs.unknownFields());
    }

    // clipSets (a separate SdfStringListOp controlling strength among the
    // dictionaries above) is merged by the registered string list-op field
    // loop earlier in this function.
  }

  // Variant sets + selections ride along from the weaker `source`, MERGED
  // per-set / per-variant (not all-or-nothing). The target (stronger) keeps its
  // own opinions; the source fills in what is absent. This matters because a
  // stronger layer may DECLARE a variantSet with empty variant blocks while the
  // actual variant CONTENT is authored in a weaker layer reached via
  // payload/reference (e.g. Pixar Kitchen_set: Chair.usd declares empty
  // ChairA/ChairB blocks, the geometry lives in Chair.geom.usd two arcs deeper).
  // An all-or-nothing copy would drop that content because the target "already
  // has" the (empty) set. The selection itself is left to the referencing prim
  // (ApplyVariants runs after arc resolution and is strongest).
  // (variantSetNames list-op edits merge in the registered string list-op
  // field loop earlier in this function.)
  if (!source.meta().variantSets().empty()) {
    std::vector<VariantSetData>& tsets = target.meta().variantSets();
    // The host (target) may express its selection as the legacy variantSelection
    // STRING rather than a per-set `selected`. The crate reader sets both, but a
    // host carrying only the string must still win over a referenced asset's
    // (weaker) per-set `selected`, which would otherwise ride along this copy and
    // take precedence in ApplyVariants. Suppress the source `selected` for the
    // set the host's string already selects (ApplyVariants then falls back to the
    // host's string).
    const VariantSelection target_sel =
        ParseVariantSelection(target.meta().variantSelection);
    for (const auto& ssvs : source.meta().variantSets()) {
      const bool host_string_selects =
          !target_sel.variant_set.empty() && ssvs.name == target_sel.variant_set;
      VariantSetData* tvs = nullptr;
      for (auto& t : tsets) {
        if (t.name == ssvs.name) { tvs = &t; break; }
      }
      if (!tvs) {
        tsets.push_back(ssvs);  // whole set is new to the target
        if (host_string_selects) tsets.back().selected.clear();  // host wins
        continue;
      }
      if (tvs->selected.empty() && !host_string_selects) {
        tvs->selected = ssvs.selected;
      }
      for (const auto& svar : ssvs.variants) {
        VariantData* tvar = nullptr;
        for (auto& v : tvs->variants) {
          if (v.name == svar.name) { tvar = &v; break; }
        }
        if (!tvar) {
          tvs->variants.push_back(svar);  // variant option new to the target
          continue;
        }
        // Shared variant option: fill-absent its content.
        for (const auto& sp : svar.properties) {
          bool have = false;
          for (const auto& tp : tvar->properties) {
            if (tp.name == sp.name) { have = true; break; }
          }
          if (!have) tvar->properties.push_back(sp);
        }
        for (const auto& sr : svar.relationships) {
          if (!tvar->relationships.count(sr.first)) {
            tvar->relationships[sr.first] = sr.second;
            auto fit = svar.relationshipFlags.find(sr.first);
            if (fit != svar.relationshipFlags.end()) {
              tvar->relationshipFlags[sr.first] = fit->second;
            }
          }
        }
        if (!tvar->content && svar.content) tvar->content = svar.content;
        if (tvar->doc.empty() && !svar.doc.empty()) tvar->doc = svar.doc;
        if (tvar->variantSets.empty() && !svar.variantSets.empty()) {
          tvar->variantSets = svar.variantSets;
        }
      }
    }
  }
  if (target.meta().variantSelection.empty() &&
      !source.meta().variantSelection.empty()) {
    target.meta().variantSelection = source.meta().variantSelection;
  }
  if (!source.meta().variantSelections().empty() &&
      target.meta().variantSelections().empty()) {
    target.meta().variantSelections() = source.meta().variantSelections();
  }
}

void Compositor::CollectArcDeletes(const Layer& layer) {
  for (const PrimSpec& prim : layer.prims()) {
    const ArcListOpEdits* e = prim.meta().arc_edits();
    if (!e) continue;
    auto take = [&](const ArcEdit& a) {
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
      prim.meta().variantSetNameEdits() = StringListOpEdits();
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

// Apply one SELECTED variant option's opinions onto `prim` (fill-absent):
// inline properties/relationships/doc/state, the content sub-layer (its
// "__self__" root merges onto the prim; descendants graft as children),
// composition arcs authored on the option, and nested variant sets
// (recursive, selection from the nested set's own `selected`).
void Compositor::ApplyOneVariant(PrimSpec& prim, const Layer& layer,
                                 const std::string& anchor_path, int depth,
                                 const VariantData& variant) {
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
  if (!variant.doc.empty() && prim.meta().doc().empty()) {
    prim.meta().doc() = variant.doc;
  }
  if (!variant.kind.empty() && !prim.meta().kindAuthored() &&
      prim.meta().kind().empty()) {
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
  set_names.reserve(prim.meta().variantSets().size());
  for (const auto& vs : prim.meta().variantSets()) set_names.push_back(vs.name);

  for (const std::string& set_name : set_names) {
    // Re-look up the set each iteration; a prior iteration may have
    // reallocated the vector.
    auto find_set = [&prim](const std::string& n) -> const VariantSetData* {
      for (const auto& s : prim.meta().variantSets()) {
        if (s.name == n) return &s;
      }
      return nullptr;
    };
    const VariantSetData* vs_p = find_set(set_name);
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
    if (chosen.empty()) continue;

    // Copy the selected option out before applying it: ApplyOneVariant mutates
    // `prim`, which owns the vector `variant` would otherwise point into (and
    // the whole recursion below reads through that reference). VariantData
    // copies are shallow in practice — Values are COW and `content` is a
    // shared_ptr<Layer>.
    bool have_variant = false;
    VariantData selected_variant;
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
  std::string str = ref_str;

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
      meta.variantSetNameEdits() = StringListOpEdits();
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

}  // namespace next
}  // namespace lightusd
