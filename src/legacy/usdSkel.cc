// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// UsdSkel API implementations

#include "usdSkel.hh"

#include <sstream>
#include <cstdint>
#include <limits>

#include "common-macros.inc"
#include "tiny-format.hh"
#include "tiny-hashmap.hh"
#include "core/prim.hh"
#include "path-util.hh"

namespace lightusd {
namespace {

struct FNV1StringHash {
  size_t operator()(const std::string &s) const noexcept {
    static constexpr uint64_t kFNV_Prime = 0x00000100000001B3ull;
    static constexpr uint64_t kFNV_Offset_Basis = 0xcbf29ce484222325ull;

    uint64_t hash = kFNV_Offset_Basis;
    for (char ch : s) {
      hash = (kFNV_Prime * hash) ^ static_cast<unsigned char>(ch);
    }
    return static_cast<size_t>(hash);
  }
};

}  // namespace

constexpr auto kInbetweensNamespace = "inbetweens";

bool BlendShape::add_inbetweenBlendShape(const double weight, Attribute &&attr) {

  if (attr.name().empty()) {
    return false;
  }

  if (attr.is_uniform()) {
    return false;
  }

  if (!attr.is_value()) {
    return false;
  }

  std::string attr_name = fmt::format("{}:{}", kInbetweensNamespace, attr.name());
  attr.set_name(attr_name);

  attr.metas().set_weight(weight);

  props[attr_name] = Property(attr, /* custom */false);

  return true;
}

bool SkelAnimation::get_blendShapes(std::vector<value::token> *toks) {
  return blendShapes.get_value(toks);
}

bool SkelAnimation::get_joints(std::vector<value::token> *dst) {
  return joints.get_value(dst);
}

bool SkelAnimation::get_blendShapeWeights(
    std::vector<float> *vals, const double t,
    const value::TimeSampleInterpolationType tinterp) {
  Animatable<std::vector<float>> v;
  if (blendShapeWeights.get_value(&v)) {
    // Evaluate at time `t` with `tinterp` interpolation
    return v.get(t, vals, tinterp);
  }

  return false;
}

bool SkelAnimation::get_rotations(std::vector<value::quatf> *vals,
                                  const double t,
                                  const value::TimeSampleInterpolationType tinterp) {
  Animatable<std::vector<value::quatf>> v;
  if (rotations.get_value(&v)) {
    // Evaluate at time `t` with `tinterp` interpolation
    return v.get(t, vals, tinterp);
  }

  return false;
}

bool SkelAnimation::get_scales(std::vector<value::half3> *vals, const double t,
                               const value::TimeSampleInterpolationType tinterp) {
  Animatable<std::vector<value::half3>> v;
  if (scales.get_value(&v)) {
    // Evaluate at time `t` with `tinterp` interpolation
    return v.get(t, vals, tinterp);
  }

  return false;
}

bool SkelAnimation::get_translations(
    std::vector<value::float3> *vals, const double t,
    const value::TimeSampleInterpolationType tinterp) {
  Animatable<std::vector<value::float3>> v;
  if (translations.get_value(&v)) {
    // Evaluate at time `t` with `tinterp` interpolation
    return v.get(t, vals, tinterp);
  }

  return false;
}

bool SkelNormalizeWeights(std::vector<float> &weights,
                          int numInfluencesPerComponent, const float eps) {
  if (numInfluencesPerComponent < 1) {
    return false;
  }

  size_t numInfluences = size_t(numInfluencesPerComponent);
  if ((weights.size() % numInfluences) != 0) {
    return false;
  }

  size_t numComponents = weights.size() / numInfluences;

  for (size_t c = 0; c < numComponents; c++) {
    size_t offset = c * numInfluences;

    float sum = 0.0f;
    for (size_t i = 0; i < numInfluences; i++) {
      sum += weights[offset + i];
    }

    if (sum > eps) {
      float invSum = 1.0f / sum;
      for (size_t i = 0; i < numInfluences; i++) {
        weights[offset + i] *= invSum;
      }
    } else {
      // All weights are effectively zero; leave them as-is.
    }
  }

  return true;
}

bool SkelSortInfluences(std::vector<int> &indices, std::vector<float> &weights,
                         int numInfluencesPerComponent) {
  if (numInfluencesPerComponent < 1) {
    return false;
  }

  size_t numInfluences = size_t(numInfluencesPerComponent);
  if (indices.size() != weights.size()) {
    return false;
  }
  if ((indices.size() % numInfluences) != 0) {
    return false;
  }

  size_t numComponents = indices.size() / numInfluences;

  // Sort each group by weight descending using simple insertion sort
  // (groups are typically very small, e.g. 4 or 8 elements)
  for (size_t c = 0; c < numComponents; c++) {
    size_t offset = c * numInfluences;

    for (size_t i = 1; i < numInfluences; i++) {
      float keyW = weights[offset + i];
      int keyI = indices[offset + i];
      size_t j = i;
      while (j > 0 && weights[offset + j - 1] < keyW) {
        weights[offset + j] = weights[offset + j - 1];
        indices[offset + j] = indices[offset + j - 1];
        j--;
      }
      weights[offset + j] = keyW;
      indices[offset + j] = keyI;
    }
  }

  return true;
}

bool BuildSkelTopology(
  const std::vector<value::token> &joints,
  std::vector<int> &dst,
  std::string *err) {

  if (joints.empty()) {
    return true;
  }

  if (joints.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    if (err) (*err) += "Joint count exceeds the supported int index range.";
    return false;
  }

  std::vector<Path> paths(joints.size());
  for (size_t i = 0; i < joints.size(); i++) {
    Path p = Path(joints[i].str(), "");

    if (!p.is_valid()) {
      if (err) {
        (*err) += fmt::format("joints[{}] is invalid Prim path: `{}`", i, joints[i].str());
      }
      return false;
    }

    if (p.is_root_path()) {
      if (err) {
        (*err) += fmt::format("joints[{}] Root Prim path '/' cannot be used for joint Prim path.", i);
      }
      return false;
    }

    std::string _err;

    if (!pathutil::ValidatePrimPath(p, &_err)) {
      if (err) {
        (*err) += fmt::format("joints[{}] is not a valid Prim path: `{}`, reason = {}", i, joints[i].str(), _err);
      }
      return false;
    }

    paths[i] = p;
  }

  // path name <-> index map
  lightusd::HashMap<std::string, size_t, FNV1StringHash> pathMap;
  pathMap.reserve(paths.size());
  for (size_t i = 0; i < paths.size(); i++) {
    pathMap[paths[i].prim_part()] = i;
  }

  auto GetParentIndex = [](const lightusd::HashMap<std::string, size_t, FNV1StringHash> &_pathMap,
                           const Path &path) -> int {
    if (path.is_root_path()) {
      return -1;
    }

    // from pxrUSD's comment...
    //
    // Recurse over all ancestor paths, not just the direct parent.
    // For instance, if the map includes only paths 'a' and 'a/b/c',
    // 'a' will be treated as the parent of 'a/b/c'.
    //
    Path parentPath = path.get_parent_prim_path();

    uint32_t kMaxRec = 1024 * 128; // to avoid infinite loop.

    uint32_t depth = 0;
    while (parentPath.is_valid() && !parentPath.is_root_path()) {

      auto it = _pathMap.find(parentPath.prim_part());
      if (it != _pathMap.end()) {
        return static_cast<int>(it->second);
      }

      parentPath = parentPath.get_parent_prim_path();
      depth++;

      if (depth >= kMaxRec) {
        // TODO: Report error
        return -1;
      }
    }

    return -1;
  };

  dst.resize(joints.size());
  for (size_t i = 0; i < paths.size(); i++) {
    dst[i] = GetParentIndex(pathMap, paths[i]);
  }

  return true;
}

bool SkelValidateTopology(
  const std::vector<int> &topology,
  std::string *err) {

  if (topology.empty()) {
    return true;
  }

  int numJoints = int(topology.size());
  int numRoots = 0;

  for (int i = 0; i < numJoints; i++) {
    int parent = topology[size_t(i)];

    if (parent < 0) {
      numRoots++;
      continue;
    }

    if (parent >= numJoints) {
      if (err) {
        (*err) += fmt::format("Joint {} has out-of-range parent index {}.", i, parent);
      }
      return false;
    }

    if (parent >= i) {
      if (err) {
        (*err) += fmt::format("Joint {} has parent {} which is not ordered before it. "
                              "Parent indices must be less than child indices.", i, parent);
      }
      return false;
    }
  }

  if (numRoots == 0) {
    if (err) {
      (*err) += "No root joint found (no joint with parent index -1). Possible cycle.";
    }
    return false;
  }

  if (numRoots > 1) {
    if (err) {
      (*err) += fmt::format("Topology must be single-rooted but has {} roots.", numRoots);
    }
    return false;
  }

  return true;
}

}  // namespace lightusd
