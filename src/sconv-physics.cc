// SPDX-License-Identifier: Apache 2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// USDC writer: Physics prim property extraction
// Split from stage-converter.cc
//
#include "sconv-detail.hh"
#include "usdPhysics.hh"
#include "mjcPhysics.hh"
#include "newtonPhysics.hh"

namespace lightusd {
namespace experimental {

// Helper macro: extract a TypedAttribute<T> as a crate field.
// Goes through value::Value -> ConvertValue for general types.
#define EXTRACT_TYPED(attr, name) do { \
  auto _opt = (attr).get_value(); \
  if (_opt.has_value()) { \
    crate::CrateValue _cv; \
    value::Value _v(_opt.value()); \
    std::string _cerr; \
    if (ConvertValue(_v, _cv, &_cerr)) { \
      fields.push_back({(name), _cv}); \
      /* Preserve the role spelling degraded by ConvertValue (vector3f -> */ \
      /* float3 crate value) -- see AddArrayAttribute in stage-converter.cc. */ \
      if ((attr).has_actual_type()) { \
        crate::CrateValue _ty; \
        _ty.Set(value::token((attr).get_actual_type_name())); \
        fields.push_back({std::string(name) + ".typeName", _ty}); \
      } else if (_v.type_name() != _cv.type_name()) { \
        crate::CrateValue _ty; \
        _ty.Set(value::token(_v.type_name())); \
        fields.push_back({std::string(name) + ".typeName", _ty}); \
      } \
    } \
  } \
  sconv_detail::EmitAttrVariability((name), (attr), fields); \
} while(0)

// Helper macro: extract a TypedAttributeWithFallback<T>
// NOTE: guarded on authored(). A TypedAttributeWithFallback ALWAYS yields a
// value from get_value() -- the schema fallback when nothing was authored -- so
// writing it unconditionally turns "no opinion" into an AUTHORED opinion on
// read-back. That is not cosmetic: an authored opinion is a STRONG one, and it
// blocks weaker opinions during composition.
#define EXTRACT_FALLBACK(attr, name) do { \
  if (!(attr).authored()) break; \
  crate::CrateValue _cv; \
  value::Value _v((attr).get_value()); \
  std::string _cerr; \
  if (ConvertValue(_v, _cv, &_cerr)) { \
    fields.push_back({(name), _cv}); \
    /* Preserve the role spelling degraded by ConvertValue (point3f -> */ \
    /* float3 crate value) -- see AddArrayAttribute in stage-converter.cc. */ \
      if ((attr).has_actual_type()) { \
        crate::CrateValue _ty; \
        _ty.Set(value::token((attr).get_actual_type_name())); \
        fields.push_back({std::string(name) + ".typeName", _ty}); \
      } else if (_v.type_name() != _cv.type_name()) { \
        crate::CrateValue _ty; \
        _ty.Set(value::token(_v.type_name())); \
        fields.push_back({std::string(name) + ".typeName", _ty}); \
      } \
  } \
  sconv_detail::EmitAttrVariability((name), (attr), fields); \
} while(0)

// Helper macro: extract a token TypedAttribute directly (not as uint index)
#define EXTRACT_TOKEN(attr, name) do { \
  auto _opt = (attr).get_value(); \
  if (_opt.has_value()) { \
    crate::CrateValue _cv; \
    _cv.Set(_opt.value()); \
    fields.push_back({(name), _cv}); \
  } \
  sconv_detail::EmitAttrVariability((name), (attr), fields); \
} while(0)

// Helper macro: extract a token TypedAttributeWithFallback directly
#define EXTRACT_TOKEN_FALLBACK(attr, name) do { \
  if (!(attr).authored()) break; \
  crate::CrateValue _cv; \
  _cv.Set((attr).get_value()); \
  fields.push_back({(name), _cv}); \
  sconv_detail::EmitAttrVariability((name), (attr), fields); \
} while(0)

// Like EXTRACT_FALLBACK / EXTRACT_TOKEN_FALLBACK, but only emit when the
// attribute was actually authored. Used for large API structs (e.g. the ~60
// field MjcSceneAPI) where unconditionally writing every fallback would bloat
// the crate and turn unauthored attrs into authored-with-default on read-back.
#define EXTRACT_FALLBACK_IF_AUTHORED(attr, name) do { \
  if ((attr).authored()) { EXTRACT_FALLBACK(attr, name); } \
} while(0)
#define EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(attr, name) do { \
  if ((attr).authored()) { EXTRACT_TOKEN_FALLBACK(attr, name); } \
} while(0)

// Helper: write a RelationshipProperty as a separate relationship spec
// (relationships must be separate SpecType::Relationship specs, not prim fields)
#define EXTRACT_REL(rp, name) do { \
  if ((rp).authored()) { \
    ConvertRelationshipToFields((name), (rp).relationship(), prim_path, err); \
  } \
} while(0)

// ============================================================================
// PhysicsScene
// ============================================================================

bool CrateWriter::ExtractPhysicsSceneProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const PhysicsScene *scene = prim.data().as<PhysicsScene>();
  if (!scene) {
    if (err) *err = "Failed to cast prim to PhysicsScene";
    return false;
  }
  EXTRACT_TYPED(scene->gravityDirection, "physics:gravityDirection");
  EXTRACT_TYPED(scene->gravityMagnitude, "physics:gravityMagnitude");
  // MjcSceneAPI mirror: the reconstruct path consumes mjc:option/flag/compiler
  // props into the typed MjcSceneAPI struct and removes them from `props`, so
  // the generic props-map pass never re-emits them. Re-emit the authored
  // fields here so a MuJoCo PhysicsScene survives USDC round-trip (previously
  // these were silently dropped on write). Mirrors EXTRACT_JOINT_BASE's mjc
  // block for joints and pprint-physics.cc's print_mjc_scene_api for USDA.
  if (scene->mjcScene.has_value()) {
    const auto &_m = scene->mjcScene.value();
    // option:*
    EXTRACT_FALLBACK_IF_AUTHORED(_m.timestep, "mjc:option:timestep");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.impratio, "mjc:option:impratio");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.wind, "mjc:option:wind");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.magnetic, "mjc:option:magnetic");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.density, "mjc:option:density");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.viscosity, "mjc:option:viscosity");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.o_margin, "mjc:option:o_margin");
    EXTRACT_TYPED(_m.o_solref, "mjc:option:o_solref");
    EXTRACT_TYPED(_m.o_solimp, "mjc:option:o_solimp");
    EXTRACT_TYPED(_m.o_friction, "mjc:option:o_friction");
    EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(_m.integrator, "mjc:option:integrator");
    EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(_m.cone, "mjc:option:cone");
    EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(_m.jacobian, "mjc:option:jacobian");
    EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(_m.solver, "mjc:option:solver");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.iterations, "mjc:option:iterations");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.tolerance, "mjc:option:tolerance");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.ls_iterations, "mjc:option:ls_iterations");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.ls_tolerance, "mjc:option:ls_tolerance");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.noslip_iterations, "mjc:option:noslip_iterations");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.noslip_tolerance, "mjc:option:noslip_tolerance");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.ccd_iterations, "mjc:option:ccd_iterations");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.ccd_tolerance, "mjc:option:ccd_tolerance");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.sdf_iterations, "mjc:option:sdf_iterations");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.sdf_initpoints, "mjc:option:sdf_initpoints");
    EXTRACT_TYPED(_m.actuatorgroupdisable, "mjc:option:actuatorgroupdisable");
    // flag:*
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_constraint, "mjc:flag:constraint");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_equality, "mjc:flag:equality");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_frictionloss, "mjc:flag:frictionloss");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_limit, "mjc:flag:limit");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_contact, "mjc:flag:contact");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_spring, "mjc:flag:spring");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_damper, "mjc:flag:damper");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_gravity, "mjc:flag:gravity");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_clampctrl, "mjc:flag:clampctrl");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_warmstart, "mjc:flag:warmstart");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_filterparent, "mjc:flag:filterparent");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_actuation, "mjc:flag:actuation");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_refsafe, "mjc:flag:refsafe");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_sensor, "mjc:flag:sensor");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_midphase, "mjc:flag:midphase");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_nativeccd, "mjc:flag:nativeccd");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_eulerdamp, "mjc:flag:eulerdamp");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_autoreset, "mjc:flag:autoreset");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_island, "mjc:flag:island");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_override, "mjc:flag:override");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_energy, "mjc:flag:energy");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_fwdinv, "mjc:flag:fwdinv");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_invdiscrete, "mjc:flag:invdiscrete");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.flag_multiccd, "mjc:flag:multiccd");
    // compiler:*
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_autoLimits, "mjc:compiler:autoLimits");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_boundMass, "mjc:compiler:boundMass");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_boundInertia, "mjc:compiler:boundInertia");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_setTotalMass, "mjc:compiler:setTotalMass");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_useThread, "mjc:compiler:useThread");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_balanceInertia, "mjc:compiler:balanceInertia");
    EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(_m.compiler_angle, "mjc:compiler:angle");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_fitAABB, "mjc:compiler:fitAABB");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_fuseStatic, "mjc:compiler:fuseStatic");
    EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(_m.compiler_inertiaFromGeom, "mjc:compiler:inertiaFromGeom");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_alignFree, "mjc:compiler:alignFree");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_inertiaGroupRangeMin, "mjc:compiler:inertiaGroupRange:min");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_inertiaGroupRangeMax, "mjc:compiler:inertiaGroupRange:max");
    EXTRACT_FALLBACK_IF_AUTHORED(_m.compiler_saveInertial, "mjc:compiler:saveInertial");
  }
  if (scene->newtonScene.has_value()) {
    const auto &_n = scene->newtonScene.value();
    EXTRACT_FALLBACK(_n.maxSolverIterations, "newton:maxSolverIterations");
    EXTRACT_FALLBACK(_n.timeStepsPerSecond, "newton:timeStepsPerSecond");
    EXTRACT_FALLBACK(_n.gravityEnabled, "newton:gravityEnabled");
  }
  if (scene->newtonXpbdScene.has_value()) {
    const auto &_n = scene->newtonXpbdScene.value();
    EXTRACT_FALLBACK(_n.softBodyRelaxation, "newton:xpbd:softBodyRelaxation");
    EXTRACT_FALLBACK(_n.softContactRelaxation, "newton:xpbd:softContactRelaxation");
    EXTRACT_FALLBACK(_n.jointLinearRelaxation, "newton:xpbd:jointLinearRelaxation");
    EXTRACT_FALLBACK(_n.jointAngularRelaxation, "newton:xpbd:jointAngularRelaxation");
    EXTRACT_FALLBACK(_n.jointLinearCompliance, "newton:xpbd:jointLinearCompliance");
    EXTRACT_FALLBACK(_n.jointAngularCompliance, "newton:xpbd:jointAngularCompliance");
    EXTRACT_FALLBACK(_n.rigidContactRelaxation, "newton:xpbd:rigidContactRelaxation");
    EXTRACT_FALLBACK(_n.rigidContactConWeighting, "newton:xpbd:rigidContactConWeighting");
    EXTRACT_FALLBACK(_n.angularDamping, "newton:xpbd:angularDamping");
    EXTRACT_FALLBACK(_n.restitutionEnabled, "newton:xpbd:restitutionEnabled");
  }
  if (scene->newtonKaminoScene.has_value()) {
    const auto &_n = scene->newtonKaminoScene.value();
    EXTRACT_FALLBACK(_n.padmmPrimalTolerance, "newton:kamino:padmm:primalTolerance");
    EXTRACT_FALLBACK(_n.padmmDualTolerance, "newton:kamino:padmm:dualTolerance");
    EXTRACT_FALLBACK(_n.padmmComplementarityTolerance, "newton:kamino:padmm:complementarityTolerance");
    EXTRACT_TOKEN_FALLBACK(_n.padmmWarmstarting, "newton:kamino:padmm:warmstarting");
    EXTRACT_FALLBACK(_n.padmmUseAcceleration, "newton:kamino:padmm:useAcceleration");
    EXTRACT_FALLBACK(_n.constraintsUsePreconditioning, "newton:kamino:constraints:usePreconditioning");
    EXTRACT_FALLBACK(_n.constraintsAlpha, "newton:kamino:constraints:alpha");
    EXTRACT_FALLBACK(_n.constraintsBeta, "newton:kamino:constraints:beta");
    EXTRACT_FALLBACK(_n.constraintsGamma, "newton:kamino:constraints:gamma");
    EXTRACT_TOKEN_FALLBACK(_n.jointCorrection, "newton:kamino:jointCorrection");
  }
  (void)prim_path;
  return true;
}

// ============================================================================
// Physics Joint types
// ============================================================================

#define EXTRACT_JOINT_BASE(j) do { \
  EXTRACT_REL((j).body0, "physics:body0"); \
  EXTRACT_REL((j).body1, "physics:body1"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).localPos0, "physics:localPos0"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).localPos1, "physics:localPos1"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).localRot0, "physics:localRot0"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).localRot1, "physics:localRot1"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).jointEnabled, "physics:jointEnabled"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).collisionEnabled, "physics:collisionEnabled"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).breakForce, "physics:breakForce"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).breakTorque, "physics:breakTorque"); \
  EXTRACT_FALLBACK_IF_AUTHORED((j).excludeFromArticulation, "physics:excludeFromArticulation"); \
  /* MjcJointAPI mirror: the reconstruct path consumes mjc:* into a typed   \
   * MjcJointAPI struct and removes them from `props`, so the generic       \
   * props-map iteration in stage-converter.cc never sees them. Re-emit     \
   * here so USDC round-trip preserves the values for downstream consumers  \
   * (MuJoCo / Genesis / Newton / PhysX schema mirror). Only emit when the  \
   * API schema is actually attached.                                       \
  */ \
  if ((j).mjcJoint.has_value()) { \
    const auto &_m = (j).mjcJoint.value(); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.group, "mjc:group"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.stiffness, "mjc:stiffness"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.damping, "mjc:damping"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.armature, "mjc:armature"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.frictionloss, "mjc:frictionloss"); \
    EXTRACT_TYPED(_m.springdamper, "mjc:springdamper"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.springref, "mjc:springref"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.ref, "mjc:ref"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.margin, "mjc:margin"); \
    EXTRACT_TYPED(_m.solreflimit, "mjc:solreflimit"); \
    EXTRACT_TYPED(_m.solimplimit, "mjc:solimplimit"); \
    EXTRACT_TYPED(_m.solreffriction, "mjc:solreffriction"); \
    EXTRACT_TYPED(_m.solimpfriction, "mjc:solimpfriction"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.actuatorfrcrange_min, "mjc:actuatorfrcrange:min"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.actuatorfrcrange_max, "mjc:actuatorfrcrange:max"); \
    EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(_m.actuatorfrclimited, "mjc:actuatorfrclimited"); \
    EXTRACT_FALLBACK_IF_AUTHORED(_m.actuatorgravcomp, "mjc:actuatorgravcomp"); \
  } \
  if ((j).newtonMimic.has_value()) { \
    const auto &_n = (j).newtonMimic.value(); \
    EXTRACT_FALLBACK(_n.mimicEnabled, "newton:mimicEnabled"); \
    EXTRACT_REL(_n.mimicJoint, "newton:mimicJoint"); \
    EXTRACT_FALLBACK(_n.mimicCoef0, "newton:mimicCoef0"); \
    EXTRACT_FALLBACK(_n.mimicCoef1, "newton:mimicCoef1"); \
  } \
} while(0)

bool CrateWriter::ExtractPhysicsJointProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const PhysicsJoint *j = prim.data().as<PhysicsJoint>();
  if (!j) { if (err) *err = "Failed to cast to PhysicsJoint"; return false; }
  EXTRACT_JOINT_BASE(*j);
  return true;
}

bool CrateWriter::ExtractPhysicsRevoluteJointProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const PhysicsRevoluteJoint *j = prim.data().as<PhysicsRevoluteJoint>();
  if (!j) { if (err) *err = "Failed to cast to PhysicsRevoluteJoint"; return false; }
  EXTRACT_JOINT_BASE(*j);
  EXTRACT_TOKEN(j->axis, "physics:axis");
  EXTRACT_TYPED(j->lowerLimit, "physics:lowerLimit");
  EXTRACT_TYPED(j->upperLimit, "physics:upperLimit");
  return true;
}

bool CrateWriter::ExtractPhysicsPrismaticJointProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const PhysicsPrismaticJoint *j = prim.data().as<PhysicsPrismaticJoint>();
  if (!j) { if (err) *err = "Failed to cast to PhysicsPrismaticJoint"; return false; }
  EXTRACT_JOINT_BASE(*j);
  EXTRACT_TOKEN(j->axis, "physics:axis");
  EXTRACT_TYPED(j->lowerLimit, "physics:lowerLimit");
  EXTRACT_TYPED(j->upperLimit, "physics:upperLimit");
  return true;
}

bool CrateWriter::ExtractPhysicsSphericalJointProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const PhysicsSphericalJoint *j = prim.data().as<PhysicsSphericalJoint>();
  if (!j) { if (err) *err = "Failed to cast to PhysicsSphericalJoint"; return false; }
  EXTRACT_JOINT_BASE(*j);
  EXTRACT_TOKEN(j->axis, "physics:axis");
  EXTRACT_TYPED(j->coneAngle0Limit, "physics:coneAngle0Limit");
  EXTRACT_TYPED(j->coneAngle1Limit, "physics:coneAngle1Limit");
  return true;
}

bool CrateWriter::ExtractPhysicsFixedJointProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const PhysicsFixedJoint *j = prim.data().as<PhysicsFixedJoint>();
  if (!j) { if (err) *err = "Failed to cast to PhysicsFixedJoint"; return false; }
  EXTRACT_JOINT_BASE(*j);
  return true;
}

bool CrateWriter::ExtractPhysicsDistanceJointProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const PhysicsDistanceJoint *j = prim.data().as<PhysicsDistanceJoint>();
  if (!j) { if (err) *err = "Failed to cast to PhysicsDistanceJoint"; return false; }
  EXTRACT_JOINT_BASE(*j);
  EXTRACT_TYPED(j->minDistance, "physics:minDistance");
  EXTRACT_TYPED(j->maxDistance, "physics:maxDistance");
  return true;
}

// ============================================================================
// PhysicsCollisionGroup
// ============================================================================

bool CrateWriter::ExtractPhysicsCollisionGroupProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const PhysicsCollisionGroup *g = prim.data().as<PhysicsCollisionGroup>();
  if (!g) { if (err) *err = "Failed to cast to PhysicsCollisionGroup"; return false; }
  EXTRACT_TOKEN(g->mergeGroup, "physics:mergeGroup");
  EXTRACT_FALLBACK(g->invertFilteredGroups, "physics:invertFilteredGroups");
  EXTRACT_REL(g->filteredGroups, "physics:filteredGroups");
  return true;
}

// ============================================================================
// MjcActuator, MjcTendon, MjcKeyframe
// ============================================================================

bool CrateWriter::ExtractMjcActuatorProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const MjcActuator *a = prim.data().as<MjcActuator>();
  if (!a) { if (err) *err = "Failed to cast to MjcActuator"; return false; }
  EXTRACT_FALLBACK_IF_AUTHORED(a->group, "mjc:group");
  EXTRACT_REL(a->target, "mjc:target");
  EXTRACT_REL(a->refSite, "mjc:refSite");
  EXTRACT_REL(a->sliderSite, "mjc:sliderSite");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(a->dynType, "mjc:dynType");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(a->gainType, "mjc:gainType");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(a->biasType, "mjc:biasType");
  // The remaining typed attributes were previously dropped on USDC write (only
  // group/target/dyn/gain/bias above survived), losing ctrlRange/forceRange/
  // gainPrm/biasPrm/gear/lengthRange/etc. through a crate round-trip. Emit each
  // authored field so a converted MjcActuator round-trips losslessly.
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(a->ctrlLimited, "mjc:ctrlLimited");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(a->forceLimited, "mjc:forceLimited");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(a->actLimited, "mjc:actLimited");
  EXTRACT_FALLBACK_IF_AUTHORED(a->ctrlRange_min, "mjc:ctrlRange:min");
  EXTRACT_FALLBACK_IF_AUTHORED(a->ctrlRange_max, "mjc:ctrlRange:max");
  EXTRACT_FALLBACK_IF_AUTHORED(a->forceRange_min, "mjc:forceRange:min");
  EXTRACT_FALLBACK_IF_AUTHORED(a->forceRange_max, "mjc:forceRange:max");
  EXTRACT_FALLBACK_IF_AUTHORED(a->actRange_min, "mjc:actRange:min");
  EXTRACT_FALLBACK_IF_AUTHORED(a->actRange_max, "mjc:actRange:max");
  EXTRACT_FALLBACK_IF_AUTHORED(a->lengthRange_min, "mjc:lengthRange:min");
  EXTRACT_FALLBACK_IF_AUTHORED(a->lengthRange_max, "mjc:lengthRange:max");
  EXTRACT_TYPED(a->gear, "mjc:gear");
  EXTRACT_FALLBACK_IF_AUTHORED(a->crankLength, "mjc:crankLength");
  EXTRACT_FALLBACK_IF_AUTHORED(a->jointInParent, "mjc:jointInParent");
  EXTRACT_FALLBACK_IF_AUTHORED(a->actDim, "mjc:actDim");
  EXTRACT_TYPED(a->dynPrm, "mjc:dynPrm");
  EXTRACT_TYPED(a->gainPrm, "mjc:gainPrm");
  EXTRACT_TYPED(a->biasPrm, "mjc:biasPrm");
  EXTRACT_FALLBACK_IF_AUTHORED(a->actEarly, "mjc:actEarly");
  EXTRACT_FALLBACK_IF_AUTHORED(a->inheritRange, "mjc:inheritRange");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(a->plugin, "mjc:plugin");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(a->instance, "mjc:instance");
  return true;
}

bool CrateWriter::ExtractNewtonActuatorProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const NewtonActuator *a = prim.data().as<NewtonActuator>();
  if (!a) { if (err) *err = "Failed to cast to NewtonActuator"; return false; }
  EXTRACT_REL(a->targets, "newton:targets");
  EXTRACT_FALLBACK(a->delaySteps, "newton:delaySteps");
  EXTRACT_FALLBACK(a->constEffort, "newton:constEffort");
  EXTRACT_FALLBACK(a->kp, "newton:kp");
  EXTRACT_FALLBACK(a->kd, "newton:kd");
  EXTRACT_FALLBACK(a->ki, "newton:ki");
  EXTRACT_FALLBACK(a->integralMax, "newton:integralMax");
  EXTRACT_TYPED(a->modelPath, "newton:modelPath");
  EXTRACT_FALLBACK(a->maxEffort, "newton:maxEffort");
  EXTRACT_FALLBACK(a->maxMotorEffort, "newton:maxMotorEffort");
  EXTRACT_FALLBACK(a->saturationEffort, "newton:saturationEffort");
  EXTRACT_FALLBACK(a->velocityLimit, "newton:velocityLimit");
  EXTRACT_TYPED(a->lookupPositions, "newton:lookupPositions");
  EXTRACT_TYPED(a->lookupEfforts, "newton:lookupEfforts");
  (void)prim_path;
  return true;
}

bool CrateWriter::ExtractMjcTendonProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const MjcTendon *t = prim.data().as<MjcTendon>();
  if (!t) { if (err) *err = "Failed to cast to MjcTendon"; return false; }
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(t->type, "mjc:type");
  EXTRACT_REL(t->path, "mjc:path");
  EXTRACT_REL(t->sideSites, "mjc:sideSites");
  EXTRACT_TYPED(t->path_indices, "mjc:path:indices");
  EXTRACT_TYPED(t->sideSites_indices, "mjc:sideSites:indices");
  EXTRACT_TYPED(t->path_segments, "mjc:path:segments");
  EXTRACT_TYPED(t->path_divisors, "mjc:path:divisors");
  EXTRACT_TYPED(t->path_coef, "mjc:path:coef");
  EXTRACT_FALLBACK_IF_AUTHORED(t->group, "mjc:group");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(t->limited, "mjc:limited");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(t->actuatorfrclimited, "mjc:actuatorfrclimited");
  EXTRACT_FALLBACK_IF_AUTHORED(t->range_min, "mjc:range:min");
  EXTRACT_FALLBACK_IF_AUTHORED(t->range_max, "mjc:range:max");
  EXTRACT_FALLBACK_IF_AUTHORED(t->actuatorfrcrange_min, "mjc:actuatorfrcrange:min");
  EXTRACT_FALLBACK_IF_AUTHORED(t->actuatorfrcrange_max, "mjc:actuatorfrcrange:max");
  EXTRACT_TYPED(t->solreflimit, "mjc:solreflimit");
  EXTRACT_TYPED(t->solimplimit, "mjc:solimplimit");
  EXTRACT_TYPED(t->solreffriction, "mjc:solreffriction");
  EXTRACT_TYPED(t->solimpfriction, "mjc:solimpfriction");
  EXTRACT_FALLBACK_IF_AUTHORED(t->margin, "mjc:margin");
  EXTRACT_FALLBACK_IF_AUTHORED(t->frictionloss, "mjc:frictionloss");
  EXTRACT_TYPED(t->springlength, "mjc:springlength");
  EXTRACT_FALLBACK_IF_AUTHORED(t->stiffness, "mjc:stiffness");
  EXTRACT_FALLBACK_IF_AUTHORED(t->damping, "mjc:damping");
  EXTRACT_FALLBACK_IF_AUTHORED(t->armature, "mjc:armature");
  EXTRACT_FALLBACK_IF_AUTHORED(t->width, "mjc:width");
  EXTRACT_FALLBACK_IF_AUTHORED(t->rgba, "mjc:rgba");
  return true;
}

bool CrateWriter::ExtractMjcKeyframeProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const MjcKeyframe *kf = prim.data().as<MjcKeyframe>();
  if (!kf) { if (err) *err = "Failed to cast to MjcKeyframe"; return false; }
  EXTRACT_TYPED(kf->qpos, "mjc:qpos");
  EXTRACT_TYPED(kf->qvel, "mjc:qvel");
  EXTRACT_TYPED(kf->act, "mjc:act");
  EXTRACT_TYPED(kf->ctrl, "mjc:ctrl");
  EXTRACT_TYPED(kf->mpos, "mjc:mpos");
  EXTRACT_TYPED(kf->mquat, "mjc:mquat");
  (void)prim_path;
  return true;
}

bool CrateWriter::ExtractMjcSensorProperties(
    const Prim &prim, const Path &prim_path,
    crate::FieldValuePairVector &fields, std::string *err) {
  const MjcSensor *s = prim.data().as<MjcSensor>();
  if (!s) { if (err) *err = "Failed to cast to MjcSensor"; return false; }
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(s->type, "mjc:type");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(s->objType, "mjc:objtype");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(s->objName, "mjc:objname");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(s->refType, "mjc:reftype");
  EXTRACT_TOKEN_FALLBACK_IF_AUTHORED(s->refName, "mjc:refname");
  EXTRACT_FALLBACK_IF_AUTHORED(s->group, "mjc:group");
  EXTRACT_FALLBACK_IF_AUTHORED(s->cutoff, "mjc:cutoff");
  EXTRACT_FALLBACK_IF_AUTHORED(s->noise, "mjc:noise");
  EXTRACT_TYPED(s->user, "mjc:user");
  (void)prim_path;
  return true;
}

#undef EXTRACT_TYPED
#undef EXTRACT_FALLBACK
#undef EXTRACT_REL
#undef EXTRACT_TOKEN
#undef EXTRACT_TOKEN_FALLBACK
#undef EXTRACT_JOINT_BASE

}  // namespace experimental
}  // namespace lightusd
