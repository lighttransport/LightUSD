// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdPhysics Scene Schema Implementation

#include "physics-scene.hh"

#include <cmath>

namespace lightusd {
namespace next {

bool IsPhysicsScene(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "PhysicsScene";
}

bool GetPhysicsSceneData(const Stage& stage, const UsdPrim& prim,
                         PhysicsSceneData* out, double time) {
  if (!IsPhysicsScene(prim) || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  EvalResult magnitude = eval.Eval(prim, "physics:gravityMagnitude");
  const float* magnitude_value = magnitude.value.as_float();
  out->gravityMagnitude = magnitude_value ? *magnitude_value : 9.81f;
  // Convert only the schema fallback sentinel here. Preserve an authored
  // negative request for callers that need to distinguish authored state.
  if (magnitude.from_schema_fallback) {
    out->gravityMagnitude = 9.81f;
  }

  float dir[3] = {0.0f, -1.0f, 0.0f};
  if (stage.GetUpAxis() == "Z") {
    dir[1] = 0.0f;
    dir[2] = -1.0f;
  }
  float authored_dir[3] = {dir[0], dir[1], dir[2]};
  if (eval.EvalFloat3(prim, "physics:gravityDirection", authored_dir) &&
      (authored_dir[0] != 0.0f || authored_dir[1] != 0.0f ||
       authored_dir[2] != 0.0f)) {
    dir[0] = authored_dir[0];
    dir[1] = authored_dir[1];
    dir[2] = authored_dir[2];
  }
  out->gravityDirection[0] = dir[0];
  out->gravityDirection[1] = dir[1];
  out->gravityDirection[2] = dir[2];

  return true;
}

} // namespace next
} // namespace lightusd
