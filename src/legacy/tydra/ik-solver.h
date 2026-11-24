/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026-Present Light Transport Entertainment Inc. */
/*
 * ik-solver.h — Inverse Kinematics solver (C API)
 *
 * Dependency-free IK for skeletal chains with joint limit constraints.
 * Supports CCD (Cyclic Coordinate Descent) and FABRIK algorithms.
 *
 * Usage:
 *   1. Populate a TydraIKChain with joints, target, and settings
 *   2. Call tydra_ik_solve(&chain)
 *   3. Read updated joint transforms from chain.joints[i].current_local
 *
 * Memory: The C API does not allocate. Caller provides all buffers.
 * See ik-solver.hh for C++ helpers that build chains from Tydra data.
 */
#ifndef LIGHTUSD_TYDRA_IK_SOLVER_H_
#define LIGHTUSD_TYDRA_IK_SOLVER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* Enums                                                                    */
/* ======================================================================== */

typedef enum {
  TYDRA_IK_JOINT_FREE = 0,     /* 3-DOF rotation, no limits         */
  TYDRA_IK_JOINT_REVOLUTE,     /* 1-DOF rotation around axis        */
  TYDRA_IK_JOINT_PRISMATIC,    /* 1-DOF translation along axis      */
  TYDRA_IK_JOINT_SPHERICAL,    /* 3-DOF rotation with cone limits   */
  TYDRA_IK_JOINT_FIXED         /* 0-DOF, locked to rest pose        */
} TydraIKJointType;

typedef enum {
  TYDRA_IK_AXIS_X = 0,
  TYDRA_IK_AXIS_Y = 1,
  TYDRA_IK_AXIS_Z = 2
} TydraIKAxis;

typedef enum {
  TYDRA_IK_ALGO_CCD    = 0,    /* Cyclic Coordinate Descent         */
  TYDRA_IK_ALGO_FABRIK = 1     /* Forward And Backward Reaching IK  */
} TydraIKAlgorithm;

typedef enum {
  TYDRA_IK_OK                =  0,
  TYDRA_IK_ERR_NULL_INPUT    = -1,
  TYDRA_IK_ERR_INVALID_CHAIN = -2,
  TYDRA_IK_ERR_NO_CONVERGENCE= -3,
  TYDRA_IK_ERR_UNREACHABLE   = -4,
  TYDRA_IK_ERR_INTERNAL      = -5
} TydraIKResult;

/* ======================================================================== */
/* Math types — aliases to shared rb-math.h types                           */
/* ======================================================================== */

#include "rb-math.h"

typedef TydraPhysVec3 TydraIKVec3;
typedef TydraPhysQuat TydraIKQuat;
typedef TydraPhysMat4 TydraIKMat4;

/* ======================================================================== */
/* IK Joint                                                                 */
/* ======================================================================== */

typedef struct {
  int32_t joint_id;           /* Index in original skeleton            */
  int32_t parent_id;          /* Index in THIS chain array (-1 = root) */

  TydraIKMat4 rest_local;    /* Rest-pose local transform             */
  TydraIKMat4 current_local; /* Current local transform (solver I/O)  */
  TydraIKMat4 world;         /* World transform (computed by FK)      */

  TydraIKJointType type;
  TydraIKAxis      axis;     /* Primary axis (revolute/prismatic)     */

  /* Limits (radians for rotation, world units for translation).
   * Set to +/-1e18 for unlimited. */
  float lower_limit;
  float upper_limit;
  float cone_angle0_limit;   /* Spherical: swing limit around axis 0  */
  float cone_angle1_limit;   /* Spherical: swing limit around axis 1  */

  float bone_length;         /* Distance to parent joint              */
} TydraIKJoint;

/* ======================================================================== */
/* IK Target                                                                */
/* ======================================================================== */

typedef struct {
  TydraIKVec3  position;          /* World-space target position      */
  TydraIKQuat  orientation;       /* World-space target orientation   */
  int32_t      use_orientation;   /* 0 = position only, 1 = + orient */
  float        position_weight;   /* [0,1] blend weight               */
  float        orientation_weight;/* [0,1] blend weight               */
} TydraIKTarget;

/* ======================================================================== */
/* Solver settings                                                          */
/* ======================================================================== */

typedef struct {
  TydraIKAlgorithm algorithm;
  int32_t max_iterations;         /* Default: 32                      */
  float   tolerance;              /* Position error threshold. 1e-4   */
  float   damping;                /* CCD damping factor. 1.0 = none   */
  int32_t enforce_limits;         /* 0 = ignore, 1 = clamp            */
} TydraIKSettings;

/* ======================================================================== */
/* IK Chain                                                                 */
/* ======================================================================== */

typedef struct {
  TydraIKJoint   *joints;        /* Array [0]=root .. [num-1]=tip     */
  int32_t         num_joints;

  TydraIKTarget   target;
  TydraIKSettings settings;

  /* Output (filled by solver) */
  int32_t iterations_used;
  float   final_error;
} TydraIKChain;

/* ======================================================================== */
/* API Functions                                                            */
/* ======================================================================== */

/* Fill settings with sensible defaults. */
void tydra_ik_settings_default(TydraIKSettings *s);

/* Set position-only target. */
void tydra_ik_target_set(TydraIKTarget *t, float px, float py, float pz);

/* Compute forward kinematics: fills joints[i].world from current_local. */
TydraIKResult tydra_ik_forward_kinematics(TydraIKChain *chain);

/* Solve IK. Updates joints[i].current_local.
 * Returns TYDRA_IK_OK on convergence, TYDRA_IK_ERR_NO_CONVERGENCE if
 * max_iterations reached (partial solution still valid). */
TydraIKResult tydra_ik_solve(TydraIKChain *chain);

/* Clamp a single joint's current_local to its type-specific limits. */
void tydra_ik_clamp_joint(TydraIKJoint *joint);

/* Get end-effector world position (tip joint).
 * Returns zero vec if chain is null/empty. */
TydraIKVec3 tydra_ik_effector_position(const TydraIKChain *chain);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LIGHTUSD_TYDRA_IK_SOLVER_H_ */
