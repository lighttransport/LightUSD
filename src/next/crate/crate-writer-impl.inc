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
  // BuildTables pre-registers these field names. Reuse their indices while
  // emitting millions of property specs instead of hashing the same strings
  // for every field. BuildTables refreshes them on each Write call.
  uint32_t type_name_token_ = 0;
  uint32_t specifier_token_ = 0;
  uint32_t default_token_ = 0;
  uint32_t variability_token_ = 0;
  // Inferred type tokens are interned lazily to preserve first-use ordering.
  std::vector<uint32_t> type_name_token_cache_;
  // Declared type names arrive as TypeNameTable ids on the slot; this maps an
  // id to its crate token (interned at first use, so token order is unchanged).
  std::vector<uint32_t> declared_type_token_cache_;
  // Field-name tokens the per-prim / per-property emit paths need. Each is
  // interned at its first use -- the same call that interned the literal
  // before -- so the TOKENS order is unchanged; the cache only removes the
  // literal's hash + table probe from every later prim/property.
  enum FieldTok : uint8_t {
    kTokActive, kTokHidden, kTokDocumentation, kTokApiSchemas, kTokKind,
    kTokPermission, kTokDisplayName, kTokInstanceable, kTokComment,
    kTokProperties, kTokPrimChildren, kTokCustom, kTokConnectionPaths,
    kTokTargetPaths, kTokTimeSamples, kTokVariantSelection, kTokCount
  };
  static const char* FieldTokName(FieldTok t) {
    static const char* const kNames[kTokCount] = {
        "active", "hidden", "documentation", "apiSchemas", "kind",
        "permission", "displayName", "instanceable", "comment",
        "properties", "primChildren", "custom", "connectionPaths",
        "targetPaths", "timeSamples", "variantSelection"};
    return kNames[t];
  }
  uint32_t field_tok_[kTokCount];
  void ResetFieldTokens() {
    for (uint32_t& t : field_tok_) t = UINT32_MAX;
  }
  uint32_t FieldToken(FieldTok t) {
    uint32_t& tok = field_tok_[t];
    if (tok != UINT32_MAX) return tok;
    if (WorkerCtx* w = WorkerTLS()) {  // chunk-local cache; committed later
      uint32_t& wtok = w->field_tok[t];
      if (wtok == UINT32_MAX) wtok = InternToken(FieldTokName(t));
      return wtok;
    }
    tok = InternToken(FieldTokName(t));
    return tok;
  }
  // Per-prim name token and per-property-name token, recorded while
  // BuildTables interns them, so the `primChildren` / `properties` fields are
  // assembled from indices instead of re-hashing every name.
  std::vector<uint32_t> prim_name_tokens_;
  std::vector<uint32_t> prop_name_tokens_;  // indexed by PropNameId
  // Token recorded by BuildTables, or UINT32_MAX for a name it did not see
  // (relationships, time-sample-only properties). Never interns: a missing
  // name must be interned where the `properties` field is built, as before,
  // or its first-use position in TOKENS would move.
  uint32_t PropNameToken(PropNameId id) const {
    return id.id < prop_name_tokens_.size() ? prop_name_tokens_[id.id]
                                            : UINT32_MAX;
  }
  // True when every non-root prim is linked exactly once from its path
  // parent's child_indices (see BuildFieldsAndSpecs): primChildren then comes
  // straight from those links.
  bool children_from_links_ = false;

  // STRINGS: each entry is a token index
  std::vector<uint32_t> string_indices_;
  std::unordered_map<std::string_view, uint32_t> string_to_index_;  // views of tokens_

  // Paths. Each path is a canonical (stem, name) VIEW pair (see PathRec in
  // crate-writer-tables.inc); pointers reference storage that outlives the
  // write: the input Layer's prim paths, the PropNameTable, or path_owned_.
  struct PathRec {
    const char* stem_data = nullptr;
    const char* name_data = nullptr;
    uint32_t stem_size = 0;
    uint32_t name_size = 0;

    PathRec() = default;
    PathRec(std::string_view stem, std::string_view name)
        : stem_data(stem.empty() ? "" : stem.data()),
          name_data(name.empty() ? "" : name.data()),
          stem_size(static_cast<uint32_t>(stem.size())),
          name_size(static_cast<uint32_t>(name.size())) {}
    std::string_view stem() const { return {stem_data, stem_size}; }
    std::string_view name() const { return {name_data, name_size}; }
  };
  static_assert(sizeof(PathRec) == 24, "PathRec must stay compact");
  std::vector<PathRec> paths_;
  std::vector<uint16_t> path_fingerprints_;  // low hash bits; high bits live in index slots
  // Open-addressed index, split into kPathShards independent tables by hash
  // bits 28..31 (disjoint from the slot-index bits below and the fingerprint
  // above), so the parallel commit can probe/insert shards concurrently while
  // record ids stay global (paths_ order). A slot holds record index + 1 in
  // its low 32 bits (0 = empty) and the record hash's high 32 bits, so a probe
  // chain is walked without touching paths_ (far larger than any cache) until
  // the 48-bit fingerprint matches.
  static constexpr size_t kPathShards = 16;
  struct PathShard {
    std::vector<uint64_t> slots;
    size_t count = 0;
  };
  PathShard path_shards_[kPathShards];
  std::deque<std::string> path_owned_;  // copies of transient path strings

  // Fields and fieldsets
  // The two FIELDS columns are compressed independently. Retaining them in
  // column order avoids CrateField padding and full-column extraction copies.
  struct FieldColumns {
    std::vector<uint32_t> tokens;
    std::vector<ValueRep> reps;
    size_t size() const { return reps.size(); }
    void clear() { tokens.clear(); reps.clear(); }
    void reserve(size_t count) { tokens.reserve(count); reps.reserve(count); }
    void push_back(const CrateField& field) {
      tokens.push_back(field.token_index.value);
      reps.push_back(field.value_rep);
    }
  } fields_;
  // Retain fieldsets in their final flat order, including the sentinel. Each
  // spec needs only an offset instead of an allocated vector of field indices.
  std::vector<uint32_t> fieldset_offsets_;
  std::vector<uint32_t> flat_fieldsets_;

  uint32_t BeginFieldset(size_t field_count) {
    const size_t limit = (std::numeric_limits<uint32_t>::max)();
    if (fieldset_offsets_.size() >= limit || flat_fieldsets_.size() >= limit ||
        field_count > limit - flat_fieldsets_.size() - 1) {
      result_.error = "Crate fieldset index limit exceeded";
      return 0;
    }
    const uint32_t number = static_cast<uint32_t>(fieldset_offsets_.size());
    fieldset_offsets_.push_back(static_cast<uint32_t>(flat_fieldsets_.size()));
    return number;
  }

  uint32_t AddFieldset(const std::vector<uint32_t>& indices) {
    const uint32_t number = BeginFieldset(indices.size());
    if (!result_.error.empty()) return 0;
    flat_fieldsets_.insert(flat_fieldsets_.end(), indices.begin(), indices.end());
    flat_fieldsets_.push_back(UINT32_MAX);
    return number;
  }

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

  // ============================================================
  // Parallel fields_specs workers
  // ============================================================
  // Design (doc/large-scene.md): the fields_specs lap is chunked over prims.
  // Each worker encodes its chunk into private buffers:
  //   * tokens/strings: a private table + ONE ordered intern-event stream
  //     (kind, local id, text). Workers never read the live global tables
  //     (the commit thread mutates them), so no snapshot/copy is needed and
  //     every miss logs an event. Replaying the streams in chunk order = the
  //     exact serial TOKENS/STRINGS first-use order.
  //   * paths: a private open-addressed table; misses log a path event with a
  //     precomputed hash, replayed serially (probe + insert) at commit.
  //   * blocks: a block-op log (intern/push) with digests precomputed on the
  //     worker; the commit replays exact InternBlock/StreamInternBlock
  //     semantics. Provisional (log-local) block refs in field ValueReps and
  //     inside tagged inline blobs are remapped at commit (see the blob tags
  //     in crate-writer-fields.inc).
  // Local id domains: tokens are provisional starting at the phase-start
  // token count (base), likewise strings/paths, so a column value `v < base`
  // is already global/final and `v >= base` remaps via the chunk's fin vector.
  struct InternEvent {
    std::string_view text;   // points into the worker's owned storage
    uint32_t local;          // provisional id >= base
    uint8_t kind;            // 0 = token, 1 = string
  };
  struct PathEvent {
    std::string_view stem;
    std::string_view name;
    uint64_t hash;
    uint32_t local;
    // Index of the ctx path_store copy the views point into when the caller's
    // text was transient (the commit adopts that string), or kStableText.
    uint32_t store;
  };
  static constexpr uint32_t kStableText = UINT32_MAX;
  // Block op kinds.
  enum : uint8_t { kOpIntern = 0, kOpPush = 1 };

  struct WorkerCtx {
    // token/string local domains (ids offset by the phase-start base counts)
    std::deque<std::string> tok_store;
    std::unordered_map<std::string_view, uint32_t> tok_map;
    uint32_t ntok = 0;
    std::deque<std::string> str_store;
    std::unordered_map<std::string_view, uint32_t> str_map;
    uint32_t nstr = 0;

    std::vector<InternEvent> events;

    // path local table (ids offset by the phase-start path count)
    struct WPathTable {
      std::vector<uint32_t> slots;   // idx+1, 0 = empty (open-addressed)
      std::vector<uint64_t> hashes;  // per record
      std::vector<PathRec> recs;     // views into layer / PropNameTable / owned
      void Reserve(size_t n) {
        recs.reserve(n);
        hashes.reserve(n);
        size_t cap = 16;
        while (cap < n * 2) cap *= 2;
        if (cap > slots.size()) {
          slots.assign(cap, 0);
          for (uint32_t i = 0; i < recs.size(); ++i) {
            size_t s = static_cast<size_t>(hashes[i]) & (cap - 1);
            while (slots[s] != 0) s = (s + 1) & (cap - 1);
            slots[s] = i + 1;
          }
        }
      }
      uint32_t Find(std::string_view stem, std::string_view name, uint64_t h,
                    size_t* empty_slot = nullptr) const {
        if (slots.empty()) return UINT32_MAX;
        const size_t mask = slots.size() - 1;
        for (size_t i = static_cast<size_t>(h) & mask;; i = (i + 1) & mask) {
          const uint32_t slot = slots[i];
          if (slot == 0) {
            if (empty_slot) *empty_slot = i;
            return UINT32_MAX;
          }
          const uint32_t idx = slot - 1;
          if (hashes[idx] == h && recs[idx].stem() == stem &&
              recs[idx].name() == name) {
            return idx;
          }
        }
      }
      uint32_t Add(std::string_view stem, std::string_view name, uint64_t h,
                   size_t slot) {
        if ((recs.size() + 1) * 2 > slots.size()) {
          Reserve(std::max<size_t>(64, (recs.size() + 1) * 2));
          slot = kNoPathSlot;
        }
        const uint32_t idx = static_cast<uint32_t>(recs.size());
        recs.push_back(PathRec{stem, name});
        hashes.push_back(h);
        if (slot == kNoPathSlot) {
          const size_t mask = slots.size() - 1;
          size_t s = static_cast<size_t>(h) & mask;
          while (slots[s] != 0) s = (s + 1) & mask;
          slots[s] = idx + 1;
        } else {
          slots[slot] = idx + 1;
        }
        return idx;
      }
    } ptab;
    std::deque<std::string> path_store;  // owned copies of transient paths
    uint32_t npath = 0;
    std::vector<PathEvent> pevents;
    // Streaming stage B per-op probe results (P1): pre_kind 1 = source-range
    // hit, 2 = content-digest hit (pre_off = output offset), 3 = miss, 0 = not
    // probed (raw push / block with embedded reps).
    std::vector<uint8_t> pre_kind;
    std::vector<uint64_t> pre_off;
    // Blocks to write, in op order, with the offsets stage B planned for
    // them (stage W writes them and checks the offsets).
    std::vector<uint32_t> wjob_op;
    std::vector<uint64_t> wjob_off;
    // New dedup / source-range entries of this chunk (stage B, P3 inserts).
    struct PendDedup { uint64_t h, h2, n, off; };
    struct PendRange { uint64_t id, off, len, out; };
    std::vector<PendDedup> pend_dd;
    std::vector<PendRange> pend_rg;

    // pevents indices grouped by path shard (built by the worker when the
    // chunk is done): shard s owns pe_by_shard[pe_shard_off[s] ..
    // pe_shard_off[s + 1]).
    std::vector<uint32_t> pe_by_shard;
    uint32_t pe_shard_off[kPathShards + 1] = {};

    // block op log
    struct BlockLogOp {
      DataBlock block;
      uint64_t digest = 0;   // staged: HashBlockBytes; streaming: XXH3 low64
      uint64_t digest2 = 0;  // streaming: XXH3 high64
      uint8_t kind = kOpIntern;  // kOpIntern | kOpPush
      uint8_t tag = 0;           // BlockBlobTag (inline-blob id fix-ups)
      // Streaming stage B: the id fix-up pass (P1) changed the bytes of a
      // block whose embedded reps are fixed later (P2); its digest is redone
      // after that.
      bool ids_changed = false;
    };
    std::vector<BlockLogOp> ops;

    // fields/fieldsets/specs chunk delta (rebased at commit)
    FieldColumns fields;
    std::vector<uint32_t> fs_offsets;  // per fieldset: local flat offset
    std::vector<uint32_t> flat;        // local field ids + sentinels
    std::vector<CrateSpec> specs;

    // final ids for this chunk's provisional domains, produced by commit
    // stage A (tok/str/path) and stage B (block), consumed by B and C
    std::vector<uint32_t> fin_tok, fin_str, fin_path;
    std::vector<uint64_t> fin_block;

    // counters merged at commit
    uint8_t req_minor = 0;
    size_t passed = 0, reencoded = 0;

    // chunk-local caches (the member caches are main-thread only)
    uint32_t field_tok[kTokCount];
    std::vector<uint32_t> type_cache;       // 2 * TypeId::Count lazily sized
    std::vector<uint32_t> decl_type_cache;  // by TypeNameTable id

    std::string error;
    void Reset() {
      tok_store.clear(); tok_map.clear(); ntok = 0;
      str_store.clear(); str_map.clear(); nstr = 0;
      events.clear();
      ptab.slots.clear(); ptab.hashes.clear(); ptab.recs.clear();
      path_store.clear();
      npath = 0;
      pevents.clear();
      ops.clear();
      fields.clear();
      fs_offsets.clear(); flat.clear();
      specs.clear();
      req_minor = 0; passed = 0; reencoded = 0;
      // Cached type-name tokens are chunk-local provisional ids; a reused ctx
      // must not serve them to the next chunk. Keep the capacity.
      type_cache.clear();
      decl_type_cache.clear();
      for (uint32_t& t : field_tok) t = UINT32_MAX;
      error.clear();
    }
    WorkerCtx() { for (uint32_t& t : field_tok) t = UINT32_MAX; }
  };

  // Per-thread worker context (null on the main/commit thread and on serial runs).
  static WorkerCtx*& WorkerTLS() {
    static thread_local WorkerCtx* p = nullptr;
    return p;
  }
  static WorkerCtx* CurWorker() { return WorkerTLS(); }

  // Inline-blob fix-up tags: what provisional ids a worker-logged block
  // contains. CommitChunk rewrites them into final global indices BEFORE the
  // serial dedup/probe pass runs (so dedup compares final bytes, exactly like
  // the serial writer). ValueReps recorded in a block's
  // `embedded_value_rep_offsets` are fixed by that list, independent of tag.
  enum BlockBlobTag : uint8_t {
    kBlobNone = 0,
    kBlobTokenVec,        // [u64 count][u32 token]*
    kBlobStringVec,       // [u64 count][u32 string]*
    kBlobTokenListOp,     // u8 header + runs [u64 n][u32 token]*
    kBlobStringListOp,    // u8 header + runs [u64 n][u32 string]*
    kBlobPathListOp,      // u8 header + runs [u64 n][u32 path]*
    kBlobVarSel,          // [u64 count][(u32 string, u32 string)]*
    kBlobRelocates,       // [u64 count][(u32 path, u32 path)]*
    kBlobDict,            // [u64 count] + entries [u32 string][i64][u64 rep]
    kBlobTimeSamples,     // [i64][rep][i64][u64 n][rep]*n (reps fixed via tag)
    kBlobSpline,          // [u64 sz][blob][u64 n] + per knot [f64][u64 n] +
                          //   entries [u32 string][i64][u64 rep]
    kBlobRefItems,        // u8 header + runs of 24-byte SdfReference items
                          //   (+ per-item customData mini-dict entries)
    kBlobPayloadItems,    // u8 header + runs of 24-byte SdfPayload items
    kBlobString4,         // single [u32 string] (pathExpression scalar block)
    kBlobNestedRep,       // [i64 8][u64 rep] (unregistered value wrapper)
  };

  // Provisional base counts, captured right after the serial pre-phase.
  uint32_t w_base_tok = 0, w_base_str = 0, w_base_path = 0;

  void WSetError(std::string msg) {
    if (WorkerCtx* w = WorkerTLS()) {
      if (w->error.empty()) w->error = std::move(msg);
    } else if (result_.error.empty()) {
      result_.error = std::move(msg);
    }
  }
  bool WHaveError() const {
    const WorkerCtx* w = WorkerTLS();
    return w ? !w->error.empty() : !result_.error.empty();
  }
  void WRequireMinor(uint8_t v) {
    if (WorkerCtx* w = WorkerTLS()) {
      if (w->req_minor < v) w->req_minor = v;
    } else if (required_version_minor_ < v) {
      required_version_minor_ = v;
    }
  }
  void WCountPassThrough() {
    if (WorkerCtx* w = WorkerTLS()) ++w->passed; else ++arrays_passed_through_;
  }
  void WCountReencode() {
    if (WorkerCtx* w = WorkerTLS()) ++w->reencoded; else ++arrays_reencoded_;
  }

  // Worker-aware field/fieldset/spec emitters (fields_ etc. belong to the
  // main thread; workers append to their own chunk columns).
  size_t WFieldCount() const {
    const WorkerCtx* w = WorkerTLS();
    return w ? w->fields.size() : fields_.size();
  }
  void WPushField(const CrateField& f) {
    if (WorkerCtx* w = WorkerTLS()) w->fields.push_back(f);
    else fields_.push_back(f);
  }
  uint32_t WAddFieldset(const std::vector<uint32_t>& indices) {
    if (WorkerCtx* w = WorkerTLS()) {
      const uint32_t num = static_cast<uint32_t>(w->fs_offsets.size());
      w->fs_offsets.push_back(static_cast<uint32_t>(w->flat.size()));
      w->flat.insert(w->flat.end(), indices.begin(), indices.end());
      w->flat.push_back(UINT32_MAX);
      return num;
    }
    return AddFieldset(indices);
  }
  void WBeginFieldset() {
    if (WorkerCtx* w = WorkerTLS()) {
      w->fs_offsets.push_back(static_cast<uint32_t>(w->flat.size()));
    } else {
      fieldset_offsets_.push_back(static_cast<uint32_t>(flat_fieldsets_.size()));
    }
  }
  void WFieldsetIndex(uint32_t local_field) {
    if (WorkerCtx* w = WorkerTLS()) w->flat.push_back(local_field);
    else flat_fieldsets_.push_back(local_field);
  }
  void WEndFieldset() {
    if (WorkerCtx* w = WorkerTLS()) w->flat.push_back(UINT32_MAX);
    else flat_fieldsets_.push_back(UINT32_MAX);
  }
  void WPushSpec(const CrateSpec& spec) {
    if (WorkerCtx* w = WorkerTLS()) w->specs.push_back(spec);
    else specs_.push_back(spec);
  }

  // EmitPropertySpec body shared by both modes.
  void WEmitPropertySpec(uint32_t path_index, SpecType st,
                         const std::vector<CrateField>& field_list) {
    WBeginFieldset();
    if (WHaveError()) return;
    for (const auto& f : field_list) {
      WFieldsetIndex(static_cast<uint32_t>(WFieldCount()));
      WPushField(f);
    }
    WEndFieldset();
    CrateSpec spec;
    spec.path_index.value = path_index;
    spec.fieldset_index.value = WFieldsetCount() - 1;
    spec.spec_type = st;
    WPushSpec(spec);
  }
  uint32_t WFieldsetCount() const {
    const WorkerCtx* w = WorkerTLS();
    return w ? static_cast<uint32_t>(w->fs_offsets.size())
             : static_cast<uint32_t>(fieldset_offsets_.size());
  }

  // Log a block op in worker mode: digests are precomputed here (parallel),
  // the serial table semantics replay at commit. Returns the provisional
  // block ref (1 + op index; 0 stays the reserved empty-array marker).
  uint64_t WorkerLogBlock(WorkerCtx& w, DataBlock&& block, uint8_t kind,
                          uint8_t tag = kBlobNone) {
    const size_t n = block.size();
    WorkerCtx::BlockLogOp op;
    op.kind = kind;
    op.tag = tag;
    op.block = std::move(block);
    if (n) {
      if (stream_values_) {
        const XXH128_hash_t digest = XXH3_128bits(op.block.bytes(), n);
        op.digest = digest.low64;
        op.digest2 = digest.high64;
      } else {
        op.digest = HashBlockBytes(op.block.bytes(), n);
      }
    }
    w.ops.push_back(std::move(op));
    return w.ops.size();  // log-local block ref: op index + 1
  }

  // Append `block` to value_data_ unless an existing block has identical bytes;
  // returns the (shared or new) block index. Must NOT be used for blocks that
  // are patched in place after creation (the TimeSamples indirection header):
  // sharing one would apply the index->offset patch twice.
  uint64_t InternBlock(DataBlock&& block, uint8_t tag = kBlobNone) {
    if (WorkerCtx* w = WorkerTLS())
      return WorkerLogBlock(*w, std::move(block), kOpIntern, tag);
    const size_t n = block.size();
    if (stream_values_) return StreamInternBlock(block);
    const uint8_t* p = block.bytes();
    const uint64_t h = HashBlockBytes(p, n);
    return InternBlockWithHash(std::move(block), h);
  }

  // Serial staged intern with a precomputed content hash (identical
  // semantics + counters to the old inline body; CommitChunk reuses it).
  uint64_t InternBlockWithHash(DataBlock&& block, uint64_t h) {
    const size_t n = block.size();
    if (n == 0) {
      // Never share the empty block: payload 0 is the reserved empty-array
      // marker and a zero-length block at a nonzero offset has no meaning.
      uint64_t idx = value_data_.size();
      value_data_.push_back(std::move(block));
      return idx;
    }
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
      if (ex.size() == n && std::memcmp(ex.bytes(), block.bytes(), n) == 0) {
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

  // ---- Streaming-value mode ---------------------------------------------
  //
  // With a seekable sink (file output), each VALUE block is written straight
  // to the sink as it is built -- 8-byte aligned, exactly mirroring the
  // staged layout (ComputeValueLayoutAndPatch + StreamValueBytes) -- and then
  // dropped, instead of being staged in value_data_ until the end. Every
  // block-producing call then returns the block's ABSOLUTE file offset rather
  // than a value_data_ index, so ValueReps (including the ones embedded in
  // TimeSamples / dictionary / spline / array-edit blocks, which are always
  // built after the blocks they reference) are final when written and the
  // relocation passes are skipped. The bootstrap is written as a placeholder
  // and backfilled at the end. Output bytes are identical to the staged
  // write.
  bool stream_values_ = false;
  const CrateWriteSink* value_sink_ = nullptr;
  uint64_t value_stream_pos_ = 0;  // absolute sink position of the VALUE tail
  bool value_stream_error_ = false;
  // Streaming stage B plans each block's output offset ahead of the writes
  // (stage W); this is the planned sink position (== value_stream_pos_ once
  // W has caught up).
  uint64_t stream_plan_pos_ = 0;
  bool stream_plan_mismatch_ = false;

  // Streaming dedup: no block bytes are retained to memcmp against, so blocks
  // are content-addressed by a FULL-content 128-bit digest plus size (the
  // staged path's sampled hash would falsely share two large blocks that only
  // agree on their sampled regions). Entries live in fixed-size segments:
  // indices stay stable and growth never re-copies the table (animation-heavy
  // scenes intern millions of tiny blocks).
  struct StreamDedup {
    static constexpr uint32_t kInvalid = ~0u;
    static constexpr size_t kSegmentBits = 16;
    static constexpr size_t kSegmentSize = size_t(1) << kSegmentBits;
    struct Entry {
      uint64_t hash;
      uint64_t hash2;
      uint64_t size;
      uint64_t offset;
      uint32_t next;
    };
    std::vector<uint32_t> buckets;
    std::vector<std::unique_ptr<Entry[]>> segments;
    size_t count = 0;
    Entry& at(uint32_t i) {
      return segments[i >> kSegmentBits][i & (kSegmentSize - 1)];
    }
    void clear() {
      buckets.clear();
      segments.clear();
      count = 0;
    }
    const uint64_t* find(uint64_t h, uint64_t h2, uint64_t sz) {
      if (buckets.empty()) return nullptr;
      const size_t mask = buckets.size() - 1;
      for (uint32_t e = buckets[h & mask]; e != kInvalid; e = at(e).next) {
        const Entry& en = at(e);
        if (en.hash == h && en.hash2 == h2 && en.size == sz) return &en.offset;
      }
      return nullptr;
    }
    // Two-step prefetch for a coming find(): the bucket head first, then
    // (once the head is cached) the first chain entry. Hints only.
    void prefetch_bucket(uint64_t h) const {
#if defined(__GNUC__) || defined(__clang__)
      if (!buckets.empty())
        __builtin_prefetch(&buckets[static_cast<size_t>(h) & (buckets.size() - 1)]);
#else
      (void)h;
#endif
    }
    void prefetch_entry(uint64_t h) const {
#if defined(__GNUC__) || defined(__clang__)
      if (buckets.empty()) return;
      const uint32_t e = buckets[static_cast<size_t>(h) & (buckets.size() - 1)];
      if (e != kInvalid)
        __builtin_prefetch(&segments[e >> kSegmentBits][e & (kSegmentSize - 1)]);
#else
      (void)h;
#endif
    }
    void insert(uint64_t h, uint64_t h2, uint64_t sz, uint64_t off) {
      if (buckets.empty() || (count + 1) * 2 >= buckets.size()) {
        const size_t nb = buckets.empty() ? 1024 : buckets.size() * 2;
        buckets.assign(nb, kInvalid);
        const size_t mask = nb - 1;
        for (uint32_t i = 0; i < count; ++i) {
          Entry& en = at(i);
          const size_t b = static_cast<size_t>(en.hash) & mask;
          en.next = buckets[b];
          buckets[b] = i;
        }
      }
      if ((count >> kSegmentBits) >= segments.size()) {
        segments.emplace_back(new Entry[kSegmentSize]);
      }
      const size_t mask = buckets.size() - 1;
      const size_t b = static_cast<size_t>(h) & mask;
      const uint32_t idx = static_cast<uint32_t>(count++);
      at(idx) = Entry{h, h2, sz, off, buckets[b]};
      buckets[b] = idx;
    }
  };
  // Content-dedup and source-range tables are split into kStreamShards
  // independent tables by high hash bits (the slot / bucket index uses the
  // low bits), so parallel commit stage B can insert shards concurrently.
  // Keys are unique (a key is inserted only after a miss), so lookups do not
  // depend on insertion order.
  static constexpr size_t kStreamShards = 16;
  struct ShardedStreamDedup {
    StreamDedup shards[kStreamShards];
    static size_t ShardOf(uint64_t h) { return static_cast<size_t>(h >> 60); }
    void clear() { for (StreamDedup& d : shards) d.clear(); }
    const uint64_t* find(uint64_t h, uint64_t h2, uint64_t sz) {
      return shards[ShardOf(h)].find(h, h2, sz);
    }
    void insert(uint64_t h, uint64_t h2, uint64_t sz, uint64_t off) {
      shards[ShardOf(h)].insert(h, h2, sz, off);
    }
    void prefetch_bucket(uint64_t h) const { shards[ShardOf(h)].prefetch_bucket(h); }
    void prefetch_entry(uint64_t h) const { shards[ShardOf(h)].prefetch_entry(h); }
  } stream_dedup_;

  // Lazy-array Values are copied while composing instance subtrees, so many
  // values can refer to the same immutable source byte range.  Once one such
  // range has been emitted, its output offset is known without hashing the
  // bytes again.  Different source ranges still use the content-addressed
  // table below, preserving cross-source deduplication.
  // Open-addressed (source identity, offset, length) -> output offset table.
  // Most lazy-array values reaching the writer are copies of the same source
  // ranges (instance subtrees), so this lookup runs once per array value and
  // must stay cheap: flat slots, no node allocation, probed before any
  // DataBlock is built (see TryPassThrough).
  struct StreamSourceRangeTable {
    struct Entry {
      uint64_t source_id;
      uint64_t offset;
      uint64_t length;  // 0 = empty slot (zero-length ranges are never keyed)
      uint64_t out;
    };
    std::vector<Entry> slots;
    size_t used = 0;
    static size_t Hash(uint64_t id, uint64_t off, uint64_t len) {
      uint64_t h = id * 0x9E3779B97F4A7C15ull;
      h ^= (off + 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2));
      h *= 0xFF51AFD7ED558CCDull;
      h ^= (len + (h << 6) + (h >> 2));
      h ^= h >> 31;
      return static_cast<size_t>(h);
    }
    void clear() {
      std::vector<Entry>().swap(slots);
      used = 0;
    }
    const uint64_t* find(uint64_t id, uint64_t off, uint64_t len) const {
      if (slots.empty() || len == 0) return nullptr;
      const size_t mask = slots.size() - 1;
      for (size_t i = Hash(id, off, len) & mask;; i = (i + 1) & mask) {
        const Entry& e = slots[i];
        if (e.length == 0) return nullptr;
        if (e.source_id == id && e.offset == off && e.length == len)
          return &e.out;
      }
    }
    void prefetch(uint64_t id, uint64_t off, uint64_t len) const {
#if defined(__GNUC__) || defined(__clang__)
      if (!slots.empty())
        __builtin_prefetch(&slots[Hash(id, off, len) & (slots.size() - 1)]);
#else
      (void)id; (void)off; (void)len;
#endif
    }
    // Insert if absent (an existing key keeps its first offset, like
    // unordered_map::emplace).
    void emplace(uint64_t id, uint64_t off, uint64_t len, uint64_t out) {
      if (len == 0) return;
      if (slots.empty() || (used + 1) * 2 > slots.size()) {
        std::vector<Entry> old;
        old.swap(slots);
        slots.assign(old.empty() ? 1024 : old.size() * 2, Entry{0, 0, 0, 0});
        const size_t m = slots.size() - 1;
        for (const Entry& e : old) {
          if (e.length == 0) continue;
          size_t i = Hash(e.source_id, e.offset, e.length) & m;
          while (slots[i].length != 0) i = (i + 1) & m;
          slots[i] = e;
        }
      }
      const size_t mask = slots.size() - 1;
      size_t i = Hash(id, off, len) & mask;
      for (; slots[i].length != 0; i = (i + 1) & mask) {
        const Entry& e = slots[i];
        if (e.source_id == id && e.offset == off && e.length == len) return;
      }
      slots[i] = Entry{id, off, len, out};
      ++used;
    }
  };
  struct ShardedSourceRanges {
    StreamSourceRangeTable shards[kStreamShards];
    static size_t ShardOf(uint64_t id, uint64_t off, uint64_t len) {
      return StreamSourceRangeTable::Hash(id, off, len) >> 60;
    }
    void clear() { for (StreamSourceRangeTable& t : shards) t.clear(); }
    const uint64_t* find(uint64_t id, uint64_t off, uint64_t len) const {
      return shards[ShardOf(id, off, len)].find(id, off, len);
    }
    void emplace(uint64_t id, uint64_t off, uint64_t len, uint64_t out) {
      shards[ShardOf(id, off, len)].emplace(id, off, len, out);
    }
    void prefetch(uint64_t id, uint64_t off, uint64_t len) const {
      shards[ShardOf(id, off, len)].prefetch(id, off, len);
    }
  } stream_source_ranges_;

  // Write `n` bytes at the next 8-byte-aligned sink position; returns that
  // absolute offset.
  uint64_t StreamRawBlock(const uint8_t* bytes, size_t n) {
    static const uint8_t kZeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const uint64_t pad = (8 - (value_stream_pos_ % 8)) % 8;
    if (pad) {
      if (!(*value_sink_)(kZeros, static_cast<size_t>(pad))) {
        value_stream_error_ = true;
      }
      value_stream_pos_ += pad;
    }
    const uint64_t off = value_stream_pos_;
    if (n) {
      if (!(*value_sink_)(bytes, n)) value_stream_error_ = true;
      value_stream_pos_ += n;
    }
    return off;
  }

  uint64_t StreamInternBlock(const DataBlock& block) {
    const size_t n = block.size();
    uint64_t h = 0, h2 = 0;
    if (n) {
      const XXH128_hash_t digest = XXH3_128bits(block.bytes(), n);
      h = digest.low64;
      h2 = digest.high64;
    }
    return StreamInternBlockWithDigest(block, h, h2);
  }

  // Streaming intern with precomputed digests (CommitChunk reuses this).
  uint64_t StreamInternBlockWithDigest(const DataBlock& block, uint64_t h,
                                       uint64_t h2) {
    const size_t n = block.size();
    const bool has_source_key = block.is_ref();
    const uint64_t key_id = has_source_key ? block.src->identity() : 0;
    if (has_source_key) {
      if (const uint64_t* cached = stream_source_ranges_.find(
              key_id, block.src_offset, block.src_len)) {
        ++blocks_deduped_;
        return *cached;
      }
    }
    const uint8_t* p = block.bytes();
    if (const uint64_t* off = stream_dedup_.find(h, h2, n)) {
      ++blocks_deduped_;
      if (has_source_key)
        stream_source_ranges_.emplace(key_id, block.src_offset, block.src_len, *off);
      return *off;
    }
    const uint64_t off = StreamRawBlock(p, n);
    if (block.is_ref()) DropStreamedSourceRange(block);
    // Empty blocks are never shared (a staged empty block is never deduped).
    if (n) {
      stream_dedup_.insert(h, h2, n, off);
      if (has_source_key)
        stream_source_ranges_.emplace(key_id, block.src_offset, block.src_len, off);
    }
    return off;
  }

  // Append a block WITHOUT content dedup (the TimeSamples header, dictionary,
  // spline and array-edit blocks, which carry embedded ValueReps). Returns its
  // value_data_ index (staged) or absolute file offset (streaming).
  uint64_t PushValueBlock(DataBlock&& block, uint8_t tag = kBlobNone) {
    if (WorkerCtx* w = WorkerTLS())
      return WorkerLogBlock(*w, std::move(block), kOpPush, tag);
    if (stream_values_) {
      const uint64_t off = StreamRawBlock(block.bytes(), block.size());
      if (block.is_ref()) DropStreamedSourceRange(block);
      return off;
    }
    const uint64_t idx = value_data_.size();
    value_data_.push_back(std::move(block));
    return idx;
  }

  // A pass-through block's source bytes are written exactly once: drop their
  // resident pages from the (file-mmapped) source so a large write does not
  // fault the whole input into RSS. Only the page-aligned interior is dropped
  // (a boundary page may hold a neighbouring block), and the mapping is
  // read-only, so any later touch just re-faults the same bytes from the file
  // -- the output cannot change. No-op for owned (non-mmap) sources.
  static constexpr size_t kDropResidentMinBytes = size_t(1) << 20;
  static void DropStreamedSourceRange(const DataBlock& block) {
    if (!block.is_ref() || block.src_len < kDropResidentMinBytes) return;
    constexpr uint64_t kPage = 4096;
    const uint64_t begin = (block.src_offset + kPage - 1) & ~(kPage - 1);
    const uint64_t end = (block.src_offset + block.src_len) & ~(kPage - 1);
    if (end > begin) block.src->DiscardRange(begin, end - begin);
  }

#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
  // Deferred-fill layer (see PrimFiller): prims are composed right before
  // they are encoded and released after; null when the layer is complete
  // (or was filled up front).
  PrimFiller *deferred_filler_ = nullptr;
  void FillForWrite(const Layer &layer, size_t i) {
    if (deferred_filler_)
      deferred_filler_->Fill(const_cast<Layer &>(layer), static_cast<uint32_t>(i));
  }
  void ReleaseAfterWrite(const Layer &layer, size_t i) {
    if (deferred_filler_)
      deferred_filler_->Release(const_cast<Layer &>(layer), static_cast<uint32_t>(i));
  }
#else
  void FillForWrite(const Layer &, size_t) {}
  void ReleaseAfterWrite(const Layer &, size_t) {}
#endif

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

  // Run fn(t, lo, hi) over contiguous slices of [0, n) on up to
  // ResolveThreads() threads (t = slice number); serial below `min_parallel`
  // items or in a non-threaded build. Returns the slice count used.
  template <class Fn>
  size_t ParallelRanges(size_t n, size_t min_parallel, Fn fn) const {
    size_t T = 1;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (n >= min_parallel) T = static_cast<size_t>(ResolveThreads());
    if (T > n) T = n;
    if (T > 1) {
      std::vector<std::thread> pool;
      pool.reserve(T - 1);
      for (size_t t = 1; t < T; ++t)
        pool.emplace_back([&fn, t, n, T] { fn(t, n * t / T, n * (t + 1) / T); });
      fn(size_t(0), size_t(0), n / T);
      for (std::thread& th : pool) th.join();
      return T;
    }
#else
    (void)min_parallel;
#endif
    fn(size_t(0), size_t(0), n);
    return 1;
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
