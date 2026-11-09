// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// cache.cc - implementation of the cached / lazy composition engine.
//
#include "pcp/cache.hh"

#include <algorithm>
#include <functional>
#include <set>

#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
#include <thread>
#endif

#include "composition-graph.hh"  // ValidateNoDuplicateSiblingNames
#include "composition.hh"  // CompositeSublayers, LayerToStage
#include "core/prim-spec.hh"
#include "namespace-mapping.hh"
#include "pcp/cache-impl.hh"
#include "core/security-policy.hh"
#include "stage.hh"

namespace lightusd { namespace legacy {
namespace pcp {

namespace {

// Return true if `path` equals `prefix` or is a namespace descendant of it
// ("/A" is a prefix of "/A" and "/A/B" but not of "/AB").
bool PathPrefixMatch(const std::string &path, const std::string &prefix) {
  if (path == prefix) return true;
  if (path.size() <= prefix.size()) return false;
  if (path.compare(0, prefix.size(), prefix) != 0) return false;
  return path[prefix.size()] == '/';
}

std::vector<std::string> GatherComposedChildNames(
    const cg::CompositionContext &ctx, const cg::PrimIndex &index) {
  std::vector<std::string> names;
  std::set<std::string> seen;
  for (uint16_t order_idx : index.GetStrengthOrder()) {
    const cg::CompNode &n = index.GetNode(order_idx);
    if (n.is_culled() || n.is_payload_deferred()) continue;
    if (n.layer_stack_idx == cg::CompNode::kInvalidIndex ||
        n.layer_stack_idx >= ctx._layer_stacks.size()) {
      continue;
    }
    const cg::LayerStackEntry &ls = ctx._layer_stacks[n.layer_stack_idx];
    if (!ls.layer) continue;
    if (n.site_path_idx >= ctx._path_table.size()) continue;
    const std::string &site = ctx._path_table[n.site_path_idx];
    const PrimSpec *ps = nullptr;
    std::string fe;
    if (!ls.layer->find_primspec_at(Path(site, ""), &ps, &fe) || !ps) {
      continue;
    }
    for (const auto &child : ps->children()) {
      if (seen.insert(child.name()).second) {
        names.push_back(child.name());
      }
    }
  }
  return names;
}

int ResolveThreadCount(int requested) {
#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
  if (requested < 0) {
    unsigned int hw = std::thread::hardware_concurrency();
    return (hw > 0) ? static_cast<int>(hw) : 4;
  }
  return (requested < 1) ? 1 : requested;
#else
  (void)requested;
  return 1;
#endif
}

}  // namespace

// ===========================================================================
// Cache (public, pimpl-forwarding)
// ===========================================================================

Cache::Cache() : _impl(new Impl()) {}
Cache::~Cache() = default;
Cache::Cache(Cache &&) noexcept = default;
Cache &Cache::operator=(Cache &&) noexcept = default;

nonstd::expected<Cache, std::string> Cache::Open(
    AssetResolutionResolver &resolver, const Layer &root_layer,
    const CacheOptions &options) {
  Cache c;
  c._impl->opts = options;
  c._impl->resolver = &resolver;

  auto composited = std::make_shared<Layer>();
  std::string warn, err;
  if (!CompositeSublayers(resolver, root_layer, composited.get(), &warn, &err)) {
    return nonstd::make_unexpected("pcp::Cache: CompositeSublayers failed: " +
                                   err);
  }
  c._impl->composited_root = std::move(composited);
  c._impl->root_layer = c._impl->composited_root.get();
  return c;
}

const cg::PrimIndex *Cache::ComputePrimIndex(const Path &prim_path,
                                             std::string *warn,
                                             std::string *err) {
  return _impl->ComputePrimIndex(prim_path, warn, err);
}

nonstd::expected<bool, std::string> Cache::PrewarmPrimIndices(
    const std::vector<Path> &paths, std::string *warn, std::string *err) {
  return _impl->PrewarmPrimIndices(paths, warn, err);
}

bool Cache::BuildStage(Stage *stage, std::string *warn, std::string *err) {
  return _impl->BuildStage(stage, warn, err);
}

nonstd::expected<bool, std::string> Cache::LoadPayload(const Path &prim_path,
                                                       std::string *warn,
                                                       std::string *err) {
  return _impl->LoadPayload(prim_path, warn, err);
}

nonstd::expected<bool, std::string> Cache::UnloadPayload(
    const Path &prim_path) {
  return _impl->UnloadPayload(prim_path);
}

bool Cache::HasDeferredPayload(const Path &prim_path) const {
  return _impl->HasDeferredPayload(prim_path);
}

std::vector<Path> Cache::GetDeferredPayloadPaths() const {
  return _impl->GetDeferredPayloadPaths();
}

void Cache::Invalidate(const Path &prim_path) { _impl->Invalidate(prim_path); }

void Cache::InvalidateLayer(const std::string &resolved_layer_id) {
  _impl->InvalidateLayer(resolved_layer_id);
}

bool Cache::HasComputedPrimIndex(const Path &prim_path) const {
  return _impl->index_cache.find(prim_path.prim_part()) !=
         _impl->index_cache.end();
}

std::vector<Path> Cache::GetComputedPrimPaths() const {
  std::vector<Path> result;
  result.reserve(_impl->index_cache.size());
  for (const auto &kv : _impl->index_cache) {
    result.emplace_back(kv.first, "");
  }
  return result;
}

size_t Cache::ComputedPrimIndexCount() const {
  return _impl->index_cache.size();
}

const LayerRegistry &Cache::layer_registry() const { return _impl->layers; }

const Layer *Cache::composited_root_layer() const { return _impl->root_layer; }

// ===========================================================================
// Cache::Impl
// ===========================================================================

const Layer *Cache::Impl::LoadLayerThunk(void *userdata,
                                         const std::string &asset_path,
                                         const std::string &cwp,
                                         std::string *warn, std::string *err) {
  Impl *self = static_cast<Impl *>(userdata);
  return self->layers.GetOrLoad(*self->resolver, asset_path, cwp, warn, err);
}

const PrimSpec *Cache::Impl::FindLocalPrimSpec(const Path &prim_path) const {
  if (!root_layer) return nullptr;
  const PrimSpec *ps = nullptr;
  std::string find_err;
  if (!root_layer->find_primspec_at(prim_path, &ps, &find_err)) {
    return nullptr;
  }
  return ps;
}

nonstd::expected<std::shared_ptr<CachedPrimIndex>, std::string>
Cache::Impl::BuildEntry(const Path &prim_path, std::string *warn,
                        std::string *err) {
  (void)warn;
  const PrimSpec *local = FindLocalPrimSpec(prim_path);
  if (!local) {
    return nonstd::make_unexpected("pcp::Cache: prim not found: " +
                                   prim_path.prim_part());
  }

  auto entry = std::make_shared<CachedPrimIndex>();
  entry->ctx._resolver = resolver;
  entry->ctx._options = opts.composition;
  entry->ctx._root_layer = root_layer;
  entry->ctx.load_layer_fn = &Impl::LoadLayerThunk;
  entry->ctx.load_layer_userdata = this;

  // Register the (shared) root layer in this entry's own layer-stack table.
  uint16_t root_ls =
      entry->ctx.AddLayerStack(root_layer, "<root>", LayerOffset());

  cg::PrimIndexBuilder builder(&entry->ctx, prim_path, *local, root_ls);
  auto result = builder.Build();
  if (!result) {
    if (err) *err = result.error();
    return nonstd::make_unexpected(result.error());
  }
  entry->index = std::move(*result);
  return entry;
}

const cg::PrimIndex *Cache::Impl::ComputePrimIndex(const Path &prim_path,
                                                   std::string *warn,
                                                   std::string *err) {
  const std::string key = prim_path.prim_part();
  auto it = index_cache.find(key);
  if (it != index_cache.end()) {
    return &it->second->index;
  }

  auto built = BuildEntry(prim_path, warn, err);
  if (!built) {
    if (err) *err = built.error();
    return nullptr;
  }

  std::shared_ptr<CachedPrimIndex> entry = built.value();
  index_cache[key] = entry;
  RegisterDependencies(key, *entry);
  return &entry->index;
}

nonstd::expected<bool, std::string> Cache::Impl::PrewarmPrimIndices(
    const std::vector<Path> &paths, std::string *warn, std::string *err) {
  (void)warn;  // only consumed by the parallel branch below
  (void)err;
  const int nthreads = ResolveThreadCount(opts.num_threads);

#if defined(LIGHTUSD_ENABLE_THREAD) && !defined(__EMSCRIPTEN__)
  if (nthreads > 1 && paths.size() >= opts.min_paths_for_parallel) {
    return BuildParallel(paths, static_cast<size_t>(nthreads), warn, err);
  }
#else
  (void)nthreads;
#endif

  // Single-threaded path (also the only path on wasm).
  for (const auto &p : paths) {
    std::string w, e;
    (void)ComputePrimIndex(p, &w, &e);  // best-effort; skip failures
  }
  return true;
}

// -- Dependency tracking --

void Cache::Impl::RegisterDependencies(const std::string &prim_path,
                                       const CachedPrimIndex &entry) {
  UnregisterDependencies(prim_path);  // idempotent (e.g. after payload reload)

  for (uint16_t i = 0; i < entry.index.GetNodeCount(); i++) {
    const cg::CompNode &n = entry.index.GetNode(i);
    if (n.is_culled()) continue;
    if (n.layer_stack_idx == cg::CompNode::kInvalidIndex) continue;
    if (n.layer_stack_idx >= entry.ctx._layer_stacks.size()) continue;

    Site s;
    s.layer_id = entry.ctx._layer_stacks[n.layer_stack_idx].identifier;
    if (n.site_path_idx < entry.ctx._path_table.size()) {
      s.prim_path = entry.ctx._path_table[n.site_path_idx];
    }
    site_to_indices[s].insert(prim_path);
    index_to_sites[prim_path].push_back(s);
  }
}

void Cache::Impl::UnregisterDependencies(const std::string &prim_path) {
  auto sit = index_to_sites.find(prim_path);
  if (sit == index_to_sites.end()) return;
  for (const Site &s : sit->second) {
    auto m = site_to_indices.find(s);
    if (m != site_to_indices.end()) {
      m->second.erase(prim_path);
      if (m->second.empty()) site_to_indices.erase(m);
    }
  }
  index_to_sites.erase(sit);
}

void Cache::Impl::DropIndex(const std::string &prim_path) {
  UnregisterDependencies(prim_path);
  index_cache.erase(prim_path);
}

void Cache::Impl::Invalidate(const Path &prim_path) {
  const std::string changed = prim_path.prim_part();
  std::set<std::string> victims;

  // (a) Indices that read a site at/under the changed prim path.
  for (const auto &kv : site_to_indices) {
    if (PathPrefixMatch(kv.first.prim_path, changed)) {
      victims.insert(kv.second.begin(), kv.second.end());
    }
  }
  // (b) The changed prim's own index and its namespace descendants.
  for (const auto &kv : index_cache) {
    if (PathPrefixMatch(kv.first, changed)) victims.insert(kv.first);
  }

  for (const auto &v : victims) DropIndex(v);
}

void Cache::Impl::InvalidateLayer(const std::string &resolved_layer_id) {
  std::set<std::string> victims;
  for (const auto &kv : site_to_indices) {
    if (kv.first.layer_id == resolved_layer_id) {
      victims.insert(kv.second.begin(), kv.second.end());
    }
  }
  // Drop dependent indices BEFORE dropping the layer (borrowed pointers).
  for (const auto &v : victims) DropIndex(v);
  layers.Drop(resolved_layer_id);
}

// -- Payloads --

bool Cache::Impl::HasDeferredPayload(const Path &prim_path) const {
  auto it = index_cache.find(prim_path.prim_part());
  if (it == index_cache.end()) return false;
  const cg::PrimIndex &index = it->second->index;
  for (uint16_t i = 0; i < index.GetNodeCount(); i++) {
    if (index.GetNode(i).is_payload_deferred()) return true;
  }
  return false;
}

std::vector<Path> Cache::Impl::GetDeferredPayloadPaths() const {
  std::vector<Path> result;
  for (const auto &kv : index_cache) {
    const cg::PrimIndex &index = kv.second->index;
    for (uint16_t i = 0; i < index.GetNodeCount(); i++) {
      if (index.GetNode(i).is_payload_deferred()) {
        result.emplace_back(kv.first, "");
        break;
      }
    }
  }
  return result;
}

nonstd::expected<bool, std::string> Cache::Impl::LoadPayload(
    const Path &prim_path, std::string *warn, std::string *err) {
  const std::string key = prim_path.prim_part();
  auto it = index_cache.find(key);
  if (it == index_cache.end()) {
    return nonstd::make_unexpected("pcp::Cache: PrimIndex not computed for " +
                                   key);
  }
  CachedPrimIndex &entry = *it->second;

  // Find the deferred payload descriptor for this prim.
  cg::DeferredPayloadInfo *info = nullptr;
  for (auto &dp : entry.ctx._deferred_payloads) {
    if (dp.prim_path.prim_part() == key) {
      info = &dp;
      break;
    }
  }
  if (!info) {
    return nonstd::make_unexpected("pcp::Cache: no deferred payload for " + key);
  }

  cg::CompNode &node = cg::GetMutableNode(entry.index, info->node_idx);
  if (!node.is_payload_deferred()) {
    return nonstd::make_unexpected("pcp::Cache: payload not in deferred state");
  }

  std::string asset_path = info->payload.asset_path.GetAssetPath();
  uint16_t ls_idx = cg::CompNode::kInvalidIndex;
  Path target;

  if (asset_path.empty()) {
    // Internal payload: the target lives in the root layer stack.
    target = info->payload.prim_path;
    const PrimSpec *target_ps = nullptr;
    std::string find_err;
    if (!target.is_valid() ||
        !root_layer->find_primspec_at(target, &target_ps, &find_err) ||
        !target_ps) {
      return nonstd::make_unexpected(
          "pcp::Cache: internal payload target not found: " +
          target.prim_part());
    }
    // Reuse the owning node's layer stack (the root layer stack).
    ls_idx = (node.parent != cg::CompNode::kInvalidIndex)
                 ? entry.index.GetNode(node.parent).layer_stack_idx
                 : 0;
  } else {
    if (!security_policy::ValidateAndNormalizeAssetPath(asset_path,
                                                        &asset_path)) {
      return nonstd::make_unexpected("pcp::Cache: unsafe payload asset path");
    }
    const Layer *layer = layers.GetOrLoad(*resolver, asset_path,
                                          info->current_working_path, warn, err);
    if (!layer) {
      return nonstd::make_unexpected("pcp::Cache: failed to load payload: " +
                                     asset_path);
    }
    // Resolve the target prim (defaultPrim fallback).
    target = info->payload.prim_path;
    if (!target.is_valid() || target.prim_part().empty()) {
      std::string dp = layer->metas().defaultPrim.str();
      if (!dp.empty()) target = Path("/" + dp, "");
    }
    const PrimSpec *target_ps = nullptr;
    std::string find_err;
    if (!target.is_valid() ||
        !layer->find_primspec_at(target, &target_ps, &find_err) || !target_ps) {
      return nonstd::make_unexpected(
          "pcp::Cache: payload target prim not found in " + asset_path);
    }
    ls_idx = entry.ctx.AddLayerStack(layer, asset_path, info->payload.layerOffset);
  }

  node.layer_stack_idx = ls_idx;
  if (target.is_valid() && !target.prim_part().empty()) {
    NamespaceMapping mapping =
        MakeReferenceMapping(target, prim_path, asset_path.empty());
    node.site_path_idx = entry.ctx.InternPath(target.prim_part());
    node.map_expr_idx = entry.ctx.AddMapExpression(mapping, -1);
  }
  node.flags = (node.flags & ~cg::NodeFlags::PayloadDeferred &
                ~cg::NodeFlags::Inert & ~cg::NodeFlags::Culled) |
               cg::NodeFlags::PayloadLoaded | cg::NodeFlags::HasSpecs;

  cg::PrimIndexBuilder reprocessor(&entry.ctx, prim_path);
  std::string reprocess_err;
  if (!reprocessor.ReprocessNode(&entry.index, info->node_idx,
                                 &reprocess_err)) {
    return nonstd::make_unexpected("pcp::Cache: failed to reprocess payload "
                                   "node after load: " +
                                   reprocess_err);
  }

  cg::RecomputeStrengthOrder(entry.index);
  RegisterDependencies(key, entry);  // node now contributes a site
  return true;
}

nonstd::expected<bool, std::string> Cache::Impl::UnloadPayload(
    const Path &prim_path) {
  const std::string key = prim_path.prim_part();
  auto it = index_cache.find(key);
  if (it == index_cache.end()) {
    return nonstd::make_unexpected("pcp::Cache: PrimIndex not computed for " +
                                   key);
  }
  CachedPrimIndex &entry = *it->second;

  bool found = false;
  for (uint16_t i = 0; i < entry.index.GetNodeCount(); i++) {
    cg::CompNode &n = cg::GetMutableNode(entry.index, i);
    if (n.arc_type == cg::ArcType::Payload && n.is_payload_loaded()) {
      n.flags = (n.flags & ~cg::NodeFlags::PayloadLoaded &
                 ~cg::NodeFlags::HasSpecs) |
                cg::NodeFlags::PayloadDeferred;
      n.layer_stack_idx = cg::CompNode::kInvalidIndex;
      found = true;
    }
  }
  if (!found) {
    return nonstd::make_unexpected("pcp::Cache: no loaded payloads for " + key);
  }

  cg::RecomputeStrengthOrder(entry.index);
  RegisterDependencies(key, entry);
  return true;
}

// -- BuildStage --

void Cache::Impl::GatherAllPrimPaths(std::vector<Path> *out) const {
  if (!root_layer) return;

  struct Item {
    std::string path;
    const PrimSpec *ps;
  };
  std::vector<Item> stack;
  for (const auto &pair : root_layer->primspecs()) {
    stack.push_back({"/" + pair.first, &pair.second});
  }
  while (!stack.empty()) {
    Item item = std::move(stack.back());
    stack.pop_back();
    out->emplace_back(item.path, "");
    for (const auto &child : item.ps->children()) {
      stack.push_back({item.path + "/" + child.name(), &child});
    }
  }
}

bool Cache::Impl::BuildStage(Stage *stage, std::string *warn,
                             std::string *err) {
  if (!stage) {
    if (err) *err = "pcp::Cache::BuildStage: stage is nullptr";
    return false;
  }

  // 1) Compute every prim's PrimIndex (honoring num_threads).
  std::vector<Path> all_paths;
  GatherAllPrimPaths(&all_paths);
  (void)PrewarmPrimIndices(all_paths, warn, err);

  // 2) Compose PrimSpecs from the cached indices into a single Layer, then
  //    reuse the existing LayerToStage reconstruct pipeline.
  Layer composed_layer;
  if (root_layer) {
    composed_layer.metas() = root_layer->metas();
  }

  struct StageIndexEntry {
    cg::CompositionContext *ctx{nullptr};
    const cg::PrimIndex *index{nullptr};
    std::shared_ptr<CachedPrimIndex> cached_owner;
    std::shared_ptr<cg::PrimIndex> temp_owner;
  };

  HashMap<std::string, StageIndexEntry> stage_indices;
  HashMap<std::string, std::vector<std::string>> children_of;
  std::vector<std::string> roots;

  // Start from root prims in the root layer, then expand each composed
  // namespace by descending the parent PrimIndex. This mirrors
  // CompositionGraph::BuildPrimIndex and reconstructs descendants introduced by
  // references/payloads without changing lazy ComputePrimIndex() semantics.
  if (root_layer) {
    for (const auto &pair : root_layer->primspecs()) {
      const std::string root_path = "/" + pair.first;
      auto eit = index_cache.find(root_path);
      if (eit == index_cache.end()) continue;
      roots.push_back(root_path);
      StageIndexEntry entry;
      entry.ctx = &eit->second->ctx;
      entry.index = &eit->second->index;
      entry.cached_owner = eit->second;
      stage_indices[root_path] = entry;
    }
  }

  std::vector<std::string> stack = roots;
  while (!stack.empty()) {
    const std::string parent_path = std::move(stack.back());
    stack.pop_back();

    auto pit = stage_indices.find(parent_path);
    if (pit == stage_indices.end() || !pit->second.ctx ||
        !pit->second.index) {
      continue;
    }

    const std::vector<std::string> child_names =
        GatherComposedChildNames(*pit->second.ctx, *pit->second.index);
    for (auto it = child_names.rbegin(); it != child_names.rend(); ++it) {
      const std::string child_path = parent_path + "/" + *it;
      children_of[parent_path].push_back(child_path);
      if (stage_indices.find(child_path) == stage_indices.end()) {
        cg::PrimIndexBuilder builder(pit->second.ctx, Path(child_path, ""));
        auto built = builder.BuildChildFrom(*pit->second.index, *it);
        if (!built) {
          continue;
        }
        StageIndexEntry child_entry;
        child_entry.ctx = pit->second.ctx;
        child_entry.temp_owner =
            std::make_shared<cg::PrimIndex>(std::move(*built));
        child_entry.index = child_entry.temp_owner.get();
        child_entry.cached_owner = pit->second.cached_owner;
        stage_indices[child_path] = child_entry;
      }
      stack.push_back(child_path);
    }
  }

  std::function<void(const std::string &, PrimSpec &)> compose_children =
      [&](const std::string &parent_path, PrimSpec &parent_ps) {
        auto cit = children_of.find(parent_path);
        if (cit == children_of.end()) return;
        for (const std::string &cpath : cit->second) {
          auto eit = stage_indices.find(cpath);
          if (eit == stage_indices.end() || !eit->second.ctx ||
              !eit->second.index) {
            continue;
          }
          PrimSpec child_ps;
          if (cg::ComposePrimSpecFromIndex(eit->second.ctx->_layer_stacks,
                                           eit->second.ctx->_path_table,
                                           *eit->second.index,
                                           &child_ps, warn, err)) {
            compose_children(cpath, child_ps);
            parent_ps.children().push_back(std::move(child_ps));
          }
        }
      };

  for (const std::string &path_str : roots) {
    auto eit = stage_indices.find(path_str);
    if (eit == stage_indices.end() || !eit->second.ctx ||
        !eit->second.index) {
      continue;
    }
    PrimSpec composed_ps;
    if (!cg::ComposePrimSpecFromIndex(eit->second.ctx->_layer_stacks,
                                      eit->second.ctx->_path_table,
                                      *eit->second.index,
                                      &composed_ps, warn, err)) {
      continue;
    }
    compose_children(path_str, composed_ps);
    composed_layer.add_primspec(composed_ps.name(), composed_ps);
  }

  if (!cg::ValidateNoDuplicateSiblingNames(composed_layer, warn, err)) {
    return false;
  }
  return LayerToStage(std::move(composed_layer), stage, warn, err);
}

}  // namespace pcp
}  // namespace legacy
}  // namespace lightusd
