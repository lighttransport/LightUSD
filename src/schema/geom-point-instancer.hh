// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdGeomPointInstancer Schema
// Convenience API for PointInstancer primitives

#pragma once

#include "stage/stage.hh"
#include "types/value.hh"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

struct PointInstancerTransform {
  double matrix[16] = {
      1.0, 0.0, 0.0, 0.0,
      0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, 0.0, 0.0, 1.0};
};

/// UsdGeomPointInstancer - typed wrapper for PointInstancer prims.
class UsdGeomPointInstancer {
 public:
  explicit UsdGeomPointInstancer(const UsdPrim& prim);

  bool IsValid() const { return prim_.IsValid() && is_point_instancer_; }
  explicit operator bool() const { return IsValid(); }

  const UsdPrim& GetPrim() const { return prim_; }

  std::vector<Path> GetPrototypes() const;
  std::vector<int32_t> GetProtoIndices(double time = 0.0) const;
  std::vector<float> GetPositions(double time = 0.0) const;
  std::vector<float> GetOrientations(double time = 0.0) const;
  std::vector<float> GetScales(double time = 0.0) const;
  std::vector<float> GetVelocities(double time = 0.0) const;
  std::vector<float> GetAngularVelocities(double time = 0.0) const;
  std::vector<int64_t> GetIds(double time = 0.0) const;
  std::vector<int64_t> GetInvisibleIds(double time = 0.0) const;
  std::vector<int64_t> GetInactiveIds() const;

  size_t GetInstanceCount(double time = 0.0) const;
  bool HasValidInstanceArrays(double time = 0.0, std::string* reason = nullptr) const;

  /// Compute per-instance local transforms from positions, orientations
  /// (quatf/quath/quatd flattened as real,x,y,z), and scales. Missing
  /// orientations/scales default to identity.
  std::vector<PointInstancerTransform> ComputeInstanceTransforms(double time = 0.0) const;

  /// Per-instance visibility mask (size == GetInstanceCount): element `i` is
  /// false when instance `i`'s id appears in `invisibleIds` (at `time`) or
  /// `inactiveIds`. When `ids` is unauthored the id equals the instance index.
  std::vector<bool> ComputeMaskAtTime(double time = 0.0) const;

 private:
  std::vector<float> GetFloatArray(const char* name, double time) const;
  std::vector<int32_t> GetIntArray(const char* name, double time) const;
  std::vector<int64_t> GetInt64Array(const char* name, double time) const;

  UsdPrim prim_;
  bool is_point_instancer_ = false;
};

bool IsPointInstancer(const UsdPrim& prim);
std::vector<UsdGeomPointInstancer> GetAllPointInstancers(const Stage& stage);

}  // namespace next
}  // namespace lightusd
