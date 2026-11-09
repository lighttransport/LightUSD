// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file usdPhysics.hh
/// @brief USD Physics schema definitions
///
/// Implements physics simulation primitives following USD's UsdPhysics schema.
/// Includes support for rigid bodies, collisions, joints, materials, and scenes.
///
/// Key classes:
/// - PhysicsScene: Global simulation settings (gravity, etc.)
/// - PhysicsRigidBodyAPI: Rigid body properties (applied API)
/// - PhysicsCollisionAPI: Collision properties (applied API)
/// - PhysicsMaterialAPI: Physical material properties (applied API)
/// - PhysicsMeshCollisionAPI: Mesh collision approximation (applied API)
/// - PhysicsRevoluteJoint, PhysicsPrismaticJoint, etc.: Joint types
///
#pragma once

#include "value-types.hh"
#include "nonstd/optional.hpp"
#include "nonstd/expected.hpp"
#include "core/prim-enums.hh"
#include "core/path.hh"
#include "core/extent.hh"
#include "core/composition-types.hh"
#include "core/prim-metas.hh"
#include "core/animatable.hh"
#include "core/typed-attribute.hh"
#include "core/relationship.hh"
#include "core/attribute.hh"
#include "core/property.hh"
#include "core/variant-types.hh"

#include <limits>

// Physics extension API schemas are used by value in optional<> fields, so
// full definitions are needed here.
#include "mjcPhysics.hh"
#include "newtonPhysics.hh"

namespace lightusd {

// Prim type name constants
constexpr auto kPhysicsScene = "PhysicsScene";
constexpr auto kPhysicsRevoluteJoint = "PhysicsRevoluteJoint";
constexpr auto kPhysicsPrismaticJoint = "PhysicsPrismaticJoint";
constexpr auto kPhysicsSphericalJoint = "PhysicsSphericalJoint";
constexpr auto kPhysicsFixedJoint = "PhysicsFixedJoint";
constexpr auto kPhysicsDistanceJoint = "PhysicsDistanceJoint";

//
// API Schema structs (applied to host prims, not standalone)
//

struct PhysicsRigidBodyAPI {
  TypedAttributeWithFallback<bool> rigidBodyEnabled{true};  // physics:rigidBodyEnabled
  TypedAttributeWithFallback<bool> kinematicEnabled{false};  // physics:kinematicEnabled
  RelationshipProperty simulationOwner;                      // physics:simulationOwner
  TypedAttribute<float> mass;                                // physics:mass
  TypedAttribute<float> density;                             // physics:density
  TypedAttribute<value::point3f> centerOfMass;               // physics:centerOfMass
  TypedAttribute<value::float3> diagonalInertia;             // physics:diagonalInertia
  TypedAttribute<value::quatf> principalAxes;                // physics:principalAxes
  TypedAttribute<Animatable<value::vector3f>> velocity;      // physics:velocity
  TypedAttribute<Animatable<value::vector3f>> angularVelocity; // physics:angularVelocity
  TypedAttributeWithFallback<bool> startsAsleep{false};      // physics:startsAsleep
};

struct PhysicsCollisionAPI {
  TypedAttributeWithFallback<bool> collisionEnabled{true};   // physics:collisionEnabled
  RelationshipProperty simulationOwner;                       // physics:simulationOwner
};

struct PhysicsMaterialAPI {
  TypedAttribute<float> staticFriction;    // physics:staticFriction
  TypedAttribute<float> dynamicFriction;   // physics:dynamicFriction
  TypedAttribute<float> restitution;       // physics:restitution
  TypedAttribute<float> density;           // physics:density
};

struct PhysicsMeshCollisionAPI {
  // "convexDecomposition", "convexHull", "meshSimplification",
  // "boundingCube", "boundingSphere", "none"
  TypedAttributeWithFallback<value::token> approximation{value::token("none")}; // physics:approximation
};

struct PhysicsMassAPI {
  TypedAttributeWithFallback<float> mass{0.0f};                // physics:mass
  TypedAttributeWithFallback<float> density_{0.0f};             // physics:density (name avoids collision with MaterialAPI)
  TypedAttributeWithFallback<value::point3f> centerOfMass{
      value::point3f{-std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity()}}; // physics:centerOfMass
  TypedAttributeWithFallback<value::float3> diagonalInertia{
      value::float3{{0.0f, 0.0f, 0.0f}}};                       // physics:diagonalInertia
  TypedAttributeWithFallback<value::quatf> principalAxes{
      value::quatf{{{0.0f, 0.0f, 0.0f}}, 0.0f}};                // physics:principalAxes
};

struct PhysicsFilteredPairsAPI {
  RelationshipProperty filteredPairs;  // physics:filteredPairs

  ///
  /// @brief Resolve the listed target paths.
  ///
  /// Empty vector if the rel has no value or has been blocked. Order
  /// follows the underlying rel's pathVector.
  ///
  std::vector<Path> get_filtered_pair_paths() const;
};

// PhysicsArticulationRootAPI — marker schema, no properties
struct PhysicsArticulationRootAPI {};

// PhysicsDriveAPI — multi-apply API schema for motorized joints.
// Applied per-DOF, e.g. apiSchemas = ["PhysicsDriveAPI:rotX"].
// Attributes are namespaced: physics:drive:<dof>:type, etc.
struct PhysicsDriveAPI {
  std::string dof;  // "transX", "transY", "transZ", "rotX", "rotY", "rotZ"

  // "force" or "acceleration"
  TypedAttributeWithFallback<value::token> type{value::token("force")}; // physics:drive:<dof>:type
  TypedAttributeWithFallback<float> maxForce{
      std::numeric_limits<float>::infinity()}; // physics:drive:<dof>:maxForce
  TypedAttributeWithFallback<float> targetPosition{0.0f}; // physics:drive:<dof>:targetPosition
  TypedAttributeWithFallback<float> targetVelocity{0.0f}; // physics:drive:<dof>:targetVelocity
  TypedAttributeWithFallback<float> damping{0.0f};        // physics:drive:<dof>:damping
  TypedAttributeWithFallback<float> stiffness{0.0f};      // physics:drive:<dof>:stiffness
};

// PhysicsLimitAPI — multi-apply API schema for per-DOF joint limits.
// Applied per-DOF, e.g. apiSchemas = ["PhysicsLimitAPI:rotX"].
// Attributes are namespaced: physics:limit:<dof>:low, etc.
struct PhysicsLimitAPI {
  std::string dof;  // "transX", "transY", "transZ", "rotX", "rotY", "rotZ", "distance"

  TypedAttributeWithFallback<float> low{
      -std::numeric_limits<float>::infinity()}; // physics:limit:<dof>:low
  TypedAttributeWithFallback<float> high{
      std::numeric_limits<float>::infinity()};  // physics:limit:<dof>:high
};

//
// Concrete Prim types
//

constexpr auto kPhysicsJoint = "PhysicsJoint";
constexpr auto kPhysicsCollisionGroup = "PhysicsCollisionGroup";

struct PhysicsScene {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  // Standard UsdPhysics attributes (varying — no uniform required)
  TypedAttribute<value::vector3f> gravityDirection;  // physics:gravityDirection (vector3f per USD spec)
  TypedAttribute<float> gravityMagnitude;            // physics:gravityMagnitude (float per USD spec)

  // MuJoCo scene API (optional)
  nonstd::optional<MjcSceneAPI> mjcScene;

  // Newton scene APIs (optional)
  nonstd::optional<NewtonSceneAPI> newtonScene;
  nonstd::optional<NewtonXpbdSceneAPI> newtonXpbdScene;
  nonstd::optional<NewtonKaminoSceneAPI> newtonKaminoScene;

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

// Base for all physics joint types
struct PhysicsJointBase {
  RelationshipProperty body0;      // physics:body0
  RelationshipProperty body1;      // physics:body1
  TypedAttributeWithFallback<value::point3f> localPos0{
      value::point3f{0.0f, 0.0f, 0.0f}};  // physics:localPos0
  TypedAttributeWithFallback<value::point3f> localPos1{
      value::point3f{0.0f, 0.0f, 0.0f}};  // physics:localPos1
  TypedAttributeWithFallback<value::quatf> localRot0{
      value::quatf{{{0.0f, 0.0f, 0.0f}}, 1.0f}};  // physics:localRot0
  TypedAttributeWithFallback<value::quatf> localRot1{
      value::quatf{{{0.0f, 0.0f, 0.0f}}, 1.0f}};  // physics:localRot1
  TypedAttributeWithFallback<bool> jointEnabled{true};      // physics:jointEnabled
  TypedAttributeWithFallback<bool> collisionEnabled{false};  // physics:collisionEnabled
  TypedAttributeWithFallback<float> breakForce{
      std::numeric_limits<float>::infinity()};  // physics:breakForce
  TypedAttributeWithFallback<float> breakTorque{
      std::numeric_limits<float>::infinity()};  // physics:breakTorque
  TypedAttributeWithFallback<bool> excludeFromArticulation{false};  // physics:excludeFromArticulation

  // Multi-apply API schemas (keyed by DOF name: "rotX", "transY", etc.)
  std::map<std::string, PhysicsDriveAPI> drives;   // PhysicsDriveAPI:*
  std::map<std::string, PhysicsLimitAPI> limits;    // PhysicsLimitAPI:*

  // MuJoCo joint API (optional)
  nonstd::optional<MjcJointAPI> mjcJoint;

  // Newton mimic joint API (optional)
  nonstd::optional<NewtonMimicAPI> newtonMimic;
};

// PhysicsJoint — generic D6 joint (all DOFs free by default)
struct PhysicsJoint : PhysicsJointBase {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

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

struct PhysicsRevoluteJoint : PhysicsJointBase {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<value::token> axis;   // physics:axis
  TypedAttribute<float> lowerLimit;    // physics:lowerLimit
  TypedAttribute<float> upperLimit;    // physics:upperLimit

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

struct PhysicsPrismaticJoint : PhysicsJointBase {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<value::token> axis;   // physics:axis
  TypedAttribute<float> lowerLimit;
  TypedAttribute<float> upperLimit;

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

struct PhysicsSphericalJoint : PhysicsJointBase {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<value::token> axis;            // physics:axis
  TypedAttribute<float> coneAngle0Limit;       // physics:coneAngle0Limit
  TypedAttribute<float> coneAngle1Limit;       // physics:coneAngle1Limit

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

struct PhysicsFixedJoint : PhysicsJointBase {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

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

struct PhysicsDistanceJoint : PhysicsJointBase {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<float> minDistance;  // physics:minDistance
  TypedAttribute<float> maxDistance;  // physics:maxDistance

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

// PhysicsCollisionGroup — concrete prim for collision filtering
struct PhysicsCollisionGroup {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return name; }
  Specifier &specifier() LIGHTUSD_LIFETIMEBOUND { return spec; }
  const Specifier &specifier() const LIGHTUSD_LIFETIMEBOUND { return spec; }

  TypedAttribute<value::token> mergeGroup;             // physics:mergeGroup
  TypedAttributeWithFallback<bool> invertFilteredGroups{false}; // physics:invertFilteredGroups
  RelationshipProperty filteredGroups;                  // physics:filteredGroups

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

// ---------------------------------------------------------------
// Typed accessors for collision-filtering schemas.
//
// These resolve API-schema applications + relationship targets on
// arbitrary Prims, so a downstream physics importer (e.g. lightgeom,
// MuJoCo, Genesis) can do strict-conformant lookups without
// hand-walking generic Property maps.
// ---------------------------------------------------------------
class Prim;

///
/// @brief Populate a PhysicsFilteredPairsAPI struct from a Prim.
///
/// Returns true iff the prim lists PhysicsFilteredPairsAPI in its
/// apiSchemas metadata AND the `physics:filteredPairs` relationship
/// is present (even if empty / blocked). The struct's
/// `filteredPairs` member is filled in either case so the caller can
/// distinguish "no rel authored" (return value false) from "rel
/// authored but resolved to no targets".
///
bool GetPhysicsFilteredPairsAPI(const Prim &prim,
                                PhysicsFilteredPairsAPI *out);

///
/// @brief Populate a Physics*API struct from a Prim's authored properties.
///
/// UsdPhysics applied/multi-apply API schemas (RigidBody/Collision/Material/
/// Mass/MeshCollision) attach `physics:*` properties to an arbitrary host prim
/// (Mesh, Xform, ...). LightUSD keeps those in the host prim's generic property
/// map; these helpers extract them into the typed struct on demand. Each returns
/// true iff the prim lists the corresponding schema in its apiSchemas metadata
/// (authored properties then fill the matching struct members; absent ones keep
/// their struct defaults).
///
bool GetPhysicsRigidBodyAPI(const Prim &prim, PhysicsRigidBodyAPI *out);
bool GetPhysicsCollisionAPI(const Prim &prim, PhysicsCollisionAPI *out);
bool GetPhysicsMaterialAPI(const Prim &prim, PhysicsMaterialAPI *out);
bool GetPhysicsMassAPI(const Prim &prim, PhysicsMassAPI *out);
bool GetPhysicsMeshCollisionAPI(const Prim &prim, PhysicsMeshCollisionAPI *out);

///
/// @brief Resolve membership of the auto-applied CollectionAPI:colliders
///        on a PhysicsCollisionGroup prim.
///
/// Returns the include / exclude target lists from
/// `collection:colliders:includes` and `collection:colliders:excludes`.
/// Either out-pointer may be null. Returns true iff the prim is a
/// PhysicsCollisionGroup and at least one of the relationships is
/// authored.
///
bool GetPhysicsCollidersCollection(const Prim &prim,
                                   std::vector<Path> *includes,
                                   std::vector<Path> *excludes);

namespace value {

// Register UsdPhysics Prim types.
#include "define-type-trait.inc"
DEFINE_TYPE_TRAIT(PhysicsJoint, kPhysicsJoint, TYPE_ID_PHYSICS_JOINT, 1);
DEFINE_TYPE_TRAIT(PhysicsScene, kPhysicsScene, TYPE_ID_PHYSICS_SCENE, 1);
DEFINE_TYPE_TRAIT(PhysicsRevoluteJoint, kPhysicsRevoluteJoint, TYPE_ID_PHYSICS_REVOLUTE_JOINT, 1);
DEFINE_TYPE_TRAIT(PhysicsPrismaticJoint, kPhysicsPrismaticJoint, TYPE_ID_PHYSICS_PRISMATIC_JOINT, 1);
DEFINE_TYPE_TRAIT(PhysicsSphericalJoint, kPhysicsSphericalJoint, TYPE_ID_PHYSICS_SPHERICAL_JOINT, 1);
DEFINE_TYPE_TRAIT(PhysicsFixedJoint, kPhysicsFixedJoint, TYPE_ID_PHYSICS_FIXED_JOINT, 1);
DEFINE_TYPE_TRAIT(PhysicsDistanceJoint, kPhysicsDistanceJoint, TYPE_ID_PHYSICS_DISTANCE_JOINT, 1);
DEFINE_TYPE_TRAIT(PhysicsCollisionGroup, kPhysicsCollisionGroup, TYPE_ID_PHYSICS_COLLISION_GROUP, 1);

#undef DEFINE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
