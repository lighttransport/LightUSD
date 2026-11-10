// SPDX-License-Identifier: Apache-2.0
#include "material-optimize.hh"

#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "diff/layer-diff.hh"
#include "layer-edit.hh"
#include "layer/asset-anchor.hh"
#include "writer/usda-writer.hh"

namespace lightusd::usdz {
namespace {
struct Candidate {
  std::string path;
  Layer normalized;
};

Layer Normalize(const Layer& source, uint32_t root) {
  Layer result;
  const std::string prefix = source.prim(root)->path().str();
  std::vector<std::pair<uint32_t, uint32_t>> pending{{root, UINT32_MAX}};
  while (!pending.empty()) {
    const auto item = pending.back();
    pending.pop_back();
    const PrimSpec& original = *source.prim(item.first);
    PrimSpec copy = original.Clone();
    copy.clear_child_indices();
    copy.set_path(
        Path("/Material" + original.path().str().substr(prefix.size())));
    if (item.first == root) copy.set_name("Material");
    copy.remap_target_prefix(prefix, "/Material");
    const uint32_t index = result.add_prim(std::move(copy));
    if (item.second == UINT32_MAX)
      result.add_root(index);
    else
      result.set_parent(index, item.second);
    const auto& children = original.child_indices();
    for (auto it = children.rbegin(); it != children.rend(); ++it)
      pending.emplace_back(*it, index);
  }
  result.finalize();
  // Equal relative texture spellings authored in different layers can resolve
  // to different files. Compare source-resolved paths while retaining the
  // representative material's original authoring in the actual output layer.
  RebaseLayerAssetPaths(&result, {});
  return result;
}

// Restrict semantic comparison to a closed graph of supported local shaders.
// External/interface connections, cycles and extra material outputs retain the
// exact subtree key so deduplication cannot discard unexamined shading
// behavior.
bool PreviewNormalize(const Layer& source, uint32_t root, Layer* result) {
  const PrimSpec& material = *source.prim(root);
  const auto* surface = material.connection("outputs:surface");
  if (!surface || surface->size() != 1) return false;
  for (const auto& name : material.authored_property_names())
    if (name.rfind("outputs:", 0) == 0 && name != "outputs:surface")
      return false;
  const std::string prefix = material.path().str() + "/";
  const std::string surface_path = surface->front().prim_path().str();
  std::map<std::string, uint32_t> children;
  for (uint32_t index : material.child_indices()) {
    const auto* child = source.prim(index);
    if (!child || !child->child_indices().empty()) return false;
    children.emplace(child->path().str(), index);
  }
  if (!children.count(surface_path)) return false;
  const std::string canonical_surface = prefix + "__PreviewSurface";
  if (canonical_surface != surface_path && children.count(canonical_surface))
    return false;
  std::set<std::string> visiting, visited;
  std::function<bool(const std::string&, size_t)> visit;
  visit = [&](const std::string& path, size_t depth) {
    if (depth > 64 || visiting.count(path) || !children.count(path))
      return false;
    if (visited.count(path)) return true;
    const auto* shader = source.prim(children.at(path));
    const auto* value = shader->property_value("info:id");
    const auto id = value ? value->as_token() : StringView{};
    if (shader->type_name() != "Shader" || !id ||
        (path == surface_path
             ? *id != "UsdPreviewSurface"
             : (*id != "UsdUVTexture" && *id != "UsdPrimvarReader_float2")))
      return false;
    visiting.insert(path);
    for (const auto& name : shader->authored_property_names()) {
      const auto* connections = shader->connection(name);
      if (!connections) continue;
      for (const auto& target : *connections)
        if (!visit(target.prim_path().str(), depth + 1)) return false;
    }
    visiting.erase(path);
    visited.insert(path);
    return true;
  };
  if (!visit(surface_path, 0)) return false;
  Layer graph;
  PrimSpec parent = material.Clone();
  parent.clear_child_indices();
  for (const auto& name : parent.authored_property_names())
    if (name != "outputs:surface") parent.remove_property(name);
  const uint32_t graph_root = graph.add_prim(std::move(parent));
  graph.add_root(graph_root);
  std::map<std::string, std::string> paths;
  for (const auto& path : visited)
    paths.emplace(path == surface_path ? canonical_surface : path, path);
  for (const auto& pair : paths) {
    PrimSpec copy = source.prim(children.at(pair.second))->Clone();
    copy.clear_child_indices();
    if (pair.second == surface_path) {
      copy.set_path(Path(canonical_surface));
      copy.set_name("__PreviewSurface");
      copy.remove_property("outputs:surface");
    }
    graph.set_parent(graph.add_prim(std::move(copy)), graph_root);
  }
  for (size_t index = 0; index < graph.prim_count(); ++index)
    graph.prim_mutable(static_cast<uint32_t>(index))
        ->remap_target_prefix(surface_path, canonical_surface);
  graph.finalize();
  *result = Normalize(graph, graph_root);
  return true;
}

bool Equal(const Layer& lhs, const Layer& rhs) {
  DiffOptions options;
  options.floatUlps = 0;
  options.doubleUlps = 0;
  options.timeUlps = 0;
  options.fuzzyAssetPaths = false;
  return DiffToText(lhs, rhs, "left", "right", options) ==
         "No differences found.\n";
}
}  // namespace

size_t DedupeMaterials(Layer* layer, bool preview) {
  if (!layer) return 0;
  std::map<std::string, std::vector<Candidate>> groups;
  std::vector<std::pair<std::string, std::string>> remaps;
  std::vector<uint32_t> pending(layer->root_indices().rbegin(),
                                layer->root_indices().rend());
  while (!pending.empty()) {
    const uint32_t index = pending.back();
    pending.pop_back();
    const PrimSpec* prim = layer->prim(index);
    if (!prim) continue;
    if (prim->type_name() == "Material") {
      // A layer's default prim is an externally visible entry point.
      if (prim->path().str() == "/" + layer->meta().defaultPrim) continue;
      Layer normalized;
      if (!preview || !PreviewNormalize(*layer, index, &normalized))
        normalized = Normalize(*layer, index);
      auto& candidates = groups[WriteLayerToString(normalized)];
      bool duplicate = false;
      for (const Candidate& candidate : candidates) {
        if (!Equal(candidate.normalized, normalized)) continue;
        remaps.emplace_back(prim->path().str(), candidate.path);
        duplicate = true;
        break;
      }
      if (!duplicate)
        candidates.push_back({prim->path().str(), std::move(normalized)});
      continue;
    }
    const auto& children = prim->child_indices();
    pending.insert(pending.end(), children.rbegin(), children.rend());
  }
  for (const auto& remap : remaps) {
    for (size_t i = 0; i < layer->prim_count(); ++i)
      layer->prim_mutable(static_cast<uint32_t>(i))
          ->remap_target_prefix(remap.first, remap.second);
    layer->remove_prim_at_path(remap.first);
  }
  if (!remaps.empty()) {
    // remove_prim_at_path unlinks but retains append-only storage. Crate
    // writing is path-based (including disconnected variant holders), so drop
    // removed storage while retaining every still-indexed authored spec.
    detail::CompactLayer(layer);
  }
  return remaps.size();
}
}  // namespace lightusd::usdz
