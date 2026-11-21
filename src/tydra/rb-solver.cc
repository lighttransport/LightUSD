/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026-Present Light Transport Entertainment Inc. */
/*
 * rb-dynamics.cc -- Rigid body dynamics implementation
 *
 * Sequential impulse constraint solver with semi-implicit Euler integration,
 * Coulomb friction, joint constraints, island-based sleeping, and CCD.
 */

#include "rb-dynamics.h"
#include <string.h>
#include <float.h>
#include <math.h>

#include <cstdint>
#include <memory>

/* ======================================================================== */
/* Contact constraint cache (file-local)                                    */
/* ======================================================================== */

#define TYDRA_MAX_CONTACT_CACHE 4096

#include "rb-constraints-internal.hh"

/* Fallback cache for single-world use. If world->contact_cache is set,
 * that per-world buffer is used instead (thread-safe). */
static ContactConstraint s_contact_cache_fallback[TYDRA_MAX_CONTACT_CACHE];

static ContactConstraint *get_contact_cache(TydraPhysWorld *world) {
  if (world->contact_cache) return static_cast<ContactConstraint *>(world->contact_cache);
  return s_contact_cache_fallback;
}

/* ======================================================================== */
/* PART 1: C Implementation                                                 */
/* ======================================================================== */

/* ---------------------------------------------------------------- */
/* 1. Utility functions                                             */
/* ---------------------------------------------------------------- */

void tydra_phys_world_defaults(TydraPhysWorld *world) {
  if (!world) return;
  world->gravity = tp_v3(0.0f, 0.0f, -9.81f);
  world->timestep = 1.0f / 60.0f;
  world->solver_iterations = 8;
  world->baumgarte_bias = 0.2f;
  world->slop = 0.005f;
  world->sleep_threshold = 0.01f;
  world->sleep_time = 0.5f;
  world->ccd_enabled = 0;
  world->ccd_motion_threshold = 2.0f;
  world->warm_start = 1;
}

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
    int32_t *island_body_buf) {
  if (!world) return;

  memset(world, 0, sizeof(TydraPhysWorld));

  world->bodies = body_buf;
  world->max_bodies = max_bodies;
  world->num_bodies = 0;

  world->colliders = col_buf;
  world->max_colliders = max_colliders;
  world->num_colliders = 0;

  world->joints = joint_buf;
  world->max_joints = max_joints;
  world->num_joints = 0;

  world->contacts = contact_buf;
  world->max_contacts = max_contacts;
  world->num_contacts = 0;

  /* Broadphase */
  world->broadphase.aabbs = aabb_buf;
  world->broadphase.num_aabbs = 0;
  world->broadphase.pairs = pair_buf;
  world->broadphase.num_pairs = 0;
  world->broadphase.max_pairs = max_pairs;
  world->broadphase.sort_axis = 0;
  world->broadphase.sorted_indices = sort_buf;

  /* Islands */
  world->islands = island_buf;
  world->max_islands = max_islands;
  world->num_islands = 0;
  world->island_union = union_buf;
  world->island_body_buf = island_body_buf;

  tydra_phys_world_defaults(world);
}

void tydra_phys_body_default(TydraPhysBody *body) {
  if (!body) return;
  memset(body, 0, sizeof(TydraPhysBody));
  body->xform = tp_xform_identity();
  body->linear_velocity = tp_v3(0, 0, 0);
  body->angular_velocity = tp_v3(0, 0, 0);
  body->inverse_mass = 1.0f;
  body->local_inv_inertia = tp_v3(1.0f, 1.0f, 1.0f);
  body->inv_inertia_world = tp_m3_identity();
  body->force_accum = tp_v3(0, 0, 0);
  body->torque_accum = tp_v3(0, 0, 0);
  body->body_type = TYDRA_PHYS_BODY_DYNAMIC;
  body->flags = TYDRA_PHYS_BODY_FLAG_GRAVITY | TYDRA_PHYS_BODY_FLAG_CAN_SLEEP;
  body->collider_start = -1;
  body->collider_count = 0;
  body->island_id = -1;
  body->sleep_timer = 0.0f;
  body->motion_energy = 0.0f;
}

static int body_index_is_valid(const TydraPhysWorld *world, int32_t idx) {
  return world && idx >= 0 && idx < world->num_bodies;
}

static int body_index_or_world_is_valid(const TydraPhysWorld *world,
                                        int32_t idx) {
  return idx < 0 || body_index_is_valid(world, idx);
}

int32_t tydra_phys_add_body(TydraPhysWorld *world, const TydraPhysBody *body) {
  if (!world || !body) return -1;
  if (world->num_bodies >= world->max_bodies) return -1;
  int32_t idx = world->num_bodies;
  world->bodies[idx] = *body;
  world->num_bodies++;
  return idx;
}

int32_t tydra_phys_add_collider(TydraPhysWorld *world,
                                 const TydraPhysCollider *col) {
  if (!world || !col) return -1;
  if (world->num_colliders >= world->max_colliders) return -1;
  if (!body_index_or_world_is_valid(world, col->body_index)) return -1;
  int32_t idx = world->num_colliders;
  world->colliders[idx] = *col;
  world->num_colliders++;
  return idx;
}

int32_t tydra_phys_add_joint(TydraPhysWorld *world,
                              const TydraPhysJoint *joint) {
  if (!world || !joint) return -1;
  if (world->num_joints >= world->max_joints) return -1;
  if (!body_index_or_world_is_valid(world, joint->body_a)) return -1;
  if (!body_index_or_world_is_valid(world, joint->body_b)) return -1;
  int32_t idx = world->num_joints;
  world->joints[idx] = *joint;
  world->num_joints++;
  return idx;
}

void tydra_phys_apply_force(TydraPhysBody *body, TydraPhysVec3 force) {
  if (!body) return;
  body->force_accum = tp_v3_add(body->force_accum, force);
}

void tydra_phys_apply_force_at(TydraPhysBody *body,
                                TydraPhysVec3 force,
                                TydraPhysVec3 world_point) {
  if (!body) return;
  body->force_accum = tp_v3_add(body->force_accum, force);
  TydraPhysVec3 r = tp_v3_sub(world_point, body->xform.position);
  body->torque_accum = tp_v3_add(body->torque_accum, tp_v3_cross(r, force));
}

void tydra_phys_apply_torque(TydraPhysBody *body, TydraPhysVec3 torque) {
  if (!body) return;
  body->torque_accum = tp_v3_add(body->torque_accum, torque);
}

void tydra_phys_clear_forces(TydraPhysWorld *world) {
  int32_t i;
  if (!world) return;
  for (i = 0; i < world->num_bodies; i++) {
    world->bodies[i].force_accum = tp_v3(0, 0, 0);
    world->bodies[i].torque_accum = tp_v3(0, 0, 0);
  }
}

/* ---------------------------------------------------------------- */
/* 2. World inertia update                                          */
/* ---------------------------------------------------------------- */

static void update_world_inertia(TydraPhysWorld *world) {
  int32_t i;
  for (i = 0; i < world->num_bodies; i++) {
    TydraPhysBody *b = &world->bodies[i];
    if (b->body_type != TYDRA_PHYS_BODY_DYNAMIC) continue;
    b->inv_inertia_world = tp_inertia_world(b->local_inv_inertia,
                                             b->xform.rotation);
  }
}

/* ---------------------------------------------------------------- */
/* 3. Semi-implicit Euler integrator                                */
/* ---------------------------------------------------------------- */

static int body_is_sleeping(const TydraPhysBody *b) {
  return (b->flags & TYDRA_PHYS_BODY_FLAG_SLEEPING) != 0;
}

static void integrate_velocities(TydraPhysWorld *world) {
  int32_t i;
  float dt = world->timestep;
  float linear_damping = 1.0f - 0.01f * dt;
  float angular_damping = 1.0f - 0.01f * dt;

  if (linear_damping < 0.0f) linear_damping = 0.0f;
  if (angular_damping < 0.0f) angular_damping = 0.0f;

  for (i = 0; i < world->num_bodies; i++) {
    TydraPhysBody *b = &world->bodies[i];
    if (b->body_type != TYDRA_PHYS_BODY_DYNAMIC) continue;
    if (body_is_sleeping(b)) continue;

    /* Linear velocity: v += (F * inv_mass + g) * dt */
    TydraPhysVec3 accel = tp_v3_scale(b->force_accum, b->inverse_mass);
    if (b->flags & TYDRA_PHYS_BODY_FLAG_GRAVITY) {
      accel = tp_v3_add(accel, world->gravity);
    }
    b->linear_velocity = tp_v3_add(b->linear_velocity,
                                    tp_v3_scale(accel, dt));
    /* Linear damping */
    b->linear_velocity = tp_v3_scale(b->linear_velocity, linear_damping);

    /* Angular velocity: omega += I_world^-1 * tau * dt */
    TydraPhysVec3 ang_accel = tp_m3_mul_vec(&b->inv_inertia_world,
                                             b->torque_accum);
    b->angular_velocity = tp_v3_add(b->angular_velocity,
                                     tp_v3_scale(ang_accel, dt));
    /* Angular damping */
    b->angular_velocity = tp_v3_scale(b->angular_velocity, angular_damping);
  }
}

static void integrate_positions(TydraPhysWorld *world) {
  int32_t i;
  float dt = world->timestep;

  for (i = 0; i < world->num_bodies; i++) {
    TydraPhysBody *b = &world->bodies[i];
    if (b->body_type != TYDRA_PHYS_BODY_DYNAMIC) continue;
    if (body_is_sleeping(b)) continue;

    /* Position: x += v * dt */
    b->xform.position = tp_v3_add(b->xform.position,
                                   tp_v3_scale(b->linear_velocity, dt));

    /* Quaternion integration:
     * dq = 0.5 * quat(omega, 0) * q * dt
     * q' = normalize(q + dq) */
    {
      TydraPhysVec3 w = b->angular_velocity;
      TydraPhysQuat omega_q = tp_q(w.x, w.y, w.z, 0.0f);
      TydraPhysQuat spin = tp_q_mul(omega_q, b->xform.rotation);
      float half_dt = 0.5f * dt;
      TydraPhysQuat q = b->xform.rotation;
      q.x += spin.x * half_dt;
      q.y += spin.y * half_dt;
      q.z += spin.z * half_dt;
      q.w += spin.w * half_dt;
      b->xform.rotation = tp_q_normalize(q);
    }
  }
}

/* ---------------------------------------------------------------- */
/* 4. Contact constraint solver                                     */
/* ---------------------------------------------------------------- */

/* Compute tangent basis from normal (Frisvad method) */
static void compute_tangent_basis(TydraPhysVec3 n,
                                  TydraPhysVec3 *t1,
                                  TydraPhysVec3 *t2) {
  if (n.z < -0.9999999f) {
    *t1 = tp_v3(0.0f, -1.0f, 0.0f);
    *t2 = tp_v3(-1.0f, 0.0f, 0.0f);
    return;
  }
  float a = 1.0f / (1.0f + n.z);
  float b = -n.x * n.y * a;
  *t1 = tp_v3(1.0f - n.x * n.x * a, b, -n.x);
  *t2 = tp_v3(b, 1.0f - n.y * n.y * a, -n.y);
}

/* Compute effective mass for a constraint direction */
static float compute_effective_mass(
    const TydraPhysBody *ba, const TydraPhysBody *bb,
    TydraPhysVec3 r_a, TydraPhysVec3 r_b,
    TydraPhysVec3 dir) {
  float inv_mass_sum = 0.0f;

  if (ba->body_type == TYDRA_PHYS_BODY_DYNAMIC) {
    inv_mass_sum += ba->inverse_mass;
    TydraPhysVec3 rxn = tp_v3_cross(r_a, dir);
    TydraPhysVec3 iw_rxn = tp_m3_mul_vec(&ba->inv_inertia_world, rxn);
    inv_mass_sum += tp_v3_dot(iw_rxn, rxn);
  }

  if (bb->body_type == TYDRA_PHYS_BODY_DYNAMIC) {
    inv_mass_sum += bb->inverse_mass;
    TydraPhysVec3 rxn = tp_v3_cross(r_b, dir);
    TydraPhysVec3 iw_rxn = tp_m3_mul_vec(&bb->inv_inertia_world, rxn);
    inv_mass_sum += tp_v3_dot(iw_rxn, rxn);
  }

  return (inv_mass_sum > TP_EPSILON) ? (1.0f / inv_mass_sum) : 0.0f;
}

/* Apply impulse to a body */
static void apply_impulse(TydraPhysBody *body,
                          TydraPhysVec3 impulse,
                          TydraPhysVec3 r) {
  if (body->body_type != TYDRA_PHYS_BODY_DYNAMIC) return;
  body->linear_velocity = tp_v3_add(body->linear_velocity,
                                     tp_v3_scale(impulse, body->inverse_mass));
  TydraPhysVec3 ang_impulse = tp_m3_mul_vec(&body->inv_inertia_world,
                                              tp_v3_cross(r, impulse));
  body->angular_velocity = tp_v3_add(body->angular_velocity, ang_impulse);
}

/* Compute relative velocity at contact point */
static TydraPhysVec3 relative_velocity(const TydraPhysBody *ba,
                                        const TydraPhysBody *bb,
                                        TydraPhysVec3 r_a,
                                        TydraPhysVec3 r_b) {
  TydraPhysVec3 va = tp_v3_add(ba->linear_velocity,
                                tp_v3_cross(ba->angular_velocity, r_a));
  TydraPhysVec3 vb = tp_v3_add(bb->linear_velocity,
                                tp_v3_cross(bb->angular_velocity, r_b));
  return tp_v3_sub(va, vb);
}

static void prepare_contacts(TydraPhysWorld *world) {
  int32_t i;
  float dt = world->timestep;
  float inv_dt = (dt > TP_EPSILON) ? (1.0f / dt) : 0.0f;
  int32_t num = world->num_contacts;

  int32_t cache_cap = world->contact_cache ? world->max_contacts
                                           : TYDRA_MAX_CONTACT_CACHE;
  if (num > cache_cap) num = cache_cap;

  for (i = 0; i < num; i++) {
    TydraPhysContact *c = &world->contacts[i];
    ContactConstraint *cc = &get_contact_cache(world)[i];
    if (!body_index_is_valid(world, c->body_a) ||
        !body_index_is_valid(world, c->body_b)) {
      memset(cc, 0, sizeof(ContactConstraint));
      continue;
    }
    TydraPhysBody *ba = &world->bodies[c->body_a];
    TydraPhysBody *bb = &world->bodies[c->body_b];

    cc->r_a = tp_v3_sub(c->point, ba->xform.position);
    cc->r_b = tp_v3_sub(c->point, bb->xform.position);

    /* Normal effective mass */
    cc->effective_mass_n = compute_effective_mass(ba, bb,
                                                  cc->r_a, cc->r_b, c->normal);

    /* Baumgarte velocity bias: drives velocity to push bodies apart.
     * bias is negative so lambda = -(vrel_n + bias) = -(vrel_n - |bias|)
     * gives positive lambda when vrel_n < |bias| (approaching). */
    float penetration = c->depth - world->slop;
    cc->bias = 0.0f;
    if (penetration > 0.0f) {
      cc->bias = -(world->baumgarte_bias * inv_dt * penetration);
    }

    /* Add restitution bias (target bounce velocity) */
    {
      TydraPhysVec3 vrel = relative_velocity(ba, bb, cc->r_a, cc->r_b);
      float vrel_n = tp_v3_dot(vrel, c->normal);
      if (vrel_n < -1.0f) {
        cc->bias -= c->combined_restitution * vrel_n; /* makes bias more negative */
      }
    }

    /* Tangent basis */
    compute_tangent_basis(c->normal, &cc->tangent[0], &cc->tangent[1]);

    /* Tangent effective masses */
    cc->effective_mass_t[0] = compute_effective_mass(ba, bb,
                                                     cc->r_a, cc->r_b,
                                                     cc->tangent[0]);
    cc->effective_mass_t[1] = compute_effective_mass(ba, bb,
                                                     cc->r_a, cc->r_b,
                                                     cc->tangent[1]);

    /* Warm start */
    if (world->warm_start) {
      TydraPhysVec3 P = tp_v3_scale(c->normal, c->normal_impulse_accum);
      P = tp_v3_add(P, tp_v3_scale(cc->tangent[0],
                                    c->tangent_impulse_accum[0]));
      P = tp_v3_add(P, tp_v3_scale(cc->tangent[1],
                                    c->tangent_impulse_accum[1]));
      apply_impulse(ba, P, cc->r_a);
      apply_impulse(bb, tp_v3_negate(P), cc->r_b);
    } else {
      c->normal_impulse_accum = 0.0f;
      c->tangent_impulse_accum[0] = 0.0f;
      c->tangent_impulse_accum[1] = 0.0f;
    }
  }
}

static void solve_contacts(TydraPhysWorld *world) {
  int32_t i;
  int32_t num = world->num_contacts;
  int32_t cache_cap2 = world->contact_cache ? world->max_contacts
                                            : TYDRA_MAX_CONTACT_CACHE;
  if (num > cache_cap2) num = cache_cap2;

  for (i = 0; i < num; i++) {
    TydraPhysContact *c = &world->contacts[i];
    ContactConstraint *cc = &get_contact_cache(world)[i];
    if (!body_index_is_valid(world, c->body_a) ||
        !body_index_is_valid(world, c->body_b)) {
      continue;
    }
    TydraPhysBody *ba = &world->bodies[c->body_a];
    TydraPhysBody *bb = &world->bodies[c->body_b];

    TydraPhysVec3 vrel = relative_velocity(ba, bb, cc->r_a, cc->r_b);

    /* Normal constraint */
    {
      float vrel_n = tp_v3_dot(vrel, c->normal);
      float lambda = -(vrel_n + cc->bias) * cc->effective_mass_n;
      float old_accum = c->normal_impulse_accum;
      float new_accum = old_accum + lambda;
      if (new_accum < 0.0f) new_accum = 0.0f;
      float applied_lambda = new_accum - old_accum;
      c->normal_impulse_accum = new_accum;

      TydraPhysVec3 impulse = tp_v3_scale(c->normal, applied_lambda);
      apply_impulse(ba, impulse, cc->r_a);
      apply_impulse(bb, tp_v3_negate(impulse), cc->r_b);
    }

    /* Re-read velocity after normal impulse */
    vrel = relative_velocity(ba, bb, cc->r_a, cc->r_b);

    /* Friction constraints (2 tangent directions) */
    {
      float max_friction = c->combined_friction * c->normal_impulse_accum;
      int t;
      for (t = 0; t < 2; t++) {
        float vrel_t = tp_v3_dot(vrel, cc->tangent[t]);
        float lambda_t = -vrel_t * cc->effective_mass_t[t];
        float old_accum = c->tangent_impulse_accum[t];
        float new_accum = old_accum + lambda_t;
        /* Coulomb cone clamping */
        if (new_accum > max_friction) new_accum = max_friction;
        if (new_accum < -max_friction) new_accum = -max_friction;
        lambda_t = new_accum - old_accum;
        c->tangent_impulse_accum[t] = new_accum;

        TydraPhysVec3 impulse = tp_v3_scale(cc->tangent[t], lambda_t);
        apply_impulse(ba, impulse, cc->r_a);
        apply_impulse(bb, tp_v3_negate(impulse), cc->r_b);
      }
    }
  }
}

/* ---------------------------------------------------------------- */
/* 5. Joint constraint solver                                       */
/* ---------------------------------------------------------------- */

/* Helper: get world anchor from body + local anchor */
static TydraPhysVec3 joint_world_anchor(const TydraPhysBody *body,
                                         TydraPhysTransform local_anchor) {
  return tp_xform_point(body->xform, local_anchor.position);
}

/* Helper: get world axis from body rotation + local axis */
static TydraPhysVec3 joint_world_axis(const TydraPhysBody *body,
                                       TydraPhysVec3 local_axis) {
  return tp_q_rotate(body->xform.rotation, local_axis);
}

/* Apply positional constraint along one axis */
static void solve_position_constraint(
    TydraPhysBody *ba, TydraPhysBody *bb,
    TydraPhysVec3 r_a, TydraPhysVec3 r_b,
    TydraPhysVec3 axis, float error,
    float *lambda_accum) {

  float eff_mass = compute_effective_mass(ba, bb, r_a, r_b, axis);
  if (eff_mass < TP_EPSILON) return;

  /* Compute constraint velocity */
  TydraPhysVec3 vrel = relative_velocity(ba, bb, r_a, r_b);
  float vn = tp_v3_dot(vrel, axis);

  /* Baumgarte position correction (use a moderate factor) */
  float bias = 0.1f * error;

  float lambda = -(vn + bias) * eff_mass;
  *lambda_accum += lambda;

  TydraPhysVec3 impulse = tp_v3_scale(axis, lambda);
  apply_impulse(ba, impulse, r_a);
  apply_impulse(bb, tp_v3_negate(impulse), r_b);
}

/* Apply angular constraint along one axis */
static void solve_angular_constraint(
    TydraPhysBody *ba, TydraPhysBody *bb,
    TydraPhysVec3 axis, float error,
    float *lambda_accum) {

  /* Effective mass for pure angular constraint */
  float inv_mass = 0.0f;
  if (ba->body_type == TYDRA_PHYS_BODY_DYNAMIC) {
    TydraPhysVec3 iw = tp_m3_mul_vec(&ba->inv_inertia_world, axis);
    inv_mass += tp_v3_dot(iw, axis);
  }
  if (bb->body_type == TYDRA_PHYS_BODY_DYNAMIC) {
    TydraPhysVec3 iw = tp_m3_mul_vec(&bb->inv_inertia_world, axis);
    inv_mass += tp_v3_dot(iw, axis);
  }
  if (inv_mass < TP_EPSILON) return;
  float eff_mass = 1.0f / inv_mass;

  /* Angular velocity difference along axis */
  float w_rel = tp_v3_dot(tp_v3_sub(ba->angular_velocity,
                                     bb->angular_velocity), axis);

  float bias = 0.1f * error;
  float lambda = -(w_rel + bias) * eff_mass;
  *lambda_accum += lambda;

  TydraPhysVec3 impulse = tp_v3_scale(axis, lambda);
  if (ba->body_type == TYDRA_PHYS_BODY_DYNAMIC) {
    TydraPhysVec3 dw = tp_m3_mul_vec(&ba->inv_inertia_world, impulse);
    ba->angular_velocity = tp_v3_add(ba->angular_velocity, dw);
  }
  if (bb->body_type == TYDRA_PHYS_BODY_DYNAMIC) {
    TydraPhysVec3 dw = tp_m3_mul_vec(&bb->inv_inertia_world, impulse);
    bb->angular_velocity = tp_v3_sub(bb->angular_velocity, dw);
  }
}

/* Resolve joint body pointers. If body index < 0, use a static dummy. */
static void resolve_joint_bodies(TydraPhysWorld *world, TydraPhysJoint *j,
                                  TydraPhysBody *dummy,
                                  TydraPhysBody **ba, TydraPhysBody **bb) {
  tydra_phys_body_default(dummy);
  dummy->body_type = TYDRA_PHYS_BODY_STATIC;
  dummy->inverse_mass = 0.0f;
  dummy->local_inv_inertia = tp_v3(0, 0, 0);
  dummy->inv_inertia_world = tp_m3_identity();
  memset(dummy->inv_inertia_world.m, 0, sizeof(dummy->inv_inertia_world.m));
  *ba = body_index_is_valid(world, j->body_a) ? &world->bodies[j->body_a]
                                              : dummy;
  *bb = body_index_is_valid(world, j->body_b) ? &world->bodies[j->body_b]
                                              : dummy;
}

/* Solve a single ball joint (3 positional constraints) */
static void solve_ball_joint(TydraPhysWorld *world, TydraPhysJoint *j) {
  TydraPhysBody dummy, *ba, *bb;
  resolve_joint_bodies(world, j, &dummy, &ba, &bb);

  if (j->body_a < 0) {
    dummy.xform = tp_xform_identity();
  }

  TydraPhysVec3 wa = joint_world_anchor(ba, j->local_anchor_a);
  TydraPhysVec3 wb = joint_world_anchor(bb, j->local_anchor_b);
  TydraPhysVec3 err = tp_v3_sub(wa, wb);

  TydraPhysVec3 r_a = tp_v3_sub(wa, ba->xform.position);
  TydraPhysVec3 r_b = tp_v3_sub(wb, bb->xform.position);

  /* Solve along X, Y, Z */
  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(1, 0, 0), err.x,
                            &j->lambda_accum[0]);
  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(0, 1, 0), err.y,
                            &j->lambda_accum[1]);
  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(0, 0, 1), err.z,
                            &j->lambda_accum[2]);
}

/* Solve a hinge joint (3 positional + 2 angular + optional limit) */
static void solve_hinge_joint(TydraPhysWorld *world, TydraPhysJoint *j) {
  TydraPhysBody dummy, *ba, *bb;
  resolve_joint_bodies(world, j, &dummy, &ba, &bb);

  if (j->body_a < 0) dummy.xform = tp_xform_identity();

  /* Position constraints (same as ball joint) */
  TydraPhysVec3 wa = joint_world_anchor(ba, j->local_anchor_a);
  TydraPhysVec3 wb = joint_world_anchor(bb, j->local_anchor_b);
  TydraPhysVec3 pos_err = tp_v3_sub(wa, wb);

  TydraPhysVec3 r_a = tp_v3_sub(wa, ba->xform.position);
  TydraPhysVec3 r_b = tp_v3_sub(wb, bb->xform.position);

  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(1, 0, 0), pos_err.x,
                            &j->lambda_accum[0]);
  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(0, 1, 0), pos_err.y,
                            &j->lambda_accum[1]);
  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(0, 0, 1), pos_err.z,
                            &j->lambda_accum[2]);

  /* Angular constraints: 2 axes perpendicular to hinge axis must be aligned */
  TydraPhysVec3 hinge_axis_a = joint_world_axis(ba, j->axis);
  TydraPhysVec3 hinge_axis_b = joint_world_axis(bb, j->axis);

  /* Compute two perpendicular axes to hinge_axis_a */
  TydraPhysVec3 perp1, perp2;
  compute_tangent_basis(hinge_axis_a, &perp1, &perp2);

  /* Angular error: hinge_axis_b should align with hinge_axis_a */
  float ang_err1 = tp_v3_dot(hinge_axis_b, perp1);
  float ang_err2 = tp_v3_dot(hinge_axis_b, perp2);

  solve_angular_constraint(ba, bb, perp1, ang_err1, &j->lambda_accum[3]);
  solve_angular_constraint(ba, bb, perp2, ang_err2, &j->lambda_accum[4]);

  /* Hinge angle limit */
  if (j->limit_enabled) {
    /* Compute hinge angle: project body_b's local X onto body_a's
     * plane perpendicular to the hinge axis */
    TydraPhysVec3 ref_a = tp_q_rotate(ba->xform.rotation, tp_v3(1, 0, 0));
    TydraPhysVec3 ref_b = tp_q_rotate(bb->xform.rotation, tp_v3(1, 0, 0));

    /* Project onto plane perpendicular to hinge_axis_a */
    ref_a = tp_v3_sub(ref_a, tp_v3_scale(hinge_axis_a,
                                          tp_v3_dot(ref_a, hinge_axis_a)));
    ref_b = tp_v3_sub(ref_b, tp_v3_scale(hinge_axis_a,
                                          tp_v3_dot(ref_b, hinge_axis_a)));

    float len_a = tp_v3_length(ref_a);
    float len_b = tp_v3_length(ref_b);
    if (len_a > TP_EPSILON && len_b > TP_EPSILON) {
      ref_a = tp_v3_scale(ref_a, 1.0f / len_a);
      ref_b = tp_v3_scale(ref_b, 1.0f / len_b);

      float cos_angle = tp_clampf(tp_v3_dot(ref_a, ref_b), -1.0f, 1.0f);
      float sin_angle = tp_v3_dot(tp_v3_cross(ref_a, ref_b), hinge_axis_a);
      float angle = atan2f(sin_angle, cos_angle);

      if (angle < j->lower_limit) {
        float err = angle - j->lower_limit;
        solve_angular_constraint(ba, bb, hinge_axis_a, err,
                                 &j->lambda_accum[5]);
      } else if (angle > j->upper_limit) {
        float err = angle - j->upper_limit;
        solve_angular_constraint(ba, bb, hinge_axis_a, err,
                                 &j->lambda_accum[5]);
      }
    }
  }
}

/* Solve a slider/prismatic joint (2 positional + 3 angular + optional limit) */
static void solve_slider_joint(TydraPhysWorld *world, TydraPhysJoint *j) {
  TydraPhysBody dummy, *ba, *bb;
  resolve_joint_bodies(world, j, &dummy, &ba, &bb);

  if (j->body_a < 0) dummy.xform = tp_xform_identity();

  TydraPhysVec3 slide_axis = joint_world_axis(ba, j->axis);

  /* Position constraints perpendicular to slide axis */
  TydraPhysVec3 wa = joint_world_anchor(ba, j->local_anchor_a);
  TydraPhysVec3 wb = joint_world_anchor(bb, j->local_anchor_b);
  TydraPhysVec3 diff = tp_v3_sub(wb, wa);

  TydraPhysVec3 r_a = tp_v3_sub(wa, ba->xform.position);
  TydraPhysVec3 r_b = tp_v3_sub(wb, bb->xform.position);

  TydraPhysVec3 perp1, perp2;
  compute_tangent_basis(slide_axis, &perp1, &perp2);

  float err1 = tp_v3_dot(diff, perp1);
  float err2 = tp_v3_dot(diff, perp2);

  solve_position_constraint(ba, bb, r_a, r_b, perp1, -err1,
                            &j->lambda_accum[0]);
  solve_position_constraint(ba, bb, r_a, r_b, perp2, -err2,
                            &j->lambda_accum[1]);

  /* 3 angular constraints: body frames must be fully aligned */
  TydraPhysQuat q_rel = tp_q_mul(tp_q_conjugate(ba->xform.rotation),
                                  bb->xform.rotation);
  /* Angular error from relative quaternion */
  TydraPhysVec3 ang_err = tp_v3(q_rel.x * 2.0f, q_rel.y * 2.0f,
                                q_rel.z * 2.0f);

  TydraPhysVec3 world_x = tp_q_rotate(ba->xform.rotation, tp_v3(1, 0, 0));
  TydraPhysVec3 world_y = tp_q_rotate(ba->xform.rotation, tp_v3(0, 1, 0));
  TydraPhysVec3 world_z = tp_q_rotate(ba->xform.rotation, tp_v3(0, 0, 1));

  solve_angular_constraint(ba, bb, world_x, ang_err.x, &j->lambda_accum[2]);
  solve_angular_constraint(ba, bb, world_y, ang_err.y, &j->lambda_accum[3]);
  solve_angular_constraint(ba, bb, world_z, ang_err.z, &j->lambda_accum[4]);

  /* Translation limit along slide axis */
  if (j->limit_enabled) {
    float slide_dist = tp_v3_dot(diff, slide_axis);

    if (slide_dist < j->lower_limit) {
      float err = slide_dist - j->lower_limit;
      solve_position_constraint(ba, bb, r_a, r_b, slide_axis, err,
                                &j->lambda_accum[5]);
    } else if (slide_dist > j->upper_limit) {
      float err = slide_dist - j->upper_limit;
      solve_position_constraint(ba, bb, r_a, r_b, slide_axis, err,
                                &j->lambda_accum[5]);
    }
  }
}

/* Solve a fixed joint (3 positional + 3 angular) */
static void solve_fixed_joint(TydraPhysWorld *world, TydraPhysJoint *j) {
  TydraPhysBody dummy, *ba, *bb;
  resolve_joint_bodies(world, j, &dummy, &ba, &bb);

  if (j->body_a < 0) dummy.xform = tp_xform_identity();

  /* Position constraints */
  TydraPhysVec3 wa = joint_world_anchor(ba, j->local_anchor_a);
  TydraPhysVec3 wb = joint_world_anchor(bb, j->local_anchor_b);
  TydraPhysVec3 pos_err = tp_v3_sub(wa, wb);

  TydraPhysVec3 r_a = tp_v3_sub(wa, ba->xform.position);
  TydraPhysVec3 r_b = tp_v3_sub(wb, bb->xform.position);

  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(1, 0, 0), pos_err.x,
                            &j->lambda_accum[0]);
  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(0, 1, 0), pos_err.y,
                            &j->lambda_accum[1]);
  solve_position_constraint(ba, bb, r_a, r_b, tp_v3(0, 0, 1), pos_err.z,
                            &j->lambda_accum[2]);

  /* Angular constraints */
  TydraPhysQuat q_rel = tp_q_mul(tp_q_conjugate(ba->xform.rotation),
                                  bb->xform.rotation);
  TydraPhysVec3 ang_err = tp_v3(q_rel.x * 2.0f, q_rel.y * 2.0f,
                                q_rel.z * 2.0f);

  TydraPhysVec3 world_x = tp_q_rotate(ba->xform.rotation, tp_v3(1, 0, 0));
  TydraPhysVec3 world_y = tp_q_rotate(ba->xform.rotation, tp_v3(0, 1, 0));
  TydraPhysVec3 world_z = tp_q_rotate(ba->xform.rotation, tp_v3(0, 0, 1));

  solve_angular_constraint(ba, bb, world_x, ang_err.x, &j->lambda_accum[3]);
  solve_angular_constraint(ba, bb, world_y, ang_err.y, &j->lambda_accum[4]);
  solve_angular_constraint(ba, bb, world_z, ang_err.z, &j->lambda_accum[5]);
}

/* Solve a distance joint (1 constraint) */
static void solve_distance_joint(TydraPhysWorld *world, TydraPhysJoint *j) {
  TydraPhysBody dummy, *ba, *bb;
  resolve_joint_bodies(world, j, &dummy, &ba, &bb);

  if (j->body_a < 0) dummy.xform = tp_xform_identity();

  TydraPhysVec3 wa = joint_world_anchor(ba, j->local_anchor_a);
  TydraPhysVec3 wb = joint_world_anchor(bb, j->local_anchor_b);
  TydraPhysVec3 diff = tp_v3_sub(wa, wb);
  float dist = tp_v3_length(diff);

  if (dist < TP_EPSILON) return;

  TydraPhysVec3 axis = tp_v3_scale(diff, 1.0f / dist);
  float error = 0.0f;

  if (dist < j->min_distance) {
    error = dist - j->min_distance;
  } else if (dist > j->max_distance) {
    error = dist - j->max_distance;
  } else {
    return; /* Within valid range */
  }

  TydraPhysVec3 r_a = tp_v3_sub(wa, ba->xform.position);
  TydraPhysVec3 r_b = tp_v3_sub(wb, bb->xform.position);

  solve_position_constraint(ba, bb, r_a, r_b, axis, error,
                            &j->lambda_accum[0]);
}

static void prepare_joints(TydraPhysWorld *world) {
  /* Zero accumulated impulses if not warm starting */
  int32_t i;
  if (!world->warm_start) {
    for (i = 0; i < world->num_joints; i++) {
      memset(world->joints[i].lambda_accum, 0,
             sizeof(world->joints[i].lambda_accum));
    }
  }
}

static void solve_joints(TydraPhysWorld *world) {
  int32_t i;
  for (i = 0; i < world->num_joints; i++) {
    TydraPhysJoint *j = &world->joints[i];
    switch (j->type) {
      case TYDRA_PHYS_JOINT_BALL:
        solve_ball_joint(world, j);
        break;
      case TYDRA_PHYS_JOINT_HINGE:
        solve_hinge_joint(world, j);
        break;
      case TYDRA_PHYS_JOINT_SLIDER:
        solve_slider_joint(world, j);
        break;
      case TYDRA_PHYS_JOINT_FIXED:
        solve_fixed_joint(world, j);
        break;
      case TYDRA_PHYS_JOINT_DISTANCE:
        solve_distance_joint(world, j);
        break;
    }
  }
}

/* ---------------------------------------------------------------- */
/* 6. Island detection (union-find)                                 */
/* ---------------------------------------------------------------- */

static int32_t uf_find(int32_t *parent, int32_t i) {
  while (parent[i] != i) {
    parent[i] = parent[parent[i]]; /* Path compression (halving) */
    i = parent[i];
  }
  return i;
}

static void uf_union(int32_t *parent, int32_t a, int32_t b) {
  int32_t ra = uf_find(parent, a);
  int32_t rb = uf_find(parent, b);
  if (ra != rb) {
    /* Union by index (simple) */
    if (ra < rb) {
      parent[rb] = ra;
    } else {
      parent[ra] = rb;
    }
  }
}

static void detect_islands(TydraPhysWorld *world) {
  int32_t i;
  int32_t num_bodies = world->num_bodies;

  if (!world->island_union || !world->islands || !world->island_body_buf) {
    return;
  }

  /* Initialize union-find */
  for (i = 0; i < num_bodies; i++) {
    world->island_union[i] = i;
    world->bodies[i].island_id = -1;
  }

  /* Union bodies connected by contacts */
  for (i = 0; i < world->num_contacts; i++) {
    int32_t a = world->contacts[i].body_a;
    int32_t b = world->contacts[i].body_b;
    if (a < 0 || b < 0) continue;
    if (!body_index_is_valid(world, a) || !body_index_is_valid(world, b)) continue;
    if (world->bodies[a].body_type == TYDRA_PHYS_BODY_STATIC) continue;
    if (world->bodies[b].body_type == TYDRA_PHYS_BODY_STATIC) continue;
    uf_union(world->island_union, a, b);
  }

  /* Union bodies connected by joints */
  for (i = 0; i < world->num_joints; i++) {
    int32_t a = world->joints[i].body_a;
    int32_t b = world->joints[i].body_b;
    if (a < 0 || b < 0) continue;
    if (!body_index_is_valid(world, a) || !body_index_is_valid(world, b)) continue;
    if (world->bodies[a].body_type == TYDRA_PHYS_BODY_STATIC) continue;
    if (world->bodies[b].body_type == TYDRA_PHYS_BODY_STATIC) continue;
    uf_union(world->island_union, a, b);
  }

  /* Find roots (with full path compression) */
  for (i = 0; i < num_bodies; i++) {
    uf_find(world->island_union, i);
  }

  /* Group bodies by root -> islands */
  world->num_islands = 0;

  /* Scratch first half maps roots to islands; the second half stores
   * body indices. Keep per-island counts in the island records: reusing the
   * root map as counts corrupts offsets for unequal or capped islands. */
  int32_t island_count = 0;
  int32_t *root_to_island = world->island_body_buf; /* size = max_bodies */
  for (i = 0; i < num_bodies; i++) root_to_island[i] = -1;

  for (i = 0; i < num_bodies && island_count < world->max_islands; i++) {
    if (world->bodies[i].body_type == TYDRA_PHYS_BODY_STATIC) continue;
    int32_t root = world->island_union[i];
    if (root >= 0 && root < num_bodies && root_to_island[root] < 0) {
      root_to_island[root] = island_count;
      world->islands[island_count].num_bodies = 0;
      world->islands[island_count].sleeping = 0;
      island_count++;
    }
  }
  world->num_islands = island_count;

  /* Build body lists: use island_body_buf after the root_to_island area.
   * Layout: [root_to_island: num_bodies] [body lists: remaining] */
  int32_t *body_buf = world->island_body_buf + num_bodies;

  for (i = 0; i < num_bodies; ++i) {
    if (world->bodies[i].body_type == TYDRA_PHYS_BODY_STATIC) continue;
    const int32_t root = world->island_union[i];
    const int32_t isl = (root >= 0 && root < num_bodies) ? root_to_island[root] : -1;
    if (isl >= 0 && isl < island_count) ++world->islands[isl].num_bodies;
  }

  /* Compute per-island offsets into body_buf */
  {
    int32_t offset = 0;
    for (i = 0; i < island_count && i < world->max_islands; i++) {
      world->islands[i].body_indices = &body_buf[offset];
      offset += world->islands[i].num_bodies;
    }
  }

  /* Reset counts for fill pass */
  for (i = 0; i < island_count; i++) {
    world->islands[i].num_bodies = 0;
  }

  /* Fill island body lists */
  for (i = 0; i < num_bodies; i++) {
    if (world->bodies[i].body_type == TYDRA_PHYS_BODY_STATIC) continue;
    int32_t root = world->island_union[i];
    int32_t isl = (root >= 0 && root < num_bodies) ? root_to_island[root] : -1;
    if (isl >= 0 && isl < island_count) {
      int32_t idx = world->islands[isl].num_bodies;
      world->islands[isl].body_indices[idx] = i;
      world->islands[isl].num_bodies++;
      world->bodies[i].island_id = isl;
    }
  }
}

/* ---------------------------------------------------------------- */
/* 7. Sleep detection                                               */
/* ---------------------------------------------------------------- */

static void update_sleep(TydraPhysWorld *world) {
  int32_t i, j;
  float dt = world->timestep;

  /* Update per-body motion energy */
  for (i = 0; i < world->num_bodies; i++) {
    TydraPhysBody *b = &world->bodies[i];
    if (b->body_type != TYDRA_PHYS_BODY_DYNAMIC) continue;
    if (!(b->flags & TYDRA_PHYS_BODY_FLAG_CAN_SLEEP)) continue;

    /* Kinetic energy approximation (unnormalized) */
    float lin_ke = tp_v3_length_sq(b->linear_velocity);
    float ang_ke = tp_v3_length_sq(b->angular_velocity);
    float ke = 0.5f * (lin_ke + ang_ke);

    /* Exponential smoothing */
    b->motion_energy = 0.8f * b->motion_energy + 0.2f * ke;

    if (b->motion_energy < world->sleep_threshold) {
      b->sleep_timer += dt;
    } else {
      b->sleep_timer = 0.0f;
      /* Wake body if it was sleeping */
      b->flags &= ~static_cast<uint32_t>(TYDRA_PHYS_BODY_FLAG_SLEEPING);
    }
  }

  /* Per-island sleep check */
  for (i = 0; i < world->num_islands; i++) {
    TydraPhysIsland *island = &world->islands[i];
    int all_sleepy = 1;

    for (j = 0; j < island->num_bodies; j++) {
      TydraPhysBody *b = &world->bodies[island->body_indices[j]];
      if (!(b->flags & TYDRA_PHYS_BODY_FLAG_CAN_SLEEP)) {
        all_sleepy = 0;
        break;
      }
      if (b->sleep_timer < world->sleep_time) {
        all_sleepy = 0;
        break;
      }
    }

    if (all_sleepy && island->num_bodies > 0) {
      island->sleeping = 1;
      for (j = 0; j < island->num_bodies; j++) {
        TydraPhysBody *b = &world->bodies[island->body_indices[j]];
        b->flags |= TYDRA_PHYS_BODY_FLAG_SLEEPING;
        b->linear_velocity = tp_v3(0, 0, 0);
        b->angular_velocity = tp_v3(0, 0, 0);
      }
    } else {
      island->sleeping = 0;
    }
  }
}

/* ---------------------------------------------------------------- */
/* 8. Step orchestration                                            */
/* ---------------------------------------------------------------- */

TydraPhysResult tydra_phys_step(TydraPhysWorld *world) {
  int32_t i, iter;

  if (!world) return TYDRA_PHYS_ERR_NULL_INPUT;

  /* 1. Update world-space inertia tensors */
  update_world_inertia(world);

  /* 2. Broadphase: compute AABBs and find candidate pairs */
  world->broadphase.num_pairs = 0;
  world->broadphase.num_aabbs = world->num_colliders;
  for (i = 0; i < world->num_colliders; i++) {
    TydraPhysCollider *col = &world->colliders[i];
    TydraPhysTransform bx;
    if (col->body_index >= 0 && col->body_index < world->num_bodies) {
      bx = world->bodies[col->body_index].xform;
    } else {
      bx = tp_xform_identity();
    }
    tydra_phys_collider_aabb(col, &bx, &world->broadphase.aabbs[i]);
  }

  tydra_phys_broadphase_update(&world->broadphase,
                                world->colliders, nullptr,
                                world->num_colliders);

  /* 3. Narrowphase: generate contacts */
  world->num_contacts = 0;
  for (i = 0; i < world->broadphase.num_pairs; i++) {
    int32_t a = world->broadphase.pairs[i].a;
    int32_t b = world->broadphase.pairs[i].b;
    int32_t body_a = world->colliders[a].body_index;
    int32_t body_b = world->colliders[b].body_index;
    TydraPhysTransform xa, xb;

    if (!body_index_or_world_is_valid(world, body_a) ||
        !body_index_or_world_is_valid(world, body_b)) {
      continue;
    }

    if (body_index_is_valid(world, body_a)) {
      xa = world->bodies[body_a].xform;
    } else {
      xa = tp_xform_identity();
    }
    if (body_index_is_valid(world, body_b)) {
      xb = world->bodies[body_b].xform;
    } else {
      xb = tp_xform_identity();
    }

    int32_t remaining = world->max_contacts - world->num_contacts;
    if (remaining <= 0) break;

    int32_t nc = tydra_phys_narrow_phase(
        &world->colliders[a], &xa,
        &world->colliders[b], &xb,
        &world->contacts[world->num_contacts], remaining);
    if (nc > 0) {
      /* Fill in body indices and zero impulse accumulators for each new contact */
      int32_t k;
      for (k = 0; k < nc; k++) {
        TydraPhysContact *c = &world->contacts[world->num_contacts + k];
        c->body_a = body_a;
        c->body_b = body_b;
        c->collider_a = a;
        c->collider_b = b;
        c->normal_impulse_accum = 0.0f;
        c->tangent_impulse_accum[0] = 0.0f;
        c->tangent_impulse_accum[1] = 0.0f;
      }
      world->num_contacts += nc;
    }
  }

  /* 4. Island detection */
  detect_islands(world);

  /* 5. Pre-step constraints */
  prepare_contacts(world);
  prepare_joints(world);

  /* 6. Integrate velocities (apply forces/gravity to velocities BEFORE solving) */
  integrate_velocities(world);

  /* 7. Sequential impulse iterations (modify velocities to satisfy constraints) */
  for (iter = 0; iter < world->solver_iterations; iter++) {
    solve_contacts(world);
    solve_joints(world);
  }

  /* 8. Integrate positions (apply solved velocities to positions) */
  integrate_positions(world);

  /* 8. Sleep detection */
  update_sleep(world);

  /* 9. Clear forces */
  tydra_phys_clear_forces(world);

  return TYDRA_PHYS_OK;
}

void tydra_phys_wake_body(TydraPhysWorld *world, int32_t body_index) {
  int32_t i;
  if (!world) return;
  if (body_index < 0 || body_index >= world->num_bodies) return;

  TydraPhysBody *b = &world->bodies[body_index];
  b->flags &= ~static_cast<uint32_t>(TYDRA_PHYS_BODY_FLAG_SLEEPING);
  b->sleep_timer = 0.0f;

  /* Wake all bodies in the same island */
  int32_t island_id = b->island_id;
  if (island_id >= 0 && island_id < world->num_islands) {
    TydraPhysIsland *island = &world->islands[island_id];
    island->sleeping = 0;
    for (i = 0; i < island->num_bodies; i++) {
      TydraPhysBody *ib = &world->bodies[island->body_indices[i]];
      ib->flags &= ~static_cast<uint32_t>(TYDRA_PHYS_BODY_FLAG_SLEEPING);
      ib->sleep_timer = 0.0f;
    }
  }
}
