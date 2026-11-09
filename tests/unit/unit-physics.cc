// SPDX-License-Identifier: Apache 2.0
// USD Physics and MuJoCo physics annotation unit tests.
// Exercises the full pipeline: ASCII parser -> prim-reconstruct -> Stage,
// plus pprint roundtrip and Tydra JSON export.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-physics.h"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usdc-writer.hh"
#include "core/prim.hh"
#include "usdPhysics.hh"
#include "mjcPhysics.hh"
#include "io-util.hh"
#include "tydra/physics-to-json.hh"
#include "tydra/urdf-to-usd.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "external/jsonhpp/nlohmann/json.hpp"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace lightusd;

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------
namespace {

static bool parse_usda(const char *usda, Stage *stage, std::string *warn,
                       std::string *err) {
  return LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda), "test.usda",
      stage, warn, err);
}

static std::string UsdaFixturePath(const std::string &filename) {
  std::string base_dir = io::GetBaseDir(std::string(__FILE__));
  std::string usda_dir = io::JoinPath(base_dir, "../usda");
  return io::JoinPath(usda_dir, filename);
}

// Author -> USDC bytes -> reload. Exercises sconv-physics.cc + the crate
// writer's generic property-bag pass for physx*:* / state:* attributes.
static bool usdc_roundtrip(const char *usda, Stage *out, std::string *warn,
                           std::string *err) {
  Stage tmp;
  if (!parse_usda(usda, &tmp, warn, err)) return false;
  std::vector<uint8_t> bytes;
  if (!usdc::SaveAsUSDCToMemory(tmp, &bytes, warn, err)) return false;
  return LoadUSDCFromMemory(bytes.data(), bytes.size(), "test.usdc",
                            out, warn, err);
}

static bool usdc_roundtrip_stage(const Stage &stage, Stage *out,
                                 std::string *warn, std::string *err) {
  std::vector<uint8_t> bytes;
  if (!usdc::SaveAsUSDCToMemory(stage, &bytes, warn, err)) return false;
  return LoadUSDCFromMemory(bytes.data(), bytes.size(), "test.usdc",
                            out, warn, err);
}

// Tolerant float comparison — values authored as `custom float` in USDA
// land in tests as `static_cast<double>(float)`, which is NOT equal to
// the same numeric literal typed in C++ as double (0.01f promoted is
// not 0.01). Anything ≥ 1e-6 wins.
static bool approx_eq(double a, double b) {
  const double tol = 1e-5 * (1.0 + std::abs(b));
  return std::abs(a - b) <= tol;
}

static nlohmann::json parse_json_noexcept(const std::string &json) {
  return nlohmann::json::parse(json, nullptr, false);
}

// Pull a numeric value out of a generic property bag (post-parse). Returns
// false if the key is absent or not a numeric attribute. Matches the
// priority-lookup pattern downstream consumers use.
static bool get_prop_num(const std::map<std::string, Property> &props,
                         const std::string &key, double *out) {
  auto it = props.find(key);
  if (it == props.end()) return false;
  if (!it->second.is_attribute()) return false;
  const auto &attr = it->second.get_attribute();
  if (auto v = attr.get_value<int>())      { *out = static_cast<double>(*v); return true; }
  if (auto v = attr.get_value<uint32_t>()) { *out = static_cast<double>(*v); return true; }
  if (auto v = attr.get_value<int64_t>())  { *out = static_cast<double>(*v); return true; }
  if (auto v = attr.get_value<uint64_t>()) { *out = static_cast<double>(*v); return true; }
  if (auto v = attr.get_value<float>())    { *out = static_cast<double>(*v); return true; }
  if (auto v = attr.get_value<double>())   { *out = *v; return true; }
  return false;
}

static bool has_api(const Prim *prim, APISchemas::APIName want) {
  if (!prim || !prim->metas().has_apiSchemas()) return false;
  const auto schemas = prim->metas().get_apiSchemas();
  for (const auto &n : schemas.names) {
    if (n.first == want) return true;
  }
  return false;
}

static const nlohmann::json *find_json_joint(const nlohmann::json &root,
                                             const std::string &path) {
  if (!root.contains("joints") || !root["joints"].is_array()) return nullptr;
  for (const auto &joint : root["joints"]) {
    if (joint.contains("path") && joint["path"].is_string() &&
        joint["path"].get<std::string>() == path) {
      return &joint;
    }
  }
  return nullptr;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// 1. PhysicsScene — basic gravity attributes
// ---------------------------------------------------------------------------
void physics_scene_reconstruct_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "SimScene"
{
    vector3f physics:gravityDirection = (0, 0, -1)
    float physics:gravityMagnitude = 9.81
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/SimScene", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim != nullptr);
  if (!prim) return;
  TEST_CHECK(prim->is<PhysicsScene>());

  const auto *scene = prim->as<PhysicsScene>();
  TEST_CHECK(scene != nullptr);
  if (!scene) return;

  // Check gravity direction
  auto gd_opt = scene->gravityDirection.get_value();
  TEST_CHECK(gd_opt.has_value());
  if (gd_opt.has_value()) {
    auto gd = gd_opt.value();
    TEST_CHECK(gd[0] == 0.0f);
    TEST_CHECK(gd[1] == 0.0f);
    TEST_CHECK(gd[2] == -1.0f);
  }

  // Check gravity magnitude
  auto gm_opt = scene->gravityMagnitude.get_value();
  TEST_CHECK(gm_opt.has_value());
  if (gm_opt.has_value()) {
    TEST_CHECK(std::fabs(gm_opt.value() - 9.81f) < 1e-5f);
  }

  // No MjcSceneAPI since no mjc: properties
  TEST_CHECK(!scene->mjcScene.has_value());
}

// ---------------------------------------------------------------------------
// 2. PhysicsScene with MjcSceneAPI
// ---------------------------------------------------------------------------
void physics_scene_mjc_scene_api_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "SimScene" (
    prepend apiSchemas = ["MjcSceneAPI"]
)
{
    vector3f physics:gravityDirection = (0, 0, -1)
    float physics:gravityMagnitude = 9.81
    uniform double mjc:option:timestep = 0.005
    uniform token mjc:option:integrator = "implicit"
    uniform token mjc:option:solver = "newton"
    uniform int mjc:option:iterations = 200
    uniform bool mjc:flag:gravity = 1
    uniform bool mjc:flag:contact = 1
    uniform bool mjc:flag:override = 0
    uniform bool mjc:compiler:autoLimits = 1
    uniform token mjc:compiler:angle = "radian"
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/SimScene", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim != nullptr);
  if (!prim) return;

  const auto *scene = prim->as<PhysicsScene>();
  TEST_CHECK(scene != nullptr);
  if (!scene) return;

  // MjcSceneAPI should be populated
  TEST_CHECK(scene->mjcScene.has_value());
  if (!scene->mjcScene.has_value()) return;

  const auto &mjc = scene->mjcScene.value();
  TEST_CHECK(mjc.timestep.get_value() == 0.005);
  TEST_CHECK(mjc.integrator.get_value().str() == "implicit");
  TEST_CHECK(mjc.solver.get_value().str() == "newton");
  TEST_CHECK(mjc.iterations.get_value() == 200);
  TEST_CHECK(mjc.flag_gravity.get_value() == true);
  TEST_CHECK(mjc.flag_contact.get_value() == true);
  TEST_CHECK(mjc.flag_override.get_value() == false);
  TEST_CHECK(mjc.compiler_autoLimits.get_value() == true);
  TEST_CHECK(mjc.compiler_angle.get_value().str() == "radian");
}

// ---------------------------------------------------------------------------
// 2b. PhysicsScene with NewtonSceneAPI / NewtonKaminoSceneAPI
// ---------------------------------------------------------------------------
void physics_scene_newton_api_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "SimScene" (
    prepend apiSchemas = ["NewtonSceneAPI", "NewtonKaminoSceneAPI"]
)
{
    vector3f physics:gravityDirection = (0, 0, -1)
    float physics:gravityMagnitude = 9.81
    uniform int newton:maxSolverIterations = 64
    uniform int newton:timeStepsPerSecond = 500
    bool newton:gravityEnabled = false
    uniform float newton:kamino:constraints:alpha = 0.25
    uniform token newton:kamino:jointCorrection = "none"
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/SimScene", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonSceneAPI));
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonKaminoSceneAPI));

  const auto *scene = prim->as<PhysicsScene>();
  TEST_CHECK(scene != nullptr);
  if (!scene) return;
  TEST_CHECK(scene->newtonScene.has_value());
  TEST_CHECK(scene->newtonKaminoScene.has_value());
  if (scene->newtonScene.has_value()) {
    const auto &n = scene->newtonScene.value();
    TEST_CHECK(n.maxSolverIterations.get_value() == 64);
    TEST_CHECK(n.timeStepsPerSecond.get_value() == 500);
    TEST_CHECK(n.gravityEnabled.get_value() == false);
  }
  if (scene->newtonKaminoScene.has_value()) {
    const auto &n = scene->newtonKaminoScene.value();
    TEST_CHECK(approx_eq(n.constraintsAlpha.get_value(), 0.25));
    TEST_CHECK(n.jointCorrection.get_value().str() == "none");
  }
}

// ---------------------------------------------------------------------------
// 2c. PhysicsScene with NewtonXpbdSceneAPI
// ---------------------------------------------------------------------------
void physics_scene_newton_xpbd_api_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "SimScene" (
    prepend apiSchemas = ["NewtonSceneAPI", "NewtonXpbdSceneAPI"]
)
{
    vector3f physics:gravityDirection = (0, 0, -1)
    float physics:gravityMagnitude = 9.81
    uniform int newton:maxSolverIterations = 32
    uniform int newton:timeStepsPerSecond = 240
    uniform float newton:xpbd:softBodyRelaxation = 0.7
    uniform float newton:xpbd:jointLinearCompliance = 0.001
    uniform float newton:xpbd:jointAngularRelaxation = 0.35
    uniform bool newton:xpbd:restitutionEnabled = true
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/SimScene", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonSceneAPI));
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonXpbdSceneAPI));

  const auto *scene = prim->as<PhysicsScene>();
  TEST_CHECK(scene != nullptr);
  if (!scene) return;
  TEST_CHECK(scene->newtonScene.has_value());
  TEST_CHECK(scene->newtonXpbdScene.has_value());
  if (scene->newtonScene.has_value()) {
    TEST_CHECK(scene->newtonScene.value().maxSolverIterations.get_value() == 32);
    TEST_CHECK(scene->newtonScene.value().timeStepsPerSecond.get_value() == 240);
  }
  if (scene->newtonXpbdScene.has_value()) {
    const auto &xpbd = scene->newtonXpbdScene.value();
    TEST_CHECK(approx_eq(xpbd.softBodyRelaxation.get_value(), 0.7));
    TEST_CHECK(approx_eq(xpbd.jointLinearCompliance.get_value(), 0.001));
    TEST_CHECK(approx_eq(xpbd.jointAngularRelaxation.get_value(), 0.35));
    TEST_CHECK(xpbd.restitutionEnabled.get_value() == true);
  }
}

// ---------------------------------------------------------------------------
// 3. PhysicsRevoluteJoint
// ---------------------------------------------------------------------------
void physics_revolute_joint_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "HingeJoint"
{
    rel physics:body0 = </World/Body0>
    rel physics:body1 = </World/Body1>
    token physics:axis = "Z"
    float physics:lowerLimit = -90
    float physics:upperLimit = 90
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/HingeJoint", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<PhysicsRevoluteJoint>());

  const auto *joint = prim->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  auto axis_opt = joint->axis.get_value();
  TEST_CHECK(axis_opt.has_value());
  if (axis_opt.has_value()) {
    TEST_CHECK(axis_opt.value().str() == "Z");
  }

  // Check body0 relationship
  TEST_CHECK(joint->body0.authored());
  auto paths0 = joint->body0.get_targetPaths();
  TEST_CHECK(paths0.size() == 1);
  if (paths0.size() == 1) {
    TEST_CHECK(paths0[0].prim_part() == "/World/Body0");
  }
}

// ---------------------------------------------------------------------------
// 4. PhysicsPrismaticJoint
// ---------------------------------------------------------------------------
void physics_prismatic_joint_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsPrismaticJoint "SlideJoint"
{
    rel physics:body0 = </World/Base>
    rel physics:body1 = </World/Slider>
    token physics:axis = "Y"
    float physics:lowerLimit = 0
    float physics:upperLimit = 10
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/SlideJoint", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<PhysicsPrismaticJoint>());
  const auto *joint = prim->as<PhysicsPrismaticJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;
  auto axis_opt = joint->axis.get_value();
  TEST_CHECK(axis_opt.has_value());
  if (axis_opt.has_value()) {
    TEST_CHECK(axis_opt.value().str() == "Y");
  }
}

// ---------------------------------------------------------------------------
// 5. PhysicsFixedJoint
// ---------------------------------------------------------------------------
void physics_fixed_joint_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsFixedJoint "WeldJoint"
{
    rel physics:body0 = </World/A>
    rel physics:body1 = </World/B>
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/WeldJoint", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  TEST_CHECK((*result)->is<PhysicsFixedJoint>());
}

// ---------------------------------------------------------------------------
// 6. PhysicsDistanceJoint
// ---------------------------------------------------------------------------
void physics_distance_joint_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsDistanceJoint "DistJoint"
{
    rel physics:body0 = </World/A>
    rel physics:body1 = </World/B>
    float physics:minDistance = 1.0
    float physics:maxDistance = 5.0
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/DistJoint", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  TEST_CHECK((*result)->is<PhysicsDistanceJoint>());
}

void physics_distance_joint_json_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsDistanceJoint "DistJoint"
{
    rel physics:body0 = </World/A>
    rel physics:body1 = </World/B>
    float physics:minDistance = 0.25
    float physics:maxDistance = 2.75
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/DistJoint", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsDistanceJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;
  auto lo = joint->minDistance.get_value();
  auto hi = joint->maxDistance.get_value();
  TEST_CHECK(lo.has_value() && approx_eq(lo.value(), 0.25));
  TEST_CHECK(hi.has_value() && approx_eq(hi.value(), 2.75));

  std::string json;
  std::string json_err;
  tydra::PhysicsJsonExportOptions opts;
  bool json_ok = tydra::ConvertPhysicsToJson(stage, &json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  if (!json_ok) return;
  nlohmann::json root = parse_json_noexcept(json);
  if (root.is_discarded()) {
    TEST_MSG("JSON failed to parse:\n%s", json.c_str());
  }
  TEST_CHECK(!root.is_discarded());
  if (root.is_discarded()) return;
  const nlohmann::json *j = find_json_joint(root, "/DistJoint");
  TEST_CHECK(j != nullptr);
  if (!j) return;
  TEST_CHECK((*j)["type"] == "PhysicsDistanceJoint");
  TEST_CHECK(approx_eq((*j)["minDistance"].get<double>(), 0.25));
  TEST_CHECK(approx_eq((*j)["maxDistance"].get<double>(), 2.75));
}

// ---------------------------------------------------------------------------
// 6b. PhysicsSphericalJoint
// ---------------------------------------------------------------------------
void physics_spherical_joint_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsSphericalJoint "BallJoint"
{
    rel physics:body0 = </BodyA>
    rel physics:body1 = </BodyB>
    token physics:axis = "Y"
    float physics:coneAngle0Limit = 45
    float physics:coneAngle1Limit = 30
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/BallJoint", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<PhysicsSphericalJoint>());

  const auto *joint = prim->as<PhysicsSphericalJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  auto axis_opt = joint->axis.get_value();
  TEST_CHECK(axis_opt.has_value());
  if (axis_opt.has_value()) {
    TEST_CHECK(axis_opt.value().str() == "Y");
  }

  auto cone0_opt = joint->coneAngle0Limit.get_value();
  TEST_CHECK(cone0_opt.has_value());
  if (cone0_opt.has_value()) {
    TEST_CHECK(cone0_opt.value() == 45.0f);
  }

  auto cone1_opt = joint->coneAngle1Limit.get_value();
  TEST_CHECK(cone1_opt.has_value());
  if (cone1_opt.has_value()) {
    TEST_CHECK(cone1_opt.value() == 30.0f);
  }

  // Check body0 relationship
  TEST_CHECK(joint->body0.authored());
  auto paths0 = joint->body0.get_targetPaths();
  TEST_CHECK(paths0.size() == 1);
  if (paths0.size() == 1) {
    TEST_CHECK(paths0[0].prim_part() == "/BodyA");
  }

  // Check body1 relationship
  TEST_CHECK(joint->body1.authored());
  auto paths1 = joint->body1.get_targetPaths();
  TEST_CHECK(paths1.size() == 1);
  if (paths1.size() == 1) {
    TEST_CHECK(paths1[0].prim_part() == "/BodyB");
  }
}

// ---------------------------------------------------------------------------
// 7. PhysicsRevoluteJoint with MjcJointAPI
// ---------------------------------------------------------------------------
void physics_joint_mjc_api_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "MjcHinge" (
    prepend apiSchemas = ["MjcJointAPI"]
)
{
    rel physics:body0 = </World/Body0>
    rel physics:body1 = </World/Body1>
    token physics:axis = "X"
    uniform double mjc:stiffness = 100
    uniform double mjc:damping = 10
    uniform double mjc:armature = 0.01
    uniform double mjc:frictionloss = 0.5
    uniform double mjc:ref = 0.5
    uniform int mjc:group = 2
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/MjcHinge", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const auto *joint = (*result)->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  TEST_CHECK(joint->mjcJoint.has_value());
  if (!joint->mjcJoint.has_value()) return;

  const auto &mjc = joint->mjcJoint.value();
  TEST_CHECK(mjc.stiffness.get_value() == 100.0);
  TEST_CHECK(mjc.damping.get_value() == 10.0);
  TEST_CHECK(mjc.armature.get_value() == 0.01);
  TEST_CHECK(mjc.frictionloss.get_value() == 0.5);
  TEST_CHECK(mjc.ref.get_value() == 0.5);
  TEST_CHECK(mjc.group.get_value() == 2);
}

void physics_joint_mjc_full_json_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "MjcFullHinge" (
    prepend apiSchemas = ["MjcJointAPI"]
)
{
    rel physics:body0 = </World/Body0>
    rel physics:body1 = </World/Body1>
    token physics:axis = "Z"
    uniform int mjc:group = 4
    uniform double mjc:stiffness = 120
    uniform double mjc:damping = 8
    uniform double mjc:armature = 0.02
    uniform double mjc:frictionloss = 0.1
    uniform double[] mjc:springdamper = [4, 0.5]
    uniform double mjc:springref = 0.25
    uniform double mjc:ref = -0.1
    uniform double mjc:margin = 0.03
    uniform double[] mjc:solreflimit = [0.03, 1.1]
    uniform double[] mjc:solimplimit = [0.8, 0.9, 0.01, 0.5, 2]
    uniform double[] mjc:solreffriction = [0.04, 1.2]
    uniform double[] mjc:solimpfriction = [0.7, 0.8, 0.02, 0.4, 1.5]
    uniform double mjc:actuatorfrcrange:min = -5
    uniform double mjc:actuatorfrcrange:max = 6
    uniform token mjc:actuatorfrclimited = "true"
    uniform bool mjc:actuatorgravcomp = 1
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/MjcFullHinge", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;
  TEST_CHECK(joint->mjcJoint.has_value());
  if (!joint->mjcJoint.has_value()) return;
  const auto &m = joint->mjcJoint.value();
  TEST_CHECK(m.group.authored() && m.group.get_value() == 4);
  auto springdamper = m.springdamper.get_value();
  TEST_CHECK(springdamper.has_value());
  if (springdamper.has_value()) {
    const auto &v = springdamper.value();
    TEST_CHECK(v.size() == 2 && approx_eq(v[0], 4.0) && approx_eq(v[1], 0.5));
  }
  auto solreflimit = m.solreflimit.get_value();
  TEST_CHECK(solreflimit.has_value());
  if (solreflimit.has_value()) {
    const auto &v = solreflimit.value();
    TEST_CHECK(v.size() == 2 && approx_eq(v[0], 0.03) && approx_eq(v[1], 1.1));
  }
  TEST_CHECK(m.solimplimit.get_value().has_value());
  TEST_CHECK(m.solreffriction.get_value().has_value());
  TEST_CHECK(m.solimpfriction.get_value().has_value());
  TEST_CHECK(m.actuatorfrclimited.authored() &&
             m.actuatorfrclimited.get_value().str() == "true");
  TEST_CHECK(m.actuatorgravcomp.authored() &&
             m.actuatorgravcomp.get_value() == true);

  std::string json;
  std::string json_err;
  tydra::PhysicsJsonExportOptions opts;
  opts.include_mjc = true;
  bool json_ok = tydra::ConvertPhysicsToJson(stage, &json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  if (!json_ok) return;
  nlohmann::json root = parse_json_noexcept(json);
  if (root.is_discarded()) {
    TEST_MSG("JSON failed to parse:\n%s", json.c_str());
  }
  TEST_CHECK(!root.is_discarded());
  if (root.is_discarded()) return;
  const nlohmann::json *j = find_json_joint(root, "/MjcFullHinge");
  TEST_CHECK(j != nullptr);
  if (!j) return;
  TEST_CHECK(j->contains("mjc"));
  if (!j->contains("mjc")) return;
  const nlohmann::json &mjc = (*j)["mjc"];
  TEST_CHECK(mjc["group"] == 4);
  TEST_CHECK(mjc["actuatorfrclimited"] == "true");
  TEST_CHECK(mjc["actuatorgravcomp"] == true);
  TEST_CHECK(mjc.contains("solreflimit"));
  TEST_CHECK(mjc["solreflimit"].is_array() && mjc["solreflimit"].size() == 2);
  if (mjc["solreflimit"].is_array() && mjc["solreflimit"].size() == 2) {
    TEST_CHECK(approx_eq(mjc["solreflimit"][0].get<double>(), 0.03));
    TEST_CHECK(approx_eq(mjc["solreflimit"][1].get<double>(), 1.1));
  }
  TEST_CHECK(mjc.contains("solimplimit"));
  TEST_CHECK(mjc.contains("solreffriction"));
  TEST_CHECK(mjc.contains("solimpfriction"));
}

void physics_mjc_joint_authorship_usdc_roundtrip_test(void) {
  Stage src;
  std::string warn, err;
  const std::string path =
      UsdaFixturePath("physics-mjc-joint-authorship-001.usda");
  bool ok = LoadUSDFromFile(path, &src, &warn, &err);
  if (!ok) { TEST_MSG("load failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  Stage stage;
  ok = usdc_roundtrip_stage(src, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/World/Hinge", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;
  TEST_CHECK(joint->mjcJoint.has_value());
  if (!joint->mjcJoint.has_value()) return;

  const auto &mjc = joint->mjcJoint.value();
  TEST_CHECK(mjc.damping.authored());
  TEST_CHECK(approx_eq(mjc.damping.get_value(), 0.75));
  TEST_CHECK(!mjc.group.authored());
  TEST_CHECK(!mjc.stiffness.authored());
  TEST_CHECK(!mjc.armature.authored());
  TEST_CHECK(!mjc.frictionloss.authored());
  TEST_CHECK(!mjc.ref.authored());
  TEST_CHECK(!mjc.actuatorfrclimited.authored());
}

// ---------------------------------------------------------------------------
// 7b. PhysicsRevoluteJoint with all four mirror namespaces
//     (physics:* > physxJoint:* / physxLimit:* > state:* > mjc:*)
// ---------------------------------------------------------------------------
void physics_joint_physx_state_mirror_test(void) {
  // This authors the same physical quantity under all four cross-engine
  // namespaces, then verifies each one survives parse and reaches the
  // expected destination:
  //   - mjc:* keys land on the typed MjcJointAPI struct (joint.mjcJoint).
  //   - physxJoint:* / physxLimit:* / state:* keys are NOT consumed into
  //     a typed struct; they remain on joint.props for the reader to walk.
  // The reader-side priority (canonical > PhysX > Newton > MJC) is the
  // responsibility of each downstream consumer (web/sim/src/usd-physics.js,
  // src/usd_mjcf_reader.cc, etc.); here we only assert preservation.
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "MirrorHinge" (
    prepend apiSchemas = ["MjcJointAPI"]
)
{
    rel physics:body0 = </World/A>
    rel physics:body1 = </World/B>
    token physics:axis = "Y"

    uniform double mjc:stiffness = 2.5
    uniform double mjc:damping = 0.75
    uniform double mjc:armature = 0.01
    uniform double mjc:frictionloss = 0.05

    custom float physxJoint:armature = 0.01
    custom float physxJoint:jointFriction = 0.05
    custom float physxJoint:maxJointVelocity = 12.5
    custom float physxLimit:angular:damping = 0.75
    custom float physxLimit:angular:stiffness = 2.5

    custom float state:angular:physics:position = 15.0
    custom float state:angular:physics:velocity = -2.0
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/MirrorHinge", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  // mjc:* — typed MjcJointAPI struct.
  TEST_CHECK(joint->mjcJoint.has_value());
  if (joint->mjcJoint.has_value()) {
    const auto &m = joint->mjcJoint.value();
    TEST_CHECK(m.stiffness.get_value() == 2.5);
    TEST_CHECK(m.damping.get_value() == 0.75);
    TEST_CHECK(m.armature.get_value() == 0.01);
    TEST_CHECK(m.frictionloss.get_value() == 0.05);
  }

  // physxJoint:* / physxLimit:* / state:* — generic props map.
  TEST_CHECK(joint->props.count("physxJoint:armature") > 0);
  TEST_CHECK(joint->props.count("physxJoint:jointFriction") > 0);
  TEST_CHECK(joint->props.count("physxJoint:maxJointVelocity") > 0);
  TEST_CHECK(joint->props.count("physxLimit:angular:damping") > 0);
  TEST_CHECK(joint->props.count("physxLimit:angular:stiffness") > 0);
  TEST_CHECK(joint->props.count("state:angular:physics:position") > 0);
  TEST_CHECK(joint->props.count("state:angular:physics:velocity") > 0);
}

// ---------------------------------------------------------------------------
// 7c. PhysicsRevoluteJoint with all four namespaces — USDC round-trip.
//     Verifies sconv-physics.cc re-emits MjcJointAPI from the typed struct
//     (the reconstruct path consumes mjc:* into mjcJoint and removes them
//     from props), and the generic stage-converter.cc props-map pass
//     preserves physxJoint:* / physxLimit:* / state:* untouched.
// ---------------------------------------------------------------------------
void physics_joint_physx_state_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "MirrorHinge" (
    prepend apiSchemas = ["MjcJointAPI"]
)
{
    rel physics:body0 = </World/A>
    rel physics:body1 = </World/B>
    token physics:axis = "Y"
    float physics:lowerLimit = -90.0
    float physics:upperLimit = 90.0

    uniform double mjc:stiffness = 2.5
    uniform double mjc:damping = 0.75
    uniform double mjc:armature = 0.01
    uniform double mjc:frictionloss = 0.05

    custom float physxJoint:armature = 0.01
    custom float physxJoint:jointFriction = 0.05
    custom float physxJoint:maxJointVelocity = 12.5
    custom float physxLimit:angular:damping = 0.75
    custom float physxLimit:angular:stiffness = 2.5

    custom float state:angular:physics:position = 15.0
    custom float state:angular:physics:velocity = -2.0
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/MirrorHinge", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  // Canonical UsdPhysics — typed limits.
  auto lo = joint->lowerLimit.get_value();
  auto hi = joint->upperLimit.get_value();
  TEST_CHECK(lo.has_value() && lo.value() == -90.0f);
  TEST_CHECK(hi.has_value() && hi.value() ==  90.0f);

  // MJC — round-trips through the typed MjcJointAPI struct.
  TEST_CHECK(joint->mjcJoint.has_value());
  if (joint->mjcJoint.has_value()) {
    const auto &m = joint->mjcJoint.value();
    TEST_CHECK(m.stiffness.get_value() == 2.5);
    TEST_CHECK(m.damping.get_value() == 0.75);
    TEST_CHECK(m.armature.get_value() == 0.01);
    TEST_CHECK(m.frictionloss.get_value() == 0.05);
  }

  // PhysX + state — round-trip via the generic props map. Check values
  // not just presence so a botched value-encoding in the crate writer
  // would surface immediately.
  double v = 0.0;
  TEST_CHECK(get_prop_num(joint->props, "physxJoint:armature", &v));
  TEST_CHECK(approx_eq(v, 0.01));
  TEST_CHECK(get_prop_num(joint->props, "physxJoint:jointFriction", &v));
  TEST_CHECK(approx_eq(v, 0.05));
  TEST_CHECK(get_prop_num(joint->props, "physxJoint:maxJointVelocity", &v));
  TEST_CHECK(approx_eq(v, 12.5));
  TEST_CHECK(get_prop_num(joint->props, "physxLimit:angular:damping", &v));
  TEST_CHECK(approx_eq(v, 0.75));
  TEST_CHECK(get_prop_num(joint->props, "physxLimit:angular:stiffness", &v));
  TEST_CHECK(approx_eq(v, 2.5));
  TEST_CHECK(get_prop_num(joint->props, "state:angular:physics:position", &v));
  TEST_CHECK(approx_eq(v, 15.0));
  TEST_CHECK(get_prop_num(joint->props, "state:angular:physics:velocity", &v));
  TEST_CHECK(approx_eq(v, -2.0));
}

// ---------------------------------------------------------------------------
// 7d. PhysicsPrismaticJoint with state:linear:physics:* only.
//     Mirrors the physics-state-init.usda test scene — exercises the
//     prismatic path (only the revolute axis is covered by 7b/7c).
// ---------------------------------------------------------------------------
void physics_prismatic_state_init_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsPrismaticJoint "Slider"
{
    rel physics:body0 = </World/Cart>
    rel physics:body1 = </World/Pole>
    token physics:axis = "Z"

    custom float state:linear:physics:position = 0.05
    custom float state:linear:physics:velocity = -0.5
    custom float physxLimit:linear:damping = 1.2
    custom float physxLimit:linear:stiffness = 30.0
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/Slider", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsPrismaticJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  // Prismatic state — meters / m-per-sec per the USD PhysX convention
  // (no degrees conversion, unlike revolute).
  double v = 0.0;
  TEST_CHECK(get_prop_num(joint->props, "state:linear:physics:position", &v));
  TEST_CHECK(approx_eq(v, 0.05));
  TEST_CHECK(get_prop_num(joint->props, "state:linear:physics:velocity", &v));
  TEST_CHECK(approx_eq(v, -0.5));
  TEST_CHECK(get_prop_num(joint->props, "physxLimit:linear:damping", &v));
  TEST_CHECK(approx_eq(v, 1.2));
  TEST_CHECK(get_prop_num(joint->props, "physxLimit:linear:stiffness", &v));
  TEST_CHECK(approx_eq(v, 30.0));

  // No MjcJointAPI in this scene — the reconstruct path should NOT have
  // synthesized one from the physx*:* / state:* prefixes.
  TEST_CHECK(!joint->mjcJoint.has_value());
}

// ---------------------------------------------------------------------------
// 7e. Tydra physics-to-JSON includes the physx / state sub-blocks for
//     revolute joints (per the cross-engine mirror section in doc/usd.md).
// ---------------------------------------------------------------------------
void physics_joint_physx_state_to_json_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "Hinge"
{
    rel physics:body0 = </A>
    rel physics:body1 = </B>
    token physics:axis = "Z"

    custom float physxJoint:armature = 0.01
    custom float physxJoint:jointFriction = 0.05
    custom float physxLimit:angular:damping = 0.75
    custom float physxLimit:angular:stiffness = 2.5
    custom float state:angular:physics:position = 15.0
    custom float state:angular:physics:velocity = -2.0
}

def PhysicsPrismaticJoint "Slider"
{
    rel physics:body0 = </C>
    rel physics:body1 = </D>
    token physics:axis = "X"

    custom float physxLimit:linear:damping = 1.2
    custom float state:linear:physics:position = 0.05
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  std::string json;
  std::string json_err;
  tydra::PhysicsJsonExportOptions opts;
  opts.include_mjc = true;
  bool json_ok = tydra::ConvertPhysicsToJson(stage, &json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  TEST_CHECK(!json.empty());
  if (json.empty()) return;

  // Revolute joint surfaces a `physx` and `state` sub-block.
  TEST_CHECK(json.find("\"physx\"")           != std::string::npos);
  TEST_CHECK(json.find("\"state\"")           != std::string::npos);
  TEST_CHECK(json.find("\"armature\"")        != std::string::npos);
  TEST_CHECK(json.find("\"jointFriction\"")   != std::string::npos);
  TEST_CHECK(json.find("\"damping\"")         != std::string::npos);
  TEST_CHECK(json.find("\"stiffness\"")       != std::string::npos);
  TEST_CHECK(json.find("\"position\"")        != std::string::npos);
  TEST_CHECK(json.find("\"velocity\"")        != std::string::npos);
  // Numeric round-trip — at least one of the values from above lands.
  TEST_CHECK(json.find("0.01")   != std::string::npos);
  TEST_CHECK(json.find("15")     != std::string::npos);
}

// ---------------------------------------------------------------------------
// 7f. MjcJointAPI round-trips through USDC. The reconstruct path consumes
//     mjc:* into MjcJointAPI and *removes* the keys from props, so without
//     the sconv-physics.cc re-emission, USDC -> USDA -> USDC would silently
//     drop joint damping / stiffness / armature / frictionloss.
// ---------------------------------------------------------------------------
void physics_joint_mjc_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "MjcOnlyHinge" (
    prepend apiSchemas = ["MjcJointAPI"]
)
{
    rel physics:body0 = </A>
    rel physics:body1 = </B>
    token physics:axis = "X"
    uniform double mjc:stiffness = 100.0
    uniform double mjc:damping = 10.0
    uniform double mjc:armature = 0.01
    uniform double mjc:frictionloss = 0.5
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/MjcOnlyHinge", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;
  TEST_CHECK(joint->mjcJoint.has_value());
  if (!joint->mjcJoint.has_value()) return;
  const auto &m = joint->mjcJoint.value();
  TEST_CHECK(m.stiffness.get_value()    == 100.0);
  TEST_CHECK(m.damping.get_value()      == 10.0);
  TEST_CHECK(m.armature.get_value()     == 0.01);
  TEST_CHECK(m.frictionloss.get_value() == 0.5);
}

// ---------------------------------------------------------------------------
// 7g. NewtonMimicAPI on a physics joint
// ---------------------------------------------------------------------------
void physics_joint_newton_mimic_api_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "Follower" (
    prepend apiSchemas = ["NewtonMimicAPI"]
)
{
    rel physics:body0 = </A>
    rel physics:body1 = </B>
    token physics:axis = "X"
    bool newton:mimicEnabled = true
    rel newton:mimicJoint = </Leader>
    float newton:mimicCoef0 = 0.25
    float newton:mimicCoef1 = -1
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/Follower", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const Prim *prim = *result;
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonMimicAPI));

  const auto *joint = prim->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;
  TEST_CHECK(joint->newtonMimic.has_value());
  if (!joint->newtonMimic.has_value()) return;

  const auto &n = joint->newtonMimic.value();
  TEST_CHECK(n.mimicEnabled.get_value() == true);
  TEST_CHECK(approx_eq(n.mimicCoef0.get_value(), 0.25));
  TEST_CHECK(approx_eq(n.mimicCoef1.get_value(), -1.0));
  TEST_CHECK(n.mimicJoint.authored());
  auto paths = n.mimicJoint.get_targetPaths();
  TEST_CHECK(paths.size() == 1);
  if (paths.size() == 1) {
    TEST_CHECK(paths[0].prim_part() == "/Leader");
  }
}

// ---------------------------------------------------------------------------
// 8. MjcActuator
// ---------------------------------------------------------------------------
void mjc_actuator_test(void) {
  const char *usda = R"(#usda 1.0

def MjcActuator "MotorActuator"
{
    rel mjc:target = </World/Joint1>
    uniform int mjc:group = 1
    uniform token mjc:dynType = "integrator"
    uniform token mjc:gainType = "fixed"
    uniform token mjc:biasType = "affine"
    uniform double mjc:crankLength = 0.1
    uniform bool mjc:actEarly = 1
    uniform double mjc:ctrlRange:min = -1
    uniform double mjc:ctrlRange:max = 1
    uniform token mjc:ctrlLimited = "true"
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/MotorActuator", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<MjcActuator>());

  const auto *act = prim->as<MjcActuator>();
  TEST_CHECK(act != nullptr);
  if (!act) return;

  TEST_CHECK(act->group.get_value() == 1);
  TEST_CHECK(act->dynType.get_value().str() == "integrator");
  TEST_CHECK(act->gainType.get_value().str() == "fixed");
  TEST_CHECK(act->biasType.get_value().str() == "affine");
  TEST_CHECK(act->crankLength.get_value() == 0.1);
  TEST_CHECK(act->actEarly.get_value() == true);
  TEST_CHECK(act->ctrlRange_min.get_value() == -1.0);
  TEST_CHECK(act->ctrlRange_max.get_value() == 1.0);
  TEST_CHECK(act->ctrlLimited.get_value().str() == "true");

  // Check target relationship
  TEST_CHECK(act->target.authored());
  auto paths = act->target.get_targetPaths();
  TEST_CHECK(paths.size() == 1);
  if (paths.size() == 1) {
    TEST_CHECK(paths[0].prim_part() == "/World/Joint1");
  }
}

// ---------------------------------------------------------------------------
// 8b. NewtonActuator
// ---------------------------------------------------------------------------
void newton_actuator_test(void) {
  const char *usda = R"(#usda 1.0

def NewtonActuator "FingerDrive" (
    prepend apiSchemas = ["NewtonPDControlAPI", "NewtonMaxEffortClampingAPI"]
)
{
    rel newton:targets = [</World/Joint1>, </World/Joint2>]
    float newton:kp = 120
    float newton:kd = 4
    float newton:constEffort = 0.5
    float newton:maxEffort = 30
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/FingerDrive", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<NewtonActuator>());
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonPDControlAPI));
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonMaxEffortClampingAPI));

  const auto *act = prim->as<NewtonActuator>();
  TEST_CHECK(act != nullptr);
  if (!act) return;
  TEST_CHECK(approx_eq(act->kp.get_value(), 120.0));
  TEST_CHECK(approx_eq(act->kd.get_value(), 4.0));
  TEST_CHECK(approx_eq(act->constEffort.get_value(), 0.5));
  TEST_CHECK(approx_eq(act->maxEffort.get_value(), 30.0));
  TEST_CHECK(act->targets.authored());
  auto paths = act->targets.get_targetPaths();
  TEST_CHECK(paths.size() == 2);
  if (paths.size() == 2) {
    TEST_CHECK(paths[0].prim_part() == "/World/Joint1");
    TEST_CHECK(paths[1].prim_part() == "/World/Joint2");
  }
}

// ---------------------------------------------------------------------------
// 8c. NewtonActuator extended control/clamping APIs
// ---------------------------------------------------------------------------
void newton_actuator_extended_api_test(void) {
  const char *usda = R"(#usda 1.0

def NewtonActuator "FingerDrive" (
    prepend apiSchemas = [
        "NewtonActuatorDelayAPI",
        "NewtonPIDControlAPI",
        "NewtonDCMotorClampingAPI",
        "NewtonPositionBasedClampingAPI"
    ]
)
{
    rel newton:targets = [</World/Joint1>]
    uniform int newton:delaySteps = 3
    float newton:kp = 120
    float newton:kd = 4
    float newton:ki = 0.75
    float newton:integralMax = 2
    float newton:maxMotorEffort = 30
    float newton:saturationEffort = 25
    float newton:velocityLimit = 12
    float[] newton:lookupPositions = [-1, 0, 1]
    float[] newton:lookupEfforts = [8, 10, 8]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/FingerDrive", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<NewtonActuator>());
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonActuatorDelayAPI));
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonPIDControlAPI));
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonDCMotorClampingAPI));
  TEST_CHECK(has_api(prim, APISchemas::APIName::NewtonPositionBasedClampingAPI));

  const auto *act = prim->as<NewtonActuator>();
  TEST_CHECK(act != nullptr);
  if (!act) return;
  TEST_CHECK(act->delaySteps.get_value() == 3);
  TEST_CHECK(approx_eq(act->ki.get_value(), 0.75));
  TEST_CHECK(approx_eq(act->integralMax.get_value(), 2.0));
  TEST_CHECK(approx_eq(act->maxMotorEffort.get_value(), 30.0));
  TEST_CHECK(approx_eq(act->saturationEffort.get_value(), 25.0));
  TEST_CHECK(approx_eq(act->velocityLimit.get_value(), 12.0));
  auto positions = act->lookupPositions.get_value();
  auto efforts = act->lookupEfforts.get_value();
  TEST_CHECK(positions.has_value());
  TEST_CHECK(efforts.has_value());
  if (positions.has_value()) {
    TEST_CHECK(positions.value().size() == 3);
    TEST_CHECK(approx_eq(positions.value()[0], -1.0));
    TEST_CHECK(approx_eq(positions.value()[2], 1.0));
  }
  if (efforts.has_value()) {
    TEST_CHECK(efforts.value().size() == 3);
    TEST_CHECK(approx_eq(efforts.value()[1], 10.0));
  }
}

// ---------------------------------------------------------------------------
// 9. MjcTendon
// ---------------------------------------------------------------------------
void mjc_tendon_test(void) {
  const char *usda = R"(#usda 1.0

def MjcTendon "SpatialTendon"
{
    uniform token mjc:type = "spatial"
    uniform int mjc:group = 0
    uniform double mjc:stiffness = 50
    uniform double mjc:damping = 5
    uniform double mjc:width = 0.005
    uniform color4f mjc:rgba = (0.8, 0.2, 0.2, 1)
    uniform token mjc:limited = "true"
    uniform double mjc:range:min = 0.1
    uniform double mjc:range:max = 0.5
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/SpatialTendon", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<MjcTendon>());

  const auto *tendon = prim->as<MjcTendon>();
  TEST_CHECK(tendon != nullptr);
  if (!tendon) return;

  TEST_CHECK(tendon->type.get_value().str() == "spatial");
  TEST_CHECK(tendon->stiffness.get_value() == 50.0);
  TEST_CHECK(tendon->damping.get_value() == 5.0);
  TEST_CHECK(tendon->width.get_value() == 0.005);
  TEST_CHECK(tendon->limited.get_value().str() == "true");
  TEST_CHECK(tendon->range_min.get_value() == 0.1);
  TEST_CHECK(tendon->range_max.get_value() == 0.5);
}

void physics_mjc_tendon_full_usdc_roundtrip_test(void) {
  Stage src;
  std::string warn, err;
  const std::string path = UsdaFixturePath("physics-mjc-tendon-full-001.usda");
  bool ok = LoadUSDFromFile(path, &src, &warn, &err);
  if (!ok) { TEST_MSG("load failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  Stage stage;
  ok = usdc_roundtrip_stage(src, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/World/FullTendon", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *t = (*result)->as<MjcTendon>();
  TEST_CHECK(t != nullptr);
  if (!t) return;

  TEST_CHECK(t->type.authored() && t->type.get_value().str() == "spatial");
  TEST_CHECK(t->path.authored());
  TEST_CHECK(t->path.get_targetPaths().size() == 2);
  TEST_CHECK(t->sideSites.authored());
  TEST_CHECK(t->sideSites.get_targetPaths().size() == 1);
  auto coef = t->path_coef.get_value();
  TEST_CHECK(coef.has_value() && coef.value().size() == 2);
  if (coef.has_value() && coef.value().size() == 2) {
    TEST_CHECK(approx_eq(coef.value()[0], 1.0));
    TEST_CHECK(approx_eq(coef.value()[1], -0.5));
  }
  auto divisors = t->path_divisors.get_value();
  TEST_CHECK(divisors.has_value() && divisors.value().size() == 2);
  TEST_CHECK(t->limited.authored() && t->limited.get_value().str() == "true");
  TEST_CHECK(t->actuatorfrclimited.authored() &&
             t->actuatorfrclimited.get_value().str() == "false");
  TEST_CHECK(t->range_min.authored() && approx_eq(t->range_min.get_value(), 0.1));
  TEST_CHECK(t->range_max.authored() && approx_eq(t->range_max.get_value(), 0.9));
  TEST_CHECK(t->actuatorfrcrange_min.authored() &&
             approx_eq(t->actuatorfrcrange_min.get_value(), -3.0));
  TEST_CHECK(t->actuatorfrcrange_max.authored() &&
             approx_eq(t->actuatorfrcrange_max.get_value(), 4.0));
  TEST_CHECK(t->solreflimit.get_value().has_value());
  TEST_CHECK(t->solimplimit.get_value().has_value());
  TEST_CHECK(t->solreffriction.get_value().has_value());
  TEST_CHECK(t->solimpfriction.get_value().has_value());
  TEST_CHECK(t->margin.authored() && approx_eq(t->margin.get_value(), 0.02));
  TEST_CHECK(t->frictionloss.authored() &&
             approx_eq(t->frictionloss.get_value(), 0.3));
  TEST_CHECK(t->springlength.get_value().has_value());
  TEST_CHECK(t->stiffness.authored() && approx_eq(t->stiffness.get_value(), 120.0));
  TEST_CHECK(t->damping.authored() && approx_eq(t->damping.get_value(), 3.0));
  TEST_CHECK(t->armature.authored() && approx_eq(t->armature.get_value(), 0.02));
  TEST_CHECK(t->width.authored() && approx_eq(t->width.get_value(), 0.006));
  TEST_CHECK(t->rgba.authored());
}

// ---------------------------------------------------------------------------
// 10. MjcKeyframe
// ---------------------------------------------------------------------------
void mjc_keyframe_test(void) {
  const char *usda = R"(#usda 1.0

def MjcKeyframe "InitialState"
{
    double[] mjc:qpos = [0, 0, 0.5, 1, 0, 0, 0]
    double[] mjc:qvel = [0, 0, 0, 0, 0, 0]
    double[] mjc:ctrl = [0.1, 0.2, 0.3]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/InitialState", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<MjcKeyframe>());

  const auto *kf = prim->as<MjcKeyframe>();
  TEST_CHECK(kf != nullptr);
  if (!kf) return;

  // Check qpos
  auto qpos_opt = kf->qpos.get_value();
  TEST_CHECK(qpos_opt.has_value());
  if (qpos_opt.has_value()) {
    const auto &qpos = qpos_opt.value();
    TEST_CHECK(qpos.size() == 7);
    if (qpos.size() >= 3) {
      TEST_CHECK(qpos[2] == 0.5);
    }
  }

  // Check ctrl
  auto ctrl_opt = kf->ctrl.get_value();
  TEST_CHECK(ctrl_opt.has_value());
  if (ctrl_opt.has_value()) {
    const auto &ctrl = ctrl_opt.value();
    TEST_CHECK(ctrl.size() == 3);
    if (ctrl.size() >= 3) {
      TEST_CHECK(ctrl[0] == 0.1);
      TEST_CHECK(ctrl[1] == 0.2);
      TEST_CHECK(ctrl[2] == 0.3);
    }
  }
}

// ---------------------------------------------------------------------------
// 11. PrettyPrint round-trip: parse -> export -> reparse
// ---------------------------------------------------------------------------
void physics_pprint_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "Scene"
{
    vector3f physics:gravityDirection = (0, -1, 0)
    float physics:gravityMagnitude = 9.81
}

def PhysicsRevoluteJoint "Joint1"
{
    rel physics:body0 = </Body0>
    rel physics:body1 = </Body1>
    token physics:axis = "X"
}

def MjcActuator "Motor"
{
    rel mjc:target = </Joint1>
    uniform int mjc:group = 0
}

def MjcKeyframe "Key0"
{
    double[] mjc:qpos = [0, 0, 1]
}
)";
  // Parse first time
  Stage stage1;
  std::string warn1, err1;
  bool ok1 = parse_usda(usda, &stage1, &warn1, &err1);
  if (!ok1) { TEST_MSG("first parse failed: %s", err1.c_str()); }
  TEST_CHECK(ok1);
  if (!ok1) return;

  // Export to string
  std::string exported = stage1.ExportToString();
  TEST_CHECK(!exported.empty());
  if (exported.empty()) {
    TEST_MSG("ExportToString returned empty");
    return;
  }

  // Parse again from exported string
  Stage stage2;
  std::string warn2, err2;
  bool ok2 = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(exported.c_str()),
      exported.size(), "roundtrip.usda", &stage2, &warn2, &err2);
  if (!ok2) { TEST_MSG("roundtrip parse failed: %s\nExported:\n%s", err2.c_str(), exported.c_str()); }
  TEST_CHECK(ok2);
  if (!ok2) return;

  // Verify types are preserved
  {
    auto r = stage2.GetPrimAtPath(Path("/Scene", ""));
    TEST_CHECK(bool(r));
    if (r) TEST_CHECK((*r)->is<PhysicsScene>());
  }
  {
    auto r = stage2.GetPrimAtPath(Path("/Joint1", ""));
    TEST_CHECK(bool(r));
    if (r) TEST_CHECK((*r)->is<PhysicsRevoluteJoint>());
  }
  {
    auto r = stage2.GetPrimAtPath(Path("/Motor", ""));
    TEST_CHECK(bool(r));
    if (r) TEST_CHECK((*r)->is<MjcActuator>());
  }
  {
    auto r = stage2.GetPrimAtPath(Path("/Key0", ""));
    TEST_CHECK(bool(r));
    if (r) TEST_CHECK((*r)->is<MjcKeyframe>());
  }
}

// ---------------------------------------------------------------------------
// 12. Tydra physics-to-JSON export
// ---------------------------------------------------------------------------
void physics_to_json_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "Scene"
{
    vector3f physics:gravityDirection = (0, 0, -1)
    float physics:gravityMagnitude = 9.81
    uniform double mjc:option:timestep = 0.002
    uniform token mjc:option:integrator = "euler"
}

def Scope "Joints"
{
    def PhysicsRevoluteJoint "Hinge"
    {
        rel physics:body0 = </Body0>
        rel physics:body1 = </Body1>
        token physics:axis = "Z"
    }
}

def Scope "Actuators"
{
    def MjcActuator "Motor"
    {
        rel mjc:target = </Joints/Hinge>
        uniform int mjc:group = 0
    }
}

def MjcKeyframe "Key0"
{
    double[] mjc:qpos = [0, 0.5]
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  std::string json;
  std::string json_err;
  tydra::PhysicsJsonExportOptions opts;
  opts.include_mjc = true;
  bool json_ok = tydra::ConvertPhysicsToJson(stage, &json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  TEST_CHECK(!json.empty());

  if (!json.empty()) {
    // Basic content checks (substring search)
    TEST_CHECK(json.find("\"physicsScene\"") != std::string::npos);
    TEST_CHECK(json.find("\"gravityMagnitude\"") != std::string::npos);
    TEST_CHECK(json.find("\"joints\"") != std::string::npos);
    TEST_CHECK(json.find("\"actuators\"") != std::string::npos);
    TEST_CHECK(json.find("\"keyframes\"") != std::string::npos);
    TEST_CHECK(json.find("\"timestep\"") != std::string::npos);
    TEST_CHECK(json.find("\"euler\"") != std::string::npos);
  }
}

void physics_to_json_include_defaults_test(void) {
  const char *usda = R"(#usda 1.0

def "World"
{
    def MjcActuator "act"
    {
        rel mjc:target = </World/Joint>
    }

    def MjcSensor "sensor"
    {
        uniform token mjc:type = "framepos"
        uniform token mjc:objname = "body, }"
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  std::string compact_json;
  std::string json_err;
  tydra::PhysicsJsonExportOptions opts;
  opts.include_mjc = true;
  opts.include_defaults = false;
  bool json_ok = tydra::ConvertPhysicsToJson(stage, &compact_json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  if (!json_ok) return;
  TEST_CHECK(compact_json.find("\"target\": \"/World/Joint\"") !=
             std::string::npos);
  TEST_CHECK(compact_json.find("\"group\"") == std::string::npos);
  TEST_CHECK(compact_json.find("\"dynType\"") == std::string::npos);
  TEST_CHECK(compact_json.find("\"ctrlLimited\"") == std::string::npos);
  nlohmann::json compact = parse_json_noexcept(compact_json);
  if (compact.is_discarded()) {
    TEST_MSG("compact JSON failed to parse:\n%s", compact_json.c_str());
  }
  TEST_CHECK(!compact.is_discarded());
  if (!compact.is_discarded()) {
    TEST_CHECK(compact["sensors"][0]["objname"] == "body, }");
  }

  std::string defaults_json;
  opts.include_defaults = true;
  json_ok = tydra::ConvertPhysicsToJson(stage, &defaults_json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  if (!json_ok) return;
  TEST_CHECK(defaults_json.find("\"group\": 0") != std::string::npos);
  TEST_CHECK(defaults_json.find("\"dynType\": \"none\"") != std::string::npos);
  TEST_CHECK(defaults_json.find("\"ctrlLimited\": \"auto\"") !=
             std::string::npos);
  nlohmann::json defaults = parse_json_noexcept(defaults_json);
  if (defaults.is_discarded()) {
    TEST_MSG("defaults JSON failed to parse:\n%s", defaults_json.c_str());
  }
  TEST_CHECK(!defaults.is_discarded());
  if (!defaults.is_discarded()) {
    TEST_CHECK(defaults["sensors"][0]["objname"] == "body, }");
  }
}

void physics_mjc_tendon_full_to_json_test(void) {
  Stage stage;
  std::string warn, err;
  const std::string path = UsdaFixturePath("physics-mjc-tendon-full-001.usda");
  bool ok = LoadUSDFromFile(path, &stage, &warn, &err);
  if (!ok) { TEST_MSG("load failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  std::string json;
  std::string json_err;
  tydra::PhysicsJsonExportOptions opts;
  opts.include_mjc = true;
  bool json_ok = tydra::ConvertPhysicsToJson(stage, &json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  if (!json_ok) return;

  TEST_CHECK(json.find("\"tendons\"") != std::string::npos);
  TEST_CHECK(json.find("\"route\": [\"/World/Sites/p0\", \"/World/Sites/p1\"]") !=
             std::string::npos);
  TEST_CHECK(json.find("\"sideSites\": [\"/World/Sites/p1\"]") !=
             std::string::npos);
  TEST_CHECK(json.find("\"routeIndices\": [0, 1]") != std::string::npos);
  TEST_CHECK(json.find("\"sideSiteIndices\": [1]") != std::string::npos);
  TEST_CHECK(json.find("\"routeSegments\": [0, 1]") != std::string::npos);
  TEST_CHECK(json.find("\"routeDivisors\": [1, 2]") != std::string::npos);
  TEST_CHECK(json.find("\"routeCoef\": [1, -0.5]") != std::string::npos);
  TEST_CHECK(json.find("\"actuatorfrclimited\": \"false\"") != std::string::npos);
  TEST_CHECK(json.find("\"actuatorfrcrange_min\": -3") != std::string::npos);
  TEST_CHECK(json.find("\"actuatorfrcrange_max\": 4") != std::string::npos);
  TEST_CHECK(json.find("\"springlength\": [0.2, 0.8]") != std::string::npos);
  TEST_CHECK(json.find("\"width\": 0.006") != std::string::npos);
  TEST_CHECK(json.find("\"rgba\": [0.2, 0.3, 0.4, 0.5]") !=
             std::string::npos);
}

// ---------------------------------------------------------------------------
// 14. PhysicsCollisionGroup
// ---------------------------------------------------------------------------
void physics_collision_group_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsCollisionGroup "GroupA"
{
    token physics:mergeGroup = "default"
    bool physics:invertFilteredGroups = 0
    rel physics:filteredGroups = </GroupB>
}

def PhysicsCollisionGroup "GroupB"
{
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/GroupA", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<PhysicsCollisionGroup>());
  const auto *grp = prim->as<PhysicsCollisionGroup>();
  TEST_CHECK(grp != nullptr);
  if (!grp) return;

  // Check mergeGroup
  auto mg = grp->mergeGroup.get_value();
  TEST_CHECK(mg.has_value());
  if (mg.has_value()) {
    TEST_CHECK(mg.value().str() == "default");
  }

  // Check invertFilteredGroups
  TEST_CHECK(grp->invertFilteredGroups.get_value() == false);

  // Check filteredGroups relationship
  TEST_CHECK(grp->filteredGroups.authored());
}

// ---------------------------------------------------------------------------
// 14b. PhysicsCollisionGroup: collection:colliders:includes accessor.
// Exercises GetPhysicsCollidersCollection() — the auto-applied
// CollectionAPI:colliders membership rel.
// ---------------------------------------------------------------------------
void physics_collision_group_colliders_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsCollisionGroup "Rig" (
    prepend apiSchemas = ["CollectionAPI:colliders"]
)
{
    rel physics:filteredGroups = </Rig>
    rel collection:colliders:includes = [</Body1>, </Body2>, </Body3>]
}

def Cube "Body1" {}
def Cube "Body2" {}
def Cube "Body3" {}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/Rig", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const Prim *prim = *result;
  TEST_CHECK(prim->is<PhysicsCollisionGroup>());

  std::vector<Path> includes;
  std::vector<Path> excludes;
  bool got = GetPhysicsCollidersCollection(*prim, &includes, &excludes);
  TEST_CHECK(got);
  TEST_CHECK(includes.size() == 3);
  TEST_CHECK(excludes.empty());
  if (includes.size() == 3) {
    TEST_CHECK(includes[0].full_path_name() == "/Body1");
    TEST_CHECK(includes[1].full_path_name() == "/Body2");
    TEST_CHECK(includes[2].full_path_name() == "/Body3");
  }

  // Self-filter rel must round-trip.
  const auto *grp = prim->as<PhysicsCollisionGroup>();
  TEST_CHECK(grp->filteredGroups.authored());
}

// ---------------------------------------------------------------------------
// 14c. PhysicsFilteredPairsAPI typed accessor on a Cube.
// Exercises GetPhysicsFilteredPairsAPI() and
// PhysicsFilteredPairsAPI::get_filtered_pair_paths().
// ---------------------------------------------------------------------------
void physics_filtered_pairs_api_test(void) {
  const char *usda = R"(#usda 1.0

def Cube "BodyA" (
    prepend apiSchemas = ["PhysicsFilteredPairsAPI"]
)
{
    rel physics:filteredPairs = [</BodyB>, </BodyC>]
}

def Cube "BodyB" {}
def Cube "BodyC" {}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/BodyA", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const Prim *prim = *result;

  PhysicsFilteredPairsAPI api;
  bool got = GetPhysicsFilteredPairsAPI(*prim, &api);
  TEST_CHECK(got);
  TEST_CHECK(api.filteredPairs.authored());

  std::vector<Path> targets = api.get_filtered_pair_paths();
  TEST_CHECK(targets.size() == 2);
  if (targets.size() == 2) {
    TEST_CHECK(targets[0].full_path_name() == "/BodyB");
    TEST_CHECK(targets[1].full_path_name() == "/BodyC");
  }

  // Negative case: a prim *without* the API schema must yield false.
  auto bres = stage.GetPrimAtPath(Path("/BodyB", ""));
  TEST_CHECK(bool(bres));
  if (bres) {
    PhysicsFilteredPairsAPI api2;
    TEST_CHECK(!GetPhysicsFilteredPairsAPI(**bres, &api2));
  }
}

// ---------------------------------------------------------------------------
// 14d. PhysicsCollisionGroup: invertFilteredGroups + multi-target rel.
// ---------------------------------------------------------------------------
void physics_collision_group_invert_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsCollisionGroup "G1"
{
    bool physics:invertFilteredGroups = 1
    rel physics:filteredGroups = </G2>
}

def PhysicsCollisionGroup "G2"
{
}

def PhysicsCollisionGroup "G3"
{
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto r = stage.GetPrimAtPath(Path("/G1", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const auto *g1 = (*r)->as<PhysicsCollisionGroup>();
  TEST_CHECK(g1 != nullptr);
  if (!g1) return;
  TEST_CHECK(g1->invertFilteredGroups.get_value() == true);
  TEST_CHECK(g1->filteredGroups.authored());

  // G2's invertFilteredGroups defaults to false.
  auto r2 = stage.GetPrimAtPath(Path("/G2", ""));
  if (r2) {
    const auto *g2 = (*r2)->as<PhysicsCollisionGroup>();
    if (g2) {
      TEST_CHECK(g2->invertFilteredGroups.get_value() == false);
      TEST_CHECK(!g2->filteredGroups.authored());
    }
  }
}

// ---------------------------------------------------------------------------
// 15. DriveAPI + LimitAPI on joint
// ---------------------------------------------------------------------------
void physics_drive_limit_api_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "Joint" (
    prepend apiSchemas = ["PhysicsDriveAPI:rotX", "PhysicsLimitAPI:rotX"]
)
{
    rel physics:body0 = </Arm>
    rel physics:body1 = </Forearm>
    token physics:axis = "X"

    token physics:drive:rotX:type = "force"
    float physics:drive:rotX:maxForce = 100
    float physics:drive:rotX:targetPosition = 1.57
    float physics:drive:rotX:stiffness = 50
    float physics:drive:rotX:damping = 10

    float physics:limit:rotX:low = -1.57
    float physics:limit:rotX:high = 1.57
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto result = stage.GetPrimAtPath(Path("/Joint", ""));
  TEST_CHECK(bool(result));
  if (!result) return;

  const Prim *prim = *result;
  TEST_CHECK(prim->is<PhysicsRevoluteJoint>());
  const auto *joint = prim->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  // DriveAPI and LimitAPI attributes use multi-apply namespace syntax
  // (physics:drive:rotX:*) and are preserved verbatim in the generic props map
  // for round-trip.
  TEST_CHECK(joint->props.count("physics:drive:rotX:type") > 0 ||
             joint->props.count("physics:drive:rotX:maxForce") > 0 ||
             joint->props.count("physics:drive:rotX:stiffness") > 0);
  TEST_CHECK(joint->props.count("physics:limit:rotX:low") > 0 ||
             joint->props.count("physics:limit:rotX:high") > 0);

  // Phase 1a: the reconstruct path now ALSO populates the typed
  // PhysicsJointBase::drives / ::limits maps (a read-model on top of props).
  TEST_CHECK(joint->drives.count("rotX") == 1);
  if (joint->drives.count("rotX") == 1) {
    const PhysicsDriveAPI &d = joint->drives.at("rotX");
    TEST_CHECK(d.dof == "rotX");
    TEST_CHECK(d.type.get_value().str() == "force");
    TEST_CHECK(d.maxForce.authored() && approx_eq(d.maxForce.get_value(), 100.0));
    TEST_CHECK(d.targetPosition.authored() &&
               approx_eq(d.targetPosition.get_value(), 1.57));
    TEST_CHECK(d.stiffness.authored() && approx_eq(d.stiffness.get_value(), 50.0));
    TEST_CHECK(d.damping.authored() && approx_eq(d.damping.get_value(), 10.0));
  }
  TEST_CHECK(joint->limits.count("rotX") == 1);
  if (joint->limits.count("rotX") == 1) {
    const PhysicsLimitAPI &l = joint->limits.at("rotX");
    TEST_CHECK(l.dof == "rotX");
    TEST_CHECK(l.low.authored() && approx_eq(l.low.get_value(), -1.57));
    TEST_CHECK(l.high.authored() && approx_eq(l.high.get_value(), 1.57));
  }
}

void physics_drive_limit_multi_dof_parse_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsJoint "D6" (
    prepend apiSchemas = [
        "PhysicsDriveAPI:transX",
        "PhysicsDriveAPI:rotZ",
        "PhysicsLimitAPI:transX",
        "PhysicsLimitAPI:rotZ"
    ]
)
{
    rel physics:body0 = </Base>
    rel physics:body1 = </Tool>

    token physics:drive:transX:type = "acceleration"
    float physics:drive:transX:maxForce = 10
    float physics:drive:transX:targetVelocity = 0.5
    float physics:drive:rotZ:stiffness = 25
    float physics:drive:rotZ:damping = 3

    float physics:limit:transX:low = -0.2
    float physics:limit:transX:high = 0.4
    float physics:limit:rotZ:low = -1
    float physics:limit:rotZ:high = 1

    custom float physics:drive:rotZ:maxForce:extra = 99
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/D6", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  TEST_CHECK(joint->drives.count("transX") == 1);
  TEST_CHECK(joint->drives.count("rotZ") == 1);
  TEST_CHECK(joint->drives.count("rotZ:maxForce") == 0);
  if (joint->drives.count("transX") == 1) {
    const PhysicsDriveAPI &d = joint->drives.at("transX");
    TEST_CHECK(d.dof == "transX");
    TEST_CHECK(d.type.authored() && d.type.get_value().str() == "acceleration");
    TEST_CHECK(d.maxForce.authored() && approx_eq(d.maxForce.get_value(), 10.0));
    TEST_CHECK(d.targetVelocity.authored() &&
               approx_eq(d.targetVelocity.get_value(), 0.5));
  }
  if (joint->drives.count("rotZ") == 1) {
    const PhysicsDriveAPI &d = joint->drives.at("rotZ");
    TEST_CHECK(d.dof == "rotZ");
    TEST_CHECK(d.stiffness.authored() && approx_eq(d.stiffness.get_value(), 25.0));
    TEST_CHECK(d.damping.authored() && approx_eq(d.damping.get_value(), 3.0));
    TEST_CHECK(!d.maxForce.authored());
  }

  TEST_CHECK(joint->limits.count("transX") == 1);
  TEST_CHECK(joint->limits.count("rotZ") == 1);
  if (joint->limits.count("transX") == 1) {
    const PhysicsLimitAPI &l = joint->limits.at("transX");
    TEST_CHECK(l.dof == "transX");
    TEST_CHECK(l.low.authored() && approx_eq(l.low.get_value(), -0.2));
    TEST_CHECK(l.high.authored() && approx_eq(l.high.get_value(), 0.4));
  }
  if (joint->limits.count("rotZ") == 1) {
    const PhysicsLimitAPI &l = joint->limits.at("rotZ");
    TEST_CHECK(l.dof == "rotZ");
    TEST_CHECK(l.low.authored() && approx_eq(l.low.get_value(), -1.0));
    TEST_CHECK(l.high.authored() && approx_eq(l.high.get_value(), 1.0));
  }

  TEST_CHECK(joint->props.count("physics:drive:rotZ:maxForce:extra") == 1);
  double v = 0.0;
  TEST_CHECK(get_prop_num(joint->props, "physics:drive:rotZ:maxForce:extra", &v));
  TEST_CHECK(approx_eq(v, 99.0));
}

// ---------------------------------------------------------------------------
// 15b. UsdPhysics schema defaults and missing RigidBodyAPI fields.
// ---------------------------------------------------------------------------
void physics_schema_defaults_and_owner_test(void) {
  Stage stage;
  std::string warn, err;
  const std::string path = UsdaFixturePath("physics-schema-defaults-001.usda");
  bool ok = LoadUSDFromFile(path, &stage, &warn, &err);
  if (!ok) { TEST_MSG("load failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto body_r = stage.GetPrimAtPath(Path("/World/Body", ""));
  TEST_CHECK(bool(body_r));
  if (!body_r) return;

  PhysicsRigidBodyAPI rb;
  TEST_CHECK(GetPhysicsRigidBodyAPI(**body_r, &rb));
  TEST_CHECK(rb.rigidBodyEnabled.get_value() == true);
  TEST_CHECK(rb.kinematicEnabled.get_value() == true);
  TEST_CHECK(rb.simulationOwner.authored());
  TEST_CHECK(rb.simulationOwner.get_targetPaths().size() == 1);
  if (rb.simulationOwner.get_targetPaths().size() == 1) {
    TEST_CHECK(rb.simulationOwner.get_targetPaths()[0].full_path_name() ==
               "/World/Scene");
  }

  PhysicsMassAPI mass;
  TEST_CHECK(GetPhysicsMassAPI(**body_r, &mass));
  value::point3f com = mass.centerOfMass.get_value();
  TEST_CHECK(std::isinf(com[0]) && com[0] < 0.0f);
  TEST_CHECK(std::isinf(com[1]) && com[1] < 0.0f);
  TEST_CHECK(std::isinf(com[2]) && com[2] < 0.0f);
  value::quatf pa = mass.principalAxes.get_value();
  TEST_CHECK(pa[0] == 0.0f && pa[1] == 0.0f && pa[2] == 0.0f &&
             pa[3] == 0.0f);

  auto joint_r = stage.GetPrimAtPath(Path("/World/Joint", ""));
  TEST_CHECK(bool(joint_r));
  if (!joint_r) return;
  const auto *joint = (*joint_r)->as<PhysicsJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;
  TEST_CHECK(joint->jointEnabled.get_value() == true);
  TEST_CHECK(joint->collisionEnabled.get_value() == false);
  TEST_CHECK(std::isinf(joint->breakForce.get_value()) &&
             joint->breakForce.get_value() > 0.0f);
  TEST_CHECK(std::isinf(joint->breakTorque.get_value()) &&
             joint->breakTorque.get_value() > 0.0f);
  TEST_CHECK(joint->excludeFromArticulation.get_value() == false);
}

void physics_spherical_schema_names_fixture_test(void) {
  Stage stage;
  std::string warn, err;
  const std::string path =
      UsdaFixturePath("physics-spherical-schema-names-001.usda");
  bool ok = LoadUSDFromFile(path, &stage, &warn, &err);
  if (!ok) { TEST_MSG("load failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto joint_r = stage.GetPrimAtPath(Path("/World/Ball", ""));
  TEST_CHECK(bool(joint_r));
  if (!joint_r) return;
  const auto *joint = (*joint_r)->as<PhysicsSphericalJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;
  TEST_CHECK(joint->coneAngle0Limit.authored());
  TEST_CHECK(joint->coneAngle1Limit.authored());
  TEST_CHECK(approx_eq(joint->coneAngle0Limit.get_value().value(), 30.0));
  TEST_CHECK(approx_eq(joint->coneAngle1Limit.get_value().value(), 45.0));
}

// ---------------------------------------------------------------------------
// 15. Mesh-per-geom collider convention.
//
// Exercises the schema convention adopted by lightgeom `export_usdz.py`,
// lightusd `urdf-to-usd`, and NVIDIA / Newton's `mujoco-usd-converter`:
//
//   * One UsdGeom.Mesh per source geom (no `visual_*` / `collision_*`
//     duplication).
//   * Applied APIs on collider meshes:
//       PhysicsCollisionAPI + PhysicsMeshCollisionAPI
//       MjcCollisionAPI + MjcImageableAPI    (codeless typed schemas)
//   * `purpose = "guide"` when MuJoCo geom.group ∉ {0,1,2}
//     (collision-only mesh).
//   * `physics:collisionEnabled = false` is the `mujoco-usd-converter`
//     opt-out: every Mesh carries PhysicsCollisionAPI, visual-only ones
//     disable it instead of dropping the API.
//
// This test parses a Mesh authored under all three sub-cases (visual-
// only, dual-purpose, collision-only, disabled) and checks that
// apiSchemas + purpose + relevant attrs are recoverable, then exports
// to physics-JSON and checks the surfaced fields.
// ---------------------------------------------------------------------------
void physics_mesh_collider_convention_test(void) {
  const char *usda = R"(#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Z"
)

def Xform "World"
{
    def Xform "Robot" (
        prepend apiSchemas = ["PhysicsRigidBodyAPI"]
    )
    {
        def Mesh "VisualOnly" (
            prepend apiSchemas = ["MjcImageableAPI"]
        )
        {
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            uniform int mjc:group = 0
        }

        def Mesh "VisualAndCollider" (
            prepend apiSchemas = [
                "PhysicsCollisionAPI",
                "PhysicsMeshCollisionAPI",
                "MjcCollisionAPI",
                "MjcImageableAPI",
            ]
        )
        {
            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            uniform token physics:approximation = "convexHull"
            uniform int mjc:group = 2
            uniform int mjc:condim = 3
        }

        def Mesh "ColliderOnly" (
            prepend apiSchemas = [
                "PhysicsCollisionAPI",
                "PhysicsMeshCollisionAPI",
                "MjcCollisionAPI",
                "MjcImageableAPI",
            ]
        )
        {
            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            uniform token purpose = "guide"
            uniform token physics:approximation = "convexHull"
            uniform int mjc:group = 3
            uniform int mjc:condim = 6
            uniform double mjc:margin = 0.001
        }

        def Mesh "DisabledCollider" (
            prepend apiSchemas = [
                "PhysicsCollisionAPI",
                "PhysicsMeshCollisionAPI",
            ]
        )
        {
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            uniform bool physics:collisionEnabled = 0
            uniform token physics:approximation = "convexHull"
        }
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = parse_usda(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("parse failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto has_api = [](const Prim *prim, APISchemas::APIName want) -> bool {
    if (!prim->metas().has_apiSchemas()) return false;
    const auto schemas = prim->metas().get_apiSchemas();
    for (const auto &n : schemas.names) {
      if (n.first == want) return true;
    }
    return false;
  };

  // ---- 1) Visual-only ----------------------------------------------------
  auto vo_r = stage.GetPrimAtPath(Path("/World/Robot/VisualOnly", ""));
  TEST_CHECK(bool(vo_r));
  if (vo_r) {
    const Prim *p = *vo_r;
    TEST_CHECK(p->is<GeomMesh>());
    TEST_CHECK(!has_api(p, APISchemas::APIName::PhysicsCollisionAPI));
    TEST_CHECK( has_api(p, APISchemas::APIName::MjcImageableAPI));
    if (const auto *m = p->as<GeomMesh>()) {
      TEST_CHECK(m->purpose.get_value() == Purpose::Default);
    }
  }

  // ---- 2) Dual-purpose (visual + collider, default group) ---------------
  auto vc_r = stage.GetPrimAtPath(Path("/World/Robot/VisualAndCollider", ""));
  TEST_CHECK(bool(vc_r));
  if (vc_r) {
    const Prim *p = *vc_r;
    TEST_CHECK(p->is<GeomMesh>());
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsMeshCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::MjcCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::MjcImageableAPI));
    if (const auto *m = p->as<GeomMesh>()) {
      // purpose NOT authored as guide for visible-default geoms.
      TEST_CHECK(m->purpose.get_value() == Purpose::Default);
      // physics:approximation = "convexHull" must round-trip into props.
      auto it = m->props.find("physics:approximation");
      TEST_CHECK(it != m->props.end());
    }
  }

  // ---- 3) Collision-only (purpose=guide, mjc:group=3) -------------------
  auto co_r = stage.GetPrimAtPath(Path("/World/Robot/ColliderOnly", ""));
  TEST_CHECK(bool(co_r));
  if (co_r) {
    const Prim *p = *co_r;
    TEST_CHECK(p->is<GeomMesh>());
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::MjcCollisionAPI));
    if (const auto *m = p->as<GeomMesh>()) {
      TEST_CHECK(m->purpose.get_value() == Purpose::Guide);
      // mjc:group authored as 3 — round-trips via props bag.
      auto it = m->props.find("mjc:group");
      TEST_CHECK(it != m->props.end());
    }
  }

  // ---- 4) Disabled-collider (collisionEnabled=false opt-out) ------------
  auto dc_r = stage.GetPrimAtPath(Path("/World/Robot/DisabledCollider", ""));
  TEST_CHECK(bool(dc_r));
  if (dc_r) {
    const Prim *p = *dc_r;
    TEST_CHECK(p->is<GeomMesh>());
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsCollisionAPI));
    if (const auto *m = p->as<GeomMesh>()) {
      auto it = m->props.find("physics:collisionEnabled");
      TEST_CHECK(it != m->props.end());
    }
  }

  // Note: tydra::ConvertPhysicsToJson scopes to PhysicsScene / Joint /
  // Actuator / Tendon / Keyframe — Mesh-collider surfacing lives in
  // web/binding.cc's extractPhysicsSceneJSON (`AppendPhysicsPrimJson`),
  // which is exercised from the web/js test harness, not here.
}

// ---------------------------------------------------------------------------
// 24. Newton collision/material APIs are known and generic attrs preserve.
// ---------------------------------------------------------------------------
void physics_newton_collision_material_api_test(void) {
  const char *usda = R"(#usda 1.0

def "World"
{
    def Material "Rubber" (
        prepend apiSchemas = ["PhysicsMaterialAPI", "NewtonMaterialAPI"]
    )
    {
        float physics:dynamicFriction = 0.9
        float newton:rollingFriction = 0.002
        float newton:torsionalFriction = 0.01
    }

    def Mesh "Collider" (
        prepend apiSchemas = [
            "PhysicsCollisionAPI",
            "PhysicsMeshCollisionAPI",
            "NewtonCollisionAPI",
            "NewtonMeshCollisionAPI"
        ]
    )
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        uniform token physics:approximation = "convexHull"
        float newton:contactMargin = 0.005
        float newton:contactGap = 0
        uniform int newton:maxHullVertices = 64
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto mat_r = stage.GetPrimAtPath(Path("/World/Rubber", ""));
  TEST_CHECK(bool(mat_r));
  if (mat_r) {
    const Prim *p = *mat_r;
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsMaterialAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonMaterialAPI));
    if (const auto *m = p->as<Material>()) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(m->props, "newton:rollingFriction", &v));
      TEST_CHECK(approx_eq(v, 0.002));
      TEST_CHECK(get_prop_num(m->props, "newton:torsionalFriction", &v));
      TEST_CHECK(approx_eq(v, 0.01));
    }
  }

  auto mesh_r = stage.GetPrimAtPath(Path("/World/Collider", ""));
  TEST_CHECK(bool(mesh_r));
  if (mesh_r) {
    const Prim *p = *mesh_r;
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsMeshCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonMeshCollisionAPI));
    if (const auto *m = p->as<GeomMesh>()) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(m->props, "newton:contactMargin", &v));
      TEST_CHECK(approx_eq(v, 0.005));
      TEST_CHECK(get_prop_num(m->props, "newton:contactGap", &v));
      TEST_CHECK(approx_eq(v, 0.0));
      auto it = m->props.find("newton:maxHullVertices");
      TEST_CHECK(it != m->props.end());
    }
  }
}

// ---------------------------------------------------------------------------
// 25. URDF/MJCF JSON conversion authors spherical joints.
// ---------------------------------------------------------------------------
void urdf_json_spherical_joint_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "BallJsonBot",
  "upAxis": "Z",
  "links": [
    { "name": "base", "inertial": { "mass": 1.0 } },
    { "name": "tip", "inertial": { "mass": 0.25 } }
  ],
  "joints": [
    {
      "name": "ball",
      "type": "spherical",
      "parent": "base",
      "child": "tip",
      "axis": [0, 1, 0],
      "axisToken": "Y",
      "originMatrix": [1, 0, 0, 0,
                       0, 1, 0, 0,
                       0, 0, 1, 0,
                       0, 0, 0, 1],
      "localRot0": [1, 0, 0, 0],
      "localRot1": [1, 0, 0, 0]
    }
  ]
})JSON";

  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(
      robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("ConvertURDFJsonToUSDStage failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto joint_r = stage.GetPrimAtPath(Path("/World/Joints/ball", ""));
  TEST_CHECK(bool(joint_r));
  if (!joint_r) return;

  const Prim *p = *joint_r;
  TEST_CHECK(p->is<PhysicsSphericalJoint>());
  const auto *joint = p->as<PhysicsSphericalJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  auto axis = joint->axis.get_value();
  TEST_CHECK(axis.has_value());
  if (axis.has_value()) TEST_CHECK(axis.value().str() == "Y");

  auto body0 = joint->body0.get_targetPaths();
  auto body1 = joint->body1.get_targetPaths();
  TEST_CHECK(body0.size() == 1);
  TEST_CHECK(body1.size() == 1);
  if (body0.size() == 1) TEST_CHECK(body0[0].prim_part() == "/World/Links/base");
  if (body1.size() == 1) TEST_CHECK(body1[0].prim_part() == "/World/Links/tip");
}

// ---------------------------------------------------------------------------
// 26. URDF JSON conversion authors Newton APIs for scene/colliders/actuators.
// ---------------------------------------------------------------------------
void urdf_json_newton_api_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "NewtonJsonBot",
  "upAxis": "Z",
  "gravity": [0, 0, -1],
  "timestep": 0.01,
  "newton": {
    "maxSolverIterations": 48,
    "gravityEnabled": true,
    "selfCollisionEnabled": false
  },
  "links": [
    {
      "name": "base",
      "inertial": {
        "mass": 1.0,
        "centerOfMass": [0, 0, 0],
        "diagonalInertia": [1, 1, 1]
      },
      "collisions": [
        {
          "name": "base_col",
          "matrix": [1, 0, 0, 0,
                     0, 1, 0, 0,
                     0, 0, 1, 0,
                     0, 0, 0, 1],
          "shape": { "type": "box" },
          "newton": {
            "contactMargin": 0.01,
            "contactGap": 0.002
          }
        }
      ]
    },
    {
      "name": "finger",
      "inertial": {
        "mass": 0.25,
        "centerOfMass": [0, 0, 0],
        "diagonalInertia": [0.1, 0.1, 0.1]
      }
    }
  ],
  "joints": [
    {
      "name": "hinge",
      "type": "revolute",
      "parent": "base",
      "child": "finger",
      "axis": [0, 0, 1],
      "axisToken": "Z",
      "originMatrix": [1, 0, 0, 0,
                       0, 1, 0, 0,
                       0, 0, 1, 0,
                       0, 0, 0, 1],
      "limit": { "lower": -1.0, "upper": 1.0 },
      "dynamics": { "damping": 0.1, "friction": 0.02 },
      "mimic": { "joint": "hinge", "multiplier": 1.0, "offset": 0.0 }
    }
  ],
  "actuators": [
    {
      "name": "hinge_drive",
      "joint": "hinge",
      "control": "pd",
      "kp": 80.0,
      "kd": 3.0,
      "maxEffort": 12.0,
      "delaySteps": 2
    }
  ]
})JSON";

  Stage tmp;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(
      robot_json, &tmp, &warn, &err);
  if (!ok) { TEST_MSG("ConvertURDFJsonToUSDStage failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  std::vector<uint8_t> bytes;
  ok = usdc::SaveAsUSDCToMemory(tmp, &bytes, &warn, &err);
  if (!ok) { TEST_MSG("SaveAsUSDCToMemory failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  Stage stage;
  ok = LoadUSDCFromMemory(bytes.data(), bytes.size(), "newton-json.usdc",
                          &stage, &warn, &err);
  if (!ok) { TEST_MSG("LoadUSDCFromMemory failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto scene_r = stage.GetPrimAtPath(Path("/World/PhysicsScene", ""));
  TEST_CHECK(bool(scene_r));
  if (scene_r) {
    const Prim *p = *scene_r;
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonSceneAPI));
    const auto *scene = p->as<PhysicsScene>();
    TEST_CHECK(scene != nullptr);
    if (scene && scene->newtonScene.has_value()) {
      TEST_CHECK(scene->newtonScene.value().maxSolverIterations.get_value() == 48);
      TEST_CHECK(scene->newtonScene.value().timeStepsPerSecond.get_value() == 100);
    }
  }

  auto base_r = stage.GetPrimAtPath(Path("/World/Links/base", ""));
  TEST_CHECK(bool(base_r));
  if (base_r) {
    const Prim *p = *base_r;
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsArticulationRootAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonArticulationRootAPI));
    TEST_CHECK(p->as<Xform>() != nullptr);
    if (const auto *x = p->as<Xform>()) {
      TEST_CHECK(x->props.find("newton:selfCollisionEnabled") != x->props.end());
    }
  }

  auto col_r = stage.GetPrimAtPath(Path("/World/Links/base/base_col", ""));
  TEST_CHECK(bool(col_r));
  if (col_r) {
    const Prim *p = *col_r;
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonCollisionAPI));
    if (const auto *cube = p->as<GeomCube>()) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(cube->props, "newton:contactMargin", &v));
      TEST_CHECK(approx_eq(v, 0.01));
      TEST_CHECK(get_prop_num(cube->props, "newton:contactGap", &v));
      TEST_CHECK(approx_eq(v, 0.002));
    }
  }

  auto joint_r = stage.GetPrimAtPath(Path("/World/Joints/hinge", ""));
  TEST_CHECK(bool(joint_r));
  if (joint_r) {
    const Prim *p = *joint_r;
    TEST_CHECK(has_api(p, APISchemas::APIName::MjcJointAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonMimicAPI));
    const auto *joint = p->as<PhysicsRevoluteJoint>();
    TEST_CHECK(joint != nullptr);
    if (joint) {
      TEST_CHECK(joint->newtonMimic.has_value());
      if (joint->newtonMimic.has_value()) {
        auto targets = joint->newtonMimic.value().mimicJoint.get_targetPaths();
        TEST_CHECK(targets.size() == 1);
        if (targets.size() == 1) {
          TEST_CHECK(targets[0].prim_part() == "/World/Joints/hinge");
        }
      }
    }
  }

  auto act_r = stage.GetPrimAtPath(Path("/World/Actuators/hinge_drive", ""));
  TEST_CHECK(bool(act_r));
  if (act_r) {
    const Prim *p = *act_r;
    TEST_CHECK(p->is<NewtonActuator>());
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonPDControlAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonMaxEffortClampingAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::NewtonActuatorDelayAPI));
    const auto *act = p->as<NewtonActuator>();
    TEST_CHECK(act != nullptr);
    if (act) {
      TEST_CHECK(approx_eq(act->kp.get_value(), 80.0));
      TEST_CHECK(approx_eq(act->kd.get_value(), 3.0));
      TEST_CHECK(approx_eq(act->maxEffort.get_value(), 12.0));
      TEST_CHECK(act->delaySteps.get_value() == 2);
      auto targets = act->targets.get_targetPaths();
      TEST_CHECK(targets.size() == 1);
      if (targets.size() == 1) {
        TEST_CHECK(targets[0].prim_part() == "/World/Joints/hinge");
      }
    }
  }
}

void urdf_json_mjcf_contact_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "MjcfContactBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "gravity": [0, 0, -1],
  "links": [
    {
      "name": "base",
      "collisions": [
        {
          "name": "floor",
          "matrix": [1, 0, 0, 0,
                     0, 1, 0, 0,
                     0, 0, 1, 0,
                     0, 0, 0, 1],
          "shape": { "type": "plane", "width": 24, "length": 68, "axis": "Z" },
          "mjc": {
            "geomSize": [12, 34, 0.25],
            "geomContype": 4,
            "geomConaffinity": 8,
            "geomFriction": [0.9, 0.02, 0.003],
            "priority": 7,
            "solref": [0.04, 1],
            "solimp": [0.8, 0.9, 0.02, 0.7, 3],
            "solmix": 0.33,
            "margin": 0.04,
            "gap": 0.02
          },
          "condim": 6
        },
        {
          "name": "plain_col",
          "matrix": [1, 0, 0, 0,
                     0, 1, 0, 0,
                     0, 0, 1, 0,
                     0, 0, 0, 1],
          "shape": { "type": "box" }
        }
      ]
    },
    { "name": "finger" }
  ],
  "filteredPairs": [
    { "body1": "base", "body2": "finger" }
  ]
})JSON";

  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(
      robot_json, &stage, &warn, &err);
  if (!ok) {
    TEST_MSG("ConvertURDFJsonToUSDStage failed: %s", err.c_str());
  }
  TEST_CHECK(ok);
  if (!ok) return;

  auto floor_r = stage.GetPrimAtPath(Path("/World/Links/base/floor", ""));
  TEST_CHECK(bool(floor_r));
  if (floor_r) {
    const Prim *p = *floor_r;
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsCollisionAPI));
    TEST_CHECK(has_api(p, APISchemas::APIName::MjcCollisionAPI));
    const auto *plane = p->as<GeomPlane>();
    TEST_CHECK(plane != nullptr);
    if (plane) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(plane->props, "mjc:geomContype", &v));
      TEST_CHECK(approx_eq(v, 4.0));
      TEST_CHECK(get_prop_num(plane->props, "mjc:geomConaffinity", &v));
      TEST_CHECK(approx_eq(v, 8.0));
      TEST_CHECK(get_prop_num(plane->props, "mjc:priority", &v));
      TEST_CHECK(approx_eq(v, 7.0));
      TEST_CHECK(get_prop_num(plane->props, "mjc:condim", &v));
      TEST_CHECK(approx_eq(v, 6.0));
      TEST_CHECK(get_prop_num(plane->props, "mjc:solmix", &v));
      TEST_CHECK(approx_eq(v, 0.33));
      TEST_CHECK(get_prop_num(plane->props, "mjc:margin", &v));
      TEST_CHECK(approx_eq(v, 0.04));
      TEST_CHECK(get_prop_num(plane->props, "mjc:gap", &v));
      TEST_CHECK(approx_eq(v, 0.02));
      TEST_CHECK(get_prop_num(plane->props, "newton:contactMargin", &v));
      TEST_CHECK(approx_eq(v, 0.04));
      TEST_CHECK(get_prop_num(plane->props, "newton:contactGap", &v));
      TEST_CHECK(approx_eq(v, 0.02));

      auto check_vec = [&](const char *key, size_t n) {
        auto it = plane->props.find(key);
        TEST_CHECK(it != plane->props.end());
        if (it == plane->props.end() || !it->second.is_attribute()) return;
        auto vals = it->second.get_attribute().get_value<std::vector<double>>();
        TEST_CHECK(vals.has_value());
        if (vals.has_value()) {
          TEST_CHECK(vals.value().size() == n);
        }
      };
      check_vec("mjc:geomSize", 3);
      check_vec("mjc:geomFriction", 3);
      check_vec("mjc:solref", 2);
      check_vec("mjc:solimp", 5);
    }
  }

  auto plain_r = stage.GetPrimAtPath(Path("/World/Links/base/plain_col", ""));
  TEST_CHECK(bool(plain_r));
  if (plain_r) {
    const auto *cube = (*plain_r)->as<GeomCube>();
    TEST_CHECK(cube != nullptr);
    if (cube) {
      TEST_CHECK(cube->props.find("mjc:group") == cube->props.end());
      TEST_CHECK(cube->props.find("mjc:condim") == cube->props.end());
    }
  }

  auto base_r = stage.GetPrimAtPath(Path("/World/Links/base", ""));
  TEST_CHECK(bool(base_r));
  if (base_r) {
    const Prim *p = *base_r;
    TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsFilteredPairsAPI));
    const Xform *xform = p->data().as<Xform>();
    TEST_CHECK(xform != nullptr);
    auto it = xform ? xform->props.find("physics:filteredPairs") :
                      std::map<std::string, Property>::const_iterator{};
    TEST_CHECK(xform && it != xform->props.end());
    if (xform && it != xform->props.end()) {
      TEST_CHECK(it->second.is_relationship());
      auto targets = it->second.get_relationship().targetPathVector;
      TEST_CHECK(targets.size() == 1);
      if (targets.size() == 1) {
        TEST_CHECK(targets[0].prim_part() == "/World/Links/finger");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// upAxis handling for URDF/MJCF export (see doc/usd-physics-upAxis.md):
//  - `physics:axis` is a LOCAL-frame token: it must be passed through unchanged
//    regardless of the stage upAxis (remapping it would double-rotate).
//  - URDF/MJCF source data is Z-up; a Y-up stage must be reconciled with a
//    SINGLE corrective root rotation Rx(-90deg) on /World, never per-property.
// ---------------------------------------------------------------------------
void physics_urdf_upaxis_axis_invariant_test(void) {
  // Same Z-up robot (joint axis token "Z"), exported once per up axis.
  auto convert = [](const char *up_axis, const char *gravity_json,
                    Stage *out) -> bool {
    const std::string robot_json =
        std::string("{\n  \"name\": \"AxisBot\",\n  \"upAxis\": \"") + up_axis +
        "\",\n  \"gravity\": " + gravity_json +
        ",\n"
        "  \"links\": [\n"
        "    { \"name\": \"base\", \"inertial\": { \"mass\": 1.0 } },\n"
        "    { \"name\": \"arm\", \"inertial\": { \"mass\": 0.25 } }\n"
        "  ],\n"
        "  \"joints\": [\n"
        "    { \"name\": \"hinge\", \"type\": \"revolute\",\n"
        "      \"parent\": \"base\", \"child\": \"arm\",\n"
        "      \"axis\": [0, 0, 1], \"axisToken\": \"Z\",\n"
        "      \"originMatrix\": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],\n"
        "      \"limit\": { \"lower\": -1.0, \"upper\": 1.0 } }\n"
        "  ]\n}";
    std::string warn, err;
    bool ok =
        lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, out, &warn, &err);
    if (!ok) { TEST_MSG("convert (%s) failed: %s", up_axis, err.c_str()); }
    return ok;
  };

  // Reads /World/Joints/hinge's physics:axis token. The invariant under test:
  // it is ALWAYS "Z" (never remapped to the stage up axis).
  auto joint_axis = [](Stage *stage, std::string *out_axis) -> bool {
    auto r = stage->GetPrimAtPath(Path("/World/Joints/hinge", ""));
    if (!r) return false;
    const auto *joint = (*r)->as<PhysicsRevoluteJoint>();
    if (!joint) return false;
    auto a = joint->axis.get_value();
    if (!a.has_value()) return false;
    *out_axis = a.value().str();
    return true;
  };

  // --- Z-up: native target, no corrective rotation ---
  {
    Stage stage;
    TEST_CHECK(convert("Z", "[0, 0, -1]", &stage));
    TEST_CHECK(stage.metas().upAxis.get_value() == Axis::Z);

    // SI unit scale (MuJoCo is meters/kilograms) must be AUTHORED, not just left
    // at the in-memory fallback — otherwise it is not written to the layer and
    // usdchecker's StageMetadataChecker fails ("does not specify metersPerUnit").
    TEST_CHECK(stage.metas().metersPerUnit.authored());
    TEST_CHECK(stage.metas().metersPerUnit.get_value() == 1.0);
    TEST_CHECK(stage.metas().kilogramsPerUnit.authored());
    TEST_CHECK(stage.metas().kilogramsPerUnit.get_value() == 1.0);

    std::string axis;
    TEST_CHECK(joint_axis(&stage, &axis));
    TEST_CHECK(axis == "Z");  // pass-through

    auto wr = stage.GetPrimAtPath(Path("/World", ""));
    TEST_CHECK(bool(wr));
    if (wr) {
      const auto *world = (*wr)->as<Xform>();
      TEST_CHECK(world != nullptr);
      if (world) {
        // Z-up source -> Z-up stage: no corrective root rotation.
        TEST_CHECK(world->xformOps.empty());
      }
    }

    auto sr = stage.GetPrimAtPath(Path("/World/PhysicsScene", ""));
    TEST_CHECK(bool(sr));
    if (sr) {
      const auto *scene = (*sr)->as<PhysicsScene>();
      TEST_CHECK(scene != nullptr);
      auto g = scene->gravityDirection.get_value();
      TEST_CHECK(g.has_value());
      if (g.has_value()) {
        TEST_CHECK(approx_eq(g.value()[0], 0.0));
        TEST_CHECK(approx_eq(g.value()[1], 0.0));
        TEST_CHECK(approx_eq(g.value()[2], -1.0));
      }
    }
  }

  // --- Y-up: single corrective root rotation Rx(-90), axis token UNCHANGED ---
  {
    Stage stage;
    TEST_CHECK(convert("Y", "[0, -1, 0]", &stage));
    TEST_CHECK(stage.metas().upAxis.get_value() == Axis::Y);

    std::string axis;
    TEST_CHECK(joint_axis(&stage, &axis));
    TEST_CHECK(axis == "Z");  // STILL "Z" — not remapped to Y (no double-rotate)

    auto wr = stage.GetPrimAtPath(Path("/World", ""));
    TEST_CHECK(bool(wr));
    if (wr) {
      const auto *world = (*wr)->as<Xform>();
      TEST_CHECK(world != nullptr);
      if (world) {
        // Exactly one corrective op: xformOp:rotateX = -90 (+Z -> +Y).
        TEST_CHECK(world->xformOps.size() == 1);
        if (world->xformOps.size() == 1) {
          const XformOp &op = world->xformOps[0];
          TEST_CHECK(op.op_type == XformOp::OpType::RotateX);
          auto angle = op.get_value<double>();
          TEST_CHECK(angle.has_value());
          if (angle.has_value()) {
            TEST_CHECK(approx_eq(angle.value(), -90.0));
          }
        }
      }
    }

    auto sr = stage.GetPrimAtPath(Path("/World/PhysicsScene", ""));
    TEST_CHECK(bool(sr));
    if (sr) {
      const auto *scene = (*sr)->as<PhysicsScene>();
      TEST_CHECK(scene != nullptr);
      auto g = scene->gravityDirection.get_value();
      TEST_CHECK(g.has_value());
      if (g.has_value()) {
        TEST_CHECK(approx_eq(g.value()[0], 0.0));
        TEST_CHECK(approx_eq(g.value()[1], -1.0));
        TEST_CHECK(approx_eq(g.value()[2], 0.0));
      }
    }
  }
}

// ===========================================================================
// Large-scene crate-writer regression canaries.
//
// The "large scale scene support" work rewired the crate writer's value store
// (cross-spec VALUE-block dedup `1d61a59dc`, COW array storage `73cf5f680`,
// diet PrimVar `5af6afd43`, 16-byte Path SBO `bbaf277a8`). Physics attributes
// that are NOT re-emitted as typed structs by sconv-physics.cc round-trip
// through the generic stage-converter props pass + that value store, so a
// mis-association or value-sharing bug there would surface as a silently wrong
// physics value after USDC write. These tests author physics scenes, push them
// through usdc_roundtrip(), and assert VALUES (not just presence). The scalar
// values are chosen to be "dedup-collision-prone" (e.g. mass=2.5, density=1000
// are values likely to also appear on unrelated specs).
// ===========================================================================

// R1. PhysicsRigidBodyAPI + PhysicsMassAPI value survival through USDC.
void physics_rigidbody_mass_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Cube "Body" (
        prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI"]
    )
    {
        bool physics:rigidBodyEnabled = 0
        float physics:mass = 2.5
        float physics:density = 1000
        point3f physics:centerOfMass = (0.1, 0.2, 0.3)
        float3 physics:diagonalInertia = (1, 2, 3)
        quatf physics:principalAxes = (1, 0, 0, 0)
        vector3f physics:velocity = (0.5, 0, 0)
        vector3f physics:angularVelocity = (0, 0, 1)
        bool physics:startsAsleep = 1
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/World/Body", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const Prim *prim = *result;

  PhysicsRigidBodyAPI rb;
  TEST_CHECK(GetPhysicsRigidBodyAPI(*prim, &rb));
  TEST_CHECK(rb.rigidBodyEnabled.get_value() == false);
  TEST_CHECK(rb.startsAsleep.get_value() == true);
  {
    auto m = rb.mass.get_value();
    TEST_CHECK(m.has_value() && approx_eq(m.value(), 2.5));
    auto d = rb.density.get_value();
    TEST_CHECK(d.has_value() && approx_eq(d.value(), 1000.0));
    auto com = rb.centerOfMass.get_value();
    TEST_CHECK(com.has_value());
    if (com.has_value()) {
      TEST_CHECK(approx_eq(com.value()[0], 0.1));
      TEST_CHECK(approx_eq(com.value()[1], 0.2));
      TEST_CHECK(approx_eq(com.value()[2], 0.3));
    }
    auto in = rb.diagonalInertia.get_value();
    TEST_CHECK(in.has_value());
    if (in.has_value()) {
      TEST_CHECK(approx_eq(in.value()[0], 1.0));
      TEST_CHECK(approx_eq(in.value()[1], 2.0));
      TEST_CHECK(approx_eq(in.value()[2], 3.0));
    }
    TEST_CHECK(rb.principalAxes.get_value().has_value());
  }

  // PhysicsMassAPI reads the same physics:mass/physics:density (density_) keys.
  PhysicsMassAPI mass;
  TEST_CHECK(GetPhysicsMassAPI(*prim, &mass));
  TEST_CHECK(approx_eq(mass.mass.get_value(), 2.5));
  TEST_CHECK(approx_eq(mass.density_.get_value(), 1000.0));
}

// R2. PhysicsMaterialAPI + PhysicsCollisionAPI + PhysicsMeshCollisionAPI.
// staticFriction/dynamicFriction are near-valued floats — a prime false-share
// candidate for the dedup pass.
void physics_collision_material_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Material "Rubber" (
        prepend apiSchemas = ["PhysicsMaterialAPI"]
    )
    {
        float physics:staticFriction = 0.6
        float physics:dynamicFriction = 0.5
        float physics:restitution = 0.2
        float physics:density = 900
    }

    def Mesh "Collider" (
        prepend apiSchemas = ["PhysicsCollisionAPI", "PhysicsMeshCollisionAPI"]
    )
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        bool physics:collisionEnabled = 1
        uniform token physics:approximation = "convexHull"
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto mr = stage.GetPrimAtPath(Path("/World/Rubber", ""));
  TEST_CHECK(bool(mr));
  if (mr) {
    PhysicsMaterialAPI mat;
    TEST_CHECK(GetPhysicsMaterialAPI(**mr, &mat));
    auto sf = mat.staticFriction.get_value();
    auto df = mat.dynamicFriction.get_value();
    auto rest = mat.restitution.get_value();
    auto den = mat.density.get_value();
    TEST_CHECK(sf.has_value() && approx_eq(sf.value(), 0.6));
    TEST_CHECK(df.has_value() && approx_eq(df.value(), 0.5));
    TEST_CHECK(rest.has_value() && approx_eq(rest.value(), 0.2));
    TEST_CHECK(den.has_value() && approx_eq(den.value(), 900.0));
  }

  auto cr = stage.GetPrimAtPath(Path("/World/Collider", ""));
  TEST_CHECK(bool(cr));
  if (cr) {
    PhysicsCollisionAPI col;
    TEST_CHECK(GetPhysicsCollisionAPI(**cr, &col));
    TEST_CHECK(col.collisionEnabled.get_value() == true);
    PhysicsMeshCollisionAPI mcol;
    TEST_CHECK(GetPhysicsMeshCollisionAPI(**cr, &mcol));
    // approximation is TypedAttributeWithFallback<token> -> get_value() is the
    // token (fallback "none" when unauthored).
    TEST_CHECK(mcol.approximation.get_value().str() == "convexHull");
  }
}

// R3. Multi-apply physics:drive:<dof>:* / physics:limit:<dof>:* value survival
// through USDC (the generic props pass for namespaced keys — highest-risk).
void physics_drive_limit_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "Joint" (
    prepend apiSchemas = ["PhysicsDriveAPI:rotX", "PhysicsLimitAPI:rotX"]
)
{
    rel physics:body0 = </Arm>
    rel physics:body1 = </Forearm>
    token physics:axis = "X"

    token physics:drive:rotX:type = "force"
    float physics:drive:rotX:maxForce = 100
    float physics:drive:rotX:targetPosition = 1.57
    float physics:drive:rotX:stiffness = 50
    float physics:drive:rotX:damping = 10

    float physics:limit:rotX:low = -1.57
    float physics:limit:rotX:high = 1.57
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/Joint", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const auto *joint = (*result)->as<PhysicsRevoluteJoint>();
  TEST_CHECK(joint != nullptr);
  if (!joint) return;

  // Values survive in the generic props map.
  double v = 0.0;
  TEST_CHECK(get_prop_num(joint->props, "physics:drive:rotX:maxForce", &v));
  TEST_CHECK(approx_eq(v, 100.0));
  TEST_CHECK(get_prop_num(joint->props, "physics:drive:rotX:targetPosition", &v));
  TEST_CHECK(approx_eq(v, 1.57));
  TEST_CHECK(get_prop_num(joint->props, "physics:drive:rotX:stiffness", &v));
  TEST_CHECK(approx_eq(v, 50.0));
  TEST_CHECK(get_prop_num(joint->props, "physics:drive:rotX:damping", &v));
  TEST_CHECK(approx_eq(v, 10.0));
  TEST_CHECK(get_prop_num(joint->props, "physics:limit:rotX:low", &v));
  TEST_CHECK(approx_eq(v, -1.57));
  TEST_CHECK(get_prop_num(joint->props, "physics:limit:rotX:high", &v));
  TEST_CHECK(approx_eq(v, 1.57));

  // After Phase 1a (typed-map population) these also populate joint->drives /
  // joint->limits. Assert when present so this test tightens automatically.
  if (!joint->drives.empty()) {
    auto it = joint->drives.find("rotX");
    TEST_CHECK(it != joint->drives.end());
    if (it != joint->drives.end()) {
      TEST_CHECK(it->second.dof == "rotX");
      TEST_CHECK(it->second.maxForce.authored() &&
                 approx_eq(it->second.maxForce.get_value(), 100.0));
      TEST_CHECK(it->second.targetPosition.authored() &&
                 approx_eq(it->second.targetPosition.get_value(), 1.57));
    }
  }
  if (!joint->limits.empty()) {
    auto it = joint->limits.find("rotX");
    TEST_CHECK(it != joint->limits.end());
    if (it != joint->limits.end()) {
      TEST_CHECK(it->second.low.authored() &&
                 approx_eq(it->second.low.get_value(), -1.57));
      TEST_CHECK(it->second.high.authored() &&
                 approx_eq(it->second.high.get_value(), 1.57));
    }
  }
}

// R4. Joint local frames (point3f localPos + quatf localRot) + body rels.
// Compares a direct parse against a USDC round-trip so the assertion is
// convention-agnostic for quats while still catching value corruption.
void physics_joints_localframe_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsRevoluteJoint "Rev"
{
    rel physics:body0 = </A>
    rel physics:body1 = </B>
    point3f physics:localPos0 = (1, 2, 3)
    point3f physics:localPos1 = (-1, -2, -3)
    quatf physics:localRot0 = (0.70710677, 0.70710677, 0, 0)
    quatf physics:localRot1 = (1, 0, 0, 0)
    token physics:axis = "X"
}

def PhysicsPrismaticJoint "Prism"
{
    rel physics:body0 = </B>
    rel physics:body1 = </C>
    point3f physics:localPos0 = (0.5, 0, 0)
    quatf physics:localRot0 = (0, 1, 0, 0)
    token physics:axis = "Z"
}

def PhysicsDistanceJoint "Dist"
{
    rel physics:body0 = </C>
    rel physics:body1 = </D>
    float physics:minDistance = 0.1
    float physics:maxDistance = 2.5
}
)";
  Stage direct, rt;
  std::string warn, err;
  TEST_CHECK(parse_usda(usda, &direct, &warn, &err));
  bool ok = usdc_roundtrip(usda, &rt, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  // Revolute: localPos0 exact, localRot0 == direct parse (convention-agnostic),
  // body rel targets intact.
  {
    auto dr = direct.GetPrimAtPath(Path("/Rev", ""));
    auto rr = rt.GetPrimAtPath(Path("/Rev", ""));
    TEST_CHECK(bool(dr) && bool(rr));
    if (dr && rr) {
      const auto *dj = (*dr)->as<PhysicsRevoluteJoint>();
      const auto *rj = (*rr)->as<PhysicsRevoluteJoint>();
      TEST_CHECK(dj && rj);
      if (dj && rj) {
        TEST_CHECK(rj->localPos0.authored());
        if (rj->localPos0.authored()) {
          value::point3f p = rj->localPos0.get_value();
          TEST_CHECK(approx_eq(p[0], 1.0));
          TEST_CHECK(approx_eq(p[1], 2.0));
          TEST_CHECK(approx_eq(p[2], 3.0));
        }
        TEST_CHECK(dj->localRot0.authored() && rj->localRot0.authored());
        if (dj->localRot0.authored() && rj->localRot0.authored()) {
          value::quatf qd = dj->localRot0.get_value();
          value::quatf qr = rj->localRot0.get_value();
          for (size_t i = 0; i < 4; i++) {
            TEST_CHECK(approx_eq(qr[i], qd[i]));
          }
        }
        TEST_CHECK(rj->body0.authored() && rj->body1.authored());
        auto t0 = rj->body0.get_targetPaths();
        TEST_CHECK(t0.size() == 1 && t0[0].prim_part() == "/A");
      }
    }
  }

  // Prismatic localRot0 == direct parse.
  {
    auto dr = direct.GetPrimAtPath(Path("/Prism", ""));
    auto rr = rt.GetPrimAtPath(Path("/Prism", ""));
    if (dr && rr) {
      const auto *dj = (*dr)->as<PhysicsPrismaticJoint>();
      const auto *rj = (*rr)->as<PhysicsPrismaticJoint>();
      if (dj && rj) {
        TEST_CHECK(dj->localRot0.authored() && rj->localRot0.authored());
        if (dj->localRot0.authored() && rj->localRot0.authored()) {
          value::quatf qd = dj->localRot0.get_value();
          value::quatf qr = rj->localRot0.get_value();
          for (size_t i = 0; i < 4; i++) {
            TEST_CHECK(approx_eq(qr[i], qd[i]));
          }
        }
      }
    }
  }

  // Distance joint min/max.
  {
    auto rr = rt.GetPrimAtPath(Path("/Dist", ""));
    TEST_CHECK(bool(rr));
    if (rr) {
      const auto *dj = (*rr)->as<PhysicsDistanceJoint>();
      TEST_CHECK(dj != nullptr);
      if (dj) {
        auto lo = dj->minDistance.get_value();
        auto hi = dj->maxDistance.get_value();
        TEST_CHECK(lo.has_value() && approx_eq(lo.value(), 0.1));
        TEST_CHECK(hi.has_value() && approx_eq(hi.value(), 2.5));
      }
    }
  }
}

// R5. PhysicsCollisionGroup mergeGroup/invert/filteredGroups rel +
// colliders collection includes survive USDC (rel-target vectors are a
// separate crate section from scalar values).
void physics_collision_group_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsCollisionGroup "GroupA" (
    prepend apiSchemas = ["CollectionAPI:colliders"]
)
{
    token physics:mergeGroup = "armA"
    bool physics:invertFilteredGroups = 1
    rel physics:filteredGroups = [</GroupB>]
    rel collection:colliders:includes = [</Body1>, </Body2>]
}

def PhysicsCollisionGroup "GroupB" {}
def Cube "Body1" {}
def Cube "Body2" {}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/GroupA", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const Prim *prim = *result;
  const auto *grp = prim->as<PhysicsCollisionGroup>();
  TEST_CHECK(grp != nullptr);
  if (!grp) return;

  auto mg = grp->mergeGroup.get_value();
  TEST_CHECK(mg.has_value() && mg.value().str() == "armA");
  TEST_CHECK(grp->invertFilteredGroups.get_value() == true);
  TEST_CHECK(grp->filteredGroups.authored());

  std::vector<Path> includes, excludes;
  TEST_CHECK(GetPhysicsCollidersCollection(*prim, &includes, &excludes));
  TEST_CHECK(includes.size() == 2);
  if (includes.size() == 2) {
    TEST_CHECK(includes[0].full_path_name() == "/Body1");
    TEST_CHECK(includes[1].full_path_name() == "/Body2");
  }
}

// R6. PhysicsScene carrying MjcSceneAPI + NewtonSceneAPI + NewtonKaminoSceneAPI
// simultaneously, round-tripped. Locks the typed re-emit path (sconv-physics.cc)
// AND dedup across co-resident sibling API blocks on one prim.
void physics_scene_full_mjc_newton_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "Sim" (
    prepend apiSchemas = ["MjcSceneAPI", "NewtonSceneAPI", "NewtonKaminoSceneAPI"]
)
{
    vector3f physics:gravityDirection = (0, 0, -1)
    float physics:gravityMagnitude = 9.81

    uniform double mjc:option:timestep = 0.002
    uniform token mjc:option:integrator = "implicit"
    uniform int mjc:option:iterations = 150
    uniform bool mjc:flag:contact = 1

    uniform int newton:maxSolverIterations = 48
    uniform int newton:timeStepsPerSecond = 360
    bool newton:gravityEnabled = true
    uniform float newton:kamino:constraints:alpha = 0.5
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto result = stage.GetPrimAtPath(Path("/Sim", ""));
  TEST_CHECK(bool(result));
  if (!result) return;
  const Prim *prim = *result;
  const auto *scene = prim->as<PhysicsScene>();
  TEST_CHECK(scene != nullptr);
  if (!scene) return;

  TEST_CHECK(scene->mjcScene.has_value());
  TEST_CHECK(scene->newtonScene.has_value());
  TEST_CHECK(scene->newtonKaminoScene.has_value());
  if (scene->mjcScene.has_value()) {
    const auto &m = scene->mjcScene.value();
    TEST_CHECK(approx_eq(m.timestep.get_value(), 0.002));
    TEST_CHECK(m.integrator.get_value().str() == "implicit");
    TEST_CHECK(m.iterations.get_value() == 150);
    TEST_CHECK(m.flag_contact.get_value() == true);
  }
  if (scene->newtonScene.has_value()) {
    const auto &n = scene->newtonScene.value();
    TEST_CHECK(n.maxSolverIterations.get_value() == 48);
    TEST_CHECK(n.timeStepsPerSecond.get_value() == 360);
    TEST_CHECK(n.gravityEnabled.get_value() == true);
  }
  if (scene->newtonKaminoScene.has_value()) {
    TEST_CHECK(approx_eq(scene->newtonKaminoScene.value().constraintsAlpha.get_value(), 0.5));
  }
  // gravity (typed PhysicsScene attrs) also survive.
  auto g = scene->gravityDirection.get_value();
  TEST_CHECK(g.has_value());
  if (g.has_value()) TEST_CHECK(approx_eq(g.value()[2], -1.0));
  TEST_CHECK(approx_eq(scene->gravityMagnitude.get_value().value_or(0.0f), 9.81));
}

// R7. Tydra physics-to-JSON export works on a crate-reconstructed stage
// (not just a freshly-USDA-parsed one).
void physics_to_json_after_usdc_test(void) {
  const char *usda = R"(#usda 1.0

def PhysicsScene "Scene"
{
    vector3f physics:gravityDirection = (0, 0, -1)
    float physics:gravityMagnitude = 9.81
    uniform double mjc:option:timestep = 0.002
}

def Scope "Joints"
{
    def PhysicsRevoluteJoint "Hinge"
    {
        rel physics:body0 = </Body0>
        rel physics:body1 = </Body1>
        token physics:axis = "Z"
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  std::string json, json_err;
  tydra::PhysicsJsonExportOptions opts;
  opts.include_mjc = true;
  bool json_ok = tydra::ConvertPhysicsToJson(stage, &json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  TEST_CHECK(!json.empty());
  if (!json.empty()) {
    TEST_CHECK(json.find("\"physicsScene\"") != std::string::npos);
    TEST_CHECK(json.find("\"gravityMagnitude\"") != std::string::npos);
    TEST_CHECK(json.find("\"joints\"") != std::string::npos);
    TEST_CHECK(json.find("\"timestep\"") != std::string::npos);
  }
}

// R8. PhysX scene-level + rigidbody-level generic-prop preservation through
// USDC (documents the "physx survives as generic props" contract — gap 1f).
void physx_scene_rigidbody_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def PhysicsScene "Scene"
    {
        vector3f physics:gravityDirection = (0, 0, -1)
        float physics:gravityMagnitude = 9.81
        custom uint physxScene:timeStepsPerSecond = 120
        custom float physxScene:bounceThreshold = 0.2
        custom token physxScene:broadphaseType = "MBP"
    }

    def Cube "Body" (
        prepend apiSchemas = ["PhysicsRigidBodyAPI"]
    )
    {
        float physics:mass = 1.0
        custom float physxRigidBody:maxLinearVelocity = 50.0
        custom float physxRigidBody:sleepThreshold = 0.01
        custom bool physxRigidBody:disableGravity = 0
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto sr = stage.GetPrimAtPath(Path("/World/Scene", ""));
  TEST_CHECK(bool(sr));
  if (sr) {
    const auto *scene = (*sr)->as<PhysicsScene>();
    TEST_CHECK(scene != nullptr);
    if (scene) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(scene->props, "physxScene:timeStepsPerSecond", &v));
      TEST_CHECK(approx_eq(v, 120.0));
      TEST_CHECK(get_prop_num(scene->props, "physxScene:bounceThreshold", &v));
      TEST_CHECK(approx_eq(v, 0.2));
      TEST_CHECK(scene->props.count("physxScene:broadphaseType") > 0);
    }
  }

  auto br = stage.GetPrimAtPath(Path("/World/Body", ""));
  TEST_CHECK(bool(br));
  if (br) {
    const auto *cube = (*br)->as<GeomCube>();
    TEST_CHECK(cube != nullptr);
    if (cube) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(cube->props, "physxRigidBody:maxLinearVelocity", &v));
      TEST_CHECK(approx_eq(v, 50.0));
      TEST_CHECK(get_prop_num(cube->props, "physxRigidBody:sleepThreshold", &v));
      TEST_CHECK(approx_eq(v, 0.01));
      TEST_CHECK(cube->props.count("physxRigidBody:disableGravity") > 0);
    }
  }
}

// ===========================================================================
// Phase 1d: MJCF <tendon>/<equality>/<contact> -> USD conversion.
// JSON->USD half (the shared converter). The XML->JSON half is exercised by
// the urdf-to-usd CLI ctest and the JS parser tests.
// ===========================================================================

// MJCF <tendon><fixed> -> MjcTendon prim under /World/Tendons.
void urdf_json_mjcf_tendon_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "TendonBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" }, { "name": "l" }, { "name": "r" } ],
  "joints": [
    { "name": "jl", "type": "revolute", "parent": "base", "child": "l",
      "axis": [0,1,0], "axisToken": "Y" },
    { "name": "jr", "type": "revolute", "parent": "base", "child": "r",
      "axis": [0,1,0], "axisToken": "Y" }
  ],
  "tendons": [
    { "name": "couple", "type": "fixed", "stiffness": 120, "damping": 3,
      "range": [-1, 1],
      "joints": [ {"joint": "jl", "coef": 1.0}, {"joint": "jr", "coef": -1.0} ] }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto r = stage.GetPrimAtPath(Path("/World/Tendons/couple", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const Prim *p = *r;
  TEST_CHECK(p->is<MjcTendon>());
  const auto *t = p->as<MjcTendon>();
  TEST_CHECK(t != nullptr);
  if (!t) return;
  TEST_CHECK(t->type.get_value().str() == "fixed");
  TEST_CHECK(approx_eq(t->stiffness.get_value(), 120.0));
  TEST_CHECK(approx_eq(t->damping.get_value(), 3.0));
  TEST_CHECK(approx_eq(t->range_min.get_value(), -1.0));
  TEST_CHECK(approx_eq(t->range_max.get_value(), 1.0));
  TEST_CHECK(t->path.authored());
  auto targets = t->path.get_targetPaths();
  TEST_CHECK(targets.size() == 2);
  if (targets.size() == 2) {
    TEST_CHECK(targets[0].prim_part() == "/World/Joints/jl");
    TEST_CHECK(targets[1].prim_part() == "/World/Joints/jr");
  }
  auto coef = t->path_coef.get_value();
  TEST_CHECK(coef.has_value());
  if (coef.has_value()) {
    TEST_CHECK(coef.value().size() == 2);
    if (coef.value().size() == 2) {
      TEST_CHECK(approx_eq(coef.value()[0], 1.0));
      TEST_CHECK(approx_eq(coef.value()[1], -1.0));
    }
  }
}

void urdf_json_mjcf_tendon_full_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "TendonFullBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "sites": [
    { "name": "p0", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
    { "name": "p1", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0.1,0,0,1] }
  ],
  "tendons": [
    { "name": "full", "type": "spatial",
      "route": ["/World/Sites/p0", "/World/Sites/p1"],
      "sideSites": ["/World/Sites/p1"],
      "routeIndices": [0, 1],
      "sideSiteIndices": [1],
      "routeSegments": [0, 1],
      "routeDivisors": [1, 2],
      "routeCoef": [1, -0.5],
      "group": 2,
      "limited": "true",
      "actuatorfrclimited": "false",
      "range_min": 0.1,
      "range_max": 0.9,
      "actuatorfrcrange_min": -3,
      "actuatorfrcrange_max": 4,
      "solreflimit": [0.03, 1.1],
      "solimplimit": [0.8, 0.9, 0.01, 0.5, 2],
      "solreffriction": [0.04, 1.2],
      "solimpfriction": [0.7, 0.8, 0.02, 0.4, 1.5],
      "margin": 0.02,
      "frictionloss": 0.3,
      "springlength": [0.2, 0.8],
      "stiffness": 120,
      "damping": 3,
      "armature": 0.02,
      "width": 0.006,
      "rgba": [0.2, 0.3, 0.4, 0.5] }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto r = stage.GetPrimAtPath(Path("/World/Tendons/full", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const auto *t = (*r)->as<MjcTendon>();
  TEST_CHECK(t != nullptr);
  if (!t) return;

  TEST_CHECK(t->type.get_value().str() == "spatial");
  TEST_CHECK(t->path.authored());
  TEST_CHECK(t->path.get_targetPaths().size() == 2);
  TEST_CHECK(t->sideSites.authored());
  TEST_CHECK(t->sideSites.get_targetPaths().size() == 1);
  auto indices = t->path_indices.get_value();
  TEST_CHECK(indices.has_value() && indices.value().size() == 2);
  auto side_indices = t->sideSites_indices.get_value();
  TEST_CHECK(side_indices.has_value() && side_indices.value().size() == 1);
  auto segments = t->path_segments.get_value();
  TEST_CHECK(segments.has_value() && segments.value().size() == 2);
  auto divisors = t->path_divisors.get_value();
  TEST_CHECK(divisors.has_value() && divisors.value().size() == 2);
  auto coef = t->path_coef.get_value();
  TEST_CHECK(coef.has_value() && coef.value().size() == 2);
  if (coef.has_value() && coef.value().size() == 2) {
    TEST_CHECK(approx_eq(coef.value()[1], -0.5));
  }
  TEST_CHECK(t->group.authored() && t->group.get_value() == 2);
  TEST_CHECK(t->limited.authored() && t->limited.get_value().str() == "true");
  TEST_CHECK(t->actuatorfrclimited.authored() &&
             t->actuatorfrclimited.get_value().str() == "false");
  TEST_CHECK(t->range_min.authored() && approx_eq(t->range_min.get_value(), 0.1));
  TEST_CHECK(t->range_max.authored() && approx_eq(t->range_max.get_value(), 0.9));
  TEST_CHECK(t->actuatorfrcrange_min.authored() &&
             approx_eq(t->actuatorfrcrange_min.get_value(), -3.0));
  TEST_CHECK(t->actuatorfrcrange_max.authored() &&
             approx_eq(t->actuatorfrcrange_max.get_value(), 4.0));
  TEST_CHECK(t->solreflimit.get_value().has_value());
  TEST_CHECK(t->solimplimit.get_value().has_value());
  TEST_CHECK(t->solreffriction.get_value().has_value());
  TEST_CHECK(t->solimpfriction.get_value().has_value());
  TEST_CHECK(t->springlength.get_value().has_value());
  TEST_CHECK(t->width.authored() && approx_eq(t->width.get_value(), 0.006));
  TEST_CHECK(t->rgba.authored());
}

void urdf_json_mjcf_tendon_alias_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "TendonAliasBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "sites": [
    { "name": "p0", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
    { "name": "p1", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0.1,0,0,1] }
  ],
  "tendons": [
    { "name": "object_route", "type": "spatial",
      "route": [ {"site": "p0"}, {"target": "/World/Sites/p1"} ],
      "sideSites": [ {"site": "p0"} ],
      "routeCoef": [1, -1] },
    { "name": "partial_route", "type": "spatial",
      "route": ["/World/Sites/p0"] }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto r = stage.GetPrimAtPath(Path("/World/Tendons/object_route", ""));
  TEST_CHECK(bool(r));
  if (r) {
    const auto *t = (*r)->as<MjcTendon>();
    TEST_CHECK(t != nullptr);
    if (t) {
      TEST_CHECK(t->path.authored());
      auto targets = t->path.get_targetPaths();
      TEST_CHECK(targets.size() == 2);
      if (targets.size() == 2) {
        TEST_CHECK(targets[0].prim_part() == "/World/Sites/p0");
        TEST_CHECK(targets[1].prim_part() == "/World/Sites/p1");
      }
      TEST_CHECK(t->sideSites.authored());
      TEST_CHECK(t->sideSites.get_targetPaths().size() == 1);
      auto coef = t->path_coef.get_value();
      TEST_CHECK(coef.has_value() && coef.value().size() == 2);
    }
  }

  auto partial = stage.GetPrimAtPath(Path("/World/Tendons/partial_route", ""));
  TEST_CHECK(bool(partial));
  if (partial) {
    const auto *t = (*partial)->as<MjcTendon>();
    TEST_CHECK(t != nullptr);
    if (t) {
      TEST_CHECK(t->path.authored());
      TEST_CHECK(t->path.get_targetPaths().size() == 1);
    }
  }
}

void urdf_json_mjc_actuator_full_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "ActuatorFullBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "sites": [
    { "name": "p0", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
    { "name": "p1", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0.1,0,0,1] }
  ],
  "mjcActuators": [
    { "name": "full",
      "target": "/World/Links/base",
      "group": 3,
      "ctrlLimited": "true",
      "forceLimited": "false",
      "actLimited": "auto",
      "ctrlRange_min": -1,
      "ctrlRange_max": 1,
      "forceRange_min": -200,
      "forceRange_max": 200,
      "actRange": [0, 2],
      "lengthRange_min": 0.18,
      "lengthRange_max": 0.29,
      "gear": [1, 0, 0, 0, 0, 0],
      "crankLength": 0.1,
      "jointInParent": true,
      "refSite": "p0",
      "sliderSite": "p1",
      "actDim": 2,
      "dynType": "filter",
      "gainType": "affine",
      "biasType": "affine",
      "dynPrm": [0.01, 0.02],
      "gainPrm": [350, 0, 0],
      "biasPrm": [0, -350, -10],
      "actEarly": true,
      "inheritRange": 0.5,
      "plugin": "pid0",
      "instance": "inst0" }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto r = stage.GetPrimAtPath(Path("/World/MjcActuators/full", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const auto *a = (*r)->as<MjcActuator>();
  TEST_CHECK(a != nullptr);
  if (!a) return;

  TEST_CHECK(a->target.authored());
  auto targets = a->target.get_targetPaths();
  TEST_CHECK(targets.size() == 1);
  if (targets.size() == 1) {
    TEST_CHECK(targets[0].prim_part() == "/World/Links/base");
  }
  TEST_CHECK(a->group.authored() && a->group.get_value() == 3);
  TEST_CHECK(a->ctrlLimited.get_value().str() == "true");
  TEST_CHECK(a->forceLimited.get_value().str() == "false");
  TEST_CHECK(a->actLimited.get_value().str() == "auto");
  TEST_CHECK(approx_eq(a->ctrlRange_min.get_value(), -1.0));
  TEST_CHECK(approx_eq(a->ctrlRange_max.get_value(), 1.0));
  TEST_CHECK(approx_eq(a->forceRange_min.get_value(), -200.0));
  TEST_CHECK(approx_eq(a->forceRange_max.get_value(), 200.0));
  TEST_CHECK(approx_eq(a->actRange_min.get_value(), 0.0));
  TEST_CHECK(approx_eq(a->actRange_max.get_value(), 2.0));
  TEST_CHECK(approx_eq(a->lengthRange_min.get_value(), 0.18));
  TEST_CHECK(approx_eq(a->lengthRange_max.get_value(), 0.29));
  TEST_CHECK(approx_eq(a->crankLength.get_value(), 0.1));
  TEST_CHECK(a->jointInParent.get_value() == true);
  TEST_CHECK(a->refSite.authored());
  if (a->refSite.authored()) {
    auto rels = a->refSite.get_targetPaths();
    TEST_CHECK(rels.size() == 1);
    if (rels.size() == 1) TEST_CHECK(rels[0].prim_part() == "/World/Sites/p0");
  }
  TEST_CHECK(a->sliderSite.authored());
  if (a->sliderSite.authored()) {
    auto rels = a->sliderSite.get_targetPaths();
    TEST_CHECK(rels.size() == 1);
    if (rels.size() == 1) TEST_CHECK(rels[0].prim_part() == "/World/Sites/p1");
  }
  TEST_CHECK(a->actDim.get_value() == 2);
  TEST_CHECK(a->dynType.get_value().str() == "filter");
  TEST_CHECK(a->gainType.get_value().str() == "affine");
  TEST_CHECK(a->biasType.get_value().str() == "affine");
  auto dyn = a->dynPrm.get_value();
  TEST_CHECK(dyn.has_value() && dyn.value().size() == 2);
  auto gain = a->gainPrm.get_value();
  TEST_CHECK(gain.has_value() && gain.value().size() == 3);
  auto bias = a->biasPrm.get_value();
  TEST_CHECK(bias.has_value() && bias.value().size() == 3);
  TEST_CHECK(a->actEarly.get_value() == true);
  TEST_CHECK(approx_eq(a->inheritRange.get_value(), 0.5));
  TEST_CHECK(a->plugin.get_value().str() == "pid0");
  TEST_CHECK(a->instance.get_value().str() == "inst0");
}

// MJCF <equality> connect/weld/joint -> Xform host prims with MjcEquality*API.
void urdf_json_mjcf_equality_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "EqBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" }, { "name": "l" }, { "name": "r" } ],
  "joints": [
    { "name": "jl", "type": "revolute", "parent": "base", "child": "l", "axisToken": "Y" },
    { "name": "jr", "type": "revolute", "parent": "base", "child": "r", "axisToken": "Y" }
  ],
  "equalities": [
    { "name": "mirror", "type": "joint", "joint1": "jl", "joint2": "jr",
      "polycoef": [0, -1, 0, 0, 0], "solref": [0.02, 1] },
    { "name": "lockit", "type": "weld", "body1": "l", "body2": "r",
      "torquescale": 0.5, "anchor": [0, 0, 0] }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto mr = stage.GetPrimAtPath(Path("/World/Equalities/mirror", ""));
  TEST_CHECK(bool(mr));
  if (mr) {
    const Prim *p = *mr;
    TEST_CHECK(has_api(p, APISchemas::APIName::MjcEqualityJointAPI));
    const auto *x = p->as<Xform>();
    TEST_CHECK(x != nullptr);
    if (x) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(x->props, "mjc:coef1", &v));
      TEST_CHECK(approx_eq(v, -1.0));
      // target rel -> the two joints
      auto it = x->props.find("mjc:target");
      TEST_CHECK(it != x->props.end());
    }
  }

  auto wr = stage.GetPrimAtPath(Path("/World/Equalities/lockit", ""));
  TEST_CHECK(bool(wr));
  if (wr) {
    const Prim *p = *wr;
    TEST_CHECK(has_api(p, APISchemas::APIName::MjcEqualityWeldAPI));
    const auto *x = p->as<Xform>();
    if (x) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(x->props, "mjc:torqueScale", &v));
      TEST_CHECK(approx_eq(v, 0.5));
    }
  }
}

// ===========================================================================
// Phase 1c: full (non-diagonal) inertia tensor -> diagonalInertia (principal
// moments) + principalAxes (eigenvector quaternion). Correctness gate: rebuild
// R*diag*R^T from the authored attrs and assert it matches the input tensor.
// ===========================================================================
void urdf_json_fullinertia_diagonalize_test(void) {
  // Symmetric M = [[4,1,0],[1,4,0],[0,0,6]] -> eigenvalues {3,5,6}.
  // fullInertia = [Ixx, Iyy, Izz, Ixy, Ixz, Iyz] = [4,4,6,1,0,0].
  const char *robot_json = R"JSON({
  "name": "InertiaBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [
    { "name": "base",
      "inertial": { "mass": 2.0, "centerOfMass": [0,0,0],
                    "fullInertia": [4, 4, 6, 1, 0, 0],
                    "diagonalInertia": [4, 4, 6] } }
  ],
  "joints": []
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto r = stage.GetPrimAtPath(Path("/World/Links/base", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const auto *xf = (*r)->as<Xform>();
  TEST_CHECK(xf != nullptr);
  if (!xf) return;

  // Read diagonalInertia (float3) + principalAxes (quatf).
  auto di_it = xf->props.find("physics:diagonalInertia");
  auto pa_it = xf->props.find("physics:principalAxes");
  TEST_CHECK(di_it != xf->props.end());
  TEST_CHECK(pa_it != xf->props.end());
  if (di_it == xf->props.end() || pa_it == xf->props.end()) return;

  auto di = di_it->second.get_attribute().get_value<value::float3>();
  auto pa = pa_it->second.get_attribute().get_value<value::quatf>();
  TEST_CHECK(di.has_value() && pa.has_value());
  if (!di.has_value() || !pa.has_value()) return;

  // Eigenvalues must be a permutation of {3,5,6}.
  double evals[3] = {di.value()[0], di.value()[1], di.value()[2]};
  std::sort(evals, evals + 3);
  TEST_CHECK(approx_eq(evals[0], 3.0));
  TEST_CHECK(approx_eq(evals[1], 5.0));
  TEST_CHECK(approx_eq(evals[2], 6.0));

  // Rebuild R from the quaternion, then M_rec = R * diag(eval) * R^T and
  // compare against the input symmetric tensor.
  const double w = pa.value().real;
  const double x = pa.value().imag[0];
  const double y = pa.value().imag[1];
  const double z = pa.value().imag[2];
  const double R[3][3] = {
      {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
      {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
      {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
  const double d[3] = {di.value()[0], di.value()[1], di.value()[2]};
  double Mrec[3][3];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      double s = 0.0;
      for (int k = 0; k < 3; k++) s += R[i][k] * d[k] * R[j][k];
      Mrec[i][j] = s;
    }
  const double Min[3][3] = {{4, 1, 0}, {1, 4, 0}, {0, 0, 6}};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      TEST_CHECK(approx_eq(Mrec[i][j], Min[i][j]));
    }
}

// MJCF spatial (muscle) tendon + sites + muscle actuator (ms_human_700 style)
// -> MjcSite markers, spatial MjcTendon routed through the sites, and an
// MjcActuator targeting the tendon. JSON->USD half.
void urdf_json_mjcf_muscle_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "MuscleBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "pelvis" }, { "name": "femur" } ],
  "joints": [
    { "name": "hip", "type": "revolute", "parent": "pelvis", "child": "femur", "axisToken": "Y" }
  ],
  "sites": [
    { "name": "muscle_p1", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0.1,1], "group": 3, "size": 0.004 },
    { "name": "muscle_p2", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0.05,0,-0.2,1], "group": 3 }
  ],
  "tendons": [
    { "name": "glute_tendon", "type": "spatial", "width": 0.005,
      "rgba": [0.95, 0.3, 0.3, 1], "stiffness": 10,
      "path": [ {"site": "muscle_p1"}, {"site": "muscle_p2"} ] }
  ],
  "mjcActuators": [
    { "name": "glute", "actuatorType": "general", "targetTendon": "glute_tendon",
      "lengthRange": [0.18, 0.29], "gainPrm": [0.75, 1.05, 916.8],
      "biasPrm": [0.75, 1.05, 916.8], "ctrlRange": [0, 1] }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  // Sites -> GeomSphere markers with MjcSiteAPI.
  auto s1 = stage.GetPrimAtPath(Path("/World/Sites/muscle_p1", ""));
  TEST_CHECK(bool(s1));
  if (s1) {
    TEST_CHECK((*s1)->is<GeomSphere>());
    TEST_CHECK(has_api(*s1, APISchemas::APIName::MjcSiteAPI));
  }

  // Spatial tendon routed through the two sites.
  auto tr = stage.GetPrimAtPath(Path("/World/Tendons/glute_tendon", ""));
  TEST_CHECK(bool(tr));
  if (tr) {
    const auto *t = (*tr)->as<MjcTendon>();
    TEST_CHECK(t != nullptr);
    if (t) {
      TEST_CHECK(t->type.get_value().str() == "spatial");
      TEST_CHECK(t->path.authored());
      auto targets = t->path.get_targetPaths();
      TEST_CHECK(targets.size() == 2);
      if (targets.size() == 2) {
        TEST_CHECK(targets[0].prim_part() == "/World/Sites/muscle_p1");
        TEST_CHECK(targets[1].prim_part() == "/World/Sites/muscle_p2");
      }
    }
  }

  // Muscle actuator targeting the tendon, with gain/bias/lengthrange preserved.
  auto ar = stage.GetPrimAtPath(Path("/World/MjcActuators/glute", ""));
  TEST_CHECK(bool(ar));
  if (ar) {
    const auto *a = (*ar)->as<MjcActuator>();
    TEST_CHECK(a != nullptr);
    if (a) {
      TEST_CHECK(a->target.authored());
      auto tg = a->target.get_targetPaths();
      TEST_CHECK(tg.size() == 1);
      if (tg.size() == 1) {
        TEST_CHECK(tg[0].prim_part() == "/World/Tendons/glute_tendon");
      }
      TEST_CHECK(approx_eq(a->lengthRange_min.get_value(), 0.18));
      TEST_CHECK(approx_eq(a->lengthRange_max.get_value(), 0.29));
      auto gp = a->gainPrm.get_value();
      TEST_CHECK(gp.has_value());
      if (gp.has_value() && gp.value().size() >= 3) {
        TEST_CHECK(approx_eq(gp.value()[2], 916.8));
      }
    }
  }
}

// MJCF mocap body -> mjc:mocap on the link; <custom> -> /World/MjcCustom.
void urdf_json_mjc_mocap_custom_test(void) {
  const char *robot_json = R"JSON({
  "name": "MocapBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "target", "mocap": true }, { "name": "base" } ],
  "joints": [],
  "custom": {
    "numeric": [ { "name": "max_contact_points", "data": [8] } ],
    "text": [ { "name": "description", "data": "demo" } ]
  }
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto mr = stage.GetPrimAtPath(Path("/World/Links/target", ""));
  TEST_CHECK(bool(mr));
  if (mr) {
    const auto *x = (*mr)->as<Xform>();
    if (x) {
      auto it = x->props.find("mjc:mocap");
      TEST_CHECK(it != x->props.end());
    }
  }
  auto cr = stage.GetPrimAtPath(Path("/World/MjcCustom", ""));
  TEST_CHECK(bool(cr));
  if (cr) {
    const auto *x = (*cr)->as<Xform>();
    if (x) {
      TEST_CHECK(x->props.count("mjc:custom:max_contact_points") > 0);
      TEST_CHECK(x->props.count("mjc:customtext:description") > 0);
    }
  }
}

// Regression canary: an MjcActuator's transmission/gain/bias/range attributes
// must survive a USDC round-trip. The crate writer's ExtractMjcActuatorProperties
// formerly emitted only group/target/dyn/gain/biasType, silently dropping
// ctrlRange/forceRange/gainPrm/biasPrm/gear/lengthRange on write.
void physics_mjc_actuator_usdc_roundtrip_test(void) {
  const char *usda = R"(#usda 1.0

def "World"
{
    def Scope "MjcActuators"
    {
        def MjcActuator "act0"
        {
            uniform double mjc:ctrlRange:min = 0
            uniform double mjc:ctrlRange:max = 0.04
            uniform double mjc:forceRange:min = -200
            uniform double mjc:forceRange:max = 200
            uniform double mjc:lengthRange:min = 0.18
            uniform double mjc:lengthRange:max = 0.29
            uniform double[] mjc:gear = [1, 0, 0, 0, 0, 0]
            uniform double[] mjc:gainPrm = [350, 0, 0]
            uniform double[] mjc:biasPrm = [0, -350, -10]
        }
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("usdc roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);

  auto ar = stage.GetPrimAtPath(Path("/World/MjcActuators/act0", ""));
  TEST_CHECK(bool(ar));
  if (!ar) return;
  const auto *a = (*ar)->as<MjcActuator>();
  TEST_CHECK(a != nullptr);
  if (!a) return;
  TEST_CHECK(approx_eq(a->ctrlRange_min.get_value(), 0.0));
  TEST_CHECK(approx_eq(a->ctrlRange_max.get_value(), 0.04));
  TEST_CHECK(approx_eq(a->forceRange_min.get_value(), -200.0));
  TEST_CHECK(approx_eq(a->forceRange_max.get_value(), 200.0));
  TEST_CHECK(approx_eq(a->lengthRange_min.get_value(), 0.18));
  TEST_CHECK(approx_eq(a->lengthRange_max.get_value(), 0.29));
  auto gear = a->gear.get_value();
  TEST_CHECK(gear.has_value() && gear.value().size() == 6);
  auto gp = a->gainPrm.get_value();
  TEST_CHECK(gp.has_value());
  if (gp.has_value() && gp.value().size() >= 3) {
    TEST_CHECK(approx_eq(gp.value()[0], 350.0));
  }
  auto bp = a->biasPrm.get_value();
  TEST_CHECK(bp.has_value());
  if (bp.has_value() && bp.value().size() >= 3) {
    TEST_CHECK(approx_eq(bp.value()[1], -350.0));
  }
}

// MJCF <extension><plugin><instance><config> + <actuator><plugin> ->
// /World/MjcPlugins config scope + MjcActuator with mjc:plugin/mjc:instance.
// Also asserts the plugin/instance tokens survive a USDC round-trip.
void urdf_json_mjc_plugin_test(void) {
  const char *robot_json = R"JSON({
  "name": "PidBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" }, { "name": "link0" } ],
  "joints": [
    { "name": "j0", "type": "revolute", "parent": "base", "child": "link0", "axisToken": "Z" }
  ],
  "mjcActuators": [
    { "name": "act0", "actuatorType": "plugin", "targetJoint": "j0",
      "plugin": "mujoco.pid", "instance": "pid0", "ctrlRange": [-1, 1] }
  ],
  "plugins": [
    { "instance": "pid0", "plugin": "mujoco.pid",
      "config": { "kp": "2.8", "ki": "4.0", "kd": "0.03" } }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  // MjcActuator carries plugin id + instance reference.
  auto ar = stage.GetPrimAtPath(Path("/World/MjcActuators/act0", ""));
  TEST_CHECK(bool(ar));
  if (ar) {
    const auto *a = (*ar)->as<MjcActuator>();
    TEST_CHECK(a != nullptr);
    if (a) {
      TEST_CHECK(a->plugin.get_value().str() == "mujoco.pid");
      TEST_CHECK(a->instance.get_value().str() == "pid0");
    }
  }

  // Plugin instance config preserved on /World/MjcPlugins.
  auto pr = stage.GetPrimAtPath(Path("/World/MjcPlugins", ""));
  TEST_CHECK(bool(pr));
  if (pr) {
    const auto *x = (*pr)->as<Xform>();
    TEST_CHECK(x != nullptr);
    if (x) {
      TEST_CHECK(x->props.count("mjc:plugin:pid0:plugin") > 0);
      TEST_CHECK(x->props.count("mjc:plugin:pid0:config:kp") > 0);
      TEST_CHECK(x->props.count("mjc:plugin:pid0:config:kd") > 0);
    }
  }

  // The plugin/instance tokens must survive a USDC round-trip.
  std::vector<uint8_t> bytes;
  if (usdc::SaveAsUSDCToMemory(stage, &bytes, &warn, &err)) {
    Stage rt;
    if (LoadUSDCFromMemory(bytes.data(), bytes.size(), "plugin.usdc", &rt,
                           &warn, &err)) {
      auto ar2 = rt.GetPrimAtPath(Path("/World/MjcActuators/act0", ""));
      TEST_CHECK(bool(ar2));
      if (ar2) {
        const auto *a2 = (*ar2)->as<MjcActuator>();
        TEST_CHECK(a2 != nullptr);
        if (a2) {
          TEST_CHECK(a2->plugin.get_value().str() == "mujoco.pid");
          TEST_CHECK(a2->instance.get_value().str() == "pid0");
        }
      }
    } else {
      TEST_MSG("usdc reload failed: %s", err.c_str());
      TEST_CHECK(false);
    }
  } else {
    TEST_MSG("usdc save failed: %s", err.c_str());
    TEST_CHECK(false);
  }
}

// A link flagged "static" (the synthetic "world" link holding worldbody-level
// floor/ground/hfield geoms) must convert to a static collider:
// physics:rigidBodyEnabled=false and NO articulation-root API. A normal
// parentless link stays a dynamic articulation root.
void physics_static_world_link_test(void) {
  const char *robot_json = R"JSON({
  "name": "FloorBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [
    { "name": "world", "static": true, "inertial": { "mass": 0 },
      "visuals": [ { "name": "floor",
        "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
        "geometry": { "positions": [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0],
                      "normals": [0,0,1, 0,0,1, 0,0,1, 0,0,1],
                      "indices": [0,1,2, 0,2,3] } } ],
      "collisions": [] },
    { "name": "base" }
  ],
  "joints": []
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  // Static world link: not an articulation root, rigid body disabled.
  auto wr = stage.GetPrimAtPath(Path("/World/Links/world", ""));
  TEST_CHECK(bool(wr));
  if (wr) {
    const Prim *w = *wr;
    TEST_CHECK(!has_api(w, APISchemas::APIName::PhysicsArticulationRootAPI));
    const auto *wx = w->as<Xform>();
    TEST_CHECK(wx != nullptr);
    if (wx) {
      auto it = wx->props.find("physics:rigidBodyEnabled");
      TEST_CHECK(it != wx->props.end());
      if (it != wx->props.end() && it->second.is_attribute()) {
        auto v = it->second.get_attribute().get_value<bool>();
        TEST_CHECK(v.has_value() && v.value() == false);
      }
      // the worldbody-level floor mesh landed under the static link
      TEST_CHECK(!w->children().empty());
    }
  }

  // A normal parentless link is still a dynamic articulation root.
  auto br = stage.GetPrimAtPath(Path("/World/Links/base", ""));
  TEST_CHECK(bool(br));
  if (br) {
    TEST_CHECK(has_api(*br, APISchemas::APIName::PhysicsArticulationRootAPI));
  }
}

// MJCF <freejoint> -> a floating-base link is an articulation root carrying
// mjc:freeJoint=true, distinguishing it from a fixed (anchored) base which is a
// parentless link WITHOUT the flag.
void physics_freejoint_floating_base_test(void) {
  const char *robot_json = R"JSON({
  "name": "FloatBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [
    { "name": "floater", "floating": true },
    { "name": "anchored" }
  ],
  "joints": []
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto fr = stage.GetPrimAtPath(Path("/World/Links/floater", ""));
  TEST_CHECK(bool(fr));
  if (fr) {
    const Prim *f = *fr;
    TEST_CHECK(has_api(f, APISchemas::APIName::PhysicsArticulationRootAPI));
    const auto *fx = f->as<Xform>();
    TEST_CHECK(fx != nullptr && fx->props.count("mjc:freeJoint") > 0);
  }
  // A fixed (anchored) base: parentless articulation root WITHOUT mjc:freeJoint.
  auto ar = stage.GetPrimAtPath(Path("/World/Links/anchored", ""));
  TEST_CHECK(bool(ar));
  if (ar) {
    const auto *ax = (*ar)->as<Xform>();
    TEST_CHECK(ax != nullptr && ax->props.count("mjc:freeJoint") == 0);
  }
}

// MJCF <contact><pair> -> /World/Contacts host prim with mjc:* props.
void urdf_json_mjc_contact_pair_test(void) {
  const char *robot_json = R"JSON({
  "name": "PairBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "joints": [],
  "contactPairs": [
    { "name": "foot_floor", "geom1": "foot", "geom2": "floor",
      "condim": 6, "friction": [1, 0.005, 0.0001], "solref": [0.02, 1], "margin": 0.001 }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;
  auto r = stage.GetPrimAtPath(Path("/World/Contacts/foot_floor", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const auto *x = (*r)->as<Xform>();
  TEST_CHECK(x != nullptr);
  if (x) {
    double v = 0.0;
    TEST_CHECK(get_prop_num(x->props, "mjc:condim", &v));
    TEST_CHECK(approx_eq(v, 6.0));
    TEST_CHECK(get_prop_num(x->props, "mjc:margin", &v));
    TEST_CHECK(approx_eq(v, 0.001));
    TEST_CHECK(x->props.count("mjc:geom1") > 0);
    TEST_CHECK(x->props.count("mjc:friction") > 0);
  }
}

// MJCF <adhesion>/<cylinder>/... actuators -> MjcActuator (body/joint target).
void urdf_json_mjc_actuator_types_test(void) {
  const char *robot_json = R"JSON({
  "name": "ActBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" }, { "name": "foot" } ],
  "joints": [ { "name": "ankle", "type": "revolute", "parent": "base", "child": "foot", "axisToken": "Y" } ],
  "mjcActuators": [
    { "name": "grip", "actuatorType": "adhesion", "targetBody": "foot", "gainPrm": [5.0] },
    { "name": "piston", "actuatorType": "cylinder", "targetJoint": "ankle", "ctrlRange": [0, 1] }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto gr = stage.GetPrimAtPath(Path("/World/MjcActuators/grip", ""));
  TEST_CHECK(bool(gr));
  if (gr) {
    const auto *a = (*gr)->as<MjcActuator>();
    TEST_CHECK(a != nullptr);
    if (a) {
      TEST_CHECK(a->target.authored());
      auto tg = a->target.get_targetPaths();
      TEST_CHECK(tg.size() == 1 && tg[0].prim_part() == "/World/Links/foot");
    }
  }
  auto pr = stage.GetPrimAtPath(Path("/World/MjcActuators/piston", ""));
  TEST_CHECK(bool(pr));
  if (pr) {
    const auto *a = (*pr)->as<MjcActuator>();
    if (a && a->target.authored()) {
      auto tg = a->target.get_targetPaths();
      TEST_CHECK(tg.size() == 1 && tg[0].prim_part() == "/World/Joints/ankle");
    }
  }
}

// MJCF <sensor> -> MjcSensor prims (typed schema) under /World/Sensors.
void urdf_json_mjc_sensors_test(void) {
  const char *robot_json = R"JSON({
  "name": "SensBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "joints": [],
  "sensors": [
    { "name": "imu_gyro", "type": "gyro", "objtype": "site", "objname": "imu",
      "cutoff": 34.9, "noise": 0.01 },
    { "name": "hip_pos", "type": "jointpos", "objtype": "joint", "objname": "hip" },
    { "name": "torso_pos", "type": "framepos", "objtype": "body", "objname": "torso",
      "reftype": "body", "refname": "world" }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto gr = stage.GetPrimAtPath(Path("/World/Sensors/imu_gyro", ""));
  TEST_CHECK(bool(gr));
  if (gr) {
    TEST_CHECK((*gr)->is<MjcSensor>());
    const auto *s = (*gr)->as<MjcSensor>();
    TEST_CHECK(s != nullptr);
    if (s) {
      TEST_CHECK(s->type.get_value().str() == "gyro");
      TEST_CHECK(s->objType.get_value().str() == "site");
      TEST_CHECK(s->objName.get_value().str() == "imu");
      TEST_CHECK(approx_eq(s->cutoff.get_value(), 34.9));
      TEST_CHECK(approx_eq(s->noise.get_value(), 0.01));
    }
  }
  auto fr = stage.GetPrimAtPath(Path("/World/Sensors/torso_pos", ""));
  TEST_CHECK(bool(fr));
  if (fr) {
    const auto *s = (*fr)->as<MjcSensor>();
    if (s) {
      TEST_CHECK(s->type.get_value().str() == "framepos");
      TEST_CHECK(s->refType.get_value().str() == "body");
      TEST_CHECK(s->refName.get_value().str() == "world");
    }
  }

  std::string json;
  std::string json_err;
  tydra::PhysicsJsonExportOptions opts;
  opts.include_mjc = true;
  bool json_ok = tydra::ConvertPhysicsToJson(stage, &json, &json_err, opts);
  if (!json_ok) { TEST_MSG("JSON export failed: %s", json_err.c_str()); }
  TEST_CHECK(json_ok);
  if (json_ok) {
    TEST_CHECK(json.find("\"sensors\"") != std::string::npos);
    TEST_CHECK(json.find("\"path\": \"/World/Sensors/imu_gyro\"") !=
               std::string::npos);
    TEST_CHECK(json.find("\"type\": \"gyro\"") != std::string::npos);
    TEST_CHECK(json.find("\"objtype\": \"site\"") != std::string::npos);
    TEST_CHECK(json.find("\"objname\": \"imu\"") != std::string::npos);
    TEST_CHECK(json.find("\"cutoff\": 34.9") != std::string::npos);
    TEST_CHECK(json.find("\"noise\": 0.01") != std::string::npos);
    TEST_CHECK(json.find("\"type\": \"framepos\"") != std::string::npos);
    TEST_CHECK(json.find("\"reftype\": \"body\"") != std::string::npos);
    TEST_CHECK(json.find("\"refname\": \"world\"") != std::string::npos);
  }

  // USDC round-trip preserves the sensor prim + typed fields.
  Stage rt;
  std::string w2, e2;
  if (usdc_roundtrip(R"(#usda 1.0
def "World" { def MjcSensor "g" { uniform token mjc:type = "gyro"
    uniform token mjc:objtype = "site"
    uniform double mjc:cutoff = 12.5 } })", &rt, &w2, &e2)) {
    auto r = rt.GetPrimAtPath(Path("/World/g", ""));
    TEST_CHECK(bool(r));
    if (r) {
      const auto *s = (*r)->as<MjcSensor>();
      TEST_CHECK(s != nullptr);
      if (s) {
        TEST_CHECK(s->type.get_value().str() == "gyro");
        TEST_CHECK(approx_eq(s->cutoff.get_value(), 12.5));
      }
    }
  }
}

void urdf_json_mjc_sensor_aliases_test(void) {
  const char *robot_json = R"JSON({
  "name": "SensorAliasBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "sensors": [
    { "name": "g", "type": "gyro", "site": "imu",
      "group": 4, "user": [1, 2, 3] },
    { "name": "tv", "type": "tendonvel", "tendon": "achilles" },
    { "name": "fp", "type": "framepos", "body": "torso",
      "refSite": "imu" }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto g = stage.GetPrimAtPath(Path("/World/Sensors/g", ""));
  TEST_CHECK(bool(g));
  if (g) {
    const auto *s = (*g)->as<MjcSensor>();
    TEST_CHECK(s != nullptr);
    if (s) {
      TEST_CHECK(s->objType.get_value().str() == "site");
      TEST_CHECK(s->objName.get_value().str() == "imu");
      TEST_CHECK(s->group.authored() && s->group.get_value() == 4);
      auto user = s->user.get_value();
      TEST_CHECK(user.has_value() && user.value().size() == 3);
    }
  }

  auto tv = stage.GetPrimAtPath(Path("/World/Sensors/tv", ""));
  TEST_CHECK(bool(tv));
  if (tv) {
    const auto *s = (*tv)->as<MjcSensor>();
    TEST_CHECK(s != nullptr);
    if (s) {
      TEST_CHECK(s->objType.get_value().str() == "tendon");
      TEST_CHECK(s->objName.get_value().str() == "achilles");
    }
  }

  auto fp = stage.GetPrimAtPath(Path("/World/Sensors/fp", ""));
  TEST_CHECK(bool(fp));
  if (fp) {
    const auto *s = (*fp)->as<MjcSensor>();
    TEST_CHECK(s != nullptr);
    if (s) {
      TEST_CHECK(s->objType.get_value().str() == "body");
      TEST_CHECK(s->objName.get_value().str() == "torso");
      TEST_CHECK(s->refType.get_value().str() == "site");
      TEST_CHECK(s->refName.get_value().str() == "imu");
    }
  }
}

// MJCF <asset><material> -> UsdShade Material (UsdPreviewSurface) + geom binding.
void urdf_json_mjc_materials_test(void) {
  const char *robot_json = R"JSON({
  "name": "MatBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [
    { "name": "base", "visuals": [
      { "name": "box",
        "positions": [0,0,0, 1,0,0, 0,1,0],
        "indices": [0,1,2],
        "material": "red" }
    ] }
  ],
  "joints": [],
  "materials": [
    { "name": "red", "rgba": [1, 0, 0, 1], "metallic": 0.2, "roughness": 0.8 }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  // Material prim + child UsdPreviewSurface shader.
  auto mr = stage.GetPrimAtPath(Path("/World/Materials/red", ""));
  TEST_CHECK(bool(mr));
  if (mr) {
    TEST_CHECK((*mr)->is<Material>());
    auto sr = stage.GetPrimAtPath(Path("/World/Materials/red/PreviewSurface", ""));
    TEST_CHECK(bool(sr));
    if (sr) TEST_CHECK((*sr)->is<Shader>());
  }
  // Visual mesh bound to the material.
  auto vr = stage.GetPrimAtPath(Path("/World/Links/base/box", ""));
  TEST_CHECK(bool(vr));
  if (vr) {
    TEST_CHECK((*vr)->is<GeomMesh>());
    const auto *mesh = (*vr)->as<GeomMesh>();
    TEST_CHECK(mesh != nullptr);
    if (mesh) TEST_CHECK(mesh->has_materialBinding());
  }
}

// MJCF <light>/<camera> -> UsdLux (DistantLight/SphereLight) + GeomCamera.
void urdf_json_mjc_lights_cameras_test(void) {
  const char *robot_json = R"JSON({
  "name": "LitBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "joints": [],
  "lights": [
    { "name": "sun", "type": "directional",
      "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,3,1],
      "dir": [0,0,-1], "color": [0.8, 0.8, 0.7], "castshadow": true },
    { "name": "bulb", "type": "point",
      "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 1,0,2,1], "color": [1,1,1] }
  ],
  "cameras": [
    { "name": "cam", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-2,1,1], "fovy": 60 }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto sun = stage.GetPrimAtPath(Path("/World/Lights/sun", ""));
  TEST_CHECK(bool(sun));
  if (sun) TEST_CHECK((*sun)->is<DistantLight>());

  auto bulb = stage.GetPrimAtPath(Path("/World/Lights/bulb", ""));
  TEST_CHECK(bool(bulb));
  if (bulb) TEST_CHECK((*bulb)->is<SphereLight>());

  auto cam = stage.GetPrimAtPath(Path("/World/Cameras/cam", ""));
  TEST_CHECK(bool(cam));
  if (cam) {
    TEST_CHECK((*cam)->is<GeomCamera>());
    const auto *gc = (*cam)->as<GeomCamera>();
    TEST_CHECK(gc != nullptr);
    if (gc) {
      // Camera placed via a baked world transform (xformOp:transform).
      TEST_CHECK(!gc->xformOps.empty());
    }
  }
}

// MJCF <keyframe><key> -> MjcKeyframe prim under /World/Keyframes.
void urdf_json_mjc_keyframe_export_test(void) {
  const char *robot_json = R"JSON({
  "name": "KeyBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "joints": [],
  "keyframes": [
    { "name": "init", "qpos": [0, 0, 0.5, 1, 0, 0, 0], "qvel": [0, 0, 0, 0, 0, 0],
      "ctrl": [0.1, 0.2] }
  ]
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto r = stage.GetPrimAtPath(Path("/World/Keyframes/init", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const auto *kf = (*r)->as<MjcKeyframe>();
  TEST_CHECK(kf != nullptr);
  if (!kf) return;
  auto qpos = kf->qpos.get_value();
  TEST_CHECK(qpos.has_value());
  if (qpos.has_value()) {
    TEST_CHECK(qpos.value().size() == 7);
    if (qpos.value().size() == 7) {
      TEST_CHECK(approx_eq(qpos.value()[2], 0.5));
      TEST_CHECK(approx_eq(qpos.value()[3], 1.0));
    }
  }
  auto ctrl = kf->ctrl.get_value();
  TEST_CHECK(ctrl.has_value());
  if (ctrl.has_value() && ctrl.value().size() == 2) {
    TEST_CHECK(approx_eq(ctrl.value()[0], 0.1));
    TEST_CHECK(approx_eq(ctrl.value()[1], 0.2));
  }
}

// MJCF <option>/<option><flag>/<compiler> full set -> MjcSceneAPI (JSON->USD).
// The schema + USDC round-trip already existed; this locks the parser->converter
// path that now populates all the fields (not just timestep).
void urdf_json_mjc_scene_options_test(void) {
  const char *robot_json = R"JSON({
  "name": "OptBot",
  "sourceFormat": "mjcf",
  "upAxis": "Z",
  "links": [ { "name": "base" } ],
  "joints": [],
  "mjcScene": {
    "option": {
      "timestep": 0.001, "integrator": "implicitfast", "solver": "newton",
      "cone": "elliptic", "jacobian": "auto", "iterations": 50,
      "ls_iterations": 8, "tolerance": 1e-9, "impratio": 5.0,
      "density": 1.2, "viscosity": 0.001, "wind": [1, 0, 0]
    },
    "flag": { "contact": false, "gravity": true, "eulerdamp": false, "override": true },
    "compiler": { "autolimits": true, "angle": "radian", "boundmass": 0.0001,
                  "inertiafromgeom": "auto", "balanceinertia": true }
  }
})JSON";
  Stage stage;
  std::string warn, err;
  bool ok = lightusd::tydra::ConvertURDFJsonToUSDStage(robot_json, &stage, &warn, &err);
  if (!ok) { TEST_MSG("convert failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto r = stage.GetPrimAtPath(Path("/World/PhysicsScene", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const auto *scene = (*r)->as<PhysicsScene>();
  TEST_CHECK(scene != nullptr);
  if (!scene || !scene->mjcScene.has_value()) { TEST_CHECK(false); return; }
  const auto &m = scene->mjcScene.value();
  // option
  TEST_CHECK(approx_eq(m.timestep.get_value(), 0.001));
  TEST_CHECK(m.integrator.get_value().str() == "implicitfast");
  TEST_CHECK(m.solver.get_value().str() == "newton");
  TEST_CHECK(m.cone.get_value().str() == "elliptic");
  TEST_CHECK(m.iterations.get_value() == 50);
  TEST_CHECK(m.ls_iterations.get_value() == 8);
  TEST_CHECK(approx_eq(m.impratio.get_value(), 5.0));
  TEST_CHECK(approx_eq(m.density.get_value(), 1.2));
  // flag (disable -> false; enable -> true)
  TEST_CHECK(m.flag_contact.get_value() == false);
  TEST_CHECK(m.flag_gravity.get_value() == true);
  TEST_CHECK(m.flag_eulerdamp.get_value() == false);
  TEST_CHECK(m.flag_override.get_value() == true);
  // compiler
  TEST_CHECK(m.compiler_autoLimits.get_value() == true);
  TEST_CHECK(m.compiler_angle.get_value().str() == "radian");
  TEST_CHECK(approx_eq(m.compiler_boundMass.get_value(), 0.0001));
  TEST_CHECK(m.compiler_balanceInertia.get_value() == true);
}

// ===========================================================================
// Phase 3b: schema-coverage tests for previously-unasserted APIs.
// ===========================================================================

// PhysicsArticulationRootAPI + NewtonArticulationRootAPI marker schemas survive
// parse and USDC round-trip.
void physics_articulation_root_api_test(void) {
  const char *usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Xform "Robot" (
        prepend apiSchemas = ["PhysicsArticulationRootAPI", "NewtonArticulationRootAPI"]
    )
    {
        bool newton:selfCollisionEnabled = 0
    }
}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;
  auto r = stage.GetPrimAtPath(Path("/World/Robot", ""));
  TEST_CHECK(bool(r));
  if (!r) return;
  const Prim *p = *r;
  TEST_CHECK(has_api(p, APISchemas::APIName::PhysicsArticulationRootAPI));
  TEST_CHECK(has_api(p, APISchemas::APIName::NewtonArticulationRootAPI));
}

// MjcEquality{Connect,Weld,Joint}API applied schemas + their mjc:* attributes
// survive parse and USDC round-trip (generic-prop preservation contract).
void mjc_equality_api_test(void) {
  const char *usda = R"(#usda 1.0

def Xform "Connect" (
    prepend apiSchemas = ["MjcEqualityConnectAPI"]
)
{
    rel mjc:target = </BodyB>
    double[] mjc:solref = [0.02, 1.0]
}

def Xform "Weld" (
    prepend apiSchemas = ["MjcEqualityWeldAPI"]
)
{
    rel mjc:target = </BodyB>
    float mjc:torqueScale = 0.75
}

def Xform "JointEq" (
    prepend apiSchemas = ["MjcEqualityJointAPI"]
)
{
    double mjc:coef0 = 0.0
    double mjc:coef1 = -1.0
}

def Cube "BodyB" {}
)";
  Stage stage;
  std::string warn, err;
  bool ok = usdc_roundtrip(usda, &stage, &warn, &err);
  if (!ok) { TEST_MSG("USDC roundtrip failed: %s", err.c_str()); }
  TEST_CHECK(ok);
  if (!ok) return;

  auto cr = stage.GetPrimAtPath(Path("/Connect", ""));
  TEST_CHECK(bool(cr));
  if (cr) TEST_CHECK(has_api(*cr, APISchemas::APIName::MjcEqualityConnectAPI));

  auto wr = stage.GetPrimAtPath(Path("/Weld", ""));
  TEST_CHECK(bool(wr));
  if (wr) {
    TEST_CHECK(has_api(*wr, APISchemas::APIName::MjcEqualityWeldAPI));
    const auto *x = (*wr)->as<Xform>();
    if (x) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(x->props, "mjc:torqueScale", &v));
      TEST_CHECK(approx_eq(v, 0.75));
    }
  }

  auto jr = stage.GetPrimAtPath(Path("/JointEq", ""));
  TEST_CHECK(bool(jr));
  if (jr) {
    TEST_CHECK(has_api(*jr, APISchemas::APIName::MjcEqualityJointAPI));
    const auto *x = (*jr)->as<Xform>();
    if (x) {
      double v = 0.0;
      TEST_CHECK(get_prop_num(x->props, "mjc:coef1", &v));
      TEST_CHECK(approx_eq(v, -1.0));
    }
  }
}

#include "usdObj.hh"
#include "tydra/obj-export.hh"
void physics_obj_c11_import_test(void) {
  lightusd::GPrim mesh;
  std::string error;
  const char* text =
      "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn "
      "0 0 -1\nf 1/1/1 2/2/1 3/3/1 4/4/1\n";
  TEST_CHECK(lightusd::usdObj::ReadObjFromString(text, &mesh, &error));
  auto counts = mesh.props.at("faceVertexCounts")
                    .get_attribute()
                    .get_value<std::vector<int32_t>>();
  TEST_CHECK(counts && counts->size() == 1 && (*counts)[0] == 4);
  auto normals = mesh.props.at("normals")
                     .get_attribute()
                     .get_value<std::vector<lightusd::value::normal3f>>();
  TEST_CHECK(normals && normals->size() == 4 && (*normals)[0][2] == -1);
  TEST_CHECK(mesh.props.count("primvars:st") == 1);
  lightusd::tydra::RenderScene scene;
  lightusd::tydra::RenderMesh rendered;
  rendered.points = {{0,0,0}, {1,0,0}, {0,1,0}};
  rendered.usdFaceVertexCounts = {3};
  rendered.usdFaceVertexIndices = {0,1,2};
  rendered.joint_and_weights.elementSize = 2;
  rendered.joint_and_weights.jointIndices = {0,1,1,0,0,1};
  rendered.joint_and_weights.jointWeights = {.123456789f,.876543211f,2,0,-.5f,1.5f};
  scene.meshes.push_back(std::move(rendered));
  std::string exported, mtl, warning;
  TEST_CHECK(lightusd::tydra::export_to_obj(scene, 0, exported, mtl, &warning, &error));
  TEST_CHECK(exported.find("vw 0 0 ") != std::string::npos);
  lightusd::GPrim reloaded;
  TEST_CHECK(lightusd::usdObj::ReadObjFromString(exported, &reloaded, &error));
  auto weights = reloaded.props.at("primvars:skel:jointWeights")
                      .get_attribute().get_value<std::vector<float>>();
  TEST_CHECK(weights && *weights == scene.meshes[0].joint_and_weights.jointWeights);
}

// USD -> OBJ export: face-varying corners are 1-based corner indices, subset
// groups follow face order, materials are written once, and the result
// reloads with the same topology and attributes.
void physics_obj_export_facevarying_test(void) {
  lightusd::tydra::RenderScene scene;
  lightusd::tydra::RenderMesh rendered;
  rendered.prim_name = "quads";
  rendered.points = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {2, 1, 0}};
  rendered.usdFaceVertexCounts = {4, 4, 3};
  rendered.usdFaceVertexIndices = {0, 1, 2, 3, 1, 4, 5, 2, 0, 1, 3};
  std::vector<float> uv;
  std::vector<float> normals;
  for (size_t corner = 0; corner < 11; corner++) {
    uv.push_back(float(corner) * 0.1f);
    uv.push_back(1.0f - float(corner) * 0.05f);
    normals.insert(normals.end(), {0.0f, 0.0f, corner % 2 ? 1.0f : -1.0f});
  }
  lightusd::tydra::VertexAttribute texcoord;
  texcoord.format = lightusd::tydra::VertexAttributeFormat::Vec2;
  texcoord.variability = lightusd::tydra::VertexVariability::FaceVarying;
  texcoord.set_buffer(reinterpret_cast<const uint8_t *>(uv.data()), uv.size() * sizeof(float));
  rendered.texcoords[0] = texcoord;
  rendered.normals.format = lightusd::tydra::VertexAttributeFormat::Vec3;
  rendered.normals.variability = lightusd::tydra::VertexVariability::FaceVarying;
  rendered.normals.set_buffer(reinterpret_cast<const uint8_t *>(normals.data()),
                              normals.size() * sizeof(float));
  for (const char *name : {"red", "blue"}) {
    lightusd::tydra::RenderMaterial material;
    material.name = name;
    scene.materials.push_back(material);
  }
  // Subsets listed out of face order; face 0 has no subset.
  lightusd::tydra::MaterialSubset late, early;
  late.material_id = 0;
  late.usdIndices = {2};
  early.material_id = 1;
  early.usdIndices = {1};
  rendered.material_subsetMap["zz_late"] = late;
  rendered.material_subsetMap["aa_early"] = early;
  rendered.material_id = 0;
  scene.meshes.push_back(std::move(rendered));
  std::string obj, mtl, warning, error;
  TEST_CHECK(lightusd::tydra::export_to_obj(scene, 0, obj, mtl, &warning, &error));
  // Corner (not face) offsets, 1-based, in face order.
  TEST_CHECK(obj.find("f 1/1/1 2/2/2 3/3/3 4/4/4\n") != std::string::npos);
  TEST_CHECK(obj.find("g aa_early\nusemtl blue\nf 2/5/5 5/6/6 6/7/7 3/8/8\n") != std::string::npos);
  TEST_CHECK(obj.find("g zz_late\nusemtl red\nf 1/9/9 2/10/10 4/11/11\n") != std::string::npos);
  TEST_CHECK(obj.find("g aa_early") < obj.find("g zz_late"));
  // Each material once, even when used by two groups.
  TEST_CHECK(mtl.find("newmtl red") == mtl.rfind("newmtl red"));
  TEST_CHECK(mtl.find("# 2 materials.") != std::string::npos);
  lightusd::GPrim reloaded;
  TEST_CHECK(lightusd::usdObj::ReadObjFromString(obj, &reloaded, &error));
  // Out-of-range subset faces are rejected, not read past the array.
  scene.meshes[0].material_subsetMap["zz_late"].usdIndices = {7};
  TEST_CHECK(!lightusd::tydra::export_to_obj(scene, 0, obj, mtl, &warning, &error));
}

#include <chrono>
#include <filesystem>
#include <fstream>
#include "composition.hh"
#include "asset-resolution.hh"
#include "layer.hh"
void physics_obj_composition_test(void) {
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() /
      ("lightusd-obj-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvw 0 0 1\nf 1 2 3\nl 1 2\np 3\n";
  { std::ofstream file(dir / "mesh.obj"); file << obj; }
  for (const char* arc : {"references", "payload", "subLayers"}) {
    std::string text = "#usda 1.0\n";
    if (std::string(arc) == "subLayers")
      text += "(subLayers = [@mesh.obj@])\n";
    else
      text += std::string("def Xform \"Rig\" (prepend ") + arc + " = @mesh.obj@) {}\n";
    { std::ofstream file(dir / "root.usda"); file << text; }
    Stage stage;
    Layer layer, stacked, composed;
    AssetResolutionResolver resolver;
    std::string warning, error;
    const bool ok = LoadLayerFromFile((dir / "root.usda").string(), &layer,
                                     &warning, &error) &&
                    CompositeSublayers(resolver, layer, &stacked, &warning, &error) &&
                    CompositeAllArcs(resolver, stacked, &composed, &warning, &error) &&
                    LayerToStage(std::move(composed), &stage, &warning, &error);
    TEST_CHECK(ok);
    TEST_MSG("%s: %s", arc, error.c_str());
    if (ok) {
      const std::string root = std::string(arc) == "subLayers" ? "/World" : "/Rig";
      TEST_CHECK(bool(stage.GetPrimAtPath(Path(root + "/Links/Geometry/mesh", ""))));
      TEST_MSG("arc: %s; warnings: %s", arc, warning.c_str());
      TEST_CHECK(bool(stage.GetPrimAtPath(Path(root + "/Links/Geometry/mesh_curves", ""))));
      TEST_CHECK(bool(stage.GetPrimAtPath(Path(root + "/Links/Geometry/mesh_points", ""))));
    }
  }
  fs::remove_all(dir);
}
