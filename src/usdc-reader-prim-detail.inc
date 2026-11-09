// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Shared by usdc-reader-prim.cc (dispatch) and usdc-reader-prim-reconstruct-{1..4}.cc:
// the USDCReader::Impl::ReconstructPrim<T> definition + prim::ReconstructPrim<T> externs +
// extern-template decls so the dispatch TU does not instantiate all 58 specializations
// (they are split across the reconstruct-*.cc siblings). #include after usdc-reader-impl.hh.

// Prim type headers not pulled in by usdc-reader-impl.hh (Physics*/Mjc*/Preliminary_*/SpatialAudio).
#include "usdPhysics.hh"
#include "usdAR.hh"
#include "usdMedia.hh"
#include "mjcPhysics.hh"

// Explicit instantiation declarations for prim::ReconstructPrim specializations
// defined in other translation units (prim-reconstruct.cc, prim-reconstruct-physics.cc, etc.).
// Suppresses -Wundefined-func-template.
#define EXTERN_RECONSTRUCT_PRIM(__ty) \
  extern template bool ReconstructPrim<__ty>( \
      const Specifier &, PropertyMap &, const ReferenceList &, \
      __ty *, std::string *, std::string *, const PrimReconstructOptions &)

namespace lightusd {
namespace prim {

// Placeholder schema prim types (UsdVol / UsdRender / UsdProc): defined in
// prim-reconstruct.cc, explicitly instantiated in usdc-reader-prim-reconstruct-1.cc.
EXTERN_RECONSTRUCT_PRIM(GeomPlane);
EXTERN_RECONSTRUCT_PRIM(GeomCylinder_1);
EXTERN_RECONSTRUCT_PRIM(Volume);
EXTERN_RECONSTRUCT_PRIM(VolumeFieldBase);
EXTERN_RECONSTRUCT_PRIM(FieldBase);
EXTERN_RECONSTRUCT_PRIM(VolumeFieldAsset);
EXTERN_RECONSTRUCT_PRIM(OpenVDBAsset);
EXTERN_RECONSTRUCT_PRIM(FieldAsset);
EXTERN_RECONSTRUCT_PRIM(Field3DAsset);
EXTERN_RECONSTRUCT_PRIM(RenderSettings);
EXTERN_RECONSTRUCT_PRIM(RenderProduct);
EXTERN_RECONSTRUCT_PRIM(RenderVar);
EXTERN_RECONSTRUCT_PRIM(GenerativeProcedural);
EXTERN_RECONSTRUCT_PRIM(GeomCapsule_1);
EXTERN_RECONSTRUCT_PRIM(GeomTetMesh);
EXTERN_RECONSTRUCT_PRIM(GeomNurbsPatch);
EXTERN_RECONSTRUCT_PRIM(GeomHermiteCurves);
EXTERN_RECONSTRUCT_PRIM(DomeLight_1);
EXTERN_RECONSTRUCT_PRIM(LightFilter);
EXTERN_RECONSTRUCT_PRIM(PluginLightFilter);
EXTERN_RECONSTRUCT_PRIM(PhysicsJoint);
EXTERN_RECONSTRUCT_PRIM(PhysicsScene);
EXTERN_RECONSTRUCT_PRIM(PhysicsRevoluteJoint);
EXTERN_RECONSTRUCT_PRIM(PhysicsPrismaticJoint);
EXTERN_RECONSTRUCT_PRIM(PhysicsSphericalJoint);
EXTERN_RECONSTRUCT_PRIM(PhysicsFixedJoint);
EXTERN_RECONSTRUCT_PRIM(PhysicsDistanceJoint);
EXTERN_RECONSTRUCT_PRIM(PhysicsCollisionGroup);
EXTERN_RECONSTRUCT_PRIM(MjcActuator);
EXTERN_RECONSTRUCT_PRIM(NewtonActuator);
EXTERN_RECONSTRUCT_PRIM(MjcTendon);
EXTERN_RECONSTRUCT_PRIM(MjcKeyframe);
EXTERN_RECONSTRUCT_PRIM(MjcSensor);
EXTERN_RECONSTRUCT_PRIM(Preliminary_PhysicsGravitationalForce);
EXTERN_RECONSTRUCT_PRIM(Preliminary_InfiniteColliderPlane);
EXTERN_RECONSTRUCT_PRIM(Preliminary_ReferenceImage);
EXTERN_RECONSTRUCT_PRIM(Preliminary_Behavior);
EXTERN_RECONSTRUCT_PRIM(Preliminary_Trigger);
EXTERN_RECONSTRUCT_PRIM(Preliminary_Action);
EXTERN_RECONSTRUCT_PRIM(Preliminary_Text);
EXTERN_RECONSTRUCT_PRIM(SpatialAudio);

#undef EXTERN_RECONSTRUCT_PRIM

} // namespace prim
} // namespace lightusd

namespace lightusd {
namespace usdc {

template <typename T>
bool USDCReader::Impl::ReconstructPrim(const Specifier &spec, const crate::CrateReader::Node &node,
                                       const PathIndexToSpecIndexMap &psmap,
                                       T *prim) {
  // Prim's properties are stored in its children nodes.
  prim::PropertyMap properties;
  if (!BuildPropertyMap(node.GetChildren(), psmap, &properties)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Failed to build PropertyMap.");
  }

  prim::ReferenceList refs;  // dummy

  prim::PrimReconstructOptions reconstruct_options;
  reconstruct_options.strict_allowedToken_check = _config.strict_allowedToken_check;
  reconstruct_options.strict_shader_type_check = _config.strict_shader_type_check;

  // Per-call diagnostic buffers, merged under lock: this runs on worker
  // threads during parallel prim reconstruction.
  std::string local_warn;
  std::string local_err;
  const bool reconstruct_ok = prim::ReconstructPrim<T>(
      spec, properties, refs, prim, &local_warn, &local_err,
      reconstruct_options);
  AppendWarnRaw(local_warn);
  AppendErrorRaw(local_err);
  if (!reconstruct_ok) {
    return false;
  }

  return true;
}

template <typename T>
std::unique_ptr<Prim> USDCReader::Impl::ReconstructTypedPrim(
    const crate::CrateReader::Node &node, const Specifier spec,
    const PathIndexToSpecIndexMap &psmap, const std::string &prim_name,
    const std::string &primTypeName, const PrimMeta &meta,
    const std::vector<value::token> &properties,
    const std::vector<value::token> &primChildren) {
  T typed_prim;
  if (!ReconstructPrim(spec, node, psmap, &typed_prim)) {
    PUSH_ERROR("Failed to reconstruct Prim " << value::TypeTraits<T>::type_name()
               << " elementName: " << prim_name);
    return nullptr;
  }
  typed_prim.meta = meta;
  typed_prim.name = prim_name;
  typed_prim.spec = spec;
  typed_prim.propertyNames() = properties;
  typed_prim.primChildrenNames() = primChildren;
  value::Value primdata(std::move(typed_prim));
  auto result = std::unique_ptr<Prim>(new Prim(prim_name, std::move(primdata)));
  result->prim_type_name() = primTypeName;
  result->specifier() = spec;
  result->metas().primChildren = primChildren;
  return result;
}

// extern-template decls: suppress instantiation of USDCReader::Impl::ReconstructPrim<T> in
// the dispatch TU (usdc-reader-prim.cc); the reconstruct-*.cc siblings instantiate them.
// NOTE: the definition above is out-of-line (non-inline) so this suppression is honored
// ([temp.explicit]/13 exempts inline functions).
#define USDC_EXTERN_RECONSTRUCT_PRIM(__ty) \
  extern template bool USDCReader::Impl::ReconstructPrim<__ty>( \
      const Specifier &, const crate::CrateReader::Node &, \
      const PathIndexToSpecIndexMap &, __ty *); \
  extern template std::unique_ptr<Prim> USDCReader::Impl::ReconstructTypedPrim<__ty>( \
      const crate::CrateReader::Node &, const Specifier, \
      const PathIndexToSpecIndexMap &, const std::string &, \
      const std::string &, const PrimMeta &, \
      const std::vector<value::token> &, const std::vector<value::token> &)
USDC_EXTERN_RECONSTRUCT_PRIM(Xform);
USDC_EXTERN_RECONSTRUCT_PRIM(Model);
USDC_EXTERN_RECONSTRUCT_PRIM(Scope);
USDC_EXTERN_RECONSTRUCT_PRIM(Volume);
USDC_EXTERN_RECONSTRUCT_PRIM(VolumeFieldBase);
USDC_EXTERN_RECONSTRUCT_PRIM(FieldBase);
USDC_EXTERN_RECONSTRUCT_PRIM(VolumeFieldAsset);
USDC_EXTERN_RECONSTRUCT_PRIM(OpenVDBAsset);
USDC_EXTERN_RECONSTRUCT_PRIM(Field3DAsset);
USDC_EXTERN_RECONSTRUCT_PRIM(RenderSettings);
USDC_EXTERN_RECONSTRUCT_PRIM(RenderProduct);
USDC_EXTERN_RECONSTRUCT_PRIM(RenderVar);
USDC_EXTERN_RECONSTRUCT_PRIM(GenerativeProcedural);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomPoints);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomMesh);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomCapsule);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomCube);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomCone);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomCylinder);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomSphere);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomSubset);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomBasisCurves);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomNurbsCurves);
USDC_EXTERN_RECONSTRUCT_PRIM(Volume);
USDC_EXTERN_RECONSTRUCT_PRIM(VolumeFieldBase);
USDC_EXTERN_RECONSTRUCT_PRIM(FieldBase);
USDC_EXTERN_RECONSTRUCT_PRIM(VolumeFieldAsset);
USDC_EXTERN_RECONSTRUCT_PRIM(OpenVDBAsset);
USDC_EXTERN_RECONSTRUCT_PRIM(Field3DAsset);
USDC_EXTERN_RECONSTRUCT_PRIM(RenderSettings);
USDC_EXTERN_RECONSTRUCT_PRIM(RenderProduct);
USDC_EXTERN_RECONSTRUCT_PRIM(RenderVar);
USDC_EXTERN_RECONSTRUCT_PRIM(GenerativeProcedural);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomPlane);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomCylinder_1);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomCapsule_1);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomTetMesh);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomNurbsPatch);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomHermiteCurves);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomCamera);
USDC_EXTERN_RECONSTRUCT_PRIM(GeomPointInstancer);
USDC_EXTERN_RECONSTRUCT_PRIM(SphereLight);
USDC_EXTERN_RECONSTRUCT_PRIM(DomeLight);
USDC_EXTERN_RECONSTRUCT_PRIM(DiskLight);
USDC_EXTERN_RECONSTRUCT_PRIM(DistantLight);
USDC_EXTERN_RECONSTRUCT_PRIM(CylinderLight);
USDC_EXTERN_RECONSTRUCT_PRIM(RectLight);
USDC_EXTERN_RECONSTRUCT_PRIM(GeometryLight);
USDC_EXTERN_RECONSTRUCT_PRIM(DomeLight_1);
USDC_EXTERN_RECONSTRUCT_PRIM(LightFilter);
USDC_EXTERN_RECONSTRUCT_PRIM(PluginLightFilter);
USDC_EXTERN_RECONSTRUCT_PRIM(SkelRoot);
USDC_EXTERN_RECONSTRUCT_PRIM(SkelAnimation);
USDC_EXTERN_RECONSTRUCT_PRIM(Skeleton);
USDC_EXTERN_RECONSTRUCT_PRIM(BlendShape);
USDC_EXTERN_RECONSTRUCT_PRIM(Material);
USDC_EXTERN_RECONSTRUCT_PRIM(Shader);
USDC_EXTERN_RECONSTRUCT_PRIM(NodeGraph);
USDC_EXTERN_RECONSTRUCT_PRIM(PhysicsJoint);
USDC_EXTERN_RECONSTRUCT_PRIM(PhysicsScene);
USDC_EXTERN_RECONSTRUCT_PRIM(PhysicsRevoluteJoint);
USDC_EXTERN_RECONSTRUCT_PRIM(PhysicsPrismaticJoint);
USDC_EXTERN_RECONSTRUCT_PRIM(PhysicsSphericalJoint);
USDC_EXTERN_RECONSTRUCT_PRIM(PhysicsFixedJoint);
USDC_EXTERN_RECONSTRUCT_PRIM(PhysicsDistanceJoint);
USDC_EXTERN_RECONSTRUCT_PRIM(PhysicsCollisionGroup);
USDC_EXTERN_RECONSTRUCT_PRIM(MjcActuator);
USDC_EXTERN_RECONSTRUCT_PRIM(NewtonActuator);
USDC_EXTERN_RECONSTRUCT_PRIM(MjcTendon);
USDC_EXTERN_RECONSTRUCT_PRIM(MjcKeyframe);
USDC_EXTERN_RECONSTRUCT_PRIM(MjcSensor);
USDC_EXTERN_RECONSTRUCT_PRIM(Preliminary_PhysicsGravitationalForce);
USDC_EXTERN_RECONSTRUCT_PRIM(Preliminary_InfiniteColliderPlane);
USDC_EXTERN_RECONSTRUCT_PRIM(Preliminary_ReferenceImage);
USDC_EXTERN_RECONSTRUCT_PRIM(Preliminary_Behavior);
USDC_EXTERN_RECONSTRUCT_PRIM(Preliminary_Trigger);
USDC_EXTERN_RECONSTRUCT_PRIM(Preliminary_Action);
USDC_EXTERN_RECONSTRUCT_PRIM(Preliminary_Text);
USDC_EXTERN_RECONSTRUCT_PRIM(SpatialAudio);
#undef USDC_EXTERN_RECONSTRUCT_PRIM

}  // namespace usdc
}  // namespace lightusd

// Explicit template instantiations for ReconstructPrim<T> + ReconstructTypedPrim<T>
#define INSTANTIATE_RECONSTRUCT_PRIM(__ty) \
  template bool USDCReader::Impl::ReconstructPrim<__ty>( \
      const Specifier &, const crate::CrateReader::Node &, \
      const PathIndexToSpecIndexMap &, __ty *); \
  template std::unique_ptr<Prim> USDCReader::Impl::ReconstructTypedPrim<__ty>( \
      const crate::CrateReader::Node &, const Specifier, \
      const PathIndexToSpecIndexMap &, const std::string &, \
      const std::string &, const PrimMeta &, \
      const std::vector<value::token> &, const std::vector<value::token> &)
