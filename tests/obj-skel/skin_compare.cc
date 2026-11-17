// skin_compare SCENE MESH_PRIM FBX TOL FRAME...
//
// Deformed points of MESH_PRIM in SCENE as LightUSD evaluates them (UsdSkel
// blend shapes, then Tydra EvaluateSkinnedPoints) against ufbx's own
// deformation of the FBX node of the same name at the same frames (blend
// offsets, then sum of weight * cluster.geometry_to_world * vertex on the
// evaluated scene). Point sets are compared by nearest neighbour in both
// directions, so the mesh may come from the FBX itself or from an OBJ whose
// skeleton/animation come from the FBX (skel_lib/skel_anim). Exits 1 when the
// largest distance at some frame exceeds TOL.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "external/ufbx/ufbx.h"
#include "load-usd.hh"
#include "stage/stage.hh"
#include "schema/usd-skel.hh"
#include "tydra/render-extract.hh"
#include "tydra/scene-access.hh"

namespace {
double MaxNearest(const std::vector<float>& a, const std::vector<float>& b) {
  double worst = 0;
  for (size_t i = 0; i + 2 < a.size(); i += 3) {
    double best = 1e300;
    for (size_t j = 0; j + 2 < b.size(); j += 3) {
      const double dx = a[i] - b[j], dy = a[i + 1] - b[j + 1], dz = a[i + 2] - b[j + 2];
      best = std::min(best, dx * dx + dy * dy + dz * dz);
    }
    worst = std::max(worst, std::sqrt(best));
  }
  return worst;
}
// UsdSkel blend shapes on `points` (UsdSkelBlendShapeQuery semantics): a
// shape's weight w is piecewise linear over (0, base), (w_i, in-between i),
// (1, full), extrapolated by the end segments. Weights come from the
// skeleton's SkelAnimation (matched by token). Returns false on bad data.
bool Morph(const lightusd::Stage& stage, const lightusd::UsdPrim& mesh, double time,
           std::vector<float>* points) {
  namespace n = lightusd;
  const auto shapes = n::tydra::GetBlendShapes(mesh);
  if (shapes.empty()) return true;
  n::UsdPrim skel;
  for (auto p = mesh; p.IsValid() && !skel.IsValid(); p = p.GetParent()) {
    const auto* t = p.GetRelationship("skel:skeleton");
    if (t && !t->empty()) skel = stage.GetPrimAtPath((*t)[0]);
  }
  if (!skel.IsValid()) return false;
  const auto* src = skel.GetRelationship("skel:animationSource");
  if (!src || src->empty()) return true;  // no weights: rest shape
  n::SkelAnimationData anim;
  if (!n::GetSkelAnimationDataAtTime(stage, stage.GetPrimAtPath((*src)[0]), &anim, time))
    return false;
  for (const auto& info : shapes) {
    size_t k = 0;
    while (k < anim.blendShapes.size() && anim.blendShapes[k] != info.name) ++k;
    if (k >= anim.blendShapes.size() || k >= anim.blendShapeWeights.size()) continue;
    const float w = anim.blendShapeWeights[k];
    n::BlendShapeData bs;
    if (!n::GetBlendShapeData(stage, stage.GetPrimAtPath(info.path), &bs)) return false;
    std::vector<std::pair<float, const std::vector<float>*>> knots;  // weight -> offsets
    const std::vector<float> zero(bs.offsets.size(), 0.0f);
    knots.push_back({0.0f, &zero});
    for (const auto& in : bs.inbetweens)
      if (in.has_weight && in.offsets.size() == bs.offsets.size()) knots.push_back({in.weight, &in.offsets});
    knots.push_back({1.0f, &bs.offsets});
    size_t seg = 0;  // segment [seg, seg+1] containing w (end segments extrapolate)
    while (seg + 2 < knots.size() && w > knots[seg + 1].first) ++seg;
    const float w0 = knots[seg].first, w1 = knots[seg + 1].first;
    const float u = w1 > w0 ? (w - w0) / (w1 - w0) : 0.0f;
    const size_t np = bs.offsets.size() / 3;
    for (size_t i = 0; i < np; ++i) {
      const size_t v = bs.hasPointIndices ? size_t(bs.pointIndices[i]) : i;
      if (3 * v + 2 >= points->size()) return false;
      for (int c = 0; c < 3; ++c) {
        const float a = (*knots[seg].second)[3 * i + c], b = (*knots[seg + 1].second)[3 * i + c];
        (*points)[3 * v + c] += a + u * (b - a);
      }
    }
  }
  return true;
}

// Blend shapes, then skinning (unskinned meshes keep the morphed points).
bool Deform(const lightusd::Stage& stage, const lightusd::UsdPrim& mesh, double time,
            const lightusd::tydra::ValueArrayRead<float>& points, std::vector<float>* out,
            std::string* err) {
  std::vector<float> morphed(points.begin(), points.end());
  if (!Morph(stage, mesh, time, &morphed)) {
    *err = "invalid blend shapes";
    return false;
  }
  if (!mesh.GetPropertyValue("primvars:skel:jointIndices")) {
    *out = std::move(morphed);
    return true;
  }
  lightusd::tydra::ValueArrayRead<float> m;
  m.view.data = morphed.data();
  m.view.size = morphed.size();
  return lightusd::tydra::EvaluateSkinnedPoints(stage, mesh, time, m, out, err);
}
}  // namespace

// --dump SCENE TIME: `<prim path> <float count> x y z ...` per skinned mesh, the
// native dump of tools/view_control/skinning_compare.py (pxr UsdSkel.Cache).
static int Dump(const char* scene, double time) {
  lightusd::Stage stage;
  std::string warn, err;
  if (!lightusd::LoadUSDComposed(scene, &stage, &warn, &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  std::vector<lightusd::UsdPrim> todo = stage.GetRootPrims();
  while (!todo.empty()) {
    lightusd::UsdPrim prim = todo.back();
    todo.pop_back();
    for (const auto& c : prim.GetChildren()) todo.push_back(c);
    if (prim.GetTypeName() != "Mesh") continue;
    if (!prim.GetPropertyValue("primvars:skel:jointIndices") &&
        !prim.GetPropertyValue("skel:blendShapes"))
      continue;  // not deformed
    lightusd::tydra::ValueArrayRead<float> points;
    if (!lightusd::tydra::ReadFloatArray(prim, "points", time, &points)) continue;
    std::vector<float> got;
    if (!Deform(stage, prim, time, points, &got, &err)) {
      std::fprintf(stderr, "skinning failed: %s\n", err.c_str());
      return 1;
    }
    if (got.empty()) continue;  // not skinned
    std::printf("%s %zu", prim.GetPath().str().c_str(), got.size());
    for (float v : got) std::printf(" %.9g", v);
    std::printf("\n");
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--dump") return Dump(argv[2], std::atof(argv[3]));
  if (argc < 6) {
    std::fprintf(stderr, "usage: skin_compare SCENE MESH_PRIM FBX TOL FRAME...\n");
    return 2;
  }
  const double tol = std::atof(argv[4]);
  lightusd::Stage stage;
  std::string warn, err;
  if (!lightusd::LoadUSDComposed(argv[1], &stage, &warn, &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  lightusd::UsdPrim mesh = stage.GetPrimAtPath(argv[2]);
  if (!mesh.IsValid()) {
    std::fprintf(stderr, "no prim %s\n", argv[2]);
    return 1;
  }
  // FBX "-": no reference animation; the skeleton's rest pose must leave the
  // mesh at its bind points (a self-consistent inline skeleton).
  const bool rest_mode = std::string(argv[3]) == "-";
  std::ifstream f(rest_mode ? "/dev/null" : argv[3], std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string bytes = ss.str();
  ufbx_load_opts opts = {};
  opts.target_axes = ufbx_axes_right_handed_y_up;
  opts.target_unit_meters = 1.0;
  opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
  ufbx_error uerr;
  ufbx_scene* scene =
      rest_mode ? nullptr : ufbx_load_memory(bytes.data(), bytes.size(), &opts, &uerr);
  if (!rest_mode && (!scene || !scene->anim_stacks.count)) {
    std::fprintf(stderr, "ufbx: cannot load %s or no animation\n", argv[3]);
    return 1;
  }
  const double fps = scene ? scene->settings.frames_per_second : 24.0;
  const std::string prim_path = argv[2];
  const std::string mesh_name = prim_path.substr(prim_path.rfind('/') + 1);
  int fails = 0;
  for (int a = 5; a < argc; ++a) {
    const double frame = std::atof(argv[a]);
    lightusd::tydra::ValueArrayRead<float> points;
    if (!lightusd::tydra::ReadFloatArray(mesh, "points", frame, &points)) {
      std::fprintf(stderr, "no points\n");
      return 1;
    }
    std::vector<float> got;
    if (!Deform(stage, mesh, frame, points, &got, &err) || got.empty()) {
      std::fprintf(stderr, "LightUSD skinning failed: %s\n", err.c_str());
      return 1;
    }
    ufbx_scene* ev = rest_mode ? nullptr
                               : ufbx_evaluate_scene(scene, scene->anim_stacks.data[0]->anim,
                                                     frame / fps, nullptr, &uerr);
    std::vector<float> want;
    if (rest_mode) want.assign(points.begin(), points.end());
    for (size_t i = 0; ev && i < ev->nodes.count; ++i) {
      const ufbx_node* node = ev->nodes.data[i];
      if (!node->mesh || (!node->mesh->skin_deformers.count && !node->mesh->blend_deformers.count))
        continue;
      // only the FBX node named like the prim (scenes may hold several meshes)
      if (std::string(node->name.data, node->name.length) != mesh_name) continue;
      const ufbx_mesh* m = node->mesh;
      const ufbx_skin_deformer* skin = m->skin_deformers.count ? m->skin_deformers.data[0] : nullptr;
      for (size_t v = 0; v < m->vertices.count; ++v) {
        // blend shapes (ufbx's FBX in-between interpolation), then skinning
        ufbx_vec3 base = m->vertices.data[v];
        for (size_t d = 0; d < m->blend_deformers.count; ++d) {
          const ufbx_vec3 o = ufbx_get_blend_vertex_offset(m->blend_deformers.data[d], v);
          base.x += o.x, base.y += o.y, base.z += o.z;
        }
        if (!skin) {
          const ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_world, base);
          want.insert(want.end(), {float(p.x), float(p.y), float(p.z)});
          continue;
        }
        ufbx_vec3 acc = {0, 0, 0};
        double sum = 0;
        const ufbx_skin_vertex sv = skin->vertices.data[v];
        for (uint32_t k = 0; k < sv.num_weights; ++k) {
          const ufbx_skin_weight w = skin->weights.data[sv.weight_begin + k];
          const ufbx_vec3 p = ufbx_transform_position(
              &skin->clusters.data[w.cluster_index]->geometry_to_world, base);
          acc.x += w.weight * p.x;
          acc.y += w.weight * p.y;
          acc.z += w.weight * p.z;
          sum += w.weight;
        }
        // normalized weights, as UsdSkel (and the DCC that wrote the file)
        if (sum > 0) acc.x /= sum, acc.y /= sum, acc.z /= sum;
        want.insert(want.end(), {float(acc.x), float(acc.y), float(acc.z)});
      }
    }
    if (ev) ufbx_free_scene(ev);
    if (std::getenv("SKIN_COMPARE_DEBUG"))
      for (size_t i = 0; i < 4 * 3 && i + 2 < got.size(); i += 3)
        std::printf("  [%zu] lightusd (%g %g %g) ufbx (%g %g %g) bind (%g %g %g)\n", i / 3, got[i],
                    got[i + 1], got[i + 2], want[i], want[i + 1], want[i + 2], points[i],
                    points[i + 1], points[i + 2]);
    const double d = std::max(MaxNearest(got, want), MaxNearest(want, got));
    // point counts may differ (the OBJ import may split vertices per face)
    const bool ok = d <= tol;
    std::printf("%s frame %g: %zu/%zu points, max distance %.2e\n", ok ? "ok  " : "FAIL", frame,
                got.size() / 3, want.size() / 3, d);
    fails += !ok;
  }
  if (scene) ufbx_free_scene(scene);
  return fails ? 1 : 0;
}
