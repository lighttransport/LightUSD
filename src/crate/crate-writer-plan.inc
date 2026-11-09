// SPDX-License-Identifier: Apache-2.0
// Private, same-process encoded-operation scratch. Never a persisted format.
// Replay uses the ordinary commit stages so dedup compares FINAL raw bytes.

  struct PreparedLogSource final : LazyArraySource {
    SpillVec<uint8_t>::ReadView view;
    const uint8_t* base() const override { return view.data(); }
    size_t size() const override { return view.size(); }
    CrateVersion version() const override { return CrateVersion{0, 8, 0}; }
    bool is_mmapped() const override { return view.is_mapped(); }
    bool MaterializeArray(const LazyArrayRef&, Value*) const override { return false; }
    void DiscardRange(uint64_t offset, uint64_t count) const override {
#if defined(LIGHTUSD_PLAN_HAVE_MMAP)
      if (!view.is_mapped() || offset >= size()) return;
      const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
      if (!page || page > size()) return;
      const uint64_t end = offset + std::min<uint64_t>(count, size() - offset);
      const uint64_t lo = (offset + page - 1) / page * page;
      const uint64_t hi = end / page * page;
      if (hi > lo) (void)::madvise(const_cast<uint8_t*>(base() + lo),
                                  static_cast<size_t>(hi - lo), MADV_DONTNEED);
#else
      (void)offset; (void)count;
#endif
    }
  };
  std::unique_ptr<SpillVec<uint8_t>> prepared_log_;
  size_t prepared_batches_ = 0;
  size_t prepared_contexts_ = 0;
  bool prepared_continuation_ = false;
  bool preparing_plan_ = false;

  bool PlanPut(const void* bytes, size_t count) {
    if (!result_.error.empty()) return false;
    if (prepared_log_->size() > options_.max_prepared_bytes ||
        count > options_.max_prepared_bytes - prepared_log_->size() ||
        count > SIZE_MAX - prepared_log_->size()) {
      result_.error = "prepared writer log byte limit exceeded";
      return false;
    }
    const auto* p = static_cast<const uint8_t*>(bytes);
    while (count) {
      const size_t part = std::min(count, size_t(1) << 20);
      prepared_log_->append(p, part);
      prepared_log_->SpillSealed(UINT64_MAX, &result_.error);
      if (!result_.error.empty() || prepared_log_->io_failed()) return false;
      count -= part;
      p += part;
    }
    return true;
  }
  template<class T> bool PlanPod(const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "scratch POD required");
    return PlanPut(&value, sizeof(value));
  }
  bool PlanString(std::string_view value) {
    return PlanPod(uint64_t(value.size())) && PlanPut(value.data(), value.size());
  }
  template<class T> bool PlanVector(const std::vector<T>& values) {
    return PlanPod(uint64_t(values.size())) &&
        PlanPut(values.data(), values.size() * sizeof(T));
  }

  bool SavePreparedContext(const WorkerCtx& w) {
    PlanPod(uint8_t(prepared_continuation_));
    PlanPod(w_base_tok); PlanPod(w_base_str); PlanPod(w_base_path);
    PlanPod(w.ntok); PlanPod(w.nstr); PlanPod(w.req_minor);
    PlanPod(uint64_t(w.passed)); PlanPod(uint64_t(w.reencoded));
    PlanPod(uint64_t(w.events.size()));
    for (const InternEvent& e : w.events) {
      PlanPod(e.kind); PlanPod(e.local); PlanPod(e.field); PlanString(e.text);
    }
    PlanPod(uint64_t(w.pevents.size()));
    for (const PathEvent& e : w.pevents) {
      PlanPod(e.local); PlanPod(e.hash);
      std::string full(e.stem);
      if (!e.name.empty()) { full.push_back('.'); full.append(e.name); }
      PlanString(full);
    }
    PlanPod(uint64_t(w.ops.size()));
    for (const auto& op : w.ops) {
      PlanPod(op.kind); PlanPod(op.tag); PlanPod(op.block.type);
      PlanPod(op.digest); PlanPod(op.digest2);
      PlanVector(op.block.embedded_value_rep_offsets);
      PlanPod(uint64_t(op.block.size()));
      const uint64_t* saved = op.kind == kOpIntern && op.block.size()
          ? stream_dedup_.find(op.digest, op.digest2, op.block.size()) : nullptr;
      PlanPod(uint8_t(saved ? 0 : 1));
      if (saved) {
        PlanPod(*saved);
        continue;
      }
      const uint64_t payload_offset = prepared_log_->size();
      for (size_t at = 0; at < op.block.size() && result_.error.empty();) {
        const size_t part = std::min(size_t(1) << 20, op.block.size() - at);
        PlanPut(op.block.bytes() + at, part);
        DropStreamedSourceRange(op.block, at, part);
        at += part;
      }
      if (op.kind == kOpIntern && op.block.size() && result_.error.empty())
        stream_dedup_.insert(op.digest, op.digest2, op.block.size(), payload_offset);
    }
    PlanVector(w.fields.tokens); PlanVector(w.fields.reps);
    PlanVector(w.fs_offsets); PlanVector(w.flat); PlanVector(w.specs);
    PlanPod(uint64_t(w.pending_instance_fields.size()));
    for (const auto& field : w.pending_instance_fields) {
      PlanPod(uint64_t(field.field)); PlanPod(uint8_t(field.authored));
      PlanString(field.path);
    }
    return result_.error.empty();
  }

  bool FlushPreparedContext(WorkerCtx& w) {
    if (!w.error.empty()) { result_.error = w.error; return false; }
    if (w.fields.size() == 0 && w.ops.empty() && w.events.empty()) return true;
    if (!SavePreparedContext(w)) return false;
    ++prepared_contexts_;
    prepared_continuation_ = true;
    // Keep the token/string domains: batch-wide name arrays reference them.
    // Subsequent records contain only new events, with cumulative domain sizes.
    w.events.clear();
    w.ops.clear(); w.fields.clear(); w.fs_offsets.clear(); w.flat.clear(); w.specs.clear();
    w.pending_instance_fields.clear();
    w.ptab.slots.clear(); w.ptab.hashes.clear(); w.ptab.recs.clear();
    w.path_store.clear(); w.pevents.clear(); w.npath = 0;
    w.req_minor = 0; w.passed = 0; w.reencoded = 0;
    return true;
  }

  struct PlanReader {
    std::shared_ptr<PreparedLogSource> source;
    size_t at = 0;
    bool good = true;
    const uint8_t* span(uint64_t n) {
      if (!good || n > source->size() - at) { good = false; return nullptr; }
      const uint8_t* p = source->base() + at;
      at += static_cast<size_t>(n);
      return p;
    }
    template<class T> bool pod(T* value) {
      const uint8_t* p = span(sizeof(T));
      if (p) std::memcpy(value, p, sizeof(T));
      return good;
    }
    uint64_t count() { uint64_t n = 0; pod(&n); return n; }
    std::string string() {
      const uint64_t n = count();
      const uint8_t* p = span(n);
      return p ? std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(n))
               : std::string();
    }
    template<class T> bool vector(std::vector<T>* values) {
      const uint64_t n = count();
      if (n > (source->size() - at) / sizeof(T)) { good = false; return false; }
      const uint8_t* p = span(n * sizeof(T));
      if (!p) return false;
      values->resize(static_cast<size_t>(n));
      if (n) std::memcpy(values->data(), p, static_cast<size_t>(n) * sizeof(T));
      return good;
    }
  };

  bool LoadPreparedContext(PlanReader& r, WorkerCtx& w, bool* continuation) {
    uint8_t continues = 0; r.pod(&continues); *continuation = continues != 0;
    r.pod(&w_base_tok); r.pod(&w_base_str); r.pod(&w_base_path);
    r.pod(&w.ntok); r.pod(&w.nstr); r.pod(&w.req_minor);
    w.passed = static_cast<size_t>(r.count());
    w.reencoded = static_cast<size_t>(r.count());
    uint64_t count = r.count();
    for (uint64_t i = 0; i < count && r.good; ++i) {
      InternEvent e{};
      r.pod(&e.kind); r.pod(&e.local); r.pod(&e.field);
      auto& strings = e.kind == 0 ? w.tok_store : w.str_store;
      strings.push_back(r.string());
      e.text = strings.back(); w.events.push_back(e);
    }
    count = r.count();
    for (uint64_t i = 0; i < count && r.good; ++i) {
      PathEvent e{};
      r.pod(&e.local); r.pod(&e.hash);
      e.store = static_cast<uint32_t>(w.path_store.size());
      w.path_store.push_back(r.string());
      const PathRec path = SplitPath(w.path_store.back());
      e.stem = path.stem(); e.name = path.name(); w.pevents.push_back(e);
    }
    w.npath = static_cast<uint32_t>(w.pevents.size());
    count = r.count();
    for (uint64_t i = 0; i < count && r.good; ++i) {
      WorkerCtx::BlockLogOp op;
      r.pod(&op.kind); r.pod(&op.tag); r.pod(&op.block.type);
      r.pod(&op.digest); r.pod(&op.digest2);
      r.vector(&op.block.embedded_value_rep_offsets);
      const uint64_t size = r.count();
      uint8_t inline_payload = 0; r.pod(&inline_payload);
      size_t offset = r.at;
      const uint8_t* bytes = nullptr;
      if (inline_payload) bytes = r.span(size);
      else {
        const uint64_t prior = r.count();
        if (prior > offset || size > offset - prior) r.good = false;
        else { offset = static_cast<size_t>(prior); bytes = r.source->base() + offset; }
      }
      if (!bytes) break;
      if (op.tag || !op.block.embedded_value_rep_offsets.empty()) {
        op.block.data.assign(bytes, bytes + static_cast<size_t>(size));
      } else {
        op.block.src = r.source; op.block.src_offset = offset; op.block.src_len = size;
      }
      w.ops.push_back(std::move(op));
    }
    r.vector(&w.fields.tokens); r.vector(&w.fields.reps);
    r.vector(&w.fs_offsets); r.vector(&w.flat); r.vector(&w.specs);
    count = r.count();
    for (uint64_t i = 0; i < count && r.good; ++i) {
      WorkerCtx::PendingInstanceField field{};
      field.field = static_cast<size_t>(r.count());
      uint8_t authored = 0; r.pod(&authored); field.authored = authored != 0;
      field.path = r.string(); w.pending_instance_fields.push_back(std::move(field));
    }
    if (!r.good) result_.error = "truncated prepared writer log";
    return r.good;
  }

  void FixPreparedInstances(WorkerCtx& w,
                             const std::unordered_set<std::string>& cleared) {
    if (w.pending_instance_fields.empty() || cleared.empty()) return;
    const size_t n = w.fields.size();
    std::vector<uint32_t> remap(n, 0);
    for (const auto& f : w.pending_instance_fields) {
      if (!cleared.count(f.path)) continue;
      if (f.authored)
        w.fields.reps[f.field] = ValueRep::Make(CrateTypeId::Bool, 0, false, true);
      else remap[f.field] = UINT32_MAX;
    }
    w.events.erase(std::remove_if(w.events.begin(), w.events.end(),
        [&](const InternEvent& e) {
          return e.kind == 2 && e.field < n && remap[static_cast<size_t>(e.field)] == UINT32_MAX;
        }), w.events.end());
    size_t next = 0;
    for (size_t i = 0; i < n; ++i) {
      if (remap[i] == UINT32_MAX) continue;
      remap[i] = static_cast<uint32_t>(next);
      w.fields.tokens[next] = w.fields.tokens[i]; w.fields.reps[next] = w.fields.reps[i];
      ++next;
    }
    w.fields.tokens.resize(next); w.fields.reps.resize(next);
    size_t out = 0;
    for (uint32_t& offset : w.fs_offsets) {
      size_t in = offset;
      offset = static_cast<uint32_t>(out);
      while (w.flat[in] != UINT32_MAX) {
        const uint32_t field = remap[w.flat[in++]];
        if (field != UINT32_MAX) w.flat[out++] = field;
      }
      w.flat[out++] = UINT32_MAX;
    }
    w.flat.resize(out);
  }

public:
  bool HasPreparedBatches() const {
    return prepared_log_ && prepared_contexts_ && result_.error.empty();
  }
  void ReleasePreparedLog() {
    prepared_log_.reset(); prepared_contexts_ = 0; prepared_batches_ = 0;
  }
  CrateWriteResult PreparedFailure(const std::string& fallback) const {
    CrateWriteResult result = result_;
    result.success = false;
    if (result.error.empty()) result.error = fallback;
    return result;
  }
  bool StreamPrepareBatch(Layer& batch, const std::vector<uint8_t>& spine,
      const std::vector<uint8_t>& repeated,
      const std::unordered_map<std::string, std::vector<std::string>>& children) {
#if !defined(LIGHTUSD_PLAN_HAVE_MMAP)
    result_.error = "prepared writing requires the native mapped spill backend";
    return false;
#endif
    if (options_.spill_directory.empty()) {
      result_.error = "prepared writing requires an explicit disk spill directory";
      return false;
    }
    StreamTablesBatch(batch, repeated);
    if (!result_.error.empty()) return false;
    if (!prepared_log_) {
      prepared_log_.reset(new SpillVec<uint8_t>());
      prepared_log_->set_spill_enabled(true);
      prepared_log_->set_spill_directory(options_.spill_directory);
    }
    WorkerCtx w;
    prepared_continuation_ = false;
    w_base_tok = static_cast<uint32_t>(tokens_.size());
    w_base_str = static_cast<uint32_t>(string_indices_.size());
    w_base_path = 0;
    // No VALUE bytes enter the final sink yet; the worker log records exactly
    // those writes and their provisional references in namespace order.
    stream_values_ = true; serial_sections_ = true; preparing_plan_ = true;
    struct Scope {
      Impl& self;
      Scope(Impl& s, WorkerCtx& w) : self(s) { WorkerTLS() = &w; }
      ~Scope() { WorkerTLS() = nullptr; self.preparing_plan_ = false; }
    } scope(*this, w);
    if (!StreamWriteBatch(batch, spine, repeated, children) || !w.error.empty()) {
      if (result_.error.empty()) result_.error = w.error;
      return false;
    }
    const bool ok = FlushPreparedContext(w);
    ClearStreamPaths();
    if (ok) {
      ++prepared_batches_;
      if (options_.enable_timing)
        LIGHTUSD_LOG_I("[next_crate_prepare] batch=" + std::to_string(prepared_batches_) +
            " contexts=" + std::to_string(prepared_contexts_) +
            " scratch_bytes=" + std::to_string(prepared_log_->size()));
    }
    return ok;
  }

  bool ReplayPrepared(const std::vector<std::string>& cleared_prototypes) {
    if (!prepared_log_ || !result_.error.empty()) return false;
    auto source = std::make_shared<PreparedLogSource>();
    if (!prepared_log_->ResolveReadOnly(&source->view, &result_.error, true)) return false;
    // During preparation this table indexes identical scratch payloads. Replay
    // must rebuild it from FINAL bytes and final output offsets, in op order.
    stream_dedup_.clear();
    PlanReader reader{source};
    std::unordered_set<std::string> cleared(cleared_prototypes.begin(), cleared_prototypes.end());
    ShardTeam team;
    std::vector<uint32_t> prior_tokens, prior_strings;
    for (size_t context = 0; context < prepared_contexts_; ++context) {
      WorkerCtx w;
      bool continuation = false;
      if (!LoadPreparedContext(reader, w, &continuation)) return false;
      FixPreparedInstances(w, cleared);
      if (continuation) {
        w.fin_tok.swap(prior_tokens); w.fin_str.swap(prior_strings);
      }
      w.fin_tok.resize(w.ntok); w.fin_str.resize(w.nstr);
      for (const InternEvent& event : w.events) {
        if (event.kind == 2) {
          w.fin_tok[event.local - w_base_tok] = InternToken(event.text);
        } else if (event.kind == 3) {
          const auto known = token_to_index_.find(event.text);
          w.fin_tok[event.local - w_base_tok] = known == token_to_index_.end() ? 0 : known->second;
        } else if (event.kind == 0) {
          w.fin_tok[event.local - w_base_tok] = InternToken(event.text);
        } else w.fin_str[event.local - w_base_str] = InternString(event.text);
      }
      BucketPathEvents(w);
      CommitStageA2(w, team);
      CommitStageB(w);
      CommitStageC(w);
      w.fin_tok.swap(prior_tokens); w.fin_str.swap(prior_strings);
      MaybeSpillColumns();
      if (!result_.error.empty() || value_stream_error_) return false;
    }
    if (reader.at != source->size()) {
      result_.error = "unexpected prepared writer log tail";
      return false;
    }
    return true;
  }
private:
