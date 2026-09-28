// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - CrateWriter::Impl section emission and VALUE layout.
// Included inside CrateWriter::Impl private section.

  // ============================================================
  // Section writing
  // ============================================================

  // Structural section payloads (TOKENS, FIELDS, FIELDSETS, SPECS) are built
  // into their own buffers -- independently, so the threaded writer prepares
  // them concurrently -- and appended to the output in fixed crate order, which
  // keeps TOC offsets and output bytes identical at any thread count.
  static void WriteCompressedBytes(StreamWriter& out, const uint8_t* data,
                                   size_t size) {
    CompressResult cr = CompressCrateBlob(data, size);
    if (!cr.success) {
      // Fall back to storing uncompressed.
      out.write_u64(size);
      if (size) out.write_bytes(data, size);
    } else {
      out.write_u64(cr.data.size());
      if (!cr.data.empty()) out.write_bytes(cr.data.data(), cr.data.size());
    }
  }

  // Usd_IntegerCompression: delta encode + LZ4 (TfFastCompression format).
  static void WriteCompressedDeltaU32(StreamWriter& out, const uint32_t* vals,
                                      size_t count) {
    std::vector<uint8_t> delta = EncodeDeltaU32(vals, count);
    WriteCompressedBytes(out, delta.data(), delta.size());
  }

  void WriteBootstrap(int64_t toc_offset) {
    // Bootstrap is exactly 64 bytes: [ident(8)][version(8)][tocOffset(8)][reserved(40)]
    size_t pos = 0;
    std::memcpy(buffer_.data() + pos, kCrateMagic, 8); pos += 8;
    buffer_[pos++] = options_.version_major;
    buffer_[pos++] = std::max(options_.version_minor, required_version_minor_);
    buffer_[pos++] = options_.version_patch;
    buffer_[pos++] = 0; // pad
    buffer_[pos++] = 0; buffer_[pos++] = 0;
    buffer_[pos++] = 0; buffer_[pos++] = 0; // total 8 bytes for version
    writer_.patch_u64(pos, static_cast<uint64_t>(toc_offset)); pos += 8;
    // Reserved padding to 64 bytes
    while (pos < 64) buffer_[pos++] = 0;
  }

  void BuildTokensSectionPayload(std::vector<uint8_t>* payload) {
    StreamWriter out(*payload);
    // Build uncompressed token data: concatenated null-terminated strings
    std::vector<uint8_t> raw;
    size_t total = 0;
    for (const auto& token : tokens_) total += token.size() + 1;
    raw.reserve(total);  // avoid incremental reallocations
    for (const auto& token : tokens_) {
      raw.insert(raw.end(), token.begin(), token.end());
      raw.push_back(0);  // null terminator
    }

    out.write_u64(tokens_.size());
    out.write_u64(raw.size());  // uncompressed_size

    if (raw.empty()) {
      out.write_u64(0);  // compressed_size = 0
      return;
    }
    WriteCompressedBytes(out, raw.data(), raw.size());
  }

  void WriteStringsSection() {
    writer_.write_u64(string_indices_.size());
    for (uint32_t str_idx : string_indices_) {
      writer_.write_u32(str_idx);
    }
  }

  // Returns false (with *error set) when the payload cannot be sized.
  bool BuildFieldsSectionPayload(std::vector<uint8_t>* payload,
                                 std::string* error) {
    StreamWriter out(*payload);
    uint64_t num_fields = fields_.size();
    out.write_u64(num_fields);

    // Encode token indices using Usd_IntegerCompression (delta + LZ4)
    {
      std::vector<uint32_t> indices(num_fields);
      for (size_t i = 0; i < num_fields; i++) {
        indices[i] = fields_[i].token_index.value;
      }
      WriteCompressedDeltaU32(out, indices.data(), num_fields);
    }

    // Write compressed value reps (TfFastCompression LZ4 format)
    size_t value_rep_bytes = 0;
    if (!safe::mul(static_cast<size_t>(num_fields), size_t(8), &value_rep_bytes)) {
      *error = "Field value-rep byte size overflow";
      return false;
    }
    std::vector<uint8_t> raw_reps(value_rep_bytes);
    for (size_t i = 0; i < num_fields; i++) {
      uint64_t v = fields_[i].value_rep.raw();
      std::memcpy(raw_reps.data() + i * 8, &v, 8);
    }
    WriteCompressedBytes(out, raw_reps.data(), raw_reps.size());
    return true;
  }

  void BuildFieldsetsSectionPayload(std::vector<uint8_t>* payload) {
    StreamWriter out(*payload);
    // Build flat array: for each fieldset, write its field indices then a
    // 0xFFFFFFFF sentinel (matching pxrUSD and the old lightusd writer).
    size_t flat_size = 0;
    for (const auto& fieldset : fieldsets_) flat_size += fieldset.size() + 1;
    std::vector<uint32_t> flat;
    flat.reserve(flat_size);
    for (const auto& fieldset : fieldsets_) {
      flat.insert(flat.end(), fieldset.begin(), fieldset.end());
      flat.push_back(0xFFFFFFFF);  // sentinel
    }

    uint64_t flat_count = flat.size();
    out.write_u64(flat_count);
    // Use Usd_IntegerCompression (delta + LZ4)
    WriteCompressedDeltaU32(out, flat.data(), static_cast<size_t>(flat_count));
  }

  // Pre-order rank of a path character: the structural separators sort below
  // every name character ('/' < '.' < everything else), which makes a
  // byte-wise comparison of full path texts a pre-order of the path tree.
  static const uint32_t* PathCharRank() {
    static const uint32_t* const table = []() -> const uint32_t* {
      static uint32_t t[256];
      for (int c = 0; c < 256; ++c) t[c] = 2u + static_cast<uint32_t>(c);
      t[static_cast<unsigned char>('/')] = 0;
      t[static_cast<unsigned char>('.')] = 1;
      return t;
    }();
    return table;
  }
  static bool RankLess(std::string_view a, std::string_view b) {
    const uint32_t* rank = PathCharRank();
    const size_t n = (std::min)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
      const unsigned char ca = static_cast<unsigned char>(a[i]);
      const unsigned char cb = static_cast<unsigned char>(b[i]);
      if (ca != cb) return rank[ca] < rank[cb];
    }
    return a.size() < b.size();
  }
  // RankLess over the full texts of two path records (stem [+ '.' + name]).
  bool PathRecRankLess(uint32_t ia, uint32_t ib) const {
    const PathRec& a = paths_[ia];
    const PathRec& b = paths_[ib];
    const size_t la = a.stem.size() + (a.name.empty() ? 0 : 1 + a.name.size());
    const size_t lb = b.stem.size() + (b.name.empty() ? 0 : 1 + b.name.size());
    auto at = [](const PathRec& r, size_t k) -> unsigned char {
      if (k < r.stem.size()) return static_cast<unsigned char>(r.stem[k]);
      if (k == r.stem.size()) return static_cast<unsigned char>('.');
      return static_cast<unsigned char>(r.name[k - r.stem.size() - 1]);
    };
    const uint32_t* rank = PathCharRank();
    const size_t n = (std::min)(la, lb);
    for (size_t k = 0; k < n; ++k) {
      const unsigned char ca = at(a, k);
      const unsigned char cb = at(b, k);
      if (ca != cb) return rank[ca] < rank[cb];
    }
    return la < lb;
  }

  void WritePathsSection() {
    // The compressed path tree is the pre-order of all paths sorted by their
    // full text under PathCharRank (`/` < `.` < name characters). That order
    // is derived here from the path table's (stem, name) split without
    // building or globally sorting full path strings: every path's parent is
    // looked up (all ancestors exist; see SynthesizeAncestorPaths), each
    // parent's children are sorted -- child prims ("<p>/<elem>") before
    // properties ("<p>.<name>"), each by element text -- and the tree is
    // walked depth-first. Children of the root "/" (whose child texts do not
    // extend it with a separator) and parentless paths compare full texts.
    //
    // The empty path (SdfPath(), interned for a relocate-to-nothing target
    // `</a>: <>`) must NOT be encoded as a PATHS tree node: pxr excludes it
    // ("we do not write out the empty path", crateFile.cpp) and a real node
    // for it makes pxr reject the whole crate ("spec has empty path"). Keep it
    // in total_paths so its index stays valid; the reader leaves an unencoded
    // index as the empty path, matching pxr.
    const size_t N = paths_.size();
    constexpr uint32_t kNoNode = 0xFFFFFFFF;
    std::vector<uint32_t> parent(N, kNoNode);
    std::vector<std::string_view> elem(N);
    std::vector<uint8_t> encoded(N, uint8_t{1});
    std::vector<uint8_t> is_prop(N, uint8_t{0});
    std::vector<uint8_t> is_root(N, uint8_t{0});
    size_t num_paths = 0;
    for (size_t i = 0; i < N; ++i) {
      const PathRec& r = paths_[i];
      if (r.stem.empty() && r.name.empty()) {
        encoded[i] = 0;
        continue;
      }
      ++num_paths;
      if (!r.name.empty()) {
        is_prop[i] = 1;
        elem[i] = r.name;
        if (!r.stem.empty()) parent[i] = FindPath(r.stem);
        continue;
      }
      if (r.stem == "/") {
        is_root[i] = 1;
        continue;
      }
      const size_t sl = r.stem.rfind('/');
      if (sl == std::string_view::npos) {
        elem[i] = r.stem;  // relative path: a parentless node
        continue;
      }
      elem[i] = r.stem.substr(sl + 1);
      parent[i] = FindPath(sl == 0 ? std::string_view("/") : r.stem.substr(0, sl));
    }

    // Write total paths count AND encoded tree nodes count
    uint64_t total_paths = static_cast<uint64_t>(N);
    writer_.write_u64(total_paths);
    writer_.write_u64(static_cast<uint64_t>(num_paths));

    if (num_paths == 0) {
      // No paths other than root, write empty arrays
      std::vector<uint8_t> empty_delta = EncodeDeltaU32(nullptr, 0);
      CompressResult empty_cr = CompressCrateBlob(empty_delta.data(), empty_delta.size());
      writer_.write_u64(empty_cr.data.size());
      writer_.write_bytes(empty_cr.data.data(), empty_cr.data.size());
      writer_.write_u64(empty_cr.data.size());
      writer_.write_bytes(empty_cr.data.data(), empty_cr.data.size());
      writer_.write_u64(empty_cr.data.size());
      writer_.write_bytes(empty_cr.data.data(), empty_cr.data.size());
      return;
    }

    // Children lists (CSR, slot 0 = parentless nodes), in table order.
    std::vector<uint32_t> child_off(N + 2, 0);
    for (size_t i = 0; i < N; ++i) {
      if (!encoded[i]) continue;
      ++child_off[(parent[i] == kNoNode ? 0 : size_t(parent[i]) + 1) + 1];
    }
    for (size_t k = 0; k + 1 < child_off.size(); ++k) {
      child_off[k + 1] += child_off[k];
    }
    std::vector<uint32_t> children(num_paths);
    {
      std::vector<uint32_t> cursor(child_off.begin(), child_off.end() - 1);
      for (size_t i = 0; i < N; ++i) {
        if (!encoded[i]) continue;
        const size_t list = parent[i] == kNoNode ? 0 : size_t(parent[i]) + 1;
        children[cursor[list]++] = static_cast<uint32_t>(i);
      }
    }
    auto sort_list = [&](size_t list) {
      auto first = children.begin() + child_off[list];
      auto last = children.begin() + child_off[list + 1];
      if (last - first < 2) return;
      const bool full_text = (list == 0) || is_root[list - 1];
      if (full_text) {
        std::sort(first, last, [&](uint32_t a, uint32_t b) {
          return PathRecRankLess(a, b);
        });
      } else {
        std::sort(first, last, [&](uint32_t a, uint32_t b) {
          if (is_prop[a] != is_prop[b]) return is_prop[a] < is_prop[b];
          return RankLess(elem[a], elem[b]);
        });
      }
    };
    const size_t num_lists = N + 1;
    bool sorted_lists = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    const int nthreads = ResolveThreads();
    if (nthreads > 1 && num_paths >= 200000) {
      // Lists are disjoint slices; a strided split balances large parents.
      std::vector<std::thread> pool;
      pool.reserve(static_cast<size_t>(nthreads));
      for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t] {
          for (size_t list = static_cast<size_t>(t); list < num_lists;
               list += static_cast<size_t>(nthreads)) {
            sort_list(list);
          }
        });
      }
      for (auto& th : pool) th.join();
      sorted_lists = true;
    }
#endif
    if (!sorted_lists) {
      for (size_t list = 0; list < num_lists; ++list) sort_list(list);
    }

    // Depth-first pre-order walk.
    std::vector<uint32_t> order;
    order.reserve(num_paths);
    std::vector<uint8_t> has_sibling(N, uint8_t{0});
    {
      std::vector<std::pair<uint32_t, uint32_t>> stack;  // (list, next pos)
      stack.emplace_back(0u, child_off[0]);
      while (!stack.empty()) {
        auto& top = stack.back();
        if (top.second >= child_off[top.first + 1]) {
          stack.pop_back();
          continue;
        }
        const uint32_t node = children[top.second++];
        if (top.second < child_off[top.first + 1]) has_sibling[node] = 1;
        order.push_back(node);
        const uint32_t list = node + 1;
        if (child_off[list] < child_off[list + 1]) {
          stack.emplace_back(list, child_off[list]);
        }
      }
    }
    // Subtree sizes (reverse pre-order accumulation).
    std::vector<uint32_t> subtree(N, 1);
    for (size_t k = order.size(); k-- > 0;) {
      const uint32_t node = order[k];
      if (parent[node] != kNoNode) subtree[parent[node]] += subtree[node];
    }

    // Build 3 arrays: path indices, element token indices (negated for a
    // property), jump indices:
    //   jump  > 0 : child (next node) AND sibling (at +jump = subtree size)
    //   jump == 0 : sibling only;  -1: child only;  -2: leaf
    std::vector<uint32_t> path_indices(order.size());
    std::vector<uint32_t> element_tokens(order.size());
    std::vector<uint32_t> jump_indices(order.size());
    std::string elem_scratch;
    for (size_t k = 0; k < order.size(); ++k) {
      const uint32_t node = order[k];
      path_indices[k] = node;
      elem_scratch.assign(elem[node].data(), elem[node].size());
      int32_t tok = static_cast<int32_t>(
          InternToken(is_root[node] ? std::string() : elem_scratch));
      // Property element tokens are negated (pxr's is_property marker).
      if (!is_root[node] && is_prop[node]) tok = -tok;
      element_tokens[k] = static_cast<uint32_t>(tok);
      const bool has_child = subtree[node] > 1;
      int32_t jump;
      if (has_sibling[node] && has_child) {
        jump = static_cast<int32_t>(subtree[node]);
      } else if (has_sibling[node]) {
        jump = 0;
      } else if (has_child) {
        jump = -1;
      } else {
        jump = -2;
      }
      jump_indices[k] = static_cast<uint32_t>(jump);
    }

    // Write 3 compressed arrays using Usd_IntegerCompression (delta + LZ4)
    WriteCompressedDeltaU32(writer_, path_indices.data(), order.size());
    WriteCompressedDeltaU32(writer_, element_tokens.data(), order.size());
    WriteCompressedDeltaU32(writer_, jump_indices.data(), order.size());
  }

  void BuildSpecsSectionPayload(std::vector<uint8_t>* payload) {
    StreamWriter out(*payload);
    uint64_t num_specs = specs_.size();
    out.write_u64(num_specs);

    // Convert fieldset numbers to flat array offsets (matching pxrUSD format).
    // Build offset mapping: for each fieldset, its offset in the flat array
    // is the sum of all prior fieldsets' sizes (including sentinels).
    std::vector<uint32_t> fieldset_number_to_offset;
    {
      uint32_t current_offset = 0;
      for (const auto& fs : fieldsets_) {
        fieldset_number_to_offset.push_back(current_offset);
        current_offset += static_cast<uint32_t>(fs.size() + 1); // +1 for sentinel
      }
    }

    // Write 3 compressed integer arrays (delta+LZ4 Usd_IntegerCompression)
    std::vector<uint32_t> path_indices(num_specs);
    std::vector<uint32_t> fieldset_indices(num_specs);
    std::vector<uint32_t> spec_types(num_specs);
    for (size_t i = 0; i < num_specs; i++) {
      path_indices[i] = specs_[i].path_index.value;
      // Convert fieldset number → flat array offset
      uint32_t fs_num = specs_[i].fieldset_index.value;
      fieldset_indices[i] = (fs_num < fieldset_number_to_offset.size())
                            ? fieldset_number_to_offset[fs_num]
                            : 0;
      spec_types[i] = static_cast<uint32_t>(specs_[i].spec_type);
    }

    WriteCompressedDeltaU32(out, path_indices.data(), num_specs);
    WriteCompressedDeltaU32(out, fieldset_indices.data(), num_specs);
    WriteCompressedDeltaU32(out, spec_types.data(), num_specs);
  }

  void _PatchTimeSamplesBlock(uint64_t data_idx) {
    // Indirection format header:
    // [i64 fwd1=8][ValueRep times_rep][i64 fwd2=8][u64 count][ValueRep values[N]]
    // times_rep.payload and values[i].payload are block indices to patch to abs offsets.
    if (data_idx >= value_data_.size()) return;
    DataBlock& block = value_data_[data_idx];
    if (block.data.size() < 32) return; // minimum: 2*i64 + 1*u64 + times_rep

    // Patch times_rep at offset 8
    uint64_t vrep_raw;
    std::memcpy(&vrep_raw, block.data.data() + 8, 8);
    {
      ValueRep vrep(vrep_raw);
      if (!vrep.is_inlined()) {
        uint64_t payload = vrep.payload();
        if (payload != 0 && payload < value_offsets_.size()) {
          uint64_t abs_offset = value_start_offset_ + value_offsets_[static_cast<size_t>(payload)];
          vrep_raw = ValueRep::Make(vrep.type_id(), abs_offset,
                                     vrep.is_array(), false,
                                     vrep.is_compressed()).raw();
          std::memcpy(block.data.data() + 8, &vrep_raw, 8);
        }
      }
    }

    // Read num_values at offset 24
    uint64_t num_samples;
    std::memcpy(&num_samples, block.data.data() + 24, 8);
    if (num_samples == 0) return;

    // Patch each sample ValueRep starting at offset 32
    for (size_t i = 0; i < num_samples; i++) {
      size_t voff = 32 + i * 8;
      if (voff + 8 > block.data.size()) break;

      std::memcpy(&vrep_raw, block.data.data() + voff, 8);
      ValueRep vrep(vrep_raw);
      if (vrep.is_inlined()) continue;

      uint64_t payload = vrep.payload();
      if (payload != 0 && payload < value_offsets_.size()) {
        uint64_t abs_offset = value_start_offset_ + value_offsets_[static_cast<size_t>(payload)];
        vrep_raw = ValueRep::Make(vrep.type_id(), abs_offset,
                                   vrep.is_array(), false,
                                   vrep.is_compressed()).raw();
        std::memcpy(block.data.data() + voff, &vrep_raw, 8);
      }
    }
  }

  // Relocate the embedded value ValueReps inside a Dictionary block (built by
  // EncodeDictionary) from block-index payloads to absolute file offsets. Block
  // layout: [u64 count] then per entry [u32 keyStrIdx][i64 recOffset][u64 rep]
  // (20 bytes/entry); the rep sits 12 bytes into each entry.
  void _PatchDictBlock(uint64_t data_idx) {
    if (data_idx >= value_data_.size()) return;
    DataBlock& block = value_data_[data_idx];
    if (block.data.size() < 8) return;
    uint64_t count;
    std::memcpy(&count, block.data.data(), 8);
    for (uint64_t i = 0; i < count; ++i) {
      const size_t voff = 8 + static_cast<size_t>(i) * 20 + 12;
      if (voff + 8 > block.data.size()) break;
      uint64_t vrep_raw;
      std::memcpy(&vrep_raw, block.data.data() + voff, 8);
      ValueRep vrep(vrep_raw);
      if (vrep.is_inlined()) continue;
      uint64_t payload = vrep.payload();
      if (payload != 0 && payload < value_offsets_.size()) {
        uint64_t abs_offset =
            value_start_offset_ + value_offsets_[static_cast<size_t>(payload)];
        vrep_raw = ValueRep::Make(vrep.type_id(), abs_offset, vrep.is_array(),
                                  false, vrep.is_compressed()).raw();
        std::memcpy(block.data.data() + voff, &vrep_raw, 8);
      }
    }
  }

  void _PatchEmbeddedValueReps(DataBlock* block) {
    if (!block) return;
    for (size_t offset : block->embedded_value_rep_offsets) {
      if (offset + 8 > block->data.size()) continue;
      uint64_t raw = 0;
      std::memcpy(&raw, block->data.data() + offset, 8);
      ValueRep rep(raw);
      if (rep.is_inlined()) continue;
      const uint64_t payload = rep.payload();
      if (payload == 0 || payload >= value_offsets_.size()) continue;
      const uint64_t absolute =
          value_start_offset_ + value_offsets_[static_cast<size_t>(payload)];
      raw = ValueRep::Make(rep.type_id(), absolute, rep.is_array(), false,
                           rep.is_compressed()).raw();
      std::memcpy(block->data.data() + offset, &raw, 8);
    }
  }

  // Compute the VALUE section layout (8-byte-aligned block offsets) and patch
  // every field/time-sample ValueRep with its absolute file offset. Returns the
  // total VALUE section size in bytes (== the offset just past the last block).
  // This is pure bookkeeping (no bytes emitted); the bytes are written later by
  // EmitValueBytesToBuffer() (in-memory) or StreamValueBytes() (streaming).
  uint64_t ComputeValueLayoutAndPatch() {
    // The VALUE section layout: [Block0][Block1]... each block is raw bytes.
    // Field ValueReps with is_inlined==false hold the offset from the START OF
    // VALUE SECTION to the block's data. Align each block to 8 bytes for
    // memory-mapped zero-copy reads.
    value_offsets_.resize(value_data_.size());
    uint64_t current_offset = 0;
    for (size_t i = 0; i < value_data_.size(); ++i) {
      if (current_offset % 8 != 0) {
        current_offset += 8 - (current_offset % 8);
      }
      value_offsets_[i] = current_offset;
      current_offset += value_data_[i].size();
    }

    // Patch all field ValueReps with actual file offsets (absolute).
    for (auto& field : fields_) {
      if (field.value_rep.is_inlined()) continue;

      CrateTypeId type = field.value_rep.type_id();
      uint64_t data_idx = field.value_rep.payload();

      // payload 0 (non-inlined) is the empty-array marker (block 0 is reserved
      // and never referenced) — leave it as 0 so it is not relocated.
      if (data_idx == 0) continue;

      if (data_idx < value_data_.size() && data_idx < value_offsets_.size()) {
        uint64_t offset = value_start_offset_ + value_offsets_[data_idx];
        field.value_rep = ValueRep::Make(type, offset,
                                         field.value_rep.is_array(), false,
                                         field.value_rep.is_compressed(),
                                         field.value_rep.is_array_edit());
      }
      // Patch embedded ValueReps in TimeSamples data blocks
      if (type == CrateTypeId::TimeSamples) {
        _PatchTimeSamplesBlock(data_idx);
      }
    }

    // Patch embedded ValueReps in Dictionary blocks. Nested dictionaries are
    // referenced from other dict blocks (not from fields), so iterate every
    // block rather than only field-referenced ones.
    for (size_t i = 0; i < value_data_.size(); ++i) {
      if (value_data_[i].type == TypeId::Dictionary) {
        _PatchDictBlock(static_cast<uint64_t>(i));
      }
      _PatchEmbeddedValueReps(&value_data_[i]);
    }

    return current_offset;
  }

  // Emit the VALUE section data blocks into buffer_ (in-memory path), 8-byte
  // aligned. Must produce exactly ComputeValueLayoutAndPatch()'s byte count.
  void EmitValueBytesToBuffer() {
    // Place every value block at its precomputed 8-byte-aligned offset (from
    // ComputeValueLayoutAndPatch) into a single pre-sized, zero-filled region.
    // The blocks occupy disjoint sub-ranges, so the copies parallelize trivially.
    // Byte-identical to the old align()+append loop (align() also zero-pads).
    const size_t n = value_data_.size();
    if (n == 0) return;
    const size_t base = buffer_.size();  // == value_start_offset_ (post-bootstrap)
    const size_t total = static_cast<size_t>(value_offsets_.back()) +
                         value_data_.back().size();
    buffer_.resize(base + total, 0);
    uint8_t* const out = buffer_.data() + base;
    auto copy_range = [&](size_t lo, size_t hi) {
      for (size_t i = lo; i < hi; ++i) {
        const size_t sz = value_data_[i].size();
        if (sz) std::memcpy(out + value_offsets_[i], value_data_[i].bytes(), sz);
      }
    };
#if defined(LIGHTUSD_ENABLE_THREAD)
    int P = (std::min)(ResolveThreads(), 16);
    if (P > 1 && n >= 4096 && total >= (8u << 20)) {
      // Byte-balanced index boundaries (offsets are ascending).
      std::vector<size_t> ib(P + 1);
      ib[0] = 0;
      ib[P] = n;
      for (int t = 1; t < P; ++t) {
        const uint64_t target = static_cast<uint64_t>(total) * t / P;
        ib[t] = static_cast<size_t>(
            std::lower_bound(value_offsets_.begin(), value_offsets_.end(), target) -
            value_offsets_.begin());
      }
      std::vector<std::thread> pool;
      pool.reserve(P);
      for (int t = 0; t < P; ++t)
        pool.emplace_back([&copy_range, &ib, t] { copy_range(ib[t], ib[t + 1]); });
      for (auto& th : pool) th.join();
      return;
    }
#endif
    copy_range(0, n);
  }

  // Stream the VALUE section data blocks straight to `sink`, copying each block
  // verbatim from its source (the retained crate buffer for pass-through arrays,
  // or the re-encoded block) without ever materializing the section. Emits
  // exactly ComputeValueLayoutAndPatch()'s byte count (same 8-byte alignment).
  bool StreamValueBytes(const CrateWriteSink& sink) {
    static const uint8_t kZeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t pos = value_start_offset_;  // value section begins right after bootstrap
    for (size_t i = 0; i < value_data_.size(); ++i) {
      const uint64_t pad = (8 - (pos % 8)) % 8;
      if (pad) {
        if (!sink(kZeros, static_cast<size_t>(pad))) return false;
        pos += pad;
      }
      const DataBlock& block = value_data_[i];
      const size_t sz = block.size();
      if (sz) {
        if (!sink(block.bytes(), sz)) return false;
        pos += sz;
        // A pass-through block's source bytes are written exactly once: drop
        // their resident pages from the (file-mmapped) source so a large
        // write does not fault the whole input into RSS. Only the page-aligned
        // interior is dropped (a boundary page may hold a neighbouring block),
        // and the mapping is read-only, so any later touch just re-faults the
        // same bytes from the file -- the output cannot change. No-op for
        // owned (non-mmap) sources.
        if (block.is_ref() && sz >= kDropResidentMinBytes) {
          constexpr uint64_t kPage = 4096;
          const uint64_t begin = (block.src_offset + kPage - 1) & ~(kPage - 1);
          const uint64_t end = (block.src_offset + block.src_len) & ~(kPage - 1);
          if (end > begin) block.src->DiscardRange(begin, end - begin);
        }
      }
    }
    return true;
  }
  static constexpr size_t kDropResidentMinBytes = size_t(1) << 20;

  void WriteTOC(const std::vector<CrateSection>& sections) {
    writer_.write_u64(sections.size());
    for (const auto& sec : sections) {
      writer_.write_bytes(sec.name, 16);
      writer_.write_i64(sec.start);
      writer_.write_i64(sec.size);
    }
  }
