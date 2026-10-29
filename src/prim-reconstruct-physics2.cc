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

// MjcActuator / NewtonActuator / MjcTendon / MjcKeyframe / PhysicsCollisionGroup
// (split from prim-reconstruct-physics.cc to parallelize the back-end codegen).
template <>
bool ReconstructPrim<MjcActuator>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    MjcActuator *actuator, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ actuator
  for (auto &prop : properties) {
    MJC_ACTUATOR_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    MJC_ACTUATOR_RELS(EXPAND_SINGLE_REL)
    ADD_PROPERTY(table, prop, MjcActuator, actuator->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// NewtonActuator
// ============================================================================
template <>
bool ReconstructPrim<NewtonActuator>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    NewtonActuator *actuator, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ actuator
  for (auto &prop : properties) {
    NEWTON_ACTUATOR_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    NEWTON_ACTUATOR_RELS(EXPAND_MULTI_REL)
    ADD_PROPERTY(table, prop, NewtonActuator, actuator->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// MjcTendon
// ============================================================================
template <>
bool ReconstructPrim<MjcTendon>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    MjcTendon *tendon, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ tendon
  for (auto &prop : properties) {
    MJC_TENDON_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    // MJC_TENDON_RELS (mjc:path / mjc:sideSites) are multi-target rels:
    // a `<fixed>` tendon (e.g. Panda's finger linkage) lists 2+ joint
    // paths, and a `<spatial>` tendon lists N path/site segments.
    // Use the path-vector parser; single targets still parse cleanly.
    MJC_TENDON_RELS(EXPAND_MULTI_REL)
    ADD_PROPERTY(table, prop, MjcTendon, tendon->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// MjcKeyframe
// ============================================================================
template <>
bool ReconstructPrim<MjcKeyframe>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    MjcKeyframe *keyframe, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ keyframe
  for (auto &prop : properties) {
    MJC_KEYFRAME_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, MjcKeyframe, keyframe->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// MjcSensor
// ============================================================================
template <>
bool ReconstructPrim<MjcSensor>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    MjcSensor *sensor, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ sensor
  for (auto &prop : properties) {
    MJC_SENSOR_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, MjcSensor, sensor->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// PhysicsCollisionGroup
// ============================================================================
template <>
bool ReconstructPrim<PhysicsCollisionGroup>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    PhysicsCollisionGroup *group, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ group
  for (auto &prop : properties) {
    PHYSICS_COLLISION_GROUP_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PHYSICS_COLLISION_GROUP_RELS(EXPAND_SINGLE_REL)
    ADD_PROPERTY(table, prop, PhysicsCollisionGroup, group->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

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

RECONSTRUCT_PRIM_PRIMSPEC_IMPL(PhysicsCollisionGroup)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(MjcActuator)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(NewtonActuator)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(MjcTendon)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(MjcKeyframe)
RECONSTRUCT_PRIM_PRIMSPEC_IMPL(MjcSensor)

}  // namespace prim
}  // namespace lightusd
