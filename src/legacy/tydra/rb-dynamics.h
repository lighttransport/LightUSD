/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026-Present Light Transport Entertainment Inc. */
/*
 * rb-dynamics.h — Rigid body dynamics simulation (C API)
 *
 * Sequential impulse constraint solver with semi-implicit Euler integration,
 * Coulomb friction, joint constraints, island-based sleeping, and CCD.
 *
 * Usage:
 *   1. Allocate buffers and call tydra_phys_world_init()
 *   2. Add bodies, colliders, joints via tydra_phys_add_*()
 *   3. Each frame: call tydra_phys_step(&world)
 *   4. Read body transforms from world.bodies[i].xform
 *
 * Memory: The C API does not allocate. Caller provides all buffers.
 * See rb-dynamics.hh for C++ helpers that build worlds from USD Stages.
 */
#ifndef LIGHTUSD_TYDRA_RB_DYNAMICS_H_
#define LIGHTUSD_TYDRA_RB_DYNAMICS_H_

#include "rb-math.h"
#include "rb-collision.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* Enums                                                                    */
/* ======================================================================== */

typedef enum {
  TYDRA_PHYS_BODY_DYNAMIC   = 0,
  TYDRA_PHYS_BODY_STATIC    = 1,
  TYDRA_PHYS_BODY_KINEMATIC = 2
} TydraPhysBodyType;

typedef enum {
  TYDRA_PHYS_BODY_FLAG_CCD_ENABLED   = (1 << 0),
  TYDRA_PHYS_BODY_FLAG_GRAVITY       = (1 << 1),
  TYDRA_PHYS_BODY_FLAG_CAN_SLEEP     = (1 << 2),
  TYDRA_PHYS_BODY_FLAG_SLEEPING      = (1 << 3)
} TydraPhysBodyFlags;

typedef enum {
  TYDRA_PHYS_JOINT_BALL = 0,
  TYDRA_PHYS_JOINT_HINGE,
  TYDRA_PHYS_JOINT_SLIDER,
  TYDRA_PHYS_JOINT_FIXED,
  TYDRA_PHYS_JOINT_DISTANCE
} TydraPhysJointType;

typedef enum {
  TYDRA_PHYS_OK              =  0,
  TYDRA_PHYS_ERR_NULL_INPUT  = -1,
  TYDRA_PHYS_ERR_INTERNAL    = -2,
  TYDRA_PHYS_ERR_OVERFLOW    = -3
} TydraPhysResult;

/* ======================================================================== */
/* Rigid body                                                               */
/* ======================================================================== */

typedef struct {
  TydraPhysTransform xform;          /* World-space position + rotation    */
  TydraPhysVec3 linear_velocity;
  TydraPhysVec3 angular_velocity;

  float inverse_mass;                 /* 0 for static/kinematic            */
  TydraPhysVec3 local_inv_inertia;    /* Diagonal inverse inertia (local)  */
  TydraPhysMat3 inv_inertia_world;    /* Computed each step                */

  TydraPhysVec3 force_accum;          /* External forces (world space)     */
  TydraPhysVec3 torque_accum;         /* External torques (world space)    */

  TydraPhysBodyType body_type;
  uint32_t flags;                     /* TydraPhysBodyFlags bitmask        */

  int32_t collider_start;             /* First collider index              */
  int32_t collider_count;             /* Number of colliders               */
  int32_t island_id;                  /* Island index (-1 = unassigned)    */

  float sleep_timer;                  /* Seconds below motion threshold    */
  float motion_energy;                /* Smoothed kinetic energy           */
} TydraPhysBody;

/* ======================================================================== */
/* Joint constraint                                                         */
/* ======================================================================== */

typedef struct {
  TydraPhysJointType type;
  int32_t body_a;                     /* Body index (-1 = world anchor)    */
  int32_t body_b;                     /* Body index                        */
  TydraPhysTransform local_anchor_a;  /* Anchor frame in body A local      */
  TydraPhysTransform local_anchor_b;  /* Anchor frame in body B local      */

  /* Limits (type-dependent interpretation) */
  float lower_limit;
  float upper_limit;
  int32_t limit_enabled;

  /* Spring (from MjcJointAPI if available) */
  float stiffness;
  float damping;

  /* Distance joint specific */
  float min_distance;
  float max_distance;

  /* Hinge axis (local to anchor_a frame) */
  TydraPhysVec3 axis;

  /* Solver cache (accumulated impulses for warm starting) */
  float lambda_accum[6];
} TydraPhysJoint;

/* ======================================================================== */
/* Island                                                                   */
/* ======================================================================== */

typedef struct {
  int32_t *body_indices;
  int32_t  num_bodies;
  int32_t  sleeping;
} TydraPhysIsland;

/* ======================================================================== */
/* Simulation world                                                         */
/* ======================================================================== */

typedef struct {
  /* Bodies */
  TydraPhysBody     *bodies;
  int32_t            num_bodies;
  int32_t            max_bodies;

  /* Colliders */
  TydraPhysCollider *colliders;
  int32_t            num_colliders;
  int32_t            max_colliders;

  /* Joints */
  TydraPhysJoint    *joints;
  int32_t            num_joints;
  int32_t            max_joints;

  /* Contacts (regenerated each step) */
  TydraPhysContact  *contacts;
  int32_t            num_contacts;
  int32_t            max_contacts;

  /* Contact constraint cache (opaque, allocated by BuildPhysWorld or user).
   * Must be at least max_contacts * sizeof(internal constraint cache entry).
   * If NULL, a static global array is used (NOT thread-safe). */
  void              *contact_cache;

  /* Broadphase */
  TydraPhysBroadphase broadphase;

  /* Islands */
  TydraPhysIsland   *islands;
  int32_t            num_islands;
  int32_t            max_islands;
  int32_t           *island_union;   /* Union-find parent (one per body)   */
  int32_t           *island_body_buf;/* Scratch buffer for island bodies   */

  /* Simulation parameters */
  TydraPhysVec3 gravity;
  float         timestep;
  int32_t       solver_iterations;   /* Sequential impulse iterations      */
  float         baumgarte_bias;      /* Position correction (0.1-0.3)      */
  float         slop;                /* Allowed penetration (anti-jitter)  */
  float         sleep_threshold;     /* Energy threshold for sleep         */
  float         sleep_time;          /* Seconds below threshold to sleep   */
  int32_t       ccd_enabled;         /* Global CCD toggle                  */
  float         ccd_motion_threshold;/* Speed threshold for CCD            */
  int32_t       warm_start;          /* Enable warm starting               */
} TydraPhysWorld;

/* ======================================================================== */
/* API Functions                                                            */
/* ======================================================================== */

/* Initialize world with caller-provided buffers.
 * All buffer pointers must remain valid for the world's lifetime. */
void tydra_phys_world_init(
    TydraPhysWorld *world,
    TydraPhysBody *body_buf, int32_t max_bodies,
    TydraPhysCollider *col_buf, int32_t max_colliders,
    TydraPhysJoint *joint_buf, int32_t max_joints,
    TydraPhysContact *contact_buf, int32_t max_contacts,
    TydraPhysAABB *aabb_buf,
    TydraPhysCollisionPair *pair_buf, int32_t max_pairs,
    int32_t *sort_buf,
    TydraPhysIsland *island_buf, int32_t max_islands,
    int32_t *union_buf,
    int32_t *island_body_buf);

/* Set simulation parameters to sensible defaults. */
void tydra_phys_world_defaults(TydraPhysWorld *world);

/* Add a body. Returns body index, or -1 on overflow. */
int32_t tydra_phys_add_body(TydraPhysWorld *world, const TydraPhysBody *body);

/* Add a collider. Returns collider index, or -1 on overflow. */
int32_t tydra_phys_add_collider(TydraPhysWorld *world,
                                 const TydraPhysCollider *col);

/* Add a joint. Returns joint index, or -1 on overflow. */
int32_t tydra_phys_add_joint(TydraPhysWorld *world,
                              const TydraPhysJoint *joint);

/* Apply force at center of mass (world space). */
void tydra_phys_apply_force(TydraPhysBody *body, TydraPhysVec3 force);

/* Apply force at world-space point (generates torque). */
void tydra_phys_apply_force_at(TydraPhysBody *body,
                                TydraPhysVec3 force,
                                TydraPhysVec3 world_point);

/* Apply torque (world space). */
void tydra_phys_apply_torque(TydraPhysBody *body, TydraPhysVec3 torque);

/* Step the simulation. */
TydraPhysResult tydra_phys_step(TydraPhysWorld *world);

/* Wake a body and its island. */
void tydra_phys_wake_body(TydraPhysWorld *world, int32_t body_index);

/* Clear accumulated forces/torques on all bodies. */
void tydra_phys_clear_forces(TydraPhysWorld *world);

/* Initialize a body to default values (dynamic, gravity enabled, can sleep). */
void tydra_phys_body_default(TydraPhysBody *body);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LIGHTUSD_TYDRA_RB_DYNAMICS_H_ */
