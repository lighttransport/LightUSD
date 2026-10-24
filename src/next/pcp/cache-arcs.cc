// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "cache-internal.hh"
namespace lightusd {
namespace next {
namespace pcp {

std::string Cache::Impl::RealAnchorOf(std::string id) {
    while (id.compare(0, 8, "variant:") == 0) {
      std::string s = id.substr(8);
      bool ok = true;
      for (int k = 0; k < 3; ++k) {  // strip trailing :site:vset:vname
        auto p = s.rfind(':');
        if (p == std::string::npos) { ok = false; break; }
        s.resize(p);
      }
      if (!ok) break;
      const size_t host_sep = s.find('\x1f');
      if (host_sep != std::string::npos) s.resize(host_sep);
      id.swap(s);
    }
    return id;
  }

std::string Cache::Impl::ResolveArcPrimPath(const std::string &authoring_site,
                                        const std::string &prim_path) {
    if (prim_path.empty() || prim_path[0] == '/') return prim_path;
    std::vector<std::string> parts;
    size_t start = 1;  // authoring_site is absolute ("/A/B")
    while (start < authoring_site.size()) {
      const size_t sep = authoring_site.find('/', start);
      const size_t end = sep == std::string::npos ? authoring_site.size() : sep;
      if (end > start) parts.push_back(authoring_site.substr(start, end - start));
      if (sep == std::string::npos) break;
      start = sep + 1;
    }
    start = 0;
    while (start <= prim_path.size()) {
      const size_t sep = prim_path.find('/', start);
      const size_t end = sep == std::string::npos ? prim_path.size() : sep;
      const std::string part = prim_path.substr(start, end - start);
      if (part == "..") {
        if (parts.empty()) return std::string();  // above the pseudo-root
        parts.pop_back();
      } else if (!part.empty() && part != ".") {
        parts.push_back(part);
      }
      if (sep == std::string::npos) break;
      start = sep + 1;
    }
    std::string out;
    for (const std::string &part : parts) out += "/" + part;
    return out;
  }

void Cache::Impl::ProcessDeferredVariants(std::vector<Src> *main_list,
                               std::vector<Src> *spec_list,
                               std::map<std::string, std::string> *sels,
                               std::string *warn, std::string *err) {
    const auto owned = [&](const DeferredVariant &e) {
      return e.out == main_list || e.out == spec_list;
    };
    for (int round = 0; round < 8; ++round) {
      bool applied_any = false;
      for (size_t ei = 0; ei < deferred_variants_.size(); ++ei) {
        if (!owned(deferred_variants_[ei])) continue;
        const PrimVariantSetData *vss = nullptr;
        for (const PrimVariantSetData &v :
             deferred_variants_[ei].spec->meta().variantSets()) {
          if (v.name == deferred_variants_[ei].set_name) {
            vss = &v;
            break;
          }
        }
        std::string chosen;
        if (vss) {
          auto sit = sels->find(deferred_variants_[ei].set_name);
          if (sit != sels->end()) {
            chosen = sit->second;
          } else if (const std::string *fb = FallbackSelection(*vss)) {
            chosen = *fb;
          }
        }
        if (!vss || chosen.empty()) continue;

        // Take the entry out BEFORE applying: the graft may push new deferred
        // entries (nested sets) and reallocate the vector.
        DeferredVariant e = std::move(deferred_variants_[ei]);
        deferred_variants_.erase(deferred_variants_.begin() +
                                 static_cast<ptrdiff_t>(ei));
        --ei;

        const PrimVariantData *vd = nullptr;
        for (const PrimVariantData &o : vss->variants) {
          if (o.name == chosen) {
            vd = &o;
            break;
          }
        }
        std::vector<Src> tmp_main;
        std::vector<Src> tmp_spec;
        if (vd) {
          // The chosen option's sibling-set selections become visible first
          // (first-wins: a stronger recorded selection stays).
          for (const auto &op : vd->variantSelections) {
            sels->emplace(op.first, op.second);
          }
          ApplyVariantOption(e.src, nullptr, e.set_name, vd, *e.spec, 0,
                             e.layer_id, &tmp_main, &tmp_spec, sels, e.chain,
                             warn, err);
        }
        // Crate-holder representation of the same set.
        const std::string holder =
            e.src.site + "/{" + e.set_name + "=" + chosen + "}";
        if (!Specs(e.src.stack_idx, holder).empty()) {
          Src vsrc;
          vsrc.stack_idx = e.src.stack_idx;
          vsrc.site = holder;
          vsrc.map_idx = InternMapping(NamespaceMapping::Compose(
              Mapping(e.src.map_idx), NamespaceMapping{holder, e.src.site}));
          vsrc.offset = e.src.offset;
          vsrc.arc_kind = ArcType::Variant;
          vsrc.implied_anchor = e.src.implied_anchor;
          ExpandArcs(vsrc, nullptr, &tmp_main, &tmp_spec, sels, e.chain, warn,
                     err);
        }

        const auto splice = [&](std::vector<Src> *dst, size_t pos,
                                std::vector<Src> *tmp) {
          if (tmp->empty()) return;
          if (pos > dst->size()) pos = dst->size();
          const size_t n = tmp->size();
          // Shift positions of EXISTING entries in the destination list first,
          // then retarget entries recorded during this graft (they point into
          // the tmp vector, with tmp-relative positions).
          for (DeferredVariant &de : deferred_variants_) {
            if (de.out == dst && de.main_pos >= pos) de.main_pos += n;
            if (de.spec_out == dst && de.spec_pos >= pos) de.spec_pos += n;
          }
          for (DeferredVariant &de : deferred_variants_) {
            if (de.out == tmp) {
              de.out = dst;
              de.main_pos += pos;
            }
            if (de.spec_out == tmp) {
              de.spec_out = dst;
              de.spec_pos += pos;
            }
          }
          dst->insert(dst->begin() + static_cast<ptrdiff_t>(pos),
                      std::make_move_iterator(tmp->begin()),
                      std::make_move_iterator(tmp->end()));
          tmp->clear();
        };
        splice(e.out, e.main_pos, &tmp_main);
        splice(e.spec_out, e.spec_pos, &tmp_spec);
        applied_any = true;
      }
      if (!applied_any) break;
    }
    deferred_variants_.erase(
        std::remove_if(deferred_variants_.begin(), deferred_variants_.end(),
                       owned),
        deferred_variants_.end());
  }

void Cache::Impl::ApplyVariantOption(const Src &src, const ExpansionFrame *frame,
                          const std::string &set_name, const PrimVariantData *vd,
                          const PrimSpec &owner, int vdepth,
                          const std::string &layer_id,
                          std::vector<Src> *out, std::vector<Src> *spec_out,
                          std::map<std::string, std::string> *sels,
                          const ArcChain &chain, std::string *warn,
                          std::string *err) {

          // Subtree content: model as a reference-style Variant source into the
          // content layer's "/__self__" root, so child prims compose normally.
          if (vd->content) {
            // Stable variant-content identity: (authoring layer, host stack,
            // host site, variantSet, variantName). The previous pointer-derived id aliased
            // after a PrimVariantData was freed and reallocated (stale instance
            // keys), and never matched across two parses of the same asset.
            const std::string variant_anchor = RealAnchorOf(
                layer_id.empty() ? layer_stacks[src.stack_idx].identifier
                                  : layer_id);
            const std::string vid = "variant:" +
                                    variant_anchor + "\x1f" +
                                    layer_stacks[src.stack_idx].identifier +
                                    ":" + src.site + ":" + set_name + ":" +
                                    vd->name;
            uint32_t cstack = InternLayerStack(vd->content, vid, warn, err);
            if (cstack == kInvalidStack) return;
            Src vsrc;
            vsrc.stack_idx = cstack;
            vsrc.site = "/__self__";
            vsrc.map_idx = InternMapping(NamespaceMapping::Compose(
                Mapping(src.map_idx), NamespaceMapping{"/__self__", src.site}));
            vsrc.offset = src.offset;
            vsrc.arc_kind = ArcType::Variant;
            const ExpansionFrame vframe{cstack, vsrc.map_idx, &vsrc.site, frame,
                                        frame ? frame->depth + 1 : 0};
            ExpandArcs(vsrc, &vframe, out, spec_out, sels, chain, warn, err);
          }
          // Inline opinions (properties/relationships on the host prim
          // itself), plus prim-state opinions (`active = false` on a variant
          // option deactivates the host — a common pruning pattern; authored
          // `hidden = false` / `active = true` are real opinions too, and an
          // option doc composes onto the prim).
          if (!vd->properties.empty() || !vd->relationships.empty() ||
              !vd->active || vd->hidden || vd->active_authored ||
              vd->hidden_authored || !vd->doc.empty() || !vd->kind.empty()) {
            Src vsrc = src;
            vsrc.arc_kind = ArcType::Variant;
            vsrc.variant = vd;
            out->push_back(std::move(vsrc));
          }
          // Composition arcs authored on the variant OPTION (e.g. a `payload`
          // that supplies the def-Mesh geometry — XGen assets author geometry on
          // the selected variant, not in its body). Compose at the variant's
          // strength (after the variant's own content/inline, before the host's
          // references), anchored to the layer that authored the variant so a
          // relative `@./geo.usd@` resolves correctly.
          for (const std::string &s : vd->inherits) {
            ProcessArc(src, Compositor::ParseReference(s), ArcType::Inherit,
                       frame, out, spec_out, sels, chain, warn, err, layer_id);
          }
          for (const std::string &s : vd->references) {
            ProcessArc(src, Compositor::ParseReference(s), ArcType::Reference,
                       frame, out, spec_out, sels, chain, warn, err, layer_id);
          }
          if (!vd->payloads.empty()) {
            const std::string root_prim_path = Mapping(src.map_idx).Apply(src.site);
            bool any_deferred = false;
            for (const std::string &pl_str : vd->payloads) {
              CompositionArc arc = Compositor::ParsePayload(pl_str);
              if (!ShouldLoadPayload(root_prim_path, arc.asset_path,
                                     owner)) {
                any_deferred = true;
                continue;
              }
              ProcessArc(src, arc, ArcType::Payload, frame, out, spec_out, sels,
                         chain, warn, err, layer_id);
            }
            // insert-only here: another arc group's loaded payloads must not
            // clear a deferral recorded for this option's payloads.
            if (any_deferred) deferred_payload_prims.insert(root_prim_path);
          }
          for (const std::string &s : vd->specializes) {
            ProcessArc(src, Compositor::ParseReference(s), ArcType::Specialize,
                       frame, out, spec_out, sels, chain, warn, err, layer_id);
          }

          // Nested variant sets authored on this option (inline / USDA
          // representation). Crate-representation nesting recurses naturally
          // through the holder graft below (the holder prim carries the
          // nested sets in its own meta).
          if (vdepth < 16) {
            for (const PrimVariantSetData &nvs : vd->variantSets) {
              std::string chosen;
              auto nsit = sels->find(nvs.name);
              if (nsit != sels->end()) chosen = nsit->second;
              if (chosen.empty()) chosen = nvs.selected;
              if (chosen.empty()) continue;
              for (const PrimVariantData &nvd : nvs.variants) {
                if (nvd.name == chosen) {
                  ApplyVariantOption(src, frame, nvs.name, &nvd, owner,
                                     vdepth + 1, layer_id, out, spec_out, sels,
                                     chain, warn, err);
                  break;
                }
              }
            }
          }
  }

bool Cache::Impl::PathInRelocateSource(uint32_t stack, const std::string &path) const {
    const StackRelocates &rel = layer_stacks[stack].relocates;
    if (rel.empty()) return false;
    for (const auto &r : rel.src_to_dst) {
      if (IsPathAtOrUnder(path, r.first)) return true;
    }
    return false;
  }

bool Cache::Impl::PathInRelocateTarget(uint32_t stack, const std::string &path) const {
    const StackRelocates &rel = layer_stacks[stack].relocates;
    if (rel.empty()) return false;
    for (const auto &r : rel.src_to_dst) {
      if (!r.second.empty() && IsPathAtOrUnder(path, r.second)) return true;
    }
    return false;
  }

bool Cache::Impl::AncestorAuthorsVariantSet(uint32_t stack,
                                 const std::string &path) const {
    Path cur(path);
    for (;;) {
      Path parent = cur.parent();
      if (parent.is_root() || parent.empty() || parent.str() == "/") break;
      for (const SpecRef &sr : Specs(stack, parent.str())) {
        if (!sr.spec->meta().variantSets().empty()) return true;
      }
      cur = parent;
    }
    return false;
  }

void Cache::Impl::ExpandArcs(const Src &src, const ExpansionFrame *frame,
                  std::vector<Src> *out, std::vector<Src> *spec_out,
                  std::map<std::string, std::string> *sels,
                  const ArcChain &chain, std::string *warn,
                  std::string *err) {
    out->push_back(src);

    // A relocation-source anchor: its own specs (and thus their arcs and
    // variant selections) are prohibited opinions — expand nothing.
    if (src.suppress_site_specs) return;

    const std::vector<SpecRef> &specs = Specs(src.stack_idx, src.site);
    // Stamp the copy just pushed (local vector, no sharing): the structure
    // walk and the fill then answer "has specs?" without re-probing.
    out->back().spec_state.v =
        specs.empty() ? SpecState::kNone : SpecState::kSome;
    if (specs.empty()) return;
    // Record this source's variant selections (strong-first wins) so a selection
    // authored on a stronger source applies to a variantSet defined on a weaker
    // one (cross-source selection).
    for (const SpecRef &sr : specs) {
      RecordSelections(*sr.spec, sels, src.expression_variables, warn, err);
    }

    // Phase 7 (S5): with apply_list_ops, gather each arc field once across the
    // whole site (cross-layer list-op merge: explicit-replace / prepend /
    // append / delete / dedup); otherwise expand each spec's arcs independently
    // (legacy strong-first concatenation). Variants are always per-spec.
    const bool merge = options.apply_list_ops;

    if (merge) {
      for (const auto &s : MergeArcField(specs, ArcSel::Inherits)) {
        ProcessArc(src, Compositor::ParseReference(s.first), ArcType::Inherit,
                   frame, out, spec_out, sels, chain, warn, err, s.second);
      }
    }

    for (const SpecRef &sr : specs) {
      const PrimSpec *spec = sr.spec;

      // Inherits (stronger than references) -- legacy per-spec path.
      if (!merge) {
        for (const std::string &s : spec->meta().inherits) {
          ProcessArc(src, Compositor::ParseReference(s), ArcType::Inherit, frame,
                     out, spec_out, sels, chain, warn, err);
        }
      }

      // Variants (weaker than inherits, stronger than references). For each
      // selected variant, graft its inline opinions and/or its content
      // subtree. `apply_variant` recurses into nested variant sets authored
      // on the option itself (selection from the accumulated map — which
      // includes caller overrides — falling back to the nested set's own
      // `selected`).
      if (!spec->meta().variantSets().empty()) {
        // A selected variant can author a selection for another variant set on
        // the same host. Propagate those opinions to a fixed point before
        // choosing all options (e.g. v1=a authors v2=b, while v2 is a sibling
        // set rather than nested inside v1).
        for (size_t pass = 0; pass < spec->meta().variantSets().size(); ++pass) {
          bool changed = false;
          for (const PrimVariantSetData& set : spec->meta().variantSets()) {
            auto selected = sels->find(set.name);
            if (selected == sels->end()) continue;
            for (const PrimVariantData& option : set.variants) {
              if (option.name != selected->second) continue;
              for (const auto& opinion : option.variantSelections) {
                // `sels` is strength-ordered and pre-seeded with caller
                // overrides. A selected option may fill a still-unknown
                // sibling/nested selection, but must not replace a stronger
                // opinion that is already present.
                changed = sels->emplace(opinion.first, opinion.second).second ||
                          changed;
              }
              break;
            }
          }
          if (!changed) break;
        }
        for (const SelectedVariant &sv : SelectVariants(*spec, *sels)) {
          ApplyVariantOption(src, frame, *sv.set_name, sv.vd, *spec, 0,
                             sr.layer_id(), out, spec_out, sels, chain, warn, err);
        }
        // A variantSet whose selection is authored at a WEAKER site (not yet
        // expanded) cannot graft now, but its opinions must compose at THIS
        // position (variants sit between inherits and references). Record a
        // deferred entry; ExpandList grafts it once every selection is known
        // and splices the result back here (pxr computes selections across
        // the whole index before applying each node's sets —
        // TrickyVariantWeakerSelection).
        for (const PrimVariantSetData &vss : spec->meta().variantSets()) {
          if (sels->count(vss.name) || FallbackSelection(vss)) continue;
          deferred_variants_.push_back(DeferredVariant{
              out, spec_out, out->size(), spec_out->size(), src, spec,
              vss.name, sr.layer_id(), chain});
        }

        // Crate representation: pxr-authored crates store variant content as
        // bracketed HOLDER prims ("/Prim/{set=sel}/..." in the SAME layer stack)
        // and a PrimVariantSetData that carries only {name, selected} -- no inline
        // PrimVariantData.content, so SelectVariants() above finds nothing. For each
        // selected set, graft the selected holder as a Variant source mapped onto
        // the host, so its descendants compose as the host's children (matching
        // the USDA content path). USDA-authored variants have no holder prim at
        // this site, so Specs() is empty and this is a no-op for them.
        for (const PrimVariantSetData &vss : spec->meta().variantSets()) {
          auto sit = sels->find(vss.name);
          const std::string *selection =
              sit != sels->end() ? &sit->second : FallbackSelection(vss);
          if (!selection || selection->empty()) continue;
          const std::string holder =
              src.site + "/{" + vss.name + "=" + *selection + "}";
          if (Specs(src.stack_idx, holder).empty()) continue;  // not crate-style
          Src vsrc;
          vsrc.stack_idx = src.stack_idx;
          vsrc.site = holder;
          vsrc.map_idx = InternMapping(NamespaceMapping::Compose(
              Mapping(src.map_idx), NamespaceMapping{holder, src.site}));
          vsrc.offset = src.offset;
          vsrc.arc_kind = ArcType::Variant;
          const ExpansionFrame vframe{src.stack_idx, vsrc.map_idx, &vsrc.site, frame,
                                      frame ? frame->depth + 1 : 0};
          ExpandArcs(vsrc, &vframe, out, spec_out, sels, chain, warn, err);
        }
      }

      if (merge) continue;  // merged refs/payloads/specializes handled below

      // References. Anchor relative asset paths to THIS spec's authoring layer.
      for (const std::string &ref_str : spec->meta().references) {
        ProcessArc(src, Compositor::ParseReference(ref_str), ArcType::Reference,
                   frame, out, spec_out, sels, chain, warn, err, sr.layer_id());
      }

      // Payloads (deferrable, weaker than references). See the merge path:
      // erase the deferral marker only when NO payload on this prim deferred.
      if (!spec->meta().payloads.empty()) {
        const std::string root_prim_path = Mapping(src.map_idx).Apply(src.site);
        bool any_deferred = false;
        for (const std::string &pl_str : spec->meta().payloads) {
          CompositionArc arc = Compositor::ParsePayload(pl_str);
          if (!ShouldLoadPayload(root_prim_path, arc.asset_path, *spec)) {
            any_deferred = true;
            continue;  // deferred: contributes no opinions.
          }
          ProcessArc(src, arc, ArcType::Payload, frame, out, spec_out, sels,
                     chain, warn, err, sr.layer_id());
        }
        if (any_deferred) {
          deferred_payload_prims.insert(root_prim_path);
        } else {
          deferred_payload_prims.erase(root_prim_path);
        }
      }

      // Specializes (globally weakest; routed into spec_out by ProcessArc).
      for (const std::string &s : spec->meta().specializes) {
        ProcessArc(src, Compositor::ParseReference(s), ArcType::Specialize,
                   frame, out, spec_out, sels, chain, warn, err, sr.layer_id());
      }
    }

    if (merge) {
      // Cross-layer-merged references / payloads / specializes, processed once
      // in LIVRPS order (all inherits already emitted above, before variants).
      for (const auto &s : MergeArcField(specs, ArcSel::References)) {
        ProcessArc(src, Compositor::ParseReference(s.first), ArcType::Reference,
                   frame, out, spec_out, sels, chain, warn, err, s.second);
      }
      auto mpay = MergeArcField(specs, ArcSel::Payloads);
      if (!mpay.empty()) {
        const std::string root_prim_path = Mapping(src.map_idx).Apply(src.site);
        // Track deferral across ALL of this prim's payloads: erasing per
        // loaded arc let a later loaded payload clear the marker set by an
        // earlier deferred one (HasDeferredPayload then lied depending on
        // authored arc order). Erase only when nothing was deferred.
        bool any_deferred = false;
        for (const auto &pl : mpay) {
          CompositionArc arc = Compositor::ParsePayload(pl.first);
          // Preserve the payload's authoring PrimSpec after list-op merging.
          // Besides property-based policies, the owner carries the layer
          // anchor required to stat relative payload asset paths correctly.
          const PrimSpec *policy_owner = specs.front().spec;
          for (const SpecRef &candidate : specs) {
            if (candidate.layer_id() == pl.second) {
              policy_owner = candidate.spec;
              break;
            }
          }
          if (!ShouldLoadPayload(root_prim_path, arc.asset_path,
                                 *policy_owner)) {
            any_deferred = true;
            continue;
          }
          ProcessArc(src, arc, ArcType::Payload, frame, out, spec_out, sels,
                     chain, warn, err, pl.second);
        }
        if (any_deferred) {
          deferred_payload_prims.insert(root_prim_path);
        } else {
          deferred_payload_prims.erase(root_prim_path);
        }
      }
      for (const auto &s : MergeArcField(specs, ArcSel::Specializes)) {
        ProcessArc(src, Compositor::ParseReference(s.first), ArcType::Specialize,
                   frame, out, spec_out, sels, chain, warn, err, s.second);
      }
    }
  }

std::vector<Src> Cache::Impl::ExpandList(const std::vector<Src> &base,
                              const std::string &root_prim_path,
                              std::string *warn, std::string *err) {
    std::vector<Src> main;
    std::vector<Src> spec;  // specialize-derived: globally weakest
    // Variant selections accumulated across the prim's sources (cross-source).
    // Seed with the caller's variant overrides BEFORE expansion: selection
    // recording uses first-wins emplace, so pre-seeding makes an override
    // beat every authored selection during grafting. (It was previously
    // applied after ExpandArcs, so the prim's own variant graft still used
    // the authored selection and only descendant expansion saw the override.)
    std::map<std::string, std::string> sels;
    for (const auto &ov : options.variant_overrides) {
      sels[ov.first] = ov.second;
    }
    const auto scoped = options.variant_overrides_by_path.find(root_prim_path);
    if (scoped != options.variant_overrides_by_path.end()) {
      for (const auto &ov : scoped->second) sels[ov.first] = ov.second;
    }
    for (const Src &s : base) {
      // Carry a base source's own arc kind so a specialize re-rooted onto a
      // child stays globally weakest.
      std::vector<Src> *tgt = (s.arc_kind == ArcType::Specialize) ? &spec : &main;
      // Ancestor chain for implied class arcs: every layer stack (with the
      // mapping it was entered through) between the root stack and this source.
      // A re-rooted child source carries its parent's chain (Src::arc_chain), so
      // a class inherited by a referenced CHILD prim is still implied in the root
      // stack AND in every intermediate stack of the arc chain -- an override
      // authored on the class path at any of those levels composes.
      ArcChain chain;
      if (s.arc_chain) chain = *s.arc_chain;
      if (chain.empty() || chain.front().first != 0) {
        chain.insert(chain.begin(), {0u, 0u});
      }
      if (chain.back().first != s.stack_idx ||
          chain.back().second != s.map_idx) {
        chain.emplace_back(s.stack_idx, s.map_idx);
      }
      // Re-seed the cycle-detection frame from the persisted arc-site trail:
      // the live frame chain reset when the BuildStage walk cached this source
      // and derived it as a child, so an arc that re-targets a site crossed to
      // REACH this source is otherwise invisible. Rebuilding synthetic frames
      // (only stack_idx + site matter to frame->Contains) lets ProcessArc drop
      // the cyclic arc — pxr's ancestral-reference-cycle behavior. The strings
      // live in s.arc_sites (held for this call), so the pointers stay valid.
      std::vector<ExpansionFrame> anc_frames;
      const ExpansionFrame *anc = nullptr;
      if (s.arc_sites && !s.arc_sites->empty()) {
        anc_frames.reserve(s.arc_sites->size());
        for (const auto &hop : *s.arc_sites) {
          anc_frames.push_back(
              ExpansionFrame{hop.first, 0u, &hop.second, anc, 0u});
          anc = &anc_frames.back();
        }
      }
      const ExpansionFrame seed{s.stack_idx, s.map_idx, &s.site, anc, 0};
      ExpandArcs(s, &seed, tgt, &spec, &sels, chain, warn, err);
    }
    // Variant sets whose selection was authored at a WEAKER site graft now,
    // at their recorded strength position (pxr resolves selections across the
    // whole prim index before applying each node's sets).
    ProcessDeferredVariants(&main, &spec, &sels, warn, err);
    // Shadow STALE variant content: a source whose site bakes in a variant
    // selector `{Set=Val}` that CONFLICTS with the finally-resolved selection
    // (sels[Set] != Val) is a superseded variant option's content. It reaches
    // here when a class's own (weaker) selection eagerly grafted its option at
    // the class site, then the INHERITING instance re-selected the same set to a
    // different value (TrickySpookyVariantSelectionInClass: SymLegRig's default
    // 1Leg content arrives via the inherit AND RightLegRig re-selects 2Leg;
    // Inherit outranks Variant, so the stale 1Leg would otherwise win). pxr
    // resolves ONE selection per set across the whole prim index, so drop the
    // conflicting content. Only fires when the set was actually re-selected --
    // an unmentioned set (sels has no entry) keeps all its content.
    const auto has_conflicting_selector = [&](const std::string &site) {
      for (size_t i = 0; (i = site.find('{', i)) != std::string::npos;) {
        const size_t close = site.find('}', i);
        if (close == std::string::npos) break;
        const size_t eq = site.find('=', i);
        if (eq != std::string::npos && eq < close) {
          const std::string set = site.substr(i + 1, eq - i - 1);
          const std::string val = site.substr(eq + 1, close - eq - 1);
          auto it = sels.find(set);
          if (it != sels.end() && it->second != val) return true;
        }
        i = close + 1;
      }
      return false;
    };
    for (std::vector<Src> *lst : {&main, &spec}) {
      lst->erase(std::remove_if(lst->begin(), lst->end(),
                                [&](const Src &s) {
                                  return has_conflicting_selector(s.site);
                                }),
                 lst->end());
    }
    // pxr strength for IMPLIED classes: the implied block composes at its
    // expressing ancestor stack's position — immediately after that stack's
    // own positional opinions — not at the position of the reference that
    // introduced the inherit. (BasicInstancing pcp.txt prim stack:
    // root-local, root-implied-class, set-ancestral, set-implied-class,
    // prop-ref, prop-class.) Move each contiguous implied block up to just
    // after the last earlier non-implied source of its anchor stack.
    for (size_t i = 0; i < main.size();) {
      const uint32_t anchor = main[i].implied_anchor;
      if (anchor == UINT32_MAX) {
        ++i;
        continue;
      }
      size_t k = i + 1;
      while (k < main.size() && main[k].implied_anchor == anchor) ++k;
      size_t ins = SIZE_MAX;
      for (size_t j = i; j-- > 0;) {
        // Insert just after the anchor stack's last positional opinion OR the
        // last ALREADY-RELOCATED implied sibling of the same anchor. The latter
        // keeps a chain of implied classes (A inherits B inherits C, all
        // implied into the same ancestor stack) in DFS strength order: without
        // it every block collapses to the same slot and the chain INVERTS
        // (weakest class ends up strongest — TrickyClassHierarchy).
        if ((main[j].implied_anchor == UINT32_MAX &&
             main[j].stack_idx == anchor) ||
            (main[j].implied_anchor == anchor && main[j].stack_idx == anchor)) {
          ins = j + 1;
          break;
        }
      }
      if (ins == SIZE_MAX || ins >= i) {
        i = k;
        continue;
      }
      std::rotate(main.begin() + static_cast<ptrdiff_t>(ins),
                  main.begin() + static_cast<ptrdiff_t>(i),
                  main.begin() + static_cast<ptrdiff_t>(k));
      i = k;
    }
    main.insert(main.end(), spec.begin(), spec.end());
    return main;
  }

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
