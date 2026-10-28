// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - CrateWriter::Impl top-level write orchestration.
// Included inside CrateWriter::Impl public section.

  explicit Impl(const CrateWriteOptions& options)
      : options_(options), writer_(buffer_, options.max_file_size_bytes) {
    // Initialize buffer
  }

  // `patch` (seek-write at an absolute output position), when given with a
  // sink, enables the streaming-value write: VALUE blocks go to the sink as
  // they are built and the bootstrap is backfilled through `patch`.
  CrateWriteResult Write(const Layer& layer, const CrateWriteSink* sink = nullptr,
                         const CrateWritePatch* patch = nullptr) {
    result_ = CrateWriteResult();
    required_version_minor_ = 0;
    CratePhaseTimer timer(options_.enable_timing, "next_crate_write");
    auto lap = [&](const char* phase) { timer.lap(phase); };

    if (!CheckWriteOptions()) return result_;
    if (options_.max_memory_bytes) {
      // Layer::memory_usage includes retained values and source buffers. Add a
      // conservative per-spec allowance for the writer's table, hash-map, and
      // relocation entries before reserving or constructing those structures.
      size_t spec_count = 0;
      for (const auto& prim : layer.prims()) {
        const size_t properties = prim.properties().slots().size();
        if (spec_count == (std::numeric_limits<size_t>::max)() ||
            properties > (std::numeric_limits<size_t>::max)() - spec_count - 1) {
          result_.error = "Crate estimated working set exceeds configured memory limit";
          return result_;
        }
        spec_count += 1 + properties;
      }
      constexpr size_t kEstimatedBytesPerSpec = 512;
      const size_t layer_bytes = layer.memory_usage();
      if (spec_count > ((std::numeric_limits<size_t>::max)() - layer_bytes) /
                           kEstimatedBytesPerSpec ||
          layer_bytes + spec_count * kEstimatedBytesPerSpec >
              options_.max_memory_bytes) {
        result_.error = "Crate estimated working set exceeds configured memory limit";
        return result_;
      }
    }
    ResetWriteState();

    // Reserve table/map capacity up front: a big scene appends millions of
    // paths/specs/fields and dedup entries, and the repeated geometric reallocs +
    // unordered_map rehashes are a top serial cost (alloc churn, ~18% on reference scene).
    // Each prim yields ~1 prim spec + 1 spec per property; sizes are heuristic
    // (over-reserve is cheaper than rehashing), so this never changes output.
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
    deferred_filler_ = layer.prim_filler();
#endif
    auto reserve = [&]() {
      size_t nprop = 0;
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
      // A deferred-fill layer has no properties yet (sizing hint only).
      if (!deferred_filler_)
#endif
      for (const auto& p : layer.prims()) nprop += PropertySpecBound(p);
      ReserveTables(layer.prims().size(), nprop, !(sink && patch && *patch));
    };
    // The reserves (path / spec / field tables, ~5 GB on a 60 M-spec write,
    // zero-filled path shards) touch none of the token / string tables
    // BuildTables fills, so a large write overlaps the two.
    bool reserving = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::thread reserver;
    if (layer.prims().size() >= kPrimTablesParallelMinPrims && ResolveThreads() > 1) {
      reserver = std::thread(reserve);
      reserving = true;
    }
#endif
    if (!reserving) reserve();

    if (sink && patch && *patch) {
      if (!BeginValueStream(sink)) {
#if defined(LIGHTUSD_ENABLE_THREAD)
        if (reserver.joinable()) reserver.join();
#endif
        return result_;
      }
    }

    lap("setup");
    // Build tables from layer
    BuildTables(layer);
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (reserver.joinable()) reserver.join();
#endif
    lap("tables");

    // Build fields, fieldsets, and specs from layer
    BuildFieldsAndSpecs(layer);
    lap("fields_specs");
    if (!result_.error.empty()) {
      result_.success = false;
      return result_;
    }
    return FinishWrite(sink, patch, timer);
  }

  // ---- Batched write (CrateWriter::AddBatchTables .. FinishBatched) ----
  // One composed layer delivered as consecutive namespace pre-order batches,
  // twice. Pass 1 interns every batch's BuildTables entries in prim order, so
  // the token/string tables end up exactly as BuildTables builds them for the
  // whole layer; pass 2 writes the batches' specs in the same order.

  void StreamTablesBatch(const Layer& batch, const std::vector<uint8_t>& repeated) {
    if (!stream_meta_done_) {
      result_ = CrateWriteResult();
      required_version_minor_ = 0;
      if (!CheckWriteOptions()) return;
      ResetWriteState();
      stream_paths_.clear();
      stream_root_done_ = false;
      stream_nprim_ = stream_nprop_ = 0;
      InternFixedTokens(batch);
      stream_meta_done_ = true;
    }
    PropNameTable& name_table = GetPropNameTable();
    const auto& prims = batch.prims();
    for (size_t i = 0; i < prims.size(); ++i) {
      if (i < repeated.size() && repeated[i]) continue;
      (void)InternPrimTables(prims[i], name_table);
      ++stream_nprim_;
      stream_nprop_ += PropertySpecBound(prims[i]);
    }
  }

  bool StreamWriteBegin(const CrateWriteSink* sink) {
    if (!result_.error.empty()) return false;
    serial_sections_ = true;
    ReserveTables(stream_nprim_, stream_nprop_, false);
    return BeginValueStream(sink);
  }

  bool StreamWriteBatch(
      Layer& batch, const std::vector<uint8_t>& spine,
      const std::vector<uint8_t>& repeated,
      const std::unordered_map<std::string, std::vector<std::string>>& full_children) {
    if (!result_.error.empty()) return false;
    const size_t n = batch.prims().size();
    // A full child list in the order Layer::apply_namespace_ordering gives
    // the linked children: the names `order` lists first, then the rest.
    auto ordered = [](std::vector<std::string> names,
                      const std::vector<std::string>& order) {
      if (order.empty() || names.empty()) return names;
      std::vector<std::string> out;
      std::vector<uint8_t> taken(names.size(), 0);
      out.reserve(names.size());
      for (const std::string& wanted : order) {
        for (size_t k = 0; k < names.size(); ++k) {
          if (!taken[k] && names[k] == wanted) {
            out.push_back(names[k]);
            taken[k] = 1;
            break;
          }
        }
      }
      for (size_t k = 0; k < names.size(); ++k)
        if (!taken[k]) out.push_back(std::move(names[k]));
      return out;
    };
    if (!stream_root_done_) {
      auto roots = full_children.find("/");
      std::vector<std::string> root_names;
      if (roots != full_children.end()) {
        root_names = ordered(roots->second, batch.meta().rootPrimOrder);
      } else {
        for (uint32_t ri : batch.root_indices())
          if (const PrimSpec* rp = batch.prim(ri)) root_names.push_back(rp->name());
      }
      EmitPseudoRootSpec(batch, &root_names);
      stream_root_done_ = true;
    }
    // Every name was interned in pass 1: these are lookups.
    const size_t tokens_before = tokens_.size();
    prim_name_tokens_.assign(n, 0);
    for (size_t i = 0; i < n; ++i)
      prim_name_tokens_[i] = InternToken(batch.prims()[i].name());
    stream_child_tokens_.clear();
    for (size_t i = 0; i < n; ++i) {
      if (i >= spine.size() || !spine[i] || repeated[i]) continue;
      auto it = full_children.find(batch.prims()[i].path().str());
      if (it == full_children.end()) continue;
      const PrimMetadata& meta = batch.prims()[i].meta();
      std::vector<uint32_t>& toks = stream_child_tokens_[static_cast<uint32_t>(i)];
      for (const std::string& cn : ordered(it->second, meta.primOrder())) {
        if (IsVariantHolderName(cn)) continue;
        toks.push_back(InternToken(cn));
      }
    }
    if (tokens_.size() != tokens_before) {
      result_.error = "batched write: a prim name is missing from the pass-1 tables";
      return false;
    }
    // The path table keeps views of the prim paths (moved out below, after
    // the batch is written); a short path's text lives inside its string
    // object, so give it heap storage that survives the move.
    for (size_t i = 0; i < n; ++i) {
      if (repeated[i]) continue;
      const std::string& p = batch.prims()[i].path().str();
      if (p.size() >= 16) continue;
      std::string heap;
      heap.reserve(16);
      heap = p;
      batch.prim(static_cast<uint32_t>(i))->set_path(Path(std::move(heap)));
    }
    skip_prims_ = &repeated;
    BuildPrimSpecs(batch);
    skip_prims_ = nullptr;
    stream_child_tokens_.clear();
    for (size_t i = 0; i < n; ++i) {
      if (!repeated[i])
        stream_paths_.push_back(batch.prim(static_cast<uint32_t>(i))->take_path());
    }
    if (!result_.error.empty()) {
      result_.success = false;
      return false;
    }
    return true;
  }

  CrateWriteResult StreamFinish(const CrateWriteSink* sink,
                                const CrateWritePatch* patch) {
    stream_meta_done_ = false;
    if (!result_.error.empty()) {
      result_.success = false;
      return result_;
    }
    CratePhaseTimer timer(options_.enable_timing, "next_crate_write");
    return FinishWrite(sink, patch, timer);
  }

  // Upper bound of a prim's property specs: one per slot, plus relationships
  // and time-sampled properties, which are not always slots. Reserving it
  // keeps the spec / path / fieldset vectors from doubling (and copying GBs)
  // near the end of a large write.
  static size_t PropertySpecBound(const PrimSpec& p) {
    return p.properties().slots().size() + p.extra_property_spec_bound();
  }

  // Version and file-size options a write can start with.
  bool CheckWriteOptions() {
    const CrateVersion requested_version{options_.version_major,
                                         options_.version_minor,
                                         options_.version_patch};
    if (!requested_version.is_valid()) {
      result_.error = "Unsupported Crate output version " +
                      requested_version.to_string();
      return false;
    }
    if (options_.max_file_size_bytes &&
        options_.max_file_size_bytes < kCrateBootstrapSize) {
      result_.error = "Crate output exceeds configured file-size limit";
      return false;
    }
    return true;
  }

  // Clears all accumulated table state from a previous write.
  void ResetWriteState() {
    buffer_.clear();
    writer_.clearFailure();
    serial_sections_ = false;

    // Clear all accumulated table state from previous writes
    tokens_.clear();
    token_to_index_.clear();
    type_name_token_cache_.clear();
    declared_type_token_cache_.clear();
    ResetFieldTokens();
    prim_name_tokens_.clear();
    prop_name_tokens_.clear();
    children_from_links_ = false;
    string_indices_.clear();
    string_to_index_.clear();
    paths_.clear();
    for (PathShard& sh : path_shards_) {
      sh.slots.clear();
      sh.count = 0;
    }
    path_owned_.clear();
    path_fingerprints_.clear();
    fields_.clear();
    fieldset_offsets_.clear();
    flat_fieldsets_.clear();
    specs_.clear();
    value_data_.clear();
    value_offsets_.clear();
    arrays_passed_through_ = 0;
    arrays_reencoded_ = 0;
    block_slots_.clear();
    block_slots_used_ = 0;
    stream_dedup_.clear();
    stream_source_ranges_.clear();
    stream_values_ = false;
    value_sink_ = nullptr;
    value_stream_error_ = false;
    blocks_deduped_ = 0;
    // Reserve block index 0 (empty placeholder). The writer emits empty arrays
    // as a ValueRep with payload==0 / non-inlined (pxrUSD's empty-array marker);
    // reserving index 0 ensures no real block is ever referenced by payload 0,
    // so the offset-patch pass can safely leave payload-0 reps untouched.
    value_data_.push_back({TypeId::Invalid, std::vector<uint8_t>()});
    // writer_ already references buffer_; just clear buffer

    // Reserve space for bootstrap header
    buffer_.resize(kCrateBootstrapSize, 0);
  }

  void ReserveTables(size_t nprim, size_t nprop, bool staged) {
    const size_t nspec = nprim + nprop + 16;
    ReservePaths(nspec);
    specs_.reserve(nspec);
    fieldset_offsets_.reserve(nspec);
    // Replace the old per-spec vector-header reservation with the smaller
    // offsets plus an estimated three fields and one sentinel per spec.
    size_t flat_capacity = 0;
    if (safe::mul(nspec, size_t(4), &flat_capacity)) {
      flat_fieldsets_.reserve(flat_capacity);
    }
    fields_.reserve(nspec * 3);
    // Large writes: back the big append-only tables with huge pages (fewer
    // first-touch faults, and a process exit tears ~6 GB down in a fraction
    // of the 4 KiB-page cost).
    if (nspec >= (size_t(1) << 22)) {
      AdviseHugePages(paths_.data(), paths_.capacity() * sizeof(PathRec));
      AdviseHugePages(path_fingerprints_.data(),
                      path_fingerprints_.capacity() * sizeof(uint16_t));
      AdviseHugePages(fields_.tokens.data(), fields_.tokens.capacity() * sizeof(uint32_t));
      AdviseHugePages(fields_.reps.data(), fields_.reps.capacity() * sizeof(ValueRep));
      AdviseHugePages(flat_fieldsets_.data(), flat_fieldsets_.capacity() * sizeof(uint32_t));
      AdviseHugePages(fieldset_offsets_.data(),
                      fieldset_offsets_.capacity() * sizeof(uint32_t));
      AdviseHugePages(specs_.data(), specs_.capacity() * sizeof(CrateSpec));
    }
    if (staged) {
      // Staged VALUE blocks (the streaming write keeps none).
      value_data_.reserve(nprop + nprim + 16);
      // Most values inline or dedup; the table grows (amortized) past this.
      GrowBlockSlots(
          std::min<size_t>((nprop + nprim + 16) * 2, size_t(1) << 20));
    }
  }

  // Streaming-value write: the VALUE section starts right after a 64-byte
  // bootstrap placeholder (backfilled at the end), as in the staged layout.
  bool BeginValueStream(const CrateWriteSink* sink) {
    stream_values_ = true;
    value_sink_ = sink;
    value_start_offset_ = 64;
    value_stream_pos_ = 64;
    static const uint8_t kBootstrapPlaceholder[64] = {0};
    if (!(*sink)(kBootstrapPlaceholder, 64)) {
      result_.error = "sink aborted (bootstrap placeholder)";
      return false;
    }
    return true;
  }

  // Everything after the specs: synthesized paths and tokens, then the VALUE
  // layout, the structural sections, the TOC and the bootstrap.
  CrateWriteResult FinishWrite(const CrateWriteSink* sink,
                               const CrateWritePatch* patch,
                               CratePhaseTimer& timer) {
    auto lap = [&](const char* phase) { timer.lap(phase); };
    // Counts for the result (a batched write frees the tables early).
    const size_t n_specs = specs_.size();
    const size_t n_fields = fields_.size();

    // Structural payloads, built below (concurrently in a threaded build when
    // the tables are large) and appended in crate order.
    std::vector<uint8_t> tokens_payload;
    std::vector<uint8_t> fields_payload;
    std::vector<uint8_t> fieldsets_payload;
    std::vector<uint8_t> specs_payload;
    std::vector<uint8_t> paths_payload;
    bool paths_ok = true;
    std::string fields_error;
    bool fields_ok = true;
    auto build_fields = [&]() {
      fields_ok = BuildFieldsSectionPayload(&fields_payload, &fields_error);
    };
    auto build_fieldsets = [&]() {
      BuildFieldsetsSectionPayload(&fieldsets_payload);
    };
    auto build_specs = [&]() { BuildSpecsSectionPayload(&specs_payload); };
    bool early_started = false;
    // consume_prims freed most of the composed stage during fields_specs;
    // return those pages while the path tree and sections build (the
    // allocator would otherwise hold them to the end, and a process exit
    // pays for every resident page).
    MemoryTrimmer trimmer;
    if (consume_prims_ && specs_.size() >= (size_t(1) << 22)) trimmer.Request();
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Joins on every return path (declared after the payloads it fills).
    struct Joiner {
      std::vector<std::thread> threads;
      void join() {
        for (std::thread& th : threads)
          if (th.joinable()) th.join();
      }
      ~Joiner() { join(); }
    } early;
    // A streamed write's FIELDS / FIELDSETS / SPECS columns are final now
    // (value reps carry their final offsets; ancestor paths only append), so
    // those payloads build while the path tree is completed.
    if (stream_values_ && !serial_sections_ && ResolveThreads() > 1 &&
        fields_.size() + fieldset_offsets_.size() + specs_.size() >= 4096) {
      early.threads.emplace_back(build_fields);
      early.threads.emplace_back(build_fieldsets);
      early.threads.emplace_back(build_specs);
      early_started = true;
    }
#endif

    // Complete the path tree (synthesized ancestors) and pre-register every
    // path element token so they're included in the TOKENS section:
    // the PATHS payload only looks them up (built alongside TOKENS).
    bool paths_ready = false;  // paths_payload built (speculatively, below)
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (early_started) {
      // A composed stage normally has every ancestor path and element token
      // already, so the PATHS payload is built while the two read-only
      // pre-passes confirm that. If anything is missing, the speculative
      // payload is dropped and rebuilt after the additions (same bytes).
      // The PATHS build's parent lookups answer SynthesizeAncestorPaths'
      // dry run; the element-token check runs alongside.
      bool add_paths = false;
      std::thread th_paths(
          [&] { paths_ok = BuildPathsSectionPayload(&paths_payload, &add_paths); });
      const bool add_tokens = RegisterPathElementTokens(/*dry_run=*/true);
      th_paths.join();
      if (add_paths || add_tokens) {
        std::vector<uint8_t>().swap(paths_payload);
        paths_ok = true;
        SynthesizeAncestorPaths();
        RegisterPathElementTokens();
      } else {
        paths_ready = true;
      }
    } else
#endif
    {
      SynthesizeAncestorPaths();
      RegisterPathElementTokens();
    }
    const size_t n_paths = paths_.size();

    lap("path_tokens");
    // Write all sections and record offsets
    std::vector<CrateSection> sections;

    // VALUE DATA section must come FIRST (before structural sections), NOT in TOC.
    // pxrUSD stores VALUE data between bootstrap and structural sections.
    // The bootstrap TOC offset points past the VALUE data to the TOC itself.
    // Bootstrap is 64 bytes, so reserve 64 bytes before VALUE section.
    // WriteBootstrap at the end patches these first 64 bytes.
    buffer_.resize(64, 0);
    value_start_offset_ = 64;

    // Compute the VALUE section layout and patch field offsets. Streamed
    // blocks already sit at their final offsets (baked into every ValueRep),
    // so the section size is just how many bytes were streamed.
    uint64_t value_section_size = 0;
    if (stream_values_) {
      if (value_stream_error_) {
        result_.error = "sink aborted (value section)";
        return result_;
      }
      value_section_size = value_stream_pos_ - value_start_offset_;
    } else {
      value_section_size = ComputeValueLayoutAndPatch();
    }
    lap("value_layout");
    if (value_section_size >
        static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
      result_.error = "Crate output exceeds addressable size";
      return result_;
    }
    if (options_.max_file_size_bytes &&
        (value_section_size > options_.max_file_size_bytes ||
         kCrateBootstrapSize > options_.max_file_size_bytes -
                                   value_section_size)) {
      result_.error = "Crate output exceeds configured file-size limit";
      return result_;
    }
    if (options_.max_file_size_bytes) {
      writer_.setMaxSize(sink
          ? options_.max_file_size_bytes - value_section_size
          : options_.max_file_size_bytes);
    }

    // Streaming mode keeps the VALUE bytes OUT of buffer_ (they are streamed
    // straight from their source at the end), so the structural sections sit at
    // physical buffer offset 64 but their TRUE file offset is shifted past the
    // (absent) value section. struct_base is that shift; 0 in the in-memory path
    // where the value bytes are physically present in buffer_.
    const uint64_t struct_base = sink ? value_section_size : 0;

    if (!sink) {
      // Reserve the output buffer up-front. The VALUE section dominates the
      // output; without a reserve std::vector growth would double its capacity
      // repeatedly, transiently needing ~2x the output size — expensive and a
      // real risk near the wasm32 2 GB ceiling. Over-estimating only wastes
      // capacity; under-estimating just falls back to normal growth.
      {
        size_t est = 64;
        for (const auto& b : value_data_) est += b.size() + 8;  // +8 align slack
        est += est / 8 + (size_t(1) << 20);  // ~12% for structural sections + 1MB
        if (options_.max_file_size_bytes &&
            est > options_.max_file_size_bytes) {
          est = static_cast<size_t>(std::min<uint64_t>(
              options_.max_file_size_bytes,
              static_cast<uint64_t>((std::numeric_limits<size_t>::max)())));
        }
        if (est > buffer_.capacity()) buffer_.reserve(est);
      }
      EmitValueBytesToBuffer();
    }
    lap("value_emit");

    {
      auto build_tokens = [&]() { BuildTokensSectionPayload(&tokens_payload); };
      auto build_paths = [&]() {
        paths_ok = BuildPathsSectionPayload(&paths_payload);
      };
      bool prepared = false;
      if (serial_sections_) {
        // Batched (low-memory) write: one payload at a time, PATHS first, and
        // each source table freed as soon as its payload exists. Only the
        // compressed payloads wait for their place in file order.
        build_paths();
        std::vector<PathRec>().swap(paths_);
        std::vector<uint16_t>().swap(path_fingerprints_);
        for (PathShard& sh : path_shards_) {
          std::vector<uint64_t>().swap(sh.slots);
          sh.count = 0;
        }
        std::deque<std::string>().swap(path_owned_);
        std::deque<Path>().swap(stream_paths_);
        // With the path table gone the rest fit side by side.
        auto fields_task = [&]() {
          build_fields();
          fields_.tokens = std::vector<uint32_t>();
          fields_.reps = std::vector<ValueRep>();
        };
        auto fieldsets_task = [&]() {
          build_fieldsets();
          std::vector<uint32_t>().swap(flat_fieldsets_);
        };
        auto specs_task = [&]() {
          build_specs();
          std::vector<uint32_t>().swap(fieldset_offsets_);
          std::vector<CrateSpec>().swap(specs_);
        };
#if defined(LIGHTUSD_ENABLE_THREAD)
        if (ResolveThreads() > 1) {
          std::thread th_fields(fields_task);
          std::thread th_fieldsets(fieldsets_task);
          std::thread th_specs(specs_task);
          build_tokens();
          th_fields.join();
          th_fieldsets.join();
          th_specs.join();
        } else
#endif
        {
          fields_task();
          fieldsets_task();
          specs_task();
          build_tokens();
        }
        prepared = true;
      }
#if defined(LIGHTUSD_ENABLE_THREAD)
      if (!prepared && early_started) {
        if (paths_ready) {
          build_tokens();
        } else {
          std::thread th_paths(build_paths);
          build_tokens();
          th_paths.join();
        }
        early.join();
        prepared = true;
      }
      if (!prepared && ResolveThreads() > 1 &&
          tokens_.size() + fields_.size() + fieldset_offsets_.size() + specs_.size() >=
              4096) {
        std::thread th_fields(build_fields);
        std::thread th_fieldsets(build_fieldsets);
        std::thread th_specs(build_specs);
        std::thread th_paths(build_paths);
        build_tokens();
        th_fields.join();
        th_fieldsets.join();
        th_specs.join();
        th_paths.join();
        prepared = true;
      }
#endif
      if (!prepared) {
        build_tokens();
        build_fields();
        build_fieldsets();
        build_specs();
        build_paths();
      }
    }
    if (!paths_ok) {
      result_.error = "Crate PATHS: a path element token is missing";
      result_.success = false;
      return result_;
    }
    if (!fields_ok) {
      result_.error = fields_error;
      result_.success = false;
      return result_;
    }
    lap("structural_payloads");

    // A streamed write with no file-size cap sends each structural section
    // to the sink as soon as it exists instead of collecting them in buffer_
    // (which keeps only the bootstrap and whatever is not flushed yet).
    const bool direct = sink && stream_values_ && !options_.max_file_size_bytes;
    uint64_t flushed = 0;  // structural bytes already in the sink
    auto flush = [&]() -> bool {
      if (!direct || buffer_.size() <= 64) return true;
      if (!(*sink)(buffer_.data() + 64, buffer_.size() - 64)) return false;
      flushed += buffer_.size() - 64;
      buffer_.resize(64);
      return true;
    };
    auto pos = [&]() -> int64_t {
      return static_cast<int64_t>(buffer_.size() + flushed + struct_base);
    };

    // Append one prepared section payload and record its TOC entry.
    auto append_section = [&](const char* name,
                              const std::vector<uint8_t>& payload) -> bool {
      CrateSection sec;
      std::memset(sec.name, 0, sizeof(sec.name));
      std::strncpy(sec.name, name, sizeof(sec.name) - 1);
      if (direct) {
        if (!flush() ||
            (!payload.empty() && !(*sink)(payload.data(), payload.size()))) {
          result_.error = "sink aborted (structural sections)";
          return false;
        }
        sec.start = pos();
        flushed += payload.size();
        sec.size = static_cast<int64_t>(payload.size());
        sections.push_back(sec);
        return true;
      }
      sec.start = static_cast<int64_t>(buffer_.size() + struct_base);
      if (!payload.empty()) writer_.write_bytes(payload.data(), payload.size());
      if (writer_.failed()) {
        result_.error = "Crate output exceeds configured file-size limit";
        return false;
      }
      sec.size = static_cast<int64_t>(buffer_.size() + struct_base) - sec.start;
      sections.push_back(sec);
      return true;
    };

    // TOKENS section
    if (!append_section("TOKENS", tokens_payload)) return result_;
    std::vector<uint8_t>().swap(tokens_payload);

    // STRINGS section
    {
      CrateSection sec;
      std::memset(sec.name, 0, sizeof(sec.name));
      std::strncpy(sec.name, "STRINGS", sizeof(sec.name) - 1);
      sec.start = pos();
      WriteStringsSection();
      lap("STRINGS");
      if (writer_.failed()) {
        result_.error = "Crate output exceeds configured file-size limit";
        return result_;
      }
      sec.size = pos() - sec.start;
      sections.push_back(sec);
      if (!flush()) {
        result_.error = "sink aborted (structural sections)";
        return result_;
      }
    }

    // FIELDS section (written before VALUE so we know the offsets)
    if (!append_section("FIELDS", fields_payload)) {
      result_.success = false;
      return result_;
    }
    std::vector<uint8_t>().swap(fields_payload);

    // FIELDSETS section
    if (!append_section("FIELDSETS", fieldsets_payload)) return result_;
    std::vector<uint8_t>().swap(fieldsets_payload);

    // PATHS section
    if (!append_section("PATHS", paths_payload)) return result_;
    std::vector<uint8_t>().swap(paths_payload);
    lap("PATHS");

    // SPECS section
    if (!append_section("SPECS", specs_payload)) return result_;

    // Write TOC at the end (VALUE section is NOT included in TOC)
    int64_t toc_offset = pos();
    WriteTOC(sections);
    if (writer_.failed()) {
      result_.error = "Crate output exceeds configured file-size limit";
      return result_;
    }

    // Write bootstrap header at the beginning
    const uint64_t complete_size = sink
        ? static_cast<uint64_t>(64) + value_section_size +
              flushed + static_cast<uint64_t>(buffer_.size() - 64)
        : static_cast<uint64_t>(buffer_.size());
    if (options_.max_file_size_bytes &&
        complete_size > options_.max_file_size_bytes) {
      result_.error = "Crate output exceeds configured file-size limit";
      return result_;
    }
    WriteBootstrap(toc_offset);

    // Streaming emit: bootstrap (64 B) -> VALUE bytes (streamed from source) ->
    // structural sections + TOC (the only part staged in buffer_). buffer_ holds
    // [bootstrap(64)][TOKENS..SPECS][TOC] with the VALUE section logically spliced
    // in after the bootstrap; stream the three parts in file order.
    if (sink && stream_values_) {
      // The placeholder bootstrap and the whole VALUE section are already in
      // the sink: append the structural sections + TOC, then backfill the real
      // bootstrap at offset 0.
      if (buffer_.size() > 64 &&
          !(*sink)(buffer_.data() + 64, buffer_.size() - 64)) {
        result_.error = "sink aborted (structural sections)";
        return result_;
      }
      if (!(*patch)(0, buffer_.data(), 64)) {
        result_.error = "sink aborted (bootstrap backfill)";
        return result_;
      }
    } else if (sink) {
      if (!(*sink)(buffer_.data(), 64)) {
        result_.error = "sink aborted (bootstrap)";
        return result_;
      }
      if (!StreamValueBytes(*sink)) {
        result_.error = "sink aborted (value section)";
        return result_;
      }
      if (buffer_.size() > 64 &&
          !(*sink)(buffer_.data() + 64, buffer_.size() - 64)) {
        result_.error = "sink aborted (structural sections)";
        return result_;
      }
    }

    lap("toc_stream");
    result_.success = true;
    result_.bytes_written =
        sink ? (64 + value_section_size + flushed + (buffer_.size() - 64))
             : buffer_.size();
    result_.token_count = tokens_.size();
    result_.string_count = string_indices_.size();
    result_.path_count = n_paths;
    result_.spec_count = n_specs;
    result_.field_count = n_fields;
    result_.arrays_passed_through = arrays_passed_through_;
    result_.arrays_reencoded = arrays_reencoded_;
    result_.blocks_deduped = blocks_deduped_;

    return result_;
  }

  const std::vector<uint8_t>& buffer() const { return buffer_; }
  // Move the finished output buffer out (avoids copying the whole crate -- the
  // VALUE section alone is multi-GB on large scenes). The Impl is not reused after
  // a Write() without a fresh clear(), so moving here is safe.
  std::vector<uint8_t> take_buffer() { return std::move(buffer_); }
