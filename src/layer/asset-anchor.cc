// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - asset-path anchors.

#include "asset-anchor.hh"

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "layer.hh"
#include "resolver/asset-resolver.hh"

namespace lightusd {

namespace {

bool IsAbsoluteDir(const std::string& p) {
  if (p.empty()) return false;
  if (p[0] == '/' || p[0] == '\\') return true;
  // Windows drive letter ("C:\...").
  return p.size() >= 2 && p[1] == ':';
}

// Resolve only against an explicitly supplied base. Interning never queries
// CWD.
std::string AbsolutizeDir(const std::string& dir, const std::string& base) {
  if (dir.empty() || base.empty() || IsAbsoluteDir(dir) ||
      !AssetResolver::GetIdentifierScheme(dir).empty())
    return dir;
  return AssetResolver::NormalizePath(AssetResolver::JoinPath(base, dir));
}

struct AnchorTable {
  std::mutex mu;
  // deque, not vector: AssetAnchorPath hands out a reference to an element and
  // another thread may intern concurrently. deque::push_back keeps references
  // to existing elements valid; vector reallocation would dangle them.
  std::deque<std::string> paths{std::string()};  // index 0 == "no anchor"
  std::unordered_map<std::string, uint32_t> by_path;
};

AnchorTable& Table() {
  static AnchorTable* t = new AnchorTable();  // leaked on purpose: ids outlive
  return *t;                                  // any stage that holds them
}

}  // namespace

uint32_t InternAssetAnchor(const std::string& dir,
                           const std::string& working_directory) {
  if (dir.empty()) return 0;

  const std::string abs = AbsolutizeDir(dir, working_directory);

  AnchorTable& t = Table();
  std::lock_guard<std::mutex> lk(t.mu);
  auto it = t.by_path.find(abs);
  if (it != t.by_path.end()) return it->second;

  const uint32_t id = static_cast<uint32_t>(t.paths.size());
  t.paths.push_back(abs);
  t.by_path.emplace(abs, id);
  return id;
}

const std::string& AssetAnchorPath(uint32_t id) {
  AnchorTable& t = Table();
  std::lock_guard<std::mutex> lk(t.mu);
  if (id == 0 || id >= t.paths.size()) return t.paths[0];  // == ""
  return t.paths[id];
}

std::string ResolveAssetAnchorPath(const std::string& asset, uint32_t id) {
  if (asset.empty() || AssetResolver::IsAbsolutePath(asset) ||
      !AssetResolver::GetIdentifierScheme(asset).empty())
    return asset;
  const std::string& anchor = AssetAnchorPath(id);
  if (anchor.empty()) return asset;
  std::string package, entry;
  if (AssetResolver::ParsePackagePath(anchor, &package, &entry)) {
    const std::string directory = AssetResolver::GetDirectory(entry);
    std::string joined = AssetResolver::NormalizePath(
        directory == "." ? asset : AssetResolver::JoinPath(directory, asset));
    std::replace(joined.begin(), joined.end(), '\\', '/');
    if (joined.empty() || AssetResolver::IsAbsolutePath(joined) ||
        joined == ".." || joined.compare(0, 3, "../") == 0 ||
        (joined.size() >= 2 && joined[1] == ':'))
      return {};
    return package + "[" + joined + "]";
  }
  return AssetResolver::NormalizePath(AssetResolver::JoinPath(anchor, asset));
}

void StampLayerAssetAnchor(Layer* layer, const std::string& identifier,
                           bool only_unanchored) {
  if (!layer || identifier.empty()) return;
  const uint32_t id =
      InternAssetAnchor(AssetResolver::IsPackagePath(identifier)
                            ? identifier
                            : AssetResolver::GetDirectory(identifier));
  std::vector<Layer*> pending{layer};
  std::unordered_set<Layer*> visited;
  while (!pending.empty()) {
    Layer* current = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second) continue;
    if (!only_unanchored || current->source_asset_anchor_id() == 0)
      current->set_source_asset_anchor_id(id);
    for (size_t i = 0; i < current->prim_count(); ++i) {
      PrimSpec* prim = current->prim_mutable(static_cast<uint32_t>(i));
      if (!prim) continue;
      if (!only_unanchored || prim->asset_anchor_id() == 0)
        prim->set_asset_anchor_id(id);
      for (const auto& set :
           static_cast<const PrimSpec*>(prim)->meta().variantSets())
        for (const auto& variant : set.variants)
          if (variant.content) pending.push_back(variant.content.get());
    }
  }
}

size_t RebaseLayerAssetPaths(Layer* layer,
                             const std::map<std::string, std::string>& remap) {
  if (!layer) return 0;
  size_t count = 0;
  std::vector<Layer*> layers{layer};
  std::unordered_set<Layer*> visited;
  while (!layers.empty()) {
    Layer* current = layers.back();
    layers.pop_back();
    if (!visited.insert(current).second) continue;
    for (size_t i = 0; i < current->prim_count(); ++i) {
      PrimSpec* prim = current->prim_mutable(static_cast<uint32_t>(i));
      if (!prim) continue;
      count += prim->remap_asset_paths(remap, true);
      if (static_cast<const PrimSpec*>(prim)->meta().variantSets().empty())
        continue;
      std::vector<std::vector<PrimVariantSetData>*> sets{
          &prim->meta().variantSets()};
      while (!sets.empty()) {
        auto* collection = sets.back();
        sets.pop_back();
        for (auto& set : *collection)
          for (auto& variant : set.variants) {
            for (auto& property : variant.properties) {
              const auto asset = property.value.as_asset_path();
              if (!asset) continue;
              const auto* mapped = FindAnchoredAssetPathRemap(
                  remap, *asset, prim->asset_anchor_id());
              const std::string rebased =
                  mapped
                      ? *mapped
                      : ResolveAssetAnchorPath(*asset, prim->asset_anchor_id());
              if (mapped || rebased != *asset) {
                property.value = Value::MakeAssetPath(rebased);
                ++count;
              }
            }
            if (!variant.variantSets.empty())
              sets.push_back(&variant.variantSets);
            if (variant.content) {
              // Variant content layers are shared by Layer::Clone. A resumable
              // composition retry must not mutate the cached parsed source.
              if (variant.content.use_count() != 1)
                variant.content =
                    std::make_shared<Layer>(variant.content->Clone());
              layers.push_back(variant.content.get());
            }
          }
      }
    }
  }
  return count;
}

void StampPackageAssetAnchor(Layer* layer, const std::string& identifier) {
  if (!layer || !AssetResolver::IsPackagePath(identifier)) return;
  const uint32_t id = InternAssetAnchor(identifier);
  std::vector<Layer*> pending{layer};
  std::unordered_set<Layer*> visited;
  while (!pending.empty()) {
    Layer* current = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second) continue;
    current->set_source_asset_anchor_id(id);
    for (size_t i = 0; i < current->prim_count(); ++i) {
      auto* prim = current->prim_mutable(static_cast<uint32_t>(i));
      if (!prim) continue;
      prim->set_asset_anchor_id(id);
      for (const auto& set :
           static_cast<const PrimSpec*>(prim)->meta().variantSets())
        for (const auto& variant : set.variants)
          if (variant.content) pending.push_back(variant.content.get());
    }
  }
}

}  // namespace lightusd
