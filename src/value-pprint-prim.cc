// SPDX-License-Identifier: Apache 2.0
// Copyright present, Light Transport Entertainment, Inc.
//
// Schema-prim pretty-print — split out of value-pprint-dispatch.cc.
// pprint_prim_value() renders a value::Value that holds a schema *prim* type
// (Model/Scope/Xform/GeomMesh/.../Material/Shader/Physics*/SpatialAudio) by
// dispatching v.as<PrimType>() -> to_string(). These ~60 per-prim-type
// instantiations + the schema headers they need are isolated here so the
// base-type/array renderer (pprint_value in value-pprint-dispatch.cc) stays light.
// pprint_value() forwards here for type_ids in [MODEL_BEGIN, MODEL_END).
#include "value-pprint.hh"

#include <sstream>

#include "pprinter.hh"
#include "core/prim.hh"
#include "core/model-scope.hh"  // Model/Scope + UsdVol/UsdRender placeholders
#include "str-util.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "value-types.hh"

#include "common-macros.inc"

namespace lightusd {
namespace value {

#define CASE_GPRIM_LIST(__FUNC) \
  __FUNC(Model)                 \
  __FUNC(Scope)                 \
  __FUNC(Xform)                 \
  __FUNC(GeomMesh)              \
  __FUNC(GeomSphere)            \
  __FUNC(GeomSubset)            \
  __FUNC(GeomPoints)            \
  __FUNC(GeomCube)              \
  __FUNC(GeomCylinder)          \
  __FUNC(GeomCapsule)           \
  __FUNC(GeomCone)              \
  __FUNC(GeomBasisCurves)       \
  __FUNC(GeomNurbsCurves)       \
  __FUNC(GeomPlane)             \
  __FUNC(GeomCylinder_1)        \
  __FUNC(GeomCapsule_1)         \
  __FUNC(GeomTetMesh)           \
  __FUNC(GeomNurbsPatch)        \
  __FUNC(GeomHermiteCurves)     \
  __FUNC(GeomCamera)            \
  __FUNC(GeomPointInstancer)        \
  __FUNC(Volume)                \
  __FUNC(VolumeFieldBase)       \
  __FUNC(FieldBase)             \
  __FUNC(VolumeFieldAsset)      \
  __FUNC(FieldAsset)            \
  __FUNC(OpenVDBAsset)          \
  __FUNC(Field3DAsset)          \
  __FUNC(SphereLight)           \
  __FUNC(DomeLight)             \
  __FUNC(DiskLight)             \
  __FUNC(DistantLight)          \
  __FUNC(CylinderLight)         \
  __FUNC(RectLight)             \
  __FUNC(GeometryLight)         \
  __FUNC(PortalLight)           \
  __FUNC(DomeLight_1)           \
  __FUNC(LightFilter)           \
  __FUNC(PluginLightFilter)     \
  __FUNC(SkelRoot)              \
  __FUNC(Skeleton)              \
  __FUNC(SkelAnimation)         \
  __FUNC(BlendShape)            \
  __FUNC(Material)              \
  __FUNC(Shader)                \
  __FUNC(NodeGraph)             \
  __FUNC(RenderSettings)        \
  __FUNC(RenderProduct)         \
  __FUNC(RenderVar)             \
  __FUNC(GenerativeProcedural)  \
  __FUNC(PhysicsJoint)           \
  __FUNC(PhysicsScene)          \
  __FUNC(PhysicsRevoluteJoint)  \
  __FUNC(PhysicsPrismaticJoint) \
  __FUNC(PhysicsSphericalJoint) \
  __FUNC(PhysicsFixedJoint)     \
  __FUNC(PhysicsDistanceJoint)  \
  __FUNC(PhysicsCollisionGroup) \
  __FUNC(MjcActuator)           \
  __FUNC(NewtonActuator)        \
  __FUNC(MjcTendon)             \
  __FUNC(MjcKeyframe)           \
  __FUNC(MjcSensor)             \
  __FUNC(Preliminary_PhysicsGravitationalForce) \
  __FUNC(Preliminary_InfiniteColliderPlane) \
  __FUNC(Preliminary_ReferenceImage) \
  __FUNC(Preliminary_Behavior)  \
  __FUNC(Preliminary_Trigger)   \
  __FUNC(Preliminary_Action)    \
  __FUNC(Preliminary_Text)      \
  __FUNC(SpatialAudio)

std::string pprint_prim_value(const value::Value &v, const uint32_t indent,
                              bool closing_brace) {
#define PRIMTYPE_CASE_EXPR(__ty)                           \
  case TypeTraits<__ty>::type_id(): {                      \
    auto p = v.as<__ty>();                                 \
    if (p) {                                               \
      os << to_string(*p, indent, closing_brace);          \
    } else {                                               \
      os << "[InternalError: Prim type TypeId mismatch.]"; \
    }                                                      \
    break;                                                 \
  }

  std::stringstream os;
  switch (v.type_id()) {
    CASE_GPRIM_LIST(PRIMTYPE_CASE_EXPR)
    default: {
      os << "[InternalError: pprint_prim_value called on non-prim type_id "
         << v.type_id() << "]";
      break;
    }
  }
#undef PRIMTYPE_CASE_EXPR
#undef CASE_GPRIM_LIST
  return os.str();
}

}  // namespace value
}  // namespace lightusd
