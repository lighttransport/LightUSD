// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "cache-internal.hh"

#include "../phase-faults.hh"
#include <queue>
namespace lightusd {
namespace next {
namespace pcp {

// ---------------------------------------------------------------------------
// Cache (thin forwarding shell)
// ---------------------------------------------------------------------------

Cache::Cache() : impl_(new Impl()) {}
Cache::~Cache() = default;
Cache::Cache(Cache &&) noexcept = default;
Cache &Cache::operator=(Cache &&) noexcept = default;

expected<Cache, std::string> Cache::Open(
    AssetResolver &resolver, std::shared_ptr<Layer> root_layer,
    const std::string &root_identifier, const CompositionOptions &options) {
  if (!root_layer) {
    return make_unexpected(std::string("pcp::Cache::Open: null root layer"));
  }
  Cache cache;
  cache.impl_->resolver = &resolver;
  cache.impl_->options = options;
  if (cache.impl_->options.strict_aousd_conformance) {
    cache.impl_->options.usda_parse_options.strict_aousd_conformance = true;
  }
  // Back-compat: a lone `flatten_instances = true` (mode left Native) means the
  // self-contained Holder flatten.
  if (cache.impl_->options.flatten_instances &&
      cache.impl_->options.instance_flatten_mode == InstanceFlattenMode::Native) {
    cache.impl_->options.instance_flatten_mode = InstanceFlattenMode::Holder;
  }
  cache.impl_->root_layer = root_layer;
  cache.impl_->root_identifier = root_identifier;

  std::string warn, err;
  uint32_t root_stack =
      cache.impl_->InternLayerStack(root_layer, root_identifier, &warn, &err);
  if (root_stack == Impl::kInvalidStack) {
    return make_unexpected(err.empty()
                                       ? std::string("pcp::Cache::Open: failed "
                                                     "to build root layer stack")
                                       : err);
  }
  return cache;
}

const PrimIndex *Cache::ComputePrimIndex(const Path &prim_path,
                                         std::string *warn, std::string *err) {
  return impl_->ComputePrimIndex(prim_path, warn, err);
}

bool Cache::PrewarmPrimIndices(const std::vector<Path> &paths, std::string *warn,
                               std::string *err) {
  return impl_->PrewarmPrimIndices(paths, warn, err);
}

bool Cache::BuildStage(Stage *stage, std::string *warn, std::string *err,
                       const PreviewCallback& preview_callback) {
  if (!stage) return false;
  const bool ok = impl_->BuildStage(stage, warn, err, preview_callback);
  if (ok) impl_->reg_->RetainImportedAssets(stage->GetRootLayer());
  return ok;
}

std::vector<std::string> Cache::GetLayerDependencies() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  std::vector<std::string> dependencies;
  auto physical_identifier = [](std::string identifier) {
    // Variant content has a synthetic layer-stack id
    // variant:<host>:<site>:<set>:<selection>. Its bytes live in <host>, which
    // is the dependency cache validation must stat. Nested variants unwrap
    // repeatedly. Keep Windows drive-letter colons: only the final three
    // composition suffixes are removed each iteration.
    while (identifier.compare(0, 8, "variant:") == 0) {
      std::string host = identifier.substr(8);
      bool valid = true;
      for (int suffix = 0; suffix < 3; ++suffix) {
        const size_t colon = host.rfind(':');
        if (colon == std::string::npos) { valid = false; break; }
        host.resize(colon);
      }
      if (!valid) break;
      identifier.swap(host);
    }
    return identifier;
  };
  for (const LayerStack& stack : impl_->layer_stacks) {
    for (const std::string& identifier : stack.layer_identifiers) {
      const std::string physical = physical_identifier(identifier);
      if (!physical.empty()) dependencies.push_back(physical);
    }
  }
  if (!impl_->root_identifier.empty()) {
    dependencies.push_back(impl_->root_identifier);
  }
  std::sort(dependencies.begin(), dependencies.end());
  dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                     dependencies.end());
  return dependencies;
}

Cache::MemoryStats Cache::GetMemoryStats() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  MemoryStats stats;
  auto add_source = [&stats](size_t bytes) {
    stats.source_layer_bytes =
        safe::saturating_add(stats.source_layer_bytes, bytes);
  };
  auto add_transient = [&stats](size_t bytes) {
    stats.transient_cache_bytes =
        safe::saturating_add(stats.transient_cache_bytes, bytes);
  };
  std::unordered_set<const Layer *> layers;
  for (const LayerStack &stack : impl_->layer_stacks) {
    for (const std::shared_ptr<Layer> &layer : stack.layers) {
      if (layer && layers.insert(layer.get()).second) {
        add_source(layer->memory_usage());
      }
    }
    add_transient(stack.identifier.capacity());
    add_transient(safe::saturating_mul(stack.layers.capacity(),
                                      sizeof(stack.layers[0])));
    add_transient(safe::saturating_mul(
        stack.layer_identifiers.capacity(), sizeof(stack.layer_identifiers[0])));
    for (const std::string &id : stack.layer_identifiers) {
      add_transient(id.capacity());
    }
    add_transient(safe::saturating_mul(
        stack.layer_offsets.capacity(), sizeof(stack.layer_offsets[0])));
  }
  stats.layer_count = layers.size();
  stats.prim_index_count = impl_->index_cache.size();
  stats.composed_prim_count = impl_->composed_cache_.size();

  for (const std::string &path : impl_->path_table) {
    add_transient(safe::saturating_add(sizeof(path), path.capacity()));
  }
  for (const auto &entry : impl_->index_cache) {
    add_transient(safe::saturating_add(sizeof(entry),
                                      entry.first.capacity()));
    if (!entry.second) continue;
    const std::vector<CompNode> &nodes = entry.second->GetNodes();
    add_transient(safe::saturating_mul(nodes.capacity(), sizeof(CompNode)));
    for (const CompNode &node : nodes) {
      add_transient(safe::saturating_mul(
          node.children.capacity(), sizeof(node.children[0])));
    }
    add_transient(safe::saturating_mul(
        entry.second->GetStrengthOrder().capacity(), sizeof(uint16_t)));
  }
  // Stats must not restore owned copies of borrowed source keys. A const
  // Cache still owns a mutable Impl, so select the const traversal explicitly.
  const SrcCache &sources = impl_->sources_cache;
  add_transient(safe::saturating_mul(sources.slots_.capacity(),
                                    sizeof(SrcCache::Slot)));
  add_transient(sources.live_.capacity());
  for (const std::string &key : sources.keys_) {
    add_transient(safe::saturating_add(sizeof(key), key.capacity()));
  }
  sources.for_each([&](const std::string &, const std::vector<Src> &srcs) {
    add_transient(sizeof(srcs));
    add_transient(safe::saturating_mul(srcs.capacity(), sizeof(Src)));
    for (const Src &source : srcs) {
      add_transient(source.site.capacity());
    }
  });
  for (const StackSpecCache &specs : impl_->spec_cache_by_stack_) {
    add_transient(safe::saturating_mul(specs.slots_.capacity(),
                                      sizeof(StackSpecCache::Slot)));
    for (const std::string &key : specs.keys_) {
      add_transient(safe::saturating_add(sizeof(key), key.capacity()));
    }
    for (const std::vector<SpecRef> &refs : specs.vals_) {
      add_transient(sizeof(refs));
      add_transient(safe::saturating_mul(refs.capacity(), sizeof(SpecRef)));
    }
  }
  for (const PathHashFilter &filter : impl_->path_filter_by_stack_) {
    add_transient(safe::saturating_mul(filter.words.capacity(), sizeof(uint64_t)));
  }
  for (const auto &entry : impl_->composed_cache_) {
    add_transient(safe::saturating_add(sizeof(entry),
                                      entry.first.capacity()));
    if (entry.second) add_transient(entry.second->memory_usage());
  }
  for (const auto &entry : impl_->composed_children_) {
    add_transient(safe::saturating_add(sizeof(entry),
                                      entry.first.capacity()));
    add_transient(safe::saturating_mul(entry.second.capacity(),
                                      sizeof(entry.second[0])));
    for (const std::string &child : entry.second) {
      add_transient(child.capacity());
    }
  }
  return stats;
}

void Cache::TrimTransientCaches() {
  NEXT_PCP_WRITE_LOCK(impl_->api_mu_);
  impl_->index_cache.clear();
  impl_->sources_cache.clear();
  impl_->composed_cache_.clear();
  impl_->composed_children_.clear();
  impl_->site_to_indices.clear();
  impl_->index_to_sites.clear();
  impl_->spec_cache_by_stack_.clear();
  impl_->path_filter_by_stack_.clear();
  impl_->path_filter_bits_ = 0;
  impl_->arc_target_memo_.clear();
  impl_->prototype_by_key.clear();
  impl_->prototype_of.clear();
  impl_->instances_by_prototype.clear();
  impl_->path_table.clear();
  impl_->path_intern.clear();
  impl_->nm_pool_.clear();
  impl_->nm_pool_.push_back(NamespaceMapping{});
  std::vector<std::pair<uint32_t, const std::vector<Src>*>>().swap(impl_->fill_);
}

const PrimSpec *Cache::ComposePrim(const Path &prim_path, std::string *warn,
                                   std::string *err) {
#if defined(LIGHTUSD_ENABLE_THREAD) && defined(LIGHTUSD_NEXT_FINE_LOCKS)
  {
    NEXT_PCP_READ_LOCK(impl_->api_mu_);
    auto it = impl_->composed_cache_.find(prim_path.str());
    if (it != impl_->composed_cache_.end()) return it->second.get();
  }
#endif
  NEXT_PCP_WRITE_LOCK(impl_->api_mu_);
  return impl_->ComposePrim_locked(prim_path, warn, err);
}

std::vector<std::string> Cache::ComposedChildNames(const Path &prim_path,
                                                   std::string *warn,
                                                   std::string *err) {
  NEXT_PCP_WRITE_LOCK(impl_->api_mu_);
  impl_->ComposePrim_locked(prim_path, warn, err);  // ensure cached
  auto it = impl_->composed_children_.find(prim_path.str());
  return it != impl_->composed_children_.end() ? it->second
                                               : std::vector<std::string>();
}

bool Cache::IsInstance(const Path &p) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  auto it = impl_->prototype_of.find(p.str());
  return it != impl_->prototype_of.end() && it->second.proto != p.str();
}
Path Cache::GetPrototype(const Path &p) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  auto it = impl_->prototype_of.find(p.str());
  return it != impl_->prototype_of.end() ? Path(it->second.proto.str()) : Path();
}
std::vector<Path> Cache::GetPrototypePaths() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  std::vector<Path> out;
  out.reserve(impl_->instances_by_prototype.size());
  for (const auto &kv : impl_->instances_by_prototype) out.push_back(Path(kv.first));
  return out;
}
std::vector<Path> Cache::GetInstancesForPrototype(const Path &proto) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  std::vector<Path> out;
  auto it = impl_->instances_by_prototype.find(proto.str());
  if (it != impl_->instances_by_prototype.end()) {
    for (const std::string &s : it->second) out.push_back(Path(s));
  }
  return out;
}
size_t Cache::PrototypeCount() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  return impl_->instances_by_prototype.size();
}
Path Cache::TranslatePathToPrototype(const Path &path) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  if (path.empty()) return Path();
  // An instance root A maps A -> prototype_of[A] (which differs from A; the
  // prototype maps to itself). Rewrite the nearest enclosing instance prefix,
  // then repeat so a nested instance living inside the prototype is translated
  // too. Bounded by a hard iteration cap: a prototype cannot structurally
  // contain itself (instance keys forbid it), so this converges well within.
  std::string cur = path.str();
  bool translated = false;
  for (int iter = 0; iter < 128; ++iter) {
    // Nearest ancestor (or cur itself) that is an instance (prototype_of maps
    // it to a *different* prototype root).
    std::string a;
    for (Path p(cur); !p.empty(); p = p.parent()) {
      auto it = impl_->prototype_of.find(p.str());
      if (it != impl_->prototype_of.end() && it->second.proto != p.str()) {
        a = p.str();
        break;
      }
      if (p.is_root()) break;
    }
    if (a.empty()) break;  // no enclosing instance remains
    // `a` is a prefix of `cur` on a '/' boundary (or equal); splice its
    // prototype root in for the instance-space prefix.
    cur = impl_->prototype_of.at(a).proto.str() + cur.substr(a.size());
    translated = true;
  }
  return translated ? Path(cur) : Path();
}
Path Cache::TranslatePathFromPrototype(const Path &proto_path,
                                       const Path &instance_root) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  auto it = impl_->prototype_of.find(instance_root.str());
  if (it == impl_->prototype_of.end() || it->second.proto == instance_root.str()) {
    return Path();  // instance_root is not an instance
  }
  const std::string &proto = it->second.proto.str();
  const std::string &pp = proto_path.str();
  if (pp == proto) return instance_root;  // the prototype root itself
  // proto must enclose proto_path on a '/' boundary.
  if (pp.size() > proto.size() && pp.compare(0, proto.size(), proto) == 0 &&
      pp[proto.size()] == '/') {
    return Path(instance_root.str() + pp.substr(proto.size()));
  }
  return Path();
}
std::string Cache::ComputeInstanceKey(const Path &p, std::string *warn,
                                      std::string *err) {
  return impl_->ComputeInstanceKey(p, warn, err);
}

bool Cache::LoadPayload(const Path &p, std::string *warn, std::string *err) {
  return impl_->LoadPayload(p, /*with_descendants=*/true, warn, err);
}
bool Cache::LoadPayload(const Path &p, LoadPolicy policy, std::string *warn,
                        std::string *err) {
  return impl_->LoadPayload(p, policy == LoadPolicy::WithDescendants, warn, err);
}
bool Cache::LoadPayloads(const std::vector<Path> &paths, LoadPolicy policy) {
  return impl_->LoadPayloads(paths,
                            policy == LoadPolicy::WithDescendants);
}
bool Cache::UnloadPayload(const Path &p) { return impl_->UnloadPayload(p); }
void Cache::SetLoadRules(const LoadRules &rules) { impl_->SetLoadRules(rules); }
LoadRules Cache::GetLoadRules() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  return impl_->load_rules_;
}
void Cache::SetVariantSelections(
    const CompositionOptions::VariantSelectionMap &selections) {
  impl_->SetVariantSelections(selections);
}
CompositionOptions::VariantSelectionMap Cache::GetVariantSelections() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  return impl_->options.variant_overrides_by_path;
}
bool Cache::HasDeferredPayload(const Path &p) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  return impl_->deferred_payload_prims.count(p.str()) != 0;
}
std::vector<Path> Cache::GetDeferredPayloadPaths() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  std::vector<Path> out;
  out.reserve(impl_->deferred_payload_prims.size());
  for (const std::string &s : impl_->deferred_payload_prims) out.push_back(Path(s));
  return out;
}

std::vector<Cache::CompositionIssue> Cache::GetCompositionIssues() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  return impl_->issues_;  // copy out under the lock (see header)
}
size_t Cache::GetCompositionIssueCount() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  return impl_->issues_.size();
}
bool Cache::GetCompositionIssue(size_t index, CompositionIssue* out) const {
  if (!out) return false;
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  if (index >= impl_->issues_.size()) return false;
  *out = impl_->issues_[index];
  return true;
}
void Cache::ClearCompositionIssues() {
  NEXT_PCP_WRITE_LOCK(impl_->api_mu_);
  impl_->issues_.clear();
}

void Cache::Invalidate(const Path &prim_path) { impl_->Invalidate(prim_path); }
void Cache::InvalidateLayer(const std::string &id) { impl_->InvalidateLayer(id); }
bool Cache::ReloadLayer(const std::string &id, std::string *warn,
                        std::string *err) {
  return impl_->ReloadLayer(id, warn, err);
}

bool Cache::HasComputedPrimIndex(const Path &prim_path) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  return impl_->index_cache.count(prim_path.str()) != 0;
}
size_t Cache::ComputedPrimIndexCount() const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  return impl_->index_cache.size();
}
const LayerRegistry &Cache::layer_registry() const { return impl_->registry; }
void Cache::PreloadLayer(const std::string &identifier,
                         std::shared_ptr<Layer> layer) {
  impl_->registry.Preload(identifier, std::move(layer));
}

// --- one-call composition helpers ------------------------------------------

namespace {
#if defined(LIGHTUSD_ENABLE_THREAD)
// Destroys composition caches off the caller's thread (background_reclaim);
// joined by WaitForBackgroundReclaim or at static destruction.
class BackgroundReclaimer {
 public:
  static BackgroundReclaimer &Get() {
    static BackgroundReclaimer r;
    return r;
  }
  void Add(Cache &&cache) {
    std::unique_ptr<Cache> owned(new Cache(std::move(cache)));
    std::lock_guard<std::mutex> lk(mu_);
    threads_.emplace_back([c = std::move(owned)]() mutable { c.reset(); });
  }
  void Wait() {
    std::vector<std::thread> threads;
    {
      std::lock_guard<std::mutex> lk(mu_);
      threads.swap(threads_);
    }
    for (std::thread &th : threads) th.join();
  }
  ~BackgroundReclaimer() { Wait(); }

 private:
  std::mutex mu_;
  std::vector<std::thread> threads_;
};
#endif
}  // namespace

void WaitForBackgroundReclaim() {
#if defined(LIGHTUSD_ENABLE_THREAD)
  BackgroundReclaimer::Get().Wait();
#endif
}

bool ComposeStageFromLayer(std::shared_ptr<Layer> root_layer,
                           AssetResolver &resolver, Stage *out_stage,
                           const std::string &root_identifier,
                           const CompositionOptions &options, std::string *warn,
                           std::string *err, CompositionReport *report) {
  if (!root_layer || !out_stage) return false;

  // A layer without composition work is already a complete stage.
  // Building a Cache/PrimIndex for it would clone every PrimSpec and
  // copy every lazy Value handle before the caller writes the stage again.
  // Keep the source layer intact so crate-backed arrays remain eligible for
  // the writer's byte-range pass-through path.  This is also the behavior of
  // the public composed loader's no-composition fast path.
  if (!LayerNeedsComposition(*root_layer)) {
    if (report) {
      // Match the composed path's overwrite semantics when callers reuse a
      // report object across loads.
      report->layer_dependencies.clear();
      report->issues.clear();
      if (!root_identifier.empty()) {
        report->layer_dependencies.push_back(root_identifier);
      }
    }
    out_stage->SetRootLayer(std::move(*root_layer));
    return true;
  }

  const bool timing = options.enable_timing;
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };
  const auto t0 = Clock::now();
  // The Cache is local to this call and discarded right after BuildStage, so
  // its cached composition sources can always be freed during the fill.
  CompositionOptions build_options = options;
  build_options.release_build_sources = true;
  auto opened = Cache::Open(resolver, std::move(root_layer), root_identifier,
                            build_options);
  if (!opened) {
    if (err) *err += opened.error() + "\n";
    return false;
  }
  Cache cache = std::move(*opened);
  const auto t1 = Clock::now();
  bool ok = cache.BuildStage(out_stage, warn, err);
  if (report) {
    report->layer_dependencies = cache.GetLayerDependencies();
    report->issues = cache.GetCompositionIssues();
  }
  const auto t2 = Clock::now();
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
  // A deferred-fill stage composes through this cache later: hand it over.
  if (Layer *root_out = out_stage->GetRootLayer()) {
    if (PrimFiller *filler = root_out->prim_filler())
      filler->keepalive = std::make_shared<Cache>(std::move(cache));
  }
#endif
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (options.background_reclaim) {
    BackgroundReclaimer::Get().Add(std::move(cache));
  } else
#endif
  {
    cache = Cache();  // synchronous reclamation belongs to load/compose latency
  }
  const auto t3 = Clock::now();
  if (timing) {
    LIGHTUSD_LOG_I("[next_compose] open=" + FormatMilliseconds(ms(t1 - t0)) +
                "ms build_stage=" + FormatMilliseconds(ms(t2 - t1)) +
                "ms reclaim=" + FormatMilliseconds(ms(t3 - t2)) + "ms");
  }
  return ok;
}

bool ComposeStageFromFile(const std::string &filename, AssetResolver &resolver,
                          Stage *out_stage, const CompositionOptions &options,
                          std::string *warn, std::string *err) {
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
  if (options.strict_aousd_conformance) {
    lopts.usda_parse_options.strict_aousd_conformance = true;
  }
  std::shared_ptr<Layer> root = LoadLayerFromFile(
      filename, warn, err, lopts);
  if (!root) return false;
  return ComposeStageFromLayer(std::move(root), resolver, out_stage, filename,
                               options, warn, err);
}

}  // namespace pcp
}  // namespace next
}  // namespace lightusd

namespace lightusd { namespace next { namespace pcp {

auto Cache::Impl::BuildStage(Layer *out, const BuildStageWork &root_work,
                  std::string *warn, std::string *err) -> void {
    using ProfClock = std::chrono::steady_clock;
    const bool prof = options.enable_timing;
    std::vector<BuildStageWork> stack;
    stack.push_back(root_work);

    while (!stack.empty()) {
      BuildStageWork w = std::move(stack.back());
      stack.pop_back();

      // Namespace-depth backstop: an arc-induced namespace that keeps growing
      // (e.g. an ancestor cycle a stronger check missed) must surface as an
      // error, never as C++ stack exhaustion.
      if (w.depth > options.max_namespace_depth) {
        AddIssue(ErrorCode::MaxDepthExceeded, w.out_path.str(),
                 "BuildStage max namespace depth exceeded at: " + w.out_path.str(),
                 err);
        continue;
      }
      auto ps0 = prof ? ProfClock::now() : ProfClock::time_point{};
      // Parallel warm keys stack-0 sources directly by the composed path.
      // Avoid SourcesKey's path-string copy on the overwhelmingly common hit;
      // retain the full resolver for an unwarmed/mutated miss.
      const std::vector<Src>* cached_srcs =
          FindCachedSources(w.out_path.str());
      if (prof) {
        if (cached_srcs) {
          ++prof_source_hits_;
        } else {
          ++prof_source_misses_;
        }
        ++prof_structure_nodes_;
      }
      const std::vector<Src> &srcs = cached_srcs
                                         ? *cached_srcs
                                         : SourcesForPath(w.out_path, warn, err);
      auto ps1 = prof ? ProfClock::now() : ProfClock::time_point{};

      // STRUCTURE PASS: fix this prim's slot, parent, child order and instancing
      // now, but DEFER opinion composition to the parallel fill below. Child-name
      // order (ComposeChildNames) and instancing (RegisterInstance) depend only on
      // `srcs`, never on the prim's own composed opinions, so deferring opinions
      // does not change the tree the walk produces.
      PrimSpec spec(w.out_path.name());
      // The path is moved into the spec just before add_prim (below).
      std::vector<std::string> children;
      bool children_filtered = false;
      // pxr instancing rule: descendants of an instanceable prim compose ONLY
      // from the instance's key-defining arcs (direct references/payloads and
      // implied classes) — "overrides on descendent prims are ignored". Local
      // and ancestral-positional sources still apply to the instance ROOT
      // itself (property overrides there are allowed), so this prim's own
      // opinion fill keeps the full source list; only the CHILD derivation is
      // filtered (recursively, via the pre-seeded source cache below).
      std::vector<Src> shared_srcs;
      bool subtree_filtered = false;
      const bool instanceable = IsInstanceableSources(srcs);
      if (instanceable) {
        shared_srcs.reserve(srcs.size());
        for (const Src &s : srcs) {
          if (!s.variant && Mapping(s.map_idx).MapsFrom(s.site)) {
            shared_srcs.push_back(s);
            // Same (stack, site): carry the known spec state across the copy.
            shared_srcs.back().spec_state.v = s.spec_state.v;
          }
        }
        if (!shared_srcs.empty()) {
          subtree_filtered = true;
          children = ComposeChildNames(shared_srcs);
          children_filtered = true;
        }
      }
      // Preserve the unfiltered child set when the instanceable source list has
      // no shareable mapped sources.  In the common non-instanceable case this
      // also avoids constructing and then potentially replacing the vector.
      auto cn0 = prof ? ProfClock::now() : ProfClock::time_point{};
      if (!children_filtered) children = ComposeChildNames(srcs);
      if (batch_scope_ && batch_scope_->Masks(w.out_path.str()))
        batch_scope_->Filter(w.out_path.str(), &children);
      auto ps2 = prof ? ProfClock::now() : ProfClock::time_point{};
      if (prof) {
        prof_childnames_ns_ +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(ps2 - cn0).count();
      }
      if (prof) {
        prof_sources_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(ps1 - ps0).count();
        prof_compose_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(ps2 - ps1).count();
      }

      // Instancing: register this prim. If it is an instance (not the prototype
      // of its group), link it to the prototype and do NOT duplicate its subtree
      // -- children are provided by the prototype (UsdPrim follows
      // instance_prototype).
      auto pr0 = prof ? ProfClock::now() : ProfClock::time_point{};
      // A deactivated prim composes no subtree and forms NO instance in pxr —
      // registering it would extract an orphan Flattened_Prototype for
      // content the flatten output prunes.
      const bool composed_active = ComposedActive(srcs);
      const bool registered = composed_active && instanceable &&
                              RegisterInstanceKnownInstanceable(
                                  w.out_path.str(), srcs);
      if (prof) prof_reg_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(ProfClock::now() - pr0).count();
      auto pm0 = prof ? ProfClock::now() : ProfClock::time_point{};
      bool is_instance = false;
      // Only an instanceable prim (registered now, or by the warm pass with
      // the same sources) can be in prototype_of: skip the string lookup for
      // the rest.
      if (registered) {
        auto pit = prototype_of.find(w.out_path.str());
        if (pit != prototype_of.end() && pit->second.proto != w.out_path.str()) {
          spec.meta().instance_prototype() = pit->second.proto.str();
          is_instance = true;
        }
      }

      auto pm1 = prof ? ProfClock::now() : ProfClock::time_point{};
      if (prof) {
        prof_protomap_ns_ +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(pm1 - pm0).count();
      }
      spec.set_path(std::move(w.out_path));
      uint32_t idx = out->add_prim(std::move(spec));
      // From here the path lives in the stored prim. No prim is added to `out`
      // until the next iteration, so the reference stays valid.
      const Path &here = out->prim(idx)->path();
      if (registered) RecordInstanceIndex(here.str(), idx);
      auto ap1 = prof ? ProfClock::now() : ProfClock::time_point{};
      if (prof) {
        prof_addprim_ns_ +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(ap1 - pm1).count();
      }
      if (w.is_root) {
        out->add_root(idx);
      } else {
        out->set_parent(idx, w.parent_idx);
      }
      // Opinions for this slot are filled (in parallel) after the whole walk.
      fill_.push_back({idx, &srcs});

      if (is_instance) continue;  // prototype provides the subtree

      // A deactivated prim composes NO subtree (pxr prunes children of an
      // inactive prim; usdcat --flatten drops the prim itself too — the prim
      // is kept here, with its authored active=false, so stage consumers can
      // still query IsActive on it).
      if (!composed_active) continue;

      // Push children in reverse order so the first child processes first
      // (preserving the original visitation order). `children` is already the
      // post-relocation child list: departed names are gone and relocated-in
      // names are present (ComposeChildNames), and each child's sources resolve
      // back to its pre-relocation site (DeriveChildSources).
      if (subtree_filtered) {
        // Pre-seed each child's composed-namespace source cache with the
        // filtered derivation so the entire prototype subtree (recursively)
        // composes shared content only.
        for (const std::string &cn : children) {
          const Path child = here.append_child_unchecked(cn);
          const std::string ck = SourcesKey(0, child.str());
          if (!sources_cache.contains(ck)) {
            std::vector<Src> base =
                DeriveChildSources(shared_srcs, cn, child.str(), warn, err);
            GetOrCreateCachedSources(ck) = ExpandList(base, child.str(), warn, err);
          }
        }
      }
      // Child names come from spec names / relocation arrivals, which the
      // parsers already validated: skip the per-character identifier check.
      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        // The walk composes each child at its own path, so only out_path is
        // carried (src_path stays empty; one path string per prim, not two).
        stack.push_back({Path(), here.append_child_unchecked(*it), idx,
                         /*is_root=*/false, w.depth + 1});
      }
      if (prof) {
        prof_push_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
            ProfClock::now() - ap1).count();
      }
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
// First-claim registry for instance keys during the parallel structure walk.
// A rank is a prim's DFS position: the item indices from the top list down
// through nested tasks, compared lexicographically. The lowest claim of a key
// is its prototype candidate.
struct Cache::Impl::WalkClaims {
  static constexpr size_t kShards = 64;
  struct Shard {
    std::mutex m;
    std::unordered_map<std::string, std::vector<uint32_t>> rank;
  };
  Shard shards[kShards];
  // Keys whose prototype an earlier batch owns (batched build): every claim
  // in this walk is a later member.
  const std::unordered_set<std::string> *seeded = nullptr;
  // True when an earlier rank already claimed `key`; otherwise records `rank`
  // as the lowest claim and returns false.
  bool EarlierOrClaim(const std::string &key, const std::vector<uint32_t> &rank) {
    if (seeded && seeded->count(key)) return true;
    Shard &sh = shards[std::hash<std::string>()(key) % kShards];
    std::lock_guard<std::mutex> lk(sh.m);
    auto it = sh.rank.find(key);
    if (it == sh.rank.end()) {
      sh.rank.emplace(key, rank);
      return false;
    }
    const std::vector<uint32_t> &held = it->second;
    // A held rank that is a proper prefix of ours is the same position: a
    // prim re-walked at commit (CommitWalk) under its own rank.
    const bool prefix = held.size() < rank.size() &&
                        std::equal(held.begin(), held.end(), rank.begin());
    if (prefix) return false;
    if (held < rank) return true;
    it->second = rank;
    return false;
  }
};

// Committing-thread side of the parallel structure walk: task results,
// worker pause control and what a re-walk at commit needs.
struct Cache::Impl::WalkCommitCtx {
  std::function<std::vector<WalkItem> *(int32_t)> task_items;
  // Commit output, in final prim order: the walked item of each prim, its
  // parent's index (UINT32_MAX for a root) and the root indices. Items stay
  // where they are (task results / rewalks) until MaterializeWalk.
  uint32_t base = 0;
  std::vector<WalkItem *> nodes;
  std::vector<uint32_t> parents;
  std::vector<uint32_t> roots;
  std::deque<std::vector<WalkItem>> rewalks;
  WalkPause *pause = nullptr;
  WalkCanDonate can_donate;
  WalkDonate donate;
  WalkClaims *claims = nullptr;
  std::string *warn = nullptr;
  std::string *err = nullptr;
  void Pause() {
    std::unique_lock<std::mutex> lk(pause->m);
    pause->req.store(true, std::memory_order_seq_cst);
    pause->cv.wait(lk, [&] {
      return pause->parked.load(std::memory_order_seq_cst) ==
             pause->running.load(std::memory_order_seq_cst);
    });
  }
  void Resume() {
    {
      std::lock_guard<std::mutex> lk(pause->m);
      pause->req.store(false, std::memory_order_seq_cst);
    }
    pause->cv.notify_all();
  }
};

// The structure walk's per-prim work, done speculatively and without
// mutating shared state (worker threads install a read-only spec store).
// Items are appended in the serial walk's pre-order. A prim whose instance
// key was seen earlier in this walk, or claimed at an earlier DFS rank, is
// walked as an instance (children not expanded). Any entry may be handed to
// `donate` at the point the serial walk would reach it; a placeholder item
// then stands in for that subtree. Anything that would need a mutation
// (source-cache miss, unseeded prototype child, depth limit) marks the item
// kWalkFail and stops that branch; committing it falls back.
void Cache::Impl::SpeculativeWalk(const BuildStageWork &root,
                                  const std::vector<uint32_t> &rank_prefix,
                                  std::vector<WalkItem> *items,
                                  const WalkCanDonate &can_donate,
                                  const WalkDonate &donate, WalkClaims *claims,
                                  std::unordered_set<std::string> *local_keys,
                                  WalkPause *pause) {
    struct Entry {
      BuildStageWork w;
      uint32_t parent;
    };
    std::vector<Entry> stack;
    const size_t first = items->size();
    std::vector<uint32_t> rank = rank_prefix;
    rank.push_back(0);
    stack.push_back({root, UINT32_MAX});
    bool at_root = true;
    while (!stack.empty()) {
      if (pause) pause->Checkpoint();
      Entry e = std::move(stack.back());
      stack.pop_back();
      const uint32_t idx = static_cast<uint32_t>(items->size());
      rank.back() = idx;
      if (!at_root && !e.w.is_root) {
        if (can_donate(e.w)) {
          constexpr size_t kSiblingBundleSize = 16;
          std::vector<BuildStageWork> bundle;
          bundle.reserve(kSiblingBundleSize);
          const uint32_t parent = e.parent;
          const uint32_t depth = e.w.depth;
          bundle.push_back(std::move(e.w));
          while (bundle.size() < kSiblingBundleSize && !stack.empty() &&
                 stack.back().parent == parent &&
                 !stack.back().w.is_root && stack.back().w.depth == depth) {
            bundle.push_back(std::move(stack.back().w));
            stack.pop_back();
          }
          const int32_t tid = donate(std::move(bundle), rank);
          WalkItem ph;
          ph.task = tid;
          ph.parent = parent;
          items->push_back(std::move(ph));
          continue;
        }
      }
      at_root = false;
      WalkItem it;
      it.parent = e.parent;
      it.depth = e.w.depth;
      if (e.w.is_root) it.flags |= kWalkRoot;
      const std::vector<Src> *srcs =
          e.w.depth > options.max_namespace_depth
              ? nullptr
              : FindCachedSources(e.w.out_path.str());
      if (!srcs) {
        it.flags |= e.w.depth > options.max_namespace_depth ? kWalkFail
                                                            : kWalkMiss;
        it.path = std::move(e.w.out_path);
        items->push_back(std::move(it));
        continue;
      }
      // Same decisions as the serial walk (BuildStage above).
      std::vector<std::string> children;
      bool children_filtered = false;
      std::vector<Src> shared_srcs;
      bool subtree_filtered = false;
      const bool instanceable = IsInstanceableSources(*srcs);
      if (instanceable) {
        shared_srcs.reserve(srcs->size());
        for (const Src &s : *srcs) {
          if (!s.variant && Mapping(s.map_idx).MapsFrom(s.site)) {
            shared_srcs.push_back(s);
            shared_srcs.back().spec_state.v = s.spec_state.v;
          }
        }
        if (!shared_srcs.empty()) {
          subtree_filtered = true;
          children = ComposeChildNames(shared_srcs);
          children_filtered = true;
        }
      }
      if (!children_filtered) children = ComposeChildNames(*srcs);
      if (batch_scope_ && batch_scope_->Masks(e.w.out_path.str()))
        batch_scope_->Filter(e.w.out_path.str(), &children);
      const bool composed_active = ComposedActive(*srcs);
      bool walked_as_instance = false;
      if (composed_active && instanceable && options.detect_instances) {
        it.flags |= kWalkRegistered;
        it.ikey.reset(new std::string(ComputeInstanceKeyImpl(*srcs)));
        if (local_keys->count(*it.ikey) || claims->EarlierOrClaim(*it.ikey, rank)) {
          walked_as_instance = true;
          it.flags |= kWalkSpecInstance;
        } else {
          local_keys->insert(*it.ikey);
        }
      }
      it.path = std::move(e.w.out_path);
      it.srcs = srcs;
      items->push_back(std::move(it));
      if (walked_as_instance || !composed_active) continue;
      const Path &here = items->back().path;
      if (subtree_filtered) {
        // The serial walk seeds a prototype's child sources when absent; here
        // that would be a mutation, left to the commit (kWalkSeed).
        bool seeded = true;
        for (const std::string &cn : children) {
          const Path child = here.append_child_unchecked(cn);
          if (!HasCachedSources(SourcesKey(0, child.str()))) {
            seeded = false;
            break;
          }
        }
        if (!seeded) {
          items->back().flags |= kWalkSeed;
          continue;
        }
      }
      for (auto cit = children.rbegin(); cit != children.rend(); ++cit) {
        stack.push_back({BuildStageWork{Path(), here.append_child_unchecked(*cit),
                                        0, false, e.w.depth + 1},
                         idx});
      }
    }
    // Pre-order: an item's subtree ends one past its last descendant.
    for (size_t k = first; k < items->size(); ++k)
      (*items)[k].end = static_cast<uint32_t>(k + 1);
    for (size_t k = items->size(); k-- > first;) {
      const uint32_t p = (*items)[k].parent;
      if (p != UINT32_MAX && (*items)[p].end < (*items)[k].end)
        (*items)[p].end = (*items)[k].end;
    }
  }

// Replay walked items in order, with the serial walk's exact instance
// registration, assigning each kept prim its final index (bookkeeping only:
// the PrimSpecs are built afterwards, in parallel, by MaterializeWalk). A
// prim walked as a prototype that registration makes an instance drops its
// walked subtree. Items the walk left to the commit are resolved here, at
// their DFS position, exactly where the serial walk does the same mutation:
// with the workers parked, a source-cache miss resolves its sources, a
// prototype seeds its children's sources; then that prim's subtree is walked
// again (ranked at this position) and committed in place. A prim walked as
// an instance that turns out to be a prototype, or the depth limit, returns
// false (the caller falls back to the serial walk).
bool Cache::Impl::CommitWalk(std::vector<WalkItem> &items, uint32_t outer_parent,
                             const std::vector<uint32_t> &rank_prefix,
                             WalkCommitCtx &ctx) {
    std::vector<uint32_t> gidx(items.size(), UINT32_MAX);
    std::vector<uint32_t> rank = rank_prefix;
    rank.push_back(0);
    auto rewalk = [&](const WalkItem &it, uint32_t parent_g) {
      ctx.rewalks.emplace_back();
      std::vector<WalkItem> &again = ctx.rewalks.back();
      std::unordered_set<std::string> local;
      SpecReadOnlyStore store;
      SpecReadOnlyTLS() = &store;
      SpeculativeWalk(BuildStageWork{Path(), it.path, 0,
                                     (it.flags & kWalkRoot) != 0, it.depth},
                      rank, &again, ctx.can_donate, ctx.donate, ctx.claims,
                      &local, nullptr);
      SpecReadOnlyTLS() = nullptr;
      // Still unresolved after the mutation: do not loop.
      if (again.empty() || (again[0].flags & (kWalkMiss | kWalkSeed)))
        return false;
      return CommitWalk(again, parent_g, rank, ctx);
    };
    size_t k = 0;
    while (k < items.size()) {
      WalkItem &it = items[k];
      rank.back() = static_cast<uint32_t>(k);
      const uint32_t parent_g =
          it.parent == UINT32_MAX ? outer_parent : gidx[it.parent];
      if (it.task >= 0) {
        std::vector<WalkItem> *sub = ctx.task_items(it.task);
        if (!sub || !CommitWalk(*sub, parent_g, rank, ctx)) return false;
        ++k;
        continue;
      }
      if (it.flags & kWalkFail) return false;
      if (it.flags & kWalkMiss) {
        ctx.Pause();
        (void)SourcesForPath(it.path, ctx.warn, ctx.err);
        ctx.Resume();
        if (!rewalk(it, parent_g)) return false;
        ++k;
        continue;
      }
      const bool registered = (it.flags & kWalkRegistered) != 0;
      const bool walked_as_instance = (it.flags & kWalkSpecInstance) != 0;
      bool is_instance = false;
      if (registered) {
        const std::string &path = it.path.str();
        AssignPrototype(path, *it.ikey, false);
        auto pit = prototype_of.find(path);
        is_instance = pit != prototype_of.end() && pit->second.proto != path;
        it.ikey.reset();  // the prototype maps keep their own copy
      }
      if (walked_as_instance && !is_instance) return false;
      if ((it.flags & kWalkSeed) && !is_instance) {
        // A prototype whose child sources are not seeded yet: seed them as
        // the serial walk does (its registration above is idempotent when
        // the re-walk registers it again), then walk its subtree.
        ctx.Pause();
        std::vector<Src> shared_srcs;
        for (const Src &s : *it.srcs) {
          if (!s.variant && Mapping(s.map_idx).MapsFrom(s.site)) {
            shared_srcs.push_back(s);
            shared_srcs.back().spec_state.v = s.spec_state.v;
          }
        }
        const std::vector<std::string> children = ComposeChildNames(shared_srcs);
        for (const std::string &cn : children) {
          const Path child = it.path.append_child_unchecked(cn);
          const std::string ck = SourcesKey(0, child.str());
          if (!sources_cache.contains(ck)) {
            std::vector<Src> base = DeriveChildSources(shared_srcs, cn, child.str(),
                                                       ctx.warn, ctx.err);
            GetOrCreateCachedSources(ck) =
                ExpandList(base, child.str(), ctx.warn, ctx.err);
          }
        }
        ctx.Resume();
        if (!rewalk(it, parent_g)) return false;
        ++k;
        continue;
      }
      const uint32_t idx = ctx.base + static_cast<uint32_t>(ctx.nodes.size());
      if (is_instance) it.flags |= kWalkIsInstance;
      ctx.nodes.push_back(&it);
      if (it.flags & kWalkRoot) {
        ctx.parents.push_back(UINT32_MAX);
        ctx.roots.push_back(idx);
      } else {
        ctx.parents.push_back(parent_g);
      }
      if (registered) RecordInstanceIndex(it.path.str(), idx);
      fill_.push_back({idx, it.srcs});
      gidx[k] = idx;
      k = (is_instance && !walked_as_instance) ? it.end : k + 1;
    }
    return true;
  }

// Build the committed prims into `out` in parallel: each PrimSpec at its
// index (name, path, instance prototype) with its children in commit order
// (ascending index, as the serial walk's set_parent appends them), then the
// roots in commit order.
void Cache::Impl::MaterializeWalk(Layer *out, WalkCommitCtx &ctx, int threads) {
    const size_t n = ctx.nodes.size();
    const size_t base = ctx.base;
    // Children (CSR over parent index), filled in commit order.
    std::vector<uint32_t> off(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t p = ctx.parents[i];
      if (p != UINT32_MAX) ++off[p - base + 1];
    }
    for (size_t i = 0; i < n; ++i) off[i + 1] += off[i];
    std::vector<uint32_t> kids(off[n]);
    // Parent indices are strictly earlier than their children. Once an item
    // has been visited, its parent entry is dead and can hold its own child
    // cursor. This avoids copying `off` into a separate cursor vector.
    for (size_t i = 0; i < n; ++i) {
      const uint32_t p = ctx.parents[i];
      if (p != UINT32_MAX)
        kids[ctx.parents[p - base]++] = static_cast<uint32_t>(base + i);
      ctx.parents[i] = off[i];
    }
    out->resize_prims(base + n);
    auto build = [&](size_t lo, size_t hi) {
      for (size_t i = lo; i < hi; ++i) {
        WalkItem &it = *ctx.nodes[i];
        PrimSpec &ps = *out->prim_mutable(static_cast<uint32_t>(base + i));
        ps.set_name(it.path.name());
        if (it.flags & kWalkIsInstance) {
          // prototype_of no longer changes (prefer_min=false assignments
          // never demote a prototype): the same value the commit saw.
          ps.meta().instance_prototype() = prototype_of.find(it.path.str())->second.proto.str();
        }
        ps.set_path(std::move(it.path));
        if (off[i + 1] > off[i])
          ps.mutable_child_indices().assign(kids.begin() + off[i],
                                            kids.begin() + off[i + 1]);
      }
    };
    const size_t T = std::max<size_t>(1, std::min<size_t>(static_cast<size_t>(threads), 64));
    if (T == 1 || n < 65536) {
      build(0, n);
    } else {
      std::vector<std::thread> pool;
      pool.reserve(T - 1);
      for (size_t t = 1; t < T; ++t)
        pool.emplace_back([&build, t, n, T]() { build(n * t / T, n * (t + 1) / T); });
      build(0, n / T);
      for (std::thread &th : pool) th.join();
    }
    for (uint32_t r : ctx.roots) out->add_root(r);
  }

// Subtree-parallel structure walk with the serial walk's exact output.
// This thread walks the roots, handing every depth-2 entry to the task queue;
// worker threads walk tasks and, whenever some worker is idle with nothing
// queued, hand off entries of their own walk as new tasks (so one huge
// subtree still spreads over all threads). This thread then commits the top
// items in DFS order, splicing each task's items in as soon as it is done
// (CommitWalk) and freeing them. Returns false -- with `out`, fill_ and the
// instancing maps reset -- when a commit hits a failed item or a wrong
// instance guess; the caller then runs the serial walk.
bool Cache::Impl::ParallelStructureWalk(Layer *out,
                                        const std::vector<std::string> &root_names,
                                        int threads,
                                        const std::function<void()> &before_materialize,
                                        std::string *warn, std::string *err) {
    if (threads < 2) return false;
    struct TaskRec {
      std::vector<BuildStageWork> roots;  // same-parent sibling bundle
      std::vector<uint32_t> rank;  // DFS rank of the placeholder
      std::vector<WalkItem> items;
      std::atomic<bool> done{false};
    };
    std::unique_ptr<WalkClaims> claims(new WalkClaims());
    if (batch_scope_) claims->seeded = &batch_scope_->seeded_keys;
    std::deque<TaskRec> recs;    // grows only (stable element addresses)
    // Task ids not yet taken, DFS-earliest first: the commit consumes tasks
    // in DFS order, so the earliest pending one is always the most urgent.
    auto later = [&recs](int32_t a, int32_t b) {
      return recs[static_cast<size_t>(b)].rank < recs[static_cast<size_t>(a)].rank;
    };
    std::priority_queue<int32_t, std::vector<int32_t>, decltype(later)> queue(later);
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<size_t> busy{0};
    bool closed = false;         // top phase finished
    bool stop = false;
    std::atomic<int> idle{0};
    std::atomic<int> queued{0};
    size_t bundled_roots = 0;  // extra sibling entries per queued task
    WalkPause pause;
    // Task lookup by id without the queue lock: a fixed two-level table whose
    // entries are written (under `mu`) before the id is published. The
    // committing thread reaches a placeholder only after the task that made it
    // is done (acquire), so it always sees the entry.
    constexpr size_t kTaskChunk = size_t(1) << 16;
    std::vector<std::unique_ptr<TaskRec *[]>> task_table(size_t(1) << 14);
    // Completion handoff to the committing thread (see ctx.task_items).
    std::mutex cmu;
    std::condition_variable ccv;
    std::atomic<bool> commit_waiting{false};

    auto add_task = [&](std::vector<BuildStageWork> &&roots,
                        const std::vector<uint32_t> &rank) {
      // Every donating worker and the popping ones share `mu`: copy the rank
      // before taking it, and wake a worker after releasing it (a waiter
      // woken under the lock only blocks on it again).
      std::vector<uint32_t> rank_copy(rank);
      int32_t id;
      {
        std::lock_guard<std::mutex> lk(mu);
        id = static_cast<int32_t>(recs.size());
        if (!roots.empty()) bundled_roots += roots.size() - 1;
        recs.emplace_back();
        recs.back().roots = std::move(roots);
        recs.back().rank = std::move(rank_copy);
        std::unique_ptr<TaskRec *[]> &chunk = task_table[size_t(id) / kTaskChunk];
        if (!chunk) chunk.reset(new TaskRec *[kTaskChunk]);
        chunk[size_t(id) % kTaskChunk] = &recs.back();
        queue.push(id);
        queued.fetch_add(1);
      }
      cv.notify_one();
      return id;
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&]() {
        SpecReadOnlyStore store;
        SpecReadOnlyTLS() = &store;
        // Hand off work whenever a worker idles with nothing queued: the
        // commit consumes in DFS order, so short tasks keep it fed (coarser
        // hand-off measured slower: long tasks stall the in-order commit).
        const WalkCanDonate can_donate = [&](const BuildStageWork &) {
          return idle.load(std::memory_order_relaxed) >
                 queued.load(std::memory_order_relaxed);
        };
        const WalkDonate donate = [&](std::vector<BuildStageWork> &&roots,
                                      const std::vector<uint32_t> &rank) {
          return add_task(std::move(roots), rank);
        };
        for (;;) {
          int32_t id;
          TaskRec *cur = nullptr;
          {
            std::unique_lock<std::mutex> lk(mu);
            idle.fetch_add(1);
            cv.wait(lk, [&] {
              return stop || !queue.empty() || (closed && busy == 0);
            });
            idle.fetch_sub(1);
            if (stop || queue.empty()) break;
            id = queue.top();
            queue.pop();
            queued.fetch_sub(1);
            ++busy;
            // (deque growth may move its block map: index under the lock;
            // the element itself never moves)
            cur = &recs[static_cast<size_t>(id)];
          }
          TaskRec &rec = *cur;
          pause.Enter();  // parks right away while a pause is held
          std::unordered_set<std::string> local;
          for (const BuildStageWork &root : rec.roots) {
            SpeculativeWalk(root, rec.rank, &rec.items, can_donate, donate,
                            claims.get(), &local, &pause);
          }
          // Items live until MaterializeWalk; drop the push_back growth slack
          // now, before the commit takes pointers into the vector.
          if (rec.items.capacity() > rec.items.size() + rec.items.size() / 8) {
            std::vector<WalkItem>(std::make_move_iterator(rec.items.begin()),
                                  std::make_move_iterator(rec.items.end()))
                .swap(rec.items);
          }
          store.vals.clear();
          pause.Leave();
          rec.done.store(true, std::memory_order_seq_cst);
          if (commit_waiting.load(std::memory_order_seq_cst)) {
            std::lock_guard<std::mutex> lk(cmu);
            ccv.notify_one();
          }
          if (busy.fetch_sub(1) == 1) {  // lets idle workers see the end
            std::lock_guard<std::mutex> lk(mu);
            cv.notify_all();
          }
        }
        SpecReadOnlyTLS() = nullptr;
      });
    }

    // Top phase on this thread (normal spec resolution; workers only read).
    std::vector<WalkItem> top;
    std::unordered_set<std::string> top_keys;
    {
      SpecReadOnlyStore store;
      SpecReadOnlyTLS() = &store;
      const WalkCanDonate top_can_donate = [&](const BuildStageWork &w) {
        return w.depth >= 2;
      };
      const WalkDonate top_donate = [&](std::vector<BuildStageWork> &&roots,
                                        const std::vector<uint32_t> &rank) {
        return add_task(std::move(roots), rank);
      };
      for (const std::string &nm : root_names) {
        SpeculativeWalk({Path("/" + nm), Path("/" + nm), 0, true, 1}, {},
                        &top, top_can_donate, top_donate, claims.get(),
                        &top_keys, nullptr);
      }
      SpecReadOnlyTLS() = nullptr;
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      closed = true;
    }
    cv.notify_all();

    WalkCommitCtx ctx;
    // The commit appends about one entry per cached source set (the warm
    // cached ~one per composed prim): reserve once instead of doubling
    // these through ~10 M serial appends.
    ctx.nodes.reserve(sources_cache.size());
    ctx.parents.reserve(sources_cache.size());
    fill_.reserve(fill_.size() + sources_cache.size());
    ctx.task_items = [&](int32_t j) -> std::vector<WalkItem> * {
      TaskRec &rec = *task_table[size_t(j) / kTaskChunk][size_t(j) % kTaskChunk];
      // Tasks are mostly done by the time the commit reaches them: check,
      // spin briefly, and only then block (workers notify only a waiter).
      for (int spin = 0; spin < 4096 && !rec.done.load(std::memory_order_acquire);
           ++spin) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
      }
      if (!rec.done.load(std::memory_order_acquire)) {
        commit_waiting.store(true, std::memory_order_seq_cst);
        std::unique_lock<std::mutex> lk(cmu);
        ccv.wait(lk, [&] { return rec.done.load(std::memory_order_seq_cst); });
        commit_waiting.store(false, std::memory_order_relaxed);
      }
      return &rec.items;
    };
    ctx.pause = &pause;
    // A re-walk on this thread hands entries to idle workers like they do.
    ctx.can_donate = [&](const BuildStageWork &) {
      return idle.load(std::memory_order_relaxed) >
             queued.load(std::memory_order_relaxed);
    };
    ctx.donate = [&](std::vector<BuildStageWork> &&roots,
                     const std::vector<uint32_t> &rank) {
      return add_task(std::move(roots), rank);
    };
    ctx.claims = claims.get();
    ctx.warn = warn;
    ctx.err = err;
    ctx.base = static_cast<uint32_t>(out->prim_count());
    const bool ok = CommitWalk(top, UINT32_MAX, {}, ctx);
    {
      std::lock_guard<std::mutex> lk(mu);
      stop = true;
    }
    cv.notify_all();
    for (std::thread &th : pool) th.join();
    if (ok) {
      before_materialize();
      MaterializeWalk(out, ctx, threads);
    }
    if (!ok) {
      *out = Layer();
      fill_.clear();
      prototype_by_key.clear();
      prototype_of.clear();
      instances_by_prototype.clear();
      instance_key_order.clear();
      instance_prim_index_.clear();
    }
    if (options.enable_timing)
      LIGHTUSD_LOG_I("[next_walk] tasks=" + UIntToStr(recs.size()) +
                     " bundled_roots=" + UIntToStr(bundled_roots));
    return ok;
  }
#endif

// Prim index lookup for the flatten passes: the walk-recorded instance
// indices first, a full path->index map built once on the first miss.
namespace {
struct InstanceIndexLookup {
  using Recorded = std::unordered_map<std::string_view,
                                      Cache::Impl::InstanceIndexEntry>;
  const Layer *out;
  const Recorded &recorded;
  std::unordered_map<std::string, uint32_t> full;
  bool full_built = false;
  InstanceIndexLookup(const Layer *o, const Recorded &r) : out(o), recorded(r) {}
  // Returns UINT32_MAX when the path has no prim.
  uint32_t find(const std::string &path) {
    auto it = recorded.find(path);
    if (it != recorded.end()) return it->second.idx;
    if (!full_built) {
      for (uint32_t i = 0; i < out->prim_count(); ++i)
        if (const PrimSpec *p = out->prim(i)) full[p->path().str()] = i;
      full_built = true;
    }
    auto fit = full.find(path);
    return fit == full.end() ? UINT32_MAX : fit->second;
  }
};
}  // namespace

#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
// Composes a prim's opinions when the writer asks for it, with the same steps
// the eager path runs for every prim: ComposeOpinions (FillOpinions), the
// Holder instance flatten (FlattenInstances, recorded as per-prim fix-ups) and
// finalize_properties (Layer::finalize). Owns the fill-phase source / spec
// storage; `keepalive` holds the composition Cache it composes through.
class Cache::Impl::DeferredFiller final : public PrimFiller {
 public:
  Impl *impl = nullptr;
  std::vector<std::deque<std::vector<SpecRef>>> fill_specs;
  std::deque<std::vector<Src>> fill_sources;
  std::vector<const std::vector<Src> *> srcs;     // by prim index
  std::vector<const std::vector<Src> *> shared;   // sorted; kept after a fill
  bool release = false;
  struct Fixup {
    bool prototype = false;
    std::string ref;  // member: reference to its prototype
  };
  std::unordered_map<uint32_t, Fixup> fixups;  // read-only once filling starts

  void Fill(Layer &layer, uint32_t index) override {
    PrimSpec *ps = layer.prim_mutable(index);
    if (!ps) return;
    const std::vector<Src> *s = index < srcs.size() ? srcs[index] : nullptr;
    if (s) {
      impl->ComposeOpinions(*s, ps);
      if (release && !std::binary_search(shared.begin(), shared.end(), s))
        std::vector<Src>().swap(*const_cast<std::vector<Src> *>(s));
    }
    auto it = fixups.find(index);
    if (it != fixups.end()) {
      ps->meta().instanceable = !it->second.prototype;
      ps->meta().instance_prototype().clear();
      if (!it->second.prototype) {
        auto &refs = ps->meta().references;
        if (std::find(refs.begin(), refs.end(), it->second.ref) == refs.end())
          refs.push_back(it->second.ref);
      }
    }
    ps->finalize_properties();
  }
  // Back to the skeleton (the writer keeps views into the path string, which
  // stays in place).
  void Release(Layer &layer, uint32_t index) override {
    if (PrimSpec *ps = layer.prim_mutable(index)) ps->release_to_skeleton();
  }
};
#endif

auto Cache::Impl::ComposeRootNames() const -> std::vector<std::string> {
  // Root (pseudo-root child) names compose WEAK->STRONG too, like nested
  // children (pxr PcpComposeSiteChildNames): iterate the root layer stack
  // weakest-first so a sublayer-only root prim keeps its position. Prototype
  // assignment is order-independent (see test_prototype_order_independent).
  std::set<std::string> seen_root;
  std::vector<std::string> root_names;
  for (auto lit = layer_stacks[0].layers.rbegin();
       lit != layer_stacks[0].layers.rend(); ++lit) {
    const auto &lp = *lit;
    for (uint32_t ri : lp->root_indices()) {
      const PrimSpec *ps = lp->prim(ri);
      if (!ps) continue;
      if (seen_root.insert(ps->name()).second) root_names.push_back(ps->name());
    }
  }
  // A relocate may move a prim to a NEW ROOT PRIM. The root layer need not
  // author a spec for the new name, so take the names from the root layer
  // stack's namespace edits as well.
  if (const std::vector<StackRelocates::Arrival> *arrivals =
          layer_stacks[0].relocates.ArrivalsAt("/")) {
    for (const StackRelocates::Arrival &a : *arrivals) {
      if (seen_root.insert(a.name).second) root_names.push_back(a.name);
    }
  }
  return root_names;
}

auto Cache::Impl::BuildStage(Stage *stage, std::string *warn, std::string *err,
                  const Cache::PreviewCallback &preview_callback) -> bool {
    NEXT_PCP_LOCK(api_mu_);
    auto out = std::unique_ptr<Layer>(new Layer());
    // release_layers_during_fill: the source compaction and the fill free
    // gigabytes in the middle of the composition peak; return those pages on
    // a helper thread (joined when this build returns). (A purge right after
    // the warm merge contended with the structure walk for no lower peak.)
    MemoryTrimmer trimmer;
    struct TrimmerScope {
      MemoryTrimmer *&slot;
      ~TrimmerScope() { slot = nullptr; }
    } trimmer_scope{trimmer_};
    if (options.release_layers_during_fill && !batch_scope_) trimmer_ = &trimmer;

    // BuildStage re-registers every prim's instancing from scratch below, so
    // reset the prototype maps first. They are otherwise only pruned via
    // index_cache-driven Invalidate, which BuildStage's direct RegisterInstance
    // path doesn't populate -- leaving stale groupings across recomposition
    // (e.g. after Load/UnloadPayload changes a prim's arc set and instance key).
    prototype_by_key.clear();
    prototype_of.clear();
    instances_by_prototype.clear();
    instance_key_order.clear();
    instance_prim_index_.clear();
    if (batch_scope_) {
      // Prototypes from earlier batches: their later members are instances.
      for (const auto &kp : batch_scope_->seeded_protos) {
        prototype_by_key[kp.first] = PathStr(kp.second);
        instance_key_order.push_back(kp.first);
      }
    }

    const Layer *root = layer_stacks[0].layers[0].get();
    std::vector<std::string> root_names = ComposeRootNames();
    if (batch_scope_ && batch_scope_->Masks("/"))
      batch_scope_->Filter("/", &root_names);
    prof_sources_ns_ = prof_compose_ns_ = prof_reg_ns_ = 0;
    prof_childnames_ns_ = prof_protomap_ns_ = prof_addprim_ns_ = prof_push_ns_ = 0;
    prof_source_hits_ = prof_source_misses_ = prof_structure_nodes_ = 0;
    const auto warm_t0 = std::chrono::steady_clock::now();
    // Residency attribution: the composed layer reaches ~75 GiB on the
    // all-district crate, so each phase reports its share of the page faults.
    const FaultCounters fault_t0 = ReadFaultCounters();
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Resolve independent namespace subtrees concurrently before the serial
    // structure walk. ParallelWarmSources was previously implemented but never
    // invoked, leaving num_threads effective only for the much cheaper opinion
    // fill pass.
    int warm_threads = options.num_threads;
    if (warm_threads < 0)
      warm_threads = std::min<int>(
          static_cast<int>(std::thread::hardware_concurrency()),
          kMaxExecutionThreads);
    warm_threads = std::min(warm_threads, kMaxExecutionThreads);
    // Load the external layers the scene reaches through reference/payload/
    // sublayer arcs on background threads while the serial frontier discovery
    // and structure walk run. Those walks otherwise load each arc target on
    // first touch -- a serial parse in the middle of LIVRPS resolution; with
    // the prefetch running they find the layer already parsed or join its
    // in-flight load in the registry. Byte-neutral: the registry keys layers by
    // resolved path and the prefetch loads with the exact options the arc and
    // sublayer paths use, so every consumer gets the same Layer it would have
    // parsed itself. Stopped (and joined) right after the structure pass.
    ArcLayerPrefetch prefetch;
    // A batch (batched build) reaches only its own subtrees' layers, which
    // its workers load on first touch; the prefetch would crawl every arc of
    // every loaded layer again for each batch.
    if (warm_threads > 1 && !batch_scope_)
      StartArcLayerPrefetch(&prefetch, warm_threads);
    if (warm_threads > 1) {
      // Sources die with this build: drop inert ones while warming, before
      // they pile up (see DropInertSources).
      warm_drop_inert_ = options.release_build_sources;
      ParallelWarmSources(root_names, warm_threads, warn, err);
      warm_drop_inert_ = false;
    }
#endif
    const FaultCounters fault_warm1 = ReadFaultCounters();
    const auto structure_t0 = std::chrono::steady_clock::now();
    if (options.release_build_sources && sources_cache.size() >= 65536) {
      // A source's root-space site commonly equals the cache key. Reuse that
      // stable string before allocating the output's copies of those paths.
      const size_t borrowed = sources_cache.borrow_site_keys();
      if (options.enable_timing)
        LIGHTUSD_LOG_I("[next_lookup] borrowed_keys=" + UIntToStr(borrowed));
    }
    // Source warming has already discovered the namespace cardinality. Reserve
    // the flattened prim vector once before the structure pass; repeated
    // growth moves large PrimSpec objects and is especially expensive on the
    // 300k+ node Scene C sub-scenes. Keep small scenes on Layer's normal growth
    // path to avoid over-reserving for sparse compositions.
    if (sources_cache.size() >= 8192) {
      out->reserve(sources_cache.size());
    }
    // Structure pass (serial): walk every root, fixing each prim's slot/parent/
    // child-order/instancing and recording its (slot, sources) for opinion fill.
    // Namespace resolution is complete. fill_ borrows the source vectors,
    // not their lookup keys: keep those vectors address-stable while freeing
    // the large path-key/hash index before it overlaps the finished stage.
    // Prime every spec memo before discarding its lookup index. The fill
    // reads only these resolved vectors; keeping their deque storage in a
    // separate owner lets the large path keys and hash tables die first.
    // Declare this owner before fill_sources so source memos die first too.
    // Runs once the walk has committed every prim: right before the parallel
    // walk materializes its PrimSpecs (so the freed sources do not overlap
    // them), else at the start of the fill.
    std::vector<std::deque<std::vector<SpecRef>>> fill_specs;
    std::deque<std::vector<Src>> fill_sources;
    std::vector<const std::vector<Src> *> shared_fill_sources;
    bool fill_prepared = false;
    auto prepare_fill = [&]() {
      if (fill_prepared) return;
      fill_prepared = true;
      if (options.release_build_sources) {
        for (const auto &record : fill_) {
          if (!record.second) continue;
          for (const Src &source : *record.second) {
            if (!source.variant && !source.suppress_site_specs) SpecsFor(source);
          }
        }
        fill_specs.reserve(spec_cache_by_stack_.size());
        for (StackSpecCache &cache : spec_cache_by_stack_) {
          fill_specs.push_back(cache.take_values());
        }
        fill_sources = sources_cache.take_values();

        // Namespace, activation and instance keys are final. ComposeOpinions
        // skips empty/suppressed sources, so their strings and vector capacity
        // need not overlap the output's property storage. Retire lookup keys
        // first: some keys borrowed the first Src's site, which can move here.
        const auto compact_start = std::chrono::steady_clock::now();
        size_t discarded = 0, vector_bytes_released = 0;
        // Records may share one source vector; compact each distinct vector
        // once (a second pass over a compacted vector removes nothing, so the
        // result and the counts are unchanged). Distinct vectors are
        // independent, so the drop -- ~90M Src destructors on a full-LOD
        // stage, each a string free and shared_ptr releases -- runs in
        // parallel.
        std::vector<std::vector<Src> *> compact_targets;
        compact_targets.reserve(fill_.size());
        for (const auto &record : fill_) {
          if (record.second)
            compact_targets.push_back(const_cast<std::vector<Src> *>(record.second));
        }
        const auto pointer_sort_start = options.enable_timing
                                           ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
        const size_t sort_n = compact_targets.size();
        int sort_threads = options.num_threads;
#if defined(LIGHTUSD_ENABLE_THREAD)
        if (sort_threads < 0)
          sort_threads = static_cast<int>(std::thread::hardware_concurrency());
#else
        sort_threads = 1;
#endif
        sort_threads = std::max(1, std::min(sort_threads, 32));
        size_t sort_runs = 1;
        while (sort_runs * 2 <= static_cast<size_t>(sort_threads) &&
               sort_runs * 2 <= sort_n / 65536)
          sort_runs *= 2;
        const auto ptr_less = std::less<const std::vector<Src> *>{};
#if defined(LIGHTUSD_ENABLE_THREAD)
        if (sort_runs >= 2) {
          std::vector<size_t> bounds(sort_runs + 1);
          for (size_t t = 0; t <= sort_runs; ++t)
            bounds[t] = sort_n * t / sort_runs;
          auto sort_chunk = [&](size_t t) {
            std::sort(compact_targets.begin() + bounds[t],
                      compact_targets.begin() + bounds[t + 1], ptr_less);
          };
          std::vector<std::thread> pool;
          pool.reserve(sort_runs - 1);
          for (size_t t = 1; t < sort_runs; ++t)
            pool.emplace_back([&, t] { sort_chunk(t); });
          sort_chunk(0);
          for (std::thread &th : pool) th.join();

          std::vector<std::vector<Src> *> scratch(sort_n);
          std::vector<size_t> next_bounds;
          auto *src = &compact_targets;
          auto *dst = &scratch;
          while (sort_runs > 1) {
            const size_t merged_runs = (sort_runs + 1) / 2;
            next_bounds.resize(merged_runs + 1);
            for (size_t t = 0; t < merged_runs; ++t)
              next_bounds[t] = bounds[2 * t];
            next_bounds[merged_runs] = sort_n;
            auto merge_pair = [&](size_t t) {
              const size_t lo = bounds[2 * t];
              const size_t mid = bounds[std::min(2 * t + 1, sort_runs)];
              const size_t hi = bounds[std::min(2 * t + 2, sort_runs)];
              if (mid == hi) {
                std::copy(src->begin() + lo, src->begin() + hi,
                          dst->begin() + lo);
              } else {
                std::merge(src->begin() + lo, src->begin() + mid,
                           src->begin() + mid, src->begin() + hi,
                           dst->begin() + lo, ptr_less);
              }
            };
            pool.clear();
            pool.reserve(merged_runs > 0 ? merged_runs - 1 : 0);
            for (size_t t = 1; t < merged_runs; ++t)
              pool.emplace_back([&, t] { merge_pair(t); });
            merge_pair(0);
            for (std::thread &th : pool) th.join();
            std::swap(src, dst);
            bounds.swap(next_bounds);
            sort_runs = merged_runs;
          }
          if (src != &compact_targets) compact_targets.swap(scratch);
        } else
#endif
        {
          std::sort(compact_targets.begin(), compact_targets.end(), ptr_less);
        }
        size_t unique_targets = 0;
        for (size_t i = 0; i < compact_targets.size();) {
          size_t j = i + 1;
          while (j < compact_targets.size() &&
                 compact_targets[j] == compact_targets[i])
            ++j;
          if (j - i > 1) shared_fill_sources.push_back(compact_targets[i]);
          compact_targets[unique_targets++] = compact_targets[i];
          i = j;
        }
        compact_targets.resize(unique_targets);
        const double pointer_sort_ms = options.enable_timing
            ? std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - pointer_sort_start).count()
            : 0.0;
        const size_t compact_target_count = compact_targets.size();
        auto compact_range = [&](size_t lo, size_t hi, size_t *removed_out,
                                 size_t *bytes_out) {
          size_t removed_sum = 0, bytes_sum = 0;
          for (size_t i = lo; i < hi; ++i) {
            std::vector<Src> &sources = *compact_targets[i];
            const size_t before_capacity = sources.capacity();
            const size_t removed = CompactOpinionSources(sources);
            if (removed) {
              removed_sum += removed;
              if (before_capacity > sources.capacity())
                bytes_sum += (before_capacity - sources.capacity()) * sizeof(Src);
            }
          }
          *removed_out = removed_sum;
          *bytes_out = bytes_sum;
        };
#if defined(LIGHTUSD_ENABLE_THREAD)
        int compact_threads = options.num_threads;
        if (compact_threads < 0)
          compact_threads = static_cast<int>(std::thread::hardware_concurrency());
        compact_threads = std::max(1, std::min(compact_threads, kMaxExecutionThreads));
        if (compact_threads > 1 && compact_targets.size() >= 65536) {
          const size_t nt = static_cast<size_t>(compact_threads);
          std::vector<size_t> rem(nt, 0), bytes(nt, 0);
          std::vector<std::thread> pool;
          pool.reserve(nt);
          const size_t n = compact_targets.size();
          for (size_t t = 0; t < nt; ++t) {
            pool.emplace_back([&, t] {
              compact_range(n * t / nt, n * (t + 1) / nt, &rem[t], &bytes[t]);
            });
          }
          for (std::thread &th : pool) th.join();
          for (size_t t = 0; t < nt; ++t) {
            discarded = safe::saturating_add(discarded, rem[t]);
            vector_bytes_released =
                safe::saturating_add(vector_bytes_released, bytes[t]);
          }
        } else
#endif
        {
          compact_range(0, compact_targets.size(), &discarded,
                        &vector_bytes_released);
        }
        std::vector<std::vector<Src> *>().swap(compact_targets);
        // ~10 M inert sources were just freed; return their pages before the
        // PrimSpecs and opinions are built on top (allocators otherwise keep
        // them for seconds, through the composition peak).
        if (trimmer_) trimmer_->Request();
        if (options.enable_timing) {
          const double compact_ms =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - compact_start).count();
          LIGHTUSD_LOG_I("[next_fill] discarded_sources=" + UIntToStr(discarded) +
              " source_vector_bytes_released=" + UIntToStr(vector_bytes_released) +
              " compact_targets=" + UIntToStr(compact_target_count) +
              " shared_vectors=" + UIntToStr(shared_fill_sources.size()) +
              " pointer_sort=" + FormatMilliseconds(pointer_sort_ms) + "ms" +
              " compact=" + FormatMilliseconds(compact_ms) + "ms");
        }
      }
    };
    fill_.clear();
    bool walked = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Large scenes walk independent subtrees in parallel (same output; it
    // falls back here, with everything reset, when it cannot guarantee that).
    if (warm_threads > 1 && sources_cache.size() >= 65536) {
      walked = ParallelStructureWalk(out.get(), root_names, warm_threads,
                                     prepare_fill, warn, err);
      if (!walked && sources_cache.size() >= 8192)
        out->reserve(sources_cache.size());
    }
#endif
    if (!walked) {
      for (const std::string &nm : root_names) {
        BuildStage(out.get(), {Path("/" + nm), Path("/" + nm), 0, true, 1},
                   warn, err);
      }
    }
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Every layer the composed namespace needs is loaded by now; anything the
    // prefetch still has queued was not reached (e.g. arcs on inactive prims).
    StopArcLayerPrefetch(&prefetch, warn);
#endif
    if (trimmer_) {
      // The arc-layer prefetch loads every arc target it finds, including
      // unselected variants' (e.g. the proxy LODs of a full-LOD stage). A
      // layer no layer stack holds is never read by this build (composition
      // reads layers only through stacks): free those prims now, before the
      // composed prims are built.
      std::unordered_set<const Layer *> in_stacks;
      for (const LayerStack &st : layer_stacks)
        for (const auto &l : st.layers) in_stacks.insert(l.get());
      size_t unread = 0, unread_prims = 0;
      for (const std::shared_ptr<Layer> &l : reg_->layers()) {
        if (in_stacks.count(l.get()) || l->prims().empty()) continue;
        ++unread;
        unread_prims += l->prims().size();
        l->release_prims();
      }
      if (unread) trimmer_->Request();
      if (options.enable_timing)
        LIGHTUSD_LOG_I("[next_build] unread_layers_released=" + UIntToStr(unread) +
                       " prims=" + UIntToStr(unread_prims));
    }
    const FaultCounters fault_structure1 = ReadFaultCounters();
    const auto structure_t1 = std::chrono::steady_clock::now();
    if (preview_callback) {
      // The callback receives an independent immutable-in-practice Stage. The
      // authoritative layer remains owned here and is filled below only after
      // the callback returns, so a consumer may retain the preview safely.
      Layer preview_layer = BuildPreviewLayer(*out);
      preview_layer.meta() = root->meta();
      preview_layer.meta().subLayers.clear();
      preview_layer.meta().relocates.clear();
      preview_layer.finalize();
      Stage preview_stage;
      preview_stage.SetRootLayer(std::move(preview_layer));
      if (!preview_callback(std::move(preview_stage))) {
        if (err && err->empty()) *err = "stage preview callback cancelled";
        return false;
      }
    }
    // Opinion-fill pass: compose every slot's opinions, in parallel when
    // num_threads > 1 (each slot is independent; resolved vectors stay stable).
    auto fill_t0 = std::chrono::steady_clock::now();
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
    std::shared_ptr<DeferredFiller> deferred;
#endif
    if (options.enable_timing) LIGHTUSD_LOG_I("[next_fill] begin=1");
    {
      prepare_fill();
      if (options.enable_timing) LIGHTUSD_LOG_I("[next_fill] lookup_released=1");
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
      // Small stages fill eagerly: keeping the composition Cache alive through
      // the write costs more than deferring saves (merged ALab: +23% RSS).
      if (options.defer_opinion_fill && out->prim_count() >= 65536 &&
          options.instance_flatten_mode != InstanceFlattenMode::ExtractedPrototypes) {
        // Hand the fill state to the layer's filler instead of composing
        // every prim now (see DeferredFiller).
        GetPropNameTable().freeze();
        deferred = std::make_shared<DeferredFiller>();
        deferred->impl = this;
        deferred->release = options.release_build_sources;
        deferred->fill_specs = std::move(fill_specs);
        deferred->fill_sources = std::move(fill_sources);
        deferred->srcs.assign(out->prim_count(), nullptr);
        std::vector<const std::vector<Src> *> seen;
        seen.reserve(fill_.size());
        for (const auto &record : fill_) {
          if (record.first < deferred->srcs.size())
            deferred->srcs[record.first] = record.second;
          if (record.second) seen.push_back(record.second);
        }
        std::sort(seen.begin(), seen.end());
        for (size_t i = 1; i < seen.size(); ++i) {
          if (seen[i] == seen[i - 1] && (deferred->shared.empty() ||
                                         deferred->shared.back() != seen[i]))
            deferred->shared.push_back(seen[i]);
        }
      } else
#endif
      FillOpinions(out.get(), shared_fill_sources);
      std::vector<std::pair<uint32_t, const std::vector<Src>*>>().swap(fill_);
      // Drop any newly resolved entries as before. The transferred vectors
      // (including shared ones that FillOpinions could not drain) die here.
      if (options.release_build_sources) sources_cache.clear();
      // (Destruction order of the two owners, as when they were locals here.)
      std::deque<std::vector<Src>>().swap(fill_sources);
      std::vector<std::deque<std::vector<SpecRef>>>().swap(fill_specs);
    }
    const FaultCounters fault_fill1 = ReadFaultCounters();
    const auto fill_t1 = std::chrono::steady_clock::now();
    if (options.enable_timing) {
      prof_compose_ns_ +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              fill_t1 - fill_t0).count();
    }
    switch (options.instance_flatten_mode) {
      case InstanceFlattenMode::Holder:
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
        if (deferred) {
          // FlattenInstances' edits, recorded for after each prim's fill.
          if (!instances_by_prototype.empty()) {
            InstanceIndexLookup idx_by_path(out.get(), instance_prim_index_);
            for (const auto &grp : instances_by_prototype) {
              const std::string &proto_path = grp.first;
              bool has_instance = false;
              for (const std::string &m : grp.second)
                if (m != proto_path) { has_instance = true; break; }
              if (!has_instance) continue;
              const uint32_t proto_idx = idx_by_path.find(proto_path);
              if (proto_idx == UINT32_MAX) continue;
              deferred->fixups[proto_idx].prototype = true;
              const std::string ref = "<" + proto_path + ">";
              for (const std::string &m : grp.second) {
                if (m == proto_path) continue;
                const uint32_t m_idx = idx_by_path.find(m);
                if (m_idx == UINT32_MAX) continue;
                DeferredFiller::Fixup &fx = deferred->fixups[m_idx];
                fx.prototype = false;
                fx.ref = ref;
              }
            }
          }
          break;
        }
#endif
        FlattenInstances(out.get());
        break;
      case InstanceFlattenMode::ExtractedPrototypes:
        FlattenInstancesExtracted(out.get(), options.prototype_numbering);
        break;
      case InstanceFlattenMode::Native:
        break;
    }
    const FaultCounters fault_flatten1 = ReadFaultCounters();
    const auto flatten_t1 = std::chrono::steady_clock::now();

    // The composed layer is read by index (writers, Stage traversal); defer
    // its path->index map to the first path lookup instead of hashing every
    // prim path here.
    out->set_lazy_path_index(true);
    out->finalize();
    const FaultCounters fault_finalize1 = ReadFaultCounters();
    const auto finalize_t1 = std::chrono::steady_clock::now();
    if (options.enable_timing) {
      auto elapsed_ms = [](std::chrono::steady_clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
      };
      LIGHTUSD_LOG_I("[next_build] warm=" +
                  FormatMilliseconds(elapsed_ms(structure_t0 - warm_t0)) +
                  "ms sources=" +
                  FormatMilliseconds(prof_sources_ns_ / 1e6) +
                  "ms structure=" +
                  FormatMilliseconds(elapsed_ms(structure_t1 - structure_t0)) +
                  "ms compose=" + FormatMilliseconds(prof_compose_ns_ / 1e6) +
                  "ms register=" + FormatMilliseconds(prof_reg_ns_ / 1e6) +
                  "ms childnames=" +
                  FormatMilliseconds(prof_childnames_ns_ / 1e6) +
                  "ms protomap=" + FormatMilliseconds(prof_protomap_ns_ / 1e6) +
                  "ms addprim=" + FormatMilliseconds(prof_addprim_ns_ / 1e6) +
                  "ms push=" + FormatMilliseconds(prof_push_ns_ / 1e6) +
                  "ms source_hits=" + UIntToStr(prof_source_hits_) +
                  " source_misses=" + UIntToStr(prof_source_misses_) +
                  " nodes=" + UIntToStr(prof_structure_nodes_) +
                  " spec_filter=" + UIntToStr(prof_path_filter_built_) +
                  " skips=" + UIntToStr(prof_path_filter_skips_) +
                  " disabled=" + UIntToStr(prof_path_filter_disabled_) +
                  " filter_build=" +
                  FormatMilliseconds(prof_path_filter_build_ns_ / 1e6) +
                  "ms filter_mem=" + UIntToStr(path_filter_bits_ / 8 / 1024) +
                  "KiB filter_paths=" + UIntToStr(prof_path_filter_paths_) +
                  " flatten=" +
                  FormatMilliseconds(elapsed_ms(flatten_t1 - fill_t1)) +
                  "ms finalize=" +
                  FormatMilliseconds(elapsed_ms(finalize_t1 - flatten_t1)) +
                  "ms"
                  " faults=maj[" +
                  UIntToStr(fault_warm1.major - fault_t0.major) + "," +
                  UIntToStr(fault_structure1.major - fault_warm1.major) + "," +
                  UIntToStr(fault_fill1.major - fault_structure1.major) + "," +
                  UIntToStr(fault_flatten1.major - fault_fill1.major) + "," +
                  UIntToStr(fault_finalize1.major - fault_flatten1.major) +
                  "] min[" +
                  UIntToStr(fault_warm1.minor - fault_t0.minor) + "," +
                  UIntToStr(fault_structure1.minor - fault_warm1.minor) + "," +
                  UIntToStr(fault_fill1.minor - fault_structure1.minor) + "," +
                  UIntToStr(fault_flatten1.minor - fault_fill1.minor) + "," +
                  UIntToStr(fault_finalize1.minor - fault_flatten1.minor) +
                  "]");
    }
    out->meta() = root->meta();
    // Stage metadata comes from the ROOT layer only. pxr does NOT fall back
    // to sublayer-authored stage fields (verified against the 26.05 oracle:
    // a sublayer-only upAxis leaves the stage at the fallback "Y" and
    // usdcat --flatten omits it) — the old fill-from-sublayers pass here was
    // nonconformant.
    // Sublayer opinions are baked into the flattened output; keeping the
    // subLayers list would re-apply (now stale) layers when the exported
    // layer is read back. Relocates are likewise already APPLIED by the
    // stage-build walk — keeping them would re-relocate on re-read.
    out->meta().subLayers.clear();
    out->meta().relocates.clear();
    for (const std::string &path : deferred_payload_prims) {
      if (PrimSpec *prim = out->prim_at_path_mutable(path)) {
        prim->meta().loaded = false;
      }
    }
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
    if (deferred) out->set_prim_filler(std::move(deferred));
#endif
    stage->SetRootLayer(std::move(*out));
    return true;
  }


auto Cache::Impl::FlattenInstances(Layer *out) -> void {
    if (instances_by_prototype.empty()) return;

    InstanceIndexLookup idx_by_path(out, instance_prim_index_);

    // One group's Holder edits: the prototype stays a plain (non-instanceable)
    // reference target, every other member references it. Each prim is in at
    // most one group, so groups are independent. `find` resolves a path to
    // its prim index; false when the group needs the serial lookup instead.
    auto flatten_group = [&](const std::string &proto_path,
                             const std::vector<PathStr> &members,
                             auto &&find) -> bool {
      // A lone instanceable prim with no peers still composed its own subtree as
      // the prototype; leaving it untouched is already a valid flatten.
      bool has_instance = false;
      for (const std::string &m : members)
        if (m != proto_path) { has_instance = true; break; }
      // Batched build: the peers may all be in other batches.
      if (!has_instance && batch_scope_ && batch_scope_->holders_with_peers &&
          batch_scope_->holders_with_peers->count(proto_path))
        has_instance = true;
      if (!has_instance) return true;

      // A batched build's prototype may live in an earlier batch: its members
      // here are still rewritten.
      uint32_t proto_idx = UINT32_MAX;
      if (!(batch_scope_ && batch_scope_->seeded_proto_paths.count(proto_path)) &&
          !find(proto_path, &proto_idx))
        return false;
      if (proto_idx == UINT32_MAX && !batch_scope_) return true;
      std::vector<uint32_t> member_idx;
      member_idx.reserve(members.size());
      for (const std::string &m : members) {
        if (m == proto_path) continue;
        uint32_t m_idx = UINT32_MAX;
        if (!find(m, &m_idx)) return false;
        member_idx.push_back(m_idx);
      }
      // Holder: keep the composed subtree, drop the instanceable flag so it is a
      // plain reference target (an instanceable holder would hide its content).
      if (PrimSpec *proto = proto_idx == UINT32_MAX ? nullptr : out->prim(proto_idx)) {
        proto->meta().instanceable = false;
        proto->meta().instance_prototype().clear();
      }

      const std::string ref = "<" + proto_path + ">";
      for (const uint32_t m_idx : member_idx) {
        if (m_idx == UINT32_MAX) continue;
        PrimSpec *mp = out->prim(m_idx);
        if (!mp) continue;
        mp->meta().instanceable = true;
        mp->meta().instance_prototype().clear();
        // Avoid duplicate refs if FlattenInstances somehow runs twice.
        auto &refs = mp->meta().references;
        if (std::find(refs.begin(), refs.end(), ref) == refs.end())
          refs.push_back(ref);
      }
      return true;
    };
    auto serial_find = [&](const std::string &path, uint32_t *idx) {
      *idx = idx_by_path.find(path);
      return true;
    };
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Large stages (full-LOD: 1.06 M members in ~7 K groups) edit groups in
    // parallel. Workers only read the recorded index; a group with a path it
    // lacks is left whole to the serial pass (whose lookup may build the full
    // map), so every edit is applied once, by one thread.
    int nt = options.num_threads;
    if (nt < 0)
      nt = std::min<int>(static_cast<int>(std::thread::hardware_concurrency()),
                         kMaxExecutionThreads);
    nt = std::max(1, std::min(nt, 16));
    if (nt > 1 && instance_prim_index_.size() >= 65536) {
      std::vector<std::pair<const std::string *, const std::vector<PathStr> *>> groups;
      groups.reserve(instances_by_prototype.size());
      for (const auto &grp : instances_by_prototype)
        groups.emplace_back(&grp.first, &grp.second);
      auto recorded_find = [&](const std::string &path, uint32_t *idx) {
        auto it = instance_prim_index_.find(path);
        if (it == instance_prim_index_.end()) return false;
        *idx = it->second.idx;
        return true;
      };
      std::vector<uint8_t> deferred(groups.size(), 0);
      std::atomic<size_t> next{0};
      std::vector<std::thread> pool;
      for (int t = 0; t < nt; ++t) {
        pool.emplace_back([&] {
          for (size_t g; (g = next.fetch_add(1)) < groups.size();) {
            if (!flatten_group(*groups[g].first, *groups[g].second, recorded_find))
              deferred[g] = 1;
          }
        });
      }
      for (std::thread &th : pool) th.join();
      for (size_t g = 0; g < groups.size(); ++g)
        if (deferred[g]) flatten_group(*groups[g].first, *groups[g].second, serial_find);
      return;
    }
#endif
    for (const auto &grp : instances_by_prototype)
      flatten_group(grp.first, grp.second, serial_find);
  }

auto Cache::Impl::FlattenInstancesExtracted(Layer *out, PrototypeNumbering numbering) -> void {
    if (instances_by_prototype.empty()) return;

    // path -> prim index (for member rewrites in Pass 2). Member indices stay
    // valid: Pass 1 only ADDS prims (FP roots + clones), never reindexes.
    InstanceIndexLookup idx_by_path(out, instance_prim_index_);

    // --- order groups, assign /Flattened_Prototype_N ------------------------
    // members[0] == prototype path == namespace-pre-order-first instance.
    std::vector<std::string> ordered;
    {
      std::vector<std::string> protos;
      protos.reserve(instances_by_prototype.size());
      for (const auto &g : instances_by_prototype) protos.push_back(g.first);
      std::sort(protos.begin(), protos.end());  // by prototype path (== pxr front)
      if (numbering == PrototypeNumbering::Deterministic) {
        ordered = std::move(protos);
      } else {
        // UsdcatCompatible: pxr labels prototypes `__Prototype_{rank+1}` in
        // REVERSE discovery order (the LAST instance key to first appear during
        // the stage walk becomes __Prototype_1) — pxr registers prototypes on a
        // stack, so the numbering is last-in-first-numbered. GetPrototypes()
        // then re-sorts by label STRING (differs from numeric at >9 groups:
        // _1,_10,_2...). Verified against usdcat: instances referencing A,B,C
        // discovered in that order number A=_3, B=_2, C=_1.
        std::vector<std::string> front;
        front.reserve(protos.size());
        {
          std::set<std::string> remaining(protos.begin(), protos.end());
          for (size_t r = instance_key_order.size(); r-- > 0;) {
            auto kit = prototype_by_key.find(instance_key_order[r]);
            if (kit == prototype_by_key.end()) continue;
            if (remaining.erase(kit->second)) front.push_back(kit->second);
          }
          for (const std::string &pp : protos) {  // defensive leftovers
            if (remaining.count(pp)) front.push_back(pp);
          }
        }
        std::vector<std::pair<std::string, std::string>> labelled;
        labelled.reserve(front.size());
        for (size_t i = 0; i < front.size(); ++i)
          labelled.emplace_back("__Prototype_" + UIntToStr(i + 1), front[i]);
        std::sort(labelled.begin(), labelled.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        ordered.reserve(labelled.size());
        for (auto &lp : labelled) ordered.push_back(std::move(lp.second));
      }
    }

    std::unordered_map<std::string, std::string> fpath;  // proto -> "/Flattened_Prototype_j"
    {
      // "/Flattened_Prototype_N" is a root path: only a ROOT prim can clash.
      std::unordered_set<std::string> used;
      for (uint32_t ri : out->root_indices()) {
        if (const PrimSpec *p = out->prim(ri)) used.insert(p->path().str());
      }
      size_t n = 1;
      for (const std::string &proto : ordered) {
        std::string p;
        do { p = "/Flattened_Prototype_" + UIntToStr(n++); }
        while (!used.insert(p).second);  // skip user-named clashes
        fpath[proto] = p;
      }
    }

    // FP a descendant `d` should reference when cloned into a prototype subtree:
    // `d` itself if it is a (nested) prototype group key, else d's prototype.
    auto fp_ref_for = [&](const std::string &d_path,
                          const std::string &d_inst_proto) -> const std::string * {
      auto it = fpath.find(d_path);
      if (it != fpath.end()) return &it->second;            // nested prototype
      if (!d_inst_proto.empty()) {
        auto j = fpath.find(d_inst_proto);
        if (j != fpath.end()) return &j->second;            // nested instance
      }
      return nullptr;
    };

    // --- Pass 1: emit FP roots + clone each prototype's shared subtree -------
    // Reads ORIGINAL prims; all of Pass 1 runs before any Pass 2 orphaning.
    struct CloneWork {
      uint32_t src_idx;
      uint32_t dst_parent_idx;
      std::string dst_parent_path;
    };
    for (const std::string &proto : ordered) {
      const uint32_t proto_idx = idx_by_path.find(proto);
      if (proto_idx == UINT32_MAX) continue;
      const std::string &fp = fpath[proto];

      PrimSpec root(fp.substr(1));  // "Flattened_Prototype_j"
      root.set_path(Path(fp));
      root.set_specifier(PrimSpecifier::Over);  // typeless `over`, like usdcat
      uint32_t fp_idx = out->add_prim(std::move(root));
      out->add_root(fp_idx);

      // Push children reversed so the work STACK pops them first-child-first,
      // preserving source namespace order in the cloned subtree.
      std::vector<CloneWork> work;
      {
        const auto &cc = out->prim(proto_idx)->child_indices();
        for (auto it = cc.rbegin(); it != cc.rend(); ++it)
          work.push_back({*it, fp_idx, fp});
      }

      while (!work.empty()) {
        CloneWork cw = work.back();
        work.pop_back();
        // Capture everything from the source BEFORE add_prim (it may realloc).
        std::string name, new_path, src_path, inst_proto;
        std::vector<uint32_t> src_children;
        PrimSpec clone;
        {
          const PrimSpec *src = out->prim(cw.src_idx);
          if (!src) continue;
          name = src->name();
          src_path = src->path().str();
          inst_proto = src->meta().instance_prototype();
          src_children = src->child_indices();
          new_path = cw.dst_parent_path + "/" + name;
          clone = src->Clone();
        }
        const std::string *ref = fp_ref_for(src_path, inst_proto);
        const bool leaf = (ref != nullptr);

        clone.set_name(name);
        clone.set_path(Path(new_path));
        clone.clear_child_indices();  // Clone copied stale source child links
        // Retarget internal material:binding / connections that pointed inside
        // the prototype (under `proto`) to the moved /Flattened_Prototype_N root.
        clone.remap_target_prefix(proto, fp);
        if (leaf) {
          clone.meta().instanceable = true;
          clone.meta().instance_prototype().clear();
          clone.meta().references.clear();
          clone.meta().references.push_back("<" + *ref + ">");
          // pxr flatten spells the prototype arc `add references = </FP_N>`.
          ArcListOp &redit = clone.meta().ensure_arc_edits().references;
          redit = ArcListOp();
          redit.authored = true;
          redit.is_explicit = false;
          redit.added.push_back("<" + *ref + ">");
        }
        uint32_t new_idx = out->add_prim(std::move(clone));  // invalidates ptrs
        out->set_parent(new_idx, cw.dst_parent_idx);
        if (!leaf)
          for (auto it = src_children.rbegin(); it != src_children.rend(); ++it)
            work.push_back({*it, new_idx, new_path});
      }
    }

    // --- Pass 2: rewrite every member at its original namespace position -----
    for (const std::string &proto : ordered) {
      const std::string ref = "<" + fpath[proto] + ">";
      auto mit = instances_by_prototype.find(proto);
      if (mit == instances_by_prototype.end()) continue;
      for (const std::string &m : mit->second) {
        const uint32_t m_idx = idx_by_path.find(m);
        if (m_idx == UINT32_MAX) continue;
        PrimSpec *mp = out->prim(m_idx);
        if (!mp) continue;
        mp->meta().instanceable = true;
        mp->meta().instance_prototype().clear();
        mp->clear_child_indices();  // orphan inline subtree (now under the FP root)
        auto &refs = mp->meta().references;
        if (std::find(refs.begin(), refs.end(), ref) == refs.end())
          refs.push_back(ref);
        // pxr flatten spells the prototype arc `add references = </FP_N>`.
        ArcListOp &redit = mp->meta().ensure_arc_edits().references;
        redit = ArcListOp();
        redit.authored = true;
        redit.is_explicit = false;
        redit.added.push_back(ref);
      }
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::ArcLayerLoadOptions() const -> LayerLoadOptions {
    // Must match the options ProcessArc / AppendLayerAndSublayers load with:
    // the registry keys on the resolved path only, so the first loader's
    // options decide how the shared Layer is parsed.
    LayerLoadOptions load_opts;
    load_opts.max_memory = options.max_layer_memory;
    load_opts.max_array_elements = options.max_array_elements;
    load_opts.usdc_limits = options.usdc_limits;
    load_opts.max_archive_entries = options.max_archive_entries;
    load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
    load_opts.usdc_use_mmap = options.usdc_use_mmap;
    load_opts.usda_parse_options = options.usda_parse_options;
    load_opts.parse_num_threads = options.usda_parse_options.num_threads;
    load_opts.strict_aousd_conformance = options.strict_aousd_conformance;
    return load_opts;
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::CollectArcLayerTargets(const Layer &layer, const std::string &id,
                              bool payloads,
                              std::vector<ArcLayerPrefetch::Item> *out) const -> void {
    auto add = [&](const std::string &asset) {
      if (asset.empty() || asset.front() == '`') return;
      out->push_back({asset, id});
    };
    for (const std::string &sub : layer.meta().subLayers) add(sub);
    auto add_arcs = [&](const std::vector<std::string> &arcs, bool payload) {
      for (const std::string &s : arcs) {
        const CompositionArc a = payload ? Compositor::ParsePayload(s)
                                         : Compositor::ParseReference(s);
        add(a.asset_path);
      }
    };
    auto add_edit = [&](const ArcListOp &e, bool payload) {
      if (!e.authored) return;
      add_arcs(e.added, payload);
      add_arcs(e.prepended, payload);
      add_arcs(e.appended, payload);
    };
    for (const PrimSpec &ps : layer.prims()) {
      const PrimMetadata &m = ps.meta();
      add_arcs(m.references, false);
      if (payloads) add_arcs(m.payloads, true);
      if (const ArcListOps *e = m.arc_edits()) {
        add_edit(e->references, false);
        if (payloads) add_edit(e->payloads, true);
      }
    }
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::EnqueueArcLayerTargets(ArcLayerPrefetch *pf,
                              std::vector<ArcLayerPrefetch::Item> *items) -> void {
    // Resolve outside the lock (the resolver may touch the filesystem).
    std::vector<std::string> ids(items->size());
    for (size_t i = 0; i < items->size(); ++i) {
      ids[i] = resolver->ResolvePath((*items)[i].asset, (*items)[i].anchor,
                                     !options.strict_aousd_conformance);
    }
    std::lock_guard<std::mutex> lk(pf->mu);
    for (size_t i = 0; i < items->size(); ++i) {
      if (ids[i].empty() || !pf->seen.insert(ids[i]).second) continue;
      pf->queue.push_back(std::move((*items)[i]));
    }
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::StartArcLayerPrefetch(ArcLayerPrefetch *pf, int nt) -> void {
    if (!reg_ || !resolver) return;
    // Payload targets are prefetched only under the plain load-everything
    // policy; a callback or load rule may decline them.
    const bool payloads = options.load_payloads && !options.payload_policy &&
                          !options.payload_policy_with_prim &&
                          load_rules_.empty();
    LayerLoadOptions load_opts = ArcLayerLoadOptions();
    std::vector<ArcLayerPrefetch::Item> seeds;
    const LayerStack &root_stack = layer_stacks[0];
    for (size_t i = 0; i < root_stack.layers.size(); ++i) {
      const std::string &id = i < root_stack.layer_identifiers.size()
                                  ? root_stack.layer_identifiers[i]
                                  : root_stack.identifier;
      pf->seen.insert(id);
      CollectArcLayerTargets(*root_stack.layers[i], id, payloads, &seeds);
    }
    EnqueueArcLayerTargets(pf, &seeds);
    if (pf->queue.empty()) return;
    size_t n = std::min<size_t>(static_cast<size_t>(nt), pf->queue.size());
    if (load_opts.parse_num_threads <= 0) {
      // Divide the loading budget between files and each file's parser.
      // All-serial readers strand cores behind a large layer; an automatic
      // arena per outer worker would multiply the budget instead.
      n = std::min(n, static_cast<size_t>(std::max(1, nt / 4)));
      const int parse_threads = std::min(8, nt / static_cast<int>(n));
      load_opts.parse_num_threads = parse_threads;
      load_opts.usda_parse_options.num_threads = parse_threads;
      load_opts.min_parallel_parse_bytes = size_t{64} << 20;
    }
    if (options.enable_timing)
      LIGHTUSD_LOG_I("[next_prefetch] workers=" + UIntToStr(n) +
                    " parse_threads=" + IntToStr(load_opts.parse_num_threads));
    auto worker = [this, pf, payloads, load_opts]() {
      for (;;) {
        ArcLayerPrefetch::Item item;
        {
          std::unique_lock<std::mutex> lk(pf->mu);
          pf->cv.wait(lk, [pf]() {
            return pf->stop || !pf->queue.empty() || pf->active == 0;
          });
          if (pf->stop || pf->queue.empty()) {
            pf->cv.notify_all();  // queue drained with nothing in flight
            return;
          }
          item = std::move(pf->queue.front());
          pf->queue.pop_front();
          ++pf->active;
        }
        std::string w, e;
        std::shared_ptr<Layer> layer = reg_->GetOrLoad(
            *resolver, item.asset, item.anchor, &w, &e, load_opts);
        std::vector<ArcLayerPrefetch::Item> found;
        if (layer) {
          const std::string id = resolver->ResolvePath(
              item.asset, item.anchor, !options.strict_aousd_conformance);
          CollectArcLayerTargets(*layer, id, payloads, &found);
          EnqueueArcLayerTargets(pf, &found);
        }
        // Load errors are dropped: a failed load is not cached, so the
        // structure walk retries it and reports it at the authoring site.
        {
          std::lock_guard<std::mutex> lk(pf->mu);
          --pf->active;
          pf->warn += w;
        }
        pf->cv.notify_all();
      }
    };
    pf->threads.reserve(n);
    for (size_t t = 0; t < n; ++t) pf->threads.emplace_back(worker);
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::StopArcLayerPrefetch(ArcLayerPrefetch *pf, std::string *warn) -> void {
    pf->Join();
    if (warn) *warn += pf->warn;
    pf->warn.clear();
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::AdoptStack(const LayerStack &ws) -> uint32_t {
    const std::string &key =
        ws.cache_key.empty() ? ws.identifier : ws.cache_key;
    auto it = stack_by_id.find(key);
    if (it != stack_by_id.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(layer_stacks.size());
    layer_stacks.push_back(ws);
    stack_by_id.emplace(key, idx);
    return idx;
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::PreseedPrototypeChildren(const Path &proto, const std::vector<Src> &srcs,
                                std::vector<std::string> *children,
                                std::string *warn, std::string *err) -> bool {
    std::vector<Src> shared_srcs;
    for (const Src &s : srcs) {
      if (!s.variant && Mapping(s.map_idx).MapsFrom(s.site)) {
        shared_srcs.push_back(s);
      }
    }
    if (shared_srcs.empty()) return false;
    *children = ComposeChildNames(shared_srcs);
    for (const std::string &cn : *children) {
      const Path child = proto.append_child(cn);
      const std::string ck = SourcesKey(0, child.str());
      if (!HasCachedSources(ck)) {
        std::vector<Src> base =
            DeriveChildSources(shared_srcs, cn, child.str(), warn, err);
        GetOrCreateCachedSources(ck) = ExpandList(base, child.str(), warn, err);
      }
    }
    return true;
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::ExpandWarmNode(const WarmTask &it, WarmShared *shared,
                      std::vector<std::string> *children,
                      std::shared_ptr<const std::vector<Path>> *child_protos,
                      std::string *warn, std::string *err) -> bool {
    children->clear();
    *child_protos = it.protos;
    if (it.depth > options.max_namespace_depth) return false;
    const std::vector<Src> &srcs = SourcesForPath(it.p, warn, err);
    // An inactive prim composes no subtree (and forms no instance).
    if (!ComposedActive(srcs)) return false;
    if (options.detect_instances && IsInstanceableSources(srcs)) {
      if (!shared->Claim(ComputeInstanceKeyImpl(srcs), it.order)) {
        // A later instance: the serial walk skips its subtree. Should the
        // walk still expand it as a prototype, an instanceable prim with
        // shared (arc-targeted) sources derives its children from those
        // alone (PreseedPrototypeChildren / the walk's seeding), and every
        // shared source is kept -- so nothing reads the whole list again.
        if (warm_drop_inert_ && HasSharedSource(srcs)) {
          if (std::vector<Src> *own = sources_cache.find(it.p.str()))
            DropInertSources(it.p.str(), *own, nullptr);
        }
        return false;
      }
      if (PreseedPrototypeChildren(it.p, srcs, children, warn, err)) {
        // The prototype's children are seeded from its shared sources; the
        // check covers those (filtered) child names.
        if (warm_drop_inert_) {
          if (std::vector<Src> *own = sources_cache.find(it.p.str()))
            DropInertSources(it.p.str(), *own, children);
        }
        auto protos = std::make_shared<std::vector<Path>>();
        if (it.protos) *protos = *it.protos;
        protos->push_back(it.p);
        *child_protos = std::move(protos);
        if (batch_scope_ && batch_scope_->Masks(it.p.str()))
          batch_scope_->Filter(it.p.str(), children);
        return true;
      }
    }
    *children = ComposeChildNames(srcs);
    if (batch_scope_ && batch_scope_->Masks(it.p.str()))
      batch_scope_->Filter(it.p.str(), children);
    if (warm_drop_inert_ && children->empty()) {
      // A leaf derives nothing: drop its inert sources right away. (A parent
      // waits for its last child, see WarmSubtree.) Only this worker's own
      // entry is compacted -- the seed cache is shared.
      if (std::vector<Src> *own = sources_cache.find(it.p.str()))
        DropInertSources(it.p.str(), *own, children);
    }
    return true;
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::MakeChildWarmTask(
      const WarmTask &parent, const std::string &name, uint32_t index,
      const std::shared_ptr<const std::vector<Path>> &protos) -> WarmTask {
    WarmTask child;
    child.p = parent.p.append_child_unchecked(name);
    child.depth = parent.depth + 1;
    child.order.reserve(parent.order.size() + 1);
    child.order = parent.order;
    child.order.push_back(index);
    child.protos = protos;
    return child;
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::WarmSubtree(WarmTask root, WarmShared *shared,
                   std::unordered_set<std::string> *seeded_protos,
                   std::string *warn, std::string *err) -> void {
    if (root.protos) {
      std::vector<std::string> ignored;
      for (const Path &proto : *root.protos) {
        if (!seeded_protos->insert(proto.str()).second) continue;
        const std::vector<Src> &srcs = SourcesForPath(proto, warn, err);
        PreseedPrototypeChildren(proto, srcs, &ignored, warn, err);
      }
    }
    std::deque<WarmTask> stack;  // back = next (pre-order); front = shallowest
    stack.push_back(std::move(root));
    std::vector<std::string> children;
    std::shared_ptr<const std::vector<Path>> child_protos;
    while (!stack.empty()) {
      if (stack.size() > 1 &&
          shared->idle.load(std::memory_order_relaxed) > 0) {
        // Hand over the shallowest half: a wide node (thousands of leaf
        // children) would otherwise trickle out one tiny task per step.
        shared->Donate(&stack, stack.size() / 2);
      }
      WarmTask it = std::move(stack.back());
      stack.pop_back();
      const bool expanded =
          ExpandWarmNode(it, shared, &children, &child_protos, warn, err);
      if (it.parent_children) {
        // The parent's last child: every sibling this worker kept has derived
        // its sources from the parent's list by now, so its inert sources can
        // go. DropInertSources leaves it whole unless every child's list is
        // cached here (a donated sibling is derived elsewhere).
        const Path parent = it.p.parent();
        if (std::vector<Src> *own = sources_cache.find(parent.str()))
          DropInertSources(parent.str(), *own, it.parent_children.get());
      }
      if (!expanded) continue;
      const bool seeded = child_protos && child_protos != it.protos;
      if (seeded) {
        seeded_protos->insert(it.p.str());  // seeded here, in this cache
      }
      for (size_t ci = children.size(); ci-- > 0;) {
        stack.push_back(MakeChildWarmTask(
            it, children[ci], static_cast<uint32_t>(ci), child_protos));
      }
      // A prototype's children were seeded from its filtered sources; leave
      // its own list as the walk will see it.
      if (warm_drop_inert_ && !seeded && !children.empty()) {
        stack[stack.size() - children.size()].parent_children =
            std::make_shared<const std::vector<std::string>>(children);
      }
    }
  }
#endif

#if defined(LIGHTUSD_ENABLE_THREAD)
auto Cache::Impl::ParallelWarmSources(const std::vector<std::string> &root_names, int nt,
                          std::string *warn, std::string *err) -> void {
    using PWClock = std::chrono::steady_clock;
    const bool prof = options.enable_timing;
    auto t_start = prof ? PWClock::now() : PWClock::time_point{};
    // Frontier size: a few roots per worker. Discovery is SERIAL (every level
    // resolves its prims on this thread), so digging deeper for a finer
    // frontier costs more than it saves now that busy workers donate subtrees
    // to idle ones (see WarmSubtree). Measured on reference scene (32 threads), the
    // next-refactor tuning nt*16 / depth 12 cut the worker phase ~1.4 s but
    // added ~2.6 s of serial discovery (it now also expands instance
    // prototypes), a net ~1 s loss.
    const size_t want = static_cast<size_t>(nt) * 4;
    const uint32_t kMaxFrontierDepth = 8;
    // Discovery applies the workers' instancing rules (ExpandWarmNode) with
    // the shared claim table, so a top-level instance prototype -- often the
    // bulk of a scene -- is split across workers instead of warmed whole by
    // one; its filtered child seeds land in the main cache, which every worker
    // reads as its seed. Items are kept in namespace order.
    WarmShared shared;
    if (batch_scope_) {
      // Earlier batches own these prototypes: no member here is the first.
      for (const std::string &key : batch_scope_->seeded_keys)
        shared.first_member.emplace(key, WarmOrder{});
    }
    std::vector<WarmTask> level;
    level.reserve(root_names.size());
    for (size_t i = 0; i < root_names.size(); ++i) {
      WarmTask task;
      task.p = Path("/" + root_names[i]);
      task.depth = 1;
      task.order.push_back(static_cast<uint32_t>(i));
      level.push_back(std::move(task));
    }
    {
      std::vector<std::string> children;
      std::shared_ptr<const std::vector<Path>> child_protos;
      for (uint32_t d = 1;
           d < kMaxFrontierDepth && level.size() < want;
           ++d) {
        std::vector<WarmTask> next;
        next.reserve(std::min(level.size() * 4, want));
        bool grew = false;
        for (WarmTask &entry : level) {
          if (!ExpandWarmNode(entry, &shared, &children, &child_protos, warn,
                              err)) {
            continue;  // nothing below composes; the serial walk resolves it
          }
          if (children.empty()) {
            next.push_back(std::move(entry));  // leaf: nothing to parallelize
            continue;
          }
          grew = true;
          for (size_t ci = 0; ci < children.size(); ++ci) {
            next.push_back(MakeChildWarmTask(entry, children[ci],
                                             static_cast<uint32_t>(ci),
                                             child_protos));
          }
        }
        level = std::move(next);
        if (!grew) break;
      }
    }
    if (level.size() < 2) return;  // not enough fan-out to parallelize
    // Source entries are borrowed through source_seed_cache rather than copied
    // into each worker. Keep worker fanout enabled for broad production scenes:
    // those are exactly the inputs where stopping at a 100k-entry threshold
    // serialized the remaining namespace and dominated cold composition.
    auto t_discover = prof ? PWClock::now() : PWClock::time_point{};

    // Snapshot the main resolution context built by the serial discovery above.
    const size_t seed_stack_count = layer_stacks.size();
    const size_t seed_pool_count = nm_pool_.size();

    // Subtree donation (see WarmSubtree) keeps workers beyond the frontier
    // size busy, so use every worker thread.
    const int W = nt;
    std::vector<std::unique_ptr<Impl>> workers;
    workers.reserve(static_cast<size_t>(W));
    std::vector<std::string> wwarn(static_cast<size_t>(W));
    std::vector<std::string> werr(static_cast<size_t>(W));
    for (int t = 0; t < W; ++t) {
      std::unique_ptr<Impl> wp(new Impl());
      wp->resolver = resolver;
      wp->options = options;
      if (wp->options.usda_parse_options.num_threads <= 0)
        wp->options.usda_parse_options.num_threads = 1;
      wp->load_rules_ = load_rules_;
      wp->reg_ = reg_;  // borrow the shared, parse-once registry
      wp->batch_scope_ = batch_scope_;  // same namespace mask
      wp->root_layer = root_layer;
      wp->root_identifier = root_identifier;
      // Seed the worker with the main's resolution context so its reference
      // resolution produces identical layer-stack identities (the crux of
      // byte-identity). Source entries are borrowed from the immutable main
      // cache; only newly resolved entries are retained in the worker delta.
      wp->layer_stacks = layer_stacks;
      wp->stack_by_id = stack_by_id;
      wp->source_seed_cache = &sources_cache;
      wp->nm_pool_ = nm_pool_;  // seed the mapping pool (map_idx < seed are shared)
      wp->warm_drop_inert_ = warm_drop_inert_;
      workers.push_back(std::move(wp));
    }

    // Workers operate on private (seeded) Impls + the thread-safe registry only;
    // the main thread holds api_mu_ throughout. Dynamic work-stealing over the
    // frontier roots, plus subtree donation (see WarmSubtree) -- frontier
    // subtrees are very uneven, so static chunks leave workers idle behind one
    // heavy subtree.
    for (WarmTask &task : level) shared.queue.push_back(std::move(task));
    WarmShared *sh = &shared;
    std::vector<std::thread> ts;
    ts.reserve(static_cast<size_t>(W));
    for (int t = 0; t < W; ++t) {
      Impl *wk = workers[static_cast<size_t>(t)].get();
      std::string *pw = &wwarn[static_cast<size_t>(t)];
      std::string *pe = &werr[static_cast<size_t>(t)];
      ts.emplace_back([wk, sh, pw, pe]() {
        std::unordered_set<std::string> seeded_protos;
        WarmTask task;
        while (sh->Pop(&task)) {
          wk->WarmSubtree(std::move(task), sh, &seeded_protos, pw, pe);
          sh->Done();
        }
        // Drop the source vectors' push_back growth slack (~28% of their
        // bytes) here, on the worker's own thread, so the freed buffers are
        // reused by its own copies instead of lingering through the merge
        // and the structure walk. (shrink_to_fit is a no-op in this
        // -fno-exceptions build.) Worker Srcs carry no spec memo.
        wk->sources_cache.for_each([](const std::string &, std::vector<Src> &v) {
          if (v.capacity() > v.size()) {
            std::vector<Src>(std::make_move_iterator(v.begin()),
                             std::make_move_iterator(v.end()))
                .swap(v);
          }
        });
      });
    }
    for (auto &t : ts) t.join();
    auto t_workers = prof ? PWClock::now() : PWClock::time_point{};
    // The structure walk registers about one instance per claim made here
    // (both skip the subtrees of later instances): size its instancing maps
    // once instead of rehashing them through ~1 M serial commits.
    prototype_of.reserve(prototype_of.size() + shared.claims);
    instance_prim_index_.reserve(instance_prim_index_.size() + shared.claims);

    size_t worker_delta_entries = 0;
    uint64_t merge_ns = 0, destroy_ns = 0;
    // Size the main cache's index once for every worker's delta (an upper
    // bound: duplicates and stack-qualified keys are not adopted) instead of
    // growing it through several rehashes across the merges below.
    {
      size_t total = sources_cache.size();
      for (int t = 0; t < W; ++t)
        total += workers[static_cast<size_t>(t)]->sources_cache.size();
      sources_cache.reserve(total);
    }
    // All source/mapping references of a worker are adopted or remapped by the
    // time MergeSources returns, so its state can be released while the next
    // worker merges. That teardown is ~1.2 s of the merge wall on full45; a
    // single helper thread keeps at most two workers alive (the same overlap
    // as the previous inline reset) but takes the free off the critical path.
    std::mutex destroy_mu;
    std::condition_variable destroy_cv;
    std::unique_ptr<Impl> destroy_slot;
    bool destroy_quit = false;
    std::thread destroyer([&destroy_mu, &destroy_cv, &destroy_slot, &destroy_quit] {
      for (;;) {
        std::unique_ptr<Impl> victim;
        {
          std::unique_lock<std::mutex> lk(destroy_mu);
          destroy_cv.wait(lk, [&] { return destroy_quit || destroy_slot != nullptr; });
          victim = std::move(destroy_slot);
          if (!victim && destroy_quit) return;
        }
        victim.reset();
      }
    });
    auto hand_off = [&](std::unique_ptr<Impl> victim) {
      {
        std::lock_guard<std::mutex> lk(destroy_mu);
        destroy_slot = std::move(victim);
      }
      destroy_cv.notify_one();
    };
    for (int t = 0; t < W; ++t) {
      worker_delta_entries += workers[static_cast<size_t>(t)]->sources_cache.size();
      const auto m0 = PWClock::now();
      MergeSources(*workers[static_cast<size_t>(t)], seed_stack_count,
                   seed_pool_count);
      const auto m1 = PWClock::now();
      if (prof) {
        merge_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(m1 - m0).count());
      }
      if (warn) *warn += wwarn[static_cast<size_t>(t)];
      if (err) *err += werr[static_cast<size_t>(t)];
      hand_off(std::move(workers[static_cast<size_t>(t)]));
      if (prof) {
        destroy_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                PWClock::now() - m1).count());
      }
    }
    {
      std::lock_guard<std::mutex> lk(destroy_mu);
      destroy_quit = true;
    }
    destroy_cv.notify_one();
    destroyer.join();
    if (prof) {
      auto ms = [](PWClock::time_point a, PWClock::time_point b) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a)
                   .count() / 1e6;
      };
      auto t_merge = PWClock::now();
      LIGHTUSD_LOG_I("[next_warm] frontier=" + UIntToStr(level.size()) +
                  " W=" + IntToStr(W) +
                  " discover=" + FormatMilliseconds(ms(t_start, t_discover)) +
                  "ms workers=" + FormatMilliseconds(ms(t_discover, t_workers)) +
                  "ms merge=" + FormatMilliseconds(ms(t_workers, t_merge)) +
                  "ms merge_loop=" + FormatMilliseconds(merge_ns / 1e6) +
                  "ms destroy=" + FormatMilliseconds(destroy_ns / 1e6) +
                  "ms cache=" + UIntToStr(sources_cache.size()) +
                  " delta=" + UIntToStr(worker_delta_entries));
    }
  }
#endif

auto Cache::Impl::PrewarmPrimIndices(const std::vector<Path> &paths, std::string *warn,
                           std::string *err) -> bool {
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Use unique_lock (not lock_guard) so we can release api_mu_ during the
    // parallel build and reacquire it for the merge.
    std::unique_lock<PcpMutex> wlk(api_mu_);
    int nt = options.num_threads;
    if (nt < 0) {
      nt = std::min<int>(static_cast<int>(std::thread::hardware_concurrency()),
                         kMaxExecutionThreads);
    }
    nt = std::min(nt, kMaxExecutionThreads);
    if (nt > 1) {
      // (a) Prefetch root-level reference/payload assets to warm the shared
      // registry (reduces redundant double-parses across workers).
      std::vector<std::pair<std::string, std::string>> assets;  // (asset, anchor)
      std::set<std::string> seen;
      // Anchor each layer's arcs to THAT layer's identifier (a sublayer's
      // relative reference resolves against the sublayer, not the stack root).
      const auto &ids = layer_stacks[0].layer_identifiers;
      for (size_t li = 0; li < layer_stacks[0].layers.size(); ++li) {
        const auto &lp = layer_stacks[0].layers[li];
        const std::string &anchor =
            li < ids.size() ? ids[li] : layer_stacks[0].identifier;
        for (const PrimSpec &ps : lp->prims()) {
          for (const std::string &r : ps.meta().references) {
            CompositionArc a = Compositor::ParseReference(r);
            if (!a.asset_path.empty() && seen.insert(a.asset_path).second)
              assets.emplace_back(a.asset_path, anchor);
          }
          for (const std::string &r : ps.meta().payloads) {
            CompositionArc a = Compositor::ParsePayload(r);
            if (!a.asset_path.empty() && seen.insert(a.asset_path).second)
              assets.emplace_back(a.asset_path, anchor);
          }
        }
      }
      if (!assets.empty()) {
        std::atomic<size_t> next_idx{0};
        int pf = std::min<int>(nt, static_cast<int>(assets.size()));
        std::vector<std::thread> ts;
        auto fn = [&]() {
          for (;;) {
            size_t i = next_idx.fetch_add(1);
            if (i >= assets.size()) break;
            std::string w, e;
            LayerLoadOptions load_opts;
            load_opts.max_memory = options.max_layer_memory;
            load_opts.max_array_elements = options.max_array_elements;
            load_opts.usdc_limits = options.usdc_limits;
            load_opts.max_archive_entries = options.max_archive_entries;
            load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
            load_opts.usdc_use_mmap = options.usdc_use_mmap;
            load_opts.usda_parse_options = options.usda_parse_options;
            load_opts.parse_num_threads = options.usda_parse_options.num_threads;
            if (load_opts.parse_num_threads <= 0) {
              load_opts.parse_num_threads = 1;
              load_opts.usda_parse_options.num_threads = 1;
            }
            load_opts.strict_aousd_conformance =
                options.strict_aousd_conformance;
            reg_->GetOrLoad(*resolver, assets[i].first, assets[i].second, &w,
                            &e, load_opts);
          }
        };
        ts.reserve(static_cast<size_t>(pf));
        for (int t = 0; t < pf; ++t) ts.emplace_back(fn);
        for (auto &t : ts) t.join();
      }

      // (b) Parallel per-prim index build into private worker Impls, then a
      // deterministic input-order merge.
      if (paths.size() > 1) {
        const int W = std::min<int>(nt, static_cast<int>(paths.size()));
        auto chunk_start = [&](int t) -> size_t {
          return static_cast<size_t>(t) * paths.size() / static_cast<size_t>(W);
        };

        std::vector<std::unique_ptr<Impl>> workers;
        workers.reserve(static_cast<size_t>(W));
        std::vector<std::string> wwarn(static_cast<size_t>(W));
        std::vector<std::string> werr(static_cast<size_t>(W));
        for (int t = 0; t < W; ++t) {
          std::unique_ptr<Impl> wp(new Impl());
          wp->resolver = resolver;
          wp->options = options;
          if (wp->options.usda_parse_options.num_threads <= 0)
            wp->options.usda_parse_options.num_threads = 1;
          wp->load_rules_ = load_rules_;
          wp->reg_ = reg_;  // borrow the shared, parse-once registry
          wp->batch_scope_ = batch_scope_;  // same namespace mask
          wp->root_layer = root_layer;
          wp->root_identifier = root_identifier;
          wp->defer_instances_ = true;
          std::string sw, se;
          wp->InternLayerStack(root_layer, root_identifier, &sw, &se);  // root @0
          workers.push_back(std::move(wp));
        }

        // Release api_mu_ during the parallel build so that concurrent API
        // calls (ComputePrimIndex, SourcesForPath, etc.) are not blocked.
        // Workers operate on private Impls and only borrow the thread-safe
        // shared registry; they never touch shared cache state.
        wlk.unlock();

        std::vector<std::thread> bts;
        bts.reserve(static_cast<size_t>(W));
        for (int t = 0; t < W; ++t) {
          Impl *wk = workers[static_cast<size_t>(t)].get();
          size_t s = chunk_start(t), e = chunk_start(t + 1);
          std::string *pw = &wwarn[static_cast<size_t>(t)];
          std::string *pe = &werr[static_cast<size_t>(t)];
          bts.emplace_back([wk, s, e, &paths, pw, pe]() {
            for (size_t i = s; i < e; ++i) {
              wk->ComputePrimIndex(paths[i], pw, pe);
            }
          });
        }
        for (auto &t : bts) t.join();

        // Reacquire api_mu_ for the serial merge into shared cache state.
        wlk.lock();

        // Merge in input order (chunks are contiguous and ascending).
        for (int t = 0; t < W; ++t) {
          size_t s = chunk_start(t), e = chunk_start(t + 1);
          for (size_t i = s; i < e; ++i) {
            MergeWorkerIndex(*workers[static_cast<size_t>(t)], paths[i].str());
          }
        }
        for (int t = 0; t < W; ++t) {
          if (warn) *warn += wwarn[static_cast<size_t>(t)];
          if (err) *err += werr[static_cast<size_t>(t)];
          Impl &wk = *workers[static_cast<size_t>(t)];
          for (const std::string &dp : wk.deferred_payload_prims) {
            deferred_payload_prims.insert(dp);
          }
          // Fold per-worker typed diagnostics into the main Impl (workers each
          // own a private issues_, so this join is race-free).
          for (auto &iss : wk.issues_) issues_.push_back(std::move(iss));
        }
        return true;
      }
    }
#else
    {
      NEXT_PCP_LOCK(api_mu_);
      for (const Path &p : paths) ComputePrimIndex_locked(p, warn, err);
    }
#endif
#if defined(LIGHTUSD_ENABLE_THREAD)
    // The thread-enabled serial fallback reaches here with `wlk` already
    // holding api_mu_. Do not take NEXT_PCP_LOCK again: the default mutex is
    // intentionally non-recursive.
    for (const Path &p : paths) ComputePrimIndex_locked(p, warn, err);
#endif
    return true;
  }

auto Cache::Impl::ComputeInstanceKey(const Path &path, std::string *warn,
                                 std::string *err) -> std::string {
    NEXT_PCP_LOCK(api_mu_);
    const std::vector<Src> &srcs = SourcesForPath(path, warn, err);
    return ComputeInstanceKeyImpl(srcs);
  }

auto Cache::Impl::DropIndex(const std::string &key) -> void {
    auto it = index_to_sites.find(key);
    if (it != index_to_sites.end()) {
      for (const Site &s : it->second) {
        auto sit = site_to_indices.find(s);
        if (sit != site_to_indices.end()) sit->second.erase(key);
      }
      index_to_sites.erase(it);
    }
    index_cache.erase(key);
    sources_cache.erase(key);
    composed_cache_.erase(key);
    composed_children_.erase(key);
    DropInstancing(key);
  }

auto Cache::Impl::Invalidate(const Path &prim_path) -> void {
    NEXT_PCP_LOCK(api_mu_);
    Invalidate_locked(prim_path);
  }

auto Cache::Impl::Invalidate_locked(const Path &prim_path) -> void {
    const std::string base = prim_path.str();
    std::set<std::string> to_drop;

    // index_cache is a std::map (sorted). Use lower_bound to scan only entries
    // at/under base instead of the entire map (Fix #12).
    for (auto it = index_cache.lower_bound(base);
         it != index_cache.end() && IsPathAtOrUnder(it->first, base); ++it) {
      to_drop.insert(it->first);
    }
    for (const auto &kv : site_to_indices) {
      if (IsPathAtOrUnder(kv.first.prim_path, base)) {
        for (const std::string &dep : kv.second) to_drop.insert(dep);
      }
    }
    for (const std::string &k : to_drop) DropIndex(k);

    // sources_cache + deferred state may have descendants not in index_cache.
    // Stack-qualified intermediate keys ("@N:/path" / "!N:/path") are not prim
    // paths, so IsPathAtOrUnder never matches them; drop them all (they are
    // cheap to recompute and a stale relocate-source / arc-target expansion
    // under the invalidated subtree would otherwise survive).
    sources_cache.erase_if([&](const std::string &k) {
      const bool qualified = !k.empty() && (k[0] == '@' || k[0] == '!');
      return qualified || IsPathAtOrUnder(k, base);
    });
    for (auto it = deferred_payload_prims.begin();
         it != deferred_payload_prims.end();) {
      if (IsPathAtOrUnder(*it, base)) it = deferred_payload_prims.erase(it);
      else ++it;
    }
    // Phase 10: drop lazily-composed specs at/under the path.
    for (auto it = composed_cache_.begin(); it != composed_cache_.end();) {
      if (IsPathAtOrUnder(it->first, base)) it = composed_cache_.erase(it);
      else ++it;
    }
    for (auto it = composed_children_.begin(); it != composed_children_.end();) {
      if (IsPathAtOrUnder(it->first, base)) it = composed_children_.erase(it);
      else ++it;
    }
    // Diagnostics are a per-edit-cycle log: an invalidation begins a fresh
    // accumulation (matches GetCompositionIssues' contract; also bounds growth
    // across repeated recompositions). Issues are global, not path-keyed.
    issues_.clear();
  }

auto Cache::Impl::InvalidateLayer(const std::string &layer_id) -> void {
    NEXT_PCP_LOCK(api_mu_);
    std::set<std::string> to_drop;
    for (const auto &kv : site_to_indices) {
      if (kv.first.layer_id == layer_id) {
        for (const std::string &dep : kv.second) to_drop.insert(dep);
      }
    }
    for (const std::string &k : to_drop) DropIndex(k);
    // spec cache keys are (stack_idx, site), not prim paths; layer contents
    // changed so clear it entirely. (Src::specs_ memo pointers into it live
    // only in sources_cache entries, cleared just below.) The arc-target memo
    // caches resolved layers, so a dropped/re-parsed layer must re-resolve.
    spec_cache_by_stack_.clear();
    path_filter_by_stack_.clear();
    path_filter_bits_ = 0;
    arc_target_memo_.clear();
    // Sources and lazy composed specs can be populated without a PrimIndex, so
    // site_to_indices is not a complete dependency map for them. Be conservative
    // until lazy composition records its own layer-site dependencies.
    sources_cache.clear();
    composed_cache_.clear();
    composed_children_.clear();
    issues_.clear();  // fresh diagnostics for the recomposition.
    reg_->Drop(layer_id);
  }

auto Cache::Impl::ReloadLayer(const std::string &layer_id, std::string *warn,
                   std::string *err) -> bool {
    NEXT_PCP_LOCK(api_mu_);

    bool known_dependency = layer_id == root_identifier;
    for (const LayerStack &stack : layer_stacks) {
      for (const std::string &identifier : stack.layer_identifiers) {
        if (identifier == layer_id) {
          known_dependency = true;
          break;
        }
      }
      if (known_dependency) break;
    }
    if (!known_dependency) {
      if (err) *err += "Cannot reload a non-dependency layer: " + layer_id + "\n";
      return false;
    }

    LayerLoadOptions load_opts;
    load_opts.max_memory = options.max_layer_memory;
    load_opts.max_array_elements = options.max_array_elements;
    load_opts.usdc_limits = options.usdc_limits;
    load_opts.max_archive_entries = options.max_archive_entries;
    load_opts.usdc_lazy_arrays = options.usdc_lazy_arrays;
    load_opts.usdc_use_mmap = options.usdc_use_mmap;
    load_opts.usda_parse_options = options.usda_parse_options;
    load_opts.parse_num_threads = options.usda_parse_options.num_threads;
    load_opts.strict_aousd_conformance = options.strict_aousd_conformance;

    std::shared_ptr<Layer> replacement;
    if (!AssetResolver::GetIdentifierScheme(layer_id).empty() ||
        resolver->HasAssetReader()) {
      std::vector<uint8_t> bytes;
      if (resolver->ReadAsset(layer_id, &bytes, err)) {
        replacement = LoadLayerFromMemory(layer_id, bytes.data(), bytes.size(),
                                          warn, err, load_opts);
      }
    } else {
      // `layer_id` was previously resolved and is already part of this cache's
      // dependency set. Reload it directly: applying the untrusted resolver's
      // absolute-path rejection a second time would make safe reloads
      // impossible even though no new filesystem authority is being granted.
      replacement = LoadLayerFromFile(layer_id, warn, err, load_opts);
    }
    if (!replacement) return false;
    // Keep the previously registered dependency intact until parsing the
    // replacement succeeds. Preload atomically replaces the registry entry.
    reg_->Preload(layer_id, replacement);

    if (layer_id == root_identifier) root_layer = replacement;

    // PrimIndex and Src objects retain layer-stack indices and pointers into
    // layer specs. Rebuild the stack table as a unit so a reload also observes
    // edits to subLayerPaths, offsets, relocates, and expression variables.
    index_cache.clear();
    sources_cache.clear();
    sources_in_progress.clear();
    reloc_content_in_progress.clear();
    site_to_indices.clear();
    index_to_sites.clear();
    composed_cache_.clear();
    composed_children_.clear();
    spec_cache_by_stack_.clear();
    path_filter_by_stack_.clear();
    path_filter_bits_ = 0;
    arc_target_memo_.clear();
    deferred_payload_prims.clear();
    prototype_by_key.clear();
    instance_key_order.clear();
    prototype_of.clear();
    instances_by_prototype.clear();
    issues_.clear();
    layer_stacks.clear();
    stack_by_id.clear();

    const uint32_t root_stack =
        InternLayerStack(root_layer, root_identifier, warn, err);
    return root_stack != UINT32_MAX;
  }

auto Cache::Impl::LoadPayload(const Path &prim_path, bool with_descendants,
                   std::string *warn, std::string *err) -> bool {
    NEXT_PCP_LOCK(api_mu_);
    if (with_descendants) {
      load_rules_.LoadWithDescendants(prim_path.str());
    } else {
      load_rules_.LoadWithoutDescendants(prim_path.str());
    }
    Invalidate_locked(prim_path);
    ComputePrimIndex_locked(prim_path, warn, err);  // recompose now.
    return true;
  }

auto Cache::Impl::LoadPayloads(const std::vector<Path> &prim_paths,
                    bool with_descendants) -> bool {
    NEXT_PCP_LOCK(api_mu_);
    Path common;
    for (const Path &path : prim_paths) {
      if (path.empty()) continue;
      if (with_descendants) load_rules_.LoadWithDescendants(path.str());
      else load_rules_.LoadWithoutDescendants(path.str());
      if (common.empty()) {
        common = path;
      } else {
        while (!common.empty() &&
               !IsPathAtOrUnder(path.str(), common.str())) {
          common = common.parent();
        }
      }
    }
    if (!common.empty()) Invalidate_locked(common);
    return true;
  }

auto Cache::Impl::UnloadPayload(const Path &prim_path) -> bool {
    NEXT_PCP_LOCK(api_mu_);
    load_rules_.Unload(prim_path.str());
    Invalidate_locked(prim_path);
    // Recompose so the deferred-payload set (and HasDeferredPayload) reflects
    // the unload immediately -- Invalidate() drops the deferred entries under
    // the path, and ExpandArcs repopulates them on the next composition.
    std::string warn, err;
    ComputePrimIndex_locked(prim_path, &warn, &err);
    return true;
  }

auto Cache::Impl::SetLoadRules(const LoadRules &rules) -> void {
    NEXT_PCP_LOCK(api_mu_);
    // Idempotent call: viewer code and round-trip tests re-apply the same rule
    // set routinely, and nuking every prim index / composed spec / instancing
    // group for it forced a full stage recomposition for nothing.
    if (rules.rules() == load_rules_.rules()) return;
    load_rules_ = rules;
    // A wholesale rule change can affect any prim; drop everything (lazy
    // rebuild). Deferred state is recomputed as prims recompose.
    index_cache.clear();
    sources_cache.clear();
    site_to_indices.clear();
    index_to_sites.clear();
    deferred_payload_prims.clear();
    prototype_by_key.clear();
    prototype_of.clear();
    instances_by_prototype.clear();
    // Must be cleared with the rest of the instancing state. Leaving it grew
    // without bound across recompositions AND corrupted prototype numbering:
    // the parallel warm path picks prototypes by scanning it in REVERSE
    // (cache-parallel-warm.inc), so stale/duplicated keys changed which
    // prototype won.
    instance_key_order.clear();
    composed_cache_.clear();  // Phase 10
    composed_children_.clear();
    issues_.clear();  // fresh diagnostics for the recomposition.
  }

auto Cache::Impl::SetVariantSelections(
      const CompositionOptions::VariantSelectionMap &selections) -> void {
    NEXT_PCP_LOCK(api_mu_);
    std::set<std::string> changed;
    for (const auto &entry : options.variant_overrides_by_path) {
      const auto it = selections.find(entry.first);
      if (it == selections.end() || it->second != entry.second) {
        changed.insert(entry.first);
      }
    }
    for (const auto &entry : selections) {
      const auto it = options.variant_overrides_by_path.find(entry.first);
      if (it == options.variant_overrides_by_path.end() ||
          it->second != entry.second) {
        changed.insert(entry.first);
      }
    }
    options.variant_overrides_by_path = selections;
    for (const std::string &path : changed) Invalidate_locked(Path(path));
  }

}}} // namespace lightusd::next::pcp
