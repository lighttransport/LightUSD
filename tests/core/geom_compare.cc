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
#include "schema/usd-collection.hh"
#include "schema/usd-shade.hh"
#include "schema/geom-point-instancer.hh"
#include "stage/stage.hh"

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
