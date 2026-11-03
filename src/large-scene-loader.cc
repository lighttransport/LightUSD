// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// large-scene-loader.cc - see large-scene-loader.hh.
//
// Two backends selected at compile time by LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE.

#include "large-scene-loader.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>

#include "asset-resolution.hh"
#include "composition.hh"
#include "composition-graph.hh"
#include "io-util.hh"
#include "layer.hh"
#include "pcp/layer-registry.hh"
#include "lightusd.hh"
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
#include "next/layer/asset-anchor.hh"
#include "next/types/value-view.hh"
#endif

namespace lightusd {

namespace {

// Cheap file size without reading the content (used by the Budget policy).
// Returns 0 on failure (treated as "free").
uint64_t FileSizeBytes(const std::string &path) {
  if (path.empty()) return 0;
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) return 0;
  const std::streamoff pos = ifs.tellg();
  return (pos > 0) ? static_cast<uint64_t>(pos) : 0;
}

uint64_t BudgetMiBToBytes(size_t mib) {
  constexpr uint64_t kBytesPerMiB = 1024ull * 1024ull;
  const uint64_t maxMiB =
      (std::numeric_limits<uint64_t>::max)() / kBytesPerMiB;
  return (static_cast<uint64_t>(mib) > maxMiB)
             ? (std::numeric_limits<uint64_t>::max)()
             : static_cast<uint64_t>(mib) * kBytesPerMiB;
}

}  // namespace

// =========================================================================
// Old backend (CompositionGraph, enabled by default)
// =========================================================================
#if !defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)

namespace {

// Shared state for PayloadMode::Budget. Kept alive by the closure captured into
// CompositionGraphOptions::payload_policy_with_prim (owned by the graph).
struct BudgetPolicyState {
  uint64_t loaded_bytes{0};
  double loaded_extent_coverage{0.0};
  uint64_t budget_bytes{0};
  double extent_budget{0.0};
  AssetResolutionResolver *resolver{nullptr};
  std::mutex mu;  // serializes resolver re-entrancy (future threaded Compose)
};

bool ExtentCoverage(const PrimSpec &owner, double *coverage) {
  if (!coverage) return false;
  const auto it = owner.props().find("extentsHint");
  if (it == owner.props().end() || !it->second.is_attribute()) return false;

  const Attribute *attr = it->second.get_attribute_or_null();
  if (!attr || !attr->has_value()) return false;
  const auto points = attr->get_value<std::vector<value::float3>>();
  if (!points || points->size() < 2 || (points->size() % 2) != 0) return false;

  double lo[3] = {std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity()};
  double hi[3] = {-std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity()};
  for (const value::float3 &p : *points) {
    for (size_t axis = 0; axis < 3; ++axis) {
      const double v = static_cast<double>(p[axis]);
      if (!std::isfinite(v)) return false;
      lo[axis] = std::min(lo[axis], v);
      hi[axis] = std::max(hi[axis], v);
    }
  }

  const double dx = std::max(0.0, hi[0] - lo[0]);
  const double dy = std::max(0.0, hi[1] - lo[1]);
  const double dz = std::max(0.0, hi[2] - lo[2]);
  *coverage = std::max({dx * dy, dx * dz, dy * dz});
  return std::isfinite(*coverage);
}

std::function<bool(const Path &, const Payload &, const PrimSpec &)>
MakePayloadPolicy(
    const LargeSceneLoadOptions &opts, AssetResolutionResolver *resolver) {
  switch (opts.payload_mode) {
    case LargeSceneLoadOptions::PayloadMode::LoadAll:
      return nullptr;  // null => eager load (CompositionGraph default).
    case LargeSceneLoadOptions::PayloadMode::LoadNone:
      return [](const Path &, const Payload &, const PrimSpec &) {
        return false;
      };
    case LargeSceneLoadOptions::PayloadMode::Budget: {
      auto st = std::make_shared<BudgetPolicyState>();
      st->budget_bytes = BudgetMiBToBytes(opts.payload_budget_mb);
      st->extent_budget = opts.payload_extent_budget;
      st->resolver = resolver;
      return [st](const Path &, const Payload &pl,
                  const PrimSpec &owner) -> bool {
        std::lock_guard<std::mutex> lk(st->mu);

        const std::string old_cwp = st->resolver->current_working_path();
        if (!owner.get_current_working_path().empty()) {
          st->resolver->set_current_working_path(
              owner.get_current_working_path());
        }
        const std::string rp =
            st->resolver->resolve(pl.asset_path.GetAssetPath());
        st->resolver->set_current_working_path(old_cwp);
        const uint64_t sz = FileSizeBytes(rp);
        if (sz == 0 || st->loaded_bytes > st->budget_bytes ||
            sz > st->budget_bytes - st->loaded_bytes) {
          return false;  // over budget => defer
        }

        double extent_cost = 0.0;
        const bool has_extent = st->extent_budget > 0.0 &&
                                ExtentCoverage(owner, &extent_cost);
        if (has_extent &&
            (st->loaded_extent_coverage > st->extent_budget ||
             extent_cost >
                 st->extent_budget - st->loaded_extent_coverage)) {
          return false;
        }

        st->loaded_bytes += sz;
        if (has_extent) st->loaded_extent_coverage += extent_cost;
        return true;
      };
    }
  }
  return [](const Path &, const Payload &, const PrimSpec &) { return false; };
}

}  // namespace

LargeSceneLoader::LargeSceneLoader() = default;
LargeSceneLoader::~LargeSceneLoader() = default;

const Layer *LargeSceneLoader::LoadLayerThunk(
    void *userdata, const std::string &asset_path, const std::string &cwp,
    std::string *warn, std::string *err) {
  auto *self = static_cast<LargeSceneLoader *>(userdata);
  if (!self || !self->_registry || !self->_resolver) return nullptr;
  return self->_registry->GetOrLoad(*self->_resolver, asset_path, cwp, warn,
                                    err);
}

bool LargeSceneLoader::Load(const std::string &filename,
                            const LargeSceneLoadOptions &options,
                            std::string *warn, std::string *err) {
  std::lock_guard<std::mutex> lock(_mutex);
  _loaded = false;

  // 1. Resolver anchored at the scene's base directory.
  _resolver = std::make_unique<AssetResolutionResolver>();
  const std::string base_dir = io::GetBaseDir(filename);
  _resolver->set_current_working_path(base_dir.empty() ? "./" : base_dir);
  std::vector<std::string> search = {base_dir.empty() ? "./" : base_dir};
  for (const auto &p : options.search_paths) search.push_back(p);
  _resolver->set_search_paths(search);
  _resolver->set_max_asset_bytes_in_mb(options.max_asset_bytes_mb);
  _resolver->set_max_file_descriptors(options.max_file_descriptors);

  // 2. Load the root layer.
  Layer root_layer;
  {
    USDLoadOptions lopts;
    lopts.max_allowed_asset_size_in_mb = static_cast<uint32_t>(
        std::min<size_t>(options.max_asset_bytes_mb, 0xffffffffu));
    if (!LoadLayerFromFile(filename, &root_layer, warn, err, lopts)) {
      return false;
    }
  }

  // 3. Composite subLayers (L phase); CompositionGraph expects the sublayers
  //    already merged into the root layer.
  _flattened = std::make_unique<Layer>();
  {
    SublayersCompositionOptions subopts;
    subopts.allow_parent_relative_paths = options.allow_parent_relative_paths;
    subopts.max_depth = options.max_composition_depth;
    if (!CompositeSublayers(*_resolver, root_layer, _flattened.get(), warn, err,
                            subopts)) {
      return false;
    }
  }

  // 4. Parse-once registry for referenced/payload layers.
  if (options.dedup_layers) {
    _registry = std::make_unique<pcp::LayerRegistry>();
  }

  // 5. Build the composition graph (R/V/P phases) with the payload policy.
  CompositionGraphOptions cgopts;
  cgopts.payload_policy_with_prim =
      MakePayloadPolicy(options, _resolver.get());
  cgopts.detect_instances = options.detect_instances;
  cgopts.allow_parent_relative_paths = options.allow_parent_relative_paths;
  cgopts.max_depth = options.max_composition_depth;
  // Large scenes legitimately have many millions of composed prims / arc nodes;
  // raise the general-API safety backstops accordingly (this loader is the
  // explicit large-scene path).
  cgopts.max_composed_prims = 64u * 1024u * 1024u;
  cgopts.max_arc_nodes = 64u * 1024u * 1024u;
  cgopts.max_namespace_depth = 8192;
  if (_registry) {
    cgopts.load_layer_fn = &LargeSceneLoader::LoadLayerThunk;
    cgopts.load_layer_userdata = this;
  }

  auto r = CompositionGraph::Compose(*_resolver, *_flattened, cgopts);
  if (!r) {
    if (err) *err = "CompositionGraph::Compose failed: " + r.error();
    return false;
  }
  _graph = std::make_unique<CompositionGraph>(std::move(*r));

  // 6. Reconstruct the Stage from the graph.
  auto rebuilt = std::make_shared<Stage>();
  if (!_graph->BuildStage(rebuilt.get(), warn, err)) {
    return false;
  }
  _stage = std::move(rebuilt);

  _loaded = true;
  return true;
}

std::vector<Path> LargeSceneLoader::deferred_payload_paths() const {
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_graph) return {};
  return _graph->GetDeferredPayloadPaths();
}

size_t LargeSceneLoader::deferred_count() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _graph ? _graph->GetDeferredPayloadPaths().size() : 0;
}

std::shared_ptr<const Stage> LargeSceneLoader::stage_snapshot() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _stage;
}

bool LargeSceneLoader::load_payload(const Path &prim_path, std::string *warn,
                                    std::string *err) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_graph || !_resolver) {
    if (err) *err = "LargeSceneLoader: not loaded.";
    return false;
  }
  auto r = _graph->LoadPayload(prim_path, *_resolver);
  if (!r) {
    if (err) *err = r.error();
    return false;
  }
  if (!RebuildStageLocked(warn, err)) {
    return false;
  }
  return true;
}

bool LargeSceneLoader::unload_payload(const Path &prim_path, std::string *warn,
                                      std::string *err) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_graph) {
    if (err) *err = "LargeSceneLoader: not loaded.";
    return false;
  }
  auto r = _graph->UnloadPayload(prim_path);
  if (!r) {
    if (err) *err = r.error();
    return false;
  }
  if (!RebuildStageLocked(warn, err)) {
    return false;
  }
  return true;
}

bool LargeSceneLoader::rebuild_stage(std::string *warn, std::string *err) {
  std::lock_guard<std::mutex> lock(_mutex);
  return RebuildStageLocked(warn, err);
}

bool LargeSceneLoader::RebuildStageLocked(std::string *warn,
                                          std::string *err) {
  if (!_graph) {
    if (err) *err = "LargeSceneLoader: not loaded.";
    return false;
  }
  auto rebuilt = std::make_shared<Stage>();
  if (!_graph->BuildStage(rebuilt.get(), warn, err)) {
    return false;
  }
  _stage = std::move(rebuilt);
  return true;
}

size_t LargeSceneLoader::estimate_stage_memory_bytes() const {
  const std::shared_ptr<const Stage> snapshot = stage_snapshot();
  return snapshot ? snapshot->estimate_memory_usage() : 0;
}

size_t LargeSceneLoader::layer_parse_count() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _registry ? _registry->parse_count() : 0;
}

// =========================================================================
// New backend (next::pcp::Cache, enabled by LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
// =========================================================================
#else

namespace {

// Wraps a LargeSceneLoadOptions::PayloadMode + (optional) budget state into a
// callback compatible with next::pcp::CompositionOptions::payload_policy.
// Signature: bool(const next::Path &prim_path, const std::string &asset_path).

struct NextBudgetPolicyState {
  uint64_t loaded_bytes{0};
  double loaded_extent_coverage{0.0};
  uint64_t budget_bytes{0};
  double extent_budget{0.0};
  next::AssetResolver *resolver{nullptr};
  std::mutex mu;
};

bool ExtentCoverage(const next::PrimSpec &owner, double *coverage) {
  if (!coverage) return false;
  const next::Value *value = owner.property_value("extentsHint");
  if (!value || !value->is_array()) return false;
  next::ArrayScratch<float> scratch;
  next::ArrayView<float> points;
  if (!next::GetFloatArrayView(*value, &scratch, &points) ||
      points.size < 6 || (points.size % 6) != 0) {
    return false;
  }

  double lo[3] = {std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity()};
  double hi[3] = {-std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity()};
  for (size_t i = 0; i < points.size; i += 3) {
    for (size_t axis = 0; axis < 3; ++axis) {
      const double v = static_cast<double>(points[i + axis]);
      if (!std::isfinite(v)) return false;
      lo[axis] = std::min(lo[axis], v);
      hi[axis] = std::max(hi[axis], v);
    }
  }

  const double dx = std::max(0.0, hi[0] - lo[0]);
  const double dy = std::max(0.0, hi[1] - lo[1]);
  const double dz = std::max(0.0, hi[2] - lo[2]);
  *coverage = std::max({dx * dy, dx * dz, dy * dz});
  return std::isfinite(*coverage);
}

std::function<bool(const next::Path &, const std::string &,
                   const next::PrimSpec &)>
MakeNextPayloadPolicy(const LargeSceneLoadOptions &opts,
                      next::AssetResolver *resolver,
                      bool *load_payloads) {
  if (load_payloads) *load_payloads = false;
  switch (opts.payload_mode) {
    case LargeSceneLoadOptions::PayloadMode::LoadAll:
      if (load_payloads) *load_payloads = true;
      return nullptr;  // null policy + load_payloads=true => eager load.
    case LargeSceneLoadOptions::PayloadMode::LoadNone:
      return [](const next::Path &, const std::string &,
                const next::PrimSpec &) { return false; };
    case LargeSceneLoadOptions::PayloadMode::Budget: {
      auto st = std::make_shared<NextBudgetPolicyState>();
      st->budget_bytes = BudgetMiBToBytes(opts.payload_budget_mb);
      st->extent_budget = opts.payload_extent_budget;
      st->resolver = resolver;
      return [st](const next::Path &, const std::string &ap,
                  const next::PrimSpec &owner) -> bool {
        std::lock_guard<std::mutex> lk(st->mu);
        const std::string &anchor_dir =
            next::AssetAnchorPath(owner.asset_anchor_id());
        const std::string anchor = anchor_dir.empty()
                                       ? std::string()
                                       : next::AssetResolver::IsPackagePath(anchor_dir)
                                           ? anchor_dir : anchor_dir + "/__layer__.usd";
        const std::string rp = st->resolver->ResolvePath(ap, anchor);
        const uint64_t sz = FileSizeBytes(rp);
        if (sz == 0 || st->loaded_bytes > st->budget_bytes ||
            sz > st->budget_bytes - st->loaded_bytes) {
          return false;
        }

        double extent_cost = 0.0;
        const bool has_extent = st->extent_budget > 0.0 &&
                                ExtentCoverage(owner, &extent_cost);
        if (has_extent &&
            (st->loaded_extent_coverage > st->extent_budget ||
             extent_cost >
                 st->extent_budget - st->loaded_extent_coverage)) {
          return false;
        }

        st->loaded_bytes += sz;
        if (has_extent) st->loaded_extent_coverage += extent_cost;
        return true;
      };
    }
  }
  return [](const next::Path &, const std::string &,
            const next::PrimSpec &) { return false; };
}

}  // namespace

LargeSceneLoader::LargeSceneLoader() = default;
LargeSceneLoader::~LargeSceneLoader() = default;

bool LargeSceneLoader::Load(const std::string &filename,
                            const LargeSceneLoadOptions &options,
                            std::string *warn, std::string *err) {
  std::lock_guard<std::mutex> lock(_mutex);
  _loaded = false;

  // 1. Setup next-style resolver anchored at the scene's base directory.
  _resolver = std::make_unique<next::AssetResolver>();
  const std::string base_dir = io::GetBaseDir(filename);
  next::ResolverConfig rconf;
  rconf.working_directory = base_dir.empty() ? "./" : base_dir;
  rconf.search_paths = {base_dir.empty() ? "./" : base_dir};
  for (const auto &p : options.search_paths) {
    rconf.search_paths.push_back(p);
  }
  rconf.allow_absolute_paths = true;
  _resolver->SetConfig(rconf);

  // 2. Load the root layer using the next module's layer loader, which
  //    dispatches by extension to the next USDA/USDC readers and returns
  //    a shared_ptr<next::Layer> (the type that Cache::Open expects).
  std::string resolved_path = _resolver->ResolvePath(filename);
  if (resolved_path.empty()) resolved_path = filename;
  next::pcp::LayerLoadOptions root_load_options;
  root_load_options.max_memory = static_cast<size_t>(std::min<uint64_t>(
      BudgetMiBToBytes(options.max_asset_bytes_mb),
      static_cast<uint64_t>((std::numeric_limits<size_t>::max)())));
  root_load_options.usdc_lazy_arrays = options.mmap_zero_copy;
  root_load_options.usdc_use_mmap = options.mmap_zero_copy;
  auto root_shared = next::pcp::LoadLayerFromFile(
      resolved_path, warn, err, root_load_options);
  if (!root_shared) {
    if (err) {
      std::string prev = *err;
      *err = "LargeSceneLoader: failed to load root layer '" + filename +
             "': " + prev;
    }
    return false;
  }

  // 3. Open the next::pcp::Cache (handles sublayers, references + payload
  //    according to options).
  next::pcp::CompositionOptions copts;
  copts.payload_policy_with_prim =
      MakeNextPayloadPolicy(options, _resolver.get(), &copts.load_payloads);
  copts.detect_instances = options.detect_instances;
  copts.max_depth = options.max_composition_depth;
  copts.max_namespace_depth = options.max_composition_depth;
  copts.usdc_lazy_arrays = options.mmap_zero_copy;
  copts.usdc_use_mmap = options.mmap_zero_copy;

  auto cache_r = next::pcp::Cache::Open(*_resolver, root_shared,
                                        filename, copts);
  if (!cache_r) {
    if (err) *err = "next::pcp::Cache::Open failed: " + cache_r.error();
    return false;
  }
  _cache = std::make_unique<next::pcp::Cache>(std::move(*cache_r));

  // 4. Materialize the Stage.
  auto rebuilt = std::make_shared<next::Stage>();
  if (!_cache->BuildStage(rebuilt.get(), warn, err)) {
    return false;
  }
  _stage = std::move(rebuilt);

  _loaded = true;
  return true;
}

/// Helper: convert lightusd::Path (old) to lightusd::next::Path (new).
static next::Path ToNextPath(const Path &p) {
  return next::Path(p.full_path_name());
}

/// Helper: convert lightusd::next::Path (new) to lightusd::Path (old).
static Path FromNextPath(const next::Path &np) {
  return Path(np.str(), "");
}

std::vector<Path> LargeSceneLoader::deferred_payload_paths() const {
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_cache) return {};
  const std::vector<next::Path> npaths = _cache->GetDeferredPayloadPaths();
  std::vector<Path> out;
  out.reserve(npaths.size());
  for (const auto &np : npaths) out.push_back(FromNextPath(np));
  return out;
}

size_t LargeSceneLoader::deferred_count() const {
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_cache) return 0;
  return _cache->GetDeferredPayloadPaths().size();
}

std::shared_ptr<const next::Stage> LargeSceneLoader::stage_snapshot() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _stage;
}

bool LargeSceneLoader::load_payload(const Path &prim_path, std::string *warn,
                                    std::string *err) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_cache) {
    if (err) *err = "LargeSceneLoader: not loaded.";
    return false;
  }
  if (!_cache->LoadPayload(ToNextPath(prim_path), warn, err)) {
    return false;
  }
  // Rebuild the stage to materialize the loaded payload.
  if (!RebuildStageLocked(warn, err)) {
    return false;
  }
  return true;
}

bool LargeSceneLoader::unload_payload(const Path &prim_path, std::string *warn,
                                      std::string *err) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (!_cache) {
    if (err) *err = "LargeSceneLoader: not loaded.";
    return false;
  }
  if (!_cache->UnloadPayload(ToNextPath(prim_path))) {
    if (err) *err = "UnloadPayload failed.";
    return false;
  }
  // Rebuild the stage to materialize the unloaded payload.
  if (!RebuildStageLocked(warn, err)) {
    return false;
  }
  return true;
}

bool LargeSceneLoader::rebuild_stage(std::string *warn, std::string *err) {
  std::lock_guard<std::mutex> lock(_mutex);
  return RebuildStageLocked(warn, err);
}

bool LargeSceneLoader::RebuildStageLocked(std::string *warn,
                                          std::string *err) {
  if (!_cache) {
    if (err) *err = "LargeSceneLoader: not loaded.";
    return false;
  }
  auto rebuilt = std::make_shared<next::Stage>();
  if (!_cache->BuildStage(rebuilt.get(), warn, err)) {
    return false;
  }
  _stage = std::move(rebuilt);
  return true;
}

size_t LargeSceneLoader::estimate_stage_memory_bytes() const {
  const std::shared_ptr<const next::Stage> snapshot = stage_snapshot();
  return snapshot ? snapshot->GetMemoryUsage() : 0;
}

size_t LargeSceneLoader::layer_parse_count() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _cache ? _cache->layer_registry().parse_count() : 0;
}

#endif  // LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE

// =========================================================================
// One-shot convenience (old path only; new path users call LargeSceneLoader
// directly or use next::pcp::ComposeStageFromFile).
// =========================================================================

#if !defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)

bool LoadStageFromFile(const std::string &filename, Stage *stage,
                       std::string *warn, std::string *err,
                       const LargeSceneLoadOptions &options) {
  if (!stage) {
    if (err) *err = "LoadStageFromFile: null stage.";
    return false;
  }
  LargeSceneLoader loader;
  if (!loader.Load(filename, options, warn, err)) {
    return false;
  }
  (*stage) = std::move(loader.stage());
  return true;
}

#endif  // !LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE

}  // namespace lightusd
