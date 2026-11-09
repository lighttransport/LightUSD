// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Bone/Skeleton utility functions for skeletal animation
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lightusd {
namespace tydra {

///
/// Bone reduction strategy
///
enum class BoneReductionStrategy {
  // Simple greedy: Keep top N strongest weights
  Greedy = 0,

  // Hierarchical: Prefer bones in same chain/hierarchy
  // Falls back to Greedy if no hierarchy info available
  Hierarchical = 1,

  // Error-aware: Minimize deformation error using quadratic metric
  ErrorMetric = 2,

  // Adaptive: Choose strategy per vertex based on weight distribution
  Adaptive = 3
};

///
/// Bone reduction configuration
///
struct BoneReductionConfig {
  // Target number of bone influences per vertex
  uint32_t target_bone_count = 4;

  // Reduction strategy to use
  BoneReductionStrategy strategy = BoneReductionStrategy::ErrorMetric;

  // Minimum weight threshold (influences below this are discarded early)
  // Set to 0.0 to keep all non-zero weights in consideration
  float min_weight_threshold = 0.0f;

  // For ErrorMetric strategy: balance between weight preservation and count reduction
  // Higher values favor keeping more bones, lower values favor aggressive reduction
  // Range: [0.0, 1.0], default 0.5
  float error_tolerance = 0.5f;

  // Enable weight normalization after reduction (should always be true)
  bool normalize_weights = true;
};

///
/// Bone hierarchy information (optional, for hierarchical reduction)
///
struct BoneHierarchyInfo {
  // Parent bone index for each bone (-1 if root)
  std::vector<int> parent_indices;

  // Bone names (optional, for debugging)
  std::vector<std::string> bone_names;

  bool is_valid() const {
    return !parent_indices.empty();
  }
};

///
/// Result statistics from bone reduction
///
struct BoneReductionStats {
  uint32_t num_vertices = 0;
  uint32_t original_bone_count = 0;
  uint32_t target_bone_count = 0;
  float avg_weight_error = 0.0f;  // Average L2 error in weights
  float max_weight_error = 0.0f;  // Maximum L2 error in weights
  uint32_t num_vertices_modified = 0;  // Number of vertices that were reduced
};

///
/// Reduce bone influences per vertex to target count.
///
/// This is the main bone reduction function with advanced algorithms.
/// It supports multiple strategies and can utilize bone hierarchy information
/// for better quality reduction.
///
/// @param[in,out] joint_indices Joint index array (size = num_vertices * element_size)
/// @param[in,out] joint_weights Weight array (size = num_vertices * element_size)
/// @param[in] element_size Current number of bone influences per vertex
/// @param[in] num_vertices Number of vertices
/// @param[in] config Reduction configuration
/// @param[in] hierarchy Optional bone hierarchy info (can be nullptr)
/// @param[out] stats Optional statistics output (can be nullptr)
/// @return true on success, false on error
///
bool ReduceBoneInfluences(
    std::vector<int> &joint_indices,
    std::vector<float> &joint_weights,
    uint32_t element_size,
    uint32_t num_vertices,
    const BoneReductionConfig &config,
    const BoneHierarchyInfo *hierarchy = nullptr,
    BoneReductionStats *stats = nullptr);

///
/// Simple greedy bone reduction (legacy interface for compatibility)
/// Keeps only the N strongest influences and renormalizes weights.
///
/// @param[in,out] joint_indices Joint index array
/// @param[in,out] joint_weights Weight array
/// @param[in] element_size Current number of bone influences per vertex
/// @param[in] target_bone_count Target number of influences
/// @param[in] num_vertices Number of vertices
/// @return true on success
///
bool ReduceBoneInfluencesSimple(
    std::vector<int> &joint_indices,
    std::vector<float> &joint_weights,
    uint32_t element_size,
    uint32_t target_bone_count,
    uint32_t num_vertices);

///
/// Calculate bone hierarchy depth for each bone
/// Useful for hierarchical reduction strategies
///
/// @param[in] parent_indices Parent index for each bone (-1 for root)
/// @param[out] depths Output depth for each bone (root = 0)
/// @return true on success
///
bool CalculateBoneDepths(
    const std::vector<int> &parent_indices,
    std::vector<int> &depths);

///
/// Find bone chain distance between two bones in hierarchy
/// Returns -1 if bones are not in same chain
///
/// @param[in] bone_a First bone index
/// @param[in] bone_b Second bone index
/// @param[in] parent_indices Parent index array
/// @return Chain distance, or -1 if not in same chain
///
int FindBoneChainDistance(
    int bone_a,
    int bone_b,
    const std::vector<int> &parent_indices);

}  // namespace tydra
}  // namespace lightusd
