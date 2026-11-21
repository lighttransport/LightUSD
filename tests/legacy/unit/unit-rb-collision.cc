// SPDX-License-Identifier: Apache-2.0
// Collision detection unit tests.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-rb-collision.h"
#include "tydra/rb-collision.h"

#include <cmath>
#include <cstring>

// Helper: make identity transform
static TydraPhysTransform xform_id(void) {
  TydraPhysTransform x;
  x.position = tp_v3(0, 0, 0);
  x.rotation = tp_q_identity();
  return x;
}

static TydraPhysTransform xform_at(float x, float y, float z) {
  TydraPhysTransform t;
  t.position = tp_v3(x, y, z);
  t.rotation = tp_q_identity();
  return t;
}

static TydraPhysCollider make_sphere_collider(float radius, int body) {
  TydraPhysCollider c;
  memset(&c, 0, sizeof(c));
  c.shape.type = TYDRA_PHYS_SHAPE_SPHERE;
  c.shape.data.sphere.radius = radius;
  c.local_pose = xform_id();
  c.static_friction = 0.5f;
  c.dynamic_friction = 0.5f;
  c.restitution = 0.3f;
  c.collision_group = 1;
  c.collision_mask = 0xFFFFFFFF;
  c.body_index = body;
  return c;
}

static TydraPhysCollider make_plane_collider(int body) {
  TydraPhysCollider c;
  memset(&c, 0, sizeof(c));
  c.shape.type = TYDRA_PHYS_SHAPE_PLANE;
  c.shape.data.plane.normal = tp_v3(0, 0, 1);
  c.shape.data.plane.offset = 0;
  c.local_pose = xform_id();
  c.static_friction = 0.5f;
  c.dynamic_friction = 0.5f;
  c.restitution = 0.3f;
  c.collision_group = 1;
  c.collision_mask = 0xFFFFFFFF;
  c.body_index = body;
  return c;
}

static TydraPhysCollider make_box_collider(float hx, float hy, float hz, int body) {
  TydraPhysCollider c;
  memset(&c, 0, sizeof(c));
  c.shape.type = TYDRA_PHYS_SHAPE_BOX;
  c.shape.data.box.half_extents = tp_v3(hx, hy, hz);
  c.local_pose = xform_id();
  c.static_friction = 0.5f;
  c.dynamic_friction = 0.5f;
  c.restitution = 0.3f;
  c.collision_group = 1;
  c.collision_mask = 0xFFFFFFFF;
  c.body_index = body;
  return c;
}

// -----------------------------------------------------------------------
// 1. Sphere vs sphere
// -----------------------------------------------------------------------
void rb_sphere_sphere_test(void) {
  TydraPhysCollider a = make_sphere_collider(1.0f, 0);
  TydraPhysCollider b = make_sphere_collider(1.0f, 1);

  // Overlapping: centers 1.5 apart, radii sum = 2
  TydraPhysTransform xa = xform_at(0, 0, 0);
  TydraPhysTransform xb = xform_at(1.5f, 0, 0);

  TydraPhysContact contacts[4];
  int n = tydra_phys_narrow_phase(&a, &xa, &b, &xb, contacts, 4);
  TEST_CHECK(n == 1);
  TEST_CHECK(contacts[0].depth > 0);
  TEST_MSG("depth = %f", contacts[0].depth);
  TEST_CHECK(fabsf(contacts[0].depth - 0.5f) < 0.01f);

  // Separated: centers 3.0 apart
  xb = xform_at(3.0f, 0, 0);
  n = tydra_phys_narrow_phase(&a, &xa, &b, &xb, contacts, 4);
  TEST_CHECK(n == 0);
}

// -----------------------------------------------------------------------
// 2. Sphere vs plane
// -----------------------------------------------------------------------
void rb_sphere_plane_test(void) {
  TydraPhysCollider sphere = make_sphere_collider(1.0f, 0);
  TydraPhysCollider plane = make_plane_collider(1);

  // Sphere at z=0.5, radius=1 → penetrating plane at z=0 by 0.5
  TydraPhysTransform xs = xform_at(0, 0, 0.5f);
  TydraPhysTransform xp = xform_id();

  TydraPhysContact contacts[4];
  int n = tydra_phys_narrow_phase(&sphere, &xs, &plane, &xp, contacts, 4);
  TEST_CHECK(n == 1);
  if (n > 0) {
    TEST_CHECK(contacts[0].depth > 0);
    TEST_MSG("depth = %f, normal = (%f,%f,%f)", contacts[0].depth,
             contacts[0].normal.x, contacts[0].normal.y, contacts[0].normal.z);
  }

  // Sphere above plane, no contact
  xs = xform_at(0, 0, 2.0f);
  n = tydra_phys_narrow_phase(&sphere, &xs, &plane, &xp, contacts, 4);
  TEST_CHECK(n == 0);
}

// -----------------------------------------------------------------------
// 3. Sphere vs box
// -----------------------------------------------------------------------
void rb_sphere_box_test(void) {
  TydraPhysCollider sphere = make_sphere_collider(0.5f, 0);
  TydraPhysCollider box = make_box_collider(1, 1, 1, 1);

  // Sphere at (1.3, 0, 0), box half-extents (1,1,1) centered at origin
  // Closest box face at x=1, distance = 0.3, sphere radius = 0.5 → overlap 0.2
  TydraPhysTransform xs = xform_at(1.3f, 0, 0);
  TydraPhysTransform xb = xform_id();

  TydraPhysContact contacts[4];
  int n = tydra_phys_narrow_phase(&sphere, &xs, &box, &xb, contacts, 4);
  TEST_CHECK(n >= 1);
  if (n > 0) {
    TEST_CHECK(contacts[0].depth > 0);
    TEST_MSG("sphere-box depth = %f", contacts[0].depth);
  }

  // Sphere far away
  xs = xform_at(5, 0, 0);
  n = tydra_phys_narrow_phase(&sphere, &xs, &box, &xb, contacts, 4);
  TEST_CHECK(n == 0);
}

// -----------------------------------------------------------------------
// 4. Capsule vs sphere
// -----------------------------------------------------------------------
void rb_capsule_sphere_test(void) {
  TydraPhysCollider capsule;
  memset(&capsule, 0, sizeof(capsule));
  capsule.shape.type = TYDRA_PHYS_SHAPE_CAPSULE;
  capsule.shape.data.capsule.radius = 0.3f;
  capsule.shape.data.capsule.half_height = 1.0f;
  capsule.local_pose = xform_id();
  capsule.collision_group = 1;
  capsule.collision_mask = 0xFFFFFFFF;
  capsule.body_index = 0;

  TydraPhysCollider sphere = make_sphere_collider(0.5f, 1);

  // Capsule vertical (Y-axis), sphere near the top
  TydraPhysTransform xc = xform_id();
  TydraPhysTransform xs = xform_at(0, 1.5f, 0);

  TydraPhysContact contacts[4];
  int n = tydra_phys_narrow_phase(&capsule, &xc, &sphere, &xs, contacts, 4);
  // Capsule top at y=1.0 (half_height), sphere at y=1.5, radii sum = 0.8
  // Distance center-to-cap-top = 0.5, overlap = 0.3
  TEST_CHECK(n >= 1);
  if (n > 0) {
    TEST_MSG("capsule-sphere depth = %f", contacts[0].depth);
    TEST_CHECK(contacts[0].depth > 0);
  }
}

// -----------------------------------------------------------------------
// 5. AABB broadphase
// -----------------------------------------------------------------------
void rb_aabb_broadphase_test(void) {
  TydraPhysBroadphase bp;
  TydraPhysAABB aabbs[4];
  TydraPhysCollisionPair pairs[16];
  int32_t sorted[4];

  tydra_phys_broadphase_init(&bp, 4, aabbs, pairs, 16, sorted);

  // 3 sphere colliders, first two overlap, third is far
  TydraPhysCollider cols[3];
  cols[0] = make_sphere_collider(1.0f, 0);
  cols[1] = make_sphere_collider(1.0f, 1);
  cols[2] = make_sphere_collider(1.0f, 2);

  TydraPhysTransform xforms[3];
  xforms[0] = xform_at(0, 0, 0);
  xforms[1] = xform_at(1.5f, 0, 0);
  xforms[2] = xform_at(10, 0, 0);

  // Compute AABBs
  for (int i = 0; i < 3; i++) {
    tydra_phys_collider_aabb(&cols[i], &xforms[i], &aabbs[i]);
  }
  bp.num_aabbs = 3;

  int32_t np = tydra_phys_broadphase_update(&bp, cols, xforms, 3);
  TEST_MSG("broadphase pairs: %d", np);
  // Should find pair (0,1) but not (0,2) or (1,2)
  TEST_CHECK(np >= 1);
  TEST_CHECK(np <= 2);  // at most 1-2 pairs depending on AABB extent
}

// -----------------------------------------------------------------------
// 6. GJK box vs box
// -----------------------------------------------------------------------
void rb_gjk_box_box_test(void) {
  TydraPhysCollider a = make_box_collider(0.5f, 0.5f, 0.5f, 0);
  TydraPhysCollider b = make_box_collider(0.5f, 0.5f, 0.5f, 1);

  // Overlapping: boxes at (0,0,0) and (0.8, 0, 0), each half-extent 0.5
  TydraPhysTransform xa = xform_id();
  TydraPhysTransform xb = xform_at(0.8f, 0, 0);

  TydraPhysContact contacts[4];
  int n = tydra_phys_narrow_phase(&a, &xa, &b, &xb, contacts, 4);
  TEST_CHECK(n >= 1);
  if (n > 0) {
    TEST_CHECK(contacts[0].depth > 0);
    TEST_MSG("box-box depth = %f", contacts[0].depth);
  }

  // Separated
  xb = xform_at(2.0f, 0, 0);
  n = tydra_phys_narrow_phase(&a, &xa, &b, &xb, contacts, 4);
  TEST_CHECK(n == 0);

  // Rotated box: B at 45 deg around Z, overlapping
  xb.position = tp_v3(0.9f, 0, 0);
  xb.rotation = tp_q_from_axis_angle(tp_v3(0, 0, 1), 3.14159f / 4.0f);
  n = tydra_phys_narrow_phase(&a, &xa, &b, &xb, contacts, 4);
  TEST_CHECK(n >= 1);
  if (n > 0) {
    TEST_CHECK(contacts[0].depth > 0);
    TEST_MSG("rotated box-box depth = %f", contacts[0].depth);
  }

  // Rotated box: separated
  xb.position = tp_v3(1.6f, 0, 0);
  n = tydra_phys_narrow_phase(&a, &xa, &b, &xb, contacts, 4);
  TEST_CHECK(n == 0);
}
