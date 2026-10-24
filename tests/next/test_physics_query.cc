// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "c-api/lightusd-physics-c.h"
#include "next/c/lightusd-core-c.h"
#include "next/schema/physics-query.hh"
#include "next/layer/layer.hh"

namespace n = lightusd::next;
void Check(bool ok, const char* message) {
  if (!ok) { std::cerr << message << ": " << lightusd_last_error() << '\n'; std::exit(1); }
}
bool Same(lightusd_sv text, const char* expected) {
  return text.len == std::strlen(expected) && std::memcmp(text.data, expected, text.len) == 0;
}
void CQuery() {
  size_t descriptor = 0;
  const char* schema_name = "PhysicsDriveAPI";
  lu_view schema_key{schema_name, std::strlen(schema_name)};
  Check(lu_validation_schema_find(schema_key, &descriptor) == LU_OK,
        "generated schemas link through the C11 core");
  lu_validation_schema_view definition{};
  Check(lu_validation_schema_at(descriptor, &definition) == LU_OK &&
      definition.property_namespace.len == 5 &&
      std::memcmp(definition.property_namespace.data, "drive", 5) == 0,
      "multiple-apply namespace comes from native descriptors");
  const char* text = R"(#usda 1.0
def Xform "Body" (prepend apiSchemas = ["PhysicsMassAPI", "PhysicsCollisionAPI"]) {
  float physics:mass = 7
  float physics:mass.timeSamples = {0: 2, 2: 4}
  uniform double[] mjc:qpos = [1, 2, 3]
  rel physics:filteredPairs = [</Other>]
}
def Xform "Other" {}
def PhysicsCollisionGroup "Filter" {}
def PhysicsRevoluteJoint "Joint" {
  float drive:angular:physics:stiffness = 3
}
)";
  lightusd_stage* stage = nullptr;
  Check(lightusd_stage_load_from_memory(reinterpret_cast<const uint8_t*>(text), std::strlen(text), nullptr, &stage) == LIGHTUSD_OK, "load generated stage");
  lightusd_physics_query_options options;
  lightusd_physics_query_options_init(&options);
  options.time_code = 1;
  lightusd_physics_snapshot* snapshot = nullptr;
  Check(lightusd_stage_query_physics(stage, &options, &snapshot) == LIGHTUSD_OK, "query physics");
  Check(lightusd_physics_snapshot_size(snapshot) == 3, "only physics records retained");
  lightusd_stage_destroy(stage);
  lightusd_physics_record_info info{};
  Check(lightusd_physics_snapshot_record(snapshot, 0, &info) == LIGHTUSD_OK && Same(info.prim_path, "/Body"), "snapshot survives stage destruction");
  Check((info.groups & (uint64_t(1) << LIGHTUSD_PHYSICS_RIGID_BODIES)) &&
        (info.groups & (uint64_t(1) << LIGHTUSD_PHYSICS_COLLIDERS)), "group overlap");
  lightusd_sv schema{};
  Check(lightusd_physics_snapshot_schema(snapshot, 0, 0, &schema) == LIGHTUSD_OK && Same(schema, "PhysicsMassAPI"), "schema names");
  lightusd_value* array = nullptr;
  bool mass = false;
  for (size_t i = 0; i < info.property_count; ++i) {
    lightusd_physics_property_info property{};
    lightusd_value* value = nullptr;
    Check(lightusd_physics_snapshot_property(snapshot, 0, i, &property, &value) == LIGHTUSD_OK, "owned property");
    lightusd_value_view view{};
    Check(lightusd_value_get_view(value, &view) == LIGHTUSD_OK, "typed value view");
    if (Same(property.name, "physics:mass")) {
      Check(view.type == LIGHTUSD_TYPE_FLOAT && *static_cast<const float*>(view.data) == 3, "coherent numeric time");
      mass = true;
    }
    if (Same(property.name, "mjc:qpos")) {
      Check(Same(property.type_name, "double[]") && property.uniform, "authored type and variability");
      array = value;
    } else lightusd_value_destroy(value);
  }
  Check(mass && array, "all authored fields retained");
  lightusd_sv name{};
  lightusd_strlist* targets = nullptr;
  Check(lightusd_physics_snapshot_relationship(snapshot, 0, 0, &name, &targets) == LIGHTUSD_OK, "relationship query");
  Check(Same(name, "physics:filteredPairs") && Same(lightusd_strlist_get(targets, 0), "/Other"), "relationship targets");
  lightusd_strlist_destroy(targets);
  Check(lightusd_physics_snapshot_record(snapshot, 100, &info) == LIGHTUSD_ERR_NOT_FOUND && info.prim_path.len == 0, "invalid record clears outputs");
  lightusd_value* invalid = reinterpret_cast<lightusd_value*>(uintptr_t(1));
  lightusd_physics_property_info property{};
  Check(lightusd_physics_snapshot_property(snapshot, 0, 100, &property, &invalid) == LIGHTUSD_ERR_NOT_FOUND && !invalid, "invalid property clears owned output");
  lightusd_physics_snapshot_destroy(snapshot);
  lightusd_value_view retained{};
  Check(lightusd_value_get_view(array, &retained) == LIGHTUSD_OK && retained.count == 3 &&
      static_cast<const double*>(retained.data)[2] == 3, "owned array survives snapshot destruction");
  lightusd_value_destroy(array);
  Check(lightusd_stage_query_physics(nullptr, nullptr, &snapshot) == LIGHTUSD_ERR_INVALID_ARG && !snapshot, "null stage rejected");
  Check(lightusd_physics_snapshot_size(nullptr) == 0, "null snapshot size");
  for (uint32_t i = 0; i < LIGHTUSD_PHYSICS_GROUP_COUNT; ++i)
    Check(*lightusd_physics_group_name(i) != '\0', "all group names available");
}
void NativeQuery() {
  n::PhysicsSnapshot retained;
  {
    n::Layer layer;
    auto id = layer.define_prim_at_path("/State", "MjcKeyframe");
    auto* prim = layer.prim_mutable(id);
    prim->upsert_property("mjc:qpos", n::Value::MakeDoubleArray(std::vector<double>(200000, 42)));
    const auto* original = prim->property_value("mjc:qpos")->as_double_array().data();
    n::Stage stage;
    stage.SetRootLayer(std::move(layer));
    n::PhysicsQueryOptions options;
    options.max_memory = 1024 * 1024;
    Check(!n::QueryPhysics(stage, options), "native snapshot budget rejects oversized arrays");
    options.max_memory = 4 * 1024 * 1024;
    auto query = n::QueryPhysics(stage, options);
    Check(bool(query), "native owned snapshot");
    const auto& immutable = query->records[0].properties[0].value;
    Check(immutable.as_double_array().data() == original,
          "owned numeric arrays share immutable native storage");
    retained = std::move(*query);
  }
  const auto& immutable = retained.records[0].properties[0].value;
  auto values = immutable.as_double_array();
  Check(values.size() == 200000 && values[199999] == 42, "native array ownership");
}
void CConvertUnits() {
  const char* text = R"(#usda 1.0
(
  upAxis = "Z"
  metersPerUnit = 1
)
def PhysicsScene "Scene" { vector3f physics:gravityDirection = (0, 0, -1) }
def Xform "Robot" {
  matrix4d xformOp:transform = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (1, 2, 3, 1))
  uniform token[] xformOpOrder = ["xformOp:transform"]
  def Sphere "Ball" (prepend apiSchemas = ["PhysicsMassAPI"]) {
    double radius.timeSamples = {0: 1, 10: 2}
    float physics:mass = 2
  }
  def PhysicsPrismaticJoint "Slide" { float physics:upperLimit = 0.5  float drive:linear:physics:stiffness = 4 }
  def PhysicsJoint "Generic" { float newton:armature = 1 }
}
)";
  lightusd_stage* stage = nullptr;
  Check(lightusd_stage_load_from_memory(reinterpret_cast<const uint8_t*>(text), std::strlen(text), nullptr, &stage) == LIGHTUSD_OK, "load unit stage");
  lightusd_unit_conversion report{};
  lightusd_strlist* skipped = nullptr;
  Check(lightusd_stage_convert_units(stage, 0.01, 0.001, "Y", &report, &skipped) == LIGHTUSD_OK, "convert units");
  Check(report.length_scale == 100 && report.mass_scale == 1000 && report.rotated_roots == 2, "conversion report");
  Check(lightusd_strlist_size(skipped) == 1 && Same(lightusd_strlist_get(skipped, 0), "/Robot/Generic.newton:armature"),
        "unknown DOF kind is reported");
  lightusd_strlist_destroy(skipped);
  auto number = [&](const char* path, const char* name, size_t i) {
    lightusd_value_view view{};
    Check(lightusd_attr_get(lightusd_stage_prim_at_path(stage, path), name, &view) == LIGHTUSD_OK, name);
    return view.storage == LIGHTUSD_COMP_FLOAT32 ? double(static_cast<const float*>(view.data)[i])
                                                 : static_cast<const double*>(view.data)[i];
  };
  Check(number("/Robot", "xformOp:transform", 12) == 100 && number("/Robot", "xformOp:transform", 14) == 300 &&
        number("/Robot", "xformOp:transform", 0) == 1, "matrix translation scales, rotation does not");
  Check(number("/Robot/Ball", "physics:mass", 0) == 2000, "mass scales with M");
  Check(number("/Robot/Slide", "physics:upperLimit", 0) == 50 &&
        number("/Robot/Slide", "drive:linear:physics:stiffness", 0) == 4000, "linear DOF dimensions");
  Check(number("/Scene", "physics:gravityDirection", 1) == -1 && number("/Scene", "physics:gravityDirection", 2) == 0,
        "gravity follows the new up axis");
  const lightusd_prim ball = lightusd_stage_prim_at_path(stage, "/Robot/Ball");
  lightusd_value_view view{};
  double time = 0;
  Check(lightusd_attr_get(ball, "radius", &view) == LIGHTUSD_ERR_NOT_FOUND, "no default authored for sampled radius");
  Check(lightusd_attr_timesample_at(ball, "radius", 1, &time, &view) == LIGHTUSD_OK && time == 10 &&
        static_cast<const double*>(view.data)[0] == 200, "time samples scale once");
  Check(lightusd_stage_convert_units(stage, -1, 0, nullptr, nullptr, nullptr) == LIGHTUSD_ERR_INVALID_ARG &&
        lightusd_stage_convert_units(stage, 0, 0, "X", nullptr, nullptr) == LIGHTUSD_ERR_INVALID_ARG,
        "invalid targets rejected");
  // Back to Z-up meters: the up-axis rotation op is removed again.
  Check(lightusd_stage_convert_units(stage, 1, 1, "Z", &report, nullptr) == LIGHTUSD_OK, "convert back");
  Check(!lightusd_prim_has_property(lightusd_stage_prim_at_path(stage, "/Robot"), "xformOp:rotateX:upAxis") &&
        number("/Robot/Ball", "physics:mass", 0) == 2, "round trip restores the stage");
  lightusd_stage_destroy(stage);
}
int main() {
  CQuery();
  NativeQuery();
  CConvertUnits();
  std::cout << "Typed physics snapshots, arrays, groups, time, limits and unit conversion passed\n";
}
