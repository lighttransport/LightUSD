// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "cache-internal.hh"
namespace lightusd {
namespace next {
namespace pcp {

void Cache::Impl::AppendExpressionVarsFingerprint(const Value &vars,
                                              std::string *out) {
    const Dict *d = vars.as_dictionary();
    if (!d || d->entries().empty()) return;
    std::vector<const std::pair<std::string, Value> *> sorted;
    sorted.reserve(d->entries().size());
    for (const auto &entry : d->entries()) sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<std::string, Value> *a,
                 const std::pair<std::string, Value> *b) {
                return a->first < b->first;
              });
    for (const auto *entry : sorted) {
      *out += '\x1f';
      *out += entry->first;
      *out += '=';
      const Value &v = entry->second;
      if (const std::string *s = v.as_string()) *out += *s;
      else if (const std::string *s = v.as_token()) *out += *s;
      else if (const std::string *s = v.as_asset_path()) *out += *s;
      else if (const bool *b = v.as_bool()) *out += *b ? '1' : '0';
      else if (const int32_t *i = v.as_int()) *out += IntToStr(*i);
      else if (const int64_t *i = v.as_int64()) *out += IntToStr(*i);
      else if (const uint32_t *u = v.as_uint()) *out += UIntToStr(*u);
      else if (v.is_dictionary()) {
        *out += '{';
        AppendExpressionVarsFingerprint(v, out);
        *out += '}';
      } else {
        *out += '?';
        *out += UIntToStr(static_cast<uint32_t>(v.type_id()));
      }
    }
  }

uint32_t Cache::Impl::InternLayerStack(
      std::shared_ptr<Layer> layer, const std::string &identifier,
      std::string *warn, std::string *err,
      const std::shared_ptr<const Value> &inherited_vars) {
    std::string key = identifier;
    if (inherited_vars) AppendExpressionVarsFingerprint(*inherited_vars, &key);
    auto it = stack_by_id.find(key);
    if (it != stack_by_id.end()) return it->second;

    LayerStack st;
    st.identifier = identifier;
    st.cache_key = key;
    std::set<std::string> visiting;
    visiting.insert(identifier);
    // Sublayer asset-path expressions evaluate against the STACK ROOT layer's
    // expressionVariables with the REFERENCING site's variables composed over
    // them (pxr: the root-most opinion wins; sublayers' own
    // expressionVariables do not feed sibling sublayer paths).
    Value sublayer_vars = layer->meta().expressionVariables;
    if (inherited_vars && inherited_vars->is_dictionary()) {
      sublayer_vars =
          ComposeExpressionVariables(sublayer_vars, *inherited_vars);
    }
    if (!AppendLayerAndSublayers(st, layer, identifier, &visiting, 0,
                                 LayerOffset{}, sublayer_vars, warn, err)) {
      return kInvalidStack;
    }
    Value vars = Value::MakeDictionary();
    for (size_t i = st.layers.size(); i-- > 0;) {
      vars = ComposeExpressionVariables(
          vars, st.layers[i]->meta().expressionVariables);
    }
    st.expression_variables = std::move(vars);
    BuildStackRelocates(&st);

    uint32_t idx = static_cast<uint32_t>(layer_stacks.size());
    layer_stacks.push_back(std::move(st));
    stack_by_id.emplace(std::move(key), idx);
    return idx;
  }

void Cache::Impl::BuildStackRelocates(LayerStack *st) {
    std::vector<std::pair<std::string, std::string>> candidates;
    std::set<std::pair<std::string, std::string>> seen_edit;
    const auto add_candidate = [&](const std::pair<std::string, std::string> &r) {
      // A layer can be reached along two sublayer paths (so it appears in
      // `layers` twice), and the same edit may be authored in a layer and one of
      // its sublayers. Deduping IDENTICAL (src, dst) pairs keeps the pairwise
      // conflict scan below from invalidating an edit against its own copy
      // (a.second == b.second would else fire on the duplicate).
      if (seen_edit.insert(r).second) candidates.push_back(r);
    };
    for (const auto &lp : st->layers) {
      for (const auto &r : lp->meta().relocates) add_candidate(r);
      for (const PrimSpec &ps : lp->prims()) {
        for (const auto &r : ps.meta().relocates()) add_candidate(r);
      }
    }
    if (candidates.empty()) return;

    auto strict_descendant = [](const std::string &path,
                                const std::string &ancestor) {
      return path.size() > ancestor.size() &&
             path.compare(0, ancestor.size(), ancestor) == 0 &&
             path[ancestor.size()] == '/';
    };
    std::vector<bool> invalid(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
      const std::string &src = candidates[i].first;
      const std::string &dst = candidates[i].second;
      const Path sp(src);
      // Invalid authored relocates are composition errors but contribute no
      // namespace edit: identity, ancestor/descendant moves, and root-prim
      // moves are all ignored by Pcp. Applying them would delete valid subtrees
      // (the supplemental ErrorInvalidAuthoredRelocates cases). An EMPTY
      // target is NOT invalid: `</X>: <>` deletes the prim (RelocateToNone).
      if (src.empty() || src == dst ||
          (!dst.empty() &&
           (IsPathAtOrUnder(src, dst) || IsPathAtOrUnder(dst, src))) ||
          sp.parent().str() == "/") {
        invalid[i] = true;
      }
      // pxr only relocates prims introduced ACROSS a composition arc: a
      // source authored purely locally (a spec exists in this stack and no
      // ancestor-or-self spec carries any arc) cannot be relocated — the edit
      // is dropped and the local spec composes at its original path
      // (relocates_basic; "Cannot relocate a prim defined in the local layer
      // stack").
      if (!invalid[i]) {
        bool has_spec = false;
        bool has_arc = false;
        for (const auto &lp : st->layers) {
          // Walk STRICT ancestors: an arc on the source spec itself does not
          // make the prim arc-introduced (pxr still calls it local).
          std::string cur = src;
          for (;;) {
            if (const PrimSpec *psp = lp->prim_at_path(cur)) {
              if (cur == src) {
                has_spec = true;
                const size_t up = cur.rfind('/');
                if (up == std::string::npos || up == 0) break;
                cur.resize(up);
                continue;  // self arcs do not count
              }
              const PrimSpecMeta &m = psp->meta();
              const ArcListOpEdits *ae = m.arc_edits();
              if (!m.references.empty() || !m.payloads.empty() ||
                  !m.inherits.empty() || !m.specializes.empty() ||
                  !m.variantSets().empty() ||
                  (ae && (ae->references.has_authored_opinion() ||
                          ae->payloads.has_authored_opinion() ||
                          ae->inherits.has_authored_opinion() ||
                          ae->specializes.has_authored_opinion()))) {
                has_arc = true;
                break;
              }
            }
            const size_t slash = cur.rfind('/');
            if (slash == std::string::npos || slash == 0) break;
            cur.resize(slash);
          }
          if (has_arc) break;
        }
        if (has_spec && !has_arc) invalid[i] = true;
      }
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
      for (size_t j = i + 1; j < candidates.size(); ++j) {
        const auto &a = candidates[i];
        const auto &b = candidates[j];
        // Two sources cannot claim the same target; and a relocate chain whose
        // target is another relocate's source invalidates both opinions.
        // (Empty targets are DELETES: any number may coexist.)
        if ((!a.second.empty() && a.second == b.second) ||
            a.second == b.first || b.second == a.first) {
          invalid[i] = true;
          invalid[j] = true;
        }
        // A source authored beneath another source is expressed in the
        // prohibited pre-relocation namespace. Likewise a target beneath a
        // different source would itself be ancestrally relocated.
        if (strict_descendant(a.first, b.first) ||
            strict_descendant(a.second, b.first)) invalid[i] = true;
        if (strict_descendant(b.first, a.first) ||
            strict_descendant(b.second, a.first)) invalid[j] = true;
      }
    }
    // A relocate may be authored in a namespace an ANCESTRAL relocate already
    // produced (`/A -> /W`, then `/W/B -> /W/X`). `raw_path` normalizes such a
    // path back to the pre-ancestral-relocation ("raw") namespace (`/W/B` ->
    // `/A/B`). Used ONLY for `src_to_dst` below (see the split explanation).
    auto raw_path = [&](std::string path) {
      for (size_t pass = 0; pass < candidates.size(); ++pass) {
        size_t best = candidates.size();
        size_t best_len = 0;
        for (size_t k = 0; k < candidates.size(); ++k) {
          if (invalid[k]) continue;
          const std::string &target = candidates[k].second;
          if (target.empty()) continue;  // deletion edit: no arrival namespace
          if ((path == target || strict_descendant(path, target)) &&
              target.size() > best_len) {
            best = k;
            best_len = target.size();
          }
        }
        if (best == candidates.size()) break;
        path = candidates[best].first + path.substr(best_len);
      }
      return path;
    };

    // Two consumers need DIFFERENT keyings, so key each table for its consumer:
    //
    //  * `src_to_dst` (WithStackRelocates relationship/connection target remap,
    //    PathInRelocateSource) uses the RAW source. WithStackRelocates adds one
    //    rename per relocate to a single mapping applied in ONE pass; keying by
    //    the raw source pre-chains ancestral relocates (raw target -> FINAL
    //    dst), so a relationship authored in the pre-relocation namespace remaps
    //    correctly through a chain (BasicRelocateToAnimInterface: PathRig/Path/
    //    Anim -> Anim/Path/AnimScope in one hop).
    //
    //  * `departed` / `arrivals` (ComposeChildNames, SourcesForRelocateSource)
    //    use the COMPOSED (as-authored) path. The source's ancestors exist in
    //    the composed namespace as arrivals of the ancestral relocate, carrying
    //    both the relocated content AND any `over` opinions at the post-
    //    relocation path (TrickyMultipleRelocations2 `over Model_2 { Rig }`); the
    //    composed prim's departed children are keyed by their composed parent.
    StackRelocates &rel = st->relocates;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (invalid[i]) continue;
      const std::string &src = candidates[i].first;  // composed (as-authored)
      const std::string &dst = candidates[i].second;
      const Path sp(src), dp(dst);
      rel.src_to_dst[raw_path(src)] = dst;
      rel.departed[sp.parent().str()].insert(sp.name());
      if (dst.empty()) continue;  // deletion: departed only, no arrival
                                  // (and no target rename — pxr keeps
                                  // targets to deleted paths as authored).
      StackRelocates::Arrival arrival;
      arrival.name = dp.name();
      arrival.src_site = src;  // composed: SourcesForRelocateSource walks it
      arrival.dst_site = dst;
      rel.arrivals[dp.parent().str()].push_back(std::move(arrival));
    }
  }

bool Cache::Impl::AppendLayerAndSublayers(LayerStack &st, std::shared_ptr<Layer> layer,
                               const std::string &identifier,
                               std::set<std::string> *visiting,
                               uint32_t depth, const LayerOffset &cum_offset,
                               const Value &expr_vars,
                               std::string *warn, std::string *err) {
    if (!layer) return false;
    if (depth > options.max_depth) {
      AddIssue(ErrorCode::MaxDepthExceeded, identifier,
               "Sublayer max depth exceeded at: " + identifier, err);
      return false;
    }
    st.layers.push_back(layer);
    st.layer_identifiers.push_back(identifier);
    st.layer_offsets.push_back(cum_offset);
    // Anchor = the referencing layer's FILE path; AssetResolver::Resolve takes
    // its directory internally. (Passing a pre-stripped dir double-strips and
    // breaks deep relative paths like `../../Materials/x.usd`.)
    const std::string &anchor = identifier;
    const auto &sub_offsets = layer->meta().subLayerOffsets;
    for (size_t sub_i = 0; sub_i < layer->meta().subLayers.size(); ++sub_i) {
      std::string sub = layer->meta().subLayers[sub_i];
      if (!sub.empty() && sub.front() == '`' &&
          options.expression_variable_policy !=
              ExpressionVariablePolicy::Disabled) {
        const ExpressionEvaluation expression =
            EvaluateAssetPathExpression(sub, expr_vars);
        if (expression.is_expression && !expression.success) {
          AddIssue(ErrorCode::ExpressionVariableError, sub, expression.error,
                   options.expression_variable_policy ==
                           ExpressionVariablePolicy::RequireResolved
                       ? err
                       : warn);
          continue;  // the sublayer contributes nothing
        }
        if (expression.is_none) continue;  // no opinion
        if (expression.success) sub = expression.value;
      }
      // Cumulative offset for this sublayer: parent's offset composed with the
      // authored per-sublayer (offset = ..; scale = ..).
      LayerOffset sub_cum = cum_offset;
      if (sub_i < sub_offsets.size()) {
        double offset = sub_offsets[sub_i].first;
        double scale = sub_offsets[sub_i].second;
        if (!std::isfinite(offset) || !std::isfinite(scale) ||
            !(scale > 0.0)) {
          AddIssue(ErrorCode::InvalidReferenceOffset, sub,
                   "Invalid sublayer offset; using identity mapping", err);
          offset = 0.0;
          scale = 1.0;
        }
        sub_cum = cum_offset.Compose(LayerOffset{offset, scale});
      }
      // timeCodesPerSecond auto-scale (pxr parity): a sublayer authored at a
      // different tcps than its PARENT layer is scaled underneath the
      // authored offset so its samples land at the same wall-clock time.
      // Effective tcps falls back framesPerSecond -> 24 like
      // SdfLayer::GetTimeCodesPerSecond. Applied after the sublayer is
      // loaded (below) since it needs the sublayer's metadata.
      const std::string sub_id = resolver->ResolvePath(
          sub, anchor, !options.strict_aousd_conformance);
      if (sub_id.empty()) {
        AddIssue(ErrorCode::InvalidAssetPath, sub,
                 "Failed to resolve sublayer: " + sub, err);
        if (options.error_when_asset_not_found) return false;
        continue;
      }
      if (visiting && visiting->count(sub_id)) {
        // pxr warns and breaks the cycle, composing the rest — failing the
        // whole load here dropped otherwise-valid scenes.
        AddIssue(ErrorCode::SublayerCycle, sub_id,
                 "Sublayer cycle detected (skipped): " + sub_id, warn);
        continue;
      }
      LayerLoadOptions load_opts;
      load_opts.max_memory = options.max_layer_memory;
      load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
      load_opts.usdc_use_mmap = options.usdc_use_mmap;
      load_opts.usda_parse_options = options.usda_parse_options;
      load_opts.strict_aousd_conformance = options.strict_aousd_conformance;
      std::shared_ptr<Layer> sl =
          reg_->GetOrLoad(*resolver, sub, anchor, warn, err, load_opts);
      if (sl) {
        const double parent_tcps = layer->effective_timeCodesPerSecond();
        const double sub_tcps = sl->effective_timeCodesPerSecond();
        if (parent_tcps != sub_tcps && sub_tcps > 0.0 &&
            std::isfinite(parent_tcps) && std::isfinite(sub_tcps)) {
          sub_cum = sub_cum.Compose(LayerOffset{0.0, parent_tcps / sub_tcps});
        }
        if (visiting) visiting->insert(sub_id);
        if (!AppendLayerAndSublayers(st, sl, sub_id, visiting, depth + 1,
                                     sub_cum, expr_vars, warn, err)) {
          if (visiting) visiting->erase(sub_id);
          return false;
        }
        if (visiting) visiting->erase(sub_id);
      } else if (options.error_when_asset_not_found) {
        return false;
      }
    }
    return true;
  }

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
