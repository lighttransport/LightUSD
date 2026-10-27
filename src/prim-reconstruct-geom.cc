// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Reconstruct concrete Prim from PropertyMap or PrimSpec.
//
// TODO:
//   - [ ] Refactor code
//
#include "prim-reconstruct.hh"

#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "str-util.hh"
#include "io-util.hh"
#include "tiny-format.hh"
#include "enum-handlers.hh"
#include "prim-property-tables.hh"

#include "usdGeom.hh"
#include "usdSkel.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usdMtlx.hh"

#include "common-macros.inc"
#include "value-types.hh"

// For PUSH_ERROR_AND_RETURN
#define PushError(s) \
  if (err) { \
    (*err) = (s) + (err->empty() ? std::string() : std::string("\n")) + (*err); \
  }
#define PushWarn(s) \
  if (warn) { \
    (*warn) = (s) + (warn->empty() ? std::string() : std::string("\n")) + (*warn); \
  }

// __VA_ARGS__ does not allow empty, thus # of args must be 2+
#define PUSH_WARN_F(s, ...) PUSH_WARN(fmt::format(s, __VA_ARGS__))

//
// NOTE:
//
// There are mainly 5 variant of Primtive property(relationship/attribute)
//
// - TypedAttribute<T> : Uniform only. `uniform T` or `uniform T var.connect`
// - TypedAttribute<Animatable<T>> : Varying. `T var`, `T var = val`, `T var.connect` or `T value.timeSamples`
// - optional<T> : For output attribute(Just author it. e.g. `float outputs:rgb`)
// - Relationship : Typeless relation(e.g. `rel material:binding`)
// - TypedConnection : Typed relation(e.g. `token outputs:result = </material/diffuse.rgb>`)

namespace lightusd {
namespace prim {

//constexpr auto kTag = "[PrimReconstruct]";

[[maybe_unused]] constexpr auto kProxyPrim = "proxyPrim";
[[maybe_unused]] constexpr auto kVisibility = "visibility";
[[maybe_unused]] constexpr auto kExtent = "extent";
[[maybe_unused]] constexpr auto kPurpose = "purpose";
[[maybe_unused]] constexpr auto kMaterialBinding = "material:binding";
[[maybe_unused]] constexpr auto kMaterialBindingCollection = "material:binding:collection";
[[maybe_unused]] constexpr auto kMaterialBindingPreview = "material:binding:preview";
[[maybe_unused]] constexpr auto kSkelSkeleton = "skel:skeleton";
[[maybe_unused]] constexpr auto kSkelAnimationSource = "skel:animationSource";
[[maybe_unused]] constexpr auto kSkelBlendShapes = "skel:blendShapes";
[[maybe_unused]] constexpr auto kSkelBlendShapeTargets = "skel:blendShapeTargets";
// kInputsVarname moved to prim-reconstruct-shader.cc

// MaterialX Validation Helpers moved to prim-reconstruct-shader.cc


///
/// LightUSD reconstruct some frequently used shaders(e.g. UsdPreviewSurface)
/// here, not in Tydra
///
template <typename T>
bool ReconstructShader(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    T *out,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options);


#include "prim-reconstruct-common.inc"

#include "prim-reconstruct-geom-detail.inc"

template <>
bool ReconstructPrim<GeomSphere>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomSphere *sphere,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  DCOUT("Reconstruct Sphere.");
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeomSphere
#define PRIM_PTR_ sphere
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomSphere, sphere, GEOM_SPHERE_TYPED_ATTRS, /* no enums */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomPoints>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomPoints *points,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {

  (void)references;

  DCOUT("Reconstruct Points.");

  ParsePropertyTable table;
  if (!ReconstructGPrimProperties(spec, table, properties, points, warn, err, options.strict_allowedToken_check)) {
    return false;
  }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeomPoints
#define PRIM_PTR_ points
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    DCOUT("prop: " << prop.first);
    GEOM_POINTS_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, GeomPoints, points->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

#undef PRIM_CLASS_
#undef PRIM_PTR_

  return true;
}

template <>
bool ReconstructPrim<GeomCone>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomCone *cone,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeomCone
#define PRIM_PTR_ cone
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomCone, cone, GEOM_CONE_TYPED_ATTRS,
                                     GEOM_CONE_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomCylinder>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomCylinder *cylinder,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeomCylinder
#define PRIM_PTR_ cylinder
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomCylinder, cylinder, GEOM_CYLINDER_TYPED_ATTRS,
                                     GEOM_CYLINDER_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomCapsule>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomCapsule *capsule,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeomCapsule
#define PRIM_PTR_ capsule
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomCapsule, capsule, GEOM_CAPSULE_TYPED_ATTRS,
                                     GEOM_CAPSULE_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomCube>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomCube *cube,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  // pxrUSD says... "If you author size you must also author extent."
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeomCube
#define PRIM_PTR_ cube
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomCube, cube, GEOM_CUBE_TYPED_ATTRS, /* no enums */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomPlane>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    GeomPlane *plane, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
#define PRIM_CLASS_ GeomPlane
#define PRIM_PTR_ plane
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomPlane, plane, GEOM_PLANE_TYPED_ATTRS,
                                     GEOM_PLANE_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomCylinder_1>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    GeomCylinder_1 *cyl, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
#define PRIM_CLASS_ GeomCylinder_1
#define PRIM_PTR_ cyl
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomCylinder_1, cyl, GEOM_CYLINDER_1_TYPED_ATTRS,
                                     GEOM_CYLINDER_1_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomCapsule_1>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    GeomCapsule_1 *cap, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
#define PRIM_CLASS_ GeomCapsule_1
#define PRIM_PTR_ cap
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomCapsule_1, cap, GEOM_CAPSULE_1_TYPED_ATTRS,
                                     GEOM_CAPSULE_1_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomTetMesh>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    GeomTetMesh *tetmesh, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
#define PRIM_PTR_ tetmesh
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomTetMesh, tetmesh, GEOM_TET_MESH_TYPED_ATTRS, /* no enums */)
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomNurbsPatch>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    GeomNurbsPatch *patch, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
#define PRIM_PTR_ patch
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomNurbsPatch, patch, GEOM_NURBS_PATCH_TYPED_ATTRS, /* no enums */)
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeomHermiteCurves>(
    const Specifier &spec, PropertyMap &properties, const ReferenceList &references,
    GeomHermiteCurves *curves, std::string *warn, std::string *err,
    const PrimReconstructOptions &options) {
#define PRIM_PTR_ curves
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(GeomHermiteCurves, curves, GEOM_HERMITE_CURVES_TYPED_ATTRS, /* no enums */)
#undef PRIM_PTR_
}

}  // namespace prim
}  // namespace lightusd
