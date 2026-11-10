// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <utility>
#include <vector>

#include "layer/layer.hh"
namespace lightusd::usdz::detail {
// Drop detached append-only storage so path-based Crate writing cannot revive
// it.
inline void CompactLayer(Layer* layer) {
  Layer compact;
  compact.meta() = layer->meta();
  compact.set_source_asset_anchor_id(layer->source_asset_anchor_id());
  std::vector<uint32_t> indices(layer->prim_count(), UINT32_MAX);
  for (size_t i = 0; i < layer->prim_count(); ++i) {
    const PrimSpec* prim = layer->prim(static_cast<uint32_t>(i));
    if (!layer->prim_at_path(prim->path().str())) continue;
    PrimSpec copy = prim->Clone();
    copy.clear_child_indices();
    indices[i] = compact.add_prim(std::move(copy));
  }
  for (uint32_t root : layer->root_indices())
    if (indices[root] != UINT32_MAX) compact.add_root(indices[root]);
  for (size_t i = 0; i < indices.size(); ++i) {
    if (indices[i] == UINT32_MAX) continue;
    for (uint32_t child :
         layer->prim(static_cast<uint32_t>(i))->child_indices())
      if (indices[child] != UINT32_MAX)
        compact.set_parent(indices[child], indices[i]);
  }
  compact.finalize();
  *layer = std::move(compact);
}
}  // namespace lightusd::usdz::detail
