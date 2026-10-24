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
  // Per-stack negative-lookup filter, index-aligned with spec_cache_by_stack_.
  // Shares its invalidation (cleared wherever the spec cache is cleared) and is
  // built lazily once the stack has seen enough misses to amortize its union
  // scan (see Specs()).
  mutable std::deque<PathHashFilter> path_filter_by_stack_;
  mutable size_t path_filter_bits_ = 0;  // total filter budget in use
  mutable size_t prof_path_filter_built_ = 0;
  mutable size_t prof_path_filter_disabled_ = 0;
  mutable size_t prof_path_filter_skips_ = 0;
  mutable size_t prof_path_filter_paths_ = 0;
  mutable size_t prof_path_filter_build_ns_ = 0;
  // Shared empty result for sites with no authored spec. Most composed prims
  // do not exist in most contributing layers, and caching a negative result
  // would store one key copy per (stack, site) for nothing.
  mutable std::vector<SpecRef> empty_specs_;

  const std::vector<SpecRef> &Specs(uint32_t stack_idx,
                                    const std::string &site) const ;
  // Build (or disable) the per-stack path filter from `layer_stacks[stack_idx]`.
  void BuildPathHashFilter(uint32_t stack_idx) const ;

  // Specs() with a per-Src memo: the 2-3 read-side spec lookups per composed prim
  // (ComposeChildNames, IsInstanceable, instance-key, ComposeOpinions) all key on
  // the SAME (stack_idx, site) -> cache the resolved vector pointer in the Src so
  // only the first pays the site-string hash. The memo self-resets on any Src
  // copy/move (see SpecsMemo in cache.cc), so child-build/worker-seed/merge
  // copies can never carry a stale or cross-Impl pointer.
  const std::vector<SpecRef> &SpecsFor(const Src &s) const ;
  std::vector<SpecRef> FindSpecs(const LayerStack &st,
                                 const std::string &site) const ;


  bool AnyAuthors(const std::vector<Src> &srcs) const ;

  // --- payload policy -----------------------------------------------------

  bool ShouldLoadPayload(const std::string &root_prim_path,
                         const std::string &asset,
                         const PrimSpec &owner) ;

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
                        std::string *err = nullptr) ;

  // A selected variant plus the name of the set it was selected from (the set
  // name is part of the stable variant-content identity used for instancing).
  struct SelectedVariant {
    const std::string *set_name;
    const PrimVariantData *vd;
  };

  // The fallback option for a variantSet nothing selects: the first candidate
  // (in configured order) the set actually defines, or null. See
  // CompositionOptions::variant_fallbacks.
  const std::string *FallbackSelection(const PrimVariantSetData &vss) const ;

  // Resolve the selected variants for the variantSets defined on `spec`, using
  // the accumulated cross-source selection map. Returns one entry per selected
  // set.
  std::vector<SelectedVariant> SelectVariants(
      const PrimSpec &spec,
      const std::map<std::string, std::string> &sels) const ;

  // --- instancing ---------------------------------------------------------

  // Instanceable iff the COMPOSED `instanceable` value is true AND the prim
  // actually has a composition arc (more than just its own root opinions).
  // `instanceable` is ordinary boolean metadata: the STRONGEST authored opinion
  // wins (srcs are strong-first), so a local `instanceable = false` override on
  // a prim that references an instanceable=true prototype de-instances it
  // (instancing-001 ModifiedTree).
  bool IsInstanceableSources(const std::vector<Src> &srcs) const ;

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
  std::string ComputeInstanceKeyImpl(const std::vector<Src> &srcs) const ;

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
                       bool prefer_min = false) ;

  // Returns true when the prim is instanceable (and so was registered or
  // stashed); a prim that returns false cannot be in prototype_of.
  bool RegisterInstance(const std::string &prim_path,
                        const std::vector<Src> &srcs, bool prefer_min = false) ;
  // RegisterInstance for a caller that already evaluated
  // IsInstanceableSources(srcs) == true.
  bool RegisterInstanceKnownInstanceable(const std::string &prim_path,
                                         const std::vector<Src> &srcs) ;

  // Composed-layer index of each prim the structure walk registered as
  // instanceable (see BuildStage). Lets FlattenInstances* find prototypes
  // and members without a path->index map over all prims; a miss falls back
  // to building that map once.
  std::unordered_map<std::string, uint32_t> instance_prim_index_;

  // Retire one group: drop `prim_path` from it and, if that empties it or the
  // prim WAS the prototype, orphan the siblings and erase the group.
  // `it` must be valid; returns the iterator to use next.
  std::unordered_map<std::string, std::vector<std::string>>::iterator
  DropInstancingFromGroup(
      std::unordered_map<std::string, std::vector<std::string>>::iterator it,
      const std::string &prim_path) ;

  void DropInstancing(const std::string &prim_path) ;
