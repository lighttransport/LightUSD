// SPDX-License-Identifier: Apache-2.0
// Opinion-merge pass (ComposeLayer / ComposePrim / CopyLocalOpinions) of
// Compositor, extracted from composition.cc for compile-time parallelism.

#include "composition.hh"
#include "composition-internal.hh"
#include "../layer/array-edit.hh"
#include "../layer/arc-reference.hh"
#include "../layer/listop-field-table.hh"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

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
  auto deleted_by = [](const ArcListOp* e, const std::string& a) {
    return e && e->authored &&
           std::find(e->deleted.begin(), e->deleted.end(), a) != e->deleted.end();
  };
  auto merge_arcs = [&](std::vector<std::string>& dst,
                        const std::vector<std::string>& src,
                        const ArcListOp* tgt_edit) {
    for (const std::string& a : src) {
      if (deleted_by(tgt_edit, a)) continue;
      if (std::find(dst.begin(), dst.end(), a) == dst.end()) {
        dst.push_back(a);
      }
    }
  };
  const ArcListOps* te = target.meta().arc_edits();
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
  if (const ArcListOps* se = source.meta().arc_edits()) {
    auto merge_deleted = [&](const ArcListOp& from, ArcListOp& into) {
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
    ArcListOps& tedits = target.meta().ensure_arc_edits();
    merge_deleted(se->references, tedits.references);
    merge_deleted(se->payloads, tedits.payloads);
    merge_deleted(se->inherits, tedits.inherits);
    merge_deleted(se->specializes, tedits.specializes);
  }
  // Variant sets/selections: fill-absent by set name. (variantSetNames
  // list-op edits are merged inside CopyLocalOpinions' registered string
  // list-op field loop.)
  if (!source.meta().variantSets().empty()) {
    for (const PrimVariantSetData& svs : source.meta().variantSets()) {
      bool have = false;
      for (const PrimVariantSetData& tvs : target.meta().variantSets()) {
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
  // PrimMetadata's mutable cold-field accessors allocate. Keep a const view
  // for all gap tests so metadata-free prims stay allocation-free.
  const PrimMetadata& target_meta = target.meta();
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
  // Id-keyed throughout: the string form of a property name is never built
  // on this path (a std::string per property per source was a measurable
  // allocation on 60M-property stages).
  auto compose_connections = [&](PropNameId prop_name) {
    if (!source.connection(prop_name)) return;
    std::vector<PrimSpec::RelationshipOpinion> opinions;
    if (const auto* stack = target.connection_opinion_stack(prop_name)) {
      opinions = *stack;
    } else if (const std::vector<Path>* items = target.connection(prop_name)) {
      PrimSpec::RelationshipOpinion opinion;
      opinion.items = *items;
      if (const ArcListOp* edit = target.connection_edit(prop_name)) {
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
      if (const ArcListOp* edit = source.connection_edit(prop_name)) {
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
    ArcListOp& resolved = target.ensure_connection_edit(prop_name);
    resolved = ArcListOp();
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
  const bool target_has_rels = target.has_relationship_data();
  for (const auto& slot : source.properties().slots()) {
    const PropNameId pname = slot.name_id;
    const PropSlot* tgt_slot = target.property(slot.name_id);
    if (tgt_slot) {
      const Value* target_value = target.property_value(slot.name_id);
      const Value* source_value = source.property_value(slot.name_id);
      if (target_value && source_value &&
          target_value->type_id() == TypeId::PathExpression &&
          source_value->type_id() == TypeId::PathExpression) {
        auto stronger = target_value->as_string();
        auto weaker = source_value->as_string();
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
    if (!slot.is_relationship() && target_has_rels) {
      const std::string pname_str(name_table.get(slot.name_id));
      if (target.relationship(pname_str) ||
          target.relationship_opinion_stack(pname_str)) {
        continue;
      }
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
    if (slot.type_name_id != PropSlot::kNoTypeName) {
      target.set_property_type_name(slot.name_id,
                                    TypeNameId{slot.type_name_id});
    } else if (const std::string* tn = source.property_type_name(slot.name_id)) {
      target.set_property_type_name(slot.name_id, *tn);
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
  // (relationship_names() allocates and sorts; most prims author none.)
  const std::vector<std::string> source_rel_names =
      source.has_relationship_data() ? source.relationship_names()
                                     : std::vector<std::string>();
  for (const auto& rel_name : source_rel_names) {
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
    ArcListOp& resolved = target.ensure_relationship_edit(rel_name);
    resolved = ArcListOp();
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
      !target_meta.doc_authored() && target_meta.doc().empty()) {
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
      !target_meta.comment_authored() && target_meta.comment().empty()) {
    target.meta().comment() = source.meta().comment();
    target.meta().set_comment_authored();
  }
  if ((source.meta().kindAuthored() || !source.meta().kind().empty()) &&
      !target_meta.kindAuthored() && target_meta.kind().empty()) {
    target.meta().kind() = source.meta().kind();
    target.meta().setKindAuthored();
  }
  if (!source.meta().permission().empty() &&
      target_meta.permission().empty()) {
    target.meta().permission() = source.meta().permission();
  }
  if ((source.meta().displayNameAuthored() ||
      !source.meta().displayName().empty()) &&
      !target_meta.displayNameAuthored() &&
      target_meta.displayName().empty()) {
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
    const PrimMetadata& cs = source.meta();
    const PrimMetadata& ct = target.meta();
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
    std::vector<PrimVariantSetData>& tsets = target.meta().variantSets();
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
      PrimVariantSetData* tvs = nullptr;
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
        PrimVariantData* tvar = nullptr;
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


}  // namespace next
}  // namespace lightusd
