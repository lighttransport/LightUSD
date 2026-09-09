// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "cache-internal.hh"
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

nonstd::expected<Cache, std::string> Cache::Open(
    AssetResolver &resolver, std::shared_ptr<Layer> root_layer,
    const std::string &root_identifier, const CompositionOptions &options) {
  if (!root_layer) {
    return nonstd::make_unexpected(std::string("pcp::Cache::Open: null root layer"));
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
    return nonstd::make_unexpected(err.empty()
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
  return impl_->BuildStage(stage, warn, err, preview_callback);
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
  std::unordered_set<const Layer *> layers;
  for (const LayerStack &stack : impl_->layer_stacks) {
    for (const std::shared_ptr<Layer> &layer : stack.layers) {
      if (layer && layers.insert(layer.get()).second) {
        stats.source_layer_bytes += layer->memory_usage();
      }
    }
    stats.transient_cache_bytes += stack.identifier.capacity();
    stats.transient_cache_bytes +=
        stack.layers.capacity() * sizeof(stack.layers[0]);
    stats.transient_cache_bytes +=
        stack.layer_identifiers.capacity() * sizeof(stack.layer_identifiers[0]);
    for (const std::string &id : stack.layer_identifiers) {
      stats.transient_cache_bytes += id.capacity();
    }
    stats.transient_cache_bytes +=
        stack.layer_offsets.capacity() * sizeof(stack.layer_offsets[0]);
  }
  stats.layer_count = layers.size();
  stats.prim_index_count = impl_->index_cache.size();
  stats.composed_prim_count = impl_->composed_cache_.size();

  for (const std::string &path : impl_->path_table) {
    stats.transient_cache_bytes += sizeof(path) + path.capacity();
  }
  for (const auto &entry : impl_->index_cache) {
    stats.transient_cache_bytes += sizeof(entry) + entry.first.capacity();
    if (!entry.second) continue;
    const std::vector<CompNode> &nodes = entry.second->GetNodes();
    stats.transient_cache_bytes += nodes.capacity() * sizeof(CompNode);
    for (const CompNode &node : nodes) {
      stats.transient_cache_bytes +=
          node.children.capacity() * sizeof(node.children[0]);
    }
    stats.transient_cache_bytes +=
        entry.second->GetStrengthOrder().capacity() * sizeof(uint16_t);
  }
  impl_->sources_cache.for_each([&](const std::string &key,
                                    std::vector<Src> &srcs) {
    stats.transient_cache_bytes += key.capacity() + sizeof(std::vector<Src>);
    stats.transient_cache_bytes += srcs.capacity() * sizeof(Src);
    for (const Src &source : srcs) {
      stats.transient_cache_bytes += source.site.capacity();
    }
  });
  for (const auto &entry : impl_->composed_cache_) {
    stats.transient_cache_bytes += sizeof(entry) + entry.first.capacity();
    if (entry.second) stats.transient_cache_bytes += entry.second->memory_usage();
  }
  for (const auto &entry : impl_->composed_children_) {
    stats.transient_cache_bytes += sizeof(entry) + entry.first.capacity();
    stats.transient_cache_bytes +=
        entry.second.capacity() * sizeof(entry.second[0]);
    for (const std::string &child : entry.second) {
      stats.transient_cache_bytes += child.capacity();
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
                           std::string *err) {
  if (!root_layer || !out_stage) return false;
  const bool timing = options.enable_timing;
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };
  const auto t0 = Clock::now();
  auto opened = Cache::Open(resolver, std::move(root_layer), root_identifier,
                            options);
  if (!opened) {
    if (err) *err += opened.error() + "\n";
    return false;
  }
  Cache cache = std::move(*opened);
  const auto t1 = Clock::now();
  bool ok = cache.BuildStage(out_stage, warn, err);
  const auto t2 = Clock::now();
  if (timing) {
    LIGHTUSD_LOG_I("[next_compose] open=" + FormatMilliseconds(ms(t1 - t0)) +
                "ms build_stage=" + FormatMilliseconds(ms(t2 - t1)) + "ms");
  }
  return ok;
}

bool ComposeStageFromFile(const std::string &filename, AssetResolver &resolver,
                          Stage *out_stage, const CompositionOptions &options,
                          std::string *warn, std::string *err) {
  LayerLoadOptions lopts;
  lopts.max_memory = options.max_layer_memory;
  lopts.usdc_lazy_arrays = options.usdc_lazy_arrays;
  lopts.usdc_use_mmap = options.usdc_use_mmap;
  lopts.strict_aousd_conformance = options.strict_aousd_conformance;
  lopts.usda_parse_options = options.usda_parse_options;
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
