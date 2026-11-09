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

// inputs:texture:format ("automatic"/"latlong"/"mirroredBall"/"angular") for
// DomeLight / DomeLight_1. Not covered by DOME_LIGHT_TYPED_ATTRS (enum-token
// attrs need an enum handler); without this the attribute fell through to the
// generic props and DomeLight::textureFormat stayed unauthored.
[[maybe_unused]] static nonstd::expected<DomeLight::TextureFormat, std::string>
DomeLightTextureFormatEnumHandler(const std::string &tok) {
  DomeLight::TextureFormat fmt;
  if (DomeLight_TextureFormat_from_string(tok, &fmt)) {
    return fmt;
  }
  return nonstd::make_unexpected("Invalid inputs:texture:format token: " + tok);
}

[[maybe_unused]] static nonstd::expected<DomeLight_1::TextureFormat, std::string>
DomeLight1TextureFormatEnumHandler(const std::string &tok) {
  DomeLight::TextureFormat fmt;
  if (DomeLight_TextureFormat_from_string(tok, &fmt)) {
    // Same enumerator set/order as DomeLight::TextureFormat.
    return static_cast<DomeLight_1::TextureFormat>(fmt);
  }
  return nonstd::make_unexpected("Invalid inputs:texture:format token: " + tok);
}

#define RECONSTRUCT_LIGHT_PRIM_BODY(LightClass, light_ptr, TYPED_ATTRS, COMMON_ATTRS, EXTENT_HANDLING, SPECIAL_HANDLING) \
  (void)references; \
  \
  ParsePropertyTable table; \
  \
  if (!prim::ReconstructXformOpsFromProperties(spec, table, properties, &light_ptr->xformOps, err)) { \
    return false; \
  } \
  \
  /* Lights inherit Collection (BoundableLight/NonboundableLight): reconstruct */ \
  /* collection:* instances (incl. lightLink / shadowLink) into the typed map. */ \
  if (!prim::ReconstructCollectionProperties(table, properties, light_ptr, warn, err, options.strict_allowedToken_check)) { \
    return false; \
  } \
  \
  for (auto &prop : properties) { \
    SPECIAL_HANDLING \
    TYPED_ATTRS(EXPAND_TYPED_ATTR) \
    COMMON_ATTRS(EXPAND_TYPED_ATTR) \
    EXTENT_HANDLING \
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, kVisibility, Visibility, VisibilityEnumHandler, LightClass, \
                       light_ptr->visibility, options.strict_allowedToken_check) \
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, kPurpose, Purpose, PurposeEnumHandler, LightClass, \
                       light_ptr->purpose, options.strict_allowedToken_check) \
    PARSE_TARGET_PATHS_RELATION(table, prop, "light:filters", light_ptr->lightFilters) \
    ADD_PROPERTY(table, prop, LightClass, light_ptr->props) \
    PARSE_PROPERTY_END_MAKE_WARN(table, prop) \
  } \
  \
  return true;

template <>
bool ReconstructPrim<SphereLight>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    SphereLight *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ SphereLight
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_LIGHT_PRIM_BODY(SphereLight, light, SPHERE_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_WITH_SHAPING,
                              PARSE_EXTENT_ATTRIBUTE(table, prop, kExtent, SphereLight, light->extent),
                              /* no special handling */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<RectLight>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    RectLight *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ RectLight
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  // Special case: texture:file uses UsdUVTexture type
  RECONSTRUCT_LIGHT_PRIM_BODY(RectLight, light, RECT_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_WITH_SHAPING,
                              PARSE_EXTENT_ATTRIBUTE(table, prop, kExtent, RectLight, light->extent),
                              PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:texture:file", UsdUVTexture, light->file))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<DiskLight>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    DiskLight *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ DiskLight
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_LIGHT_PRIM_BODY(DiskLight, light, DISK_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_WITH_SHAPING,
                              PARSE_EXTENT_ATTRIBUTE(table, prop, kExtent, DiskLight, light->extent),
                              /* no special handling */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<CylinderLight>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    CylinderLight *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ CylinderLight
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_LIGHT_PRIM_BODY(CylinderLight, light, CYLINDER_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_WITH_SHAPING,
                              PARSE_EXTENT_ATTRIBUTE(table, prop, kExtent, CylinderLight, light->extent),
                              /* no special handling */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<DistantLight>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    DistantLight *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ DistantLight
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_LIGHT_PRIM_BODY(DistantLight, light, DISTANT_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_NO_SHAPING,
                              /* no extent */,
                              /* no special handling */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<GeometryLight>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeometryLight *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeometryLight
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_LIGHT_PRIM_BODY(GeometryLight, light, GEOMETRY_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_NO_SHAPING,
                              /* no extent */,
                              /* no special handling */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<PortalLight>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    PortalLight *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ PortalLight
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_LIGHT_PRIM_BODY(PortalLight, light, GEOMETRY_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_NO_SHAPING,
                              /* no extent */,
                              /* no special handling */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<DomeLight>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    DomeLight *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ DomeLight
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  DCOUT("Implement DomeLight");
  RECONSTRUCT_LIGHT_PRIM_BODY(DomeLight, light, DOME_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_NO_SHAPING,
                              /* no extent */,
                              PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, "inputs:texture:format",
                                  DomeLight::TextureFormat, DomeLightTextureFormatEnumHandler,
                                  DomeLight, light->textureFormat, options.strict_allowedToken_check))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<DomeLight_1>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    DomeLight_1 *light,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ DomeLight_1
#define PRIM_PTR_ light
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_LIGHT_PRIM_BODY(DomeLight_1, light, DOME_LIGHT_TYPED_ATTRS, LIGHT_COMMON_ATTRS_NO_SHAPING,
                              /* no extent */,
                              PARSE_TYPED_ATTRIBUTE(table, prop, "poleAxis", DomeLight_1, light->poleAxis)
                              PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, "inputs:texture:format",
                                  DomeLight_1::TextureFormat, DomeLight1TextureFormatEnumHandler,
                                  DomeLight_1, light->textureFormat, options.strict_allowedToken_check))
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<LightFilter>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    LightFilter *filter,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)references;

  ParsePropertyTable table;

  if (!prim::ReconstructXformOpsFromProperties(spec, table, properties, &filter->xformOps, err)) {
    return false;
  }

  for (auto &prop : properties) {
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, kVisibility, Visibility, VisibilityEnumHandler, LightFilter,
                       filter->visibility, options.strict_allowedToken_check)
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, kPurpose, Purpose, PurposeEnumHandler, LightFilter,
                       filter->purpose, options.strict_allowedToken_check)
    ADD_PROPERTY(table, prop, LightFilter, filter->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<PluginLightFilter>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    PluginLightFilter *filter,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)references;

  ParsePropertyTable table;

  if (!prim::ReconstructXformOpsFromProperties(spec, table, properties, &filter->xformOps, err)) {
    return false;
  }

  for (auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "light:shaderId", PluginLightFilter, filter->shaderId)
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, kVisibility, Visibility, VisibilityEnumHandler, PluginLightFilter,
                       filter->visibility, options.strict_allowedToken_check)
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, kPurpose, Purpose, PurposeEnumHandler, PluginLightFilter,
                       filter->purpose, options.strict_allowedToken_check)
    ADD_PROPERTY(table, prop, PluginLightFilter, filter->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

}  // namespace prim
}  // namespace lightusd
