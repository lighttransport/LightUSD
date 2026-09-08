// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache spec lookup, payload policy, variants, and instancing.
// Included inside Cache::Impl.

  // --- spec lookup --------------------------------------------------------
  // SpecRef is defined in cache-internal.hh so Src can cache a pointer
  // to a Specs() result.

  // Memoized spec resolution. The same (stack, site) is resolved 3-5x per
  // composed prim (ExpandArcs, AnyAuthors, ComposeInto, ComputePrimIndex,
  // instance-key); each call walked every layer and parsed the site Path.
  //
  // Keyed per-stack (outer index = stack_idx) then by the `site` string via an
  // open-addressed StackSpecCache, so a lookup keys by the CALLER's existing
  // `site` string BY REFERENCE with no per-call key allocation. (The old
  // (stack_idx, site) struct key deep-copied the site string into the key on
  // every call — hits included.) Stable element addresses: the per-stack caches
  // live in a deque (growing it never relocates an in-use cache) and
  // StackSpecCache stores its values in a deque (append never moves), so a
  // recursive insert during expansion never dangles an outer reference.
  // Results depend only on layer contents, which change solely on
  // InvalidateLayer -> the cache is cleared there (Invalidate(prim) leaves
  // layer stacks intact, so it stays valid).
  mutable std::deque<StackSpecCache> spec_cache_by_stack_;

  const std::vector<SpecRef> &Specs(uint32_t stack_idx,
                                    const std::string &site) const {
    if (stack_idx >= spec_cache_by_stack_.size())
      spec_cache_by_stack_.resize(stack_idx + 1);
    auto &m = spec_cache_by_stack_[stack_idx];
    const uint64_t h = StackSpecCache::HashSite(site.data(), site.size());
    if (std::vector<SpecRef> *v = m.find(site, h)) return *v;
    std::vector<SpecRef> refs = FindSpecs(layer_stacks[stack_idx], site);
    return m.insert(site, h, std::move(refs));
  }

  // Specs() with a per-Src memo: the 2-3 read-side spec lookups per composed prim
  // (ComposeChildNames, IsInstanceable, instance-key, ComposeOpinions) all key on
  // the SAME (stack_idx, site) -> cache the resolved vector pointer in the Src so
  // only the first pays the site-string hash. The memo self-resets on any Src
  // copy/move (see SpecsMemo in cache.cc), so child-build/worker-seed/merge
  // copies can never carry a stale or cross-Impl pointer.
  const std::vector<SpecRef> &SpecsFor(const Src &s) const {
    if (s.specs_.p) return *s.specs_.p;
    const std::vector<SpecRef> &r = Specs(s.stack_idx, s.site);
    s.specs_.p = &r;
    return r;
  }
  std::vector<SpecRef> FindSpecs(const LayerStack &st,
                                 const std::string &site) const {
    std::vector<SpecRef> refs;
    Path p(site);
    for (size_t i = 0; i < st.layers.size(); ++i) {
      const auto &lp = st.layers[i];
      if (const PrimSpec *s = lp->prim_at_path(p)) {
        SpecRef r;
        r.spec = s;
        r.layer = lp.get();
        if (i < st.layer_identifiers.size()) r.layer_id = st.layer_identifiers[i];
        if (i < st.layer_offsets.size()) r.layer_offset = st.layer_offsets[i];
        refs.push_back(std::move(r));
      }
    }
    return refs;
  }

  const PrimSpec *FindSpec(const LayerStack &st, const std::string &site,
                           const Layer **out_layer) const {
    std::vector<SpecRef> refs = FindSpecs(st, site);
    if (!refs.empty()) {
      if (out_layer) *out_layer = refs[0].layer;
      return refs[0].spec;
    }
    return nullptr;
  }

  bool AnyAuthors(const std::vector<Src> &srcs) const {
    for (const Src &s : srcs) {
      if (!SpecsFor(s).empty()) return true;
    }
    return false;
  }

  // --- payload policy -----------------------------------------------------

  bool ShouldLoadPayload(const std::string &root_prim_path,
                         const std::string &asset,
                         const PrimSpec &owner) {
    auto selected = [&](bool load) {
      if (load && options.payload_load_callback) {
        options.payload_load_callback(Path(root_prim_path));
      }
      return load;
    };
    // Explicit load rules win; a path with no governing rule falls back to the
    // base policy (payload_policy callback, else the load_payloads flag).
    switch (load_rules_.GetEffect(root_prim_path)) {
      case LoadRules::Effect::All:
      case LoadRules::Effect::Only:
        return selected(true);
      case LoadRules::Effect::None:
        return false;
      case LoadRules::Effect::Default:
        break;
    }
    if (options.payload_policy_with_prim) {
      return selected(options.payload_policy_with_prim(
          Path(root_prim_path), asset, owner));
    }
    if (options.payload_policy) {
      return selected(options.payload_policy(Path(root_prim_path), asset));
    }
    return selected(options.load_payloads);
  }

  // --- variant selection --------------------------------------------------

  // Record `spec`'s variant selections into the accumulator (strong-first wins,
  // so a selection authored on a stronger source overrides a weaker one).
  // Backtick selection values are variable expressions and evaluate against
  // the recording source's composed expression variables; an expression that
  // fails (or evaluates to None) records no selection for that set.
  void RecordSelections(const PrimSpec &spec,
                        std::map<std::string, std::string> *sels,
                        const std::shared_ptr<const Value> &expr_vars = nullptr,
                        std::string *warn = nullptr,
                        std::string *err = nullptr) {
    const auto record = [&](const std::string &set, const std::string &sel) {
      if (!sel.empty() && sel.front() == '`' &&
          options.expression_variable_policy !=
              ExpressionVariablePolicy::Disabled) {
        const Value empty = Value::MakeDictionary();
        const ExpressionEvaluation expression = EvaluateAssetPathExpression(
            sel, expr_vars ? *expr_vars : empty);
        if (expression.is_expression && !expression.success) {
          AddIssue(ErrorCode::ExpressionVariableError, sel, expression.error,
                   options.expression_variable_policy ==
                           ExpressionVariablePolicy::RequireResolved
                       ? err
                       : warn);
          return;
        }
        if (expression.is_none) return;  // no opinion
        if (expression.success && expression.is_expression) {
          sels->emplace(set, expression.value);
          return;
        }
      }
      sels->emplace(set, sel);
    };
    // Per-set `selected` field FIRST: it is the authoritative per-set form (what
    // the crate reader sets for EVERY selected set, and what the Compositor's
    // ApplyVariants reads first). The legacy variantSelection string carries only
    // ONE set, so without this a MULTI-set selection loaded from USDC loses every
    // set after the first. (emplace keeps the first/strongest opinion per set.)
    for (const VariantSetData &vss : spec.meta().variantSets()) {
      if (!vss.selected.empty()) record(vss.name, vss.selected);
    }
    if (!spec.meta().variantSelection.empty()) {
      VariantSelection s =
          Compositor::ParseVariantSelection(spec.meta().variantSelection);
      if (!s.variant_set.empty()) record(s.variant_set, s.variant_name);
    }
    for (const auto &sel : spec.meta().variantSelections()) {
      record(sel.first, sel.second);
    }
  }

  // A selected variant plus the name of the set it was selected from (the set
  // name is part of the stable variant-content identity used for instancing).
  struct SelectedVariant {
    const std::string *set_name;
    const VariantData *vd;
  };

  // The fallback option for a variantSet nothing selects: the first candidate
  // (in configured order) the set actually defines, or null. See
  // CompositionOptions::variant_fallbacks.
  const std::string *FallbackSelection(const VariantSetData &vss) const {
    auto it = options.variant_fallbacks.find(vss.name);
    if (it == options.variant_fallbacks.end()) return nullptr;
    for (const std::string &candidate : it->second) {
      for (const VariantData &vd : vss.variants) {
        if (vd.name == candidate) return &vd.name;
      }
    }
    return nullptr;
  }

  // Resolve the selected variants for the variantSets defined on `spec`, using
  // the accumulated cross-source selection map. Returns one entry per selected
  // set.
  std::vector<SelectedVariant> SelectVariants(
      const PrimSpec &spec,
      const std::map<std::string, std::string> &sels) const {
    std::vector<SelectedVariant> out;
    for (const VariantSetData &vss : spec.meta().variantSets()) {
      auto sit = sels.find(vss.name);
      // No authored selection anywhere: fall back to a registered global default
      // (pxr's variant fallbacks; `standin -> render`). Consulted here only, so
      // an authored selection always wins and the fallback never poisons `sels`
      // (and thus never pre-empts a selection authored in a weaker layer).
      const std::string *chosen =
          sit != sels.end() ? &sit->second : FallbackSelection(vss);
      if (!chosen) continue;  // no selection for this set
      for (const VariantData &vd : vss.variants) {
        if (vd.name == *chosen) {
          out.push_back({&vss.name, &vd});
          break;
        }
      }
    }
    return out;
  }

  // --- instancing ---------------------------------------------------------

  // Instanceable iff the COMPOSED `instanceable` value is true AND the prim
  // actually has a composition arc (more than just its own root opinions).
  // `instanceable` is ordinary boolean metadata: the STRONGEST authored opinion
  // wins (srcs are strong-first), so a local `instanceable = false` override on
  // a prim that references an instanceable=true prototype de-instances it
  // (instancing-001 ModifiedTree).
  bool IsInstanceableSources(const std::vector<Src> &srcs) const {
    if (srcs.size() <= 1) return false;
    for (const Src &s : srcs) {
      for (const SpecRef &sr : SpecsFor(s)) {
        const PrimSpecMeta &m = sr.spec->meta();
        // `instanceable = true` is always an authored opinion (the field
        // defaults to false), so it counts even when the authoring path did
        // not stamp instanceable_authored (hand-built layers). Only an
        // explicit `instanceable = false` needs the flag to distinguish it
        // from the unset default.
        if (m.instanceable) return true;
        if (m.instanceable_authored) return false;  // explicit false override
      }
    }
    return false;
  }

  // Structural key: composed type + each MASTER-DEFINING arc's (kind, layer-id,
  // site, variant). Must be instance-INDEPENDENT so two instances of the same
  // asset share a prototype (pxr PcpInstanceKey).
  //
  // The root node (i==0) is excluded -- its site is the instance's own path.
  // ANCESTRAL / positional sources are also excluded: a deeply-referenced
  // instance accumulates sources whose `site` is the instance's path WITHIN an
  // ancestor asset (e.g. `/root/set/.../leaf_0069` vs `_0070`), carrying the
  // instance-LOCAL opinions (transform/placement) that live outside the
  // instancing boundary. Their mapping renames an ANCESTOR prefix (the arc has a
  // positional offset beyond where it was introduced). A master-defining arc
  // targets the prim directly, so the mapping renames from exactly this site
  // (asset-relative, identical across instances). Keep only the latter --
  // `NamespaceMapping::MapsFrom`.
  std::string ComputeInstanceKeyImpl(const std::vector<Src> &srcs) const {
    std::string key;
    for (const Src &s : srcs) {
      bool found_type = false;
      for (const SpecRef &sr : SpecsFor(s)) {
        if (!sr.spec->type_name().empty()) {
          key += "T:" + sr.spec->type_name() + ";";
          found_type = true;
          break;
        }
      }
      if (found_type) break;
    }
    for (size_t i = 1; i < srcs.size(); ++i) {
      const Src &s = srcs[i];
      // Only sources an arc targeted DIRECTLY identify the instance; ancestral/
      // positional ones (a child re-rooted under an arc) map from an ancestor
      // prefix. (A mapping can carry several pairs -- one per arc plus one per
      // relocate -- so this asks whether ANY of them starts at this site.)
      if (!Mapping(s.map_idx).MapsFrom(s.site)) continue;
      key += ArcTypeName(s.arc_kind);
      key += "|" + layer_stacks[s.stack_idx].identifier + "|" + s.site;
      if (s.variant) key += "|v:" + s.variant->name;
      // A non-identity layer offset bakes different sample times, so instances
      // with different offsets must not share a prototype.
      if (!s.offset.is_identity()) {
        key += "|o:" + std::to_string(s.offset.offset) + ":" +
               std::to_string(s.offset.scale);
      }
      key += ";";
    }
    return key;
  }

  // Prototype bookkeeping for a prim that is an instance with key `ik`.
  //
  // prefer_min == false (BuildStage): the first prim registered for a key
  // becomes its prototype. BuildStage visits prims in deterministic namespace
  // order and decides instance-vs-prototype in a single pass, so "first wins"
  // is already order-independent there.
  //
  // prefer_min == true (per-prim ComputePrimIndex cache and the parallel
  // merge): the prototype is the lexicographically-smallest member path, so the
  // cached prototype/instance grouping is identical regardless of the order --
  // or the threads -- in which prims were computed (Phase 9 F5). A smaller path
  // arriving later demotes the previous prototype and re-points the group.
  void AssignPrototype(const std::string &prim_path, const std::string &ik,
                       bool prefer_min = false) {
    auto pit = prototype_by_key.find(ik);
    if (pit == prototype_by_key.end()) {
      prototype_by_key[ik] = prim_path;  // first of its key seeds the prototype
      prototype_of[prim_path] = prim_path;
      instances_by_prototype[prim_path].push_back(prim_path);
      instance_key_order.push_back(ik);
      return;
    }
    const std::string old_proto = pit->second;
    if (prim_path == old_proto) return;  // re-registering the prototype itself

    // Membership test is O(1) via the prototype_of reverse index, which the
    // invariant "P is in Q's group vector <=> prototype_of[P] == Q" keeps in
    // sync (see DropInstancing). The old linear std::find over the group made
    // registering N instances of one prototype O(N^2) -- minutes on 100k-instance
    // scenes (e.g. a forest of one tree asset).
    const auto poit = prototype_of.find(prim_path);
    const bool in_group =
        poit != prototype_of.end() && poit->second == old_proto;

    if (!prefer_min || old_proto < prim_path) {
      // prim_path is a (non-prototype) instance of the existing prototype.
      if (!in_group) {
        prototype_of[prim_path] = old_proto;
        instances_by_prototype[old_proto].push_back(prim_path);
      }
      return;
    }

    // prefer_min && prim_path < old_proto: prim_path becomes the new prototype.
    // Demote old_proto and re-point the whole group under the new prototype.
    pit->second = prim_path;
    std::vector<std::string> members;
    auto oit = instances_by_prototype.find(old_proto);
    if (oit != instances_by_prototype.end()) {
      members = std::move(oit->second);
      instances_by_prototype.erase(oit);
    }
    if (!in_group) members.push_back(prim_path);
    for (const std::string &m : members) prototype_of[m] = prim_path;
    instances_by_prototype[prim_path] = std::move(members);
  }

  void RegisterInstance(const std::string &prim_path,
                        const std::vector<Src> &srcs, bool prefer_min = false) {
    if (!options.detect_instances || !IsInstanceableSources(srcs)) return;
    const std::string ik = ComputeInstanceKeyImpl(srcs);
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (defer_instances_) {
      // Worker mode: stash for the deterministic input-order merge.
      pending_instance_[prim_path] = {true, ik};
      return;
    }
#endif
    AssignPrototype(prim_path, ik, prefer_min);
  }

  // Retire one group: drop `prim_path` from it and, if that empties it or the
  // prim WAS the prototype, orphan the siblings and erase the group.
  // `it` must be valid; returns the iterator to use next.
  std::unordered_map<std::string, std::vector<std::string>>::iterator
  DropInstancingFromGroup(
      std::unordered_map<std::string, std::vector<std::string>>::iterator it,
      const std::string &prim_path) {
    auto &v = it->second;
    v.erase(std::remove(v.begin(), v.end(), prim_path), v.end());
    const bool was_prototype = (it->first == prim_path);
    if (!was_prototype && !v.empty()) return ++it;
    // The group's prototype is gone: orphan the remaining siblings so their
    // prototype_of doesn't dangle to a dropped prototype. They re-group on
    // the next recomposition. (Without this, IsInstance/GetPrototype could
    // report a removed prototype after invalidating its subtree.)
    for (const std::string &m : v) prototype_of.erase(m);
    for (auto kit = prototype_by_key.begin(); kit != prototype_by_key.end();) {
      if (kit->second == it->first) {
        // instance_key_order mirrors prototype_by_key's keys; drop the entry
        // here too or it grows without bound and skews the reverse scan the
        // parallel warm path uses to pick prototypes.
        instance_key_order.erase(std::remove(instance_key_order.begin(),
                                             instance_key_order.end(),
                                             kit->first),
                                 instance_key_order.end());
        kit = prototype_by_key.erase(kit);
      } else {
        ++kit;
      }
    }
    return instances_by_prototype.erase(it);
  }

  void DropInstancing(const std::string &prim_path) {
    // Targeted: prototype_of is the reverse index, so the prim's own group (and
    // its own group if it IS a prototype) are the only ones that can mention
    // it. Walking EVERY prototype with an O(instances) std::remove made
    // invalidating a K-prim subtree O(K * prototypes * instances) -- a
    // multi-second stall on kitbashed scenes.
    auto pit = prototype_of.find(prim_path);
    const bool has_proto = pit != prototype_of.end();
    const std::string proto = has_proto ? pit->second : std::string();
    prototype_of.erase(prim_path);

    if (has_proto || instances_by_prototype.count(prim_path)) {
      if (has_proto && proto != prim_path) {
        auto git = instances_by_prototype.find(proto);
        if (git != instances_by_prototype.end()) {
          DropInstancingFromGroup(git, prim_path);
        }
      }
      auto sit = instances_by_prototype.find(prim_path);
      if (sit != instances_by_prototype.end()) {
        DropInstancingFromGroup(sit, prim_path);
      }
      return;
    }

    // No reverse-index entry: fall back to the exhaustive sweep so a stale
    // membership can never be left behind.
    for (auto it = instances_by_prototype.begin();
         it != instances_by_prototype.end();) {
      auto &v = it->second;
      v.erase(std::remove(v.begin(), v.end(), prim_path), v.end());
      const bool was_prototype = (it->first == prim_path);
      if (was_prototype || v.empty()) {
        // The group's prototype is gone: orphan the remaining siblings so their
        // prototype_of doesn't dangle to a dropped prototype. They re-group on
        // the next recomposition. (Without this, IsInstance/GetPrototype could
        // report a removed prototype after invalidating its subtree.)
        for (const std::string &m : v) prototype_of.erase(m);
        for (auto kit = prototype_by_key.begin(); kit != prototype_by_key.end();) {
          if (kit->second == it->first) {
            instance_key_order.erase(std::remove(instance_key_order.begin(),
                                                 instance_key_order.end(),
                                                 kit->first),
                                     instance_key_order.end());
            kit = prototype_by_key.erase(kit);
          } else {
            ++kit;
          }
        }
        it = instances_by_prototype.erase(it);
      } else {
        ++it;
      }
    }
  }
