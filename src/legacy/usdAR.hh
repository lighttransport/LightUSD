// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
/// @file usdAR.hh
/// @brief Apple Preliminary AR schema definitions for USDZ
///
/// Implements the Preliminary_* schemas from Apple's USDZ Interactive and
/// USDZ Physics schema extensions for augmented reality.
///
/// Typed schemas (standalone prims):
/// - Preliminary_PhysicsGravitationalForce: Gravity force
/// - Preliminary_InfiniteColliderPlane: Infinite collision plane
/// - Preliminary_ReferenceImage: Image anchor reference
/// - Preliminary_Behavior: Interactive behavior (triggers + actions)
/// - Preliminary_Trigger: Event trigger for behaviors
/// - Preliminary_Action: Action performed by triggers
/// - Preliminary_Text: 3D extruded text geometry
///
/// API schemas (applied to host prims):
/// - Preliminary_AnchoringAPI: AR anchoring (plane/image/face)
/// - Preliminary_PhysicsMaterialAPI: Physical material properties
/// - Preliminary_PhysicsRigidBodyAPI: Rigid body properties
/// - Preliminary_PhysicsColliderAPI: Collision shape
///
#pragma once

#include "value-types.hh"
#include "core/prim-enums.hh"
#include "core/path.hh"
#include "core/extent.hh"
#include "core/composition-types.hh"
#include "core/prim-metas.hh"
#include "core/typed-attribute.hh"
#include "core/relationship.hh"
#include "core/property.hh"
#include "core/variant-types.hh"

namespace lightusd {

// Prim type name constants (typed schemas only)
constexpr auto kPreliminary_PhysicsGravitationalForce = "Preliminary_PhysicsGravitationalForce";
constexpr auto kPreliminary_InfiniteColliderPlane = "Preliminary_InfiniteColliderPlane";
constexpr auto kPreliminary_ReferenceImage = "Preliminary_ReferenceImage";
constexpr auto kPreliminary_Behavior = "Preliminary_Behavior";
constexpr auto kPreliminary_Trigger = "Preliminary_Trigger";
constexpr auto kPreliminary_Action = "Preliminary_Action";
constexpr auto kPreliminary_Text = "Preliminary_Text";

//
// API schemas (applied to host prims, not standalone)
//

struct Preliminary_AnchoringAPI {
  // preliminary:anchoring:type — "plane", "image", "face", "none"
  TypedAttribute<value::token> type;
  // preliminary:planeAnchoring:alignment — "horizontal", "vertical", "any"
  TypedAttribute<value::token> alignment;
  // preliminary:imageAnchoring:referenceImage
  RelationshipProperty referenceImage;
};

struct Preliminary_PhysicsMaterialAPI {
  TypedAttribute<double> restitution;       // preliminary:physics:material:restitution
  TypedAttribute<double> friction_static;   // preliminary:physics:material:friction:static
  TypedAttribute<double> friction_dynamic;  // preliminary:physics:material:friction:dynamic
};

struct Preliminary_PhysicsRigidBodyAPI {
  TypedAttributeWithFallback<double> mass{1.0};       // preliminary:physics:rigidBody:mass
  TypedAttributeWithFallback<bool> initiallyActive{true};  // preliminary:physics:rigidBody:initiallyActive
};

struct Preliminary_PhysicsColliderAPI {
  RelationshipProperty convexShape;  // preliminary:physics:collider:convexShape
};

//
// Typed schemas (standalone prims)
//

struct Preliminary_PhysicsGravitationalForce {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  // physics:gravitationalForce:acceleration [m/s^2]
  TypedAttributeWithFallback<value::vector3d> acceleration{value::vector3d{0.0, -9.81, 0.0}};

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }
  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

struct Preliminary_InfiniteColliderPlane {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttributeWithFallback<value::point3d> position{value::point3d{0.0, 0.0, 0.0}};
  TypedAttributeWithFallback<value::vector3d> normal{value::vector3d{0.0, 1.0, 0.0}};

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }
  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

struct Preliminary_ReferenceImage {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<value::AssetPath> image;            // uniform asset image
  TypedAttributeWithFallback<double> physicalWidth{0.0};  // uniform double physicalWidth [cm]

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }
  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

struct Preliminary_Behavior {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  RelationshipProperty triggers;  // rel triggers
  RelationshipProperty actions;   // rel actions
  TypedAttributeWithFallback<bool> exclusive{false};  // uniform bool exclusive

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }
  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

struct Preliminary_Trigger {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<value::token> info_id;  // uniform token info:id

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }
  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

struct Preliminary_Action {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<value::token> info_id;  // uniform token info:id
  // "ignore", "allow", "stop"
  TypedAttributeWithFallback<value::token> multiplePerformOperation{value::token("ignore")};

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }
  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

struct Preliminary_Text {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttributeWithFallback<std::string> content{""};
  TypedAttribute<std::vector<std::string>> font;  // string[] font
  TypedAttributeWithFallback<float> pointSize{144.0f};
  TypedAttribute<float> width;
  TypedAttribute<float> height;
  TypedAttributeWithFallback<float> depth{0.0f};
  // "singleLine", "hardBreaks", "flowing"
  TypedAttributeWithFallback<value::token> wrapMode{value::token("flowing")};
  // "left", "center", "right", "justified"
  TypedAttributeWithFallback<value::token> horizontalAlignment{value::token("center")};
  // "top", "middle", "lowerMiddle", "baseline", "bottom"
  TypedAttributeWithFallback<value::token> verticalAlignment{value::token("middle")};

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() LIGHTUSD_LIFETIMEBOUND { return meta; }
  const PrimMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return meta; }

  const std::vector<value::token> &primChildrenNames() const LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  const std::vector<value::token> &propertyNames() const LIGHTUSD_LIFETIMEBOUND { return _properties; }
  std::vector<value::token> &primChildrenNames() LIGHTUSD_LIFETIMEBOUND { return _primChildren; }
  std::vector<value::token> &propertyNames() LIGHTUSD_LIFETIMEBOUND { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

namespace value {

#include "define-type-trait.inc"
DEFINE_TYPE_TRAIT(Preliminary_PhysicsGravitationalForce, kPreliminary_PhysicsGravitationalForce, TYPE_ID_PRELIMINARY_GRAVITATIONAL_FORCE, 1);
DEFINE_TYPE_TRAIT(Preliminary_InfiniteColliderPlane, kPreliminary_InfiniteColliderPlane, TYPE_ID_PRELIMINARY_INFINITE_COLLIDER_PLANE, 1);
DEFINE_TYPE_TRAIT(Preliminary_ReferenceImage, kPreliminary_ReferenceImage, TYPE_ID_PRELIMINARY_REFERENCE_IMAGE, 1);
DEFINE_TYPE_TRAIT(Preliminary_Behavior, kPreliminary_Behavior, TYPE_ID_PRELIMINARY_BEHAVIOR, 1);
DEFINE_TYPE_TRAIT(Preliminary_Trigger, kPreliminary_Trigger, TYPE_ID_PRELIMINARY_TRIGGER, 1);
DEFINE_TYPE_TRAIT(Preliminary_Action, kPreliminary_Action, TYPE_ID_PRELIMINARY_ACTION, 1);
DEFINE_TYPE_TRAIT(Preliminary_Text, kPreliminary_Text, TYPE_ID_PRELIMINARY_TEXT, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
