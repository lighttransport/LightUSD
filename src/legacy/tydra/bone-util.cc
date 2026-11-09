// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Bone/Skeleton utility implementation
//
#include "bone-util.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace lightusd {
namespace tydra {

namespace {

// Internal structure for bone influence with additional metadata
struct BoneInfluence {
  int joint_index;
  float weight;
  int hierarchy_depth;  // Depth in bone hierarchy (if available)
  int chain_group;      // Group ID for bones in same chain

  BoneInfluence()
      : joint_index(-1), weight(0.0f), hierarchy_depth(0), chain_group(-1) {}

  BoneInfluence(int idx, float w, int depth = 0, int group = -1)
      : joint_index(idx), weight(w), hierarchy_depth(depth), chain_group(group) {}
};

// Compare function for sorting by weight (descending)
inline bool CompareByWeight(const BoneInfluence &a, const BoneInfluence &b) {
  return a.weight > b.weight;
}

// Calculate L2 error between original and reduced weights
inline float CalculateWeightError(const std::vector<float> &original,
                                   const std::vector<float> &reduced) {
  float error = 0.0f;
  size_t n = std::min(original.size(), reduced.size());
  for (size_t i = 0; i < n; i++) {
    float diff = original[i] - reduced[i];
    error += diff * diff;
  }
  // Pad with zeros if sizes differ
  for (size_t i = n; i < original.size(); i++) {
    error += original[i] * original[i];
  }
  return std::sqrt(error);
}

// Compute an affinity bonus for a candidate bone based on chain distance
// to already-selected bones.  Closer bones in the hierarchy get a higher bonus.
inline float ComputeAffinityBonus(int candidate_joint,
                                   const std::vector<BoneInfluence> &selected,
                                   const std::vector<int> &parent_indices) {
  float best_bonus = 0.0f;
  for (const auto &sel : selected) {
    int dist = FindBoneChainDistance(candidate_joint, sel.joint_index, parent_indices);
    if (dist >= 0) {
      // Inverse distance bonus: adjacent bones (dist==1) get 1.0, dist==2 gets 0.5, etc.
      float bonus = 1.0f / static_cast<float>(dist + 1);
      best_bonus = std::max(best_bonus, bonus);
    }
  }
  return best_bonus;
}

}  // namespace

//
// Public API implementations
//

bool CalculateBoneDepths(const std::vector<int> &parent_indices,
                         std::vector<int> &depths) {
  size_t num_bones = parent_indices.size();
  depths.resize(num_bones, -1);

  // Iteratively calculate depths
  bool changed = true;
  int max_iterations = static_cast<int>(num_bones) + 1;
  int iterations = 0;

  while (changed && iterations < max_iterations) {
    changed = false;
    iterations++;

    for (size_t i = 0; i < num_bones; i++) {
      if (depths[i] >= 0) {
        continue;  // Already calculated
      }

      int parent = parent_indices[i];
      if (parent < 0) {
        // Root bone
        depths[i] = 0;
        changed = true;
      } else if (parent < static_cast<int>(num_bones) && depths[static_cast<size_t>(parent)] >= 0) {
        // Parent depth known
        depths[i] = depths[static_cast<size_t>(parent)] + 1;
        changed = true;
      }
    }
  }

  // Check if all depths were calculated
  for (size_t i = 0; i < num_bones; i++) {
    if (depths[i] < 0) {
      // Cycle or disconnected bone
      depths[i] = 0;
    }
  }

  return true;
}

int FindBoneChainDistance(int bone_a, int bone_b,
                          const std::vector<int> &parent_indices) {
  if (bone_a < 0 || bone_b < 0 || bone_a >= static_cast<int>(parent_indices.size()) ||
      bone_b >= static_cast<int>(parent_indices.size())) {
    return -1;
  }

  if (bone_a == bone_b) {
    return 0;
  }

  // Trace both bones to root, recording paths
  std::unordered_set<int> path_a;
  int current = bone_a;
  int max_iter = static_cast<int>(parent_indices.size()) + 1;
  int iter = 0;

  while (current >= 0 && current < static_cast<int>(parent_indices.size()) && iter < max_iter) {
    path_a.insert(current);
    current = parent_indices[static_cast<size_t>(current)];
    iter++;
  }

  // Trace bone_b and check for intersection with path_a
  current = bone_b;
  int distance_b = 0;
  iter = 0;

  while (current >= 0 && current < static_cast<int>(parent_indices.size()) && iter < max_iter) {
    if (path_a.find(current) != path_a.end()) {
      // Found common ancestor, calculate distance from bone_a
      int distance_a = 0;
      int check = bone_a;
      int check_iter = 0;
      while (check != current && check >= 0 &&
             check < static_cast<int>(parent_indices.size()) && check_iter < max_iter) {
        distance_a++;
        check = parent_indices[static_cast<size_t>(check)];
        check_iter++;
      }
      return distance_a + distance_b;
    }
    current = parent_indices[static_cast<size_t>(current)];
    distance_b++;
    iter++;
  }

  return -1;  // Not in same chain
}

bool ReduceBoneInfluencesSimple(std::vector<int> &joint_indices,
                                std::vector<float> &joint_weights,
                                uint32_t element_size, uint32_t target_bone_count,
                                uint32_t num_vertices) {
  if (target_bone_count >= element_size) {
    return true;  // No reduction needed
  }

  if (target_bone_count == 0 || num_vertices == 0) {
    return false;
  }

  if (joint_indices.size() != size_t(num_vertices) * size_t(element_size) ||
      joint_weights.size() != size_t(num_vertices) * size_t(element_size)) {
    return false;
  }

  std::vector<int> reduced_indices(size_t(num_vertices) * size_t(target_bone_count), 0);
  std::vector<float> reduced_weights(size_t(num_vertices) * size_t(target_bone_count), 0.0f);

  for (uint32_t vid = 0; vid < num_vertices; vid++) {
    size_t src_offset = size_t(vid) * size_t(element_size);
    size_t dst_offset = size_t(vid) * size_t(target_bone_count);

    // Collect non-zero influences
    std::vector<BoneInfluence> influences;
    influences.reserve(element_size);

    for (uint32_t i = 0; i < element_size; i++) {
      size_t idx = src_offset + i;
      float weight = joint_weights[idx];
      if (weight > 0.0f) {
        influences.emplace_back(joint_indices[idx], weight);
      }
    }

    // Sort by weight descending
    std::sort(influences.begin(), influences.end(), CompareByWeight);

    // Keep top N
    uint32_t keep_count = std::min(target_bone_count, uint32_t(influences.size()));

    // Renormalize
    float weight_sum = 0.0f;
    for (uint32_t i = 0; i < keep_count; i++) {
      weight_sum += influences[i].weight;
    }

    if (weight_sum > 0.0f) {
      for (uint32_t i = 0; i < keep_count; i++) {
        reduced_indices[dst_offset + i] = influences[i].joint_index;
        reduced_weights[dst_offset + i] = influences[i].weight / weight_sum;
      }
    } else if (keep_count > 0 && influences.size() > 0) {
      // Fallback
      reduced_indices[dst_offset] = influences[0].joint_index;
      reduced_weights[dst_offset] = 1.0f;
    }
  }

  joint_indices.swap(reduced_indices);
  joint_weights.swap(reduced_weights);

  return true;
}

//
// Advanced reduction strategies
//

namespace {

// Greedy strategy: Simple top-N selection
bool ReduceGreedy(const std::vector<BoneInfluence> &influences, uint32_t target_count,
                  std::vector<BoneInfluence> &out_selected) {
  out_selected.clear();
  if (influences.empty() || target_count == 0) {
    return true;
  }

  // Sort by weight
  std::vector<BoneInfluence> sorted = influences;
  std::sort(sorted.begin(), sorted.end(), CompareByWeight);

  // Keep top N
  uint32_t keep = std::min(target_count, uint32_t(sorted.size()));
  out_selected.assign(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(keep));

  return true;
}

// Hierarchical strategy: Prefer bones in same chain/hierarchy
bool ReduceHierarchical(const std::vector<BoneInfluence> &influences, uint32_t target_count,
                        const std::vector<int> &parent_indices,
                        std::vector<BoneInfluence> &out_selected) {
  out_selected.clear();
  if (influences.empty() || target_count == 0) {
    return true;
  }

  // Sort by weight first
  std::vector<BoneInfluence> sorted = influences;
  std::sort(sorted.begin(), sorted.end(), CompareByWeight);

  // Always include the strongest bone
  out_selected.push_back(sorted[0]);

  if (target_count == 1 || sorted.size() <= 1) {
    return true;
  }

  // For remaining slots, score candidates by weight * affinity_bonus.
  // This prefers bones that are both heavy AND close in the hierarchy.
  struct ScoredCandidate {
    size_t index;  // index into sorted[]
    float score;
  };

  std::vector<ScoredCandidate> candidates;
  candidates.reserve(sorted.size() - 1);

  for (size_t i = 1; i < sorted.size(); i++) {
    float affinity = ComputeAffinityBonus(sorted[i].joint_index, out_selected, parent_indices);
    // Blend weight with affinity: weight is primary, affinity provides a bonus up to 50%
    float score = sorted[i].weight * (1.0f + 0.5f * affinity);
    candidates.push_back({i, score});
  }

  // Greedily pick best candidate, recompute affinities as selection grows
  while (out_selected.size() < target_count && !candidates.empty()) {
    // Find best candidate
    auto best_it = std::max_element(candidates.begin(), candidates.end(),
        [](const ScoredCandidate &a, const ScoredCandidate &b) { return a.score < b.score; });

    out_selected.push_back(sorted[best_it->index]);
    candidates.erase(best_it);

    // Recompute scores with updated selection set
    for (auto &c : candidates) {
      float affinity = ComputeAffinityBonus(sorted[c.index].joint_index, out_selected, parent_indices);
      c.score = sorted[c.index].weight * (1.0f + 0.5f * affinity);
    }
  }

  return true;
}

// Error metric strategy: Minimize deformation error
bool ReduceErrorMetric(const std::vector<BoneInfluence> &influences, uint32_t target_count,
                       float error_tolerance, std::vector<BoneInfluence> &out_selected) {
  out_selected.clear();
  if (influences.empty() || target_count == 0) {
    return true;
  }

  // Sort by weight
  std::vector<BoneInfluence> sorted = influences;
  std::sort(sorted.begin(), sorted.end(), CompareByWeight);

  // Calculate cumulative weight sum
  float total_weight = 0.0f;
  for (const auto &inf : sorted) {
    total_weight += inf.weight;
  }

  // Select bones until we capture enough weight or reach target count
  float captured_weight = 0.0f;
  float target_weight = total_weight * (1.0f - error_tolerance * 0.1f);  // Keep 90%+ of weight

  uint32_t next_idx = 0;
  for (; next_idx < uint32_t(sorted.size()) && out_selected.size() < target_count; next_idx++) {
    out_selected.push_back(sorted[next_idx]);
    captured_weight += sorted[next_idx].weight;

    // If we've captured enough weight, stop the weight-driven loop
    if (captured_weight >= target_weight) {
      next_idx++;
      break;
    }
  }

  // Fill remaining slots up to target_count with next-heaviest bones
  for (; next_idx < uint32_t(sorted.size()) && out_selected.size() < target_count; next_idx++) {
    out_selected.push_back(sorted[next_idx]);
  }

  // Ensure we have at least one bone
  if (out_selected.empty() && !sorted.empty()) {
    out_selected.push_back(sorted[0]);
  }

  return true;
}

// Adaptive strategy: Choose best strategy per vertex based on weight distribution
bool ReduceAdaptive(const std::vector<BoneInfluence> &influences, uint32_t target_count,
                    const std::vector<int> *parent_indices, float error_tolerance,
                    std::vector<BoneInfluence> &out_selected) {
  if (influences.empty()) {
    out_selected.clear();
    return true;
  }

  // Analyze weight distribution
  float max_weight = 0.0f;
  float total_weight = 0.0f;
  for (const auto &inf : influences) {
    max_weight = std::max(max_weight, inf.weight);
    total_weight += inf.weight;
  }

  float weight_concentration = (total_weight > 0.0f) ? (max_weight / total_weight) : 0.0f;

  // If weights are highly concentrated (dominant bone), use error metric
  if (weight_concentration > 0.7f) {
    return ReduceErrorMetric(influences, target_count, error_tolerance, out_selected);
  }

  // If hierarchy available and weights are distributed, use hierarchical
  if (parent_indices && !parent_indices->empty() && weight_concentration < 0.4f) {
    return ReduceHierarchical(influences, target_count, *parent_indices, out_selected);
  }

  // Default to greedy for balanced cases
  return ReduceGreedy(influences, target_count, out_selected);
}

}  // namespace

bool ReduceBoneInfluences(std::vector<int> &joint_indices,
                          std::vector<float> &joint_weights, uint32_t element_size,
                          uint32_t num_vertices, const BoneReductionConfig &config,
                          const BoneHierarchyInfo *hierarchy, BoneReductionStats *stats) {
  // Validation
  if (config.target_bone_count >= element_size) {
    if (stats) {
      stats->num_vertices = num_vertices;
      stats->original_bone_count = element_size;
      stats->target_bone_count = element_size;
      stats->avg_weight_error = 0.0f;
      stats->max_weight_error = 0.0f;
      stats->num_vertices_modified = 0;
    }
    // This shouldn't happen if called correctly
    return true;  // No reduction needed
  }

  if (config.target_bone_count == 0 || num_vertices == 0) {
    return false;
  }

  if (joint_indices.size() != size_t(num_vertices) * size_t(element_size) ||
      joint_weights.size() != size_t(num_vertices) * size_t(element_size)) {
    return false;
  }

  // Prepare hierarchy info if available
  std::vector<int> bone_depths;
  const std::vector<int> *parent_indices_ptr = nullptr;

  if (hierarchy && hierarchy->is_valid()) {
    CalculateBoneDepths(hierarchy->parent_indices, bone_depths);
    parent_indices_ptr = &hierarchy->parent_indices;
  }

  // Allocate output
  std::vector<int> reduced_indices(size_t(num_vertices) * size_t(config.target_bone_count), 0);
  std::vector<float> reduced_weights(size_t(num_vertices) * size_t(config.target_bone_count),
                                     0.0f);

  // Statistics tracking
  float total_error = 0.0f;
  float max_error = 0.0f;
  uint32_t num_modified = 0;

  // Process each vertex
  for (uint32_t vid = 0; vid < num_vertices; vid++) {
    size_t src_offset = size_t(vid) * size_t(element_size);
    size_t dst_offset = size_t(vid) * size_t(config.target_bone_count);

    // Collect influences for this vertex
    std::vector<BoneInfluence> influences;
    influences.reserve(element_size);

    std::vector<float> original_weights_for_error;
    if (stats) {
      original_weights_for_error.reserve(element_size);
    }

    for (uint32_t i = 0; i < element_size; i++) {
      size_t idx = src_offset + i;
      float weight = joint_weights[idx];
      int joint_idx = joint_indices[idx];

      if (stats) {
        original_weights_for_error.push_back(weight);
      }

      if (weight <= config.min_weight_threshold) {
        continue;  // Skip very small weights
      }

      int depth = 0;
      if (!bone_depths.empty() && joint_idx >= 0 && joint_idx < static_cast<int>(bone_depths.size())) {
        depth = bone_depths[static_cast<size_t>(joint_idx)];
      }

      influences.emplace_back(joint_idx, weight, depth, -1);
    }

    // Select bones based on strategy
    std::vector<BoneInfluence> selected;

    switch (config.strategy) {
      case BoneReductionStrategy::Greedy:
        ReduceGreedy(influences, config.target_bone_count, selected);
        break;

      case BoneReductionStrategy::Hierarchical:
        if (parent_indices_ptr) {
          ReduceHierarchical(influences, config.target_bone_count, *parent_indices_ptr,
                             selected);
        } else {
          ReduceGreedy(influences, config.target_bone_count, selected);
        }
        break;

      case BoneReductionStrategy::ErrorMetric:
        ReduceErrorMetric(influences, config.target_bone_count, config.error_tolerance,
                          selected);
        break;

      case BoneReductionStrategy::Adaptive:
        ReduceAdaptive(influences, config.target_bone_count, parent_indices_ptr,
                       config.error_tolerance, selected);
        break;
    }

    // Normalize weights if requested
    if (config.normalize_weights && !selected.empty()) {
      float weight_sum = 0.0f;
      for (const auto &inf : selected) {
        weight_sum += inf.weight;
      }

      if (weight_sum > 0.0f) {
        for (auto &inf : selected) {
          inf.weight /= weight_sum;
        }
      } else if (!selected.empty()) {
        // Fallback: equal weights
        float equal_weight = 1.0f / static_cast<float>(selected.size());
        for (auto &inf : selected) {
          inf.weight = equal_weight;
        }
      }
    }

    // Write to output
    for (size_t i = 0; i < selected.size() && i < config.target_bone_count; i++) {
      reduced_indices[dst_offset + i] = selected[i].joint_index;
      reduced_weights[dst_offset + i] = selected[i].weight;
    }

    // Track statistics
    if (stats) {
      std::vector<float> reduced_weights_for_error(config.target_bone_count, 0.0f);
      for (size_t i = 0; i < selected.size(); i++) {
        reduced_weights_for_error[i] = selected[i].weight;
      }

      float error = CalculateWeightError(original_weights_for_error, reduced_weights_for_error);
      total_error += error;
      max_error = std::max(max_error, error);

      if (selected.size() < influences.size()) {
        num_modified++;
      }
    }
  }

  // Swap output
  joint_indices.swap(reduced_indices);
  joint_weights.swap(reduced_weights);


  // Fill statistics
  if (stats) {
    stats->num_vertices = num_vertices;
    stats->original_bone_count = element_size;
    stats->target_bone_count = config.target_bone_count;
    stats->avg_weight_error = (num_vertices > 0) ? (total_error / static_cast<float>(num_vertices)) : 0.0f;
    stats->max_weight_error = max_error;
    stats->num_vertices_modified = num_modified;
  }

  return true;
}

}  // namespace tydra
}  // namespace lightusd
