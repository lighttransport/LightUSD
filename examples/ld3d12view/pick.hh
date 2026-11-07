// ld3d12view - CPU ray picking and the selection inspector.
//
// Casts a world-space ray against the visible draws of a scene (bounding-box
// slab test, then Moller-Trumbore on the triangles) and describes what was hit:
// prim path, mesh, material and its main OpenPBR values. Skinned meshes are
// tested at their rest pose (the core stores them in world space already).
#pragma once

#include "lmv.h"

#include <string>
#include <vector>

namespace ld3d12 {

struct PickResult {
  bool hit = false;
  int draw = -1;          // index for lmv_scene_draw
  int mesh = -1;
  uint32_t triangle = 0;  // triangle index inside the mesh
  float distance = 0.0f;  // along the (unit) ray direction
  float point[3] = {};    // world-space hit position
  int material = -1;
  std::string prim_path;
  // Inspector text, one line per entry (empty when nothing was hit).
  std::vector<std::string> lines;
};

// `dir` must be a unit vector.
PickResult PickScene(const lmv_scene* scene, const float origin[3],
                     const float dir[3]);

// Describes `draw` as if it had been picked, without a hit point (the outliner
// uses this). hit is false for an invalid draw.
PickResult DescribeDraw(const lmv_scene* scene, int draw);

// World-space corners of `draw`'s bounding box at the scene's current pose
// (corner index bits: 1 = +x, 2 = +y, 4 = +z). False if the draw is invalid.
bool DrawWorldBox(const lmv_scene* scene, int draw, float corners[8][3]);

// The 12 box edges as raw lmv_line_segment floats (11 each: p0, p1, width,
// rgba), appended to `out`.
void AppendBoxEdges(const float corners[8][3], float width, const float rgba[4],
                    std::vector<float>* out);

}  // namespace ld3d12
