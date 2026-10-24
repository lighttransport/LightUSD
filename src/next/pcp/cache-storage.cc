// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "cache-internal.hh"

namespace lightusd {
namespace next {
namespace pcp {

Cache::Impl::Impl() = default;
Cache::Impl::~Impl() {
#if defined(LIGHTUSD_ENABLE_THREAD)
  // Workers borrow the main registry. Only its owner may retire source layers,
  // and explicit single-threaded composition keeps single-threaded destruction.
  if (reg_ != &registry || options.num_threads == 1) return;
  auto layers = registry.TakeFileLayersForRelease();
  size_t prims = 0;
  for (const auto& layer : layers) prims = safe::saturating_add(prims, layer->prim_count());
  if (layers.size() < 2 || prims < 65536) return;

  // Every source remains pinned until cache objects that borrow it are gone.
  // Returned Stage Values retain their own atomic/shared source ownership.
  index_cache.clear();
  composed_cache_.clear();
  sources_cache.clear();
  spec_cache_by_stack_.clear();
  arc_target_memo_.clear();
  layer_stacks.clear();
  root_layer.reset();
  registry.Clear();  // preloaded/custom-reader layers retain serial teardown

  // Large layers first: a few district layers dominate the work. Bound the
  // allocator contention and worker stacks rather than using all SMT threads.
  std::sort(layers.begin(), layers.end(), [](const auto& a, const auto& b) {
    return a->prim_count() > b->prim_count();
  });
  int nt = options.num_threads;
  if (nt <= 0) nt = static_cast<int>(std::thread::hardware_concurrency());
  TaskArena arena(static_cast<size_t>(std::max(1, std::min(nt, 8))));
  arena.Run(layers.size(), [&](size_t i) { layers[i].reset(); });
#endif
}

}  // namespace pcp
}  // namespace next
}  // namespace lightusd

namespace lightusd { namespace next { namespace pcp {

auto Cache::Impl::Specs(uint32_t stack_idx,
                                    const std::string &site) const -> const std::vector<SpecRef> & {
    if (stack_idx >= spec_cache_by_stack_.size())
      spec_cache_by_stack_.resize(stack_idx + 1);
    auto &m = spec_cache_by_stack_[stack_idx];
    const uint64_t h = StackSpecCache::HashSite(site.data(), site.size());
    if (std::vector<SpecRef> *v = m.find(site, h)) return *v;
    // Not memoized. Before walking every layer, let the per-stack path filter
    // answer the common "this stack authors nothing at this site" case with one
    // bit probe. FindSpecs() alone accounts for 10.7-14.1 s of ExpandArcs on a
    // full-LOD Caldera district, 85.6% of it negative.
    if (stack_idx >= path_filter_by_stack_.size())
      path_filter_by_stack_.resize(stack_idx + 1);
    PathHashFilter &filter = path_filter_by_stack_[stack_idx];
    if (!filter.ready) {
      if (filter.misses != UINT32_MAX) ++filter.misses;
      if (filter.misses == 1) {
        // Amortize the one-time union scan: inserting `paths` entries costs
        // about `paths` path hashes, and each skipped negative lookup saves one
        // path hash per layer. Delay the build until roughly `paths / (8 *
        // layers)` misses, so a large layer probed only a few times never pays
        // for a filter it will not amortize.
        size_t paths = 0;
        const LayerStack &st = layer_stacks[stack_idx];
        for (const auto &lp : st.layers) paths += lp->prim_count();
        const size_t layers = st.layers.size();
        if (layers != 0) {
          const size_t denom = layers * 8;
          size_t need = (paths + denom - 1) / denom;
          if (need > 0x3fffffffu) need = 0x3fffffffu;
          if (need > filter.build_after)
            filter.build_after = static_cast<uint32_t>(need);
        }
      }
      if (filter.misses >= filter.build_after)
        BuildPathHashFilter(stack_idx);
    }
    if (!filter.MightContain(h)) {
      ++prof_path_filter_skips_;
      return empty_specs_;
    }
    std::vector<SpecRef> refs = FindSpecs(layer_stacks[stack_idx], site);
    if (refs.empty()) {
      // Negative result: share one empty vector instead of caching a key copy
      // per site. Callers only read the result, and a memo pointing here stays
      // valid for the lifetime of this Impl.
      return empty_specs_;
    }
    return m.insert(site, h, std::move(refs));
  }

auto Cache::Impl::BuildPathHashFilter(uint32_t stack_idx) const -> void {
    PathHashFilter &filter = path_filter_by_stack_[stack_idx];
    if (filter.ready) return;
    const LayerStack &st = layer_stacks[stack_idx];
    size_t paths = 0;
    for (const auto &lp : st.layers) paths += lp->prim_count();
    if (st.layers.empty() || paths == 0) {
      filter.Disable();
      ++prof_path_filter_disabled_;
      return;
    }
    const size_t bits = PathHashFilter::BitsFor(paths);
    if (path_filter_bits_ + bits > PathHashFilter::kMaxTotalBits) {
      // Budget exhausted (many distinct hot stacks): keep exact lookups for
      // this one rather than growing the cache without bound.
      filter.Disable();
      ++prof_path_filter_disabled_;
      return;
    }
    const auto build_t0 = std::chrono::steady_clock::now();
    path_filter_bits_ += bits;
    filter.Reset(bits);
    for (const auto &lp : st.layers) {
      for (const PrimSpec &p : lp->prims()) {
        const std::string &s = p.path().str();
        filter.Add(StackSpecCache::HashSite(s.data(), s.size()));
      }
    }
    prof_path_filter_build_ns_ +=
        static_cast<size_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - build_t0)
                                .count());
    prof_path_filter_paths_ += paths;
    ++prof_path_filter_built_;
  }

auto Cache::Impl::SpecsFor(const Src &s) const -> const std::vector<SpecRef> & {
    if (s.specs_.p) return *s.specs_.p;
    // Known-empty (stamped by ExpandArcs when the source was expanded, or by a
    // previous resolution here): no site hash, no probe.
    if (s.spec_state.v == SpecState::kNone) return empty_specs_;
    const std::vector<SpecRef> &r = Specs(s.stack_idx, s.site);
    // A parallel-warm worker (an Impl seeded with source_seed_cache) also
    // reads Srcs that live in the MAIN Impl's cache. A memo written there
    // would point into this worker's spec cache, which is discarded after the
    // merge, and would race with the other workers -- so only an Impl that
    // owns every Src it can see memoizes (the state byte too: same race).
    if (!source_seed_cache) {
      s.specs_.p = &r;
      s.spec_state.v = r.empty() ? SpecState::kNone : SpecState::kSome;
    }
    return r;
  }

auto Cache::Impl::FindSpecs(const LayerStack &st,
                                 const std::string &site) const -> std::vector<SpecRef> {
    std::vector<SpecRef> refs;
    for (size_t i = 0; i < st.layers.size(); ++i) {
      const auto &lp = st.layers[i];
      // The string overload looks the path up directly; building a Path here
      // would copy the site string for every composed prim.
      if (const PrimSpec *s = lp->prim_at_path(site)) {
        SpecRef r;
        r.spec = s;
        r.layer = lp.get();
        if (i < st.layer_identifiers.size())
          r.layer_identifier = &st.layer_identifiers[i];
        if (i < st.layer_offsets.size()) r.layer_offset = st.layer_offsets[i];
        refs.push_back(std::move(r));
      }
    }
    return refs;
  }



auto Cache::Impl::AnyAuthors(const std::vector<Src> &srcs) const -> bool {
    for (const Src &s : srcs) {
      if (!SpecsFor(s).empty()) return true;
    }
    return false;
  }

auto Cache::Impl::ShouldLoadPayload(const std::string &root_prim_path,
                         const std::string &asset,
                         const PrimSpec &owner) -> bool {
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

auto Cache::Impl::RecordSelections(const PrimSpec &spec,
                        std::map<std::string, std::string> *sels,
                        const std::shared_ptr<const Value> &expr_vars,
                        std::string *warn,
                        std::string *err) -> void {
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
    for (const PrimVariantSetData &vss : spec.meta().variantSets()) {
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

auto Cache::Impl::FallbackSelection(const PrimVariantSetData &vss) const -> const std::string * {
    auto it = options.variant_fallbacks.find(vss.name);
    if (it == options.variant_fallbacks.end()) return nullptr;
    for (const std::string &candidate : it->second) {
      for (const PrimVariantData &vd : vss.variants) {
        if (vd.name == candidate) return &vd.name;
      }
    }
    return nullptr;
  }

auto Cache::Impl::SelectVariants(
      const PrimSpec &spec,
      const std::map<std::string, std::string> &sels) const -> std::vector<SelectedVariant> {
    std::vector<SelectedVariant> out;
    for (const PrimVariantSetData &vss : spec.meta().variantSets()) {
      auto sit = sels.find(vss.name);
      // No authored selection anywhere: fall back to a registered global default
      // (pxr's variant fallbacks; `standin -> render`). Consulted here only, so
      // an authored selection always wins and the fallback never poisons `sels`
      // (and thus never pre-empts a selection authored in a weaker layer).
      const std::string *chosen =
          sit != sels.end() ? &sit->second : FallbackSelection(vss);
      if (!chosen) continue;  // no selection for this set
      for (const PrimVariantData &vd : vss.variants) {
        if (vd.name == *chosen) {
          out.push_back({&vss.name, &vd});
          break;
        }
      }
    }
    return out;
  }

auto Cache::Impl::IsInstanceableSources(const std::vector<Src> &srcs) const -> bool {
    if (srcs.size() <= 1) return false;
    for (const Src &s : srcs) {
      for (const SpecRef &sr : SpecsFor(s)) {
        const PrimMetadata &m = sr.spec->meta();
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

auto Cache::Impl::ComputeInstanceKeyImpl(const std::vector<Src> &srcs) const -> std::string {
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

auto Cache::Impl::AssignPrototype(const std::string &prim_path, const std::string &ik,
                       bool prefer_min) -> void {
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

auto Cache::Impl::RegisterInstance(const std::string &prim_path,
                        const std::vector<Src> &srcs, bool prefer_min) -> bool {
    if (!options.detect_instances || !IsInstanceableSources(srcs)) return false;
    const std::string ik = ComputeInstanceKeyImpl(srcs);
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (defer_instances_) {
      // Worker mode: stash for the deterministic input-order merge.
      pending_instance_[prim_path] = {true, ik};
      return true;
    }
#endif
    AssignPrototype(prim_path, ik, prefer_min);
    return true;
  }

auto Cache::Impl::DropInstancingFromGroup(
      std::unordered_map<std::string, std::vector<std::string>>::iterator it,
      const std::string &prim_path) -> std::unordered_map<std::string, std::vector<std::string>>::iterator {
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

auto Cache::Impl::DropInstancing(const std::string &prim_path) -> void {
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

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::MergeWorkerIndex(Impl &w, const std::string &key) -> void {
    auto wit = w.index_cache.find(key);
    if (wit == w.index_cache.end()) return;
    if (index_cache.count(key)) return;
    std::unique_ptr<PrimIndex> idx = std::move(wit->second);

    std::unordered_map<uint32_t, uint32_t> stack_remap, path_remap;
    const size_t n = idx->GetNodes().size();
    for (size_t i = 0; i < n; ++i) {
      CompNode &node = idx->MutableNode(static_cast<uint16_t>(i));
      auto sr = stack_remap.find(node.layer_stack_idx);
      if (sr == stack_remap.end()) {
        uint32_t m = AdoptStack(w.layer_stacks[node.layer_stack_idx]);
        stack_remap.emplace(node.layer_stack_idx, m);
        node.layer_stack_idx = m;
      } else {
        node.layer_stack_idx = sr->second;
      }
      auto pr = path_remap.find(node.site_path_idx);
      if (pr == path_remap.end()) {
        uint32_t m = InternPath(w.path_table[node.site_path_idx]);
        path_remap.emplace(node.site_path_idx, m);
        node.site_path_idx = m;
      } else {
        node.site_path_idx = pr->second;
      }
    }
    idx->SetLayerStacks(&layer_stacks);
    idx->SetPathTable(&path_table, path_table.size());

    auto sit = w.index_to_sites.find(key);
    if (sit != w.index_to_sites.end()) {
      for (const Site &s : sit->second) site_to_indices[s].insert(key);
      index_to_sites[key] = sit->second;
    }

    index_cache.emplace(key, std::move(idx));

    if (options.detect_instances) {
      auto piit = w.pending_instance_.find(key);
      if (piit != w.pending_instance_.end() && piit->second.first) {
        AssignPrototype(key, piit->second.second, /*prefer_min=*/true);
      }
    }
  }

auto Cache::Impl::MergeSources(Impl &w, size_t seed_stack_count, size_t seed_pool_count) -> void {
    // Worker-local stack / mapping indices past the seed snapshot are adopted
    // lazily (only those a merged entry references), memoized in flat tables.
    std::vector<uint32_t> stack_remap(
        w.layer_stacks.size() > seed_stack_count
            ? w.layer_stacks.size() - seed_stack_count
            : 0,
        UINT32_MAX);
    auto remap = [&](uint32_t ws) -> uint32_t {
      if (ws < seed_stack_count) return ws;
      uint32_t &m = stack_remap[ws - seed_stack_count];
      if (m == UINT32_MAX) m = AdoptStack(w.layer_stacks[ws]);
      return m;
    };
    std::vector<uint32_t> map_remap(
        w.nm_pool_.size() > seed_pool_count ? w.nm_pool_.size() - seed_pool_count
                                            : 0,
        UINT32_MAX);
    auto remap_map = [&](uint32_t wm) -> uint32_t {
      if (wm < seed_pool_count) return wm;
      uint32_t &m = map_remap[wm - seed_pool_count];
      if (m == UINT32_MAX) m = InternMapping(w.nm_pool_[wm]);
      return m;
    };
    // arc_chain / arc_sites are shared by whole subtrees, so remap each
    // distinct one once; a chain entirely inside the seed snapshot is already
    // valid in the main tables and is kept as is.
    std::unordered_map<const ArcChain *, std::shared_ptr<const ArcChain>>
        chain_memo;
    using ArcSites = std::vector<std::pair<uint32_t, std::string>>;
    std::unordered_map<const ArcSites *, std::shared_ptr<const ArcSites>>
        sites_memo;
    auto remap_chain = [&](std::shared_ptr<const ArcChain> *chain) {
      auto it = chain_memo.find(chain->get());
      if (it == chain_memo.end()) {
        std::shared_ptr<const ArcChain> out = *chain;
        bool local = false;
        for (const auto &node : **chain) {
          if (node.first >= seed_stack_count || node.second >= seed_pool_count) {
            local = true;
            break;
          }
        }
        if (local) {
          ArcChain remapped;
          remapped.reserve((*chain)->size());
          for (const auto &node : **chain) {
            remapped.emplace_back(remap(node.first), remap_map(node.second));
          }
          out = std::make_shared<const ArcChain>(std::move(remapped));
        }
        it = chain_memo.emplace(chain->get(), std::move(out)).first;
      }
      *chain = it->second;
    };
    auto remap_sites = [&](std::shared_ptr<const ArcSites> *sites) {
      auto it = sites_memo.find(sites->get());
      if (it == sites_memo.end()) {
        std::shared_ptr<const ArcSites> out = *sites;
        bool local = false;
        for (const auto &hop : **sites) {
          if (hop.first >= seed_stack_count) {
            local = true;
            break;
          }
        }
        if (local) {
          auto remapped = std::make_shared<ArcSites>();
          remapped->reserve((*sites)->size());
          for (const auto &hop : **sites) {
            remapped->emplace_back(remap(hop.first), hop.second);
          }
          out = std::move(remapped);
        }
        it = sites_memo.emplace(sites->get(), std::move(out)).first;
      }
      *sites = it->second;
    };
    // One pre-size for the whole merge: growing this table by ~10M entries
    // through rehash() was a measurable part of the merge wall.
    sources_cache.reserve(sources_cache.size() + w.sources_cache.size());
    w.sources_cache.for_each([&](const std::string &key,
                                 std::vector<Src> &val) {
      // Only the composed (root-namespace) entries are adopted. Stack-qualified
      // intermediate keys ("@N:/path" / "!N:/path", built by SourcesForSite /
      // SourcesForRelocateSource) embed the WORKER's layer-stack index N, which
      // means something else in the main table -- copying them verbatim would
      // reinterpret them against the wrong stack. They are cheap to recompute
      // (the serial BuildStage repopulates any it needs), so skip them.
      if (!key.empty() && (key[0] == '@' || key[0] == '!')) {
        return;
      }
      // The worker cache is a local delta over the shared immutable seed, so
      // entries already present in the main cache (the seed, or the same
      // ancestor re-derived by another worker that took a donated subtree --
      // identical sources) are not copied back.
      bool created = false;
      std::vector<Src> &slot = sources_cache.get_or_create(key, &created);
      if (!created) return;
      // The worker Impl is discarded right after this merge, so steal its Src
      // vector instead of copying it. NOTE: a vector move is a buffer steal —
      // the elements' SpecsMemo self-reset never runs — so any worker-set
      // specs_ (pointing into the discarded worker's spec cache) must be
      // dropped explicitly below.
      std::vector<Src> &srcs = slot;
      srcs = std::move(val);
      for (Src &s : srcs) {
        s.stack_idx = remap(s.stack_idx);
        s.map_idx = remap_map(s.map_idx);
        s.specs_.p = nullptr;  // worker cache pointer; main re-resolves
        if (s.implied_anchor != UINT32_MAX) {
          s.implied_anchor = remap(s.implied_anchor);
        }
        // The implied-class arc chain also indexes the worker's tables.
        if (s.arc_chain && !s.arc_chain->empty()) remap_chain(&s.arc_chain);
        // So does the ancestral arc-target trail (stack, site) used to re-seed
        // cycle detection; a worker stack index is meaningless (or out of
        // range) in the main table.
        if (s.arc_sites && !s.arc_sites->empty()) remap_sites(&s.arc_sites);
      }
    });
  }

#endif

}}} // namespace lightusd::next::pcp

namespace lightusd { namespace next { namespace pcp {

auto cache_detail::StackSpecCache::insert(const std::string &site, uint64_t h,
                               std::vector<SpecRef> &&v) -> std::vector<SpecRef> & {
    if (slots_.empty() || (count_ + 1) * 2 > slots_.size()) rehash();
    const uint32_t vi = static_cast<uint32_t>(vals_.size());
    keys_.emplace_back(site);
    vals_.emplace_back(std::move(v));
    place(h, vi);
    ++count_;
    return vals_[vi];
  }

auto cache_detail::StackSpecCache::clear() -> void {
    slots_.clear();
    keys_.clear();
    vals_.clear();
    count_ = 0;
  }

auto cache_detail::StackSpecCache::take_values()
    -> std::deque<std::vector<SpecRef>> {
    std::deque<std::vector<SpecRef>> values;
    values.swap(vals_);
    std::vector<Slot>().swap(slots_);
    std::deque<std::string>().swap(keys_);
    count_ = 0;
    return values;
  }

namespace cache_detail {
size_t CompactOpinionSources(std::vector<Src> &sources) {
    const auto contributes = [](const Src &source) {
      // Match ComposeOpinions' variant-before-suppression ordering. An
      // unresolved memo is conservatively retained; callers normally prime all
      // non-variant, non-suppressed sources before retiring the lookup index.
      return source.variant ||
             (!source.suppress_site_specs &&
              source.spec_state.v != SpecState::kNone &&
              (!source.specs_.p || !source.specs_.p->empty()));
    };
    size_t keep = 0;
    for (const Src &source : sources) {
      if (contributes(source)) ++keep;
    }
    const size_t removed = sources.size() - keep;
    if (!removed) return 0;

    std::vector<Src> compact;
    compact.reserve(keep);
    for (Src &source : sources) {
      if (!contributes(source)) continue;
      const auto *memo = source.specs_.p;
      compact.push_back(std::move(source));
      // Src's normal move resets a cross-site/cross-Impl memo. This move keeps
      // the exact site and owning Impl, and its spec vectors remain pinned.
      compact.back().specs_.p = memo;
    }
    sources.swap(compact);
    return removed;
  }
}  // namespace cache_detail

auto cache_detail::StackSpecCache::place(uint64_t h, uint32_t vi) -> void {
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    while (slots_[i].hash != 0) i = (i + 1) & mask;
    slots_[i].hash = h;
    slots_[i].val_idx = vi;
  }

auto cache_detail::StackSpecCache::rehash() -> void {
    const size_t new_cap = slots_.empty() ? 16 : slots_.size() * 2;
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(new_cap, Slot{});
    for (const Slot &s : old) {
      if (s.hash != 0) place(s.hash, s.val_idx);
    }
  }

auto cache_detail::SrcCache::clear() -> void {  // full free (drops the path-string keys too)
    std::vector<Slot>().swap(slots_);
    std::deque<std::string>().swap(keys_);
    std::deque<std::vector<Src>>().swap(vals_);
    std::vector<uint8_t>().swap(live_);
    count_ = 0;
    occupied_ = 0;
  }

auto cache_detail::SrcCache::take_values() -> std::deque<std::vector<Src>> {
    std::deque<std::vector<Src>> values;
    values.swap(vals_);
    clear();
    return values;
  }

auto cache_detail::SrcCache::key_at(size_t index) const -> const std::string & {
    return (live_[index] & kSiteKey) ? vals_[index].front().site : keys_[index];
  }

auto cache_detail::SrcCache::own_key(size_t index) -> void {
    if (live_[index] & kSiteKey) {
      keys_[index] = vals_[index].front().site;
      live_[index] = kLive;
    }
  }

auto cache_detail::SrcCache::borrow_site_keys() -> size_t {
    size_t borrowed = 0;
    for (size_t i = 0; i < live_.size(); ++i) {
      if (live_[i] != kLive || vals_[i].empty() ||
          keys_[i] != vals_[i].front().site) continue;
      std::string().swap(keys_[i]);
      live_[i] = kLive | kSiteKey;
      ++borrowed;
    }
    return borrowed;
  }

auto cache_detail::SrcCache::find(const std::string &k) -> std::vector<Src> * {
    if (slots_.empty()) return nullptr;
    const uint64_t h = Hash(k);
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    for (;;) {
      const Slot &s = slots_[i];
      if (s.state == 0) return nullptr;
      if (s.state == 1 && s.hash == h) {
        const std::string &key = key_at(s.idx);
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          own_key(s.idx);
          return &vals_[s.idx];
        }
      }
      i = (i + 1) & mask;
    }
  }

auto cache_detail::SrcCache::find(const std::string &k) const -> const std::vector<Src> * {
    if (slots_.empty()) return nullptr;
    const uint64_t h = Hash(k);
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    for (;;) {
      const Slot &s = slots_[i];
      if (s.state == 0) return nullptr;
      if (s.state == 1 && s.hash == h) {
        const std::string &key = key_at(s.idx);
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          return &vals_[s.idx];
        }
      }
      i = (i + 1) & mask;
    }
  }

auto cache_detail::SrcCache::get_or_create(const std::string &k,
                                          bool *created) -> std::vector<Src> & {
    const uint64_t h = Hash(k);
    if (slots_.empty() || (occupied_ + 1) * 2 > slots_.size()) rehash();
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    size_t first_tomb = SIZE_MAX;
    for (;;) {
      Slot &s = slots_[i];
      if (s.state == 0) {
        const size_t at = (first_tomb == SIZE_MAX) ? i : first_tomb;
        if (created) *created = true;
        return emplace_at(at, h, k, /*was_tomb=*/slots_[at].state == 2);
      }
      if (s.state == 2) {
        if (first_tomb == SIZE_MAX) first_tomb = i;
      } else if (s.hash == h) {
        const std::string &key = key_at(s.idx);
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          own_key(s.idx);
          if (created) *created = false;
          return vals_[s.idx];
        }
      }
      i = (i + 1) & mask;
    }
  }

auto cache_detail::SrcCache::reserve(size_t expected) -> void {
    if (expected < 16) expected = 16;
    size_t want = 16;
    while (want < expected * 2) want <<= 1;
    if (want <= slots_.size()) return;
    // Grow to the requested size directly; rehash() would double step by step.
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(want, Slot{});
    occupied_ = 0;
    const size_t mask = want - 1;
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (!live_[vi]) continue;
      const uint64_t h = Hash(key_at(vi));
      size_t i = static_cast<size_t>(h) & mask;
      while (slots_[i].state != 0) i = (i + 1) & mask;
      slots_[i].hash = h;
      slots_[i].idx = vi;
      slots_[i].state = 1;
      ++occupied_;
    }
  }

auto cache_detail::SrcCache::emplace_if_absent(const std::string &k, std::vector<Src> &&v) -> void {
    if (contains(k)) return;
    get_or_create(k) = std::move(v);
  }

auto cache_detail::SrcCache::erase(const std::string &k) -> void {
    if (slots_.empty()) return;
    const uint64_t h = Hash(k);
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    for (;;) {
      Slot &s = slots_[i];
      if (s.state == 0) return;
      if (s.state == 1 && s.hash == h) {
        const std::string &key = key_at(s.idx);
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          live_[s.idx] = 0;
          std::string().swap(keys_[s.idx]);
          std::vector<Src>().swap(vals_[s.idx]);
          s.state = 2;  // tombstone: keeps the probe chain valid
          --count_;
          return;
        }
      }
      i = (i + 1) & mask;
    }
  }

auto cache_detail::SrcCache::emplace_at(size_t slot, uint64_t h, const std::string &k,
                               bool was_tomb) -> std::vector<Src> & {
    uint32_t vi;
    if (was_tomb) {
      vi = static_cast<uint32_t>(live_.size());
      for (uint32_t di = 0; di < live_.size(); ++di) {
        if (!live_[di]) { vi = di; break; }
      }
      if (vi < live_.size()) {
        keys_[vi] = k;
        vals_[vi].clear();
        live_[vi] = 1;
      } else {
        keys_.emplace_back(k);
        vals_.emplace_back();
        live_.push_back(1);
      }
    } else {
      vi = static_cast<uint32_t>(vals_.size());
      keys_.emplace_back(k);
      vals_.emplace_back();
      live_.push_back(1);
    }
    slots_[slot].hash = h;
    slots_[slot].idx = vi;
    slots_[slot].state = 1;
    ++count_;
    if (!was_tomb) ++occupied_;
    return vals_[vi];
  }

auto cache_detail::SrcCache::rehash() -> void {
    const size_t new_cap = slots_.empty() ? 16 : slots_.size() * 2;
    slots_.assign(new_cap, Slot{});
    occupied_ = 0;
    const size_t mask = new_cap - 1;
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (!live_[vi]) continue;
      const uint64_t h = Hash(key_at(vi));
      size_t i = static_cast<size_t>(h) & mask;
      while (slots_[i].state != 0) i = (i + 1) & mask;
      slots_[i].hash = h;
      slots_[i].idx = vi;
      slots_[i].state = 1;
      ++occupied_;
    }
  }

auto Cache::Impl::InternPath(const std::string &p) -> uint32_t {
    auto it = path_intern.find(p);
    if (it != path_intern.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(path_table.size());
    path_table.push_back(p);
    path_intern.emplace(p, idx);
    return idx;
  }

auto Cache::Impl::FindCachedSources(const std::string &key) const -> const std::vector<Src> * {
    if (const std::vector<Src> *local = sources_cache.find(key)) {
      return local;
    }
    if (source_seed_cache) {
      return source_seed_cache->find(key);
    }
    return nullptr;
  }

auto Cache::Impl::GetOrCreateCachedSources(const std::string &key) -> std::vector<Src> & {
    if (std::vector<Src> *local = sources_cache.find(key)) return *local;
    if (source_seed_cache) {
      if (const std::vector<Src> *seed = source_seed_cache->find(key)) {
        // This path is only used when a caller is about to populate or mutate
        // an entry that was found through the immutable seed.
        return sources_cache.get_or_create(key) = *seed;
      }
    }
    return sources_cache.get_or_create(key);
  }

auto Cache::Impl::InternMapping(NamespaceMapping m) -> uint32_t {
    if (m.is_identity()) return 0;
    uint32_t idx = static_cast<uint32_t>(nm_pool_.size());
    nm_pool_.push_back(std::move(m));
    return idx;
  }

}}} // namespace lightusd::next::pcp
