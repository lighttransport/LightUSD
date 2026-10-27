// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// AR/Interactive prim reconstruction specializations (Apple Preliminary_* schemas).
//
#include "prim-reconstruct.hh"

#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "tiny-format.hh"
#include "enum-handlers.hh"
#include "prim-property-tables.hh"

#include "usdAR.hh"

#include "common-macros.inc"
#include "value-types.hh"

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

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "prim-reconstruct-common.inc"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

// ============================================================================
// Preliminary_PhysicsGravitationalForce
// ============================================================================
template <>
bool ReconstructPrim<Preliminary_PhysicsGravitationalForce>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    Preliminary_PhysicsGravitationalForce *prim, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ prim
  for (auto &prop : properties) {
    PRELIMINARY_GRAVITATIONAL_FORCE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, Preliminary_PhysicsGravitationalForce, prim->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// Preliminary_InfiniteColliderPlane
// ============================================================================
template <>
bool ReconstructPrim<Preliminary_InfiniteColliderPlane>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    Preliminary_InfiniteColliderPlane *prim, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ prim
  for (auto &prop : properties) {
    PRELIMINARY_INFINITE_COLLIDER_PLANE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, Preliminary_InfiniteColliderPlane, prim->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// Preliminary_ReferenceImage
// ============================================================================
template <>
bool ReconstructPrim<Preliminary_ReferenceImage>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    Preliminary_ReferenceImage *prim, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ prim
  for (auto &prop : properties) {
    PRELIMINARY_REFERENCE_IMAGE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, Preliminary_ReferenceImage, prim->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// Preliminary_Behavior
// ============================================================================
template <>
bool ReconstructPrim<Preliminary_Behavior>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    Preliminary_Behavior *prim, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ prim
  for (auto &prop : properties) {
    PRELIMINARY_BEHAVIOR_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PRELIMINARY_BEHAVIOR_RELS(EXPAND_SINGLE_REL)
    ADD_PROPERTY(table, prop, Preliminary_Behavior, prim->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// Preliminary_Trigger
// ============================================================================
template <>
bool ReconstructPrim<Preliminary_Trigger>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    Preliminary_Trigger *prim, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ prim
  for (auto &prop : properties) {
    PRELIMINARY_TRIGGER_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, Preliminary_Trigger, prim->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// Preliminary_Action
// ============================================================================
template <>
bool ReconstructPrim<Preliminary_Action>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    Preliminary_Action *prim, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ prim
  for (auto &prop : properties) {
    PRELIMINARY_ACTION_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, Preliminary_Action, prim->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

// ============================================================================
// Preliminary_Text
// ============================================================================
template <>
bool ReconstructPrim<Preliminary_Text>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    Preliminary_Text *prim, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec; (void)references; (void)options;
  ParsePropertyTable table;
#define PRIM_PTR_ prim
  for (auto &prop : properties) {
    PRELIMINARY_TEXT_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, Preliminary_Text, prim->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
#undef PRIM_PTR_
  return true;
}

}  // namespace prim
}  // namespace lightusd
