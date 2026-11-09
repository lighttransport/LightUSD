// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// rb-dynamics.hh — C++ bridge: USD Stage ↔ TydraPhysWorld
//
#pragma once

#include "rb-dynamics.h"

#include <string>

namespace lightusd {

class Stage;

namespace tydra {

struct PhysWorldBuildOptions {
  bool use_mjc_params = true;      // Apply MuJoCo scene/joint parameters
  bool verbose = false;
  float default_mass = 1.0f;
  float default_friction = 0.5f;
  float default_restitution = 0.3f;
  int32_t max_bodies = 1024;
  int32_t max_colliders = 2048;
  int32_t max_joints = 512;
  int32_t max_contacts = 4096;
  int32_t max_pairs = 8192;
  int32_t max_islands = 256;
  size_t max_memory_limit_mb = 0;  // 0 = no limit; max bytes for all physics buffers
};

/// Build a TydraPhysWorld from a USD Stage.
///
/// Traverses the stage for PhysicsScene, prims with PhysicsRigidBodyAPI,
/// GeomSphere/GeomCube/etc. with PhysicsCollisionAPI, and physics joints.
/// Allocates all internal buffers (caller must call FreePhysWorld).
bool BuildPhysWorld(
    const Stage &stage,
    TydraPhysWorld *out_world,
    std::string *err = nullptr,
    const PhysWorldBuildOptions &options = {});

/// Write simulation results back to stage prim transforms.
bool SyncPhysWorldToStage(
    const TydraPhysWorld &world,
    Stage *stage,
    std::string *err = nullptr);

/// Update kinematic bodies from their matching authored Stage transforms.
/// Linear and angular velocities are derived from the pose delta and the
/// world's timestep so contacts inherit animation-driven motion.
/// Rigid bodies are matched in the same deterministic traversal order used by
/// BuildPhysWorld and SyncPhysWorldToStage.
bool SyncStageToPhysWorld(const Stage &stage, TydraPhysWorld *world,
                          std::string *err = nullptr);

/// Free all buffers allocated by BuildPhysWorld.
void FreePhysWorld(TydraPhysWorld *world);

}  // namespace tydra
}  // namespace lightusd
