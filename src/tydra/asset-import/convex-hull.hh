// SPDX-License-Identifier: Apache-2.0
// Dependency-free 3D convex hull for MJCF mesh assets that carry vertices but
// no faces. MuJoCo (mjCMesh::Process) builds the convex hull for such meshes
// (`face_.empty() -> MakeGraph/CopyGraph`); LightUSD must do the same so the
// collider, mass and inertia of a point-cloud mesh asset agree with the
// simulator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lightusd {
namespace tydra {
namespace import_asset {

enum class HullStatus {
  Ok,
  TooFewPoints,  // fewer than 4 distinct points
  Colocated,     // all points within tolerance of one point
  Collinear,     // all points on one line
  Coplanar,      // all points on one plane (no volume)
  Invalid,       // numerical failure; the caller should reject the mesh
};

// Convex hull of `count` points (3 floats each, xyz).
//
// On HullStatus::Ok, `triangles` holds 3 vertex indices per face, in the input
// point set, wound counter-clockwise seen from outside (positive volume).
// Coplanar input points are ignored; every input point lies inside or on the
// hull. `detail` (optional) receives a human-readable reason on failure.
HullStatus BuildConvexHull(const float* points, size_t count,
                           std::vector<int32_t>* triangles,
                           std::string* detail = nullptr);

}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
