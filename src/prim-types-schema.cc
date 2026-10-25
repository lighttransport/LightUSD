// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Type-erased schema-prim dispatch — split out of prim-types.cc.
// GetPrimMeta/GetPrimElementName/SetPrimElementName/CastToXformable/GetLocalTransform
// each switch v.as<SchemaType>() (or prim.as<>, or use Xformable) across every USD
// prim type. These are the ONLY functions in prim-types.cc that needed the 8 schema
// headers (usdGeom/usdLux/usdShade/usdSkel/usdPhysics/mjcPhysics/usdAR/usdMedia, which
// also transitively supply Xformable). Isolating them lets prim-types.cc (Path/Prim/
// PrimMetas/Property) skip ~2.5-3s of schema parsing. GetPrimElementName/SetPrimElement
// Name/CastToXformable/GetLocalTransform/IsXformablePrim are declared in core/prim.hh;
// GetPrimMeta in prim-meta-access.hh. (IsXformablePrim stays in prim-types.cc — it only
// switches on type_id and is called here cross-TU.)
#include "prim-meta-access.hh"

#include "core/prim.hh"
#include "core/model-scope.hh"  // Model, Scope

#include "str-util.hh"

#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "mjcPhysics.hh"
#include "usdPhysics.hh"
#include "usdAR.hh"
#include "usdMedia.hh"

#include "common-macros.inc"

namespace lightusd {

const PrimMeta *GetPrimMeta(const value::Value &v) {
  // Lookup PrimMeta variable in Prim class

#define GET_PRIM_META(__ty)       \
  if (v.as<__ty>()) {             \
    return &(v.as<__ty>()->meta); \
  }

  GET_PRIM_META(Model)
  GET_PRIM_META(Scope)
  GET_PRIM_META(Volume)
  GET_PRIM_META(VolumeFieldBase)
  GET_PRIM_META(FieldBase)
  GET_PRIM_META(VolumeFieldAsset)
  GET_PRIM_META(OpenVDBAsset)
  GET_PRIM_META(Field3DAsset)
  GET_PRIM_META(RenderSettings)
  GET_PRIM_META(RenderProduct)
  GET_PRIM_META(RenderVar)
  GET_PRIM_META(GenerativeProcedural)
  GET_PRIM_META(Xform)
  GET_PRIM_META(GPrim)
  GET_PRIM_META(GeomMesh)
  GET_PRIM_META(Volume)
  GET_PRIM_META(VolumeFieldBase)
  GET_PRIM_META(FieldBase)
  GET_PRIM_META(VolumeFieldAsset)
  GET_PRIM_META(FieldAsset)
  GET_PRIM_META(OpenVDBAsset)
  GET_PRIM_META(Field3DAsset)
  GET_PRIM_META(GeomPoints)
  GET_PRIM_META(GeomCube)
  GET_PRIM_META(GeomCapsule)
  GET_PRIM_META(GeomCylinder)
  GET_PRIM_META(GeomSphere)
  GET_PRIM_META(GeomCone)
  GET_PRIM_META(GeomPlane)
  GET_PRIM_META(GeomSubset)
  GET_PRIM_META(GeomCamera)
  GET_PRIM_META(GeomBasisCurves)
  GET_PRIM_META(DomeLight)
  GET_PRIM_META(SphereLight)
  GET_PRIM_META(CylinderLight)
  GET_PRIM_META(DiskLight)
  GET_PRIM_META(DistantLight)
  GET_PRIM_META(RectLight)
  GET_PRIM_META(Material)
  GET_PRIM_META(Shader)
  // GET_PRIM_META(UsdPreviewSurface)
  // GET_PRIM_META(UsdUVTexture)
  // GET_PRIM_META(UsdPrimvarReader_int)
  // GET_PRIM_META(UsdPrimvarReader_float)
  // GET_PRIM_META(UsdPrimvarReader_float2)
  // GET_PRIM_META(UsdPrimvarReader_float3)
  // GET_PRIM_META(UsdPrimvarReader_float4)
  GET_PRIM_META(SkelRoot)
  GET_PRIM_META(Skeleton)
  GET_PRIM_META(SkelAnimation)
  GET_PRIM_META(BlendShape)
  GET_PRIM_META(PhysicsScene)
  GET_PRIM_META(PhysicsJoint)
  GET_PRIM_META(PhysicsRevoluteJoint)
  GET_PRIM_META(PhysicsPrismaticJoint)
  GET_PRIM_META(PhysicsSphericalJoint)
  GET_PRIM_META(PhysicsFixedJoint)
  GET_PRIM_META(PhysicsDistanceJoint)
  GET_PRIM_META(PhysicsCollisionGroup)
  GET_PRIM_META(MjcActuator)
  GET_PRIM_META(NewtonActuator)
  GET_PRIM_META(MjcTendon)
  GET_PRIM_META(MjcKeyframe)
  GET_PRIM_META(MjcSensor)

  GET_PRIM_META(NodeGraph)
  GET_PRIM_META(GeomPointInstancer)
  GET_PRIM_META(GeomNurbsCurves)
  GET_PRIM_META(GeomNurbsPatch)
  GET_PRIM_META(GeomHermiteCurves)
  GET_PRIM_META(GeomTetMesh)
  GET_PRIM_META(GeomCapsule_1)
  GET_PRIM_META(GeomCylinder_1)
  GET_PRIM_META(DomeLight_1)
  GET_PRIM_META(GeometryLight)
  GET_PRIM_META(PortalLight)
  GET_PRIM_META(LightFilter)
  GET_PRIM_META(PluginLightFilter)
  GET_PRIM_META(SpatialAudio)
  GET_PRIM_META(Preliminary_Action)
  GET_PRIM_META(Preliminary_Behavior)
  GET_PRIM_META(Preliminary_InfiniteColliderPlane)
  GET_PRIM_META(Preliminary_PhysicsGravitationalForce)
  GET_PRIM_META(Preliminary_ReferenceImage)
  GET_PRIM_META(Preliminary_Text)
  GET_PRIM_META(Preliminary_Trigger)

#undef GET_PRIM_META

  return nullptr;
}

PrimMeta *GetPrimMeta(value::Value &v) {
  // Lookup PrimMeta variable in Prim class

#define GET_PRIM_META(__ty)       \
  if (v.as<__ty>()) {             \
    return &(v.as<__ty>()->meta); \
  }

  GET_PRIM_META(Model)
  GET_PRIM_META(Scope)
  GET_PRIM_META(Volume)
  GET_PRIM_META(VolumeFieldBase)
  GET_PRIM_META(FieldBase)
  GET_PRIM_META(VolumeFieldAsset)
  GET_PRIM_META(OpenVDBAsset)
  GET_PRIM_META(Field3DAsset)
  GET_PRIM_META(RenderSettings)
  GET_PRIM_META(RenderProduct)
  GET_PRIM_META(RenderVar)
  GET_PRIM_META(GenerativeProcedural)
  GET_PRIM_META(Xform)
  GET_PRIM_META(GPrim)
  GET_PRIM_META(GeomMesh)
  GET_PRIM_META(Volume)
  GET_PRIM_META(VolumeFieldBase)
  GET_PRIM_META(FieldBase)
  GET_PRIM_META(VolumeFieldAsset)
  GET_PRIM_META(FieldAsset)
  GET_PRIM_META(OpenVDBAsset)
  GET_PRIM_META(Field3DAsset)
  GET_PRIM_META(GeomPoints)
  GET_PRIM_META(GeomCube)
  GET_PRIM_META(GeomCapsule)
  GET_PRIM_META(GeomCylinder)
  GET_PRIM_META(GeomSphere)
  GET_PRIM_META(GeomCone)
  GET_PRIM_META(GeomPlane)
  GET_PRIM_META(GeomSubset)
  GET_PRIM_META(GeomCamera)
  GET_PRIM_META(GeomBasisCurves)
  GET_PRIM_META(DomeLight)
  GET_PRIM_META(SphereLight)
  GET_PRIM_META(CylinderLight)
  GET_PRIM_META(DiskLight)
  GET_PRIM_META(DistantLight)
  GET_PRIM_META(RectLight)
  GET_PRIM_META(Material)
  GET_PRIM_META(Shader)
  // GET_PRIM_META(UsdPreviewSurface)
  // GET_PRIM_META(UsdUVTexture)
  // GET_PRIM_META(UsdPrimvarReader_int)
  // GET_PRIM_META(UsdPrimvarReader_float)
  // GET_PRIM_META(UsdPrimvarReader_float2)
  // GET_PRIM_META(UsdPrimvarReader_float3)
  // GET_PRIM_META(UsdPrimvarReader_float4)
  GET_PRIM_META(SkelRoot)
  GET_PRIM_META(Skeleton)
  GET_PRIM_META(SkelAnimation)
  GET_PRIM_META(BlendShape)
  GET_PRIM_META(PhysicsScene)
  GET_PRIM_META(PhysicsJoint)
  GET_PRIM_META(PhysicsRevoluteJoint)
  GET_PRIM_META(PhysicsPrismaticJoint)
  GET_PRIM_META(PhysicsSphericalJoint)
  GET_PRIM_META(PhysicsFixedJoint)
  GET_PRIM_META(PhysicsDistanceJoint)
  GET_PRIM_META(PhysicsCollisionGroup)
  GET_PRIM_META(MjcActuator)
  GET_PRIM_META(NewtonActuator)
  GET_PRIM_META(MjcTendon)
  GET_PRIM_META(MjcKeyframe)
  GET_PRIM_META(MjcSensor)

  GET_PRIM_META(NodeGraph)
  GET_PRIM_META(GeomPointInstancer)
  GET_PRIM_META(GeomNurbsCurves)
  GET_PRIM_META(GeomNurbsPatch)
  GET_PRIM_META(GeomHermiteCurves)
  GET_PRIM_META(GeomTetMesh)
  GET_PRIM_META(GeomCapsule_1)
  GET_PRIM_META(GeomCylinder_1)
  GET_PRIM_META(DomeLight_1)
  GET_PRIM_META(GeometryLight)
  GET_PRIM_META(PortalLight)
  GET_PRIM_META(LightFilter)
  GET_PRIM_META(PluginLightFilter)
  GET_PRIM_META(SpatialAudio)
  GET_PRIM_META(Preliminary_Action)
  GET_PRIM_META(Preliminary_Behavior)
  GET_PRIM_META(Preliminary_InfiniteColliderPlane)
  GET_PRIM_META(Preliminary_PhysicsGravitationalForce)
  GET_PRIM_META(Preliminary_ReferenceImage)
  GET_PRIM_META(Preliminary_Text)
  GET_PRIM_META(Preliminary_Trigger)

#undef GET_PRIM_META

  return nullptr;
}

nonstd::optional<std::string> GetPrimElementName(const value::Value &v) {
  // Since multiple get_value() call consumes lots of stack size(depends on
  // sizeof(T)?), Following code would produce 100KB of stack in debug build. So
  // use as() instead(as() => roughly 2000 bytes for stack size).

  // Lookup name field of Prim class

#define EXTRACT_NAME_AND_RETURN_PATH(__ty) \
  if (v.as<__ty>()) {                      \
    return v.as<__ty>()->name;             \
  } else

  EXTRACT_NAME_AND_RETURN_PATH(Model)
  EXTRACT_NAME_AND_RETURN_PATH(Scope)
  EXTRACT_NAME_AND_RETURN_PATH(Volume)
  EXTRACT_NAME_AND_RETURN_PATH(VolumeFieldBase)
  EXTRACT_NAME_AND_RETURN_PATH(FieldBase)
  EXTRACT_NAME_AND_RETURN_PATH(VolumeFieldAsset)
  EXTRACT_NAME_AND_RETURN_PATH(OpenVDBAsset)
  EXTRACT_NAME_AND_RETURN_PATH(Field3DAsset)
  EXTRACT_NAME_AND_RETURN_PATH(RenderSettings)
  EXTRACT_NAME_AND_RETURN_PATH(RenderProduct)
  EXTRACT_NAME_AND_RETURN_PATH(RenderVar)
  EXTRACT_NAME_AND_RETURN_PATH(GenerativeProcedural)
  EXTRACT_NAME_AND_RETURN_PATH(Xform)
  EXTRACT_NAME_AND_RETURN_PATH(GPrim)
  EXTRACT_NAME_AND_RETURN_PATH(GeomMesh)
  EXTRACT_NAME_AND_RETURN_PATH(Volume)
  EXTRACT_NAME_AND_RETURN_PATH(VolumeFieldBase)
  EXTRACT_NAME_AND_RETURN_PATH(FieldBase)
  EXTRACT_NAME_AND_RETURN_PATH(VolumeFieldAsset)
  EXTRACT_NAME_AND_RETURN_PATH(FieldAsset)
  EXTRACT_NAME_AND_RETURN_PATH(OpenVDBAsset)
  EXTRACT_NAME_AND_RETURN_PATH(Field3DAsset)
  EXTRACT_NAME_AND_RETURN_PATH(GeomPoints)
  EXTRACT_NAME_AND_RETURN_PATH(GeomCube)
  EXTRACT_NAME_AND_RETURN_PATH(GeomCapsule)
  EXTRACT_NAME_AND_RETURN_PATH(GeomCylinder)
  EXTRACT_NAME_AND_RETURN_PATH(GeomSphere)
  EXTRACT_NAME_AND_RETURN_PATH(GeomCone)
  EXTRACT_NAME_AND_RETURN_PATH(GeomSubset)
  EXTRACT_NAME_AND_RETURN_PATH(GeomCamera)
  EXTRACT_NAME_AND_RETURN_PATH(GeomBasisCurves)
  EXTRACT_NAME_AND_RETURN_PATH(GeomNurbsCurves)
  EXTRACT_NAME_AND_RETURN_PATH(GeomPlane)
  EXTRACT_NAME_AND_RETURN_PATH(GeomCylinder_1)
  EXTRACT_NAME_AND_RETURN_PATH(GeomCapsule_1)
  EXTRACT_NAME_AND_RETURN_PATH(GeomTetMesh)
  EXTRACT_NAME_AND_RETURN_PATH(GeomNurbsPatch)
  EXTRACT_NAME_AND_RETURN_PATH(GeomHermiteCurves)
  EXTRACT_NAME_AND_RETURN_PATH(GeomPointInstancer)
  EXTRACT_NAME_AND_RETURN_PATH(DomeLight)
  EXTRACT_NAME_AND_RETURN_PATH(SphereLight)
  EXTRACT_NAME_AND_RETURN_PATH(CylinderLight)
  EXTRACT_NAME_AND_RETURN_PATH(DiskLight)
  EXTRACT_NAME_AND_RETURN_PATH(DistantLight)
  EXTRACT_NAME_AND_RETURN_PATH(RectLight)
  EXTRACT_NAME_AND_RETURN_PATH(GeometryLight)
  EXTRACT_NAME_AND_RETURN_PATH(PortalLight)
  EXTRACT_NAME_AND_RETURN_PATH(DomeLight_1)
  EXTRACT_NAME_AND_RETURN_PATH(LightFilter)
  EXTRACT_NAME_AND_RETURN_PATH(PluginLightFilter)
  EXTRACT_NAME_AND_RETURN_PATH(Material)
  EXTRACT_NAME_AND_RETURN_PATH(NodeGraph)
  EXTRACT_NAME_AND_RETURN_PATH(ShaderNode)
  EXTRACT_NAME_AND_RETURN_PATH(Shader)
  // TODO: extract name must be handled in Shader class
  EXTRACT_NAME_AND_RETURN_PATH(UsdPreviewSurface)
  EXTRACT_NAME_AND_RETURN_PATH(UsdUVTexture)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_int)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_float)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_float2)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_float3)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_float4)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_string)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_normal)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_vector)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_point)
  EXTRACT_NAME_AND_RETURN_PATH(UsdPrimvarReader_matrix)
  //
  EXTRACT_NAME_AND_RETURN_PATH(SkelRoot)
  EXTRACT_NAME_AND_RETURN_PATH(Skeleton)
  EXTRACT_NAME_AND_RETURN_PATH(SkelAnimation)
  EXTRACT_NAME_AND_RETURN_PATH(BlendShape)
  // UsdPhysics + mjcPhysics
  EXTRACT_NAME_AND_RETURN_PATH(PhysicsJoint)
  EXTRACT_NAME_AND_RETURN_PATH(PhysicsScene)
  EXTRACT_NAME_AND_RETURN_PATH(PhysicsRevoluteJoint)
  EXTRACT_NAME_AND_RETURN_PATH(PhysicsPrismaticJoint)
  EXTRACT_NAME_AND_RETURN_PATH(PhysicsSphericalJoint)
  EXTRACT_NAME_AND_RETURN_PATH(PhysicsFixedJoint)
  EXTRACT_NAME_AND_RETURN_PATH(PhysicsDistanceJoint)
  EXTRACT_NAME_AND_RETURN_PATH(PhysicsCollisionGroup)
  EXTRACT_NAME_AND_RETURN_PATH(MjcActuator)
  EXTRACT_NAME_AND_RETURN_PATH(NewtonActuator)
  EXTRACT_NAME_AND_RETURN_PATH(MjcTendon)
  EXTRACT_NAME_AND_RETURN_PATH(MjcKeyframe)
  EXTRACT_NAME_AND_RETURN_PATH(MjcSensor)
  // AR/Interactive (Apple Preliminary_*)
  EXTRACT_NAME_AND_RETURN_PATH(Preliminary_PhysicsGravitationalForce)
  EXTRACT_NAME_AND_RETURN_PATH(Preliminary_InfiniteColliderPlane)
  EXTRACT_NAME_AND_RETURN_PATH(Preliminary_ReferenceImage)
  EXTRACT_NAME_AND_RETURN_PATH(Preliminary_Behavior)
  EXTRACT_NAME_AND_RETURN_PATH(Preliminary_Trigger)
  EXTRACT_NAME_AND_RETURN_PATH(Preliminary_Action)
  EXTRACT_NAME_AND_RETURN_PATH(Preliminary_Text)
  // usdMedia
  EXTRACT_NAME_AND_RETURN_PATH(SpatialAudio) { return nonstd::nullopt; }

#undef EXTRACT_NAME_AND_RETURN_PATH

}

bool SetPrimElementName(value::Value &v, const std::string &elementName) {
  // Lookup name field of Prim class
  bool ok{false};

#define SET_ELEMENT_NAME(__name, __ty) \
  if (v.as<__ty>()) {                  \
    v.as<__ty>()->name = __name;       \
    ok = true;                         \
  } else

  SET_ELEMENT_NAME(elementName, Model)
  SET_ELEMENT_NAME(elementName, Scope)
  SET_ELEMENT_NAME(elementName, Volume)
  SET_ELEMENT_NAME(elementName, VolumeFieldBase)
  SET_ELEMENT_NAME(elementName, FieldBase)
  SET_ELEMENT_NAME(elementName, VolumeFieldAsset)
  SET_ELEMENT_NAME(elementName, OpenVDBAsset)
  SET_ELEMENT_NAME(elementName, Field3DAsset)
  SET_ELEMENT_NAME(elementName, RenderSettings)
  SET_ELEMENT_NAME(elementName, RenderProduct)
  SET_ELEMENT_NAME(elementName, RenderVar)
  SET_ELEMENT_NAME(elementName, GenerativeProcedural)
  SET_ELEMENT_NAME(elementName, Xform)
  SET_ELEMENT_NAME(elementName, GPrim)
  SET_ELEMENT_NAME(elementName, GeomMesh)
  SET_ELEMENT_NAME(elementName, Volume)
  SET_ELEMENT_NAME(elementName, VolumeFieldBase)
  SET_ELEMENT_NAME(elementName, FieldBase)
  SET_ELEMENT_NAME(elementName, VolumeFieldAsset)
  SET_ELEMENT_NAME(elementName, FieldAsset)
  SET_ELEMENT_NAME(elementName, OpenVDBAsset)
  SET_ELEMENT_NAME(elementName, Field3DAsset)
  SET_ELEMENT_NAME(elementName, GeomPoints)
  SET_ELEMENT_NAME(elementName, GeomCube)
  SET_ELEMENT_NAME(elementName, GeomCapsule)
  SET_ELEMENT_NAME(elementName, GeomCylinder)
  SET_ELEMENT_NAME(elementName, GeomSphere)
  SET_ELEMENT_NAME(elementName, GeomCone)
  SET_ELEMENT_NAME(elementName, GeomSubset)
  SET_ELEMENT_NAME(elementName, GeomCamera)
  SET_ELEMENT_NAME(elementName, GeomBasisCurves)
  SET_ELEMENT_NAME(elementName, GeomNurbsCurves)
  SET_ELEMENT_NAME(elementName, GeomPlane)
  SET_ELEMENT_NAME(elementName, GeomCylinder_1)
  SET_ELEMENT_NAME(elementName, GeomCapsule_1)
  SET_ELEMENT_NAME(elementName, GeomTetMesh)
  SET_ELEMENT_NAME(elementName, GeomNurbsPatch)
  SET_ELEMENT_NAME(elementName, GeomHermiteCurves)
  SET_ELEMENT_NAME(elementName, GeomPointInstancer)
  SET_ELEMENT_NAME(elementName, DomeLight)
  SET_ELEMENT_NAME(elementName, SphereLight)
  SET_ELEMENT_NAME(elementName, CylinderLight)
  SET_ELEMENT_NAME(elementName, DistantLight)
  SET_ELEMENT_NAME(elementName, DiskLight)
  SET_ELEMENT_NAME(elementName, RectLight)
  SET_ELEMENT_NAME(elementName, GeometryLight)
  SET_ELEMENT_NAME(elementName, PortalLight)
  SET_ELEMENT_NAME(elementName, DomeLight_1)
  SET_ELEMENT_NAME(elementName, LightFilter)
  SET_ELEMENT_NAME(elementName, PluginLightFilter)
  SET_ELEMENT_NAME(elementName, Material)
  SET_ELEMENT_NAME(elementName, NodeGraph)
  SET_ELEMENT_NAME(elementName, ShaderNode)
  SET_ELEMENT_NAME(elementName, Shader)
  // TODO: set element name must be handled in Shader class
  SET_ELEMENT_NAME(elementName, UsdPreviewSurface)
  SET_ELEMENT_NAME(elementName, UsdUVTexture)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_int)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_float)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_float2)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_float3)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_float4)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_string)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_normal)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_vector)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_point)
  SET_ELEMENT_NAME(elementName, UsdPrimvarReader_matrix)
  //
  SET_ELEMENT_NAME(elementName, SkelRoot)
  SET_ELEMENT_NAME(elementName, Skeleton)
  SET_ELEMENT_NAME(elementName, SkelAnimation)
  SET_ELEMENT_NAME(elementName, BlendShape)
  // UsdPhysics + mjcPhysics
  SET_ELEMENT_NAME(elementName, PhysicsJoint)
  SET_ELEMENT_NAME(elementName, PhysicsScene)
  SET_ELEMENT_NAME(elementName, PhysicsRevoluteJoint)
  SET_ELEMENT_NAME(elementName, PhysicsPrismaticJoint)
  SET_ELEMENT_NAME(elementName, PhysicsSphericalJoint)
  SET_ELEMENT_NAME(elementName, PhysicsFixedJoint)
  SET_ELEMENT_NAME(elementName, PhysicsDistanceJoint)
  SET_ELEMENT_NAME(elementName, PhysicsCollisionGroup)
  SET_ELEMENT_NAME(elementName, MjcActuator)
  SET_ELEMENT_NAME(elementName, NewtonActuator)
  SET_ELEMENT_NAME(elementName, MjcTendon)
  SET_ELEMENT_NAME(elementName, MjcKeyframe)
  SET_ELEMENT_NAME(elementName, MjcSensor)
  // AR/Interactive (Apple Preliminary_*)
  SET_ELEMENT_NAME(elementName, Preliminary_PhysicsGravitationalForce)
  SET_ELEMENT_NAME(elementName, Preliminary_InfiniteColliderPlane)
  SET_ELEMENT_NAME(elementName, Preliminary_ReferenceImage)
  SET_ELEMENT_NAME(elementName, Preliminary_Behavior)
  SET_ELEMENT_NAME(elementName, Preliminary_Trigger)
  SET_ELEMENT_NAME(elementName, Preliminary_Action)
  SET_ELEMENT_NAME(elementName, Preliminary_Text)
  // usdMedia
  SET_ELEMENT_NAME(elementName, SpatialAudio) { return false; }

#undef SET_ELEMENT_NAME

  return ok;
}

bool CastToXformable(const Prim &prim, const Xformable **xformable) {
  if (!xformable) {
    return false;
  }

  // __ty = class derived from Xformable.
#define TRY_CAST(__ty)             \
  if (auto pv = prim.as<__ty>()) { \
    (*xformable) = pv;             \
    return true;                   \
  }

  // TODO: Use tydra::ApplyToXformable
  TRY_CAST(GPrim)
  TRY_CAST(Xform)
  TRY_CAST(GeomMesh)
  TRY_CAST(Volume)
  TRY_CAST(VolumeFieldBase)
  TRY_CAST(FieldBase)
  TRY_CAST(VolumeFieldAsset)
  TRY_CAST(FieldAsset)
  TRY_CAST(OpenVDBAsset)
  TRY_CAST(Field3DAsset)
  TRY_CAST(GeomBasisCurves)
  TRY_CAST(GeomCube)
  TRY_CAST(GeomSphere)
  TRY_CAST(GeomCylinder)
  TRY_CAST(GeomCone)
  TRY_CAST(GeomCapsule)
  TRY_CAST(GeomPoints)
  TRY_CAST(GeomPointInstancer)
  TRY_CAST(GeomCamera)
  TRY_CAST(SkelRoot)
  TRY_CAST(Skeleton)
  TRY_CAST(RectLight)
  TRY_CAST(DomeLight)
  TRY_CAST(CylinderLight)
  TRY_CAST(SphereLight)
  TRY_CAST(DiskLight)
  TRY_CAST(DistantLight)
  TRY_CAST(RectLight)
  TRY_CAST(GeometryLight)
  TRY_CAST(PortalLight)
  TRY_CAST(PluginLight)
  TRY_CAST(SkelRoot)
  TRY_CAST(Skeleton)

  return false;
}

value::matrix4d GetLocalTransform(const Prim &prim, bool *resetXformStack,
                                  double t,
                                  value::TimeSampleInterpolationType tinterp) {
  if (!IsXformablePrim(prim)) {
    if (resetXformStack) {
      (*resetXformStack) = false;
    }
    return value::matrix4d::identity();
  }

  // default false
  if (resetXformStack) {
    (*resetXformStack) = false;
  }

  const Xformable *xformable{nullptr};
  if (CastToXformable(prim, &xformable)) {
    if (!xformable) {
      return value::matrix4d::identity();
    }

    bool rxs{false};
    nonstd::expected<value::matrix4d, std::string> ret =
        xformable->GetLocalMatrix(t, tinterp, &rxs);
    if (ret) {
      if (resetXformStack) {
        (*resetXformStack) = rxs;
      }
      return ret.value();
    }
  }

  return value::matrix4d::identity();
}


}  // namespace lightusd
