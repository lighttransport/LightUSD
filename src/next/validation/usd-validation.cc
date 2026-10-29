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
//  - Invalid list-edit qualifiers: next ArcListOp qualifiers are typed enums;
//    an invalid qualifier is rejected at parse time and cannot be
//    represented.
//  - layerRelocates live per-prim in next (PrimMetadata::relocates); they are
//    validated at the prim location under the legacy core.layer.layerRelocates
//    rule ids.

#include "usd-validation-internal.hh"
#include "validation-context.hh"

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
  result->AddIssue({severity, rule_id, location, message});
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
  auto tok = v->as_token();
  if (!tok) return false;
  *out = *tok;
  return true;
}

bool GetStringProperty(const PrimSpec &ps, const std::string &name,
                       std::string *out) {
  if (!out) return false;
  const Value *v = GetAttrValue(ps, name);
  if (!v) return false;
  auto str = v->as_string();
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
  auto asset = v->as_asset_path();
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
  auto arr = v->as_int_array();
  if (!arr) return false;
  out->assign(arr->begin(), arr->end());
  return true;
}

// Scalar float[] / double[] arrays converted to double.
bool ValueToDoubleScalarArray(const Value &v, std::vector<double> *out) {
  if (!out || !v.is_array()) return false;
  if (v.type_id() == TypeId::Double || v.type_id() == TypeId::TimeCode) {
    auto arr = v.as_double_array();
    if (!arr) return false;
    out->assign(arr->begin(), arr->end());
    return true;
  }
  if (v.type_id() == TypeId::Float) {
    auto arr = v.as_float_array();
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
    auto arr = v->as_float_array();
    if (!arr) return false;
    out->assign(arr->begin(), arr->end());
    return true;
  }
  if (v->type_id() == TypeId::Double) {
    auto arr = v->as_double_array();
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
  auto arr = v->as_token_array();
  if (!arr) return false;
  out->assign(arr->begin(), arr->end());
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
    auto flat = v->as_float_array();
    if (!flat) return false;
    for (size_t i = 0; i + 2 < flat->size(); i += 3) {
      out->push_back({(*flat)[i], (*flat)[i + 1], (*flat)[i + 2]});
    }
    return true;
  }
  if (v->is_array() && IsDoubleTriple(v->type_id())) {
    auto flat = v->as_double_array();
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

// A layer whose targets are all local: no sublayers and no external
// reference/payload asset arcs. Only then can missing targets be reported.
bool IsSelfContainedLayer(const Layer &layer) {
  if (!layer.meta().subLayers.empty()) {
    return false;
  }
  for (const PrimSpec &ps : layer.prims()) {
    const PrimMetadata &m = ps.meta();
    const ArcListOps *edits = m.arc_edits();
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
      ValidateVolume(ps, prim_location, prim_types, result);
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
    ValidatePrimMetadata(ps, prim_location, applied_schemas, result,
        options.registry ? *options.registry : GetBuiltinValidationRegistry());
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
      } else if (!(options.registry ? *options.registry : GetBuiltinValidationRegistry()).FindShader(shader_id)) {
        AddWarning(result, "shade.shader.id",
                   MakePropertyLocation(prim_location, "info:id"),
                   "shader id `" + shader_id +
                       "` is not in the active shader registry");
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
        if (source_type != "osl" && source_type != "glslfx" &&
            !(options.registry ? *options.registry : GetBuiltinValidationRegistry()).FindShader(shader_id, source_type)) {
          AddWarning(result, "shade.shader.id",
                     MakePropertyLocation(prim_location, prop_name),
                     "shader source type `" + source_type +
                         "` is not in the active shader registry");
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
                 std::find(ps.meta().apiSchemas().begin(), ps.meta().apiSchemas().end(), "MaterialBindingAPI") == ps.meta().apiSchemas().end()) {
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

void USDValidationResult::AddIssue(USDValidationIssue issue) {
  size_t bytes = sizeof(USDValidationIssue) * 2;
  for (const auto* text : {&issue.rule_id, &issue.location, &issue.message,
                            &issue.source_asset, &issue.variants}) {
    if (text->size() > (SIZE_MAX - bytes) / 2) { bytes = SIZE_MAX; break; }
    bytes += text->size() * 2;
  }
  if (issues.size() >= checked_groups.max_issues ||
      bytes > checked_groups.max_report_bytes - std::min(report_bytes, checked_groups.max_report_bytes)) {
    if (omitted_issues != SIZE_MAX) ++omitted_issues;
    complete = false;
    return;
  }
  report_bytes += bytes;
  issues.push_back(std::move(issue));
}

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

bool USDValidationResult::ok() const { return complete && error_count() == 0; }

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
  if (!dst || dst == &src) {
    return;
  }
  dst->complete = dst->complete && src.complete;
  for (const auto& issue : src.issues) dst->AddIssue(issue);
  dst->omitted_issues += std::min(src.omitted_issues, SIZE_MAX - dst->omitted_issues);
  dst->checked_groups.render = dst->checked_groups.render || src.checked_groups.render;
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

  ValidateRegisteredProperties(layer, options, &result);
  ValidateRegisteredStage(layer, options, &result);
  // Declared shader outputs come from the supplied definition even when the
  // instance does not author an outputs:* attribute explicitly.
  result.issues.erase(std::remove_if(result.issues.begin(), result.issues.end(),
      [&](const USDValidationIssue& issue) {
        if (issue.rule_id != "shade.materialX.output") return false;
        const PrimSpec* prim = layer.prim_at_path(issue.location);
        if (!prim) return false;
        const auto& registry = options.registry ? *options.registry : GetBuiltinValidationRegistry();
        const auto shader = registry.FindShader(GetShaderInfoId(*prim));
        return shader && shader->port_count(true) != 0;
      }), result.issues.end());

  if (options.normative_only) {
    result.issues.erase(std::remove_if(result.issues.begin(), result.issues.end(),
        [](const USDValidationIssue& issue) {
          const auto category = GetValidationRuleMetadata(issue.rule_id).category;
          return category != "normative" && category != "coverage";
        }), result.issues.end());
  }
  if (options.run_callbacks) {
    ValidationContext context;
    context.layer = &layer;
    context.options = options;
    const auto& registry = options.registry ? *options.registry : GetBuiltinValidationRegistry();
    registry.RunCallbacks(ValidationScope::Layer, context, nullptr, &result);
    for (const auto& prim : layer.prims())
      registry.RunCallbacks(ValidationScope::Prim, context, &prim, &result);
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
    const std::vector<PrimVariantSetData> *sets{nullptr};
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
    result.complete = result.complete && local.complete;
    for (USDValidationIssue &issue : local.issues) {
      issue.location =
          MapVariantContentLocation(action.location, issue.location);
      result.AddIssue(std::move(issue));
    }
    result.omitted_issues += std::min(local.omitted_issues, SIZE_MAX - result.omitted_issues);

    // Leave only after all content reachable from this layer, allowing the
    // active set to distinguish cycles from legitimate shared layer bodies.
    work.push_back(Action{ActionKind::LeaveLayer, action.layer, nullptr, false,
                          std::string()});
    const std::vector<PrimSpec> &prims = action.layer->prims();
    for (auto prim_it = prims.rbegin(); prim_it != prims.rend(); ++prim_it) {
      const std::vector<PrimVariantSetData> &sets = prim_it->meta().variantSets();
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
