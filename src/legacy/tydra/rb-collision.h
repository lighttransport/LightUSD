/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026-Present Light Transport Entertainment Inc. */
/*
 * rb-collision.h — Rigid-body collision detection (C API)
 *
 * Broadphase (sweep-and-prune AABB) and narrowphase (GJK/SAT) collision
 * pipeline for the TydraPhys rigid-body simulation.
 *
 * Supported shape pairs (narrowphase):
 *   sphere-sphere, sphere-box, sphere-capsule, sphere-plane,
 *   box-box, box-plane, capsule-capsule, capsule-plane,
 *   cylinder-plane.  Other combinations return 0 contacts.
 *
 * Usage:
 *   1. Build TydraPhysCollider instances from USD Physics shapes
 *   2. Call tydra_phys_broadphase_init() once with scratch buffers
 *   3. Each frame:
 *      a. tydra_phys_broadphase_update() to find candidate pairs
 *      b. For each pair call tydra_phys_narrow_phase() to generate contacts
 *      c. Feed contacts into the constraint solver (see rb-dynamics.h)
 *
 * CCD: Use tydra_phys_ccd_toi() for fast-moving bodies to find the
 *      time-of-impact before tunnelling occurs.
 *
 * Memory: The C API does not allocate. Caller provides all buffers.
 * See rb-collision.hh for C++ helpers that build colliders from Tydra data.
 */
#ifndef LIGHTUSD_TYDRA_RB_COLLISION_H_
#define LIGHTUSD_TYDRA_RB_COLLISION_H_

#include <stdint.h>

#include "rb-math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* Enums                                                                    */
/* ======================================================================== */

typedef enum {
  TYDRA_PHYS_SHAPE_SPHERE   = 0,  /* Sphere defined by radius             */
  TYDRA_PHYS_SHAPE_BOX      = 1,  /* Axis-aligned box (half-extents)      */
  TYDRA_PHYS_SHAPE_CAPSULE  = 2,  /* Capsule (Y-axis aligned)             */
  TYDRA_PHYS_SHAPE_PLANE    = 3,  /* Infinite half-space (normal+offset)  */
  TYDRA_PHYS_SHAPE_CYLINDER = 4   /* Cylinder (Y-axis aligned)            */
} TydraPhysShapeType;

typedef enum {
  TYDRA_PHYS_COL_OK             =  0,  /* Success                        */
  TYDRA_PHYS_COL_ERR_NULL_INPUT = -1,  /* A required pointer was NULL    */
  TYDRA_PHYS_COL_ERR_INTERNAL   = -2   /* Unexpected internal error      */
} TydraPhysColResult;

/* ======================================================================== */
/* Shape primitives                                                         */
/* ======================================================================== */

/* Sphere defined by a single radius around the local origin. */
typedef struct {
  float radius;                        /* Must be > 0                     */
} TydraPhysSphere;

/* Box defined by three half-extents (positive along each local axis). */
typedef struct {
  TydraPhysVec3 half_extents;         /* (hx, hy, hz)                    */
} TydraPhysBox;

/* Capsule aligned along the local Y-axis.
 * Total height = 2 * half_height + 2 * radius (hemisphere caps). */
typedef struct {
  float radius;                        /* Hemisphere / cylinder radius    */
  float half_height;                   /* Half the cylinder segment       */
} TydraPhysCapsule;

/* Infinite half-space: dot(normal, p) <= offset is interior.
 * The normal must be unit-length. */
typedef struct {
  TydraPhysVec3 normal;                /* Outward-facing unit normal      */
  float         offset;                /* Signed distance from origin     */
} TydraPhysPlane;

/* Cylinder aligned along the local Y-axis (flat caps, no hemispheres). */
typedef struct {
  float radius;                        /* Circular cross-section radius   */
  float half_height;                   /* Half the total height           */
} TydraPhysCylinder;

/* Tagged-union shape container.
 * Access the active member through data.<member> after checking type.
 * E.g.:  if (shape.type == TYDRA_PHYS_SHAPE_SPHERE)
 *            r = shape.data.sphere.radius;                             */
typedef struct {
  TydraPhysShapeType type;
  union {
    TydraPhysSphere   sphere;
    TydraPhysBox      box;
    TydraPhysCapsule  capsule;
    TydraPhysPlane    plane;
    TydraPhysCylinder cylinder;
  } data;
} TydraPhysShape;

/* ======================================================================== */
/* Collider                                                                 */
/* ======================================================================== */

/* A collider attaches a shape to a rigid body with material properties.
 * Multiple colliders may reference the same body_index (compound shapes). */
typedef struct {
  TydraPhysShape     shape;
  TydraPhysTransform local_pose;       /* Offset from body origin          */
  float              static_friction;
  float              dynamic_friction;
  float              restitution;
  uint32_t           collision_group;  /* Bitmask                          */
  uint32_t           collision_mask;   /* Which groups to collide with     */
  int32_t            body_index;       /* Index into world.bodies          */
} TydraPhysCollider;

/* ======================================================================== */
/* AABB (Axis-Aligned Bounding Box)                                         */
/* ======================================================================== */

/* World-space axis-aligned bounding box used by the broadphase.
 * Invariant: min.x <= max.x, min.y <= max.y, min.z <= max.z.           */
typedef struct {
  TydraPhysVec3 min;                   /* Lower corner                    */
  TydraPhysVec3 max;                   /* Upper corner                    */
} TydraPhysAABB;

/* ======================================================================== */
/* Broadphase                                                               */
/* ======================================================================== */

/* Index pair produced by broadphase, referencing collider indices. */
typedef struct {
  int32_t a;                           /* First collider index            */
  int32_t b;                           /* Second collider index           */
} TydraPhysCollisionPair;

/* Sweep-and-prune broadphase context.  All storage is caller-owned;
 * the struct merely aggregates pointers and bookkeeping state. */
typedef struct {
  TydraPhysAABB          *aabbs;       /* Per-collider world AABBs        */
  int32_t                 num_aabbs;   /* Current collider count          */
  TydraPhysCollisionPair *pairs;       /* Output pair buffer              */
  int32_t                 num_pairs;   /* Pairs written last update       */
  int32_t                 max_pairs;   /* Capacity of |pairs| buffer      */
  int32_t                 sort_axis;   /* 0=X, 1=Y, 2=Z (variance-based) */
  int32_t                *sorted_indices; /* Scratch for axis sort        */
} TydraPhysBroadphase;

/* ======================================================================== */
/* Contact                                                                  */
/* ======================================================================== */

/* A single contact point generated by narrowphase detection.
 * The solver accumulates impulses in the warm-start fields across frames
 * for stable stacking (persistent contact caching is the caller's
 * responsibility). */
typedef struct {
  TydraPhysVec3 point;                 /* World-space contact point        */
  TydraPhysVec3 normal;                /* B->A direction                   */
  float         depth;                 /* Penetration (positive = overlap) */
  int32_t       collider_a;
  int32_t       collider_b;
  int32_t       body_a;
  int32_t       body_b;
  float         combined_friction;
  float         combined_restitution;
  /* Solver warm-start cache */
  float         normal_impulse_accum;
  float         tangent_impulse_accum[2];
} TydraPhysContact;

/* ======================================================================== */
/* API Functions                                                            */
/* ======================================================================== */

/* Compute the world-space AABB of a collider given its body transform.
 * The resulting AABB accounts for both local_pose and body_xform.
 * All three pointers must be non-NULL. */
void tydra_phys_collider_aabb(const TydraPhysCollider *col,
                               const TydraPhysTransform *body_xform,
                               TydraPhysAABB *out);

/* Initialise a broadphase context with caller-provided buffers.
 * |aabb_buf|  must hold at least |max_colliders| entries.
 * |pair_buf|  must hold at least |max_pairs| entries.
 * |sort_buf|  must hold at least |max_colliders| int32_t indices. */
void tydra_phys_broadphase_init(TydraPhysBroadphase *bp,
                                 int32_t max_colliders,
                                 TydraPhysAABB *aabb_buf,
                                 TydraPhysCollisionPair *pair_buf,
                                 int32_t max_pairs,
                                 int32_t *sort_buf);

/* Recompute AABBs from current transforms and run sweep-and-prune.
 * Collision groups are checked: a pair (i,j) is emitted only when
 *   (colliders[i].collision_group & colliders[j].collision_mask) &&
 *   (colliders[j].collision_group & colliders[i].collision_mask).
 * Returns the number of overlapping pairs written to bp->pairs,
 * or a negative TydraPhysColResult error code. */
int32_t tydra_phys_broadphase_update(TydraPhysBroadphase *bp,
                                      const TydraPhysCollider *colliders,
                                      const TydraPhysTransform *body_xforms,
                                      int32_t num_colliders);

/* Generate contacts between two colliders using GJK/SAT narrowphase.
 * Returns the number of contacts written (0 if no overlap), capped at
 * |max_contacts|, or a negative TydraPhysColResult error code.
 * Combined friction and restitution are computed from both colliders'
 * material properties (geometric mean for friction, max for restitution). */
int32_t tydra_phys_narrow_phase(const TydraPhysCollider *col_a,
                                 const TydraPhysTransform *xform_a,
                                 const TydraPhysCollider *col_b,
                                 const TydraPhysTransform *xform_b,
                                 TydraPhysContact *contacts,
                                 int32_t max_contacts);

/* Continuous collision detection: compute the time of impact [0,1] for
 * |col| sweeping from |start| to |end| against a stationary |target|.
 * Returns 1.0 if no impact occurs within the sweep interval. */
float tydra_phys_ccd_toi(const TydraPhysCollider *col,
                          const TydraPhysTransform *start,
                          const TydraPhysTransform *end,
                          const TydraPhysCollider *target,
                          const TydraPhysTransform *target_xform);

/* Fast AABB-vs-AABB overlap test (separating-axis on three axes).
 * Returns 1 if the two AABBs overlap on all three axes, 0 otherwise.
 * Touching (edge/face contact) counts as overlapping. */
int32_t tydra_phys_aabb_overlap(const TydraPhysAABB *a,
                                 const TydraPhysAABB *b);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LIGHTUSD_TYDRA_RB_COLLISION_H_ */
