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

  void WritePathsSection() {
    // pxr's compressed path tree requires EVERY ancestor of every path to be
    // present as a node. Connection/relationship TARGET paths can reference
    // prims that have no spec in this layer (e.g. material shader outputs in
    // a not-yet-composed referenced file), so synthesize the missing ancestor
    // chain (tree-only nodes; no spec refers to them).
    {
      // Membership set keyed on string_view into the existing (stable) paths_ and
      // into `synth` (a deque, so its strings keep stable addresses). This avoids
      // copying all paths into the set — the dominant allocation on big scenes.
      // Synthesized ancestors are appended to paths_ only after the scan.
      std::unordered_set<std::string_view> have;
      have.reserve(paths_.size() * 2);
      for (const auto& p : paths_) have.insert(std::string_view(p));
      std::deque<std::string> synth;
      const size_t orig = paths_.size();
      auto add = [&](std::string_view v) -> bool {
        if (have.find(v) != have.end()) return false;
        synth.emplace_back(v);
        have.insert(std::string_view(synth.back()));
        return true;
      };
      constexpr size_t npos = std::string_view::npos;
      // Position of the property '.' separator in a path, or npos. A '.'
      // inside a bracketed variant-selection element ("{set=sel.2}") is part
      // of the element name, NOT a property separator: when the last element
      // starts with '{', the property dot can only come after its '}'.
      auto prop_dot = [](std::string_view p) -> size_t {
        const size_t slash = p.rfind('/');
        size_t i = (slash == std::string_view::npos) ? 0 : slash + 1;
        if (i < p.size() && p[i] == '{') {
          const size_t close = p.find('}', i);
          if (close == std::string_view::npos) return std::string_view::npos;
          i = close + 1;
        }
        return p.find('.', i);
      };
      for (size_t i = 0; i < orig; i++) {
        std::string_view cur(paths_[i]);
        const size_t dot = prop_dot(cur);
        if (dot != npos) {
          cur = cur.substr(0, dot);  // property path: start from its prim part
          if (!cur.empty() && cur != "/") add(cur);
        }
        while (cur.size() > 1) {
          const size_t sl = cur.rfind('/');
          if (sl == npos || sl == 0) break;
          cur = cur.substr(0, sl);
          if (!add(cur)) break;  // rest of the chain already exists
        }
      }
      for (auto& s : synth) paths_.push_back(std::move(s));
    }

    // Sort path INDICES (not string copies): swaps move 4-byte ints instead of
    // 36-byte {std::string, uint32} entries, and no path string is duplicated.
    // `sorted[i]` is the index of the i-th path in pre-order; downstream reads
    // `paths_[sorted[i]]`.
    // The empty path (SdfPath(), interned for a relocate-to-nothing target
    // `</a>: <>`) must NOT be encoded as a PATHS tree node: pxr excludes it
    // ("we do not write out the empty path", crateFile.cpp) and a real node
    // for it makes pxr reject the whole crate ("spec has empty path"). Keep it
    // in total_paths so its index stays valid; the reader leaves an unencoded
    // index as the empty path, matching pxr.
    std::vector<uint32_t> sorted;
    sorted.reserve(paths_.size());
    for (size_t i = 0; i < paths_.size(); i++) {
      if (paths_[i].empty()) continue;
      sorted.push_back(static_cast<uint32_t>(i));
    }
    // Pre-order sort. Plain byte-lexicographic order is NOT pre-order when a
    // sibling name extends another with a character below '/' (e.g. prims
    // "SM_x" and "SM_x-y": '-' < '/' puts "/A/SM_x-y" BETWEEN "/A/SM_x" and
    // its children, splitting the subtree and corrupting the jump encoding).
    // Rank the structural separators below every name character instead:
    // '/' < '.' < everything else. A precomputed 256-entry table avoids a branch
    // per character in the inner comparison loop.
    static const uint32_t* const kCharRank = []() -> const uint32_t* {
      static uint32_t t[256];
      for (int c = 0; c < 256; ++c) t[c] = 2u + static_cast<uint32_t>(c);
      t[static_cast<unsigned char>('/')] = 0;
      t[static_cast<unsigned char>('.')] = 1;
      return t;
    }();
    auto preorder_less = [](const std::string& a, const std::string& b) {
      const size_t n = (std::min)(a.size(), b.size());
      const unsigned char* pa = reinterpret_cast<const unsigned char*>(a.data());
      const unsigned char* pb = reinterpret_cast<const unsigned char*>(b.data());
      for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) return kCharRank[pa[i]] < kCharRank[pb[i]];
      }
      return a.size() < b.size();
    };
    // Parallel sort: the path pre-order sort is the single largest serial cost on
    // big scenes (~21% of USDC write on Island). Identical result to std::sort
    // (the comparator is a strict weak order and all paths are unique).
    ParallelSortIndices(
        sorted,
        [&](uint32_t a, uint32_t b) { return preorder_less(paths_[a], paths_[b]); },
        ResolveThreads());

    size_t num_paths = sorted.size();
    uint64_t num_encoded = static_cast<uint64_t>(num_paths);
    // Write total paths count AND encoded tree nodes count
    uint64_t total_paths = static_cast<uint64_t>(paths_.size());
    writer_.write_u64(total_paths);
    writer_.write_u64(num_encoded);

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

    // Build 3 arrays: path indices, element token indices, jump indices.
    //
    // node index == sorted index. A lexicographic sort of the path strings is
    // exactly the compressed-tree pre-order: every structural separator ('/'
    // for child prims, '.' for properties) is a lower byte (0x2F / 0x2E) than
    // any path-element start character, so a node's descendants are always
    // contiguous immediately after it and before any sibling. We exploit that
    // to build the tree in O(n) instead of the previous O(n^2) recursive scan
    // (the old get_next_subtree did a linear sweep per node — quadratic on
    // high-prim-count / property-heavy scenes).
    std::vector<uint32_t> path_indices(num_paths);     // path index for each node
    std::vector<int32_t> element_tokens(num_paths);    // token index (negated for property)
    std::vector<int32_t> jump_indices(num_paths, -2);  // -2 = leaf (no child, no sibling)
    for (auto& idx : path_indices) idx = 0xFFFFFFFF;

    // A path is a property if it has a '.' separator after its last prim
    // element. Property paths are encoded as "<primpath>.<propname>" and are
    // always tree leaves. A '.' inside a bracketed variant-selection element
    // ("{set=sel.2}") belongs to the element name, so when the last element
    // starts with '{' only a dot after its '}' counts.
    auto prop_dot_pos = [](const std::string& p) -> size_t {
      const size_t slash = p.rfind('/');
      size_t i = (slash == std::string::npos) ? 0 : slash + 1;
      if (i < p.size() && p[i] == '{') {
        const size_t close = p.find('}', i);
        if (close == std::string::npos) return std::string::npos;
        i = close + 1;
      }
      return p.find('.', i);
    };
    auto is_property = [&](const std::string& p) -> bool {
      return prop_dot_pos(p) != std::string::npos;
    };
    // Last path element name ("" for root): part after '.' for a property,
    // part after the last '/' for a prim.
    auto element_name = [&](const std::string& p) -> std::string {
      if (p.empty() || p == "/") return std::string();
      const size_t dot = prop_dot_pos(p);
      if (dot != std::string::npos) return p.substr(dot + 1);
      size_t pos = p.rfind('/');
      return (pos == std::string::npos) ? p : p.substr(pos + 1);
    };
    // Is `a` a proper ancestor of `b` (prefix terminated by a separator)?
    auto is_ancestor = [](const std::string& a, const std::string& b) -> bool {
      if (a.empty()) return false;
      if (a == "/") return b.size() > 1 && b[0] == '/';
      if (b.size() <= a.size()) return false;
      if (b.compare(0, a.size(), a) != 0) return false;
      char c = b[a.size()];
      return c == '/' || c == '.';
    };

    // Direct parent of each node via a monotonic ancestor stack. Each node is
    // pushed and popped at most once => O(n) amortized. The deepest ancestor
    // remaining on the stack after popping non-ancestors is the direct parent.
    constexpr uint32_t kNoNode = 0xFFFFFFFF;
    std::vector<uint32_t> parent(num_paths, kNoNode);
    {
      std::vector<uint32_t> stack;
      stack.reserve(64);
      for (uint32_t i = 0; i < num_paths; ++i) {
        const std::string& cur = paths_[sorted[i]];
        while (!stack.empty() &&
               !is_ancestor(paths_[sorted[stack.back()]], cur)) {
          stack.pop_back();
        }
        parent[i] = stack.empty() ? kNoNode : stack.back();
        stack.push_back(i);
      }
    }

    // Next-sibling links: chain each parent's children in sorted order.
    std::vector<uint32_t> next_sibling(num_paths, kNoNode);
    {
      std::unordered_map<uint32_t, uint32_t> last_child;
      last_child.reserve(num_paths * 2);
      for (uint32_t i = 0; i < num_paths; ++i) {
        uint32_t par = parent[i];
        auto it = last_child.find(par);
        if (it != last_child.end()) next_sibling[it->second] = i;
        last_child[par] = i;
      }
    }

    // Emit each node. has_child == "the next node in pre-order is our child".
    for (uint32_t i = 0; i < num_paths; ++i) {
      path_indices[i] = sorted[i];
      const std::string& p = paths_[sorted[i]];
      const bool is_root = (p == "/");
      int32_t tok = static_cast<int32_t>(
          InternToken(is_root ? std::string() : element_name(p)));
      // Property element tokens are negated (pxr's is_property marker).
      if (!is_root && is_property(p)) tok = -tok;
      element_tokens[i] = tok;

      const bool has_child = (i + 1 < num_paths) && (parent[i + 1] == i);
      const bool has_sibling = (next_sibling[i] != kNoNode);
      if (has_sibling && has_child) {
        jump_indices[i] = static_cast<int32_t>(next_sibling[i] - i);
      } else if (has_sibling) {
        jump_indices[i] = 0;
      } else if (has_child) {
        jump_indices[i] = -1;
      } else {
        jump_indices[i] = -2;
      }
    }

    // Write 3 compressed arrays using Usd_IntegerCompression (delta + LZ4)
    auto write_comp_u32 = [&](const uint32_t* vals, size_t count) {
      std::vector<uint8_t> delta = EncodeDeltaU32(vals, count);
      CompressResult cr = CompressCrateBlob(delta.data(), delta.size());
      if (cr.success) {
        writer_.write_u64(cr.data.size());
        writer_.write_bytes(cr.data.data(), cr.data.size());
      } else {
        writer_.write_u64(delta.size());
        writer_.write_bytes(delta.data(), delta.size());
      }
    };

    auto write_comp_i32 = [&](const int32_t* vals, size_t count) {
      std::vector<uint32_t> tmp(count);
      for (size_t i = 0; i < count; i++) {
        tmp[i] = static_cast<uint32_t>(static_cast<int32_t>(vals[i]));
      }
      std::vector<uint8_t> delta = EncodeDeltaU32(tmp.data(), count);
      CompressResult cr = CompressCrateBlob(delta.data(), delta.size());
      if (cr.success) {
        writer_.write_u64(cr.data.size());
        writer_.write_bytes(cr.data.data(), cr.data.size());
      } else {
        writer_.write_u64(delta.size());
        writer_.write_bytes(delta.data(), delta.size());
      }
    };

    write_comp_u32(path_indices.data(), num_paths);
    write_comp_i32(element_tokens.data(), num_paths);
    write_comp_i32(jump_indices.data(), num_paths);
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
