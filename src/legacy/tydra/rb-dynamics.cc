/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026-Present Light Transport Entertainment Inc. */
/*
 * rb-dynamics.cc -- Rigid body dynamics implementation
 *
 * Sequential impulse constraint solver with semi-implicit Euler integration,
 * Coulomb friction, joint constraints, island-based sleeping, and CCD.
 */

#include "rb-dynamics.h"
#include "core/safe-arithmetic.hh"
#include <string.h>
#include <float.h>
#include <math.h>

#include <cstdint>
#include <memory>

/* ======================================================================== */
/* Contact constraint cache (file-local)                                    */
/* ======================================================================== */

#define TYDRA_MAX_CONTACT_CACHE 4096

typedef struct {
  TydraPhysVec3 r_a, r_b;        /* vectors from body COM to contact point */
  float effective_mass_n;          /* normal effective mass */
  float effective_mass_t[2];       /* tangent effective masses */
  float bias;                      /* Baumgarte velocity bias */
  TydraPhysVec3 tangent[2];       /* tangent vectors */
} ContactConstraint;

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

  /* Count bodies per root */
  /* Use island_body_buf temporarily for counting.
   * Layout: first num_bodies entries for counts, rest for body lists. */
  int32_t *counts = world->island_body_buf;
  memset(counts, 0, sizeof(int32_t) * static_cast<size_t>(num_bodies));

  for (i = 0; i < num_bodies; i++) {
    if (world->bodies[i].body_type == TYDRA_PHYS_BODY_STATIC) continue;
    int32_t root = world->island_union[i];
    counts[root]++;
  }

  /* Assign island indices to roots.
   * Reuse island_body_buf[0..num_bodies-1] as root_to_island map (safe
   * because we rebuild it below and num_bodies <= max_bodies). */
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

  /* Compute per-island offsets into body_buf */
  {
    int32_t offset = 0;
    for (i = 0; i < island_count && i < world->max_islands; i++) {
      world->islands[i].body_indices = &body_buf[offset];
      offset += counts[i];
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

/* ======================================================================== */
/* PART 2: C++ Bridge                                                       */
/* ======================================================================== */

#ifdef __cplusplus

#include "rb-dynamics.hh"
#include "lightusd.hh"
#include "core/prim.hh"
#include "usdPhysics.hh"
#include "mjcPhysics.hh"
#include "usdGeom.hh"

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

namespace lightusd { namespace legacy {
namespace tydra {

namespace {

/* Recursive prim traversal (same pattern as physics-to-json.cc) */
using PrimVisitor = std::function<void(const Prim &, const std::string &)>;
void TraversePrims(const std::vector<Prim> &prims,
                   const std::string &parent_path,
                   PrimVisitor visitor) {
  for (const auto &prim : prims) {
    std::string path = parent_path + "/" + prim.element_name();
    visitor(prim, path);
    TraversePrims(prim.children(), path, visitor);
  }
}

using MutablePrimVisitor = std::function<void(Prim &, const std::string &)>;
void TraversePrimsMutable(std::vector<Prim> &prims,
                          const std::string &parent_path,
                          MutablePrimVisitor visitor) {
  for (auto &prim : prims) {
    const std::string path = parent_path + "/" + prim.element_name();
    visitor(prim, path);
    TraversePrimsMutable(prim.children(), path, visitor);
  }
}

const Xformable *GetXformable(const Prim &prim) {
  if (const auto *v = prim.as<GeomSphere>()) return v;
  if (const auto *v = prim.as<GeomCube>()) return v;
  if (const auto *v = prim.as<GeomCapsule>()) return v;
  if (const auto *v = prim.as<GeomCylinder>()) return v;
  if (const auto *v = prim.as<Xform>()) return v;
  return nullptr;
}

Xformable *GetXformable(Prim &prim) {
  if (auto *v = prim.get_data().as<GeomSphere>()) return v;
  if (auto *v = prim.get_data().as<GeomCube>()) return v;
  if (auto *v = prim.get_data().as<GeomCapsule>()) return v;
  if (auto *v = prim.get_data().as<GeomCylinder>()) return v;
  if (auto *v = prim.get_data().as<Xform>()) return v;
  return nullptr;
}

/* Check if a prim has a specific API schema in its apiSchemas metadata */
bool HasAPISchema(const Prim &prim, APISchemas::APIName api_name) {
  const PrimMeta &meta = prim.metas();
  if (!meta.has_apiSchemas()) return false;
  const APISchemas &schemas = meta.get_apiSchemas();
  for (const auto &entry : schemas.names) {
    if (entry.first == api_name) return true;
  }
  return false;
}

/* Helper to get a float property from a prim's props map */
bool GetFloatProp(const std::map<std::string, Property> &props,
                  const std::string &name, float *out) {
  auto it = props.find(name);
  if (it == props.end()) return false;

  const Property &prop = it->second;
  if (prop.is_attribute()) {
    const Attribute &attr = prop.get_attribute();
    float fval;
    if (attr.get_value(&fval)) {
      *out = fval;
      return true;
    }
    /* Try double */
    double dval;
    if (attr.get_value(&dval)) {
      *out = static_cast<float>(dval);
      return true;
    }
  }
  return false;
}

/* Relationship target path extraction */
std::string GetRelTargetPath(const RelationshipProperty &rp) {
  if (!rp.authored()) return "";
  const auto &rel = rp.relationship();
  if (rel.is_path()) {
    return rel.targetPath.full_path_name();
  }
  return "";
}

/* Helper to build TydraPhysCollider from geometry prim */
void BuildColliderFromGeom(const Prim &prim,
                           const std::string & /* path */,
                           int32_t body_index,
                           float default_friction,
                           float default_restitution,
                           TydraPhysCollider *col) {
  memset(col, 0, sizeof(TydraPhysCollider));
  col->body_index = body_index;
  col->local_pose = tp_xform_identity();
  col->static_friction = default_friction;
  col->dynamic_friction = default_friction;
  col->restitution = default_restitution;
  col->collision_group = 0xFFFFFFFF;
  col->collision_mask = 0xFFFFFFFF;

  /* Determine shape from prim type */
  if (auto *sphere = prim.as<GeomSphere>()) {
    col->shape.type = TYDRA_PHYS_SHAPE_SPHERE;
    double r = 2.0;
    sphere->radius.get_value().get_scalar(&r);
    col->shape.data.sphere.radius = static_cast<float>(r);
  } else if (auto *cube = prim.as<GeomCube>()) {
    col->shape.type = TYDRA_PHYS_SHAPE_BOX;
    double sz = 2.0;
    cube->size.get_value().get_scalar(&sz);
    float half = static_cast<float>(sz) * 0.5f;
    col->shape.data.box.half_extents = tp_v3(half, half, half);
  } else if (auto *capsule = prim.as<GeomCapsule>()) {
    col->shape.type = TYDRA_PHYS_SHAPE_CAPSULE;
    double r = 0.5, h = 2.0;
    capsule->radius.get_value().get_scalar(&r);
    capsule->height.get_value().get_scalar(&h);
    col->shape.data.capsule.radius = static_cast<float>(r);
    col->shape.data.capsule.half_height = static_cast<float>(h) * 0.5f;
  } else if (auto *cylinder = prim.as<GeomCylinder>()) {
    col->shape.type = TYDRA_PHYS_SHAPE_CYLINDER;
    double r = 1.0, h = 2.0;
    cylinder->radius.get_value().get_scalar(&r);
    cylinder->height.get_value().get_scalar(&h);
    col->shape.data.cylinder.radius = static_cast<float>(r);
    col->shape.data.cylinder.half_height = static_cast<float>(h) * 0.5f;
  } else {
    /* Default to a unit sphere for unsupported types */
    col->shape.type = TYDRA_PHYS_SHAPE_SPHERE;
    col->shape.data.sphere.radius = 1.0f;
  }
}

/* Convert USD axis token to TydraPhysVec3 */
TydraPhysVec3 AxisTokenToVec3(const value::token &tok) {
  std::string s = tok.str();
  if (s == "X") return tp_v3(1, 0, 0);
  if (s == "Y") return tp_v3(0, 1, 0);
  if (s == "Z") return tp_v3(0, 0, 1);
  return tp_v3(0, 1, 0); /* Default Y */
}

/* Build anchor transform from joint's localPos/localRot */
TydraPhysTransform BuildAnchorTransform(
    const TypedAttributeWithFallback<value::point3f> &pos,
    const TypedAttributeWithFallback<value::quatf> &rot) {
  TydraPhysTransform xf = tp_xform_identity();

  if (pos.authored()) {
    value::point3f p = pos.get_value();
    xf.position = tp_v3(p[0], p[1], p[2]);
  }

  if (rot.authored()) {
    value::quatf q = rot.get_value();
    /* value::quatf is (x,y,z,w) or (w,x,y,z)? Check convention. */
    /* lightusd quatf: real part first? Actually
     * value::quatf stores as [x, y, z, w] based on USD convention. */
    xf.rotation = tp_q(q[0], q[1], q[2], q[3]);
  }

  return xf;
}

}  // anonymous namespace

bool BuildPhysWorld(
    const Stage &stage,
    TydraPhysWorld *out_world,
    std::string *err,
    const PhysWorldBuildOptions &options) {

  if (!out_world) {
    if (err) *err = "out_world is null";
    return false;
  }

  if (options.max_bodies <= 0 || options.max_colliders <= 0 ||
      options.max_joints <= 0 || options.max_contacts <= 0 ||
      options.max_pairs <= 0 || options.max_islands <= 0) {
    if (err) *err = "Physics world buffer capacities must be positive";
    return false;
  }

  size_t island_body_count = 0;
  if (!safe::mul(size_t(options.max_bodies), size_t(2), &island_body_count)) {
    if (err) *err = "Integer overflow computing physics island body buffer size";
    return false;
  }

  /* Validate allocation sizes against memory budget and overflow */
  {
    size_t total_bytes = 0;
    size_t part_bytes;

#define RB_MUL_CHECK(n, sizet) \
    do { \
      if (!safe::mul(size_t(n), sizet, &part_bytes)) { \
        if (err) *err = "Integer overflow computing physics buffer allocation size"; \
        return false; \
      } \
      if (!safe::add(total_bytes, part_bytes, &total_bytes)) { \
        if (err) *err = "Integer overflow in physics buffer total allocation size"; \
        return false; \
      } \
    } while (0)

    RB_MUL_CHECK(options.max_bodies, sizeof(TydraPhysBody));
    RB_MUL_CHECK(options.max_colliders, sizeof(TydraPhysCollider));
    RB_MUL_CHECK(options.max_joints, sizeof(TydraPhysJoint));
    RB_MUL_CHECK(options.max_contacts, sizeof(TydraPhysContact));
    RB_MUL_CHECK(options.max_colliders, sizeof(TydraPhysAABB));
    RB_MUL_CHECK(options.max_pairs, sizeof(TydraPhysCollisionPair));
    RB_MUL_CHECK(options.max_colliders, sizeof(int32_t));
    RB_MUL_CHECK(options.max_islands, sizeof(TydraPhysIsland));
    RB_MUL_CHECK(options.max_bodies, sizeof(int32_t));
    RB_MUL_CHECK(island_body_count, sizeof(int32_t));
    RB_MUL_CHECK(options.max_contacts, sizeof(ContactConstraint));

#undef RB_MUL_CHECK

    if (options.max_memory_limit_mb > 0) {
      size_t limit_bytes;
      if (!safe::mul(options.max_memory_limit_mb, size_t(1024 * 1024), &limit_bytes)) {
        if (err) *err = "Integer overflow computing max_memory_limit_mb";
        return false;
      }
      if (total_bytes > limit_bytes) {
        if (err) *err = "Physics world buffer allocation exceeds max_memory_limit_mb";
        return false;
      }
    }
  }

  /* Allocate all buffers — wrapped in unique_ptr for RAII cleanup */
  std::unique_ptr<TydraPhysBody[]> body_buf(new TydraPhysBody[
      static_cast<size_t>(options.max_bodies)]);
  std::unique_ptr<TydraPhysCollider[]> col_buf(new TydraPhysCollider[
      static_cast<size_t>(options.max_colliders)]);
  std::unique_ptr<TydraPhysJoint[]> joint_buf(new TydraPhysJoint[
      static_cast<size_t>(options.max_joints)]);
  std::unique_ptr<TydraPhysContact[]> contact_buf(new TydraPhysContact[
      static_cast<size_t>(options.max_contacts)]);
  std::unique_ptr<TydraPhysAABB[]> aabb_buf(new TydraPhysAABB[
      static_cast<size_t>(options.max_colliders)]);
  std::unique_ptr<TydraPhysCollisionPair[]> pair_buf(new TydraPhysCollisionPair[
      static_cast<size_t>(options.max_pairs)]);
  std::unique_ptr<int32_t[]> sort_buf(new int32_t[
      static_cast<size_t>(options.max_colliders)]);
  std::unique_ptr<TydraPhysIsland[]> island_buf(new TydraPhysIsland[
      static_cast<size_t>(options.max_islands)]);
  std::unique_ptr<int32_t[]> union_buf(new int32_t[
      static_cast<size_t>(options.max_bodies)]);
  /* island_body_buf needs space for counts (max_bodies) + body lists (max_bodies) */
  std::unique_ptr<int32_t[]> island_body_buf(new int32_t[
      island_body_count]);

  tydra_phys_world_init(out_world,
                         body_buf.get(), options.max_bodies,
                         col_buf.get(), options.max_colliders,
                         joint_buf.get(), options.max_joints,
                         contact_buf.get(), options.max_contacts,
                         aabb_buf.get(),
                         pair_buf.get(), options.max_pairs,
                         sort_buf.get(),
                         island_buf.get(), options.max_islands,
                         union_buf.get(),
                         island_body_buf.get());

  /* Per-world contact constraint cache (thread-safe, unlike static fallback) */
  std::unique_ptr<ContactConstraint[]> contact_cache(new ContactConstraint[
      static_cast<size_t>(options.max_contacts)]);

  /* Transfer ownership to out_world — FreePhysWorld will delete[] them */
  body_buf.release();
  col_buf.release();
  joint_buf.release();
  contact_buf.release();
  aabb_buf.release();
  pair_buf.release();
  sort_buf.release();
  island_buf.release();
  union_buf.release();
  island_body_buf.release();
  out_world->contact_cache = contact_cache.release();

  /* Maps for resolving prim paths to body indices */
  std::unordered_map<std::string, int32_t> path_to_body;
  std::vector<std::string> body_paths; /* parallel to body indices */

  /* First pass: find PhysicsScene and configure world */
  TraversePrims(stage.root_prims(), "",
    [&](const Prim &prim, const std::string &path) {
      (void)path;
      if (auto *scene = prim.as<PhysicsScene>()) {
        /* Gravity */
        TydraPhysVec3 grav_dir = tp_v3(0, 0, -1);
        float grav_mag = 9.81f;

        auto gd_opt = scene->gravityDirection.get_value();
        if (gd_opt.has_value()) {
          value::vector3f gd = gd_opt.value();
          grav_dir = tp_v3(gd[0], gd[1], gd[2]);
        }
        auto gm_opt = scene->gravityMagnitude.get_value();
        if (gm_opt.has_value()) {
          grav_mag = gm_opt.value();
        }

        out_world->gravity = tp_v3_scale(
            tp_v3_normalize(grav_dir), grav_mag);

        /* MuJoCo scene parameters */
        if (options.use_mjc_params && scene->mjcScene.has_value()) {
          const MjcSceneAPI &mjc = scene->mjcScene.value();
          out_world->timestep = static_cast<float>(
              mjc.timestep.get_value());
          out_world->solver_iterations = mjc.iterations.get_value();
        }
      }
    });

  /* Second pass: create bodies and colliders for prims with RigidBodyAPI */
  TraversePrims(stage.root_prims(), "",
    [&](const Prim &prim, const std::string &path) {
      bool has_rb = HasAPISchema(prim, APISchemas::APIName::PhysicsRigidBodyAPI);
      if (!has_rb) return;

      /* Create body */
      TydraPhysBody body;
      tydra_phys_body_default(&body);

      PhysicsRigidBodyAPI rigid_api;
      if (GetPhysicsRigidBodyAPI(prim, &rigid_api)) {
        if (!rigid_api.rigidBodyEnabled.get_value()) {
          body.body_type = TYDRA_PHYS_BODY_STATIC;
          body.flags &=
              ~static_cast<uint32_t>(TYDRA_PHYS_BODY_FLAG_GRAVITY);
        } else if (rigid_api.kinematicEnabled.get_value()) {
          body.body_type = TYDRA_PHYS_BODY_KINEMATIC;
          body.flags &=
              ~static_cast<uint32_t>(TYDRA_PHYS_BODY_FLAG_GRAVITY);
        }
        if (rigid_api.startsAsleep.get_value()) {
          body.flags |= TYDRA_PHYS_BODY_FLAG_SLEEPING;
        }
        if (rigid_api.velocity.authored()) {
          value::vector3f velocity;
          const auto authored = rigid_api.velocity.get_value();
          if (authored.has_value() && authored.value().get_scalar(&velocity)) {
            body.linear_velocity = tp_v3(velocity[0], velocity[1], velocity[2]);
          }
        }
        if (rigid_api.angularVelocity.authored()) {
          value::vector3f velocity;
          const auto authored = rigid_api.angularVelocity.get_value();
          if (authored.has_value() && authored.value().get_scalar(&velocity)) {
            body.angular_velocity = tp_v3(velocity[0], velocity[1], velocity[2]);
          }
        }
      }

      if (const Xformable *xformable = GetXformable(prim)) {
        auto matrix = xformable->GetLocalMatrix();
        if (matrix) {
          value::double3 translation{{0.0, 0.0, 0.0}};
          value::double3 scale{{1.0, 1.0, 1.0}};
          value::quatd rotation{};
          if (decompose(matrix.value(), &translation, &rotation, &scale)) {
            body.xform.position = tp_v3(static_cast<float>(translation[0]),
                                        static_cast<float>(translation[1]),
                                        static_cast<float>(translation[2]));
            body.xform.rotation = tp_q(static_cast<float>(rotation[0]),
                                       static_cast<float>(rotation[1]),
                                       static_cast<float>(rotation[2]),
                                       static_cast<float>(rotation[3]));
          }
        }
      }

      /* Extract mass from props */
      float mass = options.default_mass;

      /* Try to access mass and velocity from the prim's props map.
       * The physics API properties are stored under "physics:" namespace. */
      const std::map<std::string, Property> *props = nullptr;

      /* Check various geometry types for their props map */
      if (auto *sphere = prim.as<GeomSphere>()) {
        props = &sphere->props;
      } else if (auto *cube = prim.as<GeomCube>()) {
        props = &cube->props;
      } else if (auto *capsule = prim.as<GeomCapsule>()) {
        props = &capsule->props;
      } else if (auto *cylinder = prim.as<GeomCylinder>()) {
        props = &cylinder->props;
      } else if (auto *xform = prim.as<Xform>()) {
        props = &xform->props;
      }

      if (props) {
        float m;
        if (GetFloatProp(*props, "physics:mass", &m) && m > 0.0f) {
          mass = m;
        }
      }

      body.inverse_mass = (mass > TP_EPSILON) ? (1.0f / mass) : 0.0f;

      /* Compute diagonal inverse inertia (approximate: uniform sphere) */
      if (mass > TP_EPSILON) {
        float inv = 1.0f / mass;
        /* Simple: I = (2/5)*m*r^2 for sphere; just use 1/mass as default */
        body.local_inv_inertia = tp_v3(inv, inv, inv);
      }

      int32_t body_idx = tydra_phys_add_body(out_world, &body);
      if (body_idx < 0) {
        if (err) *err = "Max bodies exceeded at path: " + path;
        return;
      }

      path_to_body[path] = body_idx;
      body_paths.push_back(path);

      /* Create collider */
      TydraPhysCollider col;
      BuildColliderFromGeom(prim, path, body_idx,
                            options.default_friction,
                            options.default_restitution, &col);

      int32_t col_idx = tydra_phys_add_collider(out_world, &col);
      if (col_idx >= 0) {
        out_world->bodies[body_idx].collider_start = col_idx;
        out_world->bodies[body_idx].collider_count = 1;
      }
    });

  /* Third pass: create joints */
  TraversePrims(stage.root_prims(), "",
    [&](const Prim &prim, const std::string & /* path */) {

      /* Helper lambda to build a joint from a joint prim */
      auto buildJoint = [&](const PhysicsJointBase &base,
                            TydraPhysJointType type,
                            TydraPhysJoint *jnt) {
        memset(jnt, 0, sizeof(TydraPhysJoint));
        jnt->type = type;
        jnt->body_a = -1;
        jnt->body_b = -1;

        /* Resolve body0/body1 paths to body indices */
        std::string b0_path = GetRelTargetPath(base.body0);
        std::string b1_path = GetRelTargetPath(base.body1);
        if (!b0_path.empty()) {
          auto it = path_to_body.find(b0_path);
          if (it != path_to_body.end()) jnt->body_a = it->second;
        }
        if (!b1_path.empty()) {
          auto it = path_to_body.find(b1_path);
          if (it != path_to_body.end()) jnt->body_b = it->second;
        }

        /* Anchor transforms */
        jnt->local_anchor_a = BuildAnchorTransform(base.localPos0,
                                                    base.localRot0);
        jnt->local_anchor_b = BuildAnchorTransform(base.localPos1,
                                                    base.localRot1);

        /* MuJoCo joint parameters */
        if (options.use_mjc_params && base.mjcJoint.has_value()) {
          const MjcJointAPI &mjc = base.mjcJoint.value();
          jnt->stiffness = static_cast<float>(mjc.stiffness.get_value());
          jnt->damping = static_cast<float>(mjc.damping.get_value());
        }
      };

      if (auto *rj = prim.as<PhysicsRevoluteJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*rj, TYDRA_PHYS_JOINT_HINGE, &jnt);

        /* Axis */
        auto axis_opt = rj->axis.get_value();
        if (axis_opt.has_value()) {
          jnt.axis = AxisTokenToVec3(axis_opt.value());
        } else {
          jnt.axis = tp_v3(0, 1, 0);
        }

        /* Limits */
        auto lower = rj->lowerLimit.get_value();
        auto upper = rj->upperLimit.get_value();
        if (lower.has_value() || upper.has_value()) {
          jnt.limit_enabled = 1;
          jnt.lower_limit = lower.has_value()
              ? (lower.value() * TP_PI / 180.0f) : -TP_PI;
          jnt.upper_limit = upper.has_value()
              ? (upper.value() * TP_PI / 180.0f) : TP_PI;
        }

        tydra_phys_add_joint(out_world, &jnt);

      } else if (auto *pj = prim.as<PhysicsPrismaticJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*pj, TYDRA_PHYS_JOINT_SLIDER, &jnt);

        auto axis_opt = pj->axis.get_value();
        if (axis_opt.has_value()) {
          jnt.axis = AxisTokenToVec3(axis_opt.value());
        } else {
          jnt.axis = tp_v3(0, 1, 0);
        }

        auto lower = pj->lowerLimit.get_value();
        auto upper = pj->upperLimit.get_value();
        if (lower.has_value() || upper.has_value()) {
          jnt.limit_enabled = 1;
          jnt.lower_limit = lower.has_value()
              ? lower.value() : -TP_UNLIMITED;
          jnt.upper_limit = upper.has_value()
              ? upper.value() : TP_UNLIMITED;
        }

        tydra_phys_add_joint(out_world, &jnt);

      } else if (auto *sj = prim.as<PhysicsSphericalJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*sj, TYDRA_PHYS_JOINT_BALL, &jnt);

        /* Spherical joints may have cone limits; store them but the ball
         * joint solver doesn't enforce cone limits currently */
        auto cone0 = sj->coneAngle0Limit.get_value();
        auto cone1 = sj->coneAngle1Limit.get_value();
        if (cone0.has_value() || cone1.has_value()) {
          jnt.limit_enabled = 1;
          jnt.lower_limit = cone0.has_value()
              ? (cone0.value() * TP_PI / 180.0f) : TP_PI;
          jnt.upper_limit = cone1.has_value()
              ? (cone1.value() * TP_PI / 180.0f) : TP_PI;
        }

        tydra_phys_add_joint(out_world, &jnt);

      } else if (auto *fj = prim.as<PhysicsFixedJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*fj, TYDRA_PHYS_JOINT_FIXED, &jnt);
        tydra_phys_add_joint(out_world, &jnt);

      } else if (auto *dj = prim.as<PhysicsDistanceJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*dj, TYDRA_PHYS_JOINT_DISTANCE, &jnt);

        auto mind = dj->minDistance.get_value();
        auto maxd = dj->maxDistance.get_value();
        jnt.min_distance = mind.has_value() ? mind.value() : 0.0f;
        jnt.max_distance = maxd.has_value() ? maxd.value() : TP_UNLIMITED;

        tydra_phys_add_joint(out_world, &jnt);
      }
    });

  return true;
}

bool SyncPhysWorldToStage(
    const TydraPhysWorld &world,
    Stage *stage,
    std::string *err) {
  if (!stage) {
    if (err) *err = "stage is null";
    return false;
  }
  int32_t body_index = 0;
  bool overflow = false;
  TraversePrimsMutable(stage->root_prims(), "",
    [&](Prim &prim, const std::string &) {
      if (!HasAPISchema(prim, APISchemas::APIName::PhysicsRigidBodyAPI)) return;
      if (body_index >= world.num_bodies) {
        overflow = true;
        return;
      }
      Xformable *xformable = GetXformable(prim);
      const TydraPhysBody &body = world.bodies[body_index++];
      if (!xformable) return;

      value::double3 scale{{1.0, 1.0, 1.0}};
      auto original = xformable->GetLocalMatrix();
      if (original) {
        value::double3 ignored_translation;
        value::quatd ignored_rotation;
        decompose(original.value(), &ignored_translation, &ignored_rotation, &scale);
      }
      XformOp translate;
      translate.op_type = XformOp::OpType::Translate;
      translate.set_value(value::double3{{
          static_cast<double>(body.xform.position.x),
          static_cast<double>(body.xform.position.y),
          static_cast<double>(body.xform.position.z)}});
      XformOp orient;
      orient.op_type = XformOp::OpType::Orient;
      value::quatd rotation;
      rotation[0] = static_cast<double>(body.xform.rotation.x);
      rotation[1] = static_cast<double>(body.xform.rotation.y);
      rotation[2] = static_cast<double>(body.xform.rotation.z);
      rotation[3] = static_cast<double>(body.xform.rotation.w);
      orient.set_value(rotation);
      xformable->xformOps.clear();
      xformable->xformOps.push_back(std::move(translate));
      xformable->xformOps.push_back(std::move(orient));
      if (std::fabs(scale[0] - 1.0) > 1.0e-12 ||
          std::fabs(scale[1] - 1.0) > 1.0e-12 ||
          std::fabs(scale[2] - 1.0) > 1.0e-12) {
        XformOp scale_op;
        scale_op.op_type = XformOp::OpType::Scale;
        scale_op.set_value(scale);
        xformable->xformOps.push_back(std::move(scale_op));
      }
    });
  if (overflow || body_index != world.num_bodies) {
    if (err) *err = "Rigid-body count no longer matches the source Stage";
    return false;
  }
  return true;
}

bool SyncStageToPhysWorld(const Stage &stage, TydraPhysWorld *world,
                          std::string *err) {
  if (!world) {
    if (err) *err = "world is null";
    return false;
  }
  int32_t body_index = 0;
  bool overflow = false;
  TraversePrims(stage.root_prims(), "",
    [&](const Prim &prim, const std::string &) {
      if (!HasAPISchema(prim, APISchemas::APIName::PhysicsRigidBodyAPI)) return;
      if (body_index >= world->num_bodies) {
        overflow = true;
        return;
      }
      TydraPhysBody &body = world->bodies[body_index++];
      if (body.body_type != TYDRA_PHYS_BODY_KINEMATIC) return;
      const Xformable *xformable = GetXformable(prim);
      if (!xformable) return;
      auto matrix = xformable->GetLocalMatrix();
      if (!matrix) return;
      value::double3 translation, scale;
      value::quatd rotation;
      if (!decompose(matrix.value(), &translation, &rotation, &scale)) return;
      const TydraPhysTransform previous = body.xform;
      const TydraPhysTransform next = {
          tp_v3(static_cast<float>(translation[0]),
                static_cast<float>(translation[1]),
                static_cast<float>(translation[2])),
          tp_q_normalize(tp_q(static_cast<float>(rotation[0]),
                              static_cast<float>(rotation[1]),
                              static_cast<float>(rotation[2]),
                              static_cast<float>(rotation[3])))};
      const float inverse_dt = world->timestep > TP_EPSILON
                                   ? 1.0f / world->timestep
                                   : 0.0f;
      body.linear_velocity = tp_v3_scale(
          tp_v3_sub(next.position, previous.position), inverse_dt);
      const TydraPhysQuat delta = tp_q_mul(
          next.rotation, tp_q_conjugate(previous.rotation));
      TydraPhysVec3 axis;
      const float angle = tp_q_to_axis_angle(delta, &axis);
      body.angular_velocity = tp_v3_scale(axis, angle * inverse_dt);
      body.xform = next;
    });
  if (overflow || body_index != world->num_bodies) {
    if (err) *err = "Rigid-body count no longer matches the source Stage";
    return false;
  }
  return true;
}

void FreePhysWorld(TydraPhysWorld *world) {
  if (!world) return;

  delete[] world->bodies;
  delete[] world->colliders;
  delete[] world->joints;
  delete[] world->contacts;
  delete[] world->broadphase.aabbs;
  delete[] world->broadphase.pairs;
  delete[] world->broadphase.sorted_indices;
  delete[] world->islands;
  delete[] world->island_union;
  delete[] world->island_body_buf;
  delete[] static_cast<ContactConstraint *>(world->contact_cache);

  memset(world, 0, sizeof(TydraPhysWorld));
}

}  // namespace tydra
}  // namespace legacy
}  // namespace lightusd

#endif /* __cplusplus */
