// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Variant holder materialization for the crate writer (see header).

#include "variant-holders.hh"

#include <string>
#include <vector>

namespace lightusd {
namespace next {

namespace {

std::string HolderPath(const std::string& owner, const std::string& set,
                       const std::string& variant) {
  return owner + "/{" + set + "=" + variant + "}";
}

bool IsBracketedName(const std::string& name) {
  return name.size() >= 2 && name.front() == '{' && name.back() == '}';
}

// Holder paths embed set/variant names between '{', '=', '}' — names
// containing those separators (or '/') would corrupt the bracket parsing on
// read-back. Parser-produced names cannot contain them; guard API-authored
// ones.
bool IsValidVariantIdentifier(const std::string& name, bool allow_empty) {
  if (name.empty()) return allow_empty;
  for (char c : name) {
    if (c == '{' || c == '}' || c == '=' || c == '/') return false;
  }
  return true;
}

// Copy the parts of `src` a variant content root ("__self__") contributes to
// its holder prim: typed properties (defaults + declared type names +
// per-property metadata + time samples), relationships, connections, and the
// commonly-authored prim metadata.
void MergeContentRootInto(const PrimSpec& src, PrimSpec* dst) {
  PropNameTable& names = GetPropNameTable();
  for (const PropSlot& slot : src.properties().slots()) {
    if (slot.is_relationship()) continue;
    const std::string pname(names.get(slot.name_id));
    const Value* v = src.property_value(slot.name_id);
    if (v) {
      dst->add_property(pname, *v, slot.flags);
    } else {
      dst->add_property_slot(slot.name_id,
                             static_cast<TypeId>(slot.value_type), slot.flags);
    }
    if (const std::string* tn = src.property_type_name(pname)) {
      dst->set_property_type_name(pname, *tn);
    }
    if (const PropMeta* pm = src.property_meta(slot.name_id)) {
      dst->ensure_property_meta(slot.name_id) = *pm;
    }
    if (const std::vector<Path>* conns = src.connection(pname)) {
      for (const Path& t : *conns) dst->add_connection(pname, t);
    }
  }
  // Time samples authored on the variant's own prim.
  for (PropNameId ts_id : src.time_sampled_properties()) {
    const auto* samples = src.time_samples(ts_id);
    if (!samples) continue;
    for (const auto& tv : *samples) {
      if (const Value* v = src.time_sample_value(tv.second)) {
        dst->add_time_sample(ts_id, tv.first, *v);
      }
    }
    dst->mark_property_time_sampled(ts_id);
    if (!dst->property(ts_id)) {
      dst->add_property_slot(ts_id, TypeId::Invalid,
                             PropSlot::kFlagTimeSampled);
    }
  }
  for (const std::string& rel : src.relationship_names()) {
    if (const std::vector<Path>* targets = src.relationship(rel)) {
      dst->set_relationship_targets(rel, *targets);
    }
  }
  if (!src.type_name().empty() && dst->type_name().empty()) {
    dst->set_type_name(src.type_name());
  }
  // Prim metadata the option's "__self__" root may carry.
  const PrimSpecMeta& sm = src.meta();
  if (!sm.kind().empty()) dst->meta().kind() = sm.kind();
  if (!sm.apiSchemas().empty()) dst->meta().apiSchemas() = sm.apiSchemas();
  if (sm.customData().is_dictionary()) {
    dst->meta().customData() = sm.customData();
  }
}

// Build one "{set=variant}" holder prim from inline VariantData and append it
// (plus any content subtree prims) to `out`. Returns the holder's prim index.
uint32_t AppendVariantHolder(Layer* out, const std::string& owner_path,
                             const std::string& set_name,
                             const VariantData& vd) {
  const std::string hp = HolderPath(owner_path, set_name, vd.name);
  PrimSpec holder("{" + set_name + "=" + vd.name + "}");
  holder.set_path(Path(hp));
  // A variant option spec never DEFINES its host (pxr option specs are
  // `over`); the PrimSpec default of Def would wrongly promote an
  // over-authored host prim to def during specifier composition.
  holder.set_specifier(PrimSpecifier::Over);

  // Inline option opinions.
  for (const VariantProperty& vp : vd.properties) {
    holder.add_property(vp.name, vp.value, vp.flags);
  }
  for (const auto& rel : vd.relationships) {
    holder.set_relationship_targets(rel.first, rel.second);
    auto fit = vd.relationshipFlags.find(rel.first);
    if (fit != vd.relationshipFlags.end()) {
      holder.set_relationship_flags(rel.first, fit->second);
    }
  }
  holder.meta().active = vd.active;
  holder.meta().hidden = vd.hidden;
  if (!vd.active || vd.active_authored) holder.meta().active_authored = true;
  if (vd.hidden || vd.hidden_authored) holder.meta().hidden_authored = true;
  if (!vd.doc.empty()) holder.meta().doc() = vd.doc;
  if (!vd.kind.empty()) {
    holder.meta().kind() = vd.kind;
    holder.meta().setKindAuthored();
  }
  holder.meta().references = vd.references;
  holder.meta().payloads = vd.payloads;
  holder.meta().inherits = vd.inherits;
  holder.meta().specializes = vd.specializes;
  if (!vd.variantSelections.empty()) {
    holder.meta().variantSelections() = vd.variantSelections;
  }
  // Unknown variant-option metadata rides on the holder prim spec: the crate
  // prim-spec field writer emits unknownMeta/unknownFields for every spec,
  // so preserving them here makes variant extension fields lossless in USDC.
  if (!vd.unknownMeta.empty()) holder.meta().unknownMeta() = vd.unknownMeta;
  if (!vd.unknownFields.empty()) {
    holder.meta().unknownFields() = vd.unknownFields;
  }
  // Nested variant sets: keep them inline on the holder; the caller
  // materializes them recursively (the holder becomes an owning prim).
  if (!vd.variantSets.empty()) {
    holder.meta().variantSets() = vd.variantSets;
  }

  // Content subtree: the content layer's "/__self__" root carries opinions
  // for the holder itself; its descendants become the holder's sub-prims.
  uint32_t holder_idx = UINT32_MAX;
  if (vd.content) {
    const Layer& content = *vd.content;
    for (size_t i = 0; i < content.prim_count(); ++i) {
      const PrimSpec* cp = content.prim(static_cast<uint32_t>(i));
      if (!cp) continue;
      const std::string& cpath = cp->path().str();
      if (cpath == "/__self__") {
        MergeContentRootInto(*cp, &holder);
        continue;
      }
      static const char kSelfPrefix[] = "/__self__/";
      if (cpath.compare(0, sizeof(kSelfPrefix) - 1, kSelfPrefix) != 0) {
        continue;  // unexpected layout; skip defensively
      }
      PrimSpec child = cp->Clone();
      child.set_path(Path(hp + "/" + cpath.substr(sizeof(kSelfPrefix) - 1)));
      // Child links reference the content layer and are meaningless in the
      // output layer; the crate writer and reader are path-based.
      child.clear_child_indices();
      out->add_prim(std::move(child));
    }
  }

  holder.clear_child_indices();
  holder_idx = out->add_prim(std::move(holder));
  return holder_idx;
}

bool PrimNeedsHolders(const Layer& layer, const PrimSpec& prim) {
  if (IsBracketedName(prim.name())) return false;
  for (const VariantSetData& vs : prim.meta().variantSets()) {
    for (const VariantData& vd : vs.variants) {
      if (!layer.prim_at_path(HolderPath(prim.path().str(), vs.name,
                                         vd.name))) {
        return true;
      }
      // Holder exists but the inline VariantData carries opinions of its own
      // (MergeVariantData pattern): materialization must run so those merge
      // into the existing holder rather than being dropped.
      if (!vd.properties.empty() || !vd.relationships.empty() || vd.content) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

bool LayerNeedsVariantHolders(const Layer& layer) {
  for (const PrimSpec& prim : layer.prims()) {
    if (PrimNeedsHolders(layer, prim)) return true;
  }
  return false;
}

Layer MaterializeVariantHolders(const Layer& layer) {
  Layer out = layer.Clone();

  // Process a growing worklist: holders appended for nested variant sets are
  // owners themselves and get their own holders in later iterations.
  for (size_t i = 0; i < out.prim_count(); ++i) {
    // Re-fetch each round: add_prim may reallocate the prim vector.
    const std::string owner_path = out.prim(static_cast<uint32_t>(i))
                                       ->path()
                                       .str();
    const std::vector<VariantSetData> sets =
        out.prim(static_cast<uint32_t>(i))->meta().variantSets();
    if (sets.empty()) continue;

    for (const VariantSetData& vs : sets) {
      // "{set=}" VariantSet declaration spec (pxr enumerates variantChildren
      // from it). Create it when any option holder is being synthesized.
      const std::string decl_path = HolderPath(owner_path, vs.name, "");
      bool needs_decl = false;

      if (!IsValidVariantIdentifier(vs.name, /*allow_empty=*/false)) {
        continue;  // would corrupt bracket path parsing; refuse to embed
      }
      for (const VariantData& vd : vs.variants) {
        if (!IsValidVariantIdentifier(vd.name, /*allow_empty=*/false)) {
          continue;
        }
        const std::string hp = HolderPath(owner_path, vs.name, vd.name);
        if (PrimSpec* existing = out.prim_at_path_mutable(hp)) {
          // Holder already present (crate-read layer). Inline VariantData may
          // still carry opinions the holder lacks (MergeVariantData pattern:
          // declared-empty holder in a stronger layer + content merged in
          // from a weaker one) — upsert those instead of dropping them.
          for (const VariantProperty& vp : vd.properties) {
            if (!existing->property(vp.name)) {
              existing->add_property(vp.name, vp.value, vp.flags);
            }
          }
          for (const auto& rel : vd.relationships) {
            if (!existing->relationship(rel.first)) {
              existing->set_relationship_targets(rel.first, rel.second);
              auto fit = vd.relationshipFlags.find(rel.first);
              if (fit != vd.relationshipFlags.end()) {
                existing->set_relationship_flags(rel.first, fit->second);
              }
            }
          }
          if (vd.content) {
            const Layer& content = *vd.content;
            static const char kSelfPrefix[] = "/__self__/";
            bool appended = false;
            for (size_t ci = 0; ci < content.prim_count(); ++ci) {
              const PrimSpec* cp = content.prim(static_cast<uint32_t>(ci));
              if (!cp) continue;
              const std::string& cpath = cp->path().str();
              if (cpath.compare(0, sizeof(kSelfPrefix) - 1, kSelfPrefix) !=
                  0) {
                continue;
              }
              const std::string np =
                  hp + "/" + cpath.substr(sizeof(kSelfPrefix) - 1);
              if (out.prim_at_path(np)) continue;
              PrimSpec child = cp->Clone();
              child.set_path(Path(np));
              child.clear_child_indices();
              out.add_prim(std::move(child));
              appended = true;
            }
            if (appended) out.build_path_index();
          }
          continue;
        }
        AppendVariantHolder(&out, owner_path, vs.name, vd);
        out.build_path_index();  // keep lookups current for nested/self refs
        needs_decl = true;
      }

      if (needs_decl && !out.prim_at_path(decl_path)) {
        PrimSpec decl("{" + vs.name + "=}");
        decl.set_path(Path(decl_path));
        out.add_prim(std::move(decl));
        out.build_path_index();
      }
    }
  }

  out.build_path_index();
  return out;
}

}  // namespace next
}  // namespace lightusd
