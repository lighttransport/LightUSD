// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// ik-solver.cc — IK solver implementation (CCD + FABRIK)
//

#include "ik-solver.h"

#include <memory>

// Math is provided by rb-math.h (included via ik-solver.h).
// Aliases from old ik_ prefix to tp_ prefix for minimal diff.

#define IK_EPSILON  TP_EPSILON
#define IK_UNLIMITED TP_UNLIMITED

// Alias ik_* functions to tp_* from rb-math.h
#define ik_v3              tp_v3
#define ik_v3_add          tp_v3_add
#define ik_v3_sub          tp_v3_sub
#define ik_v3_scale        tp_v3_scale
#define ik_v3_dot          tp_v3_dot
#define ik_v3_cross        tp_v3_cross
#define ik_v3_length       tp_v3_length
#define ik_v3_normalize    tp_v3_normalize
#define ik_q_identity      tp_q_identity
#define ik_q_mul           tp_q_mul
#define ik_q_conjugate     tp_q_conjugate
#define ik_q_normalize     tp_q_normalize
#define ik_q_from_axis_angle tp_q_from_axis_angle
#define ik_q_to_axis_angle tp_q_to_axis_angle
#define ik_q_rotate        tp_q_rotate
#define ik_m4_identity     tp_m4_identity
#define ik_m4_mul          tp_m4_mul
#define ik_m4_get_translation tp_m4_get_translation
#define ik_m4_set_translation tp_m4_set_translation
#define ik_m4_to_quat      tp_m4_to_quat
#define ik_m4_inverse_rigid tp_m4_inverse_rigid
#define ik_make_local_transform tp_make_local_transform
#define ik_axis_vector     tp_axis_vector
#define ik_clampf          tp_clampf


// ============================================================================
// Forward Kinematics
// ============================================================================

TydraIKResult tydra_ik_forward_kinematics(TydraIKChain *chain) {
  if (!chain || !chain->joints || chain->num_joints <= 0)
    return TYDRA_IK_ERR_NULL_INPUT;

  chain->joints[0].world = chain->joints[0].current_local;
  for (int i = 1; i < chain->num_joints; i++) {
    int pid = chain->joints[i].parent_id;
    if (pid < 0 || pid >= i)
      return TYDRA_IK_ERR_INVALID_CHAIN;
    ik_m4_mul(&chain->joints[pid].world, &chain->joints[i].current_local,
              &chain->joints[i].world);
  }
  return TYDRA_IK_OK;
}

// ============================================================================
// Joint limit clamping
// ============================================================================

void tydra_ik_clamp_joint(TydraIKJoint *joint) {
  if (!joint) return;

  switch (joint->type) {
    case TYDRA_IK_JOINT_FIXED:
      joint->current_local = joint->rest_local;
      break;

    case TYDRA_IK_JOINT_REVOLUTE: {
      // Decompose current local rotation to axis-angle
      TydraIKQuat q = ik_m4_to_quat(&joint->current_local);
      TydraIKVec3 ax;
      float angle = ik_q_to_axis_angle(q, &ax);

      // Project onto the joint axis
      TydraIKVec3 joint_axis = ik_axis_vector(joint->axis);
      float proj = ik_v3_dot(ax, joint_axis);
      if (proj < 0) { angle = -angle; }

      // Clamp angle
      angle = ik_clampf(angle, joint->lower_limit, joint->upper_limit);

      // Reconstruct
      TydraIKQuat clamped = ik_q_from_axis_angle(joint_axis, angle);
      TydraIKVec3 trans = ik_m4_get_translation(&joint->current_local);
      ik_make_local_transform(clamped, trans, &joint->current_local);
      break;
    }

    case TYDRA_IK_JOINT_SPHERICAL: {
      // Decompose to swing-twist around primary axis
      TydraIKQuat q = ik_m4_to_quat(&joint->current_local);
      TydraIKVec3 ax;
      float angle = ik_q_to_axis_angle(q, &ax);

      // Clamp swing angle to cone
      float max_angle = (joint->cone_angle0_limit < joint->cone_angle1_limit)
                             ? joint->cone_angle0_limit
                             : joint->cone_angle1_limit;
      if (max_angle < IK_UNLIMITED && angle > max_angle) {
        angle = max_angle;
      }

      TydraIKQuat clamped = ik_q_from_axis_angle(ax, angle);
      TydraIKVec3 trans = ik_m4_get_translation(&joint->current_local);
      ik_make_local_transform(clamped, trans, &joint->current_local);
      break;
    }

    case TYDRA_IK_JOINT_PRISMATIC: {
      // Clamp translation along axis
      TydraIKVec3 trans = ik_m4_get_translation(&joint->current_local);
      TydraIKVec3 rest_trans = ik_m4_get_translation(&joint->rest_local);
      TydraIKVec3 joint_axis = ik_axis_vector(joint->axis);
      TydraIKVec3 delta = ik_v3_sub(trans, rest_trans);
      float dist = ik_v3_dot(delta, joint_axis);
      dist = ik_clampf(dist, joint->lower_limit, joint->upper_limit);
      trans = ik_v3_add(rest_trans, ik_v3_scale(joint_axis, dist));
      ik_m4_set_translation(&joint->current_local, trans);
      break;
    }

    case TYDRA_IK_JOINT_FREE:
      break;
  }
}

// ============================================================================
// CCD Solver
// ============================================================================

static TydraIKResult solve_ccd(TydraIKChain *chain) {
  int n = chain->num_joints;
  if (n < 2) return TYDRA_IK_ERR_INVALID_CHAIN;

  TydraIKTarget *target = &chain->target;
  TydraIKSettings *settings = &chain->settings;
  int tip = n - 1;

  for (int iter = 0; iter < settings->max_iterations; iter++) {
    // Compute FK
    tydra_ik_forward_kinematics(chain);

    // Check convergence
    TydraIKVec3 effector = ik_m4_get_translation(&chain->joints[tip].world);
    TydraIKVec3 diff = ik_v3_sub(target->position, effector);
    float err = ik_v3_length(diff);

    chain->iterations_used = iter + 1;
    chain->final_error = err;

    if (err < settings->tolerance) {
      return TYDRA_IK_OK;
    }

    // Iterate from second-to-last joint down to root
    for (int i = tip - 1; i >= 0; i--) {
      if (chain->joints[i].type == TYDRA_IK_JOINT_FIXED ||
          chain->joints[i].type == TYDRA_IK_JOINT_PRISMATIC) {
        continue;
      }

      // Recompute FK (positions may have shifted from previous joint updates)
      tydra_ik_forward_kinematics(chain);

      TydraIKVec3 joint_pos = ik_m4_get_translation(&chain->joints[i].world);
      effector = ik_m4_get_translation(&chain->joints[tip].world);

      TydraIKVec3 to_effector = ik_v3_normalize(ik_v3_sub(effector, joint_pos));
      TydraIKVec3 to_target   = ik_v3_normalize(ik_v3_sub(target->position, joint_pos));

      float dot = ik_v3_dot(to_effector, to_target);
      dot = ik_clampf(dot, -1.0f, 1.0f);
      float angle = acosf(dot) * settings->damping;

      if (angle < IK_EPSILON) continue;

      TydraIKVec3 rot_axis = ik_v3_cross(to_effector, to_target);
      float axis_len = ik_v3_length(rot_axis);
      if (axis_len < IK_EPSILON) continue;
      rot_axis = ik_v3_scale(rot_axis, 1.0f / axis_len);

      // Convert world rotation axis to local space of this joint
      TydraIKMat4 world_inv;
      ik_m4_inverse_rigid(&chain->joints[i].world, &world_inv);
      TydraIKQuat world_inv_rot = ik_m4_to_quat(&world_inv);
      TydraIKVec3 local_axis = ik_q_rotate(world_inv_rot, rot_axis);
      local_axis = ik_v3_normalize(local_axis);

      // For revolute joints, project onto the joint axis
      if (chain->joints[i].type == TYDRA_IK_JOINT_REVOLUTE) {
        TydraIKVec3 j_axis = ik_axis_vector(chain->joints[i].axis);
        float proj = ik_v3_dot(local_axis, j_axis);
        if (fabsf(proj) < IK_EPSILON) continue;
        local_axis = j_axis;
        if (proj < 0) angle = -angle;
      }

      // Apply rotation to current local transform
      TydraIKQuat delta_rot = ik_q_from_axis_angle(local_axis, angle);
      TydraIKQuat cur_rot = ik_m4_to_quat(&chain->joints[i].current_local);
      TydraIKQuat new_rot = ik_q_normalize(ik_q_mul(cur_rot, delta_rot));
      TydraIKVec3 trans = ik_m4_get_translation(&chain->joints[i].current_local);
      ik_make_local_transform(new_rot, trans, &chain->joints[i].current_local);

      // Enforce limits
      if (settings->enforce_limits) {
        tydra_ik_clamp_joint(&chain->joints[i]);
      }
    }
  }

  return TYDRA_IK_ERR_NO_CONVERGENCE;
}

// ============================================================================
// FABRIK Solver
// ============================================================================

// FABRIK works on world-space positions, then reconstructs local transforms.
// Allocate position array on the stack (max chain length capped).
#define IK_FABRIK_MAX_JOINTS 256

static TydraIKResult solve_fabrik(TydraIKChain *chain) {
  int n = chain->num_joints;
  if (n < 2) return TYDRA_IK_ERR_INVALID_CHAIN;
  if (n > IK_FABRIK_MAX_JOINTS) return TYDRA_IK_ERR_INVALID_CHAIN;

  TydraIKTarget *target = &chain->target;
  TydraIKSettings *settings = &chain->settings;

  // Compute initial FK
  tydra_ik_forward_kinematics(chain);

  // Extract world positions and bone lengths
  TydraIKVec3 positions[IK_FABRIK_MAX_JOINTS];
  float lengths[IK_FABRIK_MAX_JOINTS];
  for (int i = 0; i < n; i++) {
    positions[i] = ik_m4_get_translation(&chain->joints[i].world);
  }
  for (int i = 1; i < n; i++) {
    lengths[i] = ik_v3_length(ik_v3_sub(positions[i], positions[i-1]));
    if (lengths[i] < IK_EPSILON) lengths[i] = IK_EPSILON;
  }

  TydraIKVec3 root_pos = positions[0];

  // Check total chain length vs target distance
  float total_length = 0;
  for (int i = 1; i < n; i++) total_length += lengths[i];
  (void)total_length; /* used for diagnostics, not for solving */

  for (int iter = 0; iter < settings->max_iterations; iter++) {
    // Check convergence
    float err = ik_v3_length(ik_v3_sub(target->position, positions[n-1]));
    chain->iterations_used = iter + 1;
    chain->final_error = err;
    if (err < settings->tolerance) break;

    // Forward reaching: tip to root
    positions[n-1] = target->position;
    for (int i = n - 2; i >= 0; i--) {
      TydraIKVec3 dir = ik_v3_sub(positions[i], positions[i+1]);
      float len = ik_v3_length(dir);
      if (len < IK_EPSILON) {
        dir = ik_v3(0, 1, 0);
        len = 1.0f;
      }
      dir = ik_v3_scale(dir, 1.0f / len);
      positions[i] = ik_v3_add(positions[i+1], ik_v3_scale(dir, lengths[i+1]));
    }

    // Backward reaching: root to tip
    positions[0] = root_pos;
    for (int i = 1; i < n; i++) {
      TydraIKVec3 dir = ik_v3_sub(positions[i], positions[i-1]);
      float len = ik_v3_length(dir);
      if (len < IK_EPSILON) {
        dir = ik_v3(0, 1, 0);
        len = 1.0f;
      }
      dir = ik_v3_scale(dir, 1.0f / len);
      positions[i] = ik_v3_add(positions[i-1], ik_v3_scale(dir, lengths[i]));
    }
  }

  // Reconstruct local transforms from solved world positions.
  // For each joint, compute the rotation that maps the original bone direction
  // to the new bone direction in parent space.
  for (int i = 0; i < n; i++) {
    if (i == 0) {
      // Root: keep original rotation, update translation
      ik_m4_set_translation(&chain->joints[0].current_local, positions[0]);
      // Compute rotation from rest child direction to new child direction
      if (n > 1) {
        TydraIKVec3 old_child = ik_m4_get_translation(&chain->joints[1].rest_local);
        float old_len = ik_v3_length(old_child);
        if (old_len > IK_EPSILON) {
          TydraIKVec3 old_dir = ik_v3_normalize(old_child);
          TydraIKVec3 new_dir = ik_v3_normalize(ik_v3_sub(positions[1], positions[0]));
          float dot = ik_v3_dot(old_dir, new_dir);
          dot = ik_clampf(dot, -1.0f, 1.0f);
          float angle = acosf(dot);
          if (angle > IK_EPSILON) {
            TydraIKVec3 axis = ik_v3_normalize(ik_v3_cross(old_dir, new_dir));
            TydraIKQuat rot = ik_q_from_axis_angle(axis, angle);
            TydraIKVec3 trans = positions[0];
            ik_make_local_transform(rot, trans, &chain->joints[0].current_local);
          }
        }
      }
    } else {
      // Non-root: compute local transform from parent world to this world pos
      TydraIKMat4 parent_inv;
      ik_m4_inverse_rigid(&chain->joints[i-1].world, &parent_inv);

      // Update this joint's world position
      TydraIKMat4 this_world;
      ik_m4_identity(&this_world);
      ik_m4_set_translation(&this_world, positions[i]);

      // Compute rotation: direction from parent to this in parent space
      TydraIKVec3 parent_pos = positions[i-1];
      TydraIKVec3 rest_local_trans = ik_m4_get_translation(&chain->joints[i].rest_local);
      float rest_len = ik_v3_length(rest_local_trans);

      if (rest_len > IK_EPSILON) {
        TydraIKVec3 old_dir = ik_v3_normalize(rest_local_trans);

        // New direction in parent local space
        TydraIKVec3 world_dir = ik_v3_normalize(ik_v3_sub(positions[i], parent_pos));
        TydraIKQuat parent_rot_inv = ik_q_conjugate(ik_m4_to_quat(&chain->joints[i-1].world));
        TydraIKVec3 local_dir = ik_q_rotate(parent_rot_inv, world_dir);
        local_dir = ik_v3_normalize(local_dir);

        float dot = ik_v3_dot(old_dir, local_dir);
        dot = ik_clampf(dot, -1.0f, 1.0f);
        float angle = acosf(dot);

        TydraIKQuat local_rot = ik_q_identity();
        if (angle > IK_EPSILON) {
          TydraIKVec3 axis = ik_v3_normalize(ik_v3_cross(old_dir, local_dir));
          if (ik_v3_length(axis) > IK_EPSILON) {
            local_rot = ik_q_from_axis_angle(axis, angle);
          }
        }
        TydraIKVec3 new_trans = ik_v3_scale(local_dir, rest_len);
        ik_make_local_transform(local_rot, new_trans, &chain->joints[i].current_local);
      }
    }

    // Enforce limits
    if (settings->enforce_limits) {
      tydra_ik_clamp_joint(&chain->joints[i]);
    }

    // Recompute FK from this joint onwards
    if (i > 0) {
      ik_m4_mul(&chain->joints[chain->joints[i].parent_id].world,
                &chain->joints[i].current_local,
                &chain->joints[i].world);
    } else {
      chain->joints[0].world = chain->joints[0].current_local;
    }
  }

  // Final FK + error
  tydra_ik_forward_kinematics(chain);
  TydraIKVec3 final_eff = ik_m4_get_translation(&chain->joints[n-1].world);
  chain->final_error = ik_v3_length(ik_v3_sub(target->position, final_eff));

  return (chain->final_error < settings->tolerance)
             ? TYDRA_IK_OK
             : TYDRA_IK_ERR_NO_CONVERGENCE;
}

// ============================================================================
// Public API
// ============================================================================

void tydra_ik_settings_default(TydraIKSettings *s) {
  if (!s) return;
  s->algorithm = TYDRA_IK_ALGO_CCD;
  s->max_iterations = 32;
  s->tolerance = 1e-4f;
  s->damping = 1.0f;
  s->enforce_limits = 1;
}

void tydra_ik_target_set(TydraIKTarget *t, float px, float py, float pz) {
  if (!t) return;
  t->position = ik_v3(px, py, pz);
  t->orientation = ik_q_identity();
  t->use_orientation = 0;
  t->position_weight = 1.0f;
  t->orientation_weight = 0.0f;
}

TydraIKResult tydra_ik_solve(TydraIKChain *chain) {
  if (!chain || !chain->joints || chain->num_joints <= 0)
    return TYDRA_IK_ERR_NULL_INPUT;
  if (chain->num_joints < 2)
    return TYDRA_IK_ERR_INVALID_CHAIN;

  chain->iterations_used = 0;
  chain->final_error = 0;

  switch (chain->settings.algorithm) {
    case TYDRA_IK_ALGO_CCD:
      return solve_ccd(chain);
    case TYDRA_IK_ALGO_FABRIK:
      return solve_fabrik(chain);
  }

  return TYDRA_IK_ERR_INTERNAL;
}

TydraIKVec3 tydra_ik_effector_position(const TydraIKChain *chain) {
  if (!chain || !chain->joints || chain->num_joints <= 0)
    return ik_v3(0, 0, 0);
  return ik_m4_get_translation(&chain->joints[chain->num_joints - 1].world);
}

// ============================================================================
// C++ Bridge (BuildIKChain, ApplyIKResult, FreeIKChain)
// ============================================================================
#ifdef __cplusplus

#include "ik-solver.hh"
#include "scene-access.hh"

#include <algorithm>
#include <cstring>

namespace lightusd {
namespace tydra {

static void mat4d_to_ik(const value::matrix4d &src, TydraIKMat4 *dst) {
  // value::matrix4d is row-major double[4][4], convert to float[16]
  const double *d = reinterpret_cast<const double *>(&src);
  for (int i = 0; i < 16; i++) dst->m[i] = static_cast<float>(d[i]);
}

bool BuildIKChain(
    const SkelHierarchy &skel,
    int32_t end_effector_joint_id,
    int32_t root_joint_id,
    const std::vector<IKPhysicsJointInfo> &physics_joints,
    TydraIKChain *out_chain,
    std::string *err,
    const IKChainBuildOptions &options) {

  if (!out_chain) {
    if (err) *err = "out_chain is null";
    return false;
  }

  int num_skel_joints = static_cast<int>(skel.num_joints());
  if (end_effector_joint_id < 0 || end_effector_joint_id >= num_skel_joints) {
    if (err) *err = "end_effector_joint_id out of range";
    return false;
  }

  // Walk from end effector to root, collecting joint indices
  std::vector<int> chain_indices;
  int cur = end_effector_joint_id;
  while (cur >= 0) {
    chain_indices.push_back(cur);
    if (cur == root_joint_id) break;
    cur = skel.parent_joint_indices[static_cast<size_t>(cur)];
  }
  std::reverse(chain_indices.begin(), chain_indices.end());

  int n = static_cast<int>(chain_indices.size());
  if (n < 2) {
    if (err) *err = "chain too short (need at least 2 joints)";
    return false;
  }

  constexpr int kMaxIKJoints = 1024;
  if (n > kMaxIKJoints) {
    if (err) *err = "IK chain exceeds max joint count (" +
                    std::to_string(kMaxIKJoints) + ")";
    return false;
  }

  // Allocate joints — wrapped in unique_ptr for RAII cleanup on any early return.
  std::unique_ptr<TydraIKJoint[]> joints(new TydraIKJoint[static_cast<size_t>(n)]);
  std::memset(joints.get(), 0, sizeof(TydraIKJoint) * static_cast<size_t>(n));

  for (int i = 0; i < n; i++) {
    int skel_id = chain_indices[static_cast<size_t>(i)];
    TydraIKJoint *j = &joints[static_cast<size_t>(i)];

    j->joint_id = skel_id;
    j->parent_id = (i > 0) ? (i - 1) : -1;

    mat4d_to_ik(skel.rest_transforms[static_cast<size_t>(skel_id)], &j->rest_local);
    j->current_local = j->rest_local;
    ik_m4_identity(&j->world);

    j->type = TYDRA_IK_JOINT_FREE;
    j->axis = TYDRA_IK_AXIS_Y;
    j->lower_limit = -1e18f;
    j->upper_limit =  1e18f;
    j->cone_angle0_limit = 1e18f;
    j->cone_angle1_limit = 1e18f;

    // Compute bone length from rest local translation
    TydraIKVec3 rest_trans = ik_m4_get_translation(&j->rest_local);
    j->bone_length = ik_v3_length(rest_trans);

    // Apply physics joint limits if available
    if (options.use_physics_limits && i > 0) {
      int parent_skel_id = chain_indices[static_cast<size_t>(i - 1)];
      for (const auto &pj : physics_joints) {
        if (pj.parent_joint_id == parent_skel_id &&
            pj.child_joint_id == skel_id) {
          j->type = pj.type;
          j->axis = pj.axis;
          j->lower_limit = pj.lower_limit;
          j->upper_limit = pj.upper_limit;
          j->cone_angle0_limit = pj.cone_angle0_limit;
          j->cone_angle1_limit = pj.cone_angle1_limit;
          break;
        }
      }
    }
  }

  out_chain->joints = joints.release();
  out_chain->num_joints = n;
  tydra_ik_settings_default(&out_chain->settings);
  out_chain->iterations_used = 0;
  out_chain->final_error = 0;

  return true;
}

bool ApplyIKResult(
    const TydraIKChain &chain,
    float *local_transforms_4x4,
    int32_t num_skeleton_joints) {

  if (!local_transforms_4x4 || !chain.joints) return false;

  for (int i = 0; i < chain.num_joints; i++) {
    int jid = chain.joints[i].joint_id;
    if (jid < 0 || jid >= num_skeleton_joints) continue;
    std::memcpy(&local_transforms_4x4[jid * 16],
                chain.joints[i].current_local.m,
                16 * sizeof(float));
  }
  return true;
}

void FreeIKChain(TydraIKChain *chain) {
  if (!chain) return;
  delete[] chain->joints;
  chain->joints = nullptr;
  chain->num_joints = 0;
}

}  // namespace tydra
}  // namespace lightusd

#endif  // __cplusplus
