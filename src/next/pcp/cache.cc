// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "cache-internal.hh"

#include "../phase-faults.hh"
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
  if (ok) impl_->registry.RetainImportedAssets(stage->GetRootLayer());
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
  return it != impl_->prototype_of.end() && it->second != p.str();
}
Path Cache::GetPrototype(const Path &p) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  auto it = impl_->prototype_of.find(p.str());
  return it != impl_->prototype_of.end() ? Path(it->second) : Path();
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
      if (it != impl_->prototype_of.end() && it->second != p.str()) {
        a = p.str();
        break;
      }
      if (p.is_root()) break;
    }
    if (a.empty()) break;  // no enclosing instance remains
    // `a` is a prefix of `cur` on a '/' boundary (or equal); splice its
    // prototype root in for the instance-space prefix.
    cur = impl_->prototype_of.at(a) + cur.substr(a.size());
    translated = true;
  }
  return translated ? Path(cur) : Path();
}
Path Cache::TranslatePathFromPrototype(const Path &proto_path,
                                       const Path &instance_root) const {
  NEXT_PCP_READ_LOCK(impl_->api_mu_);
  auto it = impl_->prototype_of.find(instance_root.str());
  if (it == impl_->prototype_of.end() || it->second == instance_root.str()) {
    return Path();  // instance_root is not an instance
  }
  const std::string &proto = it->second;
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
  cache = Cache();  // synchronous reclamation belongs to load/compose latency
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
        if (pit != prototype_of.end() && pit->second != w.out_path.str()) {
          spec.meta().instance_prototype() = pit->second;
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
      if (registered) instance_prim_index_[here.str()] = idx;
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

auto Cache::Impl::BuildStage(Stage *stage, std::string *warn, std::string *err,
                  const Cache::PreviewCallback &preview_callback) -> bool {
    NEXT_PCP_LOCK(api_mu_);
    auto out = std::unique_ptr<Layer>(new Layer());

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

    const Layer *root = layer_stacks[0].layers[0].get();
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
    if (warm_threads > 1) StartArcLayerPrefetch(&prefetch, warm_threads);
    if (warm_threads > 1)
      ParallelWarmSources(root_names, warm_threads, warn, err);
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
    fill_.clear();
    for (const std::string &nm : root_names) {
      BuildStage(out.get(), {Path("/" + nm), Path("/" + nm), 0, true, 1},
                 warn, err);
    }
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Every layer the composed namespace needs is loaded by now; anything the
    // prefetch still has queued was not reached (e.g. arcs on inactive prims).
    StopArcLayerPrefetch(&prefetch, warn);
#endif
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
    if (options.enable_timing) LIGHTUSD_LOG_I("[next_fill] begin=1");
    {
      // Namespace resolution is complete. fill_ borrows the source vectors,
      // not their lookup keys: keep those vectors address-stable while freeing
      // the large path-key/hash index before it overlaps the finished stage.
      // Prime every spec memo before discarding its lookup index. The fill
      // reads only these resolved vectors; keeping their deque storage in a
      // separate owner lets the large path keys and hash tables die first.
      // Declare this owner before fill_sources so source memos die first too.
      std::vector<std::deque<std::vector<SpecRef>>> fill_specs;
      std::deque<std::vector<Src>> fill_sources;
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
        std::sort(compact_targets.begin(), compact_targets.end());
        compact_targets.erase(
            std::unique(compact_targets.begin(), compact_targets.end()),
            compact_targets.end());
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
        int compact_threads = options.num_threads;
        if (compact_threads < 0)
          compact_threads = static_cast<int>(std::thread::hardware_concurrency());
        compact_threads = std::max(1, std::min(compact_threads, kMaxExecutionThreads));
#if defined(LIGHTUSD_ENABLE_THREAD)
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
        if (options.enable_timing) {
          const double compact_ms =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - compact_start).count();
          LIGHTUSD_LOG_I("[next_fill] discarded_sources=" + UIntToStr(discarded) +
              " source_vector_bytes_released=" + UIntToStr(vector_bytes_released) +
              " compact=" + FormatMilliseconds(compact_ms) + "ms");
        }
      }
      if (options.enable_timing) LIGHTUSD_LOG_I("[next_fill] lookup_released=1");
      FillOpinions(out.get());
      std::vector<std::pair<uint32_t, const std::vector<Src>*>>().swap(fill_);
      // Drop any newly resolved entries as before. The transferred vectors
      // (including shared ones that FillOpinions could not drain) die here.
      if (options.release_build_sources) sources_cache.clear();
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
    stage->SetRootLayer(std::move(*out));
    return true;
  }

// Prim index lookup for the flatten passes: the walk-recorded instance
// indices first, a full path->index map built once on the first miss.
namespace {
struct InstanceIndexLookup {
  const Layer *out;
  const std::unordered_map<std::string, uint32_t> &recorded;
  std::unordered_map<std::string, uint32_t> full;
  bool full_built = false;
  InstanceIndexLookup(const Layer *o,
                      const std::unordered_map<std::string, uint32_t> &r)
      : out(o), recorded(r) {}
  // Returns UINT32_MAX when the path has no prim.
  uint32_t find(const std::string &path) {
    auto it = recorded.find(path);
    if (it != recorded.end()) return it->second;
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

auto Cache::Impl::FlattenInstances(Layer *out) -> void {
    if (instances_by_prototype.empty()) return;

    InstanceIndexLookup idx_by_path(out, instance_prim_index_);

    for (const auto &grp : instances_by_prototype) {
      const std::string &proto_path = grp.first;
      const std::vector<std::string> &members = grp.second;
      // A lone instanceable prim with no peers still composed its own subtree as
      // the prototype; leaving it untouched is already a valid flatten.
      bool has_instance = false;
      for (const std::string &m : members)
        if (m != proto_path) { has_instance = true; break; }
      if (!has_instance) continue;

      const uint32_t proto_idx = idx_by_path.find(proto_path);
      if (proto_idx == UINT32_MAX) continue;
      // Holder: keep the composed subtree, drop the instanceable flag so it is a
      // plain reference target (an instanceable holder would hide its content).
      if (PrimSpec *proto = out->prim(proto_idx)) {
        proto->meta().instanceable = false;
        proto->meta().instance_prototype().clear();
      }

      const std::string ref = "<" + proto_path + ">";
      for (const std::string &m : members) {
        if (m == proto_path) continue;
        const uint32_t m_idx = idx_by_path.find(m);
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
    }
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
      if (!shared->Claim(ComputeInstanceKeyImpl(srcs), it.order))
        return false;  // a later instance: the serial walk skips its subtree
      if (PreseedPrototypeChildren(it.p, srcs, children, warn, err)) {
        auto protos = std::make_shared<std::vector<Path>>();
        if (it.protos) *protos = *it.protos;
        protos->push_back(it.p);
        *child_protos = std::move(protos);
        return true;
      }
    }
    *children = ComposeChildNames(srcs);
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
      if (!ExpandWarmNode(it, shared, &children, &child_protos, warn, err)) {
        continue;
      }
      if (child_protos && child_protos != it.protos) {
        seeded_protos->insert(it.p.str());  // seeded here, in this cache
      }
      for (size_t ci = children.size(); ci-- > 0;) {
        stack.push_back(MakeChildWarmTask(
            it, children[ci], static_cast<uint32_t>(ci), child_protos));
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
    // to idle ones (see WarmSubtree). Measured on Island (32 threads), the
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
      });
    }
    for (auto &t : ts) t.join();
    auto t_workers = prof ? PWClock::now() : PWClock::time_point{};

    size_t worker_delta_entries = 0;
    uint64_t merge_ns = 0, destroy_ns = 0;
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
      // All source/mapping references have been adopted or remapped above.
      // Drop this worker before merging the next, reducing overlapping state.
      workers[static_cast<size_t>(t)].reset();
      if (prof) {
        destroy_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                PWClock::now() - m1).count());
      }
    }
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
