// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdPhysics Collision Schema

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"
#include <string>

namespace lightusd {

// ============================================================
// Typed collision primitives
// ============================================================

bool IsPhysicsCollisionGroup(const UsdPrim& prim);

struct PhysicsCollisionGroupData {
  bool collisionEnabled = true;
  std::string filteredGroups; // rel physics:filteredGroups
};

bool GetPhysicsCollisionGroupData(const Stage& stage, const UsdPrim& prim,
                                   PhysicsCollisionGroupData* out);

// ============================================================
// PhysicsScene collision query
// ============================================================

struct PhysicsContactData {
  std::string body0;
  std::string body1;
  float position[3];
  float normal[3];
  float impulse = 0.0f;
  float separation = 0.0f;
};

} // namespace lightusd
