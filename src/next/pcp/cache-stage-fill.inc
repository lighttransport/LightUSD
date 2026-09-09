// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache stage opinion-fill pass.
// Included inside Cache::Impl.

  // Per-prim opinion records collected by the structure pass, drained by
  // FillOpinions. Member (not local) so a single fill can cover every root.
  // SrcCache values live in a deque and remain address-stable across inserts.
  // Retain the resolved source pointer directly instead of copying every path
  // and hashing it again during the opinion pass.
  std::vector<std::pair<uint32_t, const std::vector<Src>*>> fill_;

  static bool IsPreviewProperty(const std::string &name) {
    if (name == "extent" || name == "extentsHint" ||
        name == "visibility" || name == "purpose" ||
        name == "xformOpOrder" || name == "radius" ||
        name == "height" || name == "width" || name == "size" ||
        name == "axis" || name == "orientation" ||
        name == "primvars:displayColor" ||
        name == "primvars:displayOpacity") {
      return true;
    }
    if (name.compare(0, 8, "xformOp:") == 0) return true;
    return name == "projection" || name == "horizontalAperture" ||
           name == "verticalAperture" ||
           name == "horizontalApertureOffset" ||
           name == "verticalApertureOffset" || name == "focalLength" ||
           name == "clippingRange" || name == "clippingPlanes" ||
           name == "fStop" || name == "focusDistance" ||
           name == "shutter:open" || name == "shutter:close" ||
           name == "exposure";
  }

  // Narrow strong-to-weak opinion pass for spatial startup hints. It never
  // touches topology, point-instancer arrays, materials, textures or Ptex.
  void ComposePreviewOpinions(const std::vector<Src> &srcs, PrimSpec *out) {
    if (!out) return;
    for (const Src &s : srcs) {
      if (s.variant) {
        for (const VariantProperty &property : s.variant->properties) {
          if (!IsPreviewProperty(property.name) ||
              out->property_value(property.name)) continue;
          out->add_property(property.name, property.value, property.flags);
        }
        continue;
      }
      if (s.suppress_site_specs) continue;
      const std::vector<SpecRef> &specs = SpecsFor(s);
      for (const SpecRef &source : specs) {
        const PrimSpec *spec = source.spec;
        if (!spec) continue;
        if (out->type_name().empty() && !spec->type_name().empty()) {
          out->set_type_name(spec->type_name());
        }
        for (const PropSlot &slot : spec->properties().slots()) {
          if (slot.is_relationship()) continue;
          const std::string name(GetPropNameTable().get(slot.name_id));
          if (!IsPreviewProperty(name) || out->property_value(slot.name_id)) {
            continue;
          }
          const Value *value = spec->property_value(slot.name_id);
          if (!value) continue;
          out->add_property(slot.name_id, *value, slot.flags);
          if (const std::string *type_name = spec->property_type_name(name)) {
            out->set_property_type_name(name, *type_name);
          }
        }
      }
    }
    if (!ComposedActive(srcs)) {
      out->meta().active = false;
      out->meta().active_authored = true;
    }
  }

  enum PreviewEndpoint : uint8_t {
    PreviewEndpointNone = 0,
    PreviewEndpointBound = 1,
    PreviewEndpointCamera = 2,
  };

  PreviewEndpoint ClassifyPreviewEndpoint(const std::vector<Src> &srcs) {
    bool bound = false;
    for (const Src &s : srcs) {
      if (s.variant) {
        for (const VariantProperty &property : s.variant->properties) {
          if (property.name == "extent" || property.name == "extentsHint") {
            bound = true;
          }
        }
        continue;
      }
      if (s.suppress_site_specs) continue;
      for (const SpecRef &source : SpecsFor(s)) {
        const PrimSpec *spec = source.spec;
        if (!spec) continue;
        if (spec->type_name() == "Camera") return PreviewEndpointCamera;
        if (spec->property("extent") || spec->property("extentsHint")) {
          bound = true;
        }
      }
    }
    return bound ? PreviewEndpointBound : PreviewEndpointNone;
  }

  // Build a compact hierarchy containing only bound/camera endpoints and their
  // ancestors. A full Layer::Clone duplicates every PrimSpec in a large scene
  // and defeated the checkpoint's latency goal even though its values were
  // cheap shared backing.
  Layer BuildPreviewLayer(const Layer &structure) {
    const size_t count = structure.prim_count();
    std::vector<uint32_t> parent(count, UINT32_MAX);
    for (uint32_t i = 0; i < count; ++i) {
      const PrimSpec *prim = structure.prim(i);
      if (!prim) continue;
      for (uint32_t child : prim->child_indices()) {
        if (child < count) parent[child] = i;
      }
    }

    std::vector<uint8_t> selected(count, 0);
    size_t selected_bounds = 0;
    constexpr size_t kMaxPreviewBounds = 20000;
    for (const auto &record : fill_) {
      if (record.first >= count || !record.second) continue;
      const PreviewEndpoint endpoint =
          ClassifyPreviewEndpoint(*record.second);
      if (endpoint == PreviewEndpointNone) continue;
      if (endpoint == PreviewEndpointBound &&
          selected_bounds >= kMaxPreviewBounds) continue;
      if (endpoint == PreviewEndpointBound) ++selected_bounds;
      uint32_t index = record.first;
      while (index != UINT32_MAX && !selected[index]) {
        selected[index] = 1;
        index = parent[index];
      }
    }

    Layer preview;
    std::vector<uint32_t> remap(count, UINT32_MAX);
    std::vector<std::pair<uint32_t, const std::vector<Src>*>> preview_fill;
    preview_fill.reserve(fill_.size());
    for (const auto &record : fill_) {
      const uint32_t old_index = record.first;
      if (old_index >= count || !selected[old_index]) continue;
      const PrimSpec *source = structure.prim(old_index);
      if (!source) continue;
      PrimSpec prim(source->name());
      prim.set_path(source->path());
      const uint32_t new_index = preview.add_prim(std::move(prim));
      remap[old_index] = new_index;
      preview_fill.push_back({new_index, record.second});
    }
    for (uint32_t old_index = 0; old_index < count; ++old_index) {
      const uint32_t new_index = remap[old_index];
      if (new_index == UINT32_MAX) continue;
      const uint32_t old_parent = parent[old_index];
      if (old_parent != UINT32_MAX && remap[old_parent] != UINT32_MAX) {
        preview.set_parent(new_index, remap[old_parent]);
      } else {
        preview.add_root(new_index);
      }
    }

    GetPropNameTable().freeze();
    // This set is capped at 20k. A serial pass avoids thread launch and lock
    // traffic, and keeps this latency-only path independent of anonymous Src
    // linkage (important for strict GCC warning builds).
    for (const auto &record : preview_fill) {
      if (record.second) {
        ComposePreviewOpinions(*record.second,
                               preview.prim_mutable(record.first));
      }
    }
    return preview;
  }

  // OPINION-FILL PASS: compose each slot's opinions from its (already cached)
  // sources. Slots are disjoint PrimSpec objects, sources_cache is only read, and
  // add_prim is done -- so distinct slots fill concurrently. Byte-identical to
  // the serial ComposeInto: the same ComposeOpinions runs on the same (slot,
  // srcs) pairs; only the order across independent slots changes.
  void FillOpinions(Layer *out) {
    // The serial structure pass has parsed every referenced layer, so the full
    // set of property names is now interned. Freeze the name table: the parallel
    // fill below does heavy find() (and intern() that all HIT) on it, and the
    // per-call shared_lock otherwise contends purely on the lock's cache line.
    // Frozen, those go lock-free. A genuinely new name would unfreeze and fall
    // back to locking.
    GetPropNameTable().freeze();
    auto do_range = [&](size_t b, size_t e) {
      for (size_t i = b; i < e; ++i) {
        const std::vector<Src> *srcs = fill_[i].second;
        if (!srcs) continue;
        if (PrimSpec *ps = out->prim_mutable(fill_[i].first)) {
          ComposeOpinions(*srcs, ps);
        }
      }
    };
#if defined(LIGHTUSD_ENABLE_THREAD)
    int nt = options.num_threads;
    if (nt < 0) {
      nt = std::min<int>(static_cast<int>(std::thread::hardware_concurrency()),
                         kMaxExecutionThreads);
    }
    if (nt > 1 && fill_.size() >= 2048) {
      if (nt > kMaxExecutionThreads) nt = kMaxExecutionThreads;
      // Namespace order clusters heavy mesh/material prims. Static contiguous
      // partitions leave workers idle when one district contains most of the
      // authored properties, so claim modest batches dynamically instead.
      const size_t opinion_batch = options.opinion_batch_size != 0
                                       ? options.opinion_batch_size
                                       : std::max<size_t>(
                                             1, fill_.size() /
                                                    (static_cast<size_t>(nt) *
                                                     static_cast<size_t>(32)));
      std::atomic<size_t> next{0};
      std::vector<std::thread> pool;
      pool.reserve(nt);
      for (int t = 0; t < nt; ++t) {
        pool.emplace_back([&]() {
          for (;;) {
            const size_t b = next.fetch_add(opinion_batch,
                                            std::memory_order_relaxed);
            if (b >= fill_.size()) break;
            do_range(b, std::min(fill_.size(), b + opinion_batch));
          }
        });
      }
      for (std::thread &th : pool) th.join();
      return;
    }
#endif
    do_range(0, fill_.size());
  }
