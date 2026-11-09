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
bool ReconstructPrim<GeomMesh>(
    const Specifier &spec,
    PropertyMap &properties,
    const ReferenceList &references,
    GeomMesh *mesh,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {

  (void)references;

  DCOUT("GeomMesh");

  // Use centralized enum handlers (aliased for macro expansion)
  auto SubdivisionSchemeHandler = enum_handler::SubdivisionScheme;
  auto InterpolateBoundaryHandler = enum_handler::InterpolateBoundary;
  auto FaceVaryingLinearInterpolationHandler = enum_handler::FaceVaryingLinearInterpolation;
  auto TriangleSubdivisionRuleHandler = enum_handler::TriangleSubdivisionRule;
  auto FamilyTypeHandler = enum_handler::FamilyType;

  ParsePropertyTable table;
  if (!ReconstructGPrimProperties(spec, table, properties, mesh, warn, err, options.strict_allowedToken_check)) {
    return false;
  }

  // Define context for property table expansion macros
  // (suppress unused-macros warning since these are used inside X-macro expansion)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define PRIM_CLASS_ GeomMesh
#define PRIM_PTR_ mesh
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  for (auto &prop : properties) {
    DCOUT("GeomMesh prop: " << prop.first);

    // Relations (using property table)
    GEOM_MESH_RELATIONS(EXPAND_SINGLE_REL, EXPAND_MULTI_REL)

    // Typed attributes (using property table)
    GEOM_MESH_TYPED_ATTRS(EXPAND_TYPED_ATTR)

    // Skel-related typed attributes
    GEOM_MESH_SKEL_ATTRS(EXPAND_TYPED_ATTR)

    // Enum properties (using property table)
    GEOM_MESH_UNIFORM_ENUMS(EXPAND_UNIFORM_ENUM)
    GEOM_MESH_TIMESAMPLED_ENUMS(EXPAND_TIMESAMPLED_ENUM)

    // Special handling: subsetFamily for GeomSubset (cannot be table-driven)
    if (startsWith(prop.first, "subsetFamily")) {
      // uniform subsetFamily::<FAMILYNAME>:familyType = ...
      std::vector<std::string> names = split(prop.first, ":");

      if ((names.size() == 3) &&
          (names[0] == "subsetFamily") &&
          (names[2] == "familyType")) {

        if (table.count(prop.first)) {
          // Already processed
        } else if ((prop.second.value_type_name() == value::TypeTraits<value::token>::type_name()) &&
                   prop.second.is_attribute() &&
                   !prop.second.is_empty()) {
          // Parse the token enum value
          const Attribute &attr = prop.second.get_attribute();
          TypedAttributeWithFallback<GeomSubset::FamilyType> familyType{GeomSubset::FamilyType::Unrestricted};
          std::function<nonstd::expected<GeomSubset::FamilyType, std::string>(const std::string &)> fun = FamilyTypeHandler;

          if (!ParseUniformEnumProperty(prop.first, options.strict_allowedToken_check, fun, attr, &familyType, warn, err, options)) {
            return false;
          }

          // NOTE: Ignore metadata of familyType.
          // TODO: Validate familyName
          mesh->subsetFamilyTypeMap[value::token(names[1])] = familyType.get_value();
          table.insert(prop.first);
        }
      }
    }

    // generic property handling
    ADD_PROPERTY(table, prop, GeomMesh, mesh->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

#undef PRIM_CLASS_
#undef PRIM_PTR_

  return true;
}


}  // namespace prim
}  // namespace lightusd
