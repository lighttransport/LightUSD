// SPDX-License-Identifier: Apache-2.0
#include "physics-query.hh"
#include "../layer/layer.hh"
#include "../layer/prim-spec.hh"
#include "../types/type-info.hh"
#include <algorithm>
#include <cmath>

namespace lightusd { namespace next {
namespace {
bool Starts(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }
bool PhysicsName(const std::string& name) {
  for (const char* prefix : {"physics:", "mjc:", "newton:", "physx", "state:", "urdf:"})
    if (Starts(name, prefix)) return true;
  if ((Starts(name, "drive:") || Starts(name, "limit:")) && name.find(":physics:") != std::string::npos) return true;
  if (Starts(name, "collection:colliders:")) return true;
  if (Starts(name, "material:binding:") && name.find(":physics") != std::string::npos) return true;
  return false;
}
bool PhysicsSchema(const std::string& name) {
  return Starts(name, "Physics") || Starts(name, "Mjc") || Starts(name, "Newton");
}
void Group(PhysicsRecord& record, PhysicsGroup group) {
  record.groups |= uint64_t(1) << static_cast<uint8_t>(group);
}
void Classify(PhysicsRecord& record) {
  const auto api = [&](const char* name) {
    return std::find(record.api_schemas.begin(), record.api_schemas.end(), name) != record.api_schemas.end();
  };
  const auto& type = record.type_name;
  if (type == "PhysicsScene") Group(record, PhysicsGroup::Scenes);
  if (api("PhysicsRigidBodyAPI") || api("PhysicsMassAPI")) Group(record, PhysicsGroup::RigidBodies);
  if (api("PhysicsCollisionAPI") || api("PhysicsMeshCollisionAPI")) Group(record, PhysicsGroup::Colliders);
  if (Starts(type, "Physics") && type.size() >= 5 && type.compare(type.size() - 5, 5, "Joint") == 0)
    Group(record, PhysicsGroup::Joints);
  if (type == "PhysicsCollisionGroup") Group(record, PhysicsGroup::CollisionGroups);
  if (api("PhysicsMaterialAPI")) Group(record, PhysicsGroup::Materials);
  if (api("PhysicsArticulationRootAPI")) Group(record, PhysicsGroup::ArticulationRoots);
  if (api("PhysicsFilteredPairsAPI")) Group(record, PhysicsGroup::FilteredPairs);
  if (api("MjcSiteAPI")) Group(record, PhysicsGroup::Sites);
  for (const auto& schema : record.api_schemas)
    if (Starts(schema, "MjcEquality")) { Group(record, PhysicsGroup::Equalities); break; }
  const std::pair<const char*, PhysicsGroup> markers[] = {
      {"Actuator", PhysicsGroup::Actuators}, {"Tendon", PhysicsGroup::Tendons},
      {"Sensor", PhysicsGroup::Sensors}, {"ContactPair", PhysicsGroup::Contacts},
      {"Keyframe", PhysicsGroup::Keyframes}};
  for (const auto& marker : markers)
    if (type.find(marker.first) != std::string::npos) Group(record, marker.second);
  if (!record.groups) Group(record, PhysicsGroup::Extensions);
}
}  // namespace
const char* PhysicsGroupName(PhysicsGroup group) {
  static constexpr const char* names[] = {"scenes", "rigid_bodies", "colliders", "joints", "materials",
      "articulation_roots", "filtered_pairs", "actuators", "sites", "tendons", "equalities",
      "sensors", "contacts", "keyframes", "extensions", "collision_groups"};
  static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(PhysicsGroup::Count), "physics group names");
  static_assert(static_cast<size_t>(PhysicsGroup::Count) <= 64, "physics groups fit their public mask");
  const auto i = static_cast<size_t>(group);
  return i < static_cast<size_t>(PhysicsGroup::Count) ? names[i] : "";
}
expected<PhysicsSnapshot, std::string> QueryPhysics(const Stage& stage, const PhysicsQueryOptions& options) {
  if (!options.max_memory || !options.max_records ||
      (options.time.is_numeric() && !std::isfinite(options.time.numeric_time())))
    return make_unexpected(std::string("Invalid physics query options"));
  PhysicsSnapshot snapshot;
  std::string error;
  const auto charge = [&](size_t amount) {
    if (amount > options.max_memory - std::min(snapshot.memory_estimate, options.max_memory)) {
      error = "Physics snapshot memory budget exceeded";
      return false;
    }
    snapshot.memory_estimate += amount;
    return true;
  };
  AttributeEval eval(&stage);
  EvalOptions evaluation;
  evaluation.time = options.time;
  eval.SetOptions(evaluation);
  stage.Traverse([&](const UsdPrim& prim) {
    const auto* spec = prim.GetPrimSpec();
    if (!spec) return true;
    bool relevant = PhysicsSchema(prim.GetTypeName());
    for (const auto& schema : prim.GetMeta().apiSchemas()) relevant |= PhysicsSchema(schema);
    // Discover without retaining unrelated geometry arrays.
    const auto names = prim.GetPropertyNames();
    const auto relationships = prim.GetRelationshipNames();
    for (const auto& name : names) relevant |= PhysicsName(name);
    for (const auto& name : relationships) relevant |= PhysicsName(name);
    if (!relevant) return true;
    if (snapshot.records.size() >= options.max_records) {
      error = "Physics snapshot record limit exceeded";
      return false;
    }
    PhysicsRecord record;
    if (!charge(sizeof(PhysicsRecord) * 2 + prim.GetPath().str().size() + prim.GetTypeName().size())) return false;
    record.prim_path = prim.GetPath().str();
    record.type_name = prim.GetTypeName();
    for (const auto& schema : prim.GetMeta().apiSchemas()) {
      if (!charge(sizeof(std::string) * 2 + schema.size())) return false;
      record.api_schemas.push_back(schema);
    }
    for (const auto& name : names) {
      if (!PhysicsName(name) || !prim.HasAuthoredProperty(name) || spec->relationship(name)) continue;
      const auto* slot = spec->property(name);
      PhysicsProperty property;
      if (!charge(sizeof(PhysicsProperty) * 2 + name.size())) return false;
      property.name = name;
      if (const auto* type = spec->property_type_name(name)) property.type_name = *type;
      if (!charge(property.type_name.size())) return false;
      property.uniform = slot && (slot->flags & PropSlot::kFlagUniform);
      auto value = eval.Eval(prim, name);
      property.blocked = value.blocked;
      if (value.success) {
        size_t estimate = value.value.heap_memory_usage();
        if (value.value.is_array()) {
          const auto* type = GetTypeInfo(value.value.type_id());
          const size_t stride = type ? type->size : 0;
          if (stride && value.value.array_size() > SIZE_MAX / stride) {
            error = "Physics snapshot array size overflow (memory limit)";
            return false;
          }
          estimate = std::max(estimate, value.value.array_size() * stride);
        }
        if (!charge(estimate)) return false;
        property.value = value.value.materialized_copy();
        if (!value.value.is_empty() && property.value.is_empty()) {
          error = "Physics snapshot array materialization failed: " + record.prim_path + "." + name;
          return false;
        }
      } else if (value.blocked) property.value = Value::MakeBlock();
      else if (!value.error.empty()) {
        error = "Physics snapshot evaluation failed: " + record.prim_path + "." + name + ": " + value.error;
        return false;
      }
      record.properties.push_back(std::move(property));
    }
    for (const auto& name : relationships) {
      if (!PhysicsName(name) && name != "material:binding") continue;
      if (!charge(sizeof(PhysicsRelationship) * 2 + name.size())) return false;
      PhysicsRelationship relationship;
      relationship.name = name;
      if (const auto* targets = prim.GetRelationship(name))
        for (const auto& target : *targets) {
          if (!charge(sizeof(std::string) * 2 + target.str().size())) return false;
          relationship.targets.push_back(target.str());
        }
      record.relationships.push_back(std::move(relationship));
    }
    Classify(record);
    snapshot.records.push_back(std::move(record));
    return true;
  });
  if (!error.empty()) return make_unexpected(std::move(error));
  return snapshot;
}
} }
