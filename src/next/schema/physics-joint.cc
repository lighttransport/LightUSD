// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdPhysics Joint Schema Implementation

#include "physics-joint.hh"
#include <cstring>

namespace lightusd {
namespace next {

// ============================================================
// Joint type checking
// ============================================================

static bool IsJointType(const UsdPrim& prim, const std::string& type) {
  return prim.IsValid() && prim.GetTypeName() == type;
}

static bool GetStringOrToken(const UsdPrim& prim, const std::string& prop_name,
                             std::string* out) {
  if (!out) return false;
  const Value* val = prim.GetPropertyValue(prop_name);
  if (!val) return false;
  if (auto s = val->as_string()) {
    *out = *s;
    return true;
  }
  if (auto tok = val->as_token()) {
    *out = *tok;
    return true;
  }
  return false;
}

static void SetAxisFromToken(const std::string& token, float axis[3]) {
  if (token == "X") {
    axis[0] = 1.0f;
    axis[1] = 0.0f;
    axis[2] = 0.0f;
  } else if (token == "Y") {
    axis[0] = 0.0f;
    axis[1] = 1.0f;
    axis[2] = 0.0f;
  } else if (token == "Z") {
    axis[0] = 0.0f;
    axis[1] = 0.0f;
    axis[2] = 1.0f;
  }
}

bool IsPhysicsJoint(const UsdPrim& prim) {
  if (!prim.IsValid()) return false;
  const std::string& type = prim.GetTypeName();
  return type == "PhysicsJoint" ||
         type == "PhysicsPrismaticJoint" ||
         type == "PhysicsRevoluteJoint" ||
         type == "PhysicsSphericalJoint" ||
         type == "PhysicsFixedJoint" ||
         type == "PhysicsDistanceJoint" ||
         type == "PhysicsSliderJoint" ||
         type == "PhysicsBallJoint";
}

bool IsPhysicsPrismaticJoint(const UsdPrim& prim) {
  return IsJointType(prim, "PhysicsPrismaticJoint");
}

bool IsPhysicsRevoluteJoint(const UsdPrim& prim) {
  return IsJointType(prim, "PhysicsRevoluteJoint");
}

bool IsPhysicsSphericalJoint(const UsdPrim& prim) {
  return IsJointType(prim, "PhysicsSphericalJoint");
}

bool IsPhysicsFixedJoint(const UsdPrim& prim) {
  return IsJointType(prim, "PhysicsFixedJoint");
}

bool IsPhysicsDistanceJoint(const UsdPrim& prim) {
  return IsJointType(prim, "PhysicsDistanceJoint");
}

bool IsPhysicsSliderJoint(const UsdPrim& prim) {
  return IsJointType(prim, "PhysicsSliderJoint");
}

bool IsPhysicsBallJoint(const UsdPrim& prim) {
  return IsJointType(prim, "PhysicsBallJoint");
}

// ============================================================
// Common joint data
// ============================================================

bool GetPhysicsJointData(const Stage& stage, const UsdPrim& prim,
                          PhysicsJointData* out, double time) {
  if (!prim.IsValid() || !out) return false;

  // body0
  {
    const std::vector<Path>* targets =
        prim.GetRelationship("physics:body0");
    if (targets && !targets->empty()) {
      out->body0 = (*targets)[0].str();
      out->hasBody0 = true;
    }
  }

  // body1
  {
    const std::vector<Path>* targets =
        prim.GetRelationship("physics:body1");
    if (targets && !targets->empty()) {
      out->body1 = (*targets)[0].str();
      out->hasBody1 = true;
    }
  }

  // breakForce
  {
    const Value* val = prim.GetPropertyValue("physics:breakForce");
    if (val) {
      const float* f = val->as_float();
      if (f) out->breakForce = *f;
    }
  }

  // breakTorque
  {
    const Value* val = prim.GetPropertyValue("physics:breakTorque");
    if (val) {
      const float* f = val->as_float();
      if (f) out->breakTorque = *f;
    }
  }

  // collisionEnabled
  {
    const Value* val = prim.GetPropertyValue("physics:collisionEnabled");
    if (val) {
      const bool* b = val->as_bool();
      if (b) out->collisionEnabled = *b;
    }
  }

  // enableMotor
  {
    const Value* val = prim.GetPropertyValue("physics:enableMotor");
    if (val) {
      const bool* b = val->as_bool();
      if (b) out->enableMotor = *b;
    }
  }

  // motorTargetVelocity
  {
    const Value* val = prim.GetPropertyValue("physics:motorTargetVelocity");
    if (val) {
      const float* f = val->as_float();
      if (f) out->motorTargetVelocity = *f;
    }
  }

  // motorMaxForce
  {
    const Value* val = prim.GetPropertyValue("physics:motorMaxForce");
    if (val) {
      const float* f = val->as_float();
      if (f) out->motorMaxForce = *f;
    }
  }

  // Local joint frames: positions (vector3f) and rotations (quatf). These were
  // previously never read, so the joint frames stayed at identity even when
  // authored. EvalFloat3/EvalFloat4 leave the struct defaults in place when the
  // attribute is absent and report presence for the has* flags.
  AttributeEval eval(&stage);
  eval.SetTime(time);
  out->hasLocalPos0 = eval.EvalFloat3(prim, "physics:localPos0", out->localPos0);
  out->hasLocalPos1 = eval.EvalFloat3(prim, "physics:localPos1", out->localPos1);
  out->hasLocalRot0 = eval.EvalFloat4(prim, "physics:localRot0", out->localQuat0);
  out->hasLocalRot1 = eval.EvalFloat4(prim, "physics:localRot1", out->localQuat1);

  return true;
}

// ============================================================
// PrismaticJoint
// ============================================================

bool GetPhysicsPrismaticJointData(const Stage& stage, const UsdPrim& prim,
                                   PhysicsPrismaticJointData* out,
                                   double time) {
  if (!IsPhysicsPrismaticJoint(prim) || !out) return false;

  if (!GetPhysicsJointData(stage, prim, out, time)) return false;

  // axis
  {
    std::string axis;
    if (GetStringOrToken(prim, "physics:axis", &axis)) {
      SetAxisFromToken(axis, out->axis);
    }
  }

  // lowerLimit
  {
    const Value* val = prim.GetPropertyValue("physics:lowerLimit");
    if (val) {
      const float* f = val->as_float();
      if (f) out->lowerLimit = *f;
    }
  }

  // upperLimit
  {
    const Value* val = prim.GetPropertyValue("physics:upperLimit");
    if (val) {
      const float* f = val->as_float();
      if (f) out->upperLimit = *f;
    }
  }

  // maxForce
  {
    const Value* val = prim.GetPropertyValue("physics:maxForce");
    if (val) {
      const float* f = val->as_float();
      if (f) out->maxForce = *f;
    }
  }

  return true;
}

// ============================================================
// RevoluteJoint
// ============================================================

bool GetPhysicsRevoluteJointData(const Stage& stage, const UsdPrim& prim,
                                  PhysicsRevoluteJointData* out,
                                  double time) {
  if (!IsPhysicsRevoluteJoint(prim) || !out) return false;

  if (!GetPhysicsJointData(stage, prim, out, time)) return false;

  // axis
  {
    std::string axis;
    if (GetStringOrToken(prim, "physics:axis", &axis)) {
      SetAxisFromToken(axis, out->axis);
    }
  }

  // lowerLimit
  {
    const Value* val = prim.GetPropertyValue("physics:lowerLimit");
    if (val) {
      const float* f = val->as_float();
      if (f) out->lowerLimit = *f;
    }
  }

  // upperLimit
  {
    const Value* val = prim.GetPropertyValue("physics:upperLimit");
    if (val) {
      const float* f = val->as_float();
      if (f) out->upperLimit = *f;
    }
  }

  // maxTorque
  {
    const Value* val = prim.GetPropertyValue("physics:maxTorque");
    if (val) {
      const float* f = val->as_float();
      if (f) out->maxTorque = *f;
    }
  }

  return true;
}

// ============================================================
// SphericalJoint
// ============================================================

bool GetPhysicsSphericalJointData(const Stage& stage, const UsdPrim& prim,
                                   PhysicsSphericalJointData* out,
                                   double time) {
  if (!IsPhysicsSphericalJoint(prim) || !out) return false;

  if (!GetPhysicsJointData(stage, prim, out, time)) return false;

  // coneAngle0Limit (schema name). Also sets the legacy alias for backward compat.
  {
    const Value* val = prim.GetPropertyValue("physics:coneAngle0Limit");
    if (val) {
      const float* f = val->as_float();
      if (f) {
        out->coneAngle0Limit = *f;
        out->coneAngleLimit = *f;
      }
    }
  }

  // coneAngle1Limit (schema name). Also sets the legacy alias.
  {
    const Value* val = prim.GetPropertyValue("physics:coneAngle1Limit");
    if (val) {
      const float* f = val->as_float();
      if (f) {
        out->coneAngle1Limit = *f;
        out->coneAngleLimitY = *f;
      }
    }
  }

  // Legacy non-schema aliases: only apply when the schema property is absent,
  // so the schema name always takes priority when both are authored.
  if (!prim.GetPropertyValue("physics:coneAngle0Limit")) {
    const Value* val = prim.GetPropertyValue("physics:coneAngleLimit");
    if (val) {
      const float* f = val->as_float();
      if (f) {
        out->coneAngleLimit = *f;
        out->coneAngle0Limit = *f;
      }
    }
  }

  if (!prim.GetPropertyValue("physics:coneAngle1Limit")) {
    const Value* val = prim.GetPropertyValue("physics:coneAngleLimitY");
    if (val) {
      const float* f = val->as_float();
      if (f) {
        out->coneAngleLimitY = *f;
        out->coneAngle1Limit = *f;
      }
    }
  }

  {
    const Value* val = prim.GetPropertyValue("physics:coneAngleLimitZ");
    if (val) {
      const float* f = val->as_float();
      if (f) out->coneAngleLimitZ = *f;
    }
  }

  // maxTorque
  {
    const Value* val = prim.GetPropertyValue("physics:maxTorque");
    if (val) {
      const float* f = val->as_float();
      if (f) out->maxTorque = *f;
    }
  }

  return true;
}

// ============================================================
// FixedJoint
// ============================================================

bool GetPhysicsFixedJointData(const Stage& stage, const UsdPrim& prim,
                               PhysicsFixedJointData* out,
                               double time) {
  if (!IsPhysicsFixedJoint(prim) || !out) return false;
  return GetPhysicsJointData(stage, prim, out, time);
}

// ============================================================
// DistanceJoint
// ============================================================

bool GetPhysicsDistanceJointData(const Stage& stage, const UsdPrim& prim,
                                  PhysicsDistanceJointData* out,
                                  double time) {
  if (!IsPhysicsDistanceJoint(prim) || !out) return false;

  if (!GetPhysicsJointData(stage, prim, out, time)) return false;

  // minDistance
  {
    const Value* val = prim.GetPropertyValue("physics:minDistance");
    if (val) {
      const float* f = val->as_float();
      if (f) out->minDistance = *f;
    }
  }

  // maxDistance
  {
    const Value* val = prim.GetPropertyValue("physics:maxDistance");
    if (val) {
      const float* f = val->as_float();
      if (f) out->maxDistance = *f;
    }
  }

  return true;
}

// ============================================================
// SliderJoint (alias for PrismaticJoint)
// ============================================================

bool GetPhysicsSliderJointData(const Stage& stage, const UsdPrim& prim,
                                PhysicsSliderJointData* out,
                                double time) {
  if (!IsPhysicsSliderJoint(prim) || !out) return false;
  if (!GetPhysicsJointData(stage, prim, out, time)) return false;

  std::string axis;
  if (GetStringOrToken(prim, "physics:axis", &axis)) {
    SetAxisFromToken(axis, out->axis);
  }
  if (const Value* val = prim.GetPropertyValue("physics:lowerLimit")) {
    if (const float* f = val->as_float()) out->lowerLimit = *f;
  }
  if (const Value* val = prim.GetPropertyValue("physics:upperLimit")) {
    if (const float* f = val->as_float()) out->upperLimit = *f;
  }
  if (const Value* val = prim.GetPropertyValue("physics:maxForce")) {
    if (const float* f = val->as_float()) out->maxForce = *f;
  }
  return true;
}

// ============================================================
// BallJoint (alias for SphericalJoint)
// ============================================================

bool GetPhysicsBallJointData(const Stage& stage, const UsdPrim& prim,
                              PhysicsBallJointData* out,
                              double time) {
  if (!IsPhysicsBallJoint(prim) || !out) return false;
  if (!GetPhysicsJointData(stage, prim, out, time)) return false;

  if (const Value* val = prim.GetPropertyValue("physics:coneAngle0Limit")) {
    if (const float* f = val->as_float()) {
      out->coneAngle0Limit = *f;
      out->coneAngleLimit = *f;
    }
  }
  if (const Value* val = prim.GetPropertyValue("physics:coneAngle1Limit")) {
    if (const float* f = val->as_float()) {
      out->coneAngle1Limit = *f;
      out->coneAngleLimitY = *f;
    }
  }
  if (const Value* val = prim.GetPropertyValue("physics:coneAngleLimit")) {
    if (const float* f = val->as_float()) {
      out->coneAngleLimit = *f;
      out->coneAngle0Limit = *f;
    }
  }
  if (const Value* val = prim.GetPropertyValue("physics:coneAngleLimitY")) {
    if (const float* f = val->as_float()) {
      out->coneAngleLimitY = *f;
      out->coneAngle1Limit = *f;
    }
  }
  if (const Value* val = prim.GetPropertyValue("physics:coneAngleLimitZ")) {
    if (const float* f = val->as_float()) out->coneAngleLimitZ = *f;
  }
  if (const Value* val = prim.GetPropertyValue("physics:maxTorque")) {
    if (const float* f = val->as_float()) out->maxTorque = *f;
  }
  return true;
}

} // namespace next
} // namespace lightusd
