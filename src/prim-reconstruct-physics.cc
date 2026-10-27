// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Physics + MuJoCo prim reconstruction specializations.
// Split from prim-reconstruct.cc
//
#include "prim-reconstruct.hh"

#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "tiny-format.hh"
#include "enum-handlers.hh"
#include "prim-property-tables.hh"

#include "usdPhysics.hh"
#include "mjcPhysics.hh"
#include "newtonPhysics.hh"

#include "common-macros.inc"
#include "value-types.hh"

// For PUSH_ERROR_AND_RETURN
#define PushError(s) \
  if (err) { \
    (*err) = (s) + (err->empty() ? std::string() : std::string("\n")) + (*err); \
  }
#define PushWarn(s) \
  if (warn) { \
    (*warn) = (s) + (warn->empty() ? std::string() : std::string("\n")) + (*warn); \
  }

#define PUSH_WARN_F(s, ...) PUSH_WARN(fmt::format(s, __VA_ARGS__))

namespace lightusd {
namespace prim {

// Suppress unused-function warnings from enum handlers in the .inc that
// physics reconstruction does not use (e.g. PurposeEnumHandler).
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "prim-reconstruct-common.inc"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "prim-reconstruct-physics-detail.inc"

// ============================================================================
// PhysicsJoint (generic D6 joint)
// ============================================================================
template <>
bool ReconstructPrim<PhysicsJoint>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    PhysicsJoint *joint, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references;
  ParsePropertyTable table;
  if (!ReconstructJointBaseProperties(table, properties, joint, warn, err, options)) return false;
#define PRIM_PTR_ joint
  for (auto &prop : properties) {
    PHYSICS_JOINT_BASE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PHYSICS_JOINT_BASE_RELS(EXPAND_SINGLE_REL)
    if (joint->mjcJoint.has_value()) { MJC_JOINT_TYPED_ATTRS(EXPAND_TYPED_ATTR) }
    if (joint->newtonMimic.has_value()) {
      NEWTON_MIMIC_TYPED_ATTRS(EXPAND_TYPED_ATTR)
      NEWTON_MIMIC_RELS(EXPAND_SINGLE_REL)
    }
    ADD_PROPERTY(table, prop, PhysicsJoint, joint->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// PhysicsScene
// ============================================================================
template <>
bool ReconstructPrim<PhysicsScene>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    PhysicsScene *scene,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {

  (void)spec;
  (void)references;
  (void)options;

  ParsePropertyTable table;

  if (!scene->mjcScene.has_value() && HasPropertyPrefix(properties, "mjc:")) {
    scene->mjcScene = MjcSceneAPI();
  }
  if (!scene->newtonScene.has_value() && (
      HasPropertyPrefix(properties, "newton:maxSolverIterations") ||
      HasPropertyPrefix(properties, "newton:timeStepsPerSecond") ||
      HasPropertyPrefix(properties, "newton:gravityEnabled"))) {
    scene->newtonScene = NewtonSceneAPI();
  }
  if (!scene->newtonXpbdScene.has_value() &&
      HasPropertyPrefix(properties, "newton:xpbd:")) {
    scene->newtonXpbdScene = NewtonXpbdSceneAPI();
  }
  if (!scene->newtonKaminoScene.has_value() &&
      HasPropertyPrefix(properties, "newton:kamino:")) {
    scene->newtonKaminoScene = NewtonKaminoSceneAPI();
  }

#define PRIM_PTR_ scene

  for (auto &prop : properties) {
    PHYSICS_SCENE_TYPED_ATTRS(EXPAND_TYPED_ATTR)

    if (scene->mjcScene.has_value()) {
      MJC_SCENE_OPTION_TYPED_ATTRS(EXPAND_TYPED_ATTR)
      MJC_SCENE_FLAG_TYPED_ATTRS(EXPAND_TYPED_ATTR)
      MJC_SCENE_COMPILER_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    }
    if (scene->newtonScene.has_value()) {
      NEWTON_SCENE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    }
    if (scene->newtonXpbdScene.has_value()) {
      NEWTON_XPBD_SCENE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    }
    if (scene->newtonKaminoScene.has_value()) {
      NEWTON_KAMINO_SCENE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    }

    ADD_PROPERTY(table, prop, PhysicsScene, scene->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

#undef PRIM_PTR_

  return true;
}

// ============================================================================
// PhysicsRevoluteJoint
// ============================================================================

// ============================================================================
// PrimSpec wrappers
// ============================================================================

#define RECONSTRUCT_PRIM_PRIMSPEC_IMPL(__prim_ty) \
template <> \
bool ReconstructPrim<__prim_ty>( \
    PrimSpec &primspec, \
    __prim_ty *prim, \
    std::string *warn, \
    std::string *err, \
    const PrimReconstructOptions &options) { \
 \
  ReferenceList references; /* dummy */ \
 \
  return ReconstructPrim<__prim_ty>(primspec.specifier(), primspec.props(), references, prim, warn, err, options); \
}

RECONSTRUCT_PRIM_PRIMSPEC_IMPL(PhysicsScene)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(PhysicsJoint)

}  // namespace prim
}  // namespace lightusd
