// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache parallel worker merge helpers.
// Included inside Cache::Impl.

  void MergeWorkerIndex(Impl &w, const std::string &key) {
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

  void MergeSources(Impl &w, size_t seed_stack_count, size_t seed_pool_count) {
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
      if (sources_cache.contains(key)) return;
      // The worker Impl is discarded right after this merge, so steal its Src
      // vector instead of copying it. NOTE: a vector move is a buffer steal —
      // the elements' SpecsMemo self-reset never runs — so any worker-set
      // specs_ (pointing into the discarded worker's spec cache) must be
      // dropped explicitly below.
      std::vector<Src> &srcs = sources_cache.get_or_create(key);
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
