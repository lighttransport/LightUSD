// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// ik-solver.hh — C++ convenience wrapper for the IK solver
//
// Bridges Tydra's SkelHierarchy + UsdPhysics joint constraints
// to the C IK solver API defined in ik-solver.h.
//
#pragma once

#include "ik-solver.h"

#include <string>
#include <vector>

namespace lightusd {
namespace tydra {

class SkelHierarchy;  // forward decl (scene-access.hh)

// Physics joint info used when building an IK chain.
// Extracted from PhysicsRevoluteJoint / PhysicsSphericalJoint / etc.
struct IKPhysicsJointInfo {
  int parent_joint_id{-1};        // Skeleton joint index of parent body
  int child_joint_id{-1};         // Skeleton joint index of child body
  TydraIKJointType type{TYDRA_IK_JOINT_FREE};
  TydraIKAxis axis{TYDRA_IK_AXIS_Y};
  float lower_limit{-1e18f};
  float upper_limit{1e18f};
  float cone_angle0_limit{1e18f};
  float cone_angle1_limit{1e18f};
};

struct IKChainBuildOptions {
  bool use_physics_limits = true;
  bool verbose = false;
};

///
/// Build an IK chain from a Tydra skeleton hierarchy.
///
/// Walks parent_joint_indices from end_effector_joint_id to root_joint_id
/// (or skeleton root if root_joint_id == -1) and populates out_chain.
///
/// If physics_joints is non-empty, joint limits are applied from matching
/// entries (matched by parent/child joint ID pairs).
///
/// The caller must call FreeIKChain() when done.
///
/// @return true on success.
///
bool BuildIKChain(
    const SkelHierarchy &skel,
    int32_t end_effector_joint_id,
    int32_t root_joint_id,
    const std::vector<IKPhysicsJointInfo> &physics_joints,
    TydraIKChain *out_chain,
    std::string *err = nullptr,
    const IKChainBuildOptions &options = {});

///
/// Write IK solution back to a flat local transform array.
///
/// For each joint in the chain, copies current_local (float[16]) into
/// local_transforms[joint_id * 16 .. joint_id * 16 + 15].
///
bool ApplyIKResult(
    const TydraIKChain &chain,
    float *local_transforms_4x4,
    int32_t num_skeleton_joints);

///
/// Free the joint array allocated by BuildIKChain.
///
void FreeIKChain(TydraIKChain *chain);

}  // namespace tydra
}  // namespace lightusd
