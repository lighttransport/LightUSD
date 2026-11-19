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
  std::fprintf(stderr,
               "usage: geom_compare --dump-instancer|--dump-xform|--dump-extent SCENE TIME\n");
  return 2;
}
