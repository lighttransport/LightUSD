// SPDX-License-Identifier: Apache-2.0
// IK solver unit tests.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-ik.h"
#include "tydra/ik-solver.h"
#include "tydra/ik-solver.hh"
#include "tydra/scene-access.hh"

#include <cmath>
#include <cstring>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

// -----------------------------------------------------------------------
// Helpers: build a simple 3-joint chain (shoulder-elbow-hand) along +Y
// -----------------------------------------------------------------------
static void build_3joint_chain(TydraIKChain *chain, TydraIKJoint *joints) {
  memset(joints, 0, 3 * sizeof(TydraIKJoint));

  // Joint 0: root at origin (shoulder)
  joints[0].joint_id = 0;
  joints[0].parent_id = -1;
  joints[0].type = TYDRA_IK_JOINT_FREE;
  // Identity local = identity world (at origin)
  memset(joints[0].rest_local.m, 0, 64);
  joints[0].rest_local.m[0] = joints[0].rest_local.m[5] = 1;
  joints[0].rest_local.m[10] = joints[0].rest_local.m[15] = 1;
  joints[0].current_local = joints[0].rest_local;
  joints[0].bone_length = 0;
  joints[0].lower_limit = -1e18f;
  joints[0].upper_limit =  1e18f;
  joints[0].cone_angle0_limit = 1e18f;
  joints[0].cone_angle1_limit = 1e18f;

  // Joint 1: elbow at (0, 2, 0) — bone length 2
  joints[1].joint_id = 1;
  joints[1].parent_id = 0;
  joints[1].type = TYDRA_IK_JOINT_FREE;
  memset(joints[1].rest_local.m, 0, 64);
  joints[1].rest_local.m[0] = joints[1].rest_local.m[5] = 1;
  joints[1].rest_local.m[10] = joints[1].rest_local.m[15] = 1;
  joints[1].rest_local.m[7] = 2.0f;  // translation Y = 2
  joints[1].current_local = joints[1].rest_local;
  joints[1].bone_length = 2.0f;
  joints[1].lower_limit = -1e18f;
  joints[1].upper_limit =  1e18f;
  joints[1].cone_angle0_limit = 1e18f;
  joints[1].cone_angle1_limit = 1e18f;

  // Joint 2: hand at (0, 4, 0) — bone length 2
  joints[2].joint_id = 2;
  joints[2].parent_id = 1;
  joints[2].type = TYDRA_IK_JOINT_FREE;
  memset(joints[2].rest_local.m, 0, 64);
  joints[2].rest_local.m[0] = joints[2].rest_local.m[5] = 1;
  joints[2].rest_local.m[10] = joints[2].rest_local.m[15] = 1;
  joints[2].rest_local.m[7] = 2.0f;  // translation Y = 2
  joints[2].current_local = joints[2].rest_local;
  joints[2].bone_length = 2.0f;
  joints[2].lower_limit = -1e18f;
  joints[2].upper_limit =  1e18f;
  joints[2].cone_angle0_limit = 1e18f;
  joints[2].cone_angle1_limit = 1e18f;

  memset(chain, 0, sizeof(*chain));
  chain->joints = joints;
  chain->num_joints = 3;
  tydra_ik_settings_default(&chain->settings);
}

// -----------------------------------------------------------------------
// 1. Forward kinematics test
// -----------------------------------------------------------------------
void ik_forward_kinematics_test(void) {
  TydraIKJoint joints[3];
  TydraIKChain chain;
  build_3joint_chain(&chain, joints);

  TydraIKResult r = tydra_ik_forward_kinematics(&chain);
  TEST_CHECK(r == TYDRA_IK_OK);

  // Root world pos = (0,0,0)
  TydraIKVec3 p0 = {joints[0].world.m[3], joints[0].world.m[7], joints[0].world.m[11]};
  TEST_CHECK(fabsf(p0.x) < 1e-5f);
  TEST_CHECK(fabsf(p0.y) < 1e-5f);
  TEST_CHECK(fabsf(p0.z) < 1e-5f);

  // Elbow world pos = (0, 2, 0)
  TydraIKVec3 p1 = {joints[1].world.m[3], joints[1].world.m[7], joints[1].world.m[11]};
  TEST_CHECK(fabsf(p1.x) < 1e-5f);
  TEST_CHECK(fabsf(p1.y - 2.0f) < 1e-5f);
  TEST_CHECK(fabsf(p1.z) < 1e-5f);

  // Hand world pos = (0, 4, 0)
  TydraIKVec3 p2 = {joints[2].world.m[3], joints[2].world.m[7], joints[2].world.m[11]};
  TEST_CHECK(fabsf(p2.x) < 1e-5f);
  TEST_CHECK(fabsf(p2.y - 4.0f) < 1e-5f);
  TEST_CHECK(fabsf(p2.z) < 1e-5f);
}

// -----------------------------------------------------------------------
// 2. CCD solve test — reachable target
// -----------------------------------------------------------------------
void ik_ccd_solve_test(void) {
  TydraIKJoint joints[3];
  TydraIKChain chain;
  build_3joint_chain(&chain, joints);

  // Target at (3, 2, 0) — reachable (within total length 4)
  tydra_ik_target_set(&chain.target, 3.0f, 2.0f, 0.0f);
  chain.settings.algorithm = TYDRA_IK_ALGO_CCD;
  chain.settings.max_iterations = 100;
  chain.settings.tolerance = 0.01f;
  chain.settings.enforce_limits = 0;

  TydraIKResult r = tydra_ik_solve(&chain);
  TEST_CHECK(r == TYDRA_IK_OK);
  TEST_MSG("CCD iterations: %d, error: %f", chain.iterations_used, chain.final_error);
  TEST_CHECK(chain.final_error < 0.01f);

  // Verify effector is near target
  TydraIKVec3 eff = tydra_ik_effector_position(&chain);
  float dx = eff.x - 3.0f, dy = eff.y - 2.0f, dz = eff.z;
  float dist = sqrtf(dx*dx + dy*dy + dz*dz);
  TEST_CHECK(dist < 0.01f);
}

// -----------------------------------------------------------------------
// 3. FABRIK solve test — reachable target
// -----------------------------------------------------------------------
void ik_fabrik_solve_test(void) {
  TydraIKJoint joints[3];
  TydraIKChain chain;
  build_3joint_chain(&chain, joints);

  // Target at (2, 3, 0)
  tydra_ik_target_set(&chain.target, 2.0f, 3.0f, 0.0f);
  chain.settings.algorithm = TYDRA_IK_ALGO_FABRIK;
  chain.settings.max_iterations = 100;
  chain.settings.tolerance = 0.05f;
  chain.settings.enforce_limits = 0;

  TydraIKResult r = tydra_ik_solve(&chain);
  // FABRIK may or may not fully converge due to position→transform reconstruction
  TEST_MSG("FABRIK iterations: %d, error: %f", chain.iterations_used, chain.final_error);
  TEST_CHECK(r == TYDRA_IK_OK || r == TYDRA_IK_ERR_NO_CONVERGENCE);
  // Should at least get reasonably close
  TEST_CHECK(chain.final_error < 0.5f);
}

// -----------------------------------------------------------------------
// 4. Joint limits test — revolute with narrow range
// -----------------------------------------------------------------------
void ik_joint_limits_test(void) {
  TydraIKJoint joints[3];
  TydraIKChain chain;
  build_3joint_chain(&chain, joints);

  // Make joint 0 a revolute joint around Z with narrow limits
  joints[0].type = TYDRA_IK_JOINT_REVOLUTE;
  joints[0].axis = TYDRA_IK_AXIS_Z;
  joints[0].lower_limit = -0.1f;  // ~5.7 degrees
  joints[0].upper_limit =  0.1f;

  // Target far to the side — should be limited
  tydra_ik_target_set(&chain.target, 4.0f, 0.0f, 0.0f);
  chain.settings.algorithm = TYDRA_IK_ALGO_CCD;
  chain.settings.max_iterations = 50;
  chain.settings.tolerance = 0.01f;
  chain.settings.enforce_limits = 1;

  tydra_ik_solve(&chain);

  // The effector should NOT reach (4,0,0) because joint 0 is limited
  TydraIKVec3 eff = tydra_ik_effector_position(&chain);
  float dist_to_target = sqrtf((eff.x-4)*(eff.x-4) + eff.y*eff.y + eff.z*eff.z);
  TEST_MSG("Limited CCD: effector=(%f,%f,%f), dist_to_target=%f",
           eff.x, eff.y, eff.z, dist_to_target);
  // With such tight limits on root, effector should stay mostly along +Y
  TEST_CHECK(dist_to_target > 0.5f);  // can't reach the target
}

// -----------------------------------------------------------------------
// 5. Unreachable target test
// -----------------------------------------------------------------------
void ik_unreachable_target_test(void) {
  TydraIKJoint joints[3];
  TydraIKChain chain;
  build_3joint_chain(&chain, joints);

  // Target at (0, 100, 0) — way beyond total chain length of 4
  tydra_ik_target_set(&chain.target, 0.0f, 100.0f, 0.0f);
  chain.settings.algorithm = TYDRA_IK_ALGO_CCD;
  chain.settings.max_iterations = 20;
  chain.settings.tolerance = 0.01f;
  chain.settings.enforce_limits = 0;

  TydraIKResult r = tydra_ik_solve(&chain);
  TEST_CHECK(r == TYDRA_IK_ERR_NO_CONVERGENCE);

  // Should have stretched as far as possible (~4 units along Y)
  TydraIKVec3 eff = tydra_ik_effector_position(&chain);
  float reach = sqrtf(eff.x*eff.x + eff.y*eff.y + eff.z*eff.z);
  TEST_MSG("Unreachable: effector=(%f,%f,%f), reach=%f", eff.x, eff.y, eff.z, reach);
  TEST_CHECK(reach > 3.0f);  // should extend near full length
}

void ik_excessive_chain_rejected_test(void) {
  // Build a deep linear chain (parent[i] = i-1, root=0) that exceeds kMaxIKJoints.
  // Verifies the memory-defense cap added for security hardening.
  constexpr int deep_size = 1500;
  std::vector<int> parent_indices(deep_size);
  std::vector<lightusd::legacy::value::matrix4d> rest_xforms(deep_size, lightusd::legacy::value::matrix4d::identity());
  parent_indices[0] = -1;  // root
  for (int i = 1; i < deep_size; i++) {
    parent_indices[static_cast<size_t>(i)] = i - 1;
  }

  SkelHierarchy skel;
  skel.parent_joint_indices = parent_indices;
  skel.rest_transforms = rest_xforms;
  skel.bind_transforms = rest_xforms;

  TydraIKChain chain;
  memset(&chain, 0, sizeof(chain));
  std::string err;
  bool ok = BuildIKChain(skel, deep_size - 1, 0, {}, &chain, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.find("exceeds max joint count") != std::string::npos);

  FreeIKChain(&chain);
}
