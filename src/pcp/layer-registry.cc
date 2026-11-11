// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - PCP LayerRegistry implementation

#include "layer-registry.hh"
#include "layer-source-snapshot.hh"
#include "safe-file-size.hh"

#include "layer/asset-anchor.hh"
#include "layer/layer.hh"
#include "reader/usda-reader.hh"
#include "reader/usdc-reader.hh"
#include "reader/usdz-reader.hh"
#include "core/mtlx-dom.hh"
#include "tydra/asset-import/import.hh"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>

namespace lightusd {
namespace pcp {

bool LayerRegistry::EnableSourceSnapshots(const std::string& directory,
                                         uint64_t max_bytes,
                                         std::string* error) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lock(*mu_);
  if (!in_flight_.empty()) return false;
#endif
  if (directory.empty() || !max_bytes || !by_resolved_.empty() ||
      !snapshots_.empty() || !LayerSourceSnapshot::supported()) {
    if (error) *error += "invalid source snapshot configuration\n";
    return false;
  }
  snapshot_directory_ = directory;
  snapshot_budget_ = std::make_shared<AssetPayloadBudget>(max_bytes);
  return true;
}

size_t LayerRegistry::EvictSnapshottedLayers() {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lock(*mu_);
  if (!in_flight_.empty()) return 0;
#endif
  size_t count = 0;
  for (const auto& entry : snapshots_) {
    auto found = by_resolved_.find(entry.first);
    if (found == by_resolved_.end() || !found->second ||
        found->second->prim_filler() || found->second->meta().imported_assets)
      continue;
    file_layers_.erase(found->second.get());
    by_resolved_.erase(found);
    ++count;
  }
  return count;
}

std::vector<std::string> LayerRegistry::snapshot_identifiers() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lock(*mu_);
#endif
  std::vector<std::string> ids;
  ids.reserve(snapshots_.size());
  for (const auto& entry : snapshots_) ids.push_back(entry.first);
  return ids;
}

namespace {
std::string SourceExtension(const std::string& identifier) {
  std::string package, entry;
  const std::string& name = AssetResolver::ParsePackagePath(identifier, &package,
                                                         &entry) ? entry : identifier;
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = name.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}
bool SnapshotFormat(const std::string& identifier) {
  const std::string ext = SourceExtension(identifier);
  return ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz";
}
}  // namespace

void LayerRegistry::RetainImportedAssets(Layer* output) const {
  if (!output) return;
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lock(*mu_);
#endif
  std::shared_ptr<ImportedAssets> merged;
  auto append = [&](const Layer* layer) {
    if (!layer || !layer->meta().imported_assets) return;
    if (!merged) merged = std::make_shared<ImportedAssets>();
    for (const auto& entry : layer->meta().imported_assets->by_identifier)
      merged->by_identifier.emplace(entry.first, entry.second);
  };
  append(output);
  for (const auto& entry : by_resolved_) append(entry.second.get());
  output->meta().imported_assets = std::move(merged);
}

std::shared_ptr<Layer> LayerRegistry::GetOrLoad(AssetResolver &resolver,
                                                const std::string &asset_path,
                                                const std::string &anchor,
                                                std::string *warn,
                                                std::string *err,
                                                const LayerLoadOptions &options) {
  SnapshotRequest request;
  ResolvedAsset resolved_asset;
  if (snapshot_budget_) {
    request = SnapshotRequest(anchor, asset_path, options.strict_aousd_conformance);
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::lock_guard<std::mutex> lock(*mu_);
#endif
    auto found = snapshot_resolutions_.find(request);
    if (found != snapshot_resolutions_.end()) resolved_asset = found->second;
  }
  if (resolved_asset.resolved_path.empty())
    resolved_asset = resolver.Resolve(asset_path, anchor,
                                      !options.strict_aousd_conformance);
  const std::string resolved = resolved_asset.resolved_path;
  if (resolved.empty()) {
    if (err) *err += "Failed to resolve asset path: " + asset_path + "\n";
    return nullptr;
  }
  const bool native_file = AssetResolver::GetIdentifierScheme(resolved).empty() &&
                           !resolver.HasAssetReader() &&
                           resolver.NativeFilesystemEnabled() &&
                           !resolver.HasMemoryAsset(resolved);
  LayerLoadOptions effective = options;
  // Keep dependent resources on the same resolver/context as their OBJ layer.
  if (!native_file) {
    effective.asset_read = [&resolver](
        const std::string& name, const std::string& anchor_name,
        std::vector<uint8_t>* bytes, std::string* id, std::string* error) {
      auto asset = resolver.Resolve(name, anchor_name, false);
      *id = asset.resolved_path;
      return asset.exists && resolver.ReadAsset(asset, bytes, error);
    };
    // Resolve once, read lazily: the importer can then recognize two authored
    // names that map to one identifier (e.g. map_Kd/map_Ks aliases) and reuse
    // the retained bytes instead of invoking the resolver reader again.
    effective.asset_open = [&resolver](
        const std::string& name, const std::string& anchor_name,
        tydra::import_asset::AssetSource* source, std::string* error) {
      if (!source) return false;
      auto asset = resolver.Resolve(name, anchor_name, false);
      if (!asset.exists || asset.resolved_path.empty()) {
        if (error) *error = "Failed to resolve asset path: " + name;
        return false;
      }
      source->identifier = asset.resolved_path;
      source->canonical = asset.canonical_path;
      source->read = [&resolver, asset](std::vector<uint8_t>* bytes,
                                        std::string* why) {
        return resolver.ReadAsset(asset, bytes, why);
      };
      return true;
    };
  }
  std::shared_ptr<LayerSourceSnapshot> snapshot;
  auto load_resolved = [&](std::string* load_warn,
                           std::string* load_err) -> std::shared_ptr<Layer> {
    if (snapshot_budget_ && (!native_file || SnapshotFormat(resolved))) {
      {
#if defined(LIGHTUSD_ENABLE_THREAD)
        std::lock_guard<std::mutex> lock(*mu_);
#endif
        auto found = snapshots_.find(resolved);
        if (found != snapshots_.end()) snapshot = found->second;
      }
      const bool replay = static_cast<bool>(snapshot);
      if (!snapshot) {
        if (native_file) {
          ResolvedAsset source = resolved_asset;
          std::string package, entry;
          if (AssetResolver::ParsePackagePath(resolved, &package, &entry))
            source = resolver.Resolve(package, "", false);
          snapshot = LayerSourceSnapshot::FromFile(
              source, effective.max_memory, snapshot_directory_, snapshot_budget_, load_err);
          if (snapshot && !resolver.FileIdentityUnchanged(source)) {
            if (load_err) *load_err += "asset changed while snapshotting: " + resolved + "\n";
            snapshot.reset();
          }
        } else {
          std::vector<uint8_t> bytes;
          if (!resolver.ReadAsset(resolved_asset, &bytes, load_err)) return nullptr;
          if (effective.max_memory && bytes.size() > effective.max_memory) {
            if (load_err) *load_err += "source snapshot input limit exceeded\n";
            return nullptr;
          }
          // Content-based callback readers can return foreign XML even under
          // a USD-looking identifier. Preserve that reader's dispatch and pin
          // its parsed dependency owner instead of attempting native replay.
          const bool usd_magic =
              (bytes.size() >= 8 && std::memcmp(bytes.data(), "PXR-USDC", 8) == 0) ||
              (bytes.size() >= 4 && std::memcmp(bytes.data(), "PK\x03\x04", 4) == 0);
          if (!usd_magic && (LooksLikeMtlxXML(bytes.data(), bytes.size()) ||
                              SourceExtension(resolved) == ".obj"))
            return LoadLayerFromMemory(resolved, bytes.data(), bytes.size(),
                                       load_warn, load_err, effective);
          snapshot = LayerSourceSnapshot::FromMemory(
              bytes.data(), bytes.size(), snapshot_directory_, snapshot_budget_, load_err);
        }
        if (!snapshot) return nullptr;
        snapshot->set_options(effective, !native_file);
      }
      auto loaded = LoadLayerFromSourceSnapshot(
          resolved, snapshot->path(), load_warn, load_err,
          replay ? snapshot->replay_options(effective) : effective,
          snapshot->content_dispatch());
      // A package may select a foreign importer rather than a native USD
      // entry. Its auxiliary dependencies have not been snapshotted.
      if (loaded && (loaded->prim_filler() || loaded->meta().imported_assets))
        snapshot.reset();
      return loaded;
    }
    if (!native_file) {
      std::vector<uint8_t> bytes;
      if (!resolver.ReadAsset(resolved_asset, &bytes, load_err)) return nullptr;
      return LoadLayerFromMemory(resolved, bytes.data(), bytes.size(),
                                 load_warn, load_err, effective);
    }
    std::shared_ptr<Layer> loaded =
        LoadLayerFromFile(resolved, load_warn, load_err, effective);
    if (loaded && !resolver.FileIdentityUnchanged(resolved_asset)) {
      if (load_err) {
        *load_err += "asset changed while loading: " + resolved + "\n";
      }
      return nullptr;
    }
    return loaded;
  };

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::shared_future<LoadOutcome> wait_fut;
  std::shared_ptr<std::promise<LoadOutcome>> my_promise;
  {
    std::lock_guard<std::mutex> lk(*mu_);
    auto it = by_resolved_.find(resolved);
    if (it != by_resolved_.end()) {
      if (snapshots_.count(resolved))
        snapshot_resolutions_.emplace(request, resolved_asset);
      return it->second;
    }

    auto fit = in_flight_.find(resolved);
    if (fit != in_flight_.end()) {
      wait_fut = fit->second;  // another thread is parsing this path
    } else {
      // Become the loader: publish a future, then parse outside the lock.
      my_promise = std::make_shared<std::promise<LoadOutcome>>();
      in_flight_.emplace(resolved, my_promise->get_future().share());
    }
  }

  if (!my_promise) {
    // Someone else is loading this exact path; wait for them (outside the lock).
    LoadOutcome outcome = wait_fut.get();
    if (outcome.layer && snapshot_budget_) {
      std::lock_guard<std::mutex> lock(*mu_);
      if (snapshots_.count(resolved))
        snapshot_resolutions_.emplace(request, resolved_asset);
    }
    if (warn) *warn += outcome.warn;
    if (err) *err += outcome.err;
    return outcome.layer;
  }

  // Parse WITHOUT holding the lock, so other paths load concurrently.
  LoadOutcome outcome;
  outcome.layer = load_resolved(&outcome.warn, &outcome.err);
  {
    std::lock_guard<std::mutex> lk(*mu_);
    if (outcome.layer) {
      ++parse_count_;
      by_resolved_.emplace(resolved, outcome.layer);
      if (snapshot) {
        snapshots_.emplace(resolved, snapshot);
        snapshot_resolutions_.emplace(request, resolved_asset);
      }
      if (native_file) file_layers_.insert(outcome.layer.get());
    }
    in_flight_.erase(resolved);  // a failed load is retried by the next caller
  }
  if (warn) *warn += outcome.warn;
  if (err) *err += outcome.err;
  my_promise->set_value(outcome);  // unblock any waiters
  return outcome.layer;
#else
  auto it = by_resolved_.find(resolved);
  if (it != by_resolved_.end()) {
    if (snapshots_.count(resolved))
      snapshot_resolutions_.emplace(request, resolved_asset);
    return it->second;  // Cache hit -- no re-parse.
  }

  std::shared_ptr<Layer> layer = load_resolved(warn, err);
  if (!layer) {
    return nullptr;
  }

  ++parse_count_;
  by_resolved_.emplace(resolved, layer);
  if (snapshot) {
    snapshots_.emplace(resolved, snapshot);
    snapshot_resolutions_.emplace(request, resolved_asset);
  }
  if (native_file) file_layers_.insert(layer.get());
  return layer;
#endif
}

void LayerRegistry::Drop(const std::string &resolved_path) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*mu_);
#endif
  auto it = by_resolved_.find(resolved_path);
  if (it != by_resolved_.end()) {
    file_layers_.erase(it->second.get());
    by_resolved_.erase(it);
  }
}

std::vector<std::shared_ptr<Layer>> LayerRegistry::TakeFileLayersForRelease() {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*mu_);
  if (!in_flight_.empty()) return {};
#endif
  std::vector<std::shared_ptr<Layer>> layers;
  layers.reserve(file_layers_.size());
  for (auto it = by_resolved_.begin(); it != by_resolved_.end();) {
    if (file_layers_.count(it->second.get())) {
      layers.push_back(std::move(it->second));
      it = by_resolved_.erase(it);
    } else {
      ++it;
    }
  }
  file_layers_.clear();
  return layers;
}

}  // namespace pcp
}  // namespace lightusd
