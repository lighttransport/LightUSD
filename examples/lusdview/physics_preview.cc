// SPDX-License-Identifier: Apache-2.0
#include "physics_preview.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "lightusd-physics-c.h"
#include "lightusd-render-c.h"
#include "public_stage_queries.hh"
#include "tydra/rb-constraints-internal.hh"

namespace lusdview {
namespace {
constexpr float kRadians = 0.017453292519943295f;
bool Fail(std::string* error, const std::string& text) {
  if (error) *error = text;
  return false;
}
struct Pose {
  TydraPhysTransform xform = tp_xform_identity();
  float scale[3]{1, 1, 1};
};
// Reject shear/reflection rather than feed a non-rigid frame to the solver.
bool ReadPose(const lightusd_stage* stage, lightusd_prim prim, double time,
              Pose* pose) {
  double m[16];
  if (lightusd_prim_world_transform(stage, prim, time, m) != LIGHTUSD_OK)
    return false;
  for (double n : m)
    if (!std::isfinite(n)) return false;
  for (int i = 12; i < 15; ++i)
    if (std::fabs(m[i]) > 1e12) return false;
  double r[9];
  for (int c = 0; c < 3; ++c) {
    const double scale =
        std::sqrt(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] +
                  m[c * 4 + 2] * m[c * 4 + 2]);
    if (scale < 1e-8 || scale > 1e12) return false;
    pose->scale[c] = static_cast<float>(scale);
    for (int row = 0; row < 3; ++row) r[c * 3 + row] = m[c * 4 + row] / scale;
  }
  for (int a = 0; a < 3; ++a)
    for (int b = a + 1; b < 3; ++b)
      if (std::fabs(r[a * 3] * r[b * 3] + r[a * 3 + 1] * r[b * 3 + 1] +
                    r[a * 3 + 2] * r[b * 3 + 2]) > 1e-5)
        return false;
  const double det = r[0] * (r[4] * r[8] - r[5] * r[7]) -
                     r[3] * (r[1] * r[8] - r[2] * r[7]) +
                     r[6] * (r[1] * r[5] - r[2] * r[4]);
  if (det < 0) return false;
  // Stable quaternion extraction, column-major matrix.
  double q[4]{};
  const double trace = r[0] + r[4] + r[8];
  if (trace > 0) {
    const double s = std::sqrt(trace + 1) * 2;
    q[3] = s / 4;
    q[0] = (r[5] - r[7]) / s;
    q[1] = (r[6] - r[2]) / s;
    q[2] = (r[1] - r[3]) / s;
  } else {
    int i = 0;
    if (r[4] > r[0]) i = 1;
    if (r[8] > r[i * 3 + i]) i = 2;
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const double s =
        std::sqrt(1 + r[i * 3 + i] - r[j * 3 + j] - r[k * 3 + k]) * 2;
    q[i] = s / 4;
    q[j] = (r[i * 3 + j] + r[j * 3 + i]) / s;
    q[k] = (r[i * 3 + k] + r[k * 3 + i]) / s;
    q[3] = (r[j * 3 + k] - r[k * 3 + j]) / s;
  }
  pose->xform = {
      tp_v3(static_cast<float>(m[12]), static_cast<float>(m[13]),
            static_cast<float>(m[14])),
      tp_q_normalize(tp_q(static_cast<float>(q[0]), static_cast<float>(q[1]),
                          static_cast<float>(q[2]), static_cast<float>(q[3])))};
  return true;
}
float Number(const lightusd_stage* stage, lightusd_prim prim, const char* name,
             double time, float fallback) {
  lightusd::api::Value value;
  lightusd_value_view view{};
  if (lightusd_attr_eval(stage, prim, name, time, value.put()) != LIGHTUSD_OK ||
      lightusd_value_get_view(value.get(), &view) != LIGHTUSD_OK ||
      !view.data || view.is_array || view.components != 1)
    return fallback;
  double n = fallback;
  switch (view.storage) {
    case LIGHTUSD_COMP_FLOAT32:
      n = *static_cast<const float*>(view.data);
      break;
    case LIGHTUSD_COMP_FLOAT64:
      n = *static_cast<const double*>(view.data);
      break;
    case LIGHTUSD_COMP_INT32:
      n = *static_cast<const int32_t*>(view.data);
      break;
    case LIGHTUSD_COMP_UINT8:
      n = *static_cast<const uint8_t*>(view.data);
      break;
    default:
      break;
  }
  return std::isfinite(n) && std::fabs(n) <= std::numeric_limits<float>::max()
             ? static_cast<float>(n)
             : fallback;
}
TydraPhysVec3 Vector(const lightusd_stage* stage, lightusd_prim prim,
                     const char* name, double time, TydraPhysVec3 fallback) {
  lightusd::api::Value value;
  lightusd_value_view v{};
  if (lightusd_attr_eval(stage, prim, name, time, value.put()) != LIGHTUSD_OK ||
      lightusd_value_get_view(value.get(), &v) != LIGHTUSD_OK || !v.data ||
      v.is_array || v.components != 3)
    return fallback;
  float xyz[3];
  for (int i = 0; i < 3; ++i) {
    if (v.storage == LIGHTUSD_COMP_FLOAT32)
      xyz[i] = static_cast<const float*>(v.data)[i];
    else if (v.storage == LIGHTUSD_COMP_FLOAT64)
      xyz[i] = static_cast<float>(static_cast<const double*>(v.data)[i]);
    else
      return fallback;
    if (!std::isfinite(xyz[i])) return fallback;
  }
  return tp_v3(xyz[0], xyz[1], xyz[2]);
}
TydraPhysQuat Quaternion(const lightusd_stage* stage, lightusd_prim prim,
                         const char* name, double time) {
  lightusd::api::Value value;
  lightusd_value_view v{};
  if (lightusd_attr_eval(stage, prim, name, time, value.put()) != LIGHTUSD_OK ||
      lightusd_value_get_view(value.get(), &v) != LIGHTUSD_OK || !v.data ||
      v.is_array || v.components != 4)
    return tp_q_identity();
  float q[4];
  for (int i = 0; i < 4; ++i) {
    if (v.storage == LIGHTUSD_COMP_FLOAT32)
      q[i] = static_cast<const float*>(v.data)[i];
    else if (v.storage == LIGHTUSD_COMP_FLOAT64)
      q[i] = static_cast<float>(static_cast<const double*>(v.data)[i]);
    else
      return tp_q_identity();
    if (!std::isfinite(q[i])) return tp_q_identity();
  }
  return tp_q_normalize(tp_q(q[1], q[2], q[3], q[0]));
}
bool MeshBounds(const lightusd_stage* stage, lightusd_prim prim, double time,
                float minimum[3], float maximum[3]) {
  using Array = lightusd::api::Owner<lightusd_render_float_array,
                                     lightusd_render_float_array_destroy>;
  Array extent;
  if (lightusd_render_float_array_create(stage, prim, "extent", time,
                                         extent.put()) == LIGHTUSD_OK &&
      lightusd_render_float_array_size(extent.get()) == 6) {
    float values[6];
    if (lightusd_render_float_array_copy(extent.get(), 0, 6, values) ==
        LIGHTUSD_OK) {
      bool valid = true;
      for (int i = 0; i < 3; ++i)
        valid = valid && std::isfinite(values[i]) &&
                std::isfinite(values[i + 3]) && values[i] <= values[i + 3];
      if (valid) {
        std::copy_n(values, 3, minimum);
        std::copy_n(values + 3, 3, maximum);
        return true;
      }
    }
  }
  Array points;
  if (lightusd_render_float_array_create(stage, prim, "points", time,
                                         points.put()) != LIGHTUSD_OK)
    return false;
  const size_t count = lightusd_render_float_array_size(points.get());
  // Bounded scan with a fixed scratch buffer; never expand the whole mesh.
  if (!count || count % 3 || count > size_t(16) * 1024 * 1024 * 3) return false;
  std::fill_n(minimum, 3, std::numeric_limits<float>::max());
  std::fill_n(maximum, 3, -std::numeric_limits<float>::max());
  float chunk[3072];
  for (size_t offset = 0; offset < count; offset += 3072) {
    const size_t size = std::min(size_t(3072), count - offset);
    if (lightusd_render_float_array_copy(points.get(), offset, size, chunk) !=
        LIGHTUSD_OK)
      return false;
    for (size_t i = 0; i < size; ++i) {
      if (!std::isfinite(chunk[i])) return false;
      const size_t axis = i % 3;
      minimum[axis] = std::min(minimum[axis], chunk[i]);
      maximum[axis] = std::max(maximum[axis], chunk[i]);
    }
  }
  return true;
}
std::string Token(lightusd_prim p, const char* name, const char* fallback) {
  lightusd_sv value{};
  return lightusd_attr_get_string(p, name, &value) == LIGHTUSD_OK
             ? PublicString(value)
             : fallback;
}
TydraPhysVec3 Axis(lightusd_prim p, const char* fallback) {
  const auto axis = Token(p, "axis", fallback);
  return axis == "X"   ? tp_v3(1, 0, 0)
         : axis == "Y" ? tp_v3(0, 1, 0)
                       : tp_v3(0, 0, 1);
}
std::string Target(lightusd_prim p, const char* name) {
  if (lightusd_rel_target_count(p, name) != 1) return {};
  return PublicString(lightusd_rel_target(p, name, 0));
}
}  // namespace
struct PhysicsPreview::State {
  TydraPhysWorld world{};
  lightusd::api::Stage stage;
  std::vector<std::string> paths;
  std::vector<Pose> poses;
  std::vector<TydraPhysBody> bodies{1024};
  std::vector<TydraPhysCollider> colliders{2048};
  std::vector<TydraPhysJoint> joints{512};
  std::vector<TydraPhysContact> contacts{4096};
  std::vector<TydraPhysAABB> aabbs{2048};
  std::vector<TydraPhysCollisionPair> pairs{8192};
  std::vector<int32_t> sorted = std::vector<int32_t>(2048),
                       unions = std::vector<int32_t>(1024),
                       islandBodies = std::vector<int32_t>(2048);
  std::vector<TydraPhysIsland> islands{256};
  std::vector<ContactConstraint> cache{4096};
  State() {
    tydra_phys_world_init(
        &world, bodies.data(), 1024, colliders.data(), 2048, joints.data(), 512,
        contacts.data(), 4096, aabbs.data(), pairs.data(), 8192, sorted.data(),
        islands.data(), 256, unions.data(), islandBodies.data());
    tydra_phys_world_defaults(&world);
    world.contact_cache = cache.data();
  }
};
PhysicsPreview::PhysicsPreview() = default;
PhysicsPreview::~PhysicsPreview() = default;
void PhysicsPreview::clear() { state_.reset(); }
const TydraPhysWorld& PhysicsPreview::world() const {
  static const TydraPhysWorld empty{};
  return state_ ? state_->world : empty;
}
const lightusd_stage* PhysicsPreview::stage() const {
  return state_ ? state_->stage.get() : nullptr;
}
const std::vector<std::string>& PhysicsPreview::bodyPaths() const {
  static const std::vector<std::string> empty;
  return state_ ? state_->paths : empty;
}

bool PhysicsPreview::initialize(const lightusd_stage* input, double time,
                                std::string* error) {
  if (!input || !std::isfinite(time))
    return Fail(error, "Physics preview requires a stage and finite time");
  auto next = std::make_unique<State>();
  lightusd_physics_query_options options;
  lightusd_physics_query_options_init(&options);
  options.time_code = time;
  options.max_memory_bytes = 64ull * 1024 * 1024;
  options.max_records = 4096;
  lightusd::api::Owner<lightusd_physics_snapshot,
                       lightusd_physics_snapshot_destroy>
      snapshot;
  if (lightusd_stage_query_physics(input, &options, snapshot.put()) !=
      LIGHTUSD_OK)
    return Fail(error, lightusd_last_error());
  std::vector<lightusd_physics_record_info> records;
  for (size_t i = 0; i < lightusd_physics_snapshot_size(snapshot.get()); ++i) {
    lightusd_physics_record_info info{};
    if (lightusd_physics_snapshot_record(snapshot.get(), i, &info) !=
        LIGHTUSD_OK)
      return Fail(error, "Invalid physics record");
    records.push_back(info);
  }
  PublicStageInfo metadata;
  ReadPublicStageInfo(input, &metadata);
  const float gravity = static_cast<float>(
      9.81 / (metadata.metersPerUnit > 0 ? metadata.metersPerUnit : 0.01));
  next->world.gravity = tp_v3_scale(
      metadata.upAxis == "Z" ? tp_v3(0, 0, -1) : tp_v3(0, -1, 0), gravity);
  const auto prim = [&](const lightusd_physics_record_info& record) {
    return lightusd_stage_prim_at_path(input,
                                       PublicString(record.prim_path).c_str());
  };
  std::map<std::string, int32_t> bodyIds;
  for (const auto& record : records) {
    const auto p = prim(record);
    if (record.groups & (uint64_t(1) << LIGHTUSD_PHYSICS_SCENES)) {
      const auto direction = Vector(input, p, "physics:gravityDirection", time,
                                    tp_v3_normalize(next->world.gravity));
      float magnitude =
          Number(input, p, "physics:gravityMagnitude", time, gravity);
      if (magnitude < 0) magnitude = gravity;
      next->world.gravity = tp_v3_scale(tp_v3_normalize(direction), magnitude);
      next->world.timestep =
          std::clamp(Number(input, p, "mjc:option:timestep", time, 1.0f / 60),
                     1e-5f, 0.1f);
      next->world.solver_iterations = static_cast<int>(std::clamp(
          Number(input, p, "mjc:option:iterations", time, 8), 1.0f, 1000.0f));
    }
    if (!(record.groups & (uint64_t(1) << LIGHTUSD_PHYSICS_RIGID_BODIES)))
      continue;
    const std::string path = PublicString(record.prim_path);
    Pose pose;
    if (!ReadPose(input, p, time, &pose))
      return Fail(error, "Unsupported non-rigid body transform: " + path);
    TydraPhysBody body;
    tydra_phys_body_default(&body);
    body.xform = pose.xform;
    if (!Number(input, p, "physics:rigidBodyEnabled", time, 1))
      body.body_type = TYDRA_PHYS_BODY_STATIC;
    else if (Number(input, p, "physics:kinematicEnabled", time, 0))
      body.body_type = TYDRA_PHYS_BODY_KINEMATIC;
    if (Number(input, p, "physics:startsAsleep", time, 0))
      body.flags |= TYDRA_PHYS_BODY_FLAG_SLEEPING;
    const float authoredMass = Number(input, p, "physics:mass", time, 0);
    const float mass = authoredMass > 0 ? authoredMass : 1;
    body.inverse_mass = 1 / mass;
    const auto inertia = Vector(input, p, "physics:diagonalInertia", time,
                                tp_v3(mass, mass, mass));
    body.local_inv_inertia = tp_v3(inertia.x > 0 ? 1 / inertia.x : 1 / mass,
                                   inertia.y > 0 ? 1 / inertia.y : 1 / mass,
                                   inertia.z > 0 ? 1 / inertia.z : 1 / mass);
    body.linear_velocity =
        Vector(input, p, "physics:velocity", time, tp_v3(0, 0, 0));
    body.angular_velocity = tp_v3_scale(
        Vector(input, p, "physics:angularVelocity", time, tp_v3(0, 0, 0)),
        kRadians);
    const int id = tydra_phys_add_body(&next->world, &body);
    if (id < 0) return Fail(error, "Physics preview body capacity exceeded");
    bodyIds[path] = id;
    next->paths.push_back(path);
    next->poses.push_back(pose);
  }
  for (const auto& record : records) {
    if (!(record.groups & (uint64_t(1) << LIGHTUSD_PHYSICS_COLLIDERS)))
      continue;
    const auto p = prim(record);
    if (!Number(input, p, "physics:collisionEnabled", time, 1)) continue;
    const std::string path = PublicString(record.prim_path),
                      type = PublicString(record.type_name);
    Pose pose;
    if (!ReadPose(input, p, time, &pose))
      return Fail(error, "Unsupported collider transform: " + path);
    TydraPhysCollider col{};
    col.body_index = -1;
    col.local_pose = pose.xform;
    col.collision_group = 1;
    col.collision_mask = ~uint32_t(0);
    std::string ancestor = path;
    while (!ancestor.empty()) {
      const auto found = bodyIds.find(ancestor);
      if (found != bodyIds.end()) {
        col.body_index = found->second;
        col.local_pose = tp_xform_mul(
            tp_xform_inverse(
                next->bodies[static_cast<size_t>(col.body_index)].xform),
            pose.xform);
        break;
      }
      const auto slash = ancestor.find_last_of('/');
      if (slash == std::string::npos) break;
      ancestor.resize(slash);
    }
    if (col.body_index < 0) {
      // Contacts require two body records. An authored static collider has no
      // RigidBodyAPI, but still needs an infinite-mass body in this solver.
      TydraPhysBody body;
      tydra_phys_body_default(&body);
      body.body_type = TYDRA_PHYS_BODY_STATIC;
      body.inverse_mass = 0;
      body.local_inv_inertia = tp_v3(0, 0, 0);
      body.flags = 0;
      body.xform = pose.xform;
      col.body_index = tydra_phys_add_body(&next->world, &body);
      if (col.body_index < 0)
        return Fail(error, "Physics preview static-body capacity exceeded");
      col.local_pose = tp_xform_identity();
      next->paths.push_back(path);
      next->poses.push_back(pose);
    }
    col.static_friction =
        Number(input, p, "physics:staticFriction", time, 0.5f);
    col.dynamic_friction =
        Number(input, p, "physics:dynamicFriction", time, 0.5f);
    col.restitution = Number(input, p, "physics:restitution", time, 0.3f);
    const float radius = std::max(1e-6f, Number(input, p, "radius", time, 1));
    if (type == "Sphere") {
      col.shape.type = TYDRA_PHYS_SHAPE_SPHERE;
      col.shape.data.sphere.radius =
          radius * std::max({pose.scale[0], pose.scale[1], pose.scale[2]});
    } else if (type == "Cube") {
      const float half =
          std::max(1e-6f, Number(input, p, "size", time, 2)) * 0.5f;
      col.shape.type = TYDRA_PHYS_SHAPE_BOX;
      col.shape.data.box.half_extents = tp_v3(
          half * pose.scale[0], half * pose.scale[1], half * pose.scale[2]);
    } else if (type == "Mesh") {
      float minimum[3], maximum[3];
      if (!MeshBounds(input, p, time, minimum, maximum))
        return Fail(error,
                    "Mesh collider needs finite bounds within the diagnostic "
                    "point limit: " +
                        path);
      float half[3], center[3];
      for (int i = 0; i < 3; ++i) {
        const double a = minimum[i], b = maximum[i];
        half[i] = static_cast<float>((b - a) * 0.5 * pose.scale[i]);
        center[i] = static_cast<float>((a + b) * 0.5 * pose.scale[i]);
        if (!std::isfinite(half[i]) || !std::isfinite(center[i]))
          return Fail(error, "Mesh collider bounds overflow: " + path);
        half[i] = std::max(half[i], 1e-6f);
      }
      col.shape.type = TYDRA_PHYS_SHAPE_BOX;
      col.shape.data.box.half_extents = tp_v3(half[0], half[1], half[2]);
      col.local_pose.position =
          tp_v3_add(col.local_pose.position,
                    tp_q_rotate(col.local_pose.rotation,
                                tp_v3(center[0], center[1], center[2])));
    } else if (type == "Plane") {
      col.shape.type = TYDRA_PHYS_SHAPE_PLANE;
      col.shape.data.plane.normal = Axis(p, "Z");
      col.shape.data.plane.offset = 0;
    } else if (type == "Capsule" || type == "Cylinder") {
      const auto axis = Axis(p, "Z");
      const int longitudinal = axis.x ? 0 : axis.y ? 1 : 2;
      const float axial = pose.scale[longitudinal],
                  radial = std::max(pose.scale[(longitudinal + 1) % 3],
                                    pose.scale[(longitudinal + 2) % 3]);
      const float height =
          std::max(0.0f, Number(input, p, "height", time, 2)) * 0.5f * axial;
      if (type == "Capsule") {
        col.shape.type = TYDRA_PHYS_SHAPE_CAPSULE;
        col.shape.data.capsule = {radius * radial, height};
      } else {
        col.shape.type = TYDRA_PHYS_SHAPE_CYLINDER;
        col.shape.data.cylinder = {radius * radial, height};
      }
      const auto rotation =
          axis.x   ? tp_q_from_axis_angle(tp_v3(0, 0, 1), -1.57079632679f)
          : axis.z ? tp_q_from_axis_angle(tp_v3(1, 0, 0), 1.57079632679f)
                   : tp_q_identity();
      col.local_pose.rotation = tp_q_mul(col.local_pose.rotation, rotation);
    } else
      return Fail(error, "Unsupported diagnostic collider geometry: " + type +
                             " at " + path);
    if (!std::isfinite(col.static_friction) ||
        !std::isfinite(col.dynamic_friction) || !std::isfinite(col.restitution))
      return Fail(error, "Invalid physics material at " + path);
    const float maxScale =
        std::max({pose.scale[0], pose.scale[1], pose.scale[2]});
    if (!std::isfinite(radius * maxScale) ||
        !std::isfinite(Number(input, p, "height", time, 2) * maxScale) ||
        !std::isfinite(Number(input, p, "size", time, 2) * maxScale))
      return Fail(error, "Collider dimensions overflow at " + path);
    const int id = tydra_phys_add_collider(&next->world, &col);
    if (id < 0)
      return Fail(error, "Physics preview collider capacity exceeded");
    if (col.body_index >= 0) {
      auto& body = next->bodies[static_cast<size_t>(col.body_index)];
      if (!body.collider_count) body.collider_start = id;
      ++body.collider_count;
    }
  }
  // Solver collider ranges are contiguous per body; preserve deterministic
  // body ordering while gathering compound child shapes.
  std::stable_sort(
      next->colliders.begin(),
      next->colliders.begin() + next->world.num_colliders,
      [](const auto& a, const auto& b) { return a.body_index < b.body_index; });
  for (int i = 0; i < next->world.num_bodies; ++i) {
    next->bodies[static_cast<size_t>(i)].collider_count = 0;
    next->bodies[static_cast<size_t>(i)].collider_start = -1;
  }
  for (int i = 0; i < next->world.num_colliders; ++i) {
    const int b = next->colliders[static_cast<size_t>(i)].body_index;
    if (b >= 0) {
      auto& body = next->bodies[static_cast<size_t>(b)];
      if (!body.collider_count) body.collider_start = i;
      ++body.collider_count;
    }
  }
  for (const auto& record : records) {
    if (!(record.groups & (uint64_t(1) << LIGHTUSD_PHYSICS_JOINTS))) continue;
    const auto p = prim(record);
    const auto type = PublicString(record.type_name);
    TydraPhysJoint joint{};
    joint.body_a = -1;
    joint.body_b = -1;
    const std::string targets[] = {Target(p, "physics:body0"),
                                   Target(p, "physics:body1")};
    int* ids[] = {&joint.body_a, &joint.body_b};
    for (int i = 0; i < 2; ++i)
      if (!targets[i].empty()) {
        auto found = bodyIds.find(targets[i]);
        if (found == bodyIds.end())
          return Fail(error,
                      "Joint targets a missing rigid body: " + targets[i]);
        *ids[i] = found->second;
      }
    joint.local_anchor_a = {
        Vector(input, p, "physics:localPos0", time, tp_v3(0, 0, 0)),
        Quaternion(input, p, "physics:localRot0", time)};
    joint.local_anchor_b = {
        Vector(input, p, "physics:localPos1", time, tp_v3(0, 0, 0)),
        Quaternion(input, p, "physics:localRot1", time)};
    joint.axis = Axis(p, "X");
    if (type == "PhysicsFixedJoint")
      joint.type = TYDRA_PHYS_JOINT_FIXED;
    else if (type == "PhysicsRevoluteJoint")
      joint.type = TYDRA_PHYS_JOINT_HINGE;
    else if (type == "PhysicsPrismaticJoint")
      joint.type = TYDRA_PHYS_JOINT_SLIDER;
    else if (type == "PhysicsSphericalJoint" || type == "PhysicsJoint")
      joint.type = TYDRA_PHYS_JOINT_BALL;
    else if (type == "PhysicsDistanceJoint")
      joint.type = TYDRA_PHYS_JOINT_DISTANCE;
    else
      return Fail(error, "Unsupported diagnostic joint: " + type);
    const float factor = joint.type == TYDRA_PHYS_JOINT_HINGE ? kRadians : 1;
    joint.lower_limit =
        Number(input, p, "physics:lowerLimit", time, -TP_UNLIMITED) * factor;
    joint.upper_limit =
        Number(input, p, "physics:upperLimit", time, TP_UNLIMITED) * factor;
    joint.limit_enabled = joint.lower_limit > -TP_UNLIMITED * factor ||
                          joint.upper_limit < TP_UNLIMITED * factor;
    joint.min_distance = Number(input, p, "physics:minDistance", time, 0);
    joint.max_distance =
        Number(input, p, "physics:maxDistance", time, TP_UNLIMITED);
    joint.stiffness = Number(input, p, "mjc:stiffness", time, 0);
    joint.damping = Number(input, p, "mjc:damping", time, 0);
    if (tydra_phys_add_joint(&next->world, &joint) < 0)
      return Fail(error, "Physics preview joint capacity exceeded");
  }
  lightusd_stage* flat = nullptr;
  if (lightusd_stage_flatten(input, &flat) != LIGHTUSD_OK)
    return Fail(error, lightusd_last_error());
  next->stage = lightusd::api::Stage(flat);
  state_ = std::move(next);
  return true;
}

bool PhysicsPreview::step(const lightusd_stage* authored, double time,
                          int steps, float dt, std::string* error) {
  if (!state_ || !authored)
    return Fail(error, "Physics preview is not initialized");
  if (!std::isfinite(time) || !std::isfinite(dt) || dt < 1e-5f || dt > 0.1f ||
      steps < 1 || steps > 600)
    return Fail(error, "Invalid physics step/timestep");
  state_->world.timestep = dt;
  for (size_t i = 0; i < state_->paths.size(); ++i) {
    auto& body = state_->bodies[i];
    if (body.body_type != TYDRA_PHYS_BODY_KINEMATIC) continue;
    Pose pose;
    const auto p =
        lightusd_stage_prim_at_path(authored, state_->paths[i].c_str());
    if (!ReadPose(authored, p, time, &pose))
      return Fail(error, "Kinematic body no longer has a rigid transform");
    body.linear_velocity = tp_v3_scale(
        tp_v3_sub(pose.xform.position, body.xform.position), 1 / dt);
    auto delta =
        tp_q_mul(pose.xform.rotation, tp_q_conjugate(body.xform.rotation));
    if (delta.w < 0) {
      delta.x = -delta.x;
      delta.y = -delta.y;
      delta.z = -delta.z;
      delta.w = -delta.w;
    }
    TydraPhysVec3 axis{};
    const float angle = tp_q_to_axis_angle(delta, &axis);
    body.angular_velocity = tp_v3_scale(axis, angle / dt);
    body.xform = pose.xform;
  }
  for (int i = 0; i < steps; ++i)
    if (tydra_phys_step(&state_->world) != TYDRA_PHYS_OK)
      return Fail(error, "Diagnostic solver failed");
  for (int i = 0; i < state_->world.num_bodies; ++i) {
    const auto& body = state_->bodies[static_cast<size_t>(i)];
    for (float n :
         {body.xform.position.x, body.xform.position.y, body.xform.position.z,
          body.xform.rotation.x, body.xform.rotation.y, body.xform.rotation.z,
          body.xform.rotation.w, body.linear_velocity.x, body.linear_velocity.y,
          body.linear_velocity.z, body.angular_velocity.x,
          body.angular_velocity.y, body.angular_velocity.z})
      if (!std::isfinite(n))
        return Fail(error, "Diagnostic solver produced a nonfinite state");
  }
  for (size_t i = 0; i < state_->paths.size(); ++i) {
    const auto& body = state_->bodies[i];
    if (body.body_type != TYDRA_PHYS_BODY_DYNAMIC) continue;
    TydraPhysMat4 rotation;
    tp_q_to_m4(body.xform.rotation, &rotation);
    float m[16];
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) m[c * 4 + r] = rotation.m[r * 4 + c];
    for (int c = 0; c < 3; ++c)
      for (int r = 0; r < 3; ++r) m[c * 4 + r] *= state_->poses[i].scale[c];
    m[12] = body.xform.position.x;
    m[13] = body.xform.position.y;
    m[14] = body.xform.position.z;
    double matrix[16];
    for (int j = 0; j < 16; ++j) matrix[j] = m[j];
    const auto& path = state_->paths[i];
    const char* order[] = {"!resetXformStack!",
                           "xformOp:transform:physicsPreview"};
    if (lightusd_attr_set(state_->stage.get(), path.c_str(), order[1],
                          LIGHTUSD_TYPE_MATRIX4D, 0, matrix, 1,
                          0) != LIGHTUSD_OK ||
        lightusd_attr_set_token_array(state_->stage.get(), path.c_str(),
                                      "xformOpOrder", LIGHTUSD_TYPE_TOKEN,
                                      order, 2,
                                      LIGHTUSD_PROP_UNIFORM) != LIGHTUSD_OK)
      return Fail(error, lightusd_last_error());
  }
  return true;
}
}  // namespace lusdview
