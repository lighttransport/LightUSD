// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "core/prim.hh"
#include "scene-access.hh"
#include "usdGeom.hh"
#include "usdPhysics.hh"

#include <map>
#include <string>
#include <vector>

namespace lightusd {

namespace {

bool PrimHasAPISchema(const Prim &prim, APISchemas::APIName api_name) {
  const PrimMeta &meta = prim.metas();
  if (!meta.has_apiSchemas()) return false;
  const APISchemas &schemas = meta.get_apiSchemas();
  for (const auto &entry : schemas.names) {
    if (entry.first == api_name) return true;
  }
  return false;
}

// Direct access to a prim's generic Property map. tydra::GetProperty
// only dispatches a fixed set of typed prims; for collision-filter
// schemas we need to reach into PhysicsCollisionGroup, Cube, Xform,
// and friends. Returns nullptr for unsupported prim types.
const std::map<std::string, Property> *PrimPropsMap(const Prim &prim) {
#define X(T)                                              \
  if (prim.is<T>()) return &prim.as<T>()->props;
  X(Model)
  X(Xform)
  X(Scope)
  X(Material)
  X(GeomMesh)
  X(GeomCube)
  X(GeomSphere)
  X(GeomCylinder)
  X(GeomCapsule)
  X(GeomCone)
  X(PhysicsScene)
  X(PhysicsCollisionGroup)
  X(PhysicsRevoluteJoint)
  X(PhysicsPrismaticJoint)
  X(PhysicsFixedJoint)
  X(PhysicsSphericalJoint)
  X(PhysicsDistanceJoint)
  X(PhysicsJoint)
#undef X
  return nullptr;
}

bool ResolveRelTargets(const Prim &prim, const std::string &rel_name,
                       std::vector<Path> *out) {
  const auto *props = PrimPropsMap(prim);
  if (!props) return false;
  auto it = props->find(rel_name);
  if (it == props->end()) return false;
  const Property &prop = it->second;
  if (!prop.is_relationship()) return false;
  const Relationship &rel = prop.get_relationship();
  if (rel.is_path()) {
    out->push_back(rel.targetPath);
  } else if (rel.is_pathvector()) {
    *out = rel.targetPathVector;
  }
  return true;
}

}  // namespace

bool GetPhysicsFilteredPairsAPI(const Prim &prim,
                                PhysicsFilteredPairsAPI *out) {
  if (!out) return false;
  if (!PrimHasAPISchema(prim, APISchemas::APIName::PhysicsFilteredPairsAPI)) {
    return false;
  }
  const auto *props = PrimPropsMap(prim);
  if (!props) return false;
  auto it = props->find("physics:filteredPairs");
  if (it == props->end()) return false;
  const Property &prop = it->second;
  if (!prop.is_relationship()) return false;
  out->filteredPairs = RelationshipProperty(prop.get_relationship());
  return true;
}

bool GetPhysicsCollidersCollection(const Prim &prim,
                                   std::vector<Path> *includes,
                                   std::vector<Path> *excludes) {
  if (!prim.is<PhysicsCollisionGroup>()) {
    return false;
  }
  bool any = false;
  if (includes) {
    includes->clear();
    if (ResolveRelTargets(prim, "collection:colliders:includes", includes)) {
      any = true;
    }
  }
  if (excludes) {
    excludes->clear();
    if (ResolveRelTargets(prim, "collection:colliders:excludes", excludes)) {
      any = true;
    }
  }
  return any;
}

namespace {

// Extract a non-animatable scalar/vec/quat property (stored value type T) into a
// Typed[WithFallback]Attribute target. No-op when the property is absent.
template <typename T, typename Target>
void ExtractTypedValue(const std::map<std::string, Property> &props,
                       const char *name, Target *target) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) return;
  if (auto v = it->second.get_attribute().get_value<T>()) {
    target->set_value(*v);
  }
}

// Animatable counterpart: wrap the authored default into Animatable<T>.
template <typename T, typename Target>
void ExtractAnimatableValue(const std::map<std::string, Property> &props,
                            const char *name, Target *target) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) return;
  if (auto v = it->second.get_attribute().get_value<T>()) {
    target->set_value(Animatable<T>(*v));
  }
}

void ExtractRel(const std::map<std::string, Property> &props, const char *name,
                RelationshipProperty *target) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_relationship()) return;
  *target = RelationshipProperty(it->second.get_relationship());
}

}  // namespace

bool GetPhysicsRigidBodyAPI(const Prim &prim, PhysicsRigidBodyAPI *out) {
  if (!out) return false;
  if (!PrimHasAPISchema(prim, APISchemas::APIName::PhysicsRigidBodyAPI)) return false;
  const auto *props = PrimPropsMap(prim);
  if (!props) return false;
  ExtractTypedValue<bool>(*props, "physics:rigidBodyEnabled", &out->rigidBodyEnabled);
  ExtractTypedValue<bool>(*props, "physics:kinematicEnabled", &out->kinematicEnabled);
  ExtractRel(*props, "physics:simulationOwner", &out->simulationOwner);
  ExtractTypedValue<bool>(*props, "physics:startsAsleep", &out->startsAsleep);
  ExtractTypedValue<float>(*props, "physics:mass", &out->mass);
  ExtractTypedValue<float>(*props, "physics:density", &out->density);
  ExtractTypedValue<value::point3f>(*props, "physics:centerOfMass", &out->centerOfMass);
  ExtractTypedValue<value::float3>(*props, "physics:diagonalInertia", &out->diagonalInertia);
  ExtractTypedValue<value::quatf>(*props, "physics:principalAxes", &out->principalAxes);
  ExtractAnimatableValue<value::vector3f>(*props, "physics:velocity", &out->velocity);
  ExtractAnimatableValue<value::vector3f>(*props, "physics:angularVelocity", &out->angularVelocity);
  return true;
}

bool GetPhysicsCollisionAPI(const Prim &prim, PhysicsCollisionAPI *out) {
  if (!out) return false;
  if (!PrimHasAPISchema(prim, APISchemas::APIName::PhysicsCollisionAPI)) return false;
  const auto *props = PrimPropsMap(prim);
  if (!props) return false;
  ExtractTypedValue<bool>(*props, "physics:collisionEnabled", &out->collisionEnabled);
  ExtractRel(*props, "physics:simulationOwner", &out->simulationOwner);
  return true;
}

bool GetPhysicsMaterialAPI(const Prim &prim, PhysicsMaterialAPI *out) {
  if (!out) return false;
  if (!PrimHasAPISchema(prim, APISchemas::APIName::PhysicsMaterialAPI)) return false;
  const auto *props = PrimPropsMap(prim);
  if (!props) return false;
  ExtractTypedValue<float>(*props, "physics:staticFriction", &out->staticFriction);
  ExtractTypedValue<float>(*props, "physics:dynamicFriction", &out->dynamicFriction);
  ExtractTypedValue<float>(*props, "physics:restitution", &out->restitution);
  ExtractTypedValue<float>(*props, "physics:density", &out->density);
  return true;
}

bool GetPhysicsMassAPI(const Prim &prim, PhysicsMassAPI *out) {
  if (!out) return false;
  if (!PrimHasAPISchema(prim, APISchemas::APIName::PhysicsMassAPI)) return false;
  const auto *props = PrimPropsMap(prim);
  if (!props) return false;
  ExtractTypedValue<float>(*props, "physics:mass", &out->mass);
  ExtractTypedValue<float>(*props, "physics:density", &out->density_);
  ExtractTypedValue<value::point3f>(*props, "physics:centerOfMass", &out->centerOfMass);
  ExtractTypedValue<value::float3>(*props, "physics:diagonalInertia", &out->diagonalInertia);
  ExtractTypedValue<value::quatf>(*props, "physics:principalAxes", &out->principalAxes);
  return true;
}

bool GetPhysicsMeshCollisionAPI(const Prim &prim, PhysicsMeshCollisionAPI *out) {
  if (!out) return false;
  if (!PrimHasAPISchema(prim, APISchemas::APIName::PhysicsMeshCollisionAPI)) return false;
  const auto *props = PrimPropsMap(prim);
  if (!props) return false;
  ExtractTypedValue<value::token>(*props, "physics:approximation", &out->approximation);
  return true;
}

}  // namespace lightusd
