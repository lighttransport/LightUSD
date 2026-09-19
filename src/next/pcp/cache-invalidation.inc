// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache invalidation, payload loading, and load rules.
// Included inside Cache::Impl.

  std::string ComputeInstanceKey(const Path &path, std::string *warn,
                                 std::string *err) {
    NEXT_PCP_LOCK(api_mu_);
    const std::vector<Src> &srcs = SourcesForPath(path, warn, err);
    return ComputeInstanceKeyImpl(srcs);
  }
  // --- invalidation -------------------------------------------------------

  void DropIndex(const std::string &key) {
    auto it = index_to_sites.find(key);
    if (it != index_to_sites.end()) {
      for (const Site &s : it->second) {
        auto sit = site_to_indices.find(s);
        if (sit != site_to_indices.end()) sit->second.erase(key);
      }
      index_to_sites.erase(it);
    }
    index_cache.erase(key);
    sources_cache.erase(key);
    composed_cache_.erase(key);
    composed_children_.erase(key);
    DropInstancing(key);
  }

  void Invalidate(const Path &prim_path) {
    NEXT_PCP_LOCK(api_mu_);
    Invalidate_locked(prim_path);
  }

  // Assumes api_mu_ is already held (called by LoadPayload / UnloadPayload).
  void Invalidate_locked(const Path &prim_path) {
    const std::string base = prim_path.str();
    std::set<std::string> to_drop;

    // index_cache is a std::map (sorted). Use lower_bound to scan only entries
    // at/under base instead of the entire map (Fix #12).
    for (auto it = index_cache.lower_bound(base);
         it != index_cache.end() && IsPathAtOrUnder(it->first, base); ++it) {
      to_drop.insert(it->first);
    }
    for (const auto &kv : site_to_indices) {
      if (IsPathAtOrUnder(kv.first.prim_path, base)) {
        for (const std::string &dep : kv.second) to_drop.insert(dep);
      }
    }
    for (const std::string &k : to_drop) DropIndex(k);

    // sources_cache + deferred state may have descendants not in index_cache.
    // Stack-qualified intermediate keys ("@N:/path" / "!N:/path") are not prim
    // paths, so IsPathAtOrUnder never matches them; drop them all (they are
    // cheap to recompute and a stale relocate-source / arc-target expansion
    // under the invalidated subtree would otherwise survive).
    sources_cache.erase_if([&](const std::string &k) {
      const bool qualified = !k.empty() && (k[0] == '@' || k[0] == '!');
      return qualified || IsPathAtOrUnder(k, base);
    });
    for (auto it = deferred_payload_prims.begin();
         it != deferred_payload_prims.end();) {
      if (IsPathAtOrUnder(*it, base)) it = deferred_payload_prims.erase(it);
      else ++it;
    }
    // Phase 10: drop lazily-composed specs at/under the path.
    for (auto it = composed_cache_.begin(); it != composed_cache_.end();) {
      if (IsPathAtOrUnder(it->first, base)) it = composed_cache_.erase(it);
      else ++it;
    }
    for (auto it = composed_children_.begin(); it != composed_children_.end();) {
      if (IsPathAtOrUnder(it->first, base)) it = composed_children_.erase(it);
      else ++it;
    }
    // Diagnostics are a per-edit-cycle log: an invalidation begins a fresh
    // accumulation (matches GetCompositionIssues' contract; also bounds growth
    // across repeated recompositions). Issues are global, not path-keyed.
    issues_.clear();
  }

  void InvalidateLayer(const std::string &layer_id) {
    NEXT_PCP_LOCK(api_mu_);
    std::set<std::string> to_drop;
    for (const auto &kv : site_to_indices) {
      if (kv.first.layer_id == layer_id) {
        for (const std::string &dep : kv.second) to_drop.insert(dep);
      }
    }
    for (const std::string &k : to_drop) DropIndex(k);
    // spec cache keys are (stack_idx, site), not prim paths; layer contents
    // changed so clear it entirely. (Src::specs_ memo pointers into it live
    // only in sources_cache entries, cleared just below.) The arc-target memo
    // caches resolved layers, so a dropped/re-parsed layer must re-resolve.
    spec_cache_by_stack_.clear();
    arc_target_memo_.clear();
    // Sources and lazy composed specs can be populated without a PrimIndex, so
    // site_to_indices is not a complete dependency map for them. Be conservative
    // until lazy composition records its own layer-site dependencies.
    sources_cache.clear();
    composed_cache_.clear();
    composed_children_.clear();
    issues_.clear();  // fresh diagnostics for the recomposition.
    reg_->Drop(layer_id);
  }

  bool ReloadLayer(const std::string &layer_id, std::string *warn,
                   std::string *err) {
    NEXT_PCP_LOCK(api_mu_);

    bool known_dependency = layer_id == root_identifier;
    for (const LayerStack &stack : layer_stacks) {
      for (const std::string &identifier : stack.layer_identifiers) {
        if (identifier == layer_id) {
          known_dependency = true;
          break;
        }
      }
      if (known_dependency) break;
    }
    if (!known_dependency) {
      if (err) *err += "Cannot reload a non-dependency layer: " + layer_id + "\n";
      return false;
    }

    LayerLoadOptions load_opts;
    load_opts.max_memory = options.max_layer_memory;
    load_opts.max_array_elements = options.max_array_elements;
    load_opts.max_archive_entries = options.max_archive_entries;
    load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
    load_opts.usdc_use_mmap = options.usdc_use_mmap;
    load_opts.usda_parse_options = options.usda_parse_options;
    load_opts.strict_aousd_conformance = options.strict_aousd_conformance;

    std::shared_ptr<Layer> replacement;
    if (!AssetResolver::GetIdentifierScheme(layer_id).empty() ||
        resolver->HasAssetReader()) {
      std::vector<uint8_t> bytes;
      if (resolver->ReadAsset(layer_id, &bytes, err)) {
        replacement = LoadLayerFromMemory(layer_id, bytes.data(), bytes.size(),
                                          warn, err, load_opts);
      }
    } else {
      // `layer_id` was previously resolved and is already part of this cache's
      // dependency set. Reload it directly: applying the untrusted resolver's
      // absolute-path rejection a second time would make safe reloads
      // impossible even though no new filesystem authority is being granted.
      replacement = LoadLayerFromFile(layer_id, warn, err, load_opts);
    }
    if (!replacement) return false;
    // Keep the previously registered dependency intact until parsing the
    // replacement succeeds. Preload atomically replaces the registry entry.
    reg_->Preload(layer_id, replacement);

    if (layer_id == root_identifier) root_layer = replacement;

    // PrimIndex and Src objects retain layer-stack indices and pointers into
    // layer specs. Rebuild the stack table as a unit so a reload also observes
    // edits to subLayerPaths, offsets, relocates, and expression variables.
    index_cache.clear();
    sources_cache.clear();
    sources_in_progress.clear();
    reloc_content_in_progress.clear();
    site_to_indices.clear();
    index_to_sites.clear();
    composed_cache_.clear();
    composed_children_.clear();
    spec_cache_by_stack_.clear();
    arc_target_memo_.clear();
    deferred_payload_prims.clear();
    prototype_by_key.clear();
    instance_key_order.clear();
    prototype_of.clear();
    instances_by_prototype.clear();
    issues_.clear();
    layer_stacks.clear();
    stack_by_id.clear();

    const uint32_t root_stack =
        InternLayerStack(root_layer, root_identifier, warn, err);
    return root_stack != UINT32_MAX;
  }

  // --- payloads -----------------------------------------------------------

  bool LoadPayload(const Path &prim_path, bool with_descendants,
                   std::string *warn, std::string *err) {
    NEXT_PCP_LOCK(api_mu_);
    if (with_descendants) {
      load_rules_.LoadWithDescendants(prim_path.str());
    } else {
      load_rules_.LoadWithoutDescendants(prim_path.str());
    }
    Invalidate_locked(prim_path);
    ComputePrimIndex_locked(prim_path, warn, err);  // recompose now.
    return true;
  }

  bool LoadPayloads(const std::vector<Path> &prim_paths,
                    bool with_descendants) {
    NEXT_PCP_LOCK(api_mu_);
    Path common;
    for (const Path &path : prim_paths) {
      if (path.empty()) continue;
      if (with_descendants) load_rules_.LoadWithDescendants(path.str());
      else load_rules_.LoadWithoutDescendants(path.str());
      if (common.empty()) {
        common = path;
      } else {
        while (!common.empty() &&
               !IsPathAtOrUnder(path.str(), common.str())) {
          common = common.parent();
        }
      }
    }
    if (!common.empty()) Invalidate_locked(common);
    return true;
  }

  bool UnloadPayload(const Path &prim_path) {
    NEXT_PCP_LOCK(api_mu_);
    load_rules_.Unload(prim_path.str());
    Invalidate_locked(prim_path);
    // Recompose so the deferred-payload set (and HasDeferredPayload) reflects
    // the unload immediately -- Invalidate() drops the deferred entries under
    // the path, and ExpandArcs repopulates them on the next composition.
    std::string warn, err;
    ComputePrimIndex_locked(prim_path, &warn, &err);
    return true;
  }

  void SetLoadRules(const LoadRules &rules) {
    NEXT_PCP_LOCK(api_mu_);
    // Idempotent call: viewer code and round-trip tests re-apply the same rule
    // set routinely, and nuking every prim index / composed spec / instancing
    // group for it forced a full stage recomposition for nothing.
    if (rules.rules() == load_rules_.rules()) return;
    load_rules_ = rules;
    // A wholesale rule change can affect any prim; drop everything (lazy
    // rebuild). Deferred state is recomputed as prims recompose.
    index_cache.clear();
    sources_cache.clear();
    site_to_indices.clear();
    index_to_sites.clear();
    deferred_payload_prims.clear();
    prototype_by_key.clear();
    prototype_of.clear();
    instances_by_prototype.clear();
    // Must be cleared with the rest of the instancing state. Leaving it grew
    // without bound across recompositions AND corrupted prototype numbering:
    // the parallel warm path picks prototypes by scanning it in REVERSE
    // (cache-parallel-warm.inc), so stale/duplicated keys changed which
    // prototype won.
    instance_key_order.clear();
    composed_cache_.clear();  // Phase 10
    composed_children_.clear();
    issues_.clear();  // fresh diagnostics for the recomposition.
  }

  void SetVariantSelections(
      const CompositionOptions::VariantSelectionMap &selections) {
    NEXT_PCP_LOCK(api_mu_);
    std::set<std::string> changed;
    for (const auto &entry : options.variant_overrides_by_path) {
      const auto it = selections.find(entry.first);
      if (it == selections.end() || it->second != entry.second) {
        changed.insert(entry.first);
      }
    }
    for (const auto &entry : selections) {
      const auto it = options.variant_overrides_by_path.find(entry.first);
      if (it == options.variant_overrides_by_path.end() ||
          it->second != entry.second) {
        changed.insert(entry.first);
      }
    }
    options.variant_overrides_by_path = selections;
    for (const std::string &path : changed) Invalidate_locked(Path(path));
  }
