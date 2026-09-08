// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache opinion composition and lazy prim/index composition.
// Included inside Cache::Impl.

  std::vector<std::string> ComposeInto(const std::vector<Src> &srcs,
                                        PrimSpec *out) {
    ComposeOpinions(srcs, out);
    // Pass 2 (weak->strong): compose child-name ORDER.
    return ComposeChildNames(srcs);
  }
  // ComposeInto's Pass 1 (opinion merge), factored out so the parallel compose
  // can fill PrimSpec opinion slots independently after a serial structure pass
  // has fixed every prim's index/parent/child-order. Writes only into `out`
  // (and interns property names into the shared, locked PropNameTable), so
  // distinct slots fill concurrently without interfering.
  void ComposeOpinions(const std::vector<Src> &srcs, PrimSpec *out);

  // Resolved `active` for a prim's sources: strongest authored opinion wins
  // (fill-absent, mirroring ComposeOpinions' metadata merge — a selected
  // variant authors active only when it deactivates). Used by BuildStage to
  // prune the subtree of a deactivated prim WITHOUT composing its opinions
  // (those are deferred to the parallel fill pass).
  bool ComposedActive(const std::vector<Src> &srcs) {
    for (const Src &s : srcs) {
      if (s.variant) {
        if (!s.variant->active) return false;
        continue;  // variant active=true is not an authored opinion
      }
      const std::vector<SpecRef> &specs = SpecsFor(s);
      for (const SpecRef &sr : specs) {
        if (sr.spec->meta().active_authored) return sr.spec->meta().active;
      }
    }
    return true;
  }

  // Composed child-name ORDER for a prim's sources (weak->strong): iterate sources
  // and their layer-stack specs in reverse (weakest first); within a single layer
  // the primChildren are in authored (forward) order. Append unseen names. This is
  // ComposeInto's Pass 2, factored out so the parallel source-warming pre-pass
  // discovers exactly the same child namespace the serial compose walks.
  std::vector<std::string> ComposeChildNames(const std::vector<Src> &srcs) {
    // Namespace edits authored by any contributing layer stack: a relocated-away
    // name is PROHIBITED here (it no longer exists at this parent, even when a
    // weaker stack still authors a spec for it -- the "shadow spec" case), and a
    // relocated-in name is a child of this prim regardless of where its content
    // physically lives.
    const ChildRelocations edits = RelocationsAt(srcs);
    std::vector<std::string> child_names;
    std::set<std::string> seen_child;
    for (auto sit = srcs.rbegin(); sit != srcs.rend(); ++sit) {
      const Src &s = *sit;
      if (s.variant) continue;  // variant child prims not modeled yet
      // Opinions authored at a relocation SOURCE path are prohibited (pxr
      // "salted earth"): their child prims must NOT enter the composed child
      // list either. The Src itself stays for traversal (chained relocations),
      // but its own spec children are suppressed — a child authored at the
      // source (ErrorInvalidReferenceToRelocationSource PreReloChild) leaked
      // while arc-delivered children (model.usd ModelChild) correctly follow.
      if (s.suppress_site_specs) continue;
      const std::vector<SpecRef> &specs = SpecsFor(s);
      for (auto rit = specs.rbegin(); rit != specs.rend(); ++rit) {
        const PrimSpec *spec = rit->spec;
        const Layer *layer = rit->layer;
        for (uint32_t ci : spec->child_indices()) {
          const PrimSpec *cs = layer->prim(ci);
          if (!cs) continue;
          // Bracketed variant HOLDER prims ("{set=sel}") are namespace markers,
          // not real children -- the selected holder's descendants are grafted
          // onto the host as a Variant source (see ExpandArcs). Skip the markers
          // so they never leak into the composed child list.
          if (!cs->name().empty() && cs->name().front() == '{') continue;
          if (edits.departed.count(cs->name())) continue;
          if (seen_child.insert(cs->name()).second) {
            child_names.push_back(cs->name());
          }
        }
      }
    }
    for (const std::string &name : edits.arrived) {
      // A name that one stack relocates IN and another relocates OUT is a
      // chained-relocation pass-through (CHARACTER -> Character -> Model): the
      // content flows to the final destination, so the intermediate name must
      // NOT materialize (pxr collapses the chain). TrickyMultipleRelocations5.
      if (edits.departed.count(name)) continue;
      if (seen_child.insert(name).second) child_names.push_back(name);
    }
    return child_names;
  }

  // --- lazy per-prim composition (Phase 10) -------------------------------

  // Compose just `prim_path` on first access (cached), reusing the exact
  // source-resolution + opinion-merge of BuildStageRec's per-prim step, but
  // without walking/emitting the whole namespace. Returns nullptr if the prim
  // authors no opinions. Assumes api_mu_ is held.
  const PrimSpec *ComposePrim_locked(const Path &prim_path, std::string *warn,
                                     std::string *err) {
    const std::string key = prim_path.str();
    auto it = composed_cache_.find(key);
    if (it != composed_cache_.end()) return it->second.get();

    const std::vector<Src> &srcs = SourcesForPath(prim_path, warn, err);
    if (!AnyAuthors(srcs)) return nullptr;

    auto spec = std::unique_ptr<PrimSpec>(new PrimSpec(prim_path.name()));
    spec->set_path(prim_path);
    std::vector<std::string> children = ComposeInto(srcs, spec.get());

    const PrimSpec *ret = spec.get();
    composed_cache_.emplace(key, std::move(spec));
    composed_children_[key] = std::move(children);
    return ret;
  }

  // --- ComputePrimIndex ---------------------------------------------------

  // Public entry: takes the lock once, then runs the lock-free worker.
  const PrimIndex *ComputePrimIndex(const Path &prim_path, std::string *warn,
                                    std::string *err) {
#if defined(LIGHTUSD_ENABLE_THREAD) && defined(LIGHTUSD_NEXT_FINE_LOCKS)
    // F4 fast path: a shared lock lets cache hits resolve concurrently. On a
    // miss we fall through to the exclusive build; ComputePrimIndex_locked
    // re-checks index_cache under the write lock, so a prim contended by many
    // threads is still built exactly once.
    {
      NEXT_PCP_READ_LOCK(api_mu_);
      auto it = index_cache.find(prim_path.str());
      if (it != index_cache.end()) return it->second.get();
    }
#endif
    NEXT_PCP_WRITE_LOCK(api_mu_);
    return ComputePrimIndex_locked(prim_path, warn, err);
  }

  // Assumes api_mu_ is already held. Internal callers (LoadPayload /
  // UnloadPayload / the serial PrewarmPrimIndices path) use this directly so the
  // lock is never re-entered (F6: the mutex is non-recursive).
  const PrimIndex *ComputePrimIndex_locked(const Path &prim_path,
                                           std::string *warn, std::string *err) {
    const std::string key = prim_path.str();
    auto it = index_cache.find(key);
    if (it != index_cache.end()) return it->second.get();

    const std::vector<Src> &srcs = SourcesForPath(prim_path, warn, err);
    if (!AnyAuthors(srcs)) {
      return nullptr;  // prim does not exist in the composed namespace.
    }

    auto index = std::unique_ptr<PrimIndex>(new PrimIndex());
    index->SetPath(prim_path);
    index->SetLayerStacks(&layer_stacks);

    std::vector<uint16_t> order;
    std::vector<Site> sites;
    for (size_t i = 0; i < srcs.size(); ++i) {
      const Src &s = srcs[i];
      CompNode n;
      n.arc_type = (i == 0) ? ArcType::Root : s.arc_kind;
      n.parent = (i == 0) ? 0xFFFF : 0;
      n.layer_stack_idx = s.stack_idx;
      n.site_path_idx = InternPath(s.site);
      n.map_to_root = Mapping(s.map_idx);
      n.offset = s.offset;
      const std::vector<SpecRef> &specs = SpecsFor(s);
      if (!specs.empty()) {
        n.flags |= NodeFlags::HasSpecs;
        for (const SpecRef &sr : specs) {
          sites.push_back(Site{sr.layer_id.empty()
                                   ? layer_stacks[s.stack_idx].identifier
                                   : sr.layer_id,
                               s.site});
        }
      }
      uint16_t ni = index->AddNode(std::move(n));
      if (ni == PrimIndex::kInvalidNode) {
        AddIssue(ErrorCode::IndexCapacityExceeded, key,
                 "PrimIndex node count exceeds uint16 capacity for " + key, err);
        return nullptr;
      }
      if (i != 0) index->MutableNode(0).children.push_back(ni);
      order.push_back(ni);
    }
    index->SetStrengthOrder(std::move(order));
    // Bind the path table AFTER the node loop interned every site, so the
    // size snapshot covers all of this index's site_path_idx values (F3).
    index->SetPathTable(&path_table, path_table.size());

    // Per-prim cache: pick the prototype deterministically (min path) so the
    // grouping does not depend on the order/threads of ComputePrimIndex (F5).
    RegisterInstance(key, srcs, /*prefer_min=*/true);

    for (const Site &site : sites) site_to_indices[site].insert(key);
    index_to_sites[key] = std::move(sites);

    const PrimIndex *ret = index.get();
    index_cache.emplace(key, std::move(index));
    return ret;
  }
