// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../eval/attribute-eval.hh"
#include "../expected.hh"

namespace lightusd { namespace next {
enum class PhysicsGroup : uint8_t {
  Scenes, RigidBodies, Colliders, Joints, Materials, ArticulationRoots,
  FilteredPairs, Actuators, Sites, Tendons, Equalities, Sensors, Contacts,
  Keyframes, Extensions, CollisionGroups, Count
};
const char* PhysicsGroupName(PhysicsGroup group);
struct PhysicsProperty {
  std::string name, type_name;
  Value value;
  bool uniform = false, blocked = false;
};
struct PhysicsRelationship {
  std::string name;
  std::vector<std::string> targets;
};
struct PhysicsRecord {
  std::string prim_path, type_name;
  uint64_t groups = 0;
  std::vector<std::string> api_schemas;
  std::vector<PhysicsProperty> properties;
  std::vector<PhysicsRelationship> relationships;
};
struct PhysicsQueryOptions {
  TimeQuery time = TimeQuery::Numeric(0.0);
  size_t max_memory = size_t(512) * 1024 * 1024;
  size_t max_records = 1000000;
};
struct PhysicsSnapshot {
  std::vector<PhysicsRecord> records;
  size_t memory_estimate = 0;
};
// Authored physics/engine properties only, evaluated at one coherent time.
// Values own their storage and survive stage edits/destruction. Numeric arrays
// retain immutable C11 storage instead of expanding into per-element objects.
expected<PhysicsSnapshot, std::string> QueryPhysics(
    const Stage& stage, const PhysicsQueryOptions& options = {});
} }
