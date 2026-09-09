// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Light Transport Entertainment Inc.
//
// LightUSD Next - AOUSD Core semantic validation.
//
// Port of the legacy validator (src/usd-validation.cc) onto the next-core
// layer model. Rule ids / severities / messages mirror the legacy validator.
//
// Legacy checks that are not fully expressible on next::Layer / next::PrimSpec
// are skipped or structurally approximated:
//  - crate/package rules require the original bytes and are run by callers
//    such as lusdchecker, not by Layer-only validation.
//  - API-schema validation uses next's compact built-in inventory rather than
//    OpenUSD's dynamically discovered plugin registry.
//  - Invalid list-edit qualifiers: next ArcEdit qualifiers are typed enums;
//    an invalid qualifier is rejected at parse time and cannot be
//    represented.
//  - layerRelocates live per-prim in next (PrimSpecMeta::relocates); they are
//    validated at the prim location under the legacy core.layer.layerRelocates
//    rule ids.

#include "usd-validation-internal.hh"

#include "../prim/identifier.hh"
#include "../schema/schema-registry.hh"

#include "../../external/fast_float/include/fast_float/fast_float.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../composition/composition.hh"
#include "../layer/prim-spec.hh"
#include "../pcp/layer-registry.hh"
#include "../prim/path.hh"
#include "../types/type-id.hh"
#include "../types/value.hh"
#include "color-transform.hh"

namespace lightusd {
namespace next {
namespace validation_detail {

constexpr const char *kLayerLocation = "<layer>";
constexpr const char *kSpecVersion = "AOUSD Core Spec v1.0.1 (2025-12-12)";


bool HasAppliedSchema(const std::vector<AppliedSchema> &schemas,
                      const std::string &schema_name);


// ---------------------------------------------------------------------------
// Small string utilities
// ---------------------------------------------------------------------------

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

bool StartsWith(const std::string &s, const std::string &prefix) {
  return s.rfind(prefix, 0) == 0;
}

bool EndsWith(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Identifier validation delegates to the shared strict UTF-8 + Unicode XID
// validator (src/next/prim/identifier.hh) — the same implementation the
// parser and lexer use. The previous file-local approximation accepted ANY
// byte >= 0x80, so malformed UTF-8 and non-XID codepoints passed validation.
// (The shared IsValidIdentifier / IsValidNamespacedIdentifier are visible
// here through the enclosing lightusd::next namespace.)

// ---------------------------------------------------------------------------
// Issue helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Type-name sets (kept in sync with the legacy validator)
// ---------------------------------------------------------------------------

bool IsGprimTypeName(const std::string &type_name) {
  static const std::unordered_set<std::string> kGprimTypes = {
      "GPrim",       "Mesh",        "Sphere",       "Cube",
      "Cone",        "Cylinder",    "Cylinder_1",   "Capsule",
      "Capsule_1",   "Points",      "BasisCurves",  "NurbsCurves",
      "NurbsPatch",  "HermiteCurves", "Plane",      "TetMesh",
      "PointInstancer",
  };
  // Types whose ancestry the schema registry records (e.g. Volume -> Gprim)
  // derive their placement from it instead of growing this hand list.
  return kGprimTypes.count(type_name) > 0 ||
         GetSchemaRegistry().InheritsFrom(type_name, "Gprim");
}

bool IsXformableTypeName(const std::string &type_name) {
  static const std::unordered_set<std::string> kNonXformableTypes = {
      "", "Scope", "GeomSubset", "Material", "Shader", "NodeGraph",
      "PhysicsScene", "PhysicsCollisionGroup"};
  if (kNonXformableTypes.count(type_name) > 0) return false;
  // Registry-recorded ancestry (e.g. RenderSettings -> RenderSettingsBase ->
  // Typed) decides for the schemas the registry knows; types without a
  // recorded parent keep the permissive default.
  const SchemaRegistry &registry = GetSchemaRegistry();
  if (registry.HasParentEntry(type_name)) {
    return registry.InheritsFrom(type_name, "Xformable");
  }
  return true;
}

bool IsShadeContainerTypeName(const std::string &type_name) {
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
  static const std::unordered_set<std::string> kNames = {
      "PhysicsRigidBodyAPI", "PhysicsCollisionAPI", "PhysicsMaterialAPI",
      "PhysicsMeshCollisionAPI", "PhysicsMassAPI", "PhysicsFilteredPairsAPI",
      "PhysicsArticulationRootAPI", "PhysicsDriveAPI", "PhysicsLimitAPI",
      "MjcSceneAPI", "MjcJointAPI", "MjcCollisionAPI", "MjcMeshCollisionAPI",
      "MjcMaterialAPI", "MjcSiteAPI", "MjcImageableAPI", "MjcEqualityAPI",
      "MjcEqualityConnectAPI", "MjcEqualityWeldAPI", "MjcEqualityJointAPI",
      "NewtonSceneAPI", "NewtonXpbdSceneAPI", "NewtonKaminoSceneAPI",
      "NewtonArticulationRootAPI", "NewtonCollisionAPI",
      "NewtonMeshCollisionAPI", "NewtonMaterialAPI", "NewtonMimicAPI",
      "NewtonActuatorDelayAPI", "NewtonPDControlAPI", "NewtonPIDControlAPI",
      "NewtonNeuralControlAPI", "NewtonMaxEffortClampingAPI",
      "NewtonDCMotorClampingAPI", "NewtonPositionBasedClampingAPI",
      "Preliminary_PhysicsMaterialAPI", "Preliminary_PhysicsRigidBodyAPI",
      "Preliminary_PhysicsColliderAPI"};
  return kNames.count(schema_name) > 0;
}

bool IsShaderInputName(const std::string &prop_name) {
  return StartsWith(prop_name, "inputs:") &&
         prop_name.size() > std::string("inputs:").size();
}

bool IsShaderOutputName(const std::string &prop_name) {
  return StartsWith(prop_name, "outputs:") &&
         prop_name.size() > std::string("outputs:").size();
}

bool IsMaterialTerminalOutputName(const std::string &prop_name) {
  return prop_name == "outputs:surface" ||
         prop_name == "outputs:displacement" || prop_name == "outputs:volume" ||
         prop_name == "outputs:mtlx:surface" ||
         prop_name == "outputs:mtlx:displacement" ||
         prop_name == "outputs:mtlx:volume";
}

bool IsUsdPrimvarReaderId(const std::string &shader_id) {
  return StartsWith(shader_id, "UsdPrimvarReader_");
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
  return StartsWith(shader_id, "ND_") || StartsWith(shader_id, "MaterialX") ||
         shader_id == "MtlxUsdPreviewSurface" ||
         shader_id == "MtlxAutodeskStandardSurface" ||
         shader_id == "MtlxOpenPBRSurface";
}

bool IsMaterialXAssetPath(const std::string &asset_path) {
  return EndsWith(asset_path, ".mtlx") || EndsWith(asset_path, ".MTLX");
}

bool IsMultipleApplySchemaName(const std::string &schema_name) {
  return schema_name == "CollectionAPI" || schema_name == "PhysicsDriveAPI" ||
         schema_name == "PhysicsLimitAPI" ||
         schema_name == "ColorSpaceDefinitionAPI";
}

bool IsKnownAPISchemaName(const std::string &schema_name) {
  if (IsPhysicsSchemaName(schema_name)) return true;
  static const std::unordered_set<std::string> kNames = {
      "CollectionAPI",          "ColorSpaceAPI",
      "ColorSpaceDefinitionAPI", "SkelBindingAPI",
      "MaterialBindingAPI",     "MaterialXConfigAPI",
      "ShapingAPI",             "ShadowAPI",
      "LightAPI",               "MeshLightAPI",
      "VolumeLightAPI",         "PortalAPI",
      "LightListAPI",           "MotionAPI",
      "PrimvarsAPI",            "VisibilityAPI",
      "ModelAPI",               "AssetPreviewsAPI",
      "NodeDefAPI",             "CoordSysAPI"};
  return kNames.count(schema_name) != 0;
}

// Imageable-but-not-connectable prim types for the Material-encapsulation
// rule. Everything a UsdGeomImageable-derived schema covers except the
// UsdShade connectables (Material/NodeGraph/Shader are handled by
// IsShadeConnectableTypeName) and lights (LightAPI makes lights connectable).
bool IsNonConnectableImageableTypeName(const std::string &type_name) {
  if (IsGprimTypeName(type_name)) return true;
  static const std::unordered_set<std::string> kTypes = {
      "Scope", "Xform", "SkelRoot", "Skeleton", "Camera", "GeomSubset",
  };
  return kTypes.count(type_name) != 0;
}

// UsdPreviewSurface input name -> expected (role) value type. Shares the
// generated table with the legacy validator (see
// scripts/gen-usd-preview-surface-inputs.py).
const std::unordered_map<std::string, std::string> &
UsdPreviewSurfaceInputTypes() {
  static const std::unordered_map<std::string, std::string> kInputs = {
#include "../../usd-preview-surface-inputs.inc"
  };
  return kInputs;
}

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

// ---------------------------------------------------------------------------
// Path validation (string-based; next::Path is an unvalidated string wrapper)
// ---------------------------------------------------------------------------

void SplitScenePathString(const std::string &path, std::string *prim_part,
                          std::string *prop_part) {
  const size_t dot = path.find('.');
  if (dot == std::string::npos) {
    if (prim_part) *prim_part = path;
    if (prop_part) prop_part->clear();
    return;
  }
  if (prim_part) *prim_part = path.substr(0, dot);
  if (prop_part) *prop_part = path.substr(dot + 1);
}

// Absolute prim path: "/", or "/Elem(/Elem)*" with identifier elements and no
// property part.
bool IsValidAbsolutePrimPathString(const std::string &path, std::string *err) {
  if (path.empty()) {
    if (err) *err = "path is empty";
    return false;
  }
  if (path[0] != '/') {
    if (err) *err = "path must be absolute (start with '/')";
    return false;
  }
  if (path.find('.') != std::string::npos) {
    if (err) *err = "prim path must not contain a property part";
    return false;
  }
  if (path == "/") {
    return true;
  }
  if (path.find("//") != std::string::npos) {
    if (err) *err = "path contains an empty element (`//`)";
    return false;
  }
  if (path.back() == '/') {
    if (err) *err = "path must not end with '/'";
    return false;
  }
  const std::vector<std::string> elems = SplitString(path.substr(1), '/');
  for (const std::string &elem : elems) {
    if (!IsValidIdentifier(elem)) {
      if (err) *err = "path element `" + elem + "` is not a valid identifier";
      return false;
    }
  }
  return true;
}

// Relationship / composition-arc target: absolute non-root prim path.
bool IsValidAbsolutePrimTargetPathString(const std::string &path,
                                         std::string *err) {
  if (!IsValidAbsolutePrimPathString(path, err)) {
    return false;
  }
  if (path == "/") {
    if (err) *err = "target must be an absolute non-root prim path";
    return false;
  }
  return true;
}

// Scene path (relationship target that may address a property).
bool IsValidScenePathString(const std::string &path, std::string *err) {
  std::string prim_part;
  std::string prop_part;
  SplitScenePathString(path, &prim_part, &prop_part);
  if (path.find('.') != std::string::npos && prop_part.empty()) {
    if (err) *err = "path has an empty property part";
    return false;
  }
  if (!IsValidAbsolutePrimPathString(prim_part, err)) {
    return false;
  }
  if (!prop_part.empty() && !IsValidNamespacedIdentifier(prop_part)) {
    if (err) *err = "property part `" + prop_part + "` is not a valid name";
    return false;
  }
  return true;
}

// Connection target: absolute prim path plus a mandatory property part.
bool IsValidConnectionTargetPathString(const std::string &path,
                                       std::string *err) {
  std::string prim_part;
  std::string prop_part;
  SplitScenePathString(path, &prim_part, &prop_part);
  if (prop_part.empty()) {
    if (err) *err = "connection target must be a prim property path";
    return false;
  }
  if (!IsValidAbsolutePrimPathString(prim_part, err)) {
    return false;
  }
  if (!IsValidNamespacedIdentifier(prop_part)) {
    if (err) *err = "property part `" + prop_part + "` is not a valid name";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// PrimSpec property adapters. In next-core, attributes live in the PropIndex
// (PropSlot) while relationships live in a separate name->targets map.
// ---------------------------------------------------------------------------

const PropSlot *GetSlot(const PrimSpec &ps, const std::string &name) {
  return ps.property(name);
}

bool IsRelationshipProp(const PrimSpec &ps, const std::string &name) {
  if (ps.relationship(name)) {
    return true;
  }
  const PropSlot *slot = GetSlot(ps, name);
  return slot && slot->is_relationship();
}

bool HasProperty(const PrimSpec &ps, const std::string &name) {
  return GetSlot(ps, name) != nullptr || ps.relationship(name) != nullptr;
}

bool HasAttributeProp(const PrimSpec &ps, const std::string &name) {
  const PropSlot *slot = GetSlot(ps, name);
  return slot && !slot->is_relationship();
}

bool HasConnections(const PrimSpec &ps, const std::string &name) {
  const std::vector<Path> *conns = ps.connection(name);
  if (conns && !conns->empty()) {
    return true;
  }
  const PropSlot *slot = GetSlot(ps, name);
  return slot && slot->is_connection();
}

// Authored default value of an attribute (nullptr when the slot is missing, a
// relationship, connection/declared-only, or an authored `= None` block).
const Value *GetAttrValue(const PrimSpec &ps, const std::string &name) {
  const PropSlot *slot = GetSlot(ps, name);
  if (!slot || slot->is_relationship()) {
    return nullptr;
  }
  const Value *v = ps.property_value(name);
  if (!v || v->is_empty() || v->is_block()) {
    return nullptr;
  }
  return v;
}

const std::vector<Path> &RelTargets(const PrimSpec &ps,
                                    const std::string &name) {
  static const std::vector<Path> kEmpty;
  const std::vector<Path> *targets = ps.relationship(name);
  return targets ? *targets : kEmpty;
}

// Declared USD type name of a property, or a name derived from the authored
// value type. Empty when neither is available.
std::string AttrTypeNameOf(const PrimSpec &ps, const std::string &name) {
  const std::string *declared = ps.property_type_name(name);
  if (declared && !declared->empty()) {
    std::string s = *declared;
    // Compare the element type; role information is preserved in declared
    // names ("color3f", "texCoord2f", ...).
    if (EndsWith(s, "[]")) {
      s = s.substr(0, s.size() - 2);
    }
    return s;
  }
  const Value *v = GetAttrValue(ps, name);
  if (!v) {
    return std::string();
  }
  const char *n = GetTypeName(v->type_id());
  return n ? std::string(n) : std::string();
}

// ---------------------------------------------------------------------------
// Value adapters
// ---------------------------------------------------------------------------

bool ValueToDouble(const Value &v, double *out) {
  if (!out || v.is_array()) {
    return false;
  }
  if (const float *f = v.as_float()) {
    *out = static_cast<double>(*f);
    return true;
  }
  if (const double *d = v.as_double()) {
    *out = *d;
    return true;
  }
  if (const int32_t *i = v.as_int()) {
    *out = static_cast<double>(*i);
    return true;
  }
  if (const int64_t *i64 = v.as_int64()) {
    *out = static_cast<double>(*i64);
    return true;
  }
  if (const uint32_t *u = v.as_uint()) {
    *out = static_cast<double>(*u);
    return true;
  }
  return false;
}

bool ValueToInt64(const Value &v, int64_t *out) {
  if (!out || v.is_array()) {
    return false;
  }
  if (const int32_t *i = v.as_int()) {
    *out = *i;
    return true;
  }
  if (const int64_t *i64 = v.as_int64()) {
    *out = *i64;
    return true;
  }
  if (const uint32_t *u = v.as_uint()) {
    *out = static_cast<int64_t>(*u);
    return true;
  }
  return false;
}

bool IsFloatTriple(TypeId id) {
  return id == TypeId::Float3 || id == TypeId::Point3f ||
         id == TypeId::Vector3f || id == TypeId::Normal3f;
}

bool IsDoubleTriple(TypeId id) {
  return id == TypeId::Double3 || id == TypeId::Point3d ||
         id == TypeId::Vector3d || id == TypeId::Normal3d;
}

bool ValueToVec3(const Value &v, std::array<double, 3> *out) {
  if (!out || v.is_array()) {
    return false;
  }
  if (IsFloatTriple(v.type_id())) {
    const float *p = static_cast<const float *>(v.raw_data());
    if (!p) return false;
    (*out)[0] = static_cast<double>(p[0]);
    (*out)[1] = static_cast<double>(p[1]);
    (*out)[2] = static_cast<double>(p[2]);
    return true;
  }
  if (IsDoubleTriple(v.type_id())) {
    const double *p = static_cast<const double *>(v.raw_data());
    if (!p) return false;
    (*out)[0] = p[0];
    (*out)[1] = p[1];
    (*out)[2] = p[2];
    return true;
  }
  return false;
}

bool ValueToQuat(const Value &v, std::array<double, 4> *out) {
  if (!out || v.is_array()) {
    return false;
  }
  if (v.type_id() == TypeId::Quatf) {
    const float *p = static_cast<const float *>(v.raw_data());
    if (!p) return false;
    for (size_t i = 0; i < 4; i++) (*out)[i] = static_cast<double>(p[i]);
    return true;
  }
  if (v.type_id() == TypeId::Quatd) {
    const double *p = static_cast<const double *>(v.raw_data());
    if (!p) return false;
    for (size_t i = 0; i < 4; i++) (*out)[i] = p[i];
    return true;
  }
  return false;
}

bool ValueToFloat2(const Value &v, std::array<float, 2> *out) {
  if (!out || v.is_array()) {
    return false;
  }
  if (v.type_id() == TypeId::Float2 || v.type_id() == TypeId::Texcoord2f) {
    const float *p = static_cast<const float *>(v.raw_data());
    if (!p) return false;
    (*out)[0] = p[0];
    (*out)[1] = p[1];
    return true;
  }
  return false;
}

bool GetTokenProperty(const PrimSpec &ps, const std::string &name,
                      std::string *out) {
  if (!out) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  const std::string *tok = v->as_token();
  if (!tok) return false;
  *out = *tok;
  return true;
}

bool GetStringProperty(const PrimSpec &ps, const std::string &name,
                       std::string *out) {
  if (!out) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  const std::string *str = v->as_string();
  if (!str) return false;
  *out = *str;
  return true;
}

bool GetStringLikeProperty(const PrimSpec &ps, const std::string &name,
                           std::string *out) {
  return GetTokenProperty(ps, name, out) || GetStringProperty(ps, name, out);
}

bool GetAssetPathProperty(const PrimSpec &ps, const std::string &name,
                          std::string *out) {
  if (!out) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  const std::string *asset = v->as_asset_path();
  if (!asset) return false;
  *out = *asset;
  return true;
}

bool GetNumericScalarProperty(const PrimSpec &ps, const std::string &name,
                              double *out) {
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  return ValueToDouble(*v, out);
}

bool GetIntegerScalarProperty(const PrimSpec &ps, const std::string &name,
                              int64_t *out) {
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  return ValueToInt64(*v, out);
}

bool GetIntArrayProperty(const PrimSpec &ps, const std::string &name,
                         std::vector<int32_t> *out) {
  if (!out) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v || v->type_id() != TypeId::Int || !v->is_array()) return false;
  const std::vector<int32_t> *arr = v->as_int_array();
  if (!arr) return false;
  *out = *arr;
  return true;
}

// Scalar float[] / double[] arrays converted to double.
bool ValueToDoubleScalarArray(const Value &v, std::vector<double> *out) {
  if (!out || !v.is_array()) return false;
  if (v.type_id() == TypeId::Double || v.type_id() == TypeId::TimeCode) {
    const std::vector<double> *arr = v.as_double_array();
    if (!arr) return false;
    *out = *arr;
    return true;
  }
  if (v.type_id() == TypeId::Float) {
    const std::vector<float> *arr = v.as_float_array();
    if (!arr) return false;
    out->assign(arr->begin(), arr->end());
    return true;
  }
  return false;
}

bool GetDoubleArrayProperty(const PrimSpec &ps, const std::string &name,
                            std::vector<double> *out) {
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  return ValueToDoubleScalarArray(*v, out);
}

bool GetFloatArrayProperty(const PrimSpec &ps, const std::string &name,
                           std::vector<float> *out) {
  if (!out) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v || !v->is_array()) return false;
  if (v->type_id() == TypeId::Float) {
    const std::vector<float> *arr = v->as_float_array();
    if (!arr) return false;
    *out = *arr;
    return true;
  }
  if (v->type_id() == TypeId::Double) {
    const std::vector<double> *arr = v->as_double_array();
    if (!arr) return false;
    out->assign(arr->begin(), arr->end());
    return true;
  }
  return false;
}

bool GetTokenArrayProperty(const PrimSpec &ps, const std::string &name,
                           std::vector<std::string> *out) {
  if (!out) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v || !v->is_array()) return false;
  const std::vector<std::string> *arr = v->as_token_array();
  if (!arr) return false;
  *out = *arr;
  return true;
}

// Generic array length (any element type; lazy arrays report their size
// without decoding).
bool GetValueArrayLength(const Value &v, size_t *size) {
  if (!size || !v.is_array() || v.is_block()) {
    return false;
  }
  *size = v.array_size();
  return true;
}

bool GetArrayLengthProperty(const PrimSpec &ps, const std::string &name,
                            size_t *size) {
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  return GetValueArrayLength(*v, size);
}

// points/positions: array of 3-component float/double elements.
bool GetPoint3ArrayLen(const PrimSpec &ps, const std::string &name,
                       size_t *size) {
  if (!size) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v || !v->is_array()) return false;
  if (!IsFloatTriple(v->type_id()) && !IsDoubleTriple(v->type_id())) {
    return false;
  }
  *size = v->array_size();
  return true;
}

// extent: float3[2] (Float3-family array of 2, or the crate Extent scalar).
bool GetExtentProperty(const PrimSpec &ps, const std::string &name,
                       std::vector<std::array<float, 3>> *out) {
  if (!out) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  out->clear();
  if (!v->is_array() && v->type_id() == TypeId::Extent) {
    const float *p = static_cast<const float *>(v->raw_data());
    if (!p) return false;
    out->push_back({p[0], p[1], p[2]});
    out->push_back({p[3], p[4], p[5]});
    return true;
  }
  if (v->is_array() && IsFloatTriple(v->type_id())) {
    const std::vector<float> *flat = v->as_float_array();
    if (!flat) return false;
    for (size_t i = 0; i + 2 < flat->size(); i += 3) {
      out->push_back({(*flat)[i], (*flat)[i + 1], (*flat)[i + 2]});
    }
    return true;
  }
  if (v->is_array() && IsDoubleTriple(v->type_id())) {
    const std::vector<double> *flat = v->as_double_array();
    if (!flat) return false;
    for (size_t i = 0; i + 2 < flat->size(); i += 3) {
      out->push_back({static_cast<float>((*flat)[i]),
                      static_cast<float>((*flat)[i + 1]),
                      static_cast<float>((*flat)[i + 2])});
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Time samples
// ---------------------------------------------------------------------------

const std::vector<std::pair<double, uint32_t>> *GetTimeSamplesOf(
    const PrimSpec &ps, const std::string &name) {
  const PropNameId id = GetPropNameTable().find(name);
  if (!id.is_valid()) {
    return nullptr;
  }
  return ps.time_samples(id);
}

void ValidateTimeSamplesProp(const PrimSpec &ps, const std::string &prop_name,
                             const std::string &rule_id,
                             const std::string &location,
                             USDValidationResult *result) {
  const auto *samples = GetTimeSamplesOf(ps, prop_name);
  if (!samples) {
    return;
  }
  // NOTE: next TimeSampleStorage keeps samples sorted with last-wins upsert,
  // so the legacy "strictly increasing" violation is normally unrepresentable;
  // the check is kept as a defensive guard.
  bool have_prev = false;
  double prev = 0.0;
  for (size_t i = 0; i < samples->size(); i++) {
    const double t = (*samples)[i].first;
    if (!std::isfinite(t)) {
      AddError(result, rule_id, location,
               "timeSamples sample " + std::to_string(i) +
                   " has a non-finite time");
    }
    if (have_prev && t <= prev) {
      AddError(result, rule_id, location,
               "timeSamples must be strictly increasing; sample " +
                   std::to_string(i) + " is at " + std::to_string(t) +
                   " after " + std::to_string(prev));
    }
    have_prev = true;
    prev = t;
  }
}

bool IsLengthCompatible(size_t count, size_t expected, bool allow_singleton) {
  return count == expected || (allow_singleton && count == 1);
}

// Default + every time sample of `prop_name` must be an array of `expected`
// elements (optionally allowing a length-1 constant).
void ValidateArrayLengthAtAllSamples(const PrimSpec &ps,
                                     const std::string &prop_name,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     size_t expected,
                                     const std::string &expected_name,
                                     USDValidationResult *result) {
  const Value *v = GetAttrValue(ps, prop_name);
  if (v) {
    size_t count = 0;
    if (GetValueArrayLength(*v, &count) && count != expected) {
      AddError(result, rule_id, location,
               prop_name + " has " + std::to_string(count) +
                   " default elements, but " + expected_name + " has " +
                   std::to_string(expected) + " elements");
    }
  }

  const auto *samples = GetTimeSamplesOf(ps, prop_name);
  if (!samples) {
    return;
  }
  ValidateTimeSamplesProp(ps, prop_name, rule_id, location, result);
  for (size_t i = 0; i < samples->size(); i++) {
    const Value *sv = ps.time_sample_value((*samples)[i].second);
    if (!sv || sv->is_empty() || sv->is_block()) {
      continue;
    }
    size_t count = 0;
    if (!GetValueArrayLength(*sv, &count)) {
      AddWarning(result, rule_id, location,
                 prop_name + " timeSamples sample " + std::to_string(i) +
                     " should be an array value");
      continue;
    }
    if (count != expected) {
      AddError(result, rule_id, location,
               prop_name + " timeSamples sample " + std::to_string(i) +
                   " at time " + std::to_string((*samples)[i].first) +
                   " has " + std::to_string(count) + " elements, but " +
                   expected_name + " has " + std::to_string(expected) +
                   " elements");
    }
  }
}

// ---------------------------------------------------------------------------
// Generic property validators (rule-id parameterized; ported from legacy)
// ---------------------------------------------------------------------------

bool IsPrimvarPropertyName(const std::string &prop_name) {
  return StartsWith(prop_name, "primvars:") &&
         prop_name.size() > std::string("primvars:").size() &&
         !EndsWith(prop_name, ":indices");
}

bool IsValidPrimvarInterpolation(const std::string &interp) {
  return interp == "constant" || interp == "uniform" ||
         interp == "vertex" || interp == "varying" ||
         interp == "faceVarying";
}

void ValidateTokenSetProperty(const PrimSpec &ps, const std::string &prop_name,
                              const std::set<std::string> &allowed,
                              const std::string &rule_id,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a token attribute");
    return;
  }
  // A declared-only or timeSamples-only attribute is valid authoring (the
  // schema fallback / animation supplies the value); only an authored
  // DEFAULT of the wrong type is an error. GetAttrValue returns null for
  // slots without an authored default.
  if (const Value *v = GetAttrValue(ps, prop_name)) {
    const std::string *token = v->as_token();
    if (!token) {
      AddError(result, rule_id, location,
               prop_name + " must have a token value");
      return;
    }
    if (allowed.count(*token) == 0) {
      AddError(result, rule_id, location,
               prop_name + " token `" + *token + "` is not valid");
      return;
    }
  }
  // Animated tokens must also stay inside the allowed set. One diagnostic
  // (the first offending sample) per property keeps reports readable.
  if (const std::vector<std::pair<double, uint32_t>> *samples =
          GetTimeSamplesOf(ps, prop_name)) {
    for (const auto &sample : *samples) {
      const Value *sv = ps.time_sample_value(sample.second);
      if (!sv || sv->is_block()) continue;
      const std::string *token = sv->as_token();
      if (token && allowed.count(*token) > 0) continue;
      AddError(result, rule_id, location,
               prop_name + " timeSample at " + std::to_string(sample.first) +
                   (token ? " token `" + *token + "` is not valid"
                          : " must be a token value"));
      return;
    }
  }
}

void ValidatePositiveNumericProperty(const PrimSpec &ps,
                                     const std::string &prop_name,
                                     const std::string &rule_id,
                                     const std::string &prim_location,
                                     USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
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
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  double value = 0.0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetNumericScalarProperty(ps, prop_name, &value)) {
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
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  int64_t value = 0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetIntegerScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have an integer scalar value");
    return;
  }
  if (value < 0) {
    AddError(result, rule_id, location, prop_name + " must be non-negative");
  }
}

void ValidateFiniteVec3Property(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result,
                                bool require_nonzero) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a 3-component numeric attribute");
    return;
  }
  std::array<double, 3> value;
  const Value *v = GetAttrValue(ps, prop_name);
  if (!v || !ValueToVec3(*v, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a 3-component numeric value");
    return;
  }
  const double len2 =
      value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
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
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a 3-component numeric attribute");
    return;
  }
  std::array<double, 3> value;
  const Value *v = GetAttrValue(ps, prop_name);
  if (!v || !ValueToVec3(*v, &value)) {
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
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a quaternion attribute");
    return;
  }
  std::array<double, 4> value;
  const Value *v = GetAttrValue(ps, prop_name);
  if (!v || !ValueToQuat(*v, &value)) {
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
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
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
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
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
                                const std::string &rel_name, size_t *count) {
  if (!count) {
    return false;
  }
  if (!IsRelationshipProp(ps, rel_name)) {
    return false;
  }
  *count = RelTargets(ps, rel_name).size();
  return true;
}

void ValidateIntArrayLengthMatchesRelationship(
    const PrimSpec &ps, const std::string &array_prop,
    const std::string &rel_prop, const std::string &rule_id,
    const std::string &prim_location, USDValidationResult *result) {
  if (!HasProperty(ps, array_prop)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, array_prop);
  std::vector<int32_t> values;
  if (IsRelationshipProp(ps, array_prop) ||
      !GetIntArrayProperty(ps, array_prop, &values)) {
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
  if (!HasProperty(ps, array_prop)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, array_prop);
  std::vector<double> values;
  if (IsRelationshipProp(ps, array_prop) ||
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

void ValidateNumericMinMaxPair(const PrimSpec &ps, const std::string &min_prop,
                               const std::string &max_prop,
                               const std::string &rule_id,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  for (const std::string &prop_name : {min_prop, max_prop}) {
    if (HasProperty(ps, prop_name)) {
      double value = 0.0;
      if (IsRelationshipProp(ps, prop_name) ||
          !GetNumericScalarProperty(ps, prop_name, &value) ||
          !std::isfinite(value)) {
        AddError(result, rule_id,
                 MakePropertyLocation(prim_location, prop_name),
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
                                 const std::string &prop_name, size_t expected,
                                 bool allow_singleton,
                                 const std::string &rule_id,
                                 const std::string &prim_location,
                                 const std::string &expected_name,
                                 USDValidationResult *result) {
  if (!HasAttributeProp(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  const Value *v = GetAttrValue(ps, prop_name);
  size_t count = 0;
  if (v && GetValueArrayLength(*v, &count) &&
      !IsLengthCompatible(count, expected, allow_singleton)) {
    AddError(result, rule_id, location,
             prop_name + " has " + std::to_string(count) +
                 " elements, but must have " +
                 (allow_singleton ? "1 or " : "") + std::to_string(expected) +
                 " elements to match " + expected_name);
  }
  const auto *samples = GetTimeSamplesOf(ps, prop_name);
  if (!samples) {
    return;
  }
  ValidateTimeSamplesProp(ps, prop_name, rule_id, location, result);
  for (size_t i = 0; i < samples->size(); i++) {
    const Value *sv = ps.time_sample_value((*samples)[i].second);
    if (!sv || sv->is_empty() || sv->is_block()) {
      continue;
    }
    size_t sample_count = 0;
    if (!GetValueArrayLength(*sv, &sample_count)) {
      AddWarning(result, rule_id, location,
                 prop_name + " timeSamples sample " + std::to_string(i) +
                     " should be an array value");
      continue;
    }
    if (!IsLengthCompatible(sample_count, expected, allow_singleton)) {
      AddError(result, rule_id, location,
               prop_name + " timeSamples sample " + std::to_string(i) +
                   " at time " + std::to_string((*samples)[i].first) +
                   " has " + std::to_string(sample_count) +
                   " elements, but must have " +
                   (allow_singleton ? "1 or " : "") +
                   std::to_string(expected) + " elements to match " +
                   expected_name);
    }
  }
}

void ValidateNumericRangeProperty(const PrimSpec &ps,
                                  const std::string &prop_name,
                                  double min_value, double max_value,
                                  const std::string &rule_id,
                                  const std::string &prim_location,
                                  USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  double value = 0.0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetNumericScalarProperty(ps, prop_name, &value)) {
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

void ValidateNumericMinProperty(const PrimSpec &ps,
                                const std::string &prop_name, double min_value,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  double value = 0.0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a numeric attribute");
    return;
  }
  if (!std::isfinite(value) || value < min_value) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be finite and >= " + std::to_string(min_value));
  }
}

void ValidateFiniteNumericProperty(const PrimSpec &ps,
                                   const std::string &prop_name,
                                   const std::string &rule_id,
                                   const std::string &prim_location,
                                   USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  double value = 0.0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a numeric attribute");
    return;
  }
  if (!std::isfinite(value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be finite");
  }
}

void ValidateAssetPathProperty(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &rule_id,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  std::string asset;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetAssetPathProperty(ps, prop_name, &asset)) {
    // A connected asset input has no local value; treat a connection-only
    // property as satisfying the "asset attribute" requirement.
    if (HasConnections(ps, prop_name) && !IsRelationshipProp(ps, prop_name)) {
      return;
    }
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be an asset attribute");
    return;
  }
  if (asset.empty()) {
    AddWarning(result, rule_id, MakePropertyLocation(prim_location, prop_name),
               prop_name + " is authored as an empty asset path");
  }
}

// ---------------------------------------------------------------------------
// Relationship / connection target validators
// ---------------------------------------------------------------------------

const std::string *FindPrimType(const PrimTypeByPath &prim_types,
                                const std::string &prim_path) {
  auto it = prim_types.find(prim_path);
  if (it == prim_types.end()) {
    return nullptr;
  }
  return &it->second;
}

const PrimSpecifier *FindPrimSpecifier(
    const PrimSpecifierByPath &prim_specifiers, const std::string &prim_path) {
  auto it = prim_specifiers.find(prim_path);
  if (it == prim_specifiers.end()) {
    return nullptr;
  }
  return &it->second;
}

// Targets must be absolute non-root prim paths; empty target list warns.
void ValidateRelationshipTargetPaths(const PrimSpec &ps,
                                     const std::string &rel_name,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     USDValidationResult *result) {
  if (!IsRelationshipProp(ps, rel_name)) {
    AddError(result, rule_id, location, "expected a relationship");
    return;
  }
  const std::vector<Path> &targets = RelTargets(ps, rel_name);
  if (targets.empty()) {
    AddWarning(result, rule_id, location, "relationship has no targets");
    return;
  }
  for (const Path &target : targets) {
    std::string err;
    if (!IsValidAbsolutePrimTargetPathString(target.str(), &err)) {
      AddError(result, rule_id, location,
               "contains invalid target path `" + target.str() + "`: " + err);
    }
  }
}

// Targets may be scene paths (prim or property paths); used by CollectionAPI.
void ValidateRelationshipScenePaths(const PrimSpec &ps,
                                    const std::string &rel_name,
                                    const std::string &rule_id,
                                    const std::string &location,
                                    USDValidationResult *result) {
  for (const Path &target : RelTargets(ps, rel_name)) {
    std::string err;
    if (!IsValidScenePathString(target.str(), &err)) {
      AddError(result, rule_id, location,
               "contains invalid target path `" + target.str() + "`: " + err);
    }
  }
}

void ValidateRelationshipTargetPrimTypes(
    const PrimSpec &ps, const std::string &rel_name,
    const std::string &rule_id, const std::string &location,
    const PrimTypeByPath &prim_types,
    const std::set<std::string> &expected_types, USDValidationResult *result) {
  if (!IsRelationshipProp(ps, rel_name)) {
    return;
  }
  for (const Path &target : RelTargets(ps, rel_name)) {
    std::string prim_part;
    SplitScenePathString(target.str(), &prim_part, nullptr);
    const std::string *target_type = FindPrimType(prim_types, prim_part);
    if (target_type && !target_type->empty() &&
        expected_types.count(*target_type) == 0) {
      AddWarning(result, rule_id, location,
                 "relationship target `" + prim_part + "` has type `" +
                     *target_type + "`");
    }
  }
}

void ValidateConnectionTargets(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &rule_id,
                               const std::string &location,
                               const PrimTypeByPath &prim_types,
                               USDValidationResult *result,
                               bool require_output_target) {
  const std::vector<Path> *conns = ps.connection(prop_name);
  if (!conns) {
    return;
  }
  for (const Path &target : *conns) {
    std::string err;
    if (!IsValidConnectionTargetPathString(target.str(), &err)) {
      AddError(result, rule_id, location,
               "contains invalid connection target `" + target.str() +
                   "`: " + err);
      continue;
    }
    std::string prim_part;
    std::string target_prop;
    SplitScenePathString(target.str(), &prim_part, &target_prop);
    if (require_output_target && !IsShaderOutputName(target_prop)) {
      AddWarning(result, rule_id, location,
                 "connection target `" + target.str() +
                     "` is not an outputs:* source");
    } else if (!require_output_target && !IsShaderOutputName(target_prop) &&
               !IsShaderInputName(target_prop)) {
      AddWarning(result, rule_id, location,
                 "connection target `" + target.str() +
                     "` is not an inputs:* or outputs:* shade port");
    }
    const std::string *target_type = FindPrimType(prim_types, prim_part);
    if (target_type && !target_type->empty() &&
        !IsShadeConnectionTargetTypeName(*target_type)) {
      AddWarning(result, rule_id, location,
                 "connection target prim `" + prim_part + "` has type `" +
                     *target_type + "`, expected Material, Shader, or "
                     "NodeGraph");
    }
  }
}

// ---------------------------------------------------------------------------
// geom rules
// ---------------------------------------------------------------------------

// Ancestor-derived context threaded through the recursive prim walk.

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

  std::vector<std::array<float, 3>> extent;
  if (GetExtentProperty(ps, "extent", &extent)) {
    const std::string location = MakePropertyLocation(prim_location, "extent");
    if (extent.size() != 2) {
      AddError(result, "geom.gprim.extent", location,
               "extent must contain exactly 2 points");
    } else {
      auto finite_point = [](const std::array<float, 3> &p) {
        return std::isfinite(p[0]) && std::isfinite(p[1]) &&
               std::isfinite(p[2]);
      };
      if (!finite_point(extent[0]) || !finite_point(extent[1])) {
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
  const std::string &type_name = ps.type_name();
  if (type_name != "Points" && type_name != "PointInstancer" &&
      type_name != "BasisCurves" && type_name != "NurbsCurves" &&
      type_name != "HermiteCurves" && type_name != "NurbsPatch" &&
      GetPoint3ArrayLen(ps, "points", &point_count)) {
    ValidateArrayLengthProperty(ps, "velocities", point_count, false,
                                "geom.gprim.arraySize", prim_location,
                                "points", result);
    ValidateArrayLengthProperty(ps, "accelerations", point_count, false,
                                "geom.gprim.arraySize", prim_location,
                                "points", result);
  }
}

void ValidateGeomPrimitive(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  static const std::set<std::string> kAxis = {"X", "Y", "Z"};
  const std::string &type_name = ps.type_name();

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
  if (!GetPoint3ArrayLen(ps, "points", &point_count)) {
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

  if (ps.type_name() == "BasisCurves") {
    ValidateTokenSetProperty(ps, "type", kBasisCurveTypes,
                             "geom.curves.tokens", prim_location, result);
    ValidateTokenSetProperty(ps, "basis", kBasis, "geom.curves.tokens",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "wrap", kWrap, "geom.curves.tokens",
                             prim_location, result);
  } else if (ps.type_name() == "NurbsPatch") {
    ValidateTokenSetProperty(ps, "uForm", kForm, "geom.curves.tokens",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "vForm", kForm, "geom.curves.tokens",
                             prim_location, result);
  }

  size_t point_count = 0;
  const bool have_points = GetPoint3ArrayLen(ps, "points", &point_count);
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

// ---------------------------------------------------------------------------
// UsdVol / UsdRender structural checks (PRODUCT PARITY, not AOUSD Core):
// vol.volume.fieldRel / vol.fieldAsset.* / render.*
// ---------------------------------------------------------------------------

void ValidateVolume(const PrimSpec &ps, const std::string &prim_location,
                    USDValidationResult *result) {
  // Volume binds its fields through `field:<name>` RELATIONSHIPS; an
  // attribute in that namespace is an authoring error, and a bound field
  // relationship without a target renders nothing. Attributes live in the
  // property slots; relationships (including declared-only ones) live in the
  // prim's relationship map, so both containers are walked.
  for (const PropSlot &slot : ps.properties().slots()) {
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (prop_name.compare(0, 6, "field:") != 0) continue;
    if (slot.is_relationship()) continue;  // targets checked below
    AddError(result, "vol.volume.fieldRel",
             MakePropertyLocation(prim_location, prop_name),
             "Volume `" + prop_name +
                 "` must be a relationship targeting a field prim");
  }
  for (const std::string &rel_name : ps.relationship_names()) {
    if (rel_name.compare(0, 6, "field:") != 0) continue;
    const std::vector<Path> *targets = ps.relationship(rel_name);
    if (!targets || targets->empty()) {
      AddWarning(result, "vol.volume.fieldRel",
                 MakePropertyLocation(prim_location, rel_name),
                 "Volume field relationship `" + rel_name +
                     "` has no target");
    }
  }
}

void ValidateFieldAsset(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  static const std::set<std::string> kVdbDataTypes = {
      "half",  "float",  "double", "int",     "uint",     "int64",
      "half2", "float2", "double2", "int2",   "half3",    "float3",
      "double3", "int3", "matrix3d", "matrix4d", "quatd", "bool",
      "mask",  "string"};
  static const std::set<std::string> kField3dDataTypes = {
      "half", "float", "double", "half3", "float3", "double3"};
  static const std::set<std::string> kFieldClass = {"levelSet", "fogVolume",
                                                    "staggered", "unknown"};
  static const std::set<std::string> kVectorRole = {"None", "Point", "Normal",
                                                    "Vector", "Color"};

  std::string file;
  if (!HasAttributeProp(ps, "filePath")) {
    AddWarning(result, "vol.fieldAsset.filePath",
               MakePropertyLocation(prim_location, "filePath"),
               ps.type_name() + " should author filePath");
  } else {
    // A time-sampled filePath is the canonical animated-volume authoring
    // (one asset per sample); only a sample-less, default-less (or empty)
    // filePath warrants a diagnostic.
    const std::vector<std::pair<double, uint32_t>> *file_samples =
        GetTimeSamplesOf(ps, "filePath");
    if ((!file_samples || file_samples->empty()) &&
        (!GetAssetPathProperty(ps, "filePath", &file) || file.empty())) {
      AddWarning(result, "vol.fieldAsset.filePath",
                 MakePropertyLocation(prim_location, "filePath"),
                 ps.type_name() + " filePath should be a non-empty asset path");
    }
  }
  // pxr usdVol schema: for fieldDataType, "A missing value is considered an
  // error." That statement is about the COMPOSED stage; this checker sees one
  // layer, where the value may legitimately arrive from a sublayer or another
  // arc — so a missing VALUE (no default, no timeSamples; the registry has no
  // fallback either) reports as a warning, not a hard error.
  {
    const std::vector<std::pair<double, uint32_t>> *dt_samples =
        GetTimeSamplesOf(ps, "fieldDataType");
    if (!GetAttrValue(ps, "fieldDataType") &&
        (!dt_samples || dt_samples->empty())) {
      AddWarning(result, "vol.fieldAsset.dataType",
                 MakePropertyLocation(prim_location, "fieldDataType"),
                 ps.type_name() + " does not author a fieldDataType value "
                 "(pxr: a missing value is an error on the composed stage)");
    }
  }
  if (ps.type_name() == "OpenVDBAsset") {
    ValidateTokenSetProperty(ps, "fieldDataType", kVdbDataTypes,
                             "vol.fieldAsset.dataType", prim_location, result);
    ValidateTokenSetProperty(ps, "fieldClass", kFieldClass,
                             "vol.fieldAsset.fieldClass", prim_location,
                             result);
  } else if (ps.type_name() == "Field3DAsset") {
    ValidateTokenSetProperty(ps, "fieldDataType", kField3dDataTypes,
                             "vol.fieldAsset.dataType", prim_location, result);
  }
  ValidateTokenSetProperty(ps, "vectorDataRoleHint", kVectorRole,
                           "vol.fieldAsset.vectorDataRoleHint", prim_location,
                           result);
}

void ValidateParticleField(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &prim_location, USDValidationResult *result) {
  const bool gaussian = ps.type_name() == "ParticleField3DGaussianSplat";
  if (gaussian) {
    ValidateTokenSetProperty(ps, "projectionModeHint",
                             {"perspective", "tangential"},
                             "vol.particleField.projectionModeHint",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "sortingModeHint",
                             {"zDepth", "cameraDistance", "rayHitDistance"},
                             "vol.particleField.sortingModeHint",
                             prim_location, result);
  }

  const bool has_position_api =
      gaussian || HasAppliedSchema(applied_schemas,
                                   "ParticleFieldPositionAttributeAPI");
  const bool has_kernel_api =
      gaussian ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelGaussianEllipsoidAPI") ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelGaussianSurfletAPI") ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelConstantSurfletAPI");
  if (!has_position_api) {
    AddWarning(result, "vol.particleField.position", prim_location,
               "ParticleField has no applied position provider API");
  }
  if (!has_kernel_api) {
    AddWarning(result, "vol.particleField.kernel", prim_location,
               "ParticleField has no applied kernel API");
  }

  size_t particle_count = 0;
  bool have_positions = GetArrayLengthProperty(ps, "positions", &particle_count);
  if (!have_positions) {
    have_positions =
        GetArrayLengthProperty(ps, "positionsh", &particle_count);
  }
  if (!have_positions) return;

  // OpenUSD discards a short auxiliary array and truncates a long one. Report
  // the authored mismatch without treating that defined fallback behavior as
  // a fatal validation error.
  for (const char *prop_name :
       {"orientations", "orientationsh", "scales", "scalesh", "opacities",
        "opacitiesh"}) {
    size_t value_count = 0;
    if (GetArrayLengthProperty(ps, prop_name, &value_count) &&
        value_count != particle_count) {
      AddWarning(result, "vol.particleField.arraySize",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) + " has " +
                     std::to_string(value_count) + " elements for " +
                     std::to_string(particle_count) +
                     " particles; OpenUSD will " +
                     (value_count < particle_count ? "discard" : "truncate") +
                     " it");
    }
  }
}

void ValidateOpenUsd2608GeomAPIs(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &prim_location, USDValidationResult *result) {
  if (HasAppliedSchema(applied_schemas, "GeomModelAPI")) {
    ValidateTokenSetProperty(ps, "model:cardVisibility",
                             {"full", "simple", "inherited"},
                             "geom.model.cardVisibility", prim_location,
                             result);
  }
  for (const AppliedSchema &schema : applied_schemas) {
    if (schema.name != "BackPlateAPI" || schema.instance_name.empty()) continue;
    const std::string prop_name = "backPlate:" + schema.instance_name +
                                  ":plateVisibility";
    ValidateTokenSetProperty(ps, prop_name, {"all", "solo", "mute"},
                             "geom.backPlate.visibility", prim_location,
                             result);
  }
}

// pxr usdValidation:AttributeTypeMismatch for schema attributes whose
// DECLARED type must match the registry exactly. Empty means "not declared
// locally" and is fine.
void CheckDeclaredAttrType(const PrimSpec &ps, const char *prop_name,
                           const char *expected,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  const std::string authored = AttrTypeNameOf(ps, prop_name);
  if (!authored.empty() && authored != expected) {
    AddWarning(result, "core.schema.attributeType",
               MakePropertyLocation(prim_location, prop_name),
               std::string(prop_name) + " is declared `" + authored +
                   "` but the schema attribute type is `" + expected + "`");
  }
}

void ValidateRenderPrim(const PrimSpec &ps, const std::string &prim_location,
                        const PrimTypeByPath &prim_types,
                        USDValidationResult *result) {
  static const std::set<std::string> kConformPolicy = {
      "expandAperture", "cropAperture", "adjustApertureWidth",
      "adjustApertureHeight", "adjustPixelAspectRatio"};
  static const std::set<std::string> kSourceType = {"raw", "primvar", "lpe",
                                                    "intrinsic"};
  static const std::set<std::string> kCameraType = {"Camera"};
  static const std::set<std::string> kProductType = {"RenderProduct"};
  static const std::set<std::string> kVarType = {"RenderVar"};
  static const std::set<std::string> kRenderSourceTypes = {
      "RenderSettings", "RenderProduct"};
  static const std::set<std::string> kPassType = {"RenderPass"};
  const std::string &type_name = ps.type_name();
  if (type_name == "RenderSettings" || type_name == "RenderProduct") {
    // Rule ids name the prim type they fire on (render.settings.* vs
    // render.product.*) so id-based filtering points at the right schema.
    const bool is_settings = type_name == "RenderSettings";
    ValidateTokenSetProperty(ps, "aspectRatioConformPolicy", kConformPolicy,
                             is_settings
                                 ? "render.settings.aspectRatioConformPolicy"
                                 : "render.product.aspectRatioConformPolicy",
                             prim_location, result);
    // Per pxr, `products` is a RenderSettings property and `orderedVars` a
    // RenderProduct property; the same name on the OTHER type is an ordinary
    // custom property and must not be kind-checked.
    const char *rels[2] = {"camera",
                           is_settings ? "products" : "orderedVars"};
    for (const char *rel : rels) {
      if (HasProperty(ps, rel) && !IsRelationshipProp(ps, rel)) {
        AddError(result,
                 is_settings ? "render.settings.relationship"
                             : "render.product.relationship",
                 MakePropertyLocation(prim_location, rel),
                 std::string(rel) + " must be a relationship");
      }
    }
    if (!is_settings) {
      CheckDeclaredAttrType(ps, "productName", "token", prim_location, result);
    }
    ValidateRelationshipTargetPrimTypes(
        ps, "camera",
        is_settings ? "render.settings.targetType"
                    : "render.product.targetType",
        MakePropertyLocation(prim_location, "camera"), prim_types, kCameraType,
        result);
    ValidateRelationshipTargetPrimTypes(
        ps, is_settings ? "products" : "orderedVars",
        is_settings ? "render.settings.targetType"
                    : "render.product.targetType",
        MakePropertyLocation(prim_location,
                             is_settings ? "products" : "orderedVars"),
        prim_types, is_settings ? kProductType : kVarType, result);
  } else if (type_name == "RenderVar") {
    ValidateTokenSetProperty(ps, "sourceType", kSourceType,
                             "render.var.sourceType", prim_location, result);
    CheckDeclaredAttrType(ps, "sourceName", "string", prim_location, result);
  } else if (type_name == "RenderPass") {
    for (const char *rel : {"renderSource", "inputPasses"}) {
      if (HasProperty(ps, rel) && !IsRelationshipProp(ps, rel)) {
        AddError(result, "render.pass.relationship",
                 MakePropertyLocation(prim_location, rel),
                 std::string(rel) + " must be a relationship");
      }
    }
    ValidateRelationshipTargetPrimTypes(
        ps, "renderSource", "render.pass.targetType",
        MakePropertyLocation(prim_location, "renderSource"), prim_types,
        kRenderSourceTypes, result);
    ValidateRelationshipTargetPrimTypes(
        ps, "inputPasses", "render.pass.targetType",
        MakePropertyLocation(prim_location, "inputPasses"), prim_types,
        kPassType, result);
  }
}

void ValidatePointInstancer(const PrimSpec &ps,
                            const std::string &prim_location,
                            USDValidationResult *result) {
  size_t position_count = 0;
  const bool have_positions =
      GetPoint3ArrayLen(ps, "positions", &position_count);

  size_t prototype_count = 0;
  if (!HasProperty(ps, "prototypes")) {
    AddError(result, "geom.pointInstancer.prototypes",
             MakePropertyLocation(prim_location, "prototypes"),
             "PointInstancer must author rel prototypes");
  } else if (!IsRelationshipProp(ps, "prototypes")) {
    AddError(result, "geom.pointInstancer.prototypes",
             MakePropertyLocation(prim_location, "prototypes"),
             "prototypes must be a relationship");
  } else {
    const std::vector<Path> targets = RelTargets(ps, "prototypes");
    prototype_count = targets.size();
    if (targets.empty()) {
      AddError(result, "geom.pointInstancer.prototypes",
               MakePropertyLocation(prim_location, "prototypes"),
               "prototypes relationship must target at least one prototype");
    }
    for (const Path &target : targets) {
      std::string err;
      if (!IsValidScenePathString(target.str(), &err)) {
        AddError(result, "geom.pointInstancer.prototypes",
                 MakePropertyLocation(prim_location, "prototypes"),
                 "prototypes target `" + target.str() +
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
    for (const char *prop_name :
         {"orientations", "scales", "ids", "velocities", "accelerations",
          "angularVelocities"}) {
      ValidateArrayLengthProperty(ps, prop_name, position_count, false,
                                  "geom.pointInstancer.arraySize",
                                  prim_location, "positions", result);
    }
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

  const Value *cr = GetAttrValue(ps, "clippingRange");
  std::array<float, 2> clipping_range = {{0.0f, 0.0f}};
  if (cr && ValueToFloat2(*cr, &clipping_range)) {
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
  const bool have_points = GetPoint3ArrayLen(ps, "points", &point_count);
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
  for (const PropSlot &slot : ps.properties().slots()) {
    if (slot.is_relationship()) {
      continue;
    }
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (!IsPrimvarPropertyName(prop_name)) {
      continue;
    }
    const std::string prop_location =
        MakePropertyLocation(prim_location, prop_name);

    const PropMeta *meta = ps.property_meta(prop_name);
    if (meta && (meta->authored & PropMeta::kInterpolation) &&
        !IsValidPrimvarInterpolation(meta->interpolation)) {
      AddError(result, "geom.primvar.metadata", prop_location,
               "invalid primvar interpolation `" + meta->interpolation + "`");
    }
    if (meta && (meta->authored & PropMeta::kElementSize) &&
        meta->elementSize <= 0) {
      AddError(result, "geom.primvar.metadata", prop_location,
               "primvar elementSize must be greater than 0");
    }

    const std::string indices_name = prop_name + ":indices";
    if (!HasProperty(ps, indices_name)) {
      continue;
    }
    const std::string indices_location =
        MakePropertyLocation(prim_location, indices_name);
    if (IsRelationshipProp(ps, indices_name)) {
      AddError(result, "geom.primvar.indices", indices_location,
               "indexed primvar indices must be an int[] attribute");
      continue;
    }

    std::vector<int32_t> indices;
    if (!GetIntArrayProperty(ps, indices_name, &indices)) {
      AddError(result, "geom.primvar.indices", indices_location,
               "indexed primvar indices must have int[] values");
      continue;
    }

    size_t value_count = 0;
    const bool have_value_count =
        GetArrayLengthProperty(ps, prop_name, &value_count);
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] < 0 ||
          (have_value_count &&
           static_cast<size_t>(indices[i]) >= value_count)) {
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

// ---------------------------------------------------------------------------
// skel rules
// ---------------------------------------------------------------------------

void ValidateSkeletonTopology(const PrimSpec &ps,
                              const std::string &prim_location,
                              USDValidationResult *result,
                              size_t *out_joint_count) {
  std::vector<std::string> joints;
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
    const std::string &joint = joints[i];
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

  if (HasAttributeProp(ps, "jointNames")) {
    size_t count = 0;
    if (GetArrayLengthProperty(ps, "jointNames", &count) &&
        count != joints.size()) {
      AddError(result, "geom.skel.skeleton.jointNames",
               MakePropertyLocation(prim_location, "jointNames"),
               "jointNames has " + std::to_string(count) +
                   " elements, but joints has " +
                   std::to_string(joints.size()) + " elements");
    }
  }

  for (const char *prop_name : {"bindTransforms", "restTransforms"}) {
    if (!HasAttributeProp(ps, prop_name)) {
      continue;
    }
    size_t count = 0;
    if (!GetArrayLengthProperty(ps, prop_name, &count)) {
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
    ValidateTimeSamplesProp(ps, prop_name, "geom.skel.skeleton.transforms",
                            MakePropertyLocation(prim_location, prop_name),
                            result);
  }
}

void ValidateSkelAnimation(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  std::vector<std::string> joints;
  const bool have_joints = GetTokenArrayProperty(ps, "joints", &joints);
  if (!have_joints) {
    AddWarning(result, "geom.skel.animation.joints",
               MakePropertyLocation(prim_location, "joints"),
               "SkelAnimation should author uniform token[] joints");
  }

  if (have_joints) {
    for (const char *prop_name : {"translations", "rotations", "scales"}) {
      if (!HasAttributeProp(ps, prop_name)) {
        continue;
      }
      ValidateArrayLengthAtAllSamples(
          ps, prop_name, "geom.skel.animation.transforms",
          MakePropertyLocation(prim_location, prop_name), joints.size(),
          "joints", result);
    }
  }

  std::vector<std::string> blend_shapes;
  if (GetTokenArrayProperty(ps, "blendShapes", &blend_shapes)) {
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < blend_shapes.size(); i++) {
      const std::string &name = blend_shapes[i];
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

    if (HasAttributeProp(ps, "blendShapeWeights")) {
      ValidateArrayLengthAtAllSamples(
          ps, "blendShapeWeights", "geom.skel.animation.blendShapes",
          MakePropertyLocation(prim_location, "blendShapeWeights"),
          blend_shapes.size(), "blendShapes", result);
    }
  } else if (HasAttributeProp(ps, "blendShapeWeights")) {
    AddWarning(result, "geom.skel.animation.blendShapes",
               MakePropertyLocation(prim_location, "blendShapeWeights"),
               "blendShapeWeights is authored without uniform token[] "
               "blendShapes");
  }
}

void ValidateBlendShape(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  size_t offset_count = 0;
  bool have_offsets = false;
  if (!HasAttributeProp(ps, "offsets")) {
    AddError(result, "geom.skel.blendShape.offsets",
             MakePropertyLocation(prim_location, "offsets"),
             "BlendShape must author uniform vector3f[] offsets");
  } else if (!GetArrayLengthProperty(ps, "offsets", &offset_count)) {
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

  if (HasAttributeProp(ps, "normalOffsets")) {
    ValidateTimeSamplesProp(
        ps, "normalOffsets", "geom.skel.blendShape.normalOffsets",
        MakePropertyLocation(prim_location, "normalOffsets"), result);
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          ps, "normalOffsets", "geom.skel.blendShape.normalOffsets",
          MakePropertyLocation(prim_location, "normalOffsets"), offset_count,
          "offsets", result);
    }
  }

  if (HasAttributeProp(ps, "pointIndices")) {
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          ps, "pointIndices", "geom.skel.blendShape.pointIndices",
          MakePropertyLocation(prim_location, "pointIndices"), offset_count,
          "offsets", result);
    }

    std::vector<int32_t> indices;
    if (GetIntArrayProperty(ps, "pointIndices", &indices)) {
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

  for (const PropSlot &slot : ps.properties().slots()) {
    if (slot.is_relationship()) {
      continue;
    }
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (!StartsWith(prop_name, "inbetweens:")) {
      continue;
    }
    const std::string location =
        MakePropertyLocation(prim_location, prop_name);
    const PropMeta *meta = ps.property_meta(prop_name);
    if (!meta || !(meta->authored & PropMeta::kWeight)) {
      AddError(result, "geom.skel.blendShape.inbetween", location,
               "BlendShape inbetween `" + prop_name +
                   "` must author weight metadata");
    } else if (!std::isfinite(meta->weight)) {
      AddError(result, "geom.skel.blendShape.inbetween", location,
               "BlendShape inbetween `" + prop_name +
                   "` has a non-finite weight");
    }
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(ps, prop_name,
                                      "geom.skel.blendShape.inbetween",
                                      location, offset_count, "offsets",
                                      result);
    }
  }
}

std::vector<std::string> AllPropertyNames(const PrimSpec &ps);

void ValidateSkelBinding(const PrimSpec &ps,
                         const std::vector<AppliedSchema> &applied_schemas,
                         const std::string &prim_location,
                         const PrimTypeByPath &prim_types,
                         const AncestorContext &ancestors,
                         USDValidationResult *result) {
  bool has_binding_property = false;
  // AllPropertyNames, not slots: a relationship WITH targets (`rel
  // skel:skeleton = </Skel>`) lives only in the relationships map, so the
  // slot-only loop never saw the most common UsdSkelBinding spelling and the
  // missing-SkelBindingAPI check was silent on exactly the prims it existed
  // for (OpenUSD's SkelBindingApiAppliedValidator flags these).
  for (const std::string &name : AllPropertyNames(ps)) {
    if (StartsWith(name, "skel:") || StartsWith(name, "primvars:skel:")) {
      has_binding_property = true;
      break;
    }
  }
  const bool has_binding_api =
      HasAppliedSchema(applied_schemas, "SkelBindingAPI");
  if (has_binding_property && !has_binding_api) {
    AddError(result, "geom.skel.binding.api", prim_location,
             "SkelBinding property is authored without applying "
             "SkelBindingAPI");
  }
  if (has_binding_api && ps.type_name() != "SkelRoot" &&
      !ancestors.has_skel_root_ancestor) {
    AddError(result, "geom.skel.binding.root", prim_location,
             "SkelBindingAPI must be applied on a SkelRoot or beneath one");
  }

  if (HasProperty(ps, "skel:skeleton")) {
    ValidateRelationshipTargetPaths(
        ps, "skel:skeleton", "geom.skel.binding.skeleton",
        MakePropertyLocation(prim_location, "skel:skeleton"), result);
    if (IsRelationshipProp(ps, "skel:skeleton")) {
      const std::vector<Path> targets = RelTargets(ps, "skel:skeleton");
      for (const Path &target : targets) {
        std::string prim_part;
        SplitScenePathString(target.str(), &prim_part, nullptr);
        const std::string *target_type = FindPrimType(prim_types, prim_part);
        if (target_type && *target_type != "Skeleton") {
          AddWarning(result, "geom.skel.binding.skeleton",
                     MakePropertyLocation(prim_location, "skel:skeleton"),
                     "skel:skeleton target `" + target.str() +
                         "` has type `" + *target_type +
                         "`, expected Skeleton");
        }
      }
    }
  }

  if (HasProperty(ps, "skel:animationSource")) {
    ValidateRelationshipTargetPaths(
        ps, "skel:animationSource", "geom.skel.binding.animationSource",
        MakePropertyLocation(prim_location, "skel:animationSource"), result);
    if (IsRelationshipProp(ps, "skel:animationSource")) {
      const std::vector<Path> targets =
          RelTargets(ps, "skel:animationSource");
      for (const Path &target : targets) {
        std::string prim_part;
        SplitScenePathString(target.str(), &prim_part, nullptr);
        const std::string *target_type = FindPrimType(prim_types, prim_part);
        if (target_type && *target_type != "SkelAnimation") {
          AddWarning(
              result, "geom.skel.binding.animationSource",
              MakePropertyLocation(prim_location, "skel:animationSource"),
              "skel:animationSource target `" + target.str() +
                  "` has type `" + *target_type + "`, expected SkelAnimation");
        }
      }
    }
  }
}

void ValidateSkinningPrimvars(const PrimSpec &ps,
                              const std::string &prim_location,
                              const PrimTypeByPath &prim_types,
                              const SkeletonJointCountByPath &skeleton_joints,
                              USDValidationResult *result) {
  const bool have_indices_attr =
      HasAttributeProp(ps, "primvars:skel:jointIndices");
  const bool have_weights_attr =
      HasAttributeProp(ps, "primvars:skel:jointWeights");
  if (!have_indices_attr && !have_weights_attr) {
    return;
  }

  if (!have_indices_attr || !have_weights_attr) {
    AddError(result, "geom.skel.skinning.pair", prim_location,
             "primvars:skel:jointIndices and primvars:skel:jointWeights must "
             "be authored together");
    return;
  }

  const PropMeta *indices_meta = ps.property_meta("primvars:skel:jointIndices");
  const PropMeta *weights_meta = ps.property_meta("primvars:skel:jointWeights");

  const auto validate_skin_interp = [&](const PropMeta *meta,
                                        const std::string &name) {
    const std::string location = MakePropertyLocation(prim_location, name);
    if (!meta || !(meta->authored & PropMeta::kInterpolation)) {
      AddError(result, "geom.skel.skinning.interpolation", location,
               name + " must author interpolation metadata");
      return;
    }
    if (meta->interpolation != "vertex" && meta->interpolation != "varying") {
      AddError(result, "geom.skel.skinning.interpolation", location,
               name + " interpolation must be vertex or varying, but is `" +
                   meta->interpolation + "`");
    }
  };
  validate_skin_interp(indices_meta, "primvars:skel:jointIndices");
  validate_skin_interp(weights_meta, "primvars:skel:jointWeights");

  const bool indices_es_ok = indices_meta &&
                             (indices_meta->authored & PropMeta::kElementSize) &&
                             indices_meta->elementSize > 0;
  const bool weights_es_ok = weights_meta &&
                             (weights_meta->authored & PropMeta::kElementSize) &&
                             weights_meta->elementSize > 0;
  if (!indices_es_ok || !weights_es_ok) {
    AddError(result, "geom.skel.skinning.elementSize", prim_location,
             "skinning primvars must author non-zero elementSize metadata");
  } else if (indices_meta->elementSize != weights_meta->elementSize) {
    AddError(result, "geom.skel.skinning.elementSize", prim_location,
             "primvars:skel:jointIndices and primvars:skel:jointWeights must "
             "use the same elementSize");
  }

  std::vector<int32_t> joint_indices;
  std::vector<float> joint_weights;
  if (GetIntArrayProperty(ps, "primvars:skel:jointIndices", &joint_indices) &&
      GetFloatArrayProperty(ps, "primvars:skel:jointWeights",
                            &joint_weights)) {
    if (joint_indices.size() != joint_weights.size()) {
      AddError(result, "geom.skel.skinning.size", prim_location,
               "jointIndices has " + std::to_string(joint_indices.size()) +
                   " elements, but jointWeights has " +
                   std::to_string(joint_weights.size()) + " elements");
    }

    size_t local_joint_count = 0;
    bool have_local_joint_count = false;
    if (IsRelationshipProp(ps, "skel:skeleton")) {
      const std::vector<Path> targets = RelTargets(ps, "skel:skeleton");
      if (targets.size() == 1) {
        std::string prim_part;
        SplitScenePathString(targets[0].str(), &prim_part, nullptr);
        const std::string *target_type = FindPrimType(prim_types, prim_part);
        if (target_type && *target_type == "Skeleton") {
          auto joint_count_it = skeleton_joints.find(prim_part);
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

    if (weights_meta && (weights_meta->authored & PropMeta::kElementSize)) {
      const size_t element_size =
          weights_meta->elementSize > 0
              ? static_cast<size_t>(weights_meta->elementSize)
              : 0;
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

// ---------------------------------------------------------------------------
// shade rules
// ---------------------------------------------------------------------------

std::string GetShaderInfoId(const PrimSpec &ps) {
  std::string id;
  (void)GetTokenProperty(ps, "info:id", &id);
  return id;
}

void ValidateUsdPreviewSurface(const PrimSpec &ps,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  static const std::string kInputsPrefix = "inputs:";
  const auto &inputs = UsdPreviewSurfaceInputTypes();

  for (const PropSlot &slot : ps.properties().slots()) {
    if (slot.is_relationship()) {
      continue;
    }
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (!StartsWith(prop_name, kInputsPrefix)) {
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

    const std::string authored = AttrTypeNameOf(ps, prop_name);
    if (!authored.empty() && !ValueTypesAgree(authored, it->second)) {
      AddWarning(result, "shade.preview.inputType", prop_location,
                 "UsdPreviewSurface input `" + input_name + "` should be `" +
                     it->second + "` but is `" + authored + "`");
    } else if (!authored.empty() && authored != it->second) {
      // Same storage, different declared spelling (float3 vs color3f).
      // OpenUSD's ShaderSdrCompliance flags this as MismatchedPropertyType:
      // the sdr registry type is exact, not role-tolerant.
      AddWarning(result, "shade.shader.typeMismatch", prop_location,
                 "UsdPreviewSurface input `" + input_name +
                     "` is declared `" + authored +
                     "` but the shader registry type is `" + it->second + "`");
    }
  }

  for (const char *prop_name :
       {"inputs:roughness", "inputs:metallic", "inputs:clearcoat",
        "inputs:clearcoatRoughness", "inputs:opacity", "inputs:occlusion"}) {
    double value = 0.0;
    if (GetNumericScalarProperty(ps, prop_name, &value) &&
        (value < 0.0 || value > 1.0)) {
      AddWarning(result, "shade.preview.inputRange",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) +
                     " should be in range [0, 1], but is " +
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

bool IsUsdUVTextureWrapToken(const std::string &token) {
  return token == "useMetadata" || token == "black" || token == "clamp" ||
         token == "repeat" || token == "mirror";
}

bool IsUsdUVTextureColorSpaceToken(const std::string &token) {
  return token == "auto" || token == "raw" || token == "sRGB";
}

void ValidateUsdUVTexture(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result) {
  std::string file;
  if (!HasAttributeProp(ps, "inputs:file")) {
    AddWarning(result, "shade.uvTexture.file",
               MakePropertyLocation(prim_location, "inputs:file"),
               "UsdUVTexture should author inputs:file");
  } else if (!HasConnections(ps, "inputs:file") &&
             (!GetAssetPathProperty(ps, "inputs:file", &file) ||
              file.empty())) {
    AddWarning(result, "shade.uvTexture.file",
               MakePropertyLocation(prim_location, "inputs:file"),
               "UsdUVTexture inputs:file should be a non-empty asset path");
  }

  for (const char *prop_name : {"inputs:wrapS", "inputs:wrapT"}) {
    std::string token;
    if (GetTokenProperty(ps, prop_name, &token) &&
        !IsUsdUVTextureWrapToken(token)) {
      AddWarning(result, "shade.uvTexture.wrap",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) + " has invalid wrap token `" + token +
                     "`");
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

void ValidateUsdPrimvarReader(const PrimSpec &ps, const std::string &shader_id,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  if (!HasAttributeProp(ps, "inputs:varname")) {
    AddWarning(result, "shade.primvarReader.varname",
               MakePropertyLocation(prim_location, "inputs:varname"),
               "UsdPrimvarReader should author inputs:varname");
    return;
  }

  const std::string varname_type = AttrTypeNameOf(ps, "inputs:varname");
  if (varname_type == "token") {
    // Accepted for evaluation (the pre-21.02 spec used token), but the sdr
    // registry type is `string` -- OpenUSD's ShaderSdrCompliance flags the
    // token spelling as MismatchedPropertyType, connected or not.
    AddWarning(result, "shade.shader.typeMismatch",
               MakePropertyLocation(prim_location, "inputs:varname"),
               "inputs:varname is declared `token` but the shader registry "
               "type is `string`");
  }
  if (HasConnections(ps, "inputs:varname")) {
    // A connected varname's VALUE resolves at composition/evaluation time.
  } else if (varname_type != "token" && varname_type != "string") {
    AddWarning(result, "shade.primvarReader.varname",
               MakePropertyLocation(prim_location, "inputs:varname"),
               "UsdPrimvarReader inputs:varname should be token or string, "
               "but is `" +
                   varname_type + "`");
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

  const std::string location =
      MakePropertyLocation(prim_location, "outputs:result");
  if (!HasProperty(ps, "outputs:result")) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " should author outputs:result");
    return;
  }
  if (IsRelationshipProp(ps, "outputs:result")) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " outputs:result should be an attribute");
    return;
  }

  const std::string result_type = AttrTypeNameOf(ps, "outputs:result");
  if (!ValueTypesAgree(result_type, *expected_type)) {
    AddWarning(result, "shade.primvarReader.result", location,
               shader_id + " outputs:result should be " + *expected_type +
                   ", but is `" + result_type + "`");
  }
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

void ValidateMaterialXConfig(const PrimSpec &ps,
                             const std::vector<AppliedSchema> &applied_schemas,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  bool has_config_property = false;
  for (const PropSlot &slot : ps.properties().slots()) {
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (StartsWith(prop_name, "config:mtlx:")) {
      has_config_property = true;
      break;
    }
  }

  const bool has_config_api =
      HasAppliedSchema(applied_schemas, "MaterialXConfigAPI");
  const bool has_mtlx_terminal =
      HasProperty(ps, "outputs:mtlx:surface") ||
      HasProperty(ps, "outputs:mtlx:displacement") ||
      HasProperty(ps, "outputs:mtlx:volume");
  if ((has_config_property || has_mtlx_terminal) && !has_config_api) {
    AddWarning(result, "shade.materialX.configAPI", prim_location,
               "MaterialX properties are authored without applying "
               "MaterialXConfigAPI");
  }

  if (has_config_api && ps.type_name() != "Material") {
    AddWarning(result, "shade.materialX.configAPI", prim_location,
               "MaterialXConfigAPI is expected on Material prims");
  }

  std::string version;
  if (HasProperty(ps, "config:mtlx:version")) {
    const std::string location =
        MakePropertyLocation(prim_location, "config:mtlx:version");
    if (IsRelationshipProp(ps, "config:mtlx:version") ||
        AttrTypeNameOf(ps, "config:mtlx:version") != "string" ||
        !GetStringProperty(ps, "config:mtlx:version", &version)) {
      AddError(result, "shade.materialX.version", location,
               "config:mtlx:version must be a string attribute");
    } else if (version.empty() || !StartsWith(version, "1.")) {
      AddWarning(result, "shade.materialX.version", location,
                 "MaterialX version should be a 1.x version string, but is `" +
                     version + "`");
    }
  }

  std::string source_uri;
  if ((GetStringProperty(ps, "config:mtlx:sourceUri", &source_uri) ||
       GetAssetPathProperty(ps, "config:mtlx:sourceUri", &source_uri)) &&
      !source_uri.empty() && !IsMaterialXAssetPath(source_uri)) {
    AddWarning(result, "shade.materialX.sourceUri",
               MakePropertyLocation(prim_location, "config:mtlx:sourceUri"),
               "MaterialX sourceUri should usually reference a .mtlx asset");
  }
}

void ValidateMaterialXShader(const PrimSpec &ps, const std::string &shader_id,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  if (!IsMaterialXShaderId(shader_id)) {
    return;
  }

  bool has_output = false;
  for (const PropSlot &slot : ps.properties().slots()) {
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (StartsWith(prop_name, "outputs:")) {
      has_output = true;
      break;
    }
  }
  if (!has_output) {
    AddWarning(result, "shade.materialX.output", prim_location,
               "MaterialX shader `" + shader_id +
                   "` should author at least one outputs:* port");
  }
}

void ValidateMaterialXSynthesizedMaterial(
    const PrimSpec &ps, const std::string &prim_location,
    const PrimTypeByPath &prim_types, USDValidationResult *result) {
  if (ps.type_name() != "Material" ||
      !StartsWith(prim_location, "/MaterialX/Materials/")) {
    return;
  }

  bool has_shader = false;
  for (const char *rel_name : {"mtlx:surface:source",
                               "mtlx:displacement:source",
                               "mtlx:volume:source"}) {
    const std::string relationship_name(rel_name);
    if (!HasProperty(ps, relationship_name)) {
      continue;
    }
    has_shader = true;
    const std::string location =
        MakePropertyLocation(prim_location, relationship_name);
    if (!IsRelationshipProp(ps, relationship_name)) {
      AddError(result, "shade.materialX.nodeReference", location,
               relationship_name + " must be a relationship");
      continue;
    }
    ValidateRelationshipTargetPaths(ps, relationship_name,
                                    "shade.materialX.nodeReference", location,
                                    result);
    for (const Path &target : RelTargets(ps, relationship_name)) {
      std::string target_prim;
      SplitScenePathString(target.str(), &target_prim, nullptr);
      const std::string *target_type = FindPrimType(prim_types, target_prim);
      if (!target_type) {
        AddError(result, "shade.materialX.nodeReference", location,
                 "MaterialX shader target `" + target_prim +
                     "` does not exist");
      } else if (*target_type != "Shader" && *target_type != "NodeGraph") {
        AddError(result, "shade.materialX.nodeReference", location,
                 "MaterialX shader target `" + target_prim + "` has type `" +
                     *target_type + "`");
      }
    }
  }
  if (!has_shader) {
    AddError(result, "shade.materialX.nodeReference", prim_location,
             "MaterialX material has no surface, displacement, or volume "
             "shader reference");
  }
}

// ---------------------------------------------------------------------------
// lux rules
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Dictionary metadata helpers (next::Dict)
// ---------------------------------------------------------------------------

bool GetMetaDouble(const Dict &dict, const std::string &key, double *out) {
  const Value *v = dict.find(key);
  if (!v || !out) {
    return false;
  }
  return ValueToDouble(*v, out);
}

bool GetMetaAssetPath(const Dict &dict, const std::string &key,
                      std::string *out) {
  const Value *v = dict.find(key);
  if (!v || !out) {
    return false;
  }
  if (const std::string *asset = v->as_asset_path()) {
    *out = *asset;
    return true;
  }
  if (const std::string *str = v->as_string()) {
    *out = *str;
    return true;
  }
  return false;
}

bool GetMetaString(const Dict &dict, const std::string &key,
                   std::string *out) {
  const Value *v = dict.find(key);
  if (!v || !out) {
    return false;
  }
  if (const std::string *str = v->as_string()) {
    *out = *str;
    return true;
  }
  if (const std::string *tok = v->as_token()) {
    *out = *tok;
    return true;
  }
  return false;
}

bool GetMetaStringArray(const Dict &dict, const std::string &key,
                        std::vector<std::string> *out) {
  const Value *v = dict.find(key);
  if (!v || !out || !v->is_array()) {
    return false;
  }
  const std::vector<std::string> *arr = v->as_token_array();
  if (!arr) {
    return false;
  }
  *out = *arr;
  return true;
}

bool GetMetaDouble2Array(const Dict &dict, const std::string &key,
                         std::vector<std::array<double, 2>> *out) {
  const Value *v = dict.find(key);
  if (!v || !out || !v->is_array() || v->type_id() != TypeId::Double2) {
    return false;
  }
  const std::vector<double> *flat = v->as_double_array();
  if (!flat) {
    return false;
  }
  out->clear();
  for (size_t i = 0; i + 1 < flat->size(); i += 2) {
    out->push_back({{(*flat)[i], (*flat)[i + 1]}});
  }
  return true;
}

void ValidateDictionaryKeys(const Dict &dict, const std::string &rule_id,
                            const std::string &location,
                            USDValidationResult *result) {
  for (const auto &entry : dict.entries()) {
    const std::string key_location = location + "." + entry.first;
    if (!IsValidNamespacedIdentifier(entry.first)) {
      AddError(result, rule_id, location,
               "dictionary key `" + entry.first +
                   "` is not a valid namespaced identifier");
    }
    if (const Dict *child = entry.second.as_dictionary()) {
      ValidateDictionaryKeys(*child, rule_id, key_location, result);
    }
  }
}

void ValidateDictValue(const Value &v, const std::string &rule_id,
                       const std::string &location,
                       USDValidationResult *result) {
  if (const Dict *dict = v.as_dictionary()) {
    ValidateDictionaryKeys(*dict, rule_id, location, result);
  }
}

// ---------------------------------------------------------------------------
// Composition metadata (arcs, variants, relocates)
// ---------------------------------------------------------------------------

// True if `ps` authors any composition arc (used to gate locality-dependent
// rules; a composed prim may pull opinions from another layer).
bool HasCompositionArc(const PrimSpec &ps) {
  const PrimSpecMeta &m = ps.meta();
  if (!m.references.empty() || !m.payloads.empty() || !m.inherits.empty() ||
      !m.specializes.empty()) {
    return true;
  }
  if (!m.variantSelection.empty() || !m.variantSelections().empty() ||
      !m.variantSets().empty()) {
    return true;
  }
  const ArcListOpEdits *edits = m.arc_edits();
  if (edits && (edits->references.has_authored_opinion() ||
                edits->payloads.has_authored_opinion() ||
                edits->inherits.has_authored_opinion() ||
                edits->specializes.has_authored_opinion())) {
    return true;
  }
  return false;
}

bool IsOverride(const PrimSpec &ps) {
  return ps.specifier() == PrimSpecifier::Over;
}

// Gather every authored arc string of one arc field (inline explicit list +
// any list-op edit buckets).
std::vector<std::string> GatherArcStrings(const std::vector<std::string> &inline_items,
                                          const ArcEdit *edit) {
  std::vector<std::string> items = inline_items;
  if (edit) {
    items.insert(items.end(), edit->prepended.begin(), edit->prepended.end());
    items.insert(items.end(), edit->appended.begin(), edit->appended.end());
    items.insert(items.end(), edit->deleted.begin(), edit->deleted.end());
    items.insert(items.end(), edit->ordered.begin(), edit->ordered.end());
  }
  return items;
}

bool ParseFiniteLayerOffsetNumber(const char *begin, const char *end,
                                  double *out) {
  if (!out || begin == end) return false;
  auto parsed = fast_float::from_chars(begin, end, *out);
  if (!(parsed.ec == std::errc{} && parsed.ptr == end) && begin < end &&
      *begin == '+') {
    parsed = fast_float::from_chars(begin + 1, end, *out);
  }
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool ParseRawLayerOffset(const std::string &text, double *offset,
                         double *scale) {
  if (!offset || !scale) return false;
  *offset = 0.0;
  *scale = 1.0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return ParseFiniteLayerOffsetNumber(begin, end, offset);
  }
  return ParseFiniteLayerOffsetNumber(begin, begin + colon, offset) &&
         ParseFiniteLayerOffsetNumber(begin + colon + 1, end, scale);
}

void ValidateLayerOffset(double offset, double scale,
                         const std::string &rule_id,
                         const std::string &location,
                         USDValidationResult *result) {
  if (!std::isfinite(offset)) {
    AddError(result, rule_id, location,
             "composition layerOffset offset must be finite");
  }
  if (!std::isfinite(scale) || !(scale > 0.0)) {
    AddError(result, rule_id, location,
             "composition layerOffset scale must be finite and greater than 0");
  }
}

void ValidateRefArcString(const std::string &arc_str, bool is_payload,
                          const std::string &rule_id,
                          const std::string &prim_location,
                          USDValidationResult *result) {
  CompositionArc arc = is_payload ? Compositor::ParsePayload(arc_str)
                                  : Compositor::ParseReference(arc_str);
  // Programmatically-built layers may store a bare prim path (the USDA parser
  // stores the canonical "</path>" form). Accept both, like the compositor.
  if (arc.prim_path.empty() && arc.asset_path.empty() && !arc_str.empty() &&
      arc_str[0] == '/') {
    arc.prim_path = arc_str;
  }
  const char *what = is_payload ? "payload" : "reference";
  if (arc.asset_path.empty() && arc.prim_path.empty()) {
    AddError(result, rule_id, prim_location,
             std::string(what) + " must author an asset path or prim path");
  }
  if (!arc.prim_path.empty()) {
    std::string err;
    if (!IsValidAbsolutePrimTargetPathString(arc.prim_path, &err)) {
      AddError(result, rule_id, prim_location,
               std::string(what) + " primPath `" + arc.prim_path +
                   "` is invalid: " + err);
    }
  }
  if (!arc.layer_offset.empty()) {
    double offset = 0.0;
    double scale = 1.0;
    if (!ParseRawLayerOffset(arc.layer_offset, &offset, &scale)) {
      AddError(result, rule_id, prim_location,
               "composition layerOffset must contain numeric offset/scale values");
    } else {
      ValidateLayerOffset(offset, scale, rule_id, prim_location, result);
    }
  }
}

void ValidatePathArcString(const std::string &arc_str,
                           const std::string &rule_id,
                           const std::string &prim_location,
                           const PrimSpecifierByPath &prim_specifiers,
                           USDValidationResult *result,
                           bool expect_class_target) {
  CompositionArc arc = Compositor::ParseReference(arc_str);
  if (arc.prim_path.empty() && arc.asset_path.empty() && !arc_str.empty() &&
      arc_str[0] == '/') {
    arc.prim_path = arc_str;
  }
  const std::string &target = arc.prim_path;
  std::string err;
  if (!IsValidAbsolutePrimTargetPathString(target, &err)) {
    AddError(result, rule_id, prim_location,
             "target `" + (target.empty() ? arc_str : target) +
                 "` is invalid: " + err);
    return;
  }
  if (target == prim_location) {
    AddError(result, rule_id, prim_location,
             "composition arc must not target the prim itself");
  }
  const PrimSpecifier *target_spec = FindPrimSpecifier(prim_specifiers, target);
  if (expect_class_target && target_spec &&
      *target_spec != PrimSpecifier::Class) {
    AddWarning(result, rule_id, prim_location,
               "local target `" + target + "` is not a class prim");
  }
}

void ValidateVariantSetsAndSelections(const PrimSpec &ps,
                                      const std::string &prim_location,
                                      USDValidationResult *result) {
  // NOTE (structural approximation): variant OPTION bodies (properties/child
  // prims inside a variant) are stored as VariantData / graft layers in
  // next-core rather than PrimSpecs, and are not deep-validated here. Only
  // set / variant / selection names are checked (legacy additionally walks
  // the variant PrimSpec bodies).
  std::set<std::string> local_sets;
  std::map<std::string, std::set<std::string>> local_variants;
  for (const VariantSetData &vs : ps.meta().variantSets()) {
    local_sets.insert(vs.name);
    if (vs.name.empty() || !IsValidIdentifier(vs.name)) {
      AddError(result, "core.composition.variantSet", prim_location,
               "variant set statement name `" + vs.name +
                   "` is not a valid identifier");
    }
    if (vs.variants.empty()) {
      AddWarning(result, "core.composition.variantSet", prim_location,
                 "variant set `" + vs.name + "` has no variants");
    }
    for (const VariantData &variant : vs.variants) {
      local_variants[vs.name].insert(variant.name);
      if (variant.name.empty() || !IsValidIdentifier(variant.name)) {
        AddError(result, "core.composition.variantSet", prim_location,
                 "variant name `" + variant.name + "` in set `" + vs.name +
                     "` is not a valid identifier");
      }
    }
  }

  // Collect selections: the plural list plus the legacy inline single
  // selection (the USDA parser mirrors the first selection into both fields,
  // so deduplicate).
  std::vector<std::pair<std::string, std::string>> selections =
      ps.meta().variantSelections();
  if (!ps.meta().variantSelection.empty()) {
    const VariantSelection sel =
        Compositor::ParseVariantSelection(ps.meta().variantSelection);
    if (!sel.variant_set.empty() || !sel.variant_name.empty()) {
      const std::pair<std::string, std::string> entry(sel.variant_set,
                                                      sel.variant_name);
      if (std::find(selections.begin(), selections.end(), entry) ==
          selections.end()) {
        selections.push_back(entry);
      }
    }
  }

  for (const auto &selection : selections) {
    const std::string &set_name = selection.first;
    const std::string &variant_name = selection.second;
    if (set_name.empty() || !IsValidIdentifier(set_name)) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection set name `" + set_name +
                   "` is not a valid identifier");
    }
    if (variant_name.empty() || !IsValidIdentifier(variant_name)) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection `" + variant_name + "` for set `" +
                   set_name + "` is not a valid identifier");
      continue;
    }
    if (local_sets.count(set_name) &&
        local_variants[set_name].count(variant_name) == 0) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection `" + variant_name + "` is not authored in "
                   "local variant set `" +
                   set_name + "`");
    }
  }
}

// next-core stores relocates per-prim (PrimSpecMeta::relocates); validated
// under the legacy core.layer.layerRelocates rule ids at the prim location.
void ValidateRelocates(const PrimSpec &ps, const std::string &prim_location,
                       USDValidationResult *result) {
  std::set<std::string> relocated_sources;
  for (const auto &entry : ps.meta().relocates()) {
    const std::string &source = entry.first;
    const std::string &target = entry.second;

    std::string source_err;
    if (!IsValidAbsolutePrimTargetPathString(source, &source_err)) {
      AddError(result, "core.layer.layerRelocates.source", prim_location,
               "layerRelocates source `" + source +
                   "` must be a valid absolute prim path");
    }
    std::string target_err;
    if (!target.empty() &&
        !IsValidAbsolutePrimPathString(target, &target_err)) {
      AddError(result, "core.layer.layerRelocates.target", prim_location,
               "layerRelocates target `" + target +
                   "` must be empty or a valid absolute prim path");
    }
    if (!source.empty() && !target.empty() && source == target) {
      AddError(result, "core.layer.layerRelocates.identity", prim_location,
               "layerRelocates source and target must not be identical");
    }
    if (!source.empty() && !relocated_sources.insert(source).second) {
      AddError(result, "core.layer.layerRelocates.source", prim_location,
               "layerRelocates contains duplicate source `" + source + "`");
    }
  }
}

void ValidateCompositionMetadata(const PrimSpec &ps,
                                 const std::string &prim_location,
                                 const PrimSpecifierByPath &prim_specifiers,
                                 USDValidationResult *result) {
  const PrimSpecMeta &m = ps.meta();
  const ArcListOpEdits *edits = m.arc_edits();

  // NOTE: invalid list-edit qualifiers are unrepresentable in next-core
  // (ArcEdit qualifiers are typed), so the legacy "invalid list-edit
  // qualifier" errors have no equivalent here.
  for (const std::string &ref : GatherArcStrings(
           m.references, edits ? &edits->references : nullptr)) {
    ValidateRefArcString(ref, /* is_payload */ false,
                         "core.composition.reference", prim_location, result);
  }
  for (const std::string &pl :
       GatherArcStrings(m.payloads, edits ? &edits->payloads : nullptr)) {
    ValidateRefArcString(pl, /* is_payload */ true,
                         "core.composition.payload", prim_location, result);
  }
  for (const std::string &inh :
       GatherArcStrings(m.inherits, edits ? &edits->inherits : nullptr)) {
    ValidatePathArcString(inh, "core.composition.inherits", prim_location,
                          prim_specifiers, result,
                          /* expect_class_target */ true);
  }
  for (const std::string &sp : GatherArcStrings(
           m.specializes, edits ? &edits->specializes : nullptr)) {
    ValidatePathArcString(sp, "core.composition.specializes", prim_location,
                          prim_specifiers, result,
                          /* expect_class_target */ true);
  }

  // Programmatic and USDC inputs can bypass USDA's strict parser, so validate
  // both components here as well as on the authored arc strings above.
  ValidateLayerOffset(m.layer_offset.first, m.layer_offset.second,
                      "core.composition.reference", prim_location, result);

  ValidateVariantSetsAndSelections(ps, prim_location, result);
  ValidateRelocates(ps, prim_location, result);

  if (ps.specifier() == PrimSpecifier::Over && ps.type_name().empty() &&
      !HasCompositionArc(ps) && ps.properties().size() == 0 &&
      ps.relationship_names().empty() && ps.child_indices().empty()) {
    AddWarning(result, "core.composition.over", prim_location,
               "empty typeless over has no local opinions or composition "
               "arcs");
  }
}

// ---------------------------------------------------------------------------
// apiSchemas / prim metadata
// ---------------------------------------------------------------------------

std::vector<AppliedSchema> CollectAppliedSchemas(const PrimSpec &ps) {
  std::vector<AppliedSchema> schemas;
  for (const std::string &entry : ps.meta().apiSchemas()) {
    std::string schema_name = entry;
    std::string instance_name;
    const size_t colon_pos = entry.find(':');
    if (colon_pos != std::string::npos) {
      schema_name = entry.substr(0, colon_pos);
      instance_name = entry.substr(colon_pos + 1);
    }
    schemas.push_back({schema_name, instance_name});
  }
  return schemas;
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

void ValidateAPISchemasMetadata(const std::vector<AppliedSchema> &schemas,
                                const std::string &prim_location,
                                USDValidationResult *result) {
  std::set<std::string> seen;
  for (const AppliedSchema &schema : schemas) {
    if (schema.name.empty() || !IsValidIdentifier(schema.name)) {
      AddError(result, "core.apiSchema.name", prim_location,
               "apiSchemas entry `" + schema.name +
                   "` is not a valid schema identifier");
    }
    if (!schema.instance_name.empty() &&
        !IsValidNamespacedIdentifier(schema.instance_name)) {
      AddError(result, "core.apiSchema.instance", prim_location,
               "apiSchemas instance `" + schema.instance_name +
                   "` is not a valid namespaced identifier");
    }
    if (IsMultipleApplySchemaName(schema.name) &&
        schema.instance_name.empty()) {
      AddError(result, "core.apiSchema.instance", prim_location,
               schema.name + " is a multiple-apply API schema and requires "
                             "an instance name");
    }
    if (IsKnownAPISchemaName(schema.name) &&
        !IsMultipleApplySchemaName(schema.name) &&
        !schema.instance_name.empty()) {
      AddError(result, "core.apiSchema.instance", prim_location,
               schema.name + " is a single-apply API schema and must not "
                             "have an instance name");
    }
    if (!schema.name.empty() && !IsKnownAPISchemaName(schema.name)) {
      AddWarning(result, "core.apiSchema.unknown", prim_location,
                 "API schema `" + schema.name +
                     "` is not present in LightUSD's built-in schema registry");
    }
    const std::string key = schema.name + ":" + schema.instance_name;
    if (!seen.insert(key).second) {
      AddWarning(result, "core.apiSchema.duplicate", prim_location,
                 "duplicate apiSchemas entry `" + key + "`");
    }
  }
}

void ValidatePrimMetadata(const PrimSpec &ps, const std::string &prim_location,
                          const std::vector<AppliedSchema> &applied_schemas,
                          USDValidationResult *result) {
  const PrimSpecMeta &m = ps.meta();

  if (!m.kind().empty() && !IsValidNamespacedIdentifier(m.kind())) {
    AddError(result, "core.prim.kind", prim_location,
             "kind `" + m.kind() + "` is not a valid token");
  }

  const ArcListOpEdits *edits = m.arc_edits();
  const bool has_ref_or_payload =
      !m.references.empty() || !m.payloads.empty() ||
      (edits && (edits->references.has_authored_opinion() ||
                 edits->payloads.has_authored_opinion()));
  if (m.instanceable && !has_ref_or_payload && ps.child_indices().empty()) {
    AddWarning(result, "core.prim.instanceable", prim_location,
               "instanceable prim should usually compose a prototype via "
               "references or payloads, or contain authored children");
  }

  ValidateDictValue(m.customData(), "core.prim.customData",
                    prim_location + ".customData", result);

  if (const Dict *asset_info = m.assetInfo().as_dictionary()) {
    ValidateDictionaryKeys(*asset_info, "core.prim.assetInfo",
                           prim_location + ".assetInfo", result);

    std::string identifier;
    if (GetMetaAssetPath(*asset_info, "identifier", &identifier) &&
        identifier.empty()) {
      AddError(result, "core.prim.assetInfo",
               prim_location + ".assetInfo.identifier",
               "assetInfo identifier must not be empty");
    }
    std::string name;
    if (GetMetaString(*asset_info, "name", &name) && name.empty()) {
      AddWarning(result, "core.prim.assetInfo",
                 prim_location + ".assetInfo.name",
                 "assetInfo name should not be empty");
    }
    std::vector<std::string> deps;
    if (GetMetaStringArray(*asset_info, "payloadAssetDependencies", &deps)) {
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

  ValidateAPISchemasMetadata(applied_schemas, prim_location, result);
}

void ValidateMaterialXReferenceConventions(const PrimSpec &ps,
                                           const std::string &prim_location,
                                           USDValidationResult *result) {
  const ArcListOpEdits *edits = ps.meta().arc_edits();
  for (const std::string &ref : GatherArcStrings(
           ps.meta().references, edits ? &edits->references : nullptr)) {
    const CompositionArc arc = Compositor::ParseReference(ref);
    if (!IsMaterialXAssetPath(arc.asset_path)) {
      continue;
    }
    if (arc.prim_path != "/MaterialX") {
      AddWarning(result, "shade.materialX.referencePrimPath", prim_location,
                 "MaterialX references should target </MaterialX>");
    }
  }
}

// ---------------------------------------------------------------------------
// clips metadata
// ---------------------------------------------------------------------------

void ValidateClipPairArray(const std::vector<std::array<double, 2>> &pairs,
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
                             const Dict &clip_set,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  const std::string location = prim_location + ".clips[" + clip_set_name + "]";
  std::vector<std::string> asset_paths;
  const bool have_asset_paths =
      GetMetaStringArray(clip_set, "assetPaths", &asset_paths);

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
    const bool have_end =
        GetMetaDouble(clip_set, "templateEndTime", &end_time);
    const bool have_stride =
        GetMetaDouble(clip_set, "templateStride", &stride);
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

  std::vector<std::array<double, 2>> active;
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

  std::vector<std::array<double, 2>> times;
  if (GetMetaDouble2Array(clip_set, "times", &times)) {
    ValidateClipPairArray(times, "core.clips.times", location + ".times",
                          "times", result);
  }

  std::string prim_path;
  if (GetMetaString(clip_set, "primPath", &prim_path)) {
    std::string err;
    if (!IsValidAbsolutePrimPathString(prim_path, &err) || prim_path == "/") {
      AddError(result, "core.clips.primPath", location + ".primPath",
               "primPath must be an absolute prim path, but is `" + prim_path +
                   "`");
    }
  }

  std::string manifest_asset_path;
  if (GetMetaAssetPath(clip_set, "manifestAssetPath", &manifest_asset_path) &&
      manifest_asset_path.empty()) {
    AddError(result, "core.clips.assets", location + ".manifestAssetPath",
             "manifestAssetPath must not be empty");
  }
}

void ValidateClipsMetadata(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  const Dict *clips = ps.meta().clips().as_dictionary();
  if (!clips) {
    return;
  }
  if (clips->empty()) {
    AddError(result, "core.clips", prim_location + ".clips",
             "clips dictionary must contain at least one clip set");
    return;
  }

  for (const auto &clip_entry : clips->entries()) {
    const Dict *clip_set = clip_entry.second.as_dictionary();
    if (!clip_set) {
      AddError(result, "core.clips", prim_location + ".clips",
               "clip set `" + clip_entry.first + "` must be a dictionary");
      continue;
    }
    ValidateClipSetMetadata(clip_entry.first, *clip_set, prim_location,
                            result);
  }
}

// ---------------------------------------------------------------------------
// Attribute / relationship metadata (PropMeta), attribute type checks
// ---------------------------------------------------------------------------

void ValidateTokenAttributeType(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &location,
                                USDValidationResult *result) {
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             "expected an attribute, but found a relationship");
    return;
  }
  const std::string type_name = AttrTypeNameOf(ps, prop_name);
  if (type_name != "token") {
    AddError(result, rule_id, location,
             "expected attribute type `token`, but found `" + type_name +
                 "`");
  }
}

void ValidateExactAttributeType(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &expected,
                                const std::string &rule_id,
                                const std::string &location,
                                USDValidationResult *result) {
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             "expected an attribute, but found a relationship");
    return;
  }
  const std::string type_name = AttrTypeNameOf(ps, prop_name);
  if (type_name != expected) {
    AddError(result, rule_id, location,
             "expected attribute type `" + expected + "`, but found `" +
                 type_name + "`");
  }
}

void ValidateUniformAttribute(const PrimSpec &ps, const std::string &prop_name,
                              const std::string &rule_id,
                              const std::string &location,
                              USDValidationResult *result) {
  const PropSlot *slot = GetSlot(ps, prop_name);
  if (!slot || slot->is_relationship()) {
    return;
  }
  if (!slot->is_uniform()) {
    AddError(result, rule_id, location, "expected `uniform` variability");
  }
}

void ValidateVaryingAttribute(const PrimSpec &ps, const std::string &prop_name,
                              const std::string &rule_id,
                              const std::string &location,
                              USDValidationResult *result) {
  const PropSlot *slot = GetSlot(ps, prop_name);
  if (!slot || slot->is_relationship()) {
    return;
  }
  if (slot->is_uniform()) {
    AddError(result, rule_id, location, "expected `varying` variability");
  }
}

void ValidateAttributeMetadata(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &location,
                               USDValidationResult *result) {
  const PropMeta *metas = ps.property_meta(prop_name);
  if (!metas) {
    return;
  }

  if (metas->authored & PropMeta::kCustomData) {
    ValidateDictValue(metas->customData, "core.attr.customData",
                      location + ".customData", result);
  }
  if (metas->authored & PropMeta::kSdrMetadata) {
    ValidateDictValue(metas->sdrMetadata, "core.attr.sdrMetadata",
                      location + ".sdrMetadata", result);
  }

  if ((metas->authored & PropMeta::kInterpolation) &&
      !IsValidPrimvarInterpolation(metas->interpolation)) {
    AddError(result, "core.attr.interpolation", location,
             "invalid interpolation token `" + metas->interpolation + "`");
  }
  if ((metas->authored & PropMeta::kElementSize) && metas->elementSize <= 0) {
    AddError(result, "core.attr.elementSize", location,
             "elementSize must be greater than 0");
  }
  if ((metas->authored & PropMeta::kUnauthoredIdx) &&
      metas->unauthoredValuesIndex < -1) {
    AddError(result, "core.attr.unauthoredValuesIndex", location,
             "unauthoredValuesIndex must be -1 or greater");
  }

  if (metas->authored & PropMeta::kAllowedTokens) {
    const std::vector<std::string> &allowed = metas->allowedTokens;
    if (allowed.empty()) {
      AddWarning(result, "core.attr.allowedTokens", location,
                 "allowedTokens should not be empty when authored");
    }

    std::set<std::string> allowed_set;
    for (size_t i = 0; i < allowed.size(); i++) {
      const std::string &token = allowed[i];
      if (token.empty()) {
        AddError(result, "core.attr.allowedTokens", location,
                 "allowedTokens[" + std::to_string(i) +
                     "] must not be empty");
      } else if (!allowed_set.insert(token).second) {
        AddWarning(result, "core.attr.allowedTokens", location,
                   "duplicate allowedTokens entry `" + token + "`");
      }
    }

    const Value *v = GetAttrValue(ps, prop_name);
    if (v && !allowed_set.empty()) {
      if (const std::string *token_value = v->as_token()) {
        if (allowed_set.count(*token_value) == 0) {
          AddError(result, "core.attr.allowedTokens", location,
                   prop_name + " value `" + *token_value +
                       "` is not listed in allowedTokens");
        }
      }
      if (v->is_array() && v->type_id() == TypeId::Token) {
        const std::vector<std::string> *token_array = v->as_token_array();
        if (token_array) {
          for (size_t i = 0; i < token_array->size(); i++) {
            const std::string &token = (*token_array)[i];
            if (allowed_set.count(token) == 0) {
              AddError(result, "core.attr.allowedTokens", location,
                       prop_name + "[" + std::to_string(i) + "] value `" +
                           token + "` is not listed in allowedTokens");
            }
          }
        }
      }
    }
  }

  if (metas->authored & PropMeta::kConnectability) {
    if (metas->connectability != "full" &&
        metas->connectability != "interfaceOnly") {
      AddError(result, "core.attr.connectability", location,
               "connectability must be `full` or `interfaceOnly`, but is `" +
                   metas->connectability + "`");
    }
  }
  if ((metas->authored & PropMeta::kRenderType) &&
      metas->renderType.empty()) {
    AddWarning(result, "core.attr.renderType", location,
               "renderType should not be empty when authored");
  }
  if ((metas->authored & PropMeta::kOutputName) &&
      metas->outputName.empty()) {
    AddWarning(result, "core.attr.outputName", location,
               "outputName should not be empty when authored");
  }
}

void ValidateRelationshipMetadata(const PrimSpec &ps,
                                  const std::string &rel_name,
                                  const std::string &location,
                                  USDValidationResult *result) {
  const uint16_t flags = ps.relationship_flags(rel_name);
  if ((flags & PropSlot::kFlagVariabilityAuthored) &&
      (flags & PropSlot::kFlagVarying)) {
    AddWarning(result, "core.relationship.variability", location,
               "relationships have uniform variability; authored `varying` "
               "is ignored by USD value resolution");
  }
  const PropMeta *metas = ps.property_meta(rel_name);
  if (!metas) {
    return;
  }
  if (metas->authored & PropMeta::kCustomData) {
    ValidateDictValue(metas->customData, "core.relationship.customData",
                      location + ".customData", result);
  }
  if (metas->authored & PropMeta::kBindMaterialAs) {
    const std::string &value = metas->bindMaterialAs;
    if (value != "strongerThanDescendants" &&
        value != "weakerThanDescendants") {
      AddError(result, "core.relationship.bindMaterialAs", location,
               "bindMaterialAs must be `strongerThanDescendants` or "
               "`weakerThanDescendants`, but is `" +
                   value + "`");
    }
  }
}

// ---------------------------------------------------------------------------
// ColorSpace / CollectionAPI schema rules
// ---------------------------------------------------------------------------

bool IsCanonicalColorSpaceToken(const std::string &token) {
  static const std::unordered_set<std::string> kCanonicalTokens = {
      "lin_ap1_scene",     "lin_ap0_scene",      "lin_rec709_scene",
      "lin_p3d65_scene",   "lin_rec2020_scene",  "lin_adobergb_scene",
      "lin_ciexyzd65_scene", "srgb_rec709_scene", "g22_rec709_scene",
      "g18_rec709_scene",  "srgb_ap1_scene",     "g22_ap1_scene",
      "srgb_p3d65_scene",  "g22_adobergb_scene", "data",
      "unknown",           "raw",                "identity",
  };
  return kCanonicalTokens.count(token) > 0;
}

bool IsColorSpaceDefinitionPropertyName(const std::string &prop_name) {
  return prop_name == "name" || prop_name == "redChroma" ||
         prop_name == "greenChroma" || prop_name == "blueChroma" ||
         prop_name == "whitePoint" || prop_name == "gamma" ||
         prop_name == "linearBias" ||
         prop_name == "colorSpaceDefinition:name" ||
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
      !segments[1].empty() && !segments[2].empty()) {
    if (instance) *instance = segments[1];
    if (field) *field = segments[2];
    return IsColorSpaceDefinitionPropertyName(segments[2]);
  }
  return false;
}

ColorSpaceSet CollectLocalColorSpaceDefinitions(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas) {
  ColorSpaceSet local_defs;
  if (!HasAppliedSchema(applied_schemas, "ColorSpaceDefinitionAPI")) {
    return local_defs;
  }
  for (const AppliedSchema &schema : applied_schemas) {
    if (schema.name != "ColorSpaceDefinitionAPI") continue;
    if (schema.instance_name.empty()) {
      // Compatibility with pre-multiple-apply AOUSD drafts.
      local_defs.insert("custom");
      for (const char *prop_name : {"name", "colorSpaceDefinition:name"}) {
        std::string value;
      if (GetTokenProperty(ps, prop_name, &value) && !value.empty()) {
        local_defs.insert(value);
        local_defs.insert(::lightusd::color::CanonicalizeToken(value));
        }
      }
      continue;
    }
    local_defs.insert(schema.instance_name);
    std::string value;
    const std::string name_prop =
        "colorSpaceDefinition:" + schema.instance_name + ":name";
    if (GetTokenProperty(ps, name_prop, &value) && !value.empty()) {
      local_defs.insert(value);
      local_defs.insert(::lightusd::color::CanonicalizeToken(value));
    }
  }
  return local_defs;
}

bool IsKnownColorSpaceToken(const std::string &token,
                            const ColorSpaceSet &visible_custom_spaces) {
  if (token.empty()) {
    return true;
  }
  const std::string canonical =
      ::lightusd::color::CanonicalizeToken(token);
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
    std::string authored_name;
    if (GetTokenProperty(ps, prefix + "name", &authored_name) &&
        !authored_name.empty()) {
      name = authored_name;
    }
    const std::string canonical =
        ::lightusd::color::CanonicalizeToken(name);
    auto inserted = names.emplace(canonical, schema.instance_name);
    if (!inserted.second) {
      AddError(result, "core.schema.ColorSpaceDefinitionAPI.duplicate",
               location, "multiple definitions resolve to color space `" +
                             canonical + "`");
    }

    float red[2] = {1.0f, 0.0f};
    float green[2] = {0.0f, 1.0f};
    float blue[2] = {0.0f, 0.0f};
    float white[2] = {1.0f / 3.0f, 1.0f / 3.0f};
    const auto read_float2 = [&](const std::string &property, float out[2]) {
      const Value *value = GetAttrValue(ps, property);
      if (!value) return;
      if (const float *lanes = value->as_float2()) {
        out[0] = lanes[0];
        out[1] = lanes[1];
      }
    };
    read_float2(prefix + "redChroma", red);
    read_float2(prefix + "greenChroma", green);
    read_float2(prefix + "blueChroma", blue);
    read_float2(prefix + "whitePoint", white);
    double gamma = 1.0;
    double bias = 0.0;
    (void)GetNumericScalarProperty(ps, prefix + "gamma", &gamma);
    (void)GetNumericScalarProperty(ps, prefix + "linearBias", &bias);
    ::lightusd::color::ColorSpaceDesc definition;
    if (!::lightusd::color::MakeColorSpaceFromChromaticities(
            canonical, red, green, blue, white, static_cast<float>(gamma),
            static_cast<float>(bias), &definition)) {
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

void ValidateColorSpaceDefinitionProperty(const PrimSpec &ps,
                                          const std::string &prop_name,
                                          const std::string &location,
                                          USDValidationResult *result) {
  std::string instance;
  std::string field;
  const bool current_multiple_apply =
      ParseColorSpaceDefinitionPropertyName(prop_name, &instance, &field);
  const std::string &effective_name = current_multiple_apply ? field : prop_name;
  if (effective_name == "name" ||
      effective_name == "colorSpaceDefinition:name") {
    ValidateTokenAttributeType(ps, prop_name,
                               "core.schema.ColorSpaceDefinitionAPI.name",
                               location, result);
    ValidateUniformAttribute(ps, prop_name,
                             "core.schema.ColorSpaceDefinitionAPI.name",
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
    ValidateExactAttributeType(
        ps, prop_name, "float2",
        "core.schema.ColorSpaceDefinitionAPI.coordinates", location, result);
    ValidateVaryingAttribute(ps, prop_name,
                             "core.schema.ColorSpaceDefinitionAPI.coordinates",
                             location, result);
    return;
  }

  if (effective_name == "gamma" ||
      effective_name == "colorSpaceDefinition:gamma" ||
      effective_name == "linearBias" ||
      effective_name == "colorSpaceDefinition:linearBias") {
    ValidateExactAttributeType(ps, prop_name, "float",
                               "core.schema.ColorSpaceDefinitionAPI.curve",
                               location, result);
    ValidateVaryingAttribute(ps, prop_name,
                             "core.schema.ColorSpaceDefinitionAPI.curve",
                             location, result);
  }
}

void ValidateCollectionProperty(const PrimSpec &ps,
                                const std::vector<std::string> &segments,
                                const std::string &prop_name,
                                const std::set<std::string> &collection_instances,
                                const std::string &location,
                                USDValidationResult *result) {
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
             "collection property is authored without applying "
             "`CollectionAPI:" +
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
    ValidateTokenAttributeType(ps, prop_name,
                               "core.schema.CollectionAPI.expansionRule",
                               location, result);
    ValidateUniformAttribute(ps, prop_name,
                             "core.schema.CollectionAPI.expansionRule",
                             location, result);
    std::string value;
    if (GetTokenProperty(ps, prop_name, &value)) {
      if (value != "explicitOnly" && value != "expandPrims" &&
          value != "expandPrimsAndProperties") {
        AddError(result, "core.schema.CollectionAPI.expansionRule", location,
                 "invalid expansionRule `" + value + "`");
      }
    }
    return;
  }

  if (member == "includeRoot") {
    ValidateExactAttributeType(ps, prop_name, "bool",
                               "core.schema.CollectionAPI.includeRoot",
                               location, result);
    ValidateUniformAttribute(ps, prop_name,
                             "core.schema.CollectionAPI.includeRoot",
                             location, result);
    return;
  }

  if (member == "includes" || member == "excludes") {
    if (!IsRelationshipProp(ps, prop_name)) {
      AddError(result, "core.schema.CollectionAPI.relationship", location,
               "expected a relationship");
      return;
    }

    ValidateRelationshipScenePaths(ps, prop_name,
                                   "core.schema.CollectionAPI.relationship",
                                   location, result);

    if (member == "excludes") {
      for (const Path &target : RelTargets(ps, prop_name)) {
        std::string prop_part;
        SplitScenePathString(target.str(), nullptr, &prop_part);
        if (!prop_part.empty() && StartsWith(prop_part, "collection:")) {
          AddError(result, "core.schema.CollectionAPI.excludes", location,
                   "excludes must not target a property referring to another "
                   "collection");
        }
      }
    }
    return;
  }

  AddError(result, "core.schema.CollectionAPI.property", location,
           "unsupported CollectionAPI property `" + member + "`");
}

// ---------------------------------------------------------------------------
// Layer metadata
// ---------------------------------------------------------------------------

void ValidateLayerMetas(const Layer &layer, USDValidationResult *result) {
  const LayerMeta &metas = layer.meta();

  if (!metas.defaultPrim.empty()) {
    const std::string &default_prim = metas.defaultPrim;
    if (!IsValidIdentifier(default_prim)) {
      AddError(result, "core.layer.defaultPrim", kLayerLocation,
               "defaultPrim `" + default_prim +
                   "` is not a valid root prim identifier");
    }
    bool found = false;
    for (uint32_t root_index : layer.root_indices()) {
      const PrimSpec *root = layer.prim(root_index);
      if (root && root->name() == default_prim) {
        found = true;
        break;
      }
    }
    if (!found) {
      AddError(result, "core.layer.defaultPrim", kLayerLocation,
               "defaultPrim `" + default_prim +
                   "` does not match any root PrimSpec in the layer");
    }
  }

  // NOTE: layerRelocates are stored per-prim in next-core; see
  // ValidateRelocates() in the prim walk.

  // upAxis: the next parser stores the raw token; guard against non-X/Y/Z.
  if (metas.upAxis_set && metas.upAxis != "X" && metas.upAxis != "Y" &&
      metas.upAxis != "Z") {
    AddWarning(result, "core.layer.upAxis", kLayerLocation,
               "upAxis is authored but is not one of X, Y, or Z");
  }

  if (metas.metersPerUnit_set && !(metas.metersPerUnit > 0.0)) {
    AddError(result, "core.layer.metersPerUnit", kLayerLocation,
             "metersPerUnit must be greater than 0");
  }

  if (metas.timeCodesPerSecond_set && !(metas.timeCodesPerSecond > 0.0)) {
    AddError(result, "core.layer.timeCodesPerSecond", kLayerLocation,
             "timeCodesPerSecond must be greater than 0");
  }

  if (metas.framesPerSecond_set && !(metas.framesPerSecond > 0.0)) {
    AddError(result, "core.layer.framesPerSecond", kLayerLocation,
             "framesPerSecond must be greater than 0");
  }

  if (metas.kilogramsPerUnit_set && !(metas.kilogramsPerUnit > 0.0)) {
    AddError(result, "core.layer.kilogramsPerUnit", kLayerLocation,
             "kilogramsPerUnit must be greater than 0");
  }

  if (metas.startTimeCode_set && metas.endTimeCode_set &&
      metas.startTimeCode > metas.endTimeCode) {
    AddWarning(result, "core.layer.timeCodeRange", kLayerLocation,
               "startTimeCode is greater than endTimeCode");
  }

  for (size_t i = 0; i < metas.subLayers.size(); i++) {
    const std::string location =
        std::string(kLayerLocation) + ".subLayers[" + std::to_string(i) + "]";
    if (metas.subLayers[i].empty()) {
      AddError(result, "core.layer.subLayers", location,
               "subLayer asset path must not be empty");
    }
    if (i < metas.subLayerOffsets.size()) {
      ValidateLayerOffset(metas.subLayerOffsets[i].first,
                          metas.subLayerOffsets[i].second,
                          "core.layer.subLayerOffset", location, result);
    }
  }

  if (metas.colorConfiguration_set && metas.colorConfiguration.empty()) {
    AddError(result, "core.layer.colorConfiguration", kLayerLocation,
             "colorConfiguration asset path must not be empty");
  }
  if (metas.colorManagementSystem_set &&
      metas.colorManagementSystem.empty()) {
    AddError(result, "core.layer.colorManagementSystem", kLayerLocation,
             "colorManagementSystem must not be empty");
  }
  if (metas.owner_set && metas.owner.empty()) {
    AddWarning(result, "core.layer.owner", kLayerLocation,
               "owner should not be empty when authored");
  }

  ValidateDictValue(metas.customLayerData, "core.layer.customLayerData",
                    "<layer>.customLayerData", result);
  ValidateDictValue(metas.expressionVariables,
                    "core.layer.expressionVariables",
                    "<layer>.expressionVariables", result);
}

// ---------------------------------------------------------------------------
// Prim index (path -> type / specifier / skeleton joint count)
// ---------------------------------------------------------------------------

void BuildPrimIndex(const Layer &layer, uint32_t prim_index,
                    const std::string &prim_path,
                    PrimTypeByPath *prim_types,
                    PrimSpecifierByPath *prim_specifiers,
                    SkeletonJointCountByPath *skeleton_joints,
                    std::unordered_set<uint32_t> *visited) {
  if (!prim_types || !prim_specifiers || !visited) {
    return;
  }

  struct WorkItem {
    uint32_t prim_index{0};
    std::string prim_path;
  };
  std::vector<WorkItem> work;
  work.push_back(WorkItem{prim_index, prim_path});

  while (!work.empty()) {
    WorkItem item = std::move(work.back());
    work.pop_back();
    const PrimSpec *ps = layer.prim(item.prim_index);
    if (!ps || !visited->insert(item.prim_index).second) {
      continue;
    }

    (*prim_types)[item.prim_path] = ps->type_name();
    (*prim_specifiers)[item.prim_path] = ps->specifier();
    if (skeleton_joints && ps->type_name() == "Skeleton") {
      std::vector<std::string> joints;
      if (GetTokenArrayProperty(*ps, "joints", &joints)) {
        (*skeleton_joints)[item.prim_path] = joints.size();
      }
    }

    const std::vector<uint32_t> &children = ps->child_indices();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      const PrimSpec *child = layer.prim(*it);
      if (!child) {
        continue;
      }
      work.push_back(
          WorkItem{*it, item.prim_path + "/" + child->name()});
    }
  }
}

// ---------------------------------------------------------------------------
// arkit rules -- the ARKit / RealityKit USDZ delivery profile, modeled on
// OpenUSD's `usdchecker --arkit` (pxr/usd/usdUtils/complianceChecker.py; see
// doc/openusd-usdz.md for the rule inventory this mirrors).
//
// NOTE (structural approximations, in the spirit of the rest of this file):
//  - OpenUSD checks a *composed* stage. These rules run on the uncomposed
//    Layer, so cross-layer targets (a Material or normal-map texture brought
//    in by a reference) cannot be resolved. Target-resolution rules therefore
//    only hard-fail when the layer is self-contained (no sublayers and no
//    reference/payload asset arcs) -- which is what an ARKit-ready `.usdz`
//    root layer is required to be anyway.
//  - OpenUSD's NormalMapTextureChecker inspects the actual image bit depth.
//    We do not decode images: the 8-bit determination is made from the file
//    extension (png/jpg/jpeg/bmp/tga are treated as 8-bit; exr/hdr as float).
//  - The `package` container rules (zip layout, allowed entry extensions,
//    `.usdc` root layer) need the original bytes and are run by callers such
//    as lusdchecker under the `arkit.package.*` rule ids.
// ---------------------------------------------------------------------------

// complianceChecker.py:792 `_allowedPrimTypeNames`.
bool IsArkitAllowedPrimTypeName(const std::string &type_name) {
  if (type_name.empty()) {
    return true;  // untyped prims ("over", pure namespace parents) are allowed
  }
  if (StartsWith(type_name, "RealityKit")) {
    return true;
  }
  static const std::unordered_set<std::string> kAllowed = {
      "Scope",        "Xform",       "Camera",
      "Shader",       "Material",    "Mesh",
      "Sphere",       "Cube",        "Cylinder",
      "Cone",         "Capsule",     "GeomSubset",
      "Points",       "SkelRoot",    "Skeleton",
      "SkelAnimation", "BlendShape", "SpatialAudio",
      "PhysicsScene", "Preliminary_ReferenceImage",
      "Preliminary_Text", "Preliminary_Trigger"};
  return kAllowed.count(type_name) > 0;
}

// Lowercase file extension of an asset path (without the dot). UDIM-style
// paths ("tex.<UDIM>.png") still yield the trailing extension.
std::string LowerAssetExtension(const std::string &asset_path) {
  const size_t slash = asset_path.find_last_of("/\\");
  const size_t dot = asset_path.find_last_of('.');
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return std::string();
  }
  std::string ext = asset_path.substr(dot + 1);
  // Drop any package-relative suffix ("scene.usdz[tex.png]" is handled by the
  // caller; a trailing "]" would otherwise leak into the extension).
  const size_t bad = ext.find_first_of("[]?#");
  if (bad != std::string::npos) {
    ext = ext.substr(0, bad);
  }
  for (char &c : ext) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return ext;
}

// complianceChecker.py:232 `_basicUSDZImageFormats`.
bool IsArkitTextureExtension(const std::string &ext) {
  return ext == "exr" || ext == "jpg" || ext == "jpeg" || ext == "png";
}

// complianceChecker.py `_extsForFilesThatCanBeDecoded`: readable by the USD
// image plugins, but not portable to ARKit/RealityKit.
bool IsDecodableButNonPortableTextureExtension(const std::string &ext) {
  return ext == "bmp" || ext == "tga" || ext == "hdr" || ext == "tif" ||
         ext == "tiff" || ext == "tx" || ext == "zfile";
}

// Extension-based 8-bit determination (see NOTE above).
bool IsEightBitTextureExtension(const std::string &ext) {
  return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" ||
         ext == "tga";
}

bool IsArkitLayerExtension(const std::string &ext) {
  return ext == "usd" || ext == "usda" || ext == "usdc" || ext == "usdz";
}

// ARKitShaderChecker: `id`-implementation shaders drawn from the UsdPreview /
// MaterialX node families.
bool IsArkitShaderId(const std::string &shader_id) {
  return shader_id == "UsdPreviewSurface" || shader_id == "UsdUVTexture" ||
         shader_id == "UsdTransform2d" || IsUsdPrimvarReaderId(shader_id) ||
         StartsWith(shader_id, "UsdPrimvarReader") ||
         StartsWith(shader_id, "ND_");
}

// The shader ids lightusd's built-in registry knows: the UsdPreviewSurface
// node family plus MaterialX ND_* definitions. OpenUSD's sdr registry is
// plugin-extensible, so an id outside this set is only a WARNING (it may be
// perfectly valid in a pipeline that ships the plugin) -- mirrors
// ShaderSdrCompliance's MissingShaderIdInRegistry.
bool IsBuiltinRegistryShaderId(const std::string &shader_id) {
  return IsArkitShaderId(shader_id);
}

bool ValueToFloat4(const Value &v, std::array<double, 4> *out) {
  if (!out || v.is_array()) {
    return false;
  }
  if (v.type_id() == TypeId::Float4 || v.type_id() == TypeId::Color4f) {
    const float *p = static_cast<const float *>(v.raw_data());
    if (!p) return false;
    for (size_t i = 0; i < 4; i++) (*out)[i] = static_cast<double>(p[i]);
    return true;
  }
  if (v.type_id() == TypeId::Double4) {
    const double *p = static_cast<const double *>(v.raw_data());
    if (!p) return false;
    for (size_t i = 0; i < 4; i++) (*out)[i] = p[i];
    return true;
  }
  return false;
}

bool GetFloat4Property(const PrimSpec &ps, const std::string &name,
                       std::array<double, 4> *out) {
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  return ValueToFloat4(*v, out);
}

bool Float4Equals(const std::array<double, 4> &v, double x, double y, double z,
                  double w) {
  const double kEps = 1.0e-4;
  return std::fabs(v[0] - x) <= kEps && std::fabs(v[1] - y) <= kEps &&
         std::fabs(v[2] - z) <= kEps && std::fabs(v[3] - w) <= kEps;
}

std::string FormatFloat4(const std::array<double, 4> &v) {
  std::ostringstream ss;
  ss << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
  return ss.str();
}

// arkit.texture.format: every asset-path-valued shader input.
void ValidateArkitTextureFormats(const PrimSpec &ps,
                                 const std::string &prim_location,
                                 USDValidationResult *result) {
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    if (!IsShaderInputName(prop_name) || IsRelationshipProp(ps, prop_name)) {
      continue;
    }
    std::string asset_path;
    if (!GetAssetPathProperty(ps, prop_name, &asset_path) ||
        asset_path.empty()) {
      continue;
    }
    const std::string location = MakePropertyLocation(prim_location, prop_name);
    const std::string ext = LowerAssetExtension(asset_path);
    if (IsArkitTextureExtension(ext)) {
      continue;
    }
    if (IsDecodableButNonPortableTextureExtension(ext)) {
      AddError(result, "arkit.texture.format", location,
               "texture `" + asset_path + "` has extension `" + ext +
                   "`, which is decodable by USD but not portable to ARKit; "
                   "convert it to exr, jpg, jpeg, or png");
    } else {
      AddError(result, "arkit.texture.format", location,
               "texture `" + asset_path + "` has unsupported extension `" +
                   ext + "`; ARKit allows only exr, jpg, jpeg, and png");
    }
  }
}

// An 8-bit UsdUVTexture feeding a UsdPreviewSurface `inputs:normal` must
// remap [0,1] to [-1,1] and stay raw. Shared by the always-on shade rule
// (`shade.normalMap.scaleBias`, warning -- OpenUSD's NormalMapTextureValidator)
// and the ARKit profile (`arkit.normalMap.scaleBias`, error).
void ValidateNormalMapTextureImpl(const Layer &layer, const PrimSpec &surface,
                                  const std::string &surface_location,
                                  const char *rule_id,
                                  USDValidationSeverity severity,
                                  USDValidationResult *result) {
  const std::vector<Path> *conns = surface.connection("inputs:normal");
  if (!conns) {
    return;
  }
  for (const Path &conn : *conns) {
    std::string prim_part;
    SplitScenePathString(conn.str(), &prim_part, nullptr);
    const PrimSpec *tex = layer.prim_at_path(prim_part);
    if (!tex || tex->type_name() != "Shader" ||
        GetShaderInfoId(*tex) != "UsdUVTexture") {
      // Cross-layer or non-texture source; nothing structural to check.
      continue;
    }
    std::string file;
    if (!GetAssetPathProperty(*tex, "inputs:file", &file) || file.empty()) {
      continue;
    }
    if (!IsEightBitTextureExtension(LowerAssetExtension(file))) {
      continue;
    }

    const std::string scale_location =
        MakePropertyLocation(prim_part, "inputs:scale");
    std::array<double, 4> scale{};
    if (!GetFloat4Property(*tex, "inputs:scale", &scale)) {
      AddIssue(result, severity, rule_id, scale_location,
               "8-bit normal map read by `" + surface_location +
                   ".inputs:normal` must author inputs:scale = (2, 2, 2, 1)");
    } else if (!Float4Equals(scale, 2.0, 2.0, 2.0, 1.0)) {
      AddIssue(result, severity, rule_id, scale_location,
               "8-bit normal map inputs:scale must be (2, 2, 2, 1), but is " +
                   FormatFloat4(scale));
    }

    const std::string bias_location =
        MakePropertyLocation(prim_part, "inputs:bias");
    std::array<double, 4> bias{};
    if (!GetFloat4Property(*tex, "inputs:bias", &bias)) {
      AddIssue(result, severity, rule_id, bias_location,
               "8-bit normal map read by `" + surface_location +
                   ".inputs:normal` must author inputs:bias = (-1, -1, -1, 0)");
    } else if (!Float4Equals(bias, -1.0, -1.0, -1.0, 0.0)) {
      AddIssue(result, severity, rule_id, bias_location,
               "8-bit normal map inputs:bias must be (-1, -1, -1, 0), but is " +
                   FormatFloat4(bias));
    }

    std::string color_space;
    const std::string cs_location =
        MakePropertyLocation(prim_part, "inputs:sourceColorSpace");
    if (!GetTokenProperty(*tex, "inputs:sourceColorSpace", &color_space)) {
      AddIssue(result, severity, rule_id, cs_location,
               "normal map read by `" + surface_location +
                   ".inputs:normal` must author "
                   "inputs:sourceColorSpace = \"raw\"");
    } else if (color_space != "raw") {
      AddIssue(result, severity, rule_id, cs_location,
               "normal map inputs:sourceColorSpace must be `raw`, but is `" +
                   color_space + "`");
    }
  }
}

// arkit.shader.*
void ValidateArkitShader(const Layer &layer, const PrimSpec &ps,
                         const std::string &prim_location,
                         USDValidationResult *result) {
  std::string impl_source;
  if (GetTokenProperty(ps, "info:implementationSource", &impl_source) &&
      impl_source != "id") {
    AddError(result, "arkit.shader.implementationSource",
             MakePropertyLocation(prim_location, "info:implementationSource"),
             "ARKit shaders must use the `id` implementation source, but `" +
                 impl_source + "` is authored");
  }

  const std::string shader_id = GetShaderInfoId(ps);
  if (shader_id.empty()) {
    AddError(result, "arkit.shader.id",
             MakePropertyLocation(prim_location, "info:id"),
             "Shader must author an `info:id` token");
  } else if (!IsArkitShaderId(shader_id)) {
    AddError(result, "arkit.shader.id",
             MakePropertyLocation(prim_location, "info:id"),
             "shader id `" + shader_id +
                 "` is not supported by ARKit; allowed ids are "
                 "UsdPreviewSurface, UsdUVTexture, UsdTransform2d, "
                 "UsdPrimvarReader*, and ND_*");
  }

  // arkit.shader.connection: at most one connection source per input.
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    if (!IsShaderInputName(prop_name)) {
      continue;
    }
    const std::vector<Path> *conns = ps.connection(prop_name);
    if (conns && conns->size() > 1) {
      AddError(result, "arkit.shader.connection",
               MakePropertyLocation(prim_location, prop_name),
               "ARKit allows at most one connection per shader input, but `" +
                   prop_name + "` has " + std::to_string(conns->size()));
    }
  }

  ValidateArkitTextureFormats(ps, prim_location, result);

  if (shader_id == "UsdPreviewSurface") {
    ValidateNormalMapTextureImpl(layer, ps, prim_location,
                                 "arkit.normalMap.scaleBias",
                                 USDValidationSeverity::Error, result);
  }
}

// arkit.material.binding: direct and collection-based bindings must resolve to
// an existing Material prim.
void ValidateArkitMaterialBinding(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  const PrimTypeByPath &prim_types,
                                  bool layer_self_contained,
                                  USDValidationResult *result) {
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    if (prop_name != "material:binding" &&
        !StartsWith(prop_name, "material:binding:")) {
      continue;
    }
    if (!IsRelationshipProp(ps, prop_name)) {
      // Reported as an error by shade.material.binding.
      continue;
    }
    const std::string location = MakePropertyLocation(prim_location, prop_name);
    const bool is_collection =
        StartsWith(prop_name, "material:binding:collection:");
    const std::vector<Path> &targets = RelTargets(ps, prop_name);
    if (targets.empty()) {
      AddError(result, "arkit.material.binding", location,
               "material binding relationship has no target");
      continue;
    }
    bool found_material = false;
    for (const Path &target : targets) {
      std::string prim_part;
      std::string prop_part;
      SplitScenePathString(target.str(), &prim_part, &prop_part);
      if (!prop_part.empty()) {
        // The collection property target of a collection-based binding.
        if (!is_collection) {
          AddError(result, "arkit.material.binding", location,
                   "direct material binding target `" + target.str() +
                       "` must be a Material prim path, not a property path");
        }
        continue;
      }
      const std::string *target_type = FindPrimType(prim_types, prim_part);
      if (!target_type) {
        if (layer_self_contained) {
          AddError(result, "arkit.material.binding", location,
                   "material binding target `" + prim_part +
                       "` does not resolve to a prim in this layer");
        }
        continue;
      }
      if (*target_type != "Material") {
        AddError(result, "arkit.material.binding", location,
                 "material binding target `" + prim_part + "` is a `" +
                     *target_type + "`, not a Material");
        continue;
      }
      found_material = true;
    }
    if (is_collection && !found_material && layer_self_contained) {
      AddError(result, "arkit.material.binding", location,
               "collection-based material binding does not target a Material "
               "prim");
    }
  }
}

// arkit.layer.extension: reference / payload asset paths (sublayers are
// checked at layer scope).
void ValidateArkitCompositionAssetPaths(const PrimSpec &ps,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  const PrimSpecMeta &m = ps.meta();
  const ArcListOpEdits *edits = m.arc_edits();
  const auto check = [&](const std::vector<std::string> &arcs,
                         const char *what) {
    for (const std::string &encoded : arcs) {
      const CompositionArc arc = Compositor::ParseReference(encoded);
      if (arc.asset_path.empty()) {
        continue;
      }
      const std::string ext = LowerAssetExtension(arc.asset_path);
      if (!IsArkitLayerExtension(ext)) {
        AddError(result, "arkit.layer.extension", prim_location,
                 std::string(what) + " asset `" + arc.asset_path +
                     "` must be a usd, usda, usdc, or usdz layer");
      }
    }
  };
  check(GatherArcStrings(m.references, edits ? &edits->references : nullptr),
        "reference");
  check(GatherArcStrings(m.payloads, edits ? &edits->payloads : nullptr),
        "payload");
}

void ValidateArkitPrim(const Layer &layer, const PrimSpec &ps,
                       const std::string &prim_location,
                       const PrimTypeByPath &prim_types,
                       bool layer_self_contained,
                       USDValidationResult *result) {
  const std::string &type_name = ps.type_name();
  if (!IsArkitAllowedPrimTypeName(type_name)) {
    AddError(result, "arkit.prim.type", prim_location,
             "prim type `" + type_name +
                 "` is not in the ARKit-supported prim type set");
  }

  if (type_name == "Shader") {
    ValidateArkitShader(layer, ps, prim_location, result);
  }

  ValidateArkitMaterialBinding(ps, prim_location, prim_types,
                               layer_self_contained, result);
  ValidateArkitCompositionAssetPaths(ps, prim_location, result);
}

// arkit.stage.* / arkit.layer.extension at layer scope.
void ValidateArkitLayerMetas(const Layer &layer, USDValidationResult *result) {
  const LayerMeta &metas = layer.meta();

  if (!metas.upAxis_set) {
    AddError(result, "arkit.stage.upAxis", kLayerLocation,
             "ARKit assets must author upAxis = \"Y\"");
  } else if (metas.upAxis != "Y") {
    AddError(result, "arkit.stage.upAxis", kLayerLocation,
             "ARKit assets must be Y-up, but upAxis is `" + metas.upAxis + "`");
  }

  if (!metas.metersPerUnit_set) {
    AddError(result, "arkit.stage.metersPerUnit", kLayerLocation,
             "ARKit assets must author metersPerUnit");
  }

  if (metas.defaultPrim.empty()) {
    AddError(result, "arkit.stage.defaultPrim", kLayerLocation,
             "ARKit assets must author a defaultPrim");
  }

  for (size_t i = 0; i < metas.subLayers.size(); i++) {
    const std::string &sublayer = metas.subLayers[i];
    if (sublayer.empty()) {
      continue;  // reported by core.layer.subLayers
    }
    const std::string ext = LowerAssetExtension(sublayer);
    if (!IsArkitLayerExtension(ext)) {
      AddError(result, "arkit.layer.extension",
               std::string(kLayerLocation) + ".subLayers[" +
                   std::to_string(i) + "]",
               "subLayer asset `" + sublayer +
                   "` must be a usd, usda, usdc, or usdz layer");
    }
  }
}

// A layer whose targets are all local: no sublayers and no external
// reference/payload asset arcs. Only then can missing targets be reported.
bool IsSelfContainedLayer(const Layer &layer) {
  if (!layer.meta().subLayers.empty()) {
    return false;
  }
  for (const PrimSpec &ps : layer.prims()) {
    const PrimSpecMeta &m = ps.meta();
    const ArcListOpEdits *edits = m.arc_edits();
    for (const std::vector<std::string> &arcs :
         {GatherArcStrings(m.references,
                           edits ? &edits->references : nullptr),
          GatherArcStrings(m.payloads, edits ? &edits->payloads : nullptr)}) {
      for (const std::string &encoded : arcs) {
        if (!Compositor::ParseReference(encoded).asset_path.empty()) {
          return false;
        }
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Prim validation (explicit work stack; safe for deep hierarchies)
// ---------------------------------------------------------------------------

struct PrimChildValidationState {
  ColorSpaceSet color_spaces;
  AncestorContext ancestors;
};

bool ValidateOnePrimSpec(const Layer &layer, uint32_t prim_index,
                         const std::string &prim_location,
                         const ColorSpaceSet &inherited_color_spaces,
                         const AncestorContext &ancestors,
                         const ValidationOptions &options,
                         const PrimTypeByPath &prim_types,
                         const PrimSpecifierByPath &prim_specifiers,
                         const SkeletonJointCountByPath &skeleton_joints,
                         bool layer_self_contained,
                         std::unordered_set<uint32_t> *visited,
                         USDValidationResult *result,
                         PrimChildValidationState *child_state) {
  const PrimSpec *psp = layer.prim(prim_index);
  if (!psp || !visited || !child_state ||
      !visited->insert(prim_index).second) {
    return false;
  }
  const PrimSpec &ps = *psp;
  const std::vector<AppliedSchema> applied_schemas = CollectAppliedSchemas(ps);
  const std::string &type_name = ps.type_name();
  const bool has_arc = HasCompositionArc(ps);
  const bool is_over = IsOverride(ps);

  // ---- core.prim.name: prim name must be a valid USD identifier ----
  if (options.core && !ps.name().empty() && !IsValidIdentifier(ps.name())) {
    AddError(result, "core.prim.name", prim_location,
             "prim name `" + ps.name() + "` is not a valid identifier");
  }

  // ---- geom.encapsulation.nestedGprim ----
  if (options.geom && IsGprimTypeName(type_name) &&
      ancestors.has_gprim_ancestor) {
    AddWarning(result, "geom.encapsulation.nestedGprim", prim_location,
               "Gprim `" + type_name + "` is nested under another Gprim at `" +
                   ancestors.nearest_gprim_path + "`");
  }

  // ---- shade.encapsulation.imageableInMaterial ----
  // OpenUSD's EncapsulationMaterialValidator: only connectable prims (Shader,
  // NodeGraph) belong under a Material; any other imageable descendant (a
  // Scope, Xform, Gprim, ...) is flagged. Connectable mis-parents are
  // shade.encapsulation.shaderParent; this rule is about the non-connectables.
  if (options.shade && ancestors.has_material_ancestor &&
      !IsShadeConnectableTypeName(type_name) &&
      IsNonConnectableImageableTypeName(type_name)) {
    AddWarning(result, "shade.encapsulation.imageableInMaterial", prim_location,
               "Imageable `" + type_name +
                   "` is not a valid descendant of Material `" +
                   ancestors.nearest_material_path + "`");
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
      // SubsetParentIsImageable: a GeomSubset only means something as the
      // DIRECT child of the imageable it subsets.
      if (ancestors.parent_type.empty() ||
          (!IsGprimTypeName(ancestors.parent_type) &&
           !IsNonConnectableImageableTypeName(ancestors.parent_type))) {
        AddWarning(result, "geom.subset.parent", prim_location,
                   "GeomSubset must be a direct child of an Imageable prim, "
                   "but its parent is `" +
                       (ancestors.parent_type.empty()
                            ? std::string("<root>")
                            : ancestors.parent_type) +
                       "`");
      }
      // SubsetsMaterialBindFamily: an explicitly unrestricted familyType on
      // a materialBind-family subset defeats material-binding semantics.
      std::string family_type;
      std::string family_name;
      (void)GetTokenProperty(ps, "familyName", &family_name);
      if (family_name == "materialBind" &&
          GetTokenProperty(ps, "familyType", &family_type) &&
          family_type == "unrestricted") {
        AddWarning(result, "geom.subset.familyType",
                   MakePropertyLocation(prim_location, "familyType"),
                   "materialBind-family GeomSubset must use a restricted "
                   "familyType (nonOverlapping or partition)");
      }
    } else if (type_name == "Points") {
      ValidateGeomPoints(ps, prim_location, result);
    } else if (type_name == "BasisCurves" || type_name == "NurbsCurves" ||
               type_name == "HermiteCurves" || type_name == "NurbsPatch") {
      ValidateGeomCurves(ps, prim_location, result);
    } else if (type_name == "PointInstancer") {
      ValidatePointInstancer(ps, prim_location, result);
    } else if (type_name == "Camera") {
      ValidateCamera(ps, prim_location, result);
    } else if (type_name == "Volume") {
      // UsdVol runs under `geom` BY CONTRACT: Volume is a Gprim (a
      // renderable), and its field-asset prims are the data sources that
      // geometry references — unlike UsdRender, which is pure scene
      // description and has its own group below.
      ValidateVolume(ps, prim_location, result);
    } else if (type_name == "OpenVDBAsset" || type_name == "Field3DAsset") {
      ValidateFieldAsset(ps, prim_location, result);
    } else if (type_name == "ParticleField" ||
               type_name == "ParticleField3DGaussianSplat") {
      ValidateParticleField(ps, applied_schemas, prim_location, result);
    }
    ValidateOpenUsd2608GeomAPIs(ps, applied_schemas, prim_location, result);
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

  // UsdRender is its own rule group: scene-description checks, not geometry.
  if (options.render && !has_arc && !is_over &&
      (type_name == "RenderSettings" || type_name == "RenderProduct" ||
       type_name == "RenderVar" || type_name == "RenderPass")) {
    ValidateRenderPrim(ps, prim_location, prim_types, result);
  }

  if (options.physics && !has_arc && !is_over) {
    ValidatePhysics(ps, applied_schemas, prim_location, prim_types, ancestors,
                    result);
  }

  if (options.geom && !has_arc && !is_over) {
    ValidateSkelBinding(ps, applied_schemas, prim_location, prim_types,
                        ancestors, result);
  }

  if (options.core) {
    ValidateCompositionMetadata(ps, prim_location, prim_specifiers, result);
    ValidatePrimMetadata(ps, prim_location, applied_schemas, result);
    ValidateClipsMetadata(ps, prim_location, result);
  }

  // ---- arkit.prim.type / arkit.shader.* / arkit.texture.format /
  //      arkit.normalMap.scaleBias / arkit.material.binding /
  //      arkit.layer.extension ----
  if (options.arkit) {
    ValidateArkitPrim(layer, ps, prim_location, prim_types,
                      layer_self_contained, result);
  }

  // ---- shade.encapsulation.shaderParent ----
  if (options.shade && IsShadeConnectableTypeName(type_name) && !has_arc &&
      !is_over && !ancestors.parent_type.empty() &&
      !IsShadeContainerTypeName(ancestors.parent_type)) {
    AddWarning(result, "shade.encapsulation.shaderParent", prim_location,
               type_name + " must be parented by a Material or NodeGraph, but "
               "its parent is `" + ancestors.parent_type + "`");
  }

  const std::string shader_id = (options.shade && type_name == "Shader")
                                    ? GetShaderInfoId(ps)
                                    : std::string();

  // ---- shade.subset.materialBindFamily ----
  // SubsetMaterialBindFamilyName: a GeomSubset that binds a material must be
  // in the `materialBind` family, or the binding is ignored by renderers.
  if (options.shade && !has_arc && !is_over && type_name == "GeomSubset") {
    bool has_material_binding = false;
    for (const std::string &prop_name : AllPropertyNames(ps)) {
      if (prop_name == "material:binding" ||
          StartsWith(prop_name, "material:binding:")) {
        has_material_binding = true;
        break;
      }
    }
    if (has_material_binding) {
      std::string family_name;
      (void)GetTokenProperty(ps, "familyName", &family_name);
      if (family_name != "materialBind") {
        AddWarning(result, "shade.subset.materialBindFamily",
                   MakePropertyLocation(prim_location, "familyName"),
                   family_name.empty()
                       ? std::string(
                             "GeomSubset with a material binding must author "
                             "familyName = \"materialBind\"")
                       : "GeomSubset with a material binding must be in the "
                         "`materialBind` family, but is in `" +
                             family_name + "`");
      }
    }
  }

  // ---- shade.shader.id ----
  // ShaderSdrCompliance: an `id`-implementation shader must name a shader the
  // registry knows. `sourceAsset`/`sourceCode` shaders resolve elsewhere and
  // are exempt.
  if (options.shade && !has_arc && !is_over && type_name == "Shader") {
    std::string impl_source;
    const bool has_impl_source =
        GetTokenProperty(ps, "info:implementationSource", &impl_source);
    if (!has_impl_source || impl_source == "id") {
      if (shader_id.empty()) {
        AddWarning(result, "shade.shader.id",
                   MakePropertyLocation(prim_location, "info:id"),
                   "Shader should author an `info:id` token (or use a "
                   "sourceAsset/sourceCode implementation source)");
      } else if (!IsBuiltinRegistryShaderId(shader_id)) {
        AddWarning(result, "shade.shader.id",
                   MakePropertyLocation(prim_location, "info:id"),
                   "shader id `" + shader_id +
                       "` is not in the built-in shader registry");
      }
    } else if (impl_source == "sourceAsset" || impl_source == "sourceCode") {
      // ShaderSdrCompliance.MissingSourceTypeInRegistry: the sourceType
      // spelled in `info:<sourceType>:sourceAsset` / `:sourceCode` must be a
      // registered shading language. Only osl and glslfx ship with stock
      // OpenUSD; anything else (mdl, ...) needs a renderer plugin.
      const std::string suffix =
          impl_source == "sourceAsset" ? ":sourceAsset" : ":sourceCode";
      for (const std::string &prop_name : AllPropertyNames(ps)) {
        if (!StartsWith(prop_name, "info:") || !EndsWith(prop_name, suffix)) {
          continue;
        }
        const std::string source_type = prop_name.substr(
            5, prop_name.size() - 5 - suffix.size());
        if (source_type.empty() || source_type.find(':') != std::string::npos) {
          continue;  // info:sourceAsset (untyped) or a nested key
        }
        if (source_type != "osl" && source_type != "glslfx") {
          AddWarning(result, "shade.shader.id",
                     MakePropertyLocation(prim_location, prop_name),
                     "shader source type `" + source_type +
                         "` is not in the built-in shader registry");
        }
      }
    }
  }

  // ---- shade.preview.* / shade.uvTexture.* / shade.primvarReader.* ----
  if (options.shade && shader_id == "UsdPreviewSurface") {
    ValidateUsdPreviewSurface(ps, prim_location, result);
    // NormalMapTextureValidator runs unconditionally in usdchecker, not just
    // under the ARKit profile. Warning here; the ARKit profile re-checks at
    // error severity.
    if (!options.arkit) {
      ValidateNormalMapTextureImpl(layer, ps, prim_location,
                                   "shade.normalMap.scaleBias",
                                   USDValidationSeverity::Warning, result);
    }
  } else if (options.shade && shader_id == "UsdUVTexture") {
    ValidateUsdUVTexture(ps, prim_location, result);
  } else if (options.shade && IsUsdPrimvarReaderId(shader_id)) {
    ValidateUsdPrimvarReader(ps, shader_id, prim_location, result);
  }
  if (options.shade) {
    ValidateMaterialXConfig(ps, applied_schemas, prim_location, result);
    if (type_name == "Shader") {
      ValidateMaterialXShader(ps, shader_id, prim_location, result);
    }
    ValidateMaterialXSynthesizedMaterial(ps, prim_location, prim_types, result);
    ValidateMaterialXReferenceConventions(ps, prim_location, result);
  }

  std::set<std::string> collection_instances;
  if (options.core) {
    collection_instances = CollectAppliedCollectionInstances(applied_schemas);
  }

  if (options.core) {
    for (const auto &schema : applied_schemas) {
      if (schema.name == "ColorSpaceAPI" &&
          !schema.instance_name.empty()) {
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

  // Per-property checks (attribute slots + relationships).
  for (const std::string &prop_name : AllPropertyNames(ps)) {
    const bool is_rel = IsRelationshipProp(ps, prop_name);
    const std::string prop_location =
        MakePropertyLocation(prim_location, prop_name);

    if (options.core && !is_rel) {
      ValidateAttributeMetadata(ps, prop_name, prop_location, result);
      ValidateTimeSamplesProp(ps, prop_name, "core.attr.timeSamples",
                              prop_location, result);
    } else if (options.core && is_rel) {
      ValidateRelationshipMetadata(ps, prop_name, prop_location, result);
    }

    if (options.core && prop_name == "colorSpace:name") {
      if (!has_color_space_api) {
        AddError(result, "core.schema.ColorSpaceAPI.applied", prop_location,
                 "`colorSpace:name` is authored without applying "
                 "ColorSpaceAPI");
      }
      ValidateTokenAttributeType(ps, prop_name,
                                 "core.schema.ColorSpaceAPI.name",
                                 prop_location, result);
      ValidateUniformAttribute(ps, prop_name, "core.schema.ColorSpaceAPI.name",
                               prop_location, result);
      std::string token;
      if (GetTokenProperty(ps, prop_name, &token)) {
        ValidateColorSpaceToken(token, visible_color_spaces,
                                "core.schema.ColorSpaceAPI.name",
                                prop_location, result);
      }
    } else if (options.core && StartsWith(prop_name, "collection:")) {
      ValidateCollectionProperty(ps, SplitString(prop_name, ':'), prop_name,
                                 collection_instances, prop_location, result);
    } else if (options.core && StartsWith(prop_name, "colorSpaceDefinition:")) {
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
      ValidateColorSpaceDefinitionProperty(ps, prop_name, prop_location,
                                           result);
    } else if (options.core && has_color_space_definition_api &&
               IsColorSpaceDefinitionPropertyName(prop_name)) {
      ValidateColorSpaceDefinitionProperty(ps, prop_name, prop_location,
                                           result);
    }

    if (options.core && !is_rel) {
      const PropMeta *meta = ps.property_meta(prop_name);
      if (meta && (meta->authored & PropMeta::kColorSpace)) {
        ValidateColorSpaceToken(meta->colorSpace, visible_color_spaces,
                                "core.attr.colorSpace", prop_location, result);
      }
    }

    // ---- shade.connection.target ----
    if (options.shade && !has_arc && !is_over && !is_rel &&
        HasConnections(ps, prop_name) &&
        (type_name == "Material" || type_name == "Shader" ||
         type_name == "NodeGraph") &&
        (IsShaderInputName(prop_name) || IsShaderOutputName(prop_name))) {
      const bool require_output_target =
          (type_name == "Material" && IsMaterialTerminalOutputName(prop_name));
      ValidateConnectionTargets(ps, prop_name, "shade.connection.target",
                                prop_location, prim_types, result,
                                require_output_target);
    }

    // ---- shade.material.outputConnection ----
    if (options.shade && !has_arc && !is_over && type_name == "Material" &&
        IsMaterialTerminalOutputName(prop_name)) {
      if (is_rel) {
        AddError(result, "shade.material.outputConnection", prop_location,
                 "`" + prop_name + "` must be a connection attribute");
      } else if (!HasConnections(ps, prop_name)) {
        AddError(result, "shade.material.outputConnection", prop_location,
                 "`" + prop_name +
                     "` must connect to a shader or node graph output");
      }
    }

    // ---- shade.material.binding ----
    if (options.shade && (prop_name == "material:binding" ||
                          StartsWith(prop_name, "material:binding:"))) {
      if (!is_rel) {
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

  // ---- core.xformOp.order ----
  if (options.core && !has_arc && !is_over) {
    std::vector<std::string> order;
    if (GetTokenArrayProperty(ps, "xformOpOrder", &order)) {
      for (const std::string &op_name : order) {
        if (op_name == "!resetXformStack!") {
          continue;
        }
        if (!HasProperty(ps, op_name)) {
          AddError(result, "core.xformOp.order",
                   MakePropertyLocation(prim_location, "xformOpOrder"),
                   "xformOpOrder references `" + op_name +
                       "` but no such attribute is authored on the prim");
        }
      }
    }
  }

  // Propagate ancestor context to children.
  AncestorContext child_ctx;
  child_ctx.parent_type = type_name;
  child_ctx.parent_mesh_face_count_known = local_mesh_face_count_known;
  child_ctx.parent_mesh_face_count = local_mesh_face_count;
  child_ctx.has_skel_root_ancestor =
      ancestors.has_skel_root_ancestor || type_name == "SkelRoot";
  child_ctx.has_articulation_ancestor =
      ancestors.has_articulation_ancestor ||
      HasAppliedSchema(applied_schemas, "PhysicsArticulationRootAPI");
  if (type_name == "Material") {
    child_ctx.has_material_ancestor = true;
    child_ctx.nearest_material_path = prim_location;
  } else {
    child_ctx.has_material_ancestor = ancestors.has_material_ancestor;
    child_ctx.nearest_material_path = ancestors.nearest_material_path;
  }
  if (IsGprimTypeName(type_name)) {
    child_ctx.has_gprim_ancestor = true;
    child_ctx.nearest_gprim_path = prim_location;
  } else {
    child_ctx.has_gprim_ancestor = ancestors.has_gprim_ancestor;
    child_ctx.nearest_gprim_path = ancestors.nearest_gprim_path;
  }

  child_state->color_spaces = std::move(visible_color_spaces);
  child_state->ancestors = std::move(child_ctx);
  return true;
}

void ValidatePrimSpecs(const Layer &layer, uint32_t prim_index,
                       const std::string &prim_location,
                       const ColorSpaceSet &inherited_color_spaces,
                       const AncestorContext &ancestors,
                       const ValidationOptions &options,
                       const PrimTypeByPath &prim_types,
                       const PrimSpecifierByPath &prim_specifiers,
                       const SkeletonJointCountByPath &skeleton_joints,
                       bool layer_self_contained,
                       std::unordered_set<uint32_t> *visited,
                       USDValidationResult *result) {
  struct WorkItem {
    uint32_t prim_index{0};
    std::string prim_location;
    ColorSpaceSet color_spaces;
    AncestorContext ancestors;
  };
  std::vector<WorkItem> work;
  work.push_back(
      WorkItem{prim_index, prim_location, inherited_color_spaces, ancestors});

  while (!work.empty()) {
    WorkItem item = std::move(work.back());
    work.pop_back();
    PrimChildValidationState child_state;
    if (!ValidateOnePrimSpec(
            layer, item.prim_index, item.prim_location, item.color_spaces,
            item.ancestors, options, prim_types, prim_specifiers,
            skeleton_joints, layer_self_contained, visited, result,
            &child_state)) {
      continue;
    }

    const PrimSpec *ps = layer.prim(item.prim_index);
    if (!ps) {
      continue;
    }
    const std::vector<uint32_t> &children = ps->child_indices();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      const PrimSpec *child = layer.prim(*it);
      if (!child) {
        continue;
      }
      work.push_back(WorkItem{*it,
                              item.prim_location + "/" + child->name(),
                              child_state.color_spaces,
                              child_state.ancestors});
    }
  }
}

}  // namespace validation_detail
using namespace validation_detail;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

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
  options.render = true;
  options.package = true;
  options.crate = true;
  // `arkit` stays off: it is an opt-in delivery profile (Y-up, whitelisted
  // prim types, portable textures), not a class of defects. See the header.
  return options;
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
  if (options.render) {
    groups.emplace_back("render");
  }
  if (options.package) {
    groups.emplace_back("package");
  }
  if (options.crate) {
    groups.emplace_back("crate");
  }
  if (options.arkit) {
    groups.emplace_back("arkit");
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
  dst->checked_groups.package =
      dst->checked_groups.package || src.checked_groups.package;
  dst->checked_groups.crate =
      dst->checked_groups.crate || src.checked_groups.crate;
  dst->checked_groups.arkit =
      dst->checked_groups.arkit || src.checked_groups.arkit;
}

USDValidationResult ValidateLayerAgainstAOUSDCore(const Layer &layer) {
  return ValidateLayerAgainstAOUSDCore(layer, ValidationOptions());
}

void ValidateStageMetadataPresence(const Layer &layer,
                                   const ValidationOptions &options,
                                   USDValidationResult *result) {
  const LayerMeta &metas = layer.meta();
  if (options.geom) {
    // usdGeomValidators:StageMetadataChecker (always-on in usdchecker, even
    // under --noAssetChecks). Warnings by default;
    // ApplyUsdcheckerCompatSeverities upgrades them to usdchecker's errors.
    if (!metas.upAxis_set) {
      AddWarning(result, "geom.stage.upAxis", kLayerLocation,
                 "stage does not specify an upAxis");
    }
    if (!metas.metersPerUnit_set) {
      AddWarning(result, "geom.stage.metersPerUnit", kLayerLocation,
                 "stage does not specify its linear scale in metersPerUnit");
    }
  }
  if (options.core && options.asset_checks && metas.defaultPrim.empty()) {
    // usdValidation:StageMetadataChecker -- a referenceable asset must name a
    // defaultPrim. Gated by asset_checks (usdchecker --noAssetChecks).
    // Validity of an AUTHORED defaultPrim is core.layer.defaultPrim.
    AddWarning(result, "core.layer.defaultPrim.missing", kLayerLocation,
               "stage does not author a defaultPrim; it cannot be safely "
               "referenced as an asset");
  }
}

namespace {

std::string MapVariantContentLocation(const std::string &base,
                                      const std::string &nested_location) {
  if (base.empty()) return nested_location;
  static const std::string kSelf = "/__self__";
  if (nested_location == kSelf) return base;
  if (StartsWith(nested_location, kSelf + "/")) {
    return base + nested_location.substr(kSelf.size());
  }
  if (nested_location == kLayerLocation || nested_location.empty()) {
    return base;
  }
  return base + ":" + nested_location;
}

// Validate one layer without descending into variant-content layers.
// `is_root_layer` is false for the synthetic graft layers that hold variant
// option bodies: those carry no stage metadata of their own, so layer-scope
// ARKit stage rules must not fire on them.
USDValidationResult ValidateLayerLocal(const Layer &layer,
                                       const ValidationOptions &options,
                                       bool is_root_layer) {
  USDValidationResult result;
  result.checked_groups = options;
  // Container groups need the original package/crate bytes and are run by
  // callers that retain them (for example lusdchecker), not by Layer-only
  // semantic validation.
  result.checked_groups.package = false;
  result.checked_groups.crate = false;

  if (options.core) {
    ValidateLayerMetas(layer, &result);
  }

  // Stage-metadata PRESENCE rules, modeled on OpenUSD usdchecker. They apply
  // to the ROOT layer only: a sublayer or variant fragment legitimately
  // inherits stage metadata from its parent. Composed-layer callers disable
  // them (stage_presence_checks) and check the authored root layer instead.
  if (is_root_layer && options.stage_presence_checks) {
    ValidateStageMetadataPresence(layer, options, &result);
  }

  if (options.arkit && is_root_layer) {
    ValidateArkitLayerMetas(layer, &result);
  }
  // A variant-content fragment (is_root_layer == false) is only PART of the
  // composed namespace: its relationship/connection targets may resolve to
  // prims outside the fragment, and its composed ancestor chain (SkelRoot,
  // Gprim, ...) is not present. So it is never treated as self-contained for
  // target resolution, and ancestor-context rules must not hard-fail on it.
  const bool layer_self_contained =
      options.arkit && is_root_layer ? IsSelfContainedLayer(layer) : false;
  AncestorContext root_ancestors;
  if (!is_root_layer) {
    // Unknown composed ancestor: assume it could satisfy the placement rules so
    // a valid asset's variant body is not falsely rejected.
    root_ancestors.has_skel_root_ancestor = true;
    root_ancestors.has_articulation_ancestor = true;
  }

  PrimTypeByPath prim_types;
  PrimSpecifierByPath prim_specifiers;
  SkeletonJointCountByPath skeleton_joints;
  {
    std::unordered_set<uint32_t> visited;
    for (uint32_t root_index : layer.root_indices()) {
      const PrimSpec *root = layer.prim(root_index);
      if (!root) {
        continue;
      }
      BuildPrimIndex(layer, root_index, "/" + root->name(), &prim_types,
                     &prim_specifiers, &skeleton_joints, &visited);
    }
  }

  std::unordered_set<uint32_t> visited;
  for (uint32_t root_index : layer.root_indices()) {
    const PrimSpec *root = layer.prim(root_index);
    if (!root) {
      continue;
    }
    ValidatePrimSpecs(layer, root_index, "/" + root->name(), ColorSpaceSet(),
                      root_ancestors, options, prim_types, prim_specifiers,
                      skeleton_joints, layer_self_contained, &visited,
                      &result);
  }

  return result;
}

USDValidationResult ValidateLayerImpl(const Layer &layer,
                                      const ValidationOptions &options,
                                      bool is_root_layer) {
  enum class ActionKind { ValidateLayer, ValidateVariantSets, LeaveLayer };
  struct Action {
    ActionKind kind{ActionKind::ValidateLayer};
    const Layer *layer{nullptr};
    const std::vector<VariantSetData> *sets{nullptr};
    bool is_root_layer{false};
    std::string location;
  };

  USDValidationResult result;
  result.checked_groups = options;
  result.checked_groups.package = false;
  result.checked_groups.crate = false;

  std::vector<Action> work;
  std::unordered_set<const Layer *> active_layers;
  work.push_back(Action{ActionKind::ValidateLayer, &layer, nullptr,
                        is_root_layer, std::string()});

  while (!work.empty()) {
    Action action = std::move(work.back());
    work.pop_back();

    if (action.kind == ActionKind::LeaveLayer) {
      active_layers.erase(action.layer);
      continue;
    }

    if (action.kind == ActionKind::ValidateVariantSets) {
      if (!action.sets) continue;
      // Push in reverse so the explicit LIFO work list retains the old
      // set/option DFS order: content first, then nested variant sets.
      for (auto set_it = action.sets->rbegin();
           set_it != action.sets->rend(); ++set_it) {
        for (auto variant_it = set_it->variants.rbegin();
             variant_it != set_it->variants.rend(); ++variant_it) {
          const std::string owner = action.location + "{" + set_it->name +
                                    "=" + variant_it->name + "}";
          if (!variant_it->variantSets.empty()) {
            work.push_back(Action{ActionKind::ValidateVariantSets, nullptr,
                                  &variant_it->variantSets, false, owner});
          }
          if (variant_it->content) {
            work.push_back(Action{ActionKind::ValidateLayer,
                                  variant_it->content.get(), nullptr, false,
                                  owner});
          }
        }
      }
      continue;
    }

    if (!action.layer) continue;
    if (!active_layers.insert(action.layer).second) {
      AddError(&result, "core.composition.variantCycle", action.location,
               "variant content contains a cycle to an ancestor layer");
      continue;
    }

    USDValidationResult local =
        ValidateLayerLocal(*action.layer, options, action.is_root_layer);
    for (USDValidationIssue &issue : local.issues) {
      issue.location =
          MapVariantContentLocation(action.location, issue.location);
      result.issues.push_back(std::move(issue));
    }

    // Leave only after all content reachable from this layer, allowing the
    // active set to distinguish cycles from legitimate shared layer bodies.
    work.push_back(Action{ActionKind::LeaveLayer, action.layer, nullptr, false,
                          std::string()});
    const std::vector<PrimSpec> &prims = action.layer->prims();
    for (auto prim_it = prims.rbegin(); prim_it != prims.rend(); ++prim_it) {
      const std::vector<VariantSetData> &sets = prim_it->meta().variantSets();
      if (sets.empty()) continue;
      const std::string owner = MapVariantContentLocation(
          action.location, prim_it->path().str());
      work.push_back(Action{ActionKind::ValidateVariantSets, nullptr, &sets,
                            false, owner});
    }
  }

  return result;
}

}  // namespace

USDValidationResult ValidateLayerAgainstAOUSDCore(
    const Layer &layer, const ValidationOptions &options) {
  return ValidateLayerImpl(layer, options, /* is_root_layer */ true);
}

bool ValidateUSDFromMemoryAgainstAOUSDCore(
    const uint8_t *data, size_t size, const std::string &filename,
    const ValidationOptions &options, USDValidationResult *result,
    std::string *warn, std::string *err) {
  if (!result) {
    if (err) {
      (*err) += "null pointer for `result` argument.\n";
    }
    return false;
  }

  std::shared_ptr<Layer> layer =
      pcp::LoadLayerFromMemory(filename, data, size, warn, err,
                               pcp::LayerLoadOptions());
  if (!layer) {
    if (err && err->empty()) {
      (*err) += "Failed to parse USD data.\n";
    }
    return false;
  }

  *result = ValidateLayerAgainstAOUSDCore(*layer, options);
  return true;
}

}  // namespace next
}  // namespace lightusd
