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

// Heavy ReconstructPrim<T> PropertyMap-overload specializations (skel) peeled from
// prim-reconstruct.cc to divide back-end codegen; PrimSpec wrappers stay in prim-reconstruct.cc.

template <>
bool ReconstructPrim<SkelRoot>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    SkelRoot *root,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {

  (void)references;
  (void)options;

  ParsePropertyTable table;
  if (!prim::ReconstructXformOpsFromProperties(spec, table, properties, &root->xformOps, err)) {
    return false;
  }

  // SkelRoot is something like a grouping node, having 1 Skeleton and possibly
  // multiple Prim hierarchy containing GeomMesh.
  // SkelBindingAPI properties (skel:animationSource, skel:skeleton) can be
  // authored on SkelRoot and inherited by child prims per the USD spec.

  for (auto &prop : properties) {  // Non-const to allow move from property metadata

    // SkelBindingAPI: animationSource relationship
    if (prop.first == kSkelAnimationSource) {
      if (prop.second.is_relationship()) {
        const Relationship &rel = prop.second.get_relationship();
        if (rel.is_path() || rel.is_pathvector()) {
          root->animationSource = rel;
          table.insert(kSkelAnimationSource);
        } else {
          PUSH_WARN("`" << kSkelAnimationSource << "` target must be Path.");
        }
      }
    }

    // SkelBindingAPI: skeleton relationship
    if (prop.first == kSkelSkeleton) {
      if (prop.second.is_relationship()) {
        const Relationship &rel = prop.second.get_relationship();
        if (rel.is_path() || rel.is_pathvector()) {
          root->skeleton = rel;
          table.insert(kSkelSkeleton);
        } else {
          PUSH_WARN("`" << kSkelSkeleton << "` target must be Path.");
        }
      }
    }

    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, kVisibility, Visibility, VisibilityEnumHandler, SkelRoot,
                   root->visibility, options.strict_allowedToken_check)
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, kPurpose, Purpose, PurposeEnumHandler, SkelRoot,
                       root->purpose, options.strict_allowedToken_check)
    PARSE_EXTENT_ATTRIBUTE(table, prop, kExtent, SkelRoot, root->extent)
    ADD_PROPERTY(table, prop, SkelRoot, root->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<Skeleton>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    Skeleton *skel,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {

  (void)warn;
  (void)references;

  ParsePropertyTable table;
  if (!prim::ReconstructXformOpsFromProperties(spec, table, properties, &skel->xformOps, err)) {
    return false;
  }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ Skeleton
#define PRIM_PTR_ skel
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  for (auto &prop : properties) {

    // SkelBindingAPI: animationSource relationship
    if (prop.first == kSkelAnimationSource) {
      // Must be relation of type Path.
      if (prop.second.is_relationship() && prop.second.get_relationship().is_path()) {
        const Relationship &rel = prop.second.get_relationship();
        if (rel.is_path()) {
          skel->animationSource = rel;
          table.insert(kSkelAnimationSource);
        } else {
          PUSH_ERROR_AND_RETURN("`" << kSkelAnimationSource << "` target must be Path.");
        }
      } else {
        PUSH_ERROR_AND_RETURN(
            "`" << kSkelAnimationSource << "` must be a Relationship with Path target.");
      }
    }

    SKELETON_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    PARSE_TIMESAMPLED_ENUM_PROPERTY(table, prop, kVisibility, Visibility, VisibilityEnumHandler, Skeleton,
                   skel->visibility, options.strict_allowedToken_check)
    PARSE_UNIFORM_ENUM_PROPERTY(table, prop, "purpose", Purpose, PurposeEnumHandler, Skeleton,
                       skel->purpose, options.strict_allowedToken_check)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", Skeleton, skel->extent)
    ADD_PROPERTY(table, prop, Skeleton, skel->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

#undef PRIM_CLASS_
#undef PRIM_PTR_


  return true;
}

template <>
bool ReconstructPrim<SkelAnimation>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    SkelAnimation *skelanim,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {

  (void)spec;
  (void)warn;
  (void)references;
  (void)options;

  ParsePropertyTable table;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ SkelAnimation
#define PRIM_PTR_ skelanim
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  for (auto &prop : properties) {
    SKEL_ANIMATION_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, SkelAnimation, skelanim->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

#undef PRIM_CLASS_
#undef PRIM_PTR_

  return true;
}

template <>
bool ReconstructPrim<BlendShape>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    BlendShape *bs,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)spec;
  (void)warn;
  (void)references;
  (void)options;

  DCOUT("Reconstruct BlendShape");

  ParsePropertyTable table;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ BlendShape
#define PRIM_PTR_ bs
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  for (auto &prop : properties) {
    BLEND_SHAPE_TYPED_ATTRS(EXPAND_TYPED_ATTR)
    ADD_PROPERTY(table, prop, BlendShape, bs->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

#undef PRIM_CLASS_
#undef PRIM_PTR_


  return true;
}



}  // namespace prim
}  // namespace lightusd
