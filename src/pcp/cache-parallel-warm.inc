// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache parallel source warming and index prewarm.
// Included inside Cache::Impl.

  bool BuildStage(Stage *stage, std::string *warn, std::string *err,
                  const Cache::PreviewCallback &preview_callback) ;

  // Convert native instancing to a self-contained flatten: each prototype group
  // keeps its prototype member as the shared content holder (made
  // non-instanceable), and every other member is emptied + internally references
  // that holder. See CompositionOptions::flatten_instances.
  void FlattenInstances(Layer *out) ;

  // usdcat-style flatten: move each prototype group's shared subtree to a root
  // `over "/Flattened_Prototype_N"` and rewrite every member (holder included)
  // to `instanceable = true` + `references = </Flattened_Prototype_N>`. Numbering
  // per `numbering` (Deterministic = sort by prototype path; UsdcatCompatible =
  // pxr's two-stage scheme). See CompositionOptions::instance_flatten_mode.
  void FlattenInstancesExtracted(Layer *out, PrototypeNumbering numbering) ;

#if defined(LIGHTUSD_ENABLE_THREAD)
  // --- background arc-layer prefetch ---------------------------------------

  // Transitive loader for the layers reachable through reference/payload/
  // sublayer arcs, run on background threads during BuildStage (see there).
  // It only warms the thread-safe, parse-once LayerRegistry; composition
  // itself never reads from it, so it cannot change any composed result.
  struct ArcLayerPrefetch {
    struct Item {
      std::string asset;   // authored asset path
      std::string anchor;  // identifier of the authoring layer
    };
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Item> queue;
    std::unordered_set<std::string> seen;  // resolved identifiers
    size_t active = 0;
    bool stop = false;
    std::vector<std::thread> threads;
    std::string warn;  // load warnings (the registry reports them only once)

    ~ArcLayerPrefetch() { Join(); }
    void Join() {
      {
        std::lock_guard<std::mutex> lk(mu);
        stop = true;
      }
      cv.notify_all();
      for (std::thread &t : threads) t.join();
      threads.clear();
    }
  };

  LayerLoadOptions ArcLayerLoadOptions() const ;

  // The external arc targets `layer` (identified by `id`) authors directly:
  // its sublayers and every prim's reference/payload assets. Expression-valued
  // asset paths and variant-scoped arcs are skipped (their targets depend on
  // composed state); the structure walk loads those on demand as before.
  void CollectArcLayerTargets(const Layer &layer, const std::string &id,
                              bool payloads,
                              std::vector<ArcLayerPrefetch::Item> *out) const ;

  void EnqueueArcLayerTargets(ArcLayerPrefetch *pf,
                              std::vector<ArcLayerPrefetch::Item> *items) ;

  void StartArcLayerPrefetch(ArcLayerPrefetch *pf, int nt) ;

  void StopArcLayerPrefetch(ArcLayerPrefetch *pf, std::string *warn) ;

  // --- parallel-build merge helpers (main thread only) --------------------

  // Adopt a worker's prebuilt LayerStack into the main table (dedup by the
  // stack's full identity key — (identifier, expression variables), see
  // InternLayerStack; the bare identifier is the fallback for stacks built
  // without a key). Returns the main-table index. The LayerStack is copied,
  // which just bumps the shared_ptr<Layer> refcounts -- the layers stay
  // shared.
  uint32_t AdoptStack(const LayerStack &ws) ;

  // Namespace pre-order position of a prim in the warm walk: the root prim's
  // index followed by the child index at each level. Lexicographic comparison
  // == namespace pre-order == the order the serial BuildStage visits prims.
  using WarmOrder = std::vector<uint32_t>;

  // One subtree root to warm. `protos` lists the claimed instance prototypes
  // (outermost first) the prim lies under: their children's sources are the
  // FILTERED pre-seed (see PreseedPrototypeChildren), and a prim below them
  // derives its sources from that seed. A worker taking a donated task
  // re-creates those seeds in its own cache before resolving the prim, so it
  // never derives a prototype-subtree prim from unfiltered sources.
  struct WarmTask {
    Path p;
    uint32_t depth = 0;
    WarmOrder order;
    std::shared_ptr<const std::vector<Path>> protos;
    // On the last child of a parent whose inert sources a worker drops once
    // its children are derived: the parent's composed child names (see
    // WarmSubtree / DropInertSources).
    std::shared_ptr<const std::vector<std::string>> parent_children;
  };

  // State shared by the parallel-warm walk: the task queue (frontier roots,
  // plus subtrees a busy worker donates while others are idle) and the
  // instance claim table.
  //
  // Instancing-aware warming: the serial BuildStage composes the subtree of
  // only the FIRST member (namespace order) of each instance group -- its
  // prototype -- and stops at every other instance. The warm walk descends an
  // instanceable prim only when no EARLIER member of its instance key has been
  // claimed, so the prototype subtree the serial pass needs is always warmed
  // and the redundant subtrees of later instances are skipped. A later member
  // claimed before the earlier one arrives only costs time: the serial pass
  // never visits an instance's subtree.
  struct WarmShared {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<WarmTask> queue;
    size_t busy = 0;
    std::atomic<int> idle{0};
    std::unordered_map<std::string, WarmOrder> first_member;  // key -> order
    size_t claims = 0;  // Claim calls: instanceable prims reached

    // True when `order` is the earliest member of `key` seen so far.
    bool Claim(std::string key, const WarmOrder &order) {
      std::lock_guard<std::mutex> lk(mu);
      ++claims;
      auto ins = first_member.emplace(std::move(key), order);
      if (ins.second) return true;
      if (!(order < ins.first->second)) return false;
      ins.first->second = order;
      return true;
    }

    // Pop a task, waiting while a busy worker may still donate one. False once
    // the queue is drained and no worker is busy.
    bool Pop(WarmTask *task) {
      std::unique_lock<std::mutex> lk(mu);
      idle.fetch_add(1, std::memory_order_relaxed);
      cv.wait(lk, [this]() { return !queue.empty() || busy == 0; });
      idle.fetch_sub(1, std::memory_order_relaxed);
      if (queue.empty()) {
        cv.notify_all();
        return false;
      }
      *task = std::move(queue.front());
      queue.pop_front();
      ++busy;
      return true;
    }
    void Done() {
      std::lock_guard<std::mutex> lk(mu);
      if (--busy == 0 && queue.empty()) cv.notify_all();
    }
    // Move the `n` front (shallowest) entries of a worker's pending stack to
    // the queue.
    void Donate(std::deque<WarmTask> *stack, size_t n) {
      {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < n; ++i) {
          queue.push_back(std::move(stack->front()));
          stack->pop_front();
        }
      }
      cv.notify_all();
    }
  };

  // Pre-seed the children of instance prototype `proto` (whose composed
  // sources are `srcs`) with exactly the filtered derivation the serial
  // BuildStage uses (its subtree_filtered path): descendants of an instance
  // compose only from the instance's key-defining arcs. Returns false when no
  // source is key-defining (the serial walk then derives the children from
  // the unfiltered sources, as for any prim).
  bool PreseedPrototypeChildren(const Path &proto, const std::vector<Src> &srcs,
                                std::vector<std::string> *children,
                                std::string *warn, std::string *err) ;

  // Resolve one prim of the warm walk and compute the children the serial
  // BuildStage would descend into, mirroring its instancing rules. Returns
  // false when the serial walk composes no subtree below `it` (inactive prim,
  // or an instance that is not the first member of its group); the prototype
  // member gets its children pre-seeded and is appended to `*child_protos`.
  bool ExpandWarmNode(const WarmTask &it, WarmShared *shared,
                      std::vector<std::string> *children,
                      std::shared_ptr<const std::vector<Path>> *child_protos,
                      std::string *warn, std::string *err) ;

  static WarmTask MakeChildWarmTask(
      const WarmTask &parent, const std::string &name, uint32_t index,
      const std::shared_ptr<const std::vector<Path>> &protos) ;

  // Worker-side: warm this (private, SEEDED) Impl's sources_cache for the whole
  // subtree rooted at `root`, following the same child namespace the serial
  // compose walks (ComposeChildNames). The Impl was seeded with a snapshot of the
  // main's layer-stack table + ancestor sources_cache, so the root->frontier
  // ancestors are already cached (no re-resolution) and any new reference resolves
  // against the SAME anchors the serial build sees -> identical layer-stack
  // identifiers. Fills a cache only; missed paths still resolve identically in the
  // serial BuildStage.
  //
  // Load balancing: subtrees are very uneven (one reference scene element holds a
  // quarter of the namespace, one tree prototype 15k prims), so while another
  // worker is idle a busy worker donates its shallowest pending subtree to the
  // shared queue. The taker re-derives the donated prim's ancestors into its
  // own cache -- first re-creating the filtered seeds of the prototypes it lies
  // under (`protos`), so it resolves exactly the sources the donor would have.
  void WarmSubtree(WarmTask root, WarmShared *shared,
                   std::unordered_set<std::string> *seeded_protos,
                   std::string *warn, std::string *err) ;

  // Parallel pre-warm of sources_cache (the LIVRPS arc resolution that dominates
  // build_stage). Serially BFS the namespace to discover a wide-enough frontier of
  // subtree roots (warming the shallow top levels into the main cache + table),
  // SNAPSHOT the main's layer-stack table + ancestor sources_cache, then have
  // worker threads (private Impls seeded from that snapshot, borrowing the
  // thread-safe shared registry) warm each frontier subtree's sources in parallel,
  // and merge the results back. The serial BuildStage then finds SourcesForPath a
  // cache hit. Byte-identical: composed values/instancing key off layer-stack
  // identifier strings, the seed gives workers the same identities the serial
  // build sees, property emission is name-ordered (parse-order-independent), and
  // any un-warmed path resolves serially.
  void ParallelWarmSources(const std::vector<std::string> &root_names, int nt,
                          std::string *warn, std::string *err) ;
#endif  // LIGHTUSD_ENABLE_THREAD

  // Batch build. With threads enabled and num_threads != 1, this first prefetches
  // first-level reference/payload layers into the (thread-safe) registry in
  // parallel, then builds the per-prim indices CONCURRENTLY: each worker runs the
  // ordinary build on its own private Impl (borrowing the shared, parse-once
  // registry), and a deterministic input-order merge folds the results into the
  // cache. Composed values are identical to a serial run. Non-threaded builds (or
  // num_threads <= 1) use the plain serial loop.
  bool PrewarmPrimIndices(const std::vector<Path> &paths, std::string *warn,
                           std::string *err) ;
