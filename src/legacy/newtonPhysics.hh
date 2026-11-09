// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present, Light Transport Entertainment Inc.

///
/// @file newtonPhysics.hh
/// @brief Newton physics annotation schema definitions.
///
/// Implements the codeless Newton USD physics schemas used by
/// newton-usd-schemas. The applied APIs extend standard UsdPhysics prims,
/// while NewtonActuator is a concrete prim for joint actuation.
///
#pragma once

#include <limits>

#include "value-types.hh"
#include "nonstd/optional.hpp"
#include "core/prim-enums.hh"
#include "core/path.hh"
#include "core/composition-types.hh"
#include "core/prim-metas.hh"
#include "core/typed-attribute.hh"
#include "core/relationship.hh"
#include "core/property.hh"
#include "core/variant-types.hh"

namespace lightusd {

constexpr auto kNewtonActuator = "NewtonActuator";

struct NewtonSceneAPI {
  TypedAttributeWithFallback<int> maxSolverIterations{-1};
  TypedAttributeWithFallback<int> timeStepsPerSecond{1000};
  TypedAttributeWithFallback<bool> gravityEnabled{true};
};

struct NewtonXpbdSceneAPI {
  TypedAttributeWithFallback<float> softBodyRelaxation{0.9f};
  TypedAttributeWithFallback<float> softContactRelaxation{0.9f};
  TypedAttributeWithFallback<float> jointLinearRelaxation{0.7f};
  TypedAttributeWithFallback<float> jointAngularRelaxation{0.4f};
  TypedAttributeWithFallback<float> jointLinearCompliance{0.0f};
  TypedAttributeWithFallback<float> jointAngularCompliance{0.0f};
  TypedAttributeWithFallback<float> rigidContactRelaxation{0.8f};
  TypedAttributeWithFallback<bool> rigidContactConWeighting{true};
  TypedAttributeWithFallback<float> angularDamping{0.0f};
  TypedAttributeWithFallback<bool> restitutionEnabled{false};
};

struct NewtonKaminoSceneAPI {
  TypedAttributeWithFallback<float> padmmPrimalTolerance{1e-6f};
  TypedAttributeWithFallback<float> padmmDualTolerance{1e-6f};
  TypedAttributeWithFallback<float> padmmComplementarityTolerance{1e-6f};
  TypedAttributeWithFallback<value::token> padmmWarmstarting{value::token("containers")};
  TypedAttributeWithFallback<bool> padmmUseAcceleration{true};
  TypedAttributeWithFallback<bool> constraintsUsePreconditioning{true};
  TypedAttributeWithFallback<float> constraintsAlpha{0.01f};
  TypedAttributeWithFallback<float> constraintsBeta{0.01f};
  TypedAttributeWithFallback<float> constraintsGamma{0.01f};
  TypedAttributeWithFallback<value::token> jointCorrection{value::token("twopi")};
};

struct NewtonArticulationRootAPI {
  TypedAttributeWithFallback<bool> selfCollisionEnabled{true};
};

struct NewtonCollisionAPI {
  TypedAttributeWithFallback<float> contactMargin{0.0f};
  TypedAttributeWithFallback<float> contactGap{-std::numeric_limits<float>::infinity()};
};

struct NewtonMeshCollisionAPI : NewtonCollisionAPI {
  TypedAttributeWithFallback<int> maxHullVertices{-1};
};

struct NewtonMaterialAPI {
  TypedAttributeWithFallback<float> torsionalFriction{0.005f};
  TypedAttributeWithFallback<float> rollingFriction{0.0001f};
};

struct NewtonMimicAPI {
  TypedAttributeWithFallback<bool> mimicEnabled{true};
  RelationshipProperty mimicJoint;
  TypedAttributeWithFallback<float> mimicCoef0{0.0f};
  TypedAttributeWithFallback<float> mimicCoef1{1.0f};
};

struct NewtonActuatorDelayAPI {
  TypedAttributeWithFallback<int> delaySteps{1};
};

struct NewtonActuatorControlBaseAPI {};

struct NewtonPDControlAPI {
  TypedAttributeWithFallback<float> constEffort{0.0f};
  TypedAttributeWithFallback<float> kp{0.0f};
  TypedAttributeWithFallback<float> kd{0.0f};
};

struct NewtonPIDControlAPI : NewtonPDControlAPI {
  TypedAttributeWithFallback<float> ki{0.0f};
  TypedAttributeWithFallback<float> integralMax{std::numeric_limits<float>::infinity()};
};

struct NewtonNeuralControlAPI {
  TypedAttribute<value::AssetPath> modelPath;
};

struct NewtonActuatorClampingBaseAPI {};

struct NewtonMaxEffortClampingAPI {
  TypedAttributeWithFallback<float> maxEffort{std::numeric_limits<float>::infinity()};
};

struct NewtonDCMotorClampingAPI {
  TypedAttributeWithFallback<float> maxMotorEffort{std::numeric_limits<float>::infinity()};
  TypedAttributeWithFallback<float> saturationEffort{std::numeric_limits<float>::infinity()};
  TypedAttributeWithFallback<float> velocityLimit{std::numeric_limits<float>::infinity()};
};

struct NewtonPositionBasedClampingAPI {
  TypedAttribute<std::vector<float>> lookupPositions;
  TypedAttribute<std::vector<float>> lookupEfforts;
};

struct NewtonActuator {
  std::string name;
  Specifier spec{Specifier::Def};
  int64_t parent_id{-1};

  void set_name(const std::string &name_) { name = name_; }
  const std::string &get_name() const { return name; }
  Specifier &specifier() { return spec; }
  const Specifier &specifier() const { return spec; }

  RelationshipProperty targets;

  // Actuator API attributes. They are stored directly on the concrete prim
  // because USD authors them on NewtonActuator with applied control/clamp APIs.
  TypedAttributeWithFallback<int> delaySteps{1};
  TypedAttributeWithFallback<float> constEffort{0.0f};
  TypedAttributeWithFallback<float> kp{0.0f};
  TypedAttributeWithFallback<float> kd{0.0f};
  TypedAttributeWithFallback<float> ki{0.0f};
  TypedAttributeWithFallback<float> integralMax{std::numeric_limits<float>::infinity()};
  TypedAttribute<value::AssetPath> modelPath;
  TypedAttributeWithFallback<float> maxEffort{std::numeric_limits<float>::infinity()};
  TypedAttributeWithFallback<float> maxMotorEffort{std::numeric_limits<float>::infinity()};
  TypedAttributeWithFallback<float> saturationEffort{std::numeric_limits<float>::infinity()};
  TypedAttributeWithFallback<float> velocityLimit{std::numeric_limits<float>::infinity()};
  TypedAttribute<std::vector<float>> lookupPositions;
  TypedAttribute<std::vector<float>> lookupEfforts;

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

#include "define-type-trait.inc"
DEFINE_TYPE_TRAIT(NewtonActuator, kNewtonActuator, TYPE_ID_NEWTON_ACTUATOR, 1);

}  // namespace value

}  // namespace lightusd
