/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026-Present Light Transport Entertainment Inc. */
/*
 * rb-collision.cc -- Collision detection implementation (broadphase + narrowphase)
 *
 * Pure C implementation. No C++ features used despite the .cc extension.
 */

#include "rb-collision.h"

#include <math.h>
#include <string.h>
#include <float.h>

/* ======================================================================== */
/* Internal helpers                                                         */
/* ======================================================================== */

/* Compose body_xform * col->local_pose into a world transform. */
static TydraPhysTransform collider_world_xform(const TydraPhysCollider *col,
                                               const TydraPhysTransform *body_xform) {
  return tp_xform_mul(*body_xform, col->local_pose);
}

/* Closest points between two line segments P0P1 and Q0Q1.
 * Returns parametric values s,t in [0,1] for the closest points. */
static void closest_points_segments(TydraPhysVec3 p0, TydraPhysVec3 p1,
                                    TydraPhysVec3 q0, TydraPhysVec3 q1,
                                    float *out_s, float *out_t) {
  TydraPhysVec3 d1 = tp_v3_sub(p1, p0);
  TydraPhysVec3 d2 = tp_v3_sub(q1, q0);
  TydraPhysVec3 r  = tp_v3_sub(p0, q0);

  float a = tp_v3_dot(d1, d1);
  float e = tp_v3_dot(d2, d2);
  float f = tp_v3_dot(d2, r);

  float s, t;

  if (a <= TP_EPSILON && e <= TP_EPSILON) {
    /* Both segments degenerate to points. */
    *out_s = 0.0f;
    *out_t = 0.0f;
    return;
  }
  if (a <= TP_EPSILON) {
    s = 0.0f;
    t = tp_clampf(f / e, 0.0f, 1.0f);
  } else {
    float c = tp_v3_dot(d1, r);
    if (e <= TP_EPSILON) {
      t = 0.0f;
      s = tp_clampf(-c / a, 0.0f, 1.0f);
    } else {
      float b = tp_v3_dot(d1, d2);
      float denom = a * e - b * b;

      if (denom > TP_EPSILON) {
        s = tp_clampf((b * f - c * e) / denom, 0.0f, 1.0f);
      } else {
        s = 0.0f;
      }

      t = (b * s + f) / e;

      if (t < 0.0f) {
        t = 0.0f;
        s = tp_clampf(-c / a, 0.0f, 1.0f);
      } else if (t > 1.0f) {
        t = 1.0f;
        s = tp_clampf((b - c) / a, 0.0f, 1.0f);
      }
    }
  }

  *out_s = s;
  *out_t = t;
}

/* Combined material properties for a contact. */
static float combined_friction(const TydraPhysCollider *a,
                               const TydraPhysCollider *b) {
  return sqrtf(a->static_friction * b->static_friction);
}

static float combined_restitution(const TydraPhysCollider *a,
                                  const TydraPhysCollider *b) {
  return tp_maxf(a->restitution, b->restitution);
}

/* Fill the body/collider indices and material fields of a contact. */
static void fill_contact_meta(TydraPhysContact *c,
                              const TydraPhysCollider *ca, int32_t ia,
                              const TydraPhysCollider *cb, int32_t ib) {
  c->collider_a = ia;
  c->collider_b = ib;
  c->body_a = ca->body_index;
  c->body_b = cb->body_index;
  c->combined_friction = combined_friction(ca, cb);
  c->combined_restitution = combined_restitution(ca, cb);
  c->normal_impulse_accum = 0.0f;
  c->tangent_impulse_accum[0] = 0.0f;
  c->tangent_impulse_accum[1] = 0.0f;
}

/* ======================================================================== */
/* 1. AABB computation                                                      */
/* ======================================================================== */

void tydra_phys_collider_aabb(const TydraPhysCollider *col,
                              const TydraPhysTransform *body_xform,
                              TydraPhysAABB *out) {
  TydraPhysTransform wx = collider_world_xform(col, body_xform);

  switch (col->shape.type) {
    case TYDRA_PHYS_SHAPE_SPHERE: {
      float r = col->shape.data.sphere.radius;
      out->min = tp_v3_sub(wx.position, tp_v3(r, r, r));
      out->max = tp_v3_add(wx.position, tp_v3(r, r, r));
      break;
    }

    case TYDRA_PHYS_SHAPE_BOX: {
      TydraPhysVec3 he = col->shape.data.box.half_extents;
      /* Transform all 8 corners and take min/max. */
      TydraPhysVec3 vmin = tp_v3( FLT_MAX,  FLT_MAX,  FLT_MAX);
      TydraPhysVec3 vmax = tp_v3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
      int i;
      for (i = 0; i < 8; i++) {
        float sx = (i & 1) ? he.x : -he.x;
        float sy = (i & 2) ? he.y : -he.y;
        float sz = (i & 4) ? he.z : -he.z;
        TydraPhysVec3 corner = tp_xform_point(wx, tp_v3(sx, sy, sz));
        vmin = tp_v3_min(vmin, corner);
        vmax = tp_v3_max(vmax, corner);
      }
      out->min = vmin;
      out->max = vmax;
      break;
    }

    case TYDRA_PHYS_SHAPE_CAPSULE: {
      float r  = col->shape.data.capsule.radius;
      float hh = col->shape.data.capsule.half_height;
      /* Two sphere endpoints at +/-half_height along local Y. */
      TydraPhysVec3 p0 = tp_xform_point(wx, tp_v3(0, -hh, 0));
      TydraPhysVec3 p1 = tp_xform_point(wx, tp_v3(0,  hh, 0));
      TydraPhysVec3 rv = tp_v3(r, r, r);
      out->min = tp_v3_sub(tp_v3_min(p0, p1), rv);
      out->max = tp_v3_add(tp_v3_max(p0, p1), rv);
      break;
    }

    case TYDRA_PHYS_SHAPE_PLANE: {
      /* Infinite half-space: use a large AABB. */
      out->min = tp_v3(-1e6f, -1e6f, -1e6f);
      out->max = tp_v3( 1e6f,  1e6f,  1e6f);
      break;
    }

    case TYDRA_PHYS_SHAPE_CYLINDER: {
      float r  = col->shape.data.cylinder.radius;
      float hh = col->shape.data.cylinder.half_height;
      /* Two endpoint centers, then expand by radius in all axes.
       * More precise: the radius extends in the plane perpendicular to the
       * cylinder axis. We compute the world-space axis and use it. */
      TydraPhysVec3 p0 = tp_xform_point(wx, tp_v3(0, -hh, 0));
      TydraPhysVec3 p1 = tp_xform_point(wx, tp_v3(0,  hh, 0));
      TydraPhysVec3 axis = tp_v3_normalize(tp_v3_sub(p1, p0));
      /* For each world axis i, the extent of the circle is
       * r * sqrt(1 - axis[i]^2). We use the conservative r for simplicity. */
      TydraPhysVec3 rv = tp_v3(r, r, r);
      out->min = tp_v3_sub(tp_v3_min(p0, p1), rv);
      out->max = tp_v3_add(tp_v3_max(p0, p1), rv);
      (void)axis;
      break;
    }

  }
}

int32_t tydra_phys_aabb_overlap(const TydraPhysAABB *a,
                                const TydraPhysAABB *b) {
  if (a->max.x < b->min.x || a->min.x > b->max.x) return 0;
  if (a->max.y < b->min.y || a->min.y > b->max.y) return 0;
  if (a->max.z < b->min.z || a->min.z > b->max.z) return 0;
  return 1;
}

/* ======================================================================== */
/* 2. Broadphase sweep-and-prune                                            */
/* ======================================================================== */

void tydra_phys_broadphase_init(TydraPhysBroadphase *bp,
                                int32_t /*max_colliders*/,
                                TydraPhysAABB *aabb_buf,
                                TydraPhysCollisionPair *pair_buf,
                                int32_t max_pairs,
                                int32_t *sort_buf) {
  bp->aabbs = aabb_buf;
  bp->num_aabbs = 0;
  bp->pairs = pair_buf;
  bp->num_pairs = 0;
  bp->max_pairs = max_pairs;
  bp->sort_axis = 0;
  bp->sorted_indices = sort_buf;
}

/* Access the min value of an AABB on the given axis (0=X, 1=Y, 2=Z). */
static float aabb_min_on_axis(const TydraPhysAABB *aabb, int axis) {
  switch (axis) {
    case 0: return aabb->min.x;
    case 1: return aabb->min.y;
    case 2: return aabb->min.z;
  }
  return 0.0f;
}

static float aabb_max_on_axis(const TydraPhysAABB *aabb, int axis) {
  switch (axis) {
    case 0: return aabb->max.x;
    case 1: return aabb->max.y;
    case 2: return aabb->max.z;
  }
  return 0.0f;
}

/* Check if two colliders can collide based on group/mask. */
static int groups_compatible(const TydraPhysCollider *a,
                             const TydraPhysCollider *b) {
  if ((a->collision_group & b->collision_mask) == 0) return 0;
  if ((b->collision_group & a->collision_mask) == 0) return 0;
  return 1;
}

int32_t tydra_phys_broadphase_update(TydraPhysBroadphase *bp,
                                     const TydraPhysCollider *colliders,
                                     const TydraPhysTransform *body_xforms,
                                     int32_t num_colliders) {
  if (!bp || !colliders) return TYDRA_PHYS_COL_ERR_NULL_INPUT;

  bp->num_aabbs = num_colliders;
  bp->num_pairs = 0;

  int32_t i, j;

  /* Step 1: Compute AABBs (skip if body_xforms is NULL — caller pre-computed). */
  for (i = 0; i < num_colliders; i++) {
    if (body_xforms) {
      tydra_phys_collider_aabb(&colliders[i],
                               &body_xforms[colliders[i].body_index],
                               &bp->aabbs[i]);
    }
    bp->sorted_indices[i] = i;
  }

  /* Step 2: Choose sort axis based on variance of AABB centers. */
  {
    TydraPhysVec3 mean = tp_v3(0, 0, 0);
    for (i = 0; i < num_colliders; i++) {
      TydraPhysVec3 center = tp_v3_scale(
          tp_v3_add(bp->aabbs[i].min, bp->aabbs[i].max), 0.5f);
      mean = tp_v3_add(mean, center);
    }
    if (num_colliders > 0) {
      mean = tp_v3_scale(mean, 1.0f / static_cast<float>(num_colliders));
    }

    float var[3] = {0, 0, 0};
    for (i = 0; i < num_colliders; i++) {
      TydraPhysVec3 center = tp_v3_scale(
          tp_v3_add(bp->aabbs[i].min, bp->aabbs[i].max), 0.5f);
      TydraPhysVec3 d = tp_v3_sub(center, mean);
      var[0] += d.x * d.x;
      var[1] += d.y * d.y;
      var[2] += d.z * d.z;
    }

    bp->sort_axis = 0;
    if (var[1] > var[bp->sort_axis]) bp->sort_axis = 1;
    if (var[2] > var[bp->sort_axis]) bp->sort_axis = 2;
  }

  int axis = bp->sort_axis;

  /* Step 3: Insertion sort of sorted_indices by AABB min on sort_axis. */
  for (i = 1; i < num_colliders; i++) {
    int32_t key = bp->sorted_indices[i];
    float key_val = aabb_min_on_axis(&bp->aabbs[key], axis);
    j = i - 1;
    while (j >= 0) {
      float cur_val = aabb_min_on_axis(&bp->aabbs[bp->sorted_indices[j]], axis);
      if (cur_val <= key_val) break;
      bp->sorted_indices[j + 1] = bp->sorted_indices[j];
      j--;
    }
    bp->sorted_indices[j + 1] = key;
  }

  /* Step 4: Sweep and generate pairs. */
  for (i = 0; i < num_colliders; i++) {
    int32_t ai = bp->sorted_indices[i];
    float ai_max = aabb_max_on_axis(&bp->aabbs[ai], axis);

    for (j = i + 1; j < num_colliders; j++) {
      int32_t bi = bp->sorted_indices[j];
      float bi_min = aabb_min_on_axis(&bp->aabbs[bi], axis);

      /* If sorted min exceeds current max, no more overlaps on this axis. */
      if (bi_min > ai_max) break;

      /* Full 3-axis AABB overlap check. */
      if (!tydra_phys_aabb_overlap(&bp->aabbs[ai], &bp->aabbs[bi]))
        continue;

      /* Skip pairs on the same body. */
      if (colliders[ai].body_index == colliders[bi].body_index)
        continue;

      /* Check collision group/mask compatibility. */
      if (!groups_compatible(&colliders[ai], &colliders[bi]))
        continue;

      /* Add pair. */
      if (bp->num_pairs < bp->max_pairs) {
        bp->pairs[bp->num_pairs].a = ai;
        bp->pairs[bp->num_pairs].b = bi;
        bp->num_pairs++;
      }
    }
  }

  return bp->num_pairs;
}

/* ======================================================================== */
/* 3. Fast-path narrow phase                                                */
/* ======================================================================== */

/* Sphere vs Sphere. */
static int32_t sphere_vs_sphere(const TydraPhysCollider *ca,
                                const TydraPhysTransform *wa,
                                const TydraPhysCollider *cb,
                                const TydraPhysTransform *wb,
                                TydraPhysContact *contacts,
                                int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  float ra = ca->shape.data.sphere.radius;
  float rb = cb->shape.data.sphere.radius;

  TydraPhysVec3 diff = tp_v3_sub(wa->position, wb->position);
  float dist_sq = tp_v3_length_sq(diff);
  float sum_r = ra + rb;

  if (dist_sq >= sum_r * sum_r) return 0;

  float dist = sqrtf(dist_sq);
  TydraPhysContact *c = &contacts[0];

  if (dist > TP_EPSILON) {
    c->normal = tp_v3_scale(diff, 1.0f / dist);
  } else {
    c->normal = tp_v3(0, 1, 0);
  }

  c->depth = sum_r - dist;
  c->point = tp_v3_add(wb->position,
                        tp_v3_scale(c->normal, rb + c->depth * 0.5f));
  return 1;
}

/* Sphere vs Plane. */
static int32_t sphere_vs_plane(const TydraPhysCollider *cs,
                               const TydraPhysTransform *ws,
                               const TydraPhysCollider *cp,
                               const TydraPhysTransform *wp,
                               TydraPhysContact *contacts,
                               int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  float r = cs->shape.data.sphere.radius;
  TydraPhysVec3 plane_normal = tp_xform_vector(*wp, cp->shape.data.plane.normal);
  TydraPhysVec3 plane_point  = tp_v3_add(wp->position,
                                          tp_v3_scale(plane_normal, cp->shape.data.plane.offset));

  float dist = tp_v3_dot(tp_v3_sub(ws->position, plane_point), plane_normal);
  float pen = r - dist;

  if (pen <= 0.0f) return 0;

  TydraPhysContact *c = &contacts[0];
  c->normal = plane_normal;   /* Points from plane (B) to sphere (A). */
  c->depth  = pen;
  c->point  = tp_v3_sub(ws->position, tp_v3_scale(plane_normal, r));
  return 1;
}

/* Sphere vs Box. */
static int32_t sphere_vs_box(const TydraPhysCollider *cs,
                             const TydraPhysTransform *ws,
                             const TydraPhysCollider *cb,
                             const TydraPhysTransform *wb,
                             TydraPhysContact *contacts,
                             int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  float r = cs->shape.data.sphere.radius;
  TydraPhysVec3 he = cb->shape.data.box.half_extents;

  /* Transform sphere center into box local space. */
  TydraPhysTransform inv_box = tp_xform_inverse(*wb);
  TydraPhysVec3 local_center = tp_xform_point(inv_box, ws->position);

  /* Clamp to box extents to find closest point on box. */
  TydraPhysVec3 closest;
  closest.x = tp_clampf(local_center.x, -he.x, he.x);
  closest.y = tp_clampf(local_center.y, -he.y, he.y);
  closest.z = tp_clampf(local_center.z, -he.z, he.z);

  TydraPhysVec3 diff = tp_v3_sub(local_center, closest);
  float dist_sq = tp_v3_length_sq(diff);

  if (dist_sq >= r * r && dist_sq > TP_EPSILON) return 0;

  TydraPhysContact *c = &contacts[0];

  if (dist_sq > TP_EPSILON) {
    /* Sphere center is outside the box. */
    float dist = sqrtf(dist_sq);
    TydraPhysVec3 local_normal = tp_v3_scale(diff, 1.0f / dist);
    c->normal = tp_xform_vector(*wb, local_normal);
    c->depth = r - dist;
    c->point = tp_xform_point(*wb, closest);
  } else {
    /* Sphere center is inside the box -- find the least penetration axis. */
    float min_pen = FLT_MAX;
    int min_axis = 0;
    float axes_pen[3];
    axes_pen[0] = he.x - tp_absf(local_center.x);
    axes_pen[1] = he.y - tp_absf(local_center.y);
    axes_pen[2] = he.z - tp_absf(local_center.z);
    int k;
    for (k = 0; k < 3; k++) {
      if (axes_pen[k] < min_pen) {
        min_pen = axes_pen[k];
        min_axis = k;
      }
    }

    TydraPhysVec3 local_normal = tp_v3(0, 0, 0);
    float center_comp = 0;
    switch (min_axis) {
      case 0: center_comp = local_center.x; local_normal.x = (center_comp >= 0) ? 1.0f : -1.0f; break;
      case 1: center_comp = local_center.y; local_normal.y = (center_comp >= 0) ? 1.0f : -1.0f; break;
      case 2: center_comp = local_center.z; local_normal.z = (center_comp >= 0) ? 1.0f : -1.0f; break;
    }

    c->normal = tp_xform_vector(*wb, local_normal);
    c->depth = r + min_pen;
    c->point = tp_xform_point(*wb, closest);
  }

  return 1;
}

/* Capsule vs Sphere. */
static int32_t capsule_vs_sphere(const TydraPhysCollider *cc,
                                 const TydraPhysTransform *wc,
                                 const TydraPhysCollider *cs,
                                 const TydraPhysTransform *ws,
                                 TydraPhysContact *contacts,
                                 int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  float cr = cc->shape.data.capsule.radius;
  float ch = cc->shape.data.capsule.half_height;
  float sr = cs->shape.data.sphere.radius;

  /* Capsule segment endpoints in world space. */
  TydraPhysVec3 p0 = tp_xform_point(*wc, tp_v3(0, -ch, 0));
  TydraPhysVec3 p1 = tp_xform_point(*wc, tp_v3(0,  ch, 0));

  /* Closest point on segment to sphere center. */
  TydraPhysVec3 seg = tp_v3_sub(p1, p0);
  float seg_len_sq = tp_v3_length_sq(seg);
  float t = 0.0f;
  if (seg_len_sq > TP_EPSILON) {
    t = tp_v3_dot(tp_v3_sub(ws->position, p0), seg) / seg_len_sq;
    t = tp_clampf(t, 0.0f, 1.0f);
  }
  TydraPhysVec3 closest = tp_v3_add(p0, tp_v3_scale(seg, t));

  /* Now it's a sphere-sphere test between (closest, cr) and (ws->position, sr). */
  TydraPhysVec3 diff = tp_v3_sub(ws->position, closest);
  float dist_sq = tp_v3_length_sq(diff);
  float sum_r = cr + sr;

  if (dist_sq >= sum_r * sum_r) return 0;

  float dist = sqrtf(dist_sq);
  TydraPhysContact *c = &contacts[0];

  if (dist > TP_EPSILON) {
    /* Normal from capsule toward sphere. But convention: normal from B to A.
     * Here capsule=A, sphere=B, so normal from sphere to capsule. */
    c->normal = tp_v3_scale(diff, -1.0f / dist);
  } else {
    c->normal = tp_v3(0, 1, 0);
  }

  c->depth = sum_r - dist;
  c->point = tp_v3_add(closest, tp_v3_scale(tp_v3_negate(c->normal), cr));
  return 1;
}

/* Capsule vs Capsule. */
static int32_t capsule_vs_capsule(const TydraPhysCollider *ca,
                                  const TydraPhysTransform *wa,
                                  const TydraPhysCollider *cb,
                                  const TydraPhysTransform *wb,
                                  TydraPhysContact *contacts,
                                  int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  float ra = ca->shape.data.capsule.radius;
  float ha = ca->shape.data.capsule.half_height;
  float rb = cb->shape.data.capsule.radius;
  float hb = cb->shape.data.capsule.half_height;

  TydraPhysVec3 a0 = tp_xform_point(*wa, tp_v3(0, -ha, 0));
  TydraPhysVec3 a1 = tp_xform_point(*wa, tp_v3(0,  ha, 0));
  TydraPhysVec3 b0 = tp_xform_point(*wb, tp_v3(0, -hb, 0));
  TydraPhysVec3 b1 = tp_xform_point(*wb, tp_v3(0,  hb, 0));

  float s, t;
  closest_points_segments(a0, a1, b0, b1, &s, &t);

  TydraPhysVec3 pa = tp_v3_add(a0, tp_v3_scale(tp_v3_sub(a1, a0), s));
  TydraPhysVec3 pb = tp_v3_add(b0, tp_v3_scale(tp_v3_sub(b1, b0), t));

  TydraPhysVec3 diff = tp_v3_sub(pa, pb);
  float dist_sq = tp_v3_length_sq(diff);
  float sum_r = ra + rb;

  if (dist_sq >= sum_r * sum_r) return 0;

  float dist = sqrtf(dist_sq);
  TydraPhysContact *c = &contacts[0];

  if (dist > TP_EPSILON) {
    c->normal = tp_v3_scale(diff, 1.0f / dist);
  } else {
    c->normal = tp_v3(0, 1, 0);
  }

  c->depth = sum_r - dist;
  c->point = tp_v3_add(pb, tp_v3_scale(c->normal, rb));
  return 1;
}

/* Box vs Plane: project box corners onto plane, generate contacts for penetrating ones. */
static int32_t box_vs_plane(const TydraPhysCollider *cb,
                            const TydraPhysTransform *wb,
                            const TydraPhysCollider *cp,
                            const TydraPhysTransform *wp,
                            TydraPhysContact *contacts,
                            int32_t max_contacts) {
  TydraPhysVec3 he = cb->shape.data.box.half_extents;
  TydraPhysVec3 plane_normal = tp_xform_vector(*wp, cp->shape.data.plane.normal);
  TydraPhysVec3 plane_point  = tp_v3_add(wp->position,
                                          tp_v3_scale(plane_normal, cp->shape.data.plane.offset));

  int32_t count = 0;
  int i;
  for (i = 0; i < 8 && count < max_contacts; i++) {
    float sx = (i & 1) ? he.x : -he.x;
    float sy = (i & 2) ? he.y : -he.y;
    float sz = (i & 4) ? he.z : -he.z;
    TydraPhysVec3 corner = tp_xform_point(*wb, tp_v3(sx, sy, sz));

    float dist = tp_v3_dot(tp_v3_sub(corner, plane_point), plane_normal);
    if (dist < 0.0f) {
      TydraPhysContact *c = &contacts[count];
      c->normal = plane_normal;   /* From plane (B) to box (A). */
      c->depth  = -dist;
      c->point  = corner;
      count++;
    }
  }

  return count;
}

/* Capsule vs Plane. */
static int32_t capsule_vs_plane(const TydraPhysCollider *cc,
                                const TydraPhysTransform *wc,
                                const TydraPhysCollider *cp,
                                const TydraPhysTransform *wp,
                                TydraPhysContact *contacts,
                                int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  float r  = cc->shape.data.capsule.radius;
  float hh = cc->shape.data.capsule.half_height;

  TydraPhysVec3 plane_normal = tp_xform_vector(*wp, cp->shape.data.plane.normal);
  TydraPhysVec3 plane_point  = tp_v3_add(wp->position,
                                          tp_v3_scale(plane_normal, cp->shape.data.plane.offset));

  /* Two sphere endpoints. */
  TydraPhysVec3 p0 = tp_xform_point(*wc, tp_v3(0, -hh, 0));
  TydraPhysVec3 p1 = tp_xform_point(*wc, tp_v3(0,  hh, 0));

  int32_t count = 0;
  TydraPhysVec3 pts[2];
  pts[0] = p0;
  pts[1] = p1;

  int i;
  for (i = 0; i < 2 && count < max_contacts; i++) {
    float dist = tp_v3_dot(tp_v3_sub(pts[i], plane_point), plane_normal);
    float pen = r - dist;
    if (pen > 0.0f) {
      TydraPhysContact *c = &contacts[count];
      c->normal = plane_normal;
      c->depth  = pen;
      c->point  = tp_v3_sub(pts[i], tp_v3_scale(plane_normal, r));
      count++;
    }
  }

  return count;
}

/* Cylinder vs Plane. */
static int32_t cylinder_vs_plane(const TydraPhysCollider *cc,
                                 const TydraPhysTransform *wc,
                                 const TydraPhysCollider *cp,
                                 const TydraPhysTransform *wp,
                                 TydraPhysContact *contacts,
                                 int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  float r  = cc->shape.data.cylinder.radius;
  float hh = cc->shape.data.cylinder.half_height;

  TydraPhysVec3 plane_normal = tp_xform_vector(*wp, cp->shape.data.plane.normal);
  TydraPhysVec3 plane_point  = tp_v3_add(wp->position,
                                          tp_v3_scale(plane_normal, cp->shape.data.plane.offset));

  /* Cylinder axis in world space. */
  TydraPhysVec3 cyl_axis = tp_xform_vector(*wc, tp_v3(0, 1, 0));

  /* Two disk centers. */
  TydraPhysVec3 disk_top    = tp_xform_point(*wc, tp_v3(0,  hh, 0));
  TydraPhysVec3 disk_bottom = tp_xform_point(*wc, tp_v3(0, -hh, 0));

  /* For each disk, the deepest point on the rim in the plane-normal direction.
   * The rim direction is the component of plane_normal perpendicular to cyl_axis. */
  TydraPhysVec3 perp = tp_v3_sub(plane_normal,
                                  tp_v3_scale(cyl_axis, tp_v3_dot(plane_normal, cyl_axis)));
  float perp_len = tp_v3_length(perp);

  int32_t count = 0;
  TydraPhysVec3 disks[2];
  disks[0] = disk_bottom;
  disks[1] = disk_top;

  int i;
  for (i = 0; i < 2 && count < max_contacts; i++) {
    /* Deepest rim point: disk center - normalize(perp) * radius. */
    TydraPhysVec3 rim_pt;
    if (perp_len > TP_EPSILON) {
      rim_pt = tp_v3_sub(disks[i], tp_v3_scale(perp, r / perp_len));
    } else {
      /* Cylinder axis is parallel to plane normal; use disk center. */
      rim_pt = disks[i];
    }

    float dist = tp_v3_dot(tp_v3_sub(rim_pt, plane_point), plane_normal);
    if (dist < 0.0f) {
      TydraPhysContact *c = &contacts[count];
      c->normal = plane_normal;
      c->depth  = -dist;
      c->point  = rim_pt;
      count++;
    }
  }

  return count;
}

/* Box vs box (SAT on oriented boxes) */
static int32_t box_vs_box(const TydraPhysCollider *ca,
                          const TydraPhysTransform *wa,
                          const TydraPhysCollider *cb,
                          const TydraPhysTransform *wb,
                          TydraPhysContact *contacts,
                          int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  TydraPhysVec3 heA = ca->shape.data.box.half_extents;
  TydraPhysVec3 heB = cb->shape.data.box.half_extents;
  TydraPhysVec3 posA = wa->position;
  TydraPhysVec3 posB = wb->position;
  TydraPhysVec3 d = tp_v3_sub(posB, posA);

  /* Get rotation matrices (columns = axes) */
  TydraPhysMat3 rA = tp_m3_from_quat(wa->rotation);
  TydraPhysMat3 rB = tp_m3_from_quat(wb->rotation);

  /* Axes of A and B */
  TydraPhysVec3 axA[3], axB[3];
  axA[0] = tp_v3(rA.m[0], rA.m[3], rA.m[6]);
  axA[1] = tp_v3(rA.m[1], rA.m[4], rA.m[7]);
  axA[2] = tp_v3(rA.m[2], rA.m[5], rA.m[8]);
  axB[0] = tp_v3(rB.m[0], rB.m[3], rB.m[6]);
  axB[1] = tp_v3(rB.m[1], rB.m[4], rB.m[7]);
  axB[2] = tp_v3(rB.m[2], rB.m[5], rB.m[8]);

  float heAf[3] = {heA.x, heA.y, heA.z};
  float heBf[3] = {heB.x, heB.y, heB.z};

  float min_overlap = FLT_MAX;
  TydraPhysVec3 best_axis = tp_v3(0,0,0);
  int best_idx = -1;

  /* Test 6 face axes (3 from A, 3 from B) */
  int ai;
  for (ai = 0; ai < 3; ai++) {
    TydraPhysVec3 axis = axA[ai];
    float projA = heAf[ai];
    float projB = tp_absf(tp_v3_dot(axB[0], axis)) * heBf[0]
                + tp_absf(tp_v3_dot(axB[1], axis)) * heBf[1]
                + tp_absf(tp_v3_dot(axB[2], axis)) * heBf[2];
    float dist = tp_absf(tp_v3_dot(d, axis));
    float overlap = projA + projB - dist;
    if (overlap < 0) return 0; /* Separated */
    if (overlap < min_overlap) {
      min_overlap = overlap;
      best_axis = axis;
      best_idx = ai;
      if (tp_v3_dot(d, axis) < 0) best_axis = tp_v3_negate(best_axis);
    }
  }
  for (ai = 0; ai < 3; ai++) {
    TydraPhysVec3 axis = axB[ai];
    float projA = tp_absf(tp_v3_dot(axA[0], axis)) * heAf[0]
                + tp_absf(tp_v3_dot(axA[1], axis)) * heAf[1]
                + tp_absf(tp_v3_dot(axA[2], axis)) * heAf[2];
    float projB = heBf[ai];
    float dist = tp_absf(tp_v3_dot(d, axis));
    float overlap = projA + projB - dist;
    if (overlap < 0) return 0;
    if (overlap < min_overlap) {
      min_overlap = overlap;
      best_axis = axis;
      best_idx = 3 + ai;
      if (tp_v3_dot(d, axis) < 0) best_axis = tp_v3_negate(best_axis);
    }
  }

  /* Contact: midpoint along separation axis */
  TydraPhysVec3 contact_pt = tp_v3_scale(tp_v3_add(posA, posB), 0.5f);
  contacts[0].point = contact_pt;
  contacts[0].normal = best_axis; /* Points from B to A */
  contacts[0].depth = min_overlap;
  (void)best_idx;
  return 1;
}

/* ======================================================================== */
/* 4. GJK + EPA                                                             */
/* ======================================================================== */

/* GJK support function: farthest point on a shape in a given world direction. */
static TydraPhysVec3 gjk_support(const TydraPhysShape *shape,
                                 const TydraPhysTransform *xform,
                                 TydraPhysVec3 dir) {
  /* Transform direction into local space. */
  TydraPhysTransform inv = tp_xform_inverse(*xform);
  TydraPhysVec3 local_dir = tp_xform_vector(inv, dir);

  TydraPhysVec3 local_pt;

  switch (shape->type) {
    case TYDRA_PHYS_SHAPE_SPHERE: {
      float r = shape->data.sphere.radius;
      TydraPhysVec3 n = tp_v3_normalize(local_dir);
      local_pt = tp_v3_scale(n, r);
      break;
    }

    case TYDRA_PHYS_SHAPE_BOX: {
      TydraPhysVec3 he = shape->data.box.half_extents;
      local_pt.x = (local_dir.x >= 0) ? he.x : -he.x;
      local_pt.y = (local_dir.y >= 0) ? he.y : -he.y;
      local_pt.z = (local_dir.z >= 0) ? he.z : -he.z;
      break;
    }

    case TYDRA_PHYS_SHAPE_CAPSULE: {
      float r  = shape->data.capsule.radius;
      float hh = shape->data.capsule.half_height;
      /* Two sphere supports at endpoints; pick the farther one. */
      TydraPhysVec3 top    = tp_v3(0,  hh, 0);
      TydraPhysVec3 bottom = tp_v3(0, -hh, 0);
      float dt = tp_v3_dot(local_dir, top);
      float db = tp_v3_dot(local_dir, bottom);
      TydraPhysVec3 center = (dt >= db) ? top : bottom;
      TydraPhysVec3 n = tp_v3_normalize(local_dir);
      local_pt = tp_v3_add(center, tp_v3_scale(n, r));
      break;
    }

    case TYDRA_PHYS_SHAPE_CYLINDER: {
      float r  = shape->data.cylinder.radius;
      float hh = shape->data.cylinder.half_height;
      /* Y component: pick top or bottom cap. */
      float y = (local_dir.y >= 0) ? hh : -hh;
      /* Perpendicular direction in XZ plane for circle support. */
      float px = local_dir.x;
      float pz = local_dir.z;
      float perp_len = sqrtf(px * px + pz * pz);
      float cx = 0.0f, cz = 0.0f;
      if (perp_len > TP_EPSILON) {
        cx = (px / perp_len) * r;
        cz = (pz / perp_len) * r;
      }
      local_pt = tp_v3(cx, y, cz);
      break;
    }

    case TYDRA_PHYS_SHAPE_PLANE: {
      /* Plane is not well-defined for GJK support; return a far point. */
      TydraPhysVec3 n = tp_v3_normalize(local_dir);
      local_pt = tp_v3_scale(n, 1e4f);
      break;
    }

  }

  return tp_xform_point(*xform, local_pt);
}

/* Minkowski difference support: support_A(dir) - support_B(-dir). */
static TydraPhysVec3 gjk_support_diff(const TydraPhysShape *sa,
                                      const TydraPhysTransform *xa,
                                      const TydraPhysShape *sb,
                                      const TydraPhysTransform *xb,
                                      TydraPhysVec3 dir,
                                      TydraPhysVec3 *out_sup_a,
                                      TydraPhysVec3 *out_sup_b) {
  *out_sup_a = gjk_support(sa, xa, dir);
  *out_sup_b = gjk_support(sb, xb, tp_v3_negate(dir));
  return tp_v3_sub(*out_sup_a, *out_sup_b);
}

/* GJK simplex vertex: Minkowski difference point and the original supports. */
typedef struct {
  TydraPhysVec3 v;       /* Point in Minkowski difference space */
  TydraPhysVec3 sup_a;   /* Support point on shape A */
  TydraPhysVec3 sup_b;   /* Support point on shape B */
} GjkVertex;

/* GJK simplex (1-4 vertices). */
typedef struct {
  GjkVertex pts[4];
  int count;
} GjkSimplex;

/* Update the simplex and direction. Returns 1 if origin is enclosed. */
static int do_simplex(GjkSimplex *s, TydraPhysVec3 *dir) {
  switch (s->count) {
    case 2: {
      /* Line case. */
      TydraPhysVec3 a = s->pts[1].v;
      TydraPhysVec3 b = s->pts[0].v;
      TydraPhysVec3 ab = tp_v3_sub(b, a);
      TydraPhysVec3 ao = tp_v3_negate(a);

      if (tp_v3_dot(ab, ao) > 0) {
        *dir = tp_v3_cross(tp_v3_cross(ab, ao), ab);
      } else {
        s->pts[0] = s->pts[1];
        s->count = 1;
        *dir = ao;
      }
      return 0;
    }

    case 3: {
      /* Triangle case. */
      TydraPhysVec3 a = s->pts[2].v;
      TydraPhysVec3 b = s->pts[1].v;
      TydraPhysVec3 c = s->pts[0].v;
      TydraPhysVec3 ab = tp_v3_sub(b, a);
      TydraPhysVec3 ac = tp_v3_sub(c, a);
      TydraPhysVec3 ao = tp_v3_negate(a);
      TydraPhysVec3 abc = tp_v3_cross(ab, ac);

      TydraPhysVec3 abc_x_ac = tp_v3_cross(abc, ac);
      if (tp_v3_dot(abc_x_ac, ao) > 0) {
        if (tp_v3_dot(ac, ao) > 0) {
          /* Region AC. */
          s->pts[1] = s->pts[2];
          s->count = 2;
          *dir = tp_v3_cross(tp_v3_cross(ac, ao), ac);
        } else {
          /* Region A or AB. */
          if (tp_v3_dot(ab, ao) > 0) {
            s->pts[0] = s->pts[1];
            s->pts[1] = s->pts[2];
            s->count = 2;
            *dir = tp_v3_cross(tp_v3_cross(ab, ao), ab);
          } else {
            s->pts[0] = s->pts[2];
            s->count = 1;
            *dir = ao;
          }
        }
        return 0;
      }

      TydraPhysVec3 ab_x_abc = tp_v3_cross(ab, abc);
      if (tp_v3_dot(ab_x_abc, ao) > 0) {
        if (tp_v3_dot(ab, ao) > 0) {
          s->pts[0] = s->pts[1];
          s->pts[1] = s->pts[2];
          s->count = 2;
          *dir = tp_v3_cross(tp_v3_cross(ab, ao), ab);
        } else {
          s->pts[0] = s->pts[2];
          s->count = 1;
          *dir = ao;
        }
        return 0;
      }

      /* Origin is above or below the triangle. */
      if (tp_v3_dot(abc, ao) > 0) {
        *dir = abc;
      } else {
        /* Flip winding. */
        GjkVertex tmp = s->pts[0];
        s->pts[0] = s->pts[1];
        s->pts[1] = tmp;
        *dir = tp_v3_negate(abc);
      }
      return 0;
    }

    case 4: {
      /* Tetrahedron case.
       * A = newest point (index 3), B = index 2, C = index 1, D = index 0.
       * Must ensure face normals point outward before testing. */
      TydraPhysVec3 a = s->pts[3].v;
      TydraPhysVec3 b = s->pts[2].v;
      TydraPhysVec3 c = s->pts[1].v;
      TydraPhysVec3 d = s->pts[0].v;
      TydraPhysVec3 ab = tp_v3_sub(b, a);
      TydraPhysVec3 ac = tp_v3_sub(c, a);
      TydraPhysVec3 ad = tp_v3_sub(d, a);
      TydraPhysVec3 ao = tp_v3_negate(a);

      TydraPhysVec3 abc = tp_v3_cross(ab, ac);
      TydraPhysVec3 acd = tp_v3_cross(ac, ad);
      TydraPhysVec3 adb = tp_v3_cross(ad, ab);

      /* Ensure normals point outward:
       * abc should point away from D, acd away from B, adb away from C. */
      if (tp_v3_dot(abc, ad) > 0) abc = tp_v3_negate(abc);
      if (tp_v3_dot(acd, ab) > 0) acd = tp_v3_negate(acd);
      if (tp_v3_dot(adb, ac) > 0) adb = tp_v3_negate(adb);

      /* Check which face the origin is outside of. */
      int over_abc = tp_v3_dot(abc, ao) > 0;
      int over_acd = tp_v3_dot(acd, ao) > 0;
      int over_adb = tp_v3_dot(adb, ao) > 0;

      if (!over_abc && !over_acd && !over_adb) {
        /* Origin is inside the tetrahedron. */
        return 1;
      }

      if (over_abc) {
        /* Remove D (index 0), keep A(3), B(2), C(1). */
        s->pts[0] = s->pts[1];
        s->pts[1] = s->pts[2];
        s->pts[2] = s->pts[3];
        s->count = 3;
        *dir = abc;
      } else if (over_acd) {
        /* Remove B (index 2), keep A(3), C(1), D(0). */
        s->pts[2] = s->pts[3];
        s->count = 3;
        *dir = acd;
      } else {
        /* over_adb: Remove C (index 1), keep A(3), D(0), B(2). */
        s->pts[1] = s->pts[0];
        s->pts[0] = s->pts[2];
        s->pts[2] = s->pts[3];
        s->count = 3;
        *dir = adb;
      }

      /* Recurse once for the triangle case. */
      return do_simplex(s, dir);
    }
  }

  return 0;
}

#define GJK_MAX_ITER 64
#define EPA_MAX_ITER 64
#define EPA_MAX_FACES 128
#define EPA_MAX_VERTS 64
#define EPA_TOLERANCE 1e-4f

/* GJK: returns 1 if shapes intersect, 0 if separated.
 * If intersecting, simplex_out holds the final simplex for EPA. */
static int gjk_intersect(const TydraPhysShape *sa, const TydraPhysTransform *xa,
                         const TydraPhysShape *sb, const TydraPhysTransform *xb,
                         GjkSimplex *simplex_out) {
  TydraPhysVec3 dir = tp_v3_sub(xa->position, xb->position);
  if (tp_v3_length_sq(dir) < TP_EPSILON) {
    dir = tp_v3(1, 0, 0);
  }

  GjkSimplex s;
  s.count = 0;

  TydraPhysVec3 sup_a, sup_b;
  TydraPhysVec3 v = gjk_support_diff(sa, xa, sb, xb, dir, &sup_a, &sup_b);
  s.pts[0].v = v;
  s.pts[0].sup_a = sup_a;
  s.pts[0].sup_b = sup_b;
  s.count = 1;

  dir = tp_v3_negate(v);

  int iter;
  for (iter = 0; iter < GJK_MAX_ITER; iter++) {
    if (tp_v3_length_sq(dir) < TP_EPSILON) {
      /* Degenerate direction; shapes are touching. */
      *simplex_out = s;
      return 1;
    }

    v = gjk_support_diff(sa, xa, sb, xb, dir, &sup_a, &sup_b);

    if (tp_v3_dot(v, dir) < 0) {
      /* New point did not pass the origin; no intersection. */
      return 0;
    }

    s.pts[s.count].v = v;
    s.pts[s.count].sup_a = sup_a;
    s.pts[s.count].sup_b = sup_b;
    s.count++;

    if (do_simplex(&s, &dir)) {
      *simplex_out = s;
      return 1;
    }
  }

  return 0;
}

/* EPA face: indices into the vertex array and precomputed normal. */
typedef struct {
  int a, b, c;
  TydraPhysVec3 normal;
  float dist;
} EpaFace;

/* Compute the outward normal and distance to origin for an EPA face. */
static void epa_face_init(EpaFace *f, const GjkVertex *verts) {
  TydraPhysVec3 ab = tp_v3_sub(verts[f->b].v, verts[f->a].v);
  TydraPhysVec3 ac = tp_v3_sub(verts[f->c].v, verts[f->a].v);
  f->normal = tp_v3_cross(ab, ac);
  float len = tp_v3_length(f->normal);
  if (len > TP_EPSILON) {
    f->normal = tp_v3_scale(f->normal, 1.0f / len);
  } else {
    f->normal = tp_v3(0, 1, 0);
  }
  f->dist = tp_v3_dot(f->normal, verts[f->a].v);
  if (f->dist < 0) {
    /* Flip winding so normal faces away from origin. */
    int tmp = f->b;
    f->b = f->c;
    f->c = tmp;
    f->normal = tp_v3_negate(f->normal);
    f->dist = -f->dist;
  }
}

/* EPA: expand the GJK simplex to find penetration depth and normal. */
static int epa_penetration(const TydraPhysShape *sa, const TydraPhysTransform *xa,
                           const TydraPhysShape *sb, const TydraPhysTransform *xb,
                           const GjkSimplex *simplex,
                           TydraPhysVec3 *out_normal,
                           float *out_depth,
                           TydraPhysVec3 *out_point_a,
                           TydraPhysVec3 *out_point_b) {
  GjkVertex verts[EPA_MAX_VERTS];
  EpaFace faces[EPA_MAX_FACES];
  int num_verts = 0;
  int num_faces = 0;

  /* Initialize with the GJK tetrahedron (must have 4 points). */
  if (simplex->count < 4) {
    /* Shouldn't happen if GJK reported intersection with a full simplex.
     * Fall back to a default. */
    *out_normal = tp_v3(0, 1, 0);
    *out_depth = 0.0f;
    *out_point_a = xa->position;
    *out_point_b = xb->position;
    return 0;
  }

  int i;
  for (i = 0; i < 4; i++) {
    verts[num_verts++] = simplex->pts[i];
  }

  /* Build 4 faces of the tetrahedron. */
  int face_indices[4][3] = {
    {0, 1, 2},
    {0, 3, 1},
    {0, 2, 3},
    {1, 3, 2}
  };

  for (i = 0; i < 4; i++) {
    faces[num_faces].a = face_indices[i][0];
    faces[num_faces].b = face_indices[i][1];
    faces[num_faces].c = face_indices[i][2];
    epa_face_init(&faces[num_faces], verts);
    num_faces++;
  }

  int iter;
  for (iter = 0; iter < EPA_MAX_ITER; iter++) {
    /* Find the closest face to the origin. */
    int closest = 0;
    float min_dist = faces[0].dist;
    for (i = 1; i < num_faces; i++) {
      if (faces[i].dist < min_dist) {
        min_dist = faces[i].dist;
        closest = i;
      }
    }

    TydraPhysVec3 search_dir = faces[closest].normal;

    /* Get support point in this direction. */
    TydraPhysVec3 sup_a, sup_b;
    TydraPhysVec3 new_pt = gjk_support_diff(sa, xa, sb, xb, search_dir,
                                            &sup_a, &sup_b);

    float new_dist = tp_v3_dot(new_pt, search_dir);

    if (new_dist - min_dist < EPA_TOLERANCE) {
      /* Converged. Compute contact from the closest face. */
      *out_normal = faces[closest].normal;
      *out_depth  = min_dist;

      /* Barycentric coordinates on the closest face to project origin. */
      EpaFace *cf = &faces[closest];
      TydraPhysVec3 a_v = verts[cf->a].v;
      TydraPhysVec3 b_v = verts[cf->b].v;
      TydraPhysVec3 c_v = verts[cf->c].v;

      TydraPhysVec3 v0 = tp_v3_sub(b_v, a_v);
      TydraPhysVec3 v1 = tp_v3_sub(c_v, a_v);
      TydraPhysVec3 v2 = tp_v3_negate(a_v); /* origin - a */

      float d00 = tp_v3_dot(v0, v0);
      float d01 = tp_v3_dot(v0, v1);
      float d11 = tp_v3_dot(v1, v1);
      float d20 = tp_v3_dot(v2, v0);
      float d21 = tp_v3_dot(v2, v1);
      float denom = d00 * d11 - d01 * d01;

      float bary_b = 0.33f, bary_c = 0.33f;
      if (tp_absf(denom) > TP_EPSILON) {
        bary_b = (d11 * d20 - d01 * d21) / denom;
        bary_c = (d00 * d21 - d01 * d20) / denom;
      }
      float bary_a = 1.0f - bary_b - bary_c;

      /* Clamp barycentric coords. */
      bary_a = tp_clampf(bary_a, 0.0f, 1.0f);
      bary_b = tp_clampf(bary_b, 0.0f, 1.0f);
      bary_c = tp_clampf(bary_c, 0.0f, 1.0f);
      float bary_sum = bary_a + bary_b + bary_c;
      if (bary_sum > TP_EPSILON) {
        bary_a /= bary_sum;
        bary_b /= bary_sum;
        bary_c /= bary_sum;
      }

      *out_point_a = tp_v3_add(
          tp_v3_add(tp_v3_scale(verts[cf->a].sup_a, bary_a),
                    tp_v3_scale(verts[cf->b].sup_a, bary_b)),
          tp_v3_scale(verts[cf->c].sup_a, bary_c));

      *out_point_b = tp_v3_add(
          tp_v3_add(tp_v3_scale(verts[cf->a].sup_b, bary_a),
                    tp_v3_scale(verts[cf->b].sup_b, bary_b)),
          tp_v3_scale(verts[cf->c].sup_b, bary_c));

      return 1;
    }

    /* Not converged: add new vertex and rebuild faces. */
    if (num_verts >= EPA_MAX_VERTS) break;

    int new_idx = num_verts;
    verts[new_idx].v = new_pt;
    verts[new_idx].sup_a = sup_a;
    verts[new_idx].sup_b = sup_b;
    num_verts++;

    /* Remove faces visible from the new point and collect horizon edges. */
    typedef struct { int a, b; } Edge;
    Edge edges[EPA_MAX_FACES * 3];
    int num_edges = 0;

    for (i = num_faces - 1; i >= 0; i--) {
      if (tp_v3_dot(faces[i].normal,
                     tp_v3_sub(new_pt, verts[faces[i].a].v)) > 0) {
        /* This face is visible. Add its edges. */
        int fa = faces[i].a, fb = faces[i].b, fc = faces[i].c;

        /* Check if edge already exists (shared with another visible face). */
        int e_idx;
        /* Edge AB */
        int found = 0;
        for (e_idx = 0; e_idx < num_edges; e_idx++) {
          if ((edges[e_idx].a == fb && edges[e_idx].b == fa) ||
              (edges[e_idx].a == fa && edges[e_idx].b == fb)) {
            /* Remove this edge (shared, not on horizon). */
            edges[e_idx] = edges[num_edges - 1];
            num_edges--;
            found = 1;
            break;
          }
        }
        if (!found && num_edges < EPA_MAX_FACES * 3) {
          edges[num_edges].a = fa;
          edges[num_edges].b = fb;
          num_edges++;
        }

        /* Edge BC */
        found = 0;
        for (e_idx = 0; e_idx < num_edges; e_idx++) {
          if ((edges[e_idx].a == fc && edges[e_idx].b == fb) ||
              (edges[e_idx].a == fb && edges[e_idx].b == fc)) {
            edges[e_idx] = edges[num_edges - 1];
            num_edges--;
            found = 1;
            break;
          }
        }
        if (!found && num_edges < EPA_MAX_FACES * 3) {
          edges[num_edges].a = fb;
          edges[num_edges].b = fc;
          num_edges++;
        }

        /* Edge CA */
        found = 0;
        for (e_idx = 0; e_idx < num_edges; e_idx++) {
          if ((edges[e_idx].a == fa && edges[e_idx].b == fc) ||
              (edges[e_idx].a == fc && edges[e_idx].b == fa)) {
            edges[e_idx] = edges[num_edges - 1];
            num_edges--;
            found = 1;
            break;
          }
        }
        if (!found && num_edges < EPA_MAX_FACES * 3) {
          edges[num_edges].a = fc;
          edges[num_edges].b = fa;
          num_edges++;
        }

        /* Remove this face. */
        faces[i] = faces[num_faces - 1];
        num_faces--;
      }
    }

    /* Create new faces from horizon edges to the new vertex. */
    for (i = 0; i < num_edges; i++) {
      if (num_faces >= EPA_MAX_FACES) break;
      faces[num_faces].a = edges[i].a;
      faces[num_faces].b = edges[i].b;
      faces[num_faces].c = new_idx;
      epa_face_init(&faces[num_faces], verts);
      num_faces++;
    }
  }

  /* Did not converge; return best guess from closest face. */
  if (num_faces > 0) {
    int closest = 0;
    float min_dist = faces[0].dist;
    for (i = 1; i < num_faces; i++) {
      if (faces[i].dist < min_dist) {
        min_dist = faces[i].dist;
        closest = i;
      }
    }
    *out_normal = faces[closest].normal;
    *out_depth = min_dist;
    *out_point_a = xa->position;
    *out_point_b = xb->position;
    return 1;
  }

  *out_normal = tp_v3(0, 1, 0);
  *out_depth = 0.0f;
  *out_point_a = xa->position;
  *out_point_b = xb->position;
  return 0;
}

/* GJK distance: returns the distance between two separated shapes.
 * If overlapping, returns 0. Also returns closest points. */
static float gjk_distance(const TydraPhysShape *sa, const TydraPhysTransform *xa,
                          const TydraPhysShape *sb, const TydraPhysTransform *xb,
                          TydraPhysVec3 *out_closest_a,
                          TydraPhysVec3 *out_closest_b) {
  GjkSimplex simplex;
  if (gjk_intersect(sa, xa, sb, xb, &simplex)) {
    *out_closest_a = xa->position;
    *out_closest_b = xb->position;
    return 0.0f;
  }

  /* After GJK terminates without intersection, the simplex contains the
   * closest feature. For simplicity, use support in the separation direction. */
  TydraPhysVec3 dir = tp_v3_sub(xa->position, xb->position);
  if (tp_v3_length_sq(dir) < TP_EPSILON) dir = tp_v3(1, 0, 0);
  dir = tp_v3_normalize(dir);

  *out_closest_a = gjk_support(sa, xa, tp_v3_negate(dir));
  *out_closest_b = gjk_support(sb, xb, dir);

  float dist = tp_v3_length(tp_v3_sub(*out_closest_a, *out_closest_b));
  return dist;
}

/* General GJK+EPA narrow phase for arbitrary convex shape pairs. */
static int32_t gjk_epa_contacts(const TydraPhysCollider *ca,
                                const TydraPhysTransform *wa,
                                const TydraPhysCollider *cb,
                                const TydraPhysTransform *wb,
                                TydraPhysContact *contacts,
                                int32_t max_contacts) {
  if (max_contacts < 1) return 0;

  GjkSimplex simplex;
  if (!gjk_intersect(&ca->shape, wa, &cb->shape, wb, &simplex))
    return 0;

  TydraPhysVec3 normal, point_a, point_b;
  float depth;

  if (!epa_penetration(&ca->shape, wa, &cb->shape, wb, &simplex,
                       &normal, &depth, &point_a, &point_b))
    return 0;

  TydraPhysContact *c = &contacts[0];
  c->normal = normal;
  c->depth  = depth;
  c->point  = tp_v3_scale(tp_v3_add(point_a, point_b), 0.5f);
  return 1;
}

/* ======================================================================== */
/* 5. Narrow phase dispatch                                                 */
/* ======================================================================== */

/* Ordered shape type pair for dispatch (lower type first). */
#define SHAPE_PAIR(a, b) ((static_cast<int>(a) << 8) | static_cast<int>(b))

int32_t tydra_phys_narrow_phase(const TydraPhysCollider *col_a,
                                const TydraPhysTransform *xform_a,
                                const TydraPhysCollider *col_b,
                                const TydraPhysTransform *xform_b,
                                TydraPhysContact *contacts,
                                int32_t max_contacts) {
  if (!col_a || !col_b || !xform_a || !xform_b || !contacts)
    return TYDRA_PHYS_COL_ERR_NULL_INPUT;
  if (max_contacts <= 0) return 0;

  TydraPhysTransform wa = collider_world_xform(col_a, xform_a);
  TydraPhysTransform wb = collider_world_xform(col_b, xform_b);

  TydraPhysShapeType ta = col_a->shape.type;
  TydraPhysShapeType tb = col_b->shape.type;

  int32_t num = 0;
  int swapped = 0;

  /* Ensure ta <= tb for canonical ordering. */
  const TydraPhysCollider *ca = col_a;
  const TydraPhysCollider *cb = col_b;
  const TydraPhysTransform *pwa = &wa;
  const TydraPhysTransform *pwb = &wb;

  if (ta > tb) {
    /* Swap. */
    const TydraPhysCollider *tmp_c = ca; ca = cb; cb = tmp_c;
    TydraPhysTransform tmp_w = wa; wa = wb; wb = tmp_w;
    pwa = &wa; pwb = &wb;
    TydraPhysShapeType tmp_t = ta; ta = tb; tb = tmp_t;
    swapped = 1;
  }

  int pair = SHAPE_PAIR(ta, tb);

  switch (pair) {
    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_SPHERE, TYDRA_PHYS_SHAPE_SPHERE):
      num = sphere_vs_sphere(ca, pwa, cb, pwb, contacts, max_contacts);
      break;

    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_SPHERE, TYDRA_PHYS_SHAPE_BOX):
      num = sphere_vs_box(ca, pwa, cb, pwb, contacts, max_contacts);
      break;

    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_SPHERE, TYDRA_PHYS_SHAPE_CAPSULE):
      /* Capsule is B, sphere is A. Our capsule_vs_sphere expects capsule first. */
      num = capsule_vs_sphere(cb, pwb, ca, pwa, contacts, max_contacts);
      /* Flip normal since we swapped A/B in the call. */
      { int k; for (k = 0; k < num; k++) contacts[k].normal = tp_v3_negate(contacts[k].normal); }
      break;

    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_SPHERE, TYDRA_PHYS_SHAPE_PLANE):
      num = sphere_vs_plane(ca, pwa, cb, pwb, contacts, max_contacts);
      break;

    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_BOX, TYDRA_PHYS_SHAPE_BOX):
      num = box_vs_box(ca, pwa, cb, pwb, contacts, max_contacts);
      break;

    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_BOX, TYDRA_PHYS_SHAPE_PLANE):
      num = box_vs_plane(ca, pwa, cb, pwb, contacts, max_contacts);
      break;

    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_CAPSULE, TYDRA_PHYS_SHAPE_CAPSULE):
      num = capsule_vs_capsule(ca, pwa, cb, pwb, contacts, max_contacts);
      break;

    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_CAPSULE, TYDRA_PHYS_SHAPE_PLANE):
      num = capsule_vs_plane(ca, pwa, cb, pwb, contacts, max_contacts);
      break;

    case SHAPE_PAIR(TYDRA_PHYS_SHAPE_CYLINDER, TYDRA_PHYS_SHAPE_PLANE):
      num = cylinder_vs_plane(ca, pwa, cb, pwb, contacts, max_contacts);
      break;

    default:
      /* Fall back to GJK + EPA for general pairs (box-box, etc.). */
      num = gjk_epa_contacts(ca, pwa, cb, pwb, contacts, max_contacts);
      break;
  }

  /* If we swapped shapes, flip the normals so they point from original B to A. */
  if (swapped && num > 0) {
    int k;
    for (k = 0; k < num; k++) {
      contacts[k].normal = tp_v3_negate(contacts[k].normal);
    }
  }

  /* Fill metadata on all generated contacts. */
  {
    int k;
    for (k = 0; k < num; k++) {
      fill_contact_meta(&contacts[k], col_a, -1, col_b, -1);
    }
  }

  return num;
}

/* ======================================================================== */
/* 6. CCD (Continuous Collision Detection)                                  */
/* ======================================================================== */

#define CCD_MAX_ITER  32
#define CCD_MARGIN    1e-4f

float tydra_phys_ccd_toi(const TydraPhysCollider *col,
                         const TydraPhysTransform *start,
                         const TydraPhysTransform *end,
                         const TydraPhysCollider *target,
                         const TydraPhysTransform *target_xform) {
  if (!col || !start || !end || !target || !target_xform) return 1.0f;

  /* Conservative advancement: advance time by distance / relative_velocity. */
  float t = 0.0f;

  /* Compute maximum linear displacement of the sweeping body. */
  TydraPhysVec3 disp = tp_v3_sub(end->position, start->position);
  float max_linear_vel = tp_v3_length(disp);

  /* Add rotational contribution (approximate max surface velocity). */
  TydraPhysVec3 rot_axis;
  TydraPhysQuat dq = tp_q_mul(end->rotation, tp_q_conjugate(start->rotation));
  float rot_angle = tp_q_to_axis_angle(dq, &rot_axis);

  /* Estimate max radius of the shape for rotational velocity. */
  float shape_radius = 0.0f;
  switch (col->shape.type) {
    case TYDRA_PHYS_SHAPE_SPHERE:
      shape_radius = col->shape.data.sphere.radius;
      break;
    case TYDRA_PHYS_SHAPE_BOX: {
      TydraPhysVec3 he = col->shape.data.box.half_extents;
      shape_radius = tp_v3_length(he);
      break;
    }
    case TYDRA_PHYS_SHAPE_CAPSULE:
      shape_radius = col->shape.data.capsule.half_height +
                     col->shape.data.capsule.radius;
      break;
    case TYDRA_PHYS_SHAPE_CYLINDER:
      shape_radius = sqrtf(col->shape.data.cylinder.half_height *
                           col->shape.data.cylinder.half_height +
                           col->shape.data.cylinder.radius *
                           col->shape.data.cylinder.radius);
      break;
    case TYDRA_PHYS_SHAPE_PLANE:
      return 1.0f; /* Planes don't sweep. */
  }

  float max_rot_vel = rot_angle * shape_radius;
  float max_vel = max_linear_vel + max_rot_vel;

  if (max_vel < TP_EPSILON) {
    /* No movement; check if already overlapping. */
    TydraPhysVec3 ca, cb;
    TydraPhysTransform ws = collider_world_xform(col, start);
    TydraPhysTransform wt = collider_world_xform(target, target_xform);
    float dist = gjk_distance(&col->shape, &ws, &target->shape, &wt, &ca, &cb);
    return (dist < CCD_MARGIN) ? 0.0f : 1.0f;
  }

  int iter;
  for (iter = 0; iter < CCD_MAX_ITER; iter++) {
    /* Interpolate transform at time t. */
    TydraPhysTransform current;
    current.position = tp_v3_lerp(start->position, end->position, t);
    current.rotation = tp_q_slerp(start->rotation, end->rotation, t);

    TydraPhysTransform wc = collider_world_xform(col, &current);
    TydraPhysTransform wt = collider_world_xform(target, target_xform);

    TydraPhysVec3 ca, cb;
    float dist = gjk_distance(&col->shape, &wc, &target->shape, &wt, &ca, &cb);

    if (dist < CCD_MARGIN) {
      return t;
    }

    /* Advance conservatively. */
    float dt = dist / max_vel;
    t += dt;

    if (t >= 1.0f) return 1.0f;
  }

  return 1.0f;
}
