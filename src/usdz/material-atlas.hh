// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "layer/layer.hh"
#include "udim-bake.hh"

namespace lightusd::usdz {
struct MaterialAtlasOptions {
  int edge = 4096;
  int tile = 512;
  int padding = 2;
  int minimum = 2;
  size_t memory_budget = size_t(512) << 20;
};

// Bake compatible, directly bound PreviewSurface materials. The fetch callback
// resolves already anchored image paths. Unsupported graphs remain unchanged.
bool BakeMaterialAtlases(Layer&, const MaterialAtlasOptions&,
                         const udim::Fetch&,
                         std::map<std::string, std::vector<uint8_t>>* assets,
                         std::string* warning, std::string* error);
}  // namespace lightusd::usdz
