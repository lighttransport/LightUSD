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
        const VariantSetData *vss = nullptr;
        for (const VariantSetData &v :
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

        const VariantData *vd = nullptr;
        for (const VariantData &o : vss->variants) {
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
                          const std::string &set_name, const VariantData *vd,
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
            // after a VariantData was freed and reallocated (stale instance
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
            for (const VariantSetData &nvs : vd->variantSets) {
              std::string chosen;
              auto nsit = sels->find(nvs.name);
              if (nsit != sels->end()) chosen = nsit->second;
              if (chosen.empty()) chosen = nvs.selected;
              if (chosen.empty()) continue;
              for (const VariantData &nvd : nvs.variants) {
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

void Cache::Impl::ProcessArc(const Src &src, const CompositionArc &arc_in, ArcType kind,
                  const ExpansionFrame *frame, std::vector<Src> *out,
                  std::vector<Src> *spec_out,
                  std::map<std::string, std::string> *sels,
                  const ArcChain &chain, std::string *warn,
                  std::string *err, const std::string &authoring_layer_id) {
    CompositionArc arc = arc_in;
    if (arc.asset_path.empty() && !arc.prim_path.empty() &&
        arc.prim_path[0] != '/') {
      arc.prim_path = ResolveArcPrimPath(src.site, arc.prim_path);
      if (arc.prim_path.empty()) {
        AddIssue(ErrorCode::UnresolvedPrimPath, src.site,
                 "Relative arc target escapes the pseudo-root: " +
                     arc_in.prim_path,
                 warn);
        return;
      }
    }
    uint32_t arc_stack_idx;
    std::string arc_site;
    std::string evaluated_asset_path = arc.asset_path;
    if (!arc.asset_path.empty() &&
        options.expression_variable_policy !=
            ExpressionVariablePolicy::Disabled) {
      const Value empty = Value::MakeDictionary();
      const ExpressionEvaluation expression = EvaluateAssetPathExpression(
          arc.asset_path,
          src.expression_variables ? *src.expression_variables : empty);
      if (expression.is_expression && !expression.success) {
        AddIssue(ErrorCode::ExpressionVariableError, arc.asset_path,
                 expression.error,
                 options.expression_variable_policy ==
                         ExpressionVariablePolicy::RequireResolved
                     ? err
                     : warn);
        if (options.expression_variable_policy ==
            ExpressionVariablePolicy::RequireResolved) {
          return;
        }
      } else if (expression.is_expression && expression.is_none) {
        // The expression evaluated to None: the arc authors no opinion.
        return;
      } else if (expression.success) {
        evaluated_asset_path = expression.value;
      }
    }

    // An internal arc (class arc or internal reference) targets the COMPOSED
    // namespace, not a raw spec in this layer stack: the target prim may be
    // introduced by an arc of its own (SubrootReferenceNonCycle), delivered by
    // an ancestral inherit (TrickyLocalClassHierarchyWithRelocates), or carry
    // opinions from a VARIANT whose selection is authored in an outer layer
    // stack (TrickyInheritsInVariants). Reuse the target's composed sources and
    // remap them onto this site; the target path must be expressed in the
    // composed namespace to find them (this site's own mapping does that -- the
    // class path is authored in THIS stack's namespace).
    if ((arc.is_internal || evaluated_asset_path.empty()) &&
        !arc.prim_path.empty() &&
        PathInRelocateSource(src.stack_idx, arc.prim_path)) {
      // An internal arc addressing a relocation SOURCE path is invalid
      // ("salted earth": BadRigInstance inheriting the class's pre-move
      // path composes nothing in pxr).
      AddIssue(ErrorCode::UnresolvedPrimPath, src.site,
               "Arc targets a relocation source path (prohibited): " +
                   arc.prim_path,
               warn);
      return;
    }
    if ((arc.is_internal || evaluated_asset_path.empty()) &&
        !arc.prim_path.empty() &&
        (Specs(src.stack_idx, arc.prim_path).empty() ||
         PathInRelocateTarget(src.stack_idx, arc.prim_path) ||
         AncestorAuthorsVariantSet(src.stack_idx, arc.prim_path))) {
      const std::string destination = Mapping(src.map_idx).Apply(src.site);
      const std::string composed_path = Mapping(src.map_idx).Apply(arc.prim_path);
      // A target at or under this site composes from this site -- reusing its
      // sources would graft the prim under itself.
      const bool self_target = IsPathAtOrUnder(destination, composed_path) ||
                               IsPathAtOrUnder(composed_path, destination);
      if (!self_target) {
        const std::vector<Src>& composed_target =
            SourcesForPath(Path(composed_path), warn, err);
        bool has_opinions = false;
        for (const Src& target_src : composed_target) {
          if (target_src.variant || !Specs(target_src.stack_idx, target_src.site).empty()) {
            has_opinions = true;
            break;
          }
        }
        if (has_opinions) {
          std::vector<Src>* target =
              (kind == ArcType::Specialize) ? spec_out : out;
          for (const Src& target_src : composed_target) {
            if (frame && frame->Contains(target_src.stack_idx, target_src.site))
              continue;
            if (target_src.stack_idx == src.stack_idx &&
                IsPathAtOrUnder(src.site, target_src.site)) {
              continue;
            }
            Src remapped = target_src;
            remapped.map_idx = InternMapping(NamespaceMapping::Compose(
                NamespaceMapping{composed_path, destination},
                Mapping(target_src.map_idx)));
            remapped.offset = src.offset.Compose(target_src.offset);
            remapped.arc_kind = kind;
            remapped.ancestral = false;  // reached via this prim's own arc
            remapped.expression_variables = src.expression_variables;
            target->push_back(std::move(remapped));
          }
          return;
        }
      }
    }
    if (arc.is_internal || evaluated_asset_path.empty()) {
      arc_stack_idx = src.stack_idx;
      if (!arc.prim_path.empty()) {
        arc_site = arc.prim_path;
      } else {
        // An explicitly empty internal target (`<>`) selects the defaultPrim
        // of the current layer stack, just as an external @asset@ with an
        // omitted path selects the target layer's defaultPrim. It does not
        // mean a self-reference to the authoring site.
        std::string default_prim;
        for (const std::shared_ptr<Layer>& candidate :
             layer_stacks[src.stack_idx].layers) {
          if (candidate->meta().defaultPrim_set ||
              !candidate->meta().defaultPrim.empty()) {
            default_prim = candidate->meta().defaultPrim;
            break;
          }
        }
        if (default_prim.empty()) {
          AddIssue(ErrorCode::UnresolvedPrimPath, src.site,
                   "Unresolved internal arc target <>: layer stack has no "
                   "defaultPrim", warn);
          return;
        }
        arc_site = "/" + default_prim;
      }
    } else {
      // Anchor = the FILE path of the LAYER that authored the arc (Resolve
      // derives its dir). A reference/payload authored in a SUBLAYER resolves
      // relative to that sublayer, not the layer-stack's root — fall back to the
      // stack identifier only when the authoring layer is unknown.
      const std::string &anchor_source =
          !authoring_layer_id.empty()
              ? authoring_layer_id
              : layer_stacks[src.stack_idx].identifier;
      // Memo key = the arc target's pre-resolution identity (see
      // arc_target_memo_). Expression variables join by the SAME content
      // fingerprint InternLayerStack keys stacks with (arc crossings
      // make_shared a fresh — but usually equal — dictionary per instance,
      // so pointer identity would never hit; the fingerprint is empty/cheap
      // for the no-expression-variables common case).
      std::string memo_key;
      memo_key.reserve(anchor_source.size() + evaluated_asset_path.size() + 2);
      memo_key += anchor_source;
      memo_key += '\1';
      memo_key += evaluated_asset_path;
      memo_key += '\1';
      if (src.expression_variables) {
        AppendExpressionVarsFingerprint(*src.expression_variables, &memo_key);
      }

      std::shared_ptr<Layer> arc_layer;
      std::string arc_id;
      auto mit = arc_target_memo_.find(memo_key);
      if (mit != arc_target_memo_.end()) {
        arc_layer = mit->second.layer;
        arc_id = mit->second.arc_id;
        arc_stack_idx = mit->second.stack_idx;
      } else {
        // COPY, not a reference: GetOrLoad/InternLayerStack below can grow
        // layer_stacks and invalidate a reference into it.
        const std::string anchor = RealAnchorOf(anchor_source);
        LayerLoadOptions load_opts;
        load_opts.max_memory = options.max_layer_memory;
        load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
        load_opts.usdc_use_mmap = options.usdc_use_mmap;
        load_opts.usda_parse_options = options.usda_parse_options;
        load_opts.strict_aousd_conformance = options.strict_aousd_conformance;
        arc_layer =
            reg_->GetOrLoad(*resolver, evaluated_asset_path, anchor, warn, err,
                            load_opts);
        if (!arc_layer) {
          if (options.error_when_asset_not_found) {
            AddIssue(ErrorCode::InvalidAssetPath, arc.asset_path,
                     "Arc asset not found: " + arc.asset_path, err);
          }
          return;
        }
        arc_id = resolver->ResolvePath(
            evaluated_asset_path, anchor, !options.strict_aousd_conformance);
        // The referencing site's composed expression variables become part of
        // the target stack's IDENTITY (pxr: (identifier, expression variables))
        // and feed its sublayer path expressions.
        arc_stack_idx = InternLayerStack(arc_layer, arc_id, warn, err,
                                         src.expression_variables);
        if (arc_stack_idx == kInvalidStack) return;
        arc_target_memo_.emplace(
            std::move(memo_key), ArcTargetEntry{arc_layer, arc_id, arc_stack_idx});
      }
      if (!arc.prim_path.empty()) {
        arc_site = arc.prim_path;
      } else if (!arc_layer->meta().defaultPrim.empty()) {
        arc_site = "/" + arc_layer->meta().defaultPrim;
      } else {
        // No target prim path and no authored defaultPrim: pxr reports
        // "Unresolved reference prim path @...@<defaultPrim>" and the arc
        // contributes NOTHING (the referencing prim composes empty). The
        // previous silent fallback to the first root prim composed content
        // pxr never would.
        AddIssue(ErrorCode::UnresolvedPrimPath, arc_id,
                 "Unresolved reference prim path @" + arc.asset_path +
                     "@<defaultPrim>: layer has no defaultPrim and the arc "
                     "names no prim path",
                 warn);
        return;
      }
    }

    // A reference/payload to a relocation SOURCE path in the TARGET layer stack
    // is prohibited (pxr "salted earth"): the arc composes nothing, exactly like
    // the internal-arc case checked above but for external @asset@ targets
    // (ErrorInvalidReferenceToRelocationSource /Bad_RefToPreRelo references
    // char.usd</Char/PreRelo>, which char.usd relocated away).
    if (!evaluated_asset_path.empty() &&
        PathInRelocateSource(arc_stack_idx, arc_site)) {
      AddIssue(ErrorCode::UnresolvedPrimPath, arc_site,
               "Reference targets a relocation source path (prohibited): " +
                   arc_site,
               warn);
      return;
    }

    if (frame && frame->Contains(arc_stack_idx, arc_site)) {
      const std::string site =
          layer_stacks[arc_stack_idx].identifier + ":" + arc_site;
      AddIssue(ErrorCode::ArcCycle, site,
               "Composition cycle detected at arc: " + site, err);
      return;
    }
    if (frame && frame->depth + 1 >= options.max_depth) {
      AddIssue(ErrorCode::MaxDepthExceeded,
               layer_stacks[arc_stack_idx].identifier + ":" + arc_site,
               "Composition max depth exceeded", err);
      return;
    }
    // A same-stack arc that targets a namespace ANCESTOR of its own source site
    // grafts the source under itself: the composed namespace grows without
    // bound (e.g. `def "A" { def "B" (references = </A>) {} }`). The frame
    // chain cannot see this (each child prim starts a fresh expansion), so
    // reject it here.
    if (arc_stack_idx == src.stack_idx && arc_site != src.site &&
        IsPathAtOrUnder(src.site, arc_site)) {
      AddIssue(ErrorCode::ArcCycle, src.site,
               "Composition cycle detected: arc at " + src.site +
                   " targets ancestor " + arc_site,
               err);
      return;
    }

    // Same rule in COMPOSED space: a same-stack internal arc whose target maps
    // to an ANCESTOR of this prim's own composed path is a cycle even when the
    // source sites live under different roots (co-recursive inherits across
    // sibling root prims: CoRecursiveParent1/Child1 inherits CoRecursiveParent2
    // whose child inherits back — the arc target composes to /CoRecursiveParent1,
    // an ancestor of the composed /CoRecursiveParent1/Child1/Child2). pxr drops
    // the arc, leaving the re-entrant prim as a bare `over`. The source-site
    // check above misses this because the sites are under different roots.
    if (arc_stack_idx == src.stack_idx) {
      const std::string cur_composed = Mapping(src.map_idx).Apply(src.site);
      const std::string arc_composed = Mapping(src.map_idx).Apply(arc_site);
      if (arc_composed != cur_composed &&
          IsPathAtOrUnder(cur_composed, arc_composed)) {
        AddIssue(ErrorCode::ArcCycle, src.site,
                 "Composition cycle detected: arc composed target " +
                     arc_composed + " is an ancestor of " + cur_composed,
                 err);
        return;
      }
    }

    NamespaceMapping local{arc_site, src.site};
    // A LOCAL class-based arc (class and instance in the same layer stack)
    // gets pxr's identity-plus-pair map function: targets outside the class
    // namespace pass through; targets under the instance are invalid
    // instance targets and drop (see NamespaceMapping::intra_stack).
    if (IsClassBasedArc(kind) && arc_stack_idx == src.stack_idx) {
      local.intra_stack = true;
    }
    Src arc_src;
    arc_src.stack_idx = arc_stack_idx;
    arc_src.site = arc_site;
    // A subtree expanded under an implied class stays anchored to the same
    // expressing stack (the whole block composes at that strength position).
    arc_src.implied_anchor = src.implied_anchor;
    arc_src.map_idx = InternMapping(WithStackRelocates(
        arc_stack_idx,
        NamespaceMapping::Compose(Mapping(src.map_idx), local)));
    // Compose this arc's layer offset under the parent's (root..arc chain), so
    // the referenced content's time samples are mapped into root/stage time.
    LayerOffset arc_off;
    if (!arc.layer_offset.empty()) {
      double o = 0.0, s = 1.0;
      Compositor::ParseLayerOffset(arc.layer_offset, o, s);
      if (!std::isfinite(o) || !std::isfinite(s) || !(s > 0.0)) {
        AddIssue(ErrorCode::InvalidReferenceOffset, arc_site,
                 "Invalid reference/payload layer offset; using identity "
                 "mapping", err);
        o = 0.0;
        s = 1.0;
      }
      arc_off = LayerOffset{o, s};
    }
    // timeCodesPerSecond auto-scale between the referencing layer stack and
    // the referenced layer stack (pxr parity), composed UNDER the authored
    // offset — same rule as the sublayer edge in cache-layer-stack.inc.
    if (arc_stack_idx != src.stack_idx &&
        !layer_stacks[src.stack_idx].layers.empty() &&
        !layer_stacks[arc_stack_idx].layers.empty() &&
        layer_stacks[src.stack_idx].layers[0] &&
        layer_stacks[arc_stack_idx].layers[0]) {
      const double parent_tcps = layer_stacks[src.stack_idx]
                                     .layers[0]
                                     ->effective_timeCodesPerSecond();
      const double child_tcps = layer_stacks[arc_stack_idx]
                                    .layers[0]
                                    ->effective_timeCodesPerSecond();
      if (parent_tcps != child_tcps && child_tcps > 0.0 &&
          std::isfinite(parent_tcps) && std::isfinite(child_tcps)) {
        arc_off = arc_off.Compose(LayerOffset{0.0, parent_tcps / child_tcps});
      }
    }
    arc_src.offset = src.offset.Compose(arc_off);
    arc_src.arc_kind = kind;
    if (arc_stack_idx == src.stack_idx) {
      arc_src.expression_variables = src.expression_variables;
    } else {
      const Value inherited = src.expression_variables
                                  ? *src.expression_variables
                                  : Value::MakeDictionary();
      arc_src.expression_variables = std::make_shared<const Value>(
          ComposeExpressionVariables(
              layer_stacks[arc_stack_idx].expression_variables, inherited));
    }

    // A specialize subtree is globally weakest: it (and everything beneath it)
    // is collected into spec_out.
    std::vector<Src> *target = (kind == ArcType::Specialize) ? spec_out : out;
    const size_t target_mark = target->size();

    // Implied class-arc propagation: a class (inherit/specialize) reached
    // through a reference chain is ALSO expressed in EVERY ancestor layer stack
    // on that chain (root + intermediate references), so an override authored on
    // the same class path at any level composes. Ancestors are pushed first
    // (root strongest) so they outrank the referenced-stack class opinions.
    // Class paths are global, so the site is unchanged across stacks.
    if (IsClassBasedArc(kind)) {
      // The implied class path must be expressed in the ANCESTOR stack's own
      // namespace: an inherit to a SUB-ROOT class (e.g. </Models/_class_X>
      // where /Models is referenced as /World) is implied at the MAPPED path
      // (/World/_class_X) in the referencing stack, so overrides authored
      // there compose (pxr maps implied classes through the arc's map
      // function). A root-level class path lies outside every arc's source
      // prefix, so Apply() leaves it unchanged (the previous behavior).
      const std::string root_implied_site =
          Mapping(src.map_idx).Apply(arc_site);
      std::set<std::pair<uint32_t, uint32_t>> seen_node;
      for (const auto& node : chain) {
        const uint32_t as = node.first;
        const uint32_t ancestor_map_idx = node.second;
        if (as == arc_stack_idx || !seen_node.insert(node).second) continue;
        const std::string implied_site =
            Mapping(ancestor_map_idx).ReverseApply(root_implied_site);
        const std::vector<SpecRef>& implied_specs = Specs(as, implied_site);
        if (implied_specs.empty()) continue;
        // An implied class opinion in a closer-to-root layer stack is stronger
        // than selections authored at the referenced site that introduced the
        // inherit. Record that ancestor stack independently (strong-first),
        // then overlay it onto the accumulated selection context.
        std::map<std::string, std::string> ancestor_selections;
        for (const SpecRef& implied_spec : implied_specs) {
          RecordSelections(*implied_spec.spec, &ancestor_selections,
                           arc_src.expression_variables, warn, err);
        }
        // A variant selection the INHERITING prim (instance) authors on ITS OWN
        // path in this same ancestor stack beats the class's implied selection
        // (pxr: an instance's direct selection is stronger than the selection it
        // inherits from its class -- TrickySpookyVariantSelectionInClass, where
        // RightLegRig authors LegRigStyle=2Leg but inherits SymLegRig whose root
        // over selects 1Leg). Read the instance's own selections at this stack
        // (the class arc's destination, mapped back into `as`) and never let the
        // class overwrite them.
        std::map<std::string, std::string> own_selections;
        {
          const std::string dest = Mapping(src.map_idx).Apply(src.site);
          const std::string dest_in_as =
              Mapping(ancestor_map_idx).ReverseApply(dest);
          for (const SpecRef& own_spec : Specs(as, dest_in_as)) {
            RecordSelections(*own_spec.spec, &own_selections,
                             arc_src.expression_variables, warn, err);
          }
        }
        for (const auto& selection : ancestor_selections) {
          if (own_selections.count(selection.first)) continue;  // instance wins
          (*sels)[selection.first] = selection.second;
        }
        Src implied = arc_src;
        implied.stack_idx = as;
        implied.implied_anchor = as;  // composes at stack `as`'s position
        if (implied_site != arc_site) {
          implied.site = implied_site;
          // Implied specs live at implied_site and compose onto the SAME
          // destination the direct class arc maps to -- the INHERITING prim in
          // root space (`destination`), not the class prim's own composed path.
          // These differ when the inherit is authored inside a referenced layer
          // stack (src.map_idx non-identity); mapping to the class path would
          // land the implied opinions (and their relationship targets) on the
          // class instead of on the prim that inherits it.
          const std::string destination = Mapping(src.map_idx).Apply(src.site);
          NamespaceMapping im{implied_site, destination};
          // A relocate authored in the arc's (referenced) stack that moves a
          // DESCENDANT of the inheriting prim must chain into this implied map:
          // a connection/relationship target authored in the CLASS namespace
          // that points at a relocated descendant otherwise stops at the
          // pre-relocate inherited path. Add, per such relocate, a class-namespace
          // rename so one longest-prefix ApplyTarget does inherit-then-relocate
          // (TrickyConnectionToRelocatedAttribute: SymEyeRig/Anim.baz -> LEye.baz,
          // where LEyeRig/Anim->LEye lives in the FaceRig stack).
          const StackRelocates &arel = layer_stacks[arc_stack_idx].relocates;
          if (!arel.empty()) {
            const NamespaceMapping &amap = Mapping(src.map_idx);
            // Arc pairs only (relocate renames stripped): maps a relocate SOURCE
            // to its PRE-relocate composed path so it lands under `destination`;
            // the full amap (relocates baked in by WithStackRelocates) gives the
            // post-relocate destination.
            NamespaceMapping arc_only = amap;
            {
              std::vector<NamespaceMapping::Pair> kept;
              for (const auto &pr : arc_only.pairs)
                if (!arel.src_to_dst.count(pr.first)) kept.push_back(pr);
              arc_only.pairs = std::move(kept);
            }
            for (const auto &r : arel.src_to_dst) {
              if (r.second.empty()) continue;
              const std::string rsrc = arc_only.crosses_arc
                                           ? arc_only.ApplyTarget(r.first)
                                           : arc_only.Apply(r.first);
              const std::string rdst = amap.crosses_arc
                                           ? amap.ApplyTarget(r.first)
                                           : amap.Apply(r.first);
              if (rsrc.empty() || rdst.empty()) continue;
              if (!NamespaceMapping::AtOrUnder(rsrc, destination)) continue;
              im.AddRename(implied_site + rsrc.substr(destination.size()), rdst);
            }
          }
          implied.map_idx = InternMapping(std::move(im));
        }
        const ArcChain ichain{{as, implied.map_idx}};
        const ExpansionFrame iframe{as, implied.map_idx, &implied.site, frame,
                                    frame ? frame->depth + 1 : 0};
        ExpandArcs(implied, &iframe, target, spec_out, sels, ichain, warn, err);
      }

      // Same-stack implied class/specialize: this arc's target may map (through
      // an ANCESTOR class arc in THIS stack -- `/A specializes /_class_`) to a
      // DIFFERENT path that carries opinions. `/A/render` specializing
      // `/_class_/defaultImplementation` therefore ALSO implies a specialize to
      // `/A/defaultImplementation` (root_implied_site == Apply(arc_site)), whose
      // ancestor-context override composes STRONGER than the direct target
      // (SpecializesAndVariants4, VariantSpecializesAndReferenceSurprising-
      // Behavior). Add it before the direct target below, mirroring the chain
      // loop but for the current stack (which that loop skips via as==arc_stack).
      if (root_implied_site != arc_site &&
          !Specs(arc_stack_idx, root_implied_site).empty()) {
        Src implied = arc_src;
        implied.stack_idx = arc_stack_idx;
        implied.site = root_implied_site;
        implied.implied_anchor = arc_stack_idx;
        implied.map_idx = InternMapping(NamespaceMapping{
            root_implied_site, Mapping(src.map_idx).Apply(src.site)});
        const ArcChain ichain{{arc_stack_idx, implied.map_idx}};
        const ExpansionFrame iframe{arc_stack_idx, implied.map_idx,
                                    &implied.site, frame,
                                    frame ? frame->depth + 1 : 0};
        ExpandArcs(implied, &iframe, target, spec_out, sels, ichain, warn, err);
      } else if (root_implied_site != arc_site &&
                 kind == ArcType::Specialize) {
        // The implied specialize target (`root_implied_site`, e.g. /Model/Material)
        // composes via ARCS, with no raw spec of its own. pxr propagates the
        // specialize to it, and its contributors reached via an arc ANCESTRAL to
        // the specialize-declaration (a prepend reference on the INSTANCE, not
        // inside the selected variant) compose STRONGER than the DIRECT target —
        // the reverse of the target's own composition. But a contributor that is
        // also part of the DIRECT target's (`arc_site`) OWN composition (a
        // reference authored INSIDE the variant that carries the declaration) is
        // NOT ancestral and must stay in normal order.
        //   VariantSpecializesAndReferenceSurprisingBehavior: the reference is on
        //     /Model (ancestral) -> reference>variant, Material_Child.myInt=0.
        //   VariantSpecializesAndReference: the reference is inside the variant
        //     (on /New_Shading_Variant) -> variant>reference, myInt=1 (unchanged).
        // Route only the contributors of `root_implied_site` that the DIRECT
        // target does NOT already deliver, ahead of it (stronger).
        std::set<std::pair<uint32_t, std::string>> direct_sites;
        for (const Src &ds :
             SourcesForSite(arc_stack_idx, Path(arc_site), warn, err))
          direct_sites.emplace(ds.stack_idx, ds.site);
        const std::string destination = Mapping(src.map_idx).Apply(src.site);
        NamespaceMapping redirect;
        redirect.AddRename(root_implied_site, destination);
        for (const Src &cs :
             SourcesForSite(arc_stack_idx, Path(root_implied_site), warn, err)) {
          if (cs.variant) continue;  // the variant graft that carries the decl
          if (cs.arc_kind == ArcType::Root && cs.site == root_implied_site)
            continue;  // the empty composed-prim placeholder
          if (direct_sites.count({cs.stack_idx, cs.site}))
            continue;  // delivered by the DIRECT specialize (not ancestral)
          Src si = cs;
          si.map_idx =
              InternMapping(NamespaceMapping::Compose(redirect, Mapping(cs.map_idx)));
          si.arc_kind = ArcType::Specialize;
          si.implied_anchor = arc_stack_idx;
          si.ancestral = true;
          spec_out->push_back(std::move(si));
        }
      }
    }

    // The arc's target is not just the spec at `arc_site`: the target prim's own
    // ANCESTORS inside the target layer stack may carry arcs that deliver it (a
    // sub-root reference `@char.usd@</Char/Sub>` where `/Char` itself references
    // model.usd), and the target stack's relocates may have moved it there. So
    // derive the target's sources from its parent's stack-local expansion --
    // exactly how the composed namespace derives any child -- instead of seeding
    // a bare (stack, site). The site's own spec is always the first, strongest
    // entry, so the common (root-prim target) case is unchanged.
    //
    // Only the ANCESTORS resolve in the target stack's own context: variant
    // selections authored at the referencing site apply to the target prim and
    // below, so each target source is expanded here with the caller's `sels`.
    std::vector<Src> arc_srcs;
    {
      const Path arc_path(arc_site);
      const Path arc_parent = arc_path.parent();
      const bool root_prim = arc_parent.is_root() || arc_parent.empty() ||
                             arc_parent.str() == "/";
      if (root_prim) {
        Src seed;
        seed.stack_idx = arc_stack_idx;
        seed.site = arc_site;
        arc_srcs.push_back(std::move(seed));
        // A ROOT-prim target can still be a relocate ARRIVAL in the target
        // stack (`relocates = { </CharRig/.../Path>: </Path> }` moves a rig
        // scope to a NEW root prim that is then referenced directly). The
        // non-root branch gets this through DeriveChildSources; mirror the
        // arrival rule here so the relocation SOURCE's opinions (its def
        // specifier, type, and subtree arcs) compose into the arc target.
        const StackRelocates &rel = layer_stacks[arc_stack_idx].relocates;
        if (!rel.empty()) {
          if (const StackRelocates::Arrival *arrival =
                  rel.ArrivalOf("/", arc_path.name())) {
            // Pseudo-parent at the target stack's root: identity mapping, so
            // the arrival mappings stay expressed in the target stack's
            // namespace like every other arc_srcs entry (the seed above is
            // the dst-site spec itself; AddRelocatedSources would re-add it,
            // so only its relocate-source CONTENT expansion is appended).
            Src ps;
            ps.stack_idx = arc_stack_idx;
            std::vector<Src> with_content;
            AddRelocatedSources(ps, *arrival, arc_site, &with_content, warn,
                                err);
            for (Src &c : with_content) {
              // The dst-site spec itself is already seeded above.
              if (c.stack_idx == arc_stack_idx && c.site == arc_site) continue;
              arc_srcs.push_back(std::move(c));
            }
          }
        }
      } else {
        const std::vector<Src> &ancestral =
            SourcesForSite(arc_stack_idx, arc_parent, warn, err);
        arc_srcs = DeriveChildSources(ancestral, arc_path.name(), arc_site,
                                      warn, err);
      }
    }

    for (const Src &target_src : arc_srcs) {
      if (frame && frame->Contains(target_src.stack_idx, target_src.site))
        continue;
      if (target_src.stack_idx == src.stack_idx &&
          target_src.site != arc_site &&
          IsPathAtOrUnder(src.site, target_src.site)) {
        continue;  // would graft this prim under itself
      }
      Src s = arc_src;
      s.stack_idx = target_src.stack_idx;
      s.site = target_src.site;
      // A relocation-source anchor stays suppressed when reached THROUGH an arc
      // (a reference to a relocate arrival re-delivers the arrival's sources,
      // including the prohibited source-site opinions): without carrying the
      // flag, the source's specs and children leak into the referencing prim
      // (ErrorInvalidReferenceToRelocationSource /Good_RefToRelocated).
      s.suppress_site_specs = target_src.suppress_site_specs;
      // target_src's mapping is expressed in the TARGET STACK's namespace (it
      // maps its own site to `arc_site`); compose it under this arc's mapping.
      s.map_idx = InternMapping(NamespaceMapping::Compose(
          Mapping(arc_src.map_idx), Mapping(target_src.map_idx)));
      s.offset = arc_src.offset.Compose(target_src.offset);
      if (s.stack_idx != arc_stack_idx) {
        const Value inherited = arc_src.expression_variables
                                    ? *arc_src.expression_variables
                                    : Value::MakeDictionary();
        s.expression_variables = std::make_shared<const Value>(
            ComposeExpressionVariables(
                layer_stacks[s.stack_idx].expression_variables, inherited));
      }

      // Reference/payload arcs descend into a new layer stack -> extend the chain.
      // The chain rides ALONG with the source (Src::arc_chain), so a child prim
      // re-rooted from it still knows every stack (and each stack's entry
      // mapping) that a class arc must be implied in.
      ArcChain child_chain = chain;
      if (s.stack_idx != src.stack_idx) {
        child_chain.emplace_back(s.stack_idx, s.map_idx);
      }
      s.arc_chain = std::make_shared<const ArcChain>(child_chain);

      // Extend the persisted arc-site trail with EVERY arc hop (cross-stack
      // references AND same-stack internal inherits/specializes), so both an
      // ancestral reference cycle (AnotherParent) and a same-stack co-recursive
      // inherit cycle (CoRecursiveParent1<->2) are detectable after the live
      // frame chain is gone (see Src::arc_sites). A benign re-reference does NOT
      // false-positive: a LOCAL child derives from its local parent with a clean
      // trail (only content reached THROUGH the arc carries the target), so a
      // hit means the arc genuinely re-enters its own derivation path.
      {
        auto sites = std::make_shared<std::vector<std::pair<uint32_t, std::string>>>();
        if (src.arc_sites) *sites = *src.arc_sites;
        sites->emplace_back(s.stack_idx, s.site);
        s.arc_sites = std::move(sites);
      }

      const ExpansionFrame cframe{s.stack_idx, s.map_idx, &s.site, frame,
                                  frame ? frame->depth + 1 : 0};
      ExpandArcs(s, &cframe, target, spec_out, sels, child_chain, warn, err);
    }

    // A class prim can also carry opinions that only exist in the COMPOSED
    // namespace: an `over` on the class authored inside a VARIANT of one of its
    // ancestors, where the selection comes from an outer layer stack
    // (TrickyInheritsInVariants). The expansion above resolves the class in its
    // own stack, which cannot see that selection. Supplement it with the class
    // prim as the stage composes it, skipping everything already gathered.
    if (IsClassBasedArc(kind)) {
      const std::string destination = Mapping(src.map_idx).Apply(src.site);
      const std::string composed_path = Mapping(src.map_idx).Apply(arc_site);
      if (!IsPathAtOrUnder(destination, composed_path) &&
          !IsPathAtOrUnder(composed_path, destination)) {
        std::set<std::pair<uint32_t, std::string>> seen;
        for (size_t i = target_mark; i < target->size(); ++i) {
          seen.emplace((*target)[i].stack_idx, (*target)[i].site);
        }
        for (const Src &composed_src :
             SourcesForPath(Path(composed_path), warn, err)) {
          if (composed_src.variant) continue;  // inline opinions travel with their host
          if (!seen.insert({composed_src.stack_idx, composed_src.site}).second) {
            continue;
          }
          if (frame && frame->Contains(composed_src.stack_idx, composed_src.site))
            continue;
          if (Specs(composed_src.stack_idx, composed_src.site).empty()) continue;
          Src s = composed_src;
          s.map_idx = InternMapping(NamespaceMapping::Compose(
              NamespaceMapping{composed_path, destination},
              Mapping(composed_src.map_idx)));
          s.offset = src.offset.Compose(composed_src.offset);
          s.arc_kind = kind;
          s.ancestral = false;  // reached via this prim's own arc
          target->push_back(std::move(s));
        }
      }
    }
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
          for (const VariantSetData& set : spec->meta().variantSets()) {
            auto selected = sels->find(set.name);
            if (selected == sels->end()) continue;
            for (const VariantData& option : set.variants) {
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
                             sr.layer_id, out, spec_out, sels, chain, warn, err);
        }
        // A variantSet whose selection is authored at a WEAKER site (not yet
        // expanded) cannot graft now, but its opinions must compose at THIS
        // position (variants sit between inherits and references). Record a
        // deferred entry; ExpandList grafts it once every selection is known
        // and splices the result back here (pxr computes selections across
        // the whole index before applying each node's sets —
        // TrickyVariantWeakerSelection).
        for (const VariantSetData &vss : spec->meta().variantSets()) {
          if (sels->count(vss.name) || FallbackSelection(vss)) continue;
          deferred_variants_.push_back(DeferredVariant{
              out, spec_out, out->size(), spec_out->size(), src, spec,
              vss.name, sr.layer_id, chain});
        }

        // Crate representation: pxr-authored crates store variant content as
        // bracketed HOLDER prims ("/Prim/{set=sel}/..." in the SAME layer stack)
        // and a VariantSetData that carries only {name, selected} -- no inline
        // VariantData.content, so SelectVariants() above finds nothing. For each
        // selected set, graft the selected holder as a Variant source mapped onto
        // the host, so its descendants compose as the host's children (matching
        // the USDA content path). USDA-authored variants have no holder prim at
        // this site, so Specs() is empty and this is a no-op for them.
        for (const VariantSetData &vss : spec->meta().variantSets()) {
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
                   frame, out, spec_out, sels, chain, warn, err, sr.layer_id);
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
                     chain, warn, err, sr.layer_id);
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
                   frame, out, spec_out, sels, chain, warn, err, sr.layer_id);
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
            if (candidate.layer_id == pl.second) {
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
