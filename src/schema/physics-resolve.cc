// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - resolved UsdPhysics object model (see physics-resolve.hh)

#include "physics-resolve.hh"

#include <cmath>
#include <map>

#include "stage/stage.hh"

namespace lightusd {
namespace {

bool BoolOr(const UsdPrim& prim, const char* name, bool fallback) {
  if (const Value* v = prim.GetPropertyValue(name)) {
    if (const bool* b = v->as_bool()) return *b;
  }
  return fallback;
}

std::vector<std::string> Targets(const UsdPrim& prim, const char* name) {
  std::vector<std::string> out;
  if (const std::vector<Path>* t = prim.GetRelationship(name)) {
    for (const Path& p : *t) out.push_back(p.str());
  }
  return out;
}

std::string FirstTarget(const UsdPrim& prim, const char* name) {
  const std::vector<std::string> t = Targets(prim, name);
  return t.empty() ? std::string() : t[0];
}

std::string ParentOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "";
  return path.substr(0, slash);
}

bool IsEnabledBody(const UsdPrim& prim) {
  return prim.HasAPI("PhysicsRigidBodyAPI") &&
         BoolOr(prim, "physics:rigidBodyEnabled", true);
}

std::string NearestEnabledBody(const Stage& stage, const std::string& from) {
  for (std::string p = from; !p.empty(); p = ParentOf(p)) {
    const UsdPrim prim = stage.GetPrimAtPath(p);
    if (prim.IsValid() && IsEnabledBody(prim)) return p;
  }
  return "";
}

// _GetBodyPrim: first enabled body walking up, else the highest collider.
std::string JointBody(const Stage& stage, const std::string& target) {
  if (target.empty()) return "";
  std::string collision;
  for (std::string p = target; !p.empty(); p = ParentOf(p)) {
    const UsdPrim prim = stage.GetPrimAtPath(p);
    if (!prim.IsValid()) {
      if (p == target) return "";  // dangling target
      continue;
    }
    if (IsEnabledBody(prim)) return p;
    if (prim.HasAPI("PhysicsCollisionAPI")) collision = p;
  }
  return collision;
}

// Prim types UsdPhysics turns into collision shapes (non-geometric prims
// with CollisionAPI produce no collider, but still anchor joint bodies).
bool IsShapeType(const std::string& type) {
  static const char* kShapes[] = {"Cube", "Sphere", "Capsule", "Capsule_1", "Cylinder",
                                  "Cylinder_1", "Cone", "Mesh", "Plane", "Points"};
  for (const char* t : kShapes) if (type == t) return true;
  return false;
}

bool IsJointType(const std::string& type) {
  return type == "PhysicsJoint" || type == "PhysicsFixedJoint" ||
         type == "PhysicsRevoluteJoint" || type == "PhysicsPrismaticJoint" ||
         type == "PhysicsSphericalJoint" || type == "PhysicsDistanceJoint";
}

}  // namespace

ResolvedPhysics ResolvePhysics(const Stage& stage) {
  ResolvedPhysics out;
  const std::string& up = stage.GetUpAxis();
  const float mpu = static_cast<float>(stage.GetMetersPerUnit());
  std::map<std::string, size_t> merge_index;

  stage.Traverse([&](const UsdPrim& prim) {
    const std::string& type = prim.GetTypeName();
    const std::string path = prim.GetPath().str();
    if (type == "PhysicsScene") {
      ResolvedPhysics::Scene s;
      s.path = path;
      float d[3] = {0, 0, 0};
      if (const Value* v = prim.GetPropertyValue("physics:gravityDirection")) {
        if (const float* f = v->as_float3()) { d[0] = f[0]; d[1] = f[1]; d[2] = f[2]; }
      }
      if (d[0] == 0 && d[1] == 0 && d[2] == 0) {
        if (up == "X") d[0] = -1; else if (up == "Z") d[2] = -1; else d[1] = -1;
      } else {
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& c : d) c /= len;
      }
      for (int k = 0; k < 3; ++k) s.gravity_direction[k] = d[k];
      float mag = -INFINITY;
      if (const Value* v = prim.GetPropertyValue("physics:gravityMagnitude")) {
        if (const float* f = v->as_float()) mag = *f;
      }
      // Schema doc: "A negative value is a request to use a value equivalent
      // to earth gravity" (the fallback is -inf). OpenUSD's parser only
      // replaces the -inf sentinel (usdspec-conformance issues/OpenUSD/15).
      s.gravity_magnitude = (mag < 0.0f) ? 9.81f / mpu : mag;
      out.scenes.push_back(s);
    }
    if (prim.HasAPI("PhysicsRigidBodyAPI")) {
      ResolvedPhysics::Body b;
      b.path = path;
      b.enabled = BoolOr(prim, "physics:rigidBodyEnabled", true);
      b.simulation_owners = Targets(prim, "physics:simulationOwner");
      out.bodies.push_back(b);
    }
    if (prim.HasAPI("PhysicsCollisionAPI") && IsShapeType(type)) {
      ResolvedPhysics::Collider c;
      c.path = path;
      c.rigid_body = NearestEnabledBody(stage, path);
      c.collision_enabled = BoolOr(prim, "physics:collisionEnabled", true);
      c.simulation_owners = Targets(prim, "physics:simulationOwner");
      out.colliders.push_back(c);
    }
    if (IsJointType(type)) {
      ResolvedPhysics::Joint j;
      j.path = path;
      j.type = type;
      j.rel0 = FirstTarget(prim, "physics:body0");
      j.rel1 = FirstTarget(prim, "physics:body1");
      j.body0 = JointBody(stage, j.rel0);
      j.body1 = JointBody(stage, j.rel1);
      j.joint_enabled = BoolOr(prim, "physics:jointEnabled", true);
      j.collision_enabled = BoolOr(prim, "physics:collisionEnabled", false);
      j.exclude_from_articulation = BoolOr(prim, "physics:excludeFromArticulation", false);
      out.joints.push_back(j);
    }
    if (type == "PhysicsCollisionGroup") {
      ResolvedPhysics::CollisionGroup g;
      g.path = path;
      g.filtered_groups = Targets(prim, "physics:filteredGroups");
      g.invert_filtered_groups = BoolOr(prim, "physics:invertFilteredGroups", false);
      if (const Value* v = prim.GetPropertyValue("physics:mergeGroup")) {
        const StringView s = v->as_string();
        g.merge_group.assign(s.data(), s.size());
      }
      if (!g.merge_group.empty()) {
        const auto it = merge_index.find(g.merge_group);
        if (it != merge_index.end()) {
          auto& rep = out.collision_groups[it->second];
          rep.merged_groups.push_back(path);
          rep.filtered_groups.insert(rep.filtered_groups.end(), g.filtered_groups.begin(),
                                     g.filtered_groups.end());
          return true;
        }
        merge_index[g.merge_group] = out.collision_groups.size();
        g.merged_groups.push_back(path);
      }
      out.collision_groups.push_back(g);
    }
    return true;
  });
  for (auto& b : out.bodies) {
    if (!b.enabled) continue;
    for (const auto& c : out.colliders) {
      if (c.rigid_body == b.path) b.collisions.push_back(c.path);
    }
  }
  return out;
}

}  // namespace lightusd
