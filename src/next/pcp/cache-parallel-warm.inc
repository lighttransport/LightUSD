// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache parallel source warming and index prewarm.
// Included inside Cache::Impl.

  bool BuildStage(Stage *stage, std::string *warn, std::string *err,
                  const Cache::PreviewCallback &preview_callback) {
    NEXT_PCP_LOCK(api_mu_);
    auto out = std::unique_ptr<Layer>(new Layer());

    // BuildStage re-registers every prim's instancing from scratch below, so
    // reset the prototype maps first. They are otherwise only pruned via
    // index_cache-driven Invalidate, which BuildStage's direct RegisterInstance
    // path doesn't populate -- leaving stale groupings across recomposition
    // (e.g. after Load/UnloadPayload changes a prim's arc set and instance key).
    prototype_by_key.clear();
    prototype_of.clear();
    instances_by_prototype.clear();
    instance_key_order.clear();

    const Layer *root = layer_stacks[0].layers[0].get();
    // Root (pseudo-root child) names compose WEAK->STRONG too, like nested
    // children (pxr PcpComposeSiteChildNames): iterate the root layer stack
    // weakest-first so a sublayer-only root prim keeps its position. Prototype
    // assignment is order-independent (see test_prototype_order_independent).
    std::set<std::string> seen_root;
    std::vector<std::string> root_names;
    for (auto lit = layer_stacks[0].layers.rbegin();
         lit != layer_stacks[0].layers.rend(); ++lit) {
      const auto &lp = *lit;
      for (uint32_t ri : lp->root_indices()) {
        const PrimSpec *ps = lp->prim(ri);
        if (!ps) continue;
        if (seen_root.insert(ps->name()).second) root_names.push_back(ps->name());
      }
    }
    // A relocate may move a prim to a NEW ROOT PRIM. The root layer need not
    // author a spec for the new name, so take the names from the root layer
    // stack's namespace edits as well.
    if (const std::vector<StackRelocates::Arrival> *arrivals =
            layer_stacks[0].relocates.ArrivalsAt("/")) {
      for (const StackRelocates::Arrival &a : *arrivals) {
        if (seen_root.insert(a.name).second) root_names.push_back(a.name);
      }
    }
    prof_sources_ns_ = prof_compose_ns_ = prof_reg_ns_ = 0;
    prof_source_hits_ = prof_source_misses_ = prof_structure_nodes_ = 0;
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Resolve independent namespace subtrees concurrently before the serial
    // structure walk. ParallelWarmSources was previously implemented but never
    // invoked, leaving num_threads effective only for the much cheaper opinion
    // fill pass.
    int warm_threads = options.num_threads;
    if (warm_threads < 0)
      warm_threads = std::min<int>(
          static_cast<int>(std::thread::hardware_concurrency()),
          kMaxExecutionThreads);
    warm_threads = std::min(warm_threads, kMaxExecutionThreads);
    if (warm_threads > 1)
      ParallelWarmSources(root_names, warm_threads, warn, err);
#endif
    const auto structure_t0 = std::chrono::steady_clock::now();
    // Source warming has already discovered the namespace cardinality. Reserve
    // the flattened prim vector once before the structure pass; repeated
    // growth moves large PrimSpec objects and is especially expensive on the
    // 300k+ node Caldera sub-scenes. Keep small scenes on Layer's normal growth
    // path to avoid over-reserving for sparse compositions.
    if (sources_cache.size() >= 8192) {
      out->reserve(sources_cache.size());
    }
    // Structure pass (serial): walk every root, fixing each prim's slot/parent/
    // child-order/instancing and recording its (slot, sources) for opinion fill.
    fill_.clear();
    for (const std::string &nm : root_names) {
      BuildStage(out.get(), {Path("/" + nm), Path("/" + nm), 0, true, 1},
                 warn, err);
    }
    const auto structure_t1 = std::chrono::steady_clock::now();
    if (preview_callback) {
      // The callback receives an independent immutable-in-practice Stage. The
      // authoritative layer remains owned here and is filled below only after
      // the callback returns, so a consumer may retain the preview safely.
      Layer preview_layer = BuildPreviewLayer(*out);
      preview_layer.meta() = root->meta();
      preview_layer.meta().subLayers.clear();
      preview_layer.meta().relocates.clear();
      preview_layer.finalize();
      Stage preview_stage;
      preview_stage.SetRootLayer(std::move(preview_layer));
      if (!preview_callback(std::move(preview_stage))) {
        if (err && err->empty()) *err = "stage preview callback cancelled";
        return false;
      }
    }
    // Opinion-fill pass: compose every slot's opinions, in parallel when
    // num_threads > 1 (each slot is independent; sources_cache is read-only here).
    auto fill_t0 = std::chrono::steady_clock::now();
    FillOpinions(out.get());
    const auto fill_t1 = std::chrono::steady_clock::now();
    if (options.enable_timing) {
      prof_compose_ns_ +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              fill_t1 - fill_t0).count();
    }
    std::vector<std::pair<uint32_t, const std::vector<Src>*>>().swap(fill_);
    switch (options.instance_flatten_mode) {
      case InstanceFlattenMode::Holder:
        FlattenInstances(out.get());
        break;
      case InstanceFlattenMode::ExtractedPrototypes:
        FlattenInstancesExtracted(out.get(), options.prototype_numbering);
        break;
      case InstanceFlattenMode::Native:
        break;
    }
    const auto flatten_t1 = std::chrono::steady_clock::now();

    out->finalize();
    const auto finalize_t1 = std::chrono::steady_clock::now();
    if (options.enable_timing) {
      auto elapsed_ms = [](std::chrono::steady_clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
      };
      LIGHTUSD_LOG_I("[next_build] sources=" +
                  FormatMilliseconds(prof_sources_ns_ / 1e6) +
                  "ms structure=" +
                  FormatMilliseconds(elapsed_ms(structure_t1 - structure_t0)) +
                  "ms compose=" + FormatMilliseconds(prof_compose_ns_ / 1e6) +
                  "ms register=" + FormatMilliseconds(prof_reg_ns_ / 1e6) +
                  "ms source_hits=" + UIntToStr(prof_source_hits_) +
                  " source_misses=" + UIntToStr(prof_source_misses_) +
                  " nodes=" + UIntToStr(prof_structure_nodes_) +
                  " flatten=" +
                  FormatMilliseconds(elapsed_ms(flatten_t1 - fill_t1)) +
                  "ms finalize=" +
                  FormatMilliseconds(elapsed_ms(finalize_t1 - flatten_t1)) +
                  "ms");
    }
    out->meta() = root->meta();
    // Stage metadata comes from the ROOT layer only. pxr does NOT fall back
    // to sublayer-authored stage fields (verified against the 26.05 oracle:
    // a sublayer-only upAxis leaves the stage at the fallback "Y" and
    // usdcat --flatten omits it) — the old fill-from-sublayers pass here was
    // nonconformant.
    // Sublayer opinions are baked into the flattened output; keeping the
    // subLayers list would re-apply (now stale) layers when the exported
    // layer is read back. Relocates are likewise already APPLIED by the
    // stage-build walk — keeping them would re-relocate on re-read.
    out->meta().subLayers.clear();
    out->meta().relocates.clear();
    for (const std::string &path : deferred_payload_prims) {
      if (PrimSpec *prim = out->prim_at_path_mutable(path)) {
        prim->meta().loaded = false;
      }
    }
    stage->SetRootLayer(std::move(*out));
    return true;
  }

  // Convert native instancing to a self-contained flatten: each prototype group
  // keeps its prototype member as the shared content holder (made
  // non-instanceable), and every other member is emptied + internally references
  // that holder. See CompositionOptions::flatten_instances.
  void FlattenInstances(Layer *out) {
    if (instances_by_prototype.empty()) return;

    std::unordered_map<std::string, uint32_t> idx_by_path;
    for (uint32_t i = 0; i < out->prim_count(); ++i)
      if (const PrimSpec *p = out->prim(i)) idx_by_path[p->path().str()] = i;

    for (const auto &grp : instances_by_prototype) {
      const std::string &proto_path = grp.first;
      const std::vector<std::string> &members = grp.second;
      // A lone instanceable prim with no peers still composed its own subtree as
      // the prototype; leaving it untouched is already a valid flatten.
      bool has_instance = false;
      for (const std::string &m : members)
        if (m != proto_path) { has_instance = true; break; }
      if (!has_instance) continue;

      auto pit = idx_by_path.find(proto_path);
      if (pit == idx_by_path.end()) continue;
      // Holder: keep the composed subtree, drop the instanceable flag so it is a
      // plain reference target (an instanceable holder would hide its content).
      if (PrimSpec *proto = out->prim(pit->second)) {
        proto->meta().instanceable = false;
        proto->meta().instance_prototype().clear();
      }

      const std::string ref = "<" + proto_path + ">";
      for (const std::string &m : members) {
        if (m == proto_path) continue;
        auto mit = idx_by_path.find(m);
        if (mit == idx_by_path.end()) continue;
        PrimSpec *mp = out->prim(mit->second);
        if (!mp) continue;
        mp->meta().instanceable = true;
        mp->meta().instance_prototype().clear();
        // Avoid duplicate refs if FlattenInstances somehow runs twice.
        auto &refs = mp->meta().references;
        if (std::find(refs.begin(), refs.end(), ref) == refs.end())
          refs.push_back(ref);
      }
    }
  }

  // usdcat-style flatten: move each prototype group's shared subtree to a root
  // `over "/Flattened_Prototype_N"` and rewrite every member (holder included)
  // to `instanceable = true` + `references = </Flattened_Prototype_N>`. Numbering
  // per `numbering` (Deterministic = sort by prototype path; UsdcatCompatible =
  // pxr's two-stage scheme). See CompositionOptions::instance_flatten_mode.
  void FlattenInstancesExtracted(Layer *out, PrototypeNumbering numbering) {
    if (instances_by_prototype.empty()) return;

    // path -> prim index (for member rewrites in Pass 2). Member indices stay
    // valid: Pass 1 only ADDS prims (FP roots + clones), never reindexes.
    std::unordered_map<std::string, uint32_t> idx_by_path;
    for (uint32_t i = 0; i < out->prim_count(); ++i)
      if (const PrimSpec *p = out->prim(i)) idx_by_path[p->path().str()] = i;

    // --- order groups, assign /Flattened_Prototype_N ------------------------
    // members[0] == prototype path == namespace-pre-order-first instance.
    std::vector<std::string> ordered;
    {
      std::vector<std::string> protos;
      protos.reserve(instances_by_prototype.size());
      for (const auto &g : instances_by_prototype) protos.push_back(g.first);
      std::sort(protos.begin(), protos.end());  // by prototype path (== pxr front)
      if (numbering == PrototypeNumbering::Deterministic) {
        ordered = std::move(protos);
      } else {
        // UsdcatCompatible: pxr labels prototypes `__Prototype_{rank+1}` in
        // REVERSE discovery order (the LAST instance key to first appear during
        // the stage walk becomes __Prototype_1) — pxr registers prototypes on a
        // stack, so the numbering is last-in-first-numbered. GetPrototypes()
        // then re-sorts by label STRING (differs from numeric at >9 groups:
        // _1,_10,_2...). Verified against usdcat: instances referencing A,B,C
        // discovered in that order number A=_3, B=_2, C=_1.
        std::vector<std::string> front;
        front.reserve(protos.size());
        {
          std::set<std::string> remaining(protos.begin(), protos.end());
          for (size_t r = instance_key_order.size(); r-- > 0;) {
            auto kit = prototype_by_key.find(instance_key_order[r]);
            if (kit == prototype_by_key.end()) continue;
            if (remaining.erase(kit->second)) front.push_back(kit->second);
          }
          for (const std::string &pp : protos) {  // defensive leftovers
            if (remaining.count(pp)) front.push_back(pp);
          }
        }
        std::vector<std::pair<std::string, std::string>> labelled;
        labelled.reserve(front.size());
        for (size_t i = 0; i < front.size(); ++i)
          labelled.emplace_back("__Prototype_" + UIntToStr(i + 1), front[i]);
        std::sort(labelled.begin(), labelled.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        ordered.reserve(labelled.size());
        for (auto &lp : labelled) ordered.push_back(std::move(lp.second));
      }
    }

    out->build_path_index();
    std::unordered_map<std::string, std::string> fpath;  // proto -> "/Flattened_Prototype_j"
    {
      std::unordered_set<std::string> used;
      for (uint32_t i = 0; i < out->prim_count(); ++i) {
        if (const PrimSpec *p = out->prim(i)) used.insert(p->path().str());
      }
      size_t n = 1;
      for (const std::string &proto : ordered) {
        std::string p;
        do { p = "/Flattened_Prototype_" + UIntToStr(n++); }
        while (!used.insert(p).second);  // skip user-named clashes
        fpath[proto] = p;
      }
    }

    // FP a descendant `d` should reference when cloned into a prototype subtree:
    // `d` itself if it is a (nested) prototype group key, else d's prototype.
    auto fp_ref_for = [&](const std::string &d_path,
                          const std::string &d_inst_proto) -> const std::string * {
      auto it = fpath.find(d_path);
      if (it != fpath.end()) return &it->second;            // nested prototype
      if (!d_inst_proto.empty()) {
        auto j = fpath.find(d_inst_proto);
        if (j != fpath.end()) return &j->second;            // nested instance
      }
      return nullptr;
    };

    // --- Pass 1: emit FP roots + clone each prototype's shared subtree -------
    // Reads ORIGINAL prims; all of Pass 1 runs before any Pass 2 orphaning.
    struct CloneWork {
      uint32_t src_idx;
      uint32_t dst_parent_idx;
      std::string dst_parent_path;
    };
    for (const std::string &proto : ordered) {
      auto pit = idx_by_path.find(proto);
      if (pit == idx_by_path.end()) continue;
      const std::string &fp = fpath[proto];

      PrimSpec root(fp.substr(1));  // "Flattened_Prototype_j"
      root.set_path(Path(fp));
      root.set_specifier(PrimSpecifier::Over);  // typeless `over`, like usdcat
      uint32_t fp_idx = out->add_prim(std::move(root));
      out->add_root(fp_idx);

      // Push children reversed so the work STACK pops them first-child-first,
      // preserving source namespace order in the cloned subtree.
      std::vector<CloneWork> work;
      {
        const auto &cc = out->prim(pit->second)->child_indices();
        for (auto it = cc.rbegin(); it != cc.rend(); ++it)
          work.push_back({*it, fp_idx, fp});
      }

      while (!work.empty()) {
        CloneWork cw = work.back();
        work.pop_back();
        // Capture everything from the source BEFORE add_prim (it may realloc).
        std::string name, new_path, src_path, inst_proto;
        std::vector<uint32_t> src_children;
        PrimSpec clone;
        {
          const PrimSpec *src = out->prim(cw.src_idx);
          if (!src) continue;
          name = src->name();
          src_path = src->path().str();
          inst_proto = src->meta().instance_prototype();
          src_children = src->child_indices();
          new_path = cw.dst_parent_path + "/" + name;
          clone = src->Clone();
        }
        const std::string *ref = fp_ref_for(src_path, inst_proto);
        const bool leaf = (ref != nullptr);

        clone.set_name(name);
        clone.set_path(Path(new_path));
        clone.clear_child_indices();  // Clone copied stale source child links
        // Retarget internal material:binding / connections that pointed inside
        // the prototype (under `proto`) to the moved /Flattened_Prototype_N root.
        clone.remap_target_prefix(proto, fp);
        if (leaf) {
          clone.meta().instanceable = true;
          clone.meta().instance_prototype().clear();
          clone.meta().references.clear();
          clone.meta().references.push_back("<" + *ref + ">");
          // pxr flatten spells the prototype arc `add references = </FP_N>`.
          ArcEdit &redit = clone.meta().ensure_arc_edits().references;
          redit = ArcEdit();
          redit.authored = true;
          redit.is_explicit = false;
          redit.added.push_back("<" + *ref + ">");
        }
        uint32_t new_idx = out->add_prim(std::move(clone));  // invalidates ptrs
        out->set_parent(new_idx, cw.dst_parent_idx);
        if (!leaf)
          for (auto it = src_children.rbegin(); it != src_children.rend(); ++it)
            work.push_back({*it, new_idx, new_path});
      }
    }

    // --- Pass 2: rewrite every member at its original namespace position -----
    for (const std::string &proto : ordered) {
      const std::string ref = "<" + fpath[proto] + ">";
      auto mit = instances_by_prototype.find(proto);
      if (mit == instances_by_prototype.end()) continue;
      for (const std::string &m : mit->second) {
        auto it = idx_by_path.find(m);
        if (it == idx_by_path.end()) continue;
        PrimSpec *mp = out->prim(it->second);
        if (!mp) continue;
        mp->meta().instanceable = true;
        mp->meta().instance_prototype().clear();
        mp->clear_child_indices();  // orphan inline subtree (now under the FP root)
        auto &refs = mp->meta().references;
        if (std::find(refs.begin(), refs.end(), ref) == refs.end())
          refs.push_back(ref);
        // pxr flatten spells the prototype arc `add references = </FP_N>`.
        ArcEdit &redit = mp->meta().ensure_arc_edits().references;
        redit = ArcEdit();
        redit.authored = true;
        redit.is_explicit = false;
        redit.added.push_back(ref);
      }
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  // --- parallel-build merge helpers (main thread only) --------------------

  // Adopt a worker's prebuilt LayerStack into the main table (dedup by the
  // stack's full identity key — (identifier, expression variables), see
  // InternLayerStack; the bare identifier is the fallback for stacks built
  // without a key). Returns the main-table index. The LayerStack is copied,
  // which just bumps the shared_ptr<Layer> refcounts -- the layers stay
  // shared.
  uint32_t AdoptStack(const LayerStack &ws) {
    const std::string &key =
        ws.cache_key.empty() ? ws.identifier : ws.cache_key;
    auto it = stack_by_id.find(key);
    if (it != stack_by_id.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(layer_stacks.size());
    layer_stacks.push_back(ws);
    stack_by_id.emplace(key, idx);
    return idx;
  }

  // Worker-side: warm this (private, SEEDED) Impl's sources_cache for the whole
  // subtree rooted at `root`, following the same child namespace the serial
  // compose walks (ComposeChildNames). The Impl was seeded with a snapshot of the
  // main's layer-stack table + ancestor sources_cache, so the root->frontier
  // ancestors are already cached (no re-resolution) and any new reference resolves
  // against the SAME anchors the serial build sees -> identical layer-stack
  // identifiers. Fills a cache only; missed paths still resolve identically in the
  // serial BuildStage.
  void WarmSubtree(const Path &root, uint32_t root_depth, std::string *warn,
                   std::string *err) {
    struct WItem {
      Path p;
      uint32_t depth;
    };
    std::vector<WItem> stack;
    stack.reserve(128);
    stack.push_back({root, root_depth});
    while (!stack.empty()) {
      WItem it = stack.back();
      stack.pop_back();
      if (it.depth > options.max_namespace_depth) continue;
      const std::vector<Src> &srcs = SourcesForPath(it.p, warn, err);
      // Instancing-aware: the serial BuildStage composes only the prototype
      // member of an instance group and stops at every instance (is_instance ->
      // continue) -- it never descends an instance's subtree. Mirror that here so
      // warming does not resolve the (redundant) subtrees of every instance. On
      // heavily-instanced scenes the instances vastly outnumber the prototypes,
      // so these subtree resolutions dominate the worker + merge cost while the
      // serial build discards all but one per group. Stopping is byte-neutral:
      // warming only fills a cache; an unwarmed prototype subtree simply resolves
      // (identically) in the serial walk.
      if (options.detect_instances && IsInstanceableSources(srcs)) continue;
      const std::vector<std::string> children = ComposeChildNames(srcs);
      for (auto cit = children.rbegin(); cit != children.rend(); ++cit) {
        stack.push_back({it.p.append_child(*cit), it.depth + 1});
      }
    }
  }

  // Parallel pre-warm of sources_cache (the LIVRPS arc resolution that dominates
  // build_stage). Serially BFS the namespace to discover a wide-enough frontier of
  // subtree roots (warming the shallow top levels into the main cache + table),
  // SNAPSHOT the main's layer-stack table + ancestor sources_cache, then have
  // worker threads (private Impls seeded from that snapshot, borrowing the
  // thread-safe shared registry) warm each frontier subtree's sources in parallel,
  // and merge the results back. The serial BuildStage then finds SourcesForPath a
  // cache hit. Byte-identical: composed values/instancing key off layer-stack
  // identifier strings, the seed gives workers the same identities the serial
  // build sees, property emission is name-ordered (parse-order-independent), and
  // any un-warmed path resolves serially.
  void ParallelWarmSources(const std::vector<std::string> &root_names, int nt,
                          std::string *warn, std::string *err) {
    using PWClock = std::chrono::steady_clock;
    const bool prof = options.enable_timing;
    auto t_start = prof ? PWClock::now() : PWClock::time_point{};
    const size_t want = static_cast<size_t>(nt) * 4;
    const uint32_t kMaxFrontierDepth = 8;
    struct FrontierItem {
      Path path;
      uint32_t depth;
    };
    std::vector<FrontierItem> level;
    level.reserve(root_names.size());
    for (const std::string &nm : root_names) {
      level.push_back({Path("/" + nm), 1});
    }
    for (uint32_t d = 1;
         d < kMaxFrontierDepth && level.size() < want;
         ++d) {
      std::vector<FrontierItem> next;
      next.reserve(std::min(level.size() * 4, want));
      bool grew = false;
      for (const FrontierItem &entry : level) {
        const Path &p = entry.path;
        if (entry.depth > options.max_namespace_depth) {
          next.push_back(entry);
          continue;
        }
        const std::vector<Src> &srcs = SourcesForPath(p, warn, err);
        // Instancing-aware (see WarmSubtree): an instanceable prim's subtree is
        // composed at most once (its prototype), so do not expand the frontier
        // into it -- treat it as a leaf root the worker will stop at immediately.
        if (options.detect_instances && IsInstanceableSources(srcs)) {
          next.push_back(entry);
          continue;
        }
        const std::vector<std::string> children = ComposeChildNames(srcs);
        if (children.empty()) {
          next.push_back(entry);  // leaf: nothing below to parallelize
        } else {
          grew = true;
          for (const std::string &cn : children) {
            next.push_back({p.append_child(cn), entry.depth + 1});
          }
        }
      }
      level = std::move(next);
      if (!grew) break;
    }
    if (level.size() < 2) return;  // not enough fan-out to parallelize
    // Source entries are borrowed through source_seed_cache rather than copied
    // into each worker. Keep worker fanout enabled for broad production scenes:
    // those are exactly the inputs where stopping at a 100k-entry threshold
    // serialized the remaining namespace and dominated cold composition.
    auto t_discover = prof ? PWClock::now() : PWClock::time_point{};

    // Snapshot the main resolution context built by the serial discovery above.
    const size_t seed_stack_count = layer_stacks.size();
    const size_t seed_pool_count = nm_pool_.size();

    const int W = std::min<int>(nt, static_cast<int>(level.size()));
    std::vector<std::unique_ptr<Impl>> workers;
    workers.reserve(static_cast<size_t>(W));
    std::vector<std::string> wwarn(static_cast<size_t>(W));
    std::vector<std::string> werr(static_cast<size_t>(W));
    for (int t = 0; t < W; ++t) {
      std::unique_ptr<Impl> wp(new Impl());
      wp->resolver = resolver;
      wp->options = options;
      wp->load_rules_ = load_rules_;
      wp->reg_ = reg_;  // borrow the shared, parse-once registry
      wp->root_layer = root_layer;
      wp->root_identifier = root_identifier;
      // Seed the worker with the main's resolution context so its reference
      // resolution produces identical layer-stack identities (the crux of
      // byte-identity). Source entries are borrowed from the immutable main
      // cache; only newly resolved entries are retained in the worker delta.
      wp->layer_stacks = layer_stacks;
      wp->stack_by_id = stack_by_id;
      wp->source_seed_cache = &sources_cache;
      wp->nm_pool_ = nm_pool_;  // seed the mapping pool (map_idx < seed are shared)
      workers.push_back(std::move(wp));
    }

    // Workers operate on private (seeded) Impls + the thread-safe registry only;
    // the main thread holds api_mu_ throughout. Dynamic work-stealing -- frontier
    // subtrees are very uneven, so static chunks leave workers idle behind one
    // heavy subtree.
    std::atomic<size_t> next_node{0};
    std::vector<std::thread> ts;
    ts.reserve(static_cast<size_t>(W));
    for (int t = 0; t < W; ++t) {
      Impl *wk = workers[static_cast<size_t>(t)].get();
      std::string *pw = &wwarn[static_cast<size_t>(t)];
      std::string *pe = &werr[static_cast<size_t>(t)];
      ts.emplace_back([wk, &next_node, &level, pw, pe]() {
        for (;;) {
          size_t i = next_node.fetch_add(1);
          if (i >= level.size()) break;
          wk->WarmSubtree(level[i].path, level[i].depth, pw, pe);
        }
      });
    }
    for (auto &t : ts) t.join();
    auto t_workers = prof ? PWClock::now() : PWClock::time_point{};

    size_t worker_delta_entries = 0;
    for (int t = 0; t < W; ++t) {
      worker_delta_entries += workers[static_cast<size_t>(t)]->sources_cache.size();
      MergeSources(*workers[static_cast<size_t>(t)], seed_stack_count,
                   seed_pool_count);
      if (warn) *warn += wwarn[static_cast<size_t>(t)];
      if (err) *err += werr[static_cast<size_t>(t)];
    }
    if (prof) {
      auto ms = [](PWClock::time_point a, PWClock::time_point b) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a)
                   .count() / 1e6;
      };
      auto t_merge = PWClock::now();
      LIGHTUSD_LOG_I("[next_warm] frontier=" + UIntToStr(level.size()) +
                  " W=" + IntToStr(W) +
                  " discover=" + FormatMilliseconds(ms(t_start, t_discover)) +
                  "ms workers=" + FormatMilliseconds(ms(t_discover, t_workers)) +
                  "ms merge=" + FormatMilliseconds(ms(t_workers, t_merge)) +
                  "ms cache=" + UIntToStr(sources_cache.size()) +
                  " delta=" + UIntToStr(worker_delta_entries));
    }
  }
#endif  // LIGHTUSD_ENABLE_THREAD

  // Batch build. With threads enabled and num_threads != 1, this first prefetches
  // first-level reference/payload layers into the (thread-safe) registry in
  // parallel, then builds the per-prim indices CONCURRENTLY: each worker runs the
  // ordinary build on its own private Impl (borrowing the shared, parse-once
  // registry), and a deterministic input-order merge folds the results into the
  // cache. Composed values are identical to a serial run. Non-threaded builds (or
  // num_threads <= 1) use the plain serial loop.
  bool PrewarmPrimIndices(const std::vector<Path> &paths, std::string *warn,
                           std::string *err) {
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Use unique_lock (not lock_guard) so we can release api_mu_ during the
    // parallel build and reacquire it for the merge.
    std::unique_lock<PcpMutex> wlk(api_mu_);
    int nt = options.num_threads;
    if (nt < 0) {
      nt = std::min<int>(static_cast<int>(std::thread::hardware_concurrency()),
                         kMaxExecutionThreads);
    }
    nt = std::min(nt, kMaxExecutionThreads);
    if (nt > 1) {
      // (a) Prefetch root-level reference/payload assets to warm the shared
      // registry (reduces redundant double-parses across workers).
      std::vector<std::pair<std::string, std::string>> assets;  // (asset, anchor)
      std::set<std::string> seen;
      // Anchor each layer's arcs to THAT layer's identifier (a sublayer's
      // relative reference resolves against the sublayer, not the stack root).
      const auto &ids = layer_stacks[0].layer_identifiers;
      for (size_t li = 0; li < layer_stacks[0].layers.size(); ++li) {
        const auto &lp = layer_stacks[0].layers[li];
        const std::string &anchor =
            li < ids.size() ? ids[li] : layer_stacks[0].identifier;
        for (const PrimSpec &ps : lp->prims()) {
          for (const std::string &r : ps.meta().references) {
            CompositionArc a = Compositor::ParseReference(r);
            if (!a.asset_path.empty() && seen.insert(a.asset_path).second)
              assets.emplace_back(a.asset_path, anchor);
          }
          for (const std::string &r : ps.meta().payloads) {
            CompositionArc a = Compositor::ParsePayload(r);
            if (!a.asset_path.empty() && seen.insert(a.asset_path).second)
              assets.emplace_back(a.asset_path, anchor);
          }
        }
      }
      if (!assets.empty()) {
        std::atomic<size_t> next_idx{0};
        int pf = std::min<int>(nt, static_cast<int>(assets.size()));
        std::vector<std::thread> ts;
        auto fn = [&]() {
          for (;;) {
            size_t i = next_idx.fetch_add(1);
            if (i >= assets.size()) break;
            std::string w, e;
            LayerLoadOptions load_opts;
            load_opts.max_memory = options.max_layer_memory;
            load_opts.max_array_elements = options.max_array_elements;
            load_opts.max_archive_entries = options.max_archive_entries;
            load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
            load_opts.usdc_use_mmap = options.usdc_use_mmap;
            load_opts.usda_parse_options = options.usda_parse_options;
            load_opts.strict_aousd_conformance =
                options.strict_aousd_conformance;
            reg_->GetOrLoad(*resolver, assets[i].first, assets[i].second, &w,
                            &e, load_opts);
          }
        };
        ts.reserve(static_cast<size_t>(pf));
        for (int t = 0; t < pf; ++t) ts.emplace_back(fn);
        for (auto &t : ts) t.join();
      }

      // (b) Parallel per-prim index build into private worker Impls, then a
      // deterministic input-order merge.
      if (paths.size() > 1) {
        const int W = std::min<int>(nt, static_cast<int>(paths.size()));
        auto chunk_start = [&](int t) -> size_t {
          return static_cast<size_t>(t) * paths.size() / static_cast<size_t>(W);
        };

        std::vector<std::unique_ptr<Impl>> workers;
        workers.reserve(static_cast<size_t>(W));
        std::vector<std::string> wwarn(static_cast<size_t>(W));
        std::vector<std::string> werr(static_cast<size_t>(W));
        for (int t = 0; t < W; ++t) {
          std::unique_ptr<Impl> wp(new Impl());
          wp->resolver = resolver;
          wp->options = options;
          wp->load_rules_ = load_rules_;
          wp->reg_ = reg_;  // borrow the shared, parse-once registry
          wp->root_layer = root_layer;
          wp->root_identifier = root_identifier;
          wp->defer_instances_ = true;
          std::string sw, se;
          wp->InternLayerStack(root_layer, root_identifier, &sw, &se);  // root @0
          workers.push_back(std::move(wp));
        }

        // Release api_mu_ during the parallel build so that concurrent API
        // calls (ComputePrimIndex, SourcesForPath, etc.) are not blocked.
        // Workers operate on private Impls and only borrow the thread-safe
        // shared registry; they never touch shared cache state.
        wlk.unlock();

        std::vector<std::thread> bts;
        bts.reserve(static_cast<size_t>(W));
        for (int t = 0; t < W; ++t) {
          Impl *wk = workers[static_cast<size_t>(t)].get();
          size_t s = chunk_start(t), e = chunk_start(t + 1);
          std::string *pw = &wwarn[static_cast<size_t>(t)];
          std::string *pe = &werr[static_cast<size_t>(t)];
          bts.emplace_back([wk, s, e, &paths, pw, pe]() {
            for (size_t i = s; i < e; ++i) {
              wk->ComputePrimIndex(paths[i], pw, pe);
            }
          });
        }
        for (auto &t : bts) t.join();

        // Reacquire api_mu_ for the serial merge into shared cache state.
        wlk.lock();

        // Merge in input order (chunks are contiguous and ascending).
        for (int t = 0; t < W; ++t) {
          size_t s = chunk_start(t), e = chunk_start(t + 1);
          for (size_t i = s; i < e; ++i) {
            MergeWorkerIndex(*workers[static_cast<size_t>(t)], paths[i].str());
          }
        }
        for (int t = 0; t < W; ++t) {
          if (warn) *warn += wwarn[static_cast<size_t>(t)];
          if (err) *err += werr[static_cast<size_t>(t)];
          Impl &wk = *workers[static_cast<size_t>(t)];
          for (const std::string &dp : wk.deferred_payload_prims) {
            deferred_payload_prims.insert(dp);
          }
          // Fold per-worker typed diagnostics into the main Impl (workers each
          // own a private issues_, so this join is race-free).
          for (auto &iss : wk.issues_) issues_.push_back(std::move(iss));
        }
        return true;
      }
    }
#else
    {
      NEXT_PCP_LOCK(api_mu_);
      for (const Path &p : paths) ComputePrimIndex_locked(p, warn, err);
    }
#endif
#if defined(LIGHTUSD_ENABLE_THREAD)
    // The thread-enabled serial fallback reaches here with `wlk` already
    // holding api_mu_. Do not take NEXT_PCP_LOCK again: the default mutex is
    // intentionally non-recursive.
    for (const Path &p : paths) ComputePrimIndex_locked(p, warn, err);
#endif
    return true;
  }
