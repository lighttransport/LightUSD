// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Reconstruct UsdVol prims (Volume, FieldAsset, OpenVDBAsset, Field3DAsset)
// from PropertyMap or PrimSpec.
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

namespace lightusd {
namespace prim {

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

///
/// Forward decl (defined in prim-reconstruct-shader.cc). Required by the
/// shared reconstruct .inc files included below.
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
bool ReconstructPrim<VolumeFieldBase>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    VolumeFieldBase *field,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  DCOUT("Reconstruct VolumeFieldBase.");
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ VolumeFieldBase
#define PRIM_PTR_ field
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(VolumeFieldBase, field, VOLUME_FIELD_BASE_TYPED_ATTRS, /* no enums */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<FieldBase>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    FieldBase *field,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  DCOUT("Reconstruct FieldBase.");
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ FieldBase
#define PRIM_PTR_ field
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(FieldBase, field, FIELD_BASE_TYPED_ATTRS, /* no enums */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<VolumeFieldAsset>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    VolumeFieldAsset *field,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  DCOUT("Reconstruct VolumeFieldAsset.");
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ VolumeFieldAsset
#define PRIM_PTR_ field
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(VolumeFieldAsset, field, VOLUME_FIELD_ASSET_TYPED_ATTRS, /* no enums */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<FieldAsset>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    FieldAsset *field,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  DCOUT("Reconstruct FieldAsset.");
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ FieldAsset
#define PRIM_PTR_ field
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(FieldAsset, field, FIELD_ASSET_TYPED_ATTRS, /* no enums */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<OpenVDBAsset>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    OpenVDBAsset *asset,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  DCOUT("Reconstruct OpenVDBAsset.");
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ OpenVDBAsset
#define PRIM_PTR_ asset
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(OpenVDBAsset, asset, OPENVDB_ASSET_TYPED_ATTRS, /* no enums */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<Field3DAsset>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    Field3DAsset *asset,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  DCOUT("Reconstruct Field3DAsset.");
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ Field3DAsset
#define PRIM_PTR_ asset
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  RECONSTRUCT_SIMPLE_GEOM_PRIM_BODY(Field3DAsset, asset, FIELD3D_ASSET_TYPED_ATTRS, /* no enums */)
#undef PRIM_CLASS_
#undef PRIM_PTR_
}

template <>
bool ReconstructPrim<Volume>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    Volume *volume,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {
  (void)references;
  DCOUT("Reconstruct Volume.");

  std::set<std::string> table;
  if (!ReconstructGPrimProperties(spec, table, properties, volume, warn, err,
                                  options.strict_allowedToken_check)) {
    return false;
  }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ Volume
#define PRIM_PTR_ volume
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  constexpr auto kFieldPrefix = "field:";

  for (auto &prop : properties) {  // Non-const to allow move from property metadata
    // `field:<name>` relationships map a field name to a field-asset prim path.
    if (startsWith(prop.first, kFieldPrefix) && prop.second.is_relationship()) {
      const std::string field_name = prop.first.substr(std::strlen(kFieldPrefix));
      volume->fieldRelationships[field_name] = prop.second.get_relationship();
      table.insert(prop.first);
      continue;
    }
    ADD_PROPERTY(table, prop, Volume, volume->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

#undef PRIM_CLASS_
#undef PRIM_PTR_

  return true;
}

}  // namespace prim
}  // namespace lightusd
