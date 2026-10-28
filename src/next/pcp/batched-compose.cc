// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - batched composition (see batched-compose.hh).

#include "batched-compose.hh"

#include "cache-internal.hh"
#include "../composition/composition.hh"
#include "../stage/stage.hh"

#include <algorithm>
#include <chrono>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace lightusd {
namespace next {
namespace pcp {

// Cache internals the batch driver needs (Cache is otherwise opaque).
struct BatchedComposeAccess {
  static Cache::Impl *impl(Cache &cache) { return cache.impl_.get(); }

  // Cache::Open over a caller-owned registry shared by every batch, so each
  // layer is parsed once for the whole build.
  static expected<Cache, std::string> Open(AssetResolver &resolver,
                                           std::shared_ptr<Layer> root_layer,
                                           const std::string &root_identifier,
                                           const CompositionOptions &options,
                                           LayerRegistry *registry) {
    Cache cache;
    Cache::Impl *im = cache.impl_.get();
    im->reg_ = registry;
    im->resolver = &resolver;
    im->options = options;
    if (im->options.strict_aousd_conformance)
      im->options.usda_parse_options.strict_aousd_conformance = true;
    if (im->options.flatten_instances &&
        im->options.instance_flatten_mode == InstanceFlattenMode::Native)
      im->options.instance_flatten_mode = InstanceFlattenMode::Holder;
    im->root_layer = root_layer;
    im->root_identifier = root_identifier;
    std::string warn, err;
    if (im->InternLayerStack(root_layer, root_identifier, &warn, &err) ==
        Cache::Impl::kInvalidStack) {
      return make_unexpected(err.empty()
                                 ? std::string("batched compose: failed to "
                                               "build root layer stack")
                                 : err);
    }
    return cache;
  }
};

namespace {

#if defined(LIGHTUSD_ENABLE_THREAD)
// Runs a pass's per-batch callback on its own thread, one batch behind the
// composition: batch b + 1 composes while batch b is consumed. A batch is
// handed over only once the previous one was consumed and freed, so at most
// two batches are alive.
class BatchConsumer {
 public:
  explicit BatchConsumer(const std::function<bool(ComposedBatch &)> &fn)
      : fn_(fn), thread_([this] { Loop(); }) {}
  ~BatchConsumer() { Finish(); }

  // False once a callback has failed (nothing more is consumed).
  bool Push(ComposedBatch &&batch) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return !has_ || failed_; });
    if (failed_) return false;
    pending_ = std::move(batch);
    has_ = true;
    cv_.notify_all();
    return true;
  }
  // Waits for the last batch; true when every callback succeeded.
  bool Finish() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      done_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    return !failed_;
  }
  double busy_ms() const { return busy_ms_; }

 private:
  void Loop() {
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return has_ || done_; });
        if (!has_) return;
      }
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = fn_(pending_);
      pending_ = ComposedBatch();
      busy_ms_ += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
      {
        std::lock_guard<std::mutex> lk(mu_);
        has_ = false;
        if (!ok) failed_ = true;
      }
      cv_.notify_all();
    }
  }

  std::function<bool(ComposedBatch &)> fn_;
  std::mutex mu_;
  std::condition_variable cv_;
  ComposedBatch pending_;
  bool has_ = false;
  bool done_ = false;
  bool failed_ = false;
  double busy_ms_ = 0;
  std::thread thread_;
};
#endif

std::string ParentPath(const std::string &path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return path.substr(0, slash);
}

}  // namespace

struct BatchedComposer::Impl {
  AssetResolver *resolver = nullptr;
  CompositionOptions options;
  BatchedComposeOptions batch_options;
  std::string root_identifier;
  std::shared_ptr<Layer> root;
  LayerRegistry registry;
  bool whole = false;  // no batching: the stage composes in one piece

  // Frontier roots in namespace pre-order, and every expanded prim above them.
  std::vector<std::string> frontier;
  std::unordered_set<std::string> spine;
  // Batch b covers frontier[bounds[b], bounds[b + 1]); pass 1 extends it as
  // it goes. Composed prims under each frontier root (pass 1).
  std::vector<size_t> bounds;
  std::vector<size_t> root_prims;
  // Pass 1 records, for each root over the batch target, its composed
  // children and their subtree sizes (recursively for heavy expandable
  // children); pass 2 splits those roots into their children.
  struct RefineChild {
    std::string name;
    size_t prims = 0;
  };
  std::unordered_map<std::string, std::vector<RefineChild>> refine;
  void RefineFrontier();
  // Pass 2 root -> its pass-1 frontier index (heavy roots split in pass 2).
  std::vector<size_t> root_origin;
  // Layer (registry id) -> the last pass-1 frontier root whose batch used it.
  // Pass 2 drops a layer from the registry once the batches are past it (a
  // later use reloads it).
  std::unordered_map<std::string, size_t> layer_last_root;
  size_t layers_released = 0;
  // Per-pass time split (enable_timing): cache open, BuildStage, batch
  // bookkeeping, cache teardown, the caller's callback.
  double t_open = 0, t_build = 0, t_post = 0, t_drop = 0, t_callback = 0;

  // Cross-batch instancing state.
  bool planned = false;
  std::unordered_set<std::string> holders_with_peers;

  // Every batch re-composes its ancestors, and both passes compose every
  // prim: report each composition diagnostic line once.
  std::unordered_set<std::string> reported;
  void Report(const std::string &lines, std::string *out);

  bool Discover(std::string *warn, std::string *err);
  bool ComposeBatch(size_t b, std::vector<std::pair<std::string, std::string>> *protos,
                    std::unordered_set<std::string> *proto_keys,
                    std::unordered_set<std::string> *peers, ComposedBatch *out,
                    std::unordered_set<std::string> *emitted_spine,
                    std::string *warn, std::string *err);
  bool Pass(bool plan, const std::function<bool(ComposedBatch &)> &on_batch,
            std::string *warn, std::string *err);
};

bool BatchedComposer::Impl::Discover(std::string *warn, std::string *err) {
  auto opened = BatchedComposeAccess::Open(*resolver, root, root_identifier,
                                           options, &registry);
  if (!opened) {
    if (err) *err += opened.error() + "\n";
    return false;
  }
  Cache cache = std::move(*opened);
  Cache::Impl *im = BatchedComposeAccess::impl(cache);
  // Expand whole levels while the next one fits max_frontier: a leaf stays
  // a root, so the deep branches (where the prims are) keep splitting. Only an
  // active, non-instanceable prim is expanded (an instance's subtree depends on
  // which member claims the prototype, which is the walk's decision).
  std::vector<std::string> level;
  for (const std::string &name : im->ComposeRootNames())
    level.push_back("/" + name);
  spine.insert("/");
  std::vector<uint8_t> done(level.size(), 0);  // leaf / not expandable
  for (uint32_t d = 1; d < options.max_namespace_depth; ++d) {
    std::vector<std::string> next;
    std::vector<uint8_t> next_done;
    std::vector<std::string> expanded;
    for (size_t i = 0; i < level.size(); ++i) {
      std::vector<std::string> children;
      if (!done[i]) {
        const std::vector<Src> &srcs =
            im->SourcesForPath(Path(level[i]), warn, err);
        if (im->ComposedActive(srcs) && !im->IsInstanceableSources(srcs))
          children = im->ComposeChildNames(srcs);
      }
      if (children.empty()) {
        next.push_back(level[i]);
        next_done.push_back(1);
        continue;
      }
      expanded.push_back(level[i]);
      for (const std::string &cn : children) {
        next.push_back(level[i] + "/" + cn);
        next_done.push_back(0);
      }
    }
    if (expanded.empty() || next.size() > batch_options.max_frontier) break;
    for (std::string &p : expanded) spine.insert(std::move(p));
    level = std::move(next);
    done = std::move(next_done);
  }
  frontier = std::move(level);
  return true;
}

bool BatchedComposer::Impl::ComposeBatch(
    size_t b, std::vector<std::pair<std::string, std::string>> *protos,
    std::unordered_set<std::string> *proto_keys,
    std::unordered_set<std::string> *peers, ComposedBatch *out,
    std::unordered_set<std::string> *emitted_spine, std::string *warn,
    std::string *err) {
  using Clock = std::chrono::steady_clock;
  auto since = [](Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
  };
  auto t0 = Clock::now();
  CompositionOptions build_options = options;
  // The batch's Cache dies right after its BuildStage.
  build_options.release_build_sources = true;
  auto opened = BatchedComposeAccess::Open(*resolver, root, root_identifier,
                                           build_options, &registry);
  if (!opened) {
    if (err) *err += opened.error() + "\n";
    return false;
  }
  std::unique_ptr<Cache> cache(new Cache(std::move(*opened)));
  Cache::Impl *im = BatchedComposeAccess::impl(*cache);

  BatchScope scope;
  for (size_t f = bounds[b]; f < bounds[b + 1]; ++f) {
    scope.roots.insert(frontier[f]);
    for (std::string p = ParentPath(frontier[f]);; p = ParentPath(p)) {
      if (!scope.spine.insert(p).second || p == "/") break;
    }
  }
  scope.seeded_protos = *protos;
  for (const auto &kp : *protos) {
    scope.seeded_keys.insert(kp.first);
    scope.seeded_proto_paths.insert(kp.second);
  }
  if (planned) scope.holders_with_peers = &holders_with_peers;
  im->batch_scope_ = &scope;

  Stage stage;
  std::string batch_warn, batch_err;
  t_open += since(t0);
  t0 = Clock::now();
  const bool ok = cache->BuildStage(&stage, &batch_warn, &batch_err);
  t_build += since(t0);
  t0 = Clock::now();
  im->batch_scope_ = nullptr;
  Report(batch_warn, warn);
  Report(batch_err, err);
  if (!ok) return false;

  // Prototypes this batch claimed seed the later batches; a group with a
  // member other than its prototype is a Holder with peers.
  for (const auto &kp : im->prototype_by_key) {
    if (proto_keys->insert(kp.first).second)
      protos->emplace_back(kp.first, kp.second.str());
  }
  for (const auto &grp : im->instances_by_prototype) {
    for (const PathStr &m : grp.second) {
      if (m.str() != grp.first) {
        peers->insert(grp.first);
        break;
      }
    }
  }

  out->index = b;
  out->layer = stage.ReleaseRootLayer();
  const bool plan_pass = !planned;
  if (plan_pass) {
    const size_t last = bounds[b + 1] - 1;
    for (const LayerStack &st : im->layer_stacks) {
      for (const std::string &id : st.layer_identifiers) {
        size_t &l = layer_last_root[id];
        l = std::max(l, last);
      }
    }
  }
  if (out->layer && plan_pass) {
    const auto &prims = out->layer->prims();
    const size_t n = prims.size();
    std::unordered_map<std::string, size_t> root_at;
    for (size_t f = bounds[b]; f < bounds[b + 1]; ++f) root_at[frontier[f]] = f;
    // Subtree sizes (pre-order: children follow their parent).
    std::vector<size_t> size(n, 1);
    for (size_t i = n; i-- > 0;)
      for (uint32_t c : prims[i].child_indices()) size[i] += size[c];
    const size_t target = std::max<size_t>(1, batch_options.target_batch_prims);
    // A heavy prim splits into its children when it can be a spine prim:
    // it has children and is no instance (registered in no group).
    auto expandable = [&](size_t i) {
      return !prims[i].child_indices().empty() &&
             im->prototype_of.find(prims[i].path().str()) == im->prototype_of.end();
    };
    for (size_t i = 0; i < n; ++i) {
      const std::string &p = prims[i].path().str();
      auto it = root_at.find(p);
      if (it == root_at.end()) continue;
      root_prims[it->second] = size[i];
      std::vector<size_t> heavy;
      if (size[i] > target && expandable(i)) heavy.push_back(i);
      while (!heavy.empty()) {
        const size_t h = heavy.back();
        heavy.pop_back();
        std::vector<RefineChild> &kids = refine[prims[h].path().str()];
        for (uint32_t c : prims[h].child_indices()) {
          kids.push_back({prims[c].name(), size[c]});
          if (size[c] > target && expandable(c)) heavy.push_back(c);
        }
      }
    }
  }
  out->full_children = std::move(scope.full_children);
  const size_t n = out->layer ? out->layer->prims().size() : 0;
  out->spine.assign(n, 0);
  out->repeated.assign(n, 0);
  for (size_t i = 0; i < n; ++i) {
    const std::string &p = out->layer->prims()[i].path().str();
    if (!scope.spine.count(p)) continue;
    out->spine[i] = 1;
    if (!emitted_spine->insert(p).second) out->repeated[i] = 1;
  }
  t_post += since(t0);
  t0 = Clock::now();
  cache.reset();
  t_drop += since(t0);
  return true;
}

bool BatchedComposer::Impl::Pass(
    bool plan, const std::function<bool(ComposedBatch &)> &on_batch,
    std::string *warn, std::string *err) {
  std::vector<std::pair<std::string, std::string>> protos;
  std::unordered_set<std::string> proto_keys;
  std::unordered_set<std::string> peers;
  std::unordered_set<std::string> emitted_spine;
  const size_t target = std::max<size_t>(1, batch_options.target_batch_prims);
  struct RootStat {
    size_t roots = 0, prims = 0;
  };
  std::unordered_map<std::string, RootStat> by_parent;
  std::unordered_map<size_t, RootStat> by_depth;
  if (plan) {
    root_prims.assign(frontier.size(), 0);
    if (batch_options.roots_per_batch == 0) bounds = {0, 1};
  } else if (batch_options.roots_per_batch) {
    root_origin.resize(frontier.size());
    for (size_t f = 0; f < frontier.size(); ++f) root_origin[f] = f;
  } else {
    RefineFrontier();
    // Cut at the target from pass 1's exact counts.
    bounds = {0};
    size_t sum = 0;
    for (size_t f = 0; f < frontier.size(); ++f) {
      if (sum && sum + root_prims[f] > target) {
        bounds.push_back(f);
        sum = 0;
      }
      sum += root_prims[f];
    }
    bounds.push_back(frontier.size());
  }
  const auto t0 = std::chrono::steady_clock::now();
  size_t max_batch = 0, total = 0;
  std::vector<std::pair<size_t, std::string>> by_last;
  size_t release_at = 0;
  if (!plan) {
    by_last.reserve(layer_last_root.size());
    for (const auto &kv : layer_last_root) by_last.emplace_back(kv.second, kv.first);
    std::sort(by_last.begin(), by_last.end());
  }
#if defined(LIGHTUSD_ENABLE_THREAD)
  // The callback (the writer) overlaps the next batch's composition.
  std::unique_ptr<BatchConsumer> consumer;
  if (on_batch && batch_options.pipeline && options.num_threads != 1)
    consumer.reset(new BatchConsumer(on_batch));
#endif
  for (size_t b = 0; b + 1 < bounds.size(); ++b) {
    ComposedBatch batch;
    if (!ComposeBatch(b, &protos, &proto_keys, &peers, &batch, &emitted_spine,
                      warn, err))
      return false;
    if (batch.layer) {
      max_batch = std::max(max_batch, batch.layer->prims().size());
      total += batch.layer->prims().size();
    }
    const auto tc = std::chrono::steady_clock::now();
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (consumer) {
      if (!consumer->Push(std::move(batch))) return false;
    } else
#endif
    if (on_batch && !on_batch(batch)) {
      return false;
    }
    t_callback += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - tc).count();
    if (!plan && !root_origin.empty()) {
      // Layers no later batch uses (per pass 1) leave the registry; the
      // composed values keep their own array sources.
      batch = ComposedBatch();
      const size_t next = bounds[b + 1] < frontier.size()
                              ? root_origin[bounds[b + 1]]
                              : SIZE_MAX;
      while (release_at < by_last.size() && by_last[release_at].first < next) {
        registry.Drop(by_last[release_at].second);
        ++release_at;
        ++layers_released;
      }
    }
    if (plan && batch_options.roots_per_batch == 0 &&
        bounds.back() < frontier.size()) {
      // Next batch: roots up to the target, each estimated by the mean of the
      // composed roots under its parent (siblings are alike). A root under a
      // new parent is estimated by the mean at its depth, and only a few such
      // roots join one batch: cousins differ by orders of magnitude.
      constexpr size_t kNewParentRoots = 4;
      for (size_t f = bounds[b]; f < bounds[b + 1]; ++f) {
        RootStat &ps = by_parent[ParentPath(frontier[f])];
        ps.roots += 1;
        ps.prims += root_prims[f];
        RootStat &ds = by_depth[std::count(frontier[f].begin(), frontier[f].end(), '/')];
        ds.roots += 1;
        ds.prims += root_prims[f];
      }
      size_t f = bounds.back(), sum = 0, fresh = 0;
      while (f < frontier.size()) {
        size_t est = 1;
        auto pit = by_parent.find(ParentPath(frontier[f]));
        if (pit != by_parent.end()) {
          est = pit->second.prims / pit->second.roots;
        } else {
          if (fresh == kNewParentRoots) break;
          ++fresh;
          auto dit = by_depth.find(std::count(frontier[f].begin(), frontier[f].end(), '/'));
          if (dit != by_depth.end()) est = dit->second.prims / dit->second.roots;
        }
        if (f > bounds.back() && sum + est > target) break;
        sum += est;
        ++f;
      }
      bounds.push_back(f);
    }
  }
  double consumer_ms = 0;
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (consumer) {
    const auto tc = std::chrono::steady_clock::now();
    const bool consumed = consumer->Finish();
    t_callback += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - tc).count();
    consumer_ms = consumer->busy_ms();
    if (!consumed) return false;
  }
#endif
  if (options.enable_timing) {
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    LIGHTUSD_LOG_I(std::string("[next_batched] ") + (plan ? "plan" : "run") +
                   "=" + std::to_string(ms) + "ms batches=" +
                   std::to_string(bounds.size() - 1) + " roots=" +
                   std::to_string(frontier.size()) + " prims=" +
                   std::to_string(total) + " max_batch_prims=" +
                   std::to_string(max_batch) + " layers_released=" +
                   std::to_string(layers_released) + " open=" +
                   std::to_string(t_open) + "ms build=" + std::to_string(t_build) +
                   "ms post=" + std::to_string(t_post) + "ms drop=" +
                   std::to_string(t_drop) + "ms callback_wait=" +
                   std::to_string(t_callback) + "ms consumer=" +
                   std::to_string(consumer_ms) + "ms");
    t_open = t_build = t_post = t_drop = t_callback = 0;
  }
  if (plan) {
    holders_with_peers = std::move(peers);
    planned = true;
  }
  return true;
}

void BatchedComposer::Impl::Report(const std::string &lines, std::string *out) {
  if (!out || lines.empty()) return;
  size_t start = 0;
  while (start < lines.size()) {
    size_t end = lines.find('\n', start);
    if (end == std::string::npos) end = lines.size();
    std::string line = lines.substr(start, end - start);
    if (!line.empty() && reported.insert(line).second) {
      *out += line;
      *out += '\n';
    }
    start = end + 1;
  }
}

void BatchedComposer::Impl::RefineFrontier() {
  if (refine.empty()) {
    root_origin.resize(frontier.size());
    for (size_t f = 0; f < frontier.size(); ++f) root_origin[f] = f;
    return;
  }
  std::vector<std::string> out;
  std::vector<size_t> out_prims;
  root_origin.clear();
  struct Item {
    std::string path;
    size_t prims;
  };
  for (size_t f = 0; f < frontier.size(); ++f) {
    std::vector<Item> stack{{frontier[f], root_prims[f]}};
    while (!stack.empty()) {
      Item it = std::move(stack.back());
      stack.pop_back();
      auto r = refine.find(it.path);
      if (r == refine.end()) {
        out.push_back(std::move(it.path));
        out_prims.push_back(it.prims);
        root_origin.push_back(f);
        continue;
      }
      spine.insert(it.path);
      for (auto c = r->second.rbegin(); c != r->second.rend(); ++c)
        stack.push_back({it.path + "/" + c->name, c->prims});
    }
  }
  frontier = std::move(out);
  root_prims = std::move(out_prims);
  refine.clear();
}

BatchedComposer::BatchedComposer() : impl_(new Impl()) {}
BatchedComposer::~BatchedComposer() = default;

bool BatchedComposer::Open(const std::string &filename, AssetResolver &resolver,
                           const CompositionOptions &options,
                           const BatchedComposeOptions &batch_options,
                           std::string *warn, std::string *err) {
  Impl &d = *impl_;
  d.resolver = &resolver;
  d.options = options;
  d.batch_options = batch_options;
  d.root_identifier = filename;
  LayerLoadOptions lopts;
  lopts.max_memory = options.max_layer_memory;
  lopts.max_array_elements = options.max_array_elements;
  lopts.usdc_limits = options.usdc_limits;
  lopts.max_archive_entries = options.max_archive_entries;
  lopts.usdc_lazy_arrays = options.usdc_lazy_arrays;
  lopts.usdc_use_mmap = options.usdc_use_mmap;
  lopts.strict_aousd_conformance = options.strict_aousd_conformance;
  lopts.usda_parse_options = options.usda_parse_options;
  lopts.parse_num_threads = options.usda_parse_options.num_threads;
  if (options.strict_aousd_conformance)
    lopts.usda_parse_options.strict_aousd_conformance = true;
  d.root = LoadLayerFromFile(filename, warn, err, lopts);
  if (!d.root) return false;
  // ExtractedPrototypes numbers its prototypes over the whole stage, so it
  // composes in one piece (as does a layer with nothing to compose).
  d.whole = !LayerNeedsComposition(*d.root) ||
            options.instance_flatten_mode ==
                InstanceFlattenMode::ExtractedPrototypes;
  if (!d.whole) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!d.Discover(warn, err)) return false;
    if (options.enable_timing)
      LIGHTUSD_LOG_I("[next_batched] discover=" +
                     std::to_string(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count()) +
                     "ms roots=" + std::to_string(d.frontier.size()) +
                     " spine=" + std::to_string(d.spine.size()));
    if (d.frontier.empty()) d.whole = true;
  }
  if (d.whole) {
    d.bounds = {0, 1};
    return true;
  }
  if (batch_options.roots_per_batch) {
    for (size_t f = 0; f < d.frontier.size(); f += batch_options.roots_per_batch)
      d.bounds.push_back(f);
    d.bounds.push_back(d.frontier.size());
  }
  return true;
}

bool BatchedComposer::whole() const { return impl_->whole; }
size_t BatchedComposer::frontier_size() const { return impl_->frontier.size(); }
size_t BatchedComposer::batch_count() const { return impl_->bounds.size() - 1; }

bool BatchedComposer::Plan(std::string *warn, std::string *err,
                           const std::function<bool(ComposedBatch &)> &on_batch) {
  if (impl_->whole) {
    impl_->planned = true;
    return true;
  }
  return impl_->Pass(true, on_batch, warn, err);
}

bool BatchedComposer::Run(const std::function<bool(ComposedBatch &)> &on_batch,
                          std::string *warn, std::string *err) {
  Impl &d = *impl_;
  if (!d.planned) {
    if (err) *err += "batched compose: Run() before Plan()\n";
    return false;
  }
  if (d.whole) {
    Stage stage;
    if (!ComposeStageFromLayer(d.root, *d.resolver, &stage, d.root_identifier,
                               d.options, warn, err))
      return false;
    ComposedBatch batch;
    batch.layer = stage.ReleaseRootLayer();
    batch.spine.assign(batch.layer ? batch.layer->prims().size() : 0, 0);
    batch.repeated = batch.spine;
    return on_batch(batch);
  }
  return d.Pass(false, on_batch, warn, err);
}

bool ComposeStageBatched(const std::string &filename, AssetResolver &resolver,
                         Stage *out_stage, const CompositionOptions &options,
                         const BatchedComposeOptions &batch_options,
                         std::string *warn, std::string *err) {
  BatchedComposer composer;
  if (!composer.Open(filename, resolver, options, batch_options, warn, err))
    return false;
  if (!composer.Plan(warn, err)) return false;
  if (options.enable_timing)
    LIGHTUSD_LOG_I("[next_batched] frontier=" + std::to_string(composer.frontier_size()) +
                   " plan_batches=" + std::to_string(composer.batch_count()));
  std::unique_ptr<Layer> merged;
  std::unordered_map<std::string, uint32_t> spine_index;
  const bool ok = composer.Run(
      [&](ComposedBatch &batch) {
        if (!batch.layer) return true;
        if (composer.whole()) {
          merged = std::move(batch.layer);
          return true;
        }
        Layer &src = *batch.layer;
        if (!merged) {
          merged.reset(new Layer());
          merged->meta() = src.meta();
        }
        const size_t n = src.prims().size();
        std::vector<uint32_t> parent(n, UINT32_MAX);
        for (size_t i = 0; i < n; ++i)
          for (uint32_t c : src.prims()[i].child_indices()) parent[c] = uint32_t(i);
        std::vector<uint32_t> map(n, UINT32_MAX);
        for (size_t i = 0; i < n; ++i) {
          PrimSpec &p = *src.prim(uint32_t(i));
          const std::string path = p.path().str();
          if (batch.repeated[i]) {
            map[i] = spine_index.at(path);
            continue;
          }
          p.clear_child_indices();
          const uint32_t idx = merged->add_prim(std::move(p));
          map[i] = idx;
          if (parent[i] == UINT32_MAX) merged->add_root(idx);
          else merged->set_parent(idx, map[parent[i]]);
          if (batch.spine[i]) spine_index[path] = idx;
        }
        return true;
      },
      warn, err);
  if (!ok) return false;
  if (!merged) merged.reset(new Layer());
  if (!composer.whole()) {
    merged->set_lazy_path_index(true);
    merged->finalize();
  }
  out_stage->SetRootLayer(std::move(*merged));
  return true;
}

bool FlattenToUSDCFileBatched(const std::string &filename,
                              AssetResolver &resolver,
                              const std::string &out_path,
                              const CompositionOptions &options,
                              const BatchedComposeOptions &batch_options,
                              const CrateWriteOptions &write_options,
                              CrateWriteResult *result, std::string *warn,
                              std::string *err) {
  CrateWriteResult local;
  CrateWriteResult &res = result ? *result : local;
  BatchedComposer composer;
  if (!composer.Open(filename, resolver, options, batch_options, warn, err))
    return false;
  CrateWriter writer(write_options);
  if (composer.whole()) {
    // Nothing to batch: one regular compose and write.
    if (!composer.Plan(warn, err)) return false;
    bool ok = true;
    if (!composer.Run(
            [&](ComposedBatch &batch) {
              res = writer.WriteLayerToFile(out_path.c_str(), *batch.layer);
              ok = res.success;
              return ok;
            },
            warn, err))
      return false;
    return ok;
  }
  // Pass 1 fixes the writer's token/string table order; pass 2 writes.
  if (!composer.Plan(warn, err, [&](ComposedBatch &batch) {
        writer.AddBatchTables(*batch.layer, batch.repeated);
        return true;
      }))
    return false;
  if (!writer.BeginBatchedFile(out_path.c_str())) {
    res = writer.FinishBatched();
    if (res.error.empty()) res.error = "cannot open output file: " + out_path;
    return false;
  }
  const bool ran = composer.Run(
      [&](ComposedBatch &batch) {
        return writer.WriteBatch(*batch.layer, batch.spine, batch.repeated,
                                 batch.full_children);
      },
      warn, err);
  res = writer.FinishBatched();
  return ran && res.success;
}

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
