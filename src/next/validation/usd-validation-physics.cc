// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Light Transport Entertainment Inc.
#include "usd-validation-internal.hh"
#include <algorithm>
#include <cmath>
#include <limits>
namespace lightusd {
namespace next {
namespace validation_detail {

void ValidateLuxRelationships(const PrimSpec &ps,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  for (const char *name :
       {"light:filters", "portals", "proxyPrim", "geometry"}) {
    if (!HasProperty(ps, name)) {
      continue;
    }
    if (!IsRelationshipProp(ps, name)) {
      AddError(result, "lux.relationship.target",
               MakePropertyLocation(prim_location, name),
               std::string(name) + " must be a relationship");
      continue;
    }
    ValidateRelationshipTargetPaths(ps, name, "lux.relationship.target",
                                    MakePropertyLocation(prim_location, name),
                                    result);
  }
}

void ValidateLuxLight(const PrimSpec &ps, const std::string &prim_location,
                      USDValidationResult *result) {
  static const std::set<std::string> kTextureFormats = {
      "automatic", "latlong", "mirroredBall", "angular"};
  static const std::set<std::string> kPoleAxis = {"scene", "Y", "Z"};
  const std::string &type_name = ps.type_name();

  ValidateNonNegativeNumericProperty(ps, "inputs:intensity",
                                     "lux.light.inputs", prim_location,
                                     result);
  ValidateFiniteNumericProperty(ps, "inputs:exposure", "lux.light.inputs",
                                prim_location, result);
  ValidateNonNegativeNumericProperty(ps, "inputs:diffuse", "lux.light.inputs",
                                     prim_location, result);
  ValidateNonNegativeNumericProperty(ps, "inputs:specular",
                                     "lux.light.inputs", prim_location,
                                     result);
  ValidatePositiveNumericProperty(ps, "inputs:colorTemperature",
                                  "lux.light.inputs", prim_location, result);
  ValidateNumericRangeProperty(ps, "inputs:shaping:cone:angle", 0.0, 180.0,
                               "lux.shaping.inputs", prim_location, result);
  ValidateNumericRangeProperty(ps, "inputs:shaping:cone:softness", 0.0, 1.0,
                               "lux.shaping.inputs", prim_location, result);
  ValidateNonNegativeNumericProperty(ps, "inputs:shaping:focus",
                                     "lux.shaping.inputs", prim_location,
                                     result);
  ValidateNonNegativeNumericProperty(ps, "inputs:shaping:ies:angleScale",
                                     "lux.shaping.inputs", prim_location,
                                     result);
  ValidateNumericMinProperty(ps, "inputs:shadow:distance", -1.0,
                             "lux.shadow.inputs", prim_location, result);
  ValidateNumericMinProperty(ps, "inputs:shadow:falloff", -1.0,
                             "lux.shadow.inputs", prim_location, result);
  ValidateNumericMinProperty(ps, "inputs:shadow:falloffGamma", 0.0,
                             "lux.shadow.inputs", prim_location, result);

  if (type_name == "SphereLight" || type_name == "DiskLight") {
    ValidatePositiveNumericProperty(ps, "inputs:radius", "lux.light.size",
                                    prim_location, result);
  } else if (type_name == "CylinderLight") {
    ValidatePositiveNumericProperty(ps, "inputs:radius", "lux.light.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "inputs:length", "lux.light.size",
                                    prim_location, result);
  } else if (type_name == "RectLight") {
    ValidatePositiveNumericProperty(ps, "inputs:width", "lux.light.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "inputs:height", "lux.light.size",
                                    prim_location, result);
    ValidateAssetPathProperty(ps, "inputs:texture:file", "lux.texture.file",
                              prim_location, result);
  } else if (type_name == "DistantLight") {
    ValidateNumericRangeProperty(ps, "inputs:angle", 0.0, 180.0,
                                 "lux.light.angle", prim_location, result);
  } else if (type_name == "DomeLight" || type_name == "DomeLight_1") {
    ValidatePositiveNumericProperty(ps, "guideRadius", "lux.light.size",
                                    prim_location, result);
    ValidateAssetPathProperty(ps, "inputs:texture:file", "lux.texture.file",
                              prim_location, result);
    ValidateTokenSetProperty(ps, "inputs:texture:format", kTextureFormats,
                             "lux.texture.format", prim_location, result);
    if (type_name == "DomeLight_1") {
      ValidateTokenSetProperty(ps, "poleAxis", kPoleAxis, "lux.dome.poleAxis",
                               prim_location, result);
    }
  }

  ValidateLuxRelationships(ps, prim_location, result);
}

// ---------------------------------------------------------------------------
// physics rules
// ---------------------------------------------------------------------------

bool IsPhysicsDofName(const std::string &dof) {
  static const std::set<std::string> kDofs = {
      "transX", "transY", "transZ", "rotX", "rotY", "rotZ", "distance"};
  return kDofs.count(dof) > 0;
}

bool HasAppliedPhysicsSchema(const std::vector<AppliedSchema> &schemas) {
  for (const AppliedSchema &schema : schemas) {
    if (IsPhysicsSchemaName(schema.name)) {
      return true;
    }
  }
  return false;
}

// Every property name of the prim (attribute slots + relationships).
std::vector<std::string> AllPropertyNames(const PrimSpec &ps) {
  std::vector<std::string> names;
  names.reserve(ps.properties().size());
  for (const PropSlot &slot : ps.properties().slots()) {
    names.emplace_back(GetPropNameTable().get(slot.name_id));
  }
  for (const std::string &rel : ps.relationship_names()) {
    if (!ps.property(rel)) {
      names.push_back(rel);
    }
  }
  return names;
}

bool HasSchemaOrPropertyPrefix(const PrimSpec &ps,
                               const std::vector<AppliedSchema> &schemas,
                               const std::string &schema_prefix,
                               const std::string &prop_prefix) {
  for (const AppliedSchema &schema : schemas) {
    if (StartsWith(schema.name, schema_prefix)) {
      return true;
    }
  }
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    if (StartsWith(prop_name, prop_prefix)) {
      return true;
    }
  }
  return false;
}

void ValidatePhysicsRelationship(const PrimSpec &ps,
                                 const std::string &prop_name,
                                 const std::string &rule_id,
                                 const std::string &prim_location,
                                 const PrimTypeByPath &prim_types,
                                 const std::set<std::string> &target_types,
                                 USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  if (!IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a relationship");
    return;
  }
  ValidateRelationshipTargetPaths(
      ps, prop_name, rule_id, MakePropertyLocation(prim_location, prop_name),
      result);
  if (!target_types.empty()) {
    ValidateRelationshipTargetPrimTypes(
        ps, prop_name, rule_id,
        MakePropertyLocation(prim_location, prop_name), prim_types,
        target_types, result);
  }
}

void ValidatePhysicsScene(const PrimSpec &ps,
                          const std::string &prim_location,
                          USDValidationResult *result) {
  ValidateNonNegativeNumericProperty(ps, "physics:gravityMagnitude",
                                     "physics.scene.gravity", prim_location,
                                     result);

  if (!HasProperty(ps, "physics:gravityDirection")) {
    return;
  }
  std::array<double, 3> v;
  const Value *value = GetAttrValue(ps, "physics:gravityDirection");
  if (IsRelationshipProp(ps, "physics:gravityDirection") || !value ||
      !ValueToVec3(*value, &v)) {
    AddError(result, "physics.scene.gravity",
             MakePropertyLocation(prim_location, "physics:gravityDirection"),
             "physics:gravityDirection must be a vector3f attribute");
    return;
  }
  const double len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  // (0, 0, 0) is the schema fallback: "use the negative upAxis".
  (void)len2;
  if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
    AddError(result, "physics.scene.gravity",
             MakePropertyLocation(prim_location, "physics:gravityDirection"),
             "physics:gravityDirection must be finite");
  }
}

void ValidatePhysicsInertiaAndMotion(const PrimSpec &ps,
                                     const std::string &prim_location,
                                     USDValidationResult *result) {
  ValidateFiniteVec3Property(ps, "physics:centerOfMass", "physics.inertia",
                             prim_location, result, false);
  ValidateFiniteNonNegativeVec3Property(ps, "physics:diagonalInertia",
                                        "physics.inertia", prim_location,
                                        result);
  ValidateQuaternionProperty(ps, "physics:principalAxes",
                             "physics.quaternion", prim_location, result, true);
  ValidateFiniteVec3Property(ps, "physics:velocity", "physics.motion",
                             prim_location, result, false);
  ValidateFiniteVec3Property(ps, "physics:angularVelocity", "physics.motion",
                             prim_location, result, false);
}

void ValidatePhysicsJointTransforms(const PrimSpec &ps,
                                    const std::string &prim_location,
                                    USDValidationResult *result) {
  ValidateFiniteVec3Property(ps, "physics:localPos0",
                             "physics.joint.transform", prim_location, result,
                             false);
  ValidateFiniteVec3Property(ps, "physics:localPos1",
                             "physics.joint.transform", prim_location, result,
                             false);
  ValidateQuaternionProperty(ps, "physics:localRot0", "physics.quaternion",
                             prim_location, result);
  ValidateQuaternionProperty(ps, "physics:localRot1", "physics.quaternion",
                             prim_location, result);
}

void ValidatePhysicsScalarProperties(const PrimSpec &ps,
                                     const std::string &prim_location,
                                     USDValidationResult *result) {
  for (const char *prop_name :
       {"physics:mass", "physics:density", "physics:staticFriction",
        "physics:dynamicFriction", "physics:restitution",
        "physics:breakForce", "physics:breakTorque"}) {
    ValidateNonNegativeNumericProperty(ps, prop_name, "physics.value.range",
                                       prim_location, result);
  }
}

void ValidatePhysicsCollisionGroup(const PrimSpec &ps,
                                   const std::string &prim_location,
                                   const PrimTypeByPath &prim_types,
                                   USDValidationResult *result) {
  std::string merge_group;
  CheckDeclaredAttrType(ps, "physics:mergeGroup", "string", prim_location,
                        result);
  if (GetTokenProperty(ps, "physics:mergeGroup", &merge_group) &&
      merge_group.empty()) {
    AddError(result, "physics.collisionGroup.mergeGroup",
             MakePropertyLocation(prim_location, "physics:mergeGroup"),
             "physics:mergeGroup must not be empty when authored");
  }
  ValidatePhysicsRelationship(ps, "physics:filteredGroups",
                              "physics.collisionGroup.filteredGroups",
                              prim_location, prim_types,
                              {"PhysicsCollisionGroup"}, result);
}

void ValidatePhysicsApproximation(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  USDValidationResult *result) {
  static const std::set<std::string> kApproximation = {
      "convexDecomposition", "convexHull",     "meshSimplification",
      "boundingCube",        "boundingSphere", "none"};
  ValidateTokenSetProperty(ps, "physics:approximation", kApproximation,
                           "physics.collision.approximation", prim_location,
                           result);
}

void ValidatePhysicsJointLimits(const PrimSpec &ps,
                                const std::string &prim_location,
                                USDValidationResult *result) {
  static const std::set<std::string> kAxis = {"X", "Y", "Z"};
  ValidateTokenSetProperty(ps, "physics:axis", kAxis, "physics.joint.axis",
                           prim_location, result);

  ValidateFiniteNumericProperty(ps, "physics:lowerLimit",
                                "physics.joint.limit", prim_location, result);
  ValidateFiniteNumericProperty(ps, "physics:upperLimit",
                                "physics.joint.limit", prim_location, result);
  double lower = 0.0;
  double upper = 0.0;
  if (GetNumericScalarProperty(ps, "physics:lowerLimit", &lower) &&
      GetNumericScalarProperty(ps, "physics:upperLimit", &upper) &&
      lower > upper) {
    AddError(result, "physics.joint.limit", prim_location,
             "physics:lowerLimit must be <= physics:upperLimit");
  }

  ValidateNumericRangeProperty(ps, "physics:coneAngle0Limit", 0.0, 180.0,
                               "physics.joint.limit", prim_location, result);
  ValidateNumericRangeProperty(ps, "physics:coneAngle1Limit", 0.0, 180.0,
                               "physics.joint.limit", prim_location, result);

  ValidateNonNegativeNumericProperty(ps, "physics:minDistance",
                                     "physics.joint.distance", prim_location,
                                     result);
  ValidateNonNegativeNumericProperty(ps, "physics:maxDistance",
                                     "physics.joint.distance", prim_location,
                                     result);
  double min_distance = 0.0;
  double max_distance = 0.0;
  if (GetNumericScalarProperty(ps, "physics:minDistance", &min_distance) &&
      GetNumericScalarProperty(ps, "physics:maxDistance", &max_distance) &&
      min_distance > max_distance) {
    AddError(result, "physics.joint.distance", prim_location,
             "physics:minDistance must be <= physics:maxDistance");
  }
}

void ValidatePhysicsSchemaPlacement(
    const PrimSpec &ps, const std::vector<AppliedSchema> &schemas,
    const std::string &prim_location, USDValidationResult *result) {
  const std::string &type_name = ps.type_name();
  for (const AppliedSchema &schema : schemas) {
    if ((schema.name == "PhysicsDriveAPI" ||
         schema.name == "PhysicsLimitAPI" || schema.name == "MjcJointAPI" ||
         schema.name == "NewtonMimicAPI") &&
        type_name.find("Joint") == std::string::npos) {
      AddWarning(result, "physics.schemaPlacement", prim_location,
                 schema.name + " is expected on physics joint prims");
    } else if ((schema.name == "PhysicsMeshCollisionAPI" ||
                schema.name == "MjcMeshCollisionAPI" ||
                schema.name == "NewtonMeshCollisionAPI") &&
               type_name != "Mesh") {
      AddWarning(result, "physics.schemaPlacement", prim_location,
                 schema.name + " is expected on Mesh prims");
    } else if ((schema.name == "MjcSceneAPI" ||
                schema.name == "NewtonSceneAPI" ||
                schema.name == "NewtonXpbdSceneAPI" ||
                schema.name == "NewtonKaminoSceneAPI") &&
               type_name != "PhysicsScene") {
      AddWarning(result, "physics.schemaPlacement", prim_location,
                 schema.name + " is expected on PhysicsScene prims");
    }
  }
}

void ValidatePhysicsDriveAndLimitAPIs(
    const PrimSpec &ps, const std::vector<AppliedSchema> &schemas,
    const std::string &prim_location, USDValidationResult *result) {
  static const std::set<std::string> kDriveTypes = {"force", "acceleration"};

  for (const AppliedSchema &schema : schemas) {
    if ((schema.name == "PhysicsDriveAPI" ||
         schema.name == "PhysicsLimitAPI") &&
        !IsPhysicsDofName(schema.instance_name)) {
      AddError(result,
               schema.name == "PhysicsDriveAPI" ? "physics.drive.dof"
                                                : "physics.limit.dof",
               prim_location,
               schema.name + " instance `" + schema.instance_name +
                   "` is not a supported physics DOF");
    }
  }

  std::map<std::string, std::pair<double, bool>> limit_low;
  std::map<std::string, std::pair<double, bool>> limit_high;
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    // Standard multiple-apply names are drive:<dof>:physics:<field> and
    // limit:<dof>:physics:<field>. The historical physics:drive:<dof>:<field>
    // spelling is still checked but flagged as non-standard: UsdPhysics
    // consumers do not read it.
    std::string api, dof, field;
    bool standard = false;
    for (const char *ns : {"drive", "limit"}) {
      const std::string prefix = std::string(ns) + ":";
      const std::string legacy = std::string("physics:") + ns + ":";
      std::string rest;
      if (StartsWith(prop_name, prefix)) {
        rest = prop_name.substr(prefix.size());
        const size_t colon = rest.find(':');
        if (colon == std::string::npos ||
            !StartsWith(rest.substr(colon + 1), "physics:"))
          continue;
        dof = rest.substr(0, colon);
        field = rest.substr(colon + 1 + std::string("physics:").size());
        standard = true;
      } else if (StartsWith(prop_name, legacy)) {
        rest = prop_name.substr(legacy.size());
        const size_t colon = rest.find(':');
        if (colon == std::string::npos) continue;
        dof = rest.substr(0, colon);
        field = rest.substr(colon + 1);
      } else {
        continue;
      }
      api = ns;
      break;
    }
    if (api.empty()) continue;
    const std::string location = MakePropertyLocation(prim_location, prop_name);
    if (!standard) {
      AddWarning(result, "physics.property.nonStandardName", location,
                 "use " + api + ":" + dof + ":physics:" + field +
                     "; UsdPhysics does not read " + prop_name);
    }
    if (api == "drive") {
      if (!IsPhysicsDofName(dof)) {
        AddError(result, "physics.drive.dof", location,
                 "PhysicsDriveAPI DOF `" + dof + "` is not supported");
      }
      if (field == "type") {
        ValidateTokenSetProperty(ps, prop_name, kDriveTypes,
                                 "physics.drive.type", prim_location, result);
      } else if (field == "maxForce" || field == "damping" ||
                 field == "stiffness") {
        ValidateNonNegativeNumericProperty(ps, prop_name,
                                           "physics.drive.value",
                                           prim_location, result);
      } else if (field == "targetPosition" || field == "targetVelocity") {
        ValidateFiniteNumericProperty(ps, prop_name, "physics.drive.value",
                                      prim_location, result);
      }
    } else {
      if (!IsPhysicsDofName(dof)) {
        AddError(result, "physics.limit.dof", location,
                 "PhysicsLimitAPI DOF `" + dof + "` is not supported");
      }
      if (field == "low" || field == "high") {
        // +-inf are the schema fallbacks (unlimited in that direction).
        double value = 0.0;
        const bool have_value =
            GetNumericScalarProperty(ps, prop_name, &value);
        if (have_value && std::isnan(value)) {
          AddError(result, "physics.limit.range", location,
                   "limit bounds must not be NaN");
        }
        if (field == "low") {
          limit_low[dof] = std::make_pair(value, have_value);
        } else {
          limit_high[dof] = std::make_pair(value, have_value);
        }
      }
    }
  }

  // UsdPhysics: a low limit above the high limit locks the axis. That is the
  // standard spelling of a locked DOF (e.g. a D6 planar joint), not an error.
  (void)limit_high;
}

void ValidateMjcTokenEnums(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  static const std::set<std::string> kAutoBool = {"auto", "true", "false"};
  ValidateTokenSetProperty(ps, "mjc:option:integrator",
                           {"euler", "implicit", "implicitfast", "rk4", "RK4"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:option:cone", {"pyramidal", "elliptic"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:option:jacobian",
                           {"auto", "dense", "sparse"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:option:solver", {"pgs", "cg", "newton"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:compiler:angle", {"degree", "radian"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:compiler:inertiaFromGeom", kAutoBool,
                           "physics.extension.mjc.token", prim_location,
                           result);
  for (const char *prop_name :
       {"mjc:actuatorfrclimited", "mjc:ctrlLimited", "mjc:forceLimited",
        "mjc:actLimited", "mjc:limited"}) {
    ValidateTokenSetProperty(ps, prop_name, kAutoBool,
                             "physics.extension.mjc.token", prim_location,
                             result);
  }
  ValidateTokenSetProperty(ps, "mjc:dynType",
                           {"none", "integrator", "filter", "filterexact",
                            "muscle", "user"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:gainType",
                           {"fixed", "affine", "muscle", "user"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:biasType",
                           {"none", "affine", "muscle", "user"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  if (ps.type_name() == "MjcTendon") {
    ValidateTokenSetProperty(ps, "mjc:type", {"spatial", "fixed"},
                             "physics.extension.mjc.token", prim_location,
                             result);
  } else if (ps.type_name() == "MjcSensor") {
    static const std::set<std::string> kSensorObjectTypes = {
        "body", "xbody", "joint", "geom", "site", "camera", "light",
        "tendon", "actuator", "sensor", "numeric", "text", "tuple", "key",
        "plugin"};
    ValidateTokenSetProperty(
        ps, "mjc:type",
        {"touch", "accelerometer", "velocimeter", "gyro", "force", "torque",
         "magnetometer", "camprojection", "rangefinder", "jointpos",
         "jointvel", "tendonpos", "tendonvel", "actuatorpos", "actuatorvel",
         "actuatorfrc", "jointactuatorfrc", "ballquat", "ballangvel", "jointlimitpos",
         "jointlimitvel", "jointlimitfrc", "tendonlimitpos",
         "tendonlimitvel", "tendonlimitfrc", "framepos", "framequat",
         "framexaxis", "frameyaxis", "framezaxis", "framelinvel",
         "frameangvel", "framelinacc", "frameangacc", "subtreecom",
         "subtreelinvel", "subtreeangmom", "clock", "user"},
        "physics.extension.mjc.token", prim_location, result);
    ValidateTokenSetProperty(ps, "mjc:objtype", kSensorObjectTypes,
                             "physics.extension.mjc.token", prim_location,
                             result);
    ValidateTokenSetProperty(ps, "mjc:reftype", kSensorObjectTypes,
                             "physics.extension.mjc.token", prim_location,
                             result);
  }
  ValidateTokenSetProperty(ps, "mjc:inertia",
                           {"legacy", "convex", "exact", "shell"},
                           "physics.extension.mjc.token", prim_location,
                           result);
}

void ValidateMjcPhysics(const PrimSpec &ps,
                        const std::vector<AppliedSchema> &schemas,
                        const std::string &prim_location,
                        const PrimTypeByPath &prim_types,
                        USDValidationResult *result) {
  if (!HasSchemaOrPropertyPrefix(ps, schemas, "Mjc", "mjc:") &&
      !StartsWith(ps.type_name(), "Mjc")) {
    return;
  }

  ValidateMjcTokenEnums(ps, prim_location, result);

  for (const char *prop_name :
       {"mjc:option:timestep", "mjc:option:impratio"}) {
    ValidatePositiveNumericProperty(ps, prop_name,
                                    "physics.extension.mjc.range",
                                    prim_location, result);
  }

  for (const char *prop_name :
       {"mjc:option:density", "mjc:option:viscosity", "mjc:option:o_margin",
        "mjc:option:tolerance", "mjc:option:ls_tolerance", "mjc:option:noslip_tolerance",
        "mjc:option:ccd_tolerance", "mjc:torsionalfriction", "mjc:rollingfriction",
        "mjc:compiler:boundMass", "mjc:compiler:boundInertia",
        "mjc:stiffness", "mjc:damping", "mjc:armature", "mjc:frictionloss",
        "mjc:margin", "mjc:solmix", "mjc:gap", "mjc:crankLength",
        "mjc:inheritRange", "mjc:width"}) {
    ValidateNonNegativeNumericProperty(ps, prop_name,
                                       "physics.extension.mjc.range",
                                       prim_location, result);
  }

  for (const char *prop_name :
       {"mjc:group", "mjc:priority", "mjc:condim", "mjc:option:iterations",
        "mjc:option:ls_iterations", "mjc:option:noslip_iterations",
        "mjc:option:ccd_iterations", "mjc:option:sdf_iterations",
        "mjc:option:sdf_initpoints"}) {
    ValidateNonNegativeIntegerProperty(ps, prop_name,
                                       "physics.extension.mjc.range",
                                       prim_location, result);
  }
  ValidateNumericMinProperty(ps, "mjc:maxhullvert", -1.0,
                             "physics.extension.mjc.range", prim_location,
                             result);

  ValidateNumericMinMaxPair(ps, "mjc:ctrlRange:min", "mjc:ctrlRange:max",
                            "physics.extension.mjc.range", prim_location,
                            result);
  ValidateNumericMinMaxPair(ps, "mjc:forceRange:min", "mjc:forceRange:max",
                            "physics.extension.mjc.range", prim_location,
                            result);
  ValidateNumericMinMaxPair(ps, "mjc:actRange:min", "mjc:actRange:max",
                            "physics.extension.mjc.range", prim_location,
                            result);
  ValidateNumericMinMaxPair(ps, "mjc:lengthRange:min", "mjc:lengthRange:max",
                            "physics.extension.mjc.range", prim_location,
                            result);
  ValidateNumericMinMaxPair(ps, "mjc:range:min", "mjc:range:max",
                            "physics.extension.mjc.range", prim_location,
                            result);
  ValidateNumericMinMaxPair(ps, "mjc:actuatorfrcrange:min",
                            "mjc:actuatorfrcrange:max",
                            "physics.extension.mjc.range", prim_location,
                            result);

  for (const char *prop_name :
       {"mjc:option:o_solref", "mjc:solref", "mjc:solreflimit",
        "mjc:solreffriction"}) {
    ValidateNumericArrayLengthProperty(ps, prop_name, 2,
                                       "physics.extension.mjc.array",
                                       prim_location, result);
  }
  for (const char *prop_name :
       {"mjc:option:o_solimp", "mjc:solimp", "mjc:solimplimit",
        "mjc:solimpfriction"}) {
    ValidateNumericArrayLengthProperty(ps, prop_name, 5,
                                       "physics.extension.mjc.array",
                                       prim_location, result);
  }
  for (const char *prop_name :
       {"mjc:option:o_friction", "mjc:gear", "mjc:dynPrm", "mjc:gainPrm",
        "mjc:biasPrm", "mjc:path:divisors", "mjc:path:coef",
        "mjc:springdamper", "mjc:springlength", "mjc:qpos", "mjc:qvel",
        "mjc:act", "mjc:ctrl", "mjc:mpos", "mjc:mquat"}) {
    ValidateNumericArrayFiniteProperty(ps, prop_name,
                                       "physics.extension.mjc.array",
                                       prim_location, result);
  }
  ValidateIntArrayLengthMatchesRelationship(
      ps, "mjc:path:indices", "mjc:path", "physics.extension.mjc.array",
      prim_location, result);
  ValidateNumericArrayLengthMatchesRelationship(
      ps, "mjc:path:coef", "mjc:path", "physics.extension.mjc.array",
      prim_location, result);
  ValidateIntArrayLengthMatchesRelationship(
      ps, "mjc:sideSites:indices", "mjc:sideSites",
      "physics.extension.mjc.array", prim_location, result);

  ValidatePhysicsRelationship(ps, "mjc:target",
                              "physics.extension.mjc.relationship",
                              prim_location, prim_types,
                              {"PhysicsJoint", "PhysicsRevoluteJoint",
                               "PhysicsPrismaticJoint",
                               "PhysicsSphericalJoint", "PhysicsFixedJoint",
                               "PhysicsDistanceJoint"},
                              result);
  for (const char *prop_name :
       {"mjc:refSite", "mjc:sliderSite", "mjc:path", "mjc:sideSites"}) {
    ValidatePhysicsRelationship(ps, prop_name,
                                "physics.extension.mjc.relationship",
                                prim_location, prim_types, {}, result);
  }
}

// NewtonMassAPI newton:inertia = [Ixx, Iyy, Izz, Ixy, Ixz, Iyz] about the COM
// in body axes. Newton ignores a tensor that is not finite and positive
// semidefinite, and rebalances one that violates the triangle inequality.
void ValidateNewtonInertia(const PrimSpec &ps, const std::string &prim_location,
                           USDValidationResult *result) {
  std::vector<double> t;
  if (!GetDoubleArrayProperty(ps, "newton:inertia", &t)) return;
  const std::string location = MakePropertyLocation(prim_location, "newton:inertia");
  if (t.size() != 6) {
    AddError(result, "physics.extension.newton.inertia", location,
             "newton:inertia must have 6 elements [Ixx, Iyy, Izz, Ixy, Ixz, Iyz]");
    return;
  }
  for (double v : t) {
    if (!std::isfinite(v)) {
      AddError(result, "physics.extension.newton.inertia", location,
               "newton:inertia must be finite");
      return;
    }
  }
  // Eigenvalues of the symmetric tensor (closed form for 3x3).
  const double a = t[0], b = t[1], c = t[2], d = t[3], e = t[4], f = t[5];
  const double scale = std::max({std::abs(a), std::abs(b), std::abs(c), 1e-300});
  const double off = d * d + e * e + f * f;
  double eig[3];
  if (off <= 1e-30 * scale * scale) {
    eig[0] = a; eig[1] = b; eig[2] = c;
  } else {
    const double q = (a + b + c) / 3.0;
    const double p2 = (a - q) * (a - q) + (b - q) * (b - q) + (c - q) * (c - q) + 2.0 * off;
    const double p = std::sqrt(p2 / 6.0);
    // det((A - qI) / p) / 2, clamped into acos's domain.
    const double ba = (a - q) / p, bb = (b - q) / p, bc = (c - q) / p;
    const double bd = d / p, be = e / p, bf = f / p;
    double r = (ba * (bb * bc - bf * bf) - bd * (bd * bc - bf * be) +
                be * (bd * bf - bb * be)) / 2.0;
    r = std::min(1.0, std::max(-1.0, r));
    const double phi = std::acos(r) / 3.0;
    constexpr double kTwoPiOver3 = 2.0943951023931954923;
    eig[0] = q + 2.0 * p * std::cos(phi);
    eig[2] = q + 2.0 * p * std::cos(phi + kTwoPiOver3);
    eig[1] = 3.0 * q - eig[0] - eig[2];
  }
  std::sort(eig, eig + 3);
  const double tol = 1e-9 * std::max(eig[2], 1e-300);
  if (eig[0] < -tol) {
    AddError(result, "physics.extension.newton.inertia", location,
             "newton:inertia must be positive semidefinite");
    return;
  }
  if (eig[0] + eig[1] < eig[2] * (1.0 - 1e-6)) {
    AddWarning(result, "physics.extension.newton.inertia", location,
               "newton:inertia principal moments violate the triangle "
               "inequality; Newton rebalances the tensor");
  }
  // The authored principal moments must describe the same tensor.
  const Value *diag = GetAttrValue(ps, "physics:diagonalInertia");
  std::array<double, 3> principal{};
  if (diag && ValueToVec3(*diag, &principal) &&
      (principal[0] != 0.0 || principal[1] != 0.0 || principal[2] != 0.0)) {
    const double trace = a + b + c;
    const double other = principal[0] + principal[1] + principal[2];
    if (std::abs(trace - other) > 1e-4 * std::max(std::abs(trace), std::abs(other))) {
      AddWarning(result, "physics.extension.newton.inertia", location,
                 "newton:inertia and physics:diagonalInertia disagree; "
                 "Newton uses newton:inertia");
    }
  }
}

// Newton reads -inf as "use the engine default" for the joint limit gains.
void ValidateNewtonLimitGain(const PrimSpec &ps, const char *name,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  double v = 0.0;
  if (!GetNumericScalarProperty(ps, name, &v)) return;
  if (std::isnan(v) || (v < 0.0 && v != -std::numeric_limits<double>::infinity())) {
    AddError(result, "physics.extension.newton.range",
             MakePropertyLocation(prim_location, name),
             std::string(name) + " must be >= 0 or -inf (engine default)");
  }
}

// NewtonSDFCollisionAPI: Newton builds the collider SDF from these
// parameters; lightusd:sdf names LightUSD's precomputed level-set Volume.
void ValidateNewtonSdf(const PrimSpec &ps, const std::string &prim_location,
                       const PrimTypeByPath &prim_types, USDValidationResult *result) {
  double v = 0.0;
  if (GetNumericScalarProperty(ps, "newton:sdfMaxResolution", &v) && !(v > 0.0))
    AddError(result, "physics.extension.newton.range",
             MakePropertyLocation(prim_location, "newton:sdfMaxResolution"),
             "newton:sdfMaxResolution must be positive");
  if (GetNumericScalarProperty(ps, "newton:sdfNarrowBandInner", &v) &&
      !(v <= 0.0 && std::isfinite(v)))
    AddError(result, "physics.extension.newton.range",
             MakePropertyLocation(prim_location, "newton:sdfNarrowBandInner"),
             "newton:sdfNarrowBandInner must be finite and <= 0 (inside the surface)");
  if (GetNumericScalarProperty(ps, "newton:sdfNarrowBandOuter", &v) &&
      !(v >= 0.0 && std::isfinite(v)))
    AddError(result, "physics.extension.newton.range",
             MakePropertyLocation(prim_location, "newton:sdfNarrowBandOuter"),
             "newton:sdfNarrowBandOuter must be finite and >= 0 (outside the surface)");
  ValidatePhysicsRelationship(ps, "lightusd:sdf", "physics.extension.newton.relationship",
                              prim_location, prim_types, {"Volume"}, result);
}

void ValidateNewtonPhysics(const PrimSpec &ps,
                           const std::vector<AppliedSchema> &schemas,
                           const std::string &prim_location,
                           const PrimTypeByPath &prim_types,
                           USDValidationResult *result) {
  if (!HasSchemaOrPropertyPrefix(ps, schemas, "Newton", "newton:") &&
      ps.type_name() != "NewtonActuator") {
    return;
  }

  ValidateNumericMinProperty(ps, "newton:maxSolverIterations", -1.0,
                             "physics.extension.newton.range", prim_location,
                             result);
  ValidatePositiveNumericProperty(ps, "newton:timeStepsPerSecond",
                                  "physics.extension.newton.range",
                                  prim_location, result);
  ValidatePositiveNumericProperty(ps, "newton:delaySteps",
                                  "physics.extension.newton.range",
                                  prim_location, result);

  for (const char *prop_name :
       {"newton:xpbd:softBodyRelaxation", "newton:xpbd:softContactRelaxation",
        "newton:xpbd:jointLinearRelaxation",
        "newton:xpbd:jointAngularRelaxation",
        "newton:xpbd:rigidContactRelaxation"}) {
    ValidateNumericRangeProperty(ps, prop_name, 0.0, 1.0,
                                 "physics.extension.newton.range",
                                 prim_location, result);
  }
  for (const char *prop_name :
       {"newton:xpbd:jointLinearCompliance",
        "newton:xpbd:jointAngularCompliance", "newton:xpbd:angularDamping",
        "newton:kamino:padmm:primalTolerance",
        "newton:kamino:padmm:dualTolerance",
        "newton:kamino:padmm:complementarityTolerance",
        "newton:kamino:constraints:alpha", "newton:kamino:constraints:beta",
        "newton:kamino:constraints:gamma", "newton:contactMargin",
        "newton:torsionalFriction", "newton:rollingFriction", "newton:kp",
        "newton:kd", "newton:ki", "newton:maxEffort",
        "newton:maxMotorEffort", "newton:saturationEffort",
        "newton:velocityLimit", "newton:armature", "newton:friction",
        "newton:damping"}) {
    ValidateNonNegativeNumericProperty(ps, prop_name,
                                       "physics.extension.newton.range",
                                       prim_location, result);
  }
  ValidateNewtonInertia(ps, prim_location, result);
  ValidateNewtonLimitGain(ps, "newton:limitStiffness", prim_location, result);
  ValidateNewtonLimitGain(ps, "newton:limitDamping", prim_location, result);
  ValidateNewtonSdf(ps, prim_location, prim_types, result);
  ValidateTokenSetProperty(ps, "newton:kamino:padmm:warmstarting",
                           {"none", "containers", "reduced"},
                           "physics.extension.newton.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "newton:kamino:jointCorrection",
                           {"twopi", "quaternion"},
                           "physics.extension.newton.token", prim_location,
                           result);
  ValidateAssetPathProperty(ps, "newton:modelPath",
                            "physics.extension.newton.asset", prim_location,
                            result);
  ValidatePhysicsRelationship(ps, "newton:mimicJoint",
                              "physics.extension.newton.relationship",
                              prim_location, prim_types,
                              {"PhysicsJoint", "PhysicsRevoluteJoint",
                               "PhysicsPrismaticJoint",
                               "PhysicsSphericalJoint", "PhysicsFixedJoint",
                               "PhysicsDistanceJoint"},
                              result);
  ValidatePhysicsRelationship(ps, "newton:targets",
                              "physics.extension.newton.relationship",
                              prim_location, prim_types,
                              {"PhysicsJoint", "PhysicsRevoluteJoint",
                               "PhysicsPrismaticJoint",
                               "PhysicsSphericalJoint", "PhysicsFixedJoint",
                               "PhysicsDistanceJoint"},
                              result);

  std::vector<double> lookup_positions;
  std::vector<double> lookup_efforts;
  const bool have_positions =
      GetDoubleArrayProperty(ps, "newton:lookupPositions", &lookup_positions);
  const bool have_efforts =
      GetDoubleArrayProperty(ps, "newton:lookupEfforts", &lookup_efforts);
  if (have_positions || have_efforts) {
    if (!have_positions || !have_efforts ||
        lookup_positions.size() != lookup_efforts.size()) {
      AddError(result, "physics.extension.newton.lookup", prim_location,
               "newton:lookupPositions and newton:lookupEfforts must both be "
               "authored with matching lengths");
    }
    for (size_t i = 1; i < lookup_positions.size(); i++) {
      if (lookup_positions[i] <= lookup_positions[i - 1]) {
        AddError(result, "physics.extension.newton.lookup",
                 MakePropertyLocation(prim_location,
                                      "newton:lookupPositions"),
                 "newton:lookupPositions must be strictly increasing");
        break;
      }
    }
    ValidateNumericArrayFiniteProperty(ps, "newton:lookupPositions",
                                       "physics.extension.newton.lookup",
                                       prim_location, result);
    ValidateNumericArrayFiniteProperty(ps, "newton:lookupEfforts",
                                       "physics.extension.newton.lookup",
                                       prim_location, result);
  }
}

void ValidatePreliminaryPhysics(const PrimSpec &ps,
                                const std::vector<AppliedSchema> &schemas,
                                const std::string &prim_location,
                                const PrimTypeByPath &prim_types,
                                USDValidationResult *result) {
  bool has_preliminary = false;
  for (const AppliedSchema &schema : schemas) {
    if (StartsWith(schema.name, "Preliminary_Physics")) {
      has_preliminary = true;
    }
  }
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    if (StartsWith(prop_name, "preliminary:physics:") ||
        StartsWith(prop_name, "physics:gravitationalForce:") ||
        prop_name == "normal") {
      has_preliminary = true;
    }
  }
  if (!has_preliminary && !StartsWith(ps.type_name(), "Preliminary_Physics") &&
      ps.type_name() != "Preliminary_InfiniteColliderPlane") {
    return;
  }

  ValidateNonNegativeNumericProperty(
      ps, "preliminary:physics:material:restitution",
      "physics.preliminary.range", prim_location, result);
  ValidateNonNegativeNumericProperty(
      ps, "preliminary:physics:material:friction:static",
      "physics.preliminary.range", prim_location, result);
  ValidateNonNegativeNumericProperty(
      ps, "preliminary:physics:material:friction:dynamic",
      "physics.preliminary.range", prim_location, result);
  ValidatePositiveNumericProperty(ps, "preliminary:physics:rigidBody:mass",
                                  "physics.preliminary.range", prim_location,
                                  result);
  ValidateFiniteVec3Property(ps, "physics:gravitationalForce:acceleration",
                             "physics.preliminary.acceleration",
                             prim_location, result, false);
  ValidateFiniteVec3Property(ps, "normal", "physics.preliminary.normal",
                             prim_location, result, true);
  ValidatePhysicsRelationship(ps, "preliminary:physics:collider:convexShape",
                              "physics.preliminary.relationship",
                              prim_location, prim_types, {}, result);
}

void ValidatePhysics(const PrimSpec &ps,
                     const std::vector<AppliedSchema> &applied_schemas,
                     const std::string &prim_location,
                     const PrimTypeByPath &prim_types,
                     const AncestorContext &ancestors,
                     USDValidationResult *result) {
  const std::string &type_name = ps.type_name();
  if (!IsPhysicsPrimTypeName(type_name) &&
      !HasAppliedPhysicsSchema(applied_schemas) &&
      !HasSchemaOrPropertyPrefix(ps, applied_schemas, "Mjc", "mjc:") &&
      !HasSchemaOrPropertyPrefix(ps, applied_schemas, "Newton", "newton:") &&
      !HasSchemaOrPropertyPrefix(ps, applied_schemas, "Preliminary_Physics",
                                 "preliminary:physics:")) {
    return;
  }

  if (type_name == "PhysicsScene") {
    ValidatePhysicsScene(ps, prim_location, result);
  }

  const bool rigid_body =
      HasAppliedSchema(applied_schemas, "PhysicsRigidBodyAPI");
  const bool collision =
      HasAppliedSchema(applied_schemas, "PhysicsCollisionAPI");
  const bool articulation =
      HasAppliedSchema(applied_schemas, "PhysicsArticulationRootAPI");

  if (rigid_body && !IsXformableTypeName(type_name)) {
    AddError(result, "physics.rigidBody.xformable", prim_location,
             "PhysicsRigidBodyAPI must be applied to an Xformable prim");
  }
  if (articulation && ancestors.has_articulation_ancestor) {
    AddError(result, "physics.articulation.nested", prim_location,
             "nested PhysicsArticulationRootAPI prims are not supported");
  }
  if (articulation && rigid_body) {
    const Value *enabled = GetAttrValue(ps, "physics:rigidBodyEnabled");
    if (enabled && enabled->as_bool() && !*enabled->as_bool()) {
      AddError(result, "physics.articulation.staticBody", prim_location,
               "ArticulationRootAPI cannot be applied to a disabled/static "
               "rigid body");
    }
  }
  if (collision && type_name == "Points") {
    size_t point_count = 0;
    std::vector<float> widths;
    const bool have_points = GetPoint3ArrayLen(ps, "points", &point_count);
    const bool have_widths = GetFloatArrayProperty(ps, "widths", &widths);
    if (!have_points || !have_widths || point_count == 0 || widths.empty() ||
        point_count != widths.size()) {
      AddError(result, "physics.collider.points", prim_location,
               "Points collider requires non-empty points and widths arrays "
               "of equal length");
    }
  }

  ValidatePhysicsScalarProperties(ps, prim_location, result);
  ValidatePhysicsInertiaAndMotion(ps, prim_location, result);
  ValidatePhysicsApproximation(ps, prim_location, result);
  ValidatePhysicsSchemaPlacement(ps, applied_schemas, prim_location, result);

  if (type_name.find("Joint") != std::string::npos) {
    ValidatePhysicsRelationship(ps, "physics:body0", "physics.joint.body",
                                prim_location, prim_types, {}, result);
    ValidatePhysicsRelationship(ps, "physics:body1", "physics.joint.body",
                                prim_location, prim_types, {}, result);
    ValidatePhysicsJointLimits(ps, prim_location, result);
    ValidatePhysicsJointTransforms(ps, prim_location, result);
  }

  ValidatePhysicsRelationship(ps, "physics:simulationOwner",
                              "physics.relationship.target", prim_location,
                              prim_types, {"PhysicsScene"}, result);
  ValidatePhysicsRelationship(ps, "physics:filteredPairs",
                              "physics.relationship.target", prim_location,
                              prim_types, {}, result);
  if (type_name == "PhysicsCollisionGroup") {
    ValidatePhysicsCollisionGroup(ps, prim_location, prim_types, result);
  } else {
    ValidatePhysicsRelationship(ps, "physics:filteredGroups",
                                "physics.relationship.target", prim_location,
                                prim_types, {"PhysicsCollisionGroup"},
                                result);
  }

  ValidatePhysicsDriveAndLimitAPIs(ps, applied_schemas, prim_location, result);
  ValidateMjcPhysics(ps, applied_schemas, prim_location, prim_types, result);
  ValidateNewtonPhysics(ps, applied_schemas, prim_location, prim_types,
                        result);
  ValidatePreliminaryPhysics(ps, applied_schemas, prim_location, prim_types,
                             result);
}

}  // namespace validation_detail
}  // namespace next
}  // namespace lightusd
