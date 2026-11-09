#include "pick.hh"

#include "d3d12/mathutil.hh"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ld3d12 {
namespace {

const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// Skinned vertices are already in world space; everything else uses the draw's
// world matrix.
const float* WorldFor(const lmv_mesh_view& mv, const lmv_draw_view& dv) {
  return mv.skinned ? kIdentity : dv.world;
}

// Slab test; true when the ray overlaps [bmin, bmax] for some t >= 0.
bool RayBox(const float o[3], const float d[3], const float bmin[3],
            const float bmax[3]) {
  float t0 = 0.0f, t1 = 1e30f;
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(d[i]) < 1e-20f) {
      if (o[i] < bmin[i] || o[i] > bmax[i]) return false;
      continue;
    }
    float a = (bmin[i] - o[i]) / d[i], b = (bmax[i] - o[i]) / d[i];
    if (a > b) std::swap(a, b);
    t0 = std::max(t0, a);
    t1 = std::min(t1, b);
    if (t0 > t1) return false;
  }
  return true;
}

// Moller-Trumbore, two-sided. Returns t > eps or a negative value on a miss.
float RayTriangle(const float o[3], const float d[3], const lmv_vertex& a,
                  const lmv_vertex& b, const lmv_vertex& c) {
  const float e1[3] = {b.px - a.px, b.py - a.py, b.pz - a.pz};
  const float e2[3] = {c.px - a.px, c.py - a.py, c.pz - a.pz};
  const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                      d[0] * e2[1] - d[1] * e2[0]};
  const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::fabs(det) < 1e-20f) return -1.0f;
  const float inv = 1.0f / det;
  const float s[3] = {o[0] - a.px, o[1] - a.py, o[2] - a.pz};
  const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0.0f || u > 1.0f) return -1.0f;
  const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                      s[0] * e1[1] - s[1] * e1[0]};
  const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0.0f || u + v > 1.0f) return -1.0f;
  const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
  return t > 1e-6f ? t : -1.0f;
}

int MaterialOf(const lmv_mesh_view& mv, const lmv_draw_view& dv, uint32_t tri) {
  if (dv.material_override >= 0) return dv.material_override;
  const uint32_t first = tri * 3;
  for (uint32_t s = 0; s < mv.subset_count; ++s) {
    const lmv_subset& sub = mv.subsets[s];
    if (first >= sub.first_index && first < sub.first_index + sub.index_count &&
        sub.material_id >= 0)
      return sub.material_id;
  }
  return mv.material_id;
}

std::string Format(const char* fmt, ...) {
  char buf[320];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return buf;
}

const char* ShaderName(uint32_t type) {
  switch (type) {
    case 1: return "UsdPreviewSurface";
    case 2: return "OpenPBR";
    default: return "no shader";
  }
}


// Fills prim path, material and the inspector lines for best.draw / best.mesh
// (and the hit triangle / point when `with_hit`).
void Describe(const lmv_scene* scene, PickResult* best, bool with_hit) {
  lmv_draw_view dv = {};
  lmv_mesh_view mv = {};
  lmv_scene_draw(scene, static_cast<uint32_t>(best->draw), &dv);
  lmv_scene_mesh(scene, static_cast<uint32_t>(best->mesh), &mv);
  best->prim_path = mv.prim_path ? mv.prim_path : "";
  best->material = MaterialOf(mv, dv, with_hit ? best->triangle : 0);

  best->lines.push_back(!best->prim_path.empty()
                            ? best->prim_path
                            : std::string(mv.name ? mv.name : "(unnamed)"));
  if (with_hit) {
    best->lines.push_back(Format("mesh %d   triangle %u   %u verts, %u tris%s",
                                 best->mesh, best->triangle, mv.vertex_count,
                                 mv.index_count / 3,
                                 mv.skinned ? "   skinned (rest pose)" : ""));
  } else {
    best->lines.push_back(Format("mesh %d   %u verts, %u tris%s", best->mesh,
                                 mv.vertex_count, mv.index_count / 3,
                                 mv.skinned ? "   skinned (rest pose)" : ""));
  }
  lmv_material_view mat = {};
  if (best->material >= 0 &&
      lmv_scene_material(scene, static_cast<uint32_t>(best->material), &mat) == LMV_OK &&
      mat.pbr) {
    const char* label = (mat.prim_path && *mat.prim_path)
                            ? mat.prim_path
                            : (mat.name && *mat.name ? mat.name : "(unnamed)");
    best->lines.push_back(Format("material %s  [%s]", label, ShaderName(mat.shader_type)));
    best->lines.push_back(Format(
        "base (%.2f %.2f %.2f)  metal %.2f  rough %.2f%s",
        mat.pbr[LMV_PBR_BASE_COLOR], mat.pbr[LMV_PBR_BASE_COLOR + 1],
        mat.pbr[LMV_PBR_BASE_COLOR + 2], mat.pbr[LMV_PBR_METALNESS],
        mat.pbr[LMV_PBR_SPECULAR_ROUGHNESS],
        mat.alpha_mode == 2 ? "  blended" : (mat.alpha_mode == 1 ? "  masked" : "")));
  } else {
    best->lines.push_back("material (none: default surface)");
  }
  if (with_hit)
    best->lines.push_back(Format("hit (%.3f, %.3f, %.3f)   distance %.3f",
                                 best->point[0], best->point[1], best->point[2],
                                 best->distance));
}

}  // namespace

PickResult PickScene(const lmv_scene* scene, const float origin[3],
                     const float dir[3]) {
  PickResult best;
  if (!scene) return best;
  float best_t = 1e30f;
  const uint32_t draws = lmv_scene_draw_count(scene);
  for (uint32_t di = 0; di < draws; ++di) {
    lmv_draw_view dv = {};
    lmv_mesh_view mv = {};
    if (lmv_scene_draw(scene, di, &dv) != LMV_OK || !dv.visible || dv.mesh_id < 0 ||
        !dv.world || lmv_scene_mesh(scene, static_cast<uint32_t>(dv.mesh_id), &mv) != LMV_OK ||
        !mv.vertices || !mv.indices)
      continue;
    float inv[16];
    if (!Invert4x4(WorldFor(mv, dv), inv)) continue;
    float lo[3], ld[3];  // ray in mesh space (ld is not normalized: same t)
    TransformPoint(inv, origin, lo);
    TransformDir(inv, dir, ld);
    if (!RayBox(lo, ld, mv.bbox_min, mv.bbox_max)) continue;
    const uint32_t tris = mv.index_count / 3;
    for (uint32_t t = 0; t < tris; ++t) {
      const uint32_t i0 = mv.indices[t * 3], i1 = mv.indices[t * 3 + 1],
                     i2 = mv.indices[t * 3 + 2];
      if (i0 >= mv.vertex_count || i1 >= mv.vertex_count || i2 >= mv.vertex_count)
        continue;
      const float hit_t = RayTriangle(lo, ld, mv.vertices[i0], mv.vertices[i1],
                                      mv.vertices[i2]);
      if (hit_t > 0.0f && hit_t < best_t) {
        best_t = hit_t;
        best.hit = true;
        best.draw = static_cast<int>(di);
        best.mesh = dv.mesh_id;
        best.triangle = t;
      }
    }
  }
  if (!best.hit) return best;

  best.distance = best_t;
  for (int i = 0; i < 3; ++i) best.point[i] = origin[i] + dir[i] * best_t;
  Describe(scene, &best, /*with_hit=*/true);
  return best;
}

PickResult DescribeDraw(const lmv_scene* scene, int draw) {
  PickResult r;
  lmv_draw_view dv = {};
  if (!scene || draw < 0 ||
      lmv_scene_draw(scene, static_cast<uint32_t>(draw), &dv) != LMV_OK ||
      dv.mesh_id < 0)
    return r;
  r.hit = true;
  r.draw = draw;
  r.mesh = dv.mesh_id;
  Describe(scene, &r, /*with_hit=*/false);
  return r;
}

bool DrawWorldBox(const lmv_scene* scene, int draw, float corners[8][3]) {
  if (!scene || draw < 0) return false;
  lmv_draw_view dv = {};
  lmv_mesh_view mv = {};
  if (lmv_scene_draw(scene, static_cast<uint32_t>(draw), &dv) != LMV_OK ||
      dv.mesh_id < 0 || !dv.world ||
      lmv_scene_mesh(scene, static_cast<uint32_t>(dv.mesh_id), &mv) != LMV_OK)
    return false;
  const float* world = WorldFor(mv, dv);
  for (int c = 0; c < 8; ++c) {
    const float p[3] = {(c & 1) ? mv.bbox_max[0] : mv.bbox_min[0],
                        (c & 2) ? mv.bbox_max[1] : mv.bbox_min[1],
                        (c & 4) ? mv.bbox_max[2] : mv.bbox_min[2]};
    TransformPoint(world, p, corners[c]);
  }
  return true;
}

void AppendBoxEdges(const float corners[8][3], float width, const float rgba[4],
                    std::vector<float>* out) {
  for (int c = 0; c < 8; ++c) {
    for (int axis = 0; axis < 3; ++axis) {
      const int bit = 1 << axis;
      if (c & bit) continue;  // each edge once, from its low corner
      const float* a = corners[c];
      const float* b = corners[c | bit];
      out->insert(out->end(), {a[0], a[1], a[2], b[0], b[1], b[2], width, rgba[0],
                               rgba[1], rgba[2], rgba[3]});
    }
  }
}

}  // namespace ld3d12
