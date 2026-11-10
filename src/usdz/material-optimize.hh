// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>

#include "layer/layer.hh"

namespace lightusd::usdz {
// Deduplicate identical material subtrees in a composed layer. Connections and
// relationship targets are rewritten structurally; asset/string values remain
// untouched. Returns the number of removed materials.
// Preview mode compares the reachable supported PreviewSurface graph,
// normalizes the surface shader name and ignores unused shader children.
// Unsupported graphs fall back to exact subtree comparison.
size_t DedupeMaterials(Layer* layer, bool preview = false);
}  // namespace lightusd::usdz
