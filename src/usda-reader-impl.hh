// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// USDAReader::Impl + supporting types, extracted from usda-reader.cc so the per-prim-type
// RegisterReconstructCallback<T> / ReconstructPrim<T> instantiations can be split across
// sibling TUs (usda-reader-reconstruct-*.cc). See usda-reader.cc for the rest of the impl.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stack>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

#include "ascii-parser.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "usdGeom.hh"
#include "usdSkel.hh"
#include "usdPhysics.hh"
#include "usdAR.hh"
#include "usdMedia.hh"
#include "mjcPhysics.hh"
#include "usda-reader.hh"
#include "layer.hh"
#include "parser-timing.hh"
#include "enum-handlers.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "nonstd/expected.hpp"
#include "nonstd/optional.hpp"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "io-util.hh"
#include "math-util.inc"
#include "pprint-enum.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "prim-reconstruct.hh"
#include "primvar.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "usdShade.hh"
#include "value-pprint.hh"
#include "value-types.hh"
#include "tiny-format.hh"
#include "common-macros.inc"

namespace lightusd {
namespace prim {

// template specialization forward decls.
// implimentations will be located in prim-reconstruct.cc
#define RECONSTRUCT_PRIM_DECL(__ty) template<> bool ReconstructPrim<__ty>(const Specifier &spec, PropertyMap &, const ReferenceList &, __ty *, std::string *, std::string *, const PrimReconstructOptions &)

RECONSTRUCT_PRIM_DECL(Xform);
RECONSTRUCT_PRIM_DECL(Model);
RECONSTRUCT_PRIM_DECL(Scope);
RECONSTRUCT_PRIM_DECL(Skeleton);
RECONSTRUCT_PRIM_DECL(SkelRoot);
RECONSTRUCT_PRIM_DECL(SkelAnimation);
RECONSTRUCT_PRIM_DECL(BlendShape);
RECONSTRUCT_PRIM_DECL(DomeLight);
RECONSTRUCT_PRIM_DECL(SphereLight);
RECONSTRUCT_PRIM_DECL(CylinderLight);
RECONSTRUCT_PRIM_DECL(DiskLight);
RECONSTRUCT_PRIM_DECL(DistantLight);
RECONSTRUCT_PRIM_DECL(RectLight);
RECONSTRUCT_PRIM_DECL(GeometryLight);
RECONSTRUCT_PRIM_DECL(PortalLight);
RECONSTRUCT_PRIM_DECL(DomeLight_1);
RECONSTRUCT_PRIM_DECL(LightFilter);
RECONSTRUCT_PRIM_DECL(PluginLightFilter);
RECONSTRUCT_PRIM_DECL(GPrim);
RECONSTRUCT_PRIM_DECL(GeomMesh);
RECONSTRUCT_PRIM_DECL(GeomSubset);
RECONSTRUCT_PRIM_DECL(GeomSphere);
RECONSTRUCT_PRIM_DECL(Volume);
RECONSTRUCT_PRIM_DECL(VolumeFieldBase);
RECONSTRUCT_PRIM_DECL(FieldBase);
RECONSTRUCT_PRIM_DECL(VolumeFieldAsset);
RECONSTRUCT_PRIM_DECL(FieldAsset);
RECONSTRUCT_PRIM_DECL(OpenVDBAsset);
RECONSTRUCT_PRIM_DECL(Field3DAsset);
RECONSTRUCT_PRIM_DECL(GeomPoints);
RECONSTRUCT_PRIM_DECL(GeomCone);
RECONSTRUCT_PRIM_DECL(GeomCube);
RECONSTRUCT_PRIM_DECL(GeomCylinder);
RECONSTRUCT_PRIM_DECL(GeomCapsule);
RECONSTRUCT_PRIM_DECL(GeomBasisCurves);
RECONSTRUCT_PRIM_DECL(GeomNurbsCurves);
RECONSTRUCT_PRIM_DECL(GeomPlane);
RECONSTRUCT_PRIM_DECL(GeomCylinder_1);
RECONSTRUCT_PRIM_DECL(GeomCapsule_1);
RECONSTRUCT_PRIM_DECL(GeomTetMesh);
RECONSTRUCT_PRIM_DECL(GeomNurbsPatch);
RECONSTRUCT_PRIM_DECL(GeomHermiteCurves);
RECONSTRUCT_PRIM_DECL(GeomCamera);
RECONSTRUCT_PRIM_DECL(GeomPointInstancer);
RECONSTRUCT_PRIM_DECL(Material);
RECONSTRUCT_PRIM_DECL(Shader);
RECONSTRUCT_PRIM_DECL(NodeGraph);
// Placeholder schema prim types (UsdVol / UsdRender / UsdProc)
RECONSTRUCT_PRIM_DECL(Volume);
RECONSTRUCT_PRIM_DECL(OpenVDBAsset);
RECONSTRUCT_PRIM_DECL(Field3DAsset);
RECONSTRUCT_PRIM_DECL(RenderSettings);
RECONSTRUCT_PRIM_DECL(RenderProduct);
RECONSTRUCT_PRIM_DECL(RenderVar);
RECONSTRUCT_PRIM_DECL(GenerativeProcedural);
// UsdPhysics + mjcPhysics
RECONSTRUCT_PRIM_DECL(PhysicsJoint);
RECONSTRUCT_PRIM_DECL(PhysicsScene);
RECONSTRUCT_PRIM_DECL(PhysicsRevoluteJoint);
RECONSTRUCT_PRIM_DECL(PhysicsPrismaticJoint);
RECONSTRUCT_PRIM_DECL(PhysicsSphericalJoint);
RECONSTRUCT_PRIM_DECL(PhysicsFixedJoint);
RECONSTRUCT_PRIM_DECL(PhysicsDistanceJoint);
RECONSTRUCT_PRIM_DECL(PhysicsCollisionGroup);
RECONSTRUCT_PRIM_DECL(MjcActuator);
RECONSTRUCT_PRIM_DECL(NewtonActuator);
RECONSTRUCT_PRIM_DECL(MjcTendon);
RECONSTRUCT_PRIM_DECL(MjcKeyframe);
RECONSTRUCT_PRIM_DECL(MjcSensor);
// AR/Interactive (Apple Preliminary_*)
RECONSTRUCT_PRIM_DECL(Preliminary_PhysicsGravitationalForce);
RECONSTRUCT_PRIM_DECL(Preliminary_InfiniteColliderPlane);
RECONSTRUCT_PRIM_DECL(Preliminary_ReferenceImage);
RECONSTRUCT_PRIM_DECL(Preliminary_Behavior);
RECONSTRUCT_PRIM_DECL(Preliminary_Trigger);
RECONSTRUCT_PRIM_DECL(Preliminary_Action);
RECONSTRUCT_PRIM_DECL(Preliminary_Text);
// usdMedia
RECONSTRUCT_PRIM_DECL(SpatialAudio);

#undef RECONSTRUCT_PRIM_DECL

} // namespace prim

namespace usda {

constexpr auto kTag = "[USDA]";  // moved from usda-reader.cc

// Supporting types for USDAReader::Impl. NOTE: originally TU-local (anonymous namespace);
// promoted to named lightusd::usda for use across the split reconstruct TUs.
struct VariantNode {
  PrimMeta metas;
  std::map<std::string, Property> props;
  std::vector<int64_t> primChildren;
  int64_t variantPrimIdx{-1};
  std::map<std::string, std::map<std::string, VariantNode>> variantSets;
};

struct PrimNode {
  value::Value prim; // stores typed Prim value. Xform, GeomMesh, ...
  std::string elementName;
  std::string typeName; // Prim's typeName
  Specifier specifier{Specifier::Def}; // `def`, `over` or `class`

  int64_t parent{-1};            // -1 = root node
  //bool parent_is_variant{false}; // True when this Prim is defined under variantSet stmt.
  std::vector<size_t> children;  // index to USDAReader._prims[] of childPrims. it contains variant's primChildren also.

  std::map<std::string, std::map<std::string, VariantNode>> variantNodeMap;
};

// For USD scene read for composition(read by references, subLayers, payloads)
struct PrimSpecNode {
  PrimSpec primSpec;

  int64_t parent{-1};            // -1 = root node
  //bool parent_is_variant{false}; // True when this Prim is defined under variantSet stmt.
  std::vector<size_t> children;  // index to USDAReader._primspecs[]

  std::map<std::string, std::map<std::string, VariantNode>> variantNodeMap;
};

// TODO: Move to prim-types.hh?

template <typename T>
struct PrimTypeTraits;

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#endif

#define DEFINE_PRIM_TYPE(__dty, __name, __tyid)    \
  template <>                                      \
  struct PrimTypeTraits<__dty> {                    \
    using primt_type = __dty;                      \
    static constexpr uint32_t type_id = __tyid;    \
    static constexpr auto prim_type_name = __name; \
  }

DEFINE_PRIM_TYPE(Model, "Model", value::TYPE_ID_MODEL);

DEFINE_PRIM_TYPE(Xform, kGeomXform, value::TYPE_ID_GEOM_XFORM);
DEFINE_PRIM_TYPE(Volume, kVolume, value::TYPE_ID_VOLUME);
DEFINE_PRIM_TYPE(VolumeFieldBase, kVolumeFieldBase, value::TYPE_ID_VOLUME_FIELD_BASE);
DEFINE_PRIM_TYPE(FieldBase, kFieldBase, value::TYPE_ID_FIELD_BASE);
DEFINE_PRIM_TYPE(VolumeFieldAsset, kVolumeFieldAsset, value::TYPE_ID_VOLUME_FIELD_ASSET);
DEFINE_PRIM_TYPE(FieldAsset, kFieldAsset, value::TYPE_ID_FIELD_ASSET);
DEFINE_PRIM_TYPE(OpenVDBAsset, kOpenVDBAsset, value::TYPE_ID_OPENVDB_ASSET);
DEFINE_PRIM_TYPE(Field3DAsset, kField3DAsset, value::TYPE_ID_FIELD3D_ASSET);
DEFINE_PRIM_TYPE(GeomMesh, kGeomMesh, value::TYPE_ID_GEOM_MESH);
DEFINE_PRIM_TYPE(GeomPoints, kGeomPoints, value::TYPE_ID_GEOM_POINTS);
DEFINE_PRIM_TYPE(GeomSphere, kGeomSphere, value::TYPE_ID_GEOM_SPHERE);
DEFINE_PRIM_TYPE(GeomCube, kGeomCube, value::TYPE_ID_GEOM_CUBE);
DEFINE_PRIM_TYPE(GeomCone, kGeomCone, value::TYPE_ID_GEOM_CONE);
DEFINE_PRIM_TYPE(GeomCapsule, kGeomCapsule, value::TYPE_ID_GEOM_CAPSULE);
DEFINE_PRIM_TYPE(GeomCylinder, kGeomCylinder, value::TYPE_ID_GEOM_CYLINDER);
DEFINE_PRIM_TYPE(GeomBasisCurves, kGeomBasisCurves,
                 value::TYPE_ID_GEOM_BASIS_CURVES);
DEFINE_PRIM_TYPE(GeomNurbsCurves, kGeomNurbsCurves,
                 value::TYPE_ID_GEOM_NURBS_CURVES);
DEFINE_PRIM_TYPE(GeomPlane, kGeomPlane, value::TYPE_ID_GEOM_PLANE);
DEFINE_PRIM_TYPE(GeomCylinder_1, kGeomCylinder_1, value::TYPE_ID_GEOM_CYLINDER_1);
DEFINE_PRIM_TYPE(GeomCapsule_1, kGeomCapsule_1, value::TYPE_ID_GEOM_CAPSULE_1);
DEFINE_PRIM_TYPE(GeomTetMesh, kGeomTetMesh, value::TYPE_ID_GEOM_TET_MESH);
DEFINE_PRIM_TYPE(GeomNurbsPatch, kGeomNurbsPatch, value::TYPE_ID_GEOM_NURBS_PATCH);
DEFINE_PRIM_TYPE(GeomHermiteCurves, kGeomHermiteCurves, value::TYPE_ID_GEOM_HERMITE_CURVES);
DEFINE_PRIM_TYPE(GeomSubset, kGeomSubset, value::TYPE_ID_GEOM_GEOMSUBSET);
DEFINE_PRIM_TYPE(SphereLight, kSphereLight, value::TYPE_ID_LUX_SPHERE);
DEFINE_PRIM_TYPE(DomeLight, kDomeLight, value::TYPE_ID_LUX_DOME);
DEFINE_PRIM_TYPE(DiskLight, kDiskLight, value::TYPE_ID_LUX_DISK);
DEFINE_PRIM_TYPE(DistantLight, kDistantLight, value::TYPE_ID_LUX_DISTANT);
DEFINE_PRIM_TYPE(CylinderLight, kCylinderLight, value::TYPE_ID_LUX_CYLINDER);
DEFINE_PRIM_TYPE(RectLight, kRectLight, value::TYPE_ID_LUX_RECT);
DEFINE_PRIM_TYPE(GeometryLight, kGeometryLight, value::TYPE_ID_LUX_GEOMETRY);
DEFINE_PRIM_TYPE(PortalLight, kPortalLight, value::TYPE_ID_LUX_PORTAL);
DEFINE_PRIM_TYPE(DomeLight_1, kDomeLight_1, value::TYPE_ID_LUX_DOME_1);
DEFINE_PRIM_TYPE(LightFilter, kLightFilter, value::TYPE_ID_LUX_LIGHT_FILTER);
DEFINE_PRIM_TYPE(PluginLightFilter, kPluginLightFilter, value::TYPE_ID_LUX_PLUGIN_LIGHT_FILTER);
DEFINE_PRIM_TYPE(Material, kMaterial, value::TYPE_ID_MATERIAL);
DEFINE_PRIM_TYPE(Shader, kShader, value::TYPE_ID_SHADER);
DEFINE_PRIM_TYPE(NodeGraph, kNodeGraph, value::TYPE_ID_NODEGRAPH);
DEFINE_PRIM_TYPE(SkelRoot, kSkelRoot, value::TYPE_ID_SKEL_ROOT);
DEFINE_PRIM_TYPE(Skeleton, kSkeleton, value::TYPE_ID_SKELETON);
DEFINE_PRIM_TYPE(SkelAnimation, kSkelAnimation, value::TYPE_ID_SKELANIMATION);
DEFINE_PRIM_TYPE(BlendShape, kBlendShape, value::TYPE_ID_BLENDSHAPE);
DEFINE_PRIM_TYPE(GeomCamera, kGeomCamera, value::TYPE_ID_GEOM_CAMERA);
DEFINE_PRIM_TYPE(GeomPointInstancer, kPointInstancer, value::TYPE_ID_GEOM_POINT_INSTANCER);
// UsdPhysics + mjcPhysics
DEFINE_PRIM_TYPE(PhysicsJoint, kPhysicsJoint, value::TYPE_ID_PHYSICS_JOINT);
DEFINE_PRIM_TYPE(PhysicsScene, kPhysicsScene, value::TYPE_ID_PHYSICS_SCENE);
DEFINE_PRIM_TYPE(PhysicsRevoluteJoint, kPhysicsRevoluteJoint, value::TYPE_ID_PHYSICS_REVOLUTE_JOINT);
DEFINE_PRIM_TYPE(PhysicsPrismaticJoint, kPhysicsPrismaticJoint, value::TYPE_ID_PHYSICS_PRISMATIC_JOINT);
DEFINE_PRIM_TYPE(PhysicsSphericalJoint, kPhysicsSphericalJoint, value::TYPE_ID_PHYSICS_SPHERICAL_JOINT);
DEFINE_PRIM_TYPE(PhysicsFixedJoint, kPhysicsFixedJoint, value::TYPE_ID_PHYSICS_FIXED_JOINT);
DEFINE_PRIM_TYPE(PhysicsDistanceJoint, kPhysicsDistanceJoint, value::TYPE_ID_PHYSICS_DISTANCE_JOINT);
DEFINE_PRIM_TYPE(PhysicsCollisionGroup, kPhysicsCollisionGroup, value::TYPE_ID_PHYSICS_COLLISION_GROUP);
DEFINE_PRIM_TYPE(MjcActuator, kMjcActuator, value::TYPE_ID_MJC_ACTUATOR);
DEFINE_PRIM_TYPE(NewtonActuator, kNewtonActuator, value::TYPE_ID_NEWTON_ACTUATOR);
DEFINE_PRIM_TYPE(MjcTendon, kMjcTendon, value::TYPE_ID_MJC_TENDON);
DEFINE_PRIM_TYPE(MjcKeyframe, kMjcKeyframe, value::TYPE_ID_MJC_KEYFRAME);
DEFINE_PRIM_TYPE(MjcSensor, kMjcSensor, value::TYPE_ID_MJC_SENSOR);
// AR/Interactive (Apple Preliminary_*)
DEFINE_PRIM_TYPE(Preliminary_PhysicsGravitationalForce, kPreliminary_PhysicsGravitationalForce, value::TYPE_ID_PRELIMINARY_GRAVITATIONAL_FORCE);
DEFINE_PRIM_TYPE(Preliminary_InfiniteColliderPlane, kPreliminary_InfiniteColliderPlane, value::TYPE_ID_PRELIMINARY_INFINITE_COLLIDER_PLANE);
DEFINE_PRIM_TYPE(Preliminary_ReferenceImage, kPreliminary_ReferenceImage, value::TYPE_ID_PRELIMINARY_REFERENCE_IMAGE);
DEFINE_PRIM_TYPE(Preliminary_Behavior, kPreliminary_Behavior, value::TYPE_ID_PRELIMINARY_BEHAVIOR);
DEFINE_PRIM_TYPE(Preliminary_Trigger, kPreliminary_Trigger, value::TYPE_ID_PRELIMINARY_TRIGGER);
DEFINE_PRIM_TYPE(Preliminary_Action, kPreliminary_Action, value::TYPE_ID_PRELIMINARY_ACTION);
DEFINE_PRIM_TYPE(Preliminary_Text, kPreliminary_Text, value::TYPE_ID_PRELIMINARY_TEXT);
// usdMedia
DEFINE_PRIM_TYPE(SpatialAudio, kSpatialAudio, value::TYPE_ID_SPATIAL_AUDIO);
DEFINE_PRIM_TYPE(Scope, "Scope", value::TYPE_ID_SCOPE);

// UsdRender / UsdProc placeholder prim types.
DEFINE_PRIM_TYPE(RenderSettings, "RenderSettings", value::TYPE_ID_RENDER_SETTINGS);
DEFINE_PRIM_TYPE(RenderProduct, "RenderProduct", value::TYPE_ID_RENDER_PRODUCT);
DEFINE_PRIM_TYPE(RenderVar, "RenderVar", value::TYPE_ID_RENDER_VAR);
DEFINE_PRIM_TYPE(GenerativeProcedural, "GenerativeProcedural", value::TYPE_ID_GENERATIVE_PROCEDURAL);

DEFINE_PRIM_TYPE(GPrim, "GPrim", value::TYPE_ID_GPRIM);

#ifdef __clang__
#pragma clang diagnostic pop
#endif

class USDAReader::Impl {
 private:
  Stage _stage;

 public:
  Impl(StreamReader *sr) { _parser.SetStream(sr); }


  void SetBaseDir(const std::string &str) { _base_dir = str; }

  void SetFilename(const std::string &str) { _filename = str; }


  void set_reader_config(const USDAReaderConfig &config) {
    _config = config;
    _parser.SetMaxMemoryLimit(config.max_memory_limit_in_mb);
  }

  const USDAReaderConfig get_reader_config() const {
    return _config;
  }

  void SetProgressCallback(std::function<bool(float progress, void *userptr)> callback, void *userptr) {
    _parser.SetProgressCallback(callback, userptr);
  }

  std::string GetCurrentPath() {
    if (_path_stack.empty()) {
      return "/";
    }

    return _path_stack.top();
  }

  bool PathStackDepth() { return _path_stack.size(); }

  void PushPath(const std::string &p) { _path_stack.push(p); }

  void PopPath() {
    if (!_path_stack.empty()) {
      _path_stack.pop();
    }
  }

  void PushError(const std::string &s) {
    if (!_err.empty()) {
      _err += "\n";
    }
    _err += s;
  }

  void PushWarn(const std::string &s) {
    if (!_warn.empty()) {
      _warn += "\n";
    }
    _warn += s;
  }

  template <typename T>
  bool ReconstructPrim(
      const Path &full_path,
      const Specifier &spec,
      prim::PropertyMap &properties,
      const prim::ReferenceList &references,
      T *out);

  // Hoisted out of ReconstructPrim<T>: the T-independent PrimReconstructOptions
  // setup (incl. 4 diagnostic lambdas) so it is instantiated once instead of
  // once per prim type.
  void buildReconstructOptions(const Path &full_path,
                               prim::PrimReconstructOptions &options);

  // Non-template error path for ReconstructPrim<T>, kept out of the template so
  // the template body does not depend on the TU-local AppendPrimPath helper
  // (lets ReconstructPrim<T> be instantiated from sibling TUs). Always returns
  // false.
  bool reportReconstructPrimError(const std::string &type_name,
                                  const Path &full_path,
                                  const std::string &err);

  bool ProcessVariantSetContent(const uint32_t depth, const std::map<std::string, ascii::AsciiParser::VariantSetContent> &in_variants, std::map<std::string, std::map<std::string, VariantNode>> &dst) {
    if (depth > 512) {
      PUSH_ERROR_AND_RETURN("VariantSet nesting too deep (> 512).");
    }

    //
    // variantSet
    // NOTE: variantChildren setup is delayed. It will be processed in ConstructPrimSpecTreeRec
    //
    std::map<std::string, std::map<std::string, VariantNode>> variantSets;
    for (const auto &variantContext : in_variants) {
      const std::string variant_name = variantContext.first;

      DCOUT("variantName: " << variant_name);

      // Convert VariantContent -> VariantNode
      std::map<std::string, VariantNode> variantNodes;
      for (const auto &item : variantContext.second.variantSets) {

        // process child variantSet first.
        std::map<std::string, std::map<std::string, VariantNode>> childVariantSets;
        if (!ProcessVariantSetContent(depth+1, item.second.variantSets, childVariantSets))
        {
          return false;
        }

        VariantNode variant;

        DCOUT("variantPrimIdx = " << variantContext.second.variantPrimIdx);
        variant.variantPrimIdx = variantContext.second.variantPrimIdx;
        DCOUT("child variantSets.size " << childVariantSets.size());
        variant.variantSets = std::move(childVariantSets);

        if (!ReconstructPrimMeta(item.second.metas, &variant.metas)) {
          PUSH_ERROR_AND_RETURN(fmt::format("Failed to process Prim metadataum in variantSet {} item {} ", variant_name, item.first));
        }
        variant.props = item.second.props;

        // child Prim should be already reconstructed.
        for (const auto &childPrimIdx : item.second.primIndices) {
          if (childPrimIdx < 0) {
            PUSH_ERROR_AND_RETURN(fmt::format("[InternalError] Invalid primIndex found within VariantSet."));
          }

          if (size_t(childPrimIdx) >= _prim_nodes.size()) {
            PUSH_ERROR_AND_RETURN(fmt::format("[InternalError] Invalid primIndex found within VariantSet. variantChildPrimIdsx {} Exceeds _prim_nodes.size() {}", childPrimIdx, _prim_nodes.size()));
          }

          variant.primChildren.push_back(childPrimIdx);

          //_prim_nodes[size_t(childPrimIdx)].parent_is_variant = true;
        }
        DCOUT("Add variant: " << item.first);
        variantNodes[item.first] = std::move(variant);
      }

      DCOUT("Add variantSet: " << variant_name);
      variantSets[variant_name] = std::move(variantNodes);
    }

    DCOUT("variantSets.size = " << variantSets.size());
    dst = std::move(variantSets);

    return true;
  }

  template <typename T>
  bool RegisterReconstructCallback();

  // Convert a parser-side VariantSetList (variantSetName -> VariantSetContent)
  // into the reader-side variantNodeMap (variantSetName -> variantName ->
  // VariantNode), recursing into each variant block's OWN nested variantSets so
  // they survive layer-mode load. Without this recursion the PrimSpec/Layer
  // path silently drops nested variant content (e.g. ALab's `render_high` geo
  // variant holding a `geo_vis` variantSet that supplies the proxy mesh).
  nonstd::expected<bool, std::string> ConvertVariantSetList(
      const ascii::AsciiParser::VariantSetList &in_variants,
      std::map<std::string, std::map<std::string, VariantNode>> &out) {
    for (const auto &variantContext : in_variants) {
      const std::string variant_name = variantContext.first;

      std::map<std::string, VariantNode> variantNodes;
      for (const auto &item : variantContext.second.variantSets) {
        VariantNode variant;
        if (!ReconstructPrimMeta(item.second.metas, &variant.metas)) {
          return nonstd::make_unexpected(fmt::format("Failed to process Prim metadataum in variantSet {} item {} ", variant_name, item.first));
        }
        variant.props = item.second.props;

        // child Prim should be already reconstructed.
        for (const auto &childPrimIdx : item.second.primIndices) {
          if (childPrimIdx < 0) {
            return nonstd::make_unexpected(fmt::format("[InternalError] Invalid primIndex found within VariantSet."));
          }
          if (size_t(childPrimIdx) >= _primspec_nodes.size()) {
            return nonstd::make_unexpected(fmt::format("[InternalError] Invalid primIndex found within VariantSet. variantChildPrimIdsx {} Exceeds _prim_nodes.size() {}", childPrimIdx, _primspec_nodes.size()));
          }
          variant.primChildren.push_back(childPrimIdx);
        }

        // Recurse into nested variantSets authored inside this variant block.
        if (!item.second.variantSets.empty()) {
          auto nret = ConvertVariantSetList(item.second.variantSets, variant.variantSets);
          if (!nret) {
            return nret;
          }
        }

        variantNodes.emplace(item.first, std::move(variant));
      }
      out.emplace(variant_name, std::move(variantNodes));
    }
    return true;
  }

  void RegisterPrimSpecHandler() {
    _parser.RegisterPrimSpecFunction(
         [&](const Path &full_path, const Specifier spec, const std::string &typeName, const Path &prim_name, const int64_t primIdx,
            const int64_t parentPrimIdx,
            const prim::PropertyMap &properties,
            const ascii::AsciiParser::PrimMetaMap &in_meta,
            const ascii::AsciiParser::VariantSetList &in_variants)
            -> nonstd::expected<bool, std::string> {

          if (!prim_name.is_valid()) {
            return nonstd::make_unexpected("Invalid Prim name: " +
                                           prim_name.full_path_name());
          }
          if (prim_name.is_absolute_path() || prim_name.is_root_path()) {
            return nonstd::make_unexpected(
                "Prim name should not starts with '/' or contain `/`: Prim "
                "name = " +
                prim_name.full_path_name());
          }

          if (!prim_name.prop_part().empty()) {
            return nonstd::make_unexpected(
                "Prim path should not contain property part(`.`): Prim name "
                "= " +
                prim_name.full_path_name());
          }

          if (primIdx < 0) {
            return nonstd::make_unexpected(
                "Unexpected primIdx value. primIdx must be positive.");
          }

          if (prim_name.prim_part().empty()) {
            return nonstd::make_unexpected("Prim's name should not be empty ");
          }

          PrimSpec primspec;
          primspec.name() = prim_name.prim_part();
          primspec.specifier() = spec;
          primspec.typeName() = typeName;

          DCOUT("primspec name, primType = " << prim_name.prim_part() << ", " << typeName);

          if (!ReconstructPrimMeta(in_meta, &primspec.metas())) {
            return nonstd::make_unexpected(
                "Failed to process Prim metadataum.");
          }

          primspec.props() = properties;

          //
          // variants
          // NOTE: variantChildren setup is delayed. It will be processed ConstructPrimTreeRec()
          //
          std::map<std::string, std::map<std::string, VariantNode>> variantSets;
          {
            auto vret = ConvertVariantSetList(in_variants, variantSets);
            if (!vret) {
              return vret;
            }
          }


          // Assign index for PrimSpec
          // TODO: Use sample id table(= _prim_nodes)

          if (size_t(primIdx) >= _primspec_nodes.size()) {
            _primspec_nodes.resize(size_t(primIdx) + 1);
          }
          DCOUT("sz " << std::to_string(_primspec_nodes.size())
                      << ", primIdx = " << primIdx);

          _primspec_nodes[size_t(primIdx)].primSpec = std::move(primspec);
          DCOUT("primspec[" << primIdx << "].ty = "
                        << _primspec_nodes[size_t(primIdx)].primSpec.typeName());
          _primspec_nodes[size_t(primIdx)].parent = parentPrimIdx;
          _primspec_nodes[size_t(primIdx)].variantNodeMap = variantSets;

          if (parentPrimIdx == -1) {
            _toplevel_primspecs.push_back(size_t(primIdx));
          } else if (size_t(parentPrimIdx) < _primspec_nodes.size()) {
            _primspec_nodes[size_t(parentPrimIdx)].children.push_back(
                size_t(primIdx));
            return true;
          } else {
            DCOUT("Invalid parentPrimIdx: " << parentPrimIdx);
            return false;
          }

          return true;
      }
    );

  }

  void StageMetaProcessor() {
    _parser.RegisterStageMetaProcessFunction(
        [&](const ascii::AsciiParser::StageMetas &metas) {
          DCOUT("StageMeta CB:");

          _stage.metas().doc = metas.doc;
          if (metas.upAxis) {
            _stage.metas().upAxis = metas.upAxis.value();
          }

          _stage.metas().comment = metas.comment;

          if (metas.subLayers.size()) {
            // subLayers items carry an optional LayerOffset (parsed by the
            // custom subLayers loop in ascii-parser.cc).
            _stage.metas().subLayers = metas.subLayers;
          }

          _stage.metas().defaultPrim = metas.defaultPrim;
          if (metas.metersPerUnit) {
            _stage.metas().metersPerUnit = metas.metersPerUnit.value();
          }

          if (metas.kilogramsPerUnit) {
            _stage.metas().kilogramsPerUnit = metas.kilogramsPerUnit.value();
          }

          if (metas.timeCodesPerSecond) {
            _stage.metas().timeCodesPerSecond =
                metas.timeCodesPerSecond.value();
          }

          if (metas.startTimeCode) {
            _stage.metas().startTimeCode = metas.startTimeCode.value();
          }

          if (metas.endTimeCode) {
            _stage.metas().endTimeCode = metas.endTimeCode.value();
          }

          if (metas.framesPerSecond) {
            _stage.metas().framesPerSecond = metas.framesPerSecond.value();
          }

          if (metas.autoPlay) {
            _stage.metas().autoPlay = metas.autoPlay.value();
          }

          if (metas.playbackMode) {
            value::token tok = metas.playbackMode.value();
            if (tok.str() == "none") {
              _stage.metas().playbackMode = StageMetas::PlaybackMode::PlaybackModeNone;
            } else if (tok.str() == "loop") {
              _stage.metas().playbackMode = StageMetas::PlaybackMode::PlaybackModeLoop;
            } else {
              PUSH_ERROR_AND_RETURN("Unsupported playbackMode: " + tok.str());
            }
          }

          _stage.metas().customLayerData = metas.customLayerData;
          _stage.metas().customLayerDataAuthored = metas.customLayerDataAuthored;

          // AOUSD Core Spec layer metadata
          if (metas.colorConfiguration) {
            _stage.metas().colorConfiguration = metas.colorConfiguration.value();
          }
          if (metas.colorManagementSystem) {
            _stage.metas().colorManagementSystem = metas.colorManagementSystem.value();
          }
          if (metas.renderSettingsPrimPath) {
            _stage.metas().renderSettingsPrimPath =
                metas.renderSettingsPrimPath.value();
          }
          if (metas.owner) {
            _stage.metas().owner = metas.owner.value();
          }
          if (metas.hasOwnedSubLayers) {
            _stage.metas().hasOwnedSubLayers = metas.hasOwnedSubLayers.value();
          }
          if (metas.expressionVariables) {
            _stage.metas().expressionVariables = metas.expressionVariables.value();
          }

          // AOUSD Core Spec 10.3.2.6: relocates
          if (!metas.relocates.empty()) {
            _stage.metas().layerRelocates = metas.relocates;
          }

          // Unregistered layer metadata (preserved verbatim by the parser).
          if (!metas.unregisteredMetas.empty()) {
            _stage.metas().unregisteredMetas = metas.unregisteredMetas;
          }

          return true;  // ok
        });
  }

  void RegisterPrimIdxAssignCallback() {
    _parser.RegisterPrimIdxAssignFunction([&](const int64_t parentPrimIdx) {
      size_t idx = _prim_nodes.size();

      DCOUT("parentPrimIdx: " << parentPrimIdx << ", idx = " << idx);

      _prim_nodes.resize(idx + 1);

      // if (parentPrimIdx < 0) { // root
      //   // allocate empty prim to reserve _prim_nodes[idx]
      //   _prim_nodes.resize(idx + 1);
      //   DCOUT("resize to : " << (idx + 1));
      // }

      return idx;
    });
  }

  bool ReconstructPrimMeta(const ascii::AsciiParser::PrimMetaMap &in_meta,
                           PrimMeta *out) {

    auto BuildVariants = [](const Dictionary &dict) -> nonstd::expected<VariantSelectionMap, std::string> {

      // Allow empty dict.

      VariantSelectionMap m;

      for (const auto &item : dict) {
        // TODO: duplicated key check?
        if (auto pv = item.second.get_value<std::string>()) {
          m[item.first] = pv.value();
        } else if (auto pvs = item.second.get_value<value::StringData>()) {
          // TODO: store triple-quote info
          m[item.first] = pvs.value().value;
        } else {
          return nonstd::make_unexpected(fmt::format("LightUSD only accepts `string` value for `variants` element, but got type `{}`(type_id {}).", item.second.type_name(), item.second.type_id()));
        }
      }

      return std::move(m);

    };

    DCOUT("ReconstructPrimMeta");
    for (const auto &meta : in_meta) {
      DCOUT("meta.name = " << meta.first);

      const auto &listEditQual = std::get<0>(meta.second);
      const MetaVariable &var = std::get<1>(meta.second);

      if (meta.first == "active") {
        DCOUT("active. type = " << var.type_name());
        if (var.type_name() == "bool") {
          if (auto pv = var.get_value<bool>()) {
            out->set_active(pv.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "(Internal error?) `active` metadataum is not type `bool`.");
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `active` metadataum is not type `bool`. got `"
              << var.type_name() << "`.");
        }
      } else if (meta.first == "hidden") {
        DCOUT("hidden. type = " << var.type_name());
        if (var.type_name() == "bool") {
          if (auto pv = var.get_value<bool>()) {
            out->set_hidden(pv.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "(Internal error?) `hidden` metadataum is not type `bool`.");
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `hidden` metadataum is not type `bool`. got `"
              << var.type_name() << "`.");
        }

      } else if (meta.first == "instanceable") {
        DCOUT("instanceable. type = " << var.type_name());
        if (var.type_name() == "bool") {
          if (auto pv = var.get_value<bool>()) {
            out->set_instanceable(pv.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "(Internal error?) `instanceable` metadataum is not type `bool`.");
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `instanceable` metadataum is not type `bool`. got `"
              << var.type_name() << "`.");
        }

      } else if (meta.first == "sceneName") {
        DCOUT("sceneName. type = " << var.type_name());
        if (var.type_name() == value::kString) {
          if (auto pv = var.get_value<std::string>()) {
            out->set_sceneName(pv.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "(Internal error?) `sceneName` metadataum is not type `string`.");
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `sceneName` metadataum is not type `string`. got `"
              << var.type_name() << "`.");
        }
      } else if (meta.first == "displayName") {
        DCOUT("displayName. type = " << var.type_name());
        if (var.type_name() == value::kString) {
          if (auto pv = var.get_value<std::string>()) {
            out->set_displayName(pv.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "(Internal error?) `displayName` metadataum is not type `string`.");
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `displayName` metadataum is not type `string`. got `"
              << var.type_name() << "`.");
        }
      } else if (meta.first == "kind") {
        // std::tuple<ListEditQual, MetaVariable>
        // TODO: list-edit qual
        DCOUT("kind. type = " << var.type_name());
        if (var.type_name() == "token") {
          if (auto pv = var.get_value<value::token>()) {
            const value::token tok = pv.value();
            if (tok.str() == "subcomponent") {
              out->set_kind(Kind::Subcomponent);
            } else if (tok.str() == "component") {
              out->set_kind(Kind::Component);
            } else if (tok.str() == "model") {
              out->set_kind(Kind::Model);
            } else if (tok.str() == "group") {
              out->set_kind(Kind::Group);
            } else if (tok.str() == "assembly") {
              out->set_kind(Kind::Assembly);
            } else if (tok.str() == "sceneLibrary") {
              // USDZ specific: https://developer.apple.com/documentation/arkit/usdz_schemas_for_ar/scenelibrary
              out->set_kind(Kind::SceneLibrary);
            } else {
              // NOTE: empty token allowed.
              // For user-defined kind, store the string directly
              out->set_kind(tok.str());
            }
            DCOUT("Added kind: " << out->get_kind_str());
          } else {
            PUSH_ERROR_AND_RETURN(
                "(Internal error?) `kind` metadataum is not type `token`.");
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `kind` metadataum is not type `token`. got `"
              << var.type_name() << "`.");
        }
      } else if (meta.first == "sdrMetadata") {
        DCOUT("sdrMetadata. type = " << var.type_name());
        if (var.type_id() == value::TypeTraits<Dictionary>::type_id()) {
          if (auto pv = var.get_value<Dictionary>()) {
            // TODO: Check if all items are string type.
            out->set_sdrMetadata(pv.value());
          } else {
            PUSH_ERROR_AND_RETURN_TAG(kTag,
                "(Internal error?) `sdrMetadata` metadataum is not type "
                "`dictionary`. got type `"
                << var.type_name() << "`");
          }

        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `sdrMetadata` metadataum is not type "
              "`dictionary`. got type `"
              << var.type_name() << "`");
        }
      } else if (meta.first == "customData") {
        DCOUT("customData. type = " << var.type_name());
        if (var.type_id() == value::TypeTraits<Dictionary>::type_id()) {
          if (auto pv = var.get_value<Dictionary>()) {
            out->set_customData(pv.value());
          } else {
            PUSH_ERROR_AND_RETURN_TAG(kTag,
                "(Internal error?) `customData` metadataum is not type "
                "`dictionary`. got type `"
                << var.type_name() << "`");
          }

        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `customData` metadataum is not type "
              "`dictionary`. got type `"
              << var.type_name() << "`");
        }
      } else if (meta.first == "clips") {
        DCOUT("clips. type = " << var.type_name());
        if (var.type_id() == value::TypeTraits<Dictionary>::type_id()) {
          if (auto pv = var.get_value<Dictionary>()) {
            out->set_clips(pv.value());
          } else {
            PUSH_ERROR_AND_RETURN_TAG(kTag,
                "(Internal error?) `clips` metadataum is not type "
                "`dictionary`. got type `"
                << var.type_name() << "`");
          }

        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `clips` metadataum is not type "
              "`dictionary`. got type `"
              << var.type_name() << "`");
        }
      } else if (meta.first == "assetInfo") {
        DCOUT("assetInfo. type = " << var.type_name());
        if (auto pv = var.get_value<Dictionary>()) {
          out->set_assetInfo(pv.value());
        } else {
          PUSH_ERROR_AND_RETURN_TAG(kTag,
              "(Internal error?) `assetInfo` metadataum is not type "
              "`dictionary`. got type `"
              << var.type_name() << "`");
        }
      } else if (meta.first == "variants") {
        if (auto pv = var.get_value<Dictionary>()) {
          auto pm = BuildVariants(pv.value());
          if (!pm) {
            PUSH_ERROR_AND_RETURN(pm.error());
          }
          out->variants = (*pm);
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `variants` metadataum is not type "
              "`dictionary`. got type `"
              << var.type_name() << "`");
        }
      } else if (meta.first == "inherits") {
        // Initialize vector if not present
        if (!out->inherits) {
          out->inherits = std::vector<std::pair<ListEditQual, std::vector<Path>>>();
        }
        if (auto pvb = var.get_value<value::ValueBlock>()) {
          if (listEditQual != ListEditQual::ResetToExplicit) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          out->inherits->push_back(std::make_pair(listEditQual, std::vector<Path>()));
        } else if (auto pv = var.get_value<std::vector<Path>>()) {
          if (pv.value().empty() && (listEditQual != ListEditQual::ResetToExplicit)) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          out->inherits->push_back(std::make_pair(listEditQual, pv.value()));
        } else if (auto pvp = var.get_value<Path>()) {
          std::vector<Path> vs;
          vs.push_back(pvp.value());
          out->inherits->push_back(std::make_pair(listEditQual, vs));
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `inherits` metadataum should be either `path` or `path[]`. "
              "got type `"
              << var.type_name() << "`");
        }

      } else if (meta.first == "specializes") {
        // Initialize vector if not present
        if (!out->specializes) {
          out->specializes = std::vector<std::pair<ListEditQual, std::vector<Path>>>();
        }
        if (auto pvb = var.get_value<value::ValueBlock>()) {
          if (listEditQual != ListEditQual::ResetToExplicit) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          out->specializes->push_back(std::make_pair(listEditQual, std::vector<Path>()));
        } else if (auto pv = var.get_value<std::vector<Path>>()) {
          if (pv.value().empty() && (listEditQual != ListEditQual::ResetToExplicit)) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          out->specializes->push_back(std::make_pair(listEditQual, pv.value()));
        } else if (auto pvp = var.get_value<Path>()) {
          std::vector<Path> vs;
          vs.push_back(pvp.value());
          out->specializes->push_back(std::make_pair(listEditQual, vs));
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `specializes` metadataum should be either `path` or `path[]`. "
              "got type `"
              << var.type_name() << "`");
        }

      } else if (meta.first == "variantSets") {
        // Initialize vector if not present
        if (!out->variantSets) {
          out->variantSets = std::vector<std::pair<ListEditQual, std::vector<std::string>>>();
        }
        // treat as `string`
        if (auto pvb = var.get_value<value::ValueBlock>()) {
          if (listEditQual != ListEditQual::ResetToExplicit) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          out->variantSets->push_back(std::make_pair(listEditQual, std::vector<std::string>()));
        } else if (auto pv = var.get_value<value::StringData>()) {
          std::vector<std::string> vs;
          vs.push_back(pv.value().value);
          out->variantSets->push_back(std::make_pair(listEditQual, vs));
        } else if (auto pvs = var.get_value<std::string>()) {
          std::vector<std::string> vs;
          vs.push_back(pvs.value());
          out->variantSets->push_back(std::make_pair(listEditQual, vs));
        } else if (auto pva = var.get_value<std::vector<std::string>>()) {
          if (pva.value().empty() && (listEditQual != ListEditQual::ResetToExplicit)) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          out->variantSets->push_back(std::make_pair(listEditQual, pva.value()));
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `variantSets` metadataum is not type "
              "`string` or `string[]`. got type `"
              << var.type_name() << "`");
        }
      } else if (meta.first == "apiSchemas") {
        DCOUT("apiSchemas. type = " << var.type_name());
        // `apiSchemas = None` -> explicit empty list (clears any
        // previously authored prepend/append/etc. on this prim).
        if (var.is_blocked()) {
          if (listEditQual != ListEditQual::ResetToExplicit) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("`apiSchemas = None` must be unqualified (explicit), but has qualifier `{}`", to_string(listEditQual)));
          }
          APISchemas empty;
          empty.listOpQual = ListEditQual::ResetToExplicit;
          empty.explicitlyEmpty = true;  // authored as `apiSchemas = None`
          out->set_apiSchemas(std::move(empty));
          continue;
        }
        if (var.type_name() != "token[]") {
          PUSH_ERROR_AND_RETURN_TAG(kTag, "(Internal error?) `apiSchemas` metadataum is not type "
          "`token[]`. got type `"
          << var.type_name() << "`");
        }
        const bool isDelete = (listEditQual == ListEditQual::Delete);
        const bool isAdditive = (listEditQual == ListEditQual::Prepend)
                             || (listEditQual == ListEditQual::Append)
                             || (listEditQual == ListEditQual::Add)
                             || (listEditQual == ListEditQual::ResetToExplicit);
        if (!isDelete && !isAdditive) {
          PUSH_ERROR_AND_RETURN("(PrimMeta) " << "ListEdit op for `apiSchemas` must be `prepend`, `append`, `add`, `delete`, or unqualified, but got `" << to_string(listEditQual) << "`");
        }

        // Merge with any APISchemas already accumulated on this prim — a
        // single prim may carry both `prepend apiSchemas = [...]` and
        // `delete apiSchemas = [...]` (Omniverse / Newton-asset pattern).
        APISchemas apiSchemas;
        if (out->has_apiSchemas()) {
          apiSchemas = out->get_apiSchemas();
        }
        // First non-delete qualifier wins for round-trip purposes.
        if (isAdditive && apiSchemas.names.empty() && apiSchemas.unknownSchemas.empty()
            && apiSchemas.listOpQual == ListEditQual::ResetToExplicit) {
          apiSchemas.listOpQual = listEditQual;
        }

        auto pv = var.get_value<std::vector<value::token>>();
        if (!pv) {
          PUSH_ERROR_AND_RETURN_TAG(kTag, "(Internal error?) `apiSchemas` metadataum is not type "
          "`token[]`. got type `"
          << var.type_name() << "`");
        }
        // Preserve this op verbatim (qualifier + raw schema strings, in order)
        // so the writer can reproduce the authored SdfTokenListOp losslessly.
        // The resolved view (`names`/`deletedNames`) is still maintained below
        // for downstream schema-application consumers.
        std::vector<std::pair<std::string, std::string>> authoredItems;
        for (const auto &item : pv.value()) {
          // The authored op is preserved verbatim (full token, incl. any
          // `:instance` suffix) so the writer reproduces it exactly.
          authoredItems.push_back(std::make_pair(item.str(), std::string()));
          // For the resolved view, split multi-apply instances:
          // `SchemaName:instanceName` -> (SchemaName, instanceName).
          auto ret = enum_handler::APISchemaNameWithInstanceOpt(item.str());
          if (ret) {
            const auto entry = ret.value();  // (APIName, instanceName)
            if (isDelete) {
              apiSchemas.deletedNames.push_back(entry);
              apiSchemas.names.erase(
                  std::remove(apiSchemas.names.begin(), apiSchemas.names.end(), entry),
                  apiSchemas.names.end());
            } else {
              apiSchemas.names.push_back(entry);
            }
          } else if (_config.allow_unknown_apiSchema) {
            const std::string instanceName = "";
            const auto entry = std::make_pair(item.str(), instanceName);
            if (isDelete) {
              apiSchemas.deletedUnknownSchemas.push_back(entry);
              apiSchemas.unknownSchemas.erase(
                  std::remove(apiSchemas.unknownSchemas.begin(), apiSchemas.unknownSchemas.end(), entry),
                  apiSchemas.unknownSchemas.end());
            } else {
              apiSchemas.unknownSchemas.push_back(entry);
              // Unknown API schemas are preserved (round-tripped) intentionally;
              // OpenUSD does not warn on these — keep it a debug note, not a warning.
              DCOUT("(PrimMeta) Preserving unknown API schema: " << item.str());
            }
          } else {
            PUSH_ERROR_AND_RETURN("Unknown or invalid apiSchema: " + item.str());
          }
        }
        apiSchemas.authoredOps.push_back(
            std::make_pair(listEditQual, std::move(authoredItems)));

        out->set_apiSchemas(std::move(apiSchemas));
      } else if (meta.first == "references") {
        // Initialize vector if not present
        if (!out->references) {
          out->references = std::vector<std::pair<ListEditQual, std::vector<Reference>>>();
        }
        if (var.is_blocked()) {
          // Treat as empty list
          // empty list must be qualified as 'explicit'
          if (listEditQual != ListEditQual::ResetToExplicit) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          std::vector<Reference> refs;
          out->references->push_back(std::make_pair(listEditQual, refs));
        } else if (auto pv = var.get_value<Reference>()) {
          // To Reference
          std::vector<Reference> refs;
          refs.emplace_back(pv.value());
          out->references->push_back(std::make_pair(listEditQual, refs));
        } else if (auto pva = var.get_value<std::vector<Reference>>()) {
          if (pva.value().empty() && (listEditQual != ListEditQual::ResetToExplicit)) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          out->references->push_back(std::make_pair(listEditQual, pva.value()));
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `references` metadataum is not type "
              "`Reference`. got type `"
              << var.type_name() << "`");
        }
      } else if (meta.first == "payload") {
        // Initialize vector if not present
        if (!out->payload) {
          out->payload = std::vector<std::pair<ListEditQual, std::vector<Payload>>>();
        }
        if (var.is_blocked()) {
          if (listEditQual != ListEditQual::ResetToExplicit) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          // make empty
          std::vector<Payload> refs;
          out->payload->push_back(std::make_pair(listEditQual, refs));
        } else if (auto pv = var.get_value<Payload>()) {
          // To Payload
          std::vector<Payload> pls;
          pls.emplace_back(pv.value());
          out->payload->push_back(std::make_pair(listEditQual, pls));
        } else if (auto pva = var.get_value<std::vector<Payload>>()) {
          if (pva.value().empty() && (listEditQual != ListEditQual::ResetToExplicit)) {
            PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("None or Empty list must be `explicit`(no qualifier), but has qualifier `{}`", to_string(listEditQual)));
          }
          out->payload->push_back(std::make_pair(listEditQual, pva.value()));
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error) `payload` metadataum is not type "
              "Payload. got type `"
              << var.type_name() << "`");
        }
      } else if (meta.first == "doc") {
        if (auto pv = var.get_value<value::StringData>()) {
          out->set_doc(pv.value());
        } else if (auto spv = var.get_value<std::string>()) {
          out->set_doc(spv.value());
        } else {
          PUSH_ERROR_AND_RETURN(
              "(Internal error?) `doc` metadataum is not type `string`. got `"
              << var.type_name() << "`.");
        }
      } else if (meta.first == "comment") {
        if (auto pv = var.get_value<value::StringData>()) {
          // Preserve full StringData including has_comment_prefix flag
          out->set_comment(pv.value());
        } else if (auto spv = var.get_value<std::string>()) {
          value::StringData sdata;
          sdata.value = spv.value();
          out->set_comment(sdata);
        }
      } else if (meta.first == "reorder:nameChildren" ||
                 meta.first == "reorder:properties") {
        // `reorder` body statements stashed by the parser: route to the
        // dedicated PrimMetas fields (NOT unregisteredMetas — they are body
        // statements, not paren metadata).
        if (auto tv = var.get_value<std::vector<value::token>>()) {
          if (meta.first == "reorder:nameChildren") {
            out->nameChildrenReorder = tv.value();
          } else {
            out->propertiesReorder = tv.value();
          }
        }
      } else {
        // Store unregistered metadata as raw string (OpenUSD-compatible).
        // The value is stored verbatim and written back unquoted to USDA.
        if (auto spv = var.get_value<std::string>()) {
          out->unregisteredMetas[meta.first] = spv.value();
        } else {
          // Convert non-string values to their string representation
          out->unregisteredMetas[meta.first] = value::pprint_value(var.get_raw_value());
        }
      }
    }

    return true;
  }

  ///
  /// Reader entry point
  /// TODO: Use callback function(visitor) so that Reconstruct**** function is
  /// invoked in the Parser context.
  ///
  bool Read(const uint32_t state_flags, bool as_primspec);

  // std::vector<GPrim> GetGPrims() { return _gprims; }

  std::string GetDefaultPrimName() const { return _defaultPrim; }

  std::string GetError() { return _err; }

  std::string GetWarning() { return _warn; }

  ///
  /// Valid after `Read`.
  ///
  bool GetAsLayer(Layer *layer);

  ///
  /// Valid after `Read`.
  ///
  bool ReconstructStage();

  ///
  /// Valid after `ReconstructStage`.
  ///
  const Stage &GetStage() const { return _stage; }

 private:
  //bool stage_reconstructed_{false};


  ///
  /// -- Members --
  ///

  // TODO: Remove
  // std::set<std::string> _node_types;

  std::string _base_dir;  // Used for importing another USD file
  std::string _filename;  // Used for displaying error context from source file
  //AssetResolutionResolver _arr;


  // "class" defs
  //std::map<std::string, Klass> _klasses;

  std::stack<std::string> _path_stack;

  std::string _err;
  std::string _warn;

  // Cache of loaded `references`
  // <filename, {defaultPrim index, Layer(PrimSpec data of usd file)}>
  std::map<std::string, std::pair<uint32_t, Layer>>
      _reference_cache;

  // toplevel prims
  std::vector<size_t> _toplevel_prims;  // index to _prim_nodes

  // 1D Linearized array of prim nodes.
  std::vector<PrimNode> _prim_nodes;

  // Path(prim part only) -> index to _prim_nodes[]
  std::map<std::string, size_t> _primpath_to_prim_idx_map;


  // toplevel primspecs
  std::vector<size_t> _toplevel_primspecs;  // index to _prim_nodes

  // Flattened array of primspec nodes.
  std::vector<PrimSpecNode> _primspec_nodes;
  // Path(prim part only) -> index to _primspec_nodes[]
  std::map<std::string, size_t> _primpath_to_primspec_idx_map;
  bool _primspec_invalidated{false};

  std::string _defaultPrim;

  // Used for Ascii parser option
  USDAReaderConfig _config;

  ascii::AsciiParser _parser;

};  // namespace usda

// Out-of-line (NON-inline) so the extern template declarations above actually
// suppress implicit instantiation in usda-reader.cc; instantiated in the
// usda-reader-reconstruct-*.cc sibling TUs. [temp.explicit]/13 exempts inline fns.
template <typename T>
bool USDAReader::Impl::RegisterReconstructCallback() {
    _parser.RegisterPrimConstructFunction(
        PrimTypeTraits<T>::prim_type_name,
        [&](const Path &full_path, const Specifier spec, const std::string &_primTypeName, const Path &prim_name, const int64_t primIdx,
            const int64_t parentPrimIdx,
            prim::PropertyMap &properties,
            const ascii::AsciiParser::PrimMetaMap &in_meta,
            const ascii::AsciiParser::VariantSetList &in_variants)
            -> nonstd::expected<bool, std::string> {

          std::string primTypeName = _primTypeName;
          if (primTypeName == "__AnyType__") {
            primTypeName = ""; // Make empty
          }

          if (!prim_name.is_valid()) {
            return nonstd::make_unexpected("Invalid Prim name: " +
                                           prim_name.full_path_name());
          }
          if (prim_name.is_absolute_path() || prim_name.is_root_path()) {
            return nonstd::make_unexpected(
                "Prim name should not starts with '/' or contain `/`: Prim "
                "name = " +
                prim_name.full_path_name());
          }

          if (!prim_name.prop_part().empty()) {
            return nonstd::make_unexpected(
                "Prim path should not contain property part(`.`): Prim name "
                "= " +
                prim_name.full_path_name());
          }

          if (primIdx < 0) {
            return nonstd::make_unexpected(
                "Unexpected primIdx value. primIdx must be positive.");
          }

          T prim;

          if (!ReconstructPrimMeta(in_meta, &prim.meta)) {
            return nonstd::make_unexpected(
                "Failed to process Prim metadataum.");
          }

          DCOUT("primType = " << value::TypeTraits<T>::type_name()
                              << ", node.size "
                              << std::to_string(_prim_nodes.size())
                              << ", primIdx = " << primIdx
                              << ", parentPrimIdx = " << parentPrimIdx);

          DCOUT("full_path = " << full_path.full_path_name());
          DCOUT("primName = " << prim_name.full_path_name());

          prim::ReferenceList references;
          if (prim.meta.references) {
            references = prim.meta.references.value();
          }

          bool ret = ReconstructPrim<T>(full_path, spec, properties, references, &prim);

          if (!ret) {
            return nonstd::make_unexpected("Failed to reconstruct Prim: " +
                                           prim_name.full_path_name());
          }

          prim.spec = spec;
          prim.name = prim_name.prim_part();

          //
          // variants
          // NOTE: variantChildren setup is delayed. It will be processed in ConstructPrimSpecTreeRec
          //
          std::map<std::string, std::map<std::string, VariantNode>> variantSets;
          if (!ProcessVariantSetContent(0, in_variants, variantSets)) {
            return nonstd::make_unexpected(fmt::format("[InternalError] Failed to process VariantSet"));
          }

          // Add to scene graph.
          // NOTE: Scene graph is constructed from bottom up manner(Children
          // first), so add this primIdx to parent's children.
          if (size_t(primIdx) >= _prim_nodes.size()) {
            _prim_nodes.resize(size_t(primIdx) + 1);
          }
          DCOUT("sz " << std::to_string(_prim_nodes.size())
                      << ", primIdx = " << primIdx);

          _prim_nodes[size_t(primIdx)].prim = std::move(prim);
          _prim_nodes[size_t(primIdx)].typeName = primTypeName;
          _prim_nodes[size_t(primIdx)].specifier = spec;
          _prim_nodes[size_t(primIdx)].variantNodeMap = variantSets;


          // Store actual Prim typeName also for Model Prim type.
          // TODO: Find more better way.
          {
            value::Value *p = &(_prim_nodes[size_t(primIdx)].prim);
            Model *model = p->as<Model>();
            if (model) {
              DCOUT("Set prim typeName " << primTypeName << " to Model Prim[" << primIdx << "]");
              model->prim_type_name = primTypeName;
            }
          }

          DCOUT("prim[" << primIdx << "].ty = "
                        << _prim_nodes[size_t(primIdx)].prim.type_name());
          _prim_nodes[size_t(primIdx)].parent = parentPrimIdx;

          if (parentPrimIdx == -1) {
            _toplevel_prims.push_back(size_t(primIdx));
          } else {
            _prim_nodes[size_t(parentPrimIdx)].children.push_back(
                  size_t(primIdx));
          }

          return true;
        });

    return true;
}


template <typename T>
bool USDAReader::Impl::ReconstructPrim(
    const Path &full_path,
    const Specifier &spec,
    prim::PropertyMap &properties,
    const prim::ReferenceList &references,
    T *prim) {

  prim::PrimReconstructOptions options;
  buildReconstructOptions(full_path, options);
  DCOUT("strict_allowedToken_check " << options.strict_allowedToken_check);

  std::string err;
  if (!prim::ReconstructPrim(spec, properties, references, prim, &_warn, &err, options)) {
    return reportReconstructPrimError(value::TypeTraits<T>::type_name(),
                                      full_path, err);
  }
  return true;
}

// Explicit instantiation declarations for prim::ReconstructPrim placeholder
// specializations defined in prim-reconstruct.cc. These keep split reader TUs
// from instantiating from only the forward declaration under -Weverything.
}  // namespace usda
namespace prim {

#define USDA_EXTERN_PLACEHOLDER_RECONSTRUCT_PRIM(__T) \
  extern template bool ReconstructPrim<__T>( \
      const Specifier &, PropertyMap &, const ReferenceList &, \
      __T *, std::string *, std::string *, const PrimReconstructOptions &);
USDA_EXTERN_PLACEHOLDER_RECONSTRUCT_PRIM(RenderSettings)
USDA_EXTERN_PLACEHOLDER_RECONSTRUCT_PRIM(RenderProduct)
USDA_EXTERN_PLACEHOLDER_RECONSTRUCT_PRIM(RenderVar)
USDA_EXTERN_PLACEHOLDER_RECONSTRUCT_PRIM(GenerativeProcedural)
#undef USDA_EXTERN_PLACEHOLDER_RECONSTRUCT_PRIM

}  // namespace prim
namespace usda {


// --- Split reconstruct: RegisterReconstructCallback<T> is instantiated across
// usda-reader-reconstruct-{1,2,3,4}.cc. Declared extern so the RegisterReconstructCallbacks()
// driver in usda-reader.cc does not instantiate (and codegen) all 60 specializations locally.
#define USDA_EXTERN_REGISTER_RECONSTRUCT(__T) \
  extern template bool USDAReader::Impl::RegisterReconstructCallback<__T>();
USDA_EXTERN_REGISTER_RECONSTRUCT(Model)
USDA_EXTERN_REGISTER_RECONSTRUCT(GPrim)
USDA_EXTERN_REGISTER_RECONSTRUCT(Xform)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomCube)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomSphere)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomCone)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomPoints)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomCylinder)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomCapsule)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomMesh)
USDA_EXTERN_REGISTER_RECONSTRUCT(Volume)
USDA_EXTERN_REGISTER_RECONSTRUCT(VolumeFieldBase)
USDA_EXTERN_REGISTER_RECONSTRUCT(FieldBase)
USDA_EXTERN_REGISTER_RECONSTRUCT(VolumeFieldAsset)
USDA_EXTERN_REGISTER_RECONSTRUCT(FieldAsset)
USDA_EXTERN_REGISTER_RECONSTRUCT(OpenVDBAsset)
USDA_EXTERN_REGISTER_RECONSTRUCT(Field3DAsset)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomSubset)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomBasisCurves)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomNurbsCurves)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomPlane)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomCylinder_1)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomCapsule_1)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomTetMesh)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomNurbsPatch)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomHermiteCurves)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomCamera)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeomPointInstancer)
USDA_EXTERN_REGISTER_RECONSTRUCT(Material)
USDA_EXTERN_REGISTER_RECONSTRUCT(Shader)
USDA_EXTERN_REGISTER_RECONSTRUCT(NodeGraph)
USDA_EXTERN_REGISTER_RECONSTRUCT(Scope)
USDA_EXTERN_REGISTER_RECONSTRUCT(RenderSettings)
USDA_EXTERN_REGISTER_RECONSTRUCT(RenderProduct)
USDA_EXTERN_REGISTER_RECONSTRUCT(RenderVar)
USDA_EXTERN_REGISTER_RECONSTRUCT(GenerativeProcedural)
USDA_EXTERN_REGISTER_RECONSTRUCT(SphereLight)
USDA_EXTERN_REGISTER_RECONSTRUCT(DomeLight)
USDA_EXTERN_REGISTER_RECONSTRUCT(DiskLight)
USDA_EXTERN_REGISTER_RECONSTRUCT(DistantLight)
USDA_EXTERN_REGISTER_RECONSTRUCT(CylinderLight)
USDA_EXTERN_REGISTER_RECONSTRUCT(RectLight)
USDA_EXTERN_REGISTER_RECONSTRUCT(GeometryLight)
USDA_EXTERN_REGISTER_RECONSTRUCT(PortalLight)
USDA_EXTERN_REGISTER_RECONSTRUCT(DomeLight_1)
USDA_EXTERN_REGISTER_RECONSTRUCT(LightFilter)
USDA_EXTERN_REGISTER_RECONSTRUCT(PluginLightFilter)
USDA_EXTERN_REGISTER_RECONSTRUCT(SkelRoot)
USDA_EXTERN_REGISTER_RECONSTRUCT(Skeleton)
USDA_EXTERN_REGISTER_RECONSTRUCT(SkelAnimation)
USDA_EXTERN_REGISTER_RECONSTRUCT(BlendShape)
USDA_EXTERN_REGISTER_RECONSTRUCT(PhysicsJoint)
USDA_EXTERN_REGISTER_RECONSTRUCT(PhysicsScene)
USDA_EXTERN_REGISTER_RECONSTRUCT(PhysicsRevoluteJoint)
USDA_EXTERN_REGISTER_RECONSTRUCT(PhysicsPrismaticJoint)
USDA_EXTERN_REGISTER_RECONSTRUCT(PhysicsSphericalJoint)
USDA_EXTERN_REGISTER_RECONSTRUCT(PhysicsFixedJoint)
USDA_EXTERN_REGISTER_RECONSTRUCT(PhysicsDistanceJoint)
USDA_EXTERN_REGISTER_RECONSTRUCT(PhysicsCollisionGroup)
USDA_EXTERN_REGISTER_RECONSTRUCT(MjcActuator)
USDA_EXTERN_REGISTER_RECONSTRUCT(NewtonActuator)
USDA_EXTERN_REGISTER_RECONSTRUCT(MjcTendon)
USDA_EXTERN_REGISTER_RECONSTRUCT(MjcKeyframe)
USDA_EXTERN_REGISTER_RECONSTRUCT(MjcSensor)
USDA_EXTERN_REGISTER_RECONSTRUCT(Preliminary_PhysicsGravitationalForce)
USDA_EXTERN_REGISTER_RECONSTRUCT(Preliminary_InfiniteColliderPlane)
USDA_EXTERN_REGISTER_RECONSTRUCT(Preliminary_ReferenceImage)
USDA_EXTERN_REGISTER_RECONSTRUCT(Preliminary_Behavior)
USDA_EXTERN_REGISTER_RECONSTRUCT(Preliminary_Trigger)
USDA_EXTERN_REGISTER_RECONSTRUCT(Preliminary_Action)
USDA_EXTERN_REGISTER_RECONSTRUCT(Preliminary_Text)
USDA_EXTERN_REGISTER_RECONSTRUCT(SpatialAudio)
#undef USDA_EXTERN_REGISTER_RECONSTRUCT

}  // namespace usda
}  // namespace lightusd

#endif  // !LIGHTUSD_DISABLE_MODULE_USDA_READER
