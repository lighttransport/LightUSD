// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdPhysics Collision Schema Implementation

#include "physics-collision.hh"

namespace lightusd {
namespace next {

bool IsPhysicsCollisionGroup(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "PhysicsCollisionGroup";
}

bool GetPhysicsCollisionGroupData(const Stage& stage, const UsdPrim& prim,
                                   PhysicsCollisionGroupData* out) {
  if (!IsPhysicsCollisionGroup(prim) || !out) return false;

  (void)stage;

  // collisionEnabled
  {
    const Value* val = prim.GetPropertyValue("physics:collisionEnabled");
    if (val) {
      const bool* b = val->as_bool();
      if (b) out->collisionEnabled = *b;
    }
  }

  // filteredGroups (relationship)
  {
    const std::vector<Path>* targets =
        prim.GetRelationship("physics:filteredGroups");
    if (targets && !targets->empty()) {
      out->filteredGroups = (*targets)[0].str();
    }
  }

  return true;
}

} // namespace next
} // namespace lightusd
