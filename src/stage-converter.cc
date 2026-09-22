// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Stage to Crate Conversion Implementation
//
// This file contains the implementation for converting LightUSD Stage
// to Crate format specs.
//

#include "crate-writer.hh"
#include "sconv-detail.hh"
#include "array-edit.hh"  // value::ArrayEdit (VtArrayEdit)
#include <iostream>
#include <sstream>
#include <set>
#include <unordered_set>
#include "layer.hh"
#include "common-macros.inc"

#include <cstdlib>
#include <iostream>
#include <atomic>
#include <functional>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif
#include "pprinter.hh"  // For to_string(Specifier), to_string(Variability)
#include "pprint-enum.hh"  // For to_string(APISchemas::APIName)
#include "value-pprint.hh"  // For string payloads of unregistered Crate values
#include "usdShade.hh"  // For Material and Shader
#include "usdMtlx.hh"  // For MtlxOpenPBRSurface (concrete ShaderNode subtype)
#include "usdPhysics.hh"  // For PhysicsScene, PhysicsJoint, PhysicsCollisionGroup
#include "common-macros.inc"

// Disable specific clang warnings for this file
// - unused-parameter: functions have consistent signatures for API purposes
// - covered-switch-default: default cases provide fallback safety
// - switch-enum: some enum values are intentionally not handled
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

namespace lightusd {
namespace experimental {

// Per-thread spec sink for the parallel stage->specs conversion (see
// crate-writer.hh). Function-local thread_local so the private nested
// SpecData type stays private.
std::vector<CrateWriter::SpecData>*& CrateWriter::tls_spec_sink() {
  static thread_local std::vector<SpecData>* s_sink = nullptr;
  return s_sink;
}


namespace {

// Shader::value stores a CONCRETE ShaderNode subtype (UsdPreviewSurface,
// UsdUVTexture, UsdPrimvarReader_*, ...). value::Value::as<T> matches the exact
// type id, so as<ShaderNode>() (the base) does NOT match a concrete node.
// Dispatch each concrete type to reach its inherited UsdShadePrim::props — the
// map where reconstruction (ADD_PROPERTY) stashes authored inputs that aren't
// modeled as typed schema fields (e.g. Unreal's inputs:anisotropy / scalar
// inputs:specular / inputs:tangent on UsdPreviewSurface). Without this, those
// authored inputs are silently dropped on stage->USDC writes. Mirrors the
// concrete-type dispatch in pprint-shader.cc.
const std::map<std::string, Property> *GetShaderNodeProps(
    const value::Value &v) {
#define GET_SHADER_NODE_PROPS(__ty) \
  if (auto *p = v.as<__ty>()) {     \
    return &(p->props);             \
  }
  GET_SHADER_NODE_PROPS(UsdPreviewSurface)
  GET_SHADER_NODE_PROPS(UsdUVTexture)
  GET_SHADER_NODE_PROPS(UsdTransform2d)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_float)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_float2)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_float3)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_float4)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_int)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_string)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_normal)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_vector)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_point)
  GET_SHADER_NODE_PROPS(UsdPrimvarReader_matrix)
  GET_SHADER_NODE_PROPS(OpenPBRSurface)
  GET_SHADER_NODE_PROPS(MtlxOpenPBRSurface)
  GET_SHADER_NODE_PROPS(ShaderNode)  // generic fallback (info:id-only shaders)
#undef GET_SHADER_NODE_PROPS
  return nullptr;
}

const std::map<std::string, Property> *GetPrimProps(const value::Value &v) {
#define GET_PRIM_PROPS(__ty)        \
  if (v.as<__ty>()) {               \
    return &(v.as<__ty>()->props);  \
  }

  GET_PRIM_PROPS(Model)
  GET_PRIM_PROPS(Scope)
  GET_PRIM_PROPS(Volume)
  GET_PRIM_PROPS(OpenVDBAsset)
  GET_PRIM_PROPS(Field3DAsset)
  GET_PRIM_PROPS(RenderSettings)
  GET_PRIM_PROPS(RenderProduct)
  GET_PRIM_PROPS(RenderVar)
  GET_PRIM_PROPS(GenerativeProcedural)
  GET_PRIM_PROPS(Xform)
  GET_PRIM_PROPS(GPrim)
  GET_PRIM_PROPS(GeomMesh)
  GET_PRIM_PROPS(Volume)
  GET_PRIM_PROPS(FieldAsset)
  GET_PRIM_PROPS(OpenVDBAsset)
  GET_PRIM_PROPS(Field3DAsset)
  GET_PRIM_PROPS(GeomPoints)
  GET_PRIM_PROPS(GeomCube)
  GET_PRIM_PROPS(GeomCapsule)
  GET_PRIM_PROPS(GeomCylinder)
  GET_PRIM_PROPS(GeomSphere)
  GET_PRIM_PROPS(GeomCone)
  GET_PRIM_PROPS(GeomSubset)
  GET_PRIM_PROPS(GeomCamera)
  GET_PRIM_PROPS(GeomBasisCurves)
  GET_PRIM_PROPS(GeomNurbsCurves)
  GET_PRIM_PROPS(GeomHermiteCurves)
  GET_PRIM_PROPS(GeomPointInstancer)
  GET_PRIM_PROPS(GeomPlane)
  GET_PRIM_PROPS(GeomCylinder_1)
  GET_PRIM_PROPS(GeomCapsule_1)
  GET_PRIM_PROPS(GeomTetMesh)
  GET_PRIM_PROPS(GeomNurbsPatch)
  GET_PRIM_PROPS(DomeLight)
  GET_PRIM_PROPS(DomeLight_1)
  GET_PRIM_PROPS(SphereLight)
  GET_PRIM_PROPS(CylinderLight)
  GET_PRIM_PROPS(DiskLight)
  GET_PRIM_PROPS(DistantLight)
  GET_PRIM_PROPS(RectLight)
  GET_PRIM_PROPS(Material)
  // Shader stores generic/unknown shader inputs inside Shader::value
  // (a concrete ShaderNode subtype) rather than Shader::props. Prefer the inner
  // map when the outer one is empty so shaders keep their non-schema inputs/
  // outputs through USDC roundtrip. Note: as<ShaderNode>() does not match a
  // concrete node (exact-type id), so we dispatch each concrete subtype.
  if (auto *sh = v.as<Shader>()) {
    if (sh->props.empty()) {
      if (const auto *node_props = GetShaderNodeProps(sh->value)) {
        return node_props;
      }
    }
    return &sh->props;
  }
  GET_PRIM_PROPS(NodeGraph)
  GET_PRIM_PROPS(GeometryLight)
  GET_PRIM_PROPS(PortalLight)
  GET_PRIM_PROPS(SkelRoot)
  GET_PRIM_PROPS(Skeleton)
  GET_PRIM_PROPS(SkelAnimation)
  GET_PRIM_PROPS(BlendShape)

  // UsdPhysics
  GET_PRIM_PROPS(PhysicsScene)
  GET_PRIM_PROPS(PhysicsJoint)
  GET_PRIM_PROPS(PhysicsRevoluteJoint)
  GET_PRIM_PROPS(PhysicsPrismaticJoint)
  GET_PRIM_PROPS(PhysicsSphericalJoint)
  GET_PRIM_PROPS(PhysicsFixedJoint)
  GET_PRIM_PROPS(PhysicsDistanceJoint)
  GET_PRIM_PROPS(PhysicsCollisionGroup)
  GET_PRIM_PROPS(MjcActuator)
  GET_PRIM_PROPS(NewtonActuator)
  GET_PRIM_PROPS(MjcTendon)
  GET_PRIM_PROPS(MjcKeyframe)
  GET_PRIM_PROPS(MjcSensor)
  // UsdSkel — required so authored attributes in the props map (joints,
  // jointNames, blendShapeWeights, etc.) survive the USDC writer when the
  // typed-builtin fields are unauthored.
  GET_PRIM_PROPS(SkelRoot)
  GET_PRIM_PROPS(Skeleton)
  GET_PRIM_PROPS(SkelAnimation)
  GET_PRIM_PROPS(BlendShape)

#undef GET_PRIM_PROPS

  return nullptr;
}

template <typename Vec>
std::string FormatUintTuple(const Vec &v) {
  std::ostringstream ss;
  ss << "(";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) {
      ss << ", ";
    }
    ss << v[i];
  }
  ss << ")";
  return ss.str();
}

template <typename Vec>
std::string FormatUintTupleArray(const std::vector<Vec> &values) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) {
      ss << ", ";
    }
    ss << FormatUintTuple(values[i]);
  }
  ss << "]";
  return ss.str();
}

template <typename T>
int64_t SmallIntegerValue(T value) {
  return static_cast<int64_t>(value);
}

int64_t SmallIntegerValue(char value) {
  const uint8_t bits = static_cast<uint8_t>(static_cast<unsigned char>(value));
  return bits <= 0x7fu ? static_cast<int64_t>(bits)
                       : static_cast<int64_t>(bits) - 0x100;
}

template <typename T>
std::string FormatSmallIntegerValue(T value) {
  std::ostringstream ss;
  ss << SmallIntegerValue(value);
  return ss.str();
}

template <typename T>
std::string FormatSmallIntegerArray(const std::vector<T> &values) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) {
      ss << ", ";
    }
    ss << SmallIntegerValue(values[i]);
  }
  ss << "]";
  return ss.str();
}

template <typename T>
std::string FormatUnregisteredValue(const T &value) {
  std::ostringstream ss;
  ss << value;
  return ss.str();
}

template <typename T>
std::string FormatUnregisteredArray(const std::vector<T> &values) {
  std::ostringstream ss;
  ss << values;
  return ss.str();
}

// Returns the Collection mixin pointer for prims that inherit from
// Collection (GPrim and GeomSubset families). Used by the writer to
// re-emit `collection:<instance>:{includes,excludes,includeRoot,
// expansionRule}` properties from the typed Collection storage on
// USDC output. The reconstruct path consumes these from the props
// map into Collection::instances(); without this re-emit, USDC
// round-trip drops them entirely.
const Collection *GetPrimCollection(const value::Value &v) {
#define GET_PRIM_COLLECTION(__ty)              \
  if (auto *p = v.as<__ty>()) {                \
    return static_cast<const Collection *>(p); \
  }

  GET_PRIM_COLLECTION(Model)
  GET_PRIM_COLLECTION(Scope)
  GET_PRIM_COLLECTION(Xform)
  GET_PRIM_COLLECTION(GPrim)
  GET_PRIM_COLLECTION(GeomMesh)
  GET_PRIM_COLLECTION(Volume)
  GET_PRIM_COLLECTION(FieldAsset)
  GET_PRIM_COLLECTION(OpenVDBAsset)
  GET_PRIM_COLLECTION(Field3DAsset)
  GET_PRIM_COLLECTION(GeomPoints)
  GET_PRIM_COLLECTION(GeomCube)
  GET_PRIM_COLLECTION(GeomCapsule)
  GET_PRIM_COLLECTION(GeomCylinder)
  GET_PRIM_COLLECTION(GeomSphere)
  GET_PRIM_COLLECTION(GeomCone)
  GET_PRIM_COLLECTION(GeomSubset)
  GET_PRIM_COLLECTION(GeomBasisCurves)
  GET_PRIM_COLLECTION(GeomNurbsCurves)
  GET_PRIM_COLLECTION(GeomHermiteCurves)
  GET_PRIM_COLLECTION(GeomPointInstancer)
  GET_PRIM_COLLECTION(GeomPlane)
  GET_PRIM_COLLECTION(GeomCylinder_1)
  GET_PRIM_COLLECTION(GeomCapsule_1)
  GET_PRIM_COLLECTION(GeomTetMesh)
  GET_PRIM_COLLECTION(GeomNurbsPatch)
  // Lights inherit Collection (light:link / shadow:link collections).
  GET_PRIM_COLLECTION(SphereLight)
  GET_PRIM_COLLECTION(RectLight)
  GET_PRIM_COLLECTION(DiskLight)
  GET_PRIM_COLLECTION(CylinderLight)
  GET_PRIM_COLLECTION(DistantLight)
  GET_PRIM_COLLECTION(DomeLight)
  GET_PRIM_COLLECTION(DomeLight_1)
  GET_PRIM_COLLECTION(GeometryLight)
  GET_PRIM_COLLECTION(PortalLight)

#undef GET_PRIM_COLLECTION
  return nullptr;
}

}  // namespace

// ============================================================================
// Shared helpers to reduce per-Extract* lambda duplication
// ============================================================================

bool CrateWriter::AddArrayAttribute(
    const std::string& attr_name, const value::Value& val,
    crate::FieldValuePairVector& fields, std::string* err) {
  crate::CrateValue crate_val;
  if (!ConvertValue(val, crate_val, err)) {
    return false;
  }
  fields.push_back({attr_name, crate_val});
  // ConvertValue degrades role types to their underlying storage (point3f[]
  // becomes a float3[] crate value -- the crate has no role wire types), and
  // the spec assembly then infers the typeName from the VALUE. Emit the
  // original spelling as an explicit `.typeName` field (routed into the spec
  // by ConvertSinglePrim) so `point3f[] points` does not come back -- and is
  // not shown to OpenUSD -- as `float3[] points`.
  if (val.type_name() != crate_val.type_name()) {
    crate::CrateValue ty;
    ty.Set(value::token(val.type_name()));
    fields.push_back({attr_name + ".typeName", ty});
  }
  return true;
}

bool CrateWriter::AddArrayAttributeWithMetas(
    const std::string& attr_name, const value::Value& val,
    const AttrMeta& metas, const Path& prim_path, std::string* err) {
  // Build a transient Attribute carrying both the value and the typed
  // attribute's authored metadata, then emit a proper property spec.
  Attribute attr;
  attr.set_type_name(val.type_name());
  primvar::PrimVar pv;
  pv.set_value(val);
  attr.set_var(std::move(pv));
  attr.metas() = metas;
  return ConvertAttributeToFields(attr_name, attr, prim_path,
                                  /*is_custom=*/false, err);
}

void CrateWriter::AddEnumAttribute(
    const std::string& attr_name, const std::string& enum_val,
    crate::FieldValuePairVector& fields) {
  crate::CrateValue crate_val;
  value::token tok(enum_val);
  crate_val.Set(tok);
  fields.push_back({attr_name, crate_val});
}


// Template definitions for AddTypedArrayAttribute and ExtractAnimatableDefault
// are in sconv-detail.hh (needed by split files).

// ============================================================================
// Main Conversion Entry Point
// ============================================================================

bool CrateWriter::ConvertStageToSpecs(const Stage& stage, std::string* err) {
  // Add root spec first with layer metadata
  Path root_path("/", "");
  crate::FieldValuePairVector root_fields;

  // Extract layer metadata from Stage
  const StageMetas& metas = stage.metas();

  // Add timeCodesPerSecond
  if (metas.timeCodesPerSecond.authored()) {
    crate::CrateValue tcps_value;
    tcps_value.Set(metas.timeCodesPerSecond.get_value());
    root_fields.push_back({"timeCodesPerSecond", tcps_value});
  }

  // Add framesPerSecond
  if (metas.framesPerSecond.authored()) {
    crate::CrateValue fps_value;
    fps_value.Set(metas.framesPerSecond.get_value());
    root_fields.push_back({"framesPerSecond", fps_value});
  }

  // Add startTimeCode
  if (metas.startTimeCode.authored()) {
    crate::CrateValue start_value;
    start_value.Set(metas.startTimeCode.get_value());
    root_fields.push_back({"startTimeCode", start_value});
  }

  // Add endTimeCode
  if (metas.endTimeCode.authored()) {
    crate::CrateValue end_value;
    end_value.Set(metas.endTimeCode.get_value());
    root_fields.push_back({"endTimeCode", end_value});
  }

  // Add upAxis
  if (metas.upAxis.authored()) {
    crate::CrateValue axis_value;
    // Convert Axis enum to token - must be actual token, not TokenIndex
    std::string axis_str = to_string(metas.upAxis.get_value());
    value::token axis_tok(axis_str);
    axis_value.Set(axis_tok);
    root_fields.push_back({"upAxis", axis_value});
  }

  // Add metersPerUnit
  if (metas.metersPerUnit.authored()) {
    crate::CrateValue mpu_value;
    mpu_value.Set(metas.metersPerUnit.get_value());
    root_fields.push_back({"metersPerUnit", mpu_value});
  }

  // Add defaultPrim
  if (!metas.defaultPrim.str().empty()) {
    crate::CrateValue dp_value;
    // defaultPrim must be actual token, not TokenIndex
    value::token dp_tok(metas.defaultPrim.str());
    dp_value.Set(dp_tok);
    root_fields.push_back({"defaultPrim", dp_value});
  }

  // Add doc (documentation)
  if (!metas.doc.value.empty()) {
    crate::CrateValue doc_value;
    doc_value.Set(metas.doc.value);
    root_fields.push_back({"documentation", doc_value});
  }

  // Add comment (string)
  if (!metas.comment.value.empty()) {
    crate::CrateValue comment_value;
    comment_value.Set(metas.comment.value);
    root_fields.push_back({"comment", comment_value});
  }

  if (metas.colorConfiguration) {
    crate::CrateValue value;
    value.Set(metas.colorConfiguration.value());
    root_fields.push_back({"colorConfiguration", value});
  }

  if (metas.colorManagementSystem) {
    crate::CrateValue value;
    value.Set(metas.colorManagementSystem.value());
    root_fields.push_back({"colorManagementSystem", value});
  }

  if (metas.renderSettingsPrimPath) {
    crate::CrateValue value;
    value.Set(metas.renderSettingsPrimPath.value());
    root_fields.push_back({"renderSettingsPrimPath", value});
  }

  // Add customLayerData. An AUTHORED-but-empty `customLayerData = {}` is an
  // opinion too, so consult customLayerDataAuthored rather than just !empty().
  if (metas.customLayerDataAuthored || !metas.customLayerData.empty()) {
    crate::CrateValue cld_value;
    cld_value.Set(metas.customLayerData);
    root_fields.push_back({"customLayerData", cld_value});
  }

  // Unregistered layer metadata: emit as the crate UNREGISTERED_VALUE type
  // (what pxr writes for SdfUnregisteredValue), carrying the raw USDA text.
  for (const auto &kv : metas.unregisteredMetas) {
    crate::CrateValue v;
    v.SetUnregisteredValueString(kv.second);
    root_fields.push_back({kv.first, v});
  }
  // kilogramsPerUnit (UsdPhysics) and the two USDZ playback metas were written by
  // the Layer path (sconv-layer.cc) but not by this one, so they vanished on a
  // Stage write. (The timecode family above is already handled -- do not add it
  // again: a duplicate root field corrupts the fieldset encoding and the crate
  // then fails to read back at all.)
  if (metas.kilogramsPerUnit.authored()) {
    crate::CrateValue v;
    v.Set(metas.kilogramsPerUnit.get_value());
    root_fields.push_back({"kilogramsPerUnit", v});
  }

  if (metas.autoPlay.authored()) {
    crate::CrateValue v;
    v.Set(metas.autoPlay.get_value());
    root_fields.push_back({"autoPlay", v});
  }

  if (metas.playbackMode.authored()) {
    crate::CrateValue v;
    v.Set(value::token(
        metas.playbackMode.get_value() == LayerMetas::PlaybackMode::PlaybackModeLoop
            ? "loop"
            : "none"));
    root_fields.push_back({"playbackMode", v});
  }

  // Add subLayers
  if (!metas.subLayers.empty()) {
    // Convert SubLayer array to string array (asset paths)
    std::vector<std::string> sublayer_paths;
    std::vector<LayerOffset> sublayer_offsets;
    sublayer_paths.reserve(metas.subLayers.size());
    sublayer_offsets.reserve(metas.subLayers.size());

    bool has_non_default_offsets = false;

    for (const auto& sublayer : metas.subLayers) {
      // Extract asset path as string
      std::string path_str = sublayer.assetPath.GetAssetPath();
      sublayer_paths.push_back(path_str);

      // Collect layer offset
      sublayer_offsets.push_back(sublayer.layerOffset);

      // Check if any offset is non-default
      if (sublayer.layerOffset._offset != 0.0 || sublayer.layerOffset._scale != 1.0) {
        has_non_default_offsets = true;
      }
    }

    // Serialize subLayers as string array
    crate::CrateValue sublayers_value;
    sublayers_value.Set(sublayer_paths);
    root_fields.push_back({"subLayers", sublayers_value});

    // Serialize subLayerOffsets UNCONDITIONALLY: pxr's SdfLayer requires the
    // offsets vector to be the same length as subLayers (GetSubLayerOffset
    // raises "Invalid sublayer index" otherwise -- the whole file becomes
    // unreadable). pxr itself always writes identity offsets.
    (void)has_non_default_offsets;
    {
      crate::CrateValue offsets_value;
      offsets_value.Set(sublayer_offsets);
      root_fields.push_back({"subLayerOffsets", offsets_value});
    }
  }

  // Add layer-level relocates (AOUSD Core Spec 10.3.2.6). Stored as the crate
  // SdfRelocates value type (58); the writer bumps the crate version to 0.11.0.
  if (!metas.layerRelocates.empty()) {
    crate::CrateValue relocates_value;
    relocates_value.Set(metas.layerRelocates);
    root_fields.push_back({"relocates", relocates_value});
  }

  // Add primChildren listing root prim names
  {
    std::vector<value::token> root_children;
    for (const auto& prim : stage.root_prims()) {
      root_children.push_back(value::token(prim.element_name()));
    }
    if (!root_children.empty()) {
      crate::CrateValue pc_value;
      pc_value.Set(root_children);
      root_fields.push_back({"primChildren", pc_value});
    }
  }

  if (!AddSpec(root_path, SpecType::PseudoRoot, std::move(root_fields), err)) {
    if (err) *err = "Failed to add root spec: " + *err;
    return false;
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  // Large stages: convert prim subtrees on worker threads (byte-identical to
  // the serial DFS — specs assembled in DFS order, field-name tokens replayed
  // serially afterwards). Small stages stay serial (thread setup dominates).
  {
    // Sample three levels — deep single-root scenes (/Root/Group/Actor...)
    // have all their fan-out below level 1.
    size_t approx_prims = 0;
    for (const auto& prim : stage.root_prims()) {
      approx_prims += 1;
      for (const auto& c1 : prim.children()) {
        approx_prims += 1 + c1.children().size();
      }
    }
    if ((std::thread::hardware_concurrency() > 1) && (approx_prims > 64)) {
      return ConvertRootPrimsParallel(stage, err);
    }
  }
#endif

  // Convert all root prims
  for (const auto& prim : stage.root_prims()) {
    Path parent_path("/", "");
    if (!ConvertPrimIterative(prim, parent_path, err)) {
      if (err) *err = "Failed to convert prim: " + *err;
      return false;
    }
  }

  return true;
}

#if defined(LIGHTUSD_ENABLE_THREAD)
bool CrateWriter::ConvertRootPrimsParallel(const Stage& stage,
                                           std::string* err) {
  // Work unit: either a single prim's own specs (shell — its children are
  // separate units, assembled after it in child order) or a whole subtree
  // (converted with the normal DFS ConvertPrimIterative). Shells exist so
  // wide-but-shallow-rooted scenes (e.g. everything under one /Root) still
  // fan out.
  struct ConvUnit {
    const Prim* prim = nullptr;
    Path parent_path;
    bool whole_subtree = true;
    uint32_t depth = 0;
    std::vector<SpecData> specs;
    std::vector<size_t> children;  // unit indices, in child order (shells only)
    std::string unit_err;
    bool ok = true;
  };

  std::vector<ConvUnit> units;
  units.reserve(1024);

  // Split shells down to this depth; subtrees below become parallel jobs.
  // Depth 2 fans out single-root scenes (root shell -> level-1 shells ->
  // level-2 subtree jobs) without creating one unit per prim.
  constexpr uint32_t kShellDepth = 3;

  std::vector<size_t> roots;
  std::function<size_t(const Prim&, const Path&, uint32_t)> plan =
      [&](const Prim& prim, const Path& parent_path, uint32_t depth) -> size_t {
    const size_t idx = units.size();
    units.push_back(ConvUnit());
    ConvUnit& u = units.back();
    u.prim = &prim;
    u.parent_path = parent_path;
    u.depth = depth;
    if (depth < kShellDepth && !prim.children().empty()) {
      u.whole_subtree = false;
      // Build this prim's absolute path for its children (mirrors
      // ConvertPrimIterative).
      std::string parent_str = parent_path.prim_part();
      std::string abs_path_str;
      if (parent_str == "/") {
        abs_path_str = "/" + prim.element_name();
      } else {
        abs_path_str = parent_str + "/" + prim.element_name();
      }
      Path prim_path(abs_path_str, "");
      std::vector<size_t> child_ids;
      child_ids.reserve(prim.children().size());
      for (const auto& child : prim.children()) {
        child_ids.push_back(plan(child, prim_path, depth + 1));
      }
      units[idx].children = std::move(child_ids);
    }
    return idx;
  };

  {
    Path root_parent("/", "");
    for (const auto& prim : stage.root_prims()) {
      roots.push_back(plan(prim, root_parent, 0));
    }
  }

  if (const char* e = ::getenv("LIGHTUSD_FLATTEN_TIMING")) {
    if (e[0] == '1') {
      std::cerr << "[lightusd] parallel stage->specs: " << units.size()
                << " units\n";
    }
  }

  // Run units on workers. Each worker redirects AddSpec into the unit's own
  // spec buffer via the per-thread sink; token pre-registration is skipped
  // in AddSpec while a sink is active.
  {
    const uint32_t hw = std::thread::hardware_concurrency();
    const uint32_t nworkers = static_cast<uint32_t>(
        (std::min)(size_t(hw > 0 ? hw : 4), units.size()));
    std::atomic<size_t> next_unit{0};
    auto worker = [&]() {
      for (;;) {
        const size_t i = next_unit.fetch_add(1);
        if (i >= units.size()) break;
        ConvUnit& u = units[i];
        tls_spec_sink() = &u.specs;
        if (u.whole_subtree) {
          u.ok = ConvertPrimIterative(*u.prim, u.parent_path, &u.unit_err);
        } else {
          if (u.depth > kMaxPrimNestingDepth) {
            u.unit_err = "Prim nesting too deep";
            u.ok = false;
          } else {
            u.ok = ConvertSinglePrim(*u.prim, u.parent_path, &u.unit_err);
          }
        }
        tls_spec_sink() = nullptr;
      }
    };
    std::vector<std::thread> workers;
    workers.reserve(nworkers);
    for (uint32_t t = 1; t < nworkers; t++) {
      workers.emplace_back(worker);
    }
    worker();
    for (auto& th : workers) th.join();
  }

  // Assemble in DFS order (unit specs, then child units) — identical spec
  // order to the serial ConvertPrimIterative walk.
  std::function<bool(size_t)> assemble = [&](size_t idx) -> bool {
    ConvUnit& u = units[idx];
    if (!u.ok) {
      if (err) *err = "Failed to convert prim: " + u.unit_err;
      return false;
    }
    for (auto& sd : u.specs) {
      spec_data_.push_back(std::move(sd));
    }
    u.specs.clear();
    u.specs.shrink_to_fit();
    for (size_t c : u.children) {
      if (!assemble(c)) return false;
    }
    return true;
  };
  const size_t replay_begin = spec_data_.size();
  for (size_t r : roots) {
    if (!assemble(r)) return false;
  }

  // Serial token replay in DFS spec order: reproduces the serial writer's
  // first-seen token numbering exactly. Only the AddSpec-time field prefix —
  // post-appended fields (`properties`) were never pre-registered serially.
  for (size_t i = replay_begin; i < spec_data_.size(); i++) {
    const auto& sd = spec_data_[i];
    const size_t n = (std::min)(size_t(sd.fields_at_addspec), sd.fields.size());
    for (size_t f = 0; f < n; f++) {
      GetOrCreateToken(sd.fields[f].first);
    }
  }

  return true;
}
#endif  // LIGHTUSD_ENABLE_THREAD

// ============================================================================
// Per-Prim Conversion (no recursion into children)
// ============================================================================

bool CrateWriter::ConvertSinglePrim(
  const Prim& prim,
  const Path& parent_path,
  std::string* err
) {
  // Build absolute path for this prim
  std::string prim_name = prim.element_name();
  std::string parent_str = parent_path.prim_part();
  std::string abs_path_str;

  if (parent_str == "/") {
    abs_path_str = "/" + prim_name;
  } else {
    abs_path_str = parent_str + "/" + prim_name;
  }

  Path prim_path(abs_path_str, "");
  // prim_type_name() carries the parser-set / user-set string, but is empty for
  // in-memory authored prims (e.g. those built by the tydra converters). Fall
  // back to the typed schema name — mirrors ExtractPrimProperties — so the
  // type-specific spec emitters below (Material outputs, Shader inputs) run for
  // programmatically-built prims too. Without this, an authored UsdShade
  // network round-trips through USDA but loses every shader input/output on
  // USDC/USDZ write (only `info:id` survived via the generic prop path).
  // Exception: a genuinely typeless prim (`def "bora"`) is held internally as
  // the catch-all `Model` struct, whose type_name() unconditionally returns
  // "Model" -- an internal label, not an authored or inferable schema name.
  // Falling back to it here resurrects a typeName the prim never had, and
  // `def "bora"` came back from usdc as `def Model "bora"`.
  //
  // Do NOT drop the fallback itself: in-memory authored prims (e.g. those built
  // by the tydra converters) have an empty prim_type_name() but a REAL schema
  // behind them, and without it the writer skips both the `typeName` field and
  // the type-specific property extraction -- losing every shader input/output.
  std::string type_name = prim.prim_type_name();
  if (type_name.empty() && prim.type_name() != "Model") {
    type_name = prim.type_name();
  }

  // All specs this prim contributes (its Prim spec + Attribute/Relationship/
  // Connection specs) are appended to spec_data_ within this call. Record the
  // start index so the "properties"-field pass below scans only this prim's
  // specs instead of all accumulated specs — the prior all-specs scan made
  // ConvertSinglePrim O(prims x total_specs), which dominated write time for
  // spec-dense scenes (e.g. a scene with ~5000 shaders -> ~57 s write).
  std::vector<SpecData>& out_specs = active_spec_buffer();
  const size_t my_specs_begin = out_specs.size();

  DCOUT("ConvertSinglePrim: name=" << prim_name << " abs=" << abs_path_str << " type=" << type_name);

  // Extract properties from this prim
  crate::FieldValuePairVector fields;

  if (!ExtractPrimProperties(prim, prim_path, fields, err)) {
    if (err) *err = "Failed to extract properties for " + abs_path_str + ": " + *err;
    return false;
  }

  // Split fields: prim-level fields (specifier, typeName) stay on the prim
  // spec; property fields become separate Attribute specs.
  crate::FieldValuePairVector prim_fields;
  // Collect property fields: name -> {default_value, timeSamples_value, ...}
  // `extra_metas` covers AttrMeta keys beyond .interpolation: elementSize,
  // customData, displayName, displayGroup, documentation, hidden,
  // colorSpace, comment, weight, allowedTokens. Pairs are emitted into
  // the attribute spec as-is.
  struct PropEntry {
    std::string name;
    crate::CrateValue default_val;
    bool has_default{false};
    crate::CrateValue ts_val;
    bool has_ts{false};
    crate::CrateValue variability_val;
    bool has_variability{false};
    crate::CrateValue interpolation_val;
    bool has_interpolation{false};
    std::vector<std::pair<std::string, crate::CrateValue>> extra_metas;
    crate::CrateValue spline_val;
    bool has_spline{false};
    crate::CrateValue conn_val;
    bool has_conn{false};
    std::string decl_type_name;  // set for a declaration-only attribute
  };
  std::vector<PropEntry> prop_entries;

  // AttrMeta suffixes other than `.interpolation` that schema-typed
  // extractors may emit as `<base>.<key>`. The router stuffs the
  // matching value into the prop's attribute spec under field name `key`.
  static const std::set<std::string> kAttrMetaSuffixes = {
    "elementSize", "customData", "displayName", "displayGroup",
    "documentation", "doc", "hidden", "colorSpace", "comment", "weight",
    "allowedTokens",
  };

  // Set of field names that belong on the prim spec, not as separate attributes
  static const std::set<std::string> kPrimFields = {
    "specifier", "typeName", "primChildren", "properties",
    "variantSetNames", "variantSelection", "kind",
    "active", "hidden", "documentation", "comment", "customData",
    "assetInfo", "instanceable", "clips",
    "apiSchemas", "inherits", "inheritPaths", "specializes", "references", "payload",
    "displayName", "displayGroup", "sceneName",
    // `reorder nameChildren` / `reorder properties`. Anything NOT named here is
    // re-routed into an attribute spec below, which is exactly what happened to
    // these two: they came back as `token[] primOrder = [...]` properties.
    "primOrder", "propertyOrder",
  };
  std::set<std::string> prim_field_names = kPrimFields;
  for (const auto& kv : prim.metas().data()) {
    prim_field_names.insert(kv.first);
  }
  // ...and the ASCII-reader-only spelling of the same thing. Without this, an
  // unregistered prim meta is re-routed into an ATTRIBUTE spec instead of staying
  // prim metadata (the same trap `reorder` fell into).
  for (const auto& kv : prim.metas().unregisteredMetas) {
    prim_field_names.insert(kv.first);
  }

  for (auto& fv : fields) {
    if (prim_field_names.count(fv.first)) {
      prim_fields.push_back(std::move(fv));
      continue;
    }

    // Check for ".timeSamples" / ".interpolation" / other AttrMeta suffixes
    std::string base_name = fv.first;
    bool is_ts = false;
    bool is_interp = false;
    bool is_spline = false;
    bool is_conn = false;
    bool is_decl_type = false;
    std::string meta_key;  // non-empty when fv.first is `<base>.<meta_key>`
    const std::string ts_suffix = ".timeSamples";
    const std::string interp_suffix = ".interpolation";
    const std::string spline_suffix = ".spline";
    const std::string conn_suffix = ".connect";
    const std::string decl_suffix = ".typeName";
    if (base_name.size() > conn_suffix.size() &&
        base_name.compare(base_name.size() - conn_suffix.size(),
                          conn_suffix.size(), conn_suffix) == 0) {
      base_name = base_name.substr(0, base_name.size() - conn_suffix.size());
      is_conn = true;
    } else if (base_name.size() > decl_suffix.size() &&
               base_name.compare(base_name.size() - decl_suffix.size(),
                                 decl_suffix.size(), decl_suffix) == 0) {
      // A declaration-only attribute: the spec carries a typeName and NO
      // default, so the type cannot be inferred from a value.
      base_name = base_name.substr(0, base_name.size() - decl_suffix.size());
      is_decl_type = true;
    } else if (base_name.size() > ts_suffix.size() &&
        base_name.compare(base_name.size() - ts_suffix.size(),
                          ts_suffix.size(), ts_suffix) == 0) {
      base_name = base_name.substr(0, base_name.size() - ts_suffix.size());
      is_ts = true;
    } else if (base_name.size() > spline_suffix.size() &&
               base_name.compare(base_name.size() - spline_suffix.size(),
                                 spline_suffix.size(), spline_suffix) == 0) {
      base_name = base_name.substr(0, base_name.size() - spline_suffix.size());
      is_spline = true;
    } else if (base_name.size() > interp_suffix.size() &&
               base_name.compare(base_name.size() - interp_suffix.size(),
                                 interp_suffix.size(), interp_suffix) == 0) {
      base_name = base_name.substr(0, base_name.size() - interp_suffix.size());
      is_interp = true;
    } else {
      // Generic `<base>.<meta_key>` suffix (elementSize, customData, ...).
      // Only consume when the suffix matches a known AttrMeta key, to
      // avoid swallowing unrelated dot-bearing names.
      auto dot = base_name.rfind('.');
      if (dot != std::string::npos && dot + 1 < base_name.size()) {
        std::string suffix = base_name.substr(dot + 1);
        // An UNREGISTERED-marked value is by construction a `<base>.<key>`
        // property-metadata field (see sconv_detail::EmitAttrMetas) -- the
        // key cannot be in kAttrMetaSuffixes because it is unknown by
        // definition, so route it by the marker instead.
        if (kAttrMetaSuffixes.count(suffix) ||
            fv.second.IsUnregisteredValue()) {
          meta_key = std::move(suffix);
          base_name = base_name.substr(0, dot);
        }
      }
    }

    // Check for "variability" field
    bool is_variability = (fv.first == "variability");
    if (is_variability) {
      // variability belongs on the prim spec
      prim_fields.push_back(std::move(fv));
      continue;
    }

    // Find or create prop entry
    PropEntry* entry = nullptr;
    for (auto& pe : prop_entries) {
      if (pe.name == base_name) {
        entry = &pe;
        break;
      }
    }
    if (!entry) {
      PropEntry new_entry;
      new_entry.name = base_name;
      prop_entries.push_back(std::move(new_entry));
      entry = &prop_entries.back();
    }

    if (is_conn) {
      entry->conn_val = std::move(fv.second);
      entry->has_conn = true;
    } else if (is_decl_type) {
      if (auto tok = fv.second.get_value<value::token>()) {
        entry->decl_type_name = tok.value().str();
      }
    } else if (is_ts) {
      entry->ts_val = std::move(fv.second);
      entry->has_ts = true;
    } else if (is_spline) {
      entry->spline_val = std::move(fv.second);
      entry->has_spline = true;
    } else if (is_interp) {
      entry->interpolation_val = std::move(fv.second);
      entry->has_interpolation = true;
    } else if (!meta_key.empty()) {
      // documentation alias: `doc` -> `documentation` for the spec
      std::string spec_key = (meta_key == "doc") ? "documentation" : meta_key;
      entry->extra_metas.emplace_back(std::move(spec_key),
                                      std::move(fv.second));
    } else {
      entry->default_val = std::move(fv.second);
      entry->has_default = true;
    }
  }

  // Pre-collect props map pointer for later processing
  const auto* props_map = GetPrimProps(prim.data());

  // Add "primChildren" field (ordered list of child prim names)
  if (!prim.children().empty()) {
    std::vector<value::token> child_tokens;
    for (const auto& child : prim.children()) {
      child_tokens.push_back(value::token(child.element_name()));
    }
    crate::CrateValue children_value;
    children_value.Set(child_tokens);
    prim_fields.push_back({"primChildren", children_value});
  }

  // Add "variantSetChildren": the names of the variantSets that have specs
  // beneath this prim. This is the HIERARCHY field -- pxr's reader walks from
  // the prim to its {set=} VariantSet specs through it (variantSetNames is
  // only the list-op OPINION, not the child list). Without it, OpenUSD opens
  // the file, shows the prim, and silently never visits a single variant spec.
  // Our own reader reconstructs variants from the spec PATHS, so it never
  // noticed the field was missing.
  if (!prim.variantSets().empty()) {
    std::vector<value::token> vs_tokens;
    for (const auto& vs : prim.variantSets()) {
      vs_tokens.push_back(value::token(vs.first));
    }
    crate::CrateValue vs_children_value;
    vs_children_value.Set(vs_tokens);
    prim_fields.push_back({"variantSetChildren", vs_children_value});
  }

  // Add spec for this prim (prim-level fields only; "properties" added later)
  if (!AddSpec(prim_path, SpecType::Prim, std::move(prim_fields), err)) {
    if (err) *err = "Failed to add spec for: " + abs_path_str + ": " + *err;
    return false;
  }

  // Known uniform properties (must have Variability::Uniform in their Attribute spec)
  static const std::set<std::string> kUniformProps = {
    "offsets", "normalOffsets", "pointIndices",  // BlendShape
    "subdivisionScheme", "interpolateBoundary", "faceVaryingLinearInterpolation",
    "triangleSubdivisionRule",  // Mesh
    "elementType", "familyName",  // GeomSubset
    "projection", "stereoRole",  // Camera
    "type", "basis", "wrap",  // BasisCurves
    "joints", "jointNames", "bindTransforms", "restTransforms",  // Skeleton
    "blendShapes",  // SkelAnimation
    "inactiveIds",  // PointInstancer
    "purpose", "doubleSided",  // GPrim
    "info:id",  // Shader (UsdShade: `uniform token info:id`)
  };

  // Create separate Attribute specs for each property
  for (auto& pe : prop_entries) {
    Path attr_path = prim_path.AppendProperty(pe.name);
    crate::FieldValuePairVector attr_fields;

    // Determine type name: an explicit `.typeName` field wins -- it carries
    // the AUTHORED spelling, which may be a role variant of the value's type
    // (e.g. authored `float3` stored as a color3f value). Fall back to
    // inferring from the value.
    std::string prop_type_name;
    if (!pe.decl_type_name.empty()) {
      prop_type_name = pe.decl_type_name;
    } else if (pe.has_default) {
      prop_type_name = pe.default_val.type_name();
    } else if (pe.has_ts) {
      // For timeSamples, get the element type from the first sample
      auto ts_opt = pe.ts_val.get_value<value::TimeSamples>();
      if (ts_opt && ts_opt->size() > 0) {
        prop_type_name = ts_opt->get_samples()[0].value.type_name();
      }
    }

    if (!prop_type_name.empty()) {
      crate::CrateValue type_value;
      value::token type_tok(prop_type_name);
      type_value.Set(type_tok);
      attr_fields.push_back({"typeName", type_value});
    }

    // Add variability for uniform properties -- but NOT when the attribute is
    // time-sampled. A uniform attribute cannot vary over time, and the reader
    // enforces it ("Attribute `projection` is declared `uniform`, but a
    // time-sampled value was authored") by failing the whole prim. Stamping
    // Uniform unconditionally here made an animated `projection` unreadable the
    // moment the writer started emitting its timeSamples at all.
    // `subsetFamily:<FAMILYNAME>:familyType` is uniform too, but its name is not
    // a fixed token -- the family name is authored -- so it cannot live in the
    // set above. The reader parses it with ParseUniformEnumProperty and rejects
    // a non-uniform one.
    const std::string kFamilyTypeSuffix = ":familyType";
    const bool is_family_type =
        (pe.name.compare(0, 13, "subsetFamily:") == 0) &&
        (pe.name.size() > kFamilyTypeSuffix.size()) &&
        (pe.name.compare(pe.name.size() - kFamilyTypeSuffix.size(),
                         kFamilyTypeSuffix.size(), kFamilyTypeSuffix) == 0);
    const bool is_uniform = kUniformProps.count(pe.name) || is_family_type;

    if (is_uniform && !pe.has_ts) {
      crate::CrateValue var_value;
      var_value.Set(Variability::Uniform);
      attr_fields.push_back({"variability", var_value});
    }

    if (pe.has_default) {
      attr_fields.push_back({"default", std::move(pe.default_val)});
    }

    if (pe.has_ts) {
      attr_fields.push_back({"timeSamples", std::move(pe.ts_val)});
    }

    if (pe.has_spline) {
      attr_fields.push_back({"spline", std::move(pe.spline_val)});
    }

    if (pe.has_interpolation) {
      attr_fields.push_back({"interpolation", std::move(pe.interpolation_val)});
    }

    if (pe.has_conn) {
      attr_fields.push_back({"connectionPaths", std::move(pe.conn_val)});
    }

    for (auto& mk : pe.extra_metas) {
      attr_fields.push_back({mk.first, std::move(mk.second)});
    }

    if (!attr_fields.empty()) {
      if (!AddSpec(attr_path, SpecType::Attribute, std::move(attr_fields), err)) {
        DCOUT("WARNING: Failed to add attribute spec for: " << pe.name);
      }
    }
  }

  // After property extraction, handle Material outputs as separate attribute specs
  // This must happen AFTER the prim spec is added to maintain correct ordering
  if (type_name == "Material") {
    const Material* material = prim.data().as<Material>();
    if (material) {
      // These will be added as separate attribute specs after the Material prim
      if (!AddMaterialOutputSpecs(material, prim_path, err)) {
        if (err) *err = "Failed to add Material output specs: " + *err;
        return false;
      }
    }
  }

  // After adding Shader prim spec, handle shader-specific inputs as separate attribute specs
  if (type_name == "Shader") {
    const Shader* shader = prim.data().as<Shader>();
    if (shader) {
      // Check shader type and extract inputs accordingly
      if (shader->info_id == "UsdPreviewSurface") {
        // Try to get UsdPreviewSurface from shader value
        if (auto* preview_surface = shader->value.as<UsdPreviewSurface>()) {
          if (!AddUsdPreviewSurfaceInputSpecs(preview_surface, prim_path, err)) {
            if (err) *err = "Failed to add UsdPreviewSurface input specs: " + *err;
            return false;
          }
          // Terminal output declarations — call ConvertAttributeToFields
          // directly (bypasses is_empty() check in ConvertPropertyToFields)
          if (preview_surface->outputsSurface.authored()) {
            Attribute a;
            a.set_type_name(value::kToken);
            if (!ConvertAttributeToFields("outputs:surface", a, prim_path, false, err)) {
              return false;
            }
          }
          if (preview_surface->outputsDisplacement.authored()) {
            Attribute a;
            a.set_type_name(value::kToken);
            if (!ConvertAttributeToFields("outputs:displacement", a, prim_path, false, err)) {
              return false;
            }
          }
        }
      } else if (shader->info_id == "UsdUVTexture") {
        if (auto* uv_texture = shader->value.as<UsdUVTexture>()) {
          if (!AddUsdUVTextureInputSpecs(uv_texture, prim_path, err)) {
            if (err) *err = "Failed to add UsdUVTexture input specs: " + *err;
            return false;
          }
          // Terminal outputs — use actual_type_name if available, else the template type
          auto add_t = [&](const char *n, const auto &term_attr, const char *default_type) -> bool {
            if (!term_attr.authored()) return true;
            Attribute attr;
            attr.set_type_name(term_attr.has_actual_type() ? term_attr.get_actual_type_name() : default_type);
            return ConvertAttributeToFields(n, attr, prim_path, false, err);
          };
          if (!add_t("outputs:r", uv_texture->outputsR, "float")) return false;
          if (!add_t("outputs:g", uv_texture->outputsG, "float")) return false;
          if (!add_t("outputs:b", uv_texture->outputsB, "float")) return false;
          if (!add_t("outputs:a", uv_texture->outputsA, "float")) return false;
          if (!add_t("outputs:rgb", uv_texture->outputsRGB, "float3")) return false;
          if (!add_t("outputs:rgba", uv_texture->outputsRGBA, "float4")) return false;
        }
      } else if (shader->info_id == "UsdTransform2d") {
        if (auto* transform2d = shader->value.as<UsdTransform2d>()) {
          if (!AddUsdTransform2dInputSpecs(transform2d, prim_path, err)) {
            if (err) *err = "Failed to add UsdTransform2d input specs: " + *err;
            return false;
          }
          if (transform2d->result.authored()) {
            Attribute a;
            a.set_type_name(transform2d->result.has_actual_type()
                                ? transform2d->result.get_actual_type_name()
                                : "float2");
            if (!ConvertAttributeToFields("outputs:result", a, prim_path, false, err)) {
              return false;
            }
          }
        }
      } else if (shader->info_id.find("UsdPrimvarReader_") == 0) {
        if (!AddUsdPrimvarReaderInputSpecs(shader->value, shader->info_id, prim_path, err)) {
          if (err) *err = "Failed to add UsdPrimvarReader input specs: " + *err;
          return false;
        }
        // Terminal output for all PrimvarReader variants
        // `result.type_name()` is STATIC -- it is the C++ template parameter, so
        // it always reports the canonical type (float2 for
        // UsdPrimvarReader_float2) and threw away a non-conformant authored one
        // (`token outputs:result`). The authored spelling is kept in
        // actual_type_name, exactly as the UsdUVTexture terminals above use it.
        auto add_pr_terminal = [&](auto *pr) -> bool {
          if (!pr || !pr->result.authored()) return true;
          Attribute a;
          std::string tname = pr->result.has_actual_type()
                                  ? pr->result.get_actual_type_name()
                                  : pr->result.type_name();
          a.set_type_name(tname.empty() ? "float2" : tname);
          return ConvertAttributeToFields("outputs:result", a, prim_path, false, err);
        };
        if (auto *p0 = shader->value.as<UsdPrimvarReader_float2>()) { if (!add_pr_terminal(p0)) return false; }
        else if (auto *p1 = shader->value.as<UsdPrimvarReader_float>()) { if (!add_pr_terminal(p1)) return false; }
        else if (auto *p2 = shader->value.as<UsdPrimvarReader_float3>()) { if (!add_pr_terminal(p2)) return false; }
        else if (auto *p3 = shader->value.as<UsdPrimvarReader_float4>()) { if (!add_pr_terminal(p3)) return false; }
        else if (auto *p4 = shader->value.as<UsdPrimvarReader_int>()) { if (!add_pr_terminal(p4)) return false; }
        else if (auto *p5 = shader->value.as<UsdPrimvarReader_string>()) { if (!add_pr_terminal(p5)) return false; }
      } else if (auto* mtlx_surface = shader->value.as<MtlxOpenPBRSurface>()) {
        // MaterialX ND_open_pbr_surface_surfaceshader -> typed MtlxOpenPBRSurface
        if (!AddMtlxOpenPBRSurfaceInputSpecs(mtlx_surface, prim_path, err)) {
          if (err) *err = "Failed to add MtlxOpenPBRSurface input specs: " + *err;
          return false;
        }
      }
    }
  }

  // After adding prim spec, handle material bindings as separate relationship specs
  // This applies to any prim that might have material bindings (typically geometry)
  if (!AddMaterialBindingSpecs(prim, prim_path, err)) {
    // Don't fail on material binding errors - just warn
  }

  // SkelBindingAPI relationships (skel:animationSource / skel:skeleton /
  // skel:blendShapeTargets) are stored as typed optional Relationship fields and
  // would otherwise be dropped on USDC write, losing skeletal animation bindings.
  if (!AddSkelBindingSpecs(prim, prim_path, err)) {
    // Don't fail the whole export on a skel binding error - just warn.
  }

  // Handle doubleSided for GPrim-derived types via ConvertPropertyToFields
  // (The fields path doesn't preserve the bool type correctly in crate format)
  {
    const GPrim* gprim = nullptr;
    // Try all GPrim-derived types
    if (auto* g0 = prim.data().as<GeomMesh>()) gprim = g0;
    else if (auto* g1 = prim.data().as<GeomCube>()) gprim = g1;
    else if (auto* g2 = prim.data().as<GeomSphere>()) gprim = g2;
    else if (auto* g3 = prim.data().as<GeomCone>()) gprim = g3;
    else if (auto* g4 = prim.data().as<GeomCylinder>()) gprim = g4;
    else if (auto* g5 = prim.data().as<GeomCapsule>()) gprim = g5;
    else if (auto* g6 = prim.data().as<GeomPoints>()) gprim = g6;
    else if (auto* g7 = prim.data().as<GeomBasisCurves>()) gprim = g7;

    if (gprim && gprim->doubleSided.authored()) {
      Attribute ds_attr;
      ds_attr.set_type_name("bool");
      ds_attr.variability() = Variability::Uniform;
      // DECLARED without a value (`uniform bool doubleSided`): leave the
      // Attribute value-less. get_value() would hand back GPrim's `false`
      // fallback and author it as though the scene had said so.
      if (!gprim->doubleSided.is_value_empty()) {
        primvar::PrimVar pvar;
        pvar.set_value(gprim->doubleSided.get_value());
        ds_attr.set_var(std::move(pvar));
      }
      crate::FieldValuePairVector dummy;
      ConvertPropertyToFields("doubleSided", Property(ds_attr, /* custom */ false), prim_path, dummy, err);
    }
  }

  // After adding light prim spec, handle light filter relationships as separate relationship specs
  // This applies to any light prim that has light:filters relationships
  if (!AddLightFilterSpecs(prim, prim_path, err)) {
    // Don't fail on light filter relationship errors - just warn
  }

  // PointInstancer prototypes relationship: emit as a separate relationship
  // spec so `rel prototypes = [...]` survives USDC roundtrip.
  if (prim.data().as<GeomPointInstancer>()) {
    if (!AddPointInstancerPrototypesSpec(prim, prim_path, err)) {
      DCOUT("WARNING: Failed to add prototypes relationship spec");
    }
  }

  // Process generic props map for custom properties, primvars, relationships, etc.
  // This covers ALL prim types and handles properties not extracted by type-specific handlers.
  // Skip properties already extracted by type-specific handlers to avoid duplicates.
  // Re-emit Collection (`collection:<inst>:{includes,excludes,
  // includeRoot,expansionRule,membershipExpression}`) from typed storage.
  // The reconstruct path consumed these from the props map and stored
  // them on `Collection::instances()`; without this re-emit, USDC
  // round-trip would drop them.
  if (const Collection *coll_storage = GetPrimCollection(prim.data())) {
    const auto &instances = coll_storage->instances();
    for (size_t i = 0; i < instances.size(); ++i) {
      const std::string &inst_name = instances.keys()[i];
      CollectionInstance inst;
      if (!instances.at(i, &inst)) {
        continue;
      }
      const std::string prefix = "collection:" + inst_name;

      if (inst.membershipExpression.authored()) {
        const auto &expr = inst.membershipExpression;
        Attribute a;
        a.variability() = Variability::Uniform;
        a.set_type_name(expr.has_actual_type()
                            ? expr.get_actual_type_name() : "pathExpression");
        if (expr.has_value()) {
          a.set_value(expr.get_value_ref().value());
        }
        a.set_blocked(expr.is_blocked());
        a.metas() = expr.metas();
        if (expr.has_connections()) {
          a.set_connections(expr.get_connections());
        }
        if (!ConvertAttributeToFields(prefix + ":membershipExpression", a,
                                      prim_path, /*is_custom=*/false, err)) {
          return false;
        }
      }

      if (inst.includes.authored()) {
        std::string ce;
        if (!ConvertRelationshipToFields(prefix + ":includes",
                                         inst.includes.relationship(),
                                         prim_path, &ce, /*is_custom=*/false)) {
          DCOUT("WARNING: Collection includes emit failed: " << ce);
        }
      }
      if (inst.excludes.authored()) {
        std::string ce;
        if (!ConvertRelationshipToFields(prefix + ":excludes",
                                         inst.excludes.relationship(),
                                         prim_path, &ce, /*is_custom=*/false)) {
          DCOUT("WARNING: Collection excludes emit failed: " << ce);
        }
      }
      if (inst.expansionRule.authored()) {
        // expansionRule is a uniform token attribute. Map the
        // ExpansionRule enum back to its token spelling.
        const char *tok = nullptr;
        switch (inst.expansionRule.get_value()) {
          case CollectionInstance::ExpansionRule::ExplicitOnly:
            tok = kExplicitOnly; break;
          case CollectionInstance::ExpansionRule::ExpandPrims:
            tok = kExpandPrims; break;
          case CollectionInstance::ExpansionRule::ExpandPrimsAndProperties:
            tok = kExpandPrimsAndProperties; break;
        }
        if (tok) {
          Attribute a = Attribute::Uniform(value::token(tok));
          a.set_type_name("token");
          std::string ce;
          if (!ConvertAttributeToFields(prefix + ":expansionRule", a,
                                        prim_path, /*is_custom=*/false,
                                        &ce)) {
            DCOUT("WARNING: Collection expansionRule emit failed: " << ce);
          }
        }
      }
      if (inst.includeRoot.authored()) {
        bool b = false;
        if (inst.includeRoot.get_value().get_scalar(&b)) {
          Attribute a = Attribute::Uniform(b);
          a.set_type_name("bool");
          std::string ce;
          if (!ConvertAttributeToFields(prefix + ":includeRoot", a,
                                        prim_path, /*is_custom=*/false,
                                        &ce)) {
            DCOUT("WARNING: Collection includeRoot emit failed: " << ce);
          }
        }
      }
    }
  }

  if (props_map) {
    // Build set of names already extracted by type-specific handlers
    std::unordered_set<std::string> extracted_names;
    for (const auto& pe : prop_entries) {
      extracted_names.insert(pe.name);
    }

    crate::FieldValuePairVector dummy_fields;
    for (const auto& kv : *props_map) {
      if (extracted_names.count(kv.first)) {
        continue;  // Already handled by type-specific extractor
      }
      // Skip xformOp properties when the typed xformable path already
      // produced specs for them (avoids duplicates). When the typed path
      // didn't extract anything (extracted_names has no xformOp entries),
      // fall through so props-authored xformOps survive USDC round-trip.
      if (kv.first.find("xformOp:") == 0 || kv.first == "xformOpOrder") {
        bool typed_emitted = false;
        for (const auto &en : extracted_names) {
          if (en.find("xformOp:") == 0 || en == "xformOpOrder") {
            typed_emitted = true;
            break;
          }
        }
        if (typed_emitted) continue;
      }

      // For attribute properties, check if the value type is writable.
      // ConvertAttributeToFields calls AddSpec which can't be undone if
      // Finalize later fails on unsupported types.
      // Note: empty array attributes encoded via the TRY_INLINE_EMPTY_ARRAY
      // path (IsArray + payload=0). Reader handles this via per-type
      // checks at e.g. crate-reader-values.cc:983.

      std::string prop_err;
      if (!ConvertPropertyToFields(kv.first, kv.second, prim_path, dummy_fields, &prop_err)) {
        DCOUT("WARNING: Failed to convert property from props map: " << kv.first
                  << ": " << prop_err);
      }
    }
  }

  // After adding prim spec, process VariantSets if present
  // Variants represent alternative versions of prims/properties
  const auto& variant_sets = prim.variantSets();
  if (!variant_sets.empty()) {
    for (const auto& vs_item : variant_sets) {
      const auto& variantset_name = vs_item.first;
      const auto& variantset_data = vs_item.second;
      if (!ConvertVariantSetToFields(variantset_name, variantset_data, prim_path, err, /*depth*/ 0)) {
        if (err) *err = "Failed to convert VariantSet '" + variantset_name + "' for " + abs_path_str + ": " + *err;
        return false;
      }
    }
  }

  // Build "properties" field by scanning accumulated specs for child
  // Attribute/Relationship specs of this prim.  This field is required by
  // Pixar's USD reader and also drives propertyNames() ordering.
  {
    const std::string prim_prefix = abs_path_str + ".";
    std::vector<value::token> property_names;
    std::set<std::string> seen;
    // Scan only the specs this prim added (see my_specs_begin), not all of
    // spec_data_ — direct-child Attribute/Relationship specs are always among
    // this prim's own specs.
    for (size_t _i = my_specs_begin; _i < out_specs.size(); ++_i) {
      const auto &sd = out_specs[_i];
      if (sd.spec_type == SpecType::Attribute ||
          sd.spec_type == SpecType::Relationship) {
        const std::string &fp = sd.path.full_path_name();
        if (fp.size() > prim_prefix.size() &&
            fp.compare(0, prim_prefix.size(), prim_prefix) == 0) {
          std::string prop_name = fp.substr(prim_prefix.size());
          // Only direct children (no further '/' or '.')
          if (prop_name.find('/') == std::string::npos &&
              prop_name.find('.') == std::string::npos) {
            if (seen.insert(prop_name).second) {
              property_names.push_back(value::token(prop_name));
            }
          }
        }
      }
    }
    if (!property_names.empty()) {
      // Append "properties" field to this prim's Prim spec (also within the
      // [my_specs_begin, end) range this call appended).
      for (size_t _i = my_specs_begin; _i < out_specs.size(); ++_i) {
        auto &sd = out_specs[_i];
        if (sd.spec_type == SpecType::Prim &&
            sd.path.full_path_name() == abs_path_str) {
          crate::CrateValue props_value;
          props_value.Set(property_names);
          sd.fields.push_back({"properties", props_value});
          break;
        }
      }
    }
  }

  return true;
}

// ============================================================================
// Iterative Depth-First Prim Conversion
// ============================================================================

bool CrateWriter::ConvertPrimIterative(
  const Prim& root_prim,
  const Path& parent_path,
  std::string* err
) {
  // Explicit DFS stack: (prim pointer, parent path, nesting depth)
  struct WorkItem {
    const Prim* prim;
    Path parent_path;
    uint32_t depth;
  };

  std::vector<WorkItem> stack;
  stack.push_back({&root_prim, parent_path, 0});

  while (!stack.empty()) {
    if (stack.size() > kMaxDefaultTraversalLimit) {
      if (err) *err = "ConvertPrimIterative: tree too deep.";
      return false;
    }

    WorkItem item = std::move(stack.back());
    stack.pop_back();

    // Authoritative prim-nesting depth gate. The path-tree builder recurses on
    // the native stack (see kMaxPrimNestingDepth in crate-writer.hh); reject
    // here, early and clearly, so deeply nested stages don't fail cryptically
    // during path-tree building.
    if (item.depth > kMaxPrimNestingDepth) {
      if (err) {
        *err = "Prim nesting too deep (>" +
               std::to_string(kMaxPrimNestingDepth) +
               " levels) at " + item.parent_path.prim_part() + "/" +
               item.prim->element_name();
      }
      return false;
    }

    if (!ConvertSinglePrim(*item.prim, item.parent_path, err)) {
      return false;
    }

    // Build this prim's path for its children
    std::string parent_str = item.parent_path.prim_part();
    std::string abs_path_str;
    if (parent_str == "/") {
      abs_path_str = "/" + item.prim->element_name();
    } else {
      abs_path_str = parent_str + "/" + item.prim->element_name();
    }
    Path prim_path(abs_path_str, "");

    // Push children in reverse order so left-most child is processed first
    const auto& children = item.prim->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back({&(*it), prim_path, item.depth + 1});
    }
  }

  return true;
}

// ============================================================================
// Property Extraction
// ============================================================================


void CrateWriter::ExtractPrimMeta(
    const PrimMeta& metas,
    crate::FieldValuePairVector& fields) {

  if (metas.has_active()) {
    crate::CrateValue v;
    v.Set(metas.get_active());
    fields.push_back({"active", v});
  }

  if (metas.has_hidden()) {
    crate::CrateValue v;
    v.Set(metas.get_hidden());
    fields.push_back({"hidden", v});
  }

  if (metas.has_kind()) {
    crate::CrateValue v;
    value::token tok(metas.get_kind());
    v.Set(tok);
    fields.push_back({"kind", v});
  }

  if (metas.has_displayName()) {
    crate::CrateValue v;
    v.Set(metas.get_displayName());
    fields.push_back({"displayName", v});
  }

  if (metas.has_doc()) {
    crate::CrateValue v;
    v.Set(metas.get_doc().value);
    fields.push_back({"documentation", v});
  }

  if (metas.has_comment()) {
    crate::CrateValue v;
    v.Set(metas.get_comment().value);
    fields.push_back({"comment", v});
  }

  if (metas.has_sceneName()) {
    // pxrUSD stores `sceneName` (USDZ scene-library extension) as a
    // `string`, not a `token`. The reader rejects token-typed values.
    crate::CrateValue v;
    v.Set(std::string(metas.get_sceneName()));
    fields.push_back({"sceneName", v});
  }

  if (metas.variants) {
    crate::CrateValue v;
    v.Set(metas.variants.value());
    fields.push_back({"variantSelection", v});
  }

  if (metas.variantSets) {
    ListOp<std::string> vs_listop;

    for (const auto& variantSets_op : metas.variantSets.value()) {
      const ListEditQual& qual = variantSets_op.first;
      const std::vector<std::string>& vs_list = variantSets_op.second;

      switch (qual) {
        case ListEditQual::ResetToExplicit:
          vs_listop.ClearAndMakeExplicit();
          vs_listop.SetExplicitItems(vs_list);
          break;
        case ListEditQual::Append:
          vs_listop.SetAppendedItems(vs_list);
          break;
        case ListEditQual::Prepend:
          vs_listop.SetPrependedItems(vs_list);
          break;
        case ListEditQual::Add:
          vs_listop.SetAddedItems(vs_list);
          break;
        case ListEditQual::Delete:
          vs_listop.SetDeletedItems(vs_list);
          break;
        default:
          vs_listop.SetPrependedItems(vs_list);
          break;
      }
    }

    crate::CrateValue v;
    v.Set(vs_listop);
    fields.push_back({"variantSetNames", v});
  }

  // Composition arcs and dictionary metadata (customData, assetInfo, clips),
  // apiSchemas, references, payload, inherits, and specializes ARE emitted on
  // the Stage path below, mirroring the Layer path (ConvertPrimSpecRecursive in
  // sconv-layer.cc). These were historically disabled due to fieldset
  // decode/serialization issues; the encoding has since been fixed and is now
  // covered by Stage->USDC round-trip tests (see unit-crate-writer.cc:
  // crate_writer_stage_composition_arcs_test). Keep the two paths in sync when
  // adding new metadata fields.

  if (metas.has_instanceable()) {
    crate::CrateValue v;
    v.Set(metas.get_instanceable());
    fields.push_back({"instanceable", v});
  }

  // `reorder nameChildren = [...]` / `reorder properties = [...]`. Parsed into
  // PrimMeta and then written by nobody, so both statements vanished on write.
  // pxr's crate spelling for the two body statements is primOrder /
  // propertyOrder (NOT primChildren / properties, which are the full name
  // vectors); the reader picks them back up in usdc-reader-prim.cc.
  if (!metas.nameChildrenReorder.empty()) {
    crate::CrateValue v;
    v.Set(metas.nameChildrenReorder);
    fields.push_back({"primOrder", v});
  }

  if (!metas.propertiesReorder.empty()) {
    crate::CrateValue v;
    v.Set(metas.propertiesReorder);
    fields.push_back({"propertyOrder", v});
  }

  // customData (Dictionary). Keep this on the Stage path so prim-level
  // customData survives USDC round-trip; the writer supports the common scalar,
  // vector, array, nested-dictionary, token and asset-path metadata values.
  if (metas.has_customData()) {
    crate::CrateValue v;
    v.Set(metas.get_customData());
    fields.push_back({"customData", v});
  }

  // assetInfo (Dictionary).
  if (metas.has_assetInfo()) {
    crate::CrateValue v;
    v.Set(metas.get_assetInfo());
    fields.push_back({"assetInfo", v});
  }

  // clips (Dictionary). Value-clip declarations for animation streaming.
  if (metas.has_clips()) {
    crate::CrateValue v;
    v.Set(metas.get_clips());
    fields.push_back({"clips", v});
  }

  // sdrMetadata (Dictionary, UsdShade). Parsed by the readers into MetadataBase
  // and then written by nobody, so it vanished on write.
  if (metas.has_sdrMetadata()) {
    crate::CrateValue v;
    v.Set(metas.get_sdrMetadata());
    fields.push_back({"sdrMetadata", v});
  }

  // UNREGISTERED prim metadata (`def "bora" ( aa = a )`) written by the CRATE
  // reader lands in PrimMeta::data(), and the generic data() loop further down
  // already emits it. But the ASCII reader does NOT populate data() for these --
  // it keeps them only in unregisteredMetas, already pretty-printed to their
  // source spelling (see usda-reader-impl.hh), so a usda-authored one was
  // dropped. Round-trip that spelling verbatim as the crate UNREGISTERED_VALUE
  // type (what pxr writes for SdfUnregisteredValue): the crate reader routes it
  // straight back into unregisteredMetas and the printer emits it as authored.
  for (const auto &kv : metas.unregisteredMetas) {
    if (metas.data().count(kv.first)) {
      continue;  // the data() loop below owns it
    }
    crate::CrateValue v;
    v.SetUnregisteredValueString(kv.second);
    fields.push_back({kv.first, v});
  }

  // apiSchemas. `authoredOps` (populated by the ASCII/USDC readers, see
  // ToAPISchemas in usdc-reader-prim.cc) is the verbatim authored
  // SdfTokenListOp -- possibly SEVERAL ops on one prim (e.g. `delete` +
  // `prepend`, the Omniverse/Newton-asset pattern) -- and is the only source
  // that can reproduce it losslessly. The OLD code instead rebuilt a single
  // ListOp from the *resolved* view (`names`/`unknownSchemas`), which always
  // collapses to one explicit-or-prepend op: any `delete` was silently
  // dropped, `names` and `unknownSchemas` are separate vectors so interleaved
  // authoring order was lost, and an `apiSchemas = None` block (explicitly
  // empty, so both vectors are empty too) wrote nothing at all instead of an
  // explicit empty list.
  if (metas.has_apiSchemas()) {
    const auto &schemas = metas.get_apiSchemas();
    if (schemas.explicitlyEmpty) {
      // `apiSchemas = None`: an explicit, empty list-op, distinct from no
      // opinion at all.
      ListOp<value::token> listop;
      listop.ClearAndMakeExplicit();
      crate::CrateValue v;
      v.Set(listop);
      fields.push_back({"apiSchemas", v});
    } else if (!schemas.authoredOps.empty()) {
      ListOp<value::token> listop;
      for (const auto &op : schemas.authoredOps) {
        std::vector<value::token> toks;
        toks.reserve(op.second.size());
        for (const auto &item : op.second) {
          toks.push_back(value::token(item.first));
        }
        switch (op.first) {
          case ListEditQual::ResetToExplicit:
            listop.ClearAndMakeExplicit();
            listop.SetExplicitItems(toks);
            break;
          case ListEditQual::Append:  listop.SetAppendedItems(toks); break;
          case ListEditQual::Prepend: listop.SetPrependedItems(toks); break;
          case ListEditQual::Add:     listop.SetAddedItems(toks); break;
          case ListEditQual::Delete:  listop.SetDeletedItems(toks); break;
          default:
            listop.ClearAndMakeExplicit();
            listop.SetExplicitItems(toks);
            break;
        }
      }
      crate::CrateValue v;
      v.Set(listop);
      fields.push_back({"apiSchemas", v});
    } else if (!schemas.names.empty() || !schemas.unknownSchemas.empty()) {
      // Fallback for APISchemas built programmatically (e.g. attached via the
      // C API or a tydra converter) with no authored-op history to replay.
      std::vector<value::token> schema_tokens;
      for (const auto &s : schemas.names) {
        std::string name = to_string(s.first);
        if (!s.second.empty()) {
          name += ":" + s.second;  // Multi-apply instance
        }
        schema_tokens.push_back(value::token(name));
      }
      for (const auto &s : schemas.unknownSchemas) {
        std::string name = s.first;
        if (!s.second.empty()) {
          name += ":" + s.second;
        }
        schema_tokens.push_back(value::token(name));
      }
      ListOp<value::token> listop;
      if (schemas.listOpQual == ListEditQual::Prepend) {
        listop.SetPrependedItems(schema_tokens);
      } else {
        listop.ClearAndMakeExplicit();
        listop.SetExplicitItems(schema_tokens);
      }
      crate::CrateValue v;
      v.Set(listop);
      fields.push_back({"apiSchemas", v});
    }
  }

  // static: built from literals once, not per call (this sits on the
  // per-prim hot path of ConvertStageToSpecs).
  static const std::unordered_set<std::string> emitted_prim_meta_names = {
      "active",
      "apiSchemas",
      "assetInfo",
      "clips",
      "comment",
      "customData",
      "displayName",
      "documentation",
      "hidden",
      "inherits",
      "inheritPaths",
      "instanceable",
      "kind",
      "payload",
      "primChildren",
      "properties",
      "references",
      "sceneName",
      "sdrMetadata",  // emitted above from the typed member
      "specializes",
      "variantChildren",
      "variantSelection",
      "variantSetChildren",
      "variantSetNames",
      "variantSets",
      "variants"};
  for (const auto& kv : metas.data()) {
    if (emitted_prim_meta_names.count(kv.first)) {
      continue;
    }
    crate::CrateValue v;
    std::string meta_err;
    if (!ConvertValue(kv.second.get_raw_value(), v, &meta_err)) {
      DCOUT("WARNING: Skipping unsupported prim metadata `" << kv.first
            << "`: " << meta_err);
      continue;
    }
    fields.push_back({kv.first, v});
  }

  // Composition arcs — references, inherits, specializes, payload.
  // Build ListOp<T> objects with the parsed list-edit qualifiers and emit them
  // as crate fields. Mirrors the working layer-path writer in sconv-layer.cc.

  auto convert_path_listop = [](
      const std::vector<std::pair<ListEditQual, std::vector<Path>>>& src) {
    ListOp<Path> out;
    for (const auto& op : src) {
      switch (op.first) {
        case ListEditQual::ResetToExplicit:
          out.ClearAndMakeExplicit();
          out.SetExplicitItems(op.second);
          break;
        case ListEditQual::Append:  out.SetAppendedItems(op.second); break;
        case ListEditQual::Prepend: out.SetPrependedItems(op.second); break;
        case ListEditQual::Add:     out.SetAddedItems(op.second); break;
        case ListEditQual::Delete:  out.SetDeletedItems(op.second); break;
        default:
          out.ClearAndMakeExplicit();
          out.SetExplicitItems(op.second);
          break;
      }
    }
    return out;
  };

  auto convert_ref_listop = [](
      const std::vector<std::pair<ListEditQual, std::vector<Reference>>>& src) {
    ListOp<Reference> out;
    for (const auto& op : src) {
      switch (op.first) {
        case ListEditQual::ResetToExplicit:
          out.ClearAndMakeExplicit();
          out.SetExplicitItems(op.second);
          break;
        case ListEditQual::Append:  out.SetAppendedItems(op.second); break;
        case ListEditQual::Prepend: out.SetPrependedItems(op.second); break;
        case ListEditQual::Add:     out.SetAddedItems(op.second); break;
        case ListEditQual::Delete:  out.SetDeletedItems(op.second); break;
        default:
          out.ClearAndMakeExplicit();
          out.SetExplicitItems(op.second);
          break;
      }
    }
    return out;
  };

  auto convert_payload_listop = [](
      const std::vector<std::pair<ListEditQual, std::vector<Payload>>>& src) {
    ListOp<Payload> out;
    for (const auto& op : src) {
      switch (op.first) {
        case ListEditQual::ResetToExplicit:
          out.ClearAndMakeExplicit();
          out.SetExplicitItems(op.second);
          break;
        case ListEditQual::Append:  out.SetAppendedItems(op.second); break;
        case ListEditQual::Prepend: out.SetPrependedItems(op.second); break;
        case ListEditQual::Add:     out.SetAddedItems(op.second); break;
        case ListEditQual::Delete:  out.SetDeletedItems(op.second); break;
        default:
          out.ClearAndMakeExplicit();
          out.SetExplicitItems(op.second);
          break;
      }
    }
    return out;
  };

  if (metas.references.has_value()) {
    crate::CrateValue v;
    v.Set(convert_ref_listop(metas.references.value()));
    fields.push_back({"references", v});
  }

  if (metas.payload.has_value()) {
    crate::CrateValue v;
    v.Set(convert_payload_listop(metas.payload.value()));
    fields.push_back({"payload", v});
  }

  if (metas.inherits.has_value()) {
    crate::CrateValue v;
    v.Set(convert_path_listop(metas.inherits.value()));
    fields.push_back({"inheritPaths", v});
  }

  if (metas.specializes.has_value()) {
    crate::CrateValue v;
    v.Set(convert_path_listop(metas.specializes.value()));
    fields.push_back({"specializes", v});
  }
}

bool CrateWriter::ExtractPrimProperties(
  const Prim& prim,
  const Path& prim_path,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  // Get prim type name. prim_type_name() carries the parser-set or
  // user-set string; for in-memory authored prims that field is empty,
  // so fall back to the typed schema name from the underlying value.
  // Without this fallback the writer skips both the `typeName` crate
  // field and the type-specific property extraction, and the reader sees
  // a generic prim with no schema and no attributes.
  // Exception: a genuinely typeless prim (`def "bora"`) is held internally as
  // the catch-all `Model` struct, whose type_name() unconditionally returns
  // "Model" -- an internal label, not an authored or inferable schema name.
  // Falling back to it here resurrects a typeName the prim never had, and
  // `def "bora"` came back from usdc as `def Model "bora"`.
  //
  // Do NOT drop the fallback itself: in-memory authored prims (e.g. those built
  // by the tydra converters) have an empty prim_type_name() but a REAL schema
  // behind them, and without it the writer skips both the `typeName` field and
  // the type-specific property extraction -- losing every shader input/output.
  std::string type_name = prim.prim_type_name();
  if (type_name.empty() && prim.type_name() != "Model") {
    type_name = prim.type_name();
  }

  // Add specifier field (as Specifier enum, not as token)
  Specifier spec = prim.specifier();
  // USDA loader sets the spec on the typed-prim wrapper inside `prim.data()`
  // but historically did not propagate it to the outer Prim wrapper. Fall
  // back to the typed prim's `.spec` so `class`/`over` survive USDC writing.
  if (spec == Specifier::Invalid) {
    const auto &v = prim.data();
#define GET_SPEC(__TY) if (auto *t = v.as<__TY>()) { spec = t->spec; goto spec_resolved; }
    GET_SPEC(Xform)
    GET_SPEC(GeomMesh)
    GET_SPEC(Volume)
    GET_SPEC(FieldAsset)
    GET_SPEC(OpenVDBAsset)
    GET_SPEC(Field3DAsset)
    GET_SPEC(GeomSphere)
    GET_SPEC(GeomCube)
    GET_SPEC(GeomCylinder)
    GET_SPEC(GeomCone)
    GET_SPEC(GeomCapsule)
    GET_SPEC(GeomCamera)
    GET_SPEC(GeomPoints)
    GET_SPEC(GeomSubset)
    GET_SPEC(GeomBasisCurves)
    GET_SPEC(GeomNurbsCurves)
    GET_SPEC(GeomHermiteCurves)
    GET_SPEC(GeomPlane)
    GET_SPEC(GeomCylinder_1)
    GET_SPEC(GeomCapsule_1)
    GET_SPEC(GeomTetMesh)
    GET_SPEC(GeomNurbsPatch)
    GET_SPEC(GeomPointInstancer)
    GET_SPEC(SphereLight)
    GET_SPEC(RectLight)
    GET_SPEC(DiskLight)
    GET_SPEC(DistantLight)
    GET_SPEC(CylinderLight)
    GET_SPEC(DomeLight)
    GET_SPEC(DomeLight_1)
    GET_SPEC(GeometryLight)
    GET_SPEC(PortalLight)
    GET_SPEC(Scope)
    GET_SPEC(Model)
    GET_SPEC(Material)
    GET_SPEC(Shader)
    GET_SPEC(NodeGraph)
    GET_SPEC(SkelRoot)
    GET_SPEC(Skeleton)
    GET_SPEC(SkelAnimation)
    GET_SPEC(BlendShape)
    GET_SPEC(PhysicsScene)
    GET_SPEC(PhysicsJoint)
    GET_SPEC(PhysicsRevoluteJoint)
    GET_SPEC(PhysicsPrismaticJoint)
    GET_SPEC(PhysicsSphericalJoint)
    GET_SPEC(PhysicsFixedJoint)
    GET_SPEC(PhysicsDistanceJoint)
    GET_SPEC(PhysicsCollisionGroup)
    GET_SPEC(MjcActuator)
    GET_SPEC(NewtonActuator)
    GET_SPEC(MjcTendon)
    GET_SPEC(MjcKeyframe)
    GET_SPEC(MjcSensor)
#undef GET_SPEC
spec_resolved:;
  }
  if (spec == Specifier::Invalid) {
    spec = Specifier::Def;
  }
  crate::CrateValue spec_value;
  spec_value.Set(spec);  // CrateValue supports Specifier directly
  fields.push_back({"specifier", spec_value});

  // Add typeName if present - store as value::token, not as token index!
  if (!type_name.empty()) {
    crate::CrateValue typename_value;
    value::token tok(type_name);
    typename_value.Set(tok);  // Store the token, not the index!
    fields.push_back({"typeName", typename_value});
  }

  // Extract PrimMeta (kind, active, hidden, customData, apiSchemas, etc.)
  ExtractPrimMeta(prim.metas(), fields);

  // Extract type-specific properties
  if (!ExtractTypeSpecificProperties(prim, prim_path, type_name, fields, err)) {
    // Log warning but don't fail - we still want to create the prim
    DCOUT("WARNING: Failed to extract type-specific properties for "
              << type_name << ": " << (err ? *err : "unknown error"));
  }

  return true;
}

// ============================================================================
// Type-Specific Property Extraction
// ============================================================================

bool CrateWriter::ExtractTypeSpecificProperties(
  const Prim& prim,
  const Path& prim_path,
  const std::string& type_name,
  crate::FieldValuePairVector& fields,
  std::string* err
) {
  // Try to extract properties based on prim type
  if (type_name == "Xform") {
    return ExtractXformProperties(prim, prim_path, fields, err);
  } else if (type_name == "Scope") {
    return ExtractScopeProperties(prim, prim_path, fields, err);
  } else if (type_name == "Mesh") {
    return ExtractMeshProperties(prim, prim_path, fields, err);
  } else if (type_name == "Volume") {
    return ExtractVolumeProperties(prim, prim_path, fields, err);
  } else if (type_name == "FieldAsset") {
    return ExtractFieldAssetProperties(prim, prim_path, fields, err);
  } else if (type_name == "OpenVDBAsset") {
    return ExtractOpenVDBAssetProperties(prim, prim_path, fields, err);
  } else if (type_name == "Field3DAsset") {
    return ExtractField3DAssetProperties(prim, prim_path, fields, err);
  } else if (type_name == "Cube") {
    return ExtractCubeProperties(prim, prim_path, fields, err);
  } else if (type_name == "Sphere") {
    return ExtractSphereProperties(prim, prim_path, fields, err);
  } else if (type_name == "Cylinder") {
    return ExtractCylinderProperties(prim, prim_path, fields, err);
  } else if (type_name == "Cone") {
    return ExtractConeProperties(prim, prim_path, fields, err);
  } else if (type_name == "Capsule") {
    return ExtractCapsuleProperties(prim, prim_path, fields, err);
  } else if (type_name == "Points") {
    return ExtractPointsProperties(prim, prim_path, fields, err);
  } else if (type_name == "Camera") {
    return ExtractCameraProperties(prim, prim_path, fields, err);
  } else if (type_name == "BasisCurves") {
    return ExtractBasisCurvesProperties(prim, prim_path, fields, err);
  } else if (type_name == "NurbsCurves") {
    return ExtractNurbsCurvesProperties(prim, prim_path, fields, err);
  } else if (type_name == "Plane") {
    return ExtractGeomPlaneProperties(prim, prim_path, fields, err);
  } else if (type_name == "Cylinder_1") {
    return ExtractGeomCylinder1Properties(prim, prim_path, fields, err);
  } else if (type_name == "Capsule_1") {
    return ExtractGeomCapsule1Properties(prim, prim_path, fields, err);
  } else if (type_name == "TetMesh") {
    return ExtractGeomTetMeshProperties(prim, prim_path, fields, err);
  } else if (type_name == "NurbsPatch") {
    return ExtractGeomNurbsPatchProperties(prim, prim_path, fields, err);
  } else if (type_name == "HermiteCurves") {
    return ExtractGeomHermiteCurvesProperties(prim, prim_path, fields, err);
  } else if (type_name == "PointInstancer") {
    return ExtractPointInstancerProperties(prim, prim_path, fields, err);
  } else if (type_name == "GeomSubset") {
    return ExtractGeomSubsetProperties(prim, prim_path, fields, err);
  } else if (type_name == "Material") {
    return ExtractMaterialProperties(prim, prim_path, fields, err);
  } else if (type_name == "Shader") {
    return ExtractShaderProperties(prim, fields, err);
  } else if (type_name == "NodeGraph") {
    return ExtractNodeGraphProperties(prim, fields, err);
  } else if (type_name == "BlendShape") {
    return ExtractBlendShapeProperties(prim, prim_path, fields, err);
  } else if (type_name == "SphereLight") {
    return ExtractSphereLightProperties(prim, prim_path, fields, err);
  } else if (type_name == "RectLight") {
    return ExtractRectLightProperties(prim, prim_path, fields, err);
  } else if (type_name == "DiskLight") {
    return ExtractDiskLightProperties(prim, prim_path, fields, err);
  } else if (type_name == "CylinderLight") {
    return ExtractCylinderLightProperties(prim, prim_path, fields, err);
  } else if (type_name == "DistantLight") {
    return ExtractDistantLightProperties(prim, prim_path, fields, err);
  } else if (type_name == "DomeLight") {
    return ExtractDomeLightProperties(prim, prim_path, fields, err);
  } else if (type_name == "GeometryLight") {
    return ExtractGeometryLightProperties(prim, prim_path, fields, err);
  } else if (type_name == "PortalLight") {
    return ExtractPortalLightProperties(prim, prim_path, fields, err);
  } else if (type_name == "DomeLight_1") {
    return ExtractDomeLight1Properties(prim, prim_path, fields, err);
  } else if (type_name == "LightFilter") {
    return ExtractLightFilterProperties(prim, prim_path, fields, err);
  } else if (type_name == "PluginLightFilter") {
    return ExtractPluginLightFilterProperties(prim, prim_path, fields, err);
  } else if (type_name == "Skeleton") {
    return ExtractSkeletonProperties(prim, prim_path, fields, err);
  } else if (type_name == "SkelAnimation") {
    return ExtractSkelAnimationProperties(prim, prim_path, fields, err);
  } else if (type_name == "SkelRoot") {
    return ExtractSkelRootProperties(prim, prim_path, fields, err);
  }

  // UsdPhysics + mjcPhysics
  else if (type_name == "PhysicsJoint") {
    return ExtractPhysicsJointProperties(prim, prim_path, fields, err);
  } else if (type_name == "PhysicsScene") {
    return ExtractPhysicsSceneProperties(prim, prim_path, fields, err);
  } else if (type_name == "PhysicsRevoluteJoint") {
    return ExtractPhysicsRevoluteJointProperties(prim, prim_path, fields, err);
  } else if (type_name == "PhysicsPrismaticJoint") {
    return ExtractPhysicsPrismaticJointProperties(prim, prim_path, fields, err);
  } else if (type_name == "PhysicsSphericalJoint") {
    return ExtractPhysicsSphericalJointProperties(prim, prim_path, fields, err);
  } else if (type_name == "PhysicsFixedJoint") {
    return ExtractPhysicsFixedJointProperties(prim, prim_path, fields, err);
  } else if (type_name == "PhysicsDistanceJoint") {
    return ExtractPhysicsDistanceJointProperties(prim, prim_path, fields, err);
  } else if (type_name == "PhysicsCollisionGroup") {
    return ExtractPhysicsCollisionGroupProperties(prim, prim_path, fields, err);
  } else if (type_name == "MjcActuator") {
    return ExtractMjcActuatorProperties(prim, prim_path, fields, err);
  } else if (type_name == "NewtonActuator") {
    return ExtractNewtonActuatorProperties(prim, prim_path, fields, err);
  } else if (type_name == "MjcTendon") {
    return ExtractMjcTendonProperties(prim, prim_path, fields, err);
  } else if (type_name == "MjcKeyframe") {
    return ExtractMjcKeyframeProperties(prim, prim_path, fields, err);
  } else if (type_name == "MjcSensor") {
    return ExtractMjcSensorProperties(prim, prim_path, fields, err);
  // AR/Interactive (Apple Preliminary_*)
  } else if (type_name == "Preliminary_PhysicsGravitationalForce") {
    return ExtractPreliminaryGravitationalForceProperties(prim, prim_path, fields, err);
  } else if (type_name == "Preliminary_InfiniteColliderPlane") {
    return ExtractPreliminaryInfiniteColliderPlaneProperties(prim, prim_path, fields, err);
  } else if (type_name == "Preliminary_ReferenceImage") {
    return ExtractPreliminaryReferenceImageProperties(prim, prim_path, fields, err);
  } else if (type_name == "Preliminary_Behavior") {
    return ExtractPreliminaryBehaviorProperties(prim, prim_path, fields, err);
  } else if (type_name == "Preliminary_Trigger") {
    return ExtractPreliminaryTriggerProperties(prim, prim_path, fields, err);
  } else if (type_name == "Preliminary_Action") {
    return ExtractPreliminaryActionProperties(prim, prim_path, fields, err);
  } else if (type_name == "Preliminary_Text") {
    return ExtractPreliminaryTextProperties(prim, prim_path, fields, err);
  // usdMedia
  } else if (type_name == "SpatialAudio") {
    return ExtractSpatialAudioProperties(prim, prim_path, fields, err);
  }

  // For unknown types or types without specific handlers,
  // try extracting from the generic GPrim properties
  return ExtractGPrimProperties(prim, prim_path, fields, err);
}

// ============================================================================
// Value Conversion
// ============================================================================

bool CrateWriter::ConvertValue(
  const value::Value& val,
  crate::CrateValue& out,
  std::string* err
) {
  std::string type_name = val.type_name();

  // NOTE: every branch below is a standalone `if (type_name == "...")`, not a
  // link in one long `else if` chain -- MSVC hits C1061 ("blocks nested too
  // deeply") once an else-if chain accumulates enough branches (this
  // function has ~150 supported type names), because each `else if` nests
  // one level deeper than the last. Standalone ifs are flat regardless of
  // count. Safe because type_name is computed once above and never mutated,
  // so at most one of these conditions can ever be true -- see the identical
  // fix already applied for the same reason in crate-writer-values.cc's
  // TRY_PACK_DICT/TRY_PACK_META macros.
#define TRY_CONVERT_SET(TypeStr, CppType) \
  if (type_name == TypeStr) { \
    if (auto v = val.get_value<CppType>()) { \
      out.Set(*v); \
      return true; \
    } \
  }
#define TRY_CONVERT_SET_NOROLE(TypeStr, CppType) \
  if (type_name == TypeStr) { \
    if (auto v = val.get_value<CppType>(false)) { \
      out.Set(*v); \
      return true; \
    } \
  }
#define TRY_CONVERT_UNREG(TypeStr, CppType, FormatFn) \
  if (type_name == TypeStr) { \
    if (auto v = val.get_value<CppType>()) { \
      out.SetUnregisteredValueString(FormatFn(*v)); \
      return true; \
    } \
  }

  // AnimationBlock (SdfAnimationBlock): an inline type tag carried as a normal
  // default value. Emitted as Crate type 60 by the inline value packer.
  if (type_name == "AnimationBlock") {
    out.Set(value::AnimationBlock());
    return true;
  }

  // VtArrayEdit (crate >= 0.14.0): carried as a default value; the packer emits
  // a ValueRep with the IsArrayEdit bit set.
  if (type_name == "ArrayEdit") {
    if (auto* ae = val.as<value::ArrayEdit>()) {
      out.Set(*ae);
      return true;
    }
  }

  // Scalar types
  TRY_CONVERT_SET("bool", bool)
  TRY_CONVERT_SET("int", int32_t)
  TRY_CONVERT_SET("int32", int32_t)
  TRY_CONVERT_SET("uint", uint32_t)
  TRY_CONVERT_SET("uint32", uint32_t)
  TRY_CONVERT_UNREG("uint2", value::uint2, FormatUintTuple)
  TRY_CONVERT_UNREG("uint3", value::uint3, FormatUintTuple)
  TRY_CONVERT_UNREG("uint4", value::uint4, FormatUintTuple)
  TRY_CONVERT_UNREG("char", char, FormatSmallIntegerValue)
  TRY_CONVERT_UNREG("char2", value::char2, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("char3", value::char3, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("char4", value::char4, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("uchar2", value::uchar2, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("uchar3", value::uchar3, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("uchar4", value::uchar4, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("short", int16_t, FormatSmallIntegerValue)
  TRY_CONVERT_UNREG("short2", value::short2, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("short3", value::short3, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("short4", value::short4, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("ushort", uint16_t, FormatSmallIntegerValue)
  TRY_CONVERT_UNREG("ushort2", value::ushort2, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("ushort3", value::ushort3, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("ushort4", value::ushort4, FormatUnregisteredValue)
  TRY_CONVERT_SET("int64", int64_t)
  TRY_CONVERT_SET("uint64", uint64_t)
  TRY_CONVERT_SET("float", float)
  TRY_CONVERT_SET("double", double)
  TRY_CONVERT_SET("timecode", value::timecode)
  TRY_CONVERT_SET("half", value::half)

  // Token and String types
  // Store the typed token value; the crate packer pools it through the
  // tokens section at serialization time. Storing the raw pool index (uint)
  // here produced files that lightusd's reader surfaced as
  // `var_type='uint' (unresolved)` and that pxrusd rejected with "Corrupt
  // path element token index in crate file".
  TRY_CONVERT_SET("token", value::token)
  // Same rationale as the token branch above: store the string value, not
  // the strings-section index.
  TRY_CONVERT_SET("string", std::string)
  TRY_CONVERT_SET("asset", value::AssetPath)
  TRY_CONVERT_SET("asset[]", std::vector<value::AssetPath>)
  TRY_CONVERT_SET("pathExpression", value::PathExpression)
  TRY_CONVERT_SET("pathExpression[]", std::vector<value::PathExpression>)

  // Vector types - float2/3/4
  TRY_CONVERT_SET("float2", value::float2)
  TRY_CONVERT_SET("float3", value::float3)
  TRY_CONVERT_SET("float4", value::float4)

  // Vector types - half2/3/4
  TRY_CONVERT_SET("half2", value::half2)
  TRY_CONVERT_SET("half3", value::half3)
  TRY_CONVERT_SET("half4", value::half4)

  // Vector types - double2/3/4
  TRY_CONVERT_SET("double2", value::double2)
  TRY_CONVERT_SET("double3", value::double3)
  TRY_CONVERT_SET("double4", value::double4)

  // Vector types - int2/3/4
  TRY_CONVERT_SET("int2", value::int2)
  TRY_CONVERT_SET("int3", value::int3)
  TRY_CONVERT_SET("int4", value::int4)

  // Role types (point, vector, normal, color)
  // Role types are stored with their underlying type (float3, double3, etc.)
  // Use get_value with non-strict cast to get underlying type
  TRY_CONVERT_SET_NOROLE("point3f", value::float3)
  TRY_CONVERT_SET_NOROLE("point3h", value::half3)
  TRY_CONVERT_SET_NOROLE("point3d", value::double3)
  TRY_CONVERT_SET_NOROLE("vector3h", value::half3)
  TRY_CONVERT_SET_NOROLE("vector3f", value::float3)
  TRY_CONVERT_SET_NOROLE("vector3d", value::double3)
  TRY_CONVERT_SET_NOROLE("normal3f", value::float3)
  TRY_CONVERT_SET_NOROLE("normal3h", value::half3)
  TRY_CONVERT_SET_NOROLE("normal3d", value::double3)
  TRY_CONVERT_SET_NOROLE("color3h", value::half3)
  TRY_CONVERT_SET_NOROLE("color3f", value::float3)
  TRY_CONVERT_SET_NOROLE("color3d", value::double3)
  TRY_CONVERT_SET_NOROLE("color4h", value::half4)
  TRY_CONVERT_SET_NOROLE("color4f", value::float4)
  TRY_CONVERT_SET_NOROLE("color4d", value::double4)

  // Matrix types
  TRY_CONVERT_UNREG("matrix2f", value::matrix2f, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("matrix3f", value::matrix3f, FormatUnregisteredValue)
  TRY_CONVERT_UNREG("matrix4f", value::matrix4f, FormatUnregisteredValue)
  TRY_CONVERT_SET("matrix2d", value::matrix2d)
  TRY_CONVERT_SET("matrix3d", value::matrix3d)
  TRY_CONVERT_SET("matrix4d", value::matrix4d)
  // frame4d has no direct Crate representation; store as the equivalent
  // matrix4d. Bespoke (loop body), not a TRY_CONVERT_* one-liner.
  if (type_name == "frame4d") {
    if (auto v = val.get_value<value::frame4d>(false)) {
      value::matrix4d mat;
      for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
          mat.m[row][col] = (*v).m[row][col];
        }
      }
      out.Set(std::move(mat));
      return true;
    }
  }

  // Quaternion types
  TRY_CONVERT_SET("quath", value::quath)
  TRY_CONVERT_SET("quatf", value::quatf)
  TRY_CONVERT_SET("quatd", value::quatd)
  TRY_CONVERT_SET("uchar", uint8_t)

  // Array types
  TRY_CONVERT_SET("uchar[]", std::vector<uint8_t>)
  TRY_CONVERT_SET("int[]", std::vector<int32_t>)
  TRY_CONVERT_SET("uint[]", std::vector<uint32_t>)
  TRY_CONVERT_SET("uint32[]", std::vector<uint32_t>)
  TRY_CONVERT_UNREG("uint2[]", std::vector<value::uint2>, FormatUintTupleArray)
  TRY_CONVERT_UNREG("uint3[]", std::vector<value::uint3>, FormatUintTupleArray)
  TRY_CONVERT_UNREG("uint4[]", std::vector<value::uint4>, FormatUintTupleArray)
  TRY_CONVERT_UNREG("char[]", std::vector<char>, FormatSmallIntegerArray)
  TRY_CONVERT_UNREG("char2[]", std::vector<value::char2>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("char3[]", std::vector<value::char3>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("char4[]", std::vector<value::char4>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("uchar2[]", std::vector<value::uchar2>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("uchar3[]", std::vector<value::uchar3>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("uchar4[]", std::vector<value::uchar4>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("short[]", std::vector<int16_t>, FormatSmallIntegerArray)
  TRY_CONVERT_UNREG("short2[]", std::vector<value::short2>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("short3[]", std::vector<value::short3>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("short4[]", std::vector<value::short4>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("ushort[]", std::vector<uint16_t>, FormatSmallIntegerArray)
  TRY_CONVERT_UNREG("ushort2[]", std::vector<value::ushort2>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("ushort3[]", std::vector<value::ushort3>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("ushort4[]", std::vector<value::ushort4>, FormatUnregisteredArray)
  TRY_CONVERT_SET("float[]", std::vector<float>)
  TRY_CONVERT_SET("double[]", std::vector<double>)
  TRY_CONVERT_SET("timecode[]", std::vector<value::timecode>)
  TRY_CONVERT_SET("double2[]", std::vector<value::double2>)
  TRY_CONVERT_SET("string[]", std::vector<std::string>)
  TRY_CONVERT_SET("float3[]", std::vector<value::float3>)
  TRY_CONVERT_SET("double3[]", std::vector<value::double3>)

  // Role type arrays - convert to underlying type
  TRY_CONVERT_SET_NOROLE("point3f[]", std::vector<value::float3>)
  TRY_CONVERT_SET_NOROLE("point3h[]", std::vector<value::half3>)
  TRY_CONVERT_SET_NOROLE("point3d[]", std::vector<value::double3>)
  TRY_CONVERT_SET_NOROLE("normal3h[]", std::vector<value::half3>)
  TRY_CONVERT_SET_NOROLE("normal3f[]", std::vector<value::float3>)
  TRY_CONVERT_SET_NOROLE("normal3d[]", std::vector<value::double3>)
  TRY_CONVERT_SET_NOROLE("vector3f[]", std::vector<value::float3>)
  TRY_CONVERT_SET_NOROLE("vector3h[]", std::vector<value::half3>)
  TRY_CONVERT_SET_NOROLE("vector3d[]", std::vector<value::double3>)
  TRY_CONVERT_SET_NOROLE("color3h[]", std::vector<value::half3>)
  TRY_CONVERT_SET_NOROLE("color3f[]", std::vector<value::float3>)
  TRY_CONVERT_SET_NOROLE("color3d[]", std::vector<value::double3>)
  TRY_CONVERT_SET_NOROLE("color4h[]", std::vector<value::half4>)
  TRY_CONVERT_SET_NOROLE("color4f[]", std::vector<value::float4>)
  TRY_CONVERT_SET_NOROLE("color4d[]", std::vector<value::double4>)

  // TexCoord role type arrays
  TRY_CONVERT_SET_NOROLE("texCoord2h[]", std::vector<value::half2>)
  TRY_CONVERT_SET_NOROLE("texCoord2f[]", std::vector<value::float2>)
  TRY_CONVERT_SET_NOROLE("texCoord2d[]", std::vector<value::double2>)
  TRY_CONVERT_SET_NOROLE("texCoord3h[]", std::vector<value::half3>)
  TRY_CONVERT_SET_NOROLE("texCoord3f[]", std::vector<value::float3>)
  TRY_CONVERT_SET_NOROLE("texCoord3d[]", std::vector<value::double3>)

  TRY_CONVERT_SET("float2[]", std::vector<value::float2>)

  // TexCoord scalars
  TRY_CONVERT_SET_NOROLE("texCoord2h", value::half2)
  TRY_CONVERT_SET_NOROLE("texCoord2f", value::float2)
  TRY_CONVERT_SET_NOROLE("texCoord2d", value::double2)
  TRY_CONVERT_SET_NOROLE("texCoord3h", value::half3)
  TRY_CONVERT_SET_NOROLE("texCoord3f", value::float3)
  TRY_CONVERT_SET_NOROLE("texCoord3d", value::double3)
  TRY_CONVERT_SET("token[]", std::vector<value::token>)
  TRY_CONVERT_SET("int64[]", std::vector<int64_t>)
  TRY_CONVERT_SET("uint64[]", std::vector<uint64_t>)
  TRY_CONVERT_SET("quath[]", std::vector<value::quath>)
  TRY_CONVERT_SET("quatf[]", std::vector<value::quatf>)
  TRY_CONVERT_SET("quatd[]", std::vector<value::quatd>)
  TRY_CONVERT_SET("half[]", std::vector<value::half>)
  TRY_CONVERT_SET("half2[]", std::vector<value::half2>)
  TRY_CONVERT_SET("half3[]", std::vector<value::half3>)
  TRY_CONVERT_SET("half4[]", std::vector<value::half4>)
  TRY_CONVERT_SET("float4[]", std::vector<value::float4>)
  TRY_CONVERT_SET("double4[]", std::vector<value::double4>)
  TRY_CONVERT_SET("int2[]", std::vector<value::int2>)
  TRY_CONVERT_SET("int3[]", std::vector<value::int3>)
  TRY_CONVERT_SET("int4[]", std::vector<value::int4>)
  TRY_CONVERT_UNREG("matrix2f[]", std::vector<value::matrix2f>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("matrix3f[]", std::vector<value::matrix3f>, FormatUnregisteredArray)
  TRY_CONVERT_UNREG("matrix4f[]", std::vector<value::matrix4f>, FormatUnregisteredArray)
  TRY_CONVERT_SET("matrix4d[]", std::vector<value::matrix4d>)
  TRY_CONVERT_SET("matrix2d[]", std::vector<value::matrix2d>)
  TRY_CONVERT_SET("matrix3d[]", std::vector<value::matrix3d>)
  // frame4d[] has no direct Crate representation; store as the equivalent
  // matrix4d[]. Bespoke (loop body), not a TRY_CONVERT_* one-liner.
  if (type_name == "frame4d[]") {
    if (auto v = val.get_value<std::vector<value::frame4d>>(false)) {
      std::vector<value::matrix4d> mats;
      mats.reserve(v->size());
      for (const auto &frame : *v) {
        value::matrix4d mat;
        for (size_t row = 0; row < 4; ++row) {
          for (size_t col = 0; col < 4; ++col) {
            mat.m[row][col] = frame.m[row][col];
          }
        }
        mats.emplace_back(std::move(mat));
      }
      out.Set(std::move(mats));
      return true;
    }
  }

  // Half-vector arrays
  //
  // NOTE: half2[]/half3[]/half4[] (and int2[]/int3[]/int4[] and bool[]
  // below) are also matched above/elsewhere in this function. In the
  // original else-if chain this made the repeat below unreachable dead
  // code (once a type_name matched a branch in the chain, later branches
  // were never evaluated, so a get_value() failure just fell through to
  // "Type not yet supported"); as standalone ifs these are now technically
  // reachable, but since they perform the exact same check + conversion
  // as their earlier occurrence, this changes no observable behavior --
  // preserved as-is rather than silently deduplicating during an otherwise
  // purely mechanical transform.
  TRY_CONVERT_SET("half2[]", std::vector<value::half2>)
  TRY_CONVERT_SET("half3[]", std::vector<value::half3>)
  TRY_CONVERT_SET("half4[]", std::vector<value::half4>)

  // Int-vector arrays (see dead-code NOTE above)
  TRY_CONVERT_SET("int2[]", std::vector<value::int2>)
  TRY_CONVERT_SET("int3[]", std::vector<value::int3>)
  TRY_CONVERT_SET("int4[]", std::vector<value::int4>)
  TRY_CONVERT_SET("bool[]", std::vector<bool>)

#undef TRY_CONVERT_SET
#undef TRY_CONVERT_SET_NOROLE
#undef TRY_CONVERT_UNREG

  // Type not yet supported
  if (err) {
    *err = "Value conversion not yet implemented for type: " + type_name;
  }
  return false;
}

bool CrateWriter::ConvertPropertyToFields(
    const std::string& prop_name,
    const Property& prop,
    const Path& parent_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {

  // Dispatch based on property type. A typed attribute with no default,
  // timeSamples, or connections is still an authored declaration, e.g.
  // shader terminal outputs, so handle attributes before the generic empty
  // check.
  if (prop.is_attribute()) {
    // Convert attribute - creates separate spec, doesn't add to fields
    // Pass the custom flag to be added to the attribute spec
    return ConvertAttributeToFields(prop_name, prop.get_attribute(), parent_path, prop.has_custom(), err);
  } else if (prop.is_relationship()) {
    // Convert relationship - creates separate spec, doesn't add to fields.
    // The list-edit qualifier sits on the outer Property, not on the
    // Relationship itself; copy it across so the writer encodes the
    // ListOp<Path> with the matching prepended/appended/... flag.
    Relationship rel_copy = prop.get_relationship();
    if (rel_copy.get_listedit_qual() == ListEditQual::ResetToExplicit &&
        prop.get_listedit_qual() != ListEditQual::ResetToExplicit) {
      rel_copy.set_listedit_qual(prop.get_listedit_qual());
    }
    return ConvertRelationshipToFields(prop_name, rel_copy, parent_path, err,
                                       prop.has_custom());
  } else if (prop.is_attribute_connection()) {
    // Convert connection - creates separate spec, doesn't add to fields
    return ConvertConnectionToFields(prop_name, prop.get_attribute(), parent_path, err);
  } else if (prop.is_empty()) {
    // Empty property - skip
    return true;
  } else {
    if (err) *err = "Unknown property type for: " + prop_name;
    return false;
  }
}

bool CrateWriter::ConvertAttributeToFields(
    const std::string& attr_name,
    const Attribute& attr,
    const Path& parent_path,
    bool is_custom,
    std::string* err) {

  // Create separate spec for this attribute (proper USD Crate format)
  // Attributes are property specs, not prim fields

  crate::FieldValuePairVector attr_fields;

  // Create the attribute path
  Path attr_path = parent_path.AppendProperty(attr_name);

  DCOUT("[ConvertAttributeToFields] Creating separate spec for attribute: "
            << attr_path.full_path_name());

  // 1. Add type name - store as value::token, not as token index!
  if (!attr.type_name().empty()) {
    crate::CrateValue type_value;
    value::token tok(attr.type_name());
    type_value.Set(tok);  // Store the token, not the index!
    attr_fields.push_back({"typeName", type_value});
  }

  // 2. Extract value from PrimVar
  const primvar::PrimVar& pvar = attr.get_var();

  // 2a. Check for blocked value
  if (pvar.is_blocked()) {
    // Add a ValueBlock indicator
    crate::CrateValue blocked_value;
    blocked_value.Set(value::ValueBlock());
    attr_fields.push_back({"default", blocked_value});
  }
  // 2b. Extract default value
  else if (pvar.has_value()) {
    const value::Value& val = pvar.value_raw();
    crate::CrateValue crate_val;

    if (attr.type_name() == "timecode") {
      if (auto tc = val.get_value<value::timecode>()) {
        crate_val.Set(*tc);
      } else if (auto d = val.get_value<double>()) {
        crate_val.Set(value::timecode{*d});
      } else {
        if (err) *err = "Failed to convert timecode attribute value: " + attr_name;
        return false;
      }
    } else if (attr.type_name() == "timecode[]") {
      if (auto tc = val.get_value<std::vector<value::timecode>>()) {
        crate_val.Set(*tc);
      } else if (auto ds = val.get_value<std::vector<double>>()) {
        std::vector<value::timecode> tcs;
        tcs.reserve(ds->size());
        for (double d : *ds) {
          tcs.push_back(value::timecode{d});
        }
        crate_val.Set(std::move(tcs));
      } else {
        if (err) *err = "Failed to convert timecode[] attribute value: " + attr_name;
        return false;
      }
    } else if (!ConvertValue(val, crate_val, err)) {
      if (err) *err = "Failed to convert attribute value: " + attr_name;
      return false;
    }

    attr_fields.push_back({"default", crate_val});
  }

  // 2c. Extract time samples
  if (pvar.has_timesamples()) {
    // Convert TimeSamples to CrateValue
    const value::TimeSamples& ts = pvar.ts_raw();

    // Create a CrateValue with TimeSamples
    crate::CrateValue ts_crate_val;
    ts_crate_val.Set(ts);

    // Add the timeSamples field
    attr_fields.push_back({"timeSamples", ts_crate_val});

    DCOUT("[ConvertAttributeToFields] Added TimeSamples for " << attr_name
              << " with " << ts.size() << " samples");
  }

  // 2d. Extract spline (AOUSD Core Spec 7.4.2.4; Crate type 59).
  if (pvar.has_spline()) {
    crate::CrateValue spline_crate_val;
    spline_crate_val.Set(pvar.spline_data());
    attr_fields.push_back({"spline", spline_crate_val});
    DCOUT("[ConvertAttributeToFields] Added Spline for " << attr_name);
  }

  // 3. Add variability if not default - store as Variability enum, not token index
  if (attr.variability() != Variability::Varying) {
    crate::CrateValue var_value;
    var_value.Set(attr.variability());
    attr_fields.push_back({"variability", var_value});
  }

  // 4. Add metadata from AttrMetas
  const AttrMetas& metas = attr.metas();

  // Add interpolation if specified - store as value::token, not token index
  if (metas.has_interpolation()) {
    crate::CrateValue interp_value;
    interp_value.Set(metas.get_interpolation());
    attr_fields.push_back({"interpolation", interp_value});
  }

  // Add hidden if specified
  if (metas.has_hidden()) {
    crate::CrateValue hidden_value;
    hidden_value.Set(metas.get_hidden());
    attr_fields.push_back({"hidden", hidden_value});
  }

  // Add customData if present
  if (metas.has_customData()) {
    crate::CrateValue custom_data_value;
    custom_data_value.Set(metas.get_customData());
    attr_fields.push_back({"customData", custom_data_value});
    DCOUT("[ConvertAttributeToFields] Added customData for " << attr_name
              << " with " << metas.get_customData().size() << " entries");
  }

  // Add custom flag if attribute is custom
  if (is_custom) {
    crate::CrateValue custom_value;
    custom_value.Set(true);  // The field name "custom" with value true indicates custom attribute
    attr_fields.push_back({"custom", custom_value});
    DCOUT("[ConvertAttributeToFields] Added custom flag for " << attr_name);
  }

  // Add displayName / displayGroup / documentation attribute metas
  if (metas.has_displayName()) {
    crate::CrateValue v;
    v.Set(metas.get_displayName());
    attr_fields.push_back({"displayName", v});
  }
  if (metas.has_displayGroup()) {
    crate::CrateValue v;
    v.Set(metas.get_displayGroup());
    attr_fields.push_back({"displayGroup", v});
  }
  if (metas.has_doc()) {
    crate::CrateValue v;
    v.Set(metas.get_doc().value);
    attr_fields.push_back({"documentation", v});
  }
  if (metas.has_allowedTokens()) {
    crate::CrateValue v;
    v.Set(metas.get_allowedTokens());
    attr_fields.push_back({"allowedTokens", v});
  }
  if (metas.has_colorSpace()) {
    crate::CrateValue v;
    v.Set(metas.get_colorSpace());
    attr_fields.push_back({"colorSpace", v});
  }
  if (metas.has_elementSize()) {
    // pxr crate stores elementSize as int (signed); AttrMetas exposes
    // it as uint32_t. Cast on the way out so the reader's int-typed
    // unpack succeeds.
    crate::CrateValue v;
    v.Set(static_cast<int32_t>(metas.get_elementSize()));
    attr_fields.push_back({"elementSize", v});
  }
  if (metas.has_weight()) {
    // pxr crate stores BlendShape inbetween `weight` as float; our
    // AttrMetas keeps it as double. Cast to float on write so the
    // reader's float-typed unpack matches the pxr canonical.
    crate::CrateValue v;
    v.Set(static_cast<float>(metas.get_weight()));
    attr_fields.push_back({"weight", v});
  }
  // A bare string in an attribute's metadata block IS the comment in USD -- the
  // two ASCII spellings are one Sdf field, and only the ASCII parser knows which
  // was used (it parks the bare form in AttrMeta::stringData). Both go out as
  // the STANDARD `comment` field rather than a private one pxr could not read.
  // Emit exactly ONE: two `comment` fields would corrupt the fieldset encoding.
  if (metas.has_comment() || !metas.stringData.empty()) {
    std::string comment_str;
    if (metas.has_comment()) {
      comment_str = metas.get_comment().value;
    } else {
      for (size_t i = 0; i < metas.stringData.size(); i++) {
        if (i > 0) comment_str += "\n";
        comment_str += metas.stringData[i].value;
      }
    }
    crate::CrateValue v;
    v.Set(comment_str);
    attr_fields.push_back({"comment", v});
  }
  if (metas.has_bindMaterialAs()) {
    crate::CrateValue v;
    v.Set(metas.get_bindMaterialAs());
    attr_fields.push_back({"bindMaterialAs", v});
  }
  if (metas.has_outputName()) {
    crate::CrateValue v;
    v.Set(metas.get_outputName());
    attr_fields.push_back({"outputName", v});
  }
  if (metas.has_connectability()) {
    crate::CrateValue v;
    v.Set(metas.get_connectability());
    attr_fields.push_back({"connectability", v});
  }
  if (metas.has_renderType()) {
    crate::CrateValue v;
    v.Set(metas.get_renderType());
    attr_fields.push_back({"renderType", v});
  }
  if (metas.has_sdrMetadata()) {
    crate::CrateValue v;
    v.Set(metas.get_sdrMetadata());
    attr_fields.push_back({"sdrMetadata", v});
  }
  if (metas.has_unauthoredValuesIndex()) {
    crate::CrateValue v;
    v.Set(metas.get_unauthoredValuesIndex());
    attr_fields.push_back({"unauthoredValuesIndex", v});
  }

  // static: built from literals once, not per call (per-attribute hot path).
  static const std::unordered_set<std::string> emitted_meta_names = {
      "allowedTokens",
      "bindMaterialAs",
      "colorSpace",
      "comment",
      "connectability",
      "customData",
      "displayGroup",
      "displayName",
      "documentation",
      "elementSize",
      "hidden",
      "interpolation",
      "outputName",
      "renderType",
      "sdrMetadata",
      "unauthoredValuesIndex",
      "weight"};
  for (const auto& kv : metas.data()) {
    if (emitted_meta_names.count(kv.first)) {
      continue;
    }
    crate::CrateValue v;
    std::string meta_err;
    if (!ConvertValue(kv.second.get_raw_value(), v, &meta_err)) {
      DCOUT("WARNING: Skipping unsupported attribute metadata `" << kv.first
            << "` on " << attr_name << ": " << meta_err);
      continue;
    }
    attr_fields.push_back({kv.first, v});
  }

  // Unregistered property metadata: emit as the crate UNREGISTERED_VALUE type
  // (what pxr writes for SdfUnregisteredValue), carrying the raw USDA text.
  for (const auto& kv : metas.unregisteredMetas) {
    crate::CrateValue v;
    v.SetUnregisteredValueString(kv.second);
    attr_fields.push_back({kv.first, v});
  }

  // Add attribute connection paths if any.
  if (attr.has_connections()) {
    ListOp<Path> connection_paths_listop;
    connection_paths_listop.ClearAndMakeExplicit();
    connection_paths_listop.SetExplicitItems(attr.connections());
    crate::CrateValue conn_value;
    conn_value.Set(connection_paths_listop);
    attr_fields.push_back({"connectionPaths", conn_value});
    DCOUT("[ConvertAttributeToFields] Added " << attr.connections().size()
          << " connectionPaths for " << attr_name);
  }

  // Create the attribute spec
  if (!AddSpec(attr_path, SpecType::Attribute, std::move(attr_fields), err)) {
    if (err) *err = "Failed to add attribute spec: " + attr_path.full_path_name() + ": " + *err;
    return false;
  }

  DCOUT("[ConvertAttributeToFields] Successfully created attribute spec with "
            << attr_fields.size() << " fields");

  return true;
}

bool CrateWriter::ConvertRelationshipToFields(
    const std::string& rel_name,
    const Relationship& rel,
    const Path& parent_path,
    std::string* err,
    bool is_custom) {

  // Create separate spec for this relationship (proper USD Crate format)
  // Relationships are property specs, not prim fields

  crate::FieldValuePairVector rel_fields;

  // Create the relationship path
  Path rel_path = parent_path.AppendProperty(rel_name);

  DCOUT("[ConvertRelationshipToFields] Creating separate spec for relationship: "
            << rel_path.full_path_name());

  // 1. Check relationship type and add targetPaths.
  //
  // pxr Crate stores the list-edit qualifier on the ListOp itself
  // (via SetPrependedItems / SetAppendedItems / SetDeletedItems /
  // SetOrderedItems), not as a separate sibling field. Match that
  // pattern so pxr usdcat (and our own reader) can recover the
  // qualifier on round-trip.
  auto apply_listed_qual = [&](ListOp<Path>& listop,
                                const std::vector<Path>& targets) {
    switch (rel.get_listedit_qual()) {
      case ListEditQual::Append:
        listop.SetAppendedItems(targets);
        break;
      case ListEditQual::Prepend:
        listop.SetPrependedItems(targets);
        break;
      case ListEditQual::Add:
        listop.SetAddedItems(targets);
        break;
      case ListEditQual::Delete:
        listop.SetDeletedItems(targets);
        break;
      case ListEditQual::Order:
        listop.SetOrderedItems(targets);
        break;
      case ListEditQual::ResetToExplicit:
      default:
        listop.SetExplicitItems(targets);
        break;
    }
  };

  if (rel.is_blocked()) {
    // Add ValueBlock for the relationship
    crate::CrateValue blocked_value;
    blocked_value.Set(value::ValueBlock());
    rel_fields.push_back({"targetPaths", blocked_value});
  } else if (rel.is_path()) {
    crate::CrateValue path_value;
    ListOp<Path> listop;
    std::vector<Path> targets{rel.targetPath};
    apply_listed_qual(listop, targets);
    path_value.Set(listop);
    rel_fields.push_back({"targetPaths", path_value});
  } else if (rel.is_pathvector()) {
    crate::CrateValue paths_value;
    ListOp<Path> listop;
    apply_listed_qual(listop, rel.targetPathVector);
    paths_value.Set(listop);
    rel_fields.push_back({"targetPaths", paths_value});
  } else if (rel.get_listedit_qual() != ListEditQual::ResetToExplicit) {
    // A list-edit qualifier on a relationship with NO targets at all
    // (`append rel myval`, `delete rel myheight`). The qualifier is the only
    // thing authored, and it lives on the ListOp -- so emit a targetPaths ListOp
    // whose qualifier bucket is authored but EMPTY. ListOp records that
    // faithfully now (see ListOp::Has*Items), and the crate format has always had
    // a presence bit per bucket, so this needs no format change.
    crate::CrateValue empty_value;
    ListOp<Path> listop;
    apply_listed_qual(listop, std::vector<Path>{});
    empty_value.Set(listop);
    rel_fields.push_back({"targetPaths", empty_value});
  }
  // A DefineOnly relationship with no qualifier (`rel myrel`) adds no
  // targetPaths field: there is nothing to record.

  // 3. Relationships are uniform by convention, but USD still allows (and the
  // ascii/crate readers both preserve, via Relationship::is_varying_authored)
  // an explicitly-authored `varying rel`. Hardcoding Uniform here silently
  // dropped that authored opinion on every round-trip.
  crate::CrateValue var_value;
  Variability var = rel.is_varying_authored() ? Variability::Varying
                                              : Variability::Uniform;
  var_value.Set(var);
  rel_fields.push_back({"variability", var_value});

  // `custom` lives on the enclosing Property, not the Relationship itself
  // (mirrors ConvertAttributeToFields's `is_custom` parameter); callers that
  // have no Property wrapper (e.g. Collection includes/excludes) pass false.
  if (is_custom) {
    crate::CrateValue custom_value;
    custom_value.Set(true);
    rel_fields.push_back({"custom", custom_value});
  }

  // 4. Add relationship metadata from AttrMetas
  const AttrMeta& metas = rel.metas();

  // Add comment if present
  if (metas.has_comment()) {
    crate::CrateValue comment_value;
    comment_value.Set(metas.get_comment().value);  // StringData.value
    rel_fields.push_back({"comment", comment_value});
  }

  // Add hidden if present
  if (metas.has_hidden()) {
    crate::CrateValue hidden_value;
    hidden_value.Set(metas.get_hidden());
    rel_fields.push_back({"hidden", hidden_value});
  }

  // Add bindMaterialAs if present. Written for Attribute metadata
  // (ConvertAttributeToFields) but never for Relationship metadata, even
  // though `bindMaterialAs` is authored on RELATIONSHIPS (e.g.
  // `material:binding`) far more often than on attributes -- every
  // `strongerThanDescendants`/`weakerThanDescendants` qualifier on a
  // material binding was silently dropped on USDC round-trip.
  if (metas.has_bindMaterialAs()) {
    crate::CrateValue bind_material_as_value;
    bind_material_as_value.Set(metas.get_bindMaterialAs());
    rel_fields.push_back({"bindMaterialAs", bind_material_as_value});
  }

  // Add customData if present
  if (metas.has_customData()) {
    crate::CrateValue custom_data_value;
    custom_data_value.Set(metas.get_customData());
    rel_fields.push_back({"customData", custom_data_value});
    DCOUT("[ConvertRelationshipToFields] Added customData for " << rel_name
              << " with " << metas.get_customData().size() << " entries");
  }

  // Add displayName if present
  if (metas.has_displayName()) {
    crate::CrateValue display_name_value;
    display_name_value.Set(metas.get_displayName());
    rel_fields.push_back({"displayName", display_name_value});
  }

  // Add displayGroup if present
  if (metas.has_displayGroup()) {
    crate::CrateValue display_group_value;
    display_group_value.Set(metas.get_displayGroup());
    rel_fields.push_back({"displayGroup", display_group_value});
  }

  // Unregistered property metadata: emit as the crate UNREGISTERED_VALUE type
  // (what pxr writes for SdfUnregisteredValue), carrying the raw USDA text.
  for (const auto& kv : metas.unregisteredMetas) {
    crate::CrateValue v;
    v.SetUnregisteredValueString(kv.second);
    rel_fields.push_back({kv.first, v});
  }

  // Create the relationship spec
  if (!AddSpec(rel_path, SpecType::Relationship, std::move(rel_fields), err)) {
    if (err) *err = "Failed to add relationship spec: " + rel_path.full_path_name() + ": " + *err;
    return false;
  }

  DCOUT("[ConvertRelationshipToFields] Successfully created relationship spec with "
            << rel_fields.size() << " fields");

  return true;
}

bool CrateWriter::ConvertConnectionToFields(
    const std::string& conn_name,
    const Attribute& attr,
    const Path& parent_path,
    std::string* err) {

  // Create separate spec for this connection (proper USD Crate format)
  // Connections are specs at path: parent.AppendProperty(attr_name).AppendProperty("connect")

  // Attribute connections represent ".connect" relationships
  if (!attr.has_connections()) {
    // No connections to process
    return true;
  }

  crate::FieldValuePairVector conn_fields;

  // Create the connection path
  // Connection path is: /Prim.attribute.connect
  Path conn_path = parent_path.AppendProperty(conn_name).AppendProperty("connect");

  DCOUT("[ConvertConnectionToFields] Creating separate spec for connection: "
            << conn_path.full_path_name());

  // 1. Add connection paths (targets)
  const std::vector<Path>& conn_paths = attr.connections();

  if (conn_paths.size() == 1) {
    // Single connection path - wrap in a vector since CrateValue doesn't have Set(Path)
    crate::CrateValue conn_value;
    std::vector<Path> wrapped_path;
    wrapped_path.push_back(conn_paths[0]);
    conn_value.Set(wrapped_path);
    conn_fields.push_back({"targetPaths", conn_value});
  } else if (conn_paths.size() > 1) {
    // Multiple connection paths
    crate::CrateValue conns_value;
    std::vector<Path> paths_copy = conn_paths;
    conns_value.Set(paths_copy);
    conn_fields.push_back({"targetPaths", conns_value});
  }

  // 2. Add type name for the connection (same as attribute type) - store as token!
  if (!attr.type_name().empty()) {
    crate::CrateValue type_value;
    value::token tok(attr.type_name());
    type_value.Set(tok);  // Store the token, not the index!
    conn_fields.push_back({"typeName", type_value});
  }

  // 3. Create the connection spec
  if (!AddSpec(conn_path, SpecType::Connection, std::move(conn_fields), err)) {
    if (err) *err = "Failed to add connection spec: " + conn_path.full_path_name() + ": " + *err;
    return false;
  }

  DCOUT("[ConvertConnectionToFields] Successfully created connection spec with "
            << conn_fields.size() << " fields");

  return true;
}

// ============================================================================
// Variant Set and Variant Conversion (USD Composition)
// ============================================================================

bool CrateWriter::ConvertVariantSetToFields(
    const std::string& variantset_name,
    const VariantSet& variantset,
    const Path& parent_path,
    std::string* err,
    int depth) {
  if (depth > 512) {
    if (err) *err = "VariantSet nesting too deep (>512).";
    return false;
  }

  // VariantSet path: parent{variantSetName} (e.g., /Chair{materialVariant})
  // pxr's SdfPath has no bare `{set}` form: a VariantSet spec lives at the
  // EMPTY variant selection, `{set=}` (see any pxr-written crate). Our old
  // `{set}` spelling made OpenUSD reject the element outright.
  std::string variantset_path_str = parent_path.prim_part() + "{" + variantset_name + "=}";
  Path vs_path(variantset_path_str, "");

  DCOUT("[ConvertVariantSetToFields] Creating VariantSet spec: "
            << vs_path.full_path_name());

  // VariantSet spec needs "variantChildren" field listing all variant names
  crate::FieldValuePairVector vs_fields;

  std::vector<value::token> variant_tokens;
  for (const auto& variant_item : variantset.variantSet) {
    variant_tokens.push_back(value::token(variant_item.first));
  }

  crate::CrateValue variant_children_value;
  variant_children_value.Set(variant_tokens);
  vs_fields.push_back({"variantChildren", variant_children_value});

  if (!AddSpec(vs_path, SpecType::VariantSet, std::move(vs_fields), err)) {
    if (err) *err = "Failed to add VariantSet spec: " + vs_path.full_path_name() + ": " + *err;
    return false;
  }

  DCOUT("[ConvertVariantSetToFields] Created VariantSet with "
            << variantset.variantSet.size() << " variants");

  // For each variant, create a Variant spec
  // IMPORTANT: Pass parent_path (the Prim), not vs_path (the VariantSet)
  // Variant path should be /Prim{variantSet=value}, NOT /Prim{variantSet}{variantSet=value}
  for (const auto& variant_item : variantset.variantSet) {
    const auto& variant_name = variant_item.first;
    const auto& variant_data = variant_item.second;

    if (!ConvertVariantToFields(variant_name, variant_data, parent_path, variantset_name, err, depth + 1)) {
      if (err) *err = "Failed to convert variant '" + variant_name + "': " + *err;
      return false;
    }
  }

  return true;
}

bool CrateWriter::ConvertVariantToFields(
    const std::string& variant_name,
    const Variant& variant,
    const Path& parent_prim_path,  // The prim that owns the variantSet (e.g., /Chair)
    const std::string& variantset_name,
    std::string* err,
    int depth) {

  // Variant path: parent_prim_path{variantSetName=variant_name}
  // (e.g., /Chair{materialVariant=plastic})
  // NOTE: The variant path is based on the prim path, NOT the variantSet path
  std::string variant_path_str = parent_prim_path.prim_part() + "{" +
                                 variantset_name + "=" + variant_name + "}";
  Path v_path(variant_path_str, "");

  DCOUT("[ConvertVariantToFields] Creating Variant spec: "
            << v_path.full_path_name());

  // Variant spec contains variant metadata and prim children
  crate::FieldValuePairVector v_fields;

  // Add variant specifier (always "def" for variants)
  crate::CrateValue spec_value;
  spec_value.Set(Specifier::Def);
  v_fields.push_back({"specifier", spec_value});

  // A variant statement carries its own Prim metadata block -- `active`,
  // `hidden`, `kind`, and (for a nested variantSet) `variantSets`. It is a
  // full PrimMeta, populated by the readers just like a Prim's, so reuse the
  // same extractor rather than hand-rolling a subset here.
  ExtractPrimMeta(variant.metas(), v_fields);

  // Add variant properties as separate attribute specs
  for (const auto& prop_item : variant.properties()) {
    const auto& prop_name = prop_item.first;
    const auto& prop = prop_item.second;

    // Handle variant properties based on their type
    if (prop.is_attribute()) {
      if (!ConvertAttributeToFields(prop_name, prop.get_attribute(), v_path, prop.has_custom(), err)) {
        if (err) *err = "Failed to convert variant attribute: " + prop_name;
        return false;
      }
    } else if (prop.is_relationship()) {
      if (!ConvertRelationshipToFields(prop_name, prop.get_relationship(), v_path, err,
                                       prop.has_custom())) {
        if (err) *err = "Failed to convert variant relationship: " + prop_name;
        return false;
      }
    }
  }

  // "primChildren" on the VARIANT spec: pxr descends from the variant to the
  // prims defined inside it through this field, exactly as it does from a
  // regular prim. (Our reader walks spec paths instead, so round-trips never
  // missed it.)
  {
    const auto& vchildren = variant.primChildren();
    if (!vchildren.empty()) {
      std::vector<value::token> child_tokens;
      for (const auto& child_prim : vchildren) {
        child_tokens.push_back(value::token(child_prim.element_name()));
      }
      crate::CrateValue children_value;
      children_value.Set(child_tokens);
      v_fields.push_back({"primChildren", children_value});
    }
  }

  // IMPORTANT: Create the variant spec BEFORE adding child prims
  // The parent spec must exist before children in the spec list
  if (!AddSpec(v_path, SpecType::Variant, std::move(v_fields), err)) {
    if (err) *err = "Failed to add Variant spec: " + v_path.full_path_name() + ": " + *err;
    return false;
  }

  DCOUT("[ConvertVariantToFields] Created Variant spec with "
            << v_fields.size() << " fields");

  // Add variant prim children (recursively convert each child prim)
  // Variant prim children are full prims that exist only within this variant
  const auto& child_prims = variant.primChildren();
  if (!child_prims.empty()) {
    DCOUT("[ConvertVariantToFields] Processing " << child_prims.size()
              << " prim children for variant: " << variant_name);
    for (const auto& child_prim : child_prims) {
      // Recursively convert each prim child (they inherit the variant path context)
      if (!ConvertPrimIterative(child_prim, v_path, err)) {
        if (err) *err = "Failed to convert variant prim child: " + child_prim.element_name() + ": " + *err;
        return false;
      }
    }
  }

  // Process nested variant sets
  for (const auto& vs_item : variant.variantSets()) {
    if (!ConvertVariantSetToFields(vs_item.first, vs_item.second, v_path, err, depth + 1)) {
      if (err) *err = "Failed to convert nested VariantSet '" + vs_item.first
                     + "' in variant " + variant_name + ": " + *err;
      return false;
    }
  }

  return true;
}

} // namespace experimental
} // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
