// SPDX-License-Identifier: Apache-2.0
#include "lightusd-physics-c.h"
#include "c-internal.hh"
#include "next/c/lightusd-core-c.h"
#include "next/schema/physics-query.hh"
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>
namespace n = lightusd::next;
using lightusd_internal::Fail;
struct lightusd_physics_snapshot { n::PhysicsSnapshot data; };
static_assert(static_cast<uint8_t>(n::PhysicsGroup::Count) == LIGHTUSD_PHYSICS_GROUP_COUNT,
              "C/C++ physics group inventories must agree");
namespace {
lightusd_sv View(const std::string& text) { return {text.data(), text.size()}; }
const n::PhysicsRecord* Record(const lightusd_physics_snapshot* snapshot, size_t i) {
  return snapshot && i < snapshot->data.records.size() ? &snapshot->data.records[i] : nullptr;
}
lightusd_status Missing() { return Fail(LIGHTUSD_ERR_NOT_FOUND, "Physics snapshot index out of range"); }
}
extern "C" {
void lightusd_physics_query_options_init(lightusd_physics_query_options* options) {
  if (!options) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->max_memory_bytes = 512ull * 1024 * 1024;
  options->max_records = 1000000;
}
lightusd_status lightusd_stage_query_physics(const lightusd_stage* stage,
    const lightusd_physics_query_options* options, lightusd_physics_snapshot** out) {
  if (out) *out = nullptr;
  if (!stage || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Physics stage/output is null");
  n::PhysicsQueryOptions query;
  if (options) {
    if (options->struct_size < sizeof(*options) || (options->flags & ~LIGHTUSD_PHYSICS_DEFAULT_TIME) ||
        options->max_memory_bytes > SIZE_MAX || options->max_records > SIZE_MAX ||
        !options->max_memory_bytes || !options->max_records ||
        (!(options->flags & LIGHTUSD_PHYSICS_DEFAULT_TIME) && !std::isfinite(options->time_code)))
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "Invalid physics query options");
    query.max_memory = static_cast<size_t>(options->max_memory_bytes);
    query.max_records = static_cast<size_t>(options->max_records);
    query.time = options->flags & LIGHTUSD_PHYSICS_DEFAULT_TIME ? n::TimeQuery::Default() : n::TimeQuery::Numeric(options->time_code);
  }
  auto result = n::QueryPhysics(stage->ReadStage(), query);
  if (!result) return Fail(result.error().find("budget") != std::string::npos ||
      result.error().find("limit") != std::string::npos ? LIGHTUSD_ERR_RESOURCE_LIMIT : LIGHTUSD_ERR_PARSE, result.error());
  auto* snapshot = new (std::nothrow) lightusd_physics_snapshot;
  if (!snapshot) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "Cannot allocate physics snapshot");
  snapshot->data = std::move(*result);
  *out = snapshot;
  return LIGHTUSD_OK;
}
void lightusd_physics_snapshot_destroy(lightusd_physics_snapshot* snapshot) { delete snapshot; }
size_t lightusd_physics_snapshot_size(const lightusd_physics_snapshot* snapshot) { return snapshot ? snapshot->data.records.size() : 0; }
const char* lightusd_physics_group_name(uint32_t group) {
  return group < LIGHTUSD_PHYSICS_GROUP_COUNT ? n::PhysicsGroupName(static_cast<n::PhysicsGroup>(group)) : "";
}
lightusd_status lightusd_physics_snapshot_record(const lightusd_physics_snapshot* snapshot,
    size_t i, lightusd_physics_record_info* out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Physics record output is null");
  *out = {};
  const auto* record = Record(snapshot, i);
  if (!record) return Missing();
  *out = {View(record->prim_path), View(record->type_name), record->groups,
          record->api_schemas.size(), record->properties.size(), record->relationships.size()};
  return LIGHTUSD_OK;
}
lightusd_status lightusd_physics_snapshot_schema(const lightusd_physics_snapshot* snapshot,
    size_t i, size_t schema, lightusd_sv* out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Physics schema output is null");
  *out = {};
  const auto* record = Record(snapshot, i);
  if (!record || schema >= record->api_schemas.size()) return Missing();
  *out = View(record->api_schemas[schema]);
  return LIGHTUSD_OK;
}
lightusd_status lightusd_physics_snapshot_property(const lightusd_physics_snapshot* snapshot,
    size_t i, size_t property, lightusd_physics_property_info* info, lightusd_value** out) {
  if (info) *info = {};
  if (out) *out = nullptr;
  if (!info || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Physics property output is null");
  const auto* record = Record(snapshot, i);
  if (!record || property >= record->properties.size()) return Missing();
  auto* value = new (std::nothrow) lightusd_value;
  if (!value) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "Cannot allocate physics value");
  const auto& item = record->properties[property];
  value->v = item.value;
  *info = {View(item.name), View(item.type_name), uint8_t(item.uniform), uint8_t(item.blocked)};
  *out = value;
  return LIGHTUSD_OK;
}
lightusd_status lightusd_physics_snapshot_relationship(const lightusd_physics_snapshot* snapshot,
    size_t i, size_t relationship, lightusd_sv* name, lightusd_strlist** out) {
  if (name) *name = {};
  if (out) *out = nullptr;
  if (!name || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Physics relationship output is null");
  const auto* record = Record(snapshot, i);
  if (!record || relationship >= record->relationships.size()) return Missing();
  auto* targets = new (std::nothrow) lightusd_strlist;
  if (!targets) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "Cannot allocate physics targets");
  const auto& item = record->relationships[relationship];
  targets->items = item.targets;
  *name = View(item.name);
  *out = targets;
  return LIGHTUSD_OK;
}
}  // extern C

namespace {
// Unit conversion (mirrors python/lightusd/units.py).
struct Dims { int length, mass; };
bool StartsWith(const std::string& s, const char* prefix) { return s.compare(0, std::strlen(prefix), prefix) == 0; }
bool FixedDims(const std::string& name, Dims* out) {
  static const struct { const char* name; Dims dims; } kFixed[] = {
      {"points", {1, 0}}, {"extent", {1, 0}}, {"widths", {1, 0}},
      {"radius", {1, 0}}, {"height", {1, 0}}, {"size", {1, 0}}, {"width", {1, 0}}, {"length", {1, 0}},
      {"physics:localPos0", {1, 0}}, {"physics:localPos1", {1, 0}},
      {"physics:centerOfMass", {1, 0}}, {"physics:velocity", {1, 0}},
      {"physics:gravityMagnitude", {1, 0}},
      {"physics:mass", {0, 1}}, {"physics:density", {-3, 1}},
      {"physics:diagonalInertia", {2, 1}}, {"newton:inertia", {2, 1}},
      {"newton:contactMargin", {1, 0}}, {"newton:contactGap", {1, 0}},
      {"newton:sdfNarrowBandInner", {1, 0}}, {"newton:sdfNarrowBandOuter", {1, 0}},
      {"physics:breakForce", {1, 1}}, {"physics:breakTorque", {2, 1}},
      {"physics:minDistance", {1, 0}}, {"physics:maxDistance", {1, 0}},
      {"physics:masses", {0, 1}}, {"physics:thicknesses", {1, 0}}};
  for (const auto& entry : kFixed)
    if (name == entry.name) { *out = entry.dims; return true; }
  return false;
}
bool GprimSize(const std::string& name) {
  return name == "radius" || name == "height" || name == "size" || name == "width" || name == "length";
}
// Per degree of freedom: linear dims, angular dims.
bool NewtonJointDims(const std::string& name, Dims* linear, Dims* angular) {
  if (name == "newton:armature" || name == "newton:damping") { *linear = {0, 1}; *angular = {2, 1}; return true; }
  if (name == "newton:friction") { *linear = {1, 1}; *angular = {2, 1}; return true; }
  if (name == "newton:velocityLimit") { *linear = {1, 0}; *angular = {0, 0}; return true; }
  return false;
}
bool DofFieldDims(const std::string& field, Dims* linear, Dims* angular) {
  if (field == "targetPosition" || field == "targetVelocity" || field == "position" ||
      field == "velocity" || field == "low" || field == "high") { *linear = {1, 0}; *angular = {0, 0}; return true; }
  if (field == "stiffness" || field == "damping") { *linear = {0, 1}; *angular = {2, 1}; return true; }
  if (field == "maxForce") { *linear = {1, 1}; *angular = {2, 1}; return true; }
  return false;
}
// 1 linear, 0 angular, -1 unknown.
int DofKind(const std::string& type, const std::string& name) {
  for (const char* key : {":linear:", ":transX:", ":transY:", ":transZ:", ":distance:"})
    if (name.find(key) != std::string::npos) return 1;
  for (const char* key : {":angular:", ":rotX:", ":rotY:", ":rotZ:"})
    if (name.find(key) != std::string::npos) return 0;
  if (type == "PhysicsPrismaticJoint") return 1;
  if (type == "PhysicsRevoluteJoint" || type == "PhysicsSphericalJoint") return 0;
  return -1;
}
bool UnitDims(const std::string& type, const std::string& name, Dims* out) {
  if (name == "xformOp:transform" || StartsWith(name, "xformOp:translate")) { *out = {1, 0}; return true; }
  if (FixedDims(name, out))
    return !(GprimSize(name) && (type.empty() || type == "Xform" || type == "Scope" ||
                                 type == "Material" || type == "Shader"));
  Dims linear{}, angular{};
  if (NewtonJointDims(name, &linear, &angular)) {
    const int kind = DofKind(type, name);
    if (kind < 0) return false;
    *out = kind ? linear : angular;
    return true;
  }
  if ((name == "physics:lowerLimit" || name == "physics:upperLimit") && type == "PhysicsPrismaticJoint") {
    *out = {1, 0};
    return true;
  }
  if ((StartsWith(name, "drive:") || StartsWith(name, "limit:") || StartsWith(name, "state:")) &&
      name.find(":physics:") != std::string::npos &&
      DofFieldDims(name.substr(name.rfind(':') + 1), &linear, &angular)) {
    const int kind = DofKind(type, name);
    if (kind < 0) return false;
    *out = kind ? linear : angular;
    return true;
  }
  return false;
}
bool UnknownDof(const std::string& type, const std::string& name) {
  Dims a{}, b{};
  return (StartsWith(name, "drive:") || StartsWith(name, "limit:") || StartsWith(name, "state:") ||
          NewtonJointDims(name, &a, &b)) && DofKind(type, name) < 0;
}
double ViewComponent(const lightusd_value_view& view, size_t i) {
  const char* data = static_cast<const char*>(view.data);
  switch (view.storage) {
    case LIGHTUSD_COMP_FLOAT16: { uint16_t h; std::memcpy(&h, data + 2 * i, 2); return lu_half_to_float(h); }
    case LIGHTUSD_COMP_FLOAT32: { float f; std::memcpy(&f, data + 4 * i, 4); return f; }
    default: { double d; std::memcpy(&d, data + 8 * i, 8); return d; }
  }
}
struct ScaleContext { const lightusd_value_view* view; double factor; bool matrix_translation; };
int ScaledComponent(void* context, size_t i, lu_number* out) {
  const auto* c = static_cast<const ScaleContext*>(context);
  double v = ViewComponent(*c->view, i);
  // A row-major (row-vector) 4x4 matrix carries its translation in 12..14.
  if (!c->matrix_translation || (i % 16 >= 12 && i % 16 < 15)) v *= c->factor;
  out->kind = LU_NUMBER_FLOAT;
  out->value.float_value = v;
  return 1;
}
bool IsFloatView(const lightusd_value_view& view) {
  return view.data && !view.is_block && (view.storage == LIGHTUSD_COMP_FLOAT16 ||
      view.storage == LIGHTUSD_COMP_FLOAT32 || view.storage == LIGHTUSD_COMP_FLOAT64);
}
// Owned scaled copy, or null when the view is not floating point.
lightusd_value* Scaled(const lightusd_value_view& view, double factor, bool matrix) {
  if (!IsFloatView(view)) return nullptr;
  if (matrix && view.components != 16) return nullptr;
  ScaleContext context{&view, factor, matrix};
  lightusd_value* out = nullptr;
  lightusd_value_from_numbers(view.type, view.is_array, view.count, ScaledComponent, &context, &out);
  return out;
}
std::string PrimPath(lightusd_prim prim) {
  const lightusd_sv sv = lightusd_prim_path(prim);
  return std::string(sv.data ? sv.data : "", sv.len);
}
std::string SvString(lightusd_sv sv) { return std::string(sv.data ? sv.data : "", sv.len); }
bool TokenList(lightusd_prim prim, const char* name, std::vector<std::string>* out) {
  out->clear();
  lightusd_strlist* list = nullptr;
  if (lightusd_attr_get_token_array(prim, name, &list) != LIGHTUSD_OK) return false;
  for (size_t i = 0; i < lightusd_strlist_size(list); ++i) out->push_back(SvString(lightusd_strlist_get(list, i)));
  lightusd_strlist_destroy(list);
  return true;
}
lightusd_status SetTokenList(lightusd_stage* stage, const std::string& path, const char* name,
                             const std::vector<std::string>& items) {
  std::vector<const char*> raw;
  for (const auto& item : items) raw.push_back(item.c_str());
  return lightusd_attr_set_token_array(stage, path.c_str(), name, LIGHTUSD_TYPE_TOKEN, raw.data(),
                                       raw.size(), LIGHTUSD_PROP_UNIFORM);
}
// OpenVDB voxels and their index->grid map stay in the file's units: append an
// innermost uniform scale to the field prim (UsdVol places a field by the
// field prim's transform times the grid's), accumulating over conversions.
lightusd_status ScaleVolumeField(lightusd_stage* stage, const std::string& path, double factor) {
  static const char* kOp = "xformOp:scale:units";
  const lightusd_prim prim = lightusd_stage_prim_at_path(stage, path.c_str());
  std::vector<std::string> order;
  TokenList(prim, "xformOpOrder", &order);
  bool present = false;
  for (const auto& item : order) present = present || item == kOp;
  double s = factor;
  lightusd_value_view view{};
  if (present && lightusd_attr_get(prim, kOp, &view) == LIGHTUSD_OK && IsFloatView(view) &&
      !view.is_array && view.components == 3)
    s *= ViewComponent(view, 0);
  if (present && std::fabs(s - 1.0) < 1e-12) {
    std::vector<std::string> kept;
    for (const auto& item : order) if (item != kOp) kept.push_back(item);
    lightusd_status status = lightusd_attr_remove(stage, path.c_str(), kOp);
    return status == LIGHTUSD_OK ? SetTokenList(stage, path, "xformOpOrder", kept) : status;
  }
  const double scale[3] = {s, s, s};
  lightusd_status status = lightusd_attr_set(stage, path.c_str(), kOp, LIGHTUSD_TYPE_DOUBLE3, 0, scale, 1, 0);
  if (status != LIGHTUSD_OK || present) return status;
  order.push_back(kOp);
  return SetTokenList(stage, path, "xformOpOrder", order);
}
}  // namespace

extern "C" lightusd_status lightusd_stage_convert_units(lightusd_stage* stage, double meters_per_unit,
    double kilograms_per_unit, const char* up_axis, lightusd_unit_conversion* report,
    lightusd_strlist** skipped) {
  if (report) *report = {};
  if (skipped) *skipped = nullptr;
  if (!stage) return Fail(LIGHTUSD_ERR_INVALID_ARG, "stage is null");
  if (lightusd_stage_is_read_only(stage)) return Fail(LIGHTUSD_ERR_INVALID_ARG, "stage is read-only");
  if (!(meters_per_unit >= 0) || !(kilograms_per_unit >= 0) || !std::isfinite(meters_per_unit) ||
      !std::isfinite(kilograms_per_unit))
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "units must be positive");
  const n::StageMeta& meta = stage->ReadStage().GetMeta();
  const double old_mpu = meta.metersPerUnit > 0 ? meta.metersPerUnit : 0.01;
  const double old_kpu = meta.kilogramsPerUnit > 0 ? meta.kilogramsPerUnit : 1.0;
  const double new_mpu = meters_per_unit > 0 ? meters_per_unit : old_mpu;
  const double new_kpu = kilograms_per_unit > 0 ? kilograms_per_unit : old_kpu;
  const std::string old_up = meta.upAxis.empty() ? "Y" : meta.upAxis;
  const std::string new_up = up_axis && *up_axis ? up_axis : old_up;
  if (new_up != "Y" && new_up != "Z") return Fail(LIGHTUSD_ERR_INVALID_ARG, "up_axis must be Y or Z");
  if (new_up != old_up && old_up != "Y" && old_up != "Z")
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "stage upAxis must be Y or Z to rotate");
  auto* skip_list = new (std::nothrow) lightusd_strlist;
  if (!skip_list) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "Cannot allocate skipped list");
  lightusd_unit_conversion result{old_mpu / new_mpu, old_kpu / new_kpu, 0, 0};
  const double L = result.length_scale, M = result.mass_scale;

  // Preorder prim paths, collected before authoring.
  std::vector<std::string> paths;
  std::vector<lightusd_prim> stack;
  for (size_t i = lightusd_stage_root_prim_count(stage); i > 0; --i) stack.push_back(lightusd_stage_root_prim(stage, i - 1));
  while (!stack.empty()) {
    const lightusd_prim prim = stack.back();
    stack.pop_back();
    if (!lightusd_prim_is_valid(prim)) continue;
    paths.push_back(PrimPath(prim));
    for (size_t i = lightusd_prim_child_count(prim); i > 0; --i) stack.push_back(lightusd_prim_child(prim, i - 1));
  }

  lightusd_status status = LIGHTUSD_OK;
  std::vector<std::string> scenes;
  for (const auto& path : paths) {
    lightusd_prim prim = lightusd_stage_prim_at_path(stage, path.c_str());
    if (!lightusd_prim_is_valid(prim)) continue;
    const std::string type = SvString(lightusd_prim_type_name(prim));
    if (type == "PhysicsScene") scenes.push_back(path);
    struct Prop { std::string name; uint16_t flags; };
    std::vector<Prop> props;
    for (size_t i = 0; i < lightusd_prim_property_count(prim); ++i) {
      const uint16_t flags = lightusd_prim_property_flags_at(prim, i);
      if (!(flags & (LIGHTUSD_PROP_RELATIONSHIP | LIGHTUSD_PROP_CONNECTION)))
        props.push_back({SvString(lightusd_prim_property_name(prim, i)), flags});
    }
    for (const auto& prop : props) {
      const char* name = prop.name.c_str();
      Dims dims{};
      if (!UnitDims(type, prop.name, &dims)) {
        if (UnknownDof(type, prop.name)) skip_list->items.push_back(path + "." + prop.name);
        continue;
      }
      const bool matrix = prop.name == "xformOp:transform";
      const double factor = matrix ? L : std::pow(L, dims.length) * std::pow(M, dims.mass);
      if (factor == 1.0) continue;
      prim = lightusd_stage_prim_at_path(stage, path.c_str());
      // Scale every sample before authoring any (views borrow the stage).
      std::vector<std::pair<double, lightusd_value*>> samples;
      const size_t sample_count = lightusd_attr_timesample_count(prim, name);
      for (size_t i = 0; i < sample_count; ++i) {
        double time = 0;
        lightusd_value_view view{};
        if (lightusd_attr_timesample_at(prim, name, i, &time, &view) != LIGHTUSD_OK) continue;
        if (lightusd_value* scaled = Scaled(view, factor, matrix)) samples.emplace_back(time, scaled);
      }
      lightusd_value* scaled_default = nullptr;
      lightusd_value_view view{};
      if (lightusd_attr_get(prim, name, &view) == LIGHTUSD_OK) scaled_default = Scaled(view, factor, matrix);
      for (auto& sample : samples) {
        if (status == LIGHTUSD_OK)
          status = lightusd_attr_set_timesample_value(stage, path.c_str(), name, sample.first, sample.second);
        lightusd_value_destroy(sample.second);
      }
      if (scaled_default) {
        if (status == LIGHTUSD_OK)
          status = lightusd_attr_set_value(stage, path.c_str(), name, scaled_default,
                                           prop.flags & (LIGHTUSD_PROP_CUSTOM | LIGHTUSD_PROP_UNIFORM));
        lightusd_value_destroy(scaled_default);
      }
      if (status != LIGHTUSD_OK) { delete skip_list; return status; }
      ++result.converted;
    }
    if (type == "OpenVDBAsset" && L != 1.0) {
      status = ScaleVolumeField(stage, path, L);
      if (status != LIGHTUSD_OK) { delete skip_list; return status; }
      ++result.converted;
    }
  }

  if (new_up != old_up) {
    const double angle = new_up == "Z" ? 90.0 : -90.0;
    const char* op = "xformOp:rotateX:upAxis";
    std::vector<std::string> roots;
    for (size_t i = 0; i < lightusd_stage_root_prim_count(stage); ++i)
      roots.push_back(PrimPath(lightusd_stage_root_prim(stage, i)));
    for (const auto& path : roots) {
      const lightusd_prim prim = lightusd_stage_prim_at_path(stage, path.c_str());
      std::vector<std::string> order;
      TokenList(prim, "xformOpOrder", &order);
      bool present = false;
      for (const auto& item : order) present = present || item == op;
      if (present) {
        // A previous conversion's rotation: accumulate, drop at zero.
        lightusd_value_view view{};
        double total = angle;
        if (lightusd_attr_get(prim, op, &view) == LIGHTUSD_OK && IsFloatView(view)) total += ViewComponent(view, 0);
        if (std::fabs(total) < 1e-9) {
          std::vector<std::string> kept;
          for (const auto& item : order) if (item != op) kept.push_back(item);
          status = lightusd_attr_remove(stage, path.c_str(), op);
          if (status == LIGHTUSD_OK) status = SetTokenList(stage, path, "xformOpOrder", kept);
        } else {
          status = lightusd_attr_set(stage, path.c_str(), op, LIGHTUSD_TYPE_DOUBLE, 0, &total, 1, 0);
        }
      } else {
        status = lightusd_attr_set(stage, path.c_str(), op, LIGHTUSD_TYPE_DOUBLE, 0, &angle, 1, 0);
        order.insert(order.begin(), op);
        if (status == LIGHTUSD_OK) status = SetTokenList(stage, path, "xformOpOrder", order);
      }
      if (status != LIGHTUSD_OK) { delete skip_list; return status; }
      ++result.rotated_roots;
    }
    for (const auto& path : scenes) {
      const lightusd_prim scene = lightusd_stage_prim_at_path(stage, path.c_str());
      lightusd_value_view view{};
      if (lightusd_attr_get(scene, "physics:gravityDirection", &view) != LIGHTUSD_OK || !IsFloatView(view) ||
          view.is_array || view.components != 3)
        continue;
      const float x = float(ViewComponent(view, 0)), y = float(ViewComponent(view, 1)), z = float(ViewComponent(view, 2));
      // Rx(+90) (Y-up -> Z-up) maps (x, y, z) -> (x, -z, y).
      const float rotated[3] = {x, new_up == "Z" ? -z : z, new_up == "Z" ? y : -y};
      status = lightusd_attr_set(stage, path.c_str(), "physics:gravityDirection", LIGHTUSD_TYPE_VECTOR3F, 0,
                                 rotated, 1, 0);
      if (status != LIGHTUSD_OK) { delete skip_list; return status; }
    }
    status = lightusd_stage_set_metadata(stage, "upAxis", LIGHTUSD_TYPE_TOKEN, new_up.c_str(), 1);
  }
  if (status == LIGHTUSD_OK)
    status = lightusd_stage_set_metadata(stage, "metersPerUnit", LIGHTUSD_TYPE_DOUBLE, &new_mpu, 1);
  if (status == LIGHTUSD_OK)
    status = lightusd_stage_set_metadata(stage, "kilogramsPerUnit", LIGHTUSD_TYPE_DOUBLE, &new_kpu, 1);
  if (status != LIGHTUSD_OK) { delete skip_list; return status; }
  if (report) *report = result;
  if (skipped) *skipped = skip_list;
  else delete skip_list;
  return LIGHTUSD_OK;
}
