// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 Light Transport Entertainment Inc.

#include "usd-validation.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "crate-reader.hh"
#include "core/composition-types.hh"
#include "core/layer-types.hh"
#include "core/path.hh"
#include "core/prim-enums.hh"
#include "core/prim-spec.hh"
#include "color-transform.hh"
#include "enum-handlers.hh"
#include "io-util.hh"
#include "path-util.hh"
#include "pprint-enum.hh"  // to_string(APISchemas::APIName)
#include "stream-reader.hh"
#include "str-util.hh"
#include "lightusd.hh"

namespace lightusd {
namespace {

constexpr const char *kLayerLocation = "<layer>";
constexpr const char *kSpecVersion = "AOUSD Core Spec v1.0.1 (2025-12-12)";

struct AppliedSchema {
  std::string name;
  std::string instance_name;
};

using ColorSpaceSet = std::set<std::string>;
using PrimTypeByPath = std::unordered_map<std::string, std::string>;
using PrimSpecifierByPath = std::unordered_map<std::string, Specifier>;
using SkeletonJointCountByPath = std::unordered_map<std::string, size_t>;

std::vector<std::string> SplitString(const std::string &s, char delim) {
  std::vector<std::string> items;
  std::string item;

  for (char c : s) {
    if (c == delim) {
      items.emplace_back(item);
      item.clear();
    } else {
      item.push_back(c);
    }
  }

  items.emplace_back(item);
  return items;
}

std::string MakePropertyLocation(const std::string &prim_location,
                                 const std::string &prop_name) {
  if (prim_location.empty() || prim_location == kLayerLocation) {
    return prop_name;
  }

  return prim_location + "." + prop_name;
}

void AddIssue(USDValidationResult *result, USDValidationSeverity severity,
              const std::string &rule_id, const std::string &location,
              const std::string &message) {
  if (!result) {
    return;
  }

  result->issues.push_back({severity, rule_id, location, message});
}

void AddError(USDValidationResult *result, const std::string &rule_id,
              const std::string &location, const std::string &message) {
  AddIssue(result, USDValidationSeverity::Error, rule_id, location, message);
}

void AddWarning(USDValidationResult *result, const std::string &rule_id,
                const std::string &location, const std::string &message) {
  AddIssue(result, USDValidationSeverity::Warning, rule_id, location, message);
}

// Type-name sets for geom/shade encapsulation checks.
//
// Hardcoded here (rather than including the heavy usdGeom.hh/usdShade.hh) to
// keep this TU lightweight. Keep in sync with the kGeom*/kGPrim constants in
// src/usdGeom.hh and the kMaterial/kShader/kNodeGraph constants in
// src/usdShade.hh.
bool IsGprimTypeName(const std::string &type_name) {
  static const std::unordered_set<std::string> kGprimTypes = {
      "GPrim",       "Mesh",        "Sphere",       "Cube",
      "Cone",        "Cylinder",    "Cylinder_1",   "Capsule",
      "Capsule_1",   "Points",      "BasisCurves",  "NurbsCurves",
      "NurbsPatch",  "HermiteCurves", "Plane",      "TetMesh",
      "PointInstancer",
  };
  // NOTE: Xform/Scope/Camera/GeomSubset and lights are intentionally excluded
  // -- they legitimately contain or sit beside geometry.
  return kGprimTypes.count(type_name) > 0;
}

bool IsShadeContainerTypeName(const std::string &type_name) {
  // Containers that may legitimately parent a Shader/NodeGraph.
  return type_name == "Material" || type_name == "NodeGraph";
}

bool IsShadeConnectableTypeName(const std::string &type_name) {
  return type_name == "Shader" || type_name == "NodeGraph";
}

bool IsShadeConnectionTargetTypeName(const std::string &type_name) {
  return type_name == "Material" || IsShadeConnectableTypeName(type_name);
}

bool IsLuxLightTypeName(const std::string &type_name) {
  static const std::set<std::string> kLightTypes = {
      "SphereLight", "CylinderLight", "DomeLight",  "DomeLight_1",
      "DiskLight",   "RectLight",     "DistantLight", "GeometryLight",
      "PortalLight", "PluginLight"};
  return kLightTypes.count(type_name) > 0;
}

bool IsPhysicsPrimTypeName(const std::string &type_name) {
  static const std::set<std::string> kPhysicsTypes = {
      "PhysicsScene",      "PhysicsJoint",       "PhysicsRevoluteJoint",
      "PhysicsPrismaticJoint", "PhysicsSphericalJoint",
      "PhysicsFixedJoint", "PhysicsDistanceJoint",
      "PhysicsCollisionGroup", "MjcActuator", "MjcTendon", "MjcKeyframe",
      "NewtonActuator", "Preliminary_PhysicsGravitationalForce",
      "Preliminary_InfiniteColliderPlane"};
  return kPhysicsTypes.count(type_name) > 0;
}

bool IsPhysicsSchemaName(const std::string &schema_name) {
  return schema_name == "PhysicsRigidBodyAPI" ||
         schema_name == "PhysicsCollisionAPI" ||
         schema_name == "PhysicsMaterialAPI" ||
         schema_name == "PhysicsMeshCollisionAPI" ||
         schema_name == "PhysicsMassAPI" ||
         schema_name == "PhysicsFilteredPairsAPI" ||
         schema_name == "PhysicsArticulationRootAPI" ||
         schema_name == "PhysicsDriveAPI" ||
         schema_name == "PhysicsLimitAPI" ||
         schema_name == "MjcSceneAPI" ||
         schema_name == "MjcJointAPI" ||
         schema_name == "MjcCollisionAPI" ||
         schema_name == "MjcMeshCollisionAPI" ||
         schema_name == "MjcMaterialAPI" ||
         schema_name == "MjcSiteAPI" ||
         schema_name == "MjcImageableAPI" ||
         schema_name == "MjcEqualityAPI" ||
         schema_name == "MjcEqualityConnectAPI" ||
         schema_name == "MjcEqualityWeldAPI" ||
         schema_name == "MjcEqualityJointAPI" ||
         schema_name == "NewtonSceneAPI" ||
         schema_name == "NewtonXpbdSceneAPI" ||
         schema_name == "NewtonKaminoSceneAPI" ||
         schema_name == "NewtonArticulationRootAPI" ||
         schema_name == "NewtonCollisionAPI" ||
         schema_name == "NewtonMeshCollisionAPI" ||
         schema_name == "NewtonMaterialAPI" ||
         schema_name == "NewtonMimicAPI" ||
         schema_name == "NewtonActuatorDelayAPI" ||
         schema_name == "NewtonPDControlAPI" ||
         schema_name == "NewtonPIDControlAPI" ||
         schema_name == "NewtonNeuralControlAPI" ||
         schema_name == "NewtonMaxEffortClampingAPI" ||
         schema_name == "NewtonDCMotorClampingAPI" ||
         schema_name == "NewtonPositionBasedClampingAPI" ||
         schema_name == "Preliminary_PhysicsMaterialAPI" ||
         schema_name == "Preliminary_PhysicsRigidBodyAPI" ||
         schema_name == "Preliminary_PhysicsColliderAPI";
}

bool IsShaderInputName(const std::string &prop_name) {
  return prop_name.rfind("inputs:", 0) == 0 &&
         prop_name.size() > std::string("inputs:").size();
}

bool IsShaderOutputName(const std::string &prop_name) {
  return prop_name.rfind("outputs:", 0) == 0 &&
         prop_name.size() > std::string("outputs:").size();
}

bool IsMaterialTerminalOutputName(const std::string &prop_name) {
  return prop_name == "outputs:surface" ||
         prop_name == "outputs:displacement" || prop_name == "outputs:volume";
}

bool IsUsdPrimvarReaderId(const std::string &shader_id) {
  return shader_id.rfind("UsdPrimvarReader_", 0) == 0;
}

const std::string *UsdPrimvarReaderResultType(const std::string &shader_id) {
  static const std::unordered_map<std::string, std::string> kResultTypes = {
      {"UsdPrimvarReader_int", "int"},
      {"UsdPrimvarReader_float", "float"},
      {"UsdPrimvarReader_float2", "float2"},
      {"UsdPrimvarReader_float3", "float3"},
      {"UsdPrimvarReader_float4", "float4"},
      {"UsdPrimvarReader_string", "string"},
      {"UsdPrimvarReader_vector", "vector3f"},
      {"UsdPrimvarReader_normal", "normal3f"},
      {"UsdPrimvarReader_point", "point3f"},
      {"UsdPrimvarReader_matrix", "matrix4d"},
  };
  auto it = kResultTypes.find(shader_id);
  if (it == kResultTypes.end()) {
    return nullptr;
  }
  return &it->second;
}

bool IsMaterialXShaderId(const std::string &shader_id) {
  return shader_id.rfind("ND_", 0) == 0 ||
         shader_id.rfind("MaterialX", 0) == 0 ||
         shader_id == "MtlxUsdPreviewSurface" ||
         shader_id == "MtlxAutodeskStandardSurface" ||
         shader_id == "MtlxOpenPBRSurface";
}

bool EndsWith(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsMaterialXAssetPath(const std::string &asset_path) {
  return EndsWith(asset_path, ".mtlx") || EndsWith(asset_path, ".MTLX");
}

bool IsValidNamespacedIdentifier(const std::string &name) {
  if (name.empty()) {
    return false;
  }

  const std::vector<std::string> parts = SplitString(name, ':');
  for (const std::string &part : parts) {
    if (part.empty() || !is_valid_utf8_identifier(part)) {
      return false;
    }
  }
  return true;
}

bool IsMultipleApplySchemaName(const std::string &schema_name) {
  return schema_name == "CollectionAPI" || schema_name == "PhysicsDriveAPI" ||
         schema_name == "PhysicsLimitAPI" ||
         schema_name == "ColorSpaceDefinitionAPI";
}

bool IsSingleApplySchemaName(const std::string &schema_name) {
  return !schema_name.empty() && !IsMultipleApplySchemaName(schema_name);
}

// UsdPreviewSurface input name -> expected (role) value type.
//
// The table entries are generated from src/usd-preview-surface-schema.json by
// scripts/gen-usd-preview-surface-inputs.py (the same schema drives the
// validation fixtures). Regenerate after editing the schema; the
// `preview-surface-schema-sync` CTest fails if usd-preview-surface-inputs.inc
// is stale.
const std::unordered_map<std::string, std::string> &
UsdPreviewSurfaceInputTypes() {
  static const std::unordered_map<std::string, std::string> kInputs = {
#include "usd-preview-surface-inputs.inc"
  };
  return kInputs;
}

// Underlying base type for role-type agreement (Spec 6.5): color3f agrees with
// float3, etc. Used so an input authored as float3 is accepted where color3f is
// expected.
std::string BaseValueType(const std::string &type_name) {
  if (type_name == "texCoord2f") {
    return "float2";
  }
  if (type_name == "color3f" || type_name == "normal3f" ||
      type_name == "point3f" || type_name == "vector3f") {
    return "float3";
  }
  if (type_name == "color4f") {
    return "float4";
  }
  return type_name;
}

bool ValueTypesAgree(const std::string &authored, const std::string &expected) {
  if (authored == expected) {
    return true;
  }
  return BaseValueType(authored) == BaseValueType(expected);
}

// True if `ps` authors any composition arc. Used to gate locality-dependent
// rules: when a prim pulls opinions from a referenced/inherited/etc. layer, an
// attribute or parent type referenced here may be supplied by composition and
// is not a local error.
bool HasCompositionArc(const PrimSpec &ps) {
  const auto &m = ps.metas();
  return m.references.has_value() || m.payload.has_value() ||
         m.inherits.has_value() || m.specializes.has_value() ||
         m.variants.has_value() || m.variantSets.has_value();
}

bool IsOverride(const PrimSpec &ps) {
  return ps.specifier() == Specifier::Over;
}

// Ancestor-derived context threaded through the recursive prim walk.
struct AncestorContext {
  bool has_gprim_ancestor{false};
  std::string nearest_gprim_path;  // valid only when has_gprim_ancestor
  std::string parent_type;         // immediate parent typeName ("" at root)
  bool parent_mesh_face_count_known{false};
  size_t parent_mesh_face_count{0};
};

const Attribute *GetAttribute(const PrimSpec &ps, const std::string &name) {
  auto it = ps.props().find(name);
  if (it == ps.props().end() || !it->second.is_attribute()) {
    return nullptr;
  }
  return &it->second.get_attribute();
}

bool GetIntArray(const Attribute &attr, std::vector<int32_t> *out) {
  if (!out || attr.is_connection() || attr.is_blocked()) {
    return false;
  }
  return attr.get_value(out);
}

bool GetIntArrayProperty(const PrimSpec &ps, const std::string &name,
                         std::vector<int32_t> *out) {
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr) {
    return false;
  }
  return GetIntArray(*attr, out);
}

bool GetTokenProperty(const PrimSpec &ps, const std::string &name,
                      std::string *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto tok = attr->get_value<value::token>();
  if (!tok) {
    return false;
  }
  *out = tok->str();
  return true;
}

bool GetStringProperty(const PrimSpec &ps, const std::string &name,
                       std::string *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto str = attr->get_value<std::string>();
  if (!str) {
    return false;
  }
  *out = str.value();
  return true;
}

bool GetStringLikeProperty(const PrimSpec &ps, const std::string &name,
                           std::string *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto tok = attr->get_value<value::token>();
  if (tok) {
    *out = tok->str();
    return true;
  }
  auto str = attr->get_value<std::string>();
  if (str) {
    *out = str.value();
    return true;
  }
  return false;
}

bool GetAssetPathProperty(const PrimSpec &ps, const std::string &name,
                          value::AssetPath *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto asset = attr->get_value<value::AssetPath>();
  if (!asset) {
    return false;
  }
  *out = asset.value();
  return true;
}

bool GetNumericScalarProperty(const PrimSpec &ps, const std::string &name,
                              double *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto float_value = attr->get_value<float>();
  if (float_value) {
    *out = static_cast<double>(float_value.value());
    return true;
  }
  auto double_value = attr->get_value<double>();
  if (double_value) {
    *out = double_value.value();
    return true;
  }
  auto int_value = attr->get_value<int32_t>();
  if (int_value) {
    *out = static_cast<double>(int_value.value());
    return true;
  }
  auto int64_value = attr->get_value<int64_t>();
  if (int64_value) {
    *out = static_cast<double>(int64_value.value());
    return true;
  }
  auto uint_value = attr->get_value<uint32_t>();
  if (uint_value) {
    *out = static_cast<double>(uint_value.value());
    return true;
  }
  return false;
}

bool GetIntegerScalarProperty(const PrimSpec &ps, const std::string &name,
                              int64_t *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto i32 = attr->get_value<int32_t>();
  if (i32) {
    *out = static_cast<int64_t>(i32.value());
    return true;
  }
  auto i64 = attr->get_value<int64_t>();
  if (i64) {
    *out = i64.value();
    return true;
  }
  auto u32 = attr->get_value<uint32_t>();
  if (u32) {
    *out = static_cast<int64_t>(u32.value());
    return true;
  }
  return false;
}

bool GetDoubleArrayProperty(const PrimSpec &ps, const std::string &name,
                            std::vector<double> *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto double_values = attr->get_value<std::vector<double>>();
  if (double_values) {
    *out = double_values.value();
    return true;
  }
  auto float_values = attr->get_value<std::vector<float>>();
  if (float_values) {
    out->clear();
    out->reserve(float_values->size());
    for (float v : float_values.value()) {
      out->push_back(static_cast<double>(v));
    }
    return true;
  }
  return false;
}

bool GetPoint3fArrayProperty(const PrimSpec &ps, const std::string &name,
                             size_t *size) {
  if (!size) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto points = attr->get_value<std::vector<value::point3f>>();
  if (!points) {
    return false;
  }
  *size = points->size();
  return true;
}

bool GetTokenArrayProperty(const PrimSpec &ps, const std::string &name,
                           std::vector<value::token> *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  return attr->get_value(out);
}

bool GetFloatArrayProperty(const PrimSpec &ps, const std::string &name,
                           std::vector<float> *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  return attr->get_value(out);
}

bool GetFloat2Property(const PrimSpec &ps, const std::string &name,
                       value::float2 *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto value = attr->get_value<value::float2>();
  if (!value) {
    return false;
  }
  *out = value.value();
  return true;
}

bool GetVector3fProperty(const PrimSpec &ps, const std::string &name,
                         value::vector3f *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto value = attr->get_value<value::vector3f>();
  if (!value) {
    return false;
  }
  *out = value.value();
  return true;
}

bool GetVec3Property(const PrimSpec &ps, const std::string &name,
                     std::array<double, 3> *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }

  auto copy_float3 = [&](const auto &v) {
    (*out)[0] = static_cast<double>(v[0]);
    (*out)[1] = static_cast<double>(v[1]);
    (*out)[2] = static_cast<double>(v[2]);
  };

  auto v3f = attr->get_value<value::vector3f>();
  if (v3f) {
    copy_float3(v3f.value());
    return true;
  }
  auto p3f = attr->get_value<value::point3f>();
  if (p3f) {
    copy_float3(p3f.value());
    return true;
  }
  auto f3 = attr->get_value<value::float3>();
  if (f3) {
    copy_float3(f3.value());
    return true;
  }
  auto v3d = attr->get_value<value::vector3d>();
  if (v3d) {
    copy_float3(v3d.value());
    return true;
  }
  auto p3d = attr->get_value<value::point3d>();
  if (p3d) {
    copy_float3(p3d.value());
    return true;
  }
  auto d3 = attr->get_value<value::double3>();
  if (d3) {
    copy_float3(d3.value());
    return true;
  }
  return false;
}

bool GetQuatProperty(const PrimSpec &ps, const std::string &name,
                     std::array<double, 4> *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto qf = attr->get_value<value::quatf>();
  if (qf) {
    (*out)[0] = static_cast<double>(qf->imag[0]);
    (*out)[1] = static_cast<double>(qf->imag[1]);
    (*out)[2] = static_cast<double>(qf->imag[2]);
    (*out)[3] = static_cast<double>(qf->real);
    return true;
  }
  auto qd = attr->get_value<value::quatd>();
  if (qd) {
    (*out)[0] = qd->imag[0];
    (*out)[1] = qd->imag[1];
    (*out)[2] = qd->imag[2];
    (*out)[3] = qd->real;
    return true;
  }
  return false;
}

bool GetPoint3fArrayValuesProperty(const PrimSpec &ps, const std::string &name,
                                   std::vector<value::point3f> *out) {
  if (!out) {
    return false;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  return attr->get_value(out);
}

bool GetExtentArrayProperty(const PrimSpec &ps, const std::string &name,
                            std::vector<value::point3f> *out) {
  if (!out) {
    return false;
  }
  if (GetPoint3fArrayValuesProperty(ps, name, out)) {
    return true;
  }
  const Attribute *attr = GetAttribute(ps, name);
  if (!attr || attr->is_connection() || attr->is_blocked()) {
    return false;
  }
  auto values = attr->get_value<std::vector<value::float3>>();
  if (!values) {
    return false;
  }
  out->clear();
  out->reserve(values->size());
  for (const value::float3 &v : values.value()) {
    out->push_back(value::point3f{v[0], v[1], v[2]});
  }
  return true;
}

const Property *GetProperty(const PrimSpec &ps, const std::string &name) {
  auto it = ps.props().find(name);
  if (it == ps.props().end()) {
    return nullptr;
  }
  return &it->second;
}

template <typename T>
bool TryGetArrayLengthAs(const Attribute &attr, size_t *size) {
  auto value = attr.get_value<std::vector<T>>();
  if (!value) {
    return false;
  }
  *size = value->size();
  return true;
}

bool GetArrayLength(const Attribute &attr, size_t *size) {
  if (!size || attr.is_connection() || attr.is_blocked()) {
    return false;
  }

  return TryGetArrayLengthAs<bool>(attr, size) ||
         TryGetArrayLengthAs<uint8_t>(attr, size) ||
         TryGetArrayLengthAs<int16_t>(attr, size) ||
         TryGetArrayLengthAs<uint16_t>(attr, size) ||
         TryGetArrayLengthAs<int32_t>(attr, size) ||
         TryGetArrayLengthAs<uint32_t>(attr, size) ||
         TryGetArrayLengthAs<int64_t>(attr, size) ||
         TryGetArrayLengthAs<uint64_t>(attr, size) ||
         TryGetArrayLengthAs<float>(attr, size) ||
         TryGetArrayLengthAs<double>(attr, size) ||
         TryGetArrayLengthAs<value::half>(attr, size) ||
         TryGetArrayLengthAs<value::half2>(attr, size) ||
         TryGetArrayLengthAs<value::half3>(attr, size) ||
         TryGetArrayLengthAs<value::half4>(attr, size) ||
         TryGetArrayLengthAs<value::float2>(attr, size) ||
         TryGetArrayLengthAs<value::float3>(attr, size) ||
         TryGetArrayLengthAs<value::float4>(attr, size) ||
         TryGetArrayLengthAs<value::double2>(attr, size) ||
         TryGetArrayLengthAs<value::double3>(attr, size) ||
         TryGetArrayLengthAs<value::double4>(attr, size) ||
         TryGetArrayLengthAs<value::color3f>(attr, size) ||
         TryGetArrayLengthAs<value::color4f>(attr, size) ||
         TryGetArrayLengthAs<value::normal3f>(attr, size) ||
         TryGetArrayLengthAs<value::point3f>(attr, size) ||
         TryGetArrayLengthAs<value::vector3f>(attr, size) ||
         TryGetArrayLengthAs<value::quath>(attr, size) ||
         TryGetArrayLengthAs<value::quatf>(attr, size) ||
         TryGetArrayLengthAs<value::quatd>(attr, size) ||
         TryGetArrayLengthAs<value::matrix2f>(attr, size) ||
         TryGetArrayLengthAs<value::matrix3f>(attr, size) ||
         TryGetArrayLengthAs<value::matrix4f>(attr, size) ||
         TryGetArrayLengthAs<value::matrix2d>(attr, size) ||
         TryGetArrayLengthAs<value::matrix3d>(attr, size) ||
         TryGetArrayLengthAs<value::matrix4d>(attr, size) ||
         TryGetArrayLengthAs<value::token>(attr, size) ||
         TryGetArrayLengthAs<std::string>(attr, size);
}

bool GetValueArrayLength(const value::Value &value, size_t *size) {
  if (!size || value.is_none() || !value.is_array()) {
    return false;
  }

  *size = value.array_size();
  return true;
}

const value::TimeSamples *GetTimeSamples(const Attribute &attr) {
  if (!attr.has_timesamples()) {
    return nullptr;
  }

  return &attr.get_var().ts_raw();
}

void ValidateTimeSamples(const Attribute &attr, const std::string &rule_id,
                         const std::string &location,
                         USDValidationResult *result) {
  const value::TimeSamples *ts = GetTimeSamples(attr);
  if (!ts) {
    return;
  }

  const std::vector<value::TimeSamples::Sample> &samples = ts->get_samples();
  bool have_prev_time = false;
  double prev_time = 0.0;
  for (size_t i = 0; i < samples.size(); i++) {
    const value::TimeSamples::Sample &sample = samples[i];
    if (!std::isfinite(sample.t)) {
      AddError(result, rule_id, location,
               "timeSamples sample " + std::to_string(i) +
                   " has a non-finite time");
    }
    if (have_prev_time && sample.t <= prev_time) {
      AddError(result, rule_id, location,
               "timeSamples must be strictly increasing; sample " +
                   std::to_string(i) + " is at " + std::to_string(sample.t) +
                   " after " + std::to_string(prev_time));
    }
    have_prev_time = true;
    prev_time = sample.t;
  }
}

void ValidateArrayLengthAtAllSamples(const Attribute &attr,
                                     const std::string &prop_name,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     size_t expected,
                                     const std::string &expected_name,
                                     USDValidationResult *result) {
  if (attr.has_value() && !attr.is_blocked()) {
    size_t count = 0;
    if (GetArrayLength(attr, &count) && count != expected) {
      AddError(result, rule_id, location,
               prop_name + " has " + std::to_string(count) +
                   " default elements, but " + expected_name + " has " +
                   std::to_string(expected) + " elements");
    }
  }

  const value::TimeSamples *ts = GetTimeSamples(attr);
  if (!ts) {
    return;
  }

  ValidateTimeSamples(attr, rule_id, location, result);
  const std::vector<value::TimeSamples::Sample> &samples = ts->get_samples();
  for (size_t i = 0; i < samples.size(); i++) {
    const value::TimeSamples::Sample &sample = samples[i];
    if (sample.blocked || sample.value.is_none()) {
      continue;
    }
    size_t count = 0;
    if (!GetValueArrayLength(sample.value, &count)) {
      AddWarning(result, rule_id, location,
                 prop_name + " timeSamples sample " + std::to_string(i) +
                     " should be an array value");
      continue;
    }
    if (count != expected) {
      AddError(result, rule_id, location,
               prop_name + " timeSamples sample " + std::to_string(i) +
                   " at time " + std::to_string(sample.t) + " has " +
                   std::to_string(count) + " elements, but " + expected_name +
                   " has " + std::to_string(expected) + " elements");
    }
  }
}

bool IsPrimvarPropertyName(const std::string &prop_name) {
  return prop_name.rfind("primvars:", 0) == 0 &&
         prop_name.size() > std::string("primvars:").size() &&
         !(prop_name.size() >= 8 &&
           prop_name.compare(prop_name.size() - 8, 8, ":indices") == 0);
}

bool IsValidPrimvarInterpolation(const std::string &interp) {
  return interp == "constant" || interp == "uniform" ||
         interp == "vertex" || interp == "varying" ||
         interp == "faceVarying";
}

bool IsFinitePoint(const value::point3f &p) {
  return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

bool IsLengthCompatible(size_t count, size_t expected, bool allow_singleton) {
  return count == expected || (allow_singleton && count == 1);
}

void ValidateTokenSetProperty(const PrimSpec &ps, const std::string &prop_name,
                              const std::set<std::string> &allowed,
                              const std::string &rule_id,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (!prop->is_attribute()) {
    AddError(result, rule_id, location,
             prop_name + " must be a token attribute");
    return;
  }

  std::string token;
  if (!GetTokenProperty(ps, prop_name, &token)) {
    AddError(result, rule_id, location,
             prop_name + " must have a token value");
    return;
  }
  if (allowed.count(token) == 0) {
    AddError(result, rule_id, location,
             prop_name + " token `" + token + "` is not valid");
  }
}

void ValidatePositiveNumericProperty(const PrimSpec &ps,
                                     const std::string &prop_name,
                                     const std::string &rule_id,
                                     const std::string &prim_location,
                                     USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (!prop->is_attribute()) {
    AddError(result, rule_id, location,
             prop_name + " must be a numeric attribute");
    return;
  }

  double value = 0.0;
  if (!GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a numeric scalar value");
    return;
  }
  if (!std::isfinite(value) || value <= 0.0) {
    AddError(result, rule_id, location,
             prop_name + " must be finite and greater than 0");
  }
}

void ValidateNonNegativeNumericProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  double value = 0.0;
  if (!prop->is_attribute() || !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a numeric scalar value");
    return;
  }
  if (!std::isfinite(value) || value < 0.0) {
    AddError(result, rule_id, location,
             prop_name + " must be finite and non-negative");
  }
}

void ValidateNonNegativeIntegerProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  int64_t value = 0;
  if (!prop->is_attribute() || !GetIntegerScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have an integer scalar value");
    return;
  }
  if (value < 0) {
    AddError(result, rule_id, location,
             prop_name + " must be non-negative");
  }
}

void ValidateFiniteVec3Property(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result,
                                bool require_nonzero) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (!prop->is_attribute()) {
    AddError(result, rule_id, location,
             prop_name + " must be a 3-component numeric attribute");
    return;
  }
  std::array<double, 3> value;
  if (!GetVec3Property(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a 3-component numeric value");
    return;
  }
  const double len2 = value[0] * value[0] + value[1] * value[1] +
                      value[2] * value[2];
  if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
      !std::isfinite(value[2]) || (require_nonzero && len2 == 0.0)) {
    AddError(result, rule_id, location,
             prop_name + " must be finite" +
                 std::string(require_nonzero ? " and non-zero" : ""));
  }
}

void ValidateFiniteNonNegativeVec3Property(const PrimSpec &ps,
                                           const std::string &prop_name,
                                           const std::string &rule_id,
                                           const std::string &prim_location,
                                           USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (!prop->is_attribute()) {
    AddError(result, rule_id, location,
             prop_name + " must be a 3-component numeric attribute");
    return;
  }
  std::array<double, 3> value;
  if (!GetVec3Property(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a 3-component numeric value");
    return;
  }
  for (double c : value) {
    if (!std::isfinite(c) || c < 0.0) {
      AddError(result, rule_id, location,
               prop_name + " must be finite and non-negative");
      return;
    }
  }
}

void ValidateQuaternionProperty(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (!prop->is_attribute()) {
    AddError(result, rule_id, location,
             prop_name + " must be a quaternion attribute");
    return;
  }
  std::array<double, 4> value;
  if (!GetQuatProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a quaternion value");
    return;
  }
  const double len2 = value[0] * value[0] + value[1] * value[1] +
                      value[2] * value[2] + value[3] * value[3];
  for (double c : value) {
    if (!std::isfinite(c)) {
      AddError(result, rule_id, location,
               prop_name + " must be finite and non-zero");
      return;
    }
  }
  if (len2 == 0.0) {
    AddError(result, rule_id, location,
             prop_name + " must be finite and non-zero");
  } else if (std::fabs(len2 - 1.0) > 1.0e-3) {
    AddWarning(result, rule_id, location,
               prop_name + " should usually be unit length");
  }
}

void ValidateNumericArrayLengthProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        size_t expected,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (!prop->is_attribute()) {
    AddError(result, rule_id, location,
             prop_name + " must be a numeric array attribute");
    return;
  }
  std::vector<double> values;
  if (!GetDoubleArrayProperty(ps, prop_name, &values)) {
    AddError(result, rule_id, location,
             prop_name + " must have a numeric array value");
    return;
  }
  if (values.size() != expected) {
    AddError(result, rule_id, location,
             prop_name + " must have " + std::to_string(expected) +
                 " elements");
  }
  for (double value : values) {
    if (!std::isfinite(value)) {
      AddError(result, rule_id, location,
               prop_name + " elements must be finite");
      return;
    }
  }
}

void ValidateNumericArrayFiniteProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (!prop->is_attribute()) {
    AddError(result, rule_id, location,
             prop_name + " must be a numeric array attribute");
    return;
  }
  std::vector<double> values;
  if (!GetDoubleArrayProperty(ps, prop_name, &values)) {
    AddError(result, rule_id, location,
             prop_name + " must have a numeric array value");
    return;
  }
  for (double value : values) {
    if (!std::isfinite(value)) {
      AddError(result, rule_id, location,
               prop_name + " elements must be finite");
      return;
    }
  }
}

bool GetRelationshipTargetCount(const PrimSpec &ps,
                                const std::string &rel_name,
                                size_t *count) {
  if (!count) {
    return false;
  }
  const Property *rel = GetProperty(ps, rel_name);
  if (!rel || !rel->is_relationship()) {
    return false;
  }
  *count = rel->get_relationTargets().size();
  return true;
}

void ValidateIntArrayLengthMatchesRelationship(
    const PrimSpec &ps, const std::string &array_prop,
    const std::string &rel_prop, const std::string &rule_id,
    const std::string &prim_location, USDValidationResult *result) {
  const Property *prop = GetProperty(ps, array_prop);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, array_prop);
  std::vector<int32_t> values;
  if (!prop->is_attribute() || !GetIntArrayProperty(ps, array_prop, &values)) {
    AddError(result, rule_id, location,
             array_prop + " must have an int array value");
    return;
  }
  size_t target_count = 0;
  if (!GetRelationshipTargetCount(ps, rel_prop, &target_count)) {
    AddError(result, rule_id, location,
             array_prop + " requires relationship " + rel_prop);
    return;
  }
  if (values.size() != target_count) {
    AddError(result, rule_id, location,
             array_prop + " has " + std::to_string(values.size()) +
                 " elements, but " + rel_prop + " has " +
                 std::to_string(target_count) + " targets");
  }
}

void ValidateNumericArrayLengthMatchesRelationship(
    const PrimSpec &ps, const std::string &array_prop,
    const std::string &rel_prop, const std::string &rule_id,
    const std::string &prim_location, USDValidationResult *result) {
  const Property *prop = GetProperty(ps, array_prop);
  if (!prop) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, array_prop);
  std::vector<double> values;
  if (!prop->is_attribute() ||
      !GetDoubleArrayProperty(ps, array_prop, &values)) {
    AddError(result, rule_id, location,
             array_prop + " must have a numeric array value");
    return;
  }
  size_t target_count = 0;
  if (!GetRelationshipTargetCount(ps, rel_prop, &target_count)) {
    AddError(result, rule_id, location,
             array_prop + " requires relationship " + rel_prop);
    return;
  }
  if (values.size() != target_count) {
    AddError(result, rule_id, location,
             array_prop + " has " + std::to_string(values.size()) +
                 " elements, but " + rel_prop + " has " +
                 std::to_string(target_count) + " targets");
  }
}

void ValidateNumericMinMaxPair(const PrimSpec &ps,
                               const std::string &min_prop,
                               const std::string &max_prop,
                               const std::string &rule_id,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  for (const std::string &prop_name : {min_prop, max_prop}) {
    const Property *prop = GetProperty(ps, prop_name);
    if (prop) {
      double value = 0.0;
      if (!prop->is_attribute() ||
          !GetNumericScalarProperty(ps, prop_name, &value) ||
          !std::isfinite(value)) {
        AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
                 prop_name + " must have a finite numeric scalar value");
      }
    }
  }
  double min_value = 0.0;
  double max_value = 0.0;
  if (GetNumericScalarProperty(ps, min_prop, &min_value) &&
      GetNumericScalarProperty(ps, max_prop, &max_value) &&
      min_value > max_value) {
    AddError(result, rule_id, prim_location,
             min_prop + " must be <= " + max_prop);
  }
}

void ValidateArrayLengthProperty(const PrimSpec &ps,
                                 const std::string &prop_name,
                                 size_t expected,
                                 bool allow_singleton,
                                 const std::string &rule_id,
                                 const std::string &prim_location,
                                 const std::string &expected_name,
                                 USDValidationResult *result) {
  const Attribute *attr = GetAttribute(ps, prop_name);
  if (!attr) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  size_t count = 0;
  if (GetArrayLength(*attr, &count) &&
      !IsLengthCompatible(count, expected, allow_singleton)) {
    AddError(result, rule_id, location,
             prop_name + " has " + std::to_string(count) +
                 " elements, but must have " +
                 (allow_singleton ? "1 or " : "") +
                 std::to_string(expected) + " elements to match " +
                 expected_name);
  }
  if (attr->has_timesamples()) {
    const value::TimeSamples *ts = GetTimeSamples(*attr);
    if (!ts) {
      return;
    }
    ValidateTimeSamples(*attr, rule_id, location, result);
    const std::vector<value::TimeSamples::Sample> &samples =
        ts->get_samples();
    for (size_t i = 0; i < samples.size(); i++) {
      const value::TimeSamples::Sample &sample = samples[i];
      if (sample.blocked || sample.value.is_none()) {
        continue;
      }
      size_t sample_count = 0;
      if (!GetValueArrayLength(sample.value, &sample_count)) {
        AddWarning(result, rule_id, location,
                   prop_name + " timeSamples sample " + std::to_string(i) +
                       " should be an array value");
        continue;
      }
      if (!IsLengthCompatible(sample_count, expected, allow_singleton)) {
        AddError(result, rule_id, location,
                 prop_name + " timeSamples sample " + std::to_string(i) +
                     " at time " + std::to_string(sample.t) + " has " +
                     std::to_string(sample_count) + " elements, but must have " +
                     (allow_singleton ? "1 or " : "") +
                     std::to_string(expected) + " elements to match " +
                     expected_name);
      }
    }
  }
}

void ValidateGeomCommonProperties(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  USDValidationResult *result) {
  static const std::set<std::string> kVisibility = {"inherited", "invisible"};
  static const std::set<std::string> kPurpose = {"default", "render", "proxy",
                                                 "guide"};
  static const std::set<std::string> kOrientation = {"rightHanded",
                                                     "leftHanded"};

  ValidateTokenSetProperty(ps, "visibility", kVisibility,
                           "geom.gprim.visibility", prim_location, result);
  ValidateTokenSetProperty(ps, "purpose", kPurpose, "geom.gprim.purpose",
                           prim_location, result);
  ValidateTokenSetProperty(ps, "orientation", kOrientation,
                           "geom.gprim.orientation", prim_location, result);

  std::vector<value::point3f> extent;
  if (GetExtentArrayProperty(ps, "extent", &extent)) {
    const std::string location = MakePropertyLocation(prim_location, "extent");
    if (extent.size() != 2) {
      AddError(result, "geom.gprim.extent", location,
               "extent must contain exactly 2 points");
    } else {
      if (!IsFinitePoint(extent[0]) || !IsFinitePoint(extent[1])) {
        AddError(result, "geom.gprim.extent", location,
                 "extent bounds must be finite");
      }
      for (size_t axis = 0; axis < 3; axis++) {
        if (extent[0][axis] > extent[1][axis]) {
          AddError(result, "geom.gprim.extent", location,
                   "extent minimum must be <= maximum on every axis");
          break;
        }
      }
    }
  }

  size_t point_count = 0;
  if (ps.typeName() != "Points" && ps.typeName() != "PointInstancer" &&
      ps.typeName() != "BasisCurves" && ps.typeName() != "NurbsCurves" &&
      ps.typeName() != "HermiteCurves" && ps.typeName() != "NurbsPatch" &&
      GetPoint3fArrayProperty(ps, "points", &point_count)) {
    ValidateArrayLengthProperty(ps, "velocities", point_count, false,
                                "geom.gprim.arraySize", prim_location,
                                "points", result);
    ValidateArrayLengthProperty(ps, "accelerations", point_count, false,
                                "geom.gprim.arraySize", prim_location,
                                "points", result);
  }
}

void ValidateGeomPrimitive(const PrimSpec &ps, const std::string &prim_location,
                           USDValidationResult *result) {
  static const std::set<std::string> kAxis = {"X", "Y", "Z"};
  const std::string &type_name = ps.typeName();

  if (type_name == "Sphere") {
    ValidatePositiveNumericProperty(ps, "radius", "geom.primitive.size",
                                    prim_location, result);
  } else if (type_name == "Cube") {
    ValidatePositiveNumericProperty(ps, "size", "geom.primitive.size",
                                    prim_location, result);
  } else if (type_name == "Cone" || type_name == "Cylinder" ||
             type_name == "Capsule") {
    ValidatePositiveNumericProperty(ps, "radius", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "height", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  } else if (type_name == "Cylinder_1" || type_name == "Capsule_1") {
    ValidatePositiveNumericProperty(ps, "radiusTop", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "radiusBottom", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "height", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  } else if (type_name == "Plane") {
    ValidatePositiveNumericProperty(ps, "width", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "length", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  }
}

void ValidateGeomPoints(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  size_t point_count = 0;
  if (!GetPoint3fArrayProperty(ps, "points", &point_count)) {
    return;
  }

  ValidateArrayLengthProperty(ps, "ids", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result);
  ValidateArrayLengthProperty(ps, "widths", point_count, true,
                              "geom.points.size", prim_location, "points",
                              result);
  ValidateArrayLengthProperty(ps, "normals", point_count, true,
                              "geom.points.size", prim_location, "points",
                              result);
  ValidateArrayLengthProperty(ps, "velocities", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result);
  ValidateArrayLengthProperty(ps, "accelerations", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result);

  std::vector<float> widths;
  if (GetFloatArrayProperty(ps, "widths", &widths)) {
    for (size_t i = 0; i < widths.size(); i++) {
      if (!std::isfinite(widths[i]) || widths[i] < 0.0f) {
        AddError(result, "geom.points.widths",
                 MakePropertyLocation(prim_location, "widths"),
                 "widths[" + std::to_string(i) +
                     "] must be finite and non-negative");
      }
    }
  }
}

void ValidateGeomCurves(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  static const std::set<std::string> kBasisCurveTypes = {"linear", "cubic"};
  static const std::set<std::string> kBasis = {"bezier", "bspline",
                                               "catmullRom"};
  static const std::set<std::string> kWrap = {"nonperiodic", "periodic",
                                              "pinned"};
  static const std::set<std::string> kForm = {"open", "closed", "periodic"};

  if (ps.typeName() == "BasisCurves") {
    ValidateTokenSetProperty(ps, "type", kBasisCurveTypes,
                             "geom.curves.tokens", prim_location, result);
    ValidateTokenSetProperty(ps, "basis", kBasis, "geom.curves.tokens",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "wrap", kWrap, "geom.curves.tokens",
                             prim_location, result);
  } else if (ps.typeName() == "NurbsPatch") {
    ValidateTokenSetProperty(ps, "uForm", kForm, "geom.curves.tokens",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "vForm", kForm, "geom.curves.tokens",
                             prim_location, result);
  }

  size_t point_count = 0;
  const bool have_points = GetPoint3fArrayProperty(ps, "points", &point_count);
  std::vector<int32_t> counts;
  const bool have_counts =
      GetIntArrayProperty(ps, "curveVertexCounts", &counts);
  if (have_counts) {
    size_t total = 0;
    for (size_t i = 0; i < counts.size(); i++) {
      if (counts[i] <= 0) {
        AddError(result, "geom.curves.topology",
                 MakePropertyLocation(prim_location, "curveVertexCounts"),
                 "curveVertexCounts[" + std::to_string(i) +
                     "] must be positive");
        continue;
      }
      total += static_cast<size_t>(counts[i]);
    }
    if (have_points && total != point_count) {
      AddError(result, "geom.curves.topology",
               MakePropertyLocation(prim_location, "curveVertexCounts"),
               "sum(curveVertexCounts) is " + std::to_string(total) +
                   " but points has " + std::to_string(point_count) +
                   " elements");
    }
  }

  std::vector<float> widths;
  if (GetFloatArrayProperty(ps, "widths", &widths)) {
    for (size_t i = 0; i < widths.size(); i++) {
      if (!std::isfinite(widths[i]) || widths[i] < 0.0f) {
        AddError(result, "geom.curves.widths",
                 MakePropertyLocation(prim_location, "widths"),
                 "widths[" + std::to_string(i) +
                     "] must be finite and non-negative");
      }
    }

    const bool length_ok =
        widths.size() == 1 || (have_points && widths.size() == point_count) ||
        (have_counts && widths.size() == counts.size());
    if (!length_ok) {
      AddError(result, "geom.curves.widths",
               MakePropertyLocation(prim_location, "widths"),
               "widths length must be 1, match points, or match curve count");
    }
  }
}

void ValidatePointInstancer(const PrimSpec &ps,
                            const std::string &prim_location,
                            USDValidationResult *result) {
  size_t position_count = 0;
  const bool have_positions =
      GetPoint3fArrayProperty(ps, "positions", &position_count);

  const Property *prototypes = GetProperty(ps, "prototypes");
  size_t prototype_count = 0;
  if (!prototypes) {
    AddError(result, "geom.pointInstancer.prototypes",
             MakePropertyLocation(prim_location, "prototypes"),
             "PointInstancer must author rel prototypes");
  } else if (!prototypes->is_relationship()) {
    AddError(result, "geom.pointInstancer.prototypes",
             MakePropertyLocation(prim_location, "prototypes"),
             "prototypes must be a relationship");
  } else {
    const std::vector<Path> targets = prototypes->get_relationTargets();
    prototype_count = targets.size();
    if (targets.empty()) {
      AddError(result, "geom.pointInstancer.prototypes",
               MakePropertyLocation(prim_location, "prototypes"),
               "prototypes relationship must target at least one prototype");
    }
    for (const Path &target : targets) {
      std::string err;
      if (!pathutil::ValidatePath(target, &err)) {
        AddError(result, "geom.pointInstancer.prototypes",
                 MakePropertyLocation(prim_location, "prototypes"),
                 "prototypes target `" + target.full_path_name() +
                     "` is invalid: " + err);
      }
    }
  }

  std::vector<int32_t> proto_indices;
  if (GetIntArrayProperty(ps, "protoIndices", &proto_indices)) {
    if (have_positions && proto_indices.size() != position_count) {
      AddError(result, "geom.pointInstancer.indices",
               MakePropertyLocation(prim_location, "protoIndices"),
               "protoIndices length must match positions length");
    }
    for (size_t i = 0; i < proto_indices.size(); i++) {
      if (proto_indices[i] < 0 ||
          static_cast<size_t>(proto_indices[i]) >= prototype_count) {
        AddError(result, "geom.pointInstancer.indices",
                 MakePropertyLocation(prim_location, "protoIndices"),
                 "protoIndices[" + std::to_string(i) +
                     "] does not reference an authored prototype");
      }
    }
  }

  if (have_positions) {
    ValidateArrayLengthProperty(ps, "orientations", position_count, false,
                                "geom.pointInstancer.arraySize",
                                prim_location, "positions", result);
    ValidateArrayLengthProperty(ps, "scales", position_count, false,
                                "geom.pointInstancer.arraySize",
                                prim_location, "positions", result);
    ValidateArrayLengthProperty(ps, "ids", position_count, false,
                                "geom.pointInstancer.arraySize",
                                prim_location, "positions", result);
    ValidateArrayLengthProperty(ps, "velocities", position_count, false,
                                "geom.pointInstancer.arraySize",
                                prim_location, "positions", result);
    ValidateArrayLengthProperty(ps, "accelerations", position_count, false,
                                "geom.pointInstancer.arraySize",
                                prim_location, "positions", result);
    ValidateArrayLengthProperty(ps, "angularVelocities", position_count, false,
                                "geom.pointInstancer.arraySize",
                                prim_location, "positions", result);
  }
}

void ValidateCamera(const PrimSpec &ps, const std::string &prim_location,
                    USDValidationResult *result) {
  static const std::set<std::string> kProjection = {"perspective",
                                                    "orthographic"};
  static const std::set<std::string> kStereoRole = {"mono", "left", "right"};

  ValidateTokenSetProperty(ps, "projection", kProjection,
                           "geom.camera.tokens", prim_location, result);
  ValidateTokenSetProperty(ps, "stereoRole", kStereoRole,
                           "geom.camera.tokens", prim_location, result);

  value::float2 clipping_range;
  if (GetFloat2Property(ps, "clippingRange", &clipping_range)) {
    if (!std::isfinite(clipping_range[0]) ||
        !std::isfinite(clipping_range[1]) || clipping_range[0] < 0.0f ||
        clipping_range[1] <= clipping_range[0]) {
      AddError(result, "geom.camera.range",
               MakePropertyLocation(prim_location, "clippingRange"),
               "clippingRange must be finite with near >= 0 and far > near");
    }
  }

  ValidatePositiveNumericProperty(ps, "focalLength", "geom.camera.optics",
                                  prim_location, result);
  ValidatePositiveNumericProperty(ps, "horizontalAperture",
                                  "geom.camera.optics", prim_location, result);
  ValidatePositiveNumericProperty(ps, "verticalAperture",
                                  "geom.camera.optics", prim_location, result);
  ValidateNonNegativeNumericProperty(ps, "fStop", "geom.camera.optics",
                                     prim_location, result);

  double shutter_open = 0.0;
  double shutter_close = 0.0;
  const bool have_open =
      GetNumericScalarProperty(ps, "shutter:open", &shutter_open) ||
      GetNumericScalarProperty(ps, "shutterOpen", &shutter_open);
  const bool have_close =
      GetNumericScalarProperty(ps, "shutter:close", &shutter_close) ||
      GetNumericScalarProperty(ps, "shutterClose", &shutter_close);
  if (have_open && have_close &&
      (!std::isfinite(shutter_open) || !std::isfinite(shutter_close) ||
       shutter_close < shutter_open)) {
    AddError(result, "geom.camera.shutter", prim_location,
             "camera shutter close must be finite and >= shutter open");
  }
}

void ValidateNumericRangeProperty(const PrimSpec &ps,
                                  const std::string &prop_name,
                                  double min_value, double max_value,
                                  const std::string &rule_id,
                                  const std::string &prim_location,
                                  USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  double value = 0.0;
  if (!prop->is_attribute() || !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a numeric attribute");
    return;
  }
  if (!std::isfinite(value) || value < min_value || value > max_value) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be finite and in range [" +
                 std::to_string(min_value) + ", " +
                 std::to_string(max_value) + "]");
  }
}

void ValidateNumericMinProperty(const PrimSpec &ps, const std::string &prop_name,
                                double min_value,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  double value = 0.0;
  if (!prop->is_attribute() || !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a numeric attribute");
    return;
  }
  if (!std::isfinite(value) || value < min_value) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be finite and >= " +
                 std::to_string(min_value));
  }
}

void ValidateFiniteNumericProperty(const PrimSpec &ps,
                                   const std::string &prop_name,
                                   const std::string &rule_id,
                                   const std::string &prim_location,
                                   USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  double value = 0.0;
  if (!prop->is_attribute() || !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a numeric attribute");
    return;
  }
  if (!std::isfinite(value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be finite");
  }
}

void ValidateAssetPathProperty(const PrimSpec &ps, const std::string &prop_name,
                               const std::string &rule_id,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  value::AssetPath asset;
  if (!prop->is_attribute() || !GetAssetPathProperty(ps, prop_name, &asset)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be an asset attribute");
    return;
  }
  if (asset.GetAssetPath().empty()) {
    AddWarning(result, rule_id, MakePropertyLocation(prim_location, prop_name),
               prop_name + " is authored as an empty asset path");
  }
}

void ValidateRelationshipTargetPaths(const Property &prop,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     USDValidationResult *result);

void ValidateLuxRelationships(const PrimSpec &ps,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  static const std::vector<std::string> kRelationshipNames = {
      "light:filters", "portals", "proxyPrim", "geometry"};
  for (const std::string &name : kRelationshipNames) {
    const Property *prop = GetProperty(ps, name);
    if (!prop) {
      continue;
    }
    if (!prop->is_relationship()) {
      AddError(result, "lux.relationship.target",
               MakePropertyLocation(prim_location, name),
               name + " must be a relationship");
      continue;
    }
    ValidateRelationshipTargetPaths(*prop, "lux.relationship.target",
                                    MakePropertyLocation(prim_location, name),
                                    result);
  }
}

void ValidateLuxLight(const PrimSpec &ps, const std::string &prim_location,
                      USDValidationResult *result) {
  static const std::set<std::string> kTextureFormats = {
      "automatic", "latlong", "mirroredBall", "angular"};
  static const std::set<std::string> kPoleAxis = {"scene", "Y", "Z"};
  const std::string &type_name = ps.typeName();

  ValidateNonNegativeNumericProperty(ps, "inputs:intensity",
                                     "lux.light.inputs", prim_location,
                                     result);
  ValidateFiniteNumericProperty(ps, "inputs:exposure", "lux.light.inputs",
                                prim_location, result);
  ValidateNonNegativeNumericProperty(ps, "inputs:diffuse", "lux.light.inputs",
                                     prim_location, result);
  ValidateNonNegativeNumericProperty(ps, "inputs:specular", "lux.light.inputs",
                                     prim_location, result);
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

bool HasSchemaOrPropertyPrefix(const PrimSpec &ps,
                               const std::vector<AppliedSchema> &schemas,
                               const std::string &schema_prefix,
                               const std::string &prop_prefix) {
  for (const AppliedSchema &schema : schemas) {
    if (schema.name.rfind(schema_prefix, 0) == 0) {
      return true;
    }
  }
  for (const auto &prop_entry : ps.props()) {
    if (prop_entry.first.rfind(prop_prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

void ValidateRelationshipTargetPrimTypes(
    const Property &prop, const std::string &rule_id,
    const std::string &location, const PrimTypeByPath &prim_types,
    const std::set<std::string> &expected_types,
    USDValidationResult *result);

void ValidatePhysicsRelationship(const PrimSpec &ps,
                                 const std::string &prop_name,
                                 const std::string &rule_id,
                                 const std::string &prim_location,
                                 const PrimTypeByPath &prim_types,
                                 const std::set<std::string> &target_types,
                                 USDValidationResult *result) {
  const Property *prop = GetProperty(ps, prop_name);
  if (!prop) {
    return;
  }
  if (!prop->is_relationship()) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a relationship");
    return;
  }
  ValidateRelationshipTargetPaths(*prop, rule_id,
                                  MakePropertyLocation(prim_location, prop_name),
                                  result);
  if (!target_types.empty()) {
    ValidateRelationshipTargetPrimTypes(
        *prop, rule_id, MakePropertyLocation(prim_location, prop_name),
        prim_types, target_types, result);
  }
}

void ValidatePhysicsScene(const PrimSpec &ps,
                          const std::string &prim_location,
                          USDValidationResult *result) {
  ValidateNonNegativeNumericProperty(ps, "physics:gravityMagnitude",
                                     "physics.scene.gravity", prim_location,
                                     result);

  const Property *direction = GetProperty(ps, "physics:gravityDirection");
  if (!direction) {
    return;
  }
  value::vector3f v;
  if (!direction->is_attribute() ||
      !GetVector3fProperty(ps, "physics:gravityDirection", &v)) {
    AddError(result, "physics.scene.gravity",
             MakePropertyLocation(prim_location, "physics:gravityDirection"),
             "physics:gravityDirection must be a vector3f attribute");
    return;
  }
  const double len2 = double(v[0]) * double(v[0]) +
                      double(v[1]) * double(v[1]) +
                      double(v[2]) * double(v[2]);
  if (!std::isfinite(v[0]) || !std::isfinite(v[1]) ||
      !std::isfinite(v[2]) || len2 == 0.0) {
    AddError(result, "physics.scene.gravity",
             MakePropertyLocation(prim_location, "physics:gravityDirection"),
             "physics:gravityDirection must be finite and non-zero");
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
                             "physics.quaternion", prim_location, result);
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
  static const std::vector<std::string> kNonNegative = {
      "physics:mass",           "physics:density",
      "physics:staticFriction", "physics:dynamicFriction",
      "physics:restitution",    "physics:breakForce",
      "physics:breakTorque"};
  for (const std::string &prop_name : kNonNegative) {
    ValidateNonNegativeNumericProperty(ps, prop_name, "physics.value.range",
                                       prim_location, result);
  }
}

void ValidatePhysicsCollisionGroup(const PrimSpec &ps,
                                   const std::string &prim_location,
                                   const PrimTypeByPath &prim_types,
                                   USDValidationResult *result) {
  std::string merge_group;
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
  const std::string &type_name = ps.typeName();
  for (const AppliedSchema &schema : schemas) {
    if ((schema.name == "PhysicsDriveAPI" ||
         schema.name == "PhysicsLimitAPI" ||
         schema.name == "MjcJointAPI" ||
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
  for (const auto &prop_entry : ps.props()) {
    const std::string &prop_name = prop_entry.first;
    if (prop_name.rfind("physics:drive:", 0) == 0) {
      const std::string rest =
          prop_name.substr(std::string("physics:drive:").size());
      const size_t colon = rest.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const std::string dof = rest.substr(0, colon);
      const std::string field = rest.substr(colon + 1);
      const std::string location = MakePropertyLocation(prim_location, prop_name);
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
    } else if (prop_name.rfind("physics:limit:", 0) == 0) {
      const std::string rest =
          prop_name.substr(std::string("physics:limit:").size());
      const size_t colon = rest.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const std::string dof = rest.substr(0, colon);
      const std::string field = rest.substr(colon + 1);
      const std::string location = MakePropertyLocation(prim_location, prop_name);
      if (!IsPhysicsDofName(dof)) {
        AddError(result, "physics.limit.dof", location,
                 "PhysicsLimitAPI DOF `" + dof + "` is not supported");
      }
      if (field == "low" || field == "high") {
        ValidateFiniteNumericProperty(ps, prop_name, "physics.limit.range",
                                      prim_location, result);
        double value = 0.0;
        const bool have_value = GetNumericScalarProperty(ps, prop_name, &value);
        if (field == "low") {
          limit_low[dof] = std::make_pair(value, have_value);
        } else {
          limit_high[dof] = std::make_pair(value, have_value);
        }
      }
    }
  }

  for (const auto &entry : limit_low) {
    const auto high_it = limit_high.find(entry.first);
    if (high_it == limit_high.end()) {
      continue;
    }
    if (entry.second.second && high_it->second.second &&
        entry.second.first > high_it->second.first) {
      AddError(result, "physics.limit.range", prim_location,
               "physics:limit:" + entry.first + " low must be <= high");
    }
  }
}

void ValidateMjcTokenEnums(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  static const std::set<std::string> kAutoBool = {"auto", "true", "false"};
  ValidateTokenSetProperty(ps, "mjc:option:integrator",
                           {"euler", "implicit", "implicitfast", "RK4"},
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
  ValidateTokenSetProperty(ps, "mjc:compiler:inertiaFromGeom",
                           {"auto", "true", "false"},
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:actuatorfrclimited", kAutoBool,
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:ctrlLimited", kAutoBool,
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:forceLimited", kAutoBool,
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:actLimited", kAutoBool,
                           "physics.extension.mjc.token", prim_location,
                           result);
  ValidateTokenSetProperty(ps, "mjc:limited", kAutoBool,
                           "physics.extension.mjc.token", prim_location,
                           result);
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
  if (ps.typeName() == "MjcTendon") {
    ValidateTokenSetProperty(ps, "mjc:type", {"spatial", "fixed"},
                             "physics.extension.mjc.token", prim_location,
                             result);
  } else if (ps.typeName() == "MjcSensor") {
    static const std::set<std::string> kSensorObjectTypes = {
        "body", "xbody", "joint", "geom", "site", "camera", "light",
        "tendon", "actuator", "sensor", "numeric", "text", "tuple", "key",
        "plugin"};
    ValidateTokenSetProperty(
        ps, "mjc:type",
        {"touch", "accelerometer", "velocimeter", "gyro", "force", "torque",
         "magnetometer", "camprojection", "rangefinder", "jointpos",
         "jointvel", "tendonpos", "tendonvel", "actuatorpos", "actuatorvel",
         "actuatorfrc", "ballquat", "ballangvel", "jointlimitpos",
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
      ps.typeName().rfind("Mjc", 0) != 0) {
    return;
  }

  ValidateMjcTokenEnums(ps, prim_location, result);

  static const std::vector<std::string> kPositiveScalars = {
      "mjc:option:timestep", "mjc:option:impratio",
      "mjc:option:tolerance", "mjc:option:ls_tolerance",
      "mjc:option:noslip_tolerance", "mjc:option:ccd_tolerance",
      "mjc:torsionalfriction", "mjc:rollingfriction"};
  for (const std::string &prop_name : kPositiveScalars) {
    ValidatePositiveNumericProperty(ps, prop_name, "physics.extension.mjc.range",
                                    prim_location, result);
  }

  static const std::vector<std::string> kNonNegativeScalars = {
      "mjc:option:density", "mjc:option:viscosity", "mjc:option:o_margin",
      "mjc:compiler:boundMass", "mjc:compiler:boundInertia",
      "mjc:stiffness", "mjc:damping", "mjc:armature",
      "mjc:frictionloss", "mjc:margin", "mjc:solmix", "mjc:gap",
      "mjc:crankLength", "mjc:inheritRange", "mjc:width"};
  for (const std::string &prop_name : kNonNegativeScalars) {
    ValidateNonNegativeNumericProperty(ps, prop_name,
                                       "physics.extension.mjc.range",
                                       prim_location, result);
  }

  static const std::vector<std::string> kNonNegativeInts = {
      "mjc:group", "mjc:priority", "mjc:condim",
      "mjc:option:iterations", "mjc:option:ls_iterations",
      "mjc:option:noslip_iterations", "mjc:option:ccd_iterations",
      "mjc:option:sdf_iterations", "mjc:option:sdf_initpoints"};
  for (const std::string &prop_name : kNonNegativeInts) {
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
  ValidatePhysicsRelationship(ps, "mjc:refSite",
                              "physics.extension.mjc.relationship",
                              prim_location, prim_types, {}, result);
  ValidatePhysicsRelationship(ps, "mjc:sliderSite",
                              "physics.extension.mjc.relationship",
                              prim_location, prim_types, {}, result);
  ValidatePhysicsRelationship(ps, "mjc:path",
                              "physics.extension.mjc.relationship",
                              prim_location, prim_types, {}, result);
  ValidatePhysicsRelationship(ps, "mjc:sideSites",
                              "physics.extension.mjc.relationship",
                              prim_location, prim_types, {}, result);
}

void ValidateNewtonPhysics(const PrimSpec &ps,
                           const std::vector<AppliedSchema> &schemas,
                           const std::string &prim_location,
                           const PrimTypeByPath &prim_types,
                           USDValidationResult *result) {
  if (!HasSchemaOrPropertyPrefix(ps, schemas, "Newton", "newton:") &&
      ps.typeName() != "NewtonActuator") {
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
       {"newton:xpbd:softBodyRelaxation",
        "newton:xpbd:softContactRelaxation",
        "newton:xpbd:jointLinearRelaxation",
        "newton:xpbd:jointAngularRelaxation",
        "newton:xpbd:rigidContactRelaxation"}) {
    ValidateNumericRangeProperty(ps, prop_name, 0.0, 1.0,
                                 "physics.extension.newton.range",
                                 prim_location, result);
  }
  for (const char *prop_name :
       {"newton:xpbd:jointLinearCompliance",
        "newton:xpbd:jointAngularCompliance",
        "newton:xpbd:angularDamping",
        "newton:kamino:padmm:primalTolerance",
        "newton:kamino:padmm:dualTolerance",
        "newton:kamino:padmm:complementarityTolerance",
        "newton:kamino:constraints:alpha",
        "newton:kamino:constraints:beta",
        "newton:kamino:constraints:gamma",
        "newton:contactMargin", "newton:torsionalFriction",
        "newton:rollingFriction", "newton:kp", "newton:kd",
        "newton:ki", "newton:maxEffort", "newton:maxMotorEffort",
        "newton:saturationEffort", "newton:velocityLimit"}) {
    ValidateNonNegativeNumericProperty(ps, prop_name,
                                       "physics.extension.newton.range",
                                       prim_location, result);
  }
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
                 MakePropertyLocation(prim_location, "newton:lookupPositions"),
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
    if (schema.name.rfind("Preliminary_Physics", 0) == 0) {
      has_preliminary = true;
    }
  }
  for (const auto &prop_entry : ps.props()) {
    if (prop_entry.first.rfind("preliminary:physics:", 0) == 0 ||
        prop_entry.first.rfind("physics:gravitationalForce:", 0) == 0 ||
        prop_entry.first == "normal") {
      has_preliminary = true;
    }
  }
  if (!has_preliminary &&
      ps.typeName().rfind("Preliminary_Physics", 0) != 0 &&
      ps.typeName() != "Preliminary_InfiniteColliderPlane") {
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
                     USDValidationResult *result) {
  const std::string &type_name = ps.typeName();
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
                              prim_types, {"PhysicsScene"},
                              result);
  ValidatePhysicsRelationship(ps, "physics:filteredPairs",
                              "physics.relationship.target", prim_location,
                              prim_types, {},
                              result);
  if (type_name == "PhysicsCollisionGroup") {
    ValidatePhysicsCollisionGroup(ps, prim_location, prim_types, result);
  } else {
    ValidatePhysicsRelationship(ps, "physics:filteredGroups",
                                "physics.relationship.target", prim_location,
                                prim_types, {"PhysicsCollisionGroup"}, result);
  }

  ValidatePhysicsDriveAndLimitAPIs(ps, applied_schemas, prim_location, result);
  ValidateMjcPhysics(ps, applied_schemas, prim_location, prim_types, result);
  ValidateNewtonPhysics(ps, applied_schemas, prim_location, prim_types, result);
  ValidatePreliminaryPhysics(ps, applied_schemas, prim_location, prim_types,
                             result);
}

void ValidateMeshTopology(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result,
                          size_t *out_face_count) {
  std::vector<int32_t> counts;
  std::vector<int32_t> indices;
  const bool have_counts = GetIntArrayProperty(ps, "faceVertexCounts", &counts);
  const bool have_indices =
      GetIntArrayProperty(ps, "faceVertexIndices", &indices);

  if (out_face_count && have_counts) {
    *out_face_count = counts.size();
  }

  if (!have_counts && !have_indices) {
    return;
  }

  size_t total = 0;
  if (have_counts) {
    for (size_t i = 0; i < counts.size(); i++) {
      if (counts[i] <= 0) {
        AddError(result, "geom.mesh.topology.count",
                 MakePropertyLocation(prim_location, "faceVertexCounts"),
                 "faceVertexCounts[" + std::to_string(i) + "] must be "
                 "positive, but is " + std::to_string(counts[i]));
        continue;
      }
      total += static_cast<size_t>(counts[i]);
    }
  }

  if (have_counts && have_indices && total != indices.size()) {
    AddError(result, "geom.mesh.topology.size",
             MakePropertyLocation(prim_location, "faceVertexIndices"),
             "sum(faceVertexCounts) is " + std::to_string(total) +
                 " but faceVertexIndices has " +
                 std::to_string(indices.size()) + " elements");
  }

  size_t point_count = 0;
  const bool have_points = GetPoint3fArrayProperty(ps, "points", &point_count);
  if (have_indices && have_points) {
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] < 0 || static_cast<size_t>(indices[i]) >= point_count) {
        AddError(result, "geom.mesh.topology.index",
                 MakePropertyLocation(prim_location, "faceVertexIndices"),
                 "faceVertexIndices[" + std::to_string(i) + "] = " +
                     std::to_string(indices[i]) +
                     " is outside points range [0, " +
                     std::to_string(point_count) + ")");
      }
    }
  }
}

void ValidateGeomSubsetTopology(const PrimSpec &ps,
                                const std::string &prim_location,
                                const AncestorContext &ancestors,
                                USDValidationResult *result) {
  if (ancestors.parent_type != "Mesh" ||
      !ancestors.parent_mesh_face_count_known) {
    return;
  }

  std::string element_type = "face";
  (void)GetTokenProperty(ps, "elementType", &element_type);
  if (element_type != "face") {
    return;
  }

  std::vector<int32_t> indices;
  if (!GetIntArrayProperty(ps, "indices", &indices)) {
    return;
  }

  std::set<int32_t> seen;
  for (size_t i = 0; i < indices.size(); i++) {
    const int32_t index = indices[i];
    if (index < 0 ||
        static_cast<size_t>(index) >= ancestors.parent_mesh_face_count) {
      AddError(result, "geom.subset.indices",
               MakePropertyLocation(prim_location, "indices"),
               "indices[" + std::to_string(i) + "] = " +
                   std::to_string(index) +
                   " is outside parent face range [0, " +
                   std::to_string(ancestors.parent_mesh_face_count) + ")");
      continue;
    }
    if (!seen.insert(index).second) {
      AddWarning(result, "geom.subset.indices",
                 MakePropertyLocation(prim_location, "indices"),
                 "indices contains duplicate element " +
                     std::to_string(index));
    }
  }
}

void ValidatePrimvars(const PrimSpec &ps, const std::string &prim_location,
                      USDValidationResult *result) {
  for (const auto &prop_entry : ps.props()) {
    const std::string &prop_name = prop_entry.first;
    const Property &prop = prop_entry.second;
    if (!IsPrimvarPropertyName(prop_name) || !prop.is_attribute()) {
      continue;
    }

    const Attribute &attr = prop.get_attribute();
    const std::string prop_location =
        MakePropertyLocation(prim_location, prop_name);

    if (attr.metas().has_interpolation()) {
      const std::string interp = attr.metas().get_interpolation().str();
      if (!IsValidPrimvarInterpolation(interp)) {
        AddError(result, "geom.primvar.metadata", prop_location,
                 "invalid primvar interpolation `" + interp + "`");
      }
    }

    if (attr.metas().has_elementSize() && attr.metas().get_elementSize() == 0) {
      AddError(result, "geom.primvar.metadata", prop_location,
               "primvar elementSize must be greater than 0");
    }

    auto indices_it = ps.props().find(prop_name + ":indices");
    if (indices_it == ps.props().end()) {
      continue;
    }
    const std::string indices_location =
        MakePropertyLocation(prim_location, prop_name + ":indices");
    if (!indices_it->second.is_attribute()) {
      AddError(result, "geom.primvar.indices", indices_location,
               "indexed primvar indices must be an int[] attribute");
      continue;
    }

    std::vector<int32_t> indices;
    if (!GetIntArray(indices_it->second.get_attribute(), &indices)) {
      AddError(result, "geom.primvar.indices", indices_location,
               "indexed primvar indices must have int[] values");
      continue;
    }

    size_t value_count = 0;
    const bool have_value_count = GetArrayLength(attr, &value_count);
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] < 0 ||
          (have_value_count && static_cast<size_t>(indices[i]) >= value_count)) {
        std::string msg = "indices[" + std::to_string(i) + "] = " +
                          std::to_string(indices[i]);
        if (have_value_count) {
          msg += " is outside primvar value range [0, " +
                 std::to_string(value_count) + ")";
        } else {
          msg += " must be non-negative";
        }
        AddError(result, "geom.primvar.indices", indices_location, msg);
      }
    }
  }
}

// Returns the `info:id` token authored on a Shader prim, or empty.
std::string GetShaderInfoId(const PrimSpec &ps) {
  auto it = ps.props().find("info:id");
  if (it == ps.props().end() || !it->second.is_attribute()) {
    return std::string();
  }
  auto info_id = it->second.get_attribute().get_value<value::token>();
  return info_id ? info_id->str() : std::string();
}

// Validate a UsdPreviewSurface Shader's `inputs:*` against its schema: unknown
// inputs and input type mismatches. Reported as warnings -- a UsdPreviewSurface
// with off-schema inputs still loads, but the content is non-conformant.
void ValidateUsdPreviewSurface(const PrimSpec &ps,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  static const std::string kInputsPrefix = "inputs:";
  const auto &inputs = UsdPreviewSurfaceInputTypes();

  for (const auto &prop_entry : ps.props()) {
    const std::string &prop_name = prop_entry.first;
    if (prop_name.rfind(kInputsPrefix, 0) != 0) {
      continue;
    }

    const Property &prop = prop_entry.second;
    if (!prop.is_attribute()) {
      continue;
    }

    const std::string input_name = prop_name.substr(kInputsPrefix.size());
    const std::string prop_location =
        MakePropertyLocation(prim_location, prop_name);

    auto it = inputs.find(input_name);
    if (it == inputs.end()) {
      AddWarning(result, "shade.preview.unknownInput", prop_location,
                 "`" + prop_name + "` is not a UsdPreviewSurface input");
      continue;
    }

    const std::string authored = prop.value_type_name();
    if (!authored.empty() && !ValueTypesAgree(authored, it->second)) {
      AddWarning(result, "shade.preview.inputType", prop_location,
                 "UsdPreviewSurface input `" + input_name + "` should be `" +
                     it->second + "` but is `" + authored + "`");
    }
  }

  const std::array<const char *, 6> unit_range_inputs = {
      "inputs:roughness", "inputs:metallic", "inputs:clearcoat",
      "inputs:clearcoatRoughness", "inputs:opacity", "inputs:occlusion"};
  for (const char *prop_name : unit_range_inputs) {
    double value = 0.0;
    if (GetNumericScalarProperty(ps, prop_name, &value) &&
        (value < 0.0 || value > 1.0)) {
      AddWarning(result, "shade.preview.inputRange",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) + " should be in range [0, 1], but is " +
                     std::to_string(value));
    }
  }

  double ior = 0.0;
  if (GetNumericScalarProperty(ps, "inputs:ior", &ior) && !(ior > 0.0)) {
    AddWarning(result, "shade.preview.inputRange",
               MakePropertyLocation(prim_location, "inputs:ior"),
               "inputs:ior should be greater than 0, but is " +
                   std::to_string(ior));
  }

  std::string opacity_mode;
  if (GetTokenProperty(ps, "inputs:opacityMode", &opacity_mode) &&
      opacity_mode != "opacity" && opacity_mode != "transparent" &&
      opacity_mode != "presence") {
    AddWarning(result, "shade.preview.opacityMode",
               MakePropertyLocation(prim_location, "inputs:opacityMode"),
               "invalid UsdPreviewSurface opacityMode `" + opacity_mode + "`");
  }
}

bool ValidateScenePath(const Path &path, std::string *err) {
  return pathutil::ValidatePath(path, err);
}

bool ValidatePrimPath(const Path &path, std::string *err) {
  return pathutil::ValidatePrimPath(path, err);
}

bool IsAbsolutePrimPath(const Path &path) {
  return path.is_valid() && path.is_prim_path() && path.is_absolute_path();
}

bool IsEmptyPath(const Path &path) {
  return path.prim_part().empty() && path.prop_part().empty();
}

const std::string *FindPrimType(const PrimTypeByPath &prim_types,
                                const std::string &prim_path) {
  auto it = prim_types.find(prim_path);
  if (it == prim_types.end()) {
    return nullptr;
  }
  return &it->second;
}

const Specifier *FindPrimSpecifier(const PrimSpecifierByPath &prim_specifiers,
                                   const std::string &prim_path) {
  auto it = prim_specifiers.find(prim_path);
  if (it == prim_specifiers.end()) {
    return nullptr;
  }
  return &it->second;
}

bool IsValidAbsolutePrimTargetPath(const Path &path, std::string *err) {
  if (!ValidatePrimPath(path, err)) {
    return false;
  }
  if (!path.is_absolute_path() || path.is_root_path()) {
    if (err) {
      *err = "target must be an absolute non-root prim path";
    }
    return false;
  }
  return true;
}

bool IsValidConnectionTargetPath(const Path &path, std::string *err) {
  if (!path.is_prim_property_path()) {
    if (err) {
      *err = "connection target must be a prim property path";
    }
    return false;
  }

  Path prim_path(path.prim_part(), "");
  if (!ValidatePrimPath(prim_path, err)) {
    return false;
  }
  if (!pathutil::ValidatePropPath(path, err)) {
    return false;
  }
  return true;
}

void ValidateRelationshipTargetPaths(const Property &prop,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     USDValidationResult *result) {
  if (!prop.is_relationship()) {
    AddError(result, rule_id, location, "expected a relationship");
    return;
  }

  const std::vector<Path> targets = prop.get_relationTargets();
  if (targets.empty()) {
    AddWarning(result, rule_id, location, "relationship has no targets");
    return;
  }

  for (const Path &target : targets) {
    std::string err;
    if (!IsValidAbsolutePrimTargetPath(target, &err)) {
      AddError(result, rule_id, location,
               "contains invalid target path `" + target.full_path_name() +
                   "`: " + err);
    }
  }
}

void ValidateRelationshipTargetPrimTypes(
    const Property &prop, const std::string &rule_id,
    const std::string &location, const PrimTypeByPath &prim_types,
    const std::set<std::string> &expected_types,
    USDValidationResult *result) {
  if (!prop.is_relationship()) {
    return;
  }

  for (const Path &target : prop.get_relationTargets()) {
    const std::string *target_type =
        FindPrimType(prim_types, target.prim_part());
    if (target_type && !target_type->empty() &&
        expected_types.count(*target_type) == 0) {
      AddWarning(result, rule_id, location,
                 "relationship target `" + target.prim_part() +
                     "` has type `" + *target_type + "`");
    }
  }
}

void ValidateConnectionTargets(const Attribute &attr,
                               const std::string &rule_id,
                               const std::string &location,
                               const PrimTypeByPath &prim_types,
                               USDValidationResult *result,
                               bool require_output_target) {
  for (const Path &target : attr.connections()) {
    std::string err;
    if (!IsValidConnectionTargetPath(target, &err)) {
      AddError(result, rule_id, location,
               "contains invalid connection target `" +
                   target.full_path_name() + "`: " + err);
      continue;
    }

    const std::string &target_prop = target.prop_part();
    if (require_output_target && !IsShaderOutputName(target_prop)) {
      AddWarning(result, rule_id, location,
                 "connection target `" + target.full_path_name() +
                     "` is not an outputs:* source");
    } else if (!require_output_target && !IsShaderOutputName(target_prop) &&
               !IsShaderInputName(target_prop)) {
      AddWarning(result, rule_id, location,
                 "connection target `" + target.full_path_name() +
                     "` is not an inputs:* or outputs:* shade port");
    }

    const std::string *target_type =
        FindPrimType(prim_types, target.prim_part());
    if (target_type && !target_type->empty() &&
        !IsShadeConnectionTargetTypeName(*target_type)) {
      AddWarning(result, rule_id, location,
                 "connection target prim `" + target.prim_part() +
                     "` has type `" + *target_type +
                     "`, expected Material, Shader, or NodeGraph");
    }
  }
}

bool IsUsdUVTextureWrapToken(const std::string &token) {
  return token == "useMetadata" || token == "black" || token == "clamp" ||
         token == "repeat" || token == "mirror";
}

bool IsUsdUVTextureColorSpaceToken(const std::string &token) {
  return token == "auto" || token == "raw" || token == "sRGB";
}

bool HasAppliedSchema(const std::vector<AppliedSchema> &schemas,
                      const std::string &schema_name);

void ValidateUsdUVTexture(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result) {
  value::AssetPath file;
  const Attribute *file_attr = GetAttribute(ps, "inputs:file");
  if (!file_attr) {
    AddWarning(result, "shade.uvTexture.file",
               MakePropertyLocation(prim_location, "inputs:file"),
               "UsdUVTexture should author inputs:file");
  } else if (!file_attr->has_connections() &&
             (!GetAssetPathProperty(ps, "inputs:file", &file) ||
              file.GetAssetPath().empty())) {
    AddWarning(result, "shade.uvTexture.file",
               MakePropertyLocation(prim_location, "inputs:file"),
               "UsdUVTexture inputs:file should be a non-empty asset path");
  }

  const std::array<const char *, 2> wrap_inputs = {"inputs:wrapS",
                                                   "inputs:wrapT"};
  for (const char *prop_name : wrap_inputs) {
    std::string token;
    if (GetTokenProperty(ps, prop_name, &token) &&
        !IsUsdUVTextureWrapToken(token)) {
      AddWarning(result, "shade.uvTexture.wrap",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) +
                     " has invalid wrap token `" + token + "`");
    }
  }

  std::string color_space;
  if (GetTokenProperty(ps, "inputs:sourceColorSpace", &color_space) &&
      !IsUsdUVTextureColorSpaceToken(color_space)) {
    AddWarning(result, "shade.uvTexture.sourceColorSpace",
               MakePropertyLocation(prim_location, "inputs:sourceColorSpace"),
               "UsdUVTexture sourceColorSpace should be auto, raw, or sRGB, "
               "but is `" +
                   color_space + "`");
  }
}

void ValidateUsdPrimvarReader(const PrimSpec &ps,
                              const std::string &shader_id,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  const Attribute *varname_attr = GetAttribute(ps, "inputs:varname");
  if (!varname_attr) {
    AddWarning(result, "shade.primvarReader.varname",
               MakePropertyLocation(prim_location, "inputs:varname"),
               "UsdPrimvarReader should author inputs:varname");
    return;
  }

  if (varname_attr->has_connections()) {
    // Connected varname inputs resolve at composition/evaluation time. The
    // output terminal type is still local schema data and must be checked.
  } else if (varname_attr->type_name() != "token" &&
             varname_attr->type_name() != "string") {
    AddWarning(result, "shade.primvarReader.varname",
               MakePropertyLocation(prim_location, "inputs:varname"),
               "UsdPrimvarReader inputs:varname should be token or string, "
               "but is `" +
                   varname_attr->type_name() + "`");
  } else {
    std::string varname;
    if (!GetStringLikeProperty(ps, "inputs:varname", &varname) ||
        varname.empty()) {
      AddWarning(result, "shade.primvarReader.varname",
                 MakePropertyLocation(prim_location, "inputs:varname"),
                 "UsdPrimvarReader inputs:varname should be non-empty");
    }
  }

  const std::string *expected_type = UsdPrimvarReaderResultType(shader_id);
  if (!expected_type) {
    AddWarning(result, "shade.primvarReader.result", prim_location,
               "unknown UsdPrimvarReader shader id `" + shader_id + "`");
    return;
  }

  const std::string location = MakePropertyLocation(prim_location,
                                                    "outputs:result");
  const Property *result_prop = GetProperty(ps, "outputs:result");
  if (!result_prop) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " should author outputs:result");
    return;
  }
  if (!result_prop->is_attribute()) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " outputs:result should be an attribute");
    return;
  }

  const Attribute &result_attr = result_prop->get_attribute();
  if (!ValueTypesAgree(result_attr.type_name(), *expected_type)) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " outputs:result should be " + *expected_type +
                   ", but is `" + result_attr.type_name() + "`");
  }
}

void ValidateMaterialXConfig(const PrimSpec &ps,
                             const std::vector<AppliedSchema> &applied_schemas,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  bool has_config_property = false;
  for (const auto &prop_entry : ps.props()) {
    if (prop_entry.first.rfind("config:mtlx:", 0) == 0) {
      has_config_property = true;
      break;
    }
  }

  const bool has_config_api =
      HasAppliedSchema(applied_schemas, "MaterialXConfigAPI");
  if (has_config_property && !has_config_api) {
    AddWarning(result, "shade.materialX.configAPI", prim_location,
               "config:mtlx:* properties are authored without applying "
               "MaterialXConfigAPI");
  }

  if (has_config_api && ps.typeName() != "Material") {
    AddWarning(result, "shade.materialX.configAPI", prim_location,
               "MaterialXConfigAPI is expected on Material prims");
  }

  std::string version;
  if (GetStringProperty(ps, "config:mtlx:version", &version) &&
      (version.empty() || version.rfind("1.", 0) != 0)) {
    AddWarning(result, "shade.materialX.version",
               MakePropertyLocation(prim_location, "config:mtlx:version"),
               "MaterialX version should be a 1.x version string, but is `" +
                   version + "`");
  }

  std::string source_uri;
  if (GetStringProperty(ps, "config:mtlx:sourceUri", &source_uri) &&
      !source_uri.empty() && !IsMaterialXAssetPath(source_uri)) {
    AddWarning(result, "shade.materialX.sourceUri",
               MakePropertyLocation(prim_location, "config:mtlx:sourceUri"),
               "MaterialX sourceUri should usually reference a .mtlx asset");
  }
}

void ValidateMaterialUEConfig(const PrimSpec &ps,
                              const std::vector<AppliedSchema> &applied_schemas,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  bool has_config_property = false;
  for (const auto &prop_entry : ps.props()) {
    if (prop_entry.first.rfind("config:unreal:", 0) == 0) {
      has_config_property = true;
      break;
    }
  }

  const bool has_config_api =
      HasAppliedSchema(applied_schemas, "MaterialUEConfigAPI");
  if (has_config_property && !has_config_api) {
    AddWarning(result, "shade.materialUE.configAPI", prim_location,
               "config:unreal:* properties are authored without applying "
               "MaterialUEConfigAPI");
  }
  if (has_config_api && ps.typeName() != "Material") {
    AddWarning(result, "shade.materialUE.configAPI", prim_location,
               "MaterialUEConfigAPI is expected on Material prims");
  }

  std::string version;
  if (GetStringProperty(ps, "config:unreal:version", &version) &&
      version != "1") {
    AddWarning(result, "shade.materialUE.version",
               MakePropertyLocation(prim_location, "config:unreal:version"),
               "MaterialUEConfigAPI version is not the supported version `1`");
  }
  std::string authority;
  if (GetStringProperty(ps, "config:unreal:graphAuthority", &authority) &&
      authority.empty()) {
    AddWarning(result, "shade.materialUE.graphAuthority",
               MakePropertyLocation(prim_location,
                                    "config:unreal:graphAuthority"),
               "graphAuthority should not be empty");
  }
}

void ValidateMaterialXShader(const PrimSpec &ps,
                             const std::string &shader_id,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  if (!IsMaterialXShaderId(shader_id)) {
    return;
  }

  if (ps.props().count("outputs:surface") == 0 &&
      ps.props().count("outputs:out") == 0) {
    AddWarning(result, "shade.materialX.output", prim_location,
               "MaterialX shader `" + shader_id +
                   "` should author outputs:surface or outputs:out");
  }
}

void ValidateSkeletonTopology(const PrimSpec &ps,
                              const std::string &prim_location,
                              USDValidationResult *result,
                              size_t *out_joint_count) {
  std::vector<value::token> joints;
  if (!GetTokenArrayProperty(ps, "joints", &joints)) {
    AddWarning(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "Skeleton should author uniform token[] joints");
    return;
  }

  if (out_joint_count) {
    *out_joint_count = joints.size();
  }

  if (joints.empty()) {
    AddError(result, "geom.skel.skeleton.joints",
             MakePropertyLocation(prim_location, "joints"),
             "Skeleton joints must not be empty");
    return;
  }

  std::unordered_map<std::string, size_t> joint_to_index;
  size_t root_count = 0;
  for (size_t i = 0; i < joints.size(); i++) {
    const std::string joint = joints[i].str();
    if (joint.empty() || joint[0] == '/' || EndsWith(joint, "/") ||
        joint.find("//") != std::string::npos) {
      AddError(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "joints[" + std::to_string(i) +
                   "] must be a relative joint path, but is `" + joint + "`");
      continue;
    }
    if (!joint_to_index.emplace(joint, i).second) {
      AddError(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "duplicate joint path `" + joint + "`");
      continue;
    }

    const size_t slash = joint.find_last_of('/');
    if (slash == std::string::npos) {
      root_count++;
      continue;
    }

    const std::string parent = joint.substr(0, slash);
    auto parent_it = joint_to_index.find(parent);
    if (parent_it == joint_to_index.end() || parent_it->second >= i) {
      AddError(result, "geom.skel.skeleton.topology",
               MakePropertyLocation(prim_location, "joints"),
               "joint `" + joint +
                   "` must appear after its parent joint `" + parent + "`");
    }
  }

  if (root_count != 1) {
    AddWarning(result, "geom.skel.skeleton.topology",
               MakePropertyLocation(prim_location, "joints"),
               "Skeleton should have one root joint, but has " +
                   std::to_string(root_count));
  }

  const Attribute *joint_names = GetAttribute(ps, "jointNames");
  if (joint_names) {
    size_t count = 0;
    if (GetArrayLength(*joint_names, &count) && count != joints.size()) {
      AddError(result, "geom.skel.skeleton.jointNames",
               MakePropertyLocation(prim_location, "jointNames"),
               "jointNames has " + std::to_string(count) +
                   " elements, but joints has " +
                   std::to_string(joints.size()) + " elements");
    }
  }

  const std::array<const char *, 2> transform_arrays = {"bindTransforms",
                                                       "restTransforms"};
  for (const char *prop_name : transform_arrays) {
    const Attribute *attr = GetAttribute(ps, prop_name);
    if (!attr) {
      continue;
    }
    size_t count = 0;
    if (!GetArrayLength(*attr, &count)) {
      AddWarning(result, "geom.skel.skeleton.transforms",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) +
                     " should be an authored matrix4d[] value");
      continue;
    }
    if (count != joints.size()) {
      AddError(result, "geom.skel.skeleton.transforms",
               MakePropertyLocation(prim_location, prop_name),
               std::string(prop_name) + " has " + std::to_string(count) +
                   " elements, but joints has " +
                   std::to_string(joints.size()) + " elements");
    }
    ValidateTimeSamples(*attr, "geom.skel.skeleton.transforms",
                        MakePropertyLocation(prim_location, prop_name),
                        result);
  }
}

void ValidateSkelAnimation(const PrimSpec &ps, const std::string &prim_location,
                           USDValidationResult *result) {
  std::vector<value::token> joints;
  const bool have_joints = GetTokenArrayProperty(ps, "joints", &joints);
  if (!have_joints) {
    AddWarning(result, "geom.skel.animation.joints",
               MakePropertyLocation(prim_location, "joints"),
               "SkelAnimation should author uniform token[] joints");
  }

  if (have_joints) {
    const std::array<const char *, 3> trs_arrays = {"translations", "rotations",
                                                   "scales"};
    for (const char *prop_name : trs_arrays) {
      const Attribute *attr = GetAttribute(ps, prop_name);
      if (!attr) {
        continue;
      }
      ValidateArrayLengthAtAllSamples(
          *attr, prop_name, "geom.skel.animation.transforms",
          MakePropertyLocation(prim_location, prop_name), joints.size(),
          "joints", result);
    }
  }

  std::vector<value::token> blend_shapes;
  if (GetTokenArrayProperty(ps, "blendShapes", &blend_shapes)) {
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < blend_shapes.size(); i++) {
      const std::string name = blend_shapes[i].str();
      if (name.empty()) {
        AddError(result, "geom.skel.animation.blendShapes",
                 MakePropertyLocation(prim_location, "blendShapes"),
                 "blendShapes[" + std::to_string(i) + "] must not be empty");
      } else if (!seen.emplace(name).second) {
        AddError(result, "geom.skel.animation.blendShapes",
                 MakePropertyLocation(prim_location, "blendShapes"),
                 "duplicate blendShape token `" + name + "`");
      }
    }

    const Attribute *weights = GetAttribute(ps, "blendShapeWeights");
    if (weights) {
      ValidateArrayLengthAtAllSamples(
          *weights, "blendShapeWeights", "geom.skel.animation.blendShapes",
          MakePropertyLocation(prim_location, "blendShapeWeights"),
          blend_shapes.size(), "blendShapes", result);
    }
  } else if (GetAttribute(ps, "blendShapeWeights")) {
    AddWarning(result, "geom.skel.animation.blendShapes",
               MakePropertyLocation(prim_location, "blendShapeWeights"),
               "blendShapeWeights is authored without uniform token[] "
               "blendShapes");
  }
}

void ValidateBlendShape(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  const Attribute *offsets = GetAttribute(ps, "offsets");
  size_t offset_count = 0;
  bool have_offsets = false;
  if (!offsets) {
    AddError(result, "geom.skel.blendShape.offsets",
             MakePropertyLocation(prim_location, "offsets"),
             "BlendShape must author uniform vector3f[] offsets");
  } else if (!GetArrayLength(*offsets, &offset_count)) {
    AddError(result, "geom.skel.blendShape.offsets",
             MakePropertyLocation(prim_location, "offsets"),
             "BlendShape offsets must be an authored array value");
  } else {
    have_offsets = true;
    if (offset_count == 0) {
      AddWarning(result, "geom.skel.blendShape.offsets",
                 MakePropertyLocation(prim_location, "offsets"),
                 "BlendShape offsets should not be empty");
    }
  }

  const Attribute *normal_offsets = GetAttribute(ps, "normalOffsets");
  if (normal_offsets) {
    ValidateTimeSamples(*normal_offsets, "geom.skel.blendShape.normalOffsets",
                        MakePropertyLocation(prim_location, "normalOffsets"),
                        result);
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          *normal_offsets, "normalOffsets",
          "geom.skel.blendShape.normalOffsets",
          MakePropertyLocation(prim_location, "normalOffsets"), offset_count,
          "offsets", result);
    }
  }

  const Attribute *point_indices = GetAttribute(ps, "pointIndices");
  if (point_indices) {
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          *point_indices, "pointIndices", "geom.skel.blendShape.pointIndices",
          MakePropertyLocation(prim_location, "pointIndices"), offset_count,
          "offsets", result);
    }

    std::vector<int32_t> indices;
    if (GetIntArray(*point_indices, &indices)) {
      std::unordered_set<int32_t> seen;
      for (size_t i = 0; i < indices.size(); i++) {
        if (indices[i] < 0) {
          AddError(result, "geom.skel.blendShape.pointIndices",
                   MakePropertyLocation(prim_location, "pointIndices"),
                   "pointIndices[" + std::to_string(i) +
                       "] must be non-negative");
        } else if (!seen.emplace(indices[i]).second) {
          AddWarning(result, "geom.skel.blendShape.pointIndices",
                     MakePropertyLocation(prim_location, "pointIndices"),
                     "duplicate pointIndices value " +
                         std::to_string(indices[i]));
        }
      }
    }
  }

  for (const auto &prop_entry : ps.props()) {
    const std::string &prop_name = prop_entry.first;
    if (prop_name.rfind("inbetweens:", 0) != 0) {
      continue;
    }
    const std::string location = MakePropertyLocation(prim_location, prop_name);
    if (!prop_entry.second.is_attribute()) {
      AddError(result, "geom.skel.blendShape.inbetween", location,
               "BlendShape inbetween `" + prop_name +
                   "` must be an attribute");
      continue;
    }

    const Attribute &attr = prop_entry.second.get_attribute();
    if (!attr.metas().has_weight()) {
      AddError(result, "geom.skel.blendShape.inbetween", location,
               "BlendShape inbetween `" + prop_name +
                   "` must author weight metadata");
    } else if (!std::isfinite(attr.metas().get_weight())) {
      AddError(result, "geom.skel.blendShape.inbetween", location,
               "BlendShape inbetween `" + prop_name +
                   "` has a non-finite weight");
    }
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          attr, prop_name, "geom.skel.blendShape.inbetween", location,
          offset_count, "offsets", result);
    }
  }
}

void ValidateSkelBinding(const PrimSpec &ps, const std::string &prim_location,
                         const PrimTypeByPath &prim_types,
                         USDValidationResult *result) {
  const Property *skeleton = GetProperty(ps, "skel:skeleton");
  if (skeleton) {
    ValidateRelationshipTargetPaths(*skeleton, "geom.skel.binding.skeleton",
                                    MakePropertyLocation(prim_location,
                                                         "skel:skeleton"),
                                    result);
    if (skeleton->is_relationship()) {
      for (const Path &target : skeleton->get_relationTargets()) {
        const std::string *target_type =
            FindPrimType(prim_types, target.prim_part());
        if (target_type && *target_type != "Skeleton") {
          AddWarning(result, "geom.skel.binding.skeleton",
                     MakePropertyLocation(prim_location, "skel:skeleton"),
                     "skel:skeleton target `" + target.full_path_name() +
                         "` has type `" + *target_type +
                         "`, expected Skeleton");
        }
      }
    }
  }

  const Property *animation_source = GetProperty(ps, "skel:animationSource");
  if (animation_source) {
    ValidateRelationshipTargetPaths(
        *animation_source, "geom.skel.binding.animationSource",
        MakePropertyLocation(prim_location, "skel:animationSource"), result);
    if (animation_source->is_relationship()) {
      for (const Path &target : animation_source->get_relationTargets()) {
        const std::string *target_type =
            FindPrimType(prim_types, target.prim_part());
        if (target_type && *target_type != "SkelAnimation") {
          AddWarning(result, "geom.skel.binding.animationSource",
                     MakePropertyLocation(prim_location,
                                          "skel:animationSource"),
                     "skel:animationSource target `" +
                         target.full_path_name() + "` has type `" +
                         *target_type + "`, expected SkelAnimation");
        }
      }
    }
  }
}

void ValidateSkinningPrimvars(const PrimSpec &ps, const std::string &prim_location,
                              const PrimTypeByPath &prim_types,
                              const SkeletonJointCountByPath &skeleton_joints,
                              USDValidationResult *result) {
  const Attribute *indices_attr =
      GetAttribute(ps, "primvars:skel:jointIndices");
  const Attribute *weights_attr =
      GetAttribute(ps, "primvars:skel:jointWeights");
  if (!indices_attr && !weights_attr) {
    return;
  }

  if (!indices_attr || !weights_attr) {
    AddError(result, "geom.skel.skinning.pair", prim_location,
             "primvars:skel:jointIndices and primvars:skel:jointWeights must "
             "be authored together");
    return;
  }

  const auto validate_skin_interp = [&](const Attribute &attr,
                                        const std::string &name) {
    const std::string location = MakePropertyLocation(prim_location, name);
    if (!attr.metas().has_interpolation()) {
      AddError(result, "geom.skel.skinning.interpolation", location,
               name + " must author interpolation metadata");
      return;
    }
    const std::string interp = attr.metas().get_interpolation().str();
    if (interp != "vertex" && interp != "varying") {
      AddError(result, "geom.skel.skinning.interpolation", location,
               name + " interpolation must be vertex or varying, but is `" +
                   interp + "`");
    }
  };
  validate_skin_interp(*indices_attr, "primvars:skel:jointIndices");
  validate_skin_interp(*weights_attr, "primvars:skel:jointWeights");

  if (!indices_attr->metas().has_elementSize() ||
      indices_attr->metas().get_elementSize() == 0 ||
      !weights_attr->metas().has_elementSize() ||
      weights_attr->metas().get_elementSize() == 0) {
    AddError(result, "geom.skel.skinning.elementSize", prim_location,
             "skinning primvars must author non-zero elementSize metadata");
  } else if (indices_attr->metas().get_elementSize() !=
             weights_attr->metas().get_elementSize()) {
    AddError(result, "geom.skel.skinning.elementSize", prim_location,
             "primvars:skel:jointIndices and primvars:skel:jointWeights must "
             "use the same elementSize");
  }

  std::vector<int32_t> joint_indices;
  std::vector<float> joint_weights;
  if (GetIntArray(*indices_attr, &joint_indices) &&
      GetFloatArrayProperty(ps, "primvars:skel:jointWeights", &joint_weights)) {
    if (joint_indices.size() != joint_weights.size()) {
      AddError(result, "geom.skel.skinning.size", prim_location,
               "jointIndices has " + std::to_string(joint_indices.size()) +
                   " elements, but jointWeights has " +
                   std::to_string(joint_weights.size()) + " elements");
    }

    size_t local_joint_count = 0;
    bool have_local_joint_count = false;
    const Property *skeleton = GetProperty(ps, "skel:skeleton");
    if (skeleton && skeleton->is_relationship()) {
      const std::vector<Path> targets = skeleton->get_relationTargets();
      if (targets.size() == 1) {
        const std::string *target_type =
            FindPrimType(prim_types, targets[0].prim_part());
        if (target_type && *target_type == "Skeleton") {
          auto joint_count_it =
              skeleton_joints.find(targets[0].prim_part());
          if (joint_count_it != skeleton_joints.end()) {
            local_joint_count = joint_count_it->second;
            have_local_joint_count = true;
          }
        }
      }
    }

    for (size_t i = 0; i < joint_indices.size(); i++) {
      if (joint_indices[i] < 0) {
        AddError(result, "geom.skel.skinning.index",
                 MakePropertyLocation(prim_location,
                                      "primvars:skel:jointIndices"),
                 "jointIndices[" + std::to_string(i) +
                     "] must be non-negative");
      } else if (have_local_joint_count &&
                 static_cast<size_t>(joint_indices[i]) >= local_joint_count) {
        AddError(result, "geom.skel.skinning.index",
                 MakePropertyLocation(prim_location,
                                      "primvars:skel:jointIndices"),
                 "jointIndices[" + std::to_string(i) + "] references joint " +
                     std::to_string(joint_indices[i]) + ", but skeleton has " +
                     std::to_string(local_joint_count) + " joints");
      }
    }

    if (weights_attr->metas().has_elementSize()) {
      const size_t element_size = weights_attr->metas().get_elementSize();
      if (element_size != 0 && (joint_weights.size() % element_size) == 0) {
        for (size_t group = 0; group < joint_weights.size() / element_size;
             group++) {
          double sum = 0.0;
          bool all_finite = true;
          for (size_t item = 0; item < element_size; item++) {
            const float weight = joint_weights[group * element_size + item];
            if (!std::isfinite(weight)) {
              all_finite = false;
            }
            sum += static_cast<double>(weight);
          }
          if (!all_finite) {
            AddError(result, "geom.skel.skinning.weight",
                     MakePropertyLocation(prim_location,
                                          "primvars:skel:jointWeights"),
                     "jointWeights group " + std::to_string(group) +
                         " contains a non-finite value");
          } else if (sum <= 0.0) {
            AddWarning(result, "geom.skel.skinning.weight",
                       MakePropertyLocation(prim_location,
                                            "primvars:skel:jointWeights"),
                       "jointWeights group " + std::to_string(group) +
                           " has no positive influence");
          } else if (std::fabs(sum - 1.0) > 1.0e-3) {
            AddWarning(result, "geom.skel.skinning.weight",
                       MakePropertyLocation(prim_location,
                                            "primvars:skel:jointWeights"),
                       "jointWeights group " + std::to_string(group) +
                           " sums to " + std::to_string(sum) +
                           " instead of 1");
          }
        }
      }
    }
  }
}

void ValidateLayerOffset(const LayerOffset &offset, const std::string &rule_id,
                         const std::string &location,
                         USDValidationResult *result) {
  if (offset._scale == 0.0) {
    AddError(result, rule_id, location,
             "composition layerOffset scale must not be 0");
  }
}

void ValidateReferenceList(
    const std::vector<std::pair<ListEditQual, std::vector<Reference>>> &ops,
    const std::string &prim_location, USDValidationResult *result) {
  for (const auto &op : ops) {
    if (op.first == ListEditQual::Invalid) {
      AddError(result, "core.composition.reference", prim_location,
               "references has an invalid list-edit qualifier");
    }
    for (const Reference &ref : op.second) {
      const std::string asset_path = ref.asset_path.GetAssetPath();
      if (asset_path.empty() && !ref.prim_path.is_valid()) {
        AddError(result, "core.composition.reference", prim_location,
                 "reference must author an asset path or prim path");
      }
      if (ref.prim_path.is_valid()) {
        std::string err;
        if (!IsValidAbsolutePrimTargetPath(ref.prim_path, &err)) {
          AddError(result, "core.composition.reference", prim_location,
                   "reference primPath `" + ref.prim_path.full_path_name() +
                       "` is invalid: " + err);
        }
      }
      ValidateLayerOffset(ref.layerOffset, "core.composition.reference",
                          prim_location, result);
    }
  }
}

void ValidatePayloadList(
    const std::vector<std::pair<ListEditQual, std::vector<Payload>>> &ops,
    const std::string &prim_location, USDValidationResult *result) {
  for (const auto &op : ops) {
    if (op.first == ListEditQual::Invalid) {
      AddError(result, "core.composition.payload", prim_location,
               "payload has an invalid list-edit qualifier");
    }
    for (const Payload &payload : op.second) {
      if (payload.is_none()) {
        continue;
      }
      if (payload.asset_path.GetAssetPath().empty() &&
          !payload.prim_path.is_valid()) {
        AddError(result, "core.composition.payload", prim_location,
                 "payload must author an asset path or prim path");
      }
      if (payload.prim_path.is_valid()) {
        std::string err;
        if (!IsValidAbsolutePrimTargetPath(payload.prim_path, &err)) {
          AddError(result, "core.composition.payload", prim_location,
                   "payload primPath `" +
                       payload.prim_path.full_path_name() +
                       "` is invalid: " + err);
        }
      }
      ValidateLayerOffset(payload.layerOffset, "core.composition.payload",
                          prim_location, result);
    }
  }
}

void ValidatePathListOp(
    const std::vector<std::pair<ListEditQual, std::vector<Path>>> &ops,
    const std::string &rule_id, const std::string &prim_location,
    const PrimSpecifierByPath &prim_specifiers, USDValidationResult *result,
    bool expect_class_target) {
  for (const auto &op : ops) {
    if (op.first == ListEditQual::Invalid) {
      AddError(result, rule_id, prim_location,
               "composition arc has an invalid list-edit qualifier");
    }
    for (const Path &target : op.second) {
      std::string err;
      if (!IsValidAbsolutePrimTargetPath(target, &err)) {
        AddError(result, rule_id, prim_location,
                 "target `" + target.full_path_name() +
                     "` is invalid: " + err);
        continue;
      }
      if (target.full_path_name() == prim_location) {
        AddError(result, rule_id, prim_location,
                 "composition arc must not target the prim itself");
      }
      const Specifier *target_spec =
          FindPrimSpecifier(prim_specifiers, target.full_path_name());
      if (expect_class_target && target_spec &&
          *target_spec != Specifier::Class) {
        AddWarning(result, rule_id, prim_location,
                   "local target `" + target.full_path_name() +
                       "` is not a class prim");
      }
    }
  }
}

void ValidateVariantSetList(
    const std::vector<std::pair<ListEditQual, std::vector<std::string>>> &ops,
    const std::string &prim_location, USDValidationResult *result) {
  for (const auto &op : ops) {
    if (op.first == ListEditQual::Invalid) {
      AddError(result, "core.composition.variantSets", prim_location,
               "variantSets has an invalid list-edit qualifier");
    }
    for (const std::string &name : op.second) {
      if (name.empty() || !is_valid_utf8_identifier(name)) {
        AddError(result, "core.composition.variantSets", prim_location,
                 "variant set name `" + name +
                     "` is not a valid identifier");
      }
    }
  }
}

void ValidateVariantDefinitionsAndSelections(
    const PrimSpec &ps, const PrimMeta &metas, const std::string &prim_location,
    USDValidationResult *result) {
  for (const auto &variant_set_entry : ps.variantSets()) {
    const std::string &set_name = variant_set_entry.first;
    const VariantSetSpec &variant_set = variant_set_entry.second;
    if (set_name.empty() || !is_valid_utf8_identifier(set_name)) {
      AddError(result, "core.composition.variantSet", prim_location,
               "variant set statement name `" + set_name +
                   "` is not a valid identifier");
    }
    if (!variant_set.name.empty() && variant_set.name != set_name) {
      AddError(result, "core.composition.variantSet", prim_location,
               "variant set statement key `" + set_name +
                   "` does not match stored name `" + variant_set.name + "`");
    }
    if (variant_set.variantSet.empty()) {
      AddWarning(result, "core.composition.variantSet", prim_location,
                 "variant set `" + set_name + "` has no variants");
    }
    for (const auto &variant_entry : variant_set.variantSet) {
      const std::string &variant_name = variant_entry.first;
      if (variant_name.empty() || !is_valid_utf8_identifier(variant_name)) {
        AddError(result, "core.composition.variantSet", prim_location,
                 "variant name `" + variant_name + "` in set `" + set_name +
                     "` is not a valid identifier");
      }
    }
  }

  if (!metas.variants) {
    return;
  }

  for (const auto &selection : metas.variants.value()) {
    const std::string &set_name = selection.first;
    const std::string &variant_name = selection.second;
    if (set_name.empty() || !is_valid_utf8_identifier(set_name)) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection set name `" + set_name +
                   "` is not a valid identifier");
    }
    if (variant_name.empty() || !is_valid_utf8_identifier(variant_name)) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection `" + variant_name + "` for set `" +
                   set_name + "` is not a valid identifier");
      continue;
    }

    const auto set_it = ps.variantSets().find(set_name);
    if (set_it != ps.variantSets().end() &&
        set_it->second.variantSet.find(variant_name) ==
            set_it->second.variantSet.end()) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection `" + variant_name + "` is not authored in "
                   "local variant set `" +
                   set_name + "`");
    }
  }
}

bool GetMetaAssetPath(const Dictionary &dict, const std::string &key,
                      std::string *out);
bool GetMetaString(const Dictionary &dict, const std::string &key,
                   std::string *out);
bool GetMetaAssetPathArray(const Dictionary &dict, const std::string &key,
                           std::vector<std::string> *out);
void ValidateDictionaryKeys(const Dictionary &dict, const std::string &rule_id,
                            const std::string &location,
                            USDValidationResult *result);

void ValidateCompositionMetadata(const PrimSpec &ps,
                                 const std::string &prim_location,
                                 const PrimSpecifierByPath &prim_specifiers,
                                 USDValidationResult *result) {
  const PrimMeta &metas = ps.metas();
  if (metas.references) {
    ValidateReferenceList(metas.references.value(), prim_location, result);
  }
  if (metas.payload) {
    ValidatePayloadList(metas.payload.value(), prim_location, result);
  }
  if (metas.inherits) {
    ValidatePathListOp(metas.inherits.value(), "core.composition.inherits",
                       prim_location, prim_specifiers, result,
                       true);
  }
  if (metas.specializes) {
    ValidatePathListOp(metas.specializes.value(),
                       "core.composition.specializes", prim_location,
                       prim_specifiers, result, true);
  }
  if (metas.variantSets) {
    ValidateVariantSetList(metas.variantSets.value(), prim_location, result);
  }
  ValidateVariantDefinitionsAndSelections(ps, metas, prim_location, result);

  if (ps.specifier() == Specifier::Over && ps.typeName().empty() &&
      !HasCompositionArc(ps) && ps.props().empty() && ps.children().empty()) {
    AddWarning(result, "core.composition.over", prim_location,
               "empty typeless over has no local opinions or composition arcs");
  }
}

void ValidateAPISchemasMetadata(const PrimMeta &metas,
                                const std::string &prim_location,
                                USDValidationResult *result) {
  if (!metas.has_apiSchemas()) {
    return;
  }

  const APISchemas api_schemas = metas.get_apiSchemas();
  std::set<std::string> seen;
  const auto validate_schema = [&](const std::string &schema_name,
                                   const std::string &instance_name,
                                   const std::string &rule_id,
                                   bool unknown) {
    if (schema_name.empty() || !is_valid_utf8_identifier(schema_name)) {
      AddError(result, rule_id, prim_location,
               "apiSchemas entry `" + schema_name +
                   "` is not a valid schema identifier");
    }
    if (!instance_name.empty() &&
        !IsValidNamespacedIdentifier(instance_name)) {
      AddError(result, "core.apiSchema.instance", prim_location,
               "apiSchemas instance `" + instance_name +
                   "` is not a valid namespaced identifier");
    }
    if (!unknown && IsMultipleApplySchemaName(schema_name) &&
        instance_name.empty()) {
      AddError(result, "core.apiSchema.instance", prim_location,
               schema_name + " is a multiple-apply API schema and requires "
                             "an instance name");
    }
    if (!unknown && IsSingleApplySchemaName(schema_name) &&
        !instance_name.empty()) {
      AddError(result, "core.apiSchema.instance", prim_location,
               schema_name +
                   " is a single-apply API schema and must not author an "
                   "instance name");
    }
    const std::string key = schema_name + ":" + instance_name;
    if (!seen.insert(key).second) {
      AddWarning(result, "core.apiSchema.duplicate", prim_location,
                 "duplicate apiSchemas entry `" + key + "`");
    }
    if (unknown) {
      AddWarning(result, "core.apiSchema.unknown", prim_location,
                 "unknown API schema `" + schema_name + "`");
    }
  };

  for (const auto &entry : api_schemas.names) {
    validate_schema(to_string(entry.first), entry.second,
                    "core.apiSchema.name", false);
  }
  for (const auto &entry : api_schemas.unknownSchemas) {
    std::string schema_name = entry.first;
    std::string instance_name = entry.second;
    const size_t colon_pos = schema_name.find(':');
    if (colon_pos != std::string::npos) {
      if (instance_name.empty()) {
        instance_name = schema_name.substr(colon_pos + 1);
      }
      schema_name = schema_name.substr(0, colon_pos);
    }
    const bool unknown = !enum_handler::APISchemaNameOpt(schema_name);
    validate_schema(schema_name, instance_name, "core.apiSchema.name",
                    unknown);
  }
}

void ValidatePrimMetadata(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result) {
  const PrimMeta &metas = ps.metas();

  if (metas.has_kind()) {
    const std::string kind = metas.get_kind_str();
    if (kind.empty() || !IsValidNamespacedIdentifier(kind)) {
      AddError(result, "core.prim.kind", prim_location,
               "kind `" + kind + "` is not a valid token");
    }
  }

  if (metas.has_instanceable() && metas.get_instanceable() &&
      !metas.references && !metas.payload && ps.children().empty()) {
    AddWarning(result, "core.prim.instanceable", prim_location,
               "instanceable prim should usually compose a prototype via "
               "references or payloads, or contain authored children");
  }

  if (metas.has_customData()) {
    ValidateDictionaryKeys(metas.get_customData(), "core.prim.customData",
                           prim_location + ".customData", result);
  }
  if (metas.has_assetInfo()) {
    const Dictionary asset_info = metas.get_assetInfo();
    ValidateDictionaryKeys(asset_info, "core.prim.assetInfo",
                           prim_location + ".assetInfo", result);

    std::string identifier;
    if (GetMetaAssetPath(asset_info, "identifier", &identifier) &&
        identifier.empty()) {
      AddError(result, "core.prim.assetInfo",
               prim_location + ".assetInfo.identifier",
               "assetInfo identifier must not be empty");
    }
    std::string name;
    if (GetMetaString(asset_info, "name", &name) && name.empty()) {
      AddWarning(result, "core.prim.assetInfo",
                 prim_location + ".assetInfo.name",
                 "assetInfo name should not be empty");
    }
    std::vector<std::string> deps;
    if (GetMetaAssetPathArray(asset_info, "payloadAssetDependencies", &deps)) {
      for (size_t i = 0; i < deps.size(); i++) {
        if (deps[i].empty()) {
          AddError(result, "core.prim.assetInfo",
                   prim_location + ".assetInfo.payloadAssetDependencies",
                   "payloadAssetDependencies[" + std::to_string(i) +
                       "] must not be empty");
        }
      }
    }
  }

  ValidateAPISchemasMetadata(metas, prim_location, result);
}

void ValidateMaterialXReferenceConventions(const PrimSpec &ps,
                                           const std::string &prim_location,
                                           USDValidationResult *result) {
  if (!ps.metas().references) {
    return;
  }

  for (const auto &op : ps.metas().references.value()) {
    for (const Reference &ref : op.second) {
      const std::string asset_path = ref.asset_path.GetAssetPath();
      if (!IsMaterialXAssetPath(asset_path)) {
        continue;
      }
      if (!ref.prim_path.is_valid() ||
          ref.prim_path.full_path_name() != "/MaterialX") {
        AddWarning(result, "shade.materialX.referencePrimPath", prim_location,
                   "MaterialX references should target </MaterialX>");
      }
    }
  }
}

bool IsCanonicalColorSpaceToken(const std::string &token) {
  static const std::unordered_set<std::string> kCanonicalTokens = {
      "lin_ap1_scene",
      "lin_ap0_scene",
      "lin_rec709_scene",
      "lin_p3d65_scene",
      "lin_rec2020_scene",
      "lin_adobergb_scene",
      "lin_ciexyzd65_scene",
      "srgb_rec709_scene",
      "g22_rec709_scene",
      "g18_rec709_scene",
      "srgb_ap1_scene",
      "g22_ap1_scene",
      "srgb_p3d65_scene",
      "g22_adobergb_scene",
      "data",
      "unknown",
      "raw",
      "identity",
  };

  return kCanonicalTokens.count(token) > 0;
}

bool IsColorSpaceDefinitionPropertyName(const std::string &prop_name) {
  return prop_name == "name" || prop_name == "redChroma" ||
         prop_name == "greenChroma" || prop_name == "blueChroma" ||
         prop_name == "whitePoint" || prop_name == "gamma" ||
         prop_name == "linearBias" || prop_name == "colorSpaceDefinition:name" ||
         prop_name == "colorSpaceDefinition:redChroma" ||
         prop_name == "colorSpaceDefinition:greenChroma" ||
         prop_name == "colorSpaceDefinition:blueChroma" ||
         prop_name == "colorSpaceDefinition:whitePoint" ||
         prop_name == "colorSpaceDefinition:gamma" ||
         prop_name == "colorSpaceDefinition:linearBias";
}

bool ParseColorSpaceDefinitionPropertyName(const std::string &prop_name,
                                           std::string *instance,
                                           std::string *field) {
  const std::vector<std::string> segments = SplitString(prop_name, ':');
  if (segments.size() == 3 && segments[0] == "colorSpaceDefinition" &&
      !segments[1].empty() && !segments[2].empty() &&
      IsColorSpaceDefinitionPropertyName(segments[2])) {
    if (instance) *instance = segments[1];
    if (field) *field = segments[2];
    return true;
  }
  return false;
}

std::vector<AppliedSchema> CollectAppliedSchemas(const PrimMeta &metas) {
  std::vector<AppliedSchema> schemas;
  if (!metas.has_apiSchemas()) {
    return schemas;
  }

  const APISchemas api_schemas = metas.get_apiSchemas();
  for (const auto &entry : api_schemas.names) {
    // Emit the canonical schema name for every known applied schema so callers
    // (e.g. MaterialBindingAPI / CollectionAPI checks) can look it up by name.
    schemas.push_back({to_string(entry.first), entry.second});
  }

  for (const auto &entry : api_schemas.unknownSchemas) {
    std::string schema_name = entry.first;
    std::string instance_name = entry.second;

    const size_t colon_pos = schema_name.find(':');
    if (colon_pos != std::string::npos) {
      if (instance_name.empty()) {
        instance_name = schema_name.substr(colon_pos + 1);
      }
      schema_name = schema_name.substr(0, colon_pos);
    }

    schemas.push_back({schema_name, instance_name});
  }

  return schemas;
}

bool HasAppliedSchema(const std::vector<AppliedSchema> &schemas,
                      const std::string &schema_name) {
  for (const auto &schema : schemas) {
    if (schema.name == schema_name) {
      return true;
    }
  }
  return false;
}

std::set<std::string> CollectAppliedCollectionInstances(
    const std::vector<AppliedSchema> &schemas) {
  std::set<std::string> instances;
  for (const auto &schema : schemas) {
    if (schema.name == "CollectionAPI" && !schema.instance_name.empty()) {
      instances.insert(schema.instance_name);
    }
  }
  return instances;
}

void ValidateTokenAttributeType(const Property &prop, const std::string &rule_id,
                                const std::string &location,
                                USDValidationResult *result) {
  if (!prop.is_attribute()) {
    AddError(result, rule_id, location,
             "expected an attribute, but found a relationship");
    return;
  }

  if (prop.value_type_name() != "token") {
    AddError(result, rule_id, location,
             "expected attribute type `token`, but found `" +
                 prop.value_type_name() + "`");
  }
}

void ValidateFloat2AttributeType(const Property &prop, const std::string &rule_id,
                                 const std::string &location,
                                 USDValidationResult *result) {
  if (!prop.is_attribute()) {
    AddError(result, rule_id, location,
             "expected an attribute, but found a relationship");
    return;
  }

  if (prop.value_type_name() != "float2") {
    AddError(result, rule_id, location,
             "expected attribute type `float2`, but found `" +
                 prop.value_type_name() + "`");
  }
}

void ValidateFloatAttributeType(const Property &prop, const std::string &rule_id,
                                const std::string &location,
                                USDValidationResult *result) {
  if (!prop.is_attribute()) {
    AddError(result, rule_id, location,
             "expected an attribute, but found a relationship");
    return;
  }

  if (prop.value_type_name() != "float") {
    AddError(result, rule_id, location,
             "expected attribute type `float`, but found `" +
                 prop.value_type_name() + "`");
  }
}

void ValidateBoolAttributeType(const Property &prop, const std::string &rule_id,
                               const std::string &location,
                               USDValidationResult *result) {
  if (!prop.is_attribute()) {
    AddError(result, rule_id, location,
             "expected an attribute, but found a relationship");
    return;
  }

  if (prop.value_type_name() != "bool") {
    AddError(result, rule_id, location,
             "expected attribute type `bool`, but found `" +
                 prop.value_type_name() + "`");
  }
}

void ValidateUniformAttribute(const Property &prop, const std::string &rule_id,
                              const std::string &location,
                              USDValidationResult *result) {
  if (!prop.is_attribute()) {
    return;
  }

  const Attribute &attr = prop.get_attribute();
  if (attr.variability() != Variability::Uniform) {
    AddError(result, rule_id, location,
             "expected `uniform` variability");
  }
}

void ValidateVaryingAttribute(const Property &prop, const std::string &rule_id,
                              const std::string &location,
                              USDValidationResult *result) {
  if (!prop.is_attribute()) {
    return;
  }

  const Attribute &attr = prop.get_attribute();
  if (attr.variability() != Variability::Varying) {
    AddError(result, rule_id, location,
             "expected `varying` variability");
  }
}

void ValidateAttributeMetadata(const Attribute &attr,
                               const std::string &prop_name,
                               const std::string &location,
                               USDValidationResult *result) {
  const AttrMeta &metas = attr.metas();

  if (metas.has_customData()) {
    ValidateDictionaryKeys(metas.get_customData(), "core.attr.customData",
                           location + ".customData", result);
  }
  if (metas.has_sdrMetadata()) {
    ValidateDictionaryKeys(metas.get_sdrMetadata(), "core.attr.sdrMetadata",
                           location + ".sdrMetadata", result);
  }

  if (metas.has_interpolation()) {
    const std::string interp = metas.get_interpolation().str();
    if (!IsValidPrimvarInterpolation(interp)) {
      AddError(result, "core.attr.interpolation", location,
               "invalid interpolation token `" + interp + "`");
    }
  }
  if (metas.has_elementSize() && metas.get_elementSize() == 0) {
    AddError(result, "core.attr.elementSize", location,
             "elementSize must be greater than 0");
  }
  if (metas.has_unauthoredValuesIndex() &&
      metas.get_unauthoredValuesIndex() < -1) {
    AddError(result, "core.attr.unauthoredValuesIndex", location,
             "unauthoredValuesIndex must be -1 or greater");
  }

  if (metas.has_allowedTokens()) {
    const std::vector<value::token> allowed = metas.get_allowedTokens();
    if (allowed.empty()) {
      AddWarning(result, "core.attr.allowedTokens", location,
                 "allowedTokens should not be empty when authored");
    }

    std::set<std::string> allowed_set;
    for (size_t i = 0; i < allowed.size(); i++) {
      const std::string token = allowed[i].str();
      if (token.empty()) {
        AddError(result, "core.attr.allowedTokens", location,
                 "allowedTokens[" + std::to_string(i) +
                     "] must not be empty");
      } else if (!allowed_set.insert(token).second) {
        AddWarning(result, "core.attr.allowedTokens", location,
                   "duplicate allowedTokens entry `" + token + "`");
      }
    }

    auto token_value = attr.get_value<value::token>();
    if (token_value && !allowed_set.empty() &&
        allowed_set.count(token_value->str()) == 0) {
      AddError(result, "core.attr.allowedTokens", location,
               prop_name + " value `" + token_value->str() +
                   "` is not listed in allowedTokens");
    }

    auto token_array = attr.get_value<std::vector<value::token>>();
    if (token_array && !allowed_set.empty()) {
      for (size_t i = 0; i < token_array->size(); i++) {
        const std::string token = token_array.value()[i].str();
        if (allowed_set.count(token) == 0) {
          AddError(result, "core.attr.allowedTokens", location,
                   prop_name + "[" + std::to_string(i) + "] value `" +
                       token + "` is not listed in allowedTokens");
        }
      }
    }
  }

  if (metas.has_connectability()) {
    const std::string connectability = metas.get_connectability().str();
    if (connectability != "full" && connectability != "interfaceOnly") {
      AddError(result, "core.attr.connectability", location,
               "connectability must be `full` or `interfaceOnly`, but is `" +
                   connectability + "`");
    }
  }
  if (metas.has_renderType() && metas.get_renderType().str().empty()) {
    AddWarning(result, "core.attr.renderType", location,
               "renderType should not be empty when authored");
  }
  if (metas.has_outputName() && metas.get_outputName().str().empty()) {
    AddWarning(result, "core.attr.outputName", location,
               "outputName should not be empty when authored");
  }
}

void ValidateRelationshipMetadata(const Relationship &rel,
                                  const std::string &location,
                                  USDValidationResult *result) {
  if (rel.is_varying_authored()) {
    AddWarning(result, "core.relationship.variability", location,
               "relationships are implicitly uniform; authored varying "
               "variability is ignored");
  }

  const AttrMeta &metas = rel.metas();
  if (metas.has_customData()) {
    ValidateDictionaryKeys(metas.get_customData(), "core.relationship.customData",
                           location + ".customData", result);
  }
  if (metas.has_bindMaterialAs()) {
    const std::string value = metas.get_bindMaterialAs().str();
    if (value != "strongerThanDescendants" &&
        value != "weakerThanDescendants") {
      AddError(result, "core.relationship.bindMaterialAs", location,
               "bindMaterialAs must be `strongerThanDescendants` or "
               "`weakerThanDescendants`, but is `" +
                   value + "`");
    }
  }
}

void ValidateRelationshipTargets(const Relationship &relationship,
                                 const std::string &rule_id,
                                 const std::string &location,
                                 USDValidationResult *result) {
  for (const Path &target : Property(relationship).get_relationTargets()) {
    std::string err;
    if (!ValidateScenePath(target, &err)) {
      AddError(result, rule_id, location,
               "contains invalid target path `" + target.full_path_name() +
                   "`: " + err);
    }
  }
}

ColorSpaceSet CollectLocalColorSpaceDefinitions(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas) {
  ColorSpaceSet local_defs;

  if (!HasAppliedSchema(applied_schemas, "ColorSpaceDefinitionAPI")) {
    return local_defs;
  }

  const auto token_value = [&](const std::string &prop_name,
                               std::string *out) {
    auto it = ps.props().find(prop_name);
    if (it == ps.props().end() || !it->second.is_attribute()) return false;
    auto value = it->second.get_attribute().get_value<value::token>();
    if (!value || value->str().empty()) return false;
    if (out) *out = value->str();
    return true;
  };
  for (const AppliedSchema &schema : applied_schemas) {
    if (schema.name != "ColorSpaceDefinitionAPI") continue;
    if (schema.instance_name.empty()) {
      local_defs.insert("custom");
      for (const char *name_prop : {"name", "colorSpaceDefinition:name"}) {
        std::string value;
        if (token_value(name_prop, &value)) {
          local_defs.insert(value);
          local_defs.insert(color::CanonicalizeToken(value));
        }
      }
    } else {
      local_defs.insert(schema.instance_name);
      std::string value;
      if (token_value("colorSpaceDefinition:" + schema.instance_name +
                          ":name",
                      &value)) {
        local_defs.insert(value);
        local_defs.insert(color::CanonicalizeToken(value));
      }
    }
  }

  return local_defs;
}

bool IsKnownColorSpaceToken(const std::string &token,
                            const ColorSpaceSet &visible_custom_spaces) {
  if (token.empty()) {
    return true;
  }

  const std::string canonical = color::CanonicalizeToken(token);
  if (IsCanonicalColorSpaceToken(canonical)) {
    return true;
  }

  return visible_custom_spaces.count(token) > 0 ||
         visible_custom_spaces.count(canonical) > 0;
}

void ValidateLocalColorSpaceDefinitions(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &location, USDValidationResult *result) {
  std::map<std::string, std::string> names;
  for (const AppliedSchema &schema : applied_schemas) {
    if (schema.name != "ColorSpaceDefinitionAPI") continue;
    const bool legacy = schema.instance_name.empty();
    const std::string prefix = legacy
        ? std::string()
        : "colorSpaceDefinition:" + schema.instance_name + ":";
    std::string name = legacy ? "custom" : schema.instance_name;
    const Attribute *name_attr = GetAttribute(ps, prefix + "name");
    if (name_attr && !name_attr->is_connection() && !name_attr->is_blocked()) {
      auto token = name_attr->get_value<value::token>();
      if (token && !token->str().empty()) name = token->str();
    }
    const std::string canonical = color::CanonicalizeToken(name);
    auto inserted = names.emplace(canonical, schema.instance_name);
    if (!inserted.second) {
      AddError(result, "core.schema.ColorSpaceDefinitionAPI.duplicate",
               location, "multiple definitions resolve to color space `" +
                             canonical + "`");
    }

    const value::float2 r_default{1.0f, 0.0f};
    const value::float2 g_default{0.0f, 1.0f};
    const value::float2 b_default{0.0f, 0.0f};
    const value::float2 w_default{1.0f / 3.0f, 1.0f / 3.0f};
    value::float2 r = r_default;
    value::float2 g = g_default;
    value::float2 b = b_default;
    value::float2 w = w_default;
    (void)GetFloat2Property(ps, prefix + "redChroma", &r);
    (void)GetFloat2Property(ps, prefix + "greenChroma", &g);
    (void)GetFloat2Property(ps, prefix + "blueChroma", &b);
    (void)GetFloat2Property(ps, prefix + "whitePoint", &w);
    double gamma_value = 1.0;
    double bias_value = 0.0;
    (void)GetNumericScalarProperty(ps, prefix + "gamma", &gamma_value);
    (void)GetNumericScalarProperty(ps, prefix + "linearBias", &bias_value);
    const float red[2] = {r[0], r[1]};
    const float green[2] = {g[0], g[1]};
    const float blue[2] = {b[0], b[1]};
    const float white[2] = {w[0], w[1]};
    color::ColorSpaceDesc definition;
    if (!color::MakeColorSpaceFromChromaticities(
            canonical, red, green, blue, white,
            static_cast<float>(gamma_value), static_cast<float>(bias_value),
            &definition)) {
      AddError(result, "core.schema.ColorSpaceDefinitionAPI.invalid",
               location, "definition `" + canonical +
                             "` has invalid chromaticities or transfer curve");
    }
  }
}

void ValidateColorSpaceToken(const std::string &token,
                             const ColorSpaceSet &visible_custom_spaces,
                             const std::string &rule_id,
                             const std::string &location,
                             USDValidationResult *result) {
  if (IsKnownColorSpaceToken(token, visible_custom_spaces)) {
    return;
  }

  AddError(result, rule_id, location,
           "uses unknown color space token `" + token +
               "` with no visible ColorSpaceDefinitionAPI definition");
}

void ValidateLayerMetas(const Layer &layer, USDValidationResult *result) {
  const LayerMetas &metas = layer.metas();

  if (!metas.defaultPrim.str().empty()) {
    const std::string &default_prim = metas.defaultPrim.str();
    if (!is_valid_utf8_identifier(default_prim)) {
      AddError(result, "core.layer.defaultPrim", kLayerLocation,
               "defaultPrim `" + default_prim +
                   "` is not a valid root prim identifier");
    }
    if (layer.primspecs().count(default_prim) == 0) {
      AddError(result, "core.layer.defaultPrim", kLayerLocation,
               "defaultPrim `" + default_prim +
                   "` does not match any root PrimSpec in the layer");
    }
  }

  std::set<std::string> relocated_sources;
  for (const auto &entry : metas.layerRelocates) {
    const Path &source = entry.first;
    const Path &target = entry.second;

    std::string source_err;
    if (!ValidatePrimPath(source, &source_err) || !IsAbsolutePrimPath(source) ||
        source.is_root_path()) {
      AddError(result, "core.layer.layerRelocates.source", kLayerLocation,
               "layerRelocates source `" + source.full_path_name() +
                   "` must be a valid absolute prim path");
    }

    std::string target_err;
    if ((!IsEmptyPath(target)) &&
        (!ValidatePrimPath(target, &target_err) || !IsAbsolutePrimPath(target))) {
      AddError(result, "core.layer.layerRelocates.target", kLayerLocation,
               "layerRelocates target `" + target.full_path_name() +
                   "` must be empty or a valid absolute prim path");
    }

    if (source.is_valid() && target.is_valid() &&
        source.full_path_name() == target.full_path_name()) {
      AddError(result, "core.layer.layerRelocates.identity", kLayerLocation,
               "layerRelocates source and target must not be identical");
    }
    if (!source.full_path_name().empty() &&
        !relocated_sources.insert(source.full_path_name()).second) {
      AddError(result, "core.layer.layerRelocates.source", kLayerLocation,
               "layerRelocates contains duplicate source `" +
                   source.full_path_name() + "`");
    }
  }

  // upAxis: the parser only ever stores X/Y/Z, so this is defensive against
  // other loaders leaving an Invalid axis.
  if (metas.upAxis.authored() && metas.upAxis.get_value() == Axis::Invalid) {
    AddWarning(result, "core.layer.upAxis", kLayerLocation,
               "upAxis is authored but is not one of X, Y, or Z");
  }

  // metersPerUnit must be a positive scale factor.
  if (metas.metersPerUnit.authored() && !(metas.metersPerUnit.get_value() > 0.0)) {
    AddError(result, "core.layer.metersPerUnit", kLayerLocation,
             "metersPerUnit must be greater than 0");
  }

  if (metas.timeCodesPerSecond.authored() &&
      !(metas.timeCodesPerSecond.get_value() > 0.0)) {
    AddError(result, "core.layer.timeCodesPerSecond", kLayerLocation,
             "timeCodesPerSecond must be greater than 0");
  }

  if (metas.framesPerSecond.authored() &&
      !(metas.framesPerSecond.get_value() > 0.0)) {
    AddError(result, "core.layer.framesPerSecond", kLayerLocation,
             "framesPerSecond must be greater than 0");
  }

  if (metas.kilogramsPerUnit.authored() &&
      !(metas.kilogramsPerUnit.get_value() > 0.0)) {
    AddError(result, "core.layer.kilogramsPerUnit", kLayerLocation,
             "kilogramsPerUnit must be greater than 0");
  }

  // startTimeCode must not exceed endTimeCode. Warning, since a single layer's
  // range may be overridden by sublayers or the session layer.
  if (metas.startTimeCode.authored() && metas.endTimeCode.authored() &&
      metas.startTimeCode.get_value() > metas.endTimeCode.get_value()) {
    AddWarning(result, "core.layer.timeCodeRange", kLayerLocation,
               "startTimeCode is greater than endTimeCode");
  }

  for (size_t i = 0; i < metas.subLayers.size(); i++) {
    const SubLayer &sub_layer = metas.subLayers[i];
    const std::string location =
        std::string(kLayerLocation) + ".subLayers[" + std::to_string(i) + "]";
    if (sub_layer.assetPath.GetAssetPath().empty()) {
      AddError(result, "core.layer.subLayers", location,
               "subLayer asset path must not be empty");
    }
    ValidateLayerOffset(sub_layer.layerOffset, "core.layer.subLayerOffset",
                        location, result);
  }

  if (metas.colorConfiguration &&
      metas.colorConfiguration.value().GetAssetPath().empty()) {
    AddError(result, "core.layer.colorConfiguration", kLayerLocation,
             "colorConfiguration asset path must not be empty");
  }
  if (metas.colorManagementSystem &&
      metas.colorManagementSystem.value().str().empty()) {
    AddError(result, "core.layer.colorManagementSystem", kLayerLocation,
             "colorManagementSystem must not be empty");
  }
  if (metas.owner && metas.owner.value().empty()) {
    AddWarning(result, "core.layer.owner", kLayerLocation,
               "owner should not be empty when authored");
  }
  if (metas.customLayerDataAuthored) {
    ValidateDictionaryKeys(metas.customLayerData, "core.layer.customLayerData",
                           "<layer>.customLayerData", result);
  }
  if (metas.expressionVariables) {
    ValidateDictionaryKeys(metas.expressionVariables.value(),
                           "core.layer.expressionVariables",
                           "<layer>.expressionVariables", result);
  }
}

void ValidateColorSpaceDefinitionProperty(
    const std::string &prop_name, const Property &prop,
    const std::string &location, USDValidationResult *result) {
  std::string instance;
  std::string field;
  const bool current_multiple_apply =
      ParseColorSpaceDefinitionPropertyName(prop_name, &instance, &field);
  const std::string &effective_name = current_multiple_apply ? field : prop_name;
  if (effective_name == "name" ||
      effective_name == "colorSpaceDefinition:name") {
    ValidateTokenAttributeType(prop, "core.schema.ColorSpaceDefinitionAPI.name",
                               location, result);
    ValidateUniformAttribute(prop, "core.schema.ColorSpaceDefinitionAPI.name",
                             location, result);
    return;
  }

  if (effective_name == "redChroma" ||
      effective_name == "colorSpaceDefinition:redChroma" ||
      effective_name == "greenChroma" ||
      effective_name == "colorSpaceDefinition:greenChroma" ||
      effective_name == "blueChroma" ||
      effective_name == "colorSpaceDefinition:blueChroma" ||
      effective_name == "whitePoint" ||
      effective_name == "colorSpaceDefinition:whitePoint") {
    ValidateFloat2AttributeType(
        prop, "core.schema.ColorSpaceDefinitionAPI.coordinates", location,
        result);
    ValidateVaryingAttribute(
        prop, "core.schema.ColorSpaceDefinitionAPI.coordinates", location,
        result);
    return;
  }

  if (effective_name == "gamma" ||
      effective_name == "colorSpaceDefinition:gamma" ||
      effective_name == "linearBias" ||
      effective_name == "colorSpaceDefinition:linearBias") {
    ValidateFloatAttributeType(
        prop, "core.schema.ColorSpaceDefinitionAPI.curve", location, result);
    ValidateVaryingAttribute(prop, "core.schema.ColorSpaceDefinitionAPI.curve",
                             location, result);
  }
}

void ValidateCollectionProperty(
    const std::vector<std::string> &segments, const Property &prop,
    const std::set<std::string> &collection_instances,
    const std::string &location, USDValidationResult *result) {
  if (segments.size() < 2) {
    AddError(result, "core.schema.CollectionAPI.property", location,
             "collection property must include an instance name");
    return;
  }

  const std::string &instance_name = segments[1];
  if (instance_name.empty()) {
    AddError(result, "core.schema.CollectionAPI.instance", location,
             "collection property must use a non-empty instance name");
    return;
  }

  if (collection_instances.count(instance_name) == 0) {
    AddError(result, "core.schema.CollectionAPI.applied", location,
             "collection property is authored without applying `CollectionAPI:" +
                 instance_name + "`");
  }

  if (segments.size() == 2) {
    return;
  }

  if (segments.size() != 3) {
    AddError(result, "core.schema.CollectionAPI.property", location,
             "collection property has an unsupported namespace shape");
    return;
  }

  const std::string &member = segments[2];
  if (member == "expansionRule") {
    ValidateTokenAttributeType(prop, "core.schema.CollectionAPI.expansionRule",
                               location, result);
    ValidateUniformAttribute(prop, "core.schema.CollectionAPI.expansionRule",
                             location, result);

    if (prop.is_attribute()) {
      auto token = prop.get_attribute().get_value<value::token>();
      if (token) {
        const std::string &value = token->str();
        if (value != "explicitOnly" && value != "expandPrims" &&
            value != "expandPrimsAndProperties") {
          AddError(result, "core.schema.CollectionAPI.expansionRule", location,
                   "invalid expansionRule `" + value + "`");
        }
      }
    }
    return;
  }

  if (member == "includeRoot") {
    ValidateBoolAttributeType(prop, "core.schema.CollectionAPI.includeRoot",
                              location, result);
    ValidateUniformAttribute(prop, "core.schema.CollectionAPI.includeRoot",
                             location, result);
    return;
  }

  if (member == "includes" || member == "excludes") {
    if (!prop.is_relationship()) {
      AddError(result, "core.schema.CollectionAPI.relationship", location,
               "expected a relationship");
      return;
    }

    ValidateRelationshipTargets(prop.get_relationship(),
                                "core.schema.CollectionAPI.relationship",
                                location, result);

    if (member == "excludes") {
      for (const Path &target : prop.get_relationTargets()) {
        if (target.is_property_path() &&
            target.prop_part().starts_with("collection:")) {
          AddError(
              result, "core.schema.CollectionAPI.excludes", location,
              "excludes must not target a property referring to another collection");
        }
      }
    }
    return;
  }

  AddError(result, "core.schema.CollectionAPI.property", location,
           "unsupported CollectionAPI property `" + member + "`");
}

bool GetDictionaryValue(const Dictionary &dict, const std::string &key,
                        Dictionary *out) {
  auto it = dict.find(key);
  if (it == dict.end() || !out) {
    return false;
  }
  auto value = it->second.get_value<Dictionary>();
  if (!value) {
    return false;
  }
  *out = value.value();
  return true;
}

bool GetMetaDouble(const Dictionary &dict, const std::string &key,
                   double *out) {
  auto it = dict.find(key);
  if (it == dict.end() || !out) {
    return false;
  }
  auto value = it->second.get_value<double>();
  if (!value) {
    return false;
  }
  *out = value.value();
  return true;
}

bool GetMetaAssetPath(const Dictionary &dict, const std::string &key,
                      std::string *out) {
  auto it = dict.find(key);
  if (it == dict.end() || !out) {
    return false;
  }
  if (auto asset = it->second.get_value<value::AssetPath>()) {
    *out = asset.value().GetAssetPath();
    return true;
  }
  if (auto str = it->second.get_value<std::string>()) {
    *out = str.value();
    return true;
  }
  return false;
}

bool GetMetaString(const Dictionary &dict, const std::string &key,
                   std::string *out) {
  auto it = dict.find(key);
  if (it == dict.end() || !out) {
    return false;
  }
  if (auto str = it->second.get_value<std::string>()) {
    *out = str.value();
    return true;
  }
  if (auto str = it->second.get_value<value::StringData>()) {
    *out = str.value().value;
    return true;
  }
  return false;
}

bool GetMetaAssetPathArray(const Dictionary &dict, const std::string &key,
                           std::vector<std::string> *out) {
  auto it = dict.find(key);
  if (it == dict.end() || !out) {
    return false;
  }
  auto values = it->second.get_value<std::vector<value::AssetPath>>();
  if (!values) {
    return false;
  }
  out->clear();
  for (const value::AssetPath &asset : values.value()) {
    out->push_back(asset.GetAssetPath());
  }
  return true;
}

bool GetMetaDouble2Array(const Dictionary &dict, const std::string &key,
                         std::vector<value::double2> *out) {
  auto it = dict.find(key);
  if (it == dict.end() || !out) {
    return false;
  }
  auto values = it->second.get_value<std::vector<value::double2>>();
  if (!values) {
    return false;
  }
  *out = values.value();
  return true;
}

void ValidateDictionaryKeys(const Dictionary &dict, const std::string &rule_id,
                            const std::string &location,
                            USDValidationResult *result) {
  for (const auto &entry : dict) {
    const std::string key_location = location + "." + entry.first;
    if (!IsValidNamespacedIdentifier(entry.first)) {
      AddError(result, rule_id, location,
               "dictionary key `" + entry.first +
                   "` is not a valid namespaced identifier");
    }

    if (auto child = entry.second.get_value<Dictionary>()) {
      ValidateDictionaryKeys(child.value(), rule_id, key_location, result);
    }
  }
}

bool IsAbsolutePrimPathString(const std::string &path) {
  return !path.empty() && path[0] == '/' &&
         path.find("//") == std::string::npos &&
         path.find('.') == std::string::npos;
}

void ValidateClipPairArray(const std::vector<value::double2> &pairs,
                           const std::string &rule_id,
                           const std::string &location,
                           const std::string &field_name,
                           USDValidationResult *result) {
  bool have_prev_stage_time = false;
  double prev_stage_time = 0.0;
  for (size_t i = 0; i < pairs.size(); i++) {
    const double stage_time = pairs[i][0];
    const double mapped_value = pairs[i][1];
    if (!std::isfinite(stage_time) || !std::isfinite(mapped_value)) {
      AddError(result, rule_id, location,
               field_name + "[" + std::to_string(i) +
                   "] must contain finite values");
    }
    if (have_prev_stage_time && stage_time <= prev_stage_time) {
      AddError(result, rule_id, location,
               field_name + " stage times must be strictly increasing");
    }
    have_prev_stage_time = true;
    prev_stage_time = stage_time;
  }
}

void ValidateClipSetMetadata(const std::string &clip_set_name,
                             const Dictionary &clip_set,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  const std::string location = prim_location + ".clips[" + clip_set_name + "]";
  std::vector<std::string> asset_paths;
  const bool have_asset_paths =
      GetMetaAssetPathArray(clip_set, "assetPaths", &asset_paths);

  std::string template_asset_path;
  const bool have_template_asset =
      GetMetaAssetPath(clip_set, "templateAssetPath", &template_asset_path);

  if (!have_asset_paths && !have_template_asset) {
    AddError(result, "core.clips.assets", location,
             "clip set must author assetPaths or templateAssetPath");
  }

  if (have_asset_paths) {
    if (asset_paths.empty()) {
      AddError(result, "core.clips.assets", location + ".assetPaths",
               "assetPaths must not be empty");
    }
    for (size_t i = 0; i < asset_paths.size(); i++) {
      if (asset_paths[i].empty()) {
        AddError(result, "core.clips.assets", location + ".assetPaths",
                 "assetPaths[" + std::to_string(i) + "] must not be empty");
      }
    }
  }

  if (have_template_asset) {
    if (template_asset_path.empty()) {
      AddError(result, "core.clips.template", location + ".templateAssetPath",
               "templateAssetPath must not be empty");
    }
    double start_time = 0.0;
    double end_time = 0.0;
    double stride = 1.0;
    double active_offset = 0.0;
    const bool have_start =
        GetMetaDouble(clip_set, "templateStartTime", &start_time);
    const bool have_end = GetMetaDouble(clip_set, "templateEndTime", &end_time);
    const bool have_stride = GetMetaDouble(clip_set, "templateStride", &stride);
    const bool have_active_offset =
        GetMetaDouble(clip_set, "templateActiveOffset", &active_offset);
    if (!have_start || !have_end) {
      AddError(result, "core.clips.template", location,
               "template clip sets must author templateStartTime and "
               "templateEndTime");
    } else {
      if (!std::isfinite(start_time) || !std::isfinite(end_time)) {
        AddError(result, "core.clips.template", location,
                 "templateStartTime and templateEndTime must be finite");
      }
      if (end_time < start_time) {
        AddError(result, "core.clips.template", location,
                 "templateEndTime must be greater than or equal to "
                 "templateStartTime");
      }
    }
    if (have_stride && (!std::isfinite(stride) || stride <= 0.0)) {
      AddError(result, "core.clips.template", location + ".templateStride",
               "templateStride must be positive and finite");
    }
    if (have_active_offset &&
        (!std::isfinite(active_offset) || !std::isfinite(stride) ||
         stride <= 0.0 || std::fabs(active_offset) > stride)) {
      AddError(result, "core.clips.template",
               location + ".templateActiveOffset",
               "templateActiveOffset must be finite and no greater than "
               "templateStride");
    }
  }

  std::vector<value::double2> active;
  if (GetMetaDouble2Array(clip_set, "active", &active)) {
    if (active.empty()) {
      AddError(result, "core.clips.active", location + ".active",
               "active must not be empty");
    }
    ValidateClipPairArray(active, "core.clips.active", location + ".active",
                          "active", result);
    if (have_asset_paths) {
      for (size_t i = 0; i < active.size(); i++) {
        const double asset_index = active[i][1];
        const double rounded = std::floor(asset_index);
        if (std::fabs(asset_index - rounded) > 1.0e-9 || asset_index < 0.0 ||
            static_cast<size_t>(asset_index) >= asset_paths.size()) {
          AddError(result, "core.clips.active", location + ".active",
                   "active[" + std::to_string(i) +
                       "] references asset index " +
                       std::to_string(asset_index) + ", but assetPaths has " +
                       std::to_string(asset_paths.size()) + " entries");
        }
      }
    }
  } else if (have_asset_paths) {
    AddError(result, "core.clips.active", location,
             "explicit clip sets with assetPaths must author active");
  }

  std::vector<value::double2> times;
  if (GetMetaDouble2Array(clip_set, "times", &times)) {
    ValidateClipPairArray(times, "core.clips.times", location + ".times",
                          "times", result);
  }

  std::string prim_path;
  if (GetMetaString(clip_set, "primPath", &prim_path) &&
      !IsAbsolutePrimPathString(prim_path)) {
    AddError(result, "core.clips.primPath", location + ".primPath",
             "primPath must be an absolute prim path, but is `" + prim_path +
                 "`");
  }

  std::string manifest_asset_path;
  if (GetMetaAssetPath(clip_set, "manifestAssetPath", &manifest_asset_path) &&
      manifest_asset_path.empty()) {
    AddError(result, "core.clips.assets", location + ".manifestAssetPath",
             "manifestAssetPath must not be empty");
  }
}

void ValidateClipsMetadata(const PrimSpec &ps, const std::string &prim_location,
                           USDValidationResult *result) {
  if (!ps.metas().has_clips()) {
    return;
  }

  const Dictionary clips = ps.metas().get_clips();
  if (clips.empty()) {
    AddError(result, "core.clips", prim_location + ".clips",
             "clips dictionary must contain at least one clip set");
    return;
  }

  for (const auto &clip_entry : clips) {
    Dictionary clip_set;
    if (!GetDictionaryValue(clips, clip_entry.first, &clip_set)) {
      AddError(result, "core.clips", prim_location + ".clips",
               "clip set `" + clip_entry.first + "` must be a dictionary");
      continue;
    }
    ValidateClipSetMetadata(clip_entry.first, clip_set, prim_location, result);
  }
}

void BuildPrimIndexRecursive(const PrimSpec &ps, const Path &prim_path,
                             PrimTypeByPath *prim_types,
                             PrimSpecifierByPath *prim_specifiers,
                             SkeletonJointCountByPath *skeleton_joints) {
  if (!prim_types || !prim_specifiers) {
    return;
  }

  (*prim_types)[prim_path.full_path_name()] = ps.typeName();
  (*prim_specifiers)[prim_path.full_path_name()] = ps.specifier();
  if (skeleton_joints && ps.typeName() == "Skeleton") {
    std::vector<value::token> joints;
    if (GetTokenArrayProperty(ps, "joints", &joints)) {
      (*skeleton_joints)[prim_path.full_path_name()] = joints.size();
    }
  }

  for (const auto &child_entry : ps.children()) {
    BuildPrimIndexRecursive(child_entry, prim_path.AppendPrim(child_entry.name()),
                            prim_types, prim_specifiers, skeleton_joints);
  }
}

void ValidatePrimSpecRecursive(const PrimSpec &ps, const Path &prim_path,
                               const ColorSpaceSet &inherited_color_spaces,
                               const AncestorContext &ancestors,
                               const ValidationOptions &options,
                               const PrimTypeByPath &prim_types,
                               const PrimSpecifierByPath &prim_specifiers,
                               const SkeletonJointCountByPath &skeleton_joints,
                               USDValidationResult *result) {
  const std::string prim_location = prim_path.full_path_name();
  const std::vector<AppliedSchema> applied_schemas =
      CollectAppliedSchemas(ps.metas());
  const std::string &type_name = ps.typeName();
  const bool has_arc = HasCompositionArc(ps);
  const bool is_over = IsOverride(ps);

  // ---- core.prim.name: prim name must be a valid USD identifier ----
  if (options.core && !ps.name().empty() &&
      !is_valid_utf8_identifier(ps.name())) {
    AddError(result, "core.prim.name", prim_location,
             "prim name `" + ps.name() + "` is not a valid identifier");
  }

  // ---- geom.encapsulation.nestedGprim: a Gprim must not nest under a Gprim ----
  if (options.geom && IsGprimTypeName(type_name) &&
      ancestors.has_gprim_ancestor) {
    AddWarning(result, "geom.encapsulation.nestedGprim", prim_location,
               "Gprim `" + type_name + "` is nested under another Gprim at `" +
                   ancestors.nearest_gprim_path + "`");
  }

  size_t local_mesh_face_count = 0;
  bool local_mesh_face_count_known = false;
  if (options.geom && !has_arc && !is_over) {
    ValidateGeomCommonProperties(ps, prim_location, result);
    ValidateGeomPrimitive(ps, prim_location, result);
    if (type_name == "Mesh") {
      ValidateMeshTopology(ps, prim_location, result, &local_mesh_face_count);
      std::vector<int32_t> face_counts;
      local_mesh_face_count_known =
          GetIntArrayProperty(ps, "faceVertexCounts", &face_counts);
      if (local_mesh_face_count_known) {
        local_mesh_face_count = face_counts.size();
      }
    } else if (type_name == "GeomSubset") {
      ValidateGeomSubsetTopology(ps, prim_location, ancestors, result);
    } else if (type_name == "Points") {
      ValidateGeomPoints(ps, prim_location, result);
    } else if (type_name == "BasisCurves" || type_name == "NurbsCurves" ||
               type_name == "HermiteCurves" || type_name == "NurbsPatch") {
      ValidateGeomCurves(ps, prim_location, result);
    } else if (type_name == "PointInstancer") {
      ValidatePointInstancer(ps, prim_location, result);
    } else if (type_name == "Camera") {
      ValidateCamera(ps, prim_location, result);
    }
    ValidatePrimvars(ps, prim_location, result);
    if (type_name == "Mesh") {
      ValidateSkinningPrimvars(ps, prim_location, prim_types, skeleton_joints,
                               result);
    } else if (type_name == "Skeleton") {
      ValidateSkeletonTopology(ps, prim_location, result, nullptr);
    } else if (type_name == "SkelAnimation") {
      ValidateSkelAnimation(ps, prim_location, result);
    } else if (type_name == "BlendShape") {
      ValidateBlendShape(ps, prim_location, result);
    }
  }

  if (options.lux && !has_arc && !is_over && IsLuxLightTypeName(type_name)) {
    ValidateLuxLight(ps, prim_location, result);
  }

  if (options.physics && !has_arc && !is_over) {
    ValidatePhysics(ps, applied_schemas, prim_location, prim_types, result);
  }

  if (options.geom && !has_arc && !is_over) {
    ValidateSkelBinding(ps, prim_location, prim_types, result);
  }

  if (options.core) {
    ValidateCompositionMetadata(ps, prim_location, prim_specifiers, result);
    ValidatePrimMetadata(ps, prim_location, result);
    ValidateClipsMetadata(ps, prim_location, result);
  }

  // ---- shade.encapsulation.shaderParent: Shader/NodeGraph need a container ----
  // Gated: a local typeless `over` or a composed parent may legitimately supply
  // the Material/NodeGraph container.
  if (options.shade && IsShadeConnectableTypeName(type_name) && !has_arc &&
      !is_over && !ancestors.parent_type.empty() &&
      !IsShadeContainerTypeName(ancestors.parent_type)) {
    AddWarning(result, "shade.encapsulation.shaderParent", prim_location,
               type_name + " must be parented by a Material or NodeGraph, but "
               "its parent is `" + ancestors.parent_type + "`");
  }

  const std::string shader_id =
      (options.shade && type_name == "Shader") ? GetShaderInfoId(ps)
                                                : std::string();

  // ---- shade.preview.*: UsdPreviewSurface input schema conformance ----
  if (options.shade && shader_id == "UsdPreviewSurface") {
    ValidateUsdPreviewSurface(ps, prim_location, result);
  } else if (options.shade && shader_id == "UsdUVTexture") {
    ValidateUsdUVTexture(ps, prim_location, result);
  } else if (options.shade && IsUsdPrimvarReaderId(shader_id)) {
    ValidateUsdPrimvarReader(ps, shader_id, prim_location, result);
  }
  if (options.shade) {
    ValidateMaterialXConfig(ps, applied_schemas, prim_location, result);
    ValidateMaterialUEConfig(ps, applied_schemas, prim_location, result);
    if (type_name == "Shader") {
      ValidateMaterialXShader(ps, shader_id, prim_location, result);
    }
    ValidateMaterialXReferenceConventions(ps, prim_location, result);
  }

  std::set<std::string> collection_instances;
  if (options.core) {
    collection_instances = CollectAppliedCollectionInstances(applied_schemas);
  }

  if (options.core) {
    for (const auto &schema : applied_schemas) {
      if (schema.name == "ColorSpaceAPI" && !schema.instance_name.empty()) {
        AddError(result, "core.schema.singleApply.instance", prim_location,
                 schema.name + " must not be authored with an instance name");
      }
    }
  }

  ColorSpaceSet visible_color_spaces = inherited_color_spaces;
  if (options.core) {
    const ColorSpaceSet local_defs =
        CollectLocalColorSpaceDefinitions(ps, applied_schemas);
    visible_color_spaces.insert(local_defs.begin(), local_defs.end());
  }

  const bool has_color_space_api =
      HasAppliedSchema(applied_schemas, "ColorSpaceAPI");
  const bool has_color_space_definition_api =
      HasAppliedSchema(applied_schemas, "ColorSpaceDefinitionAPI");
  if (options.core && has_color_space_definition_api) {
    ValidateLocalColorSpaceDefinitions(ps, applied_schemas, prim_location,
                                       result);
  }

  for (const auto &prop_entry : ps.props()) {
    const std::string &prop_name = prop_entry.first;
    const Property &prop = prop_entry.second;
    const std::string prop_location =
        MakePropertyLocation(prim_location, prop_name);

    if (options.core && prop.is_attribute()) {
      ValidateAttributeMetadata(prop.get_attribute(), prop_name, prop_location,
                                result);
      ValidateTimeSamples(prop.get_attribute(), "core.attr.timeSamples",
                          prop_location, result);
    } else if (options.core && prop.is_relationship()) {
      ValidateRelationshipMetadata(prop.get_relationship(), prop_location,
                                   result);
    }

    if (options.core && prop_name == "colorSpace:name") {
      if (!has_color_space_api) {
        AddError(result, "core.schema.ColorSpaceAPI.applied", prop_location,
                 "`colorSpace:name` is authored without applying ColorSpaceAPI");
      }

      ValidateTokenAttributeType(prop, "core.schema.ColorSpaceAPI.name",
                                 prop_location, result);
      ValidateUniformAttribute(prop, "core.schema.ColorSpaceAPI.name",
                               prop_location, result);

      if (prop.is_attribute()) {
        auto token = prop.get_attribute().get_value<value::token>();
        if (token) {
          ValidateColorSpaceToken(token->str(), visible_color_spaces,
                                  "core.schema.ColorSpaceAPI.name",
                                  prop_location, result);
        }
      }
    } else if (options.core && prop_name.rfind("collection:", 0) == 0) {
      ValidateCollectionProperty(SplitString(prop_name, ':'), prop,
                                 collection_instances, prop_location, result);
    } else if (options.core &&
               prop_name.rfind("colorSpaceDefinition:", 0) == 0) {
      std::string definition_instance;
      std::string definition_field;
      const bool current_property = ParseColorSpaceDefinitionPropertyName(
          prop_name, &definition_instance, &definition_field);
      bool matching_application = false;
      for (const AppliedSchema &schema : applied_schemas) {
        if (schema.name == "ColorSpaceDefinitionAPI" &&
            ((current_property && schema.instance_name == definition_instance) ||
             (!current_property && schema.instance_name.empty()))) {
          matching_application = true;
          break;
        }
      }
      if (!matching_application) {
        AddError(result, "core.schema.ColorSpaceDefinitionAPI.applied",
                 prop_location,
                 "ColorSpaceDefinitionAPI property is authored without "
                 "applying its matching ColorSpaceDefinitionAPI instance");
      }
      ValidateColorSpaceDefinitionProperty(prop_name, prop, prop_location,
                                           result);
    } else if (options.core && has_color_space_definition_api &&
               IsColorSpaceDefinitionPropertyName(prop_name)) {
      ValidateColorSpaceDefinitionProperty(prop_name, prop, prop_location,
                                           result);
    }

    if (options.core && prop.is_attribute() &&
        prop.get_attribute().metas().has_colorSpace()) {
      const std::string color_space =
          prop.get_attribute().metas().get_colorSpace().str();
      ValidateColorSpaceToken(color_space, visible_color_spaces,
                              "core.attr.colorSpace", prop_location, result);
    }

    // ---- shade.connection.target: shade connections must target shade ports ----
    if (options.shade && !has_arc && !is_over && prop.is_attribute() &&
        prop.get_attribute().has_connections() &&
        (type_name == "Material" || type_name == "Shader" ||
         type_name == "NodeGraph") &&
        (IsShaderInputName(prop_name) || IsShaderOutputName(prop_name))) {
      const bool require_output_target =
          (type_name == "Material" && IsMaterialTerminalOutputName(prop_name));
      ValidateConnectionTargets(prop.get_attribute(), "shade.connection.target",
                                prop_location, prim_types, result,
                                require_output_target);
    }

    // ---- shade.material.outputConnection: Material terminals are connections ----
    if (options.shade && !has_arc && !is_over && type_name == "Material" &&
        IsMaterialTerminalOutputName(prop_name)) {
      if (!prop.is_attribute()) {
        AddError(result, "shade.material.outputConnection", prop_location,
                 "`" + prop_name + "` must be a connection attribute");
      } else if (!prop.get_attribute().has_connections()) {
        AddError(result, "shade.material.outputConnection", prop_location,
                 "`" + prop_name + "` must connect to a shader or node graph output");
      }
    }

    // ---- shade.material.binding: material:binding* must be a relationship ----
    if (options.shade &&
        (prop_name == "material:binding" ||
         prop_name.rfind("material:binding:", 0) == 0)) {
      if (!prop.is_relationship()) {
        // Pure local syntax error -- always reported.
        AddError(result, "shade.material.binding", prop_location,
                 "`" + prop_name + "` must be a relationship");
      } else if (!has_arc && !is_over &&
                 !HasAppliedSchema(applied_schemas, "MaterialBindingAPI")) {
        AddWarning(result, "shade.material.bindingAPI", prop_location,
                   "`" + prop_name +
                       "` is authored without applying MaterialBindingAPI");
      }
    }
  }

  // ---- core.xformOp.order: every op in xformOpOrder must be authored here ----
  // Gated: an override or composed prim may inherit the op attributes.
  if (options.core && !has_arc && !is_over) {
    auto it = ps.props().find("xformOpOrder");
    if (it != ps.props().end() && it->second.is_attribute()) {
      auto order =
          it->second.get_attribute().get_value<std::vector<value::token>>();
      if (order) {
        for (const value::token &op : order.value()) {
          const std::string &op_name = op.str();
          if (op_name == "!resetXformStack!") {
            continue;
          }
          if (ps.props().count(op_name) == 0) {
            AddError(result, "core.xformOp.order",
                     MakePropertyLocation(prim_location, "xformOpOrder"),
                     "xformOpOrder references `" + op_name +
                         "` but no such attribute is authored on the prim");
          }
        }
      }
    }
  }

  // Propagate ancestor context to children.
  AncestorContext child_ctx;
  child_ctx.parent_type = type_name;
  child_ctx.parent_mesh_face_count_known = local_mesh_face_count_known;
  child_ctx.parent_mesh_face_count = local_mesh_face_count;
  if (IsGprimTypeName(type_name)) {
    child_ctx.has_gprim_ancestor = true;
    child_ctx.nearest_gprim_path = prim_location;
  } else {
    child_ctx.has_gprim_ancestor = ancestors.has_gprim_ancestor;
    child_ctx.nearest_gprim_path = ancestors.nearest_gprim_path;
  }

  for (const auto &child_entry : ps.children()) {
    Path child_path = prim_path.AppendPrim(child_entry.name());
    ValidatePrimSpecRecursive(child_entry, child_path, visible_color_spaces,
                              child_ctx, options, prim_types, prim_specifiers,
                              skeleton_joints, result);
  }

  for (const auto &variant_set_entry : ps.variantSets()) {
    const std::string &variant_set_name = variant_set_entry.first;
    const VariantSetSpec &variant_set = variant_set_entry.second;

    for (const auto &variant_entry : variant_set.variantSet) {
      const std::string &variant_name = variant_entry.first;
      const PrimSpec &variant_ps = variant_entry.second;

      Path variant_path = prim_path;
      std::string variant_location =
          prim_location + "{" + variant_set_name + "=" + variant_name + "}";
      // Preserve precise child/property paths when walking the variant body.
      if (!variant_ps.name().empty() && variant_ps.name() != ps.name()) {
        variant_path = prim_path.AppendPrim(variant_ps.name());
        variant_location = variant_path.full_path_name() + "{" +
                           variant_set_name + "=" + variant_name + "}";
      }

      (void)variant_location;
      // Variant body shares the owning prim's ancestor context.
      ValidatePrimSpecRecursive(variant_ps, variant_path, visible_color_spaces,
                                child_ctx, options, prim_types,
                                prim_specifiers, skeleton_joints, result);
    }
  }
}

std::string CrateLocation(const std::string &filename) {
  return filename.empty() ? std::string("<crate>") : filename;
}

std::string CrateSectionName(const crate::Section &section) {
  const char *begin = section.name;
  const char *end = begin;
  while ((end < begin + crate::kSectionNameMaxLength + 1) && (*end != '\0')) {
    ++end;
  }
  return std::string(begin, end);
}

bool HasExtension(const std::string &filename, const std::string &extension) {
  return filename.size() >= extension.size() &&
         to_lower(filename.substr(filename.size() - extension.size())) ==
             extension;
}

void AddLinesAsIssues(USDValidationResult *result,
                      USDValidationSeverity severity,
                      const std::string &rule_id,
                      const std::string &location,
                      const std::string &text) {
  for (const std::string &line : SplitString(text, '\n')) {
    if (line.empty()) {
      continue;
    }
    AddIssue(result, severity, rule_id, location, line);
  }
}

void ValidateCrateTables(const crate::CrateReader &reader,
                         const std::string &location,
                         USDValidationResult *result) {
  const crate::TableOfContents &toc = reader.GetTableOfContents();
  const std::vector<value::token> &tokens = reader.GetTokens();
  const std::vector<crate::Index> &string_indices = reader.GetStringIndices();
  const std::vector<crate::Field> &fields = reader.GetFields();
  const std::vector<crate::Index> &fieldset_indices =
      reader.GetFieldsetIndices();
  const std::vector<Path> &paths = reader.GetPaths();
  const std::vector<crate::Spec> &specs = reader.GetSpecs();

  static const std::array<const char *, 6> kRequiredSections = {
      "TOKENS", "STRINGS", "FIELDS", "FIELDSETS", "SPECS", "PATHS"};
  std::unordered_map<std::string, size_t> section_counts;
  struct SectionRange {
    int64_t start;
    int64_t end;
    std::string name;
  };
  std::vector<SectionRange> ranges;

  for (const crate::Section &section : toc.sections) {
    const std::string name = CrateSectionName(section);
    section_counts[name]++;
    ranges.push_back({section.start, section.start + section.size, name});
  }

  for (const char *required : kRequiredSections) {
    const size_t count = section_counts[required];
    if (count == 0) {
      AddError(result, "crate.toc.requiredSection", location,
               std::string("Required Crate section `") + required +
                   "` is missing");
    } else if (count > 1) {
      AddError(result, "crate.toc.duplicateSection", location,
               std::string("Crate section `") + required + "` appears " +
                   std::to_string(count) + " times");
    }
  }

  for (const auto &entry : section_counts) {
    const bool known = std::any_of(
        kRequiredSections.begin(), kRequiredSections.end(),
        [&](const char *required) { return entry.first == required; });
    if (!known) {
      AddWarning(result, "crate.toc.unknownSection", location,
                 "Unknown Crate section `" + entry.first + "` is present");
    }
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const SectionRange &a, const SectionRange &b) {
              return a.start < b.start;
            });
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].start < ranges[i - 1].end) {
      AddError(result, "crate.toc.overlap", location,
               "Crate section `" + ranges[i].name + "` overlaps `" +
                   ranges[i - 1].name + "`");
    }
  }

  for (size_t i = 0; i < string_indices.size(); ++i) {
    if (string_indices[i].value >= tokens.size()) {
      AddError(result, "crate.strings.tokenIndex",
               location + "#strings[" + std::to_string(i) + "]",
               "String table entry references token index " +
                   std::to_string(string_indices[i].value) +
                   ", but only " + std::to_string(tokens.size()) +
                   " tokens are present");
    }
  }

  for (size_t i = 0; i < fields.size(); ++i) {
    const crate::Field &field = fields[i];
    const std::string field_location =
        location + "#fields[" + std::to_string(i) + "]";
    if (field.token_index.value >= tokens.size()) {
      AddError(result, "crate.field.tokenIndex", field_location,
               "Field references token index " +
                   std::to_string(field.token_index.value) +
                   ", but only " + std::to_string(tokens.size()) +
                   " tokens are present");
    }

    const int32_t type_id = field.value_rep.GetType();
    const auto data_type = crate::GetCrateDataType(type_id);
    if (!data_type) {
      AddError(result, "crate.field.valueType", field_location,
               "ValueRep uses invalid Crate data type id " +
                   std::to_string(type_id));
    } else if (field.value_rep.IsArray() && !data_type.value().supports_array) {
      AddError(result, "crate.field.valueType", field_location,
               "ValueRep marks `" + std::string(data_type.value().name) +
                   "` as an array, but that Crate type does not support arrays");
    }

    if (field.value_rep.IsCompressed() && !field.value_rep.IsArray()) {
      AddError(result, "crate.field.compression", field_location,
               "ValueRep sets the compressed bit on a scalar value");
    }
  }

  for (size_t i = 0; i < fieldset_indices.size(); ++i) {
    const crate::Index field_index = fieldset_indices[i];
    if ((field_index != crate::Index()) &&
        (field_index.value >= fields.size())) {
      AddError(result, "crate.fieldsets.fieldIndex",
               location + "#fieldsets[" + std::to_string(i) + "]",
               "FieldSet entry references field index " +
                   std::to_string(field_index.value) + ", but only " +
                   std::to_string(fields.size()) + " fields are present");
    }
  }

  for (size_t i = 0; i < specs.size(); ++i) {
    const crate::Spec &spec = specs[i];
    const std::string spec_location =
        location + "#specs[" + std::to_string(i) + "]";
    if ((spec.path_index != crate::Index()) &&
        (spec.path_index.value >= paths.size())) {
      AddError(result, "crate.spec.pathIndex", spec_location,
               "Spec references path index " +
                   std::to_string(spec.path_index.value) + ", but only " +
                   std::to_string(paths.size()) + " paths are present");
    }
    if ((spec.fieldset_index != crate::Index()) &&
        ((spec.fieldset_index.value >= fieldset_indices.size()) ||
         !reader.IsFieldSetStartIndex(spec.fieldset_index))) {
      AddError(result, "crate.spec.fieldSetIndex", spec_location,
               "Spec references invalid FieldSet start index " +
                   std::to_string(spec.fieldset_index.value));
    }

    const int spec_type = static_cast<int>(spec.spec_type);
    if (spec_type <= static_cast<int>(SpecType::Unknown) ||
        spec_type >= static_cast<int>(SpecType::Invalid)) {
      AddError(result, "crate.spec.specType", spec_location,
               "Spec uses invalid or unknown spec type id " +
                   std::to_string(spec_type));
    }
  }
}

void ValidateUSDCAssetAgainstAOUSDCore(const uint8_t *addr, size_t length,
                                       const std::string &filename,
                                       USDValidationResult *result) {
  const std::string location = CrateLocation(filename);
  if (!addr || length < 88) {
    AddError(result, "crate.usdc.input", location,
             "USDC data is null or shorter than the Crate bootstrap header");
    return;
  }

  StreamReader sr(addr, length, /* swap_endian */ false);
  crate::CrateReaderConfig config;
  config.numThreads = 1;
  crate::CrateReader reader(&sr, config);

  const auto fail = [&](const std::string &step) {
    const std::string err = reader.GetError();
    AddError(result, "crate.parse." + step, location,
             err.empty() ? ("Failed to read Crate " + step) : err);
  };

  if (!reader.ReadBootStrap()) {
    fail("bootstrap");
    return;
  }
  if (!reader.ReadTOC()) {
    fail("toc");
    return;
  }
  if (!reader.ReadTokens()) {
    fail("tokens");
    return;
  }
  if (!reader.ReadStrings()) {
    fail("strings");
    return;
  }
  if (!reader.ReadFields()) {
    fail("fields");
    return;
  }
  if (!reader.ReadFieldSets()) {
    fail("fieldsets");
    return;
  }
  if (!reader.ReadPaths()) {
    fail("paths");
    return;
  }
  if (!reader.ReadSpecs()) {
    fail("specs");
    return;
  }
  if (!reader.BuildLiveFieldSets()) {
    fail("values");
    return;
  }

  ValidateCrateTables(reader, location, result);
}

}  // namespace

size_t USDValidationResult::error_count() const {
  size_t count = 0;
  for (const auto &issue : issues) {
    if (issue.severity == USDValidationSeverity::Error) {
      count++;
    }
  }
  return count;
}

size_t USDValidationResult::warning_count() const {
  size_t count = 0;
  for (const auto &issue : issues) {
    if (issue.severity == USDValidationSeverity::Warning) {
      count++;
    }
  }
  return count;
}

bool USDValidationResult::ok() const { return error_count() == 0; }

const char *GetAOUSDCoreSpecVersionString() { return kSpecVersion; }

ValidationOptions MakeValidateAllOptions() {
  ValidationOptions options;
  options.core = true;
  options.geom = true;
  options.shade = true;
  options.lux = true;
  options.physics = true;
  options.crate = true;
  return options;
}

ValidationOptions MakeContainerValidationOptions(
    const ValidationOptions &options) {
  ValidationOptions container_options;
  container_options.core = false;
  container_options.geom = false;
  container_options.shade = false;
  container_options.lux = false;
  container_options.physics = false;
  container_options.crate = options.core || options.crate;
  return container_options;
}

std::vector<std::string> GetValidationGroupNames(
    const ValidationOptions &options) {
  std::vector<std::string> groups;
  if (options.core) {
    groups.emplace_back("core");
  }
  if (options.geom) {
    groups.emplace_back("geom");
  }
  if (options.shade) {
    groups.emplace_back("shade");
  }
  if (options.lux) {
    groups.emplace_back("lux");
  }
  if (options.physics) {
    groups.emplace_back("physics");
  }
  if (options.crate) {
    groups.emplace_back("crate");
  }
  return groups;
}

std::vector<const USDValidationIssue *> GetOrderedValidationIssues(
    const USDValidationResult &result) {
  std::vector<const USDValidationIssue *> ordered;
  ordered.reserve(result.issues.size());
  for (const auto &issue : result.issues) {
    ordered.push_back(&issue);
  }
  std::stable_sort(
      ordered.begin(), ordered.end(),
      [](const USDValidationIssue *a, const USDValidationIssue *b) {
        if (a->severity != b->severity) {
          return static_cast<int>(a->severity) < static_cast<int>(b->severity);
        }
        const bool a_layer = (a->location == kLayerLocation);
        const bool b_layer = (b->location == kLayerLocation);
        if (a_layer != b_layer) {
          return a_layer;
        }
        if (a->location != b->location) {
          return a->location < b->location;
        }
        return a->rule_id < b->rule_id;
      });
  return ordered;
}

void MergeValidationResults(USDValidationResult *dst,
                            const USDValidationResult &src) {
  if (!dst) {
    return;
  }

  dst->issues.insert(dst->issues.end(), src.issues.begin(), src.issues.end());
  dst->checked_groups.core =
      dst->checked_groups.core || src.checked_groups.core;
  dst->checked_groups.geom =
      dst->checked_groups.geom || src.checked_groups.geom;
  dst->checked_groups.shade =
      dst->checked_groups.shade || src.checked_groups.shade;
  dst->checked_groups.lux = dst->checked_groups.lux || src.checked_groups.lux;
  dst->checked_groups.physics =
      dst->checked_groups.physics || src.checked_groups.physics;
  dst->checked_groups.crate =
      dst->checked_groups.crate || src.checked_groups.crate;
}

USDValidationResult ValidateUSDContainerAgainstAOUSDCore(
    const uint8_t *addr, size_t length, const std::string &filename,
    const ValidationOptions &options) {
  USDValidationResult result;
  result.checked_groups.core = false;
  result.checked_groups.geom = false;
  result.checked_groups.shade = false;
  result.checked_groups.lux = false;
  result.checked_groups.physics = false;
  result.checked_groups.crate = false;
  if (options.crate) {
    if (IsUSDC(addr, length)) {
      result.checked_groups.crate = true;
      ValidateUSDCAssetAgainstAOUSDCore(addr, length, filename, &result);
    } else if (IsUSDZ(addr, length)) {
      result.checked_groups.crate = true;
      std::string warn;
      std::string err;
      const bool valid_usdz = ValidateUSDZ(addr, length, &warn, &err);
      AddLinesAsIssues(&result, USDValidationSeverity::Warning,
                       "crate.usdz.package", CrateLocation(filename), warn);
      if (!valid_usdz) {
        AddLinesAsIssues(&result, USDValidationSeverity::Error,
                         "crate.usdz.package", CrateLocation(filename), err);
      }

      USDZAsset asset;
      warn.clear();
      err.clear();
      if (!ReadUSDZAssetInfoFromMemory(addr, length, /* asset_on_memory */ true,
                                       &asset, &warn, &err)) {
        AddLinesAsIssues(&result, USDValidationSeverity::Warning,
                         "crate.usdz.package", CrateLocation(filename), warn);
        AddLinesAsIssues(&result, USDValidationSeverity::Error,
                         "crate.usdz.package", CrateLocation(filename), err);
      } else {
        for (const auto &entry : asset.asset_map) {
          if (!HasExtension(entry.first, ".usdc")) {
            continue;
          }
          const size_t begin = entry.second.first;
          const size_t end = entry.second.second;
          if (begin > end || end > length) {
            AddError(&result, "crate.usdz.assetRange",
                     CrateLocation(filename) + "!" + entry.first,
                     "USDZ asset byte range is outside the package data");
            continue;
          }
          ValidateUSDCAssetAgainstAOUSDCore(addr + begin, end - begin,
                                            CrateLocation(filename) + "!" +
                                                entry.first,
                                            &result);
        }
      }
    }
  }

  return result;
}

USDValidationResult ValidateUSDContainerFileAgainstAOUSDCore(
    const std::string &filename, const ValidationOptions &options,
    std::string *warn, std::string *err, size_t max_file_size_in_mb) {
  USDValidationResult result;
  result.checked_groups.core = false;
  result.checked_groups.geom = false;
  result.checked_groups.shade = false;
  result.checked_groups.lux = false;
  result.checked_groups.physics = false;
  result.checked_groups.crate = false;
  if (!options.crate) {
    return result;
  }

  const size_t max_mb = max_file_size_in_mb;
  const size_t mb = 1024ull * 1024ull;
  const size_t max_bytes =
      (max_mb > ((std::numeric_limits<size_t>::max)() / mb))
          ? (std::numeric_limits<size_t>::max)()
          : max_mb * mb;
  std::vector<uint8_t> data;
  std::string local_err;
  if (!io::ReadWholeFile(&data, &local_err, filename, max_bytes,
                         /* userdata */ nullptr)) {
    result.checked_groups.crate = true;
    AddError(&result, "crate.input.read", CrateLocation(filename), local_err);
    if (err) {
      (*err) += local_err;
      if (!err->empty() && err->back() != '\n') {
        (*err) += "\n";
      }
    }
  } else {
    result = ValidateUSDContainerAgainstAOUSDCore(data.data(), data.size(),
                                                 filename, options);
  }

  (void)warn;
  return result;
}

bool ValidateUSDFromMemoryAgainstAOUSDCore(
    const uint8_t *addr, size_t length, const std::string &filename,
    const ValidationOptions &options, const USDLoadOptions &load_options,
    USDValidationResult *result, std::string *warn, std::string *err) {
  if (!result) {
    if (err) {
      (*err) += "null pointer for `result` argument.\n";
    }
    return false;
  }

  Layer layer;
  if (!LoadLayerFromMemory(addr, length, filename, &layer, warn, err,
                           load_options)) {
    return false;
  }

  *result = ValidateLayerAgainstAOUSDCore(layer, options);
  MergeValidationResults(
      result, ValidateUSDContainerAgainstAOUSDCore(
                  addr, length, filename,
                  MakeContainerValidationOptions(options)));
  return true;
}

bool ValidateUSDFileAgainstAOUSDCore(
    const std::string &filename, const ValidationOptions &options,
    const USDLoadOptions &load_options, USDValidationResult *result,
    std::string *warn, std::string *err) {
  if (!result) {
    if (err) {
      (*err) += "null pointer for `result` argument.\n";
    }
    return false;
  }

  Layer layer;
  if (!LoadLayerFromFile(filename, &layer, warn, err, load_options)) {
    return false;
  }

  *result = ValidateLayerAgainstAOUSDCore(layer, options);
  MergeValidationResults(
      result, ValidateUSDContainerFileAgainstAOUSDCore(
                  filename, MakeContainerValidationOptions(options), warn, err,
                  load_options.max_memory_limit_in_mb > 0
                      ? static_cast<size_t>(
                            load_options.max_memory_limit_in_mb)
                      : 0ull));
  return true;
}

USDValidationResult ValidateLayerAgainstAOUSDCore(const Layer &layer) {
  return ValidateLayerAgainstAOUSDCore(layer, ValidationOptions());
}

USDValidationResult ValidateLayerAgainstAOUSDCore(
    const Layer &layer, const ValidationOptions &options) {
  USDValidationResult result;
  result.checked_groups = options;
  result.checked_groups.crate = false;

  if (options.core) {
    ValidateLayerMetas(layer, &result);
  }

  PrimTypeByPath prim_types;
  PrimSpecifierByPath prim_specifiers;
  SkeletonJointCountByPath skeleton_joints;
  for (const auto &root_entry : layer.primspecs()) {
    const PrimSpec &ps = root_entry.second;
    Path prim_path("/", "");
    prim_path = prim_path.AppendPrim(ps.name());
    BuildPrimIndexRecursive(ps, prim_path, &prim_types, &prim_specifiers,
                            &skeleton_joints);
  }

  for (const auto &root_entry : layer.primspecs()) {
    const PrimSpec &ps = root_entry.second;
    Path prim_path("/", "");
    prim_path = prim_path.AppendPrim(ps.name());
    ValidatePrimSpecRecursive(ps, prim_path, ColorSpaceSet(), AncestorContext(),
                              options, prim_types, prim_specifiers,
                              skeleton_joints, &result);
  }

  return result;
}

namespace {

std::string Pluralize(size_t n, const char *noun) {
  std::ostringstream ss;
  ss << n << " " << noun;
  if (n != 1) {
    ss << "s";
  }
  return ss.str();
}

}  // namespace

std::string FormatValidationResult(const USDValidationResult &result) {
  std::ostringstream ss;
  ss << "AOUSD validation report (" << GetAOUSDCoreSpecVersionString() << ")\n";
  ss << "Checked rule groups: " << result.checked_groups.group_summary()
     << "\n";

  const std::vector<const USDValidationIssue *> ordered =
      GetOrderedValidationIssues(result);

  if (!ordered.empty()) {
    ss << "\n";
    for (const USDValidationIssue *issue : ordered) {
      // Fixed-width severity keeps rule ids aligned.
      ss << (issue->severity == USDValidationSeverity::Error ? "ERROR"
                                                             : "WARN ")
         << " [" << issue->rule_id << "] ";

      if (!issue->location.empty()) {
        ss << issue->location << ": ";
      }

      ss << issue->message << "\n";
    }
  }

  const size_t errors = result.error_count();
  const size_t warnings = result.warning_count();

  ss << "\nResult: ";
  if (errors > 0) {
    ss << "FAILED - " << Pluralize(errors, "error") << ", "
       << Pluralize(warnings, "warning") << "\n";
  } else if (warnings > 0) {
    ss << "PASSED with warnings - " << Pluralize(warnings, "warning")
       << "\n";
  } else {
    ss << "PASSED - no issues found\n";
  }

  return ss.str();
}

std::string ValidationOptions::group_summary() const {
  std::string s;
  const auto add = [&](bool enabled, const char *name) {
    if (!enabled) {
      return;
    }
    if (!s.empty()) {
      s += ", ";
    }
    s += name;
  };
  add(core, "core");
  add(geom, "geom");
  add(shade, "shade");
  add(lux, "lux");
  add(physics, "physics");
  add(crate, "crate");
  if (s.empty()) {
    s = "(none)";
  }
  return s;
}

}  // namespace lightusd
