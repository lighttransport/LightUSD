// SPDX-License-Identifier: Apache-2.0
#include "physics_preview.hh"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

#include "lightusd-render-c.h"
#define CHECK(x)                                           \
  do {                                                     \
    if (!(x)) {                                            \
      std::cerr << __LINE__ << ": " #x " " << error << " " \
                << lightusd_last_error() << "\n";          \
      return 1;                                            \
    }                                                      \
  } while (0)
int main() {
  const char* text = R"USD(#usda 1.0
(
 upAxis="Z"
 metersPerUnit=1
)
def PhysicsScene "Scene" {
 vector3f physics:gravityDirection=(0,0,-1)
 float physics:gravityMagnitude=9.81
}
def Plane "Ground" (prepend apiSchemas=["PhysicsCollisionAPI"]) {
 uniform token axis="Z"
}
def Xform "Parent" {
 double3 xformOp:translate=(10,20,0)
 uniform token[] xformOpOrder=["xformOp:translate"]
 def Sphere "Ball" (prepend apiSchemas=["PhysicsRigidBodyAPI","PhysicsCollisionAPI","PhysicsMassAPI"]) {
  double radius=1
  float physics:mass=2
  double3 xformOp:translate=(0,0,4)
  uniform token[] xformOpOrder=["xformOp:translate"]
 }
}
def Cube "Kinematic" (prepend apiSchemas=["PhysicsRigidBodyAPI","PhysicsCollisionAPI"]) {
 bool physics:kinematicEnabled=true
 double size=1
 double3 xformOp:translate.timeSamples={0:(0,0,6),1:(2,0,6)}
 uniform token[] xformOpOrder=["xformOp:translate"]
}
def Xform "Rotated" (prepend apiSchemas=["PhysicsRigidBodyAPI"]) {
 float3 xformOp:rotateXYZ=(20,35,70)
 double3 xformOp:translate=(0,0,10)
 double3 xformOp:scale=(2,3,4)
 uniform token[] xformOpOrder=["xformOp:translate","xformOp:rotateXYZ","xformOp:scale"]
}
)USD";
  lightusd::api::Stage stage;
  std::string error;
  CHECK(stage.load_from_memory(reinterpret_cast<const uint8_t*>(text),
                               std::strlen(text)) == LIGHTUSD_OK);
  lightusd_render_prim_catalog* catalog = nullptr;
  CHECK(lightusd_render_prim_catalog_create(stage.get(), 0, 1, 1, 1,
                                            &catalog) == LIGHTUSD_OK);
  bool preserved = false;
  for (size_t i = 0; i < lightusd_render_prim_catalog_count(
                             catalog, LIGHTUSD_RENDER_PRIM_MESH);
       ++i) {
    lightusd_render_prim_info info{};
    info.struct_size = sizeof(info);
    CHECK(lightusd_render_prim_catalog_get(catalog, LIGHTUSD_RENDER_PRIM_MESH,
                                           i, &info) == LIGHTUSD_OK);
    if (std::string(info.path.data, info.path.len) == "/Parent/Ball")
      preserved = info.animated_world != 0;
  }
  lightusd_render_prim_catalog_destroy(catalog);
  CHECK(preserved);
  lusdview::PhysicsPreview preview;
  CHECK(preview.initialize(stage.get(), 0, &error));
  CHECK(preview.world().num_bodies == 4);
  CHECK(preview.world().num_colliders == 3);
  CHECK(preview.world().bodies[0].xform.position.x == 10);
  CHECK(preview.world().bodies[0].xform.position.y == 20);
  CHECK(std::fabs(preview.world().bodies[0].inverse_mass - 0.5f) < 1e-6f);
  const auto original = lightusd_stage_prim_at_path(stage.get(), "/Rotated");
  double before[16];
  CHECK(lightusd_prim_world_transform(stage.get(), original, 0, before) ==
        LIGHTUSD_OK);
  CHECK(preview.step(stage.get(), 1, 1, 1.0f / 60, &error));
  CHECK(std::fabs(preview.world().bodies[1].xform.position.x - 2) < 1e-6f);
  CHECK(std::fabs(preview.world().bodies[1].linear_velocity.x - 120) < 1e-3f);
  double after[16];
  const auto rotated = lightusd_stage_prim_at_path(preview.stage(), "/Rotated");
  CHECK(lightusd_prim_world_transform(preview.stage(), rotated, 0, after) ==
        LIGHTUSD_OK);
  for (int i = 0; i < 12; ++i) CHECK(std::fabs(before[i] - after[i]) < 1e-5);
  CHECK(preview.step(stage.get(), 1, 240, 1.0f / 120, &error));
  CHECK(preview.world().bodies[0].xform.position.z > 0.5f &&
        preview.world().bodies[0].xform.position.z < 2);
  const auto ball =
      lightusd_stage_prim_at_path(preview.stage(), "/Parent/Ball");
  CHECK(lightusd_prim_world_transform(preview.stage(), ball, 0, after) ==
        LIGHTUSD_OK);
  CHECK(std::fabs(after[12] - 10) < 1e-5 && std::fabs(after[13] - 20) < 1e-5);
  CHECK(std::fabs(after[14] - preview.world().bodies[0].xform.position.z) <
        1e-5);
  CHECK(lightusd_prim_world_transform(
            stage.get(),
            lightusd_stage_prim_at_path(stage.get(), "/Parent/Ball"), 0,
            before) == LIGHTUSD_OK);
  CHECK(before[14] == 4);
  CHECK(!preview.step(stage.get(), 0, 1,
                      std::numeric_limits<float>::quiet_NaN(), &error));
  CHECK(!preview.step(stage.get(), 0, 601, 0.01f, &error));
  CHECK(preview.initialize(stage.get(), 0, &error));
  CHECK(preview.world().bodies[0].xform.position.z == 4);
  const char* compound = R"USD(#usda 1.0
def Xform "Body" (prepend apiSchemas=["PhysicsRigidBodyAPI"]) {
 def Cube "A" (prepend apiSchemas=["PhysicsCollisionAPI"]) {}
 def Capsule "B" (prepend apiSchemas=["PhysicsCollisionAPI"]) { uniform token axis="X"; }
}
def PhysicsRevoluteJoint "Joint" {
 rel physics:body1=</Body>
 uniform token axis="Z"
 float physics:lowerLimit=-90
 float physics:upperLimit=45
}
)USD";
  lightusd::api::Stage articulated;
  CHECK(articulated.load_from_memory(reinterpret_cast<const uint8_t*>(compound),
                                     std::strlen(compound)) == LIGHTUSD_OK);
  CHECK(preview.initialize(articulated.get(), 0, &error));
  CHECK(preview.world().num_bodies == 1 && preview.world().num_colliders == 2);
  CHECK(preview.world().bodies[0].collider_count == 2);
  CHECK(preview.world().num_joints == 1);
  CHECK(preview.world().joints[0].body_a == -1 &&
        preview.world().joints[0].body_b == 0);
  CHECK(std::fabs(preview.world().joints[0].lower_limit + 1.57079632679f) <
        1e-6f);
  CHECK(preview.world().joints[0].axis.z == 1);
  const char* mesh = R"USD(#usda 1.0
def Mesh "Mesh" (prepend apiSchemas=["PhysicsRigidBodyAPI","PhysicsCollisionAPI"]) {
 point3f[] points=[(2,4,6),(6,8,10)]
}
)USD";
  lightusd::api::Stage meshStage;
  CHECK(meshStage.load_from_memory(reinterpret_cast<const uint8_t*>(mesh),
                                   std::strlen(mesh)) == LIGHTUSD_OK);
  CHECK(preview.initialize(meshStage.get(), 0, &error));
  CHECK(preview.world().colliders[0].shape.type == TYDRA_PHYS_SHAPE_BOX);
  CHECK(preview.world().colliders[0].shape.data.box.half_extents.x == 2);
  CHECK(preview.world().colliders[0].local_pose.position.x == 4);
  CHECK(preview.world().colliders[0].local_pose.position.y == 6);
  CHECK(preview.world().colliders[0].local_pose.position.z == 8);
  preview.clear();
  CHECK(preview.stage() == nullptr);
  CHECK(preview.world().num_bodies == 0);
  std::cout << "PASS core physics preview: contacts, world writeback, "
               "scale/rotation, immutable source, kinematics and reset\n";
}
