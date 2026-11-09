// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file mjcPhysics.hh
/// @brief MuJoCo physics annotation schema definitions (mjcPhysics)
///
/// Implements MuJoCo's custom USD physics schema that extends standard
/// UsdPhysics with MuJoCo-specific simulation parameters.
///
/// API Schemas (applied to existing prims):
/// - MjcSceneAPI: Global simulation options (timestep, solver, flags, compiler)
/// - MjcJointAPI: Joint physics (stiffness, damping, armature, solver params)
/// - MjcCollisionAPI: Collision parameters (condim, solref, solimp, margin)
/// - MjcMeshCollisionAPI: Mesh collision mode (inertia, maxhullvert)
/// - MjcMaterialAPI: Physical material (torsional/rolling friction)
/// - MjcSiteAPI: MuJoCo site marker
/// - MjcImageableAPI: Visual-only entity
/// - MjcEqualityAPI: Equality constraint base
/// - MjcEqualityConnectAPI, MjcEqualityWeldAPI, MjcEqualityJointAPI
///
/// Concrete Prims:
/// - MjcActuator: Force transmission (actuators)
/// - MjcTendon: Fixed and spatial tendons
/// - MjcKeyframe: Simulation state snapshots
/// - MjcSensor: Sensor declarations
///
#pragma once

#include "value-types.hh"
#include "nonstd/optional.hpp"
#include "nonstd/expected.hpp"
#include "core/prim-enums.hh"
#include "core/path.hh"
#include "core/composition-types.hh"
#include "core/prim-metas.hh"
#include "core/animatable.hh"
#include "core/typed-attribute.hh"
#include "core/relationship.hh"
#include "core/attribute.hh"
#include "core/property.hh"
#include "core/variant-types.hh"

namespace lightusd {

// Prim type name constants
constexpr auto kMjcActuator = "MjcActuator";
constexpr auto kMjcTendon = "MjcTendon";
constexpr auto kMjcKeyframe = "MjcKeyframe";
constexpr auto kMjcSensor = "MjcSensor";

//
// ============================================================
// API Schema structs (applied to host prims, not standalone)
// ============================================================
//

// MjcSceneAPI — Global simulation options
// Applied to PhysicsScene prims. Maps to MJCF <option>, <option/flag>, <compiler>.
struct MjcSceneAPI {
  // mjc:option:* — Simulation options
  TypedAttributeWithFallback<double> timestep{0.002};           // mjc:option:timestep
  TypedAttributeWithFallback<double> impratio{1.0};             // mjc:option:impratio
  TypedAttributeWithFallback<value::double3> wind{{0.0, 0.0, 0.0}};     // mjc:option:wind
  TypedAttributeWithFallback<value::double3> magnetic{{0.0, -0.5, 0.0}}; // mjc:option:magnetic
  TypedAttributeWithFallback<double> density{0.0};              // mjc:option:density
  TypedAttributeWithFallback<double> viscosity{0.0};            // mjc:option:viscosity
  TypedAttributeWithFallback<double> o_margin{0.0};             // mjc:option:o_margin
  TypedAttribute<std::vector<double>> o_solref;                 // mjc:option:o_solref  [0.02, 1.0]
  TypedAttribute<std::vector<double>> o_solimp;                 // mjc:option:o_solimp  [0.9, 0.95, 0.001, 0.5, 2.0]
  TypedAttribute<std::vector<double>> o_friction;               // mjc:option:o_friction [1,1,0.005,0.0001,0.0001]

  // Token enums for solver configuration
  TypedAttributeWithFallback<value::token> integrator{value::token("euler")};   // mjc:option:integrator
  TypedAttributeWithFallback<value::token> cone{value::token("pyramidal")};     // mjc:option:cone
  TypedAttributeWithFallback<value::token> jacobian{value::token("auto")};      // mjc:option:jacobian
  TypedAttributeWithFallback<value::token> solver{value::token("newton")};      // mjc:option:solver

  TypedAttributeWithFallback<int> iterations{100};              // mjc:option:iterations
  TypedAttributeWithFallback<double> tolerance{1e-8};           // mjc:option:tolerance
  TypedAttributeWithFallback<int> ls_iterations{50};            // mjc:option:ls_iterations
  TypedAttributeWithFallback<double> ls_tolerance{0.01};        // mjc:option:ls_tolerance
  TypedAttributeWithFallback<int> noslip_iterations{0};         // mjc:option:noslip_iterations
  TypedAttributeWithFallback<double> noslip_tolerance{1e-6};    // mjc:option:noslip_tolerance
  TypedAttributeWithFallback<int> ccd_iterations{35};           // mjc:option:ccd_iterations
  TypedAttributeWithFallback<double> ccd_tolerance{1e-6};       // mjc:option:ccd_tolerance
  TypedAttributeWithFallback<int> sdf_iterations{10};           // mjc:option:sdf_iterations
  TypedAttributeWithFallback<int> sdf_initpoints{40};           // mjc:option:sdf_initpoints
  TypedAttribute<std::vector<int>> actuatorgroupdisable;        // mjc:option:actuatorgroupdisable

  // mjc:flag:* — Simulation flags (booleans)
  TypedAttributeWithFallback<bool> flag_constraint{true};       // mjc:flag:constraint
  TypedAttributeWithFallback<bool> flag_equality{true};         // mjc:flag:equality
  TypedAttributeWithFallback<bool> flag_frictionloss{true};     // mjc:flag:frictionloss
  TypedAttributeWithFallback<bool> flag_limit{true};            // mjc:flag:limit
  TypedAttributeWithFallback<bool> flag_contact{true};          // mjc:flag:contact
  TypedAttributeWithFallback<bool> flag_spring{true};           // mjc:flag:spring
  TypedAttributeWithFallback<bool> flag_damper{true};           // mjc:flag:damper
  TypedAttributeWithFallback<bool> flag_gravity{true};          // mjc:flag:gravity
  TypedAttributeWithFallback<bool> flag_clampctrl{true};        // mjc:flag:clampctrl
  TypedAttributeWithFallback<bool> flag_warmstart{true};        // mjc:flag:warmstart
  TypedAttributeWithFallback<bool> flag_filterparent{true};     // mjc:flag:filterparent
  TypedAttributeWithFallback<bool> flag_actuation{true};        // mjc:flag:actuation
  TypedAttributeWithFallback<bool> flag_refsafe{true};          // mjc:flag:refsafe
  TypedAttributeWithFallback<bool> flag_sensor{true};           // mjc:flag:sensor
  TypedAttributeWithFallback<bool> flag_midphase{true};         // mjc:flag:midphase
  TypedAttributeWithFallback<bool> flag_nativeccd{true};        // mjc:flag:nativeccd
  TypedAttributeWithFallback<bool> flag_eulerdamp{true};        // mjc:flag:eulerdamp
  TypedAttributeWithFallback<bool> flag_autoreset{true};        // mjc:flag:autoreset
  TypedAttributeWithFallback<bool> flag_island{true};           // mjc:flag:island
  TypedAttributeWithFallback<bool> flag_override{false};        // mjc:flag:override
  TypedAttributeWithFallback<bool> flag_energy{false};          // mjc:flag:energy
  TypedAttributeWithFallback<bool> flag_fwdinv{false};          // mjc:flag:fwdinv
  TypedAttributeWithFallback<bool> flag_invdiscrete{false};     // mjc:flag:invdiscrete
  TypedAttributeWithFallback<bool> flag_multiccd{false};        // mjc:flag:multiccd

  // mjc:compiler:* — Compiler settings
  TypedAttributeWithFallback<bool> compiler_autoLimits{true};        // mjc:compiler:autoLimits
  TypedAttributeWithFallback<double> compiler_boundMass{0.0};        // mjc:compiler:boundMass
  TypedAttributeWithFallback<double> compiler_boundInertia{0.0};     // mjc:compiler:boundInertia
  TypedAttributeWithFallback<double> compiler_setTotalMass{-1.0};    // mjc:compiler:setTotalMass
  TypedAttributeWithFallback<bool> compiler_useThread{true};         // mjc:compiler:useThread
  TypedAttributeWithFallback<bool> compiler_balanceInertia{false};   // mjc:compiler:balanceInertia
  TypedAttributeWithFallback<value::token> compiler_angle{value::token("degree")}; // mjc:compiler:angle
  TypedAttributeWithFallback<bool> compiler_fitAABB{false};          // mjc:compiler:fitAABB
  TypedAttributeWithFallback<bool> compiler_fuseStatic{false};       // mjc:compiler:fuseStatic
  TypedAttributeWithFallback<value::token> compiler_inertiaFromGeom{value::token("auto")}; // mjc:compiler:inertiaFromGeom
  TypedAttributeWithFallback<bool> compiler_alignFree{false};        // mjc:compiler:alignFree
  TypedAttributeWithFallback<int> compiler_inertiaGroupRangeMin{0};  // mjc:compiler:inertiaGroupRange:min
  TypedAttributeWithFallback<int> compiler_inertiaGroupRangeMax{5};  // mjc:compiler:inertiaGroupRange:max
  TypedAttributeWithFallback<bool> compiler_saveInertial{false};     // mjc:compiler:saveInertial
};

// MjcJointAPI — Joint physics properties
// Applied to PhysicsJoint prims alongside standard joint APIs.
struct MjcJointAPI {
  TypedAttributeWithFallback<int> group{0};                // mjc:group
  TypedAttributeWithFallback<double> stiffness{0.0};       // mjc:stiffness
  TypedAttributeWithFallback<double> damping{0.0};         // mjc:damping
  TypedAttributeWithFallback<double> armature{0.0};        // mjc:armature
  TypedAttributeWithFallback<double> frictionloss{0.0};    // mjc:frictionloss
  TypedAttribute<std::vector<double>> springdamper;        // mjc:springdamper [0, 0]
  TypedAttributeWithFallback<double> springref{0.0};       // mjc:springref
  TypedAttributeWithFallback<double> ref{0.0};             // mjc:ref
  TypedAttributeWithFallback<double> margin{0.0};          // mjc:margin
  TypedAttribute<std::vector<double>> solreflimit;         // mjc:solreflimit [0.02, 1.0]
  TypedAttribute<std::vector<double>> solimplimit;         // mjc:solimplimit [0.9, 0.95, 0.001, 0.5, 2.0]
  TypedAttribute<std::vector<double>> solreffriction;      // mjc:solreffriction [0.02, 1.0]
  TypedAttribute<std::vector<double>> solimpfriction;      // mjc:solimpfriction [0.9, 0.95, 0.001, 0.5, 2.0]
  TypedAttributeWithFallback<double> actuatorfrcrange_min{0.0}; // mjc:actuatorfrcrange:min
  TypedAttributeWithFallback<double> actuatorfrcrange_max{0.0}; // mjc:actuatorfrcrange:max
  TypedAttributeWithFallback<value::token> actuatorfrclimited{value::token("auto")}; // mjc:actuatorfrclimited
  TypedAttributeWithFallback<bool> actuatorgravcomp{false}; // mjc:actuatorgravcomp
};

// MjcCollisionAPI — Collision geometry properties
// Applied alongside UsdPhysicsCollisionAPI.
struct MjcCollisionAPI {
  TypedAttributeWithFallback<int> group{0};                // mjc:group
  TypedAttributeWithFallback<int> priority{0};             // mjc:priority
  TypedAttributeWithFallback<int> condim{3};               // mjc:condim
  TypedAttributeWithFallback<double> solmix{1.0};          // mjc:solmix
  TypedAttribute<std::vector<double>> solref;              // mjc:solref [0.02, 1.0]
  TypedAttribute<std::vector<double>> solimp;              // mjc:solimp [0.9, 0.95, 0.001, 0.5, 2.0]
  TypedAttributeWithFallback<double> margin{0.0};          // mjc:margin
  TypedAttributeWithFallback<double> gap{0.0};             // mjc:gap
  TypedAttributeWithFallback<bool> shellinertia{false};    // mjc:shellinertia
};

// MjcMeshCollisionAPI — Mesh collision properties
// Applied alongside UsdPhysicsMeshCollisionAPI.
struct MjcMeshCollisionAPI {
  TypedAttributeWithFallback<value::token> inertia{value::token("legacy")}; // mjc:inertia
  TypedAttributeWithFallback<int> maxhullvert{-1};         // mjc:maxhullvert
};

// MjcMaterialAPI — Physical material properties
// Applied alongside UsdPhysicsMaterialAPI.
struct MjcMaterialAPI {
  TypedAttributeWithFallback<double> torsionalfriction{0.005};  // mjc:torsionalfriction
  TypedAttributeWithFallback<double> rollingfriction{0.0001};   // mjc:rollingfriction
};

// MjcSiteAPI — MuJoCo site marker
// Applied to UsdGeomSphere, UsdGeomCapsule, UsdGeomCylinder, or UsdGeomCube.
struct MjcSiteAPI {
  TypedAttributeWithFallback<int> group{0};  // mjc:group
};

// MjcImageableAPI — Visual-only entity (contype=0, conaffinity=0)
struct MjcImageableAPI {
  TypedAttributeWithFallback<int> group{0};  // mjc:group
};

// MjcEqualityAPI — Base equality constraint
struct MjcEqualityAPI {
  RelationshipProperty target;                    // mjc:target
  TypedAttribute<std::vector<double>> solref;     // mjc:solref [0.02, 1.0]
  TypedAttribute<std::vector<double>> solimp;     // mjc:solimp [0.9, 0.95, 0.001, 0.5, 2.0]
};

// MjcEqualityConnectAPI — Connect constraint (inherits MjcEqualityAPI)
struct MjcEqualityConnectAPI : MjcEqualityAPI {
  // No additional attributes
};

// MjcEqualityWeldAPI — Weld constraint (inherits MjcEqualityAPI)
struct MjcEqualityWeldAPI : MjcEqualityAPI {
  TypedAttributeWithFallback<float> torqueScale{1.0f};  // mjc:torqueScale
};

// MjcEqualityJointAPI — Joint equality constraint (inherits MjcEqualityAPI)
struct MjcEqualityJointAPI : MjcEqualityAPI {
  TypedAttributeWithFallback<double> coef0{0.0};  // mjc:coef0
  TypedAttributeWithFallback<double> coef1{1.0};  // mjc:coef1
  TypedAttributeWithFallback<double> coef2{0.0};  // mjc:coef2
  TypedAttributeWithFallback<double> coef3{0.0};  // mjc:coef3
  TypedAttributeWithFallback<double> coef4{0.0};  // mjc:coef4
};

//
// ============================================================
// Concrete Prim types
// ============================================================
//

// MjcActuator — Force transmission
struct MjcActuator {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const { return name; }
  Specifier &specifier() { return spec; }
  const Specifier &specifier() const { return spec; }

  // Core attributes
  TypedAttributeWithFallback<int> group{0};                    // mjc:group
  RelationshipProperty target;                                  // mjc:target

  // Control/Force/Activation limits
  TypedAttributeWithFallback<value::token> ctrlLimited{value::token("auto")};   // mjc:ctrlLimited
  TypedAttributeWithFallback<value::token> forceLimited{value::token("auto")};  // mjc:forceLimited
  TypedAttributeWithFallback<value::token> actLimited{value::token("auto")};    // mjc:actLimited
  TypedAttributeWithFallback<double> ctrlRange_min{0.0};       // mjc:ctrlRange:min
  TypedAttributeWithFallback<double> ctrlRange_max{0.0};       // mjc:ctrlRange:max
  TypedAttributeWithFallback<double> forceRange_min{0.0};      // mjc:forceRange:min
  TypedAttributeWithFallback<double> forceRange_max{0.0};      // mjc:forceRange:max
  TypedAttributeWithFallback<double> actRange_min{0.0};        // mjc:actRange:min
  TypedAttributeWithFallback<double> actRange_max{0.0};        // mjc:actRange:max
  TypedAttributeWithFallback<double> lengthRange_min{0.0};     // mjc:lengthRange:min
  TypedAttributeWithFallback<double> lengthRange_max{0.0};     // mjc:lengthRange:max

  // Transmission properties
  TypedAttribute<std::vector<double>> gear;                     // mjc:gear [1,0,0,0,0,0]
  TypedAttributeWithFallback<double> crankLength{0.0};         // mjc:crankLength
  TypedAttributeWithFallback<bool> jointInParent{false};       // mjc:jointInParent
  RelationshipProperty refSite;                                 // mjc:refSite
  RelationshipProperty sliderSite;                              // mjc:sliderSite

  // Activation dynamics and force generation
  TypedAttributeWithFallback<int> actDim{-1};                  // mjc:actDim
  TypedAttributeWithFallback<value::token> dynType{value::token("none")};   // mjc:dynType
  TypedAttributeWithFallback<value::token> gainType{value::token("fixed")};  // mjc:gainType
  TypedAttributeWithFallback<value::token> biasType{value::token("none")};   // mjc:biasType
  TypedAttribute<std::vector<double>> dynPrm;                   // mjc:dynPrm [1,0,...] (10 elements)
  TypedAttribute<std::vector<double>> gainPrm;                  // mjc:gainPrm [1,0,...] (10 elements)
  TypedAttribute<std::vector<double>> biasPrm;                  // mjc:biasPrm [0,0,...] (10 elements)
  TypedAttributeWithFallback<bool> actEarly{false};            // mjc:actEarly
  TypedAttributeWithFallback<double> inheritRange{0.0};        // mjc:inheritRange

  // Engine-plugin transmission (<actuator><plugin .../>): the plugin id and the
  // referenced <extension><plugin><instance> name. The instance's <config>
  // key/value pairs are preserved on the /World/MjcPlugins scope.
  TypedAttributeWithFallback<value::token> plugin{value::token("")};    // mjc:plugin
  TypedAttributeWithFallback<value::token> instance{value::token("")};  // mjc:instance

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() { return meta; }
  const PrimMeta &metas() const { return meta; }

  const std::vector<value::token> &primChildrenNames() const { return _primChildren; }
  const std::vector<value::token> &propertyNames() const { return _properties; }
  std::vector<value::token> &primChildrenNames() { return _primChildren; }
  std::vector<value::token> &propertyNames() { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

// MjcTendon — Fixed and spatial tendons
struct MjcTendon {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const { return name; }
  Specifier &specifier() { return spec; }
  const Specifier &specifier() const { return spec; }

  // Type and path
  TypedAttributeWithFallback<value::token> type{value::token("spatial")};  // mjc:type
  RelationshipProperty path;                     // mjc:path
  RelationshipProperty sideSites;                // mjc:sideSites
  TypedAttribute<std::vector<int>> path_indices;        // mjc:path:indices
  TypedAttribute<std::vector<int>> sideSites_indices;   // mjc:sideSites:indices
  TypedAttribute<std::vector<int>> path_segments;       // mjc:path:segments
  TypedAttribute<std::vector<double>> path_divisors;    // mjc:path:divisors
  TypedAttribute<std::vector<double>> path_coef;        // mjc:path:coef

  // Physics properties
  TypedAttributeWithFallback<int> group{0};                    // mjc:group
  TypedAttributeWithFallback<value::token> limited{value::token("auto")};      // mjc:limited
  TypedAttributeWithFallback<value::token> actuatorfrclimited{value::token("auto")}; // mjc:actuatorfrclimited
  TypedAttributeWithFallback<double> range_min{0.0};           // mjc:range:min
  TypedAttributeWithFallback<double> range_max{0.0};           // mjc:range:max
  TypedAttributeWithFallback<double> actuatorfrcrange_min{0.0}; // mjc:actuatorfrcrange:min
  TypedAttributeWithFallback<double> actuatorfrcrange_max{0.0}; // mjc:actuatorfrcrange:max

  // Solver parameters
  TypedAttribute<std::vector<double>> solreflimit;      // mjc:solreflimit [0.02, 1.0]
  TypedAttribute<std::vector<double>> solimplimit;      // mjc:solimplimit [0.9, 0.95, 0.001, 0.5, 2.0]
  TypedAttribute<std::vector<double>> solreffriction;   // mjc:solreffriction [0.02, 1.0]
  TypedAttribute<std::vector<double>> solimpfriction;   // mjc:solimpfriction [0.9, 0.95, 0.001, 0.5, 2.0]

  // Spring/damper properties
  TypedAttributeWithFallback<double> margin{0.0};              // mjc:margin
  TypedAttributeWithFallback<double> frictionloss{0.0};        // mjc:frictionloss
  TypedAttribute<std::vector<double>> springlength;            // mjc:springlength [-1, -1]
  TypedAttributeWithFallback<double> stiffness{0.0};           // mjc:stiffness
  TypedAttributeWithFallback<double> damping{0.0};             // mjc:damping
  TypedAttributeWithFallback<double> armature{0.0};            // mjc:armature

  // Visual properties
  TypedAttributeWithFallback<double> width{0.003};             // mjc:width
  TypedAttributeWithFallback<value::color4f> rgba{{0.5f, 0.5f, 0.5f, 1.0f}}; // mjc:rgba

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() { return meta; }
  const PrimMeta &metas() const { return meta; }

  const std::vector<value::token> &primChildrenNames() const { return _primChildren; }
  const std::vector<value::token> &propertyNames() const { return _properties; }
  std::vector<value::token> &primChildrenNames() { return _primChildren; }
  std::vector<value::token> &propertyNames() { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

// MjcKeyframe — Simulation state snapshots
struct MjcKeyframe {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const { return name; }
  Specifier &specifier() { return spec; }
  const Specifier &specifier() const { return spec; }

  TypedAttribute<std::vector<double>> qpos;    // mjc:qpos
  TypedAttribute<std::vector<double>> qvel;    // mjc:qvel
  TypedAttribute<std::vector<double>> act;     // mjc:act
  TypedAttribute<std::vector<double>> ctrl;    // mjc:ctrl
  TypedAttribute<std::vector<double>> mpos;    // mjc:mpos
  TypedAttribute<std::vector<double>> mquat;   // mjc:mquat

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() { return meta; }
  const PrimMeta &metas() const { return meta; }

  const std::vector<value::token> &primChildrenNames() const { return _primChildren; }
  const std::vector<value::token> &propertyNames() const { return _properties; }
  std::vector<value::token> &primChildrenNames() { return _primChildren; }
  std::vector<value::token> &propertyNames() { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

// MjcSensor — MuJoCo sensor. A single concrete prim covers all sensor types
// (gyro/accelerometer/framepos/jointpos/...); the specific type is the
// `mjc:type` token (the MJCF element name). The measured object is captured by
// `mjc:objtype`/`mjc:objname` (+ `mjc:reftype`/`mjc:refname` for frame sensors).
struct MjcSensor {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const { return name; }
  Specifier &specifier() { return spec; }
  const Specifier &specifier() const { return spec; }

  TypedAttributeWithFallback<value::token> type{value::token("")};      // mjc:type (sensor kind)
  TypedAttributeWithFallback<value::token> objType{value::token("")};   // mjc:objtype
  TypedAttributeWithFallback<value::token> objName{value::token("")};   // mjc:objname
  TypedAttributeWithFallback<value::token> refType{value::token("")};   // mjc:reftype
  TypedAttributeWithFallback<value::token> refName{value::token("")};   // mjc:refname
  TypedAttributeWithFallback<int> group{0};                             // mjc:group
  TypedAttributeWithFallback<double> cutoff{0.0};                       // mjc:cutoff
  TypedAttributeWithFallback<double> noise{0.0};                        // mjc:noise
  TypedAttribute<std::vector<double>> user;                             // mjc:user

  std::pair<ListEditQual, std::vector<Reference>> references;
  std::pair<ListEditQual, std::vector<Payload>> payload;
  std::map<std::string, VariantSet> variantSet;
  std::map<std::string, Property> props;

  PrimMeta meta;
  PrimMeta &metas() { return meta; }
  const PrimMeta &metas() const { return meta; }

  const std::vector<value::token> &primChildrenNames() const { return _primChildren; }
  const std::vector<value::token> &propertyNames() const { return _properties; }
  std::vector<value::token> &primChildrenNames() { return _primChildren; }
  std::vector<value::token> &propertyNames() { return _properties; }

 private:
  std::vector<value::token> _primChildren;
  std::vector<value::token> _properties;
};

namespace value {

// Register mjcPhysics Prim types.
#include "define-type-trait.inc"
DEFINE_TYPE_TRAIT(MjcActuator, kMjcActuator, TYPE_ID_MJC_ACTUATOR, 1);
DEFINE_TYPE_TRAIT(MjcTendon, kMjcTendon, TYPE_ID_MJC_TENDON, 1);
DEFINE_TYPE_TRAIT(MjcKeyframe, kMjcKeyframe, TYPE_ID_MJC_KEYFRAME, 1);
DEFINE_TYPE_TRAIT(MjcSensor, kMjcSensor, TYPE_ID_MJC_SENSOR, 1);

// NOTE: Do not #undef DEFINE_TYPE_TRAIT here — usdPhysics.hh includes this
// file and needs the macro for its own type registrations.

}  // namespace value

}  // namespace lightusd
