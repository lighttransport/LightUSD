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

template <>
bool ReconstructPrim<PhysicsRevoluteJoint>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    PhysicsRevoluteJoint *joint, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references;
  ParsePropertyTable table;
  if (!ReconstructJointBaseProperties(table, properties, joint, warn, err, options)) return false;
#define PRIM_PTR_ joint
  for (auto &prop : properties) {
    PHYSICS_JOINT_BASE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PHYSICS_REVOLUTE_JOINT_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PHYSICS_JOINT_BASE_RELS(EXPAND_SINGLE_REL)
    if (joint->mjcJoint.has_value()) { MJC_JOINT_TYPED_ATTRS(EXPAND_TYPED_ATTR) }
    if (joint->newtonMimic.has_value()) {
      NEWTON_MIMIC_TYPED_ATTRS(EXPAND_TYPED_ATTR)
      NEWTON_MIMIC_RELS(EXPAND_SINGLE_REL)
    }
    ADD_PROPERTY(table, prop, PhysicsRevoluteJoint, joint->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// PhysicsPrismaticJoint
// ============================================================================
template <>
bool ReconstructPrim<PhysicsPrismaticJoint>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    PhysicsPrismaticJoint *joint, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references;
  ParsePropertyTable table;
  if (!ReconstructJointBaseProperties(table, properties, joint, warn, err, options)) return false;
#define PRIM_PTR_ joint
  for (auto &prop : properties) {
    PHYSICS_JOINT_BASE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PHYSICS_PRISMATIC_JOINT_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PHYSICS_JOINT_BASE_RELS(EXPAND_SINGLE_REL)
    if (joint->mjcJoint.has_value()) { MJC_JOINT_TYPED_ATTRS(EXPAND_TYPED_ATTR) }
    if (joint->newtonMimic.has_value()) {
      NEWTON_MIMIC_TYPED_ATTRS(EXPAND_TYPED_ATTR)
      NEWTON_MIMIC_RELS(EXPAND_SINGLE_REL)
    }
    ADD_PROPERTY(table, prop, PhysicsPrismaticJoint, joint->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// PhysicsSphericalJoint
// ============================================================================
template <>
bool ReconstructPrim<PhysicsSphericalJoint>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    PhysicsSphericalJoint *joint, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references;
  ParsePropertyTable table;
  if (!ReconstructJointBaseProperties(table, properties, joint, warn, err, options)) return false;
#define PRIM_PTR_ joint
  for (auto &prop : properties) {
    PHYSICS_JOINT_BASE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PHYSICS_SPHERICAL_JOINT_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PHYSICS_JOINT_BASE_RELS(EXPAND_SINGLE_REL)
    if (joint->mjcJoint.has_value()) { MJC_JOINT_TYPED_ATTRS(EXPAND_TYPED_ATTR) }
    if (joint->newtonMimic.has_value()) {
      NEWTON_MIMIC_TYPED_ATTRS(EXPAND_TYPED_ATTR)
      NEWTON_MIMIC_RELS(EXPAND_SINGLE_REL)
    }
    ADD_PROPERTY(table, prop, PhysicsSphericalJoint, joint->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// PhysicsFixedJoint
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

RECONSTRUCT_PRIM_PRIMSPEC_IMPL(PhysicsRevoluteJoint)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(PhysicsPrismaticJoint)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(PhysicsSphericalJoint)

}  // namespace prim
}  // namespace lightusd
