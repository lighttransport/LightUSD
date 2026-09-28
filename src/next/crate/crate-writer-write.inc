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

    const CrateVersion requested_version{options_.version_major,
                                         options_.version_minor,
                                         options_.version_patch};
    if (!requested_version.is_valid()) {
      result_.error = "Unsupported Crate output version " +
                      requested_version.to_string();
      return result_;
    }
    if (options_.max_file_size_bytes &&
        options_.max_file_size_bytes < kCrateBootstrapSize) {
      result_.error = "Crate output exceeds configured file-size limit";
      return result_;
    }
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
    buffer_.clear();
    writer_.clearFailure();

    // Clear all accumulated table state from previous writes
    tokens_.clear();
    token_to_index_.clear();
    string_indices_.clear();
    string_to_index_.clear();
    paths_.clear();
    path_hashes_.clear();
    path_slots_.clear();
    path_owned_.clear();
    fields_.clear();
    fieldsets_.clear();
    specs_.clear();
    value_data_.clear();
    value_offsets_.clear();
    arrays_passed_through_ = 0;
    arrays_reencoded_ = 0;
    block_slots_.clear();
    block_slots_used_ = 0;
    stream_dedup_.clear();
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

    // Reserve table/map capacity up front: a big scene appends millions of
    // paths/specs/fields and dedup entries, and the repeated geometric reallocs +
    // unordered_map rehashes are a top serial cost (alloc churn, ~18% on Island).
    // Each prim yields ~1 prim spec + 1 spec per property; sizes are heuristic
    // (over-reserve is cheaper than rehashing), so this never changes output.
    {
      size_t nprim = layer.prims().size();
      size_t nprop = 0;
      for (const auto& p : layer.prims()) nprop += p.properties().slots().size();
      const size_t nspec = nprim + nprop + 16;
      ReservePaths(nspec);
      specs_.reserve(nspec);
      fieldsets_.reserve(nspec);
      fields_.reserve(nspec * 3);
      value_data_.reserve(nprop + nprim + 16);
      // Most values inline or dedup; the table grows (amortized) past this.
      GrowBlockSlots(std::min<size_t>((nprop + nprim + 16) * 2, size_t(1) << 20));
    }

    if (sink && patch && *patch) {
      // Streaming-value write: the VALUE section starts right after a 64-byte
      // bootstrap placeholder (backfilled below), as in the staged layout.
      stream_values_ = true;
      value_sink_ = sink;
      value_start_offset_ = 64;
      value_stream_pos_ = 64;
      static const uint8_t kBootstrapPlaceholder[64] = {0};
      if (!(*sink)(kBootstrapPlaceholder, 64)) {
        result_.error = "sink aborted (bootstrap placeholder)";
        return result_;
      }
    }

    lap("setup");
    // Build tables from layer
    BuildTables(layer);
    lap("tables");

    // Build fields, fieldsets, and specs from layer
    BuildFieldsAndSpecs(layer);
    lap("fields_specs");
    if (!result_.error.empty()) {
      result_.success = false;
      return result_;
    }

    // Complete the path tree (synthesized ancestors) and pre-register every
    // path element token so they're included in the TOKENS section:
    // WritePathsSection runs AFTER the TOKENS section is built.
    SynthesizeAncestorPaths();
    RegisterPathElementTokens();

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

    // Prepare the independent structural payloads (concurrently in a threaded
    // build when the tables are large), then append every section in crate
    // order below.
    std::vector<uint8_t> tokens_payload;
    std::vector<uint8_t> fields_payload;
    std::vector<uint8_t> fieldsets_payload;
    std::vector<uint8_t> specs_payload;
    std::string fields_error;
    bool fields_ok = true;
    {
      auto build_tokens = [&]() { BuildTokensSectionPayload(&tokens_payload); };
      auto build_fields = [&]() {
        fields_ok = BuildFieldsSectionPayload(&fields_payload, &fields_error);
      };
      auto build_fieldsets = [&]() {
        BuildFieldsetsSectionPayload(&fieldsets_payload);
      };
      auto build_specs = [&]() { BuildSpecsSectionPayload(&specs_payload); };
      bool prepared = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
      if (ResolveThreads() > 1 &&
          tokens_.size() + fields_.size() + fieldsets_.size() + specs_.size() >=
              4096) {
        std::thread th_fields(build_fields);
        std::thread th_fieldsets(build_fieldsets);
        std::thread th_specs(build_specs);
        build_tokens();
        th_fields.join();
        th_fieldsets.join();
        th_specs.join();
        prepared = true;
      }
#endif
      if (!prepared) {
        build_tokens();
        build_fields();
        build_fieldsets();
        build_specs();
      }
    }
    if (!fields_ok) {
      result_.error = fields_error;
      result_.success = false;
      return result_;
    }
    lap("structural_payloads");

    // Append one prepared section payload and record its TOC entry.
    auto append_section = [&](const char* name,
                              const std::vector<uint8_t>& payload) -> bool {
      CrateSection sec;
      std::memset(sec.name, 0, sizeof(sec.name));
      std::strncpy(sec.name, name, sizeof(sec.name) - 1);
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
      sec.start = static_cast<int64_t>(buffer_.size() + struct_base);
      WriteStringsSection();
      lap("STRINGS");
      if (writer_.failed()) {
        result_.error = "Crate output exceeds configured file-size limit";
        return result_;
      }
      sec.size = static_cast<int64_t>(buffer_.size() + struct_base) - sec.start;
      sections.push_back(sec);
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
    {
      CrateSection sec;
      std::memset(sec.name, 0, sizeof(sec.name));
      std::strncpy(sec.name, "PATHS", sizeof(sec.name) - 1);
      sec.start = static_cast<int64_t>(buffer_.size() + struct_base);
      WritePathsSection();
      lap("PATHS");
      if (writer_.failed()) {
        result_.error = "Crate output exceeds configured file-size limit";
        return result_;
      }
      sec.size = static_cast<int64_t>(buffer_.size() + struct_base) - sec.start;
      sections.push_back(sec);
    }

    // SPECS section
    if (!append_section("SPECS", specs_payload)) return result_;

    // Write TOC at the end (VALUE section is NOT included in TOC)
    int64_t toc_offset = static_cast<int64_t>(buffer_.size() + struct_base);
    WriteTOC(sections);
    if (writer_.failed()) {
      result_.error = "Crate output exceeds configured file-size limit";
      return result_;
    }

    // Write bootstrap header at the beginning
    const uint64_t complete_size = sink
        ? static_cast<uint64_t>(64) + value_section_size +
              static_cast<uint64_t>(buffer_.size() - 64)
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
        sink ? (64 + value_section_size + (buffer_.size() - 64)) : buffer_.size();
    result_.token_count = tokens_.size();
    result_.string_count = string_indices_.size();
    result_.path_count = paths_.size();
    result_.spec_count = specs_.size();
    result_.field_count = fields_.size();
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
