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

    // Whole-column contiguous bytes. A spilled column hands back its spilled
    // prefix with one private copy; an unspilled one aliases the resident
    // storage exactly as the plain vectors did.
    std::unique_ptr<uint32_t[]> tok_copy;
    std::unique_ptr<ValueRep[]> rep_copy;
    const uint32_t* tokens = nullptr;
    const ValueRep* reps = nullptr;
    if (!fields_.tokens.Resolve(&tok_copy, &tokens, error) ||
        !fields_.reps.Resolve(&rep_copy, &reps, error)) {
      return false;
    }

    // Encode the retained token column directly (delta + LZ4).
    WriteCompressedDeltaU32(out, tokens, num_fields);

    // Write compressed value reps (TfFastCompression LZ4 format)
    size_t value_rep_bytes = 0;
    if (!safe::mul(static_cast<size_t>(num_fields), size_t(8), &value_rep_bytes)) {
      *error = "Field value-rep byte size overflow";
      return false;
    }
    static_assert(sizeof(ValueRep) == sizeof(uint64_t) &&
                      std::is_trivially_copyable<ValueRep>::value,
                  "ValueRep must retain its raw 64-bit object representation");
    WriteCompressedBytes(out,
        reinterpret_cast<const uint8_t*>(reps), value_rep_bytes);
    // The copies (if any) die here; the resident chunks die with the caller's
    // Release() calls.
    return true;
  }

  void BuildFieldsetsSectionPayload(std::vector<uint8_t>* payload) {
    StreamWriter out(*payload);
    // The construction pass already retains indices and sentinels in exactly
    // this order. Compress in place without a second full-size flat copy.
    std::unique_ptr<uint32_t[]> flat_copy;
    const uint32_t* flat = nullptr;
    {
      std::string resolve_error;
      if (!flat_fieldsets_.Resolve(&flat_copy, &flat, &resolve_error)) {
        // The writer-level spill I/O flag aborts the finish; leave the
        // payload incomplete rather than corrupt the shared result from this
        // thread.
        return;
      }
    }
    const uint64_t flat_count = flat_fieldsets_.size();
    out.write_u64(flat_count);
    // Use Usd_IntegerCompression (delta + LZ4)
    WriteCompressedDeltaU32(out, flat, static_cast<size_t>(flat_count));
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
    const size_t la = a.stem().size() + (a.name().empty() ? 0 : 1 + a.name().size());
    const size_t lb = b.stem().size() + (b.name().empty() ? 0 : 1 + b.name().size());
    auto at = [](const PathRec& r, size_t k) -> unsigned char {
      if (k < r.stem().size()) return static_cast<unsigned char>(r.stem()[k]);
      if (k == r.stem().size()) return static_cast<unsigned char>('.');
      return static_cast<unsigned char>(r.name()[k - r.stem().size() - 1]);
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

  // PATHS section payload. Reads only the finished path and token tables, so
  // it is built concurrently with the other structural payloads. Returns
  // false when a path element token is missing from TOKENS (already built).
  // `missing_parent` (optional): set when an absolute path's parent is not in
  // the table -- exactly when SynthesizeAncestorPaths would add a path (every
  // ancestor present means every record's parent lookup hits); the build then
  // stops, since its caller drops the payload. `before_tokens` (optional)
  // runs once the tree is ordered, before the first token lookup: the token
  // table may still change until then.
  bool BuildPathsSectionPayload(
      std::vector<uint8_t>* payload, bool* missing_parent = nullptr,
      const std::function<void()>& before_tokens = std::function<void()>()) {
    StreamWriter out(*payload);
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
    // Per-record state, kept to ~21 bytes per path at the peak (61.8M paths
    // on full-LOD benchmark scene, built concurrently with the other sections): the
    // element is an offset into the record's stem (a property's element is
    // its name), flags share one byte, and every array is released as soon
    // as the next step no longer reads it.
    enum : uint8_t { kEncoded = 1, kProp = 2, kRoot = 4, kSibling = 8 };
    std::vector<uint32_t> parent(N, kNoNode);
    std::vector<uint32_t> elem_off(N, 0);
    std::vector<uint8_t> flags(N, uint8_t{0});
    auto elem_of = [&](size_t i) -> std::string_view {
      const PathRec& r = paths_[i];
      if (flags[i] & kProp) return r.name();
      return std::string_view(r.stem()).substr(elem_off[i]);
    };
    // Parent/element per record: FindPath is a read-only probe, so slices run
    // concurrently (each with its own last-stem cache; the cache only skips a
    // repeated lookup of the same stem, so the results are identical).
    std::vector<size_t> slice_paths(static_cast<size_t>(ResolveThreads()) + 1, 0);
    std::atomic<bool> parent_miss{false};
    ParallelRanges(N, 100000, [&](size_t t, size_t lo, size_t hi) {
      size_t count = 0;
      std::string_view cached_prop_stem;
      uint32_t cached_prop_parent = kNoNode;
      for (size_t i = lo; i < hi; ++i) {
        const PathRec& r = paths_[i];
        if (r.stem().empty() && r.name().empty()) continue;
        flags[i] = kEncoded;
        ++count;
        if (!r.name().empty()) {
          flags[i] |= kProp;
          if (!r.stem().empty()) {
            if (r.stem() != cached_prop_stem || cached_prop_parent == kNoNode) {
              cached_prop_parent = FindPath(r.stem());
              cached_prop_stem = r.stem();
            }
            parent[i] = cached_prop_parent;
            if (cached_prop_parent == kNoNode && r.stem() != "/")
              parent_miss.store(true, std::memory_order_relaxed);
          }
          continue;
        }
        if (r.stem() == "/") {
          flags[i] |= kRoot;
          continue;
        }
        const size_t sl = r.stem().rfind('/');
        if (sl == std::string_view::npos) continue;  // relative: parentless
        elem_off[i] = static_cast<uint32_t>(sl + 1);
        parent[i] =
            FindPath(sl == 0 ? std::string_view("/") : r.stem().substr(0, sl));
        if (parent[i] == kNoNode && sl != 0)
          parent_miss.store(true, std::memory_order_relaxed);
      }
      slice_paths[t] = count;
    });
    if (missing_parent) {
      *missing_parent = parent_miss.load();
      if (*missing_parent) return true;
    }
    size_t num_paths = 0;
    for (size_t c : slice_paths) num_paths += c;

    // Write total paths count AND encoded tree nodes count
    uint64_t total_paths = static_cast<uint64_t>(N);
    out.write_u64(total_paths);
    out.write_u64(static_cast<uint64_t>(num_paths));

    if (num_paths == 0) {
      // No paths other than root, write empty arrays
      std::vector<uint8_t> empty_delta = EncodeDeltaU32(nullptr, 0);
      CompressResult empty_cr = CompressCrateBlob(empty_delta.data(), empty_delta.size());
      out.write_u64(empty_cr.data.size());
      out.write_bytes(empty_cr.data.data(), empty_cr.data.size());
      out.write_u64(empty_cr.data.size());
      out.write_bytes(empty_cr.data.data(), empty_cr.data.size());
      out.write_u64(empty_cr.data.size());
      out.write_bytes(empty_cr.data.data(), empty_cr.data.size());
      return true;
    }

    // Depth-first pre-order of the encoded nodes, plus subtree sizes; the
    // children lists (CSR, slot 0 = parentless nodes, in table order) and the
    // parent links die with this block.
    std::vector<uint32_t> order;
    std::vector<uint32_t> subtree(N, 1);
    {
      // Children lists (CSR). Counting and scattering run in parallel; the
      // order inside a list is irrelevant because every list is sorted below
      // by a total order (siblings have distinct elements).
      std::vector<uint32_t> child_off(N + 2, 0);
      ParallelRanges(N, 1000000, [&](size_t, size_t lo, size_t hi) {
        for (size_t i = lo; i < hi; ++i) {
          if (!(flags[i] & kEncoded)) continue;
          __atomic_fetch_add(
              &child_off[(parent[i] == kNoNode ? 0 : size_t(parent[i]) + 1) + 1],
              1u, __ATOMIC_RELAXED);
        }
      });
      for (size_t k = 0; k + 1 < child_off.size(); ++k) {
        child_off[k + 1] += child_off[k];
      }
      std::vector<uint32_t> children(num_paths);
      {
        std::vector<uint32_t> cursor(child_off.begin(), child_off.end() - 1);
        ParallelRanges(N, 1000000, [&](size_t, size_t lo, size_t hi) {
          for (size_t i = lo; i < hi; ++i) {
            if (!(flags[i] & kEncoded)) continue;
            const size_t list = parent[i] == kNoNode ? 0 : size_t(parent[i]) + 1;
            children[__atomic_fetch_add(&cursor[list], 1u, __ATOMIC_RELAXED)] =
                static_cast<uint32_t>(i);
          }
        });
      }
      auto sort_list = [&](size_t list) {
        auto first = children.begin() + child_off[list];
        auto last = children.begin() + child_off[list + 1];
        if (last - first < 2) return;
        const bool full_text = (list == 0) || (flags[list - 1] & kRoot);
        if (full_text) {
          std::sort(first, last, [&](uint32_t a, uint32_t b) {
            return PathRecRankLess(a, b);
          });
        } else {
          std::sort(first, last, [&](uint32_t a, uint32_t b) {
            const bool pa = (flags[a] & kProp) != 0, pb = (flags[b] & kProp) != 0;
            if (pa != pb) return pa < pb;
            return RankLess(elem_of(a), elem_of(b));
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

      // (list, next pos, pre-order position of the list's owner): a node's
      // subtree size is known when its list is exhausted.
      struct Frame {
        uint32_t list, next, pos;
      };
      bool walked = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
      if (nthreads > 1 && num_paths >= (size_t(1) << 20)) {
        // Large trees: split the walk at a frontier of subtree roots (whole
        // levels expanded while fewer than 65536 have children). Their sizes are
        // counted in parallel, a serial walk of the levels above places each
        // frontier subtree in the pre-order, and the subtrees are then walked
        // in parallel into their slots. Same order, sizes and sibling flags.
        constexpr uint8_t kTask = 16;
        // Only nodes with children count (property leaves would fill a level
        // long before a deep subtree is split); leaves above the frontier are
        // emitted by the top walk.
        std::vector<uint32_t> tasks;
        auto add_inner = [&](uint32_t list, std::vector<uint32_t>* to) {
          for (uint32_t k = child_off[list]; k < child_off[list + 1]; ++k) {
            const uint32_t node = children[k];
            if (child_off[node + 1] < child_off[node + 2]) to->push_back(node);
          }
        };
        add_inner(0, &tasks);
        for (int depth = 0; depth < 64 && tasks.size() < 65536; ++depth) {
          std::vector<uint32_t> next;
          for (uint32_t node : tasks) add_inner(node + 1, &next);
          if (next.empty()) break;
          tasks.swap(next);
        }
        for (size_t k = 0; k < tasks.size(); ++k) {
          flags[tasks[k]] |= kTask;
          subtree[tasks[k]] = static_cast<uint32_t>(k);  // task index until placed
        }
        std::vector<uint32_t> task_size(tasks.size(), 0), task_pos(tasks.size(), 0);
        auto run_tasks = [&](auto&& fn) {
          std::atomic<size_t> next_task{0};
          std::vector<std::thread> pool;
          pool.reserve(static_cast<size_t>(nthreads));
          for (int t = 0; t < nthreads; ++t)
            pool.emplace_back([&] {
              for (size_t k; (k = next_task.fetch_add(1, std::memory_order_relaxed)) <
                             tasks.size();)
                fn(k);
            });
          for (auto& th : pool) th.join();
        };
        run_tasks([&](size_t k) {
          std::vector<uint32_t> st{tasks[k]};
          uint32_t count = 0;
          while (!st.empty()) {
            const uint32_t node = st.back();
            st.pop_back();
            ++count;
            st.insert(st.end(), children.begin() + child_off[node + 1],
                      children.begin() + child_off[node + 2]);
          }
          task_size[k] = count;
        });
        order.resize(num_paths);
        uint32_t cursor = 0;
        std::vector<Frame> stack;
        stack.push_back({0u, child_off[0], 0u});
        while (!stack.empty()) {
          Frame& top = stack.back();
          if (top.next >= child_off[top.list + 1]) {
            if (top.list != 0) subtree[top.list - 1] = cursor - top.pos;
            stack.pop_back();
            continue;
          }
          const uint32_t node = children[top.next++];
          if (top.next < child_off[top.list + 1]) flags[node] |= kSibling;
          const uint32_t pos = cursor;
          if (flags[node] & kTask) {
            const uint32_t k = subtree[node];
            task_pos[k] = pos;
            subtree[node] = task_size[k];
            cursor += task_size[k];
            continue;
          }
          order[cursor++] = node;
          const uint32_t list = node + 1;
          if (child_off[list] < child_off[list + 1]) {
            stack.push_back({list, child_off[list], pos});
          }
        }
        run_tasks([&](size_t k) {
          const uint32_t root = tasks[k];
          uint32_t c = task_pos[k];
          order[c++] = root;
          std::vector<Frame> st;
          if (child_off[root + 1] < child_off[root + 2])
            st.push_back({root + 1, child_off[root + 1], task_pos[k]});
          while (!st.empty()) {
            Frame& top = st.back();
            if (top.next >= child_off[top.list + 1]) {
              subtree[top.list - 1] = c - top.pos;
              st.pop_back();
              continue;
            }
            const uint32_t node = children[top.next++];
            if (top.next < child_off[top.list + 1]) flags[node] |= kSibling;
            const uint32_t pos = c;
            order[c++] = node;
            const uint32_t list = node + 1;
            if (child_off[list] < child_off[list + 1]) {
              st.push_back({list, child_off[list], pos});
            }
          }
        });
        order.resize(cursor);
        walked = true;
      }
#endif
      if (!walked) {
      order.reserve(num_paths);
      std::vector<Frame> stack;
      stack.push_back({0u, child_off[0], 0u});
      while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next >= child_off[top.list + 1]) {
          if (top.list != 0)
            subtree[top.list - 1] =
                static_cast<uint32_t>(order.size() - top.pos);
          stack.pop_back();
          continue;
        }
        const uint32_t node = children[top.next++];
        if (top.next < child_off[top.list + 1]) flags[node] |= kSibling;
        const uint32_t pos = static_cast<uint32_t>(order.size());
        order.push_back(node);
        const uint32_t list = node + 1;
        if (child_off[list] < child_off[list + 1]) {
          stack.push_back({list, child_off[list], pos});
        }
      }
      }
    }
    std::vector<uint32_t>().swap(parent);

    // Build 3 arrays: path indices (the pre-order itself), element token
    // indices (negated for a property), jump indices:
    //   jump  > 0 : child (next node) AND sibling (at +jump = subtree size)
    //   jump == 0 : sibling only;  -1: child only;  -2: leaf
    std::vector<uint32_t> element_tokens(order.size());
    std::vector<uint32_t> jump_indices(order.size());
    // Every element token was registered by RegisterPathElementTokens before
    // TOKENS was built, so the slices only look tokens up (the table is read
    // concurrently by the TOKENS payload build). A miss cannot be encoded.
    std::atomic<bool> token_miss{false};
    if (before_tokens) before_tokens();
    auto build_nodes = [&](size_t lo, size_t hi) {
      for (size_t k = lo; k < hi; ++k) {
        const uint32_t node = order[k];
        const bool root = (flags[node] & kRoot) != 0;
        const std::string_view tok_text = root ? std::string_view() : elem_of(node);
        auto it = token_to_index_.find(tok_text);
        if (it == token_to_index_.end()) {
          token_miss.store(true, std::memory_order_relaxed);
          return;
        }
        const uint32_t tok_id = it->second;
        int32_t tok = static_cast<int32_t>(tok_id);
        // Property element tokens are negated (pxr's is_property marker).
        if (!root && (flags[node] & kProp)) tok = -tok;
        element_tokens[k] = static_cast<uint32_t>(tok);
        const bool has_child = subtree[node] > 1;
        const bool has_sibling = (flags[node] & kSibling) != 0;
        int32_t jump;
        if (has_sibling && has_child) {
          jump = static_cast<int32_t>(subtree[node]);
        } else if (has_sibling) {
          jump = 0;
        } else if (has_child) {
          jump = -1;
        } else {
          jump = -2;
        }
        jump_indices[k] = static_cast<uint32_t>(jump);
      }
    };
    ParallelRanges(order.size(), 100000, [&](size_t, size_t lo, size_t hi) {
      build_nodes(lo, hi);
    });
    if (token_miss.load()) return false;
    std::vector<uint32_t>().swap(subtree);
    std::vector<uint32_t>().swap(elem_off);
    std::vector<uint8_t>().swap(flags);

    // Write 3 compressed arrays using Usd_IntegerCompression (delta + LZ4),
    // each released once written.
    const size_t num_nodes = order.size();
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (num_nodes >= (size_t(1) << 20) && ResolveThreads() > 1) {
      // The three columns compress independently (LZ4 is single-threaded per
      // blob); append them in order.
      std::vector<uint8_t> col[3];
      const uint32_t* data[3] = {order.data(), element_tokens.data(),
                                 jump_indices.data()};
      auto compress = [&](int c) {
        StreamWriter w(col[c]);
        WriteCompressedDeltaU32(w, data[c], num_nodes);
      };
      std::thread t1(compress, 1), t2(compress, 2);
      compress(0);
      t1.join();
      t2.join();
      for (const auto& bytes : col) out.write_bytes(bytes.data(), bytes.size());
      return true;
    }
#endif
    WriteCompressedDeltaU32(out, order.data(), num_nodes);
    std::vector<uint32_t>().swap(order);
    WriteCompressedDeltaU32(out, element_tokens.data(), num_nodes);
    std::vector<uint32_t>().swap(element_tokens);
    WriteCompressedDeltaU32(out, jump_indices.data(), num_nodes);
    return true;
  }

  void BuildSpecsSectionPayload(std::vector<uint8_t>* payload) {
    StreamWriter out(*payload);
    uint64_t num_specs = specs_.size();
    out.write_u64(num_specs);

    // Rebuild the (possibly spilled) columns contiguously. A spilled spec /
    // fieldset-offset column returns through one private pread copy; the
    // unspilled shapes alias as before.
    std::unique_ptr<CrateSpec[]> spec_copy;
    const CrateSpec* spec_arr = nullptr;
    std::unique_ptr<uint32_t[]> fs_off_copy;
    const uint32_t* fs_off = nullptr;
    {
      std::string resolve_error;
      if (!specs_.Resolve(&spec_copy, &spec_arr, &resolve_error) ||
          !fieldset_offsets_.Resolve(&fs_off_copy, &fs_off, &resolve_error)) {
        return;  // io flag aborts below
      }
    }

    // Compress each column before reusing its storage for the next. Keeping
    // all three columns live together costs eight extra bytes per spec and
    // provides no benefit to the independent, ordered codec calls below.
    std::vector<uint32_t> indices(num_specs);
    for (size_t i = 0; i < num_specs; i++) {
      indices[i] = spec_arr[i].path_index.value;
    }
    WriteCompressedDeltaU32(out, indices.data(), num_specs);

    // Offsets were recorded when each fieldset was appended.
    for (size_t i = 0; i < num_specs; i++) {
      // Convert fieldset number → flat array offset
      uint32_t fs_num = spec_arr[i].fieldset_index.value;
      indices[i] = (fs_num < fieldset_offsets_.size())
                       ? fs_off[fs_num]
                       : 0;
    }
    WriteCompressedDeltaU32(out, indices.data(), num_specs);

    for (size_t i = 0; i < num_specs; i++) {
      indices[i] = static_cast<uint32_t>(spec_arr[i].spec_type);
    }
    WriteCompressedDeltaU32(out, indices.data(), num_specs);
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

    // Patch all field ValueReps with actual file offsets (absolute). The
    // columns are chunked containers; spill is inactive on this (non-stream)
    // path, but iterate through chunks regardless of layout.
    fields_.reps.ForEachMut([&](ValueRep& field) {
      if (field.is_inlined()) return;

      CrateTypeId type = field.type_id();
      uint64_t data_idx = field.payload();

      // payload 0 (non-inlined) is the empty-array marker (block 0 is reserved
      // and never referenced) — leave it as 0 so it is not relocated.
      if (data_idx == 0) return;

      if (data_idx < value_data_.size() && data_idx < value_offsets_.size()) {
        uint64_t offset = value_start_offset_ + value_offsets_[data_idx];
        field = ValueRep::Make(type, offset,
                               field.is_array(), false,
                               field.is_compressed(),
                               field.is_array_edit());
      }
      // Patch embedded ValueReps in TimeSamples data blocks
      if (type == CrateTypeId::TimeSamples) {
        _PatchTimeSamplesBlock(data_idx);
      }
    });

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
        DropStreamedSourceRange(block);
      }
    }
    return true;
  }

  void WriteTOC(const std::vector<CrateSection>& sections) {
    writer_.write_u64(sections.size());
    for (const auto& sec : sections) {
      writer_.write_bytes(sec.name, 16);
      writer_.write_i64(sec.start);
      writer_.write_i64(sec.size);
    }
  }
