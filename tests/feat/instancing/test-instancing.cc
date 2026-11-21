// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Feature test for preliminary PointInstancer instancing support:
//   1. GeomPointInstancer compute API (ComputeInstanceTransformsAtTime /
//      ComputeMaskAtTime).
//   2. Tydra render-scene expansion into RenderScene::instances.
//
// Fixture: tests/usda/pointinstancer-expand-001.usda

#include <cmath>
#include <iostream>
#include <vector>

#include "lightusd.hh"
#include "usdGeom.hh"
#include "tydra/render-data.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::cerr << "FAIL: " << (msg) << "  [" << #cond << "] (line "  \
                << __LINE__ << ")\n";                                 \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

bool nearlyEqual(double a, double b, double eps = 1e-4) {
  return std::fabs(a - b) <= eps;
}

const char *kScene = "tests/usda/pointinstancer-expand-001.usda";

// --- Part 1: compute API directly on the GeomPointInstancer prim ---
bool test_compute_api(const Stage &stage) {
  std::cout << "[compute-api] " << kScene << "\n";

  auto pr = stage.GetPrimAtPath(Path("/instancer", ""));
  if (!pr) {
    std::cerr << "FAIL: cannot find /instancer: " << pr.error() << "\n";
    ++g_failures;
    return false;
  }
  const GeomPointInstancer *pi = (*pr)->as<GeomPointInstancer>();
  CHECK(pi != nullptr, "/instancer is a GeomPointInstancer");
  if (!pi) return false;

  // Accessors.
  const std::vector<int32_t> protoIndices = pi->get_protoIndices();
  CHECK(protoIndices.size() == 4, "protoIndices has 4 entries");

  // Transforms.
  std::vector<lightusd::legacy::value::matrix4d> xforms;
  std::string err;
  bool ok = ComputeInstanceTransformsAtTime(
      *pi, lightusd::legacy::value::TimeCode::Default(),
      lightusd::legacy::value::TimeSampleInterpolationType::Linear, &xforms, &err);
  CHECK(ok, "ComputeInstanceTransformsAtTime succeeds");
  CHECK(xforms.size() == 4, "4 instance transforms");

  if (xforms.size() == 4) {
    // instance 0: translate (0,0,0), scale 1.
    CHECK(nearlyEqual(xforms[0].m[3][0], 0.0) &&
              nearlyEqual(xforms[0].m[3][1], 0.0) &&
              nearlyEqual(xforms[0].m[3][2], 0.0),
          "instance 0 translation == (0,0,0)");
    CHECK(nearlyEqual(xforms[0].m[0][0], 1.0), "instance 0 scale.x == 1");

    // instance 1: translate (10,0,0), scale 2.
    CHECK(nearlyEqual(xforms[1].m[3][0], 10.0) &&
              nearlyEqual(xforms[1].m[3][1], 0.0) &&
              nearlyEqual(xforms[1].m[3][2], 0.0),
          "instance 1 translation == (10,0,0)");
    CHECK(nearlyEqual(xforms[1].m[0][0], 2.0) &&
              nearlyEqual(xforms[1].m[1][1], 2.0) &&
              nearlyEqual(xforms[1].m[2][2], 2.0),
          "instance 1 scale == 2");

    // instance 3: translate (10,10,0), scale 1.
    CHECK(nearlyEqual(xforms[3].m[3][0], 10.0) &&
              nearlyEqual(xforms[3].m[3][1], 10.0) &&
              nearlyEqual(xforms[3].m[3][2], 0.0),
          "instance 3 translation == (10,10,0)");
  }

  // Mask: index 2 masked via invisibleIds.
  std::vector<bool> mask;
  ok = ComputeMaskAtTime(*pi, lightusd::legacy::value::TimeCode::Default(), &mask, &err);
  CHECK(ok, "ComputeMaskAtTime succeeds");
  CHECK(mask.size() == 4, "mask has 4 entries");
  if (mask.size() == 4) {
    CHECK(mask[0] && mask[1] && mask[3], "instances 0,1,3 visible");
    CHECK(!mask[2], "instance 2 masked (invisibleIds)");
  }

  return true;
}

// --- Part 2: Tydra expansion into RenderScene::instances ---
bool test_tydra_expansion(Stage &stage) {
  std::cout << "[tydra-expand] " << kScene << "\n";

  RenderScene scene;
  RenderSceneConverterEnv env(stage);
  env.scene_config.expand_point_instancers = true;

  RenderSceneConverter converter;
  if (!converter.ConvertToRenderScene(env, &scene)) {
    std::cerr << "FAIL: ConvertToRenderScene: " << converter.GetError() << "\n";
    ++g_failures;
    return false;
  }

  // 4 instances, 1 masked => 3 visible RenderInstances.
  CHECK(scene.instances.size() == 3,
        "3 RenderInstances (1 of 4 masked)");

  // Collect by instance index suffix.
  const RenderInstance *inst1 = nullptr;
  const RenderInstance *inst3 = nullptr;
  bool saw_inst2 = false;
  for (const auto &ri : scene.instances) {
    if (ri.abs_path.find("/instance_1") != std::string::npos) inst1 = &ri;
    if (ri.abs_path.find("/instance_3") != std::string::npos) inst3 = &ri;
    if (ri.abs_path.find("/instance_2") != std::string::npos) saw_inst2 = true;
  }
  CHECK(!saw_inst2, "masked instance 2 not emitted");
  CHECK(inst1 != nullptr, "instance_1 RenderInstance present");
  CHECK(inst3 != nullptr, "instance_3 RenderInstance present");

  if (inst1) {
    CHECK(nearlyEqual(inst1->global_matrix.m[3][0], 10.0) &&
              nearlyEqual(inst1->global_matrix.m[3][1], 0.0),
          "instance_1 global translation == (10,0,0)");
    CHECK(nearlyEqual(inst1->global_matrix.m[0][0], 2.0),
          "instance_1 global scale.x == 2");
  }

  // Instances 1 and 3 both use ProtoB => shared mesh_id (geometry not duplicated).
  if (inst1 && inst3) {
    CHECK(inst1->mesh_id >= 0, "instance_1 has a valid mesh_id");
    CHECK(inst1->mesh_id == inst3->mesh_id,
          "instance_1 and instance_3 share ProtoB's mesh_id");
  }

  return true;
}

}  // namespace

int main(int argc, char **argv) {
  const char *scene_path = (argc > 1) ? argv[1] : kScene;

  Stage stage;
  std::string warn, err;
  if (!LoadUSDFromFile(scene_path, &stage, &warn, &err)) {
    std::cerr << "Load failed: " << err << "\n";
    return 1;
  }
  if (!warn.empty()) std::cerr << "warn: " << warn << "\n";

  test_compute_api(stage);
  test_tydra_expansion(stage);

  if (g_failures == 0) {
    std::cout << "All instancing tests passed.\n";
    return 0;
  }
  std::cerr << g_failures << " instancing check(s) failed.\n";
  return 1;
}
