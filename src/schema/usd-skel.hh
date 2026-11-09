// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdSkel Schema
// Skeleton, SkelAnimation, BlendShape, SkelRoot convenience wrappers

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"
#include <vector>
#include <string>

namespace lightusd {
namespace next {

// ============================================================
// Prim type checking (free functions)
// ============================================================

bool IsSkelRoot(const UsdPrim& prim);
bool IsSkeleton(const UsdPrim& prim);
bool IsSkelAnimation(const UsdPrim& prim);
bool IsBlendShape(const UsdPrim& prim);

// ============================================================
// Skeleton data extracted from a Skeleton prim
// ============================================================

struct SkeletonData {
  // Joint hierarchy (uniform token[])
  std::vector<std::string> joints;
  std::vector<std::string> jointNames;

  // Transforms (uniform matrix4d[])
  std::vector<double> bindTransforms;   // 16 doubles per joint, row-major
  std::vector<double> restTransforms;   // 16 doubles per joint, row-major

  // Binding
  std::string animationSource;  // rel skel:animationSource

  bool hasAnimationSource = false;
};

/// Get skeleton data from a Skeleton prim
bool GetSkeletonData(const Stage& stage, const UsdPrim& prim,
                     SkeletonData* out);

// ============================================================
// SkelAnimation data
// ============================================================

struct SkelAnimationData {
  // Joint animation data (float3[] or quatf[] per joint)
  std::vector<float> translations;     // float3[] xyz interleaved
  std::vector<float> rotations;        // quatf[] wxyz interleaved
  std::vector<float> scales;           // half3[] stored as float for convenience
  std::vector<float> blendShapeWeights; // float[]

  // Blend shape names (uniform token[])
  std::vector<std::string> blendShapes;

  // Joint names (uniform token[])
  std::vector<std::string> joints;

  bool hasTranslations = false;
  bool hasRotations = false;
  bool hasScales = false;
  bool hasBlendShapes = false;
};

/// Get SkelAnimation data at a given time
bool GetSkelAnimationData(const Stage& stage, const UsdPrim& prim,
                          SkelAnimationData* out, double time = 0.0);

/// Get SkelAnimation data with time sample interpolation
bool GetSkelAnimationDataAtTime(const Stage& stage, const UsdPrim& prim,
                                SkelAnimationData* out, double time);

// ============================================================
// BlendShape data
// ============================================================

struct BlendShapeData {
  struct Inbetween {
    std::string name;
    float weight = 0.0f;
    // True when the `weight` attribute metadata was authored. An in-between
    // without an authored weight is unusable (0.0 collides with the base
    // shape); consumers should skip it (matches legacy tydra).
    bool has_weight = false;
    std::vector<float> offsets;
  };
  // Offsets (vector3f[])
  std::vector<float> offsets;       // xyz interleaved
  std::vector<float> normalOffsets; // xyz interleaved

  // Point indices (int[])
  std::vector<int32_t> pointIndices;

  bool hasNormalOffsets = false;
  bool hasPointIndices = false;
  std::vector<Inbetween> inbetweens;
};

/// Get BlendShape data
bool GetBlendShapeData(const Stage& stage, const UsdPrim& prim,
                       BlendShapeData* out);

// ============================================================
// SkelRoot data
// ============================================================

struct SkelRootData {
  std::string skeleton;         // rel skel:skeleton
  std::string animationSource;  // rel skel:animationSource

  bool hasSkeleton = false;
  bool hasAnimationSource = false;
};

/// Get SkelRoot data
bool GetSkelRootData(const Stage& stage, const UsdPrim& prim,
                     SkelRootData* out);

// ============================================================
// Utility functions
// ============================================================

/// Build parent index array from Skeleton joint paths.
/// dst[i] = parent joint index, -1 for root.
bool BuildSkelTopology(const std::vector<std::string>& joints,
                       std::vector<int>& dst, std::string* err);

/// Validate skeleton topology (single root, no cycles, valid parent indices).
bool SkelValidateTopology(const std::vector<int>& topology,
                          std::string* err);

/// Normalize skinning weights so each group sums to 1.0.
bool SkelNormalizeWeights(std::vector<float>& weights,
                          int numInfluencesPerComponent,
                          float eps = 1e-6f);

/// Sort joint indices and weights per component by weight descending.
bool SkelSortInfluences(std::vector<int>& indices,
                        std::vector<float>& weights,
                        int numInfluencesPerComponent);

} // namespace next
} // namespace lightusd
