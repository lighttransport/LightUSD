// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "cache-internal.hh"
namespace lightusd {
namespace next {
namespace pcp {

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
      if (clips_snapshot_taken || !out->meta().clips().is_dictionary()) return;
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
      // prims are not yet modeled (VariantData has no child storage).
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
            out->meta().kind().empty()) {
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
      const LayerOffset spec_off = s.offset.Compose(sr.layer_offset);
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
              if (!r.empty() || s.arc_kind != ArcType::Inherit || !s.arc_chain)
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
              for (auto it = s.arc_chain->rbegin(); it != s.arc_chain->rend();
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
      const PrimSpecMeta &cm = out->meta();
      if (!cm.variantSelection.empty()) out->meta().variantSelection.clear();
      if (!cm.variantSets().empty()) out->meta().variantSets().clear();
      if (cm.variantSetNameEdits().authored) {
        out->meta().variantSetNameEdits() = StringListOpEdits();
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
  }

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
