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
#include "prim-reconstruct-geom-detail.inc"  // ReconstructGPrimProperties

// Heavy ReconstructPrim<T> PropertyMap-overload specializations (curves) peeled from
// prim-reconstruct.cc to divide back-end codegen; PrimSpec wrappers stay in prim-reconstruct.cc.

template <>
bool ReconstructPrim(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomBasisCurves *curves,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)references;

  DCOUT("GeomBasisCurves");

  // Use centralized enum handlers
  auto BasisHandler = enum_handler::BasisCurvesBasis;
  auto TypeHandler = enum_handler::BasisCurvesType;
  auto WrapHandler = enum_handler::BasisCurvesWrap;

  ParsePropertyTable table;
  if (!ReconstructGPrimProperties(spec, table, properties, curves, warn, err, options.strict_allowedToken_check)) {
    return false;
  }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeomBasisCurves
#define PRIM_PTR_ curves
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    GEOM_BASIS_CURVES_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    GEOM_BASIS_CURVES_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM)
    ADD_PROPERTY(table, prop, GeomBasisCurves, curves->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

#undef PRIM_CLASS_
#undef PRIM_PTR_

  return true;
}

template <>
bool ReconstructPrim(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomNurbsCurves *curves,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)references;
  (void)options;

  ParsePropertyTable table;
  if (!ReconstructGPrimProperties(spec, table, properties, curves, warn, err, options.strict_allowedToken_check)) {
    return false;
  }

  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    PARSE_TYPED_ATTRIBUTE(table, prop, "curveVertexCounts", GeomNurbsCurves,
                         curves->curveVertexCounts)
    PARSE_TYPED_ATTRIBUTE(table, prop, "points", GeomNurbsCurves, curves->points)
    PARSE_TYPED_ATTRIBUTE(table, prop, "velocities", GeomNurbsCurves,
                          curves->velocities)
    PARSE_TYPED_ATTRIBUTE(table, prop, "normals", GeomNurbsCurves,
                  curves->normals)
    PARSE_TYPED_ATTRIBUTE(table, prop, "accelerations", GeomNurbsCurves,
                 curves->accelerations)
    PARSE_TYPED_ATTRIBUTE(table, prop, "widths", GeomNurbsCurves, curves->widths)

    //
    PARSE_TYPED_ATTRIBUTE(table, prop, "order", GeomNurbsCurves, curves->order)
    PARSE_TYPED_ATTRIBUTE(table, prop, "knots", GeomNurbsCurves, curves->knots)
    PARSE_TYPED_ATTRIBUTE(table, prop, "ranges", GeomNurbsCurves, curves->ranges)
    PARSE_TYPED_ATTRIBUTE(table, prop, "pointWeights", GeomNurbsCurves, curves->pointWeights)

    ADD_PROPERTY(table, prop, GeomNurbsCurves, curves->props)

    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

// ============================================================================
// Generic macro for light prim reconstruction
// ============================================================================
// Consolidates the common pattern for all light types:
// SphereLight, RectLight, DiskLight, CylinderLight, DistantLight, GeometryLight, DomeLight
//
// IMPORTANT: Caller must define PRIM_CLASS_ and PRIM_PTR_ macros before calling
//            this macro, and undef them afterward. These are required by
//            EXPAND_TYPED_ATTR macros.
//
// Parameters:
//   LightClass: The light class (e.g., SphereLight, RectLight)
//   light_ptr: Pointer to the light instance
//   TYPED_ATTRS: Property table macro (e.g., SPHERE_LIGHT_TYPED_ATTRS)
//   COMMON_ATTRS: Light common attrs macro (LIGHT_COMMON_ATTRS_WITH_SHAPING or LIGHT_COMMON_ATTRS_NO_SHAPING)
//   EXTENT_HANDLING: Either PARSE_EXTENT_ATTRIBUTE(...) or /* no extent */
//   SPECIAL_HANDLING: Special attribute handling for exceptions like RectLight's texture:file or /* no special handling */

// ============================================================================
// Generic macro for simple geometry prim reconstruction
// ============================================================================
// Consolidates the common pattern for GeomSphere, GeomCone, GeomCylinder,
// GeomCapsule, GeomCube: ReconstructGPrimProperties + property loop
//
// IMPORTANT: Caller must define PRIM_CLASS_ and PRIM_PTR_ macros before calling
//            this macro, and undef them afterward. These are required by
//            EXPAND_TYPED_ATTR and EXPAND_UNIFORM_ENUM macros.
//
// Parameters:
//   PrimClass: The geometry class (e.g., GeomSphere, GeomCone)
//   prim_ptr: Pointer to the prim instance
//   TYPED_ATTRS: Property table macro (e.g., GEOM_SPHERE_TYPED_ATTRS)
//   ENUM_EXPANSION: Enum handling macro call or empty
//                   - For shapes without enums: /* empty */
//                   - For shapes with enums: GEOM_XXX_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM)



// Shader/Material/NodeGraph reconstruction moved to prim-reconstruct-shader.cc
// Physics + MuJoCo reconstruction moved to prim-reconstruct-physics.cc


///
/// -- PrimSpec
///



}  // namespace prim
}  // namespace lightusd
