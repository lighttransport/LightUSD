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
    if (SpecReadOnlyStore *ro = SpecReadOnlyTLS()) {
      // Parallel structure walk: same result, no shared-state mutation.
      const uint64_t h = StackSpecCache::HashSite(site.data(), site.size());
      if (stack_idx < spec_cache_by_stack_.size()) {
        if (std::vector<SpecRef> *v = spec_cache_by_stack_[stack_idx].find(site, h))
          return *v;
      }
      if (stack_idx < path_filter_by_stack_.size() &&
          !path_filter_by_stack_[stack_idx].MightContain(h)) {
        return empty_specs_;
      }
      std::vector<SpecRef> refs = FindSpecs(layer_stacks[stack_idx], site);
      if (refs.empty()) return empty_specs_;
      ro->vals.push_back(std::move(refs));
      return ro->vals.back();
    }
    if (stack_idx >= spec_cache_by_stack_.size())
      spec_cache_by_stack_.resize(stack_idx + 1);
    auto &m = spec_cache_by_stack_[stack_idx];
    const uint64_t h = StackSpecCache::HashSite(site.data(), site.size());
    if (std::vector<SpecRef> *v = m.find(site, h)) return *v;
    // Not memoized. Before walking every layer, let the per-stack path filter
    // answer the common "this stack authors nothing at this site" case with one
    // bit probe. FindSpecs() alone accounts for 10.7-14.1 s of ExpandArcs on a
    // full-LOD benchmark scene district, 85.6% of it negative.
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
    if (!source_seed_cache && !SpecReadOnlyTLS()) {
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
      if (!s.offset().is_identity()) {
        key += "|o:" + std::to_string(s.offset().offset) + ":" +
               std::to_string(s.offset().scale);
      }
      key += ";";
    }
    return key;
  }

// After source warming, most composition sources are inert: their site has no
// spec anywhere in their layer stack (full-LOD benchmark scene: 90M of 104M sources,
// ~23 GB of Src vectors and site strings, held until the opinion fill
// compacts them). An inert source contributes no opinions, child names,
// activation or instanceability, and -- a layer cannot hold a spec below a
// site it has no spec at -- neither does anything derived from it, unless a
// mapping pair re-roots namespace at or below the site. The source warm drops
// them as it goes (ExpandWarmNode), so they never pile up next to the loaded
// layers and the composed stage. Kept, so every reader sees the same result:
//  - the first source (ComputeInstanceKeyImpl is positional about it);
//  - variant and relocation-anchor (suppress_site_specs) sources;
//  - unresolved or non-empty sources;
//  - sources whose mapping has a pair at or below the site (MapsFrom feeds
//    the instance key and the instance subtree filter; a deeper pair would
//    make a derived descendant MapsFrom).
// A list with a source in a stack with relocates is left whole (RelocationsAt
// reads departures for every source, and a departed child is derived from
// this list), and so is one with a composed child that has no cached list yet
// (a later walk would derive it from this list: ExpandList's dedup and
// ordering see the whole list). `children` are the list's composed child
// names, or null for a list nothing derives from through its whole source
// list (see ExpandWarmNode: an instance whose subtree is skipped). Spec
// states a copy reset (a warm worker's derived Srcs) are looked up.
bool Cache::Impl::IsInertSource(const Src &s) const {
    if (s.variant || s.suppress_site_specs) return false;
    if (s.spec_state.v == SpecState::kUnknown && !s.specs_.p) {
      if (!SpecsFor(s).empty()) return false;
    } else if (s.spec_state.v != SpecState::kNone &&
               (!s.specs_.p || !s.specs_.p->empty())) {
      return false;
    }
    for (const NamespaceMapping::Pair &pr : Mapping(s.map_idx).pairs) {
      if (NamespaceMapping::AtOrUnder(pr.first, s.site)) return false;
    }
    return true;
  }

size_t Cache::Impl::DropInertSources(const std::string &key,
                                     std::vector<Src> &srcs,
                                     const std::vector<std::string> *children) {
    if (srcs.size() < 2) return 0;
    std::vector<uint8_t> inert(srcs.size(), 0);
    size_t n_inert = 0;
    for (size_t i = 0; i < srcs.size(); ++i) {
      if (!layer_stacks[srcs[i].stack_idx].relocates.empty()) return 0;
      if (i != 0 && IsInertSource(srcs[i])) {
        inert[i] = 1;
        ++n_inert;
      }
    }
    if (!n_inert) return 0;
    std::string child_key = key;
    child_key += '/';
    const size_t base = child_key.size();
    for (const std::string &cn : children ? *children : std::vector<std::string>()) {
      child_key.resize(base);
      child_key += cn;
      if (!HasCachedSources(child_key)) return 0;
    }
    std::vector<Src> compact;
    compact.reserve(srcs.size() - n_inert);
    for (size_t i = 0; i < srcs.size(); ++i) {
      if (inert[i]) continue;
      // Same site, same Impl: the spec memo stays valid across the move.
      const auto *memo = srcs[i].specs_.p;
      compact.push_back(std::move(srcs[i]));
      compact.back().specs_.p = memo;
    }
    srcs.swap(compact);
    return n_inert;
  }

auto Cache::Impl::AssignPrototype(const std::string &prim_path, const std::string &ik,
                       bool prefer_min) -> void {
    auto pit = prototype_by_key.find(ik);
    if (pit == prototype_by_key.end()) {
      const PathStr node = PathNode(prim_path);
      prototype_by_key[ik] = node;  // first of its key seeds the prototype
      SetPrototypeOf(node, node);
      instances_by_prototype[prim_path].push_back(node);
      instance_key_order.push_back(ik);
      return;
    }
    const PathStr old_proto = pit->second;
    if (prim_path == old_proto) return;  // re-registering the prototype itself

    // Membership test is O(1) via the prototype_of reverse index, which the
    // invariant "P is in Q's group vector <=> prototype_of[P] == Q" keeps in
    // sync (see DropInstancing). The old linear std::find over the group made
    // registering N instances of one prototype O(N^2) -- minutes on 100k-instance
    // scenes (e.g. a forest of one tree asset).
    const auto poit = prototype_of.find(prim_path);
    const bool in_group =
        poit != prototype_of.end() && poit->second.proto == old_proto;
    const PathStr node =
        poit != prototype_of.end() ? poit->second.path : PathStr(prim_path);

    if (!prefer_min || old_proto.str() < prim_path) {
      // prim_path is a (non-prototype) instance of the existing prototype.
      if (!in_group) {
        SetPrototypeOf(node, old_proto);
        instances_by_prototype[old_proto.str()].push_back(node);
      }
      return;
    }

    // prefer_min && prim_path < old_proto: prim_path becomes the new prototype.
    // Demote old_proto and re-point the whole group under the new prototype.
    pit->second = node;
    std::vector<PathStr> members;
    auto oit = instances_by_prototype.find(old_proto.str());
    if (oit != instances_by_prototype.end()) {
      members = std::move(oit->second);
      instances_by_prototype.erase(oit);
    }
    if (!in_group) members.push_back(node);
    for (const PathStr &m : members) SetPrototypeOf(m, node);
    instances_by_prototype[prim_path] = std::move(members);
  }

auto Cache::Impl::RegisterInstanceKnownInstanceable(
    const std::string &prim_path, const std::vector<Src> &srcs) -> bool {
    if (!options.detect_instances) return false;
    const std::string ik = ComputeInstanceKeyImpl(srcs);
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (defer_instances_) {
      pending_instance_[prim_path] = {true, ik};
      return true;
    }
#endif
    AssignPrototype(prim_path, ik, false);
    return true;
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
      std::unordered_map<std::string, std::vector<PathStr>>::iterator it,
      const std::string &prim_path) -> std::unordered_map<std::string, std::vector<PathStr>>::iterator {
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
    const std::string proto = has_proto ? pit->second.proto.str() : std::string();
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
      // The worker is discarded after this merge and each of its mappings is
      // interned at most once (memoized here): move instead of copying.
      if (m == UINT32_MAX) m = InternMapping(std::move(w.nm_pool_[wm]));
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
    // One pre-size for the whole merge: growing this table by ~10M entries
    // through rehash() was a measurable part of the merge wall.
    sources_cache.reserve(sources_cache.size() + w.sources_cache.size());
    // Merge the worker's composed entries. Serial semantics (one pass in the
    // worker's index order): skip stack-qualified keys and keys the main cache
    // already has; move the rest in, remapping each Src's worker-local stack /
    // mapping indices -- adopting a local stack or mapping lazily at its first
    // reference, which fixes the main tables' order -- and its arc chains /
    // sites through per-pointer memos. That pass walked ~90 M cold Src records
    // on the all-district scene. Same result, in phases:
    //   1. (parallel) probe each key in the main cache (unchanged during the
    //      merge; a worker's keys are distinct) and record the first position
    //      at which each local stack / mapping is referenced;
    //   2. (serial) adopt them in first-reference order -> identical tables;
    //      build one remapped chain / sites object per distinct local pointer;
    //   3. (parallel) remap the Src records in place in the worker storage;
    //   4. (serial) move the keys and vectors into the main cache in order.
    // Stack-qualified intermediate keys ("@N:/path" / "!N:/path", built by
    // SourcesForSite / SourcesForRelocateSource) embed the WORKER's layer-stack
    // index N and are not adopted (the serial BuildStage recomputes any it
    // needs). Entries already present (the seed, or the same ancestor
    // re-derived by another worker that took a donated subtree -- identical
    // sources) are not copied back.
    std::vector<uint32_t> ent;
    std::vector<uint64_t> ent_hash;
    w.sources_cache.live_with_hash(&ent, &ent_hash);
    const size_t ne = ent.size();
    std::vector<uint8_t> take(ne, 0);
    const size_t nls = stack_remap.size(), nlm = map_remap.size();
    auto chain_local = [&](const ArcChain &c) {
      for (const auto &node : c)
        if (node.first >= seed_stack_count || node.second >= seed_pool_count)
          return true;
      return false;
    };
    auto sites_local = [&](const ArcSites &st) {
      for (const auto &hop : st)
        if (hop.first >= seed_stack_count) return true;
      return false;
    };
    size_t T = std::max<size_t>(1, std::min<size_t>(
        std::thread::hardware_concurrency(), 16));
    if (ne < 8192) T = 1;
    auto parallel = [&](const std::function<void(size_t, size_t, size_t)> &fn) {
      if (T == 1) {
        fn(0, 0, ne);
        return;
      }
      std::vector<std::thread> pool;
      pool.reserve(T - 1);
      for (size_t t = 1; t < T; ++t)
        pool.emplace_back([&fn, t, ne, this, T]() {
          (void)this;
          fn(t, ne * t / T, ne * (t + 1) / T);
        });
      fn(0, 0, ne / T);
      for (std::thread &th : pool) th.join();
    };
    struct FirstRefs {
      std::vector<uint64_t> stack, map;
      std::unordered_set<const ArcChain *> chains;
      std::unordered_set<const ArcSites *> sites;
    };
    std::vector<FirstRefs> refs(T);
    parallel([&](size_t t, size_t lo, size_t hi) {
      FirstRefs &fr = refs[t];
      fr.stack.assign(nls, UINT64_MAX);
      fr.map.assign(nlm, UINT64_MAX);
      for (size_t e = lo; e < hi; ++e) {
        const std::string &key = w.sources_cache.key_ref(ent[e]);
        if (!key.empty() && (key[0] == '@' || key[0] == '!')) continue;
        if (sources_cache.find_hashed(key, ent_hash[e])) continue;
        take[e] = 1;
        // Remap events in the serial pass's order within this entry.
        uint64_t pos = static_cast<uint64_t>(e) << 24;
        auto note_stack = [&](uint32_t ws) {
          if (ws >= seed_stack_count && ws - seed_stack_count < nls) {
            uint64_t &m = fr.stack[ws - seed_stack_count];
            if (pos < m) m = pos;
          }
          ++pos;
        };
        auto note_map = [&](uint32_t wm) {
          if (wm >= seed_pool_count && wm - seed_pool_count < nlm) {
            uint64_t &m = fr.map[wm - seed_pool_count];
            if (pos < m) m = pos;
          }
          ++pos;
        };
        for (const Src &s : w.sources_cache.vals_[ent[e]]) {
          note_stack(s.stack_idx);
          note_map(s.map_idx);
          if (s.implied_anchor != UINT32_MAX) note_stack(s.implied_anchor);
          if (s.arc_chain() && !s.arc_chain()->empty() && chain_local(*s.arc_chain())) {
            fr.chains.insert(s.arc_chain().get());
            for (const auto &node : *s.arc_chain()) {
              note_stack(node.first);
              note_map(node.second);
            }
          }
          if (s.arc_sites() && !s.arc_sites()->empty() && sites_local(*s.arc_sites())) {
            fr.sites.insert(s.arc_sites().get());
            for (const auto &hop : *s.arc_sites()) note_stack(hop.first);
          }
        }
      }
    });
    {
      std::vector<std::pair<uint64_t, uint32_t>> order;
      for (size_t i = 0; i < nls; ++i) {
        uint64_t m = UINT64_MAX;
        for (const FirstRefs &fr : refs) m = std::min(m, fr.stack[i]);
        if (m != UINT64_MAX) order.emplace_back(m, static_cast<uint32_t>(i));
      }
      std::sort(order.begin(), order.end());
      for (const auto &o : order)
        remap(static_cast<uint32_t>(seed_stack_count) + o.second);
      order.clear();
      for (size_t i = 0; i < nlm; ++i) {
        uint64_t m = UINT64_MAX;
        for (const FirstRefs &fr : refs) m = std::min(m, fr.map[i]);
        if (m != UINT64_MAX) order.emplace_back(m, static_cast<uint32_t>(i));
      }
      std::sort(order.begin(), order.end());
      for (const auto &o : order)
        remap_map(static_cast<uint32_t>(seed_pool_count) + o.second);
    }
    // Every local stack / mapping a taken entry references is adopted now, so
    // the remapped chain / sites objects are built in parallel (one per
    // distinct local pointer; a pointer seen by two slices is built twice and
    // the memo keeps one -- the copies are identical).
    auto rs = [&](uint32_t ws) {
      return ws < seed_stack_count ? ws : stack_remap[ws - seed_stack_count];
    };
    auto rm = [&](uint32_t wm) {
      return wm < seed_pool_count ? wm : map_remap[wm - seed_pool_count];
    };
    std::vector<std::vector<std::pair<const ArcChain *, std::shared_ptr<const ArcChain>>>>
        built_chains(T);
    std::vector<std::vector<std::pair<const ArcSites *, std::shared_ptr<const ArcSites>>>>
        built_sites(T);
    parallel([&](size_t t, size_t, size_t) {
      const FirstRefs &fr = refs[t];
      built_chains[t].reserve(fr.chains.size());
      for (const ArcChain *c : fr.chains) {
        ArcChain remapped;
        remapped.reserve(c->size());
        for (const auto &node : *c) remapped.emplace_back(rs(node.first), rm(node.second));
        built_chains[t].emplace_back(
            c, std::make_shared<const ArcChain>(std::move(remapped)));
      }
      built_sites[t].reserve(fr.sites.size());
      for (const ArcSites *st : fr.sites) {
        auto remapped = std::make_shared<ArcSites>();
        remapped->reserve(st->size());
        for (const auto &hop : *st) remapped->emplace_back(rs(hop.first), hop.second);
        built_sites[t].emplace_back(st, std::move(remapped));
      }
    });
    {
      size_t nc = 0, ns = 0;
      for (size_t t = 0; t < T; ++t) {
        nc += built_chains[t].size();
        ns += built_sites[t].size();
      }
      chain_memo.reserve(nc);
      sites_memo.reserve(ns);
      for (size_t t = 0; t < T; ++t) {
        for (auto &kv : built_chains[t]) chain_memo.emplace(kv.first, std::move(kv.second));
        for (auto &kv : built_sites[t]) sites_memo.emplace(kv.first, std::move(kv.second));
      }
    }
    std::vector<std::vector<std::pair<const ArcChain *, std::shared_ptr<const ArcChain>>>>().swap(built_chains);
    std::vector<std::vector<std::pair<const ArcSites *, std::shared_ptr<const ArcSites>>>>().swap(built_sites);
    std::vector<FirstRefs>().swap(refs);
    // Adopt the worker's StackSpecCache entries into the main cache. This
    // runs after the Src-referenced stacks were adopted (any stack adopted
    // here is one no taken Src references -- the same order as when this ran
    // after the whole drain) and before the remap below, which primes each
    // remapped Src's spec memo from the (now complete) main cache.
    // Resolve each worker stack's target stack serially (adoption mutates the
    // main tables), then insert the entries per target stack in parallel: they
    // are disjoint caches, so the pass is embarrassingly parallel and is the
    // second largest serial piece of the merge (~1.2 s of 6 s on full45).
    {
      std::vector<std::pair<uint32_t, uint32_t>> pairs;  // (worker, main)
      uint32_t max_ms = 0;
      for (size_t ws = 0; ws < w.spec_cache_by_stack_.size(); ++ws) {
        const StackSpecCache &wcache = w.spec_cache_by_stack_[ws];
        if (wcache.entry_count() == 0) continue;
        if (ws >= w.layer_stacks.size()) continue;
        uint32_t ms = 0;
        if (ws < seed_stack_count) {
          ms = static_cast<uint32_t>(ws);
        } else {
          if (ws - seed_stack_count >= stack_remap.size()) continue;
          if (stack_remap[ws - seed_stack_count] == UINT32_MAX) {
            stack_remap[ws - seed_stack_count] = AdoptStack(w.layer_stacks[ws]);
          }
          ms = stack_remap[ws - seed_stack_count];
        }
        if (ms >= max_ms) max_ms = ms + 1;
        pairs.emplace_back(static_cast<uint32_t>(ws), ms);
      }
      if (max_ms > spec_cache_by_stack_.size())
        spec_cache_by_stack_.resize(max_ms);
      if (!pairs.empty()) {
        std::atomic<size_t> next_pair{0};
        auto adopt_pair = [&]() {
          for (;;) {
            const size_t i = next_pair.fetch_add(1);
            if (i >= pairs.size()) return;
            const size_t ws = pairs[i].first;
            StackSpecCache &mcache = spec_cache_by_stack_[pairs[i].second];
            const StackSpecCache &wcache = w.spec_cache_by_stack_[ws];
            const LayerStack &wst = w.layer_stacks[ws];
            const LayerStack &mst = layer_stacks[pairs[i].second];
            const auto &wkeys = wcache.keys();
            const auto &wvals = wcache.vals();
            for (size_t k = 0; k < wkeys.size(); ++k) {
              const std::string &site = wkeys[k];
              const std::vector<SpecRef> &refs = wvals[k];
              const uint64_t h = StackSpecCache::HashSite(site.data(), site.size());
              if (mcache.find(site, h)) continue;
              std::vector<SpecRef> remapped;
              remapped.reserve(refs.size());
              for (const SpecRef &sr : refs) {
                SpecRef r = sr;
                if (r.layer_identifier) {
                  const ptrdiff_t idx =
                      r.layer_identifier - wst.layer_identifiers.data();
                  if (idx >= 0 &&
                      idx < static_cast<ptrdiff_t>(wst.layer_identifiers.size()) &&
                      idx < static_cast<ptrdiff_t>(mst.layer_identifiers.size())) {
                    r.layer_identifier = &mst.layer_identifiers[idx];
                  } else {
                    r.layer_identifier = nullptr;
                  }
                }
                remapped.push_back(std::move(r));
              }
              mcache.insert(site, h, std::move(remapped));
            }
          }
        };
        if (T == 1 || pairs.size() == 1) {
          adopt_pair();
        } else {
          std::vector<std::thread> adopters;
          adopters.reserve(T - 1);
          for (size_t t = 1; t < T; ++t) adopters.emplace_back(adopt_pair);
          adopt_pair();
          for (std::thread &th : adopters) th.join();
        }
      }
    }
    parallel([&](size_t, size_t lo, size_t hi) {
      // Sources share their arc context (SrcContext): remap each context's
      // chain / site trail once and hand the remapped copy to every source
      // that held it, instead of a copy-on-write clone per source.
      std::unordered_map<const SrcContext *, std::shared_ptr<SrcContext>> ctx_memo;
      for (size_t e = lo; e < hi; ++e) {
        if (!take[e]) continue;
        std::vector<Src> &vec = w.sources_cache.vals_[ent[e]];
        for (Src &s : vec) {
          s.stack_idx = rs(s.stack_idx);
          s.map_idx = rm(s.map_idx);
          // NOTE: the vector is moved (a buffer steal) below, so the elements'
          // SpecsMemo self-reset never runs: drop the worker spec pointer.
          s.specs_.p = nullptr;  // worker cache pointer; main re-resolves
          // Prime the memo exactly as the main SpecsFor would store it when
          // the main spec cache already holds (stack, site): the structure
          // walk and the fill then skip ~one hash + probe per Src. Read-only
          // lookups; nothing inserts into the spec caches during this phase.
          // Only worth it when this phase runs in parallel: serially it just
          // moves (and, bypassing the path filter, enlarges) the walk's work.
          if (T > 1 && s.stack_idx < spec_cache_by_stack_.size()) {
            StackSpecCache &sc = spec_cache_by_stack_[s.stack_idx];
            if (std::vector<SpecRef> *v = sc.find(
                    s.site, StackSpecCache::HashSite(s.site.data(), s.site.size()))) {
              s.specs_.p = v;
              s.spec_state.v = v->empty() ? SpecState::kNone : SpecState::kSome;
            } else if (s.spec_state.v == SpecState::kUnknown &&
                       s.stack_idx < layer_stacks.size() &&
                       FindSpecs(layer_stacks[s.stack_idx], s.site).empty()) {
              // Negative results are not cached; the layers' path lookups
              // are read-only, so resolve them here (in parallel) and stamp
              // the known-empty state SpecsFor would store.
              s.spec_state.v = SpecState::kNone;
            }
          }
          if (s.implied_anchor != UINT32_MAX) s.implied_anchor = rs(s.implied_anchor);
          if (const SrcContext *c = s.context().get()) {
            auto memo = ctx_memo.find(c);
            if (memo == ctx_memo.end()) {
              std::shared_ptr<SrcContext> remapped;
              if (c->arc_chain && !c->arc_chain->empty()) {
                auto it = chain_memo.find(c->arc_chain.get());
                if (it != chain_memo.end()) {
                  remapped = std::make_shared<SrcContext>(*c);
                  remapped->arc_chain = it->second;
                }
              }
              if (c->arc_sites && !c->arc_sites->empty()) {
                auto it = sites_memo.find(c->arc_sites.get());
                if (it != sites_memo.end()) {
                  if (!remapped) remapped = std::make_shared<SrcContext>(*c);
                  remapped->arc_sites = it->second;
                }
              }
              memo = ctx_memo.emplace(c, std::move(remapped)).first;
            }
            if (memo->second) s.set_context(memo->second);
          }
        }
      }
    });
    // Allocate address-stable values in the original order, then populate the
    // index in parallel. The prescan established that all taken keys are
    // distinct and absent. No lookup runs until every insertion has joined.
#if defined(__GNUC__) || defined(__clang__)
    if (T > 1 && sources_cache.occupied_ == sources_cache.count_ &&
        sources_cache.vals_.size() + ne <= UINT32_MAX) {
      std::vector<uint32_t> indices(ne, UINT32_MAX);
      size_t added = 0;
      for (size_t e = 0; e < ne; ++e) {
        if (!take[e]) continue;
        indices[e] = static_cast<uint32_t>(sources_cache.vals_.size());
        sources_cache.keys_.emplace_back(w.sources_cache.take_key(ent[e]));
        sources_cache.vals_.emplace_back(std::move(w.sources_cache.vals_[ent[e]]));
        sources_cache.live_.push_back(SrcCache::kLive);
        ++added;
      }
      const size_t mask = sources_cache.slots_.size() - 1;
      parallel([&](size_t, size_t lo, size_t hi) {
        for (size_t e = lo; e < hi; ++e) {
          if (indices[e] == UINT32_MAX) continue;
          size_t i = static_cast<size_t>(ent_hash[e]) & mask;
          for (;;) {
            auto &slot = sources_cache.slots_[i];
            uint8_t empty = 0;
            // State 3 reserves the slot; only the owning thread writes its
            // payload, and no cache reader runs during this phase.
            if (__atomic_compare_exchange_n(&slot.state, &empty, 3, false,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
              slot.hash = ent_hash[e];
              slot.idx = indices[e];
              __atomic_store_n(&slot.state, 1, __ATOMIC_RELAXED);
              break;
            }
            i = (i + 1) & mask;
          }
        }
      });
      sources_cache.count_ += added;
      sources_cache.occupied_ += added;
      return;
    }
#endif
    for (size_t e = 0; e < ne; ++e) {
      if (!take[e]) continue;
      for (size_t a = e + 1; a < ne && a <= e + 8; ++a) {
        if (take[a]) {
          sources_cache.prefetch(ent_hash[a]);
          break;
        }
      }
      bool created = false;
      std::vector<Src> &slot = sources_cache.get_or_create_hashed(
          w.sources_cache.take_key(ent[e]), ent_hash[e], &created);
      slot = std::move(w.sources_cache.vals_[ent[e]]);
    }
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
    // Entries are independent (key compare + free per index): a 10 M-entry
    // cache splits across threads.
    auto range = [this](size_t lo, size_t hi) {
      size_t borrowed = 0;
      for (size_t i = lo; i < hi; ++i) {
        if (live_[i] != kLive || vals_[i].empty() ||
            keys_[i] != vals_[i].front().site) continue;
        std::string().swap(keys_[i]);
        live_[i] = kLive | kSiteKey;
        ++borrowed;
      }
      return borrowed;
    };
    const size_t n = live_.size();
#if defined(LIGHTUSD_ENABLE_THREAD)
    const size_t hw = std::max(1u, std::thread::hardware_concurrency());
    const size_t T = n >= 262144 ? std::min<size_t>(hw, 16) : 1;
    if (T > 1) {
      std::vector<size_t> counts(T, 0);
      std::vector<std::thread> pool;
      pool.reserve(T - 1);
      for (size_t t = 1; t < T; ++t)
        pool.emplace_back([&, t] { counts[t] = range(n * t / T, n * (t + 1) / T); });
      counts[0] = range(0, n / T);
      for (std::thread &th : pool) th.join();
      size_t total = 0;
      for (size_t c : counts) total += c;
      return total;
    }
#endif
    return range(0, n);
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

auto cache_detail::SrcCache::get_or_create_hashed(std::string &&k, uint64_t h,
                                                 bool *created)
    -> std::vector<Src> & {
    if (slots_.empty() || (occupied_ + 1) * 2 > slots_.size()) rehash();
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    size_t first_tomb = SIZE_MAX;
    for (;;) {
      Slot &s = slots_[i];
      if (s.state == 0) {
        const size_t at = (first_tomb == SIZE_MAX) ? i : first_tomb;
        if (created) *created = true;
        return emplace_at(at, h, std::move(k), /*was_tomb=*/slots_[at].state == 2);
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

auto cache_detail::SrcCache::live_with_hash(std::vector<uint32_t> *idx,
                                           std::vector<uint64_t> *hash) const
    -> void {
    std::vector<uint64_t> hv(live_.size(), 0);
    for (const Slot &s : slots_)
      if (s.state == 1 && s.idx < hv.size()) hv[s.idx] = s.hash;
    idx->clear();
    hash->clear();
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (!live_[vi]) continue;
      idx->push_back(vi);
      hash->push_back(hv[vi]);
    }
  }

auto cache_detail::SrcCache::find_hashed(const std::string &k, uint64_t h) const
    -> const std::vector<Src> * {
    if (slots_.empty()) return nullptr;
    const size_t mask = slots_.size() - 1;
    for (size_t i = static_cast<size_t>(h) & mask;; i = (i + 1) & mask) {
      const Slot &s = slots_[i];
      if (s.state == 0) return nullptr;
      if (s.state == 1 && s.hash == h) {
        const std::string &key = key_at(s.idx);
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          return &vals_[s.idx];
        }
      }
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

auto cache_detail::SrcCache::emplace_at(size_t slot, uint64_t h, std::string k,
                               bool was_tomb) -> std::vector<Src> & {
    uint32_t vi;
    if (was_tomb) {
      vi = static_cast<uint32_t>(live_.size());
      for (uint32_t di = 0; di < live_.size(); ++di) {
        if (!live_[di]) { vi = di; break; }
      }
      if (vi < live_.size()) {
        keys_[vi] = std::move(k);
        vals_[vi].clear();
        live_[vi] = 1;
      } else {
        keys_.emplace_back(std::move(k));
        vals_.emplace_back();
        live_.push_back(1);
      }
    } else {
      vi = static_cast<uint32_t>(vals_.size());
      keys_.emplace_back(std::move(k));
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
