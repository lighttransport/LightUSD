// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#include "schema-registry.hh"

#include <algorithm>
#include <limits>

namespace lightusd {
namespace next {

namespace {

Value Token(const char* value) { return Value::MakeToken(value); }

bool HasAppliedSchema(const PrimSpec& prim, const std::string& schema) {
  for (const std::string& applied : prim.meta().apiSchemas()) {
    if (applied == schema ||
        (applied.size() > schema.size() &&
         applied.compare(0, schema.size(), schema) == 0 &&
         applied[schema.size()] == ':')) {
      return true;
    }
  }
  // OpenUSD auto-applies these API schemas to the concrete 3D Gaussian type.
  // They need not appear in authored apiSchemas metadata.
  if (prim.type_name() == "ParticleField3DGaussianSplat") {
    static const char* kGaussianSchemas[] = {
        "ParticleFieldPositionAttributeAPI",
        "ParticleFieldOrientationAttributeAPI",
        "ParticleFieldScaleAttributeAPI",
        "ParticleFieldOpacityAttributeAPI",
        "ParticleFieldKernelGaussianEllipsoidAPI",
        "ParticleFieldSphericalHarmonicsAttributeAPI"};
    for (const char* auto_applied : kGaussianSchemas) {
      if (schema == auto_applied) return true;
    }
  }
  return false;
}

bool MatchDefinitionName(const PrimSpec& prim,
                         const SchemaPropertyDefinition& def,
                         const std::string& property_name) {
  static const std::string marker = "__INSTANCE__";
  const size_t marker_pos = def.instance_marker_pos;
  if (marker_pos == std::string::npos) return def.name == property_name;
  for (const std::string& applied : prim.meta().apiSchemas()) {
    const std::string prefix = def.schema_type + ":";
    if (applied.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string instance = applied.substr(prefix.size());
    if (instance.empty()) continue;
    std::string instantiated = def.name;
    instantiated.replace(marker_pos, marker.size(), instance);
    if (instantiated == property_name) return true;
  }
  return false;
}

bool IsAPISchemaTypeName(const std::string& name) {
  return name.size() > 3 && name.compare(name.size() - 3, 3, "API") == 0;
}

// Rank of the applied schema `schema` within the prim's apiSchemas (list-op
// resolved order), or -1. Instance-qualified entries match their family.
// The Gaussian auto-applies rank after every authored entry.
int AppliedSchemaRank(const PrimSpec& prim, const std::string& schema) {
  int rank = 0;
  for (const std::string& applied : prim.meta().apiSchemas()) {
    if (applied == schema ||
        (applied.size() > schema.size() &&
         applied.compare(0, schema.size(), schema) == 0 &&
         applied[schema.size()] == ':')) {
      return rank;
    }
    ++rank;
  }
  if (HasAppliedSchema(prim, schema)) return rank;
  return -1;
}

}  // namespace

bool SchemaRegistry::IsAbstractTypedSchemaName(const std::string& name) {
  // OpenUSD v26.11: every typed schema name in this registry that has no
  // concrete prim definition (Usd.SchemaRegistry.FindConcretePrimDefinition).
  static const char* kAbstract[] = {
      "Typed", "APISchemaBase", "Imageable", "Xformable", "Boundable",
      "Gprim", "PointBased", "Curves", "BoundableLightBase",
      "NonboundableLightBase", "VolumeFieldBase", "FieldBase",
      "VolumeFieldAsset", "FieldAsset", "RenderSettingsBase"};
  for (const char* a : kAbstract) {
    if (name == a) return true;
  }
  return false;
}

bool SchemaRegistry::IsConcreteTypedSchemaName(const std::string& name) const {
  if (name.empty() || IsAbstractTypedSchemaName(name) ||
      IsAPISchemaTypeName(name)) {
    return false;
  }
  return IsKnownSchema(name);
}

SchemaRegistry::SchemaRegistry() {
#define LIGHTUSD_NEXT_SUPPORTED_SCHEMA(name) known_schemas_.push_back(name);
#include "generated/openusd-supported-schema-names.inc"
#undef LIGHTUSD_NEXT_SUPPORTED_SCHEMA
  parents_ = {
      {"Xform", "Xformable"}, {"Xformable", "Imageable"},
      {"Curves", "PointBased"}, {"BasisCurves", "Curves"},
      {"NurbsCurves", "Curves"}, {"Plane", "Gprim"},
      {"Scope", "Imageable"},
      {"Boundable", "Xformable"}, {"Gprim", "Boundable"},
      {"Mesh", "Gprim"}, {"Points", "PointBased"},
      // pxr: PointBased inherits Gprim (NOT Boundable directly), so curves and
      // points must pick up Gprim's doubleSided/orientation fallbacks.
      {"PointBased", "Gprim"}, {"BasisCurves", "Curves"},
      {"NurbsCurves", "Curves"}, {"Curves", "PointBased"},
      {"Sphere", "Gprim"}, {"Cube", "Gprim"},
      {"Cylinder", "Gprim"}, {"Cone", "Gprim"},
      {"Capsule", "Gprim"}, {"Plane", "Gprim"},
      {"Camera", "Xformable"}, {"PointInstancer", "Boundable"},
      {"PhysicsJoint", "Imageable"},
      {"PhysicsFixedJoint", "PhysicsJoint"},
      {"PhysicsRevoluteJoint", "PhysicsJoint"},
      {"PhysicsPrismaticJoint", "PhysicsJoint"},
      {"PhysicsSphericalJoint", "PhysicsJoint"},
      {"PhysicsDistanceJoint", "PhysicsJoint"},
      {"GeomSubset", "Imageable"}, {"DistantLight", "Xformable"},
      {"DomeLight", "Xformable"}, {"RectLight", "Xformable"},
      {"DiskLight", "Xformable"}, {"SphereLight", "Xformable"},
      {"CylinderLight", "Xformable"},
      // Non-core OpenUSD domains (PRODUCT PARITY, not AOUSD Core):
      // UsdGeom breadth (Hermite/TetMesh/NURBS patch) + UsdVol + UsdRender.
      {"HermiteCurves", "Curves"}, {"TetMesh", "PointBased"},
      {"NurbsPatch", "PointBased"},
      {"Volume", "Gprim"},
      // pxr 25.x renamed FieldBase/FieldAsset to VolumeFieldBase/
      // VolumeFieldAsset and kept the old names as deprecated aliases; both
      // spellings resolve through the same chain here.
      {"VolumeFieldBase", "Xformable"}, {"FieldBase", "VolumeFieldBase"},
      {"VolumeFieldAsset", "FieldBase"}, {"FieldAsset", "VolumeFieldAsset"},
      {"Field3DAsset", "FieldAsset"}, {"OpenVDBAsset", "FieldAsset"},
      {"ParticleField", "Gprim"},
      {"ParticleField3DGaussianSplat", "ParticleField"},
      // UsdRender types inherit Typed in pxr (NOT Imageable/Xformable): no
      // visibility/purpose fallbacks. Typed is a terminal marker with no
      // properties; recording it makes the ancestry KNOWN so consumers can
      // derive non-Xformable/non-Gprim placement from the registry.
      {"RenderSettingsBase", "Typed"},
      {"RenderSettings", "RenderSettingsBase"},
      {"RenderProduct", "RenderSettingsBase"},
      {"RenderVar", "Typed"},
      {"RenderPass", "Typed"},
      {"ClipsAPI", "Typed"},
      {"CollectionAPI", "Typed"},
      {"MaterialBindingAPI", "Typed"},
      {"LightListAPI", "Typed"},
      {"LightAPI", "Typed"},
      {"GenerativeProcedural", "Boundable"},
      {"Backdrop", "Typed"},
  };

  auto add = [&](const char* schema, const char* name, const char* type,
                 Value fallback) {
    SchemaPropertyDefinition d;
    d.schema_type = schema;
    d.name = name;
    d.type_name = type;
    d.fallback = std::move(fallback);
    d.has_fallback = true;
    d.instance_marker_pos = d.name.find("__INSTANCE__");
    properties_.push_back(std::move(d));
  };
  auto declare = [&](const char* schema, const char* name, const char* type) {
    SchemaPropertyDefinition d;
    d.schema_type = schema;
    d.name = name;
    d.type_name = type;
    d.instance_marker_pos = d.name.find("__INSTANCE__");
    properties_.push_back(std::move(d));
  };

#define AOUSD_SCHEMA_FALLBACK(schema, name, type, fallback) \
  add(schema, name, type, fallback);
#define AOUSD_SCHEMA_DECLARE(schema, name, type) declare(schema, name, type);
#include "generated/aousd-core-schema-definitions.inc"
#undef AOUSD_SCHEMA_DECLARE
#undef AOUSD_SCHEMA_FALLBACK

  // Current OpenUSD spells ColorSpaceDefinitionAPI as a multiple-apply API.
  // Keep the generated AOUSD 1.0.1 unqualified declarations above for read
  // compatibility and add the standard instantiated property namespace.
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:name",
      "token", Token("custom"));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:redChroma",
      "float2", Value::MakeFloat2(1.0f, 0.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:greenChroma",
      "float2", Value::MakeFloat2(0.0f, 1.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:blueChroma",
      "float2", Value::MakeFloat2(0.0f, 0.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:whitePoint",
      "float2", Value::MakeFloat2(1.0f / 3.0f, 1.0f / 3.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:gamma",
      "float", Value(1.0f));
  add("ColorSpaceDefinitionAPI", "colorSpaceDefinition:__INSTANCE__:linearBias",
      "float", Value(0.0f));
  add("Imageable", "visibility", "token", Token("inherited"));
  add("Imageable", "purpose", "token", Token("default"));
  add("Xformable", "xformOpOrder", "token[]",
      Value::MakeTokenArray(std::vector<std::string>()));
  add("Gprim", "doubleSided", "bool", Value(false));
  add("Gprim", "orientation", "token", Token("rightHanded"));
  add("Mesh", "subdivisionScheme", "token", Token("catmullClark"));
  add("Mesh", "interpolateBoundary", "token", Token("edgeAndCorner"));
  add("Mesh", "faceVaryingLinearInterpolation", "token",
      Token("cornersPlus1"));
  add("Mesh", "triangleSubdivisionRule", "token", Token("catmullClark"));
  add("Sphere", "radius", "double", Value(1.0));
  add("Cube", "size", "double", Value(2.0));
  add("Cylinder", "radius", "double", Value(1.0));
  add("Cylinder", "height", "double", Value(2.0));
  add("Cylinder", "axis", "token", Token("Z"));
  add("Cone", "radius", "double", Value(1.0));
  add("Cone", "height", "double", Value(2.0));
  add("Cone", "axis", "token", Token("Z"));
  add("Capsule", "radius", "double", Value(0.5));
  add("Capsule", "height", "double", Value(1.0));
  add("Capsule", "axis", "token", Token("Z"));
  add("Camera", "projection", "token", Token("perspective"));
  add("Camera", "horizontalAperture", "float", Value(20.955f));
  add("Camera", "verticalAperture", "float", Value(15.2908f));
  add("Camera", "focalLength", "float", Value(50.0f));
  add("Camera", "focusDistance", "float", Value(5.0f));
  add("Camera", "fStop", "float", Value(0.0f));
  add("PhysicsScene", "physics:gravityDirection", "vector3f",
      Value::MakeVector3f(0.0f, 0.0f, 0.0f));
  add("PhysicsScene", "physics:gravityMagnitude", "float",
      Value(-std::numeric_limits<float>::infinity()));

  add("PhysicsRigidBodyAPI", "physics:rigidBodyEnabled", "bool", Value(true));
  add("PhysicsRigidBodyAPI", "physics:kinematicEnabled", "bool", Value(false));
  add("PhysicsRigidBodyAPI", "physics:startsAsleep", "bool", Value(false));
  add("PhysicsRigidBodyAPI", "physics:velocity", "vector3f",
      Value::MakeVector3f(0.0f, 0.0f, 0.0f));
  add("PhysicsRigidBodyAPI", "physics:angularVelocity", "vector3f",
      Value::MakeVector3f(0.0f, 0.0f, 0.0f));
  add("PhysicsCollisionAPI", "physics:collisionEnabled", "bool", Value(true));
  add("PhysicsMassAPI", "physics:mass", "float", Value(0.0f));
  add("PhysicsMassAPI", "physics:density", "float", Value(0.0f));
  const float infinity = std::numeric_limits<float>::infinity();
  // UsdPhysics schema sentinel values request automatic computation. In
  // particular, mass=0 and principalAxes=(0,0,0,0) are not physical zero-mass
  // or normalized-rotation assertions.
  add("PhysicsMassAPI", "physics:centerOfMass", "point3f",
      Value::MakePoint3f(-infinity, -infinity, -infinity));
  add("PhysicsMassAPI", "physics:diagonalInertia", "float3",
      Value::MakeFloat3(0, 0, 0));
  add("PhysicsMassAPI", "physics:principalAxes", "quatf", Value::MakeQuatf(0, 0, 0, 0));
  add("PhysicsMaterialAPI", "physics:density", "float", Value(0.0f));
  add("PhysicsMeshCollisionAPI", "physics:approximation", "token", Token("none"));
  declare("PhysicsCollisionGroup", "physics:invertFilteredGroups", "bool");
  declare("PhysicsCollisionGroup", "physics:mergeGroup", "string");
  declare("PhysicsCollisionGroup", "physics:filteredGroups", "relationship");
  // CollisionGroup includes CollectionAPI:colliders without an authored API
  // list. AOUSD Core 1.0.1 / 1.1 define includeRoot's fallback as false;
  // OpenUSD 26.11's generated schema currently leaves it unset.
  add("PhysicsCollisionGroup", "collection:colliders:expansionRule", "token",
      Token("expandPrims"));
  add("PhysicsCollisionGroup", "collection:colliders:includeRoot", "bool",
      Value(false));
  declare("PhysicsCollisionGroup", "collection:colliders:includes", "relationship");
  declare("PhysicsCollisionGroup", "collection:colliders:excludes", "relationship");

  add("PhysicsJoint", "physics:jointEnabled", "bool", Value(true));
  add("PhysicsJoint", "physics:collisionEnabled", "bool", Value(false));
  add("PhysicsJoint", "physics:excludeFromArticulation", "bool", Value(false));
  add("PhysicsJoint", "physics:breakForce", "float", Value(infinity));
  add("PhysicsJoint", "physics:breakTorque", "float", Value(infinity));
  for (const char* name : {"physics:localPos0", "physics:localPos1"})
    add("PhysicsJoint", name, "point3f", Value::MakePoint3f(0, 0, 0));
  for (const char* name : {"physics:localRot0", "physics:localRot1"})
    add("PhysicsJoint", name, "quatf", Value::MakeQuatf(1, 0, 0, 0));
  for (const char* schema : {"PhysicsRevoluteJoint", "PhysicsPrismaticJoint"}) {
    add(schema, "physics:axis", "token", Token("X"));
    add(schema, "physics:lowerLimit", "float", Value(-infinity));
    add(schema, "physics:upperLimit", "float", Value(infinity));
  }
  add("PhysicsSphericalJoint", "physics:axis", "token", Token("X"));
  add("PhysicsSphericalJoint", "physics:coneAngle0Limit", "float", Value(-1.0f));
  add("PhysicsSphericalJoint", "physics:coneAngle1Limit", "float", Value(-1.0f));
  add("PhysicsDistanceJoint", "physics:minDistance", "float", Value(-1.0f));
  add("PhysicsDistanceJoint", "physics:maxDistance", "float", Value(-1.0f));
  add("PhysicsDriveAPI", "drive:__INSTANCE__:physics:type", "token", Token("force"));
  add("PhysicsDriveAPI", "drive:__INSTANCE__:physics:maxForce", "float", Value(infinity));
  for (const char* name : {"drive:__INSTANCE__:physics:targetPosition",
       "drive:__INSTANCE__:physics:targetVelocity", "drive:__INSTANCE__:physics:damping",
       "drive:__INSTANCE__:physics:stiffness"})
    add("PhysicsDriveAPI", name, "float", Value(0.0f));
  add("PhysicsLimitAPI", "limit:__INSTANCE__:physics:low", "float", Value(-infinity));
  add("PhysicsLimitAPI", "limit:__INSTANCE__:physics:high", "float", Value(infinity));

  // UsdGeom Imageable/Camera/curves/subset fallbacks (values verified
  // against pxr's generated usdGeom schema.usda).
  add("Imageable", "visibility", "token", Token("inherited"));
  add("Imageable", "purpose", "token", Token("default"));
  add("Camera", "clippingRange", "float2", Value::MakeFloat2(1.0f, 1000000.0f));
  add("Camera", "horizontalApertureOffset", "float", Value(0.0f));
  add("Camera", "verticalApertureOffset", "float", Value(0.0f));
  add("Camera", "shutter:open", "double", Value(0.0));
  add("Camera", "shutter:close", "double", Value(0.0));
  add("Camera", "stereoRole", "token", Token("mono"));
  add("BasisCurves", "type", "token", Token("cubic"));
  add("BasisCurves", "basis", "token", Token("bezier"));
  add("BasisCurves", "wrap", "token", Token("nonperiodic"));
  add("Plane", "width", "double", Value(2.0));
  add("Plane", "length", "double", Value(2.0));
  add("Plane", "axis", "token", Token("Z"));
  add("GeomSubset", "elementType", "token", Token("face"));
  add("GeomSubset", "familyName", "token", Token(""));

  // UsdLux common light inputs (each concrete light carries them; values
  // from pxr's usdLux schema.usda). DistantLight overrides intensity.
  {
    const char* kLights[] = {"DistantLight", "DomeLight",  "RectLight",
                             "DiskLight",    "SphereLight", "CylinderLight"};
    for (const char* lt : kLights) {
      add(lt, "inputs:intensity", "float",
          Value(std::string(lt) == "DistantLight" ? 50000.0f : 1.0f));
      add(lt, "inputs:exposure", "float", Value(0.0f));
      add(lt, "inputs:diffuse", "float", Value(1.0f));
      add(lt, "inputs:specular", "float", Value(1.0f));
      add(lt, "inputs:normalize", "bool", Value(false));
      add(lt, "inputs:color", "color3f", Value::MakeColor3f(1.0f, 1.0f, 1.0f));
      add(lt, "inputs:enableColorTemperature", "bool", Value(false));
      add(lt, "inputs:colorTemperature", "float", Value(6500.0f));
    }
  }
  add("DistantLight", "inputs:angle", "float", Value(0.53f));
  add("DomeLight", "inputs:texture:format", "token", Token("automatic"));
  add("SphereLight", "inputs:radius", "float", Value(0.5f));
  add("SphereLight", "treatAsPoint", "bool", Value(false));
  add("RectLight", "inputs:width", "float", Value(1.0f));
  add("RectLight", "inputs:height", "float", Value(1.0f));
  add("DiskLight", "inputs:radius", "float", Value(0.5f));
  add("CylinderLight", "inputs:length", "float", Value(1.0f));
  add("CylinderLight", "inputs:radius", "float", Value(0.5f));
  add("CylinderLight", "treatAsLine", "bool", Value(false));

  // UsdLux applied APIs.
  add("ShapingAPI", "inputs:shaping:cone:angle", "float", Value(90.0f));
  add("ShapingAPI", "inputs:shaping:cone:softness", "float", Value(0.0f));
  add("ShapingAPI", "inputs:shaping:focus", "float", Value(0.0f));
  add("ShadowAPI", "inputs:shadow:enable", "bool", Value(true));
  add("ShadowAPI", "inputs:shadow:color", "color3f",
      Value::MakeColor3f(0.0f, 0.0f, 0.0f));
  add("ShadowAPI", "inputs:shadow:distance", "float", Value(-1.0f));
  add("ShadowAPI", "inputs:shadow:falloff", "float", Value(-1.0f));
  add("ShadowAPI", "inputs:shadow:falloffGamma", "float", Value(1.0f));

  // Non-core OpenUSD domain fallbacks (PRODUCT PARITY; values verified
  // against pxr's generated usdGeom/usdVol/usdRender schema.usda).
  add("NurbsPatch", "uForm", "token", Token("open"));
  add("NurbsPatch", "vForm", "token", Token("open"));
  add("NurbsCurves", "order", "int[]",
      Value::MakeIntArray(std::vector<int32_t>()));
  // pxr: `vector3f[] tangents = []` (an authored empty-array fallback).
  add("HermiteCurves", "tangents", "vector3f[]",
      Value::MakeFloatCompArray(std::vector<float>(), TypeId::Vector3f, 3));
  add("VolumeFieldAsset", "vectorDataRoleHint", "token", Token("None"));
  add("RenderSettingsBase", "resolution", "int2", Value::MakeInt2(2048, 1080));
  add("RenderSettingsBase", "pixelAspectRatio", "float", Value(1.0f));
  add("RenderSettingsBase", "aspectRatioConformPolicy", "token",
      Token("expandAperture"));
  add("RenderSettingsBase", "dataWindowNDC", "float4",
      Value::MakeFloat4(0.0f, 0.0f, 1.0f, 1.0f));
  add("RenderSettingsBase", "instantaneousShutter", "bool", Value(false));
  add("RenderSettingsBase", "disableMotionBlur", "bool", Value(false));
  add("RenderSettingsBase", "disableDepthOfField", "bool", Value(false));
  add("RenderSettings", "includedPurposes", "token[]",
      Value::MakeTokenArray({"default", "render"}));
  add("RenderSettings", "materialBindingPurposes", "token[]",
      Value::MakeTokenArray({"full", ""}));
  add("RenderVar", "dataType", "token", Token("color3f"));
  add("RenderVar", "sourceName", "string", Value(std::string("")));
  add("RenderVar", "sourceType", "token", Token("raw"));
  add("RenderProduct", "productType", "token", Token("raster"));
  add("RenderProduct", "productName", "token", Token(""));
  add("RenderPass", "collection:renderVisibility:includeRoot", "bool",
      Value(true));
  add("RenderPass", "collection:cameraVisibility:includeRoot", "bool",
      Value(true));

  // OpenUSD 26.08 UsdVol ParticleField definitions. The concrete Gaussian
  // type auto-applies the six data/kernel APIs handled by HasAppliedSchema().
  add("ParticleField3DGaussianSplat", "projectionModeHint", "token",
      Token("perspective"));
  add("ParticleField3DGaussianSplat", "sortingModeHint", "token",
      Token("zDepth"));
  add("ParticleFieldSphericalHarmonicsAttributeAPI",
      "radiance:sphericalHarmonicsDegree", "int", Value(int32_t(3)));

  // OpenUSD 26.08 additions. Multiple-apply names use the registry's
  // __INSTANCE__ marker and are expanded from authored apiSchemas metadata.
  add("GeomModelAPI", "model:cardVisibility", "token", Token("inherited"));
  add("BackPlateAPI", "backPlate:__INSTANCE__:image", "asset",
      Value::MakeAssetPath(""));
  add("BackPlateAPI", "backPlate:__INSTANCE__:alpha:image", "asset",
      Value::MakeAssetPath(""));
  add("BackPlateAPI", "backPlate:__INSTANCE__:depth:image", "asset",
      Value::MakeAssetPath(""));
  add("BackPlateAPI", "backPlate:__INSTANCE__:depth:minOffset", "float",
      Value(0.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:depth:normalizingFactor",
      "float", Value(1.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:depth:cameraSpaceOffset",
      "float", Value(0.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:scale:tweak", "float2",
      Value::MakeFloat2(1.0f, 1.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:rotateXYZ:tweak", "float3",
      Value::MakeFloat3(0.0f, 0.0f, 0.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:translate:tweak", "float3",
      Value::MakeFloat3(0.0f, 0.0f, 0.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:luma:gain", "float3",
      Value::MakeFloat3(1.0f, 1.0f, 1.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:luma:lift", "float3",
      Value::MakeFloat3(0.0f, 0.0f, 0.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:luma:gamma", "float3",
      Value::MakeFloat3(1.0f, 1.0f, 1.0f));
  add("BackPlateAPI", "backPlate:__INSTANCE__:plateVisibility", "token",
      Token("solo"));
  add("SemanticsLabelsAPI", "semantics:labels:__INSTANCE__", "token[]",
      Value::MakeTokenArray({}));

  // Data-driven declarations for the remaining schemas implemented by next.
  // Declarations populate HasProperty()/property-name queries without
  // inventing a fallback where the schema intentionally has none.
  const struct Decl { const char* schema; const char* name; const char* type; }
      declarations[] = {
          {"Boundable", "extent", "float3[]"},
          {"PointBased", "points", "point3f[]"},
          {"PointBased", "velocities", "vector3f[]"},
          {"PointBased", "accelerations", "vector3f[]"},
          {"PointBased", "normals", "normal3f[]"},
          {"PointBased", "widths", "float[]"},
          {"Mesh", "faceVertexCounts", "int[]"},
          {"Mesh", "faceVertexIndices", "int[]"},
          {"PointInstancer", "protoIndices", "int[]"},
          {"PointInstancer", "positions", "point3f[]"},
          {"PointInstancer", "orientations", "quath[]"},
          {"PointInstancer", "scales", "float3[]"},
          {"PointInstancer", "ids", "int64[]"},
          {"PointInstancer", "invisibleIds", "int64[]"},
          {"PointInstancer", "prototypes", "relationship"},
          {"Shader", "info:id", "token"},
          {"NodeGraph", "outputs", "namespace"},
          {"Material", "outputs:surface", "token"},
          {"Material", "outputs:displacement", "token"},
          {"Material", "outputs:volume", "token"},
          {"Skeleton", "joints", "token[]"},
          {"Skeleton", "bindTransforms", "matrix4d[]"},
          {"Skeleton", "restTransforms", "matrix4d[]"},
          {"SkelAnimation", "joints", "token[]"},
          {"SkelAnimation", "translations", "float3[]"},
          {"SkelAnimation", "rotations", "quath[]"},
          {"SkelAnimation", "scales", "half3[]"},
          {"BlendShape", "offsets", "vector3f[]"},
          {"BlendShape", "normalOffsets", "vector3f[]"},
          {"BlendShape", "pointIndices", "int[]"},
          {"SkelBindingAPI", "skel:joints", "token[]"},
          {"SkelBindingAPI", "skel:jointIndices", "int[]"},
          {"SkelBindingAPI", "skel:jointWeights", "float[]"},
          {"SkelBindingAPI", "skel:skeleton", "relationship"},
          {"SkelBindingAPI", "skel:animationSource", "relationship"},
          // Non-core OpenUSD domains (PRODUCT PARITY): UsdGeom breadth.
          {"TetMesh", "tetVertexIndices", "int4[]"},
          {"TetMesh", "surfaceFaceVertexIndices", "int3[]"},
          {"NurbsPatch", "uVertexCount", "int"},
          {"NurbsPatch", "vVertexCount", "int"},
          {"NurbsPatch", "uOrder", "int"},
          {"NurbsPatch", "vOrder", "int"},
          {"NurbsPatch", "uKnots", "double[]"},
          {"NurbsPatch", "vKnots", "double[]"},
          {"NurbsPatch", "uRange", "double2"},
          {"NurbsPatch", "vRange", "double2"},
          {"NurbsPatch", "pointWeights", "double[]"},
          {"NurbsPatch", "trimCurve:counts", "int[]"},
          {"NurbsPatch", "trimCurve:orders", "int[]"},
          {"NurbsPatch", "trimCurve:vertexCounts", "int[]"},
          {"NurbsPatch", "trimCurve:knots", "double[]"},
          {"NurbsPatch", "trimCurve:ranges", "double2[]"},
          {"NurbsPatch", "trimCurve:points", "double3[]"},
          {"NurbsCurves", "knots", "double[]"},
          {"NurbsCurves", "ranges", "double2[]"},
          {"NurbsCurves", "pointWeights", "double[]"},
          // UsdVol: Volume's `field:<name>` relationships are DYNAMIC (any
          // name in the namespace), so no builtin is declared for them —
          // pxr's UsdVolVolume likewise has no property literally named
          // "field". The validator checks the namespace structurally.
          {"VolumeFieldAsset", "filePath", "asset"},
          {"VolumeFieldAsset", "fieldName", "token"},
          {"VolumeFieldAsset", "fieldIndex", "int"},
          {"VolumeFieldAsset", "fieldDataType", "token"},
          {"VolumeFieldAsset", "vectorDataRoleHint", "token"},
          {"OpenVDBAsset", "fieldClass", "token"},
          {"Field3DAsset", "fieldPurpose", "token"},
          // UsdVol ParticleField applied API attributes.
          {"ParticleFieldPositionAttributeAPI", "positions", "point3f[]"},
          {"ParticleFieldPositionAttributeAPI", "positionsh", "point3h[]"},
          {"ParticleFieldOrientationAttributeAPI", "orientations", "quatf[]"},
          {"ParticleFieldOrientationAttributeAPI", "orientationsh", "quath[]"},
          {"ParticleFieldScaleAttributeAPI", "scales", "float3[]"},
          {"ParticleFieldScaleAttributeAPI", "scalesh", "half3[]"},
          {"ParticleFieldOpacityAttributeAPI", "opacities", "float[]"},
          {"ParticleFieldOpacityAttributeAPI", "opacitiesh", "half[]"},
          {"ParticleFieldSphericalHarmonicsAttributeAPI",
           "radiance:sphericalHarmonicsCoefficients", "float3[]"},
          {"ParticleFieldSphericalHarmonicsAttributeAPI",
           "radiance:sphericalHarmonicsCoefficientsh", "half3[]"},
          // UsdRender relationships + optional token.
          {"RenderSettingsBase", "camera", "relationship"},
          {"RenderSettings", "products", "relationship"},
          {"RenderSettings", "renderingColorSpace", "token"},
          {"RenderProduct", "orderedVars", "relationship"},
          {"RenderPass", "passType", "token"},
          {"RenderPass", "command", "string[]"},
          {"RenderPass", "renderSource", "relationship"},
          {"RenderPass", "inputPasses", "relationship"},
          {"RenderPass", "fileName", "asset"},
      };
  for (const Decl& declaration : declarations) {
    declare(declaration.schema, declaration.name, declaration.type);
  }

  add("Shader", "info:implementationSource", "token", Token("id"));
  add("PhysicsMaterialAPI", "physics:staticFriction", "float", Value(0.0f));
  add("PhysicsMaterialAPI", "physics:dynamicFriction", "float", Value(0.0f));
  add("PhysicsMaterialAPI", "physics:restitution", "float", Value(0.0f));
}

const SchemaPropertyDefinition* SchemaRegistry::FindProperty(
    const PrimSpec& prim, const std::string& property_name) const {
  // Ancestry chain as POINTERS into strings the registry (or the prim) already
  // owns. This used to build a std::vector<std::string> and copy every type
  // name into it on every call; FindProperty is called for essentially every
  // property of every prim, so that allocation alone was a measurable share of
  // a material-heavy load. Chains are a handful of entries deep -- the array
  // covers far more than any registered schema needs, and a chain that somehow
  // exceeded it simply stops extending (the same answer the old loop gave when
  // it ran out of parents).
  const std::string* schemas[16];
  size_t schema_count = 0;
  // An abstract or API typeName leaves the prim typeless (Core 1.1 "Typed
  // Schemas"): no typed chain at all.
  const std::string* current =
      IsConcreteTypedSchemaName(prim.type_name()) ? &prim.type_name() : nullptr;
  while (current && !current->empty() && schema_count < 16) {
    schemas[schema_count++] = current;
    const std::string* next = nullptr;
    for (const auto& p : parents_) {
      if (p.first == *current) {
        next = &p.second;
        break;
      }
    }
    if (!next) break;
    current = next;
  }
  auto chain_rank = [&](const std::string& schema_type) -> int {
    for (size_t i = 0; i < schema_count; ++i) {
      if (*schemas[i] == schema_type) return static_cast<int>(i);
    }
    return -1;
  };
  // Prim definition order (Core 1.1 "The Prim Definition" / "Fallback Value
  // Resolution"): the typed schema (most-derived first) is strongest, then
  // each applied schema in list-op resolved order. Pick the strongest
  // matching definition rather than the first one registered.
  const SchemaPropertyDefinition* best = nullptr;
  int best_rank = 0;
  for (const SchemaPropertyDefinition& def : properties_) {
    if (!MatchDefinitionName(prim, def, property_name)) continue;
    int rank = chain_rank(def.schema_type);
    if (rank == 0) return &def;
    if (rank < 0) {
      const int applied = AppliedSchemaRank(prim, def.schema_type);
      if (applied < 0) continue;
      rank = 16 + applied;
    }
    if (!best || rank < best_rank) {
      best = &def;
      best_rank = rank;
    }
  }
  return best;
}

std::vector<std::string> SchemaRegistry::PropertyNames(
    const PrimSpec& prim) const {
  std::vector<std::string> result;
  std::vector<std::string> schemas;
  std::string current =
      IsConcreteTypedSchemaName(prim.type_name()) ? prim.type_name() : "";
  while (!current.empty()) {
    schemas.push_back(current);
    auto it = std::find_if(parents_.begin(), parents_.end(),
                           [&](const auto& p) { return p.first == current; });
    if (it == parents_.end()) break;
    current = it->second;
  }
  const auto applies = [&](const SchemaPropertyDefinition& def) {
    return std::find(schemas.begin(), schemas.end(), def.schema_type) !=
               schemas.end() ||
           HasAppliedSchema(prim, def.schema_type);
  };
  for (const SchemaPropertyDefinition& def : properties_) {
    if (!applies(def)) continue;
    std::string name = def.name;
    const std::string marker = "__INSTANCE__";
    const size_t marker_pos = name.find(marker);
    if (marker_pos != std::string::npos) {
      for (const std::string& applied : prim.meta().apiSchemas()) {
        const std::string prefix = def.schema_type + ":";
        if (applied.compare(0, prefix.size(), prefix) != 0) continue;
        std::string instantiated = name;
        instantiated.replace(marker_pos, marker.size(),
                             applied.substr(prefix.size()));
        if (std::find(result.begin(), result.end(), instantiated) ==
            result.end()) {
          result.push_back(std::move(instantiated));
        }
      }
      continue;
    }
    if (std::find(result.begin(), result.end(), name) == result.end()) {
      result.push_back(std::move(name));
    }
  }
  return result;
}

bool SchemaRegistry::InheritsFrom(const std::string& schema_type,
                                  const std::string& ancestor) const {
  std::string current = schema_type;
  // The parents table is a forest; a generous hop cap guards against an
  // accidentally-authored cycle.
  for (int hops = 0; hops < 32 && !current.empty(); ++hops) {
    if (current == ancestor) return true;
    auto it = std::find_if(parents_.begin(), parents_.end(),
                           [&](const auto& p) { return p.first == current; });
    if (it == parents_.end()) return false;
    current = it->second;
  }
  return false;
}

bool SchemaRegistry::HasParentEntry(const std::string& schema_type) const {
  return std::find_if(parents_.begin(), parents_.end(), [&](const auto& p) {
           return p.first == schema_type;
         }) != parents_.end();
}

bool SchemaRegistry::IsKnownSchema(const std::string& schema_type) const {
  if (std::find(known_schemas_.begin(), known_schemas_.end(), schema_type) !=
      known_schemas_.end()) return true;
  if (std::find_if(properties_.begin(), properties_.end(), [&](const auto& p) {
        return p.schema_type == schema_type;
      }) != properties_.end()) return true;
  return std::find_if(parents_.begin(), parents_.end(), [&](const auto& p) {
           return p.first == schema_type || p.second == schema_type;
         }) != parents_.end();
}

std::vector<std::string> SchemaRegistry::SchemaTypes() const {
  std::vector<std::string> result;
  auto add_unique = [&](const std::string& schema) {
    if (std::find(result.begin(), result.end(), schema) == result.end())
      result.push_back(schema);
  };
  for (const std::string& schema : known_schemas_) add_unique(schema);
  for (const auto& parent : parents_) {
    add_unique(parent.first);
    add_unique(parent.second);
  }
  for (const auto& property : properties_) add_unique(property.schema_type);
  std::sort(result.begin(), result.end());
  return result;
}

const SchemaRegistry& GetSchemaRegistry() {
  static const SchemaRegistry registry;
  return registry;
}

bool ParseSchemaFamilyAndVersion(const std::string& name, std::string* family,
                                 uint32_t* version, bool* has_version) {
  if (family) *family = name;
  if (version) *version = 0;
  if (has_version) *has_version = false;
  if (name.empty()) return false;
  size_t end = name.size();
  size_t digits = end;
  while (digits > 0 && name[digits - 1] >= '0' && name[digits - 1] <= '9') --digits;
  // Need >= 1 digit, a '_' right before them, and a non-empty family.
  if (digits == end || digits < 2 || name[digits - 1] != '_') return true;
  uint64_t v = 0;
  for (size_t i = digits; i < end; ++i) {
    v = v * 10u + static_cast<uint64_t>(name[i] - '0');
    if (v > UINT32_MAX) { v = UINT32_MAX; break; }
  }
  if (family) *family = name.substr(0, digits - 1);
  if (version) *version = static_cast<uint32_t>(v);
  if (has_version) *has_version = true;
  return true;
}

}  // namespace next
}  // namespace lightusd
