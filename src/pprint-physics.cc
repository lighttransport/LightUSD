// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Physics prim to_string (extracted from pprinter.cc pattern).
//
#include "pprinter.hh"
#include "pprint-detail.hh"

#include "common-macros.inc"

namespace lightusd {

// Helper: print a RelationshipProperty if authored
static std::string print_rel_prop(const RelationshipProperty &rp,
                                  const std::string &name, uint32_t indent) {
  if (!rp.authored()) return "";
  return print_relationship(rp.relationship(), rp.get_listedit_qual(),
                            /* custom */ false, name, indent);
}

// Helper: print MjcSceneAPI attributes
static std::string print_mjc_scene_api(const MjcSceneAPI &api, uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  // Option attributes
  ss << print_typed_attr(api.timestep, "mjc:option:timestep", indent);
  ss << print_typed_attr(api.impratio, "mjc:option:impratio", indent);
  ss << print_typed_attr(api.wind, "mjc:option:wind", indent);
  ss << print_typed_attr(api.magnetic, "mjc:option:magnetic", indent);
  ss << print_typed_attr(api.density, "mjc:option:density", indent);
  ss << print_typed_attr(api.viscosity, "mjc:option:viscosity", indent);
  ss << print_typed_attr(api.o_margin, "mjc:option:o_margin", indent);
  ss << print_typed_attr(api.o_solref, "mjc:option:o_solref", indent);
  ss << print_typed_attr(api.o_solimp, "mjc:option:o_solimp", indent);
  ss << print_typed_attr(api.o_friction, "mjc:option:o_friction", indent);
  ss << print_typed_attr(api.integrator, "mjc:option:integrator", indent);
  ss << print_typed_attr(api.cone, "mjc:option:cone", indent);
  ss << print_typed_attr(api.jacobian, "mjc:option:jacobian", indent);
  ss << print_typed_attr(api.solver, "mjc:option:solver", indent);
  ss << print_typed_attr(api.iterations, "mjc:option:iterations", indent);
  ss << print_typed_attr(api.tolerance, "mjc:option:tolerance", indent);
  ss << print_typed_attr(api.ls_iterations, "mjc:option:ls_iterations", indent);
  ss << print_typed_attr(api.ls_tolerance, "mjc:option:ls_tolerance", indent);
  ss << print_typed_attr(api.noslip_iterations, "mjc:option:noslip_iterations", indent);
  ss << print_typed_attr(api.noslip_tolerance, "mjc:option:noslip_tolerance", indent);
  ss << print_typed_attr(api.ccd_iterations, "mjc:option:ccd_iterations", indent);
  ss << print_typed_attr(api.ccd_tolerance, "mjc:option:ccd_tolerance", indent);
  ss << print_typed_attr(api.sdf_iterations, "mjc:option:sdf_iterations", indent);
  ss << print_typed_attr(api.sdf_initpoints, "mjc:option:sdf_initpoints", indent);
  ss << print_typed_attr(api.actuatorgroupdisable, "mjc:option:actuatorgroupdisable", indent);

  // Flag attributes
  ss << print_typed_attr(api.flag_constraint, "mjc:flag:constraint", indent);
  ss << print_typed_attr(api.flag_equality, "mjc:flag:equality", indent);
  ss << print_typed_attr(api.flag_frictionloss, "mjc:flag:frictionloss", indent);
  ss << print_typed_attr(api.flag_limit, "mjc:flag:limit", indent);
  ss << print_typed_attr(api.flag_contact, "mjc:flag:contact", indent);
  ss << print_typed_attr(api.flag_spring, "mjc:flag:spring", indent);
  ss << print_typed_attr(api.flag_damper, "mjc:flag:damper", indent);
  ss << print_typed_attr(api.flag_gravity, "mjc:flag:gravity", indent);
  ss << print_typed_attr(api.flag_clampctrl, "mjc:flag:clampctrl", indent);
  ss << print_typed_attr(api.flag_warmstart, "mjc:flag:warmstart", indent);
  ss << print_typed_attr(api.flag_filterparent, "mjc:flag:filterparent", indent);
  ss << print_typed_attr(api.flag_actuation, "mjc:flag:actuation", indent);
  ss << print_typed_attr(api.flag_refsafe, "mjc:flag:refsafe", indent);
  ss << print_typed_attr(api.flag_sensor, "mjc:flag:sensor", indent);
  ss << print_typed_attr(api.flag_midphase, "mjc:flag:midphase", indent);
  ss << print_typed_attr(api.flag_nativeccd, "mjc:flag:nativeccd", indent);
  ss << print_typed_attr(api.flag_eulerdamp, "mjc:flag:eulerdamp", indent);
  ss << print_typed_attr(api.flag_autoreset, "mjc:flag:autoreset", indent);
  ss << print_typed_attr(api.flag_island, "mjc:flag:island", indent);
  ss << print_typed_attr(api.flag_override, "mjc:flag:override", indent);
  ss << print_typed_attr(api.flag_energy, "mjc:flag:energy", indent);
  ss << print_typed_attr(api.flag_fwdinv, "mjc:flag:fwdinv", indent);
  ss << print_typed_attr(api.flag_invdiscrete, "mjc:flag:invdiscrete", indent);
  ss << print_typed_attr(api.flag_multiccd, "mjc:flag:multiccd", indent);

  // Compiler attributes
  ss << print_typed_attr(api.compiler_autoLimits, "mjc:compiler:autoLimits", indent);
  ss << print_typed_attr(api.compiler_boundMass, "mjc:compiler:boundMass", indent);
  ss << print_typed_attr(api.compiler_boundInertia, "mjc:compiler:boundInertia", indent);
  ss << print_typed_attr(api.compiler_setTotalMass, "mjc:compiler:setTotalMass", indent);
  ss << print_typed_attr(api.compiler_useThread, "mjc:compiler:useThread", indent);
  ss << print_typed_attr(api.compiler_balanceInertia, "mjc:compiler:balanceInertia", indent);
  ss << print_typed_attr(api.compiler_angle, "mjc:compiler:angle", indent);
  ss << print_typed_attr(api.compiler_fitAABB, "mjc:compiler:fitAABB", indent);
  ss << print_typed_attr(api.compiler_fuseStatic, "mjc:compiler:fuseStatic", indent);
  ss << print_typed_attr(api.compiler_inertiaFromGeom, "mjc:compiler:inertiaFromGeom", indent);
  ss << print_typed_attr(api.compiler_alignFree, "mjc:compiler:alignFree", indent);
  ss << print_typed_attr(api.compiler_inertiaGroupRangeMin, "mjc:compiler:inertiaGroupRange:min", indent);
  ss << print_typed_attr(api.compiler_inertiaGroupRangeMax, "mjc:compiler:inertiaGroupRange:max", indent);
  ss << print_typed_attr(api.compiler_saveInertial, "mjc:compiler:saveInertial", indent);

  return pp_ss.str();
}

// Helper: print MjcJointAPI attributes
static std::string print_mjc_joint_api(const MjcJointAPI &api, uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  ss << print_typed_attr(api.group, "mjc:group", indent);
  ss << print_typed_attr(api.stiffness, "mjc:stiffness", indent);
  ss << print_typed_attr(api.damping, "mjc:damping", indent);
  ss << print_typed_attr(api.armature, "mjc:armature", indent);
  ss << print_typed_attr(api.frictionloss, "mjc:frictionloss", indent);
  ss << print_typed_attr(api.springdamper, "mjc:springdamper", indent);
  ss << print_typed_attr(api.springref, "mjc:springref", indent);
  ss << print_typed_attr(api.ref, "mjc:ref", indent);
  ss << print_typed_attr(api.margin, "mjc:margin", indent);
  ss << print_typed_attr(api.solreflimit, "mjc:solreflimit", indent);
  ss << print_typed_attr(api.solimplimit, "mjc:solimplimit", indent);
  ss << print_typed_attr(api.solreffriction, "mjc:solreffriction", indent);
  ss << print_typed_attr(api.solimpfriction, "mjc:solimpfriction", indent);
  ss << print_typed_attr(api.actuatorfrcrange_min, "mjc:actuatorfrcrange:min", indent);
  ss << print_typed_attr(api.actuatorfrcrange_max, "mjc:actuatorfrcrange:max", indent);
  ss << print_typed_attr(api.actuatorfrclimited, "mjc:actuatorfrclimited", indent);
  ss << print_typed_attr(api.actuatorgravcomp, "mjc:actuatorgravcomp", indent);

  return pp_ss.str();
}

static std::string print_newton_scene_api(const NewtonSceneAPI &api, uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_typed_attr(api.maxSolverIterations, "newton:maxSolverIterations", indent);
  ss << print_typed_attr(api.timeStepsPerSecond, "newton:timeStepsPerSecond", indent);
  ss << print_typed_attr(api.gravityEnabled, "newton:gravityEnabled", indent);
  return pp_ss.str();
}

static std::string print_newton_xpbd_scene_api(const NewtonXpbdSceneAPI &api,
                                               uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_typed_attr(api.softBodyRelaxation, "newton:xpbd:softBodyRelaxation", indent);
  ss << print_typed_attr(api.softContactRelaxation, "newton:xpbd:softContactRelaxation", indent);
  ss << print_typed_attr(api.jointLinearRelaxation, "newton:xpbd:jointLinearRelaxation", indent);
  ss << print_typed_attr(api.jointAngularRelaxation, "newton:xpbd:jointAngularRelaxation", indent);
  ss << print_typed_attr(api.jointLinearCompliance, "newton:xpbd:jointLinearCompliance", indent);
  ss << print_typed_attr(api.jointAngularCompliance, "newton:xpbd:jointAngularCompliance", indent);
  ss << print_typed_attr(api.rigidContactRelaxation, "newton:xpbd:rigidContactRelaxation", indent);
  ss << print_typed_attr(api.rigidContactConWeighting, "newton:xpbd:rigidContactConWeighting", indent);
  ss << print_typed_attr(api.angularDamping, "newton:xpbd:angularDamping", indent);
  ss << print_typed_attr(api.restitutionEnabled, "newton:xpbd:restitutionEnabled", indent);
  return pp_ss.str();
}

static std::string print_newton_kamino_scene_api(const NewtonKaminoSceneAPI &api,
                                                 uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_typed_attr(api.padmmPrimalTolerance, "newton:kamino:padmm:primalTolerance", indent);
  ss << print_typed_attr(api.padmmDualTolerance, "newton:kamino:padmm:dualTolerance", indent);
  ss << print_typed_attr(api.padmmComplementarityTolerance, "newton:kamino:padmm:complementarityTolerance", indent);
  ss << print_typed_attr(api.padmmWarmstarting, "newton:kamino:padmm:warmstarting", indent);
  ss << print_typed_attr(api.padmmUseAcceleration, "newton:kamino:padmm:useAcceleration", indent);
  ss << print_typed_attr(api.constraintsUsePreconditioning, "newton:kamino:constraints:usePreconditioning", indent);
  ss << print_typed_attr(api.constraintsAlpha, "newton:kamino:constraints:alpha", indent);
  ss << print_typed_attr(api.constraintsBeta, "newton:kamino:constraints:beta", indent);
  ss << print_typed_attr(api.constraintsGamma, "newton:kamino:constraints:gamma", indent);
  ss << print_typed_attr(api.jointCorrection, "newton:kamino:jointCorrection", indent);
  return pp_ss.str();
}

static std::string print_newton_mimic_api(const NewtonMimicAPI &api, uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_typed_attr(api.mimicEnabled, "newton:mimicEnabled", indent);
  ss << print_rel_prop(api.mimicJoint, "newton:mimicJoint", indent);
  ss << print_typed_attr(api.mimicCoef0, "newton:mimicCoef0", indent);
  ss << print_typed_attr(api.mimicCoef1, "newton:mimicCoef1", indent);
  return pp_ss.str();
}

// Helper: print PhysicsJointBase attributes
template <typename JointT>
static std::string print_joint_base(const JointT &joint, uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  ss << print_rel_prop(joint.body0, "physics:body0", indent);
  ss << print_rel_prop(joint.body1, "physics:body1", indent);
  ss << print_typed_attr(joint.localPos0, "physics:localPos0", indent);
  ss << print_typed_attr(joint.localPos1, "physics:localPos1", indent);
  ss << print_typed_attr(joint.localRot0, "physics:localRot0", indent);
  ss << print_typed_attr(joint.localRot1, "physics:localRot1", indent);
  ss << print_typed_attr(joint.jointEnabled, "physics:jointEnabled", indent);
  ss << print_typed_attr(joint.collisionEnabled, "physics:collisionEnabled", indent);
  ss << print_typed_attr(joint.breakForce, "physics:breakForce", indent);
  ss << print_typed_attr(joint.breakTorque, "physics:breakTorque", indent);
  ss << print_typed_attr(joint.excludeFromArticulation, "physics:excludeFromArticulation", indent);

  if (joint.mjcJoint.has_value()) {
    ss << print_mjc_joint_api(joint.mjcJoint.value(), indent);
  }
  if (joint.newtonMimic.has_value()) {
    ss << print_newton_mimic_api(joint.newtonMimic.value(), indent);
  }

  return pp_ss.str();
}

// Macro for common joint prim structure
#define PRINT_PRIM_HEADER(prim, type_name)                                       \
  do {                                                                            \
    ss << pprint::Indent(indent) << to_string(prim.spec) << " " << type_name     \
       << " \"" << prim.name << "\"\n";                                           \
    if (prim.meta.authored()) {                                                   \
      ss << pprint::Indent(indent) << "(\n";                                      \
      ss << print_prim_metas(prim.meta, indent + 1);                              \
      ss << pprint::Indent(indent) << ")\n";                                      \
    }                                                                             \
    ss << pprint::Indent(indent) << "{\n";                                        \
  } while (0)

#define PRINT_PRIM_FOOTER(prim)                                                   \
  do {                                                                            \
    ss << print_props(prim.props, indent + 1);                                    \
    if (closing_brace) {                                                          \
      ss << pprint::Indent(indent) << "}\n";                                      \
    }                                                                             \
  } while (0)

std::string to_string(const PhysicsScene &scene, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(scene, "PhysicsScene");

  ss << print_typed_attr(scene.gravityDirection, "physics:gravityDirection", indent + 1);
  ss << print_typed_attr(scene.gravityMagnitude, "physics:gravityMagnitude", indent + 1);

  if (scene.mjcScene.has_value()) {
    ss << print_mjc_scene_api(scene.mjcScene.value(), indent + 1);
  }
  if (scene.newtonScene.has_value()) {
    ss << print_newton_scene_api(scene.newtonScene.value(), indent + 1);
  }
  if (scene.newtonXpbdScene.has_value()) {
    ss << print_newton_xpbd_scene_api(scene.newtonXpbdScene.value(), indent + 1);
  }
  if (scene.newtonKaminoScene.has_value()) {
    ss << print_newton_kamino_scene_api(scene.newtonKaminoScene.value(), indent + 1);
  }

  PRINT_PRIM_FOOTER(scene);
  return pp_ss.str();
}

std::string to_string(const PhysicsRevoluteJoint &joint, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(joint, "PhysicsRevoluteJoint");

  ss << print_typed_attr(joint.axis, "physics:axis", indent + 1);
  ss << print_typed_attr(joint.lowerLimit, "physics:lowerLimit", indent + 1);
  ss << print_typed_attr(joint.upperLimit, "physics:upperLimit", indent + 1);
  ss << print_joint_base(joint, indent + 1);

  PRINT_PRIM_FOOTER(joint);
  return pp_ss.str();
}

std::string to_string(const PhysicsPrismaticJoint &joint, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(joint, "PhysicsPrismaticJoint");

  ss << print_typed_attr(joint.axis, "physics:axis", indent + 1);
  ss << print_typed_attr(joint.lowerLimit, "physics:lowerLimit", indent + 1);
  ss << print_typed_attr(joint.upperLimit, "physics:upperLimit", indent + 1);
  ss << print_joint_base(joint, indent + 1);

  PRINT_PRIM_FOOTER(joint);
  return pp_ss.str();
}

std::string to_string(const PhysicsSphericalJoint &joint, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(joint, "PhysicsSphericalJoint");

  ss << print_typed_attr(joint.axis, "physics:axis", indent + 1);
  ss << print_typed_attr(joint.coneAngle0Limit, "physics:coneAngle0Limit", indent + 1);
  ss << print_typed_attr(joint.coneAngle1Limit, "physics:coneAngle1Limit", indent + 1);
  ss << print_joint_base(joint, indent + 1);

  PRINT_PRIM_FOOTER(joint);
  return pp_ss.str();
}

std::string to_string(const PhysicsJoint &joint, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(joint, "PhysicsJoint");

  ss << print_joint_base(joint, indent + 1);

  PRINT_PRIM_FOOTER(joint);
  return pp_ss.str();
}

std::string to_string(const PhysicsFixedJoint &joint, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(joint, "PhysicsFixedJoint");

  ss << print_joint_base(joint, indent + 1);

  PRINT_PRIM_FOOTER(joint);
  return pp_ss.str();
}

std::string to_string(const PhysicsDistanceJoint &joint, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(joint, "PhysicsDistanceJoint");

  ss << print_typed_attr(joint.minDistance, "physics:minDistance", indent + 1);
  ss << print_typed_attr(joint.maxDistance, "physics:maxDistance", indent + 1);
  ss << print_joint_base(joint, indent + 1);

  PRINT_PRIM_FOOTER(joint);
  return pp_ss.str();
}

std::string to_string(const PhysicsCollisionGroup &group, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(group, "PhysicsCollisionGroup");

  ss << print_typed_attr(group.mergeGroup, "physics:mergeGroup", indent + 1);
  ss << print_typed_attr(group.invertFilteredGroups, "physics:invertFilteredGroups", indent + 1);
  ss << print_rel_prop(group.filteredGroups, "physics:filteredGroups", indent + 1);

  PRINT_PRIM_FOOTER(group);
  return pp_ss.str();
}

std::string to_string(const MjcActuator &actuator, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(actuator, "MjcActuator");

  ss << print_typed_attr(actuator.group, "mjc:group", indent + 1);
  ss << print_rel_prop(actuator.target, "mjc:target", indent + 1);

  ss << print_typed_attr(actuator.ctrlLimited, "mjc:ctrlLimited", indent + 1);
  ss << print_typed_attr(actuator.forceLimited, "mjc:forceLimited", indent + 1);
  ss << print_typed_attr(actuator.actLimited, "mjc:actLimited", indent + 1);
  ss << print_typed_attr(actuator.ctrlRange_min, "mjc:ctrlRange:min", indent + 1);
  ss << print_typed_attr(actuator.ctrlRange_max, "mjc:ctrlRange:max", indent + 1);
  ss << print_typed_attr(actuator.forceRange_min, "mjc:forceRange:min", indent + 1);
  ss << print_typed_attr(actuator.forceRange_max, "mjc:forceRange:max", indent + 1);
  ss << print_typed_attr(actuator.actRange_min, "mjc:actRange:min", indent + 1);
  ss << print_typed_attr(actuator.actRange_max, "mjc:actRange:max", indent + 1);
  ss << print_typed_attr(actuator.lengthRange_min, "mjc:lengthRange:min", indent + 1);
  ss << print_typed_attr(actuator.lengthRange_max, "mjc:lengthRange:max", indent + 1);

  ss << print_typed_attr(actuator.gear, "mjc:gear", indent + 1);
  ss << print_typed_attr(actuator.crankLength, "mjc:crankLength", indent + 1);
  ss << print_typed_attr(actuator.jointInParent, "mjc:jointInParent", indent + 1);
  ss << print_rel_prop(actuator.refSite, "mjc:refSite", indent + 1);
  ss << print_rel_prop(actuator.sliderSite, "mjc:sliderSite", indent + 1);

  ss << print_typed_attr(actuator.actDim, "mjc:actDim", indent + 1);
  ss << print_typed_attr(actuator.dynType, "mjc:dynType", indent + 1);
  ss << print_typed_attr(actuator.gainType, "mjc:gainType", indent + 1);
  ss << print_typed_attr(actuator.biasType, "mjc:biasType", indent + 1);
  ss << print_typed_attr(actuator.dynPrm, "mjc:dynPrm", indent + 1);
  ss << print_typed_attr(actuator.gainPrm, "mjc:gainPrm", indent + 1);
  ss << print_typed_attr(actuator.biasPrm, "mjc:biasPrm", indent + 1);
  ss << print_typed_attr(actuator.actEarly, "mjc:actEarly", indent + 1);
  ss << print_typed_attr(actuator.inheritRange, "mjc:inheritRange", indent + 1);
  ss << print_typed_attr(actuator.plugin, "mjc:plugin", indent + 1);
  ss << print_typed_attr(actuator.instance, "mjc:instance", indent + 1);

  PRINT_PRIM_FOOTER(actuator);
  return pp_ss.str();
}

std::string to_string(const NewtonActuator &actuator, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(actuator, "NewtonActuator");

  ss << print_rel_prop(actuator.targets, "newton:targets", indent + 1);
  ss << print_typed_attr(actuator.delaySteps, "newton:delaySteps", indent + 1);
  ss << print_typed_attr(actuator.constEffort, "newton:constEffort", indent + 1);
  ss << print_typed_attr(actuator.kp, "newton:kp", indent + 1);
  ss << print_typed_attr(actuator.kd, "newton:kd", indent + 1);
  ss << print_typed_attr(actuator.ki, "newton:ki", indent + 1);
  ss << print_typed_attr(actuator.integralMax, "newton:integralMax", indent + 1);
  ss << print_typed_attr(actuator.modelPath, "newton:modelPath", indent + 1);
  ss << print_typed_attr(actuator.maxEffort, "newton:maxEffort", indent + 1);
  ss << print_typed_attr(actuator.maxMotorEffort, "newton:maxMotorEffort", indent + 1);
  ss << print_typed_attr(actuator.saturationEffort, "newton:saturationEffort", indent + 1);
  ss << print_typed_attr(actuator.velocityLimit, "newton:velocityLimit", indent + 1);
  ss << print_typed_attr(actuator.lookupPositions, "newton:lookupPositions", indent + 1);
  ss << print_typed_attr(actuator.lookupEfforts, "newton:lookupEfforts", indent + 1);

  PRINT_PRIM_FOOTER(actuator);
  return pp_ss.str();
}

std::string to_string(const MjcTendon &tendon, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(tendon, "MjcTendon");

  ss << print_typed_attr(tendon.type, "mjc:type", indent + 1);
  ss << print_rel_prop(tendon.path, "mjc:path", indent + 1);
  ss << print_rel_prop(tendon.sideSites, "mjc:sideSites", indent + 1);
  ss << print_typed_attr(tendon.path_indices, "mjc:path:indices", indent + 1);
  ss << print_typed_attr(tendon.sideSites_indices, "mjc:sideSites:indices", indent + 1);
  ss << print_typed_attr(tendon.path_segments, "mjc:path:segments", indent + 1);
  ss << print_typed_attr(tendon.path_divisors, "mjc:path:divisors", indent + 1);
  ss << print_typed_attr(tendon.path_coef, "mjc:path:coef", indent + 1);

  ss << print_typed_attr(tendon.group, "mjc:group", indent + 1);
  ss << print_typed_attr(tendon.limited, "mjc:limited", indent + 1);
  ss << print_typed_attr(tendon.actuatorfrclimited, "mjc:actuatorfrclimited", indent + 1);
  ss << print_typed_attr(tendon.range_min, "mjc:range:min", indent + 1);
  ss << print_typed_attr(tendon.range_max, "mjc:range:max", indent + 1);
  ss << print_typed_attr(tendon.actuatorfrcrange_min, "mjc:actuatorfrcrange:min", indent + 1);
  ss << print_typed_attr(tendon.actuatorfrcrange_max, "mjc:actuatorfrcrange:max", indent + 1);

  ss << print_typed_attr(tendon.solreflimit, "mjc:solreflimit", indent + 1);
  ss << print_typed_attr(tendon.solimplimit, "mjc:solimplimit", indent + 1);
  ss << print_typed_attr(tendon.solreffriction, "mjc:solreffriction", indent + 1);
  ss << print_typed_attr(tendon.solimpfriction, "mjc:solimpfriction", indent + 1);

  ss << print_typed_attr(tendon.margin, "mjc:margin", indent + 1);
  ss << print_typed_attr(tendon.frictionloss, "mjc:frictionloss", indent + 1);
  ss << print_typed_attr(tendon.springlength, "mjc:springlength", indent + 1);
  ss << print_typed_attr(tendon.stiffness, "mjc:stiffness", indent + 1);
  ss << print_typed_attr(tendon.damping, "mjc:damping", indent + 1);
  ss << print_typed_attr(tendon.armature, "mjc:armature", indent + 1);

  ss << print_typed_attr(tendon.width, "mjc:width", indent + 1);
  ss << print_typed_attr(tendon.rgba, "mjc:rgba", indent + 1);

  PRINT_PRIM_FOOTER(tendon);
  return pp_ss.str();
}

std::string to_string(const MjcKeyframe &keyframe, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(keyframe, "MjcKeyframe");

  ss << print_typed_attr(keyframe.qpos, "mjc:qpos", indent + 1);
  ss << print_typed_attr(keyframe.qvel, "mjc:qvel", indent + 1);
  ss << print_typed_attr(keyframe.act, "mjc:act", indent + 1);
  ss << print_typed_attr(keyframe.ctrl, "mjc:ctrl", indent + 1);
  ss << print_typed_attr(keyframe.mpos, "mjc:mpos", indent + 1);
  ss << print_typed_attr(keyframe.mquat, "mjc:mquat", indent + 1);

  PRINT_PRIM_FOOTER(keyframe);
  return pp_ss.str();
}

std::string to_string(const MjcSensor &sensor, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();

  PRINT_PRIM_HEADER(sensor, "MjcSensor");

  ss << print_typed_attr(sensor.type, "mjc:type", indent + 1);
  ss << print_typed_attr(sensor.objType, "mjc:objtype", indent + 1);
  ss << print_typed_attr(sensor.objName, "mjc:objname", indent + 1);
  ss << print_typed_attr(sensor.refType, "mjc:reftype", indent + 1);
  ss << print_typed_attr(sensor.refName, "mjc:refname", indent + 1);
  ss << print_typed_attr(sensor.group, "mjc:group", indent + 1);
  ss << print_typed_attr(sensor.cutoff, "mjc:cutoff", indent + 1);
  ss << print_typed_attr(sensor.noise, "mjc:noise", indent + 1);
  ss << print_typed_attr(sensor.user, "mjc:user", indent + 1);

  PRINT_PRIM_FOOTER(sensor);
  return pp_ss.str();
}

#undef PRINT_PRIM_HEADER
#undef PRINT_PRIM_FOOTER

}  // namespace lightusd
