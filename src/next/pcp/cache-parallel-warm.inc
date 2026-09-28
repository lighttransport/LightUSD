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
    const auto warm_t0 = std::chrono::steady_clock::now();
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
    // Load the external layers the scene reaches through reference/payload/
    // sublayer arcs on background threads while the serial frontier discovery
    // and structure walk run. Those walks otherwise load each arc target on
    // first touch -- a serial parse in the middle of LIVRPS resolution; with
    // the prefetch running they find the layer already parsed or join its
    // in-flight load in the registry. Byte-neutral: the registry keys layers by
    // resolved path and the prefetch loads with the exact options the arc and
    // sublayer paths use, so every consumer gets the same Layer it would have
    // parsed itself. Stopped (and joined) right after the structure pass.
    ArcLayerPrefetch prefetch;
    if (warm_threads > 1) StartArcLayerPrefetch(&prefetch, warm_threads);
    if (warm_threads > 1)
      ParallelWarmSources(root_names, warm_threads, warn, err);
#endif
    const auto structure_t0 = std::chrono::steady_clock::now();
    // Source warming has already discovered the namespace cardinality. Reserve
    // the flattened prim vector once before the structure pass; repeated
    // growth moves large PrimSpec objects and is especially expensive on the
    // 300k+ node Scene C sub-scenes. Keep small scenes on Layer's normal growth
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
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Every layer the composed namespace needs is loaded by now; anything the
    // prefetch still has queued was not reached (e.g. arcs on inactive prims).
    StopArcLayerPrefetch(&prefetch, warn);
#endif
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
    std::vector<std::pair<uint32_t, const std::vector<Src>*>>().swap(fill_);
    // FillOpinions emptied the Src vectors it composed; drop the whole cache
    // (its path-string keys included) so a later SourcesForPath re-resolves
    // instead of reading an emptied entry.
    if (options.release_build_sources) sources_cache.clear();
    const auto fill_t1 = std::chrono::steady_clock::now();
    if (options.enable_timing) {
      prof_compose_ns_ +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              fill_t1 - fill_t0).count();
    }
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
      LIGHTUSD_LOG_I("[next_build] warm=" +
                  FormatMilliseconds(elapsed_ms(structure_t0 - warm_t0)) +
                  "ms sources=" +
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
  // --- background arc-layer prefetch ---------------------------------------

  // Transitive loader for the layers reachable through reference/payload/
  // sublayer arcs, run on background threads during BuildStage (see there).
  // It only warms the thread-safe, parse-once LayerRegistry; composition
  // itself never reads from it, so it cannot change any composed result.
  struct ArcLayerPrefetch {
    struct Item {
      std::string asset;   // authored asset path
      std::string anchor;  // identifier of the authoring layer
    };
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Item> queue;
    std::unordered_set<std::string> seen;  // resolved identifiers
    size_t active = 0;
    bool stop = false;
    std::vector<std::thread> threads;
    std::string warn;  // load warnings (the registry reports them only once)

    ~ArcLayerPrefetch() { Join(); }
    void Join() {
      {
        std::lock_guard<std::mutex> lk(mu);
        stop = true;
      }
      cv.notify_all();
      for (std::thread &t : threads) t.join();
      threads.clear();
    }
  };

  LayerLoadOptions ArcLayerLoadOptions() const {
    // Must match the options ProcessArc / AppendLayerAndSublayers load with:
    // the registry keys on the resolved path only, so the first loader's
    // options decide how the shared Layer is parsed.
    LayerLoadOptions load_opts;
    load_opts.max_memory = options.max_layer_memory;
    load_opts.max_array_elements = options.max_array_elements;
    load_opts.max_archive_entries = options.max_archive_entries;
    load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
    load_opts.usdc_use_mmap = options.usdc_use_mmap;
    load_opts.usda_parse_options = options.usda_parse_options;
    load_opts.strict_aousd_conformance = options.strict_aousd_conformance;
    return load_opts;
  }

  // The external arc targets `layer` (identified by `id`) authors directly:
  // its sublayers and every prim's reference/payload assets. Expression-valued
  // asset paths and variant-scoped arcs are skipped (their targets depend on
  // composed state); the structure walk loads those on demand as before.
  void CollectArcLayerTargets(const Layer &layer, const std::string &id,
                              bool payloads,
                              std::vector<ArcLayerPrefetch::Item> *out) const {
    auto add = [&](const std::string &asset) {
      if (asset.empty() || asset.front() == '`') return;
      out->push_back({asset, id});
    };
    for (const std::string &sub : layer.meta().subLayers) add(sub);
    auto add_arcs = [&](const std::vector<std::string> &arcs, bool payload) {
      for (const std::string &s : arcs) {
        const CompositionArc a = payload ? Compositor::ParsePayload(s)
                                         : Compositor::ParseReference(s);
        add(a.asset_path);
      }
    };
    auto add_edit = [&](const ArcEdit &e, bool payload) {
      if (!e.authored) return;
      add_arcs(e.added, payload);
      add_arcs(e.prepended, payload);
      add_arcs(e.appended, payload);
    };
    for (const PrimSpec &ps : layer.prims()) {
      const PrimSpecMeta &m = ps.meta();
      add_arcs(m.references, false);
      if (payloads) add_arcs(m.payloads, true);
      if (const ArcListOpEdits *e = m.arc_edits()) {
        add_edit(e->references, false);
        if (payloads) add_edit(e->payloads, true);
      }
    }
  }

  void EnqueueArcLayerTargets(ArcLayerPrefetch *pf,
                              std::vector<ArcLayerPrefetch::Item> *items) {
    // Resolve outside the lock (the resolver may touch the filesystem).
    std::vector<std::string> ids(items->size());
    for (size_t i = 0; i < items->size(); ++i) {
      ids[i] = resolver->ResolvePath((*items)[i].asset, (*items)[i].anchor,
                                     !options.strict_aousd_conformance);
    }
    std::lock_guard<std::mutex> lk(pf->mu);
    for (size_t i = 0; i < items->size(); ++i) {
      if (ids[i].empty() || !pf->seen.insert(ids[i]).second) continue;
      pf->queue.push_back(std::move((*items)[i]));
    }
  }

  void StartArcLayerPrefetch(ArcLayerPrefetch *pf, int nt) {
    if (!reg_ || !resolver) return;
    // Payload targets are prefetched only under the plain load-everything
    // policy; a callback or load rule may decline them.
    const bool payloads = options.load_payloads && !options.payload_policy &&
                          !options.payload_policy_with_prim &&
                          load_rules_.empty();
    const LayerLoadOptions load_opts = ArcLayerLoadOptions();
    std::vector<ArcLayerPrefetch::Item> seeds;
    const LayerStack &root_stack = layer_stacks[0];
    for (size_t i = 0; i < root_stack.layers.size(); ++i) {
      const std::string &id = i < root_stack.layer_identifiers.size()
                                  ? root_stack.layer_identifiers[i]
                                  : root_stack.identifier;
      pf->seen.insert(id);
      CollectArcLayerTargets(*root_stack.layers[i], id, payloads, &seeds);
    }
    EnqueueArcLayerTargets(pf, &seeds);
    if (pf->queue.empty()) return;
    auto worker = [this, pf, payloads, load_opts]() {
      for (;;) {
        ArcLayerPrefetch::Item item;
        {
          std::unique_lock<std::mutex> lk(pf->mu);
          pf->cv.wait(lk, [pf]() {
            return pf->stop || !pf->queue.empty() || pf->active == 0;
          });
          if (pf->stop || pf->queue.empty()) {
            pf->cv.notify_all();  // queue drained with nothing in flight
            return;
          }
          item = std::move(pf->queue.front());
          pf->queue.pop_front();
          ++pf->active;
        }
        std::string w, e;
        std::shared_ptr<Layer> layer = reg_->GetOrLoad(
            *resolver, item.asset, item.anchor, &w, &e, load_opts);
        std::vector<ArcLayerPrefetch::Item> found;
        if (layer) {
          const std::string id = resolver->ResolvePath(
              item.asset, item.anchor, !options.strict_aousd_conformance);
          CollectArcLayerTargets(*layer, id, payloads, &found);
          EnqueueArcLayerTargets(pf, &found);
        }
        // Load errors are dropped: a failed load is not cached, so the
        // structure walk retries it and reports it at the authoring site.
        {
          std::lock_guard<std::mutex> lk(pf->mu);
          --pf->active;
          pf->warn += w;
        }
        pf->cv.notify_all();
      }
    };
    const size_t n = std::min<size_t>(static_cast<size_t>(nt),
                                      std::max<size_t>(1, pf->queue.size()));
    pf->threads.reserve(n);
    for (size_t t = 0; t < n; ++t) pf->threads.emplace_back(worker);
  }

  void StopArcLayerPrefetch(ArcLayerPrefetch *pf, std::string *warn) {
    pf->Join();
    if (warn) *warn += pf->warn;
    pf->warn.clear();
  }

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

  // Namespace pre-order position of a prim in the warm walk: the root prim's
  // index followed by the child index at each level. Lexicographic comparison
  // == namespace pre-order == the order the serial BuildStage visits prims.
  using WarmOrder = std::vector<uint32_t>;

  // One subtree root to warm. `protos` lists the claimed instance prototypes
  // (outermost first) the prim lies under: their children's sources are the
  // FILTERED pre-seed (see PreseedPrototypeChildren), and a prim below them
  // derives its sources from that seed. A worker taking a donated task
  // re-creates those seeds in its own cache before resolving the prim, so it
  // never derives a prototype-subtree prim from unfiltered sources.
  struct WarmTask {
    Path p;
    uint32_t depth = 0;
    WarmOrder order;
    std::shared_ptr<const std::vector<Path>> protos;
  };

  // State shared by the parallel-warm walk: the task queue (frontier roots,
  // plus subtrees a busy worker donates while others are idle) and the
  // instance claim table.
  //
  // Instancing-aware warming: the serial BuildStage composes the subtree of
  // only the FIRST member (namespace order) of each instance group -- its
  // prototype -- and stops at every other instance. The warm walk descends an
  // instanceable prim only when no EARLIER member of its instance key has been
  // claimed, so the prototype subtree the serial pass needs is always warmed
  // and the redundant subtrees of later instances are skipped. A later member
  // claimed before the earlier one arrives only costs time: the serial pass
  // never visits an instance's subtree.
  struct WarmShared {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<WarmTask> queue;
    size_t busy = 0;
    std::atomic<int> idle{0};
    std::unordered_map<std::string, WarmOrder> first_member;  // key -> order

    // True when `order` is the earliest member of `key` seen so far.
    bool Claim(std::string key, const WarmOrder &order) {
      std::lock_guard<std::mutex> lk(mu);
      auto ins = first_member.emplace(std::move(key), order);
      if (ins.second) return true;
      if (!(order < ins.first->second)) return false;
      ins.first->second = order;
      return true;
    }

    // Pop a task, waiting while a busy worker may still donate one. False once
    // the queue is drained and no worker is busy.
    bool Pop(WarmTask *task) {
      std::unique_lock<std::mutex> lk(mu);
      idle.fetch_add(1, std::memory_order_relaxed);
      cv.wait(lk, [this]() { return !queue.empty() || busy == 0; });
      idle.fetch_sub(1, std::memory_order_relaxed);
      if (queue.empty()) {
        cv.notify_all();
        return false;
      }
      *task = std::move(queue.front());
      queue.pop_front();
      ++busy;
      return true;
    }
    void Done() {
      std::lock_guard<std::mutex> lk(mu);
      if (--busy == 0 && queue.empty()) cv.notify_all();
    }
    // Move the `n` front (shallowest) entries of a worker's pending stack to
    // the queue.
    void Donate(std::deque<WarmTask> *stack, size_t n) {
      {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < n; ++i) {
          queue.push_back(std::move(stack->front()));
          stack->pop_front();
        }
      }
      cv.notify_all();
    }
  };

  // Pre-seed the children of instance prototype `proto` (whose composed
  // sources are `srcs`) with exactly the filtered derivation the serial
  // BuildStage uses (its subtree_filtered path): descendants of an instance
  // compose only from the instance's key-defining arcs. Returns false when no
  // source is key-defining (the serial walk then derives the children from
  // the unfiltered sources, as for any prim).
  bool PreseedPrototypeChildren(const Path &proto, const std::vector<Src> &srcs,
                                std::vector<std::string> *children,
                                std::string *warn, std::string *err) {
    std::vector<Src> shared_srcs;
    for (const Src &s : srcs) {
      if (!s.variant && Mapping(s.map_idx).MapsFrom(s.site)) {
        shared_srcs.push_back(s);
      }
    }
    if (shared_srcs.empty()) return false;
    *children = ComposeChildNames(shared_srcs);
    for (const std::string &cn : *children) {
      const Path child = proto.append_child(cn);
      const std::string ck = SourcesKey(0, child.str());
      if (!HasCachedSources(ck)) {
        std::vector<Src> base =
            DeriveChildSources(shared_srcs, cn, child.str(), warn, err);
        GetOrCreateCachedSources(ck) = ExpandList(base, child.str(), warn, err);
      }
    }
    return true;
  }

  // Resolve one prim of the warm walk and compute the children the serial
  // BuildStage would descend into, mirroring its instancing rules. Returns
  // false when the serial walk composes no subtree below `it` (inactive prim,
  // or an instance that is not the first member of its group); the prototype
  // member gets its children pre-seeded and is appended to `*child_protos`.
  bool ExpandWarmNode(const WarmTask &it, WarmShared *shared,
                      std::vector<std::string> *children,
                      std::shared_ptr<const std::vector<Path>> *child_protos,
                      std::string *warn, std::string *err) {
    children->clear();
    *child_protos = it.protos;
    if (it.depth > options.max_namespace_depth) return false;
    const std::vector<Src> &srcs = SourcesForPath(it.p, warn, err);
    // An inactive prim composes no subtree (and forms no instance).
    if (!ComposedActive(srcs)) return false;
    if (options.detect_instances && IsInstanceableSources(srcs)) {
      if (!shared->Claim(ComputeInstanceKeyImpl(srcs), it.order))
        return false;  // a later instance: the serial walk skips its subtree
      if (PreseedPrototypeChildren(it.p, srcs, children, warn, err)) {
        auto protos = std::make_shared<std::vector<Path>>();
        if (it.protos) *protos = *it.protos;
        protos->push_back(it.p);
        *child_protos = std::move(protos);
        return true;
      }
    }
    *children = ComposeChildNames(srcs);
    return true;
  }

  static WarmTask MakeChildWarmTask(
      const WarmTask &parent, const std::string &name, uint32_t index,
      const std::shared_ptr<const std::vector<Path>> &protos) {
    WarmTask child;
    child.p = parent.p.append_child(name);
    child.depth = parent.depth + 1;
    child.order.reserve(parent.order.size() + 1);
    child.order = parent.order;
    child.order.push_back(index);
    child.protos = protos;
    return child;
  }

  // Worker-side: warm this (private, SEEDED) Impl's sources_cache for the whole
  // subtree rooted at `root`, following the same child namespace the serial
  // compose walks (ComposeChildNames). The Impl was seeded with a snapshot of the
  // main's layer-stack table + ancestor sources_cache, so the root->frontier
  // ancestors are already cached (no re-resolution) and any new reference resolves
  // against the SAME anchors the serial build sees -> identical layer-stack
  // identifiers. Fills a cache only; missed paths still resolve identically in the
  // serial BuildStage.
  //
  // Load balancing: subtrees are very uneven (one Island element holds a
  // quarter of the namespace, one tree prototype 15k prims), so while another
  // worker is idle a busy worker donates its shallowest pending subtree to the
  // shared queue. The taker re-derives the donated prim's ancestors into its
  // own cache -- first re-creating the filtered seeds of the prototypes it lies
  // under (`protos`), so it resolves exactly the sources the donor would have.
  void WarmSubtree(WarmTask root, WarmShared *shared,
                   std::unordered_set<std::string> *seeded_protos,
                   std::string *warn, std::string *err) {
    if (root.protos) {
      std::vector<std::string> ignored;
      for (const Path &proto : *root.protos) {
        if (!seeded_protos->insert(proto.str()).second) continue;
        const std::vector<Src> &srcs = SourcesForPath(proto, warn, err);
        PreseedPrototypeChildren(proto, srcs, &ignored, warn, err);
      }
    }
    std::deque<WarmTask> stack;  // back = next (pre-order); front = shallowest
    stack.push_back(std::move(root));
    std::vector<std::string> children;
    std::shared_ptr<const std::vector<Path>> child_protos;
    while (!stack.empty()) {
      if (stack.size() > 1 &&
          shared->idle.load(std::memory_order_relaxed) > 0) {
        // Hand over the shallowest half: a wide node (thousands of leaf
        // children) would otherwise trickle out one tiny task per step.
        shared->Donate(&stack, stack.size() / 2);
      }
      WarmTask it = std::move(stack.back());
      stack.pop_back();
      if (!ExpandWarmNode(it, shared, &children, &child_protos, warn, err)) {
        continue;
      }
      if (child_protos && child_protos != it.protos) {
        seeded_protos->insert(it.p.str());  // seeded here, in this cache
      }
      for (size_t ci = children.size(); ci-- > 0;) {
        stack.push_back(MakeChildWarmTask(
            it, children[ci], static_cast<uint32_t>(ci), child_protos));
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
    // Frontier size: a few roots per worker. Discovery is SERIAL (every level
    // resolves its prims on this thread), so digging deeper for a finer
    // frontier costs more than it saves now that busy workers donate subtrees
    // to idle ones (see WarmSubtree). Measured on Island (32 threads), the
    // next-refactor tuning nt*16 / depth 12 cut the worker phase ~1.4 s but
    // added ~2.6 s of serial discovery (it now also expands instance
    // prototypes), a net ~1 s loss.
    const size_t want = static_cast<size_t>(nt) * 4;
    const uint32_t kMaxFrontierDepth = 8;
    // Discovery applies the workers' instancing rules (ExpandWarmNode) with
    // the shared claim table, so a top-level instance prototype -- often the
    // bulk of a scene -- is split across workers instead of warmed whole by
    // one; its filtered child seeds land in the main cache, which every worker
    // reads as its seed. Items are kept in namespace order.
    WarmShared shared;
    std::vector<WarmTask> level;
    level.reserve(root_names.size());
    for (size_t i = 0; i < root_names.size(); ++i) {
      WarmTask task;
      task.p = Path("/" + root_names[i]);
      task.depth = 1;
      task.order.push_back(static_cast<uint32_t>(i));
      level.push_back(std::move(task));
    }
    {
      std::vector<std::string> children;
      std::shared_ptr<const std::vector<Path>> child_protos;
      for (uint32_t d = 1;
           d < kMaxFrontierDepth && level.size() < want;
           ++d) {
        std::vector<WarmTask> next;
        next.reserve(std::min(level.size() * 4, want));
        bool grew = false;
        for (WarmTask &entry : level) {
          if (!ExpandWarmNode(entry, &shared, &children, &child_protos, warn,
                              err)) {
            continue;  // nothing below composes; the serial walk resolves it
          }
          if (children.empty()) {
            next.push_back(std::move(entry));  // leaf: nothing to parallelize
            continue;
          }
          grew = true;
          for (size_t ci = 0; ci < children.size(); ++ci) {
            next.push_back(MakeChildWarmTask(entry, children[ci],
                                             static_cast<uint32_t>(ci),
                                             child_protos));
          }
        }
        level = std::move(next);
        if (!grew) break;
      }
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

    // Subtree donation (see WarmSubtree) keeps workers beyond the frontier
    // size busy, so use every worker thread.
    const int W = nt;
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
    // the main thread holds api_mu_ throughout. Dynamic work-stealing over the
    // frontier roots, plus subtree donation (see WarmSubtree) -- frontier
    // subtrees are very uneven, so static chunks leave workers idle behind one
    // heavy subtree.
    for (WarmTask &task : level) shared.queue.push_back(std::move(task));
    WarmShared *sh = &shared;
    std::vector<std::thread> ts;
    ts.reserve(static_cast<size_t>(W));
    for (int t = 0; t < W; ++t) {
      Impl *wk = workers[static_cast<size_t>(t)].get();
      std::string *pw = &wwarn[static_cast<size_t>(t)];
      std::string *pe = &werr[static_cast<size_t>(t)];
      ts.emplace_back([wk, sh, pw, pe]() {
        std::unordered_set<std::string> seeded_protos;
        WarmTask task;
        while (sh->Pop(&task)) {
          wk->WarmSubtree(std::move(task), sh, &seeded_protos, pw, pe);
          sh->Done();
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
