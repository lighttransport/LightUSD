// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include <unordered_set>
#include <string_view>
#include "cache-internal.hh"
namespace lightusd {
namespace next {
namespace pcp {

std::vector<PropertyOpinion> Cache::GetPropertyStack(
    const Path& path, const std::string& property, size_t limit,
    bool* truncated, std::string* warn, std::string* err) {
  NEXT_PCP_WRITE_LOCK(impl_->api_mu_);
  if (truncated) *truncated = false;
  std::vector<PropertyOpinion> out;
  const auto& sources = impl_->SourcesForPath(path, warn, err);
  auto append = [&](PropertyOpinion opinion) {
    if (out.size() == limit) { if (truncated) *truncated = true; return; }
    out.push_back(std::move(opinion));
  };
  for (const auto& source : sources) {
    if (source.variant) {
      for (const auto& p : source.variant->properties) {
        if (p.name != property) continue;
        PropertyOpinion item;
        // Inline variants keep a pointer to their authored option. Locate its
        // owning spec instead of attributing a weak-layer option to the root.
        auto locate = [&](auto&& self, const std::vector<PrimVariantSetData>& sets,
                          std::string* suffix, size_t depth) -> bool {
          if (depth > 256) return false;
          for (const auto& set : sets) for (const auto& variant : set.variants) {
            const std::string component = "{" + set.name + "=" + variant.name + "}";
            if (&variant == source.variant) { *suffix = component; return true; }
            std::string nested;
            if (self(self, variant.variantSets, &nested, depth + 1)) {
              *suffix = component + nested; return true;
            }
          }
          return false;
        };
        item.prim_path = source.site;
        for (const auto& spec : impl_->SpecsFor(source)) {
          std::string suffix;
          if (locate(locate, spec.spec->meta().variantSets(), &suffix, 0)) {
            item.layer_identifier = spec.layer_id();
            item.prim_path += suffix;
            break;
          }
        }
        item.arc = "variant";
        item.offset = source.offset().offset;
        item.scale = source.offset().scale;
        if (source.expression_variables()) {
          item.expression_variables = *source.expression_variables();
        }
        item.has_default = !p.value.is_empty();
        item.default_value = p.value;
        append(std::move(item));
      }
      continue;
    }
    for (const auto& spec : impl_->SpecsFor(source)) {
      const auto* slot = spec.spec->property(property);
      if (!slot) continue;
      PropertyOpinion item;
      item.layer_identifier = spec.layer_id();
      item.prim_path = spec.spec->path().str();
      item.arc = ArcTypeName(source.arc_kind);
      const LayerOffset time = source.offset().Compose(spec.layer_offset);
      item.offset = time.offset;
      item.scale = time.scale;
      item.suppressed = source.suppress_site_specs;
      if (source.expression_variables()) {
        item.expression_variables = *source.expression_variables();
      }
      if (const auto* value = spec.spec->property_value(slot->name_id)) {
        item.has_default = !value->is_empty();
        item.default_value = *value;
      }
      if (const auto* samples = spec.spec->time_samples(slot->name_id)) {
        item.has_samples = !samples->empty();
        item.sample_times.reserve(samples->size());
        for (const auto& sample : *samples) {
          item.sample_times.push_back(time.Apply(sample.first));
        }
      }
      if (const auto* connections = spec.spec->connection(property)) {
        item.has_connection = !connections->empty();
        item.connections.reserve(connections->size());
        for (const Path& connection : *connections) {
          item.connections.push_back(connection.str());
        }
      }
      append(std::move(item));
    }
  }
  return out;
}

void Cache::Impl::ComposeOpinions(const std::vector<Src> &srcs, PrimSpec *out) {
    // Composed specifier (pxr _GetPrimSpecifierImpl): NOT plain strength
    // order. A defining specifier always beats `over`, and a `class` due to a
    // DIRECT (non-ancestral) inherit is weaker than any other defining
    // specifier — classness is not inherited, so scanning continues looking
    // for a (possibly weaker) `def`. A class reached through an ancestral
    // inherit resolves in plain strength order. Resolved here independently
    // of CopyLocalOpinions' fill-absent promotion and applied after the loop.
    PrimSpecifier resolved_specifier = PrimSpecifier::Over;
    bool specifier_done = false;
    bool specifier_seen = false;

    // Value-clip strength (LVRPS): opinions from sources at-or-stronger than
    // the source that introduces the `clips` metadata beat clips; opinions
    // from weaker sources (a reference/payload behind the clips author) LOSE
    // to clips. Composition merges opinions fill-absent with provenance lost,
    // so snapshot which properties already carry a value/samples when clips
    // first compose; anything filled afterwards is recorded as clip-shadowed
    // for AttributeEval.
    bool clips_snapshot_taken = false;
    std::vector<PropNameId> pre_clip_props;
    const auto snapshot_pre_clip_props = [&]() {
      // Read through a const view: the mutable accessor would allocate the
      // metadata ext (~1.3 KB) on every composed prim.
      const PrimMetadata &out_meta = out->meta();
      if (clips_snapshot_taken || !out_meta.clips().is_dictionary()) return;
      clips_snapshot_taken = true;
      for (const PropSlot &sl : out->properties().slots()) {
        if (sl.is_relationship()) continue;
        if (sl.value_offset != UINT32_MAX || sl.is_time_sampled()) {
          pre_clip_props.push_back(sl.name_id);
        }
      }
    };

    // Pass 1 (strong->weak): compose opinions.
    for (const Src &s : srcs) {
      // Variant source: graft the selected variant's inline opinions
      // (properties + relationships) with fill-absent semantics. Variant child
      // prims are not yet modeled (PrimVariantData has no child storage).
      if (s.variant) {
        // Prim-state opinions from the selected option (authored, so a
        // weaker source's default cannot flip them back). Fill-absent like
        // every other authored opinion: a STRONGER source's authored active
        // wins over a weaker variant's (keeps ComposedActive() in agreement
        // with the filled output).
        if ((!s.variant->active || s.variant->active_authored) &&
            !out->meta().active_authored) {
          out->meta().active = s.variant->active;
          out->meta().active_authored = true;
        }
        // Authored hidden (true OR false) is a real opinion: `hidden = 0`
        // on the selected option must appear on the composed prim (pxr
        // parity — flatten emits `hidden = false`).
        if ((s.variant->hidden || s.variant->hidden_authored) &&
            !out->meta().hidden_authored) {
          out->meta().hidden = s.variant->hidden;
          out->meta().hidden_authored = true;
        }
        if (!s.variant->doc.empty() && out->meta().doc().empty()) {
          out->meta().doc() = s.variant->doc;
        }
        if (!s.variant->kind.empty() && !out->meta().kindAuthored() &&
            static_cast<const PrimMetadata &>(out->meta()).kind().empty()) {
          out->meta().kind() = s.variant->kind;
          out->meta().setKindAuthored();
        }
        for (const auto &pr : s.variant->properties) {
          const PropSlot *ts = out->property(pr.name);
          if (!ts) {
            out->add_property(pr.name, pr.value, pr.flags);
          } else if (ts->value_offset == UINT32_MAX) {
            // Field-level fill-absent (see CopyLocalOpinions): a variant default
            // fills a stronger connection-only / declared-only slot.
            out->fill_property_value_if_absent(
                GetPropNameTable().intern(pr.name), pr.value);
          }
        }
        for (const auto &rp : s.variant->relationships) {
          if (out->relationship(rp.first)) continue;
          for (const Path &t : rp.second) {
            out->add_relationship(rp.first, Path(Mapping(s.map_idx).Apply(t.str())));
          }
        }
        continue;
      }

      if (s.suppress_site_specs) continue;  // relocation-source opinions
      const std::vector<SpecRef> &specs = SpecsFor(s);
      if (specs.empty()) continue;

      // A relocate authored in a layer stack does NOT remap the TARGET paths of
      // opinions authored in THAT SAME stack's own layers -- only targets brought
      // in via arcs BENEATH it (pxr: "relocates only affect target paths brought
      // in from the reference, not paths authored locally in this layer",
      // BasicRelocateToAnimInterfaceAsNewRootPrim). Every source `s` here is a
      // site-own opinion of `s.stack_idx`, so strip that stack's own relocate
      // renames from its target-remap map (ancestor stacks' relocates, carried
      // through the arc, are kept). Falls back to the full map when the stack
      // authored no relocates.
      // Relocates are uncommon, while this loop runs for every composed prim
      // source. Do not copy the source mapping (and its pair vector) merely to
      // discover that there is nothing to strip.
      const NamespaceMapping *s_target_map = &Mapping(s.map_idx);
      NamespaceMapping arc_only_storage;
      if (!layer_stacks[s.stack_idx].relocates.empty()) {
        arc_only_storage = ArcOnlyMapping(s);
        s_target_map = &arc_only_storage;
      }

      for (const SpecRef &sr : specs) {
        const PrimSpec *spec = sr.spec;

        specifier_seen = true;
        if (!specifier_done) {
          const PrimSpecifier cur = spec->specifier();
          if (cur == PrimSpecifier::Def) {
            resolved_specifier = PrimSpecifier::Def;
            specifier_done = true;
          } else if (cur == PrimSpecifier::Class) {
            resolved_specifier = PrimSpecifier::Class;
            // A direct-inherit class is tentative: keep scanning for a def.
            if (s.arc_kind != ArcType::Inherit || s.ancestral) {
              specifier_done = true;
            }
          }
          // `over` never contributes a defining specifier.
        }

        // A relative asset path anchors to the layer that AUTHORED it, and this
        // loop runs strong->weak, so the strongest source carrying an anchor is
        // the one whose opinions (and asset paths) win. Without this the
        // flattened prim loses its authoring layer and `@../tex/foo.png@` gets
        // resolved against the stage root. See asset-anchor.hh.
        if (out->asset_anchor_id() == 0 && spec->asset_anchor_id() != 0) {
          out->set_asset_anchor_id(spec->asset_anchor_id());
        }

        // Remap relationship/connection TARGET paths from this arc's
        // (site-local) namespace into the composed namespace, so a referenced
        // asset's internal targets (e.g. material:binding, .connect) resolve to
        // their flattened paths. Identity for local opinions. The time offset
      // composes the arc's offset with the spec layer's sublayer offset.
      const LayerOffset spec_off = s.offset().Compose(sr.layer_offset);
      const bool remap_opinion_targets = !s_target_map->pairs.empty();
      if (!remap_opinion_targets) {
        // Most local opinions do not cross an arc or relocation. Avoid
        // constructing a per-property std::function/string remapper for this
        // overwhelmingly common case.
        Compositor::CopyLocalOpinions(*out, *spec, spec_off.offset,
                                      spec_off.scale);
      } else {
        Compositor::CopyLocalOpinions(
            *out, *spec, spec_off.offset, spec_off.scale,
            [&](const std::string &p) {
              // Target paths outside a reference/payload arc's namespace are
              // unmappable: return "" so CopyLocalOpinions drops them (pxr
              // behavior). VARIANT arcs are namespace-preserving — targets in
              // a variant body are authored in the HOST's namespace (outside
              // the holder/content prefix by construction), so map leniently.
              if (s.arc_kind == ArcType::Variant) {
                return Mapping(s.map_idx).Apply(p);
              }
              std::string r = s_target_map->ApplyTarget(p);
              if (!r.empty() || s.arc_kind != ArcType::Inherit || !s.arc_chain())
                return r;
              // A connection/relationship target authored in an inherited local
              // CLASS that points OUTSIDE the class namespace (at a SIBLING
              // instance of that class) is unmappable by the class's own
              // implied-class map, which only covers the class subtree — yet the
              // class lives in a REFERENCED layer stack, so the target resolves
              // through that reference. Walk the arc chain (deepest first) and map
              // through the first arc-crossing (reference/payload) map that can
              // express the target. pxr: ErrorInvalidInstanceTargetPath's
              // SymBrow.sculpt.amount.connect -> sibling {L,R}Brow.InnUD.
              const std::string inst_root =
                  Path(Mapping(s.map_idx).Apply(s.site)).parent().str();
              const std::string prim =
                  p.substr(0, p.find_first_of(".["));  // strip .prop / [target]
              for (auto it = s.arc_chain()->rbegin(); it != s.arc_chain()->rend();
                   ++it) {
                const NamespaceMapping &cm = Mapping(it->second);
                if (!cm.crosses_arc) continue;
                const std::string cr = cm.ApplyTarget(p);
                if (cr.empty()) continue;
                // INVALID SELF-INSTANCE TARGET: a target that, un-relocated in the
                // referenced stack, lies UNDER the very instance this class is
                // inherited into is a non-invertible self-reference. pxr keeps it
                // at the reference-mapped but PRE-relocate path (does NOT apply the
                // instance's own relocate). Other-instance targets relocate
                // normally.
                const std::string inst_arc = cm.ReverseApply(inst_root);
                const std::string prim_unrelo =
                    FullyUnrelocate(it->first, prim);
                if (NamespaceMapping::AtOrUnder(prim_unrelo, inst_arc)) {
                  // Un-do the composed (root-stack) relocate that `cm` applied,
                  // returning the reference-mapped but PRE-relocate path — the
                  // invalid self-instance target is kept there (pxr does not
                  // relocate it, unlike an other-instance target).
                  const size_t dot = cr.find_first_of(".[");
                  const std::string cr_prim =
                      dot == std::string::npos ? cr : cr.substr(0, dot);
                  const std::string cr_suf =
                      dot == std::string::npos ? std::string() : cr.substr(dot);
                  const std::string ur = FullyUnrelocate(0, cr_prim);
                  if (!ur.empty()) return ur + cr_suf;
                }
                return cr;
              }
              return r;
            });
      }
        snapshot_pre_clip_props();
      }
    }
    if (specifier_seen) {
      out->set_specifier(resolved_specifier);
    } else {
      // No contributing spec at all (e.g. a relocate-arrival "ghost" prim
      // whose source authored nothing): pxr materializes it as `over`.
      out->set_specifier(PrimSpecifier::Over);
    }
    if (clips_snapshot_taken) {
      for (const PropSlot &sl : out->properties().slots()) {
        if (sl.is_relationship()) continue;
        if (sl.value_offset == UINT32_MAX && !sl.is_time_sampled()) continue;
        if (std::find(pre_clip_props.begin(), pre_clip_props.end(),
                      sl.name_id) == pre_clip_props.end()) {
          out->meta().clipShadowedProps().push_back(sl.name_id);
        }
      }
    }

    // Flatten drops variant SELECTION metadata: the selected variant's content
    // has already been grafted inline (ExpandArcs / variant Src), so pxr's
    // flattened output carries no `variants = {...}` or variantSets (usdcat emits
    // zero). Strip the vestigial selection that rode along via CopyLocalOpinions.
    // Use a const view for the emptiness checks so we never allocate the meta ext
    // just to clear an absent field.
    {
      const PrimMetadata &cm = out->meta();
      if (!cm.variantSelection.empty()) out->meta().variantSelection.clear();
      if (!cm.variantSets().empty()) out->meta().variantSets().clear();
      if (cm.variantSetNameEdits().authored) {
        out->meta().variantSetNameEdits() = StringListOps();
      }
      if (!cm.variantSelections().empty()) out->meta().variantSelections().clear();
    }

    // Sparse array edits stacked across the sources above (CopyLocalOpinions
    // concatenates weaker-under-stronger); this loop has now seen EVERY source
    // of the prim, so nothing weaker can still supply a base array. Resolve the
    // leftovers -- over the prim's own value, else an empty array -- exactly as
    // the serial compositor's depth-1 ResolveArrayEditsInLayer does (pxr
    // flatten emits concrete arrays, never edit text). Failures stay silent by
    // design: this can run from a parallel-warm worker fill (issues_ is not
    // thread-safe there), and a failed edit degrades to its authored text.
    {
      std::string apply_err;
      Compositor::ResolveArrayEditsOnPrim(out, &apply_err);
    }

    // Free what the merges touched but left at defaults (the stripped
    // variant-set list op above leaves an empty rare metadata tier on every
    // prim that references a variant-set asset), and share plain property
    // metadata now rather than at the layer's finalize, after every prim of
    // a ~10M-prim stage has been filled.
    out->meta().trim_ext();
    out->share_property_metas();
  }

}  // namespace pcp
}  // namespace next
}  // namespace lightusd

namespace lightusd { namespace next { namespace pcp {

auto Cache::Impl::SelectInlineArc(const PrimMetadata &m,
                                                         ArcSel f) -> const std::vector<std::string> & {
    switch (f) {
      case ArcSel::References: return m.references;
      case ArcSel::Payloads: return m.payloads;
      case ArcSel::Inherits: return m.inherits;
      default: return m.specializes;
    }
  }

auto Cache::Impl::SelectArcEdit(const PrimMetadata &m, ArcSel f) -> const ArcListOp * {
    const ArcListOps *e = m.arc_edits();
    if (!e) return nullptr;
    switch (f) {
      case ArcSel::References: return &e->references;
      case ArcSel::Payloads: return &e->payloads;
      case ArcSel::Inherits: return &e->inherits;
      default: return &e->specializes;
    }
  }

auto Cache::Impl::MergeArcField(
      const std::vector<SpecRef> &specs, ArcSel f) const -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> result;
    for (auto it = specs.rbegin(); it != specs.rend(); ++it) {
      const PrimMetadata &m = it->spec->meta();
      const std::string &lid = it->layer_id();
      const ArcListOp *e = SelectArcEdit(m, f);
      const std::vector<std::string> &inl = SelectInlineArc(m, f);
      if ((!e || !e->authored) && inl.empty()) {
        continue;
      }
      auto tag = [&](const std::vector<std::string> &v) {
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(v.size());
        for (const std::string &s : v) out.emplace_back(s, lid);
        return out;
      };
      if (!e || !e->authored || e->is_explicit) {
        result = tag(inl);
        continue;
      }
      auto removeAll = [&](const std::vector<std::string> &rm) {
        for (const std::string &x : rm) {
          result.erase(std::remove_if(result.begin(), result.end(),
                                      [&](const auto &p) { return p.first == x; }),
                       result.end());
        }
      };
      removeAll(e->deleted);
      removeAll(e->prepended);
      removeAll(e->appended);
      for (const auto &added : tag(e->added)) {
        const bool present = std::find_if(
            result.begin(), result.end(), [&](const auto &p) {
              return p.first == added.first;
            }) != result.end();
        if (!present) result.push_back(added);
      }
      std::vector<std::pair<std::string, std::string>> merged;
      merged.reserve(e->prepended.size() + result.size() + e->appended.size());
      auto pre = tag(e->prepended), app = tag(e->appended);
      merged.insert(merged.end(), pre.begin(), pre.end());
      merged.insert(merged.end(), result.begin(), result.end());
      merged.insert(merged.end(), app.begin(), app.end());
      result.swap(merged);

      // `reorder` changes only effective order; it neither adds nor removes
      // arcs. Apply the shared SdfListOp ordering rule to arc keys, then carry
      // each key's authoring-layer tag along so relative assets retain their
      // original anchor.
      if (!e->ordered.empty() && !result.empty()) {
        const auto tagged = result;
        std::vector<std::string> keys;
        keys.reserve(result.size());
        for (const auto &item : result) keys.push_back(item.first);
        ApplyStringListOrder(e->ordered, &keys);
        result.clear();
        result.reserve(keys.size());
        for (const std::string &key : keys) {
          const auto found = std::find_if(
              tagged.begin(), tagged.end(), [&](const auto &item) {
                return item.first == key;
              });
          if (found != tagged.end()) result.push_back(*found);
        }
      }
    }
    return result;
  }

auto Cache::Impl::ComposeInto(const std::vector<Src> &srcs,
                                        PrimSpec *out) -> std::vector<std::string> {
    ComposeOpinions(srcs, out);
    // Pass 2 (weak->strong): compose child-name ORDER.
    return ComposeChildNames(srcs);
  }

auto Cache::Impl::ComposedActive(const std::vector<Src> &srcs) -> bool {
    for (const Src &s : srcs) {
      if (s.variant) {
        if (!s.variant->active) return false;
        continue;  // variant active=true is not an authored opinion
      }
      const std::vector<SpecRef> &specs = SpecsFor(s);
      for (const SpecRef &sr : specs) {
        if (sr.spec->meta().active_authored) return sr.spec->meta().active;
      }
    }
    return true;
  }

auto Cache::Impl::ComposeChildNames(const std::vector<Src> &srcs) -> std::vector<std::string> {
    // Namespace edits authored by any contributing layer stack: a relocated-away
    // name is PROHIBITED here (it no longer exists at this parent, even when a
    // weaker stack still authors a spec for it -- the "shadow spec" case), and a
    // relocated-in name is a child of this prim regardless of where its content
    // physically lives.
    const ChildRelocations edits = RelocationsAt(srcs);
    std::vector<std::string> child_names;
    // Dedup without a node allocation per child: a linear scan while the list
    // is short (the common case), a hash set of views once it grows. Views
    // point into child_names' strings; the set is rebuilt if those move.
    std::unordered_set<std::string_view> seen_big;
    auto first_seen = [&](const std::string &name) -> bool {
      if (child_names.size() < 32) {
        for (const std::string &c : child_names)
          if (c == name) return false;
        child_names.push_back(name);
        if (child_names.size() == 32) {
          seen_big.reserve(128);
          for (const std::string &c : child_names) seen_big.insert(c);
        }
        return true;
      }
      if (seen_big.count(name)) return false;
      const std::string *old_data = child_names.data();
      child_names.push_back(name);
      if (child_names.data() != old_data) {
        seen_big.clear();
        for (const std::string &c : child_names) seen_big.insert(c);
      } else {
        seen_big.insert(child_names.back());
      }
      return true;
    };
    for (auto sit = srcs.rbegin(); sit != srcs.rend(); ++sit) {
      const Src &s = *sit;
      if (s.variant) continue;  // variant child prims not modeled yet
      // Opinions authored at a relocation SOURCE path are prohibited (pxr
      // "salted earth"): their child prims must NOT enter the composed child
      // list either. The Src itself stays for traversal (chained relocations),
      // but its own spec children are suppressed — a child authored at the
      // source (ErrorInvalidReferenceToRelocationSource PreReloChild) leaked
      // while arc-delivered children (model.usd ModelChild) correctly follow.
      if (s.suppress_site_specs) continue;
      const std::vector<SpecRef> &specs = SpecsFor(s);
      for (auto rit = specs.rbegin(); rit != specs.rend(); ++rit) {
        const PrimSpec *spec = rit->spec;
        const Layer *layer = rit->layer;
        for (uint32_t ci : spec->child_indices()) {
          const PrimSpec *cs = layer->prim(ci);
          if (!cs) continue;
          // Bracketed variant HOLDER prims ("{set=sel}") are namespace markers,
          // not real children -- the selected holder's descendants are grafted
          // onto the host as a Variant source (see ExpandArcs). Skip the markers
          // so they never leak into the composed child list.
          if (!cs->name().empty() && cs->name().front() == '{') continue;
          if (edits.departed.count(cs->name())) continue;
          first_seen(cs->name());
        }
      }
    }
    for (const std::string &name : edits.arrived) {
      // A name that one stack relocates IN and another relocates OUT is a
      // chained-relocation pass-through (CHARACTER -> Character -> Model): the
      // content flows to the final destination, so the intermediate name must
      // NOT materialize (pxr collapses the chain). TrickyMultipleRelocations5.
      if (edits.departed.count(name)) continue;
      first_seen(name);
    }
    return child_names;
  }

auto Cache::Impl::ComposePrim_locked(const Path &prim_path, std::string *warn,
                                     std::string *err) -> const PrimSpec * {
    const std::string key = prim_path.str();
    auto it = composed_cache_.find(key);
    if (it != composed_cache_.end()) return it->second.get();

    const std::vector<Src> &srcs = SourcesForPath(prim_path, warn, err);
    if (!AnyAuthors(srcs)) return nullptr;

    auto spec = std::unique_ptr<PrimSpec>(new PrimSpec(prim_path.name()));
    spec->set_path(prim_path);
    std::vector<std::string> children = ComposeInto(srcs, spec.get());

    const PrimSpec *ret = spec.get();
    composed_cache_.emplace(key, std::move(spec));
    composed_children_[key] = std::move(children);
    return ret;
  }

auto Cache::Impl::ComputePrimIndex(const Path &prim_path, std::string *warn,
                                    std::string *err) -> const PrimIndex * {
#if defined(LIGHTUSD_ENABLE_THREAD) && defined(LIGHTUSD_NEXT_FINE_LOCKS)
    // F4 fast path: a shared lock lets cache hits resolve concurrently. On a
    // miss we fall through to the exclusive build; ComputePrimIndex_locked
    // re-checks index_cache under the write lock, so a prim contended by many
    // threads is still built exactly once.
    {
      NEXT_PCP_READ_LOCK(api_mu_);
      auto it = index_cache.find(prim_path.str());
      if (it != index_cache.end()) return it->second.get();
    }
#endif
    NEXT_PCP_WRITE_LOCK(api_mu_);
    return ComputePrimIndex_locked(prim_path, warn, err);
  }

auto Cache::Impl::ComputePrimIndex_locked(const Path &prim_path,
                                           std::string *warn, std::string *err) -> const PrimIndex * {
    const std::string key = prim_path.str();
    auto it = index_cache.find(key);
    if (it != index_cache.end()) return it->second.get();

    const std::vector<Src> &srcs = SourcesForPath(prim_path, warn, err);
    if (!AnyAuthors(srcs)) {
      return nullptr;  // prim does not exist in the composed namespace.
    }

    auto index = std::unique_ptr<PrimIndex>(new PrimIndex());
    index->SetPath(prim_path);
    index->SetLayerStacks(&layer_stacks);

    std::vector<uint16_t> order;
    std::vector<Site> sites;
    for (size_t i = 0; i < srcs.size(); ++i) {
      const Src &s = srcs[i];
      CompNode n;
      n.arc_type = (i == 0) ? ArcType::Root : s.arc_kind;
      n.parent = (i == 0) ? 0xFFFF : 0;
      n.layer_stack_idx = s.stack_idx;
      n.site_path_idx = InternPath(s.site);
      n.map_to_root = Mapping(s.map_idx);
      n.offset = s.offset();
      const std::vector<SpecRef> &specs = SpecsFor(s);
      if (!specs.empty()) {
        n.flags |= NodeFlags::HasSpecs;
        for (const SpecRef &sr : specs) {
          sites.push_back(Site{sr.layer_id().empty()
                                   ? layer_stacks[s.stack_idx].identifier
                                   : sr.layer_id(),
                               s.site});
        }
      }
      uint16_t ni = index->AddNode(std::move(n));
      if (ni == PrimIndex::kInvalidNode) {
        AddIssue(ErrorCode::IndexCapacityExceeded, key,
                 "PrimIndex node count exceeds uint16 capacity for " + key, err);
        return nullptr;
      }
      if (i != 0) index->MutableNode(0).children.push_back(ni);
      order.push_back(ni);
    }
    index->SetStrengthOrder(std::move(order));
    // Bind the path table AFTER the node loop interned every site, so the
    // size snapshot covers all of this index's site_path_idx values (F3).
    index->SetPathTable(&path_table, path_table.size());

    // Per-prim cache: pick the prototype deterministically (min path) so the
    // grouping does not depend on the order/threads of ComputePrimIndex (F5).
    RegisterInstance(key, srcs, /*prefer_min=*/true);

    for (const Site &site : sites) site_to_indices[site].insert(key);
    index_to_sites[key] = std::move(sites);

    const PrimIndex *ret = index.get();
    index_cache.emplace(key, std::move(index));
    return ret;
  }

auto Cache::Impl::IsPreviewProperty(const std::string &name) -> bool {
    if (name == "extent" || name == "extentsHint" ||
        name == "visibility" || name == "purpose" ||
        name == "xformOpOrder" || name == "radius" ||
        name == "height" || name == "width" || name == "size" ||
        name == "axis" || name == "orientation" ||
        name == "primvars:displayColor" ||
        name == "primvars:displayOpacity") {
      return true;
    }
    if (name.compare(0, 8, "xformOp:") == 0) return true;
    return name == "projection" || name == "horizontalAperture" ||
           name == "verticalAperture" ||
           name == "horizontalApertureOffset" ||
           name == "verticalApertureOffset" || name == "focalLength" ||
           name == "clippingRange" || name == "clippingPlanes" ||
           name == "fStop" || name == "focusDistance" ||
           name == "shutter:open" || name == "shutter:close" ||
           name == "exposure";
  }

auto Cache::Impl::ComposePreviewOpinions(const std::vector<Src> &srcs, PrimSpec *out) -> void {
    if (!out) return;
    for (const Src &s : srcs) {
      if (s.variant) {
        for (const VariantProperty &property : s.variant->properties) {
          if (!IsPreviewProperty(property.name) ||
              out->property_value(property.name)) continue;
          out->add_property(property.name, property.value, property.flags);
        }
        continue;
      }
      if (s.suppress_site_specs) continue;
      const std::vector<SpecRef> &specs = SpecsFor(s);
      for (const SpecRef &source : specs) {
        const PrimSpec *spec = source.spec;
        if (!spec) continue;
        if (out->type_name().empty() && !spec->type_name().empty()) {
          out->set_type_name(spec->type_name());
        }
        for (const PropSlot &slot : spec->properties().slots()) {
          if (slot.is_relationship()) continue;
          const std::string name(GetPropNameTable().get(slot.name_id));
          if (!IsPreviewProperty(name) || out->property_value(slot.name_id)) {
            continue;
          }
          const Value *value = spec->property_value(slot.name_id);
          if (!value) continue;
          out->add_property(slot.name_id, *value, slot.flags);
          if (const std::string *type_name = spec->property_type_name(name)) {
            out->set_property_type_name(name, *type_name);
          }
        }
      }
    }
    if (!ComposedActive(srcs)) {
      out->meta().active = false;
      out->meta().active_authored = true;
    }
  }

auto Cache::Impl::ClassifyPreviewEndpoint(const std::vector<Src> &srcs) -> PreviewEndpoint {
    bool bound = false;
    for (const Src &s : srcs) {
      if (s.variant) {
        for (const VariantProperty &property : s.variant->properties) {
          if (property.name == "extent" || property.name == "extentsHint") {
            bound = true;
          }
        }
        continue;
      }
      if (s.suppress_site_specs) continue;
      for (const SpecRef &source : SpecsFor(s)) {
        const PrimSpec *spec = source.spec;
        if (!spec) continue;
        if (spec->type_name() == "Camera") return PreviewEndpointCamera;
        if (spec->property("extent") || spec->property("extentsHint")) {
          bound = true;
        }
      }
    }
    return bound ? PreviewEndpointBound : PreviewEndpointNone;
  }

auto Cache::Impl::BuildPreviewLayer(const Layer &structure) -> Layer {
    const size_t count = structure.prim_count();
    std::vector<uint32_t> parent(count, UINT32_MAX);
    for (uint32_t i = 0; i < count; ++i) {
      const PrimSpec *prim = structure.prim(i);
      if (!prim) continue;
      for (uint32_t child : prim->child_indices()) {
        if (child < count) parent[child] = i;
      }
    }

    std::vector<uint8_t> selected(count, 0);
    size_t selected_bounds = 0;
    constexpr size_t kMaxPreviewBounds = 20000;
    for (const auto &record : fill_) {
      if (record.first >= count || !record.second) continue;
      const PreviewEndpoint endpoint =
          ClassifyPreviewEndpoint(*record.second);
      if (endpoint == PreviewEndpointNone) continue;
      if (endpoint == PreviewEndpointBound &&
          selected_bounds >= kMaxPreviewBounds) continue;
      if (endpoint == PreviewEndpointBound) ++selected_bounds;
      uint32_t index = record.first;
      while (index != UINT32_MAX && !selected[index]) {
        selected[index] = 1;
        index = parent[index];
      }
    }

    Layer preview;
    std::vector<uint32_t> remap(count, UINT32_MAX);
    std::vector<std::pair<uint32_t, const std::vector<Src>*>> preview_fill;
    preview_fill.reserve(fill_.size());
    for (const auto &record : fill_) {
      const uint32_t old_index = record.first;
      if (old_index >= count || !selected[old_index]) continue;
      const PrimSpec *source = structure.prim(old_index);
      if (!source) continue;
      PrimSpec prim(source->name());
      prim.set_path(source->path());
      const uint32_t new_index = preview.add_prim(std::move(prim));
      remap[old_index] = new_index;
      preview_fill.push_back({new_index, record.second});
    }
    for (uint32_t old_index = 0; old_index < count; ++old_index) {
      const uint32_t new_index = remap[old_index];
      if (new_index == UINT32_MAX) continue;
      const uint32_t old_parent = parent[old_index];
      if (old_parent != UINT32_MAX && remap[old_parent] != UINT32_MAX) {
        preview.set_parent(new_index, remap[old_parent]);
      } else {
        preview.add_root(new_index);
      }
    }

    GetPropNameTable().freeze();
    // This set is capped at 20k. A serial pass avoids thread launch and lock
    // traffic, and keeps this latency-only path independent of anonymous Src
    // linkage (important for strict GCC warning builds).
    for (const auto &record : preview_fill) {
      if (record.second) {
        ComposePreviewOpinions(*record.second,
                               preview.prim_mutable(record.first));
      }
    }
    return preview;
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
// For release_layers_during_fill: the fill batch after which each source
// layer is no longer read. A source reads specs (and variant content) from the
// layers of its layer stack, so every layer of every stack a record's sources
// use counts as read by that record. Root-stack layers (possibly the caller's)
// are never released.
auto Cache::Impl::PlanLayerRelease(size_t opinion_batch, int nt) -> LayerReleasePlan {
  const auto t0 = std::chrono::steady_clock::now();
  LayerReleasePlan plan;
  const size_t nstacks = layer_stacks.size();
  const size_t n = fill_.size();
  const size_t workers = std::max<size_t>(1, std::min<size_t>(nt, 16));
  // Last record per stack, per worker slice (records ascend within a slice).
  std::vector<std::vector<uint32_t>> last(workers,
                                          std::vector<uint32_t>(nstacks, UINT32_MAX));
  {
    std::vector<std::thread> pool;
    for (size_t t = 0; t < workers; ++t) {
      pool.emplace_back([&, t] {
        std::vector<uint32_t> &mine = last[t];
        const size_t lo = n * t / workers, hi = n * (t + 1) / workers;
        for (size_t i = lo; i < hi; ++i) {
          const std::vector<Src> *srcs = fill_[i].second;
          if (!srcs) continue;
          for (const Src &s : *srcs)
            if (s.stack_idx < nstacks) mine[s.stack_idx] = static_cast<uint32_t>(i);
        }
      });
    }
    for (std::thread &th : pool) th.join();
  }
  std::unordered_set<const Layer *> keep;
  if (!layer_stacks.empty())
    for (const auto &l : layer_stacks[0].layers) keep.insert(l.get());
  std::unordered_map<Layer *, uint32_t> layer_last;
  for (size_t st = 0; st < nstacks; ++st) {
    uint32_t lst = UINT32_MAX;
    for (size_t t = 0; t < workers; ++t)
      if (last[t][st] != UINT32_MAX && (lst == UINT32_MAX || last[t][st] > lst))
        lst = last[t][st];
    if (lst == UINT32_MAX) continue;  // stack read by no record
    const LayerStack &stack = layer_stacks[st];
    for (size_t li = 0; li < stack.layers.size(); ++li) {
      const auto &l = stack.layers[li];
      if (!l || keep.count(l.get())) continue;
      if (batch_scope_) {
        // A batch releases only the layers no later batch reads.
        const std::string &id = li < stack.layer_identifiers.size()
                                    ? stack.layer_identifiers[li]
                                    : stack.identifier;
        if (!batch_scope_->releasable_layers ||
            !batch_scope_->releasable_layers->count(id)) {
          keep.insert(l.get());
          layer_last.erase(l.get());
          continue;
        }
      }
      uint32_t &v = layer_last[l.get()];
      if (lst + 1 > v) v = lst + 1;  // stored +1: 0 = unset
    }
  }
  // A layer some stack keeps but no record reads stays (not in the map).
  const size_t nbatches = (n + opinion_batch - 1) / opinion_batch;
  plan.by_batch.resize(nbatches);
  for (const auto &kv : layer_last) {
    plan.by_batch[(kv.second - 1) / opinion_batch].push_back(kv.first);
    ++plan.layers;
  }
  plan.ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
  return plan;
}
#endif

auto Cache::Impl::FillOpinions(
    Layer *out,
    const std::vector<const std::vector<Src> *>& shared_srcs) -> void {
    // The serial structure pass has parsed every referenced layer, so the full
    // set of property names is now interned. Freeze the name table: the parallel
    // fill below does heavy find() (and intern() that all HIT) on it, and the
    // per-call shared_lock otherwise contends purely on the lock's cache line.
    // Frozen, those go lock-free. A genuinely new name would unfreeze and fall
    // back to locking.
    GetPropNameTable().freeze();
    // With release_build_sources, free each slot's composition sources the
    // moment its opinions are composed, so the transient compose state (~600
    // MB of Src vectors on reference scene) drains as the output fills in instead of
    // coexisting in full with the finished stage. The structure walk is done,
    // so nothing resolves sources again during the fill, and the caller drops
    // sources_cache right after it (those emptied entries must never be read
    // back as "no sources"). A vector shared by several slots (one source path
    // placed at two output paths) is left for the final drop.
    const bool release = options.release_build_sources;
    auto release_shared = [&] {
      if (!release) return;
      const size_t n = shared_srcs.size();
      if (shared_srcs.empty()) {
        if (options.enable_timing)
          LIGHTUSD_LOG_I("[next_fill] shared_vectors=0 shared_release=0ms");
        return;
      }
      const auto start = std::chrono::steady_clock::now();
#if defined(LIGHTUSD_ENABLE_THREAD)
      int nt = options.num_threads;
      if (nt < 0)
        nt = std::min<int>(static_cast<int>(std::thread::hardware_concurrency()),
                           kMaxExecutionThreads);
      nt = std::max(1, std::min(nt, kMaxExecutionThreads));
      if (nt > 1 && n >= 2048) {
        const size_t workers = std::min<size_t>(static_cast<size_t>(nt), n);
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (size_t t = 0; t < workers; ++t) {
          const size_t lo = n * t / workers;
          const size_t hi = n * (t + 1) / workers;
          pool.emplace_back([&, lo, hi] {
            for (size_t i = lo; i < hi; ++i)
              std::vector<Src>().swap(
                  *const_cast<std::vector<Src> *>(shared_srcs[i]));
          });
        }
        for (std::thread &th : pool) th.join();
      } else
#endif
      {
        for (const std::vector<Src> *srcs : shared_srcs)
          std::vector<Src>().swap(*const_cast<std::vector<Src> *>(srcs));
      }
      if (options.enable_timing) {
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        LIGHTUSD_LOG_I("[next_fill] shared_vectors=" +
                       UIntToStr(n) + " shared_release=" +
                       FormatMilliseconds(elapsed) + "ms");
      }
    };
    auto do_range = [&](size_t b, size_t e) {
      for (size_t i = b; i < e; ++i) {
        const std::vector<Src> *srcs = fill_[i].second;
        if (!srcs) continue;
        if (PrimSpec *ps = out->prim_mutable(fill_[i].first)) {
          ComposeOpinions(*srcs, ps);
        }
        if (release &&
            !std::binary_search(shared_srcs.begin(), shared_srcs.end(), srcs,
                                std::less<const std::vector<Src> *>{})) {
          // sources_cache owns the vector (address-stable deque storage);
          // fill_ only borrows it read-only for the structure pass.
          std::vector<Src>().swap(*const_cast<std::vector<Src> *>(srcs));
        }
      }
    };
#if defined(LIGHTUSD_ENABLE_THREAD)
    int nt = options.num_threads;
    if (nt < 0) {
      nt = std::min<int>(static_cast<int>(std::thread::hardware_concurrency()),
                         kMaxExecutionThreads);
    }
    if (nt > 1 && fill_.size() >= 2048) {
      if (nt > kMaxExecutionThreads) nt = kMaxExecutionThreads;
      // Namespace order clusters heavy mesh/material prims. Static contiguous
      // partitions leave workers idle when one district contains most of the
      // authored properties, so claim modest batches dynamically instead.
      const size_t opinion_batch = options.opinion_batch_size != 0
                                       ? options.opinion_batch_size
                                       : std::max<size_t>(
                                             1, fill_.size() /
                                                    (static_cast<size_t>(nt) *
                                                     static_cast<size_t>(32)));
      std::atomic<size_t> next{0};
      // release_layers_during_fill: a source layer's prims are freed once
      // every batch up to the last record reading it is done.
      LayerReleasePlan plan;
      if (release && options.release_layers_during_fill &&
          (!batch_scope_ || batch_scope_->releasable_layers))
        plan = PlanLayerRelease(opinion_batch, nt);
      const size_t nbatches = (fill_.size() + opinion_batch - 1) / opinion_batch;
      std::unique_ptr<std::atomic<uint8_t>[]> batch_done;
      std::mutex watermark_mu;
      size_t watermark = 0;  // every batch below is done
      std::atomic<size_t> released_prims{0};
      std::atomic<size_t> purged_at{0};
      if (!plan.by_batch.empty()) {
        batch_done.reset(new std::atomic<uint8_t>[nbatches]);
        for (size_t k = 0; k < nbatches; ++k) batch_done[k].store(0);
      }
      auto finish_batch = [&](size_t k) {
        if (!batch_done) return;
        batch_done[k].store(1, std::memory_order_release);
        std::vector<Layer *> free_now;
        {
          std::lock_guard<std::mutex> lk(watermark_mu);
          while (watermark < nbatches &&
                 batch_done[watermark].load(std::memory_order_acquire)) {
            for (Layer *l : plan.by_batch[watermark]) free_now.push_back(l);
            ++watermark;
          }
        }
        size_t freed = 0;
        for (Layer *l : free_now) {
          freed += l->prims().size();
          l->release_prims();
        }
        if (!freed) return;
        // Hand the freed pages back in steps, not per layer (a purge walks
        // every arena).
        const size_t total = released_prims.fetch_add(freed) + freed;
        size_t last = purged_at.load();
        if (trimmer_ && total - last >= (size_t(1) << 20) &&
            purged_at.compare_exchange_strong(last, total))
          trimmer_->Request();
      };
      std::vector<std::thread> pool;
      pool.reserve(nt);
      for (int t = 0; t < nt; ++t) {
        pool.emplace_back([&]() {
          for (;;) {
            const size_t b = next.fetch_add(opinion_batch,
                                            std::memory_order_relaxed);
            if (b >= fill_.size()) break;
            do_range(b, std::min(fill_.size(), b + opinion_batch));
            finish_batch(b / opinion_batch);
          }
        });
      }
      for (std::thread &th : pool) th.join();
      release_shared();
      if (batch_done && trimmer_) trimmer_->Request();
      if (options.enable_timing && !plan.by_batch.empty())
        LIGHTUSD_LOG_I("[next_fill] layers_released=" +
                       UIntToStr(plan.layers) + " layer_prims_released=" +
                       UIntToStr(released_prims.load()) + " plan=" +
                       FormatMilliseconds(plan.ms) + "ms");
      return;
    }
#endif
    do_range(0, fill_.size());
    release_shared();
  }

}}} // namespace lightusd::next::pcp
