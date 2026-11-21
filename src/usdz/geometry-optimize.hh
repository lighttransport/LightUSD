// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <string>
namespace lightusd {
class Stage;
}
namespace lightusd::usdz {
struct MeshMergeOptions {
  size_t max_input_faces{2048};
  size_t max_input_points{4096};
  size_t max_aggregate_faces{65536};
  size_t min_group_size{2};
  size_t memory_budget_bytes{size_t(512) << 20};
};
struct GeometryOptimizationStats {
  size_t num_meshes_before{0};
  size_t num_meshes_after{0};
  size_t num_meshes_eligible{0};
  size_t num_meshes_merged{0};
  size_t num_mesh_aggregates{0};
};
// Whole-mesh aggregation for static delivery stages. Unsupported/animated
// meshes remain intact. Preserves disconnected subdivision topology and
// material subsets. Merged mesh paths change; callers must flatten composition
// before using this.
bool MergeMeshes(Stage* stage, const MeshMergeOptions& options,
                 GeometryOptimizationStats* stats, std::string* warn,
                 std::string* error);
}  // namespace lightusd::usdz
