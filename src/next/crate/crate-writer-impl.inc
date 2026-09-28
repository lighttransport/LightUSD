// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - private CrateWriter implementation body.
// Included by crate-writer.cc; kept separate to make the public wrapper
// translation unit navigable while the writer internals are being split.

// ============================================================
// CrateWriter Implementation
// ============================================================

class CrateWriter::Impl {
public:
  #include "crate-writer-write.inc"
  bool strict_aousd_conformance() const {
    return options_.strict_aousd_conformance;
  }

private:
  CrateWriteOptions options_;
  CrateWriteResult result_;
  std::vector<uint8_t> buffer_;
  StreamWriter writer_;

  // Table storage
  // deque: token strings keep their address, so the lookup maps can key on
  // views of them (no std::string construction per lookup).
  std::deque<std::string> tokens_;
  std::unordered_map<std::string_view, uint32_t> token_to_index_;

  // STRINGS: each entry is a token index
  std::vector<uint32_t> string_indices_;
  std::unordered_map<std::string_view, uint32_t> string_to_index_;  // views of tokens_

  // Paths. Each path is a canonical (stem, name) VIEW pair (see PathRec in
  // crate-writer-tables.inc); the views point at storage that outlives the
  // write: the input Layer's prim paths, the PropNameTable, or path_owned_.
  struct PathRec {
    std::string_view stem;  // prim part (the whole path for a prim path)
    std::string_view name;  // property name ("" for a prim path)
  };
  std::vector<PathRec> paths_;
  std::vector<uint64_t> path_hashes_;   // per record, for rehash
  std::vector<uint32_t> path_slots_;    // open-addressed index + 1 (0 = empty)
  std::deque<std::string> path_owned_;  // copies of transient path strings

  // Fields and fieldsets
  std::vector<CrateField> fields_;
  std::vector<std::vector<uint32_t>> fieldsets_;

  // Specs
  std::vector<CrateSpec> specs_;

  // VALUE data section payloads. A block is either inline (`data`) or a verbatim
  // pass-through reference into a retained source crate buffer (`src` pins the
  // buffer alive; bytes are copied to the output only in WriteValueSection, so
  // the large array payload is never staged in a second heap allocation).
  struct DataBlock {
    TypeId type;
    std::vector<uint8_t> data;
    std::shared_ptr<const LazyArraySource> src;  // null => inline `data`
    uint64_t src_offset = 0;
    uint64_t src_len = 0;
    // Byte offsets of recursively-written ValueReps embedded in this block.
    // These payloads start as value_data_ indices and are relocated after the
    // VALUE layout is known (used by spline per-knot customData maps).
    std::vector<size_t> embedded_value_rep_offsets;
    DataBlock() : type(TypeId::Invalid) {}
    DataBlock(TypeId t, std::vector<uint8_t>&& bytes)
        : type(t), data(std::move(bytes)) {}
    bool is_ref() const { return static_cast<bool>(src); }
    size_t size() const { return is_ref() ? static_cast<size_t>(src_len) : data.size(); }
    const uint8_t* bytes() const {
      return is_ref() ? src->base() + src_offset : data.data();
    }
  };
  std::vector<DataBlock> value_data_;
  uint64_t value_start_offset_ = 0; // absolute file offset of VALUE section start
  std::vector<uint64_t> value_offsets_; // offset in VALUE section for each data block

  // Append `block` to value_data_ unless an existing block has identical bytes;
  // returns the (shared or new) block index. Must NOT be used for blocks that
  // are patched in place after creation (the TimeSamples indirection header):
  // sharing one would apply the index->offset patch twice.
  uint64_t InternBlock(DataBlock&& block) {
    const size_t n = block.size();
    if (n == 0) {
      // Never share the empty block: payload 0 is the reserved empty-array
      // marker and a zero-length block at a nonzero offset has no meaning.
      uint64_t idx = value_data_.size();
      value_data_.push_back(std::move(block));
      return idx;
    }
    const uint8_t* p = block.bytes();
    const uint64_t h = HashBlockBytes(p, n);
    if (block_slots_.empty() || (block_slots_used_ + 1) * 2 > block_slots_.size()) {
      GrowBlockSlots(block_slots_.empty() ? 1024 : block_slots_.size() * 2);
    }
    // At most one indexed block can hold these exact bytes (identical blocks
    // are never indexed twice), so the first full match is THE match.
    const size_t mask = block_slots_.size() - 1;
    size_t slot = static_cast<size_t>(h) & mask;
    for (; block_slots_[slot].idx != 0; slot = (slot + 1) & mask) {
      const BlockSlot& bs = block_slots_[slot];
      if (bs.hash != h) continue;
      const DataBlock& ex = value_data_[bs.idx];
      if (ex.size() == n && std::memcmp(ex.bytes(), p, n) == 0) {
        ++blocks_deduped_;
        return bs.idx;
      }
    }
    uint64_t idx = value_data_.size();
    value_data_.push_back(std::move(block));
    block_slots_[slot] = BlockSlot{h, idx};
    ++block_slots_used_;
    return idx;
  }

  // Lazy-array write accounting.
  size_t arrays_passed_through_ = 0;
  // Highest crate minor version the authored content REQUIRES (pxr-style
  // write-version upgrade): timecode values need 0.9, relocates 0.11.
  // WriteBootstrap stamps max(options version, this).
  uint8_t required_version_minor_ = 0;
  size_t arrays_reencoded_ = 0;

  // Parallel build: when these point at the main builder's COMPLETED token/string
  // tables, InternToken/InternString do a read-only lookup (workers never mutate
  // the shared tables, so blocks/fields they emit already carry GLOBAL indices).
  // A miss means the serial pre-pass was incomplete; the worker sets `frozen_miss_`
  // and the driver discards the parallel result and writes serially (safe).
  const std::unordered_map<std::string_view, uint32_t>* frozen_tok_ = nullptr;
  const std::unordered_map<std::string_view, uint32_t>* frozen_str_ = nullptr;
  bool frozen_miss_ = false;

  // Cross-spec block dedup: open-addressed (content hash, block index) table.
  // Flattened composition duplicates referenced array payloads per instance;
  // content-addressing the VALUE blocks writes each distinct payload once.
  // Block index 0 (the reserved empty block) is never indexed, so idx 0 marks
  // an empty slot.
  struct BlockSlot {
    uint64_t hash;
    uint64_t idx;
  };
  std::vector<BlockSlot> block_slots_;
  size_t block_slots_used_ = 0;
  size_t blocks_deduped_ = 0;

  void GrowBlockSlots(size_t capacity) {
    size_t cap = 16;
    while (cap < capacity) cap *= 2;
    if (cap <= block_slots_.size()) return;
    std::vector<BlockSlot> old;
    old.swap(block_slots_);
    block_slots_.assign(cap, BlockSlot{0, 0});
    const size_t mask = cap - 1;
    for (const BlockSlot& bs : old) {
      if (bs.idx == 0) continue;
      size_t slot = static_cast<size_t>(bs.hash) & mask;
      while (block_slots_[slot].idx != 0) slot = (slot + 1) & mask;
      block_slots_[slot] = bs;
    }
  }

  static uint64_t HashBlockBytes(const uint8_t* p, size_t n) {
    // Word-at-a-time multiply/xor-shift over (length, head, middle, tail) --
    // cheap on multi-MB blocks; candidates are confirmed with a full memcmp,
    // so collisions only cost compares.
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(n) *
                                          0xC2B2AE3D27D4EB4Full);
    auto mix = [&h](const uint8_t* q, size_t m) {
      size_t i = 0;
      for (; i + 8 <= m; i += 8) {
        uint64_t w;
        std::memcpy(&w, q + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
      }
      uint64_t tail = 0;
      if (i < m) std::memcpy(&tail, q + i, m - i);
      h = (h ^ tail ^ static_cast<uint64_t>(m - i)) * 0xC4CEB9FE1A85EC53ull;
      h ^= h >> 29;
    };
    if (n <= 512) {
      mix(p, n);
    } else {
      mix(p, 256);
      mix(p + n / 2 - 128, 256);
      mix(p + n - 256, 256);
    }
    return h;
  }


  // Effective worker count: 1 = serial; <=0 = auto (hardware_concurrency capped
  // at 16); >1 = as requested. Always 1 in a non-threaded build.
  int ResolveThreads() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
    const int req = options_.num_threads;
    if (req == 1) return 1;
    if (req > 1) return req;
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw < 1) hw = 1;
    return std::min(hw, 16);
#else
    return 1;
#endif
  }

  // Sort `v` with `cmp` across `nthreads` workers: sort P contiguous chunks in
  // parallel, then pairwise-merge the sorted runs (log P rounds, parallel within
  // a round). Falls back to std::sort when serial or small. Result is identical
  // to std::sort(v, cmp) for a strict-weak-ordering `cmp` with unique elements.
  template <class Cmp>
  static void ParallelSortIndices(std::vector<uint32_t>& v, Cmp cmp,
                                  int nthreads) {
#if defined(LIGHTUSD_ENABLE_THREAD)
    const size_t N = v.size();
    if (nthreads <= 1 || N < 200000) {
      std::sort(v.begin(), v.end(), cmp);
      return;
    }
    int P = 1;
    while (P * 2 <= nthreads && P * 2 <= 16 && (N / (P * 2)) >= 50000) P *= 2;
    if (P <= 1) { std::sort(v.begin(), v.end(), cmp); return; }
    std::vector<size_t> bnd(P + 1);
    for (int i = 0; i <= P; ++i) bnd[i] = N * static_cast<size_t>(i) / P;
    {
      std::vector<std::thread> pool;
      pool.reserve(P);
      for (int t = 0; t < P; ++t) {
        pool.emplace_back([&, t] {
          std::sort(v.begin() + bnd[t], v.begin() + bnd[t + 1], cmp);
        });
      }
      for (auto& th : pool) th.join();
    }
    for (int step = 1; step < P; step *= 2) {
      std::vector<std::thread> pool;
      for (int t = 0; t + step < P; t += 2 * step) {
        const size_t lo = bnd[t];
        const size_t mid = bnd[t + step];
        const size_t hi = bnd[std::min(t + 2 * step, P)];
        pool.emplace_back([&v, &cmp, lo, mid, hi] {
          std::inplace_merge(v.begin() + lo, v.begin() + mid, v.begin() + hi, cmp);
        });
      }
      for (auto& th : pool) th.join();
    }
#else
    (void)nthreads;
    std::sort(v.begin(), v.end(), cmp);
#endif
  }

  #include "crate-writer-tables.inc"

  #include "crate-writer-passthrough.inc"

  #include "crate-writer-values.inc"

  #include "crate-writer-properties.inc"

  #include "crate-writer-fields.inc"

  #include "crate-writer-sections.inc"

};

// ============================================================
// CrateWriter public methods
// ============================================================
