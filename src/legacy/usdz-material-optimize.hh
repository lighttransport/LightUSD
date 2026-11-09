// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment, Inc.
#pragma once

#include <string>

#include "usdz-convert.hh"

namespace lightusd {

class Layer;

namespace usdz {

struct MaterialOptimizationStats {
  size_t num_materials_before{0};
  size_t num_materials_after{0};
  size_t num_materials_deduped{0};
  size_t num_materials_preview_converted{0};
  size_t num_materials_skipped{0};
  size_t num_atlas_materials{0};
  size_t num_atlas_textures{0};
};

bool OptimizeMaterialsInLayer(const UsdzConvertOptions &options, Layer *layer,
                              MaterialOptimizationStats *stats,
                              std::string *warn, std::string *err);

}  // namespace usdz
}  // namespace lightusd
