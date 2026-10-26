// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache stage materialization and instance flattening.
// Included inside Cache::Impl.

  // --- BuildStage ---------------------------------------------------------

  // `out_path` is where the composed prim is placed and where its sources are
  // looked up (the walk keys sources by composed path). `src_path` is unused
  // by the walk and left empty for children.
  struct BuildStageWork {
    Path src_path;
    Path out_path;
    uint32_t parent_idx = 0;
    bool is_root = false;
    uint32_t depth = 0;
  };

  // Temporary internal-breakdown profiling accumulators (ns), summed across all
  // root subtrees; printed by the top-level BuildStage under LIGHTUSD_NEXT_TIMING.
  mutable uint64_t prof_sources_ns_ = 0;
  mutable uint64_t prof_compose_ns_ = 0;
  mutable uint64_t prof_reg_ns_ = 0;
  // Walk-only attribution (temporary, LIGHTUSD_NEXT_TIMING): child-name
  // composition, instance registration + prototype lookup, prim publication and
  // child-work push.
  mutable uint64_t prof_childnames_ns_ = 0;
  mutable uint64_t prof_protomap_ns_ = 0;
  mutable uint64_t prof_addprim_ns_ = 0;
  mutable uint64_t prof_push_ns_ = 0;
  mutable uint64_t prof_source_hits_ = 0;
  mutable uint64_t prof_source_misses_ = 0;
  mutable uint64_t prof_structure_nodes_ = 0;

  void BuildStage(Layer *out, const BuildStageWork &root_work,
                  std::string *warn, std::string *err) ;

#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
  // Deferred (USD-style) opinion fill: the stage's layer keeps a skeleton per
  // prim and this filler composes opinions on demand (see PrimFiller).
  class DeferredFiller;
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
  // Subtree-parallel structure walk (see ParallelStructureWalk in cache.cc).
  // One composed prim, computed speculatively off the main thread. Kept
  // compact (no PrimSpec): the commit only assigns indices; the PrimSpecs are
  // built in parallel afterwards (MaterializeWalk).
  struct WalkItem {
    Path path;
    const std::vector<Src> *srcs = nullptr;
    // Instance key when registered (most items are not: a pointer, not an
    // inline string head, per item).
    std::unique_ptr<std::string> ikey;
    uint32_t parent = UINT32_MAX;         // local parent item (MAX: outer)
    uint32_t end = 0;                     // one past this item's subtree
    int32_t task = -1;                    // >= 0: placeholder for that task
    uint32_t depth = 0;
    uint8_t flags = 0;
  };
  enum : uint8_t {
    kWalkRegistered = 1,    // instanceable + active (+ detect_instances)
    kWalkSpecInstance = 2,  // walked as an instance (children not expanded)
    kWalkRoot = 4,
    kWalkFail = 8,          // depth limit: fatal if committed
    kWalkMiss = 16,         // source-cache miss: resolved at commit
    kWalkSeed = 32,         // prototype child sources unseeded: resolved at
                            // commit (seeding is a mutation)
    kWalkIsInstance = 64,   // set by the commit: a (non-prototype) instance
  };
  // Parks walk workers at their next prim so the committing thread can do a
  // serial mutation (see CommitWalk).
  // Workers count themselves in `running` while walking a task (seq_cst, no
  // lock); the requester waits until every running worker is parked. A worker
  // that starts after the request parks at its first checkpoint.
  struct WalkPause {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> req{false};
    std::atomic<int> running{0};
    std::atomic<int> parked{0};
    void Checkpoint() {
      if (!req.load(std::memory_order_seq_cst)) return;
      std::unique_lock<std::mutex> lk(m);
      parked.fetch_add(1, std::memory_order_seq_cst);
      cv.notify_all();
      cv.wait(lk, [&] { return !req.load(std::memory_order_seq_cst); });
      parked.fetch_sub(1, std::memory_order_seq_cst);
    }
    void Enter() {
      running.fetch_add(1, std::memory_order_seq_cst);
      Checkpoint();
    }
    void Leave() {
      running.fetch_sub(1, std::memory_order_seq_cst);
      if (req.load(std::memory_order_seq_cst)) {
        std::lock_guard<std::mutex> lk(m);
        cv.notify_all();
      }
    }
  };
  struct WalkCommitCtx;
  struct WalkClaims;  // first-claim registry of instance keys by DFS rank
  // Hands a not-yet-walked entry to another thread: returns its task id (the
  // caller emits a placeholder there) or -1 to walk it here. `rank` is the
  // entry's DFS rank (see SpeculativeWalk).
  using WalkDonate =
      std::function<int32_t(std::vector<BuildStageWork> &&,
                            const std::vector<uint32_t> &)>;
  using WalkCanDonate = std::function<bool(const BuildStageWork &)>;
  void SpeculativeWalk(const BuildStageWork &root,
                       const std::vector<uint32_t> &rank_prefix,
                       std::vector<WalkItem> *items,
                       const WalkCanDonate &can_donate,
                       const WalkDonate &donate, WalkClaims *claims,
                       std::unordered_set<std::string> *local_keys,
                       WalkPause *pause);
  bool CommitWalk(std::vector<WalkItem> &items, uint32_t outer_parent,
                  const std::vector<uint32_t> &rank_prefix, WalkCommitCtx &ctx);
  void MaterializeWalk(Layer *out, WalkCommitCtx &ctx, int threads);
  // `before_materialize` runs once every prim is committed, before the
  // PrimSpecs are built (not on a fallback).
  bool ParallelStructureWalk(Layer *out, const std::vector<std::string> &root_names,
                             int threads,
                             const std::function<void()> &before_materialize,
                             std::string *warn, std::string *err);
#endif
