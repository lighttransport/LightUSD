// SPDX-License-Identifier: Apache-2.0
// geom_compare --dump-instancer SCENE TIME
// geom_compare --dump-xform SCENE TIME
// geom_compare --dump-extent SCENE TIME  (<path> min.xyz max.xyz per Boundable)
//
// Native runtime dump of UsdGeomPointInstancer evaluation for the conformance
// oracle (usdspec-conformance tests/next/test-domain-geom-runtime.py), which
// compares it with UsdGeom.PointInstancer.ComputeInstanceTransformsAtTime
// (ExcludeProtoXform) and ComputeMaskAtTime. One line per instancer:
//
//   <prim path> <instance count> <mask bits as 0/1...> <16 doubles per instance>
//
// --dump-xform: `<prim path> <16 doubles>` local-to-world per prim whose
// world transform LightUSD computes (compared with UsdGeom.XformCache).
//
// Matrices are row-major with the translation in the last row (Gf layout).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "eval/xform-eval.hh"
#include "load-usd.hh"
#include "schema/geom-boundable.hh"
#include "schema/physics-resolve.hh"
#include "schema/usd-collection.hh"
#include "schema/usd-render.hh"
#include "schema/usd-shade.hh"
#include "schema/geom-point-instancer.hh"
#include "stage/stage.hh"
#include "tydra/render-converter.hh"

namespace {
int DumpInstancers(const char* scene, double time) {
  lightusd::Stage stage;
  std::string warn, err;
  if (!lightusd::LoadUSDComposed(scene, &stage, &warn, &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  for (const auto& pi : lightusd::GetAllPointInstancers(stage)) {
    const auto xforms = pi.ComputeInstanceTransforms(time);
    const auto mask = pi.ComputeMaskAtTime(time);
    std::printf("%s %zu", pi.GetPrim().GetPath().str().c_str(), xforms.size());
    for (size_t i = 0; i < xforms.size(); ++i) {
      std::printf(" %d", (i < mask.size() && !mask[i]) ? 0 : 1);
    }
    for (const auto& x : xforms) {
      for (double v : x.matrix) std::printf(" %.17g", v);
    }
    std::printf("\n");
  }
  return 0;
}
int DumpXforms(const char* scene, double time) {
  lightusd::Stage stage;
  std::string warn, err;
  if (!lightusd::LoadUSDComposed(scene, &stage, &warn, &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  stage.Traverse([&](const lightusd::UsdPrim& prim) {
    double m[16];
    if (lightusd::ComputeWorldTransform(stage, prim, m, time)) {
      std::printf("%s", prim.GetPath().str().c_str());
      for (double v : m) std::printf(" %.17g", v);
      std::printf("\n");
    }
    return true;
  });
  return 0;
}
int DumpExtents(const char* scene, double time) {
  lightusd::Stage stage;
  std::string warn, err;
  if (!lightusd::LoadUSDComposed(scene, &stage, &warn, &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  stage.Traverse([&](const lightusd::UsdPrim& prim) {
    double e[6];
    if (lightusd::ComputeExtent(prim, time, e)) {
      std::printf("%s", prim.GetPath().str().c_str());
      for (double v : e) std::printf(" %.9g", v);
      std::printf("\n");
    }
    return true;
  });
  return 0;
}
int LoadOr(const char* scene, lightusd::Stage* stage) {
  std::string warn, err;
  if (!lightusd::LoadUSDComposed(scene, stage, &warn, &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  return 0;
}

// `<prim path> <material path or ->` per prim, for purpose "" / preview / full.
int DumpBindings(const char* scene, const std::string& purpose) {
  lightusd::Stage stage;
  if (LoadOr(scene, &stage)) return 1;
  stage.Traverse([&](const lightusd::UsdPrim& prim) {
    const std::string m = lightusd::ComputeBoundMaterial(stage, prim.GetPath().str(), purpose);
    std::printf("%s %s\n", prim.GetPath().str().c_str(), m.empty() ? "-" : m.c_str());
    return true;
  });
  return 0;
}

// `<material path> <shader.output or ->` for render contexts CTX (comma list).
int DumpSurface(const char* scene, const std::string& ctx_list) {
  lightusd::Stage stage;
  if (LoadOr(scene, &stage)) return 1;
  std::vector<std::string> contexts;
  size_t start = 0;
  while (start <= ctx_list.size()) {
    const size_t comma = ctx_list.find(',', start);
    const std::string c = ctx_list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!c.empty() && c != "-") contexts.push_back(c);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  stage.Traverse([&](const lightusd::UsdPrim& prim) {
    if (lightusd::IsMaterial(prim)) {
      const std::string src = lightusd::ComputeTerminalSource(stage, prim, contexts);
      std::printf("%s %s\n", prim.GetPath().str().c_str(), src.empty() ? "-" : src.c_str());
    }
    return true;
  });
  return 0;
}

void JsonList(const std::vector<std::string>& v) {
  std::printf("[");
  for (size_t i = 0; i < v.size(); ++i) std::printf("%s\"%s\"", i ? "," : "", v[i].c_str());
  std::printf("]");
}

// One JSON object per line: {"kind": ..., "path": ..., fields}.
int DumpPhysics(const char* scene) {
  lightusd::Stage stage;
  if (LoadOr(scene, &stage)) return 1;
  const lightusd::ResolvedPhysics r = lightusd::ResolvePhysics(stage);
  for (const auto& s : r.scenes) {
    std::printf("{\"kind\":\"Scene\",\"path\":\"%s\",\"gravityDirection\":[%.9g,%.9g,%.9g],"
                "\"gravityMagnitude\":%.9g}\n", s.path.c_str(), s.gravity_direction[0],
                s.gravity_direction[1], s.gravity_direction[2], s.gravity_magnitude);
  }
  for (const auto& b : r.bodies) {
    std::printf("{\"kind\":\"RigidBody\",\"path\":\"%s\",\"rigidBodyEnabled\":%s,"
                "\"simulationOwners\":", b.path.c_str(), b.enabled ? "true" : "false");
    JsonList(b.simulation_owners);
    std::printf(",\"collisions\":");
    JsonList(b.collisions);
    std::printf("}\n");
  }
  for (const auto& c : r.colliders) {
    std::printf("{\"kind\":\"Collider\",\"path\":\"%s\",\"rigidBody\":\"%s\","
                "\"collisionEnabled\":%s,\"simulationOwners\":", c.path.c_str(),
                c.rigid_body.c_str(), c.collision_enabled ? "true" : "false");
    JsonList(c.simulation_owners);
    std::printf("}\n");
  }
  for (const auto& j : r.joints) {
    std::printf("{\"kind\":\"Joint\",\"path\":\"%s\",\"rel0\":\"%s\",\"rel1\":\"%s\","
                "\"body0\":\"%s\",\"body1\":\"%s\",\"jointEnabled\":%s,"
                "\"collisionEnabled\":%s,\"excludeFromArticulation\":%s}\n",
                j.path.c_str(), j.rel0.c_str(), j.rel1.c_str(), j.body0.c_str(), j.body1.c_str(),
                j.joint_enabled ? "true" : "false", j.collision_enabled ? "true" : "false",
                j.exclude_from_articulation ? "true" : "false");
  }
  for (const auto& g : r.collision_groups) {
    std::printf("{\"kind\":\"CollisionGroup\",\"path\":\"%s\",\"invertFilteredGroups\":%s,"
                "\"filteredGroups\":", g.path.c_str(), g.invert_filtered_groups ? "true" : "false");
    JsonList(g.filtered_groups);
    std::printf(",\"mergedGroups\":");
    JsonList(g.merged_groups);
    std::printf("}\n");
  }
  return 0;
}

// One line per Volume field binding, as tydra's converter resolves it:
// <volume> <binding> <file> <grid name> <name index> <value type> <class>
// <active voxels> <index min xyz> <index max xyz>   (or "<volume> <binding> ERROR")
int DumpVolumes(const char* scene, double time) {
  lightusd::Stage stage;
  if (LoadOr(scene, &stage)) return 1;
  lightusd::tydra::ConverterConfig cfg;
  cfg.time_code = time;
  const std::string s(scene);
  const size_t slash = s.find_last_of('/');
  cfg.asset_base_dir = slash == std::string::npos ? "." : s.substr(0, slash);
  lightusd::tydra::RenderSceneConverter conv(cfg);
  const auto res = conv.Convert(stage);
  for (const auto& v : res.scene.volumes) {
    for (const auto& f : v.fields) {
      if (!f.resolution_error.empty()) {
        std::printf("%s %s ERROR\n", v.prim_path.c_str(), f.binding.c_str());
        continue;
      }
      const auto& g = f.field.grid;
      std::string file = f.field.request.file_path;
      const size_t fs = file.find_last_of('/');
      if (fs != std::string::npos) file = file.substr(fs + 1);
      std::printf("%s %s %s %s %d %s %s %llu %d %d %d %d %d %d\n", v.prim_path.c_str(),
                  f.binding.c_str(), file.c_str(), g.name.c_str(), g.name_index,
                  f.field.value_type.c_str(), f.field.level_set ? "levelSet" : "fogVolume",
                  static_cast<unsigned long long>(g.active_voxels), g.index_min[0], g.index_min[1],
                  g.index_min[2], g.index_max[0], g.index_max[1], g.index_max[2]);
    }
  }
  return 0;
}

// UsdRenderComputeSpec lines in the format of usdspec-conformance's C++
// oracle (tests/next/oracle/render_spec.cc).
int DumpRenderSpec(const char* scene) {
  lightusd::Stage stage;
  if (LoadOr(scene, &stage)) return 1;
  stage.Traverse([&](const lightusd::UsdPrim& prim) {
    lightusd::RenderSpec spec;
    if (!lightusd::ComputeRenderSpec(stage, prim, &spec)) return true;
    std::printf("settings %s purposes", spec.settings_path.c_str());
    for (const auto& t : spec.included_purposes) std::printf(" %s", t.c_str());
    std::printf("\n");
    for (const auto& p : spec.products) {
      std::printf("product %s camera %s res %d %d par %.9g policy %s aperture %.9g %.9g "
                  "dw %.9g %.9g %.9g %.9g mb %d dof %d vars",
                  p.path.c_str(), p.camera.c_str(), p.resolution[0], p.resolution[1],
                  p.pixel_aspect_ratio, p.aspect_ratio_conform_policy.c_str(), p.aperture[0],
                  p.aperture[1], p.data_window_ndc[0], p.data_window_ndc[1], p.data_window_ndc[2],
                  p.data_window_ndc[3], int(p.disable_motion_blur), int(p.disable_depth_of_field));
      for (const auto& v : p.render_vars) std::printf(" %s", v.c_str());
      std::printf("\n");
    }
    return true;
  });
  return 0;
}

// `<path> 0|1` membership of every prim in OWNER.collection:NAME.
int DumpCollection(const char* scene, const char* owner, const char* name) {
  lightusd::Stage stage;
  if (LoadOr(scene, &stage)) return 1;
  const lightusd::UsdPrim o = stage.GetPrimAtPath(std::string(owner));
  stage.Traverse([&](const lightusd::UsdPrim& prim) {
    std::printf("%s %d\n", prim.GetPath().str().c_str(),
                lightusd::IsPathInCollection(stage, o, name, prim.GetPath().str()) ? 1 : 0);
    return true;
  });
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--dump-instancer") {
    return DumpInstancers(argv[2], std::atof(argv[3]));
  }
  if (argc == 4 && std::string(argv[1]) == "--dump-xform") {
    return DumpXforms(argv[2], std::atof(argv[3]));
  }
  if (argc == 4 && std::string(argv[1]) == "--dump-extent") {
    return DumpExtents(argv[2], std::atof(argv[3]));
  }
  if (argc == 4 && std::string(argv[1]) == "--dump-binding") {
    return DumpBindings(argv[2], std::string(argv[3]) == "all" ? "" : argv[3]);
  }
  if (argc == 4 && std::string(argv[1]) == "--dump-volume") {
    return DumpVolumes(argv[2], std::atof(argv[3]));
  }
  if (argc == 3 && std::string(argv[1]) == "--dump-render-spec") {
    return DumpRenderSpec(argv[2]);
  }
  if (argc == 3 && std::string(argv[1]) == "--dump-physics") {
    return DumpPhysics(argv[2]);
  }
  if (argc == 4 && std::string(argv[1]) == "--dump-surface") {
    return DumpSurface(argv[2], argv[3]);
  }
  if (argc == 5 && std::string(argv[1]) == "--dump-collection") {
    return DumpCollection(argv[2], argv[3], argv[4]);
  }
  std::fprintf(stderr,
               "usage: geom_compare --dump-instancer|--dump-xform|--dump-extent SCENE TIME\n");
  return 2;
}
