// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>

#include "layer/layer.hh"

namespace lightusd::usdz {
// Shared exact comparison for optimization passes. The supplied index is the
// root of the material subtree.
Layer NormalizeMaterialSubtree(const Layer& layer, uint32_t index);
bool EqualNormalizedMaterials(const Layer& lhs, const Layer& rhs);


// Deduplicate identical material subtrees in a composed layer. Connections and
// relationship targets are rewritten structurally; asset/string values remain
// untouched. Returns the number of removed materials.
// Preview mode compares the reachable supported PreviewSurface graph,
// normalizes the surface shader name and ignores unused shader children.
// Unsupported graphs fall back to exact subtree comparison.
size_t DedupeMaterials(Layer* layer, bool preview = false);
}  // namespace lightusd::usdz
