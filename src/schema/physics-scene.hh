// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdPhysics Scene Schema

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"

namespace lightusd {

bool IsPhysicsScene(const UsdPrim& prim);

struct PhysicsSceneData {
  float gravityMagnitude = 9.81f;  // physics:gravityMagnitude
  float gravityDirection[3] = {0.0f, -1.0f, 0.0f};  // physics:gravityDirection
};

bool GetPhysicsSceneData(const Stage& stage, const UsdPrim& prim,
                         PhysicsSceneData* out, double time = 0.0);

} // namespace lightusd
