// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdGeomMesh Schema
// Convenience API for Mesh primitives

#pragma once

#include "stage/stage.hh"
#include "types/value.hh"
#include <vector>

namespace lightusd {

/// UsdGeomMesh - Typed wrapper for Mesh prims
/// Provides convenient accessors for mesh-specific properties
class UsdGeomMesh {
public:
  /// Construct from UsdPrim (prim must be a Mesh type)
  explicit UsdGeomMesh(const UsdPrim& prim);

  /// Check if this is a valid mesh
  bool IsValid() const { return prim_.IsValid() && is_mesh_; }
  explicit operator bool() const { return IsValid(); }

  /// Get the underlying prim
  const UsdPrim& GetPrim() const { return prim_; }

  // ============================================================
  // Topology
  // ============================================================

  /// Get face vertex counts (number of vertices per face)
  std::vector<int> GetFaceVertexCounts() const;

  /// Get face vertex indices
  std::vector<int> GetFaceVertexIndices() const;

  /// Get total face count
  size_t GetFaceCount() const;

  /// Get total vertex count (from points array)
  size_t GetPointCount() const;

  // ============================================================
  // Geometry
  // ============================================================

  /// Get points (vertex positions)
  std::vector<float> GetPoints() const;

  /// Get points as vec3 array (x, y, z triplets)
  bool GetPoints(std::vector<float>& x, std::vector<float>& y, std::vector<float>& z) const;

  /// Get normals (if present)
  std::vector<float> GetNormals() const;

  /// Check if normals are present
  bool HasNormals() const;

  /// Get extent (bounding box)
  bool GetExtent(float* min, float* max) const;

  // ============================================================
  // Primvars (UV coordinates, etc.)
  // ============================================================

  /// Get UV coordinates (primvars:st)
  std::vector<float> GetUVs() const;

  /// Check if UVs are present
  bool HasUVs() const;

  /// Get UV indices (if indexed)
  std::vector<int> GetUVIndices() const;

  // ============================================================
  // Subdivision
  // ============================================================

  /// Get subdivision scheme ("none", "catmullClark", "loop", "bilinear")
  std::string GetSubdivisionScheme() const;

  /// Check if this is a subdivision surface
  bool IsSubdivisionSurface() const;

  // ============================================================
  // Time-varying data
  // ============================================================

  /// Get points at a timecode: linearly interpolates between bracketing
  /// time samples (pxr semantics), holds outside the sampled range, and
  /// falls back to the default value. NaN reads the default value only.
  std::vector<float> GetPointsAtTimecode(double timecode) const;

  /// Check if points are animated
  bool HasAnimatedPoints() const;

  /// Get time sample times for points
  std::vector<double> GetPointsTimeSamples() const;

private:
  UsdPrim prim_;
  bool is_mesh_ = false;
};

/// Check if a prim is a Mesh
bool IsMesh(const UsdPrim& prim);

/// Get all meshes in stage
std::vector<UsdGeomMesh> GetAllMeshes(const Stage& stage);

}  // namespace lightusd
