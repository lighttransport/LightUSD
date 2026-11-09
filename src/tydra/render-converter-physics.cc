// SPDX-License-Identifier: Apache-2.0
#include "render-converter-internal.hh"
#include "schema/usdPhysics.hh"
#include <string>
#include <vector>
namespace lightusd { namespace tydra {
using ::lightusd::Stage; using ::lightusd::UsdPrim;
namespace {
std::string ValueSummary(const Value& value) {
  if (const bool* v = value.as_bool()) return *v ? "true" : "false";
  if (const int32_t* v = value.as_int()) return std::to_string(*v);
  if (const int64_t* v = value.as_int64()) return std::to_string(*v);
  if (const float* v = value.as_float()) return std::to_string(*v);
  if (const double* v = value.as_double()) return std::to_string(*v);
  if (auto v = value.as_string()) return *v;
  if (auto v = value.as_token()) return *v;
  if (auto v = value.as_asset_path()) return *v;
  const char* name = ::lightusd::GetTypeName(value.type_id());
  return name ? name : "value";
}

bool IsPhysicsExtensionPropertyName(const std::string& name) {
  return name.rfind("mjc:", 0) == 0 ||
         name.rfind("newton:", 0) == 0 ||
         name.rfind("physx", 0) == 0 ||
         name.rfind("state:", 0) == 0;
}

std::vector<PhysicsProperty> CollectPhysicsExtensionProperties(
    const UsdPrim& prim) {
  std::vector<PhysicsProperty> props;
  for (const std::string& name : prim.GetPropertyNames()) {
    if (!IsPhysicsExtensionPropertyName(name)) continue;
    if (const Value* value = prim.GetPropertyValue(name)) {
      PhysicsProperty prop;
      prop.name = name;
      prop.value = ValueSummary(*value);
      props.push_back(std::move(prop));
    }
  }
  for (const std::string& name : prim.GetRelationshipNames()) {
    if (!IsPhysicsExtensionPropertyName(name)) continue;
    PhysicsProperty prop;
    prop.name = name;
    const std::vector<::lightusd::Path>* targets =
        prim.GetRelationship(name);
    if (targets) {
      prop.value = "rel[" + std::to_string(targets->size()) + "]";
    }
    props.push_back(std::move(prop));
  }
  return props;
}

Float3 Float3FromArray(const float v[3]) {
  return Float3(v[0], v[1], v[2]);
}

Float4 Float4FromArray(const float v[4]) {
  return Float4(v[0], v[1], v[2], v[3]);
}
}  // namespace

void RenderSceneConverter::Impl::ExtractPhysicsAnnotations(const Stage& stage,
                                                     RenderScene* scene) {
  if (!scene) return;

  stage.Traverse([&](const UsdPrim& prim) {
    const std::string path = prim.GetPath().str();

    if (::lightusd::IsPhysicsScene(prim)) {
      ::lightusd::PhysicsSceneData data;
      if (::lightusd::GetPhysicsSceneData(stage, prim, &data,
                                                config_.time_code)) {
        PhysicsSceneAnnotation out;
        out.prim_path = path;
        out.gravity_direction = Float3FromArray(data.gravityDirection);
        out.gravity_magnitude = data.gravityMagnitude;
        out.extension_properties = CollectPhysicsExtensionProperties(prim);
        scene->physics.scenes.push_back(std::move(out));
      }
    }

    if (::lightusd::HasPhysicsRigidBodyAPI(prim) ||
        ::lightusd::HasPhysicsMassAPI(prim)) {
      PhysicsRigidBodyAnnotation out;
      out.prim_path = path;
      if (::lightusd::HasPhysicsRigidBodyAPI(prim)) {
        ::lightusd::PhysicsRigidBodyData data;
        if (::lightusd::GetPhysicsRigidBodyData(stage, prim, &data,
                                                      config_.time_code)) {
          out.rigid_body_enabled = data.rigidBodyEnabled;
          out.kinematic_enabled = data.kinematicEnabled;
          out.simulation_owner = data.simulationOwner;
          out.velocity = Float3FromArray(data.velocity);
          out.angular_velocity = Float3FromArray(data.angularVelocity);
          out.starts_asleep = data.startsAsleep;
        }
      }
      if (::lightusd::HasPhysicsMassAPI(prim)) {
        ::lightusd::PhysicsMassData data;
        if (::lightusd::GetPhysicsMassData(stage, prim, &data)) {
          out.has_mass = true;
          out.mass = data.mass;
          out.density = data.density;
          out.center_of_mass = Float3FromArray(data.centerOfMass);
          out.diagonal_inertia = Float3FromArray(data.diagonalInertia);
          out.principal_axes = Float4FromArray(data.principalAxes);
        }
      }
      out.extension_properties = CollectPhysicsExtensionProperties(prim);
      scene->physics.rigid_bodies.push_back(std::move(out));
    }

    if (::lightusd::HasPhysicsCollisionAPI(prim) ||
        ::lightusd::HasPhysicsMeshCollisionAPI(prim)) {
      PhysicsColliderAnnotation out;
      out.prim_path = path;
      if (::lightusd::HasPhysicsCollisionAPI(prim)) {
        ::lightusd::PhysicsCollisionData data;
        if (::lightusd::GetPhysicsCollisionData(stage, prim, &data)) {
          out.collision_enabled = data.collisionEnabled;
          out.simulation_owner = data.simulationOwner;
        }
      }
      if (::lightusd::HasPhysicsMeshCollisionAPI(prim)) {
        ::lightusd::PhysicsMeshCollisionData data;
        if (::lightusd::GetPhysicsMeshCollisionData(prim, &data)) {
          out.has_mesh_collision = true;
          out.approximation = data.approximation;
        }
      }
      out.extension_properties = CollectPhysicsExtensionProperties(prim);
      scene->physics.colliders.push_back(std::move(out));
    }

    if (::lightusd::IsPhysicsJoint(prim)) {
      PhysicsJointAnnotation out;
      out.prim_path = path;
      out.type_name = prim.GetTypeName();

      ::lightusd::PhysicsJointData base;
      if (::lightusd::GetPhysicsJointData(stage, prim, &base,
                                                config_.time_code)) {
        out.body0 = base.body0;
        out.body1 = base.body1;
        out.has_body0 = base.hasBody0;
        out.has_body1 = base.hasBody1;
        out.local_pos0 = Float3FromArray(base.localPos0);
        out.local_pos1 = Float3FromArray(base.localPos1);
        out.local_rot0 = Float4FromArray(base.localQuat0);
        out.local_rot1 = Float4FromArray(base.localQuat1);
        out.collision_enabled = base.collisionEnabled;
      }

      if (::lightusd::IsPhysicsRevoluteJoint(prim)) {
        ::lightusd::PhysicsRevoluteJointData data;
        if (::lightusd::GetPhysicsRevoluteJointData(
                stage, prim, &data, config_.time_code)) {
          out.axis = Float3FromArray(data.axis);
          out.lower_limit = data.lowerLimit;
          out.upper_limit = data.upperLimit;
        }
      } else if (::lightusd::IsPhysicsPrismaticJoint(prim)) {
        ::lightusd::PhysicsPrismaticJointData data;
        if (::lightusd::GetPhysicsPrismaticJointData(
                stage, prim, &data, config_.time_code)) {
          out.axis = Float3FromArray(data.axis);
          out.lower_limit = data.lowerLimit;
          out.upper_limit = data.upperLimit;
        }
      } else if (::lightusd::IsPhysicsSliderJoint(prim)) {
        ::lightusd::PhysicsSliderJointData data;
        if (::lightusd::GetPhysicsSliderJointData(
                stage, prim, &data, config_.time_code)) {
          out.axis = Float3FromArray(data.axis);
          out.lower_limit = data.lowerLimit;
          out.upper_limit = data.upperLimit;
        }
      } else if (::lightusd::IsPhysicsSphericalJoint(prim)) {
        ::lightusd::PhysicsSphericalJointData data;
        if (::lightusd::GetPhysicsSphericalJointData(
                stage, prim, &data, config_.time_code)) {
          out.cone_angle0_limit = data.coneAngle0Limit;
          out.cone_angle1_limit = data.coneAngle1Limit;
        }
      } else if (::lightusd::IsPhysicsBallJoint(prim)) {
        ::lightusd::PhysicsBallJointData data;
        if (::lightusd::GetPhysicsBallJointData(
                stage, prim, &data, config_.time_code)) {
          out.cone_angle0_limit = data.coneAngle0Limit;
          out.cone_angle1_limit = data.coneAngle1Limit;
        }
      } else if (::lightusd::IsPhysicsDistanceJoint(prim)) {
        ::lightusd::PhysicsDistanceJointData data;
        if (::lightusd::GetPhysicsDistanceJointData(
                stage, prim, &data, config_.time_code)) {
          out.min_distance = data.minDistance;
          out.max_distance = data.maxDistance;
        }
      }

      out.extension_properties = CollectPhysicsExtensionProperties(prim);
      scene->physics.joints.push_back(std::move(out));
    }

    if (::lightusd::HasPhysicsMaterialAPI(prim)) {
      ::lightusd::PhysicsMaterialData data;
      if (::lightusd::GetPhysicsMaterialData(stage, prim, &data)) {
        PhysicsMaterialAnnotation out;
        out.prim_path = path;
        out.static_friction = data.staticFriction;
        out.dynamic_friction = data.dynamicFriction;
        out.restitution = data.restitution;
        out.density = data.density;
        out.extension_properties = CollectPhysicsExtensionProperties(prim);
        scene->physics.materials.push_back(std::move(out));
      }
    }

    if (::lightusd::HasPhysicsFilteredPairsAPI(prim)) {
      ::lightusd::PhysicsFilteredPairsData data;
      if (::lightusd::GetPhysicsFilteredPairsData(prim, &data)) {
        PhysicsFilteredPairsAnnotation out;
        out.prim_path = path;
        out.filtered_pair_paths = std::move(data.filteredPairPaths);
        scene->physics.filtered_pairs.push_back(std::move(out));
      }
    }

    if (::lightusd::HasPhysicsArticulationRootAPI(prim)) {
      scene->physics.articulation_roots.push_back(path);
    }

    return true;
  });
}
}}  // namespace lightusd::tydra
