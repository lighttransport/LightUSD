// SPDX-License-Identifier: Apache-2.0
#include "parametric_tess.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "light3d/math.h"

// Tessellation functions from the core library
#include "tydra/shape-to-mesh.hh"

namespace lusdview {

namespace {

using namespace lightusd::tydra;


// Convert ParamPrimType to the appropriate Generate*Mesh call.
// Returns true if tessellation was performed.
bool TessellatePrim(const ParametricPrim& prim, int segments,
                    std::vector<std::array<float, 3>>& points,
                    std::vector<int>& faceVertexCounts,
                    std::vector<int>& faceVertexIndices,
                    std::vector<std::array<float, 3>>& normals,
                    std::vector<std::array<float, 2>>& uvs) {
  switch (prim.type) {
    case ParamPrimType::Sphere: {
      // Adaptive: use icosphere with subdivisions based on segments
      int subdivs = std::max(1, std::min(segments / 4, 6));
      GenerateIcosphereMesh(prim.params.sphere.radius, subdivs,
                            points, faceVertexCounts, faceVertexIndices, normals, uvs);
      return true;
    }
    case ParamPrimType::Cube: {
      // Cube doesn't benefit from re-tessellation (always 8 verts)
      // But we can subdivide faces for smoother shading
      GenerateCubeMesh(prim.params.cube.size,
                       points, faceVertexCounts, faceVertexIndices, normals, uvs);
      return true;
    }
    case ParamPrimType::Cylinder: {
      int heightSegs = std::max(1, segments / 8);
      GenerateCylinderMesh(prim.params.cylinder.radius, prim.params.cylinder.height,
                           segments, heightSegs,
                           points, faceVertexCounts, faceVertexIndices, normals, uvs);
      return true;
    }
    case ParamPrimType::Cone: {
      GenerateConeMesh(prim.params.cone.radius, prim.params.cone.height,
                       segments,
                       points, faceVertexCounts, faceVertexIndices, normals, uvs);
      return true;
    }
    case ParamPrimType::Capsule: {
      int heightSegs = std::max(1, segments / 8);
      GenerateCapsuleMesh(prim.params.capsule.radius, prim.params.capsule.height,
                          segments, heightSegs,
                          points, faceVertexCounts, faceVertexIndices, normals, uvs);
      return true;
    }
    case ParamPrimType::Plane: {
      int segs = std::max(1, segments / 4);
      GeneratePlaneMesh(prim.params.plane.width, prim.params.plane.length,
                        segs, segs,
                        points, faceVertexCounts, faceVertexIndices, normals, uvs);
      return true;
    }
  }
  return false;
}

// Build a DrawMeshCPU from tessellated parametric primitive data.
bool BuildDrawMeshFromParam(const ParametricPrim& prim, int segments,
                            DrawMeshCPU* out) {
  std::vector<std::array<float, 3>> points;
  std::vector<int> faceVertexCounts;
  std::vector<int> faceVertexIndices;
  std::vector<std::array<float, 3>> normals;
  std::vector<std::array<float, 2>> uvs;

  if (!TessellatePrim(prim, segments, points, faceVertexCounts,
                      faceVertexIndices, normals, uvs)) {
    return false;
  }

  DrawMeshCPU dm;
  dm.name = prim.absPath;
  dm.absPath = prim.absPath;

  // Convert tessellated output to DrawVertex format
  // Tessellators output quads/tris with face-varying data.
  // We expand to single-indexed by duplicating vertices per face-vertex.
  dm.vertices.reserve(faceVertexIndices.size());
  dm.indices.reserve(faceVertexIndices.size());

  for (size_t k = 0; k < faceVertexIndices.size(); ++k) {
    const int pidx = faceVertexIndices[k];
    DrawVertex v;
    if (pidx >= 0 && static_cast<size_t>(pidx) < points.size()) {
      v.px = points[pidx][0];
      v.py = points[pidx][1];
      v.pz = points[pidx][2];
    } else {
      v.px = v.py = v.pz = 0.0f;
    }
    if (k < normals.size()) {
      v.nx = normals[k][0];
      v.ny = normals[k][1];
      v.nz = normals[k][2];
    } else {
      v.nx = v.ny = v.nz = 0.0f;
    }
    if (k < uvs.size()) {
      v.u = uvs[k][0];
      v.v = uvs[k][1];
    } else {
      v.u = v.v = 0.0f;
    }
    dm.vertices.push_back(v);
    dm.indices.push_back(static_cast<uint32_t>(k));
  }

  // Single submesh with default material
  DrawSubmesh sub;
  sub.indexOffset = 0;
  sub.indexCount = static_cast<uint32_t>(dm.indices.size());
  sub.materialId = -1;
  dm.submeshes.push_back(sub);

  // Apply world transform
  std::memcpy(dm.world, prim.world, sizeof(float) * 16);

  // Compute AABB
  float lmin[3] = {1e30f, 1e30f, 1e30f};
  float lmax[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& v : dm.vertices) {
    lmin[0] = std::min(lmin[0], v.px);
    lmin[1] = std::min(lmin[1], v.py);
    lmin[2] = std::min(lmin[2], v.pz);
    lmax[0] = std::max(lmax[0], v.px);
    lmax[1] = std::max(lmax[1], v.py);
    lmax[2] = std::max(lmax[2], v.pz);
  }
  // Transform AABB to world space
  light3d::Mat4 W;
  std::memcpy(W.m, prim.world, sizeof(float) * 16);
  float wmin[3] = {1e30f, 1e30f, 1e30f};
  float wmax[3] = {-1e30f, -1e30f, -1e30f};
  for (int corner = 0; corner < 8; ++corner) {
    light3d::Vec3 lp{(corner & 1) ? lmax[0] : lmin[0],
                     (corner & 2) ? lmax[1] : lmin[1],
                     (corner & 4) ? lmax[2] : lmin[2]};
    light3d::Vec3 wp = light3d::transformPoint(W, lp);
    wmin[0] = std::min(wmin[0], wp.x);
    wmin[1] = std::min(wmin[1], wp.y);
    wmin[2] = std::min(wmin[2], wp.z);
    wmax[0] = std::max(wmax[0], wp.x);
    wmax[1] = std::max(wmax[1], wp.y);
    wmax[2] = std::max(wmax[2], wp.z);
  }
  for (int c = 0; c < 3; ++c) {
    out->aabbMin[c] = wmin[c];
    out->aabbMax[c] = wmax[c];
  }

  *out = std::move(dm);
  return true;
}

}  // namespace

int AdaptiveTessellator::addPrimitive(const ParametricPrim& prim) {
  int idx = static_cast<int>(parametrics_.size());
  pathMap_[prim.absPath] = idx;
  parametrics_.push_back(prim);
  return idx;
}

int AdaptiveTessellator::computeTessLevel(const ParametricPrim& prim,
                                          const light3d::Vec3& cameraPos,
                                          float qualityScale) {
  // Compute world-space position of the primitive center
  light3d::Mat4 W;
  std::memcpy(W.m, prim.world, sizeof(float) * 16);
  light3d::Vec3 center = light3d::transformPoint(W, {0, 0, 0});

  // Distance from camera to primitive center
  light3d::Vec3 diff = cameraPos - center;
  float dist = light3d::length(diff);

  // Estimate primitive bounding radius from parameters
  float boundRadius = 1.0f;
  switch (prim.type) {
    case ParamPrimType::Sphere:
      boundRadius = prim.params.sphere.radius;
      break;
    case ParamPrimType::Cube:
      boundRadius = prim.params.cube.size * 0.866f;  // half-diagonal
      break;
    case ParamPrimType::Cylinder:
      boundRadius = std::max(prim.params.cylinder.radius,
                             prim.params.cylinder.height * 0.5f);
      break;
    case ParamPrimType::Cone:
      boundRadius = std::max(prim.params.cone.radius,
                             prim.params.cone.height * 0.5f);
      break;
    case ParamPrimType::Capsule:
      boundRadius = std::max(prim.params.capsule.radius,
                             prim.params.capsule.height * 0.5f + prim.params.capsule.radius);
      break;
    case ParamPrimType::Plane:
      boundRadius = std::max(prim.params.plane.width,
                             prim.params.plane.length) * 0.707f;
      break;
  }

  // Screen-space coverage estimate:
  // Assume ~1000px viewport height, 60-degree vertical FOV
  float fovRad = 1.047f;  // ~60 degrees
  float screenHeight = 1000.0f;
  float apparentSize = (2.0f * boundRadius * screenHeight) /
                       (2.0f * dist * std::tan(fovRad * 0.5f));
  // apparentSize is approximate pixel height of the primitive

  // Map pixel height to tessellation segments:
  //   < 20px  -> 4 segments (very distant)
  //   < 50px  -> 8 segments
  //   < 100px -> 12 segments
  //   < 200px -> 16 segments
  //   < 500px -> 24 segments
  //   >= 500px -> 32 segments (close up)
  int segments;
  if (apparentSize < 20.0f) segments = 4;
  else if (apparentSize < 50.0f) segments = 8;
  else if (apparentSize < 100.0f) segments = 12;
  else if (apparentSize < 200.0f) segments = 16;
  else if (apparentSize < 500.0f) segments = 24;
  else segments = 32;

  // Apply user quality scale
  segments = std::max(3, static_cast<int>(static_cast<float>(segments) * qualityScale));
  segments = std::min(segments, 64);  // cap at 64

  // Ensure minimum for different primitive types
  switch (prim.type) {
    case ParamPrimType::Sphere:
    case ParamPrimType::Cylinder:
    case ParamPrimType::Cone:
    case ParamPrimType::Capsule:
      segments = std::max(segments, 4);
      break;
    case ParamPrimType::Plane:
      segments = std::max(segments, 1);
      break;
    case ParamPrimType::Cube:
      segments = 1;  // cubes don't benefit from re-tessellation
      break;
  }

  return segments;
}

bool AdaptiveTessellator::retessellate(const light3d::Vec3& cameraPos,
                                       float qualityScale,
                                       DrawScene* draw) {
  if (!draw || parametrics_.empty()) return false;

  bool anyUpdated = false;

  for (auto& prim : parametrics_) {
    if (prim.drawMeshIndex < 0 ||
        static_cast<size_t>(prim.drawMeshIndex) >= draw->meshes.size()) {
      continue;
    }

    int newSegments = computeTessLevel(prim, cameraPos, qualityScale);
    if (newSegments == prim.currentSegments) continue;

    // Re-tessellate
    DrawMeshCPU newMesh;
    if (BuildDrawMeshFromParam(prim, newSegments, &newMesh)) {
      // Preserve material assignment from the original mesh
      if (!draw->meshes[prim.drawMeshIndex].submeshes.empty()) {
        newMesh.submeshes[0].materialId =
            draw->meshes[prim.drawMeshIndex].submeshes[0].materialId;
      }

      draw->meshes[prim.drawMeshIndex] = std::move(newMesh);
      prim.currentSegments = newSegments;
      anyUpdated = true;
    }
  }

  return anyUpdated;
}

void AdaptiveTessellator::clear() {
  parametrics_.clear();
  pathMap_.clear();
}

const ParametricPrim* AdaptiveTessellator::getPrimitive(size_t index) const {
  if (index >= parametrics_.size()) return nullptr;
  return &parametrics_[index];
}

}  // namespace lusdview
