// SPDX-License-Identifier: Apache-2.0
#include "cache-internal.hh"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lightusd {
namespace next {
namespace pcp {

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
          src.expression_variables() ? *src.expression_variables() : empty);
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
            remapped.set_offset(src.offset().Compose(target_src.offset()));
            remapped.arc_kind = kind;
            remapped.ancestral = false;  // reached via this prim's own arc
            remapped.set_expression_variables(src.expression_variables());
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
      if (src.expression_variables()) {
        AppendExpressionVarsFingerprint(src.expression_variables()->view(), &memo_key);
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
        load_opts.max_array_elements = options.max_array_elements;
        load_opts.usdc_limits = options.usdc_limits;
        load_opts.max_archive_entries = options.max_archive_entries;
        load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
        load_opts.usdc_use_mmap = options.usdc_use_mmap;
        load_opts.usda_parse_options = options.usda_parse_options;
        load_opts.parse_num_threads = options.usda_parse_options.num_threads;
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
                                         src.expression_variables());
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
    arc_src.set_offset(src.offset().Compose(arc_off));
    arc_src.arc_kind = kind;
    if (arc_stack_idx == src.stack_idx) {
      arc_src.set_expression_variables(src.expression_variables());
    } else {
      const Value inherited = src.expression_variables()
                                  ? *src.expression_variables()
                                  : Value::MakeDictionary();
      arc_src.set_expression_variables(std::make_shared<const Value>(
          ComposeExpressionVariables(
              layer_stacks[arc_stack_idx].expression_variables, inherited)));
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
                           arc_src.expression_variables(), warn, err);
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
                             arc_src.expression_variables(), warn, err);
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
      s.set_offset(arc_src.offset().Compose(target_src.offset()));
      if (s.stack_idx != arc_stack_idx) {
        const Value inherited = arc_src.expression_variables()
                                    ? *arc_src.expression_variables()
                                    : Value::MakeDictionary();
        s.set_expression_variables(std::make_shared<const Value>(
            ComposeExpressionVariables(
                layer_stacks[s.stack_idx].expression_variables, inherited)));
      }

      // Reference/payload arcs descend into a new layer stack -> extend the chain.
      // The chain rides ALONG with the source (Src::arc_chain), so a child prim
      // re-rooted from it still knows every stack (and each stack's entry
      // mapping) that a class arc must be implied in.
      ArcChain child_chain = chain;
      if (s.stack_idx != src.stack_idx) {
        child_chain.emplace_back(s.stack_idx, s.map_idx);
      }
      s.set_arc_chain(std::make_shared<const ArcChain>(child_chain));

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
        if (src.arc_sites()) *sites = *src.arc_sites();
        sites->emplace_back(s.stack_idx, s.site);
        s.set_arc_sites(std::move(sites));
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
          s.set_offset(src.offset().Compose(composed_src.offset()));
          s.arc_kind = kind;
          s.ancestral = false;  // reached via this prim's own arc
          target->push_back(std::move(s));
        }
      }
    }
  }


}  // namespace pcp
}  // namespace next
}  // namespace lightusd
