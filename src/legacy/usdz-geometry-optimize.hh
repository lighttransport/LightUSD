// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment, Inc.
#pragma once

#include <cstddef>
#include <string>

#include "usdz-convert.hh"

namespace lightusd {

class Layer;

namespace usdz {

struct GeometryOptimizationStats {
  size_t num_meshes_before{0};
  size_t num_meshes_after{0};
  size_t num_meshes_eligible{0};
  size_t num_meshes_merged{0};
  size_t num_meshes_skipped{0};
  size_t num_mesh_aggregates{0};
  size_t num_faces_merged{0};
  size_t num_points_merged{0};
};

bool OptimizeGeometryInLayer(const UsdzConvertOptions &options, Layer *layer,
                             GeometryOptimizationStats *stats,
                             std::string *warn, std::string *err);

}  // namespace usdz
}  // namespace lightusd
