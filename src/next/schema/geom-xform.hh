// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdGeomXform Schema
// Convenience API for Xform (transform) primitives

#pragma once

#include "../stage/stage.hh"
#include "../types/value.hh"
#include <vector>
#include <array>

namespace lightusd {
namespace next {

/// Transform operation types
enum class XformOpType {
  Translate,
  Rotate,     // Euler rotation
  RotateX,
  RotateY,
  RotateZ,
  Scale,
  Orient,     // Quaternion
  Transform,  // Full 4x4 matrix
  Unknown
};

/// Individual transform operation
struct XformOp {
  XformOpType type = XformOpType::Unknown;
  std::string suffix;  // Optional suffix (e.g., "pivot")
  Value value;
  bool is_inverse = false;
};

/// UsdGeomXform - Typed wrapper for Xform prims
/// Provides convenient accessors for transform operations
class UsdGeomXform {
public:
  /// Construct from UsdPrim
  explicit UsdGeomXform(const UsdPrim& prim);

  /// Check if this is a valid xformable prim
  bool IsValid() const { return prim_.IsValid(); }
  explicit operator bool() const { return IsValid(); }

  /// Get the underlying prim
  const UsdPrim& GetPrim() const { return prim_; }

  // ============================================================
  // Transform Operations
  // ============================================================

  /// Get the xformOpOrder (list of operations to apply)
  std::vector<std::string> GetXformOpOrder() const;

  /// Get all transform operations
  std::vector<XformOp> GetXformOps() const;

  /// Check if transform is animated
  bool HasAnimatedTransform() const;

  // ============================================================
  // Common Transform Accessors
  // ============================================================

  /// Get translation (if xformOp:translate exists)
  bool GetTranslation(float* x, float* y, float* z) const;
  bool GetTranslation(double* x, double* y, double* z) const;

  /// Get scale (if xformOp:scale exists)
  bool GetScale(float* x, float* y, float* z) const;

  /// Get rotation as Euler angles (if xformOp:rotateXYZ or similar exists)
  bool GetRotation(float* x, float* y, float* z) const;

  /// Get orientation quaternion (if xformOp:orient exists)
  bool GetOrientation(float* w, float* x, float* y, float* z) const;

  // ============================================================
  // Computed Transform
  // ============================================================

  /// Compute local transform matrix (4x4, row-major)
  bool ComputeLocalTransform(float* matrix) const;
  bool ComputeLocalTransform(double* matrix) const;

  /// Get translation at a timecode: linearly interpolates between
  /// bracketing time samples (pxr semantics), holds outside the sampled
  /// range, and falls back to the default value. NaN reads the default
  /// value only.
  bool GetTranslationAtTimecode(double timecode, float* x, float* y,
                                float* z) const;

private:
  UsdPrim prim_;

  XformOpType ParseOpType(const std::string& op_name) const;
};

/// Check if a prim is xformable (Xform, Mesh, etc.)

/// Check if a prim is specifically an Xform
bool IsXform(const UsdPrim& prim);

}  // namespace next
}  // namespace lightusd
