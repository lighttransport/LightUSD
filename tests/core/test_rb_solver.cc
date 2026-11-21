// SPDX-License-Identifier: Apache-2.0
// Core regression for the shared numerical collision/dynamics solver.
#include <iostream>
static int failures = 0;
#define TEST_CHECK(x)                        \
  do {                                       \
    if (!(x)) {                              \
      ++failures;                            \
      std::cerr << __LINE__ << ": " #x "\n"; \
    }                                        \
  } while (0)
#define TEST_ASSERT(x) \
  do {                 \
    if (!(x)) {        \
      TEST_CHECK(x);   \
      return;          \
    }                  \
  } while (0)
#define TEST_MSG(...) \
  do {                \
  } while (0)
// SPDX-License-Identifier: Apache-2.0
// Collision detection unit tests.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#include <cmath>
#include <cstring>

#include "tydra/rb-collision.h"

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

static TydraPhysCollider make_box_collider(float hx, float hy, float hz,
                                           int body) {
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
// SPDX-License-Identifier: Apache-2.0
// Rigid body dynamics unit tests.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#include <cmath>
#include <cstring>

#include "tydra/rb-dynamics.h"

// Max buffer sizes for tests
#define MAX_BODIES 32
#define MAX_COLLIDERS 64
#define MAX_JOINTS 16
#define MAX_CONTACTS 128
#define MAX_PAIRS 256
#define MAX_ISLANDS 16

// Helper: allocate all world buffers on the stack
struct TestWorldBuffers {
  TydraPhysBody bodies[MAX_BODIES];
  TydraPhysCollider colliders[MAX_COLLIDERS];
  TydraPhysJoint joints[MAX_JOINTS];
  TydraPhysContact contacts[MAX_CONTACTS];
  TydraPhysAABB aabbs[MAX_COLLIDERS];
  TydraPhysCollisionPair pairs[MAX_PAIRS];
  int32_t sorted[MAX_COLLIDERS];
  TydraPhysIsland islands[MAX_ISLANDS];
  int32_t union_buf[MAX_BODIES];
  int32_t island_body_buf[MAX_BODIES * 2];
};

static void init_test_world(TydraPhysWorld* world, TestWorldBuffers* buf) {
  memset(buf, 0, sizeof(*buf));
  tydra_phys_world_init(world, buf->bodies, MAX_BODIES, buf->colliders,
                        MAX_COLLIDERS, buf->joints, MAX_JOINTS, buf->contacts,
                        MAX_CONTACTS, buf->aabbs, buf->pairs, MAX_PAIRS,
                        buf->sorted, buf->islands, MAX_ISLANDS, buf->union_buf,
                        buf->island_body_buf);
  tydra_phys_world_defaults(world);
}

void rb_invalid_body_indices_rejected_test(void) {
  TydraPhysWorld world;
  TestWorldBuffers buf;
  init_test_world(&world, &buf);

  TydraPhysBody body;
  tydra_phys_body_default(&body);
  int body_idx = tydra_phys_add_body(&world, &body);
  TEST_CHECK(body_idx == 0);

  TydraPhysCollider col;
  memset(&col, 0, sizeof(col));
  col.shape.type = TYDRA_PHYS_SHAPE_SPHERE;
  col.shape.data.sphere.radius = 1.0f;
  col.local_pose = tp_xform_identity();
  col.collision_group = 1;
  col.collision_mask = 1;

  col.body_index = world.num_bodies;
  TEST_CHECK(tydra_phys_add_collider(&world, &col) == -1);

  col.body_index = -1;
  TEST_CHECK(tydra_phys_add_collider(&world, &col) >= 0);

  TydraPhysJoint joint;
  memset(&joint, 0, sizeof(joint));
  joint.type = TYDRA_PHYS_JOINT_BALL;
  joint.body_a = 0;
  joint.body_b = world.num_bodies;
  TEST_CHECK(tydra_phys_add_joint(&world, &joint) == -1);

  joint.body_b = -1;
  TEST_CHECK(tydra_phys_add_joint(&world, &joint) >= 0);
}

// -----------------------------------------------------------------------
// 1. Falling sphere: sphere should fall under gravity onto a plane
// -----------------------------------------------------------------------
void rb_falling_sphere_test(void) {
  TydraPhysWorld world;
  TestWorldBuffers buf;
  init_test_world(&world, &buf);

  world.gravity = tp_v3(0, 0, -9.81f);
  world.timestep = 1.0f / 60.0f;

  // Static ground plane (body 0)
  TydraPhysBody ground;
  tydra_phys_body_default(&ground);
  ground.body_type = TYDRA_PHYS_BODY_STATIC;
  ground.inverse_mass = 0;
  ground.xform.position = tp_v3(0, 0, 0);
  ground.xform.rotation = tp_q_identity();
  ground.flags = 0;
  int gi = tydra_phys_add_body(&world, &ground);
  TEST_CHECK(gi == 0);

  TydraPhysCollider ground_col;
  memset(&ground_col, 0, sizeof(ground_col));
  ground_col.shape.type = TYDRA_PHYS_SHAPE_PLANE;
  ground_col.shape.data.plane.normal = tp_v3(0, 0, 1);
  ground_col.shape.data.plane.offset = 0;
  ground_col.local_pose.position = tp_v3(0, 0, 0);
  ground_col.local_pose.rotation = tp_q_identity();
  ground_col.static_friction = 0.5f;
  ground_col.dynamic_friction = 0.5f;
  ground_col.restitution = 0.3f;
  ground_col.collision_group = 1;
  ground_col.collision_mask = 0xFFFFFFFF;
  ground_col.body_index = gi;
  tydra_phys_add_collider(&world, &ground_col);

  // Dynamic sphere (body 1) at z = 5
  TydraPhysBody sphere;
  tydra_phys_body_default(&sphere);
  sphere.body_type = TYDRA_PHYS_BODY_DYNAMIC;
  sphere.inverse_mass = 1.0f;  // mass = 1
  sphere.local_inv_inertia =
      tp_v3(2.5f, 2.5f, 2.5f);  // solid sphere I = 2/5*m*r^2, r=1
  sphere.xform.position = tp_v3(0, 0, 5);
  sphere.xform.rotation = tp_q_identity();
  sphere.flags = TYDRA_PHYS_BODY_FLAG_GRAVITY | TYDRA_PHYS_BODY_FLAG_CAN_SLEEP;
  int si = tydra_phys_add_body(&world, &sphere);
  TEST_CHECK(si == 1);

  TydraPhysCollider sphere_col;
  memset(&sphere_col, 0, sizeof(sphere_col));
  sphere_col.shape.type = TYDRA_PHYS_SHAPE_SPHERE;
  sphere_col.shape.data.sphere.radius = 1.0f;
  sphere_col.local_pose.position = tp_v3(0, 0, 0);
  sphere_col.local_pose.rotation = tp_q_identity();
  sphere_col.static_friction = 0.5f;
  sphere_col.dynamic_friction = 0.5f;
  sphere_col.restitution = 0.3f;
  sphere_col.collision_group = 1;
  sphere_col.collision_mask = 0xFFFFFFFF;
  sphere_col.body_index = si;
  tydra_phys_add_collider(&world, &sphere_col);

  // Step 120 frames (2 seconds) — sphere should fall and rest on plane at z ≈ 1
  // (radius)
  for (int i = 0; i < 120; i++) {
    TydraPhysResult r = tydra_phys_step(&world);
    TEST_CHECK(r == TYDRA_PHYS_OK);
  }

  float final_z = world.bodies[1].xform.position.z;
  TEST_MSG("Final sphere z = %f (expected ~1.0)", final_z);
  // Should be near z=1 (resting on plane, radius=1)
  TEST_CHECK(final_z > 0.5f);
  TEST_CHECK(final_z < 2.0f);
}

// -----------------------------------------------------------------------
// 2. Pendulum: hinge joint should constrain bodies
// -----------------------------------------------------------------------
void rb_pendulum_hinge_test(void) {
  TydraPhysWorld world;
  TestWorldBuffers buf;
  init_test_world(&world, &buf);

  world.gravity = tp_v3(0, 0, -9.81f);
  world.timestep = 1.0f / 60.0f;

  // Body 0: static anchor at (0, 0, 5)
  TydraPhysBody anchor;
  tydra_phys_body_default(&anchor);
  anchor.body_type = TYDRA_PHYS_BODY_STATIC;
  anchor.inverse_mass = 0;
  anchor.xform.position = tp_v3(0, 0, 5);
  anchor.flags = 0;
  tydra_phys_add_body(&world, &anchor);

  // Body 1: dynamic bob at (2, 0, 5) — offset to side for swing
  TydraPhysBody bob;
  tydra_phys_body_default(&bob);
  bob.body_type = TYDRA_PHYS_BODY_DYNAMIC;
  bob.inverse_mass = 1.0f;
  bob.local_inv_inertia = tp_v3(1, 1, 1);
  bob.xform.position = tp_v3(2, 0, 5);
  bob.flags = TYDRA_PHYS_BODY_FLAG_GRAVITY;
  tydra_phys_add_body(&world, &bob);

  // No colliders needed for this test

  // Ball joint connecting anchor to bob (distance = 2)
  TydraPhysJoint joint;
  memset(&joint, 0, sizeof(joint));
  joint.type = TYDRA_PHYS_JOINT_BALL;
  joint.body_a = 0;
  joint.body_b = 1;
  joint.local_anchor_a.position = tp_v3(0, 0, 0);
  joint.local_anchor_a.rotation = tp_q_identity();
  joint.local_anchor_b.position = tp_v3(-2, 0, 0);  // relative to bob
  joint.local_anchor_b.rotation = tp_q_identity();
  tydra_phys_add_joint(&world, &joint);

  // Step 180 frames (3 seconds)
  for (int i = 0; i < 180; i++) {
    tydra_phys_step(&world);
  }

  // Bob should have swung down (z < 5) and stayed approximately 2 units from
  // anchor
  float bx = world.bodies[1].xform.position.x;
  float bz = world.bodies[1].xform.position.z;
  float dist = sqrtf(bx * bx + (bz - 5) * (bz - 5));
  TEST_MSG("Bob pos = (%f, %f), dist from anchor = %f", bx, bz, dist);
  TEST_CHECK(bz < 5.0f);                   // should have fallen
  TEST_CHECK(dist > 1.0f && dist < 3.0f);  // roughly constrained to distance ~2
}

// -----------------------------------------------------------------------
// 3. Stacking + sleep: boxes stacked should eventually sleep
// -----------------------------------------------------------------------
void rb_stacking_sleep_test(void) {
  TydraPhysWorld world;
  TestWorldBuffers buf;
  init_test_world(&world, &buf);

  world.gravity = tp_v3(0, 0, -9.81f);
  world.timestep = 1.0f / 60.0f;
  world.sleep_threshold = 0.05f;
  world.sleep_time = 0.5f;

  // Ground
  TydraPhysBody ground;
  tydra_phys_body_default(&ground);
  ground.body_type = TYDRA_PHYS_BODY_STATIC;
  ground.inverse_mass = 0;
  ground.flags = 0;
  tydra_phys_add_body(&world, &ground);

  TydraPhysCollider gc;
  memset(&gc, 0, sizeof(gc));
  gc.shape.type = TYDRA_PHYS_SHAPE_PLANE;
  gc.shape.data.plane.normal = tp_v3(0, 0, 1);
  gc.local_pose.position = tp_v3(0, 0, 0);
  gc.local_pose.rotation = tp_q_identity();
  gc.static_friction = 0.8f;
  gc.dynamic_friction = 0.8f;
  gc.restitution = 0.1f;
  gc.collision_group = 1;
  gc.collision_mask = 0xFFFFFFFF;
  gc.body_index = 0;
  tydra_phys_add_collider(&world, &gc);

  // Stack 3 boxes
  for (int i = 0; i < 3; i++) {
    TydraPhysBody box;
    tydra_phys_body_default(&box);
    box.body_type = TYDRA_PHYS_BODY_DYNAMIC;
    box.inverse_mass = 1.0f;
    box.local_inv_inertia = tp_v3(6, 6, 6);  // cube inertia
    box.xform.position = tp_v3(0, 0, 0.5f + (float)i * 1.01f);
    box.flags = TYDRA_PHYS_BODY_FLAG_GRAVITY | TYDRA_PHYS_BODY_FLAG_CAN_SLEEP;
    int bi = tydra_phys_add_body(&world, &box);

    TydraPhysCollider bc;
    memset(&bc, 0, sizeof(bc));
    bc.shape.type = TYDRA_PHYS_SHAPE_BOX;
    bc.shape.data.box.half_extents = tp_v3(0.5f, 0.5f, 0.5f);
    bc.local_pose.position = tp_v3(0, 0, 0);
    bc.local_pose.rotation = tp_q_identity();
    bc.static_friction = 0.8f;
    bc.dynamic_friction = 0.8f;
    bc.restitution = 0.1f;
    bc.collision_group = 1;
    bc.collision_mask = 0xFFFFFFFF;
    bc.body_index = bi;
    tydra_phys_add_collider(&world, &bc);
  }

  // Step 600 frames (10 seconds) — should settle
  for (int i = 0; i < 600; i++) {
    tydra_phys_step(&world);
  }

  // All boxes should still be above ground (collision working)
  int above_ground = 1;
  for (int i = 1; i <= 3; i++) {
    float z = world.bodies[i].xform.position.z;
    float v = tp_v3_length(world.bodies[i].linear_velocity);
    TEST_MSG("Body %d: z = %f, velocity = %f", i, z, v);
    if (z < -1.0f) above_ground = 0;
  }
  TEST_CHECK(above_ground);
}

void rb_capacity_scratch_test() {
  TydraPhysWorld world;
  TestWorldBuffers buffers;
  init_test_world(&world, &buffers);
  for (int i = 0; i < MAX_BODIES; ++i) {
    TydraPhysBody body;
    tydra_phys_body_default(&body);
    body.xform.position = tp_v3(float(i), 0, 10);
    TEST_ASSERT(tydra_phys_add_body(&world, &body) == i);
  }
  TEST_CHECK(tydra_phys_step(&world) == TYDRA_PHYS_OK);
  for (int i = 0; i < MAX_BODIES; ++i)
    TEST_CHECK(std::isfinite(world.bodies[i].xform.position.z));
}

int main() {
  rb_capacity_scratch_test();
  rb_sphere_sphere_test();
  rb_sphere_plane_test();
  rb_sphere_box_test();
  rb_capsule_sphere_test();
  rb_aabb_broadphase_test();
  rb_gjk_box_box_test();
  rb_invalid_body_indices_rejected_test();
  rb_falling_sphere_test();
  rb_pendulum_hinge_test();
  rb_stacking_sleep_test();
  return failures ? 1 : 0;
}
